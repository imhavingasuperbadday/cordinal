// WASAPI device handling and float-to-device-format conversion (see audio_output.h). The render
// thread opens the default device, re-opens it when it is lost or the default changes, and keeps a
// chunk table mapping device frames already played back to source positions for PlayedPosition().
#include "audio_output.h"

#ifdef _WIN32
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#endif

#include <algorithm>
#include <cmath>
#include <deque>
#include <emmintrin.h>
#include <string>
#include <vector>

namespace {
__forceinline __m128i FiniteMask(__m128 value) {
    return _mm_cmplt_epi32(_mm_and_si128(_mm_castps_si128(value), _mm_set1_epi32(0x7FFFFFFF)),
                          _mm_set1_epi32(0x7F800000));
}

// Power-of-two scaling is exact for float input. Mask non-finite input before
// arithmetic, and saturate before conversion so no lane overflows the integer.
template<unsigned Bits> __forceinline __m128i Quantize(const float* in) {
    __m128 v = _mm_loadu_ps(in);
    const __m128i finite = _mm_cmplt_epi32(_mm_and_si128(_mm_castps_si128(v), _mm_set1_epi32(0x7FFFFFFF)),
                                         _mm_set1_epi32(0x7F800000));
    v = _mm_and_ps(v, _mm_castsi128_ps(finite));
    v = _mm_max_ps(_mm_set1_ps(-1), _mm_min_ps(_mm_set1_ps(1), v));
    v = _mm_mul_ps(v, _mm_set1_ps((float)(uint64_t(1) << (Bits - 1))));
    if constexpr (Bits < 32)
        return _mm_cvtps_epi32(_mm_min_ps(v, _mm_set1_ps((float)((uint32_t(1) << (Bits - 1)) - 1))));
    else {
        const __m128i high = _mm_castps_si128(_mm_cmpeq_ps(v, _mm_set1_ps(2147483648.0f)));
        // INT32_MAX rounds up as a float. Convert its preceding float safely,
        // then fill its seven low bits only for positively saturated lanes.
        const __m128i value = _mm_cvtps_epi32(_mm_min_ps(v, _mm_set1_ps(2147483520.0f)));
        return _mm_or_si128(value, _mm_and_si128(high, _mm_set1_epi32(127)));
    }
}

// Keep the dither state and its double-precision work outside the other formats' loops.
__declspec(noinline) void Convert16(const float* in, size_t samples, int16_t* o) {
    // ReplayGain, EQ and volume leave samples between 16-bit steps; rounding those plainly
    // turns the error into distortion that follows the music (audible in quiet passages
    // and fades). TPDF dither of +/-1 step makes it a constant, signal-free noise floor
    // instead. A block that is all 16-bit values already (an untouched 16-bit source,
    // digital silence) is converted exactly, so the bit-perfect path stays bit-perfect.
    bool exact = true;
    size_t checked = 0;
    for (; checked + 2 <= samples; checked += 2) {
        __m128 v = _mm_castsi128_ps(_mm_loadl_epi64((const __m128i*)(in + checked)));
        v = _mm_and_ps(v, _mm_castsi128_ps(FiniteMask(v)));
        // At |input| >= 256, every float is already an integer after
        // scaling by 32768. Bound those lanes before integer conversion.
        // Use double as the scalar test does: float scaling could flush
        // a small fractional value to zero when FTZ is enabled.
        const __m128d steps = _mm_mul_pd(_mm_max_pd(_mm_set1_pd(-256),
            _mm_min_pd(_mm_set1_pd(256), _mm_cvtps_pd(v))), _mm_set1_pd(32768));
        if (_mm_movemask_pd(_mm_cmpeq_pd(steps, _mm_cvtepi32_pd(_mm_cvtpd_epi32(steps)))) != 3) {
            exact = false;
            break;
        }
    }
    for (; checked < samples && exact; ++checked)
        exact = !std::isfinite(in[checked]) || in[checked] * 32768.0 == std::nearbyint(in[checked] * 32768.0);
    size_t i = 0;
    if (exact) {
        for (; i + 8 <= samples; i += 8)
            _mm_storeu_si128((__m128i*)(o + i), _mm_packs_epi32(Quantize<16>(in + i), Quantize<16>(in + i + 4)));
        for (; i < samples; ++i) o[i] = (int16_t)(std::isfinite(in[i]) ?
            (int64_t)std::clamp(std::nearbyint(in[i] * 32768.0), -32768.0, 32767.0) : 0);
        return;
    }
    static thread_local uint32_t noise = 0x9E3779B9u;
    for (; i + 2 <= samples; i += 2) {
        int32_t error[2];
        for (unsigned j = 0; j < 2; ++j) {
            noise = noise * 1664525u + 1013904223u; const int32_t first = (int32_t)(noise >> 8);
            noise = noise * 1664525u + 1013904223u; error[j] = first - (int32_t)(noise >> 8);
        }
        // The two uniforms are integer multiples of 2^-24, so subtracting
        // before conversion is exact and keeps the original noise sequence.
        const __m128d dither = _mm_mul_pd(_mm_cvtepi32_pd(_mm_setr_epi32(error[0], error[1], 0, 0)),
                                         _mm_set1_pd(1.0 / 16777216.0));
        const __m128 v = _mm_castsi128_ps(_mm_loadl_epi64((const __m128i*)(in + i)));
        const __m128i finite = FiniteMask(v);
        const __m128d value = _mm_add_pd(_mm_mul_pd(_mm_cvtps_pd(_mm_and_ps(v, _mm_castsi128_ps(finite))),
                                                   _mm_set1_pd(32768)), dither);
        const __m128i rounded = _mm_and_si128(finite, _mm_cvtpd_epi32(
            _mm_min_pd(_mm_set1_pd(32767), _mm_max_pd(_mm_set1_pd(-32768), value))));
        const int pair = _mm_cvtsi128_si32(_mm_packs_epi32(rounded, rounded));
        memcpy(o + i, &pair, sizeof pair);
    }
    for (; i < samples; ++i) {
        noise = noise * 1664525u + 1013904223u;
        double dither = (noise >> 8) * (1.0 / 16777216.0);
        noise = noise * 1664525u + 1013904223u;
        dither -= (noise >> 8) * (1.0 / 16777216.0);
        o[i] = (int16_t)(std::isfinite(in[i]) ? (long long)std::clamp(std::nearbyint(in[i] * 32768.0 + dither), -32768.0, 32767.0) : 0LL);
    }
}

}


