// Convert (convert.h): the plan, the tags carried over, the run with its replacements' undo logs, and
// the Recycle Bin both ways. The files themselves are written and checked by cord_export.cpp. No UI.
#include "convert.h"

#include "cord.h"
#include "decoder.h"
#include "tags.h"

#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cwctype>
#include <unordered_map>
#include <unordered_set>

namespace {

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool SamePath(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

std::string Upper(std::string_view s) {
    std::string r(s);
    for (char& c : r)
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    return r;
}

// A positive whole number, or 0 when the text is anything else.
uint32_t Number(std::string_view s) {
    if (s.empty() || s.size() > 9 || !std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; })) return 0;
    return (uint32_t)std::strtoul(std::string(s).c_str(), nullptr, 10);
}

void PutNumber(std::string& meta, uint8_t id, uint32_t v) {
    std::string bytes;
    cord::PutLe(bytes, v, 4);
    cord::PutEntry(meta, id, {}, bytes);
}

// One Vorbis comment as META: the fields cordinal shows become typed entries when their value
// fits the type exactly; anything else stays free-form under its own key, so nothing is lost.
void PutComment(std::string& meta, std::string_view rawKey, std::string_view value) {
    const std::string key = Upper(rawKey);
    static const struct { const char* key; uint8_t id; } kText[] = {
        {"TITLE", cord::kTitle}, {"ARTIST", cord::kArtist}, {"ALBUM", cord::kAlbum}, {"ALBUMARTIST", cord::kAlbumArtist},
        {"ALBUM ARTIST", cord::kAlbumArtist}, {"GENRE", cord::kGenre}, {"DATE", cord::kDate}, {"COMPOSER", cord::kComposer},
        {"COMMENT", cord::kComment}, {"GROUPING", cord::kGrouping}, {"LABEL", cord::kLabel}, {"LYRICS", cord::kLyrics},
    };
    for (const auto& field : kText)
        if (key == field.key) return cord::PutEntry(meta, field.id, {}, value);
    // "3/12" in a track or disc number carries its total too.
    auto pair = [&](uint8_t id, uint8_t totalId) {
        const size_t slash = value.find('/');
        const uint32_t n = Number(value.substr(0, slash)), total = slash == std::string_view::npos ? 0 : Number(value.substr(slash + 1));
        if (!n || (slash != std::string_view::npos && !total)) return false;
        PutNumber(meta, id, n);
        if (total) PutNumber(meta, totalId, total);
        return true;
    };
    if (key == "TRACKNUMBER" && pair(cord::kTrack, cord::kTrackTotal)) return;
    if (key == "DISCNUMBER" && pair(cord::kDisc, cord::kDiscTotal)) return;
    static const struct { const char* key; uint8_t id; } kNumbers[] = {
        {"TRACKTOTAL", cord::kTrackTotal}, {"TOTALTRACKS", cord::kTrackTotal}, {"DISCTOTAL", cord::kDiscTotal},
        {"TOTALDISCS", cord::kDiscTotal}, {"BPM", cord::kBpm},
    };
    for (const auto& field : kNumbers)
        if (key == field.key)
            if (const uint32_t n = Number(value)) return PutNumber(meta, field.id, n);
    if (key == "COMPILATION" && value == "1") return cord::PutEntry(meta, cord::kCompilation, {}, std::string_view("\x01", 1));
    static const struct { const char* key; uint8_t id; } kGains[] = {
        {"REPLAYGAIN_TRACK_GAIN", cord::kTrackGain}, {"REPLAYGAIN_TRACK_PEAK", cord::kTrackPeak},
        {"REPLAYGAIN_ALBUM_GAIN", cord::kAlbumGain}, {"REPLAYGAIN_ALBUM_PEAK", cord::kAlbumPeak},
    };
    for (const auto& field : kGains)
        if (key == field.key) {
            const std::string text(value);
            char* end = nullptr;
            const float v = std::strtof(text.c_str(), &end);
            while (end && *end == ' ') ++end;
            if (end != text.c_str() && std::isfinite(v) && (!*end || !_stricmp(end, "dB"))) {
                std::string bytes(4, '\0');
                memcpy(bytes.data(), &v, 4);
                return cord::PutEntry(meta, field.id, {}, bytes);
            }
        }
    if (key.size() <= 255) cord::PutEntry(meta, cord::kFreeForm, key, value);
}

uint32_t Be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

