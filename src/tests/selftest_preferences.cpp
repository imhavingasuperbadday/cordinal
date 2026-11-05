// Preferences use an isolated profile and real controls. Reload through the startup readers,
// without SaveState: changes must already be on disk before the application exits.
#include "selftest_common.h"

#include "metadata_fetch.h"

namespace {
HWND ProcessWindow(const wchar_t* className) {
    for (HWND window = nullptr; (window = FindWindowExW(nullptr, window, className, nullptr));) {
        DWORD process = 0;
        GetWindowThreadProcessId(window, &process);
        if (process == GetCurrentProcessId()) return window;
    }
    return nullptr;
}

// True when `menu` or one of its submenus holds command `id`.
bool Holds(HMENU menu, UINT id) {
    for (int i = 0, n = GetMenuItemCount(menu); i < n; ++i)
        if (GetMenuItemID(menu, i) == id || (GetSubMenu(menu, i) && Holds(GetSubMenu(menu, i), id))) return true;
    return false;
}

void TestWaveform(const std::wstring& scratch) {
    using namespace app;
    testlog::Batch batch;
    const auto savedPlaying = g.playing;
    const auto savedLayout = g.layout;
    const auto savedTheme = g.themeMode;
    const auto savedSize = g.size;
    const bool savedLive = g.liveWaveform, savedFullLive = g.fullLiveWaveform;
    g.liveWaveform = g.fullLiveWaveform = false;
    // One chapter at 30 seconds in an otherwise empty ID3v2.3 file. The waveform is seeded in
    // the private peaks cache, so these state checks need neither a decoder nor an audio device.
    const std::string header("ID3\x03\0\0\0\0\0\x1c", 10);
    const std::string chapter = std::string("CHAP\0\0\0\x12\0\0", 10) +
        std::string("c\0\0\0\x75\x30\0\0\xea\x60\xff\xff\xff\xff\xff\xff\xff\xff", 18);
    const std::wstring plain = scratch + L"\\waveform-plain.mp3", marked = scratch + L"\\waveform-chapter.mp3";
    Ready(WriteTestBytes(plain, std::string("ID3\x03\0\0\0\0\0\0", 10)) &&
              WriteTestBytes(marked, header + chapter), L"chapter waveform fixtures are prepared");
    TrackId ids[] = {g_lib.Add(plain), g_lib.Add(marked)};
    for (TrackId id : ids) g_lib[id].durationMs = 120000;
    g.playing = ids[1];
    batch.Next(L"chapters keep the playing title and section navigation while loading works at both edges");
    Check(CurrentChapterText() == L"Chapter 1/1" && ChapterStartAfter(0) == 30,
          L"waveform chapters remain available for the playing title and section navigation");
    g.themeMode = ThemeMode::Dark;
    ApplyTheme();
    g.size = ViewSize::Normal;
    g.layout = MainLayout{};
    bool clean = true;
    for (bool ready : {false, true}) {
        if (ready) {
            StartPeaks(nullptr, 0, scratch + L"\\waveform-peaks");
            Peaks peaks{};
            std::memset(peaks.v, 128, sizeof(peaks.v));
            for (TrackId id : ids) StorePeaks(g_lib.Path(id), 0, 0, 0, 0, peaks);
            WaveformTrackChanged();
            Check(WaveformShown(), L"the playing waveform retries after the peaks service becomes ready");
        }
        for (Dock dock : {Dock::Top, Dock::Left}) {
            g.layout.player = dock;
            Layout();
            for (TrackId id : ids) {
                g.playing = id;
                WaveformTrackChanged();
                clean = clean && WaveformShown() == ready;
            }
        }
    }
    Check(clean, L"chapters preserve horizontal and vertical waveform loading and ready states");
    SendMessageW(g.seek, TBM_SETPOS, FALSE, 625);
    g.liveWaveform = true; WaveformTrackChanged(true); WaveformTick();
    bool live = LiveWaveformShown() && WaveformShown() && SendMessageW(g.seek, TBM_GETPOS, 0, 0) == 625;
    if (const Tweak* display = FindTweak("view.seek_bar")) {
        const auto layout = g.layout;
        RECT window{}; GetWindowRect(g.hwnd, &window);
        SetWindowPos(g.hwnd, nullptr, 0, 0, S(ui::kLayoutWidth), S(ui::kLayoutHeight),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        const bool enabled = IsWindowEnabled(g.seek) != 0;
        EnableWindow(g.seek, TRUE);
        SetTweak(*display, 0);
        testlog::Cases regular;
        for (ViewSize size : {ViewSize::Compact, ViewSize::Normal, ViewSize::Spotlight, ViewSize::Big})
            for (Dock dock : {Dock::Top, Dock::Bottom, Dock::Left, Dock::Right}) {
                g.size = size; g.layout.player = dock; Layout();
                RECT client{}; GetClientRect(g.seek, &client);
                HDC dc = CreateCompatibleDC(nullptr);
                BITMAPINFO info{};
                info.bmiHeader = {sizeof(BITMAPINFOHEADER), client.right, -client.bottom, 1, 32, BI_RGB};
                void* pixels = nullptr;
                HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
                HGDIOBJ old = bitmap ? SelectObject(dc, bitmap) : nullptr;
                bool regularDrawn = dc && bitmap && !DrawWaveform(dc, client, client, 0.625);
                if (regularDrawn) {
                    SendMessageW(g.seek, WM_PRINTCLIENT, (WPARAM)dc, PRF_CLIENT);
                    bool vertical = SeekVertical();
                    int inset = S(vertical ? 4 : ui::kSliderInset);
                    int length = vertical ? client.bottom : client.right, cross = vertical ? client.right : client.bottom;
                    int along = inset + (int)std::lround((length - 2 * inset) * 0.625);
                    int across = cross / 2 - S(ui::kSliderThumbHeight) / 2;
                    regularDrawn = GetPixel(dc, vertical ? across : along, vertical ? along : across) != PlayerColor();
                }
                regular.Add(regularDrawn && !LiveWaveformShown() && !WaveformShown() && !FountainShown() && g.liveWaveform &&
                                SendMessageW(g.seek, TBM_GETPOS, 0, 0) == 625,
                            std::to_wstring((int)size) + L"/" + std::to_wstring((int)dock));
                if (old) SelectObject(dc, old);
                if (bitmap) DeleteObject(bitmap);
                if (dc) DeleteDC(dc);
            }
        SetWindowPos(g.hwnd, nullptr, 0, 0, window.right - window.left, window.bottom - window.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        g.size = ViewSize::Normal; g.layout = layout; Layout();
        SetTweak(*display, 1);
        batch.Next(L"regular playback overrides the live waveform in every size and dock without moving the song");
        Check(regular.Passed() && LiveWaveformShown() && WaveformShown() && g.liveWaveform &&
                  SendMessageW(g.seek, TBM_GETPOS, 0, 0) == 625,
              L"regular playback overrides live audio, draws a handle and restores the saved live choice%ls",
              regular.failed.c_str());
        g.liveWaveform = false;
        SetTweak(*display, 0);
        bool overviewHidden = !WaveformShown();
        SetTweak(*display, 1);
        Check(overviewHidden && WaveformShown(), L"the playback display choice also hides and restores whole-song peaks");
        g.liveWaveform = true;
        EnableWindow(g.seek, enabled);
    } else Check(false, L"View preferences includes Seek bar");
    const auto originalDuration = g_lib[ids[1]].durationMs;
    testlog::Cases lit;
    for (Dock dock : {Dock::Top, Dock::Bottom}) {
        g.layout.player = dock; Layout();
        for (uint32_t duration : {300000u, 7200000u}) {
            g_lib[ids[1]].durationMs = duration;
            for (int position : {1, 500, 999}) {
                SendMessageW(g.seek, TBM_SETPOS, FALSE, position); WaveformTick();
                g.fullLiveWaveform = true;
                bool full = WaveformRestColor() == WaveformPlayedColor();
                g.fullLiveWaveform = false;
                lit.Add(full && WaveformRestColor() != WaveformPlayedColor(),
                        std::to_wstring((int)dock) + L"/" + std::to_wstring(duration) + L"/" + std::to_wstring(position));
            }
        }
    }
    batch.Next(L"live waveforms switch full color at every position and never draw an upright playhead line");
    Check(lit.Passed(), L"full live color can switch off and on at every position in short and two-hour songs%ls",
          lit.failed.c_str());
    {
        HDC dc = CreateCompatibleDC(nullptr);
        BITMAPINFO info{};
        info.bmiHeader = {sizeof(BITMAPINFOHEADER), 120, -26, 1, 32, BI_RGB};
        void* pixels = nullptr;
        HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        HGDIOBJ old = bitmap ? SelectObject(dc, bitmap) : nullptr;
        bool noLine = dc && bitmap;
        if (noLine) for (bool full : {false, true}) {
            g.fullLiveWaveform = full;
            noLine = DrawWaveform(dc, {0, 0, 120, 26}, {4, 0, 116, 26}, 0.001) &&
                GetPixel(dc, 4, S(2)) == PlayerColor() && noLine;
        }
        Check(noLine, L"live waveforms have no upright playhead line with either color choice");
        if (old) SelectObject(dc, old);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
    }
    g.fullLiveWaveform = savedFullLive;
    g_lib[ids[1]].durationMs = originalDuration;
    SendMessageW(g.seek, TBM_SETPOS, FALSE, 625);
    g.liveWaveform = false; WaveformTrackChanged(true); WaveformTick();
    Check(live && !LiveWaveformShown() && WaveformShown() && SendMessageW(g.seek, TBM_GETPOS, 0, 0) == 625,
          L"songs switch between live and whole-track waveforms without changing the seek position");
    g.liveWaveform = savedLive;
    const bool paused = g.engine.Paused(), stopAfter = g.stopAfterCurrent, startupTheme = g.startupTheme;
    g.engine.SetPaused(false);
    g.stopAfterCurrent = false;
    g.startupTheme = false;
    g.layout.waveOwnRow = true;
    testlog::Cases switches;
    for (int origin = 0; origin < (int)ThemeMode::Count; ++origin) {
        for (ThemeMode target : {ThemeMode::Dark, ThemeMode::Light}) {
            bool colored = true;
            for (int dock = 0; dock < (int)Dock::Count; ++dock) {
                g.layout.player = (Dock)dock;
                OnCommand(IDM_THEME_BASE + origin, 0, nullptr);
                Layout();
                SendMessageW(g.seek, TBM_SETPOS, FALSE, 375);
                WaveformTick();
                OnCommand(IDM_THEME_BASE + (int)target, 0, nullptr);
                colored = colored && SendMessageW(g.seek, TBM_GETPOS, 0, 0) == 375;
                for (int pos : {250, 750, 0, 1000}) {
                    SendMessageW(g.seek, TBM_SETPOS, FALSE, pos);
                    WaveformTick();
                    colored = colored && WaveformShown() && WaveformPlayedColor() == Ref(P().accent) &&
                              SendMessageW(g.seek, TBM_GETPOS, 0, 0) == pos;
                }
            }
            switches.Add(colored, std::wstring(ThemeInfo((ThemeMode)origin).name) + L" to " + ThemeInfo(target).name);
        }
    }
    batch.Next(L"waveform keeps its played accent and position from every theme to Light and Dark at every edge");
    Check(switches.Passed(), L"waveform keeps its played accent and position from every theme to Light and Dark at every "
          L"edge%ls", switches.failed.c_str());
    g.layout.player = Dock::Top;
    g.size = ViewSize::Normal;
    g.engine.SetPaused(false);
    OnCommand(IDM_THEME_BASE + (int)ThemeMode::Apollo, 0, nullptr);
    g.playing = kNone;
    WaveformTrackChanged();
    StopPeaks();
    StartPeaks(nullptr, 0, scratch + L"\\waveform-late-peaks");
    g.playing = ids[0];
    WaveformTrackChanged();
    const bool waiting = WaveformShown();
    {
        g.liveWaveform = g.fullLiveWaveform = false;
        HDC dc = CreateCompatibleDC(nullptr);
        BITMAPINFO info{};
        info.bmiHeader = {sizeof(BITMAPINFOHEADER), 120, -26, 1, 32, BI_RGB};
        void* pixels = nullptr;
        HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        HGDIOBJ old = bitmap ? SelectObject(dc, bitmap) : nullptr;
        const bool columns = dc && bitmap && !LiveWaveformShown() &&
                             DrawWaveform(dc, {0, 0, 120, 26}, {4, 0, 116, 26}, 0.625) &&
                             GetPixel(dc, 4, 13) != PlayerColor() && GetPixel(dc, 4, 2) == PlayerColor();
        batch.Next(L"whole-track columns appear while an uncached song loads and refresh from completed cache data");
        Check(waiting && columns, L"whole-track waveform columns appear while an uncached song is loading");
        if (old) SelectObject(dc, old);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
    }
    Peaks completed{};
    std::memset(completed.v, 128, sizeof(completed.v));
    std::memset(completed.mood, 128, sizeof(completed.mood));
    StorePeaks(g_lib.Path(g.playing), 0, 0, 0, 0, completed);
    SendMessageW(g.seek, TBM_SETPOS, FALSE, 625);
    OnCommand(IDM_THEME_BASE + (int)ThemeMode::Dark, 0, nullptr);
    Check(waiting && WaveformShown() && SendMessageW(g.seek, TBM_GETPOS, 0, 0) == 625,
          L"switching from Apollo refreshes a pending waveform from completed cache data without replay");
    {
        // The reflection is the dimmer lower half; off keeps the upper half alone. Both the plain
        // waveform and the flat moodbar stripe follow the setting.
        g.waveReflection = true;
        WaveformTrackChanged();
        Pump(5000, [] { return WaveformPeak(0, 1) > 0; });
        HDC dc = CreateCompatibleDC(nullptr);
        BITMAPINFO info{};
        info.bmiHeader = {sizeof(BITMAPINFOHEADER), 120, -26, 1, 32, BI_RGB};
        void* pixels = nullptr;
        HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        HGDIOBJ old = bitmap ? SelectObject(dc, bitmap) : nullptr;
        const RECT rc{0, 0, 120, 26}, channel{4, 0, 116, 26};
        const int style = g.seekStyle;
        testlog::Cases reflections;
        bool drawn = dc && bitmap && !LiveWaveformShown();
        const bool savedGlow = g.seekGlow;
        g.seekGlow = false;  // the shape alone; the glow's own checks follow
        for (int painted : {kSeekWaveform, kSeekMoodbar}) {
            g.seekStyle = painted;
            for (bool mirror : {true, false}) {
                g.waveReflection = mirror;
                // Without the mirror, Full height still mirrors the waveform, its lower half as bright
                // as the upper (row 14 matches row 12, row 15 matches row 11); the stripe keeps the
                // upper half, ending at the center line.
                const bool waveform = painted == kSeekWaveform;
                auto drawnAt = [&](int y) { return GetPixel(dc, 10, y) != PlayerColor(); };
                auto same = [&](int a, int b) { return GetPixel(dc, 10, a) == GetPixel(dc, 10, b); };
                const bool ok = drawn && DrawWaveform(dc, rc, channel, 0.5) && drawnAt(12) &&
                                (waveform ? drawnAt(14) && same(14, 12) != mirror && (mirror || same(15, 11))
                                          : drawnAt(14) == mirror);
                reflections.Add(ok, std::wstring(painted == kSeekWaveform ? L"waveform " : L"moodbar ") +
                                       (mirror ? L"reflection" : L"upper half only"));
            }
        }
        // Upper half: the cut-off shape, stopping at the center line, for those who choose it.
        g.seekStyle = kSeekWaveform;
        g.waveReflection = false;
        g.waveFullHeight = false;
        const bool cut = drawn && DrawWaveform(dc, rc, channel, 0.5) && GetPixel(dc, 10, 12) != PlayerColor() &&
                         GetPixel(dc, 10, 14) == PlayerColor() && GetPixel(dc, 10, 22) == PlayerColor();
        reflections.Add(cut, L"waveform upper half (cut off)");
        g.waveFullHeight = true;
        batch.Next(L"waveform reflection draws the mirrored lower half or the upper half alone");
        Check(reflections.Passed(), L"waveform and moodbar reflection can be turned off without losing the upper half%ls",
              reflections.failed.c_str());
        // Seek bar glow: every look lights the bar above its top edge (the first row that isn't the
        // bar's own colour rises) and below its reflection, fading out before the bar's edge. Off, the
        // bar is as before.
        g.waveReflection = true;
        testlog::Cases glows;
        auto firstLit = [&](bool fromTop) {
            for (int i = 0; i < 26; ++i)
                if (GetPixel(dc, 10, fromTop ? i : 25 - i) != PlayerColor()) return i;
            return 26;
        };
        for (int painted : {kSeekWaveform, kSeekMoodbar, kSeekMoodWaveform}) {
            g.seekStyle = painted;
            g.seekGlow = false;
            const bool unlit = drawn && DrawWaveform(dc, rc, channel, 0.5);
            // The stripe stands shorter so its glow fits in the same height: its outer rows change
            // to the glow instead of the lit part growing.
            const int top = firstLit(true), bottom = firstLit(false);
            const COLORREF topEdge = GetPixel(dc, 10, top), bottomEdge = GetPixel(dc, 10, 25 - bottom);
            g.seekGlow = true;
            const bool glowing = unlit && DrawWaveform(dc, rc, channel, 0.5);
            const bool above = firstLit(true) < top || GetPixel(dc, 10, top) != topEdge;
            const bool below = firstLit(false) < bottom || GetPixel(dc, 10, 25 - bottom) != bottomEdge;
            glows.Add(glowing && above && below && firstLit(true) > 0 && firstLit(false) > 0,
                      L"look " + std::to_wstring(painted));
        }
        // With the upper half alone the stripe stands on the center line and still glows below it.
        g.seekStyle = kSeekMoodbar;
        g.waveReflection = false;
        const bool upper = drawn && DrawWaveform(dc, rc, channel, 0.5);
        const COLORREF stripeColor = GetPixel(dc, 10, 12), under = GetPixel(dc, 10, 14);
        glows.Add(upper && under != PlayerColor() && under != stripeColor && GetPixel(dc, 10, 25) == PlayerColor(),
                  L"below the upper half alone");
        g.waveReflection = true;
        g.seekGlow = savedGlow;
        batch.Next(L"seek bar glow lights past the edges of every look");
        Check(glows.Passed(), L"seek bar glow lights past the edges of every look and fades before the bar's edge%ls",
              glows.failed.c_str());
        // Every look builds its pixels directly, the upright seek bar's transposed DC included, and
        // writes the glow without a rectangle a row. Those pixels must be the GDI path's (which a
        // non-MM_TEXT target still takes), and the upright bar the horizontal one turned. Paused, so
        // the glow's pulse holds still between the paints.
        {
            const bool wasPaused = g.engine.Paused(), wasReflection = g.waveReflection, wasFull = g.waveFullHeight;
            g.engine.SetPaused(true);
            g.seekGlow = true;
            auto render = [&](bool upright, bool gdi, std::vector<uint32_t>& out) {
                const int w = upright ? 26 : 120, h = upright ? 120 : 26;
                HDC target = CreateCompatibleDC(nullptr);
                BITMAPINFO image{};
                image.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
                void* bits = nullptr;
                HBITMAP surface = CreateDIBSection(target, &image, DIB_RGB_COLORS, &bits, nullptr, 0);
                HGDIOBJ previous = surface ? SelectObject(target, surface) : nullptr;
                if (gdi) {
                    SetMapMode(target, MM_ANISOTROPIC);
                    SetWindowExtEx(target, 1, 1, nullptr);
                    SetViewportExtEx(target, 1, 1, nullptr);
                }
                if (upright) {
                    SetGraphicsMode(target, GM_ADVANCED);
                    XFORM transpose{0, 1, 1, 0, 0, 0};
                    SetWorldTransform(target, &transpose);
                }
                const bool ok = surface && DrawWaveform(target, rc, channel, 0.5);
                GdiFlush();
                out.clear();
                if (ok) for (int i = 0; i < w * h; ++i) out.push_back(static_cast<uint32_t*>(bits)[i] & 0xFFFFFF);
                if (previous) SelectObject(target, previous);
                if (surface) DeleteObject(surface);
                DeleteDC(target);
                return ok;
            };
            testlog::Cases paths;
            for (int painted : {kSeekWaveform, kSeekMoodbar, kSeekMoodWaveform}) {
                for (int shape = 0; shape < 3; ++shape) {
                    g.seekStyle = painted;
                    g.waveReflection = shape == 0;
                    g.waveFullHeight = shape == 1;
                    std::vector<uint32_t> plain, plainGdi, up, upGdi;
                    bool ok = render(false, false, plain) && render(false, true, plainGdi) && render(true, false, up) &&
                              render(true, true, upGdi) && plain == plainGdi && up == upGdi;
                    for (int x = 0; ok && x < 120; ++x)
                        for (int y = 0; ok && y < 26; ++y) ok = up[x * 26 + y] == plain[y * 120 + x];
                    paths.Add(ok, L"look " + std::to_wstring(painted) + L" shape " + std::to_wstring(shape));
                }
            }
            batch.Next(L"direct and upright seek bar pixels match the GDI path for every look with the glow");
            Check(paths.Passed(), L"every look's direct pixels, upright or not, match the GDI path with the glow%ls",
                  paths.failed.c_str());
            g.engine.SetPaused(wasPaused);
            g.waveReflection = wasReflection;
            g.waveFullHeight = wasFull;
            g.seekGlow = savedGlow;
        }
        g.seekStyle = style;
        if (old) SelectObject(dc, old);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
    }
    testlog::Cases sizes;
    for (ViewSize size : {ViewSize::Compact, ViewSize::Normal, ViewSize::Spotlight, ViewSize::Big}) {
        g.size = size;
        bool states = true;
        for (ThemeMode target : {ThemeMode::Dark, ThemeMode::Light}) {
            for (int state = 0; state < 4; ++state) {
                g.engine.SetPaused(state != 0);
                g.stopAfterCurrent = state == 2;
                if (state == 3) g_lib[g.playing].flags |= Track::kFailed;
                OnCommand(IDM_THEME_BASE + (int)ThemeMode::Apollo, 0, nullptr);
                SendMessageW(g.seek, TBM_SETPOS, FALSE, 500);
                WaveformTick();
                OnCommand(IDM_THEME_BASE + (int)target, 0, nullptr);
                WaveformTick();
                const Rgb expected = state >= 2 ? P().error : state == 1 ? P().amber : P().accent;
                states = states && SendMessageW(g.seek, TBM_GETPOS, 0, 0) == 500 &&
                         WaveformPlayedColor() == Ref(expected);
                g_lib[g.playing].flags &= ~Track::kFailed;
            }
        }
        sizes.Add(states, L"view size " + std::to_wstring((int)size));
    }
    batch.Next(L"waveform theme switches preserve progress, pause and warning colors in every view size");
    Check(sizes.Passed(), L"waveform theme switches preserve progress, pause and warning colors in every view size%ls",
          sizes.failed.c_str());
    // A play request starts the actual song's waveform before the engine announces playback.
    const std::wstring early = scratch + L"\\waveform-early.wav", earlyCache = scratch + L"\\waveform-early-cache";
    if (Ready(WriteTestBytes(early, MakeSineWav(2, 2)), L"early waveform fixture is prepared")) {
        TrackId id = g_lib.Add(early);
        g.playing = kNone;
        WaveformTrackChanged();
        StopPeaks();
        StartPeaks(nullptr, 0, earlyCache);
        g.liveWaveform = g.fullLiveWaveform = false;
        PrepareWaveform(id);
        const std::wstring cached = earlyCache + L"\\" + PeaksKeyName(early, 0, 0) + L".pk";
        Pump(5000, [&] { return GetFileAttributesW(cached.c_str()) != INVALID_FILE_ATTRIBUTES; });
        const bool prepared = GetFileAttributesW(cached.c_str()) != INVALID_FILE_ATTRIBUTES;
        g.playing = id;
        WaveformTrackChanged();
        batch.Next(L"a play request prepares the song's whole waveform before playback starts");
        Check(prepared && WaveformShown() && WaveformPeak(0.75, 0.8) > 0,
              L"a play request prepares the song's whole waveform before playback starts");
        DeleteFileW(early.c_str());
    }
    {
        // A theme build can save these; the plain waveform still reads them when this build does
        // not link the fountain menu or animation.
        const wchar_t* names[] = {L"Top", L"Bottom", L"Left", L"Right"};
        const Rgb played[] = {0x112233, 0x223344, 0x334455, 0x445566};
        const Rgb rest[] = {0x556677, 0x667788, 0x778899, 0x8899AA};
        std::wstring savedPlayed[4], savedRest[4];
        for (int i = 0; i < (int)Dock::Count; ++i) {
            savedPlayed[i] = IniRead(L"Waveform", (std::wstring(names[i]) + L"Played").c_str(), L"");
            savedRest[i] = IniRead(L"Waveform", (std::wstring(names[i]) + L"Unplayed").c_str(), L"");
        }
        g.size = ViewSize::Normal;
        g.layout.player = Dock::Top;
        g.playing = kNone;
        g.engine.SetPaused(false);
        g.stopAfterCurrent = false;
        g.fullLiveWaveform = false;
        const COLORREF themePlayed = WaveformPlayedColor(), themeRest = WaveformRestColor();
        auto write = [&](int dock, const std::wstring& playedValue, const std::wstring& restValue) {
            IniWrite(L"Waveform", (std::wstring(names[dock]) + L"Played").c_str(), playedValue);
            IniWrite(L"Waveform", (std::wstring(names[dock]) + L"Unplayed").c_str(), restValue);
        };
        for (int i = 0; i < (int)Dock::Count; ++i) {
            wchar_t p[8], r[8];
            swprintf_s(p, L"%06X", played[i]);
            swprintf_s(r, L"%06X", rest[i]);
            write(i, p, r);
        }
        LoadWaveLook();
        testlog::Cases positions;
        for (int i = 0; i < (int)Dock::Count; ++i) {
            g.layout.player = (Dock)i;
            bool own = WaveformPlayedColor() == Ref(played[i]) && WaveformRestColor() == Ref(rest[i]);
            g.size = ViewSize::Compact;
            bool theme = WaveformPlayedColor() == themePlayed && WaveformRestColor() == themeRest;
            g.size = ViewSize::Normal;
            positions.Add(own && theme, names[i]);
        }
        for (int i = 0; i < (int)Dock::Count; ++i) write(i, L"", L"");
        LoadWaveLook();
        g.layout.player = Dock::Bottom;
        bool cleared = WaveformPlayedColor() == themePlayed && WaveformRestColor() == themeRest;
        for (int i = 0; i < (int)Dock::Count; ++i) write(i, savedPlayed[i], savedRest[i]);
        LoadWaveLook();
        batch.Next(L"per-position waveform colors load from settings in every build");
        Check(positions.Passed() && cleared,
              L"per-position waveform colors apply in Normal and fall back in Compact and after reset%ls",
              positions.failed.c_str());
    }
    g.engine.SetPaused(paused);
    g.stopAfterCurrent = stopAfter;
    g.startupTheme = startupTheme;
    g.liveWaveform = savedLive;
    g.fullLiveWaveform = savedFullLive;
    StopPeaks();
    g.playing = savedPlaying;
    WaveformTrackChanged();
    g.layout = savedLayout;
    g.themeMode = savedTheme;
    g.size = savedSize;
    ApplyTheme();
    Layout();
    DeleteFileW(plain.c_str());
    DeleteFileW(marked.c_str());
}

void TestTabOverflow() {
    using namespace app;
    testlog::Group group(L"playlist tab overflow");
    testlog::Batch batch;
    size_t count = g.playlists.size();
    UINT dpi = g.dpi;
    for (int i = 0; i < 16; ++i) g.playlists.push_back({L"Long playlist for overflow " + std::to_wstring(i), {}});
    g.layout.tabs = g.layout.search = true;
    bool clipped = true, scrolls = true;
    for (ThemeMode theme : {ThemeMode::Dark, ThemeMode::Deltarune}) {
        g.themeMode = theme;
        ApplyTheme();
        for (UINT testDpi : {96u, 144u, 192u}) {
            g.dpi = testDpi;
            CreateFonts();
            ApplyFonts();
            SetWindowPos(g.hwnd, nullptr, 0, 0, S(640), S(752), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            Layout();
            RefreshTabs();
            InvalidateRect(g.tabs, nullptr, TRUE);
            UpdateWindow(g.tabs);  // attaches and reserves the native scroll buttons
            HWND scroll = FindWindowExW(g.tabs, nullptr, UPDOWN_CLASSW, nullptr);
            if (!scroll || !(GetWindowLongPtrW(scroll, GWL_STYLE) & WS_VISIBLE)) {
                clipped = scrolls = false;
                continue;
            }
            RECT client{}, arrows{}, search{}, tabWindow{};
            GetClientRect(g.tabs, &client);
            GetWindowRect(scroll, &arrows);
            GetWindowRect(g.search, &search);
            GetWindowRect(g.tabs, &tabWindow);
            clipped = clipped && arrows.right <= tabWindow.right && tabWindow.right <= search.left;
            MapWindowPoints(nullptr, g.tabs, (POINT*)&arrows, 2);
            RECT first{}, after{}, buttonClient{};
            TabCtrl_GetItemRect(g.tabs, 0, &first);
            GetClientRect(scroll, &buttonClient);
            LPARAM right = MAKELPARAM(buttonClient.right * 3 / 4, buttonClient.bottom / 2);
            SendMessageW(scroll, WM_LBUTTONDOWN, MK_LBUTTON, right);
            SendMessageW(scroll, WM_LBUTTONUP, 0, right);
            TabCtrl_GetItemRect(g.tabs, 0, &after);
            scrolls = scrolls && after.left < first.left;
        }
    }
    batch.Next(L"themed overflow arrows sit clear of search and scroll the native tab strip");
    Check(clipped, L"tabs stop before the overflow arrows and the arrows stay clear of search");
    Check(scrolls, L"the themed overflow arrows still scroll the native tab strip");
    g.playlists.resize(count);
    g.dpi = dpi;
    CreateFonts();
    ApplyFonts();
    RefreshTabs();
}

void TestPreferenceSavingModes(const std::wstring& scratch) {
    using namespace app;
    testlog::Group group(L"preference saving modes");
    testlog::Batch batch;
    const std::wstring original = g.iniPath, profile = scratch + L"\\preference-modes.ini";
    ViewSize originalSize = g.size;
    WriteTestBytes(profile, "\xFF\xFE");
    g.iniPath = profile;
    auto reload = [] { LoadPreferences(); LoadBrowserSettings(); ResetPreferenceSuggestions(); };
    reload();
    SetViewSize(ViewSize::Normal);
    ResetPreferenceSuggestions();
    batch.Next(L"new profiles ask before adopting defaults without assuming a future policy");
    Check(g.preferenceSaving == PreferenceSaving::Ask && !g.preferenceSavingChosen,
          L"new profiles ask before adopting defaults without assuming a future policy decision");
    OnCommand(IDM_PREFERENCE_MANUAL, 0, nullptr);
    g.themeMode = ThemeMode::Dark;
    g.libMode = LibraryMode::Tracks;
    ObservePreferenceChanges(GetTickCount64());
    FlushAutomaticPreferences();
    batch.Next(L"manual mode disables questions and automatic saving, and survives a restart");
    Check(!PreferenceSuggestionsDue(GetTickCount64() + 300000) && g.defaultTheme == ThemeMode::Light &&
              g.defaultLibraryMode == LibraryMode::Albums,
          L"manual mode disables questions and automatic saving");
    reload();
    Check(g.preferenceSaving == PreferenceSaving::Manual && g.preferenceSavingChosen && g.themeMode == ThemeMode::Light &&
              g.libMode == LibraryMode::Albums, L"manual saving and approved defaults survive a restart");
    g.themeMode = ThemeMode::Dark;
    g.libMode = LibraryMode::Tracks;
    OnCommand(IDM_PREFERENCE_AUTO, 0, nullptr);
    FlushAutomaticPreferences();
    batch.Next(L"automatic saving commits only categories changed after opt-in and survives a restart");
    Check(g.defaultTheme == ThemeMode::Light && g.defaultLibraryMode == LibraryMode::Albums,
          L"automatic opt-in does not approve existing unchecked choices");
    g.browserVisible = false;
    g.layout.player = Dock::Right;
    ObservePreferenceChanges(GetTickCount64());
    Check(!g.defaultBrowserVisible && g.defaultLayout.player == Dock::Right &&
              g.defaultTheme == ThemeMode::Light && g.defaultLibraryMode == LibraryMode::Albums,
          L"automatic saving commits only categories changed after opt-in");
    reload();
    Check(g.preferenceSaving == PreferenceSaving::Automatic && !g.browserVisible && g.layout.player == Dock::Right &&
              g.themeMode == ThemeMode::Light && g.libMode == LibraryMode::Albums,
          L"automatic policy and its future changes survive restarting");
    g.themeMode = ThemeMode::Dark;
    FlushAutomaticPreferences();
    reload();
    batch.Next(L"the exit flush saves, re-enabling questions restores batching and invalid policies ask");
    Check(g.themeMode == ThemeMode::Dark, L"the exit flush saves changes made before the next UI tick");
    OnCommand(IDM_PREFERENCE_ASK, 0, nullptr);
    g.themeMode = ThemeMode::Light;
    ObservePreferenceChanges(GetTickCount64());
    Check(PreferenceSuggestionsDue(GetTickCount64() + 300000) && g.defaultTheme == ThemeMode::Dark,
          L"re-enabling questions restores batching instead of silently saving choices");
    IniWrite(L"Suggestions", L"SavingMode", L"99");
    LoadPreferences();
    Check(g.preferenceSaving == PreferenceSaving::Ask && !g.preferenceSavingChosen,
          L"invalid saving policies fall back to asking without assuming consent");
    SetFileAttributesW(profile.c_str(), FILE_ATTRIBUTE_READONLY);
    g.message.clear();
    bool refused = !IniWrite(L"Player", L"Volume", L"12") && g.message.find(L"Couldn't save settings") != std::wstring::npos;
    g.message.clear();
    IniWrite(L"Player", L"Shuffle", L"1");
    batch.Next(L"a failed settings write is reported once and writes recover once the file is writable");
    Check(refused && g.message.empty(), L"a failed settings write is reported once, not once per key");
    SetFileAttributesW(profile.c_str(), FILE_ATTRIBUTE_NORMAL);
    Check(IniWrite(L"Player", L"Volume", L"12"), L"settings writes succeed again once the file is writable");
    SetViewSize(originalSize);
    g.iniPath = original;
    reload();
    DeleteFileW(profile.c_str());
}

// The Tweaks registry reads and writes settings.ini exactly as LoadPreferences/SavePreferences did
// before it, a live change goes through the setting's own command, and the palette finds and runs
// menu commands.
void TestTweaks(const std::wstring& scratch) {
    using namespace app;
    testlog::Group group(L"tweaks and command palette");
    testlog::Batch batch;
    const std::wstring original = g.iniPath, profile = scratch + L"\\tweaks.ini";
    WriteTestBytes(profile, "\xFF\xFE");
    g.iniPath = profile;
    LoadPreferences();

    bool unique = true, sane = true, defaults = true;
    for (const Tweak& t : Tweaks()) {
        unique = unique && FindTweak(t.key) == &t;
        sane = sane && strchr(t.key, '.') && t.min <= t.def && t.def <= t.max && *t.help &&
               (t.type == TweakType::Float || t.max >= 1) && (t.command || t.apply || t.type == TweakType::Float);
        // Start with Windows lives in Windows' Run key, not in the profile.
        defaults = defaults && ((t.flags & kTweakOutside) || !TweakChanged(t));
    }
    batch.Next(L"tweak keys are unique and sane, every row has a default and an empty profile loads them all");
    Check(unique && sane && Tweaks().size() >= 20, L"tweak keys are unique and every row has a default in range and help");
    // The session-rate rows (speed, tempo) start where the engine does: normal.
    bool rates = false;
    if (const Tweak* speed = FindTweak("playback.speed"))
        if (const Tweak* tempo = FindTweak("playback.tempo")) {
            g.speed = 1.5;
            g.tempo = 1.25;
            LoadPreferences();
            rates = g.speed == 1 && g.tempo == 1 && !TweakChanged(*speed) && !TweakChanged(*tempo);
        }
    std::wstring changedRows;
    for (const Tweak& t : Tweaks())
        if (!(t.flags & kTweakOutside) && TweakChanged(t))
            changedRows += std::wstring(changedRows.empty() ? L"" : L", ") + FromUtf8(t.key);
    Check(defaults && rates && changedRows.empty(), L"an empty profile loads every tweak at its default%ls",
          changedRows.empty() ? L"" : (L" (changed: " + changedRows + L")").c_str());
    // The reverb preset row mirrors the slider: at the slider's default it sits at 40%.
    if (const Tweak* preset = FindTweak("effects.reverb_mix_choice")) {
        const float live = g.reverb.mix;
        g.reverb.mix = 0.4f;
        batch.Next(L"the reverb preset row matches the slider's default amount");
        Check(preset->get() == 1 && !TweakChanged(*preset), L"the reverb preset row sits at 40 percent when the slider does");
        g.reverb.mix = live;
    } else {
        Check(false, L"Preferences lists the reverb preset");
    }
    {
        // Online sources stay off until chosen; each row names what it sends, and choices survive a reload.
        // A chosen cover is shown in cordinal only unless asked otherwise, so no file changes by default.
        bool off = !online::AnyCoverSource(), named = true, kept = true;
        int rows = 0;
        for (const Tweak& t : Tweaks()) {
            if (strncmp(t.key, "online.", 7)) continue;
            ++rows;
            const int def = strcmp(t.key, "online.cover_save") ? 0 : (int)online::CoverSave::Custom;
            off = off && t.def == def && t.get() == def && !wcscmp(TweakPage(t), L"Online");
            if (!strncmp(t.key, "online.covers_", 14)) named = named && wcsstr(t.help, L"Sends ") && wcsstr(t.help, L"artist and title");
            if (!strncmp(t.key, "online.lyrics_", 14)) named = named && wcsstr(t.help, L"Sends the song's artist");
        }
        for (const char* key : {"online.covers_deezer", "online.covers_musicbrainz", "online.cover_save"})
            if (const Tweak* t = FindTweak(key)) SetTweak(*t, 1);
        online::Config() = online::Settings{};
        LoadPreferences();
        kept = online::Config().covers[0] && !online::Config().covers[1] && online::Config().covers[2] &&
               online::Config().coverSave == 1 && online::CoverOrder().size() == 2 &&
               IniRead(L"Online", L"CoversDeezer", L"") == L"1";
        for (const char* key : {"online.covers_deezer", "online.covers_musicbrainz", "online.cover_save"})
            if (const Tweak* t = FindTweak(key)) SetTweak(*t, t->def);
        Check(rows >= 8 && off && named && kept && !online::AnyCoverSource(),
              L"online sources default to off, each names what it sends, and choices round-trip settings.ini");
    }
    // Seek bar and Seek bar shows each keep two of the [Window] keys earlier versions wrote, so a
    // profile reads the same in every version. Every choice must round-trip through them.
    const Tweak* seekBar = FindTweak("view.seek_bar");
    const Tweak* seekShows = FindTweak("view.seek_shows");
    if (seekBar && seekShows) {
        testlog::Cases looks;
        const struct { int value; const wchar_t* waveform; const wchar_t* style; } bars[] = {
            {1, L"1", L"0"}, {2, L"1", L"1"}, {3, L"1", L"2"}, {0, L"0", L"2"}};  // Progress bar keeps the look
        for (const auto& bar : bars) {
            g.waveform = bar.value == 0;  // start from another choice, so the row really changes
            SetTweak(*seekBar, bar.value); g.waveform = bar.value == 0; g.seekStyle = 0; LoadPreferences();
            looks.Add(seekBar->get() == bar.value && IniRead(L"Window", L"Waveform", L"") == bar.waveform &&
                          IniRead(L"Window", L"SeekStyle", L"") == bar.style,
                      L"seek bar " + std::to_wstring(bar.value));
        }
        const struct { int value; const wchar_t* live; const wchar_t* full; } shows[] = {
            {1, L"1", L"1"}, {2, L"1", L"0"}, {0, L"0", L"0"}};  // Whole song keeps the progress choice
        for (const auto& show : shows) {
            g.liveWaveform = show.value == 0;
            SetTweak(*seekShows, show.value); g.liveWaveform = show.value == 0; g.fullLiveWaveform = true;
            LoadPreferences();
            looks.Add(seekShows->get() == show.value && IniRead(L"Window", L"LiveWaveform", L"") == show.live &&
                          IniRead(L"Window", L"FullLiveWaveform", L"") == show.full,
                      L"seek bar shows " + std::to_wstring(show.value));
        }
        // A profile from before the merge: a regular bar over a remembered moodbar, live with progress.
        IniWrite(L"Window", L"Waveform", L"0"); IniWrite(L"Window", L"SeekStyle", L"1");
        IniWrite(L"Window", L"LiveWaveform", L"1"); IniWrite(L"Window", L"FullLiveWaveform", L"0");
        LoadPreferences();
        looks.Add(seekBar->get() == 0 && g.seekStyle == kSeekMoodbar && seekShows->get() == 2, L"older profile");
        SetTweak(*seekBar, 2);
        looks.Add(g.waveform && g.seekStyle == kSeekMoodbar, L"moodbar after a progress bar");
        SetTweak(*seekBar, 1); SetTweak(*seekShows, 0); SetTweak(*seekShows, 1); SetTweak(*seekShows, 0);
        looks.Add(seekBar->def == 1 && seekShows->def == 0 && !wcscmp(TweakPage(*seekBar), L"View") &&
                      !wcscmp(TweakPage(*seekShows), L"View") && g.fullLiveWaveform,
                  L"defaults and page");
        batch.Next(L"View preferences save and reload every seek bar look and source through the older keys");
        Check(looks.Passed(), L"Seek bar and Seek bar shows round-trip every choice through the older keys%ls",
              looks.failed.c_str());
    } else Check(false, L"View preferences includes Seek bar and Seek bar shows");
    testlog::Cases liveSettings;
    const struct { const char* key; const wchar_t* ini; bool* value; } liveRows[] = {
        {"view.live_fountain", L"LiveFountain", &g.liveFountain},
        {"view.seek_glow", L"SeekGlow", &g.seekGlow}
    };
    for (const auto& row : liveRows) {
        const Tweak* live = FindTweak(row.key);
        if (!live) { liveSettings.Add(false, FromUtf8(row.key)); continue; }
        SetTweak(*live, 1);
        *row.value = false; LoadPreferences();
        bool enabled = *row.value && !wcscmp(TweakPage(*live), L"View");
        SetTweak(*live, 0);
        *row.value = true; LoadPreferences();
        liveSettings.Add(enabled && !*row.value && IniRead(L"Window", row.ini, L"") == L"0", FromUtf8(row.key));
        SetTweak(*live, live->def);
    }
    batch.Next(L"View preferences save and reload the live fountain and seek bar glow");
    Check(liveSettings.Passed(), L"View preferences saves and reloads the live fountain and seek bar glow%ls",
          liveSettings.failed.c_str());
    if (const Tweak* size = FindTweak("view.band_wave_size")) {
        SetTweak(*size, 3); g.bandWaveSize = 0; LoadPreferences();
        bool tall = g.bandWaveSize == 3 && !wcscmp(TweakPage(*size), L"View");
        SetTweak(*size, 0); g.bandWaveSize = 5; LoadPreferences();
        Check(tall && g.bandWaveSize == 0 && IniRead(L"Window", L"BandWaveSize", L"") == L"0",
              L"top/bottom waveform size persists and can return to the layout's own size");
    } else Check(false, L"View preferences includes Top/bottom waveform size");
    const Tweak* colors = FindTweak("view.mood_colors");
    const Tweak* smooth = FindTweak("view.smooth_seek");
    if (colors && smooth) {
        SetTweak(*colors, kMoodSpectrum); SetTweak(*smooth, 1); g.moodColors = 0; g.smoothSeek = false; LoadPreferences();
        bool kept = g.moodColors == kMoodSpectrum && g.smoothSeek && colors->def == 0 && smooth->def == 0;
        SetTweak(*colors, kMoodTheme); SetTweak(*smooth, 0); LoadPreferences();
        Check(kept && g.moodColors == kMoodTheme && !g.smoothSeek && IniRead(L"Window", L"MoodColors", L"") == L"0",
              L"moodbar colours and the smooth seek bar persist both ways");
    } else Check(false, L"View preferences includes Moodbar colours and Smooth seek bar");
    // Waveform colour, its unplayed part and the mood waveform's own scheme persist, and the colours
    // the seek bar draws with follow them.
    const Tweak* waveColor = FindTweak("view.wave_color");
    const Tweak* waveRest = FindTweak("view.wave_rest_color");
    const Tweak* moodWave = FindTweak("view.mood_wave_colors");
    if (waveColor && waveRest && moodWave) {
        SetTweak(*waveColor, kWaveTeal); SetTweak(*waveRest, kWaveRestFaded); SetTweak(*moodWave, kMoodColorBlind + 1);
        SetTweak(*colors, kMoodWarm);
        g.waveColor = g.waveRestColor = g.moodWaveColors = g.moodColors = 0;
        LoadPreferences();
        const bool kept = g.waveColor == kWaveTeal && g.waveRestColor == kWaveRestFaded &&
                          g.moodWaveColors == kMoodColorBlind + 1 && g.moodColors == kMoodWarm;
        const COLORREF bar = PlayerColor();
        const Rgb barRgb = (Rgb)GetRValue(bar) << 16 | (Rgb)GetGValue(bar) << 8 | GetBValue(bar);
        const Rgb teal = WaveColorOn(kWaveTeal, barRgb);
        const bool follows = PreferredWaveColor() == teal && PreferredWaveRest(teal) == FadedWaveColor(teal, barRgb);
        SetTweak(*waveColor, kWaveTheme); SetTweak(*waveRest, kWaveRestTheme); SetTweak(*moodWave, 0);
        SetTweak(*colors, kMoodTheme);
        LoadPreferences();
        Check(kept && follows && g.waveColor == kWaveTheme && PreferredWaveColor() == kNoRgb &&
                  PreferredWaveRest(0x123456) == kNoRgb && g.moodWaveColors == 0 && waveColor->def == 0 &&
                  IniRead(L"Window", L"WaveColor", L"") == L"0",
              L"waveform colour, its unplayed part and the mood waveform colours persist both ways and drive the colours");
    } else Check(false, L"View preferences includes Waveform colour, its unplayed part and Mood waveform colours");
    if (const Tweak* reflection = FindTweak("view.wave_reflection")) {
        // Upper half, Mirrored and Full height, through WaveReflection (the older on/off key, so 0 and 1
        // mean what they always did) and WaveFullHeight beside it.
        testlog::Cases shapes;
        const struct { int value; const wchar_t* mirrored; const wchar_t* full; } rows[] = {
            {0, L"0", L"0"}, {2, L"0", L"1"}, {1, L"1", L"1"}};
        for (const auto& row : rows) {
            SetTweak(*reflection, row.value); g.waveReflection = row.value != 1; g.waveFullHeight = row.value != 2;
            LoadPreferences();
            shapes.Add(reflection->get() == row.value && IniRead(L"Window", L"WaveReflection", L"") == row.mirrored &&
                           IniRead(L"Window", L"WaveFullHeight", L"") == row.full,
                       L"shape " + std::to_wstring(row.value));
        }
        shapes.Add(reflection->def == 1 && !wcscmp(TweakPage(*reflection), L"View"), L"defaults and page");
        Check(shapes.Passed(), L"waveform reflection starts mirrored and persists all three shapes%ls", shapes.failed.c_str());
    } else Check(false, L"View preferences includes Waveform reflection");
    Check(ColumnAt(kColMood).name == std::wstring(L"Mood"), L"the track list offers a Mood column");
    if (const Tweak* gpu = FindTweak("view.gpu_drawing")) {
        const bool offByDefault = gpu->def == 0 && !wcscmp(TweakPage(*gpu), L"View");
        SetTweak(*gpu, 1); g.gpuDrawing = false; LoadPreferences();
        const bool kept = g.gpuDrawing && IniRead(L"Window", L"GpuDrawing", L"") == L"1";
        SetTweak(*gpu, 0); g.gpuDrawing = true; LoadPreferences();
        Check(offByDefault && kept && !g.gpuDrawing,
              L"drawing with the graphics card starts off, and the choice persists both ways");
    } else Check(false, L"View preferences includes Draw with the graphics card");
    {
        // The rows for features that had no registry entry: layout parts and the two library sizes
        // apply to the session at once (startup defaults approves them), while speed, Apollo's
        // bands and foobar2000's Album List view write their feature's own keys.
        const Tweak* dock = FindTweak("window.player_dock");
        const Tweak* transport = FindTweak("window.transport");
        const Tweak* cover = FindTweak("library.cover_size");
        const Tweak* height = FindTweak("library.browser_height");
        const Tweak* speed = FindTweak("playback.speed");
        const Tweak* apollo = FindTweak("view.apollo_analyzer");
        const Tweak* apolloFade = FindTweak("view.apollo_opacity");
        const Tweak* foobarView = FindTweak("view.foobar_view");
        const MainLayout live = g.layout, approved = g.defaultLayout;
        const int liveCover = g.coverSize, liveHeight = g.browserHeight;
        const double liveSpeed = g.speed;
        if (Ready(dock && transport && cover && height && speed && apollo && apolloFade && foobarView,
                  L"Preferences lists the layout, library size and per-theme rows")) {
            bool words = TweakText(*dock, 2) == L"Left" && TweakText(*apolloFade, 4) == L"100%";
            g.layout = g.defaultLayout = MainLayout{};
            SetTweak(*dock, 2);
            SetTweak(*transport, 0);
            const bool liveLayout = g.layout.player == Dock::Left && !g.layout.transport;
            const bool notApproved = g.defaultLayout == MainLayout{};
            SetTweak(*cover, 220);
            SetTweak(*height, 240);
            const bool sizes = g.coverSize == 220 && g.browserHeight == 240 && g.defaultCoverSize == 160 &&
                               g.defaultBrowserHeight == 150;
            SetTweak(*speed, 5);
            const bool rate = std::fabs(g.speed - 1.25) < 1e-9 && IniRead(L"Player", L"Speed", L"").empty();
            SetTweak(*apollo, 0);
            SetTweak(*apolloFade, 4);
            const bool apolloKeys = IniInt(L"Apollo", L"Analyzer", -1) == 0 && IniInt(L"Apollo", L"RebarOpacity", -1) == 100;
            SetTweak(*foobarView, 2);
            const bool foobarKey = IniInt(L"Foobar", L"AlbumListView", -1) == 2;
            batch.Next(L"the layout, library size and per-theme rows change the session or their own keys");
            Check(words && liveLayout && notApproved && sizes && rate && apolloKeys && foobarKey,
                  L"layout rows stay in the session, size rows stay unapproved, and theme rows write their keys");
            SetTweak(*dock, (double)(int)live.player);
            SetTweak(*transport, live.transport);
            SetTweak(*cover, liveCover);
            SetTweak(*height, liveHeight);
            SetTweak(*speed, (double)(std::find(std::begin(kSpeeds), std::end(kSpeeds), liveSpeed) - std::begin(kSpeeds)));
            SetTweak(*apollo, 1);
            SetTweak(*apolloFade, 1);
            SetTweak(*foobarView, 0);
            g.layout = live;
            g.defaultLayout = approved;
            ApplyMainLayout();
        }
    }

    const Tweak* resume = FindTweak("playback.resume");
    const Tweak* preamp = FindTweak("replaygain.preamp");
    const Tweak* loops = FindTweak("playback.game_loops");
    const Tweak* albums = FindTweak("view.group_albums");
    const Tweak* amount = FindTweak("effects.reverb_amount");
    const Tweak* newMusic = FindTweak("library.new_music");
    if (!resume || !preamp || !loops || !albums || !amount || !newMusic) {
        Check(false, L"the registry lists the settings the checks below use");
        g.iniPath = original;
        LoadPreferences();
        return;
    }
    bool automaticDefault = !g.inboxEnabled && newMusic->get() == 0 && !wcscmp(TweakPage(*newMusic), L"Library") &&
                            TweakText(*newMusic, 0) == L"Add automatically";
    SetTweak(*newMusic, 1);
    g.inboxEnabled = false;
    LoadPreferences();
    bool review = g.inboxEnabled && IniRead(L"Library", L"Inbox", L"") == L"1";
    SetTweak(*newMusic, 0);
    g.inboxEnabled = true;
    LoadPreferences();
    batch.Next(L"new music defaults to automatic and the Inbox choice stays compatible and persists");
    Check(automaticDefault && review && !g.inboxEnabled && IniRead(L"Library", L"Inbox", L"") == L"0",
          L"new music defaults to automatic, and the Inbox choice remains compatible and persists through Preferences");
    resume->put(0);
    preamp->put(2.5);
    loops->put(1);
    SavePreferences();
    batch.Next(L"tweaks keep their ini sections and number formats, and come back on the next start");
    Check(IniRead(L"Player", L"ResumeOn", L"") == L"0" && IniRead(L"ReplayGain", L"Preamp", L"") == L"2.50" &&
              IniRead(L"Player", L"Loops", L"") == L"1",
          L"tweaks keep their settings.ini sections, keys and number formats");
    resume->put(1);
    preamp->put(0);
    loops->put(2);
    LoadPreferences();
    Check(!g.resumeOn && g.rgPreamp == 2.5f && g.loops == 1, L"saved tweaks come back on the next start");
    const Tweak* resumeInSong = FindTweak("playback.resume_in_song");
    const Tweak* resumePlay = FindTweak("playback.resume_play");
    if (resumeInSong && resumePlay) {
        resumeInSong->put(1);
        resumePlay->put(1);
        SavePreferences();
        const bool saved = IniRead(L"Player", L"ResumeInSong", L"") == L"1" && IniRead(L"Player", L"ResumePlay", L"") == L"1";
        g.resumeInSong = g.resumePlay = false;
        LoadPreferences();
        const bool loaded = g.resumeInSong && g.resumePlay;
        resumeInSong->put(0);
        resumePlay->put(0);
        SavePreferences();
        Check(saved && loaded && !g.resumeInSong && !g.resumePlay && IniRead(L"Player", L"ResumeInSong", L"") == L"0",
              L"reopening in the last song and starting it on open are Preferences settings that persist");
    } else {
        Check(false, L"Preferences lists reopening in the last song and starting it on open");
    }

    IniWrite(L"Player", L"Loops", L"9");
    IniWrite(L"ReplayGain", L"KeepLoud", L"5");
    IniWrite(L"ReplayGain", L"Preamp", L"nan");
    WritePrivateProfileStringW(L"ReplayGain", L"Enabled", nullptr, g.iniPath.c_str());
    IniWrite(L"ReplayGain", L"Mode", L"0");
    LoadPreferences();
    batch.Next(L"out-of-range and invalid values are clamped or ignored, and old profiles still load their mode");
    Check(g.loops == 3 && g.rgKeep == 0.9f && g.rgPreamp == 0, L"out-of-range and invalid values are clamped or ignored");
    Check(!g.rgOn, L"a profile from before the ReplayGain switch still loads its old mode");

    SetTweak(*albums, 0);
    SetTweak(*loops, 0);
    SetTweak(*amount, 5);
    batch.Next(L"a live change runs the setting's command, is clamped, saved at once and described in words");
    Check(!g.groupAlbums && IniRead(L"Window", L"GroupAlbums", L"") == L"0" && g.loops == 0 && g.reverb.mix == 1 &&
              IniRead(L"Effects", L"ReverbAmount", L"") == L"1.00",
          L"a live change runs the setting's command, is clamped into range and is saved at once");
    Check(TweakChanged(*albums) && TweakText(*loops, 0) == L"Ignore loop points" && TweakText(*preamp, -3) == L"-3.00 dB",
          L"tweaks describe their values in words");
    const Tweak* saving = FindTweak("startup.saving");
    const Tweak* robotic = FindTweak("view.robotic_sounds");
    if (saving && robotic) {
        SetTweak(*saving, (double)PreferenceSaving::Manual);
        SetTweak(*robotic, 0);
        batch.Next(L"Preference saving and robotic sounds are set from Preferences under their old settings keys");
        Check(g.preferenceSaving == PreferenceSaving::Manual && g.preferenceSavingChosen &&
                  IniRead(L"Suggestions", L"SavingMode", L"") == L"2" && !RoboticSounds() &&
                  IniRead(L"Window", L"RoboticSounds", L"") == L"0" && !wcscmp(TweakPage(*saving), L"Startup"),
              L"Preference saving and robotic sounds are set from Preferences, under their old settings.ini keys");
        SetTweak(*robotic, 1);
        SetTweak(*saving, (double)PreferenceSaving::Ask);
    } else {
        Check(false, L"Preferences lists Preference saving and robotic sounds");
    }

    const Tweak* pauseScrobbling = FindTweak("scrobbling.paused");
    const Tweak* nowPlaying = FindTweak("scrobbling.now_playing");
    if (Ready(pauseScrobbling && nowPlaying, L"Preferences exposes scrobbling controls")) {
        SetTweak(*pauseScrobbling, 1); SetTweak(*nowPlaying, 0);
        listenbrainz::SetPaused(false); listenbrainz::SetSendNowPlaying(true); LoadPreferences();
        batch.Next(L"Scrobbling choices persist and its setup window exposes all three services");
        Check(listenbrainz::Paused() && !listenbrainz::SendNowPlaying() &&
                  !wcscmp(TweakPage(*pauseScrobbling), L"Scrobbling"),
              L"Scrobbling pause and now-playing choices persist on their own Preferences page");
        SetTweak(*pauseScrobbling, 0); SetTweak(*nowPlaying, 1);
    }
    ShowListenBrainz();
    HWND scrobbling = FindWindowW(L"CordinalListenBrainz", nullptr);
    HWND services = GetDlgItem(scrobbling, 102);
    bool serviceUi = services && SendMessageW(services, CB_GETCOUNT, 0, 0) == 3 &&
        IsWindowVisible(GetDlgItem(scrobbling, 100)) && !IsWindowVisible(GetDlgItem(scrobbling, 103));
    for (int i : {1, 2}) {
        SendMessageW(services, CB_SETCURSEL, i, 0);
        SendMessageW(scrobbling, WM_COMMAND, MAKEWPARAM(102, CBN_SELCHANGE), (LPARAM)services);
        serviceUi = serviceUi && IsWindowVisible(GetDlgItem(scrobbling, 103)) &&
            IsWindowVisible(GetDlgItem(scrobbling, 104)) && !IsWindowVisible(GetDlgItem(scrobbling, 100)) &&
            (GetWindowLongW(GetDlgItem(scrobbling, 104), GWL_STYLE) & ES_PASSWORD);
    }
    Check(serviceUi, L"Scrobbling setup exposes all three services and switches between token and browser authentication");
    if (scrobbling) DestroyWindow(scrobbling);

    int effects = 0, nightRow = -1;
    for (const Tweak& t : Tweaks())
        if (!strncmp(t.key, "effects.", 8)) {
            if (!strcmp(t.key, "effects.night_mode")) nightRow = effects;
            ++effects;
        }
    ShowPreferences("effects.night_mode");
    HWND window = ProcessWindow(L"CordinalPreferences");
    HWND list = window ? FindWindowExW(window, nullptr, WC_LISTVIEWW, nullptr) : nullptr;
    HWND search = window ? FindWindowExW(window, nullptr, WC_EDITW, nullptr) : nullptr;
    HWND pages = window ? FindWindowExW(window, nullptr, WC_LISTBOXW, nullptr) : nullptr;
    auto choosePage = [&](int page) {
        SendMessageW(pages, LB_SETCURSEL, page, 0);
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(pages), LBN_SELCHANGE), (LPARAM)pages);
    };
    batch.Next(L"Preferences opens on the asked setting, lists every tweak and searches across all pages");
    Check(list && pages && ListView_GetItemCount(list) == effects &&
              ListView_GetNextItem(list, -1, LVNI_SELECTED) == nightRow,
          L"Preferences opens on the asked-for setting's page with that setting selected");
    if (pages) choosePage(0);
    const auto shownTweaks = std::count_if(Tweaks().begin(), Tweaks().end(), TweakShown);
    Check(list && ListView_GetItemCount(list) == (int)shownTweaks, L"All settings lists every tweak this build has");
    if (pages) choosePage(1);
    if (search) SetWindowTextW(search, L"night");
    Check(list && ListView_GetItemCount(list) == 1 && SendMessageW(pages, LB_GETCURSEL, 0, 0) == 0,
          L"a search from any page covers every page");
    Pump(5000, [] { return !g.rgRunning; });
    if (Ready(window && pages && !g.rgRunning, L"ReplayGain controls have an idle worker and an open Preferences window")) {
        const bool automatic = g.rgAutoScan;
        g.rgAutoScan = false;
        const std::wstring a = scratch + L"\\rg-controls-a.wav", b = scratch + L"\\rg-controls-b.wav";
        if (Ready(WriteTestBytes(a, MakeSineWav(1, 1)) && WriteTestBytes(b, MakeSineWav(1, 1)),
                  L"ReplayGain controls have two available library files")) {
            for (const auto& path : {a, b}) g_lib[g_lib.Add(path)].flags |= Track::kInLibrary;
            const auto targets = replaygain::LibraryTracks();
            const auto active = Active().items, selected = SelectedTracks();
            choosePage(3);
            HWND scans[3] = {GetDlgItem(window, 110), GetDlgItem(window, 111), GetDlgItem(window, 112)};
            HWND cancel = GetDlgItem(window, 113);
            batch.Next(L"ReplayGain Preferences scans the whole library and keeps its controls in step with the worker");
            Check(IsWindowVisible(scans[0]) && IsWindowVisible(scans[1]) && IsWindowVisible(scans[2]) &&
                  IsWindowEnabled(scans[2]) && !IsWindowVisible(cancel),
                  L"ReplayGain Preferences exposes selection, playlist and library scans with cancellation hidden while idle");
            SendMessageW(scans[2], BM_CLICK, 0, 0);
            MSG action{};
            bool posted = PeekMessageW(&action, g.hwnd, WM_COMMAND, WM_COMMAND, PM_REMOVE) &&
                          LOWORD(action.wParam) == IDM_RG_SCAN_ALL;
            if (posted) DispatchMessageW(&action);
            bool covers = posted && g.rgRunning && !g.rgAuto && g.rgRunIds.size() == targets.size();
            for (TrackId id : targets) covers &= g.rgRunIds.count(id) != 0;
            Check(covers && Active().items == active && SelectedTracks() == selected,
                  L"the real Scan library button queues every available library track without changing the playlist or selection");
            SendMessageW(window, WM_TIMER, 1, 0);
            Check(!IsWindowEnabled(scans[0]) && !IsWindowEnabled(scans[1]) && !IsWindowEnabled(scans[2]) &&
                  IsWindowVisible(cancel) && IsWindowEnabled(cancel),
                  L"a manual scan disables duplicate starts and exposes Cancel scan while Preferences stays open");
            RECT originalBounds;
            GetWindowRect(window, &originalBounds);
            bool fits = true;
            const UINT dpi = g.dpi;
            for (UINT scale : {dpi, dpi * 2}) {
                g.dpi = scale;
                SetWindowPos(window, nullptr, 0, 0, S(700), S(520), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                RECT client;
                GetClientRect(window, &client);
                int right = 0;
                for (HWND button : {scans[0], scans[1], scans[2], cancel}) {
                    RECT r;
                    GetWindowRect(button, &r);
                    MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&r), 2);
                    fits &= r.left >= right && r.right <= client.right && r.top >= 0 && r.bottom <= client.bottom;
                    right = r.right;
                }
            }
            g.dpi = dpi;
            SetWindowPos(window, nullptr, 0, 0, originalBounds.right - originalBounds.left,
                         originalBounds.bottom - originalBounds.top,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            Check(fits, L"ReplayGain scan and cancel buttons fit the minimum window at normal and double scaling");
            SendMessageW(cancel, BM_CLICK, 0, 0);
            if (PeekMessageW(&action, g.hwnd, WM_COMMAND, WM_COMMAND, PM_REMOVE)) DispatchMessageW(&action);
            Pump(10000, [] { return !g.rgRunning; });
            SendMessageW(window, WM_TIMER, 1, 0);
            Check(!g.rgRunning && !g.rgWorker.joinable() && !IsWindowVisible(cancel) && IsWindowEnabled(scans[2]),
                  L"Cancel scan finishes the worker and restores the scan buttons without closing Preferences");
        }
        g.rgAutoScan = automatic;
    }
    if (window) DestroyWindow(window);

    std::vector<std::wstring> texts = {L"Display mode", L"Play / Pause Playback", L"Repeat playlist Playback",
                                       L"Remove Edit"};
    const std::vector<std::wstring> ties{L"Replay", L"Play one", L"Can play", L"Display", L"PLAY two", L"More play"};
    batch.Next(L"the palette ranks matches, lists the menu bar's commands and runs the chosen one");
    Check(RankMatches(texts, L"play") == std::vector<int>{1, 2, 0} && RankMatches(texts, L"re pl") == std::vector<int>{2} &&
              RankMatches(ties, L" PLAY ") == std::vector<int>{1, 4, 2, 5, 0, 3} &&
              RankMatches(ties, L"") == std::vector<int>{0, 1, 2, 3, 4, 5} && RankMatches(ties, L"absent").empty(),
          L"the palette ranks prefixes, word starts and other matches, preserving ties and requiring every word");

    HMENU bar = MainMenuBar() ? nullptr : BuildMenu();  // the test window is created without one
    if (bar) SetMenu(g.hwnd, bar);
    ShowCommandPalette();
    HWND palette = FindWindowW(L"CordinalPalette", nullptr);
    HWND edit = palette ? FindWindowExW(palette, nullptr, WC_EDITW, nullptr) : nullptr;
    HWND items = palette ? FindWindowExW(palette, nullptr, WC_LISTBOXW, nullptr) : nullptr;
    const int all = items ? (int)SendMessageW(items, LB_GETCOUNT, 0, 0) : 0;
    if (edit) SetWindowTextW(edit, L"group tracks by album");
    Check(all > 80 && items && SendMessageW(items, LB_GETCOUNT, 0, 0) == 1,
          L"the palette lists the menu bar's commands and finds one by its words (%d listed)", all);
    if (edit) SendMessageW(edit, WM_KEYDOWN, VK_RETURN, 0);
    Pump(200, [] { return g.groupAlbums; });
    Check(!FindWindowW(L"CordinalPalette", nullptr) && g.groupAlbums, L"Enter closes the palette and runs the command");
    if (FindWindowW(L"CordinalPalette", nullptr)) DestroyWindow(FindWindowW(L"CordinalPalette", nullptr));
    ShowCommandPalette();
    palette = FindWindowW(L"CordinalPalette", nullptr);
    edit = palette ? FindWindowExW(palette, nullptr, WC_EDITW, nullptr) : nullptr;
    items = palette ? FindWindowExW(palette, nullptr, WC_LISTBOXW, nullptr) : nullptr;
    auto listed = [&](const wchar_t* words) {
        if (edit) SetWindowTextW(edit, words);
        return items ? (int)SendMessageW(items, LB_GETCOUNT, 0, 0) : 0;
    };
    batch.Next(L"the palette still finds settings that moved to Preferences and keeps existing pack shortcuts");
    Check(listed(L"skip-aware shuffle") == 1 && listed(L"File Scrobbling") == 1 &&
              listed(L"start with windows") == 1 && MenuCommandByName(L"Skip-aware shuffle") == IDM_SKIP_SHUFFLE &&
              MenuCommandByName(L"Web server") == IDM_WEB_SERVER,
          L"the palette still finds the settings and windows that moved from the menus to Preferences");
    Check(MenuCommandByName(L"Playback > Keep playing like this (radio)") == IDM_RADIO &&
              MenuCommandByName(L"Keep playing like this (radio)") == IDM_RADIO,
          L"existing pack shortcuts keep working after the playback-order command was renamed");
    if (palette) DestroyWindow(palette);

    // The menus keep actions and the toggles changed while a song plays: what they used to set is in
    // Preferences, and a cancel command shows only while its work runs.
    HMENU menus = BuildMenu(), view = GetSubMenu(menus, 5), extras = nullptr;
    for (int i = 0, n = GetMenuItemCount(view); i < n && !extras; ++i)
        if (HMENU sub = GetSubMenu(view, i); sub && Holds(sub, IDM_POINTS)) extras = sub;
    batch.Next(L"menus hold actions while settings live in Preferences, each with a row or a window there");
    Check(GetMenuItemCount(menus) == (int)std::size(ui::kMenuBar) && extras && Holds(extras, IDM_GAME_NAME_THAT_TRACK) &&
              Holds(extras, IDM_GAME_WHICH_CAME_FIRST) && Holds(extras, IDM_REVIEW),
          L"no Games menu: the games, points and Year in review share one View submenu");
    Check(Holds(GetSubMenu(menus, 0), IDM_WEB_SERVER) && !Holds(GetSubMenu(menus, 4), IDM_WEB_SERVER) &&
              Holds(GetSubMenu(menus, 0), IDM_LISTENBRAINZ) && !Holds(GetSubMenu(menus, 4), IDM_LISTENBRAINZ),
          L"Web server and Scrobbling are reachable from the File menu, not the Library menu");
    auto inPreferences = [](int command) {
        for (const Tweak& t : Tweaks())
            if (t.command == command) return true;
        for (const PreferenceLink& link : PreferenceLinks())
            if (link.command == command) return true;
        return false;
    };
    Check(inPreferences(IDM_LISTENBRAINZ), L"Scrobbling remains available in Preferences");
    Check(Holds(menus, IDM_RG_SCAN_ALL) && inPreferences(IDM_RG_SCAN_ALL) &&
          inPreferences(IDM_RG_SCAN_SEL) && inPreferences(IDM_RG_SCAN_PL),
          L"ReplayGain scanning is reachable from both Playback and Preferences");
    bool moved = !Holds(menus, IDM_OUTPUT_EXCLUSIVE) && !Holds(menus, IDM_PREFERENCE_MANUAL);
    for (int id : {IDM_SKIP_SHUFFLE, IDM_RESUME, IDM_OUTPUT_SHARED, IDM_LOOPS_BASE, IDM_RG_AUTO_SCAN, IDM_RG_ESTIMATE,
                   IDM_LOUDNESS_COMP, IDM_SOUND_EFFECTS, IDM_TASKBAR_PROGRESS, IDM_START_WITH_WINDOWS, IDM_THEME_ROBOTIC,
                   IDM_PREFERENCE_ASK, IDM_STARTUP_DEFAULTS, IDM_FORMAT, IDM_CONVERT_RECYCLE, IDM_CONVERT_SHOW_ORIGINALS,
                   IDM_CONVERT_MEMORY_BASE})
        moved = moved && !Holds(menus, id) && inPreferences(id);
    Check(moved, L"settings left the menus for Preferences, each with a row or a window there");
    // Waits out any import or scan an earlier check started, then stands in for one.
    Pump(5000, [] { return !g.importRunning && !g.rgRunning; });
    const bool idle = !g.importRunning && !g.rgRunning, automatic = g.rgAuto;
    UpdateMainMenu(menus);
    bool comesAndGoes = idle && !Holds(menus, IDM_CANCEL_IMPORT) && !Holds(menus, IDM_RG_CANCEL);
    if (idle) {
        g.importRunning = true;
        g.rgRunning = true;
        g.rgAuto = false;
        UpdateMainMenu(menus);
        HMENU file = GetSubMenu(menus, 0);
        int folder = -1;
        for (int i = 0, n = GetMenuItemCount(file); i < n; ++i)
            if (GetMenuItemID(file, i) == IDM_ADD_FOLDER) folder = i;
        comesAndGoes = comesAndGoes && folder >= 0 && GetMenuItemID(file, folder + 1) == IDM_CANCEL_IMPORT &&
                       Holds(menus, IDM_RG_CANCEL);
        g.importRunning = false;
        g.rgRunning = false;
        g.rgAuto = automatic;
        UpdateMainMenu(menus);
        comesAndGoes = comesAndGoes && !Holds(menus, IDM_CANCEL_IMPORT) && !Holds(menus, IDM_RG_CANCEL);
    }
    batch.Next(L"Cancel import and Cancel scan are in the menus only while an import or a scan runs");
    Check(comesAndGoes, L"Cancel import and Cancel scan are in the menus only while an import or a scan runs");
    DestroyMenu(menus);
    if (bar) {
        SetMenu(g.hwnd, nullptr);
        DestroyMenu(bar);
    }

    g.iniPath = original;
    LoadPreferences();
    RefreshViewKeepingSelection();
    DeleteFileW(profile.c_str());
}

// Every theme stays legible (PRINCIPLES.md): PaletteLegibility holds body text to WCAG AAA, secondary
// and selected text to AA, and the accent and status colors to the 3:1 that UI marks need, on the
// surfaces they sit on; pack themes meet the same function. A theme's system face reaches the UI
// font and leaves with the theme.
void TestThemes() {
    using namespace app;
    testlog::Group group(L"themes");
    testlog::Batch batch;
    std::wstring failures;
    for (int t = 0; t < (int)ThemeMode::Count; ++t) {
        const Theme& theme = ThemeInfo((ThemeMode)t);
        const Palette* palettes[] = {&LightPalette(), &DarkPalette()};
        for (const Palette* p : theme.palette ? std::vector<const Palette*>{theme.palette} : std::vector(palettes, palettes + 2))
            if (!PaletteLegibility(*p).empty()) failures += std::wstring(failures.empty() ? L"" : L", ") + theme.name;
    }
    if (!failures.empty()) failures = L" (fails: " + failures + L")";
    batch.Next(L"every theme palette and the accent keep text, selection and markers legible");
    Check(failures.empty(), L"every theme palette keeps text, selection and accents legible%ls", failures.c_str());
    Check(TextOn(0xFFFF00) == 0x000000 && TextOn(0xC8303A) == 0xFFFFFF && Contrast(0x000000, 0xFFFFFF) > 20.9,
          L"text on the accent is black on yellow and white on cardinal red");
    const Palette& foobar = ThemePalette(ThemeMode::Foobar);
    Check(foobar.bg == 0 && foobar.surface == 0x202020 && foobar.text == 0x00DD00 && foobar.playing == foobar.text &&
              TextOn(foobar.playing) == 0,
          L"foobar2000 keeps the reference's black plots, gray chrome and green trace and playback fill");

    // The player's background takes the cover's hue and the list stripes every other row, in Light and
    // Dark only and never at the cost of legibility.
    auto cover = [](std::initializer_list<Rgb> colors) {
        std::vector<uint8_t> px;
        for (Rgb c : colors)
            for (int i = 0; i < 100; ++i) px.insert(px.end(), {(uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c, 255});
        return CoverHue(px.data(), px.size() / 4, false);
    };
    double red = cover({0xC02020, 0x101010, 0x101010}), blue = cover({0x2040D0, 0x3050E0, 0xD0D0D0}), gray = cover({0x808080, 0x202020});
    batch.Next(L"a cover's hue is its vivid pixels' and a gray cover has none");
    Check(red >= 0 && (red < 10 || red > 350) && blue > 220 && blue < 235 && gray < 0,
          L"a cover's hue is its vivid pixels' (red %.0f, blue %.0f) and a gray cover has none (%.0f)", red, blue, gray);
    bool legible = true, tinted = true;
    for (const Palette* p : {&LightPalette(), &DarkPalette()}) {
        for (double hue = 0; hue < 360; hue += 15) {
            Rgb s = PlayerSurface(*p, hue);
            legible = legible && Contrast(p->text, s) >= 7 && Contrast(p->sub, s) >= 4.5 && Contrast(p->accent, s) >= 3;
            tinted = tinted && s != PlayerSurface(*p, -1);
        }
        Rgb stripe = RowStripe(*p);
        legible = legible && stripe != p->bg && Contrast(p->text, stripe) >= 7 && Contrast(p->sub, stripe) >= 4.5 &&
                  Contrast(p->playing, stripe) >= 3 && Contrast(p->faint, stripe) >= Contrast(p->faint, p->bg) * 0.9;
    }
    batch.Next(L"the player's tint and the row stripes keep Light and Dark legible, and other themes keep their own");
    Check(legible && tinted && PlayerSurface(foobar, 0) == foobar.surface && RowStripe(foobar) == foobar.bg,
          L"the player's tint and the row stripes keep Light and Dark legible, and other themes keep their own");
    batch.Next(L"a release offers Light, Dark, Apollo and the system theme; a saved theme it lacks opens as Light or Dark");
    bool offered = true;
    for (ThemeMode t : {ThemeMode::System, ThemeMode::Light, ThemeMode::Dark, ThemeMode::Apollo})
        offered = offered && ThemeAvailable(t) && AvailableTheme(t) == t;
    for (ThemeMode t : {ThemeMode::Deltarune, ThemeMode::Foobar, ThemeMode::Paper, ThemeMode::Midnight, ThemeMode::Terminal})
        offered = offered && (kExtraThemes ? AvailableTheme(t) == t
                                           : !ThemeAvailable(t) && AvailableTheme(t) == (ThemeInfo(t).palette->dark ? ThemeMode::Dark : ThemeMode::Light));
    Check(offered, L"a release offers Light, Dark, Apollo and the system theme; a saved theme it lacks opens as Light or Dark");

    const ThemeMode original = g.themeMode;
    auto face = [] {
        LOGFONTW lf{};
        GetObjectW(g.font, sizeof(lf), &lf);
        return std::wstring(lf.lfFaceName);
    };
    OnCommand(IDM_THEME_BASE + (int)ThemeMode::Dark, 0, nullptr);
    if (!kExtraThemes) {
        testlog::Skip(L"the themes' own faces: this build leaves out the extra themes");
    } else {
        const std::wstring system = face();
        bool fonts = true;
        for (ThemeMode t : {ThemeMode::Paper, ThemeMode::Midnight, ThemeMode::Terminal}) {
            OnCommand(IDM_THEME_BASE + (int)t, 0, nullptr);
            fonts = fonts && g.themeMode == t && face() == ThemeInfo(t).font && g.pal == ThemeInfo(t).palette;
        }
        OnCommand(IDM_THEME_BASE + (int)ThemeMode::Dark, 0, nullptr);
        batch.Next(L"Paper, Midnight and Terminal bring their own face and Dark restores the system one");
        Check(fonts && face() == system, L"Paper, Midnight and Terminal bring their own face and Dark restores the system one");
    }

    // Apollo's window in the normal size: no menu bar, Apollo's columns under its own headings, no
    // album groups and no album canvas; leaving it brings the menu bar and Cordinal's columns back.
    if (g.size == ViewSize::Normal && g.hwnd) {
        bool hadMenu = GetMenu(g.hwnd) != nullptr;
        OnCommand(IDM_THEME_BASE + (int)ThemeMode::Apollo, 0, nullptr);
        wchar_t heading[16] = {};
        HDITEMW item{};
        item.mask = HDI_TEXT;
        item.pszText = heading;
        item.cchTextMax = (int)std::size(heading);
        Header_GetItem(g.header, 0, &item);
        bool apollo = ApolloMode() && !GetMenu(g.hwnd) && g.columns.size() == 8 && g.columns[1] == kColApolloTitle &&
                      std::wstring(heading) == L"No" && !GroupingActive() && !CanvasActive() && face() == L"Tahoma";
        OnCommand(IDM_THEME_BASE + (int)ThemeMode::Dark, 0, nullptr);
        batch.Next(L"the Apollo theme arranges Apollo's window and leaving it restores Cordinal's");
        Check(apollo && !ApolloMode() && (GetMenu(g.hwnd) != nullptr) == hadMenu &&
                  (g.columns.size() < 2 || g.columns[1] != kColApolloTitle),
              L"the Apollo theme arranges Apollo's window and leaving it restores Cordinal's");
    } else {
        testlog::Skip(L"the Apollo theme's window needs the normal window size");
    }
    OnCommand(IDM_THEME_BASE + (int)original, 0, nullptr);
}
}  // namespace

void TestPersonalization(const std::wstring& scratch) {
    using namespace app;
    testlog::Group group(L"personalization across restarts");
    testlog::Batch batch;
    // A theme other than the default Light, among those this build has.
    const ThemeMode chosen = kExtraThemes ? ThemeMode::Deltarune : ThemeMode::System;
    const std::wstring original = g.iniPath;
    const std::wstring profile = scratch + L"\\preferences-test.ini";
    const std::wstring empty = scratch + L"\\preferences-defaults.ini";
    WriteTestBytes(profile, "\xFF\xFE");
    WriteTestBytes(empty, "\xFF\xFE");
    auto reload = [&] {
        LoadPreferences();
        LoadBrowserSettings();
        LoadColumns();
        SetupColumns();
    };
    g.iniPath = profile;
    reload();
    ResetPreferenceSuggestions();
    batch.Next(L"a new profile starts with the built-in personalization defaults");
    Check(g.layout == MainLayout{} && g.volumePercent == 80 && g.browserVisible &&
              g.libMode == LibraryMode::Albums && g.columns == kDefaultColumns &&
              g.defaultSize == ViewSize::Normal && !g.defaultMiniPlayer,
          L"a new profile starts with the built-in personalization defaults");

    OnCommand(IDM_LAYOUT_DOCK_BASE + (int)Dock::Left, 0, nullptr);
    OnCommand(IDM_LAYOUT_WAVE_BASE + 2, 0, nullptr);
    OnCommand(IDM_LAYOUT_WAVE_ROW, 0, nullptr);
    OnCommand(IDM_LAYOUT_PANELS_LEFT, 0, nullptr);
    OnCommand(IDM_LAYOUT_PART_BASE + 8, 0, nullptr);  // playlist tabs
    OnCommand(IDM_LAYOUT_PART_BASE + 11, 0, nullptr);  // up next
    MainLayout expected = g.layout;
    OnCommand(IDM_THEME_BASE + (int)chosen, 0, nullptr);
    OnCommand(IDM_SHUFFLE_ALBUMS, 0, nullptr);
    OnCommand(IDM_SKIP_SHUFFLE, 0, nullptr);
    OnCommand(IDM_RADIO, 0, nullptr);
    OnCommand(IDM_REPEAT_BASE + (int)Repeat::One, 0, nullptr);
    OnCommand(IDC_TIME, STN_CLICKED, g.time);
    OnCommand(IDM_GROUP_ALBUMS, 0, nullptr);
    OnCommand(IDM_ALWAYS_ON_TOP, 0, nullptr);
    OnCommand(IDM_TASKBAR_PROGRESS, 0, nullptr);
    OnCommand(IDM_RG_ENABLE, 0, nullptr);
    OnCommand(IDM_RG_AUTO_SCAN, 0, nullptr);
    if (const Tweak* preamp = FindTweak("replaygain.preamp")) SetTweak(*preamp, 3);
    if (const Tweak* mode = FindTweak("replaygain.mode")) SetTweak(*mode, 2);
    OnCommand(IDM_LOOPS_BASE + 3, 0, nullptr);
    OnCommand(IDM_REVERB_BASE + (int)ReverbPreset::Hall, 0, nullptr);
    OnCommand(IDM_REVERB_MIX_BASE + 2, 0, nullptr);
    OnCommand(IDM_LOUDNESS_COMP, 0, nullptr);
    OnCommand(IDM_NIGHT_MODE, 0, nullptr);
    OnCommand(IDM_BITCRUSH, 0, nullptr);
    if (const Tweak* bits = FindTweak("effects.bitcrush_bits")) SetTweak(*bits, 5 - kBitcrushMinBits);
    if (const Tweak* crushRate = FindTweak("effects.bitcrush_rate")) SetTweak(*crushRate, 1);
    if (const Tweak* crushMix = FindTweak("effects.bitcrush_mix")) SetTweak(*crushMix, 0.65);
    OnCommand(IDM_RG_ESTIMATE, 0, nullptr);
    OnCommand(IDM_CONVERT_RECYCLE, 0, nullptr);
    OnCommand(IDM_CONVERT_SHOW_ORIGINALS, 0, nullptr);
    g.convertMemory = 1;  // set directly: the menu command also prunes the conversion logs
    SetVolumePercent(37);
    ToggleMute();
    SetLibraryMode(LibraryMode::Tracks);
    ToggleBrowser();
    g.browserHeight = 213;
    g.coverSize = 200;
    g.panes[0].field = BrowseField::Composer;
    g.panes[1].field = BrowseField::Folder;
    SaveBrowserSettings();
    ToggleColumn(kColRating);
    ListView_SetColumnWidth(g.list, SubitemOf(2), S(333));
    std::vector<int> order(g.columns.size());
    for (int i = 0; i < (int)order.size(); ++i) order[i] = (int)order.size() - 1 - i;
    ListView_SetColumnOrderArray(g.list, (int)order.size(), order.data());
    SendMessageW(g.header, WM_LBUTTONUP, 0, MAKELPARAM(-1, -1));
    g.eq.enabled = true;
    g.eq.preamp = -2.5f;
    g.eq.gains[2] = 4.5f;
    std::wstring eqError;
    ParseAutoEq("Preamp: -2.5 dB\nFilter 1: ON LSC Fc 105 Hz Gain 5.5 dB Q 0.71\n"
                "Filter 2: OFF PK Fc 1234.5 Hz Gain -3.25 dB Q 2.125", g.eq, eqError);
    g.eq.crossfeed = true;
    SaveEq();
    SetViewSize(ViewSize::Compact);
    ULONGLONG changed = GetTickCount64();
    ObservePreferenceChanges(changed);
    batch.Next(L"presentation changes collect into one review after five settled minutes");
    Check(PendingStartupDefaults() != 0 && (PendingStartupDefaults() & kDefaultCoverSize) && (PendingStartupDefaults() & kDefaultBrowserHeight) &&
              PendingStartupDefaults() == kAllStartupDefaults && !PreferenceSuggestionsDue(changed + 20000) && PreferenceSuggestionsDue(changed + 300000),
          L"presentation changes collect into one review after five settled minutes (pending %u)", PendingStartupDefaults());
    StartupPreferences choices = CaptureStartupPreferences();
    SaveStartupDefaults(choices, 0);
    SaveViewSize();
    SavePreferences();
    SaveBrowserSettings();
    batch.Next(L"unchecked choices retain approved defaults through all session-save paths");
    Check(g.defaultSize == ViewSize::Normal && IniInt(L"Window", L"DefaultSize", -1) == (int)ViewSize::Normal &&
              IniInt(L"Window", L"Theme", -1) == (int)ThemeMode::Light &&
              IniInt(L"Library", L"View", -1) == (int)LibraryMode::Albums &&
              IniInt(L"Layout", L"Player", -1) == (int)Dock::Top,
          L"unchecked choices retain approved defaults through all session-save paths");
    g.browserVisible = true;
    ObservePreferenceChanges(changed + 200000);
    batch.Next(L"later changes restart the settling delay and reverted choices leave the batch");
    Check(!PreferenceSuggestionsDue(changed + 300000) && PreferenceSuggestionsDue(changed + 500000) &&
              !(PendingStartupDefaults() & kDefaultBrowser),
          L"later changes restart the settling delay and reverted choices leave the batch");
    g.browserVisible = false;
    SaveStartupDefaults(choices, kDefaultWindowSize | kDefaultTheme);
    batch.Next(L"saving from a review writes only checked defaults, keeps its snapshot and saves explicit preferences");
    Check(g.defaultSize == ViewSize::Compact && g.defaultTheme == chosen &&
              g.defaultLibraryMode == LibraryMode::Albums && g.defaultBrowserVisible && g.defaultLayout == MainLayout{} &&
              g.libMode == LibraryMode::Tracks && !g.browserVisible && g.layout == expected,
          L"selecting some defaults leaves unchecked defaults and the current session untouched");
    g.themeMode = ThemeMode::Dark;
    SaveStartupDefaults(choices, kDefaultTheme);
    Check(g.defaultTheme == chosen && g.themeMode == ThemeMode::Dark,
          L"a review saves its displayed snapshot even if the live choice changes later");
    g.themeMode = choices.theme;
    SaveStartupDefaults(choices, kAllStartupDefaults);
    Check(g.defaultCoverSize == 200 && g.defaultBrowserHeight == 213 && IniInt(L"Library", L"CoverSize", -1) == 200 &&
              IniInt(L"Library", L"BrowserHeight", -1) == 213 && g.coverSize == 200 && g.browserHeight == 213,
          L"cover size and browser height are offered as startup defaults and survive saving");
    Check(IniInt(L"Layout", L"Player", -1) == (int)Dock::Left &&
              IniInt(L"Window", L"Theme", -1) == (int)chosen &&
              IniInt(L"Player", L"Volume", -1) == 0 && IniInt(L"Player", L"LastAudibleVolume", -1) == 37 &&
              IniInt(L"Window", L"DefaultSize", -1) == (int)ViewSize::Compact,
          L"selected startup defaults and explicit playback preferences save immediately");

    // Discard the in-memory choices, then load the saved profile as a new launch does.
    g.iniPath = empty;
    reload();
    batch.Next(L"missing settings use defaults rather than stale choices, and older profiles keep their size");
    Check(g.layout == MainLayout{} && !g.eq.enabled && g.eq.gains[2] == 0 && g.volumePercent == 80 && !g.shuffle &&
              !g.eq.crossfeed && !g.eq.parametric && !g.eq.filterCount && !g.skipAwareShuffle && !g.radio &&
              g.reverb == ReverbSettings{} && g.bitcrush == BitcrushSettings{} && !g.loudnessComp && !g.nightMode &&
              g.rgEstimate && g.convertRecycle &&
              !g.convertShowOriginals && g.convertMemory == 3,
          L"missing settings use defaults rather than stale choices from a previous profile");
    IniWrite(L"Window", L"Size", std::to_wstring((int)ViewSize::Spotlight));
    reload();
    Check(g.defaultSize == ViewSize::Spotlight && !g.defaultMiniPlayer,
          L"an older profile keeps its saved size until a different default is approved");
    g.iniPath = profile;
    reload();
    batch.Next(L"player position, layout, theme, playback, ReplayGain, equalizer, reverb and conversion "
               L"choices survive reloading");
    Check(g.layout == expected && g.themeMode == chosen && g.shuffle == kShuffleAlbums && g.repeat == Repeat::One &&
              !g.timeRemaining && !g.groupAlbums && g.alwaysOnTop && !g.taskbarProgress && g.loops == 3,
          L"player position, layout parts, theme and playback/display choices survive reloading");
    Check(!g.rgOn && !g.rgAutoScan && g.rgPreamp == 3 && g.rgMode == ReplayGainMode::PlaybackOrder && g.eq.enabled && g.eq.preamp == -2.5f && g.eq.gains[2] == 4.5f,
          L"ReplayGain and equalizer choices survive reloading");
    Check(g.reverb.preset == ReverbPreset::Hall && std::fabs(g.reverb.mix - kReverbMixes[2]) < 1e-4f && g.loudnessComp && g.nightMode &&
              !g.rgEstimate,
          L"reverb, loudness compensation, night mode and the quick estimate choice survive reloading");
    Check(g.bitcrush.enabled && g.bitcrush.bits == 5 && g.bitcrush.rate == kBitcrushRates[1] &&
              std::fabs(g.bitcrush.mix - 0.65f) < 1e-4f,
          L"bitcrush and its depth, rate and mix survive reloading (%d, %d bits, %.0f Hz, %.2f)", g.bitcrush.enabled,
          g.bitcrush.bits, g.bitcrush.rate, g.bitcrush.mix);
    Check(!g.convertRecycle && g.convertShowOriginals && g.convertMemory == 1,
          L"conversion options (Recycle Bin, kept originals shown, undo memory) survive reloading");
    Check(g.skipAwareShuffle && g.radio && g.eq.parametric && g.eq.crossfeed && g.eq.filterCount == 2 &&
              g.eq.filters[0].type == EqFilterType::LowShelf && g.eq.filters[1].frequency == 1234.5f &&
              g.eq.filters[1].q == 2.125f && !g.eq.filters[1].enabled,
          L"skip-aware shuffle, radio, crossfeed and exact parametric filters survive restart");
    {
        EqSettings savedEq = g.eq;
        WNDCLASSW eqClass{};
        eqClass.hInstance = g.inst;
        eqClass.lpszClassName = kEqClass;
        eqClass.lpfnWndProc = EqProc;
        RegisterClassW(&eqClass);
        WNDCLASSW embeddedHeadphones{};
        embeddedHeadphones.hInstance = g.inst;
        embeddedHeadphones.lpszClassName = kHeadphonesClass;
        embeddedHeadphones.lpfnWndProc = HeadphonesProc;
        RegisterClassW(&embeddedHeadphones);
        g.eqWnd = CreateWindowW(kEqClass, L"EQ test", WS_POPUP, 0, 0, S(740), S(590), g.hwnd, nullptr, g.inst, nullptr);
        Ready(g.eqWnd != nullptr, L"the native parametric EQ editor is created");
        if (g.eqWnd) {
            bool fits = true;
            RECT client{};
            GetClientRect(g.eqWnd, &client);
            for (int id : {IDC_EQ_PARAMETRIC, IDC_EQ_CROSSFEED, IDC_EQ_IMPORT, IDC_EQ_APPLY, IDC_EQ_TEXT, IDC_EQ_HINT}) {
                RECT control{};
                GetWindowRect(GetDlgItem(g.eqWnd, id), &control);
                MapWindowPoints(nullptr, g.eqWnd, (POINT*)&control, 2);
                fits &= control.left >= 0 && control.top >= 0 && control.right <= client.right &&
                        control.bottom <= client.bottom && control.right > control.left && control.bottom > control.top;
            }
            batch.Next(L"headphone EQ controls and instructions fit inside the native window at the active DPI");
            Check(fits, L"headphone EQ controls fit inside the native window");
            HWND hint = GetDlgItem(g.eqWnd, IDC_EQ_HINT);
            wchar_t hintText[512];
            GetWindowTextW(hint, hintText, (int)std::size(hintText));
            RECT hintClient{}, measured{};
            GetClientRect(hint, &hintClient);
            measured.right = hintClient.right;
            HDC hintDc = GetDC(hint);
            HGDIOBJ oldFont = SelectObject(hintDc, (HFONT)SendMessageW(hint, WM_GETFONT, 0, 0));
            DrawTextW(hintDc, hintText, -1, &measured, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
            SelectObject(hintDc, oldFont);
            ReleaseDC(hint, hintDc);
            Check(measured.bottom <= hintClient.bottom, L"EQ instructions fit without clipping at the active DPI and font");
            HWND editor = GetDlgItem(g.eqWnd, IDC_EQ_TEXT);
            SetWindowTextW(editor, L"Preamp: -4 dB\r\nFilter 1: ON PK Fc 2500 Hz Gain -2 dB Q 3");
            const float beforeSwitch = g.eq.preamp;
            const int hardwarePreset = g.headset.preset;
            SendDlgItemMessageW(g.eqWnd, IDC_EQ_HEADPHONE_PAGE, BM_CLICK, 0, 0);
            HWND embeddedPage = g.headphonesWnd;
            batch.Next(L"Headphone EQ switches to embedded controls, keeps both drafts and switches back with Ctrl+Tab");
            Check(embeddedPage && GetParent(embeddedPage) == g.eqWnd &&
                  (GetWindowLongPtrW(embeddedPage, GWL_STYLE) & WS_VISIBLE) &&
                  !(GetWindowLongPtrW(editor, GWL_STYLE) & WS_VISIBLE),
                  L"Headphone EQ switches to controls embedded in the same EQ window");
            if (embeddedPage) {
                SetDlgItemTextW(embeddedPage, IDC_EQ_IDLE, L"45");
                SendDlgItemMessageW(g.eqWnd, IDC_EQ_CORDINAL_PAGE, BM_CLICK, 0, 0);
                wchar_t draft[256];
                GetWindowTextW(editor, draft, (int)std::size(draft));
                bool draftKept = wcscmp(draft, L"Preamp: -4 dB\r\nFilter 1: ON PK Fc 2500 Hz Gain -2 dB Q 3") == 0;
                SendDlgItemMessageW(g.eqWnd, IDC_EQ_HEADPHONE_PAGE, BM_CLICK, 0, 0);
                wchar_t minutes[16];
                GetDlgItemTextW(embeddedPage, IDC_EQ_IDLE, minutes, (int)std::size(minutes));
                Check(draftKept && wcscmp(minutes, L"45") == 0 && g.headphonesWnd == embeddedPage &&
                      g.eq.preamp == beforeSwitch && g.headset.preset == hardwarePreset,
                      L"page switches retain both drafts and leave cordinal and hardware sound settings unchanged");
                BYTE oldKeys[256], controlKeys[256]{};
                GetKeyboardState(oldKeys);
                controlKeys[VK_CONTROL] = 0x80;
                SetKeyboardState(controlKeys);
                MSG key{};
                key.hwnd = GetDlgItem(embeddedPage, IDC_EQ_IDLE);
                key.message = WM_KEYDOWN;
                key.wParam = VK_TAB;
                bool handled = EqualizerDialogMessage(&key);
                SetKeyboardState(oldKeys);
                Check(handled && Button_GetCheck(GetDlgItem(g.eqWnd, IDC_EQ_CORDINAL_PAGE)) == BST_CHECKED &&
                      (GetWindowLongPtrW(editor, GWL_STYLE) & WS_VISIBLE),
                      L"Ctrl+Tab switches EQ pages from a nested headphone control");
            }
            SelectEqPage(false);
            SendMessageW(g.eqWnd, WM_COMMAND, IDC_EQ_APPLY, 0);
            batch.Next(L"Apply, crossfeed, Reset and the preamp slider drive the graphic and parametric settings");
            Check(g.eq.parametric && g.eq.preamp == -4 && g.eq.filterCount == 1 && g.eq.filters[0].frequency == 2500 &&
                      !IsWindowEnabled(GetDlgItem(g.eqWnd, IDC_EQ_SLIDER0 + 1)),
                  L"Apply profile updates actual settings and disables graphic band editing");
            SendMessageW(GetDlgItem(g.eqWnd, IDC_EQ_CROSSFEED), BM_CLICK, 0, 0);
            Check(!g.eq.crossfeed && g.eq.parametric, L"crossfeed toggles independently of parametric EQ");
            SendMessageW(g.eqWnd, WM_COMMAND, IDC_EQ_RESET, 0);
            Check(!g.eq.parametric && !g.eq.filterCount && g.eq.preamp == 0 &&
                      IsWindowEnabled(GetDlgItem(g.eqWnd, IDC_EQ_SLIDER0 + 1)),
                  L"Reset returns the editor to flat graphic EQ");
            g.eq.enabled = false;
            HWND preamp = GetDlgItem(g.eqWnd, IDC_EQ_SLIDER0);
            SendMessageW(preamp, TBM_SETPOS, TRUE, 60);
            SendMessageW(g.eqWnd, WM_VSCROLL, TB_ENDTRACK, (LPARAM)preamp);
            Check(g.eq.enabled && g.eq.preamp == -6 &&
                  Button_GetCheck(GetDlgItem(g.eqWnd, IDC_EQ_ENABLE)) == BST_CHECKED,
                  L"moving the preamp slider activates EQ and updates the enable control");
            // The curve: a press in graphic mode sets the nearest band; in parametric mode a double-click
            // adds a peak there, the wheel narrows it and Delete removes it again.
            HWND graph = GetDlgItem(g.eqWnd, IDC_EQ_GRAPH);
            RECT plot{};
            GetClientRect(graph, &plot);
            const int midX = (int)(S(34) + (plot.right - S(42)) * 0.5), topY = S(6) + 2;  // about 632 Hz, near +18 dB
            SendMessageW(graph, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(midX, topY));
            SendMessageW(graph, WM_LBUTTONUP, 0, MAKELPARAM(midX, topY));
            const bool bandSet = g.eq.gains[4] == 12 && g.eq.gains[5] == 0;
            SendMessageW(GetDlgItem(g.eqWnd, IDC_EQ_RESET), BM_CLICK, 0, 0);
            g.eq.parametric = true;
            g.eq.filterCount = 0;
            EqApply();
            SendMessageW(graph, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(midX, topY));
            const bool added = g.eq.filterCount == 1 && std::fabs(g.eq.filters[0].frequency - 632) < 8 &&
                               g.eq.filters[0].gain > 17;
            SendMessageW(graph, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), 0);
            const bool narrowed = g.eq.filters[0].q > kGraphicQ;
            SendMessageW(graph, WM_KEYDOWN, VK_DELETE, 0);
            batch.Next(L"the EQ curve sets bands and adds, reshapes and deletes filters; closing drops the headphone page");
            Check(bandSet && added && narrowed && g.eq.filterCount == 0,
                  L"the EQ curve sets graphic bands, and adds, reshapes and deletes parametric filters");
            DestroyWindow(g.eqWnd);
            Check(!g.headphonesWnd, L"closing the EQ window also destroys its embedded headphone page");
        }
        g.eq = savedEq;
        SaveEq();
        g.engine.SetEq({});
    }
    batch.Next(L"a muted restart retains the previous audible volume and unmuting restores the chosen one");
    Check(g.volumePercent == 0 && g.lastAudibleVolume == 37, L"a muted restart retains the previous audible volume");
    ToggleMute();
    Check(g.volumePercent == 37, L"unmuting after a restart restores the chosen volume");
    std::vector<int> restoredOrder(g.columns.size());
    ListView_GetColumnOrderArray(g.list, (int)restoredOrder.size(), restoredOrder.data());
    batch.Next(L"library view, browser fields/height and column visibility/width/order survive reloading");
    Check(g.libMode == LibraryMode::Tracks && !g.browserVisible && g.browserHeight == 213 &&
              g.panes[0].field == BrowseField::Composer && g.panes[1].field == BrowseField::Folder &&
              SubitemOf(kColRating) < 0 &&
              ListView_GetColumnWidth(g.list, SubitemOf(2)) == S(333) && restoredOrder == order,
          L"library view, browser fields/height and column visibility/width/order survive reloading");
    RestoreWindowPlacement(SW_HIDE);
    batch.Next(L"the saved compact size and reduced memory policy are restored without a new suggestion");
    Check(g.size == ViewSize::Compact && ReducedMemoryMode(),
          L"the saved compact size and reduced memory policy are restored on launch");
    Check(!PreferenceSuggestionsDue(GetTickCount64() + 300000),
          L"startup restoration does not suggest an already approved size");
    testlog::Cases approved;
    for (ViewSize size : {ViewSize::Compact, ViewSize::Normal, ViewSize::Spotlight, ViewSize::Big}) {
        SetViewSize(size);
        SaveStartupDefaults(CaptureStartupPreferences(), kDefaultWindowSize);
        SetViewSize(size == ViewSize::Normal ? ViewSize::Compact : ViewSize::Normal);
        SaveViewSize();
        SavePreferences();
        reload();
        RestoreWindowPlacement(SW_HIDE);
        approved.Add(g.size == size && !g.defaultMiniPlayer, L"size " + std::to_wstring((int)size));
    }
    batch.Next(L"each approved startup size survives temporary size changes and profile reloads");
    Check(approved.Passed(), L"each approved startup size survives temporary size changes and profile reloads%ls",
          approved.failed.c_str());
    batch.End();
    TestPreferenceSavingModes(scratch);
    TestTweaks(scratch);

    SetViewSize(ViewSize::Normal);
    g.iniPath = empty;
    reload();
    if (!kExtraThemes) {
        testlog::Skip(L"the DELTARUNE fountain: this build leaves out the extra themes");
    } else {
        g.themeMode = ThemeMode::Deltarune;
        ApplyTheme();
        OnCommand(IDM_WAVE_STYLE_BASE + 2, 0, nullptr);  // Dark Fountain
        TrackId savedPlaying = g.playing;
        g.playing = g_lib.Count() ? 0 : kNone;
        bool themeScoped = true;
        for (int style : {1, 2}) {  // Fountain bars and Dark Fountain
            OnCommand(IDM_WAVE_STYLE_BASE + style, 0, nullptr);
            for (ThemeMode theme : {ThemeMode::System, ThemeMode::Light, ThemeMode::Dark, ThemeMode::Deltarune}) {
                OnCommand(IDM_THEME_BASE + (int)theme, 0, nullptr);
                for (int dock = 0; dock < (int)Dock::Count; ++dock) {
                    g.layout.player = (Dock)dock;
                    themeScoped = themeScoped && FountainShown() == (theme == ThemeMode::Deltarune);
                }
                // Switching away and back must preserve the user's chosen fountain style.
                themeScoped = themeScoped && IniInt(L"Waveform", L"Left", -1) == style;
            }
        }
        batch.Next(L"fountain styles appear only in DELTARUNE at every edge and survive theme switches");
        Check(themeScoped, L"fountain styles appear only in DELTARUNE at every edge and survive theme switches");
        bool savedLiveWave = g.liveWaveform, savedLiveFountain = g.liveFountain;
        g.liveWaveform = true; g.liveFountain = false;
        bool bars = !FountainShown();
        SendMessageW(g.seek, TBM_SETPOS, FALSE, 625);
        g.liveFountain = true; WaveformTick();
        bool fountain = FountainShown() && SendMessageW(g.seek, TBM_GETPOS, 0, 0) == 625;
        if (const Tweak* display = FindTweak("view.seek_bar")) {
            SetTweak(*display, 0);
            bool regular = !FountainShown() && !LiveWaveformShown() && g.liveWaveform && g.liveFountain;
            SetTweak(*display, 1);
            Check(regular && FountainShown() && SendMessageW(g.seek, TBM_GETPOS, 0, 0) == 625,
                  L"regular playback overrides the live fountain and restores it without changing the seek position");
        }
        g.liveFountain = false; WaveformTick();
        Check(bars && fountain && !FountainShown(),
              L"the live fountain can replace live bars and be switched back without moving the song's seek position");
        g.liveWaveform = savedLiveWave; g.liveFountain = savedLiveFountain;
        g.playing = savedPlaying;
    }
    OnCommand(IDM_WAVE_STYLE_BASE, 0, nullptr);  // Bars remains an explicit choice at every edge
    batch.Next(L"plain waveform style remains available and a hidden waveform does not run the fountain");
    Check(!FountainShown(), L"plain waveform style remains available");
    g.layout.wave = false;
    Check(!FountainShown(), L"a hidden waveform does not run the fountain");
    batch.End();
    TestWaveform(scratch);
    TestTabOverflow();
    TestThemes();
    TestPackIntegration(scratch);
    TestHelperIntegration(scratch);

    {
        testlog::Group outputGroup(L"per-output sound profiles");
        const EqSettings originalEq = g.eq;
        const HeadsetInfo originalHeadset = g.headset;
        const float originalHeadroom = g.rgHeadphoneHeadroom;
        g.eqOutputId.clear();
        g.rememberOutputEq = false;
        g.globalEq = g.eq;
        SyncOutputEq(L"test-headphones-a");
        g.eq.preamp = -5.5f;
        RememberOutputEq(true);
        g.eq.crossfeed = false;
        SaveEq();
        SaveOutputReplayGainHeadroom(6.5f);
        SyncOutputEq(L"test-headphones-b");
        batch.Next(L"per-output EQ and ReplayGain headroom stay separate per endpoint, survive reload and never leak");
        Check(!g.rememberOutputEq && g.eq.preamp == originalEq.preamp && g.eq.crossfeed == originalEq.crossfeed,
              L"an unassigned output uses global sound, not the previous headphone curve");
        Check(g.rgHeadphoneHeadroom == 0, L"an output has no guessed ReplayGain headset reserve");
        SaveOutputReplayGainHeadroom(2);
        g.eq.preamp = -1.5f;
        SaveEq();
        RememberOutputEq(true);
        g.eq.preamp = -8;
        SaveEq();
        SyncOutputEq(L"test-headphones-a");
        Check(g.rememberOutputEq && g.eq.preamp == -5.5f && !g.eq.crossfeed &&
              ReadEqSettings(L"Equalizer").preamp == -1.5f,
              L"explicitly remembered headphone curves stay separate from each other and global settings");
        Check(g.rgHeadphoneHeadroom == 6.5f, L"each output restores its explicitly saved ReplayGain headroom");
        LoadPreferences();
        SyncOutputEq(L"test-headphones-b");
        Check(g.rememberOutputEq && g.eq.preamp == -8, L"per-device EQ survives a startup reload");
        RememberOutputEq(false);
        SyncOutputEq(L"test-headphones-a");
        SyncOutputEq(L"test-headphones-b");
        Check(!g.rememberOutputEq && g.eq.preamp == -1.5f,
              L"turning off Remember restores global EQ and does not re-enable the profile on reconnect");
        Check(g.rgHeadphoneHeadroom == 2, L"headphone ReplayGain headroom survives reload independently of the software EQ checkbox");
        auto section = OutputProfileSection(L"test-headphones-a");
        IniWrite(section.c_str(), L"OutputId", L"another-endpoint");
        SyncOutputEq(L"test-headphones-a");
        Check(!g.rememberOutputEq && g.eq.preamp == -1.5f,
              L"a profile's full endpoint id is verified before restoring its sound settings");

        g.headset = {};
        g.headset.id = g.eqOutputId;
        g.headset.name = L"Speakers (Razer BlackShark V2 Pro 2.4)";
        g.headset.model = RecognizeHeadphones(g.headset.name);
        g.headset.hardware = true;
        g.headset.battery = 64; g.headset.charging = 0; g.headset.sidetone = 2;
        g.headset.idleMinutes = 30; g.headset.dnd = 0; g.headset.preset = 8;
        g.headset.eqKnown = true;
        g.headset.eqBands = {3, 2, 1, 0, -1, -2, -1, 0, 1, 2};
        g.headset.status = L"USB hardware connected. Changes affect the headset in every application.";
        WNDCLASSW headphonesClass{};
        headphonesClass.hInstance = g.inst;
        headphonesClass.lpszClassName = kHeadphonesClass;
        headphonesClass.lpfnWndProc = HeadphonesProc;
        RegisterClassW(&headphonesClass);
        g.eqWnd = CreateWindowW(kEqClass, L"EQ page test", WS_POPUP,
            0, 0, S(740), S(590), g.hwnd, nullptr, g.inst, nullptr);
        SelectEqPage(true);
        batch.Next(L"the native headphone page shows supported hardware values and distinguishes saved from active curves");
        Check(g.headphonesWnd && GetParent(g.headphonesWnd) == g.eqWnd,
              L"the native headphone settings page creates inside the equalizer");
        if (g.headphonesWnd) {
            HeadphonesSync(true);
            Check(IsWindowEnabled(GetDlgItem(g.headphonesWnd, IDC_EQ_HW_APPLY)) &&
                  SendDlgItemMessageW(g.headphonesWnd, IDC_EQ_SIDETONE, CB_GETCURSEL, 0, 0) == 2 &&
                  SendDlgItemMessageW(g.headphonesWnd, IDC_EQ_HW_PRESET, CB_GETCURSEL, 0, 0) == 2,
                  L"supported hardware values populate the real controls");
            wchar_t curve[512];
            GetDlgItemTextW(g.headphonesWnd, IDC_EQ_HW_CURVE, curve, (int)std::size(curve));
            RECT curveRect, pageRect;
            GetWindowRect(GetDlgItem(g.headphonesWnd, IDC_EQ_HW_CURVE), &curveRect);
            GetWindowRect(g.headphonesWnd, &pageRect);
            Check(wcsstr(curve, L"Music") && wcsstr(curve, L"Saved Custom curve (inactive") &&
                  wcsstr(curve, L"31: +3") && wcsstr(curve, L"1k: -2") &&
                  curveRect.bottom <= pageRect.bottom,
                  L"headphone EQ distinguishes the saved Custom curve from the active factory preset");
            SendDlgItemMessageW(g.headphonesWnd, IDC_EQ_REMEMBER, BM_CLICK, 0, 0);
            batch.Next(L"Remember needs an explicit checkbox choice, and unknown devices keep software controls usable");
            Check(g.rememberOutputEq && IniInt(section.c_str(), L"Remember", 0) == 1,
                  L"Remember requires an explicit checkbox choice and saves the current output only");
            g.headset.hardware = false;
            g.headset.battery = g.headset.sidetone = g.headset.idleMinutes = g.headset.dnd = g.headset.preset = -1;
            HeadphonesSync(true);
            Check(IsWindowEnabled(GetDlgItem(g.headphonesWnd, IDC_EQ_HW_APPLY)) &&
                  IsWindowEnabled(GetDlgItem(g.headphonesWnd, IDC_EQ_RG_HEADROOM)) &&
                  !IsWindowEnabled(GetDlgItem(g.headphonesWnd, IDC_EQ_SIDETONE)) &&
                  IsWindowEnabled(GetDlgItem(g.headphonesWnd, IDC_EQ_REMEMBER)),
                  L"unknown devices retain software profiles and ReplayGain reserve while unsupported hardware controls stay disabled");
            SetDlgItemTextW(g.headphonesWnd, IDC_EQ_RG_HEADROOM, L"4.5");
            SendDlgItemMessageW(g.headphonesWnd, IDC_EQ_HW_APPLY, BM_CLICK, 0, 0);
            batch.Next(L"Apply saves a valid ReplayGain reserve, refuses invalid values and verifies the endpoint id");
            Check(g.rgHeadphoneHeadroom == 4.5f && IniFloat(section.c_str(), L"ReplayGainHeadroom", 0) == 4.5f,
                  L"the real headphone Apply button saves an explicit ReplayGain reserve without requiring native hardware");
            for (const wchar_t* bad : {L"-1", L"25", L"nan", L"6junk", L""}) {
                SetDlgItemTextW(g.headphonesWnd, IDC_EQ_RG_HEADROOM, bad);
                SendDlgItemMessageW(g.headphonesWnd, IDC_EQ_HW_APPLY, BM_CLICK, 0, 0);
            }
            Check(g.rgHeadphoneHeadroom == 4.5f && IniFloat(section.c_str(), L"ReplayGainHeadroom", 0) == 4.5f,
                  L"invalid headset headroom cannot change the current gain or saved reserve");
            IniWrite(section.c_str(), L"ReplayGainHeadroomOutputId", L"another-endpoint");
            SyncOutputEq(L"test-headphones-b");
            SyncOutputEq(L"test-headphones-a");
            Check(g.rgHeadphoneHeadroom == 0, L"headphone headroom checks the complete endpoint id before restoring it");
        }
        if (g.eqWnd) DestroyWindow(g.eqWnd);
        g.eq = g.globalEq = originalEq;
        g.eqOutputId.clear();
        g.rememberOutputEq = false;
        g.headset = originalHeadset;
        g.rgHeadphoneHeadroom = originalHeadroom;
        SaveEq();
        batch.End();
    }

    g.iniPath = original;
    reload();
    ApplyTheme();
    SetWindowPos(g.hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    DeleteFileW(profile.c_str());
    DeleteFileW(empty.c_str());
}

void TestWebPreferences(const std::wstring& scratch) {
    using namespace app;
    testlog::Group group(L"web server controls and shared listening");
    testlog::Batch batch;
    const std::wstring original = g.iniPath, profile = scratch + L"\\web-preferences.ini";
    WriteTestBytes(profile, "\xFF\xFE"); g.iniPath = profile; LoadPreferences();
    StopWebServer(); InitWebServer(); ShowWebServer();
    HWND dialog = GetWindow(g.hwnd, GW_ENABLEDPOPUP);
    wchar_t name[64]{}; GetClassNameW(dialog, name, 64);
    if (wcscmp(name, L"CordinalWebServer") != 0) {
        Check(false, L"server settings open as an owned native window"); g.iniPath = original; LoadPreferences(); return;
    }
    auto text = [&](int id) { wchar_t value[128]{}; GetDlgItemTextW(dialog, id, value, 128); return std::wstring(value); };
    auto click = [&](int id) { SendDlgItemMessageW(dialog, id, BM_CLICK, 0, 0); };
    batch.Next(L"a new profile defaults to port 8787, a new code, no startup and away-from-home listening off");
    Check(text(101) == L"8787" && SendDlgItemMessageW(dialog, 103, BM_GETCHECK, 0, 0) == BST_UNCHECKED &&
              text(112).empty() && !WebStartsWithCordinal(),
          L"a new profile defaults to port 8787, a new code each time and does not opt into startup");
    Check(SendDlgItemMessageW(dialog, 115, BM_GETCHECK, 0, 0) == BST_UNCHECKED && text(119).starts_with(L"Off.") &&
              !IsWindowEnabled(GetDlgItem(dialog, 116)) && !IsWindowEnabled(GetDlgItem(dialog, 117)) &&
              text(118).starts_with(L"Choose Pair device"),
          L"away-from-home listening is off in a new profile, with nothing to copy or scan");
    SetWebPortForTest(0);  // what the checkbox starts listens on loopback only
    click(103);
    batch.Next(L"explicit startup opt-in is saved and starts the server, and opting out leaves it running");
    Check(IniInt(L"Web", L"Start", 0) == 1 && WebStartsWithCordinal() && text(102) == L"Stop server",
          L"explicit startup opt-in is saved immediately and starts the server now");
    click(103);
    Check(IniInt(L"Web", L"Start", 1) == 0 && text(102) == L"Stop server",
          L"explicit startup opt-out is saved immediately and leaves the running server alone");
    testlog::Cases weakCodes;
    for (const wchar_t* weak : {L"12 34", L"777777", L"12a456"}) {
        SetDlgItemTextW(dialog, 112, weak); click(113);
        weakCodes.Add(IniRead(L"Web", L"Code", L"").empty() && text(114).find(L"(fixed)") == std::wstring::npos, weak);
    }
    batch.Next(L"fixed codes are refused, normalised, kept across restarts and can go back to a new code each time");
    Check(weakCodes.Passed(), L"unusable fixed codes (too short, repeated, not digits) are refused and not saved%ls",
          weakCodes.failed.c_str());
    SetDlgItemTextW(dialog, 112, L"4829 1735"); click(113);
    Check(IniRead(L"Web", L"Code", L"") == L"48291735" && text(112) == L"48291735" &&
              text(114) == L"Code: 48291735  (fixed)",
          L"a fixed code is normalised, saved and applied to the running server");
    StopWebServer(); OpenWebPlayer();
    Check(text(114) == L"Code: 48291735  (fixed)", L"the fixed code is in force again when the server restarts");
    Check(text(118).starts_with(L"Scan with the phone's camera"), L"pairing also shows a code to scan");
    StopWebServer();
    SetDlgItemTextW(dialog, 112, L""); click(113);
    Check(IniRead(L"Web", L"Code", L"x").empty() && text(114) == L"Choose Pair device to show a code.",
          L"clearing the fixed code goes back to a new code each time");
    wchar_t port[16]{};
    SetDlgItemTextW(dialog, 101, L"70000"); SendDlgItemMessageW(dialog, 102, BM_CLICK, 0, 0);
    GetDlgItemTextW(dialog, 101, port, 16);
    batch.Next(L"invalid edited or stored ports keep the edited value or fall back, and bad stored codes are unused");
    Check(wcscmp(port, L"70000") == 0 && IsWindowEnabled(GetDlgItem(dialog, 101)),
          L"invalid ports do not start a listener or change the edited value");
    DestroyWindow(dialog);
    IniWrite(L"Web", L"Port", L"0"); InitWebServer(); ShowWebServer();
    dialog = GetWindow(g.hwnd, GW_ENABLEDPOPUP); GetDlgItemTextW(dialog, 101, port, 16);
    Check(wcscmp(port, L"8787") == 0, L"invalid stored ports fall back to the documented default");
    DestroyWindow(dialog);
    IniWrite(L"Web", L"Code", L"12ab56"); InitWebServer(); ShowWebServer();
    dialog = GetWindow(g.hwnd, GW_ENABLEDPOPUP);
    Check(text(112).empty(), L"a hand-edited fixed code that fails the check is not used");
    DestroyWindow(dialog); StopWebServer();
    TrackId id = g_lib.Add(scratch + L"\\browser-listen.wav");
    g_lib[id].durationMs = 2000;
    TrackId playing = g.playing;
    double desktopSeconds = g.listened;
    uint32_t plays = g_lib[id].playCount, skips = g_lib[id].skipCount;
    RecordListening(id, .6, true); RecordListening(id, 1.1, false);
    batch.Next(L"browser listens use normal thresholds without changing the desktop playback session");
    Check(g_lib[id].playCount == plays + 1 && g_lib[id].skipCount == skips + 1 &&
          g.playing == playing && g.listened == desktopSeconds,
          L"browser listens use normal thresholds without changing the desktop playback session");
    g.iniPath = original; LoadPreferences(); DeleteFileW(profile.c_str());
}
