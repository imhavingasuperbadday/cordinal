// Shared state and declarations for the main window modules (app_*.cpp).
// Defines the global `g` App, the App message/control/command ids and small shared
// helpers. State is UI-thread owned; worker threads touch `g` only through the
// mutexes and atomics declared beside their fields, or by posting WM_APP_* messages.
#pragma once
#include "art.h"
#include "autoplaylist.h"
#include "cue.h"
#include "decoder.h"
#include "dsp.h"
#include "engine.h"
#include "headsets.h"
#include "library.h"
#include "playlist_shuffle.h"
#include "points.h"
#include "replaygain.h"
#include "resource.h"
#include "scanner.h"
#include "sfx.h"
#include "sort.h"
#include "tagwriter.h"
#include "tags.h"
#include "theme_palette.h"
#include "theme_win.h"
#include "titleformat.h"
#include "ui_spec.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <thread>
#include <unordered_map>
#include <unordered_set>

using Microsoft::WRL::ComPtr;

// Diagnostics run directly in cordinal-test.exe; the player forwards the same commands to it.
// Empty for ordinary player arguments; otherwise preserves the full diagnostic exit code.
std::optional<int> RunTestMode(const std::vector<std::wstring>& args);

namespace app {

// Posted-message ids the UI thread handles in WndProc; wParam/lParam layouts are
// noted inline where they carry a payload. Keeping work on the UI thread via posted
// messages is what lets workers stay lock-free with respect to the window.
constexpr UINT WM_APP_META = WM_APP + 2;
constexpr UINT WM_APP_RG = WM_APP + 3;
constexpr UINT WM_APP_SCAN = WM_APP + 4;
constexpr UINT WM_APP_BROWSE = WM_APP + 5;
constexpr UINT WM_APP_TAGWRITE = WM_APP + 6;
constexpr UINT WM_APP_ORGANIZE = WM_APP + 7;
constexpr UINT WM_APP_IMPORT = WM_APP + 8;
constexpr UINT WM_APP_ART = WM_APP + 9;
constexpr UINT WM_APP_CONVERT = WM_APP + 10;  // wParam 1: lParam is a ConvertResult; else lParam = files started
constexpr UINT WM_APP_COPY = WM_APP + 11;     // wParam 1: lParam is a CopyResult; else lParam = the plan index started
// How long conversions stay undoable (Preferences > Conversion), in days, in the order offered.
constexpr unsigned kConvertMemoryDays[] = {1, 3, 7, 49};
// Playback speed (pitch follows), tempo (pitch stays) and reverb amount, in menu order.
constexpr double kSpeeds[] = {0.5, 0.75, 0.9, 1.0, 1.1, 1.25, 1.5, 2.0};
constexpr double kTempos[] = {0.75, 0.9, 1.0, 1.1, 1.25, 1.5, 1.75, 2.0};
constexpr float kReverbMixes[] = {0.25f, 0.4f, 0.55f, 0.75f};
constexpr UINT WM_APP_SMTC = WM_APP + 60;          // wParam = smtc::Button
constexpr UINT WM_APP_LISTENBRAINZ = WM_APP + 61;  // see listenbrainz.h
constexpr UINT WM_APP_PEAKS = WM_APP + 62;         // wParam = peaks request id
constexpr UINT WM_APP_GROUP_ART = WM_APP + 63;     // album group header covers are ready
constexpr UINT WM_APP_TRAY = WM_APP + 64;          // the notification-area icon's callback
constexpr UINT WM_APP_STARTUP = WM_APP + 65;       // first frame is up: finish optional setup
constexpr UINT WM_APP_WEB = WM_APP + 66;           // server status/listening events are ready
constexpr UINT WM_APP_BACKUP = WM_APP + 67;        // lParam owns a backup worker result
constexpr UINT WM_APP_HELPER = WM_APP + 68;        // a helper tool's progress (wParam 0), or its result (1: lParam owns it)
constexpr UINT_PTR kSearchTimer = 3;
constexpr UINT_PTR kUiTimer = 1;
constexpr UINT_PTR kBusyTimer = 5;  // animates the busy spinners (app_badges.cpp)
constexpr UINT_PTR kStripTimer = 2;  // the strip's controls show while the mouse is over it
constexpr ULONG_PTR kCopyDataPaths = 0x43444650;    // 'CDFP': paths to add and play
constexpr ULONG_PTR kCopyDataAddPaths = 0x43444641;  // 'CDFA': paths to add without playing
constexpr ULONG_PTR kCopyDataCommand = 0x43444643;  // 'CDFC': a transport command
constexpr TrackId kNone = UINT32_MAX;
inline constexpr wchar_t kClassName[] = L"CordinalMainWindow";
inline constexpr wchar_t kEqClass[] = L"CordinalEqualizer";
inline constexpr wchar_t kHeadphonesClass[] = L"CordinalHeadphones";
inline constexpr wchar_t kEqGraphClass[] = L"CordinalEqGraph";
inline constexpr wchar_t kFoldersClass[] = L"CordinalLibraryFolders";
inline constexpr wchar_t kAutoClass[] = L"CordinalAutoPlaylist";
inline constexpr wchar_t kAppName[] = L"cordinal";

// Control ids. The base+N blocks (IDC_BROWSER0, IDC_VIEW0, IDC_EQ_SLIDER0, ...) let
// handlers map an id back to an index by subtraction instead of a lookup table.
enum : int {
    IDC_LIST = 2001, IDC_TABS, IDC_SEEK, IDC_VOLUME, IDC_SEARCH, IDC_ORDER, IDC_STATUS,
    IDC_NP_TITLE, IDC_NP_SUB, IDC_TIME, IDC_SPEAKER, IDC_RENAME,
    IDC_PREV, IDC_PLAY, IDC_PAUSE, IDC_STOP, IDC_NEXT,
    IDC_BROWSER0, IDC_BROWSER1, IDC_BROWSER2, IDC_VIEW0, IDC_VIEW1, IDC_VIEW2, IDC_VIEW3, IDC_VIEW4, IDC_LIBVIEW,
    IDC_FOLDERS_LIST, IDC_FOLDERS_ADD, IDC_FOLDERS_REMOVE, IDC_FOLDERS_RESCAN, IDC_FOLDERS_HINT,
    IDC_EQ_ENABLE, IDC_EQ_LABEL, IDC_EQ_PRESET, IDC_EQ_RESET, IDC_EQ_SLIDER0,  // + kEqBands
    IDC_EQ_PARAMETRIC = 2850, IDC_EQ_CROSSFEED, IDC_EQ_IMPORT, IDC_EQ_APPLY, IDC_EQ_TEXT, IDC_EQ_HINT,
    IDC_EQ_DEVICE, IDC_EQ_REMEMBER, IDC_EQ_HARDWARE, IDC_EQ_HW_STATUS, IDC_EQ_HW_REFRESH, IDC_EQ_HW_APPLY,
    IDC_EQ_SIDETONE, IDC_EQ_IDLE, IDC_EQ_DND, IDC_EQ_HW_PRESET, IDC_EQ_SIDETONE_LABEL, IDC_EQ_IDLE_LABEL,
    IDC_EQ_CORDINAL_PAGE, IDC_EQ_HEADPHONE_PAGE, IDC_EQ_PAGE_HINT,
    IDC_EQ_HW_CURVE,
    IDC_EQ_RG_HEADROOM, IDC_EQ_RG_HEADROOM_LABEL,
    IDC_EQ_GRAPH, IDC_EQ_AUTO_PREAMP,
    IDC_SHUFFLE = 2900, IDC_REPEAT, IDC_RESUME,
    IDC_ELAPSED = 2910, IDC_LYRICS,
    IDC_FB_TREE = 2920, IDC_FB_VIEW, IDC_FB_FILTER, IDC_FB_PROPS,  // the foobar2000 theme's Album List panel
    IDC_FOLDERS_MUSIC,
    IDC_RADIO_TAB = 2930,
};

// Menu/command ids. The trailing ranges (IDM_*_BASE) reserve contiguous blocks so
// menus and the dispatch in app_commands.cpp can decode them by arithmetic.
enum : int {
    IDM_ADD_FILES = 3001, IDM_ADD_FOLDER, IDM_IMPORT_M3U, IDM_EXPORT_M3U, IDM_EXIT,
    IDM_SELECT_ALL, IDM_REMOVE, IDM_CROP, IDM_CLEAR, IDM_REMOVE_DUPES, IDM_REMOVE_DEAD, IDM_FIND,
    IDM_PLAY_PAUSE, IDM_STOP, IDM_PREV, IDM_NEXT, IDM_STOP_AFTER,
    IDM_NEW_PL, IDM_RENAME_PL, IDM_REMOVE_PL, IDM_NEXT_PL, IDM_PREV_PL,
    IDM_ALWAYS_ON_TOP, IDM_ABOUT, IDM_PLAY_SEL, IDM_SHOW_IN_EXPLORER, IDM_PROPERTIES,
    IDM_EQUALIZER, IDM_RG_SCAN_SEL, IDM_RG_SCAN_PL, IDM_RG_ENABLE,
    IDM_RG_AUTO_SCAN, IDM_RG_CANCEL, IDM_RG_CLEAR, IDM_RG_WRITE_TAGS,
    IDM_LIBRARY_FOLDERS, IDM_RESCAN, IDM_REMOVE_MISSING, IDM_INBOX_ACCEPT, IDM_INBOX_ACCEPT_ALL, IDM_EXCLUDE,
    IDM_COLUMNS_RESET, IDM_QUEUE_NEXT, IDM_QUEUE_ADD, IDM_QUEUE_CLEAR, IDM_NEW_AUTO, IDM_EDIT_AUTO, IDM_ORGANIZE, IDM_ORGANIZE_UNDO,
    IDM_TAG_UNDO, IDM_OUTPUT_SHARED, IDM_OUTPUT_EXCLUSIVE,
    IDM_SOUND_EFFECTS, IDM_FORMAT, IDM_TASKBAR_PROGRESS, IDM_GAME_NAME_THAT_TRACK, IDM_GAME_WHICH_CAME_FIRST,
    IDM_POINTS, IDM_REVIEW, IDM_LISTENBRAINZ, IDM_STEAM, IDM_DUPLICATES, IDM_MUTE,
    IDM_GROUP_ALBUMS, IDM_MINI_PLAYER, IDM_LYRICS,
    IDM_TRAY_SHOW, IDM_START_WITH_WINDOWS,
    IDM_SMALLER, IDM_LARGER, IDM_PANELS_RESET, IDM_STARTUP_DEFAULTS,
    IDM_PREFERENCE_ASK, IDM_PREFERENCE_AUTO, IDM_PREFERENCE_MANUAL,
    IDM_REGION_PLAY, IDM_REGION_START, IDM_REGION_END,  // the seek bar's right-click menu (app_region.cpp)
    IDM_WEB_SERVER, IDM_PREFERENCES, IDM_COMMAND_PALETTE,
    IDM_BACKUP, IDM_RESTORE_BACKUP, IDM_IMPORT_STATISTICS,
    IDM_CANCEL_BACKUP, IDM_OPEN_RADIO, IDM_WRITE_STATISTICS,
    IDM_DUPLICATE_PL = 4000, IDM_REOPEN_PL, IDM_MOVE_PL_LEFT, IDM_MOVE_PL_RIGHT, IDM_PLAYING_PL,
    IDM_RELOAD_TAGS, IDM_CANCEL_IMPORT, IDM_WATCH_IMPORTED, IDM_SELECT_PL_BASE = 4020,
    IDM_SHUFFLE = 3200, IDM_REPEAT_BASE = 3201,  // + Repeat
    IDM_RESUME = 3205, IDM_SKIP_SHUFFLE, IDM_RADIO, IDM_HEADPHONES,
    IDM_REGION_LOOP = 3210, IDM_REGION_CLEAR, IDM_REGION_PLAYER_MENU,
    IDM_SHUFFLE_ALBUMS,
    IDM_THEME_BASE = 3220,
    IDM_THEME_ROBOTIC = 3239,  // the DELTARUNE theme's Chapter 2 sounds (app_deltarune.cpp), after the themes
    IDM_RATE_BASE = 3260,    // + 0..10 half stars
    IDM_LOOPS_BASE = 3280,   // + 0..3 passes through a game-music loop
    IDM_PANEL_BASE = 3285,   // + big mode panel (app_big.cpp)
    IDM_SIZE_BASE = 3290,    // + ViewSize
    IDM_COLUMN_BASE = 3300,  // + column type
    // Customize layout (app_custom.cpp): where the normal window's player docks and what it shows.
    IDM_LAYOUT_DOCK_BASE = 3400,    // + Dock
    IDM_LAYOUT_WAVE_BASE = 3405,    // + waveform size (slim, medium, tall)
    IDM_LAYOUT_WAVE_ROW = 3408, IDM_LAYOUT_PANELS_LEFT, IDM_LAYOUT_MENUS,
    IDM_LAYOUT_WAVE_SIZE = 3411,
    IDM_LAYOUT_PART_BASE = 3412,    // + part (kLayoutParts, app_custom.cpp)
    IDM_LAYOUT_PRESET_BASE = 3440,  // + preset
    // The waveform's look (app_fountain.cpp): its style, the fountain's toggles and colors.
    IDM_WAVE_STYLE_BASE = 3460,     // + waveform style (bars, fountain bars, Dark Fountain)
    IDM_WAVE_MOTION = 3464, IDM_WAVE_DR_COLORS, IDM_WAVE_GLOW, IDM_WAVE_BLACK,
    IDM_WAVE_COLOR_BASE = 3470,     // + Dock * 3 + (played, unplayed, theme colors)
    // Effects and smart volume (app_playback.cpp).
    IDM_REVERB_BASE = 3500,      // + ReverbPreset
    IDM_REVERB_MIX_BASE = 3505,  // + kReverbMixes index
    IDM_SPEED_BASE = 3510,       // + kSpeeds index
    IDM_TEMPO_BASE = 3520,       // + kTempos index
    IDM_LOUDNESS_COMP = 3530, IDM_NIGHT_MODE, IDM_RG_ESTIMATE,
    // Converting to .cord (app_convert.cpp).
    IDM_CONVERT = 3540, IDM_CONVERT_UNDO, IDM_CONVERT_CANCEL, IDM_CONVERT_RECYCLE, IDM_CONVERT_SHOW_ORIGINALS,
    IDM_CONVERT_MEMORY_BASE = 3550,  // + kConvertMemoryDays index
    // Copy to folder or device (app_copy.cpp): the selection or playlist, the playlist tab's, Cancel.
    IDM_COPY_TO = 3560, IDM_COPY_PL, IDM_COPY_CANCEL,
    IDM_EXPORT_BASE = 3570,  // + CordExport, excluding None
    // Packs (app_packs.cpp): View > Packs, one item per pack, and the pack themes in View > Theme.
    IDM_PACK_BASE = 3600,        // + pack index, up to kMaxPacks
    IDM_PACKS_FOLDER = 3680, IDM_PACKS_RELOAD, IDM_PACKS_REPORT, IDM_PACKS_NEW,
    IDM_PACK_THEME_BASE = 3700,  // + pack theme index, up to kMaxPackThemes
    IDM_PACK_AUTO_BASE = 3800,   // + a pack auto-playlist in Playlist > Add auto-playlist from a pack
    IDM_PACK_COLSET_BASE = 3870, // + a column set in the track list header's menu
    // Helpers (app_helpers.cpp): View > Helpers, one item per helper and per visualizer, and the
    // tools in Edit > Run and the track list's right-click menu.
    IDM_HELPER_BASE = 3900,       // + helper index, up to kMaxHelpers
    IDM_HELPERS_FOLDER = 3932, IDM_HELPERS_RELOAD, IDM_HELPERS_REPORT, IDM_HELPER_CANCEL,
    IDM_HELPER_VIS_BASE = 3940,   // + visualizer, up to kMaxHelperVisualizers
    IDM_HELPER_TOOL_BASE = 3960,  // + tool, up to kMaxHelperTools
};
constexpr int kMaxPacks = 64, kMaxPackThemes = 64, kMaxPackAutoPlaylists = 64, kMaxPackColumnSets = 30;
constexpr int kMaxHelpers = 32, kMaxHelperVisualizers = 16, kMaxHelperTools = 40;
// Column types run from the built-in columns on into the packs' (app_packs.cpp), each a command id.
constexpr int kMaxColumnTypes = 100;
static_assert(ui::kColumnCount < kMaxColumnTypes);

// Shuffle and repeat are independent: shuffle visits the playlist once, in random track or album
// order, then stops unless repeat is on.
enum class Repeat : uint8_t { Off, All, One, Count };

// The main window's sizes, smallest first; Ctrl+- and Ctrl+= step through them (app_sizes.cpp).
enum class ViewSize : uint8_t { Compact, Normal, Spotlight, Big, Count };
enum class PreferenceSaving : uint8_t { Ask, Automatic, Manual, Count };

// Where the normal window's player sits. At a side it becomes a column and its waveform runs down.
enum class Dock : uint8_t { Top, Bottom, Left, Right, Count };

// The normal window's arrangement (app_custom.cpp), kept in [Layout] of settings.ini. The other
// sizes keep their own fixed arrangements.
struct MainLayout {
    Dock player = Dock::Top;
    uint8_t waveSize = 0;     // 0 slim, 1 medium, 2 tall: the band's height, or the column's width
    bool waveOwnRow = false;  // at the top or bottom: the waveform on a line of its own under the buttons
    bool transport = true, order = true, wave = true, times = true, volume = true;
    bool nowPlaying = true, largeCover = false, points = false;
    bool tabs = true, search = true, views = true, upNext = false, panelsLeft = false;
    bool status = true, menuBar = true;
    // Extras that crowd the window, off unless asked for: the item count after each tab's name,
    // ReplayGain / EQ / output in the status bar, and how-to hints in the now playing line.
    bool tabCounts = false, statusDetails = false, hints = false;
    bool operator==(const MainLayout&) const = default;
};

// The track list's columns live in ui_spec.h, shared with the Linux port.
using ui::Column;
using ui::kColumns;
using ui::kColumnCount;
using ui::kColRating;
using ui::kColCover;
using ui::kColTrackNo;
using ui::kColTitleArtist;
using ui::kColApolloTitle;
using ui::kColApolloBitrate;
using ui::kColApolloRate;
using ui::kColApolloBits;
using ui::kColApolloSize;
using ui::kColApolloTag;
inline const std::vector<int> kDefaultColumns(std::begin(ui::kDefaultColumns), std::end(ui::kDefaultColumns));

// The Library tab's main view and its column browser.
enum class LibraryMode { Tracks, AlbumTracks, Albums, Artists, Count };

struct StartupPreferences {
    ViewSize size = ViewSize::Normal;
    bool mini = false;
    ThemeMode theme = ThemeMode::Light;
    LibraryMode library = LibraryMode::Albums;
    bool browser = true;
    MainLayout layout;
    bool operator==(const StartupPreferences&) const = default;
};
// Saved as indices in settings.ini; append fields so existing panes keep their meaning.
enum class BrowseField : uint8_t { Genre, AlbumArtist, Artist, Album, Composer, Year, Label, Grouping, Folder, Count };
constexpr int kViewButtons = (int)LibraryMode::Count + 1;  // the modes, then the browser toggle

struct BrowserPane {
    HWND list = nullptr;
    BrowseField field = BrowseField::Genre;
    std::vector<StringPool::Handle> values;  // rows 1.. (row 0 is "All")
    std::vector<uint32_t> counts;
    std::vector<StringPool::Handle> selected;  // empty = all
};

// Consecutive tracks of one album in a track list, shown under a two-row header (app_groups.cpp).
struct TrackGroup {
    uint32_t firstView, count;  // g.view positions of its tracks
    uint32_t firstRow;          // list row of its header; its tracks follow
    uint32_t pad = 0;           // empty rows after its tracks (the foobar2000 theme fits its cover)
    StringPool::Handle album, artist, date;  // artist: the album artist, or the one artist of all its tracks, or 0
    double seconds;
};

struct TokenInfo {
    TrackId id;
    int pl;
    uint64_t playlistIdentity = 0;
    uint32_t index = UINT32_MAX, following = UINT32_MAX, anchorIndex = UINT32_MAX;
    uint32_t radioNext = UINT32_MAX;  // stable radio choice while the engine decodes ahead
    TrackId anchor = UINT32_MAX;  // playlist position playback continues from (queued tracks: the
                                  // track before the queue); UINT32_MAX = the track itself
    bool queued = false;          // came from the queue and is still at its head
    bool estimated = false;       // the engine's quick loudness estimate arrived (estimate, LUFS)
    double estimate = 0;
    TrackId Position() const { return anchor != UINT32_MAX ? anchor : id; }
};

// ---------------------------------------------------------------------------
// State

// Worker hand-off records: the UI thread queues requests; workers post results back
// (WM_APP_IMPORT / WM_APP_ART / WM_APP_META / ...) so no UI state is touched off-thread.
struct ImportRequest { uint64_t playlistIdentity; std::vector<std::wstring> inputs; bool playFirst; };
struct ImportBatch {
    uint64_t playlistIdentity;
    std::vector<std::wstring> paths;
    std::vector<std::wstring> folders;  // the final batch: the request's inputs that were folders
    bool playFirst = false, final = false, cancelled = false;
    size_t added = 0, errors = 0;
};
struct ArtRequest { uint64_t generation = 0; std::wstring path; int size = 64; };
struct ArtResult { uint64_t generation; ArtImage image; };
struct ClosedPlaylist { Playlist playlist; int index; };


// The single application instance (`g`). Fields below are UI-thread owned except the
// worker-thread groups (art/import/tag/ReplayGain), each of which is guarded by its own
// mutex/atomic and reports back by posting to g.hwnd.
struct App {
    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr, list = nullptr, header = nullptr, tabs = nullptr, seek = nullptr, volume = nullptr,
         status = nullptr, search = nullptr, npTitle = nullptr, npSub = nullptr, time = nullptr,
         speaker = nullptr, tips = nullptr, rename = nullptr, eqWnd = nullptr, headphonesWnd = nullptr, foldersWnd = nullptr,
         elapsed = nullptr;  // left of the seek bar; `time` on its right shows the time left or the length
    bool timeRemaining = true;  // click the time right of the seek bar to switch
    bool liveWaveform = false;  // animate the audio being heard instead of the whole-song overview
    bool liveFountain = false;  // let the DELTARUNE fountain follow audible audio, including radio
    bool fullLiveWaveform = true;  // light the whole live display instead of splitting it at the song position
    int bandWaveSize = 0;       // normal top/bottom waveform height; 0 follows the layout preset
    bool resumeOn = true;       // remember the stopped / last-playing track's position across restarts
    bool explorerMenu = true;   // Explorer's folder verbs point at this build (app_platform.cpp)
    HWND buttons[6] = {};
    HWND fbButtons[2] = {};  // stop and pause: the foobar2000 theme's toolbar has its own (app_foobar.cpp)
    HACCEL accel = nullptr;
    UINT dpi = 96;
    HFONT font = nullptr, fontBold = nullptr, fontTitle = nullptr, fontSymbol = nullptr, fontArt = nullptr,
          fontIcons = nullptr,  // Segoe MDL2 Assets: the transport glyphs
          fontIconsBig = nullptr,  // the same, for the compact player's larger buttons
          fontIconsHuge = nullptr;  // and for big mode's
    RECT artRect{};

