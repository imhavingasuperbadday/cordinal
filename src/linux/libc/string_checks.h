// cordinal's memory and string functions (string.cpp) against glibc's own, for checks.h: every
// size to 300 and larger ones, every alignment, overlapping moves both ways, and strings that end
// against an unreadable page, where reading one byte too far would crash. Bench() times both.
#pragma once
#include <dlfcn.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <time.h>
#include <wchar.h>

#include "string_variants.h"

#include <cstdio>
#include <cstdlib>

namespace libc_checks {

// glibc's copy of a function, which dlsym finds because cordinal's are not exported.
template <class F> inline F Glibc(const char* name, F) { return (F)dlsym(RTLD_DEFAULT, name); }
inline int Sign(long x) { return (x > 0) - (x < 0); }

struct Random {
    uint64_t state;
    uint64_t Next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }
    // Bytes from a small alphabet with zeros, so matches, differences and ends are all common.
    unsigned char Byte() { return "\0aAbB\x80\xff z"[Next() % 9]; }
};

// Memory with an unreadable page after it: Edge(n) is the last n bytes before that page.
struct Guarded {
    static constexpr size_t kSize = 65536;
    unsigned char* base = nullptr;
    Guarded() {
        void* p = mmap(nullptr, kSize + 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) return;
        base = (unsigned char*)p;
        mprotect(base + kSize, 4096, PROT_NONE);
    }
    ~Guarded() {
        if (base) munmap(base, kSize + 4096);
    }
    unsigned char* Edge(size_t n) const { return base + kSize - n; }
};

