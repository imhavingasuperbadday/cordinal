// AudioOutput (audio_output.h) on PipeWire, for the Linux build.
//
// One playback stream of interleaved stereo float, following the default device (PipeWire moves
// it when the default changes).
// Shared mode: the stream runs at 48 kHz and the engine resamples, like the Windows mixer.
// Exclusive mode: the stream is opened at the track's own rate and asks PipeWire to run the
// device at that rate (node.rate). PipeWire does so when nothing else is playing and the rate is
// among its allowed rates; the stream then reaches the device unresampled. The graph's actual
// rate is checked on every cycle: while PipeWire runs at another rate, the output reports shared
// mode with the reason, so cordinal never claims bit-perfect output it doesn't have.
// PipeWire may start after cordinal (at login) or restart: a stream that can't connect, or
// fails, is opened again every second.
#include "audio_output.h"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/result.h>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <vector>

namespace {

constexpr unsigned kSharedRate = 48000;

struct Pipewire {
    pw_thread_loop* loop = nullptr;
    pw_stream* stream = nullptr;
    spa_hook listener{};
    unsigned rate = kSharedRate;
    bool exclusive = false;
};

std::once_flag g_pwInit;

}  // namespace

// The stream's state, touched only on the PipeWire loop thread or under its lock.
struct PipewireState {
    AudioOutput* out = nullptr;
    Pipewire pw;
    // Device frames -> source positions, one entry per buffer written (as on Windows).
    struct Chunk {
        unsigned long long deviceEnd, sourceEnd;
        size_t audio, total;
    };
    std::vector<Chunk> chunks;
    unsigned long long deviceWritten = 0, sourcePos = 0;
    unsigned graphRate = 0;
    std::string device;  // the sink to open the stream on; empty is the default
    std::atomic<bool> failed{false};  // the stream errored (PipeWire went away): open it again
    bool closing = false;             // destroying the stream on purpose: not a failure
    std::mutex retryMx;
    std::condition_variable retryCv;
};

