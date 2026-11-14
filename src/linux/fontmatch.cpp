// Font discovery and matching for the Linux front end (fontmatch.h): the sfnt reader, the
// directory index and its cache, the fontconfig configuration subset, and the matcher.
#include "fontmatch.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <fnmatch.h>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <unordered_map>

namespace lx::fonts {

namespace {

struct OrthRange {
    uint32_t packed;
    uint32_t First() const { return packed & 0x1FFFFF; }
    uint32_t Last() const { return First() + (packed >> 21); }
};
struct Orthography {
    const char* lang;
    const OrthRange* ranges;
    size_t count;
};
#include "font_orth.inc"
constexpr size_t kLangCount = sizeof(kOrthographies) / sizeof(kOrthographies[0]);
static_assert(kLangCount <= 64, "Face::langs holds one bit per language");

// ---- The sfnt reader ------------------------------------------------------------------------

// A bounds-checked view: every read past the end gives 0 and marks the view bad.
struct Bytes {
    const unsigned char* p = nullptr;
    size_t n = 0;
    bool Has(size_t at, size_t len) const { return at <= n && len <= n - at; }
    uint8_t U8(size_t at) const { return Has(at, 1) ? p[at] : 0; }
    uint16_t U16(size_t at) const { return Has(at, 2) ? (uint16_t)(p[at] << 8 | p[at + 1]) : 0; }
    int16_t S16(size_t at) const { return (int16_t)U16(at); }
    uint32_t U32(size_t at) const {
        return Has(at, 4) ? (uint32_t)p[at] << 24 | (uint32_t)p[at + 1] << 16 | (uint32_t)p[at + 2] << 8 | p[at + 3] : 0;
    }
    Bytes Sub(size_t at, size_t len) const {
        if (!Has(at, 0)) return {};
        return {p + at, std::min(len, n - at)};
    }
};

constexpr uint32_t Tag(const char (&t)[5]) { return (uint32_t)t[0] << 24 | (uint32_t)t[1] << 16 | (uint32_t)t[2] << 8 | (uint32_t)t[3]; }

struct Tables {
    Bytes file;
    std::map<uint32_t, Bytes> t;
    Bytes Get(uint32_t tag) const {
        auto it = t.find(tag);
        return it == t.end() ? Bytes{} : it->second;
    }
    bool Has(uint32_t tag) const { return t.count(tag) != 0; }
};

bool ReadTables(Bytes file, size_t offset, Tables* out) {
    uint32_t version = file.U32(offset);
    if (version != 0x00010000 && version != Tag("OTTO") && version != Tag("true")) return false;
    uint16_t count = file.U16(offset + 4);
    if (!file.Has(offset + 12, (size_t)count * 16)) return false;
    out->file = file;
    for (uint16_t i = 0; i < count; ++i) {
        size_t rec = offset + 12 + (size_t)i * 16;
        uint32_t tag = file.U32(rec), at = file.U32(rec + 8), len = file.U32(rec + 12);
        if (!file.Has(at, len)) continue;  // a table past the end is as good as absent
        out->t.emplace(tag, file.Sub(at, len));
    }
    return true;
}

void AppendUtf8(std::string& s, uint32_t c) {
    if (c < 0x80) s += (char)c;
    else if (c < 0x800) s += (char)(0xC0 | c >> 6), s += (char)(0x80 | (c & 0x3F));
    else if (c < 0x10000) s += (char)(0xE0 | c >> 12), s += (char)(0x80 | (c >> 6 & 0x3F)), s += (char)(0x80 | (c & 0x3F));
    else s += (char)(0xF0 | c >> 18), s += (char)(0x80 | (c >> 12 & 0x3F)), s += (char)(0x80 | (c >> 6 & 0x3F)), s += (char)(0x80 | (c & 0x3F));
}

// Mac Roman's upper half, for the old Macintosh name records.
const uint16_t kMacRoman[128] = {
    0xC4, 0xC5, 0xC7, 0xC9, 0xD1, 0xD6, 0xDC, 0xE1, 0xE0, 0xE2, 0xE4, 0xE3, 0xE5, 0xE7, 0xE9, 0xE8,
    0xEA, 0xEB, 0xED, 0xEC, 0xEE, 0xEF, 0xF1, 0xF3, 0xF2, 0xF4, 0xF6, 0xF5, 0xFA, 0xF9, 0xFB, 0xFC,
    0x2020, 0xB0, 0xA2, 0xA3, 0xA7, 0x2022, 0xB6, 0xDF, 0xAE, 0xA9, 0x2122, 0xB4, 0xA8, 0x2260, 0xC6, 0xD8,
    0x221E, 0xB1, 0x2264, 0x2265, 0xA5, 0xB5, 0x2202, 0x2211, 0x220F, 0x3C0, 0x222B, 0xAA, 0xBA, 0x3A9, 0xE6, 0xF8,
    0xBF, 0xA1, 0xAC, 0x221A, 0x192, 0x2248, 0x2206, 0xAB, 0xBB, 0x2026, 0xA0, 0xC0, 0xC3, 0xD5, 0x152, 0x153,
    0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0xF7, 0x25CA, 0xFF, 0x178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
    0x2021, 0xB7, 0x201A, 0x201E, 0x2030, 0xC2, 0xCA, 0xC1, 0xCB, 0xC8, 0xCD, 0xCE, 0xCF, 0xCC, 0xD3, 0xD4,
    0xF8FF, 0xD2, 0xDA, 0xDB, 0xD9, 0x131, 0x2C6, 0x2DC, 0xAF, 0x2D8, 0x2D9, 0x2DA, 0xB8, 0x2DD, 0x2DB, 0x2C7};

struct Name {
    uint16_t id;
    bool english;
    std::string text;
};

std::vector<Name> ReadNames(Bytes name) {
    std::vector<Name> out;
    uint16_t count = name.U16(2), strings = name.U16(4);
    for (uint16_t i = 0; i < count && name.Has(6 + (size_t)i * 12, 12); ++i) {
        size_t rec = 6 + (size_t)i * 12;
        uint16_t platform = name.U16(rec), encoding = name.U16(rec + 2), language = name.U16(rec + 4), id = name.U16(rec + 6);
        uint16_t len = name.U16(rec + 8), at = name.U16(rec + 10);
        if (id != 1 && id != 2 && id != 16 && id != 17 && id != 21 && id != 22) continue;
        size_t from = (size_t)strings + at;
        if (!name.Has(from, len)) continue;
        std::string text;
        if (platform == 0 || (platform == 3 && (encoding == 0 || encoding == 1 || encoding == 10))) {
            for (size_t k = 0; k + 1 < len; k += 2) {
                uint32_t c = name.U16(from + k);
                if (c >= 0xD800 && c < 0xDC00 && k + 3 < len) {
                    uint32_t lo = name.U16(from + k + 2);
                    if (lo >= 0xDC00 && lo < 0xE000) c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00), k += 2;
                    else c = 0xFFFD;
                } else if (c >= 0xD800 && c < 0xE000) c = 0xFFFD;
                if (c) AppendUtf8(text, c);
            }
        } else if (platform == 1 && encoding == 0) {
            for (size_t k = 0; k < len; ++k) {
                uint8_t c = name.U8(from + k);
                if (c) AppendUtf8(text, c < 0x80 ? c : kMacRoman[c - 0x80]);
            }
        } else {
            continue;
        }
        while (!text.empty() && text.back() == ' ') text.pop_back();
        while (!text.empty() && text.front() == ' ') text.erase(text.begin());
        if (text.empty()) continue;
        bool english = (platform == 3 && (language & 0x3FF) == 0x09) || (platform == 1 && language == 0) || platform == 0;
        out.push_back({id, english, std::move(text)});
    }
    return out;
}

void AddRange(std::vector<Range>& out, uint32_t first, uint32_t last) {
    if (first > last || first > 0x10FFFF) return;
    last = std::min<uint32_t>(last, 0x10FFFF);
    out.push_back({first, last});
}

// Sorted, merged ranges of the code points with a real glyph (not .notdef, not past numGlyphs).
std::vector<Range> Normalize(std::vector<Range> r) {
    std::sort(r.begin(), r.end(), [](const Range& a, const Range& b) { return a.first < b.first; });
    std::vector<Range> out;
    for (const Range& x : r) {
        if (!out.empty() && x.first <= out.back().last + 1) out.back().last = std::max(out.back().last, x.last);
        else out.push_back(x);
    }
    return out;
}

void ReadCmapFormat4(Bytes s, uint32_t glyphs, std::vector<Range>& out) {
    uint16_t segX2 = s.U16(6);
    size_t ends = 14, starts = 16 + segX2, deltas = 16 + 2 * (size_t)segX2, offsets = 16 + 3 * (size_t)segX2;
    if (!s.Has(offsets, segX2)) return;
    for (size_t i = 0; i < segX2 / 2; ++i) {
        uint32_t end = s.U16(ends + 2 * i), start = s.U16(starts + 2 * i);
        uint16_t delta = s.U16(deltas + 2 * i), rangeOffset = s.U16(offsets + 2 * i);
        if (start > end) continue;
        uint32_t runStart = 0;
        bool run = false;
        for (uint32_t c = start; c <= end; ++c) {
            uint32_t g;
            if (!rangeOffset) {
                g = (c + delta) & 0xFFFF;
            } else {
                size_t at = offsets + 2 * i + rangeOffset + 2 * (size_t)(c - start);
                g = s.Has(at, 2) ? s.U16(at) : 0;
                if (g) g = (g + delta) & 0xFFFF;
            }
            bool ok = g != 0 && g < glyphs && c != 0xFFFF;
            if (ok && !run) runStart = c, run = true;
            if (!ok && run) AddRange(out, runStart, c - 1), run = false;
        }
        if (run) AddRange(out, runStart, end);
    }
}

void ReadCmapGroups(Bytes s, bool constant, uint32_t glyphs, std::vector<Range>& out) {
    uint32_t groups = s.U32(12);
    if (!s.Has(16, 0)) return;
    groups = (uint32_t)std::min<size_t>(groups, (s.n - 16) / 12);
    for (uint32_t i = 0; i < groups; ++i) {
        size_t at = 16 + (size_t)i * 12;
        uint32_t first = s.U32(at), last = s.U32(at + 4), g = s.U32(at + 8);
        if (first > last || first > 0x10FFFF) continue;
        last = std::min<uint32_t>(last, 0x10FFFF);
        if (constant) {
            if (g && g < glyphs) AddRange(out, first, last);
            continue;
        }
        if (g >= glyphs) continue;
        if (g == 0) ++first, ++g;  // the group's first code point maps to .notdef
        if (first > last) continue;
        uint64_t room = (uint64_t)glyphs - g;  // code points whose glyph stays below numGlyphs
        uint64_t lastOk = std::min<uint64_t>(last, first + room - 1);
        AddRange(out, first, (uint32_t)lastOk);
    }
}

void ReadCmapSimple(Bytes s, int format, uint32_t glyphs, std::vector<Range>& out) {
    if (format == 0) {
        for (uint32_t c = 0; c < 256; ++c) {
            uint32_t g = s.U8(6 + c);
            if (g && g < glyphs) AddRange(out, c, c);
        }
    } else if (format == 6) {
        uint32_t first = s.U16(6), count = s.U16(8);
        for (uint32_t k = 0; k < count && s.Has(10 + 2 * (size_t)k, 2); ++k) {
            uint32_t g = s.U16(10 + 2 * k);
            if (g && g < glyphs) AddRange(out, first + k, first + k);
        }
    }
}

// The Unicode cmap FreeType would pick: a full-repertoire one first, then the BMP, then the
// symbol encoding (and its 0xF0xx code points), then an old Macintosh one.
bool ReadCmap(Bytes cmap, uint32_t glyphs, std::vector<Range>* out, bool* symbol) {
    uint16_t count = cmap.U16(2);
    int best = -1;
    size_t bestAt = 0;
    for (uint16_t i = 0; i < count && cmap.Has(4 + (size_t)i * 8, 8); ++i) {
        uint16_t platform = cmap.U16(4 + i * 8), encoding = cmap.U16(6 + i * 8);
        uint32_t at = cmap.U32(8 + i * 8);
        uint16_t format = cmap.U16(at);
        int rank = -1;
        if ((platform == 3 && encoding == 10) || (platform == 0 && (encoding == 4 || encoding == 6)))
            rank = (format == 12 || format == 13) ? 5 : -1;
        else if ((platform == 3 && encoding == 1) || (platform == 0 && encoding <= 3))
            rank = (format == 12 || format == 13) ? 5 : (format == 4 || format == 6) ? 4 : -1;
        else if (platform == 3 && encoding == 0)
            rank = (format == 4 || format == 6 || format == 12) ? 2 : -1;
        else if (platform == 1 && encoding == 0)
            rank = (format == 0 || format == 6) ? 1 : -1;
        if (rank > best && cmap.Has(at, 4)) best = rank, bestAt = at;
    }
    if (best < 0) return false;
    *symbol = best == 2;
    uint16_t format = cmap.U16(bestAt);
    std::vector<Range> raw;
    if (format == 4) {
        // The 16-bit length overflows in large subtables; FreeType reads to the table's end, as here.
        ReadCmapFormat4(cmap.Sub(bestAt, SIZE_MAX), glyphs, raw);
    } else if (format == 12 || format == 13) {
        uint32_t len = cmap.U32(bestAt + 4);
        ReadCmapGroups(cmap.Sub(bestAt, len), format == 13, glyphs, raw);
    } else {
        uint16_t len = cmap.U16(bestAt + 2);
        ReadCmapSimple(cmap.Sub(bestAt, len), format, glyphs, raw);
    }
    *out = Normalize(std::move(raw));
    return !out->empty();
}

// fontconfig's weight scale from OpenType's, by its table and linear between the steps.
float WeightFromOpenType(double ot) {
    static const double kMap[][2] = {{0, 0}, {100, 0}, {200, 40}, {300, 50}, {350, 55}, {380, 75}, {400, 80},
                                     {500, 100}, {600, 180}, {700, 200}, {800, 205}, {900, 210}, {1000, 215}};
    if (ot >= 1 && ot <= 9) ot *= 100;  // fonts that give the class as 1..9
    ot = std::clamp(ot, 0.0, 1000.0);
    for (size_t i = 1; i < std::size(kMap); ++i)
        if (ot <= kMap[i][0])
            return (float)(kMap[i - 1][1] + (ot - kMap[i - 1][0]) * (kMap[i][1] - kMap[i - 1][1]) / (kMap[i][0] - kMap[i - 1][0]));
    return 215;
}

float WidthFromClass(int c) {
    static const float kMap[] = {100, 50, 62.5f, 75, 87.5f, 100, 112.5f, 125, 150, 200};
    return c >= 1 && c <= 9 ? kMap[c] : 100;
}

std::string Lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

bool Contains(const std::string& hay, const char* needle) { return Lower(hay).find(needle) != std::string::npos; }

// The weight a style name implies when the font gives no OS/2 class.
float WeightFromStyle(const std::string& style) {
    static const std::pair<const char*, float> kWords[] = {
        {"thin", 0}, {"extralight", 40}, {"ultralight", 40}, {"semilight", 55}, {"demilight", 55}, {"light", 50},
        {"book", 75}, {"medium", 100}, {"semibold", 180}, {"demibold", 180}, {"extrabold", 205}, {"ultrabold", 205},
        {"bold", 200}, {"black", 210}, {"heavy", 210}};
    std::string s;
    for (char c : Lower(style))
        if (c != ' ' && c != '-') s += c;
    for (auto& [word, w] : kWords)
        if (s.find(word) != std::string::npos) return w;
    return 80;
}

bool CoversAll(const std::vector<Range>& have, const OrthRange* need, size_t count) {
    size_t h = 0;
    for (size_t i = 0; i < count; ++i) {
        uint32_t c = need[i].First(), last = need[i].Last();
        while (c <= last) {
            while (h < have.size() && have[h].last < c) ++h;
            if (h == have.size() || have[h].first > c) return false;
            if (have[h].last >= last) break;
            c = have[h].last + 1;
        }
    }
    return true;
}

// The languages fontconfig would say the face supports: every letter of the orthography, and a
// font that claims exactly one of the Han code pages claims no other Han language.
uint64_t LangsOf(const std::vector<Range>& coverage, Bytes os2) {
    static const std::pair<int, const char*> kHan[] = {{17, "ja"}, {18, "zh-cn"}, {19, "ko"}, {20, "zh-tw"}};
    const char* exclusive = nullptr;
    uint16_t version = os2.U16(0);
    if (os2.Has(0, 86) && version >= 1 && version != 0xFFFF) {
        uint32_t pages = os2.U32(78);
        for (auto& [bit, lang] : kHan) {
            if (!(pages & (1u << bit))) continue;
            if (exclusive) {
                exclusive = nullptr;
                break;
            }
            exclusive = lang;
        }
    }
    uint64_t out = 0;
    for (size_t i = 0; i < kLangCount; ++i) {
        const char* lang = kOrthographies[i].lang;
        if (exclusive && strcmp(lang, exclusive) != 0 &&
            std::any_of(std::begin(kHan), std::end(kHan), [&](auto& h) { return !strcmp(h.second, lang); }))
            continue;
        if (CoversAll(coverage, kOrthographies[i].ranges, kOrthographies[i].count)) out |= 1ull << i;
    }
    return out;
}

bool ReadFace(const Tables& t, int index, Face* face) {
    Bytes maxp = t.Get(Tag("maxp")), os2 = t.Get(Tag("OS/2")), head = t.Get(Tag("head")), post = t.Get(Tag("post"));
    uint32_t glyphs = maxp.Has(4, 2) ? maxp.U16(4) : 0;
    if (!glyphs) return false;
    std::vector<Range> coverage;
    if (!ReadCmap(t.Get(Tag("cmap")), glyphs, &coverage, &face->symbol)) return false;
    face->index = index;
    std::vector<Name> names = ReadNames(t.Get(Tag("name")));
    auto pick = [&](uint16_t id) -> std::string {
        for (const Name& n : names)
            if (n.id == id && n.english) return n.text;
        for (const Name& n : names)
            if (n.id == id) return n.text;
        return "";
    };
    for (uint16_t id : {16, 21, 1})
        for (const Name& n : names)
            if (n.id == id && std::find(face->families.begin(), face->families.end(), n.text) == face->families.end())
                face->families.push_back(n.text);
    face->family = pick(16);
    if (face->family.empty()) face->family = pick(1);
    if (face->family.empty() && !face->families.empty()) face->family = face->families[0];
    if (face->family.empty()) return false;
    face->style = pick(17);
    if (face->style.empty()) face->style = pick(2);

    float weight = os2.Has(4, 2) && os2.U16(4) ? WeightFromOpenType(os2.U16(4)) : WeightFromStyle(face->style);
    float width = os2.Has(6, 2) ? WidthFromClass(os2.U16(6)) : 100;
    uint16_t selection = os2.Has(62, 2) ? os2.U16(62) : 0, macStyle = head.Has(44, 2) ? head.U16(44) : 0;
    float slant = 0;
    if (Contains(face->style, "oblique") || Contains(face->style, "inclined") || (selection & 0x200)) slant = 110;
    if (Contains(face->style, "italic") || Contains(face->style, "kursiv") || (selection & 1) || (macStyle & 2)) slant = 100;
    face->weightMin = face->weightMax = weight;
    face->widthMin = face->widthMax = width;
    face->slantMin = face->slantMax = slant;
    // A variable font matches anywhere along its axes, as fontconfig's named instances do.
    Bytes fvar = t.Get(Tag("fvar"));
    if (fvar.U16(0) == 1) {
        uint16_t axesAt = fvar.U16(4), axes = fvar.U16(8), size = fvar.U16(10);
        for (uint16_t a = 0; a < axes && size >= 20 && fvar.Has(axesAt + (size_t)a * size, 20); ++a) {
            size_t at = axesAt + (size_t)a * size;
            uint32_t tag = fvar.U32(at);
            double lo = (int32_t)fvar.U32(at + 4) / 65536.0, hi = (int32_t)fvar.U32(at + 12) / 65536.0;
            if (!(lo <= hi)) continue;
            if (tag == Tag("wght")) face->weightMin = WeightFromOpenType(lo), face->weightMax = WeightFromOpenType(hi);
            else if (tag == Tag("wdth") && lo > 0) face->widthMin = (float)lo, face->widthMax = (float)hi;
            else if (tag == Tag("ital") && hi > lo) face->slantMin = (float)(lo * 100), face->slantMax = (float)(hi * 100);
        }
    }
    // Monospace as fontconfig decides it: the post table's flag, or every glyph one advance (or two).
    if (post.Has(12, 4) && post.U32(12)) {
        face->spacing = 100;
    } else {
        Bytes hhea = t.Get(Tag("hhea")), hmtx = t.Get(Tag("hmtx"));
        uint16_t metrics = hhea.Has(34, 2) ? hhea.U16(34) : 0;
        std::set<uint16_t> advances;
        for (uint16_t i = 0; i < metrics && hmtx.Has((size_t)i * 4, 2) && advances.size() <= 2; ++i)
            if (uint16_t adv = hmtx.U16((size_t)i * 4)) advances.insert(adv);
        if (advances.size() == 1) face->spacing = 100;
        else if (advances.size() == 2 && (*advances.rbegin() == 2 * *advances.begin())) face->spacing = 90;
    }
    if (fvar.U16(0) == 1 && fvar.U16(8) && !fvar.U16(12)) face->variableOnly = true;
    face->version = head.U32(4);
    std::string squashedStyle;
    for (char c : Lower(face->style))
        if (c != ' ') squashedStyle += c;
    for (const char* word : {"shadow", "caps", "antiqua", "romansc", "embosed", "dunhill"})
        if (squashedStyle.find(word) != std::string::npos) face->decorative = true;
    face->color = t.Has(Tag("COLR")) || t.Has(Tag("CBDT")) || t.Has(Tag("sbix")) || t.Has(Tag("SVG "));
    face->langs = LangsOf(coverage, os2);
    face->coverage = std::move(coverage);
    return true;
}

}  // namespace

