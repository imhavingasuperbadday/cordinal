// Worker-thread jobs: reading tags for newly seen files, and the ReplayGain / loudness scanner.
// Workers own only a copied path list; they never touch the live Library or StringPool. Results
// come back to the UI thread as a posted message (WM_APP_META, WM_APP_RG) whose payload the UI
// takes ownership of, so compaction (app_memory.cpp) can safely run while a worker is idle.
#include "app.h"
#include "peaks.h"
#include "background_cpu.h"
#include "migration.h"

#include <cstdio>
#include <memory>
#include <set>
#include <tuple>

namespace app {

namespace {
// Called only by the UI thread. Workers never read a mutating Library/StringPool, and
// cannot outpace the UI by posting a library's worth of metadata into the message queue.
void DispatchTagBatch() {
    std::lock_guard<std::mutex> lock(g.tagMx);
    if (g.tagQuit || g.tagBatchInFlight || g.tagQueue.empty()) return;
    g.tagBatch.reserve(64);
    while (!g.tagQueue.empty() && g.tagBatch.size() < 64) {
        TrackId id = g.tagQueue.front();
        g.tagQueue.pop_front();
        if (id < g_lib.Count()) g.tagBatch.push_back(g_lib.SnapshotPath(id));
    }
    if (g.tagQueue.empty()) std::deque<TrackId>().swap(g.tagQueue);
    g.tagBatchInFlight = !g.tagBatch.empty();
    for (const auto& item : g.tagBatch) g.tagReading.insert(item.id);
    g.tagCv.notify_one();
}
}  // namespace

// UI thread. Queued reads coalesce; a file changed during its current read gets one fresh read
// after that batch returns, so an older snapshot cannot win over a completed tag edit.
void QueueTags(const std::vector<TrackId>& ids) {
    if (ids.empty() || g.closing) return;
    {
        std::lock_guard<std::mutex> lock(g.tagMx);
        for (TrackId id : ids) {
            if (!g_lib.Contains(id)) continue;
            if (g.tagReading.contains(id)) {
                if (std::find(g.tagAgain.begin(), g.tagAgain.end(), id) == g.tagAgain.end()) g.tagAgain.push_back(id);
            } else if (g.tagPending.insert(id).second) g.tagQueue.push_back(id);
        }
    }
    DispatchTagBatch();
}

// UI thread: takes ownership of a batch posted by TagWorker. Results whose path changed since the
// request was made are dropped.
void AcceptTagBatch(std::vector<TagResult>* results) {
    std::unique_ptr<std::vector<TagResult>> owned(results);
    if (!owned) return;
    if (!owned->empty()) g.dirty = true;  // tags or the retry flag changed, even when the read failed
    for (auto& r : *results) {
        g.tagPending.erase(r.id);
        if (!g_lib.Contains(r.id) || g_lib.Path(r.id) != r.path) continue;
        if (std::find(g.tagAgain.begin(), g.tagAgain.end(), r.id) != g.tagAgain.end()) continue;
        if (r.success) {
            Track& t = g_lib[r.id];
            const bool artChanged = r.haveStats && (t.fileTime != r.fileTime || t.fileSize != r.fileSize);
            if (r.haveStats) {
                t.fileSize = r.fileSize;
                t.fileTime = r.fileTime;
            }
            g_lib.ApplyTags(r.id, r.tags);
            if (artChanged && g.artFor == r.id) LoadArtFor(r.id, true);
        }
        else g_lib[r.id].flags = (uint16_t)((g_lib[r.id].flags & ~Track::kLoaded) | Track::kTagFailed);
    }
    ClearCellCache();
    g.compactPending = true;
    {
        std::lock_guard<std::mutex> lock(g.tagMx);
        g.tagBatchInFlight = false;
    }
    std::unordered_set<TrackId>().swap(g.tagReading);
    auto again = std::move(g.tagAgain);
    owned.reset();  // release this batch before the next one can start decoding
    QueueTags(again);
    DispatchTagBatch();
}

// Worker loop. Waits for a batch prepared by DispatchTagBatch, decodes it, and posts the results;
// a failed post means the window is gone and the thread exits.
void TagWorker(HWND hwnd) {
    for (;;) {
        std::vector<TrackPath> batch;
        {
            std::unique_lock<std::mutex> lock(g.tagMx);
            g.tagCv.wait(lock, [] { return g.tagQuit || !g.tagBatch.empty(); });
            if (g.tagQuit) break;
            batch.swap(g.tagBatch);
        }
        auto* results = new std::vector<TagResult>();
        results->reserve(batch.size());
        bool quitting = false;
        for (const auto& source : batch) {
            {
                std::lock_guard<std::mutex> lock(g.tagMx);
                quitting = g.tagQuit;  // shutdown: don't wait out the rest of a slow network folder
            }
            if (quitting) break;
            TagResult r{source.id, {}, false, source.Path()};
            // Capture identity before reading: a later replacement must still look changed to
            // the scanner. Virtual subtrack paths have no direct stat and retain their scan stamp.
            WIN32_FILE_ATTRIBUTE_DATA attrs{};
            r.haveStats = GetFileAttributesExW(r.path.c_str(), GetFileExInfoStandard, &attrs) != FALSE;
            if (r.haveStats) {
                r.fileSize = (uint64_t)attrs.nFileSizeHigh << 32 | attrs.nFileSizeLow;
                r.fileTime = (uint64_t)attrs.ftLastWriteTime.dwHighDateTime << 32 | attrs.ftLastWriteTime.dwLowDateTime;
            }
            r.success = ReadTags(r.path, r.tags);
            results->push_back(std::move(r));
        }
        if (quitting) {
            delete results;
            break;
        }
        if (!PostMessageW(hwnd, WM_APP_META, 0, (LPARAM)results)) {
            delete results;
            break;
        }
    }
}

// ReplayGain: the levelling model, album gains and measuring live in replaygain.cpp (shared with
// the Linux build). This side owns the settings, the scan thread and its messages, and the
// background sweep (MaybeAutoMeasure).
namespace {
ReplayGainOptions Options() {
    return {g.rgOn, g.rgPreamp, g.rgPreampNoInfo, g.rgKeep, g.rgHeadphoneHeadroom,
            g.rgMode, g.shuffle == kShuffleTracks};
}

// Whether the EQ settings or the output rate changed since the gains were last worked out.
EqSettings g_rgEqSettings;
unsigned g_rgEqRate = 0;
bool UpdateEqBudget() {
    const unsigned rate = g.engine.Rate();
    if (rate == g_rgEqRate && g.eq == g_rgEqSettings) return false;
    g_rgEqRate = rate;
    g_rgEqSettings = g.eq;
    return true;
}
double EqPeak() {
    UpdateEqBudget();
    return replaygain::EqPeakDb(g.eq, g.engine.Rate());
}
}  // namespace

void ReplayGainEqTick() {
    if (UpdateEqBudget() && g.rgOn) RefreshGains();
}

float RgGainFor(TrackId id) { return replaygain::Gain(g_lib[id], Options(), g.rgOn ? EqPeak() : 0); }

float RgGainForEstimate(TrackId id, double lufs) {
    return replaygain::GainForEstimate(g_lib[id], lufs, Options(), g.rgOn ? EqPeak() : 0);
}

void ForgetAlbumDynamics(bool keepMeasurements) {
    replaygain::ForgetAlbumDynamics(keepMeasurements);
    g.rgAlbumsRevision = 0;
}

// Applies the current gains to every live token. `includePlaying` is false for background
// measurements: the track being heard keeps its level until it ends instead of jumping mid-song.
void RefreshGains(bool includePlaying) {
    g.engine.SetLimiter(g.rgOn, g.rgHeadphoneHeadroom);
    for (auto& [token, info] : g.tokens)
        if (includePlaying || token != g.playingToken)
            g.engine.SetGain(token, info.estimated ? RgGainForEstimate(info.id, info.estimate) : RgGainFor(info.id));
    UpdateStatus();
}

namespace {
constexpr size_t kSweepTracks = 300;  // per background run, so compaction and new priorities get a turn

// Stops a running scan and waits for its thread, then applies the results it already posted
// right away, so nothing from it can land after whatever the caller does next.
void StopRgWorker() {
    if (!g.rgRunning) return;
    g.rgCancel = true;
    if (auto decoder = g.rgDecoder.load()) decoder->Abort();
    if (g.rgWorker.joinable()) g.rgWorker.join();
    g.rgRunning = false;
    g.rgCancel = false;
    std::unordered_set<TrackId>().swap(g.rgRunIds);
    MSG posted;
    while (PeekMessageW(&posted, g.hwnd, WM_APP_RG, WM_APP_RG, PM_REMOVE)) FinishReplayGain(posted.wParam, posted.lParam);
}

// Starts the worker on exactly `ids`, in that order.
void StartRgScan(const std::vector<TrackId>& ids, bool background) {
    StopRgWorker();
    if (g.rgWorker.joinable()) g.rgWorker.join();
    std::vector<TrackPath> items;
    std::vector<std::pair<uint64_t, uint64_t>> sizeAndTime;
    std::unordered_set<TrackId> seen;
    for (TrackId id : ids) {
        if (id >= g_lib.Count() || !seen.insert(id).second) continue;
        if (!background && (g_lib[id].flags & Track::kRgScanFailed)) {
            g_lib[id].flags &= (uint16_t)~Track::kRgScanFailed;
            g.dirty = true;
        }
        items.push_back(g_lib.SnapshotPath(id));
        sizeAndTime.emplace_back(g_lib[id].fileSize, g_lib[id].fileTime);
    }
    if (items.empty()) return;
    g.rgRunIds = std::move(seen);
    g.rgCancel = false;
    g.rgRunning = true;
    g.rgAuto = background;
    g.rgDone = 0;
    g.rgOk = g.rgFailed = 0;
    g.rgTotal = (int)items.size();
    g.rgStart = GetTickCount64();
    g.rgWorker = std::thread(RgWorker, g.hwnd, std::move(items), std::move(sizeAndTime), ++g.rgGen, background);
    UpdateStatus();
}

// Whether other work that touches the same files or track ids is under way.
bool BackgroundBusy() {
    return g.closing || g.importRunning || !g.tagPending.empty() || g.scanner.Busy() || ScanInProgress() ||
           g.compactPending || TagWriteBusy() || OrganizeBusy();
}
}  // namespace

void UpdateAlbumGains(const std::vector<TrackId>& ids) {
    if (replaygain::UpdateAlbumGains(ids)) g.dirty = true;
}

// Saved album measurements, once per library change (see replaygain::RefreshAlbums).
void RefreshAlbums() {
    auto r = replaygain::RefreshAlbums(g.rgAlbumsRevision);
    if (r.dirty) g.dirty = true;
    if (r.gains) RefreshGains(false);
}

// Worker: measures each track's loudness and true peak, in the order given. Results are posted in
// small batches, so a long scan shows up in the library as it goes and a cancelled one keeps
// everything it finished. Background runs use background priority (CPU and disk), so playback
// and the UI never wait on them.
void RgWorker(HWND hwnd, std::vector<TrackPath> items, std::vector<std::pair<uint64_t, uint64_t>> sizeAndTime,
              unsigned generation, bool background) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (background) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    BackgroundCpuBudget cpuBudget;
    auto batch = std::make_unique<RgBatch>(RgBatch{generation, background, false, {}});
    ULONGLONG posted = GetTickCount64();
    auto post = [&](bool final) {
        batch->final = final;
        if (PostMessageW(hwnd, WM_APP_RG, 2, (LPARAM)batch.get())) batch.release();
        batch = std::make_unique<RgBatch>(RgBatch{generation, background, false, {}});
        posted = GetTickCount64();
    };
    for (size_t k = 0; k < items.size() && !g.rgCancel; ++k) {
        g.rgCurrent = items[k].id;
        RgResult result = replaygain::Measure(items[k], sizeAndTime[k].first, sizeAndTime[k].second, g.rgCancel, g.rgDecoder,
                                              background ? &cpuBudget : nullptr);
        g.rgCurrent = kNone;
        if (g.rgCancel) break;  // cut short: neither measured nor failed
        batch->results.push_back(result);
        ++g.rgDone;
        if (batch->results.size() >= 16 || GetTickCount64() - posted >= 1500) post(false);
        else PostMessageW(hwnd, WM_APP_RG, 1, 0);  // progress
    }
    post(true);
    CoUninitialize();
}

