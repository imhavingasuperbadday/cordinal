// The player's waveform as a Dark Fountain, and the waveform colors of every player position.
//
// At every player position (app_custom.cpp), the seek bar can be drawn three ways
// (View > Customize layout > Waveform > Waveform style):
//   Bars            the plain waveform (app_waveform.cpp)
//   Fountain bars   the waveform's own bars in the fountain's light: the hue flows up the column,
//                   the bars ripple and sparks rise. Nothing from the game is needed.
//   Dark Fountain   DELTARUNE's obj_darkfountain itself, its sprites redrawn with the column's
//                   width following the track's loudness: the scrolling fountain texture inside,
//                   the rippling edges with their two wobbling echoes, the basin at the foot.
// The game's sprites (spr_fountainedge, spr_fountainbottom, bg_fountain1) are built into the
// executable (src/deltarune, cordinal.rc).
//
// Both fountain styles follow obj_darkfountain's Draw event at the game's 30 frames a second:
//   hue      = (siner / 4) mod 255
//   colcol   = make_color_hsv(hue, 160 + sin(siner / 32) * 60, 255)          the fountain's light
//   backdrop = make_color_hsv(hue, 255, 60 + sin(siner / 16) * 40)           the room behind it
// and four toggles: Moving (the clock runs while a track plays, and slows to a stop on pause like
// the fountain sealing), DELTARUNE colors (colcol, or else the position's played color), Fountain
// glow (the backdrop, or else black) and Black surroundings (as in the game, or the bar's surface).
// What has played is lit; the rest of the track is dimmed, and the playhead is a bright line.
//
// Waveform colors: each player position (top, bottom, left, right) can replace the theme's played
// and unplayed colors with its own (Waveform > Colors). Paused and warning keep their state colors.
#include "app.h"
#include "peaks.h"
#include "resource.h"

#include <shlwapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cmath>
#include <array>

using Microsoft::WRL::ComPtr;

namespace app {

namespace {

enum class WaveStyle : uint8_t { Bars, FountainBars, DarkFountain, Count };

constexpr Rgb kThemeColor = 0xFFFFFFFF;  // a position color left to the theme
constexpr UINT_PTR kFountainTimer = 0xF0A1;  // on g.seek
constexpr double kFps = 30;                  // DELTARUNE's frame rate: siner counts these frames

struct Look {
    WaveStyle style = WaveStyle::DarkFountain;
    bool motion = true, drColors = true, glow = true, black = true;
    Rgb played[(int)Dock::Count] = {kThemeColor, kThemeColor, kThemeColor, kThemeColor};
    Rgb rest[(int)Dock::Count] = {kThemeColor, kThemeColor, kThemeColor, kThemeColor};
} look;

// A sprite as straight-alpha BGRA.
struct Sprite {
    int w = 0, h = 0;
    std::vector<uint8_t> px;
    bool Empty() const { return px.empty(); }
};
Sprite edge, basin, texture;
bool spritesTried = false;

// obj_darkfountain's clock, in game frames. `speed` eases toward 1 while it moves and toward 0 when
// it stops, as the game's slowdown does when the fountain is sealed.
struct Clock {
    double siner = 100, bgsiner = 0, hscroll = 0, speed = 0;
    ULONGLONG last = 0;
    bool running = false;  // kFountainTimer is set
} fclock;

// The paint's pixel buffer, kept between frames.
HBITMAP dib = nullptr;
uint32_t* bits = nullptr;
int dibW = 0, dibH = 0;

// Decodes the PNG resource `id` (RCDATA) into a sprite.
bool LoadSprite(IWICImagingFactory* wic, int id, Sprite& out) {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    HGLOBAL data = res ? LoadResource(nullptr, res) : nullptr;
    const BYTE* bytes = data ? (const BYTE*)LockResource(data) : nullptr;
    if (!bytes) return false;
    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(bytes, SizeofResource(nullptr, res)));
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapSource> bgra;
    if (!stream || FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(WICConvertBitmapSource(GUID_WICPixelFormat32bppBGRA, frame.Get(), &bgra)))
        return false;
    UINT w = 0, h = 0;
    if (FAILED(bgra->GetSize(&w, &h)) || w == 0 || h == 0 || w > 1024 || h > 1024) return false;
    out.w = (int)w;
    out.h = (int)h;
    out.px.resize((size_t)w * h * 4);
    if (FAILED(bgra->CopyPixels(nullptr, w * 4, (UINT)out.px.size(), out.px.data()))) {
        out = {};
        return false;
    }
    return true;
}

// Decoded on first use; without them (a decode failure) the Dark Fountain draws Fountain bars.
bool SpritesReady() {
    if (!spritesTried) {
        spritesTried = true;
        ComPtr<IWICImagingFactory> wic;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
            !LoadSprite(wic.Get(), IDR_FOUNTAIN_EDGE, edge) || !LoadSprite(wic.Get(), IDR_FOUNTAIN_BASIN, basin) ||
            !LoadSprite(wic.Get(), IDR_FOUNTAIN_TEXTURE, texture))
            edge = basin = texture = {};
    }
    return !edge.Empty();
}

