// Waveform peaks and the background service that computes and caches them (see peaks.h). A track
// reduces to kPeakBuckets loudness values and a moodbar; the cache key hashes path+size+time+range
// and the .pk stores a copy of that key, so a collision or a stale file is recomputed, not misread.
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
constexpr size_t kListCount = 256;  // Mood column results kept in memory
constexpr size_t kListQueue = 64;   // Mood column requests waiting; the oldest scrolled away first
constexpr uint64_t kCacheLimit = 20ull << 20;  // bytes of .pk files kept on disk
constexpr unsigned kPruneEvery = 64;           // stores between directory scans
constexpr auto kRequestDelay = std::chrono::milliseconds(400);  // let rapid track changes settle
constexpr auto kProgressInterval = std::chrono::milliseconds(100);
constexpr auto kPreviewBudget = std::chrono::milliseconds(35);
// Version 2 added the RMS levels and version 3 the moodbar; an older file is recomputed rather than read.
constexpr char kMagic[4] = {'C', 'P', 'K', '3'};
constexpr DWORD kFileSize = 4 + 8 + (2 + kMoodBands) * kPeakBuckets;

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

// Stored as: magic, key (little endian), kPeakBuckets peak bytes, kPeakBuckets RMS bytes, then the
// moodbar's bass, mid and treble bytes. The key is checked on load so a hash collision or a damaged
// file is simply recomputed.
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
    std::memcpy(out.rms, buf + 12 + kPeakBuckets, kPeakBuckets);
    std::memcpy(out.mood, buf + 12 + 2 * kPeakBuckets, sizeof(out.mood));
    return true;
}

bool WritePeakFile(const std::wstring& file, uint64_t key, const Peaks& p) {
    unsigned char buf[kFileSize];
    std::memcpy(buf, kMagic, 4);
    for (int i = 0; i < 8; ++i) buf[4 + i] = (unsigned char)(key >> (8 * i));
    std::memcpy(buf + 12, p.v, kPeakBuckets);
    std::memcpy(buf + 12 + kPeakBuckets, p.rms, kPeakBuckets);
    std::memcpy(buf + 12 + 2 * kPeakBuckets, p.mood, sizeof(p.mood));
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
    struct Sample { double at; uint8_t peak, rms; double bands[kMoodBands]; };
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
        double squares = 0;
        for (size_t j = 0; j < n * 2; ++j) peak = std::max(peak, std::fabs(buf[j])), squares += (double)buf[j] * buf[j];
        Sample& sample = samples[sampled++];
        sample = {(position - start) / seconds, CompandPeak(peak), CompandPeak((float)std::sqrt(squares / (n * 2)))};
        MoodSplit split;  // a fresh split per read: the reads are far apart
        split.Begin(dec.Rate());
        split.Add(buf, n, sample.bands);
        for (double& band : sample.bands) band /= n;
    }
    if (sampled < 2 || (cancel && cancel->load(std::memory_order_relaxed))) return false;
    std::sort(samples.begin(), samples.begin() + sampled, [](const Sample& a, const Sample& b) { return a.at < b.at; });
    int right = 0;
    std::vector<double> energy(kPeakBuckets * kMoodBands);
    for (int i = 0; i < kPeakBuckets; ++i) {
        double at = (i + 0.5) / kPeakBuckets;
        while (right < sampled - 1 && samples[right].at < at) ++right;
        const Sample& a = samples[std::max(0, right - 1)];
        const Sample& b = samples[right];
        double fraction = a.at == b.at ? 0 : std::clamp((at - a.at) / (b.at - a.at), 0.0, 1.0);
        out.v[i] = (uint8_t)std::lround(a.peak + (b.peak - a.peak) * fraction);
        out.rms[i] = (uint8_t)std::lround(a.rms + (b.rms - a.rms) * fraction);
        for (int band = 0; band < kMoodBands; ++band)
            energy[i * kMoodBands + band] = a.bands[band] + (b.bands[band] - a.bands[band]) * fraction;
    }
    FillMood(energy.data(), out);
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
    acc.Begin(seconds > 0 ? limit : 0, rate);
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
                    std::copy(partial.rms, partial.rms + covered, preview.rms);
                    for (int band = 0; band < kMoodBands; ++band)
                        std::copy(partial.mood[band], partial.mood[band] + covered, preview.mood[band]);
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

