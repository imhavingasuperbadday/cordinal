// Linux window sizes and Big mode's three panels. The middle playback strip stays fixed while
// Library, Now playing and Up next can be tabbed, split, closed and dragged between the two areas.
// No renderer state lives here: rectangles, a bounded layout tree and window placement survive
// renderer changes. All edits run on the UI thread and use the existing atomic settings writer.
#include "big_ui.h"
#include "ui_internal.h"
#include <charconv>
#include <cstdio>
#include <string_view>

namespace lx {
namespace {
constexpr const char* kNames[] = {"Library", "Now playing", "Up next"};
enum Zone { Center, Left, Top, Right, Bottom };
struct Node {
    std::unique_ptr<Node> a, b;
    bool vertical = false;
    float ratio = .5f;
    std::vector<int> tabs;
    int active = 0;
    Rect rect, header, splitter;
    bool Split() const { return a != nullptr; }
};
struct State {
    std::unique_ptr<Node> area[2];
    Rect areas[2], bodies[3], nowTitle, drop;
    Node *pressed = nullptr, *splitting = nullptr, *target = nullptr;
    int panel = -1, targetArea = -1, keyboardPanel = 0;
    float pressX = 0, pressY = 0;
    Zone zone = Center;
    bool close = false, dragging = false, dropValid = false, initialized = false, keyboardFocus = false;
} B;

struct Placement {
    int x = SDL_WINDOWPOS_UNDEFINED, y = SDL_WINDOWPOS_UNDEFINED, w = 440, h = 600;
    bool maximized = false, valid = false;
};
Placement full, compact;

std::unique_ptr<Node> MakePanelGroup(std::initializer_list<int> tabs) {
    auto n = std::make_unique<Node>();
    n->tabs = tabs;
    return n;
}
void Defaults() {
    B.area[0] = MakePanelGroup({0});
    B.area[1] = std::make_unique<Node>();
    B.area[1]->a = MakePanelGroup({1});
    B.area[1]->b = MakePanelGroup({2});
    B.initialized = true;
}
template<class F> void Groups(Node* node, F&& f) {
    if (!node) return;
    if (node->Split()) { Groups(node->a.get(), f); Groups(node->b.get(), f); }
    else f(*node);
}
Node* PanelGroup(int panel) {
    Node* found = nullptr;
    for (auto& area : B.area) Groups(area.get(), [&](Node& n) {
        if (std::find(n.tabs.begin(), n.tabs.end(), panel) != n.tabs.end()) found = &n;
    });
    return found;
}
void Write(const Node* n, std::string& text) {
    if (!n) { text += '-'; return; }
    if (n->Split()) {
        text += n->vertical ? 'v' : 'h';
        text += std::to_string((int)std::lround(n->ratio * 1000));
        Write(n->a.get(), text); Write(n->b.get(), text);
    } else {
        text += 'g' + std::to_string(n->active) + ':';
        for (size_t i = 0; i < n->tabs.size(); ++i) text += (i ? "." : "") + std::to_string(n->tabs[i]);
        text += ';';
    }
}
bool Number(std::string_view& text, int& out) {
    auto end = text.data() + text.size();
    auto result = std::from_chars(text.data(), end, out);
    if (result.ec != std::errc{} || result.ptr == text.data()) return false;
    text.remove_prefix(result.ptr - text.data());
    return true;
}
std::unique_ptr<Node> Read(std::string_view& text, int depth, unsigned& seen, bool& ok) {
    ok = false;
    if (depth > 12 || text.empty()) return nullptr;
    char kind = text.front(); text.remove_prefix(1);
    if (kind == '-' && depth == 0) { ok = true; return nullptr; }
    if (kind == 'v' || kind == 'h') {
        int ratio = 0;
        if (!Number(text, ratio)) return nullptr;
        auto n = std::make_unique<Node>();
        n->vertical = kind == 'v'; n->ratio = std::clamp(ratio / 1000.f, .05f, .95f);
        n->a = Read(text, depth + 1, seen, ok);
        if (!ok || !n->a) { ok = false; return nullptr; }
        n->b = Read(text, depth + 1, seen, ok);
        if (!ok || !n->b) { ok = false; return nullptr; }
        return n;
    }
    if (kind != 'g') return nullptr;
    auto n = std::make_unique<Node>();
    if (!Number(text, n->active) || text.empty() || text.front() != ':') return nullptr;
    text.remove_prefix(1);
    for (;;) {
        int panel = -1;
        if (!Number(text, panel) || panel < 0 || panel > 2 || (seen & (1u << panel))) return nullptr;
        seen |= 1u << panel;
        n->tabs.push_back(panel);
        if (text.empty()) return nullptr;
        char separator = text.front(); text.remove_prefix(1);
        if (separator == ';') break;
        if (separator != '.') return nullptr;
    }
    n->active = std::clamp(n->active, 0, (int)n->tabs.size() - 1);
    ok = true;
    return n;
}
void CancelPress() {
    B.pressed = B.splitting = B.target = nullptr;
    B.panel = -1; B.targetArea = -1;
    B.dragging = B.close = B.dropValid = false;
    B.drop = {};
}
void Changed() { SaveSettings(); g.redraw = true; }
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
    int index = (int)(it - n->tabs.begin());
    n->tabs.erase(it);
    if (n->active > index || n->active >= (int)n->tabs.size()) n->active = std::max(0, n->active - 1);
    if (n->tabs.empty()) n.reset();
    return true;
}
void RemovePanel(int panel) { for (auto& area : B.area) Remove(area, panel); }
void Insert(Node& target, Zone zone, int panel) {
    if (zone == Center) {
        target.tabs.push_back(panel); target.active = (int)target.tabs.size() - 1; return;
    }
    auto old = std::make_unique<Node>();
    old->tabs = std::move(target.tabs); old->active = target.active;
    auto fresh = MakePanelGroup({panel});
    bool first = zone == Left || zone == Top;
    target.tabs.clear(); target.active = 0;
    target.vertical = zone == Top || zone == Bottom; target.ratio = .5f;
    target.a = first ? std::move(fresh) : std::move(old);
    target.b = first ? std::move(old) : std::move(fresh);
}
Rect TabRect(const Node& n, int index) {
    float x = n.header.x;
    for (int i = 0; i < index; ++i) x += TextWidth(Font::Normal, kNames[n.tabs[i]]) + S(44);
    float w = TextWidth(Font::Normal, kNames[n.tabs[index]]) + S(44);
    return {x, n.header.y, std::max(0.f, std::min(w, n.header.R() - x)), n.header.h};
}
Rect CloseRect(Rect r) { return {r.R() - S(24), r.y + S(4), S(20), std::max(0.f, r.h - S(8))}; }
void LayoutNode(Node& n, Rect rect) {
    n.rect = rect; n.header = n.splitter = {};
    if (n.Split()) {
        float gap = std::min(S(6), n.vertical ? rect.h : rect.w);
        float total = std::max(0.f, (n.vertical ? rect.h : rect.w) - gap);
        float first = std::floor(total * n.ratio);
        Rect a = rect, b = rect;
        if (n.vertical) {
            a.h = first; b.y += first + gap; b.h = total - first;
            n.splitter = {rect.x, a.B(), rect.w, gap};
        } else {
            a.w = first; b.x += first + gap; b.w = total - first;
            n.splitter = {a.R(), rect.y, gap, rect.h};
        }
        LayoutNode(*n.a, a); LayoutNode(*n.b, b);
    } else {
        n.header = {rect.x, rect.y, rect.w, std::min(rect.h, S(30))};
        int panel = n.tabs[n.active];
        B.bodies[panel] = {rect.x, n.header.B(), rect.w, std::max(0.f, rect.h - n.header.h)};
    }
}
Node* GroupAt(Node* n, float x, float y) {
    if (!n || !n->rect.In(x, y)) return nullptr;
    if (!n->Split()) return n;
    if (auto group = GroupAt(n->a.get(), x, y)) return group;
    return GroupAt(n->b.get(), x, y);
}
Node* GroupAt(float x, float y) {
    for (auto& area : B.area) if (auto group = GroupAt(area.get(), x, y)) return group;
    return nullptr;
}
Node* ParentOf(Node* root, const Node* child) {
    if (!root || !root->Split()) return nullptr;
    if (root->a.get() == child || root->b.get() == child) return root;
    if (auto parent = ParentOf(root->a.get(), child)) return parent;
    return ParentOf(root->b.get(), child);
}
Node* ParentOf(const Node* child) {
    for (auto& root : B.area) if (auto parent = ParentOf(root.get(), child)) return parent;
    return nullptr;
}
Node* SplitterAt(Node* n, float x, float y) {
    if (!n || !n->Split()) return nullptr;
    if (n->splitter.In(x, y)) return n;
    if (auto split = SplitterAt(n->a.get(), x, y)) return split;
    return SplitterAt(n->b.get(), x, y);
}
Node* SplitterAt(float x, float y) {
    for (auto& area : B.area) if (auto split = SplitterAt(area.get(), x, y)) return split;
    return nullptr;
}
int TabAt(const Node& group, float x, float y) {
    if (!group.header.In(x, y)) return -1;
    for (int i = 0; i < (int)group.tabs.size(); ++i) if (TabRect(group, i).In(x, y)) return i;
    return -1;
}
bool FindDrop(float x, float y) {
    B.target = nullptr; B.targetArea = -1; B.zone = Center; B.drop = {};
    for (int i = 0; i < 2; ++i) if (!B.area[i] && B.areas[i].In(x, y)) {
        B.targetArea = i; B.drop = B.areas[i]; return true;
    }
    Node* group = GroupAt(x, y);
    if (!group || group->rect.w <= 0 || group->rect.h <= 0) return false;
    Rect r = group->rect;
    float fx = (x - r.x) / r.w, fy = (y - r.y) / r.h;
    float edge = std::min({fx, 1 - fx, fy, 1 - fy});
    Zone zone = group->header.In(x, y) || edge > .28f ? Center :
        edge == fx ? Left : edge == 1 - fx ? Right : edge == fy ? Top : Bottom;
    if (group == B.pressed && (zone == Center || group->tabs.size() == 1)) return false;
    B.target = group; B.zone = zone; B.drop = r;
    if (zone == Left) B.drop.w /= 2;
    if (zone == Right) { B.drop.x += r.w / 2; B.drop.w /= 2; }
    if (zone == Top) B.drop.h /= 2;
    if (zone == Bottom) { B.drop.y += r.h / 2; B.drop.h /= 2; }
    return true;
}
void LayoutStrip(Rect r, bool compactMode) {
    L.player = r;
    L.grip = L.cover = L.title = L.subtitle = L.chip = {};
    float pad = S(12), button = S(compactMode ? 38 : 42);
    float buttonsW = button * 6 + S(8), bx = r.x + (r.w - buttonsW) / 2;
    float by = r.y + S(8);
    for (int i = 0; i < 6; ++i) {
        if (i == 3) bx += S(8);
        L.buttons[i] = {bx, by, button, button}; bx += button;
    }
    float y = by + button + S(6), time = std::min(S(58), r.w / 7), vol = std::min(S(90), r.w / 6);
    L.elapsed = {r.x + pad, y, time, S(22)};
    L.remaining = {r.R() - pad - time, y, time, S(22)};
    L.seek = {L.elapsed.R() + S(6), y, std::max(0.f, r.w - 2 * pad - 2 * time - S(12)), S(26)};
    y += S(30);
    L.speaker = {r.R() - pad - vol - S(20), y, S(20), S(18)};
    L.volume = {r.R() - pad - vol, y - S(4), vol, S(26)};
}
void LayoutCompact(Rect area, Rect& library) {
    float cover = std::min(S(96), area.h / 4), head = cover + S(24);
    LayoutStrip({area.x, area.y + head, area.w, S(126)}, true);
    L.player = {area.x, area.y, area.w, head + S(126)};
    L.cover = {area.x + S(12), area.y + S(12), cover, cover};
    float tx = L.cover.R() + S(12), tw = std::max(0.f, area.R() - S(12) - tx);
    L.title = {tx, L.cover.y + cover / 2 - S(22), tw, S(24)};
    L.subtitle = {tx, L.title.B() + S(4), tw, S(24)};
    library = {area.x, L.player.B(), area.w, std::max(0.f, area.B() - L.player.B())};
    L.upNext = {};
}
void PaintNode(const Node& n, const Palette& p) {
    if (n.Split()) {
        Fill(n.splitter, p.border); PaintNode(*n.a, p); PaintNode(*n.b, p); return;
    }
    Fill(n.header, p.surface);
    Frame(n.header, p.border);
    SDL_Rect clip{(int)n.header.x, (int)n.header.y, (int)n.header.w, (int)n.header.h};
    SDL_SetRenderClipRect(g.renderer, &clip);
    for (int i = 0; i < (int)n.tabs.size(); ++i) {
        Rect tab = TabRect(n, i);
        if (tab.w <= 0) continue;
        bool active = i == n.active;
        if (active) Fill(tab, p.select);
        DrawText(Font::Normal, kNames[n.tabs[i]], tab.x + S(10), TextY(tab), active ? p.selectText : p.sub, tab.w - S(32));
        Rect close = CloseRect(tab);
        DrawText(Font::Normal, "×", close.x, TextY(close), active ? p.selectText : p.sub, close.w, 1);
        if (B.keyboardFocus && n.tabs[i] == B.keyboardPanel) Frame(tab, p.accent);
    }
    SDL_SetRenderClipRect(g.renderer, nullptr);
}
void DrawNow(const Rect& r, const Palette& p) {
    if (r.w <= 0 || r.h <= 0) return;
    Fill(r, p.surface2);
    SDL_Rect previousClip{};
    const bool hadClip = SDL_RenderClipEnabled(g.renderer);
    SDL_GetRenderClipRect(g.renderer, &previousClip);
    const SDL_Rect clip{(int)r.x, (int)r.y, (int)r.w, (int)r.h};
    SDL_SetRenderClipRect(g.renderer, &clip);
    float h = std::min(r.h, S(112)), y = r.y + (r.h - h) / 2, pad = S(18);
    TrackId shown = g.playing != kNone ? g.playing : g.resumeTrack;
    std::string title = g_lib.Contains(shown) ? Utf8(FormatTitle(EffectiveFormat(kPackNowPlaying, g.settings.fmtTitle),
        Lookup(shown, g.playing != kNone))) : "Nothing playing";
    std::string sub = g_lib.Contains(shown) ? Utf8(FormatTitle(EffectiveFormat(kPackNowPlaying2, g.settings.fmtSub),
        Lookup(shown, g.playing != kNone))) : "Choose music from the Library";
    B.nowTitle = {r.x + pad, y, std::max(0.f, r.w - 2 * pad), std::min(S(38), r.B() - y)};
    DrawText(Font::Title, title, B.nowTitle.x, TextY(B.nowTitle, Font::Title), p.text, B.nowTitle.w, 1);
    DrawText(Font::Normal, sub, r.x + pad, y + S(42), p.sub, r.w - 2 * pad, 1);
    auto next = UpNextTracks();
    if (!next.empty()) DrawText(Font::Normal, "Up next · " + Utf8(TrackTitle(next[0])),
        r.x + pad, y + S(74), p.sub, r.w - 2 * pad, 1);
    SDL_SetRenderClipRect(g.renderer, hadClip ? &previousClip : nullptr);
}
Placement Capture() {
    Placement p;
    SDL_GetWindowSize(g.window, &p.w, &p.h);
    SDL_GetWindowPosition(g.window, &p.x, &p.y);
    p.maximized = (SDL_GetWindowFlags(g.window) & SDL_WINDOW_MAXIMIZED) != 0;
    p.valid = true; return p;
}
void RememberCompact() {
    // Maximizing does not replace the compact window's own normal-position rectangle.
    if (!(SDL_GetWindowFlags(g.window) & SDL_WINDOW_MAXIMIZED)) compact = Capture();
}
void Restore(const Placement& p) {
    if (!p.valid) return;
    SDL_RestoreWindow(g.window);
    SDL_SetWindowSize(g.window, p.w, p.h);
    if (!SDL_WINDOWPOS_ISUNDEFINED(p.x) && !SDL_WINDOWPOS_ISUNDEFINED(p.y)) SDL_SetWindowPosition(g.window, p.x, p.y);
    if (p.maximized) SDL_MaximizeWindow(g.window);
}
}  // namespace

