// cordinal's own printf family (stage 5a of replacing glibc): the formatting core behind vfprintf,
// printf, snprintf, sprintf, asprintf, dprintf, swprintf and their fortified _chk forms. It writes
// through cordinal's FILE streams (stdio.cpp), which glibc's printf cannot do and the other way
// round.
//
// The float formatter is musl's fmt_fp (src/stdio/vfprintf.c, MIT licensed, read November 17,
// 2026): a base-10^9 big integer filled from the mantissa, with the same ties-to-even rounding
// glibc uses, so %f, %e, %g and %a agree with glibc's output without multiprecision code. The
// state machine around it follows musl's printf_core too, with glibc's own answers where the two
// differ: %p prints "(nil)" for a null pointer and otherwise reads as %#x; a null %s with a
// precision shorter than "(null)" prints nothing; width and precision may come from *; flags and
// width on %% are ignored (%5% prints one %). Positional arguments (%1$d) are not implemented:
// nothing in cordinal or the static libstdc++ uses them (the libc check says so).
//
// The output is multiplied through the locale's own conversions (mbrtowc and wcrtomb, cordinal's
// own since stage 5b): %ls in a narrow format becomes multibyte, %s in a wide format becomes wide.
// The C and UTF-8 locales are what cordinal runs in, so the bytes agree with glibc's.
#include "stdio_internal.h"

#include <errno.h>
#include <float.h>
#include <limits.h>
#include <string.h>

// The process-wide allocator (malloc.cpp), glibc's strerror for %m, and glibc's multibyte
// conversions. None of their headers is included: <wchar.h> declares swprintf, which this file
// defines.
extern "C" void* malloc(size_t);
extern "C" void free(void*);
extern "C" void* realloc(void*, size_t);
extern "C" char* strerror(int);
extern "C" size_t mbrtowc(wchar_t*, const char*, size_t, void*);
extern "C" size_t wcrtomb(char*, wchar_t, void*);
extern "C" int btowc(int);
extern "C" __attribute__((noreturn)) void abort(void) noexcept;
// printf.cpp's output goes through the stream layer (stdio.cpp) and, for dprintf, the descriptor.
extern "C" FILE* stdout;
extern "C" size_t fwrite(const void*, size_t, size_t, FILE*);
extern "C" long write(int, const void*, size_t);

#ifndef EOF
#define EOF (-1)
#endif

namespace {

// glibc's mbstate_t on x86-64 is 8 bytes: a count and a small union. Its layout is the header's,
// which this file cannot include (it declares swprintf); the libc check pins the size down.
struct MbState {
    int count;
    union {
        unsigned int value;
        char bytes[4];
    } state;
};

constexpr size_t kUnlimited = (size_t)-1;

// Where formatted output goes: a FILE, a caller's narrow buffer (sprintf/snprintf, with a fixed
// capacity), a growing buffer (vfprintf/asprintf, which hands the bytes over in one write), or a
// caller's wide buffer (swprintf). `length` counts the units the caller is told about: bytes for
// every narrow target, wide characters for a wide one, whether or not they fit.
struct Sink {
    FILE* file = nullptr;
    char* buffer = nullptr;
    size_t capacity = 0;  // narrow: includes the terminating NUL; 0 means no bytes may be stored
    bool growable = false;
    wchar_t* wideBuffer = nullptr;
    size_t wideCapacity = 0;
    bool isWide = false;
    bool failed = false;
    int error = 0;
    size_t length = 0;
    MbState state{};

    void Fail(int number) {
        if (!failed) error = number;
        failed = true;
    }

    void StoreNarrow(const char* s, size_t n) {
        if (growable) {
            if (length + n >= capacity) {  // one spare byte for Finish's terminator
                size_t grown = capacity ? capacity : 256;
                while (grown < length + n) grown *= 2;
                char* bigger = (char*)realloc(buffer, grown);
                if (!bigger) {
                    Fail(ENOMEM);
                    return;
                }
                buffer = bigger;
                capacity = grown;
            }
            memcpy(buffer + length, s, n);
            return;
        }
        if (capacity) {
            const size_t room = length < capacity - 1 ? capacity - 1 - length : 0;
            if (room) memcpy(buffer + length, s, n < room ? n : room);
        }
    }

    void EmitWide(wchar_t wc) {
        if (isWide) {
            if (wideCapacity && length < wideCapacity - 1) wideBuffer[length] = wc;
            ++length;
            return;
        }
        char mb[8];
        MbState fresh{};
        const size_t n = wcrtomb(mb, wc, &fresh);
        if (n == (size_t)-1) {
            Fail(EILSEQ);
            return;
        }
        StoreNarrow(mb, n);
        if (!failed) length += n;
    }

    void Put(const char* s, size_t n) {
        if (!n) return;
        if (!isWide) {
            StoreNarrow(s, n);
            if (!failed) length += n;
            return;
        }
        // Wide output decodes the bytes one at a time, so a multibyte character split between two
        // calls still comes out whole, as glibc's mbsrtowcs-based conversion does.
        for (size_t i = 0; i < n; ++i) {
            wchar_t wc;
            const size_t r = mbrtowc(&wc, &s[i], 1, &state);
            if (r == (size_t)-2) continue;
            if (r == (size_t)-1) {
                Fail(EILSEQ);
                state = MbState{};
                continue;
            }
            if (wideCapacity && length < wideCapacity - 1) wideBuffer[length] = wc;
            ++length;
        }
    }

    void Pad(char c, size_t count) {
        char block[64];
        memset(block, c, sizeof block);
        while (count) {
            const size_t take = count < sizeof block ? count : sizeof block;
            Put(block, take);
            count -= take;
        }
    }