void PeakAccumulator::Begin(uint64_t totalFrames, unsigned rate) {
    maxima_.clear();
    maxima_.reserve(kMaxBlocks);
    squares_.clear();
    squares_.reserve(kMaxBlocks);
    bands_.clear();
    bands_.reserve(kMaxBlocks * kMoodBands);
    split_.Begin(rate);
    inBlock_ = 0;
    blockMax_ = 0;
    blockSquares_ = 0;
    std::fill_n(blockBands_, kMoodBands, 0.0);
    totalFrames_ = totalFrames;
    // Start with about four blocks per bucket when the length is known.
    blockFrames_ = totalFrames ? std::max<uint64_t>(1, totalFrames / (kPeakBuckets * 4)) : 512;
}

bool PeakAccumulator::Preview(Peaks& out) const {
    if (!totalFrames_ || (maxima_.empty() && !inBlock_)) return false;
    std::memset(out.v, 0, sizeof(out.v));
    std::memset(out.rms, 0, sizeof(out.rms));
    // Each bucket's mean band energy over the blocks that reach it; buckets not decoded stay unknown.
    std::vector<double> energy(kPeakBuckets * kMoodBands, -1.0);
    std::vector<int> blocks(kPeakBuckets);
    for (size_t i = 0; i < maxima_.size() + (inBlock_ != 0); ++i) {
        const uint64_t start = (uint64_t)i * blockFrames_;
        if (start >= totalFrames_) break;
        const uint64_t length = i < maxima_.size() ? blockFrames_ : inBlock_;
        const uint64_t end = start + std::min(totalFrames_ - start, length);
        const int first = std::min(kPeakBuckets - 1, (int)((long double)start * kPeakBuckets / totalFrames_));
        const int last = std::min(kPeakBuckets - 1, (int)std::ceil((long double)end * kPeakBuckets / totalFrames_) - 1);
        const uint8_t peak = CompandPeak(i < maxima_.size() ? maxima_[i] : blockMax_);
        const uint8_t rms = CompandPeak((float)std::sqrt(i < squares_.size() ? squares_[i] : blockSquares_ / inBlock_));
        for (int bucket = first; bucket <= last; ++bucket) {
            out.v[bucket] = std::max(out.v[bucket], peak);
            out.rms[bucket] = std::max(out.rms[bucket], rms);
            double* e = &energy[bucket * kMoodBands];
            if (!blocks[bucket]++) std::fill_n(e, kMoodBands, 0.0);
            for (int band = 0; band < kMoodBands; ++band)
                e[band] += i < maxima_.size() ? bands_[i * kMoodBands + band] : blockBands_[band] / inBlock_;
        }
    }
    for (int bucket = 0; bucket < kPeakBuckets; ++bucket)
        if (blocks[bucket])
            for (int band = 0; band < kMoodBands; ++band) energy[bucket * kMoodBands + band] /= blocks[bucket];
    FillMood(energy.data(), out);
    return true;
}

void PeakAccumulator::Add(const float* stereo, size_t frames) {
    while (frames) {
        size_t count = (size_t)std::min<uint64_t>(frames, blockFrames_ - inBlock_);
        const float* end = stereo + count * 2;
        float maximum = blockMax_;
        double squares = 0;
        for (; stereo != end; stereo += 2) {
            maximum = std::max(maximum, std::max(std::fabs(stereo[0]), std::fabs(stereo[1])));
            squares += stereo[0] * stereo[0] + stereo[1] * stereo[1];
        }
        blockMax_ = maximum;
        blockSquares_ += squares * 0.5;
        split_.Add(end - count * 2, count, blockBands_);
        inBlock_ += count;
        frames -= count;
        if (inBlock_ == blockFrames_) Push();
    }
}

void PeakAccumulator::Push() {
    maxima_.push_back(blockMax_);
    squares_.push_back(blockSquares_ / inBlock_);
    for (double& band : blockBands_) bands_.push_back(band / inBlock_), band = 0;
    blockMax_ = 0;
    blockSquares_ = 0;
    inBlock_ = 0;
    if (maxima_.size() == kMaxBlocks) Merge();
}

