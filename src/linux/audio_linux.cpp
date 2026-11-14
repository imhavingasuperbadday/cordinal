// AudioOutput (audio_output.h) for the Linux build, on the sound server through sound_stream.h
// (PipeWire's native protocol, or PulseAudio's).
//
// One playback stream of interleaved stereo float, following the default device unless a sink was
// chosen.
// Shared mode: the stream runs at 48 kHz and the engine resamples, like the Windows mixer.
// Exclusive mode: the stream is opened at the track's own rate and asks PipeWire to run the
// device at that rate (node.rate). PipeWire does so when nothing else is playing and the rate is
// among its allowed rates; the stream then reaches the device unresampled. The server's actual
// rate is checked on every cycle: while it runs at another rate (the stream resamples then), the
// output reports shared mode with the reason, so cordinal never claims bit-perfect output it
// doesn't have. PulseAudio always mixes, so exclusive mode on it is reported as shared.
// The server may start after cordinal (at login) or restart: a stream that can't connect, or
// fails, is opened again every second.
#include "audio_output.h"

#include "sound_stream.h"

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <vector>

namespace {

constexpr unsigned kSharedRate = 48000;

std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

}  // namespace

// The stream and its bookkeeping. `stream` is replaced only under streamMx; the render-side fields
// are touched by the stream's callbacks, or between streams under streamMx.
struct PipewireState {
    AudioOutput* out = nullptr;
    std::mutex streamMx;
    std::unique_ptr<lx::sound::Stream> stream;
    unsigned rate = kSharedRate;
    bool exclusive = false;
    std::string backend = "PipeWire";
    // Device frames -> source positions, one entry per buffer written (as on Windows).
    struct Chunk {
        unsigned long long deviceEnd, sourceEnd;
        size_t audio, total;
    };
    std::vector<Chunk> chunks;
    unsigned long long deviceWritten = 0, sourcePos = 0;
    std::atomic<bool> flushRequest{false};
    std::string device;  // the sink to open the stream on; empty is the default
    std::atomic<bool> failed{false};  // the stream failed (the server went away): open it again
    std::mutex retryMx;
    std::condition_variable retryCv;
};

namespace {

void ApplyFlush(PipewireState& st, AudioOutput& out, unsigned long long flushPos) {
    st.sourcePos = flushPos;
    st.chunks.clear();
}

void OnRender(PipewireState& st, float* dst, uint32_t frames) {
    AudioOutput* out = st.out;
    size_t audio = out->RenderInto(dst, frames, st.sourcePos);
    st.deviceWritten += frames;
    if (st.chunks.size() > 64) st.chunks.erase(st.chunks.begin(), st.chunks.end() - 32);
    st.chunks.push_back({st.deviceWritten, st.sourcePos, audio, frames});
}

// What the speakers have reached: the chunk holding the device's played frame.
void OnTiming(PipewireState& st, uint64_t playedDevice, unsigned serverRate) {
    AudioOutput* out = st.out;
    if (st.flushRequest) return;  // the position was set by Flush; the next render applies it
    unsigned long long played = st.sourcePos;
    for (const auto& c : st.chunks) {
        if (c.deviceEnd <= playedDevice) continue;
        unsigned long long start = c.deviceEnd - c.total;
        size_t into = playedDevice > start ? (size_t)(playedDevice - start) : 0;
        played = c.sourceEnd - c.audio + std::min(into, c.audio);
        break;
    }
    out->SetPlayed(played);
    out->SetGraphRate(serverRate);
}

void CloseStream(PipewireState& st) { st.stream.reset(); }

bool OpenStream(PipewireState& st, bool exclusive, unsigned rate, bool active, std::string& error) {
    CloseStream(st);
    st.exclusive = exclusive;
    st.rate = rate;
    st.deviceWritten = 0;
    st.chunks.clear();
    lx::sound::Params p;
    p.name = "cordinal";
    p.role = "Music";
    p.rate = rate;
    p.latencyFrames = rate / 50;  // 20 ms periods
    p.exclusive = exclusive;
    p.target = st.device;
    p.active = active;
    lx::sound::Callbacks cb;
    PipewireState* s = &st;
    cb.render = [s](float* out, uint32_t frames) {
        if (s->flushRequest.exchange(false)) ApplyFlush(*s, *s->out, s->out->PlayedPosition());
        OnRender(*s, out, frames);
    };
    cb.timing = [s](uint64_t played, unsigned serverRate) { OnTiming(*s, played, serverRate); };
    st.failed = false;
    st.stream = lx::sound::Open(p, std::move(cb), [s] {
        s->failed = true;
        s->retryCv.notify_all();
    }, error);
    if (st.stream) st.backend = st.stream->Backend();
    return st.stream != nullptr;
}

}  // namespace

std::vector<OutputDevice> ListOutputDevices() {
    std::vector<OutputDevice> out;
    for (auto& s : lx::sound::ListSinks()) out.push_back({s.name, s.description, s.alsaCard, s.systemDefault});
    return out;
}

AudioOutput::AudioOutput() = default;
AudioOutput::~AudioOutput() { Stop(); }

size_t AudioOutput::RenderInto(float* out, size_t frames, unsigned long long& sourcePos) {
    return render_(out, frames, sourcePos);
}

