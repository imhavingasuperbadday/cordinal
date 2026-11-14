// The software 2D renderer behind SDL_Renderer's calls, drawing into the window's pixels. It keeps
// the semantics (and, wherever the player can see the difference, the arithmetic) of SDL 3.2's
// software renderer, which the Linux build drew with before: rectangles and texture copies take
// their coordinates truncated to whole pixels; pairs of triangles that form an axis-aligned
// rectangle become a fill or a copy; other triangles are rasterized at whole-pixel vertices with the
// top-left rule; blending is "source over" with SDL's rounding (exact division for fills, lines and
// triangles, the (x + 1 + (x + 1) / 256) / 256 approximation for texture copies); textures scale
// by nearest neighbour or SDL's 7-bit bilinear filter. Pixels are 0xAARRGGBB words throughout.
#include "platform/platform.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>

struct SDL_Texture {
    SDL_Renderer* renderer = nullptr;
    int w = 0, h = 0;
    SDL_PixelFormat format = SDL_PIXELFORMAT_ARGB8888;
    bool alpha = true;  // the format has an alpha channel
    std::vector<uint32_t> px;  // 0xAARRGGBB
    SDL_BlendMode blend = SDL_BLENDMODE_BLEND;
    SDL_ScaleMode scale = SDL_SCALEMODE_LINEAR;
    // Colour and alpha modulation. SDL's software renderer sets these from a textured quad's vertex
    // colour and leaves them so, which later plain copies of the texture then use too.
    SDL_FColor mod{1, 1, 1, 1};
};

struct SDL_Renderer {
    SDL_Window* window = nullptr;
    uint8_t r = 0, g = 0, b = 0, a = 255;
    SDL_BlendMode blend = SDL_BLENDMODE_NONE;
    bool clipOn = false;
    SDL_Rect clip{};
    std::set<SDL_Texture*> textures;
    // This frame's target.
    uint32_t* px = nullptr;
    int w = 0, h = 0, stride = 0;
};

