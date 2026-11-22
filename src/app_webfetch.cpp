// Save music from the web (Library > Save music from the web...): a small modeless
// window where pasted links become files you own. SoundCloud tracks, sets, likes and
// artists' pages, YouTube videos and playlists, and Spotify tracks, albums and
// playlists (matched to the same recording on YouTube, never decrypted from Spotify's
// stream) download one at a time on a worker, are tagged with their cover, and land in
// the first library folder as Artist\Album\[NN ]Title. What has been saved and what
// failed for good is remembered in the data folder, so nothing is fetched twice and
// dead tracks are left alone until "Forget what failed" asks for them again. The folder
// scanner picks the finished files up as they land, so they simply appear in the
// library. A link that holds a playlist (or an imported Spotify export or CSV) also
// becomes a cordinal playlist tab once the run is through: every track that saved, was
// already owned, or turned out to live in the library, in the list's own order.
#include "app.h"

#include "metadata_fetch.h"
#include "playlist_import.h"
#include "tagwriter.h"
#include "theme_win.h"
#include "webfetch.h"

#include <cctype>
#include <memory>
#include <thread>

namespace app {

namespace {

const wchar_t kWebFetchClass[] = L"CordinalWebFetch";
constexpr UINT kAdded = WM_APP + 1, kUpdated = WM_APP + 2, kNotice = WM_APP + 3, kDone = WM_APP + 4,
               kLists = WM_APP + 5;
enum { kIdPaste = 100, kIdList, kIdStatus, kIdSave, kIdStop, kIdForget, kIdImport, kIdClose };

struct Window {
    HWND hwnd = nullptr, paste = nullptr, list = nullptr, status = nullptr, save = nullptr,
         stop = nullptr, forget = nullptr, import = nullptr, close = nullptr;
    std::thread worker;
    std::atomic<bool> cancel{false};
    bool running = false;
    std::vector<WebFetchItem> items;  // the UI thread's mirror of the queue's list
    std::vector<WebFetchList> lists;  // the run's list spans, posted back on completion
    import::ImportPlan plan;         // an import's rows, for rebuilding order at the end
    // The playlist tabs saves and imports made, by list title (webfetch-tabs.json): a re-run,
    // even after a restart, updates its own tab and never one the user has.
    std::vector<WebTabMark> marks;
    bool marksLoaded = false;
} w;

void StopRun() {
    w.cancel = true;
    if (w.worker.joinable()) w.worker.join();
    w.running = false;
}

std::wstring TrackText(const WebFetchItem& it) {
    std::wstring title = FromUtf8(it.track.title);
    std::wstring artist = FromUtf8(it.track.artist);
    if (!artist.empty()) return artist + L" - " + title;
    return title.empty() ? FromUtf8(it.track.id) : title;
}

std::wstring StateText(const WebFetchItem& it) {
    switch (it.state) {
    case WebState::Resolving: return L"Reading...";
    case WebState::Downloading:
        return it.percent >= 0 ? L"Downloading... " + std::to_wstring(it.percent) + L"%" : L"Downloading...";
    case WebState::Saving: return L"Saving...";
    case WebState::Done: return L"Saved";
    case WebState::Exists: return L"Already yours";
    case WebState::Failed: return FromUtf8(it.why);
    }
    return L"";
}

void SetRow(int row, const WebFetchItem& it) {
    std::wstring track = TrackText(it), state = StateText(it);
    ListView_SetItemText(w.list, row, 0, track.data());
    ListView_SetItemText(w.list, row, 1, state.data());
}

int AddRow(const WebFetchItem& it) {
    std::wstring track = TrackText(it);
    LVITEMW item{LVIF_TEXT, (int)w.items.size() - 1};
    item.pszText = track.data();
    int row = ListView_InsertItem(w.list, &item);
    if (row >= 0) SetRow(row, it);
    return row;
}

// One paste can hold several links: the box takes whatever is separated by spaces or
// line breaks, the same way soundcli's add takes its arguments.
std::vector<std::string> LinksOf(const std::wstring& pasted) {
    std::vector<std::string> links;
    std::string all = ToUtf8(pasted);
    size_t at = 0;
    while (at < all.size()) {
        while (at < all.size() && std::isspace((unsigned char)all[at])) at++;
        size_t end = at;
        while (end < all.size() && !std::isspace((unsigned char)all[end])) end++;
        if (end > at) links.push_back(all.substr(at, end - at));
        at = end;
    }
    return links;
}

// The first save asks what would be sent, the same way Artist info asks before its
// first lookup; the answer lives in Preferences > Online and can be changed there.
bool EnsureWebSave() {
    if (online::Config().saveMusic) return true;
    const wchar_t* text =
        L"Save music from the web is off, so cordinal hasn't contacted anyone.\n\n"
        L"It downloads the link you paste from that site: the audio, its cover and its details, "
        L"saved as your own file in the library. A Spotify link's audio is the same recording "
        L"matched on YouTube.\n\nTurn Save music from the web on? It can be changed in Preferences > Online.";
    if (MessageBoxW(g.hwnd, text, L"Save music from the web", MB_YESNO | MB_ICONQUESTION) != IDYES) return false;
    if (const Tweak* t = FindTweak("online.save_music")) SetTweak(*t, 1);
    return online::Config().saveMusic;
}

// The library's own answer to "is this song already mine?", captured in one pass so the
// import split and the finished playlists can match rows without touching the live
// library again.
void CaptureOwned(import::OwnedIndex& owned) {
    const size_t count = g_lib.Count();
    for (size_t i = 0; i < count; ++i) {
        const Track& t = g_lib[(TrackId)i];
        if ((t.flags & (Track::kInLibrary | Track::kInbox)) == 0) continue;
        if (t.flags & (Track::kMissing | Track::kExcluded | Track::kOriginal)) continue;
        if (!t.artist || !t.title) continue;
        owned.Add(g_lib.View(t.artist), g_lib.View(t.title), g_lib.Path((TrackId)i));
    }
}

// The playback tabs a save or import made live in webfetch-tabs.json, so they are found
// again after a restart.
void LoadMarks() {
    if (w.marksLoaded) return;
    w.marksLoaded = true;
    LoadWebTabMarks(g.dataDir, w.marks);
}

// What a playlist holds now, as the fingerprint a mark stores.
uint64_t PlaylistFiles(const Playlist& pl) {
    std::vector<std::wstring> files;
    files.reserve(pl.items.size());
    for (TrackId id : pl.items)
        if (g_lib.Contains(id)) files.push_back(g_lib.Path(id));
    return WebTabFingerprint(files);
}

// The tab a mark still owns: the named playlist, still holding exactly the files cordinal
// last wrote there. -1 when it is gone, renamed, or edited since - a user's tab is never
// written over.
int MarkedPlaylist(const WebTabMark& mark) {
    for (int i = 0; i < (int)g.playlists.size(); ++i) {
        const Playlist& pl = g.playlists[i];
        if (pl.kind != PlaylistKind::Static) continue;
        if (CompareStringOrdinal(pl.name.c_str(), -1, mark.name.c_str(), -1, TRUE) != CSTR_EQUAL) continue;
        if (PlaylistFiles(pl) == mark.seen) return i;
    }
    return -1;
}

// One finished list becomes a playlist tab - or updates the tab an earlier save made under
// that title, undoably, so a stopped and resumed import lands in one place. A tab the user
// made, or edited since, is never touched: a new name is found instead. `favorites` hearts
// what the list holds (Spotify's saved tracks, the one import whose rows are hearts).
void MarkFavorites(const std::vector<TrackId>& ids) {
    bool changed = false;
    for (TrackId id : ids) {
        if (!g_lib.Contains(id) || g_lib.Favorite(id)) continue;
        g_lib.SetFavorite(id, true);
        changed = true;
    }
    if (!changed) return;
    g_lib.Touch();
    g.dirty = true;
}

void MakeWebPlaylist(const std::wstring& title, const std::vector<std::wstring>& files, bool favorites = false) {
    if (files.empty()) return;
    LoadMarks();
    int at = -1;
    if (const WebTabMark* mark = FindWebTabMark(w.marks, title)) at = MarkedPlaylist(*mark);
    // The ids for the files as they now stand; a re-reported file stays one id.
    std::vector<TrackId> ids, unread;
    for (const std::wstring& file : files) {
        const TrackId id = g_lib.Add(file);
        if (id == kInvalidTrack) continue;
        ids.push_back(id);
        if (!g_lib[id].Loaded()) unread.push_back(id);
    }
    QueueTags(unread);
    if (at >= 0) {
        Playlist& pl = g.playlists[at];
        if (pl.items != ids) {
            RecordPlaylistEdit(pl, pl.items, L"web save");
            pl.items = std::move(ids);
            if (at == g.active) RebuildView(true);
            RefreshTabs();
            RefreshNext();
            g.dirty = true;
        }
        if (favorites) MarkFavorites(pl.items);
        SetWebTabMark(w.marks, title, pl.name, PlaylistFiles(pl));
        SaveWebTabMarks(g.dataDir, w.marks);
        return;
    }
    NewPlaylist(title);  // finds a free name, makes the tab and activates it
    const int index = (int)g.playlists.size() - 1;
    AppendFiles(index, files, false);
    if (favorites) MarkFavorites(g.playlists[index].items);
    const Playlist& pl = g.playlists[index];
    SetWebTabMark(w.marks, title, pl.name, PlaylistFiles(pl));
    SaveWebTabMarks(g.dataDir, w.marks);
}

// Every list the finished run holds becomes a playlist: an import's rows through the
// plan's slots, a link-resolved playlist through its items (a failed track still takes
// its place when the library has a copy of the song).
void FinishLists() {
    if (w.lists.empty()) return;
    import::OwnedIndex owned;
    bool ownedBuilt = false;
    for (size_t li = 0; li < w.lists.size(); ++li) {
        const WebFetchList& span = w.lists[li];
        std::vector<std::wstring> files;
        if (li < w.plan.lists.size()) {
            // The import's missing rows are exactly the span's items, so the slots
            // rebuild its order; the plan's copies of the library stay as they were.
            const import::ImportPlanList& list = w.plan.lists[li];
            if (span.count != list.missing.size()) continue;  // not the run this plan made
            files = import::AssembleFiles(w.items, span.first, list.slots, owned);
            MakeWebPlaylist(FromUtf8(list.title), files, list.favorites);
        } else {
            if (!span.playlist || span.count == 0 || span.title.empty()) continue;
            if (!ownedBuilt) {
                CaptureOwned(owned);
                ownedBuilt = true;
            }
            files = import::AssembleFiles(w.items, span.first, import::RangeSlots(span.count), owned);
            MakeWebPlaylist(FromUtf8(span.title), files);
        }
    }
}

// The shape every run start shares: one list in the window, a status line, the worker.
void StartWorker(WebFetchRun run) {
    SetWindowTextW(w.status, L"Reading the links...");
    EnableWindow(w.save, FALSE);
    EnableWindow(w.import, FALSE);
    EnableWindow(w.stop, TRUE);
    w.cancel = false;
    w.running = true;
    w.items.clear();
    w.lists.clear();
    ListView_DeleteAllItems(w.list);
    const HWND target = w.hwnd;
    w.worker = std::thread([run = std::move(run), target]() mutable {
        // Each post owns what it carries; a window gone before delivery frees it here.
        auto post = [target](UINT msg, WPARAM wp, auto* owned) {
            if (!PostMessageW(target, msg, wp, (LPARAM)owned)) delete owned;
        };
        run.added = [&](const WebFetchItem& item) { post(kAdded, 0, new WebFetchItem(item)); };
        run.updated = [&](size_t index, const WebFetchItem& item) {
            post(kUpdated, (WPARAM)index, new WebFetchItem(item));
        };
        run.notice = [&](const std::string& text) { post(kNotice, 0, new std::string(text)); };
        std::vector<WebFetchItem> items;
        std::vector<WebFetchList> lists;
        std::string error;
        RunWebFetch(run, items, error, &lists);
        post(kLists, 0, new std::vector<WebFetchList>(std::move(lists)));
        // The summary the window shows once the queue is through: what was saved, what
        // was already owned, and what failed, in words.
        unsigned saved = 0, had = 0, failed = 0;
        for (const WebFetchItem& it : items) {
            if (it.state == WebState::Done) saved++;
            else if (it.state == WebState::Exists) had++;
            else if (it.state == WebState::Failed) failed++;
        }
        std::wstring summary = std::to_wstring(saved) + L" saved, " + std::to_wstring(had) +
                               L" already yours, " + std::to_wstring(failed) + L" failed.";
        if (!error.empty()) summary += L"  (" + FromUtf8(error) + L")";
        post(kDone, 0, new std::wstring(std::move(summary)));
    });
}

// A parsed import, made into playlists: the rows the library already owns keep their
// files, the rest go through the save queue, and every list lands as its own tab in the
// file's own order. When everything is owned, no one is contacted at all.
void StartImport(std::vector<import::ImportList> lists, std::vector<std::string> albumLinks) {
    if (w.running || !w.hwnd) return;
    import::OwnedIndex owned;
    CaptureOwned(owned);
    import::ImportPlan plan;
    import::SplitByLibrary(lists, owned, plan);
    plan.albumLinks = std::move(albumLinks);
    if (plan.lists.empty() && plan.albumLinks.empty()) {
        SetWindowTextW(w.status, L"Nothing in that file could be imported.");
        return;
    }
    if (plan.MissingCount() == 0 && plan.albumLinks.empty()) {
        // Every row is already a file the user owns: the playlists can simply be made.
        for (const import::ImportPlanList& list : plan.lists)
            MakeWebPlaylist(FromUtf8(list.title), import::AssembleFiles({}, 0, list.slots, owned), list.favorites);
        SetWindowTextW(w.status, (std::to_wstring(plan.OwnedCount()) +
                                  L" tracks were already in your library; the playlists are made.")
                             .c_str());
        return;
    }
    if (!EnsureWebSave()) return;
    if (g.libraryFolders.empty()) {
        SetWindowTextW(w.status, L"Add a library folder first (Library > Library folders...).");
        return;
    }
    WebFetchRun run;
    for (const import::ImportPlanList& list : plan.lists) run.importLists.push_back({list.title, list.missing});
    run.links = plan.albumLinks;
    run.libraryRoot = g.libraryFolders.front();
    run.dataDir = g.dataDir;
    run.cancel = &w.cancel;
    w.plan = std::move(plan);
    SetWindowTextW(w.status, (L"Importing " + std::to_wstring(w.plan.TrackCount()) + L" tracks: " +
                                 std::to_wstring(w.plan.OwnedCount()) + L" already in your library...")
                             .c_str());
    StartWorker(std::move(run));
}

void ImportText(const std::string& text, const std::wstring& fallbackTitle) {
    std::vector<import::ImportList> lists;
    std::vector<std::string> albumLinks;
    std::string error;
    if (!import::ParseImportText(text, fallbackTitle, lists, albumLinks, error)) {
        SetWindowTextW(w.status, FromUtf8(error).c_str());
        return;
    }
    StartImport(std::move(lists), std::move(albumLinks));
}

// The Import file... button: one Spotify export or CSV, named by the file's own stem.
void ImportFromFile() {
    if (w.running) return;
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    DWORD options = 0;
    dlg->GetOptions(&options);
    dlg->SetOptions(options | FOS_FORCEFILESYSTEM);
    COMDLG_FILTERSPEC filters[] = {{L"Spotify export or CSV playlist", L"*.json;*.csv"}, {L"All files", L"*.*"}};
    dlg->SetFileTypes((UINT)std::size(filters), filters);
    dlg->SetTitle(L"Import a Spotify export or a CSV playlist");
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dlg->Show(w.hwnd)) || FAILED(dlg->GetResult(&item)) ||
        FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        return;
    std::wstring file = path;
    CoTaskMemFree(path);
    std::vector<import::ImportList> lists;
    std::vector<std::string> albumLinks;
    std::string error;
    if (!import::ParseImportFile(file, lists, albumLinks, error)) {
        SetWindowTextW(w.status, FromUtf8(error).c_str());
        return;
    }
    StartImport(std::move(lists), std::move(albumLinks));
}

void TrySave() {
    if (w.running || !w.hwnd) return;
    const int length = GetWindowTextLengthW(w.paste);
    std::wstring pasted((size_t)std::max(0, length), L'\0');
    GetWindowTextW(w.paste, pasted.data(), length + 1);
    // Pasted export text, or the path of an export file, is an import; anything else is links.
    const std::string all = ToUtf8(pasted);
    if (import::LooksLikeImport(all)) {
        ImportText(all, L"Imported playlist");
        return;
    }
    if (import::IsImportFileName(pasted) && GetFileAttributesW(pasted.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::string text;
        if (!ReadFileBytes(pasted, text)) {
            SetWindowTextW(w.status, L"That file could not be read.");
            return;
        }
        ImportText(text, StemOf(pasted));
        return;
    }
    std::vector<std::string> links = LinksOf(pasted);
    if (links.empty()) {
        SetWindowTextW(w.status, L"Paste a link first: a SoundCloud, YouTube or Spotify address.");
        return;
    }
    if (!EnsureWebSave()) return;
    if (g.libraryFolders.empty()) {
        SetWindowTextW(w.status, L"Add a library folder first (Library > Library folders...).");
        return;
    }
    WebFetchRun run;
    run.links = links;
    run.libraryRoot = g.libraryFolders.front();
    run.dataDir = g.dataDir;
    run.cancel = &w.cancel;
    w.plan = import::ImportPlan{};
    StartWorker(std::move(run));
}

void LayoutWindow() {
    RECT rc;
    GetClientRect(w.hwnd, &rc);
    const int pad = S(14);
    const int width = rc.right - pad * 2;
    const int rowH = S(30), bottom = rc.bottom - pad - rowH;
    SetWindowPos(w.paste, nullptr, pad, pad, std::max(S(100), width - S(110)), S(26), SWP_NOZORDER);
    SetWindowPos(w.save, nullptr, rc.right - pad - S(100), pad, S(100), S(26), SWP_NOZORDER);
    SetWindowPos(w.list, nullptr, pad, pad + S(34), std::max(S(100), width),
                 std::max(S(60), bottom - S(34) - pad - S(34)), SWP_NOZORDER);
    SetWindowPos(w.status, nullptr, pad, bottom - S(26), std::max(S(100), width), S(20), SWP_NOZORDER);
    int x = pad;
    for (HWND b : {w.import, w.stop, w.forget}) {
        SetWindowPos(b, nullptr, x, bottom, S(130), S(30), SWP_NOZORDER);
        x += S(136);
    }
    SetWindowPos(w.close, nullptr, rc.right - pad - S(100), bottom, S(100), S(30), SWP_NOZORDER);
    const int scrollbar = GetSystemMetricsForDpi(SM_CXVSCROLL, g.dpi);
    ListView_SetColumnWidth(w.list, 1, std::max(S(120), width - S(320) - scrollbar));
}

LRESULT CALLBACK WebFetchProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            w.hwnd = h;
            auto make = [&](DWORD ex, const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
                HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, h,
                                         (HMENU)(INT_PTR)id, g.inst, nullptr);
                SendMessageW(c, WM_SETFONT, (WPARAM)g.font, TRUE);
                return c;
            };
            w.paste = make(WS_EX_CLIENTEDGE, WC_EDITW, L"", ES_AUTOHSCROLL | WS_TABSTOP, kIdPaste);
            SetWindowTheme(w.paste, g.dark ? L"DarkMode_CFD" : L"Explorer", nullptr);
            w.save = make(0, WC_BUTTONW, L"&Save", BS_DEFPUSHBUTTON | WS_TABSTOP, kIdSave);
            w.list = make(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                          LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER | WS_TABSTOP, kIdList);
            ListView_SetExtendedListViewStyle(w.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            const wchar_t* columns[] = {L"Track", L"Status"};
            const int widths[] = {S(320), S(160)};
            for (int c = 0; c < 2; ++c) {
                LVCOLUMNW col{};
                col.mask = LVCF_TEXT | LVCF_WIDTH;
                col.cx = widths[c];
                col.pszText = const_cast<wchar_t*>(columns[c]);
                ListView_InsertColumn(w.list, c, &col);
            }
            for (HWND c : {w.list, ListView_GetHeader(w.list)}) ApplyControlTheme(c, g.dark);
            ListView_SetBkColor(w.list, Ref(P().surface2));
            ListView_SetTextBkColor(w.list, Ref(P().surface2));
            ListView_SetTextColor(w.list, TextColor());
            w.status = make(0, WC_STATICW, L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, kIdStatus);
            w.stop = make(0, WC_BUTTONW, L"&Stop", BS_PUSHBUTTON | WS_TABSTOP, kIdStop);
            w.forget = make(0, WC_BUTTONW, L"Forgot what &failed", BS_PUSHBUTTON | WS_TABSTOP, kIdForget);
            w.import = make(0, WC_BUTTONW, L"Import &file...", BS_PUSHBUTTON | WS_TABSTOP, kIdImport);
            w.close = make(0, WC_BUTTONW, L"Close", BS_PUSHBUTTON | WS_TABSTOP, kIdClose);
            for (HWND c : {w.save, w.stop, w.forget, w.import, w.close}) ApplyControlTheme(c, g.dark);
            EnableWindow(w.stop, FALSE);
            // A Spotify export or a CSV can simply be dropped on the window.
            DragAcceptFiles(h, TRUE);
            ApplyWindowTheme(h, P());
            LayoutWindow();
            return 0;
        }
        case WM_SIZE:
            LayoutWindow();
            return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {S(520), S(360)};
            return 0;
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(h, &rc);
            FillRect((HDC)wp, &rc, g.surfaceBrush);
            return 1;
        }
        case WM_CTLCOLORSTATIC:
            return SurfaceControlColor((HDC)wp, (HWND)lp == w.status ? SubColor() : TextColor());
        case WM_CTLCOLORBTN:
            return SurfaceControlColor((HDC)wp, TextColor());
        case WM_CTLCOLOREDIT:
            return FieldControlColor((HDC)wp);
        case WM_DROPFILES: {
            // An export or CSV dropped here imports; anything else is not this window's.
            HDROP drop = (HDROP)wp;
            UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            std::wstring first;
            for (UINT i = 0; i < count; ++i) {
                UINT len = DragQueryFileW(drop, i, nullptr, 0);
                std::wstring p(len + 1, L'\0');
                DragQueryFileW(drop, i, p.data(), len + 1);
                p.resize(len);
                if (import::IsImportFileName(p)) {
                    first = p;
                    break;
                }
            }
            DragFinish(drop);
            if (first.empty() || w.running) return 0;
            std::string text;
            if (!ReadFileBytes(first, text)) {
                SetWindowTextW(w.status, L"That file could not be read.");
                return 0;
            }
            ImportText(text, StemOf(first));
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDOK:
                case kIdSave: TrySave(); break;
                case kIdStop:
                    w.cancel = true;
                    SetWindowTextW(w.status, L"Stopping after the track in flight...");
                    break;
                case kIdForget:
                    if (WebFetchForgetFailures(g.dataDir))
                        SetWindowTextW(w.status, L"Forgotten. The next save tries those tracks again.");
                    break;
                case kIdImport: ImportFromFile(); break;
                case kIdClose:
                case IDCANCEL: DestroyWindow(h); break;
            }
            return 0;
        case kAdded: {
            std::unique_ptr<WebFetchItem> item(reinterpret_cast<WebFetchItem*>(lp));
            w.items.push_back(*item);
            AddRow(*item);
            return 0;
        }
        case kUpdated: {
            std::unique_ptr<WebFetchItem> item(reinterpret_cast<WebFetchItem*>(lp));
            if (wp < w.items.size()) {
                w.items[wp] = *item;
                SetRow((int)wp, *item);
            }
            return 0;
        }
        case kNotice: {
            std::unique_ptr<std::string> text(reinterpret_cast<std::string*>(lp));
            SetWindowTextW(w.status, FromUtf8(*text).c_str());
            return 0;
        }
        case kLists: {
            std::unique_ptr<std::vector<WebFetchList>> lists(reinterpret_cast<std::vector<WebFetchList>*>(lp));
            w.lists = std::move(*lists);
            return 0;
        }
        case kDone: {
            std::unique_ptr<std::wstring> summary(reinterpret_cast<std::wstring*>(lp));
            if (w.worker.joinable()) w.worker.join();
            w.running = false;
            EnableWindow(w.save, TRUE);
            EnableWindow(w.import, TRUE);
            EnableWindow(w.stop, FALSE);
            FinishLists();
            std::wstring text = *summary;
            if (!w.plan.lists.empty() && w.plan.OwnedCount() > 0)
                text += L"  (" + std::to_wstring(w.plan.OwnedCount()) + L" more were already in your library.)";
            SetWindowTextW(w.status, text.c_str());
            w.plan = import::ImportPlan{};
            w.lists.clear();
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY: {
            StopRun();
            MSG pending;
            for (UINT drain : {kAdded, kUpdated, kNotice, kDone, kLists})
                while (PeekMessageW(&pending, h, drain, drain, PM_REMOVE)) {
                    if (drain == kNotice) delete reinterpret_cast<std::string*>(pending.lParam);
                    else if (drain == kDone) delete reinterpret_cast<std::wstring*>(pending.lParam);
                    else if (drain == kLists) delete reinterpret_cast<std::vector<WebFetchList>*>(pending.lParam);
                    else delete reinterpret_cast<WebFetchItem*>(pending.lParam);
                }
            w.hwnd = nullptr;
            w.items.clear();
            w.lists.clear();
            w.plan = import::ImportPlan{};
            ListView_DeleteAllItems(w.list);
            return 0;
        }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

void ShowWebFetch() {
    if (w.hwnd) {
        ShowWindow(w.hwnd, SW_RESTORE);
        SetForegroundWindow(w.hwnd);
        SetFocus(w.paste);
        return;
    }
    RegisterAppClass(kWebFetchClass, WebFetchProc);
    RECT r{0, 0, S(640), S(480)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_DLGMODALFRAME, g.dpi);
    RECT owner;
    GetWindowRect(g.hwnd, &owner);
    const int width = r.right - r.left, height = r.bottom - r.top;
    CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kWebFetchClass, L"Save music from the web",
                    WS_OVERLAPPEDWINDOW & ~WS_MINIMIZEBOX & ~WS_MAXIMIZEBOX | WS_CLIPCHILDREN,
                    owner.left + (owner.right - owner.left - width) / 2, owner.top + S(80), width, height, g.hwnd,
                    nullptr, g.inst, nullptr);
    if (!w.hwnd) return;
    ShowWindow(w.hwnd, SW_SHOW);
    SetFocus(w.paste);
}

