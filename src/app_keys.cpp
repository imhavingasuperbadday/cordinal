// Your own keyboard shortcuts on Windows (File > Keyboard shortcuts, app_keys.h). The rules and names are
// shared with Linux (keymap.h); this file keeps your changes in settings.ini ([Keys] for "in cordinal",
// [Global keys] for "from any program", "Ctrl+Shift+K=Playback > Next", an empty command removing one of
// cordinal's keys), applies them, and is the window where you press a key to assign it.
//
// "In cordinal" keys join the accelerator table after the packs', so yours win. "From any program" keys
// with a modifier are RegisterHotKey's, held while cordinal runs; the numpad's bare keys are Lightweight
// mode's remote (app_lightweight.cpp), whose hook asks NumpadKeysBound which keys to take.
#include "app.h"

#include <cwctype>

namespace app {

namespace {

using keymap::Binding;
using keymap::Key;
using keymap::Scope;

constexpr ExtraCommand kExtras[] = {
    {IDM_QUICK_PICK, L"Playback", L"Play a song..."},
    {IDM_VOLUME_UP, L"Playback", L"Volume up"},
    {IDM_VOLUME_DOWN, L"Playback", L"Volume down"},
    {IDM_SEEK_BACK, L"Playback", L"Seek back 10 seconds"},
    {IDM_SEEK_FORWARD, L"Playback", L"Seek forward 10 seconds"},
};
constexpr const wchar_t* kSections[] = {L"Keys", L"Global keys"};  // by Scope
constexpr int kHotkeyBase = 0x4D00;  // WM_HOTKEY ids, beside the media keys (1-4) and packs' (0x5000)

struct State {
    bool loaded = false;
    std::vector<Binding> changes;
    std::vector<std::pair<int, int>> hotkeys;  // WM_HOTKEY id, command
    std::vector<std::wstring> taken;           // "Ctrl+Alt+K" another program holds
    int numpad[keymap::kNumpadKeys] = {};      // command per numpad key, 0 for none
} s;

void Load() {
    s.loaded = true;
    s.changes.clear();
    if (g.iniPath.empty()) return;
    for (int scope = 0; scope < 2; ++scope) {
        std::vector<wchar_t> buf(32768, 0);
        DWORD n = GetPrivateProfileSectionW(kSections[scope], buf.data(), (DWORD)buf.size(), g.iniPath.c_str());
        for (const wchar_t* at = buf.data(); at < buf.data() + n && *at; at += wcslen(at) + 1) {
            std::wstring line = at;
            size_t eq = line.find(L'=');
            if (eq == std::wstring::npos) continue;
            Binding b;
            b.scope = (Scope)scope;
            if (!keymap::Parse(line.substr(0, eq), b.key) || !keymap::Allowed(b.scope, b.key)) continue;
            b.command = line.substr(eq + 1);
            b.command.erase(0, b.command.find_first_not_of(L' '));
            b.command.erase(b.command.find_last_not_of(L' ') + 1);
            std::erase_if(s.changes, [&](const Binding& o) { return o.scope == b.scope && o.key == b.key; });
            s.changes.push_back(std::move(b));
        }
    }
}

void Save() {
    for (int scope = 0; scope < 2; ++scope) {
        std::wstring data;
        for (const Binding& b : s.changes)
            if ((int)b.scope == scope) data += keymap::Text(b.key) + L"=" + b.command + L'\0';
        if (data.empty()) WritePrivateProfileStringW(kSections[scope], nullptr, nullptr, g.iniPath.c_str());
        else WritePrivateProfileSectionW(kSections[scope], (data + L'\0').c_str(), g.iniPath.c_str());
    }
}

UINT HotkeyMods(uint8_t mods) {
    return MOD_NOREPEAT | (mods & keymap::kWin ? MOD_WIN : 0) | (mods & keymap::kCtrl ? MOD_CONTROL : 0) |
           (mods & keymap::kAlt ? MOD_ALT : 0) | (mods & keymap::kShift ? MOD_SHIFT : 0);
}

BYTE AccelMods(uint8_t mods) {
    return FVIRTKEY | (mods & keymap::kCtrl ? FCONTROL : 0) | (mods & keymap::kAlt ? FALT : 0) |
           (mods & keymap::kShift ? FSHIFT : 0);
}

// Names and Windows codes for the keys that aren't letters, digits or F-keys. Numpad keys are listed
// with their Num Lock on code; KeyFromVk also reads the Num Lock off codes.
struct VkName {
    UINT vk;
    const wchar_t* name;
};
constexpr VkName kVkNames[] = {
    {VK_SPACE, L"Space"}, {VK_TAB, L"Tab"}, {VK_ESCAPE, L"Esc"}, {VK_BACK, L"Backspace"}, {VK_DELETE, L"Delete"},
    {VK_INSERT, L"Insert"}, {VK_HOME, L"Home"}, {VK_END, L"End"}, {VK_PRIOR, L"Page Up"}, {VK_NEXT, L"Page Down"},
    {VK_UP, L"Up"}, {VK_DOWN, L"Down"}, {VK_LEFT, L"Left"}, {VK_RIGHT, L"Right"}, {VK_PAUSE, L"Pause"},
    {VK_OEM_PLUS, L"Equals"}, {VK_OEM_MINUS, L"Minus"}, {VK_OEM_COMMA, L"Comma"}, {VK_OEM_PERIOD, L"Period"},
    {VK_OEM_2, L"Slash"}, {VK_OEM_1, L"Semicolon"}, {VK_OEM_7, L"Quote"}, {VK_OEM_3, L"Backquote"},
    {VK_OEM_4, L"Left Bracket"}, {VK_OEM_6, L"Right Bracket"}, {VK_OEM_5, L"Backslash"},
    {VK_MEDIA_PLAY_PAUSE, L"Play/Pause"}, {VK_MEDIA_NEXT_TRACK, L"Next Track"}, {VK_MEDIA_PREV_TRACK, L"Previous Track"},
    {VK_MEDIA_STOP, L"Stop Media"}, {VK_VOLUME_UP, L"Volume Up"}, {VK_VOLUME_DOWN, L"Volume Down"}, {VK_VOLUME_MUTE, L"Mute"},
    {VK_DECIMAL, L"Num ."}, {VK_ADD, L"Num +"}, {VK_SUBTRACT, L"Num -"}, {VK_MULTIPLY, L"Num *"}, {VK_DIVIDE, L"Num /"},
};
// The numpad with Num Lock off sends the keys printed beside its digits, without the extended flag.
constexpr UINT kNumLockOff[11] = {VK_INSERT, VK_END, VK_DOWN, VK_NEXT, VK_LEFT, VK_CLEAR, VK_RIGHT, VK_HOME, VK_UP, VK_PRIOR, VK_DELETE};

// Every listed command's name by id, built once per use: CommandName walks the whole menu bar.
std::unordered_map<int, std::wstring> CommandNames() {
    std::unordered_map<int, std::wstring> names;
    for (const MenuCommandInfo& c : AllMenuCommands()) {
        if (names.count(c.command)) continue;
        std::wstring path = c.path;
        std::replace(path.begin(), path.end(), L'\x203A', L'>');
        names[c.command] = path.empty() ? c.label : path + L" > " + c.label;
    }
    return names;
}

bool Repeats(int command) {
    return command == IDM_VOLUME_UP || command == IDM_VOLUME_DOWN || command == IDM_SEEK_BACK || command == IDM_SEEK_FORWARD;
}

}  // namespace

std::span<const ExtraCommand> ExtraCommands() { return kExtras; }

bool KeyFromVk(UINT vk, bool extended, uint8_t mods, keymap::Key& out) {
    out = {};
    out.mods = mods;
    if (vk == VK_RETURN) out.name = extended ? L"Num Enter" : L"Enter";
    else if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) out.name = std::wstring(1, (wchar_t)vk);
    else if (vk >= VK_F1 && vk <= VK_F24) out.name = L"F" + std::to_wstring(vk - VK_F1 + 1);
    else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) out.name = keymap::NumpadName(vk - VK_NUMPAD0);
    else if (!extended) {
        for (int i = 0; i < 11 && out.name.empty(); ++i)
            if (vk == kNumLockOff[i]) out.name = keymap::NumpadName(i);
    }
    for (const VkName& n : kVkNames)
        if (out.name.empty() && vk == n.vk) out.name = n.name;
    return !out.name.empty();
}

