// Checks of the Linux text stack (linux/typeface.h, raster.h, shaper.h, textlayout.h), run by the Linux core test:
//   "text DIR [FONT...]"  synthetic fonts made in DIR: outlines, coverage, kerning, ligatures, synthetic bold,
//                         collections; then the installed Latin, Arabic, Devanagari, CJK and emoji fonts when
//                         present; then malformed copies of every font (the fixtures, FONT... and the installed
//                         ones), each opened, rasterized and shaped without a crash or a stray read under ASan
//   "bidi FILE"           Unicode's BidiCharacterTest.txt: paragraph direction, levels and visual order
#pragma once
#include "linux/fontmatch.h"
#include "linux/raster.h"
#include "linux/shaper.h"
#include "linux/textlayout.h"
#include "linux/typeface.h"
#include "tests/fontmatch_checks.h"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "third_party/stb_image.h"

#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace text_checks {

namespace type = lx::type;
using fontmatch_checks::Put16;
using fontmatch_checks::Put32;

inline bool DecodePng(std::string_view png, int* w, int* h, std::vector<uint8_t>* rgba) {
    int channels = 0;
    unsigned char* px = stbi_load_from_memory((const unsigned char*)png.data(), (int)png.size(), w, h, &channels, 4);
    if (!px) return false;
    rgba->assign(px, px + (size_t)*w * *h * 4);
    stbi_image_free(px);
    return true;
}

// ---- A TrueType fixture: 'A' a 500x700 box, 'B' 550x500, and an A-B ligature 1000x700, at 1000 units per em ----

struct Box { int x0, y0, x1, y1, advance; };

inline std::string SimpleGlyph(const Box& b) {
    std::string g;
    Put16(g, 1), Put16(g, (uint16_t)b.x0), Put16(g, (uint16_t)b.y0), Put16(g, (uint16_t)b.x1), Put16(g, (uint16_t)b.y1);
    Put16(g, 3), Put16(g, 0);
    for (int i = 0; i < 4; ++i) g += (char)1;  // on-curve, word coordinates
    int xs[4] = {b.x0, b.x0, b.x1, b.x1}, ys[4] = {b.y0, b.y1, b.y1, b.y0};
    int px = 0, py = 0;
    for (int x : xs) Put16(g, (uint16_t)(int16_t)(x - px)), px = x;
    for (int y : ys) Put16(g, (uint16_t)(int16_t)(y - py)), py = y;
    while (g.size() % 4) g += '\0';
    return g;
}

