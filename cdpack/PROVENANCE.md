# Provenance

All implementation files in `cdpack/` are original work written for this project from `spec.md`.
The author has seen Track R measurements and broad optimization descriptions; this is a fresh
implementation, not a clean-room assertion. No UPX/UCL source or generated machine code is used.

Sources consulted:

- Igor Pavlov, format specification draft, 2015-06-14, linked at https://www.7-zip.org/sdk.html .
  Only the format specification was read; the accompanying C++ implementation was not used.
- Microsoft PE/COFF format: https://learn.microsoft.com/en-us/windows/win32/debug/pe-format .
- Track R decisions 0004, 0006 and 0008: measured performance, size, and probability-lookahead
  concepts only. They are not implementation oracles.

The build uses the already-installed clang/LLVM and Python standard-library encoder.
No new package, vendored library or runtime dependency is added. Standalone tests use Python's
Python standard-library as an independent format oracle. The repository's licence applies to this original work;
independence from UPX's embedded stub does not relicense cordinal or its existing assets.

October 23 speed continuation: carry-selected integer range/probability updates, paired child
loads, a zero-width failure state and bounded word match copies are original changes derived
from our own decoder's disassembly and the format arithmetic. The small x64 assembly block uses
baseline AMD64 instructions; no research stub instructions, tables or ABI were ported.

The four-byte Adler update is derived by expanding four steps of this project's original
byte loop. Python's zlib supplies test results only; its implementation was not consulted.

October 24 source inclusion: the independent `cdpack/` subtree was copied from local research
commit `f63f010171c59600a1f781f2a6032f86719cfd24`, branch `a local research branch`, into
cordinal. That commit belongs to the local research checkout, not the public UPX project's
history. `origin.json` records the imported file hashes before the integration edits below.
The original GPL-2.0-or-later terms and GPL version 2 text are retained in `COPYING`.
No Track R, UPX/UCL implementation, generated stub, vendor source or research binary was copied.

The standalone build and pack commands now default to cordinal's ignored `build/cdpack` folder.
Cordinal's `build.bat cdpack` uses these bundled sources; normal release packing stays stock UPX.

November 4, 2026, format version 2 (in cordinal only): the chunk table, the packer's balanced
cuts and per-chunk verification, and the loader's threaded entry-point decode are original work
written in cordinal from this specification and the Windows loader behaviour it documents; the
decoder itself is unchanged. `..\upx\cdpack` keeps version 1 and was not changed.
