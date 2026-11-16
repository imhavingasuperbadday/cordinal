// WASAPI device handling and float-to-device-format conversion (see audio_output.h). The render
// thread opens the default device, re-opens it when it is lost or the default changes, and keeps a
// chunk table mapping device frames already played back to source positions for PlayedPosition().
#include "audio_output.h"

#include "upmix.h"

#ifdef _WIN32
#include "asio.h"
#include "speakers.h"

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

// One generator for 16-bit and 24-bit dither. An exact block must not call this: the following
// dithered block then keeps the sequence the conversion tests lock to this seed.
uint32_t& DitherState() {
    static thread_local uint32_t noise = 0x9E3779B9u;
    return noise;
}

// True when every finite sample already sits on a step of `Bits`, so conversion must not draw
// dither. Non-finite samples count as exact: they become silence. At |input| >= bound, scaling
// lands on integers because the float ulp is at least one step. The SIMD test clamps to that
// bound so a huge lane cannot overflow the integer convert. Double precision matches the scalar
// test: a float multiply could flush a small fraction to zero when FTZ is on.
template<unsigned Bits>
bool SamplesAreExact(const float* in, size_t samples) {
    static_assert(Bits == 16 || Bits == 24);
    constexpr double scale = Bits == 16 ? 32768.0 : 8388608.0;
    constexpr double bound = Bits == 16 ? 256.0 : 1.0;
    bool exact = true;
    size_t checked = 0;
    for (; checked + 2 <= samples; checked += 2) {
        __m128 v = _mm_castsi128_ps(_mm_loadl_epi64((const __m128i*)(in + checked)));
        v = _mm_and_ps(v, _mm_castsi128_ps(FiniteMask(v)));
        const __m128d steps = _mm_mul_pd(_mm_max_pd(_mm_set1_pd(-bound),
            _mm_min_pd(_mm_set1_pd(bound), _mm_cvtps_pd(v))), _mm_set1_pd(scale));
        if (_mm_movemask_pd(_mm_cmpeq_pd(steps, _mm_cvtepi32_pd(_mm_cvtpd_epi32(steps)))) != 3) {
            exact = false;
            break;
        }
    }
    for (; checked < samples && exact; ++checked)
        exact = !std::isfinite(in[checked]) || in[checked] * scale == std::nearbyint(in[checked] * scale);
    return exact;
}

// Two triangular samples, four generator steps, in the order Convert16 has always used.
void DrawDither(uint32_t& noise, int32_t error[2]) {
    for (unsigned j = 0; j < 2; ++j) {
        noise = noise * 1664525u + 1013904223u; const int32_t first = (int32_t)(noise >> 8);
        noise = noise * 1664525u + 1013904223u; error[j] = first - (int32_t)(noise >> 8);
    }
}

double DrawDither(uint32_t& noise) {
    noise = noise * 1664525u + 1013904223u;
    double dither = (noise >> 8) * (1.0 / 16777216.0);
    noise = noise * 1664525u + 1013904223u;
    dither -= (noise >> 8) * (1.0 / 16777216.0);
    return dither;
}

// Keep the dither state and its double-precision work outside the other formats' loops.
__declspec(noinline) void Convert16(const float* in, size_t samples, int16_t* o) {
    // ReplayGain, EQ and volume leave samples between 16-bit steps; rounding those plainly
    // turns the error into distortion that follows the music (audible in quiet passages
    // and fades). TPDF dither of +/-1 step makes it a constant, signal-free noise floor
    // instead. A block that is all 16-bit values already (an untouched 16-bit source,
    // digital silence) is converted exactly, so the bit-perfect path stays bit-perfect.
    if (SamplesAreExact<16>(in, samples)) {
        size_t i = 0;
        for (; i + 8 <= samples; i += 8)
            _mm_storeu_si128((__m128i*)(o + i), _mm_packs_epi32(Quantize<16>(in + i), Quantize<16>(in + i + 4)));
        for (; i < samples; ++i) o[i] = (int16_t)(std::isfinite(in[i]) ?
            (int64_t)std::clamp(std::nearbyint(in[i] * 32768.0), -32768.0, 32767.0) : 0);
        return;
    }
    uint32_t& noise = DitherState();
    size_t i = 0;
    for (; i + 2 <= samples; i += 2) {
        int32_t error[2];
        DrawDither(noise, error);
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
        // Draw even when the sample is non-finite, so a NaN does not shift the noise sequence.
        const double dither = DrawDither(noise);
        o[i] = (int16_t)(std::isfinite(in[i]) ? (long long)std::clamp(
            std::nearbyint(in[i] * 32768.0 + dither), -32768.0, 32767.0) : 0LL);
    }
}

