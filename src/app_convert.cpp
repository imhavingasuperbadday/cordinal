// Library > Convert... (and the track menu): the Convert window, where the format, copies or
// replacement and the copies' folder are chosen with a live count of what will convert; the run on a
// background worker (folder watching paused meanwhile, so new files and originals leaving aren't
// mistaken for library changes), progress in the status bar, and for replacements the library
// switched over to each verified file with its plays, ratings and playlists. The work itself is
// convert.cpp's. Undo walks back through the replacements still remembered.
#include "app.h"
#include "convert.h"

namespace app {

namespace {

const wchar_t kConvertClass[] = L"CordinalConvert";
constexpr UINT_PTR kReplanTimer = 7;
// Control ids; the UI suite finds the controls by them.
enum { kIdFormat = 100, kIdCopy, kIdReplace, kIdBeside, kIdFolder, kIdPath, kIdBrowse, kIdSummary };

// The window, while open: its controls, the tracks it was opened for and the plan shown.
struct ConvertWindow {
    HWND hwnd = nullptr, formatLabel = nullptr, format = nullptr, modeLabel = nullptr, copy = nullptr, replace = nullptr,
         whereLabel = nullptr, beside = nullptr, folder = nullptr, path = nullptr, browse = nullptr, help = nullptr,
         summary = nullptr, go = nullptr, cancel = nullptr;
    std::vector<TrackId> ids;
    std::vector<ConvertItem> plan;
} w;

std::thread g_converter;
std::atomic<bool> g_cancel{false};
bool g_converting = false;      // UI-thread flag: a run is in flight
bool g_replacing = false;       // the run replaces originals (copies leave the library alone)
CordExport g_format = CordExport::None;
bool g_canUndo = false;         // a log is left to undo (kept current instead of listing the folder on every menu update)
std::vector<ConvertItem> g_plan;  // the running plan, for the progress line
std::vector<size_t> g_ordinal;    // plan index -> its place among the files converted
size_t g_total = 0;
std::unordered_set<TrackId> g_convertingIds;

std::wstring LogDir() { return g.dataDir + L"\\convert"; }
unsigned MemoryDays() { return kConvertMemoryDays[g.convertMemory]; }

std::wstring Files(size_t n) { return std::to_wstring(n) + (n == 1 ? L" file" : L" files"); }

std::wstring Megabytes(int64_t bytes) {
    wchar_t s[32];
    swprintf_s(s, L"%.1f MB", (double)bytes / 1048576.0);
    return s;
}

// A kept original's place in the library: shown as a track of its own, or left out like a track
// the user removed, so folder scans don't bring it back.
void ApplyOriginalVisibility(Track& t) {
    if (g.convertShowOriginals) t.flags = (uint16_t)((t.flags | Track::kInLibrary) & ~(Track::kExcluded | Track::kMissing));
    else t.flags = (uint16_t)((t.flags | Track::kExcluded) & ~(Track::kInLibrary | Track::kInbox));
}

void Reread(std::vector<TrackId>& ids, TrackId id) {
    if (!g.tagPending.count(id)) g_lib[id].flags &= ~(Track::kLoaded | Track::kTagFailed);
    ids.push_back(id);
}

CordExport Format() {
    return (CordExport)std::clamp((int)SendMessageW(w.format, CB_GETCURSEL, 0, 0), 0, (int)CordExport::Count - 1);
}
bool Replacing() { return Button_GetCheck(w.replace) == BST_CHECKED; }
bool IntoFolder() { return !Replacing() && Button_GetCheck(w.folder) == BST_CHECKED; }

// The tracks as Convert would treat them: the library's codec tells ALAC from AAC, and the song
// playing is left alone.
std::vector<ConvertItem> Plan(const std::vector<TrackId>& ids, CordExport format, bool replace, const std::wstring& folder) {
    std::vector<std::pair<TrackId, std::wstring>> tracks;
    tracks.reserve(ids.size());
    for (TrackId id : ids) tracks.push_back({id, g_lib.Path(id)});
    std::vector<ConvertItem> plan = PlanConvert(tracks, format, replace ? ConvertMode::Replace : ConvertMode::Copy, folder,
                                                [](TrackId id) { return g_lib[id].codec; });
    for (ConvertItem& item : plan)
        if (item.skip.empty() && item.id == g.playing) item.skip = L"playing now";
    return plan;
}

// Replans and updates everything that depends on the choices: the folder row, the help line, the
// summary and whether Convert can start.
void Replan() {
    const bool replace = Replacing(), into = IntoFolder();
    for (HWND c : {w.beside, w.folder}) EnableWindow(c, !replace);
    for (HWND c : {w.path, w.browse}) EnableWindow(c, into);
    const std::wstring folder = into ? WindowText(w.path) : L"";
    w.plan = Plan(w.ids, Format(), replace, folder);
    const size_t ready = (size_t)std::count_if(w.plan.begin(), w.plan.end(), [](const ConvertItem& i) { return i.skip.empty(); });
    const CordExportInfo& info = kCordExports[(unsigned)Format()];
    std::wstring help = replace ? L"The library switches to each new file once it's checked, with its plays, ratings and playlists; "
                                  L"the originals go to the Recycle Bin or stay, as set in Preferences. Library > Undo last conversion "
                                  L"brings them back."
                                : L"Copies get the originals' tags and covers, and the originals and the library stay as they are.";
    help += info.kbps ? L" MP3 and AAC copies are 48 kHz stereo, and play without gaps."
                      : L" Lossless files start from FLAC, WAV, AIFF, ALAC, WavPack, Monkey's Audio, TTA or .cord.";
    SetWindowTextW(w.help, help.c_str());
    SetWindowTextW(w.summary, (into && folder.empty() ? L"Choose a folder for the copies." : ConvertSummary(w.plan)).c_str());
    EnableWindow(w.go, ready && !g_converting && !(into && folder.empty()));
}

void LayoutConvert() {
    RECT rc;
    GetClientRect(w.hwnd, &rc);
    const int pad = S(14), lw = S(70), W = rc.right - pad * 2, x = pad + lw;
    int y = pad;
    MoveWindow(w.formatLabel, pad, y + S(4), lw, S(20), TRUE);
    MoveWindow(w.format, x, y, S(300), S(240), TRUE);
    y += S(36);
    MoveWindow(w.modeLabel, pad, y + S(2), lw, S(20), TRUE);
    MoveWindow(w.copy, x, y, W - lw, S(22), TRUE);
    MoveWindow(w.replace, x, y + S(24), W - lw, S(22), TRUE);
    y += S(58);
    MoveWindow(w.whereLabel, pad, y + S(2), lw, S(20), TRUE);
    MoveWindow(w.beside, x, y, W - lw, S(22), TRUE);
    MoveWindow(w.folder, x, y + S(26), S(130), S(22), TRUE);
    MoveWindow(w.path, x + S(134), y + S(25), W - lw - S(134) - S(104), S(24), TRUE);
    MoveWindow(w.browse, rc.right - pad - S(96), y + S(24), S(96), S(26), TRUE);
    y += S(62);
    MoveWindow(w.help, x, y, W - lw, S(52), TRUE);
    MoveWindow(w.summary, pad, rc.bottom - pad - S(24), W - S(230), S(22), TRUE);
    MoveWindow(w.go, rc.right - pad - S(216), rc.bottom - pad - S(28), S(108), S(28), TRUE);
    MoveWindow(w.cancel, rc.right - pad - S(100), rc.bottom - pad - S(28), S(100), S(28), TRUE);
}

void BrowseFolder() {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    DWORD options = 0;
    dlg->GetOptions(&options);
    dlg->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"Put the converted copies in this folder");
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dlg->Show(w.hwnd)) || FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return;
    SetWindowTextW(w.path, path);
    CoTaskMemFree(path);
    Button_SetCheck(w.folder, BST_CHECKED);
    Button_SetCheck(w.beside, BST_UNCHECKED);
    Replan();
}

