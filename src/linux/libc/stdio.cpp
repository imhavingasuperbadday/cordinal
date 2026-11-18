// cordinal's own FILE streams (stage 5a of replacing glibc): fopen and the rest of the stream
// layer over plain read, write and lseek calls, with stdin, stdout and stderr as statically
// allocated streams. Hidden like kernel.cpp, so cordinal and the static libstdc++ use these while
// glibc keeps its own FILE streams for its internal work. printf.cpp writes through this layer and
// scanf.cpp reads through it; the formatting cores are in those files.
//
// The layout is cordinal's own (stdio_internal.h), because glibc's printf cannot write to it and
// this printf cannot write to glibc's. Buffers come from the process-wide allocator (malloc.cpp),
// so free is the same free glibc uses.
//
// Buffering follows glibc: stderr is unbuffered, stdout and stdin are line buffered on a terminal
// and fully buffered otherwise, and an explicit setvbuf overrides both. A stream's buffer is either
// the read window or the pending write bytes, never both; ungetc keeps its pushback apart so it can
// cross buffer boundaries without moving them. The three standard streams are constant-initialized
// and in the process list from the start, so they work before any constructor runs (libstdc++
// initializes iostreams in one). A flush of everything at exit is registered with glibc's atexit so
// buffered output reaches the descriptor when the program returns from main.
//
// Locking is one futex per stream, the shape malloc.cpp uses, and a list lock held only when a
// stream is added or removed and by fflush(NULL). Both multi-lock paths take the list lock first,
// so no two threads can wait for each other's locks. fork copies a lock a sibling thread held, as
// any library without atfork support does; cordinal's forked children only exec or exit, and stage
// 8 brings real thread support.
//
// No glibc header that declares these names is included: the definitions are the declarations.
#include "stdio_internal.h"

#include <errno.h>
#include <string.h>

#include "syscall.h"

// The stdio constants a caller sees, from <stdio.h>; no header that declares these functions is
// included, so they are written here.
#ifndef EOF
#define EOF (-1)
#endif
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif
#ifndef _IOFBF
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2
#endif

// The process-wide allocator (malloc.cpp, default visibility) and glibc's atexit, which runs the
// final flush. strerror is glibc's until stage 5b.
extern "C" void* malloc(size_t);
extern "C" void free(void*);
extern "C" void* realloc(void*, size_t);
extern "C" int atexit(void (*)(void));

using cordlibc::Call;

// One futex per stream (stdio_internal.h), the shape malloc.cpp's lock has: 0 free, 1 held, 2 held
// with waiters, spinning briefly first and then sleeping in the kernel. stdio's critical sections
// are short, so the spin carries most of the work.
namespace cordlibc {
void StreamLock::Acquire() {
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
        Call(__NR_futex, (long)&state, 128 /* FUTEX_WAIT_PRIVATE */, 2, 0);
        c = __atomic_exchange_n(&state, 2, __ATOMIC_ACQUIRE);
    }
}
bool StreamLock::TryAcquire() {
    int c = 0;
    return __atomic_compare_exchange_n(&state, &c, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}
void StreamLock::Release() {
    if (__atomic_exchange_n(&state, 0, __ATOMIC_RELEASE) == 2)
        Call(__NR_futex, (long)&state, 129 /* FUTEX_WAKE_PRIVATE */, 1);
}
}  // namespace cordlibc

// Forward: the atexit flush below, and rewind's flag clearing, are defined further down.
extern "C" __attribute__((visibility("hidden"))) int fflush(FILE*);
extern "C" __attribute__((visibility("hidden"))) void clearerr(FILE*);

