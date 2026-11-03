// The Linux front end's window, split by responsibility: the state, types and declarations that
// used to share one translation unit. The public surface stays in ui.h; this header is private to
// the UI implementation files, including panels.cpp.
#pragma once

#include "ui.h"

#include "app.h"
#include "archive.h"
#include "cue.h"
#include "draw.h"
#include "libview.h"
#include "panels.h"
#include "peaks.h"
#include "points.h"
#include "qr.h"
#include "radio_ui.h"
#include "steam.h"
#include "text.h"
#include "titleformat.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cwctype>
#include <deque>
#include <functional>
#include <map>
#include <numeric>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace lx {

using ui::kColumns;

// Menu geometry and the playback-speed steps, shared by the menu bar, its popups and the actions.
inline constexpr float kItemH = ui::kRow + 3, kSeparatorH = 8, kMenuPad = 4, kLabelX = 37, kShortcutRight = 25;
inline constexpr double kSpeeds[] = {0.5, 0.75, 0.9, 1.0, 1.1, 1.25, 1.5, 2.0};
inline constexpr double kTempos[] = {0.75, 0.9, 1.0, 1.1, 1.25, 1.5, 1.75, 2.0};

// --- geometry ------------------------------------------------------------------------------------
// The window's parts, from DoLayout. The names follow ui::kLayout.
struct Layout {
    Rect player, upNext, menuBar, grip, buttons[6], elapsed, seek, remaining, speaker, volume, cover, title, subtitle, chip, lyrics;
    Rect tabs, radioTab, search, viewRow, browser, splitter, list, header, rows, scrollbar, status, visuals[2];
    Rect radioSaved, radioDiscover, radioGenre, radioCountry, radioState, radioSearch, radioGrid;
    std::vector<Rect> tabRects, barItems;
    Rect views[kViewButtons];
    float rowH = 0;
};
extern Layout L;
enum Button { kBtnPrev, kBtnPlay, kBtnNext, kBtnShuffle, kBtnRepeat, kBtnResume };

// What the mouse is doing.
enum class Drag { None, Seek, Volume, Dock, Scroll, ColumnEdge, Tracks, Splitter };
extern uint64_t g_lastInput;
extern Drag g_drag;
extern int g_dragColumn;
extern float g_dragStart, g_dragStartValue;
extern double g_seekPreview;
extern float g_mouseX, g_mouseY;
extern uint64_t g_lastClick;
extern int g_lastClickLine;
extern bool g_altAlone;
extern float g_muteVolume;
extern TrackId g_sectionMenuTrack;
extern double g_sectionMenuPos;
struct TrackDrag {
    bool pending = false, collapseOnClick = false;
    float x = 0, y = 0;
    int clicked = -1, target = -1;
    uint64_t playlist = 0;
    std::vector<size_t> sources;
    std::vector<TrackId> snapshot;
};
extern TrackDrag g_trackDrag;

// --- album groups --------------------------------------------------------------------------------
// Consecutive tracks of one album (two or more) under a two-row header, as on Windows
// (app_groups.cpp). A display line is a track row (`row` >= 0) or a header's half (`row` < 0).
struct Group {
    int first = 0, count = 0;
    StringPool::Handle album = 0, artist = 0, date = 0;
    double seconds = 0;
};
struct Line {
    int row, group, part;
};
extern std::vector<Group> g_groups;
extern std::vector<Line> g_lines;
extern std::vector<int> g_lineOfRow;

// Album covers (the list's album headers and the views), by the album, its first track's folder and
// the size asked for. The views can ask for thousands, so the least recently drawn are let go past
// kThumbsKept.
struct Thumb {
    SDL_Texture* texture = nullptr;
    bool done = false;
    uint64_t used = 0;  // the frame it was last drawn in
};
inline constexpr size_t kThumbsKept = 600;
extern uint64_t g_frame;
extern std::unordered_map<uint64_t, Thumb> g_thumbs;
extern std::unordered_map<TrackId, uint64_t> g_thumbFor;  // requests in flight
extern int g_thumbsPending;