inline std::string BoxFont(const std::string& family) {
    fontmatch_checks::Spec spec;
    spec.family = family;
    spec.glyphs = 4;
    spec.bmp = {{'A', 'B'}};  // A -> 1, B -> 2
    auto t = fontmatch_checks::Tables(spec);
    std::vector<Box> boxes = {{0, 0, 0, 0, 500}, {100, 0, 600, 700, 700}, {0, 0, 550, 500, 600}, {0, 0, 1000, 700, 1000}};
    std::string glyf, loca, hmtx;
    for (const Box& b : boxes) {
        Put16(loca, (uint32_t)glyf.size() / 2);
        if (b.x1 > b.x0) glyf += SimpleGlyph(b);
        Put16(hmtx, (uint32_t)b.advance), Put16(hmtx, (uint32_t)b.x0);
    }
    Put16(loca, (uint32_t)glyf.size() / 2);
    t["glyf"] = glyf, t["loca"] = loca, t["hmtx"] = hmtx;
    t["hhea"].resize(4), Put16(t["hhea"], 800), Put16(t["hhea"], (uint16_t)(int16_t)-200);  // ascender, descender
    t["hhea"].resize(34), Put16(t["hhea"], (uint32_t)boxes.size());
    t["maxp"] = t["maxp"].substr(0, 4), Put16(t["maxp"], (uint32_t)boxes.size());
    // kern: B then A closes by 100 units.
    std::string kern;
    Put16(kern, 0), Put16(kern, 1), Put16(kern, 0), Put16(kern, 14 + 6), Put16(kern, 1);
    Put16(kern, 1), Put16(kern, 6), Put16(kern, 0), Put16(kern, 0);
    Put16(kern, 2), Put16(kern, 1), Put16(kern, (uint16_t)(int16_t)-100);
    t["kern"] = kern;
    // GSUB: a DFLT liga lookup, A B -> the ligature.
    std::string gsub, scripts, features, lookups;
    Put16(scripts, 1), scripts += "DFLT", Put16(scripts, 8);  // ScriptList, Script at 8
    Put16(scripts, 4), Put16(scripts, 0);                     // Script: default LangSys at 4
    Put16(scripts, 0), Put16(scripts, 0xFFFF), Put16(scripts, 1), Put16(scripts, 0);
    Put16(features, 1), features += "liga", Put16(features, 8), Put16(features, 0), Put16(features, 1), Put16(features, 0);
    std::string sub;  // LigatureSubst 1
    Put16(sub, 1), Put16(sub, 8), Put16(sub, 1), Put16(sub, 14);
    Put16(sub, 1), Put16(sub, 1), Put16(sub, 1);                // Coverage format 1: glyph 1
    Put16(sub, 1), Put16(sub, 4);                               // LigatureSet: one at 4
    Put16(sub, 3), Put16(sub, 2), Put16(sub, 2);                // Ligature: glyph 3 from 2 components, then B
    Put16(lookups, 1), Put16(lookups, 4);
    Put16(lookups, 4), Put16(lookups, 0), Put16(lookups, 1), Put16(lookups, 8);
    lookups += sub;
    Put32(gsub, 0x00010000), Put16(gsub, 10), Put16(gsub, (uint32_t)(10 + scripts.size()));
    Put16(gsub, (uint32_t)(10 + scripts.size() + features.size()));
    t["GSUB"] = gsub + scripts + features + lookups;
    return fontmatch_checks::Sfnt(t);
}

// A box face over arbitrary code points: one glyph per entry, in the order given (its glyph id is
// 1 + the number of entries before it), each with its own box. No GSUB, so scripts that fall back
// without positional lookups (Arabic's presentation forms) take that path.
inline std::string GlyphFont(const std::string& family, const std::vector<std::pair<uint32_t, Box>>& entries) {
    fontmatch_checks::Spec spec;
    spec.family = family;
    spec.glyphs = (uint16_t)(entries.size() + 1);
    std::vector<Box> boxes{{0, 0, 0, 0, 500}};  // .notdef
    std::string glyf, loca, hmtx;
    Put16(loca, 0);
    Put16(hmtx, 500), Put16(hmtx, 0);
    for (const auto& [cp, b] : entries) {
        spec.bmp.push_back({cp, cp});  // one range each, so ids follow the entry order
        boxes.push_back(b);
        Put16(loca, (uint32_t)glyf.size() / 2);
        if (b.x1 > b.x0) glyf += SimpleGlyph(b);
        Put16(hmtx, (uint32_t)b.advance), Put16(hmtx, (uint32_t)b.x0);
    }
    Put16(loca, (uint32_t)glyf.size() / 2);
    auto t = fontmatch_checks::Tables(spec);
    t["glyf"] = glyf, t["loca"] = loca, t["hmtx"] = hmtx;
    t["hhea"].resize(4), Put16(t["hhea"], 800), Put16(t["hhea"], (uint16_t)(int16_t)-200);
    t["hhea"].resize(34), Put16(t["hhea"], (uint32_t)boxes.size());
    t["maxp"] = t["maxp"].substr(0, 4), Put16(t["maxp"], (uint32_t)boxes.size());
    return fontmatch_checks::Sfnt(t);
}