    bool Finish() {
        if (isWide) {
            if (state.count != 0) Fail(EILSEQ);
            if (wideBuffer && wideCapacity) {
                const size_t at = length < wideCapacity ? length : wideCapacity - 1;
                wideBuffer[at] = 0;
            }
        } else if (buffer && capacity) {
            const size_t at = length < capacity ? length : capacity - 1;
            buffer[at] = 0;
        }
        if (failed) errno = error ? error : EILSEQ;
        return !failed;
    }
};

enum : unsigned {
    kAlternate = 1u << 0,
    kZeroPad = 1u << 1,
    kLeft = 1u << 2,
    kSpace = 1u << 3,
    kPlus = 1u << 4,
    kGroup = 1u << 5,  // read and ignored, as musl does; the C locale has no grouping
};

enum Length { kNone, kHH, kH, kL, kLL, kBigL, kZ, kT, kJ };

int Min(int a, int b) { return a < b ? a : b; }
int Max(int a, int b) { return a > b ? a : b; }

// frexpl for x86-64's 80-bit long double: a mantissa in [0.5, 1) and a power of two, without a
// libm call. Zero and the infinities are left for the caller.
long double SplitMantissa(long double y, int* exponent) {
    union {
        long double value;
        struct {
            unsigned long long mantissa;
            unsigned short signExp;
        } parts;
    } bits;
    bits.value = y;
    int exp = bits.parts.signExp & 0x7fff;
    unsigned long long mantissa = bits.parts.mantissa;
    if (exp == 0x7fff || (exp == 0 && mantissa == 0)) {
        *exponent = 0;
        return y;
    }
    if (exp == 0) {  // a denormal: shift until the integer bit is set
        exp = 1 - 16383;
        while (!(mantissa & (1ull << 63))) {
            mantissa <<= 1;
            --exp;
        }
    }
    *exponent = exp - 16382;
    bits.parts.mantissa = mantissa;
    bits.parts.signExp = (unsigned short)((bits.parts.signExp & 0x8000) | 0x3ffe);
    return bits.value;
}

void PadTo(Sink& out, char c, int width, int length, unsigned flags) {
    if ((flags & (kLeft | kZeroPad)) || length >= width) return;
    out.Pad(c, (size_t)(width - length));
}

void PadZeros(Sink& out, int count) {
    if (count > 0) out.Pad('0', (size_t)count);
}

const char kHexDigits[] = "0123456789ABCDEF";

char* FormatHex(unsigned long long x, char* end, bool upper) {
    for (; x; x >>= 4) *--end = (char)(kHexDigits[x & 15] | (upper ? 0 : 0x20));
    return end;
}

char* FormatOctal(unsigned long long x, char* end) {
    for (; x; x >>= 3) *--end = (char)('0' + (x & 7));
    return end;
}

char* FormatDecimal(unsigned long long x, char* end) {
    for (; x; x /= 10) *--end = (char)('0' + x % 10);
    return end;
}

// glibc's %a and %A: the significand as hexadecimal digits with the point after its first digit,
// which is how the C library this replaces prints them (x87's explicit integer bit gives a leading
// 8 to 15 for a normal long double, and a subnormal stays under the smallest normal exponent).
// Rounding keeps ties to even on the last kept digit; a leading 2 is printed as 2, and a carry out
// of the leading digit makes it 1 with the exponent four higher (16 is 2^4), all as glibc does.
template <class T>
int FormatHexFloat(Sink& out, T value, int width, int precision, unsigned flags, char conversion) {
    const char* prefix = "-0X+0X 0X-0x+0x 0x";
    int pl = 1;
    if (__builtin_signbit(value)) {
        value = -value;
    } else if (flags & kPlus) {
        prefix += 3;
    } else if (flags & kSpace) {
        prefix += 6;
    } else {
        prefix++;
        pl = 0;
    }
    if (!__builtin_isfinite(value)) {
        const char* text = (conversion & 32) ? "inf" : "INF";
        if (value != value) text = (conversion & 32) ? "nan" : "NAN";
        PadTo(out, ' ', width, 3 + pl, flags & ~kZeroPad);
        out.Put(prefix, (size_t)pl);
        out.Put(text, 3);
        PadTo(out, ' ', width, 3 + pl, flags ^ kLeft);
        return width > 3 + pl ? width : 3 + pl;
    }
    if (conversion & 32) prefix += 9;  // 0x or 0X
    pl += 2;
    const bool lowercase = (conversion & 32) != 0;
    auto Digit = [lowercase](int value) { return (char)(kHexDigits[value] | (lowercase ? 0x20 : 0)); };

    // The packed significand's leading nibble, the bits after it, and glibc's exponent.
    int lead = 0;
    unsigned long long fraction = 0;
    int nibbles = 0;  // hex digits the significand has after the point
    int exponent = 0;
    if constexpr (sizeof(T) == sizeof(double)) {
        unsigned long long bits = 0;
        memcpy(&bits, &value, sizeof bits);
        const int field = (int)((bits >> 52) & 0x7ff);
        fraction = bits & ((1ull << 52) - 1);
        nibbles = 13;
        if (field == 0) {
            exponent = fraction ? -1022 : 0;  // a subnormal keeps the smallest normal exponent
        } else {
            lead = 1;
            exponent = field - 1023;
        }
    } else {
        struct X87 {
            unsigned long long significand;
            unsigned short signExponent;
        } parts;
        memcpy(&parts, &value, sizeof parts);
        const int field = parts.signExponent & 0x7fff;
        lead = (int)(parts.significand >> 60);
        fraction = parts.significand & ((1ull << 60) - 1);
        nibbles = 15;
        if (field == 0) {
            exponent = parts.significand ? -16385 : 0;
        } else {
            exponent = field - 16386;
        }
    }

    // Round the fraction to the precision asked for, in nibbles, with ties to even.
    int shown = nibbles;
    unsigned long long kept = fraction;
    if (precision >= 0) {
        shown = precision;
        if (precision < nibbles) {
            const int dropped = (nibbles - precision) * 4;
            const unsigned long long rest = fraction & ((1ull << dropped) - 1);
            const unsigned long long half = 1ull << (dropped - 1);
            const bool odd = precision ? ((fraction >> dropped) & 1) != 0 : (lead & 1) != 0;
            if (rest > half || (rest == half && odd)) {
                if (precision == 0) {
                    ++lead;
                } else {
                    const unsigned long long keptBits = fraction >> dropped;
                    if (keptBits + 1 == (1ull << (precision * 4))) {
                        kept = 0;
                        ++lead;
                    } else {
                        kept = keptBits + 1;
                    }
                    kept <<= dropped;
                }
            } else {
                kept = (fraction >> dropped) << dropped;
            }
        }
        if (lead == 16) {  // the carry out of the leading digit renormalizes it: 16 is 2^4
            lead = 1;
            exponent += 4;
        }
    } else {
        // Without a precision the digits the significand has are printed, with trailing zeros left
        // out ('#' only keeps the point, as glibc's does).
        while (shown > 0 && ((kept >> ((nibbles - shown) * 4)) & 15) == 0) --shown;
    }

    char digits[16];
    const int fromSignificand = shown < nibbles ? shown : nibbles;
    for (int i = 0; i < fromSignificand; ++i) {
        const int at = nibbles - 1 - i;
        digits[i] = (at >= 0) ? Digit((int)((kept >> (at * 4)) & 15)) : '0';
    }
    char exponentText[16], *exponentEnd = exponentText + sizeof exponentText;
    char* at = FormatDecimal((unsigned long long)(exponent < 0 ? -exponent : exponent), exponentEnd);
    if (at == exponentEnd) *--at = '0';
    *--at = (char)(exponent < 0 ? '-' : '+');
    *--at = (char)(conversion + ('p' - 'a'));

    const int exponentLength = (int)(exponentEnd - at);
    // A precision of zero prints no digits and no point; '#' keeps the point in every case.
    const bool point = shown > 0 || (flags & kAlternate);
    const long long length = (long long)pl + 1 + exponentLength + (point ? 1 + shown : 0);
    if (length > INT_MAX) return -1;
    const int total = (int)length;
    PadTo(out, ' ', width, total, flags);
    out.Put(prefix, (size_t)pl);
    PadTo(out, '0', width, total, flags ^ kZeroPad);
    const char digit = Digit(lead);
    out.Put(&digit, 1);
    if (point) {
        out.Put(".", 1);
        out.Put(digits, (size_t)fromSignificand);
        if (shown > fromSignificand) out.Pad('0', (size_t)(shown - fromSignificand));
    }
    out.Put(at, (size_t)exponentLength);
    PadTo(out, ' ', width, total, flags ^ kLeft);
    return width > total ? width : total;
}

// musl's fmt_fp, with the sink in place of a FILE. Its rounding decides ties to even at the
// requested digit and then trims %g to the shortest form; the long double is plain 80-bit x86.
int FormatFloat(Sink& out, long double y, int width, int precision, unsigned flags, char conversion,
                bool isLongDouble) {
    const int maxMantissaBits = isLongDouble ? LDBL_MANT_DIG : DBL_MANT_DIG;
    const int maxExponent = isLongDouble ? LDBL_MAX_EXP : DBL_MAX_EXP;
    const int maxMantissaSlots = 1 + (maxMantissaBits - 29 + 7) / 8 + 1;
    const int maxExponentSlots = (maxExponent + maxMantissaBits + 28 + 8) / 9;
    uint32_t big[1 + maxMantissaSlots + maxExponentSlots + 8];
    uint32_t *a, *d, *r, *z;
    int e2 = 0, e, i, j, l;
    char buf[9 + LDBL_MANT_DIG / 4];
    const char* prefix = "-0X+0X 0X-0x+0x 0x";
    int pl;
    char exponentText[3 * sizeof(int)], *exponent = &exponentText[3 * sizeof(int)];

    pl = 1;
    if (__builtin_signbit(y)) {
        y = -y;
    } else if (flags & kPlus) {
        prefix += 3;
    } else if (flags & kSpace) {
        prefix += 6;
    } else {
        prefix++;
        pl = 0;
    }

    if (!__builtin_isfinite(y)) {
        const char* text = (conversion & 32) ? "inf" : "INF";
        if (y != y) text = (conversion & 32) ? "nan" : "NAN";
        PadTo(out, ' ', width, 3 + pl, flags & ~kZeroPad);
        out.Put(prefix, (size_t)pl);
        out.Put(text, 3);
        PadTo(out, ' ', width, 3 + pl, flags ^ kLeft);
        return width > 3 + pl ? width : 3 + pl;
    }

    y = SplitMantissa(y, &e2) * 2;
    if (y) --e2;

    if (precision < 0) precision = 6;

    if (y) y *= 0x1p28, e2 -= 28;

    if (e2 < 0)
        a = r = z = big;
    else
        a = r = z = big + sizeof(big) / sizeof(*big) - maxMantissaSlots - 1;

    do {
        *z = (uint32_t)y;
        y = 1000000000 * (y - *z++);
    } while (y);

    while (e2 > 0) {
        uint32_t carry = 0;
        const int shift = e2 < 29 ? e2 : 29;
        for (d = z - 1; d >= a; d--) {
            const uint64_t x = ((uint64_t)*d << shift) + carry;
            *d = (uint32_t)(x % 1000000000);
            carry = (uint32_t)(x / 1000000000);
        }
        if (carry) *--a = carry;
        while (z > a && !z[-1]) z--;
        e2 -= shift;
    }
    while (e2 < 0) {
        uint32_t carry = 0, *b;
        const int shift = -e2 < 9 ? -e2 : 9;
        const int need = 1 + (precision + LDBL_MANT_DIG / 3U + 8) / 9;
        for (d = a; d < z; d++) {
            const uint32_t remainder = *d & ((1u << shift) - 1);
            *d = (*d >> shift) + carry;
            carry = (1000000000u >> shift) * remainder;
        }
        if (!*a) a++;
        if (carry) *z++ = carry;
        b = (conversion | 32) == 'f' ? r : a;  // do not compute far past the precision asked for
        if (z - b > need) z = b + need;
        e2 += shift;
    }

    if (a < z)
        for (i = 10, e = 9 * (int)(r - a); *a >= (uint32_t)i; i *= 10, e++);
    else
        e = 0;

    // Round to the requested place: j digits after the radix point (which may be negative).
    j = precision - ((conversion | 32) != 'f') * e - ((conversion | 32) == 'g' && precision);
    if (j < 9 * (int)(z - r - 1)) {
        uint32_t x;
        // Avoid C's division of negative numbers.
        d = r + 1 + ((j + 9 * LDBL_MAX_EXP) / 9 - LDBL_MAX_EXP);
        j += 9 * LDBL_MAX_EXP;
        j %= 9;
        for (i = 10, j++; j < 9; i *= 10, j++);
        x = *d % (uint32_t)i;
        if (x || d + 1 != z) {  // significant digits follow the cut
            long double round = 2 / LDBL_EPSILON;
            long double small;
            if ((*d / (uint32_t)i & 1) || (i == 1000000000 && d > a && (d[-1] & 1))) round += 2;
            if (x < (uint32_t)i / 2)
                small = 0x0.8p0;
            else if (x == (uint32_t)i / 2 && d + 1 == z)
                small = 0x1.0p0;
            else
                small = 0x1.8p0;
            if (pl && *prefix == '-') round *= -1, small *= -1;
            *d -= x;
            if (round + small != round) {  // probe whether this rounds the value up
                *d = *d + (uint32_t)i;
                while (*d > 999999999) {
                    *d-- = 0;
                    if (d < a) *--a = 0;
                    (*d)++;
                }
                for (i = 10, e = 9 * (int)(r - a); *a >= (uint32_t)i; i *= 10, e++);
            }
        }
        if (z > d + 1) z = d + 1;
    }
    for (; z > a && !z[-1]; z--);

    if ((conversion | 32) == 'g') {
        if (!precision) precision++;
        if (precision > e && e >= -4) {
            conversion--;
            precision -= e + 1;
        } else {
            conversion -= 2;
            precision--;
        }
        if (!(flags & kAlternate)) {
            if (z > a && z[-1])
                for (i = 10, j = 0; z[-1] % (uint32_t)i == 0; i *= 10, j++);
            else
                j = 9;
            if ((conversion | 32) == 'f')
                precision = Min(precision, Max(0, 9 * (int)(z - r - 1) - j));
            else
                precision = Min(precision, Max(0, 9 * (int)(z - r - 1) + e - j));
        }
    }
    if (precision > INT_MAX - 1 - (precision || (flags & kAlternate))) return -1;
    l = 1 + precision + (precision || (flags & kAlternate));
    if ((conversion | 32) == 'f') {
        if (e > INT_MAX - l) return -1;
        if (e > 0) l += e;
    } else {
        exponent = FormatDecimal(e < 0 ? -e : e, exponent);
        while (&exponentText[3 * sizeof(int)] - exponent < 2) *--exponent = '0';
        *--exponent = (char)(e < 0 ? '-' : '+');
        *--exponent = conversion;
        if (&exponentText[3 * sizeof(int)] - exponent > INT_MAX - l) return -1;
        l += (int)(&exponentText[3 * sizeof(int)] - exponent);
    }

    if (l > INT_MAX - pl) return -1;
    PadTo(out, ' ', width, pl + l, flags);
    out.Put(prefix, (size_t)pl);
    PadTo(out, '0', width, pl + l, flags ^ kZeroPad);

    if ((conversion | 32) == 'f') {
        if (a > r) a = r;
        for (d = a; d <= r; d++) {
            char* text = FormatDecimal(*d, buf + 9);
            if (d != a)
                while (text > buf) *--text = '0';
            else if (text == buf + 9)
                *--text = '0';
            out.Put(text, (size_t)(buf + 9 - text));
        }
        if (precision || (flags & kAlternate)) out.Put(".", 1);
        for (; d < z && precision > 0; d++, precision -= 9) {
            char* text = FormatDecimal(*d, buf + 9);
            while (text > buf) *--text = '0';
            out.Put(text, Min(9, precision) > 0 ? (size_t)Min(9, precision) : 0);
        }
        PadZeros(out, precision);
    } else {
        if (z <= a) z = a + 1;
        for (d = a; d < z && precision >= 0; d++) {
            char* text = FormatDecimal(*d, buf + 9);
            if (text == buf + 9) *--text = '0';
            if (d != a)
                while (text > buf) *--text = '0';
            else {
                out.Put(text++, 1);
                if (precision > 0 || (flags & kAlternate)) out.Put(".", 1);
            }
            out.Put(text, Min((int)(buf + 9 - text), precision) > 0
                                ? (size_t)Min((int)(buf + 9 - text), precision)
                                : 0);
            precision -= (int)(buf + 9 - text);
        }
        PadZeros(out, precision);
        out.Put(exponent, (size_t)(&exponentText[3 * sizeof(int)] - exponent));
    }

    PadTo(out, ' ', width, pl + l, flags ^ kLeft);
    return width > pl + l ? width : pl + l;
}

// How many wide characters the first `precision` bytes of `text` become (all of it for a negative
// precision). A complete character that starts inside the limit is converted even when it runs
// past it, which is what glibc's mbsrtowcs-based conversion does.
size_t WideLengthOfBytes(const char* text, int precision) {
    MbState state{};
    size_t used = 0, chars = 0;
    bool insideCharacter = false;
    while (text[used]) {
        if (!insideCharacter && precision >= 0 && used >= (size_t)precision) break;
        wchar_t wc;
        const size_t r = mbrtowc(&wc, &text[used], 1, &state);
        ++used;
        if (r == (size_t)-2) {
            insideCharacter = true;
            continue;
        }
        if (r == (size_t)-1) return (size_t)-1;
        insideCharacter = false;
        ++chars;
    }
    return insideCharacter ? (size_t)-1 : chars;
}

// How many bytes the first `precision` bytes of a wide string occupy in the current locale.
size_t NarrowLengthOfWide(const wchar_t* text, int precision) {
    size_t bytes = 0;
    for (; *text; ++text) {
        char mb[8];
        MbState state{};
        const size_t n = wcrtomb(mb, *text, &state);
        if (n == (size_t)-1) return (size_t)-1;
        if (precision >= 0 && bytes + n > (size_t)precision) break;
        bytes += n;
    }
    return bytes;
}

void EmitWideText(Sink& out, const wchar_t* text, size_t chars) {
    for (size_t i = 0; i < chars; ++i) out.EmitWide(text[i]);
}

// The first `precision` bytes of a narrow string, decoded to wide output.
void EmitNarrowAsWide(Sink& out, const char* text, int precision) {
    MbState state{};
    size_t used = 0;
    bool insideCharacter = false;
    while (text[used]) {
        if (!insideCharacter && precision >= 0 && used >= (size_t)precision) break;
        wchar_t wc;
        const size_t r = mbrtowc(&wc, &text[used], 1, &state);
        ++used;
        if (r == (size_t)-2) {
            insideCharacter = true;
            continue;
        }
        if (r == (size_t)-1) {
            out.Fail(EILSEQ);
            return;
        }
        insideCharacter = false;
        out.EmitWide(wc);
    }
    if (insideCharacter) out.Fail(EILSEQ);
}

// The first `precision` bytes of a wide string, converted to narrow output.
void EmitWideAsNarrow(Sink& out, const wchar_t* text, int precision) {
    size_t bytes = 0;
    for (; *text; ++text) {
        char mb[8];
        MbState state{};
        const size_t n = wcrtomb(mb, *text, &state);
        if (n == (size_t)-1) {
            out.Fail(EILSEQ);
            return;
        }
        if (precision >= 0 && bytes + n > (size_t)precision) break;
        out.Put(mb, n);
        bytes += n;
    }
}

struct Spec {
    unsigned flags = 0;
    int width = 0;
    int precision = -1;
    Length length = kNone;
    char conversion = 0;
};

int ParseDecimal(const char** at) {
    int value = 0;
    const char* s = *at;
    while (*s >= '0' && *s <= '9') {
        if (value > (INT_MAX - 9) / 10) value = INT_MAX;  // glibc's EOVERFLOW, from a saner place
        else value = value * 10 + (*s - '0');
        ++s;
    }
    *at = s;
    return value;
}

// Pad `length` units out to `width` with spaces, on the side the flags ask for.
void EmitPadded(Sink& out, const char* text, size_t length, const Spec& spec, int* count) {
    if (spec.width > 0 && (size_t)spec.width > length) {
        const size_t pad = (size_t)spec.width - length;
        if (spec.flags & kLeft) {
            out.Put(text, length);
            out.Pad(' ', pad);
        } else {
            out.Pad(' ', pad);
            out.Put(text, length);
        }
        *count += spec.width;
    } else {
        out.Put(text, length);
        *count += (int)length;
    }
}

// The formatting core, musl's printf_core without positional arguments.
int FormatCore(Sink& out, const char* format, va_list* ap) {
    const char* s = format;
    int count = 0;
    for (;;) {
        if (!*s) break;
        const char* start = s;
        while (*s && *s != '%') ++s;
        if (s != start) {
            out.Put(start, (size_t)(s - start));
            count += (int)(s - start);
        }
        if (!*s) break;
        ++s;
        if (*s == '%') {
            out.Put("%", 1);
            ++count;
            ++s;
            continue;
        }

        Spec spec;
        for (;; ++s) {
            switch (*s) {
                case '#': spec.flags |= kAlternate; continue;
                case '0': spec.flags |= kZeroPad; continue;
                case '-': spec.flags |= kLeft; continue;
                case ' ': spec.flags |= kSpace; continue;
                case '+': spec.flags |= kPlus; continue;
                case '\'': spec.flags |= kGroup; continue;
                default: break;
            }
            break;
        }
        if (*s == '*') {
            const int width = va_arg(*ap, int);
            if (width < 0) {
                spec.flags |= kLeft;
                spec.width = width == INT_MIN ? INT_MAX : -width;
            } else {
                spec.width = width;
            }
            ++s;
        } else if (*s >= '0' && *s <= '9') {
            spec.width = ParseDecimal(&s);
        }
        if (*s == '.') {
            ++s;
            if (*s == '*') {
                const int precision = va_arg(*ap, int);
                spec.precision = precision < 0 ? -1 : precision;
                ++s;
            } else {
                spec.precision = ParseDecimal(&s);
            }
        }

        switch (*s) {
            case 'h':
                if (s[1] == 'h') spec.length = kHH, s += 2;
                else spec.length = kH, ++s;
                break;
            case 'l':
                if (s[1] == 'l') spec.length = kLL, s += 2;
                else spec.length = kL, ++s;
                break;
            case 'L': spec.length = kBigL; ++s; break;
            case 'z': spec.length = kZ; ++s; break;
            case 't': spec.length = kT; ++s; break;
            case 'j': spec.length = kJ; ++s; break;
            default: break;
        }
        spec.conversion = *s;
        if (!spec.conversion) {
            out.Fail(EINVAL);
            break;
        }
        ++s;

        if (spec.flags & kLeft) spec.flags &= ~kZeroPad;
        // glibc ignores the zero flag of an integer conversion when a precision is given.
        if (spec.precision >= 0 &&
            (spec.conversion == 'd' || spec.conversion == 'i' || spec.conversion == 'u' ||
             spec.conversion == 'o' || spec.conversion == 'x' || spec.conversion == 'X' ||
             spec.conversion == 'p'))
            spec.flags &= ~kZeroPad;

        switch (spec.conversion) {
            case 'd':
            case 'i': {
                intmax_t v;
                switch (spec.length) {
                    case kHH: v = (signed char)va_arg(*ap, int); break;
                    case kH: v = (short)va_arg(*ap, int); break;
                    case kL: v = va_arg(*ap, long); break;
                    case kLL: v = va_arg(*ap, long long); break;
                    case kZ: v = (intmax_t)va_arg(*ap, size_t); break;
                    case kT: v = va_arg(*ap, ptrdiff_t); break;
                    case kJ: v = va_arg(*ap, intmax_t); break;
                    default: v = va_arg(*ap, int); break;
                }
                const bool negative = v < 0;
                const uintmax_t magnitude = negative ? (uintmax_t)0 - (uintmax_t)v : (uintmax_t)v;
                char digits[72];
                char* end = digits + sizeof digits;
                char* at = FormatDecimal(magnitude, end);
                size_t length = (size_t)(end - at);
                size_t zeros = 0;
                if (spec.precision == 0 && magnitude == 0) {
                    length = 0;
                } else if (!length) {  // a zero digit, which FormatDecimal leaves out
                    zeros = spec.precision > 1 ? (size_t)spec.precision : 1;
                } else if (spec.precision > 0 && (size_t)spec.precision > length) {
                    zeros = (size_t)spec.precision - length;
                }
                const char* prefix = "";
                size_t prefixLength = 0;
                if (negative) prefix = "-", prefixLength = 1;
                else if (spec.flags & kPlus) prefix = "+", prefixLength = 1;
                else if (spec.flags & kSpace) prefix = " ", prefixLength = 1;
                size_t total = prefixLength + zeros + length;
                if (spec.width > 0 && (size_t)spec.width > total) {
                    const size_t pad = (size_t)spec.width - total;
                    if (spec.flags & kLeft) {
                        out.Put(prefix, prefixLength);
                        out.Pad('0', zeros);
                        out.Put(at, length);
                        out.Pad(' ', pad);
                    } else if (spec.flags & kZeroPad) {
                        out.Put(prefix, prefixLength);
                        out.Pad('0', pad + zeros);
                        out.Put(at, length);
                    } else {
                        out.Pad(' ', pad);
                        out.Put(prefix, prefixLength);
                        out.Pad('0', zeros);
                        out.Put(at, length);
                    }
                    total = (size_t)spec.width;
                } else {
                    out.Put(prefix, prefixLength);
                    out.Pad('0', zeros);
                    out.Put(at, length);
                }
                count += (int)total;
                break;
            }
            case 'u':
            case 'o':
            case 'x':
            case 'X': {
                uintmax_t v;
                switch (spec.length) {
                    case kHH: v = (unsigned char)va_arg(*ap, unsigned int); break;
                    case kH: v = (unsigned short)va_arg(*ap, unsigned int); break;
                    case kL: v = va_arg(*ap, unsigned long); break;
                    case kLL: v = va_arg(*ap, unsigned long long); break;
                    case kZ: v = va_arg(*ap, size_t); break;
                    case kT: v = (uintmax_t)va_arg(*ap, ptrdiff_t); break;
                    case kJ: v = va_arg(*ap, uintmax_t); break;
                    default: v = va_arg(*ap, unsigned int); break;
                }
                const unsigned base = spec.conversion == 'o' ? 8 : (spec.conversion == 'u' ? 10 : 16);
                const bool upper = spec.conversion == 'X';
                char digits[72];
                char* end = digits + sizeof digits;
                char* at = base == 8 ? FormatOctal(v, end)
                                      : base == 10 ? FormatDecimal(v, end) : FormatHex(v, end, upper);
                size_t length = (size_t)(end - at);
                size_t zeros = 0;
                if (spec.precision == 0 && v == 0) {
                    length = 0;
                } else if (!length) {  // a zero digit, which the loops above leave out
                    zeros = spec.precision > 1 ? (size_t)spec.precision : 1;
                } else if (spec.precision > 0 && (size_t)spec.precision > length) {
                    zeros = (size_t)spec.precision - length;
                }
                const char* prefix = "";
                size_t prefixLength = 0;
                if ((spec.flags & kAlternate) && base == 16 && v) {
                    prefix = upper ? "0X" : "0x";
                    prefixLength = 2;
                }
                if ((spec.flags & kAlternate) && base == 8 && !zeros && (!length || at[0] != '0'))
                    zeros = 1;
                size_t total = prefixLength + zeros + length;
                if (spec.width > 0 && (size_t)spec.width > total) {
                    const size_t pad = (size_t)spec.width - total;
                    if (spec.flags & kLeft) {
                        out.Put(prefix, prefixLength);
                        out.Pad('0', zeros);
                        out.Put(at, length);
                        out.Pad(' ', pad);
                    } else if (spec.flags & kZeroPad) {
                        out.Put(prefix, prefixLength);
                        out.Pad('0', pad + zeros);
                        out.Put(at, length);
                    } else {
                        out.Pad(' ', pad);
                        out.Put(prefix, prefixLength);
                        out.Pad('0', zeros);
                        out.Put(at, length);
                    }
                    total = (size_t)spec.width;
                } else {
                    out.Put(prefix, prefixLength);
                    out.Pad('0', zeros);
                    out.Put(at, length);
                }
                count += (int)total;
                break;
            }
            case 'p': {
                void* pointer = va_arg(*ap, void*);
                if (!pointer) {
                    EmitPadded(out, "(nil)", 5, spec, &count);
                    break;
                }
                const uintmax_t v = (uintmax_t)(uintptr_t)pointer;
                char digits[72];
                char* end = digits + sizeof digits;
                char* at = FormatHex(v, end, false);
                size_t length = (size_t)(end - at);
                size_t zeros = 0;
                if (spec.precision > 0 && (size_t)spec.precision > length)
                    zeros = (size_t)spec.precision - length;
                const size_t total = 2 + zeros + length;
                if (spec.width > 0 && (size_t)spec.width > total) {
                    const size_t pad = (size_t)spec.width - total;
                    if (spec.flags & kLeft) {
                        out.Put("0x", 2);
                        out.Pad('0', zeros);
                        out.Put(at, length);
                        out.Pad(' ', pad);
                    } else if (spec.flags & kZeroPad) {
                        out.Put("0x", 2);
                        out.Pad('0', pad + zeros);
                        out.Put(at, length);
                    } else {
                        out.Pad(' ', pad);
                        out.Put("0x", 2);
                        out.Pad('0', zeros);
                        out.Put(at, length);
                    }
                    count += spec.width;
                } else {
                    out.Put("0x", 2);
                    out.Pad('0', zeros);
                    out.Put(at, length);
                    count += (int)total;
                }
                break;
            }
            case 'c':
            case 'C': {
                const bool wideConversion = spec.conversion == 'C' || spec.length == kL;
                const int c = va_arg(*ap, int);
                if (!wideConversion) {
                    const char byte = (char)(unsigned char)c;
                    EmitPadded(out, &byte, 1, spec, &count);
                } else if (out.isWide) {
                    if (spec.width > 1) {
                        const size_t pad = (size_t)spec.width - 1;
                        if (spec.flags & kLeft) {
                            out.EmitWide((wchar_t)c);
                            out.Pad(' ', pad);
                        } else {
                            out.Pad(' ', pad);
                            out.EmitWide((wchar_t)c);
                        }
                        count += spec.width;
                    } else {
                        out.EmitWide((wchar_t)c);
                        ++count;
                    }
                } else {
                    char mb[8];
                    MbState state{};
                    const size_t n = wcrtomb(mb, (wchar_t)c, &state);
                    if (n == (size_t)-1) {
                        out.Fail(EILSEQ);
                        break;
                    }
                    EmitPadded(out, mb, n, spec, &count);
                }
                break;
            }
            case 's':
            case 'S': {
                const bool wideString = spec.conversion == 'S' || spec.length == kL;
                const void* pointer = va_arg(*ap, const void*);
                if (!wideString) {
                    const char* text = (const char*)pointer;
                    size_t length;
                    if (!text) {
                        // glibc prints "(null)" only when the precision can hold all of it.
                        if (spec.precision >= 0 && spec.precision < 5) {
                            text = "";
                            length = 0;
                        } else {
                            text = "(null)";
                            length = 6;
                        }
                    } else if (out.isWide) {
                        length = WideLengthOfBytes(text, spec.precision);
                        if (length == (size_t)-1) {
                            out.Fail(EILSEQ);
                            break;
                        }
                    } else {
                        length = spec.precision < 0 ? strlen(text)
                                                    : strnlen(text, (size_t)spec.precision);
                    }
                    if (spec.width > 0 && (size_t)spec.width > length) {
                        const size_t pad = (size_t)spec.width - length;
                        if (spec.flags & kLeft) {
                            if (out.isWide) EmitNarrowAsWide(out, text, spec.precision);
                            else out.Put(text, length);
                            out.Pad(' ', pad);
                        } else {
                            out.Pad(' ', pad);
                            if (out.isWide) EmitNarrowAsWide(out, text, spec.precision);
                            else out.Put(text, length);
                        }
                        count += spec.width;
                    } else {
                        if (out.isWide) EmitNarrowAsWide(out, text, spec.precision);
                        else out.Put(text, length);
                        count += (int)length;
                    }
                } else {
                    const wchar_t* text = (const wchar_t*)pointer;
                    size_t length;
                    if (!text) {
                        // A null wide string reads as "(null)" with the same precision rule.
                        if (spec.precision >= 0 && spec.precision < 5) {
                            text = L"";
                            length = 0;
                        } else {
                            text = L"(null)";
                            length = 6;
                        }
                    } else if (out.isWide) {
                        length = 0;
                        while (text[length] && (spec.precision < 0 || length < (size_t)spec.precision))
                            ++length;
                    } else {
                        length = NarrowLengthOfWide(text, spec.precision);
                        if (length == (size_t)-1) {
                            out.Fail(EILSEQ);
                            break;
                        }
                    }
                    if (spec.width > 0 && (size_t)spec.width > length) {
                        const size_t pad = (size_t)spec.width - length;
                        if (spec.flags & kLeft) {
                            if (out.isWide) EmitWideText(out, text, length);
                            else EmitWideAsNarrow(out, text, spec.precision);
                            out.Pad(' ', pad);
                        } else {
                            out.Pad(' ', pad);
                            if (out.isWide) EmitWideText(out, text, length);
                            else EmitWideAsNarrow(out, text, spec.precision);
                        }
                        count += spec.width;
                    } else {
                        if (out.isWide) EmitWideText(out, text, length);
                        else EmitWideAsNarrow(out, text, spec.precision);
                        count += (int)length;
                    }
                }
                break;
            }
            case 'n': {
                void* destination = va_arg(*ap, void*);
                if (destination) {
                    switch (spec.length) {
                        case kHH: *(signed char*)destination = (signed char)count; break;
                        case kH: *(short*)destination = (short)count; break;
                        case kL: *(long*)destination = count; break;
                        case kLL: *(long long*)destination = count; break;
                        case kZ: *(size_t*)destination = (size_t)count; break;
                        case kT: *(ptrdiff_t*)destination = count; break;
                        case kJ: *(intmax_t*)destination = count; break;
                        default: *(int*)destination = count; break;
                    }
                }
                break;
            }
            case 'm': {
                const char* text = strerror(errno);
                EmitPadded(out, text, strlen(text), spec, &count);
                break;
            }
            case 'f':
            case 'F':
            case 'e':
            case 'E':
            case 'g':
            case 'G':
            case 'a':
            case 'A': {
                const bool isLongDouble = spec.length == kBigL;
                int written;
                if ((spec.conversion | 32) == 'a') {
                    written = isLongDouble
                                  ? FormatHexFloat(out, va_arg(*ap, long double), spec.width,
                                                   spec.precision, spec.flags, spec.conversion)
                                  : FormatHexFloat(out, va_arg(*ap, double), spec.width,
                                                   spec.precision, spec.flags, spec.conversion);
                } else {
                    const long double value =
                        isLongDouble ? va_arg(*ap, long double) : (long double)va_arg(*ap, double);
                    written = FormatFloat(out, value, spec.width, spec.precision, spec.flags,
                                          spec.conversion, isLongDouble);
                }
                if (written < 0) {
                    out.Fail(EOVERFLOW);
                    break;
                }
                count += written;
                break;
            }
            case '%':
                out.Put("%", 1);
                ++count;
                break;
            default:
                out.Fail(EINVAL);
                break;
        }
        if (out.failed) break;
        if (count < 0) {
            out.Fail(EOVERFLOW);
            break;
        }
    }
    return out.failed ? -1 : count;
}

// A format that grows its buffer as needed, for the functions that hand the whole text over.
int FormatGrowable(Sink& out, const char* format, va_list* ap) {
    out.growable = true;
    out.capacity = 256;
    out.buffer = (char*)malloc(out.capacity);
    if (!out.buffer) {
        errno = ENOMEM;
        return -1;
    }
    const int result = FormatCore(out, format, ap);
    if (result < 0) {
        errno = out.error ? out.error : EINVAL;
        return -1;
    }
    out.Finish();
    return result;
}

int FormatNarrow(char* buffer, size_t capacity, const char* format, va_list* ap) {
    Sink out;
    out.buffer = buffer;
    out.capacity = capacity;  // 0 counts only, as snprintf with a null buffer does
    const int result = FormatCore(out, format, ap);
    if (!out.Finish()) return -1;
    return result;
}

int FormatWide(wchar_t* buffer, size_t capacity, const wchar_t* format, va_list* ap) {
    // The wide format string becomes multibyte first, so one core serves both widths. wcrtomb is
    // the locale's own conversion; the wide sink applies its inverse to the literal text.
    size_t narrowLength = 0, narrowCapacity = 64;
    char* narrow = (char*)malloc(narrowCapacity);
    if (!narrow) {
        errno = ENOMEM;
        return -1;
    }
    for (const wchar_t* at = format; *at; ++at) {
        char mb[8];
        MbState fresh{};
        const size_t n = wcrtomb(mb, *at, &fresh);
        if (n == (size_t)-1) {
            free(narrow);
            errno = EILSEQ;
            return -1;
        }
        if (narrowLength + n + 1 > narrowCapacity) {
            size_t grown = narrowCapacity * 2;
            while (grown < narrowLength + n + 1) grown *= 2;
            char* bigger = (char*)realloc(narrow, grown);
            if (!bigger) {
                free(narrow);
                errno = ENOMEM;
                return -1;
            }
            narrow = bigger;
            narrowCapacity = grown;
        }
        memcpy(narrow + narrowLength, mb, n);
        narrowLength += n;
    }
    narrow[narrowLength] = 0;

    Sink out;
    out.isWide = true;
    out.wideBuffer = buffer;
    out.wideCapacity = capacity;
    const int result = FormatCore(out, narrow, ap);
    free(narrow);
    if (!out.Finish()) return -1;
    // swprintf reports a buffer too small for the text (and its terminator) as failure, where
    // snprintf reports the length it would have needed.
    if (capacity && out.length >= capacity) return -1;
    if (!capacity) return -1;
    return result;
}

}  // namespace

