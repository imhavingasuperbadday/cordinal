// Big mode: the playback strip pinned across the middle of the window, and the panels (Library,
// Now playing, Up next) in tab groups docked in the area above it and the area below it.
//
// Each area is a tree: a split divides its rectangle between two children, side by side or one
// above the other, by a ratio its splitter bar sets (drag it). A leaf is a tab group: a row of
// tabs over the active panel. Each tab has a close box (or middle-click it); View > Big mode
// panels opens a closed panel again. Drag a tab onto another group: onto its tabs or middle to
// join it, or near an edge to split that side off; onto an empty area to fill it. A translucent
// box shows where it will land. The strip never moves: the areas can only rearrange around it.
// The layout is saved as a short string, e.g. "g0:0;|h500g0:1;g0:2;" (see Write and Read).
#include "app.h"

namespace app {

namespace {

const wchar_t* const kPanelNames[] = {L"Library", L"Now playing", L"Up next"};
const wchar_t kDropClass[] = L"CordinalDropZone";

struct Node {
    std::unique_ptr<Node> a, b;  // a split's halves: a is left of (or above) b
    bool vertical = false;
    float ratio = 0.5f;          // a's share
    std::vector<int> tabs;       // a group's panels
    int active = 0;
    RECT rc{}, header{}, bar{};  // laid out: the whole, a group's tab row, a split's splitter
    bool Split() const { return a != nullptr; }
};

enum Zone { kCenter, kLeft, kTop, kRight, kBottom };

struct Big {
    std::unique_ptr<Node> area[2];  // above and below the strip; either may be empty
    RECT areaRc[2]{};
    // Mouse: a pressed tab (maybe being dragged), a splitter being dragged, the hovered close box.
    Node* pressGroup = nullptr;
    int pressPanel = -1;
    POINT pressAt{};
    bool dragging = false, onClose = false;
    Node* splitting = nullptr;
    Node* hotGroup = nullptr;
    int hotTab = -1;
    bool hotClose = false;
    HWND drop = nullptr;  // the translucent box over the drop zone
    bool dropOk = false;
    int dropArea = 0;
    Node* dropGroup = nullptr;  // null: the empty area dropArea
    Zone dropZone = kCenter;
} b;

std::unique_ptr<Node> Group(std::initializer_list<int> tabs) {
    auto n = std::make_unique<Node>();
    n->tabs = tabs;
    return n;
}

std::unique_ptr<Node> SplitOf(bool vertical, float ratio, std::unique_ptr<Node> a, std::unique_ptr<Node> bb) {
    auto n = std::make_unique<Node>();
    n->vertical = vertical;
    n->ratio = ratio;
    n->a = std::move(a);
    n->b = std::move(bb);
    return n;
}

// The Library above the strip; Now playing and Up next side by side under it.
void DefaultLayout() {
    b.area[0] = Group({kPanelLibrary});
    b.area[1] = SplitOf(false, 0.5f, Group({kPanelNowPlaying}), Group({kPanelUpNext}));
}

// ---------------------------------------------------------------------------
// The layout as text: the area above, "|", the area below ("-" when empty). A split is "v" or
// "h" and its ratio in thousandths, then its two halves; a group is "g", the active tab, ":",
// the panels separated by ".", then ";".

void Write(const Node* n, std::wstring& out) {
    if (!n) {
        out += L'-';
    } else if (n->Split()) {
        out += n->vertical ? L'v' : L'h';
        out += std::to_wstring(std::lround(n->ratio * 1000));
        Write(n->a.get(), out);
        Write(n->b.get(), out);
    } else {
        out += L'g' + std::to_wstring(n->active) + L':';
        for (size_t i = 0; i < n->tabs.size(); ++i) out += (i ? L"." : L"") + std::to_wstring(n->tabs[i]);
        out += L';';
    }
}

// Parses the layout text; on any malformed input returns nullptr with ok=false and the caller falls
// back to the default layout. `seen` is a bitmask of panels already used: a panel may appear only
// once across the whole tree. Depth-capped so a corrupt file can't recurse without bound.
std::unique_ptr<Node> Read(const wchar_t*& s, int depth, int& seen, bool& ok) {
    ok = false;
    if (depth > 12) return nullptr;
    wchar_t* end = nullptr;
    if (depth == 0 && *s == L'-') {
        ++s;
        ok = true;
        return nullptr;
    }
    if (*s == L'v' || *s == L'h') {
        bool vertical = *s++ == L'v';
        float ratio = std::clamp((float)wcstol(s, &end, 10) / 1000, 0.05f, 0.95f);
        s = end;
        auto x = Read(s, depth + 1, seen, ok);
        auto y = ok ? Read(s, depth + 1, seen, ok) : nullptr;
        return ok ? SplitOf(vertical, ratio, std::move(x), std::move(y)) : nullptr;
    }
    if (*s++ != L'g') return nullptr;
    auto n = std::make_unique<Node>();
    n->active = (int)wcstol(s, &end, 10);
    if (end == s || *end != L':') return nullptr;
    for (s = end + 1; *s != L';'; s += *s == L'.') {
        int id = (int)wcstol(s, &end, 10);
        if (end == s || id < 0 || id >= kPanelCount || (seen & (1 << id))) return nullptr;
        seen |= 1 << id;
        n->tabs.push_back(id);
        s = end;
    }
    ++s;
    if (n->tabs.empty()) return nullptr;
    n->active = std::clamp(n->active, 0, (int)n->tabs.size() - 1);
    ok = true;
    return n;
}

void Save() {
    std::wstring s;
    Write(b.area[0].get(), s);
    s += L'|';
    Write(b.area[1].get(), s);
    IniWrite(L"Window", L"BigLayout", s);
}

// ---------------------------------------------------------------------------
// Tree edits

template <class F>
void ForEachGroup(Node* n, F&& f) {
    if (!n) return;
    if (n->Split()) {
        ForEachGroup(n->a.get(), f);
        ForEachGroup(n->b.get(), f);
    } else {
        f(*n);
    }
}

Node* GroupOf(int panel) {
    Node* found = nullptr;
    for (auto& area : b.area)
        ForEachGroup(area.get(), [&](Node& n) {
            if (std::find(n.tabs.begin(), n.tabs.end(), panel) != n.tabs.end()) found = &n;
        });
    return found;
}

// Takes `panel` out of a tree: an emptied group goes, and its split gives way to the other half.
bool Remove(std::unique_ptr<Node>& n, int panel) {
    if (!n) return false;
    if (n->Split()) {
        if (!Remove(n->a, panel) && !Remove(n->b, panel)) return false;
        if (!n->a) n = std::move(n->b);
        else if (!n->b) n = std::move(n->a);
        return true;
    }
    auto it = std::find(n->tabs.begin(), n->tabs.end(), panel);
    if (it == n->tabs.end()) return false;
    int i = (int)(it - n->tabs.begin());
    n->tabs.erase(it);
    if (n->active > i || n->active >= (int)n->tabs.size()) n->active = std::max(0, n->active - 1);
    if (n->tabs.empty()) n.reset();
    return true;
}

void RemovePanel(int panel) {
    for (auto& area : b.area) Remove(area, panel);
}

// Puts `panel` into the group `target`: as its last tab, or split off on one side. To split, the
// target node is reused in place as the new split (its old group moves into a child) so hot
// tracking and the pending drop keep pointing at the same node.
void Insert(Node* target, Zone zone, int panel) {
    if (zone == kCenter) {
        target->tabs.push_back(panel);
        target->active = (int)target->tabs.size() - 1;
        return;
    }
    auto old = std::make_unique<Node>();
    old->tabs = std::move(target->tabs);
    old->active = target->active;
    auto fresh = Group({panel});
    bool first = zone == kLeft || zone == kTop;
    target->tabs.clear();
    target->vertical = zone == kTop || zone == kBottom;
    target->ratio = 0.5f;
    target->a = first ? std::move(fresh) : std::move(old);
    target->b = first ? std::move(old) : std::move(fresh);
}

void Changed() {
    Save();
    Layout();
}

// ---------------------------------------------------------------------------
// Layout and painting

int HeaderHeight() { return S(30); }

// Computes each node's rectangles into `r`. A leaf writes its active panel's body into
// bodies[panelId]; an unplaced panel keeps an empty rect, which LayoutBig reads as "hidden".
void LayoutNode(Node& n, RECT r, RECT* bodies) {
    n.rc = r;
    n.header = n.bar = {};
    if (n.Split()) {
        int gap = S(6), total = (n.vertical ? r.bottom - r.top : r.right - r.left) - gap;
        int first = std::clamp((int)(total * n.ratio), 0, std::max(0, total));
        RECT ra = r, rb = r;
        if (n.vertical) {
            ra.bottom = r.top + first;
            rb.top = ra.bottom + gap;
            n.bar = {r.left, ra.bottom, r.right, rb.top};
        } else {
            ra.right = r.left + first;
            rb.left = ra.right + gap;
            n.bar = {ra.right, r.top, rb.left, r.bottom};
        }
        LayoutNode(*n.a, ra, bodies);
        LayoutNode(*n.b, rb, bodies);
        return;
    }
    n.header = {r.left, r.top, r.right, std::min(r.bottom, r.top + HeaderHeight())};
    n.active = std::clamp(n.active, 0, (int)n.tabs.size() - 1);
    bodies[n.tabs[n.active]] = {r.left, n.header.bottom, r.right, r.bottom};
}

// The tabs of a group, left to right; the close box is the right end of each.
std::vector<RECT> TabRects(const Node& n) {
    std::vector<RECT> rects;
    HDC dc = GetDC(g.hwnd);
    HGDIOBJ old = SelectObject(dc, g.font);
    int x = n.header.left + S(4);
    for (int panel : n.tabs) {
        SIZE sz{};
        GetTextExtentPoint32W(dc, kPanelNames[panel], (int)wcslen(kPanelNames[panel]), &sz);
        int w = sz.cx + S(46);
        rects.push_back({x, n.header.top + S(3), std::min(x + w, (int)n.header.right), n.header.bottom});
        x += w + S(2);
    }
    SelectObject(dc, old);
    ReleaseDC(g.hwnd, dc);
    return rects;
}

RECT CloseBox(const RECT& tab) { return {tab.right - S(26), tab.top + S(4), tab.right - S(6), tab.bottom - S(4)}; }

void Label(HDC dc, const wchar_t* s, RECT r, HFONT font, COLORREF color, UINT align) {
    HGDIOBJ old = SelectObject(dc, font);
    SetTextColor(dc, color);
    DrawTextW(dc, s, -1, &r, align | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, old);
}

void PaintNode(HDC dc, Node& n) {
    if (n.Split()) {
        PaintNode(dc, *n.a);
        PaintNode(dc, *n.b);
        // The splitter: a thin line in the middle of its gap.
        RECT line = n.bar;
        if (n.vertical) {
            line.top = (n.bar.top + n.bar.bottom) / 2;
            line.bottom = line.top + 1;
        } else {
            line.left = (n.bar.left + n.bar.right) / 2;
            line.right = line.left + 1;
        }
        FillSolid(dc, line, Ref(P().border));
        return;
    }
    FillSolid(dc, {n.header.left, n.header.bottom - 1, n.header.right, n.header.bottom}, Ref(P().border));
    auto rects = TabRects(n);
    SetBkMode(dc, TRANSPARENT);
    for (size_t i = 0; i < n.tabs.size(); ++i) {
        RECT t = rects[i];
        bool active = (int)i == n.active, hot = b.hotGroup == &n && b.hotTab == (int)i;
        if (active || hot) FillSolid(dc, t, active ? WindowBg() : Ref(P().hover));
        if (active) FillSolid(dc, {t.left, t.top, t.right, t.top + S(2)}, Ref(P().accent));
        RECT label{t.left + S(12), t.top, t.right - S(28), t.bottom};
        Label(dc, kPanelNames[n.tabs[i]], label, active ? g.fontBold : g.font, active ? TextColor() : SubColor(), DT_LEFT);
        RECT x = CloseBox(t);
        if (hot && b.hotClose) FillSolid(dc, x, Ref(P().select));
        if (active || hot) Label(dc, L"\xE8BB", x, g.fontIcons, SubColor(), DT_CENTER);
    }
}

// ---------------------------------------------------------------------------
// Mouse

Node* GroupAt(Node* n, POINT pt) {
    if (!n || !PtInRect(&n->rc, pt)) return nullptr;
    if (!n->Split()) return n;
    if (Node* x = GroupAt(n->a.get(), pt)) return x;
    return GroupAt(n->b.get(), pt);
}

Node* GroupAt(POINT pt) {
    for (auto& area : b.area)
        if (Node* n = GroupAt(area.get(), pt)) return n;
    return nullptr;
}

Node* SplitterAt(Node* n, POINT pt) {
    if (!n || !n->Split()) return nullptr;
    if (PtInRect(&n->bar, pt)) return n;
    if (Node* x = SplitterAt(n->a.get(), pt)) return x;
    return SplitterAt(n->b.get(), pt);
}

Node* SplitterAt(POINT pt) {
    for (auto& area : b.area)
        if (Node* n = SplitterAt(area.get(), pt)) return n;
    return nullptr;
}

int TabAt(Node& n, POINT pt, bool& close) {
    close = false;
    if (n.Split() || !PtInRect(&n.header, pt)) return -1;
    auto rects = TabRects(n);
    for (size_t i = 0; i < rects.size(); ++i)
        if (PtInRect(&rects[i], pt)) {
            RECT x = CloseBox(rects[i]);
            close = PtInRect(&x, pt) != 0;
            return (int)i;
        }
    return -1;
}

LRESULT CALLBACK DropProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) {
        RECT rc;
        GetClientRect(h, &rc);
        FillSolid((HDC)wp, rc, Ref(P().accent));
        return 1;
    }
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(h, msg, wp, lp);
}

