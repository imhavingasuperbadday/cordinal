// The shaper (shaper.h): Unicode properties and the bidi pass, the OpenType layout engine over
// GSUB, GPOS and GDEF, and the script-specific steps HarfBuzz's shapers take for Arabic,
// Devanagari and its relatives, Thai and Lao.
#include "shaper.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace lx::type {

namespace {

#include "unicode_tables.inc"

template <size_t N>
uint32_t RunValue(const uint32_t (&runs)[N], uint32_t ch, int shift) {
    const uint32_t* it = std::upper_bound(std::begin(runs), std::end(runs), ch, [shift](uint32_t c, uint32_t r) { return c < (r >> shift); });
    if (it == std::begin(runs)) return 0;
    return it[-1] & ((1u << shift) - 1);
}

constexpr uint32_t Iso(const char (&t)[5]) { return Tag(t); }

uint8_t ScriptId(uint32_t ch) { return (uint8_t)RunValue(kScriptRuns, ch, 8); }
uint32_t Props(uint32_t ch) { return RunValue(kPropRuns, ch, 8); }
int MarkKind(uint32_t ch) { return Props(ch) & 3; }
int Joining(uint32_t ch) { return Props(ch) >> 2 & 7; }  // U R D C L T
// Canonical combining class, with HarfBuzz's changes for shaping: Hebrew points in their
// visual stacking order, Arabic shadda before the other harakat, the Telugu length marks as
// starters, and Thai, Lao and Tibetan vowels around their tone marks.
uint8_t Ccc(uint32_t ch) {
    if (ch < 0x300) return 0;
    uint8_t c = (uint8_t)RunValue(kCccRuns, ch, 8);
    static const uint8_t kModified[] = {
        // 10-26: Hebrew
        22, 15, 16, 17, 23, 18, 19, 20, 21, 14, 24, 12, 25, 13, 10, 11, 26,
        // 27-35: Arabic
        28, 29, 30, 31, 32, 33, 27, 34, 35};
    if (c >= 10 && c <= 35) return kModified[c - 10];
    switch (c) {
    case 84: case 91: return 0;
    case 103: return 3;
    case 130: return 132;
    case 132: return 131;
    default: return c;
    }
}

enum Bidi { L, R, AL, EN, ES, ET, AN, CS, NSM, BN, B, S, WS, ON, LRE, LRO, RLE, RLO, PDF, LRI, RLI, FSI, PDI };
Bidi BidiClass(uint32_t ch) { return (Bidi)RunValue(kBidiRuns, ch, 5); }

uint32_t Mirror(uint32_t ch) {
    if (ch > 0xFFFF) return 0;
    const uint32_t* it = std::lower_bound(std::begin(kMirrors), std::end(kMirrors), ch << 16);
    return it != std::end(kMirrors) && (*it >> 16) == ch ? (*it & 0xFFFF) : 0;
}

uint32_t Compose(uint32_t a, uint32_t b) {
    size_t lo = 0, hi = sizeof(kCompositions) / sizeof(kCompositions[0]) / 3;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        const uint32_t* e = kCompositions + mid * 3;
        if (e[0] < a || (e[0] == a && e[1] < b)) lo = mid + 1;
        else hi = mid;
    }
    const uint32_t* e = kCompositions + lo * 3;
    return lo < sizeof(kCompositions) / sizeof(kCompositions[0]) / 3 && e[0] == a && e[1] == b ? e[2] : 0;
}

bool Decompose(uint32_t c, uint32_t* a, uint32_t* b) {
    // Hangul syllables decompose arithmetically; the rest by the composition table, searched by result.
    if (c >= 0xAC00 && c < 0xAC00 + 11172) {
        uint32_t s = c - 0xAC00;
        if (s % 28) *a = 0xAC00 + s / 28 * 28, *b = 0x11A7 + s % 28;
        else *a = 0x1100 + s / 588, *b = 0x1161 + s % 588 / 28;
        return true;
    }
    for (size_t i = 0; i < sizeof(kCompositions) / sizeof(kCompositions[0]); i += 3)
        if (kCompositions[i + 2] == c) return *a = kCompositions[i], *b = kCompositions[i + 1], true;
    return false;
}

uint8_t Indic(uint32_t ch) { return ch >= kIndicFirst && ch <= kIndicLast ? kIndic[ch - kIndicFirst] : 0; }

}  // namespace

uint32_t ScriptOf(uint32_t ch) {
    uint8_t id = ScriptId(ch);
    const char* c = kScriptCodes[id];
    return (uint32_t)(uint8_t)c[0] << 24 | (uint32_t)(uint8_t)c[1] << 16 | (uint32_t)(uint8_t)c[2] << 8 | (uint8_t)c[3];
}
bool IsDefaultIgnorable(uint32_t ch) { return Props(ch) & 32; }
bool IsMark(uint32_t ch) { return MarkKind(ch) != 0; }
bool IsEmojiPresentation(uint32_t ch) { return Props(ch) & 64; }
bool JoinsGraphemes(uint32_t ch) {
    return IsMark(ch) || ch == 0x200D || (ch >= 0xFE00 && ch <= 0xFE0F) || (ch >= 0xE0100 && ch <= 0xE01EF) ||
           (ch >= 0x1F3FB && ch <= 0x1F3FF) || (ch >= 0xE0020 && ch <= 0xE007F);
}

// ---- Bidi and script runs ----------------------------------------------------------------------

std::vector<Run> Itemize(const std::vector<uint32_t>& text, bool* rtlParagraph, int paragraph) {
    size_t n = text.size();
    std::vector<Bidi> types(n);
    for (size_t i = 0; i < n; ++i) {
        Bidi t = BidiClass(text[i]);
        if (t >= LRE && t <= PDF) t = BN;  // explicit embeddings and overrides are not modelled: dropped
        if (t >= LRI) t = ON;              // isolates read as neutrals
        types[i] = t;
    }
    // P2-P3: the first strong character sets the paragraph's direction.
    int para = paragraph == 1 ? 1 : 0;
    for (Bidi t : types)
        if (paragraph < 0 && (t == L || t == R || t == AL)) {
            para = t == L ? 0 : 1;
            break;
        }
    *rtlParagraph = para == 1;
    Bidi sos = para ? R : L;
    // W1-W7.
    Bidi prev = sos;
    for (size_t i = 0; i < n; ++i) {
        if (types[i] == NSM) types[i] = prev;
        if (types[i] != BN) prev = types[i];
    }
    Bidi strong = sos;
    for (size_t i = 0; i < n; ++i) {
        Bidi t = types[i];
        if (t == L || t == R || t == AL) strong = t;
        if (t == EN && strong == AL) types[i] = AN;
    }
    for (auto& t : types)
        if (t == AL) t = R;
    for (size_t i = 1; i + 1 < n; ++i) {
        Bidi t = types[i], a = types[i - 1], b = types[i + 1];
        if (t == ES && a == EN && b == EN) types[i] = EN;
        else if (t == CS && a == b && (a == EN || a == AN)) types[i] = a;
    }
    for (size_t i = 0; i < n; ++i) {
        if (types[i] != ET) continue;
        size_t j = i;
        while (j < n && (types[j] == ET || types[j] == BN)) ++j;
        bool en = (i > 0 && types[i - 1] == EN) || (j < n && types[j] == EN);
        for (size_t k = i; k < j; ++k)
            if (en && types[k] == ET) types[k] = EN;
        i = j - (j > i ? 1 : 0);
    }
    for (auto& t : types)
        if (t == ES || t == ET || t == CS) t = ON;
    strong = sos;
    for (auto& t : types) {
        if (t == L || t == R) strong = t;
        if (t == EN && strong == L) t = L;
    }
    auto dirOf = [](Bidi t) { return t == L ? L : (t == R || t == EN || t == AN) ? R : ON; };
    // N0: a pair of brackets takes the direction of the strong text inside it (BD16 pairs them,
    // at most 63 deep), or of the text before it when only the other direction is inside.
    {
        auto canonical = [](uint32_t ch) { return ch == 0x2329 ? 0x3008u : ch == 0x232A ? 0x3009u : ch; };
        std::vector<std::pair<size_t, size_t>> pairs;
        std::vector<std::pair<uint32_t, size_t>> open;  // closing bracket wanted, position
        for (size_t i = 0; i < n; ++i) {
            if (types[i] != ON) continue;
            uint32_t ch = canonical(text[i]);
            const uint32_t* it = std::lower_bound(std::begin(kBrackets), std::end(kBrackets), ch << 16);
            if (it != std::end(kBrackets) && (*it >> 16) == ch) {
                if (open.size() == 63) break;
                open.push_back({canonical(*it & 0xFFFF), i});
                continue;
            }
            for (size_t k = open.size(); k-- > 0;)
                if (open[k].first == ch) {
                    pairs.push_back({open[k].second, i});
                    open.resize(k);
                    break;
                }
        }
        std::sort(pairs.begin(), pairs.end());
        Bidi e = para ? R : L, o = para ? L : R;
        for (auto [a, b] : pairs) {
            bool inE = false, inO = false;
            for (size_t k = a + 1; k < b; ++k) {
                Bidi d = dirOf(types[k]);
                inE |= d == e, inO |= d != ON && d != e;
            }
            Bidi set;
            if (inE) set = e;
            else if (inO) {
                Bidi before = sos;
                for (size_t k = a; k-- > 0;)
                    if (dirOf(types[k]) != ON) {
                        before = dirOf(types[k]);
                        break;
                    }
                set = before == o ? o : e;
            } else continue;
            for (size_t at : {a, b}) {
                types[at] = set;
                for (size_t k = at + 1; k < n; ++k) {
                    Bidi c = BidiClass(text[k]);
                    if (c == NSM) types[k] = set;
                    else if (c != BN) break;
                }
            }
        }
    }
    // N1-N2: neutrals between characters of one direction take it, others the paragraph's.
    for (size_t i = 0; i < n;) {
        if (dirOf(types[i]) != ON) {
            ++i;
            continue;
        }
        size_t j = i;
        while (j < n && dirOf(types[j]) == ON) ++j;
        Bidi before = i == 0 ? sos : dirOf(types[i - 1]);
        Bidi after = j == n ? sos : dirOf(types[j]);
        Bidi set = before == after ? before : sos;
        for (size_t k = i; k < j; ++k) types[k] = set == L ? L : R;
        i = j;
    }
    // I1-I2.
    std::vector<int> levels(n, para);
    for (size_t i = 0; i < n; ++i) {
        Bidi t = types[i];
        if (para == 0) levels[i] += t == R ? 1 : (t == AN || t == EN) ? 2 : 0;
        else levels[i] += (t == L || t == EN || t == AN) ? 1 : 0;
    }
    // L1: trailing whitespace and separators return to the paragraph level.
    for (size_t i = n; i-- > 0;) {
        Bidi orig = BidiClass(text[i]);
        if (orig == WS || orig == BN || orig >= LRI || orig == S || orig == B || (orig >= LRE && orig <= PDF)) levels[i] = para;
        else break;
    }
    for (size_t i = 0; i < n; ++i) {
        Bidi orig = BidiClass(text[i]);
        if (orig == S || orig == B) {
            levels[i] = para;
            for (size_t k = i; k-- > 0;) {
                Bidi o = BidiClass(text[k]);
                if (o == WS || o >= LRI || o == BN) levels[k] = para;
                else break;
            }
        }
    }
    // Scripts: common and inherited characters take their neighbours' script.
    std::vector<uint8_t> scripts(n);
    uint8_t last = kScZyyy;
    for (size_t i = 0; i < n; ++i) {
        uint8_t s = ScriptId(text[i]);
        if (s == kScZinh || s == kScZyyy || s == kScZzzz) s = last;
        scripts[i] = s;
        if (s != kScZyyy) last = s;
    }
    uint8_t next = kScZyyy;
    for (size_t i = n; i-- > 0;) {
        if (scripts[i] == kScZyyy) scripts[i] = next;
        else next = scripts[i];
    }
    // Hiragana, Katakana and Han share a run, as CJK text mixes them.
    auto group = [](uint8_t s) { return (s == kScHira || s == kScKana) ? kScHani : s; };
    std::vector<Run> runs;
    for (size_t i = 0; i < n;) {
        size_t j = i + 1;
        while (j < n && levels[j] == levels[i] && group(scripts[j]) == group(scripts[i])) ++j;
        Run r;
        r.start = i, r.end = j, r.level = levels[i];
        const char* c = kScriptCodes[scripts[i]];
        r.script = (uint32_t)(uint8_t)c[0] << 24 | (uint32_t)(uint8_t)c[1] << 16 | (uint32_t)(uint8_t)c[2] << 8 | (uint8_t)c[3];
        runs.push_back(r);
        i = j;
    }
    return runs;
}

