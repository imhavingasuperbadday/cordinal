// Browser views over an immutable library snapshot. Queries and grouping run on request workers.
#include "web_internal.h"
#include "library.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <set>

namespace web {
namespace {
// Owned grouping fields share the same lexical order without general tuple construction/comparison.
struct GroupKey {
    std::string artist, album, date;
    bool operator<(const GroupKey& other) const {
        if (int order = artist.compare(other.artist)) return order < 0;
        if (int order = album.compare(other.album)) return order < 0;
        return date.compare(other.date) < 0;
    }
};
static_assert(sizeof(GroupKey) == 3 * sizeof(std::string));
}

std::string TrackJson(const Track& t) {
    return "{\"id\":" + Json(t.key) + ",\"title\":" + Json(t.title) + ",\"artist\":" + Json(t.artist) +
        ",\"album\":" + Json(t.album) + ",\"albumArtist\":" + Json(t.albumArtist) + ",\"date\":" + Json(t.date) +
        ",\"genre\":" + Json(t.genre) + ",\"codec\":" + Json(t.codec) + ",\"mime\":" + Json(t.mime) +
        ",\"duration\":" + std::to_string(t.duration) + ",\"number\":" + std::to_string(t.number) +
        ",\"disc\":" + std::to_string(t.disc) + ",\"plays\":" + std::to_string(t.plays) +
        ",\"original\":" + (t.original ? "true" : "false") + '}';
}
namespace {
std::string Value(const Request& r, const char* key) {
    auto it = r.query.find(key); return it == r.query.end() ? "" : it->second;
}
std::wstring Fold(std::string_view value) {
    std::wstring s = FromUtf8(value);
    if (!s.empty()) LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, s.data(), (int)s.size(),
                                  s.data(), (int)s.size(), nullptr, nullptr, 0);
    return s;
}
bool Search(const Track& t, const std::vector<std::wstring>& words) {
    if (words.empty()) return true;
    auto text = Fold(t.title + " " + t.artist + " " + t.albumArtist + " " + t.album + " " + t.genre + " " + t.date);
    for (const auto& word : words) if (text.find(word) == text.npos) return false;
    return true;
}
bool HasArtist(const Track& t, const std::string& artist) {
    if (artist.empty() || t.albumArtist == artist || t.artist == artist) return true;
    for (size_t at = 0; at < t.artist.size();) {
        size_t end = t.artist.find(';', at); if (end == t.artist.npos) end = t.artist.size();
        auto name = std::string_view(t.artist).substr(at, end - at);
        while (!name.empty() && name.front() == ' ') name.remove_prefix(1);
        while (!name.empty() && name.back() == ' ') name.remove_suffix(1);
        if (name == artist) return true;
        at = end + 1;
    }
    return false;
}
}
std::string Catalog(const Snapshot& s, const Request& r) {
    std::string kind = Value(r, "view"), playlist = Value(r, "playlist"), album = Value(r, "album"),
        artist = Value(r, "artist"), date = Value(r, "date");
    size_t offset = (size_t)strtoull(Value(r, "offset").c_str(), nullptr, 10);
    size_t limit = (size_t)std::clamp(strtoul(Value(r, "limit").c_str(), nullptr, 10), 1ul, 200ul);
    if (Value(r, "limit").empty()) limit = 100;
    if (kind == "playlists") {
        std::string out = "{\"items\":[";
        for (size_t i = 0; i < s.playlists.size(); ++i) {
            if (i) out += ',';
            const auto& p = s.playlists[i];
            out += "{\"id\":" + Json(p.key) + ",\"name\":" + Json(p.name) + ",\"count\":" +
                std::to_string(p.tracks.size()) + '}';
        }
        return out + "]}";
    }
    auto words = FilterWords(Fold(Value(r, "q")));
    std::vector<const Track*> tracks;
    if (!playlist.empty()) {
        auto p = std::find_if(s.playlists.begin(), s.playlists.end(), [&](const auto& p) { return p.key == playlist; });
        if (p != s.playlists.end()) {
            for (const auto& id : p->tracks)
                if (auto t = s.byKey.find(id); t != s.byKey.end()) tracks.push_back(&s.tracks[t->second]);
        }
    } else for (const auto& t : s.tracks) if (t.library) tracks.push_back(&t);
    std::erase_if(tracks, [&](const Track* t) {
        bool albumView = r.query.count("album") != 0;
        bool artistMatch = albumView && r.query.count("artist") ? t->albumArtist == artist :
            r.query.count("artist") && artist.empty() ? t->artist.empty() && t->albumArtist.empty() : HasArtist(*t, artist);
        return (albumView && t->album != album) ||
            (r.query.count("date") && t->date != date) || !artistMatch || !Search(*t, words);
    });
    std::vector<std::string> items;
    if (kind == "albums" || kind == "artists") {
        std::map<GroupKey, std::vector<const Track*>> groups;
        for (const auto* t : tracks) {
            if (kind == "albums") groups[{t->albumArtist, t->album, t->date}].push_back(t);
            else {
                std::string names = t->artist.empty() ? t->albumArtist : t->artist;
                for (size_t at = 0; at <= names.size();) {
                    size_t end = names.find(';', at); if (end == names.npos) end = names.size();
                    std::string name = names.substr(at, end - at);
                    while (!name.empty() && name.front() == ' ') name.erase(name.begin());
                    while (!name.empty() && name.back() == ' ') name.pop_back();
                    groups[{name, "", ""}].push_back(t);
                    if (end == names.size()) break;
                    at = end + 1;
                }
            }
        }
        for (const auto& [key, members] : groups) {
            const auto& [name, title, groupDate] = key;
            items.push_back("{\"name\":" + Json(kind == "albums" ? title : name) + ",\"artist\":" + Json(name) +
                ",\"date\":" + Json(groupDate) + ",\"cover\":" + Json(members.front()->key) +
                ",\"count\":" + std::to_string(members.size()) + '}');
        }
    } else {
        std::string out = "{\"revision\":" + std::to_string(s.revision) + ",\"total\":" +
            std::to_string(tracks.size()) + ",\"items\":[";
        for (size_t i = offset; i < tracks.size() && i - offset < limit; ++i) {
            if (i != offset) out += ',';
            out += TrackJson(*tracks[i]);
        }
        return out + "]}";
    }
    std::string out = "{\"revision\":" + std::to_string(s.revision) + ",\"total\":" + std::to_string(items.size()) +
        ",\"items\":[";
    for (size_t i = offset; i < items.size() && i - offset < limit; ++i) {
        if (i != offset) out += ',';
        out += items[i];
    }
    return out + "]}";
}
}  // namespace web
