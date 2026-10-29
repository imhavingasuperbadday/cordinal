// Main-window module declarations: menu/accelerator building, control creation, the
// window procedure, command dispatch and the equalizer window. All of it runs on the
// UI thread; app_state.h holds the shared `g` it operates on.
#pragma once
#include "app_state.h"

namespace app {

// Equalizer dialog/panel (app_window's EqProc and app_eq.cpp).
void EqSync();
void EqApply();
void EqLayout(HWND h);
LRESULT CALLBACK EqProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
void ThemeEqControls();
void ShowEqualizer();
void SelectEqPage(bool headphones);
bool EqualizerDialogMessage(MSG* message);
void HeadphonesTick();
void EqDeviceSync(bool changedOutput);
void ShowHeadphones();
void HeadphonesSync(bool values);
LRESULT CALLBACK HeadphonesProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
void UpdateOrderButtons();
void SetShuffle(int mode);
void SetRepeat(Repeat repeat);
HMENU ReplayGainMenu();
HMENU BuildMenu();
HACCEL BuildAccelerators();
void CreateControls(HWND hwnd);
void OnCommand(int id, int code, HWND ctl);
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
// Single-instance forwarding (app.cpp). `addOnly` selects the no-playback flavour used by
// Explorer's Add verb.
bool ForwardToRunningInstance(const std::vector<std::wstring>& args, bool addOnly = false);
// Window lifecycle, implemented in app_lifecycle.cpp; Create on WM_CREATE, Stop workers
// and Destroy in order on close.
void CreateMainWindow(HWND hwnd);
void FinishStartup(HWND hwnd);  // after the first frame: tray, media controls, headsets
void StopAppWorkers(HWND hwnd);
void DestroyMainWindow(HWND hwnd);
void MainTick();
void UpdateMainMenu(HMENU menu);

}  // namespace app
