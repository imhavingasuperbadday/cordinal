// File-menu backup, restore and statistics import. Workers own snapshots; the UI reviews folder
// mappings and import counts before any live state changes. Profile replacement waits for shutdown.
#include "app.h"
#include "backup.h"
#include <set>

namespace app {
namespace {
enum class Operation { Backup, Restore, Statistics, Review, Stage, Apply };
struct Result {
    Operation operation;
    std::wstring folder, error, stage;
    LibraryArchive data;
    std::string settings;
    std::vector<migration::Statistics> statistics;
    std::vector<migration::PathMap> maps;
    size_t unlocated = 0, missing = 0;
};
std::thread worker;
std::atomic<bool> cancel{false};
bool busy = false, stopping = false;
std::wstring restoreStage;
struct Window {
    HWND hwnd = nullptr, hint = nullptr, list = nullptr, change = nullptr, go = nullptr, close = nullptr;
    std::unique_ptr<Result> result;
    bool reviewed = false;
} b;

std::wstring Absolute(const std::wstring& path) {
    wchar_t full[32768];
    DWORD n = GetFullPathNameW(path.c_str(), (DWORD)std::size(full), full, nullptr);
    return n && n < std::size(full) ? std::wstring(full) : std::wstring();
}

std::wstring PickFolder(const wchar_t* title, HWND owner) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        return {};
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(title);
    ComPtr<IShellItem> item;
    PWSTR raw = nullptr;
    if (FAILED(dialog->Show(owner)) || FAILED(dialog->GetResult(&item)) ||
        FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)))
        return {};
    std::wstring path = raw;
    CoTaskMemFree(raw);
    return path;
}

void Start(std::function<Result*()> job) {
    if (worker.joinable()) worker.join();
    cancel = false;
    busy = true;
    stopping = false;
    if (b.hwnd) {
        EnableWindow(b.go, FALSE);
        EnableWindow(b.change, FALSE);
    }
    HWND owner = g.hwnd;
    worker = std::thread([owner, job = std::move(job)] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        Result* result = nullptr;
        try {
            result = job();
        } catch (...) {
            result = new Result{Operation::Backup};
            result->error = L"Couldn't finish the backup operation.";
        }
        CoUninitialize();
        if (!PostMessageW(owner, WM_APP_BACKUP, 0, (LPARAM)result)) delete result;
    });
}

