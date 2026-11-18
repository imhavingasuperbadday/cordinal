// Applies the palette-based dark/light theme to Win32 surfaces that have no public API of their own:
// immersive title bars (DWM), common controls (uxtheme) and popup menus (undocumented uxtheme
// ordinals). Every call degrades to a no-op on Windows builds that lack the entry point.
#include "theme_win.h"

#include <commctrl.h>
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

ChoiceLabelPainter s_choicePainter = nullptr;

// A checkbox or radio button that draws a glyph and a label (not a push-like one).
bool IsChoiceButton(HWND control) {
    wchar_t cls[16];
    if (!GetClassNameW(control, cls, 16) || _wcsicmp(cls, WC_BUTTONW)) return false;
    const LONG_PTR style = GetWindowLongPtrW(control, GWL_STYLE);
    if (style & BS_PUSHLIKE) return false;
    switch (style & BS_TYPEMASK) {
        case BS_CHECKBOX: case BS_AUTOCHECKBOX: case BS_3STATE: case BS_AUTO3STATE:
        case BS_RADIOBUTTON: case BS_AUTORADIOBUTTON: return true;
    }
    return false;
}

// On the parent of every themed choice button: its custom-draw notifications go to the painter before
// the parent's own window procedure sees them, so no dialog can forget to repaint the labels.
LRESULT CALLBACK ChoiceParentSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_NOTIFY && s_choicePainter) {
        auto* draw = reinterpret_cast<NMCUSTOMDRAW*>(lp);
        if (draw->hdr.code == NM_CUSTOMDRAW && GetParent(draw->hdr.hwndFrom) == h && IsChoiceButton(draw->hdr.hwndFrom))
            return s_choicePainter(draw);
    } else if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(h, ChoiceParentSubclass, 1);
    }
    return DefSubclassProc(h, msg, wp, lp);
}
}  // namespace

void SetChoiceLabelPainter(ChoiceLabelPainter painter) { s_choicePainter = painter; }

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
    // A themed checkbox or radio draws its label in the theme's color, ignoring WM_CTLCOLORBTN (black
    // on a dark surface). Installing the same subclass again only updates it, so this is idempotent.
    if (HWND parent = GetParent(control); parent && IsChoiceButton(control))
        SetWindowSubclass(parent, ChoiceParentSubclass, 1, 0);
}

// Ordinals 135 (SetPreferredAppMode) and 136 (FlushMenuThemes): the process-wide dark mode for popup
// menus; the flush applies it to menus that already exist.
void ApplyMenuTheme(bool dark) {
    if (auto set = Ordinal<SetPreferredAppModeFn>(135)) set(dark ? 2 /* ForceDark */ : 3 /* ForceLight */);
    if (auto flush = Ordinal<FlushMenuThemesFn>(136)) flush();
}
