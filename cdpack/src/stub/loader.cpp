// CDP1 PE64 startup: decode into RW pages, restore Windows linkage, then make code executable.
// An EXE decodes its eager chunks on several threads at the entry point; a DLL, or an EXE on one
// core, decodes them in turn (spec.md, "Chunks and threads"). Code the profile marked cold stays
// inaccessible and is decoded by the vectored handler on its first fault (spec.md, "Code decoded
// on demand").
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

Byte *ntdll() {
    Byte *peb;
    __asm__("movq %%gs:0x60,%0" : "=r"(peb));
    Byte *loader = *reinterpret_cast<Byte **>(peb + 24);
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
};

Api api() {
    Byte *module = ntdll();
    if (!module)
        __builtin_trap();
    return {
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
        reinterpret_cast<Signal>(exported(module, name_hash("NtResetEvent")))};
}

[[noreturn]] void fail(const Api &calls, unsigned reason) {
    if (calls.terminate)
        calls.terminate(reinterpret_cast<void *>(intptr_t(-1)), Status(0xcd000000u | reason));
    __builtin_trap();
}

bool extent(uint32_t at, size_t bytes, uint32_t end) { return at <= end && bytes <= end - at; }

void permission(const Api &calls, Byte *at, size_t bytes, uint32_t protection) {
    void *address = at;
    uint32_t previous;
    if (bytes && (!calls.protect || calls.protect(reinterpret_cast<void *>(intptr_t(-1)), &address,
                                                  &bytes, protection, &previous) < 0))
        fail(calls, 20);
}

Byte *image_base() {
    return reinterpret_cast<Byte *>(const_cast<cdp::Anchor *>(&cdp_anchor)) - cdp_anchor.self;
}

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
    const auto *optional = base + cdp::read32(base + 60) + 24;
    const uint32_t size = cdp::read32(optional + 56);
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
    if (!header->memory_rva || header->memory_rva % 4096 || !header->memory_bytes ||
        header->memory_bytes > 128 * 1024 * 1024 ||
        !extent(header->memory_rva, header->memory_bytes, cdp_anchor.metadata) ||
        !extent(cdp_anchor.metadata + header->compressed_offset, header->compressed_bytes, size) ||
        header->compressed_offset < header->metadata_bytes ||
        !extent(sizeof(cdp::Header), header->native_header_bytes, header->metadata_bytes) ||
        header->metadata_bytes < sizeof(cdp::Header) + tables || header->copy_count > 4096 ||
        header->gap_count > 97)
        fail(calls, 7);
    return header;
}

