// Readable, versioned library interchange and conservative statistics merging. No live globals or I/O.
#include "migration.h"
#include "autoplaylist.h"
#include <algorithm>
#include <charconv>
#include <limits>
#include <set>

namespace migration {
namespace {
using Cells = std::vector<std::string>;
using Table = std::vector<Cells>;

void Row(std::string& out, const Cells& cells) {
    for (size_t i = 0; i < cells.size(); ++i) {
        if (i) out += ',';
        const auto& s = cells[i];
        if (s.find_first_of(",\"\r\n") == s.npos)
            out += s;
        else {
            out += '"';
            for (char c : s) {
                if (c == '"') out += '"';
                out += c;
            }
            out += '"';
        }
    }
    out += "\r\n";
}

bool Csv(std::string_view text, Table& out) {
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    if (text.size() > 256 * 1024 * 1024 || text.find('\0') != text.npos ||
        (!text.empty() &&
         !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), (int)text.size(), nullptr, 0)))
        return false;
    Table table;
    Cells row;
    std::string cell;
    bool quoted = false, ended = false, beginning = true;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (quoted) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') {
                    cell += '"';
                    ++i;
                } else {
                    quoted = false;
                    ended = true;
                }
            } else
                cell += c;
        } else if (c == ',' || c == '\r' || c == '\n') {
            row.push_back(std::move(cell));
            cell.clear();
            ended = false;
            beginning = true;
            if (c != ',') {
                if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
                table.push_back(std::move(row));
                row.clear();
            }
        } else if (ended)
            return false;
        else if (c == '"') {
            if (!beginning) return false;
            quoted = true;
            beginning = false;
        } else {
            cell += c;
            beginning = false;
        }
        if (cell.size() > 1024 * 1024 || row.size() > 128) return false;
    }
    if (quoted) return false;
    if (!beginning || !row.empty() || ended) {
        row.push_back(std::move(cell));
        table.push_back(std::move(row));
    }
    if (table.empty()) return false;
    std::set<std::string> names;
    for (const auto& name : table[0])
        if (name.empty() || !names.insert(name).second) return false;
    for (const auto& r : table)
        if (r.size() != table[0].size()) return false;
    out = std::move(table);
    return true;
}

struct Reader {
    const Cells& header;
    const Cells& row;
    bool ok = true;
    std::string_view Text(const char* key, bool required = true) {
        auto it = std::find(header.begin(), header.end(), key);
        if (it == header.end()) {
            if (required) ok = false;
            return {};
        }
        return row[(size_t)(it - header.begin())];
    }
    template <class T> T Number(const char* key, bool required = true) {
        auto text = Text(key, required);
        if (text.empty() && !required) return 0;
        T value{};
        auto r = std::from_chars(text.data(), text.data() + text.size(), value);
        if (r.ec != std::errc() || r.ptr != text.data() + text.size()) ok = false;
        return value;
    }
};

std::string Date(uint32_t time) {
    if (!time) return {};
    ULARGE_INTEGER u;
    u.QuadPart = (time + 11644473600ull) * 10000000ull;
    FILETIME ft{u.LowPart, u.HighPart};
    SYSTEMTIME st{};
    FileTimeToSystemTime(&ft, &st);
    char b[64];
    sprintf_s(b, "%04u-%02u-%02uT%02u:%02u:%02uZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
              st.wSecond);
    return b;
}