std::vector<size_t> VisualOrder(const std::vector<Run>& runs) {
    std::vector<size_t> order(runs.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    int maxLevel = 0, minOdd = 127;
    for (auto& r : runs) {
        maxLevel = std::max(maxLevel, r.level);
        if (r.level & 1) minOdd = std::min(minOdd, r.level);
    }
    for (int level = maxLevel; level >= minOdd && level > 0; --level) {
        for (size_t i = 0; i < order.size();) {
            if (runs[order[i]].level < level) {
                ++i;
                continue;
            }
            size_t j = i;
            while (j < order.size() && runs[order[j]].level >= level) ++j;
            std::reverse(order.begin() + i, order.begin() + j);
            i = j;
        }
    }
    return order;
}

// ---- OpenType layout ---------------------------------------------------------------------------

namespace {

int CoverageIndex(Bytes t, uint16_t g) {
    uint16_t format = t.U16(0), count = t.U16(2);
    if (format == 1) {
        size_t lo = 0, hi = std::min<size_t>(count, t.n >= 4 ? (t.n - 4) / 2 : 0);
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            uint16_t v = t.U16(4 + mid * 2);
            if (v == g) return (int)mid;
            if (v < g) lo = mid + 1;
            else hi = mid;
        }
    } else if (format == 2) {
        size_t lo = 0, hi = std::min<size_t>(count, t.n >= 4 ? (t.n - 4) / 6 : 0);
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            size_t r = 4 + mid * 6;
            if (t.U16(r + 2) < g) lo = mid + 1;
            else hi = mid;
        }
        size_t r = 4 + lo * 6;
        if (lo < count && t.Has(r, 6) && t.U16(r) <= g && g <= t.U16(r + 2)) return t.U16(r + 4) + (g - t.U16(r));
    }
    return -1;
}

uint16_t ClassOf(Bytes t, uint16_t g) {
    uint16_t format = t.U16(0);
    if (format == 1) {
        uint16_t start = t.U16(2), count = t.U16(4);
        return g >= start && g - start < count ? t.U16(6 + (size_t)(g - start) * 2) : 0;
    }
    if (format == 2) {
        uint16_t count = t.U16(2);
        size_t lo = 0, hi = std::min<size_t>(count, t.n >= 4 ? (t.n - 4) / 6 : 0);
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (t.U16(4 + mid * 6 + 2) < g) lo = mid + 1;
            else hi = mid;
        }
        size_t r = 4 + lo * 6;
        if (lo < count && t.Has(r, 6) && t.U16(r) <= g) return t.U16(r + 4);
    }
    return 0;
}

Bytes At(Bytes base, size_t offsetField) {
    uint16_t off = base.U16(offsetField);
    return off ? base.Sub(off) : Bytes{};
}

enum : uint8_t { kUnclassified = 0, kBase = 1, kLigature = 2, kMark = 3, kComponent = 4 };
enum : uint16_t { kRightToLeft = 1, kIgnoreBase = 2, kIgnoreLigatures = 4, kIgnoreMarks = 8, kUseMarkFilteringSet = 0x10 };

struct Gdef {
    Bytes classes, markAttach, markSets;
    explicit Gdef(Bytes t) {
        if (t.U16(0) != 1) return;
        classes = At(t, 4);
        markAttach = At(t, 10);
        if (t.U16(2) >= 2) markSets = At(t, 12);
    }
    bool HasClasses() const { return !classes.Empty(); }
    bool InSet(uint16_t set, uint16_t g) const {
        if (set >= markSets.U16(2)) return false;
        return CoverageIndex(markSets.Sub(markSets.U32(4 + (size_t)set * 4)), g) >= 0;
    }
};

struct Info {
    uint32_t cp = 0, cluster = 0, mask = 0;
    uint16_t glyph = 0;
    uint8_t cls = kBase, markClass = 0;
    uint8_t ligId = 0, ligComp = 0, ligComps = 0;
    uint8_t ignorable = 0;  // 1 a default ignorable, 2 ZWNJ, 3 ZWJ
    uint8_t cat = 0, pos = 0, syllable = 0;  // the Indic category and position, and syllable number
    bool multiplied = false;
};

struct Pos {
    int32_t xa = 0, ya = 0, xo = 0, yo = 0;
    int16_t chain = 0;  // the attached-to glyph's offset from this one
    bool mark = false;  // attached as a mark (rather than cursively)
};

struct Lookup {
    uint16_t type = 0, flag = 0, filterSet = 0;
    std::vector<Bytes> subtables;
};

struct Table {
    Bytes t;
    std::vector<Lookup> lookups;
    bool gpos = false;
    void Load(Bytes table, bool isGpos) {
        t = table;
        gpos = isGpos;
        if (t.U16(0) != 1) return;
        Bytes list = At(t, 8);
        // Counts are held to the bytes that can hold them, and the subtables of all lookups to
        // one per byte of the table, as lookups may share one list of subtables.
        size_t count = std::min<size_t>(list.U16(0), list.n > 2 ? (list.n - 2) / 2 : 0), budget = t.n;
        for (size_t i = 0; i < count; ++i) {
            Bytes l = At(list, 2 + i * 2);
            Lookup lk;
            lk.type = l.U16(0), lk.flag = l.U16(2);
            uint16_t declared = lk.type;
            size_t subs = std::min<size_t>({l.U16(4), l.n > 6 ? (l.n - 6) / 2 : 0, budget});
            budget -= subs;
            for (size_t s = 0; s < subs; ++s) {
                Bytes st = At(l, 6 + (size_t)s * 2);
                uint16_t type = declared;
                if ((!isGpos && type == 7) || (isGpos && type == 9)) {  // extension
                    if (st.U16(0) != 1) continue;
                    type = st.U16(2);
                    st = st.Sub(st.U32(4));
                    if (lk.subtables.empty()) lk.type = type;
                    else if (type != lk.type) continue;
                }
                lk.subtables.push_back(st);
            }
            if (lk.flag & kUseMarkFilteringSet) lk.filterSet = l.U16(6 + (size_t)l.U16(4) * 2);
            lookups.push_back(std::move(lk));
        }
    }
    // The script's default language system, trying `tags` then DFLT, dflt and latn.
    Bytes LangSys(const std::vector<uint32_t>& tags) const {
        Bytes scripts = At(t, 4);
        uint16_t count = scripts.U16(0);
        std::vector<uint32_t> want = tags;
        for (uint32_t d : {Tag("DFLT"), Tag("dflt"), Tag("latn")}) want.push_back(d);
        for (uint32_t tag : want)
            for (uint16_t i = 0; i < count; ++i)
                if (scripts.U32(2 + (size_t)i * 6) == tag) {
                    Bytes s = scripts.Sub(scripts.U16(2 + (size_t)i * 6 + 4));
                    Bytes lang = At(s, 0);
                    if (!lang.Empty()) return lang;
                    if (s.U16(2)) return s.Sub(s.U16(4 + 4));  // no default: the first language
                }
        return {};
    }
    // Lookup indices of feature `tag` in the language system, and of its required feature.
    std::vector<uint16_t> FeatureLookups(Bytes lang, uint32_t tag, bool* found = nullptr) const {
        std::vector<uint16_t> out;
        Bytes features = At(t, 6);
        // Counts go no further than the table's bytes, and each feature record is read once, so a
        // hostile count can't multiply into billions of lookups.
        size_t n = std::min<size_t>(lang.U16(4), lang.n > 6 ? (lang.n - 6) / 2 : 0);
        std::vector<bool> seen(features.U16(0));
        for (size_t i = 0; i < n; ++i) {
            uint16_t index = lang.U16(6 + i * 2);
            if (index >= seen.size() || seen[index] || features.U32(2 + (size_t)index * 6) != tag) continue;
            seen[index] = true;
            if (found) *found = true;
            AppendLookups(features.Sub(features.U16(2 + (size_t)index * 6 + 4)), &out);
        }
        return out;
    }
    std::vector<uint16_t> RequiredLookups(Bytes lang) const {
        std::vector<uint16_t> out;
        uint16_t index = lang.U16(2);
        if (index == 0xFFFF || lang.Empty()) return out;
        Bytes features = At(t, 6);
        if (index >= features.U16(0)) return out;
        AppendLookups(features.Sub(features.U16(2 + (size_t)index * 6 + 4)), &out);
        return out;
    }
    void AppendLookups(Bytes feature, std::vector<uint16_t>* out) const {
        size_t count = std::min<size_t>(feature.U16(2), feature.n > 4 ? (feature.n - 4) / 2 : 0);
        for (size_t k = 0; k < count; ++k)
            if (uint16_t l = feature.U16(4 + k * 2); l < lookups.size()) out->push_back(l);
    }
};

struct Applied {
    uint16_t lookup;
    uint32_t mask;
    bool autoZwj = true, autoZwnj = true;
    bool perSyllable = false;  // context stays within the glyph's syllable (Indic features)
};

}  // namespace

// The features of one script and direction, compiled to lookups in stages.
struct Shaper::Plan {
    enum Kind { kDefault, kArabic, kIndic, kThai, kHangul } kind = kDefault;
    Table gsub, gpos;
    std::vector<std::vector<Applied>> gsubStages, gposStages;
    std::vector<int> gsubPauses;  // the shaper step after each GSUB stage (Indic reordering)
    std::map<uint32_t, uint32_t> masks;  // feature tag to its mask bit
    uint32_t globalMask = 1;
    bool arabicFallback = false, hasGposKern = false, hasGposMarks = false;
    Bytes langGsub, langGpos;
    uint32_t Mask(uint32_t tag) const {
        auto it = masks.find(tag);
        return it == masks.end() ? 0 : it->second;
    }
};

