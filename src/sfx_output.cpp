// Sound effect output: a small shared-mode WASAPI stream of its own. It opens (with its thread)
// on the first sound and closes again after a few idle seconds, so effects cost nothing while
// nobody triggers them. Sounds are rendered on that thread at the device's mix rate.
#include "sfx.h"

#include "decoder.h"

#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <random>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace {

struct Request {
    Sfx sound;
    int arg;
    bool ducked;
    float gain;
    uint32_t seed;
};

// One sound currently playing. samples are mono (an effect) or, for the game clip, interleaved
// stereo; pos is in frames. The game clip is the only stereo voice, and there is at most one.
struct Voice {
    std::vector<float> samples;
    size_t pos = 0;  // frames
    float gain = 1;
    bool stereo = false;
};

struct Clip {
    std::vector<float> samples;  // interleaved stereo
    unsigned rate = 0;
    float gain = 1;
};

constexpr ULONGLONG kIdleMs = 4000;  // keeps the stream open across a burst of clicks
constexpr size_t kMaxVoices = 8;

// Shared between the UI thread and the output thread. g_mx guards all of it except g_clipFrames,
// which is atomic so ClipPosition can read it without the lock.
std::mutex g_mx;
std::vector<Request> g_pending;
std::unique_ptr<Clip> g_clip;   // waiting to start
bool g_stopClip = false;
std::atomic<long long> g_clipFrames{-1};  // frames of the playing clip already played
unsigned g_clipRate = 0;
std::thread g_thread;
HANDLE g_wake = nullptr;
bool g_running = false, g_quit = false, g_suppressed = false;

struct Stream {
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    UINT32 bufferFrames = 0;
    unsigned rate = 48000;
    bool started = false;

    bool Open(HANDLE event) {
        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice> device;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) ||
            FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) ||
            FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client)))
            return Close();
        WAVEFORMATEX* mix = nullptr;
        if (FAILED(client->GetMixFormat(&mix))) return Close();
        rate = mix->nSamplesPerSec;
        CoTaskMemFree(mix);
        WAVEFORMATEXTENSIBLE fmt{};
        fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        fmt.Format.nChannels = 2;
        fmt.Format.nSamplesPerSec = rate;
        fmt.Format.wBitsPerSample = 32;
        fmt.Format.nBlockAlign = 8;
        fmt.Format.nAvgBytesPerSec = rate * 8;
        fmt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        fmt.Samples.wValidBitsPerSample = 32;
        fmt.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        fmt.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                      AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                          AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                      300000 /* 30 ms: a click is heard right away */, 0, &fmt.Format, nullptr)) ||
            FAILED(client->SetEventHandle(event)) || FAILED(client->GetBufferSize(&bufferFrames)) ||
            FAILED(client->GetService(IID_PPV_ARGS(&render))))
            return Close();
        started = false;
        return true;
    }

    bool Close() {
        if (client && started) client->Stop();
        render.Reset();
        client.Reset();
        started = false;
        return false;
    }
    bool IsOpen() const { return client != nullptr; }

    // Mixes the voices into whatever room the device buffer has. False if the device went away.
    bool Fill(std::vector<Voice>& voices) {
        UINT32 padding = 0;
        BYTE* data = nullptr;
        if (FAILED(client->GetCurrentPadding(&padding))) return false;
        UINT32 frames = bufferFrames - padding;
        if (frames && SUCCEEDED(render->GetBuffer(frames, &data))) {
            auto* out = reinterpret_cast<float*>(data);
            std::fill(out, out + frames * 2, 0.0f);
            for (Voice& v : voices) {
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
            for (UINT32 i = 0; i < frames * 2; ++i) out[i] = std::clamp(out[i], -1.0f, 1.0f);
            render->ReleaseBuffer(frames, 0);
        }
        std::erase_if(voices, [](const Voice& v) {
            bool done = v.pos >= (v.stereo ? v.samples.size() / 2 : v.samples.size());
            if (done && v.stereo) g_clipFrames = -1;
            return done;
        });
        if (!started) started = SUCCEEDED(client->Start());
        return true;
    }
};

