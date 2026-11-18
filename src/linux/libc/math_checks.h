// cordinal's math functions (math.cpp) against glibc's own, for checks.h: the operations that are
// exact by definition must agree bit for bit, the transcendental cores are measured in ulps of
// glibc's result, every edge case glibc answers (domain and range errors, signed zeros, infinities,
// NaN, denormals) is compared with its errno, and the arguments the decoders and the DSP really use
// (audio samples, EQ frequencies, ReplayGain gains, filter coefficients) get their own pass.
// MathBench() times the hot ones against glibc's.
#pragma once
#include <dlfcn.h>
#include <errno.h>
#include <fenv.h>
#include <float.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

#include <cstdio>

// The C library's math declarations are written here rather than taken from <math.h>: in C++ that
// header pulls libstdc++'s float and long double overloads into the global namespace, which makes
// o.exp (the function this file compares with glibc's) ambiguous.
extern "C" {
double exp(double);
double exp2(double);
double log(double);
double log2(double);
double log10(double);
double pow(double, double);
double sin(double);
double cos(double);
double tan(double);
void sincos(double, double*, double*);
double atan(double);
double atan2(double, double);
double cosh(double);
double sqrt(double);
double fmod(double, double);
double hypot(double, double);
double cabs(__complex__ double);
double floor(double);
double ceil(double);
double trunc(double);
double round(double);
double nearbyint(double);
double fabs(double);
double frexp(double, int*);
double ldexp(double, int);
long lrint(double);
long lround(double);
long long llround(double);
float expf(float);
float exp2f(float);
float log10f(float);
float log1pf(float);
float powf(float, float);
float sinf(float);
float cosf(float);
void sincosf(float, float*, float*);
float atan2f(float, float);
float hypotf(float, float);
float cabsf(__complex__ float);
float floorf(float);
float roundf(float);
float nearbyintf(float);
float fabsf(float);
float sqrtf(float);
float ldexpf(float, int);
long lroundf(float);
long double roundl(long double);
}