namespace {

constexpr int kAtFdCwd = -100;
constexpr int kOModeMask = 3;
constexpr int kOReadOnly = 0;
constexpr int kOWriteOnly = 1;
constexpr int kOReadWrite = 2;
constexpr int kOCreate = 0100;
constexpr int kOExclusive = 0200;
constexpr int kOTruncate = 01000;
constexpr int kOAppend = 02000;
constexpr int kOCloexec = 02000000;
constexpr int kFSetFd = 2;
constexpr int kFGetFl = 3;
constexpr int kFSetFl = 4;
constexpr int kFdCloexec = 1;
constexpr int kTCGETS = 0x5401;

// The three streams the program starts with. Constant-initialized (no constructor) and linked into
// the list before anything runs, so printf and scanf work in any constructor, including
// libstdc++'s iostreams. Built in reverse so each `next` is the address of the one before it.
_IO_FILE stderrFile = {2, kWritable | kUnbuffered, 0, 0, nullptr, 0, 0, 0, nullptr, 0, 0, {0}, nullptr};
_IO_FILE stdoutFile = {1, kWritable, 0, 0, nullptr, 0, 0, 0, nullptr, 0, 0, {0}, &stderrFile};
_IO_FILE stdinFile = {0, kRead, 0, 0, nullptr, 0, 0, 0, nullptr, 0, 0, {0}, &stdoutFile};

cordlibc::StreamLock listLock;
_IO_FILE* registryHead = &stdinFile;
int flushRegistered;  // 0 not yet, 1 registering, 2 registered

struct Hold {
    _IO_FILE* stream;
    explicit Hold(_IO_FILE* f) : stream(f) { stream->lock.Acquire(); }
    ~Hold() { stream->lock.Release(); }
};

int FcntlRaw(int fd, int command, long argument) { return (int)Call(__NR_fcntl, fd, command, argument); }

// Registered with glibc's atexit the first time anything is written to or opened: a program that
// only uses the standard streams (a scan printing its result, say) needs the final flush too.
void FlushAllAtExit() { fflush(nullptr); }

void EnsureFlushRegistered() {
    if (__atomic_load_n(&flushRegistered, __ATOMIC_ACQUIRE) == 2) return;
    int expected = 0;
    if (__atomic_compare_exchange_n(&flushRegistered, &expected, 1, false, __ATOMIC_ACQ_REL,
                                    __ATOMIC_ACQUIRE)) {
        atexit(FlushAllAtExit);
        __atomic_store_n(&flushRegistered, 2, __ATOMIC_RELEASE);
    }
}

// The descriptor's position plus whatever this stream has not passed on yet: pending writes are
// ahead of it, unread and pushed-back bytes behind.
long long TellLocked(_IO_FILE* f) {
    const long position = Call(__NR_lseek, f->fd, 0, SEEK_CUR);
    if (position < 0) return -1;
    if (f->writeLength) return (long long)position + (long long)f->writeLength;
    return (long long)position - (long long)((f->readEnd - f->readPosition) + f->pushbackLength);
}

// A stream's buffering mode: the terminal check (once), then the buffer if it needs one. errno is
// kept across the terminal probe, which is not the caller's failure.
int PrepareBuffer(_IO_FILE* f) {
    if (!(f->flags & kModeKnown)) {
        f->flags |= kModeKnown;
        struct {
            unsigned char bytes[64];
        } terminal{};
        const int saved = errno;
        if (Call(__NR_ioctl, f->fd, kTCGETS, &terminal) == 0) f->flags |= kLineBuffered;
        errno = saved;
    }
    if (f->flags & (kUnbuffered)) return 0;
    if (f->buffer) return 0;
    char* buffer = (char*)malloc(f->bufferSize ? f->bufferSize : cordlibc::kStreamBufferSize);
    if (!buffer) {
        f->flags |= kError;
        return -1;
    }
    f->buffer = buffer;
    if (!f->bufferSize) f->bufferSize = cordlibc::kStreamBufferSize;
    f->ownsBuffer = 1;
    return 0;
}

// Give the descriptor back the bytes this stream has read ahead, and forget the pushback.
int SeekBackUnread(_IO_FILE* f) {
    const size_t back = (f->readEnd - f->readPosition) + f->pushbackLength;
    if (!back) return 0;
    return (int)Call(__NR_lseek, f->fd, -(long)back, SEEK_CUR);
}

// One write call, continued while it comes back short; on failure the stream's error flag is set and
// `failed` says so, with the byte count that did reach the descriptor (POSIX's fwrite result).
size_t WriteRaw(_IO_FILE* f, const char* p, size_t n, bool* failed) {
    size_t done = 0;
    *failed = false;
    while (done < n) {
        const long wrote = Call(__NR_write, f->fd, p + done, n - done);
        if (wrote < 0) {
            if (errno == EINTR) continue;
            f->flags |= kError;
            *failed = true;
            return done;
        }
        if (wrote == 0) {
            errno = EIO;
            f->flags |= kError;
            *failed = true;
            return done;
        }
        done += (size_t)wrote;
    }
    return done;
}

// The pending write bytes, through the descriptor.
int FlushLocked(_IO_FILE* f) {
    if (!f->writeLength) return 0;
    const size_t length = f->writeLength;
    f->writeLength = 0;
    bool failed = false;
    WriteRaw(f, f->buffer, length, &failed);
    return failed ? -1 : 0;
}

// Fix the descriptor before a stream that has been reading starts writing, or the other way round.
void PrepareWrite(_IO_FILE* f) {
    if (!(f->readEnd > f->readPosition) && !f->pushbackLength) return;
    SeekBackUnread(f);  // a pipe cannot seek; its next write starts wherever the reads stopped
    f->readPosition = f->readEnd = 0;
    f->pushbackLength = 0;
}

size_t WriteLocked(_IO_FILE* f, const char* p, size_t n) {
    if (!(f->flags & kWritable)) {
        errno = EBADF;
        f->flags |= kError;
        return 0;
    }
    EnsureFlushRegistered();
    if (!f->writeLength) PrepareWrite(f);
    if (f->flags & kUnbuffered) {
        bool failed = false;
        return WriteRaw(f, p, n, &failed);
    }
    if (PrepareBuffer(f) < 0) return 0;
    size_t done = 0;
    while (done < n) {
        if (!f->writeLength && n - done >= f->bufferSize) {
            bool failed = false;
            done += WriteRaw(f, p + done, n - done, &failed);
            if (failed) return done;
            continue;
        }
        size_t room = f->bufferSize - f->writeLength;
        if (!room) {
            if (FlushLocked(f) < 0) return done;
            room = f->bufferSize;
        }
        const size_t take = n - done < room ? n - done : room;
        memcpy(f->buffer + f->writeLength, p + done, take);
        f->writeLength += take;
        done += take;
        if ((f->flags & kLineBuffered) && memchr(p + done - take, '\n', take)) {
            if (FlushLocked(f) < 0) return done;
        } else if (f->writeLength == f->bufferSize) {
            if (FlushLocked(f) < 0) return done;
        }
    }
    return done;
}

// One fill from the descriptor into the buffer.
int FillLocked(_IO_FILE* f) {
    if (!(f->flags & kRead)) {
        errno = EBADF;
        f->flags |= kError;
        return -1;
    }
    if (f->writeLength && FlushLocked(f) < 0) return -1;
    if (PrepareBuffer(f) < 0) return -1;
    const long got = Call(__NR_read, f->fd, f->buffer, f->bufferSize);
    if (got < 0) {
        f->flags |= kError;
        return -1;
    }
    if (got == 0) {
        f->flags |= kEndOfFile;
        return -1;
    }
    f->readPosition = 0;
    f->readEnd = (size_t)got;
    return 0;
}

size_t ReadLocked(_IO_FILE* f, char* p, size_t n) {
    if (!(f->flags & kRead)) {
        errno = EBADF;
        f->flags |= kError;
        return 0;
    }
    if (f->writeLength && FlushLocked(f) < 0) return 0;
    if (f->flags & kUnbuffered) {
        const long got = Call(__NR_read, f->fd, p, n);
        if (got < 0) {
            f->flags |= kError;
            return 0;
        }
        if (got == 0) f->flags |= kEndOfFile;
        return (size_t)got;
    }
    if (PrepareBuffer(f) < 0) return 0;
    size_t done = 0;
    while (done < n) {
        size_t available = f->readEnd - f->readPosition;
        if (!available) {
            if (FillLocked(f) < 0) break;
            available = f->readEnd - f->readPosition;
        }
        const size_t take = n - done < available ? n - done : available;
        memcpy(p + done, f->buffer + f->readPosition, take);
        f->readPosition += take;
        done += take;
    }
    return done;
}

int GetLocked(_IO_FILE* f) {
    if (f->pushbackLength) return (unsigned char)f->pushback[--f->pushbackLength];
    if (f->readPosition < f->readEnd) return (unsigned char)f->buffer[f->readPosition++];
    if (!(f->flags & kRead)) {
        errno = EBADF;
        f->flags |= kError;
        return EOF;
    }
    if (f->writeLength && FlushLocked(f) < 0) return EOF;
    if (f->flags & kUnbuffered) {
        unsigned char c;
        const long got = Call(__NR_read, f->fd, &c, 1);
        if (got < 0) {
            f->flags |= kError;
            return EOF;
        }
        if (got == 0) {
            f->flags |= kEndOfFile;
            return EOF;
        }
        return c;
    }
    if (FillLocked(f) < 0) return EOF;
    return (unsigned char)f->buffer[f->readPosition++];
}

int PutLocked(_IO_FILE* f, int c) {
    const char byte = (char)(unsigned char)c;
    if (!(f->flags & kWritable)) {
        errno = EBADF;
        f->flags |= kError;
        return EOF;
    }
    EnsureFlushRegistered();
    if (f->flags & kUnbuffered) {
        bool failed = false;
        WriteRaw(f, &byte, 1, &failed);
        return failed ? EOF : (unsigned char)c;
    }
    if (!f->writeLength) PrepareWrite(f);
    if (PrepareBuffer(f) < 0) return EOF;
    if (f->writeLength == f->bufferSize && FlushLocked(f) < 0) return EOF;
    f->buffer[f->writeLength++] = byte;
    if ((f->flags & kLineBuffered) && byte == '\n' && FlushLocked(f) < 0) return EOF;
    return (unsigned char)c;
}

FILE* MakeStream(int fd, unsigned flags) {
    FILE* f = (FILE*)malloc(sizeof(FILE));
    if (!f) {
        Call(__NR_close, fd);
        return nullptr;
    }
    memset(f, 0, sizeof *f);
    f->fd = fd;
    f->flags = flags;
    listLock.Acquire();
    f->next = registryHead;
    registryHead = f;
    listLock.Release();
    EnsureFlushRegistered();
    return f;
}

// The mode string, as glibc reads it: r, w or a first; then +, b, e, x, and glibc's m and c
// (which only choose its own buffer strategy, pointless here). Anything else is EINVAL. For fdopen
// (`descriptor`), x has nothing to guard, since the call never creates anything, and glibc ignores it.
struct Mode {
    unsigned streamFlags;
    int openFlags;
    bool read, write, append, closeOnExec;
};

bool ParseMode(const char* mode, Mode* out, bool descriptor = false) {
    if (!mode) {
        errno = EINVAL;
        return false;
    }
    Mode m{};
    switch (mode[0]) {
        case 'r': m.read = true; m.openFlags = kOReadOnly; break;
        case 'w': m.write = true; m.openFlags = kOWriteOnly | kOCreate | kOTruncate; break;
        case 'a':
            m.write = true;
            m.append = true;
            m.openFlags = kOWriteOnly | kOCreate | kOAppend;
            break;
        default: errno = EINVAL; return false;
    }
    bool exclusive = false;
    for (const char* p = mode + 1; *p; ++p) {
        switch (*p) {
            case '+': m.read = m.write = true; break;
            case 'b': break;
            case 'e': m.closeOnExec = true; break;
            case 'x': exclusive = true; break;
            case 'm': case 'c': break;
            default: errno = EINVAL; return false;
        }
    }
    if (m.read && m.write) m.openFlags = (m.openFlags & ~kOModeMask) | kOReadWrite;
    else if (m.read) m.openFlags = (m.openFlags & ~kOModeMask) | kOReadOnly;
    else m.openFlags = (m.openFlags & ~kOModeMask) | kOWriteOnly;
    if (exclusive) {
        if (descriptor) {
            // Nothing to guard: fdopen opens nothing. glibc accepts the modifier and ignores it.
        } else if (!(m.openFlags & (kOCreate))) {  // x only means something when the call creates
            errno = EINVAL;
            return false;
        } else {
            m.openFlags |= kOExclusive;
        }
    }
    if (m.closeOnExec) m.openFlags |= kOCloexec;
    m.streamFlags = (m.read ? kRead : 0u) | (m.write ? kWritable : 0u) | (m.append ? kAppend : 0u);
    *out = m;
    return true;
}

}  // namespace