void LoadBigLayout(const std::string& text) {
    CancelPress();
    B.keyboardFocus = false;
    std::string_view rest(text);
    bool ok = false; unsigned seen = 0;
    auto top = Read(rest, 0, seen, ok);
    if (!ok || rest.empty() || rest.front() != '|') { Defaults(); return; }
    rest.remove_prefix(1);
    auto bottom = Read(rest, 0, seen, ok);
    if (!ok || !rest.empty()) { Defaults(); return; }
    B.area[0] = std::move(top); B.area[1] = std::move(bottom); B.initialized = true;
}
std::string BigLayoutText() {
    if (!B.initialized) Defaults();
    std::string out; Write(B.area[0].get(), out); out += '|'; Write(B.area[1].get(), out); return out;
}
void ResetBigPanels() { CancelPress(); Defaults(); Changed(); }
bool BigPanelShown(int panel) {
    if (!B.initialized) Defaults();
    return panel >= 0 && panel < 3 && PanelGroup(panel);
}
void ToggleBigPanel(int panel) {
    if (panel < 0 || panel > 2) return;
    if (!B.initialized) Defaults();
    CancelPress();
    if (PanelGroup(panel)) RemovePanel(panel);
    else {
        int area = panel == 0 ? 0 : 1;
        Node* target = nullptr;
        Groups(B.area[area].get(), [&](Node& n) { if (!target) target = &n; });
        if (target) Insert(*target, Center, panel);
        else B.area[area] = MakePanelGroup({panel});
    }
    Changed();
}
bool LayoutPresentation(Rect bounds, Rect& library) {
    for (auto& body : B.bodies) body = {};
    B.nowTitle = {};
    if (g.settings.size == ViewSize::Normal) return false;
    L.chip = L.lyrics = L.upNext = {};
    if (g.settings.size == ViewSize::Compact) { LayoutCompact(bounds, library); return true; }
    float stripH = std::min(S(126), bounds.h), mid = bounds.y + (bounds.h - stripH) / 2;
    LayoutStrip({bounds.x, mid, bounds.w, stripH}, false);
    B.areas[0] = {bounds.x, bounds.y, bounds.w, std::max(0.f, mid - bounds.y)};
    B.areas[1] = {bounds.x, mid + stripH, bounds.w, std::max(0.f, bounds.B() - mid - stripH)};
    if (g.settings.size == ViewSize::Spotlight) {
        B.bodies[1] = B.areas[0]; L.upNext = B.areas[1]; library = {};
    } else {
        if (!B.initialized) Defaults();
        for (int i = 0; i < 2; ++i) if (B.area[i]) LayoutNode(*B.area[i], B.areas[i]);
        library = B.bodies[0]; L.upNext = B.bodies[2];
    }
    return true;
}
void DrawBigPanels(const Palette& p) {
    if (g.settings.size < ViewSize::Spotlight) return;
    if (g.settings.size == ViewSize::Big) {
        for (int i = 0; i < 2; ++i) {
            if (B.area[i]) PaintNode(*B.area[i], p);
            else if (B.areas[i].h >= LineHeight(Font::Normal) + S(24))
                DrawText(Font::Normal, "Drag a tab here, or reopen a panel from View", B.areas[i].x + S(12),
                B.areas[i].y + S(12), p.faint, std::max(0.f, B.areas[i].w - S(24)), 1);
        }
    }
    DrawNow(B.bodies[1], p);
    if (B.dropValid) { Fill(B.drop, p.accent, 60); Frame(B.drop, p.accent); }
}
bool PresentationMouseDown(float x, float y, int button, int) {
    if (g.settings.size < ViewSize::Spotlight) return false;
    B.keyboardFocus = false;
    if (B.nowTitle.In(x, y) && button == SDL_BUTTON_LEFT) { if (!Resume()) RevealPlaying(); return true; }
    if (g.settings.size != ViewSize::Big) return false;
    if (button == SDL_BUTTON_LEFT && (B.splitting = SplitterAt(x, y))) {
        SDL_CaptureMouse(true); return true;
    }
    Node* group = GroupAt(x, y);
    int tab = group ? TabAt(*group, x, y) : -1;
    if (tab >= 0 && (button == SDL_BUTTON_LEFT || button == SDL_BUTTON_MIDDLE)) {
        B.pressed = group; B.panel = group->tabs[tab]; B.pressX = x; B.pressY = y;
        B.close = button == SDL_BUTTON_MIDDLE || CloseRect(TabRect(*group, tab)).In(x, y);
        B.keyboardPanel = B.panel;
        if (!B.close && group->active != tab) { group->active = tab; Changed(); }
        SDL_CaptureMouse(true); return true;
    }
    return false;
}
bool PresentationMouseMove(float x, float y, bool leftDown) {
    if (g.settings.size != ViewSize::Big) return false;
    if (!leftDown && (B.splitting || (B.pressed && !B.close))) {
        CancelPress(); SDL_CaptureMouse(false); g.redraw = true; return false;
    }
    if (B.splitting) {
        auto& n = *B.splitting;
        float total = std::max(1.f, (n.vertical ? n.rect.h : n.rect.w) - S(6));
        float at = (n.vertical ? y - n.rect.y : x - n.rect.x) - S(3);
        float margin = std::min(.45f, S(90) / total);
        n.ratio = std::clamp(at / total, margin, 1 - margin); g.redraw = true; return true;
    }
    if (B.pressed && !B.close && leftDown) {
        if (std::abs(x - B.pressX) > S(4) || std::abs(y - B.pressY) > S(4)) B.dragging = true;
        if (B.dragging) B.dropValid = FindDrop(x, y);
        g.redraw = true; return true;
    }
    if (auto split = SplitterAt(x, y)) {
        static SDL_Cursor* ns = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NS_RESIZE);
        static SDL_Cursor* ew = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_EW_RESIZE);
        SDL_SetCursor(split->vertical ? ns : ew); return true;
    }
    return false;
}
bool PresentationMouseUp(float x, float y) {
    if (g.settings.size != ViewSize::Big || (!B.pressed && !B.splitting)) return false;
    SDL_CaptureMouse(false);
    if (B.splitting) { CancelPress(); Changed(); return true; }
    int panel = B.panel, area = B.targetArea;
    Node* target = B.target;
    Node* from = B.pressed;
    Zone zone = B.zone;
    bool moved = B.dragging && B.dropValid;
    int releasedTab = from ? TabAt(*from, x, y) : -1;
    bool close = B.close && releasedTab >= 0 && from->tabs[releasedTab] == panel;
    CancelPress();
    if (moved) {
        // Insert before removal: removal may collapse a split containing the target.
        if (target) {
            if (target == from) {
                auto it = std::find(target->tabs.begin(), target->tabs.end(), panel);
                target->tabs.erase(it);
                target->active = std::clamp(target->active, 0, (int)target->tabs.size() - 1);
                Insert(*target, zone, panel);
            } else {
                Insert(*target, zone, panel);
                // Remove only from the original group, without removing the just inserted panel.
                auto it = std::find(from->tabs.begin(), from->tabs.end(), panel);
                from->tabs.erase(it);
                from->active = std::clamp(from->active, 0, std::max(0, (int)from->tabs.size() - 1));
                if (from->tabs.empty()) {
                    // Prune an empty group by removing a sentinel that only that group contains.
                    from->tabs.push_back(3);
                    for (auto& root : B.area) Remove(root, 3);
                }
            }
        } else if (area >= 0) { RemovePanel(panel); B.area[area] = MakePanelGroup({panel}); }
        Changed();
    } else if (close) { RemovePanel(panel); Changed(); }
    return true;
}
bool PresentationKey(const SDL_KeyboardEvent& e) {
    if (g.settings.size != ViewSize::Big) return false;
    if (e.key == SDLK_F6) {
        g.searchFocused = false;
        BrowserBlur();
        SDL_StopTextInput(g.window);
        B.keyboardFocus = true;
        for (int i = 0; i < 3; ++i) {
            B.keyboardPanel = (B.keyboardPanel + (e.mod & SDL_KMOD_SHIFT ? 2 : 1)) % 3;
            if (PanelGroup(B.keyboardPanel)) break;
        }
        if (auto n = PanelGroup(B.keyboardPanel))
            n->active = (int)(std::find(n->tabs.begin(), n->tabs.end(), B.keyboardPanel) - n->tabs.begin());
        Changed(); return true;
    }
    if (!B.keyboardFocus) return false;
    if (e.key == SDLK_ESCAPE) { B.keyboardFocus = false; g.redraw = true; return true; }
    if (e.key == SDLK_DELETE) {
        if (PanelGroup(B.keyboardPanel)) ToggleBigPanel(B.keyboardPanel);
        return true;
    }
    const bool arrow = e.key == SDLK_LEFT || e.key == SDLK_RIGHT || e.key == SDLK_UP || e.key == SDLK_DOWN;
    if (arrow && (e.mod & SDL_KMOD_CTRL) && (e.mod & SDL_KMOD_SHIFT)) {
        const int panel = B.keyboardPanel;
        Node* from = PanelGroup(panel);
        Node* target = nullptr;
        for (auto& area : B.area) Groups(area.get(), [&](Node& n) {
            if (!target && &n != from) target = &n;
        });
        if (target) {
            RemovePanel(panel);
            Insert(*target, e.key == SDLK_LEFT ? Left : e.key == SDLK_RIGHT ? Right :
                e.key == SDLK_UP ? Top : Bottom, panel);
            Changed();
        }
        return true;
    }
    if (arrow && (e.mod & SDL_KMOD_ALT)) {
        if (Node* group = PanelGroup(B.keyboardPanel)) {
            if (Node* parent = ParentOf(group)) {
                float delta = e.key == SDLK_LEFT || e.key == SDLK_UP ? -.05f : .05f;
                parent->ratio = std::clamp(parent->ratio + delta, .1f, .9f);
                Changed();
            }
        }
        return true;
    }
    if ((e.mod & SDL_KMOD_CTRL) && (e.key == SDLK_UP || e.key == SDLK_DOWN)) {
        int panel = B.keyboardPanel, area = e.key == SDLK_UP ? 0 : 1;
        if (!PanelGroup(panel)) return true;
        RemovePanel(panel);
        Node* target = nullptr;
        Groups(B.area[area].get(), [&](Node& n) { if (!target) target = &n; });
        if (target) Insert(*target, Center, panel); else B.area[area] = MakePanelGroup({panel});
        Changed(); return true;
    }
    if (e.key == SDLK_LEFT || e.key == SDLK_RIGHT) {
        if (auto n = PanelGroup(B.keyboardPanel)) {
            n->active = (n->active + (e.key == SDLK_LEFT ? (int)n->tabs.size() - 1 : 1)) % (int)n->tabs.size();
            B.keyboardPanel = n->tabs[n->active]; Changed();
        }
        return true;
    }
    if (e.key == SDLK_UP || e.key == SDLK_DOWN) {
        for (int i = 0; i < 3; ++i) {
            B.keyboardPanel = (B.keyboardPanel + (e.key == SDLK_UP ? 2 : 1)) % 3;
            if (PanelGroup(B.keyboardPanel)) break;
        }
        if (auto n = PanelGroup(B.keyboardPanel))
            n->active = (int)(std::find(n->tabs.begin(), n->tabs.end(), B.keyboardPanel) - n->tabs.begin());
        Changed(); return true;
    }
    if (e.key == SDLK_RETURN) { B.keyboardFocus = false; return true; }
    return false;
}
void LoadCompactPlacement(const std::string& text) {
    compact = Placement{};
    std::string_view rest(text);
    Placement p;
    int* values[] = {&p.x, &p.y, &p.w, &p.h};
    for (int i = 0; i < 4; ++i) {
        if (!Number(rest, *values[i])) return;
        if (i < 3) {
            if (rest.empty() || rest.front() != ',') return;
            rest.remove_prefix(1);
        }
    }
    if (!rest.empty() || p.w < 300 || p.w > 16384 || p.h < 260 || p.h > 16384) return;
    p.valid = true; compact = p;
}

