// Tag-undo journal: encoding, decoding and pruning of the <dataDir>\tagundo\*.cdu files declared
// in tagundo.h. A journal is written before any file is changed, and via the temp-file + rename
// dance, so a reader sees either a complete journal or none. The per-file layout is versioned
// only by the magic kMagic; changing how a field is stored means bumping it.
#include "sort.h"
#include "tagundo.h"

#include "library.h"
#include "tags.h"

#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>

namespace {

// Format: magic, time (u64), file count (u32); per file: path (UTF-8, u32 length first),
// coverRestorable (u8), a u32 mask of the fields present, then those fields in mask order. A new
// text field appends a mask bit, which older journals simply lack.
constexpr char kMagic[4] = {'C', 'D', 'U', '1'};
constexpr size_t kMaxCover = 4u << 20;  // bigger covers are not kept
constexpr size_t kKeepJournals = 10;
constexpr size_t kHeaderBytes = 16;

// Bit i of the mask: text field i, then numbers, then compilation and cover, then the
// ReplayGain / R128 fields and the listening statistics.
constexpr int kTextCount = 11, kNumCount = 5, kTailCount = 8;
constexpr int kCompilationBit = kTextCount + kNumCount, kCoverBit = kCompilationBit + 1;
constexpr int kTailBit = kCoverBit + 1;
std::optional<std::string> TagEdit::* const kText[kTextCount] = {
    &TagEdit::title, &TagEdit::artist, &TagEdit::album, &TagEdit::albumArtist, &TagEdit::genre, &TagEdit::date,
    &TagEdit::composer, &TagEdit::comment, &TagEdit::grouping, &TagEdit::label, &TagEdit::lyrics};
std::optional<uint32_t> TagEdit::* const kNum[kNumCount] = {&TagEdit::track, &TagEdit::trackTotal, &TagEdit::disc,
                                                           &TagEdit::discTotal, &TagEdit::bpm};
std::optional<std::string> TagEdit::* const kTail[kTailCount] = {
    &TagEdit::rgTrackGain, &TagEdit::rgTrackPeak, &TagEdit::rgAlbumGain, &TagEdit::rgAlbumPeak,
    &TagEdit::r128TrackGain, &TagEdit::r128AlbumGain, &TagEdit::rating, &TagEdit::playCount};

void Put32(std::string& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out += (char)(v >> (8 * i));
}

void Put64(std::string& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out += (char)(v >> (8 * i));
}

void PutString(std::string& out, const std::string& s) {
    Put32(out, (uint32_t)s.size());
    out += s;
}

// Bounds-checked reader: any short read makes the whole journal invalid.
struct Reader {
    const std::string& data;
    size_t pos = 0;
    bool ok = true;

