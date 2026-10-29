// Recognizes named outputs and talks directly to supported USB headsets using Windows HID.
// BlackShark wire facts: https://github.com/openrazer/openrazer/pull/2862 (1125585244f3).
// Only explicit commands change persistent hardware settings; discovery queries status alone.
#include "headsets.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <initguid.h>
#include <devpkey.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cwctype>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

std::wstring RecognizeHeadphones(std::wstring_view name) {
    std::wstring folded;
    for (wchar_t c : name) if (iswalnum(c)) folded += (wchar_t)towlower(c);
    // Require a model name, never infer a headphone model from a generic DAC or virtual output.
    struct Model { const wchar_t* match; const wchar_t* name; };
    constexpr Model models[] = {
        {L"blacksharkv2pro", L"Razer BlackShark V2 Pro"},
        {L"blacksharkv2hyperspeed", L"Razer BlackShark V2 HyperSpeed"},
        {L"wh1000xm4", L"Sony WH-1000XM4"}, {L"wh1000xm5", L"Sony WH-1000XM5"},
        {L"arctisnova7", L"SteelSeries Arctis Nova 7 family"},
        {L"arctisnovapro", L"SteelSeries Arctis Nova Pro family"},
        {L"hyperxcloudalpha", L"HyperX Cloud Alpha family"},
        {L"hyperxcloudiii", L"HyperX Cloud III family"}, {L"hyperxcloudii", L"HyperX Cloud II family"},
        {L"prox2lightspeed", L"Logitech PRO X 2 LIGHTSPEED"},
    };
    for (const auto& model : models) if (folded.find(model.match) != folded.npos) return model.name;
    return {};
}

std::wstring OutputProfileSection(std::wstring_view id) {
    // Stable across processes and stdlib versions. The full id is also checked before loading,
    // so even a hash collision cannot apply another device's sound settings.
    uint64_t hash = 14695981039346656037ull;
    for (wchar_t c : id) { hash ^= (uint16_t)c; hash *= 1099511628211ull; }
    wchar_t section[40];
    swprintf_s(section, L"OutputEQ-%016llx", (unsigned long long)hash);
    return section;
}

std::array<uint8_t, 64> BlackSharkReport(uint8_t type, uint8_t command, int value) {
    std::array<uint8_t, 64> report{};
    report[0] = 2; report[1] = 0x80; report[5] = 'P'; report[6] = 'A';
    report[7] = type == 2 ? 0x0e : 8;
    report[9] = type; report[10] = command;
    if (type == 2) { report[2] = 7; report[11] = (uint8_t)value; }
    else {
        report[2] = value < 0 ? 8 : 9;
        report[12] = value < 0 ? 0 : 1;
        if (value >= 0) report[13] = (uint8_t)value;
    }
    return report;
}

bool BlackSharkReply(const uint8_t* report, size_t size, uint8_t command, uint8_t& value) {
    return BlackSharkReply(report, size, command, &value, 1);
}

bool BlackSharkReply(const uint8_t* report, size_t size, uint8_t command, uint8_t* values, size_t count) {
    if (!report || !values || !count || size < 15 || report[0] != 2 || report[12] != command || report[13] != 1 ||
        report[14] < count || report[14] > size - 15) return false;
    std::copy_n(report + 15, count, values);
    return true;
}

bool ValidHeadsetChanges(const HeadsetChanges& c) {
    return c.sidetone >= -1 && c.sidetone <= 10 && c.dnd >= -1 && c.dnd <= 1 &&
           (c.idleMinutes == -1 || c.idleMinutes == 0 || (c.idleMinutes >= 15 && c.idleMinutes <= 60)) &&
           (c.preset == -1 || c.preset == 7 || c.preset == 8 || c.preset == 9 ||
            (c.preset >= 250 && c.preset <= 255));
}