namespace {

void OnProcess(void* data) {
    auto* st = static_cast<PipewireState*>(data);
    AudioOutput* out = st->out;
    pw_buffer* b = pw_stream_dequeue_buffer(st->pw.stream);
    if (!b) return;
    spa_buffer* buf = b->buffer;
    auto* dst = static_cast<float*>(buf->datas[0].data);
    if (!dst) {
        pw_stream_queue_buffer(st->pw.stream, b);
        return;
    }
    const uint32_t stride = sizeof(float) * 2;
    uint32_t frames = buf->datas[0].maxsize / stride;
    if (b->requested) frames = std::min<uint32_t>(frames, (uint32_t)b->requested);
    size_t audio = out->RenderInto(dst, frames, st->sourcePos);
    st->deviceWritten += frames;
    if (st->chunks.size() > 64) st->chunks.erase(st->chunks.begin(), st->chunks.end() - 32);
    st->chunks.push_back({st->deviceWritten, st->sourcePos, audio, frames});
    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = stride;
    buf->datas[0].chunk->size = frames * stride;
    pw_stream_queue_buffer(st->pw.stream, b);

    // What the speakers have reached: everything written minus what is still queued on the way.
    pw_time t{};
    if (pw_stream_get_time_n(st->pw.stream, &t, sizeof(t)) == 0) {
        unsigned graph = t.rate.denom ? t.rate.denom : 0;
        if (graph) st->graphRate = graph;
        double delay = graph && st->pw.rate ? (double)t.delay * st->pw.rate / graph : 0;
        unsigned long long queued = (unsigned long long)std::max(0.0, delay) + t.queued / stride + t.buffered;
        unsigned long long playedDevice = st->deviceWritten > queued ? st->deviceWritten - queued : 0;
        unsigned long long played = st->sourcePos;
        for (const auto& c : st->chunks) {
            if (c.deviceEnd <= playedDevice) continue;
            unsigned long long start = c.deviceEnd - c.total;
            size_t into = playedDevice > start ? (size_t)(playedDevice - start) : 0;
            played = c.sourceEnd - c.audio + std::min(into, c.audio);
            break;
        }
        out->SetPlayed(played);
        out->SetGraphRate(st->graphRate);
    }
}

void OnStateChanged(void* data, pw_stream_state, pw_stream_state state, const char*) {
    auto* st = static_cast<PipewireState*>(data);
    if (!st->closing && (state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED)) {
        st->failed = true;
        st->retryCv.notify_all();
    }
}

const pw_stream_events kEvents = [] {
    pw_stream_events e{};
    e.version = PW_VERSION_STREAM_EVENTS;
    e.state_changed = OnStateChanged;
    e.process = OnProcess;
    return e;
}();

void CloseStream(PipewireState& st) {
    if (st.pw.stream) {
        st.closing = true;
        pw_stream_destroy(st.pw.stream);
        st.closing = false;
        st.pw.stream = nullptr;
    }
}

bool OpenStream(PipewireState& st, bool exclusive, unsigned rate, bool active) {
    CloseStream(st);
    st.pw.exclusive = exclusive;
    st.pw.rate = rate;
    char rateText[32], latency[32];
    snprintf(rateText, sizeof(rateText), "1/%u", rate);
    snprintf(latency, sizeof(latency), "%u/%u", rate / 50, rate);  // 20 ms periods
    pw_properties* props =
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE, "Music",
                          PW_KEY_APP_NAME, "cordinal", PW_KEY_NODE_NAME, "cordinal", PW_KEY_NODE_LATENCY, latency, nullptr);
    if (exclusive) pw_properties_set(props, PW_KEY_NODE_RATE, rateText);  // ask the graph to run at our rate
    if (!st.device.empty()) pw_properties_set(props, PW_KEY_TARGET_OBJECT, st.device.c_str());
    st.pw.stream = pw_stream_new_simple(pw_thread_loop_get_loop(st.pw.loop), "cordinal", props, &kEvents, &st);
    if (!st.pw.stream) return false;
    uint8_t buffer[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.rate = rate;
    info.channels = 2;
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;
    const spa_pod* params[1] = {spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info)};
    auto flags = (pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                   (active ? 0 : PW_STREAM_FLAG_INACTIVE));
    if (pw_stream_connect(st.pw.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, params, 1) < 0) {
        CloseStream(st);
        return false;
    }
    st.deviceWritten = 0;
    st.chunks.clear();
    st.graphRate = 0;
    return true;
}

}  // namespace

// The sinks, from a short-lived connection of its own: the registry announces every global at once, and a
// sync round trip says when that is all of them.
namespace {

struct SinkList {
    pw_main_loop* loop = nullptr;
    pw_core* core = nullptr;
    spa_hook coreListener{}, registryListener{};
    int pending = 0;
    std::vector<OutputDevice> sinks;
};

void OnGlobal(void* data, uint32_t, uint32_t, const char* type, uint32_t, const spa_dict* props) {
    auto* list = static_cast<SinkList*>(data);
    if (!props || strcmp(type, PW_TYPE_INTERFACE_Node)) return;
    const char* mediaClass = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    const char* name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    if (!mediaClass || !name || strcmp(mediaClass, "Audio/Sink")) return;
    const char* text = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
    if (!text) text = spa_dict_lookup(props, PW_KEY_NODE_NICK);
    list->sinks.push_back({name, text ? text : name});
}

void OnDone(void* data, uint32_t id, int seq) {
    auto* list = static_cast<SinkList*>(data);
    if (id == PW_ID_CORE && seq == list->pending) pw_main_loop_quit(list->loop);
}

}  // namespace