// Halves the number of blocks by keeping the larger peak and the mean power of each pair, so
// memory stays bounded.
void PeakAccumulator::Merge() {
    // Push only calls this with the even kMaxBlocks count.
    for (size_t i = 0; i < kMaxBlocks / 2; ++i) {
        maxima_[i] = std::max(maxima_[2 * i], maxima_[2 * i + 1]);
        squares_[i] = (squares_[2 * i] + squares_[2 * i + 1]) / 2;
        for (int band = 0; band < kMoodBands; ++band)
            bands_[i * kMoodBands + band] = (bands_[2 * i * kMoodBands + band] + bands_[(2 * i + 1) * kMoodBands + band]) / 2;
    }
    maxima_.resize(kMaxBlocks / 2);
    squares_.resize(kMaxBlocks / 2);
    bands_.resize(kMaxBlocks / 2 * kMoodBands);
    blockFrames_ *= 2;
}

bool PeakAccumulator::Finish(Peaks& out) {
    if (inBlock_) Push();
    if (maxima_.empty()) return false;
    size_t n = maxima_.size();
    std::vector<double> energy(kPeakBuckets * kMoodBands);
    for (size_t i = 0; i < kPeakBuckets; ++i) {
        size_t lo = i * n / kPeakBuckets, hi = std::max(lo + 1, (i + 1) * n / kPeakBuckets);
        float m = 0;
        double power = 0;
        for (size_t k = lo; k < hi; ++k) {
            m = std::max(m, maxima_[k]), power += squares_[k];
            for (int band = 0; band < kMoodBands; ++band) energy[i * kMoodBands + band] += bands_[k * kMoodBands + band];
        }
        out.v[i] = CompandPeak(m);
        out.rms[i] = CompandPeak((float)std::sqrt(power / (hi - lo)));
        for (int band = 0; band < kMoodBands; ++band) energy[i * kMoodBands + band] /= (double)(hi - lo);
    }
    FillMood(energy.data(), out);
    maxima_.clear();
    squares_.clear();
    bands_.clear();
    return true;
}

// ---------------------------------------------------------------------------
// Moodbar

void MoodSplit::Begin(unsigned rate) {
    auto coefficient = [rate](double hz) { return (float)(1 - std::exp(-2 * 3.14159265358979 * hz / std::max(1u, rate))); };
    a = coefficient(250);
    b = coefficient(4000);
    primed = false;
}

void MoodSplit::Add(const float* stereo, size_t frames, double sums[kMoodBands]) {
    if (!frames) return;
    if (!primed) {
        // As if the first sample had always been there: the low-passes hold it, the high-passes 0.
        const float x = (stereo[0] + stereo[1]) * 0.5f;
        bass[0] = bass[1] = midHigh[0] = treble[0] = x;
        midHigh[1] = midLow[0] = midLow[1] = treble[1] = 0;
        primed = true;
    }
    double low = 0, mid = 0, high = 0;
    for (const float* end = stereo + frames * 2; stereo != end; stereo += 2) {
        const float x = (stereo[0] + stereo[1]) * 0.5f;
        const float l1 = bass[0] += a * (x - bass[0]);
        const float l2 = bass[1] += a * (l1 - bass[1]);
        const float m1 = x - (midHigh[0] += a * (x - midHigh[0]));
        const float m2 = m1 - (midHigh[1] += a * (m1 - midHigh[1]));
        const float m3 = midLow[0] += b * (m2 - midLow[0]);
        const float m4 = midLow[1] += b * (m3 - midLow[1]);
        const float t1 = x - (treble[0] += b * (x - treble[0]));
        const float t2 = t1 - (treble[1] += b * (t1 - treble[1]));
        low += (double)l2 * l2;
        mid += (double)m4 * m4;
        high += (double)t2 * t2;
    }
    sums[kMoodBass] += low;
    sums[kMoodMid] += mid;
    sums[kMoodTreble] += high;
    // A long silence would otherwise decay the states into slow denormals.
    for (float* state : {bass, midHigh, midLow, treble})
        for (int i = 0; i < 2; ++i)
            if (std::fabs(state[i]) < 1e-20f) state[i] = 0;
}

