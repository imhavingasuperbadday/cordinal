// The playback engine: a decode thread, the output's render thread, and a ring buffer between
// them, driven by commands from the application (engine.h documents the token protocol and the
// events posted back). The decode thread owns the decoder, the tempo stage, the resampler and the
// ring's write end; the render thread owns the ring's read end, the EQ and the effects after it, the
// volume and the fades. Both serialize on ringMx.
#include "engine.h"

#include "audio_output.h"
#include "audio_live.h"
#include "decoder.h"
#include "peaks.h"
#include "tags.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include <cstdio>
#include <emmintrin.h>

namespace {

// Keeps the benchmark's rendered samples observable, so the compiler can't drop the render work.
__declspec(noinline) void ObservePipelinePcm(const float* samples,size_t frames) {
    static volatile float sink;
    if (frames) sink=samples[0]+samples[frames*2-1];
}

// A point in the ring where what plays changes: a track start or a seek pushes one. The render
// thread reads the marker in effect at its read position for the gain and duration there, and
// Housekeeping reports the audible track from the marker the device has reached.
struct Marker {
    unsigned long long pos;  // ring frame where the track (or a seek) starts
    unsigned long long token;
    double offset;           // track time at `pos`
    float gain;
    double duration;
    PlayItem item;
    double stretch = 1;  // track seconds per output second: speed times tempo
    bool glide = false;  // the gain came from a quick estimate: move to it slowly (see Render)
};

struct Command {
    enum Type { Play, SetNext, Stop, Seek, Reopen, Retime } type;
    PlayItem item;
    bool hasItem = false;
    bool paused = false;  // Play: open the track without starting the output
    unsigned long long after = 0;
    double seconds = 0;
};

// What a PlayItem plays: its file, limited to its range or else looped by the file's loop points
// (read here, on the decode thread). Decode thread only.
std::unique_ptr<Decoder> OpenItem(const PlayItem& item, unsigned rate, int loops, double fade, std::wstring& error) {
    if (item.live) {
        auto decoder = OpenLiveAudio(item.live);
        if (!decoder) {
            error = item.live->Problem();
            if (error.empty()) error = L"The radio stream is unavailable or uses an unsupported format";
        }
        return decoder;
    }
    if (item.start < 0) {  // a CUE track whose sheet is gone (see MakeItem)
        error = L"The CUE sheet for this track is gone";
        return nullptr;
    }
    std::unique_ptr<Decoder> d = OpenDecoder(item.path, rate, error);
    if (!d) return d;
    if (item.start > 0 || item.end > 0) {  // a range turns the loop points off
        d = OpenRange(std::move(d), item.start, item.end);
        if (!d) error = L"Couldn't seek to the track's start";
        if (!d) return nullptr;
    }
    if (item.loopEnd > item.loopStart) {
        const unsigned sourceRate = d->Rate();
        return OpenLooped(std::move(d), (int64_t)std::llround(item.loopStart * sourceRate),
                          (int64_t)std::llround(item.loopEnd * sourceRate), 0, 0);
    }
    if (item.start > 0 || item.end > 0) return d;
    // A sub-track ("file|N") is one song of a game-music file, not the file as a whole.
    int64_t loopStart = 0, loopEnd = 0;
    if (loops <= 0 || item.path.find(L'|') != std::wstring::npos || !ReadLoopPoints(item.path, loopStart, loopEnd))
        return d;
    if (d->Duration() > 0 && (double)loopEnd > d->Duration() * d->Rate()) return d;  // points past the end: ignored
    return OpenLooped(std::move(d), loopStart, loopEnd, loops, fade);
}

}  // namespace

void ClipPcm(float* samples,size_t count) {
    size_t i=0;
    for (;i+4<=count;i+=4) {
        // MINPS/MAXPS forward their second operand for NaNs. Keeping the input
        // there preserves its bits without a separate comparison and blend.
        __m128 value=_mm_loadu_ps(samples+i);
        _mm_storeu_ps(samples+i,_mm_min_ps(_mm_set1_ps(1),_mm_max_ps(_mm_set1_ps(-1),value)));
    }
    for (;i<count;++i) samples[i]=std::clamp(samples[i],-1.0f,1.0f);
}

// ---------------------------------------------------------------------------

// Everything the engine owns. Three threads touch it: the UI thread (commands and queries), the
// decode thread and the render thread. Locking is per-subsystem (ringMx for the ring and markers,
// cmdMx for the command queue, eqMx for the EQ), and the small shared flags are atomics.
struct Engine::Impl {
    HWND notify = nullptr;
    AudioOutput out;
    std::mutex outputMx;  // serializes first device creation with UI output requests
    std::atomic<bool> outputStarted{false};
    std::atomic<unsigned> rate{48000};
    std::atomic<unsigned> inputRate{0};  // resampler input rate, for Output()'s bit-perfect query

    // Ring buffer of processed stereo frames, plus track markers. Guarded by ringMx.
    // Invariants: ring holds cap interleaved frames; writePos - readPos is what is buffered;
    // markers are ordered by pos, the front being the one now audible; gains is the per-token
    // ReplayGain applied when a marker is made (and updated in place by SetGain).
    mutable std::mutex ringMx;
    std::vector<float> ring;
    size_t cap = 0;
    unsigned long long writePos = 0, readPos = 0;
    WaveformMotion waveMotion;
    unsigned long long wavePos = 0, waveStart = 0;  // analyzed through here; history starts after a flush
    std::deque<Marker> markers;
    std::unordered_map<unsigned long long, float> gains;
    std::atomic<float> playingGain{1.0f};  // gain of the audible track (for the bit-perfect check)

    // Render thread
    Equalizer eq;
    std::mutex eqMx;
    EqSettings eqPending;
    std::atomic<bool> eqDirty{false};
    std::atomic<bool> eqActive{false};  // mirrors eq.Active(), which the render thread updates
    std::atomic<float> volume{1.0f};
    float smoothGain = 0, glideGain = 1;
    // After the EQ, in this order: night mode, loudness compensation, reverb. The limiter follows.
    NightCompressor night;
    LoudnessContour contour;
    Reverb reverb;
    std::mutex fxMx;  // reverbPending
    ReverbSettings reverbPending;
    unsigned fxRate = 0;
    std::atomic<bool> reverbDirty{false}, reverbClear{false};
    std::atomic<bool> loudnessOn{false}, nightOn{false};
    std::atomic<bool> effectsActive{false};  // any of the three is changing the sound
    std::atomic<bool> reverbRinging{false};  // a tail is still decaying, so the device keeps running
    // EQ boosts and crossfeed need the same peak protection as ReplayGain, even when ReplayGain is
    // off. The plain EQ-off/ReplayGain-off path keeps its exact samples and has no look-ahead delay.
    PeakLimiter limiter;
    float lastOut[2] = {};  // the last frame rendered, to join the limiter in without a gap
    std::atomic<bool> limiterOn{false};
    std::atomic<float> limiterHeadroom{0};
    float limiterAppliedHeadroom = 0;
    bool limiterActive = false;
    unsigned limiterRate = 0;
    // Apollo's short output history. The render thread never waits for the display's lock.
    static constexpr size_t kScope = 2048;
    std::atomic<bool> scopeOn{false};
    std::mutex scopeMx;
    std::array<float, kScope * 2> scope{};
    size_t scopeAt = 0;
    unsigned scopeRate = 0;
    // Fades (see Render). pauseReq is set by the UI; the rest is guarded by ringMx.
    std::atomic<bool> pauseReq{false};
    bool holdReq = false, held = false;  // holdReq: the decode thread wants silence; held: it's silent
    float ramp = 1.0f;
    std::condition_variable rampCv;
    ULONGLONG silentSince = 0;  // decode thread: when a pause had faded out (see Housekeeping)

