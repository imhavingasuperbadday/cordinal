// cordinal's number parsing (numbers.cpp) against glibc's, for checks.h: strtof, strtod and strtold
// over the awkward spellings (hex floats, "0x" with no digits, exponents without digits, inf and
// nan payloads, subnormals, overflow, underflow, trailing junk, leading whitespace), thousands of
// random decimal and hexadecimal numbers, and 17-, 9- and 21-digit round trips; the strtol family in
// every base with signs and prefixes and its clamping; the wide forms; and the __isoc23_ names, which
// are glibc's own C23 entry points that read 0b. Every answer is glibc's: the value's bits, the
// end pointer and errno.
#pragma once
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

namespace libc_checks {

// glibc's own copies of the parsers, and cordinal's plain strtol family under names that the C23
// redirection in <stdlib.h> and <wchar.h> cannot reach.
namespace numbers {
typedef float (*StrtofFn)(const char*, char**);
typedef double (*StrtodFn)(const char*, char**);
typedef long double (*StrtoldFn)(const char*, char**);
typedef long (*StrtolFn)(const char*, char**, int);
typedef unsigned long (*StrtoulFn)(const char*, char**, int);
typedef long (*WcstolFn)(const wchar_t*, wchar_t**, int);
typedef double (*WcstodFn)(const wchar_t*, wchar_t**);
}  // namespace numbers
extern "C" long cordinal_check_strtol(const char*, char**, int) __asm__("strtol");
extern "C" long long cordinal_check_strtoll(const char*, char**, int) __asm__("strtoll");
extern "C" unsigned long cordinal_check_strtoul(const char*, char**, int) __asm__("strtoul");
extern "C" unsigned long long cordinal_check_strtoull(const char*, char**, int) __asm__("strtoull");
extern "C" long cordinal_check_wcstol(const wchar_t*, wchar_t**, int) __asm__("wcstol");
extern "C" double cordinal_check_wcstod(const wchar_t*, wchar_t**) __asm__("wcstod");
// glibc's headers route strtol and its wide forms to these C23 names, which numbers.cpp defines.
extern "C" long __isoc23_strtol(const char*, char**, int);
extern "C" long __isoc23_wcstol(const wchar_t*, wchar_t**, int);

// A small generator, so the barrage is the same on every run.
struct NumberRandom {
    uint64_t state;
    uint64_t Next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }
    uint32_t Below(uint32_t n) { return (uint32_t)(Next() % n); }
};

inline uint64_t DoubleBits(double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}
inline uint32_t FloatBits(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}
inline void LongDoubleBits(long double value, unsigned char out[16]) {
    memset(out, 0, 16);
    memcpy(out, &value, 10);
}

