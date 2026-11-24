// Inline search query helpers: autocomplete suggestions and one-click conversion to an
// auto-playlist.
#include "search_query.h"

#include "album_editions.h"
#include "textcase.h"

#include <algorithm>
#include <cstdlib>
#include <unordered_map>

namespace {

struct FilterSpec {
    const wchar_t* key;
    const wchar_t* hint;
};

constexpr FilterSpec kFilters[] = {
    {L"year:", L"1990..1999, >=2020"},
    {L"format:", L"flac, mp3, opus, lossless"},
    {L"rating:", L">=4, 5, unrated"},
    {L"played:", L"never, 30d, >90d"},
    {L"added:", L"30d, 2025"},
    {L"bpm:", L"115..125, >=120"},
    {L"edition:", L"remaster, deluxe"},
    {L"dr:", L"<7, >=10"},
    {L"lyrics:", L"word or \"phrase\""},
    {L"artist:", L"name"},
    {L"album:", L"title"},
    {L"title:", L"song title"},
    {L"genre:", L"rock, jazz"},
    {L"plays:", L">=10, 0"},
    {L"length:", L">5:00, 180..300"},
    {L"bitrate:", L">=320"},
    {L"favorite:", L"yes, no"},
    {L"person:", L"artist or credit"},
    {L"missing:", L"genre, year, any"},
};

// Finds the start offset of the trailing token in `query`, respecting double quotes.
size_t TrailingTokenStart(std::wstring_view query) {
    bool quoted = false;
    size_t start = 0;
    for (size_t i = 0; i < query.size(); ++i) {
        if (query[i] == L'"') quoted = !quoted;
        else if (!quoted && (query[i] == L' ' || query[i] == L'\t')) start = i + 1;
    }
    return start;
}

std::wstring QuoteIfNeeded(std::wstring_view prefix, std::wstring_view value) {
    if (value.find(L' ') == std::wstring_view::npos) return std::wstring(prefix) + std::wstring(value);
    return std::wstring(prefix) + L"\"" + std::wstring(value) + L"\"";
}

void SuggestFromLibrary(const Library& library, std::wstring_view prefix, std::wstring_view partial,
                        int which, size_t maxItems, std::vector<SearchSuggestion>& out) {
    std::wstring foldedPartial = textcase::SearchFold(partial);
    struct Entry {
        std::wstring text;
        uint32_t count = 0;
    };
    std::unordered_map<std::wstring, size_t> seen;
    std::vector<Entry> items;
    auto add = [&](std::wstring_view raw) {
        if (raw.empty()) return;
        std::wstring folded = textcase::SearchFold(raw);
        if (!foldedPartial.empty() && folded.find(foldedPartial) == std::wstring::npos) return;
        auto [it, inserted] = seen.emplace(folded, items.size());
        if (inserted) items.push_back({std::wstring(raw), 1});
        else ++items[it->second].count;
    };
    const TrackId n = (TrackId)library.Count();
    // Cap scan for huge libraries so autocomplete stays sub-millisecond.
    const TrackId step = n > 10000 ? n / 10000 : 1;
    for (TrackId id = 0; id < n; id += step) {
        const Track& t = library[id];
        if (!t.InLibrary()) continue;
        if (which == 0) {
            if (t.genre) add(library.Str(t.genre));
        } else if (which == 1) {
            if (t.artist) add(library.Str(t.artist));
        } else if (which == 2) {
            if (t.album) add(library.Str(t.album));
        } else if (which == 3) {
            std::wstring ed = AlbumEditionSearchText(id);
            if (!ed.empty()) add(ed);
        } else if (which == 4) {
            if (t.codec != Codec::Unknown) add(CodecName(t.codec));
        }
    }
    std::sort(items.begin(), items.end(), [&](const Entry& a, const Entry& b) {
        bool ap = !foldedPartial.empty() && textcase::SearchFold(a.text).starts_with(foldedPartial);
        bool bp = !foldedPartial.empty() && textcase::SearchFold(b.text).starts_with(foldedPartial);
        if (ap != bp) return ap;
        if (a.count != b.count) return a.count > b.count;
        return a.text < b.text;
    });
    for (size_t i = 0; i < items.size() && out.size() < maxItems; ++i) {
        out.push_back({QuoteIfNeeded(prefix, items[i].text), items[i].text,
                       std::to_wstring(items[i].count) + (items[i].count == 1 ? L" track" : L" tracks")});
    }
}

bool ParseNum(std::wstring_view s, double& out) {
    if (s.empty()) return false;
    std::string u = ToUtf8(s);
    char* end = nullptr;
    out = std::strtod(u.c_str(), &end);
    return end != u.c_str() && *end == '\0';
}

bool ParseDurationSec(std::wstring_view s, std::string& out) {
    if (s.empty()) return false;
    if (s.back() == L's' || s.back() == L'S') {
        s.remove_suffix(1);
        double v = 0;
        if (!ParseNum(s, v)) return false;
        out = std::to_string((int)std::lround(v));
        return true;
    }
    if (s.back() == L'm' || s.back() == L'M') {
        s.remove_suffix(1);
        double v = 0;
        if (!ParseNum(s, v)) return false;
        out = std::to_string((int)std::lround(v * 60.0));
        return true;
    }
    out = ToUtf8(s);
    return true;
}

bool AddNumericRule(RuleField f, std::wstring_view expr, bool isInt, bool isDuration, std::vector<Rule>& rules) {
    if (expr.empty()) return false;
    auto fmt = [&](std::wstring_view s, double delta) -> std::string {
        if (isDuration) {
            std::string sec;
            if (ParseDurationSec(s, sec)) {
                if (delta == 0) return sec;
                double v = std::atof(sec.c_str()) + delta;
                return std::to_string((int)std::lround(v));
            }
        }
        if (delta == 0) return ToUtf8(s);
        double v = 0;
        if (!ParseNum(s, v)) return ToUtf8(s);
        if (isInt) return std::to_string((long long)std::llround(v + (delta > 0 ? 1.0 : -1.0)));
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.2f", v + delta);
        return buf;
    };
    if (size_t dots = expr.find(L".."); dots != std::wstring_view::npos) {
        std::wstring_view lo = expr.substr(0, dots), hi = expr.substr(dots + 2);
        if (!lo.empty() && !hi.empty()) {
            rules.push_back({f, RuleOp::Between, fmt(lo, 0), fmt(hi, 0)});
            return true;
        }
        if (!lo.empty()) { rules.push_back({f, RuleOp::AtLeast, fmt(lo, 0), ""}); return true; }
        if (!hi.empty()) { rules.push_back({f, RuleOp::AtMost, fmt(hi, 0), ""}); return true; }
        return false;
    }
    if (expr.starts_with(L">=")) { rules.push_back({f, RuleOp::AtLeast, fmt(expr.substr(2), 0), ""}); return true; }
    if (expr.starts_with(L"<=")) { rules.push_back({f, RuleOp::AtMost, fmt(expr.substr(2), 0), ""}); return true; }
    if (expr.starts_with(L"!=")) { rules.push_back({f, RuleOp::NotEquals, fmt(expr.substr(2), 0), ""}); return true; }
    if (expr.starts_with(L">")) { rules.push_back({f, RuleOp::AtLeast, fmt(expr.substr(1), 0.1), ""}); return true; }
    if (expr.starts_with(L"<")) { rules.push_back({f, RuleOp::AtMost, fmt(expr.substr(1), -0.1), ""}); return true; }
    if (expr.starts_with(L"=")) { rules.push_back({f, RuleOp::Equals, fmt(expr.substr(1), 0), ""}); return true; }
    rules.push_back({f, RuleOp::Equals, fmt(expr, 0), ""});
    return true;
}

bool ParseDays(std::wstring_view s, int& days) {
    if (s.empty()) return false;
    int mult = 1;
    wchar_t u = s.back();
    if (u == L'd' || u == L'D') s.remove_suffix(1);
    else if (u == L'w' || u == L'W') { mult = 7; s.remove_suffix(1); }
    else if (u == L'm' || u == L'M') { mult = 30; s.remove_suffix(1); }
    else if (u == L'y' || u == L'Y') { mult = 365; s.remove_suffix(1); }
    else return false;
    double n = 0;
    if (!ParseNum(s, n) || n < 0) return false;
    days = (int)std::lround(n * mult);
    return true;
}

bool AddDateRule(RuleField f, std::wstring_view expr, bool allowNever, std::vector<Rule>& rules) {
    std::wstring folded = textcase::SearchFold(expr);
    if (allowNever && (folded == L"never" || folded == L"none" || folded == L"0")) {
        rules.push_back({f, RuleOp::Never, "", ""});
        return true;
    }
    std::wstring_view sv = folded;
    bool gt = false;
    if (sv.starts_with(L">=")) { gt = true; sv.remove_prefix(2); }
    else if (sv.starts_with(L"<=")) sv.remove_prefix(2);
    else if (sv.starts_with(L">")) { gt = true; sv.remove_prefix(1); }
    else if (sv.starts_with(L"<")) sv.remove_prefix(1);
    int days = 0;
    if (ParseDays(sv, days)) {
        rules.push_back({f, gt ? RuleOp::NotInLastDays : RuleOp::InLastDays, std::to_string(days), ""});
        return true;
    }
    if (size_t dots = expr.find(L".."); dots != std::wstring_view::npos) {
        std::wstring_view lo = expr.substr(0, dots), hi = expr.substr(dots + 2);
        if (!lo.empty()) rules.push_back({f, RuleOp::OnOrAfter, ToUtf8(lo), ""});
        if (!hi.empty()) rules.push_back({f, RuleOp::Before, ToUtf8(hi), ""});
        return !lo.empty() || !hi.empty();
    }
    if (expr.starts_with(L">=") || expr.starts_with(L">")) {
        size_t skip = expr[1] == L'=' ? 2 : 1;
        rules.push_back({f, RuleOp::OnOrAfter, ToUtf8(expr.substr(skip)), ""});
        return true;
    }
    if (expr.starts_with(L"<=") || expr.starts_with(L"<")) {
        size_t skip = expr[1] == L'=' ? 2 : 1;
        rules.push_back({f, RuleOp::Before, ToUtf8(expr.substr(skip)), ""});
        return true;
    }
    rules.push_back({f, RuleOp::OnOrAfter, ToUtf8(expr), ""});
    return true;
}

}  // namespace