    // Decode thread
    std::thread thread;
    std::mutex cmdMx;
    std::mutex liveMx;
    std::shared_ptr<LiveAudioSource> requestedLive;
    std::condition_variable cv;
    std::deque<Command> cmds;
    std::atomic<bool> quit{false};
    std::atomic<int> loopCount{2};       // loop points: passes through the loop (0 = off), see SetLoops
    std::atomic<double> loopFade{8.0};   // seconds of fade-out after the last pass
    std::unique_ptr<Decoder> dec;
    std::unique_ptr<Decoder> waiting;  // exclusive output: the next track, held until the device can switch rate
    PlayItem waitingItem;
    PlayItem curItem;
    Resampler rs;
    std::vector<float> readBuf, rsOut, tsOut;
    // Speed is the resampler's ratio (pitch follows); tempo is the stretch before it (pitch kept).
    // Requested by the UI, applied by the decode thread when rates are set up (InitRates).
    std::atomic<double> speedReq{1.0}, tempoReq{1.0};
    double speed = 1.0;
    TimeStretch stretch;
    unsigned stretchRate = 0;
    // The quick loudness estimate of a track without ReplayGain data (see Estimate).
    std::unique_ptr<LoudnessMeter> estimator;
    unsigned long long estimateToken = 0;
    size_t estimateFrames = 0;
    unsigned estimateRate = 0;
    std::unordered_map<unsigned long long, double> estimates;  // LUFS by token, guarded by ringMx
    // What the application answered SetNext with: Unknown until it does, then Known (play
    // nextItem) or None (drain and end). Reset each time a track opens.
    enum class Next { Unknown, Known, None } nextState = Next::Unknown;
    PlayItem nextItem;
    Command pendingNext{Command::SetNext};
    bool havePendingNext = false;
    unsigned long long decodingToken = 0;
    double decodedTo = 0;  // seconds into the decoding track, for saying where a damaged file stopped
    bool idle = true, drained = false, endedPosted = false;
    std::atomic<unsigned long long> audible{0};

    std::mutex errMx;
    // Decode look-ahead can fail on several tracks before the UI handles their events.
    std::deque<std::pair<unsigned long long, std::wstring>> errors;

    // ---- helpers

    void Post(EngineEvent e, unsigned long long token) {
        if (notify) PostMessageW(notify, WM_ENGINE, (WPARAM)e, (LPARAM)token);
    }

    bool StartOutput(std::wstring& error) {
        std::lock_guard<std::mutex> lock(outputMx);
        if (outputStarted) return true;
        if (!out.Start([this](float* samples, size_t n, unsigned long long& pos) { return Render(samples, n, pos); },
                       [this](unsigned newRate) { rate = newRate; Enqueue({Command::Reopen}); }, error)) {
            out.Stop();
            return false;
        }
        rate = out.Rate();
        eqDirty = true;
        outputStarted = true;
        return true;
    }

    void Enqueue(Command c) {
        if (c.type == Command::Play || c.type == Command::Stop) {
            std::lock_guard<std::mutex> lock(liveMx);
            auto next = c.type == Command::Play ? c.item.live : nullptr;
            if (requestedLive && requestedLive != next) requestedLive->Abort();
            requestedLive = std::move(next);
        }
        {
            std::lock_guard<std::mutex> lock(cmdMx);
            if (c.type == Command::Play || c.type == Command::Stop) {
                std::erase_if(cmds, [](const Command& pending) { return pending.type != Command::Reopen; });
            } else if (c.type == Command::Seek) {
                std::erase_if(cmds, [](const Command& pending) { return pending.type == Command::Seek; });
            } else if (c.type == Command::SetNext) {
                std::erase_if(cmds, [&](const Command& pending) { return pending.type == Command::SetNext && pending.after == c.after; });
            } else if (c.type == Command::Reopen || c.type == Command::Retime) {
                std::erase_if(cmds, [&](const Command& pending) { return pending.type == c.type; });
            }
            cmds.push_back(std::move(c));
        }
        cv.notify_one();
    }

    void AllocateRing() {
        std::lock_guard<std::mutex> lock(ringMx);
        // One second of decode look-ahead plus half a second retained behind playback for the
        // clock-centered visualizers. FreeSpace keeps the queued-audio limit at one second.
        cap = (size_t)rate.load() * 3 / 2;
        std::vector<float>(cap * 2).swap(ring);
        readPos = writePos;
        waveMotion = {};
        wavePos = waveStart = writePos;
    }

    const Marker* MarkerAt(unsigned long long pos) const {  // caller holds ringMx
        const Marker* m = nullptr;
        for (const Marker& k : markers) {
            if (k.pos > pos) break;
            m = &k;
        }
        return m;
    }

    // ---- render thread

    // Fades (see Engine::SetPaused) are applied here, to what is rendered. `ramp` falls toward 0
    // while paused or while the decode thread holds the output (BeginHold), per output frame, so
    // it reaches 0 even when the ring is empty. It rises toward 1 per consumed frame, so new audio
    // fades in from its first sample. Outside the fades `ramp` is exactly 1, and the gain multiply
    // leaves the samples untouched. Nothing is consumed while paused at 0.
    static constexpr double kRampSeconds = 0.012;

