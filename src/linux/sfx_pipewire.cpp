// Sound effect output (sfx.h) on PipeWire, for the Linux build: a small stream of its own next to the
// music's, as sfx_output.cpp is on Windows. It opens on the first sound and is let go again a few idle
// seconds later, so effects cost nothing while nobody triggers them. A worker thread renders the sounds
// (and resamples a game clip) and the stream's process callback only mixes what is ready.
#include "sfx.h"

#include "decoder.h"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <random>
#include <utility>
#include <thread>

namespace {

constexpr unsigned kRate = 48000;  // PipeWire resamples to the device
constexpr auto kIdle = std::chrono::seconds(4);  // keeps the stream open across a burst of clicks
constexpr size_t kMaxVoices = 8;

struct Request {
    Sfx sound;
    int arg;
    bool ducked;
    float gain;
    uint32_t seed;
};

// One sound playing. Samples are mono (an effect) or interleaved stereo (the game clip, of which there
// is at most one); pos is in frames.
struct Voice {
    std::vector<float> samples;
    size_t pos = 0;
    float gain = 1;
    bool stereo = false;
};

struct Clip {
    std::vector<float> samples;  // interleaved stereo
    unsigned rate = 0;
    float gain = 1;
};

// g_mx guards all of this except g_clipFrames, which ClipPosition reads without the lock.
std::mutex g_mx;
std::condition_variable g_cv;
std::vector<Request> g_pending;
std::unique_ptr<Clip> g_clip;  // waiting to start
std::vector<Voice> g_voices;   // mixed by the stream's process callback
bool g_stopClip = false, g_running = false, g_quit = false, g_suppressed = false;
std::atomic<long long> g_clipFrames{-1};  // frames of the playing clip already played
std::chrono::steady_clock::time_point g_lastSound;
std::thread g_thread;
std::once_flag g_pwInit;

struct Stream {
    pw_thread_loop* loop = nullptr;
    pw_stream* stream = nullptr;
    spa_hook listener{};
};

void OnProcess(void* data) {
    auto* st = static_cast<Stream*>(data);
    pw_buffer* b = pw_stream_dequeue_buffer(st->stream);
    if (!b) return;
    spa_buffer* buf = b->buffer;
    auto* out = static_cast<float*>(buf->datas[0].data);
    if (!out) {
        pw_stream_queue_buffer(st->stream, b);
        return;
    }
    const uint32_t stride = sizeof(float) * 2;
    uint32_t frames = buf->datas[0].maxsize / stride;
    if (b->requested) frames = std::min<uint32_t>(frames, (uint32_t)b->requested);
    std::fill(out, out + frames * 2, 0.0f);
    // try_lock: this thread must never wait on a caller that waits on this loop (silence for a period instead).
    if (std::unique_lock lock(g_mx, std::try_to_lock); lock.owns_lock()) {
        for (Voice& v : g_voices) {
            size_t total = v.stereo ? v.samples.size() / 2 : v.samples.size();
            size_t n = std::min<size_t>(frames, total - v.pos);
            if (v.stereo) {
                const float* in = v.samples.data() + v.pos * 2;
                for (size_t i = 0; i < n * 2; ++i) out[i] += in[i] * v.gain;
                g_clipFrames = (long long)(v.pos + n);
            } else {
                for (size_t i = 0; i < n; ++i) {
                    float s = v.samples[v.pos + i] * v.gain;
                    out[i * 2] += s;
                    out[i * 2 + 1] += s;
                }
            }
            v.pos += n;
        }
        std::erase_if(g_voices, [](const Voice& v) {
            bool done = v.pos >= (v.stereo ? v.samples.size() / 2 : v.samples.size());
            if (done && v.stereo) g_clipFrames = -1;
            return done;
        });
        if (!g_voices.empty()) g_lastSound = std::chrono::steady_clock::now();
    }
    for (uint32_t i = 0; i < frames * 2; ++i) out[i] = std::clamp(out[i], -1.0f, 1.0f);
    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = stride;
    buf->datas[0].chunk->size = frames * stride;
    pw_stream_queue_buffer(st->stream, b);
}

const pw_stream_events kEvents = [] {
    pw_stream_events e{};
    e.version = PW_VERSION_STREAM_EVENTS;
    e.process = OnProcess;
    return e;
}();

bool OpenStream(Stream& st) {
    std::call_once(g_pwInit, [] { pw_init(nullptr, nullptr); });
    st.loop = pw_thread_loop_new("cordinal-effects", nullptr);
    if (!st.loop || pw_thread_loop_start(st.loop) < 0) return false;
    char latency[32];
    snprintf(latency, sizeof(latency), "%u/%u", kRate / 33, kRate);  // 30 ms: a click is heard right away
    pw_thread_loop_lock(st.loop);
    pw_properties* props =
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE, "Notification",
                          PW_KEY_APP_NAME, "cordinal", PW_KEY_NODE_NAME, "cordinal-effects", PW_KEY_NODE_LATENCY, latency, nullptr);
    st.stream = pw_stream_new_simple(pw_thread_loop_get_loop(st.loop), "cordinal-effects", props, &kEvents, &st);
    bool ok = st.stream != nullptr;
    if (ok) {
        uint8_t buffer[1024];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        spa_audio_info_raw info{};
        info.format = SPA_AUDIO_FORMAT_F32;
        info.rate = kRate;
        info.channels = 2;
        info.position[0] = SPA_AUDIO_CHANNEL_FL;
        info.position[1] = SPA_AUDIO_CHANNEL_FR;
        const spa_pod* params[1] = {spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info)};
        ok = pw_stream_connect(st.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                               (pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), params, 1) >= 0;
    }
    pw_thread_loop_unlock(st.loop);
    return ok;
}

