// cordinal's own math functions (stage 7 of replacing glibc on Linux): the libm calls the decoders,
// the DSP and the audio output make. Defined under glibc's names with hidden visibility, so cordinal
// and the static libstdc++ bind to these while the program exports nothing and glibc keeps its own
// copies for its own callers. No glibc header that declares these functions is included: the
// definitions are the declarations.
//
// Two kinds of function live here.
//  - The ones that are exact by construction are written directly from the bits: floor, ceil, trunc,
//    round, nearbyint, fabs, copysign, frexp, ldexp/scalbn, fmod, hypot, sqrt (the processor's own
//    sqrtsd/sqrtss, which is correctly rounded by definition) and the fenv rounding mode.
//  - The transcendental cores are musl's, whose math library is the old Sun/FreeBSD fdlibm brought
//    up to date, MIT licensed, read November 18, 2026. Each is musl-1.1.20/src/math/<file>.c, copied
//    into math/<file>.inc with only its include lines removed, and included here inside a namespace
//    of its own; their own copyright notices are at the top of every file. The tables (exp2's 256
//    entries, the 2/pi digits, pow's split logarithms) are musl's, unchanged, and they are why the
//    cores are ported rather than retyped.
//
// glibc reports domain and range errors through errno (math_errhandling is MATH_ERRNO here, and
// cordinal never changes it), so every entry point adds what glibc's own entry point adds, with
// glibc 2.44's exact rules, measured before writing them: a pole of the logarithm is ERANGE with
// -inf, a negative logarithm or square root is EDOM with the default NaN, exp/exp2/expf/exp2f and
// pow/powf call it ERANGE when the result overflows or underflows to zero (a denormal result is not
// an error), sin/cos/tan of an infinite argument is EDOM, and the rounding family, atan, atan2,
// hypot, cabs and sincos leave errno alone. sincos is glibc's exception: it does not report the
// infinite argument.
//
// The float functions are musl's own float cores rather than the double ones rounded down, so their
// tables and polynomials are the ones meant for single precision and their speed is the same.
#include <errno.h>
#include <float.h>
#include <stdint.h>

namespace {

constexpr uint64_t kSign64 = 0x8000000000000000ull;
constexpr uint64_t kExponent64 = 0x7FF0000000000000ull;
constexpr uint64_t kMantissa64 = 0x000FFFFFFFFFFFFFull;
constexpr uint32_t kSign32 = 0x80000000u;
constexpr uint32_t kExponent32 = 0x7F800000u;
constexpr uint32_t kMantissa32 = 0x007FFFFFu;

inline uint64_t Bits(double x) {
    uint64_t bits;
    __builtin_memcpy(&bits, &x, sizeof bits);
    return bits;
}
inline double Value(uint64_t bits) {
    double x;
    __builtin_memcpy(&x, &bits, sizeof x);
    return x;
}
inline uint32_t Bits(float x) {
    uint32_t bits;
    __builtin_memcpy(&bits, &x, sizeof bits);
    return bits;
}
inline float Value(uint32_t bits) {
    float x;
    __builtin_memcpy(&x, &bits, sizeof x);
    return x;
}
inline double CopySign(double x, double y) { return Value((Bits(x) & ~kSign64) | (Bits(y) & kSign64)); }
inline float CopySign(float x, float y) { return Value((Bits(x) & ~kSign32) | (Bits(y) & kSign32)); }
inline bool Finite(double x) { return (Bits(x) & kExponent64) != kExponent64; }
inline bool Infinite(double x) { return (Bits(x) & ~kSign64) == kExponent64; }
inline bool NotANumber(double x) { return x != x; }

// 2^n for |n| <= 1023, exact: the exponent field moved by n.
inline double PowerOfTwo(int n) { return Value((uint64_t)(0x3FF + n) << 52); }
inline float PowerOfTwoFloat(int n) { return Value((uint32_t)(0x7F + n) << 23); }

// glibc's answers for a domain error and for overflow, in the shapes it uses: the default NaN x86
// produces for 0/0 (which raises the invalid exception) and an infinity from a real multiplication
// (which raises overflow and inexact).
inline double DomainError() {
    volatile double zero = 0.0;
    return zero / zero;
}
inline double Overflow(double sign) {
    volatile double huge = 0x1p1023;
    return sign * (huge * huge);
}
inline float Overflow(float sign) {
    volatile float huge = 0x1p127f;
    return sign * (huge * huge);
}

}  // namespace

