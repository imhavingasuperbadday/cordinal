// cordinal's own files, directories, environment and processes (stage 4 of replacing glibc):
// opendir/readdir and the rest of the directory stream over getdents64, getcwd, realpath, mkstemp,
// mkdtemp, fchmodat, statvfs, getenv/setenv/unsetenv/putenv, fork, waitpid, posix_spawn, popen and
// the exec it needs. Hidden like kernel.cpp, so cordinal and the static libstdc++ bind to these and
// glibc keeps its own. The plain kernel calls they are built on stay in kernel.cpp; this file makes
// its own raw calls through syscall.h, the way malloc.cpp does, so it compiles without glibc
// headers that would declare the names replaced here.
//
// The environment. setenv and friends work on the process's `__environ`. A portable dynamic build
// takes glibc's array, which the link's copy relocation makes one object with cordinal's `environ`:
// glibc's own getenv, inside strftime's tzset, getaddrinfo and the locale code, therefore sees what
// is changed here, and cordinal sees what glibc changes. A static build (stage 9, CORDINAL_STATIC)
// has no glibc to share it with, so the pointer and its `environ` alias are the program's own.
//
// fork. fork here runs the atfork handlers itself (threads.cpp's lists, which malloc.cpp registers its
// allocator hooks with), in the order POSIX gives: prepare newest first, parent and child oldest first.
// The locks of the rest of this file are not held across the call: cordinal's forked children exec or
// _exit at once, and a child that lives on inherits its parent's file descriptors as any fork's does.
//
// posix_spawn is a fork whose child reports a failing file action or exec through a pipe, which
// gives glibc's error-number return and reaps the child. popen wraps its pipe with cordinal's own
// fdopen (stdio.cpp, stage 5a), so the FILE a caller gets back and the fclose pclose runs are the
// program's, as every other stream is.
#include "syscall.h"
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// The process-wide allocator (malloc.cpp, default visibility) and cordinal's FILE streams
// (stdio.cpp, hidden visibility), which this file uses but does not replace. Declared here because
// no header for them is included.
extern "C" void* malloc(size_t);
extern "C" void free(void*);
extern "C" void* realloc(void*, size_t);
typedef struct _IO_FILE FILE;
extern "C" FILE* fdopen(int, const char*);
extern "C" int fclose(FILE*);
// The environment array: glibc's in a portable dynamic build (see the header comment), the
// program's own in the static one, where `environ` is its weak alias at the same address.
#if defined(CORDINAL_STATIC)
extern "C" {
__attribute__((visibility("hidden"))) char** __environ;
extern __attribute__((weak, alias("__environ"))) char** environ;
}
#else
extern char** __environ;
#endif
extern "C" __attribute__((noreturn)) void _exit(int) noexcept;

using cordlibc::Call;

namespace {
constexpr int kAtFdCwd = -100;
constexpr int kAtSymlinkNoFollow = 0x100;
constexpr int kFSetFd = 2;
constexpr int kFdCloexec = 1;
constexpr int kSigChild = 17;
constexpr int kSignals = 64;
constexpr long kPathMax = 4096;
constexpr unsigned long kAtSecure = 23;  // the auxiliary vector's "started set-user-ID" entry
constexpr int kStatValid = 0x20;  // the kernel's "f_flags is filled in" bit, which statvfs drops
// Opening: the flags these calls pass. Values from x86-64 Linux's fcntl.h.
constexpr int kOpenCreate = 0100;
constexpr int kOpenExclusive = 0200;
constexpr int kOpenNonBlock = 04000;
constexpr int kOpenDirectory = 0200000;
constexpr int kOpenNoFollow = 0400000;
constexpr int kOpenCloexec = 02000000;
constexpr int kOpenPath = 010000000;
// Modes: the file type bits of the kernel's stat.
constexpr unsigned kModeType = 0170000;
constexpr unsigned kModeDirectory = 0040000;
constexpr unsigned kModeLink = 0120000;
// posix_spawn's attribute flags (spawn.h), and the kernel's SIG_SETMASK for rt_sigprocmask.
constexpr short kSpawnResetIds = 0x01;
constexpr short kSpawnSetPGroup = 0x02;
constexpr short kSpawnSetSigDef = 0x04;
constexpr short kSpawnSetSigMask = 0x08;
constexpr short kSpawnSetSchedParam = 0x10;
constexpr short kSpawnSetScheduler = 0x20;
constexpr short kSpawnSetSid = 0x80;
constexpr short kSpawnKnown = 0x1ff;  // every flag glibc's spawn.h defines, including USEVFORK
constexpr int kSigSetMask = 2;
constexpr int kDirBuffer = 32 << 10;  // the records one getdents64 read brings, as glibc reads them
}  // namespace

// The layout of x86-64's struct stat, which the kernel fills whole (144 bytes); these calls read
// its mode. files_checks.h checks that size against the system header's struct.
struct Stat {
    uint64_t dev, ino, nlink;
    unsigned mode, uid, gid, pad;
    uint64_t rdev, size, blockSize, blocks;
    long atime[2], mtime[2], ctime[2];
    long reserved[3];
};
static_assert(sizeof(Stat) == 144, "x86-64's struct stat");

// glibc's struct statvfs (sys/statvfs.h), which is the buffer callers pass; f_type came with
// glibc 2.39. files_checks.h checks the size and the field offsets against the header.
struct StatVfs {
    unsigned long blockSize, fragmentSize, blocks, blocksFree, blocksAvailable;
    unsigned long files, filesFree, filesAvailable;
    unsigned long id;
    unsigned long flags;
    unsigned long nameMax;
    unsigned type;
    int spare[5];
};
static_assert(sizeof(StatVfs) == 112, "x86-64's struct statvfs");

