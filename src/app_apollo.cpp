// The Apollo theme's window, in Apollo 37zz's main window arrangement (Heikki Ylinen, 1998-2007;
// the art is Apollo's own, built into the executable from src/apollo):
//   - the rebar, its background one of Apollo's four bitmaps at 40% opacity (right-click it to
//     choose), with three bands on each row:
//       the toolbar: open, playlist and options (each with its dropdown), previous, stop, pause,
//       play, next, loop (with its dropdown), shuffle, frequency analyzer, oscilloscope,
//       visualization (here the equalizer), mini window and tray, drawn from Apollo's own strip;
//       "Position" and "Volume" with classic sunken grooves and raised thumbs
//     and under them the status window ("1. Artist - Title 0:34 (3:12)"), the frequency analyzer
//     and the oscilloscope, which draw what the engine is playing
//   - the playlist: No, Title (with Apollo's track icon), Duration, Bit Rate, Sample Rate, Bit
//     Resolution, File Size and ID3 Tag; the playing row in bold; the Title column fills the width
//   - the status bar's two sunken parts: "10 tracks (45:07)" and the selection
// Apollo has no menu bar: the File menu hangs off the open button, the playlists off the playlist
// button and every other menu off the options button. It applies only in the normal window size;
// the other sizes keep Cordinal's own arrangement in Apollo's colors.
#include "app.h"

#include <wincodec.h>

namespace app {

bool ApolloMode() { return g.themeMode == ThemeMode::Apollo && g.size == ViewSize::Normal && !RadioActive(); }

namespace {

constexpr wchar_t kBarClass[] = L"CordinalApolloToolbar", kBoxClass[] = L"CordinalApolloBox";
constexpr UINT_PTR kVisTimer = 1;
constexpr int kSkins = 4;  // Capacala, butterflies, letters, notes (IDR_APOLLO_REBAR + i); kSkins: none
const wchar_t* const kSkinNames[] = {L"&Capacala", L"&Butterflies", L"&Letters", L"&Notes", L"N&one"};
const int kOpacities[] = {20, 40, 60, 80, 100};

// The glyphs of Apollo's toolbar strip, in its order.
enum Glyph { kEject, kHammer, kPrev, kStop, kPause, kPlay, kNext, kLoop, kDice, kAnalyzer, kScope, kList, kMini, kVis, kTray };
enum class Act { Open, Playlists, Options, Prev, Stop, Pause, Play, Next, Loop, Shuffle, Analyzer, Scope, Vis, Mini, Tray };
struct Tool {
    int glyph;  // -1: a separator
    Act act;
    bool drop;
    const wchar_t* tip;
};
const Tool kTools[] = {
    {kEject, Act::Open, true, L"Add files"},
    {kList, Act::Playlists, true, L"Playlists"},
    {kHammer, Act::Options, true, L"Preferences"},
    {-1, Act::Open, false, nullptr},
    {kPrev, Act::Prev, false, L"Previous track"},
    {kStop, Act::Stop, false, L"Stop"},
    {kPause, Act::Pause, false, L"Pause"},
    {kPlay, Act::Play, false, L"Play"},
    {kNext, Act::Next, false, L"Next track"},
    {-1, Act::Open, false, nullptr},
    {kLoop, Act::Loop, true, L"Loop"},
    {kDice, Act::Shuffle, false, L"Shuffle"},
    {kAnalyzer, Act::Analyzer, false, L"Frequency analyzer"},
    {kScope, Act::Scope, false, L"Oscilloscope"},
    {kVis, Act::Vis, false, L"Equalizer"},
    {-1, Act::Open, false, nullptr},
    {kMini, Act::Mini, false, L"Mini window"},
    {kTray, Act::Tray, false, L"Minimize to tray"},
};
constexpr int kToolCount = (int)std::size(kTools);

enum Box { kStatusBox, kAnalyzerBox, kScopeBox, kBoxes };

struct Apollo {
    bool on = false, themed = false;
    HWND bar = nullptr, boxes[kBoxes] = {};
    // Toolbar state: the hot tool, the pressed one and whether its dropdown arrow was pressed.
    int hot = -1, pressed = -1;
    bool pressedArrow = false, menuOpen = false;
    // Persisted in [Apollo].
    bool analyzer = true, scope = true;
    int skin = 0, opacity = 40;
    // The rebar's layout, in main-window client coordinates.
    RECT rebar{}, rows[2]{}, listFrame{};
    std::vector<RECT> bands;
    RECT labels[2]{};  // "Position" and "Volume"
    // Art.
    HBITMAP strip = nullptr;
    HDC stripDC = nullptr;
    HBRUSH skinBrush = nullptr, dither = nullptr;
    int skinFor = -1, opacityFor = -1;
    Rgb faceFor = 0;
    HICON trackIcon = nullptr, bigIcon = nullptr, smallIcon = nullptr;
    int iconDpi = 0;
    // The status window and the visualizations.
    std::wstring status;
    std::vector<float> levels, peaks, wave, spectrum;  // analyzer rows, stereo samples and current magnitudes
    std::vector<int> hold;                   // frames each peak still holds before it falls
    int titleFitFor = -1;
    bool showingMessage = false;
} ap;

// The classic 3D colors, from the palette: Apollo's face is the surface, its highlight the white
// list background, its shadow the border and its dark shadow the secondary text.
COLORREF Face() { return SurfaceColor(); }
COLORREF Light() { return Ref(P().bg); }
COLORREF Shadow() { return Ref(P().border); }
COLORREF DarkShadow() { return Ref(P().sub); }

COLORREF Mix(COLORREF a, COLORREF b, int percentA) {
    auto c = [&](int x, int y) { return (x * percentA + y * (100 - percentA)) / 100; };
    return RGB(c(GetRValue(a), GetRValue(b)), c(GetGValue(a), GetGValue(b)), c(GetBValue(a), GetBValue(b)));
}

// A one-pixel frame: `tl` along the top and left, `br` along the bottom and right.
void Frame(HDC dc, const RECT& r, COLORREF tl, COLORREF br) {
    FillSolid(dc, {r.left, r.top, r.right - 1, r.top + 1}, tl);
    FillSolid(dc, {r.left, r.top, r.left + 1, r.bottom - 1}, tl);
    FillSolid(dc, {r.left, r.bottom - 1, r.right, r.bottom}, br);
    FillSolid(dc, {r.right - 1, r.top, r.right, r.bottom}, br);
}

// The client edge of Apollo's boxes and playlist: two pixels, sunken.
void Sunken(HDC dc, RECT r) {
    Frame(dc, r, Shadow(), Light());
    InflateRect(&r, -1, -1);
    Frame(dc, r, DarkShadow(), Face());
}

// An etched line, as rebar band borders are drawn: shadow, then highlight.
void EtchedH(HDC dc, int left, int right, int y) {
    FillSolid(dc, {left, y, right, y + 1}, Shadow());
    FillSolid(dc, {left, y + 1, right, y + 2}, Light());
}
void EtchedV(HDC dc, int x, int top, int bottom) {
    FillSolid(dc, {x, top, x + 1, bottom}, Shadow());
    FillSolid(dc, {x + 1, top, x + 2, bottom}, Light());
}

// Decodes the PNG resource `id` into 32-bit BGRA rows, top-down.
bool LoadPng(int id, int& w, int& h, std::vector<uint32_t>& px) {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    HGLOBAL data = res ? LoadResource(nullptr, res) : nullptr;
    const BYTE* bytes = data ? (const BYTE*)LockResource(data) : nullptr;
    if (!bytes) return false;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(bytes, SizeofResource(nullptr, res)));
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapSource> bgra;
    if (!stream || FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(WICConvertBitmapSource(GUID_WICPixelFormat32bppBGRA, frame.Get(), &bgra)))
        return false;
    UINT uw = 0, uh = 0;
    if (FAILED(bgra->GetSize(&uw, &uh)) || !uw || !uh || uw > 1024 || uh > 1024) return false;
    w = (int)uw;
    h = (int)uh;
    px.resize((size_t)w * h);
    return SUCCEEDED(bgra->CopyPixels(nullptr, uw * 4, (UINT)(px.size() * 4), (BYTE*)px.data()));
}

HBITMAP MakeDib(int w, int h, const std::vector<uint32_t>& px) {
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(bi.bmiHeader), w, -h, 1, 32};
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp && bits) memcpy(bits, px.data(), px.size() * 4);
    return bmp;
}

