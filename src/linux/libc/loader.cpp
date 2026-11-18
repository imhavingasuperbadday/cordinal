// The static program's loader: dlopen, dlsym, dlclose, dlerror, dladdr and dladdr1 for a program that
// has no glibc loader under it. A static-PIE image cannot be linked against a shared object by the
// linker, so this file reads one itself: it maps the object's PT_LOAD segments from the standard
// library directories, walks its PT_DYNAMIC section, resolves its imports against the program's
// exported set (linux/cordial-export.map, turned into a table at configure time) and the modules
// already loaded, applies its relocations, runs its constructors, and gives every thread its own copy
// of the object's thread-local storage through the program's own dtv (threads.cpp's layout).
//
// The scope is deliberately that of a small loader: RTLD_LAZY is executed as RTLD_NOW (a relocation is
// never deferred), RTLD_LOCAL is the default scope, RTLD_GLOBAL puts a module in the lookup scope of
// later loads, the standard directories are searched rather than LD_LIBRARY_PATH, symbol versions are
// ignored, RTLD_NEXT, RTLD_NOLOAD and dladdr1's link-map form are not implemented, and an object that
// asks for the static TLS model (R_X86_64_TPOFF64, which only a fixed offset from the thread pointer
// could satisfy) is refused rather than loaded wrongly. Each of those is a deliberate limit, not an
// oversight; docs/linux-libc.md, "Static-PIE and the loader", lists them.
//
// Nothing here may include the headers of the functions the program itself defines: the generated
// export table declares those names as byte arrays, so including <string.h> or <stdlib.h> would declare
// the same names as functions. The file therefore uses raw system calls and its own byte helpers, and
// mmaps the memory its bookkeeping needs. Only the static build compiles it.
#include "syscall.h"

#include <dlfcn.h>
#include <link.h>
#include <elf.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#if defined(CORDINAL_STATIC)

extern "C" {
// The program's environment array, which files.cpp defines and start.cpp fills in, for a constructor
// that asks for the program's environment.
extern char** __environ;
}

namespace cordlibc_loader {

// One name the program answers to, and its address. linux/cordial-export.map is the list; CMake turns
// the same file into this table and into the linker's version script.
struct Export {
    const char* name;
    void* address;
};

}  // namespace cordlibc_loader

#include "loader_exports.inc"