std::vector<OutputDevice> ListOutputDevices() {
    std::call_once(g_pwInit, [] { pw_init(nullptr, nullptr); });
    SinkList list;
    list.loop = pw_main_loop_new(nullptr);
    if (!list.loop) return {};
    pw_context* context = pw_context_new(pw_main_loop_get_loop(list.loop), nullptr, 0);
    list.core = context ? pw_context_connect(context, nullptr, 0) : nullptr;
    if (list.core) {
        static const pw_core_events coreEvents = [] {
            pw_core_events e{};
            e.version = PW_VERSION_CORE_EVENTS;
            e.done = OnDone;
            return e;
        }();
        static const pw_registry_events registryEvents = [] {
            pw_registry_events e{};
            e.version = PW_VERSION_REGISTRY_EVENTS;
            e.global = OnGlobal;
            return e;
        }();
        pw_core_add_listener(list.core, &list.coreListener, &coreEvents, &list);
        pw_registry* registry = pw_core_get_registry(list.core, PW_VERSION_REGISTRY, 0);
        pw_registry_add_listener(registry, &list.registryListener, &registryEvents, &list);
        list.pending = pw_core_sync(list.core, PW_ID_CORE, 0);
        // A server that never answers must not hang the window.
        pw_loop* loop = pw_main_loop_get_loop(list.loop);
        spa_source* timer = pw_loop_add_timer(loop, [](void* data, uint64_t) { pw_main_loop_quit(static_cast<pw_main_loop*>(data)); }, list.loop);
        timespec timeout{2, 0};
        pw_loop_update_timer(loop, timer, &timeout, nullptr, false);
        pw_main_loop_run(list.loop);
        pw_loop_destroy_source(loop, timer);
        spa_hook_remove(&list.registryListener);
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry));
        spa_hook_remove(&list.coreListener);
        pw_core_disconnect(list.core);
    }
    if (context) pw_context_destroy(context);
    pw_main_loop_destroy(list.loop);
    std::sort(list.sinks.begin(), list.sinks.end(), [](const OutputDevice& a, const OutputDevice& b) { return a.description < b.description; });
    return list.sinks;
}

AudioOutput::AudioOutput() = default;
AudioOutput::~AudioOutput() { Stop(); }

size_t AudioOutput::RenderInto(float* out, size_t frames, unsigned long long& sourcePos) {
    return render_(out, frames, sourcePos);
}

