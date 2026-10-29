// CUE sheet support (cue.h): parses .cue text and the sheets embedded in FLAC files, and answers
// "which sheet covers this file" for a folder. A sheet track is addressed as "file|N", which
// OpenTrackDecoder turns into a range-limited decoder over the real audio file.
#include "sort.h"
#include "cue.h"

#include "decoder.h"
#include "game_music.h"
#include "library.h"

#include <algorithm>
#include <cwctype>
#include <mutex>

namespace {

// A sheet that renumbers its way past this is garbage, not an album.
constexpr int kMaxCueTracks = 999;

constexpr size_t kMaxCueBytes = 1024 * 1024;
constexpr size_t kMaxFlacBlockBytes = 1024 * 1024;  // bigger metadata blocks are not read
constexpr int kMaxFlacBlocks = 256;
constexpr size_t kCacheEntries = 16;
constexpr size_t kNone = SIZE_MAX;

using sv = std::string_view;

sv Trim(sv s) {
    size_t a = s.find_first_not_of(" \t\r");
    if (a == sv::npos) return {};
    size_t b = s.find_last_not_of(" \t\r");
    return s.substr(a, b - a + 1);
}

bool EqualNoCase(sv a, sv b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'a' && x <= 'z') x = (char)(x - 32);
        if (y >= 'a' && y <= 'z') y = (char)(y - 32);
        if (x != y) return false;
    }
    return true;
}

// The contents of a quoted value, or the whole trimmed text when it isn't quoted.
sv Unquote(sv s) {
    s = Trim(s);
    if (!s.empty() && s[0] == '"') {
        size_t close = s.find('"', 1);
        return close == sv::npos ? s.substr(1) : s.substr(1, close - 1);
    }
    return s;
}

bool ParseNumber(sv s, int maxValue, int& out) {
    if (s.empty() || s.size() > 6) return false;
    int n = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        n = n * 10 + (c - '0');
    }
    if (n > maxValue) return false;
    out = n;
    return true;
}

// "mm:ss:ff" with 75 frames a second.
bool ParseMsf(sv s, double& seconds) {
    int v[3] = {0, 0, 0};
    s = Trim(s);
    for (int i = 0; i < 3; ++i) {
        size_t cut = s.find(':');
        if (i < 2 && cut == sv::npos) return false;
        sv part = i < 2 ? s.substr(0, cut) : s;
        if (part.empty() || part.size() > 7) return false;
        int n = 0;
        for (char c : part) {
            if (c < '0' || c > '9') return false;
            n = n * 10 + (c - '0');
        }
        v[i] = n;
        if (i < 2) s.remove_prefix(cut + 1);
    }
    if (v[1] > 59 || v[2] > 74) return false;
    seconds = v[0] * 60.0 + v[1] + v[2] / 75.0;
    return true;
}

uint64_t ReadBE(sv b, size_t at, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) v = v << 8 | (uint8_t)b[at + i];
    return v;
}

uint32_t ReadLE32(sv b, size_t at) {
    return (uint32_t)(uint8_t)b[at] | (uint32_t)(uint8_t)b[at + 1] << 8 | (uint32_t)(uint8_t)b[at + 2] << 16 |
           (uint32_t)(uint8_t)b[at + 3] << 24;
}

// Whole-track times: a track ends where the next one starts, the last one runs to the end of its file.
void SetEnds(CueSheet::File& file) {
    for (size_t i = 0; i < file.tracks.size(); ++i)
        file.tracks[i].end = i + 1 < file.tracks.size() ? file.tracks[i + 1].start : 0;
}

// Reads a FILE name. A name that isn't on disk is replaced by a same-stem audio file next to it,
// so a cue naming "Album.wav" finds "Album.flac".
bool FileExistsW(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring ResolveAudio(const std::wstring& name, const std::wstring& cueDir) {
    std::wstring rel = name;
    std::replace(rel.begin(), rel.end(), kPathSep == L'/' ? L'\\' : L'/', kPathSep);  // sheets made on either system
    bool absolute = (rel.size() >= 2 && rel[1] == L':') || (!rel.empty() && rel[0] == kPathSep);
    std::wstring full = absolute || cueDir.empty() ? rel : cueDir + (cueDir.back() == kPathSep ? L"" : std::wstring(1, kPathSep)) + rel;
    if (FileExistsW(full)) return full;
    size_t slash = full.find_last_of(kPathSep);
    size_t dot = full.find_last_of(L'.');
    if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash)) {
        std::wstring stem = full.substr(0, dot);
        for (const wchar_t* ext : {L"flac", L"wav", L"ape", L"wv", L"aiff", L"aif", L"mp3", L"m4a", L"ogg", L"opus", L"wma", L"cord"}) {
            std::wstring candidate = stem + L"." + ext;
            if (FileExistsW(candidate)) return candidate;
        }
    }
    return full;
}