#define CL extern "C" __attribute__((visibility("hidden")))

// The builtins GCC and libstdc++ know with default visibility are hidden again in assembly.
__asm__(".hidden printf");
__asm__(".hidden fprintf");
__asm__(".hidden sprintf");
__asm__(".hidden snprintf");
__asm__(".hidden vprintf");
__asm__(".hidden vfprintf");
__asm__(".hidden vsprintf");
__asm__(".hidden vsnprintf");
__asm__(".hidden asprintf");
__asm__(".hidden vasprintf");
__asm__(".hidden dprintf");
__asm__(".hidden vdprintf");
__asm__(".hidden swprintf");
__asm__(".hidden __fprintf_chk");
__asm__(".hidden __sprintf_chk");
__asm__(".hidden __snprintf_chk");

CL int vfprintf(FILE* stream, const char* format, va_list ap) {
    if (!stream || !format) {
        errno = EINVAL;
        return -1;
    }
    va_list copy;
    va_copy(copy, ap);
    Sink out;
    const int length = FormatGrowable(out, format, &copy);
    va_end(copy);
    if (length < 0) {
        free(out.buffer);
        return -1;
    }
    const size_t written = fwrite(out.buffer, 1, (size_t)length, stream);
    free(out.buffer);
    return written == (size_t)length ? length : -1;
}

