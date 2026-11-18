// cordinal's own number parsing (stage 5b of replacing glibc): strtof, strtod, strtold, the strtol
// family, their wide forms (wcstol and the rest), and the __isoc23_ names glibc 2.38's headers route
// strtol and its wide forms to. Hidden like kernel.cpp: cordinal, the static libstdc++ and cordinal's
// own scanf use these, and glibc keeps its own.
//
// The decimal conversion is musl's floatscan (src/internal/floatscan.c, MIT licensed, read November
// 17, 2026): the digits collect into base-10^9 limbs (musl's decfloat), scale until exactly 64 bits
// sit left of the radix point, and are rounded from there. musl assembles the mantissa with long
// double arithmetic (fmodl, scalbnl, copysignl, fabsl); those are libm, which stage 7 replaces, so
// the same 64-bit significand is rounded here with integer arithmetic and the value is built from
// its IEEE or x87 fields directly. That is exact and needs no library at all.
//
// Hex floats are parsed here rather than ported from musl's hexfloat: every power of two is exact,
// so the digits are the significand's bits, and only the final rounding is shared. glibc's grammar
// and answers are what the libc check compares against: a hex number needs at least one hex digit
// ("0x" and "0x.p1" are just the zero, ending after it), an exponent without digits is left
// unconsumed ("0x1p" is 1.0 ending at the p), "inf" takes an optional "inity" ("infin" ends after
// "inf"), a nan payload needs its closing bracket, nothing convertible leaves the end pointer at the
// start (not after a sign or the whitespace), and errno is ERANGE when the value overflows or a
// subnormal result lost bits ("5e-324" sets it, the exact "0x1p-1074" does not).
#include <errno.h>
#include <float.h>
#include <limits.h>
#include <stdint.h>

