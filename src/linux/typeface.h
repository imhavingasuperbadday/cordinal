// One face of a TrueType, OpenType or collection file, read in-repo for the text stack (text.h):
// metrics, the Unicode cmap, TrueType (glyf) and CFF/CFF2 outlines scaled the way FreeType scales
// them, CBDT and sbix color bitmaps (PNG data, decoded by the caller), the kern table, and the
// GSUB, GPOS and GDEF tables the shaper (shaper.h) reads. Every read is bounds-checked: a damaged
// table reads as absent or empty, never past the buffer.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace lx::type {

// A bounds-checked big-endian view: a read past the end gives 0.
struct Bytes {
    const unsigned char* p = nullptr;
    size_t n = 0;
    bool Has(size_t at, size_t len) const { return at <= n && len <= n - at; }
    uint8_t U8(size_t at) const { return Has(at, 1) ? p[at] : 0; }
    int8_t S8(size_t at) const { return (int8_t)U8(at); }
    uint16_t U16(size_t at) const { return Has(at, 2) ? (uint16_t)(p[at] << 8 | p[at + 1]) : 0; }
    int16_t S16(size_t at) const { return (int16_t)U16(at); }
    uint32_t U24(size_t at) const { return Has(at, 3) ? (uint32_t)p[at] << 16 | (uint32_t)p[at + 1] << 8 | p[at + 2] : 0; }
    uint32_t U32(size_t at) const {
        return Has(at, 4) ? (uint32_t)p[at] << 24 | (uint32_t)p[at + 1] << 16 | (uint32_t)p[at + 2] << 8 | p[at + 3] : 0;
    }
    Bytes Sub(size_t at, size_t len = SIZE_MAX) const {
        if (!Has(at, 0)) return {};
        return {p + at, len < n - at ? len : n - at};
    }
    bool Empty() const { return n == 0; }
};

constexpr uint32_t Tag(const char (&t)[5]) {
    return (uint32_t)(uint8_t)t[0] << 24 | (uint32_t)(uint8_t)t[1] << 16 | (uint32_t)(uint8_t)t[2] << 8 | (uint8_t)t[3];
}

// FreeType's 16.16 helpers, so sizes and rounding agree with what SDL_ttf drew before.
int32_t MulFix(int32_t a, int32_t b);
int32_t DivFix(int32_t a, int32_t b);

// A glyph outline in 26.6 pixels, y up, laid out as FT_Outline: points, a tag per point (1 on the
// curve, 0 a quadratic control, 2 a cubic control) and the last point of each contour.
struct Outline {
    std::vector<int32_t> x, y;
    std::vector<uint8_t> tag;
    std::vector<uint16_t> ends;
    void Clear() { x.clear(), y.clear(), tag.clear(), ends.clear(); }
    void Add(int32_t px, int32_t py, uint8_t t) { x.push_back(px), y.push_back(py), tag.push_back(t); }
    bool Box(int32_t* xMin, int32_t* yMin, int32_t* xMax, int32_t* yMax) const;  // the control box; false when empty
};

// A color glyph image from the strike nearest the requested size: PNG data, its size in pixels at
// the strike's ppem, and the offset of its top-left corner from the glyph origin (y up).
struct ColorImage {
    std::string_view png;
    int ppem = 0;
    int width = 0, height = 0;
    int left = 0, top = 0;
    int advance = 0;  // at the strike's ppem, 0 when the strike gives none
};

// A font file's bytes, shared by every face and size opened from it: a read-only mapping of the
// file, or bytes in memory (pack fonts, the built-in pixel font).
using FontData = std::shared_ptr<const std::string_view>;
FontData MapFile(const std::string& path);  // null when it can't be read
FontData FromBytes(std::shared_ptr<const std::string> bytes);

class Face {
public:
    // Face `index` of the file in `data` (the low 16 bits; a TTC's face count bounds it), or null
    // when it isn't a font with a usable cmap.
    static std::shared_ptr<Face> Load(FontData data, int index);
    static int Count(std::string_view data);  // faces in a file: 1 for a plain font, 0 for no font

    int unitsPerEm = 1000;
    int ascender = 0, descender = 0, lineGap = 0;  // as FreeType's face metrics: hhea, else OS/2
    int underlinePosition = 0, underlineThickness = 0;
    bool integerPpem = false;                     // head flags bit 3: sizes round to whole pixels
    bool bold = false, italic = false;            // FreeType's style flags (OS/2 fsSelection, else macStyle)
    std::string family, style;                    // the names FreeType reports
    int glyphCount = 0;

    bool Scalable() const { return outlineKind_ != 0; }
    bool HasColor() const { return !cbdt_.Empty() || !sbix_.Empty(); }
    bool ColorOnly() const { return !Scalable() && HasColor(); }

    uint16_t GlyphIndex(uint32_t ch) const;                    // 0 when unmapped
    uint16_t VariantGlyph(uint32_t ch, uint32_t selector) const; // cmap 14's non-default variant, else 0
    int Advance(uint16_t glyph) const;                         // font units
    int LeftBearing(uint16_t glyph) const;
    bool GetOutline(uint16_t glyph, int32_t xScale, int32_t yScale, bool roundComposites, Outline* out) const;
    bool GetColorImage(uint16_t glyph, int ppem, ColorImage* out) const;
    int KernTable(uint16_t left, uint16_t right) const;  // the kern table's horizontal pair value, font units

    Bytes Table(uint32_t tag) const;
    Bytes gsub, gpos, gdef;
    const FontData& Data() const { return data_; }

    struct Cff;  // the parsed CFF or CFF2 table (typeface.cpp)

private:
    bool ReadCmap();
    bool GlyfOutline(uint16_t glyph, int32_t xScale, int32_t yScale, bool roundComposites, int depth, Outline* out) const;
    bool CffOutline(uint16_t glyph, int32_t xScale, int32_t yScale, Outline* out) const;
    bool CbdtImage(uint16_t glyph, int ppem, ColorImage* out) const;
    bool SbixImage(uint16_t glyph, int ppem, ColorImage* out) const;

    FontData data_;
    Bytes file_;
    std::vector<std::pair<uint32_t, Bytes>> tables_;
    Bytes cmap_, cmap14_, hmtx_, glyf_, loca_, kern_, cbdt_, cblc_, sbix_;
    int cmapFormat_ = 0;
    bool symbolCmap_ = false;
    int numHMetrics_ = 0;
    bool longLoca_ = false;
    int outlineKind_ = 0;  // 0 none, 1 glyf, 2 CFF, 3 CFF2
    std::shared_ptr<Cff> cff_;
};

}  // namespace lx::type
