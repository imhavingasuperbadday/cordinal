// cordinal's own POSIX kernel calls (the first stage of replacing glibc): read, write,
// files, sockets, memory maps, processes and randomness, each one system call (syscall.h) instead
// of glibc's wrapper. Defined under glibc's names with hidden visibility, so cordinal and the static
// libstdc++ both bind to these while the program exports nothing: glibc's own copies stay in place
// for glibc's internal use. Only calls that are a plain system call are here. Clocks stay with glibc
// for now (it reads them from the vDSO without entering the kernel), and so do fork, directories,
// getcwd and fchmodat, which glibc does more for. On x86-64 the kernel's stat, statfs, iovec and
// msghdr layouts are glibc's, so pointers to them pass straight through.
// No glibc header that declares these functions is included: the definitions are the declarations.
#include "syscall.h"
#include <stdarg.h>
#include <stddef.h>

extern "C" __attribute__((noreturn)) void abort(void) noexcept;

using cordlibc::Call;

namespace cordlibc {
struct TimeSpec {
    long sec, nsec;
};
}  // namespace cordlibc
using cordlibc::TimeSpec;

namespace {
constexpr int kAtFdCwd = -100;
constexpr int kAtSymlinkNoFollow = 0x100;
constexpr int kOCreat = 0100;
constexpr int kOTmpFile = 020000000 | 0200000;  // __O_TMPFILE | O_DIRECTORY
bool NeedsMode(int flags) { return (flags & kOCreat) || (flags & kOTmpFile) == kOTmpFile; }
}  // namespace

#define CL extern "C" __attribute__((visibility("hidden")))

// Reading and writing.
CL long read(int fd, void* buf, size_t n) { return Call(__NR_read, fd, buf, n); }
CL long write(int fd, const void* buf, size_t n) { return Call(__NR_write, fd, buf, n); }
CL long writev(int fd, const void* iov, int count) { return Call(__NR_writev, fd, iov, count); }
CL long pread(int fd, void* buf, size_t n, long offset) { return Call(__NR_pread64, fd, buf, n, offset); }
CL long pwrite(int fd, const void* buf, size_t n, long offset) { return Call(__NR_pwrite64, fd, buf, n, offset); }
CL long lseek(int fd, long offset, int whence) { return Call(__NR_lseek, fd, offset, whence); }
CL int close(int fd) { return (int)Call(__NR_close, fd); }
CL int fsync(int fd) { return (int)Call(__NR_fsync, fd); }
CL int ftruncate(int fd, long length) { return (int)Call(__NR_ftruncate, fd, length); }
CL int truncate(const char* path, long length) { return (int)Call(__NR_truncate, path, length); }
CL long sendfile(int out, int in, long* offset, size_t count) { return Call(__NR_sendfile, out, in, offset, count); }
// posix_fadvise reports its error as the return value and leaves errno alone.
CL int posix_fadvise(int fd, long offset, long length, int advice) {
    return (int)-cordlibc::Syscall(__NR_fadvise64, fd, offset, length, advice);
}

// Opening. The mode argument is there only when the file may be created.
CL int openat(int dirfd, const char* path, int flags, ...) {
    unsigned mode = 0;
    if (NeedsMode(flags)) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, unsigned);
        va_end(ap);
    }
    return (int)Call(__NR_openat, dirfd, path, flags, mode);
}
CL int open(const char* path, int flags, ...) {
    unsigned mode = 0;
    if (NeedsMode(flags)) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, unsigned);
        va_end(ap);
    }
    return (int)Call(__NR_openat, kAtFdCwd, path, flags, mode);
}
CL int fcntl(int fd, int command, ...) {
    va_list ap;
    va_start(ap, command);
    const long argument = va_arg(ap, long);
    va_end(ap);
    return (int)Call(__NR_fcntl, fd, command, argument);
}
CL int ioctl(int fd, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    const long argument = va_arg(ap, long);
    va_end(ap);
    return (int)Call(__NR_ioctl, fd, request, argument);
}
CL int dup(int fd) { return (int)Call(__NR_dup, fd); }
CL int dup2(int from, int to) { return (int)Call(__NR_dup2, from, to); }
CL int pipe(int fds[2]) { return (int)Call(__NR_pipe, fds); }
CL int pipe2(int fds[2], int flags) { return (int)Call(__NR_pipe2, fds, flags); }
CL int poll(void* fds, unsigned long count, int timeout) { return (int)Call(__NR_poll, fds, count, timeout); }
CL int select(int count, void* in, void* out, void* error, void* timeout) {
    return (int)Call(__NR_select, count, in, out, error, timeout);
}
CL int eventfd(unsigned value, int flags) { return (int)Call(__NR_eventfd2, value, flags); }
CL int memfd_create(const char* name, unsigned flags) { return (int)Call(__NR_memfd_create, name, flags); }
CL int inotify_init1(int flags) { return (int)Call(__NR_inotify_init1, flags); }
CL int inotify_add_watch(int fd, const char* path, unsigned mask) { return (int)Call(__NR_inotify_add_watch, fd, path, mask); }

