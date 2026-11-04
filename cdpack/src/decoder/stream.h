// Whole-buffer decoding of the packed stream (spec.md); the caller owns all memory.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../format.h"

#if defined(__GNUC__)
#define CDP_INLINE inline __attribute__((always_inline))
#else
#define CDP_INLINE __forceinline
#endif

namespace cdp {
enum class DecodeStatus : uint32_t { ok, input, output, distance, framing, workspace };

// Keep both children ready before the parent decision. Each pair fits its model array.
CDP_INLINE unsigned load_pair(const uint16_t *model) {
    unsigned value = read32(reinterpret_cast<const uint8_t *>(model));
#if defined(__GNUC__) && defined(__x86_64__)
    __asm__ volatile("" : "+r"(value));
#endif
    return value;
}

// The padding belongs to this layout. No speculative tree load crosses its own array.
struct LengthModel {
    uint16_t choose[2];
    uint16_t small[4][8];
    uint16_t medium[4][8];
    uint16_t large[256];
};
struct Models {
    uint16_t kind[12][4];
    uint16_t repeated[12], recent[12], second[12], third[12];
    uint16_t single[12][4];
    LengthModel length, repeat_length;
    uint16_t slot[4][64];
    uint16_t short_distance[128];
    uint16_t alignment[16];
    uint16_t literal[8][768];
};

struct RangeInput {
    const uint8_t *at;
    const uint8_t *end;
    uint32_t width, value;
    // Valid normalized widths and probabilities cannot produce zero. Missing input makes
    // zero an absorbing state, so no extra failure flag has to cross every tree iteration.
    CDP_INLINE bool exhausted() const { return !width; }

    CDP_INLINE void refill() {
        if (width < (1u << 24)) {
            width <<= 8;
            if (at == end)
                width = 0;
            else
                value = (value << 8) | *at++;
        }
    }
    CDP_INLINE unsigned decision(uint16_t &model, unsigned estimate) {
        refill();
        const uint32_t split = (width >> 11) * estimate;
#if defined(__GNUC__) && defined(__x86_64__)
        uint32_t delta = value, remaining = width - split;
        // Both probability updates are integer averages, with opposite rounding at 1/32.
        // Computing them before the subtraction lets its carry select all three results.
        uint32_t updated = estimate * 31 + 2048;
        const uint32_t lower = estimate * 31 + 31;
        bool one;
        __asm__("subl %[split],%[delta]\n\t"
                "cmovael %[delta],%[code]\n\t"
                "cmovbl %[split],%[remaining]\n\t"
                "cmovael %[lower],%[updated]"
                : [code] "+&r"(value), [delta] "+&r"(delta), [remaining] "+&r"(remaining),
                  [one] "=@ccae"(one), [updated] "+r"(updated)
                : [split] "r"(split), [lower] "r"(lower)
                : "cc");
        width = remaining;
        model = uint16_t(updated >> 5);
        return one;
#else
        if (value < split) {
            width = split;
            model = uint16_t(estimate + ((2048 - estimate) >> 5));
            return 0;
        }
        value -= split;
        width -= split;
        model = uint16_t(estimate - (estimate >> 5));
        return 1;
#endif
    }
    CDP_INLINE unsigned bit(uint16_t &model) { return decision(model, model); }

