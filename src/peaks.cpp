// Waveform peaks and the background service that computes and caches them (see peaks.h). A track
// reduces to kPeakBuckets loudness values; the cache key hashes path+size+time+range and the .pk
// stores a copy of that key, so a collision or a stale file is recomputed rather than misread.
// One low-priority worker thread, started by StartPeaks and owned by the file-static PeakService.
#include "sort.h"
#include "peaks.h"

#include "cue.h"
#include "decoder.h"
#include "library.h"

#include <shlobj.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <deque>
#include <mutex>
#include <thread>

namespace {

constexpr size_t kFailedCount = 64;
constexpr size_t kMemoryCount = 8;
constexpr uint64_t kCacheLimit = 20ull << 20;  // bytes of .pk files kept on disk
constexpr unsigned kPruneEvery = 64;           // stores between directory scans
constexpr auto kRequestDelay = std::chrono::milliseconds(400);  // let rapid track changes settle
constexpr char kMagic[4] = {'C', 'P', 'K', '1'};
constexpr DWORD kFileSize = 4 + 8 + kPeakBuckets;

// Cache key: FNV-1a over the lowercased path, then the file's identity and the range in ms.
uint64_t KeyOf(const std::wstring& path, uint64_t fileSize, uint64_t fileTime, double start, double end) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    for (wchar_t c : path) mix((uint64_t)towlower(c));
    mix(fileSize);
    mix(fileTime);
    mix((uint64_t)std::llround(std::max(start, 0.0) * 1000));
    mix((uint64_t)std::llround(std::max(end, 0.0) * 1000));
    return h;
}

// Stored as: magic, key (little endian), kPeakBuckets bytes. The key is checked on load so a
// hash collision or a damaged file is simply recomputed.
bool ReadPeakFile(const std::wstring& file, uint64_t key, Peaks& out) {
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    unsigned char buf[kFileSize];
    DWORD got = 0;
    bool ok = ReadFile(h, buf, sizeof(buf), &got, nullptr) && got == kFileSize;
    CloseHandle(h);
    if (!ok || std::memcmp(buf, kMagic, 4) != 0) return false;
    uint64_t stored = 0;
    for (int i = 7; i >= 0; --i) stored = stored << 8 | buf[4 + i];
    if (stored != key) return false;
    std::memcpy(out.v, buf + 12, kPeakBuckets);
    return true;
}