constexpr uint32_t kKey = 0xFF00FF00;  // Apollo's transparency color, pure green, as BGRA

// The toolbar strip, kept selected into a memory DC for TransparentBlt.
bool StripReady() {
    if (ap.stripDC) return true;
    int w, h;
    std::vector<uint32_t> px;
    if (!LoadPng(IDR_APOLLO_TOOLBAR, w, h, px)) return false;
    ap.strip = MakeDib(w, h, px);
    ap.stripDC = CreateCompatibleDC(nullptr);
    SelectObject(ap.stripDC, ap.strip);
    return true;
}

// The rebar background: the skin blended onto the face at the chosen opacity, as a pattern brush.
void BuildSkin() {
    if (ap.skinFor == ap.skin && ap.opacityFor == ap.opacity && ap.faceFor == P().surface) return;
    ap.skinFor = ap.skin;
    ap.opacityFor = ap.opacity;
    ap.faceFor = P().surface;
    if (ap.skinBrush) DeleteObject(ap.skinBrush);
    ap.skinBrush = nullptr;
    int w, h;
    std::vector<uint32_t> px;
    if (ap.skin < 0 || ap.skin >= kSkins || !LoadPng(IDR_APOLLO_REBAR + ap.skin, w, h, px)) return;
    Rgb face = P().surface;
    int a = ap.opacity;
    for (uint32_t& p : px) {
        if ((p | 0xFF000000) == kKey) {
            p = 0xFF000000 | face;
            continue;
        }
        auto blend = [&](int shift) {
            int f = face >> shift & 0xFF, s = p >> shift & 0xFF;
            return (uint32_t)((f * (100 - a) + s * a) / 100) << shift;
        };
        p = 0xFF000000 | blend(16) | blend(8) | blend(0);
    }
    // A packed bottom-up DIB: CreateDIBPatternBrushPt copies it (CreatePatternBrush would keep
    // using a bitmap that must then outlive the brush).
    // 24-bit rows, each padded to four bytes.
    size_t stride = ((size_t)w * 3 + 3) & ~(size_t)3;
    std::vector<BYTE> dib(sizeof(BITMAPINFOHEADER) + stride * h);
    auto* bih = reinterpret_cast<BITMAPINFOHEADER*>(dib.data());
    *bih = {sizeof(BITMAPINFOHEADER), w, h, 1, 24};
    for (int y = 0; y < h; ++y) {
        BYTE* row = dib.data() + sizeof(BITMAPINFOHEADER) + (size_t)(h - 1 - y) * stride;
        for (int x = 0; x < w; ++x) memcpy(row + x * 3, &px[(size_t)y * w + x], 3);
    }
    ap.skinBrush = CreateDIBPatternBrushPt(dib.data(), DIB_RGB_COLORS);
}

// Fills `r` (in `dc`, a DC for `from`'s client area) with the rebar background, aligned to the main
// window so the pattern runs on unbroken under every band and control.
void FillRebar(HDC dc, const RECT& r, HWND from) {
    BuildSkin();
    if (!ap.skinBrush) {
        FillSolid(dc, r, Face());
        return;
    }
    POINT o{0, 0};
    if (from && from != g.hwnd) MapWindowPoints(from, g.hwnd, &o, 1);
    POINT old;
    SetBrushOrgEx(dc, -o.x, -o.y, &old);
    FillRect(dc, &r, ap.skinBrush);
    SetBrushOrgEx(dc, old.x, old.y, nullptr);
}

int GlyphScale() { return std::max(1, ((int)g.dpi + 24) / 96); }

void LoadIcons() {
    if (ap.iconDpi == (int)g.dpi && ap.trackIcon) return;
    if (ap.trackIcon) DestroyIcon(ap.trackIcon);
    ap.trackIcon = (HICON)LoadImageW(g.inst, MAKEINTRESOURCEW(IDI_APOLLO_TRACK), IMAGE_ICON, S(16), S(16), 0);
    ap.iconDpi = (int)g.dpi;
}

// ---------------------------------------------------------------------------
// The toolbar

int ButtonW() { return S(23); }
int ArrowW() { return S(11); }
int SeparatorW() { return S(8); }

int ToolWidth(int i) {
    const Tool& t = kTools[i];
    return t.glyph < 0 ? SeparatorW() : ButtonW() + (t.drop ? ArrowW() : 0);
}

int ToolbarWidth() {
    int w = 0;
    for (int i = 0; i < kToolCount; ++i) w += ToolWidth(i);
    return w;
}

RECT ToolRect(int i) {
    int x = 0;
    for (int k = 0; k < i; ++k) x += ToolWidth(k);
    return {x, 0, x + ToolWidth(i), S(22)};
}

int ToolAt(POINT pt, bool* arrow = nullptr) {
    for (int i = 0; i < kToolCount; ++i) {
        RECT r = ToolRect(i);
        if (kTools[i].glyph >= 0 && PtInRect(&r, pt)) {
            if (arrow) *arrow = kTools[i].drop && pt.x >= r.right - ArrowW();
            return i;
        }
    }
    return -1;
}

bool Checked(Act a) {
    switch (a) {
        case Act::Playlists: return true;  // Apollo's playlist button: the playlist is showing
        case Act::Pause: return g.playing != kNone && g.engine.Paused();
        case Act::Loop: return g.repeat != Repeat::Off;
        case Act::Shuffle: return g.shuffle;
        case Act::Analyzer: return ap.analyzer;
        case Act::Scope: return ap.scope;
        default: return false;
    }
}

HBRUSH Dither() {
    if (!ap.dither) {
        const WORD rows[8] = {0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA};
        HBITMAP bmp = CreateBitmap(8, 8, 1, 1, rows);
        ap.dither = CreatePatternBrush(bmp);
        DeleteObject(bmp);
    }
    return ap.dither;
}

void DrawGlyph(HDC dc, int glyph, int cx, int cy) {
    if (!StripReady()) return;
    int k = GlyphScale(), size = 16 * k;
    GdiTransparentBlt(dc, cx - size / 2, cy - size / 2, size, size, ap.stripDC, glyph * 16, 0, 16, 16, RGB(0, 255, 0));
}

