// CDP1 PE64 startup: decode into RW pages, restore Windows linkage, then make code executable.
// An EXE decodes its eager chunks on several threads at the entry point; a DLL, or an EXE on one
// core, decodes them in turn (spec.md, "Chunks and threads"). Code the profile marked cold stays
// uncommitted and is decoded by the vectored handler on its first fault, and can be decommitted
// again (spec.md, "Code decoded on demand" and "Detached images"): such an image is restored into
// private memory of its own, which Windows is then told is the program's module.
#include "../decoder/stream.h"
#include "../filter.h"

extern "C" __declspec(dllexport) const volatile cdp::Anchor cdp_anchor = {
    {'C', 'D', 'P', 'A', 'N', 'C', 'H', 'O', 'R', '2', '0', '2', '6', '1', 0, 0}, 0, 0, 0, 0};

// Export the exact freestanding core for the host measurement harness as well as the loader.
extern "C" __declspec(dllexport) __attribute__((noinline)) cdp::DecodeStatus
cdp_decode(const uint8_t *input, size_t input_bytes, uint8_t *output, size_t output_bytes,
           cdp::Models *models, size_t workspace_bytes) {
    return cdp::decode_stream(input, input_bytes, output, output_bytes, models, workspace_bytes);
}

namespace {
using Byte = uint8_t;
using Status = int32_t;
using Protect = Status (*)(void *, void **, size_t *, uint32_t, uint32_t *);
using Flush = Status (*)(void *, const void *, size_t);
using Terminate = Status (*)(void *, Status);
struct Text {
    uint16_t length, capacity;
    void *bytes;
};
using Load = Status (*)(void *, uint32_t *, Text *, void **);
using Procedure = Status (*)(void *, Text *, uint32_t, void **);
using AddFunctions = unsigned (*)(void *, uint32_t, uint64_t);
using DeleteFunctions = unsigned (*)(void *);
using Callback = void (*)(void *, uint32_t, void *);
using ThreadStart = Status (*)(void *);
using CreateThread = Status (*)(void *, void *, uint8_t, uint32_t, size_t, size_t, ThreadStart, void *,
                                void **, void *);
using Wait = Status (*)(void *, uint8_t, void *);
using Close = Status (*)(void *);
using ExitThread = void (*)(Status);
using Allocate = Status (*)(void *, void **, uintptr_t, size_t *, uint32_t, uint32_t);
using Free = Status (*)(void *, void **, size_t *, uint32_t);
using AddHandler = void *(*)(uint32_t, void *);
using Suspend = Status (*)(void *, uint32_t *);
using NextThread = Status (*)(void *, void *, uint32_t, uint32_t, uint32_t, void **);
using QueryThread = Status (*)(void *, uint32_t, void *, uint32_t, uint32_t *);
using CreateEvent = Status (*)(void **, uint32_t, void *, uint32_t, uint8_t);
using Signal = Status (*)(void *, uint32_t *);
using GetContext = Status (*)(void *, void *);
using PcToHeader = void *(*)(void *, void **);
using TreeRemove = uint8_t (*)(void *, void *);
using TreeInsert = uint8_t (*)(void *, void *, uint8_t, void *);
using LockLoader = Status (*)(uint32_t, uint32_t *, void **);
using QueryMemory = Status (*)(void *, void *, uint32_t, void *, size_t, size_t *);
using UnlockLoader = Status (*)(uint32_t, void *);

constexpr uint32_t name_hash(const char *text) {
    uint32_t hash = 2166136261;
    while (*text) {
        hash = (hash ^ uint8_t(*text++)) * 16777619;
    }
    return hash;
}

void *exported(Byte *module, uint32_t wanted) {
    const auto *optional = module + cdp::read32(module + 60) + 24;
    const uint32_t export_rva = cdp::read32(optional + 112);
    if (!export_rva || cdp::read32(optional + 116) < 40)
        return nullptr;
    const auto *directory = module + export_rva;
    const uint32_t count = cdp::read32(directory + 24);
    const auto *names = module + cdp::read32(directory + 32);
    const auto *ordinals = module + cdp::read32(directory + 36);
    const auto *addresses = module + cdp::read32(directory + 28);
    for (uint32_t i = 0; i < count; ++i) {
        const auto *name = reinterpret_cast<const char *>(module + cdp::read32(names + i * 4));
        if (name_hash(name) == wanted)
            return module + cdp::read32(addresses + cdp::read16(ordinals + i * 2) * 4);
    }
    return nullptr;
}

Byte *peb() {
    Byte *value;
    __asm__("movq %%gs:0x60,%0" : "=r"(value));
    return value;
}

Byte *ntdll() {
    Byte *loader = *reinterpret_cast<Byte **>(peb() + 24);
    Byte *head = loader + 32;
    Byte *link = *reinterpret_cast<Byte **>(head);
    for (unsigned i = 0; i < 128 && link != head; ++i, link = *reinterpret_cast<Byte **>(link)) {
        Byte *module = *reinterpret_cast<Byte **>(link + 32);
        if (module && exported(module, name_hash("NtProtectVirtualMemory")))
            return module;
    }
    return nullptr;
}

struct Api {
    Byte *ntdll;
    Protect protect;
    Flush flush;
    Terminate terminate;
    Load load;
    Procedure procedure;
    AddFunctions add_functions;
    DeleteFunctions delete_functions;
    CreateThread create_thread;
    Wait wait;
    Close close;
    ExitThread exit_thread;
    Allocate allocate;
    Free free;
    AddHandler add_handler;
    Suspend suspend;
    Suspend resume;
    NextThread next_thread;
    QueryThread query_thread;
    CreateEvent create_event;
    Signal signal_event;
    Signal reset_event;
    GetContext get_context;
    PcToHeader pc_to_header;
    TreeRemove tree_remove;
    TreeInsert tree_insert;
    LockLoader lock_loader;
    UnlockLoader unlock_loader;
    QueryMemory query_memory;
};

Api api() {
    Byte *module = ntdll();
    if (!module)
        __builtin_trap();
    return {module,
            reinterpret_cast<Protect>(exported(module, name_hash("NtProtectVirtualMemory"))),
            reinterpret_cast<Flush>(exported(module, name_hash("NtFlushInstructionCache"))),
            reinterpret_cast<Terminate>(exported(module, name_hash("NtTerminateProcess"))),
            reinterpret_cast<Load>(exported(module, name_hash("LdrLoadDll"))),
            reinterpret_cast<Procedure>(exported(module, name_hash("LdrGetProcedureAddress"))),
            reinterpret_cast<AddFunctions>(exported(module, name_hash("RtlAddFunctionTable"))),
            reinterpret_cast<DeleteFunctions>(exported(module, name_hash("RtlDeleteFunctionTable"))),
            reinterpret_cast<CreateThread>(exported(module, name_hash("RtlCreateUserThread"))),
            reinterpret_cast<Wait>(exported(module, name_hash("NtWaitForSingleObject"))),
            reinterpret_cast<Close>(exported(module, name_hash("NtClose"))),
            reinterpret_cast<ExitThread>(exported(module, name_hash("RtlExitUserThread"))),
            reinterpret_cast<Allocate>(exported(module, name_hash("NtAllocateVirtualMemory"))),
            reinterpret_cast<Free>(exported(module, name_hash("NtFreeVirtualMemory"))),
            reinterpret_cast<AddHandler>(exported(module, name_hash("RtlAddVectoredExceptionHandler"))),
            reinterpret_cast<Suspend>(exported(module, name_hash("NtSuspendThread"))),
            reinterpret_cast<Suspend>(exported(module, name_hash("NtResumeThread"))),
            reinterpret_cast<NextThread>(exported(module, name_hash("NtGetNextThread"))),
            reinterpret_cast<QueryThread>(exported(module, name_hash("NtQueryInformationThread"))),
            reinterpret_cast<CreateEvent>(exported(module, name_hash("NtCreateEvent"))),
            reinterpret_cast<Signal>(exported(module, name_hash("NtSetEvent"))),
            reinterpret_cast<Signal>(exported(module, name_hash("NtResetEvent"))),
            reinterpret_cast<GetContext>(exported(module, name_hash("NtGetContextThread"))),
            reinterpret_cast<PcToHeader>(exported(module, name_hash("RtlPcToFileHeader"))),
            reinterpret_cast<TreeRemove>(exported(module, name_hash("RtlRbRemoveNode"))),
            reinterpret_cast<TreeInsert>(exported(module, name_hash("RtlRbInsertNodeEx"))),
            reinterpret_cast<LockLoader>(exported(module, name_hash("LdrLockLoaderLock"))),
            reinterpret_cast<UnlockLoader>(exported(module, name_hash("LdrUnlockLoaderLock"))),
            reinterpret_cast<QueryMemory>(exported(module, name_hash("NtQueryVirtualMemory")))};
}

void *const self_process = reinterpret_cast<void *>(intptr_t(-1));

[[noreturn]] void fail(const Api &calls, unsigned reason) {
    if (calls.terminate)
        calls.terminate(self_process, Status(0xcd000000u | reason));
    __builtin_trap();
}

bool extent(uint32_t at, size_t bytes, uint32_t end) { return at <= end && bytes <= end - at; }

void permission(const Api &calls, Byte *at, size_t bytes, uint32_t protection) {
    void *address = at;
    uint32_t previous;
    if (bytes &&
        (!calls.protect || calls.protect(self_process, &address, &bytes, protection, &previous) < 0))
        fail(calls, 20);
}

bool commit(const Api &calls, Byte *at, size_t bytes) {
    void *address = at;
    return !bytes || (calls.allocate && calls.allocate(self_process, &address, 0, &bytes, 0x1000, 4) >= 0);
}

// The packed image's base, from the anchor's own RVA. Stepping back from &cdp_anchor as a pointer
// leaves that constant object's bounds, and clang then treats stores through anything derived
// from it as undefined: it dropped the cold path's plain counter stores and kept only the atomic
// ones. Computed as an integer the optimizer cannot see through, the base is an ordinary address.
Byte *image_base() {
    uintptr_t anchor = reinterpret_cast<uintptr_t>(&cdp_anchor);
    __asm__("" : "+r"(anchor));
    return reinterpret_cast<Byte *>(anchor - cdp_anchor.self);
}

uint32_t image_size(const Byte *module) { return cdp::read32(module + cdp::read32(module + 60) + 24 + 56); }

struct Native {
    const Byte *optional;
    const Byte *sections;
    uint16_t count;
    uint32_t size;
    uint64_t preferred;
    bool dll;
    uint32_t directory(unsigned i) const { return cdp::read32(optional + 112 + i * 8); }
    uint32_t directory_bytes(unsigned i) const { return cdp::read32(optional + 116 + i * 8); }
};

Native native(const Api &calls, const cdp::Header *header) {
    const auto *bytes = reinterpret_cast<const Byte *>(header + 1);
    if (header->native_header_bytes < 64)
        fail(calls, 2);
    const uint32_t pe = cdp::read32(bytes + 60);
    if (!extent(pe, 264, header->native_header_bytes) || cdp::read32(bytes + pe) != 0x4550 ||
        cdp::read16(bytes + pe + 4) != 0x8664 || cdp::read16(bytes + pe + 24) != 0x20b)
        fail(calls, 3);
    const unsigned count = cdp::read16(bytes + pe + 6);
    const unsigned table = pe + 24 + cdp::read16(bytes + pe + 20);
    if (!count || count > 96 || !extent(table, count * 40, header->native_header_bytes))
        fail(calls, 4);
    const Byte *optional = bytes + pe + 24;
    return {optional, bytes + table, uint16_t(count), cdp::read32(optional + 56),
            cdp::read64(optional + 24), (cdp::read16(bytes + pe + 22) & 0x2000) != 0};
}

const cdp::Chunk *chunk_table(const cdp::Header *header) {
    const auto *bytes = reinterpret_cast<const Byte *>(header);
    return reinterpret_cast<const cdp::Chunk *>(bytes + header->metadata_bytes -
                                                header->extent_count * sizeof(cdp::Extent) -
                                                header->chunk_count * sizeof(cdp::Chunk));
}

const cdp::Extent *extent_table(const cdp::Header *header) {
    const auto *bytes = reinterpret_cast<const Byte *>(header);
    return reinterpret_cast<const cdp::Extent *>(bytes + header->metadata_bytes -
                                                 header->extent_count * sizeof(cdp::Extent));
}

bool lazy(const cdp::Header *header, uint32_t chunk) { return chunk_table(header)[chunk].flags & cdp::lazy; }

// The chunk whose extents cover `offset` (relative to memory_rva), or UINT32_MAX.
uint32_t extent_owner(const cdp::Header *header, uint32_t offset) {
    const cdp::Extent *extents = extent_table(header);
    uint32_t low = 0, high = header->extent_count;
    while (low < high) {
        const uint32_t middle = low + (high - low) / 2;
        if (extents[middle].offset <= offset)
            low = middle + 1;
        else
            high = middle;
    }
    if (!low)
        return UINT32_MAX;
    const cdp::Extent &extent = extents[low - 1];
    return offset - extent.offset < extent.bytes ? extent.chunk : UINT32_MAX;
}

// Where `offset` (relative to memory_rva) lands in a decoding chunk's scratch buffer.
uint32_t scratch_offset(const cdp::Header *header, uint32_t chunk, uint32_t offset) {
    const cdp::Extent *extents = extent_table(header);
    uint32_t at = 0;
    for (uint32_t i = 0; i < header->extent_count; ++i) {
        if (extents[i].chunk != chunk)
            continue;
        if (offset - extents[i].offset < extents[i].bytes)
            return at + offset - extents[i].offset;
        at += extents[i].bytes;
    }
    return UINT32_MAX;
}

bool code_section(const Native &old, uint32_t rva) {
    for (unsigned i = 0; i < old.count; ++i) {
        const Byte *section = old.sections + i * 40;
        const uint32_t at = cdp::read32(section + 12), bytes = cdp::read32(section + 16);
        if ((cdp::read32(section + 36) & 0x20000000) && at <= rva && rva < at + bytes)
            return true;
    }
    return false;
}

void copy_bytes(Byte *to, const Byte *from, size_t bytes) {
    for (size_t i = 0; i < bytes; ++i)
        to[i] = from[i];
}

const cdp::Header *metadata(Byte *base, const Api &calls) {
    const uint32_t size = image_size(base);
    if (!extent(cdp_anchor.metadata, cdp_anchor.metadata_bytes, size) ||
        cdp_anchor.metadata_bytes < sizeof(cdp::Header))
        fail(calls, 5);
    const auto *header = reinterpret_cast<const cdp::Header *>(base + cdp_anchor.metadata);
    if (!header->chunk_count || header->chunk_count > cdp::max_chunks || !header->extent_count ||
        header->extent_count > cdp::max_extents ||
        !extent(cdp_anchor.workspace, 4096 + header->chunk_count * cdp::workspace_slot, size))
        fail(calls, 5);
    if (cdp::read64(header->magic) != UINT64_C(0x00314b4341504443) || header->version != cdp::version ||
        header->metadata_bytes != cdp_anchor.metadata_bytes ||
        cdp::adler32(reinterpret_cast<const Byte *>(header), header->metadata_bytes,
                     offsetof(cdp::Header, metadata_adler)) != header->metadata_adler)
        fail(calls, 6);
    const size_t tables = size_t(header->chunk_count) * sizeof(cdp::Chunk) +
                          size_t(header->extent_count) * sizeof(cdp::Extent);
    // An attached image restores into CDP0, below the metadata; a detached one into memory of its
    // own, at most as large as the packed image, which the loader reserves.
    const bool attached = !(header->flags & cdp::detached);
    if (!header->memory_rva || header->memory_rva % 4096 || !header->memory_bytes ||
        header->memory_bytes > 128 * 1024 * 1024 || (header->flags & ~cdp::detached) ||
        header->cold_cap > cdp::max_chunks ||
        !extent(header->memory_rva, header->memory_bytes, attached ? cdp_anchor.metadata : size) ||
        !extent(cdp_anchor.metadata + header->compressed_offset, header->compressed_bytes, size) ||
        header->compressed_offset < header->metadata_bytes ||
        !extent(sizeof(cdp::Header), header->native_header_bytes, header->metadata_bytes) ||
        header->metadata_bytes < sizeof(cdp::Header) + tables || header->copy_count > 4096 ||
        header->gap_count > 97)
        fail(calls, 7);
    return header;
}

// Applies the image's DIR64 fixups. `selected` is a chunk index when its scratch is being decoded,
// or UINT32_MAX at start-up, when every target in chunks [0, eager) is applied in the image.
void relocate(Byte *image, const Api &calls, const cdp::Header *header, const Native &old,
              uint32_t selected, Byte *scratch, uint32_t eager) {
    const uint64_t delta = uint64_t(reinterpret_cast<uintptr_t>(image)) - old.preferred;
    uint32_t at = old.directory(5), bytes = old.directory_bytes(5);
    if (!extent(at, bytes, old.size) || (delta && !bytes))
        fail(calls, 8);
    const uint32_t end = at + bytes;
    while (at < end) {
        if (!extent(at, 8, end))
            fail(calls, 9);
        const uint32_t page = cdp::read32(image + at), block = cdp::read32(image + at + 4);
        if (block < 8 || (block & 1) || !extent(at, block, end))
            fail(calls, 10);
        for (uint32_t i = 8; i < block; i += 2) {
            const uint16_t entry = cdp::read16(image + at + i);
            if (!(entry >> 12))
                continue;
            const uint32_t target = page + (entry & 4095);
            if ((entry >> 12) != 10 || !extent(target, 8, old.size))
                fail(calls, 11);
            const uint32_t offset = target - header->memory_rva;
            if (scratch) {
                const uint32_t place = scratch_offset(header, selected, offset);
                if (place == UINT32_MAX)
                    continue;
                cdp::write64(scratch + place, cdp::read64(scratch + place) + delta);
            } else {
                const uint32_t owner = extent_owner(header, offset);
                if (owner == UINT32_MAX)
                    fail(calls, 41);
                // A cold chunk applies its own fixups when it is decoded.
                if (owner >= eager)
                    continue;
                cdp::write64(image + target, cdp::read64(image + target) + delta);
            }
        }
        at += block;
    }
}

unsigned text_length(Byte *base, uint32_t rva, uint32_t limit, unsigned maximum, const Api &calls) {
    if (rva >= limit)
        fail(calls, 12);
    unsigned length = 0;
    while (length < maximum && length < limit - rva && base[rva + length])
        ++length;
    if (length == maximum || length == limit - rva)
        fail(calls, 12);
    return length;
}

void imports(Byte *base, const Api &calls, const Native &old) {
    uint32_t at = old.directory(1), bytes = old.directory_bytes(1);
    if (!bytes)
        return;
    if (!calls.load || !calls.procedure || !extent(at, bytes, old.size))
        fail(calls, 13);
    const uint32_t end = at + bytes;
    for (; extent(at, 20, end); at += 20) {
        const uint32_t name = cdp::read32(base + at + 12);
        if (!name)
            return;
        const unsigned length = text_length(base, name, old.size, 255, calls);
        uint16_t wide[256];
        for (unsigned i = 0; i < length; ++i)
            wide[i] = base[name + i];
        wide[length] = 0;
        Text library{uint16_t(length * 2), uint16_t((length + 1) * 2), wide};
        void *module = nullptr;
        if (calls.load(nullptr, nullptr, &library, &module) < 0 || !module)
            fail(calls, 14);
        uint32_t destination = cdp::read32(base + at + 16);
        uint32_t source = cdp::read32(base + at);
        if (!source)
            source = destination;
        for (;;) {
            if (!extent(source, 8, old.size) || !extent(destination, 8, old.size))
                fail(calls, 15);
            const uint64_t item = cdp::read64(base + source);
            if (!item)
                break;
            void *address = nullptr;
            Status status;
            if (item >> 63)
                status = calls.procedure(module, nullptr, uint32_t(item & 65535), &address);
            else {
                if (item > UINT32_MAX - 2)
                    fail(calls, 16);
                const uint32_t text = uint32_t(item) + 2;
                const unsigned length = text_length(base, text, old.size, 65534, calls);
                Text symbol{uint16_t(length), uint16_t(length + 1), base + text};
                status = calls.procedure(module, &symbol, 0, &address);
            }
            if (status < 0 || !address)
                fail(calls, 17);
            cdp::write64(base + destination, reinterpret_cast<uintptr_t>(address));
            source += 8;
            destination += 8;
        }
    }
    fail(calls, 18);
}

// The workspace's first page: initialization state, the reserved argument a deferred EXE TLS
// callback received, and what the threads decoding and evicting cold code share. The suspended
// thread handles live here too, off the stack of whichever thread faults.
constexpr uint32_t max_threads = 448;
struct State {
    uint32_t phase;  // 0 not started, 1 initializing, 2 done
    uint32_t next_chunk;
    uint32_t failed;
    uint32_t tls_slot;  // a detached image's TLS index, where Windows writes it (pack.py: offset 12)
    void *reserved;
    uint32_t lock;        // 0 free, 1 held while one cold chunk is decoded or chunks are evicted
    uint32_t owner;       // thread that holds the lock, so a fault inside the decode passes on
    uint32_t eager;       // chunks [0, eager) decode at start-up; the rest are cold
    uint32_t decoded[2];  // bitset: cold chunks committed and decoded now
    void *event;          // manual-reset; set whenever the lock is released
    Byte *native;         // a detached image's restored native image; null when attached
    uint32_t suspended, last_suspended;
    uint32_t cap, resident, decodes, evictions;
    uint32_t clock, pad;
    uint32_t used[cdp::max_chunks];   // clock at each cold chunk's last decode, for the cap
    uint16_t count[cdp::max_chunks];  // times each cold chunk has been decoded
    void *threads[max_threads];
};
static_assert(sizeof(State) <= 4096 && sizeof(cdp::Models) <= cdp::workspace_slot &&
              __builtin_offsetof(State, tls_slot) == 12);

State *state_of(Byte *base) { return reinterpret_cast<State *>(base + cdp_anchor.workspace); }

// The restored native image: CDP0 of the packed one, or memory of its own when detached.
Byte *image_of(Byte *base) {
    Byte *native = state_of(base)->native;
    return native ? native : base;
}

// The first model slot is the cold path's scratch: the decode's models, and while chunks are
// evicted the suspended threads' contexts and instruction pointers. Both happen under the lock.
Byte *cold_scratch(Byte *base) { return base + cdp_anchor.workspace + 4096; }

uint64_t current_thread() {
    uint64_t id;
    __asm__("movq %%gs:0x48,%0" : "=r"(id));
    return id;
}

struct ThreadBasic {
    int32_t status;
    uint32_t pad;
    Byte *teb;
    void *process;
    uint64_t thread;
    uint64_t affinity;
    int32_t priority, base;
};

// Decoding or evicting cold code changes whole pages. Every other thread is suspended across the
// change, so none can execute a half-written page or read one as data. A thread suspended inside
// the kernel finishes its current system call first, so a lock it holds there cannot block ours.
// The handles need THREAD_SUSPEND_RESUME and THREAD_GET_CONTEXT as well as the query right: with
// the query right alone, every suspension failed and no thread was ever stopped.
constexpr uint32_t thread_access = 0x0800 | 0x0008 | 0x0002;
void suspend_others(const Api &calls, State *state) {
    state->suspended = 0;
    const uint64_t self = current_thread();
    void *key = nullptr;
    bool kept = false;
    for (unsigned i = 0; i < 8192; ++i) {
        void *thread = nullptr;
        const Status status = calls.next_thread(self_process, key, thread_access, 0, 0, &thread);
        if (key && !kept)
            calls.close(key);
        key = thread;
        kept = false;
        if (status < 0 || !thread)
            break;
        ThreadBasic info;
        uint32_t bytes = 0;
        if (calls.query_thread(thread, 0, &info, sizeof(info), &bytes) < 0 || info.thread == self)
            continue;
        uint32_t previous = 0;
        if (calls.suspend(thread, &previous) < 0)
            continue;
        // More threads than the state page can hold is not a case to copy through unsuspended.
        if (state->suspended >= max_threads)
            fail(calls, 40);
        state->threads[state->suspended++] = thread;
        kept = true;
    }
    if (key && !kept)
        calls.close(key);
}

void resume_all(const Api &calls, State *state) {
    state->last_suspended = state->suspended;
    for (uint32_t i = state->suspended; i; --i) {
        void *thread = state->threads[i - 1];
        uint32_t previous = 0;
        calls.resume(thread, &previous);
        calls.close(thread);
    }
    state->suspended = 0;
}

// One cold chunk decodes at a time. The event is manual-reset: every release sets it and every
// acquirer clears it, so a thread that misses one wake-up sleeps until the next release.
void lock_cold(const Api &calls, State *state) {
    for (;;) {
        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(&state->lock, &expected, 1, false, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED)) {
            state->owner = uint32_t(current_thread());
            calls.reset_event(state->event, nullptr);
            return;
        }
        if (calls.wait(state->event, 0, nullptr) < 0)
            fail(calls, 39);
    }
}

