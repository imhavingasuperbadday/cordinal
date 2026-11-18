// The static program's entry point (stage 8's `_start`, first used by stage 9's static link, now
// static-PIE): the kernel's first instructions in the process, the image's own relocations applied
// before any global can be read, the initial stack taken apart, the first thread's thread-local
// storage installed, and the program's constructors and main called.
//
// The kernel enters with %rsp at a word holding argc, then argv's pointers, a null, envp's pointers, a
// null, and the auxiliary vector's tag/value pairs. `_start` keeps that stack pointer, aligns the stack
// for the C++ call, and hands it to Boot, which never returns.
//
// A static-PIE image is linked at address 0 and loaded wherever the kernel's ASLR puts it, so the first
// thing Boot does is find the load bias and apply the image's relocations: every link-time address in
// the image (each pointer in .data, each GOT entry) gets its runtime one, and the resolvers the linker
// left for the string functions are called. Until that loop has run, only position-independent code may
// execute: no global variable, no GOT entry, no function whose address came from a relocation. That is
// why the bias discovery and the loop are one straight line of stack locals and raw system calls
// (musl's static-PIE start makes the same promise), and why everything after them is a separate
// function.
//
// The portable dynamic build contributes nothing from this file: crt1.o defines _start there, glibc's
// start code relocates the PIE and builds the first thread's storage, so only the static link
// (CORDINAL_STATIC, -nostdlib) needs this.
#include "syscall.h"

#include <stddef.h>
#include <stdint.h>

#if defined(CORDINAL_STATIC)

// One relocation entry, shared by the two walks that read them: the image's own RELATIVE entries and
// the linker's IFUNC table.
struct Rela {
    uint64_t offset, info;
    int64_t addend;
};

namespace {

// The control block this file builds for the first thread. The offsets are the ones threads.cpp
// measured in a running glibc process and documents there: the block points at itself in two places,
// holds the dtv, the "more than one thread" flag, and the guards the compiler's canary reads.
constexpr size_t kTcbSelf = 0x00;
constexpr size_t kTcbSelfAgain = 0x10;
constexpr size_t kTcbDtv = 0x08;
constexpr size_t kTcbMultipleThreads = 0x18;
constexpr size_t kTcbStackGuard = 0x28;
constexpr size_t kTcbPointerGuard = 0x30;
constexpr size_t kTcbSize = 0x80;
constexpr long kArchSetFs = 0x1002;
constexpr long kProtReadWrite = 3;
constexpr long kMapPrivateAnonymous = 0x02 | 0x20;
constexpr unsigned long kAtPhdr = 3;
constexpr unsigned long kAtPhnum = 5;

// The ELF structures the boot reads before the C++ runtime exists. Their names and values are the
// ones in elf.h at zero cost; the file cannot include it in a static link that also defines the
// functions that header would declare.
struct Phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};
struct Dyn {
    int64_t tag, value;
};
constexpr uint32_t kPtLoad = 1;
constexpr uint32_t kPtDynamic = 2;
constexpr uint32_t kPtPhdr = 6;
constexpr uint32_t kPtTls = 7;
constexpr int64_t kDtNull = 0;
constexpr int64_t kDtPltRelSize = 2;
constexpr int64_t kDtSymtab = 6;
constexpr int64_t kDtRela = 7;
constexpr int64_t kDtRelaSize = 8;
constexpr int64_t kDtRelaEntry = 9;
constexpr int64_t kDtSymEntry = 11;
constexpr int64_t kDtJmpRel = 23;
constexpr uint32_t kRelaRelative = 8;
constexpr uint32_t kRelaIrelative = 37;
constexpr uint32_t kRela64 = 1;
constexpr uint32_t kRelaGlobDat = 6;
constexpr uint32_t kRelaJumpSlot = 7;
constexpr long kAtFdcwd = -100;
constexpr long kOpenReadOnly = 0;

// A dynamic symbol, as the relocation loop reads one: only the value matters, since every symbol a
// static-PIE's own relocations name is defined in the image itself.
struct Sym {
    uint32_t name;
    uint8_t info, other;
    uint16_t section;
    uint64_t value, size;
};

long Raw(long number, long a = 0, long b = 0, long c = 0, long d = 0, long e = 0, long f = 0) {
    return cordlibc::Syscall(number, a, b, c, d, e, f);
}
long RoundUp(long value, long align) { return (value + align - 1) & ~(align - 1); }

typedef void (*InitFunction)(int, char**, char**);