// One number through all three float parsers, against glibc's. A mismatch names the text and both
// answers, so a failure in the log says what went wrong without a rerun.
inline void CheckFloat(const char* text) {
    using namespace numbers;
    static StrtodFn glibcStrtod;
    static StrtofFn glibcStrtof;
    static StrtoldFn glibcStrtold;
    if (!glibcStrtod) {
        glibcStrtod = (StrtodFn)dlsym(RTLD_DEFAULT, "strtod");
        glibcStrtof = (StrtofFn)dlsym(RTLD_DEFAULT, "strtof");
        glibcStrtold = (StrtoldFn)dlsym(RTLD_DEFAULT, "strtold");
    }
    {
        char* ours = nullptr;
        char* theirs = nullptr;
        errno = 0;
        const double mine = strtod(text, &ours);
        const int myErrno = errno;
        errno = 0;
        const double glibc = glibcStrtod(text, &theirs);
        if (DoubleBits(mine) != DoubleBits(glibc) || ours != theirs || myErrno != errno) {
            char detail[200];
            snprintf(detail, sizeof detail, "strtod: %016llx end %d errno %d, glibc %016llx end %d errno %d",
                     (unsigned long long)DoubleBits(mine), (int)(ours - text), myErrno,
                     (unsigned long long)DoubleBits(glibc), (int)(theirs - text), errno);
            if (failures < 40) fprintf(stderr, "FAIL [%s] %s\n", text, detail);
            ++failures;
        }
    }
    {
        char* ours = nullptr;
        char* theirs = nullptr;
        errno = 0;
        const float mine = strtof(text, &ours);
        const int myErrno = errno;
        errno = 0;
        const float glibc = glibcStrtof(text, &theirs);
        if (FloatBits(mine) != FloatBits(glibc) || ours != theirs || myErrno != errno) {
            if (failures < 40)
                fprintf(stderr, "FAIL [%s] strtof: %08x end %d errno %d, glibc %08x end %d errno %d\n", text,
                        FloatBits(mine), (int)(ours - text), myErrno, FloatBits(glibc), (int)(theirs - text), errno);
            ++failures;
        }
    }
    {
        char* ours = nullptr;
        char* theirs = nullptr;
        errno = 0;
        const long double mine = strtold(text, &ours);
        const int myErrno = errno;
        unsigned char mineBits[16], glibcBits[16];
        LongDoubleBits(mine, mineBits);
        errno = 0;
        const long double glibc = glibcStrtold(text, &theirs);
        LongDoubleBits(glibc, glibcBits);
        if (memcmp(mineBits, glibcBits, 10) || ours != theirs || myErrno != errno) {
            if (failures < 40)
                fprintf(stderr, "FAIL [%s] strtold: %02x%02x %016llx end %d errno %d, glibc %02x%02x %016llx end %d errno %d\n",
                        text, mineBits[9], mineBits[8],
                        (unsigned long long)((uint64_t)mineBits[0] | ((uint64_t)mineBits[1] << 8) |
                                             ((uint64_t)mineBits[2] << 16) | ((uint64_t)mineBits[3] << 24) |
                                             ((uint64_t)mineBits[4] << 32) | ((uint64_t)mineBits[5] << 40) |
                                             ((uint64_t)mineBits[6] << 48) | ((uint64_t)mineBits[7] << 56)),
                        (int)(ours - text), myErrno, glibcBits[9], glibcBits[8],
                        (unsigned long long)((uint64_t)glibcBits[0] | ((uint64_t)glibcBits[1] << 8) |
                                             ((uint64_t)glibcBits[2] << 16) | ((uint64_t)glibcBits[3] << 24) |
                                             ((uint64_t)glibcBits[4] << 32) | ((uint64_t)glibcBits[5] << 40) |
                                             ((uint64_t)glibcBits[6] << 48) | ((uint64_t)glibcBits[7] << 56)),
                        (int)(theirs - text), errno);
            ++failures;
        }
    }
}

