// Light and dark palettes, and the system theme-mode detection behind them.
#pragma once
#ifdef _WIN32
#include <windows.h>
#endif

#include <cstdint>
#include <string>
#include <vector>

using Rgb = uint32_t;  // 0xRRGGBB
constexpr Rgb kNoRgb = 0xFFFFFFFF;  // no colour given: use the palette's
// Rgb is packed 0xRRGGBB; COLORREF is the Win32 0x00BBGGRR, hence the byte swap.
#ifdef _WIN32
inline COLORREF Ref(Rgb c) { return RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF); }
#endif

// The themes, in the order settings.ini stores them (so entries are only ever appended). System
// follows the current Windows preference between Light and Dark; the others pin their palette.
enum class ThemeMode { System, Light, Dark, Deltarune, Foobar, Paper, Midnight, Terminal, Apollo, Count };

// One palette per theme, laid out as surfaces (bg..border), text (text/sub/faint), the accent pair,
// interaction states (hover/select/selectText) and semantic colors (playing/error/amber/ok). Every
// palette keeps text, sub and selectText legible and the accent and semantic colors visible on its
// background; a self-test measures each pair (PRINCIPLES.md, "Legible in every theme").
struct Palette {
    bool dark;
    Rgb bg, surface, surface2, border, text, sub, faint, accent, accent2, hover, select, selectText, playing, error, amber, ok;
};

const Palette& LightPalette();
const Palette& DarkPalette();
bool SystemUsesDarkTheme();
// WCAG 2 contrast ratio between two colors, 1 (none) to 21 (black on white).
double Contrast(Rgb a, Rgb b);
// Black or white, whichever reads better on `fill` (text and glyphs drawn on the accent).
Rgb TextOn(Rgb fill);
// The first legibility rule `p` breaks (PRINCIPLES.md, "Legible in every theme"), in words such as
// "sub on bg is 3.9:1, needs 4.5:1", or empty when it keeps them all. Built-in and pack themes are
// held to the same rules through this one function.
std::wstring PaletteLegibility(const Palette& p);

// A theme: its name in View > Theme, its palette (null: follow Windows between Light and Dark),
// optionally its own UI font, the mark of the playing row, what status messages start with,
// whether the sound effects are the theme's own (app_deltarune.cpp) rather than Cordinal's
// synthesized ones, the glyph ratings are drawn with, and the waveform's played and unplayed
// colours. A font built into the executable (`fontResource`, RCDATA) is a pixel font, drawn
// at `fontPx` pixels (and whole multiples of it at higher DPI) without smoothing; without a resource
// `font` names a face every supported Windows has, drawn at `fontPx` dips (0: the system size).
struct Theme {
    const wchar_t* name;
    const Palette* palette;
    const wchar_t* font = nullptr;
    int fontResource = 0, fontPx = 0;
    const wchar_t* playingMark = L"\x25B6";
    const wchar_t* messagePrefix = L"";
    bool gameSounds = false;
    const wchar_t* ratingMark = L"\x2605";
    Rgb wavePlayed = kNoRgb, waveRest = kNoRgb;
};
const Theme& ThemeInfo(ThemeMode mode);
// The palette `mode` shows now (System resolved against Windows' preference).
const Palette& ThemePalette(ThemeMode mode);

// Themes from packs follow the built-in ones, as ThemeMode values from Count on; ThemeInfo for a
// value past them falls back to Light. The caller owns the strings and palettes the entries point
// to and keeps them alive until the next call. UI thread only: other threads see built-in themes.
void SetPackThemes(std::vector<Theme> themes);
int ThemeTotal();  // built-in and pack themes
inline bool PackThemeMode(ThemeMode mode) { return (int)mode >= (int)ThemeMode::Count; }

// A release ships Light, Dark and following the system. The other built-in themes, and the art and
// sounds they embed, are compiled in only with CORDINAL_EXTRA_THEMES (build.bat themes). Pack themes
// are always available.
#ifdef CORDINAL_EXTRA_THEMES
constexpr bool kExtraThemes = true;
#else
constexpr bool kExtraThemes = false;
#endif
inline bool ThemeAvailable(ThemeMode mode) {
    return kExtraThemes || (int)mode <= (int)ThemeMode::Dark || PackThemeMode(mode);
}
// A saved theme this build leaves out opens as Light or Dark, whichever its palette is.
inline ThemeMode AvailableTheme(ThemeMode mode) {
    if (ThemeAvailable(mode)) return mode;
    const Palette* p = ThemeInfo(mode).palette;
    return p && p->dark ? ThemeMode::Dark : ThemeMode::Light;
}
