// The Linux window's headless checks: layout and pack behaviour, plus the separate theme/menu screenshots.
#include "alternate_ui.h"
#include "big_ui.h"
#include "headphones_ui.h"
#include "ui_internal.h"

namespace lx {

int CheckWindowLayouts();
int CheckLinuxPacks();  // src/tests/selftest_linux_packs.cpp
int CheckLinuxQueue();  // src/tests/selftest_linux_queue.cpp
int CheckLinuxTabs();   // src/tests/selftest_linux_tabs.cpp

int CheckLayout() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    int pw = 0, ph = 0;
    SDL_GetRenderOutputSize(g.renderer, &pw, &ph);
    // ui::kLayout is the Tracks view without the browser.
    g.settings.libraryMode = LibraryMode::Tracks;
    g.settings.browserVisible = false;
    DoLayout();
    float top = L.menuBar.B();
    const struct {
        const char* name;
        Rect r;
    } parts[] = {{"previous", L.buttons[kBtnPrev]}, {"play", L.buttons[kBtnPlay]}, {"next", L.buttons[kBtnNext]},
                 {"shuffle", L.buttons[kBtnShuffle]}, {"repeat", L.buttons[kBtnRepeat]}, {"resume", L.buttons[kBtnResume]},
                 {"elapsed", L.elapsed}, {"seek", L.seek}, {"remaining", L.remaining}, {"speaker", L.speaker},
                 {"volume", L.volume}, {"cover", L.cover}, {"title", L.title}, {"subtitle", L.subtitle},
                 {"tabs", L.tabs}, {"radio_tab", L.radioTab}, {"search", L.search}, {"list", L.list}, {"status", L.status}};
    int off = 0;
    if (!UiSymbolsAvailable()) {
        fprintf(stderr, "FAIL fonts: rating stars or the auto-playlist symbol have no glyph\n");
        ++off;
    }
    if (pw != ui::kLayoutWidth || ph != ui::kLayoutHeight + ui::kMenuBarHeight || L.menuBar.h != ui::kMenuBarHeight) {
        printf("FAIL window: %dx%d, menu bar %g (want %dx%d, %d)\n", pw, ph, L.menuBar.h, ui::kLayoutWidth,
               ui::kLayoutHeight + ui::kMenuBarHeight, ui::kMenuBarHeight);
        ++off;
    }
    for (const ui::LayoutRect& want : ui::kLayout) {
        const Rect* got = nullptr;
        for (auto& p : parts)
            if (!strcmp(p.name, want.name)) got = &p.r;
        if (!got) {
            printf("FAIL %s: not laid out\n", want.name);
            ++off;
            continue;
        }
        int x = (int)got->x, y = (int)(got->y - top), w = (int)got->w, h = (int)got->h;
        bool ok = x == want.x && y == want.y && w == want.w && h == want.h;
        printf("%s %s: %d,%d %dx%d", ok ? "ok  " : "FAIL", want.name, x, y, w, h);
        if (!ok) printf(" (Windows: %d,%d %dx%d)", want.x, want.y, want.w, want.h);
        printf("\n");
        off += !ok;
    }
    const auto savedPlaying = g.playing;
    const auto savedSettings = g.settings;
    g.playing = 0;  // a live song display needs no file or audio device for this check
    g.settings.waveform = false;
    g.settings.liveWaveform = g.settings.liveFountain = true;
    g.settings.theme = ThemeMode::Deltarune;
    bool regular = !LiveWaveformShown() && !LiveFountainShown() && !DrawWaveform(L.seek, 0.625, CurrentPalette());
    g.settings.waveform = true;
    regular = regular && LiveWaveformShown() && g.settings.liveWaveform && g.settings.liveFountain;
    printf("%s regular playback overrides live waveform and fountain without changing their settings\n",
           regular ? "ok  " : "FAIL");
    off += !regular;
    g.playing = savedPlaying;
    g.settings = savedSettings;
    // Unrated stars wait for hover or selection; a grouped title runs over the Album and matching
    // Artist and Year cells its header speaks for, stopping at the rating.
    bool stars = !ui::ShowEmptyStars(0, false, false) && ui::ShowEmptyStars(0, true, false) &&
                 ui::ShowEmptyStars(0, false, true) && ui::ShowEmptyStars(20, false, false);
    printf("%s unrated tracks show empty stars only on hover or selection\n", stars ? "ok  " : "FAIL");
    off += !stars;
    bool notches = ui::SeekNotchSeconds(19 * 60) == 0 && ui::SeekNotchSeconds(30 * 60 + 16) == 5 * 60 &&
                   ui::SeekNotchSeconds(6 * 3600 + 3 * 60) == 3600 && ui::PlayedWidth(900, 30, 60) == 450;
    bool tabsFit = ui::FitTabPadding(400, 5, 600, 18, 8) == 18 && ui::FitTabPadding(400, 5, 500, 18, 8) == 8 &&
                   ui::FitTabPadding(400, 5, 450, 18, 8) == -1;
    printf("%s crowded tabs tighten, then share the strip, before scrolling\n", tabsFit ? "ok  " : "FAIL");
    off += !tabsFit;
    printf("%s long tracks notch the seek bar and the playing row underlines its progress\n", notches ? "ok  " : "FAIL");
    off += !notches;
    {
        // A one-track library of its own, so the check needs no profile; the real one is put back.
        Library saved = std::move(g_lib);
        g_lib = Library();
        const auto savedRows = g.rows;
        const auto savedTypes = ActiveColumnTypes();
        const auto savedGroups = g_groups;
        TrackId id = g_lib.Add(L"/checks/grouped.flac");
        Track& t = g_lib[id];
        t.artist = g_lib.Intern("Artist");
        t.album = g_lib.Intern("Album");
        t.date = g_lib.Intern("2020");
        g.rows = {id};
        g_groups = {{0, 2, t.album, t.artist, t.date, 0}};
        ActiveColumnTypes() = {0, 2, 3, 4, 5, ui::kColRating, 6};
        const float title = ColumnWidth(2), all = title + ColumnWidth(3) + ColumnWidth(4) + ColumnWidth(5);
        bool runs = CellTextWidth({0, 0, 0}, 1, title) == all && CellTextWidth({0, -1, 0}, 1, title) == title;
        // Another artist than the album's keeps its Artist cell, which ends the run at the title.
        g_groups[0].artist = g_lib.Intern("Album artist");
        runs = runs && CellTextWidth({0, 0, 0}, 1, title) == title;
        printf("%s grouped titles run over the cells their album header holds\n", runs ? "ok  " : "FAIL");
        off += !runs;
        ActiveColumnTypes() = savedTypes;
        g_groups = savedGroups;
        g.rows = savedRows;
        g_lib = std::move(saved);
    }
    off += CheckWindowLayouts() + CheckPreferences() + CheckLinuxPacks() + CheckLinuxHeadsets() + CheckLinuxBig() + CheckLinuxHelperFeed() + CheckLinuxAlternateThemes() + CheckTagSuggestions() + CheckLinuxQueue() + CheckLinuxTabs();
    const bool software = SetGpuDrawing(false) && !g.settings.gpuDrawing &&
        std::string_view(SDL_GetRendererName(g.renderer)) == SDL_SOFTWARE_RENDERER &&
        UiSymbolsAvailable() && DrawText(Font::Normal, "Renderer restored", 0, 0, CurrentPalette().text) > 0;
    printf("%s switching renderer rebuilds text and cover caches without restarting\n", software ? "ok  " : "FAIL");
    off += !software;
    const bool accelerated = SetGpuDrawing(true);
    const bool recovered = g.renderer && !g.quit && UiSymbolsAvailable() &&
        (accelerated ? g.settings.gpuDrawing : !g.settings.gpuDrawing);
    printf("%s accelerated drawing switches or retains the working software renderer\n", recovered ? "ok  " : "FAIL");
    off += !recovered;
    SetGpuDrawing(false);
    return off;
}

