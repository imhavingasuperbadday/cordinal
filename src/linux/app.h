// The native Linux front end: a window on Wayland or X11 through cordinal's own platform layer
// (src/linux/platform/, which keeps SDL3's API so these call sites read as before), everything
// drawn by cordinal itself (ui.cpp), on top of the same core as the Windows build (library, tags,
// decoders, engine, scanner).
//
// Threads talk to the UI thread the way they do on Windows: PostMessageW (compat/windows.h)
// queues the message and wakes the platform layer's event loop, which hands it to OnMessage.
#pragma once
#include "autoplaylist.h"
#include "convert.h"
#include "devicecopy.h"
#include "engine.h"
#include "keymap.h"
#include "library.h"
#include "organize.h"
#include "playlist_shuffle.h"
#include "packs.h"
#include "points.h"
#include "replaygain.h"
#include "scanner.h"
#include "sfx.h"
#include "theme_palette.h"
#include "ui_spec.h"
#include "web_server.h"

#include "platform/sdl_api.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace lx {

constexpr TrackId kNone = UINT32_MAX;
constexpr UINT WM_APP_SCAN = WM_APP + 10;    // FolderScanner results (lParam = ScanResult*)
constexpr UINT WM_APP_TAGS = WM_APP + 11;    // a batch of tags read (lParam = std::vector<TagResult>*)
constexpr UINT WM_APP_COVER = WM_APP + 12;   // a cover decoded (lParam = CoverImage*)
constexpr UINT WM_APP_REMOTE = WM_APP + 13;  // a command from MPRIS or the tray (wParam = Remote)
constexpr UINT WM_APP_WEB = WM_APP + 14;
constexpr UINT WM_APP_THUMB = WM_APP + 15;   // an album header's cover decoded (lParam = CoverImage*)
constexpr UINT WM_APP_PEAKS = WM_APP + 16;   // a waveform computed (wParam = request id, peaks.h)
constexpr UINT WM_APP_RG = WM_APP + 17;      // ReplayGain progress (wParam 1) or results (wParam 2, lParam = RgBatch*)
constexpr UINT WM_APP_TAGWRITE = WM_APP + 18;  // tag writing progress (wParam 1) or done (wParam 0, lParam = WriteDone*)
constexpr UINT WM_APP_PLAYLIST_FILE = WM_APP + 20;
constexpr UINT WM_APP_CHAPTERS = WM_APP + 21;  // chapters read on the tag worker (lParam = ChapterResult*)
constexpr UINT WM_APP_CONVERT = WM_APP + 22;  // a file converted (wParam 0, lParam = index) or the run finished (wParam 1)
constexpr UINT WM_APP_EXPAND = WM_APP + 23;   // folders expanded on the worker; the results wait in expand.cpp
constexpr UINT WM_APP_COPY = WM_APP + 25;      // a file copying (wParam 0, lParam = index) or the run finished (wParam 1)
constexpr UINT WM_APP_ORGANIZE = WM_APP + 24;  // an organize run finished; the result waits in organize_run.cpp
constexpr UINT WM_APP_LISTENBRAINZ = WM_APP + 33;  // see listenbrainz.h
constexpr UINT WM_APP_LYRICS = WM_APP + 34;  // a song's lyrics read (lParam = lyrics.cpp's Loaded*)
constexpr UINT WM_APP_HELPER = WM_APP + 35;  // a helper tool's result (wParam 1: lParam owns it, helpers_ui.cpp)
constexpr UINT WM_APP_ARTISTINFO = WM_APP + 39;  // an artist looked up (lParam owns it, artistinfo_ui.cpp)
constexpr UINT WM_APP_COVERFETCH = WM_APP + 38;  // cover search results, progress and saves (coverfetch_ui.cpp)
constexpr UINT WM_APP_IPOD = WM_APP + 37;    // classic iPod progress (0) or sync result (1); lParam owns it
constexpr UINT WM_APP_GAME = WM_APP + 36;  // a game's clip or cover decoded (lParam owns it, games.cpp)
constexpr UINT WM_APP_FILE = WM_APP + 31;    // a file chosen for a dialog (wParam = what for, lParam = std::wstring*)
constexpr UINT WM_APP_COLLECTION = WM_APP + 40;  // collection export/import progress, results and choices (collection_run.cpp)
constexpr UINT WM_APP_ALBUMTAG = WM_APP + 42;  // a MusicBrainz album search finished (lParam owns it, albumtag_ui.cpp)
constexpr UINT WM_APP_RIP = WM_APP + 44;  // Rip CD: a disc read (wParam 2), progress (0) or the finished rip (1), panel_rip.cpp
constexpr UINT WM_APP_DUPLICATES = WM_APP + 43;  // a duplicate clean-up or its undo moved its files (duplicates_ui.cpp)
constexpr UINT WM_APP_SPEAKERS = WM_APP + 41;  // network speakers a search found (lParam owns them, panel_setup.cpp)
constexpr UINT WM_APP_WEBFETCH_ADDED = WM_APP + 45;    // a track joined the saving list (webfetch_ui.cpp)
constexpr UINT WM_APP_WEBFETCH_UPDATED = WM_APP + 46;  // its state or percent changed (wParam = index)
constexpr UINT WM_APP_WEBFETCH_NOTICE = WM_APP + 47;   // a line of progress for the status line
constexpr UINT WM_APP_WEBFETCH_DONE = WM_APP + 48;     // the run finished, carrying its summary

