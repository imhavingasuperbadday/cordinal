// Transactional imports and exports for playlist-ish files: every Cordinal/Waveforge cache
// generation plus Unicode M3U. Loads build into temporaries and swap into the globals only after
// the whole file parsed, so a bad file leaves the current state untouched.
#include "library.h"
#include "cue.h"

#include <shlwapi.h>
#include <algorithm>
#include <charconv>
#include <limits>
#include <span>

namespace {

// Writes a temp file and renames it over the target on Commit. Put() replaces tabs, newlines and
// NULs with spaces so fields stay on one line; Raw() writes bytes verbatim.
class LineWriter {
public:
    explicit LineWriter(const std::wstring& file) : file_(file) {
        static std::atomic<unsigned> serial{0};
        tmp_ = file + L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(++serial);
        h_ = CreateFileW(tmp_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        ownsTemporary_ = h_ != INVALID_HANDLE_VALUE;
        buf_.reserve(kChunk + 4096);
    }
    ~LineWriter() {
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
        if (ownsTemporary_ && !committed_) DeleteFileW(tmp_.c_str());
    }
    void Put(std::string_view s) {
        for (char c : s) {
            buf_ += (c == '\t' || c == '\n' || c == '\r' || c == '\0') ? ' ' : c;
            if (buf_.size() >= kChunk) Flush();
        }
    }
    void Raw(std::string_view s) { buf_.append(s); }
    void End() {
        buf_ += '\n';
        if (buf_.size() >= kChunk) Flush();
    }
    bool Commit() {
        Flush();
        if (h_ == INVALID_HANDLE_VALUE || !ok_) return false;
        if (!FlushFileBuffers(h_)) return false;
        if (!CloseHandle(h_)) { h_ = INVALID_HANDLE_VALUE; return false; }
        h_ = INVALID_HANDLE_VALUE;
        committed_ = MoveFileExW(tmp_.c_str(), file_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
        return committed_;
    }

private:
    static constexpr size_t kChunk = 64 * 1024;
    void Flush() {
        if (!ok_) { buf_.clear(); return; }
        DWORD w = 0;
        if (h_ == INVALID_HANDLE_VALUE ||
            (!buf_.empty() && (!WriteFile(h_, buf_.data(), (DWORD)buf_.size(), &w, nullptr) || w != buf_.size())))
            ok_ = false;
        buf_.clear();
    }
    std::wstring tmp_, file_;
    HANDLE h_;
    std::string buf_;
    bool ok_ = true;
    bool committed_ = false;
    bool ownsTemporary_ = false;
};

// Line reader that strips a UTF-8 BOM or decodes UTF-16LE/BE, re-joins lines split across read
// windows, and rejects invalid UTF-8, oversized lines and embedded NULs. `f` returns false to abort.
template <class F>
bool ForEachLine(const std::wstring& file, F&& f) {
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::string carry;
    std::unique_ptr<char[]> buf(new char[64 * 1024]);
    DWORD got = 0;
    bool first = true;
    bool ok = true;
    int utf16 = 0;
    auto emit = [&](std::string_view line) {
        std::string converted;
        if (utf16) {
            if (line.size() % 2) return false;
            std::wstring wide(line.size() / 2, L'\0');
            for (size_t i = 0; i < wide.size(); ++i) {
                unsigned a = (unsigned char)line[i * 2], b = (unsigned char)line[i * 2 + 1];
                wide[i] = (wchar_t)(utf16 == 1 ? a | (b << 8) : (a << 8) | b);
            }
            if (!wide.empty() && !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), (int)wide.size(),
                                                      nullptr, 0, nullptr, nullptr)) return false;
            converted = ToUtf8(wide);
            line = converted;
        }
        if (line.size() > (1u << 20) || line.find('\0') != std::string_view::npos) return false;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        f(line);
        return true;
    };
    while ((ok = ReadFile(h, buf.get(), 64 * 1024, &got, nullptr) != FALSE) && got) {
        std::string_view chunk(buf.get(), got);
        if (first) {
            if (chunk.size() >= 3 && chunk.substr(0, 3) == "\xEF\xBB\xBF") chunk.remove_prefix(3);
            else if (chunk.size() >= 2 && chunk.substr(0, 2) == "\xFF\xFE") { utf16 = 1; chunk.remove_prefix(2); }
            else if (chunk.size() >= 2 && chunk.substr(0, 2) == "\xFE\xFF") { utf16 = 2; chunk.remove_prefix(2); }
        }
        first = false;
        if (utf16) {
            carry.append(chunk);
            size_t start = 0;
            for (size_t i = 0; i + 1 < carry.size(); i += 2) {
                bool newline = utf16 == 1 ? carry[i] == '\n' && !carry[i + 1] : !carry[i] && carry[i + 1] == '\n';
                if (!newline) continue;
                if (!emit(std::string_view(carry).substr(start, i - start))) { ok = false; break; }
                start = i + 2;
            }
            carry.erase(0, start);
            if (!ok || carry.size() > (2u << 20)) { ok = false; break; }
            continue;
        }
        size_t start = 0;
        for (size_t i = 0; i < chunk.size(); ++i) {
            if (chunk[i] != '\n') continue;
            std::string_view line = chunk.substr(start, i - start);
            if (!carry.empty()) {
                carry.append(line);
                line = carry;
            }
            if (!emit(line)) { ok = false; break; }
            carry.clear();
            start = i + 1;
        }
        if (!ok) break;
        carry.append(chunk.substr(start));
        if (carry.size() > (1u << 20)) { ok = false; break; }
    }
    if (ok && !carry.empty()) {
        ok = emit(carry);
    }
    CloseHandle(h);
    return ok;
}

// Splits on tabs into the caller's buffer; returns empty when the line has more fields than fit.
std::span<std::string_view> SplitTabs(std::string_view line, std::span<std::string_view> parts) {
    size_t start = 0, count = 0;
    for (;;) {
        size_t tab = line.find('\t', start);
        if (count == parts.size()) return {};
        parts[count++] = line.substr(start, tab == std::string_view::npos ? std::string_view::npos : tab - start);
        if (tab == std::string_view::npos) return parts.first(count);
        start = tab + 1;
    }
}

// Whole-field integer parse and clamp; anything malformed reads as 0. Used on untrusted lines.
long long ToInt(std::string_view s) {
    long long v = 0;
    auto result = std::from_chars(s.data(), s.data() + s.size(), v);
    return result.ec == std::errc{} && result.ptr == s.data() + s.size() ? v : 0;
}

template <class T> T BoundedInt(std::string_view s) {
    return (T)std::clamp(ToInt(s), (long long)std::numeric_limits<T>::min(), (long long)std::numeric_limits<T>::max());
}

// Undoes the \t \n \r \\ escaping version 3 writes, so stored text can hold tabs and newlines.
std::string Unescape(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size()) {
            char next = text[i + 1];
            if (next == 't' || next == 'n' || next == 'r' || next == '\\') {
                result += next == 't' ? '\t' : next == 'n' ? '\n' : next == 'r' ? '\r' : '\\';
                ++i;
                continue;
            }
        }
        result += text[i];
    }
    return result;
}

