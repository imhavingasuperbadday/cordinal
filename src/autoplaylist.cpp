// Auto-playlist engine: reading a playlist's serialized rules, matching them against the library,
// and writing the rules back. See autoplaylist.h for the type and rule description.
//
// The saved format ("cordinal-auto 1") is a "limit" line followed by a nested "group"/"rule"/"end"
// tree mirroring RuleGroup. The k*Keys tables are the stable on-disk identifiers; the k*Names
// tables are UI-only and may change freely. Parsing is lenient: an unknown rule is skipped so a
// file from a newer version still loads.
#include "sort.h"
#include "autoplaylist.h"
#include "playlog.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <random>
#include <unordered_map>

namespace {

// Stable identifiers used in the saved text (never change them; append new ones).
const char* const kFieldKeys[] = {"any", "title", "artist", "albumartist", "album", "genre", "composer", "comment", "grouping",
                                  "label", "path", "codec", "year", "rating", "plays", "skips", "length", "bitrate", "track",
                                  "disc", "bpm", "lastplayed", "added", "plays30", "plays365"};
const wchar_t* const kFieldNames[] = {L"Any text", L"Title", L"Artist", L"Album artist", L"Album", L"Genre", L"Composer",
                                      L"Comment", L"Grouping", L"Label", L"Path", L"Codec", L"Year", L"Rating", L"Play count",
                                      L"Skip count", L"Length (seconds)", L"Bitrate (kbps)", L"Track number", L"Disc number",
                                      L"BPM", L"Last played", L"Date added", L"Plays in the last 30 days",
                                      L"Plays in the last 365 days"};
const char* const kOpKeys[] = {"contains", "notcontains", "is", "isnot", "starts", "ends", "empty", "notempty",
                               "eq", "ne", "atleast", "atmost", "between", "inlast", "notinlast", "never"};
const wchar_t* const kOpNames[] = {L"contains", L"doesn't contain", L"is", L"is not", L"starts with", L"ends with",
                                   L"is empty", L"isn't empty", L"is", L"is not", L"is at least", L"is at most", L"is between",
                                   L"in the last (days)", L"not in the last (days)", L"never"};
const char* const kPickKeys[] = {"random", "mostplayed", "leastplayed", "highestrated", "lowestrated", "recentlyadded",
                                 "recentlyplayed", "leastrecentlyplayed"};
const wchar_t* const kPickNames[] = {L"random", L"most played", L"least played", L"highest rated", L"lowest rated",
                                     L"most recently added", L"most recently played", L"least recently played"};
static_assert(std::size(kFieldKeys) == (size_t)RuleField::Count && std::size(kFieldNames) == (size_t)RuleField::Count);
static_assert(std::size(kOpKeys) == (size_t)RuleOp::Count && std::size(kOpNames) == (size_t)RuleOp::Count);
static_assert(std::size(kPickKeys) == (size_t)PickBy::Count && std::size(kPickNames) == (size_t)PickBy::Count);
const char* const kUnitKeys[] = {"tracks", "minutes", "mb", "gb"};
const wchar_t* const kUnitNames[] = {L"tracks", L"minutes", L"MB", L"GB"};
static_assert(std::size(kUnitKeys) == (size_t)LimitUnit::Count && std::size(kUnitNames) == (size_t)LimitUnit::Count);

template <size_t N>
int IndexOf(const char* const (&keys)[N], const std::string& k) {
    for (size_t i = 0; i < N; ++i)
        if (k == keys[i]) return (int)i;
    return -1;
}

std::wstring Lower(std::wstring s) {
    if (!s.empty()) LCMapStringEx(LOCALE_NAME_USER_DEFAULT, LCMAP_LOWERCASE | LCMAP_LINGUISTIC_CASING, s.c_str(), (int)s.size(),
                                  s.data(), (int)s.size(), nullptr, nullptr, 0);
    return s;
}

double ParseNumber(const std::string& s) {
    // "3:30" (minutes:seconds) or a plain number.
    size_t colon = s.find(':');
    if (colon != std::string::npos) return atof(s.c_str()) * 60 + atof(s.c_str() + colon + 1);
    return atof(s.c_str());
}

// Matches a track against rules, caching lowercased strings per handle.
class Matcher {
public:
    Matcher(const Library& library = g_lib, const std::vector<PlayEvent>& log = PlayLog())
        : library_(library), log_(log), now_(UnixNow()) {}

    bool Match(const RuleGroup& g, TrackId id) {
        if (g.rules.empty() && g.groups.empty()) return true;
        for (const Rule& r : g.rules) {
            bool m = Match(r, id);
            if (g.all && !m) return false;
            if (!g.all && m) return true;
        }
        for (const RuleGroup& sub : g.groups) {
            bool m = Match(sub, id);
            if (g.all && !m) return false;
            if (!g.all && m) return true;
        }
        return g.all;
    }

