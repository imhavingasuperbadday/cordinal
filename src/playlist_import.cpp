// See playlist_import.h. Parsers first (Spotify's two export files, then CSV), then the
// library split and the assembly that rebuilds a list's order from a finished run.
#include "playlist_import.h"

#include "library.h"     // ToUtf8 / FromUtf8 / StemOf / ExtOf
#include "tagwriter.h"   // ReadFileBytes
#include "web_remote.h"  // ParseJson

#include <algorithm>
#include <cstdlib>
#include <map>

namespace import {

namespace {

std::string_view StripBom(std::string_view text) {
    return text.substr(0, 3) == "\xEF\xBB\xBF" ? text.substr(3) : text;
}

std::string_view TrimLeft(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' ||
                             text.front() == '\n'))
        text.remove_prefix(1);
    return text;
}

// Case and spacing are the only differences a match should forgive: ASCII lowercased (UTF-8
// bytes pass through untouched, so accented names compare exactly), runs of spaces one,
// edges trimmed. Anything stricter than this and the same song stops being the same song.
std::string Norm(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    bool space = true;  // leading spaces never make it in
    for (unsigned char c : s) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            space = true;
            continue;
        }
        if (space && !out.empty()) out += ' ';
        space = false;
        out += (char)(c < 0x80 ? std::tolower(c) : c);
    }
    return out;
}

std::string Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return std::string(s);
}

// One CSV record: quoted fields may hold commas, quotes doubled, and line breaks. `at` moves
// past the record's line ending; a final record without one still comes back whole.
bool NextCsvRecord(std::string_view text, size_t& at, std::vector<std::string>& cells) {
    if (at >= text.size()) return false;
    cells.clear();
    std::string cell;
    bool quoted = false;
    while (at < text.size()) {
        const char c = text[at];
        if (quoted) {
            if (c == '"') {
                if (at + 1 < text.size() && text[at + 1] == '"') {
                    cell += '"';
                    at += 2;
                } else {
                    quoted = false;
                    ++at;
                }
            } else {
                cell += c;
                ++at;
            }
            continue;
        }
        if (c == '"' && cell.empty()) {
            quoted = true;
            ++at;
            continue;
        }
        if (c == ',') {
            cells.push_back(cell);
            cell.clear();
            ++at;
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (c == '\r' && at + 1 < text.size() && text[at + 1] == '\n') ++at;
            ++at;
            cells.push_back(cell);
            return true;
        }
        cell += c;
        ++at;
    }
    cells.push_back(cell);
    return true;
}

// -----------------------------------------------------------------------
// CSV column names, as every moving service spells them

struct CsvColumns {
    int title = -1, artist = -1, album = -1, duration = -1, uri = -1, playlist = -1;
    bool durationSeconds = false;
};

bool OneOf(std::string_view name, std::initializer_list<const char*> set) {
    for (const char* word : set)
        if (name == word) return true;
    return false;
}

CsvColumns MapCsvHeader(const std::vector<std::string>& cells) {
    CsvColumns out;
    for (int i = 0; i < (int)cells.size(); ++i) {
        const std::string name = Norm(Trim(cells[i]));
        if (OneOf(name, {"track name", "track", "title", "song title", "song", "name"})) out.title = i;
        else if (OneOf(name, {"artist name(s)", "artist name", "artist(s)", "artist names", "artists", "artist"}))
            out.artist = i;
        else if (OneOf(name, {"album name", "album"})) out.album = i;
        else if (OneOf(name, {"playlist name", "playlist"})) out.playlist = i;
        else if (OneOf(name, {"duration (s)", "time (s)", "length (s)"})) {
            out.duration = i;
            out.durationSeconds = true;
        } else if (OneOf(name, {"duration (ms)", "duration_ms", "durationms", "duration", "time (ms)",
                                "length (ms)", "length", "time"}))
            out.duration = i;
        else if (OneOf(name, {"track uri", "spotify uri", "uri", "spotify track uri"})) out.uri = i;
    }
    return out;
}