namespace lx::platform {
namespace {

// A 0xAARRGGBB picture to draw into or read from.
struct Image {
    uint32_t* px;
    int w, h, stride;
    uint32_t& at(int x, int y) const { return px[(size_t)y * stride + x]; }
};

inline uint32_t Ch(uint32_t p, int shift) { return (p >> shift) & 0xff; }
inline uint32_t Pack(uint32_t a, uint32_t r, uint32_t g, uint32_t b) { return a << 24 | r << 16 | g << 8 | b; }
// SDL's MULT_DIV_255 and blend rounding: (x + 1 + ((x + 1) >> 8)) >> 8.
inline uint32_t Div255(uint32_t x) {
    x += 1;
    x += x >> 8;
    return x >> 8;
}
inline uint32_t Mul255(uint32_t a, uint32_t b) { return Div255(a * b); }
inline uint32_t DrawMul(uint32_t a, uint32_t b) { return a * b / 255; }  // SDL_draw.h's exact division

bool Intersect(const SDL_Rect& a, const SDL_Rect& b, SDL_Rect& out) {
    if (a.w <= 0 || a.h <= 0 || b.w <= 0 || b.h <= 0) {
        out = {0, 0, 0, 0};
        return false;
    }
    int x0 = std::max(a.x, b.x), x1 = std::min(a.x + a.w, b.x + b.w);
    int y0 = std::max(a.y, b.y), y1 = std::min(a.y + a.h, b.y + b.h);
    out = {x0, y0, x1 - x0, y1 - y0};
    return out.w > 0 && out.h > 0;
}

bool IntersectF(const SDL_FRect& a, const SDL_FRect& b, SDL_FRect& out) {
    if (a.w < 0 || a.h < 0 || b.w < 0 || b.h < 0) {
        out = {0, 0, 0, 0};
        return false;
    }
    float x0 = std::max(a.x, b.x), x1 = std::min(a.x + a.w, b.x + b.w);
    float y0 = std::max(a.y, b.y), y1 = std::min(a.y + a.h, b.y + b.h);
    out = {x0, y0, x1 - x0, y1 - y0};
    return out.w >= 0 && out.h >= 0;
}

uint8_t ColorByte(float c) { return (uint8_t)std::round(std::clamp(c, 0.0f, 1.0f) * 255.0f); }

SDL_Rect ClipRect(const SDL_Renderer* r) {
    SDL_Rect full{0, 0, r->w, r->h};
    if (!r->clipOn) return full;
    SDL_Rect out;
    Intersect(full, r->clip, out);
    return out;
}

Image Target(SDL_Renderer* r) { return {r->px, r->w, r->h, r->stride}; }

// --- blits ----------------------------------------------------------------------------------------------------------

struct CopyState {
    uint8_t r = 255, g = 255, b = 255, a = 255;  // colour and alpha modulation
    SDL_BlendMode blend = SDL_BLENDMODE_NONE;
    bool srcAlpha = true;
    bool dstAlpha = false;  // drawing into an ARGB copy (keeps alpha) rather than the window
};

// One pixel through SDL's generic blitter (blit_auto): modulate, premultiply, then "over".
inline uint32_t BlendAuto(uint32_t s, uint32_t d, const CopyState& st, bool modC, bool modA) {
    uint32_t sr = Ch(s, 16), sg = Ch(s, 8), sb = Ch(s, 0), sa = st.srcAlpha ? Ch(s, 24) : 255;
    if (modC) sr = Mul255(sr, st.r), sg = Mul255(sg, st.g), sb = Mul255(sb, st.b);
    if (modA) sa = Mul255(sa, st.a);
    if (st.blend == SDL_BLENDMODE_NONE) return Pack(0xff, sr, sg, sb);
    if (sa < 255) sr = Mul255(sr, sa), sg = Mul255(sg, sa), sb = Mul255(sb, sa);
    uint32_t dr = std::min<uint32_t>(255, Mul255(255 - sa, Ch(d, 16)) + sr);
    uint32_t dg = std::min<uint32_t>(255, Mul255(255 - sa, Ch(d, 8)) + sg);
    uint32_t db = std::min<uint32_t>(255, Mul255(255 - sa, Ch(d, 0)) + sb);
    return Pack(0xff, dr, dg, db);
}

// SDL's per-pixel alpha blitter (blit_A), used when nothing is modulated.
inline uint32_t BlendPixelAlpha(uint32_t s, uint32_t d) {
    uint32_t a = Ch(s, 24), inv = 255 - a;
    return Pack(0xff, Div255(Ch(s, 16) * a + Ch(d, 16) * inv), Div255(Ch(s, 8) * a + Ch(d, 8) * inv),
                Div255(Ch(s, 0) * a + Ch(d, 0) * inv));
}

// SDL_BlitSurface: same size, clipped to the source and the target's clip rectangle.
void Blit(const Image& src, SDL_Rect s, const Image& dst, int dx, int dy, const SDL_Rect& clip, const CopyState& st) {
    SDL_Rect full{0, 0, src.w, src.h}, ss;
    if (!Intersect(s, full, ss)) return;
    SDL_Rect d{dx + ss.x - s.x, dy + ss.y - s.y, ss.w, ss.h}, dd;
    if (!Intersect(d, clip, dd)) return;
    ss.x += dd.x - d.x;
    ss.y += dd.y - d.y;
    ss.w = dd.w;
    ss.h = dd.h;
    const bool modC = (st.r & st.g & st.b) != 0xff, modA = st.a != 0xff;
    const bool pixelAlpha = st.blend == SDL_BLENDMODE_BLEND && !modC && !modA && st.srcAlpha;
    for (int y = 0; y < dd.h; ++y) {
        const uint32_t* sp = &src.at(ss.x, ss.y + y);
        uint32_t* dp = &dst.at(dd.x, dd.y + y);
        if (st.blend == SDL_BLENDMODE_NONE && !modC && !modA) {
            const uint32_t fill = st.dstAlpha && st.srcAlpha ? 0 : 0xff000000u;
            for (int x = 0; x < dd.w; ++x) dp[x] = sp[x] | fill;
        } else if (pixelAlpha) {
            for (int x = 0; x < dd.w; ++x) dp[x] = BlendPixelAlpha(sp[x], dp[x]);
        } else {
            for (int x = 0; x < dd.w; ++x) dp[x] = BlendAuto(sp[x], dp[x], st, modC, modA);
        }
    }
}

// SDL's nearest stretch: 16.16 steps starting half a step in.
void StretchNearest(const Image& src, const SDL_Rect& s, const Image& dst, const SDL_Rect& d, const CopyState* st) {
    const uint64_t incy = ((uint64_t)s.h << 16) / d.h, incx = ((uint64_t)s.w << 16) / d.w;
    const bool modC = st && (st->r & st->g & st->b) != 0xff, modA = st && st->a != 0xff;
    uint64_t posy = incy / 2;
    for (int y = 0; y < d.h; ++y, posy += incy) {
        const uint32_t* row = &src.at(s.x, s.y + (int)(posy >> 16));
        uint32_t* dp = &dst.at(d.x, d.y + y);
        uint64_t posx = incx / 2;
        for (int x = 0; x < d.w; ++x, posx += incx) {
            uint32_t p = row[posx >> 16];
            dp[x] = st ? BlendAuto(p, dp[x], *st, modC, modA) : p;
        }
    }
}

// SDL's bilinear stretch (SDL_stretch.c's scale_mat): 7-bit weights, pixman's first sample.
void StretchLinear(const Image& src, const SDL_Rect& s, const Image& dst, const SDL_Rect& d) {
    constexpr int kPrecision = 7, kOne = 1 << kPrecision;
    auto setup = [](int srcN, int dstN, int64_t& start, int& step, int& leftPad, int& rightPad) {
        step = (int)(((uint32_t)srcN << 16) / (uint32_t)dstN);
        int x0 = 1 << 15;
        int64_t t0 = (int64_t)step * (x0 >> 16), t1 = (int64_t)step * (x0 & 0xffff);
        x0 = (int)(t0 + ((t1 + 0x8000) >> 16));
        x0 -= 1 << 15;
        start = x0;
        leftPad = rightPad = 0;
        int64_t sum = x0;
        for (int i = 0; i < dstN; ++i, sum += step) {
            if (sum < 0) ++leftPad;
            else if ((int)((uint32_t)sum >> 16) > srcN - 2) ++rightPad;
        }
    };
    auto frac = [](int64_t fp) { return (int)(((uint32_t)(fp >> (16 - kPrecision))) & (kOne - 1)); };
    auto lerp = [](uint32_t c0, uint32_t c1, int f0, int f1) {  // c0 * f1 + c1 * f0, per byte, >> 7
        uint32_t out = 0;
        for (int sh = 0; sh < 32; sh += 8)
            out |= (((((c0 >> sh) & 0xff) * f1 + ((c1 >> sh) & 0xff) * f0) >> kPrecision) & 0xff) << sh;
        return out;
    };
    int64_t sumH, sumW;
    int stepH, stepW, leftH, rightH, leftW, rightW;
    setup(s.h, d.h, sumH, stepH, leftH, rightH);
    setup(s.w, d.w, sumW, stepW, leftW, rightW);
    const int64_t sumWInit = sumW + (int64_t)leftW * stepW;
    const int middle = d.w - leftW - rightW;
    for (int i = 0; i < d.h; ++i) {
        const bool noPad = !(i < leftH || i > d.h - 1 - rightH);
        int indexH = (int)((uint32_t)sumH >> 16), fracH0 = frac(sumH);
        indexH = noPad ? indexH : (i < leftH ? 0 : s.h - 1);
        fracH0 = noPad ? fracH0 : 0;
        const uint32_t* r0 = &src.at(s.x, s.y + indexH);
        const uint32_t* r1 = noPad ? r0 + src.stride : r0;
        sumH += stepH;
        const int fracH1 = kOne - fracH0;
        uint32_t* dp = &dst.at(d.x, d.y + i);
        // Vertical first, then horizontal. A weight of 0 or 1 reads only the one column it keeps
        // (SDL reads the other too, past the edge of a one-pixel-wide source).
        auto bilinear = [&](const uint32_t* p0, const uint32_t* p1, int fracW0) {
            if (fracW0 == 0) return lerp(p0[0], p1[0], fracH0, fracH1);
            if (fracW0 == kOne) return lerp(p0[1], p1[1], fracH0, fracH1);
            uint32_t t0 = lerp(p0[0], p1[0], fracH0, fracH1), t1 = lerp(p0[1], p1[1], fracH0, fracH1);
            return lerp(t0, t1, fracW0, kOne - fracW0);
        };
        int64_t sw = sumWInit;
        int x = 0;
        for (int k = 0; k < leftW; ++k) dp[x++] = bilinear(r0, r1, 0);
        for (int k = 0; k < middle; ++k) {
            int index = (int)((uint32_t)sw >> 16), f = frac(sw);
            sw += stepW;
            dp[x++] = bilinear(r0 + index, r1 + index, f);
        }
        for (int k = 0; k < rightW; ++k) dp[x++] = bilinear(r0 + s.w - 2, r1 + s.w - 2, kOne);  // reads column s.w - 1
    }
}

// SDL_BlitSurfaceScaled: clip in doubles, round, then scale (and blend through a scaled copy).
void BlitScaled(const Image& src, const SDL_Rect& s, const Image& dst, const SDL_Rect& d, const SDL_Rect& clip,
                const CopyState& st, SDL_ScaleMode mode) {
    if (s.w == d.w && s.h == d.h) {
        Blit(src, s, dst, d.x, d.y, clip, st);
        return;
    }
    if (s.w <= 0 || s.h <= 0 || d.w <= 0 || d.h <= 0) return;
    const double sw = (double)d.w / s.w, sh = (double)d.h / s.h;
    double dx0 = d.x, dy0 = d.y, dx1 = dx0 + d.w, dy1 = dy0 + d.h;
    double sx0 = s.x, sy0 = s.y, sx1 = sx0 + s.w, sy1 = sy0 + s.h;
    if (sx0 < 0) dx0 -= sx0 * sw, sx0 = 0;
    if (sx1 > src.w) dx1 -= (sx1 - src.w) * sw, sx1 = src.w;
    if (sy0 < 0) dy0 -= sy0 * sh, sy0 = 0;
    if (sy1 > src.h) dy1 -= (sy1 - src.h) * sh, sy1 = src.h;
    dx0 -= clip.x, dx1 -= clip.x, dy0 -= clip.y, dy1 -= clip.y;
    if (dx0 < 0) sx0 -= dx0 / sw, dx0 = 0;
    if (dx1 > clip.w) sx1 -= (dx1 - clip.w) / sw, dx1 = clip.w;
    if (dy0 < 0) sy0 -= dy0 / sh, dy0 = 0;
    if (dy1 > clip.h) sy1 -= (dy1 - clip.h) / sh, dy1 = clip.h;
    dx0 += clip.x, dx1 += clip.x, dy0 += clip.y, dy1 += clip.y;
    SDL_Rect fs{(int)std::round(sx0), (int)std::round(sy0), (int)std::round(sx1 - sx0), (int)std::round(sy1 - sy0)};
    SDL_Rect fd{(int)std::round(dx0), (int)std::round(dy0), (int)std::round(dx1 - dx0), (int)std::round(dy1 - dy0)};
    Intersect(SDL_Rect{0, 0, src.w, src.h}, fs, fs);
    Intersect(clip, fd, fd);
    if (fd.w <= 0 || fd.h <= 0 || fs.w <= 0 || fs.h <= 0) return;
    const bool complex = st.blend != SDL_BLENDMODE_NONE || (st.r & st.g & st.b & st.a) != 0xff;
    if (mode == SDL_SCALEMODE_NEAREST) {
        StretchNearest(src, fs, dst, fd, complex ? &st : nullptr);
        return;
    }
    if (!complex && st.srcAlpha == st.dstAlpha) {  // the same format on both sides
        StretchLinear(src, fs, dst, fd);
        return;
    }
    // Scale into a copy, then blit it with the blend and modulation.
    std::vector<uint32_t> tmp((size_t)fd.w * fd.h);
    Image t{tmp.data(), fd.w, fd.h, fd.w};
    StretchLinear(src, fs, t, SDL_Rect{0, 0, fd.w, fd.h});
    Blit(t, SDL_Rect{0, 0, fd.w, fd.h}, dst, fd.x, fd.y, clip, st);
}

// --- primitives -----------------------------------------------------------------------------------------------------

void FillRects(SDL_Renderer* r, const SDL_Rect* rects, int n, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t ca,
               SDL_BlendMode blend) {
    const SDL_Rect clip = ClipRect(r);
    Image t = Target(r);
    uint32_t pr = cr, pg = cg, pb = cb, inv = 255 - ca;
    if (blend == SDL_BLENDMODE_BLEND) pr = DrawMul(cr, ca), pg = DrawMul(cg, ca), pb = DrawMul(cb, ca);
    const uint32_t solid = Pack(0xff, cr, cg, cb);
    for (int i = 0; i < n; ++i) {
        SDL_Rect c;
        if (!Intersect(rects[i], clip, c)) continue;
        for (int y = c.y; y < c.y + c.h; ++y) {
            uint32_t* p = &t.at(c.x, y);
            if (blend == SDL_BLENDMODE_NONE) {
                std::fill(p, p + c.w, solid);
            } else {
                for (int x = 0; x < c.w; ++x) {
                    uint32_t d = p[x];
                    p[x] = Pack(0xff, DrawMul(inv, Ch(d, 16)) + pr, DrawMul(inv, Ch(d, 8)) + pg, DrawMul(inv, Ch(d, 0)) + pb);
                }
            }
        }
    }
}

struct Pen {
    Image t;
    uint32_t pr, pg, pb, inv, solid;
    bool blend;
    void Set(int x, int y) const {
        uint32_t& d = t.at(x, y);
        if (!blend) d = solid;
        else d = Pack(0xff, DrawMul(inv, Ch(d, 16)) + pr, DrawMul(inv, Ch(d, 8)) + pg, DrawMul(inv, Ch(d, 0)) + pb);
    }
};

// Cohen-Sutherland, as SDL_GetRectAndLineIntersection does it.
bool ClipLine(const SDL_Rect& rect, int& x1, int& y1, int& x2, int& y2) {
    if (rect.w <= 0 || rect.h <= 0) return false;
    const int rx1 = rect.x, ry1 = rect.y, rx2 = rect.x + rect.w - 1, ry2 = rect.y + rect.h - 1;
    auto inside = [&](int x, int y) { return x >= rx1 && x <= rx2 && y >= ry1 && y <= ry2; };
    if (inside(x1, y1) && inside(x2, y2)) return true;
    if ((x1 < rx1 && x2 < rx1) || (x1 > rx2 && x2 > rx2) || (y1 < ry1 && y2 < ry1) || (y1 > ry2 && y2 > ry2)) return false;
    if (y1 == y2) {  // horizontal
        x1 = std::clamp(x1, rx1, rx2);
        x2 = std::clamp(x2, rx1, rx2);
        return true;
    }
    if (x1 == x2) {
        y1 = std::clamp(y1, ry1, ry2);
        y2 = std::clamp(y2, ry1, ry2);
        return true;
    }
    auto code = [&](int x, int y) {
        int c = 0;
        if (y < ry1) c |= 2;
        else if (y > ry2) c |= 4;
        if (x < rx1) c |= 1;
        else if (x > rx2) c |= 8;
        return c;
    };
    int c1 = code(x1, y1), c2 = code(x2, y2);
    while (c1 || c2) {
        if (c1 & c2) return false;
        int x = 0, y = 0;
        int c = c1 ? c1 : c2;
        if (c & 2) y = ry1, x = x1 + (int)(((int64_t)(x2 - x1) * (ry1 - y1)) / (y2 - y1));
        else if (c & 4) y = ry2, x = x1 + (int)(((int64_t)(x2 - x1) * (ry2 - y1)) / (y2 - y1));
        else if (c & 1) x = rx1, y = y1 + (int)(((int64_t)(y2 - y1) * (rx1 - x1)) / (x2 - x1));
        else x = rx2, y = y1 + (int)(((int64_t)(y2 - y1) * (rx2 - x1)) / (x2 - x1));
        if (c == c1) x1 = x, y1 = y, c1 = code(x, y);
        else x2 = x, y2 = y, c2 = code(x, y);
    }
    return true;
}

void Line(const Pen& p, int x1, int y1, int x2, int y2, bool drawEnd) {
    if (y1 == y2) {
        int from = std::min(x1, x2), n = std::abs(x2 - x1) + (drawEnd ? 1 : 0);
        if (!drawEnd && x1 > x2) ++from;
        for (int i = 0; i < n; ++i) p.Set(from + i, y1);
    } else if (x1 == x2) {
        int from = std::min(y1, y2), n = std::abs(y2 - y1) + (drawEnd ? 1 : 0);
        if (!drawEnd && y1 > y2) ++from;
        for (int i = 0; i < n; ++i) p.Set(x1, from + i);
    } else if (std::abs(x1 - x2) == std::abs(y1 - y2)) {
        int x, y, step, n;
        if (y1 <= y2) x = x1, y = y1, step = x1 <= x2 ? 1 : -1, n = y2 - y1;
        else {
            x = x2, y = y2, step = x2 <= x1 ? 1 : -1, n = y1 - y2;
            if (!drawEnd) x += step, ++y;
        }
        if (drawEnd) ++n;
        for (int i = 0; i < n; ++i) p.Set(x + i * step, y + i);
    } else {  // Bresenham
        int dx = std::abs(x2 - x1), dy = std::abs(y2 - y1), n, d, inc1, inc2, xi1, xi2, yi1, yi2;
        if (dx >= dy) n = dx + 1, d = 2 * dy - dx, inc1 = dy * 2, inc2 = (dy - dx) * 2, xi1 = 1, xi2 = 1, yi1 = 0, yi2 = 1;
        else n = dy + 1, d = 2 * dx - dy, inc1 = dx * 2, inc2 = (dx - dy) * 2, xi1 = 0, xi2 = 1, yi1 = 1, yi2 = 1;
        if (x1 > x2) xi1 = -xi1, xi2 = -xi2;
        if (y1 > y2) yi1 = -yi1, yi2 = -yi2;
        int x = x1, y = y1;
        if (!drawEnd) --n;
        for (int i = 0; i < n; ++i) {
            p.Set(x, y);
            if (d < 0) d += inc1, x += xi1, y += yi1;
            else d += inc2, x += xi2, y += yi2;
        }
    }
}

// --- triangles (SDL_triangle.c's rasterizer: vertices at whole pixels, one fixed-point bit) ---------------

struct TriVertex {
    SDL_Point dst;  // fixed point (x2)
    SDL_Point src;  // texel
    SDL_Color c;
};

int64_t Cross(const SDL_Point& a, const SDL_Point& b, int cx, int cy) {
    return (int64_t)(b.x - a.x) * (cy - a.y) - (int64_t)(b.y - a.y) * (cx - a.x);
}
bool TopLeft(const SDL_Point& a, const SDL_Point& b, bool cw) {
    if (cw) return (a.y == b.y && a.x < b.x) || b.y < a.y;
    return (a.y == b.y && b.x < a.x) || a.y < b.y;
}

struct Raster {
    SDL_Rect box;
    int64_t area;
    int64_t w0row, w1row, w2row;
    int d2d1y, d0d2y, d1d0y, d1d2x, d2d0x, d0d1x;
    int bias0, bias1, bias2;
};

bool Setup(const SDL_Point& d0, const SDL_Point& d1, const SDL_Point& d2, const SDL_Rect* bounds, const SDL_Rect& clip,
           Raster& R) {
    int64_t area = Cross(d0, d1, d2.x, d2.y);
    if (area == 0) return false;
    int minx = std::min({d0.x, d1.x, d2.x}), maxx = std::max({d0.x, d1.x, d2.x});
    int miny = std::min({d0.y, d1.y, d2.y}), maxy = std::max({d0.y, d1.y, d2.y});
    R.box = {minx >> 1, miny >> 1, (maxx - minx) >> 1, (maxy - miny) >> 1};
    if (bounds) Intersect(R.box, *bounds, R.box);
    Intersect(R.box, clip, R.box);
    const bool cw = area > 0;
    R.area = std::abs(area);
    R.d2d1y = (d1.y - d2.y) * 2, R.d0d2y = (d2.y - d0.y) * 2, R.d1d0y = (d0.y - d1.y) * 2;
    R.d1d2x = (d2.x - d1.x) * 2, R.d2d0x = (d0.x - d2.x) * 2, R.d0d1x = (d1.x - d0.x) * 2;
    const int px = R.box.x * 2 + 1, py = R.box.y * 2 + 1;
    R.w0row = Cross(d1, d2, px, py), R.w1row = Cross(d2, d0, px, py), R.w2row = Cross(d0, d1, px, py);
    if (!cw) {
        R.d2d1y = -R.d2d1y, R.d0d2y = -R.d0d2y, R.d1d0y = -R.d1d0y;
        R.d1d2x = -R.d1d2x, R.d2d0x = -R.d2d0x, R.d0d1x = -R.d0d1x;
        R.w0row = -R.w0row, R.w1row = -R.w1row, R.w2row = -R.w2row;
    }
    R.bias0 = TopLeft(d1, d2, cw) ? 0 : -1;
    R.bias1 = TopLeft(d2, d0, cw) ? 0 : -1;
    R.bias2 = TopLeft(d0, d1, cw) ? 0 : -1;
    return R.box.w > 0 && R.box.h > 0;
}

template <class F> void Walk(const Raster& R, F&& f) {
    int64_t w0r = R.w0row, w1r = R.w1row, w2r = R.w2row;
    for (int y = 0; y < R.box.h; ++y) {
        int64_t w0 = w0r, w1 = w1r, w2 = w2r;
        for (int x = 0; x < R.box.w; ++x) {
            if (w0 + R.bias0 >= 0 && w1 + R.bias1 >= 0 && w2 + R.bias2 >= 0) f(x, y, w0, w1, w2);
            w0 += R.d2d1y, w1 += R.d0d2y, w2 += R.d1d0y;
        }
        w0r += R.d1d2x, w1r += R.d2d0x, w2r += R.d0d1x;
    }
}

void FillTriangle(SDL_Renderer* r, const TriVertex& v0, const TriVertex& v1, const TriVertex& v2, SDL_BlendMode blend) {
    Raster R;
    SDL_Rect bounds{0, 0, r->w, r->h};
    if (!Setup(v0.dst, v1.dst, v2.dst, &bounds, ClipRect(r), R)) return;
    const SDL_Color c0 = v0.c, c1 = v1.c, c2 = v2.c;
    const bool uniform = !memcmp(&c0, &c1, 4) && !memcmp(&c1, &c2, 4);
    auto color = [&](int64_t w0, int64_t w1, int64_t w2) {
        if (uniform) return Pack(c0.a, c0.r, c0.g, c0.b);
        auto mix = [&](int a, int b, int c) { return (uint32_t)(uint8_t)((w0 * a + w1 * b + w2 * c) / R.area); };
        return Pack(mix(c0.a, c1.a, c2.a), mix(c0.r, c1.r, c2.r), mix(c0.g, c1.g, c2.g), mix(c0.b, c1.b, c2.b));
    };
    Image t = Target(r);
    if (blend == SDL_BLENDMODE_NONE) {
        Walk(R, [&](int x, int y, int64_t w0, int64_t w1, int64_t w2) {
            t.at(R.box.x + x, R.box.y + y) = color(w0, w1, w2) | 0xff000000u;
        });
        return;
    }
    // SDL draws into a transparent ARGB copy of the box, then blends it over.
    std::vector<uint32_t> tmp((size_t)R.box.w * R.box.h, 0);
    Walk(R, [&](int x, int y, int64_t w0, int64_t w1, int64_t w2) { tmp[(size_t)y * R.box.w + x] = color(w0, w1, w2); });
    for (int y = 0; y < R.box.h; ++y) {
        uint32_t* d = &t.at(R.box.x, R.box.y + y);
        const uint32_t* s = &tmp[(size_t)y * R.box.w];
        for (int x = 0; x < R.box.w; ++x) d[x] = BlendPixelAlpha(s[x], d[x]);
    }
}

void BlitTriangle(SDL_Renderer* r, SDL_Texture* tex, TriVertex v0, TriVertex v1, TriVertex v2) {
    // The texel interpolation reaches its maximum, so the largest coordinate is pulled in by one.
    {
        int minx = std::min({v0.src.x, v1.src.x, v2.src.x}), maxx = std::max({v0.src.x, v1.src.x, v2.src.x});
        int miny = std::min({v0.src.y, v1.src.y, v2.src.y}), maxy = std::max({v0.src.y, v1.src.y, v2.src.y});
        for (TriVertex* v : {&v0, &v1, &v2}) {
            if (maxx > minx && v->src.x == maxx) --v->src.x;
            if (maxy > miny && v->src.y == maxy) --v->src.y;
        }
    }
    Raster R;
    if (!Setup(v0.dst, v1.dst, v2.dst, nullptr, ClipRect(r), R)) return;
    const SDL_Color c0 = v0.c, c1 = v1.c, c2 = v2.c;
    const bool uniform = !memcmp(&c0, &c1, 4) && !memcmp(&c1, &c2, 4);
    const bool modC = c0.r != 255 || c1.r != 255 || c2.r != 255 || c0.g != 255 || c1.g != 255 || c2.g != 255 ||
                      c0.b != 255 || c1.b != 255 || c2.b != 255;
    const bool modA = c0.a != 255 || c1.a != 255 || c2.a != 255;
    const int s2s0x = v0.src.x - v2.src.x, s2s1x = v1.src.x - v2.src.x, s2s0y = v0.src.y - v2.src.y,
              s2s1y = v1.src.y - v2.src.y;
    const int64_t s2xa = (int64_t)v2.src.x * R.area, s2ya = (int64_t)v2.src.y * R.area;
    const bool blend = tex->blend == SDL_BLENDMODE_BLEND;
    Image t = Target(r);
    Walk(R, [&](int x, int y, int64_t w0, int64_t w1, int64_t w2) {
        int sx = (int)((w0 * s2s0x + w1 * s2s1x + s2xa) / R.area), sy = (int)((w0 * s2s0y + w1 * s2s1y + s2ya) / R.area);
        sx = std::clamp(sx, 0, tex->w - 1);
        sy = std::clamp(sy, 0, tex->h - 1);
        uint32_t s = tex->px[(size_t)sy * tex->w + sx];
        uint32_t sr = Ch(s, 16), sg = Ch(s, 8), sb = Ch(s, 0), sa = tex->alpha ? Ch(s, 24) : 255;
        uint32_t mr = c0.r, mg = c0.g, mb = c0.b, ma = c0.a;
        if (!uniform) {
            mr = (uint32_t)((w0 * c0.r + w1 * c1.r + w2 * c2.r) / R.area);
            mg = (uint32_t)((w0 * c0.g + w1 * c1.g + w2 * c2.g) / R.area);
            mb = (uint32_t)((w0 * c0.b + w1 * c1.b + w2 * c2.b) / R.area);
            ma = (uint32_t)((w0 * c0.a + w1 * c1.a + w2 * c2.a) / R.area);
        }
        if (modC) sr = sr * mr / 255, sg = sg * mg / 255, sb = sb * mb / 255;
        if (modA) sa = sa * ma / 255;
        uint32_t& d = t.at(R.box.x + x, R.box.y + y);
        if (!blend) {
            d = Pack(0xff, sr, sg, sb);
            return;
        }
        if (sa < 255) sr = sr * sa / 255, sg = sg * sa / 255, sb = sb * sa / 255;
        d = Pack(0xff, std::min<uint32_t>(255, sr + (255 - sa) * Ch(d, 16) / 255),
                 std::min<uint32_t>(255, sg + (255 - sa) * Ch(d, 8) / 255), std::min<uint32_t>(255, sb + (255 - sa) * Ch(d, 0) / 255));
    });
}

// --- texture copies (SDL_RenderTexture through the software backend) --------------------------------------------

void Copy(SDL_Renderer* r, SDL_Texture* tex, const SDL_FRect& srcF, const SDL_FRect& dstF, const SDL_FColor& mod) {
    SDL_Rect s{(int)srcF.x, (int)srcF.y, (int)srcF.w, (int)srcF.h};
    SDL_Rect d{(int)dstF.x, (int)dstF.y, (int)dstF.w, (int)dstF.h};
    CopyState st;
    st.r = ColorByte(mod.r), st.g = ColorByte(mod.g), st.b = ColorByte(mod.b), st.a = ColorByte(mod.a);
    st.blend = tex->blend;
    st.srcAlpha = tex->alpha;
    Image src{tex->px.data(), tex->w, tex->h, tex->w};
    Image dst = Target(r);
    const SDL_Rect clip = ClipRect(r);
    if (s.w == d.w && s.h == d.h) {
        Blit(src, s, dst, d.x, d.y, clip, st);
        return;
    }
    if (d.w <= 0 || d.h <= 0) return;
    if (d.x < 0 || d.y < 0 || d.x + d.w > dst.w || d.y + d.h > dst.h) {
        // Scaled whole into a copy first, so clipping at the edges doesn't change the proportions.
        std::vector<uint32_t> tmp((size_t)d.w * d.h, 0);
        Image t{tmp.data(), d.w, d.h, d.w};
        CopyState plain;
        plain.srcAlpha = plain.dstAlpha = tex->alpha;
        BlitScaled(src, s, t, SDL_Rect{0, 0, d.w, d.h}, SDL_Rect{0, 0, d.w, d.h}, plain, tex->scale);
        if (!tex->alpha)
            for (uint32_t& p : tmp) p |= 0xff000000u;
        Blit(t, SDL_Rect{0, 0, d.w, d.h}, dst, d.x, d.y, clip, st);
        return;
    }
    BlitScaled(src, s, dst, d, clip, st, tex->scale);
}

bool Begin(SDL_Renderer* r) {
    if (!r || !r->window || !r->window->native) return false;
    r->px = r->window->native->Framebuffer(r->w, r->h, r->stride);
    return r->px != nullptr;
}

}  // namespace

SDL_Renderer* RendererForWindow(SDL_Window* w) { return w ? w->renderer : nullptr; }

void RendererWindowGone(SDL_Window* w) {
    if (w && w->renderer) w->renderer->window = nullptr;
}

}  // namespace lx::platform

