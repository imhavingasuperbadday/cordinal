// The coverage rasterizer (raster.h): contours are decomposed as FT_Outline_Decompose does,
// curves flattened to within a sixteenth of a pixel, and each line's signed area accumulated into
// a row-major buffer whose running sum is the winding coverage of every pixel.
#include "raster.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace lx::type {

namespace {

struct Accumulator {
    int w, h;
    std::vector<float> a;
    float lastX = 0, lastY = 0;

    Accumulator(int width, int height) : w(width), h(height), a((size_t)width * height + 4, 0.f) {}

    void Line(float x0, float y0, float x1, float y1) {
        if (y0 == y1) return;
        float dir = 1;
        if (y0 > y1) std::swap(x0, x1), std::swap(y0, y1), dir = -1;
        x0 = std::clamp(x0, 0.f, (float)w), x1 = std::clamp(x1, 0.f, (float)w);
        float dxdy = (x1 - x0) / (y1 - y0);
        float x = x0;
        if (y0 < 0) x -= y0 * dxdy;
        int yStart = std::max(0, (int)y0), yEnd = std::min(h, (int)std::ceil(y1));
        for (int y = yStart; y < yEnd; ++y) {
            size_t row = (size_t)y * w;
            float dy = std::min((float)(y + 1), y1) - std::max((float)y, y0);
            float xnext = x + dxdy * dy;
            float d = dy * dir;
            float lo = std::min(x, xnext), hi = std::max(x, xnext);
            lo = std::clamp(lo, 0.f, (float)w), hi = std::clamp(hi, 0.f, (float)w);
            float loFloor = std::floor(lo);
            int lo_i = (int)loFloor;
            float hiCeil = std::ceil(hi);
            int hi_i = (int)hiCeil;
            if (hi_i <= lo_i + 1) {
                float xmf = 0.5f * (x + xnext) - loFloor;
                a[row + lo_i] += d - d * xmf;
                a[row + lo_i + 1] += d * xmf;
            } else {
                float s = 1.f / (hi - lo);
                float lof = lo - loFloor;
                float a0 = 0.5f * s * (1 - lof) * (1 - lof);
                float hif = hi - hiCeil + 1;
                float am = 0.5f * s * hif * hif;
                a[row + lo_i] += d * a0;
                if (hi_i == lo_i + 2) {
                    a[row + lo_i + 1] += d * (1 - a0 - am);
                } else {
                    float a1 = s * (1.5f - lof);
                    a[row + lo_i + 1] += d * (a1 - a0);
                    for (int xi = lo_i + 2; xi < hi_i - 1; ++xi) a[row + xi] += d * s;
                    float a2 = a1 + (float)(hi_i - lo_i - 3) * s;
                    a[row + hi_i - 1] += d * (1 - a2 - am);
                }
                a[row + hi_i] += d * am;
            }
            x = xnext;
        }
    }
};

struct Flattener {
    Accumulator& acc;
    float ox, oy;  // pixel position of the bitmap's top-left corner
    float cx = 0, cy = 0;
    static constexpr float kTolerance = 1.f / 16;