    size_t Render(float* dst, size_t frames, unsigned long long& sourcePos) {
        if (eqDirty.exchange(false)) {
            std::lock_guard<std::mutex> lock(eqMx);
            eq.Configure(eqPending, rate);
            eqActive = eq.Active();
        }
        const float step = (float)(1.0 / (kRampSeconds * rate.load()));
        const float glide = (float)(0.5 / rate.load());
        const float vol = volume.load();
        const bool paused = pauseReq.load();
        size_t n = 0;
        {
            std::lock_guard<std::mutex> lock(ringMx);
            unsigned long long segEnd = 0;
            const Marker* m = nullptr;
            for (size_t i = 0; i < frames; ++i) {
                const float target = paused || holdReq ? 0.0f : 1.0f;
                const bool consume = readPos < writePos && !(paused && ramp <= 0.0f);
                if (consume || ramp > target) ramp = StepFade(ramp, target, step);
                if (holdReq && !held && ramp <= 0.0f) {
                    held = true;
                    rampCv.notify_all();
                }
                if (!consume) {
                    dst[i * 2] = dst[i * 2 + 1] = 0.0f;
                    continue;
                }
                if (readPos >= segEnd) {  // a track boundary (or the first one): look up its marker
                    m = MarkerAt(readPos);
                    segEnd = ~0ull;
                    for (const Marker& k : markers)
                        if (k.pos > readPos) { segEnd = k.pos; break; }
                }
                const float gain = m ? m->gain : 1.0f;
                playingGain = gain;
                // An estimated gain arrives once, seconds into the track: it moves at 0.5 per second
                // instead of the usual 10 ms, so the change is heard as a gentle level shift.
                glideGain = m && m->glide ? StepFade(glideGain, gain, glide) : gain;
                const float goal = glideGain * vol;
                if (!paused && !holdReq && ramp==1.0f && smoothGain==goal) {
                    // A steady segment can be copied/scaled as one contiguous
                    // span. Fades and marker transitions retain the scalar path.
                    size_t at=(size_t)(readPos%cap);
                    size_t count=(size_t)std::min<unsigned long long>({frames-i,writePos-readPos,segEnd-readPos,cap-at});
                    if (goal==1.0f) memcpy(dst+i*2,ring.data()+at*2,count*2*sizeof(float));
                    else {
                        __m128 scale=_mm_set1_ps(goal);size_t j=0;
                        for (;j+4<=count*2;j+=4) _mm_storeu_ps(dst+i*2+j,_mm_mul_ps(_mm_loadu_ps(ring.data()+at*2+j),scale));
                        for (;j<count*2;++j) dst[i*2+j]=ring[at*2+j]*goal;
                    }
                    readPos+=count;n+=count;i+=count-1;continue;
                }
                smoothGain += (goal - smoothGain) * 0.002f;  // ~10 ms glide, no zipper noise
                if (std::fabs(goal - smoothGain) < 1e-6f) smoothGain = goal;  // exact unity gain stays bit-perfect
                const size_t r = (size_t)(readPos % cap) * 2;
                const float g = smoothGain * ramp;
                dst[i * 2] = ring[r] * g;
                dst[i * 2 + 1] = ring[r + 1] * g;
                ++readPos;
                ++n;
            }
            sourcePos = readPos;
        }
        const bool quiet = ramp <= 0.0f;  // faded out (paused, seeking, idle) before this block
        eq.Process(dst, frames);
        eqActive = eq.Active();
        if (fxRate != rate.load()) {
            fxRate = rate.load();
            night.Configure(fxRate);
            contour.Configure(fxRate);
            reverbDirty = true;
        }
        if (reverbDirty.exchange(false)) {
            std::lock_guard<std::mutex> lock(fxMx);
            reverb.Configure(reverbPending, fxRate);
        }
        if (reverbClear.exchange(false)) reverb.Clear();
        night.Process(dst, frames, vol, nightOn.load());
        contour.Process(dst, frames, vol, loudnessOn.load());
        reverb.Process(dst, frames);
        effectsActive = night.Active() || contour.Active() || reverb.Active();
        reverbRinging = reverb.Active();
        const bool replayGain = limiterOn.load();
        // The contour fades out after a volume increase, so its old boost can briefly exceed
        // the new attenuation even though its steady-state target never does.
        const bool want = replayGain || eq.Active() || night.Active() || contour.Active() || reverb.Active();
        const float headroom = replayGain ? limiterHeadroom.load() : 0;
        // Engaging adds the look-ahead delay and leaving drops it, so mid-song the limiter is held
        // with the last frame out when it comes in (no gap) and only leaves once the output is
        // faded out (no jump). Lingering is harmless: below its ceiling it only delays.
        if (want && (!limiterActive || limiterRate != rate.load())) {
            limiter.Configure(rate.load(), headroom);
            if (!quiet) limiter.Prime(lastOut[0], lastOut[1]);
            limiterActive = true;
            limiterRate = rate.load();
            limiterAppliedHeadroom = headroom;
        } else if (!want && limiterActive && quiet) limiterActive = false;
        if (limiterActive && headroom != limiterAppliedHeadroom) {
            limiter.SetHeadroom(headroom);
            limiterAppliedHeadroom = headroom;
        }
        if (limiterActive) limiter.Process(dst, frames);
        ClipPcm(dst,frames*2);
        if (frames) {
            lastOut[0] = dst[frames * 2 - 2];
            lastOut[1] = dst[frames * 2 - 1];
        }
        if (scopeOn.load(std::memory_order_relaxed) && scopeMx.try_lock()) {
            const size_t capacity = scope.size() / 2;
            if (scopeRate != rate.load()) {
                std::fill(scope.begin(), scope.end(), 0.0f);
                scopeAt = 0;
                scopeRate = rate.load();
            }
            const float unit = vol > 0.01f ? 1.0f / vol : 1.0f;
            for (size_t i = frames > capacity ? frames - capacity : 0; i < frames; ++i) {
                scope[scopeAt * 2] = dst[i * 2] * unit;
                scope[scopeAt * 2 + 1] = dst[i * 2 + 1] * unit;
                if (++scopeAt == capacity) scopeAt = 0;
            }
            scopeMx.unlock();
        }
        return n;
    }

    // ---- decode thread

    void WriteRing(const float* data, size_t frames) {
        if (!frames) return;
        std::lock_guard<std::mutex> lock(ringMx);
        size_t done=0,at=(size_t)(writePos%cap);
        while (done<frames) {
            size_t count=std::min(frames-done,cap-at);
            memcpy(ring.data()+at*2,data+done*2,count*2*sizeof(float));
            done+=count;at=0;
        }
        writePos += frames;
    }

    size_t QueueDecoded(const float* data,size_t frames) {
        if (stretch.Active()) {
            stretch.Process(data,frames,tsOut);
            data=tsOut.data();frames=tsOut.size()/2;
        }
        return QueueResampled(data,frames);
    }
    size_t QueueResampled(const float* data,size_t frames) {
        if (rs.Passthrough()) { WriteRing(data,frames);return frames; }
        rs.Process(data,frames,rsOut);
        size_t produced=rsOut.size()/2;WriteRing(rsOut.data(),produced);return produced;
    }

    // The end of the stream, or of one input rate: what the tempo stage and resampler still hold.
    void WriteTail() {
        if (stretch.Active()) {
            tsOut.clear();
            stretch.Flush(tsOut);
            if (!tsOut.empty()) QueueResampled(tsOut.data(), tsOut.size() / 2);
        }
        rsOut.clear();
        rs.Flush(rsOut);
        WriteRing(rsOut.data(), rsOut.size() / 2);
    }

    unsigned Effective(unsigned inRate) const {
        return std::max(1u, (unsigned)std::lround(inRate * speedReq.load()));
    }

    // Sets up the resampler and tempo stage for a source rate, at the requested speed and tempo.
    void InitRates(unsigned inRate) {
        speed = speedReq.load();
        rs.Init(Effective(inRate), rate);
        inputRate = rs.InRate();
        const double tempo = tempoReq.load();
        if (tempo != stretch.Tempo() || inRate != stretchRate) {
            stretch.Configure(inRate, tempo);
            stretchRate = inRate;
        }
    }

