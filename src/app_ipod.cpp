// The Sync to iPod window: finds mounted classic iPods, shows what will be copied, and runs the
// database work on a worker. The window closes when synchronization starts; progress and the
// result arrive on the main window as WM_APP_IPOD, like the copy and convert windows.
#include "app.h"
#include "ipod.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <exception>
#include <memory>
#include <string_view>
#include <thread>

namespace app {
namespace {

const wchar_t kIpodClass[] = L"CordinalIpod";
enum { kIdDevice = 100, kIdRefresh, kIdInitialize, kIdSummary, kIdCount, kIdSync, kIdClose };

struct IpodWindow {
    HWND hwnd = nullptr, device = nullptr, refresh = nullptr, initialize = nullptr, summary = nullptr,
         count = nullptr, sync = nullptr, close = nullptr;
    std::vector<ipod::Device> devices;
    bool scanning = false;
};

struct ScanResult {
    std::vector<ipod::Device> devices;
    std::wstring error;
};

struct Progress {
    size_t done = 0, total = 0;
    std::wstring title;
};

IpodWindow w;
std::thread g_worker;
std::atomic<bool> g_cancel{false};
std::atomic<bool> g_scanning{false}, g_syncing{false};
std::vector<TrackId> g_ids;

std::wstring Size(uint64_t bytes) {
    wchar_t text[32];
    if (bytes >= 1000000000ull) swprintf_s(text, L"%.1f GB", bytes / 1e9);
    else if (bytes >= 1000000ull) swprintf_s(text, L"%.0f MB", bytes / 1e6);
    else swprintf_s(text, L"%.0f KB", bytes / 1e3);
    return text;
}

std::wstring DeviceText(const ipod::Device& device) {
    std::wstring text = device.name;
    if (!text.empty()) text += L"  ";
    text += device.mountpoint.wstring();
    return text;
}

int SelectedDevice() {
    const int index = (int)SendMessageW(w.device, CB_GETCURSEL, 0, 0);
    return index >= 0 && index < (int)w.devices.size() ? index : -1;
}

void UpdateDetails() {
    if (!w.hwnd) return;
    const int index = SelectedDevice();
    EnableWindow(w.sync, index >= 0 && !g_scanning && !g_syncing);
    if (index < 0) {
        SetWindowTextW(w.summary, w.devices.empty() ? L"No classic iPod found. Connect one and choose Refresh."
                                                    : L"Choose an iPod.");
        SetWindowTextW(w.count, L"");
        EnableWindow(w.initialize, FALSE);
        return;
    }
    const ipod::Device& device = w.devices[(size_t)index];
    EnableWindow(w.sync, device.classic && !g_scanning && !g_syncing);
    std::wstring text = FromUtf8(device.model);
    if (device.capacity) text += L"  \x2022  " + Size(device.free) + L" free of " + Size(device.capacity);
    text += device.initialized ? L"  \x2022  iTunes database found" : L"  \x2022  empty (no iTunes database yet)";
    if (!device.classic) text += L"  \x2022  iOS device: unsupported";
    SetWindowTextW(w.summary, text.c_str());
    EnableWindow(w.initialize, !device.initialized && device.classic);
    const size_t count = g_ids.size();
    uint64_t bytes = 0;
    size_t unavailable = 0;
    for (TrackId id : g_ids) {
        if (!g_lib.Contains(id)) continue;
        const Track& track = g_lib[id];
        bytes += track.fileSize;
        if (track.flags & Track::kMissing) ++unavailable;
    }
    std::wstring summary = std::to_wstring(count) + (count == 1 ? L" song" : L" songs") + L" selected";
    if (bytes) summary += L", " + Size(bytes);
    if (unavailable) summary += L"  \x2022  " + std::to_wstring(unavailable) + L" missing";
    SetWindowTextW(w.count, summary.c_str());
}

void LayoutIpod() {
    if (!w.hwnd) return;
    RECT rc;
    GetClientRect(w.hwnd, &rc);
    const int pad = S(14), label = S(24);
    int y = pad;
    SetWindowPos(w.device, nullptr, pad, y, rc.right - pad * 2 - S(96), S(220), SWP_NOZORDER);
    SetWindowPos(w.refresh, nullptr, rc.right - pad - S(88), y - S(1), S(88), S(26), SWP_NOZORDER);
    y += S(34);
    SetWindowPos(w.summary, nullptr, pad, y, rc.right - pad * 2, label, SWP_NOZORDER);
    y += label + S(4);
    SetWindowPos(w.initialize, nullptr, pad, y, rc.right - pad * 2, S(24), SWP_NOZORDER);
    y += S(34);
    SetWindowPos(w.count, nullptr, pad, y, rc.right - pad * 2, label, SWP_NOZORDER);
    const int bottom = rc.bottom - pad - S(30);
    SetWindowPos(w.sync, nullptr, rc.right - pad - S(216), bottom, S(108), S(30), SWP_NOZORDER);
    SetWindowPos(w.close, nullptr, rc.right - pad - S(100), bottom, S(100), S(30), SWP_NOZORDER);
}

void BeginScan() {
    if (g_scanning || g_syncing) return;
    if (g_worker.joinable()) g_worker.join();
    const HWND target = g.hwnd;
    g_scanning = true;
    w.scanning = true;
    if (w.hwnd) {
        EnableWindow(w.refresh, FALSE);
        EnableWindow(w.sync, FALSE);
        SetWindowTextW(w.summary, L"Looking for iPods...");
        SetWindowTextW(w.count, L"");
    }
    g_worker = std::thread([target] {
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        auto* result = new ScanResult;
        try {
            result->devices = ipod::Detect();
        } catch (const std::exception& exception) {
            result->error = FromUtf8(exception.what());
        }
        if (!PostMessageW(target, WM_APP_IPOD, 1, (LPARAM)result)) delete result;
    });
}

void PopulateDevices(ScanResult& result) {
    w.devices = std::move(result.devices);
    if (w.hwnd) {
        SendMessageW(w.device, CB_RESETCONTENT, 0, 0);
        for (const ipod::Device& device : w.devices) {
            const std::wstring text = DeviceText(device);
            SendMessageW(w.device, CB_ADDSTRING, 0, (LPARAM)text.c_str());
        }
        const std::wstring saved = IniRead(L"iPod", L"Mount", L"");
        int selected = w.devices.empty() ? -1 : 0;
        for (int i = 0; i < (int)w.devices.size(); ++i)
            if (w.devices[(size_t)i].mountpoint.wstring() == saved) selected = i;
        if (selected >= 0) SendMessageW(w.device, CB_SETCURSEL, selected, 0);
        EnableWindow(w.refresh, TRUE);
        w.scanning = false;
        UpdateDetails();
        if (w.hwnd && !result.error.empty())
            MessageBoxW(w.hwnd, result.error.c_str(), L"iPod search", MB_OK | MB_ICONWARNING);
    }
    g_scanning = false;
    if (w.hwnd) w.scanning = false;
}

std::vector<ipod::SourceTrack> CaptureTracks() {
    std::vector<ipod::SourceTrack> tracks;
    tracks.reserve(g_ids.size());
    for (TrackId id : g_ids) {
        if (!g_lib.Contains(id)) continue;
        const Track& track = g_lib[id];
        ipod::SourceTrack item;
        item.path = g_lib.Path(id);
        item.title = ToUtf8(track.title ? g_lib.Str(track.title) : StemOf(g_lib.Str(track.file)));
        item.artist = ToUtf8(track.artist ? g_lib.Str(track.artist) : L"");
        item.album = ToUtf8(track.album ? g_lib.Str(track.album) : L"");
        item.albumArtist = ToUtf8(track.albumArtist ? g_lib.Str(track.albumArtist) : L"");
        item.genre = ToUtf8(track.genre ? g_lib.Str(track.genre) : L"");
        item.composer = ToUtf8(track.composer ? g_lib.Str(track.composer) : L"");
        item.size = track.fileSize;
        item.durationMs = track.durationMs;
        const std::string_view date = g_lib.View(track.date);
        if (date.size() >= 4)
            item.year = (uint32_t)std::clamp(std::atoi(std::string(date.substr(0, 4)).c_str()), 0, 9999);
        item.trackNo = track.trackNo;
        item.discNo = track.disc;
        item.bitrate = track.bitrate;
        item.sampleRate = (uint16_t)std::min<uint32_t>(track.sampleRate, 65535);
        item.playCount = track.playCount;
        item.skipCount = track.skipCount;
        item.rating = track.rating;
        item.missing = (track.flags & Track::kMissing) != 0;
        item.protectedFile = ipod::IsProtectedAudio(item.path);
        const std::wstring extension = ExtOf(item.path);
        item.audiobook = extension == L"m4b";
        tracks.push_back(std::move(item));
    }
    return tracks;
}

void StartSync() {
    if (g_scanning || g_syncing) return;
    const int index = SelectedDevice();
    if (index < 0) {
        MessageBoxW(w.hwnd, L"Choose an iPod first.", kAppName, MB_OK | MB_ICONINFORMATION);
        return;
    }
    const ipod::Device device = w.devices[(size_t)index];
    if (!device.classic) {
        MessageBoxW(w.hwnd, L"iOS devices need their own music synchronization and are not supported by this window.",
                    kAppName, MB_OK | MB_ICONINFORMATION);
        return;
    }
    const bool initialize = Button_GetCheck(w.initialize) == BST_CHECKED;
    if (!device.initialized && !initialize) {
        MessageBoxW(w.hwnd, L"This iPod has no iTunes database yet. Tick \x201CInitialize an empty iPod\x201D to write one.",
                    kAppName, MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (!device.initialized && initialize) {
        const int answer = MessageBoxW(w.hwnd,
                                       L"This writes a new iTunesDB to the selected iPod. Do this only for an iPod whose "
                                       L"database was erased or never created; existing music is not indexed by it.",
                                       kAppName, MB_OKCANCEL | MB_ICONWARNING);
        if (answer != IDOK) return;
    }
    IniWrite(L"iPod", L"Mount", device.mountpoint.wstring());
    IniWrite(L"iPod", L"Initialize", initialize ? L"1" : L"0");
    const auto tracks = CaptureTracks();
    if (tracks.empty()) {
        MessageBoxW(w.hwnd, L"Nothing to sync.", kAppName, MB_OK | MB_ICONINFORMATION);
        return;
    }
    g_syncing = true;
    g_cancel = false;
    w.scanning = false;
    const std::wstring name = device.name.empty() ? device.mountpoint.wstring() : device.name;
    DestroyWindow(w.hwnd);
    ShowMessage(L"Syncing " + std::to_wstring(tracks.size()) + L" songs to " + name + L"...");
    if (g_worker.joinable()) g_worker.join();
    g_worker = std::thread([target = g.hwnd, mount = device.mountpoint, options = ipod::SyncOptions{initialize},
                            tracks = std::move(tracks)]() mutable {
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        auto* result = new ipod::SyncResult;
        try {
            *result = ipod::Sync(tracks, mount, options, [target](size_t done, size_t total, const std::wstring& title) {
                auto* progress = new Progress{done, total, title};
                if (!PostMessageW(target, WM_APP_IPOD, 0, (LPARAM)progress)) {
                    delete progress;
                    return false;
                }
                return !g_cancel.load();
            });
        } catch (const std::exception& exception) {
            result->errors.push_back(exception.what());
        }
        if (!PostMessageW(target, WM_APP_IPOD, 2, (LPARAM)result)) delete result;
    });
}

LRESULT CALLBACK IpodProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            w.hwnd = h;
            auto make = [&](DWORD ex, const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
                HWND control = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, h,
                                               (HMENU)(INT_PTR)id, g.inst, nullptr);
                SendMessageW(control, WM_SETFONT, (WPARAM)g.font, TRUE);
                return control;
            };
            w.device = make(0, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, kIdDevice);
            w.refresh = make(0, WC_BUTTONW, L"&Refresh", BS_PUSHBUTTON | WS_TABSTOP, kIdRefresh);
            w.summary = make(0, WC_STATICW, L"Looking for iPods...", SS_LEFT | SS_NOPREFIX, kIdSummary);
            w.initialize = make(0, WC_BUTTONW, L"&Initialize an empty iPod (writes a new iTunesDB)",
                                BS_AUTOCHECKBOX | WS_TABSTOP, kIdInitialize);
            w.count = make(0, WC_STATICW, L"", SS_LEFT | SS_NOPREFIX, kIdCount);
            w.sync = make(0, WC_BUTTONW, L"&Sync", BS_DEFPUSHBUTTON | WS_TABSTOP, kIdSync);
            w.close = make(0, WC_BUTTONW, L"Close", BS_PUSHBUTTON | WS_TABSTOP, kIdClose);
            for (HWND control : {w.device, w.refresh, w.initialize, w.sync, w.close}) ApplyControlTheme(control, g.dark);
            ApplyWindowTheme(h, P());
            LayoutIpod();
            BeginScan();
            return 0;
        }
        case WM_SIZE:
            LayoutIpod();
            return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {S(500), S(230)};
            return 0;
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(h, &rc);
            FillRect((HDC)wp, &rc, g.surfaceBrush);
            return 1;
        }
        case WM_CTLCOLORSTATIC:
            return SurfaceControlColor((HDC)wp, (HWND)lp == w.summary || (HWND)lp == w.count ? SubColor() : TextColor());
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case kIdDevice:
                    if (HIWORD(wp) == CBN_SELCHANGE) UpdateDetails();
                    break;
                case kIdRefresh: BeginScan(); break;
                case kIdInitialize: UpdateDetails(); break;
                case kIdSync:
                case IDOK: StartSync(); break;
                case kIdClose:
                case IDCANCEL: DestroyWindow(h); break;
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            w.hwnd = nullptr;
            w.scanning = false;
            std::vector<ipod::Device>().swap(w.devices);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

void ShowIpod() {
    if (g_syncing) {
        ShowMessage(L"An iPod sync is already running.");
        return;
    }
    if (w.hwnd) {
        ShowWindow(w.hwnd, SW_RESTORE);
        SetForegroundWindow(w.hwnd);
        return;
    }
    // One selection snapshot per opening. The playlist is the fallback when nothing is selected,
    // matching Copy to folder or device.
    g_ids = SelectedTracks();
    if (g_ids.empty()) g_ids = Active().items;
    RegisterAppClass(kIpodClass, IpodProc);
    RECT r{0, 0, S(620), S(250)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_DLGMODALFRAME, g.dpi);
    RECT owner;
    GetWindowRect(g.hwnd, &owner);
    const int width = r.right - r.left, height = r.bottom - r.top;
    CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kIpodClass, L"Sync to iPod",
                    WS_OVERLAPPEDWINDOW & ~WS_MINIMIZEBOX & ~WS_MAXIMIZEBOX | WS_CLIPCHILDREN,
                    owner.left + (owner.right - owner.left - width) / 2, owner.top + S(80), width, height,
                    g.hwnd, nullptr, g.inst, nullptr);
    ShowWindow(w.hwnd, SW_SHOW);
}