// The kernel's statfs, which statvfs maps onto the structure above.
struct SysStatFs {
    int64_t type, blockSize, blocks, blocksFree, blocksAvailable, files, filesFree;
    int32_t id[2];
    int64_t nameLength, fragmentSize, flags;
    int64_t spare[4];
};
static_assert(sizeof(SysStatFs) == 120, "x86-64's struct statfs");

// One record from getdents64, whose layout on x86-64 is glibc's struct dirent; the name follows.
struct DirEntry {
    uint64_t ino;
    int64_t off;
    unsigned short length;
    unsigned char type;
};

// glibc's DIR (typedef struct __dirstream DIR) is opaque to callers, so its shape is cordinal's:
// the descriptor, the records getdents64 last returned, and the position telldir reports. A record
// handed out is valid until the next call on the same stream, as glibc's is.
struct __dirstream {
    int fd;
    long used, next;   // record bytes in the buffer, and where the next record starts
    int64_t position;  // the d_off of the last record handed out, which is telldir's answer
    char buffer[kDirBuffer];
};

#define CL extern "C" __attribute__((visibility("hidden")))

// Defined at the end of the file: popen's child runs its shell with it.
extern "C" int execl(const char* path, const char* arg, ...);
extern "C" int execlp(const char* file, const char* arg, ...);

// fork, execl and execlp are GCC builtins, already declared with default visibility, so like _exit
// in kernel.cpp they are hidden with an assembler directive.
__asm__(".hidden fork");
__asm__(".hidden execl");
__asm__(".hidden execlp");

// Directory streams.
CL struct __dirstream* opendir(const char* path) {
    const long fd = Call(__NR_openat, kAtFdCwd, path, kOpenDirectory | kOpenNonBlock | kOpenCloexec, 0);
    if (fd < 0) return nullptr;
    struct __dirstream* dir = (struct __dirstream*)malloc(sizeof(struct __dirstream));
    if (!dir) {
        const int saved = errno;
        Call(__NR_close, (int)fd);
        errno = saved;  // the malloc failure is what the caller needs to see
        return nullptr;
    }
    dir->fd = (int)fd;
    dir->used = dir->next = 0;
    dir->position = 0;
    return dir;
}
// fdopendir takes the descriptor over: the stream closes it.
CL struct __dirstream* fdopendir(int fd) {
    Stat st{};
    if (Call(__NR_fstat, fd, &st) < 0) return nullptr;
    if ((st.mode & kModeType) != kModeDirectory) {
        errno = ENOTDIR;
        return nullptr;
    }
    struct __dirstream* dir = (struct __dirstream*)malloc(sizeof(struct __dirstream));
    if (!dir) return nullptr;  // the caller keeps the descriptor, as glibc's does
    dir->fd = fd;
    dir->used = dir->next = 0;
    dir->position = 0;
    return dir;
}
CL struct dirent* readdir(struct __dirstream* dir) {
    if (!dir || dir->fd < 0) {
        errno = EBADF;
        return nullptr;
    }
    for (;;) {
        if (dir->next < dir->used) {
            const DirEntry* entry = (const DirEntry*)(dir->buffer + dir->next);
            if (entry->length < sizeof(DirEntry)) {
                errno = EIO;
                return nullptr;
            }
            dir->next += entry->length;
            dir->position = entry->off;
            if (!entry->ino) continue;  // a deleted entry, which glibc skips as well
            return (struct dirent*)entry;
        }
        const long got = Call(__NR_getdents64, dir->fd, dir->buffer, (long)kDirBuffer);
        if (got <= 0) return nullptr;  // the end leaves errno alone, as glibc's does
        dir->used = got;
        dir->next = 0;
    }
}
CL struct dirent* readdir64(struct __dirstream* dir) { return readdir(dir); }
CL int closedir(struct __dirstream* dir) {
    if (!dir) {
        errno = EBADF;
        return -1;
    }
    const int fd = dir->fd;
    dir->fd = -1;
    free(dir);
    return (int)Call(__NR_close, fd);
}
CL int dirfd(struct __dirstream* dir) {
    if (!dir || dir->fd < 0) {
        errno = EINVAL;
        return -1;
    }
    return dir->fd;
}
// seekdir and telldir use the kernel's own cookies (each record's d_off), so a saved position
// survives a rewinddir.
CL void seekdir(struct __dirstream* dir, long position) {
    if (!dir) return;
    dir->next = dir->used = 0;
    dir->position = position;
    Call(__NR_lseek, dir->fd, position, 0 /* SEEK_SET */);
}
CL long telldir(struct __dirstream* dir) { return dir ? (long)dir->position : 0; }
CL void rewinddir(struct __dirstream* dir) { seekdir(dir, 0); }

// getcwd: the kernel's own call, growing an allocated buffer until it fits.
CL char* getcwd(char* buffer, size_t size) {
    if (buffer) return Call(__NR_getcwd, buffer, (long)size) < 0 ? nullptr : buffer;
    size_t capacity = size ? size : (size_t)kPathMax;
    char* grown = (char*)malloc(capacity);
    if (!grown) return nullptr;
    for (;;) {
        if (Call(__NR_getcwd, grown, (long)capacity) >= 0) return grown;
        if (errno != ERANGE) {
            const int saved = errno;
            free(grown);
            errno = saved;
            return nullptr;
        }
        char* larger = (char*)realloc(grown, capacity * 2);
        if (!larger) {
            const int saved = errno;
            free(grown);
            errno = saved;
            return nullptr;
        }
        grown = larger;
        capacity *= 2;
    }
}

