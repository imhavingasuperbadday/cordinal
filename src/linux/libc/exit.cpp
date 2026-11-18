// The static program's exit path (stage 9 of replacing glibc): atexit and __cxa_atexit register
// handlers on one list, exit runs that list in reverse registration order, flushes cordinal's streams
// and ends in the exit_group system call, and __cxa_finalize runs every handler (or one shared
// object's part of them) without ending the program. abort and __stack_chk_fail end the other way,
// with SIGABRT. A static program has no glibc to start it and no glibc exit to end it, so this is the
// only exit path there. CMakeLists.txt links this file only into that build: a hidden atexit in the
// portable dynamic build would take stdio's final flush onto this list, which glibc's exit never
// walks, and that build still ends in glibc's __libc_start_main.
// The list is glibc's shape: a static block of 32 entries first, further blocks from the allocator,
// newest block first, and both kinds of handler in the one order they were registered in, so the C++
// static destructors and the C handlers interleave exactly as they do under glibc.
// No glibc header that declares these functions is included: the definitions are the declarations.
#include <errno.h>
#include <stddef.h>
#include "syscall.h"

using cordlibc::Call;

// The process-wide allocator (malloc.cpp, default visibility) and cordinal's own fflush (stdio.cpp,
// declared through the incomplete type here; nothing else of stdio is needed).
extern "C" void* malloc(size_t);
struct _IO_FILE;
extern "C" __attribute__((visibility("hidden"))) int fflush(_IO_FILE*);
// kernel.cpp's raw exit_group, already hidden in its own object, and the two calls abort needs on
// top of it: a signal to ourselves and, for a failed stack check, a message on fd 2.
extern "C" __attribute__((noreturn)) void _exit(int);
extern "C" int getpid(void);
extern "C" int kill(int, int);
extern "C" long write(int, const void*, size_t);

namespace {

// One futex for the list, the shape malloc.cpp and stdio.cpp use (0 free, 1 held, 2 held with
// waiters): registration can come from any thread, and a handler may run another thread's
// registration while exit walks the list. Handlers themselves run with the lock released, so a
// handler that registers another one cannot deadlock.
struct alignas(64) ListLock {
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
        while (__atomic_exchange_n(&state, 2, __ATOMIC_ACQUIRE) != 0) {
            Call(__NR_futex, (long)&state, 128 /* FUTEX_WAIT_PRIVATE */, 2, 0);
        }
    }
    void Release() {
        if (__atomic_exchange_n(&state, 0, __ATOMIC_RELEASE) == 2)
            Call(__NR_futex, (long)&state, 129 /* FUTEX_WAKE_PRIVATE */, 1);
    }
};

constexpr int kBlockEntries = 32;

enum EntryKind : unsigned char { kEmpty, kPlain, kWithArgument, kConsumed };

union Handler {
    void (*plain)();
    void (*withArgument)(void*);
};

struct Entry {
    Handler handler;
    void* argument;
    void* sharedObject;
    EntryKind kind;
};

struct Block {
    Block* next;
    int count;
    Entry entries[kBlockEntries];
};

// A static first block, so the first registrations need neither the allocator nor a thread (C++
// static constructors and libstdc++'s iostream setup run before any of cordinal's own code does).
Block firstBlock;
Block* newest = &firstBlock;
ListLock listLock;

// The newest handler still to run, taken off the list under the lock: with `sharedObject` the
// handlers __cxa_finalize was asked for (only __cxa_atexit ones, and only that object's), and with
// `everything` any handler at all. glibc marks a run entry free and skips it later, which keeps the
// newest-first order when a handler runs while others are still waiting.
Entry TakeNewest(void* sharedObject, bool everything) {
    Entry none{};
    listLock.Acquire();
    for (Block* block = newest; block; block = block->next) {
        for (int i = block->count - 1; i >= 0; --i) {
            Entry& entry = block->entries[i];
            if (entry.kind != kPlain && entry.kind != kWithArgument) continue;
            if (!everything && !(entry.kind == kWithArgument && entry.sharedObject == sharedObject)) continue;
            const Entry taken = entry;
            entry.kind = kConsumed;
            listLock.Release();
            return taken;
        }
    }
    listLock.Release();
    return none;
}

