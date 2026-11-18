// cordinal's files, directories, environment and processes (files.cpp) against glibc's, for
// checks.h: the directory stream over getdents64 (listing, telldir and seekdir, fdopendir), getcwd
// in and out of a removed folder, realpath with and without a caller's buffer (symlinks, trailing
// slashes, loops), mkstemp and mkdtemp, fchmodat with AT_SYMLINK_NOFOLLOW, statvfs every field
// glibc reports, the environment functions against glibc's and `environ`, fork and waitpid, the
// allocator's state in a forked child, posix_spawn with the file actions and attributes cordinal
// uses, popen and pclose, and execl. glibc's own functions come back through dlsym, so each answer
// is compared with the one glibc gives rather than with a guess.
#pragma once
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>
#include <vector>
#include <algorithm>

#ifdef CORDINAL_OWN_LIBC
// files.cpp writes these structures from its own copy of their layouts (it includes no glibc
// header that would declare the names it replaces), so they must be the headers' ones.
static_assert(sizeof(struct statvfs) == 112, "files.cpp fills glibc's struct statvfs");
static_assert(offsetof(struct statvfs, f_flag) == 72, "files.cpp writes f_flag where glibc's has it");
static_assert(offsetof(struct statvfs, f_namemax) == 80, "files.cpp writes f_namemax where glibc's has it");
static_assert(offsetof(struct statvfs, f_type) == 88, "files.cpp writes f_type where glibc's has it");
static_assert(sizeof(struct dirent) == 280 && offsetof(struct dirent, d_name) == 19,
              "a getdents64 record is glibc's struct dirent on x86-64");
static_assert(sizeof(posix_spawnattr_t) == 336, "files.cpp reads glibc's posix_spawnattr_t");
static_assert(sizeof(posix_spawn_file_actions_t) == 80, "files.cpp reads glibc's posix_spawn_file_actions_t");
#endif

namespace libc_checks {

// The platform's own function behind a name files.cpp replaced: the hidden definition in the
// program does not show up in the dynamic symbol table, so dlsym finds glibc's.
template <class Function> inline Function Glibc(const char* name) { return (Function)dlsym(RTLD_DEFAULT, name); }

// The names, inode numbers and types of a listing, in the order the stream returns them.
template <class Readdir, class Closedir>
inline std::vector<std::string> Listing(DIR* dir, Readdir readdir_, Closedir closedir_) {
    std::vector<std::string> entries;
    if (!dir) return entries;
    while (struct dirent* entry = readdir_(dir)) {
        char line[512];
        snprintf(line, sizeof line, "%llu %u %s", (unsigned long long)entry->d_ino,
                 (unsigned)entry->d_type, entry->d_name);
        entries.push_back(line);
    }
    closedir_(dir);
    return entries;
}

inline void CheckDirectories(const std::string& root) {
    char startingFolder[4096] = {};
    const bool haveStartingFolder = getcwd(startingFolder, sizeof startingFolder) != nullptr;
    const std::string folder = root + "/files";
    mkdir(folder.c_str(), 0755);
    const std::string alpha = folder + "/alpha", beta = folder + "/beta", sub = folder + "/sub",
                      inner = sub + "/inner";
    mkdir(sub.c_str(), 0755);
    for (const std::string& file : {alpha, beta, inner}) {
        const int fd = open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            write(fd, "x", 1);
            close(fd);
        }
    }
    symlink("alpha", (folder + "/alias").c_str());
    symlink("sub", (folder + "/dirlink").c_str());
    symlink("nowhere", (folder + "/dangling").c_str());
    symlink("loop", (folder + "/loop").c_str());

    // The listing, against glibc's.
    const std::vector<std::string> ours = Listing(opendir(folder.c_str()), [](DIR* d) { return readdir(d); },
                                                  [](DIR* d) { return closedir(d); });
    auto gOpendir = Glibc<DIR* (*)(const char*)>("opendir");
    auto gReaddir = Glibc<struct dirent* (*)(DIR*)>("readdir");
    auto gClosedir = Glibc<int (*)(DIR*)>("closedir");
    if (gOpendir && gReaddir && gClosedir) {
        const std::vector<std::string> theirs = Listing(gOpendir(folder.c_str()), gReaddir, gClosedir);
        Expect(ours.size() >= 9 && ours == theirs, "the folder's listing matches glibc's, inodes and types included");
    }