// GameMaker's make_color_hsv: hue, saturation and value all 0..255. Returns 0..1 channels.
struct Color {
    float r, g, b;
};
Color Hsv(double h, double s, double v) {
    h = std::fmod(h, 255.0);
    if (h < 0) h += 255;
    s = std::clamp(s, 0.0, 255.0) / 255;
    v = std::clamp(v, 0.0, 255.0) / 255;
    double hh = h * 6 / 255, f = hh - std::floor(hh);
    double p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    switch ((int)hh % 6) {
        case 0: return {(float)v, (float)t, (float)p};
        case 1: return {(float)q, (float)v, (float)p};
        case 2: return {(float)p, (float)v, (float)t};
        case 3: return {(float)p, (float)q, (float)v};
        case 4: return {(float)t, (float)p, (float)v};
        default: return {(float)v, (float)p, (float)q};
    }
}
Color FromRgb(Rgb c) { return {((c >> 16) & 0xFF) / 255.f, ((c >> 8) & 0xFF) / 255.f, (c & 0xFF) / 255.f}; }
Color FromRef(COLORREF c) { return {GetRValue(c) / 255.f, GetGValue(c) / 255.f, GetBValue(c) / 255.f}; }
Color Mix(Color a, Color b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }
Color Mul(Color a, Color b) { return {a.r * b.r, a.g * b.g, a.b * b.b}; }
Color Scale(Color a, float k) { return {a.r * k, a.g * k, a.b * k}; }
uint32_t Pack(Color c) {
    auto u = [](float v) { return (uint32_t)(std::clamp(v, 0.f, 1.f) * 255 + 0.5f); };
    return (u(c.r) << 16) | (u(c.g) << 8) | u(c.b);
}

// A sprite texel, straight alpha 0..1, tiled or clamped (outside a clamped sprite: transparent).
struct Texel {
    Color c;
    float a;
};
Texel Sample(const Sprite& s, int u, int v, bool tileX, bool tileY) {
    if (tileX) u = ((u % s.w) + s.w) % s.w;
    if (tileY) v = ((v % s.h) + s.h) % s.h;
    if (u < 0 || v < 0 || u >= s.w || v >= s.h) return {{0, 0, 0}, 0};
    const uint8_t* p = &s.px[((size_t)v * s.w + u) * 4];
    return {{p[2] / 255.f, p[1] / 255.f, p[0] / 255.f}, p[3] / 255.f};
}

// Draws a texel tinted by `tint` over `c`, as draw_sprite_ext with a blend color does. Off the
// black surroundings the sprite's black is the surroundings instead, so the edges keep their glow
// without carving a black band into a light theme.
Color Over(Color c, Texel t, Color tint, float alpha, Color surround) {
    if (t.a <= 0) return c;
    Color lit = look.black ? Mul(t.c, tint) : Mix(surround, tint, (t.c.r + t.c.g + t.c.b) / 3);
    return Mix(c, lit, t.a * alpha);
}

bool LiveInputMoving() { return g.liveFountain && (g.playing != kNone || RadioPlaying()) && !g.engine.Paused(); }
bool Moving() { return (g.playing != kNone || RadioPlaying()) && !g.engine.Paused() && look.motion; }

// The position's played color: its own, else the theme's state color (as the plain waveform's).
Rgb DockPlayed() { return g.size == ViewSize::Normal ? look.played[(int)g.layout.player] : kThemeColor; }
Rgb DockRest() { return g.size == ViewSize::Normal ? look.rest[(int)g.layout.player] : kThemeColor; }

