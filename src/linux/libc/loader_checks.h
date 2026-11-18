// Checks of the static build's two loader-shaped features: the self-relocation that gives the image
// ASLR (start.cpp) and the loader that gives it dlopen (loader.cpp). The first re-runs this program
// twice and compares the image base each run reports from /proc/self/maps; the second loads the
// smoke-test's own plug.so, which imports only names from the program's exported set and carries no C
// library of its own, and puts the loader's promises to work. Only the static build has either.
#pragma once
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <spawn.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#if defined(CORDINAL_STATIC)

namespace libc_checks {

namespace loader_detail {

// The image's base as /proc/self/maps reports it: the first mapping is the image's own first segment,
// at file offset 0, so its start is the base and the offset is subtracted to be sure.
inline uintptr_t MappedImageBase() {
    FILE* file = fopen("/proc/self/maps", "r");
    if (!file) return 0;
    char line[256] = {};
    uintptr_t start = 0, offset = 0;
    if (fgets(line, sizeof line, file)) sscanf(line, "%lx-%*x %*s %lx", &start, &offset);
    fclose(file);
    return start - offset;
}

struct ChildBase {
    bool printed = false;
    int exitCode = -1;
    uintptr_t base = 0;
};

// One re-run of this program with the flag start.cpp looks for: the child writes its base and stops
// before any constructor or test, so two runs cost a process each and nothing else.
inline ChildBase RunChild(const char* self) {
    ChildBase child;
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) return child;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[1], 1);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    posix_spawn_file_actions_addclose(&actions, fds[1]);
    char* argv[] = {(char*)self, nullptr};
    char* environment[] = {(char*)"CORDINAL_ASLR_CHILD=1", nullptr};
    pid_t pid = 0;
    const int spawned = posix_spawn(&pid, self, &actions, nullptr, argv, environment);
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);
    char buffer[64] = {};
    long got = 0;
    if (spawned == 0) {
        while (got < (long)sizeof buffer - 1) {
            const long read = ::read(fds[0], buffer + got, sizeof buffer - 1 - got);
            if (read <= 0) break;
            got += read;
        }
        int status = 0;
        waitpid(pid, &status, 0);
        child.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    close(fds[0]);
    if (sscanf(buffer, "base 0x%lx", (unsigned long*)&child.base) == 1) child.printed = true;
    return child;
}

}  // namespace loader_detail

// Two runs of the same program must land at different addresses, which is what the self-relocation in
// start.cpp makes possible: without it the image would sit at its link-time address every time.
inline void CheckStaticPie() {
    using loader_detail::ChildBase;
    using loader_detail::MappedImageBase;
    using loader_detail::RunChild;
    const char* self = "/proc/self/exe";
    const ChildBase first = RunChild(self);
    const ChildBase second = RunChild(self);
    Expect(first.printed && first.exitCode == 0, "the first re-run reported its image base");
    Expect(second.printed && second.exitCode == 0, "the second re-run reported its image base");
    Expect(first.base != 0 && second.base != 0 && first.base != second.base,
           "the image is loaded at a different address on each run (static-PIE ASLR)");
    Expect((first.base & 0xfff) == 0 && (second.base & 0xfff) == 0, "both bases are page aligned");
    const uintptr_t own = MappedImageBase();
    Expect(own != 0, "this process reads its own image base from /proc/self/maps");
    if (own) {
        // Nothing else in the image may be at a link-time address: the globals live at the base the
        // relocation loop computed, so an address inside one must be at least the page-aligned base.
        const uintptr_t global = (uintptr_t)&libc_checks::failures;
        Expect(global >= own && global < own + (32u << 20), "a global variable's address follows the image base");
        printf("     static-pie: bases 0x%lx and 0x%lx (this run 0x%lx, a global at 0x%lx)\n",
               (unsigned long)first.base, (unsigned long)second.base, (unsigned long)own, (unsigned long)global);
    }
}

