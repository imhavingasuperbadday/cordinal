# CDP1: cordinal's independent PE64 packing format

Version 4, 2026-11-24, not yet released: an EXE with cold code is restored into private memory of
its own, so cold chunks can be decommitted and decoded again (see "Detached images"); the header
gains `flags`, `hook_rva` and `cold_cap`.
Version 3, 2026-11-09: chunks may leave out whole zero pages (see "Zero pages"), and code the
profile marks cold is decoded on first use (see "Code decoded on demand").
Version 2, 2026-11-04, was first used for the 0.1.5 release; version 1, 2026-10-23, had a single stream.

This specification is independent of UPX. Integer fields are little endian. Only AMD64 PE32+
images with 4096-byte section alignment and ordinary imports, resources, DIR64 relocations,
TLS, x64 unwind information and delay imports are accepted. Signed, managed and CFG-instrumented
images are rejected until those contracts have dedicated tests. Unsupported inputs fail before
writing an executable. The native subsystem and DLL characteristics are retained.
Delay imports need no loader support: the image's own helper reaches each descriptor through the
thunk that calls it and writes the module handle and IAT slot, which are ordinary relocated image
data. Every descriptor must hold RVAs inside the image, with its handle and IAT in writable
sections, and a load configuration that asks for protected delay-load IATs is refused, because that
helper would read the packed headers. The packed headers leave the delay-import directory out; the
reconstructed native image keeps it.
The initial profile leaves the packed load-config directory empty and accepts only the tested
default MSVC configuration without OS-managed CFG/SEH/EH-continuation tables. The original load
configuration remains byte-identical in the reconstructed native image; the native CRT initializes
its security cookie. Additional load-config features need their own implementation and tests.

An attached packed image reserves the original section RVAs in a writable, non-executable `CDP0`
BSS section; a detached one restores them elsewhere (see "Detached images"). `CDPX` contains only
our compiled loader and decoder, RX from mapping onward. `CDP1` contains the versioned metadata, preserved native headers/file gaps, resource-copy records, the
chunk table, the extent table and the chunks' raw packed streams. A relocated `.rsrc` exposes every original resource name; only RCDATA may
refer to initially empty native-image memory. Non-RCDATA payloads remain file-backed and unchanged.
`CDPW` provides a 4096-byte state page and one 32768-byte probability workspace per chunk. A small
`.reloc` describes the packed TLS directory's VA fields. The packed overlay is copied verbatim.
The packed import directory names one existing function from each native dependency, so Windows
loads those modules before TLS dispatch. The IAT directory covers exactly those thunks: without it,
Windows makes the whole section holding them writable while it binds them, and every page of
`CDP1` becomes private memory instead of staying shared with the file. The stub itself references no imports. With an importless
input, an ntdll dependency keeps DLL TLS initialization active; ntdll is already in every process.

The stream framing fixes lc=3, lp=0, pb=2, a dictionary at least as large as the expanded memory image,
and a mandatory end marker. The raw stream must consume its entire declared input, produce its
exact declared output and finish with range-code zero. Distances must refer to initialized
output and matches may never cross its end. There is no partial-output success and no readable
padding contract. The API accepts a caller-owned, 16-bit-aligned workspace; it allocates nothing.
Its model arrays have their own layout derived from the specification, not another decoder ABI.

Before compression, profile 1 converts the disp32 of E8/E9, 0F 80..8F, FF 15/25, and REX
8B/89/8D mod=00 r/m=101 in executable sections into an RVA by adding the following instruction's
RVA modulo 2^32. Scanning skips the complete recognized instruction, so operand bytes cannot
alter the inverse scan. Every recognized disp32 is converted, including false instructions in
embedded data; subtraction with the identical scan is an exact inverse. Non-executable data
is untouched. File-backed visible resource payloads are zeroed in the compressed memory image
and restored from `.rsrc` after decoding. An Adler-32 covers the transformed memory image.

The loader finds its configuration by RIP-relative addressing. It obtains ntdll routines from
the already-loaded module list; the stub has no import table, CRT, heap or executable stack.
It expands into writable non-executable pages, checks framing/checksum, reverses the filter,
copies resources, applies native relocations, resolves native imports and restores the OS TLS
index. It applies each native section's recorded permissions, leaving gaps read-only, and flushes
the instruction cache before invoking any original callback or entry point. All errors terminate
the process before control enters restored code. No user-mode write makes a page both writable and
executable; a cold chunk (below) is written while it is neither and is executable only afterwards.
TLS dispatch handles process/thread attach and detach; DLL entry dispatch preserves its arguments.
The probability workspace lives outside the stack. Compiler stack-usage reports are build outputs.
The OS-visible TLS template includes its own DIR64 fixups, independently of the restored native
template. Static TLS indexes survive decompression, and per-thread initialization uses that template.

The host verifier uses our decoder to rebuild the original file from its memory image, saved
headers/gaps and original section table. Full-file SHA-256 equality is mandatory before publishing,
as are resource payloads, section order/RVAs, flags and overlay equality. A second Python standard-library decode
is an independent oracle during packing/tests, not a runtime dependency. Python's standard library
is the build-time encoder; the player contains only our decoder and Windows-system calls.