#define CL extern "C" __attribute__((visibility("hidden")))

// The stream functions that GCC and libstdc++ also treat as builtins declare themselves with
// default visibility, so as _exit and fork in kernel.cpp they are hidden again in assembly.
__asm__(".hidden stdin");
__asm__(".hidden stdout");
__asm__(".hidden stderr");
__asm__(".hidden fclose");
__asm__(".hidden fdopen");
__asm__(".hidden feof");
__asm__(".hidden ferror");
__asm__(".hidden fflush");
__asm__(".hidden fgetc");
__asm__(".hidden fgets");
__asm__(".hidden fileno");
__asm__(".hidden fopen");
__asm__(".hidden fputc");
__asm__(".hidden fputs");
__asm__(".hidden fread");
__asm__(".hidden fseek");
__asm__(".hidden ftell");
__asm__(".hidden fwrite");
__asm__(".hidden getc");
__asm__(".hidden getchar");
__asm__(".hidden putc");
__asm__(".hidden putchar");
__asm__(".hidden puts");
__asm__(".hidden ungetc");

extern "C" {
__attribute__((visibility("hidden"))) FILE* stdin = &stdinFile;
__attribute__((visibility("hidden"))) FILE* stdout = &stdoutFile;
__attribute__((visibility("hidden"))) FILE* stderr = &stderrFile;
}