using namespace lx::platform;

// --- the API --------------------------------------------------------------------------------------------------------

SDL_Renderer* SDL_CreateRenderer(SDL_Window* window, const char*) {
    if (!window) {
        SDL_SetError("no window");
        return nullptr;
    }
    if (window->renderer) {
        SDL_SetError("Renderer already associated with window");
        return nullptr;
    }
    auto* r = new SDL_Renderer;
    r->window = window;
    window->renderer = r;
    return r;
}

void SDL_DestroyRenderer(SDL_Renderer* r) {
    if (!r) return;
    for (SDL_Texture* t : r->textures) t->renderer = nullptr;
    if (r->window) r->window->renderer = nullptr;
    delete r;
}

const char* SDL_GetRendererName(SDL_Renderer*) { return SDL_SOFTWARE_RENDERER; }
bool SDL_SetRenderVSync(SDL_Renderer*, int) { return true; }

bool SDL_GetRenderOutputSize(SDL_Renderer* r, int* w, int* h) {
    if (!r || !r->window) return false;
    if (w) *w = r->window->pw;
    if (h) *h = r->window->ph;
    return true;
}

bool SDL_SetRenderDrawColor(SDL_Renderer* r, Uint8 cr, Uint8 cg, Uint8 cb, Uint8 ca) {
    if (!r) return false;
    r->r = cr, r->g = cg, r->b = cb, r->a = ca;
    return true;
}