// How long a conversion can be undone, in days; the Windows build's kConvertMemoryDays.
constexpr unsigned kConvertMemoryDays[] = {1, 3, 7, 49};

// Commands from MPRIS, the tray icon and other cordinal processes (lParam carries a value: microseconds
// for Seek/SetPosition, 0..1000000 for SetVolume, 0/1 for SetShuffle, a Repeat for SetRepeat,
// a std::vector<std::wstring>* for Open).
enum class Remote : WPARAM {
    PlayPause = 1, Play, Pause, Stop, Next, Previous, Raise, Quit, Seek, SetPosition, SetVolume, SetShuffle, SetRepeat, Open,
    ToggleStartOnLogin, ToggleLightweight, PickSong
};
enum class Repeat : int { Off, All, One };
// The Library tab's views and the column browser's fields, as the Windows build's (app_state.h).
enum class LibraryMode : uint8_t { Tracks, AlbumTracks, Albums, Artists, Count };
enum class BrowseField : uint8_t { Genre, AlbumArtist, Artist, Album, Composer, Year, Label, Grouping, Folder, Count };

// The themes are the Windows build's (theme_palette.h), and so are the track list's columns: a
// column is its index in ui::kColumns (ui_spec.h), as in library.cdb's sort column and on Windows.
using ::ThemeMode;
using Palette = ::Palette;
const Palette& CurrentPalette();

struct TagResult {
    TrackId id;
    std::wstring path;
    TagData tags;
    bool ok;
    bool haveStats = false;
    uint64_t fileSize = 0, fileTime = 0;
};
struct ChapterResult {
    TrackId id;
    uint64_t generation;
    std::wstring path;
    std::vector<Chapter> chapters;
    bool haveStats = false;
    uint64_t fileSize = 0, fileTime = 0;
};

struct PlaybackSection {
    double start = 0, end = 0;
    bool on = false;
};

enum class PreferenceSaving : uint8_t { Ask, Automatic, Manual, Count };

// The parts of the main window that can be hidden (Settings::hiddenParts has a bit set for each hidden one).
enum LayoutPartBit : unsigned {
    kPartTransport = 1, kPartOrder = 2, kPartWave = 4, kPartTimes = 8, kPartVolume = 16, kPartNowPlaying = 32,
    kPartTabs = 64, kPartSearch = 128, kPartViews = 256, kPartStatus = 512,
};

enum class ViewSize : uint8_t { Compact, Normal, Spotlight, Big, Count };

// Session presentation; startup defaults are saved only by the explicit preference review.
struct WindowLayout {
    unsigned hiddenParts = 0;
    int dock = 0, waveSize = 0;  // top, bottom, left, right; slim, medium, tall
    bool points = false, waveOwnRow = false, largeCover = false, upNext = false, panelsLeft = false;
    bool menuBar = true, tabCounts = false, statusDetails = true, hints = false;
    bool operator==(const WindowLayout&) const = default;
};

