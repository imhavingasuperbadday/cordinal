// cordinal for Linux: start-up, the event loop and shutdown.
//
//   cordinal [files or folders...]   opens them in Now Playing and plays (or hands them to the
//                                    cordinal already running)
//   cordinal --play-pause | --play | --pause | --stop | --next | --previous
//                                    controls the running cordinal (through MPRIS)
//   cordinal --check-layout          compares the window's layout with the Windows build's (ui_spec.h)
//   cordinal --capture-ui DIR        renders every theme and menu into DIR (linux/capture-ui.sh)
#include "alternate_ui.h"
#include "big_ui.h"
#include "headphones_ui.h"
#include "app.h"
#include "customcover.h"
#include "metadata_fetch.h"
#include "musicbrainz.h"
#include "radio_ui.h"
#include "cover.h"
#include "panels.h"
#include "peaks.h"
#include "text.h"
#include "ui.h"


#include <clocale>
#include <cstring>
#include <unistd.h>

namespace lx {

App g;

namespace {

struct Message {
    UINT msg;
    WPARAM wp;
    LPARAM lp;
};
std::mutex g_queueMx;
std::deque<Message> g_queue;
Uint32 g_wakeEvent = 0;

void Dispatch(const Message& m) {
    switch (m.msg) {
        case WM_ENGINE: OnEngineEvent((EngineEvent)m.wp, (unsigned long long)m.lp); break;
        case WM_APP_SCAN: OnScanResult(reinterpret_cast<ScanResult*>(m.lp), m.wp == 1); break;
        case WM_APP_TAGS: AcceptTags(reinterpret_cast<std::vector<TagResult>*>(m.lp)); break;
        case WM_APP_CHAPTERS: AcceptChapters(reinterpret_cast<ChapterResult*>(m.lp)); break;
        case WM_APP_COVER: SetCover(reinterpret_cast<CoverImage*>(m.lp)); break;
        case WM_APP_THUMB: SetThumb(reinterpret_cast<CoverImage*>(m.lp)); break;
        case WM_APP_PEAKS: OnPeaksReady((uint32_t)m.wp); break;
        case WM_APP_REMOTE: OnRemote((Remote)m.wp, m.lp); break;
        case WM_APP_WEB: TickWeb(); g.redraw = true; break;
        case WM_APP + 30: OnFoldersChosen(reinterpret_cast<std::vector<std::wstring>*>(m.lp)); break;
        case WM_APP_RG: FinishReplayGain(m.wp, m.lp); break;
        case WM_APP_TAGWRITE: OnTagWriteMessage(m.wp, m.lp); break;
        case WM_APP_CORD_EXPORT: OnCordExport(m.wp, m.lp); break;
        case WM_APP_CONVERT: OnConvertMessage(m.wp, m.lp); break;
        case WM_APP_EXPAND: OnExpanded(); break;
        case WM_APP_ORGANIZE: OnOrganizeDone(); break;
        case WM_APP_COPY: OnCopyMessage(m.wp, m.lp); break;
        case WM_APP_COLLECTION: OnCollectionMessage(m.wp, m.lp); break;
        case WM_APP_IPOD: OnIpodMessage(m.wp, m.lp); break;
        case WM_APP_COVERFETCH: OnCoverFetchMessage(m.wp, m.lp); break;
        case WM_APP_ARTISTINFO: OnArtistInfoMessage(m.lp); break;
        case WM_APP_PLAYLIST_FILE: OnPlaylistFile(m.wp, m.lp); break;
        case WM_APP_LISTENBRAINZ: OnListenBrainzMessage(m.wp, m.lp); break;
        case WM_APP_LYRICS: OnLyricsLoaded(m.lp); break;
        case WM_APP_HELPER: OnHelperMessage(m.wp, m.lp); break;
        case WM_APP_GAME: OnGameResult(m.lp); break;
        case WM_APP_FILE: OnFileChosen((int)m.wp, reinterpret_cast<std::wstring*>(m.lp)); break;
        case WM_APP + 32: OnMigration(m.wp,m.lp); break;
        default: break;
    }
}

void DrainMessages() {
    for (;;) {
        Message m;
        {
            std::lock_guard lock(g_queueMx);
            if (g_queue.empty()) return;
            m = g_queue.front();
            g_queue.pop_front();
        }
        Dispatch(m);
    }
}

}  // namespace

// Files and folders from the command line (or a file manager), once expanded: into Now Playing, the
// first plays.
void OpenFiles(const std::vector<std::wstring>& files, bool play) {
    if (files.empty()) return;
    std::vector<TrackId> ids, unread;
    for (auto& f : files) {
        TrackId id = g_lib.Add(f);
        ids.push_back(id);
        if (!g_lib[id].Loaded()) unread.push_back(id);
    }
    QueueTags(unread);
    const int np = EnsureNowPlayingTab();
    auto& items = g.playlists[np].items;
    int first = (int)items.size();
    std::vector<TrackId> before = items;
    items.insert(items.end(), ids.begin(), ids.end());
    RecordPlaylistEdit(g.playlists[np], std::move(before), L"add");
    g.libraryDirty = true;
    SetActiveTab(np);
    if (play) PlayFromQueue(first);
}

// Runs the worker hand-offs until the scan, the tags and the album covers are done (at most 30 s).
void CapturePump() {
    uint64_t start = SDL_GetTicks(), quiet = 0;
    while (SDL_GetTicks() - start < 30000) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {}
        TickPackGlobalKeys();
        TickHelperVisualizers();
        DrainMessages();
        if (g.rowsRevision != g_lib.Revision()) RebuildRows();
        bool tags;
        {
            std::lock_guard lock(g.tagMx);
            tags = !g.tagQueue.empty() || g.tagBusy;
        }
        if (g.scanning || tags || UiBusy()) quiet = 0;
        else if (!quiet) quiet = SDL_GetTicks();
        else if (SDL_GetTicks() - quiet > 600) return;
        SDL_Delay(20);
    }
}

