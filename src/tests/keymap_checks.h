// Checks of your own keyboard shortcuts' shared rules (keymap.h) that need no window: key names, where a
// key may be bound, cordinal's own keys from any program, and laying changes over them. The Windows
// self-test (selftest_preferences.cpp, suite "ui") and the Linux core test (core_test.cpp, "keymap") both
// run them, so the two players read and write the same bindings.
#pragma once
#include "keymap.h"

#include <string>
#include <vector>

namespace keymap_checks {

struct Group {
    std::string name;
    std::vector<std::string> failures;
    bool Passed() const { return failures.empty(); }
};

inline void Add(Group& group, bool ok, const char* what) {
    if (!ok) group.failures.push_back(what);
}

inline std::wstring Canon(const wchar_t* text) {
    keymap::Key key;
    return keymap::Parse(text, key) ? keymap::Text(key) : L"(none)";
}

inline std::vector<Group> All() {
    using namespace keymap;
    std::vector<Group> groups;

    Group names{"key names read and write one way"};
    Add(names, Canon(L"ctrl+shift+k") == L"Ctrl+Shift+K", "modifiers and letters are capitalised");
    Add(names, Canon(L"Shift+Alt+Ctrl+Win+F5") == L"Win+Ctrl+Alt+Shift+F5", "modifiers take one order");
    Add(names, Canon(L"Ctrl+=") == L"Ctrl+Equals" && Canon(L"Ctrl++") == L"Ctrl+Equals", "'=' and '+' are the Equals key");
    Add(names, Canon(L"Numpad5") == L"Num 5" && Canon(L"num 5") == L"Num 5" && Canon(L"KP_5") == L"Num 5",
        "numpad spellings agree");
    Add(names, Canon(L"Num +") == L"Num +" && Canon(L"Ctrl+Num +") == L"Ctrl+Num +" && Canon(L"Num Enter") == L"Num Enter",
        "the numpad's plus and Enter");
    Add(names, Canon(L"Num.") == L"Num ." && Canon(L"NumDecimal") == L"Num .", "the numpad's point");
    Add(names, Canon(L"Page_Up") == L"Page Up" && Canon(L"pgdn") == L"Page Down", "an ini key's '_' is a space");
    Add(names, Canon(L"Win+Alt+Space") == L"Win+Alt+Space" && Canon(L"super+alt+space") == L"Win+Alt+Space",
        "Win has the desktop's other names");
    Add(names, Canon(L"Ctrl+Hyper+K") == L"(none)" && Canon(L"Ctrl+") == L"(none)" && Canon(L"F25") == L"(none)",
        "nonsense is refused");
    Add(names, Canon(L"F12") == L"F12" && Canon(L"playpause") == L"Play/Pause", "function and media keys");
    for (const Binding& b : DefaultGlobal())
        Add(names, Text(b.key).find(L'=') == std::wstring::npos && Text(b.key).find(L';') == std::wstring::npos,
            "no key name could break an ini file");
    groups.push_back(std::move(names));

    Group rules{"keys are allowed only where they leave typing alone"};
    auto allowed = [](Scope scope, const wchar_t* text) {
        Key key;
        return Parse(text, key) && Allowed(scope, key);
    };
    Add(rules, !allowed(Scope::Window, L"K") && !allowed(Scope::Global, L"K"), "a bare letter is never taken");
    Add(rules, allowed(Scope::Window, L"Ctrl+K") && allowed(Scope::Window, L"F5") && allowed(Scope::Window, L"Play/Pause"),
        "modified, function and media keys work in cordinal");
    Add(rules, !allowed(Scope::Window, L"Num 5") && allowed(Scope::Global, L"Num 5"),
        "a bare numpad key is Lightweight mode's, from any program");
    Add(rules, !allowed(Scope::Window, L"Win+K") && allowed(Scope::Global, L"Win+K"), "Win keys only from any program");
    groups.push_back(std::move(rules));

    Group layers{"your changes lie over cordinal's own keys"};
    const std::vector<Binding> defaults = DefaultGlobal();
    std::vector<Binding> changes;
    Key num5, ctrlAltRight, winAltSpace;
    Parse(L"Num 5", num5);
    Parse(L"Ctrl+Alt+Right", ctrlAltRight);
    Parse(L"Win+Alt+Space", winAltSpace);
    auto effective = [&] { return Effective(defaults, changes, Scope::Global); };
    Add(layers, KeysOf(effective(), L"Playback > Play / Pause") == std::vector<Key>{num5}, "numpad 5 plays and pauses");
    Add(layers, KeysOf(effective(), L"playback>play a song").size() == 2, "the picker has numpad 0 and Win+Alt+Space");
    std::wstring moved = Assign(changes, defaults, Scope::Global, num5, L"Playback > Next");
    Add(layers, SameCommand(moved, L"Playback > Play / Pause") && KeysOf(effective(), L"Playback > Play / Pause").empty() &&
                    KeysOf(effective(), L"Playback > Next").size() == 2,
        "a key given to another command moves, and says where from");
    Assign(changes, defaults, Scope::Global, ctrlAltRight, L"Playback > Next");
    Assign(changes, defaults, Scope::Global, winAltSpace, L"");
    Add(layers, KeysOf(effective(), L"Playback > Next").size() == 3 && KeysOf(effective(), L"Playback > Play a song...").size() == 1,
        "keys add up, and clearing one of cordinal's own removes it");
    Add(layers, changes.size() == 3 && Effective(defaults, changes, Scope::Window).empty(), "only changes are kept, per scope");
    Assign(changes, defaults, Scope::Global, num5, L"Playback > Play / Pause");
    Assign(changes, defaults, Scope::Global, winAltSpace, L"Playback > Play a song...");
    Add(layers, changes.size() == 1 && changes[0].key == ctrlAltRight,
        "putting a key back as cordinal had it leaves no change behind");
    ResetAll(changes);
    Add(layers, effective() == defaults, "Reset all brings back cordinal's own keys");
    groups.push_back(std::move(layers));
    return groups;
}

}  // namespace keymap_checks
