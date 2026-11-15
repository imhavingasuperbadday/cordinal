// Your own keyboard shortcuts, shared by the Windows and Linux players: key names and how they are
// written, which keys may be used where, and the bindings you changed from cordinal's own. Each player
// turns a binding into its native form (an accelerator table or SDL keys for "in cordinal", a
// RegisterHotKey, the numpad hook or the desktop shortcut portal for "from any program").
//
// A binding names a command as the command palette does ("Playback > Next"), so it survives new
// menus and ids. Only changes are kept: an empty command removes one of cordinal's own keys.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace keymap {

enum Mod : uint8_t { kWin = 1, kCtrl = 2, kAlt = 4, kShift = 8 };

// A key and its modifiers. `name` is the key's canonical name: "K", "7", "F5", "Space", "Page Up",
// "Equals", "Num 5", "Num Enter", "Play/Pause"... Names never hold '=' or ';', so they can be the
// keys of an ini file.
struct Key {
    uint8_t mods = 0;
    std::wstring name;
    bool operator==(const Key&) const = default;
};

// "ctrl+shift+k", "Num5", "numpad 5", "Ctrl+=" -> the canonical key; false for anything else.
bool Parse(std::wstring_view text, Key& out);
// "Win+Ctrl+Alt+Shift+K", modifiers in that order; empty for no key.
std::wstring Text(const Key& key);

// The numpad's keys, in order: Num 0..9, Num ., Num +, Num -, Num *, Num /, Num Enter.
constexpr int kNumpadKeys = 16;
int NumpadIndex(const Key& key);  // -1 unless it is a numpad key with no modifiers
const wchar_t* NumpadName(int index);
// The first eleven change with Num Lock (the digits, or the arrows and Home printed beside them);
// the operators and Enter are the same either way.
inline bool NumLockKey(int index) { return index >= 0 && index <= 10; }

enum class Scope : uint8_t { Window, Global };
// Whether `key` may be bound there. In cordinal a key without Ctrl or Alt must be one nothing types
// (F1-F24, the media keys, Pause), so the search box keeps its letters, and Win keys are the desktop's;
// from any program Win may be used, and the numpad's bare keys too (Lightweight mode's remote), while
// letters still need a modifier.
bool Allowed(Scope scope, const Key& key, std::wstring* why = nullptr);

struct Binding {
    Scope scope = Scope::Window;
    Key key;
    std::wstring command;  // "Playback > Next"; empty removes cordinal's own binding of `key`
    bool operator==(const Binding&) const = default;
};

// cordinal's own keys from any program: the numpad remote and the song picker. ("In cordinal" keys
// come from each player's accelerator table.)
std::vector<Binding> DefaultGlobal();

// The bindings in effect for a scope: `defaults` with `changes` laid over them, a change replacing
// whatever had its key. Changes whose command is empty only remove.
std::vector<Binding> Effective(const std::vector<Binding>& defaults, const std::vector<Binding>& changes, Scope scope);

// Sets `key` in `scope` to `command` among `changes` (an empty command clears it), dropping an earlier
// change of the same key. Returns the command `key` had before in the effective bindings, if another.
std::wstring Assign(std::vector<Binding>& changes, const std::vector<Binding>& defaults, Scope scope, const Key& key,
                    const std::wstring& command);
// Removes every change, so cordinal's own keys come back.
inline void ResetAll(std::vector<Binding>& changes) { changes.clear(); }

// The keys a command has in a scope, in the order they were bound.
std::vector<Key> KeysOf(const std::vector<Binding>& effective, const std::wstring& command);

// Command names compare as the palette does: case, '&', dots and the spacing around '>' don't matter.
std::wstring CommandKey(std::wstring_view name);
bool SameCommand(std::wstring_view a, std::wstring_view b);

}  // namespace keymap
