// The Keyboard shortcuts panel on Linux (File > Keyboard shortcuts), as the Windows build's window in
// app_keys.cpp: every command with its keys in cordinal and from any program; choose one, press Set and
// then the key. A key another command had moves, and the hint line says so. Clear removes a command's
// keys, and Reset all (pressed twice) brings back cordinal's own.
#include "panels_internal.h"

#include "textcase.h"

namespace lx::panel {
namespace {

using keymap::Binding;
using keymap::Key;
using keymap::Scope;

struct Row {
    std::string label, path;
    std::wstring name, searchable;  // name as bindings store it; searchable folded, as the query is
};

struct KeysPanel {
    TextField query;
    std::vector<Row> rows;
    std::vector<int> shown;
    int selected = 0, first = 0;
    Rect panel, field, list, buttons[4];
    std::vector<Binding> window, global;
    int capture = -1;  // the Scope a key is being pressed for, or -1
    std::wstring capturing;
    std::string hint;
    bool confirmReset = false;
} g_keys;
constexpr int kKeyRows = 11;
constexpr const char* kDefaultHint =
    "Choose a command, then Set and the key. In cordinal: while its window is in front. From any program: "
    "through the desktop; a numpad key on its own works in Lightweight mode.";

std::string Narrow(const std::wstring& s) { return compat::Narrow(s.c_str()); }

std::string Joined(const std::vector<Key>& keys) {
    std::string out;
    for (const Key& k : keys) out += (out.empty() ? "" : ", ") + Narrow(keymap::Text(k));
    return out;
}

void Refresh() {
    g_keys.window = EffectiveKeyBindings(Scope::Window);
    g_keys.global = EffectiveKeyBindings(Scope::Global);
    g.redraw = true;
}

void Filter() {
    std::wstring q = textcase::Fold(g_keys.query.text);
    std::vector<std::wstring> words;
    for (size_t i = 0; i < q.size();) {
        size_t end = q.find(L' ', i);
        if (end == std::wstring::npos) end = q.size();
        if (end > i) words.push_back(q.substr(i, end - i));
        i = end + 1;
    }
    g_keys.shown.clear();
    for (int i = 0; i < (int)g_keys.rows.size(); ++i)
        if (std::all_of(words.begin(), words.end(), [&](const std::wstring& w) {
                return g_keys.rows[i].searchable.find(w) != std::wstring::npos;
            }))
            g_keys.shown.push_back(i);
    g_keys.selected = std::clamp(g_keys.selected, 0, std::max(0, (int)g_keys.shown.size() - 1));
    g_keys.first = std::min(g_keys.first, g_keys.selected);
}

const Row* Selected() {
    return g_keys.selected >= 0 && g_keys.selected < (int)g_keys.shown.size() ? &g_keys.rows[g_keys.shown[g_keys.selected]]
                                                                                : nullptr;
}

void StartCapture(Scope scope) {
    const Row* r = Selected();
    if (!r) return;
    g_keys.capture = (int)scope;
    g_keys.capturing = r->name;
    g_keys.confirmReset = false;
    g_keys.hint = "Press the key for \xE2\x80\x9C" + r->label + "\xE2\x80\x9D " +
                  (scope == Scope::Window ? "in cordinal" : "from any program") + ". Esc cancels.";
    g.redraw = true;
}

void Captured(const SDL_KeyboardEvent& e) {
    const int sc = (int)e.scancode;
    if (sc >= 224 && sc <= 231) return;  // a modifier alone: wait for the rest
    const Scope scope = (Scope)g_keys.capture;
    if (e.key == SDLK_ESCAPE && !(e.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT))) {
        g_keys.capture = -1;
        g_keys.hint = kDefaultHint;
        return;
    }
    Key key;
    std::wstring why;
    if (!KeyFromEvent(e, key)) {
        g_keys.hint = "cordinal can't use that key. Try another, or Esc to cancel.";
        return;
    }
    if (!keymap::Allowed(scope, key, &why)) {
        g_keys.hint = Narrow(keymap::Text(key) + L": " + why) + " Try another, or Esc to cancel.";
        return;
    }
    std::vector<Binding> changes = g.settings.keyChanges;
    const std::wstring moved = keymap::Assign(changes, DefaultKeyBindings(), scope, key, g_keys.capturing);
    g_keys.capture = -1;
    SetKeyChanges(std::move(changes));
    Refresh();
    g_keys.hint = Narrow(keymap::Text(key) + L" now runs \x201C" + g_keys.capturing + L"\x201D.");
    if (!moved.empty()) g_keys.hint += Narrow(L" It no longer runs \x201C" + moved + L"\x201D.");
}

void ClearSelected() {
    const Row* r = Selected();
    if (!r) return;
    std::vector<Binding> changes = g.settings.keyChanges;
    const std::vector<Binding> defaults = DefaultKeyBindings();
    for (Scope scope : {Scope::Window, Scope::Global})
        for (const Key& k : keymap::KeysOf(keymap::Effective(defaults, changes, scope), r->name))
            if (keymap::Allowed(scope, k)) keymap::Assign(changes, defaults, scope, k, L"");  // a typing key stays
    SetKeyChanges(std::move(changes));
    Refresh();
    g_keys.hint = "\xE2\x80\x9C" + r->label + "\xE2\x80\x9D has no keys of its own now.";
}

void ResetAll() {
    if (!g_keys.confirmReset) {
        g_keys.confirmReset = true;
        g_keys.hint = "Press Reset all again to put back cordinal's own keys and remove every key you set.";
        return;
    }
    g_keys.confirmReset = false;
    SetKeyChanges({});
    Refresh();
    g_keys.hint = "cordinal's own keys are back.";
}

void DrawKeys(const Palette& p) {
    Rect panel = g_keys.panel = DrawFrame(p, S(760), S(520), "Keyboard shortcuts");
    const float pad = S(16), x0 = panel.x + pad, w = panel.w - 2 * pad, rowH = S(28), keysW = w * 0.22f;
    g_keys.field = {x0, panel.y + S(48), w, S(30)};
    DrawLineField(g_keys.query, g_keys.field, g_keys.capture < 0, g_keys.capture < 0, "Search commands", p);
    const float headY = g_keys.field.B() + S(6);
    DrawText(Font::Small, "Command", x0 + S(8), headY, p.sub);
    DrawText(Font::Small, "In cordinal", x0 + w - 2 * keysW, headY, p.sub);
    DrawText(Font::Small, "From any program", x0 + w - keysW, headY, p.sub);
    g_keys.list = {x0, headY + S(22), w, rowH * kKeyRows};
    for (int i = 0; i < kKeyRows && g_keys.first + i < (int)g_keys.shown.size(); ++i) {
        const int row = g_keys.first + i;
        const Row& r = g_keys.rows[g_keys.shown[row]];
        Rect rect{g_keys.list.x, g_keys.list.y + i * rowH, g_keys.list.w, rowH};
        const bool selected = row == g_keys.selected;
        if (selected) Fill(rect, p.select);
        else if (rect.In(g_mx, g_my)) Fill(rect, p.hover);
        const uint32_t main = selected ? p.selectText : p.text, sub = selected ? p.selectText : p.sub;
        const float nameW = rect.w - 2 * keysW - S(16);
        const float labelW = std::min(TextWidth(Font::Normal, r.label) + S(12), nameW);
        DrawText(Font::Normal, r.label, rect.x + S(8), TextY(rect), main, labelW);
        if (labelW + S(40) < nameW) DrawText(Font::Small, r.path, rect.x + S(8) + labelW, TextY(rect, Font::Small), sub, nameW - labelW);
        const bool capturing = g_keys.capture >= 0 && r.name == g_keys.capturing;
        std::string keys[2] = {Joined(keymap::KeysOf(g_keys.window, r.name)), Joined(keymap::KeysOf(g_keys.global, r.name))};
        if (capturing) keys[g_keys.capture] = "Press a key\xE2\x80\xA6";
        for (int col = 0; col < 2; ++col)
            DrawText(Font::Normal, keys[col], rect.R() - (2 - col) * keysW, TextY(rect),
                     capturing && col == g_keys.capture ? (selected ? p.selectText : p.accent) : main, keysW - S(8));
    }
    if (g_keys.shown.empty()) DrawText(Font::Normal, "Nothing matches.", g_keys.list.x + S(8), g_keys.list.y + S(6), p.sub);
    const float hintY = g_keys.list.B() + S(8);
    DrawText(Font::Small, g_keys.hint.empty() ? kDefaultHint : g_keys.hint.c_str(), x0, hintY, p.sub, w);
    const char* labels[4] = {"Set in cordinal", "Set from any program", "Clear",
                             g_keys.confirmReset ? "Really reset?" : "Reset all"};
    float x = x0;
    const float buttonY = panel.B() - pad - S(30);
    for (int i = 0; i < 4; ++i) {
        const float bw = TextWidth(Font::Normal, labels[i]) + S(28);
        g_keys.buttons[i] = {x, buttonY, bw, S(30)};
        DrawButton(g_keys.buttons[i], labels[i], p, false, g_keys.capture < 0 && (i == 3 || Selected()));
        x += bw + S(8);
    }
}

void Move(int to) {
    const int count = (int)g_keys.shown.size();
    g_keys.selected = std::clamp(to, 0, std::max(0, count - 1));
    if (g_keys.selected < g_keys.first) g_keys.first = g_keys.selected;
    else if (g_keys.selected >= g_keys.first + kKeyRows) g_keys.first = g_keys.selected - kKeyRows + 1;
}

void KeysMouseDown(float x, float y, int clicks) {
    if (!g_keys.panel.In(x, y)) return ClosePanel();
    if (g_keys.capture >= 0) {  // a click gives up the key being pressed
        g_keys.capture = -1;
        g_keys.hint = kDefaultHint;
        return;
    }
    if (g_keys.list.In(x, y)) {
        const int row = g_keys.first + (int)((y - g_keys.list.y) / S(28));
        if (row < (int)g_keys.shown.size()) {
            g_keys.selected = row;
            if (clicks >= 2) StartCapture(Scope::Window);
        }
        return;
    }
    if (g_keys.buttons[0].In(x, y)) StartCapture(Scope::Window);
    else if (g_keys.buttons[1].In(x, y)) StartCapture(Scope::Global);
    else if (g_keys.buttons[2].In(x, y)) ClearSelected();
    else if (g_keys.buttons[3].In(x, y)) ResetAll();
}

void KeysKey(const SDL_KeyboardEvent& e) {
    if (g_keys.capture >= 0) return Captured(e);
    switch (e.key) {
        case SDLK_ESCAPE: return ClosePanel();
        case SDLK_RETURN: case SDLK_KP_ENTER: return StartCapture(e.mod & SDL_KMOD_SHIFT ? Scope::Global : Scope::Window);
        case SDLK_UP: return Move(g_keys.selected - 1);
        case SDLK_DOWN: return Move(g_keys.selected + 1);
        case SDLK_PAGEUP: return Move(g_keys.selected - kKeyRows);
        case SDLK_PAGEDOWN: return Move(g_keys.selected + kKeyRows);
        case SDLK_DELETE:
            if (g_keys.query.text.empty()) return ClearSelected();
            break;
        default: break;
    }
    if (LineFieldKey(g_keys.query, e) == 1) Filter();
}

void KeysText(const char* s) {
    if (g_keys.capture >= 0) return;  // the key being pressed isn't typing
    if (LineFieldInsert(g_keys.query, Printable(compat::Widen(s)))) Filter();
}

}  // namespace