namespace cordlibc {
bool StreamWantsRead(FILE* f) { return (f->flags & (kRead | kClosed)) == kRead; }
bool StreamWantsWrite(FILE* f) { return (f->flags & (kWritable | kClosed)) == kWritable; }
}  // namespace cordlibc

// Opening and closing.
CL FILE* fopen(const char* path, const char* mode) {
    Mode m;
    if (!ParseMode(mode, &m) || !path) {
        if (!path) errno = EINVAL;
        return nullptr;
    }
    const int fd = (int)Call(__NR_openat, kAtFdCwd, path, m.openFlags, 0666);
    if (fd < 0) return nullptr;
    return MakeStream(fd, m.streamFlags);
}

CL FILE* fdopen(int fd, const char* mode) {
    Mode m;
    if (!ParseMode(mode, &m, true)) return nullptr;
    m.openFlags &= ~(kOCreate | kOTruncate | kOExclusive);  // fdopen never creates or truncates
    const int saved = errno;
    const int status = FcntlRaw(fd, kFGetFl, 0);
    if (status < 0) return nullptr;
    const int access = status & kOModeMask;
    const bool allowed = m.read && m.write ? access == kOReadWrite
                         : m.read            ? access == kOReadOnly || access == kOReadWrite
                                             : access == kOWriteOnly || access == kOReadWrite;
    if (!allowed) {
        errno = EINVAL;
        return nullptr;
    }
    if (m.append && !(status & kOAppend)) FcntlRaw(fd, kFSetFl, status | kOAppend);
    if (m.closeOnExec) FcntlRaw(fd, kFSetFd, kFdCloexec);
    errno = saved;
    return MakeStream(fd, m.streamFlags);
}