    bool Need(size_t n) {
        if (!ok || data.size() - pos < n) return ok = false;
        return true;
    }
    uint32_t U32() {
        if (!Need(4)) return 0;
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= (uint32_t)(uint8_t)data[pos + i] << (8 * i);
        pos += 4;
        return v;
    }
    uint64_t U64() {
        uint64_t lo = U32();
        return lo | (uint64_t)U32() << 32;
    }
    uint8_t U8() {
        if (!Need(1)) return 0;
        return (uint8_t)data[pos++];
    }
    std::string Str() {
        uint32_t n = U32();
        if (!Need(n)) return {};
        std::string s = data.substr(pos, n);
        pos += n;
        return s;
    }
};

std::string Encode(const TagJournal& j) {
    std::string out(kMagic, 4);
    Put64(out, j.time);
    Put32(out, (uint32_t)j.files.size());
    for (const TagUndoFile& f : j.files) {
        PutString(out, ToUtf8(f.path));
        out += (char)(f.coverRestorable ? 1 : 0);
        uint32_t mask = 0;
        for (int i = 0; i < kTextCount; ++i)
            if ((f.old.*kText[i]).has_value()) mask |= 1u << i;
        for (int i = 0; i < kNumCount; ++i)
            if ((f.old.*kNum[i]).has_value()) mask |= 1u << (kTextCount + i);
        if (f.old.compilation) mask |= 1u << kCompilationBit;
        if (f.old.cover) mask |= 1u << kCoverBit;
        for (int i = 0; i < kTailCount; ++i)
            if ((f.old.*kTail[i]).has_value()) mask |= 1u << (kTailBit + i);
        Put32(out, mask);
        for (int i = 0; i < kTextCount; ++i)
            if (mask & (1u << i)) PutString(out, *(f.old.*kText[i]));
        for (int i = 0; i < kNumCount; ++i)
            if (mask & (1u << (kTextCount + i))) Put32(out, *(f.old.*kNum[i]));
        if (mask & (1u << kCompilationBit)) out += (char)(*f.old.compilation ? 1 : 0);
        if (mask & (1u << kCoverBit)) PutString(out, *f.old.cover);
        for (int i = 0; i < kTailCount; ++i)
            if (mask & (1u << (kTailBit + i))) PutString(out, *(f.old.*kTail[i]));
    }
    return out;
}

// Rejects a journal that is truncated, has trailing bytes, or lists no files; a single bad field
// invalidates the whole journal (the Reader's ok flag latches false).
bool Decode(const std::string& data, TagJournal& j) {
    if (data.size() < kHeaderBytes || std::memcmp(data.data(), kMagic, 4) != 0) return false;
    Reader r{data, 4};
    j.time = r.U64();
    uint32_t count = r.U32();
    j.files.clear();
    for (uint32_t i = 0; i < count && r.ok; ++i) {
        TagUndoFile f;
        f.path = FromUtf8(r.Str());
        f.coverRestorable = r.U8() != 0;
        uint32_t mask = r.U32();
        for (int k = 0; k < kTextCount && r.ok; ++k)
            if (mask & (1u << k)) f.old.*kText[k] = r.Str();
        for (int k = 0; k < kNumCount && r.ok; ++k)
            if (mask & (1u << (kTextCount + k))) f.old.*kNum[k] = r.U32();
        if (mask & (1u << kCompilationBit)) f.old.compilation = r.U8() != 0;
        if (mask & (1u << kCoverBit)) f.old.cover = r.Str();
        for (int k = 0; k < kTailCount && r.ok; ++k)
            if (mask & (1u << (kTailBit + k))) f.old.*kTail[k] = r.Str();
        if (r.ok) j.files.push_back(std::move(f));
    }
    return r.ok && r.pos == data.size() && !j.files.empty();
}

// Journal names are unix times; a name that's taken is bumped by a second so order is kept.
bool ParseName(const std::wstring& name, uint64_t& time) {
    if (name.size() < 5 || name.substr(name.size() - 4) != L".cdu") return false;
    time = 0;
    for (size_t i = 0; i + 4 < name.size(); ++i) {
        if (name[i] < L'0' || name[i] > L'9' || time > (UINT64_MAX - 9) / 10) return false;
        time = time * 10 + (uint64_t)(name[i] - L'0');
    }
    return name.size() > 4;
}

// All journals in `dir` with their times, oldest first.
std::vector<std::pair<uint64_t, std::wstring>> ListJournals(const std::wstring& dir) {
    std::vector<std::pair<uint64_t, std::wstring>> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + kPathSep + L"*.cdu").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        uint64_t time = 0;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && ParseName(fd.cFileName, time))
            out.emplace_back(time, dir + kPathSep + fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    SortObjects(out.begin(), out.end());
    return out;
}

}  // namespace