// FLAC's metadata blocks: every Vorbis comment and every picture, as they are stored.
bool FlacTags(const std::wstring& path, std::string& meta, std::vector<std::string>& pictures) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    auto read = [&](void* out, DWORD n) {
        DWORD got = 0;
        return ReadFile(f, out, n, &got, nullptr) && got == n;
    };
    uint8_t head[10];
    bool ok = read(head, 4);
    // An ID3v2 tag some taggers put in front of the stream is skipped.
    if (ok && !memcmp(head, "ID3", 3)) {
        ok = read(head + 4, 6);
        const uint32_t size = (uint32_t)(head[6] & 127) << 21 | (head[7] & 127) << 14 | (head[8] & 127) << 7 | (head[9] & 127);
        LARGE_INTEGER to{};
        to.QuadPart = 10 + (LONGLONG)size + ((head[5] & 0x10) ? 10 : 0);
        ok = ok && SetFilePointerEx(f, to, nullptr, FILE_BEGIN) && read(head, 4);
    }
    ok = ok && !memcmp(head, "fLaC", 4);
    for (bool last = !ok; !last;) {
        uint8_t block[4];
        if (!read(block, 4)) break;
        last = block[0] & 0x80;
        const unsigned type = block[0] & 0x7F;
        const uint32_t size = (uint32_t)block[1] << 16 | (uint32_t)block[2] << 8 | block[3];
        if (type != 4 && type != 6) {
            LARGE_INTEGER skip{};
            skip.QuadPart = size;
            if (!SetFilePointerEx(f, skip, nullptr, FILE_CURRENT)) break;
            continue;
        }
        std::string body(size, '\0');
        if (!read(body.data(), size)) break;
        const uint8_t* p = (const uint8_t*)body.data();
        if (type == 4) {  // Vorbis comments: little-endian lengths, "KEY=value" entries
            size_t at = 0;
            auto take = [&](uint32_t& v) {
                if (size - at < 4) return false;
                v = cord::Le32(p + at);
                at += 4;
                return true;
            };
            uint32_t vendor = 0, count = 0;
            if (!take(vendor) || vendor > size - at) continue;
            at += vendor;
            if (!take(count)) continue;
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t n = 0;
                if (!take(n) || n > size - at) break;
                const std::string_view entry(body.data() + at, n);
                at += n;
                const size_t eq = entry.find('=');
                if (eq != std::string_view::npos && eq) PutComment(meta, entry.substr(0, eq), entry.substr(eq + 1));
            }
        } else {  // a picture: big-endian type, MIME type, description, four sizes, then the image
            size_t at = 0;
            auto take = [&](uint32_t& v) {
                if (size - at < 4) return false;
                v = Be32(p + at);
                at += 4;
                return true;
            };
            uint32_t pictureType = 0, mime = 0, described = 0, length = 0, ignored = 0;
            if (!take(pictureType) || !take(mime) || mime > size - at) continue;
            const std::string_view mimeText(body.data() + at, mime);
            at += mime;
            if (!take(described) || described > size - at) continue;
            const std::string_view description(body.data() + at, described);
            at += described;
            if (!take(ignored) || !take(ignored) || !take(ignored) || !take(ignored) || !take(length) || length > size - at) continue;
            pictures.push_back(cord::PictureBytes(std::string_view(body.data() + at, length), (uint8_t)pictureType, mimeText, description));
        }
    }
    CloseHandle(f);
    return ok;
}

std::wstring Now() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t s[32];
    swprintf_s(s, L"%04u%02u%02u-%02u%02u%02u-%03u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    return s;
}

// The logs in `dir`, oldest first: their names start with the time they were written.
std::vector<std::wstring> Logs(const std::wstring& dir) {
    std::vector<std::wstring> logs;
    WIN32_FIND_DATAW d;
    HANDLE h = FindFirstFileW((dir + kPathSep + L"*.log").c_str(), &d);
    if (h == INVALID_HANDLE_VALUE) return logs;
    do logs.push_back(dir + kPathSep + d.cFileName);
    while (FindNextFileW(h, &d));
    FindClose(h);
    std::sort(logs.begin(), logs.end());
    return logs;
}

struct LogEntry {
    std::wstring cord, original;
    bool recycled = false;
};

