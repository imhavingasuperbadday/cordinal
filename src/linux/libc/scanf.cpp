// cordinal's own scanf family (stage 5a of replacing glibc): the scanning core behind fscanf,
// scanf, sscanf, swscanf and the __isoc99/__isoc23 names glibc's headers and the static libstdc++
// ask for. It reads through cordinal's FILE streams (stdio.cpp), which glibc's scanf cannot do and
// the other way round.
//
// The grammar and return values follow musl's vfscanf (src/stdio/vfscanf.c, MIT licensed, read
// November 17, 2026) with glibc's edge answers: a match failure returns the assignments made so
// far (0 when there were none), running out of input before the first assignment returns EOF (-1),
// a malformed exponent in a float ("1e") fails the conversion, and %i reads 0x, 0b and leading-zero
// forms as glibc's C23 scanf does. Integer conversion clamps to long's range and stores the cast,
// as glibc's strtol-based path does ("%d" of 2^63-1 overflows to -1), with ERANGE. Floats are
// parsed from the characters the grammar accepts and handed to cordinal's own strtof, strtod or
// strtold (numbers.cpp, stage 5b), whose grammar and rounding the libc check compares with glibc's.
//
// Positional arguments (%1$d) and the GNU allocation modifier (%ms) are not implemented: nothing in
// cordinal or the static libstdc++ uses them (the libc check says so).
#include "stdio_internal.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

extern "C" void* malloc(size_t);
extern "C" void free(void*);
extern "C" void* realloc(void*, size_t);
extern "C" size_t mbrtowc(wchar_t*, const char*, size_t, void*);
extern "C" size_t wcrtomb(char*, wchar_t, void*);
extern "C" double strtod(const char*, char**);
extern "C" float strtof(const char*, char**);
extern "C" long double strtold(const char*, char**);
// The stream scanf reads from (stdio.cpp), declared here for the same reason as its own file.
extern "C" int fgetc(FILE*);
extern "C" FILE* stdin;

#ifndef EOF
#define EOF (-1)
#endif