    // Output frames still owed for the input pushed so far: where a track that follows starts.
    double PendingOutput() const {
        double pending = rs.PendingOutput();
        if (stretch.Active() && rs.InRate()) pending += stretch.PendingOutput() * rate / rs.InRate();
        return pending;
    }

    double Stretch() const { return speed * stretch.Tempo(); }

    // The quick loudness estimate for a track without ReplayGain data (PlayItem::estimate): its
    // first seconds as decoded, measured once and reported as EngineEvent::Estimated. A silent start
    // is given up on after 20 s; a track shorter than 6 s reports what it had. Decode thread only.
    void Estimate(const float* pcm, size_t frames, bool ended) {
        if (!estimator || estimateToken != decodingToken) return;
        estimator->Add(pcm, frames);
        estimateFrames += frames;
        const double seconds = (double)estimateFrames / estimateRate, lufs = estimator->Integrated();
        const bool ready = std::isfinite(lufs) && (seconds >= 6 || (ended && seconds >= 1));
        if (!ready && !ended && seconds < 20) return;
        if (ready) {
            {
                std::lock_guard<std::mutex> lock(ringMx);
                if (estimates.size() > 16) estimates.clear();  // never taken: their tokens are gone
                estimates[estimateToken] = lufs;
            }
            Post(EngineEvent::Estimated, estimateToken);
        }
        estimator.reset();
        estimateToken = 0;
    }

    size_t RingSpace() const {
        std::lock_guard<std::mutex> lock(ringMx);
        return cap - cap / 3 - (size_t)(writePos - readPos);
    }

    // Drops the buffered audio. The device isn't flushed: what it has queued was rendered before
    // this, through the fade, so it plays out faded and the new audio follows it in order.
    void FlushAll() {
        {
            std::lock_guard<std::mutex> lock(ringMx);
            readPos = writePos;
            markers.clear();
            waveMotion = {};
            wavePos = waveStart = writePos;
        }
        {
            std::lock_guard<std::mutex> lock(scopeMx);
            std::fill(scope.begin(), scope.end(), 0.0f);
            scopeAt = 0;
        }
        rs.Reset();
        stretch.Reset();
        reverbClear = true;  // a new place in the music: the old tail ducks out
        audible = 0;
        waiting.reset();  // don't keep a held-back next track's file open
    }

    // The decode thread changes what plays (seek, stop, a track started by hand) inside a Hold:
    // the output fades to silence first, so the cut lands in silence. Waiting is bounded in case
    // the device isn't rendering. Decode thread only.
    void BeginHold() {
        std::unique_lock<std::mutex> lock(ringMx);
        holdReq = true;
        held = out.Paused() || ramp <= 0.0f;
        if (held) ramp = 0.0f;  // an idle endpoint is already silent; the next track fades in
        rampCv.wait_for(lock, std::chrono::milliseconds(100), [&] { return held; });
    }
    void EndHold() {
        std::lock_guard<std::mutex> lock(ringMx);
        holdReq = false;
        held = false;
    }
    struct Hold {
        Impl& d;
        explicit Hold(Impl& impl) : d(impl) { d.BeginHold(); }
        ~Hold() { d.EndHold(); }
    };

    void Fail(const PlayItem& item, const std::wstring& message) {
        {
            std::lock_guard<std::mutex> lock(errMx);
            errors.emplace_back(item.token, message);
        }
        dec.reset();
        curItem = PlayItem{};
        Post(EngineEvent::Error, item.token);
    }

    // Good samples already queued still play, then the next track can start. Keep the problem
    // with its token so another failure during look-ahead cannot replace this explanation. `why` is
    // the decoder's own account (Decoder::Problem), when it has one.
    void ReportDamage(const std::wstring& why) {
        const auto at = (uint64_t)decodedTo;
        const std::wstring seconds = std::to_wstring(at % 60);
        {
            std::lock_guard<std::mutex> lock(errMx);
            errors.emplace_back(decodingToken, L"Playback failed at " + std::to_wstring(at / 60) + L":" +
                        (seconds.size() < 2 ? L"0" : L"") + seconds + L". " +
                        (why.empty() ? L"The audio is damaged or unreadable." : why + L"."));
        }
        Post(EngineEvent::Damaged, decodingToken);
    }

    bool Exclusive() const { return out.WantedMode() == OutputMode::Exclusive; }

    // Exclusive output runs the device at the track's rate: switching it means a new ring (and
    // a fresh resampler, which is then a pass-through).
    void SwitchRate(unsigned trackRate) {
        unsigned actual = out.RequestRate(trackRate);
        if (actual != rate) {
            rate = actual;
            AllocateRing();
            rs = Resampler{};
            eqDirty = true;
        }
    }

    void OpenTrack(const PlayItem& item, double start, bool flush, std::unique_ptr<Decoder> opened = nullptr) {
        if (flush) {
            if (!cap) AllocateRing();
            if (readBuf.empty()) readBuf.resize(2048 * 2);
            FlushAll();
            drained = false;
            endedPosted = false;
        }
        decodingToken = item.token;
        nextState = Next::Unknown;
        idle = false;
        std::wstring error;
        dec = opened ? std::move(opened) : OpenItem(item, rate, loopCount, loopFade, error);
        if (!dec) {
            Fail(item, error);
            return;
        }
        if (item.loopEnd > item.loopStart)
            start = std::clamp(start, item.loopStart, std::max(item.loopStart, item.loopEnd - 1.0 / dec->Rate()));
        if (start > 0 && !dec->Seek(start)) {
            Fail(item, L"Couldn't seek in the file. The audio is damaged or unreadable.");
            return;
        }
        decodedTo = std::max(0.0, start);
        unsigned inRate = dec->Rate();
        if (flush && Exclusive() && inRate != rate) SwitchRate(inRate);
        if (flush || Effective(inRate) != rs.InRate() || tempoReq.load() != stretch.Tempo()) {
            if (!flush) WriteTail();  // finish the previous track's tail at its own rate
            InitRates(inRate);
        }
        estimator.reset();
        estimateToken = 0;
        if (item.estimate) {
            estimator = std::make_unique<LoudnessMeter>(inRate);
            estimateToken = item.token;
            estimateFrames = 0;
            estimateRate = inRate;
        }
        double duration = dec->Duration() > 0 ? dec->Duration() : item.duration;
        {
            std::lock_guard<std::mutex> lock(ringMx);
            auto g = gains.find(item.token);
            markers.push_back({writePos + (unsigned long long)std::llround(PendingOutput()), item.token, start,
                               g != gains.end() ? g->second : item.gain, duration, item});
            markers.back().stretch = Stretch();
        }
        curItem = item;
        drained = false;
        endedPosted = false;
        Post(EngineEvent::NeedNext, item.token);
    }