std::vector<LogEntry> ReadLog(const std::wstring& path) {
    std::vector<LogEntry> entries;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return entries;
    LARGE_INTEGER size{};
    std::string text;
    if (GetFileSizeEx(f, &size) && size.QuadPart < (1 << 26)) {
        text.resize((size_t)size.QuadPart);
        DWORD got = 0;
        if (!ReadFile(f, text.data(), (DWORD)text.size(), &got, nullptr)) got = 0;
        text.resize(got);
    }
    CloseHandle(f);
    for (size_t start = 0; start < text.size();) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string_view line(text.data() + start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        const size_t a = line.find('\t'), b = a == std::string_view::npos ? a : line.find('\t', a + 1);
        if (b == std::string_view::npos) continue;
        entries.push_back({FromUtf8(line.substr(0, a)), FromUtf8(line.substr(a + 1, b - a - 1)), line.substr(b + 1) == "1"});
    }
    return entries;
}

std::string LogLine(const LogEntry& e) { return ToUtf8(e.cord) + "\t" + ToUtf8(e.original) + "\t" + (e.recycled ? "1" : "0") + "\r\n"; }

// Replaces a log with what is left of it, through a temp file; an empty one is removed.
void RewriteLog(const std::wstring& path, const std::vector<LogEntry>& left) {
    if (left.empty()) {
        DeleteFileW(path.c_str());
        return;
    }
    std::string text;
    for (const LogEntry& e : left) text += LogLine(e);
    const std::wstring temp = path + L".tmp";
    HANDLE f = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    const bool ok = WriteFile(f, text.data(), (DWORD)text.size(), &written, nullptr) && written == text.size() && FlushFileBuffers(f);
    CloseHandle(f);
    if (!ok || !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) DeleteFileW(temp.c_str());
}

}  // namespace

bool ReadsExactly(const std::wstring& path, Codec codec) {
    const std::wstring ext = ExtOf(path);
    for (const wchar_t* exact : {L"flac", L"fla", L"wav", L"wave", L"aif", L"aiff", L"aifc", L"au", L"snd", L"cord", L"wv", L"ape", L"tta"})
        if (ext == exact) return true;
    // MP4 holds ALAC or AAC, and CAF PCM, ALAC or lossy codecs.
    if (ext == L"m4a" || ext == L"mp4" || ext == L"m4b") return codec == Codec::Unknown || codec == Codec::ALAC;
    if (ext == L"caf") return codec == Codec::Unknown || codec == Codec::ALAC || codec == Codec::PCM;
    return false;
}

namespace {

std::wstring FolderOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring Folded(std::wstring s) {
    for (wchar_t& c : s) c = (wchar_t)std::towupper(c);
    return s;
}

// The names in each folder an output may go to, listed once a folder: a library's worth of outputs
// is far quicker to check this way than file by file. A folder that doesn't exist lists nothing.
class FolderNames {
public:
    bool Has(const std::wstring& path) {
        const std::wstring folder = FolderOf(path);
        if (folder.empty()) return Exists(path);
        auto [at, added] = names_.try_emplace(Folded(folder));
        if (added) {
            WIN32_FIND_DATAW found{};
            HANDLE h = FindFirstFileW((folder + kPathSep + L"*").c_str(), &found);
            if (h != INVALID_HANDLE_VALUE) {
                do at->second.insert(Folded(found.cFileName));
                while (FindNextFileW(h, &found));
                FindClose(h);
            }
        }
        return at->second.count(Folded(path.substr(folder.size() + 1))) != 0;
    }

private:
    std::unordered_map<std::wstring, std::unordered_set<std::wstring>> names_;
};

// The folder every source sits under; when they all share one folder, its parent, so that folder's
// name comes along into the destination.
std::wstring CommonRoot(const std::vector<std::pair<TrackId, std::wstring>>& tracks) {
    std::wstring root;
    bool first = true, one = true;
    for (const auto& track : tracks) {
        const std::wstring dir = FolderOf(track.second);
        if (first) { root = dir; first = false; continue; }
        if (SamePath(dir, root)) continue;
        one = false;
        while (!root.empty() && !(dir.size() >= root.size() && SamePath(dir.substr(0, root.size()), root) &&
                                  (dir.size() == root.size() || dir[root.size()] == L'\\' || dir[root.size()] == L'/')))
            root = FolderOf(root);
    }
    return one ? FolderOf(root) : root;
}

}  // namespace