void AudioOutput::SetGraphRate(unsigned graphRate) {
    // Exclusive is only true while the server really runs the device at the stream's rate.
    if (wantedMode_ != OutputMode::Exclusive || !graphRate) return;
    bool pulse = state_ && state_->backend == "PulseAudio";
    bool atRate = graphRate == rate_ && !pulse;
    OutputMode now = atRate ? OutputMode::Exclusive : OutputMode::Shared;
    if (mode_ == now) return;
    mode_ = now;
    std::lock_guard<std::mutex> lock(problemMx_);
    if (atRate) {
        problem_.clear();
    } else if (pulse) {
        problem_ = L"PulseAudio mixes every stream (no exclusive output through it).";
    } else {
        wchar_t buf[160];
        swprintf(buf, std::size(buf), L"PipeWire is running at %g kHz (another program is playing, or %g kHz isn't allowed); it resamples.",
                 graphRate / 1000.0, rate_ / 1000.0);
        problem_ = buf;
    }
}

bool AudioOutput::Start(RenderFn render, ResetFn reset, std::wstring& error) {
    render_ = std::move(render);
    reset_ = std::move(reset);
    state_ = std::make_unique<PipewireState>();
    state_->out = this;
    {
        std::lock_guard<std::mutex> lock(problemMx_);
        state_->device = device_;
    }
    rate_ = kSharedRate;
    mode_ = OutputMode::Shared;
    format_ = SampleFormat::Float32;
    std::string why;
    bool ok;
    {
        std::lock_guard lock(state_->streamMx);
        ok = OpenStream(*state_, false, kSharedRate, !paused_, why);
    }
    if (!ok) {
        std::lock_guard<std::mutex> lock(problemMx_);
        problem_ = L"Waiting for PipeWire or PulseAudio (no audio output yet: " + Widen(why) + L").";
    }
    quit_ = false;
    thread_ = std::thread([this] {
        while (!quit_) {
            {
                std::unique_lock lock(state_->retryMx);
                state_->retryCv.wait_for(lock, std::chrono::seconds(1));
            }
            if (quit_) break;
            {
                std::lock_guard lock(state_->streamMx);
                if (state_->stream && !state_->failed) continue;
            }
            Reopen();
        }
    });
    ok_ = true;
    return true;
}

// The stream again, as it was (mode, rate, position), after the server came (back).
void AudioOutput::Reopen() {
    std::string why;
    bool ok;
    {
        std::lock_guard lock(state_->streamMx);
        unsigned long long pos = state_->sourcePos;
        bool exclusive = wantedMode_ == OutputMode::Exclusive;
        ok = OpenStream(*state_, exclusive, exclusive ? rate_.load() : kSharedRate, !paused_, why);
        state_->sourcePos = pos;
    }
    waiting_ = !ok;
    std::lock_guard<std::mutex> lock(problemMx_);
    if (ok) problem_.clear();
    else problem_ = L"Waiting for PipeWire or PulseAudio (no audio output yet: " + Widen(why) + L").";
}

void AudioOutput::Stop() {
    if (!state_) return;
    quit_ = true;
    state_->retryCv.notify_all();
    if (thread_.joinable()) thread_.join();
    {
        std::lock_guard lock(state_->streamMx);
        CloseStream(*state_);
    }
    state_.reset();
    ok_ = false;
}

void AudioOutput::wake() {
    // Pause, resume and flush act on the stream right away (the Windows build wakes its thread).
    if (!state_) return;
    std::lock_guard lock(state_->streamMx);
    if (flush_.exchange(false)) {
        played_ = flushPos_.load();
        state_->flushRequest = true;
        if (state_->stream) state_->stream->Flush();
        else ApplyFlush(*state_, *this, flushPos_);
    }
    if (state_->stream) state_->stream->SetActive(!paused_);
}

void AudioOutput::SetDevice(const std::string& node) {
    {
        std::lock_guard<std::mutex> lock(problemMx_);
        if (device_ == node) return;
        device_ = node;
    }
    if (!state_) return;
    std::string why;
    {
        std::lock_guard lock(state_->streamMx);
        unsigned long long pos = state_->sourcePos;
        state_->device = node;
        bool exclusive = wantedMode_ == OutputMode::Exclusive;
        OpenStream(*state_, exclusive, exclusive ? rate_.load() : kSharedRate, !paused_, why);
        state_->sourcePos = pos;
    }
    mode_ = OutputMode::Shared;  // exclusive once the server is seen running at the rate
}

void AudioOutput::SetMode(OutputMode mode) {
    wantedMode_ = mode;
    if (!state_) return;
    unsigned rate = mode == OutputMode::Exclusive ? rate_.load() : kSharedRate;
    std::string why;
    {
        std::lock_guard lock(state_->streamMx);
        OpenStream(*state_, mode == OutputMode::Exclusive, rate, !paused_, why);
    }
    mode_ = OutputMode::Shared;  // exclusive once the server is seen running at the rate
    {
        std::lock_guard<std::mutex> lock(problemMx_);
        problem_.clear();
    }
    if (rate != rate_) {
        rate_ = rate;
        if (reset_) reset_(rate);
    }
}

unsigned AudioOutput::RequestRate(unsigned rate) {
    if (wantedMode_ != OutputMode::Exclusive || !state_) return rate_;
    std::string why;
    bool ok;
    {
        std::lock_guard lock(state_->streamMx);
        if (rate == rate_ && state_->exclusive && state_->stream) return rate_;
        unsigned long long pos = state_->sourcePos;
        ok = OpenStream(*state_, true, rate, !paused_, why);
        state_->sourcePos = pos;
    }
    if (ok) rate_ = rate;
    mode_ = OutputMode::Shared;
    return rate_;
}

std::wstring AudioOutput::Problem() const {
    std::lock_guard<std::mutex> lock(problemMx_);
    return problem_;
}
