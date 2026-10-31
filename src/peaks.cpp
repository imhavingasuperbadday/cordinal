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
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace {

constexpr size_t kFailedCount = 64;
constexpr size_t kMemoryCount = 8;
constexpr uint64_t kCacheLimit = 20ull << 20;  // bytes of .pk files kept on disk
constexpr unsigned kPruneEvery = 64;           // stores between directory scans
constexpr auto kRequestDelay = std::chrono::milliseconds(400);  // let rapid track changes settle
constexpr auto kProgressInterval = std::chrono::milliseconds(100);
constexpr auto kPreviewBudget = std::chrono::milliseconds(35);
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

bool PreviewPeaks(Decoder& dec, double start, double seconds, Peaks& out, const std::atomic<bool>* cancel) {
    if (!dec.CanPreview() || !dec.Rate() || !(seconds > 0) || !std::isfinite(seconds)) return false;
    constexpr int count = 32;
    struct Sample { double at; uint8_t peak; };
    std::array<Sample, count> samples{};
    const auto deadline = std::chrono::steady_clock::now() + kPreviewBudget;
    float buf[4096 * 2];
    int sampled = 0;
    for (int i = 0; i < count; ++i) {
        if ((cancel && cancel->load(std::memory_order_relaxed)) ||
            (i >= 4 && std::chrono::steady_clock::now() >= deadline)) break;
        // Visit the middle, beginning, and then progressively smaller gaps, so an interrupted
        // preview still covers the whole song rather than just its first few seconds.
        unsigned bits = (unsigned)i ^ 1u, index = 0;
        for (int bit = 0; bit < 5; ++bit) { index = (index << 1) | (bits & 1); bits >>= 1; }
        double offset = seconds * (index + 0.5) / count;
        double position = start + offset;
        if (!dec.PreviewSeek(position) || position < start || position >= start + seconds) continue;
        size_t frames = (size_t)std::min<double>({4096.0, (double)std::max(1u, dec.Rate() / 50),
                                                   (start + seconds - position) * dec.Rate()});
        size_t n = dec.Read(buf, frames);
        if (!n) continue;
        float peak = 0;
        for (size_t j = 0; j < n * 2; ++j) peak = std::max(peak, std::fabs(buf[j]));
        samples[sampled++] = {(position - start) / seconds, CompandPeak(peak)};
    }
    if (sampled < 2 || (cancel && cancel->load(std::memory_order_relaxed))) return false;
    std::sort(samples.begin(), samples.begin() + sampled, [](const Sample& a, const Sample& b) { return a.at < b.at; });
    int right = 0;
    for (int i = 0; i < kPeakBuckets; ++i) {
        double at = (i + 0.5) / kPeakBuckets;
        while (right < sampled - 1 && samples[right].at < at) ++right;
        const Sample& a = samples[std::max(0, right - 1)];
        const Sample& b = samples[right];
        double fraction = a.at == b.at ? 0 : std::clamp((at - a.at) / (b.at - a.at), 0.0, 1.0);
        out.v[i] = (uint8_t)std::lround(a.peak + (b.peak - a.peak) * fraction);
    }
    return true;
}

static bool ComputePeaksImpl(const std::wstring& path, double start, double end, Peaks& out,
                             const std::atomic<bool>* cancel, const std::function<void(const Peaks&)>* progress,
                             const std::function<bool()>* waitForSettled) {
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
    uint64_t limit = seconds > 0 ? (uint64_t)(seconds * rate) : UINT64_MAX;
    Peaks preview{};
    bool sampled = progress && PreviewPeaks(*dec, start, seconds, preview, cancel);
    if (cancel && cancel->load(std::memory_order_relaxed)) return false;
    if (sampled) (*progress)(preview);
    if ((start > 0 || (progress && dec->CanPreview())) && !dec->Seek(start)) return false;
    if (waitForSettled && !(*waitForSettled)()) return false;
    PeakAccumulator acc;
    acc.Begin(seconds > 0 ? limit : 0);
    float buf[4096 * 2];
    uint64_t done = 0;
    auto nextProgress = std::chrono::steady_clock::time_point{};
    while (done < limit && !(cancel && cancel->load(std::memory_order_relaxed))) {
        size_t want = (size_t)std::min<uint64_t>(4096, limit - done);
        size_t n = dec->Read(buf, want);
        if (!n) break;
        acc.Add(buf, n);
        done += n;
        if (progress && seconds > 0 && std::chrono::steady_clock::now() >= nextProgress) {
            Peaks partial{};
            if (acc.Preview(partial)) {
                if (sampled) {
                    int covered = std::min(kPeakBuckets, (int)std::ceil((long double)done * kPeakBuckets / limit));
                    std::copy(partial.v, partial.v + covered, preview.v);
                    (*progress)(preview);
                } else (*progress)(partial);
            }
            nextProgress = std::chrono::steady_clock::now() + kProgressInterval;
        }
    }
    if (cancel && cancel->load(std::memory_order_relaxed)) return false;
    return acc.Finish(out);
}

bool ComputePeaks(const std::wstring& path, double start, double end, Peaks& out, const std::atomic<bool>* cancel) {
    return ComputePeaksImpl(path, start, end, out, cancel, nullptr, nullptr);
}

// ---------------------------------------------------------------------------
// PeakAccumulator

void PeakAccumulator::Begin(uint64_t totalFrames) {
    maxima_.clear();
    maxima_.reserve(kMaxBlocks);
    inBlock_ = 0;
    blockMax_ = 0;
    totalFrames_ = totalFrames;
    // Start with about four blocks per bucket when the length is known.
    blockFrames_ = totalFrames ? std::max<uint64_t>(1, totalFrames / (kPeakBuckets * 4)) : 512;
}

bool PeakAccumulator::Preview(Peaks& out) const {
    if (!totalFrames_ || (maxima_.empty() && !inBlock_)) return false;
    std::memset(out.v, 0, sizeof(out.v));
    for (size_t i = 0; i < maxima_.size() + (inBlock_ != 0); ++i) {
        const uint64_t start = (uint64_t)i * blockFrames_;
        if (start >= totalFrames_) break;
        const uint64_t length = i < maxima_.size() ? blockFrames_ : inBlock_;
        const uint64_t end = start + std::min(totalFrames_ - start, length);
        const int first = std::min(kPeakBuckets - 1, (int)((long double)start * kPeakBuckets / totalFrames_));
        const int last = std::min(kPeakBuckets - 1, (int)std::ceil((long double)end * kPeakBuckets / totalFrames_) - 1);
        const uint8_t peak = CompandPeak(i < maxima_.size() ? maxima_[i] : blockMax_);
        for (int bucket = first; bucket <= last; ++bucket) out.v[bucket] = std::max(out.v[bucket], peak);
    }
    return true;
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
            prefetch_.reset();
            failed_.clear();
            dir_.clear();
            cacheCount_ = nextCache_ = 0;
            current_ = 0;
            currentId_ = 0;
            currentPrefetch_ = false;
            progressId_ = 0;
            queuedBefore_ = false;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

    void Cancel() {
        {
            std::lock_guard<std::mutex> lock(mx_);
            jobs_.clear();
            prefetch_.reset();
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
            if (currentId_ && current_ != key && !currentPrefetch_) { cancel_ = true; cv_.notify_one(); }
            if (ReadMemory(key, out)) return true;
            if (std::find(failed_.begin(), failed_.end(), key) != failed_.end()) return false;
            if (currentId_ && current_ == key && !cancel_) {
                currentPrefetch_ = false;
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
                currentPrefetch_ = false;
                requestId = currentId_;
                return false;
            }
            for (const Job& j : jobs_)
                if (j.key == key) {
                    requestId = j.id;
                    return false;
                }
            std::erase_if(jobs_, [&](const Job& j) { return j.key != key; });
            if (currentId_ && current_ != key) { cancel_ = true; cv_.notify_one(); }
            const auto now = std::chrono::steady_clock::now();
            const bool rapid = queuedBefore_ && now - lastQueued_ < kRequestDelay;
            lastQueued_ = now;
            queuedBefore_ = true;
            if (prefetch_ && prefetch_->key == key) {
                requestId = prefetch_->id;
                prefetch_->ready = now + (rapid ? kRequestDelay : std::chrono::milliseconds(0));
                jobs_.push_back(std::move(*prefetch_));
                prefetch_.reset();
                cv_.notify_one();
                return false;
            }
            if (!++nextId_) ++nextId_;  // zero means no queued request
            requestId = nextId_;
            jobs_.push_back({requestId, key, path, start, end, now + (rapid ? kRequestDelay : std::chrono::milliseconds(0))});
        }
        cv_.notify_one();
        return false;
    }

    void Prefetch(const std::wstring& path, uint64_t size, uint64_t time, double start, double end) {
        const uint64_t key = KeyOf(path, size, time, start, end);
        {
            std::lock_guard<std::mutex> lock(mx_);
            if (!running()) return;
            Peaks cached{};
            if (ReadMemory(key, cached) || std::find(failed_.begin(), failed_.end(), key) != failed_.end() ||
                (currentId_ && current_ == key && !cancel_) ||
                std::any_of(jobs_.begin(), jobs_.end(), [&](const Job& j) { return j.key == key; }) ||
                (prefetch_ && prefetch_->key == key)) return;
            if (!++nextId_) ++nextId_;
            prefetch_ = Job{nextId_, key, path, start, end, std::chrono::steady_clock::now() + kRequestDelay};
        }
        cv_.notify_one();
    }

    void CancelPrefetch() {
        {
            std::lock_guard<std::mutex> lock(mx_);
            prefetch_.reset();
            if (currentPrefetch_) cancel_ = true;
        }
        cv_.notify_one();
    }

    bool Progress(uint32_t requestId, Peaks& out) {
        std::lock_guard<std::mutex> lock(mx_);
        if (!requestId || requestId != currentId_ || requestId != progressId_ || cancel_) return false;
        out = progress_;
        return true;
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

    // Playing-track work takes priority over the one likely-next track. Lowest thread priority
    // keeps both decodes out of playback's way.
    void Run() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
        for (;;) {
            Job job;
            Peaks p{};
            bool cached = false;
            std::wstring cacheDir;
            {
                std::unique_lock<std::mutex> lock(mx_);
                cv_.wait(lock, [&] { return stop_ || !jobs_.empty() || prefetch_; });
                if (stop_) break;
                const bool background = jobs_.empty();
                const Job& pending = background ? *prefetch_ : jobs_.back();
                const uint32_t latest = pending.id;
                const auto ready = pending.ready;
                if (background && cv_.wait_until(lock, ready, [&] {
                    return stop_ || !jobs_.empty() || !prefetch_ || prefetch_->id != latest;
                }))
                    continue;
                if (background) { job = std::move(*prefetch_); prefetch_.reset(); }
                else { job = std::move(jobs_.back()); jobs_.pop_back(); }
                current_ = job.key;
                currentId_ = job.id;
                currentPrefetch_ = background;
                progressId_ = 0;
                cancel_ = false;
                cached = ReadMemory(job.key, p);
                cacheDir = dir_;
            }
            const std::function<void(const Peaks&)> report = [&](const Peaks& preview) {
                HWND notify = nullptr;
                UINT msg = 0;
                {
                    std::lock_guard<std::mutex> lock(mx_);
                    if (stop_ || cancel_ || currentId_ != job.id) return;
                    progress_ = preview;
                    progressId_ = job.id;
                    notify = notify_;
                    msg = msg_;
                }
                if (notify) PostMessageW(notify, msg, job.id, 0);
            };
            const std::function<bool()> waitForSettled = [&] {
                std::unique_lock<std::mutex> lock(mx_);
                cv_.wait_until(lock, job.ready, [&] { return stop_ || cancel_; });
                return !stop_ && !cancel_;
            };
            if (!cached) cached = ReadPeakFile(cacheDir + kPathSep + KeyName(job.key), job.key, p);
            bool ok = cached || ComputePeaksImpl(job.path, job.start, job.end, p, &cancel_, &report, &waitForSettled);
            std::wstring dir;
            HWND notify = nullptr;
            UINT msg = 0;
            unsigned writes = 0;
            {
                std::lock_guard<std::mutex> lock(mx_);
                if (stop_) break;
                current_ = 0;
                currentId_ = 0;
                currentPrefetch_ = false;
                progressId_ = 0;
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
            }
            if (notify) PostMessageW(notify, msg, job.id, 0);
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
    std::optional<Job> prefetch_;
    std::vector<uint64_t> failed_;
    std::array<CachedPeaks, kMemoryCount> cache_{};
    Peaks progress_{};
    size_t cacheCount_ = 0, nextCache_ = 0;
    uint64_t current_ = 0;
    bool currentPrefetch_ = false;
    uint32_t currentId_ = 0, nextId_ = 0, progressId_ = 0;
    std::chrono::steady_clock::time_point lastQueued_{};
    bool queuedBefore_ = false;
    unsigned stores_ = 0;
};

PeakService g_peaks;

}  // namespace

void StartPeaks(HWND notify, UINT msg, const std::wstring& cacheDir) { g_peaks.Start(notify, msg, cacheDir); }

void StopPeaks() { g_peaks.Stop(); }

void CancelPeaks() { g_peaks.Cancel(); }

void PrefetchPeaks(const std::wstring& path, uint64_t fileSize, uint64_t fileTime, double start, double end) {
    g_peaks.Prefetch(path, fileSize, fileTime, start, end);
}

void CancelPeakPrefetch() { g_peaks.CancelPrefetch(); }

bool GetPeaks(const std::wstring& path, uint64_t fileSize, uint64_t fileTime, double start, double end, Peaks& out,
              uint32_t& requestId) {
    return g_peaks.Get(path, fileSize, fileTime, start, end, out, requestId);
}

bool GetPeakProgress(uint32_t requestId, Peaks& out) { return g_peaks.Progress(requestId, out); }

void StorePeaks(const std::wstring& path, uint64_t fileSize, uint64_t fileTime, double start, double end, const Peaks& p) {
    g_peaks.Store(path, fileSize, fileTime, start, end, p);
}

std::wstring PeaksKeyName(const std::wstring& path, uint64_t fileSize, uint64_t fileTime) {
    wchar_t name[24] = {};
    swprintf_s(name, L"%016llx", (unsigned long long)KeyOf(path, fileSize, fileTime, 0, 0));
    return name;
}
