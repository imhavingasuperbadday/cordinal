// Linux Headphones page and its UI-thread integration points.
#pragma once
#include "draw.h"
#include "headsets.h"

namespace lx {
void StartHeadphones();
void StopHeadphones();
void HeadphonesTick();
void SaveHeadphoneProfiles();
const EqSettings& HeadphonesGlobalEq();
float HeadphonesHeadroom();
void OpenHeadphones();  // panels.cpp selects its Headphones page
void PrepareHeadphonesPanel();
void DrawHeadphones(const Palette& p);
void HeadphonesMouseDown(float x, float y);
void HeadphonesWheel(float dy);
void HeadphonesKey(const SDL_KeyboardEvent& e);
int CheckLinuxHeadsets();
int CheckHeadphonesPanel();
}