int SampleBytes(SampleFormat fmt) {
    switch (fmt) {
        case SampleFormat::Int16: return 2;
        case SampleFormat::Int24: return 3;
        default: return 4;
    }
}

int SampleBits(SampleFormat fmt) {
    switch (fmt) {
        case SampleFormat::Int16: return 16;
        case SampleFormat::Int24:
        case SampleFormat::Int24In32: return 24;
        default: return 32;
    }
}

void ConvertSamples(const float* in, size_t samples, SampleFormat fmt, void* out) {
    // Scaling by a power of two and rounding is exact for samples that were integers of the
    // target width to begin with (what the decoders produce from 16/24-bit sources).
    auto scaled = [](float x, double scale, double lo, double hi) {
        // A decoder can emit inf/NaN from a damaged stream; casting that to an integer is UB.
        return std::isfinite(x) ? (long long)std::clamp(std::nearbyint(x * scale), lo, hi) : 0LL;
    };
    switch (fmt) {
        case SampleFormat::Float32:
            memcpy(out, in, samples * sizeof(float));
            break;
        case SampleFormat::Int16:
            Convert16(in, samples, static_cast<int16_t*>(out));
            break;
        case SampleFormat::Int24In32: {
            auto* o = static_cast<int32_t*>(out);
            size_t i = 0;
            for (; i + 4 <= samples; i += 4)
                _mm_storeu_si128((__m128i*)(o + i), _mm_slli_epi32(Quantize<24>(in + i), 8));
            for (; i < samples; ++i) o[i] = (int32_t)(scaled(in[i], 8388608.0, -8388608.0, 8388607.0) * 256);
            break;
        }
        case SampleFormat::Int32: {
            auto* o = static_cast<int32_t*>(out);
            size_t i = 0;
            for (; i + 4 <= samples; i += 4) _mm_storeu_si128((__m128i*)(o + i), Quantize<32>(in + i));
            for (; i < samples; ++i) o[i] = (int32_t)scaled(in[i], 2147483648.0, -2147483648.0, 2147483647.0);
            break;
        }
        case SampleFormat::Int24: {
            auto* o = static_cast<uint8_t*>(out);
            size_t i = 0;
            for (; i + 4 <= samples; i += 4) {
                int32_t values[4];
                _mm_storeu_si128((__m128i*)values, Quantize<24>(in + i));
                for (unsigned j = 0; j < 4; ++j) {
                    const uint32_t v = (uint32_t)values[j];
                    o[(i + j) * 3] = (uint8_t)v;
                    o[(i + j) * 3 + 1] = (uint8_t)(v >> 8);
                    o[(i + j) * 3 + 2] = (uint8_t)(v >> 16);
                }
            }
            for (; i < samples; ++i) {
                int32_t v = (int32_t)scaled(in[i], 8388608.0, -8388608.0, 8388607.0);
                o[i * 3] = (uint8_t)v;
                o[i * 3 + 1] = (uint8_t)(v >> 8);
                o[i * 3 + 2] = (uint8_t)(v >> 16);
            }
            break;
        }
    }
}