void FillMood(const double* energy, Peaks& out) {
    constexpr double kRange = 45;     // dB of detail under the track's ceiling
    constexpr double kSilent = -100;  // dB: quieter than this is silence
    std::memset(out.mood, 0, sizeof(out.mood));
    auto decibels = [](double e) { return e > 0 ? 10 * std::log10(e) : -HUGE_VAL; };
    std::vector<double> levels;
    levels.reserve(kPeakBuckets * kMoodBands);
    for (int i = 0; i < kPeakBuckets * kMoodBands; ++i)
        if (double d = decibels(energy[i]); d > kSilent) levels.push_back(d);
    if (levels.empty()) return;
    // One ceiling for all three bands keeps their balance; at the 97th percentile, the loudest few
    // percent clip rather than set the scale.
    const size_t at = (levels.size() - 1) * 97 / 100;
    std::nth_element(levels.begin(), levels.begin() + at, levels.end());
    const double floor = levels[at] - kRange;
    for (int bucket = 0; bucket < kPeakBuckets; ++bucket)
        for (int band = 0; band < kMoodBands; ++band)
            if (double d = decibels(energy[bucket * kMoodBands + band]); d > kSilent)
                out.mood[band][bucket] = (uint8_t)std::lround(std::clamp((d - floor) / kRange, 0.0, 1.0) * 255);
}

float MoodCenter(const Peaks& p, int buckets) {
    double sum = 0;
    int count = 0;
    for (int i = 0; i < std::min(buckets, kPeakBuckets); ++i)
        if (p.mood[kMoodBass][i] || p.mood[kMoodMid][i] || p.mood[kMoodTreble][i]) {
            sum += (p.mood[kMoodTreble][i] - p.mood[kMoodBass][i]) / 255.0;
            ++count;
        }
    return count ? (float)(sum / count) : 0;
}

MoodTone MoodOf(const Peaks& p, int first, int last, float center, int buckets) {
    buckets = std::clamp(buckets, 1, kPeakBuckets);
    first = std::clamp(first, 0, buckets - 1);
    last = std::clamp(last, first + 1, buckets);
    int sums[kMoodBands] = {}, level = 0;
    for (int i = first; i < last; ++i)
        for (int band = 0; band < kMoodBands; ++band) {
            sums[band] += p.mood[band][i];
            level = std::max(level, (int)p.mood[band][i]);
        }
    MoodTone tone;
    tone.level = level / 255.0f;
    for (int band = 0; band < kMoodBands; ++band) tone.bands[band] = sums[band] / 255.0f / (last - first);
    // A track's balance swings by a few tenths of the scale at most; 2.5 spreads that over the hues.
    if (level) tone.bright = std::clamp((tone.bands[kMoodTreble] - tone.bands[kMoodBass] - center) * 2.5f, -1.0f, 1.0f);
    return tone;
}

float LiveMood::Scale(const float* energy, int buckets, Peaks& out) {
    constexpr double kRange = 45, kSilent = -100;
    constexpr double kFall = 0.15;    // dB a frame the ceiling sinks after a loud moment: 4.5 dB/s at 30 fps
    constexpr double kFollow = 0.03;  // how far a frame moves the balance: about a second to settle
    buckets = std::clamp(buckets, 0, kPeakBuckets);
    auto decibels = [](double e) { return e > 0 ? 10 * std::log10(e) : -HUGE_VAL; };
    double loudest = -HUGE_VAL;
    for (int i = 0; i < buckets * kMoodBands; ++i) loudest = std::max(loudest, decibels(energy[i]));
    if (loudest > kSilent) {
        ceiling_ = started_ ? std::max(loudest, ceiling_ - kFall) : loudest;
        started_ = true;
    }
    const double floor = ceiling_ - kRange;
    double balance = 0;
    int lit = 0;
    for (int i = 0; i < buckets; ++i) {
        for (int band = 0; band < kMoodBands; ++band) {
            double d = decibels(energy[i * kMoodBands + band]);
            out.mood[band][i] =
                started_ && d > kSilent ? (uint8_t)std::lround(std::clamp((d - floor) / kRange, 0.0, 1.0) * 255) : 0;
        }
        if (out.mood[kMoodBass][i] || out.mood[kMoodMid][i] || out.mood[kMoodTreble][i]) {
            balance += (out.mood[kMoodTreble][i] - out.mood[kMoodBass][i]) / 255.0;
            ++lit;
        }
    }
    if (lit) center_ += (balance / lit - center_) * kFollow;
    return (float)center_;
}