bool Face::Covers(uint32_t ch) const {
    auto it = std::upper_bound(coverage.begin(), coverage.end(), ch, [](uint32_t c, const Range& r) { return c < r.first; });
    return it != coverage.begin() && (it - 1)->last >= ch;
}

const std::vector<std::string>& Languages() {
    static const std::vector<std::string> langs = [] {
        std::vector<std::string> out;
        for (const auto& o : kOrthographies) out.push_back(o.lang);
        return out;
    }();
    return langs;
}

std::vector<Face> ParseFont(const unsigned char* data, size_t size) {
    std::vector<Face> out;
    Bytes file{data, size};
    std::vector<size_t> offsets;
    if (file.U32(0) == Tag("ttcf")) {
        uint32_t count = file.U32(8);
        for (uint32_t i = 0; i < count && i < 256 && file.Has(12 + (size_t)i * 4, 4); ++i) offsets.push_back(file.U32(12 + i * 4));
    } else {
        offsets.push_back(0);
    }
    for (size_t i = 0; i < offsets.size(); ++i) {
        Tables t;
        Face face;
        if (ReadTables(file, offsets[i], &t) && ReadFace(t, (int)i, &face)) out.push_back(std::move(face));
    }
    return out;
}

namespace {

// ---- Patterns and values ---------------------------------------------------------------------

struct Value {
    enum Type { Str, Num, Bool } type = Str;
    std::string s;
    double n = 0;
    bool b = false;
    static Value String(std::string s) { return {Str, std::move(s), 0, false}; }
    static Value Number(double n) { return {Num, {}, n, false}; }
    static Value Boolean(bool b) { return {Bool, {}, 0, b}; }
};

enum class Bind { Weak, Strong, Same };
struct PatternValue {
    Value v;
    Bind bind;
};
struct Pattern {
    std::map<std::string, std::vector<PatternValue>> el;
    std::vector<Range> charset;  // code points a face must have ("charset=2605")
};

// fontconfig's named constants, for the object they name.
bool Constant(const std::string& object, const std::string& name, Value* out) {
    static const struct {
        const char *name, *object;
        double value;
    } kConst[] = {
        {"thin", "weight", 0}, {"extralight", "weight", 40}, {"ultralight", "weight", 40}, {"light", "weight", 50},
        {"demilight", "weight", 55}, {"semilight", "weight", 55}, {"book", "weight", 75}, {"regular", "weight", 80},
        {"normal", "weight", 80}, {"medium", "weight", 100}, {"demibold", "weight", 180}, {"semibold", "weight", 180},
        {"bold", "weight", 200}, {"extrabold", "weight", 205}, {"ultrabold", "weight", 205}, {"black", "weight", 210},
        {"heavy", "weight", 210}, {"roman", "slant", 0}, {"italic", "slant", 100}, {"oblique", "slant", 110},
        {"ultracondensed", "width", 50}, {"extracondensed", "width", 63}, {"condensed", "width", 75},
        {"semicondensed", "width", 87}, {"normal", "width", 100}, {"semiexpanded", "width", 113}, {"expanded", "width", 125},
        {"extraexpanded", "width", 150}, {"ultraexpanded", "width", 200}, {"proportional", "spacing", 0},
        {"dual", "spacing", 90}, {"mono", "spacing", 100}, {"charcell", "spacing", 110}};
    std::string n = Lower(name);
    if (n == "true" || n == "false") {
        *out = Value::Boolean(n == "true");
        return true;
    }
    for (const auto& c : kConst)
        if (n == c.name && (object.empty() || object == c.object)) {
            *out = Value::Number(c.value);
            return true;
        }
    return false;
}

const char* ConstantObject(const std::string& name) {
    static const char* kObjects[] = {"weight", "slant", "width", "spacing"};
    Value v;
    for (const char* o : kObjects)
        if (Constant(o, name, &v)) return o;
    return nullptr;
}

std::string Squash(const std::string& s) {  // fontconfig's family comparison: no blanks, any case
    std::string out;
    for (char c : s)
        if (c != ' ') out += (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    return out;
}

// ---- The fontconfig configuration subset ----------------------------------------------------

enum class Op { Eq, NotEq, Less, LessEq, More, MoreEq, Contains, NotContains };
enum class Qual { Any, All, First, NotFirst };
enum class Mode { Assign, AssignReplace, Prepend, PrependFirst, Append, AppendLast, Delete, DeleteAll };

struct Test {
    std::string object;
    Qual qual = Qual::Any;
    Op op = Op::Eq;
    bool ignoreBlanks = false;
    std::vector<Value> values;  // any of them
};
struct Edit {
    std::string object;
    Mode mode = Mode::Assign;
    Bind bind = Bind::Weak;
    std::vector<Value> values;
};
struct Rule {
    std::vector<Test> tests;
    std::vector<Edit> edits;
};

struct Config {
    std::vector<std::string> dirs, rejects, accepts;
    std::vector<Rule> rules;
    bool read = false;  // a configuration file was found and parsed
};

struct Node {
    std::string name, text;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<Node> kids;
    std::string Attr(const char* key, const char* fallback = "") const {
        for (auto& [k, v] : attrs)
            if (k == key) return v;
        return fallback;
    }
    std::string Text() const {
        size_t a = text.find_first_not_of(" \t\r\n"), b = text.find_last_not_of(" \t\r\n");
        return a == std::string::npos ? "" : text.substr(a, b - a + 1);
    }
};

std::string Unescape(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        size_t end = s.find(';', i);
        if (end == std::string::npos || end - i > 10) {
            out += s[i];
            continue;
        }
        std::string e = s.substr(i + 1, end - i - 1);
        if (e == "amp") out += '&';
        else if (e == "lt") out += '<';
        else if (e == "gt") out += '>';
        else if (e == "quot") out += '"';
        else if (e == "apos") out += '\'';
        else if (e.size() > 1 && e[0] == '#') {
            uint32_t c = (uint32_t)strtoul(e.c_str() + (e[1] == 'x' ? 2 : 1), nullptr, e[1] == 'x' ? 16 : 10);
            if (c && c <= 0x10FFFF) AppendUtf8(out, c);
        } else {
            out += s.substr(i, end - i + 1);
        }
        i = end;
    }
    return out;
}

// Enough XML for fontconfig's files: elements, attributes, text, comments, CDATA and a DOCTYPE.
bool ParseXml(const std::string& src, Node* root) {
    std::vector<Node*> stack{root};
    size_t i = 0;
    auto isName = [](char c) { return isalnum((unsigned char)c) || c == '_' || c == '-' || c == ':' || c == '.'; };
    while (i < src.size()) {
        if (src[i] != '<') {
            size_t end = src.find('<', i);
            if (end == std::string::npos) end = src.size();
            stack.back()->text += Unescape(src.substr(i, end - i));
            i = end;
            continue;
        }
        if (src.compare(i, 4, "<!--") == 0) {
            size_t end = src.find("-->", i + 4);
            if (end == std::string::npos) return false;
            i = end + 3;
        } else if (src.compare(i, 9, "<![CDATA[") == 0) {
            size_t end = src.find("]]>", i + 9);
            if (end == std::string::npos) return false;
            stack.back()->text += src.substr(i + 9, end - i - 9);
            i = end + 3;
        } else if (src.compare(i, 2, "<?") == 0) {
            size_t end = src.find("?>", i + 2);
            if (end == std::string::npos) return false;
            i = end + 2;
        } else if (src.compare(i, 2, "<!") == 0) {
            int depth = 0;
            for (++i; i < src.size(); ++i) {
                if (src[i] == '[') ++depth;
                else if (src[i] == ']') --depth;
                else if (src[i] == '>' && depth <= 0) break;
            }
            ++i;
        } else if (src.compare(i, 2, "</") == 0) {
            size_t end = src.find('>', i);
            if (end == std::string::npos) return false;
            std::string name = src.substr(i + 2, end - i - 2);
            while (!name.empty() && isspace((unsigned char)name.back())) name.pop_back();
            for (size_t k = stack.size(); k-- > 1;)
                if (stack[k]->name == name) {
                    stack.resize(k);
                    break;
                }
            i = end + 1;
        } else {
            size_t j = i + 1;
            while (j < src.size() && isName(src[j])) ++j;
            if (j == i + 1 || stack.size() > 64) return false;
            Node node;
            node.name = src.substr(i + 1, j - i - 1);
            bool closed = false;
            while (j < src.size()) {
                while (j < src.size() && isspace((unsigned char)src[j])) ++j;
                if (j >= src.size()) return false;
                if (src[j] == '>') {
                    ++j;
                    break;
                }
                if (src.compare(j, 2, "/>") == 0) {
                    closed = true;
                    j += 2;
                    break;
                }
                size_t k = j;
                while (k < src.size() && isName(src[k])) ++k;
                if (k == j) return false;
                std::string key = src.substr(j, k - j);
                while (k < src.size() && isspace((unsigned char)src[k])) ++k;
                std::string value;
                if (k < src.size() && src[k] == '=') {
                    ++k;
                    while (k < src.size() && isspace((unsigned char)src[k])) ++k;
                    if (k >= src.size() || (src[k] != '"' && src[k] != '\'')) return false;
                    size_t end = src.find(src[k], k + 1);
                    if (end == std::string::npos) return false;
                    value = Unescape(src.substr(k + 1, end - k - 1));
                    k = end + 1;
                }
                node.attrs.emplace_back(std::move(key), std::move(value));
                j = k;
            }
            stack.back()->kids.push_back(std::move(node));
            if (!closed) stack.push_back(&stack.back()->kids.back());
            i = j;
        }
    }
    return true;
}

std::string Env(const char* name) {
    const char* v = getenv(name);
    return v && *v ? v : "";
}

std::string Home() { return Env("HOME"); }

std::string XdgDir(const char* var, const char* fallback) {
    std::string v = Env(var);
    if (!v.empty() && v[0] == '/') return v;
    return Home().empty() ? "" : Home() + "/" + fallback;
}

std::string DirOf(const std::string& path) {
    size_t slash = path.rfind('/');
    return slash == std::string::npos ? "." : slash == 0 ? "/" : path.substr(0, slash);
}

// A configuration path: "~/" is home, prefix="xdg" is under the XDG base, a relative one is
// next to the file that names it.
std::string ConfigPath(const std::string& raw, const std::string& prefix, const std::string& base, const char* xdgVar,
                       const char* xdgFallback) {
    if (raw.empty()) return "";
    if (prefix == "xdg") {
        std::string root = XdgDir(xdgVar, xdgFallback);
        return root.empty() ? "" : root + "/" + raw;
    }
    if (raw[0] == '~') {
        if (Home().empty()) return "";
        return Home() + raw.substr(1);
    }
    if (raw[0] == '/') return raw;
    return base + "/" + raw;
}

bool ReadFile(const std::string& path, std::string* out, size_t limit) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    out->clear();
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        out->append(buf, n);
        if (out->size() > limit) {
            fclose(f);
            return false;
        }
    }
    fclose(f);
    return true;
}