void unlock_cold(const Api &calls, State *state) {
    state->owner = 0;
    calls.signal_event(state->event, nullptr);
    __atomic_store_n(&state->lock, 0, __ATOMIC_RELEASE);
}

bool is_decoded(State *state, uint32_t index) {
    return (__atomic_load_n(&state->decoded[index >> 5], __ATOMIC_ACQUIRE) >> (index & 31)) & 1;
}

void set_decoded(State *state, uint32_t index, bool on) {
    const uint32_t bit = 1u << (index & 31);
    uint32_t word = __atomic_load_n(&state->decoded[index >> 5], __ATOMIC_RELAXED);
    __atomic_store_n(&state->decoded[index >> 5], on ? word | bit : word & ~bit, __ATOMIC_RELEASE);
}

// One chunk of the image: decode it into a scratch buffer, check it, reverse the filter of each of
// its extents and apply their relocations, then copy the extents into place. A cold chunk is
// committed and copied while the process's other threads are suspended, readable and writable but
// not executable, and only then made execute-read.
bool load_chunk(Byte *base, const Api &calls, const cdp::Header *header, const Native &old,
                uint32_t index, uint32_t slot) {
    const cdp::Chunk &chunk = chunk_table(header)[index];
    const cdp::Extent *extents = extent_table(header);
    Byte *image = image_of(base);
    auto *model = reinterpret_cast<cdp::Models *>(base + cdp_anchor.workspace + 4096 +
                                                  slot * cdp::workspace_slot);
    size_t bytes = (size_t(chunk.output_bytes) + 4095) & ~size_t(4095);
    void *scratch = nullptr;
    if (!calls.allocate || calls.allocate(self_process, &scratch, 0, &bytes, 0x3000, 4) < 0 || !scratch)
        return false;
    auto *output = static_cast<Byte *>(scratch);
    bool ok = cdp_decode(reinterpret_cast<const Byte *>(header) + header->compressed_offset + chunk.input,
                         chunk.input_bytes, output, chunk.output_bytes, model,
                         cdp::workspace_slot) == cdp::DecodeStatus::ok &&
              cdp::adler32(output, chunk.output_bytes) == chunk.adler;
    if (ok) {
        uint32_t at = 0;
        for (uint32_t i = 0; i < header->extent_count; ++i) {
            const cdp::Extent &piece = extents[i];
            if (piece.chunk != index)
                continue;
            const uint32_t rva = header->memory_rva + piece.offset;
            if (code_section(old, rva))
                cdp::filter_code(output + at, piece.bytes, rva, true);
            at += piece.bytes;
        }
        State *state = state_of(base);
        // A lazy chunk decoded at start-up, when the fault path is unavailable, is an eager one.
        const bool cold = index >= state->eager;
        // An eager chunk's fixups wait for the start-up pass, which runs once the whole table is
        // decoded; a cold chunk applies its own in the scratch, with the table already in place.
        if (cold)
            relocate(image, calls, header, old, index, output, state->eager);
        if (cold) {
            suspend_others(calls, state);
            for (uint32_t i = 0; i < header->extent_count; ++i)
                if (extents[i].chunk == index &&
                    !commit(calls, image + header->memory_rva + extents[i].offset, extents[i].bytes))
                    fail(calls, 42);
        }
        at = 0;
        for (uint32_t i = 0; i < header->extent_count; ++i) {
            const cdp::Extent &piece = extents[i];
            if (piece.chunk != index)
                continue;
            copy_bytes(image + header->memory_rva + piece.offset, output + at, piece.bytes);
            at += piece.bytes;
        }
        if (cold) {
            for (uint32_t i = 0; i < header->extent_count; ++i)
                if (extents[i].chunk == index)
                    permission(calls, image + header->memory_rva + extents[i].offset, extents[i].bytes, 0x20);
            for (uint32_t i = 0; i < header->extent_count; ++i)
                if (extents[i].chunk == index &&
                    (!calls.flush ||
                     calls.flush(self_process, image + header->memory_rva + extents[i].offset,
                                 extents[i].bytes) < 0))
                    ok = false;
            resume_all(calls, state);
        }
    }
    size_t released = 0;
    if (calls.free)
        calls.free(self_process, &scratch, &released, 0x8000);
    return ok;
}