struct Settings {
    uint16_t webPort = 8787;
    bool webStart = false;
    // Listening away from home (web.cpp): on, its consent given once, and the HTTPS port outside.
    bool webAway = false, webAwayAgreed = false;
    bool webShare = false;     // network players may browse the library (web::Share)
    std::string webShareId;    // its lasting "uuid:..." id, made the first time it is turned on
    uint16_t webAwayPort = 443;
    // A fixed pairing code (empty: a new code for each pairing) and the names a reverse proxy on this computer
    // answers to (Tailscale Serve, say), separated by commas or spaces: the Windows build's Code= and Hosts=.
    std::string webCode, webHosts;
    std::string outputDevice;    // the PipeWire sink to play on (node.name); empty is the system default
    std::string helpersOn;       // ids of the helpers that are on, joined by |
    std::map<std::string, std::string> helperTrust;  // id -> the record of the files it was turned on with
    std::string packThemeKey;    // "pack/theme" of the default theme when a pack provides it (resolved once the packs load)
    std::string packsOn;         // ids of the packs that are on, joined by |
    WindowLayout layout, defaultLayout;
    std::string savedLayouts[ui::kSavedLayouts];  // Layout > Save this layout, in LayoutText's words on one line
    ViewSize size = ViewSize::Normal, defaultSize = ViewSize::Normal;
    bool lyricsPanel = false;    // the lyrics panel beside the track list (lyrics.cpp)
    bool timeRemaining = true;   // the time beside the seek bar counts down instead of showing the length
    bool waveform = true;       // off uses a regular seek bar, overriding the saved live display settings
    bool liveWaveform = false;   // animate the audio being heard instead of the whole-song overview
    bool liveFountain = false;   // the DELTARUNE fountain follows audible audio, including radio
    bool listWind = false;       // wind ribbons behind the track list follow the heard audio (wind.h)
    bool fullLiveWaveform = true; // light the whole live display instead of splitting it at the song position
    bool wave60Fps = false;      // animate the live seek bar 60 times a second instead of 30
    int bandWaveSize = 0;        // normal top waveform height; 0 follows the layout size
    int seekStyle = 0;           // the seek bar's look, whole song or live: SeekStyle (peaks.h)
    int moodColors = 0;          // the moodbar's colours: MoodColors (peaks.h)
    int moodWaveColors = 0;      // the mood waveform's: 0 the moodbar's, else MoodColors + 1
    int waveColor = 0;           // the plain waveform's colour and the moodbar's base: WaveColors (peaks.h)
    int waveRestColor = 0;       // the waveform's unplayed part: WaveRestColors (peaks.h)
    bool smoothSeek = false;     // soften the whole song's outline and blend its moodbar colours
    bool smoothScroll = false;   // the library canvas eases toward wheel and paging moves (off by default)
    bool waveReflection = true;  // mirror the waveform below its center line, for the waveform and moodbar styles
    bool waveFullHeight = true;  // unmirrored, the waveforms still fill both halves, the lower as bright
    bool pauseFade = true;       // the waveform fades to its paused colour and back instead of switching at once
    int coverFade = 0;           // the cover's change to the next one: coverfade::Style, off by default (cover_fade.h)
    bool seekGlow = true;        // the seek bar's colours glow softly past its edges, every style and source alike
    bool alwaysOnTop = false;
    bool gpuDrawing = false;     // draw through the graphics card; off keeps its driver out
    // Lightweight mode (lightweight.cpp), as the Windows build's [Lightweight] keys: what stays on screen
    // (LightweightShows), the numpad (LightweightNumpad), pausing library work and giving memory back.
    int lightweightShows = 0, lightweightNumpad = 1;
    bool lightweightPause = true, lightweightTrim = true;
    // Your own keys (keys.cpp, keymap.h): the changes to cordinal's, as settings.ini's key_window.* and
    // key_global.* lines (the Windows build's [Keys] and [Global keys]).
    std::vector<keymap::Binding> keyChanges;
    // The games (games.cpp), as the Windows build's [Games] keys: Name That Track's rounds solved at each clip
    // length and then missed, and both games' current and best streaks.
    std::vector<int> nttGuesses;
    int nttStreak = 0, nttBest = 0, wcfStreak = 0, wcfBest = 0;
    // Title formatting (panel_setup.cpp): the now-playing lines, the status bar and the window title.
    std::wstring fmtTitle = L"%title%";
    std::wstring fmtSub = L"[%artist%][  \x2022  %album%][  \x2022  %date%]";
    std::wstring fmtStatus = L"[%codec% | ][%bitrate% kbps | ][%samplerate% Hz | ][%channels% | ]%playback_time% / %length%";
    std::wstring fmtWindow = L"[%artist% - ]%title% - cordinal";
    bool soundEffects = false;   // small sounds for actions that otherwise happen silently (sounds.cpp)
    bool roboticSounds = true;   // the DELTARUNE theme's sounds crushed in Chapter 2's Cyber World voice
    std::vector<std::wstring> folders;
    bool foldersChosen = false;  // a folder list was chosen, so it is never treated as a default again
    bool musicDefaultChecked = false;  // the one-time cleanup of the auto-added system Music folder ran
    bool inboxEnabled = false;
    bool steamOffered = false;
    float volume = 1;
    // Playback rates (Playback > Speed and Tempo), remembered as the menu's own values like the
    // Windows build's [Player] Speed and Tempo.
    double speed = 1, tempo = 1;
    int shuffle = kShuffleOff;
    bool skipAwareShuffle = false;
    bool radio = false;
    bool taste = false;
    bool resumeOn = true;
    bool resumeInSong = false;  // reopen with the last track loaded, paused where it stopped
    bool resumePlay = false;    // start a reopened track at once instead of waiting paused
    std::wstring resumePath;
    double resumePosition = 0;
    int resumeIndex = -1;
    std::vector<std::pair<std::wstring, double>> spots;           // songs left partway: path, place
    std::vector<std::pair<std::wstring, uint32_t>> snoozed;       // rediscoveries x'd out: path, last play
    std::map<std::wstring, PlaybackSection> sections;
    // Startup defaults (preferences_ui.cpp): theme, libraryMode and browserVisible are this session's choices;
    // the approved ones below are what settings.ini keeps and what cordinal opens with.
    ThemeMode defaultTheme = ThemeMode::System;
    LibraryMode defaultLibraryMode = LibraryMode::Albums;
    bool defaultBrowserVisible = true;
    PreferenceSaving preferenceSaving = PreferenceSaving::Ask;
    bool preferenceSavingChosen = false;  // the saving policy was answered (so it is not asked again)
    uint64_t suggestionLastReview = 0;    // Unix seconds of the last review of startup defaults
    bool exclusive = false;
    bool upmix = false;  // spread stereo over a surround sink's speakers (shared output)
    Repeat repeat = Repeat::Off;
    ThemeMode theme = ThemeMode::System;
    bool groupAlbums = true;  // album headers over each album's tracks, as View > Group tracks by album
    bool npAlbumOnly = true;    // Now Playing lists only the playing song's album, as on Windows
    bool groupAutoTabs = true;  // the auto-playlists share one tab (TabSlots), as on Windows
    bool musicBrainzLookup = false;  // optional online album-edition lookup, cached and rate-limited
    bool steamSoundEffects = false;  // Add Steam soundtracks also takes games' sound-effect folders
    // Replacements made by Library > Convert...: originals go to the trash or stay (shown in the library
    // or not), and a conversion can be undone for kConvertMemoryDays[convertMemory].
    bool convertRecycle = true, convertShowOriginals = false;
    uint8_t convertMemory = 3;
    // The Convert panel's last choices: the CordExport format, replacements or copies, and the copies'
    // folder (used when convertIntoFolder).
    uint8_t convertFormat = 1;
    bool convertReplace = false, convertIntoFolder = false;
    std::wstring convertFolder;
    // Library > Rip CD...: the format last chosen (FLAC, else .cord), the folder albums go in, and the
    // CD drive's read offset in samples (Preferences > Conversion).
    bool ripFlac = true;
    std::wstring ripFolder;
    int ripOffset = 0;
    // Library > Organize files: the folder last organized into and the naming pattern (empty: the
    // library's first folder and kDefaultOrganizePattern).
    std::wstring organizeFolder, organizePattern;
    // Library > Copy to folder or device: the last destination, naming pattern and CopyFormat.
    std::wstring copyFolder, copyPattern;
    uint8_t copyFormat = 0;
    // The Library's view and its column browser (fields, shown, height at scale 1), and the album
    // covers' size in the views (Ctrl+wheel), with the Windows build's defaults.
    LibraryMode libraryMode = LibraryMode::Albums;
    bool browserVisible = true;
    uint8_t browserFields[3] = {(uint8_t)BrowseField::Genre, (uint8_t)BrowseField::AlbumArtist, (uint8_t)BrowseField::Album};
    int browserHeight = 150, coverSize = 160;
    // Their startup defaults (preferences_ui.cpp): what the settings file keeps when they change.
    int defaultBrowserHeight = 150, defaultCoverSize = 160;
    std::vector<int> columns{std::begin(ui::kDefaultColumns), std::end(ui::kDefaultColumns)};
    std::string savedPackColumns;  // named pack columns read before the enabled packs load
    std::vector<int> widths = std::vector<int>(ui::kColumnCount, 0);  // per column, at scale 1 (0 = default)
    // The Windows window's default size: 1120 x 720 with its frame, so this client area.
    int windowW = ui::kLayoutWidth, windowH = ui::kMenuBarHeight + 662;
    bool maximized = false;
    int activeTab = 0;
    // How playback sounds: ReplayGain (the Windows build's Preferences keys) and the equalizer.
    ReplayGainOptions rg;
    bool rgAutoScan = true;  // measure tracks without ReplayGain in the background
    bool rgEstimate = true;  // unmeasured tracks get a quick estimate from their first seconds
    EqSettings eq;
    // Effects after the EQ, and game-music loop points (the engine's, as on Windows).
    ReverbSettings reverb;
    BitcrushSettings bitcrush;
    bool loudnessComp = false, nightMode = false;
    int loops = 2;  // passes of a loop before the fade; 0 ignores loop points
    // As on Windows: the slider's curve (VolumeCurveGain), whether cordinal scales the samples at all,
    // the crossfade between tracks that follow on their own (kCrossfadeSeconds index), and whether an
    // album's own tracks crossfade too. Cover spacing is the album grid's least gap, at scale 1.
    int volumeCurve = 0, crossfade = 0, coverGap = 16;
    bool softwareVolume = true, crossfadeAlbums = false;
};

