// Main-window startup, idle work and ordered shutdown.
#include "app.h"

namespace app {

// Builds the whole UI, called once from WM_CREATE. Order matters: LoadState first so
// controls honour restored state, then the tag worker and fonts/controls, the engine,
// and the tick timer last so no timer fires against a half-built window.
void CreateMainWindow(HWND hwnd) {
    g.hwnd = hwnd;
    g.closing = false;
    g.dpi = GetDpiForWindow(hwnd);
    LoadState();
    if (!g.showFocusBoxes) SendMessageW(hwnd, WM_CHANGEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS), 0);
    g.tagWorker = std::thread(TagWorker, hwnd);
    CreateFonts();
    CreateControls(hwnd);
    CreatePointsChip(hwnd);
    CreatePanels(hwnd);
    LoadPanels();
    UpdateOrderButtons();
    ApplyFonts();
    ApplyTheme();
    if (!g.message.empty()) SetStatusPart(0, g.message);
    RefreshTabs();
    RestorePlaylistPosition();
    std::wstring error;
    if (!g.engine.Init(hwnd, error, true)) ShowMessage(error);
    g.engine.SetLoops(g.loops, 8.0);
    g.engine.SetEq(g.eq);
    ApplyEffects();
    g.engine.SetLimiter(g.rgOn, g.rgHeadphoneHeadroom);
    if (g.exclusive) g.engine.SetExclusive(true);
    SetVolumePercent(g.volumePercent);
    if (g.alwaysOnTop) SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    DragAcceptFiles(hwnd, TRUE);
    UpdateInboxTab();
    StartScanner();
    SetTimer(hwnd, kUiTimer, 250, nullptr);
    Layout();
}

// Raises the parts that do not affect the first frame only after it is on screen: the tray icon,
// the media flyout and media keys, headset polling, peaks and scrobbling. On a slow launch these
// used to sit between the window being ready and its first paint.
void FinishStartup(HWND hwnd) {
    g.startupTheme = false;
    InitTaskbar();
    InitTray();
    g.headsets.Start();
    InitPlatform(hwnd);
    UpdateNowPlaying();
    FountainWake();  // ApplyTheme skipped it while the window was still hidden
    InitConvert();
    InitWebServer();
}

// Ordered shutdown of every worker. Each is signalled and joined, then its already
// posted result messages are drained and handled so the heap objects they carry are
// freed and nothing is left in the message queue when the window is destroyed.
void StopAppWorkers(HWND hwnd) {
    DestroyRadio();
    StopHelpers();  // first: it ends helper programs that workers below may be waiting on
    StopLyrics();
    StopBackup();
    StopWebServer();
    g.headsets.Stop();
    StopBackgroundJobs();
    KillTimer(hwnd, kUiTimer);
    KillTimer(hwnd, kBusyTimer);
    KillTimer(hwnd, kSearchTimer);
    g.scanner.Stop();
    StopTagWriter();
    StopOrganizer();
    StopConverter();
    StopCopier();
    MSG completed;
    while (PeekMessageW(&completed, hwnd, WM_APP_TAGWRITE, WM_APP_TAGWRITE, PM_REMOVE))
        OnTagWriteMessage(completed.wParam, completed.lParam);
    while (PeekMessageW(&completed, hwnd, WM_APP_ORGANIZE, WM_APP_ORGANIZE, PM_REMOVE))
        OnOrganizeDone(completed.lParam);
    StopGroupArt();
    {
        std::lock_guard<std::mutex> lock(g.tagMx);
        g.tagQuit = true;
    }
    g.tagCv.notify_all();
    if (g.tagWorker.joinable()) g.tagWorker.join();
    MSG tagMessage;
    while (PeekMessageW(&tagMessage, hwnd, WM_APP_META, WM_APP_META, PM_REMOVE))
        AcceptTagBatch(reinterpret_cast<std::vector<TagResult>*>(tagMessage.lParam));
    g.rgCancel = true;
    if (auto decoder = g.rgDecoder.load()) decoder->Abort();
    if (g.rgWorker.joinable()) g.rgWorker.join();
    while (PeekMessageW(&tagMessage, hwnd, WM_APP_RG, WM_APP_RG, PM_REMOVE))
        FinishReplayGain(tagMessage.wParam, tagMessage.lParam);
    while (PeekMessageW(&tagMessage, hwnd, WM_APP_SCAN, WM_APP_SCAN, PM_REMOVE))
        OnScanResult(reinterpret_cast<ScanResult*>(tagMessage.lParam), tagMessage.wParam == 1);
    g.tagQueue.clear();
    g.tagBatch.clear();
    g.tagPending.clear();
    g.tagAgain.clear();
    g.tagBatchInFlight = false;
}

// Final teardown on WM_DESTROY: workers, platform/tray/taskbar, audio engine and sfx,
// then quit the message loop.
void DestroyMainWindow(HWND hwnd) {
    StopAppWorkers(hwnd);
    ShutdownPlatform();
    ShutdownTray();
    ShutdownTaskbar();
    g.engine.Shutdown();
    sfx::Shutdown();
    PostQuitMessage(0);
}

// The kUiTimer tick (~250 ms): settle UI work in a fixed order — compaction, listening
// stats, region looping before the status paints, deferred tags, a throttled library
// refresh, status/now-playing/tray/panels/lyrics — then autosave on a long interval.
void MainTick() {
    RadioTick();
    HeadphonesTick();
    ReplayGainEqTick();
    if (g.compactPending && !g.searchPending) CompactLibrary();
    TrackListening();
    FlushDeferredTags();
    MaybeAutoMeasure(false);
    if (!g.busyTimer && AnyTrackBusy()) g.busyTimer = SetTimer(g.hwnd, kBusyTimer, 90, nullptr) != 0;
    if (g.viewRevision != g_lib.Revision() && Active().IsView() && GetTickCount64() - g.viewBuilt > 1000)
        RefreshViewKeepingSelection();
    UpdateStatus();
    if (g.playing != kNone) UpdateNowPlaying();
    UpdateTray();
    PanelsTick();
    LyricsTick();
    if (g.dirty && ++g.ticksSinceSave >= 80) {  // ~20 s
        SaveLibraryNow();
    }
    MaybeOfferPreferenceSuggestions();
}

}  // namespace app
