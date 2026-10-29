// Library folders on Linux: the scanner's listings merged into the library, and tags read on a
// worker thread. The merge follows the Windows build's (app_library.cpp): a file that moved keeps
// its stats (same name, size and time), tracks that left a listed folder are marked missing,
// and newly arrived files wait in the Inbox when it is enabled.
#include "app.h"

#include "archive.h"
#include "cue.h"
#include "game_music.h"
#include "points.h"
#include "sort.h"
#include "ui.h"

#include <unordered_map>

namespace lx {

namespace {

struct ScopeScan {
    bool initial = false, rootOk = true, deferNew = false;
    size_t startCount = 0;
    std::vector<char> seen;
    std::vector<ScanFile> pending;
    std::vector<TrackId> toRead;
    std::vector<TrackId> joined;
    size_t added = 0, moved = 0, missing = 0;
    std::unordered_map<StringPool::Handle, bool> dirUnder;
};
std::unordered_map<std::wstring, ScopeScan> g_scans;

bool PathIsUnder(const std::wstring& path, const std::wstring& dir) {
    return path.size() > dir.size() && path.compare(0, dir.size(), dir) == 0 &&
           (path[dir.size()] == L'/' || dir.back() == L'/');
}

bool Under(const std::wstring& scope, ScopeScan& st, const Track& t) {
    auto [it, added] = st.dirUnder.try_emplace(t.dir, false);
    if (added) {
        std::wstring dir = g_lib.Str(t.dir);
        it->second = dir == scope || PathIsUnder(dir, scope);
    }
    return it->second;
}

TrackId AddScanned(ScopeScan& st, const ScanFile& f) {
    TrackId id = g_lib.Add(f.path);
    Track& t = g_lib[id];
    if (!(t.flags & (Track::kInLibrary | Track::kInbox | Track::kExcluded))) {
        ++st.added;
        if (!st.initial && g.settings.inboxEnabled) t.flags |= Track::kInbox;
    }
    return id;
}

void FoundScanned(ScopeScan& st, TrackId id, const ScanFile& f) {
    Track& t = g_lib[id];
    if (t.flags & Track::kExcluded) return;
    if (!(t.flags & (Track::kInLibrary | Track::kInbox))) t.added = UnixNow();
    t.flags = (uint16_t)((t.flags | Track::kInLibrary) & ~Track::kMissing);
    if (t.InLibrary() && !(t.flags & Track::kCollected)) st.joined.push_back(id);
    bool changed = t.fileSize != f.size || t.fileTime != f.time;
    t.fileSize = f.size;
    t.fileTime = f.time;
    if (changed || !t.Loaded()) st.toRead.push_back(id);
}

void FinishScope(const std::wstring& scope, ScopeScan& st) {
    if (st.deferNew) {
        std::vector<std::pair<uint64_t, TrackId>> gone;
        for (TrackId id = 0; id < (TrackId)st.startCount; ++id) {
            const Track& t = g_lib[id];
            if (!(t.flags & (Track::kInLibrary | Track::kInbox)) || st.seen[id]) continue;
            if ((t.flags & Track::kMissing) || (st.rootOk && Under(scope, st, t))) gone.emplace_back(t.fileSize ^ t.fileTime, id);
        }
        std::vector<char>().swap(st.seen);
        std::sort(gone.begin(), gone.end());
        for (ScanFile& f : st.pending) {
            TrackId id = kNone;
            auto it = std::lower_bound(gone.begin(), gone.end(), std::make_pair(f.size ^ f.time, TrackId(0)));
            for (; it != gone.end() && it->first == (f.size ^ f.time); ++it) {
                if (it->second == kNone) continue;
                const Track& t = g_lib[it->second];
                if (t.fileSize == f.size && t.fileTime == f.time && g_lib.Str(t.file) == FileNameOf(f.path)) {
                    id = it->second;
                    it->second = kNone;
                    break;
                }
            }
            if (id != kNone) {
                g_lib.Rename(id, f.path);
                ++st.moved;
            } else {
                id = AddScanned(st, f);
            }
            FoundScanned(st, id, f);
        }
        std::vector<ScanFile>().swap(st.pending);
        if (st.rootOk)
            for (auto& [key, id] : gone) {
                if (id == kNone) continue;
                Track& t = g_lib[id];
                if (!(t.flags & Track::kMissing) && Under(scope, st, t)) {
                    t.flags |= Track::kMissing;
                    ++st.missing;
                }
            }
    }
    QueueTags(st.toRead);
    st.toRead.clear();
    g_lib.Touch();
    g.libraryDirty = true;
    AnnouncePoints(AwardLibraryAdds(st.joined));
    UpdateInboxTab();
    static uint64_t lastChirp = 0;
    if (!st.initial && st.added && SDL_GetTicks() - lastChirp > 10000) {  // one chirp for a batch of folders
        lastChirp = SDL_GetTicks();
        PlaySfx(Sfx::NewFiles);
    }
    if (!st.initial && (st.added || st.moved || st.missing)) {
        std::wstring msg = L"Library: ";
        if (st.added) msg += std::to_wstring(st.added) + (g.settings.inboxEnabled ? L" new in the Inbox" : L" added");
        if (st.moved) msg += (st.added ? L", " : L"") + std::to_wstring(st.moved) + L" moved";
        if (st.missing) msg += (st.added || st.moved ? L", " : L"") + std::to_wstring(st.missing) + L" missing";
        ShowMessage(msg + L".");
    }
}

// --- tags -------------------------------------------------------------------------------------
struct TagJob {
    TrackId id;
    std::wstring path;
    bool chapters = false;
    uint64_t generation = 0;
};
std::deque<TagJob> g_tagJobs;  // guarded by g.tagMx

void TagWorker() {
    for (;;) {
        std::vector<TagJob> batch;
        {
            std::unique_lock lock(g.tagMx);
            g.tagCv.wait(lock, [] { return g.tagQuit || !g_tagJobs.empty(); });
            if (g.tagQuit) return;
            while (!g_tagJobs.empty() && batch.size() < 64) {
                batch.push_back(std::move(g_tagJobs.front()));
                g_tagJobs.pop_front();
            }
        }
        auto* results = new std::vector<TagResult>();
        for (auto& job : batch) {
            if (job.chapters) {
                auto* result = new ChapterResult{job.id, job.generation, job.path, {}};
                WIN32_FILE_ATTRIBUTE_DATA attrs{};
                std::wstring audio = AudioFileOf(job.path);
                result->haveStats = GetFileAttributesExW(ContainerOf(audio).c_str(), GetFileExInfoStandard, &attrs) != FALSE;
                if (result->haveStats) {
                    result->fileSize = (uint64_t)attrs.nFileSizeHigh << 32 | attrs.nFileSizeLow;
                    result->fileTime = (uint64_t)attrs.ftLastWriteTime.dwHighDateTime << 32 | attrs.ftLastWriteTime.dwLowDateTime;
                    if (IsSubtrack(job.path) && !IsGameMusicFile(audio)) {
                        CueSubtrack cue;
                        if (LookupSubtrack(job.path, cue)) result->fileTime = std::max(result->fileTime, cue.cueTime);
                    }
                }
                if (!IsSubtrack(job.path)) ReadChapters(job.path, result->chapters);
                if (!PostMessageW(nullptr, WM_APP_CHAPTERS, 0, (LPARAM)result)) delete result;
                continue;
            }
            TagResult r{job.id, job.path, {}, false};
            WIN32_FILE_ATTRIBUTE_DATA attrs{};
            r.haveStats = GetFileAttributesExW(job.path.c_str(), GetFileExInfoStandard, &attrs) != FALSE;
            if (r.haveStats) {
                r.fileSize = (uint64_t)attrs.nFileSizeHigh << 32 | attrs.nFileSizeLow;
                r.fileTime = (uint64_t)attrs.ftLastWriteTime.dwHighDateTime << 32 | attrs.ftLastWriteTime.dwLowDateTime;
            }
            r.ok = ReadTags(job.path, r.tags);  // the shared reader also resolves CUE and game-music tracks
            results->push_back(std::move(r));
        }
        if (results->empty() || !PostMessageW(nullptr, WM_APP_TAGS, 0, (LPARAM)results)) delete results;
    }
}

}  // namespace

void StartScanner() {
    g.scanner.Stop();
    g_scans.clear();
    if (g.settings.folders.empty()) {
        g.scanning = false;
        return;
    }
    g.scanning = true;
    g.scanner.Start(nullptr, WM_APP_SCAN, g.settings.folders);
}

void OnScanResult(ScanResult* r, bool firstScanDone) {
    if (firstScanDone) {
        g.scanning = false;
        g_scans.clear();
        g.redraw = true;
        return;
    }
    if (!r) return;
    std::unique_ptr<ScanResult> owned(r);
    ScopeScan& st = g_scans[r->scope];
    if (r->first) {
        st = ScopeScan{};
        st.initial = r->initial;
        st.rootOk = r->rootOk;
        st.startCount = g_lib.Count();
        st.seen.assign(st.startCount, 0);
        for (TrackId id = 0; id < (TrackId)st.startCount && !st.deferNew; ++id) {
            const Track& t = g_lib[id];
            st.deferNew = (t.flags & (Track::kInLibrary | Track::kInbox)) &&
                          ((t.flags & Track::kMissing) || (st.rootOk && Under(r->scope, st, t)));
        }
    }
    st.rootOk = st.rootOk && r->rootOk;
    for (const ScanFile& f : r->files) {
        TrackId id = g_lib.Find(f.path);
        if (id == kNone) {
            if (st.deferNew) st.pending.push_back(f);
            else FoundScanned(st, AddScanned(st, f), f);
            continue;
        }
        if (id < (TrackId)st.seen.size()) st.seen[id] = 1;
        FoundScanned(st, id, f);
    }
    if (r->complete) {
        FinishScope(r->scope, st);
        g_scans.erase(r->scope);
    } else {
        QueueTags(st.toRead);
        st.toRead.clear();
        g_lib.Touch();
    }
    g.redraw = true;
}

void StartTagWorker() {
    g.tagQuit = false;
    g.tagThread = std::thread(TagWorker);
}

void StopTagWorker() {
    {
        std::lock_guard lock(g.tagMx);
        g.tagQuit = true;
    }
    g.tagCv.notify_all();
    if (g.tagThread.joinable()) g.tagThread.join();
}

void QueueTags(const std::vector<TrackId>& ids) {
    if (ids.empty()) return;
    {
        std::lock_guard lock(g.tagMx);
        for (TrackId id : ids) g_tagJobs.push_back({id, g_lib.Path(id)});
    }
    g.tagCv.notify_one();
}

void QueueChapters(TrackId id) {
    g.chapterTrack = id;
    ++g.chapterGeneration;
    g.chapters.clear();
    if (!g_lib.Contains(id)) return;
    std::wstring path = g_lib.Path(id);
    {
        std::lock_guard lock(g.tagMx);
        std::erase_if(g_tagJobs, [](const TagJob& job) { return job.chapters; });
        g_tagJobs.push_front({id, std::move(path), true, g.chapterGeneration});
    }
    g.tagCv.notify_one();
}

void AcceptChapters(ChapterResult* result) {
    std::unique_ptr<ChapterResult> owned(result);
    if (result->id != g.playing || result->id != g.chapterTrack || result->generation != g.chapterGeneration ||
        !g_lib.Contains(result->id) || g_lib.Path(result->id) != result->path) return;
    Track& t = g_lib[result->id];
    if (result->haveStats && (t.fileSize != result->fileSize || t.fileTime != result->fileTime)) {
        bool looping = SectionOf(result->id).on;
        t.fileSize = result->fileSize;
        t.fileTime = result->fileTime;
        g_lib.Touch();
        g.libraryDirty = true;
        RefreshTrackArt(result->id);
        if (t.Loaded()) QueueTags({result->id});
        if (looping) SeekTo(g.engine.Position());
    }
    g.chapters = std::move(result->chapters);
    g.redraw = true;
}

void AcceptTags(std::vector<TagResult>* results) {
    std::unique_ptr<std::vector<TagResult>> owned(results);
    bool playingChanged = false;
    for (auto& r : *results) {
        if (r.id >= g_lib.Count() || g_lib.Path(r.id) != r.path) continue;  // renamed meanwhile
        if (r.ok) {
            Track& t = g_lib[r.id];
            bool changed = r.haveStats && (t.fileSize != r.fileSize || t.fileTime != r.fileTime);
            if (r.haveStats) { t.fileSize = r.fileSize; t.fileTime = r.fileTime; }
            g_lib.ApplyTags(r.id, r.tags);
            if (changed) RefreshTrackArt(r.id);
            playingChanged |= r.id == g.playing;
        } else g_lib[r.id].flags = (uint16_t)((g_lib[r.id].flags & ~Track::kLoaded) | Track::kTagFailed);
    }
    g_lib.Touch();
    g.libraryDirty = true;
    g.redraw = true;
    if (playingChanged) MprisChanged();
}

void AddFolder(const std::wstring& folder) {
    std::wstring f = folder;
    while (f.size() > 1 && f.back() == L'/') f.pop_back();
    for (auto& existing : g.settings.folders)
        if (existing == f || PathIsUnder(f, existing)) {
            ShowMessage(L"That folder is already in the library.");
            return;
        }
    g.settings.folders.push_back(f);
    SaveSettings();
    StartScanner();
    ShowMessage(L"Adding " + f + L" to the library...");
}

void RemoveFolder(size_t index) {
    if (index >= g.settings.folders.size()) return;
    std::wstring folder = g.settings.folders[index];
    g.settings.folders.erase(g.settings.folders.begin() + (ptrdiff_t)index);
    // Its tracks leave the library (they keep their stats, should the folder come back).
    for (TrackId id = 0; id < (TrackId)g_lib.Count(); ++id) {
        Track& t = g_lib[id];
        if (!(t.flags & (Track::kInLibrary | Track::kInbox))) continue;
        std::wstring dir = g_lib.Str(t.dir);
        if (dir == folder || PathIsUnder(dir, folder)) t.flags &= ~(Track::kInLibrary | Track::kInbox);
    }
    g_lib.Touch();
    g.libraryDirty = true;
    UpdateInboxTab();
    SaveSettings();
    StartScanner();
}

}  // namespace lx
