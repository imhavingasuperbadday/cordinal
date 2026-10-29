# Published numeric-data notices

No vendored decoder/emulator implementation remains here. The former dr_libs,
stb_vorbis and Nuked OPN2 sources have been removed from the source tree and build.

- `opus-numeric-data-LICENSE.txt` preserves notices for published CELT/SILK numeric
  models and reference SILK resampler filter-design coefficients.
- `wma-numeric-data-LICENSE.txt` preserves LGPL-2.1 terms for WMA numeric arrays
  published by the FFmpeg Project (copyright 2002).

Cordinal decoder routines live in `src/audio_*`; emulator routines in `src/chiptune`.

`stb_image.h` (github.com/nothings/stb, commit f58f558c120e9b32c217290b80bad1a0729fbb2c, public domain or MIT)
decodes covers in the Linux build only.
