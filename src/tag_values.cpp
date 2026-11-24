// Smart multi-value tag splitting (tag_values.h).
#include "tag_values.h"

#include <algorithm>
#include <cctype>

namespace {

std::string_view TrimSv(std::string_view s) {
    while (!s.empty() && (unsigned char)s.front() <= ' ') s.remove_prefix(1);
    while (!s.empty() && (unsigned char)s.back() <= ' ') s.remove_suffix(1);
    return s;
}

bool EqIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return true;
}

size_t FindIgnoreCase(std::string_view haystack, std::string_view needle, size_t start = 0) {
    if (needle.empty() || haystack.size() < needle.size() + start) return std::string_view::npos;
    for (size_t i = start; i + needle.size() <= haystack.size(); ++i) {
        if (EqIgnoreCase(haystack.substr(i, needle.size()), needle)) return i;
    }
    return std::string_view::npos;
}

bool AllDigits(std::string_view s) {
    if (s.empty()) return false;
    for (unsigned char c : s)
        if (!std::isdigit(c)) return false;
    return true;
}

void PushUnique(std::vector<std::string>& out, std::string_view s) {
    s = TrimSv(s);
    if (s.empty()) return;
    for (const auto& existing : out)
        if (existing == s) return;
    out.emplace_back(s);
}

// Splits on '/' unless the token is an exception, a numeric ratio (24/7, 9/11), or an
// initialism/short-segment name (M/A/R/R/S, N/A). Always splits on spaced " / ".
void SplitSlash(std::string_view s, std::vector<std::string>& out) {
    s = TrimSv(s);
    if (s.empty()) return;
    if (IsMultiValueException(s) || s.find('/') == std::string_view::npos) {
        PushUnique(out, s);
        return;
    }
    if (s.find(" / ") != std::string_view::npos) {
        for (size_t p = 0; p <= s.size();) {
            size_t sep = s.find(" / ", p);
            if (sep == std::string_view::npos) sep = s.size();
            SplitSlash(s.substr(p, sep - p), out);
            p = sep + 3;
        }
        return;
    }
    std::vector<std::string_view> parts;
    for (size_t p = 0; p <= s.size();) {
        size_t sep = s.find('/', p);
        if (sep == std::string_view::npos) sep = s.size();
        parts.push_back(TrimSv(s.substr(p, sep - p)));
        p = sep + 1;
    }
    bool keepWhole = parts.size() < 2;
    if (!keepWhole) {
        bool allNumeric = true;
        for (std::string_view part : parts) {
            if (part.size() <= 1) { keepWhole = true; break; }
            if (!AllDigits(part)) allNumeric = false;
        }
        if (allNumeric) keepWhole = true;
    }
    if (keepWhole) {
        PushUnique(out, s);
        return;
    }
    for (std::string_view part : parts) PushUnique(out, part);
}

void SplitCommaAndAmpersand(std::string_view s, std::vector<std::string>& out) {
    s = TrimSv(s);
    if (s.empty()) return;
    if (IsMultiValueException(s)) {
        PushUnique(out, s);
        return;
    }
    // Split on ", " first, then " & " on each piece (checking exceptions at each step).
    for (size_t p = 0; p <= s.size();) {
        size_t comma = s.find(',', p);
        if (comma == std::string_view::npos) comma = s.size();
        std::string_view piece = TrimSv(s.substr(p, comma - p));
        if (!piece.empty()) {
            if (IsMultiValueException(piece)) {
                PushUnique(out, piece);
            } else {
                size_t amp = FindIgnoreCase(piece, " & ");
                if (amp != std::string_view::npos) {
                    size_t ap = 0;
                    while (ap <= piece.size()) {
                        size_t next = FindIgnoreCase(piece, " & ", ap);
                        if (next == std::string_view::npos) next = piece.size();
                        SplitSlash(piece.substr(ap, next - ap), out);
                        ap = next + 3;
                    }
                } else {
                    SplitSlash(piece, out);
                }
            }
        }
        p = comma + 1;
    }
}

