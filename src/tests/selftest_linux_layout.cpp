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

    // The Layout menu, as the Windows build's: View's layout items are there, it starts from a preset or a
    // blank canvas, and it saves, lists and forgets your own layouts.
    g.settings = saved;
    for (auto& slot : g.settings.savedLayouts) slot.clear();
    auto find = [](const std::vector<MenuItem>& items, const std::string& label) -> const MenuItem* {
        for (const MenuItem& item : items)
            if (item.label == label) return &item;
        return nullptr;
    };
    const auto view = BarMenu(ui::kViewMenu), layout = BarMenu(ui::kLayoutMenu);
    check(Plain(ui::kMenuBar[ui::kLayoutMenu]) == "Layout" && Plain(ui::kMenuBar[ui::kHelpMenu]) == "Help" &&
              find(layout, "Start from a preset") && find(layout, "Start from a blank canvas") && find(layout, "Show") &&
              find(layout, "Window size") && find(layout, "Big mode panels") && find(layout, "Mini player") &&
              find(layout, "Customize layout...") && !find(view, "Window size") && !find(view, "Mini player") &&
              !find(view, "Customize layout...") && !find(view, "Lyrics panel") && find(view, "Theme"),
          "the Layout menu sits between View and Help and holds the layout items View had");
    RunLayoutAction(kLayoutPreset, 1);
    WindowLayout bottom;
    bottom.dock = 1;
    bottom.waveSize = 1;
    bottom.waveOwnRow = bottom.largeCover = true;
    check(g.settings.layout == bottom && g.settings.defaultLayout == bottom, "Layout presets read from ui_spec.h keep their arrangements");
    RunLayoutAction(kLayoutBlank, 0);
    DoLayout();
    check(g.settings.layout.hiddenParts == 1023 && g.settings.layout.menuBar && !g.settings.layout.upNext &&
              !g.settings.layout.hints && L.seek.w == 0 && L.grip.w == 0 && L.tabs.h == 0 && L.menuBar.h > 0,
          "a blank canvas hides every part but the menu bar");
    const auto blankMenu = BarMenu(ui::kLayoutMenu);
    const MenuItem* show = find(blankMenu, "Show");
    const MenuItem* waveRow = show ? find(show->sub, "Waveform seek bar") : nullptr;
    if (waveRow) RunAction(*waveRow);
    DoLayout();
    check(waveRow && !(g.settings.layout.hiddenParts & kPartWave) && L.seek.w > 0, "a part added from Layout > Show shows again");
    const WindowLayout built = g.settings.layout;
    RunLayoutAction(kLayoutSave, 0);
    const std::string savedText = g.settings.savedLayouts[0];
    RunLayoutAction(kLayoutPreset, 0);
    const bool reset = g.settings.layout == WindowLayout{};
    RunLayoutAction(kLayoutSaved, 0);
    const auto start = BarMenu(ui::kLayoutMenu);
    const MenuItem* presets = find(start, "Start from a preset");
    const MenuItem* mine = presets ? find(presets->sub, "My layout 1") : nullptr;
    const MenuItem* save = find(start, "Save this layout");
    check(!savedText.empty() && savedText.find('\n') == std::string::npos && reset && g.settings.layout == built && mine &&
              mine->checked && save && !save->enabled && presets && find(presets->sub, "Forget a saved layout"),
          "Save this layout keeps My layout 1, which the presets list and check while the window is it");
    RunLayoutAction(kLayoutForget, 0);
    check(g.settings.savedLayouts[0].empty() && g.settings.layout == built, "forgetting a saved layout keeps the window's layout");
    g.settings = saved;
    g.scale = scale;
    DoLayout();
    return failures + CheckStartupPresentation();
}
}  // namespace lx
#endif