// The CORDINAL lines are expected to be UTF-8 already; this gates each line before parsing.
bool IsUtf8(std::string_view text) {
    return text.empty() || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), (int)text.size(), nullptr, 0) > 0;
}

// Strict whole-field unsigned parse (used for definition and selection references).
bool ParseIndex(std::string_view text, uint32_t& value) {
    auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

}  // namespace

// Reads a CORDINAL1/2/3 (or WAVEFORGE1/2) text cache: one record per tab-separated line. Version
// 3 stores a shared track table ("T") that items ("I") and selection ("S") reference by index;
// earlier versions inline the path in each item. Tracks land in `pending` and merge into g_lib
// only on success.
bool LoadPlaylists(const std::wstring& file, std::vector<Playlist>& playlists) {
    std::vector<Playlist> loaded;
    Library pending;
    std::vector<TrackId> definitions;
    int version = 0;
    bool valid = true;
    bool ok = ForEachLine(file, [&](std::string_view line) {
        if (!valid) return;
        if (!IsUtf8(line)) { valid = false; return; }
        if (!version) {
            version = line == "CORDINAL3" ? 3 : line == "CORDINAL2" || line == "WAVEFORGE2" ? 2 :
                      line == "CORDINAL1" || line == "WAVEFORGE1" ? 1 : -1;
            return;
        }
        if (version < 0 || line.empty()) return;
        if (line.size() < 2) { if (version == 3) valid = false; return; }
        std::string_view fields[32];
        auto parts = SplitTabs(line, fields);
        if (parts.empty()) { valid = false; return; }
        if (parts[0] == "P" && parts.size() >= 2) {
            loaded.push_back(Playlist{CleanPlaylistName(FromUtf8(version == 3 ? Unescape(parts[1]) : std::string(parts[1]))), {}});
        } else if (parts[0] == "I" && version == 3) {
            uint32_t ref = 0;
            if (parts.size() != 2 || loaded.empty() || !ParseIndex(parts[1], ref) || ref >= definitions.size() ||
                loaded.back().items.size() >= INT_MAX) { valid = false; return; }
            loaded.back().items.push_back(definitions[ref]);
        } else if (parts[0] == "V" && version == 3) {
            if (parts.size() != 6 || loaded.empty()) { valid = false; return; }
            auto& view = loaded.back().position;
            view.search = FromUtf8(Unescape(parts[1]));
            loaded.back().sortColumn = BoundedInt<int>(parts[2]);
            loaded.back().sortAscending = ToInt(parts[3]) != 0;
            view.top = BoundedInt<uint32_t>(parts[4]);
            view.focus = BoundedInt<uint32_t>(parts[5]);
        } else if (parts[0] == "S" && version == 3) {
            uint32_t ref = 0;
            if (parts.size() != 2 || loaded.empty() || !ParseIndex(parts[1], ref) || ref >= loaded.back().items.size() ||
                loaded.back().position.selected.size() >= loaded.back().items.size()) { valid = false; return; }
            loaded.back().position.selected.push_back(ref);
        } else if (parts[0] == "T" && parts.size() >= 2 && !parts[1].empty() && (version == 3 || !loaded.empty())) {
            TrackId id = pending.Add(FromUtf8(version == 3 ? Unescape(parts[1]) : std::string(parts[1])));
            if (id == kInvalidTrack || (version == 3 && parts.size() != 19)) { valid = false; return; }
            if (version == 3) definitions.push_back(id);
            else loaded.back().items.push_back(id);
            if (version == 1 || parts.size() < 19) return;
            Track& t = pending[id];
            if (t.Loaded()) return;
            auto intern = [&](size_t index) {
                auto text = version == 3 ? Unescape(parts[index]) : std::string(parts[index]);
                size_t length = std::min<size_t>(text.size(), 65536);
                if (length < text.size()) while (length && ((unsigned char)text[length] & 0xC0) == 0x80) --length;
                return pending.Intern(std::string_view(text).substr(0, length));
            };
            t.artist = intern(2);
            t.title = intern(3);
            t.album = intern(4);
            t.albumArtist = intern(5);
            t.date = intern(6);
            t.genre = intern(7);
            unsigned flags = BoundedInt<uint8_t>(parts[8]);
            t.flags = (uint16_t)((flags & 1) | (version == 3 && (flags & 8) ? Track::kRgScanned : 0));
            long long codec = ToInt(parts[9]);
            t.codec = codec >= 0 && codec <= (int)Codec::AC3 ? (Codec)codec : Codec::Unknown;
            t.trackNo = BoundedInt<uint16_t>(parts[10]);
            t.bitrate = BoundedInt<uint16_t>(parts[11]);
            t.sampleRate = BoundedInt<uint32_t>(parts[12]);
            t.channels = BoundedInt<uint8_t>(parts[13]);
            t.durationMs = BoundedInt<uint32_t>(parts[14]);
            t.rgTrackGain = BoundedInt<int16_t>(parts[15]);
            t.rgTrackPeak = BoundedInt<uint16_t>(parts[16]);
            t.rgAlbumGain = BoundedInt<int16_t>(parts[17]);
            t.rgAlbumPeak = BoundedInt<uint16_t>(parts[18]);
        } else if (version == 3) valid = false;
    });
    if (!ok || !valid || version <= 0) return false;
    for (auto& pl : loaded) SanitizePlaylistPosition(pl);
    if (!g_lib.Count()) g_lib = std::move(pending);
    else {
        std::vector<TrackId> remap(pending.Count());
        for (TrackId id = 0; id < pending.Count(); ++id) remap[id] = g_lib.ImportTrack(pending, id);
        for (auto& pl : loaded) for (auto& id : pl.items) id = remap[id];
    }
    playlists.swap(loaded);
    return true;
}

