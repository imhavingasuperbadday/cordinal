// Mini player: a round window that stays on top. The cover fills the disc; a ring around it shows
// how far the track has played (click or drag it to seek); previous / play / next sit across the
// middle. The main window hides while it shows; Esc, Ctrl+= or a double-click brings it back.
// Drag the window by the cover; the wheel sets the volume.
//
// It is drawn by hand into the premultiplied bitmap of a layered window, so the edge of the circle
// is smooth and the corners are see-through: every shape is a circle or a ring, whose antialiased
// coverage is a distance, and the glyphs are GDI's, drawn white on black into a mask whose grey is
// their coverage. Direct2D drew it once, but even its software target starts Direct3D's WARP
// rasterizer, a dozen threads and about 3 MB for a 196-pixel disc (docs/memory.md, "Mini player").
#include "app.h"

namespace app {

namespace {

const wchar_t kMiniClass[] = L"CordinalMiniPlayer";

enum Part { kPrev, kPlay, kNext, kRing, kPartCount };

struct Mini {
    HWND hwnd = nullptr;
    int hot = -1;
    bool seeking = false;
    double seekFrac = 0;
    // The 32-bit premultiplied picture UpdateLayeredWindow shows, and a strip as wide, as tall as
    // the largest label, the glyphs are drawn into first. Lightweight mode lets go of both between
    // frames: the window keeps its own copy of the picture, and it is redrawn once a second.
    HDC mem = nullptr, maskDc = nullptr;
    HBITMAP bmp = nullptr, maskBmp = nullptr;
    HGDIOBJ oldBmp = nullptr, oldMask = nullptr;
    uint32_t* bits = nullptr;
    const uint32_t* mask = nullptr;
    int bmpSize = 0, maskHeight = 0;
    HFONT glyph = nullptr, glyphBig = nullptr, timeFont = nullptr;
    int fontSize = 0;  // the Diameter() the fonts were made for
} m;

// Geometry, in pixels from the window's top left.
int Diameter() { return S(196); }
float Radius() { return Diameter() / 2.0f; }
float RingRadius() { return Radius() - S(9); }   // the middle of the progress ring
float DiscRadius() { return Radius() - S(17); }  // the cover inside it
struct Point { float x, y; };
Point PartCenter(int part) {
    float c = Radius(), off = (float)S(54);
    return {part == kPrev ? c - off : part == kNext ? c + off : c, c};
}
float PartRadius(int part) { return (float)(part == kPlay ? S(27) : S(20)); }

int PartAt(POINT pt) {
    float dx = pt.x + 0.5f - Radius(), dy = pt.y + 0.5f - Radius(), d = std::sqrt(dx * dx + dy * dy);
    if (d > Radius()) return -1;
    if (d >= DiscRadius() - S(2)) return kRing;
    for (int i = kPrev; i <= kNext; ++i) {
        Point c = PartCenter(i);
        float px = pt.x + 0.5f - c.x, py = pt.y + 0.5f - c.y;
        if (px * px + py * py <= PartRadius(i) * PartRadius(i)) return i;
    }
    return -1;
}

constexpr double kTau = 2 * 3.14159265358979;

// The ring's fraction under a point: 0 at twelve o'clock, growing clockwise. Dragging across the
// top doesn't jump from the end to the start: it stops there.
double RingFracAt(POINT pt, double previous) {
    double a = std::atan2(pt.x + 0.5 - Radius(), -(pt.y + 0.5 - Radius()));
    double frac = (a < 0 ? a + kTau : a) / kTau;
    if (previous > 0.75 && frac < 0.25) return 1.0;
    if (previous < 0.25 && frac > 0.75) return 0.0;
    return frac;
}

// Antialiased coverage of a pixel whose centre is `inside` pixels within a shape's edge.
float Cover(float inside) { return std::clamp(inside + 0.5f, 0.0f, 1.0f); }

// Source-over of an opaque colour at `alpha` onto a premultiplied pixel.
void Blend(uint32_t& px, Rgb c, float alpha) {
    if (alpha <= 0) return;
    const uint32_t a = (uint32_t)(alpha * 255 + 0.5f), keep = 255 - a;
    auto mix = [&](int shift) {
        return ((((c >> shift) & 0xFF) * a + ((px >> shift) & 0xFF) * keep + 127) / 255) << shift;
    };
    px = mix(0) | mix(8) | mix(16) | ((a * 255 + ((px >> 24) & 0xFF) * keep + 127) / 255) << 24;
}

// A circle filled over the whole picture, as cheap as the few dozen pixels its box holds.
void FillCircle(Point c, float r, Rgb color, float alpha = 1) {
    const int size = m.bmpSize;
    const int x0 = std::max(0, (int)(c.x - r - 1)), x1 = std::min(size, (int)(c.x + r + 2));
    const int y0 = std::max(0, (int)(c.y - r - 1)), y1 = std::min(size, (int)(c.y + r + 2));
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const float dx = x + 0.5f - c.x, dy = y + 0.5f - c.y;
            Blend(m.bits[y * size + x], color, alpha * Cover(r - std::sqrt(dx * dx + dy * dy)));
        }
}

HFONT MakeFont(const wchar_t* face, int size, int weight) {
    return CreateFontW(-size, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, face);
}

// Glyphs in `box`, centred, in `color`: drawn white on black into the mask, whose grey level is the
// glyph's coverage, then blended into the picture. GDI can't draw text onto a layered window's
// see-through pixels itself: it leaves their alpha at zero.
void Label(const wchar_t* text, HFONT font, RECT box, Rgb color, float alpha = 1) {
    RECT bounds{0, 0, m.bmpSize, m.bmpSize};
    if (!IntersectRect(&box, &box, &bounds)) return;
    RECT strip{box.left, 0, box.right, std::min<LONG>(box.bottom - box.top, m.maskHeight)};
    PatBlt(m.maskDc, strip.left, 0, strip.right - strip.left, strip.bottom, BLACKNESS);
    HGDIOBJ old = SelectObject(m.maskDc, font);
    DrawTextW(m.maskDc, text, -1, &strip, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(m.maskDc, old);
    GdiFlush();
    for (int y = 0; y < strip.bottom; ++y)
        for (int x = strip.left; x < strip.right; ++x)
            if (const uint32_t grey = (m.mask[y * m.bmpSize + x] >> 8) & 0xFF)
                Blend(m.bits[(y + box.top) * m.bmpSize + x], color, alpha * grey / 255.0f);
}

RECT Box(float l, float t, float r, float b) {
    return {(LONG)std::floor(l), (LONG)std::floor(t), (LONG)std::ceil(r), (LONG)std::ceil(b)};
}

void FreeFonts() {
    for (HFONT* f : {&m.glyph, &m.glyphBig, &m.timeFont}) {
        if (*f) DeleteObject(*f);
        *f = nullptr;
    }
}

void FreeBitmaps() {
    if (m.mem) {
        SelectObject(m.mem, m.oldBmp);
        DeleteObject(m.bmp);
        DeleteDC(m.mem);
    }
    if (m.maskDc) {
        SelectObject(m.maskDc, m.oldMask);
        DeleteObject(m.maskBmp);
        DeleteDC(m.maskDc);
    }
    m.mem = m.maskDc = nullptr;
    m.bmp = m.maskBmp = nullptr;
    m.bits = nullptr;
    m.mask = nullptr;
    m.bmpSize = 0;
}

// A 32-bit top-down DIB selected into a new memory DC.
bool MakeDib(int width, int height, HDC& dc, HBITMAP& bmp, HGDIOBJ& old, void*& bits) {
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(bi.bmiHeader), width, -height, 1, 32};
    dc = CreateCompatibleDC(nullptr);
    bmp = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (!bmp) return false;
    old = SelectObject(dc, bmp);
    return true;
}

// Makes the picture and the mask when they are missing or the size changed (a new DPI), and the
// fonts when the size changed.
bool EnsureTarget(int size) {
    if (m.fontSize != size) {
        FreeFonts();
        m.glyph = MakeFont(IconFace(), S(16), FW_NORMAL);
        m.glyphBig = MakeFont(IconFace(), S(20), FW_NORMAL);
        m.timeFont = MakeFont(L"Segoe UI", S(11), FW_SEMIBOLD);
        m.fontSize = size;
    }
    if (m.bmpSize == size) return true;
    FreeBitmaps();
    const int strip = (int)PartRadius(kPlay) * 2;  // the play button's box, the tallest label
    void *bits = nullptr, *mask = nullptr;
    if (!MakeDib(size, size, m.mem, m.bmp, m.oldBmp, bits) ||
        !MakeDib(size, strip, m.maskDc, m.maskBmp, m.oldMask, mask)) {
        FreeBitmaps();  // bmpSize stays unset, so a later paint retries
        return false;
    }
    m.bits = (uint32_t*)bits;
    m.mask = (const uint32_t*)mask;
    m.bmpSize = size;
    m.maskHeight = strip;
    SetBkMode(m.maskDc, TRANSPARENT);
    SetTextColor(m.maskDc, RGB(255, 255, 255));
    return true;
}

// The cover under a disc pixel: g.art scaled to fill the disc's square (the short side fits, the
// long one is cropped evenly), bilinear, darkened by 42% black so the buttons read on any picture.
// Sampled as the disc is drawn, so no scaled copy is kept.
struct CoverSampler {
    const ArtImage& art;
    float scale, ox, oy;
    CoverSampler(const ArtImage& image, float discLeft, float side)
        : art(image), scale(image.Empty() ? 1 : side / std::min(image.width, image.height)) {
        ox = (art.width - side / scale) / 2 - discLeft / scale;
        oy = (art.height - side / scale) / 2 - discLeft / scale;
    }
    uint32_t At(int x, int y) const {
        return art.pixels[(size_t)std::clamp(y, 0, art.height - 1) * art.width + std::clamp(x, 0, art.width - 1)];
    }
    Rgb operator()(int px, int py) const {
        const float sx = ox + (px + 0.5f) / scale - 0.5f, sy = oy + (py + 0.5f) / scale - 0.5f;
        const int x = (int)std::floor(sx), y = (int)std::floor(sy);
        const float fx = sx - x, fy = sy - y;
        const uint32_t p00 = At(x, y), p10 = At(x + 1, y), p01 = At(x, y + 1), p11 = At(x + 1, y + 1);
        uint32_t out = 0;
        for (int shift = 0; shift < 24; shift += 8) {
            auto ch = [&](uint32_t p) { return (float)((p >> shift) & 0xFF); };
            const float top = ch(p00) + (ch(p10) - ch(p00)) * fx, bottom = ch(p01) + (ch(p11) - ch(p01)) * fx;
            out |= (uint32_t)((top + (bottom - top) * fy) * 0.58f + 0.5f) << shift;
        }
        return out;
    }
};

void Draw() {
    const Palette& p = P();
    const int size = m.bmpSize;
    const float R = Radius(), rr = RingRadius(), dr = DiscRadius(), ringW = (float)S(5);
    const Point c{R, R};

    const bool art = !g.art.Empty();
    const CoverSampler cover(g.art, R - dr, 2 * dr);

    double dur = CurrentDuration(), pos = m.seeking ? m.seekFrac * dur : g.engine.Position();
    double frac = dur > 0 && g.playing != kNone ? std::clamp(pos / dur, 0.0, 1.0) : 0;
    auto at = [&](double f) {
        const double a = f * kTau;
        return Point{c.x + rr * (float)std::sin(a), c.y - rr * (float)std::cos(a)};
    };
    const Point arcStart = at(0), arcEnd = at(frac);
    const float arcAngle = (float)(frac * kTau);

    // The puck, its edge, the ring's track, the cover, and the progress arc from twelve o'clock with
    // round ends: one pass over the picture.
    for (int y = 0; y < size; ++y) {
        uint32_t* row = m.bits + (size_t)y * size;
        for (int x = 0; x < size; ++x) {
            const float dx = x + 0.5f - c.x, dy = y + 0.5f - c.y, d = std::sqrt(dx * dx + dy * dy);
            uint32_t px = 0;
            if (d < R + 1) {
                Blend(px, p.surface, Cover(R - 0.5f - d));
                Blend(px, p.border, Cover(0.5f - std::fabs(d - (R - 1))));
                Blend(px, p.border, Cover(ringW / 2 - std::fabs(d - rr)));
                if (const float disc = Cover(dr - d); disc > 0) {
                    Blend(px, art ? cover(x, y) : p.surface2, disc);
                }
                if (frac >= 0.9995) {
                    Blend(px, p.accent, Cover(ringW / 2 - std::fabs(d - rr)));
                } else if (frac > 0.002) {
                    float a = std::atan2(dx, -dy);
                    if (a < 0) a += (float)kTau;
                    float dist = std::fabs(d - rr);
                    if (a > arcAngle) {  // past the arc: the distance to its nearer end, for the round caps
                        auto from = [&](Point e) { return std::hypot(x + 0.5f - e.x, y + 0.5f - e.y); };
                        dist = std::min(from(arcStart), from(arcEnd));
                    }
                    Blend(px, p.accent, Cover(ringW / 2 - dist));
                }
            }
            row[x] = px;
        }
    }
    // A knob on the ring while pointing at it or dragging.
    if ((m.hot == kRing || m.seeking) && g.playing != kNone && dur > 0) {
        FillCircle(arcEnd, (float)S(7), p.text);
        FillCircle(arcEnd, (float)S(5), p.accent);
    }

    // Previous / play / next. Play is the round accent button in the middle.
    const Rgb white = 0xFFFFFF, ink = art ? white : p.text;
    bool playing = g.playing != kNone && !g.engine.Paused();
    const wchar_t* glyphs[] = {L"\xE892", playing ? L"\xE769" : L"\xE768", L"\xE893"};
    for (int i = kPrev; i <= kNext; ++i) {
        Point pc = PartCenter(i);
        float r = PartRadius(i);
        if (i == kPlay) FillCircle(pc, r, m.hot == kPlay ? p.accent2 : p.accent);
        else if (m.hot == i && art) FillCircle(pc, r, white, 0.2f);
        else if (m.hot == i) FillCircle(pc, r, p.hover);
        RECT box = Box(pc.x - r, pc.y - r, pc.x + r, pc.y + r);
        // The play triangle looks centered a pixel to the right of the box's center.
        if (i == kPlay && !playing) OffsetRect(&box, S(2), 0);
        Label(glyphs[i], i == kPlay ? m.glyphBig : m.glyph, box, i == kPlay ? white : ink);
    }

    // The time, under the buttons.
    if (g.playing != kNone) {
        std::wstring t = FormatDuration(pos) + L" / " + FormatDuration(dur);
        float y = c.y + PartRadius(kPlay) + S(6);
        Label(t.c_str(), m.timeFont, Box(c.x - dr, y, c.x + dr, y + S(18)), art ? white : p.sub, art ? 0.85f : 1);
    }
}

// Redraws the whole disc into the DIB and presents it with UpdateLayeredWindow. UI thread only.
void Render() {
    if (!m.hwnd) return;
    int size = Diameter();
    if (!EnsureTarget(size)) return;
    Draw();
    POINT src{0, 0};
    SIZE sz{size, size};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(m.hwnd, nullptr, nullptr, &sz, m.mem, &src, 0, &blend, ULW_ALPHA);
    if (g.lightweight) FreeBitmaps();  // the window holds what it shows; the next second makes them again
}

void FreeTarget() {
    FreeBitmaps();
    FreeFonts();
    m = Mini{};
}

void SavePosition() {
    RECT r;
    if (m.hwnd && GetWindowRect(m.hwnd, &r)) IniWrite(L"Mini", L"Position", JoinInts({(int)r.left, (int)r.top}));
}

void SetHot(int hot) {
    if (hot == m.hot) return;
    m.hot = hot;
    TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, m.hwnd, 0};
    TrackMouseEvent(&tme);
    Render();
}