// A ReplayGain worker's message: some tracks' results; the run's last one is marked final.
struct RgBatch {
    unsigned generation;
    bool background, final;
    std::vector<RgResult> results;
};

struct App {
    // Paths ($XDG_DATA_HOME/cordinal and $XDG_CONFIG_HOME/cordinal).
    std::wstring dataDir, configDir, libraryPath, settingsPath;
    Settings settings;

    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    float scale = 1;  // display scale (HiDPI)
    bool redraw = true, quit = false;
    bool lightweight = false;  // Lightweight mode is on (lightweight.cpp)

    std::vector<Playlist> playlists;
    int active = 0;
    std::deque<TrackId> queue;  // Play next / Add to queue: played before Now Playing continues (playback.cpp)
    // Choose by taste's picks are the queue's last `queuePicks` entries, after what you queued;
    // pickIndex holds each one's Now Playing position, and picksMode the order that drew them
    // (1 track shuffle, 2 Keep playing like this); pickFrom is the position the next shuffle pick
    // follows (-1: the playing one), so a removed pick doesn't come straight back.
    size_t queuePicks = 0;
    std::deque<int> pickIndex;
    int picksMode = 0, pickFrom = -1;
    std::vector<TrackId> rows;  // the active tab after the search filter and sort
    std::vector<size_t> rowItems; // each row's occurrence in its source playlist
    std::vector<char> selected; // per row
    int focus = -1, anchor = -1;
    float scroll = 0;           // list scroll offset in pixels
    std::wstring search;
    bool searchFocused = false;
    uint32_t rowsRevision = 0;  // g_lib.Revision() the rows were built for
    TrackId chapterTrack = kNone;
    uint64_t chapterGeneration = 0;
    std::vector<Chapter> chapters;

    Engine engine;
    FolderScanner scanner;
    bool scanning = false;
    bool libraryDirty = false;
    uint64_t lastSave = 0;

    // Playback.
    TrackId playing = kNone;        // audible track
    TrackId resumeTrack = kNone;    // stopped track offered for continuation
    // Songs left partway, newest first, with where each was left (panel_comeback.cpp). A song drops
    // out once it plays to its end.
    std::vector<std::pair<TrackId, double>> spots;
    int resumeIndex = -1;
    double resumePosition = 0;
    unsigned long long token = 0;   // engine token of the audible track
    int queuePos = -1;              // index of `playing` in `queue`
    bool stopAfterCurrent = false;
    std::vector<uint32_t> shuffleOrder;  // a permutation of queue indexes while shuffling
    int shufflePos = -1;
    std::mt19937 rng{std::random_device{}()};
    double listened = 0;      // seconds of the current track heard (for play counts)
    double lastPosition = 0;  // for detecting seeks while counting
    std::wstring message;     // the status bar's message
    uint64_t messageUntil = 0;