void SplitFeaturing(std::string_view s, TagFieldKind kind, std::vector<std::string>& out) {
    s = TrimSv(s);
    if (s.empty()) return;
    if (IsMultiValueException(s)) {
        PushUnique(out, s);
        return;
    }
    if (kind == TagFieldKind::Artist || kind == TagFieldKind::Composer) {
        static constexpr std::string_view kMarkers[] = {
            " (feat. ", " (ft. ", " (featuring ", " (feat ",
            " [feat. ", " [ft. ", " [featuring ", " [feat ",
            " feat. ", " ft. ", " featuring ", " feat ",
        };
        size_t bestPos = std::string_view::npos;
        size_t bestLen = 0;
        char closeChar = 0;
        for (std::string_view m : kMarkers) {
            size_t pos = FindIgnoreCase(s, m);
            if (pos != std::string_view::npos && pos < bestPos) {
                bestPos = pos;
                bestLen = m.size();
                closeChar = m[1] == '(' ? ')' : m[1] == '[' ? ']' : 0;
            }
        }
        if (bestPos != std::string_view::npos) {
            std::string_view mainPart = TrimSv(s.substr(0, bestPos));
            std::string_view rest = s.substr(bestPos + bestLen);
            std::string_view featPart = rest;
            std::string_view after;
            if (closeChar) {
                size_t close = rest.find(closeChar);
                if (close != std::string_view::npos) {
                    featPart = rest.substr(0, close);
                    after = TrimSv(rest.substr(close + 1));
                }
            }
            SplitFeaturing(mainPart, kind, out);
            SplitCommaAndAmpersand(featPart, out);
            if (!after.empty()) SplitFeaturing(after, kind, out);
            return;
        }
    }
    if (kind == TagFieldKind::Genre && s.find(',') != std::string_view::npos) {
        for (size_t p = 0; p <= s.size();) {
            size_t comma = s.find(',', p);
            if (comma == std::string_view::npos) comma = s.size();
            SplitSlash(s.substr(p, comma - p), out);
            p = comma + 1;
        }
        return;
    }
    SplitSlash(s, out);
}

}  // namespace

bool IsMultiValueException(std::string_view text) {
    text = TrimSv(text);
    if (text.empty()) return false;
    static constexpr std::string_view kExceptions[] = {
        "AC/DC",
        "A/N",
        "G/Z/R",
        "M/A/R/R/S",
        "N/E/R/D",
        "Love/Hate",
        "+/-",
        "30/70",
        "Earth, Wind & Fire",
        "Simon & Garfunkel",
        "Crosby, Stills & Nash",
        "Crosby, Stills, Nash & Young",
        "Emerson, Lake & Palmer",
        "Blood, Sweat & Tears",
        "Peter, Paul & Mary",
        "Sly & The Family Stone",
        "Hall & Oates",
        "Daryl Hall & John Oates",
        "Ike & Tina Turner",
        "Sonny & Cher",
        "Tegan & Sara",
        "Brooks & Dunn",
        "Mumford & Sons",
        "Florence + The Machine",
        "Nick Cave & The Bad Seeds",
        "Tom Petty & The Heartbreakers",
        "Bob Marley & The Wailers",
        "Bruce Springsteen & The E Street Band",
        "Hootie & the Blowfish",
        "Kool & The Gang",
        "KC & The Sunshine Band",
        "Huey Lewis & The News",
        "Siouxsie & The Banshees",
        "Echo & The Bunnymen",
        "The Mamas & The Papas",
        "Above & Beyond",
        "Chase & Status",
        "Of Mice & Men",
        "Iron & Wine",
        "Angus & Julia Stone",
        "Belle & Sebastian",
        "Coheed & Cambria",
        "Sam & Dave",
        "Ashford & Simpson",
        "Peaches & Herb",
        "Captain & Tennille",
        "Tyler, The Creator",
        "10,000 Maniacs",
        "Black Country, New Road",
        "Portugal. The Man",
        "R&B",
        "Rock & Roll",
        "Drum & Bass",
        "Rhythm & Blues",
        "Country & Western",
    };
    for (std::string_view ex : kExceptions)
        if (EqIgnoreCase(text, ex)) return true;
    return false;
}

std::vector<std::string> SplitTagValues(std::string_view text, TagFieldKind kind) {
    std::vector<std::string> out;
    text = TrimSv(text);
    if (text.empty()) return out;
    if (IsMultiValueException(text)) {
        out.emplace_back(text);
        return out;
    }
    for (size_t p = 0; p <= text.size();) {
        size_t semi = text.find(';', p);
        if (semi == std::string_view::npos) semi = text.size();
        SplitFeaturing(text.substr(p, semi - p), kind, out);
        p = semi + 1;
    }
    return out;
}
