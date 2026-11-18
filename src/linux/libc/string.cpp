// cordinal's own memory and string functions (the second stage of replacing glibc): mem*, str*,
// the wide wcs*/wmem* that cordinal's text uses, and _FORTIFY_SOURCE's checked copies. Hidden
// like kernel.cpp, so cordinal and the static libstdc++ use these and glibc keeps its own.
// The busy ones (memcpy, memmove, memset, memcmp, memchr, strlen, strchr, strcmp, strncmp) are
// written once in string_simd.inc and built twice, for SSE2 (every x86-64 processor) and AVX2;
// the program picks one as it loads (GNU ifunc, as glibc does), so a call costs one jump.
// `cordinal-core-test libc-bench` times them against glibc's. Large copies use aligned vector
// stores rather than `rep movsb`, which some processors (AMD's Zen 3) run slowly when the
// destination is unaligned.
// Case-insensitive comparisons fold through the running LC_CTYPE's narrow table, which is glibc's
// own rule (its __strcasecmp_l calls __tolower_l): the ASCII half of the table in the C and UTF-8
// locales, and an 8-bit locale's accented letters when its data is loaded. The other locale's
// functions (tolower, towlower, wcscasecmp) are cordinal's own since stage 5b (locale.cpp).
// Built with -fno-builtin and -fno-tree-loop-distribute-patterns (CMakeLists.txt), so the compiler
// never turns a loop here into a call to the function it is in.
#include "locale_data.h"

#include <immintrin.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

namespace {
typedef uint64_t __attribute__((may_alias, aligned(1))) U64;
typedef uint32_t __attribute__((may_alias, aligned(1))) U32;
typedef uint16_t __attribute__((may_alias, aligned(1))) U16;
typedef unsigned char Byte;

inline __m128i Load16(const void* p) { return _mm_loadu_si128((const __m128i*)p); }
inline void Store16(void* p, __m128i v) { _mm_storeu_si128((__m128i*)p, v); }

// Up to 16 bytes, by two overlapping loads of the widest size that fits, loaded before stored.
inline void CopySmall(Byte* d, const Byte* s, size_t n) {
    if (n >= 8) {
        const uint64_t a = *(const U64*)s, b = *(const U64*)(s + n - 8);
        *(U64*)d = a;
        *(U64*)(d + n - 8) = b;
    } else if (n >= 4) {
        const uint32_t a = *(const U32*)s, b = *(const U32*)(s + n - 4);
        *(U32*)d = a;
        *(U32*)(d + n - 4) = b;
    } else if (n >= 2) {
        const uint16_t a = *(const U16*)s, b = *(const U16*)(s + n - 2);
        *(U16*)d = a;
        *(U16*)(d + n - 2) = b;
    } else if (n) {
        *d = *s;
    }
}
inline void SetSmall(Byte* d, int c, size_t n) {
    const uint64_t b = (Byte)c * 0x0101010101010101ull;
    if (n >= 8) {
        *(U64*)d = b;
        *(U64*)(d + n - 8) = b;
    } else if (n >= 4) {
        *(U32*)d = (uint32_t)b;
        *(U32*)(d + n - 4) = (uint32_t)b;
    } else if (n >= 2) {
        *(U16*)d = (uint16_t)b;
        *(U16*)(d + n - 2) = (uint16_t)b;
    } else if (n) {
        *d = (Byte)b;
    }
}
// Under 16 bytes: big-endian words compare in memory order, so the first difference decides.
inline int CompareWords(uint64_t x, uint64_t y) { return x == y ? 0 : x < y ? -1 : 1; }
inline int CompareSmall(const Byte* a, const Byte* b, size_t n) {
    if (n >= 8) {
        if (const int r = CompareWords(__builtin_bswap64(*(const U64*)a), __builtin_bswap64(*(const U64*)b))) return r;
        return CompareWords(__builtin_bswap64(*(const U64*)(a + n - 8)), __builtin_bswap64(*(const U64*)(b + n - 8)));
    }
    if (n >= 4) {
        const uint64_t x = (uint64_t)__builtin_bswap32(*(const U32*)a) << 32 | __builtin_bswap32(*(const U32*)(a + n - 4));
        const uint64_t y = (uint64_t)__builtin_bswap32(*(const U32*)b) << 32 | __builtin_bswap32(*(const U32*)(b + n - 4));
        return CompareWords(x, y);
    }
    for (size_t i = 0; i < n; ++i)
        if (a[i] != b[i]) return (int)a[i] - (int)b[i];
    return 0;
}

// A set of bytes, for strspn and strcspn.
struct ByteSet {
    uint64_t bits[4] = {};
    explicit ByteSet(const char* chars) {
        for (const Byte* p = (const Byte*)chars; *p; ++p) bits[*p >> 6] |= 1ull << (*p & 63);
    }
    bool Has(Byte c) const { return bits[c >> 6] >> (c & 63) & 1; }
};
}  // namespace

