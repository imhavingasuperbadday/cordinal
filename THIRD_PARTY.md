# Third-party notices

cordinal is licensed under GPL-3.0-or-later; see `LICENSE`. Its decoders and emulators are its own.
What it carries from elsewhere:

| What | Source and notice |
|---|---|
| WMA codebook/band arrays | FFmpeg Project, copyright 2002, LGPL-2.1-or-later; [license](src/third_party/wma-numeric-data-LICENSE.txt) |
| Opus CELT/SILK models, quantizers and SILK resampler design coefficients | Xiph/Opus authors, BSD-style terms; [notice/license](src/third_party/opus-numeric-data-LICENSE.txt) |
| The MP3 and AAC-LC encoders in `src/encode/` | [glint](https://github.com/CrispStrobe/glint), MIT license; [license](src/encode/LICENSE.txt), adaptations in [its README](src/encode/README.md) |
| `src/third_party/stb_image.h` 2.30 (the Linux cover loader) | [nothings/stb](https://github.com/nothings/stb/tree/f58f558c120e9b32c217290b80bad1a0729fbb2c), public domain or MIT; the notice is in the header |

The WMA and Opus entries are numeric tables, not decoder routines. Other fixed codewords and
transform/quantizer constants follow the formats' specifications.

Windows supplies WASAPI, Direct2D, DirectWrite, WinHTTP, the UCRT and other system components;
Windows builds use its image loader instead of stb_image.

Release packing uses [UPX](https://upx.github.io/) 5.2.1, downloaded and hash-checked by
`tools/pack-release.ps1`. It is a build tool and its unmodified stub is used.

New dependencies must have GPL-3.0-or-later-compatible licenses and be recorded here and in
`src/third_party/README.md`.