bool ParseValue(const Node& n, const std::string& object, Value* out) {
    if (n.name == "string") *out = Value::String(n.Text());
    else if (n.name == "int" || n.name == "double") *out = Value::Number(strtod(n.Text().c_str(), nullptr));
    else if (n.name == "bool") {
        std::string t = Lower(n.Text());
        *out = Value::Boolean(t == "true" || t == "yes" || t == "1");
    } else if (n.name == "const") return Constant(object, n.Text(), out);
    else return false;
    return true;
}

bool ParseValues(const Node& n, const std::string& object, std::vector<Value>* out) {
    for (const Node& k : n.kids) {
        Value v;
        if (!ParseValue(k, object, &v)) return false;
        out->push_back(std::move(v));
    }
    return !out->empty();
}

Bind ParseBind(const std::string& s, Bind fallback) {
    if (s == "strong") return Bind::Strong;
    if (s == "same") return Bind::Same;
    if (s == "weak") return Bind::Weak;
    return fallback;
}

void ParseAlias(const Node& n, Config* c) {
    Rule rule;
    Test test;
    test.object = "family";
    Bind bind = ParseBind(n.Attr("binding"), Bind::Weak);
    for (const Node& k : n.kids) {
        if (k.name == "family") {
            test.values.push_back(Value::String(k.Text()));
            continue;
        }
        Mode mode;
        if (k.name == "prefer") mode = Mode::Prepend;
        else if (k.name == "accept") mode = Mode::Append;
        else if (k.name == "default") mode = Mode::AppendLast;
        else continue;
        Edit edit{"family", mode, bind, {}};
        for (const Node& f : k.kids)
            if (f.name == "family") edit.values.push_back(Value::String(f.Text()));
        if (!edit.values.empty()) rule.edits.push_back(std::move(edit));
    }
    if (test.values.empty() || rule.edits.empty()) return;
    rule.tests.push_back(std::move(test));
    c->rules.push_back(std::move(rule));
}

