// cordinal's threads and thread-local storage (threads.cpp), for checks.h: creating, joining and
// detaching threads and giving their stacks back, mutex contention with the recursive and errorcheck
// kinds, the three-phase condition-variable protocol with no lost wakeups and a timed wait that gives
// its place back, rwlock readers overlapping and a writer excluded, pthread_once under simultaneous
// entry, key and thread_local destructors, errno and the rest of glibc's thread-local storage on a
// cordinal thread, __libc_single_threaded, fork from a thread through the atfork lists, exceptions
// unwinding on a thread, and the attribute and signal-mask calls.
#pragma once
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace cordlibc {
// threads.cpp's report, in this order: the static TLS size, the program's thread-local block's offset
// from the thread pointer, glibc's errno offset, the module count, whether the layout was observed and
// agreed with the reverse-order walk and found the control block's pointers where they were measured,
// a thread-local the linker placed inside the program's block, then the live and reclaimed thread counts
// and the key limit. Eleven values.
__attribute__((visibility("hidden"))) void ThreadsFacts(long* out);
// A thread-local reached through the general-dynamic path (__tls_get_addr): sets it, returns the previous.
__attribute__((visibility("hidden"))) int TlsProbe(int value);
}  // namespace cordlibc

// libstdc++ reads this to skip atomic reference counts while the process has one thread.
extern "C" char __libc_single_threaded;
// The C++ thread_local destructor registration, cordinal's own since stage 8.
extern "C" __attribute__((visibility("hidden"))) int __cxa_thread_atexit_impl(void (*)(void*), void*, void*);

