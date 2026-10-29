// Helpers in the player (app_helpers.cpp): which helpers in <data>\helpers are on, their tools and
// visualizers, and where they meet the window. The helpers themselves are helpers.h.
#pragma once
#include "app_state.h"

namespace app {

// Reads the helpers folder and starts the helpers that are on. Runs before the main window exists;
// in safe mode (Shift held at start, or --safe), or when cordinal runs as administrator, no helper runs.
void LoadHelpers(bool safe);
// View > Helpers (built fresh for each menu bar) and its commands, the tools' Run menu included.
HMENU HelpersMenu();
bool HelperMenuCommand(int id);
// A "Run" submenu with the tools of the helpers that are on, for the Edit menu and the track list's
// right-click menu; nothing when there are none. `selected`: tracks are selected.
void AppendHelperTools(HMENU menu, bool selected);
// Checkmarks and enabled state of the helper items in the menu bar.
void UpdateHelperMenu(HMENU menu);
// The tool that runs: its label, or empty. Its tracks keep their ids until it finishes.
std::wstring HelperToolRunning();
// Its place in the Run menu (-1 when none runs), and the Cancel item that follows it there.
int HelperToolIndex();
std::wstring HelperCancelText();
std::wstring HelperToolStatus();  // "Running Measure loudness 3/12"
void OnHelperMessage(WPARAM wp, LPARAM lp);
// The visual feed's timer (about 60 times a second while a visualizer runs).
bool HelperTimer(UINT_PTR id);
// The helper that decodes this track, or empty when cordinal decodes it itself.
std::wstring HelperDecoding(TrackId id);
// Ends every helper program and the running tool (cordinal is closing).
void StopHelpers();

}  // namespace app
