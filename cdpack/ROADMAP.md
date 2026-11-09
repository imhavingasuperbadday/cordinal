# cdpack: what will be improved

cdpack packs the Windows release of cordinal from 0.1.5 on. This is the work still ahead of it,
roughly in order. Measurements are from November 4, 2026, on the 0.1.5 player (a 4,288,512-byte
native image) on a Windows 11 machine; the stock UPX 5.2.1 build of the same image is the reference.

## Where it stands

| | Stock UPX | cdpack (format 2) |
|---|---:|---:|
| Player bytes | 1,725,952 | 1,630,720 |
| Launch and exit | 42.2 ms | 35.8 ms |
| First paint | about 116 ms | about 111 ms |
| Idle private memory | 14.7 MB | 16.1 MB |

Item 1 has newer memory figures. Every pack is verified byte for byte against the original file before it is published, and the
loader refuses to run anything whose checksums do not match.

## 1. Memory

A packed player keeps its decoded image as private memory, which a native one shares with the file.
Measured November 9, 2026 with `cordinal-test.exe --memory-report` on one native player
(4,601,344 bytes, a 5.21 MiB image). `program` is the image's private memory: 3.09 MB at an empty
start and 3.11 MB while playing with code on demand, and 4.48 MB in every scenario before it.

| Same native player | `program` | Player bytes |
|---|---:|---:|
| format 2 | 6.71 MB | 1,731,072 |
| with the IAT directory | 5.21 MB | 1,731,072 |
| format 3 (zero pages left out) | 4.48 MB | 1,735,168 |
| format 3 with code decoded on demand | 3.09 MB | 1,823,232 |
| unpacked | 0.10 MB (1.9 to 2.3 MB resident, shared) | 4,601,344 |

The window appears in 35.6 ms with code on demand, 40.8 ms with every chunk eager and 20.8 ms
unpacked (median of five starts on this machine). Format 2 and the IAT build measured 39.4 and
38.5 ms by the older launch-and-exit method, so those two are not directly comparable.

- **The 1.3 MB above stock was `CDP1`.** The packed image had no IAT directory, so while binding
  its few imports Windows made the whole section writable, and all 1.5 MB of the compressed stream
  became private, although only three pages differ from the file. An IAT directory over the thunks
  keeps it shared.
- **Format 3 leaves out zero pages.** The decoder wrote `.data`'s uninitialized tail (207 pages)
  as zeros. Chunks now skip such runs, which stay demand-zero (`spec.md`, "Zero pages").
- **Code the profile marks cold stays compressed.** A profile of the functions the scenarios run,
  found again through the linker map, leaves 373 of the 845 `.text` pages cold: 1.46 MB in twelve
  128 KiB blocks that decode on first use (`spec.md`, "Code decoded on demand"). They cost 88 KB
  of file and save 1.39 MB of private memory, and start-up decodes less, so the window appears
  sooner than with every chunk eager. A first version used blocks of consecutive code, which
  decoded 2.75 of 3.30 MB because resident code is scattered over the section; choosing pages by
  function, and suspending the other threads across the copy so a page is never read half-written,
  is what made it pay.

What stays private is the rest of the image: `.text` 1.84 MB, `.rdata` 0.71, `.pdata` 0.20, `.rsrc`
0.15, `.data` 0.10. The native player keeps 1.5 to 1.8 MB of `.text` and 0.3 MB of `.rdata`
resident.

## 2. Windows 10 and other machines

The loader has been run on Windows 11 only. Next: Windows 10 22H2, a single-core virtual machine
(the loader falls back to decoding chunk after chunk), machines with many cores, and forced ASLR
and other exploit-protection settings, each with the full self-test on the packed build.

## 3. Antivirus and SmartScreen

Windows Defender finds nothing in the packed player, but other products judge unfamiliar packers
harshly and SmartScreen weighs a new file's reputation. Next: scan each release with more engines
before publishing, report false positives to their vendors, and consider signing the release
executable.

## 4. Size

Splitting the image into four independently compressed chunks costs about 60 KB (3.9%); one chunk
would be 1,570,304 bytes. Ideas: cut where few matches cross the boundary instead of only where the
compressed sizes balance, try three chunks on machines' typical core counts, and widen the branch
filter to more x64 instruction forms.

## 5. Start-up speed

Unpacking takes about 17 ms of a 36 ms launch on a four-core machine, against 23 ms for stock UPX.
Ideas: start the decode threads before the start-up checks finish, overlap the import resolution
with decoding, and keep tuning the decoder's inner loop, which is already about three times its
first version.

## 6. Coverage of Windows features

Images with Control Flow Guard tables, signatures, delay-loaded imports or unusual load
configurations are refused rather than packed. cordinal does not use them today; each needs its own
loader support and tests before it is accepted.

## 7. Build and review

Release CI builds the packer with a pinned LLVM-MinGW and Python and runs this directory's suite on
every push. Still to come: a reproducible-build check (the same commit gives the same bytes on two
machines), fuzzing the loader's metadata checks, and an outside review of the loader.