// ---- the parts of musl's libm.h that its math files use -----------------------------------------

typedef double double_t;
typedef float float_t;

#define asuint(f) Bits(static_cast<float>(f))
#define asfloat(i) Value(static_cast<uint32_t>(i))
#define asuint64(f) Bits(static_cast<double>(f))
#define asdouble(i) Value(static_cast<uint64_t>(i))

#define GET_HIGH_WORD(hi, d) ((hi) = (uint32_t)(Bits(d) >> 32))
#define GET_LOW_WORD(lo, d) ((lo) = (uint32_t)Bits(d))
#define INSERT_WORDS(d, hi, lo) \
    ((d) = Value((uint64_t)(((uint64_t)(uint32_t)(hi) << 32) | (uint32_t)(lo))))
#define SET_HIGH_WORD(d, hi) INSERT_WORDS(d, hi, (uint32_t)Bits(d))
#define SET_LOW_WORD(d, lo) INSERT_WORDS(d, Bits(d) >> 32, lo)
#define EXTRACT_WORDS(hi, lo, d)                 \
    do {                                         \
        const uint64_t bits_ = Bits(d);          \
        (hi) = (uint32_t)(bits_ >> 32);          \
        (lo) = (uint32_t)bits_;                  \
    } while (0)
#define GET_FLOAT_WORD(w, d) ((w) = Bits(static_cast<float>(d)))
#define SET_FLOAT_WORD(d, w) ((d) = Value(static_cast<uint32_t>(w)))

// Evaluating an expression for its floating-point side effects only (the inexact and underflow
// exceptions musl raises deliberately), with the value unable to be folded away.
inline void ForceEval(float x) {
    volatile float y = x;
    (void)y;
}
inline void ForceEval(double x) {
    volatile double y = x;
    (void)y;
}
inline void ForceEval(long double x) {
    volatile long double y = x;
    (void)y;
}
#define FORCE_EVAL(x)                                                 \
    (sizeof(x) == sizeof(float)    ? ForceEval(static_cast<float>(x)) \
     : sizeof(x) == sizeof(double) ? ForceEval(static_cast<double>(x)) \
                                   : ForceEval(static_cast<long double>(x)))
#define eval_as_double(x) (static_cast<double>(x))
#define eval_as_float(x) (static_cast<float>(x))
#define predict_true(x) __builtin_expect(!!(x), 1)
#define predict_false(x) __builtin_expect(!!(x), 0)
// musl's math.h knows this one; cordinal's math.cpp deliberately includes no libm header.
#define isnan(x) __builtin_isnan(x)

#define CL extern "C" __attribute__((visibility("hidden")))

// ---- exact bit operations ----------------------------------------------------------------------

CL double fabs(double x) { return Value(Bits(x) & ~kSign64); }
CL float fabsf(float x) { return Value(Bits(x) & ~kSign32); }
CL long double fabsl(long double x) { return __builtin_fabsl(x); }
CL double copysign(double x, double y) { return CopySign(x, y); }
CL float copysignf(float x, float y) { return CopySign(x, y); }
CL long double copysignl(long double x, long double y) { return __builtin_copysignl(x, y); }