    ThemeMode themeMode = ThemeMode::Light;
    bool dark = false;
    const Palette* pal = &LightPalette();
    HBRUSH fieldBrush = nullptr, surfaceBrush = nullptr;

    std::vector<Playlist> playlists;
    int active = 0;
    uint64_t nextPlaylistIdentity = 1;
    std::deque<ClosedPlaylist> closedPlaylists;
    bool searchPending = false, closing = false, restoringView = false, compactPending = false;
    bool startupTheme = true;  // the first ApplyTheme runs before the window can paint
    std::unordered_map<uint64_t, std::wstring> cellCache;
    size_t cellCacheBytes = 0;
    std::vector<uint32_t> view;  // the visible item indices of the active playlist, in order
    std::vector<TrackGroup> groups;  // album headers among them; list rows = view + header and pad rows per group
    bool groupAlbums = true;
    std::vector<int> columns;    // list view subitem -> column type (kColumns index)
    std::wstring filter;
    int sortColumn = -1;  // column type
    bool sortAscending = true;

    Engine engine;
    unsigned long long nextToken = 1, decodingToken = 0, playingToken = 0, firstValidToken = 1;
    std::unordered_map<unsigned long long, TokenInfo> tokens;
    TrackId playing = kNone;
    int playingPl = -1;
    // Where playback stopped (Stop, or quitting while playing): the stopped strip offers to continue.
    TrackId resumeTrack = kNone;
    int resumePl = -1;
    double resumePos = 0;
    unsigned long long resumeToken = 0;  // the token Resume() started; it seeks once it plays
    int shuffle = kShuffleOff;
    bool skipAwareShuffle = false, radio = false;
    Repeat repeat = Repeat::Off;
    bool stopAfterCurrent = false, seeking = false, alwaysOnTop = false;
    bool exclusive = false;  // WASAPI exclusive output wanted
    bool soundEffects = false;
    bool taskbarProgress = true;
    bool showFocusBoxes = false;
    // Converting to .cord: verified originals go to the Recycle Bin, or are kept and, unless shown,
    // left out of the library; conversions stay undoable for kConvertMemoryDays[convertMemory].
    bool convertRecycle = true, convertShowOriginals = false;
    uint8_t convertMemory = 3;
    ViewSize size = ViewSize::Normal;
    ViewSize defaultSize = ViewSize::Normal;
    bool defaultMiniPlayer = false;
    PreferenceSaving preferenceSaving = PreferenceSaving::Ask;
    bool preferenceSavingChosen = false;
    ThemeMode defaultTheme = ThemeMode::Light;
    LibraryMode defaultLibraryMode = LibraryMode::Albums;
    bool defaultBrowserVisible = true;
    MainLayout defaultLayout;
    RECT stripRect{};  // the spotlight's and big mode's playback strip, when shown
    MainLayout layout;
    RECT gripRect{};               // the normal window's player handle: drag to dock, click for the layout menu
    std::vector<RECT> dividers;    // lines between the docked player and the rest, painted by the window
    bool stripHover = false;  // its buttons, times and volume are showing
    WINDOWPLACEMENT fullPlacement{};  // where the full-size window goes back to
    HWND pointsChip = nullptr;
    int loops = 2;  // game-music loop points: passes through the loop (0 = ignore loop points)  // rank, level and points in the now-playing strip (app_points.cpp)
    // The section of the playing track that loops (app_region.cpp): seconds into the track, and
    // whether playback wraps at region.end. Only one section per track is kept.
    double regionStart = 0, regionEnd = 0;
    bool regionOn = false;
    int volumePercent = 80, lastAudibleVolume = 80, consecutiveErrors = 0;
    std::vector<TrackId> history;
    std::vector<uint32_t> shuffleQueue;  // playlist positions; duplicate tracks remain distinct
    std::deque<TrackId> queue;  // "Play next" / "Add to queue": played before the playlist continues
    LibraryMode libMode = LibraryMode::Albums;
    bool browserVisible = true, browserUpdating = false, splitting = false;
    int browserHeight = 150;  // at 96 DPI
    BrowserPane panes[3];
    HWND viewButtons[kViewButtons] = {};
    HWND libView = nullptr;  // the Albums / Albums & Tracks / Artists canvas
    RECT splitRect{};        // the drag bar under the browser
    FolderScanner scanner;
    std::vector<std::wstring> libraryFolders;
    // Folders imported into the library that no library folder watches; Watch imported folders adds them.
    std::vector<std::wstring> importedFolders;
    bool inboxEnabled = false;
    int scanPending = 0;  // results still arriving from the first scan
    uint32_t viewRevision = 0;  // library revision the visible view was built from
    ULONGLONG viewBuilt = 0;
    TrackId statTrack = UINT32_MAX;  // play statistics: the track being listened to
    double listened = 0, lastPos = 0;
    int shufflePl = -1;
    bool shuffleStarted = false;  // shuffleQueue holds the rest of a pass (empty = the pass is over)
    unsigned long long shuffleToken = 0;  // a repeated NeedNext must not consume the new pass's last track
    std::mt19937 rng{std::random_device{}()};

