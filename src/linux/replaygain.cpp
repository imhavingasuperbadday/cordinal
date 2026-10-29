// ReplayGain on Linux: the scan thread, the background sweep and the Playback > ReplayGain commands.
// The model, album gains and measuring are the Windows build's (src/replaygain.cpp); this file
// runs them the way app_workers.cpp does there. The worker owns only copied paths and posts its
// results back as WM_APP_RG, so it never touches the live library.
#include "app.h"

#include "background_cpu.h"
#include "decoder.h"

#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <memory>
#include <unordered_set>

namespace lx {

namespace {

constexpr size_t kSweepTracks = 300;  // per background run, so new priorities get a turn

// Measures each track in the order given, posting results in small batches so a long scan shows
// up as it goes and a cancelled one keeps what it finished. A background run lowers its own
// priority and paces itself, so playback and the window never wait on it.
void Worker(std::vector<TrackPath> items, std::vector<std::pair<uint64_t, uint64_t>> sizeAndTime, unsigned generation,
            bool background) {
    if (background) setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 15);
    BackgroundCpuBudget budget;
    auto batch = std::make_unique<RgBatch>(RgBatch{generation, background, false, {}});
    uint64_t posted = SDL_GetTicks();
    auto post = [&](bool final) {
        batch->final = final;
        if (PostMessageW(nullptr, WM_APP_RG, 2, (LPARAM)batch.get())) batch.release();
        batch = std::make_unique<RgBatch>(RgBatch{generation, background, false, {}});
        posted = SDL_GetTicks();
    };
    for (size_t k = 0; k < items.size() && !g.rgCancel; ++k) {
        RgResult result = replaygain::Measure(items[k], sizeAndTime[k].first, sizeAndTime[k].second, g.rgCancel, g.rgDecoder,
                                              background ? &budget : nullptr);
        if (g.rgCancel) break;  // cut short: neither measured nor failed
        batch->results.push_back(result);
        ++g.rgDone;
        if (batch->results.size() >= 16 || SDL_GetTicks() - posted >= 1500) post(false);
        else PostMessageW(nullptr, WM_APP_RG, 1, 0);  // progress
    }
    post(true);
}

// Whether other work that touches the same files is under way.
bool BackgroundBusy() {
    if (g.scanning || TagWriteBusy() || CordExportBusy()) return true;
    std::lock_guard lock(g.tagMx);
    return !g.tagQueue.empty() || g.tagBusy;
}

void Start(const std::vector<TrackId>& ids, bool background) {
    StopReplayGain();
    std::vector<TrackPath> items;
    std::vector<std::pair<uint64_t, uint64_t>> sizeAndTime;
    std::unordered_set<TrackId> seen;
    g.rgRunIds.clear();
    for (TrackId id : ids) {
        if (id >= g_lib.Count() || !seen.insert(id).second) continue;
        // Asking for a track that failed before tries it again.
        if (!background) g_lib[id].flags &= (uint16_t)~Track::kRgScanFailed;
        items.push_back(g_lib.SnapshotPath(id));
        sizeAndTime.emplace_back(g_lib[id].fileSize, g_lib[id].fileTime);
        g.rgRunIds.push_back(id);
    }
    if (items.empty()) return;
    g.rgCancel = false;
    g.rgRunning = true;
    g.rgAuto = background;
    g.rgDone = 0;
    g.rgOk = g.rgFailed = 0;
    g.rgTotal = (int)items.size();
    g.rgWorker = std::thread(Worker, std::move(items), std::move(sizeAndTime), ++g.rgGen, background);
    g.redraw = true;
}

}  // namespace

// Stops a running scan and waits for its thread. Results it already posted are still applied
// when they are dispatched; only a later run's final batch ends that run.
void StopReplayGain() {
    if (g.rgRunning) {
        g.rgCancel = true;
        if (auto decoder = g.rgDecoder.load()) decoder->Abort();
    }
    if (g.rgWorker.joinable()) g.rgWorker.join();
    g.rgRunning = false;
    g.rgCancel = false;
    g.rgRunIds.clear();
}

// Manual scan: exactly the given tracks. Albums they complete get their album gain.
void ScanReplayGain(const std::vector<TrackId>& ids) {
    if (ids.empty()) {
        ShowMessage(L"Select the tracks to scan.");
        return;
    }
    if (g.rgRunning && !g.rgAuto) {
        ShowMessage(L"A ReplayGain scan is already running.");
        return;
    }
    Start(ids, false);
}