// Hands a confirmed plan to the worker; the results come back as WM_APP_CONVERT messages.
void Run(std::vector<ConvertItem> plan, CordExport format, bool replace) {
    g.scanner.Stop();
    g_converting = true;
    g_replacing = replace;
    g_format = format;
    g_cancel = false;
    g_plan = plan;
    g_ordinal.assign(plan.size(), 0);
    g_total = 0;
    for (size_t i = 0; i < plan.size(); ++i)
        if (plan[i].skip.empty()) {
            g_ordinal[i] = ++g_total;
            g_convertingIds.insert(plan[i].id);
        }
    ConvertSettings settings;
    settings.recycle = g.convertRecycle;
    // Half the cores: the conversion runs in the background and never competes with playback.
    settings.threads = std::max(1u, std::thread::hardware_concurrency() / 2);
    if (g_converter.joinable()) g_converter.join();
    g_converter = std::thread([hwnd = g.hwnd, plan = std::move(plan), settings, logDir = LogDir()]() {
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        auto* r = new ConvertResult(
            ExecuteConvert(plan, settings, logDir, g_cancel, [hwnd](size_t i) { PostMessageW(hwnd, WM_APP_CONVERT, 0, (LPARAM)i); }));
        if (!PostMessageW(hwnd, WM_APP_CONVERT, 1, (LPARAM)r)) delete r;
    });
    InvalidateRect(g.list, nullptr, FALSE);
    UpdateStatus();
}