// What the fountain is lit with this frame: the game's colcol and backdrop, or the position's
// colors; warnings (stop after current, a failed track) pull it toward the error color.
struct Light {
    Color col, backdrop, rest, surround;
    bool warn;
};
Light FrameLight() {
    Light l;
    double hue = std::fmod(fclock.siner / 4, 255.0);
    Color played = FromRef(WaveformPlayedColor());
    if (look.drColors) {
        l.col = Hsv(hue, 160 + std::sin(fclock.siner / 32) * 60, 255);
        l.backdrop = Hsv(hue, 255, std::sin(fclock.siner / 16) * 40 + 60);
    } else {
        l.col = played;
        l.backdrop = Scale(played, (float)(0.18 + std::sin(fclock.siner / 16) * 0.08));
    }
    l.warn = g.playing != kNone && (g_lib[g.playing].Failed() || g.stopAfterCurrent);
    if (l.warn) l.col = Mix(l.col, FromRgb(P().error), 0.6f);
    if (!look.glow) l.backdrop = {0, 0, 0};
    Rgb rest = DockRest();
    l.surround = look.black ? Color{0, 0, 0} : FromRef(SurfaceColor());
    l.rest = rest == kThemeColor ? Color{0.42f, 0.42f, 0.42f} : FromRgb(rest);
    return l;
}

// What hasn't played yet: a little dimmer and greyer, so the column stays alive all the way down
// while the playhead still reads; or carried by the position's own unplayed color.
Color Unplayed(Color c, const Light& l) {
    if (DockRest() == kThemeColor) {
        float lum = (c.r + c.g + c.b) / 3;
        return Scale(Mix(c, {lum, lum, lum}, 0.35f), 0.68f);
    }
    float lum = (c.r + c.g + c.b) / 3;
    return Mix(Scale(c, 0.3f), Scale(l.rest, lum * 1.4f), 0.7f);
}

// Each row's loudness, 0..1, softened so the column reads as water rather than steps. Before the
// peaks are known the fountain stands at a steady width, as it does in the game.
std::vector<float> RowLevels(int height, int top, int bottom) {
    std::vector<float> raw(height, 0.55f), out(height);
    int len = std::max(1, bottom - top);
    std::array<uint8_t, 256> live{};
    if (g.liveFountain) g.engine.WaveformPeaks(live.data(), live.size());
    if (g.liveFountain || WaveformShown()) {
        for (int y = 0; y < height; ++y) {
            int r = std::clamp(y, top, bottom - 1) - top;
            if (g.liveFountain) {
                int first = std::clamp(r * 256 / len, 0, 255), last = std::clamp((r + 1) * 256 / len, first + 1, 256);
                uint8_t peak = 0;
                for (int b = first; b < last; ++b) peak = std::max(peak, live[b]);
                raw[y] = peak / 255.f;
            } else raw[y] = WaveformPeak((double)r / len, (double)(r + 1) / len) / 255.f;
        }
    }
    int k = std::max(1, S(2));
    for (int y = 0; y < height; ++y) {
        float sum = 0;
        int n = 0;
        for (int d = -k; d <= k; ++d, ++n) sum += raw[std::clamp(y + d, 0, height - 1)];
        out[y] = sum / n;
    }
    return out;
}

