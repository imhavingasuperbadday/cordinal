# Published numeric-data notices

No vendored decoder/emulator implementation remains here. The former dr_libs,
stb_vorbis and Nuked OPN2 sources have been removed from the source tree and build.

- `opus-numeric-data-LICENSE.txt` preserves notices for published CELT/SILK numeric
  models and reference SILK resampler filter-design coefficients.
- `wma-numeric-data-LICENSE.txt` preserves LGPL-2.1 terms for WMA numeric arrays
  published by the FFmpeg Project (copyright 2002).
- `musepack-numeric-data-LICENSE.txt` preserves the BSD terms of the Musepack codebooks
  (The Musepack Development Team, 2005-2009) in `src/audio_mpc_tables.h`.

Cordinal decoder routines live in `src/audio_*`; emulator routines in `src/chiptune`.

`stb_image.h` (github.com/nothings/stb, commit f58f558c120e9b32c217290b80bad1a0729fbb2c, public domain or MIT)
decodes covers and color emoji (CBDT and sbix PNGs) in the Linux build only.

`bearssl/` is BearSSL (https://www.bearssl.org/git/BearSSL, commit
7bea48e5e850ab4cafbe68d3765cdaba13a86d6f, 6 April 2026, MIT, `bearssl/LICENSE.txt`): its `inc/`
and `src/` directories copied unchanged, including the T0 sources (`*.t0`) its generated
`*.c` files come from. The Linux build compiles it into the program for HTTPS (TLS 1.2, X.509 path
validation and ECDSA), so no TLS library is needed from the distribution. cordinal's own code around
it is in `src/linux/tls_bearssl.cpp` and `src/linux/tls_der.cpp`. To update it, copy the same two
directories from a newer commit and record the commit here and in THIRD_PARTY.md.

`fontconfig-orth-data-LICENSE.txt` preserves the notice of fontconfig's language orthographies
(fc-lang/*.orth, fontconfig 2.15.0), from which `tools/make-font-orth.py` generates
`src/linux/font_orth.inc` for the Linux font matcher. Only those code point lists are used.

`unicode-data-LICENSE.txt` is the Unicode License v3 of the Unicode Character Database 15.1.0, from
which `tools/make-unicode-tables.py` generates `src/linux/unicode_tables.inc` (character properties
for the Linux text stack's itemizer, bidi pass and shaper). Only property values are used.

Nothing here serves the Linux sound output: `src/linux/pw_native.cpp` (PipeWire's native protocol,
with `spa_pod.cpp` for its POD encoding) and `src/linux/pulse_native.cpp` (PulseAudio's, also spoken
by pipewire-pulse) are written for cordinal from the protocols' documented and header-defined
layouts, so neither libpipewire, its libspa plugins nor libpulse is needed from the distribution.

`xorgproto-keysymdef-LICENSE.txt` preserves the notices of xorgproto 2024.1's keysym headers
(`keysymdef.h` and the vendor `XF86keysym.h`, `Sunkeysym.h`, `DECkeysym.h`, `HPkeysym.h`,
`ap_keysym.h`), from which `tools/make-keysym-table.py` generates
`src/linux/platform/keysym_table.inc` (keysym names, values and characters). Only those are used.

`sdl-LICENSE.txt` is SDL 3's zlib license, for the evdev-to-scancode table in
`src/linux/platform/keys.cpp` and the SDL3 API names and constants in `src/linux/platform/sdl_api.h`.
Nothing else serves the Linux window: `src/linux/platform/` speaks the X11 protocol
(`x11.cpp`, `x11_wire.cpp`) and the Wayland protocol (`wayland.cpp`, `wayland_wire.cpp`) itself,
reads XKB keymaps itself (`xkb_keymap.cpp`), and draws in software (`render.cpp`), so neither
libSDL3, libX11, libxcb, libwayland-client nor libxkbcommon is needed from the distribution.

`fluentui-system-icons-LICENSE.txt` is the MIT license of Microsoft's Fluent UI System Icons
(github.com/microsoft/fluentui-system-icons, commit ba34950eeb56d33045e4832121c68cae679e1c85).
`tools/make-fluent-icons.py` turns eleven of its regular SVGs (previous, play, pause, next, shuffle,
repeat, repeat one, history, speaker, muted speaker, dismiss) into `src/linux/fluent_icons.inc`; the
Linux build fills them where Windows draws Segoe MDL2 Assets glyphs.

`selawik/` holds Selawik 1.01 Regular and Bold (github.com/microsoft/Selawik, release 1.01, unmodified,
SIL OFL 1.1, `selawik/OFL.txt`), Microsoft's open stand-in for Segoe UI. It is compiled in only when
configured with `-DCORDINAL_SELAWIK=ON`: the FSF counts the OFL as GPL-incompatible.