namespace {

// glibc's mbstate_t on x86-64: a count and a small union (see printf.cpp).
struct MbState {
    int count;
    union {
        unsigned int value;
        char bytes[4];
    } state;
};

bool IsSpace(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

int DigitValue(int c, int base) {
    int value;
    if (c >= '0' && c <= '9') value = c - '0';
    else if (c >= 'a' && c <= 'f') value = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') value = c - 'A' + 10;
    else return -1;
    return value < base ? value : -1;
}

// One input: a FILE (fscanf), a byte string (sscanf) or a wide string (swscanf). Width limits and
// the count %n reports are kept here; one character of pushback is enough for this grammar.
struct Source {
    FILE* file = nullptr;
    const char* bytes = nullptr;
    size_t byteCount = 0, byteAt = 0;
    const wchar_t* wide = nullptr;
    size_t wideAt = 0;
    bool wideSource = false;
    int pushed = -1;
    bool hasPushed = false;
    size_t limit = 0;  // 0 = no limit
    size_t taken = 0;  // for the conversion in progress
    size_t total = 0;  // for %n

    // A code point, -1 at the end of input, -2 when the field width is used up.
    int Get() {
        if (limit && taken >= limit) return -2;
        int c;
        if (hasPushed) {
            hasPushed = false;
            c = pushed;
        } else if (file) {
            c = fgetc(file);
            if (c == EOF) return -1;
        } else if (wideSource) {
            if (!wide[wideAt]) return -1;
            c = (int)(unsigned int)wide[wideAt++];
        } else {
            if (byteAt >= byteCount) return -1;
            c = (unsigned char)bytes[byteAt++];
        }
        ++taken;
        ++total;
        return c;
    }

    void Unget(int c) {
        if (c < 0) return;
        pushed = c;
        hasPushed = true;
        if (taken) --taken;
        if (total) --total;
    }
};

void SkipSpace(Source& source) {
    source.limit = 0;
    for (;;) {
        const int c = source.Get();
        if (c < 0) return;
        if (!IsSpace(c)) {
            source.Unget(c);
            return;
        }
    }
}

void StoreInteger(void* dest, int length, bool isSigned, unsigned long long bits) {
    if (!dest) return;
    if (isSigned) {
        switch (length) {
            case 1: *(signed char*)dest = (signed char)(long long)bits; break;   // hh
            case 2: *(short*)dest = (short)(long long)bits; break;              // h
            case 3: *(long*)dest = (long)(long long)bits; break;                 // l
            case 4: *(long long*)dest = (long long)bits; break;                  // ll
            case 5: *(ptrdiff_t*)dest = (ptrdiff_t)(long long)bits; break;       // t
            case 6: *(intmax_t*)dest = (intmax_t)(long long)bits; break;         // j
            case 7: *(ptrdiff_t*)dest = (ptrdiff_t)(long long)bits; break;       // z (signed)
            default: *(int*)dest = (int)(long long)bits; break;
        }
    } else {
        switch (length) {
            case 1: *(unsigned char*)dest = (unsigned char)bits; break;
            case 2: *(unsigned short*)dest = (unsigned short)bits; break;
            case 3: *(unsigned long*)dest = (unsigned long)bits; break;
            case 4: *(unsigned long long*)dest = bits; break;
            case 5: *(size_t*)dest = (size_t)bits; break;                        // t unsigned
            case 6: *(uintmax_t*)dest = (uintmax_t)bits; break;
            case 7: *(size_t*)dest = (size_t)bits; break;
            default: *(unsigned int*)dest = (unsigned int)bits; break;
        }
    }
}

// Where one string conversion's characters go: bytes for %s/%c/%[, wide characters for the l forms.
// A wide destination fed bytes decodes them with mbrtowc across calls, and a narrow destination
// fed wide characters converts them with wcrtomb, so both forms work from either kind of input.
struct Storer {
    void* dest = nullptr;
    bool wideUnits = false;
    bool wideSource = false;
    MbState state{};
    size_t count = 0;
    bool insideCharacter = false;
    bool failed = false;

    bool Put(int c) {
        if (!dest) return true;
        if (wideUnits) {
            if (wideSource) {
                ((wchar_t*)dest)[count++] = (wchar_t)c;
                return true;
            }
            wchar_t wc;
            const size_t r = mbrtowc(&wc, (const char*)&c, 1, &state);
            if (r == (size_t)-2) {
                insideCharacter = true;
                return true;
            }
            if (r == (size_t)-1) return failed = true, false;
            ((wchar_t*)dest)[count++] = wc;
            insideCharacter = false;
            return true;
        }
        if (wideSource) {
            char mb[8];
            MbState fresh{};
            const size_t n = wcrtomb(mb, (wchar_t)c, &fresh);
            if (n == (size_t)-1) return failed = true, false;
            memcpy((char*)dest + count, mb, n);
            count += n;
            return true;
        }
        ((char*)dest)[count++] = (char)(unsigned char)c;
        return true;
    }

    bool Finish() {
        if (insideCharacter) return false;
        if (dest && !wideUnits && !wideSource) ((char*)dest)[count] = '\0';
        else if (dest && wideUnits && wideSource) ((wchar_t*)dest)[count] = L'\0';
        else if (dest && wideUnits && !wideSource)
            if (!insideCharacter) ((wchar_t*)dest)[count] = 0;
        return !failed && !insideCharacter;
    }
};

enum class Outcome { Ok, MatchFail, InputFail };

enum Length { kNone, kHH, kH, kL, kLL, kBigL, kZ, kT, kJ };

int LengthCode(Length length) {
    switch (length) {
        case kHH: return 1;
        case kH: return 2;
        case kL: return 3;
        case kLL: return 4;
        case kT: return 5;
        case kJ: return 6;
        case kZ: return 7;
        default: return 0;
    }
}

// Integer conversion with glibc's clamping: a signed conversion saturates at long's ends (then the
// caller casts to its own size), an unsigned one at unsigned long's, and either sets ERANGE.
Outcome ScanInteger(Source& source, int base, bool isSigned, void* dest, int length) {
    int c = source.Get();
    if (c < 0) return Outcome::InputFail;
    bool negative = false;
    if (c == '+' || c == '-') {
        negative = c == '-';
        c = source.Get();
        if (c < 0) return Outcome::MatchFail;  // a sign with no number
    }

    bool prefixed = false;
    if (base == 0) {
        if (c == '0') {
            const int next = source.Get();
            if (next == 'x' || next == 'X' || next == 'b' || next == 'B') {
                const int third = source.Get();
                const int chosen = (next == 'x' || next == 'X') ? 16 : 2;
                if (DigitValue(third, chosen) >= 0) {
                    base = chosen;
                    c = third;
                    prefixed = true;
                } else {
                    return Outcome::MatchFail;  // 0x or 0b without digits, as glibc's %i
                }
            } else {
                source.Unget(next);
                base = 8;
            }
        } else {
            base = 10;
        }
    } else if (base == 16 && c == '0') {
        const int next = source.Get();
        if (next == 'x' || next == 'X') {
            const int third = source.Get();
            if (DigitValue(third, 16) >= 0) {
                c = third;
                prefixed = true;
            } else {  // "0x" with no hex digit reads as 0, leaving the x
                source.Unget(third);
                source.Unget(next);
            }
        } else {
            source.Unget(next);
        }
    }
    (void)prefixed;

    unsigned long long value = 0;
    bool overflow = false;
    bool any = false;
    for (;;) {
        if (c < 0) break;
        const int digit = DigitValue(c, base);
        if (digit < 0) break;
        any = true;
        if (value > (~0ull - (unsigned)digit) / (unsigned)base) {
            overflow = true;
            value = ~0ull;
        } else {
            value = value * (unsigned)base + (unsigned)digit;
        }
        c = source.Get();
    }
    source.Unget(c);
    if (!any) return Outcome::MatchFail;

    if (isSigned) {
        const unsigned long long limit = negative ? 0x8000000000000000ull : 0x7fffffffffffffffull;
        if (value > limit) {
            value = limit;
            overflow = true;
        }
        const long long signedValue = negative ? (long long)(0 - value) : (long long)value;
        if (overflow) errno = ERANGE;
        StoreInteger(dest, length, true, (unsigned long long)signedValue);
    } else {
        if (overflow) errno = ERANGE;
        if (negative) value = 0 - value;
        StoreInteger(dest, length, false, value);
    }
    return Outcome::Ok;
}

// Float conversion: the grammar glibc's scanf accepts (a malformed exponent fails it rather than
// falling back), then the bytes go to strtof, strtod or strtold for the value.
Outcome ScanFloat(Source& source, Length length, void* dest) {
    char text[512];
    size_t used = 0;
    auto take = [&](int c) {
        if (used + 1 < sizeof text) text[used++] = (char)c;
    };

    int c = source.Get();
    if (c < 0) return Outcome::InputFail;
    if (c == '+' || c == '-') {
        take(c);
        c = source.Get();
        if (c < 0) return Outcome::MatchFail;
    }

    const int lower = c | 32;
    if (lower == 'i' || lower == 'n') {
        // inf, infinity, nan or nan(characters).
        static const char kInf[] = "inf", kNan[] = "nan", kInity[] = "inity";
        const char* word = lower == 'i' ? kInf : kNan;
        take(c);
        c = source.Get();
        for (int i = 1; word[i]; ++i) {
            if (c < 0 || (c | 32) != word[i]) return Outcome::MatchFail;
            take(c);
            c = source.Get();
        }
        if (lower == 'i' && (c | 32) == 'i') {  // "inity"
            for (int i = 0; kInity[i]; ++i) {
                if (c < 0 || (c | 32) != kInity[i]) return Outcome::MatchFail;
                take(c);
                c = source.Get();
            }
        } else if (lower == 'n' && c == '(') {
            take(c);
            c = source.Get();
            while (c >= 0 && c != ')') {
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      c == '_')) {
                    return Outcome::MatchFail;
                }
                take(c);
                c = source.Get();
            }
            if (c != ')') return Outcome::MatchFail;
            take(c);
            c = source.Get();
        }
        source.Unget(c);
        text[used] = 0;
        char* end = nullptr;
        if (length == kBigL)
            *(long double*)dest = strtold(text, &end);
        else if (length == kL)
            *(double*)dest = strtod(text, &end);
        else
            *(float*)dest = strtof(text, &end);
        return end == text + used ? Outcome::Ok : Outcome::MatchFail;
    }