inline void CheckIntegers(const char* text, int base) {
    using namespace numbers;
    static StrtolFn glibcStrtol;
    static StrtoulFn glibcStrtoul;
    static long (*glibcStrtoll)(const char*, char**, int);
    static unsigned long long (*glibcStrtoull)(const char*, char**, int);
    if (!glibcStrtol) {
        glibcStrtol = (StrtolFn)dlsym(RTLD_DEFAULT, "strtol");
        glibcStrtoul = (StrtoulFn)dlsym(RTLD_DEFAULT, "strtoul");
        glibcStrtoll = (long (*)(const char*, char**, int))dlsym(RTLD_DEFAULT, "strtoll");
        glibcStrtoull = (unsigned long long (*)(const char*, char**, int))dlsym(RTLD_DEFAULT, "strtoull");
    }
    // glibc leaves the end pointer alone for a base it cannot use, so that case compares value and
    // errno only.
    const bool badBase = base != 0 && (base < 2 || base > 36);
    char* ours = nullptr;
    char* theirs = nullptr;
    errno = 0;
    const long mine = cordinal_check_strtol(text, &ours, base);
    const int myErrno = errno;
    errno = 0;
    const long glibc = glibcStrtol(text, &theirs, base);
    if (mine != glibc || (!badBase && ours != theirs) || myErrno != errno) {
        if (failures < 40)
            fprintf(stderr, "FAIL [%s] strtol base %d: %ld end %d errno %d, glibc %ld end %d errno %d\n", text, base,
                    mine, (int)(ours - text), myErrno, glibc, (int)(theirs - text), errno);
        ++failures;
    }
    errno = 0;
    const unsigned long mineUnsigned = cordinal_check_strtoul(text, &ours, base);
    const int myErrnoUnsigned = errno;
    errno = 0;
    const unsigned long glibcUnsigned = glibcStrtoul(text, &theirs, base);
    if (mineUnsigned != glibcUnsigned || (!badBase && ours != theirs) || myErrnoUnsigned != errno) {
        if (failures < 40)
            fprintf(stderr, "FAIL [%s] strtoul base %d: %lu end %d errno %d, glibc %lu end %d errno %d\n", text, base,
                    mineUnsigned, (int)(ours - text), myErrnoUnsigned, glibcUnsigned, (int)(theirs - text), errno);
        ++failures;
    }
    errno = 0;
    const long long mineLong = cordinal_check_strtoll(text, &ours, base);
    const int myErrnoLong = errno;
    errno = 0;
    const long long glibcLong = glibcStrtoll(text, &theirs, base);
    if (mineLong != glibcLong || (!badBase && ours != theirs) || myErrnoLong != errno) {
        if (failures < 40)
            fprintf(stderr, "FAIL [%s] strtoll base %d: %lld end %d errno %d, glibc %lld end %d errno %d\n", text,
                    base, mineLong, (int)(ours - text), myErrnoLong, glibcLong, (int)(theirs - text), errno);
        ++failures;
    }
    errno = 0;
    const unsigned long long mineULL = cordinal_check_strtoull(text, &ours, base);
    const int myErrnoULL = errno;
    errno = 0;
    const unsigned long long glibcULL = glibcStrtoull(text, &theirs, base);
    if (mineULL != glibcULL || (!badBase && ours != theirs) || myErrnoULL != errno) {
        if (failures < 40)
            fprintf(stderr, "FAIL [%s] strtoull base %d: %llu end %d errno %d, glibc %llu end %d errno %d\n", text,
                    base, mineULL, (int)(ours - text), myErrnoULL, glibcULL, (int)(theirs - text), errno);
        ++failures;
    }
}

