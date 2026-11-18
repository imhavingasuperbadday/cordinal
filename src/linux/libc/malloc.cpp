// cordinal's own memory allocator (stage 2b of replacing glibc): malloc, free, calloc, realloc,
// the aligned allocators, malloc_usable_size and malloc_trim. Unlike the rest of src/linux/libc
// these are exported: glibc allocates inside its own functions (realpath, getaddrinfo, fopen) and
// cordinal frees that memory, so one allocator must serve the whole process. glibc supports a
// program replacing malloc this way, and calls these for its own allocations.
//
// Layout. Memory comes from the kernel in 4 MiB segments aligned to 4 MiB, cut into 64 KiB pages;
// page 0 holds the segment's header with one descriptor per page, so any pointer finds its
// descriptor by masking. Blocks up to 32 KiB come from 40 size classes, each page given to one
// class. Larger blocks take a run of whole pages; blocks too large for a segment get a mapping of
// their own, also 4 MiB aligned with a header in front.
// Speed. Each thread keeps a few free blocks per class, so most malloc and free calls take no
// lock; the class's lock is taken only to refill or return a batch.
// Memory. Freed pages stay mapped until 4 MiB of them are waiting, then go back to the kernel
// (MADV_DONTNEED); malloc_trim returns them at once. An empty segment beyond the first is unmapped.
// Fresh and returned pages read as zero, so calloc clears only pages that were used before.
// Built with -fno-builtin (CMakeLists.txt), so the compiler never turns code here into a call to
// malloc or calloc.
#include "syscall.h"
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace {
constexpr size_t kPage = 64 << 10;
constexpr int kPages = 64;
constexpr size_t kSegment = kPage * kPages;
constexpr size_t kMaxSmall = 32 << 10;
constexpr int kClasses = 40;
constexpr size_t kMaxRun = (kPages - 1) * kPage;
constexpr size_t kDirtyLimit = 1 << 20;
constexpr uint64_t kAllPages = ~1ull;  // every page but the header's
constexpr uint32_t kMagic = 0x636f7264;  // "cord"

// Kernel calls without errno, so a failed map leaves errno to the caller.
void* Map(size_t size) {
    const long r = cordlibc::Syscall(__NR_mmap, 0, (long)size, 3 /* read, write */, 0x22 /* private, anonymous */, -1, 0);
    return (unsigned long)r > (unsigned long)-4096L ? nullptr : (void*)r;
}
void Unmap(void* p, size_t size) { cordlibc::Syscall(__NR_munmap, (long)p, (long)size); }
void Discard(void* p, size_t size) { cordlibc::Syscall(__NR_madvise, (long)p, (long)size, 4 /* MADV_DONTNEED */); }
// A mapping of `size` bytes starting at a multiple of kSegment.
char* MapAligned(size_t size) {
    char* raw = (char*)Map(size + kSegment);
    if (!raw) return nullptr;
    char* start = (char*)(((uintptr_t)raw + kSegment - 1) & ~(uintptr_t)(kSegment - 1));
    if (start > raw) Unmap(raw, (size_t)(start - raw));
    char* const end = start + size;
    if (raw + size + kSegment > end) Unmap(end, (size_t)(raw + size + kSegment - end));
    return start;
}

// A futex lock: 0 free, 1 held, 2 held with waiters. It spins briefly first, since the work
// done under it is short.
struct alignas(64) Lock {
    int state = 0;
    void Acquire() {
        int c = 0;
        if (__atomic_compare_exchange_n(&state, &c, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
        for (int i = 0; i < 64; ++i) {
            __builtin_ia32_pause();
            c = 0;
            if (__atomic_load_n(&state, __ATOMIC_RELAXED) == 0 &&
                __atomic_compare_exchange_n(&state, &c, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
                return;
        }
        c = __atomic_exchange_n(&state, 2, __ATOMIC_ACQUIRE);
        while (c) {
            cordlibc::Syscall(__NR_futex, (long)&state, 128 /* FUTEX_WAIT_PRIVATE */, 2, 0);
            c = __atomic_exchange_n(&state, 2, __ATOMIC_ACQUIRE);
        }
    }
    void Release() {
        if (__atomic_exchange_n(&state, 0, __ATOMIC_RELEASE) == 2)
            cordlibc::Syscall(__NR_futex, (long)&state, 129 /* FUTEX_WAKE_PRIVATE */, 1);
    }
};
struct Guard {
    Lock& lock;
    explicit Guard(Lock& l) : lock(l) { lock.Acquire(); }
    ~Guard() { lock.Release(); }
};

struct Block {
    Block* next;
};
enum : uint8_t { kFreePage, kSmallPage, kRunPage, kRunTail, kHugePage };
// One per page. A small page holds blocks of one class; a run's first page describes the run.
struct Span {
    uint8_t kind;
    uint8_t cls;
    uint8_t pages;  // a run's length
    uint8_t first;  // a run tail's first page
    uint32_t used;  // blocks handed out (including those in thread caches)
    uint32_t capacity;
    Block* free;
    char* bump;  // the next block never handed out
    char* end;
    Span* next;
    Span* prev;
};
// A segment's header, in its page 0. A huge block's mapping starts with one too, with only the
// descriptor of the block's own page filled in (kHugePage), so free reads one descriptor either way.
struct Segment {
    uint32_t magic;
    uint32_t unused;
    size_t mapSize;  // a huge block: the whole mapping
    Segment* next;
    uint64_t freeMask;   // pages free
    uint64_t dirtyMask;  // free pages still holding old data
    Span spans[kPages];
};
static_assert(sizeof(Segment) <= kPage, "the header fits in page 0");
constexpr size_t kHugeHeader = (sizeof(Segment) + 63) & ~(size_t)63;

inline Segment* SegmentOf(const void* p) { return (Segment*)((uintptr_t)p & ~(uintptr_t)(kSegment - 1)); }
inline int PageIndex(const void* p) { return (int)(((uintptr_t)p & (kSegment - 1)) / kPage); }
inline char* PageAddress(Segment* s, int i) { return (char*)s + (size_t)i * kPage; }

// Size classes: 16 to 128 in steps of 16, then four steps per power of two up to 32 KiB.
constexpr size_t ClassSize(int c) {
    if (c < 8) return (size_t)(c + 1) * 16;
    const int k = c - 8, b = 7 + k / 4, j = k % 4;
    return ((size_t)1 << b) + (size_t)(j + 1) * ((size_t)1 << (b - 2));
}
constexpr int ClassBySize(size_t n) {
    if (n <= 128) return n ? (int)((n + 15) >> 4) - 1 : 0;
    const int b = 63 - __builtin_clzll(n - 1);
    return 8 + (b - 7) * 4 + (int)((n - 1 - ((size_t)1 << b)) >> (b - 2));
}
static_assert(ClassSize(kClasses - 1) == kMaxSmall, "the last class is the largest small block");
// Worked out once, so the fast paths only read: each class's size, how many free blocks of it a
// thread keeps (about 16 KiB, at least 2, at most 64), and the class of every size to 1 KiB in
// steps of 16.
struct Tables {
    uint32_t size[kClasses];
    uint16_t limit[kClasses];
    uint8_t byStep[65];
};
constexpr Tables MakeTables() {
    Tables t{};
    for (int c = 0; c < kClasses; ++c) {
        t.size[c] = (uint32_t)ClassSize(c);
        const size_t n = 8192 / ClassSize(c);
        t.limit[c] = (uint16_t)(n < 2 ? 2 : n > 64 ? 64 : n);
    }
    for (size_t i = 0; i <= 64; ++i) t.byStep[i] = (uint8_t)ClassBySize(i * 16);
    return t;
}
constexpr Tables kTables = MakeTables();
inline size_t SizeOf(int c) { return kTables.size[c]; }
inline unsigned CacheLimit(int c) { return kTables.limit[c]; }
// Rounding up to a step of 16 never crosses a class boundary below 1 KiB: every class size there
// is a multiple of 16.
inline int ClassOf(size_t n) { return n <= 1024 ? kTables.byStep[(n + 15) >> 4] : ClassBySize(n); }

struct Class {
    Lock lock;
    Span* partial = nullptr;  // pages with blocks to hand out
};
Class classes[kClasses];
Lock pageLock;
Segment* segments = nullptr;
size_t dirtyBytes = 0;

// Pages. All under pageLock.
inline uint64_t RunMask(int n) { return n >= 64 ? ~0ull : (((uint64_t)1 << n) - 1); }
void Purge() {
    for (Segment** link = &segments; *link;) {
        Segment* s = *link;
        if (s->freeMask == kAllPages && s != segments) {  // empty, and not the first: unmap it
            *link = s->next;
            Unmap(s, kSegment);
            continue;
        }
        for (uint64_t dirty = s->dirtyMask; dirty;) {  // each run of dirty pages, one call each
            const int start = __builtin_ctzll(dirty);
            const uint64_t rest = ~(dirty >> start);
            const int n = rest ? __builtin_ctzll(rest) : 64 - start;
            Discard(PageAddress(s, start), (size_t)n * kPage);
            dirty &= ~(RunMask(n) << start);
        }
        s->dirtyMask = 0;
        link = &s->next;
    }
    dirtyBytes = 0;
}
// A run of n free pages, preferring ones still holding data (they are resident already).
Span* AllocPages(int n, bool& clean) {
    Segment* s = segments;
    int start = -1;
    for (; s; s = s->next) {
        // Bits where n free pages start: each step doubles the run length checked.
        uint64_t starts = s->freeMask;
        for (int length = 1; length < n && starts;) {
            const int shift = length < n - length ? length : n - length;
            starts &= starts >> shift;
            length += shift;
        }
        if (!starts) continue;
        const uint64_t warm = starts & s->dirtyMask;
        start = __builtin_ctzll(warm ? warm : starts);
        break;
    }
    if (!s) {
        s = (Segment*)MapAligned(kSegment);
        if (!s) return nullptr;
        s->magic = kMagic;
        s->freeMask = kAllPages;
        s->next = segments;
        segments = s;
        start = 1;
    }
    const uint64_t run = RunMask(n) << start;
    const uint64_t dirty = s->dirtyMask & run;
    clean = !dirty;
    dirtyBytes -= (size_t)__builtin_popcountll(dirty) * kPage;
    s->freeMask &= ~run;
    s->dirtyMask &= ~run;
    Span* span = &s->spans[start];
    span->pages = (uint8_t)n;
    for (int i = 1; i < n; ++i) {
        s->spans[start + i].kind = kRunTail;
        s->spans[start + i].first = (uint8_t)start;
    }
    return span;
}
void FreePages(Span* span) {
    Segment* s = SegmentOf(span);
    const int start = (int)(span - s->spans), n = span->pages;
    const uint64_t run = RunMask(n) << start;
    for (int i = 0; i < n; ++i) s->spans[start + i].kind = kFreePage;
    s->freeMask |= run;
    s->dirtyMask |= run;
    dirtyBytes += (size_t)n * kPage;
    if (dirtyBytes > kDirtyLimit || (s->freeMask == kAllPages && s != segments)) Purge();
}
inline char* SpanAddress(Span* span) {
    Segment* s = SegmentOf(span);
    return PageAddress(s, (int)(span - s->spans));
}

// Small blocks, central side. All under the class's lock.
inline void Unlink(Class& k, Span* s) {
    if (s->prev) s->prev->next = s->next;
    else k.partial = s->next;
    if (s->next) s->next->prev = s->prev;
    s->next = s->prev = nullptr;
}
inline void LinkFront(Class& k, Span* s) {
    s->prev = nullptr;
    s->next = k.partial;
    if (k.partial) k.partial->prev = s;
    k.partial = s;
}
// Up to `want` blocks onto *list; returns how many.
unsigned TakeBlocks(int c, Block** list, unsigned want) {
    Class& k = classes[c];
    const size_t size = SizeOf(c);
    unsigned got = 0;
    while (got < want) {
        Span* s = k.partial;
        if (!s) {
            bool clean;
            {
                Guard g(pageLock);
                s = AllocPages(1, clean);
            }
            if (!s) break;
            char* const start = SpanAddress(s);
            s->kind = kSmallPage;
            s->cls = (uint8_t)c;
            s->used = 0;
            s->capacity = (uint32_t)(kPage / size);
            s->free = nullptr;
            s->bump = start;
            s->end = start + (size_t)s->capacity * size;
            LinkFront(k, s);
        }
        while (got < want && s->used < s->capacity) {
            Block* b = s->free;
            if (b) s->free = b->next;
            else {
                b = (Block*)s->bump;
                s->bump += size;
            }
            b->next = *list;
            *list = b;
            ++s->used;
            ++got;
        }
        if (s->used == s->capacity) Unlink(k, s);
    }
    return got;
}
void ReturnBlock(int c, Block* b) {
    Class& k = classes[c];
    Segment* seg = SegmentOf(b);
    Span* s = &seg->spans[PageIndex(b)];
    if (s->used == s->capacity) LinkFront(k, s);
    b->next = s->free;
    s->free = b;
    // An empty page goes back to the pages unless it is the class's only one.
    if (--s->used == 0 && (k.partial != s || s->next)) {
        Unlink(k, s);
        Guard g(pageLock);
        FreePages(s);
    }
}

// Thread caches. state: 0 not set up, 1 in use, 2 set-up postponed (early start), 3 thread ended.
struct Cache {
    Block* list[kClasses];
    uint16_t count[kClasses];
    uint8_t state;
};
__thread Cache cache __attribute__((tls_model("initial-exec")));
bool started = false;  // set by a constructor: before it, every call uses the central lists
Lock setupLock;
pthread_key_t exitKey;
bool keyMade = false;

void Flush(Cache& tc, int c, unsigned keep) {
    Guard g(classes[c].lock);
    while (tc.count[c] > keep) {
        Block* b = tc.list[c];
        tc.list[c] = b->next;
        --tc.count[c];
        ReturnBlock(c, b);
    }
}
void FlushAll(Cache& tc) {
    for (int c = 0; c < kClasses; ++c)
        if (tc.count[c]) Flush(tc, c, 0);
}
void ThreadEnded(void* p) {
    Cache& tc = *(Cache*)p;
    FlushAll(tc);
    tc.state = 3;  // later frees in this thread's last destructors go straight to the classes
}
// fork: hold every lock across it, so the child never inherits one held by another thread.
void BeforeFork() {
    setupLock.Acquire();
    for (Class& k : classes) k.lock.Acquire();
    pageLock.Acquire();
}
void AfterForkParent() {
    pageLock.Release();
    for (int c = kClasses - 1; c >= 0; --c) classes[c].lock.Release();
    setupLock.Release();
}
void AfterForkChild() {
    pageLock.state = 0;
    for (Class& k : classes) k.lock.state = 0;
    setupLock.state = 0;
}
bool SetUp(Cache& tc) {
    if (!started) return false;
    tc.state = 2;  // anything allocated while setting up goes to the classes directly
    {
        Guard g(setupLock);
        if (!keyMade) {
            if (pthread_key_create(&exitKey, ThreadEnded)) return false;
            pthread_atfork(BeforeFork, AfterForkParent, AfterForkChild);
            keyMade = true;
        }
    }
    pthread_setspecific(exitKey, &tc);
    tc.state = 1;
    return true;
}
__attribute__((constructor(101))) void Start() { started = true; }

void* AllocSmall(int c) {
    Cache& tc = cache;
    if (Block* b = tc.list[c]) {
        tc.list[c] = b->next;
        --tc.count[c];
        return b;
    }
    if (tc.state != 1 && (tc.state != 0 || !SetUp(tc))) {
        Block* one = nullptr;
        Guard g(classes[c].lock);
        TakeBlocks(c, &one, 1);
        return one;
    }
    {
        Guard g(classes[c].lock);
        tc.count[c] = (uint16_t)TakeBlocks(c, &tc.list[c], (CacheLimit(c) + 1) / 2);
    }
    Block* b = tc.list[c];
    if (!b) return nullptr;
    tc.list[c] = b->next;
    --tc.count[c];
    return b;
}
void FreeSmall(int c, Block* b) {
    Cache& tc = cache;
    if (tc.state == 1 || (tc.state == 0 && SetUp(tc))) {
        b->next = tc.list[c];
        tc.list[c] = b;
        if (++tc.count[c] > CacheLimit(c)) Flush(tc, c, CacheLimit(c) / 2);
        return;
    }
    Guard g(classes[c].lock);
    ReturnBlock(c, b);
}

// Large blocks: page runs, or a mapping of their own.
void* AllocRun(size_t n, bool zero) {
    const int pages = (int)((n + kPage - 1) / kPage);
    bool clean;
    Span* span;
    {
        Guard g(pageLock);
        span = AllocPages(pages, clean);
    }
    if (!span) return nullptr;
    span->kind = kRunPage;
    char* p = SpanAddress(span);
    if (zero && !clean) memset(p, 0, n);
    return p;
}
void* AllocHuge(size_t n, size_t align) {
    const size_t offset = align > 64 ? (kHugeHeader + align - 1) & ~(align - 1) : kHugeHeader;
    if (n > SIZE_MAX - offset - kSegment || offset >= kSegment) return nullptr;
    const size_t size = (offset + n + 4095) & ~(size_t)4095;
    Segment* s = (Segment*)MapAligned(size);
    if (!s) return nullptr;
    s->magic = kMagic;
    s->mapSize = size;
    char* const p = (char*)s + offset;  // a fresh mapping reads as zero
    s->spans[PageIndex(p)].kind = kHugePage;
    return p;
}

void* Allocate(size_t n, bool zero) {
    void* p;
    if (n <= kMaxSmall) {
        p = AllocSmall(ClassOf(n));
        if (p && zero) memset(p, 0, n);
    } else if (n <= kMaxRun) {
        p = AllocRun(n, zero);
    } else {
        p = AllocHuge(n, 0);
    }
    if (!p) errno = ENOMEM;
    return p;
}
size_t UsableSize(void* p) {
    Segment* s = SegmentOf(p);
    const Span& span = s->spans[PageIndex(p)];
    if (span.kind == kSmallPage) return SizeOf(span.cls);
    if (span.kind == kHugePage) return s->mapSize - (size_t)((char*)p - (char*)s);
    return (size_t)span.pages * kPage;
}
void Release(void* p) {
    Segment* s = SegmentOf(p);
    Span* span = &s->spans[PageIndex(p)];
    if (__builtin_expect(span->kind == kSmallPage, 1)) {
        FreeSmall(span->cls, (Block*)p);
    } else if (span->kind == kHugePage) {
        Unmap(s, s->mapSize);
    } else {
        Guard g(pageLock);
        FreePages(span);
    }
}
void* AllocAligned(size_t align, size_t n) {
    if (align <= 16) return Allocate(n, false);
    void* p;
    const size_t want = n > align ? n : align;
    if (want <= kMaxSmall) {
        // A power-of-two class is a multiple of the alignment, and pages start 64 KiB aligned.
        const size_t size = (size_t)1 << (64 - __builtin_clzll(want - 1));
        p = AllocSmall(ClassOf(size));
    } else if (n <= kMaxRun && align <= kPage) {
        p = AllocRun(n, false);
    } else {
        p = AllocHuge(n, align);
    }
    if (!p) errno = ENOMEM;
    return p;
}

// The common cases inline: a small block from, or back to, this thread's cache.
__attribute__((noinline)) void* AllocateSlow(size_t n) { return Allocate(n, false); }
__attribute__((noinline)) void ReleaseSlow(void* p) { Release(p); }
}  // namespace

// files.cpp's fork calls these around its clone: only glibc's fork runs atfork handlers, and
// cordinal's fork is its own. Hidden, since nothing outside the program may see them.
namespace cordlibc {
__attribute__((visibility("hidden"))) void ForkPrepare() { BeforeFork(); }
__attribute__((visibility("hidden"))) void ForkParent() { AfterForkParent(); }
__attribute__((visibility("hidden"))) void ForkChild() { AfterForkChild(); }
}  // namespace cordlibc

extern "C" {
void* malloc(size_t n) noexcept {
    if (__builtin_expect(n <= kMaxSmall, 1)) {
        Cache& tc = cache;
        const int c = ClassOf(n);
        if (Block* b = tc.list[c]) {
            tc.list[c] = b->next;
            --tc.count[c];
            return b;
        }
    }
    return AllocateSlow(n);
}
void free(void* p) noexcept {
    if (!p) return;
    const Span& span = SegmentOf(p)->spans[PageIndex(p)];
    Cache& tc = cache;
    if (__builtin_expect(span.kind == kSmallPage && tc.state == 1, 1)) {
        const int c = span.cls;
        if (tc.count[c] < CacheLimit(c)) {
            ((Block*)p)->next = tc.list[c];
            tc.list[c] = (Block*)p;
            ++tc.count[c];
            return;
        }
    }
    ReleaseSlow(p);
}
void* calloc(size_t count, size_t size) noexcept {
    size_t n;
    if (__builtin_mul_overflow(count, size, &n)) {
        errno = ENOMEM;
        return nullptr;
    }
    return Allocate(n, true);
}
void* realloc(void* p, size_t n) noexcept {
    if (!p) return Allocate(n, false);
    if (!n) {  // as glibc: free, and no block back
        Release(p);
        return nullptr;
    }
    const size_t old = UsableSize(p);
    if (n <= old) {
        // Small blocks stay if the size still fits their class; larger ones unless they shrink by half.
        const Span& span = SegmentOf(p)->spans[PageIndex(p)];
        if (span.kind == kSmallPage) {
            if (ClassOf(n) == span.cls) return p;
        } else if (n > old / 2) {
            return p;
        }
    }
    void* q = Allocate(n, false);
    if (!q) return nullptr;
    memcpy(q, p, n < old ? n : old);
    Release(p);
    return q;
}
int posix_memalign(void** out, size_t align, size_t n) noexcept {
    if (!align || (align & (align - 1)) || align % sizeof(void*)) return EINVAL;
    const int saved = errno;
    void* p = AllocAligned(align, n);
    errno = saved;
    if (!p) return ENOMEM;
    *out = p;
    return 0;
}
void* aligned_alloc(size_t align, size_t n) noexcept {
    if (!align || (align & (align - 1))) {
        errno = EINVAL;
        return nullptr;
    }
    return AllocAligned(align, n);
}
void* memalign(size_t align, size_t n) noexcept {
    if (align & (align - 1)) {  // glibc rounds a non-power-of-two up
        size_t a = 16;
        while (a < align) a <<= 1;
        align = a;
    }
    return AllocAligned(align, n);
}
void* valloc(size_t n) noexcept { return AllocAligned(4096, n); }
void* pvalloc(size_t n) noexcept { return AllocAligned(4096, (n + 4095) & ~(size_t)4095); }
size_t malloc_usable_size(void* p) noexcept { return p ? UsableSize(p) : 0; }
// Gives every free page back to the kernel, and this thread's cached blocks first.
int malloc_trim(size_t) noexcept {
    Cache& tc = cache;
    if (tc.state == 1) FlushAll(tc);
    Guard g(pageLock);
    const bool any = dirtyBytes > 0;
    Purge();
    return any;
}
}  // extern "C"
