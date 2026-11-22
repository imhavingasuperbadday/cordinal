// Discover outside my library (app_discover.h, discovery.h). The look runs on a worker: it copies
// what the library owns, asks MusicBrainz for a related artist with an unowned release, and hands
// the chosen song to the web-save queue, which matches it on YouTube and downloads it tagged. The
// preview lands in the data folder, never the library, and is deleted when nothing references it.
// Turning radio or the preference off stops new looks; a queued preview still plays like any song.
#include "app.h"

#include "discovery.h"
#include "release_radar.h"
#include "webfetch.h"

#include <atomic>
#include <memory>
#include <thread>

namespace app {
namespace {

struct Preview {
    TrackId id = kNone;
    WebTrack track;
    std::wstring file;
    std::wstring dir;
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
    return online::Config().discoverRadio && g.radio && g.playing != kNone;
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
    for (const auto& [token, info] : g.tokens)
        if (info.id == p.id || info.anchor == p.id) return true;
    return false;
}

void RemoveTree(const std::wstring& dir) { DiscoverRemoveTree(dir); }

void StartLook() {
    if (worker.joinable() && !busy) worker.join();
    discovery::Snapshot owned;
    for (size_t i = 0; i < g_lib.Count(); ++i) {
        const TrackId id = (TrackId)i;
        const Track& t = g_lib[id];
        if ((t.flags & (Track::kInLibrary | Track::kInbox)) == 0) continue;
        if (t.flags & (Track::kMissing | Track::kExcluded)) continue;
        const std::string artist(g_lib.View(t.albumArtist ? t.albumArtist : t.artist));
        owned.Add(artist, std::string(g_lib.View(t.album)), std::string(g_lib.View(t.title)));
    }
    std::vector<std::string> top;
    for (const radar::Artist& artist : radar::TopArtists(g_lib, 8)) top.push_back(artist.name);
    std::string seedArtist;
    if (g.playing != kNone && g_lib.Contains(g.playing)) {
        const Track& t = g_lib[g.playing];
        seedArtist = g_lib.View(t.albumArtist ? t.albumArtist : t.artist);
    }
    const std::wstring root = g.dataDir + L"\\discover";
    const std::wstring dir = root + L"\\" + std::to_wstring(GetTickCount64()) + L"-" +
                             std::to_wstring(++lookSeq);
    const uint32_t seed = (uint32_t)GetTickCount64();
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
        if (!PostMessageW(g.hwnd, WM_APP_DISCOVER, 0, (LPARAM)result.get())) {
            if (result->ok) RemoveTree(dir);
            busy = false;
            return;
        }
        result.release();
    });
}

}  // namespace

void DiscoverMaybe() {
    if (!Wanted() || DiscoverIsPreview(g.playing)) return;
    ++sinceOffer;
    bool pending = busy;
    for (const Preview& p : previews) pending = pending || Referenced(p);
    if (pending || !discovery::ShouldDiscover(online::Config().discoverRadio, g.radio, true, pending,
                                               offeredBefore, sinceOffer))
        return;
    if (g.libraryFolders.empty()) return;
    StartLook();
}

void DiscoverRegisterPreview(TrackId id, const WebTrack& track, const std::wstring& file, const std::wstring& dir) {
    if (id == kNone) return;
    previews.push_back({id, track, file, dir, false});
}

void DiscoverRemoveTree(const std::wstring& dir) {
    if (dir.empty()) return;
    WIN32_FIND_DATAW entry{};
    HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &entry);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = entry.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring path = dir + L"\\" + name;
            if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) DiscoverRemoveTree(path);
            else DeleteFileW(path.c_str());
        } while (FindNextFileW(find, &entry));
        FindClose(find);
    }
    RemoveDirectoryW(dir.c_str());
}

