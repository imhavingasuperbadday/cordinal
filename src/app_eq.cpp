// One equalizer window with switchable cordinal and headphone controls. Page switches preserve
// drafts and audio settings; only the controls themselves apply sound changes.
// Edits update g.eq, push it to the engine through EqApply(), and persist on TB_ENDTRACK, the end of a
// drag on the curve, or window destroy. Only one window exists at a time, tracked by g.eqWnd.
// The curve above the sliders draws what the EQ really does (the filters as designed, overlap
// included) and is edited directly: in graphic mode dragging sets the nearest band, in parametric
// mode every filter is a point to drag, scroll, add or delete.
#include "app.h"
#include "streamfile.h"

namespace app {

struct EqControls {
    HWND enable = nullptr, label = nullptr, preset = nullptr, reset = nullptr, autoPreamp = nullptr, graph = nullptr;
    HWND parametric = nullptr, crossfeed = nullptr, import = nullptr, apply = nullptr, text = nullptr, hint = nullptr;
    HWND cordinalPage = nullptr, headphonePage = nullptr, pageHint = nullptr;
    bool headphones = false;
    HWND sliders[kEqBands + 1] = {};  // [0] = preamp
    HWND names[kEqBands + 1] = {}, values[kEqBands + 1] = {};
    int selected = -1;     // the curve's selected filter (parametric) or band (graphic)
    bool dragging = false;  // a drag on the curve is changing g.eq; it is saved when it ends
} g_eqc;

void SelectEqPage(bool headphones) {
    if (!g.eqWnd) return;
    if (headphones && !g.headphonesWnd) {
        g.headphonesWnd = CreateWindowExW(WS_EX_CONTROLPARENT, kHeadphonesClass, L"Headphone EQ",
            WS_CHILD | WS_CLIPCHILDREN, 0, 0, 0, 0, g.eqWnd, (HMENU)(INT_PTR)IDC_EQ_HARDWARE, g.inst, nullptr);
        if (!g.headphonesWnd) { ShowMessage(L"Couldn't open headphone controls."); headphones = false; }
        else { HeadphonesSync(true); g.headsets.Refresh(); }
    }
    g_eqc.headphones = headphones;
    Button_SetCheck(g_eqc.cordinalPage, headphones ? BST_UNCHECKED : BST_CHECKED);
    Button_SetCheck(g_eqc.headphonePage, headphones ? BST_CHECKED : BST_UNCHECKED);
    // Keep both panels alive so a draft profile or unapplied headset setting survives switching.
    for (HWND child = GetWindow(g.eqWnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        if (child == g_eqc.cordinalPage || child == g_eqc.headphonePage || child == g_eqc.pageHint) continue;
        ShowWindow(child, (child == g.headphonesWnd ? headphones : !headphones) ? SW_SHOW : SW_HIDE);
    }
    EqLayout(g.eqWnd);
    InvalidateRect(g.eqWnd, nullptr, TRUE);
}

bool EqualizerDialogMessage(MSG* message) {
    if (!g.eqWnd || (message->hwnd != g.eqWnd && !IsChild(g.eqWnd, message->hwnd))) return false;
    if (message->message == WM_KEYDOWN && message->wParam == VK_TAB && (GetKeyState(VK_CONTROL) & 0x8000)) {
        SelectEqPage(!g_eqc.headphones);
        SetFocus(g_eqc.headphones ? g_eqc.headphonePage : g_eqc.cordinalPage);
        return true;
    }
    return IsDialogMessageW(g.eqWnd, message) != FALSE;
}

float* EqValue(int i) { return i == 0 ? &g.eq.preamp : &g.eq.gains[i - 1]; }

static unsigned EqRate() { return g.engine.Rate() ? g.engine.Rate() : 48000; }

static const wchar_t* FilterTypeName(EqFilterType type) {
    return type == EqFilterType::Peak ? L"peak" : type == EqFilterType::LowShelf ? L"low shelf" : L"high shelf";
}

// What the curve can do in the current mode, and, in parametric mode, which filter is selected.
static void UpdateEqHint() {
    std::wstring text;
    if (!g.eq.parametric) {
        text = L"Drag on the curve or the sliders: each band lands on its value. Right-click the curve for more.\n"
               L"Parametric EQ replaces the bands with up to 20 filters; crossfeed works independently.";
    } else {
        const int i = g_eqc.selected;
        if (i >= 0 && i < g.eq.filterCount) {
            const EqFilter& f = g.eq.filters[i];
            wchar_t line[160];
            swprintf_s(line, L"Filter %d of %d: %ls at %.0f Hz, %+.1f dB, Q %.2f%ls.\n", i + 1, g.eq.filterCount,
                       FilterTypeName(f.type), f.frequency, f.gain, f.q, f.enabled ? L"" : L" (off)");
            text = line;
        } else {
            text = L"Select a point on the curve to edit it.\n";
        }
        text += L"Drag a point, scroll for Q, double-click to add, right-click for type or delete.";
    }
    SetTextIfChanged(g_eqc.hint, text.c_str());
}

// Pushes g.eq into the controls: the enable check, each slider position, its signed dB label, and the curve.
void EqSync() {
    // Vertical trackbars have their minimum at the top: position = (12 - dB) * 2.
    Button_SetCheck(g_eqc.enable, g.eq.enabled ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(g_eqc.parametric, g.eq.parametric ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(g_eqc.crossfeed, g.eq.crossfeed ? BST_CHECKED : BST_UNCHECKED);
    for (int i = 0; i <= kEqBands; ++i) {
        SendMessageW(g_eqc.sliders[i], TBM_SETPOS, TRUE, (LPARAM)std::lround(((i ? 12 : 24) - *EqValue(i)) * 2));
        EnableWindow(g_eqc.sliders[i], i == 0 || !g.eq.parametric);
        wchar_t v[16];
        swprintf_s(v, L"%+.1f", *EqValue(i));
        SetWindowTextW(g_eqc.values[i], v);
    }
    if (g_eqc.selected >= (g.eq.parametric ? g.eq.filterCount : kEqBands)) g_eqc.selected = -1;
    UpdateEqHint();
    if (g_eqc.graph) InvalidateRect(g_eqc.graph, nullptr, FALSE);
}

static void ProfileToEditor() {
    SetWindowTextW(g_eqc.text, FromUtf8(FormatAutoEq(g.eq)).c_str());
}

void EqDeviceSync(bool changedOutput) {
    if (!g.eqWnd) return;
    std::wstring title = L"Equalizer";
    if (!g.headset.name.empty()) title += L" - " + (g.headset.model.empty() ? g.headset.name : g.headset.model);
    if (g.rememberOutputEq) title += L" (device profile)";
    SetWindowTextW(g.eqWnd, title.c_str());
    if (changedOutput) { EqSync(); ProfileToEditor(); }
}

static bool ApplyProfile(std::string_view text) {
    std::wstring error;
    EqSettings settings = g.eq;
    if (!ParseAutoEq(text, settings, error)) {
        MessageBoxW(g.eqWnd, error.c_str(), L"Parametric EQ", MB_OK | MB_ICONWARNING);
        return false;
    }
    g.eq = settings;
    EqApply();
    ProfileToEditor();
    SaveEq();
    return true;
}

static void ImportAutoEq() {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    COMDLG_FILTERSPEC filter[] = {{L"AutoEq parametric profiles", L"*.txt"}};
    dlg->SetFileTypes(1, filter);
    dlg->SetTitle(L"Import AutoEq ParametricEQ.txt");
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dlg->Show(g.eqWnd)) || FAILED(dlg->GetResult(&item)) ||
        FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return;
    FileSource file(path);
    CoTaskMemFree(path);
    std::string bytes;
    if (!file.Valid() || file.Size() > 65536 || !file.Read(0, (size_t)file.Size(), bytes)) {
        MessageBoxW(g.eqWnd, L"Couldn't read the profile (maximum 64 KiB).", L"Parametric EQ", MB_OK | MB_ICONWARNING);
        return;
    }
    if (bytes.starts_with("\xFF\xFE") && bytes.size() % 2 == 0) {
        std::wstring wide((bytes.size() - 2) / 2, L'\0');
        memcpy(wide.data(), bytes.data() + 2, bytes.size() - 2);
        bytes = ToUtf8(wide);
    }
    ApplyProfile(bytes);
}

void EqApply() {
    g.engine.SetEq(g.eq);
    if (g.eqWnd) EqSync();
    RefreshGains();
}

// A curve edit is heard at once; the profile text and settings.ini follow when it is finished.
static void CurveEdited(bool finished) {
    g.eq.enabled = true;
    EqApply();
    if (finished) {
        ProfileToEditor();
        SaveEq();
    }
}

// Turns the preamp down by the curve's highest boost, so a boosted EQ has headroom instead of
// leaning on the limiter. A curve that only cuts gets 0 dB.
static void AutoPreamp() {
    EqSettings curve = g.eq;
    curve.enabled = true;
    curve.preamp = 0;
    Equalizer eq;
    eq.Configure(curve, EqRate());
    g.eq.preamp = -(float)(std::ceil(std::max(0.0, eq.PeakGainDb()) * 10) / 10);
    CurveEdited(true);
}

// ---- the curve

namespace {
constexpr double kGraphLow = 20, kGraphHigh = 20000, kGraphRange = 18;  // Hz, Hz, +/- dB

struct GraphArea {
    RECT plot;
    double X(double hz) const {
        return plot.left + std::log(hz / kGraphLow) / std::log(kGraphHigh / kGraphLow) * (plot.right - plot.left);
    }
    double Hz(int x) const {
        const double t = std::clamp((double)(x - plot.left) / std::max(1L, plot.right - plot.left), 0.0, 1.0);
        return kGraphLow * std::pow(kGraphHigh / kGraphLow, t);
    }
    double Y(double db) const { return plot.top + (kGraphRange - db) / (2 * kGraphRange) * (plot.bottom - plot.top); }
    double Db(int y) const {
        return std::clamp(kGraphRange - (double)(y - plot.top) / std::max(1L, plot.bottom - plot.top) * 2 * kGraphRange,
                          -kGraphRange, kGraphRange);
    }
};

GraphArea AreaOf(HWND h) {
    RECT rc;
    GetClientRect(h, &rc);
    return {{rc.left + S(34), rc.top + S(6), rc.right - S(8), rc.bottom - S(18)}};
}

// The point a filter or band is drawn at: its frequency, and its gain (the slider's value for a band).
POINT NodeAt(const GraphArea& a, int i) {
    const double hz = g.eq.parametric ? g.eq.filters[i].frequency : kEqFrequencies[i];
    const double db = g.eq.parametric ? g.eq.filters[i].gain : g.eq.gains[i];
    return {(LONG)std::lround(a.X(std::clamp(hz, kGraphLow, kGraphHigh))),
            (LONG)std::lround(a.Y(std::clamp(db, -kGraphRange, kGraphRange)))};
}

int NodeCount() { return g.eq.parametric ? g.eq.filterCount : kEqBands; }

int HitNode(const GraphArea& a, POINT pt) {
    int best = -1;
    long bestDistance = S(9) * S(9) + 1;
    for (int i = 0; i < NodeCount(); ++i) {
        const POINT n = NodeAt(a, i);
        const long d = (n.x - pt.x) * (n.x - pt.x) + (n.y - pt.y) * (n.y - pt.y);
        if (d < bestDistance) { bestDistance = d; best = i; }
    }
    return best;
}

int NearestBand(double hz) {
    int best = 0;
    for (int i = 1; i < kEqBands; ++i)
        if (std::fabs(std::log(hz / kEqFrequencies[i])) < std::fabs(std::log(hz / kEqFrequencies[best]))) best = i;
    return best;
}

double RoundTo(double v, double step) { return std::round(v / step) * step; }

// Three significant figures, the precision AutoEq profiles use.
float RoundHz(double hz) {
    const double step = std::pow(10.0, std::floor(std::log10(std::max(hz, 1.0))) - 2);
    return (float)RoundTo(hz, step);
}

void SetFilterAt(int i, const GraphArea& a, POINT pt) {
    EqFilter& f = g.eq.filters[i];
    f.frequency = RoundHz(a.Hz(pt.x));
    f.gain = (float)RoundTo(a.Db(pt.y), 0.1);
}

void SetBandAt(const GraphArea& a, POINT pt) {
    const int band = NearestBand(a.Hz(pt.x));
    g_eqc.selected = band;
    g.eq.gains[band] = (float)std::clamp(RoundTo(a.Db(pt.y), 0.5), -12.0, 12.0);
}

bool AddFilter(double hz, double db) {
    if (g.eq.filterCount >= kParametricBands) {
        ShowMessage(L"The parametric EQ holds 20 filters.");
        return false;
    }
    g.eq.filters[g.eq.filterCount] = {EqFilterType::Peak, RoundHz(hz), (float)RoundTo(db, 0.1), kGraphicQ, true};
    g_eqc.selected = g.eq.filterCount++;
    return true;
}

void DeleteFilter(int i) {
    if (i < 0 || i >= g.eq.filterCount) return;
    std::copy(g.eq.filters.begin() + i + 1, g.eq.filters.begin() + g.eq.filterCount, g.eq.filters.begin() + i);
    g.eq.filters[--g.eq.filterCount] = {};
    g_eqc.selected = std::min(i, g.eq.filterCount - 1);
}

void ScaleQ(int i, double factor) {
    EqFilter& f = g.eq.filters[i];
    f.q = (float)std::clamp(RoundTo(f.q * factor, 0.01), 0.1, 30.0);
}

void FilterMenu(HWND h, POINT client) {
    const GraphArea a = AreaOf(h);
    const int i = g.eq.parametric ? HitNode(a, client) : -1;
    HMENU menu = CreatePopupMenu();
    if (i >= 0) {
        g_eqc.selected = i;
        const EqFilter& f = g.eq.filters[i];
        AppendMenuW(menu, MF_STRING | (f.type == EqFilterType::Peak ? MF_CHECKED : 0), 1, L"&Peak");
        AppendMenuW(menu, MF_STRING | (f.type == EqFilterType::LowShelf ? MF_CHECKED : 0), 2, L"&Low shelf");
        AppendMenuW(menu, MF_STRING | (f.type == EqFilterType::HighShelf ? MF_CHECKED : 0), 3, L"&High shelf");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (f.enabled ? MF_CHECKED : 0), 4, L"&On");
        AppendMenuW(menu, MF_STRING, 5, L"&Delete\tDel");
    } else {
        AppendMenuW(menu, MF_STRING | (g.eq.filterCount < kParametricBands ? 0 : MF_GRAYED), 6, L"&Add a filter here");
        AppendMenuW(menu, MF_STRING, 7, L"Auto &preamp");
    }
    if (!g.eq.parametric) EnableMenuItem(menu, 6, MF_BYCOMMAND | MF_GRAYED);
    POINT screen = client;
    ClientToScreen(h, &screen);
    const int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, screen.x, screen.y, 0, h, nullptr);
    DestroyMenu(menu);
    if (command >= 1 && command <= 3) g.eq.filters[i].type = (EqFilterType)(command - 1);
    else if (command == 4) g.eq.filters[i].enabled = !g.eq.filters[i].enabled;
    else if (command == 5) DeleteFilter(i);
    else if (command == 6 && !AddFilter(a.Hz(client.x), a.Db(client.y))) return;
    else if (command == 7) return AutoPreamp();
    if (command) CurveEdited(true);
    else InvalidateRect(h, nullptr, FALSE);
}

void PaintGraph(HWND h, HDC target) {
    RECT rc;
    GetClientRect(h, &rc);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, rc.right, rc.bottom), oldBitmap = (HBITMAP)SelectObject(dc, bitmap);
    const Palette& p = P();
    HBRUSH surface = CreateSolidBrush(Ref(p.surface)), field = CreateSolidBrush(Ref(p.bg));
    FillRect(dc, &rc, surface);
    const GraphArea a = AreaOf(h);
    FillRect(dc, &a.plot, field);
    HFONT oldFont = (HFONT)SelectObject(dc, g.font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, Ref(p.sub));
    HPEN grid = CreatePen(PS_SOLID, 1, Ref(p.border)), zero = CreatePen(PS_SOLID, 1, Ref(p.faint));
    HPEN oldPen = (HPEN)SelectObject(dc, grid);
    HBRUSH oldBrush = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
    for (double hz : {50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0}) {
        const int x = (int)std::lround(a.X(hz));
        MoveToEx(dc, x, a.plot.top, nullptr);
        LineTo(dc, x, a.plot.bottom);
        wchar_t label[8];
        swprintf_s(label, hz >= 1000 ? L"%gk" : L"%g", hz >= 1000 ? hz / 1000 : hz);
        RECT text{x - S(30), a.plot.bottom + S(2), x + S(30), rc.bottom};
        DrawTextW(dc, label, -1, &text, DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    }
    for (int db = -12; db <= 12; db += 6) {
        const int y = (int)std::lround(a.Y(db));
        SelectObject(dc, db ? grid : zero);
        MoveToEx(dc, a.plot.left, y, nullptr);
        LineTo(dc, a.plot.right, y);
        wchar_t label[8];
        swprintf_s(label, db ? L"%+d" : L"0", db);
        RECT text{0, y - S(9), a.plot.left - S(4), y + S(9)};
        DrawTextW(dc, label, -1, &text, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    // The curve as the engine builds it, drawn even while the EQ is off (dimmed), so it can be shaped first.
    EqSettings curve = g.eq;
    curve.enabled = true;
    Equalizer eq;
    eq.Configure(curve, EqRate());
    std::vector<POINT> line;
    for (LONG x = a.plot.left; x <= a.plot.right; ++x) {
        const double db = std::clamp(eq.FilterDb(a.Hz(x)), -kGraphRange - 1, kGraphRange + 1);
        line.push_back({x, (LONG)std::lround(a.Y(db))});
    }
    HRGN clip = CreateRectRgnIndirect(&a.plot);
    SelectClipRgn(dc, clip);
    HPEN curvePen = CreatePen(PS_SOLID, S(2), Ref(g.eq.enabled ? p.accent : p.faint));
    SelectObject(dc, curvePen);
    Polyline(dc, line.data(), (int)line.size());
    // Nodes: filled when on, a ring when off; the selected one is larger and outlined in the text colour.
    HBRUSH accent = CreateSolidBrush(Ref(g.eq.enabled ? p.accent : p.faint));
    HPEN ring = CreatePen(PS_SOLID, 1, Ref(p.surface)), selectedRing = CreatePen(PS_SOLID, S(2), Ref(p.text));
    for (int i = 0; i < NodeCount(); ++i) {
        const POINT n = NodeAt(a, i);
        const bool on = !g.eq.parametric || g.eq.filters[i].enabled, selected = i == g_eqc.selected;
        const int r = selected ? S(6) : g.eq.parametric ? S(5) : S(3);
        SelectObject(dc, selected ? selectedRing : on ? ring : curvePen);
        SelectObject(dc, on ? accent : field);
        Ellipse(dc, n.x - r, n.y - r, n.x + r + 1, n.y + r + 1);
    }
    SelectClipRgn(dc, nullptr);
    DeleteObject(clip);
    if (g.showFocusBoxes && GetFocus() == h) {
        RECT focus = a.plot;
        InflateRect(&focus, 1, 1);
        SetTextColor(dc, Ref(p.text));
        DrawFocusRect(dc, &focus);
    }
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldFont);
    BitBlt(target, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBitmap);
    for (HGDIOBJ o : {(HGDIOBJ)bitmap, (HGDIOBJ)surface, (HGDIOBJ)field, (HGDIOBJ)grid, (HGDIOBJ)zero, (HGDIOBJ)curvePen,
                      (HGDIOBJ)accent, (HGDIOBJ)ring, (HGDIOBJ)selectedRing})
        DeleteObject(o);
    DeleteDC(dc);
}

// Keys for the selected point: arrows move it (Shift: by a third of an octave or 3 dB), Page Up/Down
// change Q, Home/End pick the previous/next point, Insert adds one and Delete removes it.
bool GraphKey(WPARAM key) {
    const bool shift = GetKeyState(VK_SHIFT) & 0x8000;
    const int count = NodeCount();
    int& i = g_eqc.selected;
    if (key == VK_HOME || key == VK_END) {
        if (count) i = i < 0 ? 0 : (i + (key == VK_END ? 1 : count - 1)) % count;
        EqSync();
        return true;
    }
    if (key == VK_INSERT && g.eq.parametric) {
        if (AddFilter(1000, 0)) CurveEdited(true);
        return true;
    }
    if (i < 0 || i >= count) return false;
    const double octave = shift ? 1.0 / 3 : 1.0 / 12, step = shift ? 3 : 0.5;
    if (!g.eq.parametric) {
        if (key == VK_LEFT || key == VK_RIGHT) i = std::clamp(i + (key == VK_RIGHT ? 1 : -1), 0, kEqBands - 1);
        else if (key == VK_UP || key == VK_DOWN)
            g.eq.gains[i] = (float)std::clamp(g.eq.gains[i] + (key == VK_UP ? step : -step), -12.0, 12.0);
        else return false;
        if (key == VK_UP || key == VK_DOWN) CurveEdited(true);
        else EqSync();
        return true;
    }
    EqFilter& f = g.eq.filters[i];
    if (key == VK_LEFT || key == VK_RIGHT)
        f.frequency = RoundHz(std::clamp(f.frequency * std::pow(2.0, key == VK_RIGHT ? octave : -octave), kGraphLow, kGraphHigh));
    else if (key == VK_UP || key == VK_DOWN)
        f.gain = (float)std::clamp(RoundTo(f.gain + (key == VK_UP ? step : -step), 0.1), -kGraphRange, kGraphRange);
    else if (key == VK_PRIOR || key == VK_NEXT) ScaleQ(i, key == VK_PRIOR ? 1.15 : 1 / 1.15);
    else if (key == VK_DELETE) DeleteFilter(i);
    else return false;
    CurveEdited(true);
    return true;
}

LRESULT CALLBACK GraphProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            PaintGraph(h, dc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS;
        case WM_KEYDOWN:
            if (GraphKey(wp)) return 0;
            break;
        case WM_LBUTTONDOWN: {
            SetFocus(h);
            const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            const GraphArea a = AreaOf(h);
            if (g.eq.parametric) {
                g_eqc.selected = HitNode(a, pt);
                g_eqc.dragging = g_eqc.selected >= 0;
                EqSync();
            } else {
                g_eqc.dragging = true;
                SetBandAt(a, pt);
                CurveEdited(false);
            }
            if (g_eqc.dragging) SetCapture(h);
            return 0;
        }
        case WM_MOUSEMOVE:
            if (g_eqc.dragging && GetCapture() == h) {
                const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                const GraphArea a = AreaOf(h);
                if (g.eq.parametric) SetFilterAt(g_eqc.selected, a, pt);
                else SetBandAt(a, pt);
                CurveEdited(false);
            }
            return 0;
        case WM_LBUTTONUP:
        case WM_CAPTURECHANGED:
            if (g_eqc.dragging) {
                g_eqc.dragging = false;
                if (GetCapture() == h) ReleaseCapture();
                CurveEdited(true);
            }
            return 0;
        case WM_LBUTTONDBLCLK:
            if (g.eq.parametric) {
                const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                const GraphArea a = AreaOf(h);
                if (HitNode(a, pt) < 0 && AddFilter(a.Hz(pt.x), a.Db(pt.y))) CurveEdited(true);
            }
            return 0;
        case WM_MOUSEWHEEL:
            if (g.eq.parametric && g_eqc.selected >= 0 && g_eqc.selected < g.eq.filterCount) {
                ScaleQ(g_eqc.selected, std::pow(1.15, GET_WHEEL_DELTA_WPARAM(wp) / (double)WHEEL_DELTA));
                CurveEdited(true);
            }
            return 0;
        case WM_CONTEXTMENU: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (pt.x == -1 && pt.y == -1) {  // the keyboard's menu key: at the selected point
                pt = g_eqc.selected >= 0 && g_eqc.selected < NodeCount() ? NodeAt(AreaOf(h), g_eqc.selected) : POINT{S(40), S(10)};
            } else ScreenToClient(h, &pt);
            FilterMenu(h, pt);
            return 0;
        }
    }
    return DefWindowProcW(h, msg, wp, lp);
}
}  // namespace