// Manual scan: exactly the given tracks, one or many. Albums they complete get their album gain.
// A background measurement in progress gives way.
void ScanReplayGain(const std::vector<TrackId>& ids) {
    if ((g.rgRunning && !g.rgAuto) || ids.empty()) return;
    StartRgScan(ids, false);
}

std::vector<TrackId> AutoMeasureTracks(const std::vector<TrackId>& upcoming, size_t limit) {
    return replaygain::AutoMeasureTracks(upcoming, limit);
}

// Background measuring, called on every UI tick and when a track starts (`priority`). The
// upcoming tracks go first, then the rest of their albums (so those albums get their album gain
// soon), then the rest of the library album by album, a few hundred tracks per run. It keeps out
// of the way of imports, tag reading, compaction and manual scans, and a running sweep gives way
// when a track starts whose upcoming tracks it doesn't cover.
void MaybeAutoMeasure(bool priority) {
    if (!BackgroundBusy()) RefreshAlbums();  // saved album measurements, once per library change
    if (!g.rgOn || !g.rgAutoScan || (g.rgRunning && !g.rgAuto) || BackgroundBusy()) return;
    if (g.rgRunning && !priority) return;
    const ULONGLONG now = GetTickCount64();
    if (!g.rgNextAuto) g.rgNextAuto = now + 8000;  // let startup settle first
    std::vector<TrackId> upcoming = UpcomingTracks();
    if (g.playing != kNone) upcoming.push_back(g.playing);
    bool covered = true;
    bool needed = false;
    for (TrackId id : upcoming) {
        if (id >= g_lib.Count() || !replaygain::NeedsMeasuring(id)) continue;
        covered &= g.rgRunIds.count(id) != 0;
        needed = true;
    }
    if (g.rgRunning && covered) return;
    if (!needed && now < g.rgNextAuto) return;
    g.rgNextAuto = now + 15000;  // when nothing is found: look again later
    auto ids = AutoMeasureTracks(upcoming, kSweepTracks);
    if (ids.empty()) return;
    StartRgScan(ids, true);
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
    StopRgWorker();  // a background run must not write back what is being cleared
    const size_t n = replaygain::Clear(ids);
    if (!n) {
        ShowMessage(L"No ReplayGain information to clear.");
        return;
    }
    g.dirty = true;
    ClearCellCache();
    g.rgNextAuto = GetTickCount64() + 60000;  // a minute before the sweep measures them again
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
        ShowMessage(L"Nothing to write.");
        return;
    }
    SubmitTagWrites(items, true, true);  // atomic per file, journaled; playing files are deferred
}