// _FORTIFY_SOURCE's checked versions: a buffer smaller than the request, or a created file
// without a mode, ends the program as glibc's do.
CL long __read_chk(int fd, void* buf, size_t n, size_t size) {
    if (n > size) abort();
    return read(fd, buf, n);
}
CL int __openat_2(int dirfd, const char* path, int flags) {
    if (NeedsMode(flags)) abort();
    return openat(dirfd, path, flags);
}
CL int __open_2(const char* path, int flags) {
    if (NeedsMode(flags)) abort();
    return open(path, flags);
}

// Files and folders by name.
CL int stat(const char* path, void* st) { return (int)Call(__NR_newfstatat, kAtFdCwd, path, st, 0); }
CL int lstat(const char* path, void* st) { return (int)Call(__NR_newfstatat, kAtFdCwd, path, st, kAtSymlinkNoFollow); }
CL int fstat(int fd, void* st) { return (int)Call(__NR_fstat, fd, st); }
CL int statfs(const char* path, void* st) { return (int)Call(__NR_statfs, path, st); }
CL int access(const char* path, int mode) { return (int)Call(__NR_access, path, mode); }
CL int chdir(const char* path) { return (int)Call(__NR_chdir, path); }
CL int chmod(const char* path, unsigned mode) { return (int)Call(__NR_chmod, path, mode); }
CL int fchmod(int fd, unsigned mode) { return (int)Call(__NR_fchmod, fd, mode); }
CL int mkdir(const char* path, unsigned mode) { return (int)Call(__NR_mkdir, path, mode); }
CL int rmdir(const char* path) { return (int)Call(__NR_rmdir, path); }
CL int unlink(const char* path) { return (int)Call(__NR_unlink, path); }
CL int unlinkat(int dirfd, const char* path, int flags) { return (int)Call(__NR_unlinkat, dirfd, path, flags); }
CL int rename(const char* from, const char* to) { return (int)Call(__NR_rename, from, to); }
CL int link(const char* from, const char* to) { return (int)Call(__NR_link, from, to); }
CL int symlink(const char* target, const char* path) { return (int)Call(__NR_symlink, target, path); }
CL long readlink(const char* path, char* buf, size_t size) { return Call(__NR_readlink, path, buf, size); }
CL int utimensat(int dirfd, const char* path, const TimeSpec times[2], int flags) {
    return (int)Call(__NR_utimensat, dirfd, path, times, flags);
}
CL int futimens(int fd, const TimeSpec times[2]) { return (int)Call(__NR_utimensat, fd, nullptr, times, 0); }

