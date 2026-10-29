// Mini player: a round window that stays on top. The cover fills the disc; a ring around it shows
// how far the track has played (click or drag it to seek); previous / play / next sit across the
// middle. The main window hides while it shows; Esc, Ctrl+= or a double-click brings it back.
// Drag the window by the cover; the wheel sets the volume. Drawn with Direct2D into a layered
// window, so the edge of the circle is smooth and the corners are see-through.
#include "app.h"

#include <d2d1.h>
#include <dwrite.h>

namespace app {

namespace {

const wchar_t kMiniClass[] = L"CordinalMiniPlayer";

enum Part { kPrev, kPlay, kNext, kRing, kPartCount };

struct Mini {
    HWND hwnd = nullptr;
    int hot = -1;
    bool seeking = false;
    double seekFrac = 0;
    // A 32-bit premultiplied bitmap the render target draws into; UpdateLayeredWindow shows it.
    HDC mem = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ oldBmp = nullptr;
    int bmpSize = 0;
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dw;
    ComPtr<ID2D1DCRenderTarget> rt;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<ID2D1StrokeStyle> roundCaps;
    ComPtr<ID2D1Bitmap> cover;
    uint64_t coverKey = 0;
    ComPtr<IDWriteTextFormat> glyph, glyphBig, timeFont;
} m;

// Geometry, in pixels from the window's top left.
int Diameter() { return S(196); }
float Radius() { return Diameter() / 2.0f; }
float RingRadius() { return Radius() - S(9); }   // the middle of the progress ring
float DiscRadius() { return Radius() - S(17); }  // the cover inside it
D2D1_POINT_2F PartCenter(int part) {
    float c = Radius(), off = (float)S(54);
    return {part == kPrev ? c - off : part == kNext ? c + off : c, c};
}
float PartRadius(int part) { return (float)(part == kPlay ? S(27) : S(20)); }

int PartAt(POINT pt) {
    float dx = pt.x + 0.5f - Radius(), dy = pt.y + 0.5f - Radius(), d = std::sqrt(dx * dx + dy * dy);
    if (d > Radius()) return -1;
    if (d >= DiscRadius() - S(2)) return kRing;
    for (int i = kPrev; i <= kNext; ++i) {
        D2D1_POINT_2F c = PartCenter(i);
        float px = pt.x + 0.5f - c.x, py = pt.y + 0.5f - c.y;
        if (px * px + py * py <= PartRadius(i) * PartRadius(i)) return i;
    }
    return -1;
}

// The ring's fraction under a point: 0 at twelve o'clock, growing clockwise. Dragging across the
// top doesn't jump from the end to the start: it stops there.
double RingFracAt(POINT pt, double previous) {
    double a = std::atan2(pt.x + 0.5 - Radius(), -(pt.y + 0.5 - Radius()));
    double frac = (a < 0 ? a + 2 * 3.14159265358979 : a) / (2 * 3.14159265358979);
    if (previous > 0.75 && frac < 0.25) return 1.0;
    if (previous < 0.25 && frac > 0.75) return 0.0;
    return frac;
}

D2D1_COLOR_F Col(Rgb c, float a = 1) {
    return D2D1::ColorF(((c >> 16) & 0xFF) / 255.f, ((c >> 8) & 0xFF) / 255.f, (c & 0xFF) / 255.f, a);
}

void Fill(D2D1_POINT_2F c, float r, D2D1_COLOR_F color) {
    m.brush->SetColor(color);
    m.rt->FillEllipse(D2D1::Ellipse(c, r, r), m.brush.Get());
}

void Text(const wchar_t* s, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1_COLOR_F c) {
    m.brush->SetColor(c);
    m.rt->DrawTextW(s, (UINT32)wcslen(s), f, r, m.brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

ComPtr<IDWriteTextFormat> Format(const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight) {
    ComPtr<IDWriteTextFormat> f;
    m.dw->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &f);
    if (f) {
        f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    return f;
}

// Drops the device-dependent resources (they must be remade after D2DERR_RECREATE_TARGET); the
// cover is dropped too, so it is re-uploaded from g.art on the next Draw.
void DropDeviceResources() {
    m.rt.Reset();
    m.brush.Reset();
    m.cover.Reset();
    m.coverKey = 0;
}

// Makes the offscreen DIB and Direct2D target, reallocating them only when the size changes. All
// drawing goes here first; Render then hands the bitmap to the layered window.
bool EnsureTarget(int size) {
    if (!m.d2d && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m.d2d.GetAddressOf()))) return false;
    if (!m.dw && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                            reinterpret_cast<IUnknown**>(m.dw.GetAddressOf()))))
        return false;
    if (m.bmpSize != size) {
        if (m.mem) {
            SelectObject(m.mem, m.oldBmp);
            DeleteObject(m.bmp);
            DeleteDC(m.mem);
        }
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(bi.bmiHeader), size, -size, 1, 32};
        void* bits = nullptr;
        m.mem = CreateCompatibleDC(nullptr);
        m.bmp = m.mem ? CreateDIBSection(m.mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
        if (!m.bmp) {
            // Leave bmpSize unset so a later paint retries the allocation.
            if (m.mem) DeleteDC(m.mem);
            m.mem = nullptr;
            m.bmpSize = 0;
            return false;
        }
        m.oldBmp = SelectObject(m.mem, m.bmp);
        m.bmpSize = size;
        m.glyph = Format(IconFace(), (float)S(16), DWRITE_FONT_WEIGHT_NORMAL);
        m.glyphBig = Format(IconFace(), (float)S(20), DWRITE_FONT_WEIGHT_NORMAL);
        m.timeFont = Format(L"Segoe UI", (float)S(11), DWRITE_FONT_WEIGHT_SEMI_BOLD);
    }
    if (!m.bmp) return false;
    if (!m.rt) {
        // 96 DPI: one unit is one pixel, as everywhere else S() is used.
        // Software rasterizing: a 196-pixel disc doesn't need the GPU, and this keeps Direct3D out of memory.
        auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                                  D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        if (FAILED(m.d2d->CreateDCRenderTarget(&props, m.rt.GetAddressOf()))) return false;
        m.rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);  // ClearType needs an opaque background
        m.rt->CreateSolidColorBrush(D2D1::ColorF(0), m.brush.GetAddressOf());
    }
    if (!m.roundCaps) {
        auto sp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND);
        m.d2d->CreateStrokeStyle(sp, nullptr, 0, m.roundCaps.GetAddressOf());
    }
    RECT bind{0, 0, size, size};
    return SUCCEEDED(m.rt->BindDC(m.mem, &bind));
}