// Scaling by a power of two. The exponent field is moved directly, so a result in the normal range
// cannot round; a result below it is one exact multiply by 2^-1022 followed by one multiply that
// rounds (and that is the only rounding, which is what keeps subnormals identical to glibc's); an
// exponent past the end returns zero or an infinity. The exponent is capped where every finite
// argument has already overflowed or underflowed, so no caller pays for an absurd one step at a
// time.
CL double scalbn(double x, int n) {
    if (n > 2100) n = 2100;    // every finite argument has overflowed or underflowed by now
    if (n < -2200) n = -2200;
    uint64_t bits = Bits(x);
    int exponent = (int)((bits >> 52) & 0x7FF);
    if (exponent == 0x7FF || x == 0) return x;
    if (exponent == 0) {  // subnormal: normalise it first, which is exact
        x *= 0x1p54;
        n -= 54;
        bits = Bits(x);
        exponent = (int)((bits >> 52) & 0x7FF);
    }
    const int scaled = exponent + n;
    if (scaled >= 0x7FF) return CopySign(Overflow(1.0), x);
    if (scaled > 0) return Value((uint64_t)((bits & ~kExponent64) | ((uint64_t)scaled << 52)));
    if (scaled <= -53) return CopySign(0.0, x);  // below half the smallest denormal
    const double significand = Value((uint64_t)((bits & ~kExponent64) | 0x3FF0000000000000ull));
    return (significand * 0x1p-1022) * PowerOfTwo(scaled - 1);
}
CL float scalbnf(float x, int n) {
    if (n > 300) n = 300;
    if (n < -300) n = -300;
    uint32_t bits = Bits(x);
    int exponent = (int)((bits >> 23) & 0xFF);
    if (exponent == 0xFF || x == 0) return x;
    if (exponent == 0) {
        x *= 0x1p32f;
        n -= 32;
        bits = Bits(x);
        exponent = (int)((bits >> 23) & 0xFF);
    }
    const int scaled = exponent + n;
    if (scaled >= 0xFF) return CopySign(Overflow(1.0f), x);
    if (scaled > 0) return Value((bits & ~kExponent32) | ((uint32_t)scaled << 23));
    if (scaled <= -24) return CopySign(0.0f, x);
    const float significand = Value((bits & ~kExponent32) | 0x3F800000u);
    return (significand * 0x1p-126f) * PowerOfTwoFloat(scaled - 1);
}
// ldexp is scalbn with the range error glibc reports: overflow, or underflow all the way to zero.
CL double ldexp(double x, int n) {
    const double r = scalbn(x, n);
    if (x != 0 && Finite(x) && (r == 0 || Infinite(r))) errno = ERANGE;
    return r;
}
CL float ldexpf(float x, int n) {
    const float r = scalbnf(x, n);
    if (x != 0 && (Bits(x) & kExponent32) != kExponent32 && (r == 0 || (Bits(r) & kExponent32) == kExponent32))
        errno = ERANGE;
    return r;
}

// The exponent x was scaled by, with the significand moved into [0.5, 1).
CL double frexp(double x, int* exponent) {
    const int e = (int)((Bits(x) >> 52) & 0x7FF);
    if (e == 0) {
        if (x == 0) {
            *exponent = 0;
            return x;
        }
        const double scaled = frexp(x * 0x1p64, exponent);
        *exponent -= 64;
        return scaled;
    }
    if (e == 0x7FF) return x;  // glibc leaves the exponent alone for inf and NaN
    *exponent = e - 0x3FE;
    return Value((uint64_t)((Bits(x) & 0x800FFFFFFFFFFFFFull) | 0x3FE0000000000000ull));
}
CL float frexpf(float x, int* exponent) {
    const int e = (int)((Bits(x) >> 23) & 0xFF);
    if (e == 0) {
        if (x == 0) {
            *exponent = 0;
            return x;
        }
        const float scaled = frexpf(x * 0x1p32f, exponent);
        *exponent -= 32;
        return scaled;
    }
    if (e == 0xFF) return x;
    *exponent = e - 0x7E;
    return Value((Bits(x) & 0x807FFFFFu) | 0x3F000000u);
}

