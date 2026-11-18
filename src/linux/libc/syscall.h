// cordinal's own way into the Linux kernel on x86-64: one `syscall` instruction, no C library in
// between. The first piece of cordinal's replacement for glibc; kernel.cpp builds the POSIX calls
// on it. Only for x86-64, the one architecture the portable build targets.
#pragma once
#if !defined(__x86_64__) || !defined(__linux__)
#error "cordinal's syscall layer is written for x86-64 Linux"
#endif
#include <errno.h>
#include <asm/unistd.h>

namespace cordlibc {

// The kernel's register convention: number in rax, arguments in rdi, rsi, rdx, r10, r8, r9; it
// clobbers rcx and r11 and returns -errno on failure.
inline long Syscall(long n, long a = 0, long b = 0, long c = 0, long d = 0, long e = 0, long f = 0) {
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    long result;
    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return result;
}

// The POSIX convention on top: -1 with errno set, the way callers (and libstdc++) expect.
inline long Posix(long result) {
    if ((unsigned long)result > (unsigned long)-4096L) {
        errno = (int)-result;
        return -1;
    }
    return result;
}

template <class... Args> inline long Call(long n, Args... args) { return Posix(Syscall(n, (long)args...)); }

}  // namespace cordlibc
