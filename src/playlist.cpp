// Playlist and search helpers with no Win32-control dependency: name sanitizing, item ordering,
// filter parsing and track matching. Shared by the UI and the persistence code.
#include "album_editions.h"
#include "library.h"
#include "playlog.h"
#include "playlist_shuffle.h"
#include "sort.h"

#include <algorithm>
#include <ctime>
#include <cwctype>

void ShuffleByWeights(std::vector<uint32_t>& order, const std::vector<double>& weights, std::mt19937& rng) {
    struct Pick { double key; uint32_t index; };
    std::vector<Pick> picks;
    picks.reserve(order.size());
    for (uint32_t index : order) {
        if (index >= weights.size()) return;
        double u = std::max(1e-12, std::generate_canonical<double, 53>(rng));
        double weight = weights[index] > 1e-12 ? weights[index] : 1e-12;
        picks.push_back({-std::log(u) / weight, index});
    }
    // Exponential keys give a weighted permutation without dropping any occurrence.
    SortRecords(picks.begin(), picks.end(), [](const Pick& a, const Pick& b) { return a.key > b.key; });
    for (size_t i = 0; i < picks.size(); ++i) order[i] = picks[i].index;
}

void ShuffleByListening(std::vector<uint32_t>& order, const std::vector<TrackId>& items, std::mt19937& rng) {
    ShuffleByWeights(order, ListeningWeights(items), rng);
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

std::vector<int> TabSlots(const std::vector<Playlist>& playlists, bool groupAuto, int shownAuto) {
    std::vector<int> slots;
    slots.reserve(playlists.size());
    int autos = 0, first = -1;
    for (int i = 0; i < (int)playlists.size(); ++i)
        if (playlists[i].kind == PlaylistKind::Auto && autos++ == 0) first = i;
    bool group = groupAuto && autos >= 2;
    if (group && (shownAuto < 0 || shownAuto >= (int)playlists.size() || playlists[shownAuto].kind != PlaylistKind::Auto))
        shownAuto = first;
    for (int i = 0; i < (int)playlists.size(); ++i) {
        if (!group || playlists[i].kind != PlaylistKind::Auto) slots.push_back(i);
        else if (i == first) slots.push_back(shownAuto);
    }
    return slots;
}

bool IsAutoGroupTab(const std::vector<Playlist>& playlists, const std::vector<int>& slots, int tab) {
    // Grouped exactly when some auto-playlist has no tab of its own.
    if (tab < 0 || tab >= (int)slots.size() || slots[tab] < 0 || slots[tab] >= (int)playlists.size() ||
        playlists[slots[tab]].kind != PlaylistKind::Auto)
        return false;
    int autos = 0;
    for (const auto& pl : playlists) autos += pl.kind == PlaylistKind::Auto;
    int tabbed = 0;
    for (int slot : slots) tabbed += slot >= 0 && slot < (int)playlists.size() && playlists[slot].kind == PlaylistKind::Auto;
    return tabbed < autos;
}

int TabOfPlaylist(const std::vector<Playlist>& playlists, const std::vector<int>& slots, int index) {
    if (index < 0 || index >= (int)playlists.size()) return -1;
    for (int tab = 0; tab < (int)slots.size(); ++tab)
        if (slots[tab] == index) return tab;
    if (playlists[index].kind == PlaylistKind::Auto)
        for (int tab = 0; tab < (int)slots.size(); ++tab)
            if (IsAutoGroupTab(playlists, slots, tab)) return tab;
    return -1;
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

int MissingTagTerm(const Library& library, TrackId id, std::wstring_view word) {
    constexpr std::wstring_view prefix = L"missing:";
    if (word.size() <= prefix.size() || (word[0] | 0x20) != L'm') return -1;
    auto same = [](std::wstring_view a, std::wstring_view b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (towlower(a[i]) != b[i]) return false;
        return true;
    };
    if (!same(word.substr(0, prefix.size()), prefix)) return -1;
    const std::wstring_view field = word.substr(prefix.size());
    if (!library.Contains(id)) return 0;
    const Track& t = library[id];
    if (same(field, L"title")) return !t.title;
    if (same(field, L"artist")) return !t.artist;
    if (same(field, L"album")) return !t.album;
    if (same(field, L"albumartist")) return !t.albumArtist;
    if (same(field, L"year") || same(field, L"date")) return !t.date;
    if (same(field, L"genre")) return !t.genre;
    if (same(field, L"track")) return !t.trackNo;
    if (same(field, L"composer")) return !t.composer;
    if (same(field, L"any")) return !t.title || !t.artist || !t.album || !t.date || !t.genre || !t.trackNo;
    return -1;  // an unknown field is searched for as ordinary text
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
        if (int missing = MissingTagTerm(library_, id, word); missing >= 0) {
            if (!missing) return false;
            continue;
        }
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

// The undo history keeps at most this many item ids across all of one playlist's states (about
// 16 MB), so editing a huge playlist many times can't grow memory without bound.
static constexpr size_t kPlaylistUndoItems = 4'000'000;

bool RecordPlaylistEdit(Playlist& pl, std::vector<TrackId> before, std::wstring what) {
    if (before == pl.items) return false;
    auto& h = pl.history;
    h.undo.push_back({std::move(before), std::move(what), (uint64_t)std::time(nullptr)});
    h.redo.clear();
    size_t stored = 0;
    for (const auto& e : h.undo) stored += e.items.size();
    size_t drop = 0;
    while (h.undo.size() - drop > 1 && (h.undo.size() - drop > kPlaylistUndoLevels || stored > kPlaylistUndoItems))
        stored -= h.undo[drop++].items.size();
    h.undo.erase(h.undo.begin(), h.undo.begin() + (ptrdiff_t)drop);
    return true;
}

std::vector<uint32_t> OccurrenceMapping(const std::vector<TrackId>& from, const std::vector<TrackId>& to) {
    // The n-th occurrence of a track in `from` becomes its n-th occurrence in `to`. Each list
    // becomes (track << 32 | position) keys, sorted, so a track's occurrences sit together in
    // position order and one merge pairs them: two flat arrays, where a map of position lists cost
    // a hash node and a vector for every distinct track of a playlist that can be the library.
    auto keys = [](const std::vector<TrackId>& items) {
        std::vector<uint64_t> out(items.size());
        for (size_t i = 0; i < items.size(); ++i) out[i] = (uint64_t)items[i] << 32 | (uint32_t)i;
        SortRecords(out.begin(), out.end());
        return out;
    };
    const std::vector<uint64_t> a = keys(from), b = keys(to);
    std::vector<uint32_t> mapping(from.size(), UINT32_MAX);
    for (size_t i = 0, j = 0; i < a.size(); ++i) {
        const uint64_t track = a[i] >> 32;
        while (j < b.size() && (b[j] >> 32) < track) ++j;
        if (j < b.size() && (b[j] >> 32) == track) mapping[(uint32_t)a[i]] = (uint32_t)b[j++];
    }
    return mapping;
}

// Undo and redo are the same move between the two stacks.
static bool StepPlaylistEdit(Playlist& pl, std::vector<PlaylistEdit>& from, std::vector<PlaylistEdit>& to,
                             std::vector<uint32_t>& mapping) {
    if (from.empty()) return false;
    PlaylistEdit state = std::move(from.back());
    from.pop_back();
    mapping = OccurrenceMapping(pl.items, state.items);
    std::swap(pl.items, state.items);
    to.push_back(std::move(state));
    return true;
}

bool UndoPlaylistEdit(Playlist& pl, std::vector<uint32_t>& mapping) {
    return StepPlaylistEdit(pl, pl.history.undo, pl.history.redo, mapping);
}

bool RedoPlaylistEdit(Playlist& pl, std::vector<uint32_t>& mapping) {
    return StepPlaylistEdit(pl, pl.history.redo, pl.history.undo, mapping);
}