// Rounding to integral values, clearing or adding the bits below the point. |x| >= 2^52 has no
// fraction to drop, which also covers infinities and NaNs.
CL double trunc(double x) {
    const uint64_t bits = Bits(x);
    const int exponent = (int)((bits >> 52) & 0x7FF) - 0x3FF;
    if (exponent >= 52) return x;
    if (exponent < 0) return x == 0 ? x : CopySign(0.0, x);
    return Value((uint64_t)(bits & ~((1ull << (52 - exponent)) - 1)));
}
CL double floor(double x) {
    const uint64_t bits = Bits(x);
    const int exponent = (int)((bits >> 52) & 0x7FF) - 0x3FF;
    if (exponent >= 52) return x;
    if (exponent < 0) return x == 0 ? x : (bits & kSign64 ? -1.0 : 0.0);
    const uint64_t unit = 1ull << (52 - exponent);
    if ((bits & (unit - 1)) == 0) return x;
    const uint64_t whole = bits & ~(unit - 1);
    return Value(bits & kSign64 ? whole + unit : whole);
}
CL double ceil(double x) { return -floor(-x); }

CL float truncf(float x) {
    const uint32_t bits = Bits(x);
    const int exponent = (int)((bits >> 23) & 0xFF) - 0x7F;
    if (exponent >= 23) return x;
    if (exponent < 0) return x == 0 ? x : CopySign(0.0f, x);
    return Value(bits & ~((1u << (23 - exponent)) - 1));
}
CL float floorf(float x) {
    const uint32_t bits = Bits(x);
    const int exponent = (int)((bits >> 23) & 0xFF) - 0x7F;
    if (exponent >= 23) return x;
    if (exponent < 0) return x == 0 ? x : (bits & kSign32 ? -1.0f : 0.0f);
    const uint32_t unit = 1u << (23 - exponent);
    if ((bits & (unit - 1)) == 0) return x;
    const uint32_t whole = bits & ~(unit - 1);
    return Value(bits & kSign32 ? whole + unit : whole);
}
CL float ceilf(float x) { return -floorf(-x); }

// Ties away from zero, the C round.
CL double round(double x) {
    const int exponent = (int)((Bits(x) >> 52) & 0x7FF) - 0x3FF;
    if (exponent >= 52) return x;
    const double whole = trunc(x), fraction = x - whole;
    double result = whole;
    if (fraction >= 0.5) result += 1.0;
    else if (fraction <= -0.5) result -= 1.0;
    return result == 0 ? CopySign(0.0, x) : result;
}
CL float roundf(float x) {
    const int exponent = (int)((Bits(x) >> 23) & 0xFF) - 0x7F;
    if (exponent >= 23) return x;
    const float whole = truncf(x), fraction = x - whole;
    float result = whole;
    if (fraction >= 0.5f) result += 1.0f;
    else if (fraction <= -0.5f) result -= 1.0f;
    return result == 0 ? CopySign(0.0f, x) : result;
}
// roundl rounds the 80-bit value: above 2^63 every value is an integer already, and below it the
// long long cast truncates exactly.
CL long double roundl(long double x) {
    if (!(fabsl(x) < 0x1p63L)) return x;
    const long double whole = (long double)(long long)x, fraction = x - whole;
    long double result = whole;
    if (fraction >= 0.5L) result += 1.0L;
    else if (fraction <= -0.5L) result -= 1.0L;
    return result == 0 ? copysignl(0.0L, x) : result;
}

