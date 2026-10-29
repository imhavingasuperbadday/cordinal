// A small notice that fades in under the points chip and out again (a level-up, a new rank).
// It never takes focus; clicking it opens cordinal points.
#include "app.h"

namespace app {

namespace {

const wchar_t kToastClass[] = L"CordinalToast";
constexpr UINT_PTR kToastTimer = 1;
// Fade and hold times in milliseconds; the timer below ticks at roughly 60 fps.
constexpr ULONGLONG kFadeIn = 160, kHold = 4200, kFadeOut = 600;

// The one visible toast; showing another replaces it.
struct Toast {
    HWND hwnd = nullptr;
    std::wstring title, body;
    ULONGLONG shownAt = 0;
} t;

// Draws the border, accent stripe, star, title and body into the back buffer.
void PaintContent(HDC mem, RECT rc) {
    FillSolid(mem, rc, Ref(P().border));
    FillSolid(mem, {1, 1, rc.right - 1, rc.bottom - 1}, Ref(P().surface2));
    FillSolid(mem, {1, 1, 1 + S(4), rc.bottom - 1}, Ref(P().accent));
    SetBkMode(mem, TRANSPARENT);
    // A star in the accent, then the title and the line under it.
    RECT star{S(14), 0, S(44), rc.bottom};
    HGDIOBJ oldFont = SelectObject(mem, g.fontArt);
    SetTextColor(mem, Ref(P().accent));
    DrawTextW(mem, L"\x2605", -1, &star, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    RECT title{S(50), S(9), rc.right - S(10), rc.bottom / 2 + S(2)};
    SelectObject(mem, g.fontBold);
    SetTextColor(mem, TextColor());
    DrawTextW(mem, t.title.c_str(), -1, &title, DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    RECT body{S(50), rc.bottom / 2 + S(2), rc.right - S(10), rc.bottom - S(8)};
    SelectObject(mem, g.font);
    SetTextColor(mem, SubColor());
    DrawTextW(mem, t.body.c_str(), -1, &body, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(mem, oldFont);
}

// The toast's window procedure; the timer drives the fade and hovering holds it up.
LRESULT CALLBACK ToastProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT:
            DoubleBufferedPaint(h, PaintContent);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        case WM_LBUTTONUP:
            DestroyWindow(h);
            ShowPoints();
            return 0;
        case WM_TIMER: {
            ULONGLONG e = GetTickCount64() - t.shownAt;
            double a = e < kFadeIn ? (double)e / kFadeIn
                       : e < kFadeIn + kHold ? 1.0
                       : 1.0 - (double)(e - kFadeIn - kHold) / kFadeOut;
            // Hovering keeps it up.
            POINT pt;
            GetCursorPos(&pt);
            RECT wr;
            GetWindowRect(h, &wr);
            if (PtInRect(&wr, pt) && e > kFadeIn) {
                t.shownAt = GetTickCount64() - kFadeIn;
                a = 1.0;
            }
            if (a <= 0) {
                DestroyWindow(h);
                return 0;
            }
            SetLayeredWindowAttributes(h, 0, (BYTE)(255 * std::clamp(a, 0.0, 1.0)), LWA_ALPHA);
            return 0;
        }
        case WM_DESTROY:
            KillTimer(h, kToastTimer);
            if (t.hwnd == h) t.hwnd = nullptr;
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

// Shows a toast anchored under the points chip, the mini player, or the screen corner.
void ShowToast(const std::wstring& title, const std::wstring& body) {
    RegisterAppClass(kToastClass, ToastProc, 0, false);
    if (t.hwnd) DestroyWindow(t.hwnd);
    t.title = title;
    t.body = body;
    int w = S(270), h = S(60);
    // Under the points chip; with the main window away, under the mini player or at the corner of the screen.
    RECT anchor{};
    HWND mini = MiniPlayerWindow();
    bool mainShown = g.hwnd && IsWindowVisible(g.hwnd) && !IsIconic(g.hwnd);
    if (mainShown && g.pointsChip && IsWindowVisible(g.pointsChip)) {
        GetWindowRect(g.pointsChip, &anchor);
    } else if (mainShown) {  // the layout hid the chip: the window's top right corner
        GetWindowRect(g.hwnd, &anchor);
        anchor.right -= S(16);
        anchor.bottom = anchor.top + S(80);
    } else if (mini) {
        GetWindowRect(mini, &anchor);
    }
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromRect(&anchor, MONITOR_DEFAULTTOPRIMARY), &mi);
    RECT work = mi.rcWork;
    int x = mainShown || mini ? anchor.right - w : work.right - w - S(16);
    int y = mainShown || mini ? anchor.bottom + S(6) : work.bottom - h - S(16);
    if (y + h > work.bottom) y = anchor.top - h - S(6);
    x = std::clamp(x, (int)work.left, (int)work.right - w);
    y = std::clamp(y, (int)work.top, (int)work.bottom - h);
    t.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED, kToastClass, title.c_str(),
                             WS_POPUP, x, y, w, h, g.hwnd, nullptr, g.inst, nullptr);
    if (!t.hwnd) return;
    SetLayeredWindowAttributes(t.hwnd, 0, 0, LWA_ALPHA);
    t.shownAt = GetTickCount64();
    ShowWindow(t.hwnd, SW_SHOWNOACTIVATE);
    SetTimer(t.hwnd, kToastTimer, 16, nullptr);
}

}  // namespace app