namespace {

// Per-shaping-call state for applying lookups.
struct Ctx {
    const Shaper::Plan& plan;
    const Table& table;
    const Gdef& gdef;
    const ShapeFont& font;
    std::vector<Info>& buf;
    std::vector<Pos>* pos;
    uint32_t lookupMask = ~0u;
    uint16_t flag = 0, filterSet = 0;
    bool autoZwj = true, autoZwnj = true;
    int depth = 0;
    uint8_t* nextLigId;
    uint8_t syllable = 0;  // when set, matching stops at glyphs of other syllables
    size_t next = 0;  // the index to continue at after an applied lookup
    int ops = 0;      // a work budget against pathological fonts

    int32_t ScaleX(int v) const { return (int32_t)(((int64_t)v * (((int64_t)font.emX << 16) / font.face->unitsPerEm) + 32768) >> 16); }
    int32_t ScaleY(int v) const { return (int32_t)(((int64_t)v * (((int64_t)font.emY << 16) / font.face->unitsPerEm) + 32768) >> 16); }

    bool Ignored(const Info& g, uint16_t f, uint16_t set) const {
        if (g.cls == kBase && (f & kIgnoreBase)) return true;
        if (g.cls == kLigature && (f & kIgnoreLigatures)) return true;
        if (g.cls == kMark) {
            if (f & kIgnoreMarks) return true;
            if ((f & kUseMarkFilteringSet) && !gdef.InSet(set, g.glyph)) return true;
            if ((f & 0xFF00) && (f >> 8) != g.markClass) return true;
        }
        return false;
    }
    enum Skip { kNo, kYes, kMaybe };
    Skip MaySkip(const Info& g, bool context) const {
        if (Ignored(g, flag, filterSet)) return kYes;
        bool ignoreZwnj = table.gpos || (context && autoZwnj), ignoreZwj = table.gpos || autoZwj;
        if (g.ignorable && !(g.ignorable == 2 && !ignoreZwnj) && !(g.ignorable == 3 && !ignoreZwj)) return kMaybe;
        return kNo;
    }
    // The next (dir 1) or previous (dir -1) glyph matching `match`, skipping what the lookup ignores.
    template <class F>
    int Find(int from, int dir, bool context, F&& match) const {
        uint32_t mask = context ? ~0u : lookupMask;
        for (int j = from + dir; j >= 0 && j < (int)buf.size(); j += dir) {
            const Info& g = buf[j];
            Skip skip = MaySkip(g, context);
            if (skip == kYes) continue;
            if (syllable && g.syllable != syllable) {
                if (skip == kNo) return -1;
                continue;
            }
            bool m = (g.mask & mask) && match(g.glyph);
            if (m) return j;
            if (skip == kNo) return -1;
        }
        return -1;
    }
    void SetClass(Info& g, uint16_t glyph, bool ligature) const {
        g.glyph = glyph;
        if (gdef.HasClasses()) {
            uint16_t c = ClassOf(gdef.classes, glyph);
            g.cls = c >= 1 && c <= 4 ? (uint8_t)c : (uint8_t)(ligature ? kLigature : kUnclassified);
            g.markClass = (uint8_t)ClassOf(gdef.markAttach, glyph);
        } else if (ligature) {
            g.cls = kLigature;
        }
    }
};

bool ApplyLookup(Ctx& c, uint16_t index, size_t i);

// A sequence rule's matched input positions, and nested lookups applied at them.
bool ApplyNested(Ctx& c, std::vector<int>& match, Bytes records, uint16_t count) {
    if (c.depth >= 6) return false;
    int end = match.back() + 1;
    for (uint16_t r = 0; r < count; ++r) {
        uint16_t seq = records.U16((size_t)r * 4), lookup = records.U16((size_t)r * 4 + 2);
        if (seq >= match.size() || lookup >= c.table.lookups.size()) continue;
        int at = match[seq];
        if (at < 0 || at >= (int)c.buf.size()) continue;
        const Lookup& lk = c.table.lookups[lookup];
        if (c.Ignored(c.buf[at], lk.flag, lk.filterSet)) continue;
        size_t before = c.buf.size();
        Ctx sub = c;
        sub.depth = c.depth + 1;
        size_t keepNext = c.next;
        ApplyLookup(sub, lookup, (size_t)at);
        c.ops = sub.ops;
        c.next = keepNext;
        int delta = (int)c.buf.size() - (int)before;
        if (delta) {
            end += delta;
            for (size_t k = seq + 1; k < match.size(); ++k) {
                match[k] += delta;
                if (match[k] <= at) match[k] = at + 1;
            }
        }
    }
    c.next = (size_t)std::max(end, match[0] + 1);
    return true;
}

// Matches a sequence of `count` inputs after position i (the first already matched) by `eq`,
// a backtrack before it and a lookahead after the last input.
template <class In, class Back, class Ahead>
bool MatchContext(Ctx& c, size_t i, uint16_t inputCount, In&& in, uint16_t backCount, Back&& back, uint16_t aheadCount, Ahead&& ahead,
                  std::vector<int>* match) {
    match->assign(1, (int)i);
    for (uint16_t k = 1; k < inputCount; ++k) {
        int j = c.Find(match->back(), 1, false, [&](uint16_t g) { return in(k, g); });
        if (j < 0) return false;
        match->push_back(j);
    }
    int b = (int)i;
    for (uint16_t k = 0; k < backCount; ++k) {
        b = c.Find(b, -1, true, [&](uint16_t g) { return back(k, g); });
        if (b < 0) return false;
    }
    int a = match->back();
    for (uint16_t k = 0; k < aheadCount; ++k) {
        a = c.Find(a, 1, true, [&](uint16_t g) { return ahead(k, g); });
        if (a < 0) return false;
    }
    return true;
}

// Context (GSUB 5, GPOS 7) and chained context (GSUB 6, GPOS 8) lookups, formats 1-3.
bool ApplyContext(Ctx& c, Bytes st, bool chained, size_t i) {
    uint16_t g = c.buf[i].glyph;
    uint16_t format = st.U16(0);
    std::vector<int> match;
    if (format == 1 || format == 2) {
        int cov = CoverageIndex(At(st, 2), g);
        if (cov < 0) return false;
        Bytes backDef, inDef, aheadDef;
        size_t setsAt;
        uint16_t setIndex;
        if (format == 1) {
            setsAt = 4, setIndex = (uint16_t)cov;
        } else if (chained) {
            backDef = At(st, 4), inDef = At(st, 6), aheadDef = At(st, 8), setsAt = 10;
            setIndex = ClassOf(inDef, g);
        } else {
            inDef = At(st, 4), setsAt = 6;
            setIndex = ClassOf(inDef, g);
        }
        if (setIndex >= st.U16(setsAt)) return false;
        Bytes set = At(st, setsAt + 2 + (size_t)setIndex * 2);
        uint16_t rules = set.U16(0);
        for (uint16_t r = 0; r < rules; ++r) {
            if (++c.ops > 200000) return false;
            Bytes rule = At(set, 2 + (size_t)r * 2);
            auto value = [&](Bytes def, uint16_t gl) -> uint16_t { return format == 1 ? gl : ClassOf(def, gl); };
            size_t at = 0;
            uint16_t backCount = 0, inCount, aheadCount = 0;
            Bytes back, in, ahead;
            if (chained) {
                backCount = rule.U16(0), back = rule.Sub(2), at = 2 + (size_t)backCount * 2;
            }
            inCount = rule.U16(at);
            if (inCount == 0) continue;
            if (!chained) {
                uint16_t lookups = rule.U16(at + 2);
                in = rule.Sub(at + 4);
                Bytes records = rule.Sub(at + 4 + (size_t)(inCount - 1) * 2);
                if (MatchContext(
                        c, i, inCount, [&](uint16_t k, uint16_t gl) { return value(inDef, gl) == in.U16((size_t)(k - 1) * 2); }, 0,
                        [](uint16_t, uint16_t) { return false; }, 0, [](uint16_t, uint16_t) { return false; }, &match))
                    return ApplyNested(c, match, records, lookups);
                continue;
            }
            in = rule.Sub(at + 2);
            at += 2 + (size_t)(inCount - 1) * 2;
            aheadCount = rule.U16(at), ahead = rule.Sub(at + 2);
            at += 2 + (size_t)aheadCount * 2;
            uint16_t lookups = rule.U16(at);
            Bytes records = rule.Sub(at + 2);
            if (MatchContext(
                    c, i, inCount, [&](uint16_t k, uint16_t gl) { return value(inDef, gl) == in.U16((size_t)(k - 1) * 2); }, backCount,
                    [&](uint16_t k, uint16_t gl) { return value(backDef, gl) == back.U16((size_t)k * 2); }, aheadCount,
                    [&](uint16_t k, uint16_t gl) { return value(aheadDef, gl) == ahead.U16((size_t)k * 2); }, &match))
                return ApplyNested(c, match, records, lookups);
        }
        return false;
    }
    if (format == 3) {
        if (!chained) {
            uint16_t inCount = st.U16(2), lookups = st.U16(4);
            if (inCount == 0 || CoverageIndex(At(st, 6), g) < 0) return false;
            Bytes records = st.Sub(6 + (size_t)inCount * 2);
            if (MatchContext(
                    c, i, inCount, [&](uint16_t k, uint16_t gl) { return CoverageIndex(At(st, 6 + (size_t)k * 2), gl) >= 0; }, 0,
                    [](uint16_t, uint16_t) { return false; }, 0, [](uint16_t, uint16_t) { return false; }, &match))
                return ApplyNested(c, match, records, lookups);
            return false;
        }
        uint16_t backCount = st.U16(2);
        size_t at = 4 + (size_t)backCount * 2;
        uint16_t inCount = st.U16(at);
        size_t inAt = at + 2;
        at = inAt + (size_t)inCount * 2;
        uint16_t aheadCount = st.U16(at);
        size_t aheadAt = at + 2;
        at = aheadAt + (size_t)aheadCount * 2;
        uint16_t lookups = st.U16(at);
        Bytes records = st.Sub(at + 2);
        if (inCount == 0 || CoverageIndex(At(st, inAt), g) < 0) return false;
        if (MatchContext(
                c, i, inCount, [&](uint16_t k, uint16_t gl) { return CoverageIndex(At(st, inAt + (size_t)k * 2), gl) >= 0; }, backCount,
                [&](uint16_t k, uint16_t gl) { return CoverageIndex(At(st, 4 + (size_t)k * 2), gl) >= 0; }, aheadCount,
                [&](uint16_t k, uint16_t gl) { return CoverageIndex(At(st, aheadAt + (size_t)k * 2), gl) >= 0; }, &match))
            return ApplyNested(c, match, records, lookups);
    }
    return false;
}

// HarfBuzz's merge_clusters: [a, b) and the neighbours sharing its edge clusters take the least.
void MergeClusters(std::vector<Info>& buf, size_t a, size_t b) {
    b = std::min(b, buf.size());
    if (a >= b || b - a < 2) return;
    uint32_t cluster = buf[a].cluster;
    for (size_t i = a + 1; i < b; ++i) cluster = std::min(cluster, buf[i].cluster);
    if (cluster != buf[b - 1].cluster)
        while (b < buf.size() && buf[b - 1].cluster == buf[b].cluster) ++b;
    if (cluster != buf[a].cluster)
        while (a > 0 && buf[a - 1].cluster == buf[a].cluster) --a;
    for (size_t i = a; i < b; ++i) buf[i].cluster = cluster;
}

// ---- GSUB --------------------------------------------------------------------------------------

bool Ligate(Ctx& c, size_t i, const std::vector<int>& match, uint16_t ligGlyph) {
    std::vector<Info>& buf = c.buf;
    bool allMarks = true;
    for (int m : match) allMarks = allMarks && buf[m].cls == kMark;
    uint8_t id = 0;
    if (!allMarks) {
        id = *c.nextLigId;
        *c.nextLigId = (uint8_t)(*c.nextLigId % 255 + 1);
    }
    // Marks between components remember which component they followed.
    for (size_t k = 1; k < match.size(); ++k)
        for (int j = match[k - 1] + 1; j < match[k]; ++j)
            if (buf[j].cls == kMark && id) buf[j].ligId = id, buf[j].ligComp = (uint8_t)k;
    MergeClusters(buf, i, (size_t)match.back() + 1);
    c.SetClass(buf[i], ligGlyph, true);
    if (allMarks) buf[i].cls = kMark;
    buf[i].ligId = id, buf[i].ligComp = 0, buf[i].ligComps = (uint8_t)std::min<size_t>(match.size(), 255);
    for (size_t k = match.size(); k-- > 1;) buf.erase(buf.begin() + match[k]);
    c.next = i + 1;
    return true;
}

bool ApplyGsub(Ctx& c, const Lookup& lk, size_t i) {
    std::vector<Info>& buf = c.buf;
    uint16_t g = buf[i].glyph;
    for (const Bytes& st : lk.subtables) {
        if (++c.ops > 200000) return false;
        uint16_t format = st.U16(0);
        switch (lk.type) {
        case 1: {
            int cov = CoverageIndex(At(st, 2), g);
            if (cov < 0) continue;
            uint16_t out = format == 1 ? (uint16_t)(g + st.U16(4)) : format == 2 && cov < st.U16(4) ? st.U16(6 + (size_t)cov * 2) : 0;
            if (format != 1 && format != 2) continue;
            if (format == 2 && cov >= st.U16(4)) continue;
            c.SetClass(buf[i], out, false);
            c.next = i + 1;
            return true;
        }
        case 2: {
            int cov = CoverageIndex(At(st, 2), g);
            if (format != 1 || cov < 0 || cov >= st.U16(4)) continue;
            Bytes seq = At(st, 6 + (size_t)cov * 2);
            uint16_t n = seq.U16(0);
            if (n > 64 || buf.size() + n > 65536) continue;  // HarfBuzz caps its buffer growth too
            if (n == 0) {  // deletion
                buf.erase(buf.begin() + i);
                c.next = i;
                return true;
            }
            Info proto = buf[i];
            std::vector<Info> out(n, proto);
            for (uint16_t k = 0; k < n; ++k) {
                c.SetClass(out[k], seq.U16(2 + (size_t)k * 2), false);
                out[k].multiplied = n > 1;
                out[k].ligComp = (uint8_t)std::min<int>(k, 255);
            }
            buf.erase(buf.begin() + i);
            buf.insert(buf.begin() + i, out.begin(), out.end());
            c.next = i + n;
            return true;
        }
        case 3: {
            int cov = CoverageIndex(At(st, 2), g);
            if (format != 1 || cov < 0 || cov >= st.U16(4)) continue;
            Bytes set = At(st, 6 + (size_t)cov * 2);
            if (set.U16(0) == 0) continue;
            c.SetClass(buf[i], set.U16(2), false);
            c.next = i + 1;
            return true;
        }
        case 4: {
            int cov = CoverageIndex(At(st, 2), g);
            if (format != 1 || cov < 0 || cov >= st.U16(4)) continue;
            Bytes set = At(st, 6 + (size_t)cov * 2);
            uint16_t n = set.U16(0);
            for (uint16_t l = 0; l < n; ++l) {
                Bytes lig = At(set, 2 + (size_t)l * 2);
                uint16_t comps = lig.U16(2);
                if (comps == 0 || comps > 64) continue;
                std::vector<int> match{(int)i};
                bool ok = true;
                for (uint16_t k = 1; k < comps && ok; ++k) {
                    uint16_t want = lig.U16(4 + (size_t)(k - 1) * 2);
                    int j = c.Find(match.back(), 1, false, [want](uint16_t gl) { return gl == want; });
                    if (j < 0) ok = false;
                    else match.push_back(j);
                }
                if (ok) return Ligate(c, i, match, lig.U16(0));
            }
            continue;
        }
        case 5: case 6:
            if (ApplyContext(c, st, lk.type == 6, i)) return true;
            continue;
        case 8: {
            if (format != 1 || CoverageIndex(At(st, 2), g) < 0) continue;
            uint16_t backCount = st.U16(4);
            size_t at = 6 + (size_t)backCount * 2;
            uint16_t aheadCount = st.U16(at);
            size_t aheadAt = at + 2;
            at = aheadAt + (size_t)aheadCount * 2;
            uint16_t subCount = st.U16(at);
            int cov = CoverageIndex(At(st, 2), g);
            if (cov >= subCount) continue;
            std::vector<int> match;
            if (MatchContext(
                    c, i, 1, [](uint16_t, uint16_t) { return true; }, backCount,
                    [&](uint16_t k, uint16_t gl) { return CoverageIndex(At(st, 6 + (size_t)k * 2), gl) >= 0; }, aheadCount,
                    [&](uint16_t k, uint16_t gl) { return CoverageIndex(At(st, aheadAt + (size_t)k * 2), gl) >= 0; }, &match)) {
                c.SetClass(buf[i], st.U16(at + 2 + (size_t)cov * 2), false);
                c.next = i;
                return true;
            }
            continue;
        }
        default:
            return false;
        }
    }
    return false;
}

// ---- GPOS --------------------------------------------------------------------------------------

size_t ValueSize(uint16_t format) { return (size_t)__builtin_popcount(format & 0xFF) * 2; }

// A device table's adjustment at the font's size, in 26.6 (hinting deltas, as HarfBuzz applies
// them at a known ppem; variation deltas are 0 at the default instance).
int32_t DeviceDelta(const Ctx& c, Bytes dev, int32_t scale) {
    int ppem = c.font.ppem;
    if (dev.Empty() || ppem <= 0) return 0;
    unsigned start = dev.U16(0), end = dev.U16(2), f = dev.U16(4);
    if (f < 1 || f > 3 || (unsigned)ppem < start || (unsigned)ppem > end) return 0;
    unsigned s = (unsigned)ppem - start;
    unsigned word = dev.U16(6 + (size_t)(s >> (4 - f)) * 2);
    unsigned bits = word >> (16 - (((s & ((1u << (4 - f)) - 1)) + 1) << f));
    unsigned mask = 0xFFFFu >> (16 - (1u << f));
    int delta = (int)(bits & mask);
    if ((unsigned)delta >= ((mask + 1) >> 1)) delta -= (int)mask + 1;
    return (int32_t)((int64_t)delta * scale / ppem);
}

// A value record `v` whose device offsets count from `base`.
void AddValue(const Ctx& c, Bytes base, Bytes v, uint16_t format, Pos& p) {
    size_t at = 0;
    if (format & 1) p.xo += c.ScaleX(v.S16(at)), at += 2;
    if (format & 2) p.yo += c.ScaleY(v.S16(at)), at += 2;
    if (format & 4) p.xa += c.ScaleX(v.S16(at)), at += 2;
    if (format & 8) p.ya += c.ScaleY(v.S16(at)), at += 2;
    auto device = [&](size_t off) { return off ? base.Sub(off) : Bytes(); };
    if (format & 0x10) p.xo += DeviceDelta(c, device(v.U16(at)), c.font.emX), at += 2;
    if (format & 0x20) p.yo += DeviceDelta(c, device(v.U16(at)), c.font.emY), at += 2;
    if (format & 0x40) p.xa += DeviceDelta(c, device(v.U16(at)), c.font.emX), at += 2;
    if (format & 0x80) at += 2;  // vertical advance: horizontal text only
}

// An anchor point in 26.6, unrounded as HarfBuzz keeps it (format 2's contour point reads as format 1).
bool Anchor(const Ctx& c, Bytes a, float* x, float* y) {
    if (a.Empty() || a.U16(0) < 1 || a.U16(0) > 3) return false;
    float upem = (float)c.font.face->unitsPerEm, mx = (float)c.font.emX / upem, my = (float)c.font.emY / upem;
    *x = (float)a.S16(2) * mx;
    *y = (float)a.S16(4) * my;
    if (a.U16(0) == 3) {
        if (uint16_t o = a.U16(6)) *x += (float)DeviceDelta(c, a.Sub(o), c.font.emX);
        if (uint16_t o = a.U16(8)) *y += (float)DeviceDelta(c, a.Sub(o), c.font.emY);
    }
    return true;
}

bool AttachMark(Ctx& c, size_t i, int j, Bytes markArray, int markIndex, Bytes baseAnchor) {
    if (markIndex >= markArray.U16(0)) return false;
    float mx, my, bx, by;
    if (!Anchor(c, At(markArray, 2 + (size_t)markIndex * 4 + 2), &mx, &my) || !Anchor(c, baseAnchor, &bx, &by)) return false;
    Pos& p = (*c.pos)[i];
    p.xo = (int32_t)std::lround(bx - mx);
    p.yo = (int32_t)std::lround(by - my);
    p.chain = (int16_t)(j - (int)i);
    p.mark = true;
    c.next = i + 1;
    return true;
}

bool ApplyGpos(Ctx& c, const Lookup& lk, size_t i) {
    std::vector<Info>& buf = c.buf;
    std::vector<Pos>& pos = *c.pos;
    uint16_t g = buf[i].glyph;
    for (const Bytes& st : lk.subtables) {
        if (++c.ops > 200000) return false;
        uint16_t format = st.U16(0);
        switch (lk.type) {
        case 1: {
            int cov = CoverageIndex(At(st, 2), g);
            if (cov < 0) continue;
            uint16_t vf = st.U16(4);
            if (format == 1) AddValue(c, st, st.Sub(6), vf, pos[i]);
            else if (format == 2 && cov < st.U16(6)) AddValue(c, st, st.Sub(8 + (size_t)cov * ValueSize(vf)), vf, pos[i]);
            else continue;
            c.next = i + 1;
            return true;
        }
        case 2: {
            int cov = CoverageIndex(At(st, 2), g);
            if (cov < 0) continue;
            int j = c.Find((int)i, 1, false, [](uint16_t) { return true; });
            if (j < 0) continue;
            uint16_t vf1 = st.U16(4), vf2 = st.U16(6);
            size_t s1 = ValueSize(vf1), s2 = ValueSize(vf2);
            uint16_t second = buf[j].glyph;
            bool applied = false;
            if (format == 1) {
                if (cov >= st.U16(8)) continue;
                Bytes set = At(st, 10 + (size_t)cov * 2);
                uint16_t n = set.U16(0);
                size_t rec = 2 + s1 + s2;
                size_t lo = 0, hi = n;
                while (lo < hi) {
                    size_t mid = (lo + hi) / 2;
                    uint16_t v = set.U16(2 + mid * rec);
                    if (v == second) {
                        AddValue(c, set, set.Sub(2 + mid * rec + 2), vf1, pos[i]);
                        AddValue(c, set, set.Sub(2 + mid * rec + 2 + s1), vf2, pos[j]);
                        applied = true;
                        break;
                    }
                    if (v < second) lo = mid + 1;
                    else hi = mid;
                }
            } else if (format == 2) {
                uint16_t c1 = ClassOf(At(st, 8), g), c2 = ClassOf(At(st, 10), second);
                uint16_t n1 = st.U16(12), n2 = st.U16(14);
                if (c1 >= n1 || c2 >= n2) continue;
                size_t rec = 16 + ((size_t)c1 * n2 + c2) * (s1 + s2);
                AddValue(c, st, st.Sub(rec), vf1, pos[i]);
                AddValue(c, st, st.Sub(rec + s1), vf2, pos[j]);
                applied = true;
            }
            if (!applied) continue;
            c.next = (size_t)j + (s2 ? 1 : 0);
            return true;
        }
        case 4: case 5: case 6: {
            if (format != 1) continue;
            int markIndex = CoverageIndex(At(st, 2), g);
            if (markIndex < 0) continue;
            uint16_t classCount = st.U16(6);
            Bytes markArray = At(st, 8), baseArray = At(st, 10);
            uint16_t markClass = markArray.U16(2 + (size_t)markIndex * 4);
            if (markClass >= classCount) continue;
            if (lk.type == 4) {
                // The base: the previous glyph that isn't a mark.
                int j = -1;
                for (int k = (int)i - 1; k >= 0; --k) {
                    if (c.Ignored(buf[k], kIgnoreMarks, 0)) continue;
                    j = k;
                    break;
                }
                if (j < 0) continue;
                int baseIndex = CoverageIndex(At(st, 4), buf[j].glyph);
                if (baseIndex < 0 || baseIndex >= baseArray.U16(0)) continue;
                Bytes anchor = At(baseArray, 2 + ((size_t)baseIndex * classCount + markClass) * 2);
                if (AttachMark(c, i, j, markArray, markIndex, anchor)) return true;
            } else if (lk.type == 5) {
                int j = -1;
                for (int k = (int)i - 1; k >= 0; --k) {
                    if (buf[k].cls == kMark) continue;
                    j = k;
                    break;
                }
                if (j < 0) continue;
                int ligIndex = CoverageIndex(At(st, 4), buf[j].glyph);
                if (ligIndex < 0 || ligIndex >= baseArray.U16(0)) continue;
                Bytes attach = At(baseArray, 2 + (size_t)ligIndex * 2);
                uint16_t comps = attach.U16(0);
                if (comps == 0) continue;
                const Info& m = buf[i];
                unsigned comp = (buf[j].ligId && buf[j].ligId == m.ligId && m.ligComp) ? std::min<unsigned>(comps, m.ligComp) - 1 : comps - 1u;
                Bytes anchor = At(attach, 2 + ((size_t)comp * classCount + markClass) * 2);
                if (AttachMark(c, i, j, markArray, markIndex, anchor)) return true;
            } else {
                // The previous mark (honouring the lookup's filters, but not its ignore flags).
                int j = -1;
                for (int k = (int)i - 1; k >= 0; --k) {
                    if (c.Ignored(buf[k], lk.flag & ~(kIgnoreBase | kIgnoreLigatures | kIgnoreMarks), lk.filterSet)) continue;
                    j = k;
                    break;
                }
                if (j < 0 || buf[j].cls != kMark) continue;
                const Info &a = buf[i], &b = buf[j];
                bool ok = a.ligId == b.ligId ? (a.ligId == 0 || a.ligComp == b.ligComp)
                                             : ((a.ligId && !a.ligComp) || (b.ligId && !b.ligComp));
                if (!ok) continue;
                int mark2 = CoverageIndex(At(st, 4), b.glyph);
                if (mark2 < 0 || mark2 >= baseArray.U16(0)) continue;
                Bytes anchor = At(baseArray, 2 + ((size_t)mark2 * classCount + markClass) * 2);
                if (AttachMark(c, i, j, markArray, markIndex, anchor)) return true;
            }
            continue;
        }
        case 7: case 8:
            if (ApplyContext(c, st, lk.type == 8, i)) return true;
            continue;
        default:
            return false;
        }
    }
    return false;
}

bool ApplyLookup(Ctx& c, uint16_t index, size_t i) {
    if (index >= c.table.lookups.size() || i >= c.buf.size()) return false;
    const Lookup& lk = c.table.lookups[index];
    c.flag = lk.flag, c.filterSet = lk.filterSet;
    return c.table.gpos ? ApplyGpos(c, lk, i) : ApplyGsub(c, lk, i);
}

// One lookup over the whole buffer, forward (or backward for reverse chaining).
void ApplyEverywhere(Ctx& c, const Applied& a) {
    if (a.lookup >= c.table.lookups.size()) return;
    const Lookup& lk = c.table.lookups[a.lookup];
    c.lookupMask = a.mask, c.autoZwj = a.autoZwj, c.autoZwnj = a.autoZwnj, c.depth = 0;
    if (!c.table.gpos && lk.type == 8) {
        for (size_t i = c.buf.size(); i-- > 0;) {
            Info& g = c.buf[i];
            if (!(g.mask & a.mask) || c.Ignored(g, lk.flag, lk.filterSet)) continue;
            c.flag = lk.flag, c.filterSet = lk.filterSet;
            c.syllable = a.perSyllable ? g.syllable : 0;
            ApplyGsub(c, lk, i);
        }
        return;
    }
    for (size_t i = 0; i < c.buf.size();) {
        Info& g = c.buf[i];
        if (c.ops > 200000) return;
        size_t size = c.buf.size();
        c.syllable = a.perSyllable ? g.syllable : 0;
        if ((g.mask & a.mask) && !c.Ignored(g, lk.flag, lk.filterSet) && ApplyLookup(c, a.lookup, i)) {
            // Every applied lookup moves on, except a deletion, which shortens the buffer instead.
            i = c.next > i || c.buf.size() < size ? c.next : i + 1;
            continue;
        }
        ++i;
    }
}

}  // namespace