namespace {
struct Endpoint { HeadsetInfo info; GUID container{}; };
Endpoint DefaultOutput(IMMDeviceEnumerator* enumerator) {
    Endpoint out;
    ComPtr<IMMDevice> device;
    if (!enumerator || FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) return out;
    LPWSTR id = nullptr;
    if (SUCCEEDED(device->GetId(&id))) { out.info.id = id; CoTaskMemFree(id); }
    ComPtr<IPropertyStore> properties;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties))) {
        PROPVARIANT value{};
        if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR)
            out.info.name = value.pwszVal;
        PropVariantClear(&value);
        if (SUCCEEDED(properties->GetValue(PKEY_Device_ContainerId, &value)) && value.vt == VT_CLSID && value.puuid)
            out.container = *value.puuid;
        PropVariantClear(&value);
    }
    out.info.model = RecognizeHeadphones(out.info.name);
    return out;
}

class HidLink {
public:
    ~HidLink() { if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }
    bool Open(const GUID& container) {
        if (container == GUID_NULL) return false;
        GUID hid;
        HidD_GetHidGuid(&hid);
        HDEVINFO devices = SetupDiGetClassDevsW(&hid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (devices == INVALID_HANDLE_VALUE) return false;
        SP_DEVICE_INTERFACE_DATA iface{sizeof(iface)};
        for (DWORD index = 0; SetupDiEnumDeviceInterfaces(devices, nullptr, &hid, index, &iface); ++index) {
            DWORD needed = 0;
            SetupDiGetDeviceInterfaceDetailW(devices, &iface, nullptr, 0, &needed, nullptr);
            if (needed < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
            std::vector<uint8_t> bytes(needed);
            auto* detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)bytes.data();
            detail->cbSize = sizeof(*detail);
            SP_DEVINFO_DATA device{sizeof(device)};
            if (!SetupDiGetDeviceInterfaceDetailW(devices, &iface, detail, needed, nullptr, &device)) continue;
            std::wstring path = detail->DevicePath;
            std::transform(path.begin(), path.end(), path.begin(), towlower);
            if (path.find(L"&mi_03") == path.npos) continue;
            GUID candidate{};
            DEVPROPTYPE type = 0;
            if (!SetupDiGetDevicePropertyW(devices, &device, &DEVPKEY_Device_ContainerId, &type,
                    (PBYTE)&candidate, sizeof(candidate), nullptr, 0) || type != DEVPROP_TYPE_GUID || candidate != container) continue;
            HANDLE handle = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            if (handle == INVALID_HANDLE_VALUE) continue;
            HIDD_ATTRIBUTES attrs{sizeof(attrs)};
            PHIDP_PREPARSED_DATA data = nullptr;
            HIDP_CAPS caps{};
            bool matched = HidD_GetAttributes(handle, &attrs) && attrs.VendorID == 0x1532 && attrs.ProductID == 0x0555 &&
                HidD_GetPreparsedData(handle, &data);
            if (matched) matched = HidP_GetCaps(data, &caps) == HIDP_STATUS_SUCCESS && caps.UsagePage == 0xff00 &&
                caps.Usage == 1 && caps.InputReportByteLength == 64 && caps.OutputReportByteLength == 64;
            if (data) HidD_FreePreparsedData(data);
            if (matched) { handle_ = handle; break; }
            CloseHandle(handle);
        }
        SetupDiDestroyDeviceInfoList(devices);
        return handle_ != INVALID_HANDLE_VALUE;
    }

    bool Transfer(bool write, uint8_t* bytes, DWORD& size, DWORD timeout) {
        OVERLAPPED operation{};
        operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!operation.hEvent) return false;
        BOOL ok = write ? WriteFile(handle_, bytes, size, &size, &operation) : ReadFile(handle_, bytes, size, &size, &operation);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(operation.hEvent, timeout) != WAIT_OBJECT_0) {
                CancelIoEx(handle_, &operation);
                GetOverlappedResult(handle_, &operation, &size, TRUE);
                ok = FALSE;
            } else ok = GetOverlappedResult(handle_, &operation, &size, FALSE);
        }
        CloseHandle(operation.hEvent);
        return ok != FALSE;
    }
    bool Send(std::array<uint8_t, 64> report) {
        DWORD size = 64;
        return Transfer(true, report.data(), size, 300) && size == 64;
    }
    bool Remote(bool on) {
        bool ok = Send(BlackSharkReport(2, 0xe1, on));
        Sleep(35);  // wake a dozing radio link before the next command
        return ok;
    }
    bool Query(uint8_t command, uint8_t& value) {
        return Query(command, &value, 1);
    }
    bool Query(uint8_t command, uint8_t* values, size_t count) {
        bool ok = false;
        Remote(true);
        for (int attempt = 0; attempt < 3 && !ok; ++attempt) {
            if (!Remote(true)) break;
            HidD_FlushQueue(handle_);
            if (!Send(BlackSharkReport(3, command))) break;
            ULONGLONG deadline = GetTickCount64() + 180;
            for (;;) {
                ULONGLONG now = GetTickCount64();
                if (now >= deadline) break;
                std::array<uint8_t, 64> report;
                DWORD size = 64;
                if (!Transfer(false, report.data(), size, (DWORD)(deadline - now))) break;
                if (BlackSharkReply(report.data(), size, command, values, count)) { ok = true; break; }
            }
        }
        Remote(false);
        return ok;
    }
    bool Set(uint8_t command, uint8_t value) {
        bool ok = Remote(true) && Remote(true) && Send(BlackSharkReport(4, command, value));
        Remote(false);
        return ok;
    }
    bool Preset(int preset) {
        // Selector only: writing bands here could overwrite the currently selected custom slot.
        for (int attempt = 0; attempt < 4; ++attempt) {
            if (!Remote(true) || !Remote(true) || !Send(BlackSharkReport(3, 0x1e)) ||
                !Remote(true) || !Send(BlackSharkReport(4, 0x93, preset)) ||
                !Remote(true) || !Send(BlackSharkReport(4, 0x9d, preset >= 250 && preset < 255 ? 2 : 1)) ||
                !Remote(true)) { Remote(false); return false; }
            // Finish remote-on as in the captured apply sequence; the following query closes remote mode.
            Sleep(100);  // cross-family preset selection commits after the command sequence settles
            uint8_t current = 0;
            if (Query(0x13, current) && current == preset) return true;
        }
        return false;
    }