// Reads an M3U/M3U8 into audio paths (appended only on success). .m3u8 is strict UTF-8; .m3u may
// fall back to the ANSI code page. file: URLs and relative paths resolve against the playlist's
// own folder; network URLs are skipped.
bool ImportM3U(const std::wstring& file, std::vector<std::wstring>& paths) {
    std::wstring base = DirectoryOf(NormalizeMediaPath(file));
    std::vector<std::wstring> imported;
    bool valid = true;
    bool strict = ExtOf(file) == L"m3u8";
    bool ok = ForEachLine(file, [&](std::string_view raw) {
        if (!valid) return;
        size_t first = raw.find_first_not_of(" \t");
        if (first == std::string_view::npos || raw[first] == '#') return;
        raw.remove_prefix(first);
        raw = raw.substr(0, raw.find_last_not_of(" \t") + 1);
        std::wstring line;
        int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.data(), (int)raw.size(), nullptr, 0);
        if (!n && strict) { valid = false; return; }
        UINT cp = n > 0 ? CP_UTF8 : CP_ACP;  // legacy .m3u files use the ANSI code page
        if (!n) n = MultiByteToWideChar(cp, 0, raw.data(), (int)raw.size(), nullptr, 0);
        line.resize(n);
        MultiByteToWideChar(cp, 0, raw.data(), (int)raw.size(), line.data(), n);
        if (line.size() >= 2 && line.front() == L'"' && line.back() == L'"') line = line.substr(1, line.size() - 2);
        if (_wcsnicmp(line.c_str(), L"file:", 5) == 0) {
            std::wstring local(line.size() + 1, L'\0');
            DWORD len = (DWORD)local.size();
            if (FAILED(PathCreateFromUrlW(line.c_str(), local.data(), &len, 0))) return;
            local.resize(wcslen(local.c_str()));
            line = std::move(local);
        } else if (line.find(L"://") != std::wstring::npos ||
                   (line.find(L':') != std::wstring::npos && line.find(L':') != 1)) {
            return;  // Network streams are not local playlist entries.
        } else if (PathIsRelativeW(line.c_str())) {
            if (line.size() < 2 || line[1] != L':') line = base.empty() ? line : base + kPathSep + line;
        } else if (line.size() > 1 && (line[0] == L'\\' || line[0] == L'/') && line[1] != L'\\' && line[1] != L'/' &&
                   base.size() >= 2 && base[1] == L':') {
            line = base.substr(0, 2) + line;
        }
        line = NormalizeMediaPath(line);
        if (IsCueFile(line)) { ExpandCueFile(line, imported); return; }
        if (line.empty() || !IsSupportedAudioFile(line)) return;
        auto tracks = IsSubtrack(line) ? std::vector<std::wstring>{} : ExpandSubtracks(line);
        if (tracks.empty()) imported.push_back(std::move(line));
        else imported.insert(imported.end(), std::make_move_iterator(tracks.begin()), std::make_move_iterator(tracks.end()));
    });
    if (ok && valid) paths.insert(paths.end(), std::make_move_iterator(imported.begin()), std::make_move_iterator(imported.end()));
    return ok && valid;
}