// DELTARUNE's Dark Fountain, its sprites drawn at their own scale as the game draws them: the
// scrolling texture in two layers, the rippling edge and its two echoes wobbling 12 game pixels
// apart, and the basin bobbing at the foot. The track's loudness moves each row's edge in or out
// (`hw`, the half-width at the edge's line, texel 90 of 100) rather than stretching the sprite,
// so the edge keeps the game's line work while the column swells with the music.
void RenderSprites(int w, int h, int playY, const std::vector<float>& level, const Light& l) {
    float cx = w / 2.f, maxHalf = std::max(4.f, w / 2.f - S(2));
    double s = maxHalf / 66;  // screen pixels per sprite pixel: the basin's 120 texels nearly span the bar
    // In a slim horizontal band, preserve broad water detail instead of shrinking the texture
    // into pixel noise. The outline and basin still fit the actual thickness of the player.
    double detailScale = SeekVertical() ? s : std::max(s, 0.6 * S(1));
    float minHalf = maxHalf * (g.liveFountain ? 0.16f : 0.5f), hwFoot = (float)(57 * s);
    maxHalf *= 0.9f;  // the column's widest, short of the bar's sides so the dark shows around it
    // Live water leaves room for the shared audio swell; the game's idle wobble stays subtle.
    double wob = std::sin(fclock.siner / 16) * (g.liveFountain ? 1.8 : 6);
    double edgeScroll = fclock.bgsiner * 20;       // 280 game pixels per 7 bgsiner
    // The basin's rounded foot: its rows from 70, where its corners begin.
    double basinTop = h - basin.h * s, footTop = h - 70 * s, bob = std::sin(fclock.siner / 16);
    for (int y = 0; y < h; ++y) {
        float hw = minHalf + (maxHalf - minHalf) * level[y];
        // Over the last stretch the column settles to the basin's width, so it pours into its U.
        float settle = std::clamp((float)((y - (footTop - 24 * s)) / (24 * s)), 0.f, 1.f);
        hw += (hwFoot - hw) * settle * settle * (3 - 2 * settle);
        int ev = (int)std::floor(y / detailScale + edgeScroll);
        int t1v = (int)std::floor(y / detailScale + fclock.siner / 2), t2v = (int)std::floor(y / detailScale - fclock.siner / 2);
        uint32_t* row = bits + (size_t)y * w;
        for (int x = 0; x < w; ++x) {
            double dx = x + 0.5 - cx, past = (std::fabs(dx) - hw) / s;  // texels outside the edge's line
            // The edge sprite's right side for the right half, its left side for the left half.
            auto edgeU = [&](double shift) {
                return (int)std::floor(dx >= 0 ? std::max(50.0, 90 + past) + shift : std::min(49.0, 10 - past) + shift);
            };
            Color c = l.surround;
            if (past < 10) {
                c = l.backdrop;
                c = Over(c, Sample(texture, (int)std::floor(x / detailScale + fclock.siner / 2), t1v, true, true), l.col, 0.7f, l.surround);
                c = Over(c, Sample(texture, (int)std::floor(x / detailScale + 120 - fclock.hscroll / 2), t2v, true, true), l.col, 0.3f, l.surround);
                c = Over(c, Sample(edge, edgeU(0), ev, false, true), l.col, 1, l.surround);
            }
            c = Over(c, Sample(edge, edgeU(-wob), ev, false, true), l.col, 0.5f, l.surround);
            c = Over(c, Sample(edge, edgeU(wob), ev, false, true), l.col, 0.5f, l.surround);
            if (y >= footTop) {
                int bu = (int)std::floor(basin.w / 2.0 + dx / s);
                double bv = (y - basinTop) / s;
                c = Over(c, Sample(basin, bu, (int)std::floor(bv + 4 - bob * 4), false, false), l.col, 0.3f, l.surround);
                c = Over(c, Sample(basin, bu, (int)std::floor(bv + 2 - bob * 2), false, false), l.col, 0.5f, l.surround);
                c = Over(c, Sample(basin, bu, (int)std::floor(bv), false, false), l.col, 1, l.surround);
            }
            if (y > playY) c = Unplayed(c, l);
            row[x] = Pack(c);
        }
    }
}

uint32_t Hash(uint32_t v) {
    v ^= v >> 16;
    v *= 0x7feb352d;
    v ^= v >> 15;
    v *= 0x846ca68b;
    return v ^ (v >> 16);
}

