// Your own keyboard shortcuts on Windows (app_keys.cpp, File > Keyboard shortcuts): the changes you made
// to cordinal's keys (keymap.h), applied to the accelerator table ("in cordinal"), to RegisterHotKey and
// Lightweight mode's numpad hook ("from any program"), and to the shortcuts the menus show.
#pragma once
#include "app_state.h"
#include "keymap.h"

#include <span>

namespace app {

// Commands a key can run that no menu holds, listed by the palette and the shortcut window beside the
// menu bar's: their menu path, name and id.
struct ExtraCommand {
    int command;
    const wchar_t* path;
    const wchar_t* label;
};
std::span<const ExtraCommand> ExtraCommands();

// cordinal's own keys in its window, before packs and your changes: BuildAccelerators' table.
std::vector<ACCEL> BuiltInAccelerators();
// Your changes, from settings.ini's [Keys] and [Global keys], and written back there.
const std::vector<keymap::Binding>& KeyChanges();
void SetKeyChanges(std::vector<keymap::Binding> changes);  // saves, then rebuilds the table and the keys
// cordinal's own bindings as keymap names: the accelerator table's (in cordinal) and DefaultGlobal().
std::vector<keymap::Binding> DefaultBindings();
std::vector<keymap::Binding> EffectiveBindings(keymap::Scope scope);
// Lays your "in cordinal" changes over `keys` (BuildAccelerators, after the packs').
void ApplyKeyChanges(std::vector<ACCEL>& keys);
// "From any program": RegisterHotKey for each key with a modifier (or a key nothing types), always; the
// numpad's bare keys go to Lightweight mode's hook. The keys another program holds are reported.
void RegisterGlobalKeys();
void UnregisterGlobalKeys();
bool GlobalKeyHotkey(int id);                    // WM_HOTKEY: true when it was one of yours
const std::vector<std::wstring>& TakenGlobalKeys();
uint32_t NumpadKeysBound();                      // bit per keymap numpad index with a command
void NumpadKeyPressed(int index, bool repeat);   // the hook's WM_APP_KEYS
// The menus show your keys: each item's text after its tab follows your changes.
void ApplyShortcutLabels(HMENU menu);

// Windows key codes and keymap names. `extended` is the keyboard's flag that tells the arrow block
// from the numpad with Num Lock off.
bool KeyFromVk(UINT vk, bool extended, uint8_t mods, keymap::Key& out);
UINT VkOf(const keymap::Key& key);  // the code with Num Lock on for numpad keys; 0 for none

// Volume and seeking steps shared by the keys, the numpad and the mini player's wheel.
void StepVolume(bool up);
// The first key from any program that runs `command` and isn't the numpad's, for a menu to show.
std::wstring GlobalKeyText(int command);

void ShowKeyboardShortcuts();
bool KeyboardShortcutsDialogMessage(MSG* msg);

}  // namespace app