    bool Match(const Rule& r, TrackId id) {
        const Track& t = library_[id];
        if (IsDateField(r.field)) {
            uint32_t when = r.field == RuleField::LastPlayed ? t.lastPlayed : t.added;
            double days = ParseNumber(r.value);
            if ((r.op == RuleOp::InLastDays || r.op == RuleOp::NotInLastDays) &&
                (!std::isfinite(days) || days < 0)) return false;
            uint32_t cutoff = now_ - (uint32_t)std::clamp(std::isfinite(days) ? days * 86400.0 : 0.0, 0.0, (double)now_);
            switch (r.op) {
                case RuleOp::InLastDays: return when && when >= cutoff;
                case RuleOp::NotInLastDays: return !when || when < cutoff;
                case RuleOp::Never: return !when;
                case RuleOp::IsEmpty: return !when;
                case RuleOp::NotEmpty: return when != 0;
                default: return false;
            }
        }
        if (!IsTextField(r.field)) {
            double v = r.field == RuleField::Plays30 || r.field == RuleField::Plays365 ? RecentPlays(r.field, id)
                                                                                    : Number(r.field, t);
            double a = ParseNumber(r.value), b = ParseNumber(r.value2);
            // A small tolerance: the values are typed in as text and compared as doubles.
            switch (r.op) {
                case RuleOp::Equals: return std::fabs(v - a) < 1e-6;
                case RuleOp::NotEquals: return std::fabs(v - a) >= 1e-6;
                case RuleOp::AtLeast: return v >= a - 1e-6;
                case RuleOp::AtMost: return v <= a + 1e-6;
                case RuleOp::Between: return v >= std::min(a, b) - 1e-6 && v <= std::max(a, b) + 1e-6;
                case RuleOp::IsEmpty: return v == 0;
                case RuleOp::NotEmpty: return v != 0;
                default: return false;
            }
        }
        if (r.op == RuleOp::IsEmpty || r.op == RuleOp::NotEmpty) {
            bool empty = TextEmpty(r.field, id, t);
            return r.op == RuleOp::IsEmpty ? empty : !empty;
        }
        std::wstring hay = Text(r.field, id, t);
        const std::wstring& needle = Needle(r.value);
        switch (r.op) {
            case RuleOp::Contains: return hay.find(needle) != std::wstring::npos;
            case RuleOp::NotContains: return hay.find(needle) == std::wstring::npos;
            case RuleOp::Is: return hay == needle;
            case RuleOp::IsNot: return hay != needle;
            case RuleOp::StartsWith: return hay.compare(0, needle.size(), needle) == 0;
            case RuleOp::EndsWith: return hay.size() >= needle.size() && hay.compare(hay.size() - needle.size(), needle.size(), needle) == 0;
            default: return false;
        }
    }

private:
    const Library& library_;
    const std::vector<PlayEvent>& log_;
    std::unordered_map<StringPool::Handle, std::wstring> lower_;
    std::unordered_map<std::string, std::wstring> needles_;
    uint32_t now_;
    std::vector<uint32_t> plays30_, plays365_;  // counted plays per track, built on first use
    bool built30_ = false, built365_ = false;

    // Counted plays of a track in the last 30 or 365 days. The counts for every track are built
    // in one pass over the log, once per matcher (so once per evaluation).
    uint32_t RecentPlays(RuleField f, TrackId id) {
        bool thirty = f == RuleField::Plays30;
        std::vector<uint32_t>& counts = thirty ? plays30_ : plays365_;
        bool& built = thirty ? built30_ : built365_;
        if (!built) {
            uint32_t days = thirty ? 30 : 365;
            uint32_t since = now_ - (uint32_t)std::min<uint64_t>((uint64_t)days * 86400, now_);
            counts.assign(library_.Count(), 0);
            for (const PlayEvent& e : log_)
                if ((e.flags & PlayEvent::kCounted) && e.time >= since && e.id < counts.size()) ++counts[e.id];
            built = true;
        }
        return id < counts.size() ? counts[id] : 0;
    }

    const std::wstring& Needle(const std::string& s) {
        auto [it, added] = needles_.emplace(s, std::wstring());
        if (added) it->second = Lower(FromUtf8(s));
        return it->second;
    }

    const std::wstring& Str(StringPool::Handle h) {
        auto [it, added] = lower_.emplace(h, std::wstring());
        if (added) it->second = Lower(library_.Str(h));
        return it->second;
    }