// ---- Plans -------------------------------------------------------------------------------------

namespace {

std::vector<uint32_t> OtScriptTags(uint32_t iso) {
    switch (iso) {
    case Iso("Deva"): return {Tag("dev2"), Tag("deva")};
    case Iso("Beng"): return {Tag("bng2"), Tag("beng")};
    case Iso("Guru"): return {Tag("gur2"), Tag("guru")};
    case Iso("Gujr"): return {Tag("gjr2"), Tag("gujr")};
    case Iso("Orya"): return {Tag("ory2"), Tag("orya")};
    case Iso("Taml"): return {Tag("tml2"), Tag("taml")};
    case Iso("Telu"): return {Tag("tel2"), Tag("telu")};
    case Iso("Knda"): return {Tag("knd2"), Tag("knda")};
    case Iso("Mlym"): return {Tag("mlm2"), Tag("mlym")};
    case Iso("Mymr"): return {Tag("mym2"), Tag("mymr")};
    case Iso("Hira"): case Iso("Kana"): return {Tag("kana")};
    case Iso("Laoo"): return {Tag("lao ")};
    case Iso("Yiii"): return {Tag("yi  ")};
    case Iso("Nkoo"): return {Tag("nko ")};
    case Iso("Vaii"): return {Tag("vai ")};
    case Iso("Zyyy"): case Iso("Zinh"): case Iso("Zzzz"): return {};
    }
    return {iso | 0x20202020};  // the ISO code in lower case
}

bool IsIndic(uint32_t iso) {
    switch (iso) {
    case Iso("Deva"): case Iso("Beng"): case Iso("Guru"): case Iso("Gujr"): case Iso("Orya"):
    case Iso("Taml"): case Iso("Telu"): case Iso("Knda"): case Iso("Mlym"):
        return true;
    }
    return false;
}

struct FeatureSpec {
    uint32_t tag;
    bool global;
    bool manualJoiners = false;
    bool perSyllable = false;
};

}  // namespace