// Fountain bars: the plain waveform's bars, lit the fountain's way. The hue runs up the column a
// band at a time, each bar's length ripples with a wave rising through them, the bars' ends burn
// white, a glow of the backdrop fills the middle and sparks drift up.
void RenderBars(int w, int h, int playY, const std::vector<float>& level, const Light& l) {
    float cx = w / 2.f, half = std::max(2.f, w / 2.f - S(3));
    int bar = std::max(2, S(2)), step = bar + std::max(1, S(1));
    double hue = std::fmod(fclock.siner / 4, 255.0), sat = 160 + std::sin(fclock.siner / 32) * 60;
    for (int y = 0; y < h; ++y) {
        uint32_t* row = bits + (size_t)y * w;
        bool onBar = (y % step) < bar;
        int b0 = y - y % step;  // the bar's first row: the whole bar shares one length
        float rippleDepth = g.liveFountain ? 0.03f : 0.14f;
        float ripple = 1 - rippleDepth + rippleDepth * (float)std::sin(b0 / (8.0 * S(1)) + fclock.siner / 5);
        float len = std::max((float)S(1), half * std::max(level[b0], 0.04f) * ripple);
        // Hue bands that rise: constant where y / 4 + siner / 4 is.
        Color col = look.drColors ? Hsv(hue + b0 * 0.6 / S(1), sat, 255) : l.col;
        if (look.drColors && l.warn) col = Mix(col, FromRgb(P().error), 0.6f);
        for (int x = 0; x < w; ++x) {
            float d = std::fabs(x + 0.5f - cx);
            float glow = std::max(0.f, 1 - (d / half) * (d / half));
            Color c = Mix(l.surround, l.backdrop, glow);
            if (onBar && d <= len) {
                float tip = std::clamp((d - (len - 2.5f * S(1))) / (2.5f * S(1)), 0.f, 1.f);  // the white-hot end
                c = Mix(Scale(col, 0.75f + 0.25f * (d / std::max(1.f, len))), {1, 1, 1}, tip * 0.7f);
            }
            if (y > playY) c = Unplayed(c, l);
            row[x] = Pack(c);
        }
    }
    // Sparks: each rises at its own pace from the foot of the column, wavering, fading as it climbs.
    for (uint32_t i = 0; i < 28; ++i) {
        uint32_t r = Hash(i * 2654435761u + 17);
        double rise = 0.6 + (r & 0xFF) / 255.0 * 1.4;  // game pixels a frame
        double travel = std::fmod(fclock.siner * rise * S(1) + (r >> 8 & 0xFFF), (double)h + S(20));
        int y = (int)(h - travel);
        float x0 = cx + ((int)(r >> 20 & 0xFF) - 128) / 128.f * half * 0.8f;
        int x = (int)(x0 + std::sin(fclock.siner / 10 + i) * S(3));
        float fade = (float)std::clamp(travel / h, 0.0, 1.0);
        Color spark = Mix({1, 1, 1}, look.drColors ? Hsv(hue + i * 9, sat, 255) : l.col, 0.4f);
        int size = std::max(1, S(1) + (int)(r >> 28 & 1));
        for (int dy = 0; dy < size; ++dy)
            for (int dx = 0; dx < size; ++dx) {
                int px = x + dx, py = y + dy;
                if (px < 0 || py < 0 || px >= w || py >= h) continue;
                uint32_t c = bits[(size_t)py * w + px];
                Color under{(c >> 16 & 0xFF) / 255.f, (c >> 8 & 0xFF) / 255.f, (c & 0xFF) / 255.f};
                Color lit = Mix(under, spark, 1 - fade);
                bits[(size_t)py * w + px] = Pack(py > playY ? Unplayed(lit, l) : lit);
            }
    }
}

void StopClock() {
    if (fclock.running && g.seek) KillTimer(g.seek, kFountainTimer);
    fclock.running = false;
}

void SaveLook() {
    IniWrite(L"Waveform", L"Left", std::to_wstring((int)look.style));
    IniWrite(L"Waveform", L"Motion", look.motion ? L"1" : L"0");
    IniWrite(L"Waveform", L"DeltaruneColors", look.drColors ? L"1" : L"0");
    IniWrite(L"Waveform", L"Glow", look.glow ? L"1" : L"0");
    IniWrite(L"Waveform", L"Black", look.black ? L"1" : L"0");
    const wchar_t* names[] = {L"Top", L"Bottom", L"Left", L"Right"};
    auto hex = [](Rgb c) {
        if (c == kThemeColor) return std::wstring();
        wchar_t s[8];
        swprintf_s(s, L"%06X", c & 0xFFFFFF);
        return std::wstring(s);
    };
    for (int i = 0; i < (int)Dock::Count; ++i) {
        IniWrite(L"Waveform", (std::wstring(names[i]) + L"Played").c_str(), hex(look.played[i]));
        IniWrite(L"Waveform", (std::wstring(names[i]) + L"Unplayed").c_str(), hex(look.rest[i]));
    }
}