// The WASAPI output; the Linux build has its own (linux/audio_pipewire.cpp).
#ifdef _WIN32
using Microsoft::WRL::ComPtr;

namespace {

// Tells the render thread when the default output device changes, so it can re-open. Reference
// counted the COM way; Run() holds the one reference and releases it when the thread ends.
class DeviceNotifier final : public IMMNotificationClient {
public:
    explicit DeviceNotifier(AudioOutput* out) : out_(out) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = (ULONG)InterlockedDecrement(&ref_);
        if (!r) delete this;
        return r;
    }
    STDMETHODIMP OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole) out_->DefaultDeviceChanged();
        return S_OK;
    }
    STDMETHODIMP OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    STDMETHODIMP OnDeviceAdded(LPCWSTR) override { return S_OK; }
    STDMETHODIMP OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    STDMETHODIMP OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    AudioOutput* out_;
    LONG ref_ = 1;
};

struct Device {
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    UINT32 bufferFrames = 0;
    unsigned rate = 0;
    bool exclusive = false;
    SampleFormat format = SampleFormat::Float32;
};

// An extensible 2-channel format at `rate` for the chosen sample type, on the front L/R mask.
WAVEFORMATEXTENSIBLE MakeFormat(unsigned rate, SampleFormat f) {
    WAVEFORMATEXTENSIBLE fmt{};
    int bytes = SampleBytes(f);
    fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    fmt.Format.nChannels = 2;
    fmt.Format.nSamplesPerSec = rate;
    fmt.Format.wBitsPerSample = (WORD)(bytes * 8);
    fmt.Format.nBlockAlign = (WORD)(bytes * 2);
    fmt.Format.nAvgBytesPerSec = rate * bytes * 2;
    fmt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    fmt.Samples.wValidBitsPerSample = (WORD)SampleBits(f);
    fmt.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    fmt.SubFormat = f == SampleFormat::Float32 ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
    return fmt;
}

HRESULT FinishOpen(HANDLE event, Device& d) {
    d.client->SetEventHandle(event);
    d.client->GetBufferSize(&d.bufferFrames);
    return d.client->GetService(IID_PPV_ARGS(&d.render));
}

// Shared mode runs at the device mix rate in 32-bit float; AUTOCONVERTPCM lets WASAPI resample
// when the format isn't natively accepted.
HRESULT OpenShared(IMMDevice* device, HANDLE event, Device& d) {
    HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &d.client);
    if (FAILED(hr)) return hr;
    WAVEFORMATEX* mix = nullptr;
    hr = d.client->GetMixFormat(&mix);
    if (FAILED(hr)) return hr;
    d.rate = mix->nSamplesPerSec;
    CoTaskMemFree(mix);
    WAVEFORMATEXTENSIBLE fmt = MakeFormat(d.rate, SampleFormat::Float32);
    hr = d.client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                              AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                  AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                              1000000 /* 100 ms */, 0, &fmt.Format, nullptr);
    if (FAILED(hr)) return hr;
    d.exclusive = false;
    d.format = SampleFormat::Float32;
    return FinishOpen(event, d);
}