// A <match target="pattern"> whose tests this matcher can evaluate; edits of an expression it
// cannot (matrices, arithmetic) are dropped, as they never change which file a pattern picks.
void ParseMatch(const Node& n, Config* c) {
    std::string target = n.Attr("target", "pattern");
    if (target != "pattern") return;
    Rule rule;
    for (const Node& k : n.kids) {
        if (k.name == "test") {
            Test t;
            t.object = k.Attr("name");
            std::string qual = k.Attr("qual", "any"), cmp = k.Attr("compare", "eq"), tt = k.Attr("target", "default");
            if (tt != "default" && tt != "pattern") return;
            t.qual = qual == "all" ? Qual::All : qual == "first" ? Qual::First : qual == "not_first" ? Qual::NotFirst : Qual::Any;
            static const std::pair<const char*, Op> kOps[] = {{"eq", Op::Eq}, {"not_eq", Op::NotEq}, {"less", Op::Less},
                                                              {"less_eq", Op::LessEq}, {"more", Op::More}, {"more_eq", Op::MoreEq},
                                                              {"contains", Op::Contains}, {"not_contains", Op::NotContains}};
            bool known = false;
            for (auto& [name, op] : kOps)
                if (cmp == name) t.op = op, known = true;
            t.ignoreBlanks = k.Attr("ignore-blanks") == "true";
            if (!known || t.object.empty() || !ParseValues(k, t.object, &t.values)) return;
            rule.tests.push_back(std::move(t));
        } else if (k.name == "edit") {
            Edit e;
            e.object = k.Attr("name");
            std::string mode = k.Attr("mode", "assign");
            static const std::pair<const char*, Mode> kModes[] = {
                {"assign", Mode::Assign}, {"assign_replace", Mode::AssignReplace}, {"prepend", Mode::Prepend},
                {"prepend_first", Mode::PrependFirst}, {"append", Mode::Append}, {"append_last", Mode::AppendLast},
                {"delete", Mode::Delete}, {"delete_all", Mode::DeleteAll}};
            bool known = false;
            for (auto& [name, m] : kModes)
                if (mode == name) e.mode = m, known = true;
            e.bind = ParseBind(k.Attr("binding"), Bind::Weak);
            bool deleting = e.mode == Mode::Delete || e.mode == Mode::DeleteAll;
            if (!known || e.object.empty() || (!deleting && !ParseValues(k, e.object, &e.values))) continue;
            rule.edits.push_back(std::move(e));
        }
    }
    if (!rule.edits.empty()) c->rules.push_back(std::move(rule));
}

