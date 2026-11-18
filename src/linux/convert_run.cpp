// Library > Convert...'s run on Linux, as app_convert.cpp's on Windows: the question before replacing,
// the conversion on a worker (folder scanning paused meanwhile, so new files and originals leaving
// aren't mistaken for library changes), progress in the status bar, and for replacements the library
// switched over to each verified file with its plays, ratings and playlists. The work is convert.cpp's;
// the panel is panel_convert.cpp's. Undo walks back through the replacements still remembered.
#include "app.h"

#include <algorithm>
#include <memory>
#include <unordered_set>

namespace lx {
namespace {

std::thread worker;
std::atomic<bool> cancel{false};
bool converting = false;         // a run is in flight
bool replacing = false;          // the run replaces originals (copies leave the library alone)
CordExport runFormat = CordExport::None;
bool stopping = false;           // shutting down: the library is updated, nothing is said
bool canUndo = false;            // a log is left to undo, kept current instead of listing the folder on every menu update
std::vector<ConvertItem> plan;   // the running plan, for the progress line
std::vector<size_t> ordinal;     // plan index -> its place among the files converted
size_t total = 0;
ConvertResult* finished = nullptr;  // the worker's result until the UI thread takes it

std::wstring LogDir() { return g.dataDir + L"/convert"; }
unsigned MemoryDays() { return kConvertMemoryDays[g.settings.convertMemory]; }
std::wstring Files(size_t n) { return std::to_wstring(n) + (n == 1 ? L" file" : L" files"); }

std::wstring Megabytes(int64_t bytes) {
    wchar_t s[32];
    swprintf_s(s, L"%.1f MB", (double)bytes / 1048576.0);
    return s;
}

// A kept original's place in the library: shown as a track of its own, or left out like a track
// the user removed, so folder scans don't bring it back.
void ApplyOriginalVisibility(Track& t) {
    if (g.settings.convertShowOriginals) t.flags = (uint16_t)((t.flags | Track::kInLibrary) & ~(Track::kExcluded | Track::kMissing));
    else t.flags = (uint16_t)((t.flags | Track::kExcluded) & ~(Track::kInLibrary | Track::kInbox));
}

void Reread(std::vector<TrackId>& ids, TrackId id) {
    g_lib[id].flags &= ~(Track::kLoaded | Track::kTagFailed);
    ids.push_back(id);
}

}  // namespace

bool ConvertBusy() { return converting; }
bool CanUndoConversion() { return canUndo; }

void InitConvert() {
    PruneConvertLogs(LogDir(), MemoryDays());
    canUndo = CanUndoConvert(LogDir());
}

std::vector<ConvertItem> PlanConvertTracks(const std::vector<TrackId>& ids, CordExport format, bool replace, const std::wstring& folder) {
    std::vector<std::pair<TrackId, std::wstring>> tracks;
    tracks.reserve(ids.size());
    for (TrackId id : ids) tracks.push_back({id, g_lib.Path(id)});
    std::vector<ConvertItem> plan = PlanConvert(tracks, format, replace ? ConvertMode::Replace : ConvertMode::Copy, folder,
                                                [](TrackId id) { return g_lib[id].codec; });
    for (ConvertItem& item : plan)
        if (item.skip.empty() && item.id == g.playing) item.skip = L"playing now";
    return plan;
}

bool StartConvert(std::vector<ConvertItem> items, CordExport format, bool replace) {
    if (converting) return false;
    if (std::none_of(items.begin(), items.end(), [](const ConvertItem& i) { return i.skip.empty(); })) {
        ShowMessage(ConvertSummary(items) + L".");
        return false;
    }
    if (replace &&
        !Confirm(ToUtf8(ReplaceQuestion(items, g.settings.convertRecycle, g.settings.convertShowOriginals, MemoryDays())), "Convert"))
        return false;
    g.scanner.Stop();
    converting = true;
    replacing = replace;
    runFormat = format;
    cancel = false;
    plan = items;
    ordinal.assign(plan.size(), 0);
    total = 0;
    for (size_t i = 0; i < plan.size(); ++i)
        if (plan[i].skip.empty()) ordinal[i] = ++total;
    ConvertSettings settings;
    settings.recycle = g.settings.convertRecycle;
    // Half the cores: the conversion runs in the background and never competes with playback.
    settings.threads = std::max(1u, std::thread::hardware_concurrency() / 2);
    if (worker.joinable()) worker.join();
    worker = std::thread([items = std::move(items), settings, logDir = LogDir()]() {
        auto* result = new ConvertResult(ExecuteConvert(items, settings, logDir, cancel, [](size_t i) {
            PostMessageW(nullptr, WM_APP_CONVERT, 0, (LPARAM)i);
        }));
        finished = result;
        PostMessageW(nullptr, WM_APP_CONVERT, 1, 0);
    });
    g.redraw = true;
    return true;
}

// On the UI thread, from WM_APP_CONVERT: a file starting (progress), or the finished run. A replaced
// track moves to its new file, keeping its id, and a kept original gets a record of its own; copies
// leave the library alone (folder scanning finds them like any new file).
void OnConvertMessage(WPARAM wp, LPARAM lp) {
    if (!wp) {
        const size_t i = (size_t)lp;
        if (i < plan.size())
            ShowMessage(L"Converting " + std::to_wstring(ordinal[i]) + L" of " + std::to_wstring(total) + L" to " +
                        kCordExports[(unsigned)runFormat].label + L": " + FileNameOf(plan[i].from));
        return;
    }
    std::unique_ptr<ConvertResult> r(finished);
    finished = nullptr;
    if (!r) return;
    if (worker.joinable()) worker.join();
    converting = false;
    plan.clear();
    std::vector<TrackId> reread;
    int64_t saved = 0;
    for (const ConvertDone& d : r->done) {
        if (!replacing || !g_lib.Contains(d.id)) continue;
        const int64_t before = (int64_t)g_lib[d.id].fileSize;
        WIN32_FILE_ATTRIBUTE_DATA a{};
        if (GetFileAttributesExW(d.to.c_str(), GetFileExInfoStandard, &a))
            saved += before - (int64_t)((uint64_t)a.nFileSizeHigh << 32 | a.nFileSizeLow);
        g_lib.Rename(d.id, d.to);
        Reread(reread, d.id);
        if (!d.recycled) {
            TrackId original = g_lib.Find(d.from);
            if (original == kNone) original = g_lib.Add(d.from);
            g_lib[original].flags |= Track::kOriginal;
            ApplyOriginalVisibility(g_lib[original]);
            if (g.settings.convertShowOriginals) Reread(reread, original);
        }
    }
    QueueTags(reread);
    if (replacing) {
        g_lib.Touch();
        g.libraryDirty = true;
        RebuildRows();
    }
    if (!stopping) StartScanner();
    PruneConvertLogs(LogDir(), MemoryDays());
    canUndo = CanUndoConvert(LogDir());
    std::wstring done = ConvertDoneText(*r, runFormat, replacing, g.settings.convertRecycle);
    if (saved > 0) done += L" They take " + Megabytes(saved) + L" less.";
    g.redraw = true;
    if (stopping) return;
    if (!r->failed.empty()) {
        std::string text = ToUtf8(done) + "\n\n" + std::to_string(r->failed.size()) + " couldn't be converted and were left as they were:\n\n";
        for (size_t i = 0; i < r->failed.size() && i < 8; ++i)
            text += ToUtf8(r->failed[i].first) + "\n    " + ToUtf8(r->failed[i].second) + "\n";
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "Some files couldn't be converted", text.c_str(), g.window);
    }
    ShowMessage(done);
}