unsigned NumberText(std::string_view text) {
    unsigned long long value = 0;
    for (char c : text)
        if (c >= '0' && c <= '9') value = value * 10 + (unsigned)(c - '0');
        else if (c != ' ' && c != '\t') break;  // a stray unit or prefix stops the number
    return (unsigned)std::min<unsigned long long>(value, 3600000000ull);
}

// "212000" is milliseconds (the column said so), "3:45" is always a clock, and an unmarked
// number is taken as milliseconds the way the known exports write it.
unsigned DurationCell(std::string_view text, bool columnSeconds) {
    const std::string cell = Trim(text);
    if (cell.find(':') != std::string::npos) {
        unsigned long long seconds = 0, part = 0;
        for (char c : cell) {
            if (c == ':') {
                seconds = seconds * 60 + part;
                part = 0;
            } else if (c >= '0' && c <= '9') {
                part = part * 10 + (unsigned)(c - '0');
            } else {
                return 0;
            }
        }
        seconds = seconds * 60 + part;
        return (unsigned)std::min<unsigned long long>(seconds * 1000, 3600000000ull);
    }
    const unsigned value = NumberText(cell);
    return columnSeconds ? (unsigned)std::min<unsigned long long>((unsigned long long)value * 1000, 3600000000ull)
                         : value;
}

// -----------------------------------------------------------------------
// Spotify ids

std::string TrackIdOfUri(std::string_view uri) {
    size_t at = uri.find("spotify:track:");
    if (at != std::string_view::npos) return Trim(uri.substr(at + 14));
    at = uri.find("open.spotify.com/track/");
    if (at != std::string_view::npos) {
        std::string id = Trim(uri.substr(at + 23));
        const size_t cut = id.find_first_of("/?#");
        return cut == std::string::npos ? id : Trim(id.substr(0, cut));
    }
    return "";
}

std::string AlbumLinkOfUri(std::string_view uri) {
    size_t at = uri.find("spotify:album:");
    if (at != std::string_view::npos) return "https://open.spotify.com/album/" + Trim(uri.substr(at + 14));
    at = uri.find("open.spotify.com/album/");
    if (at != std::string_view::npos) return "https://" + Trim(std::string(uri.substr(at)));
    return "";
}

std::string JoinNames(const web::JsonValue& array) {
    std::string out;
    for (const web::JsonValue& a : array.items) {
        const std::string name = a.kind == web::JsonValue::Kind::Object ? a.String("name") : a.text;
        if (name.empty()) continue;
        if (!out.empty()) out += ", ";
        out += name;
    }
    return out;
}

// The two exports spell the artist three ways: an object with a name, a plain string, or a
// list of artists (the playlist export's way).
std::string ArtistOf(const web::JsonValue& row) {
    const web::JsonValue& artist = row["artist"];
    if (artist.kind == web::JsonValue::Kind::Object && !artist.String("name").empty()) return artist.String("name");
    if (artist.kind == web::JsonValue::Kind::String && !artist.text.empty()) return artist.text;
    return JoinNames(row["artists"]);
}

std::string AlbumOf(const web::JsonValue& row) {
    const web::JsonValue& album = row["album"];
    if (album.kind == web::JsonValue::Kind::Object) return album.String("name");
    return album.kind == web::JsonValue::Kind::String ? album.text : std::string();
}

// A row nobody can use: a local file the service only remembered, whose bytes the user
// already has, or an entry the service itself removed.
bool UnusableRow(const web::JsonValue& item, const web::JsonValue& track) {
    return item["episode"].kind != web::JsonValue::Kind::Null ||      // a podcast episode
           track["local"].text == "true" ||                           // the user's own file
           track["episode"].text == "true" ||                         // an episode wrapped as a track
           track.String("uri").rfind("spotify:local", 0) == 0;
}

}  // namespace