namespace libc_checks {

// ---- the values, as bits -----------------------------------------------------------------------

inline uint64_t MathBits(double x) {
    uint64_t bits;
    memcpy(&bits, &x, sizeof bits);
    return bits;
}
inline double MathValue(uint64_t bits) {
    double x;
    memcpy(&x, &bits, sizeof x);
    return x;
}
inline uint32_t MathBits(float x) {
    uint32_t bits;
    memcpy(&bits, &x, sizeof bits);
    return bits;
}
inline float MathValue(uint32_t bits) {
    float x;
    memcpy(&x, &bits, sizeof x);
    return x;
}
inline bool MathIsNan(double x) { return x != x; }

// The distance from glibc's result in ulps of that result, with an absolute floor of one ulp of 1.0.
// The floor is what a result that cancelled towards zero needs: sin(pi) is 1.2e-16, where one ulp of
// the result is 2.5e-32 and no library, glibc's included, lands within a few thousand of them.
inline double DoubleUlps(double ours, double theirs) {
    if (ours == theirs) return 0;
    if (MathIsNan(ours) || MathIsNan(theirs)) return MathIsNan(ours) && MathIsNan(theirs) ? 0 : 1e300;
    const double magnitude = fabs(theirs);
    if (magnitude > DBL_MAX) return 1e300;
    double ulp = MathValue(MathBits(magnitude) + 1) - magnitude;
    if (ulp < 0x1p-52) ulp = 0x1p-52;
    return fabs(ours - theirs) / ulp;
}
inline double FloatUlps(float ours, float theirs) {
    if (ours == theirs) return 0;
    if (MathIsNan(ours) || MathIsNan(theirs)) return MathIsNan(ours) && MathIsNan(theirs) ? 0 : 1e300;
    const float magnitude = fabsf(theirs);
    if (magnitude > FLT_MAX) return 1e300;
    float ulp = MathValue(MathBits(magnitude) + 1) - magnitude;
    if (ulp < 0x1p-23f) ulp = 0x1p-23f;
    return fabs(ours - theirs) / ulp;
}

// One function's tally: the worst error, how many cases were exactly glibc's answer, and whether
// any case went over the budget.
struct MathTally {
    const char* name;
    double budget;
    double worst = 0;
    double worstArgument = 0;
    long long cases = 0;
    long long exact = 0;
    long long over = 0;
    long long badErrno = 0;
    long long allowed = 0;  // errno differences the notes explain (the float subnormal range)
    int shown = 0;
    void Record(double error, double argument) {
        ++cases;
        if (error == 0) ++exact;
        if (error > worst) {
            worst = error;
            worstArgument = argument;
        }
        if (error > budget + 1e-9) ++over;
    }
    void Errno(const char* what, double argument, int ours, int theirs) {
        ++badErrno;
        if (shown++ < 3) {
            printf("%s(%g): %s errno %d, glibc errno %d\n", name, argument, what, ours, theirs);
        }
    }
};

// ---- glibc's copies ----------------------------------------------------------------------------

// dlsym reaches glibc's functions because cordinal's are hidden, so nothing here compares a function
// with itself.
struct GlibcMath {
    double (*exp)(double);
    double (*exp2)(double);
    double (*log)(double);
    double (*log2)(double);
    double (*log10)(double);
    float (*log10f)(float);
    float (*log1pf)(float);
    double (*pow)(double, double);
    float (*powf)(float, float);
    float (*expf)(float);
    float (*exp2f)(float);
    double (*sin)(double);
    double (*cos)(double);
    double (*tan)(double);
    void (*sincos)(double, double*, double*);
    float (*sinf)(float);
    float (*cosf)(float);
    void (*sincosf)(float, float*, float*);
    double (*atan)(double);
    double (*atan2)(double, double);
    float (*atan2f)(float, float);
    double (*cosh)(double);
    double (*sqrt)(double);
    float (*sqrtf)(float);
    double (*fmod)(double, double);
    float (*hypotf)(float, float);
    double (*cabs)(double _Complex);
    float (*cabsf)(float _Complex);
    double (*floor)(double);
    double (*ceil)(double);
    double (*trunc)(double);
    double (*round)(double);
    double (*nearbyint)(double);
    float (*floorf)(float);
    float (*roundf)(float);
    float (*nearbyintf)(float);
    double (*fabs)(double);
    float (*fabsf)(float);
    long double (*roundl)(long double);
    long (*lrint)(double);
    long (*lround)(double);
    long long (*llround)(double);
    long (*lroundf)(float);
    double (*frexp)(double, int*);
    double (*ldexp)(double, int);
    float (*ldexpf)(float, int);
    int (*fegetround)(void);
    int (*fesetround)(int);
};

inline GlibcMath LoadGlibcMath() {
    GlibcMath g{};
    g.exp = Glibc("exp", &exp);
    g.exp2 = Glibc("exp2", &exp2);
    g.log = Glibc("log", &log);
    g.log2 = Glibc("log2", &log2);
    g.log10 = Glibc("log10", &log10);
    g.log10f = Glibc("log10f", &log10f);
    g.log1pf = Glibc("log1pf", &log1pf);
    g.pow = Glibc("pow", &pow);
    g.powf = Glibc("powf", &powf);
    g.expf = Glibc("expf", &expf);
    g.exp2f = Glibc("exp2f", &exp2f);
    g.sin = Glibc("sin", &sin);
    g.cos = Glibc("cos", &cos);
    g.tan = Glibc("tan", &tan);
    g.sincos = Glibc("sincos", (void (*)(double, double*, double*))nullptr);
    g.sinf = Glibc("sinf", &sinf);
    g.cosf = Glibc("cosf", &cosf);
    g.sincosf = Glibc("sincosf", (void (*)(float, float*, float*))nullptr);
    g.atan = Glibc("atan", &atan);
    g.atan2 = Glibc("atan2", &atan2);
    g.atan2f = Glibc("atan2f", &atan2f);
    g.cosh = Glibc("cosh", &cosh);
    g.sqrt = Glibc("sqrt", &sqrt);
    g.sqrtf = Glibc("sqrtf", &sqrtf);
    g.fmod = Glibc("fmod", &fmod);
    g.hypotf = Glibc("hypotf", &hypotf);
    g.cabs = Glibc("cabs", (double (*)(double _Complex))nullptr);
    g.cabsf = Glibc("cabsf", (float (*)(float _Complex))nullptr);
    g.floor = Glibc("floor", &floor);
    g.ceil = Glibc("ceil", &ceil);
    g.trunc = Glibc("trunc", &trunc);
    g.round = Glibc("round", &round);
    g.nearbyint = Glibc("nearbyint", &nearbyint);
    g.floorf = Glibc("floorf", &floorf);
    g.roundf = Glibc("roundf", &roundf);
    g.nearbyintf = Glibc("nearbyintf", &nearbyintf);
    g.fabs = Glibc("fabs", &fabs);
    g.fabsf = Glibc("fabsf", &fabsf);
    g.roundl = Glibc("roundl", &roundl);
    g.lrint = Glibc("lrint", &lrint);
    g.lround = Glibc("lround", &lround);
    g.llround = Glibc("llround", &llround);
    g.lroundf = Glibc("lroundf", &lroundf);
    g.frexp = Glibc("frexp", &frexp);
    g.ldexp = Glibc("ldexp", &ldexp);
    g.ldexpf = Glibc("ldexpf", &ldexpf);
    g.fegetround = Glibc("fegetround", &fegetround);
    g.fesetround = Glibc("fesetround", &fesetround);
    return g;
}

// cordinal's own, through function pointers as well: a direct call from this file could be folded
// into the compiler's builtin for the same function (sqrt into sqrtsd, exp of a constant into its
// value), and then the case would not be testing math.cpp at all.
inline GlibcMath LoadOurMath() {
    GlibcMath o{};
    o.exp = static_cast<double (*)(double)>(&exp);
    o.exp2 = static_cast<double (*)(double)>(&exp2);
    o.log = static_cast<double (*)(double)>(&log);
    o.log2 = static_cast<double (*)(double)>(&log2);
    o.log10 = static_cast<double (*)(double)>(&log10);
    o.log10f = static_cast<float (*)(float)>(&log10f);
    o.log1pf = static_cast<float (*)(float)>(&log1pf);
    o.pow = static_cast<double (*)(double, double)>(&pow);
    o.powf = static_cast<float (*)(float, float)>(&powf);
    o.expf = static_cast<float (*)(float)>(&expf);
    o.exp2f = static_cast<float (*)(float)>(&exp2f);
    o.sin = static_cast<double (*)(double)>(&sin);
    o.cos = static_cast<double (*)(double)>(&cos);
    o.tan = static_cast<double (*)(double)>(&tan);
    o.sincos = static_cast<void (*)(double, double*, double*)>(&sincos);
    o.sinf = static_cast<float (*)(float)>(&sinf);
    o.cosf = static_cast<float (*)(float)>(&cosf);
    o.sincosf = static_cast<void (*)(float, float*, float*)>(&sincosf);
    o.atan = static_cast<double (*)(double)>(&atan);
    o.atan2 = static_cast<double (*)(double, double)>(&atan2);
    o.atan2f = static_cast<float (*)(float, float)>(&atan2f);
    o.cosh = static_cast<double (*)(double)>(&cosh);
    o.sqrt = static_cast<double (*)(double)>(&sqrt);
    o.sqrtf = static_cast<float (*)(float)>(&sqrtf);
    o.fmod = static_cast<double (*)(double, double)>(&fmod);
    o.hypotf = static_cast<float (*)(float, float)>(&hypotf);
    o.cabs = static_cast<double (*)(double _Complex)>(&cabs);
    o.cabsf = static_cast<float (*)(float _Complex)>(&cabsf);
    o.floor = static_cast<double (*)(double)>(&floor);
    o.ceil = static_cast<double (*)(double)>(&ceil);
    o.trunc = static_cast<double (*)(double)>(&trunc);
    o.round = static_cast<double (*)(double)>(&round);
    o.nearbyint = static_cast<double (*)(double)>(&nearbyint);
    o.floorf = static_cast<float (*)(float)>(&floorf);
    o.roundf = static_cast<float (*)(float)>(&roundf);
    o.nearbyintf = static_cast<float (*)(float)>(&nearbyintf);
    o.fabs = static_cast<double (*)(double)>(&fabs);
    o.fabsf = static_cast<float (*)(float)>(&fabsf);
    o.roundl = static_cast<long double (*)(long double)>(&roundl);
    o.lrint = static_cast<long (*)(double)>(&lrint);
    o.lround = static_cast<long (*)(double)>(&lround);
    o.llround = static_cast<long long (*)(double)>(&llround);
    o.lroundf = static_cast<long (*)(float)>(&lroundf);
    o.frexp = static_cast<double (*)(double, int*)>(&frexp);
    o.ldexp = static_cast<double (*)(double, int)>(&ldexp);
    o.ldexpf = static_cast<float (*)(float, int)>(&ldexpf);
    o.fegetround = static_cast<int (*)(void)>(&fegetround);
    o.fesetround = static_cast<int (*)(int)>(&fesetround);
    return o;
}

// ---- the arguments -----------------------------------------------------------------------------

// Four shapes of argument, all from one fixed seed so a failure reproduces, plus the edge values
// every libm has to answer: signed zeros, denormals, the largest and smallest finite values, the
// infinities and NaN.
constexpr int kMathCases = 20000;

struct MathCaseSet {
    double wide[kMathCases];      // both signs, every exponent
    double small[kMathCases];     // [-16, 16], where the trig functions are used
    double positive[kMathCases];  // > 0, every exponent: the logarithms and the square root
    double audio[kMathCases];     // [-1, 1]: samples, gains and everything made from them
    double pairX[kMathCases], pairY[kMathCases];
    float wideF[kMathCases], smallF[kMathCases], positiveF[kMathCases], audioF[kMathCases];
    float pairXF[kMathCases], pairYF[kMathCases];