// The load bias: the difference between an address the kernel gave the image and the link-time
// address the linker wrote for it. AT_PHDR names the program headers at runtime; their link-time
// address is in the PT_PHDR segment when the link has one. GNU ld's static-PIE link does not write
// PT_PHDR, so the fallback pairs the ELF header's own address (a label the linker places, which a
// PC-relative reference reaches without any relocation) with the link-time address of the load
// segment that holds file offset zero, which is where the header always is.
uintptr_t ImageBias(const Phdr* headers, int phnum, uintptr_t phdrAddress, uintptr_t headerAddress) {
    for (int i = 0; i < phnum; ++i)
        if (headers[i].type == kPtPhdr) return phdrAddress - (uintptr_t)headers[i].vaddr;
    for (int i = 0; i < phnum; ++i)
        if (headers[i].type == kPtLoad && headers[i].offset == 0)
            return headerAddress - (uintptr_t)headers[i].vaddr;
    return 0;
}

// The RELATIVE relocations pair the image's link-time addresses with their runtime ones: every
// pointer the compiler put in .data, and every GOT entry the linker filled, lands on its own
// relocated address. The linker keeps the entries that need a resolver (.rela.iplt's IRELATIVE ones,
// and a link that folds them into .rela.plt says so through DT_JMPREL) in a second table, so this walk
// reads both and calls each resolver once, as glibc's static startup does. Nothing here reads a global
// or calls a function whose address came from one.
void ApplyRelocations(uintptr_t bias, const Dyn* dynamic) {
    const Rela* tables[2] = {nullptr, nullptr};
    size_t sizes[2] = {0, 0};
    const Sym* symbols = nullptr;
    size_t symbolEntry = sizeof(Sym);
    for (const Dyn* item = dynamic; item->tag != kDtNull; ++item) {
        switch (item->tag) {
            case kDtRela: tables[0] = (const Rela*)(bias + (uintptr_t)item->value); break;
            case kDtRelaSize: sizes[0] = (size_t)item->value; break;
            case kDtJmpRel: tables[1] = (const Rela*)(bias + (uintptr_t)item->value); break;
            case kDtPltRelSize: sizes[1] = (size_t)item->value; break;
            case kDtSymtab: symbols = (const Sym*)(bias + (uintptr_t)item->value); break;
            case kDtSymEntry: symbolEntry = (size_t)item->value; break;
            default: break;
        }
    }
    const size_t entry = sizeof(Rela);
    for (int table = 0; table < 2; ++table) {
        if (!tables[table] || !sizes[table]) continue;
        for (size_t at = 0; at + entry <= sizes[table]; at += entry) {
            const Rela* one = (const Rela*)((const char*)tables[table] + at);
            const uint32_t type = (uint32_t)(one->info & 0xffffffffu);
            uintptr_t* where = (uintptr_t*)(bias + (uintptr_t)one->offset);
            if (type == kRelaRelative) {
                *where = bias + (uintptr_t)one->addend;
            } else if (type == kRelaIrelative) {
                *where = ((uintptr_t(*)(void))(bias + (uintptr_t)one->addend))();
            } else if (type == kRelaJumpSlot || type == kRelaGlobDat || type == kRela64) {
                // A symbol the image defines itself: a GOT entry the linker could not fill at link time
                // only because the linker had not decided the symbol's address yet.
                const uint32_t index = (uint32_t)(one->info >> 32);
                if (!symbols || !index || !symbolEntry) continue;
                const Sym* symbol = (const Sym*)((const char*)symbols + (size_t)index * symbolEntry);
                *where = bias + (uintptr_t)symbol->value + (type == kRela64 ? (uintptr_t)one->addend : 0);
            }
        }
    }
}

// The bit of the stack the boot needs before anything else exists: the argument pointers, the
// environment the auxiliary vector follows, and the two entries of it that say where the program
// headers are.
struct StartArgs {
    long argc;
    char** argv;
    char** envp;
    const Phdr* headers;
    int phnum;
};

StartArgs ReadStart(const long* stack) {
    StartArgs start;
    start.argc = stack[0];
    start.argv = (char**)(stack + 1);
    start.envp = start.argv + start.argc + 1;
    char** p = start.envp;
    while (*p) ++p;
    uintptr_t phdr = 0;
    start.phnum = 0;
    for (const unsigned long* aux = (const unsigned long*)(p + 1); aux[0]; aux += 2) {
        if (aux[0] == kAtPhdr) phdr = aux[1];
        else if (aux[0] == kAtPhnum) start.phnum = (int)aux[1];
    }
    start.headers = (const Phdr*)phdr;
    return start;
}

// A decimal/hex writer for the ASLR check's child, which has no stream to write to.
char* WriteHex(char* out, uintptr_t value) {
    *out++ = '0';
    *out++ = 'x';
    for (int shift = 60; shift >= 0; shift -= 4) *out++ = "0123456789abcdef"[(value >> shift) & 15];
    return out;
}