// Exclusive, event driven, at the device's default period (re-done with an aligned period when
// the driver asks for one).
HRESULT OpenExclusiveFormat(IMMDevice* device, HANDLE event, unsigned rate, SampleFormat f, Device& d) {
    HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &d.client);
    if (FAILED(hr)) return hr;
    WAVEFORMATEXTENSIBLE fmt = MakeFormat(rate, f);
    hr = d.client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &fmt.Format, nullptr);
    if (hr != S_OK) return FAILED(hr) ? hr : AUDCLNT_E_UNSUPPORTED_FORMAT;
    REFERENCE_TIME period = 0, minimum = 0;
    d.client->GetDevicePeriod(&period, &minimum);
    hr = d.client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, period, &fmt.Format, nullptr);
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        UINT32 frames = 0;
        d.client->GetBufferSize(&frames);
        period = (REFERENCE_TIME)(10000000.0 * frames / rate + 0.5);
        d.client.Reset();
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &d.client);
        if (FAILED(hr)) return hr;
        hr = d.client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, period, &fmt.Format, nullptr);
    }
    if (FAILED(hr)) return hr;
    d.rate = rate;
    d.exclusive = true;
    d.format = f;
    return FinishOpen(event, d);
}

std::wstring HrText(HRESULT hr) {
    if (hr == AUDCLNT_E_DEVICE_IN_USE) return L"another program is using the device exclusively";
    if (hr == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED)
        return L"exclusive mode is turned off for this device (Sound settings > device > Advanced)";
    if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT) return L"the device doesn't support that format";
    wchar_t buf[48];
    swprintf_s(buf, L"error 0x%08lX", (unsigned long)hr);
    return buf;
}

// Opens the default device: shared, or exclusive at `rate` (else at another rate it supports,
// else shared). `problem` says why exclusive output isn't exactly what was asked for.
HRESULT OpenDevice(IMMDeviceEnumerator* enumerator, HANDLE event, OutputMode mode, unsigned rate, Device& d, std::wstring& problem) {
    d = Device{};
    problem.clear();
    ComPtr<IMMDevice> device;
    HRESULT hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr)) return hr;
    if (mode == OutputMode::Exclusive) {
        ComPtr<IAudioClient> probe;
        unsigned mixRate = 48000;
        if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &probe))) {
            WAVEFORMATEX* mix = nullptr;
            if (SUCCEEDED(probe->GetMixFormat(&mix))) {
                mixRate = mix->nSamplesPerSec;
                CoTaskMemFree(mix);
            }
        }
        probe.Reset();
        HRESULT last = S_OK;
        std::vector<unsigned> rates{rate ? rate : mixRate};
        for (unsigned r : {mixRate, 48000u, 44100u, 96000u})
            if (std::find(rates.begin(), rates.end(), r) == rates.end()) rates.push_back(r);
        for (unsigned r : rates) {
            for (SampleFormat f : {SampleFormat::Float32, SampleFormat::Int32, SampleFormat::Int24In32, SampleFormat::Int24,
                                   SampleFormat::Int16}) {
                Device attempt;
                last = OpenExclusiveFormat(device.Get(), event, r, f, attempt);
                if (SUCCEEDED(last)) {
                    d = attempt;
                    if (rate && r != rate) problem = L"the device can't play " + std::to_wstring(rate) + L" Hz exclusively";
                    return S_OK;
                }
                if (last == AUDCLNT_E_DEVICE_IN_USE || last == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED) break;
            }
            if (last == AUDCLNT_E_DEVICE_IN_USE || last == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED) break;
        }
        problem = L"Exclusive output isn't available (" + HrText(last) + L"); using shared output.";
    }
    return OpenShared(device.Get(), event, d);
}

}  // namespace

bool AudioOutput::Start(RenderFn render, ResetFn reset, std::wstring& error) {
    render_ = std::move(render);
    reset_ = std::move(reset);
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    rateDone_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    quit_ = false;
    thread_ = std::thread([this] { Run(); });
    WaitForSingleObject(ready_, 5000);
    error = error_;
    return ok_;
}

