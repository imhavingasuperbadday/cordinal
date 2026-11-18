// cordinal's own threads and thread-local storage (stage 8 of replacing glibc): pthread_create and the
// thread lifecycle over clone and futex, the mutex, condition variable, rwlock, once and key families,
// __cxa_thread_atexit_impl, the atfork lists, __tls_get_addr, dl_iterate_phdr, __libc_single_threaded
// and the thread-local storage every one of them runs on. Defined under glibc's names with hidden
// visibility, so cordinal and the static libstdc++ bind to them while glibc keeps its own copies for
// its own callers.
//
// The thread pointer. glibc's thread pointer (fs) points at the thread control block, and the static
// TLS blocks sit below it: the thread pointer is the address the linker's local-exec offsets count
// back from. Everything here was measured on a running program (glibc 2.44, x86-64, November 18,
// 2026) rather than written from memory:
//   - fs:0x00 and fs:0x10 both hold fs, the pointer to the control block itself (tcb and self).
//   - fs:0x08 holds the dtv: 16 bytes per module, the block's address then a flag word.
//   - fs:0x18 holds multiple_threads (0 in the first thread, 1 in the others).
//   - fs:0x28 holds the stack guard, the same random value in every thread of the process.
//   - fs:0x30 holds the pointer guard (0 here: nothing in cordinal mangles pointers).
//   - the program's own block starts at fs - round_up(PT_TLS.p_memsz, PT_TLS.p_align): a 100-byte,
//     16-aligned block sat at fs-0x90, and a static-pie binary's 0xe0-byte block at fs-0xe0.
//   - the other modules' blocks are below that, in reverse load order: with libc, libgcc_s,
//     libstdc++ and the program loaded, libc's 0x80-byte block was at fs-0xa8, libgcc_s's 8 bytes at
//     fs-0x28 and libstdc++'s 0x20 bytes at fs-0x20, ending exactly at the thread pointer.
//   - glibc's errno is initial-exec TLS in libc's block: it was at fs-0x78, and h_errno at fs-0x50.
// A new thread therefore gets the program's whole static area at the same fs-relative offsets, laid out
// by reading the module list and confirming each block against the first thread's dtv (Layout below).
// glibc code that still runs on these threads (dlsym, getpwuid, strerror) finds its TLS exactly where
// its instructions look for it; cordinal's own thread-local storage is the program's block,
// re-initialized from its init image.
//
// What stays with glibc, and why: cancellation (nothing cancels a thread here, so setcancelstate keeps
// bookkeeping only and no thread ever ends in PTHREAD_CANCELED), robust mutexes (the owner-death
// protocol needs kernel-side state nothing here uses; a robust mutex behaves as a normal one), the exit
// path (exit.cpp in the static build, glibc's own in the dynamic one), and __libc_start_main.
// No glibc header that declares these functions is included except pthread.h, whose types the callers
// allocate; the definitions are the declarations, with the exception specifications pthread.h states.
#include "syscall.h"

#include <dlfcn.h>
#include <errno.h>
#include <link.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define CL extern "C" __attribute__((visibility("hidden")))
#define HIDDEN __attribute__((visibility("hidden")))

// The allocator (malloc.cpp), the calls the exit path needs, and the two symbols the static build takes
// from crtbegin or exit.cpp.
extern "C" void* malloc(size_t);
extern "C" void* calloc(size_t, size_t);
extern "C" void free(void*);
extern "C" __attribute__((noreturn)) void abort(void) noexcept;
extern "C" int __cxa_atexit(void (*)(void*), void*, void*);
extern "C" void* __dso_handle;
// The flag libstdc++ reads to skip atomic reference counts (defined at the end of this file).
extern "C" __attribute__((visibility("hidden"))) char __libc_single_threaded;
#if defined(CORDINAL_STATIC)
// loader.cpp (the static build's loader): the image's load bias, which a static-PIE link's program
// headers do not carry in a PT_PHDR segment, and the two hooks the loader answers through.
extern "C" __attribute__((visibility("hidden"))) uintptr_t cordlibc_LoaderBias(void);
extern "C" __attribute__((visibility("hidden"))) void* LoaderTlsGet(unsigned long);
extern "C" __attribute__((visibility("hidden"))) int LoaderPhdrWalk(int (*)(struct dl_phdr_info*, size_t, void*), void*);
#endif