CL FILE* freopen(const char* path, const char* mode, FILE* f) {
    Mode m;
    if (!ParseMode(mode, &m)) return nullptr;
    if (!f) {
        errno = EINVAL;
        return nullptr;
    }
    listLock.Acquire();
    bool opened = true;
    {
        Hold hold(f);
        if (f->flags & kWritable) FlushLocked(f);
        else SeekBackUnread(f);  // the old stream's unread bytes go back to the descriptor
        if (f->ownsBuffer) free(f->buffer);
        if (f->pushback) free(f->pushback);
        f->buffer = nullptr;
        f->bufferSize = 0;
        f->ownsBuffer = 0;
        f->pushback = nullptr;
        f->pushbackLength = f->pushbackCapacity = 0;
        f->readPosition = f->readEnd = 0;
        f->writeLength = 0;
        f->flags = m.streamFlags;
        if (!path) {  // reopen the same descriptor with a new mode
            const int status = FcntlRaw(f->fd, kFGetFl, 0);
            if (status < 0) {
                opened = false;
            } else {
                FcntlRaw(f->fd, kFSetFl,
                         (status & ~(kOModeMask | kOAppend)) |
                             (m.read && m.write ? kOReadWrite : m.read ? kOReadOnly : kOWriteOnly) |
                             (m.append ? kOAppend : 0));
                if (m.closeOnExec) FcntlRaw(f->fd, kFSetFd, kFdCloexec);
            }
        } else {
            Call(__NR_close, f->fd);
            const int fd = (int)Call(__NR_openat, kAtFdCwd, path, m.openFlags, 0666);
            if (fd < 0) {
                opened = false;
            } else {
                f->fd = fd;
            }
        }
        if (!opened) f->flags = kClosed;
    }
    // A failed reopen closes the stream for good: a heap stream leaves the list as fclose would.
    const bool standard = f == &stdinFile || f == &stdoutFile || f == &stderrFile;
    if (!opened && !standard) {
        for (_IO_FILE** at = &registryHead; *at; at = &(*at)->next) {
            if (*at == f) {
                *at = f->next;
                break;
            }
        }
    }
    listLock.Release();
    if (opened) return f;
    const int failure = errno;
    if (!standard) {
        free(f);
    } else {
        errno = failure;
    }
    return nullptr;
}