uint32_t Time(Reader& r, const char* key, bool required = true) {
    auto s = r.Text(key, required);
    if (s.empty()) return 0;
    if (s.size() != 20 || s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' || s[16] != ':' ||
        s[19] != 'Z') {
        r.ok = false;
        return 0;
    }
    auto n = [&](size_t at, size_t len) -> WORD {
        unsigned value = 0;
        auto res = std::from_chars(s.data() + at, s.data() + at + len, value);
        if (res.ec != std::errc() || res.ptr != s.data() + at + len) r.ok = false;
        return (WORD)value;
    };
    SYSTEMTIME st{};
    st.wYear = n(0, 4);
    st.wMonth = n(5, 2);
    st.wDay = n(8, 2);
    st.wHour = n(11, 2);
    st.wMinute = n(14, 2);
    st.wSecond = n(17, 2);
    FILETIME ft{};
    if (!SystemTimeToFileTime(&st, &ft)) {
        r.ok = false;
        return 0;
    }
    SYSTEMTIME check{};
    if (!FileTimeToSystemTime(&ft, &check) || check.wYear != st.wYear || check.wMonth != st.wMonth ||
        check.wDay != st.wDay || check.wHour != st.wHour || check.wMinute != st.wMinute ||
        check.wSecond != st.wSecond)
        r.ok = false;
    uint64_t ticks = ((uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime) / 10000000ull;
    if (ticks <= 11644473600ull || ticks - 11644473600ull > UINT32_MAX) r.ok = false;
    return r.ok ? (uint32_t)(ticks - 11644473600ull) : 0;
}

std::string N(auto value) {
    return std::to_string(value);
}
std::string W(const Library& lib, StringPool::Handle h) {
    return std::string(lib.View(h));
}

const File* Find(const std::vector<File>& files, const wchar_t* name) {
    for (const auto& f : files)
        if (f.path == name) return &f;
    return nullptr;
}

bool FormatOne(const File* file) {
    if (!file) return false;
    std::string_view text = file->bytes;
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    bool section = false, found = false;
    auto trim = [](std::string_view s) {
        auto a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
        return a == s.npos ? std::string_view() : s.substr(a, b - a + 1);
    };
    while (!text.empty()) {
        auto end = std::min(text.find('\n'), text.size());
        auto line = trim(text.substr(0, end));
        text.remove_prefix(std::min(end + 1, text.size()));
        if (line.starts_with("["))
            section = line == "[backup]";
        else if (section) {
            auto equals = line.find('=');
            if (equals != line.npos && trim(line.substr(0, equals)) == "format") {
                if (found || trim(line.substr(equals + 1)) != "1") return false;
                found = true;
            }
        }
    }
    return found;
}

bool ReadTable(const std::vector<File>& files, const wchar_t* name, Table& out, std::wstring& error) {
    const auto* f = Find(files, name);
    if (f && Csv(f->bytes, out)) return true;
    error = std::wstring(L"Couldn't read ") + name + L". The backup is incomplete or its CSV is invalid.";
    return false;
}

Statistics Stat(Reader& r) {
    Statistics s;
    s.path = FromUtf8(r.Text("path"));
    s.hasCount = !r.Text("play_count", false).empty();
    s.hasSkips = !r.Text("skip_count", false).empty();
    s.hasRating = !r.Text("rating", false).empty();
    s.count = r.Number<uint32_t>("play_count", false);
    s.skips = r.Number<uint32_t>("skip_count", false);
    s.rating = r.Number<uint8_t>("rating", false);
    s.added = Time(r, "added", false);
    s.first = Time(r, "first_played", false);
    s.last = Time(r, "last_played", false);
    if (s.path.empty() || s.rating > 100 || s.rating % 10 || (s.first && s.last && s.first > s.last))
        r.ok = false;
    return s;
}

std::wstring Slashes(std::wstring path) {
    std::replace(path.begin(), path.end(), L'/', L'\\');
    while (path.size() > 1 && path.back() == L'\\') path.pop_back();
    return path;
}
} // namespace

std::wstring RemapPath(std::wstring path, const std::vector<PathMap>& maps) {
    const auto key = Slashes(path);
    const PathMap* best = nullptr;
    size_t length = 0;
    for (const auto& map : maps) {
        auto from = Slashes(map.from);
        if (from.empty() || map.to.empty() || from.size() < length || key.size() < from.size()) continue;
        if (key.size() != from.size() && key[from.size()] != L'\\' && key[from.size()] != L'|') continue;
        bool ignoreCase = true;
#ifndef _WIN32
        ignoreCase = map.from.size() > 1 && (map.from[1] == L':' || map.from.starts_with(L"\\\\"));
#endif
        if (CompareStringOrdinal(key.data(), (int)from.size(), from.data(), (int)from.size(), ignoreCase) !=
            CSTR_EQUAL)
            continue;
        best = &map;
        length = from.size();
    }
    if (!best) return path;
    auto dest = best->to;
    while (!dest.empty() && (dest.back() == L'\\' || dest.back() == L'/')) dest.pop_back();
    auto tail = path.substr(length);
    for (auto& c : tail)
        if (c == L'/' || c == L'\\') c = kPathSep;
    return dest + tail;
}