namespace {

// ---------------------------------------------------------------------------------------------------
// The kernel's side of a thread.

// clone's flags: share the address space, the file table, signal dispositions and the sysv semaphore
// state; be a thread of this process; have the kernel install the thread pointer, write the tid, and
// clear and wake it as the thread ends.
constexpr int kCloneVm = 0x00000100;
constexpr int kCloneFs = 0x00000200;
constexpr int kCloneFiles = 0x00000400;
constexpr int kCloneSignalHandler = 0x00000800;
constexpr int kCloneThread = 0x00010000;
constexpr int kCloneSysVsem = 0x00040000;
constexpr int kCloneSetTls = 0x00080000;
constexpr int kCloneParentSetTid = 0x00100000;
constexpr int kCloneChildClearTid = 0x00200000;

// futex operations. The private flag keeps a lock's waits out of the inode-keyed hash; the tid word a
// thread ends on is cleared and woken by the kernel with a plain wake, so joining uses that form.
constexpr int kFutexWait = 0;
constexpr int kFutexWake = 1;
constexpr int kFutexPrivate = 128;

// The control block's fields this file relies on (offsets measured above).
constexpr size_t kTcbSelf = 0x00;
constexpr size_t kTcbSelfAgain = 0x10;
constexpr size_t kTcbDtv = 0x08;
constexpr size_t kTcbMultipleThreads = 0x18;
constexpr size_t kTcbHeaderCopy = 0x78;  // what a control block holds, copied from the first thread's
constexpr size_t kThreadSlot = 0x80;     // this file's own descriptor, in a thread's reserve

constexpr size_t kDefaultStack = 8u << 20;
constexpr size_t kDefaultGuard = 4096;
constexpr size_t kTcbReserve = 0x2000;  // control block, descriptor, and the space glibc's own would use
constexpr size_t kMapPrivateAnonymousStack = 0x02 | 0x20 | 0x20000;
constexpr int kProtNone = 0;
constexpr int kProtReadWrite = 3;
constexpr int kMaxModules = 64;
constexpr size_t kDtvEntryBytes = 16;  // a block's address, then a flag word
constexpr int kKeys = 1024;
constexpr int kKeyBlock = 32;  // the key values that sit in the thread's own storage
constexpr int kDestructorPasses = 4;
constexpr int kMaxForkHandlers = 32;
constexpr long kRlimitStack = 3;
constexpr uint64_t kThreadMagic = 0x636f726454485244ull;


inline long Raw(long number, long a = 0, long b = 0, long c = 0, long d = 0, long e = 0, long f = 0) {
    return cordlibc::Syscall(number, a, b, c, d, e, f);
}
inline uintptr_t ThreadPointer() {
    uintptr_t value;
    __asm__ volatile("mov %%fs:0, %0" : "=r"(value));
    return value;
}
inline int GetTid() { return (int)Raw(__NR_gettid); }
inline int GetPid() { return (int)Raw(__NR_getpid); }

// A futex lock of the shape malloc.cpp and stdio.cpp use: 0 free, 1 held, 2 held with waiters.
struct alignas(64) Lock {
    int state;
    void Acquire() {
        int expected = 0;
        if (__atomic_compare_exchange_n(&state, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
        for (int i = 0; i < 64; ++i) {
            __builtin_ia32_pause();
            expected = 0;
            if (__atomic_load_n(&state, __ATOMIC_RELAXED) == 0 &&
                __atomic_compare_exchange_n(&state, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
                return;
        }
        while (__atomic_exchange_n(&state, 2, __ATOMIC_ACQUIRE) != 0)
            Raw(__NR_futex, (long)&state, kFutexWait | kFutexPrivate, 2);
    }
    void Release() {
        if (__atomic_exchange_n(&state, 0, __ATOMIC_RELEASE) == 2)
            Raw(__NR_futex, (long)&state, kFutexWake | kFutexPrivate, 1);
    }
};
struct Guard {
    Lock& lock;
    explicit Guard(Lock& l) : lock(l) { lock.Acquire(); }
    ~Guard() { lock.Release(); }
};

void FutexWait(int* address, int value, const struct timespec* timeout, bool shared = false) {
    Raw(__NR_futex, (long)address, kFutexWait | (shared ? 0 : kFutexPrivate), value, (long)timeout);
}
void FutexWake(int* address, int count) { Raw(__NR_futex, (long)address, kFutexWake | kFutexPrivate, count); }
// 0 when woken (or when the word had already changed), ETIMEDOUT, or another error number.
int FutexWaitResult(int* address, int value, const struct timespec* timeout) {
    const long r = Raw(__NR_futex, (long)address, kFutexWait | kFutexPrivate, value, (long)timeout);
    return r < 0 ? (int)-r : 0;
}

// glibc declares several of these arguments nonnull, so a direct comparison with null warns; the checks
// stay anyway, because a caller can still pass one.
inline bool Missing(const void* pointer) { return pointer == nullptr; }

inline long RoundUp(long value, long align) { return (value + align - 1) & ~(align - 1); }

// ---------------------------------------------------------------------------------------------------
// The static TLS layout: what the running process already has, measured once and replicated.

struct Module {
    uintptr_t bias;       // the load address its link-time addresses count from
    const char* name;     // the loader's name ("" for the program itself)
    uintptr_t phdr;       // its program headers, where the loader mapped them
    int phnum;
    int tlsModid;            // its id in the loader's thread-local slot table (0 without a PT_TLS segment)
    long fromThreadPointer;  // where its TLS block starts, relative to the thread pointer
    const void* image;       // its initialized TLS bytes
    uint32_t filesz;
    uint32_t memsz;
    uint32_t align;
};

struct Layout {
    long staticSize;  // bytes below the thread pointer that every thread carries
    long exeFromThreadPointer;
    long errnoFromThreadPointer;  // glibc's errno, measured, for the checks and the notes
    int modules;
    int exeModule;    // the program's own slot id: the loader numbers modules that have TLS from 1
    bool observed;    // the first thread's slot table was readable and named the blocks
    bool agrees;      // those blocks were exactly the ones the reverse-order walk computes
    bool tcbAsExpected;
    bool ownTlsInside;  // a thread-local the linker placed itself lies inside the program's block
    Module module[kMaxModules];
};
Layout layout;
Lock layoutLock;

// The program's program headers and load bias, from the auxiliary vector (time.cpp's walk, extended).
struct Auxv {
    uintptr_t phdr;
    int phnum;
    uintptr_t bias;
};
Auxv auxv;

// A module in the loader's list: the five members every ELF loader keeps for the debugger.
struct LinkMap {
    uintptr_t address;
    char* name;
    void* dynamic;
    LinkMap* next;
    LinkMap* previous;
};
struct DebugStructure {
    int version;
    LinkMap* map;
    uintptr_t breakAddress;
    int state;
    uintptr_t loaderBase;
};

// Reads one module's program headers for its PT_TLS. The headers of a shared object are in its first
// read-only segment, at the link-time address plus the load bias. The loader gives a slot id to the
// modules that have a segment, numbering them from 1 in load order.
void ReadTls(Module& module, uintptr_t bias, uintptr_t phdrAddress, int phnum) {
    const ElfW(Phdr)* phdr = (const ElfW(Phdr)*)phdrAddress;
    for (int i = 0; i < phnum; ++i) {
        if (phdr[i].p_type != PT_TLS || phdr[i].p_memsz == 0) continue;
        module.image = (const void*)(bias + phdr[i].p_vaddr);
        module.filesz = (uint32_t)phdr[i].p_filesz;
        module.memsz = (uint32_t)phdr[i].p_memsz;
        module.align = (uint32_t)(phdr[i].p_align ? phdr[i].p_align : 1);
    }
}

int CollectModules(const Auxv& aux, Module* out, int capacity) {
    int count = 0;
    // The program itself always comes first, with its headers from the auxiliary vector.
    out[count].bias = aux.bias;
    out[count].name = "";
    out[count].phdr = aux.phdr;
    out[count].phnum = aux.phnum;
    ReadTls(out[count], aux.bias, aux.phdr, aux.phnum);
    ++count;
    // Its dynamic section names the loader's debug structure, which heads the module list.
    uintptr_t debugAddress = 0;
    const ElfW(Phdr)* phdr = (const ElfW(Phdr)*)aux.phdr;
    for (int i = 0; i < aux.phnum; ++i) {
        if (phdr[i].p_type != PT_DYNAMIC) continue;
        for (const ElfW(Dyn)* dyn = (const ElfW(Dyn)*)(aux.bias + phdr[i].p_vaddr); dyn->d_tag != DT_NULL; ++dyn)
            if (dyn->d_tag == DT_DEBUG) debugAddress = (uintptr_t)dyn->d_un.d_ptr;
    }
    const DebugStructure* debug = (const DebugStructure*)debugAddress;
    if (!debug || !debug->map) return count;  // a static program: there is nothing else loaded
    for (const LinkMap* map = debug->map->next; map && count < capacity; map = map->next) {
        const ElfW(Ehdr)* header = (const ElfW(Ehdr)*)map->address;
        if (!header || memcmp(header->e_ident, ELFMAG, SELFMAG) || header->e_phnum == 0) continue;
        if (header->e_phentsize != sizeof(ElfW(Phdr))) continue;
        Module& module = out[count];
        module.bias = map->address;
        module.name = map->name ? map->name : "";
        module.phdr = map->address + header->e_phoff;
        module.phnum = header->e_phnum;
        ReadTls(module, module.bias, module.phdr, module.phnum);
        ++count;
    }
    return count;
}

// The loader's slot ids, which are what a general-dynamic access names: modules with a PT_TLS segment
// are numbered from 1 in load order, and a module without one has no slot at all.
void NumberModules(int count) {
    int next = 1;
    for (int i = 0; i < count; ++i) layout.module[i].tlsModid = layout.module[i].memsz ? next++ : 0;
}

// Declared here for the layout's own confirmation below; defined with the keys further down.
extern __thread void* keyValues[kKeyBlock] __attribute__((tls_model("initial-exec")));

// The first thread's dtv, as glibc wrote it: a slot count and a generation word in front of the entries,// then one 16-byte entry (the block's address, then a flag) per module *with thread-local storage*, in
// load order. A module without a PT_TLS segment gets no slot.
struct Dtv {
    uintptr_t* entries;
    size_t slots;
};
Dtv CurrentDtv() {
    uintptr_t* dtv = *(uintptr_t**)(ThreadPointer() + kTcbDtv);
    if (!dtv) return {nullptr, 0};
    const size_t slots = *(const uintptr_t*)((const char*)dtv - kDtvEntryBytes);
    if (slots < 2 || slots > (1u << 20)) return {nullptr, 0};
    return {dtv, slots};
}
inline uintptr_t DtvBlock(const Dtv& dtv, size_t index) {
    return index < dtv.slots ? *(const uintptr_t*)((const char*)dtv.entries + kDtvEntryBytes * index) : 0;
}

void FinishLayout() {
    const uintptr_t tp = ThreadPointer();
    // The loader's walk, measured against a running glibc 2.44 process: the last module in the list (the
    // program itself) owns the highest block, which ends at the thread pointer because that is what the
    // linker's local-exec offsets count back from, and every other module follows it downwards in
    // reverse list order. A module without a PT_TLS segment takes no room. The total is rounded up to the
    // widest alignment any module asks for -- not to the control block's own -- so the program's block can
    // start a few bytes below the thread pointer (a 100-byte, 16-aligned block began at fs-0x90 there).
    long offset = 0, maxAlign = 1;
    long computed[kMaxModules] = {};
    for (int i = layout.modules - 1; i >= 0; --i) {
        const Module& m = layout.module[i];
        if (!m.memsz) continue;
        if ((long)m.align > maxAlign) maxAlign = m.align;
        offset = RoundUp(offset, m.align);
        computed[i] = offset;
        offset += m.memsz;
    }
    const long total = RoundUp(offset, maxAlign);
    layout.staticSize = total;
    for (int i = 0; i < layout.modules; ++i)
        layout.module[i].fromThreadPointer = layout.module[i].memsz ? computed[i] - total : 0;
    NumberModules(layout.modules);
    layout.exeModule = layout.module[0].tlsModid;
    layout.exeFromThreadPointer = layout.module[0].fromThreadPointer;
    // What the running process already holds, to confirm it: the first thread's slot table names every
    // static block it has, and those addresses must be exactly the ones the walk just worked out. A
    // module loaded later with thread-local storage of its own has an allocation elsewhere and is not
    // part of this area, so only entries inside it are compared.
    const Dtv dtv = CurrentDtv();
    if (dtv.entries && total > 0) {
        int matched[kMaxModules] = {};
        bool extra = false;
        for (size_t i = 1; i < dtv.slots; ++i) {
            const uintptr_t block = DtvBlock(dtv, i);
            if (!block) continue;
            if (block < tp - (uintptr_t)total || block >= tp) continue;
            bool found = false;
            for (int m = 0; m < layout.modules; ++m)
                if (layout.module[m].memsz && (uintptr_t)(tp + layout.module[m].fromThreadPointer) == block) {
                    matched[m] = 1;
                    found = true;
                    break;
                }
            if (!found) extra = true;
        }
        layout.observed = true;
        layout.agrees = !extra;
        for (int m = 0; m < layout.modules; ++m)
            if (layout.module[m].memsz && !matched[m]) layout.agrees = false;
    }
    layout.tcbAsExpected = *(const uintptr_t*)(tp + kTcbSelf) == tp &&
                           *(const uintptr_t*)(tp + kTcbSelfAgain) == tp;
    // A second, independent confirmation: a thread-local the *linker* placed (its access is a plain
    // fs-relative offset, not this file's arithmetic) has to land inside the program's block here.
    const uintptr_t own = (uintptr_t)&keyValues[0];
    layout.ownTlsInside = own >= tp + (uintptr_t)layout.exeFromThreadPointer &&
                          own < tp + (uintptr_t)layout.exeFromThreadPointer + layout.module[0].memsz;
    layout.errnoFromThreadPointer = (long)((char*)__errno_location() - (char*)tp);
}

void FindLayout(int, char**, char** environment) {
    Guard g(layoutLock);
    if (layout.staticSize) return;
    if (!environment) return;
    char** p = environment;
    while (*p) ++p;
    for (const unsigned long* entry = (const unsigned long*)(p + 1); entry[0]; entry += 2) {
        if (entry[0] == AT_PHDR) auxv.phdr = entry[1];
        else if (entry[0] == AT_PHNUM) auxv.phnum = (int)entry[1];
    }
    // The program's load bias: the file's program headers sit at their link-time address plus it.
    const ElfW(Phdr)* phdr = (const ElfW(Phdr)*)auxv.phdr;
    for (int i = 0; i < auxv.phnum; ++i)
        if (phdr[i].p_type == PT_PHDR) auxv.bias = auxv.phdr - phdr[i].p_vaddr;
#if defined(CORDINAL_STATIC)
    // A static-PIE link has no PT_PHDR segment, so the bias comes from the startup code that applied
    // the image's own relocations (start.cpp, through loader.cpp).
    if (!auxv.bias) auxv.bias = cordlibc_LoaderBias();
#endif
    layout.modules = CollectModules(auxv, layout.module, kMaxModules);
    if (!layout.modules) return;
    FinishLayout();
}

__attribute__((constructor(101))) void StartLayout(int argc, char** argv, char** environment) {
    FindLayout(argc, argv, environment);
}

// ---------------------------------------------------------------------------------------------------
// A thread: the descriptor this file keeps, and the clone trampoline that starts it.

struct Thread {
    uint64_t magic;
    uintptr_t threadPointer;
    void* mapBase;
    size_t mapSize;
    void* stackAddress;
    size_t stackSize;
    size_t guardSize;
    int tid;  // the kernel's; CLONE_CHILD_CLEARTID clears it as the thread ends
    int detached;
    int schedulerPolicy;  // the attr's, when it named one
    int schedulerPriority;
    void* (*start)(void*);
    void* argument;
    void* retval;
    Thread* next;
    Thread* nextExited;
};
static_assert(kThreadSlot + sizeof(Thread) <= kTcbReserve, "the descriptor fits the reserve");

Lock registryLock;
Thread* registry;
Thread* exitedList;
int liveThreads;
int reclaimedThreads;

inline Thread* ThreadAt(uintptr_t tp) {
    Thread* thread = (Thread*)(tp + kThreadSlot);
    return thread->magic == kThreadMagic && thread->threadPointer == tp ? thread : nullptr;
}
Thread* Self() { return ThreadAt(ThreadPointer()); }

void UnlinkRegistry(Thread* thread) {
    Thread** link = &registry;
    while (*link && *link != thread) link = &(*link)->next;
    if (*link == thread) *link = thread->next;
    Thread** exited = &exitedList;
    while (*exited && *exited != thread) exited = &(*exited)->nextExited;
    if (*exited == thread) *exited = thread->nextExited;
    --liveThreads;
    if (liveThreads == 0) __libc_single_threaded = 1;
}

// Detached threads that have ended: the kernel cleared their tid word, so nothing runs on them and the
// mapping can go back. A thread cannot unmap its own stack, so it leaves this to the next creation or
// join; the list lock is what makes that safe.
void Reap() {
    for (;;) {
        Thread* gone = nullptr;
        void* base = nullptr;
        size_t size = 0;
        {
            Guard g(registryLock);
            Thread** link = &exitedList;
            while (*link) {
                Thread* candidate = *link;
                if (candidate->detached && __atomic_load_n(&candidate->tid, __ATOMIC_ACQUIRE) == 0) {
                    gone = candidate;
                    break;
                }
                link = &candidate->nextExited;
            }
            if (!gone) break;
            *link = gone->nextExited;
            Thread** entry = &registry;
            while (*entry && *entry != gone) entry = &(*entry)->next;
            if (*entry == gone) *entry = gone->next;
            --liveThreads;
            if (liveThreads == 0) __libc_single_threaded = 1;
            base = gone->mapBase;
            size = gone->mapSize;
        }
        Raw(__NR_munmap, (long)base, (long)size);
        ++reclaimedThreads;
    }
}

// Fills one thread's static TLS area from each module's init image, the way glibc's own thread start
// does, and installs the control block fields that differ from the first thread's.
void InitTls(uintptr_t tp, uintptr_t firstThreadPointer) {
    for (int i = 0; i < layout.modules; ++i) {
        const Module& m = layout.module[i];
        if (!m.memsz) continue;
        char* block = (char*)tp + m.fromThreadPointer;
        memset(block, 0, m.memsz);
        if (m.filesz) memcpy(block, m.image, m.filesz);
    }
    memcpy((void*)tp, (const void*)firstThreadPointer, kTcbHeaderCopy);
    *(uintptr_t*)(tp + kTcbSelf) = tp;
    *(uintptr_t*)(tp + kTcbSelfAgain) = tp;
    *(int*)(tp + kTcbMultipleThreads) = 1;
    *(uintptr_t*)(tp + kTcbDtv) = 0;
}

// A copy of the first thread's dtv that points at this thread's own blocks. Entries for blocks inside
// the static area move to the matching place; a block a loaded module allocated for itself is left
// unallocated, as any fresh thread's would be, so glibc's own lazy code would fill it.
size_t DtvBytes() {
    const Dtv dtv = CurrentDtv();
    return dtv.entries ? (dtv.slots + 2) * kDtvEntryBytes : 0;
}
void CopyDtv(uintptr_t tp, uintptr_t firstThreadPointer, void* storage) {
    const Dtv source = CurrentDtv();
    if (!source.entries) return;
    char* copy = (char*)storage + 2 * kDtvEntryBytes;  // room for the count and the generation
    memcpy(copy - 2 * kDtvEntryBytes, (const char*)source.entries - 2 * kDtvEntryBytes,
           (source.slots + 2) * kDtvEntryBytes);
    const uintptr_t firstArea = firstThreadPointer - (uintptr_t)layout.staticSize;
    const uintptr_t area = tp - (uintptr_t)layout.staticSize;
    for (size_t i = 1; i < source.slots; ++i) {
        const uintptr_t block = DtvBlock(source, i);
        if (block && layout.staticSize && block >= firstArea && block < firstThreadPointer) {
            *(uintptr_t*)(copy + kDtvEntryBytes * i) = area + (block - firstArea);
        } else if (block) {
            *(uintptr_t*)(copy + kDtvEntryBytes * i) = 0;
            *(uintptr_t*)(copy + kDtvEntryBytes * i + sizeof(uintptr_t)) = 0;
        }
    }
    *(uintptr_t*)(tp + kTcbDtv) = (uintptr_t)copy;
}

void RunThreadDtors();
void RunKeyDestructors();

// The first thing a new thread runs: everything it needs is reachable from its own thread pointer.
extern "C" HIDDEN __attribute__((noreturn)) void cordlibc_thread_start() {
    Thread* thread = Self();
    if (!thread) abort();
    if (thread->schedulerPolicy >= 0) {
        const int priority = thread->schedulerPriority;
        Raw(__NR_sched_setscheduler, GetTid(), thread->schedulerPolicy, (long)&priority);
    }
    thread->tid = GetTid();
    void* result = thread->start(thread->argument);
    // The C++ thread_local destructors, then the POSIX key destructors, as glibc's thread end does.
    RunThreadDtors();
    RunKeyDestructors();
    thread->retval = result;
    {
        Guard g(registryLock);
        thread->nextExited = exitedList;
        exitedList = thread;
    }
    Raw(__NR_exit, 0);  // the kernel clears the tid word and wakes the joiner
    __builtin_unreachable();
}

// clone's child continues on the new stack and must not return into this frame, so the call into
// cordlibc_thread_start sits in assembly. The kernel's fourth argument (the child tid) is in r10.
extern "C" HIDDEN long cordlibc_clone(long flags, void* stack, int* parentTid, int* childTid, void* tls);
__asm__(
    ".text\n"
    ".globl cordlibc_clone\n"
    ".hidden cordlibc_clone\n"
    ".type cordlibc_clone, @function\n"
    "cordlibc_clone:\n"
    "    movq %rcx, %r10\n"
    "    andq $-16, %rsi\n"
    "    movq $56, %rax\n"  // __NR_clone
    "    syscall\n"
    "    testq %rax, %rax\n"
    "    jne 1f\n"
    "    xorl %ebp, %ebp\n"
    "    call cordlibc_thread_start\n"
    "    hlt\n"
    "1:\n"
    "    ret\n"
    ".size cordlibc_clone, .-cordlibc_clone\n");

// ---------------------------------------------------------------------------------------------------
// Keys and thread-local destructors.

void (*keyDestructor[kKeys])(void*);
bool keyUsed[kKeys];
pthread_key_t nextKey;
Lock keyLock;

// A thread's key values: the first 32 in its own storage, the rest in an array from the allocator the
// first time one of them is set (as glibc does, for the same reason).
__thread void* keyValues[kKeyBlock] __attribute__((tls_model("initial-exec")));
__thread void** keyValuesRest __attribute__((tls_model("initial-exec")));

struct ThreadDtor {
    ThreadDtor* next;
    void (*function)(void*);
    void* object;
};
__thread ThreadDtor* threadDtors __attribute__((tls_model("initial-exec")));
bool mainThreadDtorsRegistered;

void RunThreadDtors() {
    for (;;) {
        ThreadDtor* dtor = threadDtors;
        if (!dtor) return;
        threadDtors = dtor->next;
        dtor->function(dtor->object);
        free(dtor);
    }
}

// The first thread's thread_local destructors run as the program exits; glibc registers its own way of
// doing that, and a static program's exit path walks the same atexit list, so one registration serves
// both builds.
void RunMainThreadDtors(void*) { RunThreadDtors(); }

void StoreKey(int key, void* value) {
    if (key < kKeyBlock) keyValues[key] = value;
    else if (keyValuesRest) keyValuesRest[key - kKeyBlock] = value;
}
void* LoadKey(int key) {
    if (key < kKeyBlock) return keyValues[key];
    return keyValuesRest ? keyValuesRest[key - kKeyBlock] : nullptr;
}

void RunKeyDestructors() {
    for (int pass = 0; pass < kDestructorPasses; ++pass) {
        bool any = false;
        for (int key = 0; key < kKeys; ++key) {
            void (*dtor)(void*) = keyDestructor[key];
            if (!dtor) continue;
            void* value = LoadKey(key);
            if (!value) continue;
            StoreKey(key, nullptr);
            dtor(value);
            any = true;
        }
        if (!any) break;
    }
    if (keyValuesRest) {
        free(keyValuesRest);
        keyValuesRest = nullptr;
    }
}

// ---------------------------------------------------------------------------------------------------
// The mutex: glibc's 40-byte object, its own field offsets, and its futex protocol.

constexpr int kMutexRecursive = 1;
constexpr int kMutexErrorCheck = 2;
struct MutexMemory {
    int lock;
    unsigned count;
    int owner;
    unsigned users;
    int kind;
    short spins;
    short elision;
    void* previous;
    void* next;
};
static_assert(sizeof(MutexMemory) == 40, "glibc's mutex is 40 bytes on x86-64");
static_assert(sizeof(pthread_mutex_t) == sizeof(MutexMemory), "the caller's mutex object is this one");
static_assert(offsetof(MutexMemory, kind) == 16, "the kind sits where the static initializers put it");

inline MutexMemory* Mutex(pthread_mutex_t* mutex) { return (MutexMemory*)mutex; }

void MutexAcquire(MutexMemory* m) {
    int expected = 0;
    if (__atomic_compare_exchange_n(&m->lock, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
    for (int i = 0; i < 64; ++i) {
        __builtin_ia32_pause();
        expected = 0;
        if (__atomic_load_n(&m->lock, __ATOMIC_RELAXED) == 0 &&
            __atomic_compare_exchange_n(&m->lock, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return;
    }
    while (__atomic_exchange_n(&m->lock, 2, __ATOMIC_ACQUIRE) != 0) FutexWait(&m->lock, 2, nullptr);
}

// ---------------------------------------------------------------------------------------------------
// The condition variable: glibc's 48-byte object, holding a sequence number every signal bumps and a
// count of the waiters that have not been signalled yet. No internal lock is needed: a waiter takes its
// place, re-reads the sequence, and only then sleeps, so a signal either lands in the sequence before
// that read or arrives while the waiter is counted.

struct CondMemory {
    unsigned sequence;  // the futex word
    unsigned waiters;
    int padding[10];
};
static_assert(sizeof(CondMemory) == 48, "glibc's condition variable is 48 bytes on x86-64");
static_assert(sizeof(pthread_cond_t) == sizeof(CondMemory), "the caller's condition variable is this one");
inline CondMemory* Cond(pthread_cond_t* cond) { return (CondMemory*)cond; }

void CondCreditBack(CondMemory* c) {
    for (;;) {
        unsigned waiters = __atomic_load_n(&c->waiters, __ATOMIC_RELAXED);
        if (!waiters) return;
        if (__atomic_compare_exchange_n(&c->waiters, &waiters, waiters - 1, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            return;
    }
}

int CondWait(pthread_cond_t* cond, pthread_mutex_t* mutex, int clockId, const struct timespec* deadline) {
    CondMemory* c = Cond(cond);
    const unsigned before = __atomic_load_n(&c->sequence, __ATOMIC_ACQUIRE);
    __atomic_fetch_add(&c->waiters, 1, __ATOMIC_RELAXED);
    if (__atomic_load_n(&c->sequence, __ATOMIC_ACQUIRE) != before) {
        // A signal arrived between taking the count and confirming it. It is this waiter's, and POSIX
        // allows returning as woken even if it was meant for another one.
        CondCreditBack(c);
        return 0;
    }
    pthread_mutex_unlock(mutex);
    bool signalled = false;
    for (;;) {
        if (__atomic_load_n(&c->sequence, __ATOMIC_ACQUIRE) != before) {
            signalled = true;
            break;
        }
        if (!deadline) {
            FutexWait((int*)&c->sequence, (int)before, nullptr);
            continue;
        }
        struct timespec now;
        clock_gettime(clockId, &now);
        const long long left = (long long)(deadline->tv_sec - now.tv_sec) * 1000000000ll +
                               (long long)(deadline->tv_nsec - now.tv_nsec);
        if (left <= 0) break;  // out of time, and not signalled: the last check above said so
        struct timespec wait{left / 1000000000ll, left % 1000000000ll};
        if (FutexWaitResult((int*)&c->sequence, (int)before, &wait) != ETIMEDOUT) continue;
        if (__atomic_load_n(&c->sequence, __ATOMIC_ACQUIRE) != before) {
            signalled = true;
            break;
        }
        break;
    }
    if (!signalled) {
        // Give back this waiter's place, unless a signal took it in the meantime, which makes the
        // return a woken one after all. Either way a later signal is never spent on nobody.
        for (;;) {
            if (__atomic_load_n(&c->sequence, __ATOMIC_ACQUIRE) != before) {
                signalled = true;
                break;
            }
            unsigned waiters = __atomic_load_n(&c->waiters, __ATOMIC_RELAXED);
            if (!waiters) {
                signalled = true;
                break;
            }
            if (__atomic_compare_exchange_n(&c->waiters, &waiters, waiters - 1, false, __ATOMIC_RELAXED,
                                            __ATOMIC_RELAXED))
                break;
        }
    }
    pthread_mutex_lock(mutex);
    return signalled ? 0 : ETIMEDOUT;
}

// ---------------------------------------------------------------------------------------------------
// The rwlock: glibc's 56-byte object. Readers share it, a writer owns it alone and waits for the
// readers to leave, and readers may still join while a writer waits (glibc's default preference too).

struct RwlockMemory {
    int readers;  // the futex word a writer waits on
    int writer;   // 1 while a writer wants or owns it; the futex word readers wait on
    int owner;    // 1 once that writer has the lock (its readers have left)
    int padding[11];
};
static_assert(sizeof(RwlockMemory) == 56, "glibc's rwlock is 56 bytes on x86-64");
static_assert(sizeof(pthread_rwlock_t) == sizeof(RwlockMemory), "the caller's rwlock object is this one");
inline RwlockMemory* Rwlock(pthread_rwlock_t* lock) { return (RwlockMemory*)lock; }

// The attributes callers allocate: glibc's 56-byte object, as measured from pthread_attr_init and its
// setters (priority 0, policy 4, flags 8, guard size 16, stack end 24, stack size 32). The address field
// holds the *end* of the stack: since glibc 2.34 pthread_attr_getstack subtracts the size from it, which
// a probe of this glibc confirmed (a stack of 0x12345 bytes with a zero address there answered
// 0 - 0x12345).
struct AttrMemory {
    int schedulerPriority;
    int schedulerPolicy;
    int flags;
    int padding;
    size_t guardSize;
    void* stackEnd;
    size_t stackSize;
    void* cpuSet;
    size_t cpuSetSize;
};
static_assert(sizeof(AttrMemory) == 56, "glibc's attr is 56 bytes on x86-64");
static_assert(sizeof(pthread_attr_t) == sizeof(AttrMemory), "the caller's attr object is this one");
static_assert(offsetof(AttrMemory, guardSize) == 16 && offsetof(AttrMemory, stackSize) == 32,
              "the attr fields sit where the probe found them");
constexpr int kAttrDetached = 0x01;
constexpr int kAttrStackAddress = 0x08;
constexpr int kAttrPolicySet = 0x40;

}  // namespace

// ---------------------------------------------------------------------------------------------------
// The thread lifecycle.

CL int pthread_create(pthread_t* threadOut, const pthread_attr_t* attr, void* (*start)(void*), void* argument) noexcept {
    if (Missing(threadOut) || Missing((const void*)start)) return EINVAL;
    if (!layout.staticSize) return EAGAIN;  // the layout was not found: nothing here can be trusted
    Reap();
    const AttrMemory* a = (const AttrMemory*)attr;
    if (a && (a->flags & kAttrStackAddress) && a->stackEnd) return EINVAL;  // deprecated by POSIX
    size_t stackSize = a && a->stackSize ? a->stackSize : kDefaultStack;
    size_t guardSize = a ? a->guardSize : kDefaultGuard;
    if (stackSize < 16384) stackSize = 16384;
    if (guardSize && guardSize < 4096) guardSize = 4096;
    guardSize = (size_t)RoundUp((long)guardSize, 4096);
    if (guardSize > stackSize / 2) guardSize = 0;

    const size_t tlsSize = (size_t)RoundUp(layout.staticSize, 64);
    const size_t dtvBytes = DtvBytes();
    size_t mapSize = guardSize + stackSize + tlsSize + kTcbReserve + dtvBytes + 128;
    mapSize = (size_t)RoundUp((long)mapSize, 4096);
    char* base = (char*)Raw(__NR_mmap, 0, (long)mapSize, kProtReadWrite, (long)kMapPrivateAnonymousStack, -1, 0);
    if ((uintptr_t)base >= (uintptr_t)-4095L) {
        errno = ENOMEM;
        return EAGAIN;
    }
    if (guardSize) Raw(__NR_mprotect, (long)base, (long)guardSize, kProtNone);
    char* stackTop = base + guardSize + stackSize;
    char* tlsArea = (char*)RoundUp((long)stackTop, 64);
    const uintptr_t tp = (uintptr_t)tlsArea + tlsSize;
    Thread* thread = (Thread*)(tp + kThreadSlot);
    memset(thread, 0, sizeof *thread);
    thread->magic = kThreadMagic;
    thread->threadPointer = tp;
    thread->mapBase = base;
    thread->mapSize = mapSize;
    thread->stackAddress = base + guardSize;
    thread->stackSize = stackSize;
    thread->guardSize = guardSize;
    thread->start = start;
    thread->argument = argument;
    thread->detached = a && (a->flags & kAttrDetached) ? 1 : 0;
    thread->schedulerPolicy = a && (a->flags & kAttrPolicySet) ? a->schedulerPolicy : -1;
    thread->schedulerPriority = a ? a->schedulerPriority : 0;
    InitTls(tp, ThreadPointer());
    if (dtvBytes) CopyDtv(tp, ThreadPointer(), (char*)(tp + kTcbReserve));
    {
        Guard g(registryLock);
        thread->next = registry;
        registry = thread;
        ++liveThreads;
        __libc_single_threaded = 0;
    }
    const int flags = kCloneVm | kCloneFs | kCloneFiles | kCloneSignalHandler | kCloneThread | kCloneSysVsem |
                      kCloneSetTls | kCloneParentSetTid | kCloneChildClearTid;
    const long child = cordlibc_clone(flags, stackTop, &thread->tid, &thread->tid, (void*)tp);
    if (child < 0) {
        {
            Guard g(registryLock);
            UnlinkRegistry(thread);
        }
        Raw(__NR_munmap, (long)base, (long)mapSize);
        return (int)-child;
    }
    *threadOut = (pthread_t)tp;
    return 0;
}

CL int pthread_join(pthread_t threadId, void** result) {
    const uintptr_t tp = (uintptr_t)threadId;
    if (tp == ThreadPointer()) return EDEADLK;
    Thread* thread;
    {
        Guard g(registryLock);
        thread = registry;
        while (thread && thread->threadPointer != tp) thread = thread->next;
        if (!thread) return ESRCH;
        if (thread->detached) return EINVAL;
    }
    for (;;) {
        const int tid = __atomic_load_n(&thread->tid, __ATOMIC_ACQUIRE);
        if (!tid) break;
        FutexWait(&thread->tid, tid, nullptr, true);
    }
    if (result) *result = thread->retval;
    void* base;
    size_t size;
    {
        Guard g(registryLock);
        UnlinkRegistry(thread);
        base = thread->mapBase;
        size = thread->mapSize;
    }
    Raw(__NR_munmap, (long)base, (long)size);
    ++reclaimedThreads;
    Reap();
    return 0;
}

CL int pthread_detach(pthread_t threadId) noexcept {
    const uintptr_t tp = (uintptr_t)threadId;
    if (tp == ThreadPointer()) return 0;  // the first thread is the process: it is never reclaimed
    Thread* thread;
    {
        Guard g(registryLock);
        thread = registry;
        while (thread && thread->threadPointer != tp) thread = thread->next;
        if (!thread) return ESRCH;
        __atomic_store_n(&thread->detached, 1, __ATOMIC_RELEASE);
    }
    Reap();
    return 0;
}

CL __attribute__((noreturn)) void pthread_exit(void* result) {
    Thread* thread = Self();
    RunThreadDtors();
    if (!thread) {  // the first thread: there is no other thread to return a value to
        Raw(__NR_exit_group, 0);
        __builtin_unreachable();
    }
    RunKeyDestructors();
    thread->retval = result;
    {
        Guard g(registryLock);
        thread->nextExited = exitedList;
        exitedList = thread;
    }
    Raw(__NR_exit, 0);
    __builtin_unreachable();
}

CL pthread_t pthread_self(void) noexcept { return (pthread_t)ThreadPointer(); }
CL int pthread_equal(pthread_t a, pthread_t b) noexcept { return a == b; }
CL int pthread_getattr_np(pthread_t threadId, pthread_attr_t* attr) noexcept {
    if (Missing(attr)) return EINVAL;
    AttrMemory* a = (AttrMemory*)attr;
    memset(a, 0, sizeof *a);
    const uintptr_t tp = (uintptr_t)threadId;
    Thread* thread;
    {
        Guard g(registryLock);
        thread = registry;
        while (thread && thread->threadPointer != tp) thread = thread->next;
        if (thread) {
            a->flags = thread->detached ? kAttrDetached : 0;
            a->guardSize = thread->guardSize;
            a->stackSize = thread->stackSize;
            a->stackEnd = (char*)thread->stackAddress + thread->stackSize;
            return 0;
        }
    }
    if (tp != ThreadPointer()) return ESRCH;
    // The first thread's stack is the process's: its size is the limit, and its end is the current frame
    // rounded up to a page, which is as much as anything here needs to know.
    unsigned long limit[2] = {0, 0};
    if (Raw(__NR_prlimit64, 0, kRlimitStack, 0, (long)limit) != 0 || limit[0] == 0) limit[0] = 8u << 20;
    const uintptr_t frame = (uintptr_t)__builtin_frame_address(0);
    a->guardSize = kDefaultGuard;
    a->stackSize = (size_t)limit[0];
    a->stackEnd = (void*)((frame + 4095) & ~(uintptr_t)4095);
    return 0;
}

CL int pthread_setschedparam(pthread_t threadId, int policy, const struct sched_param* param) noexcept {
    const uintptr_t tp = (uintptr_t)threadId;
    int tid;
    if (tp == ThreadPointer()) {
        tid = GetTid();
    } else {
        Thread* thread;
        {
            Guard g(registryLock);
            thread = registry;
            while (thread && thread->threadPointer != tp) thread = thread->next;
            if (!thread) return ESRCH;
            tid = __atomic_load_n(&thread->tid, __ATOMIC_ACQUIRE);
        }
        if (!tid) return ESRCH;
    }
    const int priority = Missing(param) ? 0 : param->sched_priority;
    const long r = Raw(__NR_sched_setscheduler, tid, policy, (long)&priority);
    return r < 0 ? (int)-r : 0;
}

CL int pthread_sigmask(int how, const sigset_t* set, sigset_t* old) noexcept {
    const long r = Raw(__NR_rt_sigprocmask, how, (long)set, (long)old, 8);
    return r < 0 ? (int)-r : 0;
}

CL int pthread_kill(pthread_t threadId, int signal) noexcept {
    const uintptr_t tp = (uintptr_t)threadId;
    int tid;
    if (tp == ThreadPointer()) {
        tid = GetTid();
    } else {
        Thread* thread;
        {
            Guard g(registryLock);
            thread = registry;
            while (thread && thread->threadPointer != tp) thread = thread->next;
            if (!thread) return ESRCH;
            tid = __atomic_load_n(&thread->tid, __ATOMIC_ACQUIRE);
        }
        if (!tid) return ESRCH;
    }
    const long r = Raw(__NR_tgkill, GetPid(), tid, signal);
    return r < 0 ? (int)-r : 0;
}

// Cancellation is not used here: a thread runs to its routine's end. The state is kept so a caller that
// sets and restores it sees its own values, and no thread ever ends in PTHREAD_CANCELED.
__thread int cancelState = PTHREAD_CANCEL_ENABLE;
CL int pthread_setcancelstate(int state, int* old) {
    if (state != PTHREAD_CANCEL_ENABLE && state != PTHREAD_CANCEL_DISABLE) return EINVAL;
    if (old) *old = cancelState;
    cancelState = state;
    return 0;
}

CL int pthread_once(pthread_once_t* once, void (*function)(void)) {
    const int state = __atomic_load_n(once, __ATOMIC_ACQUIRE);
    if (state == 2) return 0;
    int expected = 0;
    if (state == 0 && __atomic_compare_exchange_n(once, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        function();
        __atomic_store_n(once, 2, __ATOMIC_RELEASE);
        FutexWake(once, 0x7fffffff);
        return 0;
    }
    while (__atomic_load_n(once, __ATOMIC_ACQUIRE) != 2) FutexWait(once, 1, nullptr);
    return 0;
}

// The mutex family.
CL int pthread_mutex_lock(pthread_mutex_t* mutex) noexcept {
    MutexMemory* m = Mutex(mutex);
    const int kind = __atomic_load_n(&m->kind, __ATOMIC_RELAXED);
    int tid = 0;
    if (kind) {
        tid = GetTid();
        if (__atomic_load_n(&m->owner, __ATOMIC_RELAXED) == tid) {
            if (kind == kMutexRecursive) {
                ++m->count;
                return 0;
            }
            return EDEADLK;
        }
    }
    MutexAcquire(m);
    if (kind) {
        m->count = 1;
        __atomic_store_n(&m->owner, tid, __ATOMIC_RELAXED);
    }
    return 0;
}

CL int pthread_mutex_trylock(pthread_mutex_t* mutex) noexcept {
    MutexMemory* m = Mutex(mutex);
    const int kind = __atomic_load_n(&m->kind, __ATOMIC_RELAXED);
    int tid = 0;
    if (kind) {
        tid = GetTid();
        if (__atomic_load_n(&m->owner, __ATOMIC_RELAXED) == tid) {
            if (kind == kMutexRecursive) {
                ++m->count;
                return 0;
            }
            return EDEADLK;
        }
    }
    int expected = 0;
    if (!__atomic_compare_exchange_n(&m->lock, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return EBUSY;
    if (kind) {
        m->count = 1;
        __atomic_store_n(&m->owner, tid, __ATOMIC_RELAXED);
    }
    return 0;
}

CL int pthread_mutex_unlock(pthread_mutex_t* mutex) noexcept {
    MutexMemory* m = Mutex(mutex);
    const int kind = __atomic_load_n(&m->kind, __ATOMIC_RELAXED);
    if (kind) {
        if (__atomic_load_n(&m->owner, __ATOMIC_RELAXED) != GetTid()) return EPERM;
        if (kind == kMutexRecursive && --m->count) return 0;
        __atomic_store_n(&m->owner, 0, __ATOMIC_RELAXED);
    }
    if (__atomic_exchange_n(&m->lock, 0, __ATOMIC_RELEASE) == 2) FutexWake(&m->lock, 1);
    return 0;
}

CL int pthread_mutex_destroy(pthread_mutex_t*) noexcept { return 0; }

// The condition variable family.
CL int pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex) {
    return CondWait(cond, mutex, CLOCK_REALTIME, nullptr);
}

CL int pthread_cond_timedwait(pthread_cond_t* cond, pthread_mutex_t* mutex, const struct timespec* deadline) {
    if (Missing(deadline)) return EINVAL;
    return CondWait(cond, mutex, CLOCK_REALTIME, deadline);
}

CL int pthread_cond_clockwait(pthread_cond_t* cond, pthread_mutex_t* mutex, clockid_t clockId,
                              const struct timespec* deadline) {
    if (Missing(deadline)) return EINVAL;
    if (clockId != CLOCK_REALTIME && clockId != CLOCK_MONOTONIC && clockId != CLOCK_MONOTONIC_RAW &&
        clockId != CLOCK_BOOTTIME)
        return EINVAL;
    return CondWait(cond, mutex, clockId, deadline);
}

CL int pthread_cond_signal(pthread_cond_t* cond) noexcept {
    CondMemory* c = Cond(cond);
    for (;;) {
        unsigned waiters = __atomic_load_n(&c->waiters, __ATOMIC_RELAXED);
        if (!waiters) return 0;
        if (__atomic_compare_exchange_n(&c->waiters, &waiters, waiters - 1, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            break;
    }
    __atomic_fetch_add(&c->sequence, 1, __ATOMIC_RELEASE);
    FutexWake((int*)&c->sequence, 1);
    return 0;
}

CL int pthread_cond_broadcast(pthread_cond_t* cond) noexcept {
    CondMemory* c = Cond(cond);
    const unsigned waiters = __atomic_exchange_n(&c->waiters, 0, __ATOMIC_RELAXED);
    __atomic_fetch_add(&c->sequence, 1, __ATOMIC_RELEASE);
    if (waiters) FutexWake((int*)&c->sequence, (int)waiters);
    return 0;
}

CL int pthread_cond_destroy(pthread_cond_t*) noexcept { return 0; }

// The rwlock family. `writer` is the claim a writer makes before it waits for the readers to leave, so
// new readers stop arriving; `owner` says that wait is over and the unlock is the writer's. Without the
// second field, a reader that released while a writer was still waiting would be taken for the writer.
CL int pthread_rwlock_rdlock(pthread_rwlock_t* lock) noexcept {
    RwlockMemory* l = Rwlock(lock);
    for (;;) {
        if (__atomic_load_n(&l->writer, __ATOMIC_SEQ_CST) == 0) {
            __atomic_fetch_add(&l->readers, 1, __ATOMIC_SEQ_CST);
            if (__atomic_load_n(&l->writer, __ATOMIC_SEQ_CST) == 0) return 0;
            __atomic_fetch_sub(&l->readers, 1, __ATOMIC_SEQ_CST);
        }
        FutexWait(&l->writer, 1, nullptr);
    }
}

CL int pthread_rwlock_tryrdlock(pthread_rwlock_t* lock) noexcept {
    RwlockMemory* l = Rwlock(lock);
    if (__atomic_load_n(&l->writer, __ATOMIC_SEQ_CST)) return EBUSY;
    __atomic_fetch_add(&l->readers, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&l->writer, __ATOMIC_SEQ_CST) == 0) return 0;
    __atomic_fetch_sub(&l->readers, 1, __ATOMIC_SEQ_CST);
    return EBUSY;
}

CL int pthread_rwlock_wrlock(pthread_rwlock_t* lock) noexcept {
    RwlockMemory* l = Rwlock(lock);
    int expected = 0;
    while (!__atomic_compare_exchange_n(&l->writer, &expected, 1, false, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) {
        expected = 0;
        FutexWait(&l->writer, 1, nullptr);
    }
    for (;;) {
        const int readers = __atomic_load_n(&l->readers, __ATOMIC_SEQ_CST);
        if (readers == 0) break;
        FutexWait(&l->readers, readers, nullptr);
    }
    __atomic_store_n(&l->owner, 1, __ATOMIC_RELEASE);
    return 0;
}

CL int pthread_rwlock_trywrlock(pthread_rwlock_t* lock) noexcept {
    RwlockMemory* l = Rwlock(lock);
    int expected = 0;
    if (!__atomic_compare_exchange_n(&l->writer, &expected, 1, false, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) return EBUSY;
    if (__atomic_load_n(&l->readers, __ATOMIC_SEQ_CST) == 0) {
        __atomic_store_n(&l->owner, 1, __ATOMIC_RELEASE);
        return 0;
    }
    __atomic_store_n(&l->writer, 0, __ATOMIC_RELEASE);
    FutexWake(&l->writer, 0x7fffffff);
    return EBUSY;
}

CL int pthread_rwlock_unlock(pthread_rwlock_t* lock) noexcept {
    RwlockMemory* l = Rwlock(lock);
    if (__atomic_load_n(&l->owner, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&l->owner, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&l->writer, 0, __ATOMIC_RELEASE);
        FutexWake(&l->writer, 0x7fffffff);
        return 0;
    }
    if (__atomic_fetch_sub(&l->readers, 1, __ATOMIC_SEQ_CST) == 1) FutexWake(&l->readers, 0x7fffffff);
    return 0;
}

CL int pthread_rwlock_destroy(pthread_rwlock_t*) noexcept { return 0; }

// Keys.
CL int pthread_key_create(pthread_key_t* key, void (*destructor)(void*)) noexcept {
    if (Missing(key)) return EINVAL;
    Guard g(keyLock);
    for (int i = 0; i < kKeys; ++i) {
        const int candidate = (int)((nextKey + (unsigned)i) % kKeys);
        if (keyUsed[candidate]) continue;
        keyUsed[candidate] = true;
        keyDestructor[candidate] = destructor;
        nextKey = (pthread_key_t)((candidate + 1) % kKeys);
        *key = (pthread_key_t)candidate;
        return 0;
    }
    return EAGAIN;
}

CL int pthread_key_delete(pthread_key_t key) noexcept {
    if (key >= kKeys) return EINVAL;
    Guard g(keyLock);
    if (!keyUsed[key]) return EINVAL;
    keyUsed[key] = false;
    keyDestructor[key] = nullptr;
    return 0;
}

CL void* pthread_getspecific(pthread_key_t key) noexcept {
    if (key >= kKeys) return nullptr;
    return LoadKey((int)key);
}

CL int pthread_setspecific(pthread_key_t key, const void* value) noexcept {
    if (key >= kKeys || !keyUsed[key]) return EINVAL;
    if (key >= kKeyBlock && !keyValuesRest) {
        void** rest = (void**)calloc(kKeys - kKeyBlock, sizeof(void*));
        if (!rest) return ENOMEM;
        keyValuesRest = rest;
    }
    StoreKey((int)key, const_cast<void*>(value));
    return 0;
}

// The C++ thread_local destructors. glibc's own version keeps the same per-thread list; the first
// thread's runs from the program's exit list, which both builds walk.
CL int __cxa_thread_atexit_impl(void (*function)(void*), void* object, void*) {
    ThreadDtor* dtor = (ThreadDtor*)malloc(sizeof(ThreadDtor));
    if (!dtor) return -1;
    dtor->function = function;
    dtor->object = object;
    dtor->next = threadDtors;
    threadDtors = dtor;
    if (!Self() && !mainThreadDtorsRegistered) {
        mainThreadDtorsRegistered = true;
        __cxa_atexit(RunMainThreadDtors, nullptr, &__dso_handle);
    }
    return 0;
}

// atfork. malloc.cpp registers the allocator's three hooks here; files.cpp's fork runs the lists.
struct ForkHandler {
    void (*prepare)();
    void (*parent)();
    void (*child)();
};
ForkHandler forkHandlers[kMaxForkHandlers];
int forkHandlerCount;
Lock forkLock;

CL int __register_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void), void*) {
    Guard g(forkLock);
    if (forkHandlerCount == kMaxForkHandlers) return ENOMEM;
    forkHandlers[forkHandlerCount++] = ForkHandler{prepare, parent, child};
    return 0;
}

CL int pthread_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void)) noexcept {
    return __register_atfork(prepare, parent, child, nullptr);
}

namespace cordlibc {

// The locks this file keeps across a fork, taken last so the allocator's own are held first and reset in
// the child. The child also keeps only its own thread: the others do not exist there.
HIDDEN void AtForkPrepare() {
    for (int i = forkHandlerCount - 1; i >= 0; --i)
        if (forkHandlers[i].prepare) forkHandlers[i].prepare();
    registryLock.Acquire();
    forkLock.Acquire();
    keyLock.Acquire();
    layoutLock.Acquire();
}

HIDDEN void AtForkParent() {
    layoutLock.Release();
    keyLock.Release();
    forkLock.Release();
    registryLock.Release();
    for (int i = 0; i < forkHandlerCount; ++i)
        if (forkHandlers[i].parent) forkHandlers[i].parent();
}

HIDDEN void AtForkChild() {
    registryLock.state = 0;
    forkLock.state = 0;
    keyLock.state = 0;
    layoutLock.state = 0;
    Thread* self = Self();
    registry = self;
    if (self) self->next = nullptr;
    exitedList = nullptr;
    liveThreads = self ? 1 : 0;
    __libc_single_threaded = liveThreads <= 1 ? 1 : 0;
    for (int i = 0; i < forkHandlerCount; ++i)
        if (forkHandlers[i].child) forkHandlers[i].child();
}

// What the checks read, in this order: the static TLS size, the program's block offset from the thread
// pointer, glibc's errno offset, the module count, whether the layout was observed in the running
// process and agreed with the reverse-order walk and found the control block's pointers where they were
// measured, whether a linker-placed thread-local landed inside the program's block, then the live and
// reclaimed thread counts and the key limit.
HIDDEN void ThreadsFacts(long* out) {
    out[0] = layout.staticSize;
    out[1] = layout.exeFromThreadPointer;
    out[2] = layout.errnoFromThreadPointer;
    out[3] = layout.modules;
    out[4] = layout.observed ? 1 : 0;
    out[5] = layout.agrees ? 1 : 0;
    out[6] = layout.tcbAsExpected ? 1 : 0;
    out[7] = __atomic_load_n(&liveThreads, __ATOMIC_RELAXED);
    out[8] = __atomic_load_n(&reclaimedThreads, __ATOMIC_RELAXED);
    out[9] = kKeys;
    out[10] = layout.ownTlsInside ? 1 : 0;
}

// A thread-local whose access goes through __tls_get_addr's general-dynamic path, so the checks can
// prove that path resolves to the calling thread's own copy.
__thread int tlsProbe __attribute__((tls_model("global-dynamic")));
HIDDEN int TlsProbe(int value) {
    const int previous = tlsProbe;
    tlsProbe = value;
    return previous;
}

// The first thread's thread_local destructors, for an exit path that wants them explicitly.
HIDDEN void RunMainThreadDtorsNow() { RunThreadDtors(); }

}  // namespace cordlibc

// libstdc++ skips atomic reference counts while this says the process has one thread. It starts true,
// becomes false on the first creation, and true again only once no other thread is left.
extern "C" {
__attribute__((visibility("hidden"))) char __libc_single_threaded = 1;
}

// The thread pointer's only caller outside this file: a general-dynamic access to thread-local storage
// defined in a module, which is the program itself or an object the static build's loader added.
struct TlsIndex {
    unsigned long module;
    unsigned long offset;
};
CL void* __tls_get_addr(struct TlsIndex* index) noexcept {
    if (index->module == (unsigned long)layout.exeModule)
        return (char*)ThreadPointer() + layout.exeFromThreadPointer + index->offset;
#if defined(CORDINAL_STATIC)
    if (void* block = LoaderTlsGet(index->module)) return (char*)block + index->offset;
#endif
    const Dtv dtv = CurrentDtv();
    const uintptr_t block = DtvBlock(dtv, index->module);
    if (block) return (char*)block + index->offset;
    // A module with dynamic thread-local storage this thread has no block for: no such module is loaded
    // in this program, and glibc's lazy allocation is what would fill one in.
    abort();
}

// The unwinder's view of the loaded objects, which is every module the loader has: each carries the
// header and the calling thread's TLS block, as glibc's own call reports them. A module loaded by the
// system keeps glibc's copy of this call.
CL int dl_iterate_phdr(int (*callback)(struct dl_phdr_info*, size_t, void*), void* data) {
    for (int i = 0; i < layout.modules; ++i) {
        const Module& m = layout.module[i];
        struct dl_phdr_info info;
        memset(&info, 0, sizeof info);
        info.dlpi_addr = m.bias;
        info.dlpi_name = (char*)m.name;
        info.dlpi_phdr = (const ElfW(Phdr)*)m.phdr;
        info.dlpi_phnum = (ElfW(Half))m.phnum;
#ifdef __GLIBC__
        info.dlpi_adds = 1;
        info.dlpi_subs = 0;
        info.dlpi_tls_modid = (size_t)m.tlsModid;
        info.dlpi_tls_data = m.memsz ? (void*)(ThreadPointer() + (uintptr_t)m.fromThreadPointer) : nullptr;
#endif
        const int result = callback(&info, sizeof info, data);
        if (result) return result;
    }
#if defined(CORDINAL_STATIC)
    // The modules the static build's loader added after the program started (loader.cpp).
    const int extra = LoaderPhdrWalk(callback, data);
    if (extra) return extra;
#endif
    return 0;
}

#if defined(CORDINAL_STATIC)
// The static program's own errno, in the program's thread-local block: the same place on every thread,
// so a call on a worker and a call on the first thread never share it.
namespace {
__thread int errnoStorage;
}
CL int* __errno_location(void) noexcept { return &errnoStorage; }

// libgcc's unwinder asks the loader where an address's object is, and a static program has no loader to
// ask. The result is glibc's own `struct dl_find_object` from dlfcn.h (no eh_dbase on x86-64), which is
// the layout glibc_compat.cpp's wrapper for older glibc also fills from the program headers.
CL int _dl_find_object(void* pc, struct dl_find_object* result) noexcept {
    const ElfW(Phdr)* phdr = (const ElfW(Phdr)*)layout.module[0].phdr;
    uintptr_t low = UINTPTR_MAX, high = 0;
    const void* ehFrame = nullptr;
    bool inside = false;
    for (int i = 0; i < layout.module[0].phnum; ++i) {
        const uintptr_t start = layout.module[0].bias + phdr[i].p_vaddr;
        if (phdr[i].p_type == PT_LOAD) {
            const uintptr_t end = start + phdr[i].p_memsz;
            if ((uintptr_t)pc >= start && (uintptr_t)pc < end) inside = true;
            if (start < low) low = start;
            if (end > high) high = end;
        } else if (phdr[i].p_type == PT_GNU_EH_FRAME) {
            ehFrame = (const void*)start;
        }
    }
    if (!inside) return -1;
    result->dlfo_flags = 0;
    result->dlfo_map_start = (void*)low;
    result->dlfo_map_end = (void*)high;
    result->dlfo_link_map = nullptr;
    result->dlfo_eh_frame = const_cast<void*>(ehFrame);
    return 0;
}
#endif