void PaintBar(HDC dc, RECT rc) {
    FillRebar(dc, rc, ap.bar);
    for (int i = 0; i < kToolCount; ++i) {
        const Tool& t = kTools[i];
        RECT r = ToolRect(i);
        if (t.glyph < 0) {
            int x = (r.left + r.right) / 2 - 1;
            EtchedV(dc, x, r.top + S(1), r.bottom - S(1));
            continue;
        }
        RECT button = r, arrow = r;
        if (t.drop) {
            button.right -= ArrowW();
            arrow.left = button.right;
        }
        bool hot = ap.hot == i, down = ap.pressed == i, checked = Checked(t.act);
        bool buttonDown = down && !ap.pressedArrow, arrowDown = down && (ap.pressedArrow || ap.menuOpen);
        if (checked && !buttonDown) {
            // A checked button: the classic dithered face, sunken.
            SetTextColor(dc, Light());
            SetBkColor(dc, Face());
            RECT in = button;
            InflateRect(&in, -1, -1);
            POINT old;
            SetBrushOrgEx(dc, 0, 0, &old);
            FillRect(dc, &in, Dither());
            SetBrushOrgEx(dc, old.x, old.y, nullptr);
        }
        if (buttonDown || checked) Frame(dc, button, Shadow(), Light());
        else if (hot || arrowDown) Frame(dc, button, Light(), Shadow());
        if (t.drop) {
            if (arrowDown) Frame(dc, arrow, Shadow(), Light());
            else if (hot || buttonDown) Frame(dc, arrow, Light(), Shadow());
            int ax = (arrow.left + arrow.right) / 2 + (arrowDown ? 1 : 0), ay = (arrow.top + arrow.bottom) / 2 + (arrowDown ? 1 : 0);
            int u = std::max(1, S(1));
            for (int row = 0; row < 3; ++row)  // the dropdown's little black arrow, five pixels wide
                FillSolid(dc, {ax - (2 - row) * u, ay + (row - 1) * u, ax + (3 - row) * u, ay + row * u}, Ref(P().text));
        }
        int off = buttonDown || checked ? 1 : 0;
        DrawGlyph(dc, t.glyph, (button.left + button.right) / 2 + off, (button.top + button.bottom) / 2 + off);
    }
}

void PlaceTips() {
    if (!g.tips || !ap.bar) return;
    for (int i = 0; i < kToolCount; ++i) {
        if (kTools[i].glyph < 0) continue;
        TTTOOLINFOW ti{sizeof(ti)};
        ti.hwnd = ap.bar;
        ti.uId = (UINT_PTR)(i + 1);
        ti.rect = ToolRect(i);
        SendMessageW(g.tips, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
    }
}

HMENU BarMenu(const wchar_t* name) {
    HMENU bar = MainMenuBar();
    for (int i = 0, n = bar ? GetMenuItemCount(bar) : 0; i < n; ++i) {
        wchar_t text[64] = {}, plain[64] = {};
        GetMenuStringW(bar, i, text, (int)std::size(text), MF_BYPOSITION);
        for (int a = 0, b = 0; text[a] && b < 63; ++a)
            if (text[a] != L'&') plain[b++] = text[a];
        if (!_wcsicmp(plain, name)) return GetSubMenu(bar, i);
    }
    return nullptr;
}

// Runs a popup that borrows menus of the (detached) menu bar: they are taken out again before the
// popup is destroyed, since destroying it would destroy them too. Returns the chosen id.
int TrackWithBorrowed(HMENU m, int firstBorrowed, int borrowed, POINT pt, UINT align) {
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | align, pt.x, pt.y, 0, g.hwnd, nullptr);
    for (int i = 0; i < borrowed; ++i) RemoveMenu(m, firstBorrowed, MF_BYPOSITION);
    DestroyMenu(m);
    return cmd;
}

void SaveApollo() {
    IniWrite(L"Apollo", L"Analyzer", ap.analyzer ? L"1" : L"0");
    IniWrite(L"Apollo", L"Oscilloscope", ap.scope ? L"1" : L"0");
    IniWrite(L"Apollo", L"Rebar", std::to_wstring(ap.skin));
    IniWrite(L"Apollo", L"RebarOpacity", std::to_wstring(ap.opacity));
}

void Relayout() {
    Layout();
    RedrawWindow(g.hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

// The options menu (the options button's arrow, or a right-click on the rebar): Preferences, every
// menu of the menu bar, and the rebar's background and its opacity.
void OptionsMenu(POINT pt, UINT align) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_PREFERENCES, L"&Preferences...\tCtrl+,");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    int borrowed = 0;
    if (HMENU bar = MainMenuBar())
        for (int i = 0, n = GetMenuItemCount(bar); i < n; ++i) {
            wchar_t text[64] = {};
            GetMenuStringW(bar, i, text, (int)std::size(text), MF_BYPOSITION);
            if (HMENU sub = GetSubMenu(bar, i)) {
                AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, text);
                ++borrowed;
            }
        }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    HMENU skins = CreatePopupMenu(), opacity = CreatePopupMenu();
    for (int i = 0; i <= kSkins; ++i) AppendMenuW(skins, MF_STRING | (ap.skin == i ? MF_CHECKED : 0), 1 + i, kSkinNames[i]);
    for (int i = 0; i < (int)std::size(kOpacities); ++i) {
        std::wstring label = std::to_wstring(kOpacities[i]) + L" %";
        AppendMenuW(opacity, MF_STRING | (ap.opacity == kOpacities[i] ? MF_CHECKED : 0) | (ap.skin >= kSkins ? MF_GRAYED : 0),
                    20 + i, label.c_str());
    }
    AppendMenuW(m, MF_POPUP, (UINT_PTR)skins, L"Rebar &background");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)opacity, L"Background &opacity");
    AppendMenuW(m, MF_STRING | (ap.analyzer ? MF_CHECKED : 0), 30, L"Frequency &analyzer");
    AppendMenuW(m, MF_STRING | (ap.scope ? MF_CHECKED : 0), 31, L"Osc&illoscope");
    int cmd = TrackWithBorrowed(m, 2, borrowed, pt, align);
    if (cmd >= 1 && cmd <= 1 + kSkins) ap.skin = cmd - 1;
    else if (cmd >= 20 && cmd < 20 + (int)std::size(kOpacities)) ap.opacity = kOpacities[cmd - 20];
    else if (cmd == 30) ap.analyzer = !ap.analyzer;
    else if (cmd == 31) ap.scope = !ap.scope;
    else if (cmd) {
        OnCommand(cmd, 0, nullptr);
        return;
    }
    if (!cmd) return;
    SaveApollo();
    g.engine.SetScope(ap.on && (ap.analyzer || ap.scope));
    Relayout();
}

// The playlist button: every playlist (the open one checked), then the Edit and Playlist menus.
void PlaylistsMenu(POINT pt, UINT align) {
    HMENU m = CreatePopupMenu();
    for (int i = 0; i < (int)g.playlists.size() && i < 200; ++i) {
        std::wstring name = g.playlists[i].name;
        for (size_t at = name.find(L'&'); at != std::wstring::npos; at = name.find(L'&', at + 2)) name.insert(at, 1, L'&');
        AppendMenuW(m, MF_STRING | (i == g.active ? MF_CHECKED : 0), 1 + i, name.c_str());
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    int first = GetMenuItemCount(m), borrowed = 0;
    for (const wchar_t* name : {L"Edit", L"Playlist"})
        if (HMENU sub = BarMenu(name)) {
            AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, name);
            ++borrowed;
        }
    int cmd = TrackWithBorrowed(m, first, borrowed, pt, align);
    if (cmd >= 1 && cmd <= 200) ActivatePlaylist(cmd - 1);
    else if (cmd) OnCommand(cmd, 0, nullptr);
}

