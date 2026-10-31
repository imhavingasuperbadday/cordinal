// The Copy to folder or device window (the track and playlist tab menus, and Library): a
// destination, an Organize naming pattern with a live preview, a format, and the copy itself on a
// worker with progress in the status bar and Cancel in the menus, like Convert to .cord. The
// destination can be a folder or a phone's folder, which has no drive letter and is reached through
// the shell. The work itself is devicecopy.cpp's; originals are only read.
#include "app.h"
#include "devicecopy.h"
#include "organize.h"

namespace app {

namespace {

const wchar_t kCopyClass[] = L"CordinalCopy";
constexpr UINT_PTR kPreviewTimer = 7;
enum { kIdDest = 100, kIdBrowse, kIdPattern, kIdReset, kIdFormat, kIdPlaylist, kIdStale, kIdList };

// Single window instance: its controls, the tracks and the plan shown, and the destination's
// manifest (read once per destination, not on every keystroke).
struct Copier {
    HWND hwnd = nullptr, destLabel = nullptr, dest = nullptr, browse = nullptr, patternLabel = nullptr, pattern = nullptr,
         reset = nullptr, help = nullptr, formatLabel = nullptr, format = nullptr, playlist = nullptr, stale = nullptr,
         list = nullptr, summary = nullptr, go = nullptr, cancel = nullptr;
    std::vector<TrackId> ids;
    std::wstring set, playlistName;  // the playlist's name, or kCopySelectionSet for a selection
    // A phone's folder: its shell parsing name, and the friendly path shown in the field for it.
    std::wstring device, deviceLabel;
    std::wstring manifestFor;
    std::vector<CopyRecord> manifest;
    std::vector<CopyItem> plan;
    size_t staleCount = 0;
} c;

std::thread g_copier;
std::atomic<bool> g_cancel{false};
bool g_copying = false;
std::vector<std::wstring> g_names;  // plan index -> file name, for the progress line
std::vector<size_t> g_ordinal;
size_t g_total = 0;

bool Device() { return !c.device.empty() && WindowText(c.dest) == c.deviceLabel; }
std::wstring Destination() { return Device() ? c.device : WindowText(c.dest); }
CopyFormat Format() { return (CopyFormat)std::clamp((int)SendMessageW(c.format, CB_GETCURSEL, 0, 0), 0, (int)CopyFormat::Count - 1); }
bool PlaylistCopy() { return c.set != kCopySelectionSet; }

std::wstring Size(uint64_t bytes) {
    wchar_t s[32];
    if (bytes >= 1000000000ull) swprintf_s(s, L"%.1f GB", bytes / 1e9);
    else swprintf_s(s, L"%.0f MB", std::ceil(bytes / 1e6));
    return s;
}

std::wstring Songs(size_t n) { return std::to_wstring(n) + (n == 1 ? L" song" : L" songs"); }

// Rebuilds the plan and the summary under the list: what will be copied and converted, roughly
// how much space it takes against what the destination has free, and what earlier copies the
// playlist no longer wants.
void Replan() {
    const std::wstring dest = Destination();
    const bool device = Device();
    if (dest != c.manifestFor) {
        c.manifestFor = dest;
        c.manifest.clear();
        const DWORD attributes = device ? 0 : GetFileAttributesW(dest.c_str());
        if (!dest.empty() && (device || (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY))))
            ReadCopyManifest(dest, device, c.manifest);
    }
    c.plan = PlanCopy(c.ids, dest, device, WindowText(c.pattern), Format(), c.manifest);
    size_t copying = 0, converting = 0, there = 0, blocked = 0;
    uint64_t bytes = 0;
    for (const CopyItem& item : c.plan) {
        if (!item.skip.empty()) ++blocked;
        else if (item.done) ++there;
        else {
            ++copying;
            converting += item.convert;
            bytes += item.estimate;
        }
    }
    c.staleCount = PlaylistCopy() ? StaleCopies(c.plan, c.manifest, c.set).size() : 0;
    ListView_SetItemCountEx(c.list, (int)c.plan.size(), LVSICF_NOSCROLL);
    InvalidateRect(c.list, nullptr, FALSE);
    std::wstring s = Songs(copying) + L" to copy";
    if (converting) s += L" (" + std::to_wstring(converting) + L" as AAC)";
    if (copying) s += L", about " + Size(bytes);
    ULARGE_INTEGER free{};
    if (!device && GetDiskFreeSpaceExW(dest.c_str(), &free, nullptr, nullptr))
        s += (bytes > free.QuadPart ? L" \x2014 more than the " : L" of ") + Size(free.QuadPart) + L" free";
    if (there) s += L"  \x2022  " + std::to_wstring(there) + L" already there";
    if (blocked) s += L"  \x2022  " + std::to_wstring(blocked) + L" can't be copied";
    if (c.staleCount && Button_GetCheck(c.stale) == BST_CHECKED) s += L"  \x2022  " + std::to_wstring(c.staleCount) + L" earlier copies to remove";
    if (dest.empty()) s = L"Choose a folder, a memory card or a phone to copy to.";
    SetWindowTextW(c.summary, s.c_str());
    EnableWindow(c.go, !dest.empty() && !g_copying && (copying || (c.staleCount && Button_GetCheck(c.stale) == BST_CHECKED)));
}

void LayoutCopier() {
    RECT rc;
    GetClientRect(c.hwnd, &rc);
    const int pad = S(14), lw = S(60), W = rc.right - pad * 2, field = W - lw - S(104);
    MoveWindow(c.destLabel, pad, pad + S(4), lw, S(20), TRUE);
    MoveWindow(c.dest, pad + lw, pad, field, S(24), TRUE);
    MoveWindow(c.browse, rc.right - pad - S(96), pad, S(96), S(26), TRUE);
    int y = pad + S(34);
    MoveWindow(c.patternLabel, pad, y + S(4), lw, S(20), TRUE);
    MoveWindow(c.pattern, pad + lw, y, field, S(24), TRUE);
    MoveWindow(c.reset, rc.right - pad - S(96), y, S(96), S(26), TRUE);
    y += S(34);
    MoveWindow(c.formatLabel, pad, y + S(4), lw, S(20), TRUE);
    MoveWindow(c.format, pad + lw, y, S(260), S(200), TRUE);
    MoveWindow(c.playlist, pad + lw + S(276), y + S(2), W - lw - S(276), S(22), TRUE);
    y += S(30);
    MoveWindow(c.stale, pad + lw, y, W - lw, S(22), TRUE);
    y += S(28);
    MoveWindow(c.help, pad + lw, y, W - lw, S(34), TRUE);
    y += S(40);
    const int bottom = rc.bottom - pad - S(28) - S(10);
    MoveWindow(c.list, pad, y, W, bottom - y, TRUE);
    const int half = (W - GetSystemMetrics(SM_CXVSCROLL) - S(130)) / 2;
    ListView_SetColumnWidth(c.list, 0, half);
    ListView_SetColumnWidth(c.list, 1, half);
    ListView_SetColumnWidth(c.list, 2, S(130));
    MoveWindow(c.summary, pad, rc.bottom - pad - S(24), W - S(230), S(22), TRUE);
    MoveWindow(c.go, rc.right - pad - S(216), rc.bottom - pad - S(28), S(108), S(28), TRUE);
    MoveWindow(c.cancel, rc.right - pad - S(100), rc.bottom - pad - S(28), S(100), S(28), TRUE);
}

// Without FOS_FORCEFILESYSTEM the picker also offers a phone's storage, which has no file system
// path: that destination is kept as the shell's parsing name and shown by its friendly path.
void BrowseDestination() {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    DWORD options = 0;
    dlg->GetOptions(&options);
    dlg->SetOptions((options | FOS_PICKFOLDERS) & ~FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"Copy to this folder, memory card or phone");
    ComPtr<IShellItem> item;
    if (FAILED(dlg->Show(c.hwnd)) || FAILED(dlg->GetResult(&item))) return;
    PWSTR path = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        c.device.clear();
        SetWindowTextW(c.dest, path);
    } else if (SUCCEEDED(item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &path))) {
        c.device = path;
        CoTaskMemFree(path);
        path = nullptr;
        c.deviceLabel = c.device;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_DESKTOPABSOLUTEEDITING, &path))) c.deviceLabel = path;
        SetWindowTextW(c.dest, c.deviceLabel.c_str());
    }
    if (path) CoTaskMemFree(path);
}