// Takes eager chunks until none is left. Any failure stops every thread at its next chunk.
void take_chunks(Byte *base, const Api &calls, const cdp::Header *header, const Native &old,
                 uint32_t slot) {
    State *state = state_of(base);
    for (;;) {
        if (__atomic_load_n(&state->failed, __ATOMIC_RELAXED))
            return;
        const uint32_t index = __atomic_fetch_add(&state->next_chunk, 1, __ATOMIC_RELAXED);
        if (index >= state->eager)
            return;
        if (!load_chunk(base, calls, header, old, index, slot))
            __atomic_store_n(&state->failed, 1, __ATOMIC_RELAXED);
    }
}

// A decoding thread's start routine. It runs only this loader's code: its thread attach and
// detach reach cdp_tls while initialization is still in progress, so no native callback runs.
Status chunk_worker(void *slot) {
    Byte *base = image_base();
    const Api calls = api();
    const auto *header = metadata(base, calls);
    take_chunks(base, calls, header, native(calls, header), uint32_t(uintptr_t(slot)));
    calls.exit_thread(0);
    __builtin_trap();
}

// Checks the chunk and extent tables before any is used: eager chunks first, inputs contiguous,
// extents page-aligned, sorted, non-overlapping, each chunk's extents adding up to its output.
// Cold chunks need a detached image. Returns how many chunks decode at start-up.
uint32_t validate_tables(const Api &calls, const cdp::Header *header, bool dll) {
    const cdp::Chunk *chunks = chunk_table(header);
    const cdp::Extent *extents = extent_table(header);
    uint32_t input = 0, cursor = 0, totals[cdp::max_chunks], eager = 0;
    for (uint32_t i = 0; i < cdp::max_chunks; ++i)
        totals[i] = 0;
    bool cold = false;
    for (uint32_t i = 0; i < header->chunk_count; ++i) {
        if ((chunks[i].flags & ~cdp::lazy) || !chunks[i].output_bytes || !chunks[i].input_bytes ||
            chunks[i].input != input || !extent(input, chunks[i].input_bytes, header->compressed_bytes))
            fail(calls, 36);
        input += chunks[i].input_bytes;
        if (chunks[i].flags & cdp::lazy) {
            cold = true;
        } else if (cold) {
            fail(calls, 36);
        } else {
            ++eager;
        }
    }
    if (input != header->compressed_bytes || ((cold || (header->flags & cdp::detached)) &&
                                              (dll || !(header->flags & cdp::detached))))
        fail(calls, 36);
    for (uint32_t i = 0; i < header->extent_count; ++i) {
        const cdp::Extent &piece = extents[i];
        if (!piece.bytes || piece.bytes % 4096 || piece.offset % 4096 || piece.offset < cursor ||
            !extent(piece.offset, piece.bytes, header->memory_bytes) ||
            piece.chunk >= header->chunk_count)
            fail(calls, 36);
        cursor = piece.offset + piece.bytes;
        totals[piece.chunk] += piece.bytes;
    }
    for (uint32_t i = 0; i < header->chunk_count; ++i)
        if (totals[i] != chunks[i].output_bytes)
            fail(calls, 36);
    return eager;
}

