// Reversible RVA normalization, selected solely by unchanged instruction-prefix bytes.
#pragma once
#include "format.h"

namespace cdp {
inline void filter_code(uint8_t *p, size_t bytes, uint32_t rva, bool inverse) {
    for (size_t at = 0; at < bytes;) {
        size_t prefix = 0;
        const unsigned op = p[at];
        if (op == 0xe8 || op == 0xe9)
            prefix = 1;
        else if (bytes - at >= 2 && ((op == 0x0f && (p[at + 1] & 0xf0) == 0x80) ||
                                     (op == 0xff && (p[at + 1] == 0x15 || p[at + 1] == 0x25))))
            prefix = 2;
        else if (bytes - at >= 3 && (op & 0xf0) == 0x40 &&
                 (p[at + 1] == 0x8b || p[at + 1] == 0x89 || p[at + 1] == 0x8d) &&
                 (p[at + 2] & 0xc7) == 5)
            prefix = 3;
        if (prefix && bytes - at >= prefix + 4) {
            const uint32_t next = rva + uint32_t(at + prefix + 4);
            const uint32_t old = read32(p + at + prefix);
            write32(p + at + prefix, inverse ? old - next : old + next);
            at += prefix + 4;
        } else
            ++at;
    }
}
} // namespace cdp