bool RemapLibrary(LibraryArchive& data, const std::vector<PathMap>& maps, std::wstring& error) {
    // Build a replacement index first; overlapping roots must never merge two distinct tracks.
    Library replacement;
    for (TrackId id = 0; id < data.library.Count(); ++id) {
        auto path = RemapPath(data.library.Path(id), maps);
        if (replacement.Find(path) != kInvalidTrack || replacement.Add(path) != id) {
            error = L"Two songs would have the same path, or a new path is invalid: " + path;
            return false;
        }
        auto copy = data.library[id];
        copy.dir = replacement[id].dir;
        copy.file = replacement[id].file;
        for (auto field : {&Track::title, &Track::artist, &Track::album, &Track::albumArtist, &Track::genre,
                           &Track::date, &Track::composer, &Track::comment, &Track::grouping, &Track::label})
            copy.*field = replacement.Intern(data.library.View(copy.*field));
        replacement[id] = copy;
    }
    std::map<AlbumId, AlbumMeasurement> albums;
    for (const auto& [key, album] : data.albums)
        if (!album.members.empty()) albums[AlbumKeyOf(replacement[album.members.front()])] = album;
    data.library = std::move(replacement);
    data.albums = std::move(albums);
    return true;
}

static std::string LibraryCsv(const Library& library) {
    std::string out = "\xEF\xBB\xBF";
    Row(out, {"path",       "title",       "artist",       "album",       "rating",        "play_count",
              "skip_count", "added",       "first_played", "last_played", "album_artist",  "genre",
              "date",       "composer",    "comment",      "grouping",    "label",         "file_size",
              "file_time",  "duration_ms", "sample_rate",  "bitrate",     "track_number",  "track_total",
              "bpm",        "disc",        "disc_total",   "channels",    "codec",         "flags",
              "track_gain", "album_gain",  "track_peak",   "album_peak",  "high_loudness", "audio_hash"});
    for (TrackId id = 0; id < library.Count(); ++id) {
        const auto& t = library[id];
        Row(out, {ToUtf8(library.Path(id)),
                  W(library, t.title),
                  W(library, t.artist),
                  W(library, t.album),
                  N(t.rating),
                  N(t.playCount),
                  N(t.skipCount),
                  Date(t.added),
                  Date(t.firstPlayed),
                  Date(t.lastPlayed),
                  W(library, t.albumArtist),
                  W(library, t.genre),
                  W(library, t.date),
                  W(library, t.composer),
                  W(library, t.comment),
                  W(library, t.grouping),
                  W(library, t.label),
                  N(t.fileSize),
                  N(t.fileTime),
                  N(t.durationMs),
                  N(t.sampleRate),
                  N(t.bitrate),
                  N(t.trackNo),
                  N(t.trackTotal),
                  N(t.bpm),
                  N(t.disc),
                  N(t.discTotal),
                  N(t.channels),
                  N((unsigned)t.codec),
                  N(t.flags & ~Track::kFailed),
                  N(t.rgTrackGain),
                  N(t.rgAlbumGain),
                  N(t.rgTrackPeak),
                  N(t.rgAlbumPeak),
                  N(t.rgHigh),
                  N(t.audioHash)});
    }
    return out;
}