void LoopMenu(POINT pt, UINT align) {
    HMENU m = CreatePopupMenu();
    const wchar_t* names[] = {L"Loop o&ff", L"Loop &playlist", L"Loop &track"};
    for (int i = 0; i < (int)Repeat::Count; ++i)
        AppendMenuW(m, MF_STRING | (g.repeat == (Repeat)i ? MF_CHECKED : 0), IDM_REPEAT_BASE + i, names[i]);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g.stopAfterCurrent ? MF_CHECKED : 0), IDM_STOP_AFTER, L"&Stop after current");
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | align, pt.x, pt.y, 0, g.hwnd, nullptr);
    DestroyMenu(m);
    if (cmd) OnCommand(cmd, 0, nullptr);
}

void Dropdown(int i) {
    RECT r = ToolRect(i);
    POINT pt{r.left, r.bottom};
    ClientToScreen(ap.bar, &pt);
    ap.pressed = i;
    ap.pressedArrow = true;
    ap.menuOpen = true;
    InvalidateRect(ap.bar, nullptr, FALSE);
    UpdateWindow(ap.bar);
    switch (kTools[i].act) {
        case Act::Open:
            if (HMENU file = BarMenu(L"File")) {
                int cmd = TrackPopupMenu(file, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g.hwnd, nullptr);
                if (cmd) OnCommand(cmd, 0, nullptr);
            }
            break;
        case Act::Playlists: PlaylistsMenu(pt, 0); break;
        case Act::Options: OptionsMenu(pt, 0); break;
        case Act::Loop: LoopMenu(pt, 0); break;
        default: break;
    }
    ap.menuOpen = false;
    ap.pressed = -1;
    ap.pressedArrow = false;
    if (ap.bar) InvalidateRect(ap.bar, nullptr, FALSE);
}

void Run(int i) {
    switch (kTools[i].act) {
        case Act::Open: OnCommand(IDM_ADD_FILES, 0, nullptr); break;
        case Act::Playlists: Dropdown(i); break;
        case Act::Options: OnCommand(IDM_PREFERENCES, 0, nullptr); break;
        case Act::Prev: OnCommand(IDC_PREV, 0, nullptr); break;
        case Act::Stop: OnCommand(IDC_STOP, 0, nullptr); break;
        case Act::Pause:
            if (g.playing != kNone) TogglePause();
            break;
        case Act::Play:
            // Apollo's play starts what is stopped, resumes what is paused and restarts what plays.
            if (g.playing != kNone && !g.engine.Paused()) RequestSeek(0);
            else TogglePause();
            break;
        case Act::Next: OnCommand(IDC_NEXT, 0, nullptr); break;
        case Act::Loop: SetRepeat(g.repeat == Repeat::Off ? Repeat::All : Repeat::Off); break;
        case Act::Shuffle: SetShuffle(!g.shuffle); break;
        case Act::Analyzer:
        case Act::Scope:
            (kTools[i].act == Act::Analyzer ? ap.analyzer : ap.scope) ^= true;
            SaveApollo();
            g.engine.SetScope(ap.analyzer || ap.scope);
            Relayout();
            break;
        case Act::Vis: OnCommand(IDM_EQUALIZER, 0, nullptr); break;
        case Act::Mini: OnCommand(IDM_MINI_PLAYER, 0, nullptr); break;
        case Act::Tray: ShowWindow(g.hwnd, SW_HIDE); break;  // the notification-area icon brings it back
    }
    if (ap.bar) InvalidateRect(ap.bar, nullptr, FALSE);
}

void BufferedPaint(HWND h, void (*draw)(HWND, HDC, RECT)) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    if (rc.right > 0 && rc.bottom > 0) {
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        draw(h, mem, rc);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
    }
    EndPaint(h, &ps);
}