std::string CompactPlacementText() {
    if (g.window && !MiniActive() && g.settings.size == ViewSize::Compact) RememberCompact();
    if (!compact.valid) return "";
    return std::to_string(compact.x) + "," + std::to_string(compact.y) + "," +
        std::to_string(compact.w) + "," + std::to_string(compact.h);
}
void SetViewSize(ViewSize size) {
    if (size >= ViewSize::Count) return;
    if (MiniActive()) LeaveMini();
    if (size == g.settings.size) return;
    CancelPress();
    if (size == ViewSize::Compact) {
        full = Capture();
        if (full.maximized) {
            SDL_RestoreWindow(g.window);
            full = Capture();
            full.maximized = true;
        }
        if (!compact.valid) {
            compact.x = full.x; compact.y = full.y; compact.w = 440; compact.h = 600; compact.valid = true;
        }
        SDL_SetWindowMinimumSize(g.window, 300, 260); Restore(compact);
    } else {
        if (g.settings.size == ViewSize::Compact) RememberCompact();
        SDL_SetWindowMinimumSize(g.window, 520, 320);
        if (g.settings.size == ViewSize::Compact) Restore(full);
        if (size >= ViewSize::Spotlight && !(SDL_GetWindowFlags(g.window) & SDL_WINDOW_MAXIMIZED)) {
            int w = 0, h = 0; SDL_GetWindowSize(g.window, &w, &h);
            SDL_Rect usable{};
            SDL_DisplayID display = SDL_GetDisplayForWindow(g.window);
            if (SDL_GetDisplayUsableBounds(display, &usable))
                SDL_SetWindowSize(g.window, std::min(std::max(w, 1000), usable.w),
                    std::min(std::max(h, 700), usable.h));
            else SDL_SetWindowSize(g.window, std::max(w, 1000), std::max(h, 700));
        }
    }
    g.settings.size = size;
    RebuildRows();
    Changed();
}
void InitializePresentation() {
    ViewSize requested = g.settings.size;
    full = Capture();
    g.settings.size = ViewSize::Normal;
    SetViewSize(requested);
}
void SavePresentationWindow() {
    if (MiniActive()) return;
    Placement p = Capture();
    if (g.settings.size == ViewSize::Compact) { RememberCompact(); p = full; }
    if (p.valid) {
        g.settings.maximized = p.maximized;
        if (!p.maximized) { g.settings.windowW = p.w; g.settings.windowH = p.h; }
    }
}
}  // namespace lx