CL int fprintf(FILE* stream, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    const int result = vfprintf(stream, format, ap);
    va_end(ap);
    return result;
}

CL int printf(const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    const int result = vfprintf(stdout, format, ap);
    va_end(ap);
    return result;
}

CL int vprintf(const char* format, va_list ap) { return vfprintf(stdout, format, ap); }

CL int __fprintf_chk(FILE* stream, int, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    const int result = vfprintf(stream, format, ap);
    va_end(ap);
    return result;
}

CL int __vfprintf_chk(FILE* stream, int, const char* format, va_list ap) {
    return vfprintf(stream, format, ap);
}

CL int __printf_chk(int, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    const int result = vfprintf(stdout, format, ap);
    va_end(ap);
    return result;
}

CL int __vprintf_chk(int, const char* format, va_list ap) { return vfprintf(stdout, format, ap); }

CL int vsnprintf(char* buffer, size_t capacity, const char* format, va_list ap) {
    if (!format) {
        errno = EINVAL;
        return -1;
    }
    va_list copy;
    va_copy(copy, ap);
    const int result = FormatNarrow(buffer, capacity, format, &copy);
    va_end(copy);
    return result;
}

CL int snprintf(char* buffer, size_t capacity, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    const int result = vsnprintf(buffer, capacity, format, ap);
    va_end(ap);
    return result;
}