// A code point as UTF-8, for the check strings.
inline std::string Cp(uint32_t cp) {
    std::string out;
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) out += {(char)(0xC0 | cp >> 6), (char)(0x80 | cp & 0x3F)};
    else if (cp < 0x10000) out += {(char)(0xE0 | cp >> 12), (char)(0x80 | (cp >> 6) & 0x3F), (char)(0x80 | cp & 0x3F)};
    else out += {(char)(0xF0 | cp >> 18), (char)(0x80 | (cp >> 12) & 0x3F), (char)(0x80 | (cp >> 6) & 0x3F), (char)(0x80 | cp & 0x3F)};
    return out;
}

inline type::FontData Bytes(const std::string& s) { return type::FromBytes(std::make_shared<const std::string>(s)); }

inline int32_t Scale(int px, int upem) { return type::DivFix(px * 64, upem); }

// Every structure the fuzzed copy reaches: the face, outlines and coverage, color images, and shaping.
inline void Exercise(const std::string& bytes, int index) {
    auto data = Bytes(bytes);
    auto face = type::Face::Load(data, index);
    if (!face) return;
    int glyphs = std::min(face->glyphCount, 64);
    for (int g = 0; g < glyphs; ++g) {
        type::Outline o;
        type::Coverage c;
        if (face->GetOutline((uint16_t)g, Scale(16, face->unitsPerEm), Scale(16, face->unitsPerEm), true, &o)) {
            type::Rasterize(o, false, &c);
            type::Rasterize(o, true, &c);
        }
        type::ColorImage img;
        face->GetColorImage((uint16_t)g, 16, &img);
        face->KernTable((uint16_t)g, (uint16_t)(g + 1));
    }
    if (auto font = type::Font::Open(data, index, 16)) {
        static const char* samples[] = {"Office AVAWAY fi ffl", "سلامٌ عليكم (1975)", "क्षत्रिय र्ति", "ไม่น้ำ",
                                        "日本語の文字 한국어", "שָׁלוֹם", "😀👍🏽👨‍👩‍👧 ❤️", "AB BA"};
        for (const char* s : samples) {
            font->Width(s);
            int w = 0;
            size_t len = 0;
            font->Measure(s, 40, &w, &len);
            std::vector<type::Font::Quad> quads;
            font->Quads(s, &quads);
        }
    }
}

// Where a face's tables lie in its file, so half the edits land in the tables the parsers read.
inline std::vector<std::pair<size_t, size_t>> TableSpans(const std::string& bytes, int index) {
    type::Bytes b{(const unsigned char*)bytes.data(), bytes.size()};
    size_t dir = b.U32(0) == type::Tag("ttcf") ? b.U32(12 + (size_t)index * 4) : 0;
    std::vector<std::pair<size_t, size_t>> spans;
    for (uint16_t i = 0, n = b.U16(dir + 4); i < n; ++i) {
        size_t at = b.U32(dir + 12 + (size_t)i * 16 + 8), len = b.U32(dir + 12 + (size_t)i * 16 + 12);
        if (len && b.Has(at, len)) spans.push_back({at, len});
    }
    return spans;
}

inline int Mutations(const std::string& name, const std::string& bytes, int index, int rounds, int* opened,
                     const char* save = nullptr) {
    auto run = [&](const std::string& b) {
        if (save) fontmatch_checks::WriteFile(save, b);  // the copy a crash leaves behind
        Exercise(b, index), ++*opened;
    };
    size_t step = std::max<size_t>(1, bytes.size() / 64);
    for (size_t n = 0; n < bytes.size(); n += step) run(bytes.substr(0, n));
    auto spans = TableSpans(bytes, index);
    fontmatch_checks::Rng rng{0xD1B54A32D192ED03ull ^ bytes.size()};
    for (int r = 0; r < rounds; ++r) {
        std::string b = bytes;
        int edits = 1 + rng.Next() % 16;
        for (int e = 0; e < edits; ++e) {
            size_t at = rng.Next() % b.size();
            if (!spans.empty() && rng.Next() % 2) {  // within a table, often near its start, where its header and offsets are
                auto [start, len] = spans[rng.Next() % spans.size()];
                size_t reach = rng.Next() % 2 ? std::min<size_t>(len, 64) : len;
                at = start + rng.Next() % reach;
            }
            switch (rng.Next() % 5) {
            case 0: b[at] = (char)rng.Next(); break;
            case 1: b[at] = (char)0xFF; break;
            case 2: b[at] = 0; break;
            case 3: b[at] = (char)(b[at] + (rng.Next() % 2 ? 1 : -1)); break;
            default:
                for (size_t k = at; k < std::min(b.size(), at + 4); ++k) b[k] = (char)0xFF;
            }
        }
        run(b);
    }
    printf("ok   %s: %d malformed copies read without a crash\n", name.c_str(), rounds);
    return 0;
}