CL int fclose(FILE* f) {
    if (!f) {
        errno = EINVAL;
        return EOF;
    }
    listLock.Acquire();
    f->lock.Acquire();
    int result = 0;
    int failure = 0;  // errno of the first failure
    if (f->flags & kClosed) {
        errno = EBADF;
        result = EOF;
    } else {
        if (f->flags & kWritable) {
            if (FlushLocked(f) < 0) {
                result = EOF;
                failure = errno;
            }
        }
        if (f->ownsBuffer) free(f->buffer);
        if (f->pushback) free(f->pushback);
        f->buffer = nullptr;
        f->bufferSize = 0;
        f->pushback = nullptr;
        f->pushbackLength = f->pushbackCapacity = 0;
        f->readPosition = f->readEnd = f->writeLength = 0;
        if (Call(__NR_close, f->fd) < 0 && result == 0) {
            result = EOF;
            failure = errno;
        }
        f->flags = kClosed;
        const bool standard = f == &stdinFile || f == &stdoutFile || f == &stderrFile;
        if (!standard) {  // heap streams leave the list; the standard ones stay, closed
            for (_IO_FILE** at = &registryHead; *at; at = &(*at)->next) {
                if (*at == f) {
                    *at = f->next;
                    break;
                }
            }
        }
        f->lock.Release();
        listLock.Release();
        if (!standard) {
            free(f);
            return result;
        }
        if (result) errno = failure;
        return result;
    }
    f->lock.Release();
    listLock.Release();
    return result;
}

// Reading.
CL int fgetc(FILE* f) {
    if (!f) {
        errno = EINVAL;
        return EOF;
    }
    Hold hold(f);
    if (f->flags & kClosed) {
        errno = EBADF;
        return EOF;
    }
    return GetLocked(f);
}
CL int getc(FILE* f) { return fgetc(f); }
CL int getchar(void) { return fgetc(stdin); }

CL size_t fread(void* p, size_t size, size_t count, FILE* f) {
    if (!size || !count) return 0;
    if (!f) {
        errno = EINVAL;
        return 0;
    }
    Hold hold(f);
    if (f->flags & kClosed) {
        errno = EBADF;
        return 0;
    }
    return ReadLocked(f, (char*)p, size * count) / size;
}

