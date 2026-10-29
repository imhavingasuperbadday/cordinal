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
bool RestoreUnsavedLibrary();
// Saves everything at close, falling back to library.cdb.unsaved. When neither can be written, asks
// whether to try again, save a copy elsewhere, or quit without it; returns once the user has chosen.
void SaveBeforeClose();
// Profile-API accessors over g.iniPath.
std::wstring IniRead(const wchar_t* section, const wchar_t* key, const wchar_t* def);
int IniInt(const wchar_t* section, const wchar_t* key, int def);
float IniFloat(const wchar_t* section, const wchar_t* key, float def);
bool IniWrite(const wchar_t* section, const wchar_t* key, const std::wstring& value);
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
// User preferences can be saved independently of the library and playback session.
void SavePreferences();
bool SaveBackupSettings(std::string& bytes);
void LoadPreferences();
void SaveState();
void LoadState();
void RestoreWindowPlacement(int showCmd);

}  // namespace app
