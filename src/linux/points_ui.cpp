// cordinal points in the Linux window: the chip in the now-playing strip (rank, level, points and a thin
// progress bar, with a "+N" rising when points arrive and a flash at a level-up) and the toast that
// announces a level, as the Windows build's app_points.cpp and app_toast.cpp do. The rules and the ledger
// live in points.cpp; the Points window and the year in review are panels (panels.cpp).
#include "draw.h"
#include "panels.h"
#include "points.h"

namespace lx {
namespace {

struct Chip {
    std::wstring pop;  // "+31"
    uint64_t popAt = 0;
    int levelUpFrames = 0;
} g_chip;

// The toast: fades in, holds, fades out; showing another replaces it.
struct Toast {
    std::wstring title, body;
    uint64_t shownAt = 0;
    Rect rect;
} g_toast;
constexpr uint64_t kFadeIn = 160, kHold = 4200, kFadeOut = 600;

uint64_t ToastAge() { return g_toast.shownAt ? SDL_GetTicks() - g_toast.shownAt : UINT64_MAX; }
bool ToastShown() { return ToastAge() < kFadeIn + kHold + kFadeOut; }

}  // namespace

std::wstring GroupedNumber(uint64_t v) {
    std::wstring digits = std::to_wstring(v), out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0) out += L',';
        out += digits[i];
    }
    return out;
}

PointsLevelInfo CurrentPointsLevel() {
    PointsLevelInfo l;
    uint64_t centi = PointsTotal();
    l.total = centi / 100;
    l.level = PointsLevel(centi);
    l.start = PointsThreshold(l.level) / 100;
    l.next = PointsThreshold(l.level + 1) / 100;
    return l;
}

// Called after every award: the chip updates, a "+N" rises, a level-up flashes and is announced.
void AnnouncePoints(const PointsAward& award) {
    if (award.centi >= 100) {
        g_chip.pop = L"+" + GroupedNumber(award.centi / 100);
        g_chip.popAt = SDL_GetTicks();
    }
    // With the chip hidden, points keep adding up quietly: no level-up message, toast or sound.
    if (award.levelAfter > award.levelBefore && g.settings.pointsChip) {
        g_chip.levelUpFrames = 40;
        bool newRank = PointsRank(award.levelAfter) != std::wstring(PointsRank(award.levelBefore));
        std::wstring text = L"Level " + std::to_wstring(award.levelAfter) + L": " + PointsRank(award.levelAfter);
        if (newRank) text += L". New rank!";
        ShowMessage(text);
        ShowToast(L"Level " + std::to_wstring(award.levelAfter) + L"!",
                  newRank ? std::wstring(L"New rank: ") + PointsRank(award.levelAfter)
                          : std::wstring(PointsRank(award.levelAfter)) + L" \x00B7 click for your progress");
        PlaySfx(Sfx::Milestone);
    }
    g.redraw = true;
}

bool PointsChipAnimating() {
    return g.settings.pointsChip && (g_chip.levelUpFrames > 0 || (g_chip.popAt && SDL_GetTicks() - g_chip.popAt < 2200) || ToastShown());
}

// Called each frame the chip is shown: steps the level-up glow.
void StepPointsChip() {
    if (g_chip.levelUpFrames > 0) --g_chip.levelUpFrames;
    if (g_chip.popAt && SDL_GetTicks() - g_chip.popAt >= 2200) g_chip.popAt = 0;
}

void DrawPointsChip(const Rect& r, const Palette& p) {
    PointsLevelInfo l = CurrentPointsLevel();
    bool glow = g_chip.levelUpFrames > 0 && (g_chip.levelUpFrames / 5) % 2 == 0;
    // Line 1: rank and level. Line 2: points, or the "+N" rising from it. Under them a thin progress bar.
    std::string rank = Utf8(std::wstring(PointsRank(l.level)) + L"  \x00B7  level " + std::to_wstring(l.level));
    DrawText(Font::Bold, rank, r.x, r.y, glow ? p.accent : p.text, r.w, 2);
    double since = g_chip.popAt ? (SDL_GetTicks() - g_chip.popAt) / 2200.0 : 1.0;
    const float line2 = r.y + S(16);
    if (since < 1.0) DrawText(Font::Bold, Utf8(g_chip.pop), r.x, line2 - (float)(S(6) * since), p.accent, r.w, 2);
    else DrawText(Font::Normal, Utf8(GroupedNumber(l.total) + L" cp"), r.x, line2, p.sub, r.w, 2);
    double frac = l.next > l.start ? (double)(l.total - l.start) / (double)(l.next - l.start) : 0;
    Rect bar{r.R() - S(90), r.B() - S(3), S(90), S(2)};
    Fill(bar, p.border);
    Fill({bar.x, bar.y, std::floor(bar.w * (float)std::clamp(frac, 0.0, 1.0)), bar.h}, p.accent);
}

void ShowToast(std::wstring title, std::wstring body) {
    g_toast.title = std::move(title);
    g_toast.body = std::move(body);
    g_toast.shownAt = SDL_GetTicks();
    g.redraw = true;
}

// A small notice under the chip that never takes focus; clicking it opens the Points window.
void DrawToast(const Rect& chip, const Palette& p) {
    if (!ToastShown()) return;
    const uint64_t age = ToastAge();
    float alpha = age < kFadeIn ? (float)age / kFadeIn : age < kFadeIn + kHold ? 1.0f : 1.0f - (float)(age - kFadeIn - kHold) / kFadeOut;
    const float w = S(300), h = S(56);
    g_toast.rect = {std::max(S(8), chip.R() - w), chip.B() + S(10), w, h};
    const Rect& r = g_toast.rect;
    uint8_t a = (uint8_t)std::lround(255 * std::clamp(alpha, 0.0f, 1.0f));
    Fill(r, p.border, a);
    Fill({r.x + 1, r.y + 1, r.w - 2, r.h - 2}, p.surface2, a);
    Fill({r.x + 1, r.y + 1, S(4), r.h - 2}, p.accent, a);
    // The text is not faded (the renderer's text has no alpha here); it appears with the box.
    if (alpha > 0.5f) {
        DrawText(Font::Title, "\xE2\x98\x85", r.x + S(14), r.y + (r.h - LineHeight(Font::Title)) / 2, p.accent);
        DrawText(Font::Bold, Utf8(g_toast.title), r.x + S(50), r.y + S(9), p.text, r.w - S(60));
        DrawText(Font::Normal, Utf8(g_toast.body), r.x + S(50), r.y + S(9) + LineHeight(Font::Bold) + S(2), p.sub, r.w - S(60));
    }
}

bool ToastMouseDown(float x, float y) {
    if (!ToastShown() || !g_toast.rect.In(x, y)) return false;
    g_toast.shownAt = 0;
    OpenPoints();
    return true;
}

}  // namespace lx
