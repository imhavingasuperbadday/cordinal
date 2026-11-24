// Playlist and search helpers with no Win32-control dependency: name sanitizing, item ordering,
// filter parsing and track matching. Shared by the UI and the persistence code.
#include "album_editions.h"
#include "autoplaylist.h"
#include "library.h"
#include "playlog.h"
#include "playlist_shuffle.h"
#include "sort.h"
#include "tag_values.h"
#include "textcase.h"

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
        if (x.disc != y.disc) return x.disc < y.disc;
        const WorkInfo* wx = g_lib.Work(items[a]);
        const WorkInfo* wy = g_lib.Work(items[b]);
        if (wx && wy && wx->work && wx->work == wy->work && (wx->movementNo || wy->movementNo)) {
            if (wx->movementNo != wy->movementNo) return wx->movementNo < wy->movementNo;
        }
        return std::tie(x.trackNo, a) < std::tie(y.trackNo, b);
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

void ShuffleTracks(std::vector<uint32_t>& order, const std::vector<TrackId>& items, std::mt19937& rng, int current,
                   const std::vector<double>* weights) {
    order.clear();
    if (items.empty()) return;
    struct WorkKey {
        AlbumId album;
        StringPool::Handle work = 0;
        auto operator<=>(const WorkKey&) const = default;
    };
    struct Unit {
        std::vector<uint32_t> positions;
        double weight = 1.0;
    };
    std::vector<Unit> units;
    std::map<WorkKey, size_t> workUnits;
    for (uint32_t i = 0; i < items.size(); ++i) {
        const TrackId id = items[i];
        const WorkInfo* w = g_lib.Work(id);
        if (w && w->work != 0) {
            WorkKey key{AlbumKeyOf(g_lib[id]), w->work};
            auto [it, added] = workUnits.try_emplace(key, units.size());
            if (added) units.push_back({});
            units[it->second].positions.push_back(i);
        } else {
            units.push_back({{i}, 1.0});
        }
    }
    for (Unit& u : units) {
        if (u.positions.size() > 1) {
            SortRecords(u.positions.begin(), u.positions.end(), [&](uint32_t a, uint32_t b) {
                const Track& x = g_lib[items[a]], &y = g_lib[items[b]];
                const WorkInfo* wx = g_lib.Work(items[a]);
                const WorkInfo* wy = g_lib.Work(items[b]);
                uint16_t ma = wx ? wx->movementNo : 0, mb = wy ? wy->movementNo : 0;
                return std::tie(x.disc, ma, x.trackNo, a) < std::tie(y.disc, mb, y.trackNo, b);
            });
        }
        if (weights && weights->size() == items.size()) {
            double sum = 0;
            for (uint32_t pos : u.positions) sum += (*weights)[pos];
            u.weight = sum / (double)u.positions.size();
        }
    }
    std::vector<uint32_t> unitOrder(units.size());
    for (uint32_t i = 0; i < units.size(); ++i) unitOrder[i] = i;
    if (weights && weights->size() == items.size()) {
        std::vector<double> unitWeights(units.size());
        for (size_t i = 0; i < units.size(); ++i) unitWeights[i] = units[i].weight;
        ShuffleByWeights(unitOrder, unitWeights, rng);
        std::reverse(unitOrder.begin(), unitOrder.end());
    } else {
        std::shuffle(unitOrder.begin(), unitOrder.end(), rng);
    }
    order.reserve(items.size());
    if (current >= 0 && current < (int)items.size()) {
        for (size_t ui = 0; ui < unitOrder.size(); ++ui) {
            const auto& pos = units[unitOrder[ui]].positions;
            auto it = std::find(pos.begin(), pos.end(), (uint32_t)current);
            if (it != pos.end()) {
                order.insert(order.end(), it + 1, pos.end());
                std::swap(unitOrder[0], unitOrder[ui]);
                for (size_t k = 1; k < unitOrder.size(); ++k) {
                    const auto& p = units[unitOrder[k]].positions;
                    order.insert(order.end(), p.begin(), p.end());
                }
                return;
            }
        }
    }
    for (uint32_t u : unitOrder) {
        const auto& p = units[u].positions;
        order.insert(order.end(), p.begin(), p.end());
    }
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

static int PersonMatchTerm(const Library& library, TrackId id, std::wstring_view word) {
    constexpr std::wstring_view prefix = L"person:";
    if (word.size() <= prefix.size() || (word[0] | 0x20) != L'p') return -1;
    for (size_t i = 0; i < prefix.size(); ++i)
        if (towlower(word[i]) != prefix[i]) return -1;
    if (!library.Contains(id)) return 0;
    std::wstring target = textcase::SearchFold(word.substr(prefix.size()));
    if (target.empty()) return 0;
    auto eq = [&](std::string_view v) {
        if (v.empty()) return false;
        return textcase::SearchFold(FromUtf8(v)) == target;
    };
    const Track& t = library[id];
    for (StringPool::Handle h : {t.albumArtist, t.artist}) {
        if (!h) continue;
        for (const std::string& p : SplitTagValues(library.View(h), TagFieldKind::Artist))
            if (eq(p)) return 1;
    }
    if (t.composer) {
        for (const std::string& p : SplitTagValues(library.View(t.composer), TagFieldKind::Composer))
            if (eq(p)) return 1;
    }
    if (size_t n = library.CreditsCount(id)) {
        const CreditHandle* cs = library.CreditsBegin(id);
        for (size_t i = 0; i < n; ++i)
            if (eq(library.View(cs[i].name))) return 1;
    }
    return 0;
}

namespace {

bool ParseDouble(std::wstring_view s, double& out) {
    if (s.empty()) return false;
    std::string u = ToUtf8(s);
    char* end = nullptr;
    out = std::strtod(u.c_str(), &end);
    return end != u.c_str() && *end == '\0' && std::isfinite(out);
}

// Parses seconds from "m:ss", "h:mm:ss", "90s", "3m", or a plain number.
bool ParseDurationVal(std::wstring_view s, double& out) {
    if (s.empty()) return false;
    if (s.back() == L's' || s.back() == L'S') {
        s.remove_suffix(1);
        return ParseDouble(s, out);
    }
    if (s.back() == L'm' || s.back() == L'M') {
        s.remove_suffix(1);
        if (!ParseDouble(s, out)) return false;
        out *= 60.0;
        return true;
    }
    size_t c1 = s.find(L':');
    if (c1 == std::wstring_view::npos) return ParseDouble(s, out);
    double a = 0, b = 0, c = 0;
    size_t c2 = s.find(L':', c1 + 1);
    if (c2 == std::wstring_view::npos) {
        if (!ParseDouble(s.substr(0, c1), a) || !ParseDouble(s.substr(c1 + 1), b)) return false;
        out = a * 60.0 + b;
        return true;
    }
    if (!ParseDouble(s.substr(0, c1), a) || !ParseDouble(s.substr(c1 + 1, c2 - c1 - 1), b) ||
        !ParseDouble(s.substr(c2 + 1), c))
        return false;
    out = a * 3600.0 + b * 60.0 + c;
    return true;
}

template <class Parse>
bool MatchNumericExpr(std::wstring_view expr, double actual, bool known, Parse parseVal) {
    if (expr.empty()) return false;
    if (size_t dots = expr.find(L".."); dots != std::wstring_view::npos) {
        if (!known) return false;
        std::wstring_view loS = expr.substr(0, dots), hiS = expr.substr(dots + 2);
        double lo = -1e18, hi = 1e18;
        if (!loS.empty() && !parseVal(loS, lo)) return false;
        if (!hiS.empty() && !parseVal(hiS, hi)) return false;
        return actual + 1e-6 >= lo && actual - 1e-6 <= hi;
    }
    enum class Op { Eq, Ne, Ge, Le, Gt, Lt } op = Op::Eq;
    if (expr.starts_with(L">=")) { op = Op::Ge; expr.remove_prefix(2); }
    else if (expr.starts_with(L"<=")) { op = Op::Le; expr.remove_prefix(2); }
    else if (expr.starts_with(L"!=")) { op = Op::Ne; expr.remove_prefix(2); }
    else if (expr.starts_with(L">")) { op = Op::Gt; expr.remove_prefix(1); }
    else if (expr.starts_with(L"<")) { op = Op::Lt; expr.remove_prefix(1); }
    else if (expr.starts_with(L"=")) { op = Op::Eq; expr.remove_prefix(1); }
    double target = 0;
    if (!parseVal(expr, target)) return false;
    if (!known) return op == Op::Ne;
    switch (op) {
        case Op::Eq: return std::abs(actual - target) <= 1e-4;
        case Op::Ne: return std::abs(actual - target) > 1e-4;
        case Op::Ge: return actual + 1e-6 >= target;
        case Op::Le: return actual - 1e-6 <= target;
        case Op::Gt: return actual > target + 1e-6;
        case Op::Lt: return actual + 1e-6 < target;
    }
    return false;
}

bool ParseRelativeDays(std::wstring_view s, double& days) {
    if (s.empty()) return false;
    double mult = 1.0;
    wchar_t unit = s.back();
    if (unit == L'd' || unit == L'D') s.remove_suffix(1);
    else if (unit == L'w' || unit == L'W') { mult = 7.0; s.remove_suffix(1); }
    else if (unit == L'm' || unit == L'M') { mult = 30.0; s.remove_suffix(1); }
    else if (unit == L'y' || unit == L'Y') { mult = 365.0; s.remove_suffix(1); }
    else return false;
    double n = 0;
    if (!ParseDouble(s, n) || n < 0) return false;
    days = n * mult;
    return true;
}

bool MatchDateFilter(std::wstring_view expr, uint32_t stamp, uint32_t now, bool allowNever) {
    std::wstring folded = textcase::SearchFold(expr);
    if (allowNever && (folded == L"never" || folded == L"none" || folded == L"0")) return stamp == 0;
    // Relative duration: 30d, <30d, <=7d, >90d, >=1y
    std::wstring_view sv = folded;
    enum class Op { Bare, Ge, Le, Gt, Lt } op = Op::Bare;
    if (sv.starts_with(L">=")) { op = Op::Ge; sv.remove_prefix(2); }
    else if (sv.starts_with(L"<=")) { op = Op::Le; sv.remove_prefix(2); }
    else if (sv.starts_with(L">")) { op = Op::Gt; sv.remove_prefix(1); }
    else if (sv.starts_with(L"<")) { op = Op::Lt; sv.remove_prefix(1); }
    double days = 0;
    if (ParseRelativeDays(sv, days)) {
        if (!stamp) return false;
        double ageDays = now >= stamp ? (now - stamp) / 86400.0 : 0.0;
        switch (op) {
            case Op::Bare:
            case Op::Le: return ageDays <= days;
            case Op::Lt: return ageDays < days;
            case Op::Ge: return ageDays >= days;
            case Op::Gt: return ageDays > days;
        }
    }
    // Range or absolute date: 2024..2025, >=2025-01-01, 2025
    if (size_t dots = expr.find(L".."); dots != std::wstring_view::npos) {
        if (!stamp) return false;
        uint32_t lo = 0, hi = UINT32_MAX;
        if (dots > 0 && !ParseRuleDate(ToUtf8(expr.substr(0, dots)), lo)) return false;
        if (dots + 2 < expr.size()) {
            std::string hiStr = ToUtf8(expr.substr(dots + 2));
            if (!ParseRuleDate(hiStr, hi)) return false;
            // End of year/month/day: bump to end of that day/period (~1 day for YYYY-MM-DD, or next year if 4 digits).
            if (hiStr.size() == 4) hi += 366u * 86400u;
            else hi += 86400u;
        }
        return stamp >= lo && stamp < hi;
    }
    if (op != Op::Bare) {
        uint32_t when = 0;
        if (!ParseRuleDate(ToUtf8(expr.substr(op == Op::Ge || op == Op::Le ? 2 : 1)), when)) return false;
        if (!stamp) return false;
        switch (op) {
            case Op::Ge: return stamp >= when;
            case Op::Gt: return stamp >= when + 86400u;
            case Op::Le: return stamp < when + 86400u;
            case Op::Lt: return stamp < when;
            default: return false;
        }
    }
    uint32_t when = 0;
    std::string raw = ToUtf8(expr);
    if (ParseRuleDate(raw, when)) {
        if (!stamp) return false;
        uint32_t span = raw.size() == 4 ? 366u * 86400u : 86400u;
        return stamp >= when && stamp < when + span;
    }
    return false;
}

bool ParseTrackYear(const Library& lib, const Track& t, double& out) {
    std::string_view d = lib.View(t.date);
    while (!d.empty() && (unsigned char)d.front() <= ' ') d.remove_prefix(1);
    if (d.empty() || d.front() < '0' || d.front() > '9') return false;
    long long v = 0;
    for (char c : d) {
        if (c < '0' || c > '9') break;
        v = v * 10 + (c - '0');
        if (v > 9999) break;
    }
    out = (double)v;
    return v > 0;
}

bool IsLosslessCodec(Codec c) {
    return c == Codec::FLAC || c == Codec::ALAC || c == Codec::PCM || c == Codec::DSD;
}

// Evaluates structured inline filters (year:1990..1999, format:flac, rating:>=4, played:never,
// added:30d, bpm:115..125, edition:remaster, dr:<7, lyrics:..., etc.). Returns -1 when the token
// is not a recognized filter so it falls back to ordinary text search.
int InlineFilterTerm(const Library& library, TrackId id, std::wstring_view word, uint32_t now) {
    size_t colon = word.find(L':');
    if (colon == 0 || colon == std::wstring_view::npos || colon + 1 >= word.size()) return -1;
    std::wstring key = textcase::SearchFold(word.substr(0, colon));
    std::wstring_view val = word.substr(colon + 1);
    std::wstring foldedVal = textcase::SearchFold(val);
    if (!library.Contains(id)) return 0;
    const Track& t = library[id];
    auto hasText = [&](std::wstring_view hay) {
        return !hay.empty() && textcase::SearchFold(hay).find(foldedVal) != std::wstring::npos;
    };
    auto hasHandle = [&](StringPool::Handle h) { return h && hasText(library.Str(h)); };

    if (key == L"artist") return hasHandle(t.artist ? t.artist : t.albumArtist) ? 1 : 0;
    if (key == L"albumartist") return hasHandle(t.albumArtist ? t.albumArtist : t.artist) ? 1 : 0;
    if (key == L"title") return hasText(t.title ? library.Str(t.title) : StemOf(library.Path(id))) ? 1 : 0;
    if (key == L"album") return hasHandle(t.album) ? 1 : 0;
    if (key == L"genre") return hasHandle(t.genre) ? 1 : 0;
    if (key == L"composer") return hasHandle(t.composer) ? 1 : 0;
    if (key == L"comment") return hasHandle(t.comment) ? 1 : 0;
    if (key == L"grouping") return hasHandle(t.grouping) ? 1 : 0;
    if (key == L"label") return hasHandle(t.label) ? 1 : 0;
    if (key == L"path") return hasText(library.Path(id)) ? 1 : 0;
    if (key == L"edition") return hasText(AlbumEditionSearchText(id)) ? 1 : 0;
    if (key == L"lyrics") return hasHandle(library.Lyrics(id)) ? 1 : 0;
    if (key == L"format" || key == L"codec") {
        if (foldedVal == L"lossless") return IsLosslessCodec(t.codec) ? 1 : 0;
        if (foldedVal == L"lossy") return (t.codec != Codec::Unknown && !IsLosslessCodec(t.codec)) ? 1 : 0;
        return hasText(CodecName(t.codec)) ? 1 : 0;
    }
    if (key == L"year" || key == L"date") {
        double y = 0;
        bool known = ParseTrackYear(library, t, y);
        return MatchNumericExpr(val, y, known, ParseDouble) ? 1 : 0;
    }
    if (key == L"rating") {
        if (foldedVal == L"unrated" || foldedVal == L"none" || foldedVal == L"0") return t.rating == 0 ? 1 : 0;
        return MatchNumericExpr(val, t.rating / 20.0, t.rating > 0, ParseDouble) ? 1 : 0;
    }
    if (key == L"bpm") {
        if (foldedVal == L"none" || foldedVal == L"0") return t.bpm == 0 ? 1 : 0;
        return MatchNumericExpr(val, t.bpm, t.bpm > 0, ParseDouble) ? 1 : 0;
    }
    if (key == L"dr") {
        double dr = t.rgHigh ? (t.rgHigh - 1) / 4.0 : 0.0;
        return MatchNumericExpr(val, dr, t.rgHigh > 0, ParseDouble) ? 1 : 0;
    }
    if (key == L"plays" || key == L"playcount") {
        return MatchNumericExpr(val, t.playCount, true, ParseDouble) ? 1 : 0;
    }
    if (key == L"bitrate") {
        return MatchNumericExpr(val, t.bitrate, t.bitrate > 0, ParseDouble) ? 1 : 0;
    }
    if (key == L"length" || key == L"duration") {
        return MatchNumericExpr(val, t.durationMs / 1000.0, t.durationMs > 0, ParseDurationVal) ? 1 : 0;
    }
    if (key == L"favorite" || key == L"fav") {
        if (foldedVal == L"yes" || foldedVal == L"true" || foldedVal == L"1" || foldedVal == L"on")
            return library.Favorite(id) ? 1 : 0;
        if (foldedVal == L"no" || foldedVal == L"false" || foldedVal == L"0" || foldedVal == L"off")
            return !library.Favorite(id) ? 1 : 0;
        return 0;
    }
    if (key == L"played") return MatchDateFilter(val, t.lastPlayed, now, true) ? 1 : 0;
    if (key == L"added") return MatchDateFilter(val, t.added, now, false) ? 1 : 0;
    return -1;
}

}  // namespace

TrackMatcher::TrackMatcher(const Library& library, const std::vector<std::wstring>& words)
    : library_(library), words_(words), now_(UnixNow()) {
    foldedWords_.reserve(words.size());
    for (const auto& w : words) foldedWords_.push_back(textcase::SearchFold(w));
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
        const auto& folded = foldedWords_[wordIndex];
        if (int missing = MissingTagTerm(library_, id, word); missing >= 0) {
            if (!missing) return false;
            continue;
        }
        if (int person = PersonMatchTerm(library_, id, word); person >= 0) {
            if (!person) return false;
            continue;
        }
        if (int filt = InlineFilterTerm(library_, id, word, now_); filt >= 0) {
            if (!filt) return false;
            continue;
        }
        if (folded.empty()) continue;
        // Long queries retain their full meaning; words beyond the mask are searched normally.
        const uint64_t bit = wordIndex < 64 ? uint64_t(1) << wordIndex : 0;
        bool match = false;
        for (size_t i = 0; i < std::size(fields_); ++i) {
            if (!handles[i]) continue;
            auto& field = fields_[i];
            if (field.handle != handles[i]) {
                field.text = textcase::SearchFold(i < 12 ? library_.Str(handles[i]) : CodecName(track.codec));
                field.handle = handles[i];
                field.tested = field.found = 0;
            }
            if (field.tested & bit) match = (field.found & bit) != 0;
            else {
                match = !field.text.empty() && field.text.find(folded) != std::wstring::npos;
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
                editionText_ = textcase::SearchFold(AlbumEditionSearchText(id));
                edition_.tested = edition_.found = 0;
            }
            if (!editionText_.empty()) {
                if (edition_.tested & bit) match = (edition_.found & bit) != 0;
                else {
                    match = editionText_.find(folded) != std::wstring::npos;
                    edition_.tested |= bit;
                    if (match) edition_.found |= bit;
                }
            }
        }
        if (!match) {
            if (extraTrack_ != id) {
                extraTrack_ = id;
                std::wstring rawExtra;
                if (const WorkInfo* w = library_.Work(id)) {
                    if (w->work) rawExtra += library_.Str(w->work);
                    if (w->movement) {
                        if (!rawExtra.empty()) rawExtra += L' ';
                        rawExtra += library_.Str(w->movement);
                    }
                }
                if (size_t n = library_.CreditsCount(id)) {
                    const CreditHandle* cs = library_.CreditsBegin(id);
                    for (size_t i = 0; i < n; ++i) {
                        if (!rawExtra.empty()) rawExtra += L' ';
                        if (cs[i].role) {
                            rawExtra += library_.Str(cs[i].role);
                            rawExtra += L' ';
                        }
                        rawExtra += library_.Str(cs[i].name);
                    }
                }
                extraText_ = textcase::SearchFold(rawExtra);
                extra_.tested = extra_.found = 0;
            }
            if (!extraText_.empty()) {
                if (extra_.tested & bit) match = (extra_.found & bit) != 0;
                else {
                    match = extraText_.find(folded) != std::wstring::npos;
                    extra_.tested |= bit;
                    if (match) extra_.found |= bit;
                }
            }
        }
        if (!match) {
            StringPool::Handle lh = library_.Lyrics(id);
            if (lh) {
                if (lyricsTrack_ != id || lyrics_.handle != lh) {
                    lyricsTrack_ = id;
                    lyrics_.handle = lh;
                    lyricsText_ = textcase::SearchFold(library_.Str(lh));
                    lyrics_.tested = lyrics_.found = 0;
                }
                if (!lyricsText_.empty()) {
                    if (lyrics_.tested & bit) match = (lyrics_.found & bit) != 0;
                    else {
                        match = lyricsText_.find(folded) != std::wstring::npos;
                        lyrics_.tested |= bit;
                        if (match) lyrics_.found |= bit;
                    }
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