Shaper::Shaper(const Face* face) : face_(face) {}
Shaper::~Shaper() = default;

const Shaper::Plan& Shaper::PlanFor(uint32_t script, bool rtl) {
    uint64_t key = (uint64_t)script << 1 | rtl;
    auto& slot = plans_[key];
    if (slot) return *slot;
    slot = std::make_unique<Plan>();
    Plan& p = *slot;
    p.gsub.Load(face_->gsub, false);
    p.gpos.Load(face_->gpos, true);
    std::vector<uint32_t> tags = OtScriptTags(script);
    if (script == Iso("Arab") || script == Iso("Syrc") || script == Iso("Nkoo") || script == Iso("Mong") || script == Iso("Phag"))
        p.kind = Plan::kArabic;
    else if (IsIndic(script)) p.kind = Plan::kIndic;
    else if (script == Iso("Thai") || script == Iso("Laoo")) p.kind = Plan::kThai;
    else if (script == Iso("Hang")) p.kind = Plan::kHangul;
    p.langGsub = p.gsub.LangSys(tags);
    p.langGpos = p.gpos.LangSys(tags);

    // Stages of features, as HarfBuzz orders them for the script's shaper.
    std::vector<std::vector<FeatureSpec>> stages;
    std::vector<int> pauses;  // after each stage: 0 nothing, 1 Indic initial reordering, 2 Indic final reordering
    auto stage = [&](std::vector<FeatureSpec> f, int pause = 0) {
        stages.push_back(std::move(f));
        pauses.push_back(pause);
    };
    stage({{Tag("rvrn"), true}});
    stage({{rtl ? Tag("rtla") : Tag("ltra"), true}, {rtl ? Tag("rtlm") : Tag("ltrm"), true}});
    std::vector<FeatureSpec> common = {{Tag("abvm"), true}, {Tag("blwm"), true}, {Tag("ccmp"), true}, {Tag("locl"), true},
                                       {Tag("mark"), true, true}, {Tag("mkmk"), true}, {Tag("rlig"), true}, {Tag("calt"), true},
                                       {Tag("clig"), true}, {Tag("curs"), true}, {Tag("dist"), true}, {Tag("kern"), true},
                                       {Tag("liga"), true}, {Tag("rclt"), true}};
    // HarfBuzz leaves calt off for Hangul (CJK fonts put jamo lookups there).
    if (p.kind == Plan::kHangul) common.erase(std::find_if(common.begin(), common.end(), [](const FeatureSpec& f) { return f.tag == Tag("calt"); }));
    if (p.kind == Plan::kArabic) {
        stage({{Tag("stch"), true}});
        stage({{Tag("ccmp"), true, true}, {Tag("locl"), true, true}});
        for (const char* f : {"isol", "fina", "fin2", "fin3", "medi", "med2", "init"}) {
            uint32_t t = (uint32_t)f[0] << 24 | (uint32_t)f[1] << 16 | (uint32_t)f[2] << 8 | (uint32_t)f[3];
            stage({{t, false, true}});
        }
        stage({{Tag("rlig"), true, true}});
        stage({{Tag("calt"), true, true}});
        stage({{Tag("rclt"), true, true}, {Tag("liga"), true, true}, {Tag("clig"), true, true}, {Tag("mset"), true, true}});
    } else if (p.kind == Plan::kIndic) {
        stage({{Tag("locl"), true, false, true}, {Tag("ccmp"), true, false, true}}, 1);
        for (const char* f : {"nukt", "akhn", "rphf", "rkrf", "pref", "blwf", "abvf", "half", "pstf", "vatu", "cjct"}) {
            uint32_t t = (uint32_t)f[0] << 24 | (uint32_t)f[1] << 16 | (uint32_t)f[2] << 8 | (uint32_t)f[3];
            bool global = t == Tag("nukt") || t == Tag("akhn") || t == Tag("rkrf") || t == Tag("vatu") || t == Tag("cjct");
            stage({{t, global, true, true}}, t == Tag("cjct") ? 2 : 0);
        }
        stage({{Tag("init"), false, true, true}, {Tag("pres"), true, true, true}, {Tag("abvs"), true, true, true},
               {Tag("blws"), true, true, true}, {Tag("psts"), true, true, true}, {Tag("haln"), true, true, true}});
    }
    stage(common);

    // Mask bits: one per non-global feature; global features share bit 0.
    uint32_t nextBit = 2;
    for (auto& s : stages)
        for (auto& f : s)
            if (!f.global && !p.masks.count(f.tag) && nextBit) p.masks[f.tag] = nextBit, nextBit <<= 1;
    p.globalMask = 1;
    std::map<uint32_t, int> seen;  // a feature listed twice stays in its first stage
    for (size_t si = 0; si < stages.size(); ++si)
        for (auto& f : stages[si])
            if (!seen.count(f.tag)) seen[f.tag] = (int)si;
    for (int t = 0; t < 2; ++t) {
        Table& table = t ? p.gpos : p.gsub;
        Bytes lang = t ? p.langGpos : p.langGsub;
        auto& out = t ? p.gposStages : p.gsubStages;
        out.assign(stages.size(), {});
        if (lang.Empty()) continue;
        for (size_t si = 0; si < stages.size(); ++si) {
            for (auto& f : stages[si]) {
                if (seen[f.tag] != (int)si) continue;
                bool found = false;
                uint32_t mask = f.global ? 1 : p.masks[f.tag];
                for (uint16_t l : table.FeatureLookups(lang, f.tag, &found))
                    out[si].push_back({l, mask, !f.manualJoiners, true, f.perSyllable});
                if (t == 1 && found && f.tag == Tag("kern")) p.hasGposKern = true;
                if (t == 1 && found && (f.tag == Tag("mark") || f.tag == Tag("mkmk"))) p.hasGposMarks = true;
            }
        }
        if (!out.empty())
            for (uint16_t l : table.RequiredLookups(lang)) out[0].push_back({l, ~0u, true, true});
        for (auto& st : out) {
            std::sort(st.begin(), st.end(), [](const Applied& a, const Applied& b) { return a.lookup < b.lookup; });
            // One lookup in two features of a stage applies once, to either's glyphs.
            std::vector<Applied> merged;
            for (auto& a : st) {
                if (!merged.empty() && merged.back().lookup == a.lookup) merged.back().mask |= a.mask;
                else merged.push_back(a);
            }
            st = std::move(merged);
        }
    }
    p.gsubPauses = pauses;
    if (p.kind == Plan::kArabic && script == Iso("Arab")) {
        bool any = false;
        for (const char* f : {"isol", "fina", "medi", "init"}) {
            uint32_t t = (uint32_t)f[0] << 24 | (uint32_t)f[1] << 16 | (uint32_t)f[2] << 8 | (uint32_t)f[3];
            if (!p.langGsub.Empty() && !p.gsub.FeatureLookups(p.langGsub, t).empty()) any = true;
        }
        p.arabicFallback = !any;
    }
    return p;
}

