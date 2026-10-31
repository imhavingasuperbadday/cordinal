// Playlist and search helpers with no Win32-control dependency: name sanitizing, item ordering,
// filter parsing and track matching. Shared by the UI and the persistence code.
#include "album_editions.h"
#include "library.h"
#include "playlog.h"
#include "playlist_shuffle.h"
#include "sort.h"

#include <algorithm>
#include <cwctype>

void ShuffleByListening(std::vector<uint32_t>& order, const std::vector<TrackId>& items, std::mt19937& rng) {
    auto weights = ListeningWeights(items);
    struct Pick { double key; uint32_t index; };
    std::vector<Pick> picks;
    picks.reserve(order.size());
    for (uint32_t index : order) {
        if (index >= weights.size()) return;
        double u = std::max(1e-12, std::generate_canonical<double, 53>(rng));
        picks.push_back({-std::log(u) / weights[index], index});
    }
    // Exponential keys give a weighted permutation without dropping any occurrence.
    SortRecords(picks.begin(), picks.end(), [](const Pick& a, const Pick& b) { return a.key > b.key; });
    for (size_t i = 0; i < picks.size(); ++i) order[i] = picks[i].index;
}

void ShuffleAlbums(std::vector<uint32_t>& order, const std::vector<TrackId>& items, std::mt19937& rng, int current) {
    order.resize(items.size());
    for (uint32_t i = 0; i < items.size(); ++i) order[i] = i;
    SortRecords(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        const Track& x = g_lib[items[a]], &y = g_lib[items[b]];
        auto left = AlbumKeyOf(x), right = AlbumKeyOf(y);
        if (left != right) return left < right;
        return std::tie(x.disc, x.trackNo, a) < std::tie(y.disc, y.trackNo, b);
    });
    struct Album { uint32_t first, end; };
    std::vector<Album> albums;
    for (uint32_t first = 0; first < order.size();) {
        uint32_t end = first + 1;
        auto key = AlbumKeyOf(g_lib[items[order[first]]]);
        while (end < order.size() && AlbumKeyOf(g_lib[items[order[end]]]) == key) ++end;
        albums.push_back({first, end});
        first = end;
    }
    // Shuffle albums themselves: picking a random track would favour longer albums.
    std::shuffle(albums.begin(), albums.end(), rng);
    if (current >= 0 && current < (int)items.size()) {
        auto key = AlbumKeyOf(g_lib[items[current]]);
        auto it = std::find_if(albums.begin(), albums.end(), [&](const Album& a) {
            return AlbumKeyOf(g_lib[items[order[a.first]]]) == key;
        });
        std::iter_swap(albums.begin(), it);
    }
    std::vector<uint32_t> shuffled;
    shuffled.reserve(order.size());
    for (const auto& a : albums) shuffled.insert(shuffled.end(), order.begin() + a.first, order.begin() + a.end);
    order.swap(shuffled);
}

namespace {
// Every space-like code point Windows treats as whitespace, so filter and name parsing split on
// the same boundaries for every script.
bool Space(wchar_t ch) {
    return iswspace(ch) || ch == 0xA0 || (ch >= 0x2000 && ch <= 0x200A) || ch == 0x2028 || ch == 0x2029 ||
           ch == 0x202F || ch == 0x205F || ch == 0x3000;
}
}

// Trims, caps at 128 code units on a code-point boundary, replaces control characters, and
// substitutes a default name for an empty result.
std::wstring CleanPlaylistName(std::wstring_view name) {
    while (!name.empty() && Space(name.front())) name.remove_prefix(1);
    while (!name.empty() && Space(name.back())) name.remove_suffix(1);
    std::wstring result(name.substr(0, 128));
    if (!result.empty() && result.back() >= 0xD800 && result.back() <= 0xDBFF) result.pop_back();
    for (auto& ch : result) if (ch < L' ' || ch == 127) ch = L' ';
    return result.empty() ? L"New Playlist" : result;
}

// Appends " (2)", " (3)", ... until the name is distinct (case-insensitive), keeping the
// 128-code-unit cap.
std::wstring UniquePlaylistName(const std::vector<Playlist>& playlists, std::wstring_view base) {
    std::wstring clean = CleanPlaylistName(base);
    auto exists = [&](const std::wstring& name) {
        return std::any_of(playlists.begin(), playlists.end(), [&](const Playlist& pl) {
            return CompareStringOrdinal(pl.name.c_str(), -1, name.c_str(), -1, TRUE) == CSTR_EQUAL;
        });
    };
    if (!exists(clean)) return clean;
    for (size_t suffix = 2;; ++suffix) {
        std::wstring tail = L" (" + std::to_wstring(suffix) + L")";
        auto prefix = clean.substr(0, 128 - tail.size());
        if (!prefix.empty() && prefix.back() >= 0xD800 && prefix.back() <= 0xDBFF) prefix.pop_back();
        std::wstring name = prefix + tail;
        if (!exists(name)) return name;
    }
}

// Adjusts an index for moving `from` to `to`, as the list control reports selection after a move.
int RemapPlaylistIndex(int index, int from, int to) {
    if (index < 0) return index;
    if (index == from) return to;
    if (from < to && index > from && index <= to) return index - 1;
    if (from > to && index >= to && index < from) return index + 1;
    return index;
}

