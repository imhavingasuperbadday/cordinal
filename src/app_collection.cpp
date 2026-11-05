// File > Export collection and Import collection (collection.h) on Windows: the file and
// folder choices, one worker at a time with its progress in the status bar, and after an import the
// unpacked folders joining the library folders and the collection's playlists becoming tabs.
#include "app.h"

#include "collection.h"

namespace app {
namespace {

enum class Job : uint8_t { None, Export, Read, Import };
struct Done {
    Job job = Job::None;
    std::wstring zip, dest, error;
    collection::ExportResult exported;
    std::shared_ptr<collection::ImportPlan> plan;
    collection::ImportResult imported;
};

std::thread g_worker;
std::atomic<bool> g_cancel{false};
std::atomic<int> g_posted{-1};  // the progress, in tenths of a percent, last posted to the window
std::atomic<uint64_t> g_bytes{0}, g_totalBytes{0};
std::atomic<size_t> g_files{0}, g_totalFiles{0};
Job g_running = Job::None;
bool g_asking = false, g_stopping = false;

std::wstring Songs(size_t n) { return std::to_wstring(n) + (n == 1 ? L" song" : L" songs"); }
std::wstring Playlists(size_t n) { return std::to_wstring(n) + (n == 1 ? L" playlist" : L" playlists"); }

collection::ProgressFn Progress(HWND hwnd) {
    return [hwnd](const collection::Progress& p) {
        g_bytes = p.bytes;
        g_totalBytes = p.totalBytes;
        g_files = p.files;
        g_totalFiles = p.totalFiles;
        const int permille = p.totalBytes ? (int)std::min<uint64_t>(1000, p.bytes * 1000 / p.totalBytes) : 0;
        if (g_posted.exchange(permille) != permille) PostMessageW(hwnd, WM_APP_COLLECTION, 0, 0);
    };
}

void Start(Job job, std::function<void(Done&)> work) {
    if (g_worker.joinable()) g_worker.join();
    g_running = job;
    g_cancel = false;
    g_posted = -1;
    g_bytes = g_totalBytes = 0;
    g_files = g_totalFiles = 0;
    g_worker = std::thread([hwnd = g.hwnd, job, work = std::move(work)] {
        // Background priority lowers this thread's disk priority too, so playback reads come first.
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        auto* done = new Done;
        done->job = job;
        try {
            work(*done);
        } catch (...) {
            done->error = L"Couldn't finish: cordinal ran out of memory.";
        }
        if (!PostMessageW(hwnd, WM_APP_COLLECTION, 1, (LPARAM)done)) delete done;
    });
}

std::wstring PickFile(bool save, const wchar_t* title) {
    ComPtr<IFileDialog> dialog;
    if (FAILED(CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog))))
        return {};
    COMDLG_FILTERSPEC filters[] = {{save ? L"ZIP archive" : L"cordinal collection", L"*.zip"}};
    dialog->SetFileTypes(1, filters);
    dialog->SetTitle(title);
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM);
    if (save) {
        dialog->SetDefaultExtension(L"zip");
        dialog->SetFileName(collection::DefaultName(UnixNow()).c_str());
    }
    ComPtr<IShellItem> item;
    PWSTR raw = nullptr;
    if (FAILED(dialog->Show(g.hwnd)) || FAILED(dialog->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)))
        return {};
    std::wstring path = raw;
    CoTaskMemFree(raw);
    return path;
}

std::wstring PickFolder(const wchar_t* title) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return {};
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(title);
    ComPtr<IShellItem> music;
    if (const std::wstring folder = MusicFolder();
        !folder.empty() && SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&music))))
        dialog->SetDefaultFolder(music.Get());
    ComPtr<IShellItem> item;
    PWSTR raw = nullptr;
    if (FAILED(dialog->Show(g.hwnd)) || FAILED(dialog->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)))
        return {};
    std::wstring path = raw;
    CoTaskMemFree(raw);
    return path;
}