UINT VkOf(const keymap::Key& key) {
    const std::wstring& n = key.name;
    if (n == L"Enter" || n == L"Num Enter") return VK_RETURN;
    if (n.size() == 1) return (UINT)n[0];
    if (n.size() >= 2 && n[0] == L'F' && iswdigit(n[1])) return VK_F1 + _wtoi(n.c_str() + 1) - 1;
    const int numpad = keymap::NumpadIndex({0, n});
    if (numpad >= 0 && numpad <= 9) return VK_NUMPAD0 + numpad;
    for (const VkName& v : kVkNames)
        if (n == v.name) return v.vk;
    return 0;
}

void StepVolume(bool up) {
    if (g.softwareVolume) {
        SetVolumePercent(std::clamp(g.volumePercent + (up ? 5 : -5), 0, 100));
        return;
    }
    // With cordinal's own volume control off, the system's level moves, through the shell's own keys.
    DefWindowProcW(g.hwnd, WM_APPCOMMAND, (WPARAM)g.hwnd,
                   MAKELPARAM(0, FAPPCOMMAND_KEY | (up ? APPCOMMAND_VOLUME_UP : APPCOMMAND_VOLUME_DOWN)));
}

const std::vector<Binding>& KeyChanges() {
    if (!s.loaded) Load();
    return s.changes;
}

