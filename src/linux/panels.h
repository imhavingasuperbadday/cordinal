// The Linux window's larger dialogs, drawn over it like the folders and web server panels in ui.cpp:
// the equalizer (Playback > Equalizer), the tag editor (Edit > Edit tags) and Preferences (File >
// Preferences). One is open at a time, and while it is, it takes the mouse and keyboard.
#pragma once
#include "app.h"

namespace lx {

// One command of the menu bar, for the command palette.
struct PaletteCommand {
    std::string label, path, shortcut;  // the menu path, e.g. "Playback \xE2\x80\xBA Repeat"
    bool enabled = true, checked = false;
};
std::vector<PaletteCommand> MenuCommands();  // ui.cpp: every item of the menu bar as it would show now
void RunMenuCommand(size_t index);           // ui.cpp: runs the index-th item of the last MenuCommands()

// What a file chosen in a dialog is for (WM_APP_FILE's wParam).
enum FilePurpose : int { kFileCover = 1, kFileAutoEq = 2, kFileOrganizeRoot = 3, kFileCopyDest = 4, kFilePack = 5, kFileHelper = 6 };

void OpenEqualizer();
void OpenTagEditor(const std::vector<TrackId>& ids);
void OpenPreferences();
void OpenRules(int playlist);  // -1: a new auto-playlist; else that auto-playlist's rules
void OpenOutputDevice();  // choose the PipeWire sink to play on (Preferences > Output)
void OpenPoints();  // cordinal points (View > Games and stats, or the chip)
void OpenReview();  // Your year in music
// games.cpp: Name That Track (0) and Which Came First (1), View > Games and stats. The panel is one with two
// tabs; OpenGames shows it on the game asked for.
void OpenGames(int which);
bool GamesOpen();
void ShowGame(int which);
void CloseGames();  // from ClosePanel: stops the clip, joins the workers and lets the covers go
void DrawGames(const Palette& p, float mouseX, float mouseY);
void GamesMouseDown(float x, float y);
void GamesKey(const SDL_KeyboardEvent& e);
void GamesText(const std::wstring& text);
bool GamesWantsText(float x, float y);
bool GamesAnimating();  // a clip plays, or the next pair is on its way
void OnGameResult(LPARAM lp);  // WM_APP_GAME
void OpenHelpers();  // View > Helpers
void OpenPacks();    // View > Packs
void OpenLayout();   // View > Customize layout
void OpenPalette();  // View > Command palette (Ctrl+Shift+P)
void OpenFormat();       // the title formatting window (Preferences > View)
void OpenListenBrainz();  // the token window (Preferences > Library)
void OnListenBrainzMessage(WPARAM wp, LPARAM lp);  // WM_APP_LISTENBRAINZ: a token check's answer
// Album editions: the manager and its track-comparison view. Modeless overlays like every panel.
void EditionsOpenManager(uint32_t group);
void EditionsOpenComparison(uint32_t group, TrackId focusTrack);
void OpenAlbumEditions(uint32_t group);
void OpenAlbumEditionComparison(uint32_t group, TrackId focusTrack = kNone);
void DrawEditionsPanel(const Palette& p);
void EditionsMouseDown(float x, float y, int button, int clicks);
void EditionsKey(const SDL_KeyboardEvent& e);
void EditionsText(const std::string& text);
void EditionsWheel(float x, float y, float dy);
bool EditionsWantsText(float x, float y);
void EditionsClose();
void OpenOrganize(std::vector<TrackId> ids);  // the tracks given, or the whole library when there are none
// Library > Copy to folder or device: `playlist` names the playlist the tracks are (its .m3u8, and the
// copies it owns), empty for a selection.
void OpenCopy(const std::vector<TrackId>& ids, const std::wstring& playlist);
bool PanelReplanPending();  // the organize preview waits to be replanned once typing pauses
// ui.cpp: the window's own panels, which Preferences links to.
void OpenLibraryFolders();
void OpenWebServer();
bool PanelOpen();
void ClosePanel();

void DrawPanel(const Palette& p, float mouseX, float mouseY);
void PanelMouseDown(float x, float y, int button, int clicks);
void PanelMouseMove(float x, float y);
void PanelMouseUp();
void PanelWheel(float x, float y, float dy);
void PanelKey(const SDL_KeyboardEvent& e);
void PanelText(const char* text);
void PanelFileChosen(int purpose, const std::wstring& path);
bool PanelWantsText(float x, float y);  // over a text field (for the I-beam cursor)

}  // namespace lx