// 24-bit, same rule as 16-bit. `Packed` writes three little-endian bytes; otherwise the value is
// left-aligned in an int32, which is how a 24-in-32 WAVEFORMATEXTENSIBLE container stores it.
template<bool Packed>
__declspec(noinline) void Convert24(const float* in, size_t samples, void* out) {
    auto store1 = [&](size_t i, int32_t v) {
        if constexpr (Packed) {
            auto* o = static_cast<uint8_t*>(out);
            const uint32_t u = (uint32_t)v;
            o[i * 3] = (uint8_t)u;
            o[i * 3 + 1] = (uint8_t)(u >> 8);
            o[i * 3 + 2] = (uint8_t)(u >> 16);
        } else {
            static_cast<int32_t*>(out)[i] = (int32_t)((uint32_t)v << 8);
        }
    };
    auto store4 = [&](size_t i, __m128i v) {
        if constexpr (Packed) {
            int32_t values[4];
            _mm_storeu_si128((__m128i*)values, v);
            for (unsigned j = 0; j < 4; ++j) store1(i + j, values[j]);
        } else {
            _mm_storeu_si128((__m128i*)(static_cast<int32_t*>(out) + i), _mm_slli_epi32(v, 8));
        }
    };
    if (SamplesAreExact<24>(in, samples)) {
        size_t i = 0;
        for (; i + 4 <= samples; i += 4) store4(i, Quantize<24>(in + i));
        for (; i < samples; ++i) {
            const int32_t v = std::isfinite(in[i]) ? (int32_t)std::clamp(
                std::nearbyint(in[i] * 8388608.0), -8388608.0, 8388607.0) : 0;
            store1(i, v);
        }
        return;
    }
    uint32_t& noise = DitherState();
    size_t i = 0;
    for (; i + 2 <= samples; i += 2) {
        int32_t error[2];
        DrawDither(noise, error);
        const __m128d dither = _mm_mul_pd(_mm_cvtepi32_pd(_mm_setr_epi32(error[0], error[1], 0, 0)),
                                         _mm_set1_pd(1.0 / 16777216.0));
        const __m128 v = _mm_castsi128_ps(_mm_loadl_epi64((const __m128i*)(in + i)));
        const __m128i finite = FiniteMask(v);
        const __m128d value = _mm_add_pd(_mm_mul_pd(_mm_cvtps_pd(_mm_and_ps(v, _mm_castsi128_ps(finite))),
                                                   _mm_set1_pd(8388608.0)), dither);
        const __m128i rounded = _mm_and_si128(finite, _mm_cvtpd_epi32(
            _mm_min_pd(_mm_set1_pd(8388607.0), _mm_max_pd(_mm_set1_pd(-8388608.0), value))));
        int32_t pair[2];
        _mm_storel_epi64((__m128i*)pair, rounded);
        store1(i, pair[0]);
        store1(i + 1, pair[1]);
    }
    for (; i < samples; ++i) {
        const double dither = DrawDither(noise);
        const int32_t v = std::isfinite(in[i]) ? (int32_t)std::clamp(
            std::nearbyint(in[i] * 8388608.0 + dither), -8388608.0, 8388607.0) : 0;
        store1(i, v);
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
        case SampleFormat::Int24In32:
            Convert24<false>(in, samples, out);
            break;
        case SampleFormat::Int32: {
            auto* o = static_cast<int32_t*>(out);
            size_t i = 0;
            for (; i + 4 <= samples; i += 4) _mm_storeu_si128((__m128i*)(o + i), Quantize<32>(in + i));
            for (; i < samples; ++i) o[i] = (int32_t)scaled(in[i], 2147483648.0, -2147483648.0, 2147483647.0);
            break;
        }
        case SampleFormat::Int24:
            Convert24<true>(in, samples, out);
            break;
    }
}