std::vector<ConvertItem> PlanConvert(const std::vector<std::pair<TrackId, std::wstring>>& tracks, CordExport format,
                                     ConvertMode mode, const std::wstring& folder, const std::function<Codec(TrackId)>& codec) {
    std::vector<ConvertItem> plan;
    if (format >= CordExport::Count) return plan;
    const std::wstring target = kCordExports[(unsigned)format].extension;
    const bool lossless = !kCordExports[(unsigned)format].kbps;
    const bool replace = mode == ConvertMode::Replace || (mode == ConvertMode::Auto && format == CordExport::None);
    std::wstring dest = replace ? std::wstring() : folder, root;
    while (!dest.empty() && (dest.back() == L'\\' || dest.back() == L'/')) dest.pop_back();
    if (!dest.empty()) root = CommonRoot(tracks);
    std::unordered_set<std::wstring> planned;
    FolderNames existing;
    plan.reserve(tracks.size());
    for (const auto& [id, path] : tracks) {
        ConvertItem item;
        item.id = id;
        item.from = path;
        item.format = format;
        item.replace = replace;
        const std::wstring ext = ExtOf(path);
        const size_t dot = path.find_last_of(L'.');
        const bool same = ext == target || (format == CordExport::Flac && ext == L"fla") ||
                          (format == CordExport::Wav && ext == L"wave");
        if (path.find(L'|') != std::wstring::npos) item.skip = L"part of a cue sheet";
        else if (same) item.skip = L"already ." + target;
        else if (lossless && !ReadsExactly(path, codec ? codec(id) : Codec::Unknown))
            item.skip = L"not lossless audio cordinal can read exactly";
        else if (dest.empty()) item.to = path.substr(0, dot) + L"." + target;
        else {
            // Below the common folder; a source elsewhere (no common folder at all) keeps only its name.
            std::wstring rest = path.substr(0, dot);
            if (!root.empty() && rest.size() > root.size() && SamePath(rest.substr(0, root.size()), root)) rest.erase(0, root.size());
            else rest = rest.substr(rest.find_last_of(L"\\/") + 1);
            while (!rest.empty() && (rest.front() == L'\\' || rest.front() == L'/')) rest.erase(0, 1);
            item.to = dest + kPathSep + rest + L"." + target;
        }
        if (!item.to.empty()) {
            // Outputs already planned, compared as SamePath does (ignoring case) through a set: the
            // Convert window replans the whole library on every change.
            if (existing.Has(item.to) || !planned.insert(Folded(item.to)).second)
                item.skip = L"a ." + std::wstring(kCordExports[(unsigned)format].extension) + L" file of that name already exists";
        }
        plan.push_back(std::move(item));
    }
    return plan;
}

std::string CordTagsFrom(const std::wstring& source, std::vector<std::string>& pictures) {
    std::string meta;
    pictures.clear();
    const std::wstring ext = ExtOf(source);
    if ((ext == L"flac" || ext == L"fla") && FlacTags(source, meta, pictures)) return meta;
    meta.clear();
    pictures.clear();
    TagData t;
    ReadTags(source, t);
    for (const auto& [id, value] : {std::pair<uint8_t, const std::string*>{cord::kTitle, &t.title}, {cord::kArtist, &t.artist},
                                    {cord::kAlbum, &t.album}, {cord::kAlbumArtist, &t.albumArtist}, {cord::kGenre, &t.genre},
                                    {cord::kDate, &t.date}, {cord::kComposer, &t.composer}, {cord::kComment, &t.comment},
                                    {cord::kGrouping, &t.grouping}, {cord::kLabel, &t.label}})
        if (!value->empty()) cord::PutEntry(meta, id, {}, *value);
    for (const auto& [id, value] : {std::pair<uint8_t, uint32_t>{cord::kTrack, t.track}, {cord::kTrackTotal, t.trackTotal},
                                    {cord::kDisc, t.disc}, {cord::kDiscTotal, t.discTotal}, {cord::kBpm, t.bpm}})
        if (value) PutNumber(meta, id, value);
    if (t.compilation) cord::PutEntry(meta, cord::kCompilation, {}, std::string_view("\x01", 1));
    for (const auto& [id, value] : {std::pair<uint8_t, float>{cord::kTrackGain, t.rgTrackGain}, {cord::kTrackPeak, t.rgTrackPeak},
                                    {cord::kAlbumGain, t.rgAlbumGain}, {cord::kAlbumPeak, t.rgAlbumPeak}})
        if (std::isfinite(value)) {
            std::string bytes(4, '\0');
            memcpy(bytes.data(), &value, 4);
            cord::PutEntry(meta, id, {}, bytes);
        }
    std::string text;
    if (ReadLyrics(source, text) && !text.empty()) cord::PutEntry(meta, cord::kLyrics, {}, text);
    if (ReadEmbeddedArt(source, text) && !text.empty()) pictures.push_back(cord::PictureBytes(text));
    return meta;
}

