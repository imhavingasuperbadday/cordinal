// The built-in palettes, their contrast, and Windows' light/dark preference. Palettes are positional aggregates:
// the field order must match Palette in theme_palette.h.
#include "theme_palette.h"

#include "resource.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <iterator>

// Field order (must match Palette): dark, bg, surface, surface2, border, text, sub, faint, accent,
// accent2, hover, select, selectText, playing, error, amber, ok.
// Cordinal's own look: neutrals with a faint warm plum cast, and the cardinal's red as the one
// accent (what's playing, what has played, what's special). Selection is a quiet rose, so the red
// stays reserved for playback.
const Palette& LightPalette() {
    static const Palette p{false,    0xFDFCFD, 0xF3F1F4, 0xFFFFFF, 0xDDD8DF, 0x1D1A20, 0x5E5764, 0x9C95A1,
                           0xC8303A, 0xE0525B, 0xEFEBF1, 0xF6DFE3, 0x1D1A20, 0xC02A34, 0xD93A3A, 0xB07C00, 0x2E7D32};
    return p;
}

// Same field order and intent as the light palette: a warm near-black list, the chrome a step
// lighter, a wine-tinted selection and a lighter cardinal for text on the dark ground.
const Palette& DarkPalette() {
    static const Palette p{true,     0x17151A, 0x1F1C22, 0x29252D, 0x38333D, 0xECE7EE, 0xA69FAB, 0x6F6875,
                           0xE5484D, 0xFF6B70, 0x27232B, 0x3A2832, 0xFFFFFF, 0xFF7A7F, 0xFF6B6B, 0xE6C34C, 0x4CC27A};
    return p;
}

// foobar2000's own dark look: #202020 chrome, black panels, #00DD00 text, Windows-blue sliders.
const Palette& FoobarPalette() {
    static const Palette p{true,     0x000000, 0x202020, 0x000000, 0x383838, 0x00DD00, 0x009E00, 0x005800,
                           0x007AD9, 0x00DD00, 0x171717, 0x003200, 0x00FF00, 0x00DD00, 0xFF4040, 0xDDDD00, 0x00FF00};
    return p;
}

// DELTARUNE's Dark World menus: white text on black, the selected option in yellow, the SOUL's red
// for what's playing, a dark purple Dark World chrome, TP orange for warnings and Kris' cyan, and the
// green of a won battle.
const Palette& DeltarunePalette() {
    static const Palette p{true,     0x000000, 0x0D0716, 0x170D26, 0x4B3A66, 0xFFFFFF, 0xC0C0C0, 0x808080,
                           0xFFFF00, 0x00FFFF, 0x1B1030, 0x2A1846, 0xFFFF00, 0xFF2020, 0xFF4040, 0xFFA040, 0x00FF00};
    return p;
}

// A printed page: cream paper, brown-black ink, ink blue for the accent and a highlighter-yellow
// selection, set in Georgia so the list reads like a liner-notes booklet.
const Palette& PaperPalette() {
    static const Palette p{false,    0xF6F0E1, 0xECE3CD, 0xFBF8F0, 0xD5C8AC, 0x2A2318, 0x66573F, 0xA29478,
                           0x2C5A87, 0x3F74A8, 0xEEE6D2, 0xF0DFA0, 0x2A2318, 0x2C5A87, 0xB0281E, 0x8F5F00, 0x2E6B34};
    return p;
}

// Late-night blues: a deep navy list, slate chrome and one ice-blue accent, in Bahnschrift's
// narrow, even letters. Cool where Dark is warm.
const Palette& MidnightPalette() {
    static const Palette p{true,     0x0E1522, 0x141D2E, 0x1A263B, 0x283750, 0xDBE5F4, 0x8FA2BF, 0x56677F,
                           0x58C4F5, 0x8AD8FA, 0x16213A, 0x1E3454, 0xFFFFFF, 0x7FD3FA, 0xFF6B7A, 0xF2C14E, 0x5ED6A2};
    return p;
}

// An amber phosphor terminal: one hue in three brightnesses on near-black, Consolas, a prompt for
// the playing row and for messages. Only errors and success leave the amber.
const Palette& TerminalPalette() {
    static const Palette p{true,     0x0B0802, 0x151005, 0x0B0802, 0x3A2A0A, 0xFFB000, 0xC28600, 0x6E4C00,
                           0xFFCC4D, 0xFFDB80, 0x1C1506, 0x3D2C00, 0xFFE08A, 0xFFE08A, 0xFF5A3C, 0xFFE08A, 0x9BD14B};
    return p;
}

// Apollo 37zz on Windows 2000: the classic 3D face (#D4D0C8) with its white highlight and gray
// shadow, a white playlist in black, the navy of a classic caption for the selection, and the
// Windows 2000 desktop's blue for what's playing outside the playlist (which marks it in bold).
const Palette& ApolloPalette() {
    static const Palette p{false,    0xFFFFFF, 0xD4D0C8, 0xFFFFFF, 0x808080, 0x000000, 0x404040, 0x808080,
                           0x0A246A, 0xA6CAF0, 0xEAE8E3, 0x0A246A, 0xFFFFFF, 0x4F7FC9, 0xC00000, 0x806000, 0x008000};
    return p;
}

