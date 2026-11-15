// The now playing cover's change from one picture to the next, shared by the Windows and Linux front ends
// (Preferences: Cover change, off unless chosen). The outgoing cover crunches into a badly saved JPEG: its
// colour smears into blocks, some blocks flatten or slip, tones band, grain and a darkening vignette come
// in, and it ends as a black-and-white photograph (or, in the colour style, a crunched colour one). It
// holds for a beat, then the incoming cover opens out of the centre block by block, each block arriving
// as a flat tile that sharpens into the picture. Pure pixel work on 0x00RRGGBB squares, so it runs
// headless in the tests.
#pragma once
#include <cstdint>
#include <vector>

namespace coverfade {

// The Preferences choice, as stored: 0 off, 1 black and white, 2 colour.
enum Style { kOff = 0, kMono = 1, kColour = 2 };

constexpr int kDrainMs = 620;   // colour to the crunched photograph
constexpr int kHoldMs = 240;    // the photograph alone, long enough to land
constexpr int kRevealMs = 820;  // the next cover opening out of the middle
constexpr int kTotalMs = kDrainMs + kHoldMs + kRevealMs;

// `src` (0x00RRGGBB, `w` x `h`, top-down) centre-cropped to a square and scaled to `side` x `side`,
// bilinearly. Empty when the source is.
std::vector<uint32_t> Square(const uint32_t* src, int w, int h, int side);

// The JPEG block's side for a cover `side` pixels across: about 22 blocks over the cover, at least 3 pixels.
int BlockSize(int side);

// One frame `ms` after the change: `outgoing` (side x side) treated as above, the incoming picture over
// it. `incoming` may be null, when the new state has no cover: `fill` then stands in for it. Writes
// side x side pixels to `out`, which may be `incoming` itself. `colour` keeps the photograph's colour.
// False once the change is complete, when `out` is simply the incoming picture.
bool Compose(const uint32_t* outgoing, const uint32_t* incoming, uint32_t fill, int side, int ms, uint32_t* out,
             bool colour = false);

}  // namespace coverfade
