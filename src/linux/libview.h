// The Library tab's view row, column browser and visual views on Linux (libview.cpp), as the Windows
// build's app_browser.cpp and libview.cpp: Tracks, Albums & Tracks, Albums and Artists, and the
// Genre | Album artist | Album browser over them.
#pragma once
#include "app.h"
#include "draw.h"

namespace lx {

// The view row's buttons: the four views, then the browser toggle on the right.
constexpr int kViewButtons = (int)LibraryMode::Count + 1;
const char* ViewButtonLabel(int i);
void SetLibraryMode(LibraryMode mode);
void ToggleBrowser();

// The column browser: shown on the Library tab when it is switched on.
bool BrowserShown();
float BrowserHeight();  // its panes' height in pixels, without the splitter under them
void SetBrowserHeight(float pixels);
// Narrows the Library's tracks by every pane's selection, recomputing each pane's values from what
// the panes before it let through (RebuildRows, before the search).
void FilterLibraryTracks(std::vector<TrackId>& tracks);
void DrawBrowser(const Rect& area, const Palette& p, float mouseX, float mouseY);
void BrowserMouseDown(const Rect& area, float x, float y, int button, int clicks, SDL_Keymod mod);
void BrowserWheel(const Rect& area, float x, float y, float dy);
// Keyboard: a click on a pane gives it the keys (arrows, Home/End, Page keys, Tab and Left/Right between
// panes, Enter plays, Esc leaves) and typing jumps to the value starting with it.
bool BrowserFocused();
bool BrowserKey(const SDL_KeyboardEvent& e);  // false for keys the window handles itself
void BrowserText(const char* text);
void BrowserBlur();

// The visual views, in place of the track list on the Library tab. Their selection is kept in
// g.selected, so the menus and commands act on it as on the list's.
bool CanvasActive();
void CanvasRebuild();  // after RebuildRows
void DrawCanvas(const Rect& area, const Palette& p, float mouseX, float mouseY);
void CanvasMouseDown(const Rect& area, float x, float y, int button, int clicks, SDL_Keymod mod);
void CanvasWheel(const Rect& area, float dy, bool ctrl);
bool CanvasKey(const SDL_KeyboardEvent& e);  // false for keys the window handles itself
void CanvasSelectAll();
bool CanvasWantsHand(const Rect& area, float x, float y);  // over a play button or stars

// ui.cpp, shared with the views.
void DrawAlbumCover(TrackId first, StringPool::Handle album, const std::wstring& title, const Rect& r, const Palette& p);
void OpenTrackMenu(float x, float y);
void RateTracks(const std::vector<TrackId>& ids, uint8_t rating);
// playback.cpp: plays `tracks` from `index` as Now Playing.
void PlayTracks(const std::vector<TrackId>& tracks, int index);

}  // namespace lx