// Where a dragged tab would land under `pt`: a group and a side of it (or its middle), or an
// empty area.
bool DropTarget(POINT pt, RECT& box) {
    b.dropGroup = nullptr;
    b.dropZone = kCenter;
    for (int i = 0; i < 2; ++i)
        if (!b.area[i] && PtInRect(&b.areaRc[i], pt)) {
            b.dropArea = i;
            box = b.areaRc[i];
            return true;
        }
    Node* group = GroupAt(pt);
    if (!group) return false;
    RECT r = group->rc;
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return false;
    double fx = (double)(pt.x - r.left) / w, fy = (double)(pt.y - r.top) / h;
    double edge = std::min({fx, 1 - fx, fy, 1 - fy});
    Zone zone = PtInRect(&group->header, pt) || edge > 0.28 ? kCenter
                : edge == fx                                ? kLeft
                : edge == 1 - fx                            ? kRight
                : edge == fy                                ? kTop
                                                            : kBottom;
    // Nothing to do: dropping a group's only tab back onto that group.
    if (group == b.pressGroup && (zone == kCenter || group->tabs.size() == 1)) return false;
    b.dropGroup = group;
    b.dropZone = zone;
    box = r;
    if (zone == kLeft) box.right = r.left + w / 2;
    else if (zone == kRight) box.left = r.right - w / 2;
    else if (zone == kTop) box.bottom = r.top + std::max(h / 2, HeaderHeight());
    else if (zone == kBottom) box.top = r.bottom - std::max(h / 2, HeaderHeight());
    return true;
}