// The WASAPI output; the Linux build has its own (linux/audio_linux.cpp).
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
    SpeakerLayout upmix;  // the device's speakers when the stereo render is upmixed; empty otherwise
    std::shared_ptr<AsioStream> asio;  // ASIO output: the driver renders from its own callback instead
    std::wstring asioName;             // the registry name it was opened by
    bool Open() const { return client || asio; }
};

// The nearest WASAPI sample format to an ASIO sample type, for Format().
SampleFormat FormatForAsio(long type) {
    switch (type) {
        case asio::kInt16LSB: case asio::kInt32LSB16: return SampleFormat::Int16;
        case asio::kInt24LSB: return SampleFormat::Int24;
        case asio::kInt32LSB24: return SampleFormat::Int24In32;
        case asio::kFloat32LSB: case asio::kFloat64LSB: return SampleFormat::Float32;
        default: return SampleFormat::Int32;
    }
}

// Device frames rendered by an ASIO driver mapped back to source positions, one entry per buffer
// switch, the way Run's chunk table does it for WASAPI. Written only on the driver's thread.
struct AsioClock {
    struct Chunk {
        unsigned long long deviceEnd, sourceEnd;
        size_t audio, total;
    };
    static constexpr size_t kChunks = 32;  // covers a latency of 32 buffers
    Chunk at[kChunks]{};
    size_t count = 0, next = 0;
    unsigned long long written = 0;
    void Clear() { count = next = 0; written = 0; }
    void Push(size_t frames, unsigned long long sourceEnd, size_t audio) {
        written += frames;
        at[next] = {written, sourceEnd, audio, frames};
        next = (next + 1) % kChunks;
        count = std::min(count + 1, kChunks);
    }
    // The source position reached when the device has played `playedDevice` frames.
    unsigned long long Played(unsigned long long playedDevice, unsigned long long fallback) const {
        for (size_t k = 0; k < count; ++k) {
            const Chunk& c = at[(next + kChunks - count + k) % kChunks];
            if (c.deviceEnd <= playedDevice) continue;
            const unsigned long long start = c.deviceEnd - c.total;
            const size_t into = playedDevice > start ? (size_t)(playedDevice - start) : 0;
            return c.sourceEnd - c.audio + std::min(into, c.audio);
        }
        return fallback;
    }
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

// Stereo float or 16/24/32-bit PCM can be submitted as the mix format itself. Anything else
// (surround, an unknown subtype) declines, and shared mode falls back to float32 plus WASAPI conversion.
std::optional<SampleFormat> FormatFromMix(const WAVEFORMATEX* mix) {
    if (!mix || !mix->nChannels) return std::nullopt;
    MixLayout layout;
    layout.channels = mix->nChannels;
    layout.bits = mix->wBitsPerSample;
    layout.validBits = mix->wBitsPerSample;
    layout.blockAlign = mix->nBlockAlign;
    if (mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        layout.pcm = false;
    } else if (mix->wFormatTag == WAVE_FORMAT_PCM) {
        layout.pcm = true;
    } else if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
               mix->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix);
        if (ext->Samples.wValidBitsPerSample) layout.validBits = ext->Samples.wValidBitsPerSample;
        if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) layout.pcm = false;
        else if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_PCM) layout.pcm = true;
        else return std::nullopt;
    } else {
        return std::nullopt;
    }
    return NativeMixFormat(layout);
}

