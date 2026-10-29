// Applies the palette-based dark/light theme to Win32 surfaces that have no public API of their own:
// immersive title bars (DWM), common controls (uxtheme) and popup menus (undocumented uxtheme
// ordinals). Every call degrades to a no-op on Windows builds that lack the entry point.
#include "theme_win.h"

#include <dwmapi.h>
#include <uxtheme.h>

namespace {
// Undocumented uxtheme exports (by ordinal) used by Explorer and Notepad for dark menus.
using SetPreferredAppModeFn = int(WINAPI*)(int);
using AllowDarkModeForWindowFn = bool(WINAPI*)(HWND, bool);
using FlushMenuThemesFn = void(WINAPI*)();

HMODULE UxTheme() {
    static HMODULE m = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return m;
}
template <class F>
F Ordinal(int n) {
    return UxTheme() ? reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(UxTheme(), MAKEINTRESOURCEA(n)))) : nullptr;
}
}  // namespace

// Sets the immersive dark attribute, then (Windows 11) the caption and caption-text colors so the
// title bar matches the chrome. The attribute numbers are hard-coded because the SDK headers here
// predate them.
void ApplyWindowTheme(HWND hwnd, const Palette& p) {
    BOOL dark = p.dark;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    // Dark: caption matches the menu/status surface. Light: standard system caption.
    COLORREF caption = p.dark ? Ref(p.surface) : 0xFFFFFFFF /* DWMWA_COLOR_DEFAULT */;
    COLORREF text = p.dark ? Ref(p.text) : 0xFFFFFFFF;
    DwmSetWindowAttribute(hwnd, 35 /* DWMWA_CAPTION_COLOR (Windows 11) */, &caption, sizeof(caption));
    DwmSetWindowAttribute(hwnd, 36 /* DWMWA_TEXT_COLOR */, &text, sizeof(text));
}

// Ordinal 133 (AllowDarkModeForWindow, Win10 1809+) opts the control into dark theming before the
// uxtheme theme is applied; when it is missing only the theme name is set.
void ApplyControlTheme(HWND control, bool dark, const wchar_t* lightTheme) {
    if (auto allow = Ordinal<AllowDarkModeForWindowFn>(133)) allow(control, dark);
    SetWindowTheme(control, dark ? L"DarkMode_Explorer" : lightTheme, nullptr);
}

// Ordinals 135 (SetPreferredAppMode) and 136 (FlushMenuThemes): the process-wide dark mode for popup
// menus; the flush applies it to menus that already exist.
void ApplyMenuTheme(bool dark) {
    if (auto set = Ordinal<SetPreferredAppModeFn>(135)) set(dark ? 2 /* ForceDark */ : 3 /* ForceLight */);
    if (auto flush = Ordinal<FlushMenuThemesFn>(136)) flush();
}