bool SDL_SetRenderDrawBlendMode(SDL_Renderer* r, SDL_BlendMode mode) {
    if (!r) return false;
    r->blend = mode;
    return true;
}

bool SDL_SetRenderClipRect(SDL_Renderer* r, const SDL_Rect* rect) {
    if (!r) return false;
    r->clipOn = rect != nullptr;
    r->clip = rect ? *rect : SDL_Rect{};
    return true;
}

bool SDL_GetRenderClipRect(SDL_Renderer* r, SDL_Rect* rect) {
    if (!r) return false;
    if (rect) *rect = r->clipOn ? r->clip : SDL_Rect{};
    return true;
}

bool SDL_RenderClipEnabled(SDL_Renderer* r) { return r && r->clipOn; }

bool SDL_RenderClear(SDL_Renderer* r) {
    if (!Begin(r)) return false;
    const uint32_t c = Pack(0xff, r->r, r->g, r->b);
    for (int y = 0; y < r->h; ++y) std::fill(r->px + (size_t)y * r->stride, r->px + (size_t)y * r->stride + r->w, c);
    return true;
}

bool SDL_RenderFillRects(SDL_Renderer* r, const SDL_FRect* rects, int count) {
    if (!Begin(r) || !rects) return false;
    std::vector<SDL_Rect> ir((size_t)std::max(count, 0));
    for (int i = 0; i < count; ++i)
        ir[i] = {(int)rects[i].x, (int)rects[i].y, std::max((int)rects[i].w, 1), std::max((int)rects[i].h, 1)};
    FillRects(r, ir.data(), count, r->r, r->g, r->b, r->a, r->blend);
    return true;
}