    // Tag reading.
    std::mutex tagMx;
    std::condition_variable tagCv;
    std::deque<TrackId> tagQueue;
    std::vector<TrackPath> tagBatch;
    bool tagBusy = false, tagQuit = false;
    std::thread tagThread;

    // ReplayGain measuring (replaygain.cpp). The worker reads only rgCancel and rgDecoder.
    std::thread rgWorker;
    std::atomic<bool> rgCancel{false};
    std::atomic<std::shared_ptr<Decoder>> rgDecoder;
    std::atomic<int> rgDone{0};
    int rgTotal = 0;
    bool rgRunning = false, rgAuto = false;  // rgAuto: the running scan is the background sweep
    unsigned rgGen = 0;
    size_t rgOk = 0, rgFailed = 0;
    std::vector<TrackId> rgRunIds;  // the running scan's tracks
    uint64_t rgNextAuto = 0;        // SDL ticks of the sweep's next look at the library
    uint32_t rgAlbumsRevision = 0;  // library revision whose album gains were last worked out
};
extern App g;

// settings.cpp
void InitPaths();
void LoadSettings();
void SaveSettings();
void ChooseMigration(int kind);  // 1 backup, 2 restore, 3 CSV statistics
void OnMigration(WPARAM kind, LPARAM value);
void StopMigration();
void FinishMigration();
void LoadLibraryFile();
void SaveLibraryFile(bool force);
// The system Music folder (XDG's, or $HOME/Music): the default library folder on a new profile.
std::string MusicFolder();

// library_sync.cpp
void StartScanner();
void OnScanResult(ScanResult* r, bool firstScanDone);
void UpdateInboxTab();
void StartTagWorker();
void StopTagWorker();
void QueueTags(const std::vector<TrackId>& ids);
void AcceptTags(std::vector<TagResult>* results);
void QueueChapters(TrackId id);
void AcceptChapters(ChapterResult* result);
void AddFolder(const std::wstring& folder);
void RemoveFolder(size_t index);

// playback.cpp
void PlayRow(int row);
void PlayFromQueue(int index, double start = 0, bool paused = false);
void TogglePause();
void Stop();
void Next();
void Previous();
void SeekTo(double seconds);
void SetVolume(float volume);
void ToggleShuffle();
void SetShuffle(int mode);
void SetRepeat(Repeat repeat);
void SetSkipAwareShuffle(bool on);
void SetRadio(bool on);
void SetTaste(bool on);
void SetResumeOn(bool on);
bool Resume();
// Remembered places in songs left partway (panel_comeback.cpp, as the Windows build's app_spots.cpp).
void RememberSpot(TrackId id, double pos);
void ForgetSpot(TrackId id);
void PlaySpot(size_t i);
bool HasComebacks();  // anything OpenComebacks would list besides the song now loaded
// Opens with the last session's track, paused where it stopped or playing at once, as Preferences asks.
void ReopenLastTrack();
PlaybackSection SectionOf(TrackId id);
void SetSectionFor(TrackId id, const PlaybackSection& section);
void RemapNowPlaying(const std::vector<size_t>& oldToNew);
void CycleRepeat();
void SetExclusive(bool exclusive);
void SetSpeed(double speed);
void SetTempo(double tempo);
void ToggleStopAfterCurrent();
void ApplyEffects();  // reverb, bitcrush, loudness compensation, night mode and loop points to the engine
void OnEngineEvent(EngineEvent e, unsigned long long token);
void PlaybackTick();
void RecordListening(TrackId id, double seconds, bool manual, uint32_t when = 0);
bool IsPaused();
std::wstring TrackTitle(TrackId id);
std::wstring TrackArtist(TrackId id);
void RefreshGains(bool includePlaying = true);  // ReplayGain or the EQ changed: reapply every queued track's gain
void ApplyEq();                                 // g.settings.eq to the engine, and the gains that leave room for it
float GainFor(TrackId id);
float GainForEstimate(TrackId id, double lufs);
std::vector<TrackId> UpcomingTracks();          // the next few tracks to play, for measuring ahead

// replaygain.cpp: measuring, in the background (the next tracks first) or as asked.
void ScanReplayGain(const std::vector<TrackId>& ids);
void MaybeAutoMeasure(bool priority);
void CancelReplayGain();
void StopReplayGain();
void ClearReplayGain(const std::vector<TrackId>& ids);
void WriteReplayGainTags(const std::vector<TrackId>& ids);
void SetReplayGain(bool on);
void FinishReplayGain(WPARAM wp, LPARAM lp);
std::wstring ReplayGainStatus();  // the status bar's work-in-progress text, or ""

// tagwrite.cpp: writing tags on a worker, one undo journal per edit; files playing wait their turn.
bool SubmitTagWrites(const std::vector<std::pair<TrackId, TagEdit>>& items, bool undoable,
                     std::wstring journalAfter = {});
// Ratings and play counts into the files' tags (migration::StatisticsTags), journaled for undo.
void WriteStatisticsTags(const std::vector<TrackId>& ids);
void UndoLastTagEdit();
bool CanUndoTagEdit();
// Unix time of the newest tag edit that can be undone, 0 without one.
uint64_t NewestTagEditTime();
void OnTagWriteMessage(WPARAM wp, LPARAM lp);
void FlushDeferredTags();
void StopTagWriter();
bool TagWriteBusy();
std::wstring TagWriteStatus();