std::vector<SearchSuggestion> SearchSuggestions(const Library& library, std::wstring_view query, size_t maxItems) {
    std::vector<SearchSuggestion> out;
    if (query.empty() || maxItems == 0) return out;
    const size_t start = TrailingTokenStart(query);
    std::wstring_view token = query.substr(start);
    if (token.empty()) return out;
    if (token.front() == L'"') token.remove_prefix(1);
    if (token.empty()) return out;

    size_t colon = token.find(L':');
    if (colon == std::wstring_view::npos) {
        std::wstring folded = textcase::SearchFold(token);
        for (const auto& f : kFilters) {
            if (std::wstring_view(f.key).starts_with(folded)) {
                out.push_back({f.key, f.key, f.hint});
                if (out.size() >= maxItems) break;
            }
        }
        return out;
    }

    std::wstring key = textcase::SearchFold(token.substr(0, colon));
    std::wstring_view val = token.substr(colon + 1);
    if (!val.empty() && val.front() == L'"') val.remove_prefix(1);
    std::wstring foldedVal = textcase::SearchFold(val);
    std::wstring prefix = std::wstring(token.substr(0, colon + 1));

    auto addFixed = [&](std::initializer_list<std::pair<const wchar_t*, const wchar_t*>> items) {
        for (auto [v, hint] : items) {
            if (!foldedVal.empty() && textcase::SearchFold(v).find(foldedVal) == std::wstring::npos) continue;
            out.push_back({prefix + v, v, hint});
            if (out.size() >= maxItems) break;
        }
    };

    if (key == L"format" || key == L"codec") {
        SuggestFromLibrary(library, prefix, val, 4, maxItems, out);
        if (out.size() < maxItems) addFixed({{L"lossless", L"FLAC, ALAC, WAV..."}, {L"lossy", L"MP3, Opus, AAC..."}});
    } else if (key == L"played") {
        addFixed({{L"never", L"unplayed tracks"}, {L"7d", L"last 7 days"}, {L"30d", L"last 30 days"},
                  {L"90d", L"last 90 days"}, {L">90d", L"not played recently"}});
    } else if (key == L"added") {
        addFixed({{L"7d", L"last 7 days"}, {L"30d", L"last 30 days"}, {L"90d", L"last 90 days"}, {L"365d", L"last year"}});
    } else if (key == L"rating") {
        addFixed({{L">=4", L"4 stars and up"}, {L">=4.5", L"4.5 stars and up"}, {L"5", L"5 stars"}, {L"unrated", L"no rating"}});
    } else if (key == L"dr") {
        addFixed({{L"<7", L"compressed / loud"}, {L">=10", L"dynamic"}, {L"8..14", L"range in LU"}});
    } else if (key == L"favorite" || key == L"fav") {
        addFixed({{L"yes", L"favorites only"}, {L"no", L"non-favorites"}});
    } else if (key == L"missing") {
        addFixed({{L"genre", L"empty genre"}, {L"year", L"empty date"}, {L"album", L"empty album"},
                  {L"artist", L"empty artist"}, {L"albumartist", L"empty album artist"},
                  {L"composer", L"empty composer"}, {L"track", L"no track number"}, {L"any", L"any core tag"}});
    } else if (key == L"genre") {
        SuggestFromLibrary(library, prefix, val, 0, maxItems, out);
    } else if (key == L"artist" || key == L"albumartist" || key == L"person") {
        SuggestFromLibrary(library, prefix, val, 1, maxItems, out);
    } else if (key == L"album") {
        SuggestFromLibrary(library, prefix, val, 2, maxItems, out);
    } else if (key == L"edition") {
        SuggestFromLibrary(library, prefix, val, 3, maxItems, out);
    }
    return out;
}

