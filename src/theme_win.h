// Win32 plumbing for dark title bars, menus and scrollbars on Windows 10 1809+ / 11.
// The palette is the single source of colors for every control in both modes.
#pragma once
#include "theme_palette.h"

#include <windows.h>

// lightTheme is the uxtheme theme used when not dark; most callers want the "Explorer" default.
void ApplyWindowTheme(HWND hwnd, const Palette& p);
void ApplyControlTheme(HWND control, bool dark, const wchar_t* lightTheme = L"Explorer");
void ApplyMenuTheme(bool dark);