    bool hex = false;
    bool any = false;
    if (c == '0') {
        take(c);
        any = true;
        const int next = source.Get();
        if (next == 'x' || next == 'X') {
            hex = true;
            take(next);
            c = source.Get();
        } else {
            c = next;
        }
    }
    for (;;) {
        const int digit = DigitValue(c, hex ? 16 : 10);
        if (digit < 0) break;
        take(c);
        any = true;
        c = source.Get();
    }
    if (c == '.') {
        take(c);
        c = source.Get();
        for (;;) {
            const int digit = DigitValue(c, hex ? 16 : 10);
            if (digit < 0) break;
            take(c);
            any = true;
            c = source.Get();
        }
    }
    if (!any) {
        source.Unget(c);
        return Outcome::MatchFail;
    }
    if (hex) {
        if (c == 'p' || c == 'P') {
            take(c);
            c = source.Get();
            if (c == '+' || c == '-') {
                take(c);
                c = source.Get();
            }
            if (DigitValue(c, 10) < 0) return Outcome::MatchFail;
            while (DigitValue(c, 10) >= 0) {
                take(c);
                c = source.Get();
            }
        }
    } else if (c == 'e' || c == 'E') {
        take(c);
        c = source.Get();
        if (c == '+' || c == '-') {
            take(c);
            c = source.Get();
        }
        if (DigitValue(c, 10) < 0) return Outcome::MatchFail;  // "1e" fails, as glibc's
        while (DigitValue(c, 10) >= 0) {
            take(c);
            c = source.Get();
        }
    }
    source.Unget(c);
    text[used] = 0;
    char* end = nullptr;
    if (length == kBigL)
        *(long double*)dest = strtold(text, &end);
    else if (length == kL)
        *(double*)dest = strtod(text, &end);
    else
        *(float*)dest = strtof(text, &end);
    return end == text + used ? Outcome::Ok : Outcome::MatchFail;
}

