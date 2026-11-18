// The layout and helpers cordinal's own stdio shares between its files (stage 5a of replacing
// glibc): the FILE stream structure, its lock, and the buffered read and write cores. Only
// stdio.cpp, printf.cpp and scanf.cpp include this; none of them includes a glibc header that
// declares a name they define, so the FILE here is cordinal's own (glibc only declares an opaque
// struct __dirstream and a full struct _IO_FILE, and passing a pointer of the other kind between
// the two would be undefined).
#pragma once
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

namespace cordlibc {

// A futex lock over one stream: 0 free, 1 held, 2 held with waiters. The same shape as
// malloc.cpp's, with the same raw FUTEX_WAIT_PRIVATE/FUTEX_WAKE_PRIVATE system calls, so cordinal's
// stdio needs no pthreads before stage 8.
struct alignas(64) StreamLock {
    int state;
    void Acquire();
    bool TryAcquire();
    void Release();
};

// The process glibc's stdio buffers with; small enough for a stack frame, large enough that a
// title or a path is written in one go.
constexpr size_t kStreamBufferSize = 8192;

}  // namespace cordlibc

// The state of one open stream. `flags` holds kRead and the rest of the bits below; a stream's
// buffer is either the read window [readPosition, readEnd) or the pending write bytes
// [0, writeLength), never both. Pushback from ungetc lives apart from the buffer so a pushed-back
// byte never disturbs the window a read fill just made.
struct _IO_FILE {
    int fd;
    unsigned flags;
    int ownsBuffer;  // the buffer is cordinal's (setvbuf with a caller's buffer leaves this 0)
    size_t bufferSize;
    char* buffer;
    size_t readPosition;
    size_t readEnd;
    size_t writeLength;
    char* pushback;
    size_t pushbackLength;
    size_t pushbackCapacity;
    cordlibc::StreamLock lock;
    _IO_FILE* next;  // the process-wide list fflush(NULL) walks
};
typedef struct _IO_FILE FILE;

enum : unsigned {
    kRead = 1u << 0,
    kWritable = 1u << 1,
    kAppend = 1u << 2,
    kEndOfFile = 1u << 3,
    kError = 1u << 4,
    kUnbuffered = 1u << 5,
    kLineBuffered = 1u << 6,
    kClosed = 1u << 7,
    kModeKnown = 1u << 8,  // the tty check (or setvbuf) has decided the default buffering
};

// stdio.cpp's stream table, so printf and scanf can tell a closed stream from an open one without
// reaching into its fields.
namespace cordlibc {
bool StreamWantsRead(FILE* f);
bool StreamWantsWrite(FILE* f);
}  // namespace cordlibc