    EqSettings eq;
    EqSettings globalEq;  // unassigned outputs use this curve; remembered device curves stay separate
    std::wstring eqOutputId;
    bool rememberOutputEq = false;
    HeadsetController headsets;
    HeadsetInfo headset;
    ReplayGainMode rgMode = ReplayGainMode::Album;
    bool rgOn = true;        // ReplayGain (see RgGainFor in app_workers.cpp)
    bool rgAutoScan = true;  // measure tracks without ReplayGain in the background
    float rgPreamp = 0, rgPreampNoInfo = 0;
    float rgHeadphoneHeadroom = 0;  // explicit per-output reserve for EQ performed inside a headset
    float rgKeep = 0.4f;  // the share of a loud album's cut that is skipped, so loud songs stay louder
    bool rgEstimate = true;  // unmeasured tracks get a quick estimate from their first seconds
    // Effects after the EQ. Speed and tempo last for the session; the rest are saved.
    ReverbSettings reverb;
    double speed = 1, tempo = 1;
    bool loudnessComp = false, nightMode = false;

    ArtImage art;
    TrackId artFor = kNone;
    uint64_t artGeneration = 0;
    std::thread artWorker;
    std::mutex artMx;
    std::condition_variable artCv;
    ArtRequest artRequest;
    bool artQuit = false, artReady = false, artOutstanding = false;