// Rounding to the current mode. The four modes are the bits fesetround writes; nearest, ties to
// even is one add and subtract of 1.5 * 2^52 for |x| < 2^51, the processor's conversion for the
// bucket above it, and the other modes are the exact floor, ceil and trunc of the value.
CL int fegetround(void) {
    unsigned short control;
    __asm__ volatile("fnstcw %0" : "=m"(control));
    return control & 0xC00;
}
CL int fesetround(int mode) {
    if (mode != 0 && mode != 0x400 && mode != 0x800 && mode != 0xC00) {
        errno = EINVAL;
        return -1;
    }
    unsigned short control;
    __asm__ volatile("fnstcw %0" : "=m"(control));
    control = (unsigned short)((control & ~0xC00u) | (unsigned)mode);
    __asm__ volatile("fldcw %0" : : "m"(control));
    unsigned int mxcsr;
    __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
    mxcsr = (mxcsr & ~0x6000u) | (((unsigned)mode >> 10) << 13);
    __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));
    return 0;
}
CL double nearbyint(double x) {
    unsigned short control;
    __asm__ volatile("fnstcw %0" : "=m"(control));
    const int mode = control & 0xC00;
    if (mode == 0x400) return floor(x);
    if (mode == 0x800) return ceil(x);
    if (mode == 0xC00) return trunc(x);
    if (fabs(x) < 0x1p51) {
        const double result = (x + 0x1.8p52) - 0x1.8p52;
        return result == 0 ? CopySign(0.0, x) : result;
    }
    if (!(fabs(x) < 0x1p52)) return x;
    // Between 2^51 and 2^52 the 1.5 * 2^52 sum reaches the range where one ulp is 2, so it cannot
    // round ties to even; the processor's own conversion rounds these exactly instead.
    long long whole;
    __asm__("cvtsd2si %1, %0" : "=r"(whole) : "x"(x));
    return (double)whole;
}
CL float nearbyintf(float x) {
    unsigned short control;
    __asm__ volatile("fnstcw %0" : "=m"(control));
    const int mode = control & 0xC00;
    if (mode == 0x400) return floorf(x);
    if (mode == 0x800) return ceilf(x);
    if (mode == 0xC00) return truncf(x);
    if (fabsf(x) < 0x1p22f) {
        const float result = (x + 0x1.8p23f) - 0x1.8p23f;
        return result == 0 ? CopySign(0.0f, x) : result;
    }
    if (!(fabsf(x) < 0x1p23f)) return x;
    int whole;
    __asm__("cvtss2si %1, %0" : "=r"(whole) : "x"(x));
    return (float)whole;
}

// The integer roundings give up at the long long cast, which is what glibc does too: an out of
// range value stores LONG_MIN (or LLONG_MIN) with the invalid exception and leaves errno alone.
CL long lrint(double x) { return (long)nearbyint(x); }
CL long lround(double x) { return (long)round(x); }
CL long long llround(double x) { return (long long)round(x); }
CL long lroundf(float x) { return (long)roundf(x); }

// The square roots are the processor's: correctly rounded by definition, and the negative argument
// produces the same default NaN (with the invalid exception) that glibc reports as EDOM.
CL double sqrt(double x) {
    double result;
    __asm__("sqrtsd %1, %0" : "=x"(result) : "x"(x));
    if (x < 0) errno = EDOM;
    return result;
}
CL float sqrtf(float x) {
    float result;
    __asm__("sqrtss %1, %0" : "=x"(result) : "x"(x));
    if (x < 0) errno = EDOM;
    return result;
}

// fmod is musl's bit algorithm: the exact remainder of the significands, so nothing rounds. glibc
// reports a zero divisor and a non-finite dividend as a domain error, and a NaN argument quietly.
CL double fmod(double x, double y) {
    uint64_t uxi = Bits(x), uy = Bits(y);
    const int sx = (int)(uxi >> 63);
    int ex = (int)((uxi >> 52) & 0x7FF), ey = (int)((uy >> 52) & 0x7FF);
    const bool xNan = ex == 0x7FF && (uxi & kMantissa64), yNan = ey == 0x7FF && (uy & kMantissa64);
    if ((uy << 1) == 0 || yNan || ex == 0x7FF) {
        if (!xNan && !yNan) errno = EDOM;
        return DomainError();
    }
    if ((uxi << 1) <= (uy << 1)) {
        if ((uxi << 1) == (uy << 1)) return CopySign(0.0, x);
        return x;
    }
    uint64_t i;
    if (ex == 0) {
        for (i = uxi << 12; i >> 63 == 0; ex--, i <<= 1);
        uxi <<= -ex + 1;
    } else {
        uxi &= -1ull >> 12;
        uxi |= 1ull << 52;
    }
    if (ey == 0) {
        for (i = uy << 12; i >> 63 == 0; ey--, i <<= 1);
        uy <<= -ey + 1;
    } else {
        uy &= -1ull >> 12;
        uy |= 1ull << 52;
    }
    for (; ex > ey; ex--) {
        i = uxi - uy;
        if (i >> 63 == 0) {
            if (i == 0) return CopySign(0.0, x);
            uxi = i;
        }
        uxi <<= 1;
    }
    i = uxi - uy;
    if (i >> 63 == 0) {
        if (i == 0) return CopySign(0.0, x);
        uxi = i;
    }
    for (; uxi >> 52 == 0; uxi <<= 1, ex--);
    if (ex > 0) {
        uxi -= 1ull << 52;
        uxi |= (uint64_t)ex << 52;
    } else {
        uxi >>= -ex + 1;
    }
    uxi |= (uint64_t)sx << 63;
    return Value(uxi);
}

