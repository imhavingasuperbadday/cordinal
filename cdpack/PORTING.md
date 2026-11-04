# Porting and implementation record

No source files have been ported from UPX, UCL or their vendors into this directory.

| Destination | Origin | Date |
|---|---|---|
| `src/decoder/stream.h` | Written from the 2015-06-14 format specification | 2026-10-23 |
| `src/format.h`, `src/filter.h` | Written from CDP1 specification and PE/COFF documentation | 2026-10-23 |
| `src/stub/loader.cpp` | Written from CDP1 and Windows loader/PE documentation | 2026-10-23 |
| `tools/pack.py`, `tools/build.ps1`, `tests/` | Original packer, build and verification tools | 2026-10-23 |

Before committing, compare these files against the design notes: no UPX-specific identifiers,
tables, comments, decoder ABI, filter bytes, loader instructions or generated stub headers are
used. Format-mandated constants and PE field offsets come from the published specifications.
The original integration invoked this standalone tool in the sibling research checkout through a
replaceable seam. On October 24 its independent source subtree was included in cordinal itself;
`origin.json` records the exact local research commit and imported hashes. No UPX/UCL source was copied.

October 23 audit: compared every new implementation file with `research/cdpack-question.md` and
`spec.md`. No UPX/UCL identifier, probability-layout table, comment, stub macro, filter byte sequence,
decoder ABI or code is carried into this directory. Track R machine code is extracted only by a
research tool into ignored `build-cdpack/` for black-box comparison; it is never linked into CDP1.

October 23 speed audit: compared the decoder changes with `research/cdpack-speed-question.md`
and the framing rules in `spec.md`. The carry selection, probability rounding, paired loads and
bounded match copies were written here. No UPX/UCL source, identifiers, tables, comments or
instructions were copied. Input guards, mandatory EOS and the model layout remain independent.

The checksum follow-up is original algebra on the existing Adler sums, checked against Python's
zlib on random/all-255 data, modulo-block boundaries, exact guard pages and skipped checksum bytes.
No checksum implementation was copied from zlib, UPX or another source.