void AudioOutput::Stop() {
    if (thread_.joinable()) {
        quit_ = true;
        wake();
        thread_.join();
    }
    for (HANDLE* h : {&event_, &ready_, &rateDone_})
        if (*h) {
            CloseHandle(*h);
            *h = nullptr;
        }
}

void AudioOutput::SetMode(OutputMode mode) {
    wantedMode_ = mode;
    if (mode == OutputMode::Shared) wantedRate_ = 0;
    if (thread_.joinable()) reopen_ = true;  // before Run() opens the device, the wanted mode is enough
    wake();
}

unsigned AudioOutput::RequestRate(unsigned rate) {
    if (wantedMode_ != OutputMode::Exclusive || !thread_.joinable()) return rate_;
    if (rate == rate_ && mode_ == OutputMode::Exclusive) return rate_;
    wantedRate_ = rate;
    ResetEvent(rateDone_);
    rateRequest_ = true;
    reopen_ = true;
    wake();
    WaitForSingleObject(rateDone_, 4000);
    return rate_;
}

std::wstring AudioOutput::Problem() const {
    std::lock_guard<std::mutex> lock(problemMx_);
    return problem_;
}

// How long a paused output keeps the device before letting it go. Long enough that a pause/skip
// and resume does not pay for a re-open, short enough that switching to another program's audio
// (a browser tab, say) does not have to wait for the player to be closed.
constexpr ULONGLONG kIdleReleaseMs = 2000;