// Saves the choices, makes the copies' folder, asks before replacing, and starts the run.
void Start() {
    if (g_converting) return;
    const CordExport format = Format();
    const bool replace = Replacing(), into = IntoFolder();
    const std::wstring folder = WindowText(w.path);
    IniWrite(L"Convert", L"Format", std::to_wstring((int)format));
    IniWrite(L"Convert", L"Replace", replace ? L"1" : L"0");
    IniWrite(L"Convert", L"IntoFolder", into ? L"1" : L"0");
    IniWrite(L"Convert", L"Folder", folder);
    if (into) {
        SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
        const DWORD a = GetFileAttributesW(folder.c_str());
        if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
            MessageBoxW(w.hwnd, (L"Couldn't open or create " + folder).c_str(), kAppName, MB_OK | MB_ICONWARNING);
            return;
        }
    }
    Replan();
    if (!IsWindowEnabled(w.go)) return;
    if (replace && MessageBoxW(w.hwnd, ReplaceQuestion(w.plan, g.convertRecycle, g.convertShowOriginals, MemoryDays()).c_str(), kAppName,
                               MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        return;
    Run(std::move(w.plan), format, replace);
    DestroyWindow(w.hwnd);
}

LRESULT CALLBACK ConvertProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            w.hwnd = h;
            auto make = [&](DWORD ex, const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
                HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, h, (HMENU)(INT_PTR)id, g.inst, nullptr);
                SendMessageW(c, WM_SETFONT, (WPARAM)g.font, TRUE);
                return c;
            };
            w.formatLabel = make(0, WC_STATICW, L"Format", SS_LEFT, 0);
            w.format = make(0, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, kIdFormat);
            for (const CordExportInfo& f : kCordExports) SendMessageW(w.format, CB_ADDSTRING, 0, (LPARAM)f.label);
            const int format = std::clamp(_wtoi(IniRead(L"Convert", L"Format", L"1").c_str()), 0, (int)CordExport::Count - 1);
            SendMessageW(w.format, CB_SETCURSEL, format, 0);
            w.modeLabel = make(0, WC_STATICW, L"Make", SS_LEFT, 0);
            w.copy = make(0, WC_BUTTONW, L"C&opies, and keep the originals", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, kIdCopy);
            w.replace = make(0, WC_BUTTONW, L"&Replacements for the originals in the library", BS_AUTORADIOBUTTON, kIdReplace);
            const bool replace = IniRead(L"Convert", L"Replace", L"0") == L"1";
            Button_SetCheck(replace ? w.replace : w.copy, BST_CHECKED);
            w.whereLabel = make(0, WC_STATICW, L"Copies go", SS_LEFT, 0);
            w.beside = make(0, WC_BUTTONW, L"&Next to their originals", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, kIdBeside);
            w.folder = make(0, WC_BUTTONW, L"&In this folder:", BS_AUTORADIOBUTTON, kIdFolder);
            w.path = make(WS_EX_CLIENTEDGE, WC_EDITW, IniRead(L"Convert", L"Folder", L"").c_str(), ES_AUTOHSCROLL | WS_TABSTOP, kIdPath);
            w.browse = make(0, WC_BUTTONW, L"&Browse...", BS_PUSHBUTTON | WS_TABSTOP, kIdBrowse);
            Button_SetCheck(IniRead(L"Convert", L"IntoFolder", L"0") == L"1" ? w.folder : w.beside, BST_CHECKED);
            w.help = make(0, WC_STATICW, L"", SS_LEFT | SS_NOPREFIX, 0);
            w.summary = make(0, WC_STATICW, L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, kIdSummary);
            w.go = make(0, WC_BUTTONW, L"&Convert", BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK);
            w.cancel = make(0, WC_BUTTONW, L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL);
            SetWindowTheme(w.path, g.dark ? L"DarkMode_CFD" : L"Explorer", nullptr);
            for (HWND c : {w.format, w.copy, w.replace, w.beside, w.folder, w.browse, w.go, w.cancel}) ApplyControlTheme(c, g.dark);
            ApplyWindowTheme(h, P());
            Replan();
            return 0;
        }
        case WM_SIZE:
            LayoutConvert();
            return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {S(560), S(330)};
            return 0;
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(h, &rc);
            FillRect((HDC)wp, &rc, g.surfaceBrush);
            return 1;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            return SurfaceControlColor((HDC)wp, (HWND)lp == w.help || (HWND)lp == w.summary ? SubColor() : TextColor());
        case WM_CTLCOLOREDIT:
            return FieldControlColor((HDC)wp);
        case WM_TIMER:
            if (wp == kReplanTimer) {
                KillTimer(h, kReplanTimer);
                Replan();
            }
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case kIdPath:
                    if (HIWORD(wp) == EN_CHANGE) SetTimer(h, kReplanTimer, 300, nullptr);
                    break;
                case kIdFormat:
                    if (HIWORD(wp) == CBN_SELCHANGE) Replan();
                    break;
                case kIdCopy:
                case kIdReplace:
                case kIdBeside:
                case kIdFolder: Replan(); break;
                case kIdBrowse: BrowseFolder(); break;
                case IDOK: Start(); break;
                case IDCANCEL: DestroyWindow(h); break;
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            w = ConvertWindow{};
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