void Populate() {
    ListView_DeleteAllItems(b.list);
    if (!b.result) return;
    for (size_t i = 0; i < b.result->maps.size(); ++i) {
        const auto& map = b.result->maps[i];
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = (int)i;
        item.pszText = const_cast<wchar_t*>(map.from.c_str());
        ListView_InsertItem(b.list, &item);
        ListView_SetItemText(b.list, (int)i, 1, const_cast<wchar_t*>(map.to.c_str()));
    }
    if (!b.result->maps.empty())
        ListView_SetItemState(b.list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
}

void Roots(Result& r) {
    std::set<std::wstring> dirs;
    if (r.operation == Operation::Restore) {
        std::string profile;
        if (backup::ReadBytes(r.folder + L"\\profile-path.txt", profile, 65536) && !profile.empty())
            r.maps.push_back({FromUtf8(profile), Absolute(g.dataDir)});
        // Use the original watched folders before deriving roots for playlist-only tracks.
        auto ini = r.folder + L"\\profile\\settings.ini";
        wchar_t value[32768];
        GetPrivateProfileStringW(L"Library", L"Folders", L"", value, (DWORD)std::size(value), ini.c_str());
        std::wstring roots = value;
        for (size_t at = 0; at < roots.size();) {
            size_t end = std::min(roots.find(L'|', at), roots.size());
            if (end > at) r.maps.push_back({roots.substr(at, end - at), roots.substr(at, end - at)});
            at = end + 1;
        }
        for (TrackId id = 0; id < r.data.library.Count(); ++id)
            dirs.insert(DirectoryOf(r.data.library.Path(id)));
    } else
        for (const auto& s : r.statistics)
            dirs.insert(DirectoryOf(s.path));
    // Consecutive folders on a drive share the longest directory prefix. The result is an
    // editable location, never a filename guess that could join two different songs.
    std::vector<std::wstring> extra;
    for (auto dir : dirs) {
        if (dir.empty()) continue;
        bool covered = false;
        for (const auto& m : r.maps)
            if (PathIsUnder(dir + L"\\", m.from)) {
                covered = true;
                break;
            }
        if (covered) continue;
        if (!extra.empty()) {
            auto common = extra.back();
            while (common.size() > 3 && !PathIsUnder(dir + L"\\", common))
                common = DirectoryOf(common);
            if (PathIsUnder(dir + L"\\", common)) {
                extra.back() = common;
                continue;
            }
        }
        extra.push_back(dir);
    }
    for (auto& dir : extra)
        r.maps.push_back({dir, dir});
}

std::wstring Summary(const Result& r) {
    if (r.operation == Operation::Restore || r.operation == Operation::Review)
        return std::to_wstring(r.data.library.Count()) + L" songs, " +
               std::to_wstring(r.data.playlists.size()) + L" playlists, " +
               std::to_wstring(r.data.log.size()) + L" listening events. " + std::to_wstring(r.missing) +
               L" music files unavailable at the chosen locations; their data is kept.\r\n"
               L"Music is not included. Packs, soundfonts and settings are restored. Reconnect ListenBrainz "
               L"and pair browser devices again.";
    const auto plan = migration::PlanStatistics(g_lib, r.statistics, r.maps);
    return std::to_wstring(plan.matched.size()) + L" songs matched, " +
           std::to_wstring(plan.unmatched + r.unlocated) + L" unmatched, " + std::to_wstring(plan.conflicts) +
           L" rating conflicts or duplicate records.\r\n"
           L"Keep higher counts, earliest first-play dates and existing ratings. No listening events are "
           L"invented. "
           L"A backup of the current profile is saved first.";
}

void Review() {
    if (!b.result || busy) return;
    if (b.result->operation == Operation::Statistics) {
        SetWindowTextW(b.hint, Summary(*b.result).c_str());
        SetWindowTextW(b.go, L"Import statistics");
        b.reviewed = true;
        return;
    }
    auto r = std::shared_ptr<Result>(b.result.release());
    r->operation = Operation::Review;
    ShowMessage(L"Checking the backup's new music locations...");
    Start([r] {
        auto out = new Result{Operation::Review};
        if (!migration::RemapLibrary(r->data, r->maps, r->error)) {
            out->error = r->error;
            return out;
        }
        r->settings = backup::RemapSettings(r->settings, r->maps);
        r->missing = 0;
        for (TrackId id = 0; id < r->data.library.Count(); ++id) {
            if (cancel) {
                out->error = L"Restore review cancelled.";
                return out;
            }
            auto path = r->data.library.Path(id);
            std::wstring real;
            int index;
            if (SplitSubtrack(path, real, index)) path = real;
            auto attributes = GetFileAttributesW(path.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY))
                ++r->missing;
        }
        *out = std::move(*r);
        return out;
    });
}