// Render thread. Each pass: honour a pending reopen/flush/pause, hand the device a buffer, then
// map the frames it has played back to a source position for PlayedPosition(). A lost device
// (default change, mode or rate change, unplugged) is re-opened in place.
void AudioOutput::Run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    Device dev;
    std::wstring problem;
    if (SUCCEEDED(hr)) hr = OpenDevice(enumerator.Get(), event_, wantedMode_, wantedRate_, dev, problem);
    if (FAILED(hr)) {
        wchar_t buf[96];
        swprintf_s(buf, L"No audio output device (0x%08lX).", (unsigned long)hr);
        error_ = buf;
        SetEvent(ready_);
        CoUninitialize();
        return;
    }
    auto applyDevice = [&] {
        mode_ = dev.exclusive ? OutputMode::Exclusive : OutputMode::Shared;
        format_ = dev.format;
        holding_ = true;
        waiting_ = false;
        std::lock_guard<std::mutex> lock(problemMx_);
        problem_ = problem;
    };
    applyDevice();
    auto* notifier = new DeviceNotifier(this);
    enumerator->RegisterEndpointNotificationCallback(notifier);
    rate_ = dev.rate;
    ok_ = true;
    SetEvent(ready_);

    // Map device frames back to source positions: one entry per write.
    struct Chunk {
        unsigned long long deviceEnd;
        unsigned long long sourceEnd;
        size_t audio, total;
    };
    std::vector<Chunk> chunks;
    chunks.reserve(16);
    size_t chunkHead = 0;
    std::vector<float> convert;  // render buffer when the device doesn't take float
    unsigned long long deviceWritten = 0, sourcePos = 0;
    bool running = false;
    ULONGLONG idleSince = 0;  // when a paused output started sitting on the device

    while (!quit_) {
        bool lost = reopen_.exchange(false);
        // A paused output gives the endpoint back instead of sitting on it: an exclusive stream
        // keeps the device claimed for as long as its IAudioClient lives, even stopped, so a
        // paused player blocks every other program (a browser's audio renderer then fails with
        // AUDCLNT_E_DEVICE_IN_USE). Shared mode lets go too, so a laptop or Bluetooth endpoint can
        // really idle. RequestRate is waiting on the device, so that keeps it open.
        bool wantDevice = !paused_ || rateRequest_;
        if (dev.client && paused_ && running) {
            dev.client->Stop();
            running = false;
        }
        if (dev.client && paused_ && !wantDevice) {
            ULONGLONG now = GetTickCount64();
            if (!idleSince) idleSince = now;
            else if (now - idleSince >= kIdleReleaseMs) {
                dev = Device{};  // the claim goes with the client
                holding_ = false;
            }
        } else {
            idleSince = 0;
        }
        if (!lost && wantDevice && !dev.client) lost = true;  // resume after letting go
        if (!lost && dev.client) {
            if (flush_.exchange(false)) {
                if (running) dev.client->Stop();
                dev.client->Reset();
                running = false;
                deviceWritten = 0;
                chunks.clear();
                chunkHead = 0;
                sourcePos = flushPos_;
                played_ = sourcePos;
            }
            if (!paused_) {
                UINT32 padding = 0;
                hr = dev.client->GetCurrentPadding(&padding);
                if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
                    lost = true;
                } else if (SUCCEEDED(hr)) {
                    // Exclusive event-driven streams take a whole buffer per period; GetBuffer
                    // refuses while the device still holds the last one.
                    UINT32 avail = dev.exclusive ? dev.bufferFrames : dev.bufferFrames - padding;
                    BYTE* data = nullptr;
                    if (avail > 0 && SUCCEEDED(dev.render->GetBuffer(avail, &data))) {
                        size_t audio;
                        if (dev.format == SampleFormat::Float32) {
                            audio = render_(reinterpret_cast<float*>(data), avail, sourcePos);
                        } else {
                            convert.resize((size_t)avail * 2);
                            audio = render_(convert.data(), avail, sourcePos);
                            ConvertSamples(convert.data(), convert.size(), dev.format, data);
                        }
                        dev.render->ReleaseBuffer(avail, 0);
                        deviceWritten += avail;
                        if (chunkHead > 0 && chunkHead == chunks.size()) {
                            chunks.clear();
                            chunkHead = 0;
                        } else if (chunkHead >= 8) {
                            chunks.erase(chunks.begin(), chunks.begin() + (ptrdiff_t)chunkHead);
                            chunkHead = 0;
                        }
                        chunks.push_back({deviceWritten, sourcePos, audio, avail});
                    }
                    if (!running) {
                        dev.client->Start();
                        running = true;
                    }
                    // Played device frames -> played source position.
                    UINT32 queued = 0;
                    dev.client->GetCurrentPadding(&queued);
                    unsigned long long playedDevice = deviceWritten > queued ? deviceWritten - queued : 0;
                    while (chunks.size() - chunkHead > 1 && chunks[chunkHead].deviceEnd <= playedDevice) ++chunkHead;
                    unsigned long long playedSource = sourcePos;
                    for (size_t i = chunkHead; i < chunks.size(); ++i) {
                        const Chunk& c = chunks[i];
                        if (c.deviceEnd <= playedDevice) continue;
                        unsigned long long start = c.deviceEnd - c.total;
                        size_t into = playedDevice > start ? (size_t)(playedDevice - start) : 0;
                        playedSource = c.sourceEnd - c.audio + std::min(into, c.audio);
                        break;
                    }
                    played_ = playedSource;
                }
            }
        }
        if (lost) {
            if (running && dev.client) dev.client->Stop();
            running = false;
            dev = Device{};  // release the device first: exclusive mode can't be opened twice
            holding_ = false;
            Device fresh;
            bool requested = rateRequest_.exchange(false);  // RequestRate is waiting: no reset callback
            if (SUCCEEDED(OpenDevice(enumerator.Get(), event_, wantedMode_, wantedRate_, fresh, problem))) {
                dev = fresh;
                deviceWritten = 0;
                chunks.clear();
                chunkHead = 0;
                played_ = sourcePos;
                applyDevice();
                if (dev.rate != rate_) {
                    rate_ = dev.rate;
                    if (!requested && reset_) reset_(dev.rate);
                }
                if (requested) SetEvent(rateDone_);
            } else {
                if (requested) SetEvent(rateDone_);
                waiting_ = true;  // the UI says why the clock has stopped until this succeeds
                Sleep(500);
                reopen_ = true;  // try again
            }
        }
        WaitForSingleObject(event_, 200);
    }
    if (running && dev.client) dev.client->Stop();
    enumerator->UnregisterEndpointNotificationCallback(notifier);
    notifier->Release();
    dev = Device{};
    holding_ = false;
    CoUninitialize();
}

#endif  // _WIN32
