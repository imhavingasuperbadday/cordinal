// Your own keyboard shortcuts on Linux (File > Keyboard shortcuts), as the Windows build's app_keys.cpp. The
// rules and names are keymap.h's; this file reads SDL's keys as keymap names, runs your "in cordinal" keys
// before cordinal's own, gives Lightweight mode's numpad its commands, and lists cordinal's own keys (the
// menus' shortcuts and keymap::DefaultGlobal) so the editor (panel_keys.cpp) can show and change them. Keys
// "from any program" other than the numpad's reach the desktop's shortcut portal through packs_ui.cpp.
#include "ui_internal.h"

namespace lx {

namespace {

using keymap::Binding;
using keymap::Key;
using keymap::Scope;

struct Named {
    SDL_Keycode key;
    const wchar_t* name;
};
const Named kNamed[] = {
    {SDLK_SPACE, L"Space"}, {SDLK_RETURN, L"Enter"}, {SDLK_TAB, L"Tab"}, {SDLK_ESCAPE, L"Esc"},
    {SDLK_BACKSPACE, L"Backspace"}, {SDLK_DELETE, L"Delete"}, {SDLK_INSERT, L"Insert"}, {SDLK_HOME, L"Home"},
    {SDLK_END, L"End"}, {SDLK_PAGEUP, L"Page Up"}, {SDLK_PAGEDOWN, L"Page Down"}, {SDLK_UP, L"Up"},
    {SDLK_DOWN, L"Down"}, {SDLK_LEFT, L"Left"}, {SDLK_RIGHT, L"Right"}, {SDLK_EQUALS, L"Equals"},
    {SDLK_MINUS, L"Minus"}, {SDLK_COMMA, L"Comma"}, {SDLK_PERIOD, L"Period"}, {'/', L"Slash"}, {';', L"Semicolon"},
    {'\'', L"Quote"}, {'`', L"Backquote"}, {SDLK_LEFTBRACKET, L"Left Bracket"}, {SDLK_RIGHTBRACKET, L"Right Bracket"},
    {'\\', L"Backslash"}, {SDLK_MEDIA_PLAY_PAUSE, L"Play/Pause"}, {SDLK_MEDIA_NEXT_TRACK, L"Next Track"},
    {SDLK_MEDIA_PREVIOUS_TRACK, L"Previous Track"}, {SDLK_MEDIA_STOP, L"Stop Media"}, {SDLK_VOLUMEUP, L"Volume Up"},
    {SDLK_VOLUMEDOWN, L"Volume Down"}, {SDLK_MUTE, L"Mute"},
};

// A menu item's name as bindings store it: "Playback > Next".
std::wstring NameOf(const PaletteCommand& c) {
    std::wstring label = compat::Widen(c.label.c_str()), path = compat::Widen(c.path.c_str());
    return path.empty() ? label : path + L" > " + label;
}

bool Repeats(const std::wstring& command) {
    for (const wchar_t* name : {L"Playback > Volume up", L"Playback > Volume down", L"Playback > Seek back 10 seconds",
                                L"Playback > Seek forward 10 seconds"})
        if (keymap::SameCommand(command, name)) return true;
    return false;
}

}  // namespace

void StepVolume(bool up) { SetVolume(std::clamp(g.settings.volume + (up ? 0.05f : -0.05f), 0.0f, 1.0f)); }

bool KeyFromEvent(const SDL_KeyboardEvent& e, keymap::Key& out) {
    out = {};
    out.mods = (e.mod & SDL_KMOD_GUI ? keymap::kWin : 0) | (e.mod & SDL_KMOD_CTRL ? keymap::kCtrl : 0) |
               (e.mod & SDL_KMOD_ALT ? keymap::kAlt : 0) | (e.mod & SDL_KMOD_SHIFT ? keymap::kShift : 0);
    // The numpad by where the key is, whatever Num Lock makes it type.
    const int sc = (int)e.scancode;
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) out.name = keymap::NumpadName(sc - SDL_SCANCODE_KP_1 + 1);
    else if (sc == SDL_SCANCODE_KP_0) out.name = keymap::NumpadName(0);
    else if (sc == SDL_SCANCODE_KP_PERIOD) out.name = keymap::NumpadName(10);
    else if (sc >= SDL_SCANCODE_KP_DIVIDE && sc <= SDL_SCANCODE_KP_ENTER) {
        static const int kOrder[] = {14, 13, 12, 11, 15};  // divide, multiply, minus, plus, Enter
        out.name = keymap::NumpadName(kOrder[sc - SDL_SCANCODE_KP_DIVIDE]);
    } else if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) out.name = L"F" + std::to_wstring(sc - SDL_SCANCODE_F1 + 1);
    else if (sc >= SDL_SCANCODE_F13 && sc <= SDL_SCANCODE_F24) out.name = L"F" + std::to_wstring(sc - SDL_SCANCODE_F13 + 13);
    else if (sc == SDL_SCANCODE_PAUSE) out.name = L"Pause";
    else if (e.key >= 'a' && e.key <= 'z') out.name = std::wstring(1, (wchar_t)(e.key - 'a' + 'A'));
    else if (e.key >= '0' && e.key <= '9') out.name = std::wstring(1, (wchar_t)e.key);
    else
        for (const Named& n : kNamed)
            if (e.key == n.key) out.name = n.name;
    return !out.name.empty();
}

