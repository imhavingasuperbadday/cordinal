// The vector half of string.cpp, included twice: once with 16-byte SSE2 vectors (every x86-64
// processor) and once with 32-byte AVX2 vectors, chosen when the program loads. The including
// namespace defines V, kW, Load, LoadA, Store, StoreA, Set1, CmpEq, Min, Or, And, Zero and Mask.
// Large copies and fills align their stores and move four vectors a step; scans check once per
// four vectors. Scans of unknown length read aligned blocks inside a 4*kW-aligned window, which
// never crosses a page, so reading past a string's end cannot fault.

constexpr uint32_t kFull = kW == 32 ? 0xffffffffu : 0xffffu;

// Up to 4*kW bytes, everything loaded before anything is stored (so overlap is fine).
inline bool CopyFew(Byte* d, const Byte* s, size_t n) {
    if (n <= 16) {
        CopySmall(d, s, n);
    } else if (n <= 32) {
        const __m128i a = Load16(s), b = Load16(s + n - 16);
        Store16(d, a);
        Store16(d + n - 16, b);
    } else if (n <= 2 * kW) {
        const V a = Load(s), b = Load(s + n - kW);
        Store(d, a);
        Store(d + n - kW, b);
    } else if (n <= 4 * kW) {
        const V a = Load(s), b = Load(s + kW), c = Load(s + n - 2 * kW), e = Load(s + n - kW);
        Store(d, a);
        Store(d + kW, b);
        Store(d + n - 2 * kW, c);
        Store(d + n - kW, e);
    } else {
        return false;
    }
    return true;
}
// Front to back, n > 4*kW, aligned stores. The first vector and the last four are loaded before
// the loop, so this is also right when the destination overlaps the source from below.
inline void CopyForward(Byte* d, const Byte* s, size_t n) {
    const V head = Load(s);
    const V t0 = Load(s + n - 4 * kW), t1 = Load(s + n - 3 * kW), t2 = Load(s + n - 2 * kW), t3 = Load(s + n - kW);
    const size_t skip = kW - ((uintptr_t)d & (kW - 1));
    Byte* dp = d + skip;
    const Byte* sp = s + skip;
    size_t left = n - skip;
    while (left > 4 * kW) {
        const V a = Load(sp), b = Load(sp + kW), c = Load(sp + 2 * kW), e = Load(sp + 3 * kW);
        StoreA(dp, a);
        StoreA(dp + kW, b);
        StoreA(dp + 2 * kW, c);
        StoreA(dp + 3 * kW, e);
        dp += 4 * kW;
        sp += 4 * kW;
        left -= 4 * kW;
    }
    Store(d + n - 4 * kW, t0);
    Store(d + n - 3 * kW, t1);
    Store(d + n - 2 * kW, t2);
    Store(d + n - kW, t3);
    Store(d, head);
}
// Back to front, n > 4*kW, for a destination overlapping the source from above.
inline void CopyBackward(Byte* d, const Byte* s, size_t n) {
    const V h0 = Load(s), h1 = Load(s + kW), h2 = Load(s + 2 * kW), h3 = Load(s + 3 * kW);
    const V tail = Load(s + n - kW);
    const size_t skip = (uintptr_t)(d + n) & (kW - 1);
    size_t left = n - skip;  // bytes before the aligned end
    while (left > 4 * kW) {
        left -= 4 * kW;
        const V a = Load(s + left), b = Load(s + left + kW), c = Load(s + left + 2 * kW), e = Load(s + left + 3 * kW);
        StoreA(d + left, a);
        StoreA(d + left + kW, b);
        StoreA(d + left + 2 * kW, c);
        StoreA(d + left + 3 * kW, e);
    }
    Store(d + n - kW, tail);
    Store(d, h0);
    Store(d + kW, h1);
    Store(d + 2 * kW, h2);
    Store(d + 3 * kW, h3);
}