TrackId DiscoverAddTrack(const std::wstring& file, WebTrack& track, bool permanent, const std::wstring& dir,
                         const std::string& albumArtist, const std::string& date) {
    const TrackId id = g_lib.Add(file);
    if (id == kInvalidTrack) {
        if (!dir.empty()) DiscoverRemoveTree(dir);
        return kInvalidTrack;
    }
    Track& t = g_lib[id];
    if (track.ext.empty()) track.ext = ToUtf8(ExtOf(file));
    t.title = g_lib.Intern(track.title);
    t.artist = g_lib.Intern(track.artist);
    t.album = g_lib.Intern(track.album);
    if (!albumArtist.empty()) t.albumArtist = g_lib.Intern(albumArtist);
    if (!date.empty()) t.date = g_lib.Intern(date);
    t.trackNo = (uint16_t)std::min(track.trackNo, 999u);
    t.trackTotal = (uint16_t)std::min(track.trackTotal, 999u);
    t.disc = (uint8_t)std::min(track.discNo, 99u);
    t.discTotal = (uint8_t)std::min(track.discTotal, 99u);
    t.durationMs = track.durationSec * 1000u;
    t.added = UnixNow();
    t.flags |= Track::kLoaded;
    if (permanent) {
        t.flags |= Track::kInLibrary;
        t.flags &= ~(Track::kMissing | Track::kFailed);
        QueueTags({id});
        g_lib.Touch();
        g.dirty = true;
        g.statsDirty = true;
        RefreshViewKeepingSelection();
    } else {
        DiscoverRegisterPreview(id, track, file, dir);
    }
    return id;
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

bool DiscoverIsPreview(TrackId id) {
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
    if (g.libraryFolders.empty()) {
        ShowMessage(L"Add a library folder first, so the song has somewhere to stay.");
        return false;
    }
    std::wstring kept;
    std::string error;
    if (!WebFetchKeep(found->track, found->file, g.libraryFolders.front(), g.dataDir, kept, error)) {
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
        g.dirty = true;
        g.statsDirty = true;
        RefreshViewKeepingSelection();
    }
    found->kept = true;
    if (keptOut) *keptOut = kept;
    ShowMessage(L"Kept in your library: " + FromUtf8(found->track.title) + L".");
    InvalidateRect(g.list, nullptr, FALSE);
    LibViewInvalidate();
    UpdateNowPlaying();
    UpdateStatus();
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
        DeleteFileW(p.file.c_str());
        RemoveTree(p.dir);
        previews.erase(previews.begin() + (ptrdiff_t)i);
        g.compactPending = true;  // the preview's track record has nothing left to hold it
    }
}

void DiscoverShutdown() {
    shuttingDown = true;
    cancelling = true;
    if (lookCancel) lookCancel->Stop();
    if (worker.joinable()) worker.join();
    busy = false;
    lookCancel.reset();
    MSG pending{};
    while (PeekMessageW(&pending, g.hwnd, WM_APP_DISCOVER, WM_APP_DISCOVER, PM_REMOVE)) {
        std::unique_ptr<LookResult> result(reinterpret_cast<LookResult*>(pending.lParam));
        if (result) RemoveTree(result->dir);
    }
    for (Preview& p : previews) {
        DeleteFileW(p.file.c_str());
        RemoveTree(p.dir);
    }
    previews.clear();
    RemoveTree(g.dataDir + L"\\preview");
}

void DiscoverRemap(const std::vector<TrackId>& mapping) {
    for (Preview& p : previews)
        p.id = p.id < mapping.size() ? mapping[p.id] : kNone;
    std::erase_if(previews, [](const Preview& p) { return p.id == kNone; });
}

void OnDiscoverMessage(WPARAM, LPARAM lp) {
    busy = false;
    std::unique_ptr<LookResult> result(reinterpret_cast<LookResult*>(lp));
    if (!result) return;
    if (shuttingDown || !result->ok || !Wanted()) {
        RemoveTree(result->dir);
        return;
    }
    const TrackId id = DiscoverAddTrack(result->file, result->track, false, result->dir);
    if (id == kInvalidTrack) return;
    // Ahead of Choose by taste's picks, behind anything the listener queued by hand.
    const size_t picks = std::min<size_t>(g.queuePicks, g.queue.size());
    g.queue.insert(g.queue.begin() + (ptrdiff_t)(g.queue.size() - picks), id);
    RefreshNext();
    InvalidateRect(g.list, nullptr, FALSE);
    UpdateStatus();
    std::wstring title = FromUtf8(result->track.title);
    std::wstring artist = FromUtf8(result->track.artist);
    ShowMessage(L"Found outside your library: " + (artist.empty() ? L"" : artist + L" \x2014 ") + title +
                L". Ctrl+Alt+K keeps it.");
}

}  // namespace app