// Up to eight problems in a box that waits for OK, as Copy to folder shows them.
void ShowProblems(const std::wstring& summary, const std::vector<std::pair<std::wstring, std::wstring>>& problems,
                  const wchar_t* what) {
    std::wstring text = summary + L"\n\n" + std::to_wstring(problems.size()) + L" " + what + L":\n\n";
    for (size_t i = 0; i < problems.size() && i < 8; ++i) text += problems[i].first + L"\n    " + problems[i].second + L"\n";
    if (problems.size() > 8) text += L"...";
    MessageBoxW(g.hwnd, text.c_str(), kAppName, MB_OK | MB_ICONWARNING);
}

// A read archive: where it goes, said plainly, before anything is written.
void AskWhereToImport(std::shared_ptr<collection::ImportPlan> plan) {
    g_asking = true;
    const std::wstring dest = PickFolder(L"Choose the folder to unpack the collection into");
    if (dest.empty()) {
        g_asking = false;
        ShowMessage(L"Import cancelled.");
        return;
    }
    std::wstring folders;
    for (size_t i = 0; i < plan->folders.size(); ++i)
        folders += (i ? i + 1 == plan->folders.size() ? L" and " : L", " : L"") + (dest.back() == L'\\' ? dest : dest + L"\\") +
                   plan->folders[i];
    std::wstring text = L"Import " + Songs(plan->songs) + L" (" + collection::SizeText(plan->bytes) + L")" +
                        (plan->playlists ? L" and " + Playlists(plan->playlists) : L"") + L" into " + dest + L"?\n\n";
    if (!folders.empty()) text += L"The music goes into " + folders + L", which join your library folders. ";
    text += L"Nothing there is replaced: a file that is already there stays, and a different file with the same name "
            L"keeps it while the collection's copy goes beside it.";
    const bool go = MessageBoxW(g.hwnd, text.c_str(), L"Import collection", MB_OKCANCEL | MB_ICONQUESTION) == IDOK;
    g_asking = false;
    if (!go) {
        ShowMessage(L"Import cancelled.");
        return;
    }
    Start(Job::Import, [plan, dest, progress = Progress(g.hwnd)](Done& d) {
        d.dest = dest;
        d.imported = collection::Import(*plan, dest, g_cancel, progress);
    });
    ShowMessage(L"Importing the collection...");
}

// The unpacked music joins the library: its folders are watched, and the first scan adds it
// straight to the library; the playlists open as tabs at the end.
void FinishImport(Done& d) {
    const auto& r = d.imported;
    const size_t files = r.written + r.same;
    if (r.cancelled) {
        ShowMessage(L"Import cancelled. The " + std::to_wstring(files) + L" files unpacked so far stay in " + d.dest +
                    L"; importing again skips them.");
        return;
    }
    if (!r.error.empty()) {
        ShowMessage(r.error);
        return;
    }
    bool added = false;
    for (const std::wstring& folder : r.folders) {
        if (std::any_of(g.libraryFolders.begin(), g.libraryFolders.end(), [&](const std::wstring& f) {
                return _wcsicmp(f.c_str(), folder.c_str()) == 0 || PathIsUnder(folder, f);
            }))
            continue;
        std::erase_if(g.libraryFolders, [&](const std::wstring& f) { return PathIsUnder(f, folder); });
        g.libraryFolders.push_back(folder);
        added = true;
    }
    if (added) SaveLibrarySettings();
    StartScanner();
    for (const auto& list : r.playlists) {
        std::vector<TrackId> ids, unloaded;
        for (const std::wstring& path : list.paths) {
            const TrackId id = g_lib.Add(path);
            if (id == kNone) continue;
            ids.push_back(id);
            if (!g_lib[id].Loaded()) unloaded.push_back(id);
        }
        InsertPlaylist((int)g.playlists.size(), Playlist{UniquePlaylistName(list.name), std::move(ids)});
        QueueTags(unloaded);
    }
    g.dirty = true;
    std::wstring done = L"Imported " + std::to_wstring(files) + (files == 1 ? L" file" : L" files") + L" into " + d.dest;
    if (!r.playlists.empty()) done += L", with " + Playlists(r.playlists.size());
    done += L".";
    if (r.same) done += L" " + std::to_wstring(r.same) + L" were there already.";
    if (r.renamed) done += L" " + std::to_wstring(r.renamed) + L" went beside a different file of the same name.";
    if (!r.failed.empty()) ShowProblems(done, r.failed, L"file(s) couldn't be unpacked");
    else PlaySfx(Sfx::Organized);
    ShowMessage(done);
}

