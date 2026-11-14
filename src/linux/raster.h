// Glyph outlines (typeface.h) to 8-bit coverage, by exact signed-area accumulation: each pixel gets
// the fraction of it the outline covers under the nonzero rule, as FreeType's smooth rasterizer
// gives, or on/off for the pixel font. The bitmap's box is FreeType's: the outline's control box
// widened to whole pixels (rounded instead for monochrome).
#pragma once
#include "typeface.h"

#include <cstdint>
#include <vector>

namespace lx::type {

struct Coverage {
    int left = 0, top = 0;     // the top-left pixel relative to the glyph origin, y up (FreeType's bitmap_left/top)
    int width = 0, rows = 0;
    std::vector<uint8_t> pixels;  // rows of `width` bytes, 0 transparent to 255 covered
};

void Rasterize(const Outline& outline, bool mono, Coverage* out);

}  // namespace lx::type