void SetKeyChanges(std::vector<Binding> changes) {
    s.loaded = true;
    s.changes = std::move(changes);
    Save();
    if (!g.hwnd) return;
    // A fresh menu bar, so a shortcut text you took away comes back with cordinal's key.
    HMENU old = MainMenuBar();
    ReplaceMenuBar(BuildMenu());
    if (old) DestroyMenu(old);
    if (g.accel) DestroyAcceleratorTable(g.accel);
    g.accel = BuildAccelerators();  // registers the global keys again, and the menus show your keys
    LightweightPreferencesChanged();  // the numpad hook takes the keys it now has
}

std::wstring GlobalKeyText(int command) {
    const std::wstring name = CommandName(command);
    if (name.empty()) return {};
    for (const Key& k : keymap::KeysOf(EffectiveBindings(Scope::Global), name))
        if (keymap::NumpadIndex(k) < 0) return keymap::Text(k);
    return {};
}

std::vector<Binding> DefaultBindings() {
    std::vector<Binding> out = keymap::DefaultGlobal();
    const auto names = CommandNames();
    for (const ACCEL& a : BuiltInAccelerators()) {
        auto found = names.find(a.cmd);
        // A key whose command no menu lists (Ctrl+H cycles the shuffle button) still counts as cordinal's,
        // so you can clear it; "#id" names nothing you could bind.
        std::wstring name = found != names.end() ? found->second : L"#" + std::to_wstring(a.cmd);
        uint8_t mods = (a.fVirt & FCONTROL ? keymap::kCtrl : 0) | (a.fVirt & FALT ? keymap::kAlt : 0) |
                       (a.fVirt & FSHIFT ? keymap::kShift : 0);
        Binding b;
        b.scope = Scope::Window;
        // Accelerators can't tell the numpad's Enter from the main one: it is Enter.
        if (name.empty() || !KeyFromVk(a.key, a.key != VK_RETURN, mods, b.key)) continue;
        // One name per key: Ctrl+Equals is both the main and the shifted key's; keep the first.
        if (std::any_of(out.begin(), out.end(), [&](const Binding& o) { return o.scope == Scope::Window && o.key == b.key; }))
            continue;
        b.command = std::move(name);
        out.push_back(std::move(b));
    }
    return out;
}

std::vector<Binding> EffectiveBindings(Scope scope) { return keymap::Effective(DefaultBindings(), KeyChanges(), scope); }

void ApplyKeyChanges(std::vector<ACCEL>& keys) {
    for (const Binding& b : KeyChanges()) {
        if (b.scope != Scope::Window) continue;
        const UINT vk = VkOf(b.key);
        if (!vk) continue;
        ACCEL a{AccelMods(b.key.mods), (WORD)vk, 0};
        std::erase_if(keys, [&](const ACCEL& o) { return o.fVirt == a.fVirt && o.key == a.key; });
        if (b.command.empty()) continue;
        const int command = MenuCommandByName(b.command);
        if (command <= 0) continue;
        a.cmd = (WORD)command;
        keys.insert(keys.begin(), a);
    }
}

void UnregisterGlobalKeys() {
    for (const auto& [id, command] : s.hotkeys) UnregisterHotKey(g.hwnd, id);
    s.hotkeys.clear();
}