void* Memcpy(void* __restrict dst, const void* __restrict src, size_t n) {
    if (!CopyFew((Byte*)dst, (const Byte*)src, n)) CopyForward((Byte*)dst, (const Byte*)src, n);
    return dst;
}
void* Memmove(void* dst, const void* src, size_t n) {
    Byte* d = (Byte*)dst;
    const Byte* s = (const Byte*)src;
    if (d == s || CopyFew(d, s, n)) return dst;
    if ((uintptr_t)d - (uintptr_t)s >= n) CopyForward(d, s, n);  // below the source, or apart
    else CopyBackward(d, s, n);
    return dst;
}
void* Memset(void* dst, int c, size_t n) {
    Byte* d = (Byte*)dst;
    if (n <= 16) {
        SetSmall(d, c, n);
        return dst;
    }
    if (n <= 32) {
        const __m128i v = _mm_set1_epi8((char)c);
        Store16(d, v);
        Store16(d + n - 16, v);
        return dst;
    }
    const V v = Set1(c);
    if (n <= 2 * kW) {
        Store(d, v);
        Store(d + n - kW, v);
    } else if (n <= 4 * kW) {
        Store(d, v);
        Store(d + kW, v);
        Store(d + n - 2 * kW, v);
        Store(d + n - kW, v);
    } else {
        Store(d, v);
        Byte* p = (Byte*)(((uintptr_t)d + kW) & ~(uintptr_t)(kW - 1));
        Byte* const end = d + n;
        Byte* const last = end - 4 * kW;
        while (p < last) {
            StoreA(p, v);
            StoreA(p + kW, v);
            StoreA(p + 2 * kW, v);
            StoreA(p + 3 * kW, v);
            p += 4 * kW;
        }
        Store(end - 4 * kW, v);
        Store(end - 3 * kW, v);
        Store(end - 2 * kW, v);
        Store(end - kW, v);
    }
    return dst;
}

inline int DiffAt(const void* a, const void* b, size_t i) { return (int)((const Byte*)a)[i] - (int)((const Byte*)b)[i]; }
// Bytes readable from p without leaving its page.
inline size_t Room(const void* p) { return 4096 - ((uintptr_t)p & 4095); }
// Four vectors at ascending offsets (they may overlap): the first difference, or 0. A difference
// found in one vector is the first, since any earlier one would sit in a vector checked before it.
// Equal vectors XOR to zero, so one test covers all four; only a difference needs the masks.
inline int Diff4(const Byte* a, const Byte* b, size_t o0, size_t o1, size_t o2, size_t o3) {
    const V d0 = Xor(Load(a + o0), Load(b + o0)), d1 = Xor(Load(a + o1), Load(b + o1));
    const V d2 = Xor(Load(a + o2), Load(b + o2)), d3 = Xor(Load(a + o3), Load(b + o3));
    if (AllZero(Or(Or(d0, d1), Or(d2, d3)))) return 0;
    const V zero = Zero();
    uint32_t m;
    if ((m = Mask(CmpEq(d0, zero)) ^ kFull)) return DiffAt(a, b, o0 + __builtin_ctz(m));
    if ((m = Mask(CmpEq(d1, zero)) ^ kFull)) return DiffAt(a, b, o1 + __builtin_ctz(m));
    if ((m = Mask(CmpEq(d2, zero)) ^ kFull)) return DiffAt(a, b, o2 + __builtin_ctz(m));
    m = Mask(CmpEq(d3, zero)) ^ kFull;
    return DiffAt(a, b, o3 + __builtin_ctz(m));
}
int Memcmp(const void* pa, const void* pb, size_t n) {
    const Byte* a = (const Byte*)pa;
    const Byte* b = (const Byte*)pb;
    if (n < 16) return CompareSmall(a, b, n);
    if (n < kW) {  // 16 to 31 bytes with 32-byte vectors: two overlapping 16-byte compares
        uint32_t m = (uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(Load16(a), Load16(b))) ^ 0xffffu;
        if (m) return DiffAt(a, b, __builtin_ctz(m));
        m = (uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(Load16(a + n - 16), Load16(b + n - 16))) ^ 0xffffu;
        return m ? DiffAt(a, b, n - 16 + __builtin_ctz(m)) : 0;
    }
    if (n <= 2 * kW) {
        uint32_t m = Mask(CmpEq(Load(a), Load(b))) ^ kFull;
        if (m) return DiffAt(a, b, __builtin_ctz(m));
        m = Mask(CmpEq(Load(a + n - kW), Load(b + n - kW))) ^ kFull;
        return m ? DiffAt(a, b, n - kW + __builtin_ctz(m)) : 0;
    }
    if (n <= 4 * kW) return Diff4(a, b, 0, kW, n - 2 * kW, n - kW);
    if (n <= 8 * kW) {
        if (const int r = Diff4(a, b, 0, kW, 2 * kW, 3 * kW)) return r;
        return Diff4(a, b, n - 4 * kW, n - 3 * kW, n - 2 * kW, n - kW);
    }
    size_t i = 0;
    for (; i + 4 * kW <= n; i += 4 * kW)
        if (const int r = Diff4(a, b, i, i + kW, i + 2 * kW, i + 3 * kW)) return r;
    return i < n ? Diff4(a, b, n - 4 * kW, n - 3 * kW, n - 2 * kW, n - kW) : 0;
}