CL char* fgets(char* s, int n, FILE* f) {
    if (!s || n < 1) return nullptr;
    if (!f) {
        errno = EINVAL;
        return nullptr;
    }
    Hold hold(f);
    if (f->flags & kClosed) {
        errno = EBADF;
        return nullptr;
    }
    int i = 0;
    while (i < n - 1) {
        const int c = GetLocked(f);
        if (c < 0) break;
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    s[i] = '\0';
    // glibc's fgets with n == 1 reads nothing and still reports success, even at the end of file.
    if (n == 1) return s;
    return i ? s : nullptr;
}

CL long getdelim(char** line, size_t* capacity, int delimiter, FILE* f) {
    if (!line || !capacity || !f) {
        errno = EINVAL;
        return -1;
    }
    Hold hold(f);
    if (f->flags & kClosed) {
        errno = EBADF;
        return -1;
    }
    if (!*line || !*capacity) {
        *capacity = 120;
        *line = (char*)malloc(*capacity);
        if (!*line) return -1;
    }
    size_t length = 0;
    for (;;) {
        const int c = GetLocked(f);
        if (c < 0) break;
        if (length + 1 >= *capacity) {
            const size_t grown = *capacity * 2;
            char* bigger = (char*)realloc(*line, grown);
            if (!bigger) return -1;
            *line = bigger;
            *capacity = grown;
        }
        (*line)[length++] = (char)c;
        if (c == delimiter) break;
    }
    (*line)[length] = '\0';
    if (!length && (f->flags & (kEndOfFile | kError))) return -1;
    return (long)length;
}

CL long getline(char** line, size_t* capacity, FILE* f) { return getdelim(line, capacity, '\n', f); }
// glibc exports these internal names too, and GCC compiles a call to getline as one to __getdelim
// with a newline (getline has no move of its own), so the program's callers reach them by name.
CL long __getdelim(char** line, size_t* capacity, int delimiter, FILE* f) {
    return getdelim(line, capacity, delimiter, f);
}
CL long __getline(char** line, size_t* capacity, FILE* f) {
    return getdelim(line, capacity, '\n', f);
}

CL int ungetc(int c, FILE* f) {
    if (!f || c == EOF) return EOF;
    Hold hold(f);
    if ((f->flags & (kRead | kClosed)) != kRead) return EOF;
    if (f->pushbackLength == f->pushbackCapacity) {
        const size_t grown = f->pushbackCapacity ? f->pushbackCapacity * 2 : 8;
        char* bigger = (char*)realloc(f->pushback, grown);
        if (!bigger) return EOF;
        f->pushback = bigger;
        f->pushbackCapacity = grown;
    }
    f->pushback[f->pushbackLength++] = (char)(unsigned char)c;
    f->flags &= ~kEndOfFile;
    return (unsigned char)c;
}

// Writing.
CL int fputc(int c, FILE* f) {
    if (!f) {
        errno = EINVAL;
        return EOF;
    }
    Hold hold(f);
    if (f->flags & kClosed) {
        errno = EBADF;
        return EOF;
    }
    return PutLocked(f, c);
}
CL int putc(int c, FILE* f) { return fputc(c, f); }
CL int putchar(int c) { return fputc(c, stdout); }

CL size_t fwrite(const void* p, size_t size, size_t count, FILE* f) {
    if (!size || !count) return 0;
    if (!f) {
        errno = EINVAL;
        return 0;
    }
    Hold hold(f);
    if (f->flags & kClosed) {
        errno = EBADF;
        return 0;
    }
    return WriteLocked(f, (const char*)p, size * count) / size;
}

CL int fputs(const char* s, FILE* f) {
    if (!s || !f) {
        errno = EINVAL;
        return EOF;
    }
    Hold hold(f);
    if (f->flags & kClosed) {
        errno = EBADF;
        return EOF;
    }
    const size_t length = strlen(s);
    return WriteLocked(f, s, length) == length ? 1 : EOF;
}

CL int puts(const char* s) {
    if (!s) {
        errno = EINVAL;
        return EOF;
    }
    Hold hold(stdout);
    if (stdout->flags & kClosed) {
        errno = EBADF;
        return EOF;
    }
    const size_t length = strlen(s);
    if (WriteLocked(stdout, s, length) != length) return EOF;
    if (PutLocked(stdout, '\n') < 0) return EOF;
    return length >= 0x7fffffffu ? 0x7fffffff : (int)(length + 1);
}

// Position, flags and buffering.
CL int fflush(FILE* f) {
    if (!f) {  // every open stream: the list lock comes first, then one stream at a time
        int result = 0;
        listLock.Acquire();
        for (_IO_FILE* at = registryHead; at; at = at->next) {
            if (at->flags & kClosed) continue;
            at->lock.Acquire();
            if ((at->flags & kWritable) && FlushLocked(at) < 0) result = EOF;
            at->lock.Release();
        }
        listLock.Release();
        return result;
    }
    Hold hold(f);
    if (f->flags & kClosed) {
        errno = EBADF;
        return EOF;
    }
    if (f->flags & kWritable) return FlushLocked(f);
    if (f->readEnd > f->readPosition || f->pushbackLength) {
        SeekBackUnread(f);  // a pipe cannot seek; the buffer is dropped either way
        f->readPosition = f->readEnd = 0;
        f->pushbackLength = 0;
    }
    return 0;
}

CL int fseek(FILE* f, long offset, int whence) {
    if (!f) {
        errno = EINVAL;
        return -1;
    }
    Hold hold(f);
    if (f->flags & kClosed) {
        errno = EBADF;
        return -1;
    }
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) {
        errno = EINVAL;
        return -1;
    }
    if ((f->flags & kWritable) && FlushLocked(f) < 0) return -1;
    long long target = offset;
    int from = whence;
    if (whence == SEEK_CUR) {  // relative to the logical position, pushback included
        target = TellLocked(f);
        if (target < 0) return -1;
        target += offset;
        from = SEEK_SET;
    }
    f->readPosition = f->readEnd = 0;
    f->pushbackLength = 0;
    if (Call(__NR_lseek, f->fd, (long)target, from) < 0) return -1;
    f->flags &= ~kEndOfFile;  // glibc clears the end-of-file indicator, not the error one
    return 0;
}