// Called a few times a second and when a track starts (`priority`): the upcoming tracks go first,
// then the rest of their albums, then the library album by album. It keeps out of the way of
// folder scans, tag reading and writing, and manual scans, and a running sweep gives way when a
// track starts whose upcoming tracks it doesn't cover.
void MaybeAutoMeasure(bool priority) {
    if (!BackgroundBusy()) {
        auto r = replaygain::RefreshAlbums(g.rgAlbumsRevision);  // saved album gains, once per library change
        if (r.dirty) g.libraryDirty = true;
        if (r.gains) RefreshGains(false);
    }
    if (!g.settings.rg.on || !g.settings.rgAutoScan || (g.rgRunning && !g.rgAuto) || BackgroundBusy()) return;
    if (g.rgRunning && !priority) return;
    uint64_t now = SDL_GetTicks();
    if (!g.rgNextAuto) g.rgNextAuto = now + 8000;  // let startup settle first
    std::vector<TrackId> upcoming = UpcomingTracks();
    if (g.playing != kNone) upcoming.push_back(g.playing);
    bool covered = true, needed = false;
    for (TrackId id : upcoming) {
        if (id >= g_lib.Count() || !replaygain::NeedsMeasuring(id)) continue;
        covered &= std::find(g.rgRunIds.begin(), g.rgRunIds.end(), id) != g.rgRunIds.end();
        needed = true;
    }
    if (g.rgRunning && covered) return;
    if (!needed && now < g.rgNextAuto) return;
    g.rgNextAuto = now + 15000;  // when nothing is found: look again later
    auto ids = replaygain::AutoMeasureTracks(upcoming, kSweepTracks);
    if (!ids.empty()) Start(ids, true);
}

void CancelReplayGain() {
    if (!g.rgRunning || g.rgAuto) return;
    g.rgCancel = true;
    if (auto decoder = g.rgDecoder.load()) decoder->Abort();
    ShowMessage(L"Cancelling the ReplayGain scan...");
}

void ClearReplayGain(const std::vector<TrackId>& ids) {
    if (g.rgRunning && !g.rgAuto) {
        ShowMessage(L"Wait for the ReplayGain scan to finish.");
        return;
    }
    if (ids.empty()) {
        ShowMessage(L"Select the tracks whose ReplayGain you want to clear.");
        return;
    }
    StopReplayGain();  // a background run must not write back what is being cleared
    size_t n = replaygain::Clear(ids);
    if (!n) {
        ShowMessage(L"No ReplayGain information to clear.");
        return;
    }
    g.libraryDirty = true;
    g.rgNextAuto = SDL_GetTicks() + 60000;  // a minute before the sweep measures them again
    RefreshGains();
    ShowMessage(L"Cleared ReplayGain for " + std::to_wstring(n) + L" track(s).");
}

void WriteReplayGainTags(const std::vector<TrackId>& ids) {
    if (g.rgRunning && !g.rgAuto) {
        ShowMessage(L"Wait for the ReplayGain scan to finish.");
        return;
    }
    std::unordered_set<TrackId> seen;
    std::vector<std::pair<TrackId, TagEdit>> items;
    for (TrackId id : ids)
        if (id < g_lib.Count() && seen.insert(id).second) items.push_back({id, replaygain::TagsFor(g_lib[id])});
    if (items.empty()) {
        ShowMessage(L"Select the tracks to write ReplayGain tags for.");
        return;
    }
    SubmitTagWrites(items, true);
}

void SetReplayGain(bool on) {
    g.settings.rg.on = on;
    SaveSettings();
    if (!on && g.rgRunning && g.rgAuto) StopReplayGain();
    RefreshGains();
    ShowMessage(on ? L"ReplayGain on." : L"ReplayGain off.");
}

// The WM_APP_RG handler: wp 1 is a progress tick; wp 2 carries an RgBatch, which this takes
// ownership of. Batches of an earlier run (one that gave way) are still applied; only the current
// run's final batch ends the scan.
void FinishReplayGain(WPARAM wp, LPARAM lp) {
    g.redraw = true;
    if (wp == 1) return;
    std::unique_ptr<RgBatch> batch(reinterpret_cast<RgBatch*>(lp));
    std::vector<TrackId> measured;
    for (auto& r : batch->results) {
        if (r.id >= g_lib.Count()) continue;
        replaygain::Apply(r);
        measured.push_back(r.id);  // a failed track no longer holds the rest of its album back
        ++(r.ok ? g.rgOk : g.rgFailed);
    }
    if (!measured.empty()) {
        replaygain::UpdateAlbumGains(measured);
        g.libraryDirty = true;
        RefreshGains(!batch->background);
    }
    if (!batch->final || batch->generation != g.rgGen || !g.rgRunning) return;
    bool cancelled = g.rgCancel.load();
    if (g.rgWorker.joinable()) g.rgWorker.join();
    g.rgRunning = false;
    g.rgCancel = false;
    g.rgRunIds.clear();
    g.rgNextAuto = SDL_GetTicks() + 1000;  // more may be waiting: continue the sweep shortly
    if (batch->background) return;
    std::wstring done = std::to_wstring(g.rgOk) + (g.rgOk == 1 ? L" track" : L" tracks");
    if (cancelled) ShowMessage(L"ReplayGain scan cancelled after " + done + L".");
    else if (g.rgFailed) ShowMessage(L"ReplayGain: measured " + done + L"; " + std::to_wstring(g.rgFailed) + L" couldn't be read.");
    else ShowMessage(L"ReplayGain: measured " + done + L".");
}

std::wstring ReplayGainStatus() {
    if (!g.rgRunning) return {};
    std::wstring progress = std::to_wstring(g.rgDone.load()) + L"/" + std::to_wstring(g.rgTotal);
    return g.rgAuto ? L"Measuring loudness " + progress : L"Scanning ReplayGain " + progress;
}

}  // namespace lx