// The color dialog, starting from `current`; false when cancelled.
bool PickColor(Rgb current, Rgb& out) {
    static COLORREF custom[16] = {RGB(0, 255, 0), RGB(255, 0, 255), RGB(0, 255, 255), RGB(255, 255, 0)};
    CHOOSECOLORW cc{};
    cc.lStructSize = sizeof cc;
    cc.hwndOwner = g.hwnd;
    cc.lpCustColors = custom;
    cc.rgbResult = Ref(current);
    cc.Flags = CC_FULLOPEN | CC_RGBINIT | CC_ANYCOLOR;
    if (!ChooseColorW(&cc)) return false;
    out = (Rgb)(GetRValue(cc.rgbResult) << 16 | GetGValue(cc.rgbResult) << 8 | GetBValue(cc.rgbResult));
    return true;
}

}  // namespace

// [Waveform]: Left (the shared style; legacy key), Motion, DeltaruneColors, Glow, Black, and <Position>Played /
// <Position>Unplayed as RRGGBB (empty: the theme's).
void LoadWaveLook() {
    look.style = (WaveStyle)std::clamp(IniInt(L"Waveform", L"Left", (int)WaveStyle::DarkFountain), 0, (int)WaveStyle::Count - 1);
    look.motion = IniInt(L"Waveform", L"Motion", 1) != 0;
    look.drColors = IniInt(L"Waveform", L"DeltaruneColors", 1) != 0;
    look.glow = IniInt(L"Waveform", L"Glow", 1) != 0;
    look.black = IniInt(L"Waveform", L"Black", 1) != 0;
    const wchar_t* names[] = {L"Top", L"Bottom", L"Left", L"Right"};
    auto color = [](const std::wstring& key) {
        std::wstring s = IniRead(L"Waveform", key.c_str(), L"");
        wchar_t* end = nullptr;
        unsigned long v = s.size() == 6 ? wcstoul(s.c_str(), &end, 16) : 0;
        return end && *end == 0 ? (Rgb)v : kThemeColor;
    };
    for (int i = 0; i < (int)Dock::Count; ++i) {
        look.played[i] = color(std::wstring(names[i]) + L"Played");
        look.rest[i] = color(std::wstring(names[i]) + L"Unplayed");
    }
}

// The color of what has played on the plain waveform: the position's own while playing, else the
// state's (playing accent, paused amber, warning red).
COLORREF WaveformPlayedColor() {
    Rgb own = DockPlayed();
    bool warn = g.playing != kNone && (g_lib[g.playing].Failed() || g.stopAfterCurrent);
    if (warn) return Ref(P().error);
    if (g.engine.Paused()) return Ref(P().amber);
    Rgb theme = ThemeInfo(g.themeMode).wavePlayed;  // a pack theme's own waveform colours
    return own != kThemeColor ? Ref(own) : theme != kNoRgb ? Ref(theme) : Ref(P().accent);
}

COLORREF WaveformRestColor() {
    // Every live sample has already been heard. Song length must not dim its waveform.
    if (g.fullLiveWaveform && LiveWaveformShown()) return WaveformPlayedColor();
    Rgb own = DockRest();
    Rgb theme = ThemeInfo(g.themeMode).waveRest;
    return own != kThemeColor ? Ref(own) : theme != kNoRgb ? Ref(theme) : Ref(g.dark ? P().faint : P().border);
}

bool FountainShown() {
    return (g.liveFountain || !LiveWaveformShown()) && g.themeMode == ThemeMode::Deltarune && look.style != WaveStyle::Bars &&
           g.size == ViewSize::Normal && g.layout.wave && g.seek;
}