## Chunks, extents and threads

Version 3 cuts the memory image into 1 to 64 chunks (`Header::chunk_count`), each an independent
raw stream with its own Adler-32 of the filtered bytes (`adler`) and of the compressed bytes
(`input_adler`). The stream framing is unchanged. A chunk's output is a list of page-aligned runs of
the image: the extent table (`Header::extent_count`) names, for each run, its offset from
`memory_rva`, its length and its chunk. The table is sorted by offset and the runs never overlap, so
a chunk may cover several runs separated by pages it leaves out; its output is the runs' bytes in
offset order. Chunks are in input order: their compressed streams are contiguous and cover the
stream exactly.

An extent never crosses a section, so it is entirely code or entirely data. The packer filters each
extent as a scan of its own when compressing it, and the loader reverses each one the same way, so a
cut may fall anywhere and no chunk depends on a neighbour's filter state. The verifier likewise
reverses one extent at a time and the full-file hash proves every boundary.

The loader checks every chunk's input checksum at start-up, so damage anywhere in the stream
terminates the process before native code runs, and checks the output checksum when it decodes the
chunk. Images of 1 MiB or more get four chunks decoded at start-up, balanced by each region's
compressed size. The entry point starts one thread per chunk, up to `PEB::NumberOfProcessors`, with
`RtlCreateUserThread`; every thread, the entry thread included, takes chunks through an atomic
counter, each with its own workspace slot, and the entry thread waits for the others before
restoring linkage. A thread that cannot be created leaves its chunks to the rest. Worker threads run
only loader code and end with `RtlExitUserThread`; their attach and detach reach the TLS callback
while initialization is still in progress, so no native callback sees them. The entry point then
runs the native process-attach TLS callbacks with the recorded argument, before the native entry
point. A DLL decodes its chunks in turn in its process-attach TLS callback, as in version 1, and
never carries cold chunks (below).

## Zero pages

Pages that no extent covers are zero, start and end on page boundaries, and lie outside code,
resource-copy destinations and relocation targets. `CDP0`, or a detached image's committed private
memory, is demand-zero, so the loader never writes them and they stay out of the working set until
the program touches them, as in a native image. The
packer leaves out runs of at least 16 zero pages outside code sections and resource-copy
destinations; in cordinal's player that is `.data`'s uninitialized tail of 207 pages. `memory_adler`
still covers the whole image with its zeros. The verifier fills each gap with zeros, rejects one
inside code, and the full-file hash proves the rest.

## Code decoded on demand

`tools/make-code-profile.py` runs the memory-budget scenarios against the native player under a
debugger that breaks once on each function's first byte, and writes, for each function that ran, the
FNV-1a hash of the function's object file and decorated name. A line may also give hex byte ranges
of the function to keep eager (`start:end,...`); without them the whole function is. cordinal's build
links the listed functions first, so they fill as few pages as possible. The packer looks those
hashes up in the linker map of the image being packed, so the profile survives a relink and code
moving. Every executable page no listed range covers is cold; the packer keeps both pages eager when
a relocation crosses a page boundary between them, then groups the rest into blocks of about 256 KiB
(`LAZY_BLOCK`), each a lazy
chunk (`Chunk::flags & lazy`). A hash the map no longer has is a function that changed since the
profile was taken: its pages stay cold and are decoded when they first run, which costs one fault,
never correctness. Without a profile and a map every chunk is eager, and a DLL never has cold code
because the unload would take away the code its handler points at.

Cold chunks need a detached image (below). The loader decodes the eager chunks at start-up, leaves
each cold chunk's extents reserved but not committed, and registers a vectored exception handler
before any native code runs. The handler decodes the chunk on its first page touch, a read, a write
or an instruction fetch, before any other handler in the process sees the fault, and returns
continue-execution. One cold chunk decodes or is evicted at a time behind a lock; a thread that
faults on a chunk another is decoding waits and runs again. A write to a decoded chunk is a real
fault, since code is never writable, and is passed on. Decoding copies whole pages, so the process's
other threads are suspended across the copy: none can execute a half-written page or read one as
data. A thread suspended inside a system call finishes that call first, so no lock it holds can
block the copy. The thread handles carry `THREAD_SUSPEND_RESUME` and `THREAD_GET_CONTEXT`; until
version 4 they carried only the query right, every suspension failed and no thread was stopped.
The chunk's pages are committed read-write for the copy and then made execute-read, so W^X holds
throughout, and the instruction cache is flushed before the threads resume.

The loader allocates a scratch buffer for a chunk's output, scatters its extents from that buffer
and releases it; the decode API itself still allocates nothing. A lazy chunk applies the DIR64
fixups that fall inside it when it is decoded, with the relocation table already in the image. Data
sections, `.pdata`, `.rsrc` and `.reloc` are never cold: a system call that reads or writes an
uncommitted page, such as `ReadFile` into a buffer, fails with an access-violation status instead
of raising a fault the handler can see. If the system calls the handler needs are missing, the
loader decodes every chunk at start-up instead; that costs the memory the cold pages save but is
otherwise the same image.