inline void CheckStrings() {
    Expect(!InProgram((const void*)Glibc("memcpy", &memcpy)) && InProgram((const void*)&memcpy) && InProgram((const void*)&strlen),
           "memcpy and strlen are cordinal's, glibc's are still found for comparison");
    auto gMemmove = Glibc("memmove", &memmove);
    auto gMemcmp = Glibc("memcmp", &memcmp);
    auto gMemchr = Glibc("memchr", (const void* (*)(const void*, int, size_t))nullptr);
    auto gStrcmp = Glibc("strcmp", &strcmp);
    auto gStrncmp = Glibc("strncmp", &strncmp);
    auto gStrchr = Glibc("strchr", (const char* (*)(const char*, int))nullptr);
    auto gStrstr = Glibc("strstr", (const char* (*)(const char*, const char*))nullptr);
    auto gStrspn = Glibc("strspn", &strspn);
    auto gStrcspn = Glibc("strcspn", &strcspn);
    auto gStrcasecmp = Glibc("strcasecmp", &strcasecmp);
    auto gWcscmp = Glibc("wcscmp", &wcscmp);
    auto gWmemcmp = Glibc("wmemcmp", &wmemcmp);
    if (!gMemmove || !gMemcmp || !gMemchr || !gStrcmp || !gStrncmp || !gStrchr || !gStrstr || !gStrspn || !gStrcspn ||
        !gStrcasecmp || !gWcscmp || !gWmemcmp) {
        Expect(false, "glibc's string functions are found");
        return;
    }
    Guarded g;
    if (!g.base) {
        Expect(false, "guarded memory");
        return;
    }
    Random r{0x9E3779B97F4A7C15ull};
    static unsigned char a[9000], b[9000], expect[9000];
    size_t sizes[340];
    size_t count = 0;
    for (size_t n = 0; n <= 300; ++n) sizes[count++] = n;
    for (size_t n : {383, 511, 512, 513, 1000, 2047, 2048, 2049, 2050, 4095, 4096, 5000, 8000}) sizes[count++] = n;
    int bad = 0;
    auto Fail = [&](const char* what, size_t n, size_t x, size_t y) {
        if (bad++ < 10) fprintf(stderr, "FAIL %s: size %zu, offsets %zu and %zu\n", what, n, x, y);
    };

    // The functions that come in two builds, run through the given build.
    auto gMemcpy = Glibc("memcpy", &memcpy);
    auto RunBuild = [&](const CordinalStringFunctions& f) {
        for (size_t k = 0; k < count; ++k) {
            const size_t n = sizes[k];
            for (size_t x = 0; x < 16; x += (n > 300 ? 5 : 1)) {
                for (size_t y = 0; y < 16; y += (n > 300 ? 7 : 3)) {
                    for (size_t i = 0; i < n + 64; ++i) a[i] = r.Byte(), b[i] = (unsigned char)r.Next();
                    // memcpy and memset touch exactly their n bytes.
                    gMemcpy(expect, b, n + 64);
                    gMemcpy(expect + y + 16, a + x, n);
                    f.memcpy(b + y + 16, a + x, n);
                    if (gMemcmp(b, expect, n + 64)) Fail("memcpy", n, x, y);
                    gMemcpy(expect, b, n + 64);
                    for (size_t i = 0; i < n; ++i) expect[y + 16 + i] = (unsigned char)x;
                    f.memset(b + y + 16, (int)x + 256, n);
                    if (gMemcmp(b, expect, n + 64)) Fail("memset", n, x, y);
                    // memcmp: equal, then different at one place.
                    gMemcpy(b + y, a + x, n);
                    if (f.memcmp(a + x, b + y, n)) Fail("memcmp of equal bytes", n, x, y);
                    if (n) {
                        const size_t at = r.Next() % n;
                        b[y + at] ^= (unsigned char)(1 + r.Next() % 255);
                        if (Sign(f.memcmp(a + x, b + y, n)) != Sign(gMemcmp(a + x, b + y, n))) Fail("memcmp", n, x, at);
                    }
                    // memchr for a byte that may or may not be there.
                    const int c = a[x + (n ? r.Next() % n : 0)] ^ (int)(r.Next() & 1);
                    if (f.memchr(a + x, c, n) != gMemchr(a + x, c, n)) Fail("memchr", n, x, (size_t)c);
                }
                // memmove both ways, at distances that do and don't overlap.
                for (long shift : {-100L, -33L, -32L, -17L, -16L, -1L, 1L, 3L, 15L, 16L, 31L, 32L, 33L, 64L, 100L}) {
                    for (size_t i = 0; i < n + 256; ++i) a[i] = (unsigned char)r.Next();
                    gMemcpy(b, a, n + 256);
                    const size_t from = 112 + x, to = (size_t)((long)from + shift);
                    f.memmove(a + to, a + from, n);
                    gMemmove(b + to, b + from, n);
                    if (gMemcmp(a, b, n + 256)) Fail("memmove", n, from, to);
                }
            }
            // Strings that end against the unreadable page: a crash here is a read past the end.
            for (size_t x = 0; x < 16 && n + 1 + x < Guarded::kSize; ++x) {
                unsigned char* s = g.Edge(n + 1 + x);
                for (size_t i = 0; i < n; ++i) s[i] = (unsigned char)(1 + r.Next() % 255);
                s[n] = 0;
                for (size_t i = 0; i < x; ++i) s[n + 1 + i] = (unsigned char)(1 + r.Next() % 255);
                if (f.strlen((const char*)s) != n) Fail("strlen", n, x, 0);
                if (strnlen((const char*)s, n + 1 + x) != n || strnlen((const char*)s, n / 2) != n / 2) Fail("strnlen", n, x, 0);
                const int c = n ? s[r.Next() % n] : 'q';
                if (f.strchr((const char*)s, c) != gStrchr((const char*)s, c) || f.strchr((const char*)s, 0) != (const char*)s + n)
                    Fail("strchr", n, x, (size_t)c);
                unsigned char* t = g.base + 1024 + x;
                gMemcpy(t, s, n + 1);
                if (f.strcmp((const char*)s, (const char*)t) || f.strncmp((const char*)s, (const char*)t, n + 5))
                    Fail("strcmp of equal strings", n, x, 0);
                if (n) {
                    const size_t at = r.Next() % n;
                    t[at] = (unsigned char)(t[at] == 0xff ? 1 : t[at] + 1);
                    if (Sign(f.strcmp((const char*)s, (const char*)t)) != Sign(gStrcmp((const char*)s, (const char*)t)) ||
                        Sign(f.strcmp((const char*)t, (const char*)s)) != Sign(gStrcmp((const char*)t, (const char*)s)))
                        Fail("strcmp", n, x, at);
                    const size_t limit = r.Next() % (n + 2);
                    if (Sign(f.strncmp((const char*)s, (const char*)t, limit)) != Sign(gStrncmp((const char*)s, (const char*)t, limit)))
                        Fail("strncmp", n, x, limit);
                }
                if (f.memchr(s, 0, n + 1) != s + n) Fail("memchr finds the end", n, x, 0);
            }
        }
    };
    // The functions the program calls (whichever build it picked as it loaded), then each build.
    const CordinalStringFunctions called = {&memcpy, &memmove, &memset, &memcmp,
                                            (const void* (*)(const void*, int, size_t))&memchr, &strlen,
                                            (const char* (*)(const char*, int))&strchr, &strcmp, &strncmp};
    RunBuild(called);
    RunBuild(*cordinal_string_functions(0));
    if (const CordinalStringFunctions* avx2 = cordinal_string_functions(1)) RunBuild(*avx2);
    else printf("note: this processor has no AVX2, so only the SSE2 build was checked\n");
    // Searching, spans and case: short strings from a small alphabet, against glibc.
    char h[64], needle[8], set[6], other[64];
    for (int round = 0; round < 20000; ++round) {
        const size_t hn = r.Next() % 63, nn = r.Next() % 7, sn = r.Next() % 5;
        for (size_t i = 0; i < hn; ++i) h[i] = "abcAB"[r.Next() % 5];
        h[hn] = 0;
        for (size_t i = 0; i < nn; ++i) needle[i] = "abcAB"[r.Next() % 5];
        needle[nn] = 0;
        for (size_t i = 0; i < sn; ++i) set[i] = "abcAB"[r.Next() % 5];
        set[sn] = 0;
        for (size_t i = 0; i <= hn; ++i) other[i] = (char)(r.Next() & 1 ? h[i] ^ 0x20 * (h[i] >= 'A') : h[i]);
        if (strstr(h, needle) != gStrstr(h, needle)) Fail("strstr", hn, nn, 0);
        if (strspn(h, set) != gStrspn(h, set) || strcspn(h, set) != gStrcspn(h, set)) Fail("strspn or strcspn", hn, sn, 0);
        if (Sign(strcasecmp(h, other)) != Sign(gStrcasecmp(h, other)) || Sign(strcasecmp(h, needle)) != Sign(gStrcasecmp(h, needle)))
            Fail("strcasecmp", hn, 0, 0);
        if (strncasecmp(h, other, hn) != 0) Fail("strncasecmp", hn, 0, 0);
    }
    // Wide strings, with values on both sides of zero.
    wchar_t wa[80], wb[80];
    for (int round = 0; round < 20000; ++round) {
        const size_t n = r.Next() % 70;
        for (size_t i = 0; i < n; ++i) wa[i] = (wchar_t)((r.Next() % 3 == 0) ? -(long)(r.Next() % 5) - 1 : (long)(r.Next() % 5) + 1);
        wa[n] = 0;
        wmemcpy(wb, wa, n + 1);
        if (wcslen(wa) != n || wcscmp(wa, wb) || wmemcmp(wa, wb, n)) Fail("wcslen, wcscmp or wmemcmp of equal strings", n, 0, 0);
        if (n) {
            const size_t at = r.Next() % n;
            wb[at] = wb[at] == 5 ? -5 : wb[at] + 1;
            if (!wb[at]) wb[at] = 2;
            if (Sign(wcscmp(wa, wb)) != Sign(gWcscmp(wa, wb)) || Sign(wmemcmp(wa, wb, n)) != Sign(gWmemcmp(wa, wb, n)))
                Fail("wcscmp or wmemcmp", n, at, 0);
            const wchar_t c = wa[r.Next() % n];
            const wchar_t* found = wcschr(wa, c);
            if (!found || *found != c || wmemchr(wa, c, n) != found) Fail("wcschr or wmemchr", n, 0, 0);
        }
        if (wcschr(wa, 0) != wa + n || wcschr(wa, 99) || wmemchr(wa, 99, n)) Fail("wcschr at the end", n, 0, 0);
        wmemset(wb, 7, n);
        for (size_t i = 0; i < n; ++i)
            if (wb[i] != 7) Fail("wmemset", n, i, 0);
        wcsncpy(wb, wa, n + 5);
        if (wcscmp(wa, wb) || wb[n + 4] != 0) Fail("wcsncpy", n, 0, 0);
        wcscpy(wb, wa);
        if (wcscmp(wa, wb)) Fail("wcscpy", n, 0, 0);
    }
    char small[8];
    strncpy(small, "abc", sizeof small);
    char* copy = strdup("cordinal");
    Expect(!memcmp(small, "abc\0\0\0\0\0", 8) && copy && !strcmp(copy, "cordinal"), "strncpy pads and strdup copies");
    free(copy);
    Expect(bad == 0, "memory and string functions match glibc's");
}