// Opens the window for `ids`: the selection, or the whole library when nothing is selected.
void ShowConvert(const std::vector<TrackId>& ids) {
    if (ids.empty()) {
        ShowMessage(L"Nothing to convert.");
        return;
    }
    if (g_converting) {
        ShowMessage(L"A conversion is already running.");
        return;
    }
    if (w.hwnd) DestroyWindow(w.hwnd);
    RegisterAppClass(kConvertClass, ConvertProc);
    w.ids = ids;
    RECT r{0, 0, S(640), S(360)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_DLGMODALFRAME, g.dpi);
    RECT owner;
    GetWindowRect(g.hwnd, &owner);
    const int width = r.right - r.left, height = r.bottom - r.top;
    const std::wstring title = L"Convert " + std::to_wstring(ids.size()) + (ids.size() == 1 ? L" song" : L" songs");
    CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kConvertClass, title.c_str(),
                    WS_OVERLAPPEDWINDOW & ~WS_MINIMIZEBOX | WS_CLIPCHILDREN, owner.left + (owner.right - owner.left - width) / 2,
                    owner.top + S(80), width, height, g.hwnd, nullptr, g.inst, nullptr);
    ShowWindow(w.hwnd, SW_SHOW);
}

bool ConvertBusy() { return g_converting; }
bool ConvertUsing(TrackId id) { return g_convertingIds.count(id) != 0; }
bool ConvertDialogMessage(MSG* msg) { return w.hwnd && IsDialogMessageW(w.hwnd, msg); }

void InitConvert() {
    PruneConvertLogs(LogDir(), MemoryDays());
    g_canUndo = CanUndoConvert(LogDir());
}