void ShowDropBox(const RECT* box) {
    if (!box) {
        if (b.drop) ShowWindow(b.drop, SW_HIDE);
        return;
    }
    if (!b.drop) {
        RegisterAppClass(kDropClass, DropProc, 0, false);
        b.drop = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kDropClass, L"",
                                 WS_POPUP, 0, 0, 0, 0, g.hwnd, nullptr, g.inst, nullptr);
        SetLayeredWindowAttributes(b.drop, 0, 70, LWA_ALPHA);
    }
    RECT r = *box;
    MapWindowPoints(g.hwnd, nullptr, reinterpret_cast<POINT*>(&r), 2);
    SetWindowPos(b.drop, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(b.drop, nullptr, TRUE);
}

void EndPress() {
    b.pressGroup = nullptr;
    b.pressPanel = -1;
    b.dragging = b.dropOk = false;
    b.splitting = nullptr;
    ShowDropBox(nullptr);
}

void SetHot(Node* group, int tab, bool close) {
    if (group == b.hotGroup && tab == b.hotTab && close == b.hotClose) return;
    for (Node* n : {b.hotGroup, group})
        if (n) InvalidateRect(g.hwnd, &n->header, FALSE);
    b.hotGroup = group;
    b.hotTab = tab;
    b.hotClose = close;
    if (group) {
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, g.hwnd, 0};
        TrackMouseEvent(&tme);
    }
}

}  // namespace