// Popup menus: the menu bar's, the list's and the header's. A stack: [0] is the menu, and each
// open submenu follows it.
enum Action {
    kNoAction,
    kAddFiles, kAddFolder, kOpenPlaylistFile, kSavePlaylistFile, kExit,
    kBackup, kRestore, kStatistics,
    kSelectAll, kRemove, kKeepSelected, kClear, kReloadTags, kRemoveDuplicates, kRemoveDead, kCancelDead, kFind,
    kPlayPause, kStopAction, kMute, kPrevAction, kNextAction, kShuffleAction, kShuffleAlbumsAction, kRadio, kRepeatSet, kStopAfter, kSpeed, kTempo, kShared, kExclusive,
    kNewPl, kRenamePl, kClosePl, kDuplicatePl, kReopenPl, kMovePlLeft, kMovePlRight, kNextPl, kPrevPl, kReveal,
    kFolders, kRescan, kAcceptInbox, kAcceptAllInbox, kExcludeLibrary, kDuplicateAudio, kCancelDuplicates,
    kSteam, kCancelSteam, kWebServer, kExportCord, kCancelExport, kConvert, kConvertUndo, kCancelConvert, kOrganize, kOrganizeUndo, kCopyTo, kCancelCopy,
    kIpodSync, kCancelIpodSync,
    kTheme, kOpenRadio, kGroupAlbums, kStartOnLogin, kNameThatTrack, kWhichCameFirst, kPoints, kReview, kPalette, kLyrics, kLayout, kViewSize, kBigPanel, kBigReset, kMini, kPacks, kPackPlaylist, kHelpers, kHelperTool, kHelperCancel,
    kAbout,
    kPlay, kPlayNext, kAddToQueue, kClearQueue, kAddToNowPlaying, kAddToPlaylist, kRemoveFromNowPlaying, kOpenFolder, kColumnToggle, kPackColumnSet,
    kSectionPlay, kSectionLoop, kSectionStart, kSectionEnd, kSectionClear,
    kRating, kEditTags, kUndoTags, kUndo, kRedo, kEqualizer, kPreferences, kListenBrainz, kRgEnable, kRgScanSel, kRgScanPl, kRgCancel, kRgClear, kRgWrite, kWriteStatistics,
    kNewAuto, kEditAuto, kChoice, kGroupEditions, kPreferredEdition, kRemoveEdition, kSplitEditions,
    kAlbumEditions, kEditionVersions, kRgScanAll, kFindCover, kFetchCovers, kCancelFetchCovers,
    kSaveLyricsTags, kSaveLyricsLrc, kArtistInfo, kRestoreCover,
};
extern std::function<void(int)> g_choicePick;  // the choice a panel's OpenChoice menu is for
struct MenuItem {
    std::string label, shortcut;
    int action = kNoAction, arg = 0;
    bool checked = false, radio = false, enabled = true, separator = false;
    std::vector<MenuItem> sub;
};
struct Popup {
    float x = 0, y = 0, w = 0, h = 0;
    std::vector<MenuItem> items;
    int hover = -1;
};
extern std::vector<Popup> g_popups;
extern int g_barOpen;  // the menu bar's open menu, or -1

struct ClosedPlaylist { Playlist playlist; int index; };
extern std::deque<ClosedPlaylist> g_closedPlaylists;
extern int g_renameIndex;
extern std::wstring g_renameText;
extern bool g_renameSelected;
extern size_t g_renameCaret;
extern uint64_t g_nextPlaylistIdentity;

struct PlaylistFile {
    bool save = false, ok = false, failed = false, addToExisting = false, checkDead = false;
    bool findDuplicates = false, findSteam = false, cancelled = false;
    std::wstring path;
    std::vector<M3UExportEntry> entries;
    std::vector<std::wstring> paths;
    std::vector<std::wstring> inputs;
    std::vector<std::wstring> watchedFolders;
    std::vector<SteamSong> steamSongs;
    SteamImport steam;
    std::vector<TrackId> snapshot;
    std::vector<TrackId> undoBefore;  // an import's target before its first batch, for Undo
    std::vector<std::pair<TrackId, std::wstring>> checkedPaths, missing;
    std::vector<std::pair<uint64_t, TrackId>> hashes;
    std::vector<std::vector<TrackId>> duplicateGroups;
    size_t done = 0;
    size_t added = 0, failedInputs = 0;
    uint64_t target = 0;
};
extern std::thread g_playlistFileWorker;
extern std::thread g_steamOfferWorker;
extern bool g_playlistFileBusy, g_playlistFileStopping;
extern bool g_checkingDead, g_checkingDuplicates, g_checkingSteam;
extern std::atomic<bool> g_playlistFileCancel;
extern std::atomic<bool> g_steamOfferCancel;
extern std::unique_ptr<PlaylistFile> g_playlistImport;