namespace sse2 {
typedef __m128i V;
constexpr size_t kW = 16;
inline V Load(const void* p) { return _mm_loadu_si128((const __m128i*)p); }
inline V LoadA(const void* p) { return _mm_load_si128((const __m128i*)p); }
inline void Store(void* p, V v) { _mm_storeu_si128((__m128i*)p, v); }
inline void StoreA(void* p, V v) { _mm_store_si128((__m128i*)p, v); }
inline V Set1(int c) { return _mm_set1_epi8((char)c); }
inline V CmpEq(V a, V b) { return _mm_cmpeq_epi8(a, b); }
inline V Min(V a, V b) { return _mm_min_epu8(a, b); }
inline V Or(V a, V b) { return _mm_or_si128(a, b); }
inline V And(V a, V b) { return _mm_and_si128(a, b); }
inline V Xor(V a, V b) { return _mm_xor_si128(a, b); }
inline V Zero() { return _mm_setzero_si128(); }
inline uint32_t Mask(V v) { return (uint32_t)_mm_movemask_epi8(v); }
inline bool AllZero(V v) { return Mask(_mm_cmpeq_epi8(v, _mm_setzero_si128())) == 0xffff; }
#include "string_simd.inc"
}  // namespace sse2

#pragma GCC push_options
#pragma GCC target("avx2")
namespace avx2 {
typedef __m256i V;
constexpr size_t kW = 32;
inline V Load(const void* p) { return _mm256_loadu_si256((const __m256i*)p); }
inline V LoadA(const void* p) { return _mm256_load_si256((const __m256i*)p); }
inline void Store(void* p, V v) { _mm256_storeu_si256((__m256i*)p, v); }
inline void StoreA(void* p, V v) { _mm256_store_si256((__m256i*)p, v); }
inline V Set1(int c) { return _mm256_set1_epi8((char)c); }
inline V CmpEq(V a, V b) { return _mm256_cmpeq_epi8(a, b); }
inline V Min(V a, V b) { return _mm256_min_epu8(a, b); }
inline V Or(V a, V b) { return _mm256_or_si256(a, b); }
inline V And(V a, V b) { return _mm256_and_si256(a, b); }
inline V Xor(V a, V b) { return _mm256_xor_si256(a, b); }
inline V Zero() { return _mm256_setzero_si256(); }
inline uint32_t Mask(V v) { return (uint32_t)_mm256_movemask_epi8(v); }
inline bool AllZero(V v) { return _mm256_testz_si256(v, v); }
#include "string_simd.inc"
}  // namespace avx2
#pragma GCC pop_options

// Whether the processor and the kernel both support AVX2 (the kernel must save the vector
// registers: OSXSAVE, and XCR0's SSE and AVX state bits). Runs while the program is loading,
// before anything is initialized, so it asks the processor directly.
static bool HasAvx2() {
    unsigned a, b, c, d;
    __asm__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    if (!(c & (1u << 27)) || !(c & (1u << 28))) return false;
    unsigned lo, hi;
    __asm__("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    if ((lo & 6) != 6) return false;
    __asm__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0));
    return b & (1u << 5);
}