// The scanning core, musl's vfscanf without positional arguments or %m. `ap` is the caller's va_list
// as it arrives (an x86-64 va_list is an array, so a parameter is already the pointer va_arg wants).
int ScanCore(Source& source, const char* format, va_list ap) {
    int matches = 0;
    const char* p = format;
    while (*p) {
        source.limit = 0;
        source.taken = 0;
        if (IsSpace((unsigned char)*p)) {
            while (IsSpace((unsigned char)*p)) ++p;
            SkipSpace(source);
            continue;
        }
        if (*p != '%' || p[1] == '%') {
            if (*p == '%') {  // glibc skips leading whitespace before the literal %
                p += 2;
                int c;
                do {
                    c = source.Get();
                } while (c >= 0 && IsSpace(c));
                if (c != '%') {
                    source.Unget(c);
                    return c < 0 ? (matches ? matches : EOF) : matches;
                }
            } else {
                const int expected = (unsigned char)*p++;
                const int c = source.Get();
                if (c < 0) return matches ? matches : EOF;
                if (c != expected) {
                    source.Unget(c);
                    return matches;
                }
            }
            continue;
        }

        ++p;  // past '%'
        bool suppress = false;
        void* dest = nullptr;
        if (*p == '*') {
            suppress = true;
            ++p;
        } else {
            dest = va_arg(ap, void*);
        }
        int width = 0;
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            ++p;
        }
        Length length = kNone;
        switch (*p) {
            case 'h':
                if (p[1] == 'h') length = kHH, p += 2;
                else length = kH, ++p;
                break;
            case 'l':
                if (p[1] == 'l') length = kLL, p += 2;
                else length = kL, ++p;
                break;
            case 'L': length = kBigL; ++p; break;
            case 'z': length = kZ; ++p; break;
            case 't': length = kT; ++p; break;
            case 'j': length = kJ; ++p; break;
            default: break;
        }
        char conversion = *p;
        if (!conversion) {
            errno = EINVAL;
            return matches;
        }
        ++p;

        if (conversion == 'C' || conversion == 'S') {  // glibc's aliases for %lc and %ls
            conversion |= 32;
            length = kL;
        }

        if (conversion == 'n') {
            if (dest) StoreInteger(dest, LengthCode(length), true, (unsigned long long)source.total);
            continue;
        }
        if (conversion != 'c' && conversion != '[') SkipSpace(source);
        source.limit = (size_t)width;  // 0 is unlimited, as scanf's width is
        source.taken = 0;

        const bool wideConversion = length == kL;
        if (conversion == 's' || conversion == 'c' || conversion == '[') {
            Storer storer;
            storer.dest = suppress ? nullptr : dest;
            storer.wideUnits = wideConversion;
            storer.wideSource = source.wideSource;
            unsigned char set[256];
            const bool isSet = conversion == '[';
            if (conversion == 's') {
                memset(set, 0, sizeof set);
                for (int i = 0; i < 256; ++i) set[i] = IsSpace(i) ? 0 : 1;
                set[0] = 0;
            } else if (isSet) {
                bool invert = false;
                if (*p == '^') {
                    invert = true;
                    ++p;
                }
                memset(set, invert ? 1 : 0, sizeof set);
                if (*p == ']') {
                    set[(unsigned char)']'] = invert ? 0 : 1;
                    ++p;
                } else if (*p == '-') {
                    set[(unsigned char)'-'] = invert ? 0 : 1;
                    ++p;
                }
                while (*p && *p != ']') {
                    if (p[1] == '-' && p[2] && p[2] != ']') {
                        for (int c = (unsigned char)*p; c <= (unsigned char)p[2]; ++c)
                            set[c] = invert ? 0 : 1;
                        p += 3;
                    } else {
                        set[(unsigned char)*p] = invert ? 0 : 1;
                        ++p;
                    }
                }
                if (*p != ']') {
                    errno = EINVAL;
                    return matches;
                }
                ++p;
            }
            const int needed = conversion == 'c' ? (width < 1 ? 1 : width) : 0;
            int stored = 0;
            for (;;) {
                const int c = source.Get();
                if (c < 0) break;
                if (isSet || conversion == 's') {
                    if (c > 255 || !set[c]) {
                        source.Unget(c);
                        break;
                    }
                }
                if (!storer.Put(c)) {
                    source.Unget(c);
                    return matches ? matches : EOF;
                }
                ++stored;
                if (conversion == 'c' && stored >= needed) break;
            }
            if (conversion == 'c') {
                if (stored < needed) return matches ? matches : EOF;
            } else {
                if (stored == 0) return matches;
                if (!storer.Finish()) return matches;
            }
            if (!suppress) ++matches;
            continue;
        }

        if (conversion == 'd' || conversion == 'i' || conversion == 'u' || conversion == 'o' ||
            conversion == 'x' || conversion == 'X' || conversion == 'p') {
            const int base = conversion == 'o' ? 8
                             : conversion == 'i' ? 0
                             : (conversion == 'x' || conversion == 'X' || conversion == 'p') ? 16 : 10;
            const bool isSigned = conversion == 'd' || conversion == 'i';
            void* target = suppress ? nullptr : dest;
            Outcome outcome;
            if (conversion == 'p') {
                unsigned long long value = 0;
                outcome = ScanInteger(source, 16, false, suppress ? nullptr : &value,
                                      LengthCode(kNone));
                if (outcome == Outcome::Ok && target) *(void**)target = (void*)(uintptr_t)value;
            } else {
                outcome = ScanInteger(source, base, isSigned, target, LengthCode(length));
            }
            if (outcome == Outcome::InputFail) return matches ? matches : EOF;
            if (outcome == Outcome::MatchFail) return matches;
            if (!suppress) ++matches;
            continue;
        }

        if (conversion == 'a' || conversion == 'A' || conversion == 'e' || conversion == 'E' ||
            conversion == 'f' || conversion == 'F' || conversion == 'g' || conversion == 'G') {
            float scratch = 0;
            void* target = suppress ? (void*)&scratch : dest;
            const Outcome outcome = ScanFloat(source, length, target);
            if (outcome == Outcome::InputFail) return matches ? matches : EOF;
            if (outcome == Outcome::MatchFail) return matches;
            if (!suppress) ++matches;
            continue;
        }

        errno = EINVAL;
        return matches;
    }
    return matches;
}