bool SDL_RenderFillRect(SDL_Renderer* r, const SDL_FRect* rect) {
    if (!r) return false;
    SDL_FRect full{0, 0, (float)(r->window ? r->window->pw : 0), (float)(r->window ? r->window->ph : 0)};
    return SDL_RenderFillRects(r, rect ? rect : &full, 1);
}

bool SDL_RenderLines(SDL_Renderer* r, const SDL_FPoint* points, int count) {
    if (!Begin(r) || !points) return false;
    if (count < 2) return true;
    std::vector<SDL_Point> p((size_t)count);
    for (int i = 0; i < count; ++i) p[i] = {(int)points[i].x, (int)points[i].y};
    Pen pen;
    pen.t = Target(r);
    pen.blend = r->blend == SDL_BLENDMODE_BLEND;
    pen.solid = Pack(0xff, r->r, r->g, r->b);
    pen.pr = pen.blend ? DrawMul(r->r, r->a) : r->r;
    pen.pg = pen.blend ? DrawMul(r->g, r->a) : r->g;
    pen.pb = pen.blend ? DrawMul(r->b, r->a) : r->b;
    pen.inv = 255 - r->a;
    const SDL_Rect clip = ClipRect(r);
    for (int i = 1; i < count; ++i) {
        int x1 = p[i - 1].x, y1 = p[i - 1].y, x2 = p[i].x, y2 = p[i].y;
        if (!ClipLine(clip, x1, y1, x2, y2)) continue;
        bool drawEnd = x2 != p[i].x || y2 != p[i].y;
        if (!pen.blend && x1 == x2 && y1 == y2) drawEnd = true;
        Line(pen, x1, y1, x2, y2, drawEnd);
    }
    if (p[0].x != p[count - 1].x || p[0].y != p[count - 1].y) {
        const SDL_Point& last = p[count - 1];
        if (last.x >= clip.x && last.y >= clip.y && last.x < clip.x + clip.w && last.y < clip.y + clip.h) pen.Set(last.x, last.y);
    }
    return true;
}

