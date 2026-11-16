// ASIO output, written from the published driver ABI rather than Steinberg's SDK: finding installed
// drivers in the registry, the IASIO interface every driver implements, converting the player's stereo
// float into a driver's sample type, and AsioStream, which loads one driver on a thread of its own and
// feeds it from its buffer-switch callback. AudioOutput (audio_output.cpp) uses it as its third mode
// next to WASAPI shared and exclusive. Windows only; Linux plays through PipeWire instead.
#pragma once
#ifdef _WIN32
#include <windows.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// --- The driver ABI. A driver is an in-process COM object registered under HKLM\SOFTWARE\ASIO\<name>
// with a CLSID value; it is created with that CLSID as both class and interface id. Its methods use
// the compiler's default member calling convention and its callbacks plain C functions, as the
// drivers themselves are built.
namespace asio {

using Bool = long;
using Error = long;
using SampleRate = double;
struct Samples { unsigned long hi, lo; };  // 64-bit counts split in two, as the ABI stores them

enum : Error {
    kOk = 0,                  // the call succeeded
    kSuccess = 0x3F4847A0,    // future(): the selector is supported and done
    kNotPresent = -1000,      // no hardware input or output
    kHwMalfunction = -999,
    kInvalidParameter = -998,
    kInvalidMode = -997,
    kSpNotAdvancing = -996,   // the sample clock isn't running
    kNoClock = -995,
    kNoMemory = -994,
};

// Sample types a channel reports. MSB types are big-endian (old Macs) and are refused.
enum SampleType : long {
    kInt16LSB = 16, kInt24LSB = 17, kInt32LSB = 18, kFloat32LSB = 19, kFloat64LSB = 20,
    kInt32LSB16 = 24, kInt32LSB18 = 25, kInt32LSB20 = 26, kInt32LSB24 = 27,
};

// asioMessage selectors (the host answers 1 for "handled" or "supported").
enum : long {
    kSelectorSupported = 1, kEngineVersion = 2, kResetRequest = 3, kBufferSizeChange = 4, kResyncRequest = 5,
    kLatenciesChanged = 6, kSupportsTimeInfo = 7, kSupportsTimeCode = 8, kOverload = 15,
};

struct ChannelInfo {
    long channel;
    Bool isInput;
    Bool isActive;
    long channelGroup;
    long type;  // SampleType
    char name[32];
};

struct BufferInfo {
    Bool isInput;
    long channelNum;
    void* buffers[2];  // the double buffer: the driver plays one half while the host fills the other
};

struct Callbacks {
    void (*bufferSwitch)(long doubleBufferIndex, Bool directProcess);
    void (*sampleRateDidChange)(SampleRate rate);
    long (*asioMessage)(long selector, long value, void* message, double* opt);
    void* (*bufferSwitchTimeInfo)(void* timeInfo, long doubleBufferIndex, Bool directProcess);
};

struct ClockSource;  // only passed through, never read

// The vtable, in the order drivers lay it out after IUnknown.
struct IAsio : IUnknown {
    virtual Bool init(void* sysHandle) = 0;
    virtual void getDriverName(char* name) = 0;  // up to 32 bytes
    virtual long getDriverVersion() = 0;
    virtual void getErrorMessage(char* text) = 0;  // up to 124 bytes
    virtual Error start() = 0;
    virtual Error stop() = 0;
    virtual Error getChannels(long* inputs, long* outputs) = 0;
    virtual Error getLatencies(long* input, long* output) = 0;
    virtual Error getBufferSize(long* minSize, long* maxSize, long* preferred, long* granularity) = 0;
    virtual Error canSampleRate(SampleRate rate) = 0;
    virtual Error getSampleRate(SampleRate* rate) = 0;
    virtual Error setSampleRate(SampleRate rate) = 0;
    virtual Error getClockSources(ClockSource* clocks, long* count) = 0;
    virtual Error setClockSource(long reference) = 0;
    virtual Error getSamplePosition(Samples* position, Samples* timeStamp) = 0;
    virtual Error getChannelInfo(ChannelInfo* info) = 0;
    virtual Error createBuffers(BufferInfo* infos, long channels, long bufferSize, Callbacks* callbacks) = 0;
    virtual Error disposeBuffers() = 0;
    virtual Error controlPanel() = 0;
    virtual Error future(long selector, void* opt) = 0;
    virtual Error outputReady() = 0;
};

}  // namespace asio