// Render along a vertical time axis, then turn the image for a top/bottom player. The horizontal
// stream keeps the same sprite detail and rippling outline, with its basin at the right end.
// `top` and `bottom` are the channel's time-axis endpoints in either orientation.
void PaintFountain(HDC dc, const RECT& client, int top, int bottom, double frac) {
    bool vertical = SeekVertical();
    int w = vertical ? client.right - client.left : client.bottom - client.top;
    int h = vertical ? client.bottom - client.top : client.right - client.left;
    int origin = vertical ? client.top : client.left;
    if (w <= 0 || h <= 0) return;
    if (!dib || w != dibW || h != dibH) {
        if (dib) DeleteObject(dib);
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
        void* p = nullptr;
        dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &p, nullptr, 0);
        bits = (uint32_t*)p;
        dibW = dib ? w : 0;
        dibH = dib ? h : 0;
        if (!dib) return;
    }
    std::vector<float> level = RowLevels(h, top - origin, bottom - origin);
    int positionY = g.playing == kNone ? -1 : top - origin + (int)((bottom - top) * frac);
    int markerY = g.liveFountain ? -1 : positionY;
    int playY = RadioPlaying() || (g.fullLiveWaveform && g.liveFountain && g.playing != kNone) ? h : positionY;
    Light l = FrameLight();
    if (look.style == WaveStyle::DarkFountain && SpritesReady())
        RenderSprites(w, h, playY, level, l);
    else
        RenderBars(w, h, playY, level, l);
    // The playhead: a line of the fountain's light across the column.
    if (markerY >= 0 && markerY < h)
        for (int x = 0; x < w; ++x) {
            uint32_t c = bits[(size_t)markerY * w + x];
            Color under{(c >> 16 & 0xFF) / 255.f, (c >> 8 & 0xFF) / 255.f, (c & 0xFF) / 255.f};
            bits[(size_t)markerY * w + x] = Pack(Mix(under, Mix(l.col, {1, 1, 1}, 0.4f), 0.6f));
        }
    GdiFlush();
    HDC mem = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(mem, dib);
    if (vertical) {
        BitBlt(dc, client.left, client.top, w, h, mem, 0, 0, SRCCOPY);
    } else {
        // Exact quarter turn: time still advances left to right, with no stretched sprites.
        POINT corners[] = {{client.left, client.bottom}, {client.left, client.top}, {client.right, client.bottom}};
        PlgBlt(dc, corners, mem, 0, 0, w, h, nullptr, 0, 0);
    }
    SelectObject(mem, old);
    DeleteDC(mem);
}

// WaveformTick's share, four times a second: start the clock when the fountain should move, and
// let it run down when it shouldn't (FountainStep stops it once it has).
void FountainWake() {
    bool visible = FountainShown() && IsWindowVisible(g.hwnd) && !IsIconic(g.hwnd);
    if (!visible) {
        StopClock();
        return;
    }
    if (!fclock.running && (LiveInputMoving() || Moving() || fclock.speed > 0.001)) {
        fclock.last = GetTickCount64();
        fclock.running = SetTimer(g.seek, kFountainTimer, 33, nullptr) != 0;
    }
}

// kFountainTimer on the seek bar: advance the game's clock by the frames that elapsed. True when
// the fountain changed and should repaint.
bool FountainStep(UINT_PTR id) {
    if (id != kFountainTimer) return false;
    ULONGLONG now = GetTickCount64();
    double frames = std::min(4.0, (now - fclock.last) * kFps / 1000);
    fclock.last = now;
    double target = Moving() ? 1 : 0;
    // Playing it eases in; sealed (paused, stopped) it slows by 0.02 a frame as in the game.
    fclock.speed = target > fclock.speed ? std::min(1.0, fclock.speed + 0.05 * frames) : std::max(0.0, fclock.speed - 0.02 * frames);
    double d = fclock.speed * frames;
    fclock.siner += d;
    fclock.hscroll = std::fmod(fclock.hscroll + d, 240.0);
    fclock.bgsiner = std::fmod(fclock.bgsiner + d * 0.0625, 7.0);
    if (fclock.speed <= 0 && target == 0 && !LiveInputMoving()) StopClock();
    if (!FountainShown()) StopClock();
    return true;
}

