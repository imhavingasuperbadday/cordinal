// Checks of cordinal's own C library (src/linux/libc), for `cordinal-core-test libc DIR`: each group
// of kernel calls gives glibc's results and errno, the memory and string functions match glibc's
// (string_checks.h), the streams and formatters match glibc's byte for byte (stdio_checks.h), and the
// program reaches cordinal's copies, not glibc's. Every other core check runs on the same functions;
// these pin down the edges they rarely reach.
#pragma once
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cstdio>
#include <string>

namespace libc_checks {

inline int failures = 0;
inline void Expect(bool ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL %s (errno %d: %s)\n", what, errno, strerror(errno));
        ++failures;
    }
}
// A call that must fail with this errno.
inline void ExpectError(long result, int error, const char* what) {
    Expect(result == -1 && errno == error, what);
}

// Whether `function` lives in this program rather than in a shared library such as libc.so.6.
inline bool InProgram(const void* function) {
#if defined(CORDINAL_STATIC)
    // A static program has no shared library at all, so every function in it is its own.
    (void)function;
    return true;
#else
    Dl_info info{};
    Dl_info self{};
    return dladdr(function, &info) && dladdr((const void*)&InProgram, &self) && info.dli_fname && self.dli_fname &&
           !strcmp(info.dli_fname, self.dli_fname);
#endif
}

}  // namespace libc_checks
#include "string_checks.h"
#include "malloc_checks.h"
#include "time_checks.h"
#include "files_checks.h"
#include "stdio_checks.h"
#include "numbers_checks.h"
#include "locale_checks.h"
#include "iconv_checks.h"
#include "net_checks.h"
#include "math_checks.h"
#include "threads_checks.h"
#include "misc_checks.h"
#if defined(CORDINAL_STATIC)
#include "loader_checks.h"
#endif
namespace libc_checks {

inline int Check(const std::string& dir) {
#ifndef CORDINAL_OWN_LIBC
    printf("skip: this build uses glibc's kernel calls (x86-64 portable builds use cordinal's)\n");
    return 0;
#else
    failures = 0;
    Expect(InProgram((const void*)&read) && InProgram((const void*)&socket) && InProgram((const void*)&stat),
           "read, socket and stat are cordinal's own");
    mkdir(dir.c_str(), 0700);
    const std::string file = dir + "/data", other = dir + "/moved", folder = dir + "/folder", linkPath = dir + "/link";

    // Files: write, read back at offsets, and the sizes stat and fstat agree on.
    int fd = open(file.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0640);
    Expect(fd >= 0, "open creates a file");
    Expect(write(fd, "hello world", 11) == 11, "write");
    const iovec parts[2] = {{(void*)"!", 1}, {(void*)"?", 1}};
    Expect(writev(fd, parts, 2) == 2, "writev");
    char buf[32] = {};
    Expect(pread(fd, buf, 5, 6) == 5 && !memcmp(buf, "world", 5), "pread at an offset");
    Expect(pwrite(fd, "W", 1, 6) == 1, "pwrite");
    Expect(lseek(fd, 0, SEEK_SET) == 0 && read(fd, buf, sizeof buf) == 13 && !memcmp(buf, "hello World!?", 13),
           "lseek and read");
    struct stat a{}, b{};
    Expect(fstat(fd, &a) == 0 && stat(file.c_str(), &b) == 0 && a.st_size == 13 && b.st_size == 13 &&
               a.st_ino == b.st_ino && (b.st_mode & 0777) == 0640 && S_ISREG(b.st_mode),
           "stat and fstat agree on size, inode and mode");
    Expect(ftruncate(fd, 4) == 0 && fstat(fd, &a) == 0 && a.st_size == 4, "ftruncate");
    Expect((fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0, "fcntl reads O_CLOEXEC back");
    Expect(fsync(fd) == 0 && posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL) == 0, "fsync and posix_fadvise");
    Expect(posix_fadvise(-1, 0, 0, POSIX_FADV_SEQUENTIAL) == EBADF, "posix_fadvise returns its error");
    const int copy = dup(fd);
    Expect(copy > fd && close(copy) == 0, "dup and close");
    Expect(close(fd) == 0, "close");
    ExpectError(close(fd), EBADF, "closing twice fails with EBADF");
    ExpectError(read(-1, buf, 1), EBADF, "read on a bad descriptor");
    ExpectError(open((dir + "/missing").c_str(), O_RDONLY), ENOENT, "open of a missing file fails with ENOENT");
    Expect(truncate(file.c_str(), 2) == 0 && stat(file.c_str(), &b) == 0 && b.st_size == 2, "truncate");

    // Names: rename, links, folders.
    Expect(rename(file.c_str(), other.c_str()) == 0 && access(other.c_str(), R_OK) == 0, "rename and access");
    ExpectError(access(file.c_str(), F_OK), ENOENT, "the old name is gone");
    Expect(symlink("moved", linkPath.c_str()) == 0, "symlink");
    Expect(lstat(linkPath.c_str(), &a) == 0 && S_ISLNK(a.st_mode) && stat(linkPath.c_str(), &b) == 0 && S_ISREG(b.st_mode),
           "lstat sees the link, stat its target");
    const long n = readlink(linkPath.c_str(), buf, sizeof buf);
    Expect(n == 5 && !memcmp(buf, "moved", 5), "readlink");
    Expect(chmod(other.c_str(), 0600) == 0 && stat(other.c_str(), &b) == 0 && (b.st_mode & 0777) == 0600, "chmod");
    Expect(mkdir(folder.c_str(), 0700) == 0, "mkdir");
    ExpectError(mkdir(folder.c_str(), 0700), EEXIST, "mkdir twice fails with EEXIST");
    Expect(link(other.c_str(), (folder + "/hard").c_str()) == 0, "link");
    ExpectError(rmdir(folder.c_str()), ENOTEMPTY, "rmdir of a full folder fails with ENOTEMPTY");
    Expect(unlinkat(AT_FDCWD, (folder + "/hard").c_str(), 0) == 0 && rmdir(folder.c_str()) == 0, "unlinkat and rmdir");
    const timespec times[2] = {{1000000000, 0}, {1000000000, 0}};
    Expect(utimensat(AT_FDCWD, other.c_str(), times, 0) == 0 && stat(other.c_str(), &b) == 0 &&
               b.st_mtim.tv_sec == 1000000000,
           "utimensat");
    Expect(unlink(linkPath.c_str()) == 0 && unlink(other.c_str()) == 0, "unlink");

    // Pipes, poll, eventfd and memfd.
    int fds[2];
    Expect(pipe2(fds, O_CLOEXEC) == 0, "pipe2");
    pollfd p{fds[0], POLLIN, 0};
    Expect(poll(&p, 1, 0) == 0, "poll on an empty pipe times out");
    Expect(write(fds[1], "x", 1) == 1 && poll(&p, 1, 1000) == 1 && (p.revents & POLLIN), "poll sees data");
    Expect(read(fds[0], buf, 1) == 1 && buf[0] == 'x', "read from the pipe");
    close(fds[0]);
    close(fds[1]);
    const int event = eventfd(0, EFD_CLOEXEC);
    uint64_t count = 3;
    Expect(event >= 0 && write(event, &count, 8) == 8 && read(event, &count, 8) == 8 && count == 3, "eventfd");
    close(event);
    const int memory = memfd_create("cordinal-check", MFD_CLOEXEC);
    Expect(memory >= 0 && ftruncate(memory, 4096) == 0, "memfd_create");
    void* map = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, memory, 0);
    Expect(map != MAP_FAILED && (memcpy(map, "mapped", 6), pread(memory, buf, 6, 0) == 6) && !memcmp(buf, "mapped", 6),
           "mmap of a memfd shares its pages");
    Expect(munmap(map, 4096) == 0, "munmap");
    close(memory);
    Expect(mmap(nullptr, 4096, PROT_READ, MAP_SHARED, -1, 0) == MAP_FAILED && errno == EBADF,
           "mmap reports failure as MAP_FAILED");

