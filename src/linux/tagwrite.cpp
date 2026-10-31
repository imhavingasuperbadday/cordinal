// Writing tags on Linux, as app_tageditor.cpp does on Windows: one worker at a time writes a batch
// of edits, after saving the values they replace in an undo journal ($XDG_DATA_HOME/cordinal/
// tagundo, the newest ten kept). Each write is atomic (tagwriter.h). The file that is playing is
// written once playback moves on, so the decoder never reads a file while it is being rewritten.
#include "app.h"

#include "migration.h"
#include "tagundo.h"
#include "tagwriter.h"

#include <atomic>
#include <memory>
#include <unordered_set>

namespace lx {

namespace {

struct WriteJob {
    std::vector<TrackPath> files;
    std::vector<TagEdit> edits;  // one per file
    std::wstring journalDir;     // empty: this write is not recorded for undo (it is an undo)
    std::wstring journalAfter;   // an existing undo record to remove once every write succeeds
};
struct WriteDone {
    std::vector<TrackId> written;
    std::vector<std::pair<TrackId, std::wstring>> failed;  // track, error
    std::wstring journalFile;    // the new undo record, kept for the next Undo
    std::wstring removeJournal;  // an older record to remove after this undo lands cleanly
};
struct Deferred {
    TrackId id;
    TagEdit edit;
    bool undoable;
};

std::thread g_writer;
std::atomic<int> g_writeDone{0}, g_writeTotal{0};
std::vector<Deferred> g_deferred;  // waiting for playback to move on from the file

std::wstring TagUndoDir() { return g.dataDir + L"/tagundo"; }

void WriterThread(WriteJob job) {
    auto* done = new WriteDone();
    // The old values are recorded before any file changes. A file whose old values can't be
    // read is not written, since the edit could not be undone.
    std::vector<bool> capturedOk(job.files.size(), true);
    TagJournal journal;
    bool journaled = false;
    std::wstring journalError;
    if (!job.journalDir.empty()) {
        SHCreateDirectoryExW(nullptr, job.journalDir.c_str(), nullptr);
        for (size_t i = 0; i < job.files.size(); ++i) {
            TagUndoFile undo;
            if (CaptureTagUndo(job.files[i].Path(), job.edits[i], undo)) journal.files.push_back(std::move(undo));
            else capturedOk[i] = false;
        }
        if (!journal.files.empty()) {
            journaled = SaveTagJournal(job.journalDir, journal, journalError);
            if (!journaled) journalError = L"couldn't save the undo record: " + journalError;
            else done->journalFile = job.journalDir + L"/" + std::to_wstring(journal.time) + L".cdu";
        }
    }
    done->removeJournal = job.journalAfter;
    for (size_t i = 0; i < job.files.size(); ++i) {
        const auto& file = job.files[i];
        std::wstring error;
        if (!journalError.empty()) done->failed.emplace_back(file.id, journalError);
        else if (!capturedOk[i]) done->failed.emplace_back(file.id, L"couldn't read its tags, so the change can't be undone");
        else if (WriteTags(file.Path(), job.edits[i], error)) done->written.push_back(file.id);
        else done->failed.emplace_back(file.id, error);
        ++g_writeDone;
        PostMessageW(nullptr, WM_APP_TAGWRITE, 1, 0);
    }
    // Nothing changed, so there is nothing to undo.
    if (journaled && done->written.empty()) {
        DeleteTagJournal(done->journalFile);
        done->journalFile.clear();
    }
    if (!PostMessageW(nullptr, WM_APP_TAGWRITE, 0, (LPARAM)done)) delete done;
}

}  // namespace

bool TagWriteBusy() { return g_writeTotal > 0; }

std::wstring TagWriteStatus() {
    if (!g_writeTotal) return {};
    return L"Writing tags " + std::to_wstring(g_writeDone.load()) + L"/" + std::to_wstring(g_writeTotal.load());
}

// Queues one edit per track; the file playing is deferred. False when a write is still running.
bool SubmitTagWrites(const std::vector<std::pair<TrackId, TagEdit>>& items, bool undoable, std::wstring journalAfter) {
    if (g_writeTotal > 0) {
        ShowMessage(L"Tags are still being written; try again in a moment.");
        return false;
    }
    if (OrganizeBusy()) {
        ShowMessage(L"Files are being organized; try again in a moment.");
        return false;
    }
    WriteJob job{{}, {}, undoable ? TagUndoDir() : std::wstring(), std::move(journalAfter)};
    size_t deferred = 0, unsupported = 0;
    bool cueSkipped = false;
    for (const auto& [id, edit] : items) {
        if (id >= g_lib.Count() || edit.Empty()) continue;
        std::wstring path = g_lib.Path(id);
        if (IsSubtrack(path)) cueSkipped = true;
        else if (!CanWriteTags(path)) ++unsupported;
        else if (id == g.playing) {
            g_deferred.push_back({id, edit, undoable});
            ++deferred;
        } else {
            job.files.push_back(g_lib.SnapshotPath(id));
            job.edits.push_back(edit);
        }
    }
    if (cueSkipped) ShowMessage(L"Tags of CUE tracks come from the cue sheet; editing them isn't supported yet.");
    else if (unsupported) ShowMessage(std::to_wstring(unsupported) + L" file(s) can't be written (only MP3, FLAC, Ogg, Opus, M4A and cord can).");
    if (deferred) ShowMessage(L"The playing file will be updated when playback moves on.");
    if (job.files.empty()) return true;
    if (g_writer.joinable()) g_writer.join();
    g_writeDone = 0;
    g_writeTotal = (int)job.files.size();
    g_writer = std::thread(WriterThread, std::move(job));
    g.redraw = true;
    return true;
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
    SubmitTagWrites(items, true);
}

bool CanUndoTagEdit() {
    std::wstring file;
    uint32_t files = 0;
    return FindNewestTagJournal(TagUndoDir(), file, files);
}

// Puts back what the newest journal recorded. The undo itself is not journaled, and the journal
// goes once its writes are queued, so a second undo reaches the edit before it.
void UndoLastTagEdit() {
    std::wstring file;
    uint32_t count = 0;
    if (!FindNewestTagJournal(TagUndoDir(), file, count)) {
        ShowMessage(L"There's no tag edit to undo.");
        return;
    }
    if (g_writeTotal > 0) {
        ShowMessage(L"Tags are still being written; try again in a moment.");
        return;
    }
    TagJournal journal;
    if (!LoadTagJournal(file, journal)) {
        ShowMessage(L"The undo record for the last tag edit can't be read.");
        return;
    }
    std::vector<std::pair<TrackId, TagEdit>> items;
    for (const TagUndoFile& f : journal.files) {
        TrackId id = g_lib.Find(f.path);
        if (id == kNone) continue;  // moved or removed since the edit
        TagEdit edit = f.old;
        if (!f.coverRestorable) edit.cover.reset();
        if (!edit.Empty()) items.emplace_back(id, edit);
    }
    if (items.empty()) {
        ShowMessage(L"None of those files are in the library any more.");
        DeleteTagJournal(file);
        return;
    }
    bool deferred = false;
    for (const auto& item : items) deferred = deferred || item.first == g.playing;
    if (SubmitTagWrites(items, false, deferred ? std::wstring() : file))
        ShowMessage(L"Putting back the tags of " + std::to_wstring(items.size()) + L" file(s)...");
}

void OnTagWriteMessage(WPARAM wp, LPARAM lp) {
    g.redraw = true;
    if (wp == 1) return;
    std::unique_ptr<WriteDone> done(reinterpret_cast<WriteDone*>(lp));
    if (g_writer.joinable()) g_writer.join();
    g_writeTotal = 0;
    QueueTags(done->written);  // read the new tags back into the library
    if (!done->failed.empty()) {
        const auto& [id, error] = done->failed.front();
        std::wstring text = L"Couldn't write tags to " + std::to_wstring(done->failed.size()) + L" file(s): " +
                            FileNameOf(g_lib.Path(id)) + L": " + error;
        ShowMessage(text);
        PlaySfx(Sfx::Oops);
    } else {
        ShowMessage(L"Saved tags to " + std::to_wstring(done->written.size()) + L" file(s).");
        PlaySfx(Sfx::TagsSaved);
    }
    if (!done->removeJournal.empty() && done->failed.empty()) DeleteTagJournal(done->removeJournal);
    FlushDeferredTags();
}

// Called when playback moves on: writes what was waiting for the file to be let go.
void FlushDeferredTags() {
    if (g_deferred.empty() || g_writeTotal > 0) return;
    for (size_t i = 0; i < g_deferred.size(); ++i) {
        Deferred d = g_deferred[i];
        if (d.id == g.playing) continue;
        g_deferred.erase(g_deferred.begin() + (ptrdiff_t)i);
        SubmitTagWrites({{d.id, d.edit}}, d.undoable);
        return;  // one job at a time; the next flush picks up the rest
    }
}

// At exit: lets the running write finish. Edits still waiting for the playing file are written
// now, as playback has stopped.
void StopTagWriter() {
    if (g_writer.joinable()) g_writer.join();
    g_writeTotal = 0;
    for (const Deferred& d : g_deferred) {
        std::wstring path = g_lib.Path(d.id), error;
        TagJournal journal;
        TagUndoFile undo;
        if (d.undoable && CaptureTagUndo(path, d.edit, undo)) {
            journal.files.push_back(std::move(undo));
            SHCreateDirectoryExW(nullptr, TagUndoDir().c_str(), nullptr);
            if (!SaveTagJournal(TagUndoDir(), journal, error)) continue;
        } else if (d.undoable) continue;
        WriteTags(path, d.edit, error);
    }
    g_deferred.clear();
}

}  // namespace lx
