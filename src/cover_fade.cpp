// The now playing cover's black-and-white change (cover_fade.h): resampling to a square, and each frame.
#include "cover_fade.h"

#include <algorithm>
#include <cmath>

namespace coverfade {

namespace {
float Clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
float Smooth(float t) { t = Clamp01(t); return t * t * (3 - 2 * t); }
float EaseInOut(float t) {  // cubic: slow off the photograph, slow into the finished cover
    t = Clamp01(t);
    return t < 0.5f ? 4 * t * t * t : 1 - (-2 * t + 2) * (-2 * t + 2) * (-2 * t + 2) / 2;
}
// A fixed value per cell, 0..1. Fixed, so grain and blocks hold still like a saved file instead of
// crawling like television static.
float Hash(int x, int y, uint32_t salt) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + salt * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (float)(h & 0xFFFF) / 65535.0f;
}
float Luma(float r, float g, float b) { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }

// Each block's mean colour, as floats r, g, b per block.
std::vector<float> BlockMeans(const uint32_t* px, int side, int block, int blocks) {
    std::vector<float> sum((size_t)blocks * blocks * 4, 0);
    for (int y = 0; y < side; ++y)
        for (int x = 0; x < side; ++x) {
            const uint32_t p = px[(size_t)y * side + x];
            float* s = &sum[((size_t)(y / block) * blocks + x / block) * 4];
            s[0] += (float)((p >> 16) & 255); s[1] += (float)((p >> 8) & 255); s[2] += (float)(p & 255); s[3] += 1;
        }
    std::vector<float> mean((size_t)blocks * blocks * 3);
    for (size_t i = 0; i < (size_t)blocks * blocks; ++i)
        for (int c = 0; c < 3; ++c) mean[i * 3 + c] = sum[i * 4 + 3] > 0 ? sum[i * 4 + c] / sum[i * 4 + 3] : 0;
    return mean;
}
}  // namespace

std::vector<uint32_t> Square(const uint32_t* src, int w, int h, int side) {
    std::vector<uint32_t> out;
    if (!src || w <= 0 || h <= 0 || side <= 0) return out;
    out.resize((size_t)side * side);
    const int crop = std::min(w, h);
    const float x0 = (w - crop) / 2.0f, y0 = (h - crop) / 2.0f, step = (float)crop / side;
    for (int y = 0; y < side; ++y) {
        float sy = std::clamp(y0 + (y + 0.5f) * step - 0.5f, 0.0f, (float)h - 1);
        int iy = (int)sy, iy1 = std::min(iy + 1, h - 1);
        float fy = sy - iy;
        for (int x = 0; x < side; ++x) {
            float sx = std::clamp(x0 + (x + 0.5f) * step - 0.5f, 0.0f, (float)w - 1);
            int ix = (int)sx, ix1 = std::min(ix + 1, w - 1);
            float fx = sx - ix;
            uint32_t a = src[(size_t)iy * w + ix], b = src[(size_t)iy * w + ix1];
            uint32_t c = src[(size_t)iy1 * w + ix], d = src[(size_t)iy1 * w + ix1];
            uint32_t px = 0;
            for (int shift = 0; shift < 24; shift += 8) {
                float top = ((a >> shift) & 255) * (1 - fx) + ((b >> shift) & 255) * fx;
                float bottom = ((c >> shift) & 255) * (1 - fx) + ((d >> shift) & 255) * fx;
                px |= (uint32_t)std::lround(top * (1 - fy) + bottom * fy) << shift;
            }
            out[(size_t)y * side + x] = px;
        }
    }
    return out;
}

int BlockSize(int side) { return std::max(3, side / 22); }