bool SDL_RenderRect(SDL_Renderer* r, const SDL_FRect* rect) {
    if (!r) return false;
    SDL_FRect f = rect ? *rect : SDL_FRect{0, 0, (float)(r->window ? r->window->pw : 0), (float)(r->window ? r->window->ph : 0)};
    SDL_FPoint pts[5] = {{f.x, f.y}, {f.x + f.w - 1, f.y}, {f.x + f.w - 1, f.y + f.h - 1}, {f.x, f.y + f.h - 1}, {f.x, f.y}};
    return SDL_RenderLines(r, pts, 5);
}

bool SDL_RenderTexture(SDL_Renderer* r, SDL_Texture* tex, const SDL_FRect* src, const SDL_FRect* dst) {
    if (!tex || !Begin(r) || tex->renderer != r) return false;
    SDL_FRect real{0, 0, (float)tex->w, (float)tex->h};
    if (src && (!IntersectF(*src, real, real) || real.w == 0 || real.h == 0)) return true;
    SDL_FRect full{0, 0, (float)r->w, (float)r->h};
    Copy(r, tex, real, dst ? *dst : full, tex->mod);
    return true;
}

bool SDL_RenderGeometry(SDL_Renderer* r, SDL_Texture* tex, const SDL_Vertex* v, int numVertices, const int* indices,
                        int numIndices) {
    if (!Begin(r) || !v) return false;
    if (tex && tex->renderer != r) return false;
    const int count = indices ? numIndices : numVertices;
    if (count < 3) return true;
    auto same = [&](int a, int b) {
        return !memcmp(&v[a].position, &v[b].position, sizeof(SDL_FPoint)) && !memcmp(&v[a].color, &v[b].color, sizeof(SDL_FColor)) &&
               (!tex || !memcmp(&v[a].tex_coord, &v[b].tex_coord, sizeof(SDL_FPoint)));
    };
    auto vertex = [&](int k) {
        TriVertex t;
        t.dst = {(int)v[k].position.x * 2, (int)v[k].position.y * 2};
        if (tex) t.src = {(int)(v[k].tex_coord.x * tex->w), (int)(v[k].tex_coord.y * tex->h)};
        t.c = {ColorByte(v[k].color.r), ColorByte(v[k].color.g), ColorByte(v[k].color.b), ColorByte(v[k].color.a)};
        return t;
    };
    auto triangle = [&](const int* k) {
        if (tex) BlitTriangle(r, tex, vertex(k[0]), vertex(k[1]), vertex(k[2]));
        else FillTriangle(r, vertex(k[0]), vertex(k[1]), vertex(k[2]), r->blend);
    };
    int prev[3] = {-1, -1, -1};
    for (int i = 0; i + 2 < count; i += 3) {
        int k[3];
        for (int j = 0; j < 3; ++j) {
            if (indices) {
                k[j] = indices[i + j];
            } else {  // a vertex repeated from the previous triangle is that one (SDL's remap_indices)
                k[j] = i + j;
                for (int q = 0; q < 3; ++q)
                    if (prev[q] != -1 && same(i + j, prev[q])) {
                        k[j] = prev[q];
                        break;
                    }
            }
            if (k[j] < 0 || k[j] >= numVertices) return false;
        }
        if (prev[0] == -1) {
            std::copy(k, k + 3, prev);
            continue;
        }
        int shared = 0;
        for (int q = 0; q < 3; ++q) shared += prev[q] == k[0] || prev[q] == k[1] || prev[q] == k[2];
        bool quad = shared == 2;
        int A = -1, B = -1, C = -1, C2 = -1;
        if (quad) {
            const SDL_FPoint p0 = v[k[0]].position, p1 = v[k[1]].position, p2 = v[k[2]].position;
            if (p0.x <= p1.x && p0.y <= p1.y) A = (p0.x <= p2.x && p0.y <= p2.y) ? k[0] : k[2];
            else A = (p1.x <= p2.x && p1.y <= p2.y) ? k[1] : k[2];
            if (p0.x >= p1.x && p0.y >= p1.y) B = (p0.x >= p2.x && p0.y >= p2.y) ? k[0] : k[2];
            else B = (p1.x >= p2.x && p1.y >= p2.y) ? k[1] : k[2];
            C = (k[0] != A && k[0] != B) ? k[0] : (k[1] != A && k[1] != B) ? k[1] : k[2];
            C2 = (prev[0] != A && prev[0] != B) ? prev[0] : (prev[1] != A && prev[1] != B) ? prev[1] : prev[2];
            const SDL_FPoint a = v[A].position, b = v[B].position;
            auto rectangular = [&](SDL_FPoint c) { return (a.x == c.x && b.y == c.y) || (a.y == c.y && b.x == c.x); };
            if (!rectangular(v[C].position) || !rectangular(v[C2].position)) quad = false;
        }
        if (quad) {
            const SDL_FColor c = v[A].color;
            quad = !memcmp(&c, &v[B].color, sizeof c) && !memcmp(&c, &v[C].color, sizeof c) && !memcmp(&c, &v[C2].color, sizeof c);
        }
        if (quad) {
            const SDL_FColor col = v[k[0]].color;
            SDL_FRect d{v[A].position.x, v[A].position.y, v[B].position.x - v[A].position.x, v[B].position.y - v[A].position.y};
            if (tex) {
                const float tw = (float)tex->w, th = (float)tex->h;
                SDL_FRect s{v[A].tex_coord.x * tw, v[A].tex_coord.y * th, 0, 0};
                s.w = v[B].tex_coord.x * tw - s.x;
                s.h = v[B].tex_coord.y * th - s.y;
                if (s.w != 0 && s.h != 0) {
                    tex->mod = col;
                    if (s.w > 0 && s.h > 0) {
                        SDL_RenderTexture(r, tex, &s, &d);
                    } else {  // mirrored: SDL rotates a copy here; the player never draws one, so triangles do
                        triangle(prev);
                        triangle(k);
                    }
                }
            } else if (d.w != 0 && d.h != 0) {
                SDL_Rect ir{(int)d.x, (int)d.y, std::max((int)d.w, 1), std::max((int)d.h, 1)};
                FillRects(r, &ir, 1, ColorByte(col.r), ColorByte(col.g), ColorByte(col.b), ColorByte(col.a), SDL_BLENDMODE_BLEND);
            }
            prev[0] = -1;
        } else {
            triangle(prev);
            std::copy(k, k + 3, prev);
        }
    }
    if (prev[0] != -1) triangle(prev);
    return true;
}