// The wide format string becomes multibyte first, so one core serves fscanf, sscanf and swscanf.
char* NarrowFormat(const wchar_t* format) {
    size_t length = 0, capacity = 64;
    char* narrow = (char*)malloc(capacity);
    if (!narrow) return nullptr;
    for (const wchar_t* at = format; *at; ++at) {
        char mb[8];
        MbState fresh{};
        const size_t n = wcrtomb(mb, *at, &fresh);
        if (n == (size_t)-1) {
            free(narrow);
            errno = EILSEQ;
            return nullptr;
        }
        if (length + n + 1 > capacity) {
            size_t grown = capacity * 2;
            while (grown < length + n + 1) grown *= 2;
            char* bigger = (char*)realloc(narrow, grown);
            if (!bigger) {
                free(narrow);
                return nullptr;
            }
            narrow = bigger;
            capacity = grown;
        }
        memcpy(narrow + length, mb, n);
        length += n;
    }
    narrow[length] = 0;
    return narrow;
}

}  // namespace

#define CL extern "C" __attribute__((visibility("hidden")))

// GCC and libstdc++ know these as builtins with default visibility, so they are hidden in assembly.
__asm__(".hidden scanf");
__asm__(".hidden fscanf");
__asm__(".hidden sscanf");
__asm__(".hidden vscanf");
__asm__(".hidden vfscanf");
__asm__(".hidden vsscanf");
__asm__(".hidden swscanf");
__asm__(".hidden vswscanf");
__asm__(".hidden __isoc99_scanf");
__asm__(".hidden __isoc99_fscanf");
__asm__(".hidden __isoc99_sscanf");
__asm__(".hidden __isoc99_vscanf");
__asm__(".hidden __isoc99_vfscanf");
__asm__(".hidden __isoc99_vsscanf");
__asm__(".hidden __isoc99_swscanf");
__asm__(".hidden __isoc99_vswscanf");
__asm__(".hidden __isoc23_scanf");
__asm__(".hidden __isoc23_fscanf");
__asm__(".hidden __isoc23_sscanf");
__asm__(".hidden __isoc23_vscanf");
__asm__(".hidden __isoc23_vfscanf");
__asm__(".hidden __isoc23_vsscanf");
__asm__(".hidden __isoc23_swscanf");
__asm__(".hidden __isoc23_vswscanf");