// -----------------------------------------------------------------------
// Spotify exports

bool ParseYourLibrary(std::string_view json, ImportList& liked, std::vector<std::string>& albumLinks) {
    web::JsonValue root;
    if (!web::ParseJson(json, root) || root.kind != web::JsonValue::Kind::Object) return false;
    liked.title = "Liked Songs";  // the name Spotify's own saved-tracks list carries
    liked.favorites = true;      // saved tracks are hearts: the import marks them so
    for (const web::JsonValue& row : root["tracks"].items) {
        if (row.kind != web::JsonValue::Kind::Object || UnusableRow(row, row)) {
            ++liked.skipped;
            continue;
        }
        ImportTrack t;
        t.uri = row.String("uri");
        t.title = row.String("name");
        t.artist = ArtistOf(row);
        t.album = AlbumOf(row);
        t.durationMs = NumberText(row["duration_ms"].text);
        if (t.title.empty() || t.artist.empty()) {
            ++liked.skipped;
            continue;
        }
        liked.tracks.push_back(std::move(t));
    }
    for (const web::JsonValue& row : root["albums"].items) {
        if (row.kind != web::JsonValue::Kind::Object) continue;
        std::string link = AlbumLinkOfUri(row.String("uri"));
        if (!link.empty()) albumLinks.push_back(link);
    }
    return !liked.tracks.empty() || !albumLinks.empty();
}

bool ParsePlaylist1(std::string_view json, std::vector<ImportList>& lists) {
    web::JsonValue root;
    if (!web::ParseJson(json, root) || root.kind != web::JsonValue::Kind::Object) return false;
    for (const web::JsonValue& pl : root["playlists"].items) {
        if (pl.kind != web::JsonValue::Kind::Object) continue;
        ImportList list;
        list.title = pl.String("name");
        if (list.title.empty()) list.title = "Spotify playlist";
        for (const web::JsonValue& item : pl["items"].items) {
            const web::JsonValue& track = item["track"];
            if (item.kind != web::JsonValue::Kind::Object || track.kind != web::JsonValue::Kind::Object ||
                UnusableRow(item, track)) {
                ++list.skipped;
                continue;
            }
            ImportTrack t;
            t.uri = track.String("uri");
            t.title = track.String("name");
            t.artist = ArtistOf(track);
            t.album = AlbumOf(track);
            t.durationMs = NumberText(track["duration_ms"].text);
            if (t.title.empty() || t.artist.empty()) {
                ++list.skipped;
                continue;
            }
            list.tracks.push_back(std::move(t));
        }
        if (!list.tracks.empty()) lists.push_back(std::move(list));
    }
    return !lists.empty();
}

// -----------------------------------------------------------------------
// CSV

bool ParseCsv(std::string_view text, const std::string& fallbackTitle, std::vector<ImportList>& lists) {
    std::string_view body = StripBom(text);
    std::vector<std::string> cells;
    size_t at = 0;
    if (!NextCsvRecord(body, at, cells)) return false;
    const CsvColumns cols = MapCsvHeader(cells);
    if (cols.title < 0) return false;

    // A Playlist Name column carries several lists in one file; rows without it (or with it
    // blank) fall back to the file's own name.
    std::map<std::string, size_t> byTitle;
    auto listOf = [&](const std::string& name) -> size_t {
        const std::string key = name.empty() ? fallbackTitle : name;
        auto it = byTitle.find(key);
        if (it != byTitle.end()) return it->second;
        lists.push_back({key, {}, 0});
        byTitle[key] = lists.size() - 1;
        return lists.size() - 1;
    };
    auto rowList = [&]() -> size_t {
        if (cols.playlist < 0 || cols.playlist >= (int)cells.size()) return listOf("");
        return listOf(Trim(cells[cols.playlist]));
    };
    while (NextCsvRecord(body, at, cells)) {
        if (cells.empty()) continue;
        ImportTrack t;
        if (cols.title < (int)cells.size()) t.title = Trim(cells[cols.title]);
        if (t.title.empty()) {
            ++lists[rowList()].skipped;
            continue;
        }
        if (cols.artist >= 0 && cols.artist < (int)cells.size()) t.artist = Trim(cells[cols.artist]);
        if (cols.album >= 0 && cols.album < (int)cells.size()) t.album = Trim(cells[cols.album]);
        if (cols.duration >= 0 && cols.duration < (int)cells.size())
            t.durationMs = DurationCell(cells[cols.duration], cols.durationSeconds);
        if (cols.uri >= 0 && cols.uri < (int)cells.size()) {
            const std::string uri = Trim(cells[cols.uri]);
            // A uri is only useful when it names a Spotify track: anything else (a YouTube id,
            // a plain path) would just mislabel the row, so the queue matches it by name.
            t.uri = TrackIdOfUri(uri).empty() ? "" : uri;
        }
        lists[rowList()].tracks.push_back(std::move(t));
    }
    return !lists.empty();
}