namespace cordlibc_loader {
namespace {

// ----------------------------------------------------------------------------------------------------
// Raw kernel calls and byte helpers. The program's own read, mmap, strlen and friends are named in the
// export table, so this file cannot call them by name.

constexpr long kProtNone = 0;
constexpr long kProtRead = 1;
constexpr long kProtWrite = 2;
constexpr long kProtExec = 4;
constexpr long kMapPrivate = 0x02;
constexpr long kMapFixed = 0x10;
constexpr long kMapAnonymous = 0x20;
constexpr long kAtFdcwd = -100;
constexpr long kOpenReadOnly = 0;
constexpr int kFutexWait = 0;
constexpr int kFutexWake = 1;
constexpr int kFutexPrivate = 128;
constexpr int kFutexContended = 2;
constexpr size_t kPage = 4096;

long Raw(long number, long a = 0, long b = 0, long c = 0, long d = 0, long e = 0, long f = 0) {
    return cordlibc::Syscall(number, a, b, c, d, e, f);
}
int Tid() { return (int)Raw(__NR_gettid); }
void* MapBytes(long size) {
    const long result = Raw(__NR_mmap, 0, size, kProtRead | kProtWrite, kMapPrivate | kMapAnonymous, -1, 0);
    return (uintptr_t)result >= (uintptr_t)-4095L ? nullptr : (void*)result;
}
void UnmapBytes(void* address, long size) { Raw(__NR_munmap, (long)address, size); }
uintptr_t ThreadPointer() {
    uintptr_t value;
    __asm__ volatile("mov %%fs:0, %0" : "=r"(value));
    return value;
}

bool Same(const char* a, const char* b) {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}
size_t Length(const char* text) {
    size_t length = 0;
    while (text[length]) ++length;
    return length;
}
void Copy(char* to, const char* from, size_t limit) {
    size_t at = 0;
    for (; from[at] && at + 1 < limit; ++at) to[at] = from[at];
    to[at] = 0;
}
void Fill(char* to, char value, size_t count) {
    for (size_t i = 0; i < count; ++i) to[i] = value;
}
void CopyBytes(void* to, const void* from, size_t count) {
    char* out = (char*)to;
    const char* in = (const char*)from;
    for (size_t i = 0; i < count; ++i) out[i] = in[i];
}
uintptr_t AlignDown(uintptr_t value, uintptr_t align) { return value & ~(align - 1); }
uintptr_t AlignUp(uintptr_t value, uintptr_t align) { return (value + align - 1) & ~(align - 1); }

// The futex lock malloc.cpp and threads.cpp use, made recursive: an initializer may open another
// object, and the module list is one structure. The owner is the thread's own id.
struct Lock {
    int state;
    int owner;
    int depth;
    void Acquire() {
        const int self = Tid();
        if (__atomic_load_n(&owner, __ATOMIC_RELAXED) == self) {
            ++depth;
            return;
        }
        int expected = 0;
        if (!__atomic_compare_exchange_n(&state, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            for (int i = 0; i < 64; ++i) {
                __builtin_ia32_pause();
                expected = 0;
                if (__atomic_load_n(&state, __ATOMIC_RELAXED) == 0 &&
                    __atomic_compare_exchange_n(&state, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
                    break;
            }
            if (__atomic_load_n(&state, __ATOMIC_ACQUIRE) != 0)
                while (__atomic_exchange_n(&state, kFutexContended, __ATOMIC_ACQUIRE) != 0)
                    Raw(__NR_futex, (long)&state, kFutexWait | kFutexPrivate, kFutexContended);
        }
        __atomic_store_n(&owner, self, __ATOMIC_RELAXED);
        __atomic_store_n(&depth, 1, __ATOMIC_RELAXED);
    }
    void Release() {
        if (--depth > 0) return;
        __atomic_store_n(&owner, 0, __ATOMIC_RELAXED);
        if (__atomic_exchange_n(&state, 0, __ATOMIC_RELEASE) == kFutexContended)
            Raw(__NR_futex, (long)&state, kFutexWake | kFutexPrivate, 1);
    }
};
Lock loaderLock;
void AcquireLoaderLock() { loaderLock.Acquire(); }
void ReleaseLoaderLock() { loaderLock.Release(); }

// ----------------------------------------------------------------------------------------------------
// The loaded objects.

constexpr size_t kMaxPath = 512;
constexpr int kMaxDependencies = 16;
constexpr size_t kMaxError = 256;
constexpr int kMaxDepth = 16;

struct Module {
    char path[kMaxPath];         // the file it was read from (the program's own name is read lazily)
    char soname[128];            // DT_SONAME, when the object has one
    uintptr_t bias;              // add to every link-time address
    uintptr_t mapStart, mapEnd;  // the reservation, for dlclose
    const Elf64_Phdr* phdr;      // a copy of the object's program headers
    int phnum;
    const Elf64_Dyn* dynamic;
    const Elf64_Sym* symbols;
    const char* strings;
    const uint32_t* gnuHash;
    const uint32_t* sysvHash;
    size_t symbolCount;
    void (**initArray)(int, char**, char**);
    size_t initCount;
    void (**finiArray)(void);
    size_t finiCount;
    uintptr_t (*initFunction)(int, char**, char**);
    void (*finiFunction)(void);
    Module* dependencies[kMaxDependencies];
    int dependencyCount;
    size_t tlsId;  // its slot in every thread's dtv; 0 without a PT_TLS segment
    const void* tlsImage;
    size_t tlsInitSize;
    size_t tlsSize;
    size_t tlsAlign;
    int refcount;
    bool isProgram;
    bool global;
    bool constructorsRun;
    bool hasTls;
    Module* next;
};

Module programModule;
Module* loadedModules = nullptr;  // the program first, then every loaded object
size_t nextTlsId = 2;             // slot 1 is the program's own block (start.cpp builds it)
int moduleCount = 0;

__thread char errorText[kMaxError];
__thread bool errorPending;

void SetError(const char* text) {
    Copy(errorText, text, sizeof errorText);
    errorPending = true;
}
void SetPathError(const char* path, const char* text) {
    const size_t used = Length(path) < sizeof errorText - 3 ? Length(path) : sizeof errorText - 3;
    Copy(errorText, path, used + 1);
    errorText[used] = ':';
    errorText[used + 1] = ' ';
    Copy(errorText + used + 2, text, sizeof errorText - used - 2);
    errorPending = true;
}
void SetSymbolError(const char* text, const char* name) {
    const size_t used = Length(text) < sizeof errorText - 1 ? Length(text) : sizeof errorText - 1;
    Copy(errorText, text, used + 1);
    Copy(errorText + used, name, sizeof errorText - used);
    errorPending = true;
}
void ClearError() { errorPending = false; }

bool ReadAt(int fd, void* buffer, long bytes, long offset) {
    long done = 0;
    while (done < bytes) {
        const long got = Raw(__NR_pread64, fd, (long)buffer + done, bytes - done, offset + done);
        if (got <= 0) return false;
        done += got;
    }
    return true;
}

// The program's own name, for dladdr and dl_iterate_phdr. Read once, the first time it is wanted.
void NameProgram() {
    if (programModule.path[0]) return;
    const long length = Raw(__NR_readlink, (long)"/proc/self/exe", (long)programModule.path,
                            (long)sizeof programModule.path - 1);
    if (length <= 0 || length >= (long)sizeof programModule.path) {
        Copy(programModule.path, "", sizeof programModule.path);
        return;
    }
    programModule.path[length] = 0;
}

void AddModule(Module* module) {
    Module** link = &loadedModules;
    while (*link) link = &(*link)->next;
    *link = module;
    ++moduleCount;
}

Module* FindByAddress(uintptr_t address) {
    for (Module* module = loadedModules; module; module = module->next) {
        if (address < module->mapStart || address >= module->mapEnd) continue;
        for (int i = 0; i < module->phnum; ++i) {
            if (module->phdr[i].p_type != PT_LOAD) continue;
            const uintptr_t start = module->bias + (uintptr_t)module->phdr[i].p_vaddr;
            if (address >= start && address < start + (uintptr_t)module->phdr[i].p_memsz) return module;
        }
    }
    return nullptr;
}
Module* FindByHandle(void* handle) {
    for (Module* module = loadedModules; module; module = module->next)
        if (module == (Module*)handle) return module;
    return nullptr;
}

// ----------------------------------------------------------------------------------------------------
// Symbols.

uint32_t NameHash(const char* name) {
    uint32_t hash = 5381;
    for (const unsigned char* p = (const unsigned char*)name; *p; ++p) hash = hash * 33 + *p;
    return hash;
}

// The number of entries in the dynamic symbol table: the SysV hash says it directly, and the GNU hash
// only through the last bucket's chain, which is what this walks.
size_t CountSymbols(const Module& module) {
    if (module.sysvHash) return module.sysvHash[1];
    if (!module.gnuHash) return 0;
    const uint32_t buckets = module.gnuHash[0];
    const uint32_t first = module.gnuHash[1];
    const uint32_t bloomCount = module.gnuHash[2];
    const uint32_t* bucket = module.gnuHash + 4 + bloomCount * (sizeof(uintptr_t) / 4);
    const uint32_t* chain = bucket + buckets;
    uint32_t last = 0;
    for (uint32_t i = 0; i < buckets; ++i)
        if (bucket[i] > last) last = bucket[i];
    if (!last) return first;
    uint32_t index = last;
    while (!(chain[index - first] & 1)) ++index;
    return index + 1;
}

const Elf64_Sym* FindSymbol(const Module& module, const char* name) {
    if (!module.symbols || !module.strings) return nullptr;
    if (module.gnuHash) {
        const uint32_t hash = NameHash(name);
        const uint32_t buckets = module.gnuHash[0];
        const uint32_t first = module.gnuHash[1];
        const uint32_t bloomCount = module.gnuHash[2];
        const uintptr_t* bloom = (const uintptr_t*)(module.gnuHash + 4);
        const uintptr_t word = bloom[(hash / (8 * sizeof(uintptr_t))) % bloomCount];
        const uintptr_t mask = ((uintptr_t)1 << (hash % (8 * sizeof(uintptr_t)))) |
                               ((uintptr_t)1 << ((hash >> 6) % (8 * sizeof(uintptr_t))));
        if ((word & mask) != mask) return nullptr;
        const uint32_t* bucket = (const uint32_t*)(bloom + bloomCount);
        uint32_t index = bucket[hash % buckets];
        if (index < first) return nullptr;
        const uint32_t* chain = bucket + buckets;
        for (;;) {
            const uint32_t value = chain[index - first];
            if (((value ^ hash) >> 1) == 0 && Same(module.strings + module.symbols[index].st_name, name))
                return &module.symbols[index];
            if (value & 1) return nullptr;
            ++index;
        }
    }
    if (module.sysvHash) {
        const uint32_t buckets = module.sysvHash[0];
        const uint32_t* bucket = module.sysvHash + 2;
        const uint32_t* chain = bucket + buckets;
        for (uint32_t index = bucket[NameHash(name) % buckets]; index != 0 && index < module.symbolCount;
             index = chain[index]) {
            const Elf64_Sym& symbol = module.symbols[index];
            if (symbol.st_name && Same(module.strings + symbol.st_name, name)) return &symbol;
        }
        return nullptr;
    }
    // No hash at all: a small object, so a plain walk is enough.
    for (size_t i = 0; i < module.symbolCount; ++i) {
        const Elf64_Sym& symbol = module.symbols[i];
        if (symbol.st_name && Same(module.strings + symbol.st_name, name)) return &symbol;
    }
    return nullptr;
}

struct Found {
    uintptr_t address;
    const Elf64_Sym* symbol;
    Module* module;
};

// A definition inside one module: its address, with the module's bias applied, and an indirect
// function's resolver called for the real one.
bool DefineIn(Module& module, const Elf64_Sym* symbol, Found& found) {
    if (!symbol || symbol->st_shndx == SHN_UNDEF) return false;
    if (ELF64_ST_BIND(symbol->st_info) == STB_WEAK && symbol->st_shndx == SHN_ABS && symbol->st_value == 0)
        return false;
    uintptr_t address = symbol->st_shndx == SHN_ABS ? (uintptr_t)symbol->st_value
                                                    : module.bias + (uintptr_t)symbol->st_value;
    if (ELF64_ST_TYPE(symbol->st_info) == STT_GNU_IFUNC) address = ((uintptr_t(*)(void))address)();
    found.address = address;
    found.symbol = symbol;
    found.module = &module;
    return true;
}

// The program's exported set: the table CMake generated from linux/cordial-export.map. The addresses
// are the program's own, already relocated, so nothing is added to them.
bool LookupInProgram(const char* name, Found& found) {
    for (size_t i = 0; i < sizeof kProgramExports / sizeof kProgramExports[0]; ++i) {
        if (!Same(kProgramExports[i].name, name)) continue;
        found.address = (uintptr_t)kProgramExports[i].address;
        found.symbol = nullptr;
        found.module = &programModule;
        return true;
    }
    return false;
}

// The lookup order this loader promises: the object's own definitions, then the program's exported
// set, then the modules loaded with RTLD_GLOBAL in load order, then the object's dependencies.
bool Resolve(Module& from, const char* name, Found& found, int depth) {
    if (!from.isProgram) {
        if (const Elf64_Sym* own = FindSymbol(from, name))
            if (DefineIn(from, own, found)) return true;
        if (LookupInProgram(name, found)) return true;
    }
    for (Module* module = loadedModules; module; module = module->next) {
        if (!module->global || module == &from) continue;
        if (const Elf64_Sym* symbol = FindSymbol(*module, name))
            if (DefineIn(*module, symbol, found)) return true;
    }
    if (depth < kMaxDepth) {
        for (int i = 0; i < from.dependencyCount; ++i) {
            Module* dependency = from.dependencies[i];
            if (const Elf64_Sym* symbol = FindSymbol(*dependency, name))
                if (DefineIn(*dependency, symbol, found)) return true;
            if (Resolve(*dependency, name, found, depth + 1)) return true;
        }
    }
    return false;
}

// The nearest named symbol at or below an address, for dladdr. A loaded object's table has sizes and
// types, and those are honored.
const Elf64_Sym* NearestSymbol(Module& module, uintptr_t address) {
    const Elf64_Sym* best = nullptr;
    for (size_t i = 0; i < module.symbolCount; ++i) {
        const Elf64_Sym& symbol = module.symbols[i];
        if (symbol.st_shndx == SHN_UNDEF || !symbol.st_name) continue;
        const int type = ELF64_ST_TYPE(symbol.st_info);
        if (type != STT_FUNC && type != STT_OBJECT) continue;
        const uintptr_t start = module.bias + (uintptr_t)symbol.st_value;
        if (address < start) continue;
        if (symbol.st_size && address >= start + symbol.st_size) continue;
        if (!best || module.bias + (uintptr_t)best->st_value < start) best = &symbol;
    }
    return best;
}

// ----------------------------------------------------------------------------------------------------
// Thread-local storage for a loaded object. The dtv is the one start.cpp builds and threads.cpp reads:
// a count at [-2], the generation at [-1], and one 16-byte entry per module from [0], the program's own
// at [0]. A thread that meets a module id past its array gets a larger one; a thread that has room but
// no block for the module gets one now, which is the lazy allocation musl's loader does.

uintptr_t* GrowDtv(uintptr_t* dtv, size_t slots, size_t needed) {
    const size_t grown = needed + 1;
    uintptr_t* bigger = (uintptr_t*)MapBytes((long)((grown + 2) * sizeof(uintptr_t)));
    if (!bigger) return nullptr;
    CopyBytes(bigger, dtv - 2, (slots + 2) * sizeof(uintptr_t));
    Fill((char*)(bigger + slots), 0, (grown - slots) * sizeof(uintptr_t));
    bigger[0] = (uintptr_t)grown;
    *(uintptr_t*)(ThreadPointer() + 0x08) = (uintptr_t)(bigger + 2);
    // The old array is not freed: it may sit inside the thread's own mapping (threads.cpp copies the
    // dtv into a new thread's area) and nothing records which. It is a few dozen bytes per module.
    return bigger + 2;
}

void* MakeTlsBlock(const Module& module) {
    const long bytes = (long)(module.tlsSize + module.tlsAlign + kPage);
    char* raw = (char*)MapBytes(bytes);
    if (!raw) return nullptr;
    char* block = (char*)AlignUp((uintptr_t)raw, module.tlsAlign ? module.tlsAlign : 16);
    Fill(block, 0, module.tlsSize);
    if (module.tlsInitSize) CopyBytes(block, module.tlsImage, module.tlsInitSize);
    return block;
}

// The block for one module on this thread, made on first use. A module id below 2 is the program's own,
// whose static block start.cpp already holds.
void* LoaderTlsBlock(unsigned long id) {
    if (id < 2) return nullptr;
    Module* owner = nullptr;
    for (Module* module = loadedModules; module; module = module->next)
        if (module->tlsId == id) {
            owner = module;
            break;
        }
    if (!owner) return nullptr;
    uintptr_t* dtv = *(uintptr_t**)(ThreadPointer() + 0x08);
    if (!dtv) return nullptr;
    size_t slots = dtv[-2];
    if ((size_t)id >= slots) {
        dtv = GrowDtv(dtv, slots, id);
        if (!dtv) return nullptr;
        slots = dtv[-2];
    }
    if (!dtv[id]) {
        void* block = MakeTlsBlock(*owner);
        if (!block) return nullptr;
        dtv[id] = (uintptr_t)block;
    }
    if ((size_t)id > dtv[-1]) dtv[-1] = id;  // the generation: the highest slot the thread has filled
    return (void*)dtv[id];
}

// ----------------------------------------------------------------------------------------------------
// Reading an object.

int PageProtection(const Elf64_Phdr& segment) {
    int prot = kProtRead;
    if (segment.p_flags & PF_W) prot |= kProtWrite;
    if (segment.p_flags & PF_X) prot |= kProtExec;
    return prot;
}

// Reads one object from the file and maps it: the header, the program headers, then each PT_LOAD at its
// own protection, with the bytes past the file's end cleared.
Module* MapObject(const char* path, char* error, size_t errorSize) {
    const long fd = Raw(__NR_openat, kAtFdcwd, (long)path, kOpenReadOnly);
    if (fd < 0) {
        Copy(error, "cannot open shared object file", errorSize);
        return nullptr;
    }
    Elf64_Ehdr header;
    if (!ReadAt(fd, &header, sizeof header, 0) || header.e_ident[EI_MAG0] != ELFMAG0 ||
        header.e_ident[EI_MAG1] != ELFMAG1 || header.e_ident[EI_MAG2] != ELFMAG2 ||
        header.e_ident[EI_MAG3] != ELFMAG3) {
        Raw(__NR_close, fd);
        Copy(error, "not an ELF object", errorSize);
        return nullptr;
    }
    if (header.e_ident[EI_CLASS] != ELFCLASS64 || header.e_ident[EI_DATA] != ELFDATA2LSB ||
        header.e_machine != EM_X86_64 || header.e_type != ET_DYN || header.e_phnum == 0 ||
        header.e_phentsize != sizeof(Elf64_Phdr) || header.e_phoff == 0) {
        Raw(__NR_close, fd);
        Copy(error, "not a loadable x86-64 shared object", errorSize);
        return nullptr;
    }
    const long phdrBytes = (long)header.e_phnum * (long)sizeof(Elf64_Phdr);
    Elf64_Phdr* phdr = (Elf64_Phdr*)MapBytes(phdrBytes);
    if (!phdr) {
        Raw(__NR_close, fd);
        Copy(error, "out of memory", errorSize);
        return nullptr;
    }
    if (!ReadAt(fd, phdr, phdrBytes, (long)header.e_phoff)) {
        Raw(__NR_close, fd);
        UnmapBytes(phdr, phdrBytes);
        Copy(error, "cannot read the program headers", errorSize);
        return nullptr;
    }
    uintptr_t low = UINTPTR_MAX, high = 0, alignment = kPage;
    for (int i = 0; i < header.e_phnum; ++i) {
        if (phdr[i].p_type != PT_LOAD) continue;
        const uintptr_t start = AlignDown((uintptr_t)phdr[i].p_vaddr, kPage);
        const uintptr_t end = AlignUp((uintptr_t)phdr[i].p_vaddr + (uintptr_t)phdr[i].p_memsz, kPage);
        if (start < low) low = start;
        if (end > high) high = end;
        if (phdr[i].p_align > alignment) alignment = phdr[i].p_align;
    }
    if (low == UINTPTR_MAX || high <= low) {
        Raw(__NR_close, fd);
        UnmapBytes(phdr, phdrBytes);
        Copy(error, "no loadable segments", errorSize);
        return nullptr;
    }
    const long used = (long)(high - low);
    const long reserve = used + (long)alignment + (long)kPage;
    char* region = (char*)Raw(__NR_mmap, 0, reserve, kProtNone, kMapPrivate | kMapAnonymous, -1, 0);
    if ((uintptr_t)region >= (uintptr_t)-4095L) {
        Raw(__NR_close, fd);
        UnmapBytes(phdr, phdrBytes);
        Copy(error, "out of memory", errorSize);
        return nullptr;
    }
    const uintptr_t start = AlignUp((uintptr_t)region, alignment);
    if (start > (uintptr_t)region) UnmapBytes(region, (long)(start - (uintptr_t)region));
    const uintptr_t end = start + (uintptr_t)used;
    const uintptr_t reserveEnd = (uintptr_t)region + (uintptr_t)reserve;
    if (reserveEnd > end) UnmapBytes((void*)end, (long)(reserveEnd - end));
    const uintptr_t bias = start - low;
    bool mapped = true;
    for (int i = 0; i < header.e_phnum && mapped; ++i) {
        if (phdr[i].p_type != PT_LOAD) continue;
        const uintptr_t segment = bias + (uintptr_t)phdr[i].p_vaddr;
        const uintptr_t page = AlignDown(segment, kPage);
        const long offset = (long)AlignDown((uintptr_t)phdr[i].p_offset, kPage);
        const long fileBytes = (long)((uintptr_t)phdr[i].p_filesz + (segment - page));
        const long memBytes = (long)AlignUp((uintptr_t)phdr[i].p_memsz + (segment - page), kPage);
        const int prot = PageProtection(phdr[i]);
        if (Raw(__NR_mmap, (long)page, memBytes, prot, kMapPrivate | kMapFixed, fd, offset) != (long)page) {
            mapped = false;
            break;
        }
        // The bytes past the file's end are zero, as a loader guarantees for .bss even when the last
        // file page holds more. The mapping may be read-only, so the tail is made writable first.
        if (fileBytes < memBytes) {
            const long from = (long)AlignDown(page + (uintptr_t)fileBytes, kPage);
            const long to = page + memBytes;
            Raw(__NR_mprotect, from, to - from, prot | kProtWrite);
            Fill((char*)(page + fileBytes), 0, (size_t)(memBytes - fileBytes));
            Raw(__NR_mprotect, from, to - from, prot);
        }
    }
    Raw(__NR_close, fd);
    if (!mapped) {
        UnmapBytes((void*)start, used);
        UnmapBytes(phdr, phdrBytes);
        Copy(error, "cannot map a segment", errorSize);
        return nullptr;
    }
    Module* module = (Module*)MapBytes((long)sizeof(Module));
    if (!module) {
        UnmapBytes((void*)start, used);
        UnmapBytes(phdr, phdrBytes);
        Copy(error, "out of memory", errorSize);
        return nullptr;
    }
    Fill((char*)module, 0, sizeof(Module));
    Copy(module->path, path, sizeof module->path);
    module->bias = bias;
    module->mapStart = start;
    module->mapEnd = end;
    module->phdr = phdr;
    module->phnum = header.e_phnum;
    for (int i = 0; i < header.e_phnum; ++i) {
        const Elf64_Phdr& entry = phdr[i];
        if (entry.p_type == PT_DYNAMIC) module->dynamic = (const Elf64_Dyn*)(bias + (uintptr_t)entry.p_vaddr);
        if (entry.p_type == PT_TLS && entry.p_memsz) {
            module->hasTls = true;
            module->tlsImage = (const void*)(bias + (uintptr_t)entry.p_vaddr);
            module->tlsInitSize = (size_t)entry.p_filesz;
            module->tlsSize = (size_t)entry.p_memsz;
            module->tlsAlign = (size_t)(entry.p_align ? entry.p_align : 1);
        }
    }
    return module;
}

void ReadDynamic(Module& module) {
    if (!module.dynamic) return;
    for (const Elf64_Dyn* item = module.dynamic; item->d_tag != DT_NULL; ++item) {
        const uintptr_t value = (uintptr_t)item->d_un.d_ptr;
        switch (item->d_tag) {
            case DT_SYMTAB: module.symbols = (const Elf64_Sym*)(module.bias + value); break;
            case DT_STRTAB: module.strings = (const char*)(module.bias + value); break;
            case DT_GNU_HASH: module.gnuHash = (const uint32_t*)(module.bias + value); break;
            case DT_HASH: module.sysvHash = (const uint32_t*)(module.bias + value); break;
            case DT_INIT: module.initFunction = (uintptr_t(*)(int, char**, char**))(module.bias + value); break;
            case DT_FINI: module.finiFunction = (void (*)(void))(module.bias + value); break;
            case DT_INIT_ARRAY: module.initArray = (void (**)(int, char**, char**))(module.bias + value); break;
            case DT_INIT_ARRAYSZ: module.initCount = (size_t)item->d_un.d_val / sizeof(void*); break;
            case DT_FINI_ARRAY: module.finiArray = (void (**)(void))(module.bias + value); break;
            case DT_FINI_ARRAYSZ: module.finiCount = (size_t)item->d_un.d_val / sizeof(void*); break;
            case DT_SONAME:
                if (module.strings) Copy(module.soname, module.strings + (size_t)item->d_un.d_val, sizeof module.soname);
                break;
            default: break;
        }
    }
    module.symbolCount = CountSymbols(module);
}

// ----------------------------------------------------------------------------------------------------
// Relocations.

bool ApplyRelocations(Module& module) {
    if (!module.dynamic) return true;
    const Elf64_Rela* tables[2] = {nullptr, nullptr};
    size_t sizes[2] = {0, 0};
    for (const Elf64_Dyn* item = module.dynamic; item->d_tag != DT_NULL; ++item) {
        switch (item->d_tag) {
            case DT_RELA: tables[0] = (const Elf64_Rela*)(module.bias + (uintptr_t)item->d_un.d_ptr); break;
            case DT_RELASZ: sizes[0] = (size_t)item->d_un.d_val; break;
            case DT_JMPREL: tables[1] = (const Elf64_Rela*)(module.bias + (uintptr_t)item->d_un.d_ptr); break;
            case DT_PLTRELSZ: sizes[1] = (size_t)item->d_un.d_val; break;
            default: break;
        }
    }
    for (int table = 0; table < 2; ++table) {
        if (!tables[table] || !sizes[table]) continue;
        // A linker may place .rela.plt inside .rela.dyn's range; the second walk then repeats those
        // entries, which is harmless for every type handled here (the same value is written twice).
        for (size_t at = 0; at + sizeof(Elf64_Rela) <= sizes[table]; at += sizeof(Elf64_Rela)) {
            const Elf64_Rela& entry = *(const Elf64_Rela*)((const char*)tables[table] + at);
            const uint32_t type = (uint32_t)ELF64_R_TYPE(entry.r_info);
            const uint32_t index = (uint32_t)ELF64_R_SYM(entry.r_info);
            uintptr_t* where = (uintptr_t*)(module.bias + (uintptr_t)entry.r_offset);
            const Elf64_Sym* symbol = index ? &module.symbols[index] : nullptr;
            if (type == R_X86_64_NONE) continue;
            if (type == R_X86_64_RELATIVE) {
                *where = module.bias + (uintptr_t)entry.r_addend;
                continue;
            }
            if (type == R_X86_64_IRELATIVE) {
                *where = ((uintptr_t(*)(void))(module.bias + (uintptr_t)entry.r_addend))();
                continue;
            }
            if (type == R_X86_64_TPOFF64) {
                SetPathError(module.path, "the static TLS model (R_X86_64_TPOFF64) is not supported");
                return false;
            }
            if (!symbol) {
                SetPathError(module.path, "a relocation names no symbol");
                return false;
            }
            if (index >= module.symbolCount) {
                SetPathError(module.path, "a relocation names a symbol past the table's end");
                return false;
            }
            const char* name = module.strings + symbol->st_name;
            if (type == R_X86_64_DTPMOD64) {
                Module* owner = &module;
                if (symbol->st_shndx == SHN_UNDEF) {
                    Found found{};
                    if (!Resolve(module, name, found, 0)) {
                        SetSymbolError("undefined thread-local symbol: ", name);
                        return false;
                    }
                    owner = found.module;
                }
                *where = owner->tlsId;
                continue;
            }
            if (type == R_X86_64_DTPOFF64 || type == R_X86_64_DTPOFF32) {
                *where = (uintptr_t)symbol->st_value + (uintptr_t)entry.r_addend;
                continue;
            }
            Found found{};
            if (!Resolve(module, name, found, 0)) {
                SetSymbolError("undefined symbol: ", name);
                return false;
            }
            switch (type) {
                case R_X86_64_64: *where = found.address + (uintptr_t)entry.r_addend; break;
                case R_X86_64_GLOB_DAT:
                case R_X86_64_JUMP_SLOT: *where = found.address; break;
                default:
                    SetPathError(module.path, "an unsupported relocation type");
                    return false;
            }
        }
    }
    return true;
}

// The object's read-only data stops being writable once its relocations are in place.
void ProtectRelro(Module& module) {
    for (int i = 0; i < module.phnum; ++i) {
        if (module.phdr[i].p_type != PT_GNU_RELRO) continue;
        const uintptr_t start = AlignDown(module.bias + (uintptr_t)module.phdr[i].p_vaddr, kPage);
        const uintptr_t end = AlignUp(module.bias + (uintptr_t)module.phdr[i].p_vaddr +
                                          (uintptr_t)module.phdr[i].p_memsz,
                                      kPage);
        Raw(__NR_mprotect, (long)start, (long)(end - start), kProtRead);
    }
}

void RunInitializers(Module& module) {
    if (module.constructorsRun) return;
    module.constructorsRun = true;
    char** environment = ::__environ;
    if (module.initFunction) module.initFunction(0, nullptr, environment);
    for (size_t i = 0; i < module.initCount; ++i) module.initArray[i](0, nullptr, environment);
}
void RunFinalizers(Module& module) {
    for (size_t i = module.finiCount; i > 0; --i) module.finiArray[i - 1]();
    if (module.finiFunction) module.finiFunction();
}

// ----------------------------------------------------------------------------------------------------
// Paths. An object named with a slash is used as it is; a bare name is looked for in the directories a
// 64-bit distribution keeps its libraries in. LD_LIBRARY_PATH is not read: a static program has no
// loader configuration to honor, by design.

const char* const kDirectories[] = {
    "/lib/x86_64-linux-gnu", "/usr/lib/x86_64-linux-gnu", "/lib64", "/usr/lib64",
    "/lib",                  "/usr/lib",                  "/usr/local/lib",
};

bool ContainsSlash(const char* text) {
    for (; *text; ++text)
        if (*text == '/') return true;
    return false;
}

bool FileExists(const char* path) {
    const long fd = Raw(__NR_openat, kAtFdcwd, (long)path, kOpenReadOnly);
    if (fd < 0) return false;
    Raw(__NR_close, fd);
    return true;
}

bool ResolvePath(const char* name, char* out, size_t size) {
    if (ContainsSlash(name)) {
        if (Length(name) + 1 > size) return false;
        Copy(out, name, size);
        return true;
    }
    for (size_t i = 0; i < sizeof kDirectories / sizeof kDirectories[0]; ++i) {
        const size_t needed = Length(kDirectories[i]) + 1 + Length(name) + 1;
        if (needed > size) continue;
        Copy(out, kDirectories[i], size);
        const size_t at = Length(out);
        out[at] = '/';
        Copy(out + at + 1, name, size - at - 1);
        if (FileExists(out)) return true;
    }
    Copy(out, name, size);
    return false;
}

Module* FindLoaded(const char* path) {
    for (Module* module = loadedModules; module; module = module->next) {
        if (module->isProgram) continue;
        if (Same(module->path, path)) return module;
        if (module->soname[0] && Same(module->soname, path)) return module;
    }
    return nullptr;
}

// The workhorse behind dlopen: map the object, its dependencies first, then relocate and initialize it.
Module* Load(const char* name, bool global, int depth) {
    if (depth > kMaxDepth) {
        SetError("the dependency chain is too deep");
        return nullptr;
    }
    char path[kMaxPath];
    if (!ResolvePath(name, path, sizeof path)) {
        SetPathError(name, "cannot open shared object file");
        return nullptr;
    }
    if (Module* existing = FindLoaded(path)) {
        ++existing->refcount;
        if (global) existing->global = true;
        return existing;
    }
    char error[kMaxError] = {};
    Module* module = MapObject(path, error, sizeof error);
    if (!module) {
        SetPathError(path, error);
        return nullptr;
    }
    module->refcount = 1;
    module->global = global;
    ReadDynamic(*module);
    bool ok = true;
    // The object's dependencies are loaded first, depth-first, so their constructors have run and
    // their symbols exist by the time this object's relocations are applied.
    for (const Elf64_Dyn* item = module->dynamic; item && item->d_tag != DT_NULL && ok; ++item) {
        if (item->d_tag != DT_NEEDED || !module->strings) continue;
        const char* needed = module->strings + (size_t)item->d_un.d_val;
        if (module->dependencyCount >= kMaxDependencies) {
            SetPathError(path, "too many dependencies");
            ok = false;
            break;
        }
        Module* dependency = Load(needed, global, depth + 1);
        if (!dependency) {
            ok = false;
            break;
        }
        module->dependencies[module->dependencyCount++] = dependency;
    }
    if (!ok) {
        UnmapBytes((void*)module->mapStart, (long)(module->mapEnd - module->mapStart));
        UnmapBytes((void*)module->phdr, (long)(module->phnum * sizeof(Elf64_Phdr)));
        UnmapBytes(module, (long)sizeof(Module));
        return nullptr;
    }
    if (module->hasTls) module->tlsId = nextTlsId++;
    AddModule(module);
    if (!ApplyRelocations(*module)) {
        // Out of the list, since nothing else can reach it yet: imports fail, so the object never runs.
        for (Module** link = &loadedModules; *link; link = &(*link)->next)
            if (*link == module) {
                *link = module->next;
                --moduleCount;
                break;
            }
        UnmapBytes((void*)module->mapStart, (long)(module->mapEnd - module->mapStart));
        UnmapBytes((void*)module->phdr, (long)(module->phnum * sizeof(Elf64_Phdr)));
        UnmapBytes(module, (long)sizeof(Module));
        return nullptr;
    }
    ProtectRelro(*module);
    RunInitializers(*module);
    return module;
}

}  // namespace

// ----------------------------------------------------------------------------------------------------
// The calls threads.cpp, start.cpp and the interface below make.

extern "C" void cordlibc_LoaderInit(uintptr_t bias, const void* phdr, int phnum) {
    programModule.bias = bias;
    programModule.phdr = (const Elf64_Phdr*)phdr;
    programModule.phnum = phnum;
    programModule.isProgram = true;
    programModule.global = true;
    programModule.constructorsRun = true;
    programModule.tlsId = 1;  // the slot start.cpp's own dtv entry holds
    uintptr_t low = UINTPTR_MAX, high = 0;
    for (int i = 0; i < phnum; ++i) {
        if (programModule.phdr[i].p_type != PT_LOAD) continue;
        const uintptr_t start = bias + (uintptr_t)programModule.phdr[i].p_vaddr;
        if (start < low) low = start;
        if (start + (uintptr_t)programModule.phdr[i].p_memsz > high) high = start + (uintptr_t)programModule.phdr[i].p_memsz;
    }
    programModule.mapStart = low;
    programModule.mapEnd = high;
    if (!loadedModules) AddModule(&programModule);
}

extern "C" void* LoaderTlsGet(unsigned long id) { return LoaderTlsBlock(id); }

// The image's load bias, for threads.cpp's layout discovery: a static-PIE link has no PT_PHDR segment
// for it to read the bias from.
extern "C" uintptr_t cordlibc_LoaderBias(void) { return programModule.bias; }

extern "C" int LoaderPhdrWalk(int (*callback)(struct dl_phdr_info*, size_t, void*), void* data) {
    for (Module* module = loadedModules; module; module = module->next) {
        if (module->isProgram) continue;
        struct dl_phdr_info info;
        Fill((char*)&info, 0, sizeof info);
        info.dlpi_addr = module->bias;
        info.dlpi_name = module->path;
        info.dlpi_phdr = module->phdr;
        info.dlpi_phnum = (ElfW(Half))module->phnum;
        info.dlpi_adds = (unsigned long long)moduleCount;
        info.dlpi_tls_modid = module->tlsId;
        info.dlpi_tls_data = module->tlsId ? LoaderTlsBlock(module->tlsId) : nullptr;
        const int result = callback(&info, sizeof info, data);
        if (result) return result;
    }
    return 0;
}

}  // namespace cordlibc_loader

// ----------------------------------------------------------------------------------------------------
// The interface itself.

extern "C" {

void* dlopen(const char* file, int mode) {
    using namespace cordlibc_loader;
    ClearError();
    if (!file) {
        NameProgram();
        return &programModule;
    }
    if (mode & ~(RTLD_LAZY | RTLD_NOW | RTLD_GLOBAL | RTLD_LOCAL)) {
        SetPathError(file, "unsupported dlopen flags");
        return nullptr;
    }
    AcquireLoaderLock();
    Module* module = Load(file, (mode & RTLD_GLOBAL) != 0, 0);
    ReleaseLoaderLock();
    return module;
}

void* dlsym(void* handle, const char* name) {
    using namespace cordlibc_loader;
    AcquireLoaderLock();
    Found found{};
    bool ok = false;
    if (handle == RTLD_DEFAULT) {
        ok = LookupInProgram(name, found);
        for (Module* module = loadedModules; module && !ok; module = module->next)
            if (module->global && FindSymbol(*module, name)) ok = DefineIn(*module, FindSymbol(*module, name), found);
    } else if (handle == RTLD_NEXT) {
        ReleaseLoaderLock();
        SetError("dlsym: RTLD_NEXT is not supported by the static program's loader");
        return nullptr;
    } else {
        Module* module = FindByHandle(handle);
        if (module == &programModule) {
            ok = LookupInProgram(name, found);
        } else if (!module) {
            ReleaseLoaderLock();
            SetError("dlsym: not a handle the static program's loader returned");
            return nullptr;
        } else {
            ok = Resolve(*module, name, found, 0);
        }
    }
    ReleaseLoaderLock();
    if (!ok) {
        SetSymbolError("dlsym: symbol not found: ", name);
        return nullptr;
    }
    ClearError();
    return (void*)found.address;
}

int dlclose(void* handle) {
    using namespace cordlibc_loader;
    ClearError();
    AcquireLoaderLock();
    Module* module = handle == RTLD_DEFAULT || handle == RTLD_NEXT ? nullptr : FindByHandle(handle);
    if (!module || module->isProgram) {
        ReleaseLoaderLock();
        SetError("dlclose: not a handle the static program's loader returned");
        return -1;
    }
    if (--module->refcount > 0) {
        ReleaseLoaderLock();
        return 0;
    }
    RunFinalizers(*module);
    for (Module** link = &loadedModules; *link; link = &(*link)->next)
        if (*link == module) {
            *link = module->next;
            --moduleCount;
            break;
        }
    for (int i = 0; i < module->dependencyCount; ++i) {
        Module* dependency = module->dependencies[i];
        if (!dependency || --dependency->refcount > 0) continue;
        RunFinalizers(*dependency);
        for (Module** link = &loadedModules; *link; link = &(*link)->next)
            if (*link == dependency) {
                *link = dependency->next;
                --moduleCount;
                break;
            }
        if (dependency->hasTls) continue;
        UnmapBytes((void*)dependency->mapStart, (long)(dependency->mapEnd - dependency->mapStart));
        UnmapBytes((void*)dependency->phdr, (long)(dependency->phnum * sizeof(Elf64_Phdr)));
        UnmapBytes(dependency, (long)sizeof(Module));
    }
    // A module with thread-local storage keeps its record and its mapping: every thread that touched it
    // has a block in its dtv pointing into the object's image, and nothing can prove those threads are
    // gone. That is a deliberate leak in exchange for never handing out freed thread-local storage.
    if (module->hasTls) {
        ReleaseLoaderLock();
        return 0;
    }
    UnmapBytes((void*)module->mapStart, (long)(module->mapEnd - module->mapStart));
    UnmapBytes((void*)module->phdr, (long)(module->phnum * sizeof(Elf64_Phdr)));
    UnmapBytes(module, (long)sizeof(Module));
    ReleaseLoaderLock();
    return 0;
}

char* dlerror(void) {
    using namespace cordlibc_loader;
    if (!errorPending) return nullptr;
    errorPending = false;
    return errorText;
}

int dladdr(const void* address, Dl_info* info) {
    using namespace cordlibc_loader;
    if (!address) return 0;  // `info` is nonnull in glibc's declaration, and callers pass a real one.
    AcquireLoaderLock();
    Module* module = FindByAddress((uintptr_t)address);
    if (!module) {
        ReleaseLoaderLock();
        return 0;
    }
    if (module->isProgram) NameProgram();
    info->dli_fname = module->path;
    info->dli_fbase = (void*)module->bias;
    info->dli_sname = nullptr;
    info->dli_saddr = nullptr;
    if (module->isProgram) {
        // The program's table carries addresses without sizes, so the greatest one that does not
        // exceed the query is the answer; a loaded object's own table has sizes and those are honored.
        const char* bestName = nullptr;
        uintptr_t best = 0;
        for (size_t i = 0; i < sizeof kProgramExports / sizeof kProgramExports[0]; ++i) {
            const uintptr_t candidate = (uintptr_t)kProgramExports[i].address;
            if (candidate > (uintptr_t)address) continue;
            if (!bestName || candidate > best) {
                bestName = kProgramExports[i].name;
                best = candidate;
            }
        }
        info->dli_sname = bestName;
        info->dli_saddr = (void*)best;
    } else if (const Elf64_Sym* symbol = NearestSymbol(*module, (uintptr_t)address)) {
        info->dli_sname = module->strings + symbol->st_name;
        info->dli_saddr = (void*)(module->bias + (uintptr_t)symbol->st_value);
    }
    ReleaseLoaderLock();
    return 1;
}

int dladdr1(const void* address, Dl_info* info, void** extra, int flags) {
    using namespace cordlibc_loader;
    if (flags != RTLD_DL_SYMENT) {
        ClearError();
        SetError("dladdr1: only RTLD_DL_SYMENT is supported by the static program's loader");
        return 0;
    }
    if (!dladdr(address, info)) return 0;
    AcquireLoaderLock();
    Module* module = FindByAddress((uintptr_t)address);
    *extra = module && !module->isProgram ? (void*)NearestSymbol(*module, (uintptr_t)address) : nullptr;
    ReleaseLoaderLock();
    return 1;
}

}  // extern "C"

#endif  // CORDINAL_STATIC