CL int vfscanf(FILE* stream, const char* format, va_list ap) {
    if (!stream || !format) {
        errno = EINVAL;
        return EOF;
    }
    Source source;
    source.file = stream;
    return ScanCore(source, format, ap);
}

CL int fscanf(FILE* stream, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    const int result = vfscanf(stream, format, ap);
    va_end(ap);
    return result;
}

CL int vscanf(const char* format, va_list ap) { return vfscanf(stdin, format, ap); }

CL int scanf(const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    const int result = vfscanf(stdin, format, ap);
    va_end(ap);
    return result;
}

CL int vsscanf(const char* input, const char* format, va_list ap) {
    if (!input || !format) {
        errno = EINVAL;
        return EOF;
    }
    Source source;
    source.bytes = input;
    source.byteCount = strlen(input);
    return ScanCore(source, format, ap);
}

CL int sscanf(const char* input, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    const int result = vsscanf(input, format, ap);
    va_end(ap);
    return result;
}

CL int vswscanf(const wchar_t* input, const wchar_t* format, va_list ap) {
    if (!input || !format) {
        errno = EINVAL;
        return EOF;
    }
    char* narrow = NarrowFormat(format);
    if (!narrow) return EOF;
    Source source;
    source.wide = input;
    source.wideSource = true;
    const int result = ScanCore(source, narrow, ap);
    free(narrow);
    return result;
}

