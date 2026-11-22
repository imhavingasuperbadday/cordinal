// Artist info on Linux (View > Artist info..., or a track's right-click menu), as app_artistinfo.cpp
// does on Windows with the same wording: the selected or playing track's artist as MusicBrainz knows
// them, the opening of their English Wikipedia article, their albums and EPs with the ones the library
// has marked, and a filter for the ones it lacks. Selecting an album shows its tracklist (from the
// library when owned, or MusicBrainz when missing) with Play album, Save album to library, and
// Show in library actions.
#include "panels_internal.h"

#include "discover_ui.h"
#include "draw.h"
#include "libview.h"
#include "metadata_fetch.h"
#include "release_radar.h"
#include "webfetch.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>

namespace lx {

namespace {

struct Result {
    uint64_t generation;
    bool found;
    online::ArtistInfo info;
    std::vector<online::ReleaseGroup> albums;
    bool albumsKnown;  // MusicBrainz answered for the discography (an empty one included)
};

struct AlbumResult {
    uint64_t generation;
    std::string groupId;
    bool ok = false;
    radar::Album album;
};

struct PlayResult {
    uint64_t generation = 0;
    bool ok = false, first = false, permanent = false, done = false;
    WebTrack track;
    std::string albumArtist, date;
    std::wstring file, dir;
    std::string error;
};

struct PlayState {
    online::Generation gen;
    std::shared_ptr<std::atomic<bool>> cancel;
    std::thread worker;
    unsigned seq = 0;
    bool shuttingDown = false;
} p;

struct ArtistState {
    bool open = false;
    std::string artist, status;
    online::ArtistInfo info;
    bool found = false;
    float scroll = 0, listScroll = 0, trackScroll = 0;
    std::vector<online::ReleaseGroup> discography;
    bool discographyKnown = false;
    bool missingOnly = false;
    std::vector<int> shown;  // the list's rows, as indexes into discography
    int selectedGroup = -1;  // index into discography
    int selectedTrack = -1;
    std::map<std::string, radar::Album> albumCache;
    radar::Album currentAlbum;
    bool currentAlbumValid = false;
    bool albumLoading = false;
    bool albumFailed = false;
    bool albumWaitConfirm = false;  // arrowed to a missing uncached row: press Enter to fetch
    std::vector<TrackId> ownedTracks;
    online::Generation generation, albumGeneration;
    std::shared_ptr<online::Cancel> cancel, albumCancel;
    std::thread worker, albumWorker;
    Rect text, missing, list, trackList, albumPrimary, albumSecondary, albumMb,
         musicbrainz, wikipedia, official, close;
} a;

void RemoveTree(const std::wstring& dir) {
    if (dir.empty()) return;
    std::error_code ignored;
    std::filesystem::remove_all(ToUtf8(dir), ignored);
}

void StopPlayWorker() {
    if (p.cancel) *p.cancel = true;
    if (p.worker.joinable()) p.worker.join();
    p.cancel.reset();
    p.gen.Next();
}

void StopAlbumLookup() {
    if (a.albumCancel) a.albumCancel->Stop();
    if (a.albumWorker.joinable()) a.albumWorker.join();
    a.albumCancel.reset();
    a.albumGeneration.Next();
    a.albumLoading = false;
}

void StopLookup() {
    StopAlbumLookup();
    if (a.cancel) a.cancel->Stop();
    if (a.worker.joinable()) a.worker.join();
    a.cancel.reset();
    a.generation.Next();
}

void OpenUrl(const std::string& url) {
    // The addresses came from the network: only web pages are handed to the browser.
    if (url.starts_with("https://") || url.starts_with("http://")) SDL_OpenURL(url.c_str());
}

const online::ReleaseGroup* SelectedGroup() {
    if (a.selectedGroup < 0 || a.selectedGroup >= (int)a.discography.size()) return nullptr;
    return &a.discography[a.selectedGroup];
}

void StartAlbumLookup(const online::ReleaseGroup& rg) {
    StopAlbumLookup();
    a.currentAlbum = {};
    a.currentAlbumValid = false;
    a.albumFailed = false;
    a.albumWaitConfirm = false;
    a.albumLoading = true;
    a.trackScroll = 0;
    a.selectedTrack = -1;

    radar::Item item;
    item.artist = !a.info.name.empty() ? a.info.name : a.artist;
    item.groupId = rg.id;
    item.title = rg.title;
    item.type = rg.type;
    item.firstDate = rg.firstDate;
    const uint64_t gen = a.albumGeneration.Next();
    a.albumCancel = std::make_shared<online::Cancel>();
    a.albumWorker = std::thread([item, gen, cancel = a.albumCancel] {
        auto* res = new AlbumResult{gen, item.groupId, false, {}};
        res->ok = radar::FetchAlbum(item, res->album, *cancel);
        if (!PostMessageW(nullptr, WM_APP_ARTISTINFO, 1, (LPARAM)res)) delete res;
    });
    g.redraw = true;
}

void SelectAlbumGroup(int groupIdx, bool fetchMissing) {
    a.selectedGroup = groupIdx;
    a.selectedTrack = -1;
    a.trackScroll = 0;
    a.ownedTracks.clear();
    a.currentAlbum = {};
    a.currentAlbumValid = false;
    a.albumFailed = false;
    a.albumWaitConfirm = false;
    StopAlbumLookup();
    const online::ReleaseGroup* rg = SelectedGroup();
    if (!rg) return;
    if (rg->owned) {
        a.ownedTracks = online::LibraryAlbumTracks(g_lib, a.artist, rg->title);
        return;
    }
    auto cached = a.albumCache.find(rg->id);
    if (cached != a.albumCache.end()) {
        a.currentAlbum = cached->second;
        a.currentAlbumValid = !a.currentAlbum.tracks.empty();
        a.albumFailed = !a.currentAlbumValid;
        return;
    }
    if (fetchMissing) StartAlbumLookup(*rg);
    else a.albumWaitConfirm = true;
}

void FillAlbums() {
    a.shown.clear();
    bool keep = false;
    for (int i = 0; i < (int)a.discography.size(); ++i) {
        if (!a.missingOnly || !a.discography[i].owned) {
            a.shown.push_back(i);
            if (i == a.selectedGroup) keep = true;
        }
    }
    a.listScroll = 0;
    if (!keep) SelectAlbumGroup(-1, false);
}

std::string AlbumCounts() {
    if (!a.discographyKnown) return "Their albums couldn't be fetched from MusicBrainz.";
    if (a.discography.empty()) return "MusicBrainz lists no albums or EPs for this artist.";
    size_t missing = std::count_if(a.discography.begin(), a.discography.end(), [](const auto& r) { return !r.owned; });
    return std::to_string(a.discography.size()) + (a.discography.size() == 1 ? " album or EP, " : " albums and EPs, ") +
           std::to_string(missing) + " not in your library.";
}

float RowHeight() { return LineHeight(Font::Normal) + S(6); }

std::string AlbumHeading() {
    const online::ReleaseGroup* rg = SelectedGroup();
    if (!rg) return "Select an album above to see its tracklist.";
    std::string heading = rg->title;
    if (rg->year) heading += " (" + std::to_string(rg->year) + ")";
    if (rg->owned) {
        if (a.ownedTracks.empty()) return heading + " \xE2\x80\x94 no playable tracks found in library";
        double totalSec = 0;
        for (TrackId id : a.ownedTracks) totalSec += g_lib[id].Duration();
        heading += " \xE2\x80\x94 " + std::to_string(a.ownedTracks.size()) +
                   (a.ownedTracks.size() == 1 ? " track" : " tracks");
        if (totalSec > 0) heading += ", " + Utf8(FormatDuration(totalSec));
        return heading;
    }
    if (a.albumWaitConfirm) return heading + " \xE2\x80\x94 Press Enter or click to read tracklist.";
    if (a.albumLoading) return heading + " \xE2\x80\x94 Reading tracklist from MusicBrainz...";
    if (a.albumFailed || !a.currentAlbumValid || a.currentAlbum.tracks.empty())
        return heading + " \xE2\x80\x94 MusicBrainz has no tracklist for this release.";
    unsigned totalSec = 0;
    for (const radar::AlbumTrack& t : a.currentAlbum.tracks) totalSec += t.durationSec;
    heading += " \xE2\x80\x94 " + std::to_string(a.currentAlbum.tracks.size()) +
               (a.currentAlbum.tracks.size() == 1 ? " track" : " tracks");
    if (totalSec > 0) heading += ", " + Utf8(FormatDuration((double)totalSec));
    return heading;
}

bool EnsurePlayAudio() {
    if (online::Config().saveMusic || online::Config().searchMusic) return true;
    if (!Confirm("Save music from the web is off, so cordinal hasn't contacted YouTube.\n\n"
                 "Playing an album from MusicBrainz matches each track on YouTube and streams or saves it. "
                 "Turn Save music from the web on? It can be changed in Preferences > Online.",
                 "Play album"))
        return false;
    online::Config().saveMusic = true;
    SaveSettings();
    return true;
}

void SaveCurrentAlbum() {
    if (!a.currentAlbumValid || a.currentAlbum.tracks.empty()) return;
    WebFetchLists({radar::AlbumToWebFetchList(a.currentAlbum)});
}

void ShowCurrentInLibrary() {
    if (a.ownedTracks.empty()) return;
    int lib = -1;
    for (int i = 0; i < (int)g.playlists.size() && lib < 0; ++i)
        if (g.playlists[i].kind == PlaylistKind::Library) lib = i;
    if (lib < 0) return;
    const TrackId first = a.ownedTracks.front();
    ClosePanel();
    SetActiveTab(lib);
    if (CanvasActive()) {
        CanvasShowTrack(first);
    } else {
        for (size_t i = 0; i < g.rowItems.size(); ++i)
            if (g.rowItems[i] < g.playlists[lib].items.size() && g.playlists[lib].items[g.rowItems[i]] == first) {
                MoveFocus((int)i, false);
                break;
            }
    }
    g.redraw = true;
}

void PlayMissingAlbum(int startRow) {
    if (!a.currentAlbumValid || a.currentAlbum.tracks.empty()) return;
    if (!EnsurePlayAudio()) return;
    const size_t start = (size_t)std::clamp(startRow, 0, (int)a.currentAlbum.tracks.size() - 1);
    WebFetchTrackList list = radar::AlbumToWebFetchList(a.currentAlbum);
    if (start > 0) list.tracks.erase(list.tracks.begin(), list.tracks.begin() + (ptrdiff_t)start);
    if (list.tracks.empty()) return;

    StopPlayWorker();
    const bool permanent = online::Config().searchSave == 1 && !g.settings.folders.empty();
    const std::wstring libRoot = permanent ? g.settings.folders.front() : std::wstring();
    const std::wstring dataDir = g.dataDir;
    const uint64_t gen = p.gen.Next();
    const unsigned seq = ++p.seq;
    p.cancel = std::make_shared<std::atomic<bool>>(false);
    ShowMessage(L"Finding " + FromUtf8(list.tracks.front().title) + L"...");

    p.worker = std::thread([list = std::move(list), permanent, libRoot, dataDir, gen, seq,
                            cancel = p.cancel]() mutable {
        auto post = [](PlayResult* res) {
            if (!PostMessageW(nullptr, WM_APP_ARTISTINFO, 2, (LPARAM)res)) delete res;
        };
        const std::wstring previewDir = dataDir + L"/preview/artist-" + std::to_wstring(seq);
        WebFetchRun run;
        run.libraryRoot = permanent ? libRoot : previewDir;
        run.dataDir = permanent ? dataDir : dataDir + L"/preview";
        run.cancel = cancel.get();
        const std::string albumArtist = list.albumArtist, date = list.date;
        run.importLists.push_back(std::move(list));
        bool postedFirst = false;
        std::string firstError;
        run.updated = [&](size_t, const WebFetchItem& it) {
            if (it.state == WebState::Failed && firstError.empty()) firstError = it.why;
            if ((it.state != WebState::Done && it.state != WebState::Exists) || it.file.empty()) return;
            auto* res = new PlayResult();
            res->generation = gen;
            res->ok = true;
            res->first = !postedFirst;
            postedFirst = true;
            res->permanent = permanent;
            res->track = it.track;
            res->albumArtist = albumArtist;
            res->date = date;
            res->file = it.file;
            post(res);
        };
        std::vector<WebFetchItem> items;
        std::string error;
        RunWebFetch(run, items, error);
        if (!postedFirst && !cancel->load()) {
            auto* fail = new PlayResult();
            fail->generation = gen;
            fail->first = true;
            fail->error = !firstError.empty() ? firstError : (!error.empty() ? error : "could not play that album");
            post(fail);
        }
        auto* done = new PlayResult();
        done->generation = gen;
        done->done = true;
        post(done);
    });
}

void PlaySelectedAlbum(int startRow = 0) {
    const online::ReleaseGroup* rg = SelectedGroup();
    if (!rg) return;
    if (rg->owned) {
        if (!a.ownedTracks.empty())
            PlayTracks(a.ownedTracks, std::clamp(startRow, 0, (int)a.ownedTracks.size() - 1));
        return;
    }
    PlayMissingAlbum(startRow);
}

void HandlePlayResult(PlayResult* raw) {
    std::unique_ptr<PlayResult> res(raw);
    if (!res) return;
    if (p.shuttingDown || !p.gen.Accepts(res->generation)) {
        if (!res->dir.empty()) RemoveTree(res->dir);
        return;
    }
    if (res->done) return;
    if (!res->ok) {
        if (!res->dir.empty()) RemoveTree(res->dir);
        if (res->first) ShowMessage(L"Couldn't play song: " + FromUtf8(res->error) + L".");
        return;
    }
    const TrackId id = g_lib.Add(res->file);
    if (id == kInvalidTrack) {
        if (!res->dir.empty()) RemoveTree(res->dir);
        return;
    }
    Track& t = g_lib[id];
    t.title = g_lib.Intern(res->track.title);
    t.artist = g_lib.Intern(res->track.artist);
    t.album = g_lib.Intern(res->track.album);
    if (!res->albumArtist.empty()) t.albumArtist = g_lib.Intern(res->albumArtist);
    if (!res->date.empty()) t.date = g_lib.Intern(res->date);
    t.trackNo = (uint16_t)std::min(res->track.trackNo, 999u);
    t.trackTotal = (uint16_t)std::min(res->track.trackTotal, 999u);
    t.disc = (uint8_t)std::min(res->track.discNo, 99u);
    t.discTotal = (uint8_t)std::min(res->track.discTotal, 99u);
    t.durationMs = res->track.durationSec * 1000u;
    t.added = UnixNow();
    if (res->permanent) {
        t.flags |= Track::kInLibrary | Track::kLoaded;
        t.flags &= ~(Track::kInbox | Track::kMissing | Track::kFailed);
        QueueTags({id});
        g_lib.Touch();
        g.libraryDirty = true;
        RebuildRows();
    } else {
        t.flags |= Track::kLoaded;
        t.flags &= ~(Track::kInLibrary | Track::kInbox | Track::kMissing | Track::kFailed);
        DiscoverRegisterPreview(id, res->track, res->file, res->dir);
    }
    if (res->first) {
        ClearQueue();
        PlayTracks({id}, 0);
        ShowMessage(L"Playing " + FromUtf8(res->track.album.empty() ? res->track.title : res->track.album) + L"...");
    } else {
        g.queue.insert(g.queue.begin() + (ptrdiff_t)QueuedByYou(), id);
        RefreshNext();
    }
    g.redraw = true;
}

}  // namespace

void OpenArtistInfoFor(const std::string& artist) {
    if (artist.empty()) return;
    if (!online::Config().artistInfo) {
        if (!Confirm("Artist info is off, so cordinal hasn't contacted anyone.\n\n"
                     "It sends the artist's name to musicbrainz.org, which also lists their albums, then asks "
                     "wikidata.org and en.wikipedia.org for the article MusicBrainz names.\n\nTurn Artist info on? "
                     "It can be changed in Preferences > Online.",
                     "Artist info"))
            return;
        online::Config().artistInfo = true;
        SaveSettings();
    }
    StopLookup();
    a.open = true;
    a.artist = artist;
    a.info = {};
    a.found = false;
    a.scroll = 0;
    a.listScroll = 0;
    a.trackScroll = 0;
    a.selectedGroup = -1;
    a.selectedTrack = -1;
    a.albumCache.clear();
    a.currentAlbum = {};
    a.currentAlbumValid = false;
    a.albumLoading = false;
    a.albumFailed = false;
    a.albumWaitConfirm = false;
    a.ownedTracks.clear();
    a.discography.clear();
    FillAlbums();
    a.status = "Asking MusicBrainz...";
    const uint64_t generation = a.generation.Next();
    a.cancel = std::make_shared<online::Cancel>();
    a.worker = std::thread([artist, generation, cancel = a.cancel] {
        auto* result = new Result{generation, false, {}, {}, false};
        result->found = online::FetchArtistInfo(artist, {}, *cancel, result->info);
        if (result->found) result->albumsKnown = online::FetchDiscography(result->info.mbid, *cancel, result->albums);
        if (!PostMessageW(nullptr, WM_APP_ARTISTINFO, 0, (LPARAM)result)) delete result;
    });
    ShowArtistPanel();
}

void OpenArtistInfo() {
    auto ids = SelectedTracks();
    TrackId id = !ids.empty() ? ids.front() : g.playing;
    if (id == kNone || !g_lib.Contains(id)) {
        ShowMessage(L"Select a track, or play one, to see its artist.");
        return;
    }
    const Track& t = g_lib[id];
    std::string artist = ToUtf8(g_lib.Str(t.artist ? t.artist : t.albumArtist));
    if (artist.empty()) {
        ShowMessage(L"This track has no artist in its tags.");
        return;
    }
    OpenArtistInfoFor(artist);
}

void OnArtistInfoMessage(WPARAM wp, LPARAM lp) {
    if (wp == 2) {
        HandlePlayResult(reinterpret_cast<PlayResult*>(lp));
        return;
    }
    if (wp == 1) {
        std::unique_ptr<AlbumResult> res(reinterpret_cast<AlbumResult*>(lp));
        if (!a.open || !a.albumGeneration.Accepts(res->generation)) return;
        if (a.albumWorker.joinable()) a.albumWorker.join();
        a.albumCancel.reset();
        a.albumLoading = false;
        if (res->ok && !res->album.tracks.empty()) {
            a.albumCache[res->groupId] = res->album;
            a.currentAlbum = std::move(res->album);
            a.currentAlbumValid = true;
            a.albumFailed = false;
        } else {
            a.albumCache[res->groupId] = radar::Album{};
            a.currentAlbum = {};
            a.currentAlbumValid = false;
            a.albumFailed = true;
        }
        g.redraw = true;
        return;
    }
    std::unique_ptr<Result> result(reinterpret_cast<Result*>(lp));
    if (!a.open || !a.generation.Accepts(result->generation)) return;  // closed, or another artist since
    if (a.worker.joinable()) a.worker.join();
    a.cancel.reset();
    a.found = result->found;
    a.info = std::move(result->info);
    a.discography = std::move(result->albums);
    a.discographyKnown = result->albumsKnown;
    online::MarkOwned(a.discography, online::LibraryAlbumsBy(g_lib, a.artist));
    FillAlbums();
    if (!a.found)
        a.status = "MusicBrainz has no single artist named \xE2\x80\x9C" + a.artist +
                   "\xE2\x80\x9D, so nothing is shown rather than a namesake.";
    else
        a.status = (a.info.summary.empty() ? "From MusicBrainz. " : "From MusicBrainz and Wikipedia (text under CC BY-SA 4.0). ") +
                   AlbumCounts();
    g.redraw = true;
}

void DrawArtistPanel(const Palette& p) {
    Rect panel = DrawFrame(p, S(680), S(660), "Artist info");
    const float pad = S(16);
    DrawText(Font::Bold, a.found ? a.info.name : a.artist, panel.x + pad, panel.y + S(46), p.text, panel.w - pad * 2);
    float bottom = panel.B() - pad - S(30);
    const float statusY = bottom - S(24), albumBtnY = statusY - S(34);
    const float top = panel.y + S(72), avail = std::max(S(240), albumBtnY - S(8) - top);
    const float summary = std::floor(avail * 0.24f), listH = std::floor(avail * 0.34f);

    a.text = {panel.x + pad, top, panel.w - pad * 2, summary};
    Fill(a.text, p.surface2);
    Frame(a.text, p.border);
    std::string body = !a.found ? std::string()
                       : a.info.summary.empty() ? std::string("Wikipedia has no English article for this artist.")
                                                : a.info.summary;
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= body.size() && !body.empty()) {
        size_t end = body.find('\n', start);
        std::string paragraph = body.substr(start, end == std::string::npos ? std::string::npos : end - start);
        for (const std::string& line : Wrap(Font::Normal, paragraph, a.text.w - S(16))) lines.push_back(line);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    const float line = LineHeight(Font::Normal);
    a.scroll = std::clamp(a.scroll, 0.0f, std::max(0.0f, lines.size() * line + S(16) - a.text.h));
    SDL_Rect clip{(int)a.text.x + 1, (int)a.text.y + 1, (int)a.text.w - 2, (int)a.text.h - 2};
    SDL_SetRenderClipRect(g.renderer, &clip);
    float y = a.text.y + S(8) - a.scroll;
    for (const std::string& text : lines) {
        if (y + line >= a.text.y && y <= a.text.B()) DrawText(Font::Normal, text, a.text.x + S(8), y, p.text, a.text.w - S(16));
        y += line;
    }
    SDL_SetRenderClipRect(g.renderer, nullptr);

    // Discography list.
    a.missing = {panel.x + pad, a.text.B() + S(6), panel.w - pad * 2, S(22)};
    panel::DrawCheck(a.missing, "Only albums not in my library", a.missingOnly ? 1 : 0, p);
    a.list = {panel.x + pad, a.missing.B() + S(6), panel.w - pad * 2, listH};
    Fill(a.list, p.surface2);
    Frame(a.list, p.border);
    const float row = RowHeight(), year = S(52), type = S(100), owned = S(90);
    const float titleX = a.list.x + S(8) + year, titleW = a.list.w - S(16) - year - type - owned;
    float hy = a.list.y + S(3);
    DrawText(Font::Small, "Year", a.list.x + S(8), hy, p.sub, year);
    DrawText(Font::Small, "Album", titleX, hy, p.sub, titleW);
    DrawText(Font::Small, "Type", titleX + titleW, hy, p.sub, type);
    DrawText(Font::Small, "In library", titleX + titleW + type, hy, p.sub, owned);
    const float rowsTop = a.list.y + row, rowsH = a.list.B() - rowsTop - 1;
    a.listScroll = std::clamp(a.listScroll, 0.0f, std::max(0.0f, a.shown.size() * row - rowsH));
    SDL_Rect rows{(int)a.list.x + 1, (int)rowsTop, (int)a.list.w - 2, (int)rowsH};
    SDL_SetRenderClipRect(g.renderer, &rows);
    for (int i = 0; i < (int)a.shown.size(); ++i) {
        const float ry = rowsTop + i * row - a.listScroll;
        if (ry + row < rowsTop || ry > a.list.B()) continue;
        const int gi = a.shown[i];
        const online::ReleaseGroup& r = a.discography[gi];
        Rect band{a.list.x + 1, ry, a.list.w - 2, row};
        const bool sel = gi == a.selectedGroup;
        if (sel) Fill(band, p.select);
        else if (band.In(panel::g_mx, panel::g_my) && a.list.In(panel::g_mx, panel::g_my)) Fill(band, p.hover);
        const uint32_t cText = sel ? p.selectText : p.text;
        const uint32_t cSub = sel ? p.selectText : p.sub;
        const float ty = ry + S(3);
        if (r.year) DrawText(Font::Normal, std::to_string(r.year), a.list.x + S(8), ty, cSub, year);
        DrawText(Font::Normal, r.title, titleX, ty, cText, titleW - S(8));
        DrawText(Font::Normal, r.type, titleX + titleW, ty, cSub, type - S(8));
        if (r.owned) DrawText(Font::Normal, "In library", titleX + titleW + type, ty, sel ? cText : p.accent, owned);
    }
    SDL_SetRenderClipRect(g.renderer, nullptr);

    // Selected album's tracklist.
    const float headingY = a.list.B() + S(8);
    DrawText(Font::Bold, AlbumHeading(), panel.x + pad, headingY, p.text, panel.w - pad * 2);
    const float trackTop = headingY + S(20);
    a.trackList = {panel.x + pad, trackTop, panel.w - pad * 2, std::max(S(56), albumBtnY - S(8) - trackTop)};
    Fill(a.trackList, p.surface2);
    Frame(a.trackList, p.border);
    const float numW = S(36), timeW = S(58);
    const float trTitleX = a.trackList.x + S(8) + numW + S(8);
    const float trTitleW = a.trackList.w - S(16) - numW - S(8) - timeW;
    const float thy = a.trackList.y + S(3);
    DrawText(Font::Small, "#", a.trackList.x + S(8), thy, p.sub, numW, 2);
    DrawText(Font::Small, "Track", trTitleX, thy, p.sub, trTitleW);
    DrawText(Font::Small, "Time", trTitleX + trTitleW, thy, p.sub, timeW, 2);

    const online::ReleaseGroup* rg = SelectedGroup();
    const bool isOwned = rg && rg->owned && !a.ownedTracks.empty();
    const bool missingReady = rg && !rg->owned && a.currentAlbumValid && !a.currentAlbum.tracks.empty();
    const int trackCount = isOwned ? (int)a.ownedTracks.size() : (missingReady ? (int)a.currentAlbum.tracks.size() : 0);
    const float trRowsTop = a.trackList.y + row, trRowsH = a.trackList.B() - trRowsTop - 1;
    a.trackScroll = std::clamp(a.trackScroll, 0.0f, std::max(0.0f, trackCount * row - trRowsH));
    SDL_Rect trClip{(int)a.trackList.x + 1, (int)trRowsTop, (int)a.trackList.w - 2, (int)trRowsH};
    SDL_SetRenderClipRect(g.renderer, &trClip);
    for (int i = 0; i < trackCount; ++i) {
        const float ry = trRowsTop + i * row - a.trackScroll;
        if (ry + row < trRowsTop || ry > a.trackList.B()) continue;
        Rect band{a.trackList.x + 1, ry, a.trackList.w - 2, row};
        const bool sel = i == a.selectedTrack;
        if (sel) Fill(band, p.select);
        else if (band.In(panel::g_mx, panel::g_my) && a.trackList.In(panel::g_mx, panel::g_my)) Fill(band, p.hover);
        const uint32_t cText = sel ? p.selectText : p.text;
        const uint32_t cSub = sel ? p.selectText : p.sub;
        const float ty = ry + S(3);
        std::string num, title, dur;
        if (isOwned) {
            const Track& t = g_lib[a.ownedTracks[i]];
            num = std::to_string(t.trackNo ? t.trackNo : (i + 1));
            title = t.title ? std::string(g_lib.View(t.title)) : ToUtf8(StemOf(g_lib.Str(t.file)));
            if (t.Duration() > 0) dur = Utf8(FormatDuration(t.Duration()));
        } else {
            const radar::AlbumTrack& t = a.currentAlbum.tracks[i];
            num = std::to_string(t.position ? t.position : (i + 1));
            title = t.title;
            if (t.durationSec > 0) dur = Utf8(FormatDuration((double)t.durationSec));
        }
        DrawText(Font::Normal, num, a.trackList.x + S(8), ty, cSub, numW, 2);
        DrawText(Font::Normal, title, trTitleX, ty, cText, trTitleW - S(8));
        if (!dur.empty()) DrawText(Font::Normal, dur, trTitleX + trTitleW, ty, cSub, timeW, 2);
    }
    SDL_SetRenderClipRect(g.renderer, nullptr);

    a.albumPrimary = {panel.x + pad, albumBtnY, S(108), S(28)};
    a.albumSecondary = {a.albumPrimary.R() + S(6), albumBtnY, S(164), S(28)};
    a.albumMb = {a.albumSecondary.R() + S(6), albumBtnY, S(148), S(28)};
    DrawButton(a.albumPrimary, "Play album", p, false, isOwned || missingReady);
    DrawButton(a.albumSecondary, (rg && rg->owned) ? "Show in library" : "Save album to library", p, false,
               isOwned || missingReady);
    DrawButton(a.albumMb, "Album on MusicBrainz", p, false, rg && !rg->id.empty());

    DrawText(Font::Small, a.status, panel.x + pad, statusY, p.sub, panel.w - pad * 2);
    a.musicbrainz = {panel.x + pad, bottom, S(108), S(30)};
    a.wikipedia = {a.musicbrainz.R() + S(6), bottom, S(108), S(30)};
    a.official = {a.wikipedia.R() + S(6), bottom, S(108), S(30)};
    a.close = {panel.R() - pad - S(100), bottom, S(100), S(30)};
    DrawButton(a.musicbrainz, "MusicBrainz", p, false, !a.info.musicbrainz.empty());
    DrawButton(a.wikipedia, "Wikipedia", p, false, !a.info.wikipedia.empty());
    DrawButton(a.official, "Official site", p, false, !a.info.official.empty());
    DrawButton(a.close, "Close", p, true);
}

void ArtistPanelMouseDown(float x, float y, int clicks) {
    if (a.close.In(x, y)) ClosePanel();
    else if (a.musicbrainz.In(x, y)) OpenUrl(a.info.musicbrainz);
    else if (a.wikipedia.In(x, y)) OpenUrl(a.info.wikipedia);
    else if (a.official.In(x, y)) OpenUrl(a.info.official);
    else if (a.albumPrimary.In(x, y)) PlaySelectedAlbum(0);
    else if (a.albumSecondary.In(x, y)) {
        if (const online::ReleaseGroup* rg = SelectedGroup(); rg && rg->owned) ShowCurrentInLibrary();
        else SaveCurrentAlbum();
    } else if (a.albumMb.In(x, y)) {
        if (const online::ReleaseGroup* rg = SelectedGroup()) OpenUrl(rg->Url());
    } else if (a.missing.In(x, y)) {
        a.missingOnly = !a.missingOnly;
        FillAlbums();
    } else if (a.list.In(x, y)) {
        const float row = RowHeight();
        const int i = (int)std::floor((y - a.list.y - row + a.listScroll) / row);
        if (y >= a.list.y + row && i >= 0 && i < (int)a.shown.size()) {
            const int gi = a.shown[i];
            if (gi != a.selectedGroup || a.albumWaitConfirm || a.albumFailed) SelectAlbumGroup(gi, true);
            else if (clicks >= 2) PlaySelectedAlbum(0);
        }
    } else if (a.trackList.In(x, y)) {
        const online::ReleaseGroup* rg = SelectedGroup();
        const bool owned = rg && rg->owned && !a.ownedTracks.empty();
        const bool missingReady = rg && !rg->owned && a.currentAlbumValid && !a.currentAlbum.tracks.empty();
        const int count = owned ? (int)a.ownedTracks.size() : (missingReady ? (int)a.currentAlbum.tracks.size() : 0);
        const float row = RowHeight();
        const int i = (int)std::floor((y - a.trackList.y - row + a.trackScroll) / row);
        if (y >= a.trackList.y + row && i >= 0 && i < count) {
            a.selectedTrack = i;
            if (clicks >= 2) PlaySelectedAlbum(i);
        }
    }
    g.redraw = true;
}

void ArtistPanelKey(const SDL_KeyboardEvent& e) {
    if (e.key == SDLK_ESCAPE) {
        ClosePanel();
    } else if (e.key == SDLK_RETURN || e.key == SDLK_KP_ENTER) {
        if (const online::ReleaseGroup* rg = SelectedGroup()) {
            if (!rg->owned && (a.albumWaitConfirm || a.albumFailed)) StartAlbumLookup(*rg);
            else PlaySelectedAlbum(a.selectedTrack >= 0 ? a.selectedTrack : 0);
        } else {
            ClosePanel();
        }
    } else if (e.key == SDLK_DOWN) {
        if (!a.shown.empty()) {
            int cur = -1;
            for (int i = 0; i < (int)a.shown.size(); ++i)
                if (a.shown[i] == a.selectedGroup) cur = i;
            const int next = std::min((int)a.shown.size() - 1, cur + 1);
            SelectAlbumGroup(a.shown[next], false);
            const float row = RowHeight(), rowsH = std::max(row, a.list.h - row - 1);
            if ((next + 1) * row > a.listScroll + rowsH) a.listScroll = (next + 1) * row - rowsH;
        }
    } else if (e.key == SDLK_UP) {
        if (!a.shown.empty()) {
            int cur = 0;
            for (int i = 0; i < (int)a.shown.size(); ++i)
                if (a.shown[i] == a.selectedGroup) cur = i;
            const int next = std::max(0, cur - 1);
            SelectAlbumGroup(a.shown[next], false);
            const float row = RowHeight();
            if (next * row < a.listScroll) a.listScroll = next * row;
        }
    } else if (e.key == SDLK_PAGEDOWN) {
        a.listScroll += a.list.h - RowHeight() * 2;
    } else if (e.key == SDLK_PAGEUP) {
        a.listScroll -= a.list.h - RowHeight() * 2;
    }
    g.redraw = true;
}

void ArtistPanelWheel(float x, float y, float dy) {
    if (a.text.In(x, y)) a.scroll -= dy * LineHeight(Font::Normal) * 3;
    else if (a.list.In(x, y)) a.listScroll -= dy * RowHeight() * 3;
    else if (a.trackList.In(x, y)) a.trackScroll -= dy * RowHeight() * 3;
    g.redraw = true;
}

void ArtistPanelClose() {
    StopLookup();
    a.open = false;
    a.info = {};
    a.discography.clear();
    a.shown.clear();
    a.albumCache.clear();
    a.currentAlbum = {};
    a.ownedTracks.clear();
}

void StopArtistInfo() {
    p.shuttingDown = true;
    StopPlayWorker();
    if (a.open) ClosePanel();
    StopLookup();
}

}  // namespace lx