bool IpodDialogMessage(MSG* msg) { return w.hwnd && IsDialogMessageW(w.hwnd, msg); }
bool IpodBusy() { return g_scanning.load() || g_syncing.load(); }

void OnIpodMessage(WPARAM wp, LPARAM lp) {
    if (wp == 0) {
        std::unique_ptr<Progress> progress(reinterpret_cast<Progress*>(lp));
        if (g.closing || !g_syncing) return;
        ShowMessage(L"Syncing " + std::to_wstring(progress->done + 1) + L" of " + std::to_wstring(progress->total) +
                    L": " + progress->title);
        return;
    }
    if (wp == 1) {
        std::unique_ptr<ScanResult> result(reinterpret_cast<ScanResult*>(lp));
        if (g_worker.joinable()) g_worker.join();
        if (g.closing) return;
        g_scanning = false;
        PopulateDevices(*result);
        return;
    }
    std::unique_ptr<ipod::SyncResult> result(reinterpret_cast<ipod::SyncResult*>(lp));
    if (g_worker.joinable()) g_worker.join();
    g_syncing = false;
    if (g.closing) return;
    size_t skipped = result->skippedExisting + result->skippedMissing + result->skippedProtected +
                     result->skippedUnsupported + result->failed;
    std::wstring text;
    if (result->databaseWritten)
        text = L"Synced " + std::to_wstring(result->added) + (result->added == 1 ? L" song" : L" songs");
    else if (result->added)
        text = L"Copied " + std::to_wstring(result->added) + L" songs, but the database was not written";
    else
        text = L"Nothing was added";
    if (result->bytesCopied) text += L" (" + Size(result->bytesCopied) + L")";
    if (skipped) text += L"; skipped " + std::to_wstring(skipped);
    if (result->cancelled) text += L"; cancelled";
    text += L".";
    if (!result->errors.empty()) {
        text += L"\n\n";
        for (size_t i = 0; i < result->errors.size() && i < 6; ++i) {
            text += FromUtf8(result->errors[i]);
            text += L"\n";
        }
        if (result->errors.size() > 6) text += L"(and more)\n";
        MessageBoxW(g.hwnd, text.c_str(), L"Sync to iPod", MB_OK | MB_ICONWARNING);
    } else {
        ShowMessage(text);
    }
}

void StopIpod() {
    g_cancel = true;
    if (g_worker.joinable()) g_worker.join();
    MSG pending;
    while (PeekMessageW(&pending, g.hwnd, WM_APP_IPOD, WM_APP_IPOD, PM_REMOVE)) {
        if (pending.wParam == 1) delete reinterpret_cast<ScanResult*>(pending.lParam);
        else if (pending.wParam == 2) delete reinterpret_cast<ipod::SyncResult*>(pending.lParam);
        else delete reinterpret_cast<Progress*>(pending.lParam);
    }
    g_scanning = false;
    g_syncing = false;
    if (w.hwnd) DestroyWindow(w.hwnd);
}

}  // namespace app