ConvertResult ExecuteConvert(const std::vector<ConvertItem>& plan, const ConvertSettings& settings, const std::wstring& logDir,
                             const std::atomic<bool>& cancel, const std::function<void(size_t)>& progress) {
    ConvertResult result;
    HANDLE log = INVALID_HANDLE_VALUE;
    // The log is opened before the first original can leave its folder: a conversion that can't be
    // recorded is not undone later, so its original is kept instead of recycled.
    auto openLog = [&]() -> bool {
        if (log != INVALID_HANDLE_VALUE) return true;
        if (!result.logError.empty()) return false;
        CreateDirectoryW(logDir.c_str(), nullptr);
        result.log = logDir + kPathSep + Now() + L".log";
        log = CreateFileW(result.log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, 0, nullptr);
        if (log == INVALID_HANDLE_VALUE) {
            result.log.clear();
            result.logError = L"the undo log couldn't be written";
            return false;
        }
        return true;
    };
    auto record = [&](const ConvertDone& done, bool recycled) -> bool {
        const std::string line = LogLine({done.to, done.from, recycled});
        DWORD written = 0;
        return WriteFile(log, line.data(), (DWORD)line.size(), &written, nullptr) && written == line.size() &&
               FlushFileBuffers(log);
    };
    for (size_t i = 0; i < plan.size(); ++i) {
        const ConvertItem& item = plan[i];
        if (!item.skip.empty()) continue;
        if (cancel) {
            result.cancelled = true;
            break;
        }
        if (progress) progress(i);
        std::wstring error;
        // A copy's folders below the destination are made as needed.
        for (size_t at = item.to.find_first_of(L"\\/", 3); at != std::wstring::npos; at = item.to.find_first_of(L"\\/", at + 1))
            CreateDirectoryW(item.to.substr(0, at).c_str(), nullptr);
        // The new file is read back and checked before it appears (cord_export.cpp); only then does
        // anything happen to the original.
        if (!ExportAudio(item.from, item.to, item.format, error, cancel, settings.threads)) {
            if (cancel) {
                result.cancelled = true;
                break;
            }
            result.failed.push_back({item.from, error});
            continue;
        }
        if (!item.replace) {
            result.done.push_back({item.id, item.from, item.to, false});
            continue;
        }
        const bool wantRecycle = settings.recycle;
        bool recycled = false;
        if (openLog()) {
            // Recorded first, claiming the recycle: if the recycle then fails the original is still
            // there, and Undo sees it and only removes the .cord.
            if (record({item.id, item.from, item.to, wantRecycle}, wantRecycle)) {
                if (wantRecycle) recycled = RecycleFile(item.from);
            } else {
                result.logError = L"the undo log couldn't be written";
                CloseHandle(log);
                log = INVALID_HANDLE_VALUE;
                result.log.clear();
            }
        }
        result.done.push_back({item.id, item.from, item.to, recycled});
    }
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    return result;
}

std::vector<ConvertUndone> UndoLastConvert(const std::wstring& logDir, bool recycle, std::wstring& error) {
    std::vector<ConvertUndone> undone;
    const std::vector<std::wstring> logs = Logs(logDir);
    if (logs.empty()) {
        error = L"There's no conversion left to undo.";
        return undone;
    }
    const std::wstring& path = logs.back();
    std::vector<LogEntry> left;
    size_t lost = 0;
    for (const LogEntry& e : ReadLog(path)) {
        if (!Exists(e.original) && !(e.recycled && RestoreFromRecycleBin(e.original))) {
            if (Exists(e.cord)) left.push_back(e);  // it can still be tried later, the .cord holds the audio
            else ++lost;
            continue;
        }
        undone.push_back({e.cord, e.original, Exists(e.cord) && recycle && RecycleFile(e.cord)});
    }
    RewriteLog(path, left);
    if (!left.empty())
        error = std::to_wstring(left.size()) + (left.size() == 1 ? L" original couldn't" : L" originals couldn't") +
                L" be brought back from the Recycle Bin; their .cord files hold the same audio.";
    else if (lost)
        error = std::to_wstring(lost) + L" file(s) of that conversion were gone, original and .cord.";
    return undone;
}