LRESULT CALLBACK MiniProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCHITTEST: {
            // The cover drags the window; the buttons and the ring are clicked.
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(h, &pt);
            float dx = pt.x + 0.5f - Radius(), dy = pt.y + 0.5f - Radius();
            if (dx * dx + dy * dy > Radius() * Radius()) return HTNOWHERE;
            return PartAt(pt) < 0 ? HTCAPTION : HTCLIENT;
        }
        case WM_NCMOUSEMOVE:
            SetHot(-1);
            break;
        case WM_NCLBUTTONDBLCLK:
            ToggleMiniPlayer();  // double-click the cover: the main window again
            return 0;
        case WM_MOUSEMOVE: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (m.seeking) {
                m.seekFrac = RingFracAt(pt, m.seekFrac);
                Render();
                return 0;
            }
            SetHot(PartAt(pt));
            return 0;
        }
        case WM_MOUSELEAVE:
            m.hot = -1;
            Render();
            return 0;
        case WM_LBUTTONDOWN: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (PartAt(pt) == kRing && g.playing != kNone && CurrentDuration() > 0) {
                double dur = CurrentDuration();
                m.seeking = true;
                m.seekFrac = RingFracAt(pt, std::clamp(g.engine.Position() / dur, 0.0, 1.0));
                SetCapture(h);
                Render();
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (m.seeking) {
                m.seeking = false;
                ReleaseCapture();
                g.engine.Seek(m.seekFrac * CurrentDuration());
                Render();
                return 0;
            }
            switch (PartAt(pt)) {
                case kPrev: Previous(); break;
                case kPlay: TogglePause(); break;
                case kNext: Next(); break;
            }
            MiniPlayerTick();
            return 0;
        }
        case WM_CAPTURECHANGED:
            m.seeking = false;
            return 0;
        case WM_MOUSEWHEEL:
            // With cordinal's own volume control off, the system sets the level and the wheel leaves it.
            if (g.softwareVolume) SetVolumePercent(g.volumePercent + (GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 5 : -5));
            return 0;
        case WM_KEYDOWN: {
            bool ctrl = GetKeyState(VK_CONTROL) < 0;
            switch (wp) {
                case VK_SPACE: TogglePause(); break;
                case VK_ESCAPE: ToggleMiniPlayer(); return 0;
                case VK_LEFT: if (ctrl) Previous(); break;
                case VK_RIGHT: if (ctrl) Next(); break;
                case VK_OEM_PLUS:
                case VK_ADD: if (ctrl) { ViewLarger(); return 0; } break;
                case 'M': if (ctrl && GetKeyState(VK_SHIFT) < 0) { ToggleMiniPlayer(); return 0; } break;
            }
            MiniPlayerTick();
            return 0;
        }
        case WM_CONTEXTMENU: {
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, 1, L"&Show the main window\tEsc");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, 2, L"E&xit cordinal");
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (pt.x == -1) GetCursorPos(&pt);
            int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, nullptr);
            DestroyMenu(menu);
            if (cmd == 1) ToggleMiniPlayer();
            else if (cmd == 2) {
                ToggleMiniPlayer();
                PostMessageW(g.hwnd, WM_CLOSE, 0, 0);
            }
            return 0;
        }
        case WM_APPCOMMAND:
            return SendMessageW(g.hwnd, msg, wp, lp);
        case WM_CLOSE:
            ToggleMiniPlayer();  // closing the mini player brings the main window back
            return 0;
        case WM_DESTROY:
            SavePosition();
            FreeTarget();
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

