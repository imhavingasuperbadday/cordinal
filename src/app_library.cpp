// Library folders: settings, scan results merged into the library, the Inbox, and the
// Library folders window. Everything here runs on the UI thread; the scanner only posts
// WM_APP_SCAN batches, which arrive here as ScanResult owners.
#include "app.h"

namespace app {

namespace { void ResetScanState(); }

std::wstring MusicFolder() {
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Music, 0, nullptr, &path))) return {};
    std::wstring folder = path;
    CoTaskMemFree(path);
    return folder;
}

bool PathIsUnder(const std::wstring& path, const std::wstring& dir) {
    return !dir.empty() && path.size() > dir.size() && (path[dir.size()] == L'\\' || dir.back() == L'\\') &&
           CompareStringOrdinal(path.c_str(), (int)dir.size(), dir.c_str(), (int)dir.size(), TRUE) == CSTR_EQUAL;
}

// Folders are stored pipe-separated in settings.ini.
void LoadLibrarySettings() {
    g.libraryFolders.clear();
    std::wstring all = IniRead(L"Library", L"Folders", L"");
    for (size_t start = 0; start < all.size();) {
        size_t bar = all.find(L'|', start);
        if (bar == std::wstring::npos) bar = all.size();
        if (bar > start) g.libraryFolders.push_back(all.substr(start, bar - start));
        start = bar + 1;
    }
}

void SaveLibrarySettings() {
    std::wstring all;
    for (auto& f : g.libraryFolders) all += (all.empty() ? L"" : L"|") + f;
    IniWrite(L"Library", L"Folders", all);
    FoobarFoldersChanged();
}

// Restarts the watcher over the current folder list; a first scan of a folder goes straight to
// the library, later ones to the Inbox.
void StartScanner() {
    g.scanner.Stop();
    ResetScanState();
    if (g.closing) return;
    if (g.libraryFolders.empty()) return;
    g.scanPending = (int)g.libraryFolders.size();
    g.scanner.Start(g.hwnd, WM_APP_SCAN, g.libraryFolders);
}

// ---------------------------------------------------------------------------
// Merging a scan into the library

