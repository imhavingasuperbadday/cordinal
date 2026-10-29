// QR codes (ISO/IEC 18004) for the short links a phone camera opens: byte mode, error correction
// level M, the smallest of versions 1 to 40 that fits, and the mask the standard's penalty picks.
// Portable core: no drawing and no Win32.
#pragma once
#include <cstdint>
#include <string_view>
#include <vector>

namespace qr {
struct Code {
    int size = 0;                  // modules per side; 0 when the text is too long for any version
    std::vector<uint8_t> modules;  // size * size, row by row; 1 is dark
    bool Dark(int x, int y) const { return modules[(size_t)y * size + x] != 0; }
};
Code Encode(std::string_view text);
// The Reed-Solomon check bytes for `data` (GF(256), polynomial 0x11D), exposed for the self-test.
std::vector<uint8_t> CheckBytes(const std::vector<uint8_t>& data, int count);
}  // namespace qr
