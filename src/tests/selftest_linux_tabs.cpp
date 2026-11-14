// Linux regressions for Now Playing's album, the shared auto-playlist tab and Make album, as on
// Windows (port_regressions.cpp, TestPortedUi): Now Playing lists only the playing song's album until
// a search is typed, the auto-playlists share one tab that moving a tab steps past, and the album
// maker opens over the window and closes without writing, and Album colours turn the palette toward the
// cover. Runs with --check-layout against playlists and a library of its own; nothing is played or saved.
#ifndef _WIN32
#include "../linux/panels.h"
#include "../linux/ui_internal.h"
#include "tags.h"

#include <cstdio>

namespace lx {
int CheckLinuxTabs() {
    int failed = 0;
    auto check = [&](bool ok, const char* message) {
        printf("%s %s\n", ok ? "ok  " : "FAIL", message);
        fflush(stdout);
        failed += !ok;
    };
    Library savedLib = std::move(g_lib);
    g_lib = Library();
    const auto savedSettings = g.settings;
    const auto savedPlaylists = g.playlists;
    const int savedActive = g.active;
    const TrackId savedPlaying = g.playing;
    const std::wstring savedSearch = g.search;
    const uint64_t savedShown = g_autoTabShown;
    std::vector<TrackId> ids;
    for (int i = 0; i < 5; ++i) {
        TrackId id = g_lib.Add(L"/checks/np-album-" + std::to_wstring(i) + L".flac");
        TagData tags;
        tags.title = "Now playing album " + std::to_string(i);
        if (i != 4) tags.album = i == 2 ? "Elsewhere" : "Pickaxe Songs";
        tags.albumArtist = "Rook";
        g_lib.ApplyTags(id, tags);
        ids.push_back(id);
    }
    g.playlists = {Playlist{L"Library", {}, PlaylistKind::Library}, Playlist{L"Now Playing", ids, PlaylistKind::NowPlaying},
                   Playlist{L"A", {}, PlaylistKind::Static}, Playlist{L"Auto tab one", {}, PlaylistKind::Auto},
                   Playlist{L"Auto tab two", {}, PlaylistKind::Auto}, Playlist{L"After the autos", {}, PlaylistKind::Static}};
    g.settings.npAlbumOnly = true;
    g.settings.groupAutoTabs = true;
    g.search.clear();
    g.playing = ids[1];
    SetActiveTab(1);
    FollowNowPlayingAlbum();
    check(g.rows == std::vector<TrackId>{ids[0], ids[1], ids[3]}, "Now Playing lists only the playing song's album");
    g.playing = ids[2];
    FollowNowPlayingAlbum();
    check(g.rows == std::vector<TrackId>{ids[2]}, "Now Playing follows playback into another album");
    g.search = L"\"Now playing album\"";
    RebuildRows();
    check(g.rows.size() == ids.size() && NowPlayingAlbumTrack() == kNone, "a search in Now Playing looks through the whole list");
    g.search.clear();
    g.playing = ids[4];
    FollowNowPlayingAlbum();
    check(g.rows.size() == ids.size(), "a song without an album shows the whole Now Playing list");
    g.playing = ids[0];
    g.settings.npAlbumOnly = false;
    RebuildRows();
    check(g.rows.size() == ids.size(), "with the preference off, Now Playing shows the whole list");
    g.settings.npAlbumOnly = true;

    SetActiveTab(4);
    DoLayout();
    const int shared = TabOfPlaylistIndex(4);
    const std::string label = TabLabel((size_t)shared);
    check(L.tabSlots == std::vector<int>{0, 1, 2, 4, 5} && IsAutoTab(shared) && TabOfPlaylistIndex(3) == shared &&
              label.find("Auto tab two") != std::string::npos && label.find("\xE2\x96\xBE") != std::string::npos,
          "the auto-playlists share one tab that shows the one last opened");
    SetActiveTab(5);
    ReorderPlaylist(-1);
    DoLayout();
    check(g.active == 3 && g.playlists[3].name == L"After the autos" && g.playlists[4].kind == PlaylistKind::Auto &&
              L.tabSlots.size() == 5,
          "moving a tab left steps past every auto-playlist of the shared tab");
    g.settings.groupAutoTabs = false;
    DoLayout();
    check(L.tabSlots.size() == g.playlists.size() && !IsAutoTab(TabOfPlaylistIndex(4)),
          "with the preference off, each auto-playlist has its own tab");

    OpenAlbumMaker({ids[3], ids[0], ids[3]});
    const bool opened = PanelOpen();
    ClosePanel();
    check(opened && !PanelOpen() && !TagWriteBusy(), "Make album opens over the window, and Cancel writes nothing");

    // Album colours (Preferences > View), as Windows' ApplyTheme: with the whole interface, the palette
    // turns toward the shown cover's hue, more at a bolder strength; a gray cover or the player's
    // background alone leave the theme's own palette.
    {
        const AlbumColourSettings savedColours = AlbumColourConfig();
        const TrackId savedFor = g_coverFor;
        const double savedHue = g_coverHue;
        g.settings.theme = ThemeMode::Dark;
        g.playing = g_coverFor = ids[0];
        g_coverHue = 200;
        AlbumColourConfig() = {AlbumColours::Interface, 0};
        const Palette subtle = CurrentPalette();
        AlbumColourConfig().strength = 3;
        const Palette bold = CurrentPalette();
        g_coverHue = -1;
        const bool gray = &CurrentPalette() == &DarkPalette();
        g_coverHue = 200;
        AlbumColourConfig().mode = AlbumColours::Player;
        const bool player = &CurrentPalette() == &DarkPalette();
        check(subtle.from == &DarkPalette() && bold.from == &DarkPalette() && subtle.bg != DarkPalette().bg &&
                  bold.bg != subtle.bg && PaletteLegibility(bold).empty() && gray && player,
              "album colours turn the whole interface toward the cover, and leave it for a gray cover or the player alone");
        AlbumColourConfig() = savedColours;
        g_coverFor = savedFor;
        g_coverHue = savedHue;
    }

    g.playlists = savedPlaylists;
    g.active = std::min(savedActive, (int)g.playlists.size() - 1);
    g.playing = savedPlaying;
    g.search = savedSearch;
    g.settings = savedSettings;
    g_autoTabShown = savedShown;
    g_lib = std::move(savedLib);
    g.rows.clear();
    g.rowItems.clear();
    g.selected.clear();
    RebuildRows();
    DoLayout();
    return failed;
}
}  // namespace lx
#endif
