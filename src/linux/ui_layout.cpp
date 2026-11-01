// The Linux window's measurements: the menu bar, transport, tabs, browser, list and status, in pixels.
#include "ui_internal.h"

namespace lx {

// The normal window as app_custom.cpp (LayoutBand, PlaceBandRow, PlaceNowPlaying) and app_layout.cpp
// (LayoutLibrary) place it, from the shared measurements, under the menu bar.
void DoLayout() {
    int pw = 0, ph = 0;
    SDL_GetRenderOutputSize(g.renderer, &pw, &ph);
    float W = (float)pw, H = (float)ph, pad = S(ui::kPad), tb = S(ui::kButton);
    L.menuBar = {0, 0, W, S(ui::kMenuBarHeight)};
    L.barItems.clear();
    float mx = S(1);
    for (int m : BarMenus()) {
        float w = TextWidth(Font::Menu, Plain(ui::kMenuBar[m])) + S(14);
        L.barItems.push_back({mx, 0, w, S(ui::kMenuBarHeight - 2)});
        mx += w;
    }
    float top = L.menuBar.B();
    // The band: the handle, the buttons, the times around the seek bar, the volume.
    const unsigned hide = g.settings.hiddenParts;  // View > Customize layout: a hidden part's rectangle is empty
    float waveH = hide & kPartWave ? tb : std::max(tb, S(ui::kBandWaveHeights[std::clamp(g.settings.bandWaveSize, 0, 5)]));
    float y = top + S(ui::kBandTop), by = y + (waveH - tb) / 2;
    float ty = by + (tb - S(ui::kTimeHeight)) / 2 + S(1);
    L.grip = {0, y, S(ui::kGrip), waveH};
    float x = S(ui::kGrip);
    for (int i = 0; i < 6; ++i) {
        const bool hidden = hide & (i < 3 ? kPartTransport : kPartOrder);
        if (i == 3 && !(hide & kPartTransport) && !hidden) x += S(ui::kOrderGap);
        L.buttons[i] = hidden ? Rect{} : Rect{x, by, tb, tb};
        if (!hidden) x += tb;
    }
    x += S(ui::kButtonsGap);
    float right = W - pad, timeW = S(ui::kTimeWidth);
    if (hide & kPartVolume) {
        L.volume = L.speaker = {};
    } else {
        L.volume = {right - S(ui::kVolumeWidth), by, S(ui::kVolumeWidth), tb};
        L.speaker = {L.volume.x - S(ui::kSpeakerWidth), by + S(4), S(ui::kSpeakerWidth), S(ui::kTimeHeight)};
        right = L.volume.x - S(ui::kVolumeGap);
    }
    if (hide & kPartTimes) {
        L.elapsed = L.remaining = {};
    } else {
        L.elapsed = {x, ty, timeW, S(ui::kTimeHeight)};
        L.remaining = {right - timeW, ty, timeW, S(ui::kTimeHeight)};
        x += timeW + S(ui::kTimeGap);
        right -= timeW + S(ui::kTimeGap);
    }
    L.seek = (hide & kPartWave) ? Rect{} : Rect{x, y, std::max(0.0f, right - x), waveH};
    const bool band = (hide & (kPartTransport | kPartOrder | kPartWave | kPartTimes | kPartVolume)) !=
                      (kPartTransport | kPartOrder | kPartWave | kPartTimes | kPartVolume);
    y += band ? waveH + S(ui::kBandGap) : 0;
    if (!band) L.grip = {};
    // The now playing line.
    const bool nowPlaying = !(hide & kPartNowPlaying);
    L.cover = nowPlaying ? Rect{pad, y, S(ui::kCover), S(ui::kCover)} : Rect{};
    float x0 = L.cover.R() + S(ui::kCoverGap);
    L.title = nowPlaying ? Rect{x0, y, W - x0 - pad, S(ui::kLineHeight)} : Rect{};
    L.subtitle = nowPlaying ? Rect{x0, y + S(ui::kLineHeight), W - x0 - pad, S(ui::kLineHeight)} : Rect{};
    L.chip = {};
    if (g.settings.pointsChip && nowPlaying) {  // the level and points chip, at the right end of the now-playing lines
        const float chipW = S(220);
        L.chip = {W - pad - chipW, L.cover.y + (L.cover.h - S(32)) / 2, chipW, S(32)};
        L.title.w -= chipW + S(8);
        L.subtitle.w -= chipW + S(8);
    }
    if (nowPlaying) y += S(ui::kCover) + S(ui::kBandGap);
    L.visuals[0] = L.visuals[1] = {};
    if (g.settings.theme == ThemeMode::Foobar) {
        L.visuals[0] = {pad, y, S(ui::kFoobarSpectrum), S(ui::kFoobarButton)};
        L.visuals[1] = {L.visuals[0].R() + S(10), y, std::max(0.0f, W - L.visuals[0].R() - S(10) - pad), S(ui::kFoobarButton)};
        y += S(ui::kFoobarButton) + S(ui::kBandGap);
    }
    // The Library: tabs and search, the view row, the list, the status bar.
    float searchW = std::min(S(ui::kSearchWidth), std::floor(W / 3));
    const bool searchShown = !(hide & kPartSearch), tabsShown = !(hide & kPartTabs);
    L.search = searchShown ? Rect{W - pad - searchW, y + S(1), searchW, S(ui::kTabRow) - S(2)} : Rect{};
    float tabWidth = searchShown ? W - (pad + searchW + S(ui::kSearchGap)) : W;
    float radioWidth = tabsShown && InternetRadio().tabOpen ? std::min(S(ui::kRadioTabWidth), std::max(0.0f, tabWidth)) : 0;
    L.tabs = {0, y, tabWidth - radioWidth, searchShown || tabsShown ? S(ui::kTabRow) : 0};
    L.radioTab = {L.tabs.R(), y, radioWidth, L.tabs.h};
    L.tabRects.clear();
    float tx = S(2);
    for (size_t i = 0; tabsShown && i < g.playlists.size(); ++i) {
        float w = TextWidth(Font::Normal, (int)i == g_renameIndex ? Utf8(g_renameText) : TabName(i)) + S(36);
        L.tabRects.push_back({tx, L.tabs.y, w, L.tabs.h});
        tx += w;
    }
    y += L.tabs.h;
    const float lw = LyricsWidth(), LW = W - lw;  // the lyrics panel takes the right edge
    bool library = !InternetRadio().active && !g.playlists.empty() && Active().kind == PlaylistKind::Library && !(hide & kPartViews);
    L.viewRow = {0, y, LW, library ? S(ui::kViewRow) : 0};
    // The views as one segmented control on the left, the browser toggle on the right (app_layout.cpp).
    for (int i = 0, vx = (int)pad; i < kViewButtons; ++i) {
        float bw = TextWidth(Font::Normal, ViewButtonLabel(i)) + S(16);
        L.views[i] = {i == kViewButtons - 1 ? LW - S(8) - bw : (float)vx, y + S(4), bw, S(ui::kViewButton)};
        vx += (int)(bw + S(4));
    }
    y += L.viewRow.h;
    L.status = (hide & kPartStatus) ? Rect{0, H, W, 0} : Rect{0, H - S(ui::kStatus), W, S(ui::kStatus)};
    L.browser = L.splitter = {};
    if (!InternetRadio().active && BrowserShown()) {
        float h = std::clamp(BrowserHeight(), S(60), std::max(S(60), L.status.y - y - S(120)));
        L.browser = {0, y, LW, h};
        L.splitter = {0, y + h, LW, S(5)};
        y += h + S(5);
    }
    L.list = {0, y, LW, std::max(0.0f, L.status.y - y)};
    float gap = S(6), field = std::max(S(30), std::min(S(150), (LW - S(70) - 5 * gap) / 3));
    L.radioSaved = {gap, y + S(3), S(100), S(25)};
    L.radioDiscover = {S(110), y + S(3), S(90), S(25)};
    L.radioGenre = {gap, y + S(34), field, S(24)};
    L.radioCountry = {gap * 2 + field, y + S(34), field, S(24)};
    L.radioState = {gap * 3 + field * 2, y + S(34), field, S(24)};
    L.radioSearch = {gap * 4 + field * 3, y + S(34), S(70), S(24)};
    L.radioGrid = {0, y + S(68), LW, std::max(0.0f, L.status.y - y - S(68))};
    L.header = {0, y, LW, S(24)};
    L.rowH = S(ui::kRow);
    float sbW = S(17);
    L.rows = {0, L.header.B(), LW, std::max(0.0f, L.status.y - L.header.B())};
    bool scroll = g_lines.size() * L.rowH > L.rows.h;
    if (scroll) L.rows.w -= sbW;
    L.scrollbar = {LW - sbW, L.rows.y, scroll ? sbW : 0, L.rows.h};
    L.lyrics = lw > 0 ? Rect{LW, L.viewRow.y, lw, L.status.y - L.viewRow.y} : Rect{};
}

float ContentHeight() { return g_lines.size() * L.rowH; }

float MaxScroll() { return std::max(0.0f, ContentHeight() - L.rows.h); }

void ClampScroll() { g.scroll = std::clamp(g.scroll, 0.0f, MaxScroll()); }

void EnsureVisible(int row) {
    if (row < 0 || row >= (int)g_lineOfRow.size()) return;
    int line = g_lineOfRow[row];
    // Show an album's header with its first track.
    if (line >= 2 && g_lines[line - 1].row < 0 && g_lines[line - 2].row < 0) line -= 2;
    float top = line * L.rowH, bottom = (g_lineOfRow[row] + 1) * L.rowH;
    if (top < g.scroll) g.scroll = top;
    else if (bottom > g.scroll + L.rows.h) g.scroll = bottom - L.rows.h;
    ClampScroll();
}

int LineAt(float y) {
    if (y < L.rows.y || y >= L.rows.B()) return -1;
    int line = (int)((y - L.rows.y + g.scroll) / L.rowH);
    return line >= 0 && line < (int)g_lines.size() ? line : -1;
}

float ColumnWidth(int column) {
    int w = g.settings.widths[column];
    return S((float)(w > 0 ? w : kColumns[column].width));
}

std::string TabName(size_t i) {
    return (g.playlists[i].kind == PlaylistKind::Auto ? "\xE2\x9A\xA1 " : "") + Utf8(g.playlists[i].name);
}

}  // namespace lx
