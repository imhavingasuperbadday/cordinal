// Geometry and startup defaults of Linux's normal configurable window; no image capture is used.
#ifndef _WIN32
#include "../linux/ui_internal.h"
#include <cstdio>

namespace lx {
int CheckStartupPresentation();

int CheckWindowLayouts() {
    const Settings saved = g.settings;
    const float scale = g.scale;
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        printf("%s %s\n", ok ? "ok  " : "FAIL", label);
        failures += !ok;
    };
    g.settings.size = ViewSize::Normal;
    for (float factor : {1.f, 2.f}) {
        g.scale = factor;
        bool geometry = true;
        for (int dock = 0; dock < 4; ++dock) {
            g.settings.layout = WindowLayout{};
            g.settings.layout.dock = dock;
            g.settings.layout.largeCover = true;
            g.settings.layout.upNext = true;
            g.settings.layout.points = true;
            g.settings.layout.waveOwnRow = true;
            g.settings.lyricsPanel = true;
            for (bool left : {false, true}) {
                g.settings.layout.panelsLeft = left;
                DoLayout();
                geometry &= L.list.w > 0 && L.rows.h >= 0 && L.player.w > 0 &&
                    L.upNext.w > 0 && L.lyrics.w > 0 && L.upNext.y >= L.lyrics.B() &&
                    L.seek.w >= 0 && L.seek.h >= 0 &&
                    (left ? L.lyrics.R() <= L.list.x : L.list.R() <= L.lyrics.x);
                if (dock == 0) geometry &= L.player.B() <= L.tabs.y;
                if (dock == 1) geometry &= L.list.B() <= L.player.y;
                if (dock == 2) geometry &= L.player.R() <= L.tabs.x;
                if (dock == 3) geometry &= L.list.R() <= L.player.x;
                for (const Rect& button : L.buttons)
                    geometry &= button.x >= L.player.x && button.R() <= L.player.R() + 1 &&
                        button.y >= L.player.y && button.B() <= L.player.B() + 1;
            }
        }
        check(geometry, factor == 1 ? "all four player docks and left/right panels fit at 100 percent" :
            "all four player docks and left/right panels fit at 200 percent");
    }
    g.scale = scale;
    g.settings = saved;
    g.settings.layout = WindowLayout{};
    DoLayout();
    const float small = L.cover.w;
    g.settings.layout.largeCover = true;
    DoLayout();
    check(L.cover.w == 2 * small && L.title.w > 0, "large cover gives the title and artist their own remaining space");
    g.settings.layout.menuBar = false;
    DoLayout();
    OpenBarMenu(0);
    check(L.menuBar.h == 0 && !g_popups.empty() && !g_popups[0].items.empty(),
        "hidden menu bars remain accessible through the keyboard menu path");
    MenuKey(SDLK_RIGHT);
    const bool nextMenu = g_barOpen == 1;
    MenuKey(SDLK_LEFT);
    check(nextMenu && g_barOpen == 0, "hidden menu bars support Left and Right without visible bar hit targets");
    CloseMenus();
    g.settings.layout = WindowLayout{};
    g.settings.layout.hiddenParts = kPartTransport | kPartOrder | kPartWave | kPartTimes | kPartVolume | kPartNowPlaying;
    DoLayout();
    check(L.grip.w == 0 && L.cover.w == 0 && L.seek.w == 0 && L.tabs.y <= S(ui::kMenuBarHeight + 4),
        "hiding the entire player returns its space to the library");
    g.settings = saved;
    g.scale = scale;
    DoLayout();
    return failures + CheckStartupPresentation();
}
}  // namespace lx
#endif