private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

void ReadHardware(HidLink& link, HeadsetInfo& info) {
    uint8_t value = 0;
    if (!link.Query(0x21, value) || value > 100) {
        info.status = L"Headset did not reply. Check its power and wireless connection, then Refresh.";
        return;
    }
    info.battery = value;
    if (link.Query(0x2a, value)) info.charging = value != 0;
    if (link.Query(0x18, value)) {
        if (!value) info.sidetone = 0;
        else if (link.Query(0x19, value) && value <= 10) info.sidetone = value;
    }
    if (link.Query(0x2c, value) && (value == 0 || (value >= 15 && value <= 60))) info.idleMinutes = value;
    if (link.Query(0x27, value) && value <= 1) info.dnd = value;
    if (link.Query(0x13, value)) info.preset = value;
    std::array<uint8_t, 10> bands{};
    info.eqKnown = link.Query(0x15, bands.data(), bands.size());
    if (info.eqKnown) for (size_t i = 0; i < bands.size(); ++i) info.eqBands[i] = (int8_t)bands[i];
    info.status = L"USB hardware connected. Changes affect the headset in every application.";
}

bool ApplyHardware(HidLink& link, const HeadsetChanges& changes) {
    if (changes.sidetone >= 0) {
        if (changes.sidetone && !link.Set(0x99, (uint8_t)changes.sidetone)) return false;
        if (!link.Set(0x98, changes.sidetone != 0)) return false;
    }
    if (changes.idleMinutes >= 0 && !link.Set(0xac, (uint8_t)changes.idleMinutes)) return false;
    if (changes.dnd >= 0 && !link.Set(0xa7, (uint8_t)changes.dnd)) return false;
    return changes.preset < 0 || link.Preset(changes.preset);
}
}

