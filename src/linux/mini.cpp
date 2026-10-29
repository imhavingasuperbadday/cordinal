// The mini player on Linux (View > Mini player, Ctrl+Shift+M), as the Windows build's app_mini.cpp: a round
// window that stays on top. The cover fills the disc; a ring around it shows how far the track has played
// (click or drag it to seek); previous / play / next sit across the middle. Drag the window by the cover; the
// wheel sets the volume; Esc, Ctrl+Shift+M or a double-click brings the full window back. It is the main window
// made small (no second window), so everything else keeps running as it was; a compositor that cannot shape a
// window leaves the corners square.
#include "draw.h"

#include <SDL3/SDL.h>

namespace lx {

// ui.cpp: the cover of the playing (or resumable) track, if it is loaded.
SDL_Texture* MiniCover();

namespace {

constexpr float kPi = 3.14159265f;
bool g_active = false;
int g_savedW = 0, g_savedH = 0, g_savedX = 0, g_savedY = 0;
bool g_savedMaximized = false;
enum Part { kPrev, kPlay, kNext, kRing, kNoPart = -1 };
int g_hot = kNoPart, g_pressed = kNoPart;
bool g_seeking = false, g_dragging = false;
double g_seekFrac = 0;
float g_dragDX = 0, g_dragDY = 0;

float Diameter() { return S(196); }
float Radius() { return Diameter() / 2; }
float RingRadius() { return Radius() - S(9); }
float DiscRadius() { return Radius() - S(17); }
float PartRadius(int part) { return part == kPlay ? S(27) : S(20); }
SDL_FPoint PartCenter(int part) {
    float c = Radius(), off = S(54);
    return {part == kPrev ? c - off : part == kNext ? c + off : c, c};
}

int PartAt(float x, float y) {
    float dx = x - Radius(), dy = y - Radius(), d = std::sqrt(dx * dx + dy * dy);
    if (d > Radius()) return kNoPart;
    if (d >= DiscRadius() - S(2)) return kRing;
    for (int i = kPrev; i <= kNext; ++i) {
        SDL_FPoint c = PartCenter(i);
        float px = x - c.x, py = y - c.y;
        if (px * px + py * py <= PartRadius(i) * PartRadius(i)) return i;
    }
    return kNoPart;
}

// The fraction of the ring (from the top, clockwise) the point is at.
double RingFraction(float x, float y) {
    double angle = std::atan2(y - Radius(), x - Radius()) + kPi / 2;
    if (angle < 0) angle += 2 * kPi;
    return std::clamp(angle / (2 * kPi), 0.0, 1.0);
}

double Duration() { return g.playing != kNone ? std::max(0.0, g.engine.Duration() > 0 ? g.engine.Duration() : g_lib[g.playing].Duration()) : 0; }

std::vector<SDL_FPoint> Circle(float cx, float cy, float r, int steps = 72) {
    std::vector<SDL_FPoint> pts;
    for (int i = 0; i < steps; ++i) {
        float a = 2 * kPi * i / steps;
        pts.push_back({cx + r * std::cos(a), cy + r * std::sin(a)});
    }
    return pts;
}

// The cover over a disc: a fan of triangles whose texture coordinates follow the circle.
void TexturedDisc(SDL_Texture* tex, float cx, float cy, float r) {
    std::vector<SDL_Vertex> v;
    v.push_back({{cx, cy}, FColor(0xFFFFFF), {0.5f, 0.5f}});
    const int steps = 96;
    for (int i = 0; i <= steps; ++i) {
        float a = 2 * kPi * i / steps, c = std::cos(a), s = std::sin(a);
        v.push_back({{cx + r * c, cy + r * s}, FColor(0xFFFFFF), {0.5f + 0.5f * c, 0.5f + 0.5f * s}});
    }
    std::vector<int> idx;
    for (int i = 1; i <= steps; ++i) idx.insert(idx.end(), {0, i, i + 1});
    SDL_RenderGeometry(g.renderer, tex, v.data(), (int)v.size(), idx.data(), (int)idx.size());
}

void Arc(float cx, float cy, float r, double from, double to, float width, uint32_t color) {
    const int steps = std::max(2, (int)std::ceil((to - from) / (2 * kPi) * 120));
    for (int i = 0; i < steps; ++i) {
        double a0 = from + (to - from) * i / steps, a1 = from + (to - from) * (i + 1) / steps;
        ThickLine(cx + r * (float)std::cos(a0), cy + r * (float)std::sin(a0), cx + r * (float)std::cos(a1), cy + r * (float)std::sin(a1), width, color);
    }
}

void Glyph(int part, SDL_FPoint c, float r, bool pause, uint32_t color) {
    const float h = r * 0.5f;
    if (part == kPlay && pause) {
        Fill({c.x - h * 0.8f, c.y - h, h * 0.6f, 2 * h}, color);
        Fill({c.x + h * 0.2f, c.y - h, h * 0.6f, 2 * h}, color);
    } else if (part == kPlay) {
        Polygon({{c.x - h * 0.6f, c.y - h}, {c.x + h, c.y}, {c.x - h * 0.6f, c.y + h}}, color);
    } else if (part == kPrev) {
        Fill({c.x - h, c.y - h * 0.8f, h * 0.25f, h * 1.6f}, color);
        Polygon({{c.x + h * 0.7f, c.y - h * 0.8f}, {c.x - h * 0.7f, c.y}, {c.x + h * 0.7f, c.y + h * 0.8f}}, color);
    } else {
        Fill({c.x + h * 0.75f, c.y - h * 0.8f, h * 0.25f, h * 1.6f}, color);
        Polygon({{c.x - h * 0.7f, c.y - h * 0.8f}, {c.x + h * 0.7f, c.y}, {c.x - h * 0.7f, c.y + h * 0.8f}}, color);
    }
}

void ShapeWindow() {
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(g.window, &pw, &ph);
    if (SDL_Surface* mask = SDL_CreateSurface(pw, ph, SDL_PIXELFORMAT_RGBA32)) {
        SDL_ClearSurface(mask, 0, 0, 0, 0);
        const float r = pw / 2.0f;
        for (int y = 0; y < ph; ++y)
            for (int x = 0; x < pw; ++x) {
                float dx = x + 0.5f - r, dy = y + 0.5f - ph / 2.0f;
                if (dx * dx + dy * dy <= r * r) SDL_WriteSurfacePixel(mask, x, y, 255, 255, 255, 255);
            }
        SDL_SetWindowShape(g.window, mask);  // fails without a transparent window: the corners stay square
        SDL_DestroySurface(mask);
    }
}

void Enter() {
    g_savedMaximized = SDL_GetWindowFlags(g.window) & SDL_WINDOW_MAXIMIZED;
    if (g_savedMaximized) SDL_RestoreWindow(g.window);
    SDL_GetWindowSize(g.window, &g_savedW, &g_savedH);
    SDL_GetWindowPosition(g.window, &g_savedX, &g_savedY);
    g_active = true;
    SDL_SetWindowMinimumSize(g.window, 0, 0);
    SDL_SetWindowResizable(g.window, false);
    SDL_SetWindowBordered(g.window, false);
    SDL_SetWindowAlwaysOnTop(g.window, true);
    float density = std::max(0.5f, SDL_GetWindowPixelDensity(g.window));
    int size = (int)std::lround(Diameter() / density);
    SDL_SetWindowSize(g.window, size, size);
    SDL_SyncWindow(g.window);
    ShapeWindow();
    g.redraw = true;
}

}  // namespace

bool MiniActive() { return g_active; }

void LeaveMini() {
    if (!g_active) return;
    g_active = false;
    g_hot = g_pressed = kNoPart;
    g_seeking = g_dragging = false;
    SDL_SetWindowShape(g.window, nullptr);
    SDL_SetWindowBordered(g.window, true);
    SDL_SetWindowResizable(g.window, true);
    SDL_SetWindowAlwaysOnTop(g.window, g.settings.alwaysOnTop);
    SDL_SetWindowSize(g.window, g_savedW, g_savedH);
    SDL_SetWindowPosition(g.window, g_savedX, g_savedY);
    if (g_savedMaximized) SDL_MaximizeWindow(g.window);
    g.redraw = true;
}

void ToggleMini() {
    if (g_active) LeaveMini();
    else Enter();
}

void DrawMini(const Palette& p) {
    SetColor(p.bg);
    SDL_RenderClear(g.renderer);
    const float cx = Radius(), cy = Radius();
    Polygon(Circle(cx, cy, Radius() - S(1)), p.surface);
    // The disc: the cover, dimmed a little under the buttons.
    if (SDL_Texture* cover = MiniCover()) TexturedDisc(cover, cx, cy, DiscRadius());
    else Polygon(Circle(cx, cy, DiscRadius()), p.surface2);
    Polygon(Circle(cx, cy, DiscRadius()), 0x000000, 0.28f);
    // The ring: how far the track has played (or where a drag would seek to).
    const double dur = Duration();
    double frac = g_seeking ? g_seekFrac : dur > 0 ? std::clamp(g.engine.Position() / dur, 0.0, 1.0) : 0;
    Arc(cx, cy, RingRadius(), 0, 2 * kPi, S(4), p.border);
    if (frac > 0) Arc(cx, cy, RingRadius(), -kPi / 2, -kPi / 2 + 2 * kPi * frac, S(4), g.engine.Paused() ? p.amber : p.accent);
    // Previous, play / pause, next.
    const bool paused = g.playing == kNone || g.engine.Paused();
    for (int part = kPrev; part <= kNext; ++part) {
        SDL_FPoint c = PartCenter(part);
        const bool hot = g_hot == part;
        Polygon(Circle(c.x, c.y, PartRadius(part)), hot ? p.accent : 0x000000, hot ? 0.9f : 0.55f);
        Glyph(part, c, PartRadius(part), part == kPlay && !paused, hot ? TextOn(p.accent) : 0xFFFFFF);
    }
}

// Mouse and keys while the mini player shows. True when the event is theirs.
bool MiniEvent(const SDL_Event& e) {
    if (!g_active) return false;
    g.redraw = true;
    switch (e.type) {
        case SDL_EVENT_MOUSE_MOTION: {
            float x = e.motion.x * SDL_GetWindowPixelDensity(g.window), y = e.motion.y * SDL_GetWindowPixelDensity(g.window);
            g_hot = PartAt(x, y);
            if (g_hot == kRing) g_hot = kNoPart;
            if (g_seeking) g_seekFrac = RingFraction(x, y);
            if (g_dragging) {
                float gx = 0, gy = 0;
                SDL_GetGlobalMouseState(&gx, &gy);
                SDL_SetWindowPosition(g.window, (int)(gx - g_dragDX), (int)(gy - g_dragDY));
            }
            return true;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
            if (e.button.button != SDL_BUTTON_LEFT) return true;
            float d = SDL_GetWindowPixelDensity(g.window), x = e.button.x * d, y = e.button.y * d;
            g_pressed = PartAt(x, y);
            if (g_pressed == kRing && Duration() > 0) {
                g_seeking = true;
                g_seekFrac = RingFraction(x, y);
            } else if (g_pressed == kNoPart) {
                if (e.button.clicks >= 2) return LeaveMini(), true;
                float gx = 0, gy = 0;
                int wx = 0, wy = 0;
                SDL_GetGlobalMouseState(&gx, &gy);
                SDL_GetWindowPosition(g.window, &wx, &wy);
                g_dragDX = gx - wx;
                g_dragDY = gy - wy;
                g_dragging = true;
            }
            return true;
        }
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            if (e.button.button != SDL_BUTTON_LEFT) return true;
            float d = SDL_GetWindowPixelDensity(g.window);
            int part = PartAt(e.button.x * d, e.button.y * d);
            if (g_seeking) SeekTo(g_seekFrac * Duration());
            else if (part == g_pressed && part == kPrev) Previous();
            else if (part == g_pressed && part == kPlay) TogglePause();
            else if (part == g_pressed && part == kNext) Next();
            g_seeking = g_dragging = false;
            g_pressed = kNoPart;
            return true;
        }
        case SDL_EVENT_MOUSE_WHEEL:
            SetVolume(std::clamp(g.settings.volume + (e.wheel.y > 0 ? 0.05f : -0.05f), 0.0f, 1.0f));
            return true;
        case SDL_EVENT_KEY_DOWN: {
            const bool ctrl = e.key.mod & SDL_KMOD_CTRL, shift = e.key.mod & SDL_KMOD_SHIFT;
            if (e.key.key == SDLK_ESCAPE || (ctrl && shift && e.key.key == SDLK_M) || (ctrl && e.key.key == SDLK_EQUALS)) LeaveMini();
            else if (e.key.key == SDLK_SPACE) TogglePause();
            else if (ctrl && e.key.key == SDLK_Q) g.quit = true;
            else if (ctrl && e.key.key == SDLK_RIGHT) Next();
            else if (ctrl && e.key.key == SDLK_LEFT) Previous();
            return true;
        }
        case SDL_EVENT_KEY_UP:
        case SDL_EVENT_TEXT_INPUT: return true;
        default: return false;
    }
}

}  // namespace lx
