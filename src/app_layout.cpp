// Places every main-window control for the current view size: the transport toolbar and now-playing
// row, the big/spotlight playback strip, and the Library's tabs, search, browser, list, album views
// and lyrics panel. Positioning is done in one deferred (BeginDeferWindowPos) pass per layout to
// avoid flicker.
#include "app.h"

namespace app {

// Deferred move; a null handle is ignored and negative sizes clamp to zero. Every layout pass runs
// inside a single Begin/EndDeferWindowPos batch so the controls move together without flicker.
void Place(HDWP& dwp, HWND h, int x, int y, int w, int hgt) {
    if (h) dwp = DeferWindowPos(dwp, h, nullptr, x, y, std::max(w, 0), std::max(hgt, 0), SWP_NOZORDER | SWP_NOACTIVATE);
}

// The playlist tabs and search, the Library's view row and browser, the list (or the album views)
// and the side panels (lyrics, and in the normal window Up next), inside `r`. With `show` false they
// all hide (big mode, Library closed). The normal window can hide the tabs, the search box and the
// view row, and put the side panels on the left (app_custom.cpp).
void LayoutLibrary(HDWP& dwp, RECT r, bool show) {
    auto place = [&](HWND h, int x, int y, int w, int hgt) { Place(dwp, h, x, y, w, hgt); };
    HWND lyrics = GetDlgItem(g.hwnd, IDC_LYRICS);
    bool normal = g.size == ViewSize::Normal;
    const MainLayout& l = g.layout;
    bool tabs = show && (!normal || l.tabs), search = show && (!normal || l.search);
    bool upNext = show && normal && l.upNext;
    // The Library tab's views get a row of their own under the tabs, so a view is never mistaken
    // for another tab: the views as one segmented control on the left, the browser toggle on the right.
    bool radio = show && RadioActive();
    bool library = show && !radio && LibraryActive() && g.size != ViewSize::Compact && (!normal || l.views);
    bool browser = show && !radio && LibraryActive() && g.size != ViewSize::Compact && g.browserVisible, canvas = show && !radio && CanvasActive();
    ShowWindow(g.tabs, tabs ? SW_SHOWNA : SW_HIDE);
    ShowWindow(RadioTab(), tabs && RadioTabOpen() ? SW_SHOWNA : SW_HIDE);
    ShowWindow(g.search, search ? SW_SHOWNA : SW_HIDE);
    ShowWindow(RadioView(), radio ? SW_SHOWNA : SW_HIDE);
    ShowWindow(lyrics, show && LyricsPanelShown() ? SW_SHOWNA : SW_HIDE);
    if (normal) ShowWindow(UpNextPanel(), upNext ? SW_SHOWNA : SW_HIDE);
    for (HWND b : g.viewButtons) ShowWindow(b, library ? SW_SHOWNA : SW_HIDE);
    for (auto& pane : g.panes) ShowWindow(pane.list, browser ? SW_SHOWNA : SW_HIDE);
    if (g.libView) ShowWindow(g.libView, canvas ? SW_SHOWNA : SW_HIDE);
    ShowWindow(g.list, show && !canvas && !radio ? SW_SHOWNA : SW_HIDE);
    if (!show || canvas) ReleaseGroupArt();
    g.splitRect = {};
    if (!show) return;
    // The measurements are shared with the Linux port (ui_spec.h).
    int pad = S(ui::kPad), x0 = r.left, W = r.right - r.left, rowC = tabs || search ? S(ui::kTabRow) : 0;
    int searchW = search ? std::min(S(ui::kSearchWidth), W / 3) : 0;
    if (search) place(g.search, r.right - pad - searchW, r.top + S(1), searchW, rowC - S(2));
    int tabW = W - (search ? pad + searchW + S(ui::kSearchGap) : 0);
    int radioW = tabs && RadioTabOpen() ? std::min(S(ui::kRadioTabWidth), std::max(0, tabW)) : 0;
    if (tabs) {
        place(g.tabs, x0, r.top, tabW - radioW, rowC);
        place(RadioTab(), x0 + tabW - radioW, r.top, radioW, rowC);
    }
    int yL = r.top + rowC, bottom = r.bottom;
    // The side panels take one side under the tabs (the right unless the layout says left), Up next
    // over the lyrics when both are open; the views share what's left.
    int lyricsW = std::min(LyricsPanelWidth(), W / 2), nextW = upNext ? std::min(S(300), W / 2) : 0;
    if (int sideW = std::max(lyricsW, nextW)) {
        bool left = normal && l.panelsLeft;
        int sx = left ? x0 : (int)r.right - sideW, split = lyricsW && nextW ? yL + (bottom - yL) / 2 : bottom;
        if (nextW) place(UpNextPanel(), sx, yL, sideW, split - yL);
        if (lyricsW) place(lyrics, sx, nextW ? split : yL, sideW, bottom - (nextW ? split : yL));
        if (left) x0 += sideW;
        W -= sideW;
    }
    if (radio) {
        place(RadioView(), x0, yL, W, bottom - yL);
        return;
    }
    for (int i = 0, vx = x0 + pad; library && i < kViewButtons; ++i) {
        int bw = ViewButtonWidth(i);
        bool toggle = i == kViewButtons - 1;
        place(g.viewButtons[i], toggle ? x0 + W - S(8) - bw : vx, yL + S(4), bw, S(ui::kViewButton));
        vx += bw + S(4);
    }
    if (library) yL += S(ui::kViewRow);
    if (browser) {
        int h = std::clamp(S(g.browserHeight), S(60), std::max(S(60), bottom - yL - S(120)));
        int px = 0;
        for (int i = 0; i < 3; ++i) {
            int w = i < 2 ? W / 3 : W - px;
            place(g.panes[i].list, x0 + px, yL, w - (i < 2 ? 1 : 0), h);
            px += w;
        }
        g.splitRect = {x0, yL + h, x0 + W, yL + h + S(5)};
        yL += h + S(5);
    }
    if (g.libView) place(g.libView, x0, yL, W, bottom - yL);
    place(g.list, x0, yL, W, bottom - yL);
}

// Fixed height of the big / spotlight playback strip, in 96-DPI-scaled pixels.
int StripHeight() { return S(140); }

// The strip's controls under the waveform: hidden until the mouse is over the strip.
void ShowStripControls(bool show) {
    for (HWND h : {g.buttons[0], g.buttons[1], g.buttons[2], g.buttons[3], g.buttons[4], g.buttons[5], g.volume,
                   g.speaker, g.elapsed, g.time})
        ShowWindow(h, show ? SW_SHOWNA : SW_HIDE);
}

// The spotlight's and big mode's playback strip: a big, wide waveform seek bar filling it. While
// the mouse is over the strip the waveform moves up to make room under it for the times, large
// shuffle / previous / play / next / repeat buttons in the middle and the volume on the right.
void ArrangeStrip(HDWP& dwp) {
    auto place = [&](HWND h, int x, int y, int w, int hgt) { Place(dwp, h, x, y, w, hgt); };
    const RECT& r = g.stripRect;
    int pad = S(24), W = r.right - r.left, timeW = S(58), big = S(50);
    int y = r.top + std::max(0, (int)(r.bottom - r.top) - StripHeight()) / 2 + S(5);
    if (!g.stripHover) {
        place(g.seek, r.left + pad, y, W - 2 * pad, StripHeight() - S(10));
        ShowStripControls(false);
        return;
    }
    int bar = StripHeight() - S(10) - big - S(6);
    place(g.seek, r.left + pad, y, W - 2 * pad, bar);
    y += bar + S(6);
    place(g.elapsed, r.left + pad, y + (big - S(18)) / 2, timeW, S(18));
    place(g.time, r.left + pad + timeW + S(6), y + (big - S(18)) / 2, timeW, S(18));
    int left = r.left + W / 2 - big * 6 / 2;
    // Shuffle and resume sit left of the transport cluster (previous / play / next) and Repeat to
    // its right: buttons[] is {prev, play, next, shuffle, repeat, resume}, hence this order.
    const int order[] = {3, 5, 0, 1, 2, 4};
    for (int i = 0; i < 6; ++i) place(g.buttons[order[i]], left + i * big, y, big, big);
    int volW = std::min(S(120), std::max(0, (int)r.right - pad - (left + 6 * big) - S(36)));
    place(g.volume, r.right - pad - volW, y + (big - S(26)) / 2, volW, S(26));
    place(g.speaker, r.right - pad - volW - S(22), y + (big - S(18)) / 2, S(20), S(18));
    ShowStripControls(true);
}

// Records the strip's rectangle (used for hover polling) and arranges its contents into it.
void LayoutStrip(HDWP& dwp, RECT r) {
    g.stripRect = r;
    ArrangeStrip(dwp);
}

// Polled by kStripTimer (app_sizes.cpp) while a strip is on screen: the common control gives no
// hover messages for the strip as a whole, so the cursor is tested against g.stripRect. Controls
// stay shown while another window holds capture, so dragging the volume out of the strip doesn't
// hide the controls mid-drag.
void StripHoverTick() {
    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(g.hwnd, &pt);
    HWND capture = GetCapture();
    bool hover = (PtInRect(&g.stripRect, pt) && IsWindowVisible(g.hwnd) && !IsIconic(g.hwnd)) ||
                 (capture && capture != g.hwnd && IsChild(g.hwnd, capture));  // dragging the volume
    if (hover == g.stripHover) return;
    g.stripHover = hover;
    HDWP dwp = BeginDeferWindowPos(12);
    ArrangeStrip(dwp);
    EndDeferWindowPos(dwp);
    InvalidateRect(g.seek, nullptr, TRUE);
}


void Layout() {
    FoobarSync();
    ApolloSync();
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    int W = rc.right;
    g.gripRect = {};
    g.dividers.clear();
    AlignTimes(false);  // the player column centers them (LayoutNormal)
    ShowWindow(g.seek, SW_SHOWNA);  // the normal window can hide it
    bool compact = g.size == ViewSize::Compact, spotlight = g.size == ViewSize::Spotlight, big = g.size == ViewSize::Big;
    SendMessageW(g.status, WM_SIZE, 0, 0);
    RECT sr;
    GetWindowRect(g.status, &sr);
    // Only the normal window has the status bar and points chip (and the menu bar, app_sizes.cpp).
    // The compact one keeps the slim now-playing row; the others show the Now playing panel.
    bool normal = g.size == ViewSize::Normal;
    // The normal window can hide its status bar and now playing line (app_custom.cpp, LayoutNormal).
    bool status = normal && (g.layout.status || ApolloMode());  // Apollo always has its status bar
    ShowWindow(g.status, status ? SW_SHOWNA : SW_HIDE);
    ShowWindow(g.pointsChip, normal ? SW_SHOWNA : SW_HIDE);
    for (HWND h : {g.npTitle, g.npSub}) ShowWindow(h, normal || compact ? SW_SHOWNA : SW_HIDE);
    int statusH = status ? sr.bottom - sr.top : 0;
    int parts[] = {std::max(S(100), W - S(ui::kStatusDetails)), std::max(S(200), W - S(ui::kStatusCounts)), -1};
    // foobar2000's status bar is one line of text; Apollo's is the tracks and the selection.
    int apollo[] = {S(150), -1};
    if (ApolloMode()) SendMessageW(g.status, SB_SETPARTS, 2, (LPARAM)apollo);
    else SendMessageW(g.status, SB_SETPARTS, FoobarMode() ? 1 : 3, (LPARAM)(FoobarMode() ? parts + 2 : parts));
    // Every part owner drawn and borderless from the start, even one whose text stays empty (and
    // so is never set by SetStatusPart).
    for (int i = 0; i < 3; ++i) SendMessageW(g.status, SB_SETTEXTW, i | SBT_OWNERDRAW | SBT_NOBORDERS, i);

    // foobar2000 default UI order: toolbar (transport, order, seekbar, volume), a slim
    // now-playing strip, playlist tabs + search, the playlist, then the status bar.
    int pad = S(4), tb = S(26), bottom = rc.bottom - statusH;
    HDWP dwp = BeginDeferWindowPos(40);
    auto place = [&](HWND h, int x, int y, int w, int hgt) { Place(dwp, h, x, y, w, hgt); };
    g.artRect = g.stripRect = {};
    if (!spotlight && !big) ShowStripControls(true);
    if (big) {
        LayoutBig(dwp, {0, 0, W, bottom});
    } else if (spotlight) {
        // The spotlight: just the music. Now playing over the player in the middle, then what
        // comes next.
        int stripH = StripHeight(), mid = std::max(0, bottom - stripH) / 2;
        place(NowPlayingPanel(), 0, 0, W, mid);
        LayoutStrip(dwp, {0, mid, W, mid + stripH});
        place(UpNextPanel(), 0, mid + stripH, W, bottom - mid - stripH);
        for (HWND h : {NowPlayingPanel(), UpNextPanel()}) ShowWindow(h, SW_SHOWNA);
        LayoutLibrary(dwp, {}, false);
    } else {
        ShowWindow(NowPlayingPanel(), SW_HIDE);
        ShowWindow(UpNextPanel(), SW_HIDE);
        if (compact) {
            // The player first: a larger cover beside the title, the seek bar, then bigger transport
            // buttons in the middle, shuffle / repeat on the left and the volume on the right.
            int art = S(64), timeW = S(48), large = S(38), y = S(8);
            g.artRect = {pad + S(4), y, pad + S(4) + art, y + art};
            int x0 = g.artRect.right + S(10);
            place(g.npTitle, x0, y + S(12), W - x0 - pad, S(18));
            place(g.npSub, x0, y + S(32), W - x0 - pad, S(18));
            y += art + S(6);
            place(g.elapsed, pad, y + S(5), timeW, S(18));
            place(g.time, W - pad - timeW, y + S(5), timeW, S(18));
            place(g.seek, pad + timeW + S(4), y, W - 2 * (pad + timeW + S(4)), tb);
            y += tb + S(4);
            int cx = W / 2 - large * 3 / 2;
            for (int i = 0; i < 3; ++i) place(g.buttons[i], cx + i * large, y, large, large);
            place(g.buttons[3], pad + S(4), y, large, large);
            place(g.buttons[4], pad + S(4) + large, y, large, large);
            place(g.buttons[5], pad + S(4) + 2 * large, y, large, large);
            int volW = std::min(S(90), W - pad - (cx + 3 * large) - S(28));
            place(g.volume, W - pad - volW, y + (large - tb) / 2, volW, tb);
            place(g.speaker, W - pad - volW - S(20), y + (large - S(18)) / 2, S(20), S(18));
            LayoutLibrary(dwp, {0, y + large + S(6), W, bottom}, true);
        } else {
            // The normal window: the player docked where the layout says, around the Library.
            if (FoobarMode()) LayoutFoobar(dwp, {0, 0, W, bottom});
            else if (ApolloMode()) LayoutApollo(dwp, {0, 0, W, bottom});
            else LayoutNormal(dwp, {0, 0, W, bottom});
        }
    }
    EndDeferWindowPos(dwp);
    ApolloTick();  // the Title column fills the list's new width
    // Browser panes: the value column takes whatever the count column and scrollbar leave.
    for (auto& pane : g.panes) {
        RECT pr;
        GetClientRect(pane.list, &pr);
        ListView_SetColumnWidth(pane.list, 0, std::max(S(60), (int)pr.right - S(48)));
    }
    InvalidateRect(g.hwnd, nullptr, TRUE);
}

}  // namespace app
