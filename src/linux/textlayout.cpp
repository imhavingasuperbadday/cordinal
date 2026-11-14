// Sized faces, line layout with fallbacks, and glyph images for the Linux text stack (textlayout.h).
#include "textlayout.h"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace lx::type {

namespace {

PngDecoder g_decoder = nullptr;
std::atomic<uint64_t> g_nextId{1};

int32_t PixRound(int32_t v) { return (v + 32) & ~63; }
int Floor26(int32_t v) { return (v & -64) / 64; }
int Ceil26(int32_t v) { return ((v + 63) & -64) / 64; }

// UTF-8 to code points with each one's byte offset; a malformed sequence reads as U+FFFD.
void Decode(std::string_view s, std::vector<uint32_t>* cps, std::vector<uint32_t>* offsets) {
    cps->clear(), offsets->clear();
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp = 0xFFFD;
        size_t len = 1;
        auto cont = [&](size_t k) { return i + k < s.size() && ((unsigned char)s[i + k] & 0xC0) == 0x80; };
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0 && c >= 0xC2 && cont(1)) {
            cp = (c & 0x1F) << 6 | (s[i + 1] & 0x3F), len = 2;
        } else if ((c & 0xF0) == 0xE0 && cont(1) && cont(2)) {
            cp = (c & 0x0F) << 12 | (s[i + 1] & 0x3F) << 6 | (s[i + 2] & 0x3F), len = 3;
            if (cp < 0x800 || (cp >= 0xD800 && cp < 0xE000)) cp = 0xFFFD;
        } else if ((c & 0xF8) == 0xF0 && cont(1) && cont(2) && cont(3)) {
            cp = (c & 0x07) << 18 | (s[i + 1] & 0x3F) << 12 | (s[i + 2] & 0x3F) << 6 | (s[i + 3] & 0x3F), len = 4;
            if (cp < 0x10000 || cp > 0x10FFFF) cp = 0xFFFD;
        }
        cps->push_back(cp);
        offsets->push_back((uint32_t)i);
        i += len;
    }
}

// Joiners and modifiers that belong with a missing emoji, so the emoji font gets the whole sequence.
bool EmojiGlue(uint32_t cp) {
    return cp == 0x200D || cp == 0xFE0F || cp == 0x20E3 || (cp >= 0x1F3FB && cp <= 0x1F3FF) || (cp >= 0xE0020 && cp <= 0xE007F);
}

// An area-averaging resize of unpremultiplied RGBA.
std::vector<uint8_t> Resize(const std::vector<uint8_t>& src, int sw, int sh, int dw, int dh) {
    std::vector<uint8_t> out((size_t)dw * dh * 4);
    double fx = (double)sw / dw, fy = (double)sh / dh;
    for (int y = 0; y < dh; ++y) {
        double y0 = y * fy, y1 = (y + 1) * fy;
        for (int x = 0; x < dw; ++x) {
            double x0 = x * fx, x1 = (x + 1) * fx;
            double acc[4] = {}, area = 0;
            for (int sy = (int)y0; sy < std::min(sh, (int)std::ceil(y1)); ++sy) {
                double wy = std::min(y1, sy + 1.0) - std::max(y0, (double)sy);
                for (int sx = (int)x0; sx < std::min(sw, (int)std::ceil(x1)); ++sx) {
                    double w = wy * (std::min(x1, sx + 1.0) - std::max(x0, (double)sx));
                    const uint8_t* p = &src[((size_t)sy * sw + sx) * 4];
                    double a = p[3] / 255.0 * w;
                    acc[0] += p[0] * a, acc[1] += p[1] * a, acc[2] += p[2] * a, acc[3] += a;
                    area += w;
                }
            }
            uint8_t* d = &out[((size_t)y * dw + x) * 4];
            if (acc[3] > 0) {
                for (int k = 0; k < 3; ++k) d[k] = (uint8_t)std::clamp(std::lround(acc[k] / acc[3]), 0L, 255L);
                d[3] = (uint8_t)std::clamp(std::lround(acc[3] / area * 255), 0L, 255L);
            }
        }
    }
    return out;
}

}  // namespace

void SetPngDecoder(PngDecoder decoder) { g_decoder = decoder; }

Font::~Font() = default;