void LoadConfigFile(const std::string& path, Config* c, int depth);

void LoadConfigDir(const std::string& dir, Config* c, int depth) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    std::vector<std::string> names;
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n.size() > 5 && n[0] >= '0' && n[0] <= '9' && n.compare(n.size() - 5, 5, ".conf") == 0) names.push_back(n);
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    for (const std::string& n : names) LoadConfigFile(dir + "/" + n, c, depth + 1);
}

void LoadConfigFile(const std::string& path, Config* c, int depth) {
    if (depth > 16) return;
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        LoadConfigDir(path, c, depth);
        return;
    }
    std::string src;
    Node root;
    if (!ReadFile(path, &src, 4 << 20) || !ParseXml(src, &root)) return;
    const Node* top = nullptr;
    for (const Node& k : root.kids)
        if (k.name == "fontconfig") top = &k;
    if (!top) return;
    c->read = true;
    std::string base = DirOf(path);
    for (const Node& k : top->kids) {
        if (k.name == "dir") {
            std::string dir = ConfigPath(k.Text(), k.Attr("prefix"), base, "XDG_DATA_HOME", ".local/share");
            if (!dir.empty()) c->dirs.push_back(dir);
        } else if (k.name == "reset-dirs") {
            c->dirs.clear();
        } else if (k.name == "include") {
            std::string inc = ConfigPath(k.Text(), k.Attr("prefix"), base, "XDG_CONFIG_HOME", ".config");
            if (!inc.empty()) LoadConfigFile(inc, c, depth + 1);
        } else if (k.name == "alias") {
            ParseAlias(k, c);
        } else if (k.name == "match") {
            ParseMatch(k, c);
        } else if (k.name == "selectfont") {
            for (const Node& s : k.kids)
                for (const Node& g : s.kids)
                    if (g.name == "glob" && (s.name == "rejectfont" || s.name == "acceptfont"))
                        (s.name == "rejectfont" ? c->rejects : c->accepts).push_back(g.Text());
        }
    }
}

void AddAlias(Config* c, std::vector<const char*> families, Mode mode, Bind bind, std::vector<const char*> values) {
    Rule r;
    Test t;
    t.object = "family";
    for (const char* f : families) t.values.push_back(Value::String(f));
    Edit e{"family", mode, bind, {}};
    for (const char* v : values) e.values.push_back(Value::String(v));
    r.tests.push_back(std::move(t));
    r.edits.push_back(std::move(e));
    c->rules.push_back(std::move(r));
}

// Without a fontconfig configuration: the common free faces for the generic names, emoji in
// color, the CJK faces for their languages, and sans-serif behind every family, as fontconfig's
// own stock files arrange.
void BuiltinConfig(Config* c) {
    AddAlias(c, {"Noto Color Emoji", "Apple Color Emoji", "Twemoji", "Twitter Color Emoji", "JoyPixels", "Emoji One",
                 "Segoe UI Emoji"},
             Mode::AppendLast, Bind::Same, {"emoji"});
    {
        Rule r;
        r.tests.push_back({"family", Qual::Any, Op::Eq, false, {Value::String("emoji")}});
        r.edits.push_back({"lang", Mode::Prepend, Bind::Weak, {Value::String("und-zsye")}});
        c->rules.push_back(std::move(r));
        Rule color;
        color.tests.push_back({"lang", Qual::Any, Op::Eq, false, {Value::String("und-zsye")}});
        color.tests.push_back({"color", Qual::All, Op::NotEq, false, {Value::Boolean(true)}});
        color.tests.push_back({"color", Qual::All, Op::NotEq, false, {Value::Boolean(false)}});
        color.edits.push_back({"color", Mode::Append, Bind::Weak, {Value::Boolean(true)}});
        c->rules.push_back(std::move(color));
    }
    {
        Rule r;
        for (const char* g : {"sans-serif", "serif", "monospace"})
            r.tests.push_back({"family", Qual::All, Op::NotEq, false, {Value::String(g)}});
        r.edits.push_back({"family", Mode::AppendLast, Bind::Weak, {Value::String("sans-serif")}});
        c->rules.push_back(std::move(r));
    }
    static const char* kCjk[][3] = {{"ja", "Noto Sans CJK JP", "Noto Serif CJK JP"}, {"ko", "Noto Sans CJK KR", "Noto Serif CJK KR"},
                                    {"zh-cn", "Noto Sans CJK SC", "Noto Serif CJK SC"}, {"zh-tw", "Noto Sans CJK TC", "Noto Serif CJK TC"},
                                    {"zh-hk", "Noto Sans CJK HK", "Noto Serif CJK HK"}};
    for (auto& cjk : kCjk)
        for (int serif = 0; serif < 2; ++serif) {
            Rule r;
            r.tests.push_back({"lang", Qual::Any, Op::Eq, false, {Value::String(cjk[0])}});
            r.tests.push_back({"family", Qual::Any, Op::Eq, false, {Value::String(serif ? "serif" : "sans-serif")}});
            r.edits.push_back({"family", Mode::Prepend, Bind::Weak, {Value::String(cjk[1 + serif])}});
            c->rules.push_back(std::move(r));
        }
    AddAlias(c, {"sans-serif"}, Mode::Prepend, Bind::Weak,
             {"DejaVu Sans", "Noto Sans", "Liberation Sans", "Arimo", "Nimbus Sans", "Cantarell", "Ubuntu", "FreeSans",
              "Noto Sans CJK JP", "Noto Sans Arabic", "Noto Sans Hebrew", "Noto Sans Thai", "Noto Sans Devanagari"});
    AddAlias(c, {"serif"}, Mode::Prepend, Bind::Weak,
             {"DejaVu Serif", "Noto Serif", "Liberation Serif", "Tinos", "Nimbus Roman", "FreeSerif"});
    AddAlias(c, {"monospace"}, Mode::Prepend, Bind::Weak,
             {"DejaVu Sans Mono", "Noto Sans Mono", "Liberation Mono", "Cousine", "Nimbus Mono PS", "FreeMono"});
    AddAlias(c, {"emoji"}, Mode::Prepend, Bind::Same, {"Noto Color Emoji", "Apple Color Emoji", "Twemoji", "JoyPixels"});
}

Config LoadConfig() {
    Config c;
    std::string file = Env("FONTCONFIG_FILE");
    if (!file.empty() && file[0] != '/') {
        std::string dir = Env("FONTCONFIG_PATH");
        file = (dir.empty() ? std::string("/etc/fonts") : dir) + "/" + file;
    }
    if (file.empty()) file = Env("FONTCONFIG_PATH").empty() ? "/etc/fonts/fonts.conf" : Env("FONTCONFIG_PATH") + "/fonts.conf";
    LoadConfigFile(file, &c, 0);
    if (!c.read) {
        c = Config{};
        BuiltinConfig(&c);
    }
    return c;
}