// Layout from the top: page buttons, the enable row with presets, the curve, the band sliders (one
// narrow column each, a wider gap after the preamp, values above and frequency names below), then
// the mode and profile controls. The preset combo gets a tall drop-down rect so its list is not clipped.
void EqLayout(HWND h) {
    RECT rc;
    GetClientRect(h, &rc);
    int pad = S(12), header = S(50);
    MoveWindow(g_eqc.cordinalPage, pad, pad, S(180), S(28), TRUE);
    MoveWindow(g_eqc.headphonePage, pad + S(188), pad, S(180), S(28), TRUE);
    MoveWindow(g_eqc.pageHint, pad + S(382), pad + S(5), rc.right - pad * 2 - S(382), S(22), TRUE);
    if (g.headphonesWnd && GetParent(g.headphonesWnd) == h)
        MoveWindow(g.headphonesWnd, (rc.right - S(690)) / 2, header, S(690), rc.bottom - header, TRUE);
    const int row = header + pad;
    MoveWindow(g_eqc.enable, pad, row + S(2), S(18), S(20), TRUE);
    MoveWindow(g_eqc.label, pad + S(22), row + S(4), S(160), S(18), TRUE);
    MoveWindow(g_eqc.reset, rc.right - pad - S(70), row, S(70), S(26), TRUE);
    MoveWindow(g_eqc.preset, rc.right - pad - S(70) - S(8) - S(150), row + S(1), S(150), S(300), TRUE);
    MoveWindow(g_eqc.autoPreamp, rc.right - pad - S(70) - S(8) - S(150) - S(8) - S(110), row, S(110), S(26), TRUE);
    const int graphTop = row + S(36), graphHeight = S(150);
    MoveWindow(g_eqc.graph, pad, graphTop, rc.right - pad * 2, graphHeight, TRUE);
    int top = graphTop + graphHeight + S(8), colW = (rc.right - pad * 2) / (kEqBands + 2);
    int bottom = rc.bottom - S(178);
    for (int i = 0; i <= kEqBands; ++i) {
        int x = pad + (i == 0 ? 0 : (i + 1) * colW);
        MoveWindow(g_eqc.values[i], x, top, colW, S(18), TRUE);
        MoveWindow(g_eqc.sliders[i], x + (colW - S(30)) / 2, top + S(20), S(30), std::max(S(40), bottom - top - S(48)), TRUE);
        MoveWindow(g_eqc.names[i], x, bottom - S(26), colW, S(18), TRUE);
    }
    MoveWindow(g_eqc.parametric, pad, bottom + S(6), S(150), S(24), TRUE);
    MoveWindow(g_eqc.crossfeed, pad + S(160), bottom + S(6), S(200), S(24), TRUE);
    MoveWindow(g_eqc.apply, rc.right - pad - S(125), bottom + S(4), S(125), S(28), TRUE);
    MoveWindow(g_eqc.import, rc.right - pad - S(125) - S(8) - S(140), bottom + S(4), S(140), S(28), TRUE);
    MoveWindow(g_eqc.hint, pad, bottom + S(40), rc.right - 2 * pad, S(48), TRUE);
    MoveWindow(g_eqc.text, pad, bottom + S(92), rc.right - 2 * pad, rc.bottom - pad - bottom - S(92), TRUE);
}

