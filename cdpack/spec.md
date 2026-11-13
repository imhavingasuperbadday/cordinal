# CDP1: cordinal's independent PE64 packing format

Version 3, 2026-11-09, not yet released: chunks may leave out whole zero pages (see "Zero pages"),
and code the profile marks cold is decoded on first use (see "Code decoded on demand").
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

The packed image reserves the original section RVAs in a writable, non-executable `CDP0` BSS
section. `CDPX` contains only our compiled loader and decoder, RX from mapping onward. `CDP1`
contains the versioned metadata, preserved native headers/file gaps, resource-copy records, the
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
resource-copy destinations and relocation targets. `CDP0` is demand-zero, so the loader never writes
them and they stay out of private memory until the program touches them, as in a native image. The
packer leaves out runs of at least 16 zero pages outside code sections and resource-copy
destinations; in cordinal's player that is `.data`'s uninitialized tail of 207 pages. `memory_adler`
still covers the whole image with its zeros. The verifier fills each gap with zeros, rejects one
inside code, and the full-file hash proves the rest.

## Code decoded on demand

`tools/make-code-profile.py` samples the `.text` pages the native player runs in the memory-budget
scenarios and writes, for each function it saw, the FNV-1a hash of the function's object file and
decorated name plus the parts of it that were resident. The packer looks those hashes up in the
linker map of the image being packed, so the profile survives a relink and code moving. Every
executable page no listed range covers is cold; the packer keeps both pages eager when a relocation
crosses a page boundary between them, then groups the rest into blocks of about 128 KiB, each a lazy
chunk (`Chunk::flags & lazy`). A hash the map no longer has is a function that changed since the
profile was taken: its pages stay cold and are decoded when they first run, which costs one fault,
never correctness. Without a profile and a map every chunk is eager, and a DLL never has cold code
because the unload would take away the code its handler points at.

The loader decodes the eager chunks at start-up, then makes each cold chunk's extents inaccessible
and registers a vectored exception handler before any native code runs. The handler decodes the
chunk on its first page touch, a read, a write or an instruction fetch, before any other handler in
the process sees the fault, and returns continue-execution. One cold chunk decodes at a time behind a
lock; a thread that faults on a chunk another is decoding waits, and a fault on an already decoded
chunk is passed on. Decoding copies whole pages, so the process's other threads are suspended across
the copy: none can execute a half-written page or read one as data. A thread suspended inside a
system call finishes that call first, so no lock it holds can block the copy. The chunk's pages go
from inaccessible to read-write for the copy and then to execute-read, so W^X holds throughout, and
the instruction cache is flushed before the threads resume.

The loader allocates a scratch buffer for a chunk's output, scatters its extents from that buffer
and releases it; the decode API itself still allocates nothing. A lazy chunk applies the DIR64
fixups that fall inside it when it is decoded, with the relocation table already in the image. Data
sections, `.pdata`, `.rsrc` and `.reloc` are never cold: a system call that reads or writes an
inaccessible page, such as `ReadFile` into a buffer, fails with an access-violation status instead
of raising a fault the handler can see. If the system calls the handler needs are missing, the
loader decodes every chunk at start-up instead; that costs the memory the cold pages save but is
otherwise the same image.

Metadata layouts are declared in `src/format.h`; the packer writes every field explicitly and
the loader checks the version, extents and checksum before using them. `CDP1` is deliberately
incompatible with stock UPX; stock rejection is tested, not presented as UPX interoperability.