// Sockets.
CL int socket(int domain, int type, int protocol) { return (int)Call(__NR_socket, domain, type, protocol); }
CL int bind(int fd, const void* address, unsigned length) { return (int)Call(__NR_bind, fd, address, length); }
CL int listen(int fd, int backlog) { return (int)Call(__NR_listen, fd, backlog); }
CL int accept(int fd, void* address, unsigned* length) { return (int)Call(__NR_accept, fd, address, length); }
CL int connect(int fd, const void* address, unsigned length) { return (int)Call(__NR_connect, fd, address, length); }
CL int shutdown(int fd, int how) { return (int)Call(__NR_shutdown, fd, how); }
CL long sendto(int fd, const void* buf, size_t n, int flags, const void* to, unsigned length) {
    return Call(__NR_sendto, fd, buf, n, flags, to, length);
}
CL long send(int fd, const void* buf, size_t n, int flags) { return Call(__NR_sendto, fd, buf, n, flags, nullptr, 0); }
CL long sendmsg(int fd, const void* message, int flags) { return Call(__NR_sendmsg, fd, message, flags); }
CL long recvfrom(int fd, void* buf, size_t n, int flags, void* from, unsigned* length) {
    return Call(__NR_recvfrom, fd, buf, n, flags, from, length);
}
CL long recv(int fd, void* buf, size_t n, int flags) { return Call(__NR_recvfrom, fd, buf, n, flags, nullptr, nullptr); }
CL long recvmsg(int fd, void* message, int flags) { return Call(__NR_recvmsg, fd, message, flags); }
CL int setsockopt(int fd, int level, int name, const void* value, unsigned length) {
    return (int)Call(__NR_setsockopt, fd, level, name, value, length);
}
CL int getsockopt(int fd, int level, int name, void* value, unsigned* length) {
    return (int)Call(__NR_getsockopt, fd, level, name, value, length);
}
CL int getsockname(int fd, void* address, unsigned* length) { return (int)Call(__NR_getsockname, fd, address, length); }

// Memory: maps and the X server's shared images.
CL void* mmap(void* address, size_t length, int protection, int flags, int fd, long offset) {
    return (void*)Call(__NR_mmap, address, length, protection, flags, fd, offset);
}
CL int munmap(void* address, size_t length) { return (int)Call(__NR_munmap, address, length); }
CL int shmget(int key, size_t size, int flags) { return (int)Call(__NR_shmget, key, size, flags); }
CL void* shmat(int id, const void* address, int flags) { return (void*)Call(__NR_shmat, id, address, flags); }
CL int shmdt(const void* address) { return (int)Call(__NR_shmdt, address); }

// The process.
CL int getpid(void) { return (int)cordlibc::Syscall(__NR_getpid); }
CL unsigned getuid(void) { return (unsigned)cordlibc::Syscall(__NR_getuid); }
CL unsigned geteuid(void) { return (unsigned)cordlibc::Syscall(__NR_geteuid); }
CL unsigned getgid(void) { return (unsigned)cordlibc::Syscall(__NR_getgid); }
CL int kill(int pid, int signal) { return (int)Call(__NR_kill, pid, signal); }
CL int killpg(int group, int signal) {
    if (group < 0) {
        errno = EINVAL;
        return -1;
    }
    return kill(-group, signal);
}
CL int setpriority(int which, unsigned who, int priority) { return (int)Call(__NR_setpriority, which, who, priority); }
CL int nanosleep(const TimeSpec* request, TimeSpec* remaining) { return (int)Call(__NR_nanosleep, request, remaining); }
CL int usleep(unsigned microseconds) {
    const TimeSpec request{(long)(microseconds / 1000000), (long)(microseconds % 1000000) * 1000};
    return nanosleep(&request, nullptr);
}
// _exit is a GCC builtin, already declared with default visibility, so it is hidden in assembly.
__asm__(".hidden _exit");
extern "C" __attribute__((noreturn)) void _exit(int status) {
    for (;;) cordlibc::Syscall(__NR_exit_group, status);
}
// The generic entry (cordinal asks for the thread id through it): up to six arguments.
CL long syscall(long number, ...) {
    va_list ap;
    va_start(ap, number);
    long a[6];
    for (long& argument : a) argument = va_arg(ap, long);
    va_end(ap);
    return Call(number, a[0], a[1], a[2], a[3], a[4], a[5]);
}

// Randomness from the kernel.
CL long getrandom(void* buf, size_t n, unsigned flags) { return Call(__NR_getrandom, buf, n, flags); }
CL int getentropy(void* buf, size_t n) {
    if (n > 256) {
        errno = EIO;
        return -1;
    }
    unsigned char* p = (unsigned char*)buf;
    while (n) {
        const long got = getrandom(p, n, 0);
        if (got < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += got;
        n -= (size_t)got;
    }
    return 0;
}
