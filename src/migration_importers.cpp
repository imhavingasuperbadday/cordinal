// Portable importers for iTunes/Apple Music Library.xml, MusicBee CSV/TSV exports, and XSPF playlists.
#include "migration.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <map>

namespace migration {
namespace {

constexpr size_t kMaxImportBytes = 256 * 1024 * 1024;

std::string_view StripBom(std::string_view s) {
    return s.starts_with("\xEF\xBB\xBF") ? s.substr(3) : s;
}

std::string LowerAscii(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) out += (char)(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
    return out;
}

std::string TrimAscii(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
        s.remove_suffix(1);
    return std::string(s);
}

void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7F) {
        out += (char)cp;
    } else if (cp <= 0x7FF) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp <= 0x10FFFF) {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

std::string DecodeXmlEntities(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        size_t semi = s.find(';', i + 1);
        if (semi == s.npos || semi - i > 12) {
            out += s[i];
            continue;
        }
        std::string_view ent = s.substr(i + 1, semi - i - 1);
        if (ent == "amp")
            out += '&';
        else if (ent == "lt")
            out += '<';
        else if (ent == "gt")
            out += '>';
        else if (ent == "quot")
            out += '"';
        else if (ent == "apos")
            out += '\'';
        else if (ent.starts_with("#x") || ent.starts_with("#X")) {
            uint32_t cp = 0;
            auto res = std::from_chars(ent.data() + 2, ent.data() + ent.size(), cp, 16);
            if (res.ec == std::errc() && res.ptr == ent.data() + ent.size() && cp > 0) AppendUtf8(out, cp);
        } else if (ent.starts_with('#')) {
            uint32_t cp = 0;
            auto res = std::from_chars(ent.data() + 1, ent.data() + ent.size(), cp, 10);
            if (res.ec == std::errc() && res.ptr == ent.data() + ent.size() && cp > 0) AppendUtf8(out, cp);
        } else {
            out += '&';
            continue;
        }
        i = semi;
    }
    return out;
}

int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::wstring DecodeFileUri(std::string_view uri) {
    if (uri.empty()) return {};
    std::string s(uri);
    if (s.starts_with("file://localhost/"))
        s.erase(0, 16);  // keep leading '/'
    else if (s.starts_with("file:///"))
        s.erase(0, 7);   // keep leading '/'
    else if (s.starts_with("file://"))
        s.erase(0, 5);   // UNC-like //server/share
    std::string decoded;
    decoded.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int hi = HexVal(s[i + 1]), lo = HexVal(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                decoded += (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        decoded += s[i];
    }
    // On Windows file URIs like file://localhost/C:/Music/..., drop the leading '/' before 'C:'.
    if (decoded.size() >= 3 && decoded[0] == '/' &&
        ((decoded[1] >= 'A' && decoded[1] <= 'Z') || (decoded[1] >= 'a' && decoded[1] <= 'z')) &&
        decoded[2] == ':') {
        decoded.erase(0, 1);
    }
    std::wstring w = FromUtf8(decoded);
#ifdef _WIN32
    for (wchar_t& ch : w)
        if (ch == L'/') ch = L'\\';
#endif
    return w;
}

// Howard Hinnant's days_from_civil algorithm (UTC).
int64_t DaysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097ll + (int64_t)doe - 719468ll;
}