namespace {
std::string ExtInf(TrackId id) {
    const Track& t = g_lib[id];
    std::wstring title;
    LookupField(id, L"title", title);
    std::string display = t.artist ? std::string(g_lib.View(t.artist)) + " - " + ToUtf8(title) : ToUtf8(title);
    for (char& c : display)
        if (c == '\t' || c == '\n' || c == '\r' || c == '\0') c = ' ';
    return "#EXTINF:" + std::to_string(t.durationMs / 1000) + "," + display;
}
}  // namespace

std::string M3UText(const std::vector<std::pair<TrackId, std::string>>& entries) {
    std::string text = "\xEF\xBB\xBF#EXTM3U\n";
    for (const auto& [id, location] : entries)
        if (g_lib.Contains(id)) text += ExtInf(id) + "\n" + location + "\n";
    return text;
}

bool CaptureM3U(const Playlist& playlist, std::vector<M3UExportEntry>& entries) {
    for (TrackId id : playlist.items) if (!g_lib.Contains(id)) return false;
    std::vector<M3UExportEntry> captured;
    captured.reserve(playlist.items.size());
    for (TrackId id : playlist.items) captured.push_back({ExtInf(id), ToUtf8(g_lib.Path(id))});
    entries.swap(captured);
    return true;
}

// Writes a UTF-8 BOM + #EXTM3U M3U and commits atomically. Captured entries never read g_lib,
// so a worker can save the selected playlist even while the library changes.
bool ExportM3U(const std::wstring& file, const std::vector<M3UExportEntry>& entries) {
    LineWriter w(file);
    w.Raw("\xEF\xBB\xBF#EXTM3U");
    w.End();
    for (const auto& entry : entries) {
        w.Put(entry.info);
        w.End();
        w.Put(entry.path);
        w.End();
    }
    return w.Commit();
}

bool ExportM3U(const std::wstring& file, const Playlist& playlist) {
    for (TrackId id : playlist.items) if (!g_lib.Contains(id)) return false;
    LineWriter w(file);
    w.Raw("\xEF\xBB\xBF#EXTM3U");
    w.End();
    for (TrackId id : playlist.items) {
        w.Raw(ExtInf(id));
        w.End();
        w.Put(ToUtf8(g_lib.Path(id)));
        w.End();
    }
    return w.Commit();
}
