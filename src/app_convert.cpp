// Converting to .cord and exporting .cord to FLAC (Library and track menus): the confirmation, the
// conversion on a background worker (folder watching paused meanwhile, so the new files and the
// originals leaving aren't mistaken for library changes), progress in the status bar, and the
// library switched over to each verified file with its plays, ratings and playlists. The work
// itself is convert.cpp's. Undo walks back through the conversions still remembered.
#include "app.h"
#include "convert.h"

namespace app {

namespace {

std::thread g_converter;
std::atomic<bool> g_cancel{false};
bool g_converting = false;      // UI-thread flag: a run is in flight
CordExport g_format = CordExport::None;  // exports keep their source tracks and need no undo log
bool g_canUndo = false;         // a log is left to undo (kept current instead of listing the folder on every menu update)
std::vector<ConvertItem> g_plan;  // the running plan, for the progress line
std::vector<size_t> g_ordinal;    // plan index -> its place among the files converted
size_t g_total = 0;
std::unordered_set<TrackId> g_convertingIds;

std::wstring LogDir() { return g.dataDir + L"\\convert"; }
unsigned MemoryDays() { return kConvertMemoryDays[g.convertMemory]; }

std::wstring Files(size_t n) { return std::to_wstring(n) + (n == 1 ? L" file" : L" files"); }

std::wstring Megabytes(int64_t bytes) {
    wchar_t s[32];
    swprintf_s(s, L"%.1f MB", (double)bytes / 1048576.0);
    return s;
}

// A kept original's place in the library: shown as a track of its own, or left out like a track
// the user removed, so folder scans don't bring it back.
void ApplyOriginalVisibility(Track& t) {
    if (g.convertShowOriginals) t.flags = (uint16_t)((t.flags | Track::kInLibrary) & ~(Track::kExcluded | Track::kMissing));
    else t.flags = (uint16_t)((t.flags | Track::kExcluded) & ~(Track::kInLibrary | Track::kInbox));
}

void Reread(std::vector<TrackId>& ids, TrackId id) {
    if (!g.tagPending.count(id)) g_lib[id].flags &= ~(Track::kLoaded | Track::kTagFailed);
    ids.push_back(id);
}

}  // namespace

bool ConvertBusy() { return g_converting; }
bool ConvertUsing(TrackId id) { return g_convertingIds.count(id) != 0; }

HMENU CordExportMenu() {
    HMENU menu = CreatePopupMenu();
    for (unsigned f = 1; f < (unsigned)CordExport::Count; ++f) {
        const std::wstring text = std::wstring(kCordExports[f].label) + L"...";
        AppendMenuW(menu, MF_STRING | (g_converting ? MF_GRAYED : MF_ENABLED), IDM_EXPORT_BASE + f, text.c_str());
    }
    return menu;
}

void InitConvert() {
    PruneConvertLogs(LogDir(), MemoryDays());
    g_canUndo = CanUndoConvert(LogDir());
}

// Plans the selection, says what will happen, and hands the files to the worker; the results come
// back as WM_APP_CONVERT messages.
void ConvertTracks(const std::vector<TrackId>& ids, CordExport format) {
    const bool exporting = format != CordExport::None;
    if (g_converting || ids.empty()) {
        if (ids.empty()) ShowMessage(L"Nothing to convert.");
        return;
    }
    std::vector<std::pair<TrackId, std::wstring>> tracks;
    for (TrackId id : ids) tracks.push_back({id, g_lib.Path(id)});
    std::vector<ConvertItem> plan = PlanConvert(tracks, format);
    size_t ready = 0, already = 0, other = 0;
    for (ConvertItem& item : plan) {
        if (item.skip.empty() && item.id == g.playing) item.skip = L"playing now";
        if (item.skip.empty()) ++ready;
        else if (item.skip == L"already .cord") ++already;
        else ++other;
    }
    if (!ready) {
        ShowMessage(exporting ? L"Nothing to export: select .cord files that aren't playing and have no output file of the same name."
                               : already == plan.size() ? L"Those files are .cord already."
                                           : L"Nothing to convert: only FLAC, WAV and AIFF files convert, and not one that's playing.");
        return;
    }
    std::wstring q;
    if (exporting) {
        q = L"Export " + Files(ready) + L" from .cord to " + kCordExports[(unsigned)format].label + L"?\n\nThe copies go in the same folders, "
            L"with their tags and covers. Existing files are never overwritten.\n\n";
        q += kCordExports[(unsigned)format].kbps
            ? L"These are lossy 48 kHz stereo copies. Each copy is checked for complete decoding, length and metadata.\n\n"
            : L"The exact audio and metadata are verified before each file appears.\n\n";
        q +=
            L"The .cord files are kept. No original files or conversion history are needed.";
        if (other) q += L"\n\n" + Files(other) + L" won't export: not .cord, playing, part of a cue sheet, or an output file already exists.";
    } else {
        q = L"Convert " + Files(ready) + L" to .cord?\n\nEach new file is read back and compared with its original, sample for "
            L"sample, before the library switches to it. ";
        q += g.convertRecycle ? L"The originals then go to the Recycle Bin."
                              : g.convertShowOriginals ? L"The originals stay where they are, and in the library."
                                                       : L"The originals stay where they are, left out of the library.";
        q += L"\nLibrary > Undo last conversion brings them back for " +
             (MemoryDays() == 1 ? std::wstring(L"a day") : MemoryDays() % 7 ? std::to_wstring(MemoryDays()) + L" days"
                                  : MemoryDays() == 7 ? std::wstring(L"a week") : std::to_wstring(MemoryDays() / 7) + L" weeks") + L".";
        if (already || other)
            q += L"\n\n" + Files(already + other) + L" won't convert: lossy, .cord already, playing, or a cue sheet's part.";
    }
    if (MessageBoxW(g.hwnd, q.c_str(), kAppName, MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;

    g.scanner.Stop();
    g_converting = true;
    g_format = format;
    g_cancel = false;
    g_plan = plan;
    g_ordinal.assign(plan.size(), 0);
    g_total = 0;
    for (size_t i = 0; i < plan.size(); ++i)
        if (plan[i].skip.empty()) {
            g_ordinal[i] = ++g_total;
            g_convertingIds.insert(plan[i].id);
        }
    ConvertSettings settings;
    settings.recycle = g.convertRecycle;
    // Half the cores: the conversion runs in the background and never competes with playback.
    settings.threads = std::max(1u, std::thread::hardware_concurrency() / 2);
    if (g_converter.joinable()) g_converter.join();
    g_converter = std::thread([hwnd = g.hwnd, plan = std::move(plan), settings, logDir = LogDir()]() {
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        auto* r = new ConvertResult(
            ExecuteConvert(plan, settings, logDir, g_cancel, [hwnd](size_t i) { PostMessageW(hwnd, WM_APP_CONVERT, 0, (LPARAM)i); }));
        if (!PostMessageW(hwnd, WM_APP_CONVERT, 1, (LPARAM)r)) delete r;
    });
    InvalidateRect(g.list, nullptr, FALSE);
    UpdateStatus();
}

// UI thread, from WM_APP_CONVERT: a file starting (progress), or the finished run. Each converted
// track moves to its .cord file, keeping its id, and a kept original gets a record of its own.
void OnConvertMessage(WPARAM wp, LPARAM lp) {
    const bool exporting = g_format != CordExport::None;
    if (!wp) {
        const size_t i = (size_t)lp;
        if (i < g_plan.size())
            ShowMessage((exporting ? L"Exporting " : L"Converting ") + std::to_wstring(g_ordinal[i]) + L" of " + std::to_wstring(g_total) +
                        L" to " + kCordExports[(unsigned)g_format].label + L": " +
                        FileNameOf(g_plan[i].from));
        return;
    }
    std::unique_ptr<ConvertResult> r(reinterpret_cast<ConvertResult*>(lp));
    if (g_converter.joinable()) g_converter.join();
    g_converting = false;
    std::unordered_set<TrackId>().swap(g_convertingIds);
    std::vector<ConvertItem>().swap(g_plan);
    std::vector<TrackId> reread;
    int64_t saved = 0;
    for (const ConvertDone& d : r->done) {
        if (exporting) continue;
        if (!g_lib.Contains(d.id)) continue;
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
            if (g.convertShowOriginals) Reread(reread, original);
        }
    }
    QueueTags(reread);
    if (!exporting) {
        g_lib.Touch();
        g.dirty = true;
        RebuildView();
    }
    StartScanner();
    PruneConvertLogs(LogDir(), MemoryDays());
    g_canUndo = CanUndoConvert(LogDir());
    std::wstring done = (exporting ? L"Exported " : L"Converted ") + Files(r->done.size()) + L" to " + kCordExports[(unsigned)g_format].label;
    if (saved > 0) done += L", " + Megabytes(saved) + L" smaller";
    done += r->cancelled ? L"; the rest was cancelled." : L".";
    if (exporting) done += L" The .cord files were kept.";
    if (!exporting && g.convertRecycle) {
        const size_t kept = (size_t)std::count_if(r->done.begin(), r->done.end(), [](const ConvertDone& d) { return !d.recycled; });
        if (kept)
            done += L" " + Files(kept) + (kept == 1 ? L" was" : L" were") +
                    L" kept: the Recycle Bin wasn't available there.";
    }
    if (g.closing) return;  // shutting down: the library is updated, nothing to say
    if (!r->failed.empty()) {
        std::wstring text = done + L"\n\n" + Files(r->failed.size()) + L" couldn't be converted and were left as they were:\n\n";
        for (size_t i = 0; i < r->failed.size() && i < 8; ++i) text += r->failed[i].first + L"\n    " + r->failed[i].second + L"\n";
        MessageBoxW(g.hwnd, text.c_str(), kAppName, MB_OK | MB_ICONWARNING);
    } else if (!r->done.empty()) {
        PlaySfx(Sfx::Organized);
    }
    ShowMessage(done);
    UpdateStatus();
}

// Reverses the newest conversion still remembered: the originals come back and the library
// returns to them, keeping each track's id; the .cord files go to the Recycle Bin.
void UndoConversion() {
    if (g_converting) return;
    if (!g_canUndo) {
        ShowMessage(L"There's no conversion left to undo.");
        return;
    }
    if (MessageBoxW(g.hwnd, L"Undo the last conversion?\n\nIts originals come back (from the Recycle Bin, if they went there) and "
                            L"the library returns to them. The .cord files go to the Recycle Bin.",
                    kAppName, MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        return;
    g.scanner.Stop();
    std::wstring error;
    const auto undone = UndoLastConvert(LogDir(), true, error);
    std::vector<TrackId> reread;
    for (const ConvertUndone& u : undone) {
        TrackId cord = g_lib.Find(u.cord), original = g_lib.Find(u.original);
        if (cord != kNone && original != kNone && cord != original) {
            // Both paths have a record: the tracks trade places, so the one with the plays and
            // ratings returns to the original and the kept original's record takes the .cord's path.
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
    g.dirty = true;
    RebuildView();
    StartScanner();
    g_canUndo = CanUndoConvert(LogDir());
    if (!error.empty()) PlaySfx(Sfx::Oops);
    else if (!undone.empty()) PlaySfx(Sfx::Unorganized);
    ShowMessage(error.empty() ? L"Brought back " + Files(undone.size()) + L" from before the conversion." : error);
}

bool ConvertCommand(int id) {
    if (id > IDM_EXPORT_BASE && id < IDM_EXPORT_BASE + (int)CordExport::Count) {
        auto ids = SelectedTracks();
        if (ids.empty())
            for (TrackId t = 0; t < (TrackId)g_lib.Count(); ++t)
                if (g_lib[t].InLibrary()) ids.push_back(t);
        ConvertTracks(ids, (CordExport)(id - IDM_EXPORT_BASE));
        return true;
    }
    if (id >= IDM_CONVERT_MEMORY_BASE && id < IDM_CONVERT_MEMORY_BASE + (int)std::size(kConvertMemoryDays)) {
        g.convertMemory = (uint8_t)(id - IDM_CONVERT_MEMORY_BASE);
        SavePreferences();
        PruneConvertLogs(LogDir(), MemoryDays());
        g_canUndo = CanUndoConvert(LogDir());
        ShowMessage(L"Conversions can be undone for " + std::to_wstring(MemoryDays()) + (MemoryDays() == 1 ? L" day." : L" days."));
        return true;
    }
    switch (id) {
        case IDM_CONVERT: {
            // The selection, or the whole library when nothing is selected.
            auto ids = SelectedTracks();
            if (ids.empty())
                for (TrackId t = 0; t < (TrackId)g_lib.Count(); ++t)
                    if (g_lib[t].InLibrary()) ids.push_back(t);
            ConvertTracks(ids);
            return true;
        }
        case IDM_CONVERT_UNDO: UndoConversion(); return true;
        case IDM_CONVERT_CANCEL:
            if (g_converting) {
                g_cancel = true;
                ShowMessage(L"Cancelling the conversion after the current file...");
            }
            return true;
        case IDM_CONVERT_RECYCLE:
            g.convertRecycle = !g.convertRecycle;
            SavePreferences();
            ShowMessage(g.convertRecycle ? L"Converted originals will go to the Recycle Bin."
                                         : L"Converted originals will be kept where they are.");
            return true;
        case IDM_CONVERT_SHOW_ORIGINALS: {
            g.convertShowOriginals = !g.convertShowOriginals;
            SavePreferences();
            std::vector<TrackId> reread;
            size_t count = 0;
            for (TrackId t = 0; t < (TrackId)g_lib.Count(); ++t)
                if (g_lib[t].flags & Track::kOriginal) {
                    ApplyOriginalVisibility(g_lib[t]);
                    if (g.convertShowOriginals && !g_lib[t].Loaded()) reread.push_back(t);
                    ++count;
                }
            QueueTags(reread);
            g_lib.Touch();
            g.dirty = true;
            RebuildView();
            ShowMessage((g.convertShowOriginals ? L"Showing " : L"Hiding ") + std::to_wstring(count) +
                        (count == 1 ? L" kept original" : L" kept originals") + L" in the library.");
            return true;
        }
    }
    return false;
}

// Cancel conversion comes and goes with the run (UpdateMainMenu); the options are in Preferences.
void UpdateConvertMenu(HMENU m) {
    EnableMenuItem(m, IDM_CONVERT, MF_BYCOMMAND | (g_converting ? MF_GRAYED : MF_ENABLED));
    for (unsigned f = 1; f < (unsigned)CordExport::Count; ++f)
        EnableMenuItem(m, IDM_EXPORT_BASE + f, MF_BYCOMMAND | (g_converting ? MF_GRAYED : MF_ENABLED));
    EnableMenuItem(m, IDM_CONVERT_UNDO, MF_BYCOMMAND | (!g_converting && g_canUndo ? MF_ENABLED : MF_GRAYED));
}

// Shutdown: the file in progress is abandoned (it leaves nothing behind), and the files already
// converted are applied to the library before it is saved.
void StopConverter() {
    g_cancel = true;
    if (!g_converter.joinable()) return;
    g_converter.join();
    MSG done;
    while (PeekMessageW(&done, g.hwnd, WM_APP_CONVERT, WM_APP_CONVERT, PM_REMOVE))
        if (done.wParam) OnConvertMessage(done.wParam, done.lParam);
}

}  // namespace app
