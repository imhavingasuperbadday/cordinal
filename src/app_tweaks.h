// The Tweaks registry and the command palette (docs/plugins.md, "Tier 0"). Every simple preference is
// described once: its settings.ini key, range, default and a sentence of help. LoadPreferences and
// SavePreferences go through the table, the Preferences window is built from it, and the palette
// searches it alongside every menu command. Preferences is the one home of every setting; the menus
// keep only the toggles changed while a song plays.
#pragma once
#include "app_state.h"

#include <span>
#include <string_view>

namespace app {

enum class TweakType : uint8_t { Bool, Choice, Float };
enum : uint8_t {
    kTweakRadio = 1,       // `command` is a radio base: the value is added to it
    kTweakLegacyLoad = 2,  // LoadPreferences reads it itself, to honour an older key or what goes with it
    kTweakOutside = 4,     // kept by Windows (the Run key), not in settings.ini: never loaded or saved here
};

struct Tweak {
    const char* key;         // stable id ("playback.resume"); never renamed, only aliased
    const wchar_t* section;  // where settings.ini keeps it, so existing profiles keep working
    const wchar_t* iniKey;
    const wchar_t* label;
    const wchar_t* help;     // one plain sentence: what it does
    TweakType type;
    float min, max, def;
    double (*get)();
    void (*put)(double);     // stores a loaded value, with no side effects
    // A live change goes through the menu command when there is one, so it answers exactly as the
    // menu does; otherwise `put` and then `apply` refresh what depends on it.
    int command = 0;
    void (*apply)() = nullptr;
    const wchar_t* const* choices = nullptr;  // Choice: max + 1 names
    const wchar_t* unit = nullptr;            // Float: shown after the value
    uint8_t flags = 0;
};

std::span<const Tweak> Tweaks();
// False for the settings of themes this build leaves out (theme_palette.h), which Preferences and the
// command palette then skip.
bool TweakShown(const Tweak& t);
const Tweak* FindTweak(std::string_view key);
// The Preferences page a tweak is listed on ("Effects").
const wchar_t* TweakPage(const Tweak& t);
bool TweakChanged(const Tweak& t);
std::wstring TweakText(const Tweak& t, double value);
void SetTweak(const Tweak& t, double value);
void LoadTweaks();
void SaveTweaks();
// Opens Preferences, on `key`'s page with that setting selected when one is given.
void ShowPreferences(const char* key = nullptr);
bool PreferencesDialogMessage(MSG* msg);
// The windows a Preferences page opens (Title formatting, Web server...), for the palette to list.
struct PreferenceLink {
    int command;
    const wchar_t* label;  // with its '&' mnemonic
    const wchar_t* page;
};
std::vector<PreferenceLink> PreferenceLinks();

// Command palette (app_palette.cpp): Ctrl+Shift+P, every menu bar command and every tweak.
void ShowCommandPalette();
// The menu bar command a name such as "Next" or "Playback > Next" names, as the palette shows it
// ('&' and "..." need not be typed): its id, 0 for none, -1 when two commands share the name.
int MenuCommandByName(const std::wstring& name);
bool PaletteDialogMessage(MSG* msg);
// Indices of `texts` that contain every word of `query` (case-insensitively), best first: a text
// that starts with the query, then one with a word starting with it, then the rest in their order.
std::vector<int> RankMatches(const std::vector<std::wstring>& texts, const std::wstring& query);

}  // namespace app