void SmoothPeaks(const Peaks& in, Peaks& out, int radius) {
    if (&in == &out) {
        Peaks copy = in;
        SmoothPeaks(copy, out, radius);
        return;
    }
    // Two box blurs make a soft bell: outlines round off and neighbouring colours blend.
    radius = std::clamp(radius, 0, kPeakBuckets / 4);
    auto blur = [radius](const uint8_t* from, uint8_t* to) {
        uint8_t temp[kPeakBuckets];
        const uint8_t* source = from;
        for (uint8_t* target : {temp, to}) {
            int sum = 0, count = 0;
            for (int i = -radius; i < kPeakBuckets + radius; ++i) {
                if (int add = i + radius; add < kPeakBuckets) sum += source[add], ++count;
                if (int drop = i - radius - 1; drop >= 0) sum -= source[drop], --count;
                if (i >= 0 && i < kPeakBuckets) target[i] = (uint8_t)((sum + count / 2) / std::max(1, count));
            }
            source = temp;
        }
    };
    blur(in.v, out.v);
    blur(in.rms, out.rms);
    for (int band = 0; band < kMoodBands; ++band) blur(in.mood[band], out.mood[band]);
}

namespace {

Rgb MixRgb(Rgb a, Rgb b, int percent) {  // `percent` of the way from a to b
    Rgb out = 0;
    for (int shift : {16, 8, 0}) {
        int x = (a >> shift) & 0xFF, y = (b >> shift) & 0xFF;
        out |= (Rgb)(x + (y - x) * percent / 100) << shift;
    }
    return out;
}

double Luma(Rgb c) { return 0.299 * (c >> 16 & 0xFF) + 0.587 * (c >> 8 & 0xFF) + 0.114 * (c & 0xFF); }

// What has not played yet: the colour mostly grayed, then pulled toward the background.
Rgb Unplayed(Rgb c, Rgb background) {
    return MixRgb(MixRgb(c, (Rgb)std::lround(Luma(c)) * 0x010101, 65), background, 45);
}

struct Hsl {
    double h, s, l;  // h 0..360, s and l 0..1
};

Hsl ToHsl(Rgb c) {
    double r = (c >> 16 & 0xFF) / 255.0, g = (c >> 8 & 0xFF) / 255.0, b = (c & 0xFF) / 255.0;
    double hi = std::max({r, g, b}), lo = std::min({r, g, b}), l = (hi + lo) / 2, d = hi - lo;
    if (d <= 0) return {0, 0, l};
    double s = d / (1 - std::fabs(2 * l - 1));
    double h = hi == r ? std::fmod((g - b) / d + 6, 6.0) : hi == g ? (b - r) / d + 2 : (r - g) / d + 4;
    return {h * 60, s, l};
}

Rgb FromHsl(Hsl c) {
    double h = std::fmod(std::fmod(c.h, 360.0) + 360, 360.0) / 60, chroma = (1 - std::fabs(2 * c.l - 1)) * c.s;
    double x = chroma * (1 - std::fabs(std::fmod(h, 2.0) - 1)), m = c.l - chroma / 2;
    double rgb[3] = {};
    // Per 60-degree sector: the channel that takes the chroma, then the one that takes x.
    const int order[6][2] = {{0, 1}, {1, 0}, {1, 2}, {2, 1}, {2, 0}, {0, 2}};
    const int sector = std::clamp((int)h, 0, 5);
    rgb[order[sector][0]] = chroma, rgb[order[sector][1]] = x;
    auto byte = [m](double v) { return (Rgb)std::clamp(std::lround((v + m) * 255), 0L, 255L); };
    return byte(rgb[0]) << 16 | byte(rgb[1]) << 8 | byte(rgb[2]);
}

}  // namespace

