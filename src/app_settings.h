// Settings shared by the application modules: settings.ini accessors, data-folder
// setup and the load/save of the whole profile. Calls that touch the ini require
// InitDataPaths() to have run first (it sets g.iniPath).
#pragma once
#include "app_state.h"

namespace app {

void SaveLibraryNow();
// A library that couldn't be saved goes beside library.cdb as library.cdb.unsaved; the next start
// restores it when it is newer than library.cdb, and the next successful save removes it.
std::wstring UnsavedLibraryPath();
bool KeepUnsavedLibrary();
// `kept` names the copy set aside when the recovery file itself can't be read, so the caller can
// tell the user rather than starting a silently empty library.
bool RestoreUnsavedLibrary(std::wstring* kept = nullptr);
// Saves everything at close, falling back to library.cdb.unsaved. When neither can be written, asks
// whether to try again, save a copy elsewhere, or quit without it; returns once the user has chosen.
void SaveBeforeClose();
// settings.ini (g.iniPath), read and edited in memory and saved shortly after a change (app_ini.cpp).
// Lookups follow the Win32 profile API. IniWrite returns false while the last save failed; that
// failure is reported to the user once.
std::wstring IniRead(const wchar_t* section, const wchar_t* key, const wchar_t* def);
int IniInt(const wchar_t* section, const wchar_t* key, int def);
float IniFloat(const wchar_t* section, const wchar_t* key, float def);
bool IniWrite(const wchar_t* section, const wchar_t* key, const std::wstring& value);
// Removes a key, or the whole section when `key` is null.
void IniDelete(const wchar_t* section, const wchar_t* key);
// A section's key=value entries in file order, and its replacement (empty removes the section).
std::vector<std::pair<std::wstring, std::wstring>> IniEntries(const wchar_t* section);
void IniReplaceSection(const wchar_t* section, const std::vector<std::pair<std::wstring, std::wstring>>& entries);
std::vector<std::wstring> IniSectionNames();
// Saves unsaved edits now and waits; false when the file couldn't be written.
bool IniFlush();
// Rereads settings.ini when something else changed it since it was read or saved.
void IniRefresh();
std::wstring F2S(float v);
std::vector<int> ParseInts(const std::wstring& s);
std::wstring JoinInts(const std::vector<int>& v);
void InitDataPaths();
void LoadFormats();
void SaveEq();
void SaveOutputReplayGainHeadroom(float db);
EqSettings ReadEqSettings(const wchar_t* section);
void WriteEqSettings(const wchar_t* section, const EqSettings& eq);
void SyncOutputEq(const std::wstring& id);
void RememberOutputEq(bool remember);
// True when settings.ini has any OutputEQ-* section (a remembered curve or headroom).
bool SavedOutputProfiles();
// User preferences can be saved independently of the library and playback session.
void SavePreferences();
// Marks preferences dirty and writes them on the next UI tick (or FlushPendingPreferences).
// Use on the click/toggle path so a full settings.ini rewrite does not stall the UI thread.
void SavePreferencesSoon();
void FlushPendingPreferences();
bool SaveBackupSettings(std::string& bytes);
void LoadPreferences();
void SaveState();
void LoadState();
// Library > Sync with other PCs (app_sync.cpp, syncfolder.h): a pass about once a minute from the UI
// tick (`now` starts one at once), the bookkeeping written after each library save, and a last pass
// at close that publishes this PC's changes before the final save.
void SyncTick(bool now = false);
void SyncLibrarySaved();
void SyncFinish();
void ShowSync();
void RestoreWindowPlacement(int showCmd);

}  // namespace app