    void DoSeek(double seconds, const PlayItem* updated) {
        Marker m;
        {
            std::lock_guard<std::mutex> lock(ringMx);
            const Marker* a = MarkerAt(out.PlayedPosition());
            if (!a && !markers.empty()) a = &markers.front();
            if (!a) return;
            m = *a;
        }
        if (updated && updated->token != m.token) return;
        bool changed = updated && updated->token == m.token &&
                       (updated->loopStart != m.item.loopStart || updated->loopEnd != m.item.loopEnd);
        if (changed) m.item = *updated;
        if (m.item.loopEnd > m.item.loopStart)
            seconds = std::clamp(seconds, m.item.loopStart,
                                 std::max(m.item.loopStart, m.item.loopEnd - 1.0 / rate.load()));
        else if (m.duration > 0) seconds = std::min(seconds, std::max(0.0, m.duration - 0.05));
        seconds = std::max(0.0, seconds);
        Hold hold(*this);
        if (!dec || m.token != curItem.token || changed) {
            OpenTrack(m.item, seconds, true);
            return;
        }
        FlushAll();
        if (!dec->Seek(seconds)) {
            decodingToken = m.token;
            nextState = Next::Unknown;
            drained = endedPosted = idle = false;
            Fail(m.item, L"Couldn't seek in the file. The audio is damaged or unreadable.");
            return;
        }
        decodedTo = seconds;
        {
            std::lock_guard<std::mutex> lock(ringMx);
            markers.push_back({writePos, m.token, seconds, m.gain, m.duration, m.item});
            markers.back().stretch = Stretch();
            markers.back().glide = m.glide;
        }
        drained = false;
        endedPosted = false;
        idle = false;
    }

    // Replacing the next track must not seek or discard the song being heard. If decoding has
    // already crossed its boundary, retain the current song in the ring and discard only audio
    // belonging to the old next track. Audio already handed to the device cannot be taken back.
    void SetNext(Command& c) {
        if (havePendingNext && pendingNext.after == c.after) havePendingNext = false;
        bool replaced = false;
        if (c.after != decodingToken) {
            {
                std::lock_guard<std::mutex> lock(ringMx);
                auto current = std::find_if(markers.begin(), markers.end(), [&](const Marker& m) { return m.token == c.after; });
                if (current == markers.end() || ++current == markers.end() || current->pos < readPos) return;
                if (current->pos > writePos) {
                    // The resampler or tempo stage still owes samples from the current song.
                    // Retry after decoding has written them instead of cutting them short.
                    pendingNext = c;
                    havePendingNext = true;
                    return;
                }
                writePos = current->pos;
                markers.erase(current, markers.end());
            }
            dec.reset();
            waiting.reset();
            estimator.reset();
            estimateToken = 0;
            rs = Resampler{};
            stretch.Reset();
            inputRate = 0;
            decodingToken = c.after;
            replaced = true;
        }
        nextState = c.hasItem ? Next::Known : Next::None;
        if (c.hasItem) nextItem = c.item;
        else waiting.reset();
        if (replaced || c.hasItem || !drained) {
            drained = false;  // a previously empty next choice may have started draining already
            endedPosted = false;
        }
    }

    void Handle(Command& c) {
        switch (c.type) {
            case Command::Play: {
                havePendingNext = false;
                std::wstring error;
                if (!StartOutput(error)) {
                    decodingToken = c.item.token;
                    idle = false;
                    Fail(c.item, error);
                    break;
                }
                Hold hold(*this);
                OpenTrack(c.item, c.seconds, true);
                // A paused start holds the track at its first sample: nothing is heard until Play.
                if (c.paused && dec) {
                    out.SetPaused(true);
                    pauseReq = true;
                } else {
                    out.SetPaused(false);
                    pauseReq = false;  // resumes with the new track, faded in
                }
                break;
            }
            case Command::SetNext:
                SetNext(c);
                break;
            case Command::Stop: {
                havePendingNext = false;
                Hold hold(*this);
                FlushAll();
                dec.reset();
                curItem = nextItem = waitingItem = PlayItem{};
                {
                    std::lock_guard<std::mutex> lock(ringMx);
                    std::vector<float>().swap(ring);
                    cap = 0;
                    gains.clear();
                    estimates.clear();
                }
                std::vector<float>().swap(readBuf);
                std::vector<float>().swap(rsOut);
                std::vector<float>().swap(tsOut);
                rs = Resampler{};
                stretch = TimeStretch{};
                stretchRate = 0;
                estimator.reset();
                decodingToken = 0;
                idle = true;
                out.SetPaused(false);
                pauseReq = false;
                break;
            }
            case Command::Seek:
                havePendingNext = false;
                DoSeek(c.seconds, c.hasItem ? &c.item : nullptr);
                break;
            case Command::Reopen: {  // the output's rate changed
                havePendingNext = false;
                double pos = Position();
                Marker m;
                bool any = false;
                {
                    std::lock_guard<std::mutex> lock(ringMx);
                    if (const Marker* a = MarkerAt(out.PlayedPosition())) { m = *a; any = true; }
                }
                AllocateRing();
                rs = Resampler{};
                eqDirty = true;
                if (any && !idle) OpenTrack(m.item, m.item.live ? 0 : pos, true);
                break;
            }
            case Command::Retime:  // speed or tempo changed: go on from what is heard, re-timed
                havePendingNext = false;
                if (idle || !cap) break;  // applied when the next track opens
                if (dec) InitRates(dec->Rate());
                if (curItem.live) break;  // live streams cannot seek back to the audible position
                DoSeek(Position(), nullptr);
                break;
        }
    }

    // One decode-thread pass: retire spent markers and post TrackStarted/Ended from the device's
    // played position, and stop the device after a pause/stop so the endpoint can idle.
    void Housekeeping() {
        // A pause that has faded out stops the device once it has played what it held, so the
        // audio endpoint can idle (Bluetooth headphones, laptops). Resuming starts it first.
        bool silent;
        {
            std::lock_guard<std::mutex> lock(ringMx);
            silent = (idle || (pauseReq && ramp <= 0.0f)) && !reverbRinging;
        }
        if (!silent) {
            silentSince = 0;
        } else if (!out.Paused()) {
            ULONGLONG t = GetTickCount64();
            if (!silentSince) silentSince = t;
            else if (t - silentSince >= 250) out.SetPaused(true);
        }
        unsigned long long played = out.PlayedPosition(), write, now = 0;
        {
            std::lock_guard<std::mutex> lock(ringMx);
            while (markers.size() > 1 && markers[1].pos <= played) markers.pop_front();
            // At the marker itself, the device can still hold silence from before this track.
            // Announce it only once at least one of its samples has actually been consumed.
            if (!markers.empty() && markers.front().pos < played) now = markers.front().token;
            write = writePos;
        }
        if (!idle && now && now != audible) {
            audible = now;
            Post(EngineEvent::TrackStarted, now);
        }
        if (drained && !dec && !idle && played >= write && !endedPosted) {
            endedPosted = true;
            idle = true;
            audible = 0;
            Post(EngineEvent::Ended, 0);
        }
    }