void LoadPanels() {
    std::wstring s = IniRead(L"Window", L"BigLayout", L"");
    const wchar_t* p = s.c_str();
    int seen = 0;
    bool ok = false, ok2 = false;
    auto top = Read(p, 0, seen, ok);
    auto bottom = ok && *p++ == L'|' ? Read(p, 0, seen, ok2) : nullptr;
    if (ok2 && !*p) {
        b.area[0] = std::move(top);
        b.area[1] = std::move(bottom);
    } else {
        DefaultLayout();
    }
}

void LayoutBig(HDWP& dwp, RECT r) {
    // The strip in the middle; the areas take what's above and below it.
    int stripH = StripHeight(), mid = r.top + std::max(0, (int)(r.bottom - r.top) - stripH) / 2;
    LayoutStrip(dwp, {r.left, mid, r.right, mid + stripH});
    b.areaRc[0] = {r.left, r.top, r.right, mid};
    b.areaRc[1] = {r.left, mid + stripH, r.right, r.bottom};
    RECT bodies[kPanelCount] = {};
    for (int i = 0; i < 2; ++i)
        if (b.area[i]) LayoutNode(*b.area[i], b.areaRc[i], bodies);
    LayoutLibrary(dwp, bodies[kPanelLibrary], !IsRectEmpty(&bodies[kPanelLibrary]));
    HWND panels[] = {nullptr, NowPlayingPanel(), UpNextPanel()};
    for (int i = kPanelNowPlaying; i < kPanelCount; ++i) {
        const RECT& body = bodies[i];
        bool shown = !IsRectEmpty(&body);
        if (shown) Place(dwp, panels[i], body.left, body.top, body.right - body.left, body.bottom - body.top);
        ShowWindow(panels[i], shown ? SW_SHOWNA : SW_HIDE);
    }
    PanelsTick();
}