    std::thread importWorker;
    std::mutex importMx;
    std::condition_variable importCv;
    std::deque<ImportRequest> importQueue;
    std::atomic<bool> importCancel{false}, importRunning{false};
    std::atomic<uint64_t> importTarget{0};
    bool importQuit = false;
    size_t importOutstanding = 0;

    std::wstring fmtWindow, fmtStatus, fmtTitle, fmtSub;
    std::wstring message, statusParts[3];
    // The middle status part's clickable items: ReplayGain, equalizer and output (none while busy).
    std::vector<std::wstring> statusItems;
    RECT statusItemRects[3]{};
    ULONGLONG messageUntil = 0;
    bool statsDirty = true, dirty = false;
    int ticksSinceSave = 0;
    std::wstring dataDir, iniPath, libraryPath;

    std::thread tagWorker;
    std::mutex tagMx;
    std::condition_variable tagCv;
    std::deque<TrackId> tagQueue;  // compact IDs; paths are captured only for the next batch
    std::vector<TrackPath> tagBatch;
    bool tagBatchInFlight = false;
    bool tagQuit = false;
    std::unordered_set<TrackId> tagPending;
    std::unordered_set<TrackId> tagReading;  // the batch the tag worker has now (UI thread)
    std::vector<TrackId> tagAgain;  // coalesced rereads of changed files in that batch (at most 64)
    bool busyTimer = false;      // kBusyTimer is running
    std::vector<int> busyRows;   // list rows painted with a spinner on the last busy tick