// ---- Script steps ------------------------------------------------------------------------------

namespace {

enum ArabicForm { kNoForm, kIsol, kFina, kMedi, kInit };

void ArabicJoining(std::vector<Info>& buf, std::vector<uint8_t>* forms) {
    size_t n = buf.size();
    forms->assign(n, kNoForm);
    auto type = [&](size_t i) { return Joining(buf[i].cp); };  // U R D C L T
    auto joinsBefore = [](int t) { return t == 1 || t == 2 || t == 3; };  // can join the character before it (R D C)
    auto joinsAfter = [](int t) { return t == 2 || t == 3 || t == 4; };   // can join the one after it (D C L)
    for (size_t i = 0; i < n; ++i) {
        int t = type(i);
        if (t == 5 || t == 0 || t == 3) continue;  // transparent, non-joining, join-causing: no form
        int prev = -1, next = -1;
        for (size_t k = i; k-- > 0;)
            if (type(k) != 5) {
                prev = type(k);
                break;
            }
        for (size_t k = i + 1; k < n; ++k)
            if (type(k) != 5) {
                next = type(k);
                break;
            }
        bool before = joinsBefore(t) && prev >= 0 && joinsAfter(prev);
        bool after = joinsAfter(t) && next >= 0 && joinsBefore(next);
        (*forms)[i] = before && after ? kMedi : before ? kFina : after ? kInit : kIsol;
    }
}

// A font without positional lookups: the presentation forms its cmap maps instead.
void ArabicFallback(const Face& face, std::vector<Info>& buf, const std::vector<uint8_t>& forms) {
    auto formOf = [](uint32_t cp, int form) -> uint32_t {
        size_t count = sizeof(kArabicForms) / sizeof(kArabicForms[0]) / 5;
        size_t lo = 0, hi = count;
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (kArabicForms[mid * 5] < cp) lo = mid + 1;
            else hi = mid;
        }
        if (lo >= count || kArabicForms[lo * 5] != cp) return 0;
        int column = form == kIsol ? 1 : form == kFina ? 2 : form == kInit ? 3 : 4;
        return kArabicForms[lo * 5 + column];
    };
    for (size_t i = 0; i < buf.size(); ++i) {
        // Lam with a following alef becomes one of the lam-alef ligatures.
        if (buf[i].cp == 0x0644 && i + 1 < buf.size()) {
            static const uint32_t kAlef[] = {0x0622, 0x0623, 0x0625, 0x0627};
            static const uint32_t kLig[] = {0xFEF5, 0xFEF7, 0xFEF9, 0xFEFB};
            for (int a = 0; a < 4; ++a)
                if (buf[i + 1].cp == kAlef[a]) {
                    uint32_t lig = kLig[a] + ((forms[i] == kFina || forms[i] == kMedi) ? 1 : 0);
                    if (uint16_t g = face.GlyphIndex(lig)) {
                        buf[i].glyph = g;
                        buf[i + 1].cluster = buf[i].cluster;
                        buf.erase(buf.begin() + i + 1);
                        goto next;
                    }
                }
        }
        if (forms[i] != kNoForm)
            if (uint32_t f = formOf(buf[i].cp, forms[i]))
                if (uint16_t g = face.GlyphIndex(f)) buf[i].glyph = g;
    next:;
    }
}

// Indic categories, simplified from HarfBuzz's.
enum IndicCat : uint8_t { kOther, kC, kV, kM, kH, kN, kSM, kVD, kZWJ, kZWNJ, kRa, kPlaceholder, kRepha };
// Positions, in HarfBuzz's order.
enum IndicPos : uint8_t { kPosStart, kPosReph, kPosPreM, kPosPreC, kPosBaseC, kPosAfterMain, kPosAboveC, kPosBeforeSub, kPosBelowC,
                          kPosAfterSub, kPosBeforePost, kPosPostC, kPosAfterPost, kPosSmvd, kPosEnd };

// Where a non-left matra sorts within its syllable, by script and side, as HarfBuzz has it.
uint8_t MatraPos(uint32_t cp) {
    uint8_t side = Indic(cp) >> 4;
    uint32_t block = cp & ~0x7Fu;
    bool deva = block == 0x0900, beng = block == 0x0980, guru = block == 0x0A00, gujr = block == 0x0A80, orya = block == 0x0B00,
         taml = block == 0x0B80, telu = block == 0x0C00, knda = block == 0x0C80, mlym = block == 0x0D00;
    if (side == 1 || side == 8 || side == 9 || side == 12 || side == 13 || side == 14) return kPosPreM;
    if (side == 2 || side == 7 || side == 10 || side == 11) {  // right
        if (beng || guru || gujr || orya || taml || mlym) return kPosAfterPost;
        if (telu) return cp <= 0x0C42 ? kPosBeforeSub : kPosAfterSub;
        if (knda) return cp < 0x0CC3 || cp > 0x0CD6 ? kPosBeforeSub : kPosAfterSub;
        return kPosAfterSub;
    }
    if (side == 3) {  // top
        if (guru) return kPosAfterPost;
        if (orya) return kPosAfterMain;
        if (telu || knda) return kPosBeforeSub;
        return kPosAfterSub;
    }
    // bottom
    if (guru || gujr || taml || mlym) return kPosAfterPost;
    if (telu || knda) return kPosBeforeSub;
    (void)deva;
    return kPosAfterSub;
}

uint32_t RaOf(uint32_t script) {
    switch (script) {
    case Iso("Deva"): return 0x0930;
    case Iso("Beng"): return 0x09B0;
    case Iso("Gujr"): return 0x0AB0;
    case Iso("Orya"): return 0x0B30;
    case Iso("Knda"): return 0x0CB0;
    case Iso("Mlym"): return 0x0D30;
    }
    return 0;  // no implicit reph
}

IndicCat IndicCategory(uint32_t cp, uint32_t ra) {
    if (cp == 0x200D) return kZWJ;
    if (cp == 0x200C) return kZWNJ;
    if (cp == 0x25CC || cp == 0x00A0) return kPlaceholder;
    uint8_t v = Indic(cp);
    switch (v & 15) {
    case 1: case 9: return cp == ra ? kRa : kC;
    case 11: return kPlaceholder;
    case 2: return kV;
    case 3: return kM;
    case 4: case 13: return kH;
    case 5: return kN;
    case 6: case 7: case 14: return kSM;
    case 12: return kVD;
    case 15: return kRepha;
    }
    return kOther;
}

}  // namespace

// ---- Shaping -----------------------------------------------------------------------------------

namespace {

struct Shaping {
    const Face& face;
    const ShapeFont& font;
    const Shaper::Plan& plan;
    Gdef gdef;
    std::vector<Info> buf;
    std::vector<Pos> pos;
    uint8_t nextLigId = 1;
    uint32_t script;
    bool rtl;

    Shaping(const Face& f, const ShapeFont& sf, const Shaper::Plan& p, uint32_t sc, bool r)
        : face(f), font(sf), plan(p), gdef(f.gdef), script(sc), rtl(r) {}