// The ASLR check (loader_checks.h) runs this program again with this variable set. The child writes
// the image's base from /proc/self/maps and stops before any constructor runs, so the check compares
// two real load addresses without running the rest of the suite twice. Raw calls only: the stream
// machinery does not exist yet at this point in the boot.
constexpr char kAslrFlag[] = "CORDINAL_ASLR_CHILD=1";

bool IsAslrChild(char** envp) {
    for (char** item = envp; *item; ++item) {
        const char* text = *item;
        size_t at = 0;
        while (kAslrFlag[at] && text[at] == kAslrFlag[at]) ++at;
        if (!kAslrFlag[at]) return true;
    }
    return false;
}

void WriteAslrBaseAndStop() {
    char path[] = "/proc/self/maps";
    char buffer[512];
    const long fd = Raw(__NR_openat, kAtFdcwd, (long)path, kOpenReadOnly);
    long length = fd < 0 ? -1 : Raw(__NR_read, fd, (long)buffer, (long)sizeof buffer - 1);
    if (fd >= 0) Raw(__NR_close, fd);
    // The first line is the image's first segment at file offset zero, so its start is the base.
    uintptr_t base = 0;
    if (length > 0) {
        buffer[length] = 0;
        for (long at = 0; at < length && buffer[at] != '\n'; ++at) {
            const char digit = buffer[at];
            if (digit == '-') break;
            if (digit >= '0' && digit <= '9') base = (base << 4) | (uintptr_t)(digit - '0');
            else if (digit >= 'a' && digit <= 'f') base = (base << 4) | (uintptr_t)(digit - 'a' + 10);
        }
    }
    char message[32];
    char* out = message;
    *out++ = 'b';
    *out++ = 'a';
    *out++ = 's';
    *out++ = 'e';
    *out++ = ' ';
    out = WriteHex(out, base);
    *out++ = '\n';
    Raw(__NR_write, 1, (long)message, out - message);
    Raw(__NR_exit_group, 0);
    __builtin_unreachable();
}

}  // namespace

extern "C" {
// The constructors the linker collects, the program's own main, the exit path (exit.cpp in this build),
// and the environment array files.cpp defines.
extern InitFunction __init_array_start[] __attribute__((visibility("hidden")));
extern InitFunction __init_array_end[] __attribute__((visibility("hidden")));
int main(int, char**, char**);
__attribute__((noreturn)) void exit(int);
extern char** __environ;
// The ELF header, which GNU ld places and this file only uses to find the load bias when the link has
// no PT_PHDR segment.
extern char __ehdr_start[] __attribute__((visibility("hidden")));
// The indirect-function relocations: the program's own string functions are GNU indirect functions
// (string.cpp picks SSE2 or AVX2 at load), so every call to one goes through a GOT entry these
// describe. A non-PIE link marks the range around the .rela.iplt entries with these symbols; a
// static-PIE link puts the same entries in .rela.dyn instead and defines nothing here, which is why
// they are weak and the walk below skips them when they are absent (ApplyRelatives has already called
// those resolvers in that case).
extern const Rela __rela_iplt_start[] __attribute__((visibility("hidden"), weak));
extern const Rela __rela_iplt_end[] __attribute__((visibility("hidden"), weak));
// The loader's own bookkeeping (loader.cpp) needs the image's bias and program headers before any
// constructor can call dlopen, and it can only learn them here.
void cordlibc_LoaderInit(uintptr_t, const void*, int) __attribute__((visibility("hidden")));
}

extern "C" __attribute__((visibility("hidden"), noreturn)) void cordlibc_Boot(const long* stack);

// The kernel's first instructions. The stack pointer at entry is everything it gives the program
// besides the thread pointer, which is unset here.
__asm__(
    ".text\n"
    ".globl _start\n"
    ".hidden _start\n"
    ".type _start, @function\n"
    "_start:\n"
    "    xorl %ebp, %ebp\n"
    "    movq %rsp, %rdi\n"
    "    andq $-16, %rsp\n"
    "    call cordlibc_Boot\n"
    "    hlt\n"
    ".size _start, .-_start\n");