bool ParseIsoDate(std::string_view s, uint32_t& out) {
    s = std::string_view(s);
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    if (s.empty()) {
        out = 0;
        return true;
    }
    if (s.size() < 10 || s[4] != '-' || s[7] != '-') return false;
    auto parseNum = [&](size_t at, size_t len, unsigned& val) {
        auto r = std::from_chars(s.data() + at, s.data() + at + len, val);
        return r.ec == std::errc() && r.ptr == s.data() + at + len;
    };
    unsigned yr = 0, mo = 0, dy = 0, hr = 0, mn = 0, sc = 0;
    if (!parseNum(0, 4, yr) || !parseNum(5, 2, mo) || !parseNum(8, 2, dy)) return false;
    if (mo < 1 || mo > 12 || dy < 1 || dy > 31) return false;
    if (s.size() >= 19 && (s[10] == 'T' || s[10] == ' ')) {
        if (s[13] != ':' || s[16] != ':') return false;
        if (!parseNum(11, 2, hr) || !parseNum(14, 2, mn) || !parseNum(17, 2, sc)) return false;
        if (hr > 23 || mn > 59 || sc > 59) return false;
    }
    int64_t days = DaysFromCivil((int)yr, mo, dy);
    int64_t secs = days * 86400ll + hr * 3600ll + mn * 60ll + sc;
    if (secs <= 0 || secs > UINT32_MAX) {
        out = 0;
        return secs == 0;
    }
    out = (uint32_t)secs;
    return true;
}

bool ParseUint64(std::string_view s, uint64_t& out) {
    if (s.empty()) return false;
    auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc() && r.ptr == s.data() + s.size();
}

// Lightweight XML token reader: skips comments, PI (<?...?>), and <!DOCTYPE ...> without expanding entities.
struct XmlCursor {
    std::string_view s;
    size_t pos = 0;
    bool error = false;

    void SkipWs() {
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\r' || s[pos] == '\n')) ++pos;
    }

    bool NextTag(std::string_view& name, bool& isEnd, bool& isEmpty, std::string& innerText) {
        innerText.clear();
        while (pos < s.size()) {
            size_t lt = s.find('<', pos);
            if (lt == s.npos) {
                pos = s.size();
                return false;
            }
            if (lt > pos) innerText += DecodeXmlEntities(s.substr(pos, lt - pos));
            pos = lt;
            if (s.substr(pos).starts_with("<!--")) {
                size_t end = s.find("-->", pos + 4);
                if (end == s.npos) {
                    error = true;
                    return false;
                }
                pos = end + 3;
                continue;
            }
            if (s.substr(pos).starts_with("<?")) {
                size_t end = s.find("?>", pos + 2);
                if (end == s.npos) {
                    error = true;
                    return false;
                }
                pos = end + 2;
                continue;
            }
            if (s.substr(pos).starts_with("<!")) {
                if (s.substr(pos).starts_with("<!ENTITY")) {
                    error = true;
                    return false;
                }
                if (s.substr(pos).starts_with("<![CDATA[")) {
                    size_t end = s.find("]]>", pos + 9);
                    if (end == s.npos) {
                        error = true;
                        return false;
                    }
                    innerText += s.substr(pos + 9, end - (pos + 9));
                    pos = end + 3;
                    continue;
                }
                // Skip <!DOCTYPE ...> (including optional [...] internal subset, rejecting <!ENTITY inside).
                size_t i = pos + 2;
                bool inQuote = false;
                char q = 0;
                int bracket = 0;
                while (i < s.size()) {
                    char c = s[i];
                    if (inQuote) {
                        if (c == q) inQuote = false;
                    } else if (c == '"' || c == '\'') {
                        inQuote = true;
                        q = c;
                    } else if (c == '[') {
                        ++bracket;
                    } else if (c == ']' && bracket > 0) {
                        --bracket;
                    } else if (c == '>' && bracket == 0) {
                        break;
                    }
                    ++i;
                }
                if (i >= s.size() || s.substr(pos, i - pos).find("<!ENTITY") != std::string_view::npos) {
                    error = true;
                    return false;
                }
                pos = i + 1;
                continue;
            }
            // Normal <tag ...> or </tag>
            size_t i = pos + 1;
            isEnd = (i < s.size() && s[i] == '/');
            if (isEnd) ++i;
            size_t nameStart = i;
            while (i < s.size() && s[i] != '>' && s[i] != '/' && s[i] != ' ' && s[i] != '\t' && s[i] != '\r' &&
                   s[i] != '\n')
                ++i;
            name = s.substr(nameStart, i - nameStart);
            bool inQuote = false;
            char q = 0;
            while (i < s.size()) {
                char c = s[i];
                if (inQuote) {
                    if (c == q) inQuote = false;
                } else if (c == '"' || c == '\'') {
                    inQuote = true;
                    q = c;
                } else if (c == '>') {
                    break;
                }
                ++i;
            }
            if (i >= s.size() || name.empty()) {
                error = true;
                return false;
            }
            isEmpty = (!isEnd && i > pos + 1 && s[i - 1] == '/');
            pos = i + 1;
            return true;
        }
        return false;
    }