HWND MiniPlayerWindow() { return m.hwnd; }

void ToggleMiniPlayer() {
    // In Lightweight mode, closing the mini player (Esc, a double-click) is the way out of it. The
    // mode closes the mini player it opened; one opened by hand closes below.
    if (m.hwnd && LightweightHidesWindow()) {
        LeaveLightweight();
        if (!m.hwnd) return;
    }
    if (m.hwnd) {
        CloseMiniPlayer();
        return;
    }
    RegisterAppClass(kMiniClass, MiniProc, CS_DBLCLKS);
    // Where it was last time, else the bottom right of the main window's screen.
    RECT owner;
    GetWindowRect(g.hwnd, &owner);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromRect(&owner, MONITOR_DEFAULTTONEAREST), &mi);
    int d = Diameter();
    int x = mi.rcWork.right - d - S(24), y = mi.rcWork.bottom - d - S(24);
    auto saved = ParseInts(IniRead(L"Mini", L"Position", L""));
    if (saved.size() == 2) {
        RECT want{saved[0], saved[1], saved[0] + d, saved[1] + d};
        if (HMONITOR mon = MonitorFromRect(&want, MONITOR_DEFAULTTONULL)) {
            GetMonitorInfoW(mon, &mi);  // kept inside that screen's work area
            x = std::clamp(saved[0], (int)mi.rcWork.left, (int)mi.rcWork.right - d);
            y = std::clamp(saved[1], (int)mi.rcWork.top, (int)mi.rcWork.bottom - d);
        }
    }
    m.hot = -1;
    m.seeking = false;
    m.hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_APPWINDOW | WS_EX_LAYERED, kMiniClass, kAppName,
                             WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX, x, y, d, d, nullptr, nullptr, g.inst, nullptr);
    if (!m.hwnd) return;
    ShowWindow(g.hwnd, SW_HIDE);
    UpdateViewMemory();
    MiniPlayerTick();  // a layered window shows nothing until it's given its picture
    ShowWindow(m.hwnd, SW_SHOW);
    SetForegroundWindow(m.hwnd);
    LightweightMiniChanged();
}

