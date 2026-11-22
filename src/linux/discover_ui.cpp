// Discover outside my library on Linux (discover_ui.h, discovery.h). The look runs on a worker:
// what the library owns is copied, MusicBrainz picks a related artist with an unowned release, and
// the song is downloaded tagged into the data folder by the web-save queue. It queues like any
// other song, never enters the library or the play counts, and is deleted once nothing holds it.
// Turning radio or the preference off stops new looks; a queued preview still plays like any song.
#include "discover_ui.h"

#include "discovery.h"
#include "release_radar.h"
#include "webfetch.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <thread>

namespace lx {
namespace {

struct Preview {
    TrackId id = kNone;
    WebTrack track;
    std::wstring file, dir;
    bool kept = false;
};

struct LookResult {
    bool ok = false;
    WebTrack track;
    std::wstring file, dir;
};

std::vector<Preview> previews;
std::thread worker;
std::shared_ptr<online::Cancel> lookCancel;
std::atomic<bool> cancelling{false};
std::atomic<bool> busy{false};
std::atomic<unsigned> lookSeq{0};
unsigned sinceOffer = 0;
bool offeredBefore = false;
bool shuttingDown = false;

bool Wanted() {
    return online::Config().discoverRadio && g.settings.radio && g.playing != kNone;
}

Preview* ById(TrackId id) {
    for (Preview& p : previews)
        if (p.id == id) return &p;
    return nullptr;
}

bool Referenced(const Preview& p) {
    if (p.id == kNone || g.playing == p.id) return true;
    for (TrackId id : g.queue)
        if (id == p.id) return true;
    return EngineHoldsTrack(p.id);
}

void RemoveTree(const std::wstring& dir) {
    if (dir.empty()) return;
    std::error_code ignored;
    std::filesystem::remove_all(ToUtf8(dir), ignored);
}

void StartLook() {
    if (worker.joinable() && !busy) worker.join();
    discovery::Snapshot owned;
    for (size_t i = 0; i < g_lib.Count(); ++i) {
        const Track& t = g_lib[(TrackId)i];
        if ((t.flags & (Track::kInLibrary | Track::kInbox)) == 0) continue;
        if (t.flags & (Track::kMissing | Track::kExcluded)) continue;
        owned.Add(std::string(g_lib.View(t.albumArtist ? t.albumArtist : t.artist)),
                  std::string(g_lib.View(t.album)), std::string(g_lib.View(t.title)));
    }
    std::vector<std::string> top;
    for (const radar::Artist& artist : radar::TopArtists(g_lib, 8)) top.push_back(artist.name);
    std::string seedArtist;
    if (g.playing != kNone && g_lib.Contains(g.playing)) {
        const Track& t = g_lib[g.playing];
        seedArtist = g_lib.View(t.albumArtist ? t.albumArtist : t.artist);
    }
    const std::wstring root = g.dataDir + L"/discover";
    const std::wstring dir = root + L"/" + std::to_wstring(g_lib.Revision()) + L"-" +
                             std::to_wstring(++lookSeq);
    const uint32_t seed = (uint32_t)UnixNow();
    auto cancel = std::make_shared<online::Cancel>();
    lookCancel = cancel;
    busy = true;
    offeredBefore = true;
    sinceOffer = 0;
    cancelling = false;
    worker = std::thread([owned = std::move(owned), top = std::move(top), seedArtist, root, dir, seed, cancel] {
        auto result = std::make_unique<LookResult>();
        result->dir = dir;
        discovery::Track pick;
        if (discovery::Find(seedArtist, top, owned, *cancel, pick, seed)) {
            WebTrack want;
            want.site = pick.site;
            want.id = pick.id;
            want.title = pick.title;
            want.artist = pick.artist;
            want.album = pick.album;
            want.trackNo = pick.trackNo;
            want.trackTotal = pick.trackTotal;
            want.durationSec = pick.durationSec;
            WebFetchRun run;
            run.libraryRoot = dir;
            run.dataDir = root;
            run.cancel = &cancelling;
            WebFetchTrackList list;
            list.title = "Discovery";
            list.tracks.push_back(want);
            list.playlist = false;
            run.importLists.push_back(std::move(list));
            std::vector<WebFetchItem> items;
            std::string error;
            if (RunWebFetch(run, items, error) && !items.empty() &&
                (items[0].state == WebState::Done || items[0].state == WebState::Exists) &&
                !items[0].file.empty()) {
                result->ok = true;
                result->track = items[0].track;
                result->file = items[0].file;
            }
        }
        if (!result->ok) RemoveTree(dir);
        if (!PostMessageW(nullptr, WM_APP_DISCOVER, 0, (LPARAM)result.get())) {
            if (result->ok) RemoveTree(dir);
            busy = false;
            return;
        }
        result.release();
    });
}

}  // namespace

void DiscoverMaybe() {
    if (!Wanted() || IsDiscoverPreview(g.playing)) return;
    ++sinceOffer;
    bool pending = busy;
    for (const Preview& p : previews) pending = pending || Referenced(p);
    if (pending || !discovery::ShouldDiscover(online::Config().discoverRadio, g.settings.radio, true,
                                               pending, offeredBefore, sinceOffer))
        return;
    if (g.settings.folders.empty()) return;
    StartLook();
}

void DiscoverRegisterPreview(TrackId id, const WebTrack& track, const std::wstring& file, const std::wstring& dir) {
    if (id == kNone) return;
    previews.push_back({id, track, file, dir, false});
}

bool DiscoverFindPreview(const std::string& site, const std::string& id, TrackId& trackOut) {
    for (const Preview& p : previews) {
        if (!p.kept && p.id != kNone && p.track.site == site && p.track.id == id &&
            GetFileAttributesW(p.file.c_str()) != INVALID_FILE_ATTRIBUTES) {
            trackOut = p.id;
            return true;
        }
    }
    return false;
}

bool IsDiscoverPreview(TrackId id) {
    if (id == kNone) return false;
    for (const Preview& p : previews)
        if (p.id == id && !p.kept) return true;
    return false;
}

bool DiscoverCanKeep() {
    for (const Preview& p : previews)
        if (!p.kept && p.id != kNone && Referenced(p)) return true;
    return false;
}

bool DiscoverKeepTrack(TrackId id, std::wstring* keptOut) {
    Preview* found = ById(id);
    if (!found || found->kept) return false;
    if (g.settings.folders.empty()) {
        ShowMessage(L"Add a library folder first, so the song has somewhere to stay.");
        return false;
    }
    std::wstring kept;
    std::string error;
    if (!WebFetchKeep(found->track, found->file, g.settings.folders.front(), g.dataDir, kept, error)) {
        ShowMessage(L"Couldn't keep the song: " + FromUtf8(error) + L".");
        return false;
    }
    if (g_lib.Contains(found->id)) {
        if (!g_lib.Rename(found->id, kept)) {
            ShowMessage(L"Couldn't add the song to the library. Its file is at " + kept + L".");
            return false;
        }
        Track& t = g_lib[found->id];
        t.flags |= Track::kInLibrary | Track::kLoaded;
        t.flags &= ~(Track::kMissing | Track::kFailed);
        t.added = UnixNow();
        g_lib.Touch();
        g.libraryDirty = true;
        RebuildRows();
    }
    found->kept = true;
    if (keptOut) *keptOut = kept;
    ShowMessage(L"Kept in your library: " + FromUtf8(found->track.title) + L".");
    g.redraw = true;
    MprisChanged();
    return true;
}

void DiscoverKeep() {
    Preview* found = nullptr;
    for (Preview& p : previews)
        if (!p.kept && p.id != kNone && Referenced(p) && (g.playing == p.id || !found)) found = &p;
    if (found) DiscoverKeepTrack(found->id);
}

void DiscoverTrackStarted() {
    Preview* p = ById(g.playing);
    if (!p) {
        DiscoverMaybe();
        return;
    }
    std::wstring title = FromUtf8(p->track.title);
    std::wstring artist = FromUtf8(p->track.artist);
    ShowMessage(L"Outside your library: " + (artist.empty() ? L"" : artist + L" \x2014 ") + title +
                L". Ctrl+Alt+K keeps it.");
}

void DiscoverTick() {
    for (size_t i = 0; i < previews.size();) {
        Preview& p = previews[i];
        if (Referenced(p)) {
            ++i;
            continue;
        }
        std::error_code ignored;
        std::filesystem::remove(ToUtf8(p.file), ignored);
        RemoveTree(p.dir);
        previews.erase(previews.begin() + (ptrdiff_t)i);
    }
}

void DiscoverShutdown() {
    shuttingDown = true;
    cancelling = true;
    if (lookCancel) lookCancel->Stop();
    if (worker.joinable()) worker.join();
    busy = false;
    lookCancel.reset();
    for (Preview& p : previews) {
        std::error_code ignored;
        std::filesystem::remove(ToUtf8(p.file), ignored);
        RemoveTree(p.dir);
    }
    previews.clear();
    RemoveTree(g.dataDir + L"/preview");
}

void OnDiscoverMessage(WPARAM, LPARAM lp) {
    busy = false;
    std::unique_ptr<LookResult> result(reinterpret_cast<LookResult*>(lp));
    if (!result) return;
    if (shuttingDown || !result->ok || !Wanted()) {
        RemoveTree(result->dir);
        return;
    }
    const TrackId id = g_lib.Add(result->file);
    if (id == kInvalidTrack) {
        RemoveTree(result->dir);
        return;
    }
    Track& t = g_lib[id];
    if (result->track.ext.empty()) result->track.ext = ToUtf8(ExtOf(result->file));
    t.title = g_lib.Intern(result->track.title);
    t.artist = g_lib.Intern(result->track.artist);
    t.album = g_lib.Intern(result->track.album);
    t.trackNo = (uint16_t)std::min(result->track.trackNo, 999u);
    t.trackTotal = (uint16_t)std::min(result->track.trackTotal, 999u);
    t.durationMs = result->track.durationSec * 1000u;
    t.added = UnixNow();
    t.flags |= Track::kLoaded;
    previews.push_back({id, result->track, result->file, result->dir, false});
    const size_t picks = std::min<size_t>(g.queuePicks, g.queue.size());
    g.queue.insert(g.queue.begin() + (ptrdiff_t)(g.queue.size() - picks), id);
    RefreshNext();
    g.redraw = true;
    std::wstring title = FromUtf8(result->track.title);
    std::wstring artist = FromUtf8(result->track.artist);
    ShowMessage(L"Found outside your library: " + (artist.empty() ? L"" : artist + L" \x2014 ") + title +
                L". Ctrl+Alt+K keeps it.");
}

}  // namespace lx