bool Compose(const uint32_t* outgoing, const uint32_t* incoming, uint32_t fill, int side, int ms, uint32_t* out,
             bool colour) {
    if (side <= 0 || !out) return false;
    const size_t n = (size_t)side * side;
    if (!outgoing || ms >= kTotalMs) {
        if (incoming) { if (incoming != out) std::copy(incoming, incoming + n, out); }
        else std::fill(out, out + n, fill & 0xFFFFFF);
        return false;
    }
    ms = std::max(ms, 0);
    const float drain = Smooth((float)ms / kDrainMs);
    const float reveal = EaseInOut((float)(ms - kDrainMs - kHoldMs) / kRevealMs);
    // Everything happens in blocks, as a badly saved JPEG does: colour smears into each block first (the
    // chroma a JPEG throws away), some blocks flatten to one tone or slip down from the block above, and
    // the tones band into a few grey levels. Means are taken before `out` is written, which may be
    // `incoming` itself.
    const int block = BlockSize(side), blocks = (side + block - 1) / block;
    const std::vector<float> fromMean = BlockMeans(outgoing, side, block, blocks);
    const std::vector<float> toMean = incoming && reveal > 0 ? BlockMeans(incoming, side, block, blocks) : std::vector<float>{};
    const float levels = 6 + (1 - drain) * (1 - drain) * 250;  // 256 grey levels down to 6
    // The iris opens block by block with a ragged edge; blocks at the edge come in as flat tiles, then
    // sharpen into the picture. It starts as a point and ends past every corner, so the last frame is the
    // new cover whole.
    constexpr float kSoft = 0.3f, kRagged = 0.18f;
    const float radius = reveal * (1 + kSoft + kRagged);
    const float half = side / 2.0f, toCorner = 1 / (half * 1.41421356f);
    for (int y = 0; y < side; ++y) {
        const int by = y / block;
        const float dy = (y + 0.5f - half) * toCorner;
        for (int x = 0; x < side; ++x) {
            const int bx = x / block;
            const size_t i = (size_t)y * side + x;
            const float dx = (x + 0.5f - half) * toCorner;
            const float d = std::sqrt(dx * dx + dy * dy);  // 0 at the centre, 1 in the corners
            const float k = drain, cell = Hash(bx, by, 1);
            // Some blocks slip: they show the block above them, a glitch that only grows as it drains.
            const bool slipped = by > 0 && Hash(bx, by, 2) < 0.07f * k;
            const uint32_t o = slipped ? outgoing[i - (size_t)block * side] : outgoing[i];
            const float* m = &fromMean[((size_t)(slipped ? by - 1 : by) * blocks + bx) * 3];
            const float r = (float)((o >> 16) & 255), g = (float)((o >> 8) & 255), b = (float)(o & 255);
            // Chroma per block: the block's colour with this pixel's own brightness.
            const float shift = Luma(r, g, b) - Luma(m[0], m[1], m[2]), chroma = std::min(1.0f, k * 2.2f);
            float cr = r + (m[0] + shift - r) * chroma, cg = g + (m[1] + shift - g) * chroma,
                  cb = b + (m[2] + shift - b) * chroma;
            // Flattened blocks: about a third go to one tone, the rest only soften.
            const float flat = k * (cell > 0.66f ? 1.0f : 0.3f * cell);
            cr += (m[0] - cr) * flat; cg += (m[1] - cg) * flat; cb += (m[2] - cb) * flat;
            // The photograph: more contrast, blacks lifted, grain, a tone shift per block, banding,
            // and the edges darkening toward the frame.
            const float noise = (Hash(x, y, 3) * 2 - 1) * 7 + (Hash(bx, by, 4) * 2 - 1) * 12 * k;
            auto print = [&](float v) {
                v = std::clamp((v - 128) * 1.3f + 128, 0.0f, 255.0f) * 0.9f + 16 + noise;
                v = std::round(std::clamp(v, 0.0f, 255.0f) / 255 * (levels - 1)) / (levels - 1) * 255;
                return v * (1 - 0.5f * d * d);
            };
            float pr, pg, pb;
            if (colour) {  // the same crunch, each channel its own, so the colour stays
                pr = r + (print(cr) - r) * k; pg = g + (print(cg) - g) * k; pb = b + (print(cb) - b) * k;
            } else {
                const float lum = print(Luma(cr, cg, cb));
                pr = r + (lum - r) * k; pg = g + (lum - g) * k; pb = b + (lum - b) * k;
            }
            if (!colour && k > 0 && k < 1) {  // on the way there, the blocky colour shows through
                const float mid = 4 * k * (1 - k);
                pr += (cr - r) * mid * (1 - k); pg += (cg - g) * mid * (1 - k); pb += (cb - b) * mid * (1 - k);
            }
            const float vignette = 1 - k * 0.12f * d;  // the colour darkens a little on its way out too
            pr *= vignette; pg *= vignette; pb *= vignette;
            float nr = pr, ng = pg, nb = pb;
            if (reveal > 0) {
                // Each block's distance from the centre, pushed out or in a little, so the edge is ragged.
                const float cx = ((bx + 0.5f) * block - half) * toCorner, cy = ((by + 0.5f) * block - half) * toCorner;
                const float db = std::sqrt(cx * cx + cy * cy) + (Hash(bx, by, 5) - 0.5f) * 2 * kRagged;
                const float a = 1 - Smooth((db - (radius - kSoft)) / kSoft);
                float ir, ig, ib;
                if (incoming) {
                    const uint32_t p = incoming[i];
                    const float* t = &toMean[((size_t)by * blocks + bx) * 3];
                    const float tile = 1 - a;  // still a flat tile at the edge, the picture once inside
                    ir = ((p >> 16) & 255) + (t[0] - ((p >> 16) & 255)) * tile;
                    ig = ((p >> 8) & 255) + (t[1] - ((p >> 8) & 255)) * tile;
                    ib = (p & 255) + (t[2] - (p & 255)) * tile;
                } else {
                    ir = (float)((fill >> 16) & 255); ig = (float)((fill >> 8) & 255); ib = (float)(fill & 255);
                }
                // Blocks lighting up as the edge passes them, gone by the end.
                const float rim = (a > 0 && a < 1 ? 4 * a * (1 - a) : 0) * 40 * (1 - reveal);
                nr = pr + (ir - pr) * a + rim;
                ng = pg + (ig - pg) * a + rim;
                nb = pb + (ib - pb) * a + rim;
            }
            auto byte = [](float v) { return (uint32_t)std::clamp((int)std::lround(v), 0, 255); };
            out[i] = byte(nr) << 16 | byte(ng) << 8 | byte(nb);
        }
    }
    return true;
}

}  // namespace coverfade