    // Sockets: a loopback TCP connection both ways.
    const int server = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t length = sizeof address;
    const int yes = 1;
    Expect(server >= 0 && setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes) == 0 &&
               bind(server, (sockaddr*)&address, sizeof address) == 0 && listen(server, 1) == 0 &&
               getsockname(server, (sockaddr*)&address, &length) == 0 && address.sin_port != 0,
           "socket, bind, listen and getsockname");
    const int client = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    Expect(connect(client, (sockaddr*)&address, sizeof address) == 0, "connect");
    const int accepted = accept(server, nullptr, nullptr);
    Expect(accepted >= 0, "accept");
    Expect(send(client, "ping", 4, MSG_NOSIGNAL) == 4 && recv(accepted, buf, 4, MSG_WAITALL) == 4 && !memcmp(buf, "ping", 4),
           "send and recv");
    int option = 0;
    socklen_t optionLength = sizeof option;
    Expect(getsockopt(client, SOL_SOCKET, SO_TYPE, &option, &optionLength) == 0 && option == SOCK_STREAM, "getsockopt");
    Expect(shutdown(client, SHUT_WR) == 0 && recv(accepted, buf, 1, 0) == 0, "shutdown ends the stream");
    close(accepted);
    close(client);
    close(server);
    ExpectError(connect(-1, (sockaddr*)&address, sizeof address), EBADF, "connect on a bad descriptor");

    // The process and randomness.
    Expect(getpid() > 0 && syscall(SYS_gettid) > 0 && getuid() == (uid_t)syscall(SYS_getuid), "getpid, gettid and getuid");
    Expect(kill(getpid(), 0) == 0, "kill with signal 0");
    ExpectError(killpg(-1, 0), EINVAL, "killpg of a negative group");
    ExpectError(syscall(-1), ENOSYS, "an unknown system call fails with ENOSYS");
    unsigned char random[64] = {}, zero[64] = {};
    Expect(getentropy(random, sizeof random) == 0 && memcmp(random, zero, sizeof random), "getentropy");
    unsigned char large[257];
    ExpectError(getentropy(large, sizeof large), EIO, "getentropy over 256 bytes fails with EIO");
    Expect(getrandom(random, 16, 0) == 16, "getrandom");
    Expect(usleep(1000) == 0, "usleep");