    // Decode thread: drain the command queue, decode as much as the ring has room for, then either
    // open the next track (gapless, or after a device rate switch) or wait. `Ended` is posted
    // later, by Housekeeping, once the device has actually consumed the drained track.
    void DecodeLoop() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        while (!quit) {
            bool busy = false;
            std::deque<Command> local;
            {
                std::lock_guard<std::mutex> lock(cmdMx);
                local.swap(cmds);
            }
            for (auto& c : local) Handle(c);
            if (havePendingNext) {
                Command pending = std::move(pendingNext);
                havePendingNext = false;
                SetNext(pending);
            }
            Housekeeping();

            if (dec) {
                const size_t chunk = 2048;
                double ratio = rs.InRate() ? (double)rate / rs.InRate() : 1.0;
                // A slower tempo makes more frames, and one more segment can come out with them.
                if (stretch.Active()) ratio /= stretch.Tempo();
                if (RingSpace() >= (size_t)(chunk * ratio) + (stretch.Active() ? rate / 20 : 0) + 64) {
                    size_t n = dec->Read(readBuf.data(), chunk);
                    if (n) {
                        Estimate(readBuf.data(), n, false);
                        QueueDecoded(readBuf.data(),n);
                        decodedTo += (double)n / dec->Rate();
                    } else {
                        Estimate(nullptr, 0, true);
                        if (dec->Failed()) ReportDamage(dec->Problem());
                        dec.reset();  // end of this track
                    }
                    busy = true;
                }
            }
            if (!dec && !idle && !drained) {
                if (nextState == Next::Known && Exclusive()) {
                    // A gapless join needs one rate. When the next track's differs, let this one
                    // play out, then switch the device and start the next track fresh.
                    if (!waiting || waitingItem.token != nextItem.token) {
                        std::wstring error;
                        waitingItem = nextItem;
                        waiting = OpenItem(waitingItem, rate, loopCount, loopFade, error);
                        if (!waiting) {
                            PlayItem failed = waitingItem;
                            nextState = Next::Unknown;
                            decodingToken = failed.token;
                            Fail(failed, error);
                            continue;
                        }
                    }
                    if (waiting->Rate() == rate || rs.InRate() == Effective(waiting->Rate())) {
                        PlayItem next = waitingItem;
                        OpenTrack(next, 0, false, std::move(waiting));
                        busy = true;
                    } else {
                        bool tailWritten = rs.InRate() == 0;
                        if (!tailWritten) {
                            WriteTail();
                            rs = Resampler{};
                        }
                        unsigned long long write;
                        {
                            std::lock_guard<std::mutex> lock(ringMx);
                            write = writePos;
                        }
                        if (out.PlayedPosition() >= write) {  // everything before has been heard
                            PlayItem next = waitingItem;
                            OpenTrack(next, 0, true, std::move(waiting));
                            busy = true;
                        }
                    }
                } else if (nextState == Next::Known) {
                    PlayItem next = nextItem;
                    OpenTrack(next, 0, false);
                    busy = true;
                } else if (nextState == Next::None) {
                    WriteTail();
                    drained = true;
                }
            }
            if (!busy) {
                std::unique_lock<std::mutex> lock(cmdMx);
                auto commanded = [&] { return quit.load() || !cmds.empty(); };
                // No audio can advance while stopped or after a pause has faded out and stopped
                // the device. Commands, resume and shutdown wake us directly;
                // keep the short timed wait only while playback needs housekeeping/refilling.
                if ((idle || pauseReq) && out.Paused()) cv.wait(lock, [&] { return commanded() || !out.Paused(); });
                else cv.wait_for(lock, std::chrono::milliseconds(10), commanded);
            }
        }
        dec.reset();
        CoUninitialize();
    }

    // ---- queries

    double Position() const {
        std::lock_guard<std::mutex> lock(ringMx);
        if (markers.empty()) return 0;
        unsigned long long played = out.PlayedPosition();
        const Marker* m = MarkerAt(played);
        if (!m) return markers.front().offset;
        double t = m->offset + (double)(played - m->pos) / rate * m->stretch;
        if (m->item.loopEnd > m->item.loopStart && t >= m->item.loopStart)
            return m->item.loopStart + std::fmod(t - m->item.loopStart, m->item.loopEnd - m->item.loopStart);
        return m->duration > 0 ? std::min(t, m->duration) : t;
    }
};

// ---------------------------------------------------------------------------

Engine::Engine() : impl_(std::make_unique<Impl>()) {}

std::vector<float> RenderPlayerPcm(const float* stereo, size_t frames, unsigned rate,
                                 const EqSettings& eq, bool replayGainLimiter, float trackGain, float outputHeadroomDb,
                                 const RenderEffects* effects) {
    if (!stereo || !frames || !rate) return {};
    Engine::Impl pipeline;
    pipeline.rate = rate;
    pipeline.AllocateRing();
    pipeline.ramp = 1;
    pipeline.smoothGain = trackGain;
    pipeline.markers.push_back({0, 1, 0, trackGain, 0, {}});
    if (effects) {
        pipeline.reverbPending = effects->reverb;
        pipeline.reverbDirty = true;
        pipeline.volume = effects->volume;
        pipeline.smoothGain = trackGain * effects->volume;
        pipeline.loudnessOn = effects->loudness;
        pipeline.nightOn = effects->night;
    }
    pipeline.eqPending = eq;
    pipeline.eqDirty = true;
    pipeline.limiterOn = replayGainLimiter;
    pipeline.limiterHeadroom = outputHeadroomDb;
    std::vector<float> output(frames * 2);
    for (size_t at = 0; at < frames;) {
        if (effects && at == effects->volumeChangeFrame) pipeline.volume = effects->volumeAfter;
        size_t count = std::min<size_t>(137, frames - at);
        if (effects && at < effects->volumeChangeFrame)
            count = std::min(count, effects->volumeChangeFrame - at);
        pipeline.WriteRing(stereo + at * 2, count);
        unsigned long long position = 0;
        if (pipeline.Render(output.data() + at * 2, count, position) != count) return {};
        at += count;
    }
    return output;
}