    // Load the following pair before the current decision's multiply/compare completes.
    template <unsigned Bits>
    CDP_INLINE unsigned tree(uint16_t *model) {
        unsigned node = 1, estimate = model[1];
#if defined(__clang__)
#pragma clang loop unroll(disable)
#endif
        for (unsigned depth = 1; depth < Bits; ++depth) {
            const unsigned pair = load_pair(model + node * 2);
            const unsigned one = decision(model[node], estimate);
            node = node * 2 + one;
            estimate = one ? pair >> 16 : pair & 65535;
        }
        return node * 2 + decision(model[node], estimate) - (1u << Bits);
    }
    CDP_INLINE unsigned reverse(uint16_t *model, unsigned bits) {
        unsigned node = 1, result = 0;
        for (unsigned depth = 0; depth < bits; ++depth) {
            const unsigned one = bit(model[node]);
            node = node * 2 + one;
            result |= one << depth;
        }
        return result;
    }
    CDP_INLINE unsigned length(LengthModel &model, unsigned position) {
        if (!bit(model.choose[0]))
            return tree<3>(model.small[position]);
        if (!bit(model.choose[1]))
            return 8 + tree<3>(model.medium[position]);
        return 16 + tree<8>(model.large);
    }
    CDP_INLINE uint8_t literal(uint16_t *model, unsigned predicted, bool matched) {
        if (!matched)
            return uint8_t(tree<8>(model));
        unsigned node = 1;
        unsigned estimate = model[257 + ((predicted >> 7) & 1) * 256];
        bool mismatch = false;
        while (node < 128) {
            const unsigned expected = (predicted >> 7) & 1;
            const unsigned context = 256 + expected * 256;
            const unsigned next_context = 256 + ((predicted >> 6) & 1) * 256;
            const unsigned pair = load_pair(model + next_context + node * 2);
            const unsigned one = decision(model[context + node], estimate);
            node = node * 2 + one;
            predicted <<= 1;
            if (one != expected) {
                mismatch = true;
                break;
            }
            estimate = one ? pair >> 16 : pair & 65535;
        }
        if (!mismatch)
            return uint8_t(node * 2 +
                           decision(model[256 + ((predicted >> 7) & 1) * 256 + node], estimate));
        estimate = model[node];
        while (node < 128) {
            const unsigned pair = load_pair(model + node * 2);
            const unsigned one = decision(model[node], estimate);
            node = node * 2 + one;
            estimate = one ? pair >> 16 : pair & 65535;
        }
        return uint8_t(node * 2 + decision(model[node], estimate));
    }
};

CDP_INLINE DecodeStatus decode_body(RangeInput &range, uint8_t *output, size_t output_bytes,
                                    Models *models, unsigned (&history)[4], unsigned &phase,
                                    size_t &produced) {
    for (;;) {
        if (range.exhausted())
            return DecodeStatus::input;
        const unsigned position = unsigned(produced) & 3;
        if (!range.bit(models->kind[phase][position])) {
            if (produced == output_bytes)
                return DecodeStatus::output;
            const unsigned previous = produced ? output[produced - 1] : 0;
            if (phase >= 7 && history[0] > produced)
                return DecodeStatus::distance;
            const unsigned predicted = phase >= 7 ? output[produced - history[0]] : 0;
            output[produced++] =
                range.literal(models->literal[previous >> 5], predicted, phase >= 7);
            phase = phase < 4 ? 0 : phase < 10 ? phase - 3 : phase - 6;
            continue;
        }
        unsigned count = 0;
        if (range.bit(models->repeated[phase])) {
            if (!produced)
                return DecodeStatus::distance;
            if (!range.bit(models->recent[phase])) {
                if (!range.bit(models->single[phase][position])) {
                    phase = phase < 7 ? 9 : 11;
                    count = 1;
                }
            } else {
                unsigned index = 1;
                if (range.bit(models->second[phase]))
                    index = 2 + range.bit(models->third[phase]);
                const unsigned distance = history[index];
                for (unsigned i = index; i; --i)
                    history[i] = history[i - 1];
                history[0] = distance;
            }
            if (!count) {
                count = range.length(models->repeat_length, position) + 2;
                phase = phase < 7 ? 8 : 11;
            }
        } else {
            const unsigned length_code = range.length(models->length, position);
            count = length_code + 2;
            const unsigned slot = range.tree<6>(models->slot[length_code < 3 ? length_code : 3]);
            uint32_t distance = slot;
            if (slot >= 4) {
                const unsigned bits = (slot >> 1) - 1;
                distance = (2u + (slot & 1)) << bits;
                if (slot < 14) {
                    // A private padded layout: this offset is defined by the format's slot tree.
                    distance += range.reverse(models->short_distance + distance - slot, bits);
                } else {
                    uint32_t direct = 0;
                    for (unsigned i = 4; i < bits; ++i) {
                        range.refill();
                        range.width >>= 1;
                        const unsigned one = range.value >= range.width;
                        range.value -= range.width & (0u - one);
                        direct = direct * 2 + one;
                    }
                    distance += (direct << 4) + range.reverse(models->alignment, 4);
                }
            }
            if (distance == UINT32_MAX) {
                range.refill();
                if (range.exhausted())
                    return DecodeStatus::input;
                return produced == output_bytes && !range.value && range.at == range.end
                           ? DecodeStatus::ok
                           : DecodeStatus::framing;
            }
            history[3] = history[2];
            history[2] = history[1];
            history[1] = history[0];
            history[0] = distance + 1;
            phase = phase < 7 ? 7 : 10;
        }
        if (range.exhausted())
            return DecodeStatus::input;
        if (history[0] > produced)
            return DecodeStatus::distance;
        if (count > output_bytes - produced)
            return DecodeStatus::output;
        const size_t end = produced + count;
        // A word never reads uninitialized output and never crosses the declared end.
        if (history[0] >= 8) {
            while (end - produced >= 8) {
                write64(output + produced, read64(output + produced - history[0]));
                produced += 8;
            }
        } else if (history[0] == 1 && count >= 8) {
            const uint64_t repeated = uint64_t(output[produced - 1]) * UINT64_C(0x0101010101010101);
            do {
                write64(output + produced, repeated);
                produced += 8;
            } while (end - produced >= 8);
        }
        // Copy in forward order: an LZ match can repeatedly consume bytes just written.
        while (produced < end) {
            output[produced] = output[produced - history[0]];
            ++produced;
        }
    }
}

CDP_INLINE DecodeStatus decode_stream(const uint8_t *input, size_t input_bytes, uint8_t *output,
                                    size_t output_bytes, Models *models, size_t workspace_bytes) {
    if (!models || workspace_bytes < sizeof(Models) || (uintptr_t(models) & 1))
        return DecodeStatus::workspace;
    if (input_bytes < 5)
        return DecodeStatus::input;
    if (!input || (!output && output_bytes))
        return DecodeStatus::framing;
    if (input[0])
        return DecodeStatus::framing;
    RangeInput range{input + 5, input + input_bytes, UINT32_MAX,
                     uint32_t(input[1]) << 24 | uint32_t(input[2]) << 16 | uint32_t(input[3]) << 8 |
                         input[4]};
    if (range.value == UINT32_MAX)
        return DecodeStatus::framing;
    // CDP1 targets little-endian x64. Initialize object bytes rather than indexing across
    // distinct member arrays through one uint16_t pointer.
    auto *probabilities = reinterpret_cast<uint8_t *>(models);
    for (size_t i = 0; i < sizeof(Models); i += 2) {
        probabilities[i] = 0;
        probabilities[i + 1] = 4;
    }
    unsigned history[4] = {1, 1, 1, 1};
    unsigned phase = 0;
    size_t produced = 0;
    return decode_body(range, output, output_bytes, models, history, phase, produced);
}
} // namespace cdp