// Damage in a cold chunk must stop the process before native code runs, so every stream is
// checksummed up front; the decoded bytes are checked again when a chunk is decoded.
void check_stream(const Api &calls, const cdp::Header *header) {
    const cdp::Chunk *chunks = chunk_table(header);
    const Byte *stream = reinterpret_cast<const Byte *>(header) + header->compressed_offset;
    for (uint32_t i = 0; i < header->chunk_count; ++i)
        if (cdp::adler32(stream + chunks[i].input, chunks[i].input_bytes) != chunks[i].input_adler)
            fail(calls, 38);
}

// Decodes chunks [0, eager) on this thread and up to one worker per processor.
void decode_eager(Byte *base, const Api &calls, const cdp::Header *header, const Native &old,
                  bool parallel, uint32_t eager) {
    State *state = state_of(base);
    state->eager = eager;
    state->next_chunk = 0;
    state->failed = 0;
    const uint32_t processors = cdp::read32(peb() + 0xb8);
    uint32_t workers = eager < processors ? eager : processors;
    workers = parallel && workers > 1 && calls.create_thread && calls.wait && calls.close && calls.exit_thread
                  ? workers - 1
                  : 0;
    void *threads[cdp::max_chunks];
    for (uint32_t i = 0; i < cdp::max_chunks; ++i)
        threads[i] = nullptr;
    uint32_t started = 0;
    for (uint32_t i = 0; i < workers; ++i) {
        // A thread that cannot be created leaves its chunks to the others.
        if (calls.create_thread(self_process, nullptr, 0, 0, 0, 0, chunk_worker,
                                reinterpret_cast<void *>(uintptr_t(started + 1)), &threads[started],
                                nullptr) >= 0 &&
            threads[started])
            ++started;
    }
    take_chunks(base, calls, header, old, 0);
    for (uint32_t i = 0; i < started; ++i) {
        if (calls.wait(threads[i], 0, nullptr) < 0)
            fail(calls, 37);
        calls.close(threads[i]);
    }
    if (state->failed || state->next_chunk < eager)
        fail(calls, 23);
}

