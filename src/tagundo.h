// Undo for tag edits. Before a tag write changes a file, the values it is about to replace are
// read from the file and saved in a journal, one per edit, so the edit can be reversed.
// Journals live in <dataDir>\tagundo as <unix time>.cdu; the newest 10 are kept.
// The write itself is already atomic (tagwriter.h), so this journal is undo history, not crash
// recovery: only the newest journal is ever replayed, and only by the Undo command.
#pragma once
#include "tagwriter.h"

#include <cstdint>
#include <string>
#include <vector>

// The fields one edit changes, as they were in one file.
struct TagUndoFile {
    std::wstring path;
    TagEdit old;                  // exactly the fields the edit sets, with their previous values
    bool coverRestorable = true;  // false: the previous cover was too big to keep (old.cover unset)
};

struct TagJournal {
    uint64_t time = 0;  // unix seconds; also the file name, bumped when a name is taken
    std::vector<TagUndoFile> files;
};

// Reads the values `edit` would replace in `path`. False when the file's tags can't be read,
// in which case nothing can be restored and the caller should leave the file out.
bool CaptureTagUndo(const std::wstring& path, const TagEdit& edit, TagUndoFile& out);

// Writes a journal into `dir` and prunes older ones. `journal.time` is the name it was saved under.
bool SaveTagJournal(const std::wstring& dir, TagJournal& journal, std::wstring& error);
// The newest journal in `dir`: its file, and how many files it covers (from the header alone).
bool FindNewestTagJournal(const std::wstring& dir, std::wstring& file, uint32_t& fileCount);
bool LoadTagJournal(const std::wstring& file, TagJournal& journal);
void DeleteTagJournal(const std::wstring& file);
