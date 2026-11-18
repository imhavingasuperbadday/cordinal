// cordinal's allocator (malloc.cpp), for checks.h: random use with every block's bytes checked,
// the aligned allocators, calloc on reused memory, threads freeing each other's blocks, memory
// glibc allocates and cordinal frees, malloc_trim giving memory back, and fork while other threads
// allocate. MallocBench() times it and measures its memory against glibc's allocator.
#pragma once
#include <dlfcn.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <malloc.h>

#include <atomic>
#include <thread>
#include <vector>

namespace libc_checks {

// Resident memory of this process in bytes.
inline long Resident() {
    long pages = 0, resident = 0;
    if (FILE* f = fopen("/proc/self/statm", "r")) {
        if (fscanf(f, "%ld %ld", &pages, &resident) != 2) resident = 0;
        fclose(f);
    }
    return resident * sysconf(_SC_PAGESIZE);
}

// Sizes as programs ask for them: mostly small, some page-sized, a few large and a rare huge one.
inline size_t RandomSize(Random& r) {
    const uint64_t pick = r.Next() % 1000;
    if (pick < 700) return r.Next() % 200;
    if (pick < 900) return 200 + r.Next() % 4000;
    if (pick < 985) return 4096 + r.Next() % 60000;
    if (pick < 998) return 65536 + r.Next() % (2 << 20);
    return (5 << 20) + r.Next() % (3 << 20);
}
inline unsigned char Tag(size_t slot, size_t i) { return (unsigned char)(slot * 31 + i * 7 + 1); }
inline void Fill(unsigned char* p, size_t n, size_t slot) {
    // The ends and a few places between, so large blocks stay cheap to check.
    const size_t step = n > 256 ? n / 61 : 1;
    for (size_t i = 0; i < n; i += step) p[i] = Tag(slot, i);
    if (n) p[n - 1] = Tag(slot, n - 1);
}
inline bool Intact(const unsigned char* p, size_t n, size_t slot) {
    const size_t step = n > 256 ? n / 61 : 1;
    for (size_t i = 0; i < n; i += step)
        if (p[i] != Tag(slot, i)) return false;
    return !n || p[n - 1] == Tag(slot, n - 1);
}

// Random malloc, realloc and free on `slots` live blocks; false on the first broken block.
inline bool Churn(uint64_t seed, int rounds, size_t slots, const char** what) {
    Random r{seed};
    std::vector<unsigned char*> blocks(slots, nullptr);
    std::vector<size_t> sizes(slots, 0);
    bool ok = true;
    for (int round = 0; round < rounds && ok; ++round) {
        const size_t s = r.Next() % slots;
        if (blocks[s] && !Intact(blocks[s], sizes[s], s)) ok = false, *what = "a block's bytes changed";
        const uint64_t action = r.Next() % 10;
        if (!blocks[s] || action < 4) {
            free(blocks[s]);
            sizes[s] = RandomSize(r);
            blocks[s] = (unsigned char*)malloc(sizes[s]);
            if (!blocks[s]) ok = false, *what = "malloc failed";
        } else if (action < 7) {
            const size_t n = RandomSize(r);
            unsigned char* q = (unsigned char*)realloc(blocks[s], n);
            if (!q && n) {
                ok = false, *what = "realloc failed";
                break;
            }
            // The places the old fill wrote that the new size still covers.
            const size_t old = sizes[s], kept = n < old ? n : old;
            const size_t step = old > 256 ? old / 61 : 1;
            for (size_t i = 0; i < kept && ok; i += step)
                if (q[i] != Tag(s, i)) ok = false, *what = "realloc lost bytes";
            if (old && old - 1 < kept && q[old - 1] != Tag(s, old - 1)) ok = false, *what = "realloc lost bytes";
            blocks[s] = q;
            sizes[s] = n;
        } else {
            free(blocks[s]);
            blocks[s] = nullptr;
            sizes[s] = 0;
            continue;
        }
        if (!blocks[s]) continue;
        if ((uintptr_t)blocks[s] & 15) ok = false, *what = "a block is not 16-byte aligned";
        if (malloc_usable_size(blocks[s]) < sizes[s]) ok = false, *what = "malloc_usable_size is too small";
        Fill(blocks[s], sizes[s], s);
    }
    for (size_t s = 0; s < slots; ++s) free(blocks[s]);
    return ok;
}

inline void CheckMalloc() {
#if defined(CORDINAL_STATIC)
    // No loader and no second allocator: this program's malloc and free are the only ones.
    Expect(InProgram((const void*)&malloc) && InProgram((const void*)&free), "malloc and free are cordinal's");
#else
    void* (*glibcMalloc)(size_t) = (void* (*)(size_t))dlsym(RTLD_NEXT, "malloc");
    Expect(InProgram((const void*)&malloc) && InProgram((const void*)&free) && glibcMalloc &&
               !InProgram((const void*)glibcMalloc),
           "malloc and free are cordinal's");
#endif
    const char* what = "";
    Expect(Churn(1, 200000, 512, &what), what);

    // Aligned blocks, every power of two to 1 MiB, small and large.
    bool aligned = true;
    for (size_t align = 1; align <= (1 << 20); align <<= 1) {
        for (size_t n : {1, 15, 100, 5000, 70000, 5000000}) {
            void* p = nullptr;
            if (align >= sizeof(void*) && (posix_memalign(&p, align, n) || ((uintptr_t)p & (align - 1)))) aligned = false;
            if (p) memset(p, 0x5a, n);
            free(p);
            p = aligned_alloc(align, n);
            if (!p || ((uintptr_t)p & (align - 1)) || malloc_usable_size(p) < n) aligned = false;
            if (p) memset(p, 0x5a, n);
            free(p);
        }
    }
    void* page = valloc(100);
    void* odd = memalign(48, 100);  // glibc rounds 48 up to 64
    Expect(aligned && page && !((uintptr_t)page & 4095) && odd && !((uintptr_t)odd & 63), "the aligned allocators");
    free(page);
    free(odd);
    void* bad = nullptr;
    Expect(posix_memalign(&bad, 24, 10) == EINVAL, "posix_memalign refuses a non-power-of-two");

    // calloc clears memory that was used before, at every size.
    bool zero = true;
    for (size_t n : {24, 3000, 30000, 200000, 3000000, 6000000}) {
        for (int i = 0; i < 3; ++i) {
            unsigned char* p = (unsigned char*)malloc(n);
            memset(p, 0xee, n);
            free(p);
            unsigned char* q = (unsigned char*)calloc(1, n);
            for (size_t j = 0; q && j < n; j += 97) zero &= q[j] == 0;
            zero &= q && q[n - 1] == 0;
            free(q);
        }
    }
    Expect(zero, "calloc returns zeroed memory after reuse");
    // Through volatile pointers: the compiler may otherwise drop an allocation whose block is
    // never used and assume it succeeded.
    void* (*volatile callocCall)(size_t, size_t) = &calloc;
    void* (*volatile mallocCall)(size_t) = &malloc;
    errno = 0;
    Expect(!callocCall(SIZE_MAX / 2, 3) && errno == ENOMEM, "calloc refuses an overflowing size");
    errno = 0;
    Expect(!mallocCall(SIZE_MAX - 100) && errno == ENOMEM, "malloc refuses an impossible size");
    void* empty = malloc(0);
    Expect(empty && realloc(empty, 0) == nullptr, "malloc(0) gives a block and realloc(p, 0) frees it");

    // Threads, each churning, and passing blocks to each other to free.
    std::atomic<int> threadFailures{0};
    std::atomic<void*> mailbox[64] = {};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, t] {
            const char* why = "";
            if (!Churn(100 + t, 40000, 128, &why)) ++threadFailures;
            Random r{(uint64_t)t * 7919 + 3};
            for (int i = 0; i < 40000; ++i) {
                const size_t n = RandomSize(r) % 70000;
                unsigned char* p = (unsigned char*)malloc(n + 8);
                memcpy(p, &n, 8);
                Fill(p + 8, n, n);
                void* old = mailbox[r.Next() % 64].exchange(p);  // someone else's block, perhaps
                if (old) {
                    size_t m;
                    memcpy(&m, old, 8);
                    if (!Intact((unsigned char*)old + 8, m, m)) ++threadFailures;
                    free(old);
                }
            }
        });
    }
    for (std::thread& t : threads) t.join();
    for (auto& slot : mailbox) free(slot.exchange(nullptr));
    Expect(threadFailures == 0, "eight threads allocating and freeing each other's blocks");

    // Memory glibc allocates inside its own functions, freed here.
    char* real = realpath("/", nullptr);
    char* cwd = getcwd(nullptr, 0);
    Expect(real && !strcmp(real, "/") && cwd, "realpath and getcwd allocate through cordinal's malloc");
    free(real);
    free(cwd);
    addrinfo* found = nullptr;
    addrinfo hints{};
    hints.ai_flags = AI_NUMERICHOST;
    Expect(getaddrinfo("127.0.0.1", "80", &hints, &found) == 0 && found, "getaddrinfo");
    if (found) freeaddrinfo(found);
    if (FILE* f = fopen("/proc/self/maps", "r")) {
        char* line = nullptr;
        size_t capacity = 0;
        Expect(getline(&line, &capacity, f) > 0, "getline grows its buffer with realloc");
        free(line);
        fclose(f);
    }

    // malloc_trim gives freed pages back.
    malloc_trim(0);
    const long before = Resident();
    std::vector<void*> big;
    for (int i = 0; i < 48; ++i) {
        void* p = malloc(1 << 20);
        memset(p, 1, 1 << 20);
        big.push_back(p);
    }
    const long grown = Resident();
    for (void* p : big) free(p);
    malloc_trim(0);
    const long after = Resident();
    Expect(grown - before > (40l << 20) && after - before < (8l << 20), "malloc_trim returns freed memory");
    if (failures) fprintf(stderr, "resident: %ld KiB before, %ld grown, %ld after trim\n", before >> 10, grown >> 10, after >> 10);

    // fork while other threads allocate: the child can allocate too (no lock left held).
    std::atomic<bool> stop{false};
    std::vector<std::thread> busy;
    for (int t = 0; t < 4; ++t)
        busy.emplace_back([&] {
#if defined(CORDINAL_STATIC)
            // A static program has no rand: a small counter gives the same spread of sizes.
            unsigned seed = (unsigned)(t + 1) * 2654435761u;
            while (!stop) free(malloc(64 + (size_t)((seed = seed * 1664525u + 1013904223u) >> 8) % 5000));
#else
            while (!stop) free(malloc(64 + (size_t)(rand() % 5000)));
#endif
        });
    bool forked = true;
    for (int i = 0; i < 50 && forked; ++i) {
        const pid_t child = fork();
        if (child == 0) {
#if !defined(CORDINAL_STATIC)
            alarm(5);  // the child's own loop is bounded; a static program has no alarm to set
#endif
            for (int j = 0; j < 1000; ++j) free(malloc((size_t)j * 37));
            _exit(0);
        }
        int status = 0;
        forked = child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }
    stop = true;
    for (std::thread& t : busy) t.join();
    Expect(forked, "a child forked while threads allocate can allocate");
}