// Shared mode at the device mix rate. The buffer request stays 100 ms. The engine period is raised
// toward 20 ms (the same period the Linux stream asks for) when the device allows it, so the render
// thread is not woken on a 10 ms cadence to refill a few milliseconds. `flags` carries AUTOCONVERTPCM
// only for the float fallback; a native mix format is submitted without a second converter.
HRESULT OpenSharedFormat(IMMDevice* device, HANDLE event, Device& d, WAVEFORMATEX* fmt, DWORD flags,
                         SampleFormat sampleFormat) {
    auto activate = [&] { return device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &d.client); };
    HRESULT hr = S_OK;
    ComPtr<IAudioClient3> client3;
    if (SUCCEEDED(d.client.As(&client3))) {
        UINT32 def = 0, fund = 0, minPeriod = 0, maxPeriod = 0;
        if (SUCCEEDED(client3->GetSharedModeEnginePeriod(fmt, &def, &fund, &minPeriod, &maxPeriod)) && fund &&
            maxPeriod >= fund) {
            UINT32 want = (UINT32)std::min<uint64_t>(maxPeriod, std::max<uint64_t>(def, (uint64_t)d.rate / 50));
            if (want > fund) want -= want % fund;
            if (want >= fund && want <= maxPeriod && want > def) {
                hr = client3->InitializeSharedAudioStream(flags, want, fmt, nullptr);
                UINT32 frames = 0;
                if (SUCCEEDED(hr)) hr = d.client->GetBufferSize(&frames);
                // Keep the buffer inside the 100 ms this function already requested, plus one
                // period so a driver that rounds up still qualifies.
                const UINT32 limit = d.rate / 10 + want;
                if (SUCCEEDED(hr) && frames >= want && frames <= limit) {
                    d.exclusive = false;
                    d.format = sampleFormat;
                    return FinishOpen(event, d);
                }
                d.client.Reset();
                hr = activate();
                if (FAILED(hr)) return hr;
            }
        }
    }
    hr = d.client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 1000000 /* 100 ms */, 0, fmt, nullptr);
    if (FAILED(hr)) return hr;
    d.exclusive = false;
    d.format = sampleFormat;
    return FinishOpen(event, d);
}

// The device's speakers when its mix has more than two and the upmixer can feed some of them.
SpeakerLayout UpmixLayout(const WAVEFORMATEX* mix, DWORD& mask) {
    mask = 0;
    if (!mix || mix->nChannels <= 2 || mix->nChannels > kMaxSpeakers) return {};
    if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
        mask = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix)->dwChannelMask;
    SpeakerLayout layout = LayoutFromMask(mask, mix->nChannels);
    if (!mask) {
        for (uint8_t i = 0; i < layout.count; ++i) {
            const Speaker s = layout.at[i];
            static const DWORD bits[] = {SPEAKER_FRONT_LEFT, SPEAKER_FRONT_RIGHT, SPEAKER_FRONT_CENTER,
                                         SPEAKER_LOW_FREQUENCY, SPEAKER_BACK_LEFT, SPEAKER_BACK_RIGHT,
                                         SPEAKER_SIDE_LEFT, SPEAKER_SIDE_RIGHT};
            if (s != Speaker::Other) mask |= bits[(int)s];
        }
    }
    return layout.Upmixes() ? layout : SpeakerLayout{};
}