void WriteStatisticsTags(const std::vector<TrackId>& ids) {
    std::unordered_set<TrackId> seen;
    std::vector<std::pair<TrackId, TagEdit>> items;
    for (TrackId id : ids)
        if (id < g_lib.Count() && seen.insert(id).second && CanWriteTags(g_lib.Path(id)))
            if (TagEdit e = migration::StatisticsTags(g_lib[id]); !e.Empty()) items.push_back({id, std::move(e)});
    if (items.empty()) {
        ShowMessage(ids.empty() ? L"Select the tracks to write ratings and play counts for."
                                : L"None of the selected tracks has a rating or play count to write.");
        return;
    }
    SubmitTagWrites(items, true, true);
}

// UI thread: the WM_APP_RG handler. wp==1 is a progress tick; wp==2 carries an RgBatch (some
// tracks' results, the last batch marked final), which this function takes ownership of and
// frees. Batches of an earlier run (one that gave way) are still applied; only the current run's
// final batch ends the scan.
void FinishReplayGain(WPARAM wp, LPARAM lp) {
    if (wp == 1) {  // progress
        UpdateStatus();
        return;
    }
    std::unique_ptr<RgBatch> batch(reinterpret_cast<RgBatch*>(lp));
    std::vector<TrackId> measured;
    for (auto& r : batch->results) {
        if (r.id >= g_lib.Count()) continue;
        replaygain::Apply(r);
        measured.push_back(r.id);  // a failed track no longer holds the rest of its album back
        ++(r.ok ? g.rgOk : g.rgFailed);
    }
    if (!measured.empty()) {
        UpdateAlbumGains(measured);
        g.dirty = true;
        RefreshGains(!batch->background);
    }
    if (!batch->final || batch->generation != g.rgGen || !g.rgRunning) return;
    const bool cancelled = g.rgCancel.load();
    g.rgRunning = false;
    if (g.rgWorker.joinable()) g.rgWorker.join();
    g.rgCancel = false;
    std::unordered_set<TrackId>().swap(g.rgRunIds);
    g.rgNextAuto = GetTickCount64() + 1000;  // more may be waiting: continue the sweep shortly
    UpdateStatus();
    if (batch->background) return;
    std::wstring done = std::to_wstring(g.rgOk) + (g.rgOk == 1 ? L" track" : L" tracks");
    if (cancelled)
        ShowMessage(L"ReplayGain scan cancelled after " + done + L".");
    else if (g.rgFailed)
        ShowMessage(L"ReplayGain: measured " + done + L"; " + std::to_wstring(g.rgFailed) + L" couldn't be read.");
    else
        ShowMessage(L"ReplayGain: measured " + done + L".");
}

}  // namespace app
