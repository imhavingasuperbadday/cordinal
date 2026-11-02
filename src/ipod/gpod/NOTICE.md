# Vendored libgpod subset

This directory is a source-only subset of the pure C++20 libgpod rewrite made for
`gtkpod-cpp` (`gtkpod-cpp/libgpod/cpp`), which in turn ports the classic-iPod database code from
libgpod 0.8.x. It is compiled directly into cordinal; there is no DLL.

Copyright belongs to the libgpod authors and the C++ rewrite's authors. libgpod's source headers
license the code under the GNU Lesser General Public License, version 2.1 or later; the complete
licence text is in `licenses/lgpl-2.1.txt`. Cordinal is GPL-3.0-or-later, which satisfies the
LGPL's combined-work terms. The source is included in this repository, so recipients can rebuild
the combined program with a modified version of this subset.

What is included:

- iTunesDB/iTunesCDB reading and writing, checksums, little-endian I/O, compressed-database
  zlib helpers, smart-playlist and plist parsers.
- The device model, model tables, SysInfo reading, time-zone conversion, path helpers, and file
  copy/rename helpers.
- `optional_stubs.cpp`, added for cordinal, replaces ArtworkDB, PhotoDB and SQLite generation. The
  classic sync does not use those databases; the stubs make the omission explicit and prevent a
  partial write.

Excluded intentionally: ArtworkDB, PhotoDB, image codecs, SQLite shadow databases, the optional
libimobiledevice/iPhone backend, and the standalone test programs. The upstream text is unchanged
apart from this notice, the stubs file and trailing blank-line cleanup in four vendored files.