LRESULT CALLBACK EqProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
                HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, h, (HMENU)(INT_PTR)id,
                                         g.inst, nullptr);
                SendMessageW(c, WM_SETFONT, (WPARAM)g.font, TRUE);
                return c;
            };
            // Registered here rather than at startup, so every way of creating the window has it.
            WNDCLASSW graphClass{};
            graphClass.style = CS_DBLCLKS;
            graphClass.hInstance = g.inst;
            graphClass.lpszClassName = kEqGraphClass;
            graphClass.lpfnWndProc = GraphProc;
            graphClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            RegisterClassW(&graphClass);
            g_eqc.headphones = false;
            g_eqc.selected = -1;
            g_eqc.dragging = false;
            g_eqc.cordinalPage = make(WC_BUTTONW, L"cordinal EQ", BS_AUTORADIOBUTTON | BS_PUSHLIKE | WS_GROUP | WS_TABSTOP,
                                      IDC_EQ_CORDINAL_PAGE);
            g_eqc.headphonePage = make(WC_BUTTONW, L"Headphone EQ", BS_AUTORADIOBUTTON | BS_PUSHLIKE | WS_TABSTOP,
                                       IDC_EQ_HEADPHONE_PAGE);
            g_eqc.pageHint = make(WC_STATICW, L"Switch panels: Ctrl+Tab", SS_LEFT, IDC_EQ_PAGE_HINT);
            Button_SetCheck(g_eqc.cordinalPage, BST_CHECKED);
            g_eqc.enable = make(WC_BUTTONW, L"", BS_AUTOCHECKBOX | WS_GROUP | WS_TABSTOP, IDC_EQ_ENABLE);
            g_eqc.label = make(WC_STATICW, L"Enable equalizer", SS_LEFT | SS_NOTIFY, IDC_EQ_LABEL);
            g_eqc.autoPreamp = make(WC_BUTTONW, L"Auto preamp", BS_PUSHBUTTON | WS_TABSTOP, IDC_EQ_AUTO_PREAMP);
            g_eqc.preset = make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_EQ_PRESET);
            // Item 0 is a "Presets..." placeholder so the closed combo shows a label, not a preset;
            // picking a real entry applies it and snaps the selection back to 0.
            SendMessageW(g_eqc.preset, CB_ADDSTRING, 0, (LPARAM)L"Presets...");
            for (auto& p : EqPresets()) SendMessageW(g_eqc.preset, CB_ADDSTRING, 0, (LPARAM)p.name);
            for (const PackPreset* p : PackPresets()) SendMessageW(g_eqc.preset, CB_ADDSTRING, 0, (LPARAM)p->name.c_str());
            SendMessageW(g_eqc.preset, CB_SETCURSEL, 0, 0);
            g_eqc.reset = make(WC_BUTTONW, L"Reset", BS_PUSHBUTTON | WS_TABSTOP, IDC_EQ_RESET);
            g_eqc.graph = make(kEqGraphClass, L"EQ curve", WS_TABSTOP, IDC_EQ_GRAPH);
            g_eqc.parametric = make(WC_BUTTONW, L"Parametric EQ", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_EQ_PARAMETRIC);
            g_eqc.crossfeed = make(WC_BUTTONW, L"Headphone crossfeed", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_EQ_CROSSFEED);
            g_eqc.import = make(WC_BUTTONW, L"Import AutoEq...", BS_PUSHBUTTON | WS_TABSTOP, IDC_EQ_IMPORT);
            g_eqc.apply = make(WC_BUTTONW, L"Apply profile", BS_PUSHBUTTON | WS_TABSTOP, IDC_EQ_APPLY);
            g_eqc.hint = make(WC_STATICW, L"", SS_LEFT, IDC_EQ_HINT);
            g_eqc.text = make(WC_EDITW, L"", WS_BORDER | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOVSCROLL |
                             ES_AUTOHSCROLL | ES_WANTRETURN | WS_TABSTOP, IDC_EQ_TEXT);
            SendMessageW(g_eqc.text, EM_SETLIMITTEXT, 65536, 0);
            for (int i = 0; i <= kEqBands; ++i) {
                g_eqc.sliders[i] = make(TRACKBAR_CLASSW, L"", TBS_VERT | TBS_BOTH | TBS_AUTOTICKS | WS_TABSTOP, IDC_EQ_SLIDER0 + i);
                SendMessageW(g_eqc.sliders[i], TBM_SETRANGE, FALSE, MAKELPARAM(0, i ? 48 : 168));
                SendMessageW(g_eqc.sliders[i], TBM_SETTICFREQ, 6, 0);
                SendMessageW(g_eqc.sliders[i], TBM_SETPAGESIZE, 0, 2);
                wchar_t name[16];
                if (i == 0) wcscpy_s(name, L"Preamp");
                else if (kEqFrequencies[i - 1] >= 1000) swprintf_s(name, L"%gk", kEqFrequencies[i - 1] / 1000);
                else swprintf_s(name, L"%g", kEqFrequencies[i - 1]);
                g_eqc.names[i] = make(WC_STATICW, name, SS_CENTER, 0);
                g_eqc.values[i] = make(WC_STATICW, L"", SS_CENTER, 0);
            }
            ApplyWindowTheme(h, P());
            ThemeEqControls();
            EqSync();
            ProfileToEditor();
            return 0;
        }
        case WM_SIZE:
            EqLayout(h);
            return 0;
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(h, &rc);
            FillRect((HDC)wp, &rc, g.surfaceBrush);
            return 1;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            HDC dc = (HDC)wp;
            SetTextColor(dc, TextColor());
            SetBkColor(dc, SurfaceColor());
            return (LRESULT)g.surfaceBrush;
        }
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLOREDIT:
            return FieldControlColor((HDC)wp);
        case WM_VSCROLL: {
            // Band sliders span +/-12 dB; the preamp spans +24..-60 dB for headphone profiles.
            // Touching any slider enables the EQ; saving waits for TB_ENDTRACK to avoid a write
            // per tick.
            HWND src = (HWND)lp;
            for (int i = 0; i <= kEqBands; ++i)
                if (src == g_eqc.sliders[i]) {
                    int pos = (int)SendMessageW(src, TBM_GETPOS, 0, 0);
                    *EqValue(i) = (i ? 12.0f : 24.0f) - pos / 2.0f;
                    if (i) g_eqc.selected = i - 1;
                    g.eq.enabled = true;
                    EqApply();
                    if (i == 0) ProfileToEditor();
                    if (LOWORD(wp) == TB_ENDTRACK) SaveEq();
                }
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDC_EQ_CORDINAL_PAGE:
                case IDC_EQ_HEADPHONE_PAGE:
                    SelectEqPage(LOWORD(wp) == IDC_EQ_HEADPHONE_PAGE);
                    break;
                case IDCANCEL:
                    DestroyWindow(h);
                    break;
                case IDC_EQ_IMPORT:
                    ImportAutoEq();
                    break;
                case IDC_EQ_APPLY: {
                    int length = GetWindowTextLengthW(g_eqc.text);
                    std::wstring text(length + 1, L'\0');
                    GetWindowTextW(g_eqc.text, text.data(), length + 1);
                    text.resize(length);
                    ApplyProfile(ToUtf8(text));
                    break;
                }
                case IDC_EQ_AUTO_PREAMP:
                    AutoPreamp();
                    break;
                case IDC_EQ_PARAMETRIC:
                    g.eq.parametric = Button_GetCheck(g_eqc.parametric) == BST_CHECKED;
                    g_eqc.selected = -1;
                    if (g.eq.parametric && !g.eq.filterCount) {
                        // Start from the graphic curve as the filters it already runs as, so switching
                        // keeps the sound; a flat curve starts with one flat filter to drag.
                        const auto gains = GraphicFilterGains(g.eq.gains, EqRate());
                        for (int i = 0; i < kEqBands; ++i)
                            if (g.eq.enabled && std::fabs(gains[i]) >= 0.01f)
                                g.eq.filters[g.eq.filterCount++] = {EqFilterType::Peak, kEqFrequencies[i],
                                                                    (float)RoundTo(gains[i], 0.01), kGraphicQ, true};
                        if (!g.eq.filterCount) {
                            g.eq.filters[0] = {};
                            g.eq.filterCount = 1;
                        }
                        ProfileToEditor();
                    }
                    EqApply();
                    SaveEq();
                    break;
                case IDC_EQ_CROSSFEED:
                    g.eq.crossfeed = Button_GetCheck(g_eqc.crossfeed) == BST_CHECKED;
                    EqApply();
                    SaveEq();
                    break;
                case IDC_EQ_ENABLE:
                    g.eq.enabled = Button_GetCheck(g_eqc.enable) == BST_CHECKED;
                    EqApply();
                    SaveEq();
                    break;
                case IDC_EQ_LABEL:
                    if (HIWORD(wp) == STN_CLICKED) {
                        g.eq.enabled = !g.eq.enabled;
                        EqApply();
                        SaveEq();
                    }
                    break;
                case IDC_EQ_RESET:
                    g.eq.preamp = 0;
                    g.eq.gains.fill(0);
                    g.eq.parametric = false;
                    g.eq.filterCount = 0;
                    g_eqc.selected = -1;
                    EqApply();
                    ProfileToEditor();
                    SaveEq();
                    break;
                case IDC_EQ_PRESET:
                    // Combo index 1..N names a built-in preset, then the packs' presets follow; after
                    // applying, the selection resets to the "Presets..." placeholder.
                    if (HIWORD(wp) == CBN_SELCHANGE) {
                        int sel = (int)SendMessageW(g_eqc.preset, CB_GETCURSEL, 0, 0), builtIn = (int)EqPresets().size();
                        auto packs = PackPresets();
                        if (sel > 0 && sel <= builtIn + (int)packs.size()) {
                            static_assert(std::tuple_size_v<decltype(PackPreset::gains)> == kEqBands);
                            g.eq.gains = sel <= builtIn ? EqPresets()[sel - 1].gains : packs[sel - 1 - builtIn]->gains;
                            g.eq.parametric = false;
                            g.eq.enabled = true;
                            EqApply();
                            ProfileToEditor();
                            SaveEq();
                        }
                        SendMessageW(g_eqc.preset, CB_SETCURSEL, 0, 0);
                    }
                    break;
            }
            return 0;
        case WM_NOTIFY: {
            auto* hdr = reinterpret_cast<NMHDR*>(lp);
            if (hdr->code == NM_CUSTOMDRAW) return DrawTrackbar(reinterpret_cast<NMCUSTOMDRAW*>(lp));
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) DestroyWindow(h);
            return 0;
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            SaveEq();
            g.eqWnd = nullptr;
            g_eqc.graph = nullptr;
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void ThemeEqControls() {
    for (HWND c : {g_eqc.enable, g_eqc.reset, g_eqc.autoPreamp, g_eqc.parametric, g_eqc.crossfeed, g_eqc.import,
                  g_eqc.apply, g_eqc.text, g_eqc.cordinalPage, g_eqc.headphonePage})
        ApplyControlTheme(c, g.dark);
    SetWindowTheme(g_eqc.preset, g.dark ? L"DarkMode_CFD" : L"Explorer", nullptr);
    if (g_eqc.graph) InvalidateRect(g_eqc.graph, nullptr, FALSE);
}