// Whether any suspended thread is executing inside cold chunk `index`, or `caller` returns into it.
// Their instruction pointers were captured into the cold scratch after the suspension.
bool chunk_busy(Byte *base, const cdp::Header *header, uint32_t index, uintptr_t caller) {
    State *state = state_of(base);
    const auto *rips = reinterpret_cast<const uint64_t *>(cold_scratch(base) + 2048);
    const uintptr_t start = reinterpret_cast<uintptr_t>(image_of(base)) + header->memory_rva;
    for (uint32_t i = 0; i <= state->suspended; ++i) {
        const uintptr_t at = i < state->suspended ? rips[i] : caller;
        if (at - start < header->memory_bytes && extent_owner(header, uint32_t(at - start)) == index)
            return true;
    }
    return false;
}

void capture_rips(const Api &calls, Byte *base) {
    State *state = state_of(base);
    Byte *context = cold_scratch(base);  // page-aligned, as CONTEXT must be 16-byte aligned
    auto *rips = reinterpret_cast<uint64_t *>(context + 2048);
    for (uint32_t i = 0; i < state->suspended; ++i) {
        cdp::write32(context + 0x30, 0x100001);  // CONTEXT_AMD64 | CONTEXT_CONTROL
        // A context that cannot be read protects no chunk. Eviction stays correct: a thread running
        // in an evicted chunk faults it straight back in.
        rips[i] = calls.get_context && calls.get_context(state->threads[i], context) >= 0
                      ? cdp::read64(context + 0xf8)
                      : 0;
    }
}

// Whether any page of cold chunk `index` is in the working set (MemoryWorkingSetExInformation),
// asked in batches through the cold scratch past the instruction pointers. Unknown counts as yes.
bool chunk_resident(Byte *base, const Api &calls, const cdp::Header *header, uint32_t index) {
    if (!calls.query_memory)
        return true;
    const cdp::Extent *extents = extent_table(header);
    auto *pages = reinterpret_cast<uint64_t *>(cold_scratch(base) + 8192);
    constexpr uint32_t batch = (cdp::workspace_slot - 8192) / 16;
    Byte *image = image_of(base) + header->memory_rva;
    for (uint32_t i = 0; i < header->extent_count; ++i) {
        if (extents[i].chunk != index)
            continue;
        for (uint32_t done = 0; done < extents[i].bytes / 4096;) {
            uint32_t count = extents[i].bytes / 4096 - done;
            count = count < batch ? count : batch;
            for (uint32_t j = 0; j < count; ++j) {
                pages[j * 2] = reinterpret_cast<uintptr_t>(image + extents[i].offset) + uint64_t(done + j) * 4096;
                pages[j * 2 + 1] = 0;
            }
            size_t returned = 0;
            if (calls.query_memory(self_process, nullptr, 4, pages, size_t(count) * 16, &returned) < 0)
                return true;
            for (uint32_t j = 0; j < count; ++j)
                if (pages[j * 2 + 1] & 1)
                    return true;
            done += count;
        }
    }
    return false;
}

// Decommits cold chunk `index`; the lock is held and the other threads are suspended.
void evict_chunk(Byte *base, const Api &calls, const cdp::Header *header, uint32_t index) {
    State *state = state_of(base);
    Byte *image = image_of(base);
    const cdp::Extent *extents = extent_table(header);
    for (uint32_t i = 0; i < header->extent_count; ++i) {
        if (extents[i].chunk != index)
            continue;
        void *address = image + header->memory_rva + extents[i].offset;
        size_t bytes = extents[i].bytes;
        if (!calls.free || calls.free(self_process, &address, &bytes, 0x4000) < 0)
            fail(calls, 43);
    }
    set_decoded(state, index, false);
    --state->resident;
    ++state->evictions;
}

// Keeps at most `cap` cold chunks committed: decommits the ones decoded longest ago, never
// `keep` or one in use. The lock is held.
void enforce_cap(Byte *base, const Api &calls, const cdp::Header *header, uint32_t keep) {
    State *state = state_of(base);
    if (!state->cap || state->resident <= state->cap)
        return;
    suspend_others(calls, state);
    capture_rips(calls, base);
    while (state->resident > state->cap) {
        uint32_t oldest = UINT32_MAX;
        for (uint32_t i = state->eager; i < header->chunk_count; ++i)
            if (i != keep && is_decoded(state, i) && !chunk_busy(base, header, i, 0) &&
                (oldest == UINT32_MAX || state->used[i] < state->used[oldest]))
                oldest = i;
        if (oldest == UINT32_MAX)
            break;
        evict_chunk(base, calls, header, oldest);
    }
    resume_all(calls, state);
}

// Decodes a cold chunk. A thread that faults on a chunk another is already decoding waits for it.
void decode_cold(Byte *base, const Api &calls, const cdp::Header *header, uint32_t index) {
    State *state = state_of(base);
    lock_cold(calls, state);
    bool ok = is_decoded(state, index);
    if (!ok) {
        ok = load_chunk(base, calls, header, native(calls, header), index, 0);
        if (ok) {
            set_decoded(state, index, true);
            ++state->resident;
            ++state->decodes;
            ++state->count[index];
            state->used[index] = ++state->clock;
            enforce_cap(base, calls, header, index);
        }
    }
    unlock_cold(calls, state);
    if (!ok)
        fail(calls, 23);
}

struct ExceptionRecord {
    uint32_t code, flags;
    void *record;
    void *address;
    uint32_t parameters, pad;
    void *information[15];
};
struct ExceptionPointers {
    ExceptionRecord *record;
    void *context;
};

// The cold chunk holding `address`, or UINT32_MAX.
uint32_t cold_chunk(Byte *base, const cdp::Header *header, uintptr_t address) {
    const auto start = reinterpret_cast<uintptr_t>(image_of(base)) + header->memory_rva;
    if (address - start >= header->memory_bytes)
        return UINT32_MAX;
    const uint32_t index = extent_owner(header, uint32_t(address - start));
    return index != UINT32_MAX && index >= state_of(base)->eager ? index : UINT32_MAX;
}

// A cold page starts uncommitted, so its first touch faults, and faults again after an eviction.
// This handler decodes the chunk and lets the instruction run again, before any other handler or
// debugger sees the fault. A read or fetch that finds its chunk decoded raced another thread's
// decode and runs again; a write to a decoded chunk is a real fault, since code is never writable,
// and is passed on.
int32_t cold_fault(ExceptionPointers *info) {
    if (!info || !info->record || info->record->code != 0xC0000005 || info->record->parameters < 2)
        return 0;  // EXCEPTION_CONTINUE_SEARCH
    Byte *base = image_base();
    const Api calls = api();
    const auto *header = metadata(base, calls);
    State *state = state_of(base);
    const uint32_t index = cold_chunk(base, header, reinterpret_cast<uintptr_t>(info->record->information[1]));
    if (index == UINT32_MAX)
        return 0;
    if (is_decoded(state, index))
        return info->record->information[0] == reinterpret_cast<void *>(1) ? 0 : -1;
    // A fault inside the decode itself is a real fault: waiting would hang instead of crashing.
    if (state->lock && state->owner == uint32_t(current_thread()))
        return 0;
    decode_cold(base, calls, header, index);
    return -1;  // EXCEPTION_CONTINUE_EXECUTION
}