void MoodPalette::Make(Rgb accentColor, Rgb backgroundColor, int colors) {
    accent = accentColor;
    background = backgroundColor;
    scheme = colors;
    const bool darkBackground = ToHsl(background).l < 0.5;
    Hsl base = ToHsl(accent);
    // Theme keeps the accent's hue but always reads as a colour; Vivid runs most of the wheel.
    const double span = scheme == kMoodVivid ? 120 : 60;  // degrees either side of the accent
    base.s = scheme == kMoodVivid ? 1.0 : std::max(base.s, 0.85);
    if (scheme == kMoodVivid) base.l = darkBackground ? 0.6 : 0.42;
    for (int h = 0; h < kHues; ++h) {
        Hsl tone = base;
        tone.h = base.h + span * (2.0 * h / (kHues - 1) - 1);
        Rgb full = FromHsl(tone);
        // The hue turn can land on a shade the background swallows (yellow on white): move the
        // lightness away from the background until it stands out as much as an accent must.
        for (int step = 0; step < 20 && Contrast(full, background) < 3; ++step) {
            tone.l = std::clamp(tone.l + (darkBackground ? 0.03 : -0.03), 0.0, 1.0);
            full = FromHsl(tone);
        }
        for (int l = 0; l < kLevels; ++l) {
            played[h][l] = MixRgb(background, full, 55 + 45 * l / (kLevels - 1));
            rest[h][l] = Unplayed(played[h][l], background);
        }
    }
}