// ---- The index and its cache ----------------------------------------------------------------

struct FileEntry {
    std::string path;
    uint64_t size = 0;
    int64_t mtime = 0;  // nanoseconds
    std::vector<Face> faces;
};

struct IndexedFace {
    const Face* face;
    const std::string* file;
    std::vector<std::string> squashed;  // the families as compared
};

struct Index {
    Config config;
    std::vector<FileEntry> files;
    std::vector<IndexedFace> faces;  // in fontconfig's order: directories breadth first, names sorted
    IndexInfo info;
};

constexpr char kCacheMagic[8] = {'C', 'R', 'D', 'F', 'O', 'N', 'T', 'S'};
constexpr uint32_t kCacheVersion = 2;

uint32_t LangHash() {
    uint32_t h = 2166136261u;
    for (const auto& o : kOrthographies) {
        for (const char* p = o.lang; *p; ++p) h = (h ^ (uint8_t)*p) * 16777619u;
        h = (h ^ (uint32_t)o.count) * 16777619u;
    }
    return h;
}

struct Writer {
    std::string b;
    void U32(uint32_t v) { b.append((const char*)&v, 4); }
    void U64(uint64_t v) { b.append((const char*)&v, 8); }
    void F32(float v) { b.append((const char*)&v, 4); }
    void Str(const std::string& s) { U32((uint32_t)s.size()), b += s; }
};

struct Reader {
    const std::string& b;
    size_t at = 0;
    bool ok = true;
    bool Take(void* out, size_t n) {
        if (!ok || b.size() - at < n) return ok = false;
        memcpy(out, b.data() + at, n);
        at += n;
        return true;
    }
    uint32_t U32() { uint32_t v = 0; Take(&v, 4); return v; }
    uint64_t U64() { uint64_t v = 0; Take(&v, 8); return v; }
    float F32() { float v = 0; Take(&v, 4); return v; }
    std::string Str() {
        uint32_t n = U32();
        if (!ok || b.size() - at < n) return ok = false, std::string();
        std::string s = b.substr(at, n);
        at += n;
        return s;
    }
    size_t Count(size_t each) {  // an element count that the remaining bytes could hold
        uint32_t n = U32();
        if (ok && (size_t)n * each > b.size() - at) ok = false;
        return ok ? n : 0;
    }
};

std::string CachePath() {
    std::string root = XdgDir("XDG_CACHE_HOME", ".cache");
    return root.empty() ? "" : root + "/cordinal/fonts.idx";
}

std::unordered_map<std::string, FileEntry> ReadCache(const std::string& path) {
    std::unordered_map<std::string, FileEntry> out;
    std::string b;
    if (path.empty() || !ReadFile(path, &b, 256u << 20)) return out;
    Reader r{b};
    char magic[8];
    if (!r.Take(magic, 8) || memcmp(magic, kCacheMagic, 8) || r.U32() != kCacheVersion || r.U32() != kLangCount || r.U32() != LangHash())
        return out;
    size_t files = r.Count(24);
    for (size_t i = 0; i < files && r.ok; ++i) {
        FileEntry e;
        e.path = r.Str();
        e.size = r.U64();
        e.mtime = (int64_t)r.U64();
        size_t faces = r.Count(16);
        for (size_t k = 0; k < faces && r.ok; ++k) {
            Face f;
            f.index = (int)r.U32();
            size_t names = r.Count(4);
            for (size_t n = 0; n < names && r.ok; ++n) f.families.push_back(r.Str());
            f.family = r.Str();
            f.style = r.Str();
            f.weightMin = r.F32(), f.weightMax = r.F32(), f.widthMin = r.F32(), f.widthMax = r.F32();
            f.slantMin = r.F32(), f.slantMax = r.F32();
            f.spacing = (int)r.U32();
            uint32_t flags = r.U32();
            f.color = flags & 1, f.symbol = flags & 2, f.decorative = flags & 4, f.variableOnly = flags & 8;
            f.version = r.U32();
            f.langs = r.U64();
            size_t ranges = r.Count(8);
            f.coverage.resize(ranges);
            for (Range& x : f.coverage) x.first = r.U32(), x.last = r.U32();
            e.faces.push_back(std::move(f));
        }
        if (r.ok) out[e.path] = std::move(e);
    }
    if (!r.ok) out.clear();  // a damaged cache is rebuilt whole
    return out;
}

void WriteCache(const std::string& path, const std::vector<FileEntry>& files) {
    if (path.empty()) return;
    Writer w;
    w.b.append(kCacheMagic, 8);
    w.U32(kCacheVersion), w.U32((uint32_t)kLangCount), w.U32(LangHash());
    w.U32((uint32_t)files.size());
    for (const FileEntry& e : files) {
        w.Str(e.path), w.U64(e.size), w.U64((uint64_t)e.mtime);
        w.U32((uint32_t)e.faces.size());
        for (const Face& f : e.faces) {
            w.U32((uint32_t)f.index);
            w.U32((uint32_t)f.families.size());
            for (const std::string& n : f.families) w.Str(n);
            w.Str(f.family), w.Str(f.style);
            w.F32(f.weightMin), w.F32(f.weightMax), w.F32(f.widthMin), w.F32(f.widthMax), w.F32(f.slantMin), w.F32(f.slantMax);
            w.U32((uint32_t)f.spacing), w.U32((f.color ? 1 : 0) | (f.symbol ? 2 : 0) | (f.decorative ? 4 : 0) | (f.variableOnly ? 8 : 0));
            w.U32(f.version);
            w.U64(f.langs);
            w.U32((uint32_t)f.coverage.size());
            for (const Range& x : f.coverage) w.U32(x.first), w.U32(x.last);
        }
    }
    std::string dir = DirOf(path);
    mkdir(DirOf(dir).c_str(), 0700);
    mkdir(dir.c_str(), 0700);
    std::string tmp = path + ".tmp" + std::to_string(getpid());
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    bool ok = fwrite(w.b.data(), 1, w.b.size(), f) == w.b.size();
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp.c_str(), path.c_str()) != 0) unlink(tmp.c_str());
}

std::vector<Face> ParseFile(const std::string& path, uint64_t size) {
    if (size < 12 || size > (1ull << 31)) return {};
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return {};
    void* map = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return {};
    std::vector<Face> faces = ParseFont((const unsigned char*)map, size);
    munmap(map, size);
    return faces;
}

bool FontFileName(const std::string& name) {
    size_t dot = name.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = Lower(name.substr(dot + 1));
    return ext == "ttf" || ext == "otf" || ext == "ttc" || ext == "otc";
}

bool Rejected(const Config& c, const std::string& path) {
    auto any = [&](const std::vector<std::string>& globs) {
        return std::any_of(globs.begin(), globs.end(), [&](const std::string& g) { return fnmatch(g.c_str(), path.c_str(), 0) == 0; });
    };
    return any(c.rejects) && !any(c.accepts);
}

// fontconfig's configured directories, then the usual ones it may not name.
std::vector<std::string> FontDirs(const Config& c) {
    std::vector<std::string> dirs = c.dirs;
    dirs.push_back("/usr/share/fonts");
    dirs.push_back("/usr/local/share/fonts");
    std::string data = XdgDir("XDG_DATA_HOME", ".local/share");
    if (!data.empty()) dirs.push_back(data + "/fonts");
    if (!Home().empty()) dirs.push_back(Home() + "/.fonts");
    std::string list = Env("XDG_DATA_DIRS");
    if (list.empty()) list = "/usr/local/share:/usr/share";
    for (size_t a = 0; a <= list.size();) {
        size_t b = list.find(':', a);
        if (b == std::string::npos) b = list.size();
        if (b > a && list[a] == '/') dirs.push_back(list.substr(a, b - a) + "/fonts");
        a = b + 1;
    }
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const std::string& d : dirs) {
        char* real = realpath(d.c_str(), nullptr);
        if (!real) continue;
        std::string r = real;
        free(real);
        if (seen.insert(r).second) out.push_back(d);
    }
    return out;
}