    static double Uniform(Random& r, double low, double high) {
        return low + (high - low) * (double)(r.Next() >> 11) * 0x1p-53;
    }
    // A value with a random sign and a random exponent, so every binade is reached.
    static double Spread(Random& r) {
        const uint64_t bits = (r.Next() & 0x800FFFFFFFFFFFFFull) | ((r.Next() % 0x7FE) << 52);
        return MathValue(bits);
    }
    static float SpreadF(Random& r) {
        const uint32_t bits = (uint32_t)((r.Next() & 0x807FFFFFu) | ((r.Next() % 0xFE) << 23));
        return MathValue(bits);
    }
    // One in twelve cases is an edge rather than an ordinary value.
    static double Edge(Random& r) {
        switch (r.Next() % 14) {
            case 0: return 0.0;
            case 1: return -0.0;
            case 2: return 0x1p-1074;
            case 3: return -0x1p-1074;
            case 4: return DBL_MAX;
            case 5: return -DBL_MAX;
            case 6: return __builtin_inf();
            case 7: return -__builtin_inf();
            case 8: return __builtin_nan("");
            case 9: return 1.0;
            case 10: return -1.0;
            case 11: return 0.5;
            case 12: return -0.5;
            default: return 2.0;
        }
    }
    static float EdgeF(Random& r) {
        switch (r.Next() % 14) {
            case 0: return 0.0f;
            case 1: return -0.0f;
            case 2: return 0x1p-149f;
            case 3: return -0x1p-149f;
            case 4: return FLT_MAX;
            case 5: return -FLT_MAX;
            case 6: return __builtin_inff();
            case 7: return -__builtin_inff();
            case 8: return __builtin_nanf("");
            case 9: return 1.0f;
            case 10: return -1.0f;
            case 11: return 0.5f;
            case 12: return -0.5f;
            default: return 2.0f;
        }
    }