bool CanUndoConvert(const std::wstring& logDir) { return !Logs(logDir).empty(); }

void PruneConvertLogs(const std::wstring& logDir, unsigned days) {
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    const uint64_t limit = ((uint64_t)now.dwHighDateTime << 32 | now.dwLowDateTime) - (uint64_t)days * 864000000000ull;
    for (const std::wstring& log : Logs(logDir)) {
        WIN32_FILE_ATTRIBUTE_DATA a{};
        if (GetFileAttributesExW(log.c_str(), GetFileExInfoStandard, &a) &&
            ((uint64_t)a.ftCreationTime.dwHighDateTime << 32 | a.ftCreationTime.dwLowDateTime) < limit)
            DeleteFileW(log.c_str());
    }
}

namespace {
std::wstring Songs(size_t n) { return std::to_wstring(n) + (n == 1 ? L" song" : L" songs"); }
}  // namespace

std::wstring ConvertSummary(const std::vector<ConvertItem>& plan) {
    size_t ready = 0;
    std::vector<std::pair<std::wstring, size_t>> reasons;
    for (const ConvertItem& item : plan) {
        if (item.skip.empty()) { ++ready; continue; }
        auto it = std::find_if(reasons.begin(), reasons.end(), [&](const auto& r) { return r.first == item.skip; });
        if (it == reasons.end()) reasons.push_back({item.skip, 1});
        else ++it->second;
    }
    std::stable_sort(reasons.begin(), reasons.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::wstring s = ready ? Songs(ready) + L" to convert" : L"Nothing to convert";
    if (!reasons.empty()) {
        s += L"  \x2022  " + std::to_wstring(plan.size() - ready) + L" won't: ";
        for (size_t i = 0; i < reasons.size() && i < 3; ++i)
            s += (i ? L", " : L"") + reasons[i].first + (reasons.size() > 1 ? L" (" + std::to_wstring(reasons[i].second) + L")" : L"");
        if (reasons.size() > 3) s += L"...";
    }
    return s;
}

std::wstring ReplaceQuestion(const std::vector<ConvertItem>& plan, bool recycle, bool showOriginals, unsigned days) {
    size_t ready = 0, lossless = 0;
    CordExport format = CordExport::None;
    for (const ConvertItem& item : plan)
        if (item.skip.empty()) {
            ++ready;
            format = item.format;
            lossless += ReadsExactly(item.from);
        }
    const CordExportInfo& info = kCordExports[(unsigned)format];
    std::wstring q = L"Replace " + Songs(ready) + L" with " + info.label + L"?\n\nEach new file is read back and checked" +
                     (info.kbps ? L"" : L", sample for sample,") + L" before the library switches to it, keeping its plays, "
                     L"ratings and playlists. ";
    q += recycle ? L"The originals then go to the Recycle Bin."
                 : showOriginals ? L"The originals stay where they are, and in the library."
                                 : L"The originals stay where they are, left out of the library.";
    q += L"\nLibrary > Undo last conversion brings them back for " +
         (days == 1 ? std::wstring(L"a day") : days % 7 ? std::to_wstring(days) + L" days"
                      : days == 7 ? std::wstring(L"a week") : std::to_wstring(days / 7) + L" weeks") + L".";
    if (info.kbps && lossless)
        q += L"\n\n" + Songs(lossless) + (lossless == 1 ? L" is" : L" are") + L" lossless now. " + info.label +
             L" can't hold all of the original sound, so keep the originals (or a backup) if you may want it back later.";
    return q;
}

std::wstring ConvertDoneText(const ConvertResult& r, CordExport format, bool replaced, bool recycle) {
    std::wstring done = r.done.empty() ? std::wstring(L"Nothing was converted")
                                       : (replaced ? L"Converted " : L"Made ") + Songs(r.done.size()) + (replaced ? L" into " : L" as ") +
                                             kCordExports[(unsigned)format].label;
    done += r.cancelled ? L"; the rest was cancelled." : L".";
    if (!replaced && !r.done.empty()) done += L" The originals were kept.";
    if (replaced && recycle) {
        const size_t kept = (size_t)std::count_if(r.done.begin(), r.done.end(), [](const ConvertDone& d) { return !d.recycled; });
        if (kept)
            done += L" " + Songs(kept) + (kept == 1 ? L" was" : L" were") + L" kept: " +
                    (r.logError.empty() ? L"the Recycle Bin wasn't available there." : r.logError + L".");
    }
    return done;
}