bool WebFetchDialogMessage(MSG* msg) { return w.hwnd && IsDialogMessageW(w.hwnd, msg); }

// Release Radar's save: albums go straight into the queue as files, not playlist tabs, and the
// window shows them being saved with everything else.
void ShowWebFetchLists(std::vector<WebFetchTrackList> lists) {
    if (lists.empty()) return;
    ShowWebFetch();
    if (!w.hwnd) return;
    if (w.running) {
        SetWindowTextW(w.status, L"A save is already running. Stop it or wait for it to finish.");
        return;
    }
    if (!EnsureWebSave()) return;
    if (g.libraryFolders.empty()) {
        SetWindowTextW(w.status, L"Add a library folder first (Library > Library folders...).");
        return;
    }
    WebFetchRun run;
    for (WebFetchTrackList& list : lists) run.importLists.push_back(std::move(list));
    run.libraryRoot = g.libraryFolders.front();
    run.dataDir = g.dataDir;
    run.cancel = &w.cancel;
    w.plan = import::ImportPlan{};
    SetWindowTextW(w.status, (L"Saving " + FromUtf8(run.importLists.front().title) +
                              (run.importLists.size() > 1 ? L" and more..." : L"...")).c_str());
    StartWorker(std::move(run));
}

void StopWebFetch() {
    if (w.hwnd) DestroyWindow(w.hwnd);
    StopRun();
}

}  // namespace app