void OnRemote(Remote r, LPARAM value) {
    switch (r) {
        case Remote::PlayPause: TogglePause(); break;
        case Remote::Play:
            if ((g.playing == kNone && !RadioPlaying()) || IsPaused()) TogglePause();
            break;
        case Remote::Pause:
            if ((g.playing != kNone || RadioPlaying()) && !IsPaused()) TogglePause();
            break;
        case Remote::Stop:
            HoldPendingOpens();
            Stop();
            break;
        case Remote::Next: Next(); break;
        case Remote::Previous: Previous(); break;
        case Remote::Raise:
            SDL_RestoreWindow(g.window);
            SDL_RaiseWindow(g.window);
            break;
        case Remote::Quit: g.quit = true; break;
        case Remote::Seek: SeekTo(g.engine.Position() + value / 1e6); break;
        case Remote::SetPosition: SeekTo(value / 1e6); break;
        case Remote::SetVolume:
            SetVolume((float)(value / 1e6));
            SaveSettings();
            break;
        case Remote::SetShuffle:
            if ((value != 0) != (g.settings.shuffle != 0)) SetShuffle(value ? kShuffleTracks : kShuffleOff);
            break;
        case Remote::SetRepeat:
            SetRepeat((Repeat)std::clamp((int)value, 0, 2));
            break;
        case Remote::Open: {
            std::unique_ptr<std::vector<std::wstring>> paths(reinterpret_cast<std::vector<std::wstring>*>(value));
            ExpandPaths(*paths, Opening::Play);
            SDL_RaiseWindow(g.window);
            break;
        }
        case Remote::ToggleStartOnLogin: ToggleStartOnLogin(); break;
    }
}