std::wstring ApplySearchSuggestion(std::wstring_view query, std::wstring_view insert) {
    size_t start = TrailingTokenStart(query);
    std::wstring out(query.substr(0, start));
    out.append(insert);
    if (!insert.empty() && insert.back() != L':') out += L' ';
    return out;
}

bool SearchToAutoPlaylist(std::wstring_view query, AutoPlaylist& out) {
    out = AutoPlaylist{};
    out.root.all = true;
    const auto words = FilterWords(query);
    if (words.empty()) return false;

    for (const std::wstring& word : words) {
        size_t colon = word.find(L':');
        if (colon != std::wstring::npos && colon > 0 && colon + 1 < word.size()) {
            std::wstring key = textcase::SearchFold(std::wstring_view(word).substr(0, colon));
            std::wstring_view val = std::wstring_view(word).substr(colon + 1);
            std::wstring foldedVal = textcase::SearchFold(val);
            if (key == L"missing") {
                if (foldedVal == L"title") { out.root.rules.push_back({RuleField::Title, RuleOp::IsEmpty, "", ""}); continue; }
                if (foldedVal == L"artist") { out.root.rules.push_back({RuleField::Artist, RuleOp::IsEmpty, "", ""}); continue; }
                if (foldedVal == L"album") { out.root.rules.push_back({RuleField::Album, RuleOp::IsEmpty, "", ""}); continue; }
                if (foldedVal == L"albumartist") { out.root.rules.push_back({RuleField::AlbumArtist, RuleOp::IsEmpty, "", ""}); continue; }
                if (foldedVal == L"year" || foldedVal == L"date") { out.root.rules.push_back({RuleField::Year, RuleOp::IsEmpty, "", ""}); continue; }
                if (foldedVal == L"genre") { out.root.rules.push_back({RuleField::Genre, RuleOp::IsEmpty, "", ""}); continue; }
                if (foldedVal == L"track") { out.root.rules.push_back({RuleField::TrackNo, RuleOp::IsEmpty, "", ""}); continue; }
                if (foldedVal == L"composer") { out.root.rules.push_back({RuleField::Composer, RuleOp::IsEmpty, "", ""}); continue; }
                if (foldedVal == L"any") { out.root.rules.push_back({RuleField::Any, RuleOp::IsEmpty, "", ""}); continue; }
            }
            if (key == L"artist") { out.root.rules.push_back({RuleField::Artist, RuleOp::Contains, ToUtf8(val), ""}); continue; }
            if (key == L"albumartist") { out.root.rules.push_back({RuleField::AlbumArtist, RuleOp::Contains, ToUtf8(val), ""}); continue; }
            if (key == L"title") { out.root.rules.push_back({RuleField::Title, RuleOp::Contains, ToUtf8(val), ""}); continue; }
            if (key == L"album") { out.root.rules.push_back({RuleField::Album, RuleOp::Contains, ToUtf8(val), ""}); continue; }
            if (key == L"genre") { out.root.rules.push_back({RuleField::Genre, RuleOp::Contains, ToUtf8(val), ""}); continue; }
            if (key == L"composer") { out.root.rules.push_back({RuleField::Composer, RuleOp::Contains, ToUtf8(val), ""}); continue; }
            if (key == L"comment") { out.root.rules.push_back({RuleField::Comment, RuleOp::Contains, ToUtf8(val), ""}); continue; }
            if (key == L"grouping") { out.root.rules.push_back({RuleField::Grouping, RuleOp::Contains, ToUtf8(val), ""}); continue; }
            if (key == L"label") { out.root.rules.push_back({RuleField::Label, RuleOp::Contains, ToUtf8(val), ""}); continue; }
            if (key == L"path") { out.root.rules.push_back({RuleField::Path, RuleOp::Contains, ToUtf8(val), ""}); continue; }
            if (key == L"edition") { out.root.rules.push_back({RuleField::Edition, RuleOp::Contains, ToUtf8(val), ""}); continue; }
            if (key == L"lyrics") { out.root.rules.push_back({RuleField::Lyrics, RuleOp::Contains, ToUtf8(val), ""}); continue; }
            if (key == L"format" || key == L"codec") {
                out.root.rules.push_back({RuleField::Codec, RuleOp::Contains, ToUtf8(val), ""});
                continue;
            }
            if (key == L"person") {
                out.root.rules.push_back({RuleField::Any, RuleOp::Contains, ToUtf8(val), ""});
                continue;
            }
            if ((key == L"year" || key == L"date") && AddNumericRule(RuleField::Year, val, true, false, out.root.rules)) continue;
            if (key == L"rating") {
                if (foldedVal == L"unrated" || foldedVal == L"none" || foldedVal == L"0") {
                    out.root.rules.push_back({RuleField::Rating, RuleOp::IsEmpty, "", ""});
                    continue;
                }
                if (AddNumericRule(RuleField::Rating, val, false, false, out.root.rules)) continue;
            }
            if (key == L"bpm" && AddNumericRule(RuleField::Bpm, val, true, false, out.root.rules)) continue;
            if (key == L"dr" && AddNumericRule(RuleField::Dr, val, false, false, out.root.rules)) continue;
            if ((key == L"plays" || key == L"playcount") && AddNumericRule(RuleField::PlayCount, val, true, false, out.root.rules)) continue;
            if (key == L"bitrate" && AddNumericRule(RuleField::Bitrate, val, true, false, out.root.rules)) continue;
            if ((key == L"length" || key == L"duration") && AddNumericRule(RuleField::Length, val, true, true, out.root.rules)) continue;
            if (key == L"favorite" || key == L"fav") {
                bool yes = foldedVal == L"yes" || foldedVal == L"true" || foldedVal == L"1" || foldedVal == L"on";
                out.root.rules.push_back({RuleField::Favorite, RuleOp::Equals, yes ? "1" : "0", ""});
                continue;
            }
            if (key == L"played" && AddDateRule(RuleField::LastPlayed, val, true, out.root.rules)) continue;
            if (key == L"added" && AddDateRule(RuleField::DateAdded, val, false, out.root.rules)) continue;
        }
        out.root.rules.push_back({RuleField::Any, RuleOp::Contains, ToUtf8(word), ""});
    }
    return !out.root.rules.empty();
}