// Moves one playlist to another position, preserving the order of everything in between.
bool MovePlaylist(std::vector<Playlist>& playlists, int from, int to) {
    if (from < 0 || to < 0 || from >= (int)playlists.size() || to >= (int)playlists.size() || from == to) return false;
    if (from < to) std::rotate(playlists.begin() + from, playlists.begin() + from + 1, playlists.begin() + to + 1);
    else std::rotate(playlists.begin() + to, playlists.begin() + from, playlists.begin() + from + 1);
    return true;
}

// Splits a search box into words; double quotes group a phrase into one word.
std::vector<std::wstring> FilterWords(std::wstring_view filter) {
    std::vector<std::wstring> words;
    std::wstring word;
    bool quoted = false;
    for (wchar_t ch : filter) {
        if (ch == L'"') quoted = !quoted;
        else if (!quoted && Space(ch)) {
            if (!word.empty()) { words.push_back(std::move(word)); word.clear(); }
        } else word += ch;
    }
    if (!word.empty()) words.push_back(std::move(word));
    return words;
}

// True when every word is a case-insensitive substring of some searchable field. An empty word list
// matches everything; all words must match.
bool MatchesTrack(const Library& library, TrackId id, const std::vector<std::wstring>& words) {
    return TrackMatcher(library, words).Matches(id);
}

bool TrackMatcher::Matches(TrackId id) {
    if (!library_.Contains(id)) return false;
    if (words_.empty()) return true;
    const auto& track = library_[id];
    const StringPool::Handle handles[] = {track.artist, track.title, track.album, track.albumArtist, track.genre, track.date,
                                         track.file, track.dir, track.composer, track.comment, track.grouping, track.label,
                                         (StringPool::Handle)track.codec + 1};
    for (size_t wordIndex = 0; wordIndex < words_.size(); ++wordIndex) {
        const auto& word = words_[wordIndex];
        // Long queries retain their full meaning; words beyond the mask are searched normally.
        const uint64_t bit = wordIndex < 64 ? uint64_t(1) << wordIndex : 0;
        bool match = false;
        for (size_t i = 0; i < std::size(fields_); ++i) {
            if (!handles[i]) continue;
            auto& field = fields_[i];
            if (field.handle != handles[i]) {
                field.text = i < 12 ? library_.Str(handles[i]) : CodecName(track.codec);
                field.handle = handles[i];
                field.tested = field.found = 0;
            }
            if (field.tested & bit) match = (field.found & bit) != 0;
            else {
                match = !field.text.empty() &&
                    FindNLSStringEx(LOCALE_NAME_USER_DEFAULT, FIND_FROMSTART | LINGUISTIC_IGNORECASE,
                        field.text.data(), (int)field.text.size(), word.data(), (int)word.size(),
                        nullptr, nullptr, nullptr, 0) >= 0;
                field.tested |= bit;
                if (match) field.found |= bit;
            }
            if (match) break;
        }
        if (!match) {
            // Edition labels are searchable too ("deluxe", "instrumental", a regional name), but
            // only for tracks in an explicit group; ordinary albums add no work here.
            if (editionTrack_ != id) {
                editionTrack_ = id;
                editionText_ = AlbumEditionSearchText(id);
                edition_.tested = edition_.found = 0;
            }
            if (!editionText_.empty()) {
                if (edition_.tested & bit) match = (edition_.found & bit) != 0;
                else {
                    match = FindNLSStringEx(LOCALE_NAME_USER_DEFAULT, FIND_FROMSTART | LINGUISTIC_IGNORECASE,
                        editionText_.data(), (int)editionText_.size(), word.data(), (int)word.size(),
                        nullptr, nullptr, nullptr, 0) >= 0;
                    edition_.tested |= bit;
                    if (match) edition_.found |= bit;
                }
            }
        }
        if (!match) return false;
    }
    return true;
}

// Trims persisted view state to what the playlist can still hold: editable lists validate item
// indices, views validate track ids, and everything else is clamped or dropped.
void SanitizePlaylistPosition(Playlist& pl) {
    auto& pos = pl.position;
    if (pos.search.size() > 4096) pos.search.resize(4096);
    if (!pos.search.empty() && pos.search.back() >= 0xD800 && pos.search.back() <= 0xDBFF) pos.search.pop_back();
    if (pl.sortColumn < -1 || pl.sortColumn > 24) pl.sortColumn = -1;
    auto valid = [&](TrackId value) {
        return pl.Editable() ? value < pl.items.size() : std::find(pl.items.begin(), pl.items.end(), value) != pl.items.end();
    };
    for (TrackId* value : {&pos.focus, &pos.top, &pos.expanded}) if (!valid(*value)) *value = UINT32_MAX;
    std::erase_if(pos.selected, [&](TrackId id) { return !valid(id); });
    std::sort(pos.selected.begin(), pos.selected.end());
    pos.selected.erase(std::unique(pos.selected.begin(), pos.selected.end()), pos.selected.end());
    pos.canvasScroll = std::clamp(pos.canvasScroll, 0, INT_MAX / 2);
}