// UI thread, from WM_APP_CONVERT: a file starting (progress), or the finished run. A replaced track
// moves to its new file, keeping its id, and a kept original gets a record of its own; copies leave
// the library alone (folder watching finds them like any new file).
void OnConvertMessage(WPARAM wp, LPARAM lp) {
    if (!wp) {
        const size_t i = (size_t)lp;
        if (i < g_plan.size())
            ShowMessage(L"Converting " + std::to_wstring(g_ordinal[i]) + L" of " + std::to_wstring(g_total) + L" to " +
                        kCordExports[(unsigned)g_format].label + L": " + FileNameOf(g_plan[i].from));
        return;
    }
    std::unique_ptr<ConvertResult> r(reinterpret_cast<ConvertResult*>(lp));
    if (g_converter.joinable()) g_converter.join();
    g_converting = false;
    std::unordered_set<TrackId>().swap(g_convertingIds);
    std::vector<ConvertItem>().swap(g_plan);
    std::vector<TrackId> reread;
    int64_t saved = 0;
    for (const ConvertDone& d : r->done) {
        if (!g_replacing) continue;
        // The worker can outlive an idle library compaction. Its result names the source by path,
        // so resolve the live track now instead of trusting the id it was planned with.
        TrackId id = g_lib.Find(d.from);
        if (id == kNone) continue;
        const int64_t before = (int64_t)g_lib[id].fileSize;
        WIN32_FILE_ATTRIBUTE_DATA a{};
        if (GetFileAttributesExW(d.to.c_str(), GetFileExInfoStandard, &a))
            saved += before - (int64_t)((uint64_t)a.nFileSizeHigh << 32 | a.nFileSizeLow);
        g_lib.Rename(id, d.to);
        Reread(reread, id);
        if (!d.recycled) {
            TrackId original = g_lib.Find(d.from);
            if (original == kNone) original = g_lib.Add(d.from);
            g_lib[original].flags |= Track::kOriginal;
            ApplyOriginalVisibility(g_lib[original]);
            if (g.convertShowOriginals) Reread(reread, original);
        }
    }
    QueueTags(reread);
    if (g_replacing) {
        g_lib.Touch();
        g.dirty = true;
        RebuildView();
    }
    StartScanner();
    PruneConvertLogs(LogDir(), MemoryDays());
    g_canUndo = CanUndoConvert(LogDir());
    std::wstring done = ConvertDoneText(*r, g_format, g_replacing, g.convertRecycle);
    if (saved > 0) done += L" They take " + Megabytes(saved) + L" less.";
    if (g.closing) return;  // shutting down: the library is updated, nothing to say
    if (!r->failed.empty()) {
        std::wstring text = done + L"\n\n" + Files(r->failed.size()) + L" couldn't be converted and were left as they were:\n\n";
        for (size_t i = 0; i < r->failed.size() && i < 8; ++i) text += r->failed[i].first + L"\n    " + r->failed[i].second + L"\n";
        MessageBoxW(g.hwnd, text.c_str(), kAppName, MB_OK | MB_ICONWARNING);
    } else if (!r->done.empty()) {
        PlaySfx(Sfx::Organized);
    }
    ShowMessage(done);
    UpdateStatus();
}

