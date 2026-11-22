// Unified online song search pane on Linux (search_online_ui.h). Runs debounced track search on
// a cancellable worker, keeps an owned snapshot per library revision, resolves playback through
// RunWebFetch into preview storage (or straight into the library folder when auto-save is on),
// and renders with the active theme palette inside L.online.
#include "search_online_ui.h"

#include "discover_ui.h"
#include "panels.h"
#include "radio_ui.h"
#include "text.h"
#include "ui_internal.h"
#include "webfetch.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <memory>
#include <thread>

namespace lx {
namespace {

enum class ResolveAction { PlayNow, PlayNext, AddToQueue, SaveToLibrary };

struct OnlineRow {
    online::SearchTrack track;
    bool saved = false;
    TrackId savedId = kNone;
};

struct SearchPost {
    uint64_t generation = 0;
    online::SearchReply reply;
};

struct ResolvePost {
    uint64_t generation = 0;
    ResolveAction action = ResolveAction::PlayNow;
    bool wantSavePermanent = false;
    WebTrack track;
    std::wstring file, dir;
    bool ok = false;
    std::string error;
};

struct AlbumPost {
    uint64_t generation = 0;
    bool ok = false;
    WebFetchTrackList list;
    std::string error;
};

struct State {
    std::wstring query;
    std::vector<OnlineRow> rows;
    int selected = -1;
    int scroll = 0;
    bool focused = false;
    bool searching = false;
    bool resolving = false;
    std::string statusSource;
    std::string statusNote;

    online::Generation searchGen;
    std::shared_ptr<online::Cancel> searchCancel;
    std::atomic<bool> searchWorkerBusy{false};
    std::thread searchWorker;
    uint64_t debounceUntil = 0;

    online::Generation resolveGen;
    std::shared_ptr<std::atomic<bool>> resolveCancel;
    std::atomic<bool> resolveWorkerBusy{false};
    std::thread resolveWorker;
    bool hasPendingResolve = false;
    online::SearchTrack pendingTrack;
    ResolveAction pendingAction = ResolveAction::PlayNow;
    std::atomic<unsigned> previewSeq{0};

    online::Generation albumGen;
    std::shared_ptr<online::Cancel> albumCancel;
    std::atomic<bool> albumWorkerBusy{false};
    std::thread albumWorker;
    bool hasPendingAlbum = false;
    online::SearchTrack pendingAlbumTrack;