std::string StatisticsCsv(const Library& library) {
    std::string out = "\xEF\xBB\xBF";
    Row(out, {"path", "title", "artist", "album", "rating", "play_count", "skip_count", "added",
              "first_played", "last_played"});
    auto text = [&](StringPool::Handle h) {
        auto s = W(library, h);
        size_t first = s.find_first_not_of(" \t\r\n");
        if (first != s.npos && std::string_view("=+-@").find(s[first]) != std::string_view::npos)
            s.insert(s.begin(), '\'');
        return s;
    };
    for (TrackId id = 0; id < library.Count(); ++id) {
        const auto& t = library[id];
        Row(out, {ToUtf8(library.Path(id)), text(t.title), text(t.artist), text(t.album), N(t.rating),
                  N(t.playCount), N(t.skipCount), Date(t.added), Date(t.firstPlayed), Date(t.lastPlayed)});
    }
    return out;
}

std::string HistoryCsv(const LibraryArchive& data) {
    std::string out = "\xEF\xBB\xBF";
    Row(out, {"track_id", "path", "played_at_utc", "listened_seconds", "counted", "skipped"});
    for (const auto& e : data.log)
        if (data.library.Contains(e.id))
            Row(out, {N(e.id), ToUtf8(data.library.Path(e.id)), Date(e.time), N(e.seconds),
                      N(!!(e.flags & PlayEvent::kCounted)), N(!!(e.flags & PlayEvent::kSkip))});
    return out;
}

std::vector<File> Export(const LibraryArchive& data) {
    std::vector<File> files{{L"backup.ini", "[backup]\r\nformat=1\r\n"},
                            {L"library.csv", LibraryCsv(data.library)},
                            {L"statistics.csv", StatisticsCsv(data.library)},
                            {L"history.csv", HistoryCsv(data)}};
    std::string pls, items, views, albums, days, recent, state;
    Row(pls, {"name", "kind", "rules", "sort_column", "sort_ascending"});
    Row(items, {"playlist_id", "track_id"});
    Row(views, {"playlist_id", "search", "focus", "top", "expanded", "scroll", "selected"});
    for (size_t i = 0; i < data.playlists.size(); ++i) {
        const auto& p = data.playlists[i];
        Row(pls, {ToUtf8(p.name), N((unsigned)p.kind), p.rules, N(p.sortColumn), N(p.sortAscending)});
        std::string m3u = "\xEF\xBB\xBF#EXTM3U\r\n", selected;
        for (TrackId id : p.items)
            if (data.library.Contains(id)) Row(items, {N(i), N(id)});
        auto contents = p.items;
        if (p.kind == PlaylistKind::Auto) {
            AutoPlaylist rules;
            if (ParseAutoPlaylist(p.rules, rules))
                EvaluateAutoPlaylist(rules, data.library, data.log, contents, 1);
        } else if (p.kind == PlaylistKind::Library || p.kind == PlaylistKind::Inbox) {
            contents.clear();
            for (TrackId id = 0; id < data.library.Count(); ++id)
                if (p.kind == PlaylistKind::Library
                        ? data.library[id].InLibrary()
                        : (data.library[id].flags & (Track::kInbox | Track::kMissing)) == Track::kInbox)
                    contents.push_back(id);
        }
        for (TrackId id : contents)
            if (data.library.Contains(id)) {
                auto path = ToUtf8(data.library.Path(id));
                // M3U cannot encode a newline in a path. The CSV remains lossless.
                if (path.find_first_of("\r\n") == path.npos) m3u += path + "\r\n";
            }
        for (TrackId id : p.position.selected) selected += (selected.empty() ? "" : ";") + N(id);
        Row(views, {N(i), ToUtf8(p.position.search), N(p.position.focus), N(p.position.top),
                    N(p.position.expanded), N(p.position.canvasScroll), selected});
        // Numbered names avoid collisions and invalid filename characters in playlist names.
        files.push_back({L"playlists/" + std::to_wstring(i + 1) + L".m3u8", std::move(m3u)});
    }
    Row(albums, {"gain", "peak", "dynamics", "members"});
    for (const auto& [key, a] : data.albums) {
        std::string members;
        for (auto id : a.members) members += (members.empty() ? "" : ";") + N(id);
        Row(albums, {N(a.gain), N(a.peak), N(a.dynamics), members});
    }
    Row(state, {"initialized", "total", "streak_day", "streak_length", "day", "day_seconds"});
    const auto& p = data.points;
    Row(state,
        {N(data.hasPoints), N(p.total), N(p.streakDay), N(p.streakLength), N(p.dayDay), N(p.daySeconds)});
    Row(days, {"day", "centipoints"});
    for (auto [day, points] : p.byDay) Row(days, {N(day), N(points)});
    Row(recent, {"time", "kind", "track_id", "centipoints"});
    for (const auto& e : p.recent) Row(recent, {Date(e.time), N((unsigned)e.kind), N(e.id), N(e.centi)});
    for (auto pair : {File{L"playlists.csv", std::move(pls)}, File{L"playlist-items.csv", std::move(items)},
                      File{L"views.csv", std::move(views)}, File{L"albums.csv", std::move(albums)},
                      File{L"points.csv", std::move(state)}, File{L"points-days.csv", std::move(days)},
                      File{L"points-recent.csv", std::move(recent)}})
        files.push_back(std::move(pair));
    return files;
}

