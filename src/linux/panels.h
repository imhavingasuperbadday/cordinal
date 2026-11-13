// The Linux window's larger dialogs, drawn over it like the folders and web server panels in ui.cpp:
// the equalizer (Playback > Equalizer), the tag editor (Edit > Edit tags), Preferences (File >
// Preferences) and the rest. One is open at a time, and while it is, it takes the mouse and keyboard.
// panels.cpp dispatches to them; each lives in its own panel_*.cpp (panels_internal.h lists them).
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
enum FilePurpose : int {
    kFileCover = 1, kFileAutoEq = 2, kFileOrganizeRoot = 3, kFileCopyDest = 4, kFilePack = 5, kFileHelper = 6,
    kFileCoverPick = 7,  // the cover picker's From file...
    kFileCoverDirect = 8,  // the track menu's Cover > Choose image file...
    kFileAlbumCover = 9    // the album maker's Choose cover...
};

void OpenEqualizer();
void OpenTagEditor(const std::vector<TrackId>& ids);
// Edit > Make album (panel_album.cpp, as app_albummaker.cpp): the tracks given become one album.
void OpenAlbumMaker(const std::vector<TrackId>& ids);
void OpenPreferences();
int CheckPreferences();  // headless control geometry, state and keyboard checks; saves no images
int CheckTagSuggestions();  // headless tag-editor completion check; saves no images
void OpenRules(int playlist);  // -1: a new auto-playlist; else that auto-playlist's rules
void OpenOutputDevice();  // choose the PipeWire sink to play on (Preferences > Output)
void OpenPoints();  // cordinal points (View > Games and stats, or the chip)
void OpenReview();  // Your year in music
// games.cpp: Name That Track (0) and Which Came First (1), View > Games and stats. The panel is one with two
// tabs; OpenGames shows it on the game asked for.
void OpenGames(int which);
bool GamesOpen();
void ShowGame(int which);
void ResetGameRenderer(bool restore);
void ResetPanelRenderer(bool restore);
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
void OpenListenBrainz();  // the service connections panel (Preferences > Scrobbling)
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
// coverfetch_ui.cpp: covers, as app_coverfetch.cpp does on Windows. The picker (Change cover...) and
// the review of Library > Fetch missing covers are panels; the tag editor opens the picker over
// itself and gets the chosen image back. The picker takes a file chosen for it and a dropped file,
// image or address (false when it isn't open to take one); custom covers are restored per track.
void OpenCoverSearch(const std::vector<TrackId>& ids);
void OpenCoverSearchForTags(const std::vector<TrackId>& ids, std::function<void(const std::string&)> pick);
void CoverPanelFile(const std::wstring& path);
bool CoverPanelDrop(const char* data, bool file);
bool HasCustomCover(const std::vector<TrackId>& ids);
void RestoreOriginalCovers(const std::vector<TrackId>& ids);
// Covers without the picker: an image file chosen in a dialog, the clipboard's picture, or an image
// file dropped on the window becomes the cover of the whole album of `ids` (AlbumTracksOf), kept as
// Preferences > Online says. A pasted address still opens the picker to download it. DropCover is
// false when the drop isn't an image for it (the picker is open, or the file isn't JPEG or PNG).
bool IsCoverImageFile(const std::wstring& path);
void ChooseCoverFile(const std::vector<TrackId>& ids);
void CoverFileChosen(const std::wstring& path);  // kFileCoverDirect
void SetCoverFromFile(const std::vector<TrackId>& ids, const std::wstring& file);
bool ClipboardMayHoldCover();
void PasteCover(const std::vector<TrackId>& ids);
bool DropCover(const char* data, float x, float y);
void FetchMissingCovers();
void CancelCoverFetch();
bool CoverFetchBusy();
void OnCoverFetchMessage(WPARAM wp, LPARAM lp);  // WM_APP_COVERFETCH
void StopCoverFetch();
void DrawCoverPanel(const Palette& p, float mouseX, float mouseY);
void CoverPanelMouseDown(float x, float y, int button, int clicks);
void CoverPanelKey(const SDL_KeyboardEvent& e);
void CoverPanelText(const char* text);
void CoverPanelWheel(float x, float y, float dy);
bool CoverPanelWantsText(float x, float y);
void CoverPanelClose();               // from ClosePanel: stops the search and lets the images go
void CoverPanelRenderer(bool restore);  // the renderer is being replaced
// panels.cpp: shows the cover panel (over the tag editor when `overTags`), and leaves it, back to
// the tag editor that opened it or closed.
void ShowCoverPanel(bool overTags);
void CoverPanelDone();
// artistinfo_ui.cpp: Artist info (View > Artist info...), a panel like the others; panels.cpp shows it.
void OpenArtistInfo();
void OnArtistInfoMessage(LPARAM lp);  // WM_APP_ARTISTINFO
void StopArtistInfo();
void DrawArtistPanel(const Palette& p);
void ArtistPanelMouseDown(float x, float y);
void ArtistPanelKey(const SDL_KeyboardEvent& e);
void ArtistPanelWheel(float x, float y, float dy);
void ArtistPanelClose();
void ShowArtistPanel();  // panels.cpp
// collection_run.cpp: File > Export collection's Select what to export, a panel like the others;
// panels.cpp shows it, and ClosePanel calls ExportPanelClose last (Export... then asks for the file).
void ShowExportPanel();  // panels.cpp
void DrawExportPanel(const Palette& p);
void ExportPanelMouseDown(float x, float y);
void ExportPanelKey(const SDL_KeyboardEvent& e);
void ExportPanelWheel(float x, float y, float dy);
void ExportPanelClose();
// ui.cpp: drops the decoded covers of the views and the now-playing pane, after covers changed on disk.
void ForgetCoverArt();
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