// Reverses the newest replacement still remembered: the originals come back and the library returns
// to them, keeping each track's id; the converted files go to the trash.
void UndoConversion() {
    if (converting) return;
    if (!canUndo) {
        ShowMessage(L"There's no conversion left to undo.");
        return;
    }
    if (!Confirm("Undo the last conversion?\n\nIts originals come back (from the Recycle Bin, if they went there) and the library "
                 "returns to them. The converted files go to the trash.",
                 "Undo conversion"))
        return;
    g.scanner.Stop();
    std::wstring error;
    const auto undone = UndoLastConvert(LogDir(), true, error);
    std::vector<TrackId> reread;
    for (const ConvertUndone& u : undone) {
        TrackId cord = g_lib.Find(u.cord), original = g_lib.Find(u.original);
        if (cord != kNone && original != kNone && cord != original) {
            // Both paths have a record: the tracks trade places, so the one with the plays and ratings
            // returns to the original and the kept original's record takes the .cord's path.
            const std::wstring parked = u.cord + L".undo";
            g_lib.Rename(cord, parked);
            g_lib.Rename(original, u.cord);
            g_lib.Rename(cord, u.original);
            std::swap(cord, original);
            Track& left = g_lib[cord];
            left.flags = (uint16_t)((left.flags | Track::kExcluded) & ~(Track::kOriginal | Track::kInLibrary | Track::kInbox));
        } else if (cord != kNone) {
            g_lib.Rename(cord, u.original);
            original = cord;
        }
        if (original == kNone) continue;
        Track& t = g_lib[original];
        t.flags = (uint16_t)((t.flags | Track::kInLibrary) & ~(Track::kOriginal | Track::kExcluded | Track::kMissing));
        Reread(reread, original);
    }
    QueueTags(reread);
    g_lib.Touch();
    g.libraryDirty = true;
    RebuildRows();
    StartScanner();
    canUndo = CanUndoConvert(LogDir());
    ShowMessage(error.empty() ? L"Brought back " + Files(undone.size()) + L" from before the conversion." : error);
}

// Preferences: kept originals appear in the library, or leave it, at once.
void SetConvertShowOriginals(bool on) {
    g.settings.convertShowOriginals = on;
    std::vector<TrackId> reread;
    size_t count = 0;
    for (TrackId t = 0; t < (TrackId)g_lib.Count(); ++t)
        if (g_lib[t].flags & Track::kOriginal) {
            ApplyOriginalVisibility(g_lib[t]);
            if (on && !g_lib[t].Loaded()) reread.push_back(t);
            ++count;
        }
    QueueTags(reread);
    g_lib.Touch();
    g.libraryDirty = true;
    RebuildRows();
    ShowMessage((on ? L"Showing " : L"Hiding ") + std::to_wstring(count) + (count == 1 ? L" kept original" : L" kept originals") +
                L" in the library.");
}

void SetConvertMemory(unsigned index) {
    g.settings.convertMemory = (uint8_t)index;
    PruneConvertLogs(LogDir(), MemoryDays());
    canUndo = CanUndoConvert(LogDir());
}

void CancelConversion() {
    if (!converting) return;
    cancel = true;
    ShowMessage(L"Cancelling the conversion after the current file...");
}

// Shutdown: the file in progress is abandoned (it leaves nothing behind), and the files already
// converted are applied to the library before it is saved.
void StopConverter() {
    stopping = true;
    cancel = true;
    if (worker.joinable()) worker.join();
    if (finished) OnConvertMessage(1, 0);
}

}  // namespace lx