    Ctx Context(const Table& table) {
        Ctx c{plan, table, gdef, font, buf, &pos, ~0u, 0, 0, true, true, 0, &nextLigId};
        return c;
    }

    uint16_t Glyph(uint32_t cp) const { return face.GlyphIndex(cp); }

    void Normalize() {
        bool indic = plan.kind == Shaper::Plan::kIndic;
        // Decompose what the font lacks (and, for Indic scripts, split vowel signs always).
        std::vector<Info> out;
        out.reserve(buf.size());
        for (const Info& g : buf) {
            uint32_t a, b;
            bool split = indic && (Indic(g.cp) & 15) == 3 && Decompose(g.cp, &a, &b) && (Indic(a) & 15) == 3;
            if ((!Glyph(g.cp) || split) && !IsDefaultIgnorable(g.cp)) {
                std::vector<uint32_t> parts{g.cp};
                for (int depth = 0; depth < 4; ++depth) {
                    if (!Decompose(parts[0], &a, &b)) break;
                    parts[0] = b;
                    parts.insert(parts.begin(), a);
                    if (Glyph(a) || split) break;
                }
                bool all = parts.size() > 1;
                for (uint32_t p : parts) all = all && Glyph(p);
                if (all) {
                    for (uint32_t p : parts) {
                        Info x = g;
                        x.cp = p;
                        out.push_back(x);
                    }
                    continue;
                }
            }
            out.push_back(g);
        }
        buf = std::move(out);
        // Canonical order of combining marks.
        for (size_t i = 0; i < buf.size();) {
            if (!Ccc(buf[i].cp)) {
                ++i;
                continue;
            }
            size_t j = i;
            while (j < buf.size() && Ccc(buf[j].cp)) ++j;
            if (j - i > 1 && j - i <= 32)
                std::stable_sort(buf.begin() + i, buf.begin() + j, [](const Info& x, const Info& y) { return Ccc(x.cp) < Ccc(y.cp); });
            i = j;
        }
        if (indic) return;
        // Recompose a base and marks into a character the font has.
        for (size_t i = 0; i < buf.size(); ++i) {
            if (Ccc(buf[i].cp)) continue;
            size_t starter = i;
            uint8_t lastCcc = 0;
            for (size_t j = i + 1; j < buf.size();) {
                uint8_t c = Ccc(buf[j].cp);
                if (!IsMark(buf[j].cp) && c == 0) break;
                bool blocked = lastCcc != 0 && lastCcc >= c;
                uint32_t composed = blocked ? 0 : Compose(buf[starter].cp, buf[j].cp);
                if (composed && Glyph(composed)) {
                    buf[starter].cp = composed;
                    buf.erase(buf.begin() + j);
                    continue;
                }
                lastCcc = c;
                if (c == 0) break;
                ++j;
            }
        }
    }

    void MapGlyphs() {
        uint16_t space = Glyph(' ');
        for (size_t i = 0; i < buf.size(); ++i) {
            Info& g = buf[i];
            uint32_t cp = g.cp;
            if (cp == 0x200C) g.ignorable = 2;
            else if (cp == 0x200D) g.ignorable = 3;
            else if (IsDefaultIgnorable(cp)) g.ignorable = 1;
            g.glyph = Glyph(cp);
            if (i + 1 < buf.size()) {
                uint32_t vs = buf[i + 1].cp;
                if ((vs >= 0xFE00 && vs <= 0xFE0F) || (vs >= 0xE0100 && vs <= 0xE01EF))
                    if (uint16_t v = face.VariantGlyph(cp, vs)) g.glyph = v;
            }
            if (!g.glyph && space && cp != ' ' &&
                (cp == 0x00A0 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x202F || cp == 0x205F || cp == 0x3000))
                g.glyph = space;
            if (!g.glyph && cp == 0x2011) g.glyph = Glyph(0x2010);
            g.mask = plan.globalMask;
            g.cls = MarkKind(cp) == 1 ? kMark : kBase;
            if (gdef.HasClasses()) {
                uint16_t c = ClassOf(gdef.classes, g.glyph);
                // As HarfBuzz: with GDEF classes, a glyph the font leaves unclassified is no mark.
                g.cls = c >= 1 && c <= 4 ? (uint8_t)c : kUnclassified;
                g.markClass = (uint8_t)ClassOf(gdef.markAttach, g.glyph);
            }
        }
    }

    // Spaces the font lacks take the space glyph at HarfBuzz's fallback widths.
    int32_t SpaceWidth(uint32_t cp, int32_t spaceAdvance) const {
        int32_t em = font.emX;
        switch (cp) {
        case 0x2000: case 0x2002: return em / 2;
        case 0x2001: case 0x2003: case 0x3000: return em;
        case 0x2004: return em / 3;
        case 0x2005: return em / 4;
        case 0x2006: return em / 6;
        case 0x2007: if (uint16_t z = Glyph('0')) return font.advance(z); break;
        case 0x2008: if (uint16_t d = Glyph('.')) return font.advance(d); break;
        case 0x2009: return em / 5;
        case 0x200A: return em / 16;
        case 0x202F: return spaceAdvance / 2;
        case 0x205F: return em * 4 / 18;
        }
        return spaceAdvance;
    }

    void ThaiSaraAm() {
        for (size_t i = 0; i < buf.size(); ++i) {
            uint32_t cp = buf[i].cp;
            if (cp != 0x0E33 && cp != 0x0EB3) continue;
            uint32_t base = cp & ~0xFFu;
            Info nikhahit = buf[i];
            nikhahit.cp = base | 0x4D | (cp & 0x80);
            buf[i].cp = base | 0x32 | (cp & 0x80);
            // NIKHAHIT moves before the above-base marks the syllable already has.
            size_t at = i;
            auto above = [](uint32_t c) {
                uint32_t x = c & ~0x0080u;
                return (x >= 0x0E34 && x <= 0x0E37) || (x >= 0x0E47 && x <= 0x0E4E) || x == 0x0E31 || x == 0x0E3B;
            };
            while (at > 0 && above(buf[at - 1].cp)) --at;
            uint32_t cluster = buf[at].cluster;
            buf.insert(buf.begin() + at, nikhahit);
            for (size_t k = at; k <= i + 1; ++k) buf[k].cluster = std::min(cluster, buf[k].cluster);
            ++i;
        }
    }

    // Would any lookup of `tag` turn this two-glyph sequence into a ligature?
    bool WouldLigate(uint32_t tag, uint16_t a, uint16_t b) const {
        if (plan.langGsub.Empty()) return false;
        for (uint16_t l : plan.gsub.FeatureLookups(plan.langGsub, tag)) {
            if (l >= plan.gsub.lookups.size()) continue;
            const Lookup& lk = plan.gsub.lookups[l];
            if (lk.type != 4) continue;
            for (const Bytes& st : lk.subtables) {
                int cov = CoverageIndex(At(st, 2), a);
                if (cov < 0 || cov >= st.U16(4)) continue;
                Bytes set = At(st, 6 + (size_t)cov * 2);
                for (uint16_t k = 0; k < set.U16(0); ++k) {
                    Bytes lig = At(set, 2 + (size_t)k * 2);
                    if (lig.U16(2) == 2 && lig.U16(4) == b) return true;
                }
            }
        }
        return false;
    }

    // Syllables, base consonants, reph and pre-base matras, and the masks of the basic features.
    void Merge(size_t a, size_t b) { MergeClusters(buf, a, b); }

    void IndicInitial() {
        uint32_t ra = RaOf(script);
        uint32_t virama = 0;
        for (const Info& g : buf)
            if (IndicCategory(g.cp, ra) == kH) {
                virama = g.cp;
                break;
            }
        uint16_t viramaGlyph = virama ? Glyph(virama) : 0;
        for (Info& g : buf) g.cat = IndicCategory(g.cp, ra);
        // A consonant's position: below or after the base when the font has such a form for it.
        auto consonantPos = [&](uint16_t glyph) {
            if (viramaGlyph) {
                if (WouldLigate(Tag("blwf"), viramaGlyph, glyph) || WouldLigate(Tag("blwf"), glyph, viramaGlyph)) return kPosBelowC;
                if (WouldLigate(Tag("pstf"), viramaGlyph, glyph) || WouldLigate(Tag("pstf"), glyph, viramaGlyph)) return kPosPostC;
            }
            return kPosBaseC;
        };
        uint16_t dotted = Glyph(0x25CC);
        uint8_t syllable = 1;
        for (size_t i = 0; i < buf.size();) {
            size_t start = i;
            uint8_t c = buf[i].cat;
            auto at = [&](size_t k) { return k < buf.size() ? buf[k].cat : (uint8_t)kOther; };
            bool consonantLike = c == kC || c == kRa || c == kPlaceholder;
            if (consonantLike || c == kV || c == kRepha) {
                ++i;
                if (at(i) == kN) ++i;
                if (consonantLike) {
                    while (at(i) == kH) {
                        size_t k = i + 1;
                        if (at(k) == kZWJ || at(k) == kZWNJ) ++k;
                        if (at(k) == kC || at(k) == kRa) {
                            i = k + 1;
                            if (at(i) == kN) ++i;
                        } else {
                            i = k;
                            break;
                        }
                    }
                }
                while (at(i) == kM || at(i) == kN || at(i) == kH) ++i;
                while (at(i) == kSM || at(i) == kVD) ++i;
            } else if (c == kM || c == kH || c == kN || c == kSM || c == kVD) {
                // A sign without a base: HarfBuzz puts a dotted circle under it.
                if (dotted) {
                    Info d = buf[i];
                    d.cp = 0x25CC, d.glyph = dotted, d.cat = kPlaceholder, d.cls = kBase;
                    buf.insert(buf.begin() + i, d);
                    continue;
                }
                ++i;
                while (at(i) == kM || at(i) == kN || at(i) == kH || at(i) == kSM || at(i) == kVD) ++i;
            } else {
                ++i;
            }
            size_t end = i;
            for (size_t k = start; k < end; ++k) buf[k].syllable = syllable;
            syllable = (uint8_t)(syllable % 255 + 1);
            if (consonantLike || c == kV) ReorderSyllable(start, end, ra != 0, consonantPos);
        }
    }