void Build(Index* index) {
    index->config = LoadConfig();
    index->info = {};
    index->info.config = index->config.read;
    index->info.dirs = FontDirs(index->config);
    index->info.cache = CachePath();
    std::unordered_map<std::string, FileEntry> cached = ReadCache(index->info.cache);
    std::deque<std::string> queue(index->info.dirs.begin(), index->info.dirs.end());
    std::set<std::pair<dev_t, ino_t>> visited;
    while (!queue.empty() && index->files.size() < 200000) {
        std::string dir = queue.front();
        queue.pop_front();
        struct stat ds;
        if (stat(dir.c_str(), &ds) != 0 || !S_ISDIR(ds.st_mode) || !visited.insert({ds.st_dev, ds.st_ino}).second) continue;
        DIR* d = opendir(dir.c_str());
        if (!d) continue;
        std::vector<std::string> names;
        while (dirent* e = readdir(d))
            if (e->d_name[0] != '.') names.push_back(e->d_name);
        closedir(d);
        std::sort(names.begin(), names.end());
        for (const std::string& n : names) {
            std::string path = dir + "/" + n;
            struct stat st;
            if (stat(path.c_str(), &st) != 0 || Rejected(index->config, path)) continue;
            if (S_ISDIR(st.st_mode)) {
                queue.push_back(path);
                continue;
            }
            if (!S_ISREG(st.st_mode) || !FontFileName(n)) continue;
            int64_t mtime = (int64_t)st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec;
            auto it = cached.find(path);
            if (it != cached.end() && it->second.size == (uint64_t)st.st_size && it->second.mtime == mtime) {
                index->files.push_back(std::move(it->second));
                cached.erase(it);
                continue;
            }
            FileEntry e{path, (uint64_t)st.st_size, mtime, ParseFile(path, st.st_size)};
            index->files.push_back(std::move(e));
            ++index->info.parsed;
        }
    }
    // Fonts gone since the last run leave entries behind, which a rewrite drops too.
    if (index->info.parsed || !cached.empty()) WriteCache(index->info.cache, index->files);
    for (const FileEntry& e : index->files)
        for (const Face& f : e.faces) {
            IndexedFace x{&f, &e.path, {}};
            for (const std::string& n : f.families) x.squashed.push_back(Squash(n));
            index->faces.push_back(std::move(x));
        }
    index->info.files = index->files.size();
    index->info.faces = index->faces.size();
}

// ---- Patterns, substitution and matching ----------------------------------------------------

bool NumericObject(const std::string& o) {
    return o == "weight" || o == "slant" || o == "width" || o == "size" || o == "pixelsize" || o == "spacing" || o == "dpi" ||
           o == "index" || o == "scale" || o == "aspect";
}
bool BoolObject(const std::string& o) {
    return o == "color" || o == "symbol" || o == "scalable" || o == "outline" || o == "antialias" || o == "hinting" ||
           o == "variable" || o == "decorative" || o == "embolden" || o == "autohint" || o == "verticallayout" ||
           o == "globaladvance" || o == "embeddedbitmap" || o == "minspace";
}

bool ParseCharset(const std::string& s, std::vector<Range>* out) {
    std::vector<Range> raw;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && isspace((unsigned char)s[i])) ++i;
        if (i >= s.size()) break;
        char* end = nullptr;
        unsigned long a = strtoul(s.c_str() + i, &end, 16);
        if (end == s.c_str() + i) return false;
        unsigned long b = a;
        i = end - s.c_str();
        if (i < s.size() && s[i] == '-') {
            b = strtoul(s.c_str() + i + 1, &end, 16);
            if (end == s.c_str() + i + 1) return false;
            i = end - s.c_str();
        }
        if (a > 0x10FFFF || b > 0x10FFFF || a > b) return false;
        raw.push_back({(uint32_t)a, (uint32_t)b});
    }
    *out = Normalize(std::move(raw));
    return true;
}

// FcNameParse's syntax: "family,family-size:name=value,value:constant".
Pattern ParsePattern(const std::string& text) {
    Pattern p;
    // Split on unescaped ':'; within the family part split on ',' and stop at '-' (the size).
    std::vector<std::string> families{""};
    std::string sizeText;
    size_t i = 0;
    for (; i < text.size(); ++i) {
        char c = text[i];
        if (c == '\\' && i + 1 < text.size()) {
            families.back() += text[++i];
        } else if (c == ',') {
            families.emplace_back();
        } else if (c == '-') {
            for (++i; i < text.size() && text[i] != ':'; ++i) sizeText += text[i];
            break;
        } else if (c == ':') {
            break;
        } else {
            families.back() += c;
        }
    }
    for (std::string& f : families)
        if (!f.empty()) p.el["family"].push_back({Value::String(f), Bind::Strong});
    if (!sizeText.empty()) p.el["size"].push_back({Value::Number(strtod(sizeText.c_str(), nullptr)), Bind::Strong});
    while (i < text.size()) {
        ++i;  // past ':'
        std::string element;
        for (; i < text.size() && text[i] != ':'; ++i) {
            if (text[i] == '\\' && i + 1 < text.size()) ++i;
            element += text[i];
        }
        if (element.empty()) continue;
        size_t eq = element.find('=');
        if (eq == std::string::npos) {
            const char* object = ConstantObject(element);
            Value v;
            if (object && Constant(object, element, &v)) p.el[object].push_back({v, Bind::Strong});
            continue;
        }
        std::string object = Lower(element.substr(0, eq)), values = element.substr(eq + 1);
        if (object == "charset") {
            ParseCharset(values, &p.charset);
            continue;
        }
        size_t a = 0;
        while (a <= values.size()) {
            size_t b = values.find(object == "lang" ? '|' : ',', a);
            if (b == std::string::npos) b = values.size();
            std::string v = values.substr(a, b - a);
            a = b + 1;
            if (v.empty()) continue;
            Value value;
            if (NumericObject(object)) {
                char* end = nullptr;
                double n = strtod(v.c_str(), &end);
                if (end && *end == 0) value = Value::Number(n);
                else if (!Constant(object, v, &value)) continue;
            } else if (BoolObject(object)) {
                std::string l = Lower(v);
                value = Value::Boolean(l == "true" || l == "yes" || l == "1");
            } else {
                value = Value::String(object == "lang" ? Lower(v) : v);
            }
            p.el[object].push_back({value, Bind::Strong});
        }
    }
    return p;
}

int LangIndex(const std::string& lang) {
    for (size_t i = 0; i < kLangCount; ++i)
        if (lang == kOrthographies[i].lang) return (int)i;
    return -1;
}

// FcLangNormalize, for the languages this matcher knows: "de_DE.UTF-8" is "de", "zh_CN" is "zh-cn".
std::string NormalizeLang(std::string l) {
    l = Lower(l);
    size_t cut = l.find_first_of(".@");
    if (cut != std::string::npos) l.resize(cut);
    std::replace(l.begin(), l.end(), '_', '-');
    if (l.empty() || l == "c" || l == "posix") return "en";
    if (LangIndex(l) >= 0) return l;
    size_t dash = l.find('-');
    return dash == std::string::npos ? l : l.substr(0, dash);
}

std::vector<std::string> DefaultLangs() {
    std::vector<std::string> out;
    std::string list = Env("FC_LANG");
    if (!list.empty()) {
        for (size_t a = 0; a <= list.size();) {
            size_t b = list.find(':', a);
            if (b == std::string::npos) b = list.size();
            if (b > a) out.push_back(NormalizeLang(list.substr(a, b - a)));
            a = b + 1;
        }
    }
    if (out.empty()) {
        std::string l = Env("LC_ALL");
        if (l.empty()) l = Env("LC_CTYPE");
        if (l.empty()) l = Env("LANG");
        out.push_back(NormalizeLang(l));
    }
    return out;
}

bool CompareValue(const std::string& object, const Value& l, Op op, const Value& r, bool ignoreBlanks) {
    if (l.type != r.type) return op == Op::NotEq || op == Op::NotContains;
    if (l.type == Value::Str) {
        std::string a = ignoreBlanks ? Squash(l.s) : Lower(l.s), b = ignoreBlanks ? Squash(r.s) : Lower(r.s);
        switch (op) {
        case Op::Eq: return a == b;
        case Op::NotEq: return a != b;
        case Op::Contains: return object == "lang" ? a == b || a.rfind(b + "-", 0) == 0 : a.find(b) != std::string::npos;
        case Op::NotContains: return !CompareValue(object, l, Op::Contains, r, ignoreBlanks);
        default: return false;
        }
    }
    if (l.type == Value::Bool) {
        switch (op) {
        case Op::Eq: case Op::Contains: return l.b == r.b;
        case Op::NotEq: case Op::NotContains: return l.b != r.b;
        default: return false;
        }
    }
    switch (op) {
    case Op::Eq: case Op::Contains: return l.n == r.n;
    case Op::NotEq: case Op::NotContains: return l.n != r.n;
    case Op::Less: return l.n < r.n;
    case Op::LessEq: return l.n <= r.n;
    case Op::More: return l.n > r.n;
    case Op::MoreEq: return l.n >= r.n;
    }
    return false;
}

