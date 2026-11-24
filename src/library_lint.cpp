#include "library_lint.h"

#include "metadata_fetch.h"
#include "tags.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace lint {
namespace {

uint32_t NextUtf8(std::string_view s, size_t& i) {
    const uint8_t b0 = (uint8_t)s[i];
    if (b0 < 0x80) {
        ++i;
        return b0;
    }
    if ((b0 & 0xE0) == 0xC0 && i + 1 < s.size()) {
        const uint8_t b1 = (uint8_t)s[i + 1];
        if ((b1 & 0xC0) == 0x80) {
            uint32_t cp = ((b0 & 0x1Fu) << 6) | (b1 & 0x3Fu);
            i += 2;
            return cp >= 0x80 ? cp : 0xFFFD;
        }
    } else if ((b0 & 0xF0) == 0xE0 && i + 2 < s.size()) {
        const uint8_t b1 = (uint8_t)s[i + 1], b2 = (uint8_t)s[i + 2];
        if ((b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80) {
            uint32_t cp = ((b0 & 0x0Fu) << 12) | ((b1 & 0x3Fu) << 6) | (b2 & 0x3Fu);
            i += 3;
            return (cp >= 0x800 && (cp < 0xD800 || cp > 0xDFFF)) ? cp : 0xFFFD;
        }
    } else if ((b0 & 0xF8) == 0xF0 && i + 3 < s.size()) {
        const uint8_t b1 = (uint8_t)s[i + 1], b2 = (uint8_t)s[i + 2], b3 = (uint8_t)s[i + 3];
        if ((b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80 && (b3 & 0xC0) == 0x80) {
            uint32_t cp = ((b0 & 0x07u) << 18) | ((b1 & 0x3Fu) << 12) | ((b2 & 0x3Fu) << 6) | (b3 & 0x3Fu);
            i += 4;
            return (cp >= 0x10000 && cp <= 0x10FFFF) ? cp : 0xFFFD;
        }
    }
    ++i;
    return 0xFFFD;
}

void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back((char)cp);
    } else if (cp < 0x800) {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        out.push_back((char)(0xF0 | (cp >> 18)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

bool IsValidUtf8(std::string_view s) {
    size_t i = 0;
    while (i < s.size())
        if (NextUtf8(s, i) == 0xFFFD) return false;
    return true;
}

bool HasNonAscii(std::string_view s) {
    for (unsigned char c : s)
        if (c >= 0x80) return true;
    return false;
}

std::string FoldSimple(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') ++a;
    while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
    std::string out;
    out.reserve(b - a);
    for (size_t i = a; i < b; ++i) {
        unsigned char c = (unsigned char)s[i];
        out.push_back((c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c);
    }
    return out;
}

const char* FoldLatinCp(uint32_t cp) {
    if (cp >= 'A' && cp <= 'Z') {
        static char buf[2] = {};
        buf[0] = (char)(cp + 32);
        return buf;
    }
    if ((cp >= 0xC0 && cp <= 0xC5) || (cp >= 0xE0 && cp <= 0xE5) || (cp >= 0x100 && cp <= 0x105)) return "a";
    if (cp == 0xC6 || cp == 0xE6) return "ae";
    if (cp == 0xC7 || cp == 0xE7 || (cp >= 0x106 && cp <= 0x10D)) return "c";
    if (cp == 0xD0 || cp == 0xF0 || (cp >= 0x10E && cp <= 0x111)) return "d";
    if ((cp >= 0xC8 && cp <= 0xCB) || (cp >= 0xE8 && cp <= 0xEB) || (cp >= 0x112 && cp <= 0x11B)) return "e";
    if (cp >= 0x11C && cp <= 0x123) return "g";
    if (cp >= 0x124 && cp <= 0x127) return "h";
    if ((cp >= 0xCC && cp <= 0xCF) || (cp >= 0xEC && cp <= 0xEF) || (cp >= 0x128 && cp <= 0x131)) return "i";
    if (cp == 0x134 || cp == 0x135) return "j";
    if (cp == 0x136 || cp == 0x137) return "k";
    if (cp >= 0x139 && cp <= 0x142) return "l";
    if (cp == 0xD1 || cp == 0xF1 || (cp >= 0x143 && cp <= 0x148)) return "n";
    if ((cp >= 0xD2 && cp <= 0xD6) || cp == 0xD8 || (cp >= 0xF2 && cp <= 0xF6) || cp == 0xF8 ||
        (cp >= 0x14C && cp <= 0x151))
        return "o";
    if (cp == 0x152 || cp == 0x153) return "oe";
    if (cp >= 0x154 && cp <= 0x159) return "r";
    if (cp == 0xDF) return "ss";
    if (cp >= 0x15A && cp <= 0x161) return "s";
    if (cp == 0xDE || cp == 0xFE || (cp >= 0x162 && cp <= 0x167)) return "t";
    if ((cp >= 0xD9 && cp <= 0xDC) || (cp >= 0xF9 && cp <= 0xFC) || (cp >= 0x168 && cp <= 0x173)) return "u";
    if (cp == 0x174 || cp == 0x175) return "w";
    if (cp == 0xDD || cp == 0xFD || cp == 0xFF || (cp >= 0x176 && cp <= 0x178)) return "y";
    if (cp >= 0x179 && cp <= 0x17E) return "z";
    if (cp == 0x2018 || cp == 0x2019) return "'";
    if (cp == 0x201C || cp == 0x201D) return "\"";
    if (cp == 0x2013 || cp == 0x2014) return "-";
    return nullptr;
}

bool HasMixedCase(std::string_view s) {
    bool upper = false, lower = false;
    for (unsigned char c : s) {
        if (c >= 'A' && c <= 'Z') upper = true;
        else if (c >= 'a' && c <= 'z') lower = true;
    }
    return upper && lower;
}

int ReverseWin1252(uint32_t cp) {
    if (cp <= 0xFF) return (int)cp;
    struct Entry {
        uint32_t u;
        uint8_t b;
    };
    static constexpr Entry kMap[] = {
        {0x20ACu, 0x80u}, {0x201Au, 0x82u}, {0x0192u, 0x83u}, {0x201Eu, 0x84u}, {0x2026u, 0x85u}, {0x2020u, 0x86u},
        {0x2021u, 0x87u}, {0x02C6u, 0x88u}, {0x2030u, 0x89u}, {0x0160u, 0x8Au}, {0x2039u, 0x8Bu}, {0x0152u, 0x8Cu},
        {0x017Du, 0x8Eu}, {0x2018u, 0x91u}, {0x2019u, 0x92u}, {0x201Cu, 0x93u}, {0x201Du, 0x94u}, {0x2022u, 0x95u},
        {0x2013u, 0x96u}, {0x2014u, 0x97u}, {0x02DCu, 0x98u}, {0x2122u, 0x99u}, {0x0161u, 0x9Au}, {0x203Au, 0x9Bu},
        {0x0153u, 0x9Cu}, {0x017Eu, 0x9Eu}, {0x0178u, 0x9Fu},
    };
    for (const auto& e : kMap)
        if (cp == e.u) return e.b;
    return -1;
}

bool RecoverSingleByteStream(std::string_view utf8, std::string& rawBytes) {
    rawBytes.clear();
    rawBytes.reserve(utf8.size());
    size_t i = 0;
    while (i < utf8.size()) {
        uint32_t cp = NextUtf8(utf8, i);
        if (cp == 0xFFFD) return false;
        int b = ReverseWin1252(cp);
        if (b < 0) return false;
        rawBytes.push_back((char)(uint8_t)b);
    }
    return true;
}

constexpr std::array<uint16_t, 128> kCp1251 = {{
    0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021, 0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F,
    0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x0000, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F,
    0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7, 0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407,
    0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7, 0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457,
    0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417, 0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E, 0x041F,
    0x0420, 0x0421, 0x0422, 0x0423, 0x0424, 0x0425, 0x0426, 0x0427, 0x0428, 0x0429, 0x042A, 0x042B, 0x042C, 0x042D, 0x042E, 0x042F,
    0x0430, 0x0431, 0x0432, 0x0433, 0x0434, 0x0435, 0x0436, 0x0437, 0x0438, 0x0439, 0x043A, 0x043B, 0x043C, 0x043D, 0x043E, 0x043F,
    0x0440, 0x0441, 0x0442, 0x0443, 0x0444, 0x0445, 0x0446, 0x0447, 0x0448, 0x0449, 0x044A, 0x044B, 0x044C, 0x044D, 0x044E, 0x044F,
}};

bool DecodeCp1251(std::string_view raw, std::string& utf8Out) {
    utf8Out.clear();
    utf8Out.reserve(raw.size() * 2);
    for (unsigned char b : raw) {
        if (b < 0x80) utf8Out.push_back((char)b);
        else {
            uint16_t cp = kCp1251[b - 0x80];
            if (!cp) return false;
            AppendUtf8(utf8Out, cp);
        }
    }
    return true;
}

bool DecodeShiftJis(std::string_view raw, std::string& utf8Out, bool& hasJapanese) {
    utf8Out.clear();
    hasJapanese = false;
#ifdef _WIN32
    int wn = MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, raw.data(), (int)raw.size(), nullptr, 0);
    if (wn <= 0) return false;
    std::wstring wide((size_t)wn, L'\0');
    if (MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, raw.data(), (int)raw.size(), wide.data(), wn) <= 0) return false;
    for (wchar_t wc : wide) {
        uint32_t cp = (uint16_t)wc;
        if ((cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xFF66 && cp <= 0xFF9F))
            hasJapanese = true;
        AppendUtf8(utf8Out, cp);
    }
    return true;
#else
    for (size_t i = 0; i < raw.size();) {
        uint8_t b0 = (uint8_t)raw[i];
        if (b0 < 0x80) {
            utf8Out.push_back((char)b0);
            ++i;
            continue;
        }
        if (b0 >= 0xA1 && b0 <= 0xDF) {
            AppendUtf8(utf8Out, 0xFF61u + (b0 - 0xA1));
            hasJapanese = true;
            ++i;
            continue;
        }
        if (((b0 >= 0x81 && b0 <= 0x9F) || (b0 >= 0xE0 && b0 <= 0xEF)) && i + 1 < raw.size()) {
            uint8_t b1 = (uint8_t)raw[i + 1];
            if ((b1 >= 0x40 && b1 <= 0x7E) || (b1 >= 0x80 && b1 <= 0xFC)) {
                i += 2;
                if (b0 == 0x82 && b1 >= 0x9F && b1 <= 0xF1) {
                    AppendUtf8(utf8Out, 0x3041u + (b1 - 0x9F));
                    hasJapanese = true;
                    continue;
                }
                if (b0 == 0x83 && b1 >= 0x40 && b1 <= 0x96 && b1 != 0x7F) {
                    uint32_t off = b1 > 0x7F ? (uint32_t)(b1 - 0x41) : (uint32_t)(b1 - 0x40);
                    AppendUtf8(utf8Out, 0x30A1u + off);
                    hasJapanese = true;
                    continue;
                }
                if (b0 == 0x81 && b1 == 0x40) { AppendUtf8(utf8Out, 0x3000); continue; }
                if (b0 == 0x81 && b1 == 0x5B) { AppendUtf8(utf8Out, 0x30FC); hasJapanese = true; continue; }
                return false;
            }
        }
        return false;
    }
    return true;
#endif
}

bool LooksLikeShiftJisBytes(std::string_view raw) {
    int pairs = 0;
    bool hasC1Lead = false;
    for (size_t i = 0; i < raw.size();) {
        uint8_t b0 = (uint8_t)raw[i];
        if (b0 < 0x80) { ++i; continue; }
        if ((b0 >= 0x81 && b0 <= 0x9F) || (b0 >= 0xE0 && b0 <= 0xEF)) {
            if (i + 1 >= raw.size()) return false;
            uint8_t b1 = (uint8_t)raw[i + 1];
            if (!((b1 >= 0x40 && b1 <= 0x7E) || (b1 >= 0x80 && b1 <= 0xFC))) return false;
            if (b0 <= 0x9F) hasC1Lead = true;
            ++pairs;
            i += 2;
            continue;
        }
        if (b0 >= 0xA1 && b0 <= 0xDF) { ++i; continue; }
        return false;
    }
    return pairs >= 1 && (hasC1Lead || pairs >= 2);
}

bool LooksLikeCp1251Bytes(std::string_view raw) {
    int run = 0, maxRun = 0, cyrillic = 0, letters = 0;
    for (unsigned char b : raw) {
        if (b >= 0xC0 || b == 0xA8 || b == 0xB8) {
            ++run;
            maxRun = std::max(maxRun, run);
            ++cyrillic;
            ++letters;
        } else {
            run = 0;
            if ((b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z')) ++letters;
        }
    }
    return maxRun >= 3 || (cyrillic >= 4 && letters > 0 && cyrillic * 2 >= letters);
}

uint64_t HashBytes(std::string_view s) {
    uint64_t h = 14695981039346656037ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

std::wstring FormatBytes(size_t bytes) {
    if (bytes >= (1u << 20)) {
        wchar_t buf[32];
        swprintf(buf, 32, L"%.1f MB", (double)bytes / (1024.0 * 1024.0));
        return buf;
    }
    return std::to_wstring((bytes + 1023) / 1024) + L" KB";
}

void MergeTagEdit(TagEdit& dst, const TagEdit& src) {
    if (src.title) dst.title = src.title;
    if (src.artist) dst.artist = src.artist;
    if (src.album) dst.album = src.album;
    if (src.albumArtist) dst.albumArtist = src.albumArtist;
    if (src.genre) dst.genre = src.genre;
    if (src.comment) dst.comment = src.comment;
    if (src.compilation) dst.compilation = src.compilation;
    if (src.cover) dst.cover = src.cover;
}

bool AddUniqueHash(std::vector<uint64_t>& seen, uint64_t h) {
    if (std::find(seen.begin(), seen.end(), h) != seen.end()) return false;
    seen.push_back(h);
    return true;
}

}  // namespace

std::string CanonicalArtistKey(std::string_view utf8) {
    std::string folded;
    folded.reserve(utf8.size());
    bool prevSpace = true;
    for (size_t i = 0; i < utf8.size();) {
        uint32_t cp = NextUtf8(utf8, i);
        if (cp == 0xFFFD) continue;
        if (cp <= ' ') {
            if (!prevSpace) { folded.push_back(' '); prevSpace = true; }
            continue;
        }
        prevSpace = false;
        if (const char* rep = FoldLatinCp(cp)) folded += rep;
        else AppendUtf8(folded, cp);
    }
    while (!folded.empty() && folded.back() == ' ') folded.pop_back();
    if (folded.size() > 4 && folded.compare(0, 4, "the ") == 0) folded.erase(0, 4);
    return folded;
}

bool DetectMojibake(std::string_view utf8, std::string& repairedUtf8, std::wstring& encodingName) {
    if (utf8.empty() || !HasNonAscii(utf8)) return false;
    std::string raw;
    if (!RecoverSingleByteStream(utf8, raw) || !HasNonAscii(raw)) return false;

    if (IsValidUtf8(raw) && raw != utf8) {
        repairedUtf8 = std::move(raw);
        encodingName = L"UTF-8 as Latin-1";
        return true;
    }
    if (LooksLikeShiftJisBytes(raw)) {
        std::string sjis;
        bool hasJapanese = false;
        if (DecodeShiftJis(raw, sjis, hasJapanese) && hasJapanese && sjis != utf8) {
            repairedUtf8 = std::move(sjis);
            encodingName = L"Shift-JIS";
            return true;
        }
    }
    if (LooksLikeCp1251Bytes(raw)) {
        std::string cp1251;
        if (DecodeCp1251(raw, cp1251) && cp1251 != utf8) {
            repairedUtf8 = std::move(cp1251);
            encodingName = L"CP1251";
            return true;
        }
    }
    return false;
}

void FindSplitAlbums(const Library& lib, std::vector<LintFinding>& out) {
    struct Entry {
        uint32_t dir;
        std::string albumFold;
        TrackId id;
    };
    std::vector<Entry> entries;
    for (TrackId id = 0; id < (TrackId)lib.Count(); ++id) {
        const Track& t = lib[id];
        if (!t.InLibrary() || !t.album) continue;
        std::string alb = FoldSimple(lib.View(t.album));
        if (!alb.empty()) entries.push_back({t.dir, std::move(alb), id});
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        return a.dir != b.dir ? a.dir < b.dir : (a.albumFold != b.albumFold ? a.albumFold < b.albumFold : a.id < b.id);
    });

    for (size_t i = 0; i < entries.size();) {
        size_t j = i + 1;
        while (j < entries.size() && entries[j].dir == entries[i].dir && entries[j].albumFold == entries[i].albumFold)
            ++j;
        if (j - i >= 2) {
            struct NameCount {
                std::string key;
                std::string display;
                size_t count = 0;
            };
            auto bump = [](std::vector<NameCount>& v, std::string k, std::string disp) {
                for (auto& e : v)
                    if (e.key == k) { ++e.count; return; }
                v.push_back({std::move(k), std::move(disp), 1});
            };
            std::vector<NameCount> effs, aas, arts;
            for (size_t k = i; k < j; ++k) {
                const Track& t = lib[entries[k].id];
                std::string_view aa = t.albumArtist ? lib.View(t.albumArtist) : std::string_view{};
                std::string_view ar = t.artist ? lib.View(t.artist) : std::string_view{};
                std::string_view eff = !aa.empty() ? aa : ar;
                if (!eff.empty()) bump(effs, FoldSimple(eff), std::string(eff));
                if (!aa.empty()) bump(aas, std::string(aa), std::string(aa));
                if (!ar.empty()) bump(arts, std::string(ar), std::string(ar));
            }
            if (effs.size() >= 2) {
                auto pickTop = [](const std::vector<NameCount>& v) -> std::pair<std::string, size_t> {
                    const NameCount* best = nullptr;
                    for (const auto& e : v)
                        if (!best || e.count > best->count || (e.count == best->count && e.display.size() < best->display.size()))
                            best = &e;
                    return best ? std::pair<std::string, size_t>{best->display, best->count} : std::pair<std::string, size_t>{{}, 0};
                };
                std::string target;
                bool compilation = false;
                if (!aas.empty()) target = pickTop(aas).first;
                else {
                    auto [topArt, topCount] = pickTop(arts);
                    if (topCount >= 2) target = topArt;
                    else { target = "Various Artists"; compilation = true; }
                }

                std::vector<LintFix> fixes;
                for (size_t k = i; k < j; ++k) {
                    TrackId id = entries[k].id;
                    const Track& t = lib[id];
                    if (!CanWriteTags(lib.Path(id))) continue;
                    std::string_view curAa = t.albumArtist ? lib.View(t.albumArtist) : std::string_view{};
                    bool needAa = curAa != target;
                    bool needComp = compilation && !(t.flags & Track::kCompilation);
                    if (needAa || needComp) {
                        TagEdit edit;
                        if (needAa) edit.albumArtist = target;
                        if (needComp) edit.compilation = true;
                        fixes.push_back({id, std::move(edit)});
                    }
                }
                if (!fixes.empty()) {
                    LintFinding f;
                    f.kind = LintKind::SplitAlbum;
                    f.summary = L"Split album \"" + lib.Str(lib[entries[i].id].album) + L"\" (" +
                                std::to_wstring(effs.size()) + L" album artists)";
                    for (size_t m = 0; m < effs.size(); ++m) {
                        if (m) f.before += L", ";
                        if (m >= 3) { f.before += L"..."; break; }
                        f.before += FromUtf8(effs[m].display);
                    }
                    f.after = L"Set Album Artist \x2192 \"" + FromUtf8(target) + L"\" (" + std::to_wstring(fixes.size()) +
                              (fixes.size() == 1 ? L" track)" : L" tracks)");
                    f.fixes = std::move(fixes);
                    out.push_back(std::move(f));
                }
            }
        }
        i = j;
    }
}

void FindNearDuplicateArtists(const Library& lib, std::vector<LintFinding>& out) {
    std::vector<StringPool::Handle> handles;
    for (TrackId id = 0; id < (TrackId)lib.Count(); ++id) {
        const Track& t = lib[id];
        if (!t.InLibrary()) continue;
        if (t.artist) handles.push_back(t.artist);
        if (t.albumArtist && t.albumArtist != t.artist) handles.push_back(t.albumArtist);
    }
    std::sort(handles.begin(), handles.end());

    struct Variant {
        std::string key;
        StringPool::Handle handle;
        std::string name;
        size_t count;
    };
    std::vector<Variant> variants;
    for (size_t i = 0; i < handles.size();) {
        size_t j = i + 1;
        while (j < handles.size() && handles[j] == handles[i]) ++j;
        std::string_view sv = lib.View(handles[i]);
        std::string key = CanonicalArtistKey(sv);
        if (!key.empty()) variants.push_back({std::move(key), handles[i], std::string(sv), j - i});
        i = j;
    }
    std::sort(variants.begin(), variants.end(), [](const Variant& a, const Variant& b) {
        if (a.key != b.key) return a.key < b.key;
        if (a.count != b.count) return a.count > b.count;
        bool na = HasNonAscii(a.name), nb = HasNonAscii(b.name);
        if (na != nb) return na > nb;
        bool ma = HasMixedCase(a.name), mb = HasMixedCase(b.name);
        if (ma != mb) return ma > mb;
        return a.name.size() > b.name.size();
    });

    for (size_t i = 0; i < variants.size();) {
        size_t j = i + 1;
        while (j < variants.size() && variants[j].key == variants[i].key) ++j;
        if (j - i >= 2) {
            const std::string& canonical = variants[i].name;
            auto isBad = [&](StringPool::Handle h) {
                for (size_t k = i + 1; k < j; ++k)
                    if (variants[k].handle == h) return true;
                return false;
            };
            std::vector<LintFix> fixes;
            for (TrackId id = 0; id < (TrackId)lib.Count(); ++id) {
                const Track& t = lib[id];
                if (!t.InLibrary() || !CanWriteTags(lib.Path(id))) continue;
                TagEdit edit;
                if (t.artist && isBad(t.artist)) edit.artist = canonical;
                if (t.albumArtist && isBad(t.albumArtist)) edit.albumArtist = canonical;
                if (!edit.Empty()) fixes.push_back({id, std::move(edit)});
            }
            if (!fixes.empty()) {
                LintFinding f;
                f.kind = LintKind::NearDuplicateArtist;
                f.summary = L"Near-duplicate artist \"" + FromUtf8(canonical) + L"\"";
                for (size_t k = i + 1; k < j; ++k) {
                    if (k > i + 1) f.before += L", ";
                    f.before += FromUtf8(variants[k].name) + L" (" + std::to_wstring(variants[k].count) + L")";
                }
                f.after = L"Unify \x2192 \"" + FromUtf8(canonical) + L"\" (" + std::to_wstring(fixes.size()) +
                          (fixes.size() == 1 ? L" track)" : L" tracks)");
                f.fixes = std::move(fixes);
                out.push_back(std::move(f));
            }
        }
        i = j;
    }
}

void FindMojibakeIssues(const Library& lib, std::vector<LintFinding>& out) {
    for (TrackId id = 0; id < (TrackId)lib.Count(); ++id) {
        const Track& t = lib[id];
        if (!t.InLibrary() || !CanWriteTags(lib.Path(id))) continue;

        TagEdit edit;
        std::wstring encSummary, beforeStr, afterStr;
        auto checkField = [&](StringPool::Handle h, std::optional<std::string>& dst, const wchar_t* label) {
            if (!h) return;
            std::string_view val = lib.View(h);
            std::string fixed;
            std::wstring enc;
            if (DetectMojibake(val, fixed, enc)) {
                dst = fixed;
                if (encSummary.empty()) encSummary = enc;
                if (!beforeStr.empty()) { beforeStr += L"; "; afterStr += L"; "; }
                beforeStr += std::wstring(label) + L": " + FromUtf8(val);
                afterStr += std::wstring(label) + L": " + FromUtf8(fixed);
            }
        };

        checkField(t.title, edit.title, L"Title");
        checkField(t.artist, edit.artist, L"Artist");
        checkField(t.album, edit.album, L"Album");
        checkField(t.albumArtist, edit.albumArtist, L"Album Artist");
        checkField(t.genre, edit.genre, L"Genre");
        checkField(t.comment, edit.comment, L"Comment");

        if (!edit.Empty()) {
            LintFinding f;
            f.kind = LintKind::Mojibake;
            f.summary = L"Garbled tags (" + encSummary + L") in " + FileNameOf(lib.Path(id));
            f.before = std::move(beforeStr);
            f.after = std::move(afterStr);
            f.fixes.push_back({id, std::move(edit)});
            out.push_back(std::move(f));
        }
    }
}

void FindMetadataIssues(const Library& lib, std::vector<LintFinding>& out) {
    FindSplitAlbums(lib, out);
    FindNearDuplicateArtists(lib, out);
    FindMojibakeIssues(lib, out);
}

std::vector<CoverTrackInfo> SnapshotCoverTracks(const Library& lib) {
    std::vector<CoverTrackInfo> out;
    out.reserve(lib.Count());
    for (TrackId id = 0; id < (TrackId)lib.Count(); ++id) {
        const Track& t = lib[id];
        if (!t.InLibrary()) continue;
        CoverTrackInfo info;
        info.id = id;
        info.dir = t.dir;
        info.path = lib.Path(id);
        info.writable = CanWriteTags(info.path);
        if (t.album) {
            std::string_view alb = lib.View(t.album);
            info.albumFold = FoldSimple(alb);
            info.album = FromUtf8(alb);
        }
        out.push_back(std::move(info));
    }
    return out;
}

void AuditCoverTracks(const std::vector<CoverTrackInfo>& inLib, const CoverReencoder& reencode,
                      std::vector<LintFinding>& out, const std::atomic<bool>* cancel,
                      const std::function<void(size_t done, size_t total)>& progress) {
    constexpr size_t kBloatedPngBytes = 1536u << 10;
    constexpr int kMinCoverDim = 500;

    struct TrackCover {
        size_t infoIdx = 0;
        bool hasArt = false;
        int w = 0, h = 0;
        uint64_t hash = 0;
    };
    struct BloatedEntry {
        uint64_t hash = 0;
        int w = 0, h = 0;
        size_t origSize = 0;
        std::string jpeg;
        std::vector<TrackId> ids;
    };

    std::vector<TrackCover> covers;
    covers.reserve(inLib.size());
    std::vector<BloatedEntry> bloated;
    std::vector<uint64_t> flaggedLowRes, flaggedNonSquare;

    for (size_t idx = 0; idx < inLib.size(); ++idx) {
        if (cancel && cancel->load()) return;
        const CoverTrackInfo& info = inLib[idx];
        TrackCover tc{idx};

        std::string bytes;
        if (ReadEmbeddedArt(info.path, bytes) && !bytes.empty()) {
            tc.hasArt = true;
            online::ImageType type = online::InspectImage(bytes, tc.w, tc.h);
            tc.hash = HashBytes(bytes);

            if (type == online::ImageType::Png && bytes.size() > kBloatedPngBytes && info.writable) {
                BloatedEntry* be = nullptr;
                for (auto& e : bloated)
                    if (e.hash == tc.hash) { be = &e; break; }
                if (!be) {
                    bloated.push_back({tc.hash, tc.w, tc.h, bytes.size(), {}, {}});
                    be = &bloated.back();
                    if (reencode) {
                        std::string jpeg;
                        if (reencode(bytes, jpeg) && !jpeg.empty() && jpeg.size() < bytes.size())
                            be->jpeg = std::move(jpeg);
                    }
                }
                be->ids.push_back(info.id);
            }
            if (tc.w > 0 && tc.h > 0 && std::min(tc.w, tc.h) < kMinCoverDim && AddUniqueHash(flaggedLowRes, tc.hash)) {
                LintFinding f;
                f.kind = LintKind::CoverArt;
                f.coverIssue = CoverIssue::LowRes;
                f.checked = false;
                f.fixable = false;
                std::wstring label = !info.album.empty() ? info.album : FileNameOf(info.path);
                f.summary = L"Low-res cover (" + std::to_wstring(tc.w) + L"\x00D7" + std::to_wstring(tc.h) +
                            L" px) on \"" + label + L"\"";
                f.before = std::to_wstring(tc.w) + L"\x00D7" + std::to_wstring(tc.h) + L" (< 500 px)";
                f.after = L"Replace with a higher-resolution cover";
                out.push_back(std::move(f));
            }
            int maxDim = std::max(tc.w, tc.h);
            if (tc.w > 0 && tc.h > 0 && std::abs(tc.w - tc.h) * 10 > maxDim && AddUniqueHash(flaggedNonSquare, tc.hash)) {
                LintFinding f;
                f.kind = LintKind::CoverArt;
                f.coverIssue = CoverIssue::NonSquare;
                f.checked = false;
                f.fixable = false;
                std::wstring label = !info.album.empty() ? info.album : FileNameOf(info.path);
                f.summary = L"Non-square cover (" + std::to_wstring(tc.w) + L"\x00D7" + std::to_wstring(tc.h) +
                            L" px) on \"" + label + L"\"";
                f.before = std::to_wstring(tc.w) + L"\x00D7" + std::to_wstring(tc.h);
                f.after = L"Replace with a square cover";
                out.push_back(std::move(f));
            }
        }
        covers.push_back(tc);
        if (progress && ((idx + 1) % 32 == 0 || idx + 1 == inLib.size())) progress(idx + 1, inLib.size());
    }

    for (const auto& be : bloated) {
        LintFinding f;
        f.kind = LintKind::CoverArt;
        f.coverIssue = CoverIssue::BloatedPng;
        f.summary = L"Bloated embedded PNG (" + FormatBytes(be.origSize) + L", " + std::to_wstring(be.w) + L"\x00D7" +
                    std::to_wstring(be.h) + L") on " + std::to_wstring(be.ids.size()) +
                    (be.ids.size() == 1 ? L" track" : L" tracks");
        f.before = FormatBytes(be.origSize) + L" PNG";
        if (!be.jpeg.empty()) {
            f.fixable = true;
            f.checked = true;
            f.replacementCover = be.jpeg;
            f.after = L"Re-encode to " + FormatBytes(be.jpeg.size()) + L" JPEG";
            for (TrackId id : be.ids) f.fixes.push_back({id, TagEdit{}, true});
        } else {
            f.fixable = false;
            f.checked = false;
            f.after = L"Convert to JPEG to reduce size";
        }
        out.push_back(std::move(f));
    }

    std::sort(covers.begin(), covers.end(), [&](const TrackCover& a, const TrackCover& b) {
        const auto &ia = inLib[a.infoIdx], &ib = inLib[b.infoIdx];
        return ia.dir != ib.dir ? ia.dir < ib.dir : ia.albumFold < ib.albumFold;
    });
    for (size_t i = 0; i < covers.size();) {
        size_t j = i + 1;
        while (j < covers.size() && inLib[covers[j].infoIdx].dir == inLib[covers[i].infoIdx].dir &&
               inLib[covers[j].infoIdx].albumFold == inLib[covers[i].infoIdx].albumFold)
            ++j;
        if (j - i >= 2 && !inLib[covers[i].infoIdx].albumFold.empty()) {
            std::vector<uint64_t> hashes;
            bool anyMissing = false;
            const TrackCover* best = nullptr;
            for (size_t k = i; k < j; ++k) {
                if (!covers[k].hasArt) { anyMissing = true; continue; }
                AddUniqueHash(hashes, covers[k].hash);
                if (!best || (int64_t)covers[k].w * covers[k].h > (int64_t)best->w * best->h) best = &covers[k];
            }
            if (best && (hashes.size() > 1 || anyMissing)) {
                std::string bestBytes;
                for (const auto& be : bloated)
                    if (be.hash == best->hash && !be.jpeg.empty()) { bestBytes = be.jpeg; break; }
                if (bestBytes.empty()) ReadEmbeddedArt(inLib[best->infoIdx].path, bestBytes);
                if (!bestBytes.empty()) {
                    std::vector<LintFix> fixes;
                    for (size_t k = i; k < j; ++k) {
                        const auto& info = inLib[covers[k].infoIdx];
                        if (info.writable && (!covers[k].hasArt || covers[k].hash != best->hash))
                            fixes.push_back({info.id, TagEdit{}, true});
                    }
                    if (!fixes.empty()) {
                        LintFinding f;
                        f.kind = LintKind::CoverArt;
                        f.coverIssue = CoverIssue::Inconsistent;
                        f.replacementCover = std::move(bestBytes);
                        f.summary = L"Inconsistent covers in \"" + inLib[covers[i].infoIdx].album + L"\"";
                        f.before = std::to_wstring(hashes.size()) + L" distinct cover(s)" +
                                   (anyMissing ? L" + missing" : L"");
                        f.after = L"Apply " + std::to_wstring(best->w) + L"\x00D7" + std::to_wstring(best->h) +
                                  L" cover to " + std::to_wstring(fixes.size()) +
                                  (fixes.size() == 1 ? L" track" : L" tracks");
                        f.fixes = std::move(fixes);
                        out.push_back(std::move(f));
                    }
                }
            }
        }
        i = j;
    }
}

void AuditCoverArt(const Library& lib, const CoverReencoder& reencode, std::vector<LintFinding>& out,
                   const std::atomic<bool>* cancel, const std::function<void(size_t done, size_t total)>& progress) {
    AuditCoverTracks(SnapshotCoverTracks(lib), reencode, out, cancel, progress);
}

std::vector<LintFinding> LintLibrary(const Library& lib, const CoverReencoder& reencode,
                                     const std::atomic<bool>* cancel) {
    std::vector<LintFinding> out;
    FindMetadataIssues(lib, out);
    if (!cancel || !cancel->load()) AuditCoverArt(lib, reencode, out, cancel);
    return out;
}

std::vector<std::pair<TrackId, TagEdit>> CollectCheckedEdits(const std::vector<LintFinding>& findings) {
    std::vector<std::pair<TrackId, TagEdit>> out;
    for (const auto& f : findings) {
        if (!f.checked || !f.fixable) continue;
        for (const auto& fix : f.fixes) {
            TagEdit e = fix.edit;
            if (fix.applyReplacementCover && !f.replacementCover.empty()) e.cover = f.replacementCover;
            if (e.Empty()) continue;
            bool found = false;
            for (auto& existing : out) {
                if (existing.first == fix.track) {
                    MergeTagEdit(existing.second, e);
                    found = true;
                    break;
                }
            }
            if (!found) out.emplace_back(fix.track, std::move(e));
        }
    }
    return out;
}

}  // namespace lint