int Register(Handler handler, void* argument, void* sharedObject, EntryKind kind) {
    listLock.Acquire();
    if (newest->count == kBlockEntries) {
        Block* block = (Block*)malloc(sizeof(Block));
        if (!block) {
            listLock.Release();
            errno = ENOMEM;
            return -1;
        }
        block->next = newest;
        block->count = 0;
        newest = block;
    }
    Entry& entry = newest->entries[newest->count++];
    entry.handler = handler;
    entry.argument = argument;
    entry.sharedObject = sharedObject;
    entry.kind = kind;
    listLock.Release();
    return 0;
}

void Run(const Entry& entry, int status) {
    if (entry.kind == kPlain) {
        entry.handler.plain();
        return;
    }
    // glibc hands a __cxa_atexit handler the exit status as a second argument on top of the one the
    // Itanium ABI declares, so a handler that reads it sees the same value under either library.
    // x86-64 passes it in a register the one-argument signature simply ignores.
    typedef void (*WithStatus)(void*, int);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
    reinterpret_cast<WithStatus>(entry.handler.withArgument)(entry.argument, status);
#pragma GCC diagnostic pop
}

}  // namespace

#define CL extern "C" __attribute__((visibility("hidden")))

CL int atexit(void (*function)()) {
    Handler handler;
    handler.plain = function;
    return Register(handler, nullptr, nullptr, kPlain);
}

CL int __cxa_atexit(void (*function)(void*), void* argument, void* sharedObject) {
    Handler handler;
    handler.withArgument = function;
    return Register(handler, argument, sharedObject, kWithArgument);
}

// Every handler still waiting, or one object's: glibc's answers for the same argument. The program
// keeps running, and the streams are not flushed here, since glibc's __cxa_finalize flushes nothing
// either.
CL void __cxa_finalize(void* sharedObject) {
    for (;;) {
        const Entry entry = TakeNewest(sharedObject, sharedObject == nullptr);
        if (entry.kind == kEmpty) return;
        Run(entry, 0);
    }
}

// exit is a GCC builtin already declared with default visibility, so it is hidden in assembly and
// declared without the attribute, as _exit is in kernel.cpp.
__asm__(".hidden exit");
extern "C" __attribute__((noreturn)) void exit(int status) {
    for (;;) {
        const Entry entry = TakeNewest(nullptr, true);
        if (entry.kind == kEmpty) break;
        Run(entry, status);
    }
    // stdio registers its own flush with atexit on the first write; this covers a stream whose
    // registration the handler list has already run past, and costs one empty walk otherwise.
    fflush(nullptr);
    _exit(status);
}

// The other end of the same path, which a static program needs because cordinal's own fortified
// calls reach abort and the compiler's stack checks reach __stack_chk_fail. Glibc's abort unblocks
// SIGABRT (so a blocked signal cannot stop an abort), raises it, and ends with 127 if a handler
// returns. The mask is one 64-bit word on x86-64, with signal 6 in bit 5.
__asm__(".hidden abort");
extern "C" __attribute__((noreturn)) void abort(void) noexcept {
    const unsigned long signalSet = 1ul << (6 - 1);
    Call(__NR_rt_sigprocmask, 1 /* SIG_UNBLOCK */, &signalSet, nullptr, 8 /* _NSIG / 8 */);
    kill(getpid(), 6 /* SIGABRT */);
    _exit(127);
}

// Glibc's message, written straight to fd 2 rather than through stdio, then the same abort.
__asm__(".hidden __stack_chk_fail");
extern "C" __attribute__((noreturn)) void __stack_chk_fail(void) {
    static const char message[] = "*** stack smashing detected ***: terminated\n";
    write(2, message, sizeof(message) - 1);
    abort();
}

// The C++ runtime's key for the handlers it registers. A hosted link takes it from crtbegin, which
// the static link does not use, so this copy is weak and gives way to crtbegin's where there is one.
extern "C" {
__attribute__((weak, visibility("hidden"))) void* __dso_handle = &__dso_handle;
}