// Lower-case absolute path, the key for comparing files.
std::wstring Canon(const std::wstring& path) {
    std::wstring full(MAX_PATH * 2, L'\0');
    DWORD n = GetFullPathNameW(path.c_str(), (DWORD)full.size(), full.data(), nullptr);
    if (n == 0 || n >= full.size()) full = path;
    else full.resize(n);
    for (auto& c : full) c = (wchar_t)towlower(c);
    return full;
}

uint64_t WriteTimeOf(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return 0;
    return (uint64_t)d.ftLastWriteTime.dwHighDateTime << 32 | d.ftLastWriteTime.dwLowDateTime;
}

// Random access to a file, the way the tag reader reads it.
class FileBytes {
public:
    explicit FileBytes(const std::wstring& path) {
        h_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        LARGE_INTEGER sz{};
        if (h_ != INVALID_HANDLE_VALUE && GetFileSizeEx(h_, &sz)) size_ = (uint64_t)sz.QuadPart;
    }
    ~FileBytes() {
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
    }
    bool Ok() const { return h_ != INVALID_HANDLE_VALUE; }
    uint64_t Size() const { return size_; }
    std::string Read(uint64_t offset, size_t n) const {
        if (offset >= size_) return {};
        n = (size_t)std::min<uint64_t>(n, size_ - offset);
        std::string buf(n, '\0');
        OVERLAPPED ov{};
        ov.Offset = (DWORD)offset;
        ov.OffsetHigh = (DWORD)(offset >> 32);
        DWORD got = 0;
        if (!ReadFile(h_, buf.data(), (DWORD)n, &got, &ov)) got = 0;
        buf.resize(got);
        return buf;
    }

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
    uint64_t size_ = 0;
};

// Text of a .cue file as UTF-8.
std::string CueText(const std::string& raw) {
    auto byte = [&](size_t i) { return (uint8_t)raw[i]; };
    if (raw.size() >= 3 && raw.compare(0, 3, "\xEF\xBB\xBF") == 0) return raw.substr(3);
    if (raw.size() >= 2 && ((byte(0) == 0xFF && byte(1) == 0xFE) || (byte(0) == 0xFE && byte(1) == 0xFF))) {
        bool bigEndian = byte(0) == 0xFE;
        std::wstring wide((raw.size() - 2) / 2, L'\0');
        for (size_t i = 0; i < wide.size(); ++i) {
            size_t a = 2 + 2 * i, b = a + 1;
            wide[i] = (wchar_t)(bigEndian ? byte(a) << 8 | byte(b) : byte(b) << 8 | byte(a));
        }
        return ToUtf8(wide);
    }
    if (raw.empty() || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.data(), (int)raw.size(), nullptr, 0) > 0)
        return raw;
    int n = MultiByteToWideChar(CP_ACP, 0, raw.data(), (int)raw.size(), nullptr, 0);
    if (n <= 0) return raw;
    std::wstring wide(n, L'\0');
    MultiByteToWideChar(CP_ACP, 0, raw.data(), (int)raw.size(), wide.data(), n);
    return ToUtf8(wide);
}