// Applies the image's DIR64 fixups. `selected` is a chunk index when its scratch is being decoded,
// or UINT32_MAX at start-up, when every target outside a cold chunk is applied in the image.
void relocate(Byte *base, const Api &calls, const cdp::Header *header, const Native &old,
              uint32_t selected, Byte *scratch) {
    const uint64_t delta = uint64_t(reinterpret_cast<uintptr_t>(base)) - old.preferred;
    const cdp::Chunk *chunks = chunk_table(header);
    uint32_t at = old.directory(5), bytes = old.directory_bytes(5);
    if (!extent(at, bytes, old.size) || (delta && !bytes))
        fail(calls, 8);
    const uint32_t end = at + bytes;
    while (at < end) {
        if (!extent(at, 8, end))
            fail(calls, 9);
        const uint32_t page = cdp::read32(base + at), block = cdp::read32(base + at + 4);
        if (block < 8 || (block & 1) || !extent(at, block, end))
            fail(calls, 10);
        for (uint32_t i = 8; i < block; i += 2) {
            const uint16_t entry = cdp::read16(base + at + i);
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
                if (chunks[owner].flags & cdp::lazy)
                    continue;
                cdp::write64(base + target, cdp::read64(base + target) + delta);
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
// callback received, and what the threads decoding cold code share. The suspended thread handles
// live here too, off the stack of whichever thread faults.
struct State {
    uint32_t phase;  // 0 not started, 1 initializing, 2 done
    uint32_t next_chunk;
    uint32_t failed;
    uint32_t pad;
    void *reserved;
    uint32_t lock;        // 0 free, 1 held while one cold chunk is decoded
    uint32_t owner;       // thread that holds the lock, so a fault inside the decode passes on
    uint32_t eager;       // chunks [0, eager) decode at start-up; the rest are cold
    uint32_t decoded[2];  // bitset: cold chunks already copied into the image
    void *event;          // manual-reset; set whenever a cold decode ends
    uint32_t suspended;
    uint32_t pad2;
    void *threads[480];
};
static_assert(sizeof(State) <= 4096 && sizeof(cdp::Models) <= cdp::workspace_slot);

State *state_of(Byte *base) { return reinterpret_cast<State *>(base + cdp_anchor.workspace); }

uint64_t current_thread() {
    uint64_t id;
    __asm__("movq %%gs:0x48,%0" : "=r"(id));
    return id;
}

struct ThreadBasic {
    int32_t status;
    uint32_t pad;
    void *teb;
    void *process;
    uint64_t thread;
    uint64_t affinity;
    int32_t priority, base;
};

// Decoding cold code copies whole pages. Every other thread is suspended across the protection
// changes and the copy, so none can execute a half-written page or read one as data. A thread
// suspended inside the kernel finishes its current system call first, so a lock it holds there
// cannot block ours.
void suspend_others(const Api &calls, State *state) {
    state->suspended = 0;
    const uint64_t self = current_thread();
    void *key = nullptr;
    bool kept = false;
    for (unsigned i = 0; i < 8192; ++i) {
        void *thread = nullptr;
        const Status status = calls.next_thread(reinterpret_cast<void *>(intptr_t(-1)), key, 0x0800, 0,
                                                0, &thread);
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
        if (state->suspended >= 480)
            fail(calls, 40);
        state->threads[state->suspended++] = thread;
        kept = true;
    }
    if (key && !kept)
        calls.close(key);
}

void resume_all(const Api &calls, State *state) {
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

// One chunk of the image: decode it into a scratch buffer, check it, reverse the filter of each of
// its extents and apply their relocations, then copy the extents into place. A cold chunk is
// copied while the process's other threads are suspended, between making its pages writable and
// executable again.
bool load_chunk(Byte *base, const Api &calls, const cdp::Header *header, const Native &old,
                uint32_t index, uint32_t slot) {
    const cdp::Chunk &chunk = chunk_table(header)[index];
    const cdp::Extent *extents = extent_table(header);
    auto *model = reinterpret_cast<cdp::Models *>(base + cdp_anchor.workspace + 4096 +
                                                  slot * cdp::workspace_slot);
    size_t bytes = (size_t(chunk.output_bytes) + 4095) & ~size_t(4095);
    void *scratch = nullptr;
    if (!calls.allocate ||
        calls.allocate(reinterpret_cast<void *>(intptr_t(-1)), &scratch, 0, &bytes, 0x3000, 4) < 0 ||
        !scratch)
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
        const bool cold = (chunk.flags & cdp::lazy) != 0;
        // An eager chunk's fixups wait for the start-up pass, which runs once the whole table is
        // decoded; a cold chunk applies its own in the scratch, with the table already in place.
        if (cold)
            relocate(base, calls, header, old, index, output);
        State *state = state_of(base);
        if (cold) {
            suspend_others(calls, state);
            for (uint32_t i = 0; i < header->extent_count; ++i)
                if (extents[i].chunk == index)
                    permission(calls, base + header->memory_rva + extents[i].offset, extents[i].bytes, 4);
        }
        at = 0;
        for (uint32_t i = 0; i < header->extent_count; ++i) {
            const cdp::Extent &piece = extents[i];
            if (piece.chunk != index)
                continue;
            copy_bytes(base + header->memory_rva + piece.offset, output + at, piece.bytes);
            at += piece.bytes;
        }
        if (cold) {
            for (uint32_t i = 0; i < header->extent_count; ++i)
                if (extents[i].chunk == index)
                    permission(calls, base + header->memory_rva + extents[i].offset, extents[i].bytes, 0x20);
            for (uint32_t i = 0; i < header->extent_count; ++i)
                if (extents[i].chunk == index &&
                    (!calls.flush ||
                     calls.flush(reinterpret_cast<void *>(intptr_t(-1)),
                                 base + header->memory_rva + extents[i].offset, extents[i].bytes) < 0))
                    ok = false;
            resume_all(calls, state);
        }
    }
    size_t released = 0;
    if (calls.free)
        calls.free(reinterpret_cast<void *>(intptr_t(-1)), &scratch, &released, 0x8000);
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
// Returns how many chunks decode at start-up.
uint32_t validate_tables(const Api &calls, const cdp::Header *header) {
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
    if (input != header->compressed_bytes)
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
    Byte *peb;
    __asm__("movq %%gs:0x60,%0" : "=r"(peb));
    const uint32_t processors = cdp::read32(peb + 0xb8);
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
        if (calls.create_thread(reinterpret_cast<void *>(intptr_t(-1)), nullptr, 0, 0, 0, 0, chunk_worker,
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

// Decodes a cold chunk exactly once. A thread that faults on a chunk another is already decoding
// waits for it, and a fault on an already decoded chunk is none of ours.
void decode_cold(Byte *base, const Api &calls, const cdp::Header *header, uint32_t index) {
    State *state = state_of(base);
    lock_cold(calls, state);
    const uint32_t word = index >> 5, bit = 1u << (index & 31);
    bool ok = (__atomic_load_n(&state->decoded[word], __ATOMIC_RELAXED) & bit) != 0;
    if (!ok)
        ok = load_chunk(base, calls, header, native(calls, header), index, 0);
    if (ok)
        __atomic_store_n(&state->decoded[word],
                         __atomic_load_n(&state->decoded[word], __ATOMIC_RELAXED) | bit,
                         __ATOMIC_RELAXED);
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

// A cold page starts inaccessible, so its first touch faults. This handler decodes the chunk and
// lets the instruction run again, before any other handler or debugger sees the fault.
int32_t cold_fault(ExceptionPointers *info) {
    if (!info || !info->record || info->record->code != 0xC0000005 || info->record->parameters < 2)
        return 0;  // EXCEPTION_CONTINUE_SEARCH
    Byte *base = image_base();
    const Api calls = api();
    const auto *header = metadata(base, calls);
    const auto address = reinterpret_cast<uintptr_t>(info->record->information[1]);
    const auto start = reinterpret_cast<uintptr_t>(base);
    if (address < start || address - start < header->memory_rva)
        return 0;
    const uintptr_t offset = address - start - header->memory_rva;
    if (offset >= header->memory_bytes)
        return 0;
    const uint32_t index = extent_owner(header, uint32_t(offset));
    if (index == UINT32_MAX || !(chunk_table(header)[index].flags & cdp::lazy))
        return 0;
    // A fault inside the decode itself is a real fault: waiting would hang instead of crashing.
    const State *state = state_of(base);
    if (state->lock && state->owner == uint32_t(current_thread()))
        return 0;
    decode_cold(base, calls, header, index);
    return -1;  // EXCEPTION_CONTINUE_EXECUTION
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
    uint32_t tls_index = 0;
    if (header->tls_index_rva) {
        if (!extent(header->tls_index_rva, 4, old.size))
            fail(calls, 22);
        tls_index = cdp::read32(base + header->tls_index_rva);
    }
    auto *output = base + header->memory_rva;
    for (unsigned i = 0; i < old.count; ++i) {
        const Byte *section = old.sections + i * 40;
        const uint32_t rva = cdp::read32(section + 12), raw = cdp::read32(section + 16);
        if (rva < header->memory_rva || !extent(rva, raw, old.size))
            fail(calls, 24);
    }
    const uint32_t eager = validate_tables(calls, header);
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
    for (unsigned i = 0; i < header->copy_count; ++i) {
        const uint32_t source = cdp::read32(copies + i * 12),
                       destination = cdp::read32(copies + i * 12 + 4);
        const uint32_t bytes = cdp::read32(copies + i * 12 + 8);
        const auto *optional = base + cdp::read32(base + 60) + 24;
        if (destination < header->memory_rva || !extent(destination, bytes, old.size) ||
            source < old.size || !extent(source, bytes, cdp::read32(optional + 56)))
            fail(calls, 27);
        for (uint32_t j = 0; j < bytes; ++j)
            base[destination + j] = base[source + j];
    }
    relocate(base, calls, header, old, UINT32_MAX, nullptr);
    if (header->tls_index_rva)
        cdp::write32(base + header->tls_index_rva, tls_index);
    imports(base, calls, old);
    if (old.directory_bytes(3)) {
        if (!calls.add_functions || old.directory_bytes(3) % 12 ||
            !extent(old.directory(3), old.directory_bytes(3), old.size) ||
            !calls.add_functions(base + old.directory(3), old.directory_bytes(3) / 12,
                                 reinterpret_cast<uintptr_t>(base)))
            fail(calls, 28);
    }
    permission(calls, output, header->memory_bytes, 2);
    for (unsigned i = 0; i < old.count; ++i) {
        const Byte *section = old.sections + i * 40;
        const uint32_t rva = cdp::read32(section + 12), flags = cdp::read32(section + 36);
        const uint32_t virtual_bytes = cdp::read32(section + 8), raw = cdp::read32(section + 16);
        const size_t bytes =
            (size_t(virtual_bytes > raw ? virtual_bytes : raw) + 4095) & ~size_t(4095);
        if (!extent(rva, bytes, old.size) || ((flags & 0xa0000000) == 0xa0000000))
            fail(calls, 29);
        permission(calls, base + rva, bytes,
                   flags & 0x20000000   ? 0x20
                   : flags & 0x80000000 ? 4
                                        : 2);
    }
    if (cold && able) {
        // The handler must exist before a cold page can run, and the pages are taken away only
        // after every native section has its final protection.
        if (!calls.add_handler(1, reinterpret_cast<void *>(&cold_fault)))
            fail(calls, 39);
        for (uint32_t i = 0; i < header->extent_count; ++i) {
            const cdp::Extent &piece = extent_table(header)[i];
            if (chunk_table(header)[piece.chunk].flags & cdp::lazy)
                permission(calls, base + header->memory_rva + piece.offset, piece.bytes, 1);
        }
    }
    state->phase = 2;
    // A cold chunk reuses the first model slot when it decodes, so the slots stay writable then.
    if (!cold)
        permission(calls, base + cdp_anchor.workspace + 4096, header->chunk_count * cdp::workspace_slot, 1);
    if (!calls.flush ||
        calls.flush(reinterpret_cast<void *>(intptr_t(-1)), output, header->memory_bytes) < 0)
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
    if (state_of(base)->phase == 2)
        tls_callbacks(base, calls, old, reason, reserved);
    if (reason == 0 && !reserved && old.directory_bytes(3) && calls.delete_functions)
        calls.delete_functions(base + old.directory(3));
}

extern "C" __declspec(dllexport) uint32_t cdp_entry(void *handle, uint32_t reason, void *reserved) {
    Byte *base = image_base();
    const Api calls = api();
    const auto *header = metadata(base, calls);
    const Native old = native(calls, header);
    if (initialize(base, calls, header, old, !old.dll) && !old.dll)
        tls_callbacks(base, calls, old, 1, state_of(base)->reserved);
    const uint32_t entry = cdp::read32(old.optional + 16);
    if (!entry)
        return 1;
    if (!extent(entry, 1, old.size))
        fail(calls, 35);
    using Entry = uint32_t (*)(void *, uint32_t, void *);
    return reinterpret_cast<Entry>(base + entry)(handle, reason, reserved);
}