    // Reads text until the matching </tag> (assuming simple leaf element like <string>...</string>).
    bool ReadLeafText(std::string_view tagName, bool wasEmpty, std::string& out) {
        out.clear();
        if (wasEmpty) return true;
        std::string_view name;
        bool isEnd = false, isEmpty = false;
        std::string text;
        int depth = 1;
        while (NextTag(name, isEnd, isEmpty, text)) {
            if (depth == 1 && isEnd && name == tagName) {
                out = std::move(text);
                return true;
            }
            if (!isEnd && !isEmpty)
                ++depth;
            else if (isEnd) {
                if (--depth <= 0) break;
            }
        }
        error = true;
        return false;
    }

    bool SkipElement(bool wasEmpty) {
        if (wasEmpty) return true;
        int depth = 1;
        std::string_view name;
        bool isEnd = false, isEmpty = false;
        std::string text;
        while (NextTag(name, isEnd, isEmpty, text)) {
            if (!isEnd && !isEmpty) {
                if (++depth > 64) {
                    error = true;
                    return false;
                }
            } else if (isEnd) {
                if (--depth == 0) return true;
            }
        }
        error = true;
        return false;
    }
};

uint8_t NormalizeRating0To100(uint64_t raw) {
    if (raw == 0) return 0;
    if (raw > 100) raw = 100;
    uint64_t rounded = ((raw + 5) / 10) * 10;
    return (uint8_t)std::clamp<uint64_t>(rounded, 10, 100);
}

bool ParseRatingString(std::string_view raw, uint8_t& out) {
    std::string s = TrimAscii(raw);
    if (s.empty()) {
        out = 0;
        return false;
    }
    // Count UTF-8 star characters '★' (\xE2\x98\x85)
    if (s.find("\xE2\x98\x85") != s.npos) {
        size_t stars = 0;
        for (size_t i = 0; i + 2 < s.size();) {
            if ((unsigned char)s[i] == 0xE2 && (unsigned char)s[i + 1] == 0x98 && (unsigned char)s[i + 2] == 0x85) {
                ++stars;
                i += 3;
            } else {
                ++i;
            }
        }
        if (stars == 0) return false;
        out = (uint8_t)std::min<size_t>(stars * 20, 100);
        return true;
    }
    for (char& c : s)
        if (c == ',') c = '.';
    char* end = nullptr;
    double val = std::strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size() || val < 0.0) return false;
    if (val == 0.0) {
        out = 0;
        return true;
    }
    if (val <= 5.0) {
        out = (uint8_t)std::clamp<int>((int)(val * 20.0 + 0.5), 10, 100);
        // Snap to 10s (half-stars).
        out = (uint8_t)(((out + 5) / 10) * 10);
        if (out < 10) out = 10;
        return true;
    }
    if (val <= 100.0) {
        out = NormalizeRating0To100((uint64_t)(val + 0.5));
        return true;
    }
    return false;
}