// Hook::evict. Its own return address protects the caller's chunk.
__attribute__((noinline)) uint32_t cdp_cold_evict(const void *address, uint32_t flags) {
    const auto caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    Byte *base = image_base();
    State *state = state_of(base);
    if (state->phase != 2 || !state->native || !state->event)
        return 0;
    const Api calls = api();
    const auto *header = metadata(base, calls);
    const uint32_t only = address ? cold_chunk(base, header, reinterpret_cast<uintptr_t>(address)) : UINT32_MAX;
    if (address && only == UINT32_MAX)
        return 0;
    lock_cold(calls, state);
    uint32_t evicted = 0;
    if (state->resident) {
        suspend_others(calls, state);
        capture_rips(calls, base);
        for (uint32_t i = state->eager; i < header->chunk_count; ++i)
            if ((only == UINT32_MAX || i == only) && is_decoded(state, i) &&
                !chunk_busy(base, header, i, caller) &&
                !((flags & cdp::evict_idle) && chunk_resident(base, calls, header, i))) {
                evict_chunk(base, calls, header, i);
                ++evicted;
            }
        resume_all(calls, state);
    }
    unlock_cold(calls, state);
    return evicted;
}

uint32_t cdp_cold_decodes(const void *address) {
    Byte *base = image_base();
    State *state = state_of(base);
    if (state->phase != 2)
        return UINT32_MAX;
    const Api calls = api();
    const uint32_t index = cold_chunk(base, metadata(base, calls), reinterpret_cast<uintptr_t>(address));
    return index == UINT32_MAX ? UINT32_MAX : __atomic_load_n(&state->count[index], __ATOMIC_RELAXED);
}

void cdp_cold_status(cdp::ColdStatus *out) {
    Byte *base = image_base();
    State *state = state_of(base);
    const Api calls = api();
    const auto *header = metadata(base, calls);
    if (state->event)
        lock_cold(calls, state);
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < header->extent_count; ++i) {
        const cdp::Extent &piece = extent_table(header)[i];
        if (lazy(header, piece.chunk) && is_decoded(state, piece.chunk))
            bytes += piece.bytes;
    }
    out->flags = header->flags;
    out->cold_chunks = header->chunk_count - state->eager;
    out->resident = state->resident;
    out->cap = state->cap;
    out->decodes = state->decodes;
    out->evictions = state->evictions;
    out->suspended = state->last_suspended;
    out->pad = 0;
    out->resident_bytes = bytes;
    out->stub = base;
    out->native = image_of(base);
    if (state->event)
        unlock_cold(calls, state);
}

// --- Telling Windows the restored image is the program --------------------------------------------
// Native code finds its module through PEB::ImageBaseAddress (GetModuleHandle(nullptr), resources),
// the loader's module entry (GetModuleFileName of its handle) and ntdll's inverted function table
// (RtlPcToFileHeader, which C++ throw and GetModuleHandleEx(FROM_ADDRESS) use). Only the last can
// resolve an address outside every mapped image, so each is pointed at the native image. Their
// layouts are checked before anything is written, and the result is checked afterwards.

struct Node {
    Node *child[2];
    uintptr_t parent;  // low two bits are flags
};
struct Tree {
    Node *root, *min;
};
constexpr uint32_t entry_base = 0x30, entry_point = 0x38, entry_node = 0xc8;

Byte *entry_of(Node *node) { return reinterpret_cast<Byte *>(node) - entry_node; }
Byte *key_of(Node *node) { return *reinterpret_cast<Byte **>(entry_of(node) + entry_base); }

// A writable ntdll section by name, as [start, end).
bool section(Byte *module, const char *name, Byte **start, Byte **end) {
    const Byte *pe = module + cdp::read32(module + 60);
    const Byte *table = pe + 24 + cdp::read16(pe + 20);
    for (unsigned i = 0; i < cdp::read16(pe + 6); ++i) {
        const Byte *s = table + i * 40;
        unsigned j = 0;
        while (j < 8 && name[j] && s[j] == uint8_t(name[j]))
            ++j;
        if (j < 8 && (name[j] || s[j]))
            continue;
        *start = module + cdp::read32(s + 12);
        *end = *start + cdp::read32(s + 8);
        return true;
    }
    return false;
}

// Moves the program's module entry to `image` in ntdll's base-address tree.
bool rekey_module(const Api &calls, Byte *entry, Byte *stub, Byte *image) {
    if (!calls.tree_remove || !calls.tree_insert || *reinterpret_cast<Byte **>(entry + entry_base) != stub)
        return false;
    Node *node = reinterpret_cast<Node *>(entry + entry_node), *root = node;
    for (unsigned i = 0; i < 128 && (root->parent & ~uintptr_t(3)); ++i)
        root = reinterpret_cast<Node *>(root->parent & ~uintptr_t(3));
    if (root->parent & ~uintptr_t(3))
        return false;
    // The node must be where a search for its key leads, or the offset is not this layout's.
    Node *at = root;
    for (unsigned i = 0; i < 128 && at && at != node; ++i)
        at = at->child[stub > key_of(at)];
    Node *min = root;
    while (min->child[0])
        min = min->child[0];
    Byte *start, *end;
    Tree *tree = nullptr;
    if (at != node || !section(calls.ntdll, ".data", &start, &end))
        return false;
    for (Byte *p = start; p + sizeof(Tree) <= end; p += 8) {
        auto *candidate = reinterpret_cast<Tree *>(p);
        if (candidate->root == root && candidate->min == min) {
            if (tree)
                return false;
            tree = candidate;
        }
    }
    if (!tree)
        return false;
    calls.tree_remove(tree, node);
    *reinterpret_cast<Byte **>(entry + entry_base) = image;
    Node *parent = tree->root;
    uint8_t right = 0;
    for (Node *cursor = parent; cursor;) {
        parent = cursor;
        right = image > key_of(cursor);
        cursor = cursor->child[right];
    }
    calls.tree_insert(tree, parent, right, node);
    return true;
}

struct InvertedEntry {
    void *table;
    Byte *base;
    uint32_t size, table_bytes;
};
struct Inverted {
    uint32_t count, maximum;
    uint32_t epoch, overflow;
    InvertedEntry entries[1];
};

// Adds the native image to ntdll's inverted function table, sorted after ntdll's own first entry.
bool invert(const Api &calls, Byte *stub, Byte *image, uint32_t size, Byte *table, uint32_t table_bytes) {
    Byte *start, *end;
    if (!section(calls.ntdll, ".mrdata", &start, &end))
        return false;
    const uint32_t stub_size = image_size(stub);
    Inverted *found = nullptr;
    for (Byte *p = start; p + 16 + sizeof(InvertedEntry) <= end; p += 8) {
        auto *candidate = reinterpret_cast<Inverted *>(p);
        if (!candidate->count || candidate->count > candidate->maximum || candidate->maximum > 4096 ||
            size_t(end - p) < 16 + size_t(candidate->maximum) * sizeof(InvertedEntry) ||
            candidate->entries[0].base != calls.ntdll)
            continue;
        bool ours = false, sorted = true;
        for (uint32_t i = 0; i < candidate->count; ++i) {
            ours = ours || (candidate->entries[i].base == stub && candidate->entries[i].size == stub_size);
            sorted = sorted && (i < 2 || candidate->entries[i - 1].base < candidate->entries[i].base);
        }
        if (ours && sorted) {
            if (found)
                return false;
            found = candidate;
        }
    }
    if (!found || found->count >= found->maximum)
        return false;
    uint32_t at = 1;
    while (at < found->count && found->entries[at].base < image)
        ++at;
    void *address = found;
    size_t bytes = 16 + size_t(found->maximum) * sizeof(InvertedEntry);
    uint32_t previous = 0;
    if (calls.protect(self_process, &address, &bytes, 4, &previous) < 0)
        return false;
    __atomic_fetch_add(&found->epoch, 1, __ATOMIC_SEQ_CST);
    for (uint32_t i = found->count; i > at; --i)
        found->entries[i] = found->entries[i - 1];
    found->entries[at] = {table, image, size, table_bytes};
    __atomic_store_n(&found->count, found->count + 1, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&found->epoch, 1, __ATOMIC_SEQ_CST);
    address = found;
    bytes = 16 + size_t(found->maximum) * sizeof(InvertedEntry);
    uint32_t ignored;
    calls.protect(self_process, &address, &bytes, previous, &ignored);
    return true;
}