// The library folders dialog.
struct Folders {
    bool open = false;
    Rect panel, add, music, done;
    std::vector<Rect> remove;
};
extern Folders g_folders;
struct ExcludeConfirm {
    bool open = false;
    std::vector<TrackId> ids;
    Rect panel, confirm, cancel;
};
extern ExcludeConfirm g_exclude;
extern bool g_aboutOpen;
// The Web server window (File > Web server): its controls are 0 the port, 1 Start/Stop, 2 Start with cordinal,
// 3 Pair device, 4 Remove all, 5 Close, 6 Open web player, 7 Copy phone address, 8 Copy code, 9 Listen away
// from home, 10 Check again, 11 Copy internet address, 12 the fixed code and 13 Use this code.
inline constexpr int kWebControls = 14;
struct WebPanel {
    bool open = false;
    std::string port, fixed;
    int focus = 1, scroll = 0;
    Rect panel, controls[kWebControls];
    std::vector<std::pair<Rect, std::string>> addresses, devices;
    std::string qrText;  // what `qr` encodes
    qr::Code qr;
};
extern WebPanel g_web;

// The seek bar's waveform (app_waveform.cpp): the playing track's peaks, computed in the background
// the first time it plays and cached; its columns fill as the worker decodes an uncached track.
struct Wave {
    TrackId track = kNone;  // the track `peaks` belongs to (or is being computed for)
    bool ready = false;
    uint32_t request = 0;   // the background request `track` waits on (0 = none)
    Peaks peaks{};
};
extern Wave g_wave;

extern SDL_Texture* g_cover;
extern SDL_Texture* g_disc;
extern TrackId g_coverFor, g_coverWanted;
extern double g_coverHue;
extern uint32_t g_lastRebuild;
extern size_t g_sourceCount;  // active tab's tracks before the search
extern std::unordered_map<uint64_t, std::string> g_cells;  // formatted cells by track and column
extern uint32_t g_cellsRevision;
extern AudioVisuals g_visuals;
extern uint64_t g_visualAt;

// The About box's cardinal, drawn lazily at the size its panel gives it.
struct AboutBox {
    Rect icon;
    SDL_Texture* texture = nullptr;
    int textureSize = 0;
};
extern AboutBox g_about;
// Radio station icons by URL, kept for the life of the window.
extern std::map<std::string, SDL_Texture*> g_radioArt;

// The transport glyphs, in the thin line style of Segoe MDL2 Assets, `s` pixels tall at the center
// of `r`.
enum class Icon { Prev, Play, Pause, Next, Shuffle, Repeat, RepeatOne, Resume, Speaker };

