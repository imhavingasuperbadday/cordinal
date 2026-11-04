# CDP1: cordinal's independent PE64 packing format

Version 2, 2026-11-04, first used for the 0.1.5 release (version 1, 2026-10-23, had a single stream).

This specification is independent of UPX. Integer fields are little endian. Only AMD64 PE32+
images with 4096-byte section alignment and ordinary imports, resources, DIR64 relocations,
TLS and x64 unwind information are accepted. Signed, managed, delay-import and CFG-instrumented
images are rejected until those contracts have dedicated tests. Unsupported inputs fail before
writing an executable. The native subsystem and DLL characteristics are retained.
The initial profile leaves the packed load-config directory empty and accepts only the tested
default MSVC configuration without OS-managed CFG/SEH/EH-continuation tables. The original load
configuration remains byte-identical in the reconstructed native image; the native CRT initializes
its security cookie. Additional load-config features need their own implementation and tests.

The packed image reserves the original section RVAs in a writable, non-executable `CDP0` BSS
section. `CDPX` contains only our compiled loader and decoder, RX from mapping onward. `CDP1`
contains the versioned metadata, preserved native headers/file gaps, resource-copy records, the
chunk table and the chunks' raw packed streams. A relocated `.rsrc` exposes every original resource name; only RCDATA may
refer to initially empty native-image memory. Non-RCDATA payloads remain file-backed and unchanged.
`CDPW` provides a 4096-byte state page and one 32768-byte probability workspace per chunk. A small
`.reloc` describes the packed TLS directory's VA fields. The packed overlay is copied verbatim.
The packed import directory names one existing function from each native dependency, so Windows
loads those modules before TLS dispatch. The stub itself references no imports. With an importless
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
the process before control enters restored code. No writes to executable pages occur afterward.
TLS dispatch handles process/thread attach and detach; DLL entry dispatch preserves its arguments.
The probability workspace lives outside the stack. Compiler stack-usage reports are build outputs.
The OS-visible TLS template includes its own DIR64 fixups, independently of the restored native
template. Static TLS indexes survive decompression, and per-thread initialization uses that template.

The host verifier uses our decoder to rebuild the original file from its memory image, saved
headers/gaps and original section table. Full-file SHA-256 equality is mandatory before publishing,
as are resource payloads, section order/RVAs, flags and overlay equality. A second Python standard-library decode
is an independent oracle during packing/tests, not a runtime dependency. Python's standard library
is the build-time encoder; the player contains only our decoder and Windows-system calls.

## Chunks and threads

Version 2 cuts the memory image into 1 to 8 chunks (`Header::chunk_count`, formerly the unused
`cookie_rva`), each an independent raw stream with its own Adler-32 of the filtered bytes. The
chunk table ends the metadata, after the resource-copy records: output offset (from `memory_rva`)
and size, input offset (from `compressed_offset`) and size, and checksum. Chunks are contiguous in
output and input order and cover both exactly. Images of 1 MiB or more get four chunks; the
packer balances them by each region's compressed size and moves a cut inside code to where the
filter's scan stops, so each chunk reverses the filter over its own share of every executable
section and the result equals one scan over the whole section. The verifier reverses it the
same way, chunk by chunk, so the full-file hash also proves the cuts.

Windows holds every new thread until process initialization ends, so an EXE's process-attach TLS
callback only records its reserved argument. The entry point restores the image: it starts one
thread per chunk, up to `PEB::NumberOfProcessors`, with `RtlCreateUserThread`; every thread,
the entry thread included, takes chunks through an atomic counter, each with its own workspace
slot, and the entry thread waits for the others before restoring linkage. A thread that cannot be
created leaves its chunks to the rest. Worker threads run only loader code and end with
`RtlExitUserThread`; their attach and detach reach the TLS callback while initialization is still
in progress, so no native callback sees them. The entry point then runs the native
process-attach TLS callbacks with the recorded argument, before the native entry point. A DLL
decodes its chunks in turn in its process-attach TLS callback, as in version 1.

Metadata layouts are declared in `src/format.h`; the packer writes every field explicitly and
the loader checks the version, extents and checksum before using them. `CDP1` is deliberately
incompatible with stock UPX; stock rejection is tested, not presented as UPX interoperability.