inline int Check(const char* dirArg, int extraCount, char** extra) {
    int failures = 0;
    auto check = [&](bool ok, const std::string& what) {
        printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
        failures += !ok;
    };
    type::SetPngDecoder(DecodePng);
    std::string dir = dirArg;
    const std::string boxFont = BoxFont("Box Sans");
    check(fontmatch_checks::WriteFile(dir + "/box.ttf", boxFont), "the box font fixture is written");

    // ---- The face and its outlines ----
    auto face = type::Face::Load(Bytes(boxFont), 0);
    check(face && face->unitsPerEm == 1000 && face->glyphCount == 4 && face->family == "Box Sans" && face->ascender == 800 &&
              face->descender == -200,
          "a TrueType face reads its em, glyph count, family and metrics");
    if (!face) return failures + 1;
    check(face->GlyphIndex('A') == 1 && face->GlyphIndex('B') == 2 && face->GlyphIndex('C') == 0 && face->Advance(1) == 700 &&
              face->LeftBearing(1) == 100,
          "the cmap and hmtx give glyphs, advances and bearings");
    check(face->KernTable(2, 1) == -100 && face->KernTable(1, 2) == 0, "the kern table's pair is found, and only that pair");
    type::Outline o;
    type::Coverage c;
    int32_t s10 = Scale(10, 1000);
    bool full = face->GetOutline(1, s10, s10, true, &o);
    type::Rasterize(o, false, &c);
    bool solid = c.pixels.size() == 35;
    for (uint8_t v : c.pixels) solid &= v == 255;
    check(full && c.left == 1 && c.top == 7 && c.width == 5 && c.rows == 7 && solid,
          "a box on whole pixels covers exactly its pixels, fully");
    face->GetOutline(2, s10, s10, true, &o);
    type::Rasterize(o, false, &c);
    bool half = c.width == 6 && c.rows == 5;
    for (int r = 0; half && r < c.rows; ++r)
        half = c.pixels[r * 6 + 4] == 255 && c.pixels[r * 6 + 5] >= 120 && c.pixels[r * 6 + 5] <= 136;
    check(half, "an edge halfway across a pixel antialiases it to half coverage");
    type::Rasterize(o, true, &c);
    bool mono = !c.pixels.empty();
    for (uint8_t v : c.pixels) mono &= v == 0 || v == 255;
    check(mono, "monochrome rasterizing gives only on and off pixels");

    // ---- Layout: kerning, ligatures, bold and measuring ----
    auto font = type::Font::Open(Bytes(boxFont), 0, 10);
    check(font && font->Ascent() == 8 && font->Descent() == -2 && font->Height() == 10, "a sized font gives SDL_ttf's ascent, descent and height");
    if (!font) return failures + 1;
    check(font->Width("A") == 7, "a glyph's width is its advance");
    check(font->Width("BA") == 12, "the kern table closes B and A by a pixel");
    const auto& lig = font->Layout("AB");
    check(lig.glyphs.size() == 1 && lig.glyphs[0].glyph == 3 && lig.w == 10, "GSUB's liga joins A and B into the ligature");
    int w = 0;
    size_t len = 0;
    font->Measure("BBAA", 13, &w, &len);  // B, B kerned to A, A, A
    check(len == 2 && w == 12, "measuring stops before the first glyph that doesn't fit");

    // ---- Carets: the pen at a byte offset, whichever way the line reads ----
    check(font->CaretX("AA", 1) == 7 && font->CaretX("AA", 2) == 14 && font->CaretX("AA", 0) == 0,
          "a left-to-right caret sits after the glyphs of its prefix");

    // ---- Hangul: conjoining jamo compose the syllable the face has ----
    {
        const Box s1{0, 0, 500, 700, 500}, s2{0, 0, 600, 700, 600};
        std::string bytes = GlyphFont("Hangul Box", {{0xAC00, s1}, {0xAC01, s2},
                                                     {0x1100, {0, 0, 200, 700, 200}}, {0x1161, {0, 0, 300, 700, 300}},
                                                     {0x11A8, {0, 0, 400, 700, 400}}});
        auto hangul = type::Font::Open(Bytes(bytes), 0, 10);
        auto jamoOnly = type::Font::Open(Bytes(GlyphFont("Jamo Box", {{0x1100, {0, 0, 200, 700, 200}},
                                                                     {0x1161, {0, 0, 300, 700, 300}},
                                                                     {0x11A8, {0, 0, 400, 700, 400}}})), 0, 10);
        if (hangul && jamoOnly) {
            auto g = [&](uint32_t cp) { return hangul->GetFace().GlyphIndex(cp); };
            const auto& lv = hangul->Layout(Cp(0x1100) + Cp(0x1161));
            check(lv.glyphs.size() == 1 && lv.glyphs[0].glyph == g(0xAC00),
                  "a leading and a vowel jamo compose the syllable the face has");
            const auto& lvt = hangul->Layout(Cp(0x1100) + Cp(0x1161) + Cp(0x11A8));
            check(lvt.glyphs.size() == 1 && lvt.glyphs[0].glyph == g(0xAC01),
                  "a trailing jamo joins the composed syllable");
            const auto& lvT = hangul->Layout(Cp(0xAC00) + Cp(0x11A8));
            check(lvT.glyphs.size() == 1 && lvT.glyphs[0].glyph == g(0xAC01),
                  "a precomposed syllable takes its trailing jamo the same way");
            const auto& plain = hangul->Layout(Cp(0xAC00));
            check(plain.glyphs.size() == 1 && plain.glyphs[0].glyph == g(0xAC00), "a plain syllable stays one glyph");
            const auto& unjoined = jamoOnly->Layout(Cp(0x1100) + Cp(0x1161) + Cp(0x11A8));
            check(unjoined.glyphs.size() == 3 && unjoined.glyphs[0].glyph == jamoOnly->GetFace().GlyphIndex(0x1100) &&
                      unjoined.glyphs[2].glyph == jamoOnly->GetFace().GlyphIndex(0x11A8),
                  "a face with only jamo keeps them, as HarfBuzz leaves what it cannot compose");
        }
    }

    // ---- Arabic: the presentation forms a face without positional lookups takes ----
    {
        const Box base{0, 0, 500, 700, 500};
        std::string bytes = GlyphFont("Arabic Box", {{0x0644, base}, {0x0645, base}, {0x0627, base}, {0x0633, base},
                                                     {0x064E, base},  // a mark: joining-transparent
                                                     {0xFEDF, {0, 0, 500, 700, 500}}, {0xFEE0, {0, 0, 600, 700, 600}},
                                                     {0xFEB1, {0, 0, 700, 700, 700}}, {0xFEB2, {0, 0, 800, 700, 800}},
                                                     {0xFEE4, {0, 0, 500, 700, 500}}, {0xFEE2, {0, 0, 600, 700, 600}},
                                                     {0xFEFB, {0, 0, 500, 700, 500}}, {0xFEFC, {0, 0, 600, 700, 600}}});
        auto arabic = type::Font::Open(Bytes(bytes), 0, 10);
        if (arabic) {
            auto g = [&](uint32_t cp) { return arabic->GetFace().GlyphIndex(cp); };
            const auto& la = arabic->Layout(Cp(0x0644) + Cp(0x0627));
            check(la.rtl && la.glyphs.size() == 1 && la.glyphs[0].glyph == g(0xFEFB),
                  "lam-alef becomes its isolated presentation ligature when nothing joins before");
            // lam, meem, lam, alef, seen: seen must keep its own isolated form even after the
            // ligature before it removed a letter from the buffer.
            const auto& mixed = arabic->Layout(Cp(0x0644) + Cp(0x0645) + Cp(0x0644) + Cp(0x0627) + Cp(0x0633));
            check(mixed.glyphs.size() == 4 && mixed.glyphs[0].glyph == g(0xFEB1) && mixed.glyphs[1].glyph == g(0xFEFC) &&
                      mixed.glyphs[2].glyph == g(0xFEE4) && mixed.glyphs[3].glyph == g(0xFEDF),
                  "every letter after a lam-alef ligature still takes its own form");
            // The mark between lam and alef is transparent to the ligature, as HarfBuzz's lookup
            // ignores marks, and stays a glyph of its own.
            const auto& marked = arabic->Layout(Cp(0x0644) + Cp(0x064E) + Cp(0x0627));
            check(marked.glyphs.size() == 2 && marked.glyphs[0].glyph == g(0x064E) && marked.glyphs[1].glyph == g(0xFEFB),
                  "a mark between lam and alef neither blocks the ligature nor disappears");
            // A caret at either end of a right-to-left line sits on the edge it belongs to.
            const std::string word = Cp(0x0644) + Cp(0x0645);
            check(arabic->CaretX(word, 0) == arabic->Width(word) && arabic->CaretX(word, word.size()) == 0,
                  "a right-to-left caret starts at the right edge and ends at the left");
        }
    }

    // ---- Japanese: kana and kanji are runs of their own scripts ----
    {
        std::vector<uint32_t> text{0x3042, 0x6F22, 0x3044};  // あ漢い
        bool rtl = false;
        auto runs = type::Itemize(text, &rtl);
        check(!rtl && runs.size() == 3 && runs[0].script == 0x48697261u /* Hira */ && runs[1].script == 0x48616E69u /* Hani */,
              "hiragana, kanji and hiragana shape as three runs, each under its own script");
    }
    font->SetBold(true);
    check(font->Width("A") == 8 && font->Image(1).width == 6, "synthetic bold widens each glyph and advance by the overhang");
    font->SetBold(false);
    font->SetHinting(type::Font::Hinting::Light);
    auto big = type::Font::Open(Bytes(boxFont), 0, 15);
    big->SetHinting(type::Font::Hinting::Light);
    check(big->Layout("AAA").width26 == 3 * 672 && big->Width("AAA") == 31,
          "light hinting keeps fractional advances, as HarfBuzz under SDL_ttf did");

    // ---- Collections ----
    fontmatch_checks::Spec one, two;
    one.family = "First Face", two.family = "Second Face";
    one.bmp = two.bmp = {{'a', 'z'}};
    std::string ttc = fontmatch_checks::Collection({one, two});
    auto second = type::Face::Load(Bytes(ttc), 1);
    check(type::Face::Count(ttc) == 2 && second && second->family == "Second Face" && !type::Face::Load(Bytes(ttc), 2),
          "a collection opens the face its index names, and no face past its count");

    // ---- Installed fonts ----
    auto installed = [&](const char* pattern, const char* needs, std::string* file, int* index) {
        lx::fonts::Match m;
        if (!lx::fonts::FindFont(pattern, &m) || m.file.empty()) return false;
        auto f = type::Face::Load(type::MapFile(m.file), m.index & 0xFFFF);
        if (!f) return false;
        for (const char* p = needs; *p;) {  // UTF-8, each code point must be mapped
            uint32_t cp = (unsigned char)*p;
            int n = cp < 0x80 ? 1 : cp < 0xE0 ? 2 : cp < 0xF0 ? 3 : 4;
            cp = n == 1 ? cp : cp & (0xFF >> (n + 1));
            for (int k = 1; k < n; ++k) cp = cp << 6 | (p[k] & 0x3F);
            p += n;
            if (!f->GlyphIndex(cp)) return false;
        }
        *file = m.file, *index = m.index & 0xFFFF;
        return true;
    };
    std::vector<std::pair<std::string, int>> fuzzFonts;
    std::string file;
    int index = 0;
    if (installed("sans-serif", "AV", &file, &index)) {
        auto f = type::Font::Open(type::MapFile(file), index, 32);
        check(f && f->Layout("AV").width26 < f->Layout("A").width26 + f->Layout("V").width26, "the Latin sans kerns A and V (" + file + ")");
        fuzzFonts.push_back({file, index});
    } else {
        printf("skip no installed Latin font\n");
    }
    if (installed("sans-serif:lang=ar", "سل", &file, &index)) {
        auto f = type::Font::Open(type::MapFile(file), index, 24);
        const auto& line = f->Layout("سلام");
        check(line.rtl && !line.glyphs.empty() && line.glyphs.back().offset == 0 && line.glyphs.back().glyph != f->GetFace().GlyphIndex(0x633),
              "Arabic runs right to left and its first letter takes its initial form (" + file + ")");
        fuzzFonts.push_back({file, index});
    } else {
        printf("skip no installed Arabic font\n");
    }
    if (installed("sans-serif:lang=hi", "कष", &file, &index)) {
        auto f = type::Font::Open(type::MapFile(file), index, 24);
        const auto& line = f->Layout("क्ष");
        check(line.glyphs.size() < 3, "Devanagari ka, virama and ssa form a conjunct (" + file + ")");
        const auto& matra = f->Layout("कि");
        check(matra.glyphs.size() == 2 && matra.glyphs[0].glyph != f->GetFace().GlyphIndex(0x915) &&
                  matra.glyphs[1].glyph == f->GetFace().GlyphIndex(0x915),
              "Devanagari's i matra is drawn before its consonant");
        fuzzFonts.push_back({file, index});
    } else {
        printf("skip no installed Devanagari font\n");
    }
    lx::fonts::Match cjk;
    type::FontData cjkData;
    if (lx::fonts::FindFont("sans-serif:lang=ja", &cjk) && (cjkData = type::MapFile(cjk.file)) && type::Face::Count(*cjkData) > 1) {
        auto f = type::Face::Load(cjkData, cjk.index & 0xFFFF);
        check(f && f->family == cjk.family && f->GlyphIndex(0x65E5), "a CJK collection opens the matched face, " + cjk.family);
        fuzzFonts.push_back({cjk.file, cjk.index & 0xFFFF});
    } else {
        printf("skip no installed CJK collection\n");
    }
    if (installed("emoji", "😀", &file, &index)) {
        auto f = type::Font::Open(type::MapFile(file), index, 16);
        if (f && f->GetFace().HasColor()) {
            const auto& img = f->Image(f->GetFace().GlyphIndex(0x1F600));
            check(img.color && img.rows >= 14 && img.rows <= 20 && img.width >= 14 && img.width <= 22,
                  "a color emoji's strike is scaled to the font's size (" + file + ")");
            const auto& seq = f->Layout("👨‍👩‍👧");
            check(seq.glyphs.size() == 1, "an emoji ZWJ sequence becomes its one glyph");
        } else {
            printf("skip the emoji font has no color strikes\n");
        }
        fuzzFonts.push_back({file, index});
    } else {
        printf("skip no installed emoji font\n");
    }
    if (installed("Nimbus Sans", "A", &file, &index) || installed("serif:fontformat=CFF", "A", &file, &index)) {
        auto f = type::Face::Load(type::MapFile(file), index);
        type::Outline co;
        type::Coverage cc;
        bool drawn = f && f->Table(type::Tag("CFF ")).n && f->GetOutline(f->GlyphIndex('A'), Scale(24, f->unitsPerEm), Scale(24, f->unitsPerEm), true, &co);
        if (drawn) type::Rasterize(co, false, &cc);
        if (f && f->Table(type::Tag("CFF ")).n) {
            check(drawn && cc.rows >= 15 && cc.rows <= 19, "a CFF face draws its capital at cap height (" + file + ")");
            fuzzFonts.push_back({file, index});
        }
    }

    // ---- Malformed copies ----
    int opened = 0;
    failures += Mutations("box font", boxFont, 0, 3000, &opened);
    failures += Mutations("collection", ttc, 1, 500, &opened);
    for (int i = 0; i < extraCount; ++i) {
        std::string bytes = fontmatch_checks::ReadAll(extra[i]);
        check(!bytes.empty() && type::Face::Load(Bytes(bytes), 0) != nullptr, std::string("the font ") + extra[i] + " opens");
        if (!bytes.empty()) failures += Mutations(extra[i], bytes, 0, 300, &opened);
    }
    for (auto& [path, idx] : fuzzFonts) {
        std::string bytes = fontmatch_checks::ReadAll(path);
        if (bytes.size() > 4 * 1024 * 1024) {  // a large collection: its face's tables, not every copy of the file
            Mutations(path, bytes, idx, 20, &opened);
        } else {
            failures += Mutations(path, bytes, idx, 150, &opened);
        }
    }
    printf("%d fonts opened and drawn\n", opened);
    if (failures) printf("%d text checks failed\n", failures);
    else printf("Text checks passed.\n");
    return failures ? 1 : 0;
}

