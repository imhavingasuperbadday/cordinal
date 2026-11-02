// Private Linux theme regressions: isolated model/filter state, alternate geometry, settings and
// both waveform styles. Drawing targets the existing offscreen renderer; no image is captured.
#ifndef _WIN32
#include "../linux/alternate_ui.h"
#include "../linux/ui_internal.h"
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace lx {
int CheckLinuxAlternateThemes() {
    int failed = 0;
    auto check = [&](bool ok, const char* label) {
        printf("%s %s\n", ok ? "ok  " : "FAIL", label);
        failed += !ok;
    };
    Library fixture;
    TrackId one = fixture.Add(L"/music/Artist/Album/01.ogg"), two = fixture.Add(L"/music/Artist/Album/02.ogg");
    for (TrackId id : {one, two}) {
        auto& t = fixture[id];
        t.flags = Track::kInLibrary | Track::kLoaded;
        t.artist = fixture.Intern("Artist"); t.album = fixture.Intern("Album"); t.genre = fixture.Intern("Test");
        t.title = fixture.Intern(id == one ? "Needle" : "Other");
    }
    auto folders = BuildAlbumList(fixture, 0, L"");
    auto artists = BuildAlbumList(fixture, 1, L"");
    auto filtered = BuildAlbumList(fixture, 1, L"needle");
    check(folders.size() == 1 && folders[0].label == L"music" && folders[0].tracks.size() == 2 &&
        folders[0].children[0].label == L"Artist" && folders[0].children[0].children[0].label == L"Album" &&
        artists.size() == 1 && artists[0].children.size() == 1 && artists[0].children[0].children.size() == 2 &&
        filtered.size() == 1 && filtered[0].tracks == std::vector<TrackId>{one},
        "Album List builds folder and artist/album trees and filters case-insensitively without changing library data");
    const Settings saved = g.settings;
    const auto oldAppearance = FountainAppearance();
    const float scale = g.scale;
    const std::wstring settingsPath = g.settingsPath, dataDir = g.dataDir;
    char scratch[] = "/tmp/cordinal-alternate-XXXXXX";
    if (!mkdtemp(scratch)) { check(false, "private theme checks have an isolated settings folder"); return failed; }
    {
        std::string flac(64, '\0');
        flac.replace(0, 4, "fLaC"); flac[20] = 1; flac[21] = 0x70;  // 24-bit STREAMINFO
        std::string wav(64, '\0');
        wav.replace(0, 4, "RIFF"); wav.replace(8, 4, "WAVE"); wav.replace(12, 4, "fmt "); wav[34] = 24;
        std::string mp3(256, '\0');
        mp3.replace(0, 3, "ID3"); mp3[3] = 3; mp3[4] = 2; mp3.replace(128, 3, "TAG"); mp3[254] = 1;
        const std::filesystem::path root(scratch);
        for (const auto& [name, bytes] : {std::pair{"depth.flac", flac}, {"depth.wav", wav}, {"tag.mp3", mp3}}) {
            std::ofstream out(root / name, std::ios::binary); out.write(bytes.data(), bytes.size());
        }
        auto f = ProbeApolloFile(FromUtf8((root / "depth.flac").string()), Codec::FLAC);
        auto w = ProbeApolloFile(FromUtf8((root / "depth.wav").string()), Codec::PCM);
        auto m = ProbeApolloFile(FromUtf8((root / "tag.mp3").string()), Codec::MP3);
        check(f.bits == 24 && f.size == 64 && w.bits == 24 && m.bits == 16 &&
            m.id3 == 3 && m.id3rev == 2 && m.id3v1 == 11,
            "Apollo header probes retain FLAC/WAV bit depth, exact size and both ID3 versions");
    }
    g.settingsPath = FromUtf8(std::string(scratch) + "/settings.ini");
    g.dataDir = FromUtf8(scratch);
    LoadAlternateSettings({{"wave_style", "1"}, {"wave_motion", "0"}, {"wave_2_played", "80B8E8"},
        {"wave_2_rest", "bad"}, {"apollo_skin", "99"}, {"apollo_opacity", "60"}});
    const auto text = AlternateSettingsText();
    check(FountainAppearance().style == 1 && !FountainAppearance().motion &&
        FountainAppearance().played[2] == 0x80B8E8 && FountainAppearance().rest[2] == kNoRgb &&
        text.find("apollo_skin=4\n") != std::string::npos && text.find("apollo_opacity=60\n") != std::string::npos,
        "waveform appearance validates per-dock colors and bounds optional Apollo settings");
    g.settings.size = ViewSize::Normal;
    g.settings.theme = ThemeMode::Light;
    check(!AlternateMode() && &ActiveColumnTypes() == &g.settings.columns && !FountainStyleShown(),
        "normal themes keep their own columns and never activate private layouts");
    const auto viewMenu = BarMenu(5);
    const bool appearanceItem = std::any_of(viewMenu.begin(), viewMenu.end(),
        [](const MenuItem& item) { return item.label == "Waveform appearance" && !item.sub.empty(); });
    check(appearanceItem == kExtraThemes, "View offers waveform appearance only when its private controls exist");
    if (!kExtraThemes) {
        check(WaveAppearanceMenu().empty(), "public builds offer no private waveform menu");
    } else {
        const auto baseColumns = g.settings.columns;
        for (float factor : {1.f, 2.f}) {
            g.scale = factor;
            g.settings.theme = ThemeMode::Foobar;
            ApplyTheme(); DoLayout();
            check(AlternateMode() && L.list.x >= S(200) && L.player.B() <= L.tabs.y &&
                L.visuals[0].w > 0 && L.visuals[1].w > 0 && L.seek.w > 0 &&
                ActiveColumnTypes() == std::vector<int>({0, ui::kColCover, ui::kColTrackNo, ui::kColTitleArtist}) &&
                g.settings.columns == baseColumns && DrawAlternate(CurrentPalette()),
                factor == 1 ? "foobar layout has its Album List, toolbar and dedicated columns at 100 percent" :
                    "foobar layout retains separate columns and usable geometry at 200 percent");
            g.settings.theme = ThemeMode::Apollo;
            ApplyTheme(); DoLayout();
            check(AlternateMode() && L.menuBar.h == 0 && L.tabs.h == 0 && L.list.x > 0 &&
                L.visuals[0].w > 0 && L.visuals[1].w > 0 && ActiveColumnWidths()[ui::kColApolloTitle] >= 80 &&
                ActiveColumnTypes()[1] == ui::kColApolloTitle && DrawAlternate(CurrentPalette()) &&
                DrawAlternateStatus(CurrentPalette()) && g.settings.columns == baseColumns,
                factor == 1 ? "Apollo draws its rebar, classic toolbar and fitted playlist at 100 percent" :
                    "Apollo's toolbar and playlist remain usable at 200 percent");
        }
        g.scale = scale;
        g.settings.theme = ThemeMode::Deltarune;
        g.settings.waveform = true; g.settings.liveWaveform = false; g.settings.liveFountain = false;
        ApplyTheme(); DoLayout();
        uint8_t peaks[kPeakBuckets];
        for (int i = 0; i < kPeakBuckets; ++i) peaks[i] = (uint8_t)(i % 256);
        bool shapes = true;
        for (int style : {1, 2}) for (bool vertical : {false, true}) {
            FountainAppearance().style = style;
            Rect area{0, 0, vertical ? 96.f : 300.f, vertical ? 300.f : 96.f};
            shapes &= FountainStyleShown() && DrawFountain(area, 0.4, CurrentPalette(), peaks, kPeakBuckets, false, vertical);
        }
        check(shapes, "whole-song fountain bars and sprite fountain draw horizontally and vertically from cached peaks");
        FountainAppearance().style = 0;
        check(!FountainStyleShown() && !DrawFountain({0, 0, 30, 30}, 0, CurrentPalette(), peaks, kPeakBuckets, false, false),
            "plain waveform style leaves fountain drawing disabled");
        // Text entry and cancellation are tested through the actual optional color dialog event path.
        auto menu = WaveAppearanceMenu();
        auto& played = menu.back().sub[0].sub[0];
        RunAlternateAction(played.action, played.arg);
        check(AlternateText("12abEF"), "waveform color dialog accepts a six-digit RGB value");
        SDL_KeyboardEvent event{}; event.key = SDLK_RETURN;
        AlternateKey(event);
        check(FountainAppearance().played[0] == 0x12ABEF, "keyboard confirmation saves only the explicitly chosen waveform color");
        g.settings.theme = ThemeMode::Light;
        ApplyTheme();
    }
    g.settings = saved; g.scale = scale; FountainAppearance() = oldAppearance;
    g.settingsPath = settingsPath; g.dataDir = dataDir;
    ReleaseAlternateTextures(); AlternateThemeChanged(); ApplyTheme(); DoLayout();
    std::error_code error;
    std::filesystem::remove_all(scratch, error);
    check(!error, "private theme checks remove their temporary profile");
    return failed;
}
}
#endif