void AudioOutput::SetGraphRate(unsigned graphRate) {
    // Exclusive is only true while PipeWire really runs at the stream's rate.
    if (wantedMode_ != OutputMode::Exclusive || !graphRate) return;
    bool atRate = graphRate == rate_;
    OutputMode now = atRate ? OutputMode::Exclusive : OutputMode::Shared;
    if (mode_ == now) return;
    mode_ = now;
    std::lock_guard<std::mutex> lock(problemMx_);
    if (atRate) {
        problem_.clear();
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
    std::call_once(g_pwInit, [] { pw_init(nullptr, nullptr); });
    state_ = std::make_unique<PipewireState>();
    state_->out = this;
    {
        std::lock_guard<std::mutex> lock(problemMx_);
        state_->device = device_;
    }
    state_->pw.loop = pw_thread_loop_new("cordinal-audio", nullptr);
    if (!state_->pw.loop || pw_thread_loop_start(state_->pw.loop) < 0) {
        error = L"PipeWire isn't running (no audio output).";
        state_.reset();
        return false;
    }
    rate_ = kSharedRate;
    mode_ = OutputMode::Shared;
    format_ = SampleFormat::Float32;
    pw_thread_loop_lock(state_->pw.loop);
    bool ok = OpenStream(*state_, false, kSharedRate, !paused_);
    pw_thread_loop_unlock(state_->pw.loop);
    if (!ok) {
        std::lock_guard<std::mutex> lock(problemMx_);
        problem_ = L"Waiting for PipeWire (no audio output yet).";
    }
    quit_ = false;
    thread_ = std::thread([this] {
        while (!quit_) {
            {
                std::unique_lock lock(state_->retryMx);
                state_->retryCv.wait_for(lock, std::chrono::seconds(1));
            }
            if (quit_) break;
            if (state_->pw.stream && !state_->failed) continue;
            Reopen();
        }
    });
    ok_ = true;
    return true;
}

// The stream again, as it was (mode, rate, position), after PipeWire came (back).
void AudioOutput::Reopen() {
    pw_thread_loop_lock(state_->pw.loop);
    unsigned long long pos = state_->sourcePos;
    bool exclusive = wantedMode_ == OutputMode::Exclusive;
    state_->failed = false;
    bool ok = OpenStream(*state_, exclusive, exclusive ? rate_.load() : kSharedRate, !paused_);
    state_->sourcePos = pos;
    pw_thread_loop_unlock(state_->pw.loop);
    std::lock_guard<std::mutex> lock(problemMx_);
    if (ok) problem_.clear();
    else problem_ = L"Waiting for PipeWire (no audio output yet).";
}

void AudioOutput::Stop() {
    if (!state_) return;
    quit_ = true;
    state_->retryCv.notify_all();
    if (thread_.joinable()) thread_.join();
    if (state_->pw.loop) {
        pw_thread_loop_lock(state_->pw.loop);
        CloseStream(*state_);
        pw_thread_loop_unlock(state_->pw.loop);
        pw_thread_loop_stop(state_->pw.loop);
        pw_thread_loop_destroy(state_->pw.loop);
    }
    state_.reset();
    ok_ = false;
}

void AudioOutput::wake() {
    // Pause, resume and flush act on the stream right away (the Windows build wakes its thread).
    if (!state_ || !state_->pw.stream) return;
    pw_thread_loop_lock(state_->pw.loop);
    if (flush_.exchange(false)) {
        pw_stream_flush(state_->pw.stream, false);
        state_->sourcePos = flushPos_;
        state_->deviceWritten = 0;
        state_->chunks.clear();
        played_ = flushPos_.load();
    }
    pw_stream_set_active(state_->pw.stream, !paused_);
    pw_thread_loop_unlock(state_->pw.loop);
}

void AudioOutput::SetDevice(const std::string& node) {
    {
        std::lock_guard<std::mutex> lock(problemMx_);
        if (device_ == node) return;
        device_ = node;
    }
    if (!state_) return;
    pw_thread_loop_lock(state_->pw.loop);
    unsigned long long pos = state_->sourcePos;
    state_->device = node;
    bool exclusive = wantedMode_ == OutputMode::Exclusive;
    OpenStream(*state_, exclusive, exclusive ? rate_.load() : kSharedRate, !paused_);
    state_->sourcePos = pos;
    pw_thread_loop_unlock(state_->pw.loop);
    mode_ = OutputMode::Shared;  // exclusive once PipeWire is seen running at the rate
}

void AudioOutput::SetMode(OutputMode mode) {
    wantedMode_ = mode;
    if (!state_) return;
    unsigned rate = mode == OutputMode::Exclusive ? rate_.load() : kSharedRate;
    pw_thread_loop_lock(state_->pw.loop);
    OpenStream(*state_, mode == OutputMode::Exclusive, rate, !paused_);
    pw_thread_loop_unlock(state_->pw.loop);
    mode_ = OutputMode::Shared;  // exclusive once PipeWire is seen running at the rate
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
    if (rate == rate_ && state_->pw.exclusive) return rate_;
    pw_thread_loop_lock(state_->pw.loop);
    unsigned long long pos = state_->sourcePos;
    bool ok = OpenStream(*state_, true, rate, !paused_);
    state_->sourcePos = pos;
    pw_thread_loop_unlock(state_->pw.loop);
    if (ok) rate_ = rate;
    mode_ = OutputMode::Shared;
    return rate_;
}

std::wstring AudioOutput::Problem() const {
    std::lock_guard<std::mutex> lock(problemMx_);
    return problem_;
}