// Every other thread is suspended and the loader lock is held while ntdll's structures change.
void adopt(const Api &calls, Byte *stub, Byte *image, const Native &old) {
    State *state = state_of(stub);
    if (!calls.lock_loader || !calls.unlock_loader || !calls.pc_to_header)
        fail(calls, 44);
    void *cookie = nullptr;
    uint32_t disposition = 0;
    if (calls.lock_loader(0, &disposition, &cookie) < 0)
        fail(calls, 44);
    suspend_others(calls, state);
    Byte *process = peb();
    // PEB::Ldr, then InLoadOrderModuleList's first entry: the program's own.
    Byte *entry = *reinterpret_cast<Byte **>(*reinterpret_cast<Byte **>(process + 0x18) + 0x10);
    const bool ok = *reinterpret_cast<Byte **>(process + 0x10) == stub &&
                    invert(calls, stub, image, old.size, image + old.directory(3), old.directory_bytes(3)) &&
                    rekey_module(calls, entry, stub, image);
    if (ok) {
        *reinterpret_cast<Byte **>(entry + entry_point) = image + cdp::read32(old.optional + 16);
        *reinterpret_cast<Byte **>(process + 0x10) = image;
    }
    resume_all(calls, state);
    calls.unlock_loader(0, cookie);
    void *module = nullptr;
    if (!ok || calls.pc_to_header(image + old.directory(5), &module) != image || module != image)
        fail(calls, 45);
}

// A detached image's static TLS. Windows built its template, and every thread's copy so far, from
// the packed TLS directory, whose pointers it relocated for the packed image; they must point into
// the native one. Each DIR64 fixup inside the native template is moved by the distance between the
// two, first in the template and then in every existing thread's block that still holds the old
// value, so a thread created in between is never moved twice.
void move_tls(const Api &calls, Byte *stub, Byte *image, const Native &old) {
    const Byte *optional = stub + cdp::read32(stub + 60) + 24;
    const uint32_t packed_tls = cdp::read32(optional + 112 + 9 * 8);
    if (!old.directory(9) || !packed_tls)
        return;
    Byte *packed = reinterpret_cast<Byte *>(cdp::read64(stub + packed_tls));
    const uint64_t length = cdp::read64(stub + packed_tls + 8) - cdp::read64(stub + packed_tls);
    const uint64_t native_start = cdp::read64(image + old.directory(9)) - reinterpret_cast<uintptr_t>(image);
    if (length > 65536 || native_start > old.size || length > old.size - native_start)
        fail(calls, 46);
    const uint64_t delta = uint64_t(reinterpret_cast<uintptr_t>(image) - reinterpret_cast<uintptr_t>(stub));
    State *state = state_of(stub);
    uint32_t index = state->tls_slot;
    for (int pass = 0; pass < 2; ++pass) {
        if (pass == 1)
            suspend_others(calls, state);
        uint32_t blocks = pass == 0 ? 1 : state->suspended + 1;
        for (uint32_t b = 0; b < blocks; ++b) {
            Byte *block = packed;
            if (pass == 1) {
                Byte *teb = nullptr;
                if (b == state->suspended) {
                    __asm__("movq %%gs:0x30,%0" : "=r"(teb));
                } else {
                    ThreadBasic info;
                    uint32_t bytes = 0;
                    if (calls.query_thread(state->threads[b], 0, &info, sizeof(info), &bytes) < 0)
                        continue;
                    teb = info.teb;
                }
                Byte **slots = teb ? *reinterpret_cast<Byte ***>(teb + 0x58) : nullptr;
                block = slots ? slots[index] : nullptr;
                if (!block)
                    continue;
            }
            uint32_t at = old.directory(5), end = at + old.directory_bytes(5);
            void *address = packed;
            size_t bytes = size_t(length);
            uint32_t previous = 0;
            if (pass == 0 && length && calls.protect(self_process, &address, &bytes, 4, &previous) < 0)
                fail(calls, 46);
            while (at + 8 <= end) {
                const uint32_t page = cdp::read32(image + at), size = cdp::read32(image + at + 4);
                for (uint32_t i = 8; i < size; i += 2) {
                    const uint16_t item = cdp::read16(image + at + i);
                    const uint64_t target = page + (item & 4095);
                    if (!(item >> 12) || target < native_start || target + 8 > native_start + length)
                        continue;
                    Byte *slot = block + (target - native_start);
                    const uint64_t value = cdp::read64(slot);
                    // The template holds the new value by now; a block still holding the old one moves.
                    if (pass == 0 || value + delta == cdp::read64(packed + (target - native_start)))
                        cdp::write64(slot, value + delta);
                }
                at += size;
            }
            if (pass == 0 && length) {
                address = packed;
                bytes = size_t(length);
                uint32_t ignored;
                calls.protect(self_process, &address, &bytes, previous, &ignored);
            }
        }
        if (pass == 1)
            resume_all(calls, state);
    }
}

// Applies `protection` to [rva, rva + bytes) of the native image except its cold chunks, which are
// not committed.
void protect_resident(const Api &calls, const cdp::Header *header, Byte *image, uint32_t eager, uint32_t rva,
                      uint32_t bytes, uint32_t protection) {
    const cdp::Extent *extents = extent_table(header);
    uint32_t cursor = rva;
    const uint32_t end = rva + bytes;
    for (uint32_t i = 0; i < header->extent_count && cursor < end; ++i) {
        const uint32_t low = header->memory_rva + extents[i].offset, high = low + extents[i].bytes;
        if (extents[i].chunk < eager || high <= cursor)
            continue;
        if (low >= end)
            break;
        if (low > cursor)
            permission(calls, image + cursor, low - cursor, protection);
        cursor = high;
    }
    if (cursor < end)
        permission(calls, image + cursor, end - cursor, protection);
}

// A detached image's native memory: reserved whole, as large as the packed image so every address
// Windows' module entry claims is ours, and committed except the cold chunks.
Byte *reserve_native(const Api &calls, const cdp::Header *header, Byte *stub, const Native &old, bool cold) {
    size_t bytes = image_size(stub) > old.size ? image_size(stub) : old.size;
    void *address = nullptr;
    if (!calls.allocate || calls.allocate(self_process, &address, 0, &bytes, 0x2000, 1) < 0 || !address)
        fail(calls, 47);
    Byte *image = static_cast<Byte *>(address);
    const cdp::Extent *extents = extent_table(header);
    uint32_t cursor = 0;
    for (uint32_t i = 0; i < header->extent_count; ++i) {
        const uint32_t low = header->memory_rva + extents[i].offset;
        if (cold && lazy(header, extents[i].chunk)) {
            if (!commit(calls, image + cursor, low - cursor))
                fail(calls, 47);
            cursor = low + extents[i].bytes;
        }
    }
    if (!commit(calls, image + cursor, old.size - cursor))
        fail(calls, 47);
    copy_bytes(image, reinterpret_cast<const Byte *>(header + 1), header->native_header_bytes);
    return image;
}