CL int vsprintf(char* buffer, const char* format, va_list ap) {
    return vsnprintf(buffer, kUnlimited, format, ap);
}

CL int sprintf(char* buffer, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    const int result = vsnprintf(buffer, kUnlimited, format, ap);
    va_end(ap);
    return result;
}

CL int vasprintf(char** output, const char* format, va_list ap) {
    if (!output || !format) {
        errno = EINVAL;
        return -1;
    }
    va_list copy;
    va_copy(copy, ap);
    Sink out;
    const int length = FormatGrowable(out, format, &copy);
    va_end(copy);
    if (length < 0) {
        free(out.buffer);
        *output = nullptr;
        return -1;
    }
    *output = out.buffer;
    return length;
}

CL int asprintf(char** output, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    const int result = vasprintf(output, format, ap);
    va_end(ap);
    return result;
}

CL int vdprintf(int fd, const char* format, va_list ap) {
    if (!format) {
        errno = EINVAL;
        return -1;
    }
    va_list copy;
    va_copy(copy, ap);
    Sink out;
    const int length = FormatGrowable(out, format, &copy);
    va_end(copy);
    if (length < 0) {
        free(out.buffer);
        return -1;
    }
    size_t done = 0;
    while (done < (size_t)length) {
        const long wrote = write(fd, out.buffer + done, (size_t)length - done);
        if (wrote <= 0) {
            free(out.buffer);
            return -1;
        }
        done += (size_t)wrote;
    }
    free(out.buffer);
    return length;
}