void CloseStream(Stream& st) {
    if (!st.loop) return;
    pw_thread_loop_lock(st.loop);
    if (st.stream) pw_stream_destroy(st.stream);
    st.stream = nullptr;
    pw_thread_loop_unlock(st.loop);
    pw_thread_loop_stop(st.loop);
    pw_thread_loop_destroy(st.loop);
    st.loop = nullptr;
}

// The worker: opens the stream for the first sound, renders requests and the clip into voices, and lets
// the stream go after kIdle of silence.
void Run() {
    Stream stream;
    std::unique_lock lock(g_mx);
    g_lastSound = std::chrono::steady_clock::now();
    for (;;) {
        if (g_quit) break;
        std::vector<Request> requests;
        requests.swap(g_pending);
        std::unique_ptr<Clip> clip = std::move(g_clip);
        const bool stop = std::exchange(g_stopClip, false);
        const auto now = std::chrono::steady_clock::now();
        if (requests.empty() && !clip && g_voices.empty() && now - g_lastSound >= kIdle) break;
        if (stop || clip) {
            std::erase_if(g_voices, [](const Voice& v) { return v.stereo; });
            g_clipFrames = -1;
        }
        std::vector<Voice> added;
        if (!requests.empty() || clip) {
            lock.unlock();
            const bool open = stream.stream || OpenStream(stream);  // no device: no sound
            if (open) {
                if (clip) {
                    Voice v{{}, 0, clip->gain, true};
                    if (clip->rate == kRate) {
                        v.samples = std::move(clip->samples);
                    } else {
                        Resampler rs;
                        rs.Init(clip->rate, kRate);
                        rs.Push(clip->samples.data(), clip->samples.size() / 2, v.samples);
                        rs.Flush(v.samples);
                    }
                    // 8 ms fades: a clip cut from the middle of a song must not click.
                    size_t frames = v.samples.size() / 2, fade = std::min<size_t>(kRate / 125, frames / 2);
                    for (size_t i = 0; i < fade; ++i) {
                        float k = (float)i / (float)fade;
                        for (int c = 0; c < 2; ++c) {
                            v.samples[i * 2 + c] *= k;
                            v.samples[(frames - 1 - i) * 2 + c] *= k;
                        }
                    }
                    added.push_back(std::move(v));
                }
                for (const Request& r : requests) added.push_back({sfx::Render(r.sound, r.arg, kRate, r.seed, r.ducked), 0, r.gain});
            }
            lock.lock();
            if (g_suppressed) added.clear();
            for (Voice& v : added) {
                if (v.stereo) g_clipFrames = 0;
                g_voices.push_back(std::move(v));
                if (g_voices.size() > kMaxVoices) {
                    auto effect = std::find_if(g_voices.begin(), g_voices.end(), [](const Voice& x) { return !x.stereo; });
                    if (effect != g_voices.end()) g_voices.erase(effect);
                }
            }
            if (!added.empty()) g_lastSound = std::chrono::steady_clock::now();
        }
        g_cv.wait_for(lock, std::chrono::milliseconds(100));
    }
    g_voices.clear();
    g_clipFrames = -1;
    g_running = false;
    lock.unlock();
    CloseStream(stream);
}

// Starts the worker if it is idle and wakes it. Called without g_mx held.
void EnsureRunning() {
    std::thread old;
    {
        std::lock_guard lock(g_mx);
        if (!g_running) {
            old = std::move(g_thread);
            g_running = true;
        }
    }
    if (old.joinable()) old.join();  // a worker that has just let the device go
    std::lock_guard lock(g_mx);
    if (g_running && !g_thread.joinable()) g_thread = std::thread(Run);
    g_cv.notify_all();
}

void StopThread() {
    std::thread thread;
    {
        std::lock_guard lock(g_mx);
        g_pending.clear();
        g_clip.reset();
        g_quit = true;
        thread = std::move(g_thread);
    }
    g_cv.notify_all();
    if (thread.joinable()) thread.join();
    std::lock_guard lock(g_mx);
    g_quit = false;
    g_running = false;
}

}  // namespace

namespace sfx {

void Play(Sfx sound, int arg, bool ducked, float gain) {
    if (gain <= 0) return;
    {
        std::lock_guard lock(g_mx);
        if (g_suppressed) return;
        static std::mt19937 seeds{std::random_device{}()};
        g_pending.push_back({sound, arg, ducked, gain, seeds()});
    }
    EnsureRunning();
}

void PlayClip(std::vector<float> stereo, unsigned rate, float gain) {
    if (stereo.size() < 4 || !rate) return;
    {
        std::lock_guard lock(g_mx);
        if (g_suppressed) return;
        g_clip = std::make_unique<Clip>(Clip{std::move(stereo), rate, gain});
        g_clipFrames = 0;  // reports "playing" from now on, before the worker picks it up
    }
    EnsureRunning();
}

void StopClip() {
    std::lock_guard lock(g_mx);
    g_clip.reset();
    g_stopClip = true;
    g_clipFrames = -1;
    g_cv.notify_all();
}

double ClipPosition() {
    long long frames = g_clipFrames;
    return frames < 0 ? -1.0 : (double)frames / kRate;
}

void SetSuppressed(bool suppressed) {
    {
        std::lock_guard lock(g_mx);
        g_suppressed = suppressed;
    }
    if (suppressed) StopThread();
}

void Shutdown() { SetSuppressed(true); }

}  // namespace sfx