const Ops kKeysOps{
    .draw = DrawKeys,
    .mouseDown = KeysMouseDown,
    .wheel = [](float, float, float dy) {
        g_keys.first = std::clamp(g_keys.first - (int)std::lround(dy * 2), 0, std::max(0, (int)g_keys.shown.size() - kKeyRows));
    },
    .key = KeysKey,
    .text = KeysText,
    .wantsText = [](float x, float y) { return g_keys.field.In(x, y); },
    .close = [] { g_keys = KeysPanel{}; },
    .typing = true,
};

}  // namespace lx::panel

namespace lx {
using namespace panel;

void OpenKeyboardShortcuts() {
    if (PanelOpen()) ClosePanel();
    g_keys = KeysPanel{};
    for (const PaletteCommand& c : MenuCommands()) {
        Row r;
        r.label = c.label;
        r.path = c.path;
        std::wstring label = compat::Widen(c.label.c_str()), path = compat::Widen(c.path.c_str());
        r.name = path.empty() ? label : path + L" > " + label;
        r.searchable = textcase::Fold(label + L" " + path);
        g_keys.rows.push_back(std::move(r));
    }
    Refresh();
    Filter();
    g_kind = Kind::Keys;
    SDL_StartTextInput(g.window);
    g.redraw = true;
}

// For the layout check: the panel's rows as "label | in cordinal | from any program", after a search.
std::vector<std::string> KeyboardShortcutRows(const std::wstring& query) {
    g_keys.query.text = query;
    g_keys.query.caret = query.size();
    Filter();
    std::vector<std::string> rows;
    for (int i : g_keys.shown) {
        const Row& r = g_keys.rows[i];
        rows.push_back(r.label + " | " + Joined(keymap::KeysOf(g_keys.window, r.name)) + " | " +
                       Joined(keymap::KeysOf(g_keys.global, r.name)));
    }
    return rows;
}

}  // namespace lx