HRESULT OpenShared(IMMDevice* device, HANDLE event, Device& d, bool upmix) {
    auto activate = [&] { return device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &d.client); };
    HRESULT hr = activate();
    if (FAILED(hr)) return hr;
    WAVEFORMATEX* mix = nullptr;
    hr = d.client->GetMixFormat(&mix);
    if (FAILED(hr)) return hr;
    d.rate = mix->nSamplesPerSec;
    const size_t mixBytes = sizeof(WAVEFORMATEX) + mix->cbSize;
    std::vector<unsigned char> mixCopy(mixBytes);
    memcpy(mixCopy.data(), mix, mixBytes);
    const std::optional<SampleFormat> native = FormatFromMix(mix);
    DWORD mask = 0;
    const SpeakerLayout layout = upmix ? UpmixLayout(mix, mask) : SpeakerLayout{};
    CoTaskMemFree(mix);
    auto* mixFmt = reinterpret_cast<WAVEFORMATEX*>(mixCopy.data());
    const DWORD convert = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                          AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    if (layout.count) {
        // Float in the device's own speaker layout; Windows converts to the mix's sample type.
        WAVEFORMATEXTENSIBLE fmt = MakeFormat(d.rate, SampleFormat::Float32);
        fmt.Format.nChannels = layout.count;
        fmt.Format.nBlockAlign = (WORD)(4 * layout.count);
        fmt.Format.nAvgBytesPerSec = d.rate * fmt.Format.nBlockAlign;
        fmt.dwChannelMask = mask;
        hr = OpenSharedFormat(device, event, d, &fmt.Format, convert, SampleFormat::Float32);
        if (SUCCEEDED(hr)) {
            d.upmix = layout;
            return hr;
        }
        d.client.Reset();
        hr = activate();
        if (FAILED(hr)) return hr;
    }
    if (native) {
        hr = OpenSharedFormat(device, event, d, mixFmt, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, *native);
        if (SUCCEEDED(hr)) return hr;
        d.client.Reset();
        hr = activate();
        if (FAILED(hr)) return hr;
    }
    WAVEFORMATEXTENSIBLE fmt = MakeFormat(d.rate, SampleFormat::Float32);
    return OpenSharedFormat(device, event, d, &fmt.Format, convert, SampleFormat::Float32);
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

// The driver ASIO output should use: `wanted` by name, else the first installed one.
bool FindAsioDriver(const std::wstring& wanted, AsioDriverInfo& out, std::wstring& why) {
    const std::vector<AsioDriverInfo> drivers = ListAsioDrivers();
    if (drivers.empty()) {
        why = L"no ASIO driver is installed";
        return false;
    }
    for (const AsioDriverInfo& d : drivers)
        if (!_wcsicmp(d.name.c_str(), wanted.c_str())) {
            out = d;
            return true;
        }
    out = drivers.front();
    return true;
}

// ASIO output at `rate` (0 keeps the driver's own). On success the driver is already playing from
// `render`.
bool OpenAsio(const std::wstring& wanted, unsigned rate, AsioStream::RenderFn render, AsioStream::ResetFn reset,
              Device& d, std::wstring& problem) {
    AsioDriverInfo info;
    std::wstring why;
    if (FindAsioDriver(wanted, info, why)) {
        auto stream = std::make_shared<AsioStream>();
        if (stream->Open(info, rate, std::move(render), std::move(reset), why)) {
            d.asio = std::move(stream);
            d.asioName = info.name;
            d.rate = d.asio->Rate();
            d.format = FormatForAsio(d.asio->Type());
            problem = d.asio->Problem();
            return true;
        }
        why = info.name + L": " + why;
    }
    problem = L"ASIO output isn't available (" + why + L"); using shared output.";
    return false;
}

