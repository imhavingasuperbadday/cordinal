// Declares the platform layer shared by the application modules: crash reporting, the system
// media controls, taskbar and tray integration, native scrobbling, Steam soundtrack
// discovery and the repeat-count setting. InitPlatform/ShutdownPlatform bracket it, and window
// messages are routed through OnPlatformMessage.
#pragma once
#include "app_state.h"

namespace app {

void InstallCrashReports();
// A per-machine cache folder (thumbnails, waveforms, crash reports) under %LOCALAPPDATA%\cordinal,
// or inside the profile when CORDINAL_DATA names one, so a throwaway profile keeps its caches too.
std::wstring LocalCacheDir(const wchar_t* name);
void InitPlatform(HWND hwnd);
void ShutdownPlatform();
bool OnPlatformMessage(UINT msg, WPARAM wp, LPARAM lp);
void PlatformTrackChanged();
void PlatformArtChanged();  // g.art arrived or changed: the flyout's cover follows
void PlatformTick();
void PlatformCountedPlay(TrackId id, uint32_t when, double seconds, bool local);
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
// The scrobbling connections window (app_platform.cpp).
void ShowListenBrainz();
bool ListenBrainzDialogMessage(MSG* msg);
// Steam soundtrack discovery and the repeat-count setting (app_platform.cpp).
void AddSteamSoundtracks();
bool SteamDiscoveryRunning();
void CancelSteamDiscovery();
void OnSteamDiscovery(WPARAM offer, LPARAM result);
void StopSteamDiscovery();
void SetLoops(int count);
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
void TrayNotice(const std::wstring& title, const std::wstring& text);  // a notification from the icon
const wchar_t* IconFace();  // Segoe MDL2 Assets, or Segoe Fluent Icons where only that is installed

// Lightweight mode (app_lightweight.cpp): playing in the background with the window put away.
enum class LightweightShows : uint8_t { Nothing, Mini, Compact };  // what stays on screen
enum class LightweightNumpad : uint8_t { Off, NumLockOff, Always };
void EnterLightweight();
void LeaveLightweight(bool show = true);
void ToggleLightweight();
bool LightweightHidesWindow();  // on, with nothing or the mini player in the main window's place
bool LightweightPausesWork();   // on, with background library work paused
void LightweightWindowShown();
void LightweightSizeChanged(ViewSize size);
void LightweightPreferencesChanged();
bool LightweightTimer(UINT_PTR id);
void LightweightMiniChanged();  // the mini player opened or closed
// The keymap numpad index (keymap.h) a key press is under a LightweightNumpad mode, -1 for none;
// `extended` is the keyboard's flag that tells the arrow-key block from the numpad with Num Lock off.
int NumpadKeyIndex(DWORD vk, bool extended, int mode);

}  // namespace app