namespace {

// The C locale's whitespace, which is what the leading skip uses (glibc asks isspace in the current
// locale; cordinal's locale layer serves the C locale's classes).
constexpr bool IsSpace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

// A digit's value up to base 36, or -1.
constexpr int DigitValue(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return -1;
}
constexpr bool IsHexDigit(int c) { return DigitValue(c) >= 0 && DigitValue(c) < 16; }

// ---- floats -----------------------------------------------------------------------------------

constexpr int kLimbs = 2048;     // musl's KMAX: enough decimal digits for any denormal
constexpr int kLimbDigits = 9;   // a limb holds nine digits (it is 10^9 wide)
constexpr int kMantLimbs = 3;    // 27 digits, more than the 64 significand bits need
constexpr uint32_t kLimbMax[kMantLimbs] = {18u, 446744073u, 709551615u};  // 2^64-1 in base 10^9
constexpr uint32_t kP10[] = {10u, 100u, 1000u, 10000u, 100000u, 1000000u, 10000000u, 100000000u};

// A target format's fields, and how a value is built from them.
struct FloatFormat {
    static constexpr int kBits = 24, kBias = 127, kMinNormal = -126, kMax = 127;
    static constexpr bool kExplicitBit = false;
    static constexpr int kMaxPow10 = 10;  // 10^10 is the last power a float holds exactly
    using Type = float;
    static Type Make(bool negative, unsigned exponent, uint64_t mantissa) {
        const uint32_t bits = (negative ? 0x80000000u : 0u) | (exponent << 23) | (uint32_t)(mantissa & 0x7FFFFF);
        Type value;
        __builtin_memcpy(&value, &bits, sizeof value);
        return value;
    }
    static Type Infinity(bool negative) { return Make(negative, 0xFF, 0); }
    static Type QuietNaN(bool negative) { return Make(negative, 0xFF, 1u << 22); }
    static Type Pow10(int exponent) {
        static const Type kTable[kMaxPow10 + 1] = {1e0f, 1e1f, 1e2f, 1e3f, 1e4f, 1e5f,
                                                  1e6f, 1e7f, 1e8f, 1e9f, 1e10f};
        return kTable[exponent];
    }
};
struct DoubleFormat {
    static constexpr int kBits = 53, kBias = 1023, kMinNormal = -1022, kMax = 1023;
    static constexpr bool kExplicitBit = false;
    static constexpr int kMaxPow10 = 22;
    using Type = double;
    static Type Make(bool negative, unsigned exponent, uint64_t mantissa) {
        const uint64_t bits = (negative ? 0x8000000000000000ull : 0ull) | ((uint64_t)exponent << 52) |
                              (mantissa & 0xFFFFFFFFFFFFFull);
        Type value;
        __builtin_memcpy(&value, &bits, sizeof value);
        return value;
    }
    static Type Infinity(bool negative) { return Make(negative, 0x7FF, 0); }
    static Type QuietNaN(bool negative) { return Make(negative, 0x7FF, 1ull << 51); }
    static Type Pow10(int exponent) {
        static const Type kTable[kMaxPow10 + 1] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,
                                                  1e8,  1e9,  1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
                                                  1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
        return kTable[exponent];
    }
};
// x86-64's long double: a 64-bit mantissa whose top bit is written out, a 15-bit exponent and a
// sign. Building the fields is exact; the six padding bytes are cleared.
struct LongDoubleFormat {
    static constexpr int kBits = 64, kBias = 16383, kMinNormal = -16382, kMax = 16383;
    static constexpr bool kExplicitBit = true;
    static constexpr int kMaxPow10 = 27;
    using Type = long double;
    static Type Make(bool negative, unsigned exponent, uint64_t mantissa) {
        union {
            Type value;
            struct {
                uint64_t mantissa;
                uint16_t exponent;
                uint16_t padding[3];
            } parts;
        } u;
        u.parts.mantissa = mantissa;
        u.parts.exponent = (uint16_t)((negative ? 0x8000u : 0u) | exponent);
        u.parts.padding[0] = u.parts.padding[1] = u.parts.padding[2] = 0;
        return u.value;
    }
    static Type Infinity(bool negative) { return Make(negative, 0x7FFF, 0x8000000000000000ull); }
    static Type QuietNaN(bool negative) { return Make(negative, 0x7FFF, 0xC000000000000000ull); }
    static Type Pow10(int exponent) {
        static const Type kTable[kMaxPow10 + 1] = {
            1e0L,  1e1L,  1e2L,  1e3L,  1e4L,  1e5L,  1e6L,  1e7L,  1e8L,  1e9L,  1e10L, 1e11L, 1e12L, 1e13L,
            1e14L, 1e15L, 1e16L, 1e17L, 1e18L, 1e19L, 1e20L, 1e21L, 1e22L, 1e23L, 1e24L, 1e25L, 1e26L, 1e27L};
        return kTable[exponent];
    }
};

// The common case in one operation, which is correctly rounded: a significand that fits in the
// format's own 24, 53 or 64 bits times or divided by a power of ten it holds exactly (Clinger's
// shortcut, which strtod implementations all use). Anything else goes through the limbs.
template <typename Format>
bool QuickValue(bool negative, uint64_t digits, int64_t exponent10, typename Format::Type& out) {
    const int64_t magnitude = exponent10 < 0 ? -exponent10 : exponent10;
    if (magnitude > Format::kMaxPow10) return false;
    if (digits >> (Format::kBits - 1)) return false;  // more digits than the format holds exactly
    using Type = typename Format::Type;
    const Type power = Format::Pow10((int)magnitude);
    const Type value = exponent10 < 0 ? (Type)digits / power : (Type)digits * power;
    out = negative ? -value : value;
    return true;
}

// The bits below the significand's own 64: `half` is the next bit down, `rest` anything under it.
struct Tail {
    bool half = false;
    bool rest = false;
    bool Any() const { return half || rest; }
};

// A significand of exactly 64 bits (its top bit set) whose value is mantissa * 2^exponent, rounded
// to the target format. `tail` describes what lies below the mantissa's lowest bit.
template <typename Format>
typename Format::Type Finish(bool negative, uint64_t mantissa, int64_t exponent, Tail tail) {
    using Type = typename Format::Type;
    constexpr int kBits = Format::kBits;
    int64_t binaryExponent = exponent + 63;  // the value is in [2^e, 2^(e+1))
    auto Zero = [&] { return Type(Format::Make(negative, 0, 0)); };
    auto Fields = [&](uint64_t kept) {
        return Format::kExplicitBit ? kept : (kept & ((1ULL << (kBits - 1)) - 1));
    };

    if (binaryExponent > (int64_t)Format::kMax + 1) {
        errno = ERANGE;
        return Format::Infinity(negative);
    }
    if (binaryExponent < (int64_t)Format::kMinNormal - (kBits + 1)) {
        errno = ERANGE;
        return Zero();  // below half the smallest subnormal
    }

    // How many of the 64 bits fall below the result's last place: a normal result keeps kBits, a
    // subnormal one only as many as its exponent leaves.
    const int drop = binaryExponent >= Format::kMinNormal
                         ? 64 - kBits
                         : 64 - kBits + (int)(Format::kMinNormal - binaryExponent);
    uint64_t kept;
    Tail lost;
    if (drop == 0) {
        kept = mantissa;
        lost = tail;
    } else if (drop < 64) {
        kept = mantissa >> drop;
        const uint64_t remainder = mantissa & ((1ULL << drop) - 1);
        lost.half = (remainder >> (drop - 1)) & 1;
        lost.rest = (remainder & ((1ULL << (drop - 1)) - 1)) != 0 || tail.Any();
    } else if (drop == 64) {
        kept = 0;
        lost.half = true;  // the top bit of a 64-bit significand is always set
        lost.rest = (mantissa & 0x7FFFFFFFFFFFFFFFull) != 0 || tail.Any();
    } else {
        kept = 0;
        lost.rest = mantissa != 0 || tail.Any();
    }
    if (lost.half && (lost.rest || (kept & 1))) ++kept;  // nearest, ties to even

    if (binaryExponent >= Format::kMinNormal) {
        if constexpr (kBits < 64) {
            if (kept >> kBits) {  // the rounding carried into a new leading bit
                kept >>= 1;
                ++binaryExponent;
            }
        }
        if (binaryExponent > Format::kMax) {
            errno = ERANGE;
            return Format::Infinity(negative);
        }
        return Format::Make(negative, (unsigned)(binaryExponent + Format::kBias), Fields(kept));
    }
    // A subnormal: the bits are the fraction field with an exponent of zero, and glibc reports
    // ERANGE when precision was lost on the way. Rounding up to the smallest normal is not one.
    if (kept == (1ULL << (kBits - 1))) return Format::Make(negative, 1, Fields(1ULL << (kBits - 1)));
    if (lost.Any()) errno = ERANGE;
    return Format::Make(negative, 0, kept);
}

// The decimal digits of a number, from musl's decfloat: base-10^9 limbs scaled until exactly 64
// bits sit left of the radix point, then handed to Finish. `s`..`end` is the grammar's own span
// (digits, at most one point and an optional exponent), so this pass cannot disagree with the scan.
template <typename Format, typename Ch>
typename Format::Type DecimalValue(const Ch* s, const Ch* end, bool negative) {
    using Type = typename Format::Type;
    uint32_t x[kLimbs];
    int64_t lrp = 0, dc = 0, e10 = 0;
    bool gotrad = false;
    int last = 0, k = 0;
    // The digits again as one 64-bit number, while they still fit, for QuickValue below.
    uint64_t smallDigits = 0;
    int smallCount = 0;
    bool smallFits = true;

    const Ch* q = s;
    while (q < end && *q == '0') ++q;
    if (q < end && *q == '.') {
        gotrad = true;
        ++q;
        while (q < end && *q == '0') {
            --lrp;
            ++q;
        }
    }
    x[0] = 0;
    for (; q < end && (*q == '.' || (*q >= '0' && *q <= '9')); ++q) {
        if (*q == '.') {
            gotrad = true;
            lrp = dc;
            continue;
        }
        const uint32_t digit = (uint32_t)(*q - '0');
        if (smallFits) {
            if (smallCount < 19) {
                smallDigits = smallDigits * 10 + digit;
                ++smallCount;
            } else {
                smallFits = false;
            }
        }
        if (k < kLimbs - 3) {
            ++dc;
            if (last) x[k] = x[k] * 10 + digit;
            else x[k] = digit;
            if (++last == kLimbDigits) {
                ++k;
                last = 0;
            }
        } else {
            ++dc;
            if (digit) {
                x[kLimbs - 4] |= 1;  // past the precision kept: only "some digits here" matters
            }
        }
    }
    if (!gotrad) lrp = dc;

    if (q < end && (*q | 32) == 'e') {
        ++q;
        bool negativeExponent = false;
        if (q < end && (*q == '+' || *q == '-')) {
            negativeExponent = *q == '-';
            ++q;
        }
        int64_t value = 0;
        for (; q < end && *q >= '0' && *q <= '9'; ++q) {
            if (value < 1000000000000000000LL) value = value * 10 + (*q - '0');  // saturates
        }
        e10 = negativeExponent ? -value : value;
        lrp += e10;
    }

    if (!x[0]) return Type(Format::Make(negative, 0, 0));  // every digit was zero

    // The everyday case first: a significand the format holds exactly, scaled by a power of ten it
    // holds exactly, is one correctly rounded operation.
    if (smallFits) {
        Type quick;
        if (QuickValue<Format>(negative, smallDigits, lrp - dc, quick)) return quick;
    }

    // The exponent's size, before any scaling: overflow and underflow answer at once, which also
    // keeps the loops below bounded.
    if (lrp > -Format::kMinNormal / 2) {
        errno = ERANGE;
        return Format::Infinity(negative);
    }
    if (lrp < (int64_t)Format::kMinNormal - 2 * 64) {
        errno = ERANGE;
        return Type(Format::Make(negative, 0, 0));
    }

    if (last) {  // pad the last partial limb to a full nine digits
        for (; last < kLimbDigits; ++last) x[k] *= 10;
        ++k;
        last = 0;
    }

    int a = 0, z = k, e2 = 0, rp = (int)lrp;
    constexpr int mask = kLimbs - 1;

    for (; !x[z - 1]; --z) {
    }

    // Align the radix point to a limb boundary.
    if (rp % kLimbDigits) {
        const int rpm9 = rp >= 0 ? rp % kLimbDigits : rp % kLimbDigits + kLimbDigits;
        const uint32_t p10 = kP10[8 - rpm9];
        uint32_t carry = 0;
        for (k = a; k != z; ++k) {
            const uint32_t tmp = x[k] % p10;
            x[k] = x[k] / p10 + carry;
            carry = 1000000000u / p10 * tmp;
            if (k == a && !x[k]) {
                a = (a + 1) & mask;
                rp -= kLimbDigits;
            }
        }
        if (carry) x[z++] = carry;
        rp += kLimbDigits - rpm9;
    }

    // Upscale until the wanted bits are left of the radix point.
    while (rp < kLimbDigits * kMantLimbs || (rp == kLimbDigits * kMantLimbs && x[a] < kLimbMax[0])) {
        uint32_t carry = 0;
        e2 -= 29;
        for (k = (z - 1) & mask;; k = (k - 1) & mask) {
            const uint64_t tmp = ((uint64_t)x[k] << 29) + carry;
            if (tmp > 1000000000u) {
                carry = (uint32_t)(tmp / 1000000000u);
                x[k] = (uint32_t)(tmp % 1000000000u);
            } else {
                carry = 0;
                x[k] = (uint32_t)tmp;
            }
            if (k == ((z - 1) & mask) && k != a && !x[k]) z = k;
            if (k == a) break;
        }
        if (carry) {
            rp += kLimbDigits;
            a = (a - 1) & mask;
            if (a == z) {
                z = (z - 1) & mask;
                x[(z - 1) & mask] |= x[z];
            }
            x[a] = carry;
        }
    }

    // Downscale until exactly 64 bits are left of the radix point.
    for (;;) {
        uint32_t carry = 0;
        int sh = 1;
        int i = 0;
        for (; i < kMantLimbs; ++i) {
            k = (a + i) & mask;
            if (k == z || x[k] < kLimbMax[i]) {
                i = kMantLimbs;
                break;
            }
            if (x[(a + i) & mask] > kLimbMax[i]) break;
        }
        if (i == kMantLimbs && rp == kLimbDigits * kMantLimbs) break;
        if (rp > kLimbDigits + kLimbDigits * kMantLimbs) sh = kLimbDigits;
        e2 += sh;
        for (k = a; k != z; k = (k + 1) & mask) {
            const uint32_t tmp = x[k] & ((1u << sh) - 1);
            x[k] = (x[k] >> sh) + carry;
            carry = (1000000000u >> sh) * tmp;
            if (k == a && !x[k]) {
                a = (a + 1) & mask;
                --i;
                rp -= kLimbDigits;
            }
        }
        if (carry) {
            if (((z + 1) & mask) != a) {
                x[z] = carry;
                z = (z + 1) & mask;
            } else {
                x[(z - 1) & mask] |= 1;
            }
        }
    }

    // The 64 bits themselves, as an integer.
    uint64_t mantissa = 0;
    for (int i = 0; i < kMantLimbs; ++i) {
        const int idx = (a + i) & mask;
        if (idx == z) {
            x[z] = 0;
            z = (z + 1) & mask;
        }
        mantissa = mantissa * 1000000000u + x[idx];
    }
    // What is left over is a fraction of the significand's lowest bit. Below 64 bits of precision
    // only "is anything there" matters; at 64 the fraction decides the rounding on its own.
    Tail tail;
    if (((a + kMantLimbs) & mask) != z) {
        const uint32_t next = x[(a + kMantLimbs) & mask];
        const bool after = ((a + kMantLimbs + 1) & mask) != z;
        tail.half = next >= 500000000u;
        tail.rest = next > 500000000u || after;
    }
    return Finish<Format>(negative, mantissa, e2, tail);
}

// The hexadecimal significand: a digit is four exact bits, so the digits fill a 64-bit significand
// from its top bit down and everything after it is the rounding's tail.
template <typename Format, typename Ch>
typename Format::Type HexValue(const Ch* s, const Ch* end, bool negative) {
    using Type = typename Format::Type;
    const Ch* digitsEnd = end;
    int64_t pExponent = 0;
    for (const Ch* q = s; q < end; ++q) {
        if (*q == 'p' || *q == 'P') {
            digitsEnd = q;
            bool negativeExponent = false;
            ++q;
            if (q < end && (*q == '+' || *q == '-')) {
                negativeExponent = *q == '-';
                ++q;
            }
            for (; q < end && *q >= '0' && *q <= '9'; ++q) {
                if (pExponent < 1000000000000000000LL) pExponent = pExponent * 10 + (*q - '0');
            }
            if (negativeExponent) pExponent = -pExponent;
            break;
        }
    }

    uint64_t mantissa = 0;
    int pos = 63;        // where the next significand bit goes, counting down from the top
    int seen = 0;        // digits seen, leading zeros included
    int before = 0;      // digits before the radix point
    int first = -1;      // the first digit that isn't zero
    int skipped = 0;     // leading zero bits of that digit, which the exponent must carry
    bool started = false;
    bool haveHalf = false, half = false, rest = false, point = false;
    for (const Ch* q = s; q < digitsEnd; ++q) {
        if (*q == '.') {
            point = true;
            before = seen;
            continue;
        }
        const int digit = DigitValue(*q);
        if (digit < 0 || digit > 15) break;
        if (!digit && first < 0) {  // a leading zero digit claims no significand bit
            ++seen;
            continue;
        }
        if (first < 0) first = seen;
        ++seen;
        for (int bit = 3; bit >= 0; --bit) {
            const bool set = (digit >> bit) & 1;
            if (!started) {
                if (!set) {  // a leading zero bit: it moves the significand up, not down
                    ++skipped;
                    continue;
                }
                started = true;
            }
            if (pos >= 0) {
                if (set) mantissa |= 1ULL << pos;
                --pos;
            } else if (!haveHalf) {
                half = set;
                haveHalf = true;
            } else if (set) {
                rest = true;
            }
        }
    }
    if (first < 0) return Type(Format::Make(negative, 0, 0));  // every digit was zero
    if (!point) before = seen;

    // The significand is the digit stream with its leading bit at the top: the exponent counts what
    // stands before that bit, and the bits left over from the 64.
    const int64_t exponent = 4LL * (before - first) - skipped - 64 + pExponent;
    Tail tail;
    tail.half = half;
    tail.rest = rest;
    return Finish<Format>(negative, mantissa, exponent, tail);
}

// One float: `where` ends up past what was consumed.
template <typename Format, typename Ch>
typename Format::Type ParseFloat(const Ch*& where) {
    const Ch* s = where;
    const Ch* p = s;
    while (IsSpace(*p)) ++p;
    bool negative = false;
    if (*p == '+' || *p == '-') {
        negative = *p == '-';
        ++p;
    }

    // inf, infinity, nan and nan(payload): glibc's spellings.
    if ((*p | 32) == 'i') {
        if ((p[1] | 32) == 'n' && (p[2] | 32) == 'f') {
            const Ch* q = p + 3;
            if ((q[0] | 32) == 'i' && (q[1] | 32) == 'n' && (q[2] | 32) == 'i' && (q[3] | 32) == 't' &&
                (q[4] | 32) == 'y')
                q += 5;
            where = q;
            return Format::Infinity(negative);
        }
    } else if ((*p | 32) == 'n') {
        if ((p[1] | 32) == 'a' && (p[2] | 32) == 'n') {
            const Ch* q = p + 3;
            if (*q == '(') {
                // A payload of digits, letters and underscores that closes: glibc takes it (an
                // empty one too) and still answers a plain quiet NaN.
                const Ch* r = q + 1;
                while (*r && (*r == '_' || DigitValue(*r) >= 0)) ++r;
                if (*r == ')') {
                    where = r + 1;
                    return Format::QuietNaN(negative);
                }
            }
            where = p + 3;
            return Format::QuietNaN(negative);
        }
    }

    // Hexadecimal: 0x with at least one hex digit, an optional point and an optional p exponent.
    if (p[0] == '0' && (p[1] | 32) == 'x') {
        const Ch* q = p + 2;
        bool any = false;
        while (IsHexDigit(*q)) {
            any = true;
            ++q;
        }
        if (*q == '.') {
            ++q;
            while (IsHexDigit(*q)) {
                any = true;
                ++q;
            }
        }
        if (!any) {
            where = p + 1;  // "0x" and "0x." have no digits: the number is the zero itself
            return Format::Make(negative, 0, 0);
        }
        const Ch* numberEnd = q;
        if ((*q | 32) == 'p') {
            const Ch* e = q + 1;
            if (*e == '+' || *e == '-') ++e;
            const Ch* first = e;
            while (*e >= '0' && *e <= '9') ++e;
            if (e != first) numberEnd = e;
        }
        where = numberEnd;
        return HexValue<Format>(p + 2, numberEnd, negative);
    }

    // Decimal: digits, an optional point, then an optional exponent whose digits must follow.
    const Ch* q = p;
    bool any = false;
    while (*q >= '0' && *q <= '9') {
        any = true;
        ++q;
    }
    if (*q == '.') {
        ++q;
        while (*q >= '0' && *q <= '9') {
            any = true;
            ++q;
        }
    }
    if (!any) {
        where = s;  // nothing to convert, not even the sign or the whitespace
        return Format::Make(false, 0, 0);
    }
    const Ch* numberEnd = q;
    if ((*q | 32) == 'e') {
        const Ch* e = q + 1;
        if (*e == '+' || *e == '-') ++e;
        const Ch* first = e;
        while (*e >= '0' && *e <= '9') ++e;
        if (e != first) numberEnd = e;
    }
    where = numberEnd;
    return DecimalValue<Format>(p, numberEnd, negative);
}

// ---- integers ---------------------------------------------------------------------------------

// The strtol family. glibc's rules: leading whitespace, a sign, a base prefix only where the base
// allows one and a digit follows it, digits until the first that doesn't fit, an end pointer after
// them, and clamping with ERANGE. The C23 names also read 0b.
template <typename T, typename Ch, bool kUnsigned>
T ParseInteger(const Ch* s, Ch** end, int base, bool c23) {
    constexpr uint64_t kMax = sizeof(T) >= 8 ? ~0ULL : ((1ULL << (sizeof(T) * 8)) - 1);
    constexpr uint64_t kMaxPositive = kMax >> 1;
    constexpr uint64_t kMaxNegative = kMaxPositive + 1;
    const Ch* p = s;
    while (IsSpace(*p)) ++p;
    bool negative = false;
    if (*p == '+' || *p == '-') {
        negative = *p == '-';
        ++p;
    }
    if (base != 0 && (base < 2 || base > 36)) {
        errno = EINVAL;  // glibc's answer, end pointer left alone
        return 0;
    }
    int radix = base;
    if (radix == 0 || radix == 16) {
        if (p[0] == '0' && (p[1] | 32) == 'x') {
            const int digit = DigitValue(p[2]);
            if (digit >= 0 && digit < 16) {
                radix = 16;
                p += 2;
            }
        } else if (c23 && radix == 0 && p[0] == '0' && (p[1] | 32) == 'b') {
            const int digit = DigitValue(p[2]);
            if (digit >= 0 && digit < 2) {
                radix = 2;
                p += 2;
            }
        }
    }
    if (radix == 0) radix = *p == '0' ? 8 : 10;

    uint64_t value = 0;
    bool any = false, overflow = false;
    for (int digit = DigitValue(*p); digit >= 0 && digit < radix; ++p, digit = DigitValue(*p)) {
        any = true;
        const uint64_t add = (uint64_t)digit;
        if (value > (~0ULL - add) / (uint64_t)radix) overflow = true;
        else value = value * (uint64_t)radix + add;
    }
    if (!any) {
        if (end) *end = (Ch*)s;
        return 0;
    }
    if (end) *end = (Ch*)p;
    if (kUnsigned) {
        if (overflow) {
            errno = ERANGE;
            return (T)kMax;
        }
        return (T)(negative ? 0 - value : value);  // a negative unsigned wraps, as glibc's does
    }
    if (overflow || value > (negative ? kMaxNegative : kMaxPositive)) {
        errno = ERANGE;
        return negative ? (T)(0 - kMaxNegative) : (T)kMaxPositive;
    }
    return (T)(negative ? 0 - value : value);
}

}  // namespace