// The binary CUESHEET metadata block of a FLAC (the layout is in the FLAC format description).
// Track offsets and index offsets are in samples; `rate` converts them.
bool ParseBinaryCue(sv b, uint32_t rate, CueSheet& out) {
    constexpr size_t kHeader = 396, kTrack = 36, kIndex = 12;
    if (rate == 0 || b.size() < kHeader) return false;
    out = CueSheet{};
    sv catalog = b.substr(0, 128);
    out.catalog = std::string(Trim(catalog.substr(0, catalog.find('\0'))));
    size_t count = (uint8_t)b[395], p = kHeader;
    CueSheet::File file;
    int lastNumber = 0;
    for (size_t i = 0; i < count && p + kTrack <= b.size(); ++i) {
        uint64_t offset = ReadBE(b, p, 8);
        int number = (uint8_t)b[p + 8];
        bool nonAudio = (uint8_t)b[p + 21] & 0x80;
        size_t indexes = (uint8_t)b[p + 35];
        std::string isrc(Trim(b.substr(p + 9, 12)));
        p += kTrack;
        uint64_t index1 = 0;
        bool found = false;
        for (size_t j = 0; j < indexes && p + kIndex <= b.size(); ++j, p += kIndex) {
            if ((uint8_t)b[p + 8] == 1 && !found) {
                index1 = ReadBE(b, p, 8);
                found = true;
            }
        }
        // 170 is the lead-out, and numbers past 99 aren't valid cue tracks.
        if (number < 1 || number > 99 || nonAudio || !found) continue;
        if (number <= lastNumber) number = lastNumber + 1;  // see ParseCue: numbers must rise
        if (number > kMaxCueTracks) continue;
        lastNumber = number;
        CueTrack t;
        t.number = number;
        t.isrc = isrc;
        t.start = (double)(offset + index1) / rate;
        file.tracks.push_back(std::move(t));
    }
    if (file.tracks.empty()) return false;
    SetEnds(file);
    out.files.push_back(std::move(file));
    return true;
}

// The CUESHEET entry of a Vorbis comment block (the comments are UTF-8).
bool FindVorbisCue(sv body, std::string& text) {
    if (body.size() < 4) return false;
    size_t p = 4 + (size_t)ReadLE32(body, 0);
    if (p + 4 > body.size()) return false;
    uint32_t count = ReadLE32(body, p);
    p += 4;
    for (uint32_t i = 0; i < count && p + 4 <= body.size(); ++i) {
        uint32_t len = ReadLE32(body, p);
        p += 4;
        if (len > body.size() - p) break;
        sv entry = body.substr(p, len);
        p += len;
        if (entry.size() >= 9 && EqualNoCase(entry.substr(0, 9), "CUESHEET=")) {
            text.assign(entry.substr(9));
            return true;
        }
    }
    return false;
}

// Parsed sheets, keyed by .cue path (or "embedded:" + audio path) and write time. Consecutive
// tracks of one album hit this instead of parsing the sheet again.
struct CacheEntry {
    std::wstring key;
    uint64_t time = 0, used = 0;
    std::shared_ptr<const CueSheet> sheet;  // null: the file had no usable sheet
};
std::mutex g_cacheMx;
std::vector<CacheEntry> g_cache;
uint64_t g_cacheUse = 0;

template <class Load>
std::shared_ptr<const CueSheet> CachedSheet(const std::wstring& key, uint64_t time, Load load) {
    {
        std::lock_guard<std::mutex> lock(g_cacheMx);
        for (auto& e : g_cache)
            if (e.time == time && e.key == key) {
                e.used = ++g_cacheUse;
                return e.sheet;
            }
    }
    auto sheet = std::make_shared<CueSheet>();
    std::shared_ptr<const CueSheet> result = load(*sheet) ? std::shared_ptr<const CueSheet>(sheet) : nullptr;
    std::lock_guard<std::mutex> lock(g_cacheMx);
    CacheEntry entry{key, time, ++g_cacheUse, result};
    if (g_cache.size() < kCacheEntries) {
        g_cache.push_back(std::move(entry));
    } else {
        auto oldest = std::min_element(g_cache.begin(), g_cache.end(),
                                       [](const CacheEntry& a, const CacheEntry& b) { return a.used < b.used; });
        *oldest = std::move(entry);
    }
    return result;
}

}  // namespace

// ---------------------------------------------------------------------------
// Parsing

