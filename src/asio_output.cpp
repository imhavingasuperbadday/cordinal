// ASIO driver discovery, sample conversion and the driver host thread (see asio.h). Nothing here comes
// from Steinberg's SDK: the interface is declared in asio.h from the ABI the drivers implement, and the
// registry layout, threading and callbacks are handled directly.
#include "asio.h"

#ifdef _WIN32
#include "audio_output.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

// --- Finding drivers.

namespace {

int HexDigit(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

bool ReadHex(std::wstring_view text, size_t at, size_t digits, unsigned long long& out) {
    out = 0;
    for (size_t i = 0; i < digits; ++i) {
        const int d = HexDigit(text[at + i]);
        if (d < 0) return false;
        out = out << 4 | (unsigned)d;
    }
    return true;
}

std::wstring_view Trim(std::wstring_view s) {
    while (!s.empty() && (s.front() == L' ' || s.front() == L'\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t' || s.back() == L'\0')) s.remove_suffix(1);
    return s;
}

std::wstring Widen(const char* text) {
    if (!text || !*text) return {};
    const int n = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    std::wstring out(n > 0 ? (size_t)n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_ACP, 0, text, -1, out.data(), n);
    return out;
}

std::wstring RegString(HKEY key, const wchar_t* subkey, const wchar_t* value) {
    DWORD bytes = 0;
    if (RegGetValueW(key, subkey, value, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes < 2) return {};
    std::wstring text(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(key, subkey, value, RRF_RT_REG_SZ, nullptr, text.data(), &bytes) != ERROR_SUCCESS) return {};
    text.resize(wcsnlen(text.c_str(), text.size()));
    return text;
}

}  // namespace

bool ParseClsid(std::wstring_view text, CLSID& out) {
    text = Trim(text);
    if (text.size() != 38 || text[0] != L'{' || text[37] != L'}' || text[9] != L'-' || text[14] != L'-' ||
        text[19] != L'-' || text[24] != L'-')
        return false;
    unsigned long long v = 0;
    CLSID id{};
    if (!ReadHex(text, 1, 8, v)) return false;
    id.Data1 = (unsigned long)v;
    if (!ReadHex(text, 10, 4, v)) return false;
    id.Data2 = (unsigned short)v;
    if (!ReadHex(text, 15, 4, v)) return false;
    id.Data3 = (unsigned short)v;
    for (size_t i = 0; i < 8; ++i) {
        const size_t at = i < 2 ? 20 + i * 2 : 25 + (i - 2) * 2;
        if (!ReadHex(text, at, 2, v)) return false;
        id.Data4[i] = (unsigned char)v;
    }
    out = id;
    return true;
}

bool ParseAsioDriver(std::wstring_view keyName, std::wstring_view clsidValue, std::wstring_view description,
                     AsioDriverInfo& out) {
    keyName = Trim(keyName);
    if (keyName.empty()) return false;
    CLSID clsid;
    if (!ParseClsid(clsidValue, clsid)) return false;
    out.name.assign(keyName);
    description = Trim(description);
    out.description.assign(description.empty() ? keyName : description);
    out.clsid = clsid;
    return true;
}

std::vector<AsioDriverInfo> ListAsioDrivers() {
    std::vector<AsioDriverInfo> drivers;
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ, &root) != ERROR_SUCCESS) return drivers;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256];
        DWORD length = (DWORD)std::size(name);
        const LONG r = RegEnumKeyExW(root, i, name, &length, nullptr, nullptr, nullptr, nullptr);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS) continue;
        AsioDriverInfo info;
        if (!ParseAsioDriver(name, RegString(root, name, L"CLSID"), RegString(root, name, L"Description"), info))
            continue;
        // The class must load in this process: a driver installed only for 32-bit hosts registers its
        // server under WOW6432Node, which a 64-bit CoCreateInstance never finds.
        wchar_t clsid[40];
        StringFromGUID2(info.clsid, clsid, (int)std::size(clsid));
        const std::wstring server = std::wstring(L"CLSID\\") + clsid + L"\\InprocServer32";
        HKEY probe = nullptr;
        if (RegOpenKeyExW(HKEY_CLASSES_ROOT, server.c_str(), 0, KEY_READ, &probe) != ERROR_SUCCESS) continue;
        RegCloseKey(probe);
        drivers.push_back(std::move(info));
    }
    RegCloseKey(root);
    std::sort(drivers.begin(), drivers.end(),
              [](const AsioDriverInfo& a, const AsioDriverInfo& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
    return drivers;
}

// --- Sample conversion.

bool AsioTypeSupported(long type) { return AsioSampleBytes(type) != 0; }

bool AsioTypeIsDsd(long type) {
    return type == asio::kDsdInt8LSB1 || type == asio::kDsdInt8MSB1 || type == asio::kDsdInt8NER8;
}

bool AsioTypeCarriesDop(long type) {
    return type == asio::kInt24LSB || type == asio::kInt32LSB || type == asio::kInt32LSB24;
}

int AsioSampleBytes(long type) {
    switch (type) {
        case asio::kInt16LSB: return 2;
        case asio::kInt24LSB: return 3;
        case asio::kInt32LSB: case asio::kFloat32LSB: case asio::kInt32LSB16: case asio::kInt32LSB18:
        case asio::kInt32LSB20: case asio::kInt32LSB24: return 4;
        case asio::kFloat64LSB: return 8;
        case asio::kDsdInt8LSB1: case asio::kDsdInt8MSB1: case asio::kDsdInt8NER8: return 1;
        default: return 0;
    }
}

int AsioSampleBits(long type) {
    switch (type) {
        case asio::kInt16LSB: case asio::kInt32LSB16: return 16;
        case asio::kInt32LSB18: return 18;
        case asio::kInt32LSB20: return 20;
        case asio::kInt24LSB: case asio::kInt32LSB24: return 24;
        case asio::kFloat64LSB: return 64;
        case asio::kDsdInt8LSB1: case asio::kDsdInt8MSB1: case asio::kDsdInt8NER8: return 1;
        default: return 32;
    }
}

bool AsioTypeIsFloat(long type) { return type == asio::kFloat32LSB || type == asio::kFloat64LSB; }

const wchar_t* AsioTypeName(long type) {
    switch (type) {
        case asio::kInt16LSB: return L"16-bit";
        case asio::kInt24LSB: return L"packed 24-bit";
        case asio::kInt32LSB: return L"32-bit";
        case asio::kFloat32LSB: return L"32-bit float";
        case asio::kFloat64LSB: return L"64-bit float";
        case asio::kInt32LSB16: return L"16-bit in 32";
        case asio::kInt32LSB18: return L"18-bit in 32";
        case asio::kInt32LSB20: return L"20-bit in 32";
        case asio::kInt32LSB24: return L"24-bit in 32";
        case asio::kDsdInt8LSB1: case asio::kDsdInt8MSB1: case asio::kDsdInt8NER8: return L"DSD";
        default: return type >= 0 && type < 16 ? L"big-endian" : L"unknown";
    }
}

void WriteAsioChannel(const float* interleaved, size_t frames, unsigned channels, unsigned channel, long type,
                      void* out, float* scratch) {
    for (size_t i = 0; i < frames; ++i) scratch[i] = interleaved[i * channels + channel];
    auto* o32 = static_cast<int32_t*>(out);
    switch (type) {
        case asio::kInt16LSB: ConvertSamples(scratch, frames, SampleFormat::Int16, out); break;
        case asio::kInt24LSB: ConvertSamples(scratch, frames, SampleFormat::Int24, out); break;
        case asio::kInt32LSB: ConvertSamples(scratch, frames, SampleFormat::Int32, out); break;
        case asio::kFloat32LSB: ConvertSamples(scratch, frames, SampleFormat::Float32, out); break;
        case asio::kFloat64LSB: {
            auto* o = static_cast<double*>(out);
            for (size_t i = 0; i < frames; ++i) o[i] = std::isfinite(scratch[i]) ? (double)scratch[i] : 0.0;
            break;
        }
        case asio::kInt32LSB24:
            // The 24-in-32 WASAPI container is left-aligned; this one keeps the value in the low bits.
            ConvertSamples(scratch, frames, SampleFormat::Int24In32, out);
            for (size_t i = 0; i < frames; ++i) o32[i] >>= 8;
            break;
        case asio::kInt32LSB16: {
            // Written as int16 into the front of the buffer, then widened from the back so no sample is
            // overwritten before it is read.
            auto* o16 = static_cast<int16_t*>(out);
            ConvertSamples(scratch, frames, SampleFormat::Int16, out);
            for (size_t i = frames; i-- > 0;) o32[i] = o16[i];
            break;
        }
        case asio::kInt32LSB18:
        case asio::kInt32LSB20: {
            const int shift = 32 - AsioSampleBits(type);
            const int64_t top = (int64_t(1) << (31 - shift)) - 1;
            ConvertSamples(scratch, frames, SampleFormat::Int32, out);
            for (size_t i = 0; i < frames; ++i)
                o32[i] = (int32_t)std::min<int64_t>(((int64_t)o32[i] + (int64_t(1) << (shift - 1))) >> shift, top);
            break;
        }
        default: memset(out, 0, frames * 4); break;
    }
}

namespace {
// The eight bits of a byte back to front: kDsdInt8LSB1 puts the first of the eight DSD samples in the
// low bit. Three swaps, which is less work than a lookup and costs no table.
uint8_t ReverseBits(uint8_t b) {
    b = (uint8_t)(b >> 4 | b << 4);
    b = (uint8_t)((b & 0xCC) >> 2 | (b & 0x33) << 2);
    return (uint8_t)((b & 0xAA) >> 1 | (b & 0x55) << 1);
}
}  // namespace

void WriteAsioDop(const float* interleaved, size_t frames, size_t supplied, long type, uint8_t& marker,
                  void* left, void* right) {
    uint32_t top = (uint32_t)marker << 16;
    auto* l32 = static_cast<int32_t*>(left);
    auto* r32 = static_cast<int32_t*>(right);
    auto* l8 = static_cast<uint8_t*>(left);
    auto* r8 = static_cast<uint8_t*>(right);
    for (size_t i = 0; i < frames; ++i) {
        const uint32_t l = top | (i < supplied ? DopWord(interleaved[i * 2]) : 0x6969u);
        const uint32_t r = top | (i < supplied ? DopWord(interleaved[i * 2 + 1]) : 0x6969u);
        top ^= 0xFF0000u;  // 0x05 <-> 0xFA, the alternation a DoP receiver locks onto
        if (type == asio::kInt24LSB) {
            l8[i * 3] = (uint8_t)l;
            l8[i * 3 + 1] = (uint8_t)(l >> 8);
            l8[i * 3 + 2] = (uint8_t)(l >> 16);
            r8[i * 3] = (uint8_t)r;
            r8[i * 3 + 1] = (uint8_t)(r >> 8);
            r8[i * 3 + 2] = (uint8_t)(r >> 16);
        } else if (type == asio::kInt32LSB24) {  // 24 bits in the low end, sign-extended as WriteAsioChannel does
            l32[i] = (int32_t)(l << 8) >> 8;
            r32[i] = (int32_t)(r << 8) >> 8;
        } else {
            l32[i] = (int32_t)(l << 8);  // the 24-bit word in the top of a 32-bit sample
            r32[i] = (int32_t)(r << 8);
        }
    }
    marker = (uint8_t)(top >> 16);
}

void WriteAsioDsd(const float* interleaved, size_t frames, size_t supplied, bool lsbFirst, void* left,
                  void* right) {
    auto* l = static_cast<uint8_t*>(left);
    auto* r = static_cast<uint8_t*>(right);
    const uint8_t idle = lsbFirst ? ReverseBits(kDsdIdle) : kDsdIdle;
    size_t i = 0;
    for (; i < supplied && i < frames; ++i) {
        l[i] = DsdByte(interleaved[i * 2]);
        r[i] = DsdByte(interleaved[i * 2 + 1]);
        if (lsbFirst) {
            l[i] = ReverseBits(l[i]);
            r[i] = ReverseBits(r[i]);
        }
    }
    for (; i < frames; ++i) {
        l[i] = idle;
        r[i] = idle;
    }
}

// --- The host.

namespace {
// The stream the driver's callbacks belong to. A process hosts one ASIO driver at a time.
std::atomic<AsioStream*> g_active{nullptr};

// Switches the driver between PCM and DSD. CanDo is asked first, because a driver that doesn't implement
// DSD never gets half switched. Going back to PCM always succeeds: a driver without the extension is
// already there.
bool SetIoFormat(asio::IAsio* driver, long format) {
    asio::IoFormat probe{};
    probe.formatType = format;
    if (driver->future(asio::kCanDoIoFormat, &probe) != asio::kSuccess) return format == asio::kPcmFormat;
    asio::IoFormat want{};
    want.formatType = format;
    return driver->future(asio::kSetIoFormat, &want) == asio::kSuccess;
}
}  // namespace

struct AsioCallbacks {
    static void Switch(long index, asio::Bool) {
        if (AsioStream* s = g_active.load(std::memory_order_acquire)) s->Switch(index);
    }
    static void* SwitchTimeInfo(void* time, long index, asio::Bool direct) {
        Switch(index, direct);
        return time;
    }
    static void Reset() {
        if (AsioStream* s = g_active.load(std::memory_order_acquire); s && s->reset_) s->reset_();
    }
    static void RateChanged(asio::SampleRate rate) {
        AsioStream* s = g_active.load(std::memory_order_acquire);
        if (s && std::lround(rate) != (long)s->rate_.load()) Reset();
    }
    static long Message(long selector, long value, void*, double*) {
        switch (selector) {
            case asio::kSelectorSupported:
                return value == asio::kEngineVersion || value == asio::kResetRequest ||
                       value == asio::kBufferSizeChange || value == asio::kResyncRequest ||
                       value == asio::kLatenciesChanged || value == asio::kSupportsTimeInfo ||
                       value == asio::kOverload;
            case asio::kEngineVersion: return 2;
            // A new buffer size or a reset from the driver's panel: set the stream up again.
            case asio::kResetRequest:
            case asio::kBufferSizeChange: Reset(); return 1;
            case asio::kResyncRequest:
            case asio::kLatenciesChanged:
            case asio::kOverload: return 1;
            // No: the plain buffer switch is enough, and every driver supports it.
            case asio::kSupportsTimeInfo:
            case asio::kSupportsTimeCode: return 0;
            default: return 0;
        }
    }
    static asio::Callbacks table;
};
asio::Callbacks AsioCallbacks::table = {AsioCallbacks::Switch, AsioCallbacks::RateChanged, AsioCallbacks::Message,
                                        AsioCallbacks::SwitchTimeInfo};

struct AsioStream::Thread {
    std::thread worker;
    HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::mutex mx;  // one Call at a time
    std::mutex doneMx;
    std::condition_variable doneCv;
    const std::function<void()>* work = nullptr;  // guarded by doneMx
    bool finished = false;
    std::atomic<bool> quit{false}, panel{false};
    ~Thread() { if (wake) CloseHandle(wake); }
};

AsioStream::AsioStream() : thread_(std::make_unique<Thread>()) {
    Thread* t = thread_.get();
    HANDLE started = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    t->worker = std::thread([this, t, started] {
        // Drivers are apartment threaded: created, called and released on this one thread.
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        // init() takes a window: the parent some drivers give their panel. A hidden popup of a system
        // class is enough and needs no registration.
        window_ = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"cordinal ASIO", WS_POPUP, 0, 0, 0, 0, nullptr,
                                  nullptr, GetModuleHandleW(nullptr), nullptr);
        SetEvent(started);
        for (;;) {
            const DWORD r = MsgWaitForMultipleObjects(1, &t->wake, FALSE, INFINITE, QS_ALLINPUT);
            if (r == WAIT_OBJECT_0) {
                const std::function<void()>* work = nullptr;
                {
                    std::lock_guard<std::mutex> lock(t->doneMx);
                    work = t->work;
                }
                if (work) {
                    (*work)();
                    std::lock_guard<std::mutex> lock(t->doneMx);
                    t->work = nullptr;
                    t->finished = true;
                    t->doneCv.notify_all();
                }
                if (t->panel.exchange(false) && driver_) driver_->controlPanel();
                if (t->quit) break;
            }
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        if (window_) DestroyWindow(window_);
        window_ = nullptr;
        CoUninitialize();
    });
    WaitForSingleObject(started, INFINITE);
    CloseHandle(started);
}

AsioStream::~AsioStream() {
    Call([this] { Unload(); });
    thread_->quit = true;
    SetEvent(thread_->wake);
    thread_->worker.join();
}

void AsioStream::Call(const std::function<void()>& work) {
    std::lock_guard<std::mutex> one(thread_->mx);
    std::unique_lock<std::mutex> lock(thread_->doneMx);
    thread_->work = &work;
    thread_->finished = false;
    SetEvent(thread_->wake);
    thread_->doneCv.wait(lock, [&] { return thread_->finished; });
}

bool AsioStream::Open(const AsioDriverInfo& driver, unsigned rate, AsioDsd dsd, RenderFn render, ResetFn reset,
                      std::wstring& error) {
    bool ok = false;
    Call([&] {
        if (driver_ && !IsEqualCLSID(clsid_, driver.clsid)) Unload();
        render_ = std::move(render);
        reset_ = std::move(reset);
        if (!driver_) {
            void* object = nullptr;
            // An ASIO driver answers its own CLSID as the interface id.
            const HRESULT hr = CoCreateInstance(driver.clsid, nullptr, CLSCTX_INPROC_SERVER, driver.clsid, &object);
            if (FAILED(hr) || !object) {
                wchar_t buf[64];
                swprintf_s(buf, L"the driver didn't load (error 0x%08lX)", (unsigned long)hr);
                error = buf;
                return;
            }
            driver_ = static_cast<asio::IAsio*>(object);
            clsid_ = driver.clsid;
            if (!driver_->init(window_)) {
                char text[256]{};
                driver_->getErrorMessage(text);
                const std::wstring why = Widen(text);
                error = why.empty() ? L"the driver didn't start" : L"the driver didn't start: " + why;
                Unload();
                return;
            }
            char name[64]{};
            driver_->getDriverName(name);
            driverName_ = Widen(name);
            if (driverName_.empty()) driverName_ = driver.name;
        }
        ok = Setup(rate, dsd, error);
        if (!ok) Unload();
    });
    return ok;
}

bool AsioStream::Restart(unsigned rate, AsioDsd dsd, std::wstring& error) {
    bool ok = false;
    Call([&] {
        if (!driver_) {
            error = L"the driver isn't loaded";
            return;
        }
        ok = Setup(rate, dsd, error);
        if (!ok) Unload();
    });
    return ok;
}

void AsioStream::ShowPanel() {
    thread_->panel = true;
    SetEvent(thread_->wake);
}

bool AsioStream::Setup(unsigned rate, AsioDsd dsd, std::wstring& error) {
    Teardown();
    problem_.clear();
    long inputs = 0, outputs = 0;
    if (driver_->getChannels(&inputs, &outputs) != asio::kOk || outputs < 1) {
        error = L"the driver has no outputs";
        return false;
    }
    if (dsd != AsioDsd::None && outputs < 2) {
        error = L"the driver has one output, and DSD needs two";
        return false;
    }
    outputs_ = outputs >= 2 ? 2 : 1;

    // DSD goes through the driver's own DSD mode, with its sample rate set to the DSD bit rate and its
    // data to one bit a sample. The switch has to happen before the buffers are created.
    if (dsd == AsioDsd::Native && !SetIoFormat(driver_, asio::kDsdFormat)) {
        error = L"the driver can't play native DSD";
        return false;
    }

    // The rate: the track's when the driver can do it (no resampling), else the driver's own.
    asio::SampleRate current = 0;
    driver_->getSampleRate(&current);
    if (rate && std::lround(current) != (long)rate) {
        if (driver_->canSampleRate(rate) == asio::kOk && driver_->setSampleRate(rate) == asio::kOk)
            driver_->getSampleRate(&current);
        if (std::lround(current) != (long)rate)
            problem_ = L"the driver can't play " + std::to_wstring(rate) + L" Hz";
    }
    if (dsd != AsioDsd::None) {
        // A DSD stream is only DSD at its own rate; another one would be played as if it were that rate.
        if (!rate || (unsigned)std::lround(current) != rate) {
            error = L"the driver can't play DSD at " + std::to_wstring(rate) + L" bits a second";
            return false;
        }
    } else if (!(current >= 8000 && current <= 768000)) {
        // A driver that hasn't been given a rate yet can report 0.
        for (double r : {48000.0, 44100.0})
            if (driver_->canSampleRate(r) == asio::kOk && driver_->setSampleRate(r) == asio::kOk) {
                current = r;
                break;
            }
        if (!(current >= 8000 && current <= 768000)) {
            error = L"the driver has no usable sample rate";
            return false;
        }
    }
    rate_ = (unsigned)std::lround(current);

    long minSize = 0, maxSize = 0, preferred = 0, granularity = 0;
    if (driver_->getBufferSize(&minSize, &maxSize, &preferred, &granularity) != asio::kOk) {
        error = L"the driver reported no buffer size";
        return false;
    }
    // The size set in the driver's own panel, which is where users choose their latency.
    long size = preferred > 0 ? preferred : std::max(minSize, maxSize);
    if (size <= 0 || size > 65536) {
        error = L"the driver reported no usable buffer size";
        return false;
    }

    long type = -1;
    for (unsigned ch = 0; ch < outputs_; ++ch) {
        asio::ChannelInfo info{};
        info.channel = (long)ch;
        info.isInput = 0;
        if (driver_->getChannelInfo(&info) != asio::kOk) {
            error = L"the driver didn't describe its outputs";
            return false;
        }
        if (ch == 0) type = info.type;
        else if (info.type != type) {
            if (dsd != AsioDsd::None) {
                error = L"the driver's two outputs have different sample types";
                return false;
            }
            outputs_ = 1;  // a pair of mixed types: play mono on the first
        }
    }
    // The type has to be one this host can write, and one the requested transport can carry.
    if (!AsioTypeSupported(type) || (dsd == AsioDsd::None && AsioTypeIsDsd(type)) ||
        (dsd == AsioDsd::Dop && !AsioTypeCarriesDop(type)) ||
        (dsd == AsioDsd::Native && type != asio::kDsdInt8MSB1 && type != asio::kDsdInt8LSB1)) {
        error = std::wstring(L"the driver takes ") + AsioTypeName(type) + L" samples";
        return false;
    }
    type_ = type;
    dsd_ = dsd;
    dsdLsb_ = type == asio::kDsdInt8LSB1;
    dopMarker_ = kDopMarkerA;
    bufferFrames_ = (unsigned)size;
    stereo_.assign((size_t)size * 2, 0.0f);
    scratch_.assign((size_t)size, 0.0f);
    for (unsigned ch = 0; ch < outputs_; ++ch) infos_[ch] = asio::BufferInfo{0, (long)ch, {nullptr, nullptr}};

    // Some drivers send messages from inside createBuffers, so the callbacks must find this stream first.
    g_active.store(this, std::memory_order_release);
    if (driver_->createBuffers(infos_, (long)outputs_, size, &AsioCallbacks::table) != asio::kOk) {
        error = L"the driver couldn't make its buffers";
        return false;
    }
    buffers_ = true;
    // A native DSD buffer is counted in DSD samples (bits), so it holds an eighth as many bytes.
    const size_t dsdBytes = ((size_t)size + 7) / 8;
    for (unsigned half = 0; half < 2; ++half) {
        void* left = infos_[0].buffers[half];
        void* right = infos_[1].buffers[half];
        if (dsd_ == AsioDsd::Native) {
            if (left) memset(left, kDsdIdle, dsdBytes);
            if (right) memset(right, kDsdIdle, dsdBytes);
        } else if (dsd_ == AsioDsd::Dop && left && right) {
            // The driver plays one half before the first buffer switch: DoP idle, not zeros, or a DoP DAC
            // would drop out of DSD mode. The marker carries on into the first buffer switch.
            WriteAsioDop(stereo_.data(), (size_t)size, 0, type_, dopMarker_, left, right);
        } else {
            if (left) memset(left, 0, (size_t)size * AsioSampleBytes(type_));
            if (right) memset(right, 0, (size_t)size * AsioSampleBytes(type_));
        }
    }
    long inLatency = 0, outLatency = 0;
    driver_->getLatencies(&inLatency, &outLatency);
    latency_ = (unsigned)std::max<long>(outLatency, size);
    // In native DSD the driver counts DSD samples (bits) while the render counts payload bytes.
    if (dsd_ == AsioDsd::Native) latency_ = (latency_ + 7) / 8;
    // A driver that supports outputReady plays a buffer as soon as it is filled, saving one buffer of
    // latency; asking is how a host finds out.
    outputReady_ = driver_->outputReady() == asio::kOk;
    running_ = true;  // before start(): the first buffer switch can come before start() returns
    if (driver_->start() != asio::kOk) {
        running_ = false;
        error = L"the driver didn't start playing";
        return false;
    }
    return true;
}

void AsioStream::Teardown() {
    if (!driver_) return;
    if (running_) driver_->stop();
    running_ = false;
    if (buffers_) driver_->disposeBuffers();
    buffers_ = false;
    // Back to PCM, which has to happen with no buffers: the next stream may be a PCM one.
    if (dsd_ != AsioDsd::None) SetIoFormat(driver_, asio::kPcmFormat);
    dsd_ = AsioDsd::None;
}

void AsioStream::Unload() {
    Teardown();
    if (driver_) driver_->Release();
    driver_ = nullptr;
    AsioStream* self = this;
    g_active.compare_exchange_strong(self, nullptr);
}

void AsioStream::Switch(long index) {
    if (!running_ || (index != 0 && index != 1)) return;
    // Native DSD buffers are counted in DSD samples (bits) while one payload float carries 8 of them, so
    // a buffer takes an eighth as many frames as it has samples.
    const bool native = dsd_ == AsioDsd::Native;
    const size_t frames = native ? (size_t)(bufferFrames_ + 7) / 8 : bufferFrames_;
    size_t supplied = 0;
    if (render_) supplied = render_(stereo_.data(), frames);
    else std::fill(stereo_.begin(), stereo_.begin() + (ptrdiff_t)frames * 2, 0.0f);
    if (outputs_ == 1) {
        // One output: both channels, at half level so a full-scale pair can't clip. DSD needs two, so
        // this is always PCM.
        for (size_t i = 0; i < frames; ++i) stereo_[i] = 0.5f * (stereo_[i * 2] + stereo_[i * 2 + 1]);
        WriteAsioChannel(stereo_.data(), frames, 1, 0, type_, infos_[0].buffers[index], scratch_.data());
    } else if (native) {
        WriteAsioDsd(stereo_.data(), frames, supplied, dsdLsb_, infos_[0].buffers[index],
                     infos_[1].buffers[index]);
    } else if (dsd_ == AsioDsd::Dop) {
        WriteAsioDop(stereo_.data(), frames, supplied, type_, dopMarker_, infos_[0].buffers[index],
                     infos_[1].buffers[index]);
    } else {
        for (unsigned ch = 0; ch < outputs_; ++ch)
            WriteAsioChannel(stereo_.data(), frames, 2, ch, type_, infos_[ch].buffers[index], scratch_.data());
    }
    if (outputReady_) driver_->outputReady();
}

#endif  // _WIN32