bool ParseDelimitedTable(std::string_view text, char delim, std::vector<std::vector<std::string>>& out) {
    std::vector<std::vector<std::string>> table;
    std::vector<std::string> row;
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
            } else {
                cell += c;
            }
        } else if (c == delim || c == '\r' || c == '\n') {
            row.push_back(std::move(cell));
            cell.clear();
            ended = false;
            beginning = true;
            if (c != delim) {
                if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
                table.push_back(std::move(row));
                row.clear();
            }
        } else if (ended) {
            return false;
        } else if (c == '"') {
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
    for (const auto& r : table)
        if (r.size() != table[0].size()) return false;
    out = std::move(table);
    return true;
}

char DetectDelimiter(std::string_view text) {
    size_t nl = text.find_first_of("\r\n");
    std::string_view header = nl == text.npos ? text : text.substr(0, nl);
    bool inQuote = false;
    size_t tabs = 0, commas = 0, semis = 0;
    for (char c : header) {
        if (c == '"')
            inQuote = !inQuote;
        else if (!inQuote) {
            if (c == '\t')
                ++tabs;
            else if (c == ',')
                ++commas;
            else if (c == ';')
                ++semis;
        }
    }
    if (tabs > commas && tabs >= semis) return '\t';
    if (semis > commas && semis > tabs) return ';';
    return ',';
}

}  // namespace