// Parses the textual cue grammar one line at a time. Unknown keywords, malformed lines and
// cue comments are skipped, so a partly broken sheet still yields the tracks it got right. Track
// numbers are forced to rise (see the TRACK branch) because they double as a track's identity.
bool ParseCue(std::string_view text, const std::wstring& cueDir, CueSheet& out) {
    out = CueSheet{};
    if (text.size() > kMaxCueBytes) return false;
    if (text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
    size_t fileIdx = kNone, trackIdx = kNone;
    bool anyTrack = false, hasIndex1 = false;
    int lastNumber = 0;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == sv::npos) eol = text.size();
        sv line = Trim(text.substr(pos, eol - pos));
        pos = eol + 1;
        size_t space = line.find_first_of(" \t");
        sv keyword = line.substr(0, space);
        sv rest = space == sv::npos ? sv() : Trim(line.substr(space));
        sv firstWord = rest.substr(0, rest.find_first_of(" \t"));
        sv afterWord = rest.size() > firstWord.size() ? Trim(rest.substr(firstWord.size())) : sv();

        if (EqualNoCase(keyword, "REM")) {
            if (EqualNoCase(firstWord, "DATE")) out.date = std::string(Unquote(afterWord));
            else if (EqualNoCase(firstWord, "GENRE")) out.genre = std::string(Unquote(afterWord));
            else if (EqualNoCase(firstWord, "COMMENT")) out.comment = std::string(Unquote(afterWord));
            else if (EqualNoCase(firstWord, "DISCNUMBER")) ParseNumber(Unquote(afterWord), 999, out.discNumber);
            else if (EqualNoCase(firstWord, "TOTALDISCS")) ParseNumber(Unquote(afterWord), 999, out.totalDiscs);
        } else if (EqualNoCase(keyword, "FILE")) {
            // FILE "name" TYPE, or FILE name TYPE (the name then ends before the last word).
            sv name;
            if (!rest.empty() && rest[0] == '"') {
                name = Unquote(rest);
            } else {
                size_t last = rest.find_last_of(" \t");
                name = Trim(last == sv::npos ? rest : rest.substr(0, last));
            }
            trackIdx = kNone;
            if (name.empty()) {
                fileIdx = kNone;
                continue;
            }
            out.files.push_back({ResolveAudio(FromUtf8(name), cueDir), {}});
            fileIdx = out.files.size() - 1;
        } else if (EqualNoCase(keyword, "TRACK")) {
            anyTrack = true;
            trackIdx = kNone;
            hasIndex1 = false;
            if (fileIdx == kNone) continue;
            sv type = afterWord;
            if (!EqualNoCase(Trim(type.substr(0, type.find_first_of(" \t"))), "AUDIO")) {
                // "TRACK 02 MODE1/2352" and the like are not songs
                continue;
            }
            int number = 0;
            if (!ParseNumber(firstWord, 99, number) || number < 1) continue;
            // A track's number is its identity ("file|N"), so it must be unique. Hand-made sheets
            // repeat numbers or restart them for every FILE: such a track takes the next free
            // number instead of being dropped.
            if (number <= lastNumber) number = lastNumber + 1;
            if (number > kMaxCueTracks) continue;
            lastNumber = number;
            auto& tracks = out.files[fileIdx].tracks;
            tracks.push_back(CueTrack{});
            tracks.back().number = number;
            trackIdx = tracks.size() - 1;
        } else if (EqualNoCase(keyword, "TITLE") || EqualNoCase(keyword, "PERFORMER") ||
                   EqualNoCase(keyword, "SONGWRITER") || EqualNoCase(keyword, "ISRC")) {
            // Before the first TRACK these describe the disc; inside a track, that track.
            bool disc = !anyTrack;
            if (!disc && trackIdx == kNone) continue;
            sv value = Unquote(rest);
            if (disc) {
                if (EqualNoCase(keyword, "TITLE")) out.title = std::string(value);
                else if (EqualNoCase(keyword, "PERFORMER")) out.performer = std::string(value);
                continue;
            }
            CueTrack& t = out.files[fileIdx].tracks[trackIdx];
            if (EqualNoCase(keyword, "TITLE")) t.title = std::string(value);
            else if (EqualNoCase(keyword, "PERFORMER")) t.performer = std::string(value);
            else if (EqualNoCase(keyword, "SONGWRITER")) t.songwriter = std::string(value);
            else t.isrc = std::string(value);
        } else if (EqualNoCase(keyword, "INDEX")) {
            int index = 0;
            double seconds = 0;
            if (trackIdx == kNone || !ParseNumber(firstWord, 99, index) || !ParseMsf(afterWord, seconds)) continue;
            CueTrack& t = out.files[fileIdx].tracks[trackIdx];
            if (index == 1) {
                t.start = seconds;
                hasIndex1 = true;
            } else if (index == 0 && !hasIndex1) {
                t.start = seconds;  // a track without INDEX 01 starts at its pregap
            }
        } else if (EqualNoCase(keyword, "CATALOG")) {
            out.catalog = std::string(Unquote(rest));
        }
    }
    for (auto& file : out.files) SetEnds(file);
    return out.TrackCount() > 0;
}