// realpath: a component at a time through the kernel, as glibc's does, so symlinks, "..", a
// trailing slash on something that is not a directory and the ELOOP limit all answer the same.
namespace {
// The parent of a canonical path of `length` bytes: "/a/b" gives "/a", "/a" gives "/".
size_t ParentLength(const char* path, size_t length) {
    while (length > 1 && path[length - 1] != '/') --length;
    return length > 1 ? length - 1 : 1;
}

bool Canonicalize(const char* path, char* out) {
    char work[kPathMax];
    if (path[0] == '/') {
        const size_t length = strlen(path);
        if (length + 5 >= (size_t)kPathMax) {  // room for "/name" and the null
            errno = ENAMETOOLONG;
            return false;
        }
        memcpy(work, path, length + 1);
    } else {
        char cwd[kPathMax];
        if (Call(__NR_getcwd, cwd, (long)sizeof cwd) < 0) return false;
        const size_t here = strlen(cwd), length = strlen(path);
        if (here + 1 + length + 4 >= (size_t)kPathMax) {
            errno = ENAMETOOLONG;
            return false;
        }
        memcpy(work, cwd, here);
        work[here] = '/';
        memcpy(work + here + 1, path, length + 1);
    }
    out[0] = '/';
    out[1] = 0;
    size_t outLength = 1;
    size_t at = 0;
    int links = 0;
    for (;;) {
        while (work[at] == '/') ++at;
        if (!work[at]) break;
        size_t end = at;
        while (work[end] && work[end] != '/') ++end;
        const size_t length = end - at;
        // The candidate is the parent so far plus this component, checked whole.
        char candidate[kPathMax];
        memcpy(candidate, out, outLength);
        size_t candidateLength = outLength;
        if (candidateLength > 1) candidate[candidateLength++] = '/';
        if (candidateLength + length + 2 >= (size_t)kPathMax) {
            errno = ENAMETOOLONG;
            return false;
        }
        memcpy(candidate + candidateLength, work + at, length);
        candidateLength += length;
        candidate[candidateLength] = 0;
        Stat st{};
        if (Call(__NR_newfstatat, kAtFdCwd, candidate, &st, kAtSymlinkNoFollow) < 0) return false;
        if ((st.mode & kModeType) == kModeLink) {
            if (++links > 40) {  // glibc's threshold for a symlink loop
                errno = ELOOP;
                return false;
            }
            char target[kPathMax];
            const long got = Call(__NR_readlink, candidate, target, (long)sizeof target - 1);
            if (got < 0) return false;
            target[got] = 0;
            // The target, then what was left of the path, becomes the work.
            const char* rest = work + end;
            const size_t restLength = strlen(rest);
            if ((size_t)got + restLength + 2 >= (size_t)kPathMax) {
                errno = ENAMETOOLONG;
                return false;
            }
            char spliced[kPathMax];
            memcpy(spliced, target, (size_t)got);
            memcpy(spliced + got, rest, restLength + 1);
            memcpy(work, spliced, (size_t)got + restLength + 1);
            at = 0;
            if (target[0] == '/') outLength = 1;  // an absolute target starts at the root
            out[outLength] = 0;
            continue;
        }
        if (length == 1 && work[at] == '.') {
            // The candidate was checked, and the path is unchanged.
        } else if (length == 2 && work[at] == '.' && work[at + 1] == '.') {
            outLength = ParentLength(out, outLength);
        } else {
            outLength = candidateLength;
            memcpy(out, candidate, candidateLength + 1);
        }
        out[outLength] = 0;
        at = end;
    }
    // A path ending in "/" names a directory, which lstat of the candidate with the slash tells.
    const size_t pathLength = strlen(path);
    if (pathLength > 1 && path[pathLength - 1] == '/' && strcmp(out, "/")) {
        char withSlash[kPathMax];
        const size_t length = strlen(out);
        if (length + 2 >= (size_t)kPathMax) {
            errno = ENAMETOOLONG;
            return false;
        }
        memcpy(withSlash, out, length);
        withSlash[length] = '/';
        withSlash[length + 1] = 0;
        Stat st{};
        if (Call(__NR_newfstatat, kAtFdCwd, withSlash, &st, kAtSymlinkNoFollow) < 0) return false;
    }
    return true;
}
}  // namespace

CL char* realpath(const char* path, char* resolved) {
    if (!path) {
        errno = EINVAL;
        return nullptr;
    }
    if (!*path) {
        errno = ENOENT;
        return nullptr;
    }
    char* out = resolved ? resolved : (char*)malloc((size_t)kPathMax);
    if (!out) return nullptr;
    if (!Canonicalize(path, out)) {
        if (!resolved) {
            const int saved = errno;
            free(out);
            errno = saved;
        }
        return nullptr;
    }
    return out;
}