// Reverses the newest replacement still remembered: the originals come back and the library
// returns to them, keeping each track's id; the converted files go to the Recycle Bin.
void UndoConversion() {
    if (g_converting) return;
    if (!g_canUndo) {
        ShowMessage(L"There's no conversion left to undo.");
        return;
    }
    if (MessageBoxW(g.hwnd, L"Undo the last conversion?\n\nIts originals come back (from the Recycle Bin, if they went there) and "
                            L"the library returns to them. The converted files go to the Recycle Bin.",
                    kAppName, MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        return;
    g.scanner.Stop();
    std::wstring error;
    const auto undone = UndoLastConvert(LogDir(), true, error);
    std::vector<TrackId> reread;
    for (const ConvertUndone& u : undone) {
        TrackId cord = g_lib.Find(u.cord), original = g_lib.Find(u.original);
        if (cord != kNone && original != kNone && cord != original) {
            // Both paths have a record: the tracks trade places, so the one with the plays and
            // ratings returns to the original and the kept original's record takes the .cord's path.
            const std::wstring parked = u.cord + L".undo";
            g_lib.Rename(cord, parked);
            g_lib.Rename(original, u.cord);
            g_lib.Rename(cord, u.original);
            std::swap(cord, original);
            Track& left = g_lib[cord];
            left.flags = (uint16_t)((left.flags | Track::kExcluded) & ~(Track::kOriginal | Track::kInLibrary | Track::kInbox));
        } else if (cord != kNone) {
            g_lib.Rename(cord, u.original);
            original = cord;
        }
        if (original == kNone) continue;
        Track& t = g_lib[original];
        t.flags = (uint16_t)((t.flags | Track::kInLibrary) & ~(Track::kOriginal | Track::kExcluded | Track::kMissing));
        Reread(reread, original);
    }
    QueueTags(reread);
    g_lib.Touch();
    g.dirty = true;
    RebuildView();
    StartScanner();
    g_canUndo = CanUndoConvert(LogDir());
    if (!error.empty()) PlaySfx(Sfx::Oops);
    else if (!undone.empty()) PlaySfx(Sfx::Unorganized);
    ShowMessage(error.empty() ? L"Brought back " + Files(undone.size()) + L" from before the conversion." : error);
}

bool ConvertCommand(int id) {
    if (id >= IDM_CONVERT_MEMORY_BASE && id < IDM_CONVERT_MEMORY_BASE + (int)std::size(kConvertMemoryDays)) {
        g.convertMemory = (uint8_t)(id - IDM_CONVERT_MEMORY_BASE);
        IniWrite(L"Convert", L"Memory", std::to_wstring(g.convertMemory));
        PruneConvertLogs(LogDir(), MemoryDays());
        g_canUndo = CanUndoConvert(LogDir());
        ShowMessage(L"Conversions can be undone for " + std::to_wstring(MemoryDays()) + (MemoryDays() == 1 ? L" day." : L" days."));
        return true;
    }
    switch (id) {
        case IDM_CONVERT: {
            // The selection, or the whole library when nothing is selected.
            auto ids = SelectedTracks();
            if (ids.empty())
                for (TrackId t = 0; t < (TrackId)g_lib.Count(); ++t)
                    if (g_lib[t].InLibrary()) ids.push_back(t);
            ShowConvert(ids);
            return true;
        }
        case IDM_CONVERT_UNDO: UndoConversion(); return true;
        case IDM_CONVERT_CANCEL:
            if (g_converting) {
                g_cancel = true;
                ShowMessage(L"Cancelling the conversion after the current file...");
            }
            return true;
        case IDM_CONVERT_RECYCLE:
            g.convertRecycle = !g.convertRecycle;
            IniWrite(L"Convert", L"Recycle", g.convertRecycle ? L"1" : L"0");
            ShowMessage(g.convertRecycle ? L"Converted originals will go to the Recycle Bin."
                                         : L"Converted originals will be kept where they are.");
            return true;
        case IDM_CONVERT_SHOW_ORIGINALS: {
            g.convertShowOriginals = !g.convertShowOriginals;
            IniWrite(L"Convert", L"ShowOriginals", g.convertShowOriginals ? L"1" : L"0");
            std::vector<TrackId> reread;
            size_t count = 0;
            for (TrackId t = 0; t < (TrackId)g_lib.Count(); ++t)
                if (g_lib[t].flags & Track::kOriginal) {
                    ApplyOriginalVisibility(g_lib[t]);
                    if (g.convertShowOriginals && !g_lib[t].Loaded()) reread.push_back(t);
                    ++count;
                }
            QueueTags(reread);
            g_lib.Touch();
            g.dirty = true;
            RebuildView();
            ShowMessage((g.convertShowOriginals ? L"Showing " : L"Hiding ") + std::to_wstring(count) +
                        (count == 1 ? L" kept original" : L" kept originals") + L" in the library.");
            return true;
        }
    }
    return false;
}

// Cancel conversion comes and goes with the run (UpdateMainMenu); the options are in Preferences.
void UpdateConvertMenu(HMENU m) {
    EnableMenuItem(m, IDM_CONVERT, MF_BYCOMMAND | (g_converting ? MF_GRAYED : MF_ENABLED));
    EnableMenuItem(m, IDM_CONVERT_UNDO, MF_BYCOMMAND | (!g_converting && g_canUndo ? MF_ENABLED : MF_GRAYED));
}

// Shutdown: the file in progress is abandoned (it leaves nothing behind), and the files already
// converted are applied to the library before it is saved.
void StopConverter() {
    if (w.hwnd) DestroyWindow(w.hwnd);
    g_cancel = true;
    if (!g_converter.joinable()) return;
    g_converter.join();
    MSG done;
    while (PeekMessageW(&done, g.hwnd, WM_APP_CONVERT, WM_APP_CONVERT, PM_REMOVE))
        if (done.wParam) OnConvertMessage(done.wParam, done.lParam);
}

}  // namespace app