std::unique_ptr<Font> Font::Open(FontData data, int faceIndex, float px) {
    if (!(px > 0) || px > 4096) return nullptr;
    auto face = Face::Load(std::move(data), faceIndex);
    if (!face || (!face->Scalable() && !face->HasColor())) return nullptr;
    std::unique_ptr<Font> f(new Font());
    f->face_ = face;
    f->px_ = px;
    f->id_ = g_nextId++;
    // FT_Set_Char_Size at 72 DPI, with TrueType's whole-pixel sizes when its head table asks.
    int32_t size26 = (int32_t)std::lround(px * 64);
    int upem = face->unitsPerEm;
    f->xScale_ = f->yScale_ = DivFix(size26, upem);
    f->ppem_ = (size26 + 32) >> 6;
    if (face->integerPpem && face->Scalable() && face->Table(Tag("glyf")).n)
        f->xScale_ = f->yScale_ = DivFix(f->ppem_ << 6, upem);
    f->ascent_ = Ceil26(MulFix(face->ascender, f->yScale_));
    f->descent_ = Ceil26(MulFix(face->descender, f->yScale_));
    f->height_ = Ceil26(MulFix(face->ascender - face->descender, f->yScale_));
    f->lineSkip_ = Ceil26(MulFix(face->ascender - face->descender + face->lineGap, f->yScale_));
    f->overhang_ = f->ppem_ / 10;
    f->shaper_ = std::make_unique<Shaper>(face.get());
    f->shapeFont_.face = face.get();
    f->shapeFont_.emX = (int32_t)(((uint64_t)(uint32_t)f->xScale_ * (uint64_t)upem + (1u << 15)) >> 16);
    f->shapeFont_.emY = (int32_t)(((uint64_t)(uint32_t)f->yScale_ * (uint64_t)upem + (1u << 15)) >> 16);
    f->shapeFont_.ppem = f->ppem_;
    Font* self = f.get();
    f->shapeFont_.advance = [self](uint16_t g) { return self->Advance(g); };
    return f;
}

void Font::SetBold(bool bold) {
    bool synth = bold && !face_->bold;
    if (synth == bold_) return;
    bold_ = synth;
    images_.clear(), lines_.clear();
}

void Font::SetHinting(Hinting hinting) {
    if (hinting == hinting_) return;
    hinting_ = hinting;
    mono_ = hinting == Hinting::Mono;
    images_.clear(), lines_.clear(), advances_.clear();
}

// The advance HarfBuzz got from FreeType: light hinting leaves it unrounded (FT_Get_Advance's
// 16.16 value, rounded to 26.6), and normal and mono hinting round it to whole pixels.
int32_t Font::Advance(uint16_t glyph) const {
    auto it = advances_.find(glyph);
    if (it != advances_.end()) return it->second;
    int32_t a;
    if (hinting_ != Hinting::Light) {
        a = PixRound(MulFix(face_->Advance(glyph), xScale_));
    } else {
        int64_t v = (int64_t)face_->Advance(glyph) * xScale_;
        v = (v + (v < 0 ? -32 : 32)) / 64;  // FT_MulDiv(advance, x_scale, 64)
        a = (int32_t)((v + (1 << 9)) >> 10);
    }
    advances_.emplace(glyph, a);
    return a;
}