    bool TextEmpty(RuleField f, TrackId id, const Track& t) {
        switch (f) {
            case RuleField::Title: return !t.title && !t.file;
            case RuleField::Artist: return !t.artist;
            case RuleField::AlbumArtist: return !t.albumArtist && !t.artist;
            case RuleField::Album: return !t.album;
            case RuleField::Genre: return !t.genre;
            case RuleField::Composer: return !t.composer;
            case RuleField::Comment: return !t.comment;
            case RuleField::Grouping: return !t.grouping;
            case RuleField::Label: return !t.label;
            case RuleField::Path: return !t.dir && !t.file;
            case RuleField::Codec: return *CodecName(t.codec) == 0;
            case RuleField::Any:
                return TextEmpty(RuleField::Title, id, t) && !t.artist && !t.albumArtist && !t.album && !t.genre &&
                       !t.composer && !t.comment && !t.grouping && !t.label;
            default: return true;
        }
    }

    std::wstring Text(RuleField f, TrackId id, const Track& t) {
        switch (f) {
            case RuleField::Title: {
                std::wstring title = t.title ? library_.Str(t.title) : StemOf(library_.Path(id));
                return Lower(title);
            }
            case RuleField::Artist: return Str(t.artist);
            case RuleField::AlbumArtist: return Str(t.albumArtist ? t.albumArtist : t.artist);
            case RuleField::Album: return Str(t.album);
            case RuleField::Genre: return Str(t.genre);
            case RuleField::Composer: return Str(t.composer);
            case RuleField::Comment: return Str(t.comment);
            case RuleField::Grouping: return Str(t.grouping);
            case RuleField::Label: return Str(t.label);
            case RuleField::Path: return Lower(library_.Path(id));
            case RuleField::Codec: return Lower(CodecName(t.codec));
            case RuleField::Any: {
                std::wstring all = Text(RuleField::Title, id, t);
                for (auto h : {t.artist, t.albumArtist, t.album, t.genre, t.composer, t.comment, t.grouping, t.label}) all += L"\n" + Str(h);
                return all;
            }
            default: return {};
        }
    }