// Returns true when this call restored the image, false when it was already restored.
bool initialize(Byte *base, const Api &calls, const cdp::Header *header, const Native &old,
                bool parallel) {
    State *state = state_of(base);
    if (state->phase == 2)
        return false;
    if (state->phase)
        fail(calls, 19);
    state->phase = 1;
    if (old.size != header->memory_rva + header->memory_bytes)
        fail(calls, 21);
    const bool detached = (header->flags & cdp::detached) != 0;
    uint32_t tls_index = 0;
    if (header->tls_index_rva) {
        if (!extent(header->tls_index_rva, 4, old.size))
            fail(calls, 22);
        tls_index = detached ? state->tls_slot : cdp::read32(base + header->tls_index_rva);
    }
    for (unsigned i = 0; i < old.count; ++i) {
        const Byte *section = old.sections + i * 40;
        const uint32_t rva = cdp::read32(section + 12), raw = cdp::read32(section + 16);
        if (rva < header->memory_rva || !extent(rva, raw, old.size))
            fail(calls, 24);
    }
    const uint32_t eager = validate_tables(calls, header, old.dll);
    check_stream(calls, header);
    // The fault path needs several system calls; without them every chunk decodes here, which is
    // still correct but gives up the memory the cold pages save.
    const bool cold = eager < header->chunk_count;
    const bool able = calls.allocate && calls.free && calls.add_handler && calls.suspend &&
                      calls.resume && calls.next_thread && calls.query_thread && calls.create_event &&
                      calls.wait && calls.signal_event && calls.reset_event && calls.close;
    if (cold && able && !state->event &&
        calls.create_event(&state->event, 0x1f0003, nullptr, 0, 0) < 0)
        fail(calls, 39);
    if (detached)
        state->native = reserve_native(calls, header, base, old, cold && able);
    Byte *image = image_of(base);
    state->cap = header->cold_cap;
    decode_eager(base, calls, header, old, parallel, cold && able ? eager : header->chunk_count);
    size_t cursor = sizeof(cdp::Header) + header->native_header_bytes;
    for (unsigned i = 0; i < header->gap_count; ++i) {
        if (cursor > header->metadata_bytes || header->metadata_bytes - cursor < 8)
            fail(calls, 25);
        const uint32_t bytes = cdp::read32(reinterpret_cast<const Byte *>(header) + cursor + 4);
        cursor += 8;
        if (bytes > header->metadata_bytes - cursor)
            fail(calls, 25);
        cursor += bytes;
    }
    if (cursor > header->metadata_bytes ||
        cursor + size_t(header->copy_count) * 12 + size_t(header->chunk_count) * sizeof(cdp::Chunk) +
                size_t(header->extent_count) * sizeof(cdp::Extent) !=
            header->metadata_bytes)
        fail(calls, 26);
    const auto *copies = reinterpret_cast<const Byte *>(header) + cursor;
    const uint32_t stub_size = image_size(base);
    for (unsigned i = 0; i < header->copy_count; ++i) {
        const uint32_t source = cdp::read32(copies + i * 12),
                       destination = cdp::read32(copies + i * 12 + 4);
        const uint32_t bytes = cdp::read32(copies + i * 12 + 8);
        // Sources are the packed image's own resources: past CDP0 when attached.
        if (destination < header->memory_rva || !extent(destination, bytes, old.size) ||
            (!detached && source < old.size) || !extent(source, bytes, stub_size))
            fail(calls, 27);
        for (uint32_t j = 0; j < bytes; ++j)
            image[destination + j] = base[source + j];
    }
    relocate(image, calls, header, old, UINT32_MAX, nullptr, state->eager);
    if (header->tls_index_rva)
        cdp::write32(image + header->tls_index_rva, tls_index);
    imports(image, calls, old);
    if (header->hook_rva) {
        if (!extent(header->hook_rva, sizeof(cdp::Hook), old.size) || header->hook_rva % 8)
            fail(calls, 48);
        auto *hook = reinterpret_cast<cdp::Hook *>(image + header->hook_rva);
        for (unsigned i = 0; i < 16; ++i)
            if (hook->magic[i] != cdp::hook_magic[i])
                fail(calls, 48);
        hook->version = cdp::hook_version;
        hook->bytes = sizeof(cdp::Hook);
        hook->evict = cdp_cold_evict;
        hook->decodes = cdp_cold_decodes;
        hook->status = cdp_cold_status;
    }
    if (old.directory_bytes(3)) {
        if (!calls.add_functions || old.directory_bytes(3) % 12 ||
            !extent(old.directory(3), old.directory_bytes(3), old.size) ||
            !calls.add_functions(image + old.directory(3), old.directory_bytes(3) / 12,
                                 reinterpret_cast<uintptr_t>(image)))
            fail(calls, 28);
    }
    if (detached) {
        move_tls(calls, base, image, old);
        adopt(calls, base, image, old);
        permission(calls, image, header->memory_rva, 2);
    }
    protect_resident(calls, header, image, state->eager, header->memory_rva, header->memory_bytes, 2);
    for (unsigned i = 0; i < old.count; ++i) {
        const Byte *section = old.sections + i * 40;
        const uint32_t rva = cdp::read32(section + 12), flags = cdp::read32(section + 36);
        const uint32_t virtual_bytes = cdp::read32(section + 8), raw = cdp::read32(section + 16);
        const size_t bytes =
            (size_t(virtual_bytes > raw ? virtual_bytes : raw) + 4095) & ~size_t(4095);
        if (!extent(rva, bytes, old.size) || ((flags & 0xa0000000) == 0xa0000000))
            fail(calls, 29);
        protect_resident(calls, header, image, state->eager, rva, uint32_t(bytes),
                         flags & 0x20000000   ? 0x20
                         : flags & 0x80000000 ? 4
                                              : 2);
    }
    // The handler must exist before a cold page can run. The pages are not committed, so their
    // first touch faults whatever kind of access it is.
    if (cold && able && !calls.add_handler(1, reinterpret_cast<void *>(&cold_fault)))
        fail(calls, 39);
    state->phase = 2;
    // A cold chunk reuses the first model slot when it decodes, so the slots stay writable then.
    if (!cold || !able)
        permission(calls, base + cdp_anchor.workspace + 4096, header->chunk_count * cdp::workspace_slot, 1);
    if (!calls.flush || calls.flush(self_process, image, old.size) < 0)
        fail(calls, 30);
    return true;
}

void tls_callbacks(Byte *base, const Api &calls, const Native &old, uint32_t reason,
                   void *reserved) {
    if (!old.directory(9))
        return;
    if (!extent(old.directory(9), 40, old.size))
        fail(calls, 31);
    const uint64_t address = cdp::read64(base + old.directory(9) + 24);
    if (!address)
        return;
    const uint64_t rva = address - reinterpret_cast<uintptr_t>(base);
    if (rva > UINT32_MAX)
        fail(calls, 32);
    uint32_t at = uint32_t(rva);
    for (unsigned i = 0; i < 1024; ++i, at += 8) {
        if (!extent(at, 8, old.size))
            fail(calls, 32);
        const uint64_t callback = cdp::read64(base + at);
        if (!callback)
            return;
        if (callback < reinterpret_cast<uintptr_t>(base) ||
            callback - reinterpret_cast<uintptr_t>(base) >= old.size)
            fail(calls, 33);
        reinterpret_cast<Callback>(uintptr_t(callback))(base, reason, reserved);
    }
    fail(calls, 34);
}
} // namespace

// Windows holds every new thread until process initialization ends, so an EXE cannot wait for
// decoding threads in its process-attach TLS callback. It records the callback's argument and
// restores the image at the entry point instead, then runs the native process-attach callbacks
// there, still before any other native code. A DLL restores itself here, on this thread alone.
extern "C" __declspec(dllexport) void cdp_tls(void *, uint32_t reason, void *reserved) {
    Byte *base = image_base();
    const Api calls = api();
    const auto *header = metadata(base, calls);
    const Native old = native(calls, header);
    if (reason == 1) {
        if (old.dll)
            initialize(base, calls, header, old, false);
        else
            state_of(base)->reserved = reserved;
    }
    Byte *image = image_of(base);
    if (state_of(base)->phase == 2)
        tls_callbacks(image, calls, old, reason, reserved);
    if (reason == 0 && !reserved && old.directory_bytes(3) && calls.delete_functions)
        calls.delete_functions(image + old.directory(3));
}

extern "C" __declspec(dllexport) uint32_t cdp_entry(void *handle, uint32_t reason, void *reserved) {
    Byte *base = image_base();
    const Api calls = api();
    const auto *header = metadata(base, calls);
    const Native old = native(calls, header);
    if (initialize(base, calls, header, old, !old.dll) && !old.dll)
        tls_callbacks(image_of(base), calls, old, 1, state_of(base)->reserved);
    Byte *image = image_of(base);
    const uint32_t entry = cdp::read32(old.optional + 16);
    if (!entry)
        return 1;
    if (!extent(entry, 1, old.size))
        fail(calls, 35);
    using Entry = uint32_t (*)(void *, uint32_t, void *);
    return reinterpret_cast<Entry>(image + entry)(handle, reason, reserved);
}