CL int fseeko(FILE* f, long offset, int whence) { return fseek(f, offset, whence); }

CL long ftell(FILE* f) {
    if (!f) {
        errno = EINVAL;
        return -1;
    }
    Hold hold(f);
    if (f->flags & kClosed) {
        errno = EBADF;
        return -1;
    }
    return (long)TellLocked(f);
}

CL long ftello(FILE* f) { return ftell(f); }

CL void rewind(FILE* f) {
    if (fseek(f, 0, SEEK_SET) == 0) clearerr(f);
}

CL int feof(FILE* f) { return f && (f->flags & kEndOfFile); }
CL int ferror(FILE* f) { return f && (f->flags & kError); }
CL void clearerr(FILE* f) {
    if (f) f->flags &= ~(kEndOfFile | kError);
}
CL int fileno(FILE* f) {
    if (!f || (f->flags & kClosed)) {
        errno = EBADF;
        return -1;
    }
    return f->fd;
}

CL int setvbuf(FILE* f, char* buffer, int mode, size_t size) {
    if (!f || (mode != _IOFBF && mode != _IOLBF && mode != _IONBF)) {
        errno = EINVAL;
        return -1;
    }
    Hold hold(f);
    if (f->flags & kClosed) {
        errno = EBADF;
        return -1;
    }
    if (f->flags & kWritable) FlushLocked(f);
    if (f->readEnd > f->readPosition) f->readPosition = f->readEnd = 0;
    f->pushbackLength = 0;
    if (f->ownsBuffer) free(f->buffer);
    f->buffer = nullptr;
    f->bufferSize = 0;
    f->ownsBuffer = 0;
    f->flags &= ~(kUnbuffered | kLineBuffered);
    f->flags |= kModeKnown;
    if (mode == _IONBF) {
        f->flags |= kUnbuffered;
        return 0;
    }
    if (mode == _IOLBF) f->flags |= kLineBuffered;
    if (buffer) {
        if (!size) {
            errno = EINVAL;
            return -1;
        }
        f->buffer = buffer;
        f->bufferSize = size;
        f->ownsBuffer = 0;
    } else if (size) {
        f->bufferSize = size;  // allocated on first use
    }
    return 0;
}

CL void setbuf(FILE* f, char* buffer) {
    setvbuf(f, buffer, buffer ? _IOFBF : _IONBF, buffer ? cordlibc::kStreamBufferSize : 0);
}

CL int remove(const char* path) {
    long result = Call(__NR_unlink, path);
    if (result < 0 && errno == EISDIR) result = Call(__NR_rmdir, path);
    return (int)result;
}
