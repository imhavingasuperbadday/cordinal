// Radio tab, discovery and station playback (UI thread only).
#pragma once
#include "app_state.h"

namespace app {
void CreateRadio(HWND parent);
void DestroyRadio();
HWND RadioTab();
HWND RadioView();
std::vector<HWND> RadioTabStops();
bool RadioActive();
bool RadioTabOpen();
void CloseRadio();
bool RadioPlaying();
size_t RadioCount();
void ActivateRadio();
void LeaveRadio();
void RadioSearchChanged();
void RadioSearch();
void RadioFocus();
void RadioPlaySelected();
void RadioFonts();
void RadioTick();
void RadioCancelPlayback();
bool RadioEngineEvent(EngineEvent event, unsigned long long token);
std::wstring RadioStationName();
std::wstring RadioSongTitle();
void DrawRadioTab(const DRAWITEMSTRUCT* item);
}  // namespace app
