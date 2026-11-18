// The last imports (misc.cpp) against glibc's: strerror and strerror_r for every errno x86-64 has and
// every buffer size, sysconf's whole name table (0 to 262, the cache sizes and the "no limit" names
// included) and get_nprocs, getpwuid field by field, nice's round trip, shmctl's four calls, fnmatch's
// pattern and flag battery, qsort_r's order (equal keys included), wcscoll_l, signal's handler and
// return, arc4random's bounds, the gettext calls, and strtof128 and strfromf128 over spellings,
// formats and truncation. glibc's copies are reached through dlsym, since cordinal's are hidden.
#pragma once
#include <dlfcn.h>
#include <errno.h>
#include <fnmatch.h>
#include <locale.h>
#include <pwd.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/resource.h>
#include <sys/shm.h>
#include <sys/sysinfo.h>
#include <unistd.h>
#include <wchar.h>

namespace libc_checks {

// The structures misc.cpp declares for itself, checked against the system's own headers.
inline void CheckMiscStructures() {
    static_assert(sizeof(passwd) == sizeof(char*) * 5 + sizeof(unsigned) * 2, "glibc's struct passwd");
    Expect(offsetof(passwd, pw_name) == 0 && offsetof(passwd, pw_passwd) == sizeof(char*) &&
               offsetof(passwd, pw_uid) == sizeof(char*) * 2 && offsetof(passwd, pw_gid) == sizeof(char*) * 2 + sizeof(unsigned) &&
               offsetof(passwd, pw_gecos) == sizeof(char*) * 3 && offsetof(passwd, pw_dir) == sizeof(char*) * 4 &&
               offsetof(passwd, pw_shell) == sizeof(char*) * 5,
           "struct passwd's layout");
    Expect(sizeof(shmid_ds) == 112 && offsetof(shmid_ds, shm_perm) == 0 && offsetof(shmid_ds, shm_segsz) == 48 &&
               offsetof(shmid_ds, shm_atime) == 56 && offsetof(shmid_ds, shm_dtime) == 64 &&
               offsetof(shmid_ds, shm_ctime) == 72 && offsetof(shmid_ds, shm_cpid) == 80 &&
               offsetof(shmid_ds, shm_lpid) == 84 && offsetof(shmid_ds, shm_nattch) == 88,
           "struct shmid_ds's layout");
    Expect(offsetof(shmid_ds, shm_perm.uid) == 4 && offsetof(shmid_ds, shm_perm.mode) == 20 &&
               offsetof(shmid_ds, shm_perm.__seq) == 24,
           "struct ipc_perm's layout");
    // The flags misc.cpp uses are the header's (its own copy of the values).
    Expect(FNM_PATHNAME == 1 && FNM_NOESCAPE == 2 && FNM_PERIOD == 4 && FNM_FILE_NAME == FNM_PATHNAME &&
               FNM_LEADING_DIR == 8 && FNM_CASEFOLD == 16,
           "FNM_*'s values");
}

inline void CheckStrerror() {
    auto theirs = (char* (*)(int))dlsym(RTLD_DEFAULT, "strerror");
    auto theirsR = (char* (*)(int, char*, size_t))dlsym(RTLD_DEFAULT, "strerror_r");
    if (!theirs || !theirsR) {
        Expect(false, "glibc's strerror and strerror_r are found");
        return;
    }
    Expect(InProgram((const void*)&strerror) && InProgram((const void*)&strerror_r) && !InProgram((const void*)theirs),
           "strerror and strerror_r are cordinal's");
    int compared = 0;
    for (int number = -6; number <= 200; ++number) {
        errno = 4242;
        const char* ours = strerror(number);
        const int ourErrno = errno;
        errno = 4242;
        const char* glibc = theirs(number);
        if (strcmp(ours, glibc) != 0) {
            fprintf(stderr, "FAIL strerror(%d): cordinal \"%s\" glibc \"%s\"\n", number, ours, glibc);
            ++failures;
        }
        Expect(ourErrno == errno, "strerror leaves errno alone");
        ++compared;
        for (size_t size : (size_t[]){0, 1, 2, 5, 12, 20, 24, 40, 100}) {
            char oursBuffer[128], glibcBuffer[128];
            memset(oursBuffer, 'x', sizeof oursBuffer);
            memset(glibcBuffer, 'x', sizeof glibcBuffer);
            oursBuffer[sizeof oursBuffer - 1] = 0;
            glibcBuffer[sizeof glibcBuffer - 1] = 0;
            errno = 4242;
            char* ourAnswer = strerror_r(number, oursBuffer, size);
            const int ourCallErrno = errno;
            errno = 4242;
            char* glibcAnswer = theirsR(number, glibcBuffer, size);
            const bool samePointer = (ourAnswer == oursBuffer) == (glibcAnswer == glibcBuffer);
            if (strcmp(ourAnswer, glibcAnswer) != 0 || !samePointer || memcmp(oursBuffer, glibcBuffer, sizeof glibcBuffer) != 0 ||
                ourCallErrno != errno) {
                fprintf(stderr, "FAIL strerror_r(%d, size %zu): cordinal \"%s\" (buffer %.16s) glibc \"%s\" (buffer %.16s)\n",
                        number, size, ourAnswer, oursBuffer, glibcAnswer, glibcBuffer);
                ++failures;
            }
            ++compared;
        }
    }
    Expect(compared > 1000, "strerror compared over every errno and buffer size");
    Expect(!strcmp(strerror(2), "No such file or directory") && !strcmp(strerror(0), "Success"),
           "strerror's own English for a known number");
    Expect(!strcmp(strerror(41), "Unknown error 41") && !strcmp(strerror(-1), "Unknown error -1"),
           "strerror's unknown-number form");
}

inline void CheckSysconf() {
    auto theirs = (long (*)(int))dlsym(RTLD_DEFAULT, "sysconf");
    auto theirProcs = (int (*)())dlsym(RTLD_DEFAULT, "get_nprocs");
    if (!theirs || !theirProcs) {
        Expect(false, "glibc's sysconf and get_nprocs are found");
        return;
    }
    Expect(InProgram((const void*)&sysconf) && InProgram((const void*)&get_nprocs) && !InProgram((const void*)theirs),
           "sysconf and get_nprocs are cordinal's");
    Expect(sysconf(_SC_PAGESIZE) == 4096 && sysconf(_SC_CLK_TCK) == 100 && sysconf(_SC_GETPW_R_SIZE_MAX) == 1024 &&
               sysconf(_SC_NPROCESSORS_ONLN) >= 1,
           "the names the program and libstdc++ ask for");
    Expect(get_nprocs() == theirProcs(), "get_nprocs is glibc's answer");
    Expect(sysconf(_SC_NPROCESSORS_ONLN) == theirProcs(), "sysconf's online count is get_nprocs's");
    int differences = 0, notes = 0;
    for (int name = -3; name <= 300; ++name) {
        errno = 0;
        const long ours = sysconf(name);
        const int ourErrno = errno;
        errno = 0;
        const long glibc = theirs(name);
        const int glibcErrno = errno;
        if (ours == glibc && ourErrno == glibcErrno) continue;
        // /sys has the level-1 instruction cache's associativity where glibc's CPUID path leaves -1.
        if (name == _SC_LEVEL1_ICACHE_ASSOC && ours > 0 && glibc == -1) {
            ++notes;
            continue;
        }
        // Availability moves as the machine works (this container builds while it runs), so it is
        // checked for being a sane amount of the total, and the difference is noted.
        if (name == _SC_AVPHYS_PAGES) {
            const long total = theirs(_SC_PHYS_PAGES);
            if (ours <= 0 || ours > total) {
                ++differences;
                fprintf(stderr, "FAIL sysconf(_SC_AVPHYS_PAGES): cordinal %ld of %ld pages\n", ours, total);
            } else if (ours != glibc) {
                ++notes;
            }
            continue;
        }
        ++differences;
        if (differences < 6)
            fprintf(stderr, "FAIL sysconf(%d): cordinal %ld (errno %d) glibc %ld (errno %d)\n", name, ours, ourErrno,
                    glibc, glibcErrno);
    }
    if (notes) printf("note: sysconf(_SC_LEVEL1_ICACHE_ASSOC) is %ld from /sys where glibc's CPUID path answers -1\n",
                      sysconf(_SC_LEVEL1_ICACHE_ASSOC));
    if (differences) fprintf(stderr, "%d sysconf differences from glibc\n", differences);
    Expect(differences == 0, "every sysconf name is glibc's answer");
    // The names glibc has no case for answer EINVAL, and its "no limit" ones leave errno alone.
    errno = 0;
    Expect(sysconf(-1) == -1 && errno == EINVAL && sysconf(4999) == -1 && errno == EINVAL, "an unknown name is EINVAL");
    errno = 4242;
    Expect(sysconf(_SC_TZNAME_MAX) == -1 && errno == 4242, "a no-limit name leaves errno alone");
}

inline void CheckPasswd() {
    auto theirs = (passwd * (*)(uid_t)) dlsym(RTLD_DEFAULT, "getpwuid");
    if (!theirs) {
        Expect(false, "glibc's getpwuid is found");
        return;
    }
    Expect(InProgram((const void*)&getpwuid), "getpwuid is cordinal's");
    auto Same = [](const passwd* a, const passwd* b) {
        if (!a || !b) return a == b;
        auto same = [](const char* x, const char* y) { return !strcmp(x ? x : "", y ? y : ""); };
        return same(a->pw_name, b->pw_name) && same(a->pw_passwd, b->pw_passwd) && a->pw_uid == b->pw_uid &&
               a->pw_gid == b->pw_gid && same(a->pw_gecos, b->pw_gecos) && same(a->pw_dir, b->pw_dir) &&
               same(a->pw_shell, b->pw_shell);
    };
    int checked = 0;
    for (uid_t uid : (uid_t[]){getuid(), 0, 1, 65534, 65533, 1234567, 4294967294u}) {
        errno = 4242;
        passwd* ours = getpwuid(uid);
        const int ourErrno = errno;
        errno = 4242;
        passwd* glibc = theirs(uid);
        if (!Same(ours, glibc) || ourErrno != errno) {
            fprintf(stderr, "FAIL getpwuid(%u): cordinal %s (errno %d) glibc %s (errno %d)\n", (unsigned)uid,
                    ours ? ours->pw_name : "null", ourErrno, glibc ? glibc->pw_name : "null", errno);
            ++failures;
        }
        ++checked;
    }
    Expect(checked == 7, "getpwuid compared for the caller, root and misses");
    passwd* root = getpwuid(0);
    Expect(root && !strcmp(root->pw_name, "root") && root->pw_uid == 0 && !strcmp(root->pw_dir, "/root"),
           "getpwuid(0) is root");
    Expect(getpwuid(1234567) == nullptr && errno == 0, "a miss is a null entry with errno cleared");
}

inline void CheckNice() {
    auto theirNice = (int (*)(int))dlsym(RTLD_DEFAULT, "nice");
    auto getPriority = (int (*)(int, id_t))dlsym(RTLD_DEFAULT, "getpriority");
    auto setPriority = (int (*)(int, id_t, int))dlsym(RTLD_DEFAULT, "setpriority");
    if (!theirNice || !getPriority || !setPriority) {
        Expect(false, "glibc's nice, getpriority and setpriority are found");
        return;
    }
    Expect(InProgram((const void*)&nice), "nice is cordinal's");
    errno = 0;
    const int base = getPriority(PRIO_PROCESS, 0);
    Expect(nice(0) == base && getPriority(PRIO_PROCESS, 0) == base, "nice(0) is the niceness and changes nothing");
    // Lowering the niceness needs the privilege this check usually does not have; then both libraries
    // refuse and change nothing. Where it does have it, the refusal cannot be compared.
    errno = 0;
    const int lowered = nice(-1);
    const int lowerErrno = errno;
    if (lowered == -1 && lowerErrno == EPERM) {
        errno = 0;
        const int glibc = theirNice(-1);
        if (glibc != -1 || errno != EPERM || getPriority(PRIO_PROCESS, 0) != base) {
            fprintf(stderr, "FAIL nice(-1): cordinal %d (errno %d) glibc %d (errno %d), niceness %d\n", lowered,
                    lowerErrno, glibc, errno, getPriority(PRIO_PROCESS, 0));
            ++failures;
        }
    } else {
        setPriority(PRIO_PROCESS, 0, base);
        printf("note: this system lets the check lower its niceness, so nice's refusal was not compared\n");
    }
    if (base < 19) {
        // Raising it is always allowed, and glibc reads the value back.
        const int raised = nice(1);
        Expect(raised == base + 1 && theirNice(0) == raised, "nice(1) raises it by one, as glibc reads it");
        Expect(nice(100) == 19, "a large niceness is clamped");
        Expect(theirNice(0) == 19, "glibc agrees with the clamp");
    } else {
        Expect(nice(1) == 19, "a niceness at the top stays there");
    }
}

inline void CheckShmctl() {
    auto theirGet = (int (*)(key_t, size_t, int))dlsym(RTLD_DEFAULT, "shmget");
    auto theirAt = (void* (*)(int, const void*, int))dlsym(RTLD_DEFAULT, "shmat");
    auto theirDt = (int (*)(const void*))dlsym(RTLD_DEFAULT, "shmdt");
    auto theirCtl = (int (*)(int, int, shmid_ds*))dlsym(RTLD_DEFAULT, "shmctl");
    if (!theirGet || !theirAt || !theirDt || !theirCtl) {
        Expect(false, "glibc's shared memory calls are found");
        return;
    }
    Expect(InProgram((const void*)&shmctl), "shmctl is cordinal's");
    const int id = shmget(IPC_PRIVATE, 8192, IPC_CREAT | 0600);
    if (id < 0) {
        printf("note: shared memory is not available, so shmctl's checks are skipped\n");
        return;
    }
    void* at = shmat(id, nullptr, 0);
    shmid_ds ours{}, glibc{};
    errno = 0;
    Expect(shmctl(id, IPC_STAT, &ours) == 0, "shmctl IPC_STAT");
    Expect(theirCtl(id, IPC_STAT, &glibc) == 0, "glibc's IPC_STAT");
    const bool same = ours.shm_perm.__key == glibc.shm_perm.__key && ours.shm_perm.uid == glibc.shm_perm.uid &&
                      ours.shm_perm.gid == glibc.shm_perm.gid && ours.shm_perm.cuid == glibc.shm_perm.cuid &&
                      ours.shm_perm.cgid == glibc.shm_perm.cgid && ours.shm_perm.mode == glibc.shm_perm.mode &&
                      ours.shm_perm.__seq == glibc.shm_perm.__seq && ours.shm_segsz == glibc.shm_segsz &&
                      ours.shm_cpid == glibc.shm_cpid && ours.shm_lpid == glibc.shm_lpid &&
                      ours.shm_nattch == glibc.shm_nattch && ours.shm_atime == glibc.shm_atime &&
                      ours.shm_dtime == glibc.shm_dtime && ours.shm_ctime == glibc.shm_ctime;
    Expect(same, "every field of IPC_STAT is glibc's");
    Expect(ours.shm_segsz == 8192 && ours.shm_nattch == 1, "the segment is the size and attachment made");
    shmid_ds changed = ours;
    changed.shm_perm.mode = 0640;
    Expect(shmctl(id, IPC_SET, &changed) == 0 && theirCtl(id, IPC_STAT, &glibc) == 0 && glibc.shm_perm.mode == 0640,
           "IPC_SET changes the mode glibc reads back");
    errno = 0;
    const int ourBadId = shmctl(-1, IPC_STAT, &ours);
    const int ourBadIdErrno = errno;
    errno = 0;
    const int glibcBadId = theirCtl(-1, IPC_STAT, &glibc);
    Expect(ourBadId == glibcBadId && ourBadIdErrno == errno, "an unknown id fails as glibc's does");
    errno = 0;
    const int ourBadCall = shmctl(id, 4711, &ours);
    const int ourBadCallErrno = errno;
    errno = 0;
    const int glibcBadCall = theirCtl(id, 4711, &glibc);
    Expect(ourBadCall == glibcBadCall && ourBadCallErrno == errno, "an unknown command fails as glibc's does");
    errno = 0;
    const int ourNull = shmctl(id, IPC_STAT, nullptr);
    const int ourNullErrno = errno;
    errno = 0;
    const int glibcNull = theirCtl(id, IPC_STAT, nullptr);
    Expect(ourNull == glibcNull && ourNullErrno == errno, "a null buffer fails as glibc's does");
    Expect(shmctl(id, IPC_RMID, nullptr) == 0, "IPC_RMID");
    if (at != (void*)-1) shmdt(at);
}

inline void CheckFnmatch() {
    auto theirs = (int (*)(const char*, const char*, int))dlsym(RTLD_DEFAULT, "fnmatch");
    if (!theirs) {
        Expect(false, "glibc's fnmatch is found");
        return;
    }
    Expect(InProgram((const void*)&fnmatch), "fnmatch is cordinal's");
    struct Case {
        const char* pattern;
        const char* name;
        int flags;
    };
    static const Case cases[] = {
        {"*.c", "foo.c", 0},          {"*.c", ".foo.c", 0},        {"*", ".x", 0},
        {"*", "", 0},                 {"", "", 0},                 {"?", "", 0},
        {"a?c", "abc", 0},            {"a?c", "ac", 0},            {"a[bc]d", "abd", 0},
        {"a[bc]d", "acd", 0},         {"a[!bc]d", "aad", 0},       {"a[!bc]d", "abd", 0},
        {"[a-c]", "b", 0},            {"[a-c]", "d", 0},           {"[!a-c]", "d", 0},
        {"[a-c-e]", "d", 0},          {"[a-c-e]", "e", 0},         {"[]]", "]", 0},
        {"[!]]", "a", 0},             {"[!]]", "]", 0},            {"[", "[", 0},
        {"[", "a", 0},                {"[[:", "[", 0},             {"[a-", "a", 0},
        {"a[b-]c", "a-c", 0},         {"a[b-]c", "abc", 0},        {"\\*", "*", 0},
        {"\\*", "x", 0},              {"\\[", "[", 0},             {"a\\", "a\\", 0},
        {"\\", "\\", 0},              {"a[*]b", "a*b", 0},         {"[[:alpha:]]", "a", 0},
        {"[[:alpha:]]", "1", 0},      {"[[:digit:]]", "5", 0},     {"[[:digit:]]", "a", 0},
        {"[[:upper:]]*", "Abc", 0},   {"[[:alpha:][:digit:]]", "7", 0}, {"[[:alpha:][:digit:]]", "-", 0},
        {"[[:foo:]]", "f", 0},        {"[[:alpha:]]", "/", FNM_PATHNAME}, {"[!a]", "/", FNM_PATHNAME},
        {"*", "/", FNM_PATHNAME},     {"*/*", "a/b", FNM_PATHNAME}, {"*/*", "a/b/c", FNM_PATHNAME},
        {"a/*", "a/b", FNM_PATHNAME}, {"a*b", "a/b", FNM_PATHNAME}, {"a/**", "a/b/c", FNM_PATHNAME},
        {"a/b", "a/b", FNM_PATHNAME}, {"a?b", "a/b", FNM_PATHNAME}, {"?", "/", FNM_PATHNAME},
        {"a", "a/b", FNM_LEADING_DIR}, {"a/", "a/b", FNM_LEADING_DIR}, {"a*", "abc", FNM_LEADING_DIR},
        {"a", "abc", FNM_LEADING_DIR}, {"*", ".x", FNM_PERIOD},    {"*.c", ".foo.c", FNM_PERIOD},
        {"*", "a/.x", FNM_PERIOD},    {"a/*", "a/.x", FNM_PERIOD}, {"a/.*", "a/.x", FNM_PERIOD | FNM_PATHNAME},
        {"?", ".x", FNM_PERIOD},      {".?", ".x", FNM_PERIOD},    {"a/?", "a/.", FNM_PERIOD},
        {"*.C", "foo.c", FNM_CASEFOLD}, {"[a-c]", "B", FNM_CASEFOLD}, {"[A-C]", "b", FNM_CASEFOLD},
        {"[!A-C]", "b", FNM_CASEFOLD}, {"aBc", "ABC", FNM_CASEFOLD}, {"*", "A/B", FNM_CASEFOLD | FNM_PATHNAME},
        {"a\\/b", "a/b", FNM_PATHNAME | FNM_NOESCAPE}, {"\\a", "a", FNM_NOESCAPE}, {"\\a", "a", 0},
        {"[\\]]", "]", 0},            {"[\\]]", "]", FNM_NOESCAPE}, {"*.", "x.", 0},
        {"?", "\xc3\xa4", 0},         {"*", "\xc3\xa4", 0},        {"a", "a", 0x100000},
        {"*.*", "a.b.c", 0},          {"*.[ch]", "x.c", 0},        {"*.[ch]", "x.o", 0},
        {"[0-9][0-9]*", "42abc", 0},  {"[0-9][0-9]*", "4abc", 0},  {"[!0-9]*", "abc", 0},
        {"[[.a.]]", "a", 0},          {"[[=a=]]", "a", 0},         {"a**b", "ab", 0},
        {"**", "abc", 0},             {"**/b", "a/b", FNM_PATHNAME}, {"*", "a", 0},
        {"a*", "a", 0},               {"a*", "ab", 0},             {"*a", "ba", 0},
        {"[^a]", "b", 0},             {"[^a]", "a", 0},
    };
    int compared = 0;
    for (const Case& c : cases) {
        errno = 4242;
        const int ours = fnmatch(c.pattern, c.name, c.flags);
        const int ourErrno = errno;
        errno = 4242;
        const int glibc = theirs(c.pattern, c.name, c.flags);
        if (ours != glibc || ourErrno != errno) {
            fprintf(stderr, "FAIL fnmatch(\"%s\", \"%s\", 0x%x): cordinal %d glibc %d (errno %d/%d)\n", c.pattern, c.name,
                    c.flags, ours, glibc, ourErrno, errno);
            ++failures;
        }
        ++compared;
    }
    Expect(compared > 80, "fnmatch compared over its patterns, classes and flags");
    Expect(fnmatch("*.mp3", "track.mp3", 0) == 0 && fnmatch("*.mp3", "track.flac", 0) == FNM_NOMATCH,
           "the check the program makes");
    Expect(fnmatch("*.[mM]p3", "SONG.MP3", FNM_CASEFOLD) == 0, "the case-folded check fontmatch makes");
}

inline void CheckQsort() {
    auto theirs = (void (*)(void*, size_t, size_t, int (*)(const void*, const void*, void*), void*))dlsym(
        RTLD_DEFAULT, "qsort_r");
    if (!theirs) {
        Expect(false, "glibc's qsort_r is found");
        return;
    }
    Expect(InProgram((const void*)&qsort_r), "qsort_r is cordinal's");
    struct Item {
        int key;
        int tag;
    };
    auto compare = [](const void* a, const void* b, void* context) {
        const Item& x = *(const Item*)a;
        const Item& y = *(const Item*)b;
        const int bias = *(const int*)context;
        if (x.key + bias < y.key + bias) return -1;
        return x.key + bias > y.key + bias ? 1 : 0;
    };
    int bias = 0;
    Item items[64];
    for (int i = 0; i < 64; ++i) items[i] = Item{(i * 37) % 11, i};
    Item sorted[64], glibc[64];
    memcpy(sorted, items, sizeof items);
    memcpy(glibc, items, sizeof items);
    qsort_r(sorted, 64, sizeof(Item), compare, &bias);
    theirs(glibc, 64, sizeof(Item), compare, &bias);
    Expect(memcmp(sorted, glibc, sizeof sorted) == 0, "qsort_r's order is glibc's, equal keys included");
    bool ordered = true;
    for (int i = 1; i < 64; ++i)
        if (sorted[i].key < sorted[i - 1].key) ordered = false;
    Expect(ordered, "qsort_r sorts");
    // The context reaches the comparison: one where a larger key sorts first.
    auto reverse = [](const void* a, const void* b, void* context) {
        const int direction = *(const int*)context;
        const Item& x = *(const Item*)a;
        const Item& y = *(const Item*)b;
        if (x.key * direction < y.key * direction) return -1;
        return x.key * direction > y.key * direction ? 1 : 0;
    };
    int direction = -1;
    memcpy(sorted, items, sizeof items);
    memcpy(glibc, items, sizeof items);
    qsort_r(sorted, 64, sizeof(Item), reverse, &direction);
    theirs(glibc, 64, sizeof(Item), reverse, &direction);
    Expect(memcmp(sorted, glibc, sizeof sorted) == 0 && sorted[0].key == 10, "the context argument is the comparison's");
    // Random sizes and keys, and the small counts that take the insertion sort.
    Random random{0x5170};
    for (int round = 0; round < 200; ++round) {
        const size_t count = round < 20 ? (size_t)round : (size_t)(random.Next() % 300);
        const size_t size = sizeof(Item);
        Item left[300], right[300];
        for (size_t i = 0; i < count; ++i) left[i] = Item{(int)(random.Next() % 17), (int)i};
        memcpy(right, left, count * size);
        qsort_r(left, count, size, compare, &bias);
        theirs(right, count, size, compare, &bias);
        if (memcmp(left, right, count * size) != 0) {
            fprintf(stderr, "FAIL qsort_r of %zu items differs from glibc\n", count);
            ++failures;
        }
    }
    // Nothing to sort, and one item, need no comparison at all.
    qsort_r(nullptr, 0, sizeof(Item), compare, &bias);
    qsort_r(items, 1, sizeof(Item), compare, &bias);
    Expect(true, "qsort_r takes an empty array and a single item");
}

inline void CheckCollation() {
    auto theirs = (int (*)(const wchar_t*, const wchar_t*, locale_t))dlsym(RTLD_DEFAULT, "wcscoll_l");
    auto makeLocale = (locale_t(*)(int, const char*, locale_t))dlsym(RTLD_DEFAULT, "newlocale");
    if (!theirs || !makeLocale) {
        Expect(false, "glibc's wcscoll_l and newlocale are found");
        return;
    }
    Expect(InProgram((const void*)&wcscoll_l), "wcscoll_l is cordinal's");
    locale_t ctype = makeLocale(LC_CTYPE_MASK, "C", (locale_t)0);
    locale_t utf8 = makeLocale(LC_CTYPE_MASK, "C.UTF-8", (locale_t)0);
    (void)utf8;  // the object says nothing: the program's own carry no collation either
    const wchar_t* words[] = {L"", L"a", L"b", L"ab", L"Ab", L"abc", L"abd", L"x", L"x ", L" ",
                              L"\u00e9", L"z", L"\u00ea", L"\u00ff", L"\uffff", L"\x10000", L"42", L"a\tb"};
    int compared = 0;
    for (const wchar_t* first : words) {
        for (const wchar_t* second : words) {
            const int ours = wcscoll_l(first, second, (locale_t)0);
            const int glibc = theirs(first, second, ctype);
            if (Sign(ours) != Sign(glibc)) {
                fprintf(stderr, "FAIL wcscoll_l(\"%ls\", \"%ls\"): cordinal %d glibc %d\n", first, second, ours, glibc);
                ++failures;
            }
            ++compared;
        }
    }
    Expect(compared == (int)(sizeof words / sizeof *words) * (int)(sizeof words / sizeof *words),
           "wcscoll_l compared over every pair");
    Expect(wcscoll_l(L"a", L"b", (locale_t)0) < 0 && wcscoll_l(L"b", L"a", (locale_t)0) > 0 &&
               wcscoll_l(L"a", L"a", (locale_t)0) == 0,
           "the C locale's wide order");
}

namespace {

volatile sig_atomic_t gSignalCount = 0;
volatile sig_atomic_t gSignalLast = 0;

void CountSignal(int number) {
    gSignalCount = gSignalCount + 1;
    gSignalLast = number;
}

}  // namespace

inline void CheckSignal() {
    auto theirs = (void (* (*)(int, void (*)(int)))(int)) dlsym(RTLD_DEFAULT, "signal");
    if (!theirs) {
        Expect(false, "glibc's signal is found");
        return;
    }
    Expect(InProgram((const void*)&signal), "signal is cordinal's");
    // A handler really runs, and returns: the kernel needs the restorer for that.
    gSignalCount = 0;
    void (*previous)(int) = signal(SIGUSR1, CountSignal);
    Expect(previous == SIG_DFL, "signal answers the handler it replaces");
    kill(getpid(), SIGUSR1);
    Expect(gSignalCount == 1 && gSignalLast == SIGUSR1, "the handler runs and the program carries on");
    kill(getpid(), SIGUSR1);
    Expect(gSignalCount == 2, "it runs again");
    previous = signal(SIGUSR1, SIG_IGN);
    Expect(previous == CountSignal, "the second call answers the first handler");
    kill(getpid(), SIGUSR1);
    Expect(gSignalCount == 2, "SIG_IGN ignores it");
    Expect(signal(SIGUSR1, SIG_DFL) == SIG_IGN, "SIG_DFL is restored");
    // An unknown signal number is SIG_ERR, as glibc's is.
    errno = 0;
    void (*ours)(int) = signal(1000, SIG_DFL);
    const int ourErrno = errno;
    errno = 0;
    void (*glibc)(int) = theirs(1000, SIG_DFL);
    Expect(ours == glibc && ours == SIG_ERR && ourErrno == errno, "an unknown signal number is SIG_ERR");
    // The check itself uses SIGPIPE's own pattern: setting a disposition and reading it back.
    void (*before)(int) = signal(SIGPIPE, SIG_DFL);
    Expect(before == SIG_DFL || before == SIG_IGN, "SIGPIPE's disposition is read back");
}

inline void CheckRandom() {
    Expect(InProgram((const void*)&arc4random), "arc4random is cordinal's");
    uint32_t values[256];
    for (uint32_t& value : values) value = arc4random();
    bool repeated = false;
    for (size_t i = 1; i < sizeof values / sizeof *values; ++i)
        if (values[i] == values[i - 1]) repeated = true;
    Expect(!repeated, "arc4random does not repeat itself in 256 calls");
    bool spread = false;
    for (uint32_t value : values)
        if ((value >> 24) != (values[0] >> 24)) spread = true;
    Expect(spread, "arc4random's bytes vary");
}

inline void CheckGettext() {
    Expect(InProgram((const void*)&gettext) && InProgram((const void*)&dgettext) &&
               InProgram((const void*)&bindtextdomain) && InProgram((const void*)&bind_textdomain_codeset),
           "the gettext calls are cordinal's");
    Expect(gettext("hello") == (const char*)"hello", "gettext answers its message unchanged");
    Expect(gettext(nullptr) == nullptr, "gettext of nothing is nothing");
    Expect(!strcmp(dgettext("domain", "message"), "message"), "dgettext answers its message unchanged");
    Expect(bindtextdomain("cordinal-check", "/tmp/cordinal-check") == (const char*)"/tmp/cordinal-check",
           "bindtextdomain answers the directory");
    Expect(bindtextdomain("cordinal-check", nullptr) == (const char*)"/tmp/cordinal-check",
           "and the directory again when asked for it");
    Expect(!strcmp(bindtextdomain("cordinal-never", nullptr), "/usr/share/locale"),
           "a domain with no directory is the default one");
    Expect(bindtextdomain(nullptr, "/tmp/x") == nullptr && bindtextdomain("", "/tmp/x") == nullptr,
           "a null or empty domain is refused");
    Expect(bind_textdomain_codeset("cordinal-check", "UTF-8") == (const char*)"UTF-8", "bind_textdomain_codeset");
    Expect(bind_textdomain_codeset("cordinal-check", nullptr) == (const char*)"UTF-8", "the codeset again");
    Expect(bind_textdomain_codeset(nullptr, "UTF-8") == nullptr, "a null domain is refused");
    // glibc's own answers for the pure calls, which must agree.
    auto theirText = (char* (*)(const char*))dlsym(RTLD_DEFAULT, "gettext");
    auto theirDomain = (char* (*)(const char*, const char*))dlsym(RTLD_DEFAULT, "bindtextdomain");
    auto theirCodeset = (char* (*)(const char*, const char*))dlsym(RTLD_DEFAULT, "bind_textdomain_codeset");
    if (theirText && theirDomain && theirCodeset) {
        Expect(!strcmp(theirText("hello"), gettext("hello")), "glibc answers gettext the same way");
        Expect(!strcmp(theirDomain("cordinal-check", "/tmp/cordinal-check"), bindtextdomain("cordinal-check", "/tmp/cordinal-check")),
               "glibc answers bindtextdomain the same way");
        Expect(!strcmp(theirCodeset("cordinal-check", "UTF-8"), bind_textdomain_codeset("cordinal-check", "UTF-8")),
               "glibc answers bind_textdomain_codeset the same way");
        Expect(theirDomain(nullptr, "/tmp/x") == nullptr && theirCodeset(nullptr, "UTF-8") == nullptr,
               "glibc refuses a null domain too");
    }
}

// Only cordinal's own C library is checked here (CheckMisc), and only GCC spells _Float128 constants
// (the f128 suffix): a clang build, such as the sanitizer one, leaves this out.
#ifdef CORDINAL_OWN_LIBC
inline bool SameFloat128(_Float128 a, _Float128 b) {
    unsigned char x[16], y[16];
    memcpy(x, &a, sizeof x);
    memcpy(y, &b, sizeof y);
    return memcmp(x, y, sizeof x) == 0;
}

inline _Float128 Float128FromBits(uint64_t high, uint64_t low) {
    union {
        _Float128 value;
        struct {
            uint64_t low, high;
        } words;
    } bits{};
    bits.words.high = high;
    bits.words.low = low;
    return bits.value;
}

inline void CheckFloat128() {
    auto theirStrto = (_Float128(*)(const char*, char**))dlsym(RTLD_DEFAULT, "strtof128");
    auto theirStrfrom = (int (*)(char*, size_t, const char*, _Float128))dlsym(RTLD_DEFAULT, "strfromf128");
    if (!theirStrto || !theirStrfrom) {
        Expect(false, "glibc's strtof128 and strfromf128 are found");
        return;
    }
    Expect(InProgram((const void*)&strtof128) && InProgram((const void*)&strfromf128),
           "strtof128 and strfromf128 are cordinal's");
    // Parsing: value, end pointer and errno against glibc's, for spellings the program could meet.
    static const char* const spellings[] = {
        "0", "1", "-1", "1.5", "0.1", "-3.25e10", "1e0", "1e4000", "1e-4000", "0x1.8p3", "0x1p-16494",
        "0x1p16384", "0x0.0000000000000000000000000001p-16382", "inf", "-inf", "infinity", "INF", "nan", "NAN",
        "nan(123)", "nan()", " 42", "\t-7.5", ".5", "5.", "1e", "0x", "nonsense", "", "+", "-", "1e+5", "1E-5",
        "3.14159265358979323846264338327950288", "1.189731495357231765e4932", "3.362103143112093506e-4932",
        "6.475175119438025111e-4966", "1.189731495357231766e4932", "1e4933", "1e-4967", "1e-5000", "1e5000",
        "0.00000000000000000000000000000000000000000000000001", "1234567890123456789012345678901234567890",
        "1.234567890123456789012345678901234567890123456789e-10", "0x1.ffffffffffffffffffffffffffffp+16383",
        "0x1p+0x", "1.7e308", "1.7e-308", "2.2250738585072014e-308", "4.9406564584124654e-324",
        "1e-4950", "9.99e4931", "0.5e-4965", "000000000000001", "1.0e", "1.0e+", "0x1.8", "0x.8p1", "0x1.p1",
        "1_000", "   ", "\n1", "1.2.3", "1e0000005", "1e999999", "1e-999999", "0.1e1", "123456789e-6",
        "3.4028234663852885981e38", "1.7976931348623157081e308", "1.18973149535723176508575932662800702e4932",
        "1.189731495357231765085759326628007016e4932", "3.362103143112093506262677817321752603e-4932",
        "6.47517511943802511092443895822764655e-4966", "1e-4966", "9e-4967",
    };
    int compared = 0;
    for (const char* text : spellings) {
        char* ourEnd = nullptr;
        char* glibcEnd = nullptr;
        errno = 0;
        const _Float128 ours = strtof128(text, &ourEnd);
        const int ourErrno = errno;
        errno = 0;
        const _Float128 glibc = theirStrto(text, &glibcEnd);
        const int glibcErrno = errno;
        const long ourOffset = ourEnd ? ourEnd - text : -1;
        const long glibcOffset = glibcEnd ? glibcEnd - text : -1;
        if (!SameFloat128(ours, glibc) || ourOffset != glibcOffset || ourErrno != glibcErrno) {
            char ourText[80] = "?", glibcText[80] = "?";
            theirStrfrom(ourText, sizeof ourText, "%.21g", ours);
            theirStrfrom(glibcText, sizeof glibcText, "%.21g", glibc);
            fprintf(stderr, "FAIL strtof128(\"%s\"): cordinal %s (end %ld, errno %d) glibc %s (end %ld, errno %d)\n", text,
                    ourText, ourOffset, ourErrno, glibcText, glibcOffset, glibcErrno);
            ++failures;
        }
        ++compared;
    }
    Expect(compared > 70, "strtof128 compared over its spellings");
    // Random decimal strings with exponents, for the rounding paths.
    Random random{0xf128};
    int bad = 0;
    for (int round = 0; round < 3000; ++round) {
        char text[64];
        int at = 0;
        if (random.Next() & 1) text[at++] = '-';
        const int digits = (int)(random.Next() % 24) + 1;
        for (int i = 0; i < digits; ++i) text[at++] = (char)('0' + random.Next() % 10);
        if (random.Next() & 1) {
            text[at++] = '.';
            for (int i = 0; i < 20; ++i) text[at++] = (char)('0' + random.Next() % 10);
        }
        if (random.Next() & 1) {
            text[at++] = 'e';
            if (random.Next() & 1) text[at++] = '-';
            const int exponent = (int)(random.Next() % 5000);
            at += snprintf(text + at, sizeof text - (size_t)at, "%d", exponent);
        }
        text[at] = 0;
        errno = 0;
        const _Float128 ours = strtof128(text, nullptr);
        const int ourErrno = errno;
        errno = 0;
        const _Float128 glibc = theirStrto(text, nullptr);
        const int glibcErrno = errno;
        if (!SameFloat128(ours, glibc) || ourErrno != glibcErrno) {
            if (++bad < 4) fprintf(stderr, "FAIL strtof128(\"%s\") differs from glibc\n", text);
            ++failures;
        }
    }
    Expect(bad == 0, "strtof128's rounding is glibc's over random numerals");
    // Writing: the string, the return value and the truncation, for each conversion and precision.
    const _Float128 values[] = {
        0.0f128, -0.0f128, 1.0f128, -1.5f128, 0.1f128, 0.5f128, 2.5f128, 1.0f128 / 3.0f128,
        3.14159265358979323846264338327950288f128, 123456789.0f128, 1e30f128, 1e-30f128, 9.999999f128,
        0.000099999f128, 999999.5f128, Float128FromBits(0x7ffeffffffffffULL, ~0ULL), Float128FromBits(0x0000000000000001ULL, 0),
        Float128FromBits(0x4000c90fdaa22168ULL, 0xc23f67de7ff8f3f1ULL), Float128FromBits(0x0001ffffffffffffULL, ~0ULL),
        Float128FromBits(0x7fff800000000000ULL, 0), Float128FromBits(0x7fff000000000000ULL, 0),
        Float128FromBits(0xffff000000000000ULL, 0), Float128FromBits(0x3fff000000000000ULL, 0),
    };
    static const char* const formats[] = {"%f", "%.0f", "%.3f", "%.20f", "%.40f", "%e", "%.0e", "%.3e", "%.20e",
                                          "%g", "%.0g", "%.1g", "%.10g", "%.36g", "%.40g", "%a", "%.0a", "%.3a",
                                          "%.13a", "%.28a", "%F", "%E", "%G", "%A"};
    int written = 0;
    for (const _Float128 value : values) {
        for (const char* format : formats) {
            char ours[160], glibc[160];
            memset(ours, 'x', sizeof ours);
            memset(glibc, 'x', sizeof glibc);
            const int ourLength = strfromf128(ours, sizeof ours, format, value);
            const int glibcLength = theirStrfrom(glibc, sizeof glibc, format, value);
            if (ourLength != glibcLength || strcmp(ours, glibc) != 0) {
                fprintf(stderr, "FAIL strfromf128(\"%s\"): cordinal \"%s\" (%d) glibc \"%s\" (%d)\n", format, ours,
                        ourLength, glibc, glibcLength);
                ++failures;
            }
            ++written;
        }
    }
    Expect(written > 400, "strfromf128 compared over its formats and values");
    for (size_t size : (size_t[]){0, 1, 2, 4, 8, 16}) {
        char ours[32], glibc[32];
        memset(ours, 'x', sizeof ours);
        memset(glibc, 'x', sizeof glibc);
        const int ourLength = strfromf128(ours, size, "%.3f", (_Float128)-1.5);
        const int glibcLength = theirStrfrom(glibc, size, "%.3f", (_Float128)-1.5);
        Expect(ourLength == glibcLength && memcmp(ours, glibc, sizeof glibc) == 0, "strfromf128 truncates as glibc's does");
    }
    char text[32];
    Expect(strfromf128(text, sizeof text, "%.2f", (_Float128)1.5) == 4 && !strcmp(text, "1.50"),
           "strfromf128's usual call");
    Expect(strfromf128(text, sizeof text, "%d", (_Float128)1.5) == -1, "a format it cannot read is an error");
}
#endif

inline void CheckMisc() {
    CheckMiscStructures();
#ifdef CORDINAL_OWN_LIBC
    CheckStrerror();
    CheckSysconf();
    CheckPasswd();
    CheckNice();
    CheckShmctl();
    CheckFnmatch();
    CheckQsort();
    CheckCollation();
    CheckSignal();
    CheckRandom();
    CheckGettext();
    CheckFloat128();
#endif
}

}  // namespace libc_checks