// Opens the default device: shared, or exclusive at `rate` (else at another rate it supports,
// else shared), or the ASIO driver `asioDriver` (else shared). `problem` says why exclusive or ASIO
// output isn't exactly what was asked for.
HRESULT OpenDevice(IMMDeviceEnumerator* enumerator, HANDLE event, OutputMode mode, unsigned rate, bool upmix, Device& d,
                   std::wstring& problem, const std::wstring& asioDriver = {}, AsioStream::RenderFn asioRender = {},
                   AsioStream::ResetFn asioReset = {}) {
    d = Device{};
    problem.clear();
    if (mode == OutputMode::Asio && OpenAsio(asioDriver, rate, std::move(asioRender), std::move(asioReset), d, problem))
        return S_OK;
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
            for (SampleFormat f : kExclusiveFormats) {
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
    return OpenShared(device.Get(), event, d, upmix);
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

void AudioOutput::SetUpmix(bool upmix) {
    if (wantedUpmix_.exchange(upmix) == upmix) return;
    if (thread_.joinable()) reopen_ = true;
    wake();
}

std::string AudioOutput::UpmixLabel() const {
    std::lock_guard<std::mutex> lock(problemMx_);
    return upmixLabel_;
}

void AudioOutput::SetAsioDriver(const std::wstring& name) {
    {
        std::lock_guard<std::mutex> lock(problemMx_);
        if (asioDriver_ == name) return;
        asioDriver_ = name;
    }
    if (thread_.joinable() && wantedMode_ == OutputMode::Asio) reopen_ = true;
    wake();
}

std::wstring AudioOutput::AsioDriver() const {
    std::lock_guard<std::mutex> lock(problemMx_);
    return asioDriver_;
}

std::wstring AudioOutput::DeviceName() const {
    std::lock_guard<std::mutex> lock(problemMx_);
    return deviceName_;
}

bool AudioOutput::ShowAsioPanel() {
    if (mode_ != OutputMode::Asio || !holding_) return false;
    panel_ = true;
    wake();
    return true;
}

unsigned AudioOutput::RequestRate(unsigned rate) {
    // A network speaker plays one rate whatever the mode: the engine resamples, as for shared output.
    if (wantedMode_ == OutputMode::Shared || !thread_.joinable() || !SpeakerId().empty()) return rate_;
    if (rate == rate_ && mode_ == wantedMode_) return rate_;
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

static std::wstring WideText(const std::string& utf8) {
    std::wstring out(utf8.size(), L'\0');
    out.resize((size_t)std::max(0, MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), out.data(), (int)out.size())));
    return out;
}

void AudioOutput::SetSpeaker(const std::string& id) {
    {
        std::lock_guard<std::mutex> lock(problemMx_);
        if (speaker_ == id) return;
        speaker_ = id;
    }
    wake();
}

std::string AudioOutput::SpeakerId() const {
    std::lock_guard<std::mutex> lock(problemMx_);
    return speaker_;
}

std::wstring AudioOutput::SpeakerName() const {
    speakers::Speaker s;
    return speakers::ParseDeviceId(SpeakerId(), s) ? WideText(s.name) : std::wstring();
}

// Render thread, while a network speaker is the output: the speaker's own thread renders as the
// speaker reads (see speakers.h), and this one passes on pause and flush, and opens the speaker
// again a few seconds after it failed. Returns once the speaker is no longer wanted.
void AudioOutput::RunSpeaker(unsigned long long& sourcePos) {
    const std::string id = SpeakerId();
    constexpr unsigned kRate = 48000;
    // Stream frames -> source positions, one entry per render (as for WASAPI below). Only the
    // speaker's thread touches these and `sourcePos` while a stream exists.
    struct Chunk {
        unsigned long long end, sourceEnd;
        size_t audio, total;
    };
    std::deque<Chunk> chunks;
    unsigned long long written = 0;
    std::atomic<bool> reset{false};
    std::atomic<unsigned long long> resetPos{0};
    speakers::Callbacks callbacks;
    callbacks.render = [&](float* out, uint32_t frames) {
        if (reset.exchange(false)) {
            sourcePos = resetPos;
            chunks.clear();
        }
        const size_t audio = render_(out, frames, sourcePos);
        written += frames;
        if (chunks.size() >= 1024) chunks.pop_front();
        chunks.push_back({written, sourcePos, audio, frames});
    };
    callbacks.timing = [&](uint64_t played) {
        if (reset) return;  // the position was set by Flush; the next render applies it
        while (chunks.size() > 1 && chunks.front().end <= played) chunks.pop_front();
        unsigned long long pos = sourcePos;
        for (const Chunk& c : chunks) {
            if (c.end <= played) continue;
            const unsigned long long start = c.end - c.total;
            pos = c.sourceEnd - c.audio + std::min<unsigned long long>(played > start ? played - start : 0, c.audio);
            break;
        }
        played_ = pos;
    };
    std::unique_ptr<speakers::Stream> stream;
    ULONGLONG retryAt = 0;
    bool active = false;
    auto open = [&] {
        std::string error;
        active = !paused_;
        stream = speakers::Stream::Open(id, kRate, active, callbacks, [this] { wake(); }, error);
        std::lock_guard<std::mutex> lock(problemMx_);
        problem_ = stream ? std::wstring() : L"Couldn't use the network speaker: " + WideText(error) + L".";
    };
    mode_ = OutputMode::Shared;
    format_ = SampleFormat::Int16;
    asioType_ = -1;
    holding_ = true;
    waiting_ = false;
    {
        std::lock_guard<std::mutex> lock(problemMx_);
        upmixLabel_.clear();
        deviceName_.clear();
    }
    if (rate_ != kRate) {
        rate_ = kRate;
        if (reset_) reset_(kRate);
    }
    open();
    while (!quit_ && SpeakerId() == id) {
        if (flush_.exchange(false)) {
            played_ = flushPos_.load();
            if (stream) {
                resetPos = flushPos_.load();
                reset = true;
                stream->Flush();
            } else {
                sourcePos = flushPos_;
            }
        }
        // Nothing to re-open for: a rate request answers at once, at the speaker's rate.
        if (rateRequest_.exchange(false)) SetEvent(rateDone_);
        reopen_ = false;
        if (stream) {
            if (active == paused_) {
                active = !paused_;
                stream->SetActive(active);
            }
            if (stream->Failed()) {
                const std::wstring why = WideText(stream->Problem());
                stream.reset();  // no callback runs after this: `sourcePos` is ours again
                if (reset.exchange(false)) sourcePos = resetPos;
                waiting_ = true;
                retryAt = GetTickCount64() + 3000;
                std::lock_guard<std::mutex> lock(problemMx_);
                problem_ = why + L" Trying again.";
            }
        } else if (GetTickCount64() >= retryAt) {
            open();
            waiting_ = !stream;
        }
        WaitForSingleObject(event_, 200);
    }
    stream.reset();  // tells the speaker to stop
    if (reset.exchange(false)) sourcePos = resetPos;
    waiting_ = false;
    holding_ = false;
    std::lock_guard<std::mutex> lock(problemMx_);
    problem_.clear();
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
    unsigned long long sourcePos = 0;
    // ASIO output renders on the driver's thread, from its buffer switch. While a driver is loaded only
    // that thread touches `sourcePos` and `asioClock`; this one reads them again once the driver is gone.
    AsioClock asioClock;
    std::atomic<bool> asioClockReset{false};
    std::atomic<unsigned> asioLatency{0};  // the loaded driver's, set once it is open
    AsioStream::RenderFn asioRender = [this, &sourcePos, &asioClock, &asioClockReset, &asioLatency](float* out,
                                                                                                    size_t frames) {
        if (asioClockReset.exchange(false)) asioClock.Clear();
        if (flush_.exchange(false)) {
            sourcePos = flushPos_;
            asioClock.Clear();
            played_ = sourcePos;
        }
        if (paused_) {
            memset(out, 0, frames * 2 * sizeof(float));
            return;
        }
        const size_t audio = render_(out, frames, sourcePos);
        asioClock.Push(frames, sourcePos, audio);
        // What the speakers play now went into the driver one buffer and its output latency ago.
        const unsigned long long queued = frames + asioLatency.load(std::memory_order_relaxed);
        played_ = asioClock.Played(asioClock.written > queued ? asioClock.written - queued : 0, sourcePos);
    };
    AsioStream::ResetFn asioReset = [this] {
        reopen_ = true;
        wake();
    };
    auto open = [&](Device& d) {
        asioClockReset = true;
        return OpenDevice(enumerator.Get(), event_, wantedMode_, wantedRate_, wantedUpmix_, d, problem, AsioDriver(),
                          asioRender, asioReset);
    };
    if (SUCCEEDED(hr)) hr = open(dev);
    if (FAILED(hr)) {
        wchar_t buf[96];
        swprintf_s(buf, L"No audio output device (0x%08lX).", (unsigned long)hr);
        error_ = buf;
        SetEvent(ready_);
        CoUninitialize();
        return;
    }
    std::unique_ptr<Upmixer> upmixer;  // made the first time a surround device is upmixed
    auto applyDevice = [&] {
        mode_ = dev.asio ? OutputMode::Asio : dev.exclusive ? OutputMode::Exclusive : OutputMode::Shared;
        format_ = dev.format;
        asioType_ = dev.asio ? dev.asio->Type() : -1;
        asioLatency = dev.asio ? dev.asio->Latency() : 0;
        holding_ = true;
        waiting_ = false;
        if (dev.upmix.count) {
            if (!upmixer) upmixer = std::make_unique<Upmixer>();
            upmixer->Configure(dev.upmix, dev.rate);
            upmixer->Reset();
        }
        std::lock_guard<std::mutex> lock(problemMx_);
        problem_ = problem;
        upmixLabel_ = dev.upmix.count ? LayoutLabel(dev.upmix) : std::string();
        deviceName_ = dev.asio ? dev.asio->DriverName() : std::wstring();
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
    std::vector<float> convert;  // render buffer when the device doesn't take float, or is upmixed
    unsigned long long deviceWritten = 0;
    bool running = false;
    ULONGLONG idleSince = 0;  // when a paused output started sitting on the device

    while (!quit_) {
        if (!SpeakerId().empty()) {
            // A network speaker takes over: this computer's device (or ASIO driver) is let go meanwhile.
            if (running && dev.client) dev.client->Stop();
            running = false;
            dev = Device{};
            holding_ = false;
            RunSpeaker(sourcePos);
            reopen_ = true;  // back on this computer: open its device again
            continue;
        }
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
        if (dev.Open() && paused_ && !wantDevice) {
            ULONGLONG now = GetTickCount64();
            if (!idleSince) idleSince = now;
            else if (now - idleSince >= kIdleReleaseMs) {
                dev = Device{};  // the claim goes with the client (or the ASIO driver)
                holding_ = false;
            }
        } else {
            idleSince = 0;
        }
        if (!lost && wantDevice && !dev.Open()) lost = true;  // resume after letting go
        if (panel_.exchange(false) && dev.asio) dev.asio->ShowPanel();
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
                if (upmixer) upmixer->Reset();
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
                        if (dev.upmix.count) {
                            convert.resize((size_t)avail * 2);
                            audio = render_(convert.data(), avail, sourcePos);
                            upmixer->Process(convert.data(), avail, reinterpret_cast<float*>(data));
                        } else if (dev.format == SampleFormat::Float32) {
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
            bool requested = rateRequest_.exchange(false);  // RequestRate is waiting: no reset callback
            Device fresh;
            HRESULT opened = E_FAIL;
            // The same ASIO driver is set up again in place (a new rate, a reset from its panel): loading
            // a driver can take far longer than restarting it.
            const std::wstring wantedDriver = AsioDriver();  // empty: whichever is installed first, as loaded
            if (dev.asio && wantedMode_ == OutputMode::Asio &&
                (wantedDriver.empty() || !_wcsicmp(dev.asioName.c_str(), wantedDriver.c_str()))) {
                std::wstring why;
                asioClockReset = true;
                if (dev.asio->Restart(wantedRate_, why)) {
                    fresh = dev;
                    fresh.rate = dev.asio->Rate();
                    fresh.format = FormatForAsio(dev.asio->Type());
                    problem = dev.asio->Problem();
                    opened = S_OK;
                }
            }
            if (FAILED(opened)) {
                dev = Device{};  // release the device first: exclusive mode can't be opened twice
                holding_ = false;
                opened = open(fresh);
            }
            if (SUCCEEDED(opened)) {
                dev = fresh;
                deviceWritten = 0;
                chunks.clear();
                chunkHead = 0;
                if (!dev.asio) played_ = sourcePos;  // a driver's buffer switch keeps it itself
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