// hypot and hypotf are musl's: the larger argument squared exactly as a sum of a high and a low
// part (Dekker's split), so the square root below sees an argument that did not round, and neither
// part overflows or underflows. cabs is hypot of the two halves, which is what glibc's is.
namespace {
// SPLIT: 2^27 + 1 splits a double into two 26-bit halves, which is exact here.
constexpr double kSplit = 0x1p27 + 1;

void SquareParts(double* high, double* low, double x) {
    const double scaled = x * kSplit;
    const double xh = x - scaled + scaled, xl = x - xh;
    *high = x * x;
    *low = xh * xh - *high + 2 * xh * xl + xl * xl;
}
}  // namespace

CL double hypot(double x, double y) {
    uint64_t ux = Bits(x) & ~kSign64, uy = Bits(y) & ~kSign64;
    if (ux < uy) {
        const uint64_t swap = ux;
        ux = uy;
        uy = swap;
    }
    x = Value(ux);
    y = Value(uy);
    const int ex = (int)(ux >> 52), ey = (int)(uy >> 52);
    if (ey == 0x7FF) return y;         // hypot(inf, nan) is inf, as glibc has it
    if (ex == 0x7FF || uy == 0) return x;
    if (ex - ey > 64) return x + y;    // the small one cannot change the answer
    double scale = 1;
    if (ex > 0x3FF + 510) {
        scale = 0x1p700;
        x *= 0x1p-700;
        y *= 0x1p-700;
    } else if (ey < 0x3FF - 450) {
        scale = 0x1p-700;
        x *= 0x1p700;
        y *= 0x1p700;
    }
    double highX = 0, lowX = 0, highY = 0, lowY = 0;
    SquareParts(&highX, &lowX, x);
    SquareParts(&highY, &lowY, y);
    const double result = scale * sqrt(lowY + lowX + highY + highX);
    if (Infinite(result) && Finite(x) && Finite(y)) errno = ERANGE;  // glibc's overflow
    return result;
}

CL float hypotf(float x, float y) {
    uint32_t ux = Bits(x) & ~kSign32, uy = Bits(y) & ~kSign32;
    if (ux < uy) {
        const uint32_t swap = ux;
        ux = uy;
        uy = swap;
    }
    x = Value(ux);
    y = Value(uy);
    if (uy == 0xFFu << 23) return y;
    if (ux >= 0xFFu << 23 || uy == 0 || ux - uy >= 25u << 23) return x + y;
    float scale = 1;
    if (ux >= (0x7F + 60) << 23) {
        scale = 0x1p90f;
        x *= 0x1p-90f;
        y *= 0x1p-90f;
    } else if (uy < (0x7F - 60) << 23) {
        scale = 0x1p-90f;
        x *= 0x1p90f;
        y *= 0x1p90f;
    }
    const float result = scale * sqrtf((double)x * x + (double)y * y);
    if ((Bits(result) & kExponent32) == kExponent32 && (Bits(result) & kMantissa32) == 0 &&
        Finite(x) && Finite(y))
        errno = ERANGE;
    return result;
}

CL double cabs(__complex__ double z) { return hypot(__builtin_creal(z), __builtin_cimag(z)); }
CL float cabsf(__complex__ float z) { return hypotf(__builtin_crealf(z), __builtin_cimagf(z)); }

// ---- the transcendental cores, ported from musl-1.1.20/src/math ---------------------------------

