// Declares the platform layer shared by the application modules: crash reporting, the system
// media controls, taskbar and tray integration, ListenBrainz scrobbling, Steam soundtrack
// discovery and the repeat-count setting. InitPlatform/ShutdownPlatform bracket it, and window
// messages are routed through OnPlatformMessage.
#pragma once
#include "app_state.h"

namespace app {

void InstallCrashReports();
void InitPlatform(HWND hwnd);
void ShutdownPlatform();
bool OnPlatformMessage(UINT msg, WPARAM wp, LPARAM lp);
void PlatformTrackChanged();
void PlatformTick();
void PlatformCountedPlay(TrackId id, uint32_t when);
std::optional<int> ForwardTransportCommand(const std::vector<std::wstring>& args);
void RunTransportCommand(std::wstring_view command);
// `--add` (Explorer's Add verb) makes command-line paths append without playing; it is removed
// from `args` before anything else reads them. Returns whether it was there.
bool TakeAddSwitch(std::vector<std::wstring>& args);
// The Explorer folder menu (app_platform.cpp): per-user verbs under HKCU\Software\Classes so
// right-clicking a folder offers Play with cordinal and Add to cordinal, without elevation.
// `base` is the key below HKCU; tests pass a scratch one.
inline constexpr wchar_t kExplorerBase[] = L"Software\\Classes";
std::wstring ExplorerPlayCommand();
std::wstring ExplorerAddCommand();
bool InstallExplorerMenu(const wchar_t* base = kExplorerBase);
bool RemoveExplorerMenu(const wchar_t* base = kExplorerBase);
bool ExplorerMenuInstalled(const wchar_t* base = kExplorerBase);
void ApplyExplorerMenu();   // a live preference change
void EnsureExplorerMenu();  // startup: follow the preference and this executable's path
// The ListenBrainz token window (app_platform.cpp).
void ShowListenBrainz();
bool ListenBrainzDialogMessage(MSG* msg);
// Steam soundtrack discovery and the repeat-count setting (app_platform.cpp).
void AddSteamSoundtracks();
void SetLoops(int count);
void FindDuplicateAudioPlaylist();
// Taskbar thumbnail buttons and progress (app_taskbar.cpp).
void InitTaskbar();
bool OnTaskbarMessage(UINT msg);
void UpdateTaskbar();
void TaskbarDpiChanged();
void SetTaskbarProgress(bool on);
void ShutdownTaskbar();
// Notification-area icon and the "start with Windows" logon entry (app_tray.cpp).
void InitTray();
void ShutdownTray();
bool OnTrayMessage(UINT msg, WPARAM wp, LPARAM lp);
void UpdateTray();
void TrayThemeChanged();
void TrayDpiChanged();
bool StartWithWindows();
void ToggleStartWithWindows();
void ShowMainWindow();
const wchar_t* IconFace();  // Segoe MDL2 Assets, or Segoe Fluent Icons where only that is installed

}  // namespace app