// Unicode's BidiCharacterTest.txt. Lines with explicit embeddings or isolates are counted apart:
// the line layout doesn't implement them (they show as their neutral surroundings).
inline int CheckBidi(const char* path) {
    std::ifstream in(path);
    if (!in) {
        printf("FAIL %s can't be read\n", path);
        return 1;
    }
    std::string line;
    int total = 0, pass = 0, skipped = 0, shown = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> f;
        std::stringstream ss(line);
        for (std::string part; std::getline(ss, part, ';');) f.push_back(part);
        if (f.size() < 5) continue;
        std::vector<uint32_t> cps;
        std::stringstream c0(f[0]);
        for (std::string h; c0 >> h;) cps.push_back((uint32_t)std::stoul(h, nullptr, 16));
        bool explicitFormat = false;
        for (uint32_t c : cps) explicitFormat |= (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069);
        if (explicitFormat) {
            ++skipped;
            continue;
        }
        int dir = std::stoi(f[1]);
        std::vector<std::string> levels;
        std::stringstream c3(f[3]);
        for (std::string h; c3 >> h;) levels.push_back(h);
        bool rtl = false;
        auto runs = type::Itemize(cps, &rtl, dir == 2 ? -1 : dir);
        std::vector<int> got(cps.size());
        for (auto& r : runs)
            for (size_t i = r.start; i < r.end; ++i) got[i] = r.level;
        bool ok = (rtl ? 1 : 0) == std::stoi(f[2]) && levels.size() == cps.size();
        for (size_t i = 0; ok && i < cps.size(); ++i)
            if (levels[i] != "x" && std::stoi(levels[i]) != got[i]) ok = false;
        if (ok) {
            std::vector<int> want, seen;
            std::stringstream c4(f[4]);
            for (int v; c4 >> v;) want.push_back(v);
            for (size_t r : type::VisualOrder(runs)) {
                const auto& run = runs[r];
                for (size_t k = 0; k < run.end - run.start; ++k) {
                    size_t i = run.Rtl() ? run.end - 1 - k : run.start + k;
                    if (levels[i] != "x") seen.push_back((int)i);
                }
            }
            ok = seen == want;
        }
        ++total, pass += ok;
        if (!ok && shown++ < 10) printf("FAIL %s\n", line.c_str());
    }
    printf("%d/%d bidi lines give their levels and order (%d with explicit embeddings or isolates not run)\n", pass, total, skipped);
    return total && pass == total ? 0 : 1;
}

}  // namespace text_checks