// The stream's own thread: mix pending requests and the current clip into the device buffer, then
// sleep until woken (a new sound) or until the device needs more. It exits after kIdleMs of
// silence, which releases the device.
void Run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Stream stream;
    std::vector<Voice> voices;
    ULONGLONG lastSound = GetTickCount64();
    for (;;) {
        std::vector<Request> requests;
        std::unique_ptr<Clip> clip;
        bool stopClip = false;
        {
            std::lock_guard lock(g_mx);
            if (g_quit) break;
            requests.swap(g_pending);
            clip = std::move(g_clip);
            stopClip = std::exchange(g_stopClip, false);
            if (requests.empty() && !clip && voices.empty() && GetTickCount64() - lastSound >= kIdleMs) {
                g_running = false;
                break;
            }
        }
        if (stopClip || clip) {
            std::erase_if(voices, [](const Voice& v) { return v.stereo; });
            g_clipFrames = -1;
        }
        if (clip && (stream.IsOpen() || stream.Open(event))) {
            lastSound = GetTickCount64();
            Voice v{{}, 0, clip->gain, true};
            if (clip->rate == stream.rate) {
                v.samples = std::move(clip->samples);
            } else {
                Resampler rs;
                rs.Init(clip->rate, stream.rate);
                rs.Push(clip->samples.data(), clip->samples.size() / 2, v.samples);
                rs.Flush(v.samples);
            }
            // 8 ms fades: a clip cut from the middle of a song must not click.
            size_t frames = v.samples.size() / 2, fade = std::min<size_t>(stream.rate / 125, frames / 2);
            for (size_t i = 0; i < fade; ++i) {
                float k = (float)i / (float)fade;
                for (int c = 0; c < 2; ++c) {
                    v.samples[i * 2 + c] *= k;
                    v.samples[(frames - 1 - i) * 2 + c] *= k;
                }
            }
            g_clipRate = stream.rate;
            g_clipFrames = 0;
            voices.push_back(std::move(v));
        }
        if (!requests.empty()) {
            lastSound = GetTickCount64();
            if (!stream.IsOpen() && !stream.Open(event)) requests.clear();  // device busy or gone: no sound
            for (const Request& r : requests) {
                voices.push_back({sfx::Render(r.sound, r.arg, stream.rate, r.seed, r.ducked), 0, r.gain});
                if (voices.size() > kMaxVoices) {
                    auto effect = std::find_if(voices.begin(), voices.end(), [](const Voice& v) { return !v.stereo; });
                    if (effect != voices.end()) voices.erase(effect);
                }
            }
        }
        if (stream.IsOpen() && !stream.Fill(voices)) {
            stream.Close();
            voices.clear();
        }
        if (!voices.empty()) lastSound = GetTickCount64();
        HANDLE handles[] = {g_wake, event};
        WaitForMultipleObjects(stream.IsOpen() ? 2 : 1, handles, FALSE, stream.IsOpen() ? 100 : (DWORD)kIdleMs);
    }
    stream.Close();
    CloseHandle(event);
    CoUninitialize();
}

// Stops the thread (releasing the device) and waits for it.
void StopThread() {
    std::thread thread;
    {
        std::lock_guard lock(g_mx);
        g_pending.clear();
        g_quit = true;
        thread = std::move(g_thread);
    }
    if (g_wake) SetEvent(g_wake);
    if (thread.joinable()) thread.join();
    std::lock_guard lock(g_mx);
    g_quit = false;
    g_running = false;
}

// Starts the stream thread if it is idle and wakes it. Call with g_mx held.
void EnsureRunning() {
    if (!g_wake) g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_running) {
        if (g_thread.joinable()) g_thread.join();
        g_thread = std::thread(Run);
        g_running = true;
    }
    SetEvent(g_wake);
}

}  // namespace

namespace sfx {

void Play(Sfx sound, int arg, bool ducked, float gain) {
    if (gain <= 0) return;
    std::lock_guard lock(g_mx);
    if (g_suppressed) return;
    static std::mt19937 seeds{std::random_device{}()};
    g_pending.push_back({sound, arg, ducked, gain, seeds()});
    EnsureRunning();  // a stream that just went idle has already let go
}

void PlayClip(std::vector<float> stereo, unsigned rate, float gain) {
    if (stereo.size() < 4 || !rate) return;
    std::lock_guard lock(g_mx);
    if (g_suppressed) return;
    g_clip = std::make_unique<Clip>(Clip{std::move(stereo), rate, gain});
    g_clipFrames = 0;  // reports "playing" from now on, before the thread picks it up
    EnsureRunning();
}

void StopClip() {
    std::lock_guard lock(g_mx);
    g_clip.reset();
    g_stopClip = true;
    g_clipFrames = -1;
    if (g_running) SetEvent(g_wake);
}

double ClipPosition() {
    long long frames = g_clipFrames;
    return frames < 0 ? -1.0 : g_clipRate ? (double)frames / g_clipRate : 0.0;
}

void SetSuppressed(bool suppressed) {
    {
        std::lock_guard lock(g_mx);
        g_suppressed = suppressed;
    }
    if (suppressed) StopThread();
}

void Shutdown() {
    SetSuppressed(true);
}

}  // namespace sfx
