// The radio tab: a compact station grid, directory search and local favorites in the main window.
#include "app.h"
#include "radio_browser.h"
#include "radio_stream.h"
#include <shlwapi.h>

namespace app {
namespace {
constexpr wchar_t kRadioClass[] = L"CordinalRadio";
enum { kSaved = 1, kDiscover, kGenre, kCountry, kSearch, kState };
HWND tab = nullptr, view = nullptr, buttons[3]{}, genre = nullptr, country = nullptr, state = nullptr;
std::unique_ptr<radio::Browser> browser;
std::shared_ptr<radio::Stream> stream;
radio::Station playing;
unsigned long long token = 0;
bool open = false, discover = false, started = false, tabOpen = true;
std::wstring query, previousTitle;
std::vector<radio::Station> rows;
std::map<std::string, ArtImage> images;
int selected = 0, scroll = 0;

void Rebuild() {
    if (!browser) return;
    rows.clear();
    const auto& source = discover ? browser->Results() : browser->Favorites();
    radio::Search filter{ToUtf8(query), {}, {}, 0, {}};
    for (const auto& station : source) {
        if (!discover && !radio::StationMatches(station, filter)) continue;
        rows.push_back(station);
    }
    selected = std::clamp(selected, 0, std::max(0, (int)rows.size() - 1));
    if (view) InvalidateRect(view, nullptr, FALSE);
}
void Search() {
    if (!browser) return;
    query = WindowText(g.search);
    if (discover) {
        radio::Search search{ToUtf8(query), ToUtf8(WindowText(genre)), ToUtf8(WindowText(country)), 0, ToUtf8(WindowText(state))};
        browser->SearchDirectory(search);
        if (!browser->Searching() && !browser->Problem().empty()) ShowMessage(browser->Problem());
        scroll = 0;
    } else Rebuild();
    InvalidateRect(view, nullptr, FALSE);
}
void SetMode(bool value) {
    if (discover == value && (!value || browser->Searching())) return;
    discover = value; selected = scroll = 0;
    if (discover) Search(); else Rebuild();
    for (HWND button : buttons) InvalidateRect(button, nullptr, FALSE);
    InvalidateRect(view, nullptr, FALSE);
}
void ToggleFavorite(int index) {
    if (!browser || index < 0 || index >= (int)rows.size()) return;
    radio::Station station = rows[(size_t)index];
    bool was = browser->Favorite(station);
    if (!browser->ToggleFavorite(station)) ShowMessage(browser->Problem());
    else ShowMessage(was ? L"Station removed from favorites." : L"Station saved to My stations.");
    Rebuild();
}
void Play(int index) {
    if (index < 0 || index >= (int)rows.size()) return;
    radio::Station station = rows[(size_t)index];
    Stop();
    playing = std::move(station);
    if (browser) browser->RecordPlay(playing.id);
    stream = radio::Stream::Open(playing.url);
    token = g.nextToken++;
    g.firstValidToken = token;
    g.decodingToken = token;
    started = false; previousTitle.clear();
    PlayItem item; item.token = token; item.live = stream;
    g.engine.Play(item);
    ShowMessage(L"Connecting to " + FromUtf8(playing.name) + L"...");
    UpdateNowPlaying(); UpdateStatus();
    InvalidateRect(view, nullptr, FALSE); InvalidateRect(tab, nullptr, FALSE);
}
int Columns(int width) { return std::clamp((width - S(12)) / S(250), 1, 3); }
int RowHeight() { return S(82); }
int ContentTop() { return S(68); }
int MaxScroll(int width, int height) {
    int cols = Columns(width), lines = ((int)rows.size() + cols - 1) / cols;
    return std::max(0, lines * RowHeight() - std::max(0, height - ContentTop()));
}
void UpdateScroll(HWND h) {
    RECT rc{}; GetClientRect(h, &rc);
    int max = MaxScroll(rc.right, rc.bottom);
    scroll = std::clamp(scroll, 0, max);
    SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS};
    info.nMin = 0; info.nMax = max + std::max(0, (int)rc.bottom - ContentTop()) - 1;
    info.nPage = (UINT)std::max(0, (int)rc.bottom - ContentTop()); info.nPos = scroll;
    SetScrollInfo(h, SB_VERT, &info, TRUE);
}
RECT CardRect(int index, int width) {
    int cols = Columns(width), gap = S(6), pad = S(6), cw = std::max(1, (width - pad * 2 - gap * (cols - 1)) / cols);
    int x = pad + (index % cols) * (cw + gap), y = ContentTop() + (index / cols) * RowHeight() - scroll;
    return {x, y, x + cw, y + RowHeight() - gap};
}
int Hit(int x, int y, int width) {
    if (y < ContentTop()) return -1;
    for (int i = 0; i < (int)rows.size(); ++i) {
        RECT r = CardRect(i, width);
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return i;
    }
    return -1;
}
void Draw(HDC dc, RECT rc) {
    FillSolid(dc, rc, Ref(P().bg));
    FillSolid(dc, {rc.left, rc.top, rc.right, ContentTop()}, Ref(P().surface));
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ old = SelectObject(dc, g.font);
    int saved = SaveDC(dc);
    IntersectClipRect(dc, rc.left, ContentTop(), rc.right, rc.bottom);
    if (rows.empty()) {
        std::wstring text = browser && browser->Searching() ? L"Finding stations..." :
            browser && !browser->Problem().empty() ? browser->Problem() :
            discover ? L"No stations found. Try another name, genre or country." :
                       L"No saved stations yet. Choose Discover to find stations.";
        RECT r{S(16), ContentTop() + S(24), rc.right - S(16), rc.bottom};
        SetTextColor(dc, Ref(P().sub)); DrawTextW(dc, text.c_str(), -1, &r, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    }
    for (int i = 0; i < (int)rows.size(); ++i) {
        RECT r = CardRect(i, rc.right);
        if (r.bottom <= ContentTop() || r.top >= rc.bottom) continue;
        const auto& station = rows[(size_t)i];
        bool active = token && station.id == playing.id;
        bool focus = GetFocus() == view && i == selected;
        FillSolid(dc, r, Ref(focus ? P().select : P().surface));
        if (active) FillSolid(dc, {r.left, r.top, r.left + S(3), r.bottom}, Ref(P().accent));
        RECT art{r.left + S(8), r.top + S(8), r.left + S(58), r.top + S(58)};
        const ArtImage* image = nullptr;
        if (browser && !station.icon.empty()) {
            browser->RequestIcon(station.icon);
            if (const std::string* bytes = browser->Icon(station.icon)) {
                auto found = images.find(station.icon);
                if (found == images.end()) {
                    if (images.size() >= 100) images.erase(images.begin());
                    ArtImage decoded;
                    IStream* input = SHCreateMemStream((const BYTE*)bytes->data(), (UINT)bytes->size());
                    if (input) { DecodeImage(input, S(64), decoded, 4000000); input->Release(); }
                    found = images.emplace(station.icon, std::move(decoded)).first;
                }
                if (!found->second.Empty()) image = &found->second;
            }
        }
        if (image) DrawCoverArt(dc, art, *image); else DrawCoverPlaceholder(dc, art, FromUtf8(station.name));
        DrawCoverFrame(dc, art);
        RECT star{r.right - S(30), r.top + S(4), r.right - S(4), r.top + S(30)};
        SetTextColor(dc, Ref(focus ? P().selectText : browser && browser->Favorite(station) ? P().accent : P().sub));
        DrawTextW(dc, browser && browser->Favorite(station) ? L"\x2605" : L"\x2606", 1, &star,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        RECT text{art.right + S(8), r.top + S(7), star.left - S(2), r.top + S(25)};
        std::wstring name = FromUtf8(station.name);
        SelectObject(dc, g.fontBold); SetTextColor(dc, Ref(focus ? P().selectText : P().text));
        DrawTextW(dc, name.c_str(), -1, &text, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, g.font);
        std::string tags = station.tags.substr(0, station.tags.find(','));
        std::string location = station.country + (station.country.empty() || station.state.empty() ? "" : " / ") + station.state;
        std::wstring detail = FromUtf8(location + (location.empty() || tags.empty() ? "" : "  \xE2\x80\xA2  ") + tags);
        text.top += S(20); text.bottom += S(20); SetTextColor(dc, Ref(focus ? P().selectText : P().sub));
        DrawTextW(dc, detail.c_str(), -1, &text, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        std::wstring format = FromUtf8(station.codec);
        if (station.bitrate) format += (format.empty() ? L"" : L"  \x00B7  ") + std::to_wstring(station.bitrate) + L" kbps";
        if (station.hls) format += (format.empty() ? L"" : L"  \x00B7  ") + std::wstring(L"HLS");
        text.top += S(18); text.bottom += S(18);
        DrawTextW(dc, format.c_str(), -1, &text, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (focus && g.showFocusBoxes) { RECT f = r; InflateRect(&f, -S(2), -S(2)); DrawFocusRect(dc, &f); }
    }
    RestoreDC(dc, saved); SelectObject(dc, old);
}
void DrawButton(const DRAWITEMSTRUCT* di) {
    RECT r = di->rcItem; bool hot = (di->itemState & ODS_SELECTED) != 0;
    bool on = (di->CtlID == kSaved && !discover) || (di->CtlID == kDiscover && discover);
    FillSolid(di->hDC, r, Ref(hot ? P().select : P().surface));
    if (on) FillSolid(di->hDC, {r.left + S(6), r.bottom - S(2), r.right - S(6), r.bottom}, Ref(P().accent));
    wchar_t text[64]{}; GetWindowTextW(di->hwndItem, text, (int)std::size(text));
    SetBkMode(di->hDC, TRANSPARENT); SetTextColor(di->hDC, Ref(hot ? P().selectText : P().text));
    HGDIOBJ old = SelectObject(di->hDC, on ? g.fontBold : g.font);
    DrawTextW(di->hDC, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if ((di->itemState & ODS_FOCUS) && g.showFocusBoxes) { InflateRect(&r, -S(3), -S(3)); DrawFocusRect(di->hDC, &r); }
    SelectObject(di->hDC, old);
}
LRESULT CALLBACK FilterSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_KEYDOWN && wp == VK_RETURN) { Search(); return 0; }
    if (msg == WM_KEYDOWN && wp == 'A' && GetKeyState(VK_CONTROL) < 0) { SendMessageW(h, EM_SETSEL, 0, -1); return 0; }
    if (msg == WM_CHAR && wp == L'\r') return 0;
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(h, FilterSubclass, 0);
    return DefSubclassProc(h, msg, wp, lp);
}
RECT CloseRect(HWND h) {
    RECT r{}; GetClientRect(h, &r);
    r.left = std::max(r.left, r.right - S(22)); r.right -= S(3);
    InflateRect(&r, 0, -S(3)); return r;
}
LRESULT CALLBACK RadioTabSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    static bool pressed = false;
    POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    RECT close = CloseRect(h);
    if (msg == WM_LBUTTONDOWN && PtInRect(&close, point)) {
        pressed = true; SetCapture(h); InvalidateRect(h, nullptr, FALSE); return 0;
    }
    if (msg == WM_LBUTTONUP && pressed) {
        pressed = false; ReleaseCapture();
        if (PtInRect(&close, point)) CloseRadio();
        InvalidateRect(h, nullptr, FALSE); return 0;
    }
    if (msg == WM_MBUTTONUP || (msg == WM_KEYDOWN && wp == VK_DELETE)) { CloseRadio(); return 0; }
    if (msg == WM_CAPTURECHANGED || msg == WM_CANCELMODE) pressed = false;
    if (msg == WM_MOUSEMOVE) {
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, h, 0}; TrackMouseEvent(&track);
    }
    if (msg == WM_MOUSEMOVE || msg == WM_MOUSELEAVE || msg == WM_CAPTURECHANGED)
        InvalidateRect(h, nullptr, FALSE);
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(h, RadioTabSubclass, 0);
    return DefSubclassProc(h, msg, wp, lp);
}
LRESULT CALLBACK PanelProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
            return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, 0, 0, 0, 0, h,
                                   (HMENU)(INT_PTR)id, g.inst, nullptr);
        };
        buttons[0] = make(WC_BUTTONW, L"My stations", BS_OWNERDRAW, kSaved);
        buttons[1] = make(WC_BUTTONW, L"Discover", BS_OWNERDRAW, kDiscover);
        buttons[2] = make(WC_BUTTONW, L"Search", BS_OWNERDRAW, kSearch);
        genre = make(WC_EDITW, L"", ES_AUTOHSCROLL | WS_BORDER, kGenre);
        country = make(WC_EDITW, L"US", ES_AUTOHSCROLL | WS_BORDER, kCountry);
        state = make(WC_EDITW, L"", ES_AUTOHSCROLL | WS_BORDER, kState);
        SendMessageW(genre, EM_SETCUEBANNER, TRUE, (LPARAM)L"Genre (e.g. indie)");
        SendMessageW(country, EM_SETCUEBANNER, TRUE, (LPARAM)L"Country (blank = all)");
        SendMessageW(state, EM_SETCUEBANNER, TRUE, (LPARAM)L"State (e.g. Indiana)");
        for (HWND edit : {genre, country, state}) {
            SendMessageW(edit, EM_SETLIMITTEXT, 512, 0); SetWindowSubclass(edit, FilterSubclass, 0, 0);
        }
        RadioFonts();
        return 0;
    }
    case WM_SIZE: {
        int width = LOWORD(lp), gap = S(6), y = S(34), field = std::max(S(30), std::min(S(150), (width - S(70) - 5 * gap) / 3));
        MoveWindow(buttons[0], gap, S(3), S(100), S(25), TRUE);
        MoveWindow(buttons[1], S(110), S(3), S(90), S(25), TRUE);
        MoveWindow(genre, gap, y, field, S(24), TRUE);
        MoveWindow(country, gap * 2 + field, y, field, S(24), TRUE);
        MoveWindow(state, gap * 3 + field * 2, y, field, S(24), TRUE);
        MoveWindow(buttons[2], gap * 4 + field * 3, y, S(70), S(24), TRUE);
        UpdateScroll(h); return 0;
    }
    case WM_COMMAND:
        if (HIWORD(wp) == BN_CLICKED) {
            if (LOWORD(wp) == kSaved) SetMode(false);
            else if (LOWORD(wp) == kDiscover) SetMode(true);
            else if (LOWORD(wp) == kSearch) { if (!discover) SetMode(true); else Search(); }
        }
        return 0;
    case WM_DRAWITEM: DrawButton((const DRAWITEMSTRUCT*)lp); return TRUE;
    case WM_CTLCOLOREDIT: return FieldControlColor((HDC)wp);
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: DoubleBufferedPaint(h, Draw); return 0;
    case WM_MOUSEWHEEL:
        scroll -= GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * RowHeight(); UpdateScroll(h); InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_VSCROLL: {
        SCROLLINFO info{sizeof(info), SIF_ALL}; GetScrollInfo(h, SB_VERT, &info);
        switch (LOWORD(wp)) {
        case SB_LINEUP: scroll -= RowHeight(); break; case SB_LINEDOWN: scroll += RowHeight(); break;
        case SB_PAGEUP: scroll -= (int)info.nPage; break; case SB_PAGEDOWN: scroll += (int)info.nPage; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: scroll = info.nTrackPos; break;
        }
        UpdateScroll(h); InvalidateRect(h, nullptr, FALSE); return 0;
    }
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
        RECT rc{}; GetClientRect(h, &rc); int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp), hit = Hit(x, y, rc.right);
        if (hit >= 0) {
            selected = hit; RECT r = CardRect(hit, rc.right);
            if (x >= r.right - S(32) && y < r.top + S(32)) { if (msg == WM_LBUTTONDOWN) ToggleFavorite(hit); }
            else if (msg == WM_LBUTTONDBLCLK) Play(hit);
            SetFocus(h); Rebuild(); InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_GETDLGCODE: return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_KEYDOWN: {
        RECT rc{}; GetClientRect(h, &rc); int cols = Columns(rc.right);
        if (wp == VK_RETURN) Play(selected);
        else if (wp == 'F') ToggleFavorite(selected);
        else if (wp == VK_LEFT || wp == VK_RIGHT || wp == VK_UP || wp == VK_DOWN) {
            int delta = wp == VK_LEFT ? -1 : wp == VK_RIGHT ? 1 : wp == VK_UP ? -cols : cols;
            selected = std::clamp(selected + delta, 0, std::max(0, (int)rows.size() - 1));
            RECT r = CardRect(selected, rc.right);
            if (r.top < ContentTop()) scroll -= ContentTop() - r.top;
            if (r.bottom > rc.bottom) scroll += r.bottom - rc.bottom;
            UpdateScroll(h); InvalidateRect(h, nullptr, FALSE);
            Rebuild();
        }
        return 0;
    }
    case WM_SETFOCUS: case WM_KILLFOCUS: InvalidateRect(h, nullptr, FALSE); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
}  // namespace

