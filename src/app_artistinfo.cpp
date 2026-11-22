// Artist info on Windows (View > Artist info..., or the track list's right-click menu): a small
// modeless window with the selected or playing track's artist as MusicBrainz knows them, the
// opening of their English Wikipedia article, and buttons that open MusicBrainz, Wikipedia and
// the official site in the browser. Below, the artist's albums and EPs as MusicBrainz lists them,
// each marked when the library has it, with a filter for the ones it lacks; selecting an album
// shows its tracklist (from the library when owned, or MusicBrainz when missing) with Play album,
// Save album to library, and Show in library actions.
#include "app.h"

#include "metadata_fetch.h"
#include "release_radar.h"
#include "theme_win.h"
#include "webfetch.h"

#include <algorithm>
#include <memory>
#include <thread>
#include <utility>

namespace app {

namespace {

const wchar_t kArtistClass[] = L"CordinalArtistInfo";
constexpr UINT WM_APP_ARTIST = WM_APP + 1;        // lParam = Result*, owned by the receiver
constexpr UINT WM_APP_ARTIST_ALBUM = WM_APP + 2;  // lParam = AlbumResult*, owned by the receiver
constexpr UINT_PTR kAlbumTimer = 101;
enum {
    kIdName = 100,
    kIdSummary,
    kIdStatus,
    kIdMusicBrainz,
    kIdWikipedia,
    kIdOfficial,
    kIdClose,
    kIdMissing,
    kIdAlbums,
    kIdAlbumHeading,
    kIdTracks,
    kIdAlbumPrimary,
    kIdAlbumSecondary,
    kIdAlbumMb
};

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
};
PlayState* playState = nullptr;

PlayState& Play() {
    if (!playState) playState = new PlayState();
    return *playState;
}

struct Window {
    HWND hwnd = nullptr, name = nullptr, summary = nullptr, status = nullptr, musicbrainz = nullptr,
         wikipedia = nullptr, official = nullptr, close = nullptr, missing = nullptr, albums = nullptr,
         albumHeading = nullptr, tracks = nullptr, albumPrimary = nullptr, albumSecondary = nullptr,
         albumMb = nullptr;
    std::string artist;
    online::ArtistInfo info;
    std::vector<online::ReleaseGroup> discography;
    bool discographyKnown = false;
    std::vector<int> shown;  // the list's rows, as indexes into discography
    bool fillingAlbums = false;
    int selectedGroup = -1;  // index into discography
    std::vector<std::pair<std::string, radar::Album>> albumCache;
    const radar::Album* currentAlbum = nullptr;
    bool albumLoading = false;
    bool albumFailed = false;
    std::vector<TrackId> ownedTracks;
    online::Generation generation, albumGeneration;
    std::shared_ptr<online::Cancel> cancel, albumCancel;
    std::thread worker, albumWorker;
};
Window* winState = nullptr;

Window& State() {
    if (!winState) winState = new Window();
    return *winState;
}

void RemoveTree(const std::wstring& dir) { DiscoverRemoveTree(dir); }

void StopPlayWorker() {
    if (!playState) return;
    PlayState& p = *playState;
    if (p.cancel) *p.cancel = true;
    if (p.worker.joinable()) p.worker.join();
    p.cancel.reset();
    p.gen.Next();
}

void StopAlbumLookup() {
    if (!winState) return;
    Window& w = *winState;
    if (w.hwnd) KillTimer(w.hwnd, kAlbumTimer);
    if (w.albumCancel) w.albumCancel->Stop();
    if (w.albumWorker.joinable()) w.albumWorker.join();
    w.albumCancel.reset();
    w.albumGeneration.Next();
    w.albumLoading = false;
}

void StopLookup() {
    StopAlbumLookup();
    if (!winState) return;
    Window& w = *winState;
    if (w.cancel) w.cancel->Stop();
    if (w.worker.joinable()) w.worker.join();
    w.cancel.reset();
    w.generation.Next();
}

const online::ReleaseGroup* SelectedGroup() {
    Window& w = State();
    if (w.selectedGroup < 0 || w.selectedGroup >= (int)w.discography.size()) return nullptr;
    return &w.discography[w.selectedGroup];
}

void UpdateAlbumButtons() {
    Window& w = State();
    const online::ReleaseGroup* rg = SelectedGroup();
    const bool hasRow = rg != nullptr;
    const bool owned = rg && rg->owned && !w.ownedTracks.empty();
    const bool missingReady = rg && !rg->owned && w.currentAlbum && !w.currentAlbum->tracks.empty();
    SetWindowTextW(w.albumSecondary, (rg && rg->owned) ? L"Show in &library" : L"&Save album to library");
    EnableWindow(w.albumPrimary, owned || missingReady);
    EnableWindow(w.albumSecondary, owned || missingReady);
    EnableWindow(w.albumMb, hasRow && !rg->id.empty());
}

void FillTrackRows() {
    Window& w = State();
    ListView_DeleteAllItems(w.tracks);
    const online::ReleaseGroup* rg = SelectedGroup();
    if (!rg) {
        SetWindowTextW(w.albumHeading, L"Select an album above to see its tracklist.");
        UpdateAlbumButtons();
        return;
    }
    std::wstring heading = FromUtf8(rg->title);
    if (rg->year) heading += L" (" + std::to_wstring(rg->year) + L")";
    if (rg->owned) {
        if (w.ownedTracks.empty()) {
            heading += L" \x2014 no playable tracks found in library";
        } else {
            double totalSec = 0;
            for (TrackId id : w.ownedTracks) totalSec += g_lib[id].Duration();
            heading += L" \x2014 " + std::to_wstring(w.ownedTracks.size()) +
                       (w.ownedTracks.size() == 1 ? L" track" : L" tracks");
            if (totalSec > 0) heading += L", " + FormatDuration(totalSec);
            for (int i = 0; i < (int)w.ownedTracks.size(); ++i) {
                const Track& t = g_lib[w.ownedTracks[i]];
                std::wstring num = t.trackNo ? std::to_wstring(t.trackNo) : std::to_wstring(i + 1);
                std::wstring title = t.title ? FromUtf8(g_lib.View(t.title)) : StemOf(g_lib.Str(t.file));
                std::wstring dur = t.Duration() > 0 ? FormatDuration(t.Duration()) : L"";
                LVITEMW item{LVIF_TEXT, i};
                item.pszText = num.data();
                ListView_InsertItem(w.tracks, &item);
                ListView_SetItemText(w.tracks, i, 1, title.data());
                ListView_SetItemText(w.tracks, i, 2, dur.data());
            }
        }
    } else if (w.albumLoading) {
        heading += L" \x2014 Reading tracklist from MusicBrainz...";
    } else if (w.albumFailed || !w.currentAlbum || w.currentAlbum->tracks.empty()) {
        heading += L" \x2014 MusicBrainz has no tracklist for this release.";
    } else {
        unsigned totalSec = 0;
        for (const radar::AlbumTrack& t : w.currentAlbum->tracks) totalSec += t.durationSec;
        heading += L" \x2014 " + std::to_wstring(w.currentAlbum->tracks.size()) +
                   (w.currentAlbum->tracks.size() == 1 ? L" track" : L" tracks");
        if (totalSec > 0) heading += L", " + FormatDuration((double)totalSec);
        for (int i = 0; i < (int)w.currentAlbum->tracks.size(); ++i) {
            const radar::AlbumTrack& t = w.currentAlbum->tracks[i];
            std::wstring num = t.position ? std::to_wstring(t.position) : std::to_wstring(i + 1);
            std::wstring title = FromUtf8(t.title);
            std::wstring dur = t.durationSec > 0 ? FormatDuration((double)t.durationSec) : L"";
            LVITEMW item{LVIF_TEXT, i};
            item.pszText = num.data();
            ListView_InsertItem(w.tracks, &item);
            ListView_SetItemText(w.tracks, i, 1, title.data());
            ListView_SetItemText(w.tracks, i, 2, dur.data());
        }
    }
    SetWindowTextW(w.albumHeading, heading.c_str());
    UpdateAlbumButtons();
}

void StartAlbumLookup(const online::ReleaseGroup& rg) {
    Window& w = State();
    StopAlbumLookup();
    w.currentAlbum = nullptr;
    w.albumFailed = false;
    w.albumLoading = true;
    FillTrackRows();

    radar::Item item;
    item.artist = !w.info.name.empty() ? w.info.name : w.artist;
    item.groupId = rg.id;
    item.title = rg.title;
    item.type = rg.type;
    item.firstDate = rg.firstDate;
    const uint64_t gen = w.albumGeneration.Next();
    w.albumCancel = std::make_shared<online::Cancel>();
    const HWND target = w.hwnd;
    w.albumWorker = std::thread([item, gen, target, cancel = w.albumCancel] {
        auto* res = new AlbumResult{gen, item.groupId, false, {}};
        res->ok = radar::FetchAlbum(item, res->album, *cancel);
        if (!PostMessageW(target, WM_APP_ARTIST_ALBUM, 0, (LPARAM)res)) delete res;
    });
}

void SelectAlbumGroup(int groupIdx, bool immediate) {
    Window& w = State();
    w.selectedGroup = groupIdx;
    w.ownedTracks.clear();
    w.currentAlbum = nullptr;
    w.albumFailed = false;
    StopAlbumLookup();
    const online::ReleaseGroup* rg = SelectedGroup();
    if (!rg) {
        FillTrackRows();
        return;
    }
    if (rg->owned) {
        w.ownedTracks = online::LibraryAlbumTracks(g_lib, w.artist, rg->title);
        FillTrackRows();
        return;
    }
    for (const auto& [id, album] : w.albumCache) {
        if (id != rg->id) continue;
        w.currentAlbum = &album;
        w.albumFailed = album.tracks.empty();
        FillTrackRows();
        return;
    }
    if (immediate) {
        StartAlbumLookup(*rg);
    } else {
        w.albumLoading = true;
        FillTrackRows();
        SetTimer(w.hwnd, kAlbumTimer, 220, nullptr);
    }
}

void StartLookup() {
    Window& w = State();
    StopLookup();
    w.info = {};
    w.discography.clear();
    w.shown.clear();
    w.selectedGroup = -1;
    w.albumCache.clear();
    w.currentAlbum = nullptr;
    w.albumFailed = false;
    w.ownedTracks.clear();
    w.fillingAlbums = true;
    ListView_DeleteAllItems(w.albums);
    w.fillingAlbums = false;
    FillTrackRows();
    const uint64_t generation = w.generation.Next();
    w.cancel = std::make_shared<online::Cancel>();
    SetWindowTextW(w.status, L"Asking MusicBrainz...");
    SetWindowTextW(w.summary, L"");
    for (HWND b : {w.musicbrainz, w.wikipedia, w.official}) EnableWindow(b, FALSE);
    const HWND target = w.hwnd;
    w.worker = std::thread([artist = w.artist, generation, target, cancel = w.cancel] {
        auto* result = new Result{generation, false, {}, {}, false};
        result->found = online::FetchArtistInfo(artist, {}, *cancel, result->info);
        if (result->found) result->albumsKnown = online::FetchDiscography(result->info.mbid, *cancel, result->albums);
        if (!PostMessageW(target, WM_APP_ARTIST, 0, (LPARAM)result)) delete result;
    });
}

// The discography's rows: every album, or only those the library lacks.
void FillAlbums() {
    Window& w = State();
    w.fillingAlbums = true;
    ListView_DeleteAllItems(w.albums);
    w.shown.clear();
    const bool missingOnly = Button_GetCheck(w.missing) == BST_CHECKED;
    int restoreRow = -1;
    for (int i = 0; i < (int)w.discography.size(); ++i)
        if (!missingOnly || !w.discography[i].owned) w.shown.push_back(i);
    for (int row = 0; row < (int)w.shown.size(); ++row) {
        const int gi = w.shown[row];
        if (gi == w.selectedGroup) restoreRow = row;
        const online::ReleaseGroup& r = w.discography[gi];
        std::wstring year = r.year ? std::to_wstring(r.year) : L"", title = FromUtf8(r.title), type = FromUtf8(r.type);
        LVITEMW item{LVIF_TEXT, row};
        item.pszText = year.data();
        ListView_InsertItem(w.albums, &item);
        ListView_SetItemText(w.albums, row, 1, title.data());
        ListView_SetItemText(w.albums, row, 2, type.data());
        ListView_SetItemText(w.albums, row, 3, const_cast<wchar_t*>(r.owned ? L"In library" : L""));
    }
    if (restoreRow >= 0) {
        ListView_SetItemState(w.albums, restoreRow, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(w.albums, restoreRow, FALSE);
    }
    w.fillingAlbums = false;
    if (restoreRow < 0) SelectAlbumGroup(-1, false);
}

std::wstring AlbumCounts() {
    Window& w = State();
    if (!w.discographyKnown) return L"Their albums couldn't be fetched from MusicBrainz.";
    if (w.discography.empty()) return L"MusicBrainz lists no albums or EPs for this artist.";
    size_t missing = std::count_if(w.discography.begin(), w.discography.end(), [](const auto& r) { return !r.owned; });
    return std::to_wstring(w.discography.size()) + (w.discography.size() == 1 ? L" album or EP, " : L" albums and EPs, ") +
           std::to_wstring(missing) + L" not in your library.";
}

void Show(Result& r) {
    Window& w = State();
    w.info = r.info;
    w.discography = std::move(r.albums);
    w.discographyKnown = r.albumsKnown;
    online::MarkOwned(w.discography, online::LibraryAlbumsBy(g_lib, w.artist));
    FillAlbums();
    if (!r.found) {
        std::wstring text = L"MusicBrainz has no single artist named \x201C" + FromUtf8(w.artist) +
                            L"\x201D, so nothing is shown rather than a namesake.";
        SetWindowTextW(w.status, text.c_str());
        return;
    }
    SetWindowTextW(w.name, FromUtf8(r.info.name).c_str());
    // Edit controls want CR LF line breaks.
    std::wstring summary;
    for (wchar_t c : FromUtf8(r.info.summary)) {
        if (c == L'\n') summary += L'\r';
        summary += c;
    }
    SetWindowTextW(w.summary, summary.empty() ? L"Wikipedia has no English article for this artist." : summary.c_str());
    std::wstring status = r.info.summary.empty() ? L"From MusicBrainz. "
                                                 : L"From MusicBrainz and Wikipedia (text under CC BY-SA 4.0). ";
    SetWindowTextW(w.status, (status + AlbumCounts()).c_str());
    EnableWindow(w.musicbrainz, !r.info.musicbrainz.empty());
    EnableWindow(w.wikipedia, !r.info.wikipedia.empty());
    EnableWindow(w.official, !r.info.official.empty());
}

void Open(const std::string& url) {
    Window& w = State();
    // The addresses came from the network: only web pages are handed to the browser.
    if (url.starts_with("https://") || url.starts_with("http://"))
        ShellExecuteW(w.hwnd, L"open", FromUtf8(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

bool EnsurePlayAudio() {
    Window& w = State();
    if (online::Config().saveMusic || online::Config().searchMusic) return true;
    const wchar_t* text =
        L"Save music from the web is off.\n\nTurn Save music from the web on to play tracks from MusicBrainz (matched on YouTube)? "
        L"It can be changed in Preferences > Online.";
    if (MessageBoxW(w.hwnd, text, L"Play album", MB_YESNO | MB_ICONQUESTION) != IDYES) return false;
    if (const Tweak* t = FindTweak("online.save_music")) SetTweak(*t, 1);
    return online::Config().saveMusic;
}

void SaveCurrentAlbum() {
    Window& w = State();
    if (!w.currentAlbum || w.currentAlbum->tracks.empty()) return;
    ShowWebFetchLists({radar::AlbumToWebFetchList(*w.currentAlbum)});
}

void ShowCurrentInLibrary() {
    Window& w = State();
    if (w.ownedTracks.empty()) return;
    const int lib = FindPlaylist(PlaylistKind::Library);
    if (lib < 0) return;
    if (g.active != lib || RadioActive()) ActivatePlaylist(lib);
    const TrackId first = w.ownedTracks.front();
    auto findRow = [&] {
        const auto& items = g.playlists[lib].items;
        for (int pos = 0; pos < (int)g.view.size(); ++pos)
            if (g.view[pos] < items.size() && items[g.view[pos]] == first) return RowOfViewPos(pos);
        return -1;
    };
    if (CanvasActive()) {
        if (!LibViewShowTrack(first)) {
            SetWindowTextW(g.search, L"");
            FlushSearch();
            LibViewShowTrack(first);
        }
    } else {
        int row = findRow();
        if (row < 0) {
            SetWindowTextW(g.search, L"");
            FlushSearch();
            row = findRow();
        }
        if (row >= 0) {
            ClearSelection();
            ListView_SetItemState(g.list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(g.list, row, FALSE);
            SetFocus(g.list);
        }
    }
    SetForegroundWindow(g.hwnd);
}

void PlayMissingAlbum(int startRow) {
    Window& w = State();
    PlayState& p = Play();
    if (!w.currentAlbum || w.currentAlbum->tracks.empty()) return;
    if (!EnsurePlayAudio()) return;
    const size_t start = (size_t)std::clamp(startRow, 0, (int)w.currentAlbum->tracks.size() - 1);
    WebFetchTrackList list = radar::AlbumToWebFetchList(*w.currentAlbum);
    if (start > 0) list.tracks.erase(list.tracks.begin(), list.tracks.begin() + (ptrdiff_t)start);
    if (list.tracks.empty()) return;

    StopPlayWorker();
    const bool permanent = online::Config().searchSave == 1 && !g.libraryFolders.empty();
    const std::wstring libRoot = permanent ? g.libraryFolders.front() : std::wstring();
    const std::wstring dataDir = g.dataDir;
    const uint64_t gen = p.gen.Next();
    const unsigned seq = ++p.seq;
    p.cancel = std::make_shared<std::atomic<bool>>(false);
    const HWND target = g.hwnd;
    ShowMessage(L"Finding " + FromUtf8(list.tracks.front().title) + L"...");

    p.worker = std::thread([list = std::move(list), permanent, libRoot, dataDir, gen, seq, target,
                            cancel = p.cancel]() mutable {
        auto post = [target](PlayResult* res) {
            if (!PostMessageW(target, WM_APP_ARTIST_PLAY, 0, (LPARAM)res)) delete res;
        };
        const std::wstring previewDir = dataDir + L"\\preview\\artist-" + std::to_wstring(seq);
        WebFetchRun run;
        run.libraryRoot = permanent ? libRoot : previewDir;
        run.dataDir = permanent ? dataDir : dataDir + L"\\preview";
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
    Window& w = State();
    const online::ReleaseGroup* rg = SelectedGroup();
    if (!rg) return;
    if (rg->owned) {
        if (!w.ownedTracks.empty())
            PlayList(w.ownedTracks, std::clamp(startRow, 0, (int)w.ownedTracks.size() - 1));
        return;
    }
    PlayMissingAlbum(startRow);
}

void ShowAlbumContextMenu(POINT screenPt) {
    Window& w = State();
    const online::ReleaseGroup* rg = SelectedGroup();
    if (!rg) return;
    enum { kPlay = 1, kAction, kMb };
    HMENU m = CreatePopupMenu();
    const bool owned = rg->owned && !w.ownedTracks.empty();
    const bool ready = !rg->owned && w.currentAlbum && !w.currentAlbum->tracks.empty();
    AppendMenuW(m, MF_STRING | ((owned || ready) ? MF_ENABLED : MF_GRAYED), kPlay, L"&Play album");
    AppendMenuW(m, MF_STRING | ((owned || ready) ? MF_ENABLED : MF_GRAYED), kAction,
                rg->owned ? L"&Show in library" : L"&Save album to library");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (!rg->id.empty() ? MF_ENABLED : MF_GRAYED), kMb, L"Open on &MusicBrainz");
    const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPt.x, screenPt.y, 0, w.hwnd, nullptr);
    DestroyMenu(m);
    switch (cmd) {
        case kPlay: PlaySelectedAlbum(0); break;
        case kAction:
            if (rg->owned) ShowCurrentInLibrary();
            else SaveCurrentAlbum();
            break;
        case kMb: Open(rg->Url()); break;
    }
}

void LayoutWindow() {
    Window& w = State();
    RECT rc;
    GetClientRect(w.hwnd, &rc);
    const int pad = S(14), bottom = rc.bottom - pad - S(30), width = rc.right - pad * 2;
    const int statusY = bottom - S(26), albumBtnY = statusY - S(34);
    const int top = pad + S(28), avail = std::max(S(240), albumBtnY - S(8) - top);
    const int summaryH = avail * 24 / 100;
    const int albumsY = top + summaryH + S(32), albumsH = avail * 34 / 100;
    const int headingY = albumsY + albumsH + S(8);
    const int tracksY = headingY + S(22), tracksH = std::max(S(50), albumBtnY - S(8) - tracksY);

    SetWindowPos(w.name, nullptr, pad, pad, width, S(24), SWP_NOZORDER);
    SetWindowPos(w.summary, nullptr, pad, top, width, summaryH, SWP_NOZORDER);
    SetWindowPos(w.missing, nullptr, pad, top + summaryH + S(6), width, S(22), SWP_NOZORDER);
    SetWindowPos(w.albums, nullptr, pad, albumsY, width, albumsH, SWP_NOZORDER);
    SetWindowPos(w.albumHeading, nullptr, pad, headingY, width, S(20), SWP_NOZORDER);
    SetWindowPos(w.tracks, nullptr, pad, tracksY, width, tracksH, SWP_NOZORDER);

    const int scrollbar = GetSystemMetricsForDpi(SM_CXVSCROLL, g.dpi);
    ListView_SetColumnWidth(w.albums, 1, std::max(S(120), width - S(52) - S(100) - S(90) - scrollbar - S(4)));
    ListView_SetColumnWidth(w.tracks, 1, std::max(S(120), width - S(42) - S(62) - scrollbar - S(4)));

    SetWindowPos(w.albumPrimary, nullptr, pad, albumBtnY, S(108), S(28), SWP_NOZORDER);
    SetWindowPos(w.albumSecondary, nullptr, pad + S(114), albumBtnY, S(164), S(28), SWP_NOZORDER);
    SetWindowPos(w.albumMb, nullptr, pad + S(284), albumBtnY, S(148), S(28), SWP_NOZORDER);

    SetWindowPos(w.status, nullptr, pad, statusY, width, S(20), SWP_NOZORDER);
    int x = pad;
    for (HWND b : {w.musicbrainz, w.wikipedia, w.official}) {
        SetWindowPos(b, nullptr, x, bottom, S(108), S(30), SWP_NOZORDER);
        x += S(114);
    }
    SetWindowPos(w.close, nullptr, rc.right - pad - S(100), bottom, S(100), S(30), SWP_NOZORDER);
}

LRESULT CALLBACK ArtistProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    Window& w = State();
    switch (msg) {
        case WM_CREATE: {
            w.hwnd = h;
            auto make = [&](DWORD ex, const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
                HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, h,
                                         (HMENU)(INT_PTR)id, g.inst, nullptr);
                SendMessageW(c, WM_SETFONT, (WPARAM)g.font, TRUE);
                return c;
            };
            auto initList = [&](HWND lv, const wchar_t** cols, const int* widths, const int* fmts, int n) {
                ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
                for (int c = 0; c < n; ++c) {
                    LVCOLUMNW col{};
                    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
                    col.fmt = fmts[c];
                    col.cx = widths[c];
                    col.pszText = const_cast<wchar_t*>(cols[c]);
                    ListView_InsertColumn(lv, c, &col);
                }
                for (HWND c : {lv, ListView_GetHeader(lv)}) ApplyControlTheme(c, g.dark);
                ListView_SetBkColor(lv, Ref(P().surface2));
                ListView_SetTextBkColor(lv, Ref(P().surface2));
                ListView_SetTextColor(lv, TextColor());
            };
            w.name = make(0, WC_STATICW, FromUtf8(w.artist).c_str(), SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, kIdName);
            SendMessageW(w.name, WM_SETFONT, (WPARAM)g.fontBold, TRUE);
            w.summary = make(WS_EX_CLIENTEDGE, WC_EDITW, L"",
                             ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP, kIdSummary);
            SetWindowTheme(w.summary, g.dark ? L"DarkMode_CFD" : L"Explorer", nullptr);
            w.missing = make(0, WC_BUTTONW, L"&Only albums not in my library", BS_AUTOCHECKBOX | WS_TABSTOP, kIdMissing);
            w.albums = make(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                            LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER | WS_TABSTOP, kIdAlbums);
            const wchar_t* albumCols[] = {L"Year", L"Album", L"Type", L"In library"};
            const int albumWidths[] = {S(52), S(220), S(100), S(90)};
            const int albumFmts[] = {LVCFMT_LEFT, LVCFMT_LEFT, LVCFMT_LEFT, LVCFMT_LEFT};
            initList(w.albums, albumCols, albumWidths, albumFmts, 4);

            w.albumHeading = make(0, WC_STATICW, L"Select an album above to see its tracklist.",
                                  SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, kIdAlbumHeading);
            SendMessageW(w.albumHeading, WM_SETFONT, (WPARAM)g.fontBold, TRUE);
            w.tracks = make(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                            LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER | WS_TABSTOP, kIdTracks);
            const wchar_t* trackCols[] = {L"#", L"Track", L"Time"};
            const int trackWidths[] = {S(42), S(320), S(62)};
            const int trackFmts[] = {LVCFMT_RIGHT, LVCFMT_LEFT, LVCFMT_RIGHT};
            initList(w.tracks, trackCols, trackWidths, trackFmts, 3);

            w.albumPrimary = make(0, WC_BUTTONW, L"&Play album", BS_PUSHBUTTON | WS_TABSTOP, kIdAlbumPrimary);
            w.albumSecondary =
                make(0, WC_BUTTONW, L"&Save album to library", BS_PUSHBUTTON | WS_TABSTOP, kIdAlbumSecondary);
            w.albumMb = make(0, WC_BUTTONW, L"Album on &MusicBrainz", BS_PUSHBUTTON | WS_TABSTOP, kIdAlbumMb);

            w.status = make(0, WC_STATICW, L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, kIdStatus);
            w.musicbrainz = make(0, WC_BUTTONW, L"&MusicBrainz", BS_PUSHBUTTON | WS_TABSTOP, kIdMusicBrainz);
            w.wikipedia = make(0, WC_BUTTONW, L"&Wikipedia", BS_PUSHBUTTON | WS_TABSTOP, kIdWikipedia);
            w.official = make(0, WC_BUTTONW, L"&Official site", BS_PUSHBUTTON | WS_TABSTOP, kIdOfficial);
            w.close = make(0, WC_BUTTONW, L"Close", BS_PUSHBUTTON | WS_TABSTOP, kIdClose);
            for (HWND c : {w.musicbrainz, w.wikipedia, w.official, w.close, w.missing, w.albumPrimary,
                           w.albumSecondary, w.albumMb})
                ApplyControlTheme(c, g.dark);
            ApplyWindowTheme(h, P());
            UpdateAlbumButtons();
            LayoutWindow();
            return 0;
        }
        case WM_SIZE:
            LayoutWindow();
            return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {S(520), S(500)};
            return 0;
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(h, &rc);
            FillRect((HDC)wp, &rc, g.surfaceBrush);
            return 1;
        }
        case WM_CTLCOLORSTATIC:
            if ((HWND)lp == w.summary) return FieldControlColor((HDC)wp);
            return SurfaceControlColor((HDC)wp, (HWND)lp == w.status ? SubColor() : TextColor());
        case WM_CTLCOLORBTN:
            return SurfaceControlColor((HDC)wp, TextColor());
        case WM_CTLCOLOREDIT:
            return FieldControlColor((HDC)wp);
        case WM_TIMER:
            if (wp == kAlbumTimer) {
                KillTimer(h, kAlbumTimer);
                if (const online::ReleaseGroup* rg = SelectedGroup(); rg && !rg->owned) StartAlbumLookup(*rg);
                return 0;
            }
            break;
        case WM_NOTIFY: {
            auto* hdr = reinterpret_cast<NMHDR*>(lp);
            if (hdr->idFrom == kIdAlbums) {
                if (hdr->code == LVN_ITEMCHANGED && !w.fillingAlbums) {
                    auto* nm = reinterpret_cast<NMLISTVIEW*>(lp);
                    if ((nm->uChanged & LVIF_STATE) && (nm->uNewState & LVIS_SELECTED) &&
                        !(nm->uOldState & LVIS_SELECTED)) {
                        if (nm->iItem >= 0 && nm->iItem < (int)w.shown.size())
                            SelectAlbumGroup(w.shown[nm->iItem], false);
                    }
                } else if (hdr->code == LVN_ITEMACTIVATE || hdr->code == NM_DBLCLK) {
                    int row = ListView_GetNextItem(w.albums, -1, LVNI_SELECTED);
                    if (row >= 0 && row < (int)w.shown.size()) {
                        const int gi = w.shown[row];
                        const online::ReleaseGroup& rg = w.discography[gi];
                        if (gi != w.selectedGroup || (!rg.owned && w.albumLoading && !w.albumWorker.joinable()))
                            SelectAlbumGroup(gi, true);
                        else if (rg.owned || (w.currentAlbum && !w.currentAlbum->tracks.empty()))
                            PlaySelectedAlbum(0);
                    }
                } else if (hdr->code == NM_RCLICK) {
                    auto* nm = reinterpret_cast<NMITEMACTIVATE*>(lp);
                    if (nm->iItem >= 0 && nm->iItem < (int)w.shown.size()) {
                        ListView_SetItemState(w.albums, nm->iItem, LVIS_SELECTED | LVIS_FOCUSED,
                                              LVIS_SELECTED | LVIS_FOCUSED);
                        if (w.shown[nm->iItem] != w.selectedGroup) SelectAlbumGroup(w.shown[nm->iItem], true);
                        POINT pt = nm->ptAction;
                        ClientToScreen(w.albums, &pt);
                        ShowAlbumContextMenu(pt);
                    }
                }
            } else if (hdr->idFrom == kIdTracks && (hdr->code == LVN_ITEMACTIVATE || hdr->code == NM_DBLCLK)) {
                int row = ListView_GetNextItem(w.tracks, -1, LVNI_SELECTED);
                if (row >= 0) PlaySelectedAlbum(row);
            }
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case kIdMissing: FillAlbums(); break;
                case kIdAlbumPrimary: PlaySelectedAlbum(0); break;
                case kIdAlbumSecondary:
                    if (const online::ReleaseGroup* rg = SelectedGroup(); rg && rg->owned) ShowCurrentInLibrary();
                    else SaveCurrentAlbum();
                    break;
                case kIdAlbumMb:
                    if (const online::ReleaseGroup* rg = SelectedGroup()) Open(rg->Url());
                    break;
                case kIdMusicBrainz: Open(w.info.musicbrainz); break;
                case kIdWikipedia: Open(w.info.wikipedia); break;
                case kIdOfficial: Open(w.info.official); break;
                case kIdClose:
                case IDCANCEL: DestroyWindow(h); break;
            }
            return 0;
        case WM_APP_ARTIST: {
            std::unique_ptr<Result> result(reinterpret_cast<Result*>(lp));
            if (!w.generation.Accepts(result->generation)) return 0;  // another artist was asked for since
            if (w.worker.joinable()) w.worker.join();
            w.cancel.reset();
            Show(*result);
            return 0;
        }
        case WM_APP_ARTIST_ALBUM: {
            std::unique_ptr<AlbumResult> res(reinterpret_cast<AlbumResult*>(lp));
            if (!w.albumGeneration.Accepts(res->generation)) return 0;
            if (w.albumWorker.joinable()) w.albumWorker.join();
            w.albumCancel.reset();
            w.albumLoading = false;
            auto cacheAlbum = [&](radar::Album&& al) -> const radar::Album* {
                for (auto& [id, existing] : w.albumCache)
                    if (id == res->groupId) { existing = std::move(al); return &existing; }
                w.albumCache.emplace_back(res->groupId, std::move(al));
                return &w.albumCache.back().second;
            };
            if (res->ok && !res->album.tracks.empty()) {
                w.currentAlbum = cacheAlbum(std::move(res->album));
                w.albumFailed = false;
            } else {
                w.currentAlbum = cacheAlbum(radar::Album{});
                w.albumFailed = true;
            }
            FillTrackRows();
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY: {
            StopLookup();
            MSG pending;
            while (PeekMessageW(&pending, h, WM_APP_ARTIST, WM_APP_ARTIST, PM_REMOVE))
                delete reinterpret_cast<Result*>(pending.lParam);
            while (PeekMessageW(&pending, h, WM_APP_ARTIST_ALBUM, WM_APP_ARTIST_ALBUM, PM_REMOVE))
                delete reinterpret_cast<AlbumResult*>(pending.lParam);
            w.hwnd = nullptr;
            w.info = {};
            w.discography.clear();
            w.shown.clear();
            w.albumCache.clear();
            w.currentAlbum = nullptr;
            w.ownedTracks.clear();
            return 0;
        }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// The lookup may run while it is off, but only after the user agrees to what it sends.
bool EnsureArtistInfo() {
    if (online::Config().artistInfo) return true;
    const wchar_t* text =
        L"Artist info is off, so cordinal hasn't contacted anyone.\n\n"
        L"It sends the artist's name to musicbrainz.org, which also lists their albums, then asks "
        L"wikidata.org and en.wikipedia.org for the article MusicBrainz names.\n\nTurn Artist info on? It can be changed in Preferences > Online.";
    if (MessageBoxW(g.hwnd, text, L"Artist info", MB_YESNO | MB_ICONQUESTION) != IDYES) return false;
    if (const Tweak* t = FindTweak("online.artist_info")) SetTweak(*t, 1);
    return online::Config().artistInfo;
}

}  // namespace

void OnArtistPlayMessage(WPARAM, LPARAM lp) {
    std::unique_ptr<PlayResult> res(reinterpret_cast<PlayResult*>(lp));
    if (!res) return;
    if (!playState || playState->shuttingDown || !playState->gen.Accepts(res->generation)) {
        if (!res->dir.empty()) RemoveTree(res->dir);
        return;
    }
    if (res->done) return;
    if (!res->ok) {
        if (!res->dir.empty()) RemoveTree(res->dir);
        if (res->first) ShowMessage(L"Couldn't play song: " + FromUtf8(res->error) + L".");
        return;
    }
    const TrackId id = DiscoverAddTrack(res->file, res->track, res->permanent, res->dir, res->albumArtist, res->date);
    if (id == kInvalidTrack) return;
    if (res->first) {
        ClearQueue();
        PlayList({id}, 0);
        ShowMessage(L"Playing " + FromUtf8(res->track.album.empty() ? res->track.title : res->track.album) + L"...");
    } else {
        const size_t yours = g.queue.size() - std::min(g.queuePicks, g.queue.size());
        g.queue.insert(g.queue.begin() + (ptrdiff_t)yours, id);
        RefreshNext();
    }
}

void ShowArtistInfoFor(const std::string& artist) {
    if (artist.empty()) return;
    if (!EnsureArtistInfo()) return;
    Window& w = State();
    if (w.hwnd && w.artist == artist) {
        ShowWindow(w.hwnd, SW_RESTORE);
        SetForegroundWindow(w.hwnd);
        return;
    }
    if (w.hwnd) DestroyWindow(w.hwnd);
    w.artist = artist;
    RegisterAppClass(kArtistClass, ArtistProc);
    RECT r{0, 0, S(660), S(680)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_DLGMODALFRAME, g.dpi);
    RECT owner;
    GetWindowRect(g.hwnd, &owner);
    const int width = r.right - r.left, height = r.bottom - r.top;
    CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kArtistClass, L"Artist info",
                    WS_OVERLAPPEDWINDOW & ~WS_MINIMIZEBOX & ~WS_MAXIMIZEBOX | WS_CLIPCHILDREN,
                    owner.left + (owner.right - owner.left - width) / 2, owner.top + S(60), width, height, g.hwnd,
                    nullptr, g.inst, nullptr);
    if (!w.hwnd) return;
    ShowWindow(w.hwnd, SW_SHOW);
    StartLookup();
}

void ShowArtistInfo() {
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
    ShowArtistInfoFor(artist);
}

bool ArtistInfoDialogMessage(MSG* msg) { return winState && winState->hwnd && IsDialogMessageW(winState->hwnd, msg); }

void StopArtistInfo() {
    if (playState) {
        playState->shuttingDown = true;
        StopPlayWorker();
        delete playState;
        playState = nullptr;
    }
    if (g.hwnd) {
        MSG pending{};
        while (PeekMessageW(&pending, g.hwnd, WM_APP_ARTIST_PLAY, WM_APP_ARTIST_PLAY, PM_REMOVE)) {
            auto* r = reinterpret_cast<PlayResult*>(pending.lParam);
            if (r && !r->dir.empty()) RemoveTree(r->dir);
            delete r;
        }
    }
    if (winState && winState->hwnd) DestroyWindow(winState->hwnd);
    StopLookup();
    delete winState;
    winState = nullptr;
}

}  // namespace app