void RegisterGlobalKeys() {
    UnregisterGlobalKeys();
    s.taken.clear();
    for (int& c : s.numpad) c = 0;
    if (!g.hwnd) return;
    for (const Binding& b : EffectiveBindings(Scope::Global)) {
        const int command = MenuCommandByName(b.command);
        if (command <= 0) continue;
        if (const int numpad = keymap::NumpadIndex(b.key); numpad >= 0) {
            s.numpad[numpad] = command;
            continue;
        }
        // cordinal's own keys from any program (Win+Alt+Space) are taken only in Lightweight mode, which
        // needs them; a key you set yourself is held whenever cordinal runs.
        const bool yours = std::any_of(KeyChanges().begin(), KeyChanges().end(), [&](const Binding& c) {
            return c.scope == Scope::Global && c.key == b.key;
        });
        if (!yours && !g.lightweight) continue;
        const UINT vk = VkOf(b.key);
        const int id = kHotkeyBase + (int)s.hotkeys.size();
        if (vk && RegisterHotKey(g.hwnd, id, HotkeyMods(b.key.mods), vk)) s.hotkeys.push_back({id, command});
        else s.taken.push_back(keymap::Text(b.key));
    }
}

bool GlobalKeyHotkey(int id) {
    for (const auto& [hotkey, command] : s.hotkeys)
        if (hotkey == id) {
            OnCommand(command, 0, nullptr);
            return true;
        }
    return false;
}

const std::vector<std::wstring>& TakenGlobalKeys() { return s.taken; }

uint32_t NumpadKeysBound() {
    uint32_t mask = 0;
    for (int i = 0; i < keymap::kNumpadKeys; ++i)
        if (s.numpad[i]) mask |= 1u << i;
    return mask;
}

void NumpadKeyPressed(int index, bool repeat) {
    if (index < 0 || index >= keymap::kNumpadKeys || !s.numpad[index]) return;
    const int command = s.numpad[index];
    if (repeat && !Repeats(command)) return;
    OnCommand(command, 0, nullptr);
    UpdateTray();
}

// Each item's shortcut text follows your changes: a key you moved or cleared leaves the item that showed
// it, and a key you gave a command shows on that command's item. Other items keep the text they were built
// with (Space for Play / Pause is the track list's own key, not an accelerator).
static void ApplyLabels(HMENU menu, const std::vector<Binding>& effective, const std::unordered_map<int, std::wstring>& names);

void ApplyShortcutLabels(HMENU menu) {
    if (!menu || KeyChanges().empty()) return;
    ApplyLabels(menu, EffectiveBindings(Scope::Window), CommandNames());
}

static void ApplyLabels(HMENU menu, const std::vector<Binding>& effective, const std::unordered_map<int, std::wstring>& names) {
    for (int i = 0, n = GetMenuItemCount(menu); i < n; ++i) {
        if (HMENU sub = GetSubMenu(menu, i)) {
            ApplyLabels(sub, effective, names);
            continue;
        }
        const UINT id = GetMenuItemID(menu, i);
        if (id == (UINT)-1 || !id) continue;
        wchar_t text[256]{};
        GetMenuStringW(menu, i, text, (int)std::size(text), MF_BYPOSITION);
        std::wstring label = text, shown;
        if (size_t tab = label.find(L'\t'); tab != std::wstring::npos) {
            shown = label.substr(tab + 1);
            label.resize(tab);
        }
        auto found = names.find((int)id);
        if (found == names.end()) continue;
        const std::wstring& name = found->second;
        Key old;
        const bool changedOld = !shown.empty() && keymap::Parse(shown, old) &&
                                std::any_of(KeyChanges().begin(), KeyChanges().end(), [&](const Binding& b) {
                                    return b.scope == Scope::Window && b.key == old;
                                });
        const bool added = std::any_of(KeyChanges().begin(), KeyChanges().end(), [&](const Binding& b) {
            return b.scope == Scope::Window && keymap::SameCommand(b.command, name);
        });
        if (!changedOld && !added) continue;
        // The newest key shows: the one you gave it, else what remains of cordinal's.
        std::vector<Key> keys = keymap::KeysOf(effective, name);
        const std::wstring now = keys.empty() ? L"" : keymap::Text(keys.back());
        if (now == shown) continue;
        MENUITEMINFOW mi{sizeof(mi)};
        mi.fMask = MIIM_STRING;
        std::wstring full = now.empty() ? label : label + L"\t" + now;
        mi.dwTypeData = full.data();
        SetMenuItemInfoW(menu, i, TRUE, &mi);
    }
}

// --- the Keyboard shortcuts window ----------------------------------------------------------------------