    import::OwnedIndex owned;
    uint64_t ownedRevision = UINT64_MAX;
    bool shuttingDown = false;
    online::SearchTrack menuTrack;
} s;

bool LocalSearchActive() {
    if (g.playlists.empty() || InternetRadio().active) return false;
    return online::SearchableQuery(s.query);
}

bool PromptMode() {
    return !online::Config().searchMusic && LocalSearchActive() && g.rows.empty();
}

float HeaderHeight() { return LineHeight(Font::Normal) + S(8); }
float RowHeight() { return std::max(S(ui::kRow), LineHeight(Font::Normal) + S(6)); }

void RemoveTree(const std::wstring& dir) {
    if (dir.empty()) return;
    std::error_code ignored;
    std::filesystem::remove_all(ToUtf8(dir), ignored);
}

bool EnsureOnlineSearchEnabled() {
    if (online::Config().searchMusic) return true;
    if (!Confirm(
            "Online search is off, so cordinal hasn't contacted anyone.\n\n"
            "When on, the words you type in search are sent to Deezer (and iTunes or YouTube if needed) "
            "to show songs not in your library.\n\nTurn online search on? It can be changed in Preferences > Online.",
            "Search online"))
        return false;
    online::Config().searchMusic = true;
    SaveSettings();
    OnlineSearchSettingChanged();
    return true;
}

void CancelSearchWorker() {
    s.debounceUntil = 0;
    s.searchGen.Next();
    if (s.searchCancel) s.searchCancel->Stop();
    s.searching = false;
}

void StartSearchWorker() {
    if (s.shuttingDown || !online::Config().searchMusic || !LocalSearchActive()) return;
    if (s.searchWorkerBusy.load()) {
        s.debounceUntil = SDL_GetTicks() + 60;
        return;
    }
    if (s.searchWorker.joinable()) s.searchWorker.join();
    const uint64_t gen = s.searchGen.Next();
    auto cancel = std::make_shared<online::Cancel>();
    s.searchCancel = cancel;
    s.searching = true;
    s.searchWorkerBusy = true;
    s.statusNote.clear();
    const std::string qUtf8 = online::NormalizeSearchQuery(s.query);
    import::OwnedIndex owned = OnlineSearchOwnedSnapshot();
    s.searchWorker = std::thread([gen, qUtf8, owned = std::move(owned), cancel] {
        auto* post = new SearchPost{gen, {}};
        post->reply = online::SearchTracks(qUtf8, owned, *cancel, 15);
        s.searchWorkerBusy = false;
        if (!PostMessageW(nullptr, WM_APP_ONLINE_SEARCH, 0, (LPARAM)post)) delete post;
    });
    g.redraw = true;
}

void ClampPaneScroll() {
    const float bodyH = std::max(0.0f, L.online.h - HeaderHeight());
    const int contentH = (int)s.rows.size() * (int)RowHeight();
    const int maxScroll = std::max(0, contentH - (int)bodyH);
    s.scroll = std::clamp(s.scroll, 0, maxScroll);
}

void EnsureRowVisible(int row) {
    if (row < 0 || row >= (int)s.rows.size()) return;
    const int rh = (int)RowHeight();
    const int bodyH = std::max(rh, (int)(L.online.h - HeaderHeight()));
    const int top = row * rh;
    const int bot = top + rh;
    if (top < s.scroll) s.scroll = top;
    else if (bot > s.scroll + bodyH) s.scroll = bot - bodyH;
    ClampPaneScroll();
}

void QueueOrPlayTrackId(TrackId id, ResolveAction action) {
    if (action == ResolveAction::AddToQueue) {
        QueueTracks({id}, false);
        return;
    }
    if (action == ResolveAction::PlayNext) {
        QueueTracks({id}, true);
        return;
    }
    if (RadioPlaying()) RadioCancelPlayback();
    const bool playing = g.playing != kNone || RadioPlaying();
    QueueTracks({id}, true);
    if (playing) Next();
}

void ApplyTrackMetadata(TrackId id, const WebTrack& wt, bool permanentlyInLibrary) {
    if (!g_lib.Contains(id)) return;
    Track& t = g_lib[id];
    if (!wt.title.empty()) t.title = g_lib.Intern(wt.title);
    if (!wt.artist.empty()) t.artist = g_lib.Intern(wt.artist);
    if (!wt.album.empty()) t.album = g_lib.Intern(wt.album);
    if (wt.trackNo > 0) t.trackNo = (uint16_t)std::min(wt.trackNo, 999u);
    if (wt.trackTotal > 0) t.trackTotal = (uint16_t)std::min(wt.trackTotal, 999u);
    if (wt.durationSec > 0) t.durationMs = wt.durationSec * 1000u;
    if (t.added == 0) t.added = UnixNow();
    if (permanentlyInLibrary) {
        t.flags |= Track::kInLibrary | Track::kLoaded;
        t.flags &= ~(Track::kInbox | Track::kMissing | Track::kFailed);
    } else {
        t.flags |= Track::kLoaded;
        t.flags &= ~(Track::kInLibrary | Track::kInbox | Track::kMissing | Track::kFailed);
    }
}

void MarkRowSaved(const WebTrack& wt, TrackId id) {
    for (OnlineRow& r : s.rows) {
        if (r.track.source == wt.site && r.track.id == wt.id) {
            r.saved = true;
            r.savedId = id;
        }
    }
}

void StartResolve(const online::SearchTrack& track, ResolveAction action) {
    if (s.shuttingDown) return;
    const bool wantSavePermanent =
        action == ResolveAction::SaveToLibrary || (online::Config().searchSave == 1 && !g.settings.folders.empty());
    if (action == ResolveAction::SaveToLibrary && g.settings.folders.empty()) {
        ShowMessage(L"Add a library folder first, so the song has somewhere to stay.");
        return;
    }

    const WebTrack wt = track.ToWebTrack();
    for (const OnlineRow& r : s.rows) {
        if (r.track.source == track.source && r.track.id == track.id && r.saved && r.savedId != kNone &&
            g_lib.Contains(r.savedId)) {
            if (action != ResolveAction::SaveToLibrary) QueueOrPlayTrackId(r.savedId, action);
            else ShowMessage(L"Already in your library: " + FromUtf8(track.title) + L".");
            return;
        }
    }
    TrackId previewId = kNone;
    if (DiscoverFindPreview(wt.site, wt.id, previewId) && g_lib.Contains(previewId)) {
        if (wantSavePermanent) {
            if (DiscoverKeepTrack(previewId)) {
                MarkRowSaved(wt, previewId);
                g.redraw = true;
            }
            if (action != ResolveAction::SaveToLibrary) QueueOrPlayTrackId(previewId, action);
        } else {
            QueueOrPlayTrackId(previewId, action);
        }
        return;
    }

    if (s.resolveWorkerBusy.load()) {
        s.pendingTrack = track;
        s.pendingAction = action;
        s.hasPendingResolve = true;
        s.resolveGen.Next();
        if (s.resolveCancel) *s.resolveCancel = true;
        ShowMessage(L"Finding " + FromUtf8(track.title) + L"...");
        return;
    }
    if (s.resolveWorker.joinable()) s.resolveWorker.join();

    const uint64_t gen = s.resolveGen.Next();
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    s.resolveCancel = cancel;
    s.resolving = true;
    s.resolveWorkerBusy = true;
    s.hasPendingResolve = false;

    const std::wstring previewRoot = g.dataDir + L"/preview";
    const std::wstring targetDir = wantSavePermanent
                                       ? g.settings.folders.front()
                                       : previewRoot + L"/search-" + std::to_wstring(++s.previewSeq);
    const std::wstring dataDir = wantSavePermanent ? g.dataDir : previewRoot;

    ShowMessage(action == ResolveAction::SaveToLibrary ? (L"Saving " + FromUtf8(track.title) + L"...")
                                                       : (L"Finding " + FromUtf8(track.title) + L"..."));

    s.resolveWorker = std::thread([gen, action, wantSavePermanent, wt, targetDir, dataDir, cancel] {
        auto* post = new ResolvePost{gen, action, wantSavePermanent, wt, {}, wantSavePermanent ? L"" : targetDir, false, {}};
        WebFetchRun run;
        run.libraryRoot = targetDir;
        run.dataDir = dataDir;
        run.cancel = cancel.get();
        WebFetchTrackList list;
        list.title = "Search";
        list.tracks.push_back(wt);
        list.playlist = false;
        run.importLists.push_back(std::move(list));
        std::vector<WebFetchItem> items;
        if (RunWebFetch(run, items, post->error) && !items.empty() &&
            (items.front().state == WebState::Done || items.front().state == WebState::Exists) &&
            !items.front().file.empty()) {
            post->ok = true;
            post->file = items.front().file;
        } else if (post->error.empty() && !items.empty() && !items.front().why.empty()) {
            post->error = items.front().why;
        }
        s.resolveWorkerBusy = false;
        if (!PostMessageW(nullptr, WM_APP_ONLINE_RESOLVE, 0, (LPARAM)post)) {
            if (!wantSavePermanent) RemoveTree(targetDir);
            delete post;
        }
    });
}

void StartAlbumSave(const online::SearchTrack& track) {
    if (s.shuttingDown || track.album.empty()) return;
    if (g.settings.folders.empty()) {
        ShowMessage(L"Add a library folder first, so the album has somewhere to stay.");
        return;
    }
    if (s.albumWorkerBusy.load()) {
        s.pendingAlbumTrack = track;
        s.hasPendingAlbum = true;
        s.albumGen.Next();
        if (s.albumCancel) s.albumCancel->Stop();
        ShowMessage(L"Looking up album " + FromUtf8(track.album) + L"...");
        return;
    }
    if (s.albumWorker.joinable()) s.albumWorker.join();

    const uint64_t gen = s.albumGen.Next();
    auto cancel = std::make_shared<online::Cancel>();
    s.albumCancel = cancel;
    s.albumWorkerBusy = true;
    s.hasPendingAlbum = false;
    ShowMessage(L"Looking up album " + FromUtf8(track.album) + L"...");

    s.albumWorker = std::thread([gen, track, cancel] {
        auto* post = new AlbumPost{gen, false, {}, {}};
        post->ok = online::FetchSearchAlbum(track, *cancel, post->list, post->error);
        s.albumWorkerBusy = false;
        if (!PostMessageW(nullptr, WM_APP_ONLINE_RESOLVE, 1, (LPARAM)post)) delete post;
    });
}

Rect PromptButtonRect(const Rect& r) {
    const float w = std::min(S(136), std::max(S(90), r.w - S(24)));
    return {r.R() - w - S(12), r.y + (r.h - S(26)) / 2, w, S(26)};
}

Rect SaveButtonRect(const Rect& rowRc) {
    const float w = S(58);
    return {rowRc.R() - w - S(10), rowRc.y + S(3), w, std::max(S(16), rowRc.h - S(6))};
}

}  // namespace

const import::OwnedIndex& OnlineSearchOwnedSnapshot() {
    const uint64_t rev = g_lib.Revision();
    if (s.ownedRevision != rev) {
        s.owned = import::OwnedIndex{};
        for (TrackId id = 0; id < (TrackId)g_lib.Count(); ++id) {
            const Track& t = g_lib[id];
            if (!t.InLibrary() && !(t.flags & Track::kInbox)) continue;
            if (t.flags & (Track::kMissing | Track::kExcluded)) continue;
            s.owned.Add(g_lib.View(t.artist ? t.artist : t.albumArtist), g_lib.View(t.title), g_lib.Path(id));
        }
        s.ownedRevision = rev;
    }
    return s.owned;
}

bool OnlineSearchVisible() {
    if (!LocalSearchActive()) return false;
    return online::Config().searchMusic || PromptMode();
}

float OnlineSearchWantedHeight(float availH) {
    if (!OnlineSearchVisible() || availH < S(48)) return 0;
    if (PromptMode()) return std::min(availH, S(42));
    const int wantRows = s.rows.empty() ? 2 : std::clamp((int)s.rows.size(), 2, 6);
    const float wanted = HeaderHeight() + wantRows * RowHeight() + S(4);
    const float cap = std::max(HeaderHeight() + RowHeight() * 2, availH * 0.45f);
    return std::clamp(wanted, HeaderHeight() + RowHeight() * 2, std::min(availH, cap));
}

void OnlineSearchQueryChanged(const std::wstring& query) {
    if (query == s.query) return;
    s.query = query;
    s.selected = -1;
    s.scroll = 0;
    s.focused = false;
    CancelSearchWorker();
    if (!online::Config().searchMusic || !LocalSearchActive()) {
        s.rows.clear();
        s.statusSource.clear();
        s.statusNote.clear();
        s.owned = import::OwnedIndex{};
        s.ownedRevision = UINT64_MAX;
        g.redraw = true;
        return;
    }
    s.debounceUntil = SDL_GetTicks() + 400;
    s.searching = true;
    s.statusSource.clear();
    s.statusNote.clear();
    g.redraw = true;
}

void OnlineSearchViewUpdated() {
    if (s.rows.empty()) return;
    const auto& owned = OnlineSearchOwnedSnapshot();
    std::vector<online::SearchTrack> raw;
    raw.reserve(s.rows.size());
    for (const OnlineRow& r : s.rows) raw.push_back(r.track);
    online::FilterUnownedTracks(raw, owned);
    if (raw.size() > 15) raw.resize(15);
    s.rows.clear();
    for (const online::SearchTrack& t : raw) s.rows.push_back({t, false, kNone});
    if (s.selected >= (int)s.rows.size()) s.selected = (int)s.rows.size() - 1;
    ClampPaneScroll();
}

void OnlineSearchSettingChanged() {
    if (!online::Config().searchMusic) {
        CancelSearchWorker();
        s.rows.clear();
        s.statusSource.clear();
        s.statusNote.clear();
        s.selected = -1;
        s.focused = false;
    } else if (LocalSearchActive()) {
        s.debounceUntil = SDL_GetTicks() + 50;
        s.searching = true;
    }
    g.redraw = true;
}

void OnlineSearchTrimMemory() {
    if (s.rows.empty()) {
        s.owned = import::OwnedIndex{};
        s.ownedRevision = UINT64_MAX;
    }
    online::ClearSearchCache();
}

bool OnlineSearchFocused() { return s.focused && OnlineSearchVisible() && !s.rows.empty(); }
void OnlineSearchBlur() {
    if (s.focused) {
        s.focused = false;
        g.redraw = true;
    }
}

bool OnlineSearchActivateDefault() {
    if (!OnlineSearchVisible() || s.rows.empty()) return false;
    if (s.selected < 0 || s.selected >= (int)s.rows.size()) s.selected = 0;
    s.focused = true;
    EnsureRowVisible(s.selected);
    StartResolve(s.rows[s.selected].track, ResolveAction::PlayNow);
    g.redraw = true;
    return true;
}

void OnlineSearchPlay(const online::SearchTrack& track, bool next, bool queueEnd) {
    StartResolve(track, queueEnd ? ResolveAction::AddToQueue : next ? ResolveAction::PlayNext : ResolveAction::PlayNow);
}

void OnlineSearchSave(const online::SearchTrack& track) { StartResolve(track, ResolveAction::SaveToLibrary); }

void OnlineSearchSaveAlbum(const online::SearchTrack& track) { StartAlbumSave(track); }

void OnlineSearchMenuAction(int arg) {
    switch (arg) {
        case 0: OnlineSearchPlay(s.menuTrack, false); break;
        case 1: OnlineSearchPlay(s.menuTrack, true); break;
        case 2: OnlineSearchPlay(s.menuTrack, false, true); break;
        case 3: OnlineSearchSave(s.menuTrack); break;
        case 4:
            if (!s.menuTrack.artist.empty()) OpenArtistInfoFor(s.menuTrack.artist);
            break;
        case 5: OnlineSearchSaveAlbum(s.menuTrack); break;
        default: break;
    }
}

void DrawOnlineSearch(const Rect& r, const Palette& p, float mx, float my) {
    if (r.w <= 0 || r.h <= 0) return;
    Fill(r, p.surface);
    Fill({r.x, r.y, r.w, S(1)}, p.border);

    if (PromptMode()) {
        DrawText(Font::Normal, "No songs matched in your library.", r.x + S(12), TextY(r), p.sub,
                 std::max(0.0f, r.w - S(160)));
        const Rect btn = PromptButtonRect(r);
        DrawButton(btn, "Search online...", p, true, true);
        if (btn.In(mx, my)) Frame(btn, p.text);
        return;
    }

    const float hdrH = HeaderHeight();
    const Rect hdr{r.x, r.y + S(1), r.w, hdrH - S(1)};
    Fill(hdr, p.surface2);
    Fill({r.x, r.y + hdrH - S(1), r.w, S(1)}, p.border);

    std::string rightStatus;
    if (s.searching) rightStatus = "Searching online...";
    else if (!s.statusNote.empty()) rightStatus = s.statusNote;
    else if (!s.statusSource.empty() && !s.rows.empty()) rightStatus = "via " + s.statusSource;

    float rightW = rightStatus.empty() ? 0 : std::min(r.w * 0.45f, TextWidth(Font::Normal, rightStatus) + S(16));
    DrawText(Font::Bold, "Online (not in your library)", r.x + S(12), TextY(hdr, Font::Bold), p.text,
             std::max(0.0f, r.w - rightW - S(24)));
    if (!rightStatus.empty()) {
        float rw = TextWidth(Font::Normal, rightStatus);
        DrawText(Font::Normal, rightStatus, std::max(r.x + S(12), r.R() - S(12) - rw), TextY(hdr), p.sub,
                 rightW - S(8));
    }

    const Rect body{r.x, r.y + hdrH, r.w, std::max(0.0f, r.h - hdrH)};
    if (s.rows.empty()) {
        std::string msg = s.searching ? "Looking up songs online..." : rightStatus;
        if (!msg.empty()) {
            float mw = TextWidth(Font::Normal, msg);
            DrawText(Font::Normal, msg, body.x + std::max(S(12), (body.w - mw) / 2), TextY(body), p.sub, body.w - S(24));
        }
        return;
    }

    SDL_Rect clipRect{(int)body.x, (int)body.y, (int)body.w, (int)body.h};
    SDL_SetRenderClipRect(g.renderer, &clipRect);

    const float rh = RowHeight();
    const int first = std::max(0, s.scroll / (int)rh);
    const int last = std::min<int>((int)s.rows.size(), (s.scroll + (int)body.h) / (int)rh + 2);
    for (int i = first; i < last; ++i) {
        const OnlineRow& row = s.rows[i];
        const Rect rr{body.x, body.y + i * rh - (float)s.scroll, body.w, rh};
        const bool sel = i == s.selected;
        const bool hot = !sel && rr.In(mx, my);
        if (sel) Fill(rr, p.select);
        else if (hot) Fill(rr, p.hover);
        else if (i % 2) Fill(rr, Mix(p.surface, p.bg, 45));

        const uint32_t fg = sel ? p.selectText : p.text;
        const uint32_t sub = sel ? p.selectText : p.sub;
        const Rect saveRc = SaveButtonRect(rr);
        const float durW = S(46);
        const float durX = saveRc.x - durW - S(8);
        const float avail = std::max(S(60), durX - rr.x - S(20));
        const float titleW = avail * 0.42f;
        const float artistW = avail * 0.30f;
        const float albumW = std::max(S(24), avail - titleW - artistW - S(16));

        DrawText(Font::Normal, row.track.title, rr.x + S(12), TextY(rr), fg, titleW - S(6));
        DrawText(Font::Normal, row.track.artist, rr.x + S(12) + titleW + S(4), TextY(rr), sub, artistW - S(6));
        DrawText(Font::Normal, row.track.album, rr.x + S(12) + titleW + artistW + S(8), TextY(rr), sub, albumW - S(6));
        if (row.track.durationSec > 0) {
            std::string dur = Utf8(FormatDuration((double)row.track.durationSec));
            float dw = TextWidth(Font::Normal, dur);
            DrawText(Font::Normal, dur, std::max(durX, durX + durW - dw), TextY(rr), sub, durW);
        }

        if (row.saved) {
            DrawText(Font::Normal, "Saved", saveRc.x + S(8), TextY(saveRc), sub, saveRc.w - S(12));
        } else {
            const bool saveHot = saveRc.In(mx, my);
            Fill(saveRc, saveHot ? p.hover : p.surface2);
            Frame(saveRc, p.border);
            float sw = TextWidth(Font::Normal, "Save");
            DrawText(Font::Normal, "Save", saveRc.x + std::max(S(4), (saveRc.w - sw) / 2), TextY(saveRc), p.text,
                     saveRc.w - S(6));
        }
    }
    SDL_SetRenderClipRect(g.renderer, nullptr);
}

bool OnlineSearchMouseDown(float x, float y, int button, int clicks) {
    if (!OnlineSearchVisible() || !L.online.In(x, y)) {
        if (s.focused) {
            s.focused = false;
            g.redraw = true;
        }
        return false;
    }
    if (PromptMode()) {
        if (button == SDL_BUTTON_LEFT && PromptButtonRect(L.online).In(x, y)) EnsureOnlineSearchEnabled();
        return true;
    }
    const float hdrH = HeaderHeight();
    if (y < L.online.y + hdrH) return true;
    const int idx = (int)((y - (L.online.y + hdrH) + (float)s.scroll) / RowHeight());
    if (idx < 0 || idx >= (int)s.rows.size()) return true;

    s.selected = idx;
    s.focused = true;
    g.redraw = true;

    const Rect rr{L.online.x, L.online.y + hdrH + idx * RowHeight() - (float)s.scroll, L.online.w, RowHeight()};
    if (button == SDL_BUTTON_LEFT) {
        if (SaveButtonRect(rr).In(x, y)) {
            if (!s.rows[idx].saved) StartResolve(s.rows[idx].track, ResolveAction::SaveToLibrary);
        } else if (clicks >= 2) {
            StartResolve(s.rows[idx].track, ResolveAction::PlayNow);
        }
        return true;
    }
    if (button == SDL_BUTTON_RIGHT) {
        s.menuTrack = s.rows[idx].track;
        auto item = [](const char* label, int arg, bool enabled = true) {
            MenuItem m;
            m.label = label;
            m.action = kOnlineAction;
            m.arg = arg;
            m.enabled = enabled;
            return m;
        };
        std::vector<MenuItem> items{
            item("Play now", 0),
            item("Play next", 1),
            item("Add to queue", 2),
            item("Save to library", 3, !s.rows[idx].saved),
            item("Save full album to library...", 5, !s.menuTrack.album.empty()),
        };
        if (!s.menuTrack.artist.empty()) {
            MenuItem sep;
            sep.separator = true;
            items.push_back(sep);
            items.push_back(item("Go to artist", 4));
        }
        g_barOpen = -1;
        OpenPopup(x, y, std::move(items));
        return true;
    }
    return true;
}

bool OnlineSearchWheel(float x, float y, float dy) {
    if (!OnlineSearchVisible() || PromptMode() || !L.online.In(x, y)) return false;
    s.scroll -= (int)std::lround(dy * RowHeight() * 2);
    ClampPaneScroll();
    g.redraw = true;
    return true;
}

bool OnlineSearchKey(const SDL_KeyboardEvent& e) {
    if (!OnlineSearchFocused() || s.rows.empty()) return false;
    if (e.key == SDLK_UP) {
        if (s.selected <= 0) {
            s.focused = false;
            g.redraw = true;
            return true;
        }
        --s.selected;
        EnsureRowVisible(s.selected);
        g.redraw = true;
        return true;
    }
    if (e.key == SDLK_DOWN) {
        s.selected = std::min((int)s.rows.size() - 1, s.selected + 1);
        EnsureRowVisible(s.selected);
        g.redraw = true;
        return true;
    }
    if (e.key == SDLK_RETURN || e.key == SDLK_KP_ENTER) {
        if (s.selected >= 0 && s.selected < (int)s.rows.size()) {
            const bool shift = (e.mod & SDL_KMOD_SHIFT) != 0;
            StartResolve(s.rows[s.selected].track, shift ? ResolveAction::PlayNext : ResolveAction::PlayNow);
        }
        return true;
    }
    if (e.key == SDLK_ESCAPE) {
        s.focused = false;
        g.redraw = true;
        return true;
    }
    return false;
}

void OnlineSearchTick() {
    if (s.debounceUntil > 0 && SDL_GetTicks() >= s.debounceUntil) {
        s.debounceUntil = 0;
        StartSearchWorker();
    }
}

void OnOnlineSearchMessage(WPARAM, LPARAM lp) {
    std::unique_ptr<SearchPost> post(reinterpret_cast<SearchPost*>(lp));
    if (!post || !s.searchGen.Accepts(post->generation)) return;
    s.searching = false;
    s.rows.clear();
    s.statusSource.clear();
    if (!post->reply.ok) {
        s.statusNote = post->reply.error.empty() ? "Online search couldn't connect." : post->reply.error;
        s.selected = -1;
        g.redraw = true;
        return;
    }
    for (const online::SearchTrack& t : post->reply.tracks) s.rows.push_back({t, false, kNone});
    if (!s.rows.empty()) {
        const std::string& src = s.rows.front().track.source;
        s.statusSource = src == "deezer" ? "Deezer" : src == "itunes" ? "iTunes" : src == "youtube" ? "YouTube" : src;
    }
    s.statusNote = s.rows.empty() ? "No online matches." : "";
    s.selected = s.rows.empty() ? -1 : 0;
    s.scroll = 0;
    ClampPaneScroll();
    g.redraw = true;
}

void OnOnlineResolveMessage(WPARAM wp, LPARAM lp) {
    if (wp == 1) {
        std::unique_ptr<AlbumPost> post(reinterpret_cast<AlbumPost*>(lp));
        if (!post) return;
        const bool accepted = s.albumGen.Accepts(post->generation);
        if (accepted && !s.shuttingDown) {
            if (!post->ok || post->list.tracks.empty()) {
                ShowMessage(post->error.empty() ? L"Couldn't find that album's tracklist." : FromUtf8(post->error));
            } else {
                WebFetchLists({std::move(post->list)});
            }
        }
        g.redraw = true;
        if (s.hasPendingAlbum && !s.shuttingDown) {
            online::SearchTrack nextTrack = std::move(s.pendingAlbumTrack);
            s.hasPendingAlbum = false;
            StartAlbumSave(nextTrack);
        }
        return;
    }

    std::unique_ptr<ResolvePost> post(reinterpret_cast<ResolvePost*>(lp));
    if (!post) return;
    const bool accepted = s.resolveGen.Accepts(post->generation);
    if (!accepted || s.shuttingDown) {
        if (!post->wantSavePermanent) RemoveTree(post->dir);
    } else {
        s.resolving = false;
        if (!post->ok || post->file.empty()) {
            ShowMessage(post->error.empty() ? L"Couldn't find audio for that song." : FromUtf8(post->error));
        } else {
            const TrackId id = g_lib.Add(post->file);
            if (id == kInvalidTrack) {
                if (!post->wantSavePermanent) RemoveTree(post->dir);
                ShowMessage(L"Couldn't open the downloaded song.");
            } else {
                ApplyTrackMetadata(id, post->track, post->wantSavePermanent);
                if (post->wantSavePermanent) {
                    MarkRowSaved(post->track, id);
                    g_lib.Touch();
                    g.libraryDirty = true;
                    RebuildRows();
                } else {
                    DiscoverRegisterPreview(id, post->track, post->file, post->dir);
                }
                if (post->action == ResolveAction::SaveToLibrary) {
                    ShowMessage(L"Saved to your library: " + FromUtf8(post->track.title) + L".");
                } else {
                    QueueOrPlayTrackId(id, post->action);
                }
            }
        }
    }
    g.redraw = true;

    if (s.hasPendingResolve && !s.shuttingDown) {
        online::SearchTrack nextTrack = std::move(s.pendingTrack);
        ResolveAction nextAction = s.pendingAction;
        s.hasPendingResolve = false;
        StartResolve(nextTrack, nextAction);
    }
}

void StopOnlineSearch() {
    s.shuttingDown = true;
    s.debounceUntil = 0;
    s.hasPendingResolve = false;
    s.hasPendingAlbum = false;
    s.searchGen.Next();
    s.resolveGen.Next();
    s.albumGen.Next();
    if (s.searchCancel) s.searchCancel->Stop();
    if (s.resolveCancel) *s.resolveCancel = true;
    if (s.albumCancel) s.albumCancel->Stop();
    if (s.searchWorker.joinable()) s.searchWorker.join();
    if (s.resolveWorker.joinable()) s.resolveWorker.join();
    if (s.albumWorker.joinable()) s.albumWorker.join();
    RemoveTree(g.dataDir + L"/preview");
    s.rows.clear();
    s.owned = import::OwnedIndex{};
    s.ownedRevision = UINT64_MAX;
}

size_t OnlineSearchRowCount() { return s.rows.size(); }

void SetOnlineSearchTestRows(const std::vector<online::SearchTrack>& rows) {
    CancelSearchWorker();
    s.rows.clear();
    for (const online::SearchTrack& t : rows) s.rows.push_back({t, false, kNone});
    s.statusSource = rows.empty() ? "" : "Test";
    s.statusNote.clear();
    s.selected = s.rows.empty() ? -1 : 0;
    s.scroll = 0;
    g.redraw = true;
}

}  // namespace lx
