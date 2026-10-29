// Resource identifiers shared by cordinal.rc and the code that loads the resources.
#pragma once
#define IDI_APP 101  // application/tray icon (cordinal.ico, see cordinal.rc)
#define IDR_LICENSE_ENCODERS 240  // full MIT notice for the built-in MP3/AAC sources
// DELTARUNE's Dark Fountain sprites (app_fountain.cpp) and its dialogue font as a TrueType font
// (the DELTARUNE theme, theme_palette.cpp; built by tools/make_deltarune_font.py). RCDATA.
#define IDR_FOUNTAIN_EDGE 201
#define IDR_FOUNTAIN_BASIN 202
#define IDR_FOUNTAIN_TEXTURE 203
#define IDR_FONT_DELTARUNE 204
// The game's sound effects for the DELTARUNE theme (app_deltarune.cpp), 22.05 kHz mono FLAC
// re-encoded losslessly from the game's PCM WAVs.
#define IDR_SND_SELECT 210
#define IDR_SND_MENUMOVE 211
#define IDR_SND_NOISE 212
#define IDR_SND_SAVE 213
#define IDR_SND_BELL 214
#define IDR_SND_ERROR 215
#define IDR_SND_CANTSELECT 216
#define IDR_SND_SWING 217
#define IDR_SND_LEVELUP 218
#define IDR_SND_TXTQ 219                 // Queen's text blip (Chapter 2)
#define IDR_SND_QUEEN_BITCRUSHLAUGH 220  // and her bitcrushed laugh
// Apollo's art for the Apollo theme (app_apollo.cpp), from Apollo 37zz (Heikki Ylinen): the
// butterfly and track icons, the toolbar strip (fifteen 16 px glyphs on a green key) and the four
// rebar backgrounds it shipped beside Apollo.exe, re-encoded as palette PNGs.
#define IDI_APOLLO 102
#define IDI_APOLLO_TRACK 103
#define IDR_APOLLO_TOOLBAR 230
#define IDR_APOLLO_REBAR 231  // + 0 Capacala, 1 butterflies, 2 letters, 3 notes