void Apply() {
    if (!b.result || busy) return;
    if (!b.reviewed) {
        Review();
        return;
    }
    if (b.result->operation == Operation::Statistics) {
        auto plan = migration::PlanStatistics(g_lib, b.result->statistics, b.result->maps);
        if (plan.matched.empty()) {
            ShowMessage(L"No songs matched. Add the music to your library and check the folder locations.");
            return;
        }
        auto r = std::shared_ptr<Result>(b.result.release());
        const auto profile = Absolute(g.dataDir);
        r->folder = profile + L".before-statistics-" + std::to_wstring(UnixNow());
        std::string settings;
        if (!SaveBackupSettings(settings)) {
            b.result.reset(new Result(std::move(*r)));
            ShowMessage(L"Couldn't read current settings.");
            return;
        }
        auto snapshot = std::make_shared<LibraryArchive>(CaptureLibraryArchive(g.playlists));
        ShowMessage(L"Saving a backup before importing statistics...");
        Start([r, snapshot, profile, settings] {
            auto out = new Result{Operation::Apply};
            if (backup::Create(r->folder, profile, settings, *snapshot, cancel, out->error)) {
                out->folder = r->folder;
                out->statistics = std::move(r->statistics);
                out->maps = std::move(r->maps);
            }
            return out;
        });
    } else {
        const std::wstring text =
            Summary(*b.result) + L"\r\n\r\nThis replaces the current profile at " + Absolute(g.dataDir) +
            L" and restarts cordinal. The entire previous profile is kept beside it in a "
            L".before-restore folder. Continue?";
        if (MessageBoxW(b.hwnd, text.c_str(), L"Restore backup", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
            return;
        auto r = std::shared_ptr<Result>(b.result.release());
        const auto profile = Absolute(g.dataDir);
        ShowMessage(L"Preparing the restored profile...");
        Start([r, profile] {
            auto out = new Result{Operation::Stage};
            backup::Stage(r->folder, profile, r->settings, r->data, cancel, out->stage, out->error);
            return out;
        });
    }
}

LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        b.hwnd = h;
        auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
            HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, h,
                                     (HMENU)(INT_PTR)id, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.font, TRUE);
            return c;
        };
        b.hint = make(
            WC_STATICW,
            L"Select a music folder and choose its location on this computer, then review the result.", 0, 0);
        b.list = make(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | WS_TABSTOP, 100);
        ListView_SetExtendedListViewStyle(b.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.cx = S(310);
        col.pszText = const_cast<wchar_t*>(L"Original folder");
        ListView_InsertColumn(b.list, 0, &col);
        col.pszText = const_cast<wchar_t*>(L"Location on this computer");
        ListView_InsertColumn(b.list, 1, &col);
        for (HWND c : {b.list, ListView_GetHeader(b.list)})
            ApplyControlTheme(c, g.dark);
        ListView_SetBkColor(b.list, Ref(P().surface2));
        ListView_SetTextBkColor(b.list, Ref(P().surface2));
        ListView_SetTextColor(b.list, TextColor());
        b.change = make(WC_BUTTONW, L"Change location...", WS_TABSTOP, 101);
        b.go = make(WC_BUTTONW, L"Review", WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK);
        b.close = make(WC_BUTTONW, L"Cancel", WS_TABSTOP, IDCANCEL);
        for (HWND c : {b.change, b.go, b.close})
            ApplyControlTheme(c, g.dark);
        ApplyWindowTheme(h, P());
        Populate();
        return 0;
    }
    case WM_SIZE: {
        RECT r;
        GetClientRect(h, &r);
        int pad = S(12), width = r.right - pad * 2;
        MoveWindow(b.hint, pad, pad, width, S(98), TRUE);
        MoveWindow(b.list, pad, S(118), width, std::max(S(80), (int)r.bottom - S(170)), TRUE);
        MoveWindow(b.change, pad, r.bottom - S(40), S(142), S(28), TRUE);
        MoveWindow(b.go, r.right - S(256), r.bottom - S(40), S(150), S(28), TRUE);
        MoveWindow(b.close, r.right - S(96), r.bottom - S(40), S(84), S(28), TRUE);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDCANCEL) {
            if (busy)
                cancel = true;
            else
                DestroyWindow(h);
        } else if (LOWORD(wp) == IDOK)
            Apply();
        else if (LOWORD(wp) == 101 && b.result && !busy && !b.reviewed) {
            int row = ListView_GetNextItem(b.list, -1, LVNI_SELECTED);
            if (row >= 0 && row < (int)b.result->maps.size()) {
                auto folder = PickFolder(L"Choose this music folder's new location", b.hwnd);
                if (!folder.empty()) {
                    b.result->maps[row].to = folder;
                    Populate();
                }
            }
        }
        return 0;
    case WM_CLOSE:
        if (busy)
            cancel = true;
        else
            DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        b.hwnd = nullptr;
        b.result.reset();
        b.reviewed = false;
        return 0;
    case WM_ERASEBKGND: {
        RECT r;
        GetClientRect(h, &r);
        FillRect((HDC)wp, &r, g.surfaceBrush);
        return 1;
    }
    case WM_CTLCOLORSTATIC:
        return SurfaceControlColor((HDC)wp, TextColor());
    case WM_GETMINMAXINFO:
        ((MINMAXINFO*)lp)->ptMinTrackSize = {S(620), S(360)};
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void Open(std::unique_ptr<Result> result) {
    if (b.hwnd) DestroyWindow(b.hwnd);
    Roots(*result);
    b.result = std::move(result);
    b.reviewed = false;
    RegisterAppClass(L"CordinalBackup", Proc);
    RECT r{0, 0, S(680), S(390)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_DLGMODALFRAME, g.dpi);
    CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, L"CordinalBackup",
                    b.result->operation == Operation::Statistics ? L"Import playback statistics"
                                                                 : L"Restore backup",
                    WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                    r.bottom - r.top, g.hwnd, nullptr, g.inst, nullptr);
    ShowWindow(b.hwnd, SW_SHOW);
    SetFocus(b.list);
}
} // namespace

