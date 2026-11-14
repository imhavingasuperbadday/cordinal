// Text shaping for the Linux text stack (text.h), in place of HarfBuzz: a line is split into runs
// by a bidi pass (UAX #9 without explicit embeddings) and by script, and each run is shaped with
// the face's OpenType GSUB and GPOS lookups (every lookup type but GPOS cursive attachment), the
// kern table where GPOS has no kerning, Unicode composition and mark reordering, Arabic joining
// (with presentation forms for a font without positional lookups), mirrored characters in
// right-to-left runs, Devanagari-family syllable reordering (reph, pre-base matras, half and below
// forms), Thai and Lao SARA AM, and hidden default ignorables. Positions are in 26.6 pixels, scaled
// the way HarfBuzz's FreeType font scales them, so widths agree with what SDL_ttf measured.
#pragma once
#include "typeface.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace lx::type {

// A face at a size: FreeType's 16.16 scale, HarfBuzz's 26.6 em scale and the hinted advance.
struct ShapeFont {
    const Face* face = nullptr;
    int32_t emX = 0, emY = 0;                     // 26.6 pixels per em
    int ppem = 0;                                 // whole pixels per em, for device-table adjustments
    std::function<int32_t(uint16_t)> advance;  // a glyph's advance in 26.6 pixels
};

struct ShapedGlyph {
    uint16_t glyph = 0;
    uint32_t cluster = 0;  // the caller's cluster value for its first character
    int32_t xAdvance = 0, yAdvance = 0, xOffset = 0, yOffset = 0;
};

// A run of a line: code points [start, end) at one bidi level and script (an ISO 15924 tag such
// as 'Latn', or 'Zyyy' when only common characters).
struct Run {
    size_t start = 0, end = 0;
    uint32_t script = 0;
    int level = 0;
    bool Rtl() const { return level & 1; }
};

// The runs of one line in logical order, and whether the paragraph reads right to left: from its
// first strong character when `paragraph` is -1, else 0 left to right or 1 right to left.
std::vector<Run> Itemize(const std::vector<uint32_t>& text, bool* rtlParagraph, int paragraph = -1);
// The runs' indices left to right on screen (UAX #9 rule L2).
std::vector<size_t> VisualOrder(const std::vector<Run>& runs);

uint32_t ScriptOf(uint32_t ch);     // its ISO 15924 tag, 'Zyyy' common, 'Zinh' inherited
bool IsDefaultIgnorable(uint32_t ch);
bool IsMark(uint32_t ch);           // general category M
bool IsEmojiPresentation(uint32_t ch);
bool JoinsGraphemes(uint32_t ch);   // continues the previous character's grapheme (marks, joiners, selectors, modifiers)

// Shapes one face's runs. Plans per script and direction are kept, so keep one per face.
class Shaper {
public:
    explicit Shaper(const Face* face);
    ~Shaper();
    // Shapes text[0, n) (one run) with clusters[i] the cluster value of text[i]. Glyphs come out
    // left to right on screen: reversed from the text for a right-to-left run.
    void Shape(const ShapeFont& font, const uint32_t* text, const uint32_t* clusters, size_t n, uint32_t script, bool rtl,
               std::vector<ShapedGlyph>* out);

    struct Plan;

private:
    const Plan& PlanFor(uint32_t script, bool rtl);
    const Face* face_;
    std::map<uint64_t, std::unique_ptr<Plan>> plans_;
};

}  // namespace lx::type