namespace libc_checks {

// A static program has no pthread_mutex_init or pthread_cond_init: the program itself builds both
// with the static initializers, so the checks use those there and glibc's calls in the dynamic build.
#if defined(CORDINAL_STATIC)
inline int InitMutex(pthread_mutex_t* mutex) {
    *mutex = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
    return 0;
}
inline int InitCond(pthread_cond_t* cond) {
    *cond = (pthread_cond_t)PTHREAD_COND_INITIALIZER;
    return 0;
}
#else
inline int InitMutex(pthread_mutex_t* mutex) { return pthread_mutex_init(mutex, nullptr); }
inline int InitCond(pthread_cond_t* cond) { return pthread_cond_init(cond, nullptr); }
#endif

// This process's virtual size in bytes, from /proc/self/statm: a thousand joined threads must not leave
// a thousand stacks behind.
inline long VirtualSize() {
    long pages = 0;
    if (FILE* f = fopen("/proc/self/statm", "r")) {
        if (fscanf(f, "%ld", &pages) != 1) pages = 0;
        fclose(f);
    }
    return pages * 4096;
}

// Milliseconds on the monotonic clock.
inline long long Milliseconds() {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

struct ThreadCount {
    std::atomic<int> entered{0};
    std::atomic<int> finished{0};
    void* result = nullptr;
};

inline void* CountedThread(void* pointer) {
    ThreadCount* state = (ThreadCount*)pointer;
    state->entered.fetch_add(1);
    state->finished.fetch_add(1);
    return state;
}

// Threads: create, join and detach, and the stack of each one coming back. A thousand creations one
// after another must not grow the process by a thousand stacks, and two hundred detached threads must
// have been given back before the report is taken.
inline void CheckThreadLifecycle(long* facts) {
    ThreadCount state;
    const long before = VirtualSize();
    cordlibc::ThreadsFacts(facts);
    const long reclaimedBefore = facts[8];
    int failures = 0;
    for (int i = 0; i < 1000; ++i) {
        pthread_t thread;
        if (pthread_create(&thread, nullptr, CountedThread, &state) != 0) {
            ++failures;
            continue;
        }
        void* result = nullptr;
        if (pthread_join(thread, &result) != 0 || result != &state) ++failures;
    }
    Expect(failures == 0, "a thousand threads were created, joined and returned their own value");
    Expect(state.finished.load() == 1000, "every churned thread ran");
    cordlibc::ThreadsFacts(facts);
    Expect(facts[8] - reclaimedBefore >= 1000, "a thousand thread mappings went back to the kernel");
    const long after = VirtualSize();
    Expect(after - before < (64 << 20), "the churn did not leave a thousand stacks mapped");

    // Detached threads: the one that ends cannot unmap its own stack, so the next creation does, and the
    // registry must show none of theirs left.
    std::atomic<int> detachedFinished{0};
    auto detached = [](void* pointer) -> void* {
        ((std::atomic<int>*)pointer)->fetch_add(1);
        return nullptr;
    };
    int created = 0;
    for (int i = 0; i < 200; ++i) {
        pthread_t thread;
        if (pthread_create(&thread, nullptr, detached, &detachedFinished) == 0 && pthread_detach(thread) == 0)
            ++created;
    }
    Expect(created == 200, "two hundred threads were created detached");
    for (int wait = 0; wait < 4000 && detachedFinished.load() != 200; ++wait) usleep(1000);
    Expect(detachedFinished.load() == 200, "every detached thread ran");
    pthread_t spare;
    if (pthread_create(&spare, nullptr, detached, &detachedFinished) == 0) pthread_join(spare, nullptr);
    cordlibc::ThreadsFacts(facts);
    Expect(facts[7] <= 2, "the detached threads' mappings were reclaimed");

    Expect(pthread_join(pthread_self(), nullptr) == EDEADLK, "joining oneself is EDEADLK");
    const pthread_t unknown = (pthread_t)0x1234;
    Expect(pthread_join(unknown, nullptr) == ESRCH, "joining an unknown thread is ESRCH");
    Expect(pthread_detach(unknown) == ESRCH, "detaching an unknown thread is ESRCH");
}

// The mutex: contention across threads, the recursive kind counting its own entries, and the errorcheck
// kind answering EDEADLK and EPERM as glibc does.
struct MutexWork {
    pthread_mutex_t* mutex;
    int rounds;
    long long* counter;
};

inline void* LockRounds(void* pointer) {
    MutexWork* work = (MutexWork*)pointer;
    for (int i = 0; i < work->rounds; ++i) {
        pthread_mutex_lock(work->mutex);
        *work->counter += 1;
        pthread_mutex_unlock(work->mutex);
    }
    return nullptr;
}

inline void CheckMutexes() {
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    long long counter = 0;
    MutexWork work{&mutex, 2000, &counter};
    pthread_t threads[6];
    int created = 0;
    for (pthread_t& thread : threads)
        if (pthread_create(&thread, nullptr, LockRounds, &work) == 0) ++created;
    Expect(created == 6, "six threads contend for one mutex");
    for (pthread_t thread : threads) pthread_join(thread, nullptr);
    Expect(counter == 6 * 2000, "every contended increment happened exactly once");

    pthread_mutex_lock(&mutex);
    Expect(pthread_mutex_trylock(&mutex) == EBUSY, "trylock of a held mutex is EBUSY");
    pthread_mutex_unlock(&mutex);
    Expect(pthread_mutex_trylock(&mutex) == 0 && pthread_mutex_unlock(&mutex) == 0, "trylock of a free mutex");

    pthread_mutex_t recursive = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
    Expect(pthread_mutex_lock(&recursive) == 0 && pthread_mutex_lock(&recursive) == 0,
           "a recursive mutex locks twice");
    Expect(pthread_mutex_trylock(&recursive) == 0, "a recursive mutex tries while its owner holds it");
    Expect(pthread_mutex_unlock(&recursive) == 0 && pthread_mutex_unlock(&recursive) == 0 &&
               pthread_mutex_unlock(&recursive) == 0,
           "a recursive mutex unlocks as many times as it was locked");
    Expect(pthread_mutex_trylock(&recursive) == 0 && pthread_mutex_unlock(&recursive) == 0,
           "a recursive mutex is free again");

    pthread_mutex_t errorcheck = PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP;
    Expect(pthread_mutex_lock(&errorcheck) == 0 && pthread_mutex_lock(&errorcheck) == EDEADLK,
           "an errorcheck mutex reports a second lock by its owner as EDEADLK");
    Expect(pthread_mutex_trylock(&errorcheck) == EDEADLK, "an errorcheck trylock reports EDEADLK too");
    Expect(pthread_mutex_unlock(&errorcheck) == 0, "an errorcheck mutex unlocks");
    Expect(pthread_mutex_unlock(&errorcheck) == EPERM, "unlocking a mutex nobody holds is EPERM");

    // A mutex another thread holds: it is busy, and free again once that thread released it.
    struct Handover {
        pthread_mutex_t* mutex;
        std::atomic<bool> held{false};
        std::atomic<bool> release{false};
    } handover{&errorcheck};
    auto holder = [](void* pointer) -> void* {
        Handover* h = (Handover*)pointer;
        if (pthread_mutex_lock(h->mutex) == 0) h->held = true;
        while (!h->release) usleep(200);
        pthread_mutex_unlock(h->mutex);
        return nullptr;
    };
    pthread_t thread;
    if (pthread_create(&thread, nullptr, holder, &handover) == 0) {
        while (!handover.held.load()) usleep(200);
        Expect(pthread_mutex_trylock(&errorcheck) == EBUSY, "a mutex another thread holds is EBUSY");
        handover.release = true;
        pthread_join(thread, nullptr);
    }
    Expect(pthread_mutex_trylock(&errorcheck) == 0 && pthread_mutex_unlock(&errorcheck) == 0,
           "the mutex is free once its holder released it");
}

// The condition variable: tokens handed out by a signaller and taken by waiters, which must add up
// exactly; a broadcast that wakes every waiter; and a timed wait that times out and gives its place
// back, so the next signal still reaches the next waiter. The waiters use a two-second deadline, so a
// lost wakeup fails the check instead of hanging it.
struct CondWork {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int tokens = 0;
    int consumed = 0;
    int timeouts = 0;
    bool stop = false;
    std::atomic<int> waiting{0};
};

inline void* CondWaiter(void* pointer) {
    CondWork* work = (CondWork*)pointer;
    work->waiting.fetch_add(1);
    for (;;) {
        pthread_mutex_lock(&work->mutex);
        while (work->tokens == 0 && !work->stop) {
            struct timespec deadline;
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_sec += 2;
            if (pthread_cond_timedwait(&work->cond, &work->mutex, &deadline) == ETIMEDOUT && work->tokens == 0 &&
                !work->stop)
                work->timeouts += 1;
        }
        if (work->stop) {
            pthread_mutex_unlock(&work->mutex);
            return nullptr;
        }
        work->tokens -= 1;
        work->consumed += 1;
        pthread_mutex_unlock(&work->mutex);
    }
}

inline void CheckConditionVariables() {
    // The classic protocol: the signaller takes the mutex, changes the predicate, signals and releases.
    CondWork work;
    InitMutex(&work.mutex);
    InitCond(&work.cond);
    pthread_t waiters[4];
    for (pthread_t& thread : waiters) pthread_create(&thread, nullptr, CondWaiter, &work);
    for (int spin = 0; spin < 4000 && work.waiting.load() != 4; ++spin) usleep(1000);
    const int kTokens = 5000;
    for (int i = 0; i < kTokens; ++i) {
        pthread_mutex_lock(&work.mutex);
        work.tokens += 1;
        pthread_cond_signal(&work.cond);
        pthread_mutex_unlock(&work.mutex);
        if ((i & 31) == 0) syscall(SYS_sched_yield);
    }
    for (int spin = 0; spin < 4000; ++spin) {
        pthread_mutex_lock(&work.mutex);
        const bool done = work.consumed == kTokens;
        pthread_mutex_unlock(&work.mutex);
        if (done) break;
        usleep(1000);
    }
    pthread_mutex_lock(&work.mutex);
    const int consumed = work.consumed;
    work.stop = true;
    pthread_cond_broadcast(&work.cond);
    pthread_mutex_unlock(&work.mutex);
    for (pthread_t thread : waiters) pthread_join(thread, nullptr);
    Expect(consumed == kTokens, "every signalled token was taken exactly once (no lost wakeups)");
    Expect(work.timeouts == 0, "no waiter timed out: no wakeup was lost");

    // A broadcast wakes every waiter that was waiting.
    struct Broadcast {
        pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
        pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
        int generation = 0;
        int woken = 0;
        std::atomic<int> waiting{0};
    } broadcast;
    auto broadcastWaiter = [](void* pointer) -> void* {
        Broadcast* b = (Broadcast*)pointer;
        pthread_mutex_lock(&b->mutex);
        const int generation = b->generation;
        b->waiting.fetch_add(1);
        while (b->generation == generation) pthread_cond_wait(&b->cond, &b->mutex);
        b->woken += 1;
        pthread_mutex_unlock(&b->mutex);
        return nullptr;
    };
    pthread_t broadcastThreads[6];
    for (pthread_t& thread : broadcastThreads) pthread_create(&thread, nullptr, broadcastWaiter, &broadcast);
    for (int spin = 0; spin < 4000 && broadcast.waiting.load() != 6; ++spin) usleep(1000);
    Expect(broadcast.waiting.load() == 6, "six threads wait on one condition variable");
    pthread_mutex_lock(&broadcast.mutex);
    broadcast.generation += 1;
    pthread_cond_broadcast(&broadcast.cond);
    pthread_mutex_unlock(&broadcast.mutex);
    for (pthread_t thread : broadcastThreads) pthread_join(thread, nullptr);
    Expect(broadcast.woken == 6, "a broadcast woke every waiter");

    // A timed wait that times out, then a signal for the next waiter: a timed-out waiter must have given
    // its place back, or that signal would be spent on nobody who is still waiting.
    CondWork timed;
    InitMutex(&timed.mutex);
    InitCond(&timed.cond);
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += 2;
    pthread_mutex_lock(&timed.mutex);
    const long long started = Milliseconds();
    const long long earlyDeadline = started + 60;
    struct timespec relative;
    clock_gettime(CLOCK_REALTIME, &relative);
    relative.tv_nsec += 60000000;
    if (relative.tv_nsec >= 1000000000) {
        relative.tv_sec += 1;
        relative.tv_nsec -= 1000000000;
    }
    const int result = pthread_cond_timedwait(&timed.cond, &timed.mutex, &relative);
    const long long took = Milliseconds() - started;
    pthread_mutex_unlock(&timed.mutex);
    Expect(result == ETIMEDOUT && took >= 40 && took < 2000, "a wait past its deadline returns ETIMEDOUT on time");
    Expect(earlyDeadline > 0 && deadline.tv_sec > 0, "the monotonic deadline was read");
    {
        CondWork waiter;
        waiter.mutex = timed.mutex;
        waiter.cond = timed.cond;
        pthread_t thread;
        pthread_create(&thread, nullptr, CondWaiter, &waiter);
        for (int spin = 0; spin < 4000 && waiter.waiting.load() != 1; ++spin) usleep(1000);
        pthread_mutex_lock(&waiter.mutex);
        waiter.tokens += 1;
        pthread_cond_signal(&waiter.cond);
        pthread_mutex_unlock(&waiter.mutex);
        // The waiter keeps waiting for the next token, so it is told to stop.
        usleep(20000);
        pthread_mutex_lock(&waiter.mutex);
        waiter.stop = true;
        pthread_cond_broadcast(&waiter.cond);
        pthread_mutex_unlock(&waiter.mutex);
        pthread_join(thread, nullptr);
        Expect(waiter.consumed == 1 && waiter.timeouts == 0,
               "the signal after a timed-out waiter woke the next waiter");
    }

    // A monotonic deadline through pthread_cond_clockwait: libstdc++'s wait_for and wait_until use the
    // same call, and the clock it names is the one the deadline is measured on.
    CondWork monotonic;
    InitMutex(&monotonic.mutex);
    InitCond(&monotonic.cond);
    struct MonotonicWork {
        CondWork* work;
        int result = -1;
    } monotonicWork{&monotonic};
    auto monotonicWaiter = [](void* pointer) -> void* {
        MonotonicWork* w = (MonotonicWork*)pointer;
        struct timespec deadline;
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_sec += 5;
        pthread_mutex_lock(&w->work->mutex);
        while (w->work->tokens == 0)
            w->result = pthread_cond_clockwait(&w->work->cond, &w->work->mutex, CLOCK_MONOTONIC, &deadline);
        w->work->tokens -= 1;
        pthread_mutex_unlock(&w->work->mutex);
        return nullptr;
    };
    pthread_t monotonicThread;
    pthread_create(&monotonicThread, nullptr, monotonicWaiter, &monotonicWork);
    usleep(30000);
    pthread_mutex_lock(&monotonic.mutex);
    monotonic.tokens += 1;
    pthread_cond_signal(&monotonic.cond);
    pthread_mutex_unlock(&monotonic.mutex);
    pthread_join(monotonicThread, nullptr);
    Expect(monotonicWork.result == 0, "a monotonic clockwait before its deadline returns 0");
    pthread_mutex_destroy(&monotonic.mutex);
    pthread_cond_destroy(&monotonic.cond);

    // libstdc++'s own condition variable, over cordinal's mutex and condition functions.
    std::mutex standardMutex;
    std::condition_variable standardCond;
    int value = 0;
    std::thread standardWaiter([&] {
        std::unique_lock<std::mutex> lock(standardMutex);
        const bool woken = standardCond.wait_for(lock, std::chrono::milliseconds(500), [&] { return value != 0; });
        if (woken) value += 1;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    {
        std::lock_guard<std::mutex> lock(standardMutex);
        value = 1;
    }
    standardCond.notify_one();
    standardWaiter.join();
    Expect(value == 2, "std::condition_variable over cordinal's mutex and condition functions");
    pthread_mutex_destroy(&work.mutex);
    pthread_cond_destroy(&work.cond);
    pthread_mutex_destroy(&timed.mutex);
    pthread_cond_destroy(&timed.cond);
}

// The rwlock: readers hold it together and a writer is alone. Readers check a pair the writer changes
// under its lock, so a torn read would show up, and a reader that is inside when a writer takes its turn
// would show up as well.
struct RwWork {
    pthread_rwlock_t lock = PTHREAD_RWLOCK_INITIALIZER;
    long first = 0;
    long second = 0;
    int torn = 0;
    std::atomic<int> readers{0};
};

inline void* RwReader(void* pointer) {
    RwWork* work = (RwWork*)pointer;
    for (int i = 0; i < 200; ++i) {
        pthread_rwlock_rdlock(&work->lock);
        work->readers.fetch_add(1);
        if (work->first != work->second) work->torn += 1;
        usleep(200);
        work->readers.fetch_sub(1);
        pthread_rwlock_unlock(&work->lock);
    }
    return nullptr;
}

inline void* RwWriter(void* pointer) {
    RwWork* work = (RwWork*)pointer;
    for (int i = 0; i < 100; ++i) {
        pthread_rwlock_wrlock(&work->lock);
        if (work->readers.load() != 0) work->torn += 1;  // a reader inside the writer's turn
        work->first += 1;
        usleep(100);
        work->second += 1;
        pthread_rwlock_unlock(&work->lock);
    }
    return nullptr;
}

inline void CheckRwlocks() {
    RwWork work;
    pthread_t readers[4];
    pthread_t writer;
    for (pthread_t& thread : readers) pthread_create(&thread, nullptr, RwReader, &work);
    pthread_create(&writer, nullptr, RwWriter, &work);
    // Readers can share it; a writer cannot join them, and cannot take it while they hold it.
    RwWork probe;
    pthread_rwlock_rdlock(&probe.lock);
    Expect(pthread_rwlock_trywrlock(&probe.lock) == EBUSY, "a writer cannot take a rwlock a reader holds");
    Expect(pthread_rwlock_tryrdlock(&probe.lock) == 0 && pthread_rwlock_unlock(&probe.lock) == 0,
           "another reader can take it while a reader holds it");
    pthread_rwlock_unlock(&probe.lock);
    Expect(pthread_rwlock_trywrlock(&probe.lock) == 0, "a writer takes a free rwlock");
    Expect(pthread_rwlock_tryrdlock(&probe.lock) == EBUSY, "a reader cannot take a rwlock a writer holds");
    Expect(pthread_rwlock_unlock(&probe.lock) == 0, "the writer releases it");
    for (pthread_t thread : readers) pthread_join(thread, nullptr);
    pthread_join(writer, nullptr);
    Expect(work.torn == 0, "no reader saw a half-written pair and no writer overlapped a reader");
    Expect(work.first == 100 && work.second == 100, "every writer turn happened");
}

// pthread_once under simultaneous entry: the initializer runs once, and every caller waits for it.
struct OnceWork {
    pthread_once_t once = PTHREAD_ONCE_INIT;
    std::atomic<int> runs{0};
    std::atomic<int> callers{0};
};
inline OnceWork onceWork;

inline void OnceInitializer() {
    onceWork.runs.fetch_add(1);
    usleep(20000);
}

inline void* OnceCaller(void*) {
    if (pthread_once(&onceWork.once, OnceInitializer) == 0) onceWork.callers.fetch_add(1);
    return nullptr;
}

inline void CheckOnce() {
    pthread_t threads[8];
    for (pthread_t& thread : threads) pthread_create(&thread, nullptr, OnceCaller, &onceWork);
    for (pthread_t thread : threads) pthread_join(thread, nullptr);
    Expect(onceWork.runs.load() == 1, "pthread_once ran its initializer once for eight simultaneous callers");
    Expect(onceWork.callers.load() == 8, "every pthread_once caller returned 0");
    Expect(pthread_once(&onceWork.once, OnceInitializer) == 0 && onceWork.runs.load() == 1,
           "a later pthread_once call runs nothing");
    static std::once_flag flag;
    static int count = 0;
    std::thread a([] { std::call_once(flag, [] { count += 1; }); });
    std::thread b([] { std::call_once(flag, [] { count += 1; }); });
    a.join();
    b.join();
    Expect(count == 1, "std::call_once runs once, on cordinal's pthread_once");
}

// Keys: a destructor runs for the thread that set the value and not for any other, and a destructor that
// sets the key again makes the following passes run, which is glibc's four-pass rule.
struct KeyState {
    pthread_key_t key = 0;
    std::atomic<int> calls{0};
    std::atomic<int> refills{0};
    void* seen = nullptr;
};
inline KeyState* keyState;

inline void RefillKey(void* value) {
    KeyState* state = keyState;
    if (!state) return;
    state->calls.fetch_add(1);
    state->seen = value;
    if (state->refills.fetch_sub(1) > 0) pthread_setspecific(state->key, (void*)(uintptr_t)0x5a5a);
}

inline void* SetKeyAndReturn(void* pointer) {
    KeyState* state = (KeyState*)pointer;
    pthread_setspecific(state->key, (void*)(uintptr_t)0x1234);
    return nullptr;
}

inline void CheckKeys() {
    KeyState state;
    Expect(pthread_key_create(&state.key, RefillKey) == 0, "a key is created");
    keyState = &state;
    pthread_t thread;
    pthread_create(&thread, nullptr, SetKeyAndReturn, &state);
    pthread_join(thread, nullptr);
    Expect(state.calls.load() == 1 && state.seen == (void*)(uintptr_t)0x1234,
           "the key's destructor ran once, for the thread that set it, with its value");

    // Four passes at most: two refills mean three destructor calls.
    KeyState refilling;
    Expect(pthread_key_create(&refilling.key, RefillKey) == 0, "a second key is created");
    keyState = &refilling;
    refilling.refills = 2;
    pthread_create(&thread, nullptr, SetKeyAndReturn, &refilling);
    pthread_join(thread, nullptr);
    Expect(refilling.calls.load() == 3, "a destructor that sets the key again runs on the following passes");

    pthread_key_t extra;
    Expect(pthread_key_create(&extra, nullptr) == 0, "a key with no destructor is created");
    Expect(pthread_setspecific(extra, (void*)1) == 0 && pthread_getspecific(extra) == (void*)1,
           "a key's value round trips");
    Expect(pthread_key_delete(extra) == 0, "a key is deleted");
    Expect(pthread_setspecific(extra, (void*)2) == EINVAL, "a deleted key refuses a value");
    Expect(pthread_key_create(&extra, nullptr) == 0 && extra != refilling.key && extra != state.key,
           "a new key gets its own slot");
    pthread_key_delete(extra);
    keyState = nullptr;
}

// thread_local storage: a destructor registered for a thread by this file runs as that thread ends, once,
// on the thread that registered it; a C++ thread_local object's destructor goes through the same list,
// because this program links the C++ runtime statically (a shared libstdc++ would bind to glibc's copy of
// the registration, and glibc's own thread end never runs on a cordinal thread); and a general-dynamic
// access (through __tls_get_addr) reaches the calling thread's own copy.
inline std::atomic<int> registeredDestructions{0};
inline std::atomic<int> objectDestructions{0};

inline void CountRegisteredDestruction(void* object) {
    if (object == (void*)&registeredDestructions) registeredDestructions.fetch_add(1);
}

struct LocalWithDestructor {
    ~LocalWithDestructor() { objectDestructions.fetch_add(1); }
};

struct TlsWork {
    int probeBefore = -1;
    int probeAfter = -1;
};

inline void* TouchTls(void* pointer) {
    TlsWork* work = (TlsWork*)pointer;
    work->probeBefore = cordlibc::TlsProbe(7);
    work->probeAfter = cordlibc::TlsProbe(9);
    __cxa_thread_atexit_impl(CountRegisteredDestruction, (void*)&registeredDestructions, nullptr);
    thread_local LocalWithDestructor local;
    (void)local;
    return nullptr;
}

inline void CheckThreadLocalStorage() {
    TlsWork work[3];
    pthread_t threads[3];
    for (int i = 0; i < 3; ++i) pthread_create(&threads[i], nullptr, TouchTls, &work[i]);
    for (int i = 0; i < 3; ++i) pthread_join(threads[i], nullptr);
    for (int i = 0; i < 3; ++i)
        Expect(work[i].probeBefore == 0 && work[i].probeAfter == 7,
               "a general-dynamic thread_local is private to the thread that wrote it");
    Expect(registeredDestructions.load() == 3,
           "a registered thread_local destructor ran once for each of three threads, on its own thread");
    Expect(objectDestructions.load() == 3, "a C++ thread_local object's destructor ran once per thread");
}

// errno and glibc's own thread-local storage on a cordinal thread: cordinal's calls and a call glibc still
// serves must both reach the calling thread's errno, and the first thread's must stay untouched.
struct ErrnoWork {
    long offset = 0;
    int cordinalError = 0;
    int systemError = 0;
    int strerrorBytes = 0;
};

// glibc's own open, taken from libc.so.6 (cordinal's is hidden in this program, so the handle's copy is
// the only one): a failing call of it writes its error into the calling thread's errno.
inline int (*SystemOpen())(const char*, int, ...) {
#if defined(CORDINAL_STATIC)
    // A static program has no glibc to load and ask for its own open, so this half of the errno
    // check has nothing to compare with.
    return nullptr;
#else
    static int (*openFunction)(const char*, int, ...) = nullptr;
    if (!openFunction) {
        void* libc = dlopen("libc.so.6", RTLD_LAZY | RTLD_NOLOAD);
        if (!libc) libc = dlopen("libc.so.6", RTLD_LAZY);
        if (libc) openFunction = (int (*)(const char*, int, ...))dlsym(libc, "open");
    }
    return openFunction;
#endif
}

inline void* ErrnoThread(void* pointer) {
    ErrnoWork* work = (ErrnoWork*)pointer;
    work->offset = (long)((char*)&errno - (char*)pthread_self());
    errno = 0;
    if (open("/cordinal-checks/missing", O_RDONLY) == -1) work->cordinalError = errno;
    errno = 0;
    int (*systemOpen)(const char*, int, ...) = SystemOpen();
    if (systemOpen && systemOpen("/cordinal-checks/missing", O_RDONLY) == -1) work->systemError = errno;
    // glibc's strerror reads its own tables, not errno: this is the call running on the thread rather
    // than about errno.
    const char* text = strerror(ENOENT);
    work->strerrorBytes = text ? (int)strlen(text) : 0;
    return nullptr;
}

inline void CheckThreadLocalErrors(long* facts) {
    errno = 4321;
    ErrnoWork work;
    pthread_t thread;
    pthread_create(&thread, nullptr, ErrnoThread, &work);
    pthread_join(thread, nullptr);
    Expect(work.cordinalError == ENOENT, "errno from cordinal's own call reaches the calling thread's storage");
#if defined(CORDINAL_STATIC)
    // A static program has no glibc open to make the second call with; the check above is the whole
    // of it there.
#else
    Expect(work.systemError == ENOENT, "glibc's own call writes errno into the same thread's storage");
#endif
    Expect(work.strerrorBytes > 0, "glibc's strerror runs on a cordinal thread");
    Expect(errno == 4321, "the first thread's errno is untouched by another thread's errors");
    const long mainOffset = (long)((char*)&errno - (char*)pthread_self());
    Expect(mainOffset == work.offset, "every thread keeps its errno at the same offset from its thread pointer");
    cordlibc::ThreadsFacts(facts);
    Expect(facts[2] == mainOffset, "the measured errno offset is the one the report gives");
}

// __libc_single_threaded: true while no other thread runs, false while one does, true again after it
// joins. libstdc++'s reference counts read it, and the allocator runs in both states.
inline std::atomic<bool> holdThread{true};
inline void* HeldThread(void*) {
    while (holdThread.load()) usleep(500);
    return nullptr;
}

inline void CheckSingleThreadedFlag() {
    holdThread = true;
    void* before = malloc(64);
    Expect(before != nullptr, "the allocator works while the process has one thread");
    pthread_t thread;
    if (pthread_create(&thread, nullptr, HeldThread, nullptr) != 0) {
        Expect(false, "a thread was created for the single-threaded flag");
        free(before);
        return;
    }
    for (int spin = 0; spin < 2000 && __libc_single_threaded != 0; ++spin) usleep(500);
    Expect(__libc_single_threaded == 0, "__libc_single_threaded is false while another thread runs");
    void* during = malloc(64);
    Expect(during != nullptr, "the allocator works while another thread runs");
    holdThread = false;
    pthread_join(thread, nullptr);
    Expect(__libc_single_threaded != 0, "__libc_single_threaded is true again once no other thread is left");
    void* after = malloc(64);
    Expect(after != nullptr, "the allocator works again with one thread");
    free(before);
    free(during);
    free(after);
}

// fork from a thread: the atfork lists run in POSIX's order, the allocator's locks are reset in the
// child, and the child can allocate, create a thread and leave on its own.
inline int forkOrder[8];
inline int forkOrderCount;
// The handlers stay registered for the rest of the process (there is no unregister call), so every
// later fork calls them too; the count has to stop at the array's end rather than write past it into
// whatever static comes next.
inline void RecordForkOrder(int value) {
    if (forkOrderCount < (int)(sizeof forkOrder / sizeof forkOrder[0])) forkOrder[forkOrderCount++] = value;
}
inline void ForkFirstPrepare() { RecordForkOrder(1); }
inline void ForkFirstParent() { RecordForkOrder(11); }
inline void ForkFirstChild() { RecordForkOrder(21); }
inline void ForkSecondPrepare() { RecordForkOrder(2); }
inline void ForkSecondParent() { RecordForkOrder(12); }
inline void ForkSecondChild() { RecordForkOrder(22); }

inline void* ChildAllocatorThread(void* pointer) {
    int* ok = (int*)pointer;
    void* blocks[32];
    for (int i = 0; i < 32; ++i) blocks[i] = malloc(64 + i * 61);
    for (int i = 0; i < 32; ++i) {
        if (!blocks[i]) *ok = 0;
        memset(blocks[i], i, 64 + i * 61);
    }
    for (int i = 0; i < 32; ++i) free(blocks[i]);
    return nullptr;
}

struct ForkWork {
    std::atomic<int> allocations{0};
};

inline void* ForkingThread(void* pointer) {
    ForkWork* work = (ForkWork*)pointer;
    void* warm[64];
    for (int i = 0; i < 64; ++i) warm[i] = malloc(48 + i * 29);  // fill this thread's allocator caches
    for (int i = 0; i < 64; ++i) free(warm[i]);
    int pipeFds[2];
    if (pipe(pipeFds) != 0) return nullptr;
    const pid_t child = fork();
    if (child == 0) {
        int ok = forkOrderCount == 4 && forkOrder[2] == 21 && forkOrder[3] == 22;
        for (int round = 0; round < 4; ++round) {
            void* blocks[32];
            for (int i = 0; i < 32; ++i) blocks[i] = malloc(64 + i * 61);
            for (int i = 0; i < 32; ++i)
                if (!blocks[i]) ok = 0;
                else memset(blocks[i], 0x5a, 64 + i * 61);
            for (int i = 0; i < 32; ++i) free(blocks[i]);
        }
        pthread_t thread;
        if (pthread_create(&thread, nullptr, ChildAllocatorThread, &ok) != 0) ok = 0;
        else pthread_join(thread, nullptr);
        char byte = ok ? 'k' : 'x';
        if (write(pipeFds[1], &byte, 1) != 1) _exit(3);
        _exit(0);
    }
    close(pipeFds[1]);
    char byte = 0;
    if (read(pipeFds[0], &byte, 1) == 1 && byte == 'k') work->allocations.fetch_add(1);
    close(pipeFds[0]);
    int status = 0;
    if (child > 0) waitpid(child, &status, 0);
    if (child > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0) work->allocations.fetch_add(1);
    return nullptr;
}

inline void CheckForkFromThread() {
    forkOrderCount = 0;
    Expect(pthread_atfork(ForkFirstPrepare, ForkFirstParent, ForkFirstChild) == 0, "an atfork handler is registered");
    Expect(pthread_atfork(ForkSecondPrepare, ForkSecondParent, ForkSecondChild) == 0,
           "a second atfork handler is registered");
    ForkWork work;
    pthread_t thread;
    if (pthread_create(&thread, nullptr, ForkingThread, &work) != 0) {
        Expect(false, "a thread forked");
        return;
    }
    pthread_join(thread, nullptr);
    Expect(work.allocations.load() == 2, "a child forked from a thread allocated and left cleanly");
    // prepare: newest first; parent and child: oldest first. The child's own two entries were written in
    // its copy of this array, which the parent never sees, and the child checked them itself.
    Expect(forkOrderCount == 4 && forkOrder[0] == 2 && forkOrder[1] == 1 && forkOrder[2] == 11 &&
               forkOrder[3] == 12,
           "the atfork lists ran in POSIX's order");
}

// C++ exceptions unwinding on a cordinal thread: through several frames, caught there, and rethrown.
inline int UnwindThrough(int depth) {
    if (depth == 0) throw std::runtime_error("unwound");
    return UnwindThrough(depth - 1) + 1;
}

struct ThrowWork {
    std::atomic<int> caught{0};
    std::atomic<int> rethrown{0};
};

inline void* ThrowingThread(void* pointer) {
    ThrowWork* work = (ThrowWork*)pointer;
    try {
        UnwindThrough(8);
    } catch (const std::runtime_error& error) {
        if (!strcmp(error.what(), "unwound")) work->caught.fetch_add(1);
    }
    try {
        try {
            UnwindThrough(4);
        } catch (...) {
            work->rethrown.fetch_add(1);
            throw;
        }
    } catch (const std::exception&) {
        work->rethrown.fetch_add(1);
    }
    return nullptr;
}

inline void CheckExceptions() {
    ThrowWork work;
    pthread_t thread;
    pthread_create(&thread, nullptr, ThrowingThread, &work);
    pthread_join(thread, nullptr);
    Expect(work.caught.load() == 1, "an exception thrown on a cordinal thread unwinds to the catch there");
    Expect(work.rethrown.load() == 2, "a rethrow from a catch block unwinds again on the same thread");
    ThrowWork throughStandardThread;
    std::thread standard([&] {
        try {
            UnwindThrough(5);
        } catch (const std::runtime_error&) {
            throughStandardThread.caught.fetch_add(1);
        }
    });
    standard.join();
    Expect(throughStandardThread.caught.load() == 1, "std::thread's thread unwinds exceptions the same way");
}

// The attribute calls: a stack size and guard size asked for are the ones the thread reports, both in
// glibc's accessors and in the field layout this file writes, and the stack address it reports contains
// the thread's own frame.
struct AttrWork {
    int result = -1;
    size_t stackSize = 0;   // what glibc's own accessor reads back
    size_t guardSize = 0;
    int detached = -1;
    size_t rawSize = 0;     // the field itself, at the offset a probe of this glibc found
    size_t rawGuardSize = 0;
    int containsFrame = 0;
};

inline void* AttrThread(void* pointer) {
    AttrWork* work = (AttrWork*)pointer;
    pthread_attr_t own;
    work->result = pthread_getattr_np(pthread_self(), &own);
    if (work->result != 0) return nullptr;
    struct AttrFields {
        int priority;
        int policy;
        int flags;
        int padding;
        size_t guardSize;
        void* stackEnd;
        size_t stackSize;
    };
    const AttrFields* fields = (const AttrFields*)&own;
    work->rawSize = fields->stackSize;
    work->rawGuardSize = fields->guardSize;
    void* address = nullptr;
    size_t size = 0;
    pthread_attr_getstack(&own, &address, &size);
    pthread_attr_getstacksize(&own, &work->stackSize);
    pthread_attr_getguardsize(&own, &work->guardSize);
    pthread_attr_getdetachstate(&own, &work->detached);
    int frame = 0;
    work->containsFrame = (char*)&frame >= (char*)address && (char*)&frame < (char*)address + size;
    return nullptr;
}

inline void CheckAttributesAndSignals() {
#if defined(CORDINAL_STATIC)
    // The attribute objects and the signal-set calls are glibc's: a static program creates its
    // threads with the defaults and installs no signal handler (abort unblocks SIGABRT itself).
    printf("skip: thread attributes and the signal-set calls are glibc's in this program\n");
#else
    pthread_attr_t attr;
    Expect(pthread_attr_init(&attr) == 0, "an attribute object is initialized");
    pthread_attr_setstacksize(&attr, 256 * 1024);
    pthread_attr_setguardsize(&attr, 8192);
    AttrWork work;
    pthread_t thread;
    Expect(pthread_create(&thread, &attr, AttrThread, &work) == 0, "a thread with a chosen stack is created");
    pthread_join(thread, nullptr);
    Expect(work.result == 0, "the thread reports its own attributes");
    Expect(work.rawSize == 256 * 1024, "the stack size asked for is the one the attr holds");
    Expect(work.rawGuardSize == 8192, "the guard size asked for is the one the attr holds");
    Expect(work.stackSize == work.rawSize && work.guardSize == work.rawGuardSize,
           "glibc's own accessors read the same fields this file writes");
    Expect(work.detached == PTHREAD_CREATE_JOINABLE, "a joined thread reports itself joinable");
    Expect(work.containsFrame == 1, "the reported stack address contains the thread's own frame");
    pthread_attr_destroy(&attr);

    pthread_attr_t detachedAttr;
    pthread_attr_init(&detachedAttr);
    pthread_attr_setdetachstate(&detachedAttr, PTHREAD_CREATE_DETACHED);
    std::atomic<bool> ran{false};
    auto detached = [](void* pointer) -> void* {
        ((std::atomic<bool>*)pointer)->store(true);
        return nullptr;
    };
    Expect(pthread_create(&thread, &detachedAttr, detached, &ran) == 0, "a detached thread is created");
    pthread_attr_destroy(&detachedAttr);
    for (int spin = 0; spin < 2000 && !ran.load(); ++spin) usleep(1000);
    Expect(ran.load(), "the detached thread ran");
    Expect(pthread_join(thread, nullptr) == EINVAL, "joining a detached thread is EINVAL");

    // Signal masks are per thread.
    sigset_t set;
    sigset_t current;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    Expect(pthread_sigmask(SIG_BLOCK, &set, nullptr) == 0, "a signal is blocked on the calling thread");
    sigemptyset(&current);
    pthread_sigmask(SIG_SETMASK, nullptr, &current);
    Expect(sigismember(&current, SIGUSR1) == 1, "the blocked signal is in the calling thread's mask");
    struct SignalWork {
        int member = -1;
    } signalWork;
    auto blocked = [](void* pointer) -> void* {
        sigset_t mask;
        sigemptyset(&mask);
        pthread_sigmask(SIG_SETMASK, nullptr, &mask);
        ((SignalWork*)pointer)->member = sigismember(&mask, SIGUSR1);
        return nullptr;
    };
    pthread_create(&thread, nullptr, blocked, &signalWork);
    pthread_join(thread, nullptr);
    Expect(pthread_sigmask(SIG_UNBLOCK, &set, nullptr) == 0, "the signal is unblocked again");
    Expect(pthread_sigmask(SIG_SETMASK, nullptr, &current) == 0 && sigismember(&current, SIGUSR1) == 0,
           "the blocked signal left the calling thread's mask");
    Expect(pthread_kill(pthread_self(), 0) == 0, "pthread_kill with signal 0 reaches the calling thread");
    struct sched_param parameter;
    parameter.sched_priority = 0;
    Expect(pthread_setschedparam(pthread_self(), SCHED_OTHER, &parameter) == 0,
           "the calling thread's scheduling policy is set");
#endif
}

// The whole family, and the report threads.cpp measured on this process.
inline void CheckThreads() {
    Expect(InProgram((const void*)&pthread_create) && InProgram((const void*)&pthread_mutex_lock) &&
               InProgram((const void*)&pthread_cond_signal) && InProgram((const void*)&pthread_rwlock_rdlock) &&
               InProgram((const void*)&dl_iterate_phdr) && InProgram((const void*)&__cxa_thread_atexit_impl),
           "the thread and thread-local functions are cordinal's own");
    long facts[11] = {};
    cordlibc::ThreadsFacts(facts);
    Expect(facts[0] > 0 && facts[1] < 0 && facts[1] >= -facts[0], "the static TLS area holds the program's block");
#if defined(CORDINAL_STATIC)
    // A static program has no unit that holds what it and the C library have; its own layout is the one
    // _start built, and the program's block ends at the thread pointer.
    Expect(facts[3] == 1, "the module list was read: the program itself is the only module");
    Expect(facts[1] == -facts[0], "the program's own block fills the static area of a static program");
#else
    Expect(facts[3] >= 2, "the module list was read: this program and the C library at least");
    Expect(facts[4] == 1, "the thread-local layout was observed in the running process");
    Expect(facts[5] == 1, "the observed layout agrees with the reverse-order walk the loader does");
#endif
    Expect(facts[6] == 1, "the control block keeps its self pointers where they were measured");
    Expect(facts[9] == 1024, "the key limit is PTHREAD_KEYS_MAX");
    Expect(facts[10] == 1, "a thread-local the linker placed lands inside the program's block here");
    CheckThreadLifecycle(facts);
    CheckMutexes();
    CheckConditionVariables();
    CheckRwlocks();
    CheckOnce();
    CheckKeys();
    CheckThreadLocalStorage();
    CheckThreadLocalErrors(facts);
    CheckSingleThreadedFlag();
    CheckForkFromThread();
    CheckExceptions();
    CheckAttributesAndSignals();
}

}  // namespace libc_checks