    MathCaseSet() {
        Random r{0x243F6A8885A308D3ull};
        for (int i = 0; i < kMathCases; ++i) {
            const int kind = (int)(r.Next() % 12);
            wide[i] = kind == 0 ? Edge(r) : Spread(r);
            small[i] = kind == 0 ? Edge(r) : (kind < 3 ? Spread(r) : Uniform(r, -16, 16));
            positive[i] = kind == 0 ? Edge(r) : (kind < 3 ? fabs(Spread(r)) : fabs(Uniform(r, -16, 16)));
            audio[i] = kind == 0 ? Edge(r) : Uniform(r, -1, 1);
            pairX[i] = kind == 0 ? Edge(r) : (kind < 6 ? fabs(Spread(r)) : Uniform(r, 0.001, 1000));
            pairY[i] = kind == 0 ? Edge(r) : (kind < 6 ? Spread(r) : Uniform(r, -300, 300));
            const int kindF = (int)(r.Next() % 12);
            wideF[i] = kindF == 0 ? EdgeF(r) : SpreadF(r);
            smallF[i] = kindF == 0 ? EdgeF(r) : (kindF < 3 ? SpreadF(r) : (float)Uniform(r, -16, 16));
            positiveF[i] = kindF == 0 ? EdgeF(r) : (float)fabs(kindF < 3 ? (double)SpreadF(r) : Uniform(r, -16, 16));
            audioF[i] = kindF == 0 ? EdgeF(r) : (float)Uniform(r, -1, 1);
            pairXF[i] = kindF == 0 ? EdgeF(r) : (float)(kindF < 6 ? fabs((double)SpreadF(r)) : Uniform(r, 0.001, 1000));
            pairYF[i] = kindF == 0 ? EdgeF(r) : (float)(kindF < 6 ? (double)SpreadF(r) : Uniform(r, -300, 300));
        }
    }
};

// ---- comparing one function at a time ----------------------------------------------------------

// glibc's float exponentials report ERANGE for a result that underflows into the subnormal range,
// where its double ones report it only for a result that becomes zero; cordinal follows the double
// rule everywhere. The difference is only in this corner, so the checks allow it and say so.
inline bool SubnormalUnderflow(const float* result, int ourErrno, int theirErrno) {
    return ourErrno == 0 && theirErrno == ERANGE && fabsf(*result) < FLT_MIN && *result != 0;
}

// GCC recognises the C library's math functions by name, and its model of them says a call cannot
// change errno. That assumption follows the name into cordinal's own definition, so without a
// barrier the reload of errno after the call is optimised away and every errno comparison would
// quietly pass.
inline void MathBarrier() { __asm__ volatile("" ::: "memory"); }

struct MathComparer {
    void Unary(MathTally& t, const double* args, int count, double (*ours)(double), double (*theirs)(double)) {
        for (int i = 0; i < count; ++i) {
            const double x = args[i];
            errno = 0;
            const double expected = theirs(x);
            MathBarrier();
            const int expectedErrno = errno;
            errno = 0;
            const double actual = ours(x);
            MathBarrier();
            const int actualErrno = errno;
            t.Record(DoubleUlps(actual, expected), x);
            if (actualErrno != expectedErrno) t.Errno("value", x, actualErrno, expectedErrno);
        }
    }
    void UnaryF(MathTally& t, const float* args, int count, float (*ours)(float), float (*theirs)(float)) {
        for (int i = 0; i < count; ++i) {
            const float x = args[i];
            errno = 0;
            const float expected = theirs(x);
            MathBarrier();
            const int expectedErrno = errno;
            errno = 0;
            const float actual = ours(x);
            MathBarrier();
            const int actualErrno = errno;
            t.Record(FloatUlps(actual, expected), x);
            if (actualErrno != expectedErrno) {
                if (SubnormalUnderflow(&actual, actualErrno, expectedErrno)) ++t.allowed;
                else t.Errno("value", x, actualErrno, expectedErrno);
            }
        }
    }
    void Pairs(MathTally& t, const double* xs, const double* ys, int count, double (*ours)(double, double),
               double (*theirs)(double, double), bool checkErrno = true) {
        for (int i = 0; i < count; ++i) {
            const double x = xs[i], y = ys[i];
            errno = 0;
            const double expected = theirs(x, y);
            MathBarrier();
            const int expectedErrno = errno;
            errno = 0;
            const double actual = ours(x, y);
            MathBarrier();
            const int actualErrno = errno;
            t.Record(DoubleUlps(actual, expected), x);
            if (checkErrno && actualErrno != expectedErrno) {
                ++t.badErrno;
                if (t.shown++ < 3) {
                    printf("%s(%g, %g): errno %d, glibc errno %d\n", t.name, x, y, actualErrno, expectedErrno);
                }
            }
        }
    }
    void PairsF(MathTally& t, const float* xs, const float* ys, int count, float (*ours)(float, float),
                float (*theirs)(float, float), bool checkErrno = true) {
        for (int i = 0; i < count; ++i) {
            const float x = xs[i], y = ys[i];
            errno = 0;
            const float expected = theirs(x, y);
            MathBarrier();
            const int expectedErrno = errno;
            errno = 0;
            const float actual = ours(x, y);
            MathBarrier();
            const int actualErrno = errno;
            t.Record(FloatUlps(actual, expected), x);
            if (checkErrno && actualErrno != expectedErrno) {
                if (SubnormalUnderflow(&actual, actualErrno, expectedErrno)) {
                    ++t.allowed;
                } else {
                    ++t.badErrno;
                    if (t.shown++ < 3) {
                        printf("%s(%g, %g): errno %d, glibc errno %d\n", t.name, (double)x, (double)y, actualErrno,
                               expectedErrno);
                    }
                }
            }
        }
    }
};

inline void PrintMathTallies(const MathTally* tallies, int count) {
    printf("%-10s %8s %7s %8s %7s %6s\n", "function", "worst", "budget", "cases", "exact", "wrong");
    for (int i = 0; i < count; ++i) {
        const MathTally& t = tallies[i];
        printf("%-10s %7.2f%s %7.2f %8lld %6lld%% %5lld%s\n", t.name, t.worst, t.worst > t.budget + 1e-9 ? "!" : " ",
               t.budget, t.cases, t.cases ? 100 * t.exact / t.cases : 0, t.over + t.badErrno,
               t.badErrno ? " errno" : "");
    }
}

// ---- the check ---------------------------------------------------------------------------------

// A NaN's sign and payload are not part of its value; every other result must be the same bits.
inline bool SameValue(double ours, double theirs) {
    if (MathIsNan(ours) && MathIsNan(theirs)) return true;
    return MathBits(ours) == MathBits(theirs);
}
inline bool SameValue(float ours, float theirs) {
    if (MathIsNan(ours) && MathIsNan(theirs)) return true;
    return MathBits(ours) == MathBits(theirs);
}

inline void CheckMath() {
    static MathCaseSet cases;
    GlibcMath g = LoadGlibcMath();
    GlibcMath o = LoadOurMath();
    MathComparer compare;
    int exactMismatches = 0;
    auto exact = [&](const char* name, auto ours, auto theirs, const auto* args, int count) {
        int mismatches = 0;
        for (int i = 0; i < count; ++i) {
            errno = 0;
            const auto expected = theirs(args[i]);
            const int expectedErrno = errno;
            errno = 0;
            const auto actual = ours(args[i]);
            if (!SameValue(actual, expected) || errno != expectedErrno) {
                if (++mismatches <= 3) {
                    printf("%s(%g): cordinal %.17g errno %d, glibc %.17g errno %d\n", name, (double)args[i],
                           (double)actual, errno, (double)expected, expectedErrno);
                }
            }
        }
        exactMismatches += mismatches;
        Expect(mismatches == 0, name);
    };

    // The functions that are exact by construction, bit for bit over every binade and every edge.
    exact("floor", o.floor, g.floor, cases.wide, kMathCases);
    exact("ceil", o.ceil, g.ceil, cases.wide, kMathCases);
    exact("trunc", o.trunc, g.trunc, cases.wide, kMathCases);
    exact("round", o.round, g.round, cases.wide, kMathCases);
    exact("nearbyint", o.nearbyint, g.nearbyint, cases.wide, kMathCases);
    exact("floorf", o.floorf, g.floorf, cases.wideF, kMathCases);
    exact("roundf", o.roundf, g.roundf, cases.wideF, kMathCases);
    exact("fabs", o.fabs, g.fabs, cases.wide, kMathCases);
    exact("fabsf", o.fabsf, g.fabsf, cases.wideF, kMathCases);
    exact("sqrt", o.sqrt, g.sqrt, cases.wide, kMathCases);
    exact("sqrtf", o.sqrtf, g.sqrtf, cases.wideF, kMathCases);

    // fmod takes two arguments, in both orders: exact, so the bits must be equal.
    {
        int mismatches = 0;
        for (int i = 0; i < kMathCases; ++i) {
            for (int order = 0; order < 2; ++order) {
                const double x = order ? cases.pairY[i] : cases.pairX[i];
                const double y = order ? cases.pairX[i] : cases.pairY[i];
                errno = 0;
                const double expected = g.fmod(x, y);
                MathBarrier();
                const int expectedErrno = errno;
                errno = 0;
                const double actual = o.fmod(x, y);
                MathBarrier();
                if (!SameValue(actual, expected) || errno != expectedErrno) {
                    if (++mismatches <= 3) {
                        printf("fmod(%g, %g): cordinal %.17g errno %d, glibc %.17g errno %d\n", x, y, actual,
                               errno, expected, expectedErrno);
                    }
                }
            }
        }
        exactMismatches += mismatches;
        Expect(mismatches == 0, "fmod, in both orders");
    }

    // roundl, lrint, lround, llround and lroundf, value and result together.
    {
        int mismatches = 0;
        for (int i = 0; i < kMathCases; ++i) {
            const double x = cases.wide[i];
            const long double ourLong = o.roundl(x), theirLong = g.roundl(x);
            if (!(ourLong == theirLong || (ourLong != ourLong && theirLong != theirLong))) {
                if (++mismatches <= 3) printf("roundl(%g): cordinal %La, glibc %La\n", x, ourLong, theirLong);
            }
            errno = 0;
            const long ourRound = o.lround(x), theirRound = g.lround(x);
            MathBarrier();
            if (ourRound != theirRound || errno != 0) {
                if (++mismatches <= 3) printf("lround(%g): cordinal %ld/%d, glibc %ld\n", x, ourRound, errno, theirRound);
            }
            const long ourNear = o.lrint(x), theirNear = g.lrint(x);
            if (ourNear != theirNear) {
                if (++mismatches <= 3) printf("lrint(%g): cordinal %ld, glibc %ld\n", x, ourNear, theirNear);
            }
            const long long ourLongLong = o.llround(x), theirLongLong = g.llround(x);
            if (ourLongLong != theirLongLong) {
                if (++mismatches <= 3) printf("llround(%g): cordinal %lld, glibc %lld\n", x, ourLongLong, theirLongLong);
            }
            const float f = cases.wideF[i];
            const long ourRoundF = o.lroundf(f), theirRoundF = g.lroundf(f);
            if (ourRoundF != theirRoundF) {
                if (++mismatches <= 3) printf("lroundf(%g): cordinal %ld, glibc %ld\n", (double)f, ourRoundF, theirRoundF);
            }
        }
        exactMismatches += mismatches;
        Expect(mismatches == 0, "roundl, lrint and the lround family");
    }

    // frexp and ldexp, including the subnormal scaling and the range errors.
    {
        int mismatches = 0;
        for (int i = 0; i < kMathCases; ++i) {
            int ourExponent = 0, theirExponent = 0;
            const double x = cases.wide[i];
            const double ours = o.frexp(x, &ourExponent), theirs = g.frexp(x, &theirExponent);
            if (!SameValue(ours, theirs)) {
                if (++mismatches <= 3) printf("frexp(%g): cordinal %.17g, glibc %.17g\n", x, ours, theirs);
            }
            if (o.fabs(x) > 0 && o.fabs(x) <= DBL_MAX && ourExponent != theirExponent) {
                if (++mismatches <= 3) printf("frexp(%g): exponent %d, glibc %d\n", x, ourExponent, theirExponent);
            }
            const double raw = cases.pairY[i];
            const int n = raw != raw ? 0 : (int)(raw < -4000 ? -4000 : raw > 4000 ? 4000 : raw);
            errno = 0;
            const double scaled = g.ldexp(x, n);
            const int expectedErrno = errno;
            errno = 0;
            const double ourScaled = o.ldexp(x, n);
            MathBarrier();
            if (!SameValue(ourScaled, scaled) || errno != expectedErrno) {
                if (++mismatches <= 3) {
                    printf("ldexp(%g, %d): cordinal %.17g errno %d, glibc %.17g errno %d\n", x, n, ourScaled, errno,
                           scaled, expectedErrno);
                }
            }
            errno = 0;
            const float scaledF = g.ldexpf(cases.wideF[i], n);
            const int expectedErrnoF = errno;
            errno = 0;
            const float ourScaledF = o.ldexpf(cases.wideF[i], n);
            MathBarrier();
            if (!SameValue(ourScaledF, scaledF) || errno != expectedErrnoF) {
                if (++mismatches <= 3) {
                    printf("ldexpf(%g, %d): cordinal %.9g errno %d, glibc %.9g errno %d\n", (double)cases.wideF[i], n,
                           (double)ourScaledF, errno, (double)scaledF, expectedErrnoF);
                }
            }
        }
        exactMismatches += mismatches;
        Expect(mismatches == 0, "frexp, ldexp and ldexpf");
    }

    // The transcendentals: worst ulps from glibc's result over 20,000 arguments each, and no errno
    // difference on any of them.
    MathTally tExp{"exp", 1.0}, tExp2{"exp2", 1.0}, tExpf{"expf", 1.0}, tExp2f{"exp2f", 1.0};
    MathTally tLog{"log", 1.0}, tLog2{"log2", 1.0}, tLog10{"log10", 1.0}, tLog10f{"log10f", 1.0};
    MathTally tLog1pf{"log1pf", 1.0}, tPow{"pow", 1.0}, tPowf{"powf", 1.0};
    MathTally tSin{"sin", 1.0}, tCos{"cos", 1.0}, tTan{"tan", 1.0};
    MathTally tSinf{"sinf", 1.0}, tCosf{"cosf", 1.0}, tSincos{"sincos", 1.0}, tSincosf{"sincosf", 1.0};
    MathTally tAtan{"atan", 1.0}, tAtan2{"atan2", 1.0}, tAtan2f{"atan2f", 1.0};
    MathTally tCosh{"cosh", 2.0}, tHypotf{"hypotf", 1.0}, tCabs{"cabs", 1.0};
    MathTally tSqrt{"sqrt", 0.0}, tSqrtf{"sqrtf", 0.0};

    compare.Unary(tExp, cases.small, kMathCases, o.exp, g.exp);
    compare.Unary(tExp2, cases.small, kMathCases, o.exp2, g.exp2);
    compare.UnaryF(tExpf, cases.smallF, kMathCases, o.expf, g.expf);
    compare.UnaryF(tExp2f, cases.smallF, kMathCases, o.exp2f, g.exp2f);
    compare.Unary(tLog, cases.positive, kMathCases, o.log, g.log);
    compare.Unary(tLog2, cases.positive, kMathCases, o.log2, g.log2);
    compare.Unary(tLog10, cases.positive, kMathCases, o.log10, g.log10);
    compare.UnaryF(tLog10f, cases.positiveF, kMathCases, o.log10f, g.log10f);
    compare.UnaryF(tLog1pf, cases.positiveF, kMathCases, o.log1pf, g.log1pf);
    compare.Pairs(tPow, cases.pairX, cases.pairY, kMathCases, o.pow, g.pow);
    compare.PairsF(tPowf, cases.pairXF, cases.pairYF, kMathCases, o.powf, g.powf);
    compare.Unary(tSin, cases.small, kMathCases, o.sin, g.sin);
    compare.Unary(tCos, cases.small, kMathCases, o.cos, g.cos);
    compare.Unary(tTan, cases.small, kMathCases, o.tan, g.tan);
    compare.UnaryF(tSinf, cases.smallF, kMathCases, o.sinf, g.sinf);
    compare.UnaryF(tCosf, cases.smallF, kMathCases, o.cosf, g.cosf);
    compare.Unary(tAtan, cases.wide, kMathCases, o.atan, g.atan);
    compare.Pairs(tAtan2, cases.pairY, cases.pairX, kMathCases, o.atan2, g.atan2, false);
    compare.PairsF(tAtan2f, cases.pairYF, cases.pairXF, kMathCases, o.atan2f, g.atan2f, false);
    compare.Unary(tCosh, cases.small, kMathCases, o.cosh, g.cosh);
    compare.PairsF(tHypotf, cases.pairXF, cases.pairYF, kMathCases, o.hypotf, g.hypotf);

    // sincos and sincosf give two results each.
    for (int i = 0; i < kMathCases; ++i) {
        const double x = cases.small[i];
        double ourSine = 0, ourCosine = 0, theirSine = 0, theirCosine = 0;
        errno = 0;
        g.sincos(x, &theirSine, &theirCosine);
        MathBarrier();
        const int expectedErrno = errno;
        errno = 0;
        o.sincos(x, &ourSine, &ourCosine);
        MathBarrier();
        const int actualErrno = errno;
        const double error = DoubleUlps(ourSine, theirSine) + DoubleUlps(ourCosine, theirCosine);
        tSincos.Record(error, x);
        if (actualErrno != expectedErrno) {
            ++tSincos.badErrno;
            if (tSincos.shown++ < 3) {
                printf("sincos(%g): errno %d, glibc errno %d\n", x, actualErrno, expectedErrno);
            }
        }
        const float f = cases.smallF[i];
        float ourSineF = 0, ourCosineF = 0, theirSineF = 0, theirCosineF = 0;
        errno = 0;
        g.sincosf(f, &theirSineF, &theirCosineF);
        MathBarrier();
        const int expectedErrnoF = errno;
        errno = 0;
        o.sincosf(f, &ourSineF, &ourCosineF);
        MathBarrier();
        const int actualErrnoF = errno;
        tSincosf.Record(FloatUlps(ourSineF, theirSineF) + FloatUlps(ourCosineF, theirCosineF), f);
        if (actualErrnoF != expectedErrnoF) {
            ++tSincosf.badErrno;
            if (tSincosf.shown++ < 3) {
                printf("sincosf(%g): errno %d, glibc errno %d\n", (double)f, actualErrnoF, expectedErrnoF);
            }
        }
    }

    // cabs and cabsf, over the point pairs and the quarter circles.
    auto complexOf = [](double real, double imaginary) {
        __complex__ double z;
        __real__ z = real;
        __imag__ z = imaginary;
        return z;
    };
    auto complexOfF = [](float real, float imaginary) {
        __complex__ float z;
        __real__ z = real;
        __imag__ z = imaginary;
        return z;
    };
    for (int i = 0; i < kMathCases; ++i) {
        const double x = cases.pairX[i], y = cases.pairY[i];
        errno = 0;
        const double expected = g.cabs(complexOf(x, y));
        MathBarrier();
        const int expectedErrno = errno;
        errno = 0;
        const double actual = o.cabs(complexOf(x, y));
        MathBarrier();
        const int actualErrno = errno;
        tCabs.Record(DoubleUlps(actual, expected), x);
        if (actualErrno != expectedErrno) {
            ++tCabs.badErrno;
            if (tCabs.shown++ < 3) printf("cabs(%g, %g): errno %d, glibc errno %d\n", x, y, actualErrno, expectedErrno);
        }
        const float xf = cases.pairXF[i], yf = cases.pairYF[i];
        errno = 0;
        const float expectedF = g.cabsf(complexOfF(xf, yf));
        MathBarrier();
        const int expectedErrnoF = errno;
        errno = 0;
        const float actualF = o.cabsf(complexOfF(xf, yf));
        MathBarrier();
        const int actualErrnoF = errno;
        tCabs.Record(FloatUlps(actualF, expectedF), xf);
        if (actualErrnoF != expectedErrnoF) {
            ++tCabs.badErrno;
            if (tCabs.shown++ < 3) printf("cabsf(%g, %g): errno %d, glibc errno %d\n", (double)xf, (double)yf, actualErrnoF, expectedErrnoF);
        }
    }

    // ---- the edge answers: value bits and errno, function by function --------------------------
    {
        int wrong = 0;
        auto one = [&](MathTally& t, auto ours, auto theirs, double x, bool checkErrno = true) {
            errno = 0;
            const double expected = theirs(x);
            MathBarrier();
            const int expectedErrno = errno;
            errno = 0;
            const double actual = ours(x);
            MathBarrier();
            const int actualErrno = errno;
            const double error = DoubleUlps(actual, expected);
            t.Record(error, x);
            if (checkErrno && actualErrno != expectedErrno) {
                ++t.badErrno;
                if (++wrong <= 6) {
                    printf("%s(%g): cordinal %.17g errno %d, glibc %.17g errno %d\n", t.name, x, actual,
                           actualErrno, expected, expectedErrno);
                }
            }
        };
        auto oneF = [&](MathTally& t, auto ours, auto theirs, float x) {
            errno = 0;
            const float expected = theirs(x);
            MathBarrier();
            const int expectedErrno = errno;
            errno = 0;
            const float actual = ours(x);
            MathBarrier();
            const int actualErrno = errno;
            t.Record(FloatUlps(actual, expected), x);
            if (actualErrno != expectedErrno) {
                if (SubnormalUnderflow(&actual, actualErrno, expectedErrno)) {
                    ++t.allowed;
                } else {
                    ++t.badErrno;
                    if (++wrong <= 6) {
                        printf("%s(%g): cordinal %.9g errno %d, glibc %.9g errno %d\n", t.name, (double)x,
                               (double)actual, actualErrno, (double)expected, expectedErrno);
                    }
                }
            }
        };
        static const double kEdges[] = {0.0,   -0.0,  1.0,       -1.0,       0.5,      -0.5,     2.0,
                                        -2.0,  1e-30, -1e-30,    1e300,      -1e300,  DBL_MAX,  -DBL_MAX,
                                        1e-310, 1e-320, 0x1p-1074, 709.78,  709.79,  -708.39,  -708.40,
                                        710.0, -745.13, -745.14, -1074.0,    -1024.0, -1100.0,  1024.0,
                                        88.72, -103.97, 3.141592653589793, 6.283185307179586,
                                        1.5707963267948966, 1e-8,  -1e-8,   100.0,   1e6,      -1e6};
        for (double x : kEdges) {
            one(tExp, o.exp, g.exp, x);
            one(tExp2, o.exp2, g.exp2, x);
            one(tLog, o.log, g.log, x);
            one(tLog2, o.log2, g.log2, x);
            one(tLog10, o.log10, g.log10, x);
            one(tSin, o.sin, g.sin, x);
            one(tCos, o.cos, g.cos, x);
            one(tTan, o.tan, g.tan, x);
            one(tAtan, o.atan, g.atan, x);
            one(tCosh, o.cosh, g.cosh, x);
            one(tSqrt, o.sqrt, g.sqrt, x);
        }
        static const float kEdgesF[] = {0.0f,    -0.0f,  1.0f,   -1.0f,  0.5f,  -0.5f, 2.0f,
                                        -2.0f,   1e-30f, -1e-30f, 3e38f, -3e38f, 1e-40f,
                                        88.7f,   -103.9f, -104.0f, 89.0f,  128.0f, -128.0f,
                                        3.14159265f, 1e-8f, -1e-8f, 1e6f,  -1e6f};
        for (float x : kEdgesF) {
            oneF(tExpf, o.expf, g.expf, x);
            oneF(tExp2f, o.exp2f, g.exp2f, x);
            oneF(tLog10f, o.log10f, g.log10f, x);
            oneF(tLog1pf, o.log1pf, g.log1pf, x);
            oneF(tSinf, o.sinf, g.sinf, x);
            oneF(tCosf, o.cosf, g.cosf, x);
            oneF(tSqrtf, o.sqrtf, g.sqrtf, x);
        }
        // pow's own corners: zero and negative bases, integer and fractional exponents, huge ones.
        struct PowCase {
            double x, y;
        };
        static const PowCase kPow[] = {
            {0, 0},    {0, -1},  {-0.0, -1}, {-0.0, 3}, {0, 1},    {-2, 3},   {-2, 2.5}, {-2, -3},
            {2, 1e300}, {2, -1e300}, {10, 400}, {10, -400}, {1, 1e300}, {-1, 1e300}, {-1, __builtin_inf()},
            {1, __builtin_nan("")}, {__builtin_nan(""), 0}, {__builtin_nan(""), 2}, {__builtin_inf(), -1},
            {-__builtin_inf(), 3}, {-__builtin_inf(), 2}, {-__builtin_inf(), -3},
            {0.5, -1074}, {0.5, -1075}, {4, -0.5}, {1e-300, 2}, {-1e-300, 2}, {2, 0.5}, {0, 1e-300},
            {-0.0, -2}, {-2, 2}, {-2, 1e-300}, {1.0000000000000002, 1e300}, {0.9999999999999999, 1e300},
        };
        for (const PowCase& c : kPow) {
            one(tPow, [&](double) { return o.pow(c.x, c.y); }, [&](double) { return g.pow(c.x, c.y); }, c.x);
            const float expectedF = g.powf((float)c.x, (float)c.y);
            const float actualF = o.powf((float)c.x, (float)c.y);
            tPowf.Record(FloatUlps(actualF, expectedF), c.x);
        }
        static const double kAtan2X[] = {0, -0.0, 0, -0.0, __builtin_inf(), -__builtin_inf(), __builtin_inf(),
                                         -__builtin_inf(), 1, 0, __builtin_nan(""), 1, 1e300, -1, 1, 0.0};
        static const double kAtan2Y[] = {0, 0, -0.0, -0.0, __builtin_inf(), __builtin_inf(), -__builtin_inf(),
                                         -__builtin_inf(), 0, 1, 1, __builtin_nan(""), 1e-300, 0.0, -0.0, -1};
        for (int i = 0; i < (int)(sizeof kAtan2X / sizeof *kAtan2X); ++i) {
            one(tAtan2, [&](double x) { return o.atan2(kAtan2Y[i], x); },
                [&](double x) { return g.atan2(kAtan2Y[i], x); }, kAtan2X[i], false);
        }
        Expect(wrong == 0, "the edge answers and their errno match glibc's");
    }

    // ---- what the decoders and the DSP actually ask --------------------------------------------
    {
        // Audio samples at the output scale, and the sample conversions built on nearbyint and
        // lround: 16 and 24 bit, with dither, as audio_output.cpp does it.
        int conversions = 0;
        for (int i = 0; i < kMathCases; ++i) {
            const double sample = cases.audio[i];
            const double scaled = sample * 32768.0;
            if (!SameValue(o.nearbyint(scaled), g.nearbyint(scaled))) ++conversions;
            if (!SameValue(o.nearbyint(sample * 8388608.0), g.nearbyint(sample * 8388608.0))) ++conversions;
            if (o.lround(scaled) != g.lround(scaled)) ++conversions;
            if (!SameValue(o.roundf((float)scaled), g.roundf((float)scaled))) ++conversions;
            if (!SameValue(o.fabsf(cases.audioF[i]), g.fabsf(cases.audioF[i]))) ++conversions;
            // The EQ's sine/cosine coefficients at each supported rate, and the biquad values.
            static const double kRates[] = {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0};
            const double rate = kRates[i % 6];
            const double frequency = 20.0 * o.pow(1000.0, (double)(i % 512) / 512.0);  // 20 Hz to 20 kHz
            const double w = 2 * 3.141592653589793 * frequency / rate;
            double ourSine = 0, ourCosine = 0;
            o.sincos(w, &ourSine, &ourCosine);
            double theirSine = 0, theirCosine = 0;
            g.sincos(w, &theirSine, &theirCosine);
            tSin.Record(DoubleUlps(ourSine, theirSine), w);
            tCos.Record(DoubleUlps(ourCosine, theirCosine), w);
            tTan.Record(DoubleUlps(o.tan(w), g.tan(w)), w);
            // An envelope's one-pole coefficient, exp(-1/(seconds * rate)).
            const double seconds = 0.006 + 0.4 * (double)(i % 17) / 17;
            const double coefficient = -1.0 / (seconds * rate);
            tExp.Record(DoubleUlps(o.exp(coefficient), g.exp(coefficient)), coefficient);
            // ReplayGain: a gain in dB through pow(10, db/20), and a dB value through log10.
            const double db = -24.0 + 48.0 * (double)(i % 1024) / 1024.0;
            const double gain = o.pow(10.0, db / 20.0);
            tPow.Record(DoubleUlps(gain, g.pow(10.0, db / 20.0)), db);
            const double peak = 1.0 + 20000.0 * (double)(i % 512);
            tLog10.Record(DoubleUlps(o.log10(peak / 20000.0), g.log10(peak / 20000.0)), peak);
            const float gainF = o.powf(10.0f, (float)db / 20.0f);
            tPowf.Record(FloatUlps(gainF, g.powf(10.0f, (float)db / 20.0f)), db);
            // A normaliser: the square root of a block's energy, and the float form of a sample.
            const double energy = 1e-12 + o.fabs(cases.audio[i]);
            tSqrt.Record(DoubleUlps(o.sqrt(energy), g.sqrt(energy)), energy);
            tSqrtf.Record(FloatUlps(o.sqrtf((float)energy), g.sqrtf((float)energy)), energy);
        }
        Expect(conversions == 0, "the sample conversions are exactly glibc's");
    }

    // ---- the rounding mode ---------------------------------------------------------------------
    {
        int wrong = 0;
        static const double kValues[] = {0.0, -0.0, 0.5, -0.5, 1.5, -1.5, 2.5, -2.5, 2.4999999, -2.5000001,
                                         1e16, -1e16, 1e300, -1e300, 0x1p-1074, 3.5, -3.5};
        for (int mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
            if (o.fesetround(mode) != 0) ++wrong;
            if (o.fegetround() != mode) ++wrong;
            if (g.fegetround() != mode) ++wrong;
            for (double x : kValues) {
                if (!SameValue(o.nearbyint(x), g.nearbyint(x))) ++wrong;
                if (!SameValue(o.nearbyintf((float)x), g.nearbyintf((float)x))) ++wrong;
                if (o.lrint(x) != g.lrint(x)) ++wrong;
            }
        }
        o.fesetround(FE_TONEAREST);
        errno = 0;
        const int refused = o.fesetround(12345);
        MathBarrier();
        Expect(refused == -1 && errno == EINVAL, "fesetround refuses an unknown mode with EINVAL");
        Expect(wrong == 0, "nearbyint and lrint follow every rounding mode");
    }

    // ---- everything within its budget ----------------------------------------------------------
    MathTally tallies[] = {tExp,  tExp2,  tExpf,  tExp2f, tLog,   tLog2,  tLog10,  tLog10f, tLog1pf,
                           tPow,  tPowf,  tSin,   tCos,   tTan,   tSinf,  tCosf,   tSincos, tSincosf,
                           tAtan, tAtan2, tAtan2f, tCosh, tHypotf, tCabs, tSqrt,   tSqrtf};
    const int count = (int)(sizeof tallies / sizeof *tallies);
    PrintMathTallies(tallies, count);
    long long overBudget = 0, badErrno = 0, allowedUnderflow = 0;
    for (int i = 0; i < count; ++i) {
        overBudget += tallies[i].over;
        badErrno += tallies[i].badErrno;
        allowedUnderflow += tallies[i].allowed;
        if (tallies[i].over) printf("%s: %.2f ulps over budget at %g\n", tallies[i].name, tallies[i].worst,
                                    tallies[i].worstArgument);
    }
    if (allowedUnderflow) {
        printf("note: %lld float results in the subnormal range report ERANGE in glibc and not here\n",
               allowedUnderflow);
    }
    Expect(overBudget == 0 && badErrno == 0, "every function stays within its ulp budget and glibc's errno");
    Expect(exactMismatches == 0, "the exact operations are bit for bit glibc's");
    Expect(InProgram((const void*)o.exp) && InProgram((const void*)o.pow) && InProgram((const void*)o.sin) &&
               InProgram((const void*)o.sqrt) && InProgram((const void*)o.nearbyint) && InProgram((const void*)o.fmod) &&
               InProgram((const void*)o.hypotf) && InProgram((const void*)o.fegetround) && InProgram((const void*)o.cabs),
           "the math functions are cordinal's own");
    Expect(!InProgram((const void*)g.exp) && !InProgram((const void*)g.pow) && g.exp != o.exp && g.sqrt != o.sqrt,
           "glibc keeps its own copies, so these compare two implementations");
}


// ---- timing the hot ones -----------------------------------------------------------------------

// Times cordinal's and glibc's functions over the same arguments, taking turns seven times and
// keeping each one's best, as the string benchmark does.
inline int MathBench() {
    GlibcMath g = LoadGlibcMath();
    GlibcMath o = LoadOurMath();
    constexpr int kCount = 4096;
    static double xs[kCount], ys[kCount];
    static float xfs[kCount], yfs[kCount];
    Random r{0x1234567890ABCDEFull};
    for (int i = 0; i < kCount; ++i) {
        xs[i] = MathCaseSet::Uniform(r, -16, 16);
        ys[i] = MathCaseSet::Uniform(r, 0.001, 1000);
        xfs[i] = (float)xs[i];
        yfs[i] = (float)ys[i];
    }
    volatile double sink = 0;
    printf("%-10s %10s %10s %7s\n", "function", "cordinal", "glibc", "ratio");
    auto Row = [&](const char* name, auto ours, auto theirs) {
        double a = 0, b = 0;
        TimeBoth(ours, theirs, a, b);
        printf("%-10s %8.1fns %8.1fns %6.2fx\n", name, a, b, a / b);
    };
    Row("sqrt", [&] { double s = 0; for (int i = 0; i < kCount; ++i) s += o.sqrt(ys[i]); sink = s; },
        [&] { double s = 0; for (int i = 0; i < kCount; ++i) s += g.sqrt(ys[i]); sink = s; });
    Row("nearbyint", [&] { double s = 0; for (int i = 0; i < kCount; ++i) s += o.nearbyint(xs[i] * 32768.0); sink = s; },
        [&] { double s = 0; for (int i = 0; i < kCount; ++i) s += g.nearbyint(xs[i] * 32768.0); sink = s; });
    Row("fmod", [&] { double s = 0; for (int i = 0; i < kCount; ++i) s += o.fmod(xs[i], ys[i]); sink = s; },
        [&] { double s = 0; for (int i = 0; i < kCount; ++i) s += g.fmod(xs[i], ys[i]); sink = s; });
    Row("exp", [&] { double s = 0; for (int i = 0; i < kCount; ++i) s += o.exp(xs[i]); sink = s; },
        [&] { double s = 0; for (int i = 0; i < kCount; ++i) s += g.exp(xs[i]); sink = s; });
    Row("log", [&] { double s = 0; for (int i = 0; i < kCount; ++i) s += o.log(ys[i]); sink = s; },
        [&] { double s = 0; for (int i = 0; i < kCount; ++i) s += g.log(ys[i]); sink = s; });
    Row("pow", [&] { double s = 0; for (int i = 0; i < kCount; ++i) s += o.pow(ys[i], 0.5); sink = s; },
        [&] { double s = 0; for (int i = 0; i < kCount; ++i) s += g.pow(ys[i], 0.5); sink = s; });
    Row("sin", [&] { double s = 0; for (int i = 0; i < kCount; ++i) s += o.sin(xs[i]); sink = s; },
        [&] { double s = 0; for (int i = 0; i < kCount; ++i) s += g.sin(xs[i]); sink = s; });
    Row("cos", [&] { double s = 0; for (int i = 0; i < kCount; ++i) s += o.cos(xs[i]); sink = s; },
        [&] { double s = 0; for (int i = 0; i < kCount; ++i) s += g.cos(xs[i]); sink = s; });
    Row("tan", [&] { double s = 0; for (int i = 0; i < kCount; ++i) s += o.tan(xs[i]); sink = s; },
        [&] { double s = 0; for (int i = 0; i < kCount; ++i) s += g.tan(xs[i]); sink = s; });
    Row("atan2", [&] { double s = 0; for (int i = 0; i < kCount; ++i) s += o.atan2(xs[i], ys[i]); sink = s; },
        [&] { double s = 0; for (int i = 0; i < kCount; ++i) s += g.atan2(xs[i], ys[i]); sink = s; });
    Row("hypotf", [&] { float s = 0; for (int i = 0; i < kCount; ++i) s += o.hypotf(xfs[i], yfs[i]); sink = s; },
        [&] { float s = 0; for (int i = 0; i < kCount; ++i) s += g.hypotf(xfs[i], yfs[i]); sink = s; });
    Row("expf", [&] { float s = 0; for (int i = 0; i < kCount; ++i) s += o.expf(xfs[i]); sink = s; },
        [&] { float s = 0; for (int i = 0; i < kCount; ++i) s += g.expf(xfs[i]); sink = s; });
    Row("powf", [&] { float s = 0; for (int i = 0; i < kCount; ++i) s += o.powf(yfs[i], 0.5f); sink = s; },
        [&] { float s = 0; for (int i = 0; i < kCount; ++i) s += g.powf(yfs[i], 0.5f); sink = s; });
    Row("sinf", [&] { float s = 0; for (int i = 0; i < kCount; ++i) s += o.sinf(xfs[i]); sink = s; },
        [&] { float s = 0; for (int i = 0; i < kCount; ++i) s += g.sinf(xfs[i]); sink = s; });
    Row("sincos", [&] { double s = 0, c = 0, t = 0; for (int i = 0; i < kCount; ++i) { o.sincos(xs[i], &s, &c); t += s + c; } sink = t; },
        [&] { double s = 0, c = 0, t = 0; for (int i = 0; i < kCount; ++i) { g.sincos(xs[i], &s, &c); t += s + c; } sink = t; });
    (void)sink;
    return 0;
}

}  // namespace libc_checks