namespace cord_exp {
namespace {
#include "math/exp.inc"
}
}  // namespace cord_exp
CL double exp(double x) {
    const double result = cord_exp::exp(x);
    if (Finite(x) && (result == 0 || Infinite(result))) errno = ERANGE;
    return result;
}

namespace cord_exp2 {
namespace {
#include "math/exp2.inc"
}
}  // namespace cord_exp2
CL double exp2(double x) {
    const double result = cord_exp2::exp2(x);
    if (Finite(x) && (result == 0 || Infinite(result))) errno = ERANGE;
    return result;
}

namespace cord_expf {
namespace {
#include "math/expf.inc"
}
}  // namespace cord_expf
CL float expf(float x) {
    const float result = cord_expf::expf(x);
    if (Finite(x) && (result == 0 || (Bits(result) & kExponent32) == kExponent32)) errno = ERANGE;
    return result;
}

namespace cord_exp2f {
namespace {
#include "math/exp2f.inc"
}
}  // namespace cord_exp2f
CL float exp2f(float x) {
    const float result = cord_exp2f::exp2f(x);
    if (Finite(x) && (result == 0 || (Bits(result) & kExponent32) == kExponent32)) errno = ERANGE;
    return result;
}

namespace cord_log {
namespace {
#include "math/log.inc"
}
}  // namespace cord_log
CL double log(double x) {
    if (x == 0) {
        errno = ERANGE;  // a pole: glibc's divide-by-zero is ERANGE here
        return -__builtin_inf();
    }
    if (x < 0) {
        errno = EDOM;
        return DomainError();
    }
    return cord_log::log(x);
}

namespace cord_log2 {
namespace {
#include "math/log2.inc"
}
}  // namespace cord_log2
CL double log2(double x) {
    if (x == 0) {
        errno = ERANGE;
        return -__builtin_inf();
    }
    if (x < 0) {
        errno = EDOM;
        return DomainError();
    }
    return cord_log2::log2(x);
}

namespace cord_log10 {
namespace {
#include "math/log10.inc"
}
}  // namespace cord_log10
CL double log10(double x) {
    if (x == 0) {
        errno = ERANGE;
        return -__builtin_inf();
    }
    if (x < 0) {
        errno = EDOM;
        return DomainError();
    }
    return cord_log10::log10(x);
}

namespace cord_log10f {
namespace {
#include "math/log10f.inc"
}
}  // namespace cord_log10f
CL float log10f(float x) {
    if (x == 0) {
        errno = ERANGE;
        return -__builtin_inff();
    }
    if (x < 0) {
        errno = EDOM;
        return DomainError();
    }
    return cord_log10f::log10f(x);
}

namespace cord_log1pf {
namespace {
#include "math/log1pf.inc"
}
}  // namespace cord_log1pf
CL float log1pf(float x) {
    if (x == -1) {
        errno = ERANGE;
        return -__builtin_inff();
    }
    if (x < -1) {
        errno = EDOM;
        return DomainError();
    }
    return cord_log1pf::log1pf(x);
}

namespace cord_pow {
namespace {
#include "math/pow.inc"
}
}  // namespace cord_pow
CL double pow(double x, double y) {
    const double result = cord_pow::pow(x, y);
    if (Finite(x) && Finite(y)) {
        if (result == 0 && x != 0) errno = ERANGE;      // underflow all the way to zero
        else if (Infinite(result)) errno = ERANGE;      // overflow
        else if (NotANumber(result)) errno = EDOM;      // a negative base and a fractional exponent
    }
    return result;
}

namespace cord_powf {
namespace {
#include "math/powf.inc"
}
}  // namespace cord_powf
CL float powf(float x, float y) {
    const float result = cord_powf::powf(x, y);
    if (Finite(x) && Finite(y)) {
        if (result == 0 && x != 0) errno = ERANGE;
        else if ((Bits(result) & kExponent32) == kExponent32 && (Bits(result) & kMantissa32) == 0) errno = ERANGE;
        else if (result != result) errno = EDOM;
    }
    return result;
}