inline void CheckNumbers() {
    using namespace numbers;
    Expect(InProgram((const void*)&strtod) && InProgram((const void*)&strtof) && InProgram((const void*)&strtold) &&
               InProgram((const void*)&cordinal_check_strtol) && InProgram((const void*)&cordinal_check_wcstol),
           "strtod, strtof, strtold, strtol and wcstol are cordinal's own");
    Expect(!InProgram((const void*)dlsym(RTLD_DEFAULT, "strtod")), "glibc keeps its own strtod");

    static const char* const kLiterals[] = {
        "", " ", "  ", "\t\n\v\f\r7", "+", "-", ".", "-.", "+.", "e5", ".e5", "1e", "1e+", "1e-", "1.e5", "1.5e",
        "0", "-0", "00", "00000", "0.0", "-0.0", "0x", "0X", "0x.", "0xp1", "0x.p1", "-0x", "+0x", "0x0",
        "0x0p1", "0x1", "0x1p", "0x1p+", "0x1p-", "0x1p0", "0x1P0", "0x1.8", "0x1.8p1", "0x.8", "0x.8p1",
        "0x0.8", "0x00.1", "0x1g", "0x1.8x", "0x1.8p+", "0x1.8p1z", "0xf", "0xF.fp10",
        "0xffffffffffffffff", "0x10000000000000000", "1", "-1", "1.5", "-1.5", "12 34", "1_0", "1,5", "inf",
        "INF", "Inf", "infinity", "INFINITY", "infin", "infinx", "infinityx", "nan", "NAN", "nan()", "nan(",
        "nan(a", "nan(a)", "nan(a b)", "nan(_)", "nan(a)5", "nan(abc)", "-nan", "-inf", "+inf", "+nan", "1e2",
        "1e-2", "1e+2", "0b101", "0B11", "0b2", "1d5", "5e-324", "4.9406564584124654e-324",
        "2.4703282292062327e-324", "2.4703282292062328e-324", "1.7976931348623157e308",
        "1.7976931348623158e308", "1.7976931348623159e308", "1e308", "1e309", "1e-308", "1e-323", "1e-324",
        "1e-400", "1e400", "-1e400", "1e-4950", "1e-4951", "1e4932", "1e4933", "3.4e38", "3.5e38",
        "1.17549435e-38", "1.1754944e-38", "1e-45", "7.0064923216240854e-46", "0.1", "0.2", "0.3", "1.1",
        "2.2250738585072014e-308", "2.2250738585072011e-308", "9007199254740993", "9007199254740992.5",
        "18446744073709551616", "123456789012345678901234567890", "1e1000000000000000000000",
        "1e-1000000000000000000000", "0.000000000000000000000000001", "0x1.fffffffffffffp+1023",
        "0x1.fffffffffffff8p+1023", "0x1.fffffffffffff7p+1023", "0x0.0000000000001p-1022", "0x1p-1074",
        "0x1p-1075", "0x1p-1076", "0x1.0000000000001p-1075", "0x1.8p-1075", "0x1.fffffffffffff0000001p0",
        "0x1.00000000000000000000001p0", "0x1p-16445", "0x1p-16446", "0x1p16383", "0x1p16384",
        "0x1.fffffffffffffffep16383", "1.18973149535723176502e+4932", "1.18973149535723176509e+4932",
        "3.64519953188247460253e-4951", "3.64519953188247460252e-4951", "	 \n1.5", "- 1", "+-1", "--1",
        "0.000000000000000000000000000000000000000000001e45",
    };
    for (const char* text : kLiterals) CheckFloat(text);

    // Random decimal numbers: digits, a point somewhere, an exponent, and sometimes trailing junk.
    NumberRandom random{0x5eed1234};
    for (int round = 0; round < 1500; ++round) {
        char text[80];
        char* at = text;
        if (random.Below(4) == 0) *at++ = random.Below(2) ? ' ' : '\t';
        if (random.Below(3) == 0) *at++ = random.Below(2) ? '-' : '+';
        const int digits = 1 + (int)random.Below(25);
        const int point = (int)random.Below((uint32_t)digits + 1);
        for (int i = 0; i < digits; ++i) {
            if (i == point && random.Below(2)) *at++ = '.';
            *at++ = (char)('0' + random.Below(10));
        }
        if (random.Below(3) == 0) *at++ = '.';
        if (random.Below(2)) {
            *at++ = 'e';
            const int exponent = (int)random.Below(700) - 350;
            if (exponent < 0) *at++ = '-';
            else if (random.Below(2)) *at++ = '+';
            snprintf(at, 12, "%d", exponent < 0 ? -exponent : exponent);
            at += strlen(at);
        }
        if (random.Below(4) == 0) strcpy(at, "xyz");
        else *at = 0;
        CheckFloat(text);
    }
    // Random hex numbers the same way.
    for (int round = 0; round < 1200; ++round) {
        char text[80];
        char* at = text;
        if (random.Below(3) == 0) *at++ = '-';
        strcpy(at, "0x");
        at += 2;
        const int digits = 1 + (int)random.Below(20);
        const int point = (int)random.Below((uint32_t)digits + 1);
        for (int i = 0; i < digits; ++i) {
            if (i == point && random.Below(2)) *at++ = '.';
            *at++ = "0123456789abcdef"[random.Below(16)];
        }
        if (random.Below(2)) {
            *at++ = 'p';
            const int exponent = (int)random.Below(200) - 100;
            if (exponent < 0) *at++ = '-';
            else if (random.Below(2)) *at++ = '+';
            snprintf(at, 12, "%d", exponent < 0 ? -exponent : exponent);
            at += strlen(at);
        }
        *at = 0;
        CheckFloat(text);
    }
    // The quick path's own ground: a significand of up to nineteen digits scaled by a power of ten
    // within the range each format holds exactly, which is what most of the program's text is.
    for (int round = 0; round < 900; ++round) {
        char text[64];
        char* at = text;
        if (random.Below(3) == 0) *at++ = '-';
        const int digits = 1 + (int)random.Below(19);
        const int point = (int)random.Below((uint32_t)digits + 1);
        for (int i = 0; i < digits; ++i) {
            if (i == point) *at++ = '.';
            *at++ = (char)('0' + random.Below(10));
        }
        if (random.Below(2)) {
            *at++ = 'e';
            const int exponent = (int)random.Below(49) - 24;
            if (exponent < 0) *at++ = '-';
            snprintf(at, 8, "%d", exponent < 0 ? -exponent : exponent);
            at += strlen(at);
        }
        *at = 0;
        CheckFloat(text);
    }
    // Round trips: a value printed at full precision parses back to the same bits.
    int roundTripFailures = 0;
    for (int round = 0; round < 600; ++round) {
        char text[64];
        char* end = nullptr;
        uint64_t bits = random.Next();
        double value;
        memcpy(&value, &bits, sizeof value);
        if (value == value) {  // a nan's payload does not survive printing
            snprintf(text, sizeof text, "%.17g", value);
            if (DoubleBits(strtod(text, &end)) != bits && !*end) ++roundTripFailures;
        }
        uint32_t floatBits = (uint32_t)random.Next();
        float small;
        memcpy(&small, &floatBits, sizeof small);
        if (small == small) {
            snprintf(text, sizeof text, "%.9g", (double)small);
            if (FloatBits(strtof(text, &end)) != floatBits && !*end) ++roundTripFailures;
        }
        const long double wide = (long double)value;
        if (wide == wide) {
            snprintf(text, sizeof text, "%.21Lg", wide);
            const long double back = strtold(text, &end);
            if (memcmp(&back, &wide, 10) && !*end) ++roundTripFailures;
        }
    }
    if (roundTripFailures) {
        fprintf(stderr, "FAIL %d full-precision round trips did not return the same value\n", roundTripFailures);
        failures += roundTripFailures;
    }

    // Integers: the awkward spellings in every base, then random digits.
    static const char* const kIntegerLiterals[] = {
        "", " ", "+", "-", "0x", "0xg", "0x1", "-0x1", "0x1g", "010", "08", "0b101", "0b", "0b2", "  -42xyz",
        "9223372036854775807", "9223372036854775808", "18446744073709551615", "18446744073709551616",
        "-9223372036854775808", "-9223372036854775809", "4294967296", "0xffffffffffffffff", "1", "12 34",
        "0B11", "0b11", "0009", "-0", "+0x10", "z", "Z", "999999999999999999999999999", "  +0", "0X1f",
        "  \t\n7", "1e5",
    };
    static const int kBases[] = {0, 2, 8, 10, 16, 36, 1, 37, -5};
    for (const char* text : kIntegerLiterals)
        for (int base : kBases) CheckIntegers(text, base);
    for (int round = 0; round < 1500; ++round) {
        char text[60];
        char* at = text;
        if (random.Below(4) == 0) *at++ = ' ';
        if (random.Below(3) == 0) *at++ = random.Below(2) ? '-' : '+';
        const int base = kBases[random.Below(6)];
        const char* alphabet = base == 2 ? "012" : base == 8 ? "0123456789"
                                    : base == 16 ? "0123456789abcdefg"
                                                 : "0123456789abcdefzZ";
        const int digits = 1 + (int)random.Below(22);
        for (int i = 0; i < digits; ++i) *at++ = alphabet[random.Below((uint32_t)strlen(alphabet))];
        if (random.Below(4) == 0) strcpy(at, "tail");
        else *at = 0;
        CheckIntegers(text, base);
    }

    // The wide forms and the C23 names.
    auto glibcWcstol = (WcstolFn)dlsym(RTLD_DEFAULT, "wcstol");
    auto glibcWcstod = (WcstodFn)dlsym(RTLD_DEFAULT, "wcstod");
    auto glibcC23Strtol = (StrtolFn)dlsym(RTLD_DEFAULT, "__isoc23_strtol");
    auto glibcC23Wcstol = (WcstolFn)dlsym(RTLD_DEFAULT, "__isoc23_wcstol");
    if (!glibcWcstol || !glibcWcstod || !glibcC23Strtol || !glibcC23Wcstol) {
        Expect(false, "glibc's wide and C23 parsers are found");
        return;
    }
    static const wchar_t* const kWide[] = {L"", L"  ", L"abc", L"0x10", L"0x", L"010", L"0b11", L"-42x",
                                           L"9223372036854775808", L"1.5e3", L"-inf", L"0x1.8p3", L"  +7",
                                           L"9z", L"0x1p-1075", L"1e400"};
    for (const wchar_t* text : kWide) {
        wchar_t* ours = nullptr;
        wchar_t* theirs = nullptr;
        errno = 0;
        const long mine = cordinal_check_wcstol(text, &ours, 0);
        const int myErrno = errno;
        errno = 0;
        const long glibc = glibcWcstol(text, &theirs, 0);
        if (mine != glibc || ours != theirs || myErrno != errno) {
            if (failures < 40) fprintf(stderr, "FAIL [wcstol] %ld end %d errno %d, glibc %ld end %d errno %d\n", mine,
                                       (int)(ours - text), myErrno, glibc, (int)(theirs - text), errno);
            ++failures;
        }
        errno = 0;
        const double mineWide = cordinal_check_wcstod(text, &ours);
        const int myErrnoWide = errno;
        errno = 0;
        const double glibcWide = glibcWcstod(text, &theirs);
        if (DoubleBits(mineWide) != DoubleBits(glibcWide) || ours != theirs || myErrnoWide != errno) {
            if (failures < 40) fprintf(stderr, "FAIL [wcstod] %016llx, glibc %016llx\n",
                                       (unsigned long long)DoubleBits(mineWide),
                                       (unsigned long long)DoubleBits(glibcWide));
            ++failures;
        }
    }
    static const char* const kC23[] = {"0b101", "0b2", "0b", "0B11", "  -0b1", "0x1f", "+0b0",
                                       "0b11111111111111111111111111111111", "0b1z"};
    for (const char* text : kC23) {
        char* ours = nullptr;
        char* theirs = nullptr;
        errno = 0;
        const long mine = __isoc23_strtol(text, &ours, 0);
        const int myErrno = errno;
        errno = 0;
        const long glibc = glibcC23Strtol(text, &theirs, 0);
        if (mine != glibc || ours != theirs || myErrno != errno) {
            if (failures < 40) fprintf(stderr, "FAIL [__isoc23_strtol] %s: %ld end %d errno %d, glibc %ld end %d errno %d\n",
                                       text, mine, (int)(ours - text), myErrno, glibc, (int)(theirs - text), errno);
            ++failures;
        }
        const wchar_t* wide = L"0b101";
        wchar_t* wideOurs = nullptr;
        wchar_t* wideTheirs = nullptr;
        const long mineWide = __isoc23_wcstol(wide, &wideOurs, 0);
        const long glibcWide = glibcC23Wcstol(wide, &wideTheirs, 0);
        if (mineWide != glibcWide || wideOurs != wideTheirs) {
            if (failures < 40) fprintf(stderr, "FAIL [__isoc23_wcstol] %ld against glibc %ld\n", mineWide, glibcWide);
            ++failures;
        }
    }
    // A base glibc cannot use is EINVAL, and the end pointer is left alone.
    char* end = nullptr;
    errno = 0;
    Expect(cordinal_check_strtol("12", &end, 1) == 0 && errno == EINVAL && end == nullptr,
           "strtol with base 1 answers EINVAL and leaves the end pointer");
}

}  // namespace libc_checks