void Draw() {
    const Palette& p = P();
    float R = Radius();
    D2D1_POINT_2F c{R, R};
    m.rt->Clear(D2D1::ColorF(0, 0, 0, 0));

    // The puck, its edge, and the ring's track.
    Fill(c, R - 0.5f, Col(p.surface));
    m.brush->SetColor(Col(p.border));
    m.rt->DrawEllipse(D2D1::Ellipse(c, R - 1, R - 1), m.brush.Get(), 1);
    float ringW = (float)S(5), rr = RingRadius();
    m.brush->SetColor(Col(p.border));
    m.rt->DrawEllipse(D2D1::Ellipse(c, rr, rr), m.brush.Get(), ringW);

    // The cover, scaled to fill the disc, darkened so the buttons read on any picture.
    float dr = DiscRadius();
    uint64_t key = g.art.Empty() ? 0 : g.artFor + 1ull;  // g.art only changes along with artFor
    if (key != m.coverKey) {
        m.cover.Reset();
        m.coverKey = key;
        if (key) {
            auto props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
            m.rt->CreateBitmap(D2D1::SizeU(g.art.width, g.art.height), g.art.pixels.data(), g.art.width * 4, props,
                               m.cover.GetAddressOf());
        }
    }
    bool art = m.cover != nullptr;
    if (art) {
        ComPtr<ID2D1BitmapBrush> fill;
        D2D1_SIZE_F bs = m.cover->GetSize();
        float scale = 2 * dr / std::min(bs.width, bs.height);
        auto brushProps = D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP,
                                                      D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        if (SUCCEEDED(m.rt->CreateBitmapBrush(m.cover.Get(), brushProps, fill.GetAddressOf()))) {
            fill->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) *
                               D2D1::Matrix3x2F::Translation(c.x - bs.width * scale / 2, c.y - bs.height * scale / 2));
            m.rt->FillEllipse(D2D1::Ellipse(c, dr, dr), fill.Get());
        }
        Fill(c, dr, D2D1::ColorF(0, 0, 0, 0.42f));
    } else {
        Fill(c, dr, Col(p.surface2));
    }

    // Progress: an accent arc from twelve o'clock, with a knob while pointing at it or dragging.
    double dur = CurrentDuration(), pos = m.seeking ? m.seekFrac * dur : g.engine.Position();
    double frac = dur > 0 && g.playing != kNone ? std::clamp(pos / dur, 0.0, 1.0) : 0;
    auto at = [&](double f) {
        double a = f * 2 * 3.14159265358979;
        return D2D1::Point2F(c.x + rr * (float)std::sin(a), c.y - rr * (float)std::cos(a));
    };
    m.brush->SetColor(Col(p.accent));
    if (frac >= 0.9995) {
        m.rt->DrawEllipse(D2D1::Ellipse(c, rr, rr), m.brush.Get(), ringW);
    } else if (frac > 0.002) {
        ComPtr<ID2D1PathGeometry> arc;
        ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(m.d2d->CreatePathGeometry(arc.GetAddressOf())) && SUCCEEDED(arc->Open(sink.GetAddressOf()))) {
            sink->BeginFigure(at(0), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddArc(D2D1::ArcSegment(at(frac), D2D1::SizeF(rr, rr), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                          frac > 0.5 ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            m.rt->DrawGeometry(arc.Get(), m.brush.Get(), ringW, m.roundCaps.Get());
        }
    }
    if ((m.hot == kRing || m.seeking) && g.playing != kNone && dur > 0) {
        Fill(at(frac), (float)S(7), Col(p.text));
        Fill(at(frac), (float)S(5), Col(p.accent));
    }

    // Previous / play / next. Play is the round accent button in the middle.
    D2D1_COLOR_F ink = art ? D2D1::ColorF(1, 1, 1) : Col(p.text);
    D2D1_COLOR_F hover = art ? D2D1::ColorF(1, 1, 1, 0.2f) : Col(p.hover);
    bool playing = g.playing != kNone && !g.engine.Paused();
    const wchar_t* glyphs[] = {L"\xE892", playing ? L"\xE769" : L"\xE768", L"\xE893"};
    for (int i = kPrev; i <= kNext; ++i) {
        D2D1_POINT_2F pc = PartCenter(i);
        float r = PartRadius(i);
        if (i == kPlay) Fill(pc, r, m.hot == kPlay ? Col(p.accent2) : Col(p.accent));
        else if (m.hot == i) Fill(pc, r, hover);
        D2D1_RECT_F box{pc.x - r, pc.y - r, pc.x + r, pc.y + r};
        // The play triangle looks centered a pixel to the right of the box's center.
        if (i == kPlay && !playing) box.left += S(2), box.right += S(2);
        Text(glyphs[i], i == kPlay ? m.glyphBig.Get() : m.glyph.Get(), box, i == kPlay ? D2D1::ColorF(1, 1, 1) : ink);
    }

    // The time, under the buttons.
    if (g.playing != kNone) {
        std::wstring t = FormatDuration(pos) + L" / " + FormatDuration(dur);
        float y = c.y + PartRadius(kPlay) + S(6);
        D2D1_COLOR_F sub = art ? D2D1::ColorF(1, 1, 1, 0.85f) : Col(p.sub);
        Text(t.c_str(), m.timeFont.Get(), {c.x - dr, y, c.x + dr, y + S(18)}, sub);
    }
}

// Redraws the whole disc into the DIB and presents it with UpdateLayeredWindow. UI thread only.
void Render() {
    if (!m.hwnd) return;
    int size = Diameter();
    if (!EnsureTarget(size)) return;
    m.rt->BeginDraw();
    Draw();
    if (m.rt->EndDraw() == D2DERR_RECREATE_TARGET) DropDeviceResources();
    POINT src{0, 0};
    SIZE sz{size, size};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(m.hwnd, nullptr, nullptr, &sz, m.mem, &src, 0, &blend, ULW_ALPHA);
}

void FreeTarget() {
    if (m.mem) {
        SelectObject(m.mem, m.oldBmp);
        DeleteObject(m.bmp);
        DeleteDC(m.mem);
    }
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
            SetVolumePercent(g.volumePercent + (GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 5 : -5));
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
    if (m.hwnd) {
        DetachStartupDefaultsFromMiniPlayer();
        DestroyWindow(m.hwnd);
        UpdateViewMemory();
        ShowWindow(g.hwnd, IsIconic(g.hwnd) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(g.hwnd);
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
}

void MiniPlayerTick() {
    if (!m.hwnd) return;
    wchar_t title[256] = {};
    GetWindowTextW(g.hwnd, title, (int)std::size(title));
    SetTextIfChanged(m.hwnd, title);  // the taskbar button's name
    Render();
}

}  // namespace app