// Everything after the image can be read as relocated code. Separate and never inlined: by the time
// it runs, all globals and GOT entries hold runtime addresses.
__attribute__((noinline)) void cordlibc_AfterRelocation(uintptr_t bias, const StartArgs& start) {
    // The program's own thread-local block: its size is what the linker's local-exec offsets count back
    // from, so the block has to end at the thread pointer. A static-PIE image's segment addresses are
    // its link-time ones plus the load bias.
    long tlsSize = 0;
    const char* initImage = nullptr;
    long initSize = 0;
    for (int i = 0; i < start.phnum; ++i) {
        if (start.headers[i].type != kPtTls || start.headers[i].memsz == 0) continue;
        const long align = start.headers[i].align ? (long)start.headers[i].align : 1;
        tlsSize = RoundUp((long)start.headers[i].memsz, align);
        initImage = (const char*)(bias + (uintptr_t)start.headers[i].vaddr);
        initSize = (long)start.headers[i].filesz;
    }
    // One mapping: the thread-local block, the control block, then the dtv's count and generation and
    // its entries. The thread pointer is aligned to 16 bytes by starting the control block at the next
    // such address (the block below it keeps the size the linker's offsets count back from). The dtv's
    // two spare entries leave room for the modules the loader adds (loader.cpp grows it on demand).
    const long dtvBytes = 4 * (long)sizeof(uintptr_t);
    const long total = RoundUp(tlsSize + kTcbSize + dtvBytes + 80, 4096);
    char* base = (char*)Raw(__NR_mmap, 0, total, kProtReadWrite, kMapPrivateAnonymous, -1, 0);
    if ((uintptr_t)base >= (uintptr_t)-4095L) {
        Raw(__NR_exit_group, 127);
        __builtin_unreachable();
    }
    const uintptr_t tp = ((uintptr_t)base + (uintptr_t)tlsSize + 15) & ~(uintptr_t)15;
    char* block = (char*)(tp - (uintptr_t)tlsSize);
    for (long i = 0; i < initSize; ++i) block[i] = initImage[i];
    *(uintptr_t*)(tp + kTcbSelf) = tp;
    *(uintptr_t*)(tp + kTcbSelfAgain) = tp;
    *(int*)(tp + kTcbMultipleThreads) = 0;
    // The guards the compiler's stack canary and pointer mangling compare with: random bytes from the
    // kernel, as glibc takes them at start. A kernel without getrandom is older than any cordinal target.
    unsigned long random[2] = {0, 0};
    Raw(__NR_getrandom, (long)random, (long)sizeof random, 0);
    *(unsigned long*)(tp + kTcbStackGuard) = random[0] | 1;
    *(unsigned long*)(tp + kTcbPointerGuard) = random[1];
    uintptr_t* dtv = (uintptr_t*)(tp + kTcbSize);
    dtv[-2] = 2;  // slots, counting the generation word and the program's own entry
    dtv[-1] = 1;  // generation
    dtv[0] = tp - (uintptr_t)tlsSize;
    dtv[1] = 0;
    *(uintptr_t*)(tp + kTcbDtv) = (uintptr_t)dtv;
    if (Raw(__NR_arch_prctl, kArchSetFs, (long)tp) != 0) {
        Raw(__NR_exit_group, 127);
        __builtin_unreachable();
    }
    __environ = start.envp;
    // The loader (loader.cpp) is told where the image is before a constructor can ask it to load an
    // object, then the IFUNC walk and the constructors run.
    cordlibc_LoaderInit(bias, start.headers, start.phnum);    // A dynamic loader fills in the GOT entries of the indirect functions at load; a static program
    // has no loader, so it does it here, before any constructor can call one.
    for (const Rela* rela = __rela_iplt_start; rela && rela < __rela_iplt_end; ++rela)
        *(uintptr_t*)(bias + (uintptr_t)rela->offset) =
            ((uintptr_t(*)(void))(bias + (uintptr_t)rela->addend))();
    for (InitFunction* init = __init_array_start; init != __init_array_end; ++init)
        (*init)((int)start.argc, start.argv, start.envp);
    exit(main((int)start.argc, start.argv, start.envp));
}

// The first instructions in C++: read the stack, find the image's bias, apply the image's own
// relocations, then continue in code that can use globals.
void cordlibc_Boot(const long* stack) {
    const StartArgs start = ReadStart(stack);
    const uintptr_t bias = ImageBias(start.headers, start.phnum, (uintptr_t)start.headers,
                                     (uintptr_t)__ehdr_start);
    const Dyn* dynamic = nullptr;
    for (int i = 0; i < start.phnum; ++i)
        if (start.headers[i].type == kPtDynamic) dynamic = (const Dyn*)(bias + (uintptr_t)start.headers[i].vaddr);
    if (dynamic) ApplyRelocations(bias, dynamic);
    if (IsAslrChild(start.envp)) WriteAslrBaseAndStop();
    cordlibc_AfterRelocation(bias, start);
    __builtin_unreachable();
}

#endif  // CORDINAL_STATIC