MenuItem Item(std::string label, int action, std::string shortcut = {}, bool checked = false, bool enabled = true);
MenuItem Separator();
MenuItem Radio(std::string label, int action, int arg, bool on, std::string shortcut = {});
MenuItem Submenu(std::string label, std::vector<MenuItem> items);
std::vector<MenuItem> CordExportItems();
std::vector<MenuItem> RateItems(const double* rates, size_t count, double selected, int action);
void Circle(float cx, float cy, float r, uint32_t rgb);
std::vector<SDL_FPoint> RoundPath(const Rect& r, float rad);
void FillRound(const Rect& r, float rad, uint32_t rgb, float alpha = 1);
void FrameRound(const Rect& r, float rad, uint32_t rgb);
std::string Plain(const wchar_t* label);
void DrawIcon(Icon icon, const Rect& r, uint32_t rgb);
Playlist& Active();
int NowPlayingIndex();
FieldLookup Lookup(TrackId id, bool withPlayback);
const std::string& Cell(TrackId id, int column);
int CompareNatural(const std::wstring& a, const std::wstring& b);
int CompareText(std::wstring_view a, std::wstring_view b);
bool LibraryLess(TrackId x, TrackId y);
std::vector<TrackId> PlaylistTracks(const Playlist& pl);
void SortPlaylistTracks(std::vector<TrackId>& tracks, const Playlist& pl);
bool SortKeepsAlbums(int column);
bool SameAlbum(const Track& a, const Track& b);
void BuildLines();
bool GroupHidesCell(const Line& line, int column);
float ColumnWidth(int column);
float CellTextWidth(const Line& line, size_t index, float width);
std::string TabName(size_t i);
void DoLayout();
float ContentHeight();
float MaxScroll();
void ClampScroll();
void EnsureVisible(int row);
int LineAt(float y);
std::vector<TrackId> SelectedTracks();
void AcceptInbox(const std::vector<TrackId>& ids);
void SetRating(const std::vector<TrackId>& ids, uint8_t rating);
void ExcludeFromLibrary(const std::vector<TrackId>& ids);
void ConfirmExclude(bool confirm);
std::vector<MenuItem> ReplayGainMenu();
std::vector<MenuItem> PlaylistDestinations();
bool CanvasHasFocusedEdition();
std::vector<MenuItem> BarMenu(int m);
std::vector<int> BarMenus();
float PopupItemHeight(const MenuItem& it);
void OpenPopup(float x, float y, std::vector<MenuItem> items, bool submenu = false);
Rect PopupItemRect(const Popup& p, size_t i);
void CloseMenus();
void OpenBarMenu(int index);
void OpenRowMenu(float x, float y);
void OpenHeaderMenu(float x, float y);
void ShowFolder(TrackId id);
void InsertIntoNowPlaying(const std::vector<TrackId>& ids, bool next);
void AddToPlaylist(int index, const std::vector<TrackId>& ids);
void AddDropped(const std::vector<std::wstring>& files);
void OpenChoice(float x, float y, const std::vector<std::string>& labels, int selected, std::function<void(int)> pick);
size_t RemoveOccurrences(const std::vector<char>& remove, int index = -1, const wchar_t* what = L"remove");
void RemoveFromPlaylist(bool all, bool keepSelected = false);
// Undo / redo of the active playlist's contents; `description` names the step ("Undo remove").
bool CanStepPlaylistEdit(bool redo, std::wstring* description = nullptr);
void StepPlaylistEdit(bool redo);
// Ctrl+Z: undoes whichever came last, a change to the active playlist or a tag edit.
void UndoLatest();
void RemoveDuplicateTracks();
void CheckDeadItems();
void FindDuplicateAudio();
void AddSteamSoundtracks();
std::vector<std::wstring> NewSteamFolders(const std::vector<std::wstring>& folders, const std::atomic<bool>& cancel, const std::vector<std::wstring>* discovered = nullptr);
void SetTheme(ThemeMode mode);
void RevealPlaying();
void FilesChosen(void* data, const char* const* list, int);
void FolderDialogCallback(void*, const char* const* list, int);
void ChooseFolder();
std::unique_ptr<PlaylistFile> NewPathImport();
void ChooseImportFolder();
void ChooseFiles();
void PlaylistFileChosen(void* data, const char* const* list, int);
void ChoosePlaylistFile(bool save);
void EndRename(bool commit);
void BeginRename(int index);
void NewPlaylist();
void DuplicatePlaylist();
void ClosePlaylist(int index);
void ReopenPlaylist();
void ReorderPlaylist(int direction);
void ToggleMute();
void RunAction(const MenuItem& item);
void SelectOnly(int row);
void SelectRange(int from, int to);
void MoveFocus(int row, bool extend);
void DrawFoldersDialog(const Palette& p);
void DrawExcludeConfirm(const Palette& p);
bool FoldersClick(float x, float y);
void WebAction(int control);
void DrawPairingCode(const Rect& r, const Palette& p);
void DrawWeb(const Palette& p);
void WebClick(float x, float y);
std::string AboutMemory();
void DrawAbout(const Palette& p);
void AboutClick(float x, float y);
void DrawMenuBar(const Palette& p);
void DrawPopups(const Palette& p);
void DrawBandButton(const Rect& r, Icon icon, const Palette& p, bool active = false, bool enabled = true);
void DrawSlider(const Rect& r, double frac, bool enabled, bool hot, const Palette& p, bool vertical = false,
                double notch = 0);
