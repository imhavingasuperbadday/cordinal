// Your own keyboard shortcuts (keymap.h): key names, the rules for where a key may be bound, cordinal's
// own keys from any program, and laying your changes over cordinal's own bindings. Portable: the Windows
// and Linux players share it and translate the names into their own key codes.
#include "keymap.h"

#include <algorithm>
#include <cwctype>

namespace keymap {

namespace {

// Spellings a key may be written in, lowercased without spaces, and the canonical name each means.
struct Alias {
    const wchar_t* spelling;
    const wchar_t* name;
};
constexpr Alias kAliases[] = {
    {L"space", L"Space"}, {L"spacebar", L"Space"}, {L"enter", L"Enter"}, {L"return", L"Enter"}, {L"tab", L"Tab"},
    {L"esc", L"Esc"}, {L"escape", L"Esc"}, {L"backspace", L"Backspace"}, {L"delete", L"Delete"}, {L"del", L"Delete"},
    {L"insert", L"Insert"}, {L"ins", L"Insert"}, {L"home", L"Home"}, {L"end", L"End"}, {L"pageup", L"Page Up"},
    {L"pgup", L"Page Up"}, {L"prior", L"Page Up"}, {L"pagedown", L"Page Down"}, {L"pgdn", L"Page Down"},
    {L"next", L"Page Down"}, {L"up", L"Up"}, {L"down", L"Down"}, {L"left", L"Left"}, {L"right", L"Right"},
    {L"pause", L"Pause"}, {L"break", L"Pause"},
    // The punctuation keys of a US layout, by name: '=' and ';' can't be ini keys.
    {L"=", L"Equals"}, {L"equals", L"Equals"}, {L"equal", L"Equals"}, {L"plus", L"Equals"}, {L"+", L"Equals"},
    {L"-", L"Minus"}, {L"minus", L"Minus"}, {L",", L"Comma"}, {L"comma", L"Comma"}, {L".", L"Period"},
    {L"period", L"Period"}, {L"/", L"Slash"}, {L"slash", L"Slash"}, {L";", L"Semicolon"}, {L"semicolon", L"Semicolon"},
    {L"'", L"Quote"}, {L"quote", L"Quote"}, {L"`", L"Backquote"}, {L"backquote", L"Backquote"}, {L"grave", L"Backquote"},
    {L"[", L"Left Bracket"}, {L"leftbracket", L"Left Bracket"}, {L"]", L"Right Bracket"},
    {L"rightbracket", L"Right Bracket"}, {L"\\", L"Backslash"}, {L"backslash", L"Backslash"},
    // Media keys.
    {L"playpause", L"Play/Pause"}, {L"play/pause", L"Play/Pause"}, {L"mediaplaypause", L"Play/Pause"},
    {L"nexttrack", L"Next Track"}, {L"medianext", L"Next Track"}, {L"previoustrack", L"Previous Track"},
    {L"mediaprevious", L"Previous Track"}, {L"mediaprev", L"Previous Track"}, {L"stopmedia", L"Stop Media"},
    {L"mediastop", L"Stop Media"}, {L"volumeup", L"Volume Up"}, {L"volumedown", L"Volume Down"},
    {L"volumemute", L"Mute"}, {L"mute", L"Mute"},
};

constexpr const wchar_t* kNumpad[kNumpadKeys] = {L"Num 0", L"Num 1", L"Num 2", L"Num 3", L"Num 4", L"Num 5",
                                                  L"Num 6", L"Num 7", L"Num 8", L"Num 9", L"Num .", L"Num +",
                                                  L"Num -", L"Num *", L"Num /", L"Num Enter"};

std::wstring Squeeze(std::wstring_view s) {
    std::wstring out;
    for (wchar_t c : s)
        if (c != L' ' && c != L'_') out += (wchar_t)towlower(c);  // ini readers turn spaces in a key into '_'
    return out;
}

// The canonical name of one key written without modifiers, or empty.
std::wstring KeyName(std::wstring_view part) {
    std::wstring k = Squeeze(part);
    if (k.empty()) return {};
    if (k.size() == 1 && iswalnum(k[0])) return std::wstring(1, (wchar_t)towupper(k[0]));
    if (k.size() >= 2 && k.size() <= 3 && k[0] == L'f' && iswdigit(k[1]) && (k.size() == 2 || iswdigit(k[2]))) {
        int n = std::stoi(k.substr(1));
        if (n >= 1 && n <= 24) return L"F" + std::to_wstring(n);
        return {};
    }
    for (const wchar_t* prefix : {L"numpad", L"num", L"kp"}) {
        std::wstring_view p = prefix;
        if (!k.starts_with(p) || k.size() == p.size()) continue;
        std::wstring rest = k.substr(p.size());
        if (rest == L"enter" || rest == L"return") return L"Num Enter";
        if (rest == L"plus" || rest == L"add") rest = L"+";
        else if (rest == L"minus" || rest == L"subtract") rest = L"-";
        else if (rest == L"multiply" || rest == L"star") rest = L"*";
        else if (rest == L"divide" || rest == L"slash") rest = L"/";
        else if (rest == L"period" || rest == L"decimal" || rest == L"dot" || rest == L"del") rest = L".";
        for (int i = 0; i < kNumpadKeys; ++i)
            if (rest == std::wstring(kNumpad[i] + 4)) return kNumpad[i];
        return {};
    }
    for (const Alias& a : kAliases)
        if (k == a.spelling) return a.name;
    return {};
}

bool IsFunctionKey(const std::wstring& name) { return name.size() >= 2 && name[0] == L'F' && iswdigit(name[1]); }

bool IsMediaKey(const std::wstring& name) {
    for (const wchar_t* m : {L"Play/Pause", L"Next Track", L"Previous Track", L"Stop Media", L"Volume Up", L"Volume Down", L"Mute"})
        if (name == m) return true;
    return false;
}

}  // namespace

bool Parse(std::wstring_view text, Key& out) {
    out = {};
    size_t at = 0;
    for (;;) {
        size_t plus = text.find(L'+', at);
        // "Ctrl++" and "Num +": a '+' that is the last character, or right after "num", is the key.
        if (plus != std::wstring_view::npos && plus + 1 == text.size()) plus = std::wstring_view::npos;
        if (plus != std::wstring_view::npos) {
            std::wstring before = Squeeze(text.substr(at, plus - at));
            if (before == L"num" || before == L"numpad" || before == L"kp") plus = std::wstring_view::npos;
        }
        std::wstring_view part = text.substr(at, plus == std::wstring_view::npos ? std::wstring_view::npos : plus - at);
        if (plus == std::wstring_view::npos) {
            out.name = KeyName(part);
            return !out.name.empty();
        }
        std::wstring m = Squeeze(part);
        if (m == L"ctrl" || m == L"control") out.mods |= kCtrl;
        else if (m == L"alt") out.mods |= kAlt;
        else if (m == L"shift") out.mods |= kShift;
        else if (m == L"win" || m == L"super" || m == L"logo" || m == L"meta") out.mods |= kWin;
        else return false;
        at = plus + 1;
    }
}

std::wstring Text(const Key& key) {
    if (key.name.empty()) return {};
    std::wstring s;
    if (key.mods & kWin) s += L"Win+";
    if (key.mods & kCtrl) s += L"Ctrl+";
    if (key.mods & kAlt) s += L"Alt+";
    if (key.mods & kShift) s += L"Shift+";
    return s + key.name;
}

int NumpadIndex(const Key& key) {
    if (key.mods) return -1;
    for (int i = 0; i < kNumpadKeys; ++i)
        if (key.name == kNumpad[i]) return i;
    return -1;
}

const wchar_t* NumpadName(int index) { return index >= 0 && index < kNumpadKeys ? kNumpad[index] : L""; }

bool Allowed(Scope scope, const Key& key, std::wstring* why) {
    auto no = [&](const wchar_t* reason) {
        if (why) *why = reason;
        return false;
    };
    if (key.name.empty()) return no(L"That isn't a key cordinal knows.");
    if (scope == Scope::Window && (key.mods & kWin))
        return no(L"Win keys belong to the desktop; give them to cordinal from any program instead.");
    const bool modified = key.mods & (kCtrl | kAlt | kWin);
    const bool numpad = key.name.starts_with(L"Num ");
    if (modified || IsFunctionKey(key.name) || IsMediaKey(key.name) || key.name == L"Pause") return true;
    if (scope == Scope::Global && numpad) return true;
    if (numpad) return no(L"A numpad key on its own works from any program, in Lightweight mode; in cordinal it "
                          L"needs Ctrl, Alt or Win.");
    return no(L"A key that types needs Ctrl, Alt or Win, so typing in a search box still works.");
}

std::vector<Binding> DefaultGlobal() {
    const struct { const wchar_t* key; const wchar_t* command; } defaults[] = {
        {L"Num 5", L"Playback > Play / Pause"},  {L"Num 4", L"Playback > Previous"},
        {L"Num 6", L"Playback > Next"},          {L"Num 8", L"Playback > Volume up"},
        {L"Num 2", L"Playback > Volume down"},   {L"Num 7", L"Playback > Seek back 10 seconds"},
        {L"Num 9", L"Playback > Seek forward 10 seconds"}, {L"Num 1", L"Playback > Mute"},
        {L"Num 3", L"Playback > Shuffle"},       {L"Num 0", L"Playback > Play a song..."},
        {L"Num .", L"View > Lightweight mode"},  {L"Win+Alt+Space", L"Playback > Play a song..."},
    };
    std::vector<Binding> out;
    for (const auto& d : defaults) {
        Binding b;
        b.scope = Scope::Global;
        Parse(d.key, b.key);
        b.command = d.command;
        out.push_back(std::move(b));
    }
    return out;
}

std::vector<Binding> Effective(const std::vector<Binding>& defaults, const std::vector<Binding>& changes, Scope scope) {
    std::vector<Binding> out;
    for (const Binding& b : defaults)
        if (b.scope == scope) out.push_back(b);
    for (const Binding& c : changes) {
        if (c.scope != scope) continue;
        std::erase_if(out, [&](const Binding& b) { return b.key == c.key; });
        if (!c.command.empty()) out.push_back(c);
    }
    return out;
}

std::wstring Assign(std::vector<Binding>& changes, const std::vector<Binding>& defaults, Scope scope, const Key& key,
                    const std::wstring& command) {
    std::wstring before;
    for (const Binding& b : Effective(defaults, changes, scope))
        if (b.key == key && !SameCommand(b.command, command)) before = b.command;
    std::erase_if(changes, [&](const Binding& c) { return c.scope == scope && c.key == key; });
    // A change that only restores cordinal's own binding (or clears a key it never had) is no change.
    const auto own = std::find_if(defaults.begin(), defaults.end(),
                                  [&](const Binding& d) { return d.scope == scope && d.key == key; });
    const bool same = own != defaults.end() ? SameCommand(own->command, command) : command.empty();
    if (!same) changes.push_back({scope, key, command});
    return before;
}

std::vector<Key> KeysOf(const std::vector<Binding>& effective, const std::wstring& command) {
    std::vector<Key> keys;
    for (const Binding& b : effective)
        if (SameCommand(b.command, command)) keys.push_back(b.key);
    return keys;
}

std::wstring CommandKey(std::wstring_view s) {
    std::wstring out;
    for (wchar_t ch : s) {
        wchar_t c = ch == L'\x203A' ? L'>' : (wchar_t)towlower(ch);
        if (c == L'&' || c == L'.') continue;
        if (c == L' ' && (out.empty() || out.back() == L' ' || out.back() == L'>')) continue;
        if (c == L'>' && !out.empty() && out.back() == L' ') out.pop_back();
        out += c;
    }
    while (!out.empty() && out.back() == L' ') out.pop_back();
    return out;
}

bool SameCommand(std::wstring_view a, std::wstring_view b) { return CommandKey(a) == CommandKey(b); }

}  // namespace keymap