const void* Memchr(const void* src, int c, size_t n) {
    if (!n) return nullptr;
    const Byte* start = (const Byte*)src;
    const size_t offset = (uintptr_t)start & (kW - 1);
    const Byte* block = start - offset;
    const V v = Set1(c);
    uint32_t m = Mask(CmpEq(LoadA(block), v)) >> offset;
    if (m) return __builtin_ctz(m) < n ? start + __builtin_ctz(m) : nullptr;
    block += kW;
    // Four blocks a step while all four lie inside the range, then one at a time.
    while ((size_t)(block - start) + 4 * kW <= n) {
        const V e0 = CmpEq(LoadA(block), v), e1 = CmpEq(LoadA(block + kW), v);
        const V e2 = CmpEq(LoadA(block + 2 * kW), v), e3 = CmpEq(LoadA(block + 3 * kW), v);
        if (Mask(Or(Or(e0, e1), Or(e2, e3)))) {
            const V e[4] = {e0, e1, e2, e3};
            for (int k = 0; k < 4; ++k)
                if (const uint32_t mk = Mask(e[k])) return block + k * kW + __builtin_ctz(mk);
        }
        block += 4 * kW;
    }
    for (; (size_t)(block - start) < n; block += kW) {
        if ((m = Mask(CmpEq(LoadA(block), v)))) {
            const size_t i = (size_t)(block - start) + __builtin_ctz(m);
            return i < n ? start + i : nullptr;
        }
    }
    return nullptr;
}

// The first position in s where `Hit` marks a byte, without a length, as glibc does it: one
// unaligned vector, four aligned ones, then four at a time in 4*kW-aligned windows (which never
// cross a page). Each aligned block is read only after the one before had no end in it.
// Used by strlen (the end) and strchr (the end or the character).
template <class HitFn> inline const char* Scan(const char* s, HitFn Hit) {
    uint32_t m;
    const char* block;
    if (Room(s) >= kW) {
        if ((m = Mask(Hit(Load(s))))) return s + __builtin_ctz(m);
        block = (const char*)(((uintptr_t)s | (kW - 1)) + 1);
    } else {
        const size_t offset = (uintptr_t)s & (kW - 1);
        block = s - offset;
        if ((m = Mask(Hit(LoadA(block))) >> offset)) return s + __builtin_ctz(m);
        block += kW;
    }
    if ((m = Mask(Hit(LoadA(block))))) return block + __builtin_ctz(m);
    if ((m = Mask(Hit(LoadA(block + kW))))) return block + kW + __builtin_ctz(m);
    if ((m = Mask(Hit(LoadA(block + 2 * kW))))) return block + 2 * kW + __builtin_ctz(m);
    if ((m = Mask(Hit(LoadA(block + 3 * kW))))) return block + 3 * kW + __builtin_ctz(m);
    block = (const char*)((uintptr_t)(block + 4 * kW) & ~(uintptr_t)(4 * kW - 1));
    for (;; block += 4 * kW) {
        const V e0 = Hit(LoadA(block)), e1 = Hit(LoadA(block + kW)), e2 = Hit(LoadA(block + 2 * kW)), e3 = Hit(LoadA(block + 3 * kW));
        if (Mask(Or(Or(e0, e1), Or(e2, e3)))) {
            const V e[4] = {e0, e1, e2, e3};
            for (int k = 0; k < 4; ++k)
                if (const uint32_t mk = Mask(e[k])) return block + k * kW + __builtin_ctz(mk);
        }
    }
}
size_t Strlen(const char* s) {
    const V zero = Zero();
    return (size_t)(Scan(s, [&](V x) { return CmpEq(x, zero); }) - s);
}
const char* Strchr(const char* s, int c) {
    const V zero = Zero(), v = Set1(c);
    const char* p = Scan(s, [&](V x) { return Or(CmpEq(x, v), CmpEq(x, zero)); });
    return *p == (char)c ? p : nullptr;
}