// Speed and memory against glibc's allocator, which dlsym still finds behind cordinal's.
inline int MallocBench() {
    auto gMalloc = (void* (*)(size_t))dlsym(RTLD_NEXT, "malloc");
    auto gFree = (void (*)(void*))dlsym(RTLD_NEXT, "free");
    auto gTrim = (int (*)(size_t))dlsym(RTLD_NEXT, "malloc_trim");
    void* (*volatile ourMalloc)(size_t) = &malloc;
    void (*volatile ourFree)(void*) = &free;
    if (!gMalloc || !gFree || !gTrim) return 1;
    printf("%-26s %10s %10s %7s\n", "", "cordinal", "glibc", "ratio");
    double worst = 0;
    auto Row = [&](const char* name, double a, double b, const char* unit) {
        printf("%-26s %8.1f%s %8.1f%s %6.2fx\n", name, a, unit, b, unit, a / b);
        if (a / b > worst) worst = a / b;
    };
    char label[64];
    for (size_t n : {16, 64, 256, 1024, 4096, 32768, 100000, 1 << 20}) {
        double a, b;
        TimeBoth([&] { ourFree(ourMalloc(n)); }, [&] { gFree(gMalloc(n)); }, a, b);
        snprintf(label, sizeof label, "malloc+free %zu", n);
        Row(label, a, b, "ns");
    }
    // Bursts: 1,000 blocks of mixed small sizes, then all freed.
    static void* held[1000];
    auto Burst = [&](void* (*alloc)(size_t), void (*release)(void*)) {
        for (int i = 0; i < 1000; ++i) held[i] = alloc(16 + (size_t)(i * 37) % 300);
        for (int i = 0; i < 1000; ++i) release(held[i]);
    };
    {
        double a, b;
        TimeBoth([&] { Burst(ourMalloc, ourFree); }, [&] { Burst(gMalloc, gFree); }, a, b);
        Row("1,000 small, then free", a / 1000, b / 1000, "ns");
    }
    // Four threads at once, each a million malloc+free of mixed small sizes.
    auto Threads = [&](void* (*alloc)(size_t), void (*release)(void*)) {
        timespec start{}, end{};
        clock_gettime(CLOCK_MONOTONIC, &start);
        std::vector<std::thread> ts;
        for (int t = 0; t < 4; ++t)
            ts.emplace_back([&, t] {
                void* keep[64] = {};
                for (int i = 0; i < 1000000; ++i) {
                    const int k = (i * 7 + t) & 63;
                    release(keep[k]);
                    keep[k] = alloc(16 + (size_t)(i * 13) % 500);
                }
                for (void* p : keep) release(p);
            });
        for (std::thread& th : ts) th.join();
        clock_gettime(CLOCK_MONOTONIC, &end);
        return ((end.tv_sec - start.tv_sec) * 1e9 + (end.tv_nsec - start.tv_nsec)) / 4e6;
    };
    {
        double a = 1e300, b = 1e300;
        for (int i = 0; i < 3; ++i) {
            const double x = Threads(ourMalloc, ourFree), y = Threads(gMalloc, gFree);
            if (x < a) a = x;
            if (y < b) b = y;
        }
        Row("4 threads, per operation", a, b, "ns");
    }
    // Memory: 200,000 small blocks like a library's strings, then every other one freed.
    auto Footprint = [&](void* (*alloc)(size_t), void (*release)(void*), int (*trim)(size_t), long& full, long& half) {
        trim(0);
        const long base = Resident();
        std::vector<void*> blocks(200000);
        Random r{42};
        for (void*& p : blocks) {
            const size_t n = 8 + r.Next() % 120;
            p = alloc(n);
            memset(p, 1, n);
        }
        full = Resident() - base;
        for (size_t i = 0; i < blocks.size(); i += 2) release(blocks[i]);
        trim(0);
        half = Resident() - base;
        for (size_t i = 1; i < blocks.size(); i += 2) release(blocks[i]);
        trim(0);
    };
    long ourFull, ourHalf, theirFull, theirHalf;
    Footprint(gMalloc, gFree, gTrim, theirFull, theirHalf);
    Footprint(ourMalloc, ourFree, &malloc_trim, ourFull, ourHalf);
    Row("200,000 small: resident", ourFull / 1048576.0, theirFull / 1048576.0, "MB");
    Row("half freed, trimmed", ourHalf / 1048576.0, theirHalf / 1048576.0, "MB");
    printf("slowest or largest against glibc: %.2fx\n", worst);
    return 0;
}

}  // namespace libc_checks