bool WritePeakFile(const std::wstring& file, uint64_t key, const Peaks& p) {
    unsigned char buf[kFileSize];
    std::memcpy(buf, kMagic, 4);
    for (int i = 0; i < 8; ++i) buf[4 + i] = (unsigned char)(key >> (8 * i));
    std::memcpy(buf + 12, p.v, kPeakBuckets);
    // A temp name unique to this thread: two workers may store the same key at once.
    std::wstring tmp = file + L"." + std::to_wstring(GetCurrentThreadId()) + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(h, buf, sizeof(buf), &written, nullptr) && written == sizeof(buf);
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

struct CacheEntry {
    std::wstring file;
    uint64_t size;
    FILETIME written;
};

// Deletes the oldest .pk files until the folder is under kCacheLimit.
void PruneCache(const std::wstring& dir) {
    std::vector<CacheEntry> entries;
    uint64_t total = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + kPathSep + L"*.pk").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        uint64_t size = (uint64_t)fd.nFileSizeHigh << 32 | fd.nFileSizeLow;
        entries.push_back({dir + kPathSep + fd.cFileName, size, fd.ftLastWriteTime});
        total += size;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (total <= kCacheLimit) return;
    SortObjects(entries.begin(), entries.end(), [](const CacheEntry& a, const CacheEntry& b) {
        return CompareFileTime(&a.written, &b.written) < 0;
    });
    for (const auto& e : entries) {
        if (total <= kCacheLimit) break;
        if (DeleteFileW(e.file.c_str())) total -= e.size;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Decoding

bool ComputePeaks(const std::wstring& path, double start, double end, Peaks& out, const std::atomic<bool>* cancel) {
    if (cancel && cancel->load(std::memory_order_relaxed)) return false;
    // [start, end) is in seconds of the file itself. For a CUE track that is the track's range
    // of it, which the caller resolves (app_waveform.cpp Request): mapping the track's offset
    // here as well would decode past the track - or nothing at all.
    std::wstring error;
    auto dec = OpenDecoder(AudioFileOf(path), 48000, error);
    if (!dec || !dec->Rate()) return false;
    const unsigned rate = dec->Rate();
    if (start < 0) start = 0;
    if (end > 0 && end <= start) return false;
    // Seconds to read; zero or negative when the length isn't known.
    double seconds = end > 0 ? end - start : dec->Duration() - start;
    if (start > 0 && !dec->Seek(start)) return false;
    uint64_t limit = seconds > 0 ? (uint64_t)(seconds * rate) : UINT64_MAX;
    PeakAccumulator acc;
    acc.Begin(seconds > 0 ? limit : 0);
    float buf[4096 * 2];
    uint64_t done = 0;
    while (done < limit && !(cancel && cancel->load(std::memory_order_relaxed))) {
        size_t want = (size_t)std::min<uint64_t>(4096, limit - done);
        size_t n = dec->Read(buf, want);
        if (!n) break;
        acc.Add(buf, n);
        done += n;
    }
    if (cancel && cancel->load(std::memory_order_relaxed)) return false;
    return acc.Finish(out);
}

// ---------------------------------------------------------------------------
// PeakAccumulator

void PeakAccumulator::Begin(uint64_t totalFrames) {
    maxima_.clear();
    maxima_.reserve(kMaxBlocks);
    inBlock_ = 0;
    blockMax_ = 0;
    // Start with about four blocks per bucket when the length is known.
    blockFrames_ = totalFrames ? std::max<uint64_t>(1, totalFrames / (kPeakBuckets * 4)) : 512;
}

void PeakAccumulator::Add(const float* stereo, size_t frames) {
    while (frames) {
        size_t count = (size_t)std::min<uint64_t>(frames, blockFrames_ - inBlock_);
        const float* end = stereo + count * 2;
        float maximum = blockMax_;
        for (; stereo != end; stereo += 2)
            maximum = std::max(maximum, std::max(std::fabs(stereo[0]), std::fabs(stereo[1])));
        blockMax_ = maximum;
        inBlock_ += count;
        frames -= count;
        if (inBlock_ == blockFrames_) Push();
    }
}

void PeakAccumulator::Push() {
    maxima_.push_back(blockMax_);
    blockMax_ = 0;
    inBlock_ = 0;
    if (maxima_.size() == kMaxBlocks) Merge();
}

// Halves the number of blocks by keeping the larger of each pair, so memory stays bounded.
void PeakAccumulator::Merge() {
    // Push only calls this with the even kMaxBlocks count.
    for (size_t i = 0; i < kMaxBlocks / 2; ++i)
        maxima_[i] = std::max(maxima_[2 * i], maxima_[2 * i + 1]);
    maxima_.resize(kMaxBlocks / 2);
    blockFrames_ *= 2;
}

bool PeakAccumulator::Finish(Peaks& out) {
    if (inBlock_) Push();
    if (maxima_.empty()) return false;
    size_t n = maxima_.size();
    for (size_t i = 0; i < kPeakBuckets; ++i) {
        size_t lo = i * n / kPeakBuckets, hi = std::max(lo + 1, (i + 1) * n / kPeakBuckets);
        float m = 0;
        for (size_t k = lo; k < hi; ++k) m = std::max(m, maxima_[k]);
        out.v[i] = CompandPeak(m);
    }
    maxima_.clear();
    return true;
}

// ---------------------------------------------------------------------------
// Background service

namespace {

struct Job {
    uint32_t id;
    uint64_t key;
    std::wstring path;
    double start, end;
    std::chrono::steady_clock::time_point ready;
};

struct CachedPeaks {
    uint64_t key;
    Peaks peaks;
};

// Owns the one worker thread and the memory/disk peak cache. mx_ guards every member; disk I/O
// happens outside the lock, so the cache is re-checked after it. Stop() joins the thread.
class PeakService {
public:
    ~PeakService() { Stop(); }

    void Start(HWND notify, UINT msg, const std::wstring& dir) {
        Stop();
        {
            std::lock_guard<std::mutex> lock(mx_);
            notify_ = notify;
            msg_ = msg;
            dir_ = dir;
        }
        SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
        PruneCache(dir);
        stop_ = false;
        thread_ = std::thread(&PeakService::Run, this);
    }

    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mx_);
            stop_ = true;
            cancel_ = true;
            jobs_.clear();
            failed_.clear();
            dir_.clear();
            cacheCount_ = nextCache_ = 0;
            current_ = 0;
            currentId_ = 0;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

    void Cancel() {
        {
            std::lock_guard<std::mutex> lock(mx_);
            jobs_.clear();
            cancel_ = true;
        }
        cv_.notify_one();
    }

    bool Get(const std::wstring& path, uint64_t size, uint64_t time, double start, double end, Peaks& out,
             uint32_t& requestId) {
        requestId = 0;
        uint64_t key = KeyOf(path, size, time, start, end);
        std::wstring dir;
        {
            std::lock_guard<std::mutex> lock(mx_);
            if (!running()) return false;
            if (std::erase_if(jobs_, [&](const Job& j) { return j.key != key; })) cv_.notify_one();
            if (currentId_ && current_ != key) cancel_ = true;
            if (ReadMemory(key, out)) return true;
            if (std::find(failed_.begin(), failed_.end(), key) != failed_.end()) return false;
            if (currentId_ && current_ == key && !cancel_) {
                requestId = currentId_;
                return false;
            }
            for (const Job& j : jobs_)
                if (j.key == key) {
                    requestId = j.id;
                    return false;
                }
            dir = dir_;
        }
        bool hit = ReadPeakFile(dir + kPathSep + KeyName(key), key, out);
        {
            std::lock_guard<std::mutex> lock(mx_);
            if (!running()) return hit;
            if (hit) {
                Remember(key, out);
                return true;
            }
            // The worker or a scanner may have finished while disk I/O was in progress.
            if (ReadMemory(key, out)) return true;
            if (std::find(failed_.begin(), failed_.end(), key) != failed_.end()) return false;
            if (currentId_ && current_ == key && !cancel_) {
                requestId = currentId_;
                return false;
            }
            for (const Job& j : jobs_)
                if (j.key == key) {
                    requestId = j.id;
                    return false;
                }
            std::erase_if(jobs_, [&](const Job& j) { return j.key != key; });
            if (currentId_ && current_ != key) cancel_ = true;
            if (!++nextId_) ++nextId_;  // zero means no queued request
            requestId = nextId_;
            jobs_.push_back({requestId, key, path, start, end, std::chrono::steady_clock::now() + kRequestDelay});
        }
        cv_.notify_one();
        return false;
    }

    void Store(const std::wstring& path, uint64_t size, uint64_t time, double start, double end, const Peaks& p) {
        uint64_t key = KeyOf(path, size, time, start, end);
        std::wstring dir;
        unsigned writes = 0;
        {
            std::lock_guard<std::mutex> lock(mx_);
            if (!running()) return;
            Remember(key, p);
            dir = dir_;
            writes = ++stores_;
        }
        WritePeakFile(dir + kPathSep + KeyName(key), key, p);
        if (writes % kPruneEvery == 0) PruneCache(dir);
    }

private:
    bool running() const { return !dir_.empty(); }

    // Caller holds mx_. Entries are replaced in insertion order.
    bool ReadMemory(uint64_t key, Peaks& out) const {
        for (size_t i = 0; i < cacheCount_; ++i)
            if (cache_[i].key == key) {
                out = cache_[i].peaks;
                return true;
            }
        return false;
    }

    void Remember(uint64_t key, const Peaks& peaks) {
        std::erase(failed_, key);
        for (size_t i = 0; i < cacheCount_; ++i)
            if (cache_[i].key == key) {
                cache_[i].peaks = peaks;
                return;
            }
        cache_[nextCache_] = {key, peaks};
        nextCache_ = (nextCache_ + 1) % kMemoryCount;
        if (cacheCount_ < kMemoryCount) ++cacheCount_;
    }

    static std::wstring KeyName(uint64_t key) {
        wchar_t name[32];
        swprintf_s(name, L"%016llx.pk", (unsigned long long)key);
        return name;
    }

    // Worker loop. Newest request first: a track that just started playing matters more than a
    // stale queue entry. Lowest thread priority, so decoding here never competes with playback.
    void Run() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
        for (;;) {
            Job job;
            Peaks p{};
            bool cached = false;
            {
                std::unique_lock<std::mutex> lock(mx_);
                cv_.wait(lock, [&] { return stop_ || !jobs_.empty(); });
                if (stop_) break;
                const uint32_t latest = jobs_.back().id;
                const auto ready = jobs_.back().ready;
                if (cv_.wait_until(lock, ready, [&] { return stop_ || jobs_.empty() || jobs_.back().id != latest; }))
                    continue;
                job = std::move(jobs_.back());  // newest first
                jobs_.pop_back();
                current_ = job.key;
                currentId_ = job.id;
                cancel_ = false;
                cached = ReadMemory(job.key, p);
            }
            bool ok = cached || ComputePeaks(job.path, job.start, job.end, p, &cancel_);
            std::wstring dir;
            HWND notify = nullptr;
            UINT msg = 0;
            unsigned writes = 0;
            {
                std::lock_guard<std::mutex> lock(mx_);
                if (stop_) break;
                current_ = 0;
                currentId_ = 0;
                if (cancel_) continue;  // a newer track needs the worker; this is not a failed file
                if (!ok) ok = ReadMemory(job.key, p);
                if (ok) {
                    Remember(job.key, p);
                    if (!cached) writes = ++stores_;
                } else {
                    // Don't requery a file that can't be decoded: that would loop forever.
                    if (failed_.size() >= kFailedCount) failed_.erase(failed_.begin());
                    failed_.push_back(job.key);
                }
                dir = dir_;
                notify = notify_;
                msg = msg_;
            }
            if (ok) {
                if (!cached) {
                    WritePeakFile(dir + kPathSep + KeyName(job.key), job.key, p);
                    if (writes % kPruneEvery == 0) PruneCache(dir);
                }
                if (notify) PostMessageW(notify, msg, job.id, 0);
            }
        }
        CoUninitialize();
    }

    std::mutex mx_;
    std::condition_variable cv_;
    std::thread thread_;
    std::atomic<bool> stop_{true};
    std::atomic<bool> cancel_{true};
    HWND notify_ = nullptr;
    UINT msg_ = 0;
    std::wstring dir_;
    std::deque<Job> jobs_;
    std::vector<uint64_t> failed_;
    std::array<CachedPeaks, kMemoryCount> cache_{};
    size_t cacheCount_ = 0, nextCache_ = 0;
    uint64_t current_ = 0;
    uint32_t currentId_ = 0, nextId_ = 0;
    unsigned stores_ = 0;
};

PeakService g_peaks;

}  // namespace

void StartPeaks(HWND notify, UINT msg, const std::wstring& cacheDir) { g_peaks.Start(notify, msg, cacheDir); }

void StopPeaks() { g_peaks.Stop(); }

void CancelPeaks() { g_peaks.Cancel(); }

bool GetPeaks(const std::wstring& path, uint64_t fileSize, uint64_t fileTime, double start, double end, Peaks& out,
              uint32_t& requestId) {
    return g_peaks.Get(path, fileSize, fileTime, start, end, out, requestId);
}

void StorePeaks(const std::wstring& path, uint64_t fileSize, uint64_t fileTime, double start, double end, const Peaks& p) {
    g_peaks.Store(path, fileSize, fileTime, start, end, p);
}

std::wstring PeaksKeyName(const std::wstring& path, uint64_t fileSize, uint64_t fileTime) {
    wchar_t name[24] = {};
    swprintf_s(name, L"%016llx", (unsigned long long)KeyOf(path, fileSize, fileTime, 0, 0));
    return name;
}