bool BackupDialogMessage(MSG* msg) { return b.hwnd && IsDialogMessageW(b.hwnd, msg); }
bool BackupBusy() { return busy; }
void CancelBackup() {
    cancel = true;
    ShowMessage(L"Cancelling the backup operation...");
}
std::wstring BackupStatus() {
    return busy ? cancel ? L"Cancelling backup..." : L"Backup or migration in progress..." : L"";
}

void BackUpProfile() {
    if (busy || b.hwnd) {
        ShowMessage(L"Finish or cancel the current backup operation first.");
        return;
    }
    auto parent = PickFolder(L"Choose where to save the backup", g.hwnd);
    if (parent.empty()) return;
    std::string settings;
    if (!SaveBackupSettings(settings)) {
        ShowMessage(L"Couldn't read settings.ini. The backup wasn't started.");
        return;
    }
    auto data = std::make_shared<LibraryArchive>(CaptureLibraryArchive(g.playlists));
    const auto folder = parent + L"\\cordinal-backup-" + std::to_wstring(UnixNow());
    const auto profile = Absolute(g.dataDir);
    ShowMessage(L"Backing up playlists, statistics, history and settings...");
    Start([data, settings, folder, profile] {
        auto r = new Result{Operation::Backup};
        r->folder = folder;
        backup::Create(folder, profile, settings, *data, cancel, r->error);
        return r;
    });
}

void RestoreProfile() {
    if (busy || b.hwnd) {
        ShowMessage(L"Finish or cancel the current backup operation first.");
        return;
    }
    auto folder = PickFolder(L"Choose the cordinal backup folder", g.hwnd);
    if (folder.empty()) return;
    ReviewBackupFolder(folder);
}

void ReviewBackupFolder(const std::wstring& folder) {
    if (busy || b.hwnd) return;
    ShowMessage(L"Validating the backup...");
    Start([folder] {
        auto r = new Result{Operation::Restore};
        r->folder = folder;
        if (backup::Read(folder, r->data, r->settings, r->error) &&
            r->settings.starts_with("# cordinal settings\n"))
            r->error = L"This backup contains Linux settings. Restore it with the Linux player; its "
                       L"statistics.csv can be imported here.";
        return r;
    });
}

