// Cross-module UI entry points: the mini player, the view-size modes, layout, the big-mode and
// spotlight panels, the lyrics panel, and Layout() itself. Declared here so the app_*.cpp modules
// can call one another without including each other's translation units.
#pragma once
#include "app_state.h"

namespace app {

// Mini player (app_mini.cpp): a separate round window the main window hides behind.
void ToggleMiniPlayer();
HWND MiniPlayerWindow();
void MiniPlayerTick();
// View sizes (app_sizes.cpp): step through the five window modes and their saved geometry.
void SetViewSize(ViewSize size);
void RestoreViewSize(WINDOWPLACEMENT full, int showCmd);
void SaveViewSize();
constexpr unsigned kDefaultWindowSize = 1, kDefaultTheme = 2, kDefaultLibraryView = 4,
                   kDefaultBrowser = 8, kDefaultLayout = 16, kAllStartupDefaults = 31;
StartupPreferences CaptureStartupPreferences();
unsigned PendingStartupDefaults();
void SaveStartupDefaults(const StartupPreferences& choices, unsigned selected);
void SetPreferenceSaving(PreferenceSaving mode);
void FlushAutomaticPreferences();
void ResetPreferenceSuggestions();
void ObservePreferenceChanges(ULONGLONG now);
bool PreferenceSuggestionsDue(ULONGLONG now);
void MaybeOfferPreferenceSuggestions();
void ShowStartupDefaults(bool automatic = false);
HWND StartupDefaultsWindow();
void DetachStartupDefaultsFromMiniPlayer();
bool StartupDefaultsDialogMessage(MSG* msg);
void ShowCompactMenu(POINT pt);
void ViewSmaller();
void ViewLarger();
// Layout (app_layout.cpp): deferred placement of the controls for the current size.
void Place(HDWP& dwp, HWND h, int x, int y, int w, int hgt);
void LayoutLibrary(HDWP& dwp, RECT r, bool show);
int StripHeight();
void LayoutStrip(HDWP& dwp, RECT r);
void StripHoverTick();
// Panels (app_panels.cpp): the spotlight / big-mode Now playing and Up next child windows.
void CreatePanels(HWND parent);
HWND NowPlayingPanel();
HWND UpNextPanel();
void PanelsTick();
void LoadPanels();
// Big mode: the docked, resizable panels painted onto one big surface.
void LayoutBig(HDWP& dwp, RECT r);
void PaintBig(HDC dc);
bool BigMouse(UINT msg, WPARAM wp, LPARAM lp);
bool BigSetCursor();
bool PanelOpen(int panel);
void TogglePanel(int panel);
void ResetPanels();
// Lyrics panel: parsed LRC / plain text synced to playback.
void CreateLyricsPanel(HWND parent);
void StopLyrics();
void ToggleLyricsPanel();
bool LyricsPanelShown();
int LyricsPanelWidth();
void LyricsTrackChanged();
void LyricsLibraryChanged();  // compaction renumbered track ids: drop the loaded lyrics
void LyricsTick();
void LyricsTheme();
std::vector<std::pair<double, std::wstring>> ParseLyrics(const std::wstring& text, bool& timed);
void Layout();
// Customize layout (app_custom.cpp): the normal window's docked player, its parts and presets.
void LoadMainLayout();
void SaveMainLayout();
std::wstring LayoutIniText(const MainLayout& layout);  // as a pack's layout.ini says it
void ApplyMainLayout();
bool SeekVertical();
void AlignTimes(bool column);
void LayoutNormal(HDWP& dwp, RECT r);
void PaintLayoutChrome(HDC dc);
bool LayoutMouse(UINT msg, LPARAM lp);
bool LayoutSetCursor(POINT pt);
HMENU LayoutMenu();
void UpdateLayoutMenu(HMENU m);
bool LayoutCommand(int id);
void ShowLayoutMenu(POINT pt);
void SyncMenuBar();
// The menu bar, attached or detached (the compact sizes and a hidden menu bar keep it aside).
HMENU MainMenuBar();
// Puts `bar` in place of the menu bar, attached or kept aside as the old one was; the caller frees the old.
void ReplaceMenuBar(HMENU bar);

}  // namespace app