namespace {

enum : int { kIdSearch = 100, kIdList, kIdHint, kIdSetWindow, kIdSetGlobal, kIdClear, kIdReset, kIdClose };

struct Row {
    std::wstring label, path, name;  // name: "Playback > Next", as bindings store it
};

struct Editor {
    HWND hwnd = nullptr, search = nullptr, list = nullptr, hint = nullptr;
    HWND buttons[5] = {};
    HFONT font = nullptr;
    std::vector<Row> rows;
    std::vector<int> shown;
    std::vector<Binding> window, global;  // in effect, for drawing
    int capture = -1;                      // the Scope a key is being pressed for, or -1
    std::wstring capturing;                // the command it is for
} e;

std::wstring Joined(const std::vector<Key>& keys) {
    std::wstring text;
    for (const Key& k : keys) text += (text.empty() ? L"" : L", ") + keymap::Text(k);
    return text;
}

void Refresh() {
    e.window = EffectiveBindings(Scope::Window);
    e.global = EffectiveBindings(Scope::Global);
    InvalidateRect(e.list, nullptr, FALSE);
}

void Filter() {
    std::vector<std::wstring> texts;
    for (const Row& r : e.rows) texts.push_back(r.label + L" " + r.path);
    const int keep = (int)SendMessageW(e.list, LB_GETCURSEL, 0, 0);
    const std::wstring was = keep >= 0 && keep < (int)e.shown.size() ? e.rows[e.shown[keep]].name : L"";
    e.shown = RankMatches(texts, WindowText(e.search));
    SendMessageW(e.list, WM_SETREDRAW, FALSE, 0);
    SendMessageW(e.list, LB_RESETCONTENT, 0, 0);
    int select = 0;
    for (int i : e.shown) {
        const Row& r = e.rows[i];
        if (r.name == was) select = (int)SendMessageW(e.list, LB_GETCOUNT, 0, 0);
        SendMessageW(e.list, LB_ADDSTRING, 0, (LPARAM)(r.label + L", " + r.path).c_str());
    }
    SendMessageW(e.list, LB_SETCURSEL, select, 0);
    SendMessageW(e.list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(e.list, nullptr, TRUE);
}

const Row* Selected() {
    int row = (int)SendMessageW(e.list, LB_GETCURSEL, 0, 0);
    return row >= 0 && row < (int)e.shown.size() ? &e.rows[e.shown[row]] : nullptr;
}

void Hint(const std::wstring& text) { SetWindowTextW(e.hint, text.c_str()); }

void DefaultHint() {
    Hint(L"Select a command, then press Set and the key. In cordinal: while its window is in front. From any "
         L"program: always; a numpad key on its own works in Lightweight mode.");
}

void EndCapture() {
    if (e.capture < 0) return;
    e.capture = -1;
    RegisterGlobalKeys();  // given up while a key was being pressed, so the window could see it
    for (HWND b : e.buttons) EnableWindow(b, TRUE);
    EnableWindow(e.search, TRUE);
}

void StartCapture(Scope scope) {
    const Row* r = Selected();
    if (!r) return;
    e.capture = (int)scope;
    e.capturing = r->name;
    UnregisterGlobalKeys();
    for (HWND b : e.buttons) EnableWindow(b, FALSE);
    EnableWindow(e.search, FALSE);
    SetFocus(e.list);
    Hint(L"Press the key for \x201C" + r->label + L"\x201D " +
         (scope == Scope::Window ? L"in cordinal" : L"from any program") + L". Esc cancels.");
}

// A key pressed while capturing: modifiers alone wait for the rest.
void Captured(UINT vk, LPARAM lp) {
    if (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN) return;
    const Scope scope = (Scope)e.capture;
    if (vk == VK_ESCAPE && !(GetKeyState(VK_CONTROL) < 0 || GetKeyState(VK_MENU) < 0)) {
        EndCapture();
        DefaultHint();
        return;
    }
    const uint8_t mods = (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0 ? keymap::kWin : 0) |
                         (GetKeyState(VK_CONTROL) < 0 ? keymap::kCtrl : 0) | (GetKeyState(VK_MENU) < 0 ? keymap::kAlt : 0) |
                         (GetKeyState(VK_SHIFT) < 0 ? keymap::kShift : 0);
    Key key;
    std::wstring why;
    if (!KeyFromVk(vk, (lp >> 24) & 1, mods, key)) {
        Hint(L"cordinal can't use that key. Try another, or Esc to cancel.");
        return;
    }
    if (!keymap::Allowed(scope, key, &why)) {
        Hint(keymap::Text(key) + L": " + why + L" Try another, or Esc to cancel.");
        return;
    }
    std::vector<Binding> changes = KeyChanges();
    const std::wstring moved = keymap::Assign(changes, DefaultBindings(), scope, key, e.capturing);
    const std::wstring command = e.capturing;
    EndCapture();
    SetKeyChanges(std::move(changes));
    Refresh();
    std::wstring text = keymap::Text(key) + L" now runs \x201C" + command + L"\x201D.";
    if (!moved.empty()) text += L" It no longer runs \x201C" + moved + L"\x201D.";
    const auto& taken = TakenGlobalKeys();
    if (std::find(taken.begin(), taken.end(), keymap::Text(key)) != taken.end())
        text = keymap::Text(key) + L" is held by another program, so cordinal can't have it yet. It is kept, and "
               L"works once that program lets it go.";
    Hint(text);
}

void ClearSelected() {
    const Row* r = Selected();
    if (!r) return;
    std::vector<Binding> changes = KeyChanges();
    const std::vector<Binding> defaults = DefaultBindings();
    for (Scope scope : {Scope::Window, Scope::Global})
        for (const Key& k : keymap::KeysOf(keymap::Effective(defaults, changes, scope), r->name))
            if (keymap::Allowed(scope, k)) keymap::Assign(changes, defaults, scope, k, L"");  // a typing key stays
    SetKeyChanges(std::move(changes));
    Refresh();
    Hint(L"\x201C" + r->label + L"\x201D has no keys of its own now.");
}

void ResetEverything() {
    if (MessageBoxW(e.hwnd, L"Put back cordinal's own keys and remove every key you set?", L"Keyboard shortcuts",
                    MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        return;
    SetKeyChanges({});
    Refresh();
    Hint(L"cordinal's own keys are back.");
}

void DrawRow(const DRAWITEMSTRUCT* di) {
    if (di->itemID == (UINT)-1 || di->itemID >= e.shown.size()) return;
    const Row& r = e.rows[e.shown[di->itemID]];
    const bool selected = di->itemState & ODS_SELECTED;
    HDC dc = di->hDC;
    RECT rc = di->rcItem;
    FillSolid(dc, rc, Ref(selected ? P().select : P().surface2));
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ old = SelectObject(dc, e.font);
    const COLORREF main = Ref(selected ? P().selectText : P().text), sub = Ref(selected ? P().selectText : P().sub);
    const int width = rc.right - rc.left, keysW = width * 22 / 100;
    RECT name{rc.left + S(10), rc.top, rc.right - 2 * keysW - S(10), rc.bottom};
    SetTextColor(dc, main);
    DrawTextW(dc, r.label.c_str(), -1, &name, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    SIZE extent{};
    GetTextExtentPoint32W(dc, r.label.c_str(), (int)r.label.size(), &extent);
    if (name.left + extent.cx + S(12) < name.right) {
        name.left += extent.cx + S(12);
        SetTextColor(dc, sub);
        DrawTextW(dc, r.path.c_str(), -1, &name, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    const bool capturing = e.capture >= 0 && r.name == e.capturing;
    std::wstring keys[2] = {Joined(keymap::KeysOf(e.window, r.name)), Joined(keymap::KeysOf(e.global, r.name))};
    if (capturing) keys[e.capture] = L"Press a key\x2026";
    for (int col = 0; col < 2; ++col) {
        RECT cell{rc.right - (2 - col) * keysW, rc.top, rc.right - (1 - col) * keysW - S(8), rc.bottom};
        SetTextColor(dc, capturing && col == e.capture ? Ref(selected ? P().selectText : P().accent) : main);
        DrawTextW(dc, keys[col].c_str(), -1, &cell, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    SelectObject(dc, old);
}

void Layout(HWND h) {
    RECT rc;
    GetClientRect(h, &rc);
    const int pad = S(10), w = rc.right - pad * 2, buttonH = S(28), hintH = S(40), headerH = S(22);
    MoveWindow(e.search, pad, pad, w, S(26), TRUE);
    const int listTop = pad + S(26) + pad + headerH;
    const int buttonsTop = rc.bottom - pad - buttonH, hintTop = buttonsTop - pad / 2 - hintH;
    MoveWindow(e.list, pad, listTop, w, std::max(S(60), hintTop - pad / 2 - listTop), TRUE);
    MoveWindow(e.hint, pad, hintTop, w, hintH, TRUE);
    int x = pad;
    const int widths[5] = {S(130), S(170), S(90), S(90), S(80)};
    for (int i = 0; i < 4; ++i) {
        MoveWindow(e.buttons[i], x, buttonsTop, widths[i], buttonH, TRUE);
        x += widths[i] + S(6);
    }
    MoveWindow(e.buttons[4], rc.right - pad - widths[4], buttonsTop, widths[4], buttonH, TRUE);
    InvalidateRect(h, nullptr, TRUE);
}

LRESULT CALLBACK ListSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (e.capture >= 0 && (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)) {
        Captured((UINT)wp, lp);
        return 0;
    }
    if (e.capture >= 0 && (msg == WM_CHAR || msg == WM_SYSCHAR || msg == WM_KEYUP || msg == WM_SYSKEYUP)) return 0;
    if (e.capture >= 0 && msg == WM_KILLFOCUS) {  // clicking elsewhere gives up
        EndCapture();
        DefaultHint();
    }
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        StartCapture(GetKeyState(VK_SHIFT) < 0 ? Scope::Global : Scope::Window);
        return 0;
    }
    if (msg == WM_KEYDOWN && (wp == VK_DELETE || wp == VK_BACK)) {
        ClearSelected();
        return 0;
    }
    if (msg == WM_GETDLGCODE && e.capture >= 0) return DLGC_WANTALLKEYS;
    if (msg == WM_GETDLGCODE && lp && reinterpret_cast<MSG*>(lp)->message == WM_KEYDOWN &&
        reinterpret_cast<MSG*>(lp)->wParam == VK_RETURN)
        return DefSubclassProc(h, msg, wp, lp) | DLGC_WANTMESSAGE;  // Enter sets a key, not the dialog's OK
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(h, ListSubclass, 0);
    return DefSubclassProc(h, msg, wp, lp);
}

// The search box sends the arrows to the list, so a command is found and chosen without the mouse.
LRESULT CALLBACK SearchSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_KEYDOWN && (wp == VK_UP || wp == VK_DOWN || wp == VK_PRIOR || wp == VK_NEXT)) return SendMessageW(e.list, msg, wp, lp);
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        StartCapture(GetKeyState(VK_SHIFT) < 0 ? Scope::Global : Scope::Window);
        return 0;
    }
    if (msg == WM_CHAR && wp == L'\r') return 0;
    if (msg == WM_GETDLGCODE && lp && reinterpret_cast<MSG*>(lp)->message == WM_KEYDOWN &&
        reinterpret_cast<MSG*>(lp)->wParam == VK_RETURN)
        return DefSubclassProc(h, msg, wp, lp) | DLGC_WANTMESSAGE;
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(h, SearchSubclass, 0);
    return DefSubclassProc(h, msg, wp, lp);
}

LRESULT CALLBACK EditorProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            e.hwnd = h;
            LOGFONTW font{};
            GetObjectW(g.font, sizeof(font), &font);
            e.font = CreateFontIndirectW(&font);
            auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
                HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, h,
                                         (HMENU)(INT_PTR)id, g.inst, nullptr);
                SendMessageW(c, WM_SETFONT, (WPARAM)e.font, TRUE);
                return c;
            };
            e.search = make(WC_EDITW, L"", ES_AUTOHSCROLL | WS_TABSTOP, kIdSearch);
            SendMessageW(e.search, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search commands");
            SetWindowTheme(e.search, g.dark ? L"DarkMode_CFD" : L"Explorer", nullptr);
            SetWindowSubclass(e.search, SearchSubclass, 0, 0);
            e.list = make(WC_LISTBOXW, L"Commands", WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY | LBS_OWNERDRAWFIXED |
                          LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT, kIdList);
            ApplyControlTheme(e.list, g.dark);
            SetWindowSubclass(e.list, ListSubclass, 0, 0);
            e.hint = make(WC_STATICW, L"", SS_LEFT | SS_NOPREFIX, kIdHint);
            const wchar_t* labels[5] = {L"Set &in cordinal", L"Set from &any program", L"&Clear", L"&Reset all", L"Close"};
            const int ids[5] = {kIdSetWindow, kIdSetGlobal, kIdClear, kIdReset, kIdClose};
            for (int i = 0; i < 5; ++i) {
                e.buttons[i] = make(WC_BUTTONW, labels[i], BS_PUSHBUTTON | WS_TABSTOP, ids[i]);
                ApplyControlTheme(e.buttons[i], g.dark);
            }
            // Every command of the menu bar, and the ones only keys and the palette reach.
            for (const MenuCommandInfo& c : AllMenuCommands()) {
                std::wstring path = c.path;
                std::replace(path.begin(), path.end(), L'\x203A', L'>');
                e.rows.push_back({c.label, c.path, path.empty() ? c.label : path + L" > " + c.label});
            }
            DefaultHint();
            Refresh();
            Filter();
            return 0;
        }
        case WM_SIZE: Layout(h); return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {S(560), S(360)};
            return 0;
        case WM_MEASUREITEM:
            reinterpret_cast<MEASUREITEMSTRUCT*>(lp)->itemHeight = S(26);
            return TRUE;
        case WM_DRAWITEM:
            DrawRow(reinterpret_cast<DRAWITEMSTRUCT*>(lp));
            return TRUE;
        case WM_PAINT: {  // the two key columns' headings over the list
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT list;
            GetWindowRect(e.list, &list);
            MapWindowPoints(nullptr, h, (POINT*)&list, 2);
            const int width = list.right - list.left, keysW = width * 22 / 100, top = list.top - S(22);
            HGDIOBJ old = SelectObject(dc, e.font);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, Ref(P().sub));
            RECT cells[3] = {{list.left + S(10), top, list.right - 2 * keysW, list.top},
                             {list.right - 2 * keysW, top, list.right - keysW, list.top},
                             {list.right - keysW, top, list.right, list.top}};
            const wchar_t* heads[3] = {L"Command", L"In cordinal", L"From any program"};
            for (int i = 0; i < 3; ++i) DrawTextW(dc, heads[i], -1, &cells[i], DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            SelectObject(dc, old);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_ERASEBKGND: {
            RECT r;
            GetClientRect(h, &r);
            FillRect((HDC)wp, &r, g.surfaceBrush);
            return 1;
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            return FieldControlColor((HDC)wp);
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            return SurfaceControlColor((HDC)wp, TextColor());
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case kIdSearch: if (HIWORD(wp) == EN_CHANGE) Filter(); return 0;
                case kIdList: if (HIWORD(wp) == LBN_DBLCLK) StartCapture(Scope::Window); return 0;
                case kIdSetWindow: StartCapture(Scope::Window); return 0;
                case kIdSetGlobal: StartCapture(Scope::Global); return 0;
                case kIdClear: ClearSelected(); return 0;
                case kIdReset: ResetEverything(); return 0;
                case kIdClose: case IDCANCEL: DestroyWindow(h); return 0;
            }
            return 0;
        case WM_CLOSE: DestroyWindow(h); return 0;
        case WM_DESTROY:
            EndCapture();
            if (e.font) DeleteObject(e.font);
            return 0;
        case WM_NCDESTROY:
            e = Editor{};
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

void ShowKeyboardShortcuts() {
    if (e.hwnd) {
        SetForegroundWindow(e.hwnd);
        return;
    }
    RegisterAppClass(L"CordinalKeys", EditorProc);
    HWND owner = g.hwnd;
    RECT parent{};
    GetWindowRect(owner, &parent);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &mi);
    const int w = std::min(S(760), (int)(mi.rcWork.right - mi.rcWork.left)),
              h = std::min(S(560), (int)(mi.rcWork.bottom - mi.rcWork.top));
    int x = std::clamp((int)(parent.left + parent.right - w) / 2, (int)mi.rcWork.left, (int)mi.rcWork.right - w);
    int y = std::clamp((int)(parent.top + parent.bottom - h) / 2, (int)mi.rcWork.top, (int)mi.rcWork.bottom - h);
    HWND window = CreateWindowExW(WS_EX_CONTROLPARENT, L"CordinalKeys", L"cordinal \x00B7 Keyboard shortcuts",
                                  WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX | WS_CLIPCHILDREN, x, y, w, h, owner, nullptr,
                                  g.inst, nullptr);
    if (!window) return;
    ShowWindow(window, SW_SHOW);
    SetFocus(e.search);
}

// Keys go to the window, never to the main window's accelerators, and a key being assigned reaches the
// list before IsDialogMessage can take Tab or Esc.
bool KeyboardShortcutsDialogMessage(MSG* msg) {
    if (!e.hwnd || (msg->hwnd != e.hwnd && !IsChild(e.hwnd, msg->hwnd))) return false;
    if (e.capture >= 0 && msg->hwnd == e.list && msg->message >= WM_KEYFIRST && msg->message <= WM_KEYLAST) {
        DispatchMessageW(msg);
        return true;
    }
    if (IsDialogMessageW(e.hwnd, msg)) return true;
    TranslateMessage(msg);
    DispatchMessageW(msg);
    return true;
}

}  // namespace app
