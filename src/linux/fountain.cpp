// DELTARUNE's live fountain on Linux: the same sprites and water deformation as app_fountain.cpp,
// driven by the engine's audible PCM. Assets stay embedded; the SDL renderer owns only one texture.
#include "draw.h"
#include "cover.h"
#include "radio_ui.h"
#include "fountain_images.inc"
#include <array>

namespace lx {
namespace {
struct Color { float r, g, b; };
Color Mix(Color a, Color b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }
Color Scale(Color a, float k) { return {a.r * k, a.g * k, a.b * k}; }
Color Hsv(double h, double saturation, double value) {
    h = std::fmod(h, 255.0); if (h < 0) h += 255;
    double s = std::clamp(saturation, 0.0, 255.0) / 255, v = std::clamp(value, 0.0, 255.0) / 255;
    double hue = h * 6 / 255, f = hue - std::floor(hue);
    double p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    switch ((int)hue % 6) {
        case 0: return {(float)v, (float)t, (float)p};
        case 1: return {(float)q, (float)v, (float)p};
        case 2: return {(float)p, (float)v, (float)t};
        case 3: return {(float)p, (float)q, (float)v};
        case 4: return {(float)t, (float)p, (float)v};
        default: return {(float)v, (float)p, (float)q};
    }
}
uint32_t Pack(Color c) {
    auto byte = [](float v) { return (uint32_t)(std::clamp(v, 0.f, 1.f) * 255 + 0.5f); };
    return 0xff000000u | byte(c.r) << 16 | byte(c.g) << 8 | byte(c.b);
}
struct Texel { Color c; float alpha; };
Texel Sample(const CoverImage& image, int x, int y, bool tileX, bool tileY) {
    if (tileX) x = (x % image.width + image.width) % image.width;
    if (tileY) y = (y % image.height + image.height) % image.height;
    if (x < 0 || y < 0 || x >= image.width || y >= image.height) return {{}, 0};
    const uint8_t* p = image.rgba.data() + ((size_t)y * image.width + x) * 4;
    return {{p[0] / 255.f, p[1] / 255.f, p[2] / 255.f}, p[3] / 255.f};
}
Color Over(Color under, Texel texel, Color tint, float alpha) {
    Color lit{texel.c.r * tint.r, texel.c.g * tint.g, texel.c.b * tint.b};
    return Mix(under, lit, texel.alpha * alpha);
}
CoverImage edge, basin, water;
bool tried = false;
SDL_Texture* texture = nullptr;
int textureW = 0, textureH = 0;
std::vector<uint32_t> pixels;
double phase = 100;
uint64_t last = 0;
bool SpritesReady() {
    if (!tried) {
        tried = true;
        DecodeCover(std::string((const char*)kFountain_edge, sizeof(kFountain_edge)), 1024, edge);
        DecodeCover(std::string((const char*)kFountain_basin, sizeof(kFountain_basin)), 1024, basin);
        DecodeCover(std::string((const char*)kFountain_texture, sizeof(kFountain_texture)), 1024, water);
    }
    return edge.width > 0 && basin.width > 0 && water.width > 0;
}
}  // namespace

bool DrawLiveFountain(const Rect& area, double progress, const Palette& palette) {
    int width = (int)area.w, height = (int)area.h;
    if (width <= 0 || height <= 0 || !SpritesReady()) return false;
    if (!texture || textureW != width || textureH != height) {
        if (texture) SDL_DestroyTexture(texture);
        texture = SDL_CreateTexture(g.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, width, height);
        textureW = width; textureH = height;
        if (!texture) return false;
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
        pixels.resize((size_t)width * height);
    }
    const uint64_t now = SDL_GetTicks();
    bool active = g.playing != kNone || RadioPlaying();
    if (last && active && !IsPaused()) phase += std::min(4.0, (now - last) * 0.03);
    last = now;
    // Render along the vertical sprite axis, turning it onto the horizontal seek bar at the end.
    int w = height, h = width, top = std::min(h / 2, (int)S(9)), bottom = std::max(top + 1, h - top);
    std::array<uint8_t, 256> peaks{};
    g.engine.WaveformPeaks(peaks.data(), peaks.size());
    std::vector<float> raw(h), levels(h);
    for (int y = 0; y < h; ++y) {
        int row = std::clamp(y, top, bottom - 1) - top, length = bottom - top;
        int first = std::clamp(row * 256 / length, 0, 255), end = std::clamp((row + 1) * 256 / length, first + 1, 256);
        uint8_t peak = 0;
        for (int i = first; i < end; ++i) peak = std::max(peak, peaks[i]);
        raw[y] = peak / 255.f;
    }
    int smooth = std::max(1, (int)S(2));
    for (int y = 0; y < h; ++y) {
        float sum = 0;
        for (int d = -smooth; d <= smooth; ++d) sum += raw[std::clamp(y + d, 0, h - 1)];
        levels[y] = sum / (2 * smooth + 1);
    }
    int playY = RadioPlaying() || (active && g.settings.fullLiveWaveform) ? h :
        g.playing != kNone ? top + (int)((bottom - top) * progress) : -1;
    Color light = Hsv(phase / 4, 160 + std::sin(phase / 32) * 60, 255);
    Color backdrop = Hsv(phase / 4, 255, std::sin(phase / 16) * 40 + 60);
    Color surround{((palette.bg >> 16) & 255) / 255.f, ((palette.bg >> 8) & 255) / 255.f, (palette.bg & 255) / 255.f};
    float cx = w / 2.f, maxHalf = std::max(4.f, w / 2.f - S(2)), minHalf = maxHalf * 0.16f;
    double scale = maxHalf / 66, detailScale = std::max(scale, 0.6 * S(1));
    float footHalf = (float)(57 * scale); maxHalf *= 0.9f;
    double wobble = std::sin(phase / 16) * 1.8, edgeScroll = std::fmod((phase - 100) * 0.0625, 7.0) * 20;
    double footTop = h - 70 * scale, basinTop = h - basin.height * scale, bob = std::sin(phase / 16);
    for (int y = 0; y < h; ++y) {
        float half = minHalf + (maxHalf - minHalf) * levels[y];
        float settle = std::clamp((float)((y - (footTop - 24 * scale)) / (24 * scale)), 0.f, 1.f);
        half += (footHalf - half) * settle * settle * (3 - 2 * settle);
        int edgeY = (int)std::floor(y / detailScale + edgeScroll);
        int waterY1 = (int)std::floor(y / detailScale + phase / 2), waterY2 = (int)std::floor(y / detailScale - phase / 2);
        for (int x = 0; x < w; ++x) {
            double dx = x + 0.5 - cx, past = (std::fabs(dx) - half) / scale;
            auto edgeX = [&](double shift) {
                return (int)std::floor(dx >= 0 ? std::max(50.0, 90 + past) + shift : std::min(49.0, 10 - past) + shift);
            };
            Color c = surround;
            if (past < 10) {
                c = backdrop;
                c = Over(c, Sample(water, (int)std::floor(x / detailScale + phase / 2), waterY1, true, true), light, 0.7f);
                c = Over(c, Sample(water, (int)std::floor(x / detailScale + 120 - std::fmod(phase - 100, 240.0) / 2), waterY2, true, true), light, 0.3f);
                c = Over(c, Sample(edge, edgeX(0), edgeY, false, true), light, 1);
            }
            c = Over(c, Sample(edge, edgeX(-wobble), edgeY, false, true), light, 0.5f);
            c = Over(c, Sample(edge, edgeX(wobble), edgeY, false, true), light, 0.5f);
            if (y >= footTop) {
                int basinX = (int)std::floor(basin.width / 2.0 + dx / scale);
                double basinY = (y - basinTop) / scale;
                c = Over(c, Sample(basin, basinX, (int)std::floor(basinY + 4 - bob * 4), false, false), light, 0.3f);
                c = Over(c, Sample(basin, basinX, (int)std::floor(basinY + 2 - bob * 2), false, false), light, 0.5f);
                c = Over(c, Sample(basin, basinX, (int)std::floor(basinY), false, false), light, 1);
            }
            if (y > playY) {
                float grey = (c.r + c.g + c.b) / 3;
                c = Scale(Mix(c, {grey, grey, grey}, 0.35f), 0.68f);
            }
            pixels[(size_t)(w - 1 - x) * width + y] = Pack(c);
        }
    }
    if (!SDL_UpdateTexture(texture, nullptr, pixels.data(), width * sizeof(uint32_t))) return false;
    SDL_FRect target{area.x, area.y, area.w, area.h};
    return SDL_RenderTexture(g.renderer, texture, nullptr, &target);
}
}  // namespace lx