void Font::Shape(const uint32_t* text, const uint32_t* clusters, size_t n, uint32_t script, bool rtl, Font* initial,
                 std::vector<Placed>* out) {
    out->clear();
    if (!n) return;
    std::vector<ShapedGlyph> shaped;
    shaper_->Shape(shapeFont_, text, clusters, n, script, rtl, &shaped);
    int32_t bold = bold_ ? overhang_ * 64 : 0;
    for (const ShapedGlyph& g : shaped) out->push_back({this, g.glyph, g.xOffset, g.yOffset, g.xAdvance + bold, g.cluster});
    // Spans of characters this face lacks go to each fallback in turn, as SDL_ttf's spans did.
    uint32_t first = clusters[0], end = clusters[0] + (uint32_t)n;
    for (Font* fb : fallbacks_) {
        if (fb == initial) continue;
        std::vector<bool> missing(out->size());
        bool any = false;
        for (size_t i = 0; i < out->size(); ++i) any |= (missing[i] = (*out)[i].glyph == 0);
        if (!any) break;
        for (int pass = 0; pass < 2; ++pass)
            for (size_t i = 0; i < out->size(); ++i) {
                if (missing[i]) continue;
                uint32_t c = (*out)[i].offset - first;
                if (c < n && EmojiGlue(text[c]) && ((i > 0 && missing[i - 1]) || (i + 1 < out->size() && missing[i + 1]))) missing[i] = true;
            }
        for (size_t i = out->size(); i-- > 0;) {
            if (!missing[i]) continue;
            size_t j = i;
            while (j > 0 && missing[j - 1]) --j;
            uint32_t lo = UINT32_MAX, hi = 0;
            for (size_t k = j; k <= i; ++k) lo = std::min(lo, (*out)[k].offset), hi = std::max(hi, (*out)[k].offset);
            uint32_t stop = end;
            for (const Placed& p : *out)
                if (p.offset > hi && p.offset < stop) stop = p.offset;
            std::vector<Placed> span;
            fb->Shape(text + (lo - first), clusters + (lo - first), stop - lo, script, rtl, initial, &span);
            if (!span.empty()) {
                size_t at = out->size();
                std::vector<Placed> kept;
                kept.reserve(out->size() + span.size());
                for (size_t k = 0; k < out->size(); ++k) {
                    const Placed& p = (*out)[k];
                    if (p.offset >= lo && p.offset < stop) {
                        if (at == out->size()) {
                            at = k;
                            kept.insert(kept.end(), span.begin(), span.end());
                        }
                        continue;
                    }
                    kept.push_back(p);
                }
                *out = std::move(kept);
            }
            i = j;
            if (i == 0) break;
            missing.resize(std::min(missing.size(), i));
        }
    }
}

const Font::Line& Font::Layout(std::string_view text) {
    std::string key(text);
    if (auto it = lines_.find(key); it != lines_.end()) return it->second;
    if (lines_.size() > 4096) lines_.clear();
    Line line;
    std::vector<uint32_t> cps, offsets;
    Decode(text, &cps, &offsets);
    bool rtlParagraph = false;
    std::vector<Run> runs = Itemize(cps, &rtlParagraph);
    line.rtl = rtlParagraph;
    std::vector<uint32_t> clusters(cps.size());
    for (size_t i = 0; i < cps.size(); ++i) clusters[i] = (uint32_t)i;
    for (size_t r : VisualOrder(runs)) {
        const Run& run = runs[r];
        std::vector<Placed> part;
        Shape(cps.data() + run.start, clusters.data() + run.start, run.end - run.start, run.script, run.Rtl(), this, &part);
        line.glyphs.insert(line.glyphs.end(), part.begin(), part.end());
    }
    // Pen positions; characters no font has show the first face's missing glyph.
    int32_t x = 0;
    int minx = 0, maxx = 0, miny = 0, maxy = height_;
    for (Placed& p : line.glyphs) {
        if (p.glyph == 0 && p.font != this) p = {this, 0, 0, 0, Advance(0) + (bold_ ? overhang_ * 64 : 0), p.offset};
        int32_t xo = p.x, yo = p.y;
        p.x = x + xo;
        p.y = p.font->ascent_ * 64 - yo;
        x += p.advance;
        p.offset = p.offset < offsets.size() ? offsets[p.offset] : (uint32_t)text.size();
        const GlyphImage& img = p.font->Image(p.glyph);
        int px = Floor26(p.x) + img.left, py = Floor26(p.y) - img.top;
        minx = std::min(minx, px), maxx = std::max(maxx, px + img.width);
        miny = std::min(miny, py), maxy = std::max(maxy, py + img.rows);
    }
    line.width26 = x;
    // SDL_ttf measures a line whose first glyph isn't its first character right to left, from the
    // far end back to 0, so there the pen's end doesn't widen the box.
    if (line.glyphs.empty() || line.glyphs[0].offset == 0) maxx = std::max(maxx, Floor26(x));
    line.xstart = minx < 0 ? -minx : 0;
    line.ystart = miny < 0 ? -miny : 0;
    line.w = maxx - minx;
    line.h = maxy - miny;
    return lines_.emplace(std::move(key), std::move(line)).first->second;
}