#define HIDE(name) __asm__(".hidden " #name);
HIDE(memcpy) HIDE(memmove) HIDE(memset) HIDE(memcmp) HIDE(memchr) HIDE(strlen) HIDE(strnlen) HIDE(strchr)
HIDE(strcmp) HIDE(strncmp) HIDE(strncpy) HIDE(strdup) HIDE(strspn) HIDE(strcspn) HIDE(strstr) HIDE(strcasecmp)
HIDE(strncasecmp) HIDE(explicit_bzero) HIDE(wcslen) HIDE(wcscmp) HIDE(wcschr) HIDE(wcscpy) HIDE(wcsncpy)
HIDE(wmemchr) HIDE(wmemcmp) HIDE(wmemcpy) HIDE(wmemmove) HIDE(wmemset) HIDE(__memcpy_chk) HIDE(__memmove_chk)
HIDE(__memset_chk) HIDE(__wmemcpy_chk) HIDE(__wmemset_chk)
#undef HIDE

// Each dispatched function: a resolver the dynamic loader calls once, and the ifunc symbol.
#define DISPATCH(type, name, impl, params)                                                         \
    extern "C" {                                                                                     \
    static type (*cordinal_resolve_##name(void)) params { return HasAvx2() ? avx2::impl : sse2::impl; } \
    type name params __attribute__((ifunc("cordinal_resolve_" #name)));                             \
    }
DISPATCH(void*, memcpy, Memcpy, (void* __restrict, const void* __restrict, size_t))
DISPATCH(void*, memmove, Memmove, (void*, const void*, size_t))
DISPATCH(void*, memset, Memset, (void*, int, size_t))
DISPATCH(int, memcmp, Memcmp, (const void*, const void*, size_t))
DISPATCH(const void*, memchr, Memchr, (const void*, int, size_t))
DISPATCH(size_t, strlen, Strlen, (const char*))
DISPATCH(const char*, strchr, Strchr, (const char*, int))
DISPATCH(int, strcmp, Strcmp, (const char*, const char*))
DISPATCH(int, strncmp, Strncmp, (const char*, const char*, size_t))
#undef DISPATCH

#include "string_variants.h"
extern "C" const CordinalStringFunctions* cordinal_string_functions(int avx2) {
#define BUILD(ns) {ns::Memcpy, ns::Memmove, ns::Memset, ns::Memcmp, ns::Memchr, ns::Strlen, ns::Strchr, ns::Strcmp, ns::Strncmp}
    static const CordinalStringFunctions sse2Build = BUILD(sse2), avx2Build = BUILD(avx2);
#undef BUILD
    if (!avx2) return &sse2Build;
    return HasAvx2() ? &avx2Build : nullptr;
}

extern "C" {

// A fill the compiler may not remove, for secrets.
void explicit_bzero(void* p, size_t n) {
    memset(p, 0, n);
    __asm__ volatile("" : : "r"(p) : "memory");
}

// C strings, built on the dispatched functions.
size_t strnlen(const char* s, size_t max) {
    const char* end = (const char*)memchr(s, 0, max);
    return end ? (size_t)(end - s) : max;
}
char* strncpy(char* __restrict d, const char* __restrict s, size_t n) {
    const size_t length = strnlen(s, n);
    memcpy(d, s, length);
    memset(d + length, 0, n - length);
    return d;
}
char* strdup(const char* s) {
    const size_t size = strlen(s) + 1;
    char* copy = (char*)malloc(size);
    return copy ? (char*)memcpy(copy, s, size) : nullptr;
}
size_t strspn(const char* s, const char* accept) {
    const ByteSet set(accept);
    size_t i = 0;
    while (s[i] && set.Has((Byte)s[i])) ++i;
    return i;
}
size_t strcspn(const char* s, const char* reject) {
    const ByteSet set(reject);
    size_t i = 0;
    while (s[i] && !set.Has((Byte)s[i])) ++i;
    return i;
}
char* strstr(const char* haystack, const char* needle) {
    if (!*needle) return (char*)haystack;
    const size_t length = strlen(needle);
    for (const char* p = strchr(haystack, *needle); p; p = strchr(p + 1, *needle))
        if (!strncmp(p, needle, length)) return (char*)p;
    return nullptr;
}
// The locale's byte fold, asked for once per string: glibc reads the same table in __strcasecmp_l.
int strcasecmp(const char* a, const char* b) {
    const int* lower = locale_data::NarrowToLowerTable();
    for (;; ++a, ++b) {
        const int diff = lower[(Byte)*a] - lower[(Byte)*b];
        if (diff || !*a) return diff;
    }
}
int strncasecmp(const char* a, const char* b, size_t n) {
    const int* lower = locale_data::NarrowToLowerTable();
    for (; n; --n, ++a, ++b) {
        const int diff = lower[(Byte)*a] - lower[(Byte)*b];
        if (diff || !*a) return diff;
    }
    return 0;
}

// Wide strings: wchar_t is a signed 32-bit value on x86-64 Linux, compared as such.
size_t wcslen(const wchar_t* s) {
    if ((uintptr_t)s & 3) {  // never from cordinal; kept correct anyway
        size_t n = 0;
        while (s[n]) ++n;
        return n;
    }
    const size_t offset = (uintptr_t)s & 15;
    const char* block = (const char*)s - offset;
    const __m128i zero = _mm_setzero_si128();
    unsigned m = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi32(_mm_load_si128((const __m128i*)block), zero)) >> offset;
    if (m) return __builtin_ctz(m) / 4;
    for (;;) {
        block += 16;
        m = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi32(_mm_load_si128((const __m128i*)block), zero));
        if (m) return ((size_t)(block - (const char*)s) + __builtin_ctz(m)) / 4;
    }
}
int wcscmp(const wchar_t* a, const wchar_t* b) {
    for (;; ++a, ++b) {
        if (*a != *b) return *a < *b ? -1 : 1;
        if (!*a) return 0;
    }
}
wchar_t* wcschr(const wchar_t* s, wchar_t c) {
    for (;; ++s) {
        if (*s == c) return (wchar_t*)s;
        if (!*s) return nullptr;
    }
}
wchar_t* wmemchr(const wchar_t* s, wchar_t c, size_t n) {
    for (; n; --n, ++s)
        if (*s == c) return (wchar_t*)s;
    return nullptr;
}
int wmemcmp(const wchar_t* a, const wchar_t* b, size_t n) {
    for (; n; --n, ++a, ++b)
        if (*a != *b) return *a < *b ? -1 : 1;
    return 0;
}
wchar_t* wmemcpy(wchar_t* __restrict d, const wchar_t* __restrict s, size_t n) {
    return (wchar_t*)memcpy(d, s, n * sizeof(wchar_t));
}
wchar_t* wmemmove(wchar_t* d, const wchar_t* s, size_t n) { return (wchar_t*)memmove(d, s, n * sizeof(wchar_t)); }
wchar_t* wmemset(wchar_t* d, wchar_t c, size_t n) {
    const __m128i v = _mm_set1_epi32((int)c);
    size_t i = 0;
    for (; i + 4 <= n; i += 4) Store16(d + i, v);
    for (; i < n; ++i) d[i] = c;
    return d;
}
wchar_t* wcscpy(wchar_t* __restrict d, const wchar_t* __restrict s) { return wmemcpy(d, s, wcslen(s) + 1); }
wchar_t* wcsncpy(wchar_t* __restrict d, const wchar_t* __restrict s, size_t n) {
    size_t length = 0;
    while (length < n && s[length]) ++length;
    wmemcpy(d, s, length);
    wmemset(d + length, 0, n - length);
    return d;
}

// _FORTIFY_SOURCE's checked versions: a destination smaller than the request ends the program.
void* __memcpy_chk(void* d, const void* s, size_t n, size_t size) {
    if (n > size) abort();
    return memcpy(d, s, n);
}
void* __memmove_chk(void* d, const void* s, size_t n, size_t size) {
    if (n > size) abort();
    return memmove(d, s, n);
}
void* __memset_chk(void* d, int c, size_t n, size_t size) {
    if (n > size) abort();
    return memset(d, c, n);
}
wchar_t* __wmemcpy_chk(wchar_t* d, const wchar_t* s, size_t n, size_t size) {
    if (n > size) abort();
    return wmemcpy(d, s, n);
}
wchar_t* __wmemset_chk(wchar_t* d, wchar_t c, size_t n, size_t size) {
    if (n > size) abort();
    return wmemset(d, c, n);
}

}  // extern "C"