    // Positions, rewinddir, fdopendir and the descriptor behind the stream.
    DIR* dir = opendir(folder.c_str());
    std::vector<std::string> first;
    long afterTwo = 0;
    if (dir) {
        for (int i = 0; struct dirent* entry = readdir(dir); ++i) {
            first.push_back(entry->d_name);
            if (i == 1) afterTwo = telldir(dir);
            if (first.size() > 200) break;
        }
        rewinddir(dir);
        std::vector<std::string> again;
        while (struct dirent* entry = readdir(dir)) again.push_back(entry->d_name);
        seekdir(dir, afterTwo);
        struct dirent* next = readdir(dir);
        Expect(first.size() >= 9 && again == first && next && first.size() > 2 && next->d_name == first[2],
               "rewinddir rereads the stream and seekdir and telldir keep its position");
        struct stat st{};
        Expect(dirfd(dir) >= 0 && fstat(dirfd(dir), &st) == 0 && S_ISDIR(st.st_mode), "dirfd is the stream's holder");
        closedir(dir);
    } else {
        Expect(false, "opendir for the position checks");
    }
    {
        const int fd = open(folder.c_str(), O_RDONLY | O_DIRECTORY);
        DIR* taken = fdopendir(fd);
        int count = 0;
        if (taken)
            while (readdir(taken)) ++count;
        Expect(taken && count == (int)ours.size(), "fdopendir lists what opendir does");
        closedir(taken);
    }
    {
        const int fd = open(alpha.c_str(), O_RDONLY);
        errno = 0;
        Expect(fdopendir(fd) == nullptr && errno == ENOTDIR, "fdopendir of a file fails with ENOTDIR");
        close(fd);
    }
    errno = 0;
    Expect(opendir((folder + "/missing").c_str()) == nullptr && errno == ENOENT,
           "opendir of a missing folder fails with ENOENT");
    errno = 0;
    Expect(opendir(alpha.c_str()) == nullptr && errno == ENOTDIR, "opendir of a file fails with ENOTDIR");
    {
        const std::string empty = root + "/empty";
        mkdir(empty.c_str(), 0755);
        DIR* stream = opendir(empty.c_str());
        std::vector<std::string> names;
        if (stream)
            while (struct dirent* entry = readdir(stream)) names.push_back(entry->d_name);
        Expect(names.size() == 2 && names[0] == "." && names[1] == "..",
               "a folder holds only its own . and .. until something is put in it");
        errno = 1234;
        Expect(stream && !readdir(stream) && errno == 1234, "readdir at the end leaves errno alone");
        if (stream) closedir(stream);
        rmdir(empty.c_str());
    }

    // getcwd, allocated and not, and inside a folder that is removed.
    auto gGetcwd = Glibc<char* (*)(char*, size_t)>("getcwd");
    char oursCwd[4096] = {}, theirCwd[4096] = {};
    Expect(gGetcwd && getcwd(oursCwd, sizeof oursCwd) && gGetcwd(theirCwd, sizeof theirCwd) &&
               !strcmp(oursCwd, theirCwd),
           "getcwd is glibc's answer");
    {
        char tiny[2];
        errno = 0;
        char* mine = getcwd(tiny, sizeof tiny);
        const int mineError = errno;
        errno = 0;
        char* theirs = gGetcwd ? gGetcwd(tiny, sizeof tiny) : nullptr;
        const int theirError = errno;
        Expect(!mine && !theirs && mineError == ERANGE && theirError == ERANGE,
               "getcwd into a buffer that is too small fails with ERANGE");
    }
    {
        char* allocated = getcwd(nullptr, 0);
        Expect(allocated && !strcmp(allocated, oursCwd), "getcwd(nullptr, 0) allocates the answer");
        free(allocated);
    }
    const std::string gone = root + "/gone";
    mkdir(gone.c_str(), 0755);
    if (chdir(gone.c_str()) == 0) {
        rmdir(gone.c_str());
        errno = 0;
        char* mine = getcwd(oursCwd, sizeof oursCwd);
        const int mineError = errno;
        errno = 0;
        char* theirs = gGetcwd ? gGetcwd(theirCwd, sizeof theirCwd) : nullptr;
        const int theirError = errno;
        Expect(!mine && !theirs && mineError == ENOENT && theirError == ENOENT,
               "getcwd in a removed folder fails with ENOENT, as glibc's");
        Expect(chdir(folder.c_str()) == 0, "out of the removed folder");
    } else {
        Expect(false, "into the folder that gets removed");
    }

    // realpath: glibc's answers and errno for every shape of path, allocated and into a buffer.
    // The relative names below resolve inside the folder the checks made.
    Expect(chdir(folder.c_str()) == 0, "into the folder of the file checks");
    auto gRealpath = Glibc<char* (*)(const char*, char*)>("realpath");
    const char* paths[] = {"",   "/",      "//",      "///",           ".",     "..",
                           "./", "sub",    "sub/",    "sub//",         "sub/.", "sub/..",
                           "alpha/", "alias", "alias/", "dirlink/",   "dirlink/inner",
                           "dangling", "loop", "alpha/.", "alpha/..", "sub/../alpha",
                           "./alpha", "missing", "missing/x", "/tmp/../tmp"};
    bool matched = true;
    const char* mismatched = "";
    if (gRealpath) {
        for (const char* path : paths) {
            errno = 0;
            char* mine = realpath(path, nullptr);
            const int mineError = errno;
            errno = 0;
            char* theirs = gRealpath(path, nullptr);
            const int theirError = errno;
            const bool same = mine && theirs ? !strcmp(mine, theirs) : !mine && !theirs && mineError == theirError;
            if (!same && matched) {
                matched = false;
                mismatched = path;
                fprintf(stderr, "  realpath(\"%s\"): cordinal %s (%d), glibc %s (%d)\n", path,
                        mine ? mine : "(null)", mineError, theirs ? theirs : "(null)", theirError);
            }
            free(mine);
            free(theirs);
        }
    }
    if (!matched) fprintf(stderr, "  realpath differs at \"%s\"\n", mismatched);
    Expect(matched, "realpath matches glibc's answer and errno for every shape of path");
    {
        char mine[4096] = {}, theirs[4096] = {};
        char* result = realpath("sub/../alias", mine);
        Expect(result == mine && gRealpath && gRealpath("sub/../alias", theirs) && !strcmp(mine, theirs),
               "realpath fills the caller's buffer and returns it");
    }
    {
        char* allocated = realpath("dirlink", nullptr);
        char* theirs = gRealpath ? gRealpath("dirlink", nullptr) : nullptr;
        Expect(allocated && theirs && !strcmp(allocated, theirs), "realpath allocates when asked to");
        free(allocated);
        free(theirs);
    }