// The Waveform submenu's share: the waveform's style, the fountain's toggles and the
// positions' colors.
void AppendWaveLookMenu(HMENU wave) {
    if (!kExtraThemes) return;  // the fountain styles are the DELTARUNE theme's
    HMENU style = CreatePopupMenu();
    AppendMenuW(style, MF_STRING, IDM_WAVE_STYLE_BASE + (int)WaveStyle::Bars, L"&Bars");
    AppendMenuW(style, MF_STRING, IDM_WAVE_STYLE_BASE + (int)WaveStyle::FountainBars, L"Fountain &bars");
    AppendMenuW(style, MF_STRING, IDM_WAVE_STYLE_BASE + (int)WaveStyle::DarkFountain,
                L"&Dark Fountain (DELTARUNE)");
    AppendMenuW(style, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(style, MF_STRING, IDM_WAVE_MOTION, L"&Moving");
    AppendMenuW(style, MF_STRING, IDM_WAVE_DR_COLORS, L"DELTARUNE &colors");
    AppendMenuW(style, MF_STRING, IDM_WAVE_GLOW, L"Fountain &glow");
    AppendMenuW(style, MF_STRING, IDM_WAVE_BLACK, L"Black &surroundings");
    AppendMenuW(wave, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(wave, MF_POPUP, (UINT_PTR)style, L"Waveform &style");
    HMENU colors = CreatePopupMenu();
    const wchar_t* docks[] = {L"At the &top", L"At the &bottom", L"On the &left", L"On the &right"};
    for (int i = 0; i < (int)Dock::Count; ++i) {
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING, IDM_WAVE_COLOR_BASE + i * 3, L"&Played color...");
        AppendMenuW(m, MF_STRING, IDM_WAVE_COLOR_BASE + i * 3 + 1, L"&Unplayed color...");
        AppendMenuW(m, MF_STRING, IDM_WAVE_COLOR_BASE + i * 3 + 2, L"&Theme colors");
        AppendMenuW(colors, MF_POPUP, (UINT_PTR)m, docks[i]);
    }
    AppendMenuW(wave, MF_POPUP, (UINT_PTR)colors, L"&Colors");
}

void UpdateWaveLookMenu(HMENU m) {
    CheckMenuRadioItem(m, IDM_WAVE_STYLE_BASE, IDM_WAVE_STYLE_BASE + (int)WaveStyle::Count - 1,
                       IDM_WAVE_STYLE_BASE + (int)look.style, MF_BYCOMMAND);
    auto check = [&](UINT id, bool on) { CheckMenuItem(m, id, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED)); };
    check(IDM_WAVE_MOTION, look.motion);
    check(IDM_WAVE_DR_COLORS, look.drColors);
    check(IDM_WAVE_GLOW, look.glow);
    check(IDM_WAVE_BLACK, look.black);
    bool fountain = look.style != WaveStyle::Bars;
    for (UINT id : {IDM_WAVE_MOTION, IDM_WAVE_DR_COLORS, IDM_WAVE_GLOW, IDM_WAVE_BLACK})
        EnableMenuItem(m, id, MF_BYCOMMAND | (fountain ? MF_ENABLED : MF_GRAYED));
    for (int i = 0; i < (int)Dock::Count; ++i) {
        check(IDM_WAVE_COLOR_BASE + i * 3, look.played[i] != kThemeColor);
        check(IDM_WAVE_COLOR_BASE + i * 3 + 1, look.rest[i] != kThemeColor);
        check(IDM_WAVE_COLOR_BASE + i * 3 + 2, look.played[i] == kThemeColor && look.rest[i] == kThemeColor);
    }
}

// OnCommand's share: true when `id` was one of the waveform's look commands.
bool WaveLookCommand(int id) {
    if (id >= IDM_WAVE_STYLE_BASE && id < IDM_WAVE_STYLE_BASE + (int)WaveStyle::Count) {
        look.style = (WaveStyle)(id - IDM_WAVE_STYLE_BASE);
    } else if (id == IDM_WAVE_MOTION) {
        look.motion = !look.motion;
    } else if (id == IDM_WAVE_DR_COLORS) {
        look.drColors = !look.drColors;
    } else if (id == IDM_WAVE_GLOW) {
        look.glow = !look.glow;
    } else if (id == IDM_WAVE_BLACK) {
        look.black = !look.black;
    } else if (id >= IDM_WAVE_COLOR_BASE && id < IDM_WAVE_COLOR_BASE + (int)Dock::Count * 3) {
        int dock = (id - IDM_WAVE_COLOR_BASE) / 3, what = (id - IDM_WAVE_COLOR_BASE) % 3;
        if (what == 2) {
            look.played[dock] = look.rest[dock] = kThemeColor;
        } else {
            Rgb& c = what == 0 ? look.played[dock] : look.rest[dock];
            Rgb start = c != kThemeColor ? c : what == 0 ? P().accent : (g.dark ? P().faint : P().border);
            if (!PickColor(start, c)) return true;
        }
    } else {
        return false;
    }
    SaveLook();
    FountainWake();
    InvalidateRect(g.seek, nullptr, FALSE);
    return true;
}

}  // namespace app