void PaintBig(HDC dc) {
    for (int i = 0; i < 2; ++i) {
        if (b.area[i]) {
            PaintNode(dc, *b.area[i]);
        } else if (!IsRectEmpty(&b.areaRc[i])) {
            SetBkMode(dc, TRANSPARENT);
            Label(dc, L"Drag a tab here, or open a panel from the right-click menu", b.areaRc[i], g.font, Ref(P().faint),
                  DT_CENTER);
        }
    }
}

bool BigSetCursor() {
    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(g.hwnd, &pt);
    Node* s = b.splitting ? b.splitting : SplitterAt(pt);
    if (!s) return false;
    SetCursor(LoadCursorW(nullptr, s->vertical ? IDC_SIZENS : IDC_SIZEWE));
    return true;
}

bool BigMouse(UINT msg, WPARAM, LPARAM lp) {
    POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    switch (msg) {
        case WM_LBUTTONDOWN:
        case WM_MBUTTONDOWN: {
            if (msg == WM_LBUTTONDOWN && (b.splitting = SplitterAt(pt))) {
                SetCapture(g.hwnd);
                return true;
            }
            Node* group = GroupAt(pt);
            bool close = false;
            int tab = group ? TabAt(*group, pt, close) : -1;
            if (tab < 0) return false;
            b.pressGroup = group;
            b.pressPanel = group->tabs[tab];
            b.pressAt = pt;
            b.onClose = close || msg == WM_MBUTTONDOWN;
            if (!b.onClose && group->active != tab) {
                group->active = tab;
                Changed();
            }
            SetCapture(g.hwnd);
            return true;
        }
        case WM_MOUSEMOVE: {
            if (b.splitting) {
                Node& n = *b.splitting;
                int gap = S(6);
                double total = (n.vertical ? n.rc.bottom - n.rc.top : n.rc.right - n.rc.left) - gap;
                double at = (n.vertical ? pt.y - n.rc.top : pt.x - n.rc.left) - gap / 2.0;
                // Leave a margin so neither half can be dragged to nothing.
                double margin = std::min(0.45, S(90) / std::max(1.0, total));
                n.ratio = (float)std::clamp(at / std::max(1.0, total), margin, 1 - margin);
                Layout();
                UpdateWindow(g.hwnd);
                return true;
            }
            if (b.pressGroup && !b.onClose) {
                if (!b.dragging && (std::abs(pt.x - b.pressAt.x) > GetSystemMetrics(SM_CXDRAG) ||
                                    std::abs(pt.y - b.pressAt.y) > GetSystemMetrics(SM_CYDRAG)))
                    b.dragging = true;
                if (b.dragging) {
                    RECT box;
                    b.dropOk = DropTarget(pt, box);
                    ShowDropBox(b.dropOk ? &box : nullptr);
                    SetCursor(LoadCursorW(nullptr, b.dropOk ? IDC_SIZEALL : IDC_NO));
                }
                return true;
            }
            Node* group = GroupAt(pt);
            bool close = false;
            int tab = group ? TabAt(*group, pt, close) : -1;
            SetHot(tab >= 0 ? group : nullptr, tab, close);
            return false;
        }
        case WM_MOUSELEAVE:
            SetHot(nullptr, -1, false);
            return false;
        case WM_LBUTTONUP:
        case WM_MBUTTONUP: {
            if (b.splitting) {
                EndPress();
                ReleaseCapture();
                Save();
                return true;
            }
            if (!b.pressGroup) return false;
            int panel = b.pressPanel, area = b.dropArea;
            bool dragging = b.dragging, onClose = b.onClose, dropOk = b.dropOk;
            Node *from = b.pressGroup, *target = b.dropGroup;
            Zone zone = b.dropZone;
            EndPress();
            ReleaseCapture();
            if (dragging && dropOk) {
                // Out of its group, then into the target: removing first may collapse splits, but
                // the target group itself (another node, or a group keeping other tabs) survives.
                RemovePanel(panel);
                if (target) Insert(target, zone, panel);
                else b.area[area] = Group({panel});
                Changed();
            } else if (!dragging && onClose) {
                bool close = false;
                Node* group = GroupAt(pt);
                int tab = group ? TabAt(*group, pt, close) : -1;
                if (group == from && tab >= 0 && group->tabs[tab] == panel && (close || msg == WM_MBUTTONUP)) {
                    RemovePanel(panel);
                    Changed();
                }
            }
            return true;
        }
        case WM_CAPTURECHANGED:
            if (b.pressGroup || b.splitting) EndPress();
            return false;
    }
    return false;
}

bool PanelOpen(int panel) { return GroupOf(panel) != nullptr; }

void TogglePanel(int panel) {
    if (GroupOf(panel)) {
        RemovePanel(panel);
    } else {
        // Into an empty area if there is one (below the strip first), else as a tab of the
        // first group below the strip (or above it).
        int empty = !b.area[1] ? 1 : !b.area[0] ? 0 : -1;
        if (empty >= 0) {
            b.area[empty] = Group({panel});
        } else {
            Node* first = nullptr;
            for (int i : {1, 0})
                ForEachGroup(b.area[i].get(), [&](Node& n) {
                    if (!first) first = &n;
                });
            Insert(first, kCenter, panel);
        }
        if (g.size != ViewSize::Big) SetViewSize(ViewSize::Big);
    }
    Changed();
}

void ResetPanels() {
    DefaultLayout();
    EndPress();
    Changed();
}

}  // namespace app