bool BenchmarkPlayerPipeline(const std::wstring& path,unsigned outputRate,unsigned passes,PipelineTiming& result) {
    std::wstring error;auto decoder=OpenDecoder(path,outputRate,error);
    if (!decoder || !passes) return false;
    Engine::Impl pipeline;
    if (!outputRate) outputRate=decoder->Rate();
    pipeline.rate=outputRate;pipeline.AllocateRing();
    pipeline.rs.Init(decoder->Rate(),outputRate);
    Resampler reference;reference.Init(decoder->Rate(),outputRate);
    bool verify=true;
    float input[4096];std::vector<float> rendered,gold;
    auto drain=[&](uint64_t& native,uint64_t& output) {
        native=output=0;pipeline.readPos=pipeline.writePos=0;
        pipeline.ramp=pipeline.smoothGain=1;
        pipeline.rs.Reset();
        auto render=[&](size_t frames) {
            if (frames>pipeline.cap) return false;
            if (rendered.size()<frames*2) rendered.resize(frames*2);
            unsigned long long position=0;
            if (pipeline.Render(rendered.data(),frames,position)!=frames) return false;
            if (verify) {
                if (frames*2!=gold.size()) return false;
                for (size_t i=0;i<gold.size();++i)
                    if (rendered[i]!=std::clamp(gold[i],-1.0f,1.0f)) return false;
            }
            ObservePipelinePcm(rendered.data(),frames);
            output+=frames;return true;
        };
        while (size_t n=decoder->Read(input,2048)) {
            native+=n;
            if (verify) { gold.clear();reference.Push(input,n,gold); }
            if (!render(pipeline.QueueDecoded(input,n))) return false;
        }
        pipeline.rsOut.clear();pipeline.rs.Flush(pipeline.rsOut);
        if (verify) { gold.clear();reference.Flush(gold); }
        pipeline.WriteRing(pipeline.rsOut.data(),pipeline.rsOut.size()/2);
        return render(pipeline.rsOut.size()/2);
    };
    if (!drain(result.inputFrames,result.outputFrames) || !result.inputFrames) return false;
    verify=false;
    result.inputRate=decoder->Rate();
    LARGE_INTEGER frequency,start,end;QueryPerformanceFrequency(&frequency);QueryPerformanceCounter(&start);
    for (unsigned pass=0;pass<passes;++pass) {
        uint64_t native,output;
        if (!decoder->Seek(0) || !drain(native,output) || native!=result.inputFrames || output!=result.outputFrames) return false;
    }
    QueryPerformanceCounter(&end);
    result.seconds=(end.QuadPart-start.QuadPart)/(double)frequency.QuadPart/passes;
    return true;
}
Engine::~Engine() { Shutdown(); }

bool Engine::Init(HWND notify, std::wstring& error, bool deferOutput) {
    Impl& d = *impl_;
    d.notify = notify;
    // OpenTrack allocates audio buffers on the first Play command.
    d.out.SetPaused(true);  // no silent render stream before the first track
    bool ok = deferOutput || d.StartOutput(error);
    d.eqDirty = true;
    d.thread = std::thread([&d] { d.DecodeLoop(); });
    return ok;
}

void Engine::Shutdown() {
    Impl& d = *impl_;
    if (d.thread.joinable()) {
        {
            std::lock_guard<std::mutex> lock(d.liveMx);
            if (d.requestedLive) d.requestedLive->Abort();
            d.requestedLive.reset();
        }
        d.quit = true;
        // The decode thread owns its decoder until it has joined.
        d.cv.notify_all();
        d.thread.join();
    }
    d.out.Stop();
    d.outputStarted = false;
    d.dec.reset();
    d.waiting.reset();
    d.curItem = d.nextItem = d.waitingItem = PlayItem{};
    std::vector<float>().swap(d.readBuf);
    std::vector<float>().swap(d.rsOut);
    d.rs = Resampler{};
    {
        std::lock_guard<std::mutex> lock(d.ringMx);
        std::vector<float>().swap(d.ring);
        d.cap = 0;
        d.markers.clear();
        d.gains.clear();
        d.readPos = d.writePos;
    }
    { std::lock_guard<std::mutex> lock(d.cmdMx); d.cmds.clear(); }
    { std::lock_guard<std::mutex> lock(d.errMx); d.errors.clear(); }
}

void Engine::Play(const PlayItem& item, double start, bool paused) {
    Command c{Command::Play};
    c.item = item;
    c.hasItem = true;
    c.seconds = start;
    c.paused = paused;
    // The UI asks "am I paused?" right away: answer before the decode thread reaches the command,
    // which sets the same state again when it opens the track.
    if (paused) impl_->pauseReq = true;
    impl_->Enqueue(std::move(c));
}

void Engine::SetNext(unsigned long long after, const PlayItem* next) {
    Command c{Command::SetNext};
    c.after = after;
    if (next) {
        c.item = *next;
        c.hasItem = true;
    }
    impl_->Enqueue(std::move(c));
}

void Engine::Stop() { impl_->Enqueue({Command::Stop}); }

// Pausing is a fade in Render, so the device never stops mid-wave; once the fade has played out,
// Housekeeping stops the device.
void Engine::SetPaused(bool paused) {
    {
        std::lock_guard<std::mutex> outputLock(impl_->outputMx);
        // Pair with DecodeLoop's wait so resuming an idle endpoint cannot lose its wakeup.
        std::lock_guard<std::mutex> lock(impl_->cmdMx);
        if (!paused) impl_->out.SetPaused(false);  // a pause long enough to stop the device restarts it
        impl_->pauseReq = paused;
    }
    impl_->cv.notify_one();
}

void Engine::Seek(double seconds, const PlayItem* updated) {
    if (!std::isfinite(seconds)) return;
    Command c{Command::Seek};
    c.seconds = seconds;
    if (updated) { c.item = *updated; c.hasItem = true; }
    impl_->Enqueue(std::move(c));
}

void Engine::SetVolume(float linear) { impl_->volume = std::clamp(linear, 0.0f, 1.0f); }

void Engine::SetLoops(int count, double fadeSeconds) {
    impl_->loopCount = std::clamp(count, 0, 100);
    impl_->loopFade = std::max(0.0, fadeSeconds);
}

void Engine::SetExclusive(bool exclusive) {
    std::lock_guard<std::mutex> lock(impl_->outputMx);
    impl_->out.SetMode(exclusive ? OutputMode::Exclusive : OutputMode::Shared);
}

#ifndef _WIN32
void Engine::SetOutputDevice(const std::string& node) {
    std::lock_guard<std::mutex> lock(impl_->outputMx);
    impl_->out.SetDevice(node);
}
#endif

Engine::OutputInfo Engine::Output() const {
    const Impl& d = *impl_;
    OutputInfo o;
    o.ready = d.outputStarted;
    o.exclusive = d.out.Mode() == OutputMode::Exclusive;
    o.waiting = d.out.Waiting();
    o.rate = d.out.Rate();
    o.bits = SampleBits(d.out.Format());
    o.floatFormat = d.out.Format() == SampleFormat::Float32;
    o.problem = d.out.Problem();
    // Nothing between the decoder and the device: no resampling, gain, EQ or limiter, and a
    // device format that holds 24-bit samples. ReplayGain on puts the limiter in the path (it
    // delays the output and can hold peaks down), so that is not bit-perfect even at unity gain.
    o.bitPerfect = o.exclusive && d.inputRate.load() == o.rate && d.volume.load() == 1.0f && d.playingGain.load() == 1.0f &&
                   !d.eqActive.load() && !d.limiterOn.load() && !d.effectsActive.load() && d.speedReq.load() == 1.0 &&
                   d.tempoReq.load() == 1.0 && (o.floatFormat || o.bits >= 24);
    return o;
}