    // mkstemp and mkdtemp: a fresh name each time, the modes glibc gives, and EINVAL for a template
    // without six trailing X's.
    auto MakeName = [&](bool directory) {
        char path[512];
        snprintf(path, sizeof path, "%s/tmpXXXXXX", folder.c_str());
        if (directory) return mkdtemp(path) ? std::string(path) : std::string();
        const int fd = mkstemp(path);
        if (fd < 0) return std::string();
        struct stat st{};
        const bool regular = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0777) == 0600;
        if (!regular) Expect(false, "mkstemp makes a file only its owner may read");
        close(fd);
        return std::string(path);
    };
    bool distinct = true, removed = true;
    std::vector<std::string> created;
    for (int i = 0; i < 64; ++i) {
        const std::string name = MakeName(false);
        if (name.empty() || std::find(created.begin(), created.end(), name) != created.end()) distinct = false;
        if (name.empty() || unlink(name.c_str()) != 0) removed = false;
        if (!name.empty()) created.push_back(name);
    }
    Expect(distinct && removed && created.size() == 64, "mkstemp gives a fresh name every time");
    created.clear();
    for (int i = 0; i < 16; ++i) {
        const std::string name = MakeName(true);
        struct stat st{};
        const bool made = !name.empty() && stat(name.c_str(), &st) == 0 && S_ISDIR(st.st_mode) &&
                          (st.st_mode & 0777) == 0700;
        if (!made || std::find(created.begin(), created.end(), name) != created.end()) distinct = false;
        if (name.empty() || rmdir(name.c_str()) != 0) removed = false;
        if (!name.empty()) created.push_back(name);
    }
    Expect(distinct && removed && created.size() == 16, "mkdtemp makes a fresh folder each time, mode 0700");
    {
        char few[] = "/tmp/cordinal-libc-XXXXX";
        errno = 0;
        Expect(mkstemp(few) == -1 && errno == EINVAL, "mkstemp without six X's fails with EINVAL");
        char late[] = "/tmp/cordinal-libc-XXXXXXd";
        errno = 0;
        Expect(mkdtemp(late) == nullptr && errno == EINVAL, "mkdtemp with the X's not at the end fails with EINVAL");
    }

    // fchmodat, its AT_SYMLINK_NOFOLLOW emulation, and its errors against glibc's.
    auto gFchmodat = Glibc<int (*)(int, const char*, mode_t, int)>("fchmodat");
    struct stat st{};
    Expect(fchmodat(AT_FDCWD, "alpha", 0640, 0) == 0 && stat(alpha.c_str(), &st) == 0 &&
               (st.st_mode & 0777) == 0640,
           "fchmodat sets a file's mode");
    {
        errno = 0;
        const int mine = fchmodat(AT_FDCWD, "alias", 0640, AT_SYMLINK_NOFOLLOW);
        const int mineError = errno;
        errno = 0;
        const int theirs = gFchmodat ? gFchmodat(AT_FDCWD, "alias", 0640, AT_SYMLINK_NOFOLLOW) : mine;
        const int theirError = errno;
        Expect(mine == -1 && mineError == ENOTSUP && theirs == mine && theirError == mineError,
               "fchmodat refuses a symlink's mode with ENOTSUP, as glibc's");
    }
    Expect(fchmodat(AT_FDCWD, "alpha", 0600, AT_SYMLINK_NOFOLLOW) == 0 && stat(alpha.c_str(), &st) == 0 &&
               (st.st_mode & 0777) == 0600,
           "fchmodat with AT_SYMLINK_NOFOLLOW changes a file");
    Expect(fchmodat(AT_FDCWD, "sub", 0700, AT_SYMLINK_NOFOLLOW) == 0 && stat(sub.c_str(), &st) == 0 &&
               (st.st_mode & 0777) == 0700,
           "fchmodat with AT_SYMLINK_NOFOLLOW changes a folder");
    {
        const int folderFd = open(folder.c_str(), O_RDONLY | O_DIRECTORY);
        Expect(fchmodat(folderFd, "beta", 0640, 0) == 0 && stat(beta.c_str(), &st) == 0 &&
                   (st.st_mode & 0777) == 0640,
               "fchmodat under a folder descriptor");
        close(folderFd);
    }
    {
        errno = 0;
        const int mine = fchmodat(AT_FDCWD, "missing", 0600, 0);
        const int mineError = errno;
        errno = 0;
        const int theirs = gFchmodat ? gFchmodat(AT_FDCWD, "missing", 0600, 0) : mine;
        const int theirError = errno;
        Expect(mine == -1 && mineError == ENOENT && theirs == mine && theirError == mineError,
               "fchmodat of a missing name fails with ENOENT");
    }
    {
        errno = 0;
        const int mine = fchmodat(AT_FDCWD, "alpha", 0600, 0x4000);
        const int mineError = errno;
        errno = 0;
        const int theirs = gFchmodat ? gFchmodat(AT_FDCWD, "alpha", 0600, 0x4000) : mine;
        const int theirError = errno;
        Expect(mine == -1 && mineError == EINVAL && theirs == mine && theirError == mineError,
               "fchmodat with an unknown flag fails with EINVAL");
    }

    // statvfs: every field glibc reports, on a real filesystem and on the running system's.
    auto gStatvfs = Glibc<int (*)(const char*, struct statvfs*)>("statvfs");
    auto gFstatvfs = Glibc<int (*)(int, struct statvfs*)>("fstatvfs");
    const char* places[] = {folder.c_str(), "/", "/proc", "/dev/shm"};
    bool fields = true;
    std::string wrongPlace;
    for (const char* place : places) {
        struct statvfs mine{}, theirs{};
        if (!gStatvfs || gStatvfs(place, &theirs) != 0) continue;
        if (statvfs(place, &mine) != 0 || mine.f_bsize != theirs.f_bsize || mine.f_frsize != theirs.f_frsize ||
            mine.f_flag != theirs.f_flag || mine.f_namemax != theirs.f_namemax ||
            mine.f_type != theirs.f_type || mine.f_fsid != theirs.f_fsid || mine.f_frsize == 0 ||
            mine.f_namemax == 0) {
            fields = false;
            wrongPlace = place;
        }
    }
    if (!fields) fprintf(stderr, "  statvfs differs at %s\n", wrongPlace.c_str());
    Expect(fields, "statvfs's block size, fragment size, flags, name length and type are glibc's");
    {
        struct statvfs mine{}, twice{};
        const int fd = open(folder.c_str(), O_RDONLY | O_DIRECTORY);
        const bool read = fd >= 0 && fstatvfs(fd, &mine) == 0 && statvfs(folder.c_str(), &twice) == 0 &&
                          gFstatvfs && gFstatvfs(fd, &twice) == 0 && mine.f_frsize == twice.f_frsize &&
                          mine.f_flag == twice.f_flag && mine.f_type == twice.f_type &&
                          mine.f_namemax == twice.f_namemax && mine.f_blocks > 0;
        if (fd >= 0) close(fd);
        Expect(read, "fstatvfs and statvfs agree, as glibc's do");
    }
    {
        struct statvfs scratch{};
        errno = 0;
        const int mine = statvfs((root + "/missing").c_str(), &scratch);
        const int mineError = errno;
        errno = 0;
        const int theirs = gStatvfs ? gStatvfs((root + "/missing").c_str(), &scratch) : mine;
        const int theirError = errno;
        Expect(mine == -1 && mineError == ENOENT && theirs == mine && theirError == mineError,
               "statvfs of a missing name fails with ENOENT");
        errno = 0;
        Expect(fstatvfs(-1, &scratch) == -1 && errno == EBADF, "fstatvfs of a bad descriptor fails with EBADF");
    }

    // Clean up, so the check's own folder can go. The process goes back to where it started, so
    // later checks (and the allocator's) have a working folder.
    if (haveStartingFolder) chdir(startingFolder);
    unlink(alpha.c_str());
    unlink(beta.c_str());
    unlink(inner.c_str());
    rmdir(sub.c_str());
    unlink((folder + "/alias").c_str());
    unlink((folder + "/dirlink").c_str());
    unlink((folder + "/dangling").c_str());
    unlink((folder + "/loop").c_str());
    rmdir(folder.c_str());
}