struct HeadsetController::Impl {
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;
    bool quit = false, refresh = false;
    HeadsetInfo info;
    std::optional<std::pair<std::wstring, HeadsetChanges>> pending;

    void Run() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ComPtr<IMMDeviceEnumerator> enumerator;
        CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&enumerator));
        for (;;) {
            bool detailed;
            std::optional<std::pair<std::wstring, HeadsetChanges>> command;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (quit) break;
                detailed = std::exchange(refresh, false);
                command = std::move(pending);
                pending.reset();
            }
            Endpoint endpoint = DefaultOutput(enumerator.Get());
            HeadsetInfo next = endpoint.info;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (next.id == info.id) {
                    auto name = next.name, model = next.model;
                    next = info;
                    next.name = name; next.model = model;
                    if (next.hardware) next.model = L"Razer BlackShark V2 Pro 2.4 (USB)";
                }
            }
            if (detailed || command) {
                ++next.revision;
                next.battery = next.charging = next.sidetone = next.idleMinutes = next.dnd = next.preset = -1;
                next.eqKnown = false;
                HidLink link;
                next.hardware = link.Open(endpoint.container);
                if (next.hardware) {
                    next.model = L"Razer BlackShark V2 Pro 2.4 (USB)";
                    bool applied = true;
                    if (command) applied = command->first == next.id && ApplyHardware(link, command->second);
                    ReadHardware(link, next);
                    if (command) {
                        const auto& c = command->second;
                        bool verified = applied && (c.sidetone < 0 || c.sidetone == next.sidetone) &&
                            (c.idleMinutes < 0 || c.idleMinutes == next.idleMinutes) &&
                            (c.dnd < 0 || c.dnd == next.dnd) &&
                            (c.preset < 0 || (c.preset == next.preset && next.eqKnown));
                        next.status = verified ? L"Hardware settings applied and read back from the headset." :
                            L"Couldn't verify every setting. Refresh before retrying; some changes may have applied.";
                    }
                } else next.status = L"Native controls aren't available for this output. Cordinal EQ and crossfeed are available.";
            }
            {
                std::unique_lock<std::mutex> lock(mutex);
                next.busy = pending.has_value() || refresh;
                info = std::move(next);
                wake.wait_for(lock, std::chrono::seconds(2), [&] { return quit || refresh || pending.has_value(); });
            }
        }
        enumerator.Reset();
        CoUninitialize();
    }
};

HeadsetController::HeadsetController() : impl_(std::make_unique<Impl>()) {}
HeadsetController::~HeadsetController() { Stop(); }
void HeadsetController::Start() {
    if (impl_->worker.joinable()) return;
    impl_->quit = false;
    impl_->worker = std::thread([this] { impl_->Run(); });
}
void HeadsetController::Stop() {
    { std::lock_guard<std::mutex> lock(impl_->mutex); impl_->quit = true; }
    impl_->wake.notify_all();
    if (impl_->worker.joinable()) impl_->worker.join();
}
void HeadsetController::Refresh() {
    { std::lock_guard<std::mutex> lock(impl_->mutex); impl_->refresh = true; impl_->info.busy = true; }
    impl_->wake.notify_all();
}
HeadsetInfo HeadsetController::Snapshot() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->info;
}
bool HeadsetController::Apply(const std::wstring& id, const HeadsetChanges& changes) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (id.empty() || id != impl_->info.id || !impl_->info.hardware || impl_->info.busy || !ValidHeadsetChanges(changes)) return false;
    impl_->pending = std::make_pair(id, changes);
    impl_->info.busy = true;
    impl_->wake.notify_all();
    return true;
}