    void Px(int32_t x26, int32_t y26, float* x, float* y) const { *x = x26 / 64.f - ox, *y = oy - y26 / 64.f; }
    void MoveTo(float x, float y) { cx = x, cy = y; }
    void LineTo(float x, float y) {
        acc.Line(cx, cy, x, y);
        cx = x, cy = y;
    }
    void QuadTo(float x1, float y1, float x2, float y2) {
        float dx = cx - 2 * x1 + x2, dy = cy - 2 * y1 + y2;
        float dev = std::sqrt(dx * dx + dy * dy) / 4;  // the midpoint's distance from the chord
        int n = std::clamp((int)std::ceil(std::sqrt(dev / kTolerance)), 1, 100);
        float x0 = cx, y0 = cy;
        for (int i = 1; i <= n; ++i) {
            float t = (float)i / n, u = 1 - t;
            LineTo(u * u * x0 + 2 * u * t * x1 + t * t * x2, u * u * y0 + 2 * u * t * y1 + t * t * y2);
        }
    }
    void CubicTo(float x1, float y1, float x2, float y2, float x3, float y3) {
        float ax = cx - 2 * x1 + x2, ay = cy - 2 * y1 + y2, bx = x1 - 2 * x2 + x3, by = y1 - 2 * y2 + y3;
        float d = std::sqrt(std::max(ax * ax + ay * ay, bx * bx + by * by));
        int n = std::clamp((int)std::ceil(std::sqrt(0.75f * d / kTolerance)), 1, 100);
        float x0 = cx, y0 = cy;
        for (int i = 1; i <= n; ++i) {
            float t = (float)i / n, u = 1 - t;
            float b0 = u * u * u, b1 = 3 * u * u * t, b2 = 3 * u * t * t, b3 = t * t * t;
            LineTo(b0 * x0 + b1 * x1 + b2 * x2 + b3 * x3, b0 * y0 + b1 * y1 + b2 * y2 + b3 * y3);
        }
    }
};

// FT_Outline_Decompose's walk over one contour's on, conic and cubic points.
void Contour(const Outline& o, ptrdiff_t first, ptrdiff_t last, Flattener& f) {
    auto pt = [&](ptrdiff_t i, float* x, float* y) { f.Px(o.x[i], o.y[i], x, y); };
    float sx, sy;
    ptrdiff_t k = first, limit = last;
    if (o.tag[first] == 2) return;  // a contour can't start with a cubic control
    if (o.tag[first] == 0) {
        if (o.tag[last] == 1) {
            pt(last, &sx, &sy);
            --limit;
        } else {
            float ax, ay, bx, by;
            pt(first, &ax, &ay), pt(last, &bx, &by);
            sx = (ax + bx) / 2, sy = (ay + by) / 2;
        }
        --k;  // the first point is a control point, read below
    } else {
        pt(first, &sx, &sy);
    }
    f.MoveTo(sx, sy);
    while (k < limit) {
        ++k;
        uint8_t t = o.tag[k];
        float x, y;
        pt(k, &x, &y);
        if (t == 1) {
            f.LineTo(x, y);
        } else if (t == 0) {
            float cx = x, cy = y;
            for (;;) {
                if (k >= limit) {
                    f.QuadTo(cx, cy, sx, sy);
                    return;
                }
                ++k;
                float nx, ny;
                pt(k, &nx, &ny);
                if (o.tag[k] == 1) {
                    f.QuadTo(cx, cy, nx, ny);
                    break;
                }
                if (o.tag[k] != 0) return;  // a malformed mix of controls
                f.QuadTo(cx, cy, (cx + nx) / 2, (cy + ny) / 2);
                cx = nx, cy = ny;
            }
        } else {
            if (k + 1 > limit || o.tag[k + 1] != 2) return;
            float x2, y2;
            pt(k + 1, &x2, &y2);
            k += 2;
            if (k <= limit) {
                float x3, y3;
                pt(k, &x3, &y3);
                f.CubicTo(x, y, x2, y2, x3, y3);
            } else {
                f.CubicTo(x, y, x2, y2, sx, sy);
                return;
            }
        }
    }
    f.LineTo(sx, sy);
}

int FloorPix(int32_t v) { return (int)(v >> 6); }
int CeilPix(int32_t v) { return (int)(((int64_t)v + 63) >> 6); }
int RoundPix(int32_t v) { return (int)(((int64_t)v + 32) >> 6); }

}  // namespace

void Rasterize(const Outline& o, bool mono, Coverage* out) {
    *out = {};
    int32_t xMin, yMin, xMax, yMax;
    if (o.ends.empty() || !o.Box(&xMin, &yMin, &xMax, &yMax)) return;
    int left, right, bottom, top;
    if (mono) {
        left = RoundPix(xMin), right = RoundPix(xMax), bottom = RoundPix(yMin), top = RoundPix(yMax);
        if (left == right) right = left + 1;
        if (bottom == top) top = bottom + 1;
    } else {
        left = FloorPix(xMin), right = CeilPix(xMax), bottom = FloorPix(yMin), top = CeilPix(yMax);
    }
    int w = right - left, h = top - bottom;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return;
    out->left = left, out->top = top, out->width = w, out->rows = h;
    out->pixels.assign((size_t)w * h, 0);
    Accumulator acc(w, h);
    Flattener f{acc, (float)left, (float)top};
    ptrdiff_t first = 0;
    for (uint16_t end : o.ends) {
        if (end >= o.x.size() || end < first) break;
        Contour(o, first, end, f);
        first = (ptrdiff_t)end + 1;
    }
    float sum = 0;
    for (size_t i = 0; i < (size_t)w * h; ++i) {
        sum += acc.a[i];
        float c = std::fabs(sum);
        if (mono) out->pixels[i] = c > 0.5f ? 255 : 0;
        else out->pixels[i] = (uint8_t)std::min(255, (int)(c * 256 + 0.001f));
    }
}

}  // namespace lx::type
