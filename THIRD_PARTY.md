# Third-party notices

cordinal is licensed under GPL-3.0-or-later; see `LICENSE`. Its codecs and emulators are its own.
What it carries from elsewhere:

| What | Source and notice |
|---|---|
| WMA codebook/band arrays | FFmpeg Project, copyright 2002, LGPL-2.1-or-later; [license](src/third_party/wma-numeric-data-LICENSE.txt) |
| Opus CELT/SILK models, quantizers and SILK resampler design coefficients | Xiph/Opus authors, BSD-style terms; [notice/license](src/third_party/opus-numeric-data-LICENSE.txt) |
| Musepack SV7/SV8 codebooks | The Musepack Development Team, copyright 2005-2009, BSD-3-Clause; [license](src/third_party/musepack-numeric-data-LICENSE.txt) |
| Apollo theme art (`src/apollo/`) | The butterfly and track icons, toolbar strip and four rebar backgrounds of Apollo 37zz by Heikki Ylinen, re-encoded as palette PNGs. They belong to their owner and are not covered by cordinal's license |
| `src/third_party/stb_image.h` 2.30 (the Linux cover loader) | [nothings/stb](https://github.com/nothings/stb/tree/f58f558c120e9b32c217290b80bad1a0729fbb2c), public domain or MIT; the notice is in the header |

The WMA, Opus and Musepack entries are numeric tables, not decoder routines. Other fixed codewords and
transform/quantizer constants follow the formats' specifications.

Windows supplies WASAPI, Direct2D, DirectWrite, WinHTTP, the UCRT and other system components;
Windows builds use its image loader instead of stb_image.

The Windows release is packed by cdpack (`cdpack/`), cordinal's own packer, decoder and loader. It
contains no UPX code; it is GPL-2.0-or-later (`cdpack/COPYING`), and `cdpack/PROVENANCE.md` records
its sources. Building it needs LLVM-MinGW's clang and Python 3.14, which release CI downloads at
pinned versions; neither is part of the player. `build.bat stock-nrv` packs with
[UPX](https://upx.github.io/) 5.2.1 instead, downloaded and hash-checked by `tools/pack-release.ps1`,
with its unmodified stub.

New dependencies must have GPL-3.0-or-later-compatible licenses and be recorded here and in
`src/third_party/README.md`.