bool PeakSource(TrackId id, std::wstring& path, double& start, double& end);
bool RequestPeaks(TrackId id, Peaks& peaks, uint32_t& request);
bool LiveWaveformShown();
bool LiveFountainShown();
bool DrawWaveform(const Rect& r, double frac, const Palette& p);
void DrawSection(const Rect& r, double duration, const Palette& p);
double TrackDuration();
void DrawBand(const Palette& p);
int PlayingTab();
void DrawTabs(const Palette& p);
int RadioColumns();
float RadioMaxScroll();
Rect RadioCard(int index);
void DrawRadio(const Palette& p);
void DrawVisuals(const Palette& p);
void DrawGroupCover(const Group& gr, const Rect& r, const Palette& p);
void DrawGroupHeader(const Group& gr, int index, float top, float right, const Palette& p);
void DrawList(const Palette& p);
std::wstring StatusMessage();
void DrawStatus(const Palette& p);
int MenuHit(float x, float y, int* popup);
void HoverMenu(int popup, int item);
int BarItemAt(float x, float y);
float Density();
bool SeekVertical();
double SeekValueAt(float x, float y);
std::string SectionTime(double seconds);
void OpenSectionMenu(float x, float y);
void SectionAction(int action);
float VolumeAt(float x);
int ColumnEdgeAt(float x, float y);
int ColumnAt(float x);
void SortBy(int c);
void ClickGroup(int group, bool add);
void PrepareTrackDrag(float x, float y, int clicked, bool collapseOnClick);
int TrackDropTarget(float y);
void MoveDraggedTracks();
void MouseDown(const SDL_MouseButtonEvent& e);
void MouseMove(const SDL_MouseMotionEvent& e);
void MouseUp(const SDL_MouseButtonEvent& e);
void Wheel(const SDL_MouseWheelEvent& e);
void MenuKey(SDL_Keycode key);
void Key(const SDL_KeyboardEvent& e);
void KeyUp(const SDL_KeyboardEvent& e);
void TextInput(const SDL_TextInputEvent& e);
void Draw();
std::vector<PaletteCommand> MenuCommands();
void RunMenuCommand(size_t index);
uint64_t LastInputTicks();
SDL_Texture* MiniCover();
std::wstring FormatForTrack(const std::wstring& pattern, TrackId id, bool withPlayback);
void UpdateWindowTitle();
void MaybeOfferSteamSoundtracks();
void UpdateInboxTab();
void OpenLibraryFolders();
void OpenWebServer();
const Palette& CurrentPalette();
void ApplyTheme();
bool UiAnimating();
void ShowMessage(const std::wstring& text);
bool Confirm(const std::string& question, const char* title);
void OnPlaylistFile(WPARAM kind, LPARAM value);
bool PlaylistImportPending();
void TickPlaylistImport();
void StopPlaylistFiles();
void InvalidateCellCache();
void RebuildRows();
void SetActiveTab(int tab);
void InitUi();
void ShutdownUi();
void OnScaleChanged();
void RefreshTrackArt(TrackId id);
void SetCover(CoverImage* image);
void SetThumb(CoverImage* image);
void OnFileChosen(int purpose, std::wstring* path);
void OnFoldersChosen(std::vector<std::wstring>* folders);
void Render();
void HandleEvent(const SDL_Event& e);
int CheckLayout();
bool UiBusy();
void PrepareWaveform(TrackId id);
void PrefetchWaveform(TrackId id);
void OnPeaksReady(uint32_t request);
int CaptureUi(const std::string& dir);
void DrawAlbumCover(TrackId first, StringPool::Handle album, const std::wstring& title, const Rect& r, const Palette& p);
void OpenTrackMenu(float x, float y);
void RateTracks(const std::vector<TrackId>& ids, uint8_t rating);


}  // namespace lx