// strcmp and strncmp: zero where a string ends or the two differ (min(x, x == y ? 0xff : 0)).
inline V EndOrDiff(V x, V y) { return Min(x, CmpEq(x, y)); }
int Strncmp(const char* a, const char* b, size_t n) {
    const V zero = Zero();
    while (n) {
        const size_t room = Room(a) < Room(b) ? Room(a) : Room(b);
        if (n >= kW && room >= kW) {
            // Short strings end in the first few vectors; longer ones go on four vectors a step, as
            // far as both pages allow.
            size_t singles = 0;
            for (; singles < 4 && n >= kW && room >= (singles + 1) * kW; ++singles) {
                if (const uint32_t m = Mask(CmpEq(EndOrDiff(Load(a), Load(b)), zero))) return DiffAt(a, b, __builtin_ctz(m));
                a += kW;
                b += kW;
                n -= kW;
            }
            size_t steps = (room - singles * kW) / (4 * kW);
            if (steps > n / (4 * kW)) steps = n / (4 * kW);
            for (; steps; --steps) {
                const V t0 = EndOrDiff(Load(a), Load(b)), t1 = EndOrDiff(Load(a + kW), Load(b + kW));
                const V t2 = EndOrDiff(Load(a + 2 * kW), Load(b + 2 * kW)), t3 = EndOrDiff(Load(a + 3 * kW), Load(b + 3 * kW));
                if (Mask(CmpEq(Min(Min(t0, t1), Min(t2, t3)), zero))) {
                    const V t[4] = {t0, t1, t2, t3};
                    for (int k = 0; k < 4; ++k)
                        if (const uint32_t m = Mask(CmpEq(t[k], zero))) return DiffAt(a, b, k * kW + __builtin_ctz(m));
                }
                a += 4 * kW;
                b += 4 * kW;
                n -= 4 * kW;
            }
        } else {  // near the end of a page, or the last few bytes of n: one byte at a time
            const int diff = (int)(Byte)*a - (int)(Byte)*b;
            if (diff || !*a) return diff;
            ++a;
            ++b;
            --n;
        }
    }
    return 0;
}
// strcmp without a length: one vector, then (with room) three more, then four at a time as far
// as both pages allow; a byte at a time only in the last vector's width of a page.
int Strcmp(const char* a, const char* b) {
    const V zero = Zero();
    for (;;) {
        size_t room = Room(a) < Room(b) ? Room(a) : Room(b);
        if (room < kW) {
            const int diff = (int)(Byte)*a - (int)(Byte)*b;
            if (diff || !*a) return diff;
            ++a;
            ++b;
            continue;
        }
        uint32_t m;
        if ((m = Mask(CmpEq(EndOrDiff(Load(a), Load(b)), zero)))) return DiffAt(a, b, __builtin_ctz(m));
        if (room < 4 * kW) {
            a += kW;
            b += kW;
            continue;
        }
        if ((m = Mask(CmpEq(EndOrDiff(Load(a + kW), Load(b + kW)), zero)))) return DiffAt(a, b, kW + __builtin_ctz(m));
        if ((m = Mask(CmpEq(EndOrDiff(Load(a + 2 * kW), Load(b + 2 * kW)), zero))))
            return DiffAt(a, b, 2 * kW + __builtin_ctz(m));
        if ((m = Mask(CmpEq(EndOrDiff(Load(a + 3 * kW), Load(b + 3 * kW)), zero))))
            return DiffAt(a, b, 3 * kW + __builtin_ctz(m));
        a += 4 * kW;
        b += 4 * kW;
        for (size_t steps = (room - 4 * kW) / (4 * kW); steps; --steps) {
            const V t0 = EndOrDiff(Load(a), Load(b)), t1 = EndOrDiff(Load(a + kW), Load(b + kW));
            const V t2 = EndOrDiff(Load(a + 2 * kW), Load(b + 2 * kW)), t3 = EndOrDiff(Load(a + 3 * kW), Load(b + 3 * kW));
            if (Mask(CmpEq(Min(Min(t0, t1), Min(t2, t3)), zero))) {
                if ((m = Mask(CmpEq(t0, zero)))) return DiffAt(a, b, __builtin_ctz(m));
                if ((m = Mask(CmpEq(t1, zero)))) return DiffAt(a, b, kW + __builtin_ctz(m));
                if ((m = Mask(CmpEq(t2, zero)))) return DiffAt(a, b, 2 * kW + __builtin_ctz(m));
                m = Mask(CmpEq(t3, zero));
                return DiffAt(a, b, 3 * kW + __builtin_ctz(m));
            }
            a += 4 * kW;
            b += 4 * kW;
        }
    }
}