// -----------------------------------------------------------------------
// Sniffing pasted text and reading files

bool LooksLikeImport(std::string_view text) {
    const std::string_view body = TrimLeft(StripBom(text));
    if (body.empty()) return false;
    if (body.front() == '{')
        return body.find("spotify:") != std::string_view::npos || body.find("\"playlists\"") != std::string_view::npos;
    std::vector<std::string> cells;
    size_t at = 0;
    if (!NextCsvRecord(body, at, cells)) return false;
    const CsvColumns cols = MapCsvHeader(cells);
    return cols.title >= 0 && (cols.artist >= 0 || cols.uri >= 0);
}

bool ParseImportText(std::string_view text, const std::wstring& fallbackTitle,
                     std::vector<ImportList>& lists, std::vector<std::string>& albumLinks, std::string& error) {
    lists.clear();
    albumLinks.clear();
    error.clear();
    const std::string_view body = TrimLeft(StripBom(text));
    if (!body.empty() && body.front() == '{') {
        web::JsonValue root;
        if (!web::ParseJson(body, root) || root.kind != web::JsonValue::Kind::Object) {
            error = "that text is neither a Spotify export nor a CSV playlist";
            return false;
        }
        if (root["playlists"].kind == web::JsonValue::Kind::Array) {
            if (!ParsePlaylist1(body, lists)) {
                error = "that export holds no playlist with tracks in it";
                return false;
            }
            return true;
        }
        if (root["tracks"].kind == web::JsonValue::Kind::Array || root["albums"].kind == web::JsonValue::Kind::Array) {
            ImportList liked;
            if (!ParseYourLibrary(body, liked, albumLinks)) {
                error = "that export holds no saved tracks or albums";
                return false;
            }
            if (!liked.tracks.empty()) lists.push_back(std::move(liked));
            return true;
        }
        error = "that JSON file is not a Spotify export";
        return false;
    }
    if (!ParseCsv(body, ToUtf8(fallbackTitle), lists)) {
        error = "that CSV has no Track Name column";
        return false;
    }
    return true;
}

bool ParseImportFile(const std::wstring& path, std::vector<ImportList>& lists,
                     std::vector<std::string>& albumLinks, std::string& error) {
    std::string text;
    if (!ReadFileBytes(path, text)) {
        error = "that file could not be read";
        return false;
    }
    return ParseImportText(text, StemOf(path), lists, albumLinks, error);
}

bool IsImportFileName(const std::wstring& path) {
    const std::wstring ext = ExtOf(path);
    return ext == L"json" || ext == L"csv";
}

// -----------------------------------------------------------------------
// The library split and the assembly

void OwnedIndex::Add(std::string_view artist, std::string_view title, const std::wstring& path) {
    std::string a = Norm(artist), t = Norm(title);
    if (a.empty() || t.empty()) return;
    byName_.emplace(std::pair{a, t}, path);  // the first track that claims a name keeps it
}