// One rule against the pattern, as FcConfigSubstitute: every test first, each recording where
// its object first matched, then the edits relative to those places.
void ApplyRule(const Rule& rule, Pattern& p) {
    std::map<std::string, int> position;
    for (const Test& t : rule.tests) {
        auto it = p.el.find(t.object);
        bool first = !position.count(t.object);
        if (it == p.el.end() || it->second.empty()) {
            if (t.qual == Qual::All) {
                if (first) position[t.object] = -1;
                continue;
            }
            return;
        }
        const auto& values = it->second;
        int found = -1;
        if (t.qual == Qual::All) {
            bool all = std::all_of(values.begin(), values.end(), [&](const PatternValue& v) {
                return std::any_of(t.values.begin(), t.values.end(),
                                   [&](const Value& r) { return CompareValue(t.object, v.v, t.op, r, t.ignoreBlanks); });
            });
            if (!all) return;
            found = 0;
        } else {
            size_t from = t.qual == Qual::NotFirst ? 1 : 0;
            for (const Value& r : t.values) {
                for (size_t k = from; k < values.size() && found < 0; ++k)
                    if (CompareValue(t.object, values[k].v, t.op, r, t.ignoreBlanks)) found = (int)k;
                if (found >= 0) break;
            }
            if (found < 0 || (t.qual == Qual::First && found != 0)) return;
        }
        if (first) position[t.object] = found;
    }
    for (const Edit& e : rule.edits) {
        auto& list = p.el[e.object];
        int& pos = position.try_emplace(e.object, -1).first->second;
        if (pos >= (int)list.size()) pos = -1;
        Bind bind = e.bind == Bind::Same ? (pos >= 0 ? list[pos].bind : Bind::Weak) : e.bind;
        std::vector<PatternValue> add;
        for (const Value& v : e.values) add.push_back({v, bind});
        int n = (int)add.size();
        switch (e.mode) {
        case Mode::Assign:
            if (pos >= 0) {
                list.erase(list.begin() + pos);
                list.insert(list.begin() + pos, add.begin(), add.end());
                if (!n) pos = -1;
            } else {
                list = add;
            }
            break;
        case Mode::AssignReplace: list = add, pos = -1; break;
        case Mode::Prepend:
            if (pos >= 0) list.insert(list.begin() + pos, add.begin(), add.end()), pos += n;
            else list.insert(list.begin(), add.begin(), add.end());
            break;
        case Mode::PrependFirst:
            list.insert(list.begin(), add.begin(), add.end());
            if (pos >= 0) pos += n;
            break;
        case Mode::Append:
            if (pos >= 0) list.insert(list.begin() + pos + 1, add.begin(), add.end());
            else list.insert(list.end(), add.begin(), add.end());
            break;
        case Mode::AppendLast: list.insert(list.end(), add.begin(), add.end()); break;
        case Mode::Delete:
            if (pos >= 0) list.erase(list.begin() + pos), pos = -1;
            break;
        case Mode::DeleteAll: list.clear(), pos = -1; break;
        }
        if (list.empty()) p.el.erase(e.object), position[e.object] = -1;
    }
}

void Substitute(Pattern& p, const Config& c) {
    for (const std::string& l : DefaultLangs()) p.el["lang"].push_back({Value::String(l), Bind::Weak});
    for (const Rule& r : c.rules) ApplyRule(r, p);
    // FcDefaultSubstitute's part that matching reads.
    if (!p.el.count("weight")) p.el["weight"].push_back({Value::Number(80), Bind::Strong});
    if (!p.el.count("slant")) p.el["slant"].push_back({Value::Number(0), Bind::Strong});
    if (!p.el.count("width")) p.el["width"].push_back({Value::Number(100), Bind::Strong});
}

double First(const Pattern& p, const char* object, double fallback) {
    auto it = p.el.find(object);
    if (it == p.el.end()) return fallback;
    for (const PatternValue& v : it->second)
        if (v.v.type == Value::Num) return v.v.n;
    return fallback;
}

double RangeDistance(double v, float lo, float hi) { return v < lo ? lo - v : v > hi ? v - hi : 0; }

uint32_t Missing(const std::vector<Range>& need, const Face& f) {
    uint32_t missing = 0;
    for (const Range& r : need)
        for (uint32_t c = r.first; c <= r.last && c - r.first < 0x10000; ++c) missing += !f.Covers(c);
    return missing;
}

int LangCompare(const std::string& lang, uint64_t langs) {
    int i = LangIndex(lang);
    if (i >= 0 && (langs >> i & 1)) return 0;
    std::string base = lang.substr(0, lang.find('-'));
    for (size_t k = 0; k < kLangCount; ++k) {
        if (!(langs >> k & 1)) continue;
        std::string other = kOrthographies[k].lang;
        if (other.substr(0, other.find('-')) == base) return 1;
    }
    return 2;
}

// FcFontMatch's comparison, in its priority order: variable, color, charset, strong family,
// language, weak family, symbol, spacing, slant, weight, width, decorative, font version (with
// FcDefaultSubstitute's defaults: not variable, not symbol, not decorative, the newest version).
// Ties go to the face found first.
constexpr int kScores = 13;
struct Scorer {
    struct Fam {
        std::string name;
        bool strong;
        int j;
    };
    std::vector<Fam> families;
    std::vector<std::pair<std::string, int>> langs;
    std::vector<Range> charset;
    int color = -1;  // -1 when the pattern does not ask
    double spacing = -1, weight, slant, width;

    explicit Scorer(const Pattern& p) {
        if (auto it = p.el.find("family"); it != p.el.end())
            for (size_t j = 0; j < it->second.size(); ++j)
                if (it->second[j].v.type == Value::Str)
                    families.push_back({Squash(it->second[j].v.s), it->second[j].bind == Bind::Strong, (int)j});
        if (auto it = p.el.find("lang"); it != p.el.end())
            for (size_t j = 0; j < it->second.size(); ++j)
                if (it->second[j].v.type == Value::Str) langs.push_back({NormalizeLang(it->second[j].v.s), (int)j});
        if (auto it = p.el.find("color"); it != p.el.end())
            for (const PatternValue& v : it->second)
                if (v.v.type == Value::Bool) {
                    color = v.v.b;
                    break;
                }
        charset = p.charset;
        spacing = First(p, "spacing", -1);
        weight = First(p, "weight", 80);
        slant = First(p, "slant", 0);
        width = First(p, "width", 100);
    }

    void Score(const IndexedFace& x, double out[kScores]) const {
        const Face& f = *x.face;
        *out++ = f.variableOnly ? 1 : 0;
        out[0] = color < 0 ? 0 : (f.color != (color == 1));
        out[1] = charset.empty() ? 0 : Missing(charset, f);
        double strong = 1e99, weak = 1e99;
        for (const Fam& fam : families) {
            bool same = std::find(x.squashed.begin(), x.squashed.end(), fam.name) != x.squashed.end();
            double v = (same ? 0 : 1000) + fam.j;
            (fam.strong ? strong : weak) = std::min(fam.strong ? strong : weak, v);
        }
        out[2] = strong;
        double lang = langs.empty() ? 0 : 1e99;
        for (auto& [l, j] : langs) lang = std::min(lang, LangCompare(l, f.langs) * 1000.0 + j);
        out[3] = lang;
        out[4] = weak;
        out[5] = f.symbol ? 1 : 0;
        out[6] = spacing < 0 ? 0 : std::fabs(spacing - f.spacing);
        out[7] = RangeDistance(slant, f.slantMin, f.slantMax);
        out[8] = RangeDistance(weight, f.weightMin, f.weightMax);
        out[9] = RangeDistance(width, f.widthMin, f.widthMax);
        out[10] = f.decorative ? 1 : 0;
        out[11] = 2147483647.0 - f.version;
    }
};

std::mutex g_lock;
std::unique_ptr<Index> g_index;

Index& Current() {
    if (!g_index) {
        g_index = std::make_unique<Index>();
        Build(g_index.get());
    }
    return *g_index;
}

}  // namespace

bool FindFont(const std::string& pattern, Match* out) {
    std::lock_guard<std::mutex> lock(g_lock);
    Index& index = Current();
    Pattern p = ParsePattern(pattern);
    Substitute(p, index.config);
    Scorer scorer(p);
    const IndexedFace* best = nullptr;
    double bestScore[kScores] = {}, score[kScores];
    for (const IndexedFace& x : index.faces) {
        scorer.Score(x, score);
        if (!best || std::lexicographical_compare(score, score + kScores, bestScore, bestScore + kScores)) {
            best = &x;
            std::copy(score, score + kScores, bestScore);
        }
    }
    if (!best) return false;
    if (out) *out = {*best->file, best->face->index, best->face->family};
    return true;
}

std::vector<std::string> Families(const std::string& pattern) {
    std::lock_guard<std::mutex> lock(g_lock);
    Index& index = Current();
    Pattern p = ParsePattern(pattern);
    Substitute(p, index.config);
    std::vector<std::string> out;
    for (const PatternValue& v : p.el["family"])
        if (v.v.type == Value::Str) out.push_back(v.v.s + (v.bind == Bind::Strong ? " (strong)" : ""));
    for (const PatternValue& v : p.el["lang"])
        if (v.v.type == Value::Str) out.push_back("lang " + v.v.s + (v.bind == Bind::Strong ? " (strong)" : ""));
    return out;
}

IndexInfo Info() {
    std::lock_guard<std::mutex> lock(g_lock);
    return Current().info;
}

void Reload() {
    std::lock_guard<std::mutex> lock(g_lock);
    g_index.reset();
}

}  // namespace lx::fonts