namespace {
std::vector<Theme>& PackThemes() {
    static std::vector<Theme> themes;
    return themes;
}
}  // namespace

void SetPackThemes(std::vector<Theme> themes) { PackThemes() = std::move(themes); }
int ThemeTotal() { return (int)ThemeMode::Count + (int)PackThemes().size(); }

// Indexed by ThemeMode.
const Theme& ThemeInfo(ThemeMode mode) {
    static const Theme themes[] = {
        {L"&Follow Windows", nullptr},
        {L"&Light", &LightPalette()},
        {L"&Dark", &DarkPalette()},
        // The game's dialogue font (8bitoperator JVE, fnt_main) is 16 pixels to the em; the SOUL
        // marks what's playing, and messages read like the game's text boxes.
        {L"D&ELTARUNE", &DeltarunePalette(), L"DELTARUNE Pixel", IDR_FONT_DELTARUNE, 16, L"\x2665", L"* ", true},
        {L"f&oobar2000", &FoobarPalette()},
        {L"&Paper", &PaperPalette(), L"Georgia", 0, 0, L"\x00BB"},
        {L"&Midnight", &MidnightPalette(), L"Bahnschrift"},
        {L"&Terminal", &TerminalPalette(), L"Consolas", 0, 13, L">", L"> "},
        // Apollo's playlist and status bar are Tahoma 8 pt (11 px); its window is app_apollo.cpp.
        {L"&Apollo", &ApolloPalette(), L"Tahoma", 0, 11},
    };
    static_assert(std::size(themes) == (size_t)ThemeMode::Count);
    int i = (int)mode;
    if (i >= (int)ThemeMode::Count && i < ThemeTotal()) return PackThemes()[i - (int)ThemeMode::Count];
    return themes[i >= 0 && i < (int)ThemeMode::Count ? i : (int)ThemeMode::Light];
}

const Palette& ThemePalette(ThemeMode mode) {
    const Theme& t = ThemeInfo(mode);
    if (t.palette) return *t.palette;
    return SystemUsesDarkTheme() ? DarkPalette() : LightPalette();
}

namespace {
double Luminance(Rgb c) {
    auto channel = [](Rgb v) {
        double u = v / 255.0;
        return u <= 0.04045 ? u / 12.92 : std::pow((u + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(c >> 16 & 0xFF) + 0.7152 * channel(c >> 8 & 0xFF) + 0.0722 * channel(c & 0xFF);
}
}  // namespace

double Contrast(Rgb a, Rgb b) {
    double x = Luminance(a), y = Luminance(b);
    return (std::max(x, y) + 0.05) / (std::min(x, y) + 0.05);
}

Rgb TextOn(Rgb fill) { return Contrast(fill, 0xFFFFFF) >= Contrast(fill, 0x000000) ? 0xFFFFFF : 0x000000; }

// Body text at WCAG AAA, secondary and selected text at AA, and the accent and status colors at the
// 3:1 that UI marks need, on the surfaces they sit on.
std::wstring PaletteLegibility(const Palette& p) {
    const struct { const wchar_t *fg, *bg; Rgb a, b; double need; } rules[] = {
        {L"text", L"bg", p.text, p.bg, 7},           {L"text", L"surface", p.text, p.surface, 7},
        {L"sub", L"bg", p.sub, p.bg, 4.5},           {L"sub", L"surface", p.sub, p.surface, 4.5},
        {L"selectText", L"select", p.selectText, p.select, 4.5}, {L"text", L"hover", p.text, p.hover, 7},
        {L"accent", L"bg", p.accent, p.bg, 3},       {L"playing", L"bg", p.playing, p.bg, 3},
        {L"error", L"bg", p.error, p.bg, 3},         {L"amber", L"bg", p.amber, p.bg, 3},
        {L"ok", L"bg", p.ok, p.bg, 3},               {L"playing", L"select", p.playing, p.select, 3},
        {L"text on accent", L"accent", TextOn(p.accent), p.accent, 4.5},
    };
    for (const auto& r : rules) {
        double c = Contrast(r.a, r.b);
        if (c >= r.need) continue;
        wchar_t text[96];
        swprintf(text, std::size(text), L"%ls on %ls is %.1f:1, needs %g:1", r.fg, r.bg, std::floor(c * 10) / 10, r.need);
        return text;
    }
    return L"";
}

// AppsUseLightTheme is 0 when Windows apps are set to dark; the read is best-effort, and the default
// of 1 means a missing value is treated as light.
bool SystemUsesDarkTheme() {
#ifdef _WIN32
    DWORD value = 1, size = sizeof(value);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return value == 0;
#else
    // Linux resolves its system preference on the SDL thread, then publishes that palette.
    return false;
#endif
}