bool ReadItunesLibrary(std::string_view xml, ImportBundle& out, std::wstring& error) {
    out = {};
    xml = StripBom(xml);
    if (xml.size() > kMaxImportBytes || xml.find('\0') != xml.npos) {
        error = L"The iTunes XML file is empty or exceeds the 256 MB limit.";
        return false;
    }
    XmlCursor cur{xml};
    std::string_view tag;
    bool isEnd = false, isEmpty = false;
    std::string text;
    if (!cur.NextTag(tag, isEnd, isEmpty, text) || isEnd || tag != "plist" || isEmpty) {
        error = L"This file is not an iTunes Library.xml property list.";
        return false;
    }
    if (!cur.NextTag(tag, isEnd, isEmpty, text) || isEnd || tag != "dict" || isEmpty) {
        error = L"This iTunes Library.xml has no root dictionary.";
        return false;
    }

    std::map<int64_t, size_t> trackIndexById;
    bool sawTracks = false;

    // Top-level <dict> keys.
    while (cur.NextTag(tag, isEnd, isEmpty, text)) {
        if (isEnd && tag == "dict") break;
        if (isEnd || tag != "key") {
            error = L"The iTunes Library.xml structure is invalid.";
            return false;
        }
        std::string topKey;
        if (!cur.ReadLeafText("key", isEmpty, topKey)) {
            error = L"The iTunes Library.xml key is malformed.";
            return false;
        }
        if (!cur.NextTag(tag, isEnd, isEmpty, text) || isEnd) {
            error = L"The iTunes Library.xml value is missing.";
            return false;
        }
        if (topKey == "Tracks" && tag == "dict") {
            sawTracks = true;
            if (isEmpty) continue;
            while (cur.NextTag(tag, isEnd, isEmpty, text)) {
                if (isEnd && tag == "dict") break;
                if (isEnd || tag != "key") {
                    error = L"Invalid track entry in iTunes Library.xml.";
                    return false;
                }
                std::string trackKey;
                if (!cur.ReadLeafText("key", isEmpty, trackKey)) {
                    error = L"Invalid track key in iTunes Library.xml.";
                    return false;
                }
                if (!cur.NextTag(tag, isEnd, isEmpty, text) || isEnd || tag != "dict") {
                    error = L"Invalid track dictionary in iTunes Library.xml.";
                    return false;
                }
                if (isEmpty) continue;

                Statistics st;
                int64_t trackId = -1;
                bool ratingComputed = false;
                uint64_t rawRating = 0;
                bool hasRawRating = false;
                std::string kindStr;
                while (cur.NextTag(tag, isEnd, isEmpty, text)) {
                    if (isEnd && tag == "dict") break;
                    if (isEnd || tag != "key") {
                        error = L"Invalid track field in iTunes Library.xml.";
                        return false;
                    }
                    std::string k;
                    if (!cur.ReadLeafText("key", isEmpty, k)) {
                        error = L"Invalid track field key in iTunes Library.xml.";
                        return false;
                    }
                    if (!cur.NextTag(tag, isEnd, isEmpty, text) || isEnd) {
                        error = L"Missing track field value in iTunes Library.xml.";
                        return false;
                    }
                    if (tag == "true") {
                        if (k == "Rating Computed") ratingComputed = true;
                        if (!isEmpty && !cur.SkipElement(false)) break;
                        continue;
                    }
                    if (tag == "false") {
                        if (!isEmpty && !cur.SkipElement(false)) break;
                        continue;
                    }
                    if (tag == "dict" || tag == "array" || tag == "data") {
                        if (!cur.SkipElement(isEmpty)) break;
                        continue;
                    }
                    std::string val;
                    if (!cur.ReadLeafText(tag, isEmpty, val)) break;
                    if (k == "Track ID") {
                        uint64_t v = 0;
                        if (ParseUint64(val, v)) trackId = (int64_t)v;
                    } else if (k == "Name") {
                        st.title = std::move(val);
                    } else if (k == "Artist") {
                        st.artist = std::move(val);
                    } else if (k == "Album") {
                        st.album = std::move(val);
                    } else if (k == "Location") {
                        st.path = DecodeFileUri(val);
                    } else if (k == "Total Time") {
                        uint64_t ms = 0;
                        if (ParseUint64(val, ms) && ms <= UINT32_MAX) st.durationMs = (uint32_t)ms;
                    } else if (k == "Play Count") {
                        uint64_t c = 0;
                        if (ParseUint64(val, c) && c <= UINT32_MAX) {
                            st.count = (uint32_t)c;
                            st.hasCount = true;
                        }
                    } else if (k == "Skip Count") {
                        uint64_t c = 0;
                        if (ParseUint64(val, c) && c <= UINT32_MAX) {
                            st.skips = (uint32_t)c;
                            st.hasSkips = true;
                        }
                    } else if (k == "Rating") {
                        if (ParseUint64(val, rawRating)) hasRawRating = true;
                    } else if (k == "Date Added") {
                        ParseIsoDate(val, st.added);
                    } else if (k == "Play Date UTC") {
                        ParseIsoDate(val, st.last);
                    }
                }
                if (cur.error) {
                    error = L"The iTunes Library.xml track data is malformed.";
                    return false;
                }
                if (hasRawRating && !ratingComputed) {
                    st.rating = NormalizeRating0To100(rawRating);
                    st.hasRating = true;
                }
                if (st.path.empty() && st.title.empty()) {
                    ++out.unlocated;
                } else {
                    size_t idx = out.statistics.size();
                    out.statistics.push_back(std::move(st));
                    if (trackId >= 0) trackIndexById[trackId] = idx;
                }
            }
        } else if (topKey == "Playlists" && tag == "array") {
            if (isEmpty) continue;
            while (cur.NextTag(tag, isEnd, isEmpty, text)) {
                if (isEnd && tag == "array") break;
                if (isEnd || tag != "dict") {
                    error = L"Invalid playlist entry in iTunes Library.xml.";
                    return false;
                }
                if (isEmpty) continue;
                std::string name;
                bool skipPlaylist = false;
                std::vector<int64_t> itemIds;
                while (cur.NextTag(tag, isEnd, isEmpty, text)) {
                    if (isEnd && tag == "dict") break;
                    if (isEnd || tag != "key") {
                        error = L"Invalid playlist field in iTunes Library.xml.";
                        return false;
                    }
                    std::string k;
                    if (!cur.ReadLeafText("key", isEmpty, k)) break;
                    if (!cur.NextTag(tag, isEnd, isEmpty, text) || isEnd) {
                        cur.error = true;
                        break;
                    }
                    if (k == "Playlist Items" && tag == "array") {
                        if (isEmpty) continue;
                        while (cur.NextTag(tag, isEnd, isEmpty, text)) {
                            if (isEnd && tag == "array") break;
                            if (isEnd || tag != "dict") {
                                cur.error = true;
                                break;
                            }
                            if (isEmpty) continue;
                            while (cur.NextTag(tag, isEnd, isEmpty, text)) {
                                if (isEnd && tag == "dict") break;
                                if (isEnd || tag != "key") {
                                    cur.error = true;
                                    break;
                                }
                                std::string ik;
                                if (!cur.ReadLeafText("key", isEmpty, ik)) break;
                                if (!cur.NextTag(tag, isEnd, isEmpty, text) || isEnd) {
                                    cur.error = true;
                                    break;
                                }
                                if (tag == "dict" || tag == "array" || tag == "data") {
                                    cur.SkipElement(isEmpty);
                                } else {
                                    std::string iv;
                                    cur.ReadLeafText(tag, isEmpty, iv);
                                    uint64_t tid = 0;
                                    if (ik == "Track ID" && ParseUint64(iv, tid)) itemIds.push_back((int64_t)tid);
                                }
                            }
                            if (cur.error) break;
                        }
                    } else if (tag == "true") {
                        if (k == "Master" || k == "Folder") skipPlaylist = true;
                        if (!isEmpty) cur.SkipElement(false);
                    } else if (tag == "false") {
                        if (!isEmpty) cur.SkipElement(false);
                    } else if (tag == "dict" || tag == "array" || tag == "data") {
                        cur.SkipElement(isEmpty);
                    } else {
                        std::string val;
                        cur.ReadLeafText(tag, isEmpty, val);
                        if (k == "Name")
                            name = std::move(val);
                        else if (k == "Distinguished Kind")
                            skipPlaylist = true;
                    }
                    if (cur.error) break;
                }
                if (cur.error) {
                    error = L"The iTunes Library.xml playlist data is malformed.";
                    return false;
                }
                if (!skipPlaylist && !name.empty() && !itemIds.empty()) {
                    ImportedPlaylist pl;
                    pl.name = std::move(name);
                    for (int64_t tid : itemIds) {
                        auto it = trackIndexById.find(tid);
                        if (it != trackIndexById.end()) pl.tracks.push_back(out.statistics[it->second]);
                    }
                    if (!pl.tracks.empty()) out.playlists.push_back(std::move(pl));
                }
            }
        } else {
            if (!cur.SkipElement(isEmpty)) {
                error = L"The iTunes Library.xml structure is malformed.";
                return false;
            }
        }
        if (cur.error) {
            error = L"The iTunes Library.xml file is malformed.";
            return false;
        }
    }
    if (!sawTracks) {
        error = L"This iTunes Library.xml has no Tracks section.";
        return false;
    }
    return true;
}

