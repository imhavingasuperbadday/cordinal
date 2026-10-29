// UI shared by the application modules: fonts, theme application, palette-aware
// owner-draw helpers and the subclasses for the custom-drawn controls. Drawing is
// UI-thread only and sizes itself through S() using g.dpi.
#pragma once
#include "app_state.h"

namespace app {

void CreateFonts();
void ApplyFonts();
void FillSolid(HDC dc, const RECT& r, COLORREF c);
// Covers. One without art is a tile in a muted color of its own (the same name always gets the
// same color) with the name's initial in a lighter or darker shade of it, for the current palette.
struct CoverTile {
    Rgb tile, ink;
};
CoverTile CoverTileFor(const std::wstring& name);
// The first letter or digit of `name` (skipping emoji and punctuation), uppercased, or a note.
std::wstring CoverInitial(const std::wstring& name);
// GDI: `art` filling `r`, cropped to a square from its middle; the tile above in its place; and a
// hairline just outside `r`, so a cover as dark or as light as the background keeps its edge.
void DrawCoverArt(HDC dc, const RECT& r, const ArtImage& art);
void DrawCoverPlaceholder(HDC dc, const RECT& r, const std::wstring& name);
void DrawCoverFrame(HDC dc, const RECT& r);
void ApplyTheme();
bool HandleDarkMenuBar(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT& result);
FieldLookup Lookup(TrackId id, bool withPlayback);
void SetTextIfChanged(HWND h, const std::wstring& text);
// The control's current text. Safe for any length, including an empty control.
std::wstring WindowText(HWND h);
void SetStatusPart(int part, const std::wstring& text);
void ShowMessage(const std::wstring& text);
std::wstring EmptyViewText();
bool TranslateMainKey(MSG* msg);
LRESULT DrawTrackbar(NMCUSTOMDRAW* cd);
// 0..1 along a trackbar at a client point; the vertical seek bar maps top to bottom (app_ui.cpp).
double BarFracAt(HWND h, POINT pt);
// Subclass procedures for the stock controls (trackbar, search box, tabs, list
// header, toolbar buttons). Each installs itself via SetWindowSubclass and removes
// itself on WM_NCDESTROY; the UINT_PTR/DWORD_PTR parameters are unused.
LRESULT CALLBACK TrackbarSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR);
LRESULT CALLBACK SearchSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR);
LRESULT CALLBACK TabsSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR);
LRESULT CALLBACK HeaderSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR);
LRESULT CALLBACK ButtonSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR);
void DrawToolbarButton(const DRAWITEMSTRUCT* di);
void RegisterAppClass(const wchar_t* cls, WNDPROC proc, UINT style = 0, bool appIcon = true);
LRESULT FieldControlColor(HDC dc);
LRESULT SurfaceControlColor(HDC dc, COLORREF text);
void DoubleBufferedPaint(HWND hwnd, void (*draw)(HDC, RECT));
void TrimMemory();
bool ReducedMemoryMode();
size_t ViewMemoryBudget(size_t normal);
void UpdateViewMemory();

}  // namespace app