inline void CheckEnvironment() {
    Expect(InProgram((const void*)&getenv) && InProgram((const void*)&setenv) &&
               InProgram((const void*)&unsetenv) && InProgram((const void*)&putenv) &&
               InProgram((const void*)&secure_getenv),
           "getenv, setenv, unsetenv, putenv and secure_getenv are cordinal's own");
    auto gGetenv = Glibc<char* (*)(const char*)>("getenv");
    auto gPutenv = Glibc<int (*)(char*)>("putenv");
    const char* value = getenv("CORDINAL_LIBC_CHECK");
    Expect(!value, "the check's variable starts unset");
    Expect(setenv("CORDINAL_LIBC_CHECK", "first", 1) == 0 && getenv("CORDINAL_LIBC_CHECK") &&
               !strcmp(getenv("CORDINAL_LIBC_CHECK"), "first"),
           "setenv adds a variable and getenv finds it");
    Expect(gGetenv && gGetenv("CORDINAL_LIBC_CHECK") && !strcmp(gGetenv("CORDINAL_LIBC_CHECK"), "first"),
           "glibc's getenv reads the same environment: one array serves the process");
    Expect(setenv("CORDINAL_LIBC_CHECK", "second", 0) == 0 && !strcmp(getenv("CORDINAL_LIBC_CHECK"), "first"),
           "setenv without overwrite leaves the value");
    Expect(setenv("CORDINAL_LIBC_CHECK", "second", 1) == 0 && !strcmp(getenv("CORDINAL_LIBC_CHECK"), "second"),
           "setenv with overwrite replaces it");
    {
        bool listed = false;
        for (char** entry = environ; entry && *entry; ++entry)
            if (!strcmp(*entry, "CORDINAL_LIBC_CHECK=second")) listed = true;
        Expect(listed, "the variable is in `environ`");
    }
    // putenv keeps the caller's string, so a later change to it is a change to the environment.
    static char put[] = "CORDINAL_LIBC_PUT=one";
    Expect(putenv(put) == 0 && getenv("CORDINAL_LIBC_PUT") && !strcmp(getenv("CORDINAL_LIBC_PUT"), "one"),
           "putenv adds a variable");
    memcpy(put + strlen("CORDINAL_LIBC_PUT="), "two", 4);
    Expect(getenv("CORDINAL_LIBC_PUT") && !strcmp(getenv("CORDINAL_LIBC_PUT"), "two"),
           "putenv keeps the caller's string");
    {
        char name[] = "CORDINAL_LIBC_PUT";
        Expect(putenv(name) == 0 && !getenv("CORDINAL_LIBC_PUT"), "putenv without '=' removes the variable");
    }
    if (gPutenv && gGetenv) {  // glibc's answer for the same command, so the rule is the same one
        setenv("CORDINAL_LIBC_PUT2", "x", 1);
        char name[] = "CORDINAL_LIBC_PUT2";
        Expect(gPutenv(name) == 0 && !gGetenv("CORDINAL_LIBC_PUT2"), "glibc's putenv removes a name without '=' too");
    }
    Expect(unsetenv("CORDINAL_LIBC_CHECK") == 0 && !getenv("CORDINAL_LIBC_CHECK") &&
               (!gGetenv || !gGetenv("CORDINAL_LIBC_CHECK")),
           "unsetenv removes the variable for both libraries");
    Expect(unsetenv("CORDINAL_LIBC_NEVER_SET") == 0, "unsetenv of a name that is not set succeeds");
    Expect(!getenv("CORDINAL_LIBC_NEVER_SET"), "getenv of a name that is not set is null");
    errno = 0;
    Expect(setenv("", "x", 1) == -1 && errno == EINVAL, "setenv of an empty name fails with EINVAL");
    errno = 0;
    Expect(setenv("NAME=VALUE", "x", 1) == -1 && errno == EINVAL, "setenv of a name with '=' fails with EINVAL");
    errno = 0;
    Expect(unsetenv("NAME=VALUE") == -1 && errno == EINVAL, "unsetenv of a name with '=' fails with EINVAL");
    Expect(secure_getenv("PATH") == getenv("PATH"), "secure_getenv is getenv when not started set-user-ID");
    // A grown array: many variables at once, read back, then removed again.
    bool grown = true;
    for (int i = 0; i < 40; ++i) {
        char name[32];
        snprintf(name, sizeof name, "CORDINAL_LIBC_GROW_%d", i);
        if (setenv(name, "grown value", 1) != 0) grown = false;
    }
    for (int i = 0; i < 40; ++i) {
        char name[32];
        snprintf(name, sizeof name, "CORDINAL_LIBC_GROW_%d", i);
        const char* found = getenv(name);
        if (!found || strcmp(found, "grown value")) grown = false;
    }
    Expect(grown && getenv("PATH") && (!gGetenv || gGetenv("PATH")), "the environment array grows and keeps its entries");
    for (int i = 0; i < 40; ++i) {
        char name[32];
        snprintf(name, sizeof name, "CORDINAL_LIBC_GROW_%d", i);
        unsetenv(name);
    }
    Expect(!getenv("CORDINAL_LIBC_GROW_0") && getenv("PATH"), "and shrinks again without losing the rest");
}