CL int dprintf(int fd, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    const int result = vdprintf(fd, format, ap);
    va_end(ap);
    return result;
}

// The fortified forms _FORTIFY_SOURCE sends sprintf, snprintf and fprintf to. glibc aborts when the
// known size of the destination cannot hold the text, and these do the same.
CL int __sprintf_chk(char* buffer, int, size_t size, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    va_list copy;
    va_copy(copy, ap);
    Sink out;
    const int length = FormatGrowable(out, format, &copy);
    va_end(copy);
    va_end(ap);
    if (length < 0) {
        free(out.buffer);
        return -1;
    }
    if ((size_t)length >= size) {
        free(out.buffer);
        abort();
    }
    memcpy(buffer, out.buffer, (size_t)length + 1);
    free(out.buffer);
    return length;
}

CL int __snprintf_chk(char* buffer, size_t capacity, int, size_t size, const char* format, ...) {
    if (capacity > size) abort();
    va_list ap;
    va_start(ap, format);
    const int result = vsnprintf(buffer, capacity, format, ap);
    va_end(ap);
    return result;
}

CL int __vsnprintf_chk(char* buffer, size_t capacity, int, size_t size, const char* format,
                       va_list ap) {
    if (capacity > size) abort();
    return vsnprintf(buffer, capacity, format, ap);
}