// expand.cpp: folders and files handed to cordinal (the command line, a file manager, drag and drop)
// are walked on one worker, in order; the UI thread gets only the audio files to add.
enum class Opening : uint8_t { Play, Drop };
void ExpandPaths(std::vector<std::wstring> paths, Opening how);
void OnExpanded();
void StopExpanders();
void HoldPendingOpens();  // a user's Stop: opens still being walked add their files without playing
void OpenFiles(const std::vector<std::wstring>& files, bool play = true);  // main.cpp: into Now Playing, the first plays
void AddDropped(const std::vector<std::wstring>& files);  // ui.cpp: into the active playlist

// convert_run.cpp: Library > Convert...'s run on a worker, and undoing its replacements. The panel is
// panel_convert.cpp's (OpenConvert).
bool ConvertBusy();
bool CanUndoConversion();
int EnsureNowPlayingTab();  // playback.cpp: the Now Playing tab's index, made again if it was closed
void InitConvert();
void OpenConvert(std::vector<TrackId> ids);  // the panel, for the tracks given or the whole library when there are none
// The tracks as Convert would treat them (the library's codec tells ALAC from AAC; the song playing is
// left alone), and the run itself: false when nothing would convert or a replacement wasn't confirmed.
std::vector<ConvertItem> PlanConvertTracks(const std::vector<TrackId>& ids, CordExport format, bool replace, const std::wstring& folder);
bool StartConvert(std::vector<ConvertItem> plan, CordExport format, bool replace);
void OnConvertMessage(WPARAM wp, LPARAM lp);
void UndoConversion();
void CancelConversion();
void SetConvertShowOriginals(bool on);
void SetConvertMemory(unsigned index);
void StopConverter();
// panel_rip.cpp: Library > Rip CD..., its panel and its run on a worker.
void OpenRipCd();
bool RipBusy();
void CancelRip();
void OnRipMessage(WPARAM wp, LPARAM lp);
void StopRipper();
// A question with OK and Cancel over the window; true for OK.
bool Confirm(const std::string& question, const char* title);

// organize_run.cpp: Library > Organize files moves files on a worker; Undo last organize reverses it.
bool OrganizeBusy();
std::wstring OrganizeLogDir();
void StartOrganize(const std::vector<OrganizeMove>& plan, const std::wstring& root);
void OnOrganizeDone();
void UndoOrganize();
void StopOrganizer();

// copy_run.cpp: Library > Copy to folder or device runs the Copy panel's plan on a worker.
bool CopyBusy();
std::wstring CopySize(uint64_t bytes);  // "120 MB", "1.4 GB"
void StartCopy(std::vector<CopyItem> plan, CopySettings settings, std::vector<CopyRecord> manifest);
void OnCopyMessage(WPARAM wp, LPARAM lp);
void CancelCopy();
void StopCopier();

// collection_run.cpp: File > Export collection and Import collection, on a worker. Busy covers
// the choices asked between steps; Running only the worker, which Cancel stops.
void ExportCollection();
void ImportCollection();
void CancelCollection();
bool CollectionBusy();
bool CollectionRunning();
std::string CollectionCancelText();
void OnCollectionMessage(WPARAM wp, LPARAM lp);
void StopCollection();

// ipod_run.cpp: Library > Sync to iPod writes a classic iPod's iTunesDB on a worker.
bool IpodBusy();
void StartIpodSync();
void CancelIpodSync();
void OnIpodMessage(WPARAM wp, LPARAM lp);
void StopIpod();

// views (ui.cpp)
void RebuildRows();
void InvalidateCellCache();  // album labels or ids changed without a track revision
// A small menu of choices below (x, y) for a panel; `pick` gets the index chosen.
void OpenChoice(float x, float y, const std::vector<std::string>& labels, int selected, std::function<void(int)> pick);
void SetActiveTab(int tab);
void ShowMessage(const std::wstring& text);
void OnRemote(Remote r, LPARAM value);

// mpris.cpp: MPRIS (media keys, playerctl, bar widgets), the tray icon, and one cordinal at a time.
// Hands `args` (files, or a transport command like "--next") to a cordinal already running and
// returns true if there is one.
bool ForwardToRunning(const std::vector<std::string>& args);
void StartMpris();
void StopMpris();
void MprisChanged(bool seeked = false);  // what's playing, the play state or a setting changed
void MprisSetArt(TrackId id, const std::string& path);
bool StartsOnLogin();
void ToggleStartOnLogin();
void ToggleTopBar();  // cordinal's icon on Waybar's bar (waybar.cpp), with a message when there is no Waybar
// ListenBrainz (scrobble.cpp): the submission worker, "playing now" and a listen for each counted play.
void InitScrobble();
void StopScrobble();
void DiscordTick();  // scrobble.cpp: keeps the Discord status (discord.h) with what plays
void StopDiscord();  // before StopCoverFetch: its cover lookups use the online cache
void ScrobbleTrackStarted(unsigned long long token, TrackId id);
void ScrobbleCounted(TrackId id, uint32_t when, double seconds, bool local);
// A title-format pattern (titleformat.h) filled in from `id`, with the time fields of playback if asked.
std::wstring FormatForTrack(const std::wstring& pattern, TrackId id, bool withPlayback);
void UpdateWindowTitle();