TagEdit StatisticsTags(const Track& t) {
    TagEdit e;
    if (t.rating) {
        char buf[16];
        snprintf(buf, sizeof buf, "%.2g", t.rating / 100.0);  // FMPS: 0.0 to 1.0
        e.rating = buf;
    }
    if (t.playCount) e.playCount = std::to_string(t.playCount);
    return e;
}

bool ReadStatisticsCsv(std::string_view bytes, std::vector<Statistics>& out, std::wstring& error) {
    Table table;
    if (!Csv(bytes, table)) {
        error = L"The statistics CSV is invalid.";
        return false;
    }
    std::vector<Statistics> rows;
    for (size_t i = 1; i < table.size(); ++i) {
        Reader r{table[0], table[i]};
        auto s = Stat(r);
        if (!r.ok || (!s.hasCount && !s.hasRating && !s.hasSkips && !s.first && !s.last && !s.added)) {
            error = L"Invalid statistics at CSV row " + std::to_wstring(i + 1) + L".";
            return false;
        }
        rows.push_back(std::move(s));
    }
    out = std::move(rows);
    return true;
}

bool Import(const std::vector<File>& files, LibraryArchive& out, std::wstring& error) {
    if (!FormatOne(Find(files, L"backup.ini"))) {
        error = L"This is not a supported cordinal backup (format 1).";
        return false;
    }
    LibraryArchive data;
    Table tracks, pls, items, history, views, albums, points, days, recent;
    if (!ReadTable(files, L"library.csv", tracks, error) || !ReadTable(files, L"playlists.csv", pls, error) ||
        !ReadTable(files, L"playlist-items.csv", items, error) ||
        !ReadTable(files, L"history.csv", history, error) || !ReadTable(files, L"views.csv", views, error) ||
        !ReadTable(files, L"albums.csv", albums, error) || !ReadTable(files, L"points.csv", points, error) ||
        !ReadTable(files, L"points-days.csv", days, error) ||
        !ReadTable(files, L"points-recent.csv", recent, error))
        return false;
    auto bad = [&] {
        error = L"The backup contains invalid values or references. Nothing was restored.";
        return false;
    };
    for (size_t i = 1; i < tracks.size(); ++i) {
        Reader r{tracks[0], tracks[i]};
        auto s = Stat(r);
        auto id = data.library.Add(s.path);
        if (id != i - 1 || !s.hasCount || !s.hasSkips || !s.hasRating) return bad();
        auto& t = data.library[id];
        const char* names[] = {"title", "artist",   "album",   "album_artist", "genre",
                               "date",  "composer", "comment", "grouping",     "label"};
        size_t n = 0;
        for (auto member : {&Track::title, &Track::artist, &Track::album, &Track::albumArtist, &Track::genre,
                            &Track::date, &Track::composer, &Track::comment, &Track::grouping, &Track::label})
            t.*member = data.library.Intern(r.Text(names[n++]));
        t.rating = s.rating;
        t.playCount = s.count;
        t.skipCount = s.skips;
        t.added = s.added;
        t.firstPlayed = s.first;
        t.lastPlayed = s.last;
        t.fileSize = r.Number<uint64_t>("file_size");
        t.fileTime = r.Number<uint64_t>("file_time");
        t.durationMs = r.Number<uint32_t>("duration_ms");
        t.sampleRate = r.Number<uint32_t>("sample_rate");
        t.bitrate = r.Number<uint16_t>("bitrate");
        t.trackNo = r.Number<uint16_t>("track_number");
        t.trackTotal = r.Number<uint16_t>("track_total");
        t.bpm = r.Number<uint16_t>("bpm");
        t.disc = r.Number<uint8_t>("disc");
        t.discTotal = r.Number<uint8_t>("disc_total");
        t.channels = r.Number<uint8_t>("channels");
        t.codec = (Codec)r.Number<uint8_t>("codec");
        t.flags = r.Number<uint16_t>("flags") & ~Track::kFailed;
        t.rgTrackGain = r.Number<int16_t>("track_gain");
        t.rgAlbumGain = r.Number<int16_t>("album_gain");
        t.rgTrackPeak = r.Number<uint16_t>("track_peak");
        t.rgAlbumPeak = r.Number<uint16_t>("album_peak");
        t.rgHigh = r.Number<uint8_t>("high_loudness");
        t.audioHash = r.Number<uint64_t>("audio_hash");
        if (!r.ok || t.codec > kLastCodec) return bad();
    }
    for (size_t i = 1; i < pls.size(); ++i) {
        Reader r{pls[0], pls[i]};
        Playlist p;
        p.name = FromUtf8(r.Text("name"));
        p.kind = (PlaylistKind)r.Number<uint8_t>("kind");
        p.rules = r.Text("rules");
        p.sortColumn = r.Number<int>("sort_column");
        auto ascending = r.Number<unsigned>("sort_ascending");
        p.sortAscending = ascending != 0;
        AutoPlaylist parsed;
        if (!r.ok || p.kind > PlaylistKind::Auto || ascending > 1 ||
            (p.kind == PlaylistKind::Auto && !ParseAutoPlaylist(p.rules, parsed)))
            return bad();
        data.playlists.push_back(std::move(p));
    }
    for (size_t i = 1; i < items.size(); ++i) {
        Reader r{items[0], items[i]};
        auto pl = r.Number<size_t>("playlist_id");
        auto id = r.Number<TrackId>("track_id");
        if (!r.ok || pl >= data.playlists.size() || !data.library.Contains(id)) return bad();
        data.playlists[pl].items.push_back(id);
    }
    for (size_t i = 1; i < history.size(); ++i) {
        Reader r{history[0], history[i]};
        auto id = r.Number<TrackId>("track_id");
        uint32_t time = Time(r, "played_at_utc");
        auto seconds = r.Number<uint16_t>("listened_seconds");
        auto count = r.Number<uint8_t>("counted"), skip = r.Number<uint8_t>("skipped");
        if (!r.ok || !data.library.Contains(id) || !time || count > 1 || skip > 1 || (!count && !skip))
            return bad();
        data.log.push_back({id, time, seconds, (uint8_t)(count | skip << 1)});
    }
    auto ids = [&](std::string_view s, std::vector<TrackId>& dest, uint64_t limit) {
        while (!s.empty()) {
            auto end = std::min(s.find(';'), s.size());
            TrackId id;
            auto result = std::from_chars(s.data(), s.data() + end, id);
            if (result.ec != std::errc() || result.ptr != s.data() + end || id >= limit) return false;
            dest.push_back(id);
            s.remove_prefix(std::min(end + 1, s.size()));
        }
        return true;
    };
    if (views.size() != data.playlists.size() + 1) return bad();
    for (size_t i = 1; i < views.size(); ++i) {
        Reader r{views[0], views[i]};
        auto pl = r.Number<size_t>("playlist_id");
        if (pl != i - 1) return bad();
        auto& p = data.playlists[pl];
        auto& v = p.position;
        v.search = FromUtf8(r.Text("search"));
        v.focus = r.Number<TrackId>("focus");
        v.top = r.Number<TrackId>("top");
        v.expanded = r.Number<TrackId>("expanded");
        v.canvasScroll = r.Number<int>("scroll");
        uint64_t limit = p.Editable() ? p.items.size() : data.library.Count();
        auto valid = [&](TrackId id) { return id == kInvalidTrack || id < limit; };
        if (!r.ok || v.canvasScroll < 0 || !valid(v.focus) || !valid(v.top) || !valid(v.expanded) ||
            !ids(r.Text("selected"), v.selected, limit))
            return bad();
    }
    for (size_t i = 1; i < albums.size(); ++i) {
        Reader r{albums[0], albums[i]};
        AlbumMeasurement a;
        a.gain = r.Number<int16_t>("gain");
        a.peak = r.Number<uint16_t>("peak");
        a.dynamics = r.Number<uint8_t>("dynamics");
        if (!ids(r.Text("members"), a.members, data.library.Count()) || a.members.empty() || !r.ok)
            return bad();
        if (!data.albums.emplace(AlbumKeyOf(data.library[a.members.front()]), std::move(a)).second)
            return bad();
    }
    if (points.size() != 2) return bad();
    Reader pr{points[0], points[1]};
    auto& p = data.points;
    auto initialized = pr.Number<unsigned>("initialized");
    data.hasPoints = initialized != 0;
    p.total = pr.Number<uint64_t>("total");
    p.streakDay = pr.Number<int64_t>("streak_day");
    p.streakLength = pr.Number<uint32_t>("streak_length");
    p.dayDay = pr.Number<int64_t>("day");
    p.daySeconds = pr.Number<uint32_t>("day_seconds");
    if (!pr.ok || initialized > 1) return bad();
    for (size_t i = 1; i < days.size(); ++i) {
        Reader r{days[0], days[i]};
        auto day = r.Number<uint32_t>("day");
        auto centi = r.Number<uint64_t>("centipoints");
        if (!r.ok || (!p.byDay.empty() && day <= p.byDay.back().first)) return bad();
        p.byDay.emplace_back(day, centi);
    }
    if (recent.size() > 501) return bad();
    for (size_t i = 1; i < recent.size(); ++i) {
        Reader r{recent[0], recent[i]};
        PointsEvent e;
        e.time = Time(r, "time");
        e.kind = (PointsKind)r.Number<uint8_t>("kind");
        e.id = r.Number<TrackId>("track_id");
        e.centi = r.Number<uint64_t>("centipoints");
        if (!r.ok || e.kind > PointsKind::BackPay || (e.id != kInvalidTrack && !data.library.Contains(e.id)))
            return bad();
        p.recent.push_back(e);
    }
    out = std::move(data);
    return true;
}

