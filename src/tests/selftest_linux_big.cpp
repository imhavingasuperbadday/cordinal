// Linux presentation regression checks: Windows-compatible panel trees, real hit testing and
// bounded geometry, plus independent Compact/full window placement. No images are saved.
#ifndef _WIN32
#include "../linux/big_ui.h"
#include "../linux/ui_internal.h"
#include <filesystem>
#include <cstdio>
#include <unistd.h>

namespace lx {
int CheckLinuxBig() {
    int failed = 0;
    auto check = [&](bool ok, const char* message) {
        printf("%s %s\n", ok ? "ok  " : "FAIL", message);
        fflush(stdout); failed += !ok;
    };
    char temporary[] = "/tmp/cordinal-big-XXXXXX";
    if (!mkdtemp(temporary)) { check(false, "Big checks create an isolated profile"); return failed; }
    const auto savedSettings = g.settings;
    const bool savedSearchFocused = g.searchFocused;
    const auto savedData = g.dataDir, savedPath = g.settingsPath;
    const std::string savedTree = BigLayoutText(), savedCompact = CompactPlacementText();
    g.settings = Settings{};
    g.dataDir = compat::Widen(temporary);
    g.settingsPath = g.dataDir + L"/settings.ini";

    const std::string defaults = "g0:0;|h500g0:1;g0:2;";
    LoadBigLayout("");
    check(BigLayoutText() == defaults, "Big starts with Library above and Now playing / Up next below");
    const char* invalid[] = {"", "-", "g0:0;", "g0:0;|g0:0;", "g0:3;|-", "g0:;|-",
        "h500g0:0;-|-", "g0:0;|-trailing", "g0:0.1.2.0;|-", "g999999999999999999999:0;|-",
        "h500h500h500h500h500h500h500h500h500h500h500h500h500g0:0;g0:1;|-"};
    bool rejected = true;
    for (const char* text : invalid) { LoadBigLayout(text); rejected = rejected && BigLayoutText() == defaults; }
    check(rejected, "malformed, duplicate, excessive-depth and overflowing Big layouts fall back safely");
    const std::string custom = "g1:0.1;|g0:2;";
    LoadBigLayout(custom);
    check(BigLayoutText() == custom && BigPanelShown(0) && BigPanelShown(1) && BigPanelShown(2),
        "Big reads and writes the same tab/split layout format as Windows");

    Rect library;
    g.settings.size = ViewSize::Big;
    LoadBigLayout(defaults);
    LayoutPresentation({0, 0, 1000, 800}, library);
    check(library.h > 0 && library.B() <= L.player.y && L.upNext.y >= L.player.B() &&
        L.upNext.w > 0 && L.seek.w > 0 && L.buttons[kBtnPlay].w >= S(38),
        "Big keeps its playback strip between the independent panel areas");
    ToggleBigPanel(2);
    LayoutPresentation({0, 0, 1000, 800}, library);
    check(!BigPanelShown(2) && L.upNext.w == 0, "closing Up next releases its body and collapses the split");
    ToggleBigPanel(2);
    LayoutPresentation({0, 0, 1000, 800}, library);
    check(BigPanelShown(2) && L.upNext.w > 0, "reopening a closed Big panel selects it in its area's tabs");

    LoadBigLayout(defaults);
    LayoutPresentation({0, 0, 1000, 800}, library);
    const bool pressed = PresentationMouseDown(S(20), S(15), SDL_BUTTON_LEFT, 1);
    const bool moved = PresentationMouseMove(750, 630, true);
    const bool dropped = PresentationMouseUp(750, 630);
    LayoutPresentation({0, 0, 1000, 800}, library);
    check(pressed && moved && dropped && BigLayoutText() == "-|h500g0:1;g1:2.0;" && library.h > 0 &&
        library.y >= L.player.B(), "dragging a Big tab joins the target group and moves its live panel body");

    LoadBigLayout(defaults);
    LayoutPresentation({0, 0, 1000, 800}, library);
    const float splitX = 500, splitY = L.player.B() + 60;
    const bool splitPress = PresentationMouseDown(splitX, splitY, SDL_BUTTON_LEFT, 1);
    PresentationMouseMove(700, splitY, true);
    PresentationMouseUp(700, splitY);
    LayoutPresentation({0, 0, 1000, 800}, library);
    check(splitPress && L.upNext.x > 650 && L.upNext.w < 350, "dragging a Big splitter changes its stored ratio");
    const std::string splitLayout = BigLayoutText();
    LoadBigLayout(splitLayout);
    check(BigLayoutText() == splitLayout, "a resized Big splitter round-trips through settings");

    LoadBigLayout("g0:0.1.2;|-");
    LayoutPresentation({0, 0, 1000, 800}, library);
    SDL_KeyboardEvent key{};
    key.key = SDLK_F6;
    const bool focused = PresentationKey(key);
    key.key = SDLK_DOWN; key.mod = SDL_KMOD_CTRL;
    const bool transferred = PresentationKey(key);
    check(focused && transferred && BigLayoutText().find("|g0:") != std::string::npos,
        "F6 and Ctrl+arrow move a Big panel between areas without a mouse");
    key.key = SDLK_DELETE; key.mod = 0;
    PresentationKey(key);
    check(!BigPanelShown(1), "Delete closes the keyboard-focused Big panel");
    ToggleBigPanel(1);
    check(BigPanelShown(1), "the menu can reopen a panel closed from the keyboard");

    LoadBigLayout(defaults);
    LayoutPresentation({0, 0, 1000, 800}, library);
    key.key = SDLK_F6; key.mod = 0;
    PresentationKey(key);
    key.key = SDLK_RIGHT; key.mod = SDL_KMOD_CTRL | SDL_KMOD_SHIFT;
    PresentationKey(key);
    const bool splitByKey = BigLayoutText().find('h') != std::string::npos;
    key.key = SDLK_RIGHT; key.mod = SDL_KMOD_ALT;
    const std::string beforeResize = BigLayoutText();
    PresentationKey(key);
    check(splitByKey && beforeResize != BigLayoutText(),
        "keyboard split and resize actions expose Big docking without pointer dragging");

    g.settings.size = ViewSize::Spotlight;
    LayoutPresentation({0, 0, 1000, 800}, library);
    check(library.w == 0 && L.upNext.h > 0 && L.upNext.y >= L.player.B() && L.cover.w == 0,
        "Big mode plus dedicates its areas to Now playing and Up next");
    g.settings.size = ViewSize::Compact;
    LayoutPresentation({0, 0, 440, 600}, library);
    check(L.cover.w > S(ui::kCover) && L.buttons[kBtnPlay].w > S(ui::kButton) &&
        library.y >= L.player.B() && library.h > 0 && L.seek.x >= 0 && L.seek.R() <= 440,
        "Compact enlarges cover and playback controls while preserving room for the Library");
    g.settings.size = ViewSize::Normal;
    check(!LayoutPresentation({0, 0, 1000, 800}, library), "Normal uses the customizable player layout");

    // Window operations use the existing offscreen diagnostic window and restore it afterward.
    int width = 0, height = 0;
    SDL_GetWindowSize(g.window, &width, &height);
    g.settings.size = ViewSize::Normal;
    LoadCompactPlacement("10,20,999999999999999999999999999,560");
    check(CompactPlacementText().empty(), "overflowing Compact placement is ignored safely");
    LoadCompactPlacement("10,20,420,560");
    SetViewSize(ViewSize::Compact);
    int compactW = 0, compactH = 0; SDL_GetWindowSize(g.window, &compactW, &compactH);
    SavePresentationWindow();
    const bool keptFull = g.settings.windowW == width && g.settings.windowH == height;
    SetViewSize(ViewSize::Normal);
    int restoredW = 0, restoredH = 0; SDL_GetWindowSize(g.window, &restoredW, &restoredH);
    check(compactW == 420 && compactH == 560 && keptFull && restoredW == width && restoredH == height,
        "Compact stores its own dimensions and restores the full window exactly");
    check(CompactPlacementText().find(",420,560") != std::string::npos,
        "Compact placement serializes separately from the normal window");

    LoadBigLayout(savedTree);
    LoadCompactPlacement(savedCompact);
    g.settings = savedSettings;
    g.searchFocused = savedSearchFocused;
    g.dataDir = savedData; g.settingsPath = savedPath;
    std::error_code error;
    std::filesystem::remove_all(temporary, error);
    check(!error, "Big checks remove their temporary profile");
    DoLayout();
    return failed;
}
}  // namespace lx
#endif