SDL_Surface* SDL_RenderReadPixels(SDL_Renderer* r, const SDL_Rect* rect) {
    if (!Begin(r)) return nullptr;
    SDL_Rect a = rect ? *rect : SDL_Rect{0, 0, r->w, r->h};
    if (a.x < 0 || a.y < 0 || a.w <= 0 || a.h <= 0 || a.x + a.w > r->w || a.y + a.h > r->h) {
        SDL_SetError("Tried to read outside of surface bounds");
        return nullptr;
    }
    SDL_Surface* s = SDL_CreateSurface(a.w, a.h, SDL_PIXELFORMAT_XRGB8888);
    if (!s) return nullptr;
    for (int y = 0; y < a.h; ++y)
        memcpy((uint8_t*)s->pixels + (size_t)y * s->pitch, r->px + (size_t)(a.y + y) * r->stride + a.x, (size_t)a.w * 4);
    return s;
}

bool SDL_FlushRenderer(SDL_Renderer* r) { return r != nullptr; }

bool SDL_RenderPresent(SDL_Renderer* r) {
    if (!r || !r->window || !r->window->native) return false;
    r->window->native->Present(nullptr, 0);
    return true;
}

bool SDL_UpdateWindowSurfaceRects(SDL_Window* w, const SDL_Rect* rects, int n) {
    if (!w || !w->native) return false;
    w->native->Present(rects, n);
    return true;
}

// --- textures -------------------------------------------------------------------------------------------------------