namespace cord_trig {
namespace {
int __rem_pio2_large(double*, double*, int, int, int);
int __rem_pio2(double, double*);
double __sin(double, double, int);
double __cos(double, double);
double __tan(double, double, int);
#include "math/__sin.inc"
#include "math/__cos.inc"
#include "math/__tan.inc"
#include "math/__rem_pio2.inc"
#include "math/__rem_pio2_large.inc"
#include "math/sin.inc"
#include "math/cos.inc"
#include "math/tan.inc"
#include "math/sincos.inc"
}  // namespace
}  // namespace cord_trig
CL double sin(double x) {
    if (Infinite(x)) {
        errno = EDOM;
        return x - x;
    }
    return cord_trig::sin(x);
}
CL double cos(double x) {
    if (Infinite(x)) {
        errno = EDOM;
        return x - x;
    }
    return cord_trig::cos(x);
}
CL double tan(double x) {
    if (Infinite(x)) {
        errno = EDOM;
        return x - x;
    }
    return cord_trig::tan(x);
}
// glibc's sincos reports its infinite argument as EDOM, as its sin and cos do.
CL void sincos(double x, double* sine, double* cosine) {
    cord_trig::sincos(x, sine, cosine);
    if (Infinite(x)) errno = EDOM;
}

namespace cord_trigf {
namespace {
int __rem_pio2f(float, double*);
float __sindf(double);
float __cosdf(double);
#include "math/pio2f.inc"
#include "math/__sindf.inc"
#include "math/__cosdf.inc"
#include "math/__rem_pio2f.inc"
#include "math/sinf.inc"
#include "math/cosf.inc"
#include "math/sincosf.inc"
}  // namespace
}  // namespace cord_trigf
CL float sinf(float x) {
    if ((Bits(x) & kExponent32) == kExponent32 && (Bits(x) & kMantissa32) == 0) {
        errno = EDOM;
        return x - x;
    }
    return cord_trigf::sinf(x);
}
CL float cosf(float x) {
    if ((Bits(x) & kExponent32) == kExponent32 && (Bits(x) & kMantissa32) == 0) {
        errno = EDOM;
        return x - x;
    }
    return cord_trigf::cosf(x);
}
CL void sincosf(float x, float* sine, float* cosine) {
    cord_trigf::sincosf(x, sine, cosine);
    if ((Bits(x) & kExponent32) == kExponent32 && (Bits(x) & kMantissa32) == 0) errno = EDOM;
}

namespace cord_atan {
namespace {
#include "math/atan.inc"
#include "math/atan2.inc"
}  // namespace
}  // namespace cord_atan
CL double atan(double x) { return cord_atan::atan(x); }
CL double atan2(double y, double x) { return cord_atan::atan2(y, x); }

namespace cord_atanf {
namespace {
#include "math/atanf.inc"
#include "math/atan2f.inc"
}  // namespace
}  // namespace cord_atanf
CL float atanf(float x) { return cord_atanf::atanf(x); }
CL float atan2f(float y, float x) { return cord_atanf::atan2f(y, x); }

// cosh is written here rather than ported: musl's needs expm1, which nothing else in cordinal calls.
// Below 1 the series is summed in long double, where the value is close to 1 and the exponential
// form would lose most of its digits; above it the two exponentials add rather than cancel, and
// above 700, where e^x itself overflows before cosh does, the result is built from e^(x/2) squared.
CL double cosh(double x) {
    const double a = fabs(x);
    if (a <= 1) {
        const long double s = (long double)a * a;
        const long double series =
            1 + s * (0.5L + s * (1.0L / 24 + s * (1.0L / 720 + s * (1.0L / 40320 +
                s * (1.0L / 3628800 + s * (1.0L / 479001600 + s * (1.0L / 87178291200 +
                s * (1.0L / 20922789888000 + s * (1.0L / 6402373705728000 +
                s * (1.0L / 2432902008176640000))))))))));
        return (double)series;
    }
    double result;
    if (a > 700) {
        const double half = exp(a * 0.5);
        result = (0.5 * half) * half;
    } else {
        result = 0.5 * (exp(a) + exp(-a));
    }
    if (Infinite(result) && Finite(x)) errno = ERANGE;
    return result;
}