// True when `dir` is a library folder or inside one: copies there would be scanned in as duplicates.
bool InLibraryFolder(const std::wstring& dir) {
    for (const std::wstring& root : g.libraryFolders) {
        std::wstring r = root;
        while (!r.empty() && (r.back() == L'\\' || r.back() == L'/')) r.pop_back();
        if (dir.size() >= r.size() && _wcsnicmp(dir.c_str(), r.c_str(), r.size()) == 0 &&
            (dir.size() == r.size() || dir[r.size()] == L'\\' || dir[r.size()] == L'/'))
            return true;
    }
    return false;
}

// Saves the fields, checks the destination, asks before anything would be removed, and hands the
// plan to the worker; the window closes and the result arrives as WM_APP_COPY.
void Start() {
    if (g_copying) return;
    const bool device = Device();
    const std::wstring dest = Destination();
    IniWrite(L"Copy", L"Folder", WindowText(c.dest));
    IniWrite(L"Copy", L"Device", device ? c.device : L"");
    IniWrite(L"Copy", L"Pattern", WindowText(c.pattern));
    IniWrite(L"Copy", L"Format", std::to_wstring((int)Format()));
    if (!device) {
        if (InLibraryFolder(dest)) {
            MessageBoxW(c.hwnd, L"That folder is in your library, so its copies would show up as duplicates. Choose a folder outside it.",
                        kAppName, MB_OK | MB_ICONINFORMATION);
            return;
        }
        SHCreateDirectoryExW(nullptr, dest.c_str(), nullptr);
        if (!(GetFileAttributesW(dest.c_str()) & FILE_ATTRIBUTE_DIRECTORY) || GetFileAttributesW(dest.c_str()) == INVALID_FILE_ATTRIBUTES) {
            MessageBoxW(c.hwnd, (L"Couldn't open or create " + dest).c_str(), kAppName, MB_OK | MB_ICONWARNING);
            return;
        }
    }
    c.manifestFor.clear();  // read it again: another program may have changed the destination
    Replan();
    const bool removeStale = c.staleCount && Button_GetCheck(c.stale) == BST_CHECKED;
    if (removeStale) {
        std::wstring q = L"Remove " + std::to_wstring(c.staleCount) + (c.staleCount == 1 ? L" earlier copy" : L" earlier copies") +
                         L" that \x201C" + c.playlistName + L"\x201D no longer holds?\n\nOnly files cordinal copied there are removed. "
                         L"Memory cards and phones have no Recycle Bin, so they are deleted for good (the originals on this PC stay).";
        if (MessageBoxW(c.hwnd, q.c_str(), kAppName, MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
    }
    CopySettings settings;
    settings.dest = dest;
    settings.device = device;
    settings.set = c.set;
    settings.removeStale = removeStale;
    settings.kbps = CopyKbps(Format());
    wchar_t temp[MAX_PATH + 1];
    GetTempPathW(MAX_PATH, temp);
    settings.staging = std::wstring(temp) + L"cordinal-copy-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    if (PlaylistCopy() && Button_GetCheck(c.playlist) == BST_CHECKED) {
        // Every occurrence, in the playlist's order, by its path relative to the playlist file.
        std::unordered_map<TrackId, std::string> where;
        for (const CopyItem& item : c.plan)
            if (item.skip.empty()) {
                std::wstring rel = item.rel;
                std::replace(rel.begin(), rel.end(), L'\\', L'/');
                where[item.id] = ToUtf8(rel);
            }
        std::vector<std::pair<TrackId, std::string>> entries;
        for (TrackId id : c.ids)
            if (auto w = where.find(id); w != where.end()) entries.push_back({id, w->second});
        settings.playlistName = c.playlistName;
        settings.playlist = M3UText(entries);
    }
    g_copying = true;
    g_cancel = false;
    g_names.assign(c.plan.size(), L"");
    g_ordinal.assign(c.plan.size(), 0);
    g_total = 0;
    for (size_t i = 0; i < c.plan.size(); ++i)
        if (c.plan[i].skip.empty() && !c.plan[i].done) {
            g_ordinal[i] = ++g_total;
            g_names[i] = FileNameOf(c.plan[i].rel);
        }
    if (g_copier.joinable()) g_copier.join();
    g_copier = std::thread([hwnd = g.hwnd, plan = std::move(c.plan), settings = std::move(settings), manifest = c.manifest]() {
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        auto* r = new CopyResult(ExecuteCopy(plan, settings, manifest, g_cancel, [hwnd](size_t i) { PostMessageW(hwnd, WM_APP_COPY, 0, (LPARAM)i); }));
        if (!PostMessageW(hwnd, WM_APP_COPY, 1, (LPARAM)r)) delete r;
    });
    ShowMessage(L"Copying " + Songs(g_total) + L"...");
    DestroyWindow(c.hwnd);
}

LRESULT CALLBACK CopyProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            c.hwnd = h;
            auto make = [&](DWORD ex, const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
                HWND w = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, h, (HMENU)(INT_PTR)id, g.inst, nullptr);
                SendMessageW(w, WM_SETFONT, (WPARAM)g.font, TRUE);
                return w;
            };
            const std::wstring folder = IniRead(L"Copy", L"Folder", L""), pattern = IniRead(L"Copy", L"Pattern", kDefaultOrganizePattern);
            c.device = IniRead(L"Copy", L"Device", L"");
            c.deviceLabel = c.device.empty() ? L"" : folder;
            c.destLabel = make(0, WC_STATICW, L"Copy to", SS_LEFT, 0);
            c.dest = make(WS_EX_CLIENTEDGE, WC_EDITW, folder.c_str(), ES_AUTOHSCROLL | WS_TABSTOP, kIdDest);
            c.browse = make(0, WC_BUTTONW, L"&Browse...", BS_PUSHBUTTON | WS_TABSTOP, kIdBrowse);
            c.patternLabel = make(0, WC_STATICW, L"Names", SS_LEFT, 0);
            c.pattern = make(WS_EX_CLIENTEDGE, WC_EDITW, pattern.c_str(), ES_AUTOHSCROLL | WS_TABSTOP, kIdPattern);
            c.reset = make(0, WC_BUTTONW, L"&Default", BS_PUSHBUTTON | WS_TABSTOP, kIdReset);
            c.formatLabel = make(0, WC_STATICW, L"Format", SS_LEFT, 0);
            c.format = make(0, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, kIdFormat);
            for (const wchar_t* f : {L"Keep the original files", L"Lossless to AAC 192 kb/s (.m4a)", L"Lossless to AAC 128 kb/s (.m4a)"})
                SendMessageW(c.format, CB_ADDSTRING, 0, (LPARAM)f);
            SendMessageW(c.format, CB_SETCURSEL, std::clamp(_wtoi(IniRead(L"Copy", L"Format", L"0").c_str()), 0, (int)CopyFormat::Count - 1), 0);
            c.playlist = make(0, WC_BUTTONW, L"Write a &playlist (.m3u8)", BS_AUTOCHECKBOX | WS_TABSTOP, kIdPlaylist);
            c.stale = make(0, WC_BUTTONW, L"&Remove songs no longer in the playlist", BS_AUTOCHECKBOX | WS_TABSTOP, kIdStale);
            Button_SetCheck(c.playlist, PlaylistCopy() ? BST_CHECKED : BST_UNCHECKED);
            EnableWindow(c.playlist, PlaylistCopy());
            EnableWindow(c.stale, PlaylistCopy());
            c.help = make(0, WC_STATICW,
                          L"Names as in Organize files: a backslash starts a new folder. Lossy files are copied as they are. "
                          L"ReplayGain goes into the copies, so players that read it level the music.",
                          SS_LEFT | SS_NOPREFIX, 0);
            c.list = make(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", LVS_REPORT | LVS_OWNERDATA | LVS_NOSORTHEADER | WS_TABSTOP, kIdList);
            ListView_SetExtendedListViewStyle(c.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
            const wchar_t* cols[] = {L"From", L"To", L"Status"};
            for (int i = 0; i < 3; ++i) {
                LVCOLUMNW col{};
                col.mask = LVCF_TEXT | LVCF_WIDTH;
                col.pszText = const_cast<wchar_t*>(cols[i]);
                col.cx = S(200);
                ListView_InsertColumn(c.list, i, &col);
            }
            c.summary = make(0, WC_STATICW, L"", SS_LEFT | SS_NOPREFIX, 0);
            c.go = make(0, WC_BUTTONW, L"&Copy", BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK);
            c.cancel = make(0, WC_BUTTONW, L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL);
            for (HWND w : {c.dest, c.pattern}) SetWindowTheme(w, g.dark ? L"DarkMode_CFD" : L"Explorer", nullptr);
            for (HWND w : {c.browse, c.reset, c.go, c.cancel, c.list, c.format, c.playlist, c.stale}) ApplyControlTheme(w, g.dark);
            ListView_SetBkColor(c.list, Ref(P().surface2));
            ListView_SetTextBkColor(c.list, Ref(P().surface2));
            ListView_SetTextColor(c.list, TextColor());
            ApplyControlTheme(ListView_GetHeader(c.list), g.dark);
            ApplyWindowTheme(h, P());
            Replan();
            return 0;
        }
        case WM_SIZE:
            LayoutCopier();
            return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {S(760), S(420)};
            return 0;
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(h, &rc);
            FillRect((HDC)wp, &rc, g.surfaceBrush);
            return 1;
        }
        case WM_CTLCOLORSTATIC:
            return SurfaceControlColor((HDC)wp, (HWND)lp == c.help || (HWND)lp == c.summary ? SubColor() : TextColor());
        case WM_CTLCOLOREDIT:
            return FieldControlColor((HDC)wp);
        case WM_NOTIFY: {
            auto* hdr = reinterpret_cast<NMHDR*>(lp);
            if (hdr->hwndFrom == c.list && hdr->code == LVN_GETDISPINFOW) {
                auto* di = reinterpret_cast<NMLVDISPINFOW*>(hdr);
                const int row = di->item.iItem;
                if ((di->item.mask & LVIF_TEXT) && row >= 0 && row < (int)c.plan.size()) {
                    const CopyItem& item = c.plan[row];
                    const std::wstring status = !item.skip.empty() ? item.skip : item.done ? L"already there" : item.convert ? L"convert to AAC" : L"copy";
                    const std::wstring text = di->item.iSubItem == 0 ? item.from : di->item.iSubItem == 1 ? item.rel : status;
                    wcsncpy_s(di->item.pszText, di->item.cchTextMax, text.c_str(), _TRUNCATE);
                }
            }
            return 0;
        }
        case WM_TIMER:
            if (wp == kPreviewTimer) {
                KillTimer(h, kPreviewTimer);
                Replan();
            }
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case kIdDest:
                case kIdPattern:
                    if (HIWORD(wp) == EN_CHANGE) SetTimer(h, kPreviewTimer, 300, nullptr);
                    break;
                case kIdFormat:
                    if (HIWORD(wp) == CBN_SELCHANGE) Replan();
                    break;
                case kIdStale: Replan(); break;
                case kIdBrowse: BrowseDestination(); break;
                case kIdReset: SetWindowTextW(c.pattern, kDefaultOrganizePattern); break;
                case IDOK: Start(); break;
                case IDCANCEL: DestroyWindow(h); break;
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            c.hwnd = nullptr;
            std::vector<CopyItem>().swap(c.plan);
            std::vector<CopyRecord>().swap(c.manifest);
            std::vector<TrackId>().swap(c.ids);
            c.manifestFor.clear();
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// Opens the window for `ids`. `playlist` names the playlist they are (its .m3u8, and the copies it
// owns for later removal); empty for a selection.
void ShowCopy(const std::vector<TrackId>& ids, const std::wstring& playlist) {
    if (ids.empty()) {
        ShowMessage(L"Nothing to copy.");
        return;
    }
    if (g_copying) {
        ShowMessage(L"A copy is already running.");
        return;
    }
    if (c.hwnd) DestroyWindow(c.hwnd);
    RegisterAppClass(kCopyClass, CopyProc);
    c.ids = ids;
    c.set = playlist.empty() ? kCopySelectionSet : playlist;
    c.playlistName = playlist;
    RECT r{0, 0, S(940), S(580)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_DLGMODALFRAME, g.dpi);
    RECT owner;
    GetWindowRect(g.hwnd, &owner);
    const int w = r.right - r.left, h = r.bottom - r.top;
    const std::wstring title = L"Copy " + (playlist.empty() ? Songs(ids.size()) : L"\x201C" + playlist + L"\x201D") + L" to a folder or device";
    CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kCopyClass, title.c_str(), WS_OVERLAPPEDWINDOW & ~WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                    owner.left + (owner.right - owner.left - w) / 2, owner.top + S(60), w, h, g.hwnd, nullptr, g.inst, nullptr);
    ShowWindow(c.hwnd, SW_SHOW);
}

