// glint SIMD selection, adapted for cordinal's baseline x64 build (MIT, LICENSE.txt).
// Immutable dispatch lets independent export and streaming encoders run concurrently.
#pragma once
#include "glint.h"
namespace glint {
inline constexpr int g_simd_level = GLINT_SIMD_SSE2;
inline void init_simd(glint_simd) {}
}