CL int swscanf(const wchar_t* input, const wchar_t* format, ...) {
    va_list ap;
    va_start(ap, format);
    const int result = vswscanf(input, format, ap);
    va_end(ap);
    return result;
}

// The C99 names older glibc headers and the static libstdc++ use, and the C23 names glibc's current
// headers send scanf and friends to. All of them are this core.
CL int __isoc99_vsscanf(const char* input, const char* format, va_list ap) {
    return vsscanf(input, format, ap);
}
CL int __isoc99_sscanf(const char* input, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    const int result = vsscanf(input, format, ap);
    va_end(ap);
    return result;
}
CL int __isoc99_vfscanf(FILE* stream, const char* format, va_list ap) {
    return vfscanf(stream, format, ap);
}
CL int __isoc99_fscanf(FILE* stream, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    const int result = vfscanf(stream, format, ap);
    va_end(ap);
    return result;
}
CL int __isoc99_vscanf(const char* format, va_list ap) { return vfscanf(stdin, format, ap); }
CL int __isoc99_scanf(const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    const int result = vfscanf(stdin, format, ap);
    va_end(ap);
    return result;
}
CL int __isoc99_vswscanf(const wchar_t* input, const wchar_t* format, va_list ap) {
    return vswscanf(input, format, ap);
}
CL int __isoc99_swscanf(const wchar_t* input, const wchar_t* format, ...) {
    va_list ap;
    va_start(ap, format);
    const int result = vswscanf(input, format, ap);
    va_end(ap);
    return result;
}

CL int __isoc23_vsscanf(const char* input, const char* format, va_list ap) {
    return vsscanf(input, format, ap);
}
CL int __isoc23_sscanf(const char* input, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    const int result = vsscanf(input, format, ap);
    va_end(ap);
    return result;
}
CL int __isoc23_vfscanf(FILE* stream, const char* format, va_list ap) {
    return vfscanf(stream, format, ap);
}
CL int __isoc23_fscanf(FILE* stream, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    const int result = vfscanf(stream, format, ap);
    va_end(ap);
    return result;
}
CL int __isoc23_vscanf(const char* format, va_list ap) { return vfscanf(stdin, format, ap); }
CL int __isoc23_scanf(const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    const int result = vfscanf(stdin, format, ap);
    va_end(ap);
    return result;
}
CL int __isoc23_vswscanf(const wchar_t* input, const wchar_t* format, va_list ap) {
    return vswscanf(input, format, ap);
}
CL int __isoc23_swscanf(const wchar_t* input, const wchar_t* format, ...) {
    va_list ap;
    va_start(ap, format);
    const int result = vswscanf(input, format, ap);
    va_end(ap);
    return result;
}