void ImportPlaybackStatistics() {
    if (busy || b.hwnd) {
        ShowMessage(L"Finish or cancel the current backup operation first.");
        return;
    }
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        return;
    COMDLG_FILTERSPEC filter[] = {{L"Playback statistics (foobar2000 XML or CSV)", L"*.xml;*.csv"}};
    dialog->SetFileTypes(1, filter);
    dialog->SetTitle(L"Import playback statistics");
    ComPtr<IShellItem> item;
    PWSTR raw = nullptr;
    if (FAILED(dialog->Show(g.hwnd)) || FAILED(dialog->GetResult(&item)) ||
        FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)))
        return;
    std::wstring file = raw;
    CoTaskMemFree(raw);
    ShowMessage(L"Reading playback statistics...");
    Start([file] {
        auto r = new Result{Operation::Statistics};
        if (ExtOf(file) == L"xml")
            migration::ReadFoobarStatistics(file, r->statistics, r->unlocated, r->error);
        else {
            std::string bytes;
            if (!backup::ReadBytes(file, bytes))
                r->error = L"Couldn't read the statistics CSV.";
            else
                migration::ReadStatisticsCsv(bytes, r->statistics, r->error);
        }
        return r;
    });
}

void OnBackupMessage(LPARAM lp) {
    std::unique_ptr<Result> r((Result*)lp);
    if (worker.joinable()) worker.join();
    busy = false;
    if (stopping) return;
    if (cancel) {
        if (b.hwnd) DestroyWindow(b.hwnd);
        ShowMessage(L"Backup operation cancelled.");
        return;
    }
    if (!r->error.empty()) {
        ShowMessage(r->error);
        if (b.hwnd) DestroyWindow(b.hwnd);
        return;
    }
    if (r->operation == Operation::Backup) {
        ShowMessage(L"Backup saved to " + r->folder);
    } else if (r->operation == Operation::Review) {
        b.result = std::move(r);
        b.reviewed = true;
        SetWindowTextW(b.hint, Summary(*b.result).c_str());
        SetWindowTextW(b.go, L"Restore and restart");
        EnableWindow(b.go, TRUE);
        ShowMessage(L"Backup checked. Review the restore summary.");
    } else if (r->operation == Operation::Stage) {
        restoreStage = r->stage;
        if (b.hwnd) DestroyWindow(b.hwnd);
        PostMessageW(g.hwnd, WM_CLOSE, 0, 0);
    } else if (r->operation == Operation::Apply) {
        auto plan = migration::PlanStatistics(g_lib, r->statistics, r->maps);
        migration::ApplyStatistics(g_lib, plan);
        g.dirty = true;
        g.statsDirty = true;
        SaveLibraryNow();
        RebuildView(true);
        ShowMessage(L"Imported statistics for " + std::to_wstring(plan.matched.size()) +
                    L" songs. Previous profile: " + r->folder);
        if (b.hwnd) DestroyWindow(b.hwnd);
    } else
        Open(std::move(r));
}

void StopBackup() {
    stopping = true;
    cancel = true;
    if (worker.joinable()) worker.join();
    MSG msg;
    while (PeekMessageW(&msg, g.hwnd, WM_APP_BACKUP, WM_APP_BACKUP, PM_REMOVE))
        delete (Result*)msg.lParam;
    if (b.hwnd) DestroyWindow(b.hwnd);
    busy = false;
}

void FinishProfileRestore() {
    if (restoreStage.empty()) return;
    std::wstring previous, error;
    if (!backup::Commit(Absolute(g.dataDir), restoreStage, previous, error)) {
        MessageBoxW(nullptr, error.c_str(), L"Couldn't restore the backup", MB_OK | MB_ICONERROR);
        return;
    }
    wchar_t exe[32768];
    GetModuleFileNameW(nullptr, exe, (DWORD)std::size(exe));
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    std::wstring command = L"\"" + std::wstring(exe) + L"\"";
    if (CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup,
                       &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    } else
        MessageBoxW(nullptr, L"The backup was restored. Open cordinal again to use it.", L"Backup restored",
                    MB_OK);
}
} // namespace app