namespace {

bool SameName(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

// A scope (a library folder, or one changed folder inside it) arrives as one or more chunks
// (ScanResult.first/complete). Between chunks the listing keeps what finishing it needs: the
// tracks found so far, files that may be tracks that moved (they wait for the whole listing:
// only then can a track be asked for), and the counters the folder's report is built from.
struct ScopeScan {
    bool initial = false, rootOk = true;
    bool deferNew = false;  // a track here can be gone: new files must wait for the whole listing
    size_t startCount = 0;  // the library size when the listing began
    std::vector<char> seen;               // tracks found in the listing so far
    std::vector<ScanFile> pending;        // files not in the library, awaiting the moved-file pass
    std::vector<TrackId> toRead, joined;  // tag reads; tracks that just entered the library
    size_t added = 0, moved = 0, missing = 0;
    // Cached 0/1 values share the existing handle-to-uint32_t map implementation.
    std::unordered_map<StringPool::Handle, uint32_t> dirUnder;
};
// Live scans by scope (a root folder or one changed folder); entries exist only mid-listing.
std::unordered_map<std::wstring, ScopeScan> g_scans;

void ResetScanState() {
    g_scans.clear();
    MSG pending;
    while (PeekMessageW(&pending, g.hwnd, WM_APP_SCAN, WM_APP_SCAN, PM_REMOVE))
        delete reinterpret_cast<ScanResult*>(pending.lParam);
}

// Whether a track's folder is the scanned scope or under it (a folder per distinct directory).
bool Under(const std::wstring& scope, ScopeScan& st, const Track& t) {
    auto [it, added] = st.dirUnder.try_emplace(t.dir, uint32_t(0));
    if (added) {
        std::wstring dir = g_lib.Str(t.dir);
        it->second = SameName(dir, scope) || PathIsUnder(dir, scope);
    }
    return it->second != 0;
}

// A file that just entered the library: counted, and put in the Inbox when a later scan found
// it and the Inbox is on. The caller applies the common file flags next.
TrackId AddScanned(ScopeScan& st, const ScanFile& f) {
    TrackId id = g_lib.Add(f.path);
    Track& t = g_lib[id];
    if (!(t.flags & (Track::kInLibrary | Track::kInbox | Track::kExcluded))) {
        ++st.added;
        if (!st.initial && g.inboxEnabled) t.flags |= Track::kInbox;
    }
    return id;
}

// Everything found is in the library (unless the user excluded it); tags are re-read when the
// file's size or time changed, or were never read.
void FoundScanned(ScopeScan& st, TrackId id, const ScanFile& f) {
    Track& t = g_lib[id];
    if (t.flags & Track::kExcluded) return;
    if (!(t.flags & (Track::kInLibrary | Track::kInbox))) t.added = UnixNow();  // joins the library now
    t.flags = (uint16_t)((t.flags | Track::kInLibrary) & ~Track::kMissing);
    if (t.InLibrary() && !(t.flags & Track::kCollected)) st.joined.push_back(id);
    bool changed = t.fileSize != f.size || t.fileTime != f.time;
    if (changed) t.flags &= (uint16_t)~Track::kRgScanFailed;
    t.fileSize = f.size;
    t.fileTime = f.time;
    if (changed || !t.Loaded()) st.toRead.push_back(id);
}

// The whole scope has arrived: files not in the library are matched against tracks that left
// this folder or are missing anywhere (same name, size and time: the same song moved, keeping
// its stats and playlists), the rest join the library, and tracks that left every listed
// folder are marked missing.
void FinishScope(const std::wstring& scope, ScopeScan& st) {
    if (st.deferNew) {
        std::vector<std::pair<uint64_t, TrackId>> gone;
        for (TrackId id = 0; id < (TrackId)st.startCount; ++id) {
            const Track& t = g_lib[id];
            if (!(t.flags & (Track::kInLibrary | Track::kInbox)) || st.seen[id]) continue;
            if ((t.flags & Track::kMissing) || (st.rootOk && Under(scope, st, t))) gone.emplace_back(t.fileSize ^ t.fileTime, id);
        }
        std::vector<char>().swap(st.seen);
        SortRecords(gone.begin(), gone.end());
        for (ScanFile& f : st.pending) {
            TrackId id = kNone;
            uint64_t key = f.size ^ f.time;
            auto it = std::lower_bound(gone.begin(), gone.end(), std::make_pair(key, TrackId(0)));
            for (; it != gone.end() && it->first == key; ++it) {
                if (it->second == kNone) continue;
                const Track& t = g_lib[it->second];
                if (t.fileSize == f.size && t.fileTime == f.time && SameName(g_lib.Str(t.file), FileNameOf(f.path))) {
                    id = it->second;
                    it->second = kNone;
                    break;
                }
            }
            if (id != kNone) {
                g_lib.Rename(id, f.path);
                ++st.moved;
            } else {
                id = AddScanned(st, f);
            }
            FoundScanned(st, id, f);
        }
        std::vector<ScanFile>().swap(st.pending);
        if (st.rootOk)
            for (auto& [key, id] : gone) {
                if (id == kNone) continue;
                Track& t = g_lib[id];
                if (!(t.flags & Track::kMissing) && Under(scope, st, t)) {
                    t.flags |= Track::kMissing;
                    ++st.missing;
                }
            }
    }
    QueueTags(st.toRead);
    st.toRead.clear();
    g_lib.Touch();
    g.dirty = true;
    UpdateInboxTab();
    size_t added = st.added, moved = st.moved, missing = st.missing;
    static ULONGLONG lastChirp = 0;
    if (!st.initial && added && GetTickCount64() - lastChirp > 10000) {  // one chirp for a batch of folders
        lastChirp = GetTickCount64();
        PlaySfx(Sfx::NewFiles);
    }
    if (!st.initial && (added || moved || missing)) {
        std::wstring msg = L"Library: ";
        if (added) msg += std::to_wstring(added) + (g.inboxEnabled ? L" new in the Inbox" : L" added");
        if (moved) msg += (added ? L", " : L"") + std::to_wstring(moved) + L" moved";
        if (missing) msg += (added || moved ? L", " : L"") + std::to_wstring(missing) + L" missing";
        ShowMessage(msg + L".");
    }
    AnnouncePoints(AwardLibraryAdds(st.joined));
}

}  // namespace

bool ScanInProgress() { return !g_scans.empty(); }

// Merges one scanner chunk. `firstScanDone` (lParam 1) marks the end of the initial pass over
// every root; a scope still open then had its listing cut short, so it is dropped, not finished.
void OnScanResult(ScanResult* r, bool firstScanDone) {
    if (firstScanDone) {
        g.scanPending = 0;
        g_scans.clear();  // a scope whose listing stopped mid-way has no chunk left to finish it
        UpdateStatus();
        return;
    }
    if (!r) return;
    std::unique_ptr<ScanResult> owned(r);
    ScopeScan& st = g_scans[r->scope];
    if (r->first) {
        st = ScopeScan{};
        st.initial = r->initial;
        st.rootOk = r->rootOk;
        st.startCount = g_lib.Count();
        st.seen.assign(st.startCount, 0);
        // New files wait for the whole listing only when a track of this scope (or one already
        // missing anywhere) could turn out to have moved. A fresh or unrelated library adds them
        // as they arrive, so a first scan keeps only one chunk at a time in memory.
        for (TrackId id = 0; id < (TrackId)st.startCount && !st.deferNew; ++id) {
            const Track& t = g_lib[id];
            st.deferNew = (t.flags & (Track::kInLibrary | Track::kInbox)) &&
                          ((t.flags & Track::kMissing) || (st.rootOk && Under(r->scope, st, t)));
        }
    }
    st.rootOk = st.rootOk && r->rootOk;
    for (const ScanFile& f : r->files) {
        TrackId id = g_lib.Find(f.path);
        if (id == kNone) {
            if (st.deferNew) {
                st.pending.push_back(f);
            } else {
                TrackId addedId = AddScanned(st, f);
                FoundScanned(st, addedId, f);
            }
            continue;
        }
        if (id < (TrackId)st.seen.size()) st.seen[id] = 1;
        FoundScanned(st, id, f);
    }
    if (r->complete) {
        FinishScope(r->scope, st);
        g_scans.erase(r->scope);
        if (r->initial && g.scanPending > 0) --g.scanPending;
    } else {
        QueueTags(st.toRead);
        st.toRead.clear();
    }
}

// ---------------------------------------------------------------------------
// Inbox and maintenance

// The Inbox tab exists while there's something in it, right after the Library tab.
void UpdateInboxTab() {
    size_t count = 0;
    for (TrackId id = 0; id < (TrackId)g_lib.Count(); ++id)
        if ((g_lib[id].flags & (Track::kInbox | Track::kMissing)) == Track::kInbox) ++count;
    int tab = FindPlaylist(PlaylistKind::Inbox);
    if (!count) {
        if (tab >= 0 && tab != g.active) ErasePlaylist(tab);
        else if (tab >= 0) g.playlists[tab].name = L"Inbox";
        RefreshTabs();
        return;
    }
    std::wstring name = L"Inbox (" + std::to_wstring(count) + L")";
    if (tab < 0) InsertPlaylist(1, Playlist{name, {}, PlaylistKind::Inbox});
    else if (g.playlists[tab].name != name) {
        g.playlists[tab].name = name;
        RefreshTabs();
    }
}

// Moves Inbox tracks into the library proper (clears the Inbox flag), which is what awards points.
void AcceptInbox(const std::vector<TrackId>& ids) {
    for (TrackId id : ids) g_lib[id].flags &= ~Track::kInbox;
    PointsAward points = AwardLibraryAdds(ids);
    g_lib.Touch();
    g.dirty = true;
    ClearSelection();
    RebuildView();
    UpdateInboxTab();
    ShowMessage(L"Added " + std::to_wstring(ids.size()) + (ids.size() == 1 ? L" track" : L" tracks") + L" to the library.");
    AnnouncePoints(points);
}

// Forgets missing files: clears their library flags but keeps the records, so playlists and stats
// still resolve. The user is warned first.
void RemoveMissingTracks() {
    std::vector<TrackId> missing;
    for (TrackId id = 0; id < (TrackId)g_lib.Count(); ++id)
        if ((g_lib[id].flags & Track::kMissing) && (g_lib[id].flags & (Track::kInLibrary | Track::kInbox)))
            missing.push_back(id);
    if (missing.empty()) {
        ShowMessage(L"No missing files in the library.");
        return;
    }
    std::wstring q = std::to_wstring(missing.size()) + (missing.size() == 1 ? L" library file is" : L" library files are") +
                     L" missing. Forget them, with their play counts and ratings?\n\nPlaylists keep their entries.";
    if (MessageBoxW(g.hwnd, q.c_str(), kAppName, MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    for (TrackId id : missing) g_lib[id].flags &= ~(Track::kInLibrary | Track::kInbox);
    g_lib.Touch();
    g.dirty = true;
    RebuildView();
}

// ---------------------------------------------------------------------------
// Library folders window

namespace {

struct FoldersControls {
    HWND list = nullptr, add = nullptr, music = nullptr, remove = nullptr, rescan = nullptr, hint = nullptr;
} g_fc;
bool g_foldersChanged = false;

void FoldersFill() {
    SendMessageW(g_fc.list, LB_RESETCONTENT, 0, 0);
    for (auto& f : g.libraryFolders) SendMessageW(g_fc.list, LB_ADDSTRING, 0, (LPARAM)f.c_str());
    EnableWindow(g_fc.remove, SendMessageW(g_fc.list, LB_GETCURSEL, 0, 0) >= 0);
    EnableWindow(g_fc.music, !MusicFolder().empty());
}

void FoldersLayout(HWND h) {
    RECT rc;
    GetClientRect(h, &rc);
    int pad = S(12), bw = S(110), bh = S(28);
    int listW = rc.right - pad * 3 - bw, listH = rc.bottom - pad * 2 - S(28);
    MoveWindow(g_fc.list, pad, pad, listW, listH, TRUE);
    int x = pad * 2 + listW;
    MoveWindow(g_fc.music, x, pad, bw, bh, TRUE);
    MoveWindow(g_fc.add, x, pad + bh + S(6), bw, bh, TRUE);
    MoveWindow(g_fc.remove, x, pad + (bh + S(6)) * 2, bw, bh, TRUE);
    MoveWindow(g_fc.rescan, x, pad + (bh + S(6)) * 3 + S(10), bw, bh, TRUE);
    MoveWindow(g_fc.hint, pad, pad + listH + S(8), rc.right - pad * 2, S(20), TRUE);
}

void AddLibraryFolderPath(std::wstring p) {
    p = NormalizeMediaPath(p);
    if (p.empty()) return;
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    // Skip folders already covered; a new parent replaces its subfolders.
    if (std::any_of(g.libraryFolders.begin(), g.libraryFolders.end(), [&](const std::wstring& f) {
            return SameName(f, p) || PathIsUnder(p, f);
        })) return;
    std::erase_if(g.libraryFolders, [&](const std::wstring& f) { return PathIsUnder(f, p); });
    g.libraryFolders.push_back(std::move(p));
    g_foldersChanged = true;
    FoldersFill();
}

void AddLibraryFolder() {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    DWORD options = 0;
    dlg->GetOptions(&options);
    dlg->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_ALLOWMULTISELECT);
    dlg->SetTitle(L"Add folders to the library");
    std::wstring music = MusicFolder();
    ComPtr<IShellItem> initial;
    if (!music.empty() && SUCCEEDED(SHCreateItemFromParsingName(music.c_str(), nullptr, IID_PPV_ARGS(&initial))))
        dlg->SetDefaultFolder(initial.Get());
    if (FAILED(dlg->Show(g.foldersWnd))) return;
    ComPtr<IShellItemArray> items;
    if (FAILED(dlg->GetResults(&items))) return;
    DWORD count = 0;
    items->GetCount(&count);
    for (DWORD i = 0; i < count; ++i) {
        ComPtr<IShellItem> item;
        PWSTR path = nullptr;
        if (FAILED(items->GetItemAt(i, &item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) continue;
        std::wstring p = path;
        CoTaskMemFree(path);
        AddLibraryFolderPath(std::move(p));
    }
    FoldersFill();
}

void RemoveLibraryFolder() {
    int sel = (int)SendMessageW(g_fc.list, LB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)g.libraryFolders.size()) return;
    std::wstring folder = g.libraryFolders[sel];
    std::vector<TrackId> inside;
    for (TrackId id = 0; id < (TrackId)g_lib.Count(); ++id)
        if ((g_lib[id].flags & (Track::kInLibrary | Track::kInbox)) && PathIsUnder(g_lib.Path(id), folder))
            inside.push_back(id);
    if (!inside.empty()) {
        std::wstring q = L"Also remove its " + std::to_wstring(inside.size()) +
                         L" tracks from the library?\n\nYes: they leave the library (the files stay on disk).\n"
                         L"No: they stay in the library but the folder is no longer watched.";
        int answer = MessageBoxW(g.foldersWnd, q.c_str(), kAppName, MB_YESNOCANCEL | MB_ICONQUESTION);
        if (answer == IDCANCEL) return;
        if (answer == IDYES) {
            for (TrackId id : inside) g_lib[id].flags &= ~(Track::kInLibrary | Track::kInbox | Track::kMissing);
            g_lib.Touch();
            g.dirty = true;
        }
    }
    g.libraryFolders.erase(g.libraryFolders.begin() + sel);
    g_foldersChanged = true;
    FoldersFill();
}

LRESULT CALLBACK FoldersProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            auto make = [&](DWORD ex, const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
                HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, h, (HMENU)(INT_PTR)id,
                                         g.inst, nullptr);
                SendMessageW(c, WM_SETFONT, (WPARAM)g.font, TRUE);
                return c;
            };
            g_fc.list = make(WS_EX_CLIENTEDGE, WC_LISTBOXW, L"", LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP,
                             IDC_FOLDERS_LIST);
            g_fc.add = make(0, WC_BUTTONW, L"&Add folder...", BS_PUSHBUTTON | WS_TABSTOP, IDC_FOLDERS_ADD);
            g_fc.music = make(0, WC_BUTTONW, L"Add &Music folder", BS_PUSHBUTTON | WS_TABSTOP, IDC_FOLDERS_MUSIC);
            g_fc.remove = make(0, WC_BUTTONW, L"&Remove", BS_PUSHBUTTON | WS_TABSTOP, IDC_FOLDERS_REMOVE);
            g_fc.rescan = make(0, WC_BUTTONW, L"Re&scan now", BS_PUSHBUTTON | WS_TABSTOP, IDC_FOLDERS_RESCAN);
            g_fc.hint = make(0, WC_STATICW, L"Folders are watched while cordinal runs.",
                             SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX, IDC_FOLDERS_HINT);
            for (HWND c : {g_fc.list, g_fc.add, g_fc.music, g_fc.remove, g_fc.rescan}) ApplyControlTheme(c, g.dark);
            ApplyWindowTheme(h, P());
            g_foldersChanged = false;
            FoldersFill();
            return 0;
        }
        case WM_SIZE:
            FoldersLayout(h);
            return 0;
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(h, &rc);
            FillRect((HDC)wp, &rc, g.surfaceBrush);
            return 1;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            return SurfaceControlColor((HDC)wp, (HWND)lp == g_fc.hint ? SubColor() : TextColor());
        case WM_CTLCOLORLISTBOX:
            return FieldControlColor((HDC)wp);
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDC_FOLDERS_ADD: AddLibraryFolder(); break;
                case IDC_FOLDERS_MUSIC: AddLibraryFolderPath(MusicFolder()); break;
                case IDC_FOLDERS_REMOVE: RemoveLibraryFolder(); break;
                case IDC_FOLDERS_RESCAN:
                    if (g_foldersChanged) {
                        g_foldersChanged = false;
                        SaveLibrarySettings();
                        StartScanner();
                    } else {
                        g.scanner.RescanAll();
                    }
                    break;
                case IDC_FOLDERS_LIST:
                    if (HIWORD(wp) == LBN_SELCHANGE) EnableWindow(g_fc.remove, TRUE);
                    break;
            }
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            if (g_foldersChanged) {
                SaveLibrarySettings();
                StartScanner();
            }
            g.foldersWnd = nullptr;
            RebuildView();
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

void ShowLibraryFolders() {
    if (g.foldersWnd) {
        SetForegroundWindow(g.foldersWnd);
        return;
    }
    RegisterAppClass(kFoldersClass, FoldersProc);
    RECT r{0, 0, S(560), S(300)};
    AdjustWindowRectExForDpi(&r, WS_CAPTION | WS_SYSMENU | WS_THICKFRAME, FALSE, WS_EX_DLGMODALFRAME, g.dpi);
    RECT owner;
    GetWindowRect(g.hwnd, &owner);
    int w = r.right - r.left, hgt = r.bottom - r.top;
    g.foldersWnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kFoldersClass, L"Library folders",
                                   WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_POPUP | WS_CLIPCHILDREN,
                                   owner.left + (owner.right - owner.left - w) / 2, owner.top + S(100), w, hgt, g.hwnd,
                                   nullptr, g.inst, nullptr);
    ShowWindow(g.foldersWnd, SW_SHOW);
}

}  // namespace app