int Run(int argc, char** argv) {
    setlocale(LC_ALL, "");
    // Numbers in settings.ini, ReplayGain tags and AutoEq text always use a decimal point.
    setlocale(LC_NUMERIC, "C");
    // One cordinal at a time: hand files or a command to the one running.
    // The parity checks run on their own, without a display or another cordinal: an offscreen window
    // at scale 1, the Windows build's default size.
    std::string captureDir;
    bool checkLayout = argc == 2 && !strcmp(argv[1], "--check-layout");
    if (argc == 3 && !strcmp(argv[1], "--capture-ui")) captureDir = argv[2];
    bool parity = checkLayout || !captureDir.empty();
    std::vector<std::string> forward;
    bool command = !parity && argc == 2 && argv[1][0] == '-' && argv[1][1] == '-';
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (!command && a[0] != '/') {
            char* full = realpath(argv[i], nullptr);
            if (full) a = full;
            free(full);
        }
        forward.push_back(a);
    }
    if (!parity && ForwardToRunning(forward)) return 0;
    if (command) {
        fprintf(stderr, "cordinal isn't running\n");
        return 1;
    }
    std::vector<std::wstring> paths;
    for (int i = 1; i < argc && !parity; ++i) {
        std::wstring full(4096, L'\0');
        DWORD n = GetFullPathNameW(compat::Widen(argv[i]).c_str(), (DWORD)full.size(), full.data(), nullptr);
        full.resize(n < full.size() ? n : 0);
        if (!full.empty()) paths.push_back(full);
    }

    SDL_SetAppMetadata("cordinal", "0.1.12", "cordinal");
    SDL_SetHint(SDL_HINT_VIDEO_WAYLAND_PREFER_LIBDECOR, "1");
    if (parity) SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        fprintf(stderr, "cordinal: %s\n", SDL_GetError());
        return 1;
    }
    InitPaths();
    InitRadio();
    LoadSettings();
    StartTagWorker();
    LoadLibraryFile();
    customcover::Start(g.dataDir);  // before any cover is decoded
    musicbrainz::Start(g.dataDir, g.settings.musicBrainzLookup);
    online::Start(g.dataDir);

    Settings& s = g.settings;
    if (parity) {
        s.windowW = ui::kLayoutWidth;
        s.windowH = ui::kMenuBarHeight + (checkLayout ? ui::kLayoutHeight : 662);
        s.maximized = false;
    }
    // Drawing is software (src/linux/platform/render.cpp) into shared memory the window system
    // shows (MIT-SHM on X11, wl_shm on Wayland): no GPU driver is loaded into the process.
    g.window = SDL_CreateWindow("cordinal", s.windowW, s.windowH, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                                                     (s.maximized ? SDL_WINDOW_MAXIMIZED : 0));
    if (!g.window) {
        fprintf(stderr, "cordinal: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetWindowMinimumSize(g.window, 520, 320);
    // The cardinal for the title bar and the task switcher (desktops that read it from the .desktop file
    // show the installed icon instead).
    CoverImage icon{};
    std::string_view png = AppIcon(64);
    if (DecodeCover(std::string(png), 64, icon)) {
        if (SDL_Surface* surface = SDL_CreateSurfaceFrom(icon.width, icon.height, SDL_PIXELFORMAT_ABGR8888, icon.rgba.data(), icon.width * 4)) {
            SDL_SetWindowIcon(g.window, surface);
            SDL_DestroySurface(surface);
        }
    }
    // Software keeps the GPU driver's memory and threads out of the process; a full redraw fits in a
    // frame, and frames are drawn only when something changes. Preferences can choose the GPU instead.
    // Native Wayland has no window surface, so SDL's software renderer cannot start there. Try the
    // chosen mode first and the other one second, rather than leaving the user with no window at all.
    const bool software = parity || !s.gpuDrawing;
    g.renderer = SDL_CreateRenderer(g.window, software ? SDL_SOFTWARE_RENDERER : nullptr);
    if (!g.renderer && !parity) {
        std::string chosen = SDL_GetError();
        g.renderer = SDL_CreateRenderer(g.window, software ? nullptr : SDL_SOFTWARE_RENDERER);
        if (g.renderer)
            fprintf(stderr, "cordinal: %s; falling back to %s drawing\n", chosen.c_str(),
                    software ? "graphics card" : "software");
    }
    if (!g.renderer) {
        fprintf(stderr, "cordinal: %s\n", SDL_GetError());
        return 1;
    }
    // The preference follows the renderer that actually started, so the next launch does not retry
    // a mode this display cannot run.
    if (!parity) s.gpuDrawing = std::string_view(SDL_GetRendererName(g.renderer)) != SDL_SOFTWARE_RENDERER;
    SDL_SetRenderVSync(g.renderer, 1);
    SDL_SetRenderDrawBlendMode(g.renderer, SDL_BLENDMODE_BLEND);
    g.scale = parity ? 1 : SDL_GetWindowDisplayScale(g.window);
    if (g.scale <= 0) g.scale = 1;
    if (!InitText(g.renderer, g.scale, ThemeInfo(s.theme))) {
        fprintf(stderr, "cordinal: no usable font (install a sans-serif font, e.g. noto-fonts)\n");
        return 1;
    }

    // Worker threads post here; the event loop wakes and dispatches on this thread.
    g_wakeEvent = SDL_RegisterEvents(1);
    SetPostMessageHandler([](HWND, UINT msg, WPARAM wp, LPARAM lp) {
        {
            std::lock_guard lock(g_queueMx);
            g_queue.push_back({msg, wp, lp});
        }
        SDL_Event e{};
        e.type = g_wakeEvent;
        SDL_PushEvent(&e);
    });

    std::wstring error;
    // The engine only posts events for a non-null target. The Linux compatibility layer routes
    // this handle to the UI queue, just as it does for the waveform worker below.
    if (!g.engine.Init(reinterpret_cast<HWND>(&g), error)) ShowMessage(error);
    SetVolume(s.volume);
    if (s.exclusive) g.engine.SetExclusive(true);
    g.engine.SetOutputDevice(s.outputDevice);
    g.engine.SetEq(s.eq);
    ApplyEffects();
    RefreshGains();  // the limiter follows ReplayGain
    StartCovers();
    // Waveforms are cached beside the other caches: $XDG_CACHE_HOME/cordinal/peaks.
    const char* cache = getenv("XDG_CACHE_HOME");
    const char* home = getenv("HOME");
    std::string cacheDir = cache && *cache == '/' ? cache : std::string(home ? home : "/tmp") + "/.cache";
    // peaks.cpp tells a window when a waveform is done; any handle reaches this thread's queue here.
    StartPeaks(reinterpret_cast<HWND>(&g), WM_APP_PEAKS, compat::Widen((cacheDir + "/cordinal/peaks").c_str()));
    StartMpris();
    if (!parity) StartHeadphones();
    InitConvert();
    StartScanner();
    g.active = std::clamp(s.activeTab, 0, (int)g.playlists.size() - 1);
    InitUi();
    if (!parity) InitializePresentation();
    if (g.settings.alwaysOnTop) SDL_SetWindowAlwaysOnTop(g.window, true);
    SetActiveTab(g.active);
    LoadPacks();
    LoadHelpers();
    ResetPreferenceSuggestions();
    InitWeb();
    InitScrobble();
    if (!paths.empty()) ExpandPaths(paths, Opening::Play);
    else if (!parity) ReopenLastTrack();  // the last session's song, paused or playing, as Preferences asks
    if (s.folders.empty() && paths.empty()) ShowMessage(L"Welcome! Add your music: Library \x25B8 Library folders...");
    // Diagnostics leave the user's profile untouched.
    if (checkLayout) {
        int off = CheckLayout();
        if (off) printf("%d layout or pack checks failed\n", off);
        else printf("The layout matches Windows; pack checks passed.\n");
        fflush(stdout);
        _exit(off ? 1 : 0);
    }
    if (!captureDir.empty()) {
        CapturePump();
        int failed = CaptureUi(captureDir);
        fflush(stdout);
        _exit(failed ? 1 : 0);
    }

    MaybeOfferSteamSoundtracks();

    uint64_t lastTick = 0;
    while (!g.quit) {
        // Wake for the clock while something plays (and the search box's caret blinks).
        int timeout = (g.playing != kNone || RadioPlaying()) && !IsPaused() ? 250 : g.searchFocused ? 530 : 2000;
        if (InternetRadio().browser && InternetRadio().browser->Searching()) timeout = std::min(timeout, 250);
        if (HelperVisualizersRunning()) timeout = std::min(timeout, 16);
        if (CanvasScrollAnimating()) timeout = std::min(timeout, 16);
        if (UiAnimating()) timeout = SeekAnimationOnly() && !g.settings.wave60Fps ? 33 : ui::kFoobarFrameMs;
        if (PlaylistImportPending()) timeout = 0;
        if (PackGlobalKeysActive()) timeout = std::min(timeout, 100);
        if (WebRunning()) timeout = std::min(timeout, 20);
        if (PanelReplanPending()) {  // the organize preview, once typing pauses
            timeout = std::min(timeout, 100);
            g.redraw = true;
        }
        if (g.rowsRevision != g_lib.Revision()) {  // the list is behind the library: catch up shortly
            timeout = 150;
            g.redraw = true;
        }
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, timeout)) {
            do {
                if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) g.quit = true;
                else if (e.type != g_wakeEvent) HandleEvent(e);
            } while (SDL_PollEvent(&e));
        }
        TickPackGlobalKeys();
        TickHelperVisualizers();
        TickCanvasScroll();
        DrainMessages();
        TickPlaylistImport();
        RadioTick();
        if (WebRunning()) TickWeb();
        uint64_t now = SDL_GetTicks();
        if (now - lastTick >= 250) {
            lastTick = now;
            TickAlternate();
            HeadphonesTick();
            PlaybackTick();
            DiscordTick();
            PreferenceTick(LastInputTicks());
            if (musicbrainz::Tick()) {
                InvalidateCellCache();
                RebuildRows();
                g.libraryDirty = true;
                g.redraw = true;
            }
            MaybeAutoMeasure(false);
            if (g.playing != kNone || RadioPlaying() || g.searchFocused || now < g.messageUntil + 300) g.redraw = true;
        }
        if (g.libraryDirty && now - g.lastSave > 5 * 60 * 1000) SaveLibraryFile(false);
        const bool seekOnly = SeekAnimationOnly();
        if (UiAnimating() && !seekOnly) g.redraw = true;
        if (g.redraw) Render();
        else if (seekOnly) RenderSeek();
    }

    // Window size and the rest, then the library (play counts, playlists).
    LeaveMini();  // the window size to remember is the full window's
    SavePresentationWindow();
    SaveSettings();
    StopPlaylistFiles();
    StopMigration();
    StopCordExporter();
    StopConverter();
    StopOrganizer();
    StopCopier();
    StopCollection();
    StopIpod();
    StopArtistInfo();
    StopDiscord();
    StopCoverFetch();
    StopExpanders();
    FlushAutomaticPreferences();
    StopWeb();
    StopScrobble();
    StopPackGlobalKeys();
    StopAlternate();
    StopHeadphones();
    StopHelpers();
    ShutdownSounds();
    StopReplayGain();
    Stop();  // finish the current listening session before saving the library
    ShutdownRadio();
    musicbrainz::Stop();
    StopTagWriter();
    StopMpris();
    g.scanner.Stop();
    StopTagWorker();
    StopCovers();
    StopLyrics();
    StopPeaks();
    SetPostMessageHandler(nullptr);
    DrainMessages();
    SaveLibraryFile(true);
    g.engine.Shutdown();
    ShutdownUi();
    ShutdownText();
    SDL_DestroyRenderer(g.renderer);
    SDL_DestroyWindow(g.window);
    FinishMigration();
    SDL_Quit();
    return 0;
}

}  // namespace lx

int main(int argc, char** argv) { return lx::Run(argc, argv); }