std::vector<Binding> DefaultKeyBindings() {
    std::vector<Binding> out = keymap::DefaultGlobal();
    for (const PaletteCommand& c : MenuCommands()) {
        Binding b;
        b.scope = Scope::Window;
        if (c.shortcut.empty() || !keymap::Parse(compat::Widen(c.shortcut.c_str()), b.key)) continue;
        if (std::any_of(out.begin(), out.end(), [&](const Binding& o) { return o.scope == Scope::Window && o.key == b.key; }))
            continue;
        b.command = NameOf(c);
        out.push_back(std::move(b));
    }
    return out;
}

std::vector<Binding> EffectiveKeyBindings(Scope scope) {
    return keymap::Effective(DefaultKeyBindings(), g.settings.keyChanges, scope);
}

void SetKeyChanges(std::vector<Binding> changes) {
    g.settings.keyChanges = std::move(changes);
    SaveSettings();
    ApplyGlobalKeys();
    g.redraw = true;
}

bool RunCommandNamed(const std::wstring& name) {
    const auto commands = MenuCommands();  // the menus as they are now: an item may be off
    for (size_t i = 0; i < commands.size(); ++i)
        if (keymap::SameCommand(NameOf(commands[i]), name)) {
            if (commands[i].enabled) RunMenuCommand(i);
            return true;
        }
    return false;
}

// Your keys first, so one you gave another command wins over cordinal's own. A key you cleared does
// nothing; cordinal's keys that type (Space in the track list) can't be cleared this way.
bool UserKeyDown(const SDL_KeyboardEvent& e) {
    keymap::Key key;
    if (g.settings.keyChanges.empty() || !KeyFromEvent(e, key)) return false;
    for (const Binding& b : g.settings.keyChanges) {
        if (b.scope != Scope::Window || b.key != key) continue;
        if (b.command.empty()) return keymap::Allowed(Scope::Window, key);
        return RunCommandNamed(b.command);
    }
    return false;
}

// Lightweight mode's numpad (lightweight.cpp): the command its key has "from any program".
bool NumpadCommand(int index, bool repeat) {
    for (const Binding& b : EffectiveKeyBindings(Scope::Global)) {
        if (keymap::NumpadIndex(b.key) != index) continue;
        if (!repeat || Repeats(b.command)) RunCommandNamed(b.command);
        return true;
    }
    return false;
}

}  // namespace lx
