// Hidden native UI checks of the self test (see selftest.cpp): real Win32 controls under a test
// window, covering the tabs, playback strip and background jobs, and ending with the recovered
// tab/playback behaviour checks in port_regressions.cpp. No player instance or user state is used.
#include "selftest_common.h"
#include "radio.h"

void TestPersonalization(const std::wstring& scratch);
namespace app { bool LyricsReadyFor(TrackId id, const std::wstring& line); }

namespace {
constexpr int kUiWidth = 1163, kUiHeight = 752;

// Real scanner results survive saving and reopening, including silence (no usable LUFS),
// older measurements that predate rgHigh, and a measured album's gain. No user library or
// settings are opened.
void TestReplayGainRestart(const std::wstring& scratch) {
    using namespace app;
    testlog::Group group(L"ReplayGain across restarts");
    const std::wstring sine = scratch + L"\\rg-restart-sine.wav", silent = scratch + L"\\rg-restart-silent.wav";
    const std::wstring albumA = scratch + L"\\rg-restart-album-a.wav", albumB = scratch + L"\\rg-restart-album-b.wav";
    const std::wstring albumC = scratch + L"\\rg-restart-album-c.wav";
    const std::wstring file = scratch + L"\\rg-restart.cdb";
    WriteTestBytes(sine, MakeSineWav(2, 2));
    WriteTestBytes(silent, MakeSineWav(2, 0));
    WriteTestBytes(albumA, MakeSineWav(2, 2));
    WriteTestBytes(albumB, MakeSineWav(2, 2));
    g_lib = Library();
    ForgetAlbumDynamics();
    g.playlists = {{L"Library", {}, PlaylistKind::Library}};
    g.active = 0;
    g.compactPending = false;
    g.rgOn = g.rgAutoScan = true;
    g_lib.Add(scratch + L"\\rg-unsaved.wav");  // saving renumbers the retained tracks
    auto add = [](const std::wstring& path) {
        TrackId id = g_lib.Add(path);
        TagData tags;
        ReadTags(path, tags);
        g_lib.ApplyTags(id, tags);
        g_lib[id].flags |= Track::kInLibrary;
        return id;
    };
    auto album = [](TrackId id) {
        g_lib[id].album = g_lib.Intern("Restart album");
        g_lib[id].artist = g_lib.Intern("Restart artist");
        g_lib[id].date = g_lib.Intern("2020");
    };
    TrackId measured = add(sine), failed = add(silent), a = add(albumA), b = add(albumB);
    album(a);
    album(b);
    const std::wstring oldPath = scratch + L"\\rg-older-scan.wav";
    TrackId older = add(oldPath);
    g_lib[older].rgTrackGain = -650;
    g_lib[older].flags |= Track::kRgScanned;
    auto finish = [] {
        const ULONGLONG start = GetTickCount64();
        do {
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (!g.rgRunning) return true;
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        } while (GetTickCount64() - start < 10000);
        return false;
    };
    ScanReplayGain({measured, failed, a, b});
    const bool scanned = finish();
    auto stored = g_albumMeasurements.find(AlbumKeyOf(g_lib[a]));
    Check(scanned && g_lib[measured].rgHigh && (g_lib[measured].flags & Track::kRgScanned) &&
              (g_lib[failed].flags & Track::kRgScanFailed) && stored != g_albumMeasurements.end() &&
              stored->second.members.size() == 2 && g_lib[a].rgAlbumGain == g_lib[b].rgAlbumGain &&
              g_lib[a].rgAlbumGain != kNoGain && (g_lib[a].flags & Track::kRgAlbumMeasured) &&
              (g_lib[b].flags & Track::kRgAlbumMeasured),
          L"RG saves completed, unusable and album measurements");
    const auto gain = g_lib[measured].rgTrackGain;
    const auto peak = g_lib[measured].rgTrackPeak;
    const auto high = g_lib[measured].rgHigh;
    const auto albumGain = g_lib[a].rgAlbumGain;
    const auto albumPeak = g_lib[a].rgAlbumPeak;
    bool saved = SaveLibrary(file, g.playlists);
    g_lib = Library();
    bool loaded = LoadLibrary(file, g.playlists);
    measured = g_lib.Find(sine);
    failed = g_lib.Find(silent);
    older = g_lib.Find(oldPath);
    a = g_lib.Find(albumA);
    b = g_lib.Find(albumB);
    bool found = g_lib.Contains(measured) && g_lib.Contains(failed) && g_lib.Contains(older) &&
                 g_lib.Contains(a) && g_lib.Contains(b);
    Check(saved && loaded && found && measured == 0 && g_lib[measured].rgTrackGain == gain &&
              g_lib[measured].rgTrackPeak == peak && g_lib[measured].rgHigh == high &&
              (g_lib[failed].flags & Track::kRgScanFailed) && !g_lib[older].rgHigh,
          L"RG results and failed attempts survive reopening with renumbered track IDs");
    auto reloaded = found ? g_albumMeasurements.find(AlbumKeyOf(g_lib[a])) : g_albumMeasurements.end();
    Check(found && reloaded != g_albumMeasurements.end() && reloaded->second.members.size() == 2 &&
              reloaded->second.gain == albumGain && reloaded->second.peak == albumPeak &&
              g_lib[a].rgAlbumGain == albumGain && (g_lib[a].flags & Track::kRgAlbumMeasured),
          L"RG a measured album comes back with its gain, peak and members");
    if (found) {
        g.rgAlbumsRevision = 0;
        g.rgNextAuto = 1;
        unsigned generation = g.rgGen;
        MaybeAutoMeasure(false);
        Check(g.rgGen == generation && !g.rgRunning,
              L"RG startup does not rescan completed, older or unsuccessful measurements");
        // Make the saved album a lift, so its stored levelling decides the gain: a fresh
        // calculation would ignore a dynamics value that did not come from its tracks.
        g_albumMeasurements[AlbumKeyOf(g_lib[a])].gain = g_lib[a].rgAlbumGain = g_lib[b].rgAlbumGain = 500;
        g_lib.Touch();
        g.rgNextAuto = 1;
        MaybeAutoMeasure(false);
        const float plain = RgGainFor(a);
        g_albumMeasurements[AlbumKeyOf(g_lib[a])].dynamics = 60;  // 6 dB: the most a lift is held back
        g_lib.Touch();
        g.rgNextAuto = 1;
        MaybeAutoMeasure(false);
        Check(!g.rgRunning && plain > 1.5f && RgGainFor(a) < plain - 0.01f,
              L"RG a saved album's levelling is kept and applied, not worked out again");
        // A track added to the album does not take the measured album's gain away while it waits.
        g.rgAutoScan = false;
        WriteTestBytes(albumC, MakeSineWav(2, 2));
        TrackId c = add(albumC);
        album(c);
        g_lib.Touch();
        g.rgNextAuto = 1;
        MaybeAutoMeasure(false);
        Check(g_lib[a].rgAlbumGain == 500 && (g_lib[a].flags & Track::kRgAlbumMeasured) && g_lib[c].rgAlbumGain == kNoGain,
              L"RG a measured album keeps its gain while a track added since is unmeasured");
        const float partialGain = RgGainFor(a);
        saved = SaveLibrary(file, g.playlists);
        ForgetAlbumDynamics();
        loaded = LoadLibrary(file, g.playlists);
        g.rgNextAuto = 1;
        MaybeAutoMeasure(false);
        Check(saved && loaded && std::fabs(RgGainFor(a) - partialGain) < 0.0001f,
              L"RG an incomplete album restores its saved levelling after restart");
        // Compaction can renumber both handles and IDs; it must preserve the same measurement.
        std::vector<TrackId> live;
        for (TrackId id = 0; id < g_lib.Count(); ++id) live.push_back(id);
        std::reverse(live.begin(), live.end());
        auto mapping = g_lib.Compact(live);
        a = mapping[a];
        failed = mapping[failed];
        ForgetAlbumDynamics(true);
        MaybeAutoMeasure(false);
        auto compacted = g_albumMeasurements.find(AlbumKeyOf(g_lib[a]));
        Check(compacted != g_albumMeasurements.end() && compacted->second.members.size() == 2 &&
                  std::fabs(RgGainFor(a) - partialGain) < 0.0001f,
              L"RG compaction remaps saved members and keeps an incomplete album's levelling");
        g.rgAutoScan = true;
        finish();
        ScanReplayGain({failed});
        Check(g.rgGen > generation && finish() && (g_lib[failed].flags & Track::kRgScanFailed),
              L"RG an explicit scan retries a saved failure");
        ClearReplayGain({failed});
        Check(!(g_lib[failed].flags & Track::kRgScanFailed), L"RG clearing information also clears the failed attempt");
        g.rgNextAuto = 1;
        generation = g.rgGen;
        MaybeAutoMeasure(false);
        Check(g.rgGen > generation && finish(), L"RG unmeasured tracks still scan automatically");
    }
    ForgetAlbumDynamics();
    DeleteFileW(file.c_str());
    DeleteFileW((file + L".bak").c_str());
    DeleteFileW(sine.c_str());
    DeleteFileW(silent.c_str());
    DeleteFileW(albumA.c_str());
    DeleteFileW(albumB.c_str());
    DeleteFileW(albumC.c_str());
}

void TestFolderBrowser() {
    using namespace app;
    testlog::Group group(L"Folder browser");
    BrowserPane saved[3];
    const BrowseField fields[] = {BrowseField::Folder, BrowseField::Genre, BrowseField::Album};
    for (int i = 0; i < 3; ++i) {
        saved[i] = g.panes[i];
        g.panes[i].field = fields[i];
        g.panes[i].selected.clear();
    }
    auto add = [](const wchar_t* path, const char* genre) {
        TrackId id = g_lib.Add(path);
        TagData tags;
        tags.genre = genre;
        tags.album = "Folder test";
        g_lib.ApplyTags(id, tags);
        return id;
    };
    TrackId a = add(L"C:\\folder-browser\\Música; live\\Disc 2\\one.flac", "Rock");
    TrackId b = add(L"C:\\folder-browser\\Música; live\\Disc 2\\two.flac", "Jazz");
    TrackId c = add(L"C:\\folder-browser\\Other\\Disc 2\\three.flac", "Rock");
    TrackId child = add(L"C:\\folder-browser\\Música; live\\Disc 2\\Extras\\four.flac", "Rock");
    TrackId cue = add(L"C:\\folder-browser\\Música; live\\Disc 2\\image.flac|2", "Rock");
    TrackId root = add(L"C:\\folder-browser-root.flac", "Rock");
    TrackId unc = add(L"\\\\server\\music\\Disc 2\\five.flac", "Rock");
    const std::vector<TrackId> source{a, b, c, child, cue, root, unc};
    testlog::Cases paths;
    std::vector<StringPool::Handle> values;
    for (TrackId id : source) {
        BrowserValues(id, BrowseField::Folder, values);
        paths.Add(values == std::vector<StringPool::Handle>{g_lib[id].dir} &&
                      g_lib.Str(values.front()) == DirectoryOf(g_lib.Path(id)), g_lib.Path(id));
    }
    Check(paths.Passed(), L"folders use full paths, including Unicode, semicolons, CUE, drive roots and UNC%ls",
          paths.failed.c_str());
    auto items = source;
    FilterLibrary(items);
    auto& folder = g.panes[0];
    auto count = std::find(folder.values.begin(), folder.values.end(), g_lib[a].dir);
    Check(items == source && folder.values.size() == 5 && count != folder.values.end() &&
              folder.counts[count - folder.values.begin()] == 3,
          L"folder rows count their tracks and keep namesakes and subfolders separate");
    NMLVDISPINFOW row{};
    wchar_t text[256]{};
    row.hdr.code = LVN_GETDISPINFOW;
    row.item.mask = LVIF_TEXT;
    row.item.pszText = text;
    row.item.cchTextMax = (int)std::size(text);
    OnBrowserNotify(0, &row.hdr);
    Check(!wcscmp(text, L"All (5 folders)"), L"the Folder pane names its All row");
    folder.selected = {g_lib[a].dir};
    items = source;
    FilterLibrary(items);
    Check(items == std::vector<TrackId>{a, b, cue} && g.panes[1].values.size() == 2 &&
              g.panes[2].counts == std::vector<uint32_t>{3},
          L"selecting a folder filters tracks and later panes without including subfolders");
    g.panes[1].selected = {g_lib[a].genre};
    items = source;
    FilterLibrary(items);
    Check(items == std::vector<TrackId>{a, cue}, L"folder and tag selections narrow the same library view");
    g.panes[1].selected.clear();
    folder.selected.push_back(g_lib[c].dir);
    items = source;
    FilterLibrary(items);
    Check(items == std::vector<TrackId>{a, b, c, cue}, L"several selected folders combine their tracks");
    // A changed search or an earlier pane can remove a selected folder from the input.
    items = {child, unc};
    FilterLibrary(items);
    Check(folder.selected.empty() && items == std::vector<TrackId>{child, unc},
          L"folders no longer present upstream drop out of the selection");
    folder.selected.clear();
    items = source;
    FilterLibrary(items);
    Check(items == source && ListView_GetItemState(folder.list, 0, LVIS_SELECTED),
          L"All clears folder filtering and selects the All row");
    if (!kExtraThemes) {
        testlog::Skip(L"the foobar2000 layout: this build leaves out the extra themes");
    } else {
        const ThemeMode theme = g.themeMode;
        const auto selectedFolder = g_lib[0].dir;
        SelectBrowserValue(BrowseField::Folder, selectedFolder);
        OnCommand(IDM_THEME_BASE + (int)ThemeMode::Foobar, 0, nullptr);
        Check(FoobarMode() && folder.field == BrowseField::Folder &&
                  folder.selected == std::vector<StringPool::Handle>{selectedFolder},
              L"the foobar2000 layout keeps the column browser's folder choice and selection");
        OnCommand(IDM_THEME_BASE + (int)theme, 0, nullptr);
        Check(folder.field == BrowseField::Folder && folder.selected == std::vector<StringPool::Handle>{selectedFolder} &&
                  g.playlists[0].items == std::vector<TrackId>{0, 1, 2},
              L"returning from foobar2000 restores the selected folder's tracks");
    }
    for (int i = 0; i < 3; ++i) g.panes[i] = std::move(saved[i]);
    SaveBrowserSettings();
    g.playlists[0].builtRevision = 0;
    RebuildView();
}
void TestFoobarControls() {
    using namespace app;
    testlog::Group group(L"Foobar folders and transport controls");
    if (!kExtraThemes) {
        testlog::Skip(L"the foobar2000 controls: this build leaves out the extra themes");
        return;
    }
    const auto theme = g.themeMode;
    const UINT dpi = g.dpi;
    auto folders = g.libraryFolders;
    const int active = g.active;
    const ViewSize size = g.size;
    g.size = ViewSize::Normal;
    OnCommand(IDM_THEME_BASE + (int)ThemeMode::Foobar, 0, nullptr);
    Layout();  // the hidden test window does not route the real window's theme/size messages
    std::vector<TrackId> added;
    for (const wchar_t* path : {L"D:\\Custom Music\\Band\\Album\\one.flac",
                                L"E:\\Custom Music\\Band\\Album\\two.flac",
                                L"\\\\wsl.localhost\\Linux\\home\\user\\Music\\three.flac"}) {
        TrackId id = g_lib.Add(path);
        g_lib[id].flags |= Track::kInLibrary | Track::kLoaded;
        added.push_back(id);
    }
    g.libraryFolders = {L"D:\\Custom Music", L"E:\\Custom Music", L"\\\\wsl.localhost\\Linux\\home\\user\\Music"};
    SaveLibrarySettings();
    g.libraryFolders.clear();
    LoadLibrarySettings();
    Check(g.libraryFolders.size() == 3 && g.libraryFolders[0] == L"D:\\Custom Music" &&
              g.libraryFolders[2] == L"\\\\wsl.localhost\\Linux\\home\\user\\Music",
          L"custom roots on other drives and a Linux share survive saving and loading");
    HWND panel = FindWindowExW(g.hwnd, nullptr, L"CordinalFoobarPanel", nullptr);
    HWND tree = GetDlgItem(panel, IDC_FB_TREE);
    int customRoots = 0;
    HTREEITEM first = nullptr;
    for (HTREEITEM item = TreeView_GetRoot(tree); item; item = TreeView_GetNextSibling(tree, item)) {
        wchar_t text[512]{};
        TVITEMW row{};
        row.mask = TVIF_TEXT;
        row.hItem = item;
        row.pszText = text;
        row.cchTextMax = (int)std::size(text);
        TreeView_GetItem(tree, &row);
        if (wcsstr(text, L"Custom Music") || wcsstr(text, L"wsl.localhost")) {
            ++customRoots;
            if (!first) first = item;
        }
    }
    Check(tree && (GetWindowLongW(tree, GWL_STYLE) & TVS_LINESATROOT) && customRoots == 3 &&
              first && TreeView_GetChild(tree, first),
          L"native Album List exposes expandable, separate custom and Linux roots with their first level open (tree=%p roots=%d)",
          tree, customRoots);
    auto token = g.nextToken;
    if (first) TreeView_SelectItem(tree, first);
    UINT before = first ? TreeView_GetItemState(tree, first, TVIS_EXPANDED) : 0;
    NMHDR dbl{tree, IDC_FB_TREE, NM_DBLCLK};
    SendMessageW(panel, WM_NOTIFY, IDC_FB_TREE, (LPARAM)&dbl);
    Check(first && TreeView_GetItemState(tree, first, TVIS_EXPANDED) != before && g.nextToken == token,
          L"double-clicking a folder expands or collapses it without starting playback");
    ShowLibraryFolders();
    Check(GetDlgItem(g.foldersWnd, IDC_FOLDERS_MUSIC) && GetDlgItem(g.foldersWnd, IDC_FOLDERS_ADD),
          L"Library folders offers the system Music folder and a custom folder picker in every theme");
    DestroyWindow(g.foldersWnd);

    RECT originalWindow{};
    GetWindowRect(g.hwnd, &originalWindow);
    const Dock dock = g.layout.player;
    g.layout.player = Dock::Left;
    Layout();
    Check(!SeekVertical(), L"foobar2000 keeps horizontal sliders when the standard layout is side-docked");
    bool enabled = IsWindowEnabled(g.seek) != 0;
    EnableWindow(g.seek, TRUE);
    testlog::Cases layout, positions;
    for (UINT scale : {96u, 192u}) {
        g.dpi = scale;
        CreateFonts();
        ApplyFonts();
        for (int width : {640, 1120, 1440}) {
            SetWindowPos(g.hwnd, nullptr, 0, 0, S(width), S(700), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            Layout();
            RECT seek{}, volume{}, client{};
            GetWindowRect(g.seek, &seek);
            GetWindowRect(g.volume, &volume);
            MapWindowPoints(nullptr, g.hwnd, (POINT*)&seek, 2);
            MapWindowPoints(nullptr, g.hwnd, (POINT*)&volume, 2);
            GetClientRect(g.hwnd, &client);
            layout.Add(volume.right < seek.left && seek.top == volume.top && seek.bottom == volume.bottom &&
                           seek.right + S(14 + ui::kFoobarScopeMin) <= client.right,
                       std::to_wstring(scale) + L"/" + std::to_wstring(width));
        }
        RECT rc{}, track = FoobarSliderTrack(g.seek);
        GetClientRect(g.seek, &rc);
        for (int pos : {0, 250, 750, 1000}) {
            SendMessageW(g.seek, TBM_SETPOS, FALSE, pos);
            POINT at{track.left + (track.right - track.left) * pos / 1000, rc.bottom / 2};
            positions.Add(SendMessageW(g.seek, TBM_GETPOS, 0, 0) == pos &&
                          std::fabs(BarFracAt(g.seek, at) - pos / 1000.0) < 0.02,
                          std::to_wstring(scale) + L"/" + std::to_wstring(pos));
        }
    }
    Check(layout.Passed(), L"foobar transport, sliders and wide scope fit at narrow and wide sizes and 200%% DPI%ls", layout.failed.c_str());
    Check(positions.Passed(), L"seek positions and click coordinates agree at both DPIs%ls", positions.failed.c_str());
    EnableWindow(g.seek, enabled);
    for (TrackId id : added) g_lib[id].flags &= ~Track::kInLibrary;
    g_lib.Touch();
    g.libraryFolders = std::move(folders);
    SaveLibrarySettings();
    g.dpi = dpi;
    g.size = size;
    g.layout.player = dock;
    CreateFonts();
    ApplyFonts();
    OnCommand(IDM_THEME_BASE + (int)theme, 0, nullptr);
    SetWindowPos(g.hwnd, nullptr, 0, 0, originalWindow.right - originalWindow.left,
                 originalWindow.bottom - originalWindow.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    ActivatePlaylist(active);
    Layout();
}
}  // namespace

// Real Win32 controls under a hidden test window: no player instance or user settings are touched.
void TestUiControls(const std::wstring& scratch) {
    using namespace app;
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES | ICC_TAB_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);
    g.inst = GetModuleHandleW(nullptr);
    g.iniPath = scratch + L"\\ui-test-settings.ini";
    g.dataDir = scratch;
    radio::Station station;
    station.id = "local-station"; station.name = "Test rock station"; station.url = "http://127.0.0.1:1/";
    radio::Station frequencyStation;
    frequencyStation.id = "frequency-station"; frequencyStation.name = "98.5 Test FM";
    frequencyStation.url = "https://example.test/test.m3u8"; frequencyStation.hls = true;
    const std::wstring radioDir = scratch + L"\\radio";
    bool radioFolder = CreateDirectoryW(radioDir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
    if (!Ready(radioFolder && radio::SaveFavorites(radioDir + L"\\stations.json", {station, frequencyStation}),
               L"saved stations for UI")) return;
    g_lib = Library();
    g.playlists = {{L"Library", {}, PlaylistKind::Library}, {L"Test playlist", {}},
                   {L"Test auto-playlist", {}, PlaylistKind::Auto}};
    g.active = 0;
    g.playing = kNone;
    for (int i = 0; i < 3; ++i) {
        TrackId id = g_lib.Add(L"C:\\ui-test\\track" + std::to_wstring(i) + L".flac");
        TagData tags;
        tags.title = "Track " + std::to_string(i);
        tags.artist = "Test artist";
        tags.album = "Test album";
        g_lib.ApplyTags(id, tags);
        g_lib[id].flags |= Track::kInLibrary;
    }
    g_lib.Touch();
    WNDCLASSW wc{};
    wc.hInstance = g.inst;
    wc.lpszClassName = L"CordinalUiSelfTest";
    wc.lpfnWndProc = [](HWND h, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
        if (msg == WM_GETMINMAXINFO) {
            // Hidden DPI checks need their requested geometry even on a small CI desktop.
            auto* limits = reinterpret_cast<MINMAXINFO*>(lp);
            limits->ptMaxTrackSize = {S(kUiWidth), S(kUiHeight)};
            return 0;
        }
        if (msg == WM_APP_IMPORT) { OnImportResult(lp); return 0; }
        if (msg == WM_APP_ART) { OnArtResult(lp); return 0; }
        if (msg == WM_APP_META) { AcceptTagBatch(reinterpret_cast<std::vector<TagResult>*>(lp)); return 0; }
        if (msg == WM_APP_RG) { FinishReplayGain(wp, lp); return 0; }
        if (msg == WM_APP_HELPER) { OnHelperMessage(wp, lp); return 0; }
        if (msg == WM_TIMER && wp == kSearchTimer) { FlushSearch(); return 0; }
        if (msg == WM_COMMAND) { OnCommand(LOWORD(wp), HIWORD(wp), (HWND)lp); return 0; }
        if (msg == WM_NOTIFY && ((NMHDR*)lp)->hwndFrom == g.list) return OnListNotify((NMHDR*)lp);
        return DefWindowProcW(h, msg, wp, lp);
    };
    RegisterClassW(&wc);
    g.hwnd = CreateWindowW(wc.lpszClassName, L"UI self-test", WS_OVERLAPPEDWINDOW, 0, 0, kUiWidth, kUiHeight,
                           nullptr, nullptr, g.inst, nullptr);
    Check(g.hwnd != nullptr, L"UI: hidden test window created");
    if (!g.hwnd) return;
    CreateFonts();
    CreateControls(g.hwnd);
    g.pointsChip = CreateWindowW(WC_STATICW, L"", WS_CHILD, 0, 0, 0, 0, g.hwnd, nullptr, g.inst, nullptr);
    ApplyFonts();
    RefreshTabs();
    Layout();
    {
        SetWindowTextW(g.search, L"Track 1");
        FlushSearch();
        ActivateRadio();
        bool shown = RadioActive() && (GetWindowLongPtrW(RadioView(), GWL_STYLE) & WS_VISIBLE) &&
                     !(GetWindowLongPtrW(g.list, GWL_STYLE) & WS_VISIBLE);
        SetWindowTextW(g.search, L"indie");
        bool separate = g.filter == L"Track 1";
        ActivatePlaylist(0);
        Check(shown && separate && !RadioActive() && WindowText(g.search) == L"Track 1",
              L"UI: Radio has its own view and search without changing the library search");
        SetWindowTextW(g.search, L""); FlushSearch();
    }
    {
        ActivateRadio();
        SetWindowTextW(g.search, L"");
        TogglePause();
        bool played = RadioPlaying() && RadioStationName() == L"Test rock station";
        bool wave = WaveformShown() && !IsWindowEnabled(g.seek);
        RECT radioTab{}; GetClientRect(RadioTab(), &radioTab);
        LPARAM close = MAKELPARAM(radioTab.right - S(12), radioTab.bottom / 2);
        SendMessageW(RadioTab(), WM_LBUTTONDOWN, MK_LBUTTON, close);
        SendMessageW(RadioTab(), WM_LBUTTONUP, 0, close);
        bool closed = !RadioActive() && !RadioTabOpen() &&
            !(GetWindowLongPtrW(RadioTab(), GWL_STYLE) & WS_VISIBLE) && RadioPlaying();
        OnCommand(IDM_OPEN_RADIO, 0, nullptr);
        Check(wave && closed && RadioActive() && RadioTabOpen() && RadioPlaying(),
              L"UI: Radio's close box hides its tab without interrupting live playback, and View reopens it");
        size_t playlists = g.playlists.size();
        OnCommand(IDM_REMOVE_PL, 0, nullptr);
        bool shortcut = !RadioActive() && !RadioTabOpen() && g.playlists.size() == playlists;
        ActivateRadio();
        Check(shortcut && RadioActive() && RadioTabOpen(),
              L"UI: Ctrl+W closes Radio without deleting the playlist underneath it");
        Stop();
        TogglePause();
        Check(played && RadioPlaying() && RadioStationName() == L"Test rock station",
              L"UI: Play starts the selected station and retries it after Stop");
        Stop();
        ActivatePlaylist(0);
    }
    {
        ActivateRadio();
        SetWindowTextW(g.search, L"98.5 FM");
        bool frequencyFound = RadioCount() == 1;
        SendMessageW(RadioView(), WM_KEYDOWN, 'F', 0);
        std::vector<radio::Station> saved;
        Check(frequencyFound && RadioCount() == 0 &&
                  radio::LoadFavorites(radioDir + L"\\stations.json", saved) && saved.size() == 1 &&
                  saved[0].name == "Test rock station",
              L"UI: F removes a frequency-filtered favorite from the grid and disk");
        SetWindowTextW(g.search, L"");
        ActivatePlaylist(0);
    }
    {
        // Hold the UI queue while changing songs: only one lyric result should be outstanding,
        // then the worker must load the latest request when that result is handled.
        const bool wasShown = LyricsPanelShown();
        std::wstring oldPaths[3];
        bool lyricsFiles = true;
        for (TrackId id = 0; id < 3; ++id) {
            oldPaths[id] = g_lib.Path(id);
            const auto name = scratch + L"\\lyric-switch-" + std::to_wstring(id);
            g_lib.Rename(id, name + L".flac");
            lyricsFiles &= WriteTestBytes(name + L".lrc", id == 2 ? "Latest lyric" : "Previous lyric");
        }
        if (!wasShown) ToggleLyricsPanel();
        for (int i = 0; i < 200; ++i) {
            g.playing = (TrackId)(i % 2);
            LyricsTrackChanged();
        }
        g.playing = 2;
        LyricsTrackChanged();
        HWND lyrics = GetDlgItem(g.hwnd, IDC_LYRICS);
        MSG message;
        ULONGLONG until = GetTickCount64() + 5000;
        while (!PeekMessageW(&message, lyrics, WM_APP + 1, WM_APP + 1, PM_NOREMOVE) && GetTickCount64() < until) Sleep(1);
        std::vector<MSG> results;
        while (PeekMessageW(&message, lyrics, WM_APP + 1, WM_APP + 1, PM_REMOVE)) results.push_back(message);
        for (const MSG& result : results) DispatchMessageW(&result);
        until = GetTickCount64() + 5000;
        while (!LyricsReadyFor(2, L"Latest lyric") && GetTickCount64() < until) {
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
            Sleep(1);
        }
        Check(lyricsFiles && results.size() == 1 && LyricsReadyFor(2, L"Latest lyric"),
              L"UI: rapid song changes keep one lyric read in flight and load the newest lyrics");
        g.playing = kNone;
        LyricsTrackChanged();
        if (!wasShown) ToggleLyricsPanel();
        for (TrackId id = 0; id < 3; ++id) {
            g_lib.Rename(id, oldPaths[id]);
            DeleteFileW((scratch + L"\\lyric-switch-" + std::to_wstring(id) + L".lrc").c_str());
        }
    }
    BYTE savedKeys[256]{}, editKeys[256]{};
    GetKeyboardState(savedKeys);
    editKeys[VK_CONTROL] = 0x80;
    SetKeyboardState(editKeys);
    testlog::Cases editorKeys;
    for (WPARAM key : {WPARAM('Z'), WPARAM(VK_LEFT), WPARAM(VK_RIGHT), WPARAM(VK_RETURN)}) {
        MSG msg{};
        msg.hwnd = g.search;
        msg.message = WM_KEYDOWN;
        msg.wParam = key;
        editorKeys.Add(!TranslateMainKey(&msg), L"Ctrl+" + std::to_wstring((unsigned)key));
    }
    Check(editorKeys.Passed(), L"UI: Ctrl+Z, Ctrl+Left/Right and Ctrl+Enter stay in the search editor%ls",
          editorKeys.failed.c_str());
    SetWindowTextW(g.search, L"test query"); FlushSearch();
    SendMessageW(g.search, WM_KEYDOWN, 'A', 0);
    DWORD start = 0, end = 0;
    SendMessageW(g.search, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
    Check(start == 0 && end == 10, L"UI: Ctrl+A selects the entire search query");
    BeginRename(1);
    SendMessageW(g.rename, EM_SETSEL, 2, 2);
    SendMessageW(g.rename, WM_KEYDOWN, 'A', 0);
    SendMessageW(g.rename, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
    Check(start == 0 && end == 13, L"UI: Ctrl+A selects the entire playlist name");
    EndRename(false);
    SetKeyboardState(savedKeys);
    {
        RECT tab{};
        bool hasTab = TabCtrl_GetItemRect(g.tabs, 1, &tab) != FALSE;
        LPARAM point = MAKELPARAM(tab.left + S(8), (tab.top + tab.bottom) / 2);
        SendMessageW(g.tabs, WM_KEYDOWN, VK_HOME, 0);
        bool keyboardFocus = !(SendMessageW(g.tabs, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS);
        SendMessageW(g.tabs, WM_LBUTTONDOWN, MK_LBUTTON, point);
        SendMessageW(g.tabs, WM_LBUTTONUP, 0, point);
        bool mouseFocus = (SendMessageW(g.tabs, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) != 0;
        Check(hasTab && keyboardFocus && mouseFocus && !g.rename && TabCtrl_GetCurSel(g.tabs) == 1,
              L"UI: clicking a tab switches it without a focus box; keyboard focus remains visible");

        SendMessageW(g.tabs, WM_LBUTTONDBLCLK, MK_LBUTTON, point);
        SendMessageW(g.tabs, WM_LBUTTONUP, 0, point);
        bool borderless = g.rename && !(GetWindowLongPtrW(g.rename, GWL_EXSTYLE) & WS_EX_CLIENTEDGE) &&
                          !(GetWindowLongPtrW(g.rename, GWL_STYLE) & WS_BORDER);
        DWORD first = 0, last = 0;
        SendMessageW(g.rename, EM_GETSEL, (WPARAM)&first, (LPARAM)&last);
        HDC dc = GetDC(g.hwnd);
        LRESULT brush = WndProc(g.hwnd, WM_CTLCOLOREDIT, (WPARAM)dc, (LPARAM)g.rename);
        bool surface = dc && brush == (LRESULT)g.surfaceBrush && GetBkColor(dc) == SurfaceColor();
        ThemeMode savedTheme = g.themeMode;
        ViewSize savedSize = g.size;
        const Palette* savedPalette = g.pal;
        g.themeMode = ThemeMode::Foobar;
        g.size = ViewSize::Normal;
        g.pal = &ThemePalette(ThemeMode::Foobar);
        brush = WndProc(g.hwnd, WM_CTLCOLOREDIT, (WPARAM)dc, (LPARAM)g.rename);
        bool foobarSurface = dc && brush == (LRESULT)GetStockObject(DC_BRUSH) && GetBkColor(dc) == Ref(P().border);
        g.themeMode = savedTheme;
        g.size = savedSize;
        g.pal = savedPalette;
        if (dc) ReleaseDC(g.hwnd, dc);
        Check(borderless && surface && foobarSurface && first == 0 && last == 13,
              L"UI: double-click selects the name in a borderless editor matching the tab surface");
        if (g.rename) {
            SetWindowTextW(g.rename, L"Renamed playlist");
            SendMessageW(g.rename, WM_KEYDOWN, VK_RETURN, 0);
        }
        Check(!g.rename && g.playlists[1].name == L"Renamed playlist",
              L"UI: Enter still commits the inline playlist rename");
        g.playlists[1].name = L"Test playlist";
        RefreshTabs();
    }
    SetWindowTextW(g.search, L""); FlushSearch();
    auto rect = [](HWND h) {
        RECT r{};
        GetWindowRect(h, &r);
        MapWindowPoints(nullptr, g.hwnd, (POINT*)&r, 2);
        return r;
    };
    testlog::Cases reachable;
    for (UINT dpi : {96u, 144u, 192u}) {
        g.dpi = dpi;
        CreateFonts();
        ApplyFonts();
        ThemeBrowser();
        for (int width : {640, kUiWidth}) {
            SetWindowPos(g.hwnd, nullptr, 0, 0, S(width), S(kUiHeight), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            RECT sized{};
            GetWindowRect(g.hwnd, &sized);
            Layout();
            RECT tabs = rect(g.tabs), views = rect(g.viewButtons[0]), search = rect(g.search);
            RECT toggle = rect(g.viewButtons[kViewButtons - 1]), last = rect(g.viewButtons[kViewButtons - 2]);
            // The hidden test window must really have the size asked for, or the layout says nothing.
            reachable.Add(sized.right - sized.left == S(width) && sized.bottom - sized.top == S(kUiHeight) &&
                              tabs.right - tabs.left >= S(200) && tabs.right <= search.left && views.top >= tabs.bottom &&
                              last.right <= toggle.left,
                          std::to_wstring(width) + L" px at " + std::to_wstring(dpi) + L" DPI");
        }
    }
    Check(reachable.Passed(), L"UI: tabs, search and views remain reachable at 640/1163 px and 96/144/192 DPI%ls",
          reachable.failed.c_str());
    // Customized layouts (app_custom.cpp): wherever the player docks and however tall its waveform,
    // the list keeps its room, nothing of the player lies over it, and a side dock turns the
    // waveform to run down its column.
    std::wstring layoutBad;
    MainLayout savedLayout = g.layout;
    auto shown = [](HWND h) { return (GetWindowLongPtrW(h, GWL_STYLE) & WS_VISIBLE) != 0; };  // the test window itself is hidden
    for (int dock = 0; dock < (int)Dock::Count; ++dock)
        for (int wave = 0; wave < 3; ++wave) {
            g.layout = MainLayout{};
            g.layout.player = (Dock)dock;
            g.layout.waveSize = (uint8_t)wave;
            g.layout.waveOwnRow = wave == 2;
            g.layout.upNext = wave == 1;
            Layout();
            RECT client{}, overlap{};
            GetClientRect(g.hwnd, &client);
            RECT list = rect(g.list), seek = rect(g.seek), play = rect(g.buttons[1]), volume = rect(g.volume);
            bool vertical = dock >= (int)Dock::Left;
            bool ok = !IntersectRect(&overlap, &list, &seek) && !IntersectRect(&overlap, &list, &play) &&
                      !IntersectRect(&overlap, &list, &volume) && !IntersectRect(&overlap, &seek, &play) &&
                      seek.left >= 0 && seek.top >= 0 && seek.right <= client.right && seek.bottom <= client.bottom &&
                      list.right - list.left >= S(200) && SeekVertical() == vertical &&
                      (vertical ? seek.bottom - seek.top >= S(80) : seek.right - seek.left > seek.bottom - seek.top);
            if (!ok) layoutBad += L" " + std::to_wstring(dock) + L"/" + std::to_wstring(wave);
        }
    Check(layoutBad.empty(), L"UI: the player docks at every edge in every waveform size%s", layoutBad.c_str());
    {
        const int savedBandSize = g.bandWaveSize;
        const UINT savedDpi = g.dpi;
        const ViewSize savedSize = g.size;
        testlog::Cases sizing;
        for (UINT dpi : {96u, 192u}) {
            g.dpi = dpi; CreateFonts(); ApplyFonts();
            SetWindowPos(g.hwnd, nullptr, 0, 0, S(kUiWidth), S(kUiHeight), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            for (ViewSize mode : {ViewSize::Normal, ViewSize::Compact, ViewSize::Spotlight, ViewSize::Big})
                for (Dock dock : {Dock::Top, Dock::Bottom, Dock::Left, Dock::Right})
                    for (bool ownRow : {false, true}) {
                        g.size = mode; g.layout = MainLayout{}; g.layout.player = dock; g.layout.waveOwnRow = ownRow;
                        g.bandWaveSize = 0; Layout();
                        RECT before = rect(g.seek), play = rect(g.buttons[1]), volume = rect(g.volume);
                        g.bandWaveSize = 3; Layout();
                        RECT after = rect(g.seek), playAfter = rect(g.buttons[1]), volumeAfter = rect(g.volume);
                        bool target = mode == ViewSize::Normal && (dock == Dock::Top || dock == Dock::Bottom);
                        bool same = EqualRect(&before, &after) != 0;
                        bool controls = play.bottom - play.top == playAfter.bottom - playAfter.top &&
                            volume.bottom - volume.top == volumeAfter.bottom - volumeAfter.top;
                        sizing.Add(before.bottom > before.top && controls &&
                                       (target ? after.bottom - after.top == S(72) : same),
                                   std::to_wstring(dpi) + L"/" + std::to_wstring((int)mode) + L"/" +
                                       std::to_wstring((int)dock) + L"/" + std::to_wstring(ownRow));
                    }
        }
        Check(sizing.Passed(), L"UI: waveform height affects only normal top/bottom bars at 100%% and 200%% DPI%ls",
              sizing.failed.c_str());
        g.bandWaveSize = savedBandSize; g.size = savedSize; g.dpi = savedDpi;
        CreateFonts(); ApplyFonts();
        g.layout = savedLayout;
        SetWindowPos(g.hwnd, nullptr, 0, 0, S(kUiWidth), S(kUiHeight), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
    }
    g.layout = MainLayout{};
    g.layout.player = Dock::Right;
    Layout();
    RECT seekClient{};
    GetClientRect(g.seek, &seekClient);
    Check(BarFracAt(g.seek, {seekClient.right / 2, 0}) == 0.0 && BarFracAt(g.seek, {seekClient.right / 2, seekClient.bottom}) == 1.0 &&
              std::fabs(BarFracAt(g.seek, {0, seekClient.bottom / 2}) - 0.5) < 0.02,
          L"UI: a vertical waveform seeks from its top to its bottom");
    g.layout = MainLayout{};
    Layout();
    HWND body = CanvasActive() ? g.libView : g.list;  // whichever view the Library shows
    LONG classicTop = rect(body).top;
    g.layout.tabs = g.layout.search = g.layout.status = g.layout.transport = g.layout.nowPlaying = false;
    Layout();
    Check(!shown(g.tabs) && !shown(g.search) && !shown(g.status) && !shown(g.buttons[1]) && !shown(g.npTitle) &&
              shown(g.buttons[3]) && shown(g.seek) && shown(body) && rect(body).top < classicTop - S(40),
          L"UI: hidden layout parts hide and the list takes their room");
    g.layout = savedLayout;
    Layout();
    std::wstring emptyBad, escapeBad, downBad, selectBad;
    for (LibraryMode mode : {LibraryMode::Tracks, LibraryMode::Albums, LibraryMode::AlbumTracks, LibraryMode::Artists}) {
        auto note = [&](std::wstring& list, bool ok) { if (!ok) list += L" " + std::to_wstring((int)mode); };
        SetLibraryMode(mode);
        SetWindowTextW(g.search, L"no-such-track"); FlushSearch();
        note(emptyBad, g.view.empty() && EmptyViewText().find(L"search") != std::wstring::npos);
        SendMessageW(g.search, WM_KEYDOWN, VK_ESCAPE, 0);
        note(escapeBad, g.filter.empty() && g.view.size() == 3);
        SendMessageW(g.search, WM_KEYDOWN, VK_DOWN, 0);
        note(downBad, !SelectedTracks().empty());
        if (CanvasActive()) {
            OnCommand(IDM_SELECT_ALL, 0, nullptr);
            note(selectBad, SelectedTracks().size() == 3);
        }
    }
    Check(emptyBad.empty(), L"UI: search explains no results in every library view%s", emptyBad.c_str());
    Check(escapeBad.empty(), L"UI: Escape restores search results in every library view%s", escapeBad.c_str());
    Check(downBad.empty(), L"UI: Down from search selects the visible view%s", downBad.c_str());
    Check(selectBad.empty(), L"UI: Select all menu works in every canvas view%s", selectBad.c_str());
    // Albums & Tracks: the cover is the grid's 160 a quarter larger and the rows start under a 56 px
    // heading, so clicking the third row's middle picks that one track (the old layout had no row there).
    SetLibraryMode(LibraryMode::AlbumTracks);
    if (CanvasActive() && g.view.size() == 3) {
        LPARAM third = MAKELPARAM(S(18 + 200 + 20 + 60), S(8 + 56 + 2 * 26 + 13));
        SendMessageW(g.libView, WM_LBUTTONDOWN, MK_LBUTTON, third);
        SendMessageW(g.libView, WM_LBUTTONUP, 0, third);
        auto picked = SelectedTracks();
        Check(picked.size() == 1 && picked[0] == Active().items[g.view[2]],
              L"UI: Albums & Tracks rows sit beside the larger cover, under the album heading");
    } else {
        Check(false, L"UI: Albums & Tracks rows sit beside the larger cover, under the album heading");
    }
    // Album group headers: the three tracks of "Test album" sit under one two-row header.
    ActivatePlaylist(0);
    SetLibraryMode(LibraryMode::Tracks);
    SetWindowTextW(g.search, L""); FlushSearch();
    Check(g.groups.size() == 1 && RowCount() == 5 && RowTrack(0) == kNone && RowTrack(1) == kNone && RowTrack(2) != kNone &&
              ViewPosOfRow(4) == 2 && RowOfViewPos(0) == 2 && ViewPosNear(0) == 0,
          L"UI: an album's tracks sit under a two-row header");
    SelectGroup(0, false);
    Check(SelectedTracks().size() == 3, L"UI: clicking an album header selects its tracks");
    Check(GroupHidesCell(0, 4, RowTrack(2)) && GroupHidesCell(0, 3, RowTrack(2)) && !GroupHidesCell(0, 2, RowTrack(2)),
          L"UI: grouped tracks leave the album and its artist to the header");
    g.groupAlbums = false;
    RebuildView();
    Check(g.groups.empty() && RowCount() == 3 && RowTrack(0) != kNone, L"UI: grouping by album can be turned off");
    g.groupAlbums = true;
    RebuildView();
    {
        // Owner-drawn tracks fill every column. Exercise the actual native mouse messages,
        // then cover a notification that omits its item index explicitly.
        g.playlists[1].items = {0, 1, 2};
        ActivatePlaylist(1);
        auto doubleClick = [](POINT pt) {
            LPARAM at = MAKELPARAM(pt.x, pt.y);
            PostMessageW(g.list, WM_LBUTTONDOWN, MK_LBUTTON, at);
            PostMessageW(g.list, WM_LBUTTONUP, 0, at);
            PostMessageW(g.list, WM_LBUTTONDBLCLK, MK_LBUTTON, at);
            PostMessageW(g.list, WM_LBUTTONUP, 0, at);
            Pump(10);
        };
        testlog::Cases columns, blank;
        for (bool grouped : {false, true}) {
            g.groupAlbums = grouped;
            RebuildView();
            for (int sub = 0; sub < (int)g.columns.size(); ++sub) {
                int pos = sub % 3, row = RowOfViewPos(pos);
                RECT cell{};
                ListView_GetSubItemRect(g.list, row, sub, LVIR_BOUNDS, &cell);
                POINT pt{cell.left + S(4), (cell.top + cell.bottom) / 2};
                auto before = g.nextToken;
                doubleClick(pt);
                auto found = g.tokens.find(g.decodingToken);
                columns.Add(g.nextToken == before + 1 && found != g.tokens.end() && found->second.pl == 1 &&
                                found->second.index == (uint32_t)pos,
                            L"column " + std::to_wstring(sub) + (grouped ? L" grouped" : L""));
            }
            RECT last{}, client{};
            ListView_GetItemRect(g.list, RowCount() - 1, &last, LVIR_BOUNDS);
            GetClientRect(g.list, &client);
            auto before = g.nextToken;
            doubleClick({S(100), last.bottom + S(10)});
            doubleClick({client.right - 1, (last.top + last.bottom) / 2});
            blank.Add(g.nextToken == before, grouped ? L"grouped" : L"ungrouped");
        }
        Check(columns.Passed(), L"UI: double-click starts the clicked track once in every column, grouped or not%ls",
              columns.failed.c_str());
        Check(blank.Passed(), L"UI: double-clicking blank list space leaves playback alone%ls", blank.failed.c_str());
        RECT title{};
        ListView_GetSubItemRect(g.list, RowOfViewPos(2), SubitemOf(2), LVIR_BOUNDS, &title);
        NMITEMACTIVATE activation{};
        activation.hdr.hwndFrom = g.list;
        activation.hdr.code = NM_DBLCLK;
        activation.iItem = -1;
        activation.ptAction = {title.left + S(4), (title.top + title.bottom) / 2};
        auto before = g.nextToken;
        OnListNotify(&activation.hdr);
        Check(g.nextToken == before + 1 && TokenIndex(g.decodingToken, 1) == 2,
              L"UI: a double-click without a native item index still plays the clicked track");
        g.engine.Stop();
        g.tokens.clear();
        g.decodingToken = 0;
        g.playlists[1].items.clear();
        ActivatePlaylist(0);
    }
    // The reference layout the Linux port is held to (ui_spec.h): the real controls at 96 DPI.
    {
        g.dpi = 96;
        CreateFonts();
        ApplyFonts();
        g.layout = MainLayout{};
        g.browserVisible = false;
        RECT frame{0, 0, ui::kLayoutWidth, ui::kLayoutHeight};
        AdjustWindowRectExForDpi(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0, 96);
        SetWindowPos(g.hwnd, nullptr, 0, 0, frame.right - frame.left, frame.bottom - frame.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
        auto spec = [&](const char* name) -> RECT {
            static const struct { const char* name; HWND* h; } controls[] = {
                {"previous", &g.buttons[0]}, {"play", &g.buttons[1]}, {"next", &g.buttons[2]}, {"shuffle", &g.buttons[3]},
                {"repeat", &g.buttons[4]}, {"resume", &g.buttons[5]}, {"elapsed", &g.elapsed}, {"seek", &g.seek},
                {"remaining", &g.time}, {"speaker", &g.speaker}, {"volume", &g.volume}, {"title", &g.npTitle},
                {"subtitle", &g.npSub}, {"tabs", &g.tabs}, {"search", &g.search}, {"list", &g.list}, {"status", &g.status}};
            if (!strcmp(name, "cover")) return g.artRect;
            if (!strcmp(name, "radio_tab")) return rect(RadioTab());
            for (const auto& c : controls)
                if (!strcmp(name, c.name)) return rect(*c.h);
            return {};
        };
        std::wstring off;
        RECT client{};
        GetClientRect(g.hwnd, &client);
        if (client.right != ui::kLayoutWidth || client.bottom != ui::kLayoutHeight) off += L" client";
        for (const ui::LayoutRect& want : ui::kLayout) {
            RECT r = spec(want.name);
            if (r.left != want.x || r.top != want.y || r.right - r.left != want.w || r.bottom - r.top != want.h) {
                wchar_t buf[96];
                swprintf_s(buf, L" %hs=%d,%d,%d,%d", want.name, (int)r.left, (int)r.top, (int)(r.right - r.left), (int)(r.bottom - r.top));
                off += buf;
            }
        }
        Check(off.empty(), L"UI: the window matches the layout the Linux port follows (ui_spec.h kLayout)%s", off.c_str());
        g.layout = savedLayout;
        Layout();
    }
    if (!kExtraThemes) {
        testlog::Skip(L"UI: Radio in the alternate themes: this build leaves them out");
    } else {
        ThemeMode original = g.themeMode;
        testlog::Cases layouts;
        for (ThemeMode theme : {ThemeMode::Foobar, ThemeMode::Apollo}) {
            OnCommand(IDM_THEME_BASE + (int)theme, 0, nullptr);
            ActivateRadio();
            bool alternateRadioVisible = RadioActive() && (GetWindowLongPtrW(RadioView(), GWL_STYLE) & WS_VISIBLE) &&
                                         (GetWindowLongPtrW(RadioTab(), GWL_STYLE) & WS_VISIBLE);
            ActivatePlaylist(0);
            layouts.Add(alternateRadioVisible && (theme == ThemeMode::Foobar ? FoobarMode() : ApolloMode()),
                        theme == ThemeMode::Foobar ? L"foobar2000" : L"Apollo");
        }
        OnCommand(IDM_THEME_BASE + (int)original, 0, nullptr);
        Check(layouts.Passed(), L"UI: Radio opens from either alternate theme and restores its layout%ls",
              layouts.failed.c_str());
    }
    TestFolderBrowser();
    TestFoobarControls();
    wchar_t cue[128] = {};
    SendMessageW(g.search, EM_GETCUEBANNER, (WPARAM)cue, (LPARAM)std::size(cue));
    Check(wcscmp(cue, L"Search library (Ctrl+F)") == 0, L"UI: the search box names the Library");
    ActivatePlaylist(2);
    SendMessageW(g.search, EM_GETCUEBANNER, (WPARAM)cue, (LPARAM)std::size(cue));
    Check(wcscmp(cue, L"Search Test auto-playlist (Ctrl+F)") == 0, L"UI: the search box names the playlist it searches");
    g.timeRemaining = true;
    ShowPlaybackTime(72, 199);
    wchar_t left[32] = {}, right[32] = {};
    GetWindowTextW(g.elapsed, left, 32);
    GetWindowTextW(g.time, right, 32);
    Check(wcscmp(left, L"1:12") == 0 && wcscmp(right, L"-2:07") == 0, L"UI: the seek bar shows elapsed and remaining time");
    OnCommand(IDC_TIME, STN_CLICKED, g.time);
    ShowPlaybackTime(72, 199);
    GetWindowTextW(g.time, right, 32);
    Check(!g.timeRemaining && wcscmp(right, L"3:19") == 0, L"UI: clicking the time shows the length instead");
    g.timeRemaining = true;
    ActivatePlaylist(1);
    HMENU menu = BuildMenu();
    WndProc(g.hwnd, WM_INITMENUPOPUP, (WPARAM)GetSubMenu(menu, 1), 0);
    Check((GetMenuState(GetSubMenu(menu, 1), IDM_CLEAR, MF_BYCOMMAND) & MF_GRAYED) != 0,
          L"UI: empty playlist cannot be cleared");
    ActivatePlaylist(0);
    WndProc(g.hwnd, WM_INITMENUPOPUP, (WPARAM)GetSubMenu(menu, 3), 0);
    Check((GetMenuState(GetSubMenu(menu, 3), IDM_RENAME_PL, MF_BYCOMMAND) & MF_GRAYED) != 0 &&
              (GetMenuState(GetSubMenu(menu, 3), IDM_REMOVE_PL, MF_BYCOMMAND) & MF_GRAYED) != 0,
          L"UI: Library cannot be renamed or removed");
    DestroyMenu(menu);
    ActivatePlaylist(1);
    NMLVEMPTYMARKUP empty{};
    empty.hdr.hwndFrom = g.list;
    empty.hdr.code = LVN_GETEMPTYMARKUP;
    Check(OnListNotify(&empty.hdr) == TRUE && std::wstring(empty.szMarkup).find(L"Ctrl+O") != std::wstring::npos,
          L"UI: empty playlist explains how to add music");
    ActivatePlaylist(2);
    Check(EmptyViewText().find(L"rules") != std::wstring::npos, L"UI: empty auto-playlist explains its rules");

    // Keep a long, sorted playlist in place while visiting each Library view.
    g.playlists[1].items = {0, 1, 2};
    for (int i = 3; i < 70; ++i) {
        TrackId id = g_lib.Add(L"C:\\ui-test\\track" + std::to_wstring(i) + L".flac");
        TagData tags;
        tags.title = "Track " + std::to_string(i);
        g_lib.ApplyTags(id, tags);
        g.playlists[1].items.push_back(id);
    }
    ActivatePlaylist(1);
    SetWindowTextW(g.search, L"Track"); FlushSearch();
    SortByColumn(SubitemOf(2));
    SortByColumn(SubitemOf(2));
    ListView_SetItemState(g.list, 35, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(g.list, 35, FALSE);
    const auto remembered = SelectedTracks();
    int top = ListView_GetTopIndex(g.list);
    Check(top > 0, L"UI: navigation test starts with a scrolled playlist");
    std::wstring restoreBad, rememberBad, refreshBad;
    for (LibraryMode mode : {LibraryMode::Tracks, LibraryMode::Albums, LibraryMode::AlbumTracks, LibraryMode::Artists}) {
        auto note = [&](std::wstring& list, bool ok) { if (!ok) list += L" " + std::to_wstring((int)mode); };
        ActivatePlaylist(0);
        SetLibraryMode(mode);
        SetWindowTextW(g.search, L"Track"); FlushSearch();
        SendMessageW(g.search, WM_KEYDOWN, VK_DOWN, 0);
        const auto librarySelection = SelectedTracks();
        ActivatePlaylist(1);
        note(restoreBad, g.filter == L"Track" && SelectedTracks() == remembered && ListView_GetTopIndex(g.list) == top &&
                         g.sortColumn == 2 && !g.sortAscending);
        ActivatePlaylist(0);
        note(rememberBad, g.filter == L"Track" && SelectedTracks() == librarySelection);
        g_lib.Touch();
        RefreshViewKeepingSelection();
        note(refreshBad, SelectedTracks() == librarySelection);
        ActivatePlaylist(1);
    }
    Check(restoreBad.empty(), L"UI: switching from every library view restores playlist search, selection, scroll and sort%s",
          restoreBad.c_str());
    Check(rememberBad.empty(), L"UI: every library view remembers its search and selection%s", rememberBad.c_str());
    Check(refreshBad.empty(), L"UI: refresh keeps selection in every library view%s", refreshBad.c_str());
    SetWindowTextW(g.search, L"no-such-track"); FlushSearch();
    ActivatePlaylist(0);
    g.playing = remembered.front();
    g.playingPl = 1;
    OnCommand(IDC_NP_TITLE, STN_CLICKED, g.npTitle);
    Check(g.active == 1 && g.filter.empty() && SelectedTracks() == remembered,
          L"UI: clicking the playing title reveals and selects the track through a search filter");
    g.playing = kNone;
    g.playingPl = -1;
    g.resumeTrack = kNone;
    OnCommand(IDC_NP_TITLE, STN_CLICKED, g.npTitle);
    Check(g.active == 1 && SelectedTracks() == remembered, L"UI: clicking the title while stopped does nothing");
    g.resumeTrack = remembered.front();
    g.resumePos = 72;
    UpdateNowPlaying();
    wchar_t npText[256] = {};
    GetWindowTextW(g.npTitle, npText, (int)std::size(npText));
    Check(std::wstring(npText).rfind(L"Continue: ", 0) == 0, L"UI: the stopped strip offers to continue the stopped track");
    GetWindowTextW(g.npSub, npText, (int)std::size(npText));
    Check(std::wstring(npText).find(L"1:12") != std::wstring::npos, L"UI: the stopped strip shows where playback stopped");
    g.resumeTrack = kNone;
    UpdateNowPlaying();

    // The waveform's section menu (app_region.cpp): a section is marked where the cursor is, kept
    // in settings.ini against the file's audio, and loaded back for the track that plays next.
    g.playing = remembered.front();
    g.playingPl = 1;
    g_lib[g.playing].durationMs = 180000;  // 3:00, so the section below has a track to sit in
    RegionTrackChanged();
    double secDur = CurrentDuration();
    bool noSectionYet = !CurrentRegion().on && CurrentRegion().end == 0;
    bool started = CommandRegion(IDM_REGION_START, 12.5);
    bool ended = CommandRegion(IDM_REGION_END, 40.0);
    bool armed = CommandRegion(IDM_REGION_LOOP, 20.0);
    Region loaded = CurrentRegion();
    Check(noSectionYet && started && ended && armed && loaded.on && loaded.start == 12.5 && loaded.end == 40.0,
          L"UI: a section marked on the waveform is 12.5 s to 40 s and loops");
    PlayItem marked = MakeItem(123, g.playing);
    TrackId upcoming = g_lib.Add(scratch + L"\\section-upcoming.wav");
    MakeItem(124, upcoming);
    Check(marked.start == 0 && marked.end == 0 && marked.loopStart == 12.5 && marked.loopEnd == 40 &&
              marked.duration == 180 && CurrentRegion().on && CurrentRegion().start == 12.5,
          L"UI: playback sections keep the full timeline and prefetch leaves the playing section intact");
    // The section was stored when it was marked: dropping the copy and asking for the track again
    // reads it back from settings.ini, which is what a restart does.
    g.playing = kNone;
    RegionTrackChanged();
    g.playing = remembered.front();
    RegionTrackChanged();
    Region again = CurrentRegion();
    // A message that isn't a right-click on the seek bar is passed on, never swallowed.
    HWND realSeek = g.seek;
    g.seek = nullptr;
    bool notSwallowed = !RegionMouse(MSG{});
    g.seek = realSeek;
    Check(notSwallowed && again.on && again.start == 12.5 && again.end == 40.0,
          L"UI: the section is kept for the file and comes back with the track");
    // The section follows the same x -> time mapping the seek bar seeks by, so it lands under the
    // cursor rather than beside it.
    RECT secThumb{}, secChannel{};
    SendMessageW(g.seek, TBM_GETTHUMBRECT, 0, (LPARAM)&secThumb);
    SendMessageW(g.seek, TBM_GETCHANNELRECT, 0, (LPARAM)&secChannel);
    int secMid = secChannel.left + (secThumb.right - secThumb.left) / 2 +
                 (int)((secChannel.right - secChannel.left - (secThumb.right - secThumb.left)) / 2);
    Check(std::fabs(RegionTimeAt(g.seek, {secMid, 0}) - secDur / 2) < secDur * 0.02,
          L"UI: a mark lands where the seek bar would have seeked to");
    bool cleared = CommandRegion(IDM_REGION_CLEAR, 0) && !CurrentRegion().on && CurrentRegion().end == 0;
    g.playing = kNone;
    RegionTrackChanged();
    g.playing = remembered.front();
    RegionTrackChanged();
    Check(cleared && !CurrentRegion().on && CurrentRegion().end == 0,
          L"UI: clearing a section drops it from the file's settings");
    CommandRegion(IDM_REGION_START, 8);
    CommandRegion(IDM_REGION_END, 18);
    g.playing = kNone;
    RegionTrackChanged();
    g.playing = remembered.front();
    RegionTrackChanged();
    Check(!CurrentRegion().on && CurrentRegion().start == 8 && CurrentRegion().end == 18,
          L"UI: unfinished section edits survive a track change without enabling the loop");
    CommandRegion(IDM_REGION_CLEAR, 0);
    std::wstring cueAudio = scratch + L"\\section-cue.wav", cueSheet = scratch + L"\\section-cue.cue";
    bool cueReady = WriteTestBytes(cueAudio, MakeSineWav(4, 1)) &&
        WriteTestBytes(cueSheet, "FILE \"section-cue.wav\" WAVE\nTRACK 01 AUDIO\nINDEX 01 00:00:00\nTRACK 02 AUDIO\nINDEX 01 00:02:00\nTRACK 03 AUDIO\nINDEX 01 00:03:00\n");
    TrackId cueId = g_lib.Add(SubtrackPath(cueAudio, 2));
    g_lib[cueId].durationMs = 1000;
    g.playing = cueId;
    RegionTrackChanged();
    CommandRegion(IDM_REGION_START, 0.25);
    CommandRegion(IDM_REGION_END, 0.75);
    CommandRegion(IDM_REGION_LOOP, 0.25);
    PlayItem cueItem = MakeItem(125, cueId);
    Check(cueReady && cueItem.start == 2 && cueItem.end == 3 && cueItem.loopStart == 0.25 && cueItem.loopEnd == 0.75,
          L"UI: section marks of a later CUE track stay relative to its range (ready=%d range=%.2f-%.2f section=%.2f-%.2f)",
          cueReady, cueItem.start, cueItem.end, cueItem.loopStart, cueItem.loopEnd);
    CommandRegion(IDM_REGION_CLEAR, 0);
    DeleteFileW(cueAudio.c_str());
    DeleteFileW(cueSheet.c_str());
    g.playing = kNone;
    g.playingPl = -1;

    SetVolumePercent(37);
    OnCommand(IDC_SPEAKER, STN_CLICKED, g.speaker);
    Check(g.volumePercent == 0 && SendMessageW(g.volume, TBM_GETPOS, 0, 0) == 0,
          L"UI: clicking the speaker mutes and updates the volume slider");
    OnCommand(IDM_MUTE, 0, nullptr);
    Check(g.volumePercent == 37 && SendMessageW(g.volume, TBM_GETPOS, 0, 0) == 37,
          L"UI: unmute restores the previous volume");
    SetVolumePercent(0);
    SetVolumePercent(19);
    ToggleMute();
    ToggleMute();
    Check(g.volumePercent == 19, L"UI: moving the slider establishes the next unmute volume");
    g.playlists[1].items = {0, 0, 1};
    ClearSelection();
    RebuildView();
    int second = RowOfViewPos(1);
    ListView_SetItemState(g.list, second, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ActivatePlaylist(0);
    ActivatePlaylist(1);
    Check(ListView_GetSelectedCount(g.list) == 1 && ListView_GetNextItem(g.list, -1, LVNI_FOCUSED) == second,
          L"UI: returning to a playlist keeps the selected occurrence of a duplicate track");
    // Album headers, and so their covers, are not a Library-only thing: any list groups an album
    // whose tracks sit together, Now Playing and auto-playlists (Recently added) included.
    {
        const int np = (int)g.playlists.size();
        InsertPlaylist(np, Playlist{L"Now Playing", {}, PlaylistKind::NowPlaying});
        g.playlists[np].items = {0, 1, 2};
        ActivatePlaylist(np);
        Check(g.groups.size() == 1 && RowCount() == 5 && RowTrack(2) != kNone && ViewPosOfRow(4) == 2,
              L"UI: a Now Playing album groups under a header like the Library (groups=%zu rows=%d)", g.groups.size(),
              RowCount());
    }
    TestPortedUi(scratch);
    TestReplayGainRestart(scratch);
    TestPersonalization(scratch);
    TestWebPreferences(scratch);
    StopAppWorkers(g.hwnd);
    Check(!g.rgWorker.joinable() && !g.tagWorker.joinable() && !g.importWorker.joinable() && !g.artWorker.joinable(),
          L"UI: shutdown joins every application worker before destroying the test window");
    DestroyWindow(g.tips);
    DestroyWindow(g.hwnd);
    g.hwnd = nullptr;
    g.libView = nullptr;
    g.pointsChip = nullptr;
    g.closing = false;
    ClearBrowserCaches();
}