StatisticsPlan PlanStatistics(const Library& library, const std::vector<Statistics>& rows,
                              const std::vector<PathMap>& maps) {
    StatisticsPlan plan;
    std::set<TrackId> seen;
    for (const auto& s : rows) {
        auto id = library.Find(RemapPath(s.path, maps));
        if (id == kInvalidTrack) {
            ++plan.unmatched;
            continue;
        }
        if (!seen.insert(id).second) {
            ++plan.conflicts;
            continue;
        }
        if (s.hasRating && library[id].rating && library[id].rating != s.rating) ++plan.conflicts;
        plan.matched.emplace_back(id, s);
    }
    return plan;
}

void ApplyStatistics(Library& library, const StatisticsPlan& plan) {
    for (const auto& [id, s] : plan.matched)
        if (library.Contains(id)) {
            auto& t = library[id];
            if (s.hasCount) t.playCount = std::max(t.playCount, s.count);
            if (s.hasSkips) t.skipCount = std::max(t.skipCount, s.skips);
            if (s.hasRating && !t.rating) t.rating = s.rating;
            if (s.added && (!t.added || s.added < t.added)) t.added = s.added;
            if (s.first && (!t.firstPlayed || s.first < t.firstPlayed)) t.firstPlayed = s.first;
            t.lastPlayed = std::max(t.lastPlayed, s.last);
        }
    if (!plan.matched.empty()) library.Touch();
}
} // namespace migration