// cordinal points in the window (points_ui.cpp).
struct PointsLevelInfo {
    uint64_t total;       // whole points
    int level;
    uint64_t start, next;  // whole points where this level starts and the next begins
};
PointsLevelInfo CurrentPointsLevel();
std::wstring GroupedNumber(uint64_t v);  // 1,234,567
void AnnouncePoints(const PointsAward& award);  // the chip's "+N", a level-up's message and toast
bool PointsChipAnimating();
void StepPointsChip();
void ShowToast(std::wstring title, std::wstring body);
bool ToastMouseDown(float x, float y);  // a click on the toast opens the Points window

// Startup defaults and the question of saving them (preferences_ui.cpp).
void ResetPreferenceSuggestions();  // after the settings are loaded: what is on screen is the baseline
void PreferenceTick(uint64_t lastInput);
void FlushAutomaticPreferences();
void SetPreferenceSaving(PreferenceSaving mode);
void ShowStartupDefaults(bool automatic);
void OpenStartupDefaults();
uint64_t LastInputTicks();  // when the last key or mouse event arrived

// The mini player (mini.cpp): the main window made small and round.
bool MiniActive();
void ToggleMini();
void LeaveMini();
bool MiniEvent(const SDL_Event& e);
// The window's smallest size for its view size; leaving the mini player sets it again.
void ApplyWindowMinimum();

// Lightweight mode (lightweight.cpp, View > Lightweight mode, Ctrl+Shift+L), as the Windows build's
// app_lightweight.cpp: playing in the background with the window put away.
enum class LightweightShows : uint8_t { Nothing, Mini, Compact };
enum class LightweightNumpad : uint8_t { Off, NumLockOff, Always };
void EnterLightweight();
void LeaveLightweight(bool show = true);
void ToggleLightweight();
bool LightweightHidesWindow();  // on, with nothing in the window's place (it is hidden, nothing draws)
bool LightweightPausesWork();
void LightweightSizeChanged(ViewSize size);
void LightweightPreferencesChanged();
bool LightweightKey(const SDL_KeyboardEvent& e);  // the numpad as a remote while the window has the keys
void LightweightPickerClosed();  // the song picker closed: a window shown only for it goes away again
// The song picker (panel_palette.cpp): the palette searching the library, as the Windows build's.
void OpenSongPicker();

// Your own keyboard shortcuts (keys.cpp, File > Keyboard shortcuts), as the Windows build's app_keys.cpp:
// "in cordinal" keys run before cordinal's own, "from any program" keys go to the desktop's shortcut portal
// (yours always, cordinal's own only in Lightweight mode), and the numpad's bare keys are Lightweight mode's.
bool KeyFromEvent(const SDL_KeyboardEvent& e, keymap::Key& out);
std::vector<keymap::Binding> DefaultKeyBindings();  // the menus' shortcuts and keymap::DefaultGlobal()
std::vector<keymap::Binding> EffectiveKeyBindings(keymap::Scope scope);
void SetKeyChanges(std::vector<keymap::Binding> changes);  // saves and applies them
bool UserKeyDown(const SDL_KeyboardEvent& e);  // one of your keys in cordinal: runs it, or swallows a cleared one
bool RunCommandNamed(const std::wstring& name);  // "Playback > Next" through the menus; false when none
void ApplyGlobalKeys();  // the portal's bindings again, after your keys or Lightweight mode changed
// packs_ui.cpp: the portal triggers for numpad key `index` under a LightweightNumpad mode, and every
// binding the portal is asked for ("KP_Begin=Playback > Play / Pause"), for the layout check.
std::vector<std::string> NumpadTriggers(int index, int mode);
std::vector<std::string> GlobalKeyTriggers();
void OpenKeyboardShortcuts();
std::vector<std::string> KeyboardShortcutRows(const std::wstring& query);  // layout check: the open panel's rows
bool NumpadCommand(int index, bool repeat);  // Lightweight mode's numpad key `index`: runs its command
void StepVolume(bool up);  // up or down 5%, as the mini player's wheel does

// The song picker's tray entry and its self-test hooks (panel_palette.cpp, lightweight.cpp).
void OpenSongPickerFromTray();  // with the window shown just for it, when the mode hid it
std::vector<std::string> SongPickerRows();  // self-test: "label | path" per shown row
void SetSongPickerQuery(const std::wstring& query);

// Helpers (helpers_ui.cpp, View > Helpers): separate programs that decode, tag or run on tracks.
struct HelperRow {
    std::wstring name, version, author, description, summary, programs, error, why, offAll;
    std::vector<std::wstring> warnings;
    bool on = false;
};
void LoadHelpers();
size_t HelperCount();
HelperRow HelperAt(size_t index);
void ToggleHelper(size_t index);
void OpenHelpersFolder();
bool InstallHelper(const std::wstring& source);
std::vector<std::wstring> HelperVisualizerLabels();
bool HelperVisualizerRunning(size_t index);
bool HelperVisualizersRunning();
void ToggleHelperVisualizer(size_t index);
void OpenHelperVisualizers(float x, float y);
void TickHelperVisualizers();
// The library canvas's Smooth scrolling glide (libview_canvas.cpp): the loop ticks it while it runs.
void TickCanvasScroll();
bool CanvasScrollAnimating();
int CheckLinuxHelperFeed();
std::vector<std::wstring> HelperToolLabels();  // the tools of the helpers that are on, in menu order
int RunningHelperTool();                       // its index, or -1
void RunHelperTool(size_t index, const std::vector<TrackId>& ids);
void CancelHelperTool();
void OnHelperMessage(WPARAM wp, LPARAM lp);
void StopHelpers();