bool ReadMusicBeeCsv(std::string_view text, ImportBundle& out, std::wstring& error) {
    out = {};
    text = StripBom(text);
    if (text.empty() || text.size() > kMaxImportBytes || text.find('\0') != text.npos) {
        error = L"The MusicBee export file is empty or invalid.";
        return false;
    }
    char delim = DetectDelimiter(text);
    std::vector<std::vector<std::string>> table;
    if (!ParseDelimitedTable(text, delim, table) || table.size() < 2) {
        error = L"Couldn't read rows from the MusicBee CSV/TSV export.";
        return false;
    }
    int colPath = -1, colTitle = -1, colArtist = -1, colAlbumArtist = -1, colAlbum = -1;
    int colPlays = -1, colSkips = -1, colRating = -1, colAdded = -1, colLast = -1, colPlaylist = -1;
    for (size_t c = 0; c < table[0].size(); ++c) {
        std::string h = LowerAscii(TrimAscii(table[0][c]));
        if (h == "path" || h == "file" || h == "filename" || h == "file path" || h == "location")
            colPath = (int)c;
        else if (h == "title" || h == "track name" || h == "name")
            colTitle = (int)c;
        else if (h == "artist" || h == "artist name(s)")
            colArtist = (int)c;
        else if (h == "album artist")
            colAlbumArtist = (int)c;
        else if (h == "album" || h == "album name")
            colAlbum = (int)c;
        else if (h == "play count" || h == "plays" || h == "playcount" || h == "play_count")
            colPlays = (int)c;
        else if (h == "skip count" || h == "skips" || h == "skipcount" || h == "skip_count")
            colSkips = (int)c;
        else if (h == "rating")
            colRating = (int)c;
        else if (h == "date added" || h == "added")
            colAdded = (int)c;
        else if (h == "last played" || h == "date last played" || h == "lastplayed" || h == "last_played")
            colLast = (int)c;
        else if (h == "playlist" || h == "playlist name")
            colPlaylist = (int)c;
    }
    if (colPath < 0 && colTitle < 0) {
        error = L"The CSV/TSV file has no Path or Title column.";
        return false;
    }
    if (colPlays < 0 && colSkips < 0 && colRating < 0 && colAdded < 0 && colLast < 0 && colPlaylist < 0) {
        error = L"The CSV/TSV file has no playback statistics or playlist columns.";
        return false;
    }
    std::map<std::string, size_t> playlistIndex;
    for (size_t r = 1; r < table.size(); ++r) {
        const auto& row = table[r];
        Statistics st;
        if (colPath >= 0) {
            std::string p = TrimAscii(row[colPath]);
            if (p.starts_with("file://"))
                st.path = DecodeFileUri(p);
            else
                st.path = FromUtf8(p);
        }
        if (colTitle >= 0) st.title = TrimAscii(row[colTitle]);
        if (colArtist >= 0) st.artist = TrimAscii(row[colArtist]);
        if (st.artist.empty() && colAlbumArtist >= 0) st.artist = TrimAscii(row[colAlbumArtist]);
        if (colAlbum >= 0) st.album = TrimAscii(row[colAlbum]);
        if (colPlays >= 0) {
            uint64_t n = 0;
            std::string v = TrimAscii(row[colPlays]);
            if (!v.empty() && ParseUint64(v, n) && n <= UINT32_MAX) {
                st.count = (uint32_t)n;
                st.hasCount = true;
            }
        }
        if (colSkips >= 0) {
            uint64_t n = 0;
            std::string v = TrimAscii(row[colSkips]);
            if (!v.empty() && ParseUint64(v, n) && n <= UINT32_MAX) {
                st.skips = (uint32_t)n;
                st.hasSkips = true;
            }
        }
        if (colRating >= 0) {
            uint8_t rating = 0;
            if (ParseRatingString(row[colRating], rating)) {
                st.rating = rating;
                st.hasRating = true;
            }
        }
        if (colAdded >= 0) ParseIsoDate(row[colAdded], st.added);
        if (colLast >= 0) ParseIsoDate(row[colLast], st.last);
        if (st.path.empty() && st.title.empty()) {
            ++out.unlocated;
            continue;
        }
        if (colPlaylist >= 0) {
            std::string plName = TrimAscii(row[colPlaylist]);
            if (!plName.empty()) {
                auto [it, inserted] = playlistIndex.emplace(plName, out.playlists.size());
                if (inserted) out.playlists.push_back({plName, {}});
                out.playlists[it->second].tracks.push_back(st);
            }
        }
        out.statistics.push_back(std::move(st));
    }
    return true;
}

