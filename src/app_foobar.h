// The foobar2000 theme's window: foobar2000's default UI arrangement and drawing, used while that
// theme is on in the normal window size (app_foobar.cpp).
#pragma once
#include "app_state.h"

namespace app {

// foobar2000's chrome colors, besides the palette: the light gray of its toolbar glyphs, tab and
// header labels, and the list header's own near-black.
constexpr Rgb kFoobarChromeText = 0xDEDEDE, kFoobarHeaderBg = 0x191919, kFoobarFrame = 0x646464;

bool FoobarMode();
// Follows a theme or window size change into or out of the foobar2000 arrangement: the playlist's
// columns, the Album List panel and the toolbar's extra buttons. Cheap when nothing changed.
void FoobarSync();
void LayoutFoobar(HDWP& dwp, RECT r);
void PaintFoobarChrome(HDC dc);
void PaintFoobarVisuals(HDC dc, RECT spectrum, RECT scope, const AudioVisuals& frame);
void DrawFoobarButton(const DRAWITEMSTRUCT* di);
RECT FoobarSliderTrack(HWND bar);
void FoobarTick();  // the status updates: Selection Properties follows the focused track
void FoobarTheme();
void FoobarFoldersChanged();
// The playlist: the group header row ("Artist - [Year] Album") and the cover column beside a
// group's tracks, and how many rows that cover needs.
void DrawFoobarHeader(HDC dc, int group, RECT rc);
void DrawFoobarCover(HDC dc, int row, RECT rc);
uint32_t FoobarCoverRows();

}  // namespace app
