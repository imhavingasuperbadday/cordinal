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

Every pack is verified byte for byte against the original file before it is published, and the
loader refuses to run anything whose checksums do not match.

## 1. Idle memory

cdpack's player holds about 1.3 MB more private memory once idle than the stock build of the same
image. Neither the chunked format nor the decode threads changed it, so it comes from the image
layout: which pages stay writable or get copied after start-up. The plan is to map the committed
regions of both builds, find the pages that differ, and give them back (release the probability
workspace, keep read-only data read-only and file-backed).

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