// Packs (packs_ui.cpp, View > Packs).
struct PackRow {
    std::wstring name, version, author, description, summary, error;
    std::vector<std::wstring> warnings;
    bool on = false;
};
int ColumnTotal();
ui::Column ColumnAt(int type);
std::string PackColumnKey(int type);
int PackColumnType(const std::string& key);
std::vector<std::wstring> PackColumnSetNames();
void ApplyPackColumnSet(size_t index);
std::vector<std::wstring> PackLayoutNames();
std::vector<WindowLayout> PackLayouts();  // the same, read
void ApplyPackLayout(size_t index);
// A layout in a pack's layout.ini words (ui_spec.h's presets, the blank canvas, a saved layout), and back.
WindowLayout LayoutOfValues(const IniSection& values);
WindowLayout LayoutOfText(std::string text);  // ';' may stand for its line breaks
std::wstring LayoutText(const WindowLayout& l);
bool PackGlobalKeysActive();
void TickPackGlobalKeys();
void StopPackGlobalKeys();
void LoadPacks();  // reads the packs folder and applies the ones that are on
size_t PackCount();
PackRow PackAt(size_t index);
void TogglePack(size_t index);
bool InstallPack(const std::wstring& source);
void OpenPacksFolder();
std::wstring MakeStarterPack();  // View > Packs > Make a pack: the current setup as a new pack (off); its folder, or empty
void ReloadPacks();               // reads the packs folder again, after editing a pack's files
std::string PackThemeKey(ThemeMode mode);         // the settings.ini key of a pack theme, empty for a built-in one
const PackTheme* PackThemeFor(ThemeMode mode);    // a pack theme's sounds and the like
bool PackField(TrackId id, const std::wstring& name, std::wstring& out);  // a pack's %field%
std::vector<std::wstring> PackAutoPlaylistNames();  // the auto-playlists of the packs that are on
void AddPackAutoPlaylist(size_t index);
void SetViewSize(ViewSize size);
void ApplyTheme();
bool PackKeyDown(const SDL_KeyboardEvent& e);  // a key a pack that is on binds to a menu command: runs it
std::vector<EqPreset> AllEqPresets();         // the built-in presets, then those of the packs that are on
// The pattern for a format slot: a pack that is on and sets it, else the user's own.
const std::wstring& EffectiveFormat(PackFormatSlot slot, const std::wstring& own);
const PackGroups* PackGroupLook();  // the group headers of the last pack that is on and sets them, or null

// The lyrics panel (lyrics.cpp).
bool LyricsShown();
float LyricsWidth();
void ToggleLyrics();
void OnLyricsLoaded(LPARAM lp);
void StopLyrics();
void LyricsTick();
bool LyricsAnimating();
void LyricsWheel(float dy);
bool LyricsMouseDown(float x, float y);
// Fetched lyrics (Preferences > Online) are kept only when chosen from the panel's right-click menu:
// in the song's tags (`tags`, one undoable edit) or as a .lrc file beside it.
void OpenLyricsMenu(float x, float y);  // ui_menus.cpp
bool CanSaveFetchedLyrics(bool tags);
void SaveFetchedLyrics(bool tags);
void LyricsSourcesChanged();  // a lyrics source was turned on or off: look again for the song shown

// Sound effects (sounds.cpp).
void PlaySfx(Sfx sound, int arg = 0, bool evenWhenOff = false);
void MessageBlip();  // a status message in the DELTARUNE theme, spoken in Queen's blip
bool RoboticSounds();
void ShutdownSounds();
// The playback queue (playback.cpp): Play next / Add to queue, played before the playlist continues.
void QueueTracks(const std::vector<TrackId>& ids, bool next);
void ClearQueue();
std::vector<TrackId> UpNextTracks();
void RefreshNext();
int QueuePosition(TrackId id);  // 1-based place in the queue, 0 when not queued
// The queue editor's edits (queue_ui.cpp), by queue position, as on Windows (app_playback.cpp).
void InsertIntoQueue(const std::vector<TrackId>& ids, int at);
void RemoveFromQueue(const std::vector<int>& indices);
int MoveInQueue(const std::vector<int>& indices, int to, bool keep = false);
void PlayQueuedAt(int index);
bool TastePicking();
void EnsureTastePicks();
void RepickTaste();
size_t QueuedByYou();
void InitWeb();
void StartWeb();
void StopWeb();
void TickWeb();
bool WebRunning();
web::Status WebStatus();
void PairWeb();
void RevokeWeb(const std::string& device);
// Away from home: the switch (asking for consent the first time), Check again, and what to show.
bool WebAway();
void SetWebAway(bool on);
void RetryWebAway();
std::string WebAwayText();
// Sharing with network players (DLNA): the switch, and what to show.
void SetWebShare(bool on);
std::string WebShareText();
struct WebPairing {
    std::string link;     // what the QR code carries; empty when there is no unspent link
    std::string caption;  // how to use it
};
WebPairing CurrentWebPairing();
// Keeps `typed` as the fixed pairing code (empty clears it); returns what to tell the user.
std::wstring SetWebFixedCode(const std::string& typed);

}  // namespace lx