## Detached images

An image is detached when it has cold code, or when the packer is asked to (`--detach`, which
cordinal's test companion uses so its self-test runs on this layout); `Header::flags & detached`
says so, and a DLL never is. A detached packed image has no `CDP0`: `CDPX` starts at RVA 4096, and
the packed sections keep none of the native RVAs. Its exception directory is empty. When the packed
image is smaller than the native one, a read-only uninitialized `CDPZ` section pads it to the native
size, which costs no commit, and RCDATA entries point there, so they read as zeros from the file as
they read from `CDP0` before. Windows writes the TLS index into the state page (`State::tls_slot`).

Written image pages cannot be given back: `VirtualFree` refuses image memory, and `MEM_RESET`,
`DiscardVirtualMemory` and `OfferVirtualMemory` return `ERROR_USER_MAPPED_FILE`. So the loader
restores a detached image into private memory instead. It reserves an address range as large as
the packed image or the native one, whichever is larger, wherever Windows places it (the image is
relocated there with its DIR64 fixups), commits the native headers, every eager extent and the zero
pages, and leaves cold extents reserved. The packed image stays mapped and untouched: `CDP1` and
`.rsrc` remain shared with the file.

Native code then has to find itself as the program. `PEB::ImageBaseAddress` (what
`GetModuleHandle(nullptr)` and `FindResource(nullptr, ...)` read) is set to the native image. The
program's loader entry (`LDR_DATA_TABLE_ENTRY`, first in load order) gets the native base and entry
point, and is moved in ntdll's base-address tree with `RtlRbRemoveNode` and `RtlRbInsertNodeEx`, so
`GetModuleFileName` of the module handle works; its size stays the packed image's, which the
reservation covers. The native image is added to ntdll's inverted function table: that table, not
the loader entry and not `RtlAddFunctionTable`, is what `RtlPcToFileHeader` searches, and C++
`throw` and `GetModuleHandleEx(FROM_ADDRESS)` depend on it. Without that entry every C++ exception
from private memory ends in a fast fail. These are undocumented structures. The loader finds each
by its contents (the tree by its root and minimum in ntdll's `.data`, the table by the packed
image's own entry in `.mrdata`, which it makes writable only for the insertion and bumps the
table's epoch around), checks their shape before writing, holds the loader lock with every other
thread suspended while it writes, and afterwards checks that `RtlPcToFileHeader` names the native
image. Anything unexpected terminates the process before native code runs.

Windows built the static TLS template, and every thread's block so far, from the packed TLS
directory, with pointers relocated for the packed image. Each DIR64 fixup inside the native template
is moved by the distance between the two images, first in the packed template (so new threads copy
the right values) and then in every existing thread's block that still holds the old value.

A decoded cold chunk can be decommitted again, and then faults back and decodes again, the same
bytes with the same fixups. Its pages leave the working set and the commit charge at once, with no
pagefile write. The loader evicts under the cold lock with the other threads suspended and their
contexts read: it keeps a chunk any thread is executing in, or that its caller returns into. A
thread whose return address lies in an evicted chunk needs nothing: returning faults the chunk back.
Two things evict:

- **The program**, through the hook. A native image may hold one `Hook` (`src/format.h`) in
  writable data, which the packer finds by its 16-byte magic (`Header::hook_rva`) and the loader
  fills in: `evict(address)` decommits the chunk holding `address`, or every idle one for null;
  `decodes(address)` counts a chunk's decodes; `status` reports the counts, the resident cold bytes
  and both images' bases. cordinal calls `evict(nullptr)` from its idle trim (`src/app_memory.cpp`).
- **The cap.** `Header::cold_cap`, when not zero, is the most cold chunks committed at once; a
  decode beyond it decommits the one decoded longest ago. It is zero, no cap, unless the packer is
  given `--cold-cap`: the loader cannot see which chunks are in use, only which ran first, so a cap
  smaller than a working set would decode the same chunks over and over.

`pe_fixture.cpp` checks a detached image's module identity (`MEM_PRIVATE` code,
`GetModuleHandle`, `GetModuleFileName`, `GetModuleHandleEx(FROM_ADDRESS)`, `RtlPcToFileHeader`)
and its static TLS on a new thread. With one cold chunk per page it checks a never-touched chunk is
`MEM_RESERVE` and not resident; that a call decodes it execute-read; that an eviction makes it
`MEM_RESERVE` again and lowers both the private working set and the commit charge by at least its
size; that the next call decodes it a second time; that a thread blocked inside a cold function
returns through its evicted page; that the eviction suspended that thread; and the cap's order.

Metadata layouts are declared in `src/format.h`; the packer writes every field explicitly and
the loader checks the version, extents and checksum before using them. `CDP1` is deliberately
incompatible with stock UPX; stock rejection is tested, not presented as UPX interoperability.