CL int __vsprintf_chk(char* buffer, int, size_t size, const char* format, va_list ap) {
    va_list copy;
    va_copy(copy, ap);
    Sink out;
    const int length = FormatGrowable(out, format, &copy);
    va_end(copy);
    if (length < 0) {
        free(out.buffer);
        return -1;
    }
    if ((size_t)length >= size) {
        free(out.buffer);
        abort();
    }
    memcpy(buffer, out.buffer, (size_t)length + 1);
    free(out.buffer);
    return length;
}

// swprintf and vswprintf write wide characters into a wide buffer. The formatter works in narrow
// bytes (its format string converted through the locale) and converts the output as it goes, so a
// multibyte character cut at a precision or padding boundary can differ from glibc's in width; the
// bytes themselves agree, and the check compares ASCII, %ls and %lc output.
CL int vswprintf(wchar_t* buffer, size_t capacity, const wchar_t* format, va_list ap) {
    if (!format) {
        errno = EINVAL;
        return -1;
    }
    va_list copy;
    va_copy(copy, ap);
    const int result = FormatWide(buffer, capacity, format, &copy);
    va_end(copy);
    return result;
}

CL int swprintf(wchar_t* buffer, size_t capacity, const wchar_t* format, ...) {
    va_list ap;
    va_start(ap, format);
    const int result = vswprintf(buffer, capacity, format, ap);
    va_end(ap);
    return result;
}