int CaptureUi(const std::string& dir) {
    // The Windows references show the Library's Tracks view with the first track playing from
    // Now Playing; without an audio device here it is shown as playing without being heard.
    int np = NowPlayingIndex();
    if (np >= 0 && g.playing == kNone && !g.rows.empty()) {
        g.playlists[np].items = g.rows;
        g.queuePos = 0;
        g.playing = g.rows[0];
    }
    // The theme images show the Tracks view without the browser; the view images set their own.
    auto view = [](LibraryMode mode, bool browser) {
        g.settings.libraryMode = mode;
        g.settings.browserVisible = browser;
        RebuildRows();
    };
    view(LibraryMode::Tracks, false);
    SetActiveTab(0);
    int failures = 0;
    auto save = [&](const std::string& name) {
        // Covers asked for by this frame arrive as messages; the caller pumps them (CapturePump).
        Draw();
        SDL_Surface* s = SDL_RenderReadPixels(g.renderer, nullptr);
        // PNG, written by the platform layer (src/linux/platform/image_write.cpp).
        std::string path = dir + "/" + name + ".png";
        if (!s || !SDL_SavePNG(s, path.c_str())) {
            fprintf(stderr, "capture: %s: %s\n", path.c_str(), SDL_GetError());
            ++failures;
        } else {
            printf("captured %s\n", path.c_str());
        }
        if (s) SDL_DestroySurface(s);
    };
    static const char* names[] = {"light", "dark", "deltarune", "foobar2000", "paper", "midnight", "terminal", "apollo"};
    static_assert(std::size(names) == (size_t)ThemeMode::Count - 1);
    for (int t = (int)ThemeMode::Light; t < (int)ThemeMode::Count; ++t) {
        std::string name = names[t - 1];
        g.settings.theme = (ThemeMode)t;
        ApplyTheme();
        CloseMenus();
        if (!UiSymbolsAvailable()) {
            fprintf(stderr, "capture: %s: missing rating or auto-playlist glyphs\n", name.c_str());
            ++failures;
        }
        Draw();
        CapturePump();
        save(name);
        if (t <= (int)ThemeMode::Dark && !g.rows.empty()) {
            // Folder access uses search on this port; include a path-only phrase in the reference.
            std::wstring dir = g_lib.Str(g_lib[g.rows.front()].dir);
            g.search = L"\"" + FileNameOf(DirectoryOf(dir)) + L"/" + FileNameOf(dir) + L"\"";
            RebuildRows();
            if (g.rows.empty()) {
                fprintf(stderr, "capture: folder path search returned no tracks\n");
                ++failures;
            }
            save(name + "-folders");
            g.search.clear();
            RebuildRows();
        }
        if (t == (int)ThemeMode::Foobar) {
            g_folders.open = true;
            save(name + "-library-folders");
            g_folders.open = false;
        }
        auto menus = BarMenus();
        for (size_t i = 0; i < menus.size(); ++i) {
            bool all = t <= (int)ThemeMode::Dark;
            if (!all && menus[i] != 0 && menus[i] != 5) continue;
            OpenBarMenu((int)i);
            std::string menu = Plain(ui::kMenuBar[menus[i]]);
            std::transform(menu.begin(), menu.end(), menu.begin(), ::tolower);
            save(name + "-menu-" + menu);
            if (menus[i] == 5) {  // View > Theme
                HoverMenu(0, 0);
                save(name + "-menu-view-theme");
            }
            if (menus[i] == 2 && all) {  // Playback > ReplayGain
                const auto& items = g_popups[0].items;
                for (size_t k = 0; k < items.size(); ++k)
                    if (items[k].label == "ReplayGain") HoverMenu(0, (int)k);
                save(name + "-menu-playback-replaygain");
            }
            CloseMenus();
        }
        if (t <= (int)ThemeMode::Dark && !g.rows.empty()) {  // the dialogs the Linux build adds
            OpenEqualizer();
            save(name + "-equalizer");
            ClosePanel();
            SelectOnly(0);
            OpenTagEditor({g.rows[0]});
            save(name + "-edit-tags");
            ClosePanel();
            OpenPreferences();
            save(name + "-preferences");
            ClosePanel();
        }
        // The first-run Library (Albums with the browser), and in light and dark the other two views,
        // as tools/capture-ui.ps1 takes them; foobar2000 and Apollo have layouts of their own.
        if (t != (int)ThemeMode::Foobar && t != (int)ThemeMode::Apollo) {
            const struct { LibraryMode mode; bool browser; const char* suffix; } views[] = {
                {LibraryMode::Albums, true, "-albums"}, {LibraryMode::AlbumTracks, false, "-album-tracks"},
                {LibraryMode::Artists, false, "-artists"}};
            for (const auto& v : views) {
                if (v.mode != LibraryMode::Albums && t > (int)ThemeMode::Dark) continue;
                g.selected.assign(g.rows.size(), 0);  // as a fresh start: nothing selected
                view(v.mode, v.browser);
                Draw();
                CapturePump();  // the covers at the views' sizes
                Draw();
                CapturePump();
                save(name + v.suffix);
            }
            view(LibraryMode::Tracks, false);
        }
    }
    return failures;
}

}  // namespace lx