bool ReadXspfPlaylist(std::string_view xml, const std::string& fallbackName, ImportedPlaylist& out,
                      std::wstring& error) {
    out = {};
    out.name = fallbackName;
    xml = StripBom(xml);
    if (xml.size() > kMaxImportBytes || xml.find('\0') != xml.npos) {
        error = L"The XSPF playlist file is invalid.";
        return false;
    }
    XmlCursor cur{xml};
    std::string_view tag;
    bool isEnd = false, isEmpty = false;
    std::string text;
    if (!cur.NextTag(tag, isEnd, isEmpty, text) || isEnd || tag != "playlist" || isEmpty) {
        error = L"This file is not an XSPF playlist.";
        return false;
    }
    bool inTrackList = false, inTrack = false;
    Statistics current;
    while (cur.NextTag(tag, isEnd, isEmpty, text)) {
        if (isEnd) {
            if (tag == "playlist") break;
            if (tag == "trackList") inTrackList = false;
            if (tag == "track" && inTrack) {
                inTrack = false;
                if (!current.path.empty() || !current.title.empty()) out.tracks.push_back(std::move(current));
            }
            continue;
        }
        if (!inTrackList) {
            if (tag == "title") {
                std::string t;
                if (cur.ReadLeafText("title", isEmpty, t) && !t.empty()) out.name = std::move(t);
            } else if (tag == "trackList" && !isEmpty) {
                inTrackList = true;
            } else {
                cur.SkipElement(isEmpty);
            }
        } else if (!inTrack) {
            if (tag == "track" && !isEmpty) {
                inTrack = true;
                current = {};
            } else {
                cur.SkipElement(isEmpty);
            }
        } else {
            std::string val;
            if (!cur.ReadLeafText(tag, isEmpty, val)) break;
            if (tag == "location")
                current.path = val.starts_with("file://") ? DecodeFileUri(val) : FromUtf8(val);
            else if (tag == "title")
                current.title = std::move(val);
            else if (tag == "creator")
                current.artist = std::move(val);
            else if (tag == "album")
                current.album = std::move(val);
            else if (tag == "duration") {
                uint64_t ms = 0;
                if (ParseUint64(val, ms) && ms <= UINT32_MAX) current.durationMs = (uint32_t)ms;
            }
        }
        if (cur.error) break;
    }
    if (cur.error) {
        error = L"The XSPF playlist file is malformed.";
        return false;
    }
    if (out.tracks.empty()) {
        error = L"The XSPF playlist contains no tracks.";
        return false;
    }
    return true;
}

