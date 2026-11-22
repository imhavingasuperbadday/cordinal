// Save music from the web on Linux (Library > Save music from the web...), as app_webfetch.cpp does on
// Windows with the same wording: pasted SoundCloud, YouTube and Spotify links are resolved and saved one
// at a time on a worker into the first library folder, tagged and filed as Artist/Album/[NN ]Title. The
// panel lists each track as it is read, downloaded and saved, and remembers what already belongs to you
// and what failed for good, so nothing is fetched twice and "Forgot what failed" asks for the dead ones
// again. A link that holds a playlist, and an imported Spotify export or CSV, also becomes a cordinal
// playlist tab once the run is through, in the list's own order. webfetch.h does the work; this file
// only posts its callbacks to the UI thread.
#include "panels_internal.h"

#include "draw.h"

#include "metadata_fetch.h"
#include "playlist_import.h"
#include "tagwriter.h"
#include "ui_internal.h"
#include "webfetch.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <memory>
#include <thread>

namespace lx {

namespace {

// One message to the UI thread: which run it came from, the row it is about, and a notice or the
// summary's words. A closed panel has moved on to a newer generation and drops what it gets.
struct Posted {
    uint64_t generation = 0;
    WebFetchItem item;
    std::string text;
};

// The run's list spans arrive before the summary, so the panel knows what to make playlists of.
struct PostedLists {
    uint64_t generation = 0;
    std::vector<WebFetchList> lists;
};

struct WebFetchState {
    bool open = false;
    bool running = false;
    std::string status;
    panel::TextField paste;
    std::vector<WebFetchItem> items;  // the UI thread's mirror of the queue's list
    std::vector<WebFetchList> lists;  // the run's list spans, posted back on completion
    import::ImportPlan plan;         // an import's rows, for rebuilding order at the end
    float scroll = 0;
    uint64_t generation = 0;
    std::thread worker;
} f;

// The worker's cancel flag outlives the state reset: a thread still winding down reads it.
std::atomic<bool> g_cancel{false};

// The playlist tabs saves and imports made, by list title (webfetch-tabs.json): a re-run, even
// after a restart, updates its own tab and never one the user has.
std::vector<WebTabMark> g_marks;
bool g_marksLoaded = false;

void StopRun() {
    g_cancel = true;
    if (f.worker.joinable()) f.worker.join();
    f.running = false;
    g_cancel = false;
}

std::string TrackText(const WebFetchItem& it) {
    const std::string& title = it.track.title;
    const std::string& artist = it.track.artist;
    if (!artist.empty()) return artist + " - " + title;
    return title.empty() ? it.track.id : title;
}

// The status column's words are the Windows window's (app_webfetch.cpp); a failure says its own reason.
std::string StateText(const WebFetchItem& it) {
    switch (it.state) {
    case WebState::Resolving: return "Reading...";
    case WebState::Downloading:
        return it.percent >= 0 ? "Downloading... " + std::to_string(it.percent) + "%" : "Downloading...";
    case WebState::Saving: return "Saving...";
    case WebState::Done: return "Saved";
    case WebState::Exists: return "Already yours";
    case WebState::Failed: return it.why;
    }
    return "";
}

// One paste can hold several links: the box takes whatever is separated by spaces or line breaks, the
// same rule the Windows window's LinksOf uses.
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

// The first save asks what would be sent, the same way Artist info asks before its first lookup on
// this platform; the answer lives in Preferences > Online and can be changed there.
bool EnsureWebSave() {
    if (online::Config().saveMusic) return true;
    if (!Confirm("Save music from the web is off, so cordinal hasn't contacted anyone.\n\n"
                 "It downloads the link you paste from that site: the audio, its cover and its details, "
                 "saved as your own file in the library. A Spotify link's audio is the same recording "
                 "matched on YouTube.\n\nTurn Save music from the web on? It can be changed in "
                 "Preferences > Online.",
                 "Save music from the web"))
        return false;
    online::Config().saveMusic = true;
    SaveSettings();
    return true;
}

// The library's own answer to "is this song already mine?", captured in one pass so the import split
// and the finished playlists can match rows without touching the live library again.
void CaptureOwned(import::OwnedIndex& owned) {
    const size_t count = g_lib.Count();
    for (size_t i = 0; i < count; ++i) {
        const Track& t = g_lib[i];
        if ((t.flags & (Track::kInLibrary | Track::kInbox)) == 0) continue;
        if (t.flags & (Track::kMissing | Track::kExcluded | Track::kOriginal)) continue;
        if (!t.artist || !t.title) continue;
        owned.Add(g_lib.View(t.artist), g_lib.View(t.title), g_lib.Path((TrackId)i));
    }
}

// The marks in webfetch-tabs.json, so a tab made before a restart is found again.
void LoadMarks() {
    if (g_marksLoaded) return;
    g_marksLoaded = true;
    LoadWebTabMarks(g.dataDir, g_marks);
}

// What a playlist holds now, as the fingerprint a mark stores.
uint64_t PlaylistFiles(const Playlist& pl) {
    std::vector<std::wstring> files;
    files.reserve(pl.items.size());
    for (TrackId id : pl.items)
        if (g_lib.Contains(id)) files.push_back(g_lib.Path(id));
    return WebTabFingerprint(files);
}

// The tab a mark still owns: the named playlist, still holding exactly the files cordinal last
// wrote there. -1 when it is gone, renamed, or edited since - a user's tab is never written over.
int MarkedPlaylist(const WebTabMark& mark) {
    for (int i = 0; i < (int)g.playlists.size(); ++i) {
        const Playlist& pl = g.playlists[i];
        if (pl.kind != PlaylistKind::Static) continue;
        if (CompareStringOrdinal(pl.name.c_str(), -1, mark.name.c_str(), -1, TRUE) != CSTR_EQUAL) continue;
        if (PlaylistFiles(pl) == mark.seen) return i;
    }
    return -1;
}

// One finished list becomes a playlist tab - or updates the tab an earlier save made under that
// title, undoably, so a stopped and resumed import lands in one place. A tab the user made, or
// edited since, is never touched: a new name is found instead. `favorites` hearts what the list
// holds (Spotify's saved tracks, the one import whose rows are hearts).
void MarkFavorites(const std::vector<TrackId>& ids) {
    bool changed = false;
    for (TrackId id : ids) {
        if (!g_lib.Contains(id) || g_lib.Favorite(id)) continue;
        g_lib.SetFavorite(id, true);
        changed = true;
    }
    if (!changed) return;
    g_lib.Touch();
    g.libraryDirty = true;
}

void MakeWebPlaylist(const std::wstring& title, const std::vector<std::wstring>& files, bool favorites = false) {
    if (files.empty()) return;
    LoadMarks();
    int at = -1;
    if (const WebTabMark* mark = FindWebTabMark(g_marks, title)) at = MarkedPlaylist(*mark);
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
        if (g.playlists[at].items != ids) {
            RecordPlaylistEdit(g.playlists[at], g.playlists[at].items, L"web save");
            g.playlists[at].items = std::move(ids);
            if (at == g.active) RebuildRows();
            g.libraryDirty = true;
        }
        if (favorites) MarkFavorites(g.playlists[at].items);
        SetWebTabMark(g_marks, title, g.playlists[at].name, PlaylistFiles(g.playlists[at]));
        SaveWebTabMarks(g.dataDir, g_marks);
        return;
    }
    Playlist playlist{UniquePlaylistName(g.playlists, title), {}, PlaylistKind::Static};
    playlist.identity = g_nextPlaylistIdentity++;
    playlist.items = std::move(ids);
    g.playlists.push_back(std::move(playlist));
    if (favorites) MarkFavorites(g.playlists.back().items);
    SetWebTabMark(g_marks, title, g.playlists.back().name, PlaylistFiles(g.playlists.back()));
    SaveWebTabMarks(g.dataDir, g_marks);
    g.libraryDirty = true;
    SetActiveTab((int)g.playlists.size() - 1);
}

// Every list the finished run holds becomes a playlist: an import's rows through the plan's slots, a
// link-resolved playlist through its items (a failed track still takes its place when the library has
// a copy of the song).
void FinishLists() {
    if (f.lists.empty()) return;
    import::OwnedIndex owned;
    bool ownedBuilt = false;
    for (size_t li = 0; li < f.lists.size(); ++li) {
        const WebFetchList& span = f.lists[li];
        std::vector<std::wstring> files;
        if (li < f.plan.lists.size()) {
            // The import's missing rows are exactly the span's items, so the slots rebuild its order.
            const import::ImportPlanList& list = f.plan.lists[li];
            if (span.count != list.missing.size()) continue;  // not the run this plan made
            files = import::AssembleFiles(f.items, span.first, list.slots, owned);
            MakeWebPlaylist(FromUtf8(list.title), files, list.favorites);
        } else {
            if (!span.playlist || span.count == 0 || span.title.empty()) continue;
            if (!ownedBuilt) {
                CaptureOwned(owned);
                ownedBuilt = true;
            }
            files = import::AssembleFiles(f.items, span.first, import::RangeSlots(span.count), owned);
            MakeWebPlaylist(FromUtf8(span.title), files);
        }
    }
}

void StartWorker(WebFetchRun run) {
    f.status = "Reading the links...";
    f.items.clear();
    f.lists.clear();
    f.scroll = 0;
    f.running = true;
    g_cancel = false;
    const uint64_t generation = f.generation;
    f.worker = std::thread([run = std::move(run), generation]() mutable {
        // Each post owns what it carries; a panel closed before delivery frees it here.
        auto post = [generation](UINT msg, WPARAM wp, auto* owned) {
            if (!PostMessageW(nullptr, msg, wp, (LPARAM)owned)) delete owned;
        };
        run.added = [&](const WebFetchItem& item) {
            post(WM_APP_WEBFETCH_ADDED, 0, new Posted{generation, item, {}});
        };
        run.updated = [&](size_t index, const WebFetchItem& item) {
            post(WM_APP_WEBFETCH_UPDATED, (WPARAM)index, new Posted{generation, item, {}});
        };
        run.notice = [&](const std::string& text) {
            post(WM_APP_WEBFETCH_NOTICE, 0, new Posted{generation, {}, text});
        };
        std::vector<WebFetchItem> items;
        std::vector<WebFetchList> lists;
        std::string error;
        RunWebFetch(run, items, error, &lists);
        post(WM_APP_WEBFETCH_LISTS, 0, new PostedLists{generation, std::move(lists)});
        // The summary the panel shows once the queue is through, in the Windows window's words.
        unsigned saved = 0, had = 0, failed = 0;
        for (const WebFetchItem& it : items) {
            if (it.state == WebState::Done) saved++;
            else if (it.state == WebState::Exists) had++;
            else if (it.state == WebState::Failed) failed++;
        }
        std::string summary = std::to_string(saved) + " saved, " + std::to_string(had) + " already yours, " +
                              std::to_string(failed) + " failed.";
        if (!error.empty()) summary += "  (" + error + ")";
        post(WM_APP_WEBFETCH_DONE, 0, new Posted{generation, {}, summary});
    });
    g.redraw = true;
}

// A parsed import, made into playlists: the rows the library already owns keep their files, the rest
// go through the save queue, and every list lands as its own tab in the file's own order. When
// everything is owned, no one is contacted at all.
void StartImport(std::vector<import::ImportList> lists, std::vector<std::string> albumLinks) {
    if (!f.open || f.running) return;
    import::OwnedIndex owned;
    CaptureOwned(owned);
    import::ImportPlan plan;
    import::SplitByLibrary(lists, owned, plan);
    plan.albumLinks = std::move(albumLinks);
    if (plan.lists.empty() && plan.albumLinks.empty()) {
        f.status = "Nothing in that file could be imported.";
        g.redraw = true;
        return;
    }
    if (plan.MissingCount() == 0 && plan.albumLinks.empty()) {
        // Every row is already a file the user owns: the playlists can simply be made.
        for (const import::ImportPlanList& list : plan.lists)
            MakeWebPlaylist(FromUtf8(list.title), import::AssembleFiles({}, 0, list.slots, owned), list.favorites);
        f.status = std::to_string(plan.OwnedCount()) + " tracks were already in your library; the playlists are made.";
        g.redraw = true;
        return;
    }
    if (!EnsureWebSave()) return;
    if (g.settings.folders.empty()) {
        f.status = "Add a library folder first (Library > Library folders...).";
        g.redraw = true;
        return;
    }
    WebFetchRun run;
    for (const import::ImportPlanList& list : plan.lists) run.importLists.push_back({list.title, list.missing});
    run.links = plan.albumLinks;
    run.libraryRoot = g.settings.folders.front();
    run.dataDir = g.dataDir;
    run.cancel = &g_cancel;
    f.plan = std::move(plan);
    f.status = "Importing " + std::to_string(f.plan.TrackCount()) + " tracks: " +
              std::to_string(f.plan.OwnedCount()) + " already in your library...";
    StartWorker(std::move(run));
}

void ImportText(const std::string& text, const std::wstring& fallbackTitle) {
    std::vector<import::ImportList> lists;
    std::vector<std::string> albumLinks;
    std::string error;
    if (!import::ParseImportText(text, fallbackTitle, lists, albumLinks, error)) {
        f.status = error;
        g.redraw = true;
        return;
    }
    StartImport(std::move(lists), std::move(albumLinks));
}

void StartRun() {
    if (!f.open || f.running) return;
    // Pasted export text, or the path of an export file, is an import; anything else is links.
    const std::string pasted = ToUtf8(f.paste.text);
    if (import::LooksLikeImport(pasted)) return ImportText(pasted, L"Imported playlist");
    if (import::IsImportFileName(f.paste.text) &&
        GetFileAttributesW(f.paste.text.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::string text;
        if (!ReadFileBytes(f.paste.text, text)) {
            f.status = "That file could not be read.";
            g.redraw = true;
            return;
        }
        return ImportText(text, StemOf(f.paste.text));
    }
    std::vector<std::string> links = LinksOf(f.paste.text);
    if (links.empty()) {
        f.status = "Paste a link first: a SoundCloud, YouTube or Spotify address.";
        g.redraw = true;
        return;
    }
    if (!EnsureWebSave()) return;
    if (g.settings.folders.empty()) {
        f.status = "Add a library folder first (Library > Library folders...).";
        g.redraw = true;
        return;
    }
    WebFetchRun run;
    run.links = links;
    run.libraryRoot = g.settings.folders.front();
    run.dataDir = g.dataDir;
    run.cancel = &g_cancel;
    f.plan = import::ImportPlan{};
    StartWorker(std::move(run));
}

// The Import file... button: one Spotify export or CSV, named by the file's own stem.
void ImportFromFile() {
    if (f.running) return;
    static const SDL_DialogFileFilter filter{"Spotify export or CSV playlist", "json;csv"};
    panel::ChooseFile(kFileImportWeb, filter);
}

struct WebGeometry {
    Rect field, save, list, status, stop, forget, import, close;
};

WebGeometry Geom() {
    WebGeometry G;
    const Rect out = panel::OutputRect();
    const float w = std::min(S(640), out.w - S(16)), h = std::min(S(480), out.h - S(16));
    const Rect p{std::round((out.w - w) / 2), std::round((out.h - h) / 2), w, h};
    const float pad = S(16), rh = panel::RowH(), top = p.y + S(48);
    G.field = {p.x + pad, top, p.w - 2 * pad - S(108), rh - S(4)};
    G.save = {p.R() - pad - S(100), top, S(100), rh - S(4)};
    const float footer = p.B() - pad - rh, statusY = footer - S(24);
    G.status = {p.x + pad, statusY, p.w - 2 * pad, S(20)};
    G.list = {p.x + pad, top + rh + S(8), p.w - 2 * pad,
              std::max(S(60), statusY - S(6) - (top + rh + S(8)))};
    G.stop = {p.x + pad, footer, S(100), rh};
    G.forget = {G.stop.R() + S(8), footer, S(150), rh};
    G.import = {G.forget.R() + S(8), footer, S(130), rh};
    G.close = {p.R() - pad - S(100), footer, S(100), rh};
    return G;
}

// The run's cancellable work and the status line, from the panel's buttons.
void StopSave() {
    if (!f.running) return;
    g_cancel = true;
    f.status = "Stopping after the track in flight...";
    g.redraw = true;
}

void Forget() {
    if (WebFetchForgetFailures(g.dataDir)) f.status = "Forgotten. The next save tries those tracks again.";
    g.redraw = true;
}

}  // namespace

void OpenWebFetch() {
    if (f.open) {  // already there: bring it forward, as the Windows window does
        ShowWebFetchPanel();
        return;
    }
    f.paste = panel::TextField{};
    f.status.clear();
    f.items.clear();
    f.lists.clear();
    f.plan = import::ImportPlan{};
    f.scroll = 0;
    f.running = false;
    f.open = true;
    ++f.generation;
    ShowWebFetchPanel();
    g.redraw = true;
}

// Release Radar's save: albums go straight into the queue as files, not playlist tabs, and the
// panel shows them being saved with everything else.
void WebFetchLists(std::vector<WebFetchTrackList> lists) {
    if (lists.empty()) return;
    OpenWebFetch();
    if (!f.open) return;
    if (f.running) {
        f.status = "A save is already running. Stop it or wait for it to finish.";
        g.redraw = true;
        return;
    }
    if (!EnsureWebSave()) return;
    if (g.settings.folders.empty()) {
        f.status = "Add a library folder first (Library > Library folders...).";
        g.redraw = true;
        return;
    }
    WebFetchRun run;
    for (WebFetchTrackList& list : lists) run.importLists.push_back(std::move(list));
    run.libraryRoot = g.settings.folders.front();
    run.dataDir = g.dataDir;
    run.cancel = &g_cancel;
    f.plan = import::ImportPlan{};
    f.status = "Saving " + run.importLists.front().title + (run.importLists.size() > 1 ? " and more..." : "...");
    StartWorker(std::move(run));
}

void OnWebFetchAdded(LPARAM lp) {
    std::unique_ptr<Posted> post(reinterpret_cast<Posted*>(lp));
    if (!post || !f.open || post->generation != f.generation) return;  // a closed panel drops it
    f.items.push_back(post->item);
    g.redraw = true;
}

void OnWebFetchUpdated(WPARAM index, LPARAM lp) {
    std::unique_ptr<Posted> post(reinterpret_cast<Posted*>(lp));
    if (!post || !f.open || post->generation != f.generation) return;
    if (index < f.items.size()) f.items[index] = post->item;
    g.redraw = true;
}

void OnWebFetchNotice(LPARAM lp) {
    std::unique_ptr<Posted> post(reinterpret_cast<Posted*>(lp));
    if (!post || !f.open || post->generation != f.generation) return;
    f.status = post->text;
    g.redraw = true;
}

void OnWebFetchLists(LPARAM lp) {
    std::unique_ptr<PostedLists> post(reinterpret_cast<PostedLists*>(lp));
    if (!post || !f.open || post->generation != f.generation) return;
    f.lists = std::move(post->lists);
    g.redraw = true;
}

void OnWebFetchDone(LPARAM lp) {
    std::unique_ptr<Posted> post(reinterpret_cast<Posted*>(lp));
    if (!post || !f.open || post->generation != f.generation) return;
    if (f.worker.joinable()) f.worker.join();
    f.running = false;
    g_cancel = false;
    FinishLists();
    f.status = post->text;
    if (!f.plan.lists.empty() && f.plan.OwnedCount() > 0)
        f.status += "  (" + std::to_string(f.plan.OwnedCount()) + " more were already in your library.)";
    f.plan = import::ImportPlan{};
    f.lists.clear();
    g.redraw = true;
}

void DrawWebFetchPanel(const Palette& p) {
    const WebGeometry G = Geom();
    DrawFrame(p, S(640), S(480), "Save music from the web");
    panel::DrawLineField(f.paste, G.field, true, true, "", p);
    DrawButton(G.save, "Save", p, true, !f.running);

    // The list: the track's name and where saving it stands, as the Windows list's columns.
    const Rect& list = G.list;
    Fill(list, p.surface2);
    Frame(list, p.border);
    const float row = panel::RowH();
    const float stateW = std::min(S(240), std::max(S(120), list.w * 0.4f));
    const float trackX = list.x + S(8), trackW = list.w - S(16) - stateW;
    const float stateX = trackX + trackW, hy = list.y + S(3);
    DrawText(Font::Small, "Track", trackX, hy, p.sub, trackW);
    DrawText(Font::Small, "Status", stateX, hy, p.sub, stateW);
    const float rowsTop = list.y + row, rowsH = list.B() - rowsTop - 1;
    f.scroll = std::clamp(f.scroll, 0.0f, std::max(0.0f, (float)f.items.size() * row - rowsH));
    SDL_Rect clip{(int)list.x + 1, (int)rowsTop, (int)list.w - 2, (int)rowsH};
    SDL_SetRenderClipRect(g.renderer, &clip);
    for (int i = 0; i < (int)f.items.size(); ++i) {
        const float ry = rowsTop + i * row - f.scroll;
        if (ry + row < rowsTop || ry > list.B()) continue;
        const WebFetchItem& it = f.items[(size_t)i];
        const float ty = std::round(ry + (row - LineHeight(Font::Normal)) / 2);
        DrawText(Font::Normal, TrackText(it), trackX, ty, p.text, trackW - S(8));
        DrawText(Font::Normal, StateText(it), stateX, ty, p.sub, stateW - S(4));
    }
    SDL_SetRenderClipRect(g.renderer, nullptr);

    DrawText(Font::Small, f.status, G.status.x, TextY(G.status), p.sub, G.status.w);
    DrawButton(G.stop, "Stop", p, false, f.running);
    DrawButton(G.forget, "Forgot what failed", p);
    DrawButton(G.import, "Import file...", p);
    DrawButton(G.close, "Close", p);
}

void WebFetchPanelMouseDown(float x, float y) {
    const WebGeometry G = Geom();
    if (G.save.In(x, y)) return StartRun();
    if (G.stop.In(x, y)) return StopSave();
    if (G.forget.In(x, y)) return Forget();
    if (G.import.In(x, y)) return ImportFromFile();
    if (G.close.In(x, y)) return ClosePanel();
    if (G.field.In(x, y)) {
        f.paste.caret = f.paste.text.size();
        f.paste.all = false;
        SDL_StartTextInput(g.window);
        g.redraw = true;
    }
}

void WebFetchPanelKey(const SDL_KeyboardEvent& e) {
    if (e.key == SDLK_ESCAPE) return ClosePanel();
    if (e.key == SDLK_RETURN || e.key == SDLK_KP_ENTER) return StartRun();
    if ((e.mod & SDL_KMOD_CTRL) && e.key == SDLK_V) {
        // Fields paste their own text on Linux: several links copied from a page arrive on separate
        // lines, which the box keeps as spaces so the paste still splits into links. A whole export
        // copied as text is an import instead, taken as it was copied.
        if (char* clip = SDL_GetClipboardText()) {
            std::string raw = clip;
            SDL_free(clip);
            if (import::LooksLikeImport(raw)) {
                ImportText(raw, L"Imported playlist");
                return;
            }
            std::wstring text = compat::Widen(raw.c_str());
            for (wchar_t& c : text)
                if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
            if (panel::LineFieldInsert(f.paste, panel::Printable(text))) g.redraw = true;
        }
        return;
    }
    const float page = std::max(panel::RowH(), Geom().list.h - panel::RowH() * 2);
    if (e.key == SDLK_DOWN) f.scroll += panel::RowH();
    else if (e.key == SDLK_UP) f.scroll -= panel::RowH();
    else if (e.key == SDLK_PAGEDOWN) f.scroll += page;
    else if (e.key == SDLK_PAGEUP) f.scroll -= page;
    else {
        if (panel::LineFieldKey(f.paste, e) == 1) g.redraw = true;
        return;
    }
    g.redraw = true;
}

void WebFetchPanelText(const char* text) {
    if (panel::LineFieldInsert(f.paste, panel::Printable(compat::Widen(text)))) g.redraw = true;
}

void WebFetchPanelWheel(float x, float y, float dy) {
    if (!Geom().list.In(x, y)) return;
    f.scroll -= dy * panel::RowH() * 3;
    g.redraw = true;
}

bool WebFetchPanelWantsText(float x, float y) { return Geom().field.In(x, y); }

// A file the panel's Import dialog chose, or one dropped while the panel is open.
void WebFetchPanelFile(const std::wstring& path) {
    std::string text;
    if (!ReadFileBytes(path, text)) {
        f.status = "That file could not be read.";
        g.redraw = true;
        return;
    }
    ImportText(text, StemOf(path));
}

// An export or CSV dropped while the panel is open is an import, not files to add; anything
// else is left for the window's own drop handling.
bool WebFetchPanelDrop(const std::wstring& path) {
    if (!f.open || f.running || !import::IsImportFileName(path)) return false;
    WebFetchPanelFile(path);
    return true;
}

void WebFetchPanelClose() {
    f.open = false;
    ++f.generation;  // a run still finishing finds no panel to show in
    StopRun();
    f.paste = panel::TextField{};
    f.status.clear();
    f.items.clear();
    f.lists.clear();
    f.plan = import::ImportPlan{};
    f.scroll = 0;
}

void StopWebFetch() {
    if (f.open) ClosePanel();  // WebFetchPanelClose joins the worker
    StopRun();
}

}  // namespace lx