bool CaptureTagUndo(const std::wstring& path, const TagEdit& edit, TagUndoFile& out) {
    out = TagUndoFile{};
    out.path = path;
    TagData d;
    if (!ReadTags(path, d)) return false;
    // The TagData fields, in the same order as kText (lyrics, read separately, comes last).
    const std::string* text[kTextCount - 1] = {&d.title, &d.artist, &d.album, &d.albumArtist, &d.genre,
                                               &d.date,  &d.composer, &d.comment, &d.grouping, &d.label};
    for (int i = 0; i < kTextCount - 1; ++i)
        if ((edit.*kText[i]).has_value()) out.old.*kText[i] = *text[i];
    if (edit.lyrics) {
        std::string lyrics;
        ReadLyrics(path, lyrics);  // no lyrics leaves it empty
        out.old.lyrics = lyrics;
    }
    const uint32_t* num[kNumCount] = {&d.track, &d.trackTotal, &d.disc, &d.discTotal, &d.bpm};
    for (int i = 0; i < kNumCount; ++i)
        if ((edit.*kNum[i]).has_value()) out.old.*kNum[i] = *num[i];
    if (edit.compilation) out.old.compilation = d.compilation;
    if (edit.cover) {
        std::string image;
        ReadEmbeddedArt(path, image);  // no cover leaves it empty, which the undo restores as "none"
        if (image.size() <= kMaxCover) out.old.cover = image;
        else out.coverRestorable = false;
    }
    // ReplayGain values as the writer formats them; an absent value becomes "", which removes
    // the tag again on undo.
    auto gainText = [](float g) {
        if (!std::isfinite(g)) return std::string();
        char buf[32];
        snprintf(buf, sizeof buf, "%+.2f dB", g);
        return std::string(buf);
    };
    auto peakText = [](float p) {
        if (!std::isfinite(p) || p <= 0) return std::string();
        char buf[32];
        snprintf(buf, sizeof buf, "%.6f", p);
        return std::string(buf);
    };
    if (edit.rgTrackGain) out.old.rgTrackGain = gainText(d.rgTrackGain);
    if (edit.rgTrackPeak) out.old.rgTrackPeak = peakText(d.rgTrackPeak);
    if (edit.rgAlbumGain) out.old.rgAlbumGain = gainText(d.rgAlbumGain);
    if (edit.rgAlbumPeak) out.old.rgAlbumPeak = peakText(d.rgAlbumPeak);
    if (edit.r128TrackGain) out.old.r128TrackGain = d.r128TrackGain == INT32_MIN ? std::string() : std::to_string(d.r128TrackGain);
    if (edit.r128AlbumGain) out.old.r128AlbumGain = d.r128AlbumGain == INT32_MIN ? std::string() : std::to_string(d.r128AlbumGain);
    if (edit.rating) out.old.rating = d.rating;
    if (edit.playCount) out.old.playCount = d.playCount;
    return true;
}

bool SaveTagJournal(const std::wstring& dir, TagJournal& journal, std::wstring& error) {
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    auto existing = ListJournals(dir);
    if (!existing.empty()) journal.time = std::max(journal.time, existing.back().first + 1);
    std::wstring file = dir + kPathSep + std::to_wstring(journal.time) + L".cdu";
    std::wstring tmp = file + L".tmp";
    std::string data = Encode(journal);
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        error = L"Couldn't write the tag undo journal to " + dir;
        return false;
    }
    DWORD written = 0;
    bool ok = WriteFile(h, data.data(), (DWORD)data.size(), &written, nullptr) && written == data.size() &&
              FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        error = L"Couldn't write the tag undo journal to " + dir;
        return false;
    }
    existing.push_back({journal.time, file});
    for (size_t i = 0; existing.size() > kKeepJournals && i < existing.size() - kKeepJournals; ++i)
        DeleteFileW(existing[i].second.c_str());
    return true;
}

bool FindNewestTagJournal(const std::wstring& dir, std::wstring& file, uint32_t& fileCount) {
    fileCount = 0;
    auto journals = ListJournals(dir);
    if (journals.empty()) return false;
    file = journals.back().second;
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    unsigned char head[kHeaderBytes];
    DWORD got = 0;
    bool ok = ReadFile(h, head, sizeof(head), &got, nullptr) && got == sizeof(head);
    CloseHandle(h);
    if (!ok || std::memcmp(head, kMagic, 4) != 0) return false;
    fileCount = (uint32_t)head[12] | (uint32_t)head[13] << 8 | (uint32_t)head[14] << 16 | (uint32_t)head[15] << 24;
    return fileCount > 0;
}

bool LoadTagJournal(const std::wstring& file, TagJournal& journal) {
    std::string data;
    return ReadFileBytes(file, data) && Decode(data, journal);
}

void DeleteTagJournal(const std::wstring& file) { DeleteFileW(file.c_str()); }