void CreateRadio(HWND parent) {
    open = discover = false; tabOpen = true; selected = scroll = 0; query.clear();
    browser = std::make_unique<radio::Browser>(g.dataDir);
    RegisterAppClass(kRadioClass, PanelProc, CS_DBLCLKS, false);
    tab = CreateWindowExW(0, WC_BUTTONW, L"Radio", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                          0, 0, 0, 0, parent, (HMENU)(INT_PTR)IDC_RADIO_TAB, g.inst, nullptr);
    SetWindowSubclass(tab, RadioTabSubclass, 0, 0);
    view = CreateWindowExW(0, kRadioClass, L"Radio stations. Arrow keys move, Enter plays, F saves.",
                           WS_CHILD | WS_TABSTOP | WS_VSCROLL, 0, 0, 0, 0, parent, nullptr, g.inst, nullptr);
    Rebuild();
}
void DestroyRadio() { RadioCancelPlayback(); browser.reset(); images.clear(); open = false; }
HWND RadioTab() { return tab; }
HWND RadioView() { return view; }
std::vector<HWND> RadioTabStops() { return {buttons[0], buttons[1], genre, country, state, buttons[2], view}; }
bool RadioActive() { return open; }
bool RadioTabOpen() { return tabOpen; }
void CloseRadio() {
    bool focusTabs = open || GetFocus() == tab;
    tabOpen = false;
    if (open) ActivatePlaylist(g.active); else Layout();
    if (focusTabs) SetFocus(g.tabs);
    ShowMessage(L"Radio closed. Ctrl+Shift+R reopens it.");
}
bool RadioPlaying() { return token != 0; }
size_t RadioCount() { return rows.size(); }
void ActivateRadio() {
    tabOpen = true;
    if (g.size >= ViewSize::Spotlight) SetViewSize(ViewSize::Normal);
    if (open) { SetFocus(view); return; }
    CapturePlaylistPosition(); open = true;
    TabCtrl_SetCurSel(g.tabs, -1);
    g.restoringView = true; SetTextIfChanged(g.search, query); g.restoringView = false;
    UpdateSearchCue(); Rebuild(); Layout(); UpdateStatus();
    InvalidateRect(g.tabs, nullptr, FALSE); InvalidateRect(tab, nullptr, FALSE);
    SetFocus(view);
}
void LeaveRadio() {
    if (!open) return;
    query = WindowText(g.search); open = false; g.statsDirty = true;
    InvalidateRect(tab, nullptr, FALSE);
}
void RadioSearchChanged() { if (open) { query = WindowText(g.search); if (!discover) { Rebuild(); UpdateStatus(); } } }
void RadioSearch() { if (open) Search(); }
void RadioFocus() { if (view) SetFocus(view); }
void RadioPlaySelected() { if (open) Play(selected); }
void RadioFonts() {
    for (HWND h : {tab, genre, country, state, buttons[0], buttons[1], buttons[2]})
        if (h) SendMessageW(h, WM_SETFONT, (WPARAM)g.font, TRUE);
    images.clear();
}
void RadioTick() {
    if (browser) {
        bool searching = browser->Searching();
        if (browser->Poll()) {
            Rebuild();
            if (searching && !browser->Searching() && !browser->Problem().empty()) ShowMessage(browser->Problem());
            if (open) UpdateStatus();
        }
    }
    if (token && stream) {
        std::wstring title = FromUtf8(stream->Title());
        if (title != previousTitle) { previousTitle = std::move(title); UpdateNowPlaying(); }
    }
}
void RadioCancelPlayback() {
    if (stream) stream->Abort();
    stream.reset(); token = 0; playing = {}; started = false; previousTitle.clear();
    if (tab) InvalidateRect(tab, nullptr, FALSE);
    if (view) InvalidateRect(view, nullptr, FALSE);
}
bool RadioEngineEvent(EngineEvent event, unsigned long long current) {
    if (!token || (current && current != token)) return false;
    if (event == EngineEvent::Ended && !started) return true;
    if (event == EngineEvent::NeedNext) { g.engine.SetNext(token, nullptr); return true; }
    if (event == EngineEvent::TrackStarted) {
        started = true; ShowMessage(L"Playing " + FromUtf8(playing.name) + L"."); UpdateNowPlaying(); UpdateStatus(); return true;
    }
    if (event == EngineEvent::Error || event == EngineEvent::Damaged || event == EngineEvent::Ended) {
        std::wstring error = event == EngineEvent::Ended ? L"The station stopped streaming." : g.engine.TakeError(current);
        if (error.empty()) error = L"Couldn't play this station. Try again.";
        RadioCancelPlayback(); g.engine.Stop(); ShowMessage(error); UpdateNowPlaying(); UpdateStatus(); return true;
    }
    return true;
}
std::wstring RadioStationName() { return FromUtf8(playing.name); }
std::wstring RadioSongTitle() { return stream ? FromUtf8(stream->Title()) : std::wstring(); }
void DrawRadioTab(const DRAWITEMSTRUCT* di) {
    RECT r = di->rcItem;
    FillSolid(di->hDC, r, Ref(P().surface));
    FillSolid(di->hDC, {r.left, r.bottom - S(1), r.right, r.bottom}, Ref(P().border));
    if (open) FillSolid(di->hDC, {r.left + S(8), r.bottom - S(2), r.right - S(8), r.bottom}, Ref(P().accent));
    SetBkMode(di->hDC, TRANSPARENT); SetTextColor(di->hDC, Ref(token ? P().playing : open ? P().text : P().sub));
    HGDIOBJ old = SelectObject(di->hDC, g.font);
    RECT close = CloseRect(di->hwndItem);
    r.right -= S(22);
    DrawTextW(di->hDC, L"Radio", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    POINT point{}; GetCursorPos(&point); ScreenToClient(di->hwndItem, &point);
    bool hover = PtInRect(&close, point);
    if (hover) FillSolid(di->hDC, close, Ref(P().hover));
    SetTextColor(di->hDC, Ref(hover ? P().text : P().sub));
    DrawTextW(di->hDC, L"\x00D7", 1, &close, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if ((di->itemState & ODS_FOCUS) && g.showFocusBoxes) { InflateRect(&r, -S(3), -S(3)); DrawFocusRect(di->hDC, &r); }
    SelectObject(di->hDC, old);
}
}  // namespace app