SDL_Texture* SDL_CreateTexture(SDL_Renderer* r, SDL_PixelFormat format, SDL_TextureAccess, int w, int h) {
    if (!r || w <= 0 || h <= 0 || w > 16384 || h > 16384) {
        SDL_SetError("Texture dimensions are limited to 16384x16384");
        return nullptr;
    }
    if (format != SDL_PIXELFORMAT_ARGB8888 && format != SDL_PIXELFORMAT_ABGR8888 && format != SDL_PIXELFORMAT_XRGB8888) {
        SDL_SetError("Unsupported texture format");
        return nullptr;
    }
    auto* t = new SDL_Texture;
    t->renderer = r;
    t->w = w, t->h = h, t->format = format;
    t->alpha = format != SDL_PIXELFORMAT_XRGB8888;
    t->blend = t->alpha ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE;
    t->px.assign((size_t)w * h, t->alpha ? 0 : 0xff000000u);
    r->textures.insert(t);
    return t;
}

void SDL_DestroyTexture(SDL_Texture* t) {
    if (!t) return;
    if (t->renderer) t->renderer->textures.erase(t);
    delete t;
}

bool SDL_UpdateTexture(SDL_Texture* t, const SDL_Rect* rect, const void* pixels, int pitch) {
    if (!t || !pixels) return false;
    SDL_Rect a = rect ? *rect : SDL_Rect{0, 0, t->w, t->h}, c;
    if (!Intersect(a, SDL_Rect{0, 0, t->w, t->h}, c) || c.x != a.x || c.y != a.y || c.w != a.w || c.h != a.h) {
        SDL_SetError("Parameter 'rect' is invalid");
        return false;
    }
    for (int y = 0; y < a.h; ++y) {
        const uint32_t* s = (const uint32_t*)((const uint8_t*)pixels + (size_t)y * pitch);
        uint32_t* d = &t->px[(size_t)(a.y + y) * t->w + a.x];
        if (t->format == SDL_PIXELFORMAT_ABGR8888) {
            for (int x = 0; x < a.w; ++x) {
                uint32_t p = s[x];  // bytes R, G, B, A
                d[x] = (p & 0xff00ff00u) | (p & 0xff) << 16 | (p >> 16 & 0xff);
            }
        } else if (t->format == SDL_PIXELFORMAT_XRGB8888) {
            for (int x = 0; x < a.w; ++x) d[x] = s[x] | 0xff000000u;
        } else {
            memcpy(d, s, (size_t)a.w * 4);
        }
    }
    return true;
}

bool SDL_GetTextureSize(SDL_Texture* t, float* w, float* h) {
    if (!t) return false;
    if (w) *w = (float)t->w;
    if (h) *h = (float)t->h;
    return true;
}

bool SDL_SetTextureBlendMode(SDL_Texture* t, SDL_BlendMode mode) {
    if (!t) return false;
    t->blend = mode;
    return true;
}

bool SDL_SetTextureScaleMode(SDL_Texture* t, SDL_ScaleMode mode) {
    if (!t) return false;
    t->scale = mode;
    return true;
}

bool SDL_SetTextureAlphaMod(SDL_Texture* t, Uint8 alpha) {
    if (!t) return false;
    t->mod.a = alpha / 255.0f;
    return true;
}

// --- surfaces -------------------------------------------------------------------------------------------------------

SDL_Surface* SDL_CreateSurface(int w, int h, SDL_PixelFormat format) {
    if (w <= 0 || h <= 0 || w > 32768 || h > 32768) {
        SDL_SetError("Invalid surface size");
        return nullptr;
    }
    auto* s = new SDL_Surface{};
    s->format = format, s->w = w, s->h = h, s->pitch = w * 4;
    s->pixels = calloc((size_t)w * h, 4);
    s->reserved = s->pixels;  // owned
    return s;
}

SDL_Surface* SDL_CreateSurfaceFrom(int w, int h, SDL_PixelFormat format, void* pixels, int pitch) {
    if (w <= 0 || h <= 0 || !pixels) return nullptr;
    auto* s = new SDL_Surface{};
    s->format = format, s->w = w, s->h = h, s->pitch = pitch, s->pixels = pixels;
    return s;
}

void SDL_DestroySurface(SDL_Surface* s) {
    if (!s) return;
    free(s->reserved);
    delete s;
}

namespace {
uint32_t MapRGBA(SDL_PixelFormat f, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (f == SDL_PIXELFORMAT_ABGR8888) return (uint32_t)a << 24 | (uint32_t)b << 16 | (uint32_t)g << 8 | r;
    return (uint32_t)(f == SDL_PIXELFORMAT_XRGB8888 ? 0xff : a) << 24 | (uint32_t)r << 16 | (uint32_t)g << 8 | b;
}
// A surface's pixel as 0xAARRGGBB.
uint32_t ReadARGB(const SDL_Surface* s, int x, int y) {
    uint32_t p = *(const uint32_t*)((const uint8_t*)s->pixels + (size_t)y * s->pitch + x * 4);
    if (s->format == SDL_PIXELFORMAT_ABGR8888) return (p & 0xff00ff00u) | (p & 0xff) << 16 | (p >> 16 & 0xff);
    if (s->format == SDL_PIXELFORMAT_XRGB8888) return p | 0xff000000u;
    return p;
}
}  // namespace

bool SDL_ClearSurface(SDL_Surface* s, float r, float g, float b, float a) {
    if (!s) return false;
    uint32_t c = MapRGBA(s->format, ColorByte(r), ColorByte(g), ColorByte(b), ColorByte(a));
    for (int y = 0; y < s->h; ++y) std::fill_n((uint32_t*)((uint8_t*)s->pixels + (size_t)y * s->pitch), s->w, c);
    return true;
}

bool SDL_WriteSurfacePixel(SDL_Surface* s, int x, int y, Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
    if (!s || x < 0 || y < 0 || x >= s->w || y >= s->h) return false;
    *(uint32_t*)((uint8_t*)s->pixels + (size_t)y * s->pitch + x * 4) = MapRGBA(s->format, r, g, b, a);
    return true;
}

bool SDL_SaveBMP(SDL_Surface* s, const char* file) {
    if (!s || !file) return false;
    std::vector<uint32_t> px((size_t)s->w * s->h);
    for (int y = 0; y < s->h; ++y)
        for (int x = 0; x < s->w; ++x) px[(size_t)y * s->w + x] = ReadARGB(s, x, y);
    return WriteBmp(file, s->w, s->h, px.data(), s->w);
}

bool SDL_SavePNG(SDL_Surface* s, const char* file) {
    if (!s || !file) return false;
    std::vector<uint32_t> px((size_t)s->w * s->h);
    for (int y = 0; y < s->h; ++y)
        for (int x = 0; x < s->w; ++x) px[(size_t)y * s->w + x] = ReadARGB(s, x, y);
    return WritePng(file, s->w, s->h, px.data(), s->w, s->format != SDL_PIXELFORMAT_XRGB8888);
}