void Engine::SetEq(const EqSettings& eq) {
    std::lock_guard<std::mutex> lock(impl_->eqMx);
    impl_->eqPending = eq;
    impl_->eqDirty = true;
}

void Engine::SetLimiter(bool on, float headroomDb) {
    impl_->limiterHeadroom = std::isfinite(headroomDb) ? std::clamp(headroomDb, 0.0f, 24.0f) : 0;
    impl_->limiterOn = on;
}

void Engine::SetReverb(const ReverbSettings& reverb) {
    std::lock_guard<std::mutex> lock(impl_->fxMx);
    impl_->reverbPending = reverb;
    impl_->reverbDirty = true;
}

void Engine::SetSmartVolume(bool loudness, bool night) {
    impl_->loudnessOn = loudness;
    impl_->nightOn = night;
}

void Engine::SetSpeed(double speed) {
    speed = std::isfinite(speed) ? std::clamp(speed, 0.25, 4.0) : 1.0;
    if (impl_->speedReq.exchange(speed) != speed) impl_->Enqueue({Command::Retime});
}

void Engine::SetTempo(double tempo) {
    tempo = std::isfinite(tempo) ? std::clamp(tempo, 0.25, 4.0) : 1.0;
    if (impl_->tempoReq.exchange(tempo) != tempo) impl_->Enqueue({Command::Retime});
}

double Engine::TakeEstimate(unsigned long long token) {
    std::lock_guard<std::mutex> lock(impl_->ringMx);
    auto it = impl_->estimates.find(token);
    if (it == impl_->estimates.end()) return std::nan("");
    double lufs = it->second;
    impl_->estimates.erase(it);
    return lufs;
}

void Engine::SetGain(unsigned long long token, float gain, bool glide) {
    std::lock_guard<std::mutex> lock(impl_->ringMx);
    impl_->gains[token] = gain;
    if (impl_->gains.size() > 64) {  // keep only what is still referenced
        for (auto it = impl_->gains.begin(); it != impl_->gains.end();) {
            bool used = it->first == token || std::any_of(impl_->markers.begin(), impl_->markers.end(),
                                                          [&](const Marker& m) { return m.token == it->first; });
            it = used ? std::next(it) : impl_->gains.erase(it);
        }
    }
    for (Marker& m : impl_->markers)
        if (m.token == token) {
            m.gain = gain;
            m.glide = glide;
        }
}

bool Engine::Paused() const { return impl_->pauseReq.load(); }
double Engine::Position() const { return impl_->Position(); }
float Engine::PlayingGain() const { return impl_->playingGain.load(); }
unsigned Engine::Rate() const { return impl_->rate; }

void Engine::SetScope(bool on) {
    Impl& d = *impl_;
    if (on) {
        std::lock_guard<std::mutex> lock(d.scopeMx);
        if (!d.scopeOn) {
            d.scope.fill(0);
            d.scopeAt = 0;
        }
    }
    d.scopeOn = on;
}

size_t Engine::VisualSamples(float* out, size_t frames, double& time) const {
    Impl& d = *impl_;
    std::fill_n(out, frames * 2, 0.0f);
    std::lock_guard<std::mutex> lock(d.ringMx);
    unsigned rate = d.rate.load();
    if (!rate || !d.cap || !d.writePos) return 0;
    const auto center = d.out.PlayedPosition();
    time = (double)center / rate;
    const int64_t first = (int64_t)center - (int64_t)(frames / 2);
    const auto oldest = d.writePos > d.cap ? d.writePos - d.cap : 0;
    int64_t begin = std::max(first, (int64_t)oldest), end = std::min(first + (int64_t)frames, (int64_t)d.writePos);
    if (begin < end) {
        size_t slot = (size_t)((uint64_t)begin % d.cap), count = (size_t)(end - begin);
        float* dest = out + (begin - first) * 2;
        size_t contiguous = std::min(count, d.cap - slot);
        memcpy(dest, d.ring.data() + slot * 2, contiguous * 2 * sizeof(float));
        if (contiguous < count) memcpy(dest + contiguous * 2, d.ring.data(), (count - contiguous) * 2 * sizeof(float));
    }
    return frames;
}

bool Engine::WaveformPeaks(uint8_t* out, size_t buckets) const {
    if (!out || !buckets || buckets > 4096) return false;
    std::fill_n(out, buckets, uint8_t{});
    Impl& d = *impl_;
    std::lock_guard<std::mutex> lock(d.ringMx);
    unsigned rate = d.rate.load();
    if (!rate || !d.cap || !d.writePos) return false;
    uint64_t end = std::min(d.out.PlayedPosition(), d.writePos);
    uint64_t frames = std::max(1u, rate / 4), first = end > frames ? end - frames : 0;
    uint64_t oldest = std::max<uint64_t>(d.waveStart, d.writePos > d.cap ? d.writePos - d.cap : 0);
    if (end <= oldest) return false;
    if (d.wavePos < oldest || d.wavePos > end) {
        d.waveMotion = {};
        d.wavePos = oldest;
    }
    while (d.wavePos < end) {
        size_t slot = (size_t)(d.wavePos % d.cap), count = (size_t)std::min<uint64_t>(end - d.wavePos, d.cap - slot);
        d.waveMotion.Add(d.ring.data() + slot * 2, count, rate);
        d.wavePos += count;
    }
    for (uint64_t at = std::max(first, oldest); at < end; ++at) {
        size_t slot = (size_t)(at % d.cap) * 2;
        float peak = std::max(std::fabs(d.ring[slot]), std::fabs(d.ring[slot + 1]));
        size_t bucket = (size_t)((frames - (end - at)) * buckets / frames);
        out[bucket] = std::max(out[bucket], CompandPeak(peak));
    }
    d.waveMotion.Shape(out, buckets);
    return true;
}

size_t Engine::Scope(float* out, size_t frames) const {
    Impl& d = *impl_;
    std::lock_guard<std::mutex> lock(d.scopeMx);
    const size_t capacity = d.scope.size() / 2;
    frames = std::min(frames, capacity);
    for (size_t i = 0; i < frames; ++i) {
        size_t at = (d.scopeAt + capacity - frames + i) % capacity;
        out[i * 2] = d.scope[at * 2];
        out[i * 2 + 1] = d.scope[at * 2 + 1];
    }
    return frames;
}

double Engine::Duration() const {
    std::lock_guard<std::mutex> lock(impl_->ringMx);
    const Marker* m = impl_->MarkerAt(impl_->out.PlayedPosition());
    if (!m && !impl_->markers.empty()) m = &impl_->markers.front();
    return m ? m->duration : 0;
}

std::wstring Engine::TakeError(unsigned long long token) {
    std::lock_guard<std::mutex> lock(impl_->errMx);
    auto& errors = impl_->errors;
    auto at = token ? std::find_if(errors.begin(), errors.end(),
        [token](const auto& error) { return error.first == token; }) : errors.begin();
    if (at == errors.end()) return {};
    std::wstring message = std::move(at->second);
    errors.erase(at);
    return message;
}