void FinishExport(Done& d) {
    const auto& r = d.exported;
    if (r.cancelled) {
        ShowMessage(L"Export cancelled. No archive was saved.");
        return;
    }
    if (!r.ok) {
        ShowMessage(r.error);
        return;
    }
    std::wstring done = L"Exported " + Songs(r.songs);
    if (r.playlists) done += L" and " + Playlists(r.playlists);
    done += L" (" + collection::SizeText(r.bytes) + L") to " + d.zip + L".";
    if (!r.skipped.empty()) ShowProblems(done, r.skipped, L"file(s) couldn't be included");
    else PlaySfx(Sfx::Organized);
    ShowMessage(done);
}

// --- Select what to export --------------------------------------------------------------------------
// A tree of artist > album > song with the playlists after it, every box ticked to begin with. The
// ticks live in `on` (songs) and `lists` (playlists); the tree shows them, adding an artist's albums
// and an album's songs only when it is first opened, so a large library opens at once. Ticking an
// artist or album ticks everything in it, and a part-ticked one shows a partial box.
namespace pick {

enum Kind : LPARAM { kArtist, kAlbum, kSong, kLists, kList };
constexpr int kIdTree = 100, kIdAll = 101, kIdNone = 102, kIdSummary = 103;
constexpr int kUnchecked = 1, kChecked = 2, kPartial = 3;  // state image indexes

struct Range {
    size_t first = 0, last = 0;  // songs [first, last) in `paths`
};
struct State {
    HWND hwnd = nullptr, tree = nullptr, summary = nullptr, all = nullptr, none = nullptr, go = nullptr, cancel = nullptr;
    const collection::ExportChoices* choices = nullptr;
    std::vector<const std::wstring*> paths;  // every song, artist by artist, album by album
    std::vector<Range> artists;               // each artist's songs
    std::vector<std::vector<Range>> albums;   // each album's songs
    std::vector<char> on, lists;
    bool syncing = false, accepted = false;
} s;

static_assert(sizeof(LPARAM) == 8, "the tree packs its indices into 64-bit item parameters");
// 4 bits of kind, then the song index for a song (songs are flat), or the artist (28 bits) and album
// (32 bits) for the others.
LPARAM Param(Kind kind, size_t a, size_t b = 0, size_t c = 0) {
    if (kind == kSong) return (LPARAM)((uint64_t)kind | (uint64_t)c << 4);
    return (LPARAM)((uint64_t)kind | ((uint64_t)a & 0xFFFFFFF) << 4 | (uint64_t)b << 32);
}
Kind KindOf(LPARAM p) { return (Kind)(p & 15); }
size_t ArtistOf(LPARAM p) { return (size_t)((uint64_t)p >> 4 & 0xFFFFFFF); }
size_t AlbumOf(LPARAM p) { return (size_t)((uint64_t)p >> 32); }
size_t SongOf(LPARAM p) { return (size_t)((uint64_t)p >> 4); }

int StateOf(const std::vector<char>& v, size_t first, size_t last) {
    size_t n = 0;
    for (size_t i = first; i < last; ++i) n += v[i] ? 1 : 0;
    return n == 0 ? kUnchecked : n == last - first ? kChecked : kPartial;
}

Range RangeOf(LPARAM p) {
    switch (KindOf(p)) {
        case kArtist: return s.artists[ArtistOf(p)];
        case kAlbum: return s.albums[ArtistOf(p)][AlbumOf(p)];
        case kSong: return {SongOf(p), SongOf(p) + 1};
        default: return {};
    }
}

int ShownState(LPARAM p) {
    if (KindOf(p) == kLists) return StateOf(s.lists, 0, s.lists.size());
    if (KindOf(p) == kList) return s.lists[ArtistOf(p)] ? kChecked : kUnchecked;
    const Range r = RangeOf(p);
    return StateOf(s.on, r.first, r.last);
}

LPARAM ParamOf(HTREEITEM item) {
    TVITEMW it{};
    it.mask = TVIF_PARAM | TVIF_HANDLE;
    it.hItem = item;
    TreeView_GetItem(s.tree, &it);
    return it.lParam;
}

void Show(HTREEITEM item, int state) {
    if (((TreeView_GetItemState(s.tree, item, TVIS_STATEIMAGEMASK) >> 12) & 15) != (UINT)state)
        TreeView_SetItemState(s.tree, item, INDEXTOSTATEIMAGEMASK(state), TVIS_STATEIMAGEMASK);
}

// Every box the tree holds shows the ticks again.
void Refresh(HTREEITEM item) {
    for (; item; item = TreeView_GetNextSibling(s.tree, item)) {
        Show(item, ShownState(ParamOf(item)));
        Refresh(TreeView_GetChild(s.tree, item));
    }
}

void Summarize() {
    size_t songs = 0, lists = 0;
    for (char c : s.on) songs += c ? 1 : 0;
    for (char c : s.lists) lists += c ? 1 : 0;
    std::wstring text = Songs(songs) + L" of " + std::to_wstring(s.on.size()) + L" and " + Playlists(lists) + L" ticked.";
    if (lists) text += L" A ticked playlist brings all its songs.";
    SetWindowTextW(s.summary, text.c_str());
    EnableWindow(s.go, songs || lists);
}

HTREEITEM Insert(HTREEITEM parent, const std::wstring& text, LPARAM param, bool children) {
    TVINSERTSTRUCTW ins{};
    ins.hParent = parent;
    ins.hInsertAfter = TVI_LAST;
    ins.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN | TVIF_STATE;
    ins.item.pszText = const_cast<wchar_t*>(text.c_str());
    ins.item.lParam = param;
    ins.item.cChildren = children ? 1 : 0;
    ins.item.stateMask = TVIS_STATEIMAGEMASK;
    ins.item.state = INDEXTOSTATEIMAGEMASK(ShownState(param));
    return TreeView_InsertItem(s.tree, &ins);
}

// An artist's albums or an album's songs, the first time it opens.
void Fill(HTREEITEM item) {
    if (TreeView_GetChild(s.tree, item)) return;
    const LPARAM p = ParamOf(item);
    const auto& artist = s.choices->artists[ArtistOf(p)];
    s.syncing = true;
    if (KindOf(p) == kArtist) {
        for (size_t b = 0; b < artist.albums.size(); ++b) {
            const auto& album = artist.albums[b];
            const std::wstring name = (album.name.empty() ? L"(no album)" : album.name) + L"  \x00B7  " + Songs(album.songs.size());
            Insert(item, name, Param(kAlbum, ArtistOf(p), b), true);
        }
    } else if (KindOf(p) == kAlbum) {
        const Range r = RangeOf(p);
        const auto& album = artist.albums[AlbumOf(p)];
        for (size_t i = r.first; i < r.last; ++i) Insert(item, album.songs[i - r.first].title, Param(kSong, 0, 0, i), false);
    }
    s.syncing = false;
}

// A box clicked or toggled with the space bar: its songs or playlists take the new tick.
void Toggled(HTREEITEM item, int before) {
    const LPARAM p = ParamOf(item);
    const bool tick = before != kChecked;  // a click on a partial box ticks all of it
    if (KindOf(p) == kLists) std::fill(s.lists.begin(), s.lists.end(), (char)tick);
    else if (KindOf(p) == kList) s.lists[ArtistOf(p)] = tick;
    else {
        const Range r = RangeOf(p);
        std::fill(s.on.begin() + (ptrdiff_t)r.first, s.on.begin() + (ptrdiff_t)r.last, (char)tick);
    }
    s.syncing = true;
    Refresh(TreeView_GetRoot(s.tree));
    s.syncing = false;
    Summarize();
}

void TickAll(bool tick) {
    std::fill(s.on.begin(), s.on.end(), (char)tick);
    std::fill(s.lists.begin(), s.lists.end(), (char)tick);
    s.syncing = true;
    Refresh(TreeView_GetRoot(s.tree));
    s.syncing = false;
    Summarize();
}

void Populate() {
    s.syncing = true;
    for (size_t a = 0; a < s.choices->artists.size(); ++a) {
        const auto& artist = s.choices->artists[a];
        const std::wstring name = (artist.name.empty() ? L"(no artist)" : artist.name) + L"  \x00B7  " + Songs(artist.songs);
        Insert(TVI_ROOT, name, Param(kArtist, a), true);
    }
    if (!s.lists.empty()) {
        HTREEITEM root = Insert(TVI_ROOT, L"Playlists", Param(kLists, 0), true);
        for (size_t i = 0; i < s.lists.size(); ++i) {
            const auto& list = s.choices->playlists[i];
            Insert(root, list.name + L"  \x00B7  " + Songs(list.songs), Param(kList, i), false);
        }
    }
    s.syncing = false;
}

LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            s.hwnd = h;
            auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
                HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, h, (HMENU)(INT_PTR)id, g.inst, nullptr);
                SendMessageW(c, WM_SETFONT, (WPARAM)g.font, TRUE);
                return c;
            };
            s.tree = make(WC_TREEVIEWW, L"",
                          TVS_CHECKBOXES | TVS_HASBUTTONS | TVS_LINESATROOT | TVS_SHOWSELALWAYS | TVS_FULLROWSELECT | WS_TABSTOP |
                              WS_VSCROLL,
                          kIdTree, WS_EX_CLIENTEDGE);
            TreeView_SetExtendedStyle(s.tree, TVS_EX_PARTIALCHECKBOXES | TVS_EX_DOUBLEBUFFER,
                                      TVS_EX_PARTIALCHECKBOXES | TVS_EX_DOUBLEBUFFER);
            ApplyControlTheme(s.tree, g.dark);
            TreeView_SetBkColor(s.tree, Ref(P().surface2));
            TreeView_SetTextColor(s.tree, TextColor());
            s.summary = make(WC_STATICW, L"", SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, kIdSummary);
            s.all = make(WC_BUTTONW, L"Tick &all", WS_TABSTOP, kIdAll);
            s.none = make(WC_BUTTONW, L"Tick &none", WS_TABSTOP, kIdNone);
            s.go = make(WC_BUTTONW, L"&Export...", WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK);
            s.cancel = make(WC_BUTTONW, L"Cancel", WS_TABSTOP, IDCANCEL);
            for (HWND c : {s.all, s.none, s.go, s.cancel}) ApplyControlTheme(c, g.dark);
            ApplyWindowTheme(h, P());
            Populate();
            Summarize();
            return 0;
        }
        case WM_SIZE: {
            RECT r;
            GetClientRect(h, &r);
            const int pad = S(12), by = r.bottom - pad - S(28);
            MoveWindow(s.tree, pad, pad, r.right - 2 * pad, std::max(S(60), by - S(34) - pad), TRUE);
            MoveWindow(s.summary, pad, by - S(28), r.right - 2 * pad, S(20), TRUE);
            MoveWindow(s.all, pad, by, S(96), S(28), TRUE);
            MoveWindow(s.none, pad + S(104), by, S(96), S(28), TRUE);
            MoveWindow(s.go, r.right - pad - S(196), by, S(100), S(28), TRUE);
            MoveWindow(s.cancel, r.right - pad - S(88), by, S(88), S(28), TRUE);
            return 0;
        }
        case WM_NOTIFY: {
            const auto* hdr = (const NMHDR*)lp;
            if (hdr->hwndFrom != s.tree) break;
            if (hdr->code == TVN_ITEMEXPANDINGW) {
                const auto* tv = (const NMTREEVIEWW*)lp;
                if (tv->action & TVE_EXPAND) Fill(tv->itemNew.hItem);
            } else if (hdr->code == TVN_ITEMCHANGEDW && !s.syncing) {
                const auto* c = (const NMTVITEMCHANGE*)lp;
                const UINT before = (c->uStateOld >> 12) & 15, after = (c->uStateNew >> 12) & 15;
                if ((c->uChanged & TVIF_STATE) && before != after && after) Toggled(c->hItem, before);
            }
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case kIdAll: TickAll(true); break;
                case kIdNone: TickAll(false); break;
                case IDOK:
                    s.accepted = true;
                    DestroyWindow(h);
                    break;
                case IDCANCEL: DestroyWindow(h); break;
            }
            return 0;
        case WM_CLOSE: DestroyWindow(h); return 0;
        case WM_DESTROY: s.hwnd = nullptr; return 0;
        case WM_ERASEBKGND: {
            RECT r;
            GetClientRect(h, &r);
            FillRect((HDC)wp, &r, g.surfaceBrush);
            return 1;
        }
        case WM_CTLCOLORSTATIC: return SurfaceControlColor((HDC)wp, TextColor());
        case WM_GETMINMAXINFO: ((MINMAXINFO*)lp)->ptMinTrackSize = {S(460), S(320)}; return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// Modal, like About: false when it was cancelled.