bool ReadImportBytes(std::string_view bytes, const std::string& fallbackName, ImportBundle& out,
                     ImportFormat& format, std::wstring& error) {
    out = {};
    format = ImportFormat::Unknown;
    std::string_view head = StripBom(bytes);
    while (!head.empty() && (head.front() == ' ' || head.front() == '\t' || head.front() == '\r' || head.front() == '\n'))
        head.remove_prefix(1);
    std::string_view prefix = head.substr(0, std::min<size_t>(head.size(), 1024));
    if (prefix.find("<plist") != prefix.npos) {
        format = ImportFormat::ItunesXml;
        return ReadItunesLibrary(bytes, out, error);
    }
    if (prefix.find("<playlist") != prefix.npos) {
        format = ImportFormat::Xspf;
        ImportedPlaylist pl;
        if (!ReadXspfPlaylist(bytes, fallbackName, pl, error)) return false;
        out.playlists.push_back(std::move(pl));
        return true;
    }
    if (prefix.find("<PlaybackStatistics") != prefix.npos) {
        format = ImportFormat::FoobarXml;
        error = L"foobar2000 PlaybackStatistics XML should be opened with ReadFoobarStatistics.";
        return false;
    }
    if (prefix.starts_with("path,play_count,skip_count,rating,added,first_played,last_played")) {
        format = ImportFormat::CordinalCsv;
        return ReadStatisticsCsv(bytes, out.statistics, error);
    }
    format = ImportFormat::MusicBeeCsv;
    return ReadMusicBeeCsv(bytes, out, error);
}

}  // namespace migration
