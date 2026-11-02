// The Linux window's measurements: the menu bar, transport, tabs, browser, list and status, in pixels.
#include "alternate_ui.h"
#include "big_ui.h"
#include "ui_internal.h"

namespace lx {

namespace {
Rect PlayerLayout(Rect area) {
    const auto& l = g.settings.layout;
    const unsigned hide = l.hiddenParts;
    const float pad = S(ui::kPad), tb = S(ui::kButton), timeW = S(ui::kTimeWidth);
    L.grip = L.cover = L.title = L.subtitle = L.chip = L.seek = L.elapsed = L.remaining = {};
    L.speaker = L.volume = {};
    for (auto& b : L.buttons) b = {};
    const bool now = !(hide & kPartNowPlaying), wave = !(hide & kPartWave);
    const bool transport = !(hide & kPartTransport), order = !(hide & kPartOrder);
    const bool times = !(hide & kPartTimes), volume = !(hide & kPartVolume);
    const bool player = transport || order || wave || times || volume;
    if (l.dock >= 2) {
        const float widths[] = {184, 216, 260};
        float cw = std::min(S(widths[l.waveSize]), area.w / 2);
        Rect col{l.dock == 2 ? area.x : area.R() - cw, area.y, cw, area.h};
        L.player = col;
        if (l.dock == 2) { area.x += cw + S(1); area.w -= cw + S(1); }
        else area.w -= cw + S(1);
        float in = S(12), x = col.x + in, w = std::max(0.f, cw - 2 * in);
        float y = col.y + S(ui::kGrip) + S(2), bottom = col.B() - S(6);
        L.grip = {col.x, col.y, cw, S(ui::kGrip)};
        if (now) {
            float reserve = S(ui::kGrip + 2 + 47 + 8 + 6) + (transport || order ? S(46) : 0) +
                (l.points ? S(40) : 0) + (volume ? S(34) : 0) + (times ? S(44) : 0) + (wave ? S(20) : 0);
            float art = std::max(0.f, std::min({w, S(176), col.h / 3, col.h - reserve}));
            L.cover = {col.x + (cw - art) / 2, y, art, art};
            y += art + S(8);
            L.title = {x, y, w, S(18)};
            L.subtitle = {x, y + S(19), w, S(18)};
            y += S(47);
        }
        int count = (transport ? 3 : 0) + (order ? 3 : 0);
        float gap = transport && order ? S(6) : 0;
        if (count) {
            float b = std::clamp((w - gap) / count, S(26), S(36));
            float bx = col.x + (cw - count * b - gap) / 2;
            for (int i = 0; i < 6; ++i) {
                if (i == 3 && transport && order) bx += gap;
                if (i < 3 ? !transport : !order) continue;
                L.buttons[i] = {bx, y, b, b};
                bx += b;
            }
            y += b + S(10);
        }
        if (l.points && now) { L.chip = {x, bottom - S(32), w, S(32)}; bottom -= S(40); }
        if (volume) {
            L.speaker = {x, bottom - S(22), S(20), S(18)};
            L.volume = {x + S(22), bottom - S(26), std::max(0.f, w - S(22)), S(26)};
            bottom -= S(34);
        }
        if (times) {
            L.elapsed = {x, y, w, S(18)};
            L.remaining = {x, std::max(y, bottom - S(18)), w, S(18)};
            y += S(22); bottom -= S(22);
        }
        if (wave) L.seek = {x, y, w, std::max(0.f, bottom - y)};
        return area;
    }
    const float heights[] = {26, 44, 72};
    int overrideSize = std::clamp(g.settings.bandWaveSize, 0, 5);
    float waveH = S(overrideSize ? ui::kBandWaveHeights[overrideSize] : heights[l.waveSize]);
    float rowH = l.waveOwnRow || !wave ? tb : std::max(tb, waveH);
    float playerH = player ? rowH + (wave && l.waveOwnRow ? S(4) + waveH : 0) : 0;
    float art = now ? S(l.largeCover ? 64 : ui::kCover) : 0;
    float zone = l.dock == 0 ? S(ui::kBandTop) + (player ? playerH + S(ui::kBandGap) : 0) +
        (now ? art + S(ui::kBandGap) : 0) : (now ? S(6) + art : 0) + (player ? S(8) + playerH : 0);
    float top = l.dock == 0 ? area.y : std::max(area.y, area.B() - zone);
    L.player = {area.x, top, area.w, zone};
    float py = l.dock == 0 ? top + S(ui::kBandTop) : area.B() - S(4) - playerH;
    float ny = l.dock == 0 ? py + (player ? playerH + S(ui::kBandGap) : 0) : top + S(6);
    if (player) {
        float by = py + (rowH - tb) / 2, ty = by + (tb - S(ui::kTimeHeight)) / 2 + S(1);
        L.grip = {area.x, py, S(ui::kGrip), rowH};
        float x = area.x + S(ui::kGrip), right = area.R() - pad;
        for (int i = 0; i < 6; ++i) {
            if (i == 3 && transport && order) x += S(ui::kOrderGap);
            if (i < 3 ? !transport : !order) continue;
            L.buttons[i] = {x, by, tb, tb}; x += tb;
        }
        if (transport || order) x += S(ui::kButtonsGap);
        if (volume) {
            L.volume = {right - S(ui::kVolumeWidth), by, S(ui::kVolumeWidth), tb};
            L.speaker = {L.volume.x - S(ui::kSpeakerWidth), by + S(4), S(ui::kSpeakerWidth), S(ui::kTimeHeight)};
            right = L.volume.x - S(ui::kVolumeGap);
        }
        float sy = py, sh = rowH;
        if (wave && l.waveOwnRow) {
            sy += rowH + S(4); sh = waveH;
            x = area.x + S(ui::kGrip); right = area.R() - pad;
            ty = sy + (sh - S(ui::kTimeHeight)) / 2 + S(1);
        }
        if (times) {
            L.elapsed = {x, ty, timeW, S(ui::kTimeHeight)};
            L.remaining = {right - timeW, ty, timeW, S(ui::kTimeHeight)};
            x += timeW + S(ui::kTimeGap); right -= timeW + S(ui::kTimeGap);
        }
        if (wave) L.seek = {x, sy, std::max(0.f, right - x), sh};
    }
    if (now) {
        L.cover = {area.x + pad, ny, art, art};
        float x = L.cover.R() + S(ui::kCoverGap), textY = ny + (l.largeCover ? art / 2 - S(18) : 0);
        float w = std::max(0.f, area.R() - pad - x);
        if (l.points) {
            float chipW = std::min(S(220), area.w / 3);
            L.chip = {area.R() - pad - chipW, ny + (art - S(32)) / 2, chipW, S(32)};
            w = std::max(0.f, L.chip.x - S(8) - x);
        }
        L.title = {x, textY, w, S(l.largeCover ? 18 : ui::kLineHeight)};
        L.subtitle = {x, textY + S(l.largeCover ? 20 : ui::kLineHeight), w, L.title.h};
    }
    if (l.dock == 0) { area.y += zone; area.h = std::max(0.f, area.h - zone); }
    else area.h = std::max(0.f, area.h - zone);
    return area;
}
}  // namespace

bool SeekVertical() { return g.settings.size == ViewSize::Normal && g.settings.layout.dock >= 2 && !AlternateMode(); }

void DoLayout() {
    int pw = 0, ph = 0;
    SDL_GetRenderOutputSize(g.renderer, &pw, &ph);
    float W = (float)pw, H = (float)ph, pad = S(ui::kPad);
    const auto& l = g.settings.layout;
    const unsigned hide = l.hiddenParts;
    L.menuBar = {0, 0, W, l.menuBar && g.settings.size == ViewSize::Normal ? S(ui::kMenuBarHeight) : 0};
    L.barItems.clear();
    float mx = S(1);
    if (L.menuBar.h > 0) for (int m : BarMenus()) {
        float w = TextWidth(Font::Menu, Plain(ui::kMenuBar[m])) + S(14);
        L.barItems.push_back({mx, 0, w, S(ui::kMenuBarHeight - 2)});
        mx += w;
    }
    bool normal = g.settings.size == ViewSize::Normal;
    L.status = !(hide & kPartStatus) && normal ? Rect{0, H - S(ui::kStatus), W, S(ui::kStatus)} : Rect{0, H, W, 0};
    Rect bounds{0, L.menuBar.B(), W, std::max(0.f, L.status.y - L.menuBar.B())};
    Rect content;
    const bool presentation = LayoutPresentation(bounds, content);
    if (!presentation) content = PlayerLayout(bounds);
    L.visuals[0] = L.visuals[1] = {};
    if (!presentation && g.settings.theme == ThemeMode::Foobar) {
        L.visuals[0] = {content.x + pad, content.y, S(ui::kFoobarSpectrum), S(ui::kFoobarButton)};
        L.visuals[1] = {L.visuals[0].R() + S(10), content.y,
            std::max(0.f, content.R() - L.visuals[0].R() - S(10) - pad), S(ui::kFoobarButton)};
        content.y += S(ui::kFoobarButton) + S(ui::kBandGap);
        content.h -= S(ui::kFoobarButton) + S(ui::kBandGap);
    }
    if (presentation && (content.w <= 0 || content.h <= 0)) {
        L.tabs = L.radioTab = L.search = L.viewRow = L.browser = L.splitter = L.list = {};
        L.header = L.rows = L.scrollbar = L.radioGrid = L.radioSaved = L.radioDiscover = {};
        L.radioGenre = L.radioCountry = L.radioState = L.radioSearch = {};
        L.tabRects.clear();
        for (auto& view : L.views) view = {};
        return;
    }
    float y = content.y, searchW = std::min(S(ui::kSearchWidth), std::floor(content.w / 3));
    float tabH = std::min(S(ui::kTabRow), std::max(0.f, content.B() - y));
    if (tabH < LineHeight(Font::Normal) + S(4)) tabH = 0;
    bool search = !(hide & kPartSearch) && tabH > 0, tabs = !(hide & kPartTabs) && tabH > 0;
    L.search = search ? Rect{content.R() - pad - searchW, y + S(1), searchW, S(ui::kTabRow) - S(2)} : Rect{};
    float tabsW = search ? content.w - pad - searchW - S(ui::kSearchGap) : content.w;
    float radioW = tabs && InternetRadio().tabOpen ? std::min(S(ui::kRadioTabWidth), std::max(0.f, tabsW)) : 0;
    L.tabs = {content.x, y, tabsW - radioW, search || tabs ? tabH : 0};
    L.radioTab = {L.tabs.R(), y, radioW, L.tabs.h};
    L.tabRects.clear();
    // The tabs share the strip (ui::FitTabPadding, app_tabstrip.cpp FitTabs): roomy, then tight
    // padding, then equal widths with shortened labels.
    float tx = content.x + S(2);
    std::vector<float> labels;
    for (size_t i = 0; tabs && i < g.playlists.size(); ++i)
        labels.push_back(TextWidth(Font::Normal, (int)i == g_renameIndex ? Utf8(g_renameText) : TabName(i)));
    int count = (int)labels.size(), room = (int)(L.tabs.w - S(4)), total = 0;
    for (float w : labels) total += (int)std::ceil(w);
    int tabPad = ui::FitTabPadding(total, count, room, (int)S(ui::kTabPad), (int)S(ui::kTabPadTight));
    float equal = count ? std::max(S(ui::kMinTabWidth), std::floor((float)room / count)) : 0;
    for (float label : labels) {
        float w = tabPad < 0 ? equal : label + 2 * tabPad;
        L.tabRects.push_back({tx, y, w, L.tabs.h}); tx += w;
    }
    y += L.tabs.h;
    float sideW = !presentation && (LyricsShown() || l.upNext) ? std::min(S(300), content.w / 2) : 0;
    float sideX = l.panelsLeft ? content.x : content.R() - sideW;
    if (!presentation) L.lyrics = L.upNext = {};
    float sideH = std::max(0.f, content.B() - y);
    if (!presentation && LyricsShown()) L.lyrics = {sideX, y, sideW, l.upNext ? sideH / 2 : sideH};
    if (!presentation && l.upNext) L.upNext = {sideX, y + L.lyrics.h, sideW, sideH - L.lyrics.h};
    if (l.panelsLeft) content.x += sideW;
    content.w -= sideW;
    const bool libraryControls = normal || g.settings.size == ViewSize::Big;
    bool library = libraryControls && !InternetRadio().active && !g.playlists.empty() &&
        Active().kind == PlaylistKind::Library && !(hide & kPartViews);
    L.viewRow = {content.x, y, content.w, library && content.B() - y >= S(ui::kViewRow + 24) ? S(ui::kViewRow) : 0};
    for (int i = 0, vx = (int)(content.x + pad); i < kViewButtons; ++i) {
        float bw = TextWidth(Font::Normal, ViewButtonLabel(i)) + S(16);
        L.views[i] = {i == kViewButtons - 1 ? content.R() - S(8) - bw : (float)vx,
            y + S(4), bw, S(ui::kViewButton)};
        vx += (int)(bw + S(4));
    }
    y += L.viewRow.h;
    L.browser = L.splitter = {};
    if (!InternetRadio().active && BrowserShown() && libraryControls && content.B() - y >= S(180)) {
        float h = std::clamp(BrowserHeight(), S(60), std::max(S(60), content.B() - y - S(120)));
        L.browser = {content.x, y, content.w, h};
        L.splitter = {content.x, y + h, content.w, S(5)};
        y += h + S(5);
    }
    L.list = {content.x, y, content.w, std::max(0.f, content.B() - y)};
    float gap = S(6), field = std::max(S(30), std::min(S(150), (content.w - S(70) - 5 * gap) / 3));
    float x = content.x;
    L.radioSaved = {x + gap, y + S(3), S(100), S(25)};
    L.radioDiscover = {x + S(110), y + S(3), S(90), S(25)};
    L.radioGenre = {x + gap, y + S(34), field, S(24)};
    L.radioCountry = {x + gap * 2 + field, y + S(34), field, S(24)};
    L.radioState = {x + gap * 3 + field * 2, y + S(34), field, S(24)};
    L.radioSearch = {x + gap * 4 + field * 3, y + S(34), S(70), S(24)};
    L.radioGrid = {x, y + S(68), content.w, std::max(0.f, content.B() - y - S(68))};
    L.header = {x, y, content.w, std::min(S(24), L.list.h)};
    L.rowH = S(ui::kRow);
    float sb = S(17);
    L.rows = {x, L.header.B(), content.w, std::max(0.f, content.B() - L.header.B())};
    bool scroll = g_lines.size() * L.rowH > L.rows.h;
    if (scroll) L.rows.w = std::max(0.f, L.rows.w - sb);
    L.scrollbar = {content.R() - sb, L.rows.y, scroll ? sb : 0, L.rows.h};
    ApplyAlternateLayout();
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
    int w = ActiveColumnWidths()[column];
    return S((float)(w > 0 ? w : ColumnAt(column).width));
}

std::string TabName(size_t i) {
    const auto& pl = g.playlists[i];
    std::string count;
    if (g.settings.layout.tabCounts) {
        if (pl.kind == PlaylistKind::Library || pl.kind == PlaylistKind::Auto || pl.kind == PlaylistKind::Inbox) {
            struct Count { uint32_t revision = UINT32_MAX; std::string rules; size_t value = 0; };
            static std::map<std::wstring, Count> counts;
            auto& cached = counts[pl.name];
            if (cached.revision != g_lib.Revision() || cached.rules != pl.rules) {
                cached.value = PlaylistTracks(pl).size();
                cached.revision = g_lib.Revision();
                cached.rules = pl.rules;
            }
            count = " (" + std::to_string(cached.value) + ")";
        } else count = " (" + std::to_string(pl.items.size()) + ")";
    }
    return (pl.kind == PlaylistKind::Auto ? "\xE2\x9A\xA1 " : "") + Utf8(pl.name) + count;

}

}  // namespace lx
