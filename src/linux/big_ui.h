// Compact, Spotlight and Big presentation on Linux. Big panel trees use the Windows format.
#pragma once
#include "draw.h"
namespace lx {
bool LayoutPresentation(Rect bounds, Rect& library);
void DrawBigPanels(const Palette& p);
bool PresentationMouseDown(float x, float y, int button, int clicks);
bool PresentationMouseMove(float x, float y, bool leftDown);
bool PresentationMouseUp(float x, float y);
bool PresentationKey(const SDL_KeyboardEvent& e);
bool BigPanelShown(int panel);
void ToggleBigPanel(int panel);
void ResetBigPanels();
void LoadBigLayout(const std::string& text);
std::string BigLayoutText();
void LoadCompactPlacement(const std::string& text);
std::string CompactPlacementText();
void InitializePresentation();
void SavePresentationWindow();
void SetViewSize(ViewSize size);
int CheckLinuxBig();
}