// --- Finding drivers.
struct AsioDriverInfo {
    std::wstring name;  // the registry key's name, which is what the user picks and settings.ini keeps
    std::wstring description;
    CLSID clsid{};
};

// "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}" in hex, either case; anything else is false. Parsed here
// rather than by CLSIDFromString, which also accepts ProgIDs and looks them up.
bool ParseClsid(std::wstring_view text, CLSID& out);
// One registry entry to a driver, or false when its CLSID value is missing or malformed.
bool ParseAsioDriver(std::wstring_view keyName, std::wstring_view clsidValue, std::wstring_view description,
                     AsioDriverInfo& out);
// The installed drivers, by name. Entries whose class isn't registered as an in-process server for
// this (64-bit) process are left out: a 32-bit-only driver can't load here.
std::vector<AsioDriverInfo> ListAsioDrivers();

// --- Sample conversion.
bool AsioTypeSupported(long type);
int AsioSampleBytes(long type);  // bytes per sample in the driver's buffer; 0 when unsupported
int AsioSampleBits(long type);   // significant bits (32 for float)
bool AsioTypeIsFloat(long type);
const wchar_t* AsioTypeName(long type);
// Channel `channel` of `frames` interleaved frames of `channels` floats into one driver buffer of
// `type`. `scratch` holds at least `frames` floats. Integer types round exactly for samples that came
// from integers of that width and dither otherwise, as ConvertSamples does for WASAPI.
void WriteAsioChannel(const float* interleaved, size_t frames, unsigned channels, unsigned channel, long type,
                      void* out, float* scratch);

// --- One loaded driver, playing.
// The driver lives on a single-threaded-apartment thread of its own that also pumps its window
// messages: drivers are apartment threaded, and some show their panel or run timers through windows.
// Every driver call is made on that thread; only the buffer switch runs on the driver's own thread.
class AsioStream {
public:
    // Fills `frames` interleaved stereo frames. Runs on the driver's real-time thread: it must not lock,
    // allocate or block.
    using RenderFn = std::function<void(float* stereo, size_t frames)>;
    // The driver asked to be set up again (a reset request, a new buffer size, a new rate).
    using ResetFn = std::function<void()>;

    AsioStream();
    ~AsioStream();  // stops and unloads the driver
    AsioStream(const AsioStream&) = delete;
    AsioStream& operator=(const AsioStream&) = delete;

    // Loads `driver` (when not already loaded), runs it at `rate` (0 keeps the driver's own), and starts
    // it. False with `error` set when it can't play; the driver is then unloaded.
    bool Open(const AsioDriverInfo& driver, unsigned rate, RenderFn render, ResetFn reset, std::wstring& error);
    // Sets the stream up again at `rate` without reloading the driver. False like Open.
    bool Restart(unsigned rate, std::wstring& error);
    // Opens the driver's own settings window (buffer size, clock). Returns at once; the window runs on
    // the driver's thread.
    void ShowPanel();

    unsigned Rate() const { return rate_; }
    long Type() const { return type_; }
    unsigned BufferFrames() const { return bufferFrames_; }
    // Frames between the buffer being filled and the speakers: the driver's output latency, at least
    // one buffer.
    unsigned Latency() const { return latency_; }
    const std::wstring& DriverName() const { return driverName_; }
    // A rate the driver can't do is replaced by its own; this says so (empty otherwise).
    const std::wstring& Problem() const { return problem_; }
    const CLSID& Clsid() const { return clsid_; }

private:
    struct Thread;
    void Call(const std::function<void()>& work);  // runs `work` on the driver's thread and waits
    bool Setup(unsigned rate, std::wstring& error);  // on the driver's thread
    void Teardown();                                 // on the driver's thread
    void Unload();                                   // on the driver's thread
    void Switch(long index);                         // the driver's real-time thread
    friend struct AsioCallbacks;

    std::unique_ptr<Thread> thread_;
    asio::IAsio* driver_ = nullptr;
    CLSID clsid_{};
    HWND window_ = nullptr;
    std::atomic<bool> running_{false};  // read by the buffer switch on the driver's thread
    bool buffers_ = false, outputReady_ = false;
    RenderFn render_;
    ResetFn reset_;
    asio::BufferInfo infos_[2]{};
    unsigned outputs_ = 0;  // 1 or 2 driver channels
    std::vector<float> stereo_, scratch_;
    std::atomic<unsigned> rate_{0};
    long type_ = 0;
    unsigned bufferFrames_ = 0, latency_ = 0;
    std::wstring driverName_, problem_;
};

#endif  // _WIN32
