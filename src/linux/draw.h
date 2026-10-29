// Drawing primitives shared by the Linux window (ui.cpp) and its dialogs (panels.cpp): rectangles
// in pixels, sizes scaled from the Windows build's 96-DPI measurements, and filled shapes.
#pragma once
#include "app.h"
#include "text.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace lx {

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    bool In(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
    float R() const { return x + w; }
    float B() const { return y + h; }
};

inline float S(float v) { return std::round(v * g.scale); }

inline void SetColor(uint32_t rgb, uint8_t alpha = 255) {
    SDL_SetRenderDrawColor(g.renderer, (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, alpha);
}
inline void Fill(const Rect& r, uint32_t rgb, uint8_t alpha = 255) {
    SetColor(rgb, alpha);
    SDL_FRect f{r.x, r.y, r.w, r.h};
    SDL_RenderFillRect(g.renderer, &f);
}
inline void Frame(const Rect& r, uint32_t rgb) {
    SetColor(rgb);
    SDL_FRect f{r.x, r.y, r.w, r.h};
    SDL_RenderRect(g.renderer, &f);
}
inline SDL_FColor FColor(uint32_t rgb, float a = 1) {
    return {((rgb >> 16) & 0xFF) / 255.f, ((rgb >> 8) & 0xFF) / 255.f, (rgb & 0xFF) / 255.f, a};
}
inline void Polygon(const std::vector<SDL_FPoint>& pts, uint32_t rgb, float alpha = 1) {  // convex, as a fan
    std::vector<SDL_Vertex> v;
    for (auto& p : pts) v.push_back({p, FColor(rgb, alpha), {0, 0}});
    std::vector<int> idx;
    for (size_t i = 1; i + 1 < v.size(); ++i) idx.insert(idx.end(), {0, (int)i, (int)i + 1});
    SDL_RenderGeometry(g.renderer, nullptr, v.data(), (int)v.size(), idx.data(), (int)idx.size());
}
inline void ThickLine(float x1, float y1, float x2, float y2, float w, uint32_t rgb) {
    float dx = x2 - x1, dy = y2 - y1, len = std::max(0.001f, std::hypot(dx, dy));
    float nx = -dy / len * w / 2, ny = dx / len * w / 2;
    Polygon({{x1 + nx, y1 + ny}, {x2 + nx, y2 + ny}, {x2 - nx, y2 - ny}, {x1 - nx, y1 - ny}}, rgb);
}
inline uint32_t Mix(uint32_t a, uint32_t b, int percent) {  // `percent` of the way from a to b
    uint32_t out = 0;
    for (int shift : {16, 8, 0}) {
        int x = (a >> shift) & 0xFF, y = (b >> shift) & 0xFF;
        out |= (uint32_t)(x + (y - x) * percent / 100) << shift;
    }
    return out;
}
// The text's top for `font` centered vertically in `r`.
inline float TextY(const Rect& r, Font font = Font::Normal) { return std::round(r.y + (r.h - LineHeight(font)) / 2); }

// panels.cpp: a dialog's dimmed backdrop and centered surface with its title, and its buttons.
Rect DrawFrame(const Palette& p, float w, float h, const char* title);
void DrawButton(const Rect& r, const std::string& label, const Palette& p, bool accent = false, bool enabled = true);
std::vector<std::string> Wrap(Font font, const std::string& text, float width);  // lines at most `width` wide

// points_ui.cpp: the chip in the now-playing strip and the toast under it.
void DrawPointsChip(const Rect& r, const Palette& p);
void DrawToast(const Rect& chip, const Palette& p);
void DrawLyrics(const Rect& area, const Palette& p);  // lyrics.cpp
void DrawMini(const Palette& p);                      // mini.cpp
bool DrawLiveFountain(const Rect& area, double progress, const Palette& p);  // fountain.cpp
// preferences_ui.cpp: the modeless startup-defaults card.
void DrawStartupReview(const Palette& p, float mouseX, float mouseY);
bool StartupReviewMouseDown(float x, float y, int button);  // true when the card took the click

}  // namespace lx