bool OwnedIndex::Find(std::string_view artist, std::string_view title, std::wstring& path) const {
    std::string a = Norm(artist), t = Norm(title);
    if (a.empty() || t.empty()) return false;
    auto it = byName_.find({a, t});
    if (it != byName_.end()) {
        path = it->second;
        return true;
    }
    // "A, B" is how a featuring is written; A's own file in the library is still the song.
    const size_t comma = a.find(',');
    if (comma == std::string::npos) return false;
    const std::string first = Norm(a.substr(0, comma));
    if (first.empty()) return false;
    it = byName_.find({first, t});
    if (it != byName_.end()) {
        path = it->second;
        return true;
    }
    return false;
}

size_t ImportPlanList::OwnedCount() const {
    size_t n = 0;
    for (const ImportSlot& s : slots)
        if (s.missing < 0 && !s.owned.empty()) ++n;
    return n;
}

size_t ImportPlan::TrackCount() const {
    size_t n = 0;
    for (const ImportPlanList& l : lists) n += l.slots.size();
    return n;
}

size_t ImportPlan::OwnedCount() const {
    size_t n = 0;
    for (const ImportPlanList& l : lists) n += l.OwnedCount();
    return n;
}

size_t ImportPlan::MissingCount() const {
    size_t n = 0;
    for (const ImportPlanList& l : lists) n += l.missing.size();
    return n;
}

void SplitByLibrary(const std::vector<ImportList>& lists, const OwnedIndex& owned, ImportPlan& plan) {
    plan.lists.clear();
    plan.albumLinks.clear();
    for (const ImportList& list : lists) {
        ImportPlanList out;
        out.title = list.title;
        out.favorites = list.favorites;
        for (const ImportTrack& row : list.tracks) {
            std::wstring have;
            if (owned.Find(row.artist, row.title, have)) {
                out.slots.push_back({-1, have});
                continue;
            }
            // A Spotify-style row: the queue's match keeps the row's own tags and only takes
            // the audio (and now the cover, when the row has none of its own) from YouTube.
            WebTrack t;
            t.site = "spotify";
            t.id = TrackIdOfUri(row.uri);
            if (t.id.empty()) t.id = "name:" + row.artist + "\x1f" + row.title;  // stable across re-imports
            t.title = row.title;
            t.artist = row.artist;
            t.album = row.album;
            t.durationSec = (row.durationMs + 999) / 1000;
            out.slots.push_back({(int)out.missing.size(), {}});
            out.missing.push_back(std::move(t));
        }
        if (!out.slots.empty()) plan.lists.push_back(std::move(out));
    }
}

std::vector<ImportSlot> RangeSlots(size_t count) {
    std::vector<ImportSlot> slots(count);
    for (size_t i = 0; i < count; ++i) slots[i].missing = (int)i;
    return slots;
}

std::vector<std::wstring> AssembleFiles(const std::vector<WebFetchItem>& items, size_t first,
                                        const std::vector<ImportSlot>& slots, const OwnedIndex& owned) {
    std::vector<std::wstring> files;
    for (const ImportSlot& slot : slots) {
        if (slot.missing < 0) {
            if (!slot.owned.empty()) files.push_back(slot.owned);
            continue;
        }
        const size_t index = first + (size_t)slot.missing;
        if (index >= items.size()) continue;
        const WebFetchItem& it = items[index];
        if ((it.state == WebState::Done || it.state == WebState::Exists) && !it.file.empty()) {
            files.push_back(it.file);
        } else if (it.state == WebState::Failed) {
            // A link-resolved playlist can fail on a song the library has had all along:
            // the row still deserves its place, held by the copy the user already owns.
            std::wstring have;
            if (owned.Find(it.track.artist, it.track.title, have)) files.push_back(have);
        }
    }
    return files;
}

}  // namespace import