    std::thread rgWorker;
    std::atomic<bool> rgCancel{false}, rgRunning{false};
    std::atomic<int> rgDone{0}, rgTotal{0};
    ULONGLONG rgStart = 0;      // GetTickCount64 when the scan started, for the time left
    std::atomic<std::shared_ptr<Decoder>> rgDecoder;
    std::atomic<TrackId> rgCurrent{kNone};  // the track the ReplayGain worker is measuring
    bool rgAuto = false;            // the running scan is the background measurer (not a manual scan)
    unsigned rgGen = 0;             // the running scan's generation; older runs' final messages are stale
    size_t rgOk = 0, rgFailed = 0;  // the running scan's results so far
    ULONGLONG rgNextAuto = 0;       // the background sweep's next look at the library (0 = not yet)
    uint32_t rgAlbumsRevision = 0;  // library revision whose album gains were last worked out
    std::unordered_set<TrackId> rgRunIds;     // the running scan's tracks
};
extern App g;

struct TagResult {
    TrackId id;
    TagData tags;
    bool success = false;
    std::wstring path;  // reject a stale read if a track was moved while the batch ran
    uint64_t fileSize = 0, fileTime = 0;
    bool haveStats = false;
};

// A ReplayGain worker's message: some tracks' results; the run's last one is marked final.
struct RgBatch {
    unsigned generation;
    bool background, final;
    std::vector<RgResult> results;
};

// Scales a 96-DPI ("dip") measurement to the window's current DPI; every layout
// constant in the UI code is expressed at 96 DPI and run through this.
inline int S(int px) { return MulDiv(px, (int)g.dpi, 96); }
// The active playlist, and the active theme palette. Both are always valid once the
// window exists; the UI thread owns both.
inline Playlist& Active() { return g.playlists[g.active]; }
inline const Palette& P() { return *g.pal; }

// Colors for the current theme. Everything comes from the active Palette (theme_palette.cpp),
// in light and dark mode alike, so no control can drift out of the active theme.
inline COLORREF WindowBg() { return Ref(P().bg); }
inline COLORREF TextColor() { return Ref(P().text); }
inline COLORREF SubColor() { return Ref(P().sub); }
inline COLORREF SurfaceColor() { return Ref(P().surface); }

struct Region {
    double start = 0, end = 0;
    bool on = false;
};

enum : int { kPanelLibrary, kPanelNowPlaying, kPanelUpNext, kPanelCount };
extern HWND g_hotButton;

}  // namespace app