bool LoadCueFile(const std::wstring& cuePath, CueSheet& out) {
    out = CueSheet{};
    FileBytes f(cuePath);
    if (!f.Ok() || f.Size() > kMaxCueBytes) return false;
    return ParseCue(CueText(f.Read(0, (size_t)f.Size())), DirectoryOf(cuePath), out);
}

bool ReadEmbeddedCue(const std::wstring& audioPath, CueSheet& out) {
    out = CueSheet{};
    FileBytes f(audioPath);
    if (!f.Ok() || f.Read(0, 4) != "fLaC") return false;
    uint32_t rate = 0;
    std::string text, binary;
    bool haveText = false;
    uint64_t pos = 4;
    // Walk the metadata blocks after "fLaC": STREAMINFO (0) gives the sample rate, VORBIS_COMMENT
    // (4) may hold a CUESHEET= tag, CUESHEET (5) is the binary sheet. The first of the latter two
    // that carries a sheet wins. kMaxFlacBlocks bounds a file that never sets the last-block flag.
    for (int guard = 0; guard < kMaxFlacBlocks; ++guard) {
        std::string h = f.Read(pos, 4);
        if (h.size() < 4) break;
        bool last = (uint8_t)h[0] & 0x80;
        int type = (uint8_t)h[0] & 0x7F;
        uint64_t len = ReadBE(h, 1, 3);
        if (type == 0 && len >= 18) {
            std::string info = f.Read(pos + 4, 18);
            if (info.size() == 18) rate = (uint32_t)(uint8_t)info[10] << 12 | (uint32_t)(uint8_t)info[11] << 4 | (uint8_t)info[12] >> 4;
        } else if (type == 4 && !haveText && len <= kMaxFlacBlockBytes) {
            haveText = FindVorbisCue(f.Read(pos + 4, (size_t)len), text);
        } else if (type == 5 && binary.empty() && len > 0 && len <= kMaxFlacBlockBytes) {
            binary = f.Read(pos + 4, (size_t)len);
        }
        pos += 4 + len;
        if (last) break;
    }
    CueSheet parsed;
    if (haveText) {
        // An embedded sheet may leave out FILE; the file is this one either way.
        std::string withFile = "FILE \"" + ToUtf8(FileNameOf(audioPath)) + "\" WAVE\n" + text;
        if (!ParseCue(withFile, DirectoryOf(audioPath), parsed)) return false;
    } else if (!binary.empty()) {
        if (!ParseBinaryCue(binary, rate, parsed)) return false;
    } else {
        return false;
    }
    // One file: the sheet's tracks, merged across any FILE lines, all in this file.
    out.catalog = parsed.catalog;
    out.title = parsed.title;
    out.performer = parsed.performer;
    out.date = parsed.date;
    out.genre = parsed.genre;
    out.comment = parsed.comment;
    out.discNumber = parsed.discNumber;
    out.totalDiscs = parsed.totalDiscs;
    CueSheet::File file;
    file.path = audioPath;
    for (auto& src : parsed.files) file.tracks.insert(file.tracks.end(), src.tracks.begin(), src.tracks.end());
    SetEnds(file);
    out.files.push_back(std::move(file));
    return !out.files[0].tracks.empty();
}

// ---------------------------------------------------------------------------
// Folders and sub-tracks

