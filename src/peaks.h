// Waveform peaks for the seek bar: each track is reduced to kPeakBuckets loudness values.
//
// Peaks are computed on one low-priority worker thread and cached in memory (a few recent
// results) and on disk under %LOCALAPPDATA%\cordinal\peaks, keyed on the file's path, size and
// time and on the range, so an edited file gets new peaks. The ReplayGain scanner stores peaks
// while it decodes anyway, so a scan leaves every track's waveform ready.
#pragma once
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// A fixed count per track, so the seek bar's resolution doesn't depend on the track's length.
constexpr int kPeakBuckets = 600;
// Cached peaks and the live waveform's texture use this curve to retain quiet detail. The live
// display's overall height follows its separate note envelope (WaveformMotion in dsp.h).
inline uint8_t CompandPeak(float linear) {
    if (!(linear > 0)) return 0;
    return (uint8_t)std::min(255.0f, std::sqrt(std::min(linear, 1.0f)) * 255.0f + 0.5f);
}
// Per bucket: the largest |sample| over both channels, sqrt-companded to 0..255 (0 = silence).
struct Peaks {
    uint8_t v[kPeakBuckets];
};

class Decoder;
// Quickly samples a seekable track across its full length. The result is an approximation for
// display only; the whole-file decode supplies the exact peaks and is the only result cached.
bool PreviewPeaks(Decoder& decoder, double start, double seconds, Peaks& out, const std::atomic<bool>* cancel);

// Decodes the file, or only the [start, end) seconds of the file when end > 0, and fills `out`.
// A CUE track's range is passed in file seconds; peaks.cpp does not map it itself.
// Returns false when the file can't be decoded or `cancel` is set.
bool ComputePeaks(const std::wstring& path, double start, double end, Peaks& out, const std::atomic<bool>* cancel);

// Starts the background worker. `notify` gets `msg` (wParam = request id) as a queued computation
// progresses and when it finishes; call GetPeaks and GetPeakProgress again then.
// Until this runs, GetPeaks never queues.
void StartPeaks(HWND notify, UINT msg, const std::wstring& cacheDir);
void StopPeaks();
// Abandons the current and queued waveform requests when no track needs them any more.
void CancelPeaks();
// Warms one likely next track without interrupting the playing track's waveform.
void PrefetchPeaks(const std::wstring& path, uint64_t fileSize, uint64_t fileTime, double start, double end);
void CancelPeakPrefetch();

// Returns true and fills `out` when the peaks are cached in memory or on disk. Otherwise queues
// the computation (newest request first; obsolete work is cancelled) and
// returns false with `requestId` set to the request (0 when nothing was queued).
bool GetPeaks(const std::wstring& path, uint64_t fileSize, uint64_t fileTime, double start, double end, Peaks& out,
              uint32_t& requestId);

// Copies the decoded part of an in-flight request, positioned on the whole track's timeline.
// The unprocessed part is zero; these provisional peaks are never written to the cache.
bool GetPeakProgress(uint32_t requestId, Peaks& out);

// Collects peaks while a file is decoded for another purpose (ReplayGain). Memory stays small
// for any length: fine buckets are merged as the file grows.
class PeakAccumulator {
public:
    void Begin(uint64_t totalFrames);  // 0 = unknown
    void Add(const float* stereo, size_t frames);
    bool Preview(Peaks& out) const;  // false until samples and a known length are available
    bool Finish(Peaks& out);  // false when no audio was seen

private:
    static constexpr size_t kMaxBlocks = 2 * kPeakBuckets;
    void Push();
    void Merge();
    std::vector<float> maxima_;  // one linear maximum per block of blockFrames_ frames
    uint64_t blockFrames_ = 512;
    uint64_t totalFrames_ = 0;
    uint64_t inBlock_ = 0;
    float blockMax_ = 0;
};

// Stores peaks computed elsewhere (memory and disk). Ignored until StartPeaks has run.
void StorePeaks(const std::wstring& path, uint64_t fileSize, uint64_t fileTime, double start, double end, const Peaks& p);

// The peaks cache key for a whole file as 16 hex digits: the same path+size+time identity, for
// settings that belong to one file's audio (the section in app_region.cpp) rather than to a range.
std::wstring PeaksKeyName(const std::wstring& path, uint64_t fileSize, uint64_t fileTime);