// The tracks of the active playlist, in order; the library and Inbox views list what they show.
std::vector<TrackId> PlaylistTracks() { return Active().items; }

}  // namespace

bool CopyDialogMessage(MSG* msg) { return c.hwnd && IsDialogMessageW(c.hwnd, msg); }
bool CopyBusy() { return g_copying; }

bool CopyCommand(int id) {
    switch (id) {
        case IDM_COPY_TO: {
            // The selection, or the whole playlist when nothing is selected.
            auto ids = SelectedTracks();
            if (ids.empty()) ShowCopy(PlaylistTracks(), Active().name);
            else ShowCopy(ids, L"");
            return true;
        }
        case IDM_COPY_PL: ShowCopy(PlaylistTracks(), Active().name); return true;
        case IDM_COPY_CANCEL:
            if (g_copying) {
                g_cancel = true;
                ShowMessage(L"Cancelling the copy after the current file...");
            }
            return true;
    }
    return false;
}

// UI thread, from WM_APP_COPY: a file starting (progress), or the finished run.
void OnCopyMessage(WPARAM wp, LPARAM lp) {
    if (!wp) {
        const size_t i = (size_t)lp;
        if (i < g_names.size())
            ShowMessage(L"Copying " + std::to_wstring(g_ordinal[i]) + L" of " + std::to_wstring(g_total) + L": " + g_names[i]);
        return;
    }
    std::unique_ptr<CopyResult> r(reinterpret_cast<CopyResult*>(lp));
    if (g_copier.joinable()) g_copier.join();
    g_copying = false;
    std::vector<std::wstring>().swap(g_names);
    std::vector<size_t>().swap(g_ordinal);
    if (g.closing) return;
    std::wstring done = r->copied + r->converted ? L"Copied " + Songs(r->copied + r->converted) : L"Nothing was copied";
    if (r->converted) done += L" (" + std::to_wstring(r->converted) + L" as AAC)";
    if (r->bytes) done += L", " + Size(r->bytes);
    if (r->removed) done += L"; removed " + std::to_wstring(r->removed) + L" earlier " + (r->removed == 1 ? L"copy" : L"copies");
    done += r->cancelled ? L"; the rest was cancelled." : L".";
    if (!r->failed.empty()) {
        std::wstring text = done + L"\n\n" + std::to_wstring(r->failed.size()) + L" item(s) couldn't be copied or removed:\n\n";
        for (size_t i = 0; i < r->failed.size() && i < 8; ++i) text += r->failed[i].first + L"\n    " + r->failed[i].second + L"\n";
        MessageBoxW(g.hwnd, text.c_str(), kAppName, MB_OK | MB_ICONWARNING);
    } else if (r->copied + r->converted) {
        PlaySfx(Sfx::Organized);
    }
    ShowMessage(done);
}

// Shutdown: the file in progress is abandoned and leaves nothing under its real name.
void StopCopier() {
    g_cancel = true;
    if (!g_copier.joinable()) return;
    g_copier.join();
    MSG done;
    while (PeekMessageW(&done, g.hwnd, WM_APP_COPY, WM_APP_COPY, PM_REMOVE))
        if (done.wParam) delete reinterpret_cast<CopyResult*>(done.lParam);
    g_copying = false;
}

}  // namespace app