bool Choose(const collection::ExportChoices& choices, collection::ExportSelection& out) {
    s = State{};
    s.choices = &choices;
    for (const auto& artist : choices.artists) {
        Range ar{s.paths.size(), 0};
        auto& albums = s.albums.emplace_back();
        for (const auto& album : artist.albums) {
            Range al{s.paths.size(), 0};
            for (const auto& song : album.songs) s.paths.push_back(&song.path);
            al.last = s.paths.size();
            albums.push_back(al);
        }
        ar.last = s.paths.size();
        s.artists.push_back(ar);
    }
    s.on.assign(s.paths.size(), 1);
    s.lists.assign(choices.playlists.size(), 1);

    RegisterAppClass(L"CordinalExportPick", Proc);
    constexpr DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, exStyle = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
    RECT r{0, 0, S(560), S(520)};
    AdjustWindowRectExForDpi(&r, style, FALSE, exStyle, g.dpi);
    RECT owner;
    GetWindowRect(g.hwnd, &owner);
    const int width = r.right - r.left, height = r.bottom - r.top;
    EnableWindow(g.hwnd, FALSE);
    CreateWindowExW(exStyle, L"CordinalExportPick", L"Select what to export", style,
                    owner.left + (owner.right - owner.left - width) / 2,
                    owner.top + std::max<int>(S(40), (owner.bottom - owner.top - height) / 3), width, height, g.hwnd, nullptr,
                    g.inst, nullptr);
    if (s.hwnd) {
        ShowWindow(s.hwnd, SW_SHOW);
        SetFocus(s.tree);
        MSG msg{};
        while (s.hwnd && GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (IsDialogMessageW(s.hwnd, &msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (s.hwnd) {  // WM_QUIT arrived while it was open: let the main loop see it too
            PostQuitMessage((int)msg.wParam);
            s.accepted = false;
            DestroyWindow(s.hwnd);
        }
    }
    EnableWindow(g.hwnd, TRUE);
    SetForegroundWindow(g.hwnd);
    if (s.accepted) {
        for (size_t i = 0; i < s.paths.size(); ++i)
            if (s.on[i]) out.songs.push_back(*s.paths[i]);
        for (size_t i = 0; i < s.lists.size(); ++i)
            if (s.lists[i]) out.playlists.push_back(choices.playlists[i].name);
    }
    const bool accepted = s.accepted;
    s = State{};
    return accepted;
}

}  // namespace pick

// The two ways to export: everything, or a choice of artists, albums, songs and playlists.
enum class Scope { Cancel, Everything, Choose };
Scope AskScope(size_t songs, size_t lists) {
    enum { kEverything = 100, kChoose };
    const std::wstring everything = L"Export entire collection\n" + Songs(songs) + (lists ? L" and " + Playlists(lists) : L"");
    TASKDIALOG_BUTTON buttons[] = {{kEverything, everything.c_str()},
                                   {kChoose, L"Select what to export\nTick the artists, albums, songs and playlists to include"}};
    TASKDIALOGCONFIG config{sizeof(config)};
    config.hwndParent = g.hwnd;
    config.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
    config.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    config.pszWindowTitle = L"Export collection";
    config.pszMainInstruction = L"What do you want to export?";
    config.cButtons = (UINT)std::size(buttons);
    config.pButtons = buttons;
    config.nDefaultButton = kEverything;
    int choice = IDCANCEL;
    if (FAILED(TaskDialogIndirect(&config, &choice, nullptr, nullptr))) return Scope::Everything;
    return choice == kEverything ? Scope::Everything : choice == kChoose ? Scope::Choose : Scope::Cancel;
}

}  // namespace

bool CollectionBusy() { return g_running != Job::None || g_asking; }

std::wstring CollectionCancelText() {
    return g_running == Job::Export ? L"Cancel collection export" : L"Cancel collection import";
}

bool CollectionRunning() { return g_running != Job::None; }

void ExportCollection() {
    if (CollectionBusy()) {
        ShowMessage(L"Finish or cancel the collection export or import first.");
        return;
    }
    auto source = std::make_shared<collection::ExportSource>(
        collection::CaptureExport(g_lib, g.playlists, g.libraryFolders, g.convertShowOriginals));
    if (source->tracks.empty()) {
        ShowMessage(L"The library is empty, so there is nothing to export.");
        return;
    }
    g_asking = true;
    const Scope scope = AskScope(source->tracks.size(), source->playlists.size());
    if (scope == Scope::Choose) {
        collection::ExportSelection selection;
        const auto choices = collection::ListChoices(g_lib, g.playlists, g.convertShowOriginals);
        if (!pick::Choose(choices, selection)) {
            g_asking = false;
            return;
        }
        *source = collection::CaptureExport(g_lib, g.playlists, g.libraryFolders, g.convertShowOriginals, &selection);
    }
    const std::wstring zip = scope == Scope::Cancel ? std::wstring() : PickFile(true, L"Export collection");
    g_asking = false;
    if (zip.empty()) return;
    if (source->tracks.empty()) {
        ShowMessage(L"Nothing was ticked, so there is nothing to export.");
        return;
    }
    Start(Job::Export, [source, zip, progress = Progress(g.hwnd)](Done& d) {
        d.zip = zip;
        d.exported = collection::Export(*source, zip, g_cancel, progress);
    });
    ShowMessage(L"Exporting the collection...");
}

void ImportCollection() {
    if (CollectionBusy()) {
        ShowMessage(L"Finish or cancel the collection export or import first.");
        return;
    }
    g_asking = true;
    const std::wstring zip = PickFile(false, L"Import collection");
    g_asking = false;
    if (zip.empty()) return;
    Start(Job::Read, [zip](Done& d) {
        d.zip = zip;
        auto plan = std::make_shared<collection::ImportPlan>();
        if (collection::ReadCollection(zip, *plan, d.error)) d.plan = std::move(plan);
    });
    ShowMessage(L"Reading the collection...");
}

void CancelCollection() {
    if (g_running == Job::None) return;
    g_cancel = true;
    ShowMessage(g_running == Job::Export ? L"Cancelling the export..." : L"Cancelling the import...");
}

void OnCollectionMessage(WPARAM wp, LPARAM lp) {
    if (!wp) {
        if (g_running != Job::Export && g_running != Job::Import) return;
        const uint64_t total = g_totalBytes, bytes = g_bytes;
        const unsigned percent = total ? (unsigned)(bytes * 100 / total) : 0;
        ShowMessage((g_running == Job::Export ? L"Exporting the collection: " : L"Importing the collection: ") +
                    std::to_wstring(percent) + L"% (" + std::to_wstring((size_t)g_files) + L" of " +
                    std::to_wstring((size_t)g_totalFiles) + L" files)...");
        return;
    }
    std::unique_ptr<Done> d(reinterpret_cast<Done*>(lp));
    if (g_worker.joinable()) g_worker.join();
    g_running = Job::None;
    if (g_stopping || g.closing) return;
    if (!d->error.empty()) {
        ShowMessage(d->error);
        return;
    }
    if (d->job == Job::Export) FinishExport(*d);
    else if (d->job == Job::Read) {
        if (g_cancel) ShowMessage(L"Import cancelled.");
        else AskWhereToImport(std::move(d->plan));
    } else FinishImport(*d);
}

// Shutdown: the file in progress is abandoned, leaving nothing under its real name.
void StopCollection() {
    g_stopping = true;
    g_cancel = true;
    if (g_worker.joinable()) g_worker.join();
    MSG pending;
    while (PeekMessageW(&pending, g.hwnd, WM_APP_COLLECTION, WM_APP_COLLECTION, PM_REMOVE))
        if (pending.wParam) delete reinterpret_cast<Done*>(pending.lParam);
    g_running = Job::None;
}

}  // namespace app