inline void CheckProcesses(const std::string& root) {
    Expect(InProgram((const void*)&fork) && InProgram((const void*)&waitpid) &&
               InProgram((const void*)&posix_spawn) && InProgram((const void*)&posix_spawnp) &&
               InProgram((const void*)&popen) && InProgram((const void*)&pclose) &&
               InProgram((const void*)&execl),
           "fork, waitpid, the spawn family, popen and execl are cordinal's own");

    // fork: the child's copy of the heap is intact and it can allocate (its allocator locks were
    // reset), and nothing it writes reaches the parent.
    char* block = (char*)malloc(4096);
    memset(block, 'z', 4096);
    volatile char marker = 'p';
    pid_t child = fork();
    Expect(child >= 0, "fork");
    if (child == 0) {
        bool intact = true;
        for (int i = 0; i < 4096; ++i)
            if (block[i] != 'z') intact = false;
        for (int i = 0; i < 1000 && intact; ++i) {
            void* p = malloc((size_t)(i % 2000) + 1);
            if (!p) {
                intact = false;
                break;
            }
            memset(p, 1, 8);
            free(p);
        }
        for (int i = 0; i < 4096; ++i)
            if (block[i] != 'z') intact = false;
        block[0] = 'c';
        marker = 'c';
        _exit(intact ? 0 : 1);
    }
    int status = 0;
    Expect(child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
           "a forked child allocates and frees with the parent's memory intact");
    Expect(block[0] == 'z' && marker == 'p', "the child's writes stay in its own copy");
    errno = 0;
    Expect(waitpid(child, &status, 0) == -1 && errno == ECHILD, "the child is reaped once");
    free(block);

    // execl's failure, in a child of its own.
    child = fork();
    if (child == 0) {
        execl("/nonexistent/cordinal", "cordinal", (char*)nullptr);
        _exit(errno == ENOENT ? 0 : 1);
    }
    Expect(child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
           "execl of a missing program fails with ENOENT");

    // execlp: PATH search, a name with a slash, and the ENOENT of a name nothing answers.
    Expect(InProgram((const void*)&execlp), "execlp is cordinal's own");
    child = fork();
    if (child == 0) {
        execlp("sh", "sh", "-c", "exit 3", (char*)nullptr);
        _exit(1);
    }
    Expect(child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 3,
           "execlp finds the shell through PATH");
    child = fork();
    if (child == 0) {
        execlp("/bin/sh", "sh", "-c", "exit 5", (char*)nullptr);
        _exit(1);
    }
    Expect(child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 5,
           "execlp runs a name with a slash");
    int errnoPipe[2];
    Expect(pipe(errnoPipe) == 0, "a pipe for execlp's failure");
    child = fork();
    if (child == 0) {
        close(errnoPipe[0]);
        execlp("no-such-command-cordial", "no-such-command-cordial", (char*)nullptr);
        int error = errno;
        write(errnoPipe[1], &error, sizeof error);
        _exit(0);
    }
    close(errnoPipe[1]);
    {
        int error = 0;
        Expect(waitpid(child, &status, 0) == child, "the execlp child is reaped");
        Expect(read(errnoPipe[0], &error, sizeof error) == (ssize_t)sizeof error && WIFEXITED(status) &&
                   WEXITSTATUS(status) == 0 && error == ENOENT,
               "execlp of a name nothing answers fails with ENOENT");
    }
    close(errnoPipe[0]);


    char* shell[] = {(char*)"sh", (char*)"-c", (char*)"printf spawned; exit 3", nullptr};
    char* nothing[] = {nullptr};
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    pid_t spawned = 0;

    // dup2 and open actions: the shell's output comes out of a pipe and its input from /dev/null.
    {
        int fds[2];
        Expect(pipe(fds) == 0, "a pipe for the spawned child");
        Expect(posix_spawn_file_actions_init(&actions) == 0, "posix_spawn_file_actions_init");
        Expect(posix_spawn_file_actions_adddup2(&actions, fds[1], 1) == 0, "adddup2");
        Expect(posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0) == 0, "addopen");
        const int result = posix_spawn(&spawned, "/bin/sh", &actions, nullptr, shell, environ);
        posix_spawn_file_actions_destroy(&actions);
        close(fds[1]);
        char text[32] = {};
        const ssize_t got = read(fds[0], text, sizeof text - 1);
        close(fds[0]);
        Expect(result == 0 && spawned > 0, "posix_spawn starts the shell");
        Expect(got == 7 && !strcmp(text, "spawned"), "the child's stdout is the pipe it was given");
        Expect(waitpid(spawned, &status, 0) == spawned && WIFEXITED(status) && WEXITSTATUS(status) == 3,
               "the spawned child's status is the command's");
    }
    // addopen with O_CREAT: the file appears with the mode asked for.
    {
        const std::string output = root + "/spawned.txt";
        char* writing[] = {(char*)"sh", (char*)"-c", (char*)"printf filed", nullptr};
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, 1, output.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0640);
        const int result = posix_spawn(&spawned, "/bin/sh", &actions, nullptr, writing, environ);
        posix_spawn_file_actions_destroy(&actions);
        Expect(result == 0 && waitpid(spawned, &status, 0) == spawned && WIFEXITED(status) &&
                   WEXITSTATUS(status) == 0,
               "posix_spawn with an opened stdout finishes");
        struct stat st{};
        char text[32] = {};
        const int fd = open(output.c_str(), O_RDONLY);
        const ssize_t got = fd >= 0 ? read(fd, text, sizeof text - 1) : 0;
        if (fd >= 0) close(fd);
        // The mode goes through the parent's umask, as it does for any open.
        const bool made = stat(output.c_str(), &st) == 0;
        const mode_t mask = umask(0);
        umask(mask);
        const bool mode = made && (st.st_mode & 0777) == (0640 & ~mask);
        if (!(got == 5 && !strcmp(text, "filed") && mode))
            fprintf(stderr, "  addopen: %zd bytes \"%s\", mode %o with umask %o\n", got, text,
                    made ? (unsigned)(st.st_mode & 0777) : 0u, (unsigned)mask);
        Expect(got == 5 && !strcmp(text, "filed") && mode,
               "addopen creates the file with the mode asked for");
        unlink(output.c_str());
    }
    // addchdir_np: the child runs in the folder the action names.
    {
        char* here[] = {(char*)"sh", (char*)"-c", (char*)"pwd", nullptr};
        char canonical[4096] = {};
        const std::string expected = realpath(root.c_str(), canonical) ? canonical : root;
        int fds[2];
        Expect(pipe(fds) == 0, "a pipe for the child's working folder");
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, fds[1], 1);
        posix_spawn_file_actions_addchdir_np(&actions, root.c_str());
        const int result = posix_spawn(&spawned, "/bin/sh", &actions, nullptr, here, environ);
        posix_spawn_file_actions_destroy(&actions);
        close(fds[1]);
        char text[4096] = {};
        const ssize_t got = read(fds[0], text, sizeof text - 1);
        close(fds[0]);
        if (got > 0 && text[got - 1] == '\n') text[got - 1] = 0;
        Expect(result == 0 && waitpid(spawned, &status, 0) == spawned && !strcmp(text, expected.c_str()),
               "addchdir_np puts the child in the folder the action names");
    }
    // addclose: a descriptor the parent holds open is gone in the child, while one it keeps open
    // without close-on-exec still works.
    {
        const std::string witness = root + "/witness";
        const int inherited = open(witness.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        Expect(inherited > 2, "a descriptor for the child to close");
        char script[128];
        snprintf(script, sizeof script, "printf x >&%d; exit $?", inherited);
        char* writing[] = {(char*)"sh", (char*)"-c", script, nullptr};
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addclose(&actions, inherited);
        // The shell reports the closed descriptor on its stderr, which would only be noise here.
        posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
        const int closed = posix_spawn(&spawned, "/bin/sh", &actions, nullptr, writing, environ);
        posix_spawn_file_actions_destroy(&actions);
        int closedStatus = 0;
        if (closed == 0) waitpid(spawned, &closedStatus, 0);
        const int kept = posix_spawn(&spawned, "/bin/sh", nullptr, nullptr, writing, environ);
        int keptStatus = 0;
        if (kept == 0) waitpid(spawned, &keptStatus, 0);
        Expect(closed == 0 && WIFEXITED(closedStatus) && WEXITSTATUS(closedStatus) != 0,
               "addclose takes the descriptor away from the child");
        Expect(kept == 0 && WIFEXITED(keptStatus) && WEXITSTATUS(keptStatus) == 0,
               "without addclose the child inherits the descriptor");
        close(inherited);
        unlink(witness.c_str());
    }
    // SETPGROUP: the child leads a group of its own, which nothing else would give it.
    {
        char* sleeper[] = {(char*)"sh", (char*)"-c", (char*)"sleep 5", nullptr};
        posix_spawnattr_init(&attributes);
        Expect(posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP) == 0, "posix_spawnattr_setflags");
        Expect(posix_spawnattr_setpgroup(&attributes, 0) == 0, "posix_spawnattr_setpgroup");
        const int result = posix_spawn(&spawned, "/bin/sh", nullptr, &attributes, sleeper, environ);
        posix_spawnattr_destroy(&attributes);
        Expect(result == 0 && spawned > 0, "posix_spawn with its own process group");
        Expect(kill(-spawned, 0) == 0, "the child leads a process group of its own");
        kill(spawned, SIGTERM);
        Expect(waitpid(spawned, &status, 0) == spawned && WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM,
               "and ends when it is signalled");
    }
    // SETSIGMASK and SETSIGDEF, through glibc's setters, so the structures' layouts are checked too:
    // the same command kills itself with SIGUSR1 and survives only when the mask blocks it.
    char* signalled[] = {(char*)"sh", (char*)"-c", (char*)"kill -USR1 $$; exit 0", nullptr};
    auto gSetMask = Glibc<int (*)(posix_spawnattr_t*, const sigset_t*)>("posix_spawnattr_setsigmask");
    auto gSetDefault = Glibc<int (*)(posix_spawnattr_t*, const sigset_t*)>("posix_spawnattr_setsigdefault");
    {
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGUSR1);
        posix_spawnattr_init(&attributes);
        Expect(gSetMask && gSetMask(&attributes, &mask) == 0, "glibc's setter fills the attribute structure");
        posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK);
        const int result = posix_spawn(&spawned, "/bin/sh", nullptr, &attributes, signalled, environ);
        posix_spawnattr_destroy(&attributes);
        Expect(result == 0 && waitpid(spawned, &status, 0) == spawned && WIFEXITED(status) &&
                   WEXITSTATUS(status) == 0,
               "SETSIGMASK blocks the signal in the child");
    }
    {
        struct sigaction ignore{}, previous{};
        ignore.sa_handler = SIG_IGN;
        sigemptyset(&ignore.sa_mask);
        Expect(sigaction(SIGUSR1, &ignore, &previous) == 0, "the parent ignores SIGUSR1");
        sigset_t defaults;
        sigemptyset(&defaults);
        sigaddset(&defaults, SIGUSR1);
        posix_spawnattr_init(&attributes);
        Expect(gSetDefault && gSetDefault(&attributes, &defaults) == 0, "glibc's sigdefault setter fills it");
        posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGDEF);
        const int result = posix_spawn(&spawned, "/bin/sh", nullptr, &attributes, signalled, environ);
        posix_spawnattr_destroy(&attributes);
        const bool died =
            result == 0 && waitpid(spawned, &status, 0) == spawned && WIFSIGNALED(status) && WTERMSIG(status) == SIGUSR1;
        Expect(died, "SETSIGDEF restores the default disposition, so the signal ends the child");
        const int inherited = posix_spawn(&spawned, "/bin/sh", nullptr, nullptr, signalled, environ);
        Expect(inherited == 0 && waitpid(spawned, &status, 0) == spawned && WIFEXITED(status) &&
                   WEXITSTATUS(status) == 0,
               "without SETSIGDEF the child inherits the ignored signal");
        sigaction(SIGUSR1, &previous, nullptr);
    }
    // Errors come back as numbers, as glibc's do.
    auto gSpawn = Glibc<int (*)(pid_t*, const char*, const posix_spawn_file_actions_t*,
                                const posix_spawnattr_t*, char* const[], char* const[])>("posix_spawn");
    errno = 0;
    const int missing = posix_spawn(&spawned, "/nonexistent", nullptr, nullptr, nothing, environ);
    const int missingError = errno;
    Expect(missing == ENOENT && missingError == ENOENT, "posix_spawn of a missing program returns ENOENT");
    Expect(!gSpawn || gSpawn(&spawned, "/nonexistent", nullptr, nullptr, nothing, environ) == missing,
           "and glibc's return value is the same");
    Expect(posix_spawn(&spawned, root.c_str(), nullptr, nullptr, nothing, environ) == EACCES,
           "posix_spawn of a folder returns EACCES");
    {
        const std::string plain = root + "/plain";
        FILE* file = fopen(plain.c_str(), "w");
        if (file) {
            fputs("echo plain\n", file);
            fclose(file);
        }
        chmod(plain.c_str(), 0644);
        Expect(posix_spawn(&spawned, plain.c_str(), nullptr, nullptr, nothing, environ) == EACCES,
               "posix_spawn of a file without an exec bit returns EACCES");
        chmod(plain.c_str(), 0755);
        Expect(posix_spawn(&spawned, plain.c_str(), nullptr, nullptr, nothing, environ) == ENOEXEC,
               "posix_spawn of a script without a shebang returns ENOEXEC, as glibc's does");
        unlink(plain.c_str());
    }
    {
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, 1, (root + "/missing/out").c_str(), O_WRONLY | O_CREAT, 0640);
        const int failed = posix_spawn(&spawned, "/bin/true", &actions, nullptr, nothing, environ);
        posix_spawn_file_actions_destroy(&actions);
        Expect(failed == ENOENT, "a failing file action comes back as its errno");
        errno = 0;
        Expect(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD, "a child that could not start is reaped");
    }
    {
        posix_spawn_file_actions_init(&actions);
        Expect(posix_spawn_file_actions_addclose(&actions, -1) == EBADF, "addclose of a negative descriptor is EBADF");
        Expect(posix_spawn_file_actions_adddup2(&actions, -1, 1) == EBADF,
               "adddup2 of a negative descriptor is EBADF");
        Expect(posix_spawn_file_actions_destroy(&actions) == 0, "posix_spawn_file_actions_destroy");
    }
    // posix_spawnp's PATH search.
    {
        char* seven[] = {(char*)"sh", (char*)"-c", (char*)"exit 7", nullptr};
        spawned = 0;
        Expect(posix_spawnp(&spawned, "sh", nullptr, nullptr, seven, environ) == 0 && spawned > 0,
               "posix_spawnp finds the command in PATH");
        Expect(waitpid(spawned, &status, 0) == spawned && WIFEXITED(status) && WEXITSTATUS(status) == 7,
               "the searched child's status is the command's");
        Expect(posix_spawnp(&spawned, "cordinal-no-such-command", nullptr, nullptr, nothing, environ) == ENOENT,
               "posix_spawnp of a missing command returns ENOENT");
    }

    // popen and pclose: read, write, the command's status, and popen's mode rules.
    {
        FILE* reading = popen("printf hello", "r");
        char text[32] = {};
        const size_t got = reading ? fread(text, 1, sizeof text - 1, reading) : 0;
        Expect(reading && got == 5 && !strcmp(text, "hello"), "popen reads the command's output");
        Expect(reading && pclose(reading) == 0, "pclose reports the command's success");
    }
    {
        const std::string written = root + "/popen.txt";
        const std::string command = "cat > '" + written + "'";
        FILE* writing = popen(command.c_str(), "w");
        Expect(writing && fputs("written", writing) >= 0, "popen writes to the command's input");
        Expect(writing && pclose(writing) == 0, "the writer's command ends cleanly");
        char text[32] = {};
        const int fd = open(written.c_str(), O_RDONLY);
        const ssize_t got = fd >= 0 ? read(fd, text, sizeof text - 1) : 0;
        if (fd >= 0) close(fd);
        Expect(got == 7 && !strcmp(text, "written"), "the command received what was written");
        unlink(written.c_str());
    }
    {
        FILE* failing = popen("exit 3", "r");
        auto gPopen = Glibc<FILE* (*)(const char*, const char*)>("popen");
        auto gPclose = Glibc<int (*)(FILE*)>("pclose");
        const int mine = failing ? pclose(failing) : -1;
        int theirs = -1;
        if (gPopen && gPclose) {
            FILE* stream = gPopen("exit 3", "r");
            if (stream) theirs = gPclose(stream);
        }
        Expect(mine == (3 << 8) && theirs == mine, "pclose returns the command's wait status, as glibc's");
    }    {
        errno = 0;
        FILE* bad = popen("true", "q");
        const int badError = errno;
        Expect(!bad && badError == EINVAL, "popen with an invalid mode fails with EINVAL");
        FILE* closedOnExec = popen("printf e", "re");
        char text[8] = {};
        const size_t got = closedOnExec ? fread(text, 1, sizeof text - 1, closedOnExec) : 0;
        Expect(closedOnExec && got == 1 && !strcmp(text, "e") && pclose(closedOnExec) == 0,
               "popen's 'e' mode reads the command's output too");
        const std::string plainPath = root + "/popen-plain";
        FILE* made = fopen(plainPath.c_str(), "w");
        if (made) {
            fputs("x", made);
            fclose(made);
        }
        FILE* plain = fopen(plainPath.c_str(), "r");
        const int plainFd = plain ? fileno(plain) : -1;
        const int plainResult = plain ? pclose(plain) : -1;
        // POSIX leaves this undefined. cordinal closes the stream and returns fclose's result, as
        // glibc 2.44 does; 2.41 closed it and answered -1, so only the closing and the result are
        // checked here.
        errno = 0;
        const bool closed = plainFd >= 0 && fcntl(plainFd, F_GETFD) == -1 && errno == EBADF;
        Expect(plain && plainResult == 0 && closed, "pclose closes a stream popen did not open");
        unlink(plainPath.c_str());
    }
    // A later popen's child does not inherit the descriptor of a stream still open in the parent,
    // as POSIX asks and glibc's popen arranges.
    {
        FILE* earlier = popen("true", "r");
        const int earlierFd = earlier ? fileno(earlier) : -1;
        char script[256];
        snprintf(script, sizeof script,
                 "if [ -e /proc/self/fd/%d ]; then printf inherited; else printf closed; fi", earlierFd);
        FILE* later = popen(script, "r");
        char text[32] = {};
        const size_t got = later ? fread(text, 1, sizeof text - 1, later) : 0;
        Expect(earlier && earlierFd > 2 && got == 6 && !strcmp(text, "closed"),
               "a later popen's child does not inherit an earlier stream's descriptor");
        Expect(pclose(earlier) == 0 && pclose(later) == 0, "both streams close cleanly");
    }
}

}  // namespace libc_checks