#if defined(CORDINAL_STATIC)
    // A static program has no shared library, so there is no second copy of a function to compare
    // with. It does have dlopen, dlsym and dladdr of its own (loader.cpp), and an image that relocates
    // itself, which loader_checks.h puts to work. The kernel calls above are checked on their own;
    // each group below that compares cordinal's answers with glibc's says so instead. The threads and
    // the allocator are cordinal's own in both builds, so their groups run here too.
    CheckStaticPie();
    CheckLoader(dir);
    rmdir(dir.c_str());
    printf("skip: the groups that compare with glibc's own functions (text, clocks, directories and\n"
           "      processes, streams, numbers, math, locale, iconv, network names and the last\n"
           "      imports) need a shared glibc, which a static program does not have\n");
    CheckThreads();
#ifndef CORDINAL_GLIBC_MALLOC
    CheckMalloc();
#endif
    if (failures) return 1;
    printf("ok: cordinal's kernel calls (files, names, pipes, maps, sockets, process, randomness)\n");
    printf("ok: cordinal's threads, mutexes, condition variables, rwlocks, keys and thread-local storage\n");
    printf("ok: cordinal's allocator (threads, alignment, calloc, trim, fork)\n");
    printf("ok: the static image relocates itself (ASLR) and loads shared objects through its own loader\n");
    return 0;
#else
    CheckDirectories(dir);
    CheckEnvironment();
    CheckProcesses(dir);
    CheckStdio(dir);
    CheckNetwork(dir);
    rmdir(dir.c_str());
    CheckStrings();
    CheckTime();
    CheckNumbers();
    CheckMath();
    CheckLocale();
    CheckIconv();
    CheckThreads();
    CheckMisc();
#ifndef CORDINAL_GLIBC_MALLOC
    CheckMalloc();
#endif
    if (failures) return 1;
    printf("ok: cordinal's kernel calls (files, names, pipes, maps, sockets, process, randomness)\n");
    printf("ok: cordinal's memory and string functions match glibc's\n");
    printf("ok: cordinal's clocks, calendar and local time match glibc's\n");
    printf("ok: cordinal's directories, paths, environment and processes match glibc's\n");
    printf("ok: cordinal's streams, printf and scanf match glibc's\n");
    printf("ok: cordinal's number parsing matches glibc's\n");
    printf("ok: cordinal's math functions match glibc's (values, errno, ulps, the decoding paths)\n");
    printf("ok: cordinal's locale, character classes, multibyte conversions and strftime match glibc's\n");
    printf("ok: cordinal's iconv matches glibc's for the encodings it knows\n");
    printf("ok: cordinal's last imports (strerror, sysconf, getpwuid, nice, shmctl, fnmatch, qsort_r, signal, "
           "gettext, randomness, binary128 text) match glibc's\n");
    printf("ok: cordinal's network names match glibc's (address text, hosts, services, interfaces, DNS)\n");
    printf("ok: cordinal's threads, mutexes, condition variables, rwlocks, keys and thread-local storage\n");
    printf("ok: cordinal's allocator (threads, alignment, calloc, glibc's own allocations, trim, fork)\n");
    return 0;
#endif
#endif
}

}  // namespace libc_checks