    double Number(RuleField f, const Track& t) {
        switch (f) {
            case RuleField::Year: {
                std::string_view d = library_.View(t.date);
                while (!d.empty() && (unsigned char)d.front() <= ' ') d.remove_prefix(1);
                bool neg = !d.empty() && d.front() == '-';
                if (neg || (!d.empty() && d.front() == '+')) d.remove_prefix(1);
                long long v = 0;
                for (char c : d) {
                    if (c < '0' || c > '9') break;
                    constexpr auto max = std::numeric_limits<long long>::max();
                    if (v > (max - (c - '0')) / 10) {
                        v = max;
                        break;
                    }
                    v = v * 10 + (c - '0');
                }
                return (double)(neg ? -v : v);
            }
            case RuleField::Rating: return t.rating / 20.0;
            case RuleField::PlayCount: return t.playCount;
            case RuleField::SkipCount: return t.skipCount;
            case RuleField::Length: return t.durationMs / 1000.0;
            case RuleField::Bitrate: return t.bitrate;
            case RuleField::TrackNo: return t.trackNo;
            case RuleField::Disc: return t.disc;
            case RuleField::Bpm: return t.bpm;
            default: return 0;
        }
    }
};

// Newlines and tabs would break the line-based format.
std::string Clean(const std::string& s) {
    std::string r = s;
    for (auto& c : r)
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return r;
}

// The saved format uses C-locale whitespace and decimal integers. Keep stream extraction's
// partial-number, overflow and failure behavior without linking the iostream/locale machinery.
class Fields {
    const char* p_;
    const char* end_;
    bool failed_ = false;
    static bool Space(char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
    bool Skip() {
        while (p_ != end_ && Space(*p_)) ++p_;
        return !failed_ && p_ != end_;
    }
public:
    explicit Fields(const std::string& line) : p_(line.c_str()), end_(p_ + line.size()) {}
    std::string Word() {
        if (!Skip()) return {};
        const char* first = p_;
        while (p_ != end_ && !Space(*p_)) ++p_;
        return {first, p_};
    }
    template <class T, class Parse>
    void Number(T& value, Parse parse) {
        if (!Skip()) return;
        char* end = nullptr;
        errno = 0;
        value = (T)parse(p_, &end, 10);
        failed_ = end == p_ || errno == ERANGE;
        p_ = end;
    }
    std::string Rest() const { return failed_ ? std::string() : std::string(p_, end_); }
};

bool NextLine(std::string_view& input, std::string& line) {
    if (input.empty()) return false;
    size_t end = input.find('\n');
    line = input.substr(0, end);
    input.remove_prefix(end == std::string_view::npos ? input.size() : end + 1);
    return true;
}

// One group and its nested groups, terminated by "end". A rule's value and value2 are tab-separated.
void WriteGroup(std::string& o, const RuleGroup& g) {
    o += g.all ? "group all\n" : "group any\n";
    for (const Rule& r : g.rules)
        o += std::string("rule ") + kFieldKeys[(int)r.field] + " " + kOpKeys[(int)r.op] + " " + Clean(r.value) + "\t" + Clean(r.value2) + "\n";
    for (const RuleGroup& sub : g.groups) WriteGroup(o, sub);
    o += "end\n";
}

}  // namespace

const wchar_t* RuleFieldName(RuleField f) { return kFieldNames[(int)f]; }
const wchar_t* RuleOpName(RuleOp op) { return kOpNames[(int)op]; }
const wchar_t* PickByName(PickBy p) { return kPickNames[(int)p]; }
const wchar_t* LimitUnitName(LimitUnit u) { return kUnitNames[(int)u]; }
bool IsTextField(RuleField f) { return f <= RuleField::Codec; }
bool IsDateField(RuleField f) { return f == RuleField::LastPlayed || f == RuleField::DateAdded; }

std::vector<RuleOp> OpsFor(RuleField f) {
    if (IsDateField(f)) return {RuleOp::InLastDays, RuleOp::NotInLastDays, RuleOp::Never};
    if (IsTextField(f))
        return {RuleOp::Contains, RuleOp::NotContains, RuleOp::Is, RuleOp::IsNot, RuleOp::StartsWith, RuleOp::EndsWith,
                RuleOp::IsEmpty, RuleOp::NotEmpty};
    return {RuleOp::Equals, RuleOp::NotEquals, RuleOp::AtLeast, RuleOp::AtMost, RuleOp::Between, RuleOp::IsEmpty, RuleOp::NotEmpty};
}

std::string SerializeAutoPlaylist(const AutoPlaylist& a) {
    std::string o = "cordinal-auto 1\nlimit " + std::to_string(a.limit) + " " +
                    kUnitKeys[(int)a.unit] + " " + kPickKeys[(int)a.pick] + (a.ordered ? " 1\n" : " 0\n");
    WriteGroup(o, a.root);
    return o;
}

bool ParseAutoPlaylist(const std::string& text, AutoPlaylist& out) {
    out = AutoPlaylist{};
    std::string_view in(text);
    std::string line;
    if (!NextLine(in, line) || line.rfind("cordinal-auto ", 0) != 0) return false;
    std::vector<RuleGroup*> stack;
    bool sawRoot = false;
    while (NextLine(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        Fields ls(line);
        std::string word = ls.Word();
        if (word == "limit") {
            std::string unit, pick;
            int ordered = 0;
            ls.Number(out.limit, strtoul);
            unit = ls.Word();
            pick = ls.Word();
            ls.Number(ordered, strtol);
            int u = IndexOf(kUnitKeys, unit);
            out.unit = u < 0 ? LimitUnit::Tracks : (LimitUnit)u;
            int p = IndexOf(kPickKeys, pick);
            out.pick = p < 0 ? PickBy::Random : (PickBy)p;
            out.ordered = ordered != 0;
        } else if (word == "group") {
            std::string mode = ls.Word();
            RuleGroup* g;
            if (stack.empty()) {
                if (sawRoot) return false;
                sawRoot = true;
                g = &out.root;
            } else {
                stack.back()->groups.emplace_back();
                g = &stack.back()->groups.back();
            }
            g->all = mode != "any";
            stack.push_back(g);
        } else if (word == "rule") {
            if (stack.empty()) return false;
            std::string field = ls.Word(), op = ls.Word();
            int f = IndexOf(kFieldKeys, field), o = IndexOf(kOpKeys, op);
            if (f < 0 || o < 0) continue;  // a newer version's rule: skip rather than fail
            std::string rest = ls.Rest();
            if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
            size_t tab = rest.find('\t');
            Rule r{(RuleField)f, (RuleOp)o, rest.substr(0, tab), tab == std::string::npos ? "" : rest.substr(tab + 1)};
            stack.back()->rules.push_back(std::move(r));
        } else if (word == "end") {
            if (stack.empty()) return false;
            stack.pop_back();
        }
    }
    return sawRoot && stack.empty();
}

bool RuleMatches(const Rule& r, TrackId id) { return Matcher().Match(r, id); }

// Collects the library tracks matching the rules, then applies the pick order and the limit. A
// limit measured in minutes or bytes is filled in pick order until the budget runs out; a track
// that would pass it is left out and the next ones still get their chance.
void EvaluateAutoPlaylist(const AutoPlaylist& a, std::vector<TrackId>& out, uint32_t seed) {
    EvaluateAutoPlaylist(a, g_lib, PlayLog(), out, seed);
}

void EvaluateAutoPlaylist(const AutoPlaylist& a, const Library& library, const std::vector<PlayEvent>& log,
                          std::vector<TrackId>& out, uint32_t seed) {
    out.clear();
    Matcher m(library, log);
    for (TrackId id = 0; id < (TrackId)library.Count(); ++id)
        if (library[id].InLibrary() && m.Match(a.root, id)) out.push_back(id);
    if (!a.limit && !a.ordered) {
        out.shrink_to_fit();
        return;
    }
    // Pick order: the best candidates first (ties broken by id for stable results).
    struct PickOrder {
        uint32_t (*key)(const Track&);
        bool descending;
    };
    static const PickOrder orders[] = {
        {nullptr, false},
        {[](const Track& t) -> uint32_t { return t.playCount; }, true},
        {[](const Track& t) -> uint32_t { return t.playCount; }, false},
        {[](const Track& t) -> uint32_t { return t.rating; }, true},
        {[](const Track& t) -> uint32_t { return t.rating; }, false},
        {[](const Track& t) -> uint32_t { return t.added; }, true},
        {[](const Track& t) -> uint32_t { return t.lastPlayed; }, true},
        {[](const Track& t) -> uint32_t { return t.lastPlayed; }, false},
    };
    static_assert(std::size(orders) == (size_t)PickBy::Count);
    if (a.pick == PickBy::Random) {
        std::mt19937 rng(seed);
        std::shuffle(out.begin(), out.end(), rng);
    } else if ((size_t)a.pick < std::size(orders)) {
        const PickOrder& order = orders[(size_t)a.pick];
        auto cmp = [&](TrackId x, TrackId y) {
            auto kx = order.key(library[x]), ky = order.key(library[y]);
            if (kx != ky) return order.descending ? kx > ky : kx < ky;
            return x < y;
        };
        if (a.limit && a.unit == LimitUnit::Tracks && out.size() > a.limit)
            std::partial_sort(out.begin(), out.begin() + (ptrdiff_t)a.limit, out.end(), cmp);
        else
            SortRecords(out.begin(), out.end(), cmp);
    }
    if (a.limit) {
        if (a.unit == LimitUnit::Minutes) {
            double budget = a.limit * 60.0, used = 0;
            size_t n = 0;
            while (n < out.size() && used + library[out[n]].Duration() <= budget) used += library[out[n++]].Duration();
            out.resize(n);
        } else if (a.unit != LimitUnit::Tracks) {
            // Filling a card: a big file that doesn't fit is skipped so smaller ones fill the rest.
            const uint64_t budget = (uint64_t)a.limit * (a.unit == LimitUnit::Gigabytes ? 1000000000ull : 1000000ull);
            uint64_t used = 0;
            size_t n = 0;
            for (TrackId id : out)
                if (library[id].fileSize <= budget - used) {
                    used += library[id].fileSize;
                    out[n++] = id;
                }
            out.resize(n);
        } else if (out.size() > a.limit) {
            out.resize(a.limit);
        }
    }
    out.shrink_to_fit();
}

std::vector<std::pair<std::wstring, AutoPlaylist>> DefaultAutoPlaylists() {
    std::vector<std::pair<std::wstring, AutoPlaylist>> r;
    AutoPlaylist added;
    added.root.rules.push_back({RuleField::DateAdded, RuleOp::InLastDays, "30", ""});
    added.pick = PickBy::RecentlyAdded;
    added.ordered = true;
    r.emplace_back(L"Recently added", added);
    AutoPlaylist played;
    played.root.rules.push_back({RuleField::LastPlayed, RuleOp::InLastDays, "14", ""});
    played.pick = PickBy::RecentlyPlayed;
    played.ordered = true;
    r.emplace_back(L"Recently played", played);
    AutoPlaylist top;
    top.root.rules.push_back({RuleField::PlayCount, RuleOp::AtLeast, "1", ""});
    top.limit = 25;
    top.pick = PickBy::MostPlayed;
    top.ordered = true;
    r.emplace_back(L"Top 25 most played", top);
    AutoPlaylist rated;
    rated.root.rules.push_back({RuleField::Rating, RuleOp::AtLeast, "4", ""});
    rated.pick = PickBy::HighestRated;
    rated.ordered = true;
    r.emplace_back(L"Top rated", rated);
    return r;
}