#define HIDE(name) __asm__(".hidden " #name);
HIDE(strtof) HIDE(strtod) HIDE(strtold) HIDE(__strtof_l) HIDE(__strtod_l) HIDE(strtold_l)
HIDE(strtol) HIDE(strtoll) HIDE(strtoul) HIDE(strtoull)
HIDE(__isoc23_strtol) HIDE(__isoc23_strtoll) HIDE(__isoc23_strtoul) HIDE(__isoc23_strtoull)
HIDE(wcstol) HIDE(wcstoll) HIDE(wcstoul) HIDE(wcstoull)
HIDE(__isoc23_wcstol) HIDE(__isoc23_wcstoll) HIDE(__isoc23_wcstoul) HIDE(__isoc23_wcstoull)
HIDE(wcstof) HIDE(wcstod) HIDE(wcstold)
#undef HIDE

extern "C" {
float strtof(const char* s, char** end) {
    const char* where = s;
    const float value = ParseFloat<FloatFormat>(where);
    if (end) *end = (char*)where;
    return value;
}
double strtod(const char* s, char** end) {
    const char* where = s;
    const double value = ParseFloat<DoubleFormat>(where);
    if (end) *end = (char*)where;
    return value;
}
long double strtold(const char* s, char** end) {
    const char* where = s;
    const long double value = ParseFloat<LongDoubleFormat>(where);
    if (end) *end = (char*)where;
    return value;
}
// The locale-taking forms the static libstdc++ calls (its num_get facet). The locale is ignored:
// cordinal's locale layer serves the C locale's data, whose decimal point is '.'.
float __strtof_l(const char* s, char** end, void*) { return strtof(s, end); }
double __strtod_l(const char* s, char** end, void*) { return strtod(s, end); }
long double strtold_l(const char* s, char** end, void*) { return strtold(s, end); }

long strtol(const char* s, char** end, int base) { return ParseInteger<long, char, false>(s, end, base, false); }
long long strtoll(const char* s, char** end, int base) {
    return ParseInteger<long long, char, false>(s, end, base, false);
}
unsigned long strtoul(const char* s, char** end, int base) {
    return ParseInteger<unsigned long, char, true>(s, end, base, false);
}
unsigned long long strtoull(const char* s, char** end, int base) {
    return ParseInteger<unsigned long long, char, true>(s, end, base, false);
}
long __isoc23_strtol(const char* s, char** end, int base) {
    return ParseInteger<long, char, false>(s, end, base, true);
}
long long __isoc23_strtoll(const char* s, char** end, int base) {
    return ParseInteger<long long, char, false>(s, end, base, true);
}
unsigned long __isoc23_strtoul(const char* s, char** end, int base) {
    return ParseInteger<unsigned long, char, true>(s, end, base, true);
}
unsigned long long __isoc23_strtoull(const char* s, char** end, int base) {
    return ParseInteger<unsigned long long, char, true>(s, end, base, true);
}

long wcstol(const wchar_t* s, wchar_t** end, int base) {
    return ParseInteger<long, wchar_t, false>(s, end, base, false);
}
long long wcstoll(const wchar_t* s, wchar_t** end, int base) {
    return ParseInteger<long long, wchar_t, false>(s, end, base, false);
}
unsigned long wcstoul(const wchar_t* s, wchar_t** end, int base) {
    return ParseInteger<unsigned long, wchar_t, true>(s, end, base, false);
}
unsigned long long wcstoull(const wchar_t* s, wchar_t** end, int base) {
    return ParseInteger<unsigned long long, wchar_t, true>(s, end, base, false);
}
long __isoc23_wcstol(const wchar_t* s, wchar_t** end, int base) {
    return ParseInteger<long, wchar_t, false>(s, end, base, true);
}
long long __isoc23_wcstoll(const wchar_t* s, wchar_t** end, int base) {
    return ParseInteger<long long, wchar_t, false>(s, end, base, true);
}
unsigned long __isoc23_wcstoul(const wchar_t* s, wchar_t** end, int base) {
    return ParseInteger<unsigned long, wchar_t, true>(s, end, base, true);
}
unsigned long long __isoc23_wcstoull(const wchar_t* s, wchar_t** end, int base) {
    return ParseInteger<unsigned long long, wchar_t, true>(s, end, base, true);
}

float wcstof(const wchar_t* s, wchar_t** end) {
    const wchar_t* where = s;
    const float value = ParseFloat<FloatFormat>(where);
    if (end) *end = (wchar_t*)where;
    return value;
}
double wcstod(const wchar_t* s, wchar_t** end) {
    const wchar_t* where = s;
    const double value = ParseFloat<DoubleFormat>(where);
    if (end) *end = (wchar_t*)where;
    return value;
}
long double wcstold(const wchar_t* s, wchar_t** end) {
    const wchar_t* where = s;
    const long double value = ParseFloat<LongDoubleFormat>(where);
    if (end) *end = (wchar_t*)where;
    return value;
}
}  // extern "C"