// mkstemp and mkdtemp: six trailing X's replaced from glibc's alphabet and a fresh kernel's random
// bytes, retried while the name is taken (glibc's rule and its alphabet, so names look the same).
namespace {
constexpr int kTemplateLetters = 6;
constexpr int kTempAttempts = 62 * 62 * 62;

// Fills the six X's; false when the kernel has no randomness to offer.
bool FillTemplate(char* template_) {
    unsigned char bytes[kTemplateLetters];
    long got = 0;
    while (got < kTemplateLetters) {
        const long n = Call(__NR_getrandom, bytes + got, kTemplateLetters - got, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        got += n;
    }
    static const char kLetters[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for (int i = 0; i < kTemplateLetters; ++i)
        template_[i] = kLetters[bytes[i] % (sizeof kLetters - 1)];
    return true;
}

// The template must end in six X's, else EINVAL as glibc's answers.
int MakeTemp(char* template_, bool directory) {
    const size_t length = strlen(template_);
    if (length < kTemplateLetters ||
        memcmp(template_ + length - kTemplateLetters, "XXXXXX", kTemplateLetters)) {
        errno = EINVAL;
        return -1;
    }
    char* letters = template_ + length - kTemplateLetters;
    for (int attempt = 0; attempt < kTempAttempts; ++attempt) {
        if (!FillTemplate(letters)) return -1;
        const long result =
            directory ? Call(__NR_mkdir, template_, 0700)
                      : Call(__NR_openat, kAtFdCwd, template_,
                            2 /* O_RDWR */ | kOpenCreate | kOpenExclusive, 0600);
        if (result >= 0) return (int)result;
        if (errno != EEXIST) return -1;
    }
    errno = EEXIST;
    return -1;
}
}  // namespace

CL int mkstemp(char* template_) { return MakeTemp(template_, false); }
CL char* mkdtemp(char* template_) { return MakeTemp(template_, true) < 0 ? nullptr : template_; }

// fchmodat. Without a flag the kernel's own call; AT_SYMLINK_NOFOLLOW has no kernel form, so as
// glibc: open the entry itself, refuse a symlink (which has no mode to change), and chmod the file
// through /proc. Any other flag is EINVAL.
CL int fchmodat(int dirfd, const char* path, unsigned mode, int flags) {
    if (flags & ~kAtSymlinkNoFollow) {
        errno = EINVAL;
        return -1;
    }
    if (!(flags & kAtSymlinkNoFollow)) return (int)Call(__NR_fchmodat, dirfd, path, mode);
    const long opened = Call(__NR_openat, dirfd, path, kOpenPath | kOpenNoFollow | kOpenCloexec, 0);
    if (opened < 0) return -1;
    Stat st{};
    int result = -1;
    if (Call(__NR_fstat, (int)opened, &st) == 0) {
        if ((st.mode & kModeType) == kModeLink) {
            errno = ENOTSUP;
        } else {
            char link[64];
            const char* digits = "/proc/self/fd/";
            size_t at = strlen(digits);
            memcpy(link, digits, at);
            unsigned value = (unsigned)opened;
            char number[16];
            int used = 0;
            do {
                number[used++] = (char)('0' + value % 10);
                value /= 10;
            } while (value);
            while (used) link[at++] = number[--used];
            link[at] = 0;
            result = (int)Call(__NR_chmod, link, mode);
        }
    }
    const int saved = errno;
    Call(__NR_close, (int)opened);
    errno = saved;
    return result;
}

// statvfs: the kernel's statfs, mapped as glibc's internal_statvfs maps it: f_favail is f_ffree,
// f_frsize falls back to f_bsize when the kernel leaves it out, f_flag loses the kernel's "this is
// filled in" bit, the two halves of the filesystem id make f_fsid, and f_type came with glibc 2.39.
namespace {
void FillStatVfs(const SysStatFs& fs, void* buffer) {
    StatVfs* out = (StatVfs*)buffer;
    out->blockSize = (unsigned long)fs.blockSize;
    out->fragmentSize = (unsigned long)(fs.fragmentSize ? fs.fragmentSize : fs.blockSize);
    out->blocks = (unsigned long)fs.blocks;
    out->blocksFree = (unsigned long)fs.blocksFree;
    out->blocksAvailable = (unsigned long)fs.blocksAvailable;
    out->files = (unsigned long)fs.files;
    out->filesFree = (unsigned long)fs.filesFree;
    out->filesAvailable = (unsigned long)fs.filesFree;
    out->id = (unsigned long)(uint32_t)fs.id[0] | ((unsigned long)(uint32_t)fs.id[1] << 32);
    out->flags = (unsigned long)fs.flags ^ kStatValid;
    out->nameMax = (unsigned long)fs.nameLength;
    out->type = (unsigned)fs.type;
    for (int& spare : out->spare) spare = 0;
}
}  // namespace

CL int statvfs(const char* path, void* buffer) {
    SysStatFs fs{};
    if (Call(__NR_statfs, path, &fs) < 0) return -1;
    FillStatVfs(fs, buffer);
    return 0;
}
CL int fstatvfs(int fd, void* buffer) {
    SysStatFs fs{};
    if (Call(__NR_fstatfs, fd, &fs) < 0) return -1;
    FillStatVfs(fs, buffer);
    return 0;
}

// The environment, in glibc's `__environ` (see the header comment).
namespace {
bool secureEnvironment;  // AT_SECURE: started set-user-ID or set-group-ID

// The auxiliary vector follows the environment on the initial stack, as time.cpp reads it.
__attribute__((constructor(101))) void ReadSecure(int, char**, char** environment) {
    if (!environment) return;
    char** p = environment;
    while (*p) ++p;
    for (const unsigned long* aux = (const unsigned long*)(p + 1); aux[0]; aux += 2)
        if (aux[0] == kAtSecure) secureEnvironment = aux[1] != 0;
}

// Room for `count` entries plus the null, in an array of our own (the one the program started
// with cannot grow). The old array is freed only when setenv allocated it.
char** grownArray;
size_t grownCapacity;
bool EnsureRoom(size_t count) {
    if (__environ != grownArray) {
        grownArray = nullptr;
        grownCapacity = 0;
    }
    if (grownArray && count + 1 <= grownCapacity) return true;
    const size_t capacity = grownArray ? grownCapacity * 2 : count + 17;
    char** grown = (char**)malloc(capacity * sizeof(char*));
    if (!grown) {
        errno = ENOMEM;
        return false;
    }
    for (size_t i = 0; i < count; ++i) grown[i] = __environ[i];
    grown[count] = nullptr;
    free(grownArray);
    grownArray = grown;
    grownCapacity = capacity;
    __environ = grown;
    return true;
}
size_t CountEntries() {
    size_t count = 0;
    if (__environ)
        while (__environ[count]) ++count;
    return count;
}
char** FindSlot(const char* name, size_t length) {
    for (char** slot = __environ; slot && *slot; ++slot)
        if (!strncmp(*slot, name, length) && (*slot)[length] == '=') return slot;
    return nullptr;
}
bool ValidName(const char* name) { return name && *name && !strchr(name, '='); }
char* MakeEntry(const char* name, size_t nameLength, const char* value, size_t valueLength) {
    char* entry = (char*)malloc(nameLength + valueLength + 2);
    if (!entry) return nullptr;
    memcpy(entry, name, nameLength);
    entry[nameLength] = '=';
    memcpy(entry + nameLength + 1, value, valueLength + 1);
    return entry;
}
}  // namespace

CL char* getenv(const char* name) {
    if (!ValidName(name)) return nullptr;
    const size_t length = strlen(name);
    if (char** slot = FindSlot(name, length)) return *slot + length + 1;
    return nullptr;
}
CL char* secure_getenv(const char* name) { return secureEnvironment ? nullptr : getenv(name); }
CL int setenv(const char* name, const char* value, int overwrite) {
    if (!ValidName(name) || !value) {
        errno = EINVAL;
        return -1;
    }
    const size_t nameLength = strlen(name), valueLength = strlen(value);
    if (char** slot = FindSlot(name, nameLength)) {
        if (!overwrite) return 0;
        char* entry = MakeEntry(name, nameLength, value, valueLength);
        if (!entry) return -1;
        // The string being replaced stays allocated: a caller of getenv may still hold it, and
        // the environment lives as long as the program.
        *slot = entry;
        return 0;
    }
    const size_t count = CountEntries();
    if (!EnsureRoom(count + 1)) return -1;
    char* entry = MakeEntry(name, nameLength, value, valueLength);
    if (!entry) return -1;
    __environ[count] = entry;
    __environ[count + 1] = nullptr;
    return 0;
}
CL int unsetenv(const char* name) {
    if (!name || !*name || strchr(name, '=')) {
        errno = EINVAL;
        return -1;
    }
    if (char** slot = FindSlot(name, strlen(name))) {
        for (char** at = slot; *at; ++at) at[0] = at[1];
    }
    return 0;
}
// putenv keeps the caller's string (POSIX): a later change to it is a change to the environment.
CL int putenv(char* entry) {
    if (!entry) {
        errno = EINVAL;
        return -1;
    }
    const char* equals = strchr(entry, '=');
    if (!equals) return unsetenv(entry);  // as glibc: no "=" removes the name
    const size_t length = (size_t)(equals - entry);
    if (!length) {
        errno = EINVAL;
        return -1;
    }
    if (char** slot = FindSlot(entry, length)) {
        *slot = entry;
        return 0;
    }
    const size_t count = CountEntries();
    if (!EnsureRoom(count + 1)) return -1;
    __environ[count] = entry;
    __environ[count + 1] = nullptr;
    return 0;
}

// Processes.
namespace cordlibc {
// threads.cpp's atfork lists: malloc.cpp registers its hooks there with pthread_atfork, and any other
// caller's handlers are in the same lists. prepare runs newest first and parent and child oldest first.
__attribute__((visibility("hidden"))) void AtForkPrepare();
__attribute__((visibility("hidden"))) void AtForkParent();
__attribute__((visibility("hidden"))) void AtForkChild();
}  // namespace cordlibc

// Hidden in assembly above: GCC's builtin declaration would otherwise keep default visibility.
extern "C" int fork(void) {
    cordlibc::AtForkPrepare();
    const long child = Call(__NR_clone, kSigChild, 0, nullptr, nullptr, 0);
    if (child == 0) {
        cordlibc::AtForkChild();
        return 0;
    }
    const int result = (int)child;
    const int saved = errno;
    cordlibc::AtForkParent();
    errno = saved;
    return result;
}
CL int waitpid(int pid, int* status, int options) {
    return (int)Call(__NR_wait4, pid, status, options, nullptr);
}

// popen and pclose. The streams popen opened are kept in a list, since pclose needs the child and
// a child of a later popen must not inherit an earlier stream's descriptor (POSIX asks for that).
namespace {
struct PopenStream {
    FILE* file;
    int pid;
    int fd;  // the FILE's descriptor, to close it in a later popen's child
    PopenStream* next;
};
PopenStream* popenStreams;
int popenLock;

void LockPopen() {
    while (__atomic_exchange_n(&popenLock, 1, __ATOMIC_ACQUIRE)) __builtin_ia32_pause();
}
void UnlockPopen() { __atomic_store_n(&popenLock, 0, __ATOMIC_RELEASE); }
}  // namespace

CL FILE* popen(const char* command, const char* mode) {
    // glibc's modes: "r" or "w", with an optional "e" for a close-on-exec descriptor.
    const bool reading = mode && mode[0] == 'r';
    if (!command || !mode || (!reading && mode[0] != 'w') || (mode[1] && (mode[1] != 'e' || mode[2]))) {
        errno = EINVAL;
        return nullptr;
    }
    int fds[2];
    if (Call(__NR_pipe2, fds, kOpenCloexec) < 0) return nullptr;
    const int pid = fork();
    if (pid < 0) {
        const int saved = errno;
        Call(__NR_close, fds[0]);
        Call(__NR_close, fds[1]);
        errno = saved;
        return nullptr;
    }
    if (pid == 0) {
        // The child runs the command through the shell, with the pipe on the side popen reads or
        // writes. Nothing here allocates, so no lock of the parent's is needed. Streams an earlier
        // popen left open are closed here, as POSIX asks and glibc does.
        for (const PopenStream* stream = popenStreams; stream; stream = stream->next)
            Call(__NR_close, stream->fd);
        const int result = Call(__NR_dup2, fds[reading ? 1 : 0], reading ? 1 : 0);
        Call(__NR_close, fds[0]);
        Call(__NR_close, fds[1]);
        if (result < 0) _exit(127);
        execl("/bin/sh", "sh", "-c", command, (char*)nullptr);
        _exit(127);
    }
    const int kept = reading ? fds[0] : fds[1];
    Call(__NR_close, fds[reading ? 1 : 0]);
    if (mode[1] == 'e') Call(__NR_fcntl, kept, kFSetFd, (long)kFdCloexec);
    FILE* file = fdopen(kept, mode);
    if (!file) {
        const int saved = errno;
        Call(__NR_close, kept);
        waitpid(pid, nullptr, 0);
        errno = saved;
        return nullptr;
    }
    PopenStream* stream = (PopenStream*)malloc(sizeof(PopenStream));
    if (!stream) {
        const int saved = errno;
        fclose(file);
        waitpid(pid, nullptr, 0);
        errno = saved;
        return nullptr;
    }
    stream->file = file;
    stream->pid = pid;
    stream->fd = kept;
    LockPopen();
    stream->next = popenStreams;
    popenStreams = stream;
    UnlockPopen();
    return file;
}
// pclose closes the stream and reaps the child, reporting its wait status as glibc does; a stream
// popen did not open is closed and its own result returned, which is what glibc does as well.
CL int pclose(FILE* file) {
    if (!file) {
        errno = EINVAL;
        return -1;
    }
    PopenStream* found = nullptr;
    LockPopen();
    for (PopenStream** at = &popenStreams; *at; at = &(*at)->next)
        if ((*at)->file == file) {
            found = *at;
            *at = found->next;
            break;
        }
    UnlockPopen();
    if (!found) return fclose(file);
    const int pid = found->pid;
    free(found);
    int result = fclose(file) == 0 ? 0 : -1;
    const int closeError = result < 0 ? errno : 0;
    int status = 0;
    int waited;
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) return -1;
    if (result < 0) {
        errno = closeError;
        return -1;
    }
    return status;
}

// posix_spawn, posix_spawnp and the file actions and attributes cordinal uses. The two structures
// are glibc's public ones (spawn.h), which callers allocate on their own stack, so their sizes
// must match their headers' (files_checks.h checks that).
namespace {
struct SpawnAction;
// glibc's posix_spawnattr_t (spawn.h).
struct SpawnAttr {
    short flags;
    int group;
    uint64_t defaultSignals[16];
    uint64_t maskedSignals[16];
    int priority;
    int policy;
    int cgroup;
    int pad[15];
};
static_assert(sizeof(SpawnAttr) == 336, "glibc's posix_spawnattr_t");
// glibc's posix_spawn_file_actions_t (spawn.h).
struct SpawnFileActions {
    int allocated, used;
    SpawnAction* actions;
    int pad[16];
};
static_assert(sizeof(SpawnFileActions) == 80, "glibc's posix_spawn_file_actions_t");

enum SpawnTag { kSpawnClose, kSpawnDup2, kSpawnOpen, kSpawnChangeDir };
struct SpawnAction {
    int tag;
    int fd, newFd;
    int openFlags;
    unsigned mode;
    char* path;  // owned by the list, for open and chdir
};
struct SpawnRequest {
    const char* path;
    const char* searchPath;  // PATH's value for posix_spawnp, or null
    char* const* argv;
    char* const* envp;
    const SpawnFileActions* actions;
    const SpawnAttr* attr;
};

int AddAction(void* handle, const SpawnAction& action) {
    SpawnFileActions* actions = (SpawnFileActions*)handle;
    if (!actions) return EINVAL;
    if (actions->used == actions->allocated) {
        const int allocated = actions->allocated ? actions->allocated * 2 : 8;
        SpawnAction* grown =
            (SpawnAction*)realloc(actions->actions, (size_t)allocated * sizeof(SpawnAction));
        if (!grown) return ENOMEM;
        actions->actions = grown;
        actions->allocated = allocated;
    }
    actions->actions[actions->used++] = action;
    return 0;
}

// In the child: the file actions in order, as glibc runs them.
int RunActions(const SpawnFileActions* actions) {
    if (!actions) return 0;
    for (int i = 0; i < actions->used; ++i) {
        const SpawnAction& action = actions->actions[i];
        switch (action.tag) {
            case kSpawnClose:
                Call(__NR_close, action.fd);  // an already closed descriptor is fine, as in glibc
                break;
            case kSpawnDup2:
                if (action.fd == action.newFd) {
                    // dup2 to itself does nothing, so the inherited CLOEXEC is cleared by hand.
                    if (Call(__NR_fcntl, action.fd, kFSetFd, 0L) < 0) return errno;
                } else if (Call(__NR_dup2, action.fd, action.newFd) < 0) {
                    return errno;
                }
                break;
            case kSpawnOpen: {
                const long opened = Call(__NR_openat, kAtFdCwd, action.path, action.openFlags, action.mode);
                if (opened < 0) return errno;
                if ((int)opened != action.fd) {
                    if (Call(__NR_dup2, (int)opened, action.fd) < 0) return errno;
                    Call(__NR_close, (int)opened);
                }
                break;
            }
            case kSpawnChangeDir:
                if (Call(__NR_chdir, action.path) < 0) return errno;
                break;
        }
    }
    return 0;
}

// In the child: the attributes. The kernel's sigaction has the handler first and glibc's has the
// mask first, so a default disposition is built in the kernel's layout.
int RunAttributes(const SpawnAttr* attr) {
    if (!attr) return 0;
    const short flags = attr->flags;
    if (flags & kSpawnSetPGroup)
        if (Call(__NR_setpgid, 0, attr->group) < 0) return errno;
    if (flags & kSpawnResetIds) {
        if (Call(__NR_setgid, cordlibc::Syscall(__NR_getgid)) < 0) return errno;
        if (Call(__NR_setuid, cordlibc::Syscall(__NR_getuid)) < 0) return errno;
    }
    if (flags & kSpawnSetSigMask)
        if (Call(__NR_rt_sigprocmask, kSigSetMask, attr->maskedSignals, nullptr,
                 (long)sizeof(unsigned long)) < 0)
            return errno;
    if (flags & kSpawnSetSigDef) {
        struct KernelSigaction {
            void* handler;
            unsigned long flags;
            void* restorer;
            unsigned long mask;
        } const action{};
        for (int signal = 1; signal <= kSignals; ++signal) {
            if (!((attr->defaultSignals[(signal - 1) / 64] >> ((signal - 1) % 64)) & 1)) continue;
            // 32 and 33 have no kernel disposition: their failure is not an error, as in glibc.
            Call(__NR_rt_sigaction, signal, &action, nullptr, (long)sizeof(unsigned long));
        }
    }
    if (flags & kSpawnSetScheduler) {
        if (Call(__NR_sched_setscheduler, 0, attr->policy, &attr->priority) < 0) return errno;
    } else if (flags & kSpawnSetSchedParam) {
        if (Call(__NR_sched_setparam, 0, &attr->priority) < 0) return errno;
    }
    if (flags & kSpawnSetSid)
        if (Call(__NR_setsid) < 0) return errno;
    return 0;
}

// In the child: exec `request`, which returns only on failure with errno set. posix_spawnp's search
// follows execvp's: every PATH element in turn, an empty one meaning the current directory, with
// EACCES reported when any candidate was found but could not be run.
void RunExec(const SpawnRequest& request) {
    if (!request.searchPath || strchr(request.path, '/')) {
        Call(__NR_execve, request.path, request.argv, request.envp);
        return;
    }
    int last = ENOENT;
    bool denied = false;
    const char* at = request.searchPath;
    for (;;) {
        const char* colon = strchr(at, ':');
        const size_t length = colon ? (size_t)(colon - at) : strlen(at);
        char candidate[kPathMax];
        const size_t nameLength = strlen(request.path);
        if (length + 1 + nameLength + 1 >= (size_t)kPathMax) {
            last = ENAMETOOLONG;
        } else {
            if (length) {
                memcpy(candidate, at, length);
            } else {
                candidate[0] = '.';
            }
            candidate[length ? length : 1] = '/';
            memcpy(candidate + (length ? length : 1) + 1, request.path, nameLength + 1);
            Call(__NR_execve, candidate, request.argv, request.envp);
            last = errno;
            if (last == EACCES) denied = true;
        }
        if (!colon) break;
        at = colon + 1;
    }
    errno = denied ? EACCES : last;
}

[[noreturn]] void SpawnChild(int errorFd, const SpawnRequest& request) {
    int error = RunActions(request.actions);
    if (!error) error = RunAttributes(request.attr);
    if (!error) {
        RunExec(request);
        error = errno;
    }
    Call(__NR_write, errorFd, &error, (long)sizeof error);
    _exit(127);
}

int Spawn(int* pid, const char* path, const void* fileActions, const void* attributes,
          char* const argv[], char* const envp[], bool search) {
    if (!path) return EINVAL;
    SpawnRequest request{};
    request.path = path;
    request.argv = argv;
    request.envp = envp;
    request.actions = (const SpawnFileActions*)fileActions;
    request.attr = (const SpawnAttr*)attributes;
    if (search) {
        request.searchPath = getenv("PATH");
        if (!request.searchPath) request.searchPath = "/bin:/usr/bin";  // confstr's _CS_PATH
    }
    int fds[2];
    if (Call(__NR_pipe2, fds, kOpenCloexec) < 0) return errno;
    const int child = fork();
    if (child < 0) {
        const int error = errno;
        Call(__NR_close, fds[0]);
        Call(__NR_close, fds[1]);
        return error;
    }
    if (child == 0) SpawnChild(fds[1], request);
    Call(__NR_close, fds[1]);
    int error = 0;
    long got;
    do {
        got = Call(__NR_read, fds[0], &error, (long)sizeof error);
    } while (got < 0 && errno == EINTR);
    const int readError = got < 0 ? errno : 0;
    Call(__NR_close, fds[0]);
    if (got == (long)sizeof error) {
        int status = 0;
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
        errno = error;  // glibc's child shares the caller's memory and leaves it set too
        return error;
    }
    if (readError) {
        int status = 0;
        waitpid(child, &status, 0);
        errno = readError;
        return readError;
    }
    if (pid) *pid = child;
    return 0;
}
}  // namespace

CL int posix_spawn(int* pid, const char* path, const void* fileActions, const void* attributes,
                   char* const argv[], char* const envp[]) {
    return Spawn(pid, path, fileActions, attributes, argv, envp, false);
}
CL int posix_spawnp(int* pid, const char* path, const void* fileActions, const void* attributes,
                    char* const argv[], char* const envp[]) {
    return Spawn(pid, path, fileActions, attributes, argv, envp, true);
}

CL int posix_spawn_file_actions_init(void* handle) {
    memset(handle, 0, sizeof(SpawnFileActions));
    return 0;
}
CL int posix_spawn_file_actions_destroy(void* handle) {
    SpawnFileActions* actions = (SpawnFileActions*)handle;
    if (!actions) return EINVAL;
    for (int i = 0; i < actions->used; ++i) free(actions->actions[i].path);
    free(actions->actions);
    memset(actions, 0, sizeof *actions);
    return 0;
}
CL int posix_spawn_file_actions_addclose(void* handle, int fd) {
    if (fd < 0) return EBADF;
    SpawnAction action{};
    action.tag = kSpawnClose;
    action.fd = fd;
    return AddAction(handle, action);
}
CL int posix_spawn_file_actions_adddup2(void* handle, int fd, int newFd) {
    if (fd < 0 || newFd < 0) return EBADF;
    SpawnAction action{};
    action.tag = kSpawnDup2;
    action.fd = fd;
    action.newFd = newFd;
    return AddAction(handle, action);
}
CL int posix_spawn_file_actions_addopen(void* handle, int fd, const char* path, int flags, unsigned mode) {
    if (fd < 0) return EBADF;
    if (!path) return EINVAL;
    const size_t length = strlen(path) + 1;
    char* copy = (char*)malloc(length);
    if (!copy) return ENOMEM;
    memcpy(copy, path, length);
    SpawnAction action{};
    action.tag = kSpawnOpen;
    action.fd = fd;
    action.openFlags = flags;
    action.mode = mode;
    action.path = copy;
    const int result = AddAction(handle, action);
    if (result) free(copy);
    return result;
}
CL int posix_spawn_file_actions_addchdir_np(void* handle, const char* path) {
    if (!path) return EINVAL;
    const size_t length = strlen(path) + 1;
    char* copy = (char*)malloc(length);
    if (!copy) return ENOMEM;
    memcpy(copy, path, length);
    SpawnAction action{};
    action.tag = kSpawnChangeDir;
    action.path = copy;
    const int result = AddAction(handle, action);
    if (result) free(copy);
    return result;
}
CL int posix_spawnattr_init(void* handle) {
    memset(handle, 0, sizeof(SpawnAttr));
    return 0;
}
CL int posix_spawnattr_destroy(void* handle) { return handle ? 0 : EINVAL; }
CL int posix_spawnattr_setflags(void* handle, short flags) {
    if (flags & ~kSpawnKnown) return EINVAL;
    ((SpawnAttr*)handle)->flags = flags;
    return 0;
}
CL int posix_spawnattr_getflags(const void* handle, short* flags) {
    *flags = ((const SpawnAttr*)handle)->flags;
    return 0;
}
CL int posix_spawnattr_setpgroup(void* handle, int group) {
    ((SpawnAttr*)handle)->group = group;
    return 0;
}
CL int posix_spawnattr_getpgroup(const void* handle, int* group) {
    *group = ((const SpawnAttr*)handle)->group;
    return 0;
}

// execl: the arguments up to their null sentinel on the stack, then the kernel's exec. Hidden in
// assembly at the top of the file, for the same reason as fork.
extern "C" int execl(const char* path, const char* arg, ...) {
    va_list arguments, counter;
    va_start(arguments, arg);
    va_copy(counter, arguments);
    size_t count = 1;  // `arg`
    while (va_arg(counter, const char*)) ++count;
    va_end(counter);
    const char** argv = (const char**)__builtin_alloca((count + 1) * sizeof(char*));
    argv[0] = arg;
    for (size_t i = 1; i <= count; ++i) argv[i] = va_arg(arguments, const char*);
    va_end(arguments);
    return (int)Call(__NR_execve, path, argv, __environ);
}

// execlp: execl's stack of arguments with the PATH search RunExec performs for posix_spawnp, so a
// program can be started by name in both links (the font checks run fc-match this way). The search
// follows execvp's: every PATH element in turn, an empty one meaning the current directory, with
// EACCES kept when a candidate was found but could not run. Like execl it is hidden in assembly
// above, and a name with a slash goes to the kernel as it is.
extern "C" int execlp(const char* file, const char* arg, ...) {
    va_list arguments, counter;
    va_start(arguments, arg);
    va_copy(counter, arguments);
    size_t count = 1;  // `arg`
    while (va_arg(counter, const char*)) ++count;
    va_end(counter);
    const char** argv = (const char**)__builtin_alloca((count + 1) * sizeof(char*));
    argv[0] = arg;
    for (size_t i = 1; i <= count; ++i) argv[i] = va_arg(arguments, const char*);
    va_end(arguments);
    SpawnRequest request{};
    request.path = file;
    request.argv = (char* const*)argv;
    request.envp = __environ;
    request.searchPath = getenv("PATH");
    if (!request.searchPath) request.searchPath = "/bin:/usr/bin";  // confstr's _CS_PATH
    RunExec(request);
    return -1;  // RunExec returns only on failure, with errno set.
}