namespace loader_detail {

struct TlsRun {
    int (*bump)();
    int values[2];
};

inline void* TlsThread(void* argument) {
    TlsRun* run = (TlsRun*)argument;
    run->values[0] = run->bump();
    run->values[1] = run->bump();
    return nullptr;
}

// The names a loaded object answers to, for the dl_iterate_phdr check.
struct PhdrSearch {
    const char* suffix;
    int matches;
    int modules;
};

inline int CountPhdr(struct dl_phdr_info* info, size_t, void* data) {
    PhdrSearch* search = (PhdrSearch*)data;
    ++search->modules;
    if (info->dlpi_name && strstr(info->dlpi_name, search->suffix)) ++search->matches;
    return 0;
}

}  // namespace loader_detail

// The loader's promises, against the fixture smoke-test.sh compiles next to the other test files. It
// imports strlen, snprintf, malloc and free from the program's exported set and has no C library of
// its own, so a pass says the exported set, the relocations, the constructors and the thread-local
// storage of a loaded object all work.
inline void CheckLoader(const std::string& dir) {
    using namespace loader_detail;
    // The program's own exported set, before anything is loaded.
    void* self = dlopen(nullptr, RTLD_NOW);
    Expect(self != nullptr, "dlopen(NULL) returns the program itself");
    size_t (*length)(const char*) = (size_t(*)(const char*))dlsym(self, "strlen");
    Expect(length && length("cordinal") == 8, "dlsym finds strlen in the program's exported set");
    Expect(dlsym(self, "no_such_function") == nullptr && dlerror() != nullptr,
           "dlsym of a missing name fails with a message");
    Expect(dlerror() == nullptr, "dlerror returns the message once and then nothing");

    const std::string missing = dir + "/absent.so";
    Expect(dlopen(missing.c_str(), RTLD_NOW) == nullptr, "dlopen of a missing file fails");
    const char* message = dlerror();
    Expect(message && strstr(message, "cannot open"), "the failure names the file it could not open");

    // A file that is not an ELF object at all.
    const std::string corrupt = dir + "/corrupt.so";
    FILE* broken = fopen(corrupt.c_str(), "wb");
    if (broken) {
        fputs("this is not an ELF object, it is a sentence\n", broken);
        fclose(broken);
        Expect(dlopen(corrupt.c_str(), RTLD_NOW) == nullptr, "dlopen of a corrupt file fails");
        message = dlerror();
        Expect(message && strstr(message, "not an ELF object"), "the failure says the file is not an ELF object");
        unlink(corrupt.c_str());
    }

    const std::string fixture = dir + "/plug.so";
    if (access(fixture.c_str(), R_OK) != 0) {
        printf("     loader: no plug.so in %s, so the loaded-object checks are skipped "
               "(linux/smoke-test.sh builds it)\n",
               dir.c_str());
        return;
    }
    const std::string reference = dir + "/plugref.so";

    // Loading, calling, and the thread-local storage a loaded object carries.
    void* plug = dlopen(fixture.c_str(), RTLD_NOW | RTLD_LOCAL);
    Expect(plug != nullptr, "dlopen loads plug.so");
    if (!plug) {
        printf("     loader: %s\n", dlerror());
        return;
    }
    int (*sum)(int, int) = (int(*)(int, int))dlsym(plug, "plug_sum");
    Expect(sum && sum(2, 3) == 5, "dlsym finds plug_sum and the call reaches the loaded object");
    char* (*greet)(const char*) = (char* (*)(const char*))dlsym(plug, "plug_greet");
    char* text = greet ? greet("cordinal") : nullptr;
    Expect(text && !strcmp(text, "hello cordinal (8)"), "the object's snprintf, strlen and malloc calls reach the program");
    free(text);
    int (*bump)() = (int(*)())dlsym(plug, "plug_tls_bump");
    Expect(bump != nullptr, "dlsym finds the object's thread-local function");
    if (bump) {
        // The object's __thread counter starts at zero on every thread and stays its own.
        TlsRun mainRun{bump, {0, 0}};
        TlsRun threadRun{bump, {0, 0}};
        mainRun.values[0] = bump();
        mainRun.values[1] = bump();
        pthread_t thread;
        const int created = pthread_create(&thread, nullptr, TlsThread, &threadRun);
        Expect(created == 0, "a cordinal thread starts for the loaded object's thread-local storage");
        if (created == 0) pthread_join(thread, nullptr);
        Expect(threadRun.values[0] == 1 && threadRun.values[1] == 2,
               "the loaded object's __thread counter starts at zero on a new thread");
        Expect(mainRun.values[0] == 1 && mainRun.values[1] == 2,
               "the loaded object's __thread counter is the first thread's own copy");
    }

    // dladdr on a loaded symbol.
    Dl_info info{};
    Expect(dladdr((const void*)sum, &info) == 1 && info.dli_sname && !strcmp(info.dli_sname, "plug_sum"),
           "dladdr names a loaded symbol");
    Expect(info.dli_fname && strstr(info.dli_fname, "plug.so") && info.dli_fbase,
           "dladdr names the file the symbol came from");
    void* symbolEntry = nullptr;
    Dl_info info1{};
    Expect(dladdr1((const void*)sum, &info1, &symbolEntry, RTLD_DL_SYMENT) == 1 && symbolEntry != nullptr,
           "dladdr1 reports the loaded symbol's entry");
    Expect(dladdr((const void*)&CheckLoader, &info) == 1 && info.dli_fbase,
           "dladdr reports this program for one of its own addresses");

    // The loaded module is in dl_iterate_phdr's walk.
    PhdrSearch search{"plug.so", 0, 0};
    dl_iterate_phdr(CountPhdr, &search);
    Expect(search.matches >= 1, "dl_iterate_phdr lists the loaded object");
    Expect(search.modules >= 2, "dl_iterate_phdr lists the program and the loaded object");

    // Two dlopens of one file are one object with two references.
    void* again = dlopen(fixture.c_str(), RTLD_NOW | RTLD_LOCAL);
    Expect(again == plug, "dlopen of the same file returns the object already loaded");
    Expect(dlclose(again) == 0 && dlsym(plug, "plug_sum") != nullptr,
           "the first dlclose keeps the object loaded");
    Expect(dlclose(plug) == 0, "the second dlclose unloads it");
    Expect(dlsym(plug, "plug_sum") == nullptr && dlerror() != nullptr,
           "a symbol lookup in the unloaded object fails with a message");
    search.matches = 0;
    search.modules = 0;
    dl_iterate_phdr(CountPhdr, &search);
    Expect(search.matches == 0, "dl_iterate_phdr forgets the unloaded object");

    // RTLD_LOCAL keeps an object out of a later load's scope; RTLD_GLOBAL puts it in.
    void* local = dlopen(fixture.c_str(), RTLD_NOW | RTLD_LOCAL);
    Expect(local != nullptr, "plug.so loads again, this time locally");
    if (access(reference.c_str(), R_OK) == 0) {
        Expect(dlopen(reference.c_str(), RTLD_NOW) == nullptr,
               "an object that needs a locally loaded one fails to load");
        message = dlerror();
        Expect(message && strstr(message, "undefined symbol") && strstr(message, "plug_sum"),
               "the failure names the symbol that was not in scope");
        if (local) dlclose(local);
        void* global = dlopen(fixture.c_str(), RTLD_NOW | RTLD_GLOBAL);
        Expect(global != nullptr, "plug.so loads globally");
        Expect(dlsym(RTLD_DEFAULT, "plug_sum") != nullptr, "a globally loaded object's symbol is in the default scope");
        void* user = dlopen(reference.c_str(), RTLD_NOW);
        Expect(user != nullptr, "an object that needs a globally loaded one loads");
        if (user) {
            int (*ref)(int) = (int(*)(int))dlsym(user, "plug_ref");
            Expect(ref && ref(5) == 15, "the reference object calls through to the global one");
            Expect(dlclose(user) == 0, "the reference object unloads");
        }
        if (global) Expect(dlclose(global) == 0, "the global object unloads");
    } else {
        if (local) dlclose(local);
    }
    printf("     loader: plug.so loaded, called, unloaded; its __thread counter is its own on two threads;\n"
           "             RTLD_LOCAL kept it out of a second object's scope and RTLD_GLOBAL let it in\n");
}

}  // namespace libc_checks

#endif  // CORDINAL_STATIC