    template <class F>
    void ReorderSyllable(size_t start, size_t end, bool rephScript, F&& consonantPos) {
        bool reph = rephScript && end - start >= 3 && buf[start].cat == kRa && buf[start + 1].cat == kH && buf[start + 2].cat != kZWJ;
        if (reph) {
            bool more = false;
            for (size_t k = start + 2; k < end; ++k) more = more || buf[k].cat == kC || buf[k].cat == kRa;
            reph = more;
        }
        size_t limit = reph ? start + 2 : start;
        size_t base = start;
        if (buf[start].cat != kV) {
            bool seenBelow = false;
            base = limit;
            for (size_t i = end; i-- > limit;) {
                uint8_t c = buf[i].cat;
                if (c == kC || c == kRa || c == kPlaceholder) {
                    uint8_t p = (i > limit && buf[i - 1].cat == kH) ? consonantPos(buf[i].glyph) : kPosBaseC;
                    buf[i].pos = p;
                    if (p != kPosBelowC && (p != kPosPostC || seenBelow)) {
                        base = i;
                        break;
                    }
                    if (p == kPosBelowC) seenBelow = true;
                    base = i;
                } else if (i > start && c == kZWJ && buf[i - 1].cat == kH) {
                    break;
                }
            }
        }
        // Positions, then a stable sort brings pre-base matras and consonants forward.
        for (size_t i = start; i < end; ++i) {
            Info& g = buf[i];
            if (reph && i < start + 2) g.pos = kPosReph;
            else if (i < base) g.pos = kPosPreC;
            else if (i == base) g.pos = kPosBaseC;
            else if (g.cat == kM) g.pos = MatraPos(g.cp);
            else if (g.cat == kSM || g.cat == kVD) g.pos = kPosSmvd;
            else if (g.cat == kC || g.cat == kRa || g.cat == kPlaceholder) g.pos = g.pos == kPosPostC ? kPosPostC : kPosBelowC;
            else g.pos = i > start ? buf[i - 1].pos : (uint8_t)kPosBaseC;  // halant, nukta, joiners: with what they follow
        }
        // A consonant after the base takes along what stands between it and the last consonant or matra.
        for (size_t i = base + 1, last = base; i < end; ++i) {
            uint8_t c = buf[i].cat;
            if (c == kC || c == kRa || c == kPlaceholder) {
                for (size_t j = last + 1; j < i; ++j)
                    if (buf[j].pos < kPosSmvd) buf[j].pos = buf[i].pos;
                last = i;
            } else if (c == kM) {
                last = i;
            }
        }
        // Each glyph remembers where it was, so the clusters it crossed can merge after the sort.
        std::vector<size_t> from(end - start);
        std::vector<std::pair<Info, size_t>> items;
        for (size_t i = start; i < end; ++i) items.push_back({buf[i], i - start});
        std::stable_sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.first.pos < b.first.pos; });
        for (size_t i = start; i < end; ++i) buf[i] = items[i - start].first, from[i - start] = items[i - start].second;
        uint32_t rphf = plan.Mask(Tag("rphf")), half = plan.Mask(Tag("half")), blwf = plan.Mask(Tag("blwf"));
        uint32_t abvf = plan.Mask(Tag("abvf")), pstf = plan.Mask(Tag("pstf"));
        size_t newBase = start;
        for (size_t i = start; i < end; ++i)
            if (buf[i].pos == kPosBaseC) {
                newBase = i;
                break;
            }
        for (size_t i = start; i < end; ++i) {
            Info& g = buf[i];
            if (g.pos == kPosReph) g.mask |= rphf;
            if (i < newBase) g.mask |= half | blwf;
            else if (i > newBase) g.mask |= blwf | abvf | pstf;
        }
        // From the base on, glyphs the sort permuted share a cluster (as HarfBuzz merges each
        // permutation cycle); pre-base reordering merges in the final pass.
        std::vector<bool> done(end - start, false);
        for (size_t i = newBase; i < end; ++i) {
            if (done[i - start]) continue;
            size_t lo = i, hi = i, j = start + from[i - start];
            done[i - start] = true;
            while (j != i) {
                lo = std::min(lo, j), hi = std::max(hi, j);
                done[j - start] = true;
                j = start + from[j - start];
            }
            Merge(std::max(newBase, lo), hi + 1);
        }
        // A ZWNJ keeps the consonant before it from taking its half form.
        for (size_t i = start + 1; i < end; ++i) {
            if (buf[i].cat != kZWNJ) continue;
            size_t j = i;
            do {
                --j;
                buf[j].mask &= ~half;
            } while (j > start && !(buf[j].cat == kC || buf[j].cat == kRa || buf[j].cat == kPlaceholder));
        }
    }

    void IndicFinal() {
        for (size_t start = 0; start < buf.size();) {
            size_t end = start + 1;
            while (end < buf.size() && buf[end].syllable == buf[start].syllable) ++end;
            size_t base = end;
            for (size_t i = start; i < end; ++i)
                if (buf[i].pos == kPosBaseC) {
                    base = i;
                    break;
                }
            // A pre-base matra sits after the last halant still standing before the base.
            if (base < end) {
                for (size_t i = start; i < base; ++i) {
                    if (buf[i].pos != kPosPreM || buf[i].cat != kM) continue;
                    size_t target = i;
                    for (size_t k = i + 1; k < base; ++k)
                        if (buf[k].cat == kH) target = k;
                    if (target != i) {
                        Info m = buf[i];
                        buf.erase(buf.begin() + i);
                        buf.insert(buf.begin() + target, m);
                    }
                    Merge(target, std::min(end, base + 1));
                    break;
                }
            }
            // A formed reph moves after the matras, before syllable modifiers and post-base forms.
            if (buf[start].pos == kPosReph && buf[start].ligComps > 1 && end - start > 1) {
                size_t target = end;
                for (size_t k = start + 1; k < end; ++k)
                    if (buf[k].pos > kPosBeforePost && buf[k].pos != kPosReph) {
                        target = k;
                        break;
                    }
                Info r = buf[start];
                buf.erase(buf.begin() + start);
                buf.insert(buf.begin() + (target - 1), r);
                Merge(start, target);
            }
            // A left matra starting a word takes its initial form.
            if (buf[start].pos == kPosPreM && buf[start].cat == kM) {
                bool wordStart = start == 0;
                if (!wordStart) {
                    uint32_t prev = buf[start - 1].cp;
                    uint8_t sc = ScriptId(prev);
                    wordStart = !(IsMark(prev) || IsDefaultIgnorable(prev) || (sc != kScZyyy && sc != kScZinh && sc != kScZzzz));
                }
                if (wordStart) buf[start].mask |= plan.Mask(Tag("init"));
            }
            start = end;
        }
    }

    void Substitute() {
        std::vector<uint8_t> forms;
        if (plan.kind == Shaper::Plan::kArabic) {
            ArabicJoining(buf, &forms);
            uint32_t bits[] = {0, plan.Mask(Tag("isol")), plan.Mask(Tag("fina")), plan.Mask(Tag("medi")), plan.Mask(Tag("init"))};
            for (size_t i = 0; i < buf.size(); ++i) buf[i].mask |= bits[forms[i]];
            if (plan.arabicFallback) ArabicFallback(face, buf, forms);
        }
        if (plan.kind == Shaper::Plan::kIndic) IndicInitial();
        Ctx c = Context(plan.gsub);
        for (size_t s = 0; s < plan.gsubStages.size(); ++s) {
            for (const Applied& a : plan.gsubStages[s]) ApplyEverywhere(c, a);
            if (plan.kind == Shaper::Plan::kIndic && s < plan.gsubPauses.size() && plan.gsubPauses[s] == 2) IndicFinal();
        }
    }

    void Position(std::vector<ShapedGlyph>* out) {
        pos.assign(buf.size(), {});
        uint16_t space = Glyph(' ');
        int32_t spaceAdvance = space ? font.advance(space) : 0;
        for (size_t i = 0; i < buf.size(); ++i) {
            const Info& g = buf[i];
            if (g.ignorable) continue;
            pos[i].xa = g.glyph == space && g.cp != ' ' && !face.GlyphIndex(g.cp) ? SpaceWidth(g.cp, spaceAdvance) : font.advance(g.glyph);
        }
        Ctx c = Context(plan.gpos);
        for (auto& stage : plan.gposStages)
            for (const Applied& a : stage) ApplyEverywhere(c, a);
        // The kern table when GPOS has no kerning: pairs of adjacent non-mark glyphs.
        if (!plan.hasGposKern && !buf.empty()) {
            int prev = -1;
            for (size_t i = 0; i < buf.size(); ++i) {
                if (buf[i].cls == kMark || buf[i].ignorable) continue;
                if (prev >= 0) {
                    int k = face.KernTable(buf[prev].glyph, buf[i].glyph);
                    if (k) pos[prev].xa += c.ScaleX(k);
                }
                prev = (int)i;
            }
        }
        // Marks have no advance of their own (except in Indic scripts, where HarfBuzz keeps them).
        if (plan.kind != Shaper::Plan::kIndic) {
            bool adjust = !plan.hasGposMarks && plan.gpos.lookups.empty() && !rtl;
            for (size_t i = 0; i < buf.size(); ++i)
                if (buf[i].cls == kMark) {
                    if (adjust) pos[i].xo -= pos[i].xa;
                    pos[i].xa = pos[i].ya = 0;
                }
        }
        // Attached marks: offsets relative to their base, less the advances between.
        for (size_t i = 0; i < buf.size(); ++i) Propagate(i, 0);
        // Hidden default ignorables become the space glyph with no advance.
        std::vector<ShapedGlyph> glyphs;
        glyphs.reserve(buf.size());
        for (size_t i = 0; i < buf.size(); ++i) {
            const Info& g = buf[i];
            ShapedGlyph s{g.glyph, g.cluster, pos[i].xa, pos[i].ya, pos[i].xo, pos[i].yo};
            if (g.ignorable) {
                if (!space) continue;
                s.glyph = space, s.xAdvance = s.yAdvance = s.xOffset = s.yOffset = 0;
            }
            glyphs.push_back(s);
        }
        if (rtl) std::reverse(glyphs.begin(), glyphs.end());
        *out = std::move(glyphs);
    }

    void Propagate(size_t i, int depth) {
        Pos& p = pos[i];
        if (!p.chain || depth > 32) return;
        int j = (int)i + p.chain;
        p.chain = 0;
        if (j < 0 || j >= (int)pos.size()) return;
        Propagate((size_t)j, depth + 1);
        p.xo += pos[j].xo, p.yo += pos[j].yo;
        if (!rtl)
            for (int k = j; k < (int)i; ++k) p.xo -= pos[k].xa, p.yo -= pos[k].ya;
        else
            for (int k = j + 1; k <= (int)i; ++k) p.xo += pos[k].xa, p.yo += pos[k].ya;
    }
};

}  // namespace

void Shaper::Shape(const ShapeFont& font, const uint32_t* text, const uint32_t* clusters, size_t n, uint32_t script, bool rtl,
                   std::vector<ShapedGlyph>* out) {
    out->clear();
    if (!n || !font.face) return;
    const Plan& plan = PlanFor(script, rtl);
    Shaping s(*font.face, font, plan, script, rtl);
    s.buf.resize(n);
    for (size_t i = 0; i < n; ++i) {
        uint32_t cp = text[i];
        if (rtl)
            if (uint32_t m = Mirror(cp); m && font.face->GlyphIndex(m)) cp = m;
        s.buf[i].cp = cp;
        s.buf[i].cluster = clusters[i];
    }
    // Marks and joiners belong to the cluster of what they follow, as HarfBuzz forms graphemes.
    for (size_t i = 1; i < n; ++i)
        if (JoinsGraphemes(s.buf[i].cp) || (s.buf[i - 1].cp == 0x200D && IsEmojiPresentation(s.buf[i].cp)))
            s.buf[i].cluster = s.buf[i - 1].cluster;
    if (plan.kind == Plan::kThai) s.ThaiSaraAm();
    s.Normalize();
    s.MapGlyphs();
    s.Substitute();
    s.Position(out);
}

}  // namespace lx::type