void Font::Measure(std::string_view text, int maxWidth, int* width, size_t* length) {
    const Line& line = Layout(text);
    *width = 0;
    *length = text.size();
    if (line.glyphs.empty()) return;
    int minx = 0, maxx = 0;
    auto box = [&](const Placed& p) {
        const GlyphImage& img = p.font->Image(p.glyph);
        int px = Floor26(p.x) + img.left;
        minx = std::min(minx, px), maxx = std::max(maxx, px + img.width);
    };
    auto fits = [&](int32_t x, const Placed& p) {
        int cw = std::max(maxx, Floor26(x)) - minx;
        if (!maxWidth || cw <= maxWidth) {
            *width = cw;
            return true;
        }
        *length = p.offset;
        return false;
    };
    if (line.glyphs[0].offset == 0) {
        int32_t x = 0;
        for (const Placed& p : line.glyphs) {
            box(p);
            x += p.advance;
            if (!fits(x, p)) break;
        }
    } else {
        int32_t x = line.width26;
        minx = Ceil26(line.width26);
        for (size_t i = line.glyphs.size(); i-- > 0;) {
            const Placed& p = line.glyphs[i];
            box(p);
            if (!fits(x, p)) break;
            x -= p.advance;
        }
    }
}

void Font::Quads(std::string_view text, std::vector<Quad>* out) {
    out->clear();
    int lineIndex = 0;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = start;
        while (end < text.size() && text[end] != '\n' && !(text[end] == '\r' && (end + 1 >= text.size() || text[end + 1] != '\n'))) ++end;
        std::string_view part = text.substr(start, end - start);
        if (!part.empty()) {
            const Line& line = Layout(part);
            for (const Placed& p : line.glyphs) {
                const GlyphImage& img = p.font->Image(p.glyph);
                if (img.width <= 0 || img.rows <= 0) continue;
                out->push_back({p.font, p.glyph, line.xstart + Floor26(p.x) + img.left,
                                line.ystart + lineIndex * lineSkip_ + Floor26(p.y) - img.top});
            }
        }
        if (end >= text.size()) break;
        start = end + 1;
        ++lineIndex;
    }
}

const GlyphImage& Font::Image(uint16_t glyph) {
    auto it = images_.find(glyph);
    if (it != images_.end()) return it->second;
    if (images_.size() > 8192) images_.clear();
    GlyphImage img;
    ColorImage color;
    bool done = false;
    if (face_->HasColor() && g_decoder && face_->GetColorImage(glyph, ppem_, &color)) {
        int w = 0, h = 0;
        std::vector<uint8_t> rgba;
        if (g_decoder(color.png, &w, &h, &rgba) && w > 0 && h > 0 && w <= 4096 && h <= 4096 && rgba.size() >= (size_t)w * h * 4) {
            // Strikes come in a few sizes; this one is scaled to the font's.
            double s = color.ppem > 0 ? (double)ppem_ / color.ppem : 1;
            int dw = std::max(1, (int)std::lround(w * s)), dh = std::max(1, (int)std::lround(h * s));
            img.pixels = dw == w && dh == h ? std::move(rgba) : Resize(rgba, w, h, dw, dh);
            img.width = dw, img.rows = dh;
            img.left = (int)std::lround(color.left * s);
            img.top = (int)std::lround(color.top * s);
            img.color = true;
            done = true;
        }
    }
    if (!done && face_->Scalable()) {
        Outline outline;
        Coverage cov;
        if (face_->GetOutline(glyph, xScale_, yScale_, true, &outline)) Rasterize(outline, mono_, &cov);
        img.left = cov.left, img.top = cov.top, img.width = cov.width, img.rows = cov.rows;
        img.pixels = std::move(cov.pixels);
        if (bold_) {
            // SDL_ttf's emboldening: the image widens by the overhang and each pass adds its left neighbour.
            int w = img.width + overhang_;
            std::vector<uint8_t> wide((size_t)w * img.rows, 0);
            for (int r = 0; r < img.rows; ++r)
                std::copy_n(&img.pixels[(size_t)r * img.width], img.width, &wide[(size_t)r * w]);
            for (int r = 0; r < img.rows; ++r) {
                uint8_t* row = &wide[(size_t)r * w];
                for (int pass = 1; pass <= overhang_; ++pass)
                    for (int col = w - 1; col > 0; --col)
                        row[col] = mono_ ? (row[col] | row[col - 1]) : (uint8_t)std::min(255, row[col] + row[col - 1]);
            }
            img.pixels = std::move(wide);
            img.width = w;
        }
    }
    return images_.emplace(glyph, std::move(img)).first->second;
}

}  // namespace lx::type