// Average time of one call in nanoseconds, over runs of at least 3 ms.
template <class F> inline double Time(F call) {
    size_t rounds = 1;
    for (;;) {
        timespec start{}, end{};
        clock_gettime(CLOCK_MONOTONIC, &start);
        for (size_t i = 0; i < rounds; ++i) call();
        clock_gettime(CLOCK_MONOTONIC, &end);
        const double ns = (end.tv_sec - start.tv_sec) * 1e9 + (end.tv_nsec - start.tv_nsec);
        if (ns > 3e6) return ns / rounds;
        rounds *= 2;
    }
}
// Both, taking turns seven times, and each one's best: noise from the machine only ever adds time.
template <class F, class G> inline void TimeBoth(F ours, G theirs, double& ourBest, double& theirBest) {
    ourBest = theirBest = 1e300;
    for (int i = 0; i < 7; ++i) {
        const double a = Time(ours), b = Time(theirs);
        if (a < ourBest) ourBest = a;
        if (b < theirBest) theirBest = b;
    }
}

// Times cordinal's and glibc's functions side by side; glibc picks its fastest for this processor.
inline int Bench() {
    static unsigned char x[1 << 21], y[1 << 21];
    for (size_t i = 0; i < sizeof x; ++i) x[i] = y[i] = (unsigned char)(1 + i % 251);
    void* (*volatile ourCopy)(void*, const void*, size_t) = &memcpy;
    void* (*volatile ourMove)(void*, const void*, size_t) = &memmove;
    void* (*volatile ourSet)(void*, int, size_t) = &memset;
    int (*volatile ourCmp)(const void*, const void*, size_t) = &memcmp;
    size_t (*volatile ourLen)(const char*) = &strlen;
    int (*volatile ourStrcmp)(const char*, const char*) = &strcmp;
    const void* (*volatile ourChr)(const void*, int, size_t) = (const void* (*)(const void*, int, size_t))&memchr;
    void* (*volatile theirCopy)(void*, const void*, size_t) = Glibc("memcpy", &memcpy);
    void* (*volatile theirMove)(void*, const void*, size_t) = Glibc("memmove", &memmove);
    void* (*volatile theirSet)(void*, int, size_t) = Glibc("memset", &memset);
    int (*volatile theirCmp)(const void*, const void*, size_t) = Glibc("memcmp", &memcmp);
    size_t (*volatile theirLen)(const char*) = Glibc("strlen", &strlen);
    int (*volatile theirStrcmp)(const char*, const char*) = Glibc("strcmp", &strcmp);
    const void* (*volatile theirChr)(const void*, int, size_t) = Glibc("memchr", (const void* (*)(const void*, int, size_t))nullptr);
    // Half a second of copying first, so the processor is at full speed for the first row.
    Time([&] { ourCopy(y, x, sizeof x / 2); });
    Time([&] { theirCopy(y, x, sizeof x / 2); });
    printf("%-9s %8s %10s %10s %7s\n", "function", "bytes", "cordinal", "glibc", "ratio");
    double worst = 0;
    auto Row = [&](const char* name, size_t n, auto ours, auto theirs) {
        double a, b;
        TimeBoth(ours, theirs, a, b);
        printf("%-9s %8zu %8.1fns %8.1fns %6.2fx\n", name, n, a, b, a / b);
        if (a / b > worst) worst = a / b;
    };
    for (size_t n : {8, 16, 31, 64, 100, 256, 1000, 4096, 65536, 1 << 20}) {
        Row("memcpy", n, [&] { ourCopy(y + 1, x, n); }, [&] { theirCopy(y + 1, x, n); });
        Row("memmove", n, [&] { ourMove(x + 3, x, n); }, [&] { theirMove(x + 3, x, n); });
        Row("memset", n, [&] { ourSet(y, 7, n); }, [&] { theirSet(y, 7, n); });
        for (size_t i = 0; i < sizeof x; ++i) x[i] = y[i] = (unsigned char)(1 + i % 251);
        Row("memcmp", n, [&] { ourCmp(x, y, n); }, [&] { theirCmp(x, y, n); });
        Row("memchr", n, [&] { ourChr(x, 0, n); }, [&] { theirChr(x, 0, n); });
        x[n] = y[n] = 0;
        Row("strlen", n, [&] { ourLen((const char*)x); }, [&] { theirLen((const char*)x); });
        Row("strcmp", n, [&] { ourStrcmp((const char*)x, (const char*)y); }, [&] { theirStrcmp((const char*)x, (const char*)y); });
        x[n] = y[n] = (unsigned char)(1 + n % 251);
    }
    printf("slowest against glibc: %.2fx\n", worst);
    return 0;
}

}  // namespace libc_checks