// Creates the equalizer window if it does not exist (single instance via g.eqWnd), centered over the
// main window; otherwise brings an existing one forward.
void ShowEqualizer() {
    if (g.eqWnd) {
        SetForegroundWindow(g.eqWnd);
        return;
    }
    RECT r{0, 0, S(740), S(640)};
    AdjustWindowRectExForDpi(&r, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_TOOLWINDOW, g.dpi);
    RECT owner;
    GetWindowRect(g.hwnd, &owner);
    int w = r.right - r.left, h = r.bottom - r.top;
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(g.hwnd, MONITOR_DEFAULTTONEAREST), &monitor);
    int x = std::max((int)monitor.rcWork.left, std::min((int)(owner.left + (owner.right - owner.left - w) / 2),
                                                     (int)monitor.rcWork.right - w));
    int y = std::max((int)monitor.rcWork.top, std::min((int)owner.top + S(60), (int)monitor.rcWork.bottom - h));
    g.eqWnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_CONTROLPARENT, kEqClass, L"Equalizer",
                              WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_CLIPCHILDREN,
                              x, y, w, h, g.hwnd, nullptr,
                              g.inst, nullptr);
    ShowWindow(g.eqWnd, SW_SHOW);
    EqDeviceSync(false);
}

}  // namespace app