// The icon's menu turns the mini player on and off without ending Lightweight mode: closed from there,
// the mode goes on with only the icon showing. (Esc or a double-click on the mini player itself still
// ends it, as the way back to the window.)
void TrayMiniPlayer() {
    if (m.hwnd && LightweightHidesWindow()) CloseMiniPlayer(false);
    else ToggleMiniPlayer();
}

// For the ui suite: a pixel of the picture last drawn (premultiplied 0xAARRGGBB), 0 when there is none.
uint32_t MiniPlayerPixel(int x, int y) {
    return m.bits && x >= 0 && y >= 0 && x < m.bmpSize && y < m.bmpSize ? m.bits[y * m.bmpSize + x] : 0;
}

// `showMain` false leaves the main window where it is: the notification-area menu closing the mini
// player in Lightweight mode, which stays on with only the icon showing.
void CloseMiniPlayer(bool showMain) {
    if (!m.hwnd) return;
    DetachStartupDefaultsFromMiniPlayer();
    DestroyWindow(m.hwnd);
    UpdateViewMemory();
    LightweightMiniChanged();
    if (!showMain) return;
    ShowWindow(g.hwnd, IsIconic(g.hwnd) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(g.hwnd);
}

void MiniPlayerTick() {
    if (!m.hwnd) return;
    wchar_t title[256] = {};
    GetWindowTextW(g.hwnd, title, (int)std::size(title));
    SetTextIfChanged(m.hwnd, title);  // the taskbar button's name
    Render();
}

}  // namespace app