CueFolder::CueFolder(const std::wstring& dir) {
    std::wstring prefix = dir.empty() || dir.back() == kPathSep ? dir : dir + kPathSep;
    std::vector<std::pair<std::wstring, uint64_t>> cues;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((prefix + L"*.cue").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring path = prefix + fd.cFileName;
            cues.emplace_back(path, (uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32 | fd.ftLastWriteTime.dwLowDateTime);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    SortObjects(cues.begin(), cues.end());
    for (const auto& [path, time] : cues) {
        auto sheet = CachedSheet(path, time, [&](CueSheet& s) { return LoadCueFile(path, s); });
        if (!sheet) continue;
        for (size_t f = 0; f < sheet->files.size(); ++f)
            if (!sheet->files[f].tracks.empty()) covers_.emplace_back(Canon(sheet->files[f].path), Cover{sheet, f, time});
    }
}

bool CueFolder::Covering(const std::wstring& audioPath, Cover& out) const {
    std::wstring key = Canon(audioPath);
    for (const auto& [name, cover] : covers_)
        if (name == key) {
            out = cover;
            return true;
        }
    // No .cue names it: a FLAC may carry its sheet inside.
    std::wstring ext = ExtOf(audioPath);
    if (ext != L"flac" && ext != L"fla") return false;
    uint64_t time = WriteTimeOf(audioPath);
    auto sheet = CachedSheet(L"embedded:" + key, time, [&](CueSheet& s) { return ReadEmbeddedCue(audioPath, s); });
    if (!sheet) return false;
    out = Cover{sheet, 0, 0};
    return true;
}

std::vector<std::wstring> CueFolder::Expand(const std::wstring& audioPath, uint64_t* cueTime) const {
    std::vector<std::wstring> out;
    Cover cover;
    if (Covering(audioPath, cover))
        for (const CueTrack& t : cover.sheet->files[cover.file].tracks) out.push_back(SubtrackPath(audioPath, t.number));
    if (cueTime) *cueTime = out.empty() ? 0 : cover.time;
    if (out.empty()) {
        int games = GameMusicSubtrackCount(audioPath);
        for (int i = 1; i <= games; ++i) out.push_back(SubtrackPath(audioPath, i));
    }
    return out;
}

bool LookupSubtrack(const std::wstring& path, CueSubtrack& out) {
    std::wstring file;
    int number = 0;
    if (!SplitSubtrack(path, file, number)) return false;
    CueFolder folder(DirectoryOf(file));
    CueFolder::Cover cover;
    if (!folder.Covering(file, cover)) return false;
    const auto& tracks = cover.sheet->files[cover.file].tracks;
    for (size_t i = 0; i < tracks.size(); ++i)
        if (tracks[i].number == number) {
            out = CueSubtrack{cover.sheet, cover.file, i, file, cover.time};
            return true;
        }
    return false;
}

bool SubtrackRange(const std::wstring& path, double& start, double& end) {
    CueSubtrack s;
    if (!LookupSubtrack(path, s)) return false;
    start = s.Track().start;
    end = s.Track().end;
    return true;
}

std::unique_ptr<Decoder> OpenTrackDecoder(const std::wstring& path, unsigned fallbackRate, std::wstring& error) {
    if (!IsSubtrack(path) || IsGameMusicFile(path)) return OpenDecoder(path, fallbackRate, error);
    double start = 0, end = 0;
    if (!SubtrackRange(path, start, end)) {
        error = L"The CUE sheet for this track is gone";
        return nullptr;
    }
    auto d = OpenDecoder(AudioFileOf(path), fallbackRate, error);
    if (!d) return d;
    d = OpenRange(std::move(d), start, end);
    if (!d) error = L"Couldn't seek to the track's start";
    return d;
}

std::wstring AudioFileOf(const std::wstring& path) {
    std::wstring file;
    int number = 0;
    return SplitSubtrack(path, file, number) ? file : path;
}

bool IsCueFile(const std::wstring& path) {
    return ExtOf(path) == L"cue";
}

bool ExpandCueFile(const std::wstring& cuePath, std::vector<std::wstring>& out) {
    CueSheet sheet;
    if (!LoadCueFile(cuePath, sheet)) return false;
    bool any = false;
    for (const auto& file : sheet.files) {
        if (!FileExistsW(file.path)) continue;
        for (const auto& t : file.tracks) {
            out.push_back(SubtrackPath(file.path, t.number));
            any = true;
        }
    }
    return any;
}

std::vector<std::wstring> ExpandSubtracks(const std::wstring& audioPath) {
    if (IsSubtrack(audioPath)) return {};  // already a track
    return CueFolder(DirectoryOf(audioPath)).Expand(audioPath);
}

// NSF, GBS and other files holding several songs become one track per song; a single-song file
// stays a plain track.
int GameMusicSubtrackCount(const std::wstring& audioPath) {
    if (!IsGameMusicFile(audioPath)) return 0;
    int count = GameMusicTrackCount(audioPath);
    return count > 1 ? count : 0;
}
