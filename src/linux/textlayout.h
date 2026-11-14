// A face at a pixel size for the Linux text stack (text.h): metrics, laid-out lines and glyph
// images, with the conventions SDL_ttf 3.2 used so widths and placement stay as they were: sizes
// at 72 DPI, FreeType's rounding of metrics and hinted advances, a script fallback chain that
// re-shapes each run of characters the face lacks with the next face, SDL_ttf's ink-box string
// widths, and its synthetic bold (a smear of y_ppem / 10 pixels). Color emoji strikes are scaled
// to the requested size. Nothing here draws: text.cpp puts the images in an atlas.
#pragma once
#include "raster.h"
#include "shaper.h"
#include "typeface.h"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lx::type {

// PNG decoding for color glyphs, provided by the front end (stb_image): RGBA, unpremultiplied.
using PngDecoder = bool (*)(std::string_view png, int* width, int* height, std::vector<uint8_t>* rgba);
void SetPngDecoder(PngDecoder decoder);

struct GlyphImage {
    int left = 0, top = 0, width = 0, rows = 0;  // as SDL_ttf's sz_left, sz_top, sz_width, sz_rows
    bool color = false;
    std::vector<uint8_t> pixels;  // coverage, or RGBA for a color glyph
};

class Font {
public:
    // Face `faceIndex` of the font file in `data` at `px` pixels, or null when it isn't a font.
    static std::unique_ptr<Font> Open(FontData data, int faceIndex, float px);

    void SetBold(bool bold);  // synthetic bold, as TTF_STYLE_BOLD: none for a face that is bold already
    // SDL_ttf's hinting modes, as far as they show without a hinter: Normal (its default) and Mono
    // round advances to whole pixels, Light keeps them fractional, and Mono draws on/off pixels.
    enum class Hinting { Normal, Light, Mono };
    void SetHinting(Hinting hinting);
    void AddFallback(Font* font) { fallbacks_.push_back(font); }  // not owned

    const Face& GetFace() const { return *face_; }
    float Size() const { return px_; }
    int Height() const { return height_; }   // TTF_GetFontHeight
    int Ascent() const { return ascent_; }
    int Descent() const { return descent_; }
    int LineSkip() const { return lineSkip_; }
    bool HasGlyph(uint32_t ch) const { return face_->GlyphIndex(ch) != 0; }

    struct Placed {
        Font* font;
        uint16_t glyph;
        int32_t x, y;     // 26.6 pen position (y: the font's ascent less the glyph's offset)
        int32_t advance;  // 26.6
        uint32_t offset;  // byte offset of its cluster in the line
    };
    struct Line {
        std::vector<Placed> glyphs;  // left to right on screen
        bool rtl = false;            // the paragraph's direction
        int32_t width26 = 0;
        int w = 0, h = 0;            // TTF_GetStringSize
        int xstart = 0, ystart = 0;  // where the pen starts so no ink is left of or above the box
    };
    // One line of UTF-8 text (newlines are characters here; DrawLines splits them).
    const Line& Layout(std::string_view text);
    int Width(std::string_view text) { return Layout(text).w; }
    // TTF_MeasureString: the widest prefix within maxWidth pixels, its width and byte length.
    void Measure(std::string_view text, int maxWidth, int* width, size_t* length);

    struct Quad {
        const Font* font;
        uint16_t glyph;
        int x, y;  // the image's top-left corner relative to the text's
    };
    // The glyph images of text broken at newlines, as TTF_DrawRendererText places them.
    void Quads(std::string_view text, std::vector<Quad>* out);

    const GlyphImage& Image(uint16_t glyph);
    uint64_t Id() const { return id_; }  // unique per font, for atlas keys

    ~Font();

private:
    Font() = default;
    void Shape(const uint32_t* text, const uint32_t* clusters, size_t n, uint32_t script, bool rtl, Font* initial,
               std::vector<Placed>* out);
    int32_t Advance(uint16_t glyph) const;

    std::shared_ptr<Face> face_;
    std::unique_ptr<Shaper> shaper_;
    ShapeFont shapeFont_;
    float px_ = 0;
    int32_t xScale_ = 0, yScale_ = 0;
    int ppem_ = 0;
    int ascent_ = 0, descent_ = 0, height_ = 0, lineSkip_ = 0, overhang_ = 0;
    bool bold_ = false, mono_ = false;
    Hinting hinting_ = Hinting::Normal;
    std::vector<Font*> fallbacks_;
    std::unordered_map<uint16_t, GlyphImage> images_;
    mutable std::unordered_map<uint16_t, int32_t> advances_;
    std::unordered_map<std::string, Line> lines_;
    uint64_t id_ = 0;
};

}  // namespace lx::type