LRESULT CALLBACK BarProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: BufferedPaint(h, [](HWND, HDC dc, RECT rc) { PaintBar(dc, rc); }); return 0;
        case WM_MOUSEMOVE: {
            int hot = ToolAt({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            if (hot != ap.hot) {
                ap.hot = hot;
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
                TrackMouseEvent(&tme);
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            ap.hot = -1;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            bool arrow = false;
            int i = ToolAt({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}, &arrow);
            if (i < 0) return 0;
            if (arrow) {
                Dropdown(i);  // a dropdown opens on the press, as Windows' own do
                return 0;
            }
            ap.pressed = i;
            ap.pressedArrow = false;
            SetCapture(h);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONUP: {
            int i = ap.pressed;
            bool over = i >= 0 && ToolAt({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}) == i;
            ap.pressed = -1;
            if (GetCapture() == h) ReleaseCapture();
            InvalidateRect(h, nullptr, FALSE);
            if (over) Run(i);
            return 0;
        }
        case WM_CAPTURECHANGED:
            if (!ap.menuOpen && ap.pressed >= 0) {
                ap.pressed = -1;
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// The status window, the frequency analyzer and the oscilloscope

// The visualizations draw in Apollo's own pixels: one per device pixel at 96 DPI, whole multiples
// of it above, so they keep Apollo's hard-edged look at every scale.
int VisUnit() { return GlyphScale(); }

// The cells inside a box's two-pixel sunken edge: columns and rows of Apollo pixels.
SIZE VisCells(Box b) {
    RECT rc{};
    if (ap.boxes[b]) GetClientRect(ap.boxes[b], &rc);
    int u = VisUnit();
    return {std::max(0, (int)rc.right - 4) / u, std::max(0, (int)rc.bottom - 4) / u};
}

// The analyzer's next frame, as Apollo draws it: one spectrum of both channels across the whole box
// on a logarithmic frequency axis (40 Hz to 20 kHz), tilted up 3 dB an octave so music's falling
// highs still stand, in rows of a 72 dB scale; every column keeps a peak that holds, then falls.
// Silence (stopped, paused) lets everything fall.
bool VisStep() {
    constexpr int n = 2048;
    float s[n * 2] = {};
    bool live = g.playing != kNone && !g.engine.Paused() && g.engine.Scope(s, n) == n;
    ap.wave.assign(s, s + n * 2);
    SIZE cells = VisCells(kAnalyzerBox);
    int cols = cells.cx, rows = cells.cy;
    if ((int)ap.levels.size() != cols) {
        ap.levels.assign(cols, 0);
        ap.peaks.assign(cols, 0);
        ap.hold.assign(cols, 0);
    }
    bool moving = live;
    ap.spectrum.resize(cols);
    SpectrumLevels(s, g.engine.Rate(), ap.spectrum.data(), ap.spectrum.size());
    for (int x = 0; x < cols; ++x) {
        float level = live ? ap.spectrum[x] * rows : 0;
        ap.levels[x] = std::max(level, ap.levels[x] - rows / 10.0f);
        if (ap.levels[x] >= ap.peaks[x]) {
            ap.peaks[x] = ap.levels[x];
            ap.hold[x] = 8;  // about a quarter of a second
        } else if (ap.hold[x] > 0) {
            --ap.hold[x];
        } else {
            ap.peaks[x] = std::max(0.0f, ap.peaks[x] - rows / 40.0f);
        }
        moving = moving || ap.peaks[x] > 0;
    }
    return moving;
}

// Apollo's grays, 128 (the shadow) to 255 (the white ground), from the palette's own two.
COLORREF Gray(int v) { return Mix(Light(), Shadow(), std::clamp((v - 128) * 100 / 127, 0, 100)); }

void PaintStatusBox(HDC dc, RECT rc) {
    FillSolid(dc, rc, WindowBg());
    Sunken(dc, rc);
    RECT tr{rc.left + S(4), rc.top + 2, rc.right - S(4), rc.bottom - 2};
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, TextColor());
    HGDIOBJ old = SelectObject(dc, g.font);
    DrawTextW(dc, ap.status.c_str(), (int)ap.status.size(), &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, old);
}

// Bars shaded by height, a step lighter every row (128 at the foot, +6 a row), and each column's
// peak a single pixel of 180 at the foot to 190 at the top.
void PaintAnalyzer(HDC dc, RECT rc) {
    FillSolid(dc, rc, WindowBg());
    Sunken(dc, rc);
    int u = VisUnit(), rows = VisCells(kAnalyzerBox).cy, left = rc.left + 2, bottom = rc.top + 2 + rows * u;
    for (int i = 0; i < (int)ap.levels.size(); ++i) {
        int x = left + i * u, bar = std::min(rows, (int)ap.levels[i]);
        for (int r = 0; r < bar; ++r) FillSolid(dc, {x, bottom - (r + 1) * u, x + u, bottom - r * u}, Gray(128 + 6 * r));
        int peak = std::min(rows - 1, (int)ap.peaks[i]);
        if (ap.peaks[i] > 0.3f && peak >= bar)
            FillSolid(dc, {x, bottom - (peak + 1) * u, x + u, bottom - peak * u}, Gray(180 + 10 * peak / std::max(1, rows - 1)));
    }
}

// The waveform as dots, not a line: both channels together, one sample every fourth frame per
// column, each dot shared between the two cells it falls across (so it reads anti-aliased, 128 where
// a cell is fully covered).
void PaintScope(HDC dc, RECT rc) {
    FillSolid(dc, rc, WindowBg());
    Sunken(dc, rc);
    SIZE cells = VisCells(kScopeBox);
    int u = VisUnit(), left = rc.left + 2, top = rc.top + 2;
    int frames = (int)ap.wave.size() / 2, cols = std::min((int)cells.cx, frames / 4), rows = cells.cy;
    if (cols <= 0 || rows <= 1) return;
    std::vector<float> dark((size_t)cols * rows, 0);
    int first = frames - cols * 4;
    for (int x = 0; x < cols; ++x) {
        int at = (first + x * 4) * 2;
        float v = (ap.wave[at] + ap.wave[at + 1]) * 0.5f;
        float y = std::clamp((rows - 1) / 2.0f - v * rows, 0.0f, (float)(rows - 1));
        int y0 = (int)y;
        float f = y - y0;
        dark[(size_t)y0 * cols + x] += 1 - f;
        if (y0 + 1 < rows) dark[(size_t)(y0 + 1) * cols + x] += f;
    }
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c)
            if (float d = std::min(1.0f, dark[(size_t)r * cols + c]); d > 0.03f)
                FillSolid(dc, {left + c * u, top + r * u, left + (c + 1) * u, top + (r + 1) * u}, Gray(255 - (int)(127 * d)));
}

LRESULT CALLBACK BoxProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT:
            BufferedPaint(h, [](HWND w, HDC dc, RECT rc) {
                Box b = (Box)GetWindowLongPtrW(w, GWLP_USERDATA);
                if (b == kStatusBox) PaintStatusBox(dc, rc);
                else if (b == kAnalyzerBox) PaintAnalyzer(dc, rc);
                else PaintScope(dc, rc);
            });
            return 0;
        case WM_TIMER:
            if (wp == kVisTimer) {
                bool moving = VisStep();
                for (Box b : {kAnalyzerBox, kScopeBox})
                    if (ap.boxes[b] && IsWindowVisible(ap.boxes[b])) InvalidateRect(ap.boxes[b], nullptr, FALSE);
                // Asleep once still; ApolloTick wakes it when playback starts.
                if (!moving) KillTimer(h, kVisTimer);
            }
            return 0;
        case WM_LBUTTONDBLCLK:
            if (GetWindowLongPtrW(h, GWLP_USERDATA) == kStatusBox) OnCommand(IDM_PLAYING_PL, 0, nullptr);
            return 0;
        case WM_LBUTTONUP: {
            Box b = (Box)GetWindowLongPtrW(h, GWLP_USERDATA);
            if (b != kStatusBox) {  // clicking a visualization turns it off, as its button does
                (b == kAnalyzerBox ? ap.analyzer : ap.scope) = false;
                SaveApollo();
                g.engine.SetScope(ap.analyzer || ap.scope);
                Relayout();
            }
            return 0;
        }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void WakeVis() {
    HWND timer = ap.boxes[kStatusBox];
    if (timer && ap.on && (ap.analyzer || ap.scope)) SetTimer(timer, kVisTimer, 30, nullptr);
}

void Create() {
    if (ap.bar) return;
    ap.analyzer = IniInt(L"Apollo", L"Analyzer", 1) != 0;
    ap.scope = IniInt(L"Apollo", L"Oscilloscope", 1) != 0;
    ap.skin = std::clamp(IniInt(L"Apollo", L"Rebar", 0), 0, kSkins);
    ap.opacity = std::clamp(IniInt(L"Apollo", L"RebarOpacity", 40), 0, 100);
    RegisterAppClass(kBarClass, BarProc, 0, false);
    RegisterAppClass(kBoxClass, BoxProc, CS_DBLCLKS, false);
    ap.bar = CreateWindowExW(0, kBarClass, L"Toolbar", WS_CHILD, 0, 0, 0, 0, g.hwnd, nullptr, g.inst, nullptr);
    const wchar_t* names[] = {L"Status", L"Frequency analyzer", L"Oscilloscope"};
    for (int b = 0; b < kBoxes; ++b) {
        ap.boxes[b] = CreateWindowExW(0, kBoxClass, names[b], WS_CHILD, 0, 0, 0, 0, g.hwnd, nullptr, g.inst, nullptr);
        SetWindowLongPtrW(ap.boxes[b], GWLP_USERDATA, b);
    }
    for (int i = 0; i < kToolCount; ++i) {
        if (kTools[i].glyph < 0) continue;
        TTTOOLINFOW ti{sizeof(ti)};
        ti.uFlags = TTF_SUBCLASS;
        ti.hwnd = ap.bar;
        ti.uId = (UINT_PTR)(i + 1);
        ti.lpszText = const_cast<wchar_t*>(kTools[i].tip);
        SendMessageW(g.tips, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    }
}

void ListTheme(bool on) {
    // Rows tall enough for the track icon: the list sizes its rows to its small image list's height.
    static HIMAGELIST rows = nullptr;
    static int rowsDpi = 0;
    if (on && (!rows || rowsDpi != (int)g.dpi)) {
        if (rows) ImageList_Destroy(rows);
        rows = ImageList_Create(1, S(17), ILC_COLOR32, 1, 0);
        rowsDpi = (int)g.dpi;
    }
    ListView_SetImageList(g.list, on ? rows : nullptr, LVSIL_SMALL);
    if (on) {
        // Unthemed: the classic scroll bars of a Windows 2000 list view.
        SetWindowTheme(g.list, L"", L"");
        SetWindowTheme(g.header, L"", L"");
    } else {
        ApplyControlTheme(g.list, g.dark);
        ApplyControlTheme(g.header, g.dark);
    }
}

}  // namespace

void ApolloSync() {
    bool on = ApolloMode() && g.hwnd && g.list;
    if (on) Create();
    if (ap.bar) ShowWindow(ap.bar, on ? SW_SHOWNA : SW_HIDE);
    for (int b = 0; b < kBoxes; ++b)
        if (ap.boxes[b])
            ShowWindow(ap.boxes[b], on && (b == kStatusBox || (b == kAnalyzerBox ? ap.analyzer : ap.scope)) ? SW_SHOWNA : SW_HIDE);
    if (on == ap.on) return;
    ap.on = on;
    // Leaving Apollo must not disable the sample tap that foobar2000 has just enabled.
    g.engine.SetScope(on && (ap.analyzer || ap.scope));
    if (on) WakeVis();
    else if (ap.boxes[kStatusBox]) KillTimer(ap.boxes[kStatusBox], kVisTimer);
    ListTheme(on);
    ap.titleFitFor = -1;
    // Apollo keeps its own columns ([ApolloColumns]) and never groups albums.
    LoadColumns();
    SetupColumns();
    RefreshViewKeepingSelection();
    SyncMenuBar();  // Apollo has no menu bar; its menus hang off the toolbar
    UpdateNowPlaying();
    g.statsDirty = true;
}

void LayoutApollo(HDWP& dwp, RECT r) {
    auto place = [&](HWND h, int x, int y, int w, int hgt) { Place(dwp, h, x, y, w, hgt); };
    auto show = [](HWND h, bool on) { ShowWindow(h, on ? SW_SHOWNA : SW_HIDE); };
    for (HWND h : {g.npTitle, g.npSub, g.elapsed, g.time, g.speaker, g.pointsChip, g.search, g.tabs,
                   GetDlgItem(g.hwnd, IDC_LYRICS), UpNextPanel(), g.libView})
        show(h, false);
    for (HWND b : g.buttons) show(b, false);
    for (HWND b : g.fbButtons) show(b, false);
    for (HWND b : g.viewButtons) show(b, false);
    for (auto& pane : g.panes) show(pane.list, false);
    for (HWND h : {g.seek, g.volume, g.list}) show(h, true);
    g.gripRect = g.splitRect = g.artRect = {};
    int W = r.right - r.left;
    // Two rows of bands between etched borders: 2 + 26 + 2 + 26 + 2.
    int rowH = S(26);
    ap.rebar = {r.left, r.top, r.right, r.top + 2 + rowH + 2 + rowH + 2};
    ap.rows[0] = {r.left, r.top + 2, r.right, r.top + 2 + rowH};
    ap.rows[1] = {r.left, ap.rows[0].bottom + 2, r.right, ap.rows[0].bottom + 2 + rowH};
    ap.bands.clear();
    // Row 1: the toolbar, then Position, then Volume.
    int pad = S(10), tbW = std::min(W / 2, pad + ToolbarWidth() + S(6)), volW = std::clamp(W / 5, S(110), S(190));
    RECT tb{r.left, ap.rows[0].top, r.left + tbW, ap.rows[0].bottom};
    RECT pos{tb.right, tb.top, std::max<int>(tb.right, r.right - volW), tb.bottom};
    RECT vol{pos.right, tb.top, r.right, tb.bottom};
    ap.bands.insert(ap.bands.end(), {tb, pos, vol});
    place(ap.bar, tb.left + pad, tb.top + (rowH - S(22)) / 2, std::max(0, (int)tb.right - (int)tb.left - pad), S(22));
    HDC dc = GetDC(g.hwnd);
    HGDIOBJ old = SelectObject(dc, g.font);
    const wchar_t* labels[] = {L"Position", L"Volume"};
    HWND bars[] = {g.seek, g.volume};
    for (int i = 0; i < 2; ++i) {
        const RECT& band = i ? vol : pos;
        SIZE ext{};
        GetTextExtentPoint32W(dc, labels[i], (int)wcslen(labels[i]), &ext);
        ap.labels[i] = {band.left + pad, band.top, band.left + pad + ext.cx, band.bottom};
        int x = ap.labels[i].right + S(4);
        place(bars[i], x, band.top + (rowH - S(22)) / 2, std::max(0, (int)band.right - S(4) - x), S(22));
    }
    SelectObject(dc, old);
    ReleaseDC(g.hwnd, dc);
    // Row 2: the status window as wide as the toolbar band, the visualizations sharing the rest.
    int visCount = (ap.analyzer ? 1 : 0) + (ap.scope ? 1 : 0);
    int statusW = visCount ? tbW : W;
    RECT status{r.left, ap.rows[1].top, r.left + statusW, ap.rows[1].bottom};
    ap.bands.push_back(status);
    place(ap.boxes[kStatusBox], status.left + pad, status.top + S(3), statusW - pad - S(4), rowH - S(6));
    int x = status.right, each = visCount ? (r.right - x) / visCount : 0;
    for (Box b : {kAnalyzerBox, kScopeBox}) {
        bool shown = b == kAnalyzerBox ? ap.analyzer : ap.scope;
        show(ap.boxes[b], shown);
        if (!shown) continue;
        int right = --visCount ? x + each : r.right;
        RECT band{x, ap.rows[1].top, right, ap.rows[1].bottom};
        ap.bands.push_back(band);
        place(ap.boxes[b], band.left + pad, band.top + S(3), right - band.left - pad - S(4), rowH - S(6));
        x = right;
    }
    // The playlist, in a sunken frame under the rebar.
    ap.listFrame = {r.left + S(2), ap.rebar.bottom + S(2), r.right - S(2), r.bottom - S(2)};
    place(g.list, ap.listFrame.left + 2, ap.listFrame.top + 2, ap.listFrame.right - ap.listFrame.left - 4,
          ap.listFrame.bottom - ap.listFrame.top - 4);
    PlaceTips();
    ap.levels.clear();  // the analyzer's width may have changed
    ap.titleFitFor = -1;
}

void PaintApolloChrome(HDC dc) {
    RECT client;
    GetClientRect(g.hwnd, &client);
    FillRebar(dc, ap.rebar, g.hwnd);
    EtchedH(dc, ap.rebar.left, ap.rebar.right, ap.rebar.top);
    EtchedH(dc, ap.rebar.left, ap.rebar.right, ap.rows[0].bottom);
    EtchedH(dc, ap.rebar.left, ap.rebar.right, ap.rows[1].bottom);
    for (const RECT& band : ap.bands) {
        if (band.left > ap.rebar.left) EtchedV(dc, band.left - 1, band.top, band.bottom);
        // The band's gripper: a raised bar, three pixels wide.
        int gx = band.left + S(3);
        Frame(dc, {gx, band.top + S(3), gx + 3, band.bottom - S(3)}, Light(), Shadow());
    }
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, TextColor());
    HGDIOBJ old = SelectObject(dc, g.font);
    const wchar_t* labels[] = {L"Position", L"Volume"};
    for (int i = 0; i < 2; ++i) {
        RECT r = ap.labels[i];
        DrawTextW(dc, labels[i], -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    SelectObject(dc, old);
    FillSolid(dc, {client.left, ap.rebar.bottom, client.right, ap.listFrame.top}, Face());
    Sunken(dc, ap.listFrame);
}

void ApolloTheme() {
    bool themed = g.themeMode == ThemeMode::Apollo && g.hwnd;
    if (themed) {
        // A Windows 2000 window: the butterfly, a navy caption with white text and square corners.
        COLORREF caption = Ref(P().accent), text = Ref(TextOn(P().accent));
        DwmSetWindowAttribute(g.hwnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
        DwmSetWindowAttribute(g.hwnd, 36 /* DWMWA_TEXT_COLOR */, &text, sizeof(text));
        DwmSetWindowAttribute(g.hwnd, 34 /* DWMWA_BORDER_COLOR */, &caption, sizeof(caption));
        int corners = 1;  // DWMWCP_DONOTROUND
        DwmSetWindowAttribute(g.hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corners, sizeof(corners));
        if (!ap.bigIcon) {
            ap.bigIcon = (HICON)LoadImageW(g.inst, MAKEINTRESOURCEW(IDI_APOLLO), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                           GetSystemMetrics(SM_CYICON), 0);
            ap.smallIcon = (HICON)LoadImageW(g.inst, MAKEINTRESOURCEW(IDI_APOLLO), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                             GetSystemMetrics(SM_CYSMICON), 0);
        }
        SendMessageW(g.hwnd, WM_SETICON, ICON_BIG, (LPARAM)ap.bigIcon);
        SendMessageW(g.hwnd, WM_SETICON, ICON_SMALL, (LPARAM)ap.smallIcon);
    } else if (ap.themed && g.hwnd) {
        COLORREF none = 0xFFFFFFFF;  // DWMWA_COLOR_DEFAULT
        DwmSetWindowAttribute(g.hwnd, 34, &none, sizeof(none));
        int corners = 0;  // DWMWCP_DEFAULT
        DwmSetWindowAttribute(g.hwnd, 33, &corners, sizeof(corners));
        // Back to the class icon.
        SendMessageW(g.hwnd, WM_SETICON, ICON_BIG, (LPARAM)GetClassLongPtrW(g.hwnd, GCLP_HICON));
        SendMessageW(g.hwnd, WM_SETICON, ICON_SMALL, (LPARAM)GetClassLongPtrW(g.hwnd, GCLP_HICONSM));
    }
    ap.themed = themed;
    if (ap.on) ListTheme(true);
    ap.skinFor = -1;  // the face may have changed
}

std::wstring ApolloWindowTitle() {
    if (g.playing == kNone) return L"";
    auto lookup = Lookup(g.playing, true);
    int index = TokenIndex(g.playingToken, g.playingPl);
    std::wstring title = (index >= 0 ? std::to_wstring(index + 1) + L". " : L"") + FormatTitle(L"[%artist% - ]%title%", lookup);
    return title + FormatTitle(L"[ (%length%)]", lookup);
}

void ApolloTick() {
    if (!ap.on) return;
    std::wstring status;
    if (g.playing != kNone) {
        auto lookup = Lookup(g.playing, true);
        int index = TokenIndex(g.playingToken, g.playingPl);
        status = (index >= 0 ? std::to_wstring(index + 1) + L". " : L"") + FormatTitle(L"[%artist% - ]%title%", lookup) + L" " +
                 FormatTitle(L"%playback_time%[ (%length%)]", lookup);
    }
    if (status != ap.status) {
        ap.status = status;
        InvalidateRect(ap.boxes[kStatusBox], nullptr, FALSE);
    }
    if (g.playing != kNone && !g.engine.Paused()) WakeVis();
    if (ap.bar) InvalidateRect(ap.bar, nullptr, FALSE);  // pause, loop and shuffle show their state
    // "Automatically resize playlist columns to fit the window": the Title column takes what the
    // others leave, here whenever the list's width changes or a column is resized.
    int title = SubitemOf(kColApolloTitle);
    RECT rc;
    if (title < 0 || !GetClientRect(g.list, &rc)) return;
    int others = 0;
    for (int i = 0; i < (int)g.columns.size(); ++i)
        if (i != title) others += ListView_GetColumnWidth(g.list, i);
    int want = std::max(S(80), (int)rc.right - others);
    if (want != ListView_GetColumnWidth(g.list, title)) ListView_SetColumnWidth(g.list, title, want);
}

void ApolloStatus() {
    bool message = GetTickCount64() < g.messageUntil;
    if (!g.statsDirty && message == ap.showingMessage) return;
    g.statsDirty = false;
    ap.showingMessage = message;
    double total = 0;
    for (uint32_t i : g.view) total += g_lib[Active().items[i]].Duration();
    SetStatusPart(0, std::to_wstring(g.view.size()) + (g.view.size() == 1 ? L" track (" : L" tracks (") + FormatDuration(total) + L")");
    if (message) {
        SetStatusPart(1, g.message);
        return;
    }
    auto selected = SelectedTracks();
    double chosen = 0;
    for (TrackId id : selected) chosen += g_lib[id].Duration();
    SetStatusPart(1, selected.empty()        ? std::wstring(L"No items selected")
                     : selected.size() == 1 ? L"1 item selected (" + FormatDuration(chosen) + L")"
                                            : std::to_wstring(selected.size()) + L" items selected (" + FormatDuration(chosen) + L")");
}

bool ApolloContextMenu(HWND from, POINT pt) {
    bool rebar = from == g.hwnd || from == ap.bar || from == g.volume || from == g.status;
    for (HWND b : ap.boxes) rebar = rebar || from == b;
    if (!ApolloMode() || !rebar) return false;
    if (pt.x == -1 && pt.y == -1) GetCursorPos(&pt);
    OptionsMenu(pt, 0);
    return true;
}

LRESULT ApolloDrawTrackbar(NMCUSTOMDRAW* cd) {
    HDC dc = cd->hdc;
    HWND bar = cd->hdr.hwndFrom;
    if (cd->dwItemSpec == TBCD_CHANNEL) {
        RECT all;
        GetClientRect(bar, &all);
        FillRebar(dc, all, bar);
        // A classic trackbar's channel: four pixels, sunken, the face inside.
        RECT ch = cd->rc;
        int cy = (all.top + all.bottom) / 2;
        ch.top = cy - 2;
        ch.bottom = cy + 2;
        FillSolid(dc, ch, Face());
        Frame(dc, ch, Shadow(), Light());
        RECT in = ch;
        InflateRect(&in, -1, -1);
        FillSolid(dc, {in.left, in.top, in.right, in.top + 1}, DarkShadow());
        return CDRF_SKIPDEFAULT;
    }
    if (cd->dwItemSpec == TBCD_THUMB) {
        // The raised thumb, a little narrower than the stock one.
        RECT r = cd->rc;
        int w = std::max(S(10), 6), cx = (r.left + r.right) / 2;
        r = {cx - w / 2, r.top + S(1), cx - w / 2 + w, r.bottom - S(1)};
        bool on = IsWindowEnabled(bar) != 0;
        FillSolid(dc, r, Face());
        Frame(dc, r, Light(), DarkShadow());
        RECT in = r;
        InflateRect(&in, -1, -1);
        Frame(dc, in, Face(), Shadow());
        if (!on) FillSolid(dc, {cx - 1, r.top + 3, cx + 1, r.bottom - 3}, Shadow());
        return CDRF_SKIPDEFAULT;
    }
    return cd->dwItemSpec == TBCD_TICS ? CDRF_SKIPDEFAULT : CDRF_DODEFAULT;
}

void ApolloPaintHeader(HWND h, HDC dc) {
    RECT rc;
    GetClientRect(h, &rc);
    FillSolid(dc, rc, Face());
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ old = SelectObject(dc, g.font);
    int n = Header_GetItemCount(h), right = rc.left;
    for (int i = 0; i < n; ++i) {
        RECT r;
        if (!Header_GetItemRect(h, i, &r)) continue;
        right = std::max(right, (int)r.right);
        // A raised push button per column.
        Frame(dc, r, Light(), DarkShadow());
        RECT in = r;
        InflateRect(&in, -1, -1);
        Frame(dc, in, Face(), Shadow());
        HDITEMW item{};
        wchar_t text[128] = {};
        item.mask = HDI_TEXT | HDI_FORMAT;
        item.pszText = text;
        item.cchTextMax = (int)std::size(text);
        Header_GetItem(h, i, &item);
        UINT fmt = item.fmt & (HDF_LEFT | HDF_CENTER | HDF_RIGHT);
        if (ColumnType(i) == 0) fmt = HDF_RIGHT;  // "No" sits over the right-aligned numbers
        RECT tr{r.left + S(5), r.top, r.right - S(5), r.bottom};
        bool sorted = (item.fmt & (HDF_SORTUP | HDF_SORTDOWN)) != 0;
        if (sorted) {
            int cx = r.right - S(10), cy = (r.top + r.bottom) / 2;
            bool up = (item.fmt & HDF_SORTUP) != 0;
            for (int row = 0; row < 4; ++row) {
                int y = up ? cy - S(2) + row : cy + S(2) - row;
                FillSolid(dc, {cx - row, y, cx + row + 1, y + 1}, Shadow());
            }
            tr.right -= S(12);
        }
        SetTextColor(dc, TextColor());
        DrawTextW(dc, text, -1, &tr,
                  DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX |
                      (fmt == HDF_CENTER ? DT_CENTER : fmt == HDF_RIGHT ? DT_RIGHT : DT_LEFT));
    }
    if (right < rc.right) {  // the empty header past the last column is a button too
        RECT r{right, rc.top, rc.right, rc.bottom};
        Frame(dc, r, Light(), DarkShadow());
        InflateRect(&r, -1, -1);
        Frame(dc, r, Face(), Shadow());
    }
    SelectObject(dc, old);
}

void ApolloDrawStatusPart(const DRAWITEMSTRUCT* di) {
    int part = (int)di->itemData;
    RECT r = di->rcItem;
    FillSolid(di->hDC, r, Face());
    r.right -= S(2);
    Frame(di->hDC, r, Shadow(), Light());
    SetBkMode(di->hDC, TRANSPARENT);
    SetTextColor(di->hDC, TextColor());
    HGDIOBJ old = SelectObject(di->hDC, g.font);
    RECT tr{r.left + S(3), r.top + 1, r.right - S(3), r.bottom - 1};
    const std::wstring& text = part >= 0 && part < 3 ? g.statusParts[part] : std::wstring();
    DrawTextW(di->hDC, text.c_str(), (int)text.size(), &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(di->hDC, old);
}

void ApolloDrawTrackIcon(HDC dc, const RECT& cell) {
    LoadIcons();
    if (!ap.trackIcon) return;
    int size = S(16), y = (cell.top + cell.bottom - size) / 2;
    DrawIconEx(dc, cell.left + S(2), y, ap.trackIcon, size, size, 0, nullptr, DI_NORMAL);
}

// ---------------------------------------------------------------------------
// Apollo's playlist fields

namespace {

struct Probe {
    uint64_t fileTime = 0, size = 0;
    uint8_t bits = 0;            // 0: not known
    uint8_t id3 = 0, id3rev = 0;  // ID3v2 major version and revision; 0: none
    uint8_t id3v1 = 0;           // 10 ID3v1.0, 11 ID3v1.1; 0: none
};

bool ReadAt(HANDLE f, int64_t at, void* buf, DWORD size, DWORD& got) {
    LARGE_INTEGER li;
    li.QuadPart = at;
    return SetFilePointerEx(f, li, nullptr, at < 0 ? FILE_END : FILE_BEGIN) && ReadFile(f, buf, size, &got, nullptr);
}

// Reads the file's head and tail once: the ID3v2 header, an ID3v1 tag, and the bit depth of FLAC
// and WAV (lossy formats decode to 16 bits, as Apollo's MPEG plug-in did).
const Probe& ProbeFile(TrackId id) {
    static std::unordered_map<TrackId, Probe> cache;
    const Track& t = g_lib[id];
    auto it = cache.find(id);
    if (it != cache.end() && it->second.fileTime == t.fileTime) return it->second;
    if (cache.size() > 20000) cache.clear();
    Probe& p = cache[id];
    p = {};
    p.fileTime = t.fileTime;
    switch (t.codec) {
        case Codec::MP3: case Codec::MP2: case Codec::AAC: case Codec::WMA: case Codec::Vorbis: case Codec::Opus: case Codec::AC3:
            p.bits = 16;
            break;
        default: break;
    }
    std::wstring path = g_lib.Path(id);
    if (size_t bar = path.find(L'|'); bar != std::wstring::npos) path.resize(bar);  // a CUE or game-music sub-track
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return p;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(f, &size)) p.size = (uint64_t)size.QuadPart;
    uint8_t head[64] = {};
    DWORD got = 0;
    int64_t audio = 0;
    if (ReadAt(f, 0, head, sizeof(head), got) && got >= 10 && !memcmp(head, "ID3", 3) && head[3] >= 2 && head[3] <= 4) {
        p.id3 = head[3];
        p.id3rev = head[4];
        audio = 10 + ((head[6] & 0x7F) << 21 | (head[7] & 0x7F) << 14 | (head[8] & 0x7F) << 7 | (head[9] & 0x7F));
        if (!ReadAt(f, audio, head, sizeof(head), got)) got = 0;
    }
    if (t.codec == Codec::FLAC && got >= 22 && !memcmp(head, "fLaC", 4)) {
        p.bits = (uint8_t)((((head[20] & 1) << 4) | (head[21] >> 4)) + 1);  // STREAMINFO's bits per sample - 1
    } else if (t.codec == Codec::PCM && got >= 36 && !memcmp(head, "RIFF", 4) && !memcmp(head + 8, "WAVE", 4) &&
               !memcmp(head + 12, "fmt ", 4)) {
        p.bits = head[34];
    }
    uint8_t tail[128] = {};
    if (t.fileSize >= 128 && ReadAt(f, -128, tail, sizeof(tail), got) && got == 128 && !memcmp(tail, "TAG", 3))
        p.id3v1 = tail[125] == 0 && tail[126] != 0 ? 11 : 10;
    CloseHandle(f);
    return p;
}

}  // namespace

bool ApolloField(TrackId id, const std::wstring& name, std::wstring& out) {
    if (!g_lib.Contains(id)) return false;
    const Track& t = g_lib[id];
    wchar_t buf[64];
    if (name == L"apollo_size") {
        // Some tracks reach the library without a size (added from a playlist, not a scan).
        unsigned long long size = t.fileSize ? t.fileSize : ProbeFile(id).size;
        if (!size) return false;
        if (size < 10ull << 20) swprintf_s(buf, L"%llu KB", (size + 512) >> 10);
        else swprintf_s(buf, L"%llu MB", (size + (1 << 19)) >> 20);
        out = buf;
        return true;
    }
    if (name == L"apollo_bits") {
        const Probe& p = ProbeFile(id);
        std::wstring channels = t.channels == 1 ? L"mono" : t.channels == 2 ? L"stereo" : t.channels ? std::to_wstring(t.channels) + L" channels" : L"";
        out = p.bits ? std::to_wstring(p.bits) + L" bit" + (channels.empty() ? L"" : L" " + channels) : channels;
        return !out.empty();
    }
    if (name == L"apollo_tag") {
        const Probe& p = ProbeFile(id);
        if (p.id3) swprintf_s(buf, L"v2.%u.%u", p.id3, p.id3rev);
        else if (p.id3v1) swprintf_s(buf, L"v1.%u", p.id3v1 - 10);
        else return false;
        out = buf;
        return true;
    }
    return false;
}

}  // namespace app
