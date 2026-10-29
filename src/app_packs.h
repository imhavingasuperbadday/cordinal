// Packs in the player (app_packs.cpp): the packs in <data>\packs, which ones are on, and what the ones
// that are on add to the window. Reading a pack is packs.h; this is where its values meet `g`.
#pragma once
#include "app_state.h"
#include "packs.h"

namespace app {

// Reads every pack and applies the ones that are on. Runs before the main window exists; `safe`
// (Shift held at start, or --safe) skips them all for this run.
void LoadPacks(bool safe);
// Re-reads the packs folder and applies the result to the running window.
void ReloadPacks();
// View > Packs (built fresh for each menu bar) and its commands.
HMENU PacksMenu();
bool PackCommand(int id);
// The pack themes in View > Theme, after the built-in ones.
void AppendPackThemes(HMENU themes);

// settings.ini keeps a pack theme by name, beside the built-in theme it falls back to.
ThemeMode ReadSavedTheme();
void WriteSavedTheme(ThemeMode mode);
// The built-in theme nearest to `mode`: itself, or Light / Dark for a pack theme.
ThemeMode BuiltInTheme(ThemeMode mode);

// Track list columns: the built-in ones (ui_spec.h), then the columns of the packs that are on.
int ColumnTotal();
const Column& ColumnAt(int type);
// A pack column's name in settings.ini ("pack/key"), and back; built-in columns have none.
std::wstring PackColumnName(int type);
int PackColumnType(const std::wstring& name);
std::wstring PackColumnTitle(int type);  // as a column set names it

// The group header look of the last pack that sets one, or null for cordinal's own.
const PackGroups* PackGroupLook();
// The pack that sets title format `slot` (PackFormatSlot), or null; and its pattern.
const Pack* PackFormatOwner(int slot);
// The user's own patterns from settings.ini, then the formats of the packs that are on.
void ApplyPackFormats();
// Keys from the packs that are on, resolved against the menu bar; they win over the built-in keys.
void AppendPackAccelerators(std::vector<ACCEL>& keys);
// EQ presets from the packs that are on, after the built-in ones in the Equalizer.
std::vector<const PackPreset*> PackPresets();
std::vector<const PackLayout*> PackLayoutPresets();
// The track list header's Column sets: the column sets of the packs that are on.
void AppendPackColumnSets(HMENU menu);
// A pack's own field (fields.ini) for track `id`; false for a name no pack defines.
bool PackField(TrackId id, const std::wstring& name, std::wstring& out);
// Global keys ([global keys] in keys.ini): registered again with each key table; PackHotkey runs
// WM_HOTKEY's command, false for a hotkey that isn't a pack's.
void RegisterPackHotkeys();
bool PackHotkey(int id);
// Playlist > Add auto-playlist from a pack: the auto-playlists of the packs that are on.
void AppendPackAutoPlaylists(HMENU menu);
// Plays the pack theme's own sound for `sound`, if the theme in use is a pack theme that has one.
bool PlayPackThemeSound(Sfx sound, float gain);
bool PackThemeHasSound(Sfx sound);

// Installs the pack at `source` (a folder with a pack.ini, a .cordpack, or a .zip with a pack.ini)
// into the packs folder and turns it on; with `ask`, after showing what it holds. False when
// `source` is not a pack, so the caller can treat it as music.
bool InstallPack(const std::wstring& source, bool ask);
// Installs the packs among dropped or opened paths and returns the others.
std::vector<std::wstring> TakePacks(const std::vector<std::wstring>& paths);
// View > Packs > Make a pack: writes the current theme, formats, layout, EQ and auto-playlists as a
// new pack (off) with an example of every other file, and opens its folder when `open`. Returns
// its folder, or empty on failure.
std::wstring MakeStarterPack(bool open);

}  // namespace app