Rgb MoodPalette::Pick(MoodTone tone, bool isPlayed) const {
    if (!(tone.level > 0)) return background;  // silence
    if (scheme == kMoodSpectrum) {
        // The bands as channels, the strongest at full, quantized to eight steps each so a paint
        // still uses a bounded set of colours; quiet moments darken toward black.
        const float top = std::max({tone.bands[0], tone.bands[1], tone.bands[2], 1e-6f});
        const float strength = 0.35f + 0.65f * std::min(tone.level, 1.0f);
        Rgb c = 0;
        for (int band = 0; band < kMoodBands; ++band) {
            float v = tone.bands[band] / top;
            v *= v;  // squared, so the leading band shows instead of every slice going white
            c = c << 8 | (Rgb)std::lround(std::round(v * strength * 7) / 7 * 255);
        }
        return isPlayed ? c : Unplayed(c, background);
    }
    const int h = std::clamp((int)std::lround((tone.bright + 1) * (kHues - 1) / 2), 0, kHues - 1);
    const int l = std::clamp((int)std::ceil(tone.level * kLevels) - 1, 0, kLevels - 1);
    return isPlayed ? played[h][l] : rest[h][l];
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
            list_.clear();
            listCache_.clear();
            nextList_ = 0;
            failed_.clear();
            dir_.clear();
            cacheCount_ = nextCache_ = 0;
            current_ = 0;
            currentId_ = 0;
            currentPrefetch_ = currentList_ = false;
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
            if (currentId_ && current_ != key && (!currentPrefetch_ || currentList_)) { cancel_ = true; cv_.notify_one(); }
            if (ReadMemory(key, out)) return true;
            if (std::find(failed_.begin(), failed_.end(), key) != failed_.end()) return false;
            if (currentId_ && current_ == key && !cancel_) {
                currentPrefetch_ = currentList_ = false;
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
                currentPrefetch_ = currentList_ = false;
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
            if (currentList_) cancel_ = true;  // the next track matters more than a list cell
        }
        cv_.notify_one();
    }

    bool List(const std::wstring& path, uint64_t size, uint64_t time, double start, double end, Peaks& out) {
        const uint64_t key = KeyOf(path, size, time, start, end);
        {
            std::lock_guard<std::mutex> lock(mx_);
            if (!running()) return false;
            if (ReadMemory(key, out)) return true;
            for (const CachedPeaks& c : listCache_)
                if (c.key == key) {
                    out = c.peaks;
                    return true;
                }
            if (std::find(failed_.begin(), failed_.end(), key) != failed_.end() || (currentId_ && current_ == key)) return false;
            // Newest first: the rows on screen now come before ones scrolled past.
            std::erase_if(list_, [&](const Job& j) { return j.key == key; });
            if (!++nextId_) ++nextId_;
            list_.push_front({nextId_, key, path, start, end, {}});
            if (list_.size() > kListQueue) list_.pop_back();
        }
        cv_.notify_one();
        return false;
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

    void RememberListed(uint64_t key, const Peaks& peaks) {
        for (CachedPeaks& c : listCache_)
            if (c.key == key) {
                c.peaks = peaks;
                return;
            }
        if (listCache_.size() < kListCount) listCache_.push_back({key, peaks});
        else listCache_[nextList_] = {key, peaks}, nextList_ = (nextList_ + 1) % kListCount;
    }

    static std::wstring KeyName(uint64_t key) {
        wchar_t name[32];
        swprintf_s(name, L"%016llx.pk", (unsigned long long)key);
        return name;
    }

    // Playing-track work takes priority over the one likely-next track, and both over the Mood
    // column's tracks, which yield the moment either arrives. Lowest thread priority keeps every
    // decode out of playback's way.
    void Run() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
        for (;;) {
            Job job;
            Peaks p{};
            bool cached = false, listed = false;
            std::wstring cacheDir;
            {
                std::unique_lock<std::mutex> lock(mx_);
                cv_.wait(lock, [&] { return stop_ || !jobs_.empty() || prefetch_ || !list_.empty(); });
                if (stop_) break;
                listed = jobs_.empty() && !prefetch_;
                if (listed) {
                    job = std::move(list_.front());
                    list_.pop_front();
                }
                const bool background = jobs_.empty() && !listed;
                const Job& pending = listed ? job : background ? *prefetch_ : jobs_.back();
                const uint32_t latest = pending.id;
                const auto ready = pending.ready;
                if (background && cv_.wait_until(lock, ready, [&] {
                    return stop_ || !jobs_.empty() || !prefetch_ || prefetch_->id != latest;
                }))
                    continue;
                if (background) { job = std::move(*prefetch_); prefetch_.reset(); }
                else if (!listed) { job = std::move(jobs_.back()); jobs_.pop_back(); }
                current_ = job.key;
                currentId_ = job.id;
                currentPrefetch_ = background || listed;
                currentList_ = listed;
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
            // A list cell has no seek bar to show progress on, and nothing to settle.
            bool ok = cached || ComputePeaksImpl(job.path, job.start, job.end, p, &cancel_, listed ? nullptr : &report,
                                                 listed ? nullptr : &waitForSettled);
            std::wstring dir;
            HWND notify = nullptr;
            UINT msg = 0;
            unsigned writes = 0;
            bool adopted = false;
            {
                std::lock_guard<std::mutex> lock(mx_);
                if (stop_) break;
                // A seek bar request adopted a list job when it asked for the same track.
                adopted = listed && !currentList_;
                current_ = 0;
                currentId_ = 0;
                currentPrefetch_ = currentList_ = false;
                progressId_ = 0;
                if (cancel_) {
                    // A newer track needs the worker; this is not a failed file. A list cell's
                    // track waits for the worker to be free again.
                    if (listed && !adopted) list_.push_front(std::move(job));
                    continue;
                }
                if (!ok) ok = ReadMemory(job.key, p);
                if (ok) {
                    Remember(job.key, p);
                    if (listed) RememberListed(job.key, p);
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
            // A list result goes out as request 0, which no seek bar waits on.
            if (notify && (!listed || adopted)) PostMessageW(notify, msg, job.id, 0);
            if (notify && listed) PostMessageW(notify, msg, 0, 0);
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
    std::deque<Job> list_;                // Mood column requests, newest first
    std::vector<CachedPeaks> listCache_;  // their results, replaced in insertion order
    size_t nextList_ = 0;
    std::vector<uint64_t> failed_;
    std::array<CachedPeaks, kMemoryCount> cache_{};
    Peaks progress_{};
    size_t cacheCount_ = 0, nextCache_ = 0;
    uint64_t current_ = 0;
    bool currentPrefetch_ = false, currentList_ = false;
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

bool ListPeaks(const std::wstring& path, uint64_t fileSize, uint64_t fileTime, double start, double end, Peaks& out) {
    return g_peaks.List(path, fileSize, fileTime, start, end, out);
}

void StorePeaks(const std::wstring& path, uint64_t fileSize, uint64_t fileTime, double start, double end, const Peaks& p) {
    g_peaks.Store(path, fileSize, fileTime, start, end, p);
}

std::wstring PeaksKeyName(const std::wstring& path, uint64_t fileSize, uint64_t fileTime) {
    wchar_t name[24] = {};
    swprintf_s(name, L"%016llx", (unsigned long long)KeyOf(path, fileSize, fileTime, 0, 0));
    return name;
}
