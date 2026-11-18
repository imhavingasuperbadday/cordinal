// Lets the program run on much older glibc than the one it was built on: the release is built on
// Debian 13 (glibc 2.41) and runs on glibc 2.31 and newer (Debian 11, Ubuntu 20.04 and 22.04).
// glibc is the one library cordinal still takes from the system, so the newer parts of it that the
// program and the static libstdc++/libgcc would ask for are replaced here, in two ways:
//  - Bound to an older version that every supported glibc has. --wrap (CMakeLists.txt) sends every
//    reference to __wrap_<name> here, which calls <name>@<old version> (.symver). glibc 2.34 gave new
//    versions to __libc_start_main. (stat, new in 2.33, is a system call in libc/kernel.cpp; strtol
//    and its wide forms are cordinal's own since stage 5b in libc/numbers.cpp, which also defines the
//    __isoc23_ names glibc 2.38's headers route them to; libm's newer names such as fmod, hypotf and
//    exp are cordinal's own since stage 7 in libc/math.cpp, which defines them itself so no older
//    version has to be bound; and the pthread functions glibc 2.34 moved into libc are cordinal's own
//    since stage 8 in libc/threads.cpp, which replaces them completely rather than binding older
//    versions.)
//  - Written here. _dl_find_object (2.35, the unwinder in libgcc_eh) has no older version. It is
//    reached through --wrap as well, so it stays private: under glibc's own name the program would
//    export it and take the place of glibc's copy on newer systems. arc4random (2.36,
//    std::random_device) used to be written here too; it is cordinal's own since the last-imports
//    stage, in libc/misc.cpp. __libc_single_threaded (2.32) used to be written here too; it is
//    cordinal's own since stage 8, in libc/threads.cpp, where a thread creation clears it and the
//    last thread ending sets it again.
// Before 2.34 the pthread functions live in libpthread.so.0, so the program also names that library
// (an empty stub on newer glibc). The CI checks the newest version the program asks for.
// Only for x86-64 glibc, where those old versions exist under these names.
#include <cstddef>  // for __GLIBC__
#if defined(__x86_64__) && defined(__GLIBC__) && defined(CORDINAL_GLIBC_COMPAT)
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <ctime>
#include <dlfcn.h>
#include <link.h>
#include <unistd.h>

#define OLD(name, version) __asm__(".symver cordinal_old_" #name "," #name "@" version)
OLD(__libc_start_main, "GLIBC_2.2.5");
#undef OLD

extern "C" {
// Program start. Since 2.34, crt1 passes no init function and glibc runs the program's constructors
// itself; older glibc only runs the ones it is given, so an init function that does what glibc's
// __libc_csu_init used to is always passed. glibc 2.34 and newer call it instead of their own.
// Preinit arrays and destructors are run by the dynamic loader on every version.
typedef int (*MainFn)(int, char**, char**);
int cordinal_old___libc_start_main(MainFn, int, char**, void (*)(int, char**, char**), void (*)(void),
                                   void (*)(void), void*);
void _init(void);
typedef void (*InitFn)(int, char**, char**);
extern InitFn __init_array_start[] __attribute__((visibility("hidden")));
extern InitFn __init_array_end[] __attribute__((visibility("hidden")));
static void cordinal_run_init(int argc, char** argv, char** envp) {
    _init();
    for (InitFn* f = __init_array_start; f != __init_array_end; ++f) (*f)(argc, argv, envp);
}
int __wrap___libc_start_main(MainFn main, int argc, char** argv, void (*)(void), void (*)(void),
                             void (*rtld_fini)(void), void* stack_end) {
    return cordinal_old___libc_start_main(main, argc, argv, cordinal_run_init, nullptr, rtld_fini, stack_end);
}

// std::random_device's arc4random is cordinal's own since the last-imports stage, in
// libc/misc.cpp, so this file no longer writes it and CMakeLists.txt no longer wraps it.

#if __GLIBC_PREREQ(2, 35)
// Finds the loaded object holding an address, for libgcc's unwinder (exceptions), as it did with
// dl_iterate_phdr before glibc had _dl_find_object. x86-64 has no dlfo_eh_dbase to fill.
namespace {
struct FindObject {
    uintptr_t pc;
    dl_find_object* result;
    bool found;
};
int FindObjectIn(dl_phdr_info* info, size_t, void* data) {
    FindObject* find = static_cast<FindObject*>(data);
    uintptr_t low = UINTPTR_MAX, high = 0;
    const void* ehFrame = nullptr;
    bool inside = false;
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr)& ph = info->dlpi_phdr[i];
        const uintptr_t start = info->dlpi_addr + ph.p_vaddr;
        if (ph.p_type == PT_LOAD) {
            const uintptr_t end = start + ph.p_memsz;
            if (find->pc >= start && find->pc < end) inside = true;
            if (start < low) low = start;
            if (end > high) high = end;
        } else if (ph.p_type == PT_GNU_EH_FRAME) {
            ehFrame = reinterpret_cast<const void*>(start);
        }
    }
    if (!inside) return 0;
    find->result->dlfo_flags = 0;
    find->result->dlfo_map_start = reinterpret_cast<void*>(low);
    find->result->dlfo_map_end = reinterpret_cast<void*>(high);
    find->result->dlfo_link_map = nullptr;
    find->result->dlfo_eh_frame = const_cast<void*>(ehFrame);
    find->found = true;
    return 1;
}
}  // namespace
int __wrap__dl_find_object(void* pc, dl_find_object* result) {
    FindObject find{reinterpret_cast<uintptr_t>(pc), result, false};
    dl_iterate_phdr(FindObjectIn, &find);
    return find.found ? 0 : -1;
}
#endif
}
#endif
