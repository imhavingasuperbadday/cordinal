// The equalizer on Linux (Playback > Equalizer), as the Windows build's app_eq.cpp: it edits g.settings.eq
// and is heard at once. The response curve as the engine builds it, the preamp and ten band sliders (or an
// imported AutoEq profile's filters), the presets, crossfeed and automatic preamp. Touching a slider turns
// the EQ on, as on Windows; settings.ini is written when a drag ends or a choice is made.
#include "panels_internal.h"

#include "dsp.h"

namespace lx::panel {
namespace {

constexpr double kGraphLow = 20, kGraphHigh = 20000, kGraphRange = 18;  // Hz, Hz, +/- dB, as on Windows

struct EqPanel {
    Rect panel, enable, crossfeed, autoPreamp, graph, import, reset, close, useSliders, headphones;
    Rect sliders[kEqBands + 1];  // [0] = preamp; each slider's channel
    std::vector<Rect> presets;
    int drag = -1;        // the slider being dragged, or kEqBands + 1 for the curve
    int focus = 1;        // the slider the arrow keys move
} g_eq;

// The response curve's points cost a filter design and some 300 responses, so they are kept until the
// settings, the rate or the graph change instead of being worked out on every redraw (every mouse move).
struct CurveCache {
    EqSettings eq;
    unsigned rate = 0;
    Rect r;
    float step = 0;
    std::vector<float> y;
} g_curve;

EqSettings& Eq() { return g.settings.eq; }

float* EqValue(int i) { return i == 0 ? &Eq().preamp : &Eq().gains[i - 1]; }
float EqLow(int i) { return i == 0 ? -24.0f : -12.0f; }
float EqHigh(int i) { return 12.0f; }

unsigned EqRate() {
    unsigned rate = g.engine.Rate();
    return rate ? rate : 48000;
}

// An edit is heard at once; settings.ini follows when it is finished.
void EqEdited(bool finished) {
    Eq().enabled = true;
    ApplyEq();
    if (finished) SaveSettings();
    g.redraw = true;
}

void EqChoice() {  // a switch or button: heard and saved
    ApplyEq();
    SaveSettings();
    g.redraw = true;
}

void SetEqValue(int i, double v, bool finished) {
    float* value = EqValue(i);
    float next = (float)std::clamp(std::round(v * 2) / 2, (double)EqLow(i), (double)EqHigh(i));
    if (next == *value && !finished) return;
    *value = next;
    EqEdited(finished);
}

double SliderValueAt(int i, float y) {
    const Rect& r = g_eq.sliders[i];
    double t = std::clamp((y - r.y) / std::max(1.0f, r.h), 0.0f, 1.0f);
    return EqHigh(i) - t * (EqHigh(i) - EqLow(i));
}

// Turns the preamp down by the curve's highest boost, so a boosted EQ has headroom instead of
// leaning on the limiter. A curve that only cuts gets 0 dB.
void AutoPreamp() {
    EqSettings curve = Eq();
    curve.enabled = true;
    curve.preamp = 0;
    Equalizer eq;
    eq.Configure(curve, EqRate());
    Eq().preamp = -(float)(std::ceil(std::max(0.0, eq.PeakGainDb()) * 10) / 10);
    EqEdited(true);
}

void ImportAutoEq(const std::wstring& path) {
    std::string bytes = ReadSmallFile(path, 65536);
    if (bytes.empty()) {
        ShowMessage(L"Couldn't read the profile (maximum 64 KiB).");
        return;
    }
    if (bytes.starts_with("\xFF\xFE") && bytes.size() % 2 == 0) {  // UTF-16 from Windows tools
        std::wstring wide;
        for (size_t i = 2; i + 1 < bytes.size(); i += 2) wide += (wchar_t)((uint8_t)bytes[i] | (uint8_t)bytes[i + 1] << 8);
        bytes = ToUtf8(wide);
    }
    std::wstring error;
    EqSettings settings = Eq();
    if (!ParseAutoEq(bytes, settings, error)) {
        ShowMessage(error);
        return;
    }
    Eq() = settings;
    Eq().enabled = true;
    EqChoice();
    ShowMessage(L"Imported " + std::to_wstring(Eq().filterCount) + L" filters from " + FileNameOf(path) + L".");
}

std::string Db(float v) {
    char buf[16];
    snprintf(buf, sizeof(buf), v == 0 ? "0" : "%+.1f", v);
    return buf;
}

std::string Hz(double hz) {
    char buf[16];
    if (hz >= 1000) snprintf(buf, sizeof(buf), "%gk", std::round(hz / 100) / 10);
    else snprintf(buf, sizeof(buf), "%g", std::round(hz));
    return buf;
}

double GraphX(const Rect& r, double hz) { return r.x + std::log(hz / kGraphLow) / std::log(kGraphHigh / kGraphLow) * r.w; }
double GraphY(const Rect& r, double db) { return r.y + (kGraphRange - db) / (2 * kGraphRange) * r.h; }

// The curve as the engine builds it (the filters as designed, overlap included), drawn even while
// the EQ is off (dimmed) so it can be shaped first.
void DrawCurve(const Palette& p) {
    const Rect& r = g_eq.graph;
    Fill(r, p.bg);
    Frame(r, p.border);
    for (double hz : {31.0, 62.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0}) {
        float x = std::round((float)GraphX(r, hz));
        Fill({x, r.y + S(1), S(1), r.h - S(2)}, p.border);
    }
    for (int db = -12; db <= 12; db += 6) {
        float y = std::round((float)GraphY(r, db));
        Fill({r.x + S(1), y, r.w - S(2), S(1)}, db ? p.border : p.faint);
        DrawText(Font::Small, db > 0 ? "+" + std::to_string(db) : std::to_string(db), r.x - S(34), y - LineHeight(Font::Small) / 2,
                 p.sub, S(30), 2);
    }
    EqSettings curve = Eq();
    curve.enabled = true;
    const unsigned rate = EqRate();
    const float step = S(2);
    CurveCache& c = g_curve;
    if (c.y.empty() || !(c.eq == curve) || c.rate != rate || c.step != step || c.r.x != r.x || c.r.y != r.y ||
        c.r.w != r.w || c.r.h != r.h) {
        Equalizer eq;
        eq.Configure(curve, rate);
        c.y.clear();
        for (float x = r.x + 1; x < r.R() - 1; x += step) {
            double hz = kGraphLow * std::pow(kGraphHigh / kGraphLow, (x - r.x) / r.w);
            c.y.push_back((float)GraphY(r, std::clamp(eq.FilterDb(hz), -kGraphRange - 1, kGraphRange + 1)));
        }
        c.eq = curve;
        c.rate = rate;
        c.r = r;
        c.step = step;
    }
    uint32_t color = Eq().enabled ? p.accent : p.faint;
    SDL_Rect clip{(int)r.x, (int)r.y, (int)r.w, (int)r.h};
    SDL_SetRenderClipRect(g.renderer, &clip);
    float x = r.x + 1, lastX = x;
    for (size_t i = 0; i < c.y.size(); lastX = x, x += step, ++i)
        if (i) ThickLine(lastX, c.y[i - 1], x, c.y[i], step, color);
    if (!Eq().parametric)
        for (int i = 0; i < kEqBands; ++i) {
            float x = (float)GraphX(r, kEqFrequencies[i]), y = (float)GraphY(r, Eq().gains[i]);
            float rad = i + 1 == g_eq.focus ? S(5) : S(3);
            Polygon({{x, y - rad}, {x + rad, y}, {x, y + rad}, {x - rad, y}}, color);
        }
    SDL_SetRenderClipRect(g.renderer, nullptr);
}

void DrawSlider(int i, const std::string& name, const Palette& p) {
    const Rect& r = g_eq.sliders[i];
    float v = *EqValue(i), frac = (EqHigh(i) - std::clamp(v, EqLow(i), EqHigh(i))) / (EqHigh(i) - EqLow(i));
    float cx = std::round(r.x + r.w / 2), zero = r.y + (EqHigh(i) / (EqHigh(i) - EqLow(i))) * r.h;
    bool hot = g_eq.drag == i || Rect{r.x, r.y - S(6), r.w, r.h + S(12)}.In(g_mx, g_my);
    Fill({cx - S(2), r.y, S(4), r.h}, p.border);
    float y = r.y + frac * r.h;
    Fill({cx - S(2), std::min(y, zero), S(4), std::fabs(y - zero)}, Eq().enabled ? p.accent : p.faint);
    Fill({cx - S(8), std::round(y - S(3)), S(16), S(6)}, hot || i == g_eq.focus ? p.accent : p.text);
    DrawText(Font::Small, Db(v), r.x - S(6), r.y - S(20), i == g_eq.focus ? p.text : p.sub, r.w + S(12), 1);
    DrawText(Font::Small, name, r.x - S(6), r.B() + S(6), p.sub, r.w + S(12), 1);
}

void DrawEq(const Palette& p) {
    Rect panel = g_eq.panel = DrawFrame(p, S(700), S(560), "Equalizer");
    float x = panel.x + S(16), right = panel.R() - S(16), y = panel.y + S(48);
    g_eq.enable = {x, y, S(110), S(24)};
    g_eq.crossfeed = {x + S(120), y, S(200), S(24)};
    g_eq.autoPreamp = {right - S(120), y, S(120), S(26)};
    DrawCheck(g_eq.enable, "Enabled", Eq().enabled, p);
    DrawCheck(g_eq.crossfeed, "Headphone crossfeed", Eq().crossfeed, p);
    DrawButton(g_eq.autoPreamp, "Auto preamp", p);
    y += S(36);
    g_eq.graph = {x + S(38), y, right - x - S(38), S(130)};
    DrawCurve(p);
    for (int i = 0; i < kEqBands; ++i) {
        float gx = (float)GraphX(g_eq.graph, kEqFrequencies[i]);
        DrawText(Font::Small, Hz(kEqFrequencies[i]), gx - S(20), g_eq.graph.B() + S(2), p.faint, S(40), 1);
    }
    y = g_eq.graph.B() + S(46);
    // The preamp, then the bands (or the imported filters) to its right.
    float colW = (right - x) / (kEqBands + 2), sliderH = S(110);
    g_eq.sliders[0] = {x, y, colW, sliderH};
    DrawSlider(0, "Preamp", p);
    if (!Eq().parametric) {
        for (int i = 1; i <= kEqBands; ++i) {
            g_eq.sliders[i] = {x + colW * (i + 1), y, colW, sliderH};
            DrawSlider(i, Hz(kEqFrequencies[i - 1]), p);
        }
        g_eq.useSliders = {};
    } else {
        for (int i = 1; i <= kEqBands; ++i) g_eq.sliders[i] = {};
        float lx = x + colW * 2, ly = y - S(18);
        DrawText(Font::Normal, "Parametric: " + std::to_string(Eq().filterCount) + " filters from an AutoEq profile", lx, ly, p.text);
        ly += S(24);
        float colX = lx;
        for (int i = 0; i < Eq().filterCount; ++i) {
            const EqFilter& f = Eq().filters[i];
            const char* type = f.type == EqFilterType::LowShelf ? "Low shelf" : f.type == EqFilterType::HighShelf ? "High shelf" : "Peak";
            char line[96];
            snprintf(line, sizeof(line), "%s  %s Hz  %s dB  Q %.2f", type, Hz(f.frequency).c_str(), Db(f.gain).c_str(), f.q);
            DrawText(Font::Small, line, colX, ly, f.enabled ? p.sub : p.faint, (right - lx) / 2 - S(8));
            ly += S(16);
            if (ly > y + sliderH - S(4)) {
                if (colX != lx) break;
                colX = lx + (right - lx) / 2;
                ly = y + S(6);
            }
        }
        g_eq.useSliders = {right - S(110), y + sliderH + S(4), S(110), S(26)};
        DrawButton(g_eq.useSliders, "Use sliders", p);
    }
    y += sliderH + S(40);
    DrawText(Font::Normal, "Presets", x, y + S(4), p.sub);
    float px = x + S(70);
    g_eq.presets.clear();
    for (const EqPreset& preset : AllEqPresets()) {
        std::string name = Utf8(preset.name);
        float w = TextWidth(Font::Normal, name) + S(20);
        if (px + w > right) {
            px = x + S(70);
            y += S(30);
        }
        Rect chip{px, y, w, S(26)};
        bool on = !Eq().parametric && Eq().gains == preset.gains;
        if (on) Fill(chip, p.select);
        else Fill(chip, chip.In(g_mx, g_my) ? p.hover : p.surface2);
        Frame(chip, on ? p.accent : p.border);
        DrawText(Font::Normal, name, chip.x, TextY(chip), on ? p.selectText : p.text, chip.w, 1);
        g_eq.presets.push_back(chip);
        px += w + S(6);
    }
    float by = panel.B() - S(16) - S(28);
    g_eq.import = {x, by, S(140), S(28)};
    g_eq.reset = {x + S(150), by, S(90), S(28)};
    g_eq.close = {right - S(90), by, S(90), S(28)};
    DrawButton(g_eq.import, "Import AutoEq...", p);
    g_eq.headphones = {x + S(250), by, S(130), S(28)};
    DrawButton(g_eq.headphones, "Headphones...", p);
    DrawButton(g_eq.reset, "Reset", p);
    DrawButton(g_eq.close, "Close", p, true);
}

int EqSliderAt(float x, float y) {
    for (int i = 0; i <= kEqBands; ++i) {
        const Rect& r = g_eq.sliders[i];
        if (r.w > 0 && Rect{r.x, r.y - S(8), r.w, r.h + S(16)}.In(x, y)) return i;
    }
    return -1;
}

// In graphic mode a press on the curve sets the nearest band, as on Windows.
void EqCurveAt(float x, float y, bool finished) {
    if (Eq().parametric) return;
    const Rect& r = g_eq.graph;
    double hz = kGraphLow * std::pow(kGraphHigh / kGraphLow, std::clamp((x - r.x) / r.w, 0.0f, 1.0f));
    int band = 0;
    for (int i = 1; i < kEqBands; ++i)
        if (std::fabs(std::log(kEqFrequencies[i] / hz)) < std::fabs(std::log(kEqFrequencies[band] / hz))) band = i;
    double db = kGraphRange - (y - r.y) / std::max(1.0f, r.h) * 2 * kGraphRange;
    g_eq.focus = band + 1;
    SetEqValue(band + 1, db, finished);
}

void EqMouseDown(float x, float y, int clicks) {
    if (g_eq.headphones.In(x, y)) return OpenHeadphones();
    if (!g_eq.panel.In(x, y) || g_eq.close.In(x, y)) {
        ClosePanel();
        return;
    }
    if (g_eq.enable.In(x, y)) {
        Eq().enabled = !Eq().enabled;
        EqChoice();
    } else if (g_eq.crossfeed.In(x, y)) {
        Eq().crossfeed = !Eq().crossfeed;
        EqChoice();
    } else if (g_eq.autoPreamp.In(x, y)) {
        AutoPreamp();
    } else if (g_eq.import.In(x, y)) {
        static const SDL_DialogFileFilter filter{"AutoEq parametric profiles", "txt"};
        ChooseFile(kFileAutoEq, filter);
    } else if (g_eq.reset.In(x, y)) {
        Eq().preamp = 0;
        Eq().gains.fill(0);
        Eq().parametric = false;
        Eq().filterCount = 0;
        EqChoice();
    } else if (g_eq.useSliders.w > 0 && g_eq.useSliders.In(x, y)) {
        Eq().parametric = false;
        EqChoice();
    } else if (g_eq.graph.In(x, y)) {
        g_eq.drag = kEqBands + 1;
        EqCurveAt(x, y, false);
    } else if (int i = EqSliderAt(x, y); i >= 0) {
        g_eq.focus = i;
        if (clicks >= 2) SetEqValue(i, 0, true);  // a double click puts it back to 0 dB
        else {
            g_eq.drag = i;
            SetEqValue(i, SliderValueAt(i, y), false);
        }
    } else {
        for (size_t k = 0; k < g_eq.presets.size(); ++k)
            if (g_eq.presets[k].In(x, y)) {
                Eq().gains = AllEqPresets()[k].gains;
                Eq().parametric = false;
                Eq().enabled = true;
                EqChoice();
                break;
            }
    }
}

void EqKey(const SDL_KeyboardEvent& e) {
    bool shift = e.mod & SDL_KMOD_SHIFT;
    int last = Eq().parametric ? 0 : kEqBands;
    switch (e.key) {
        case SDLK_ESCAPE:
        case SDLK_RETURN: ClosePanel(); break;
        case SDLK_LEFT: g_eq.focus = std::clamp(g_eq.focus - 1, 0, last); break;
        case SDLK_RIGHT: g_eq.focus = std::clamp(g_eq.focus + 1, 0, last); break;
        case SDLK_UP:
        case SDLK_DOWN:
            g_eq.focus = std::min(g_eq.focus, last);
            SetEqValue(g_eq.focus, *EqValue(g_eq.focus) + (e.key == SDLK_UP ? 1 : -1) * (shift ? 3 : 0.5), true);
            break;
        case SDLK_0:
        case SDLK_KP_0:
            g_eq.focus = std::min(g_eq.focus, last);
            SetEqValue(g_eq.focus, 0, true);
            break;
        default: break;
    }
    g.redraw = true;
}

void EqMouseMove(float x, float y) {
    if (g_eq.drag < 0) return;
    if (g_eq.drag == kEqBands + 1) EqCurveAt(x, y, false);
    else SetEqValue(g_eq.drag, SliderValueAt(g_eq.drag, y), false);
}

void EqMouseUp() {
    if (g_eq.drag < 0) return;
    g_eq.drag = -1;
    SaveSettings();
}

void EqWheel(float x, float y, float dy) {
    if (int i = EqSliderAt(x, y); i >= 0) {
        g_eq.focus = i;
        SetEqValue(i, *EqValue(i) + (dy > 0 ? 0.5 : -0.5), true);
    }
}

void EqClose() {
    if (g_eq.drag >= 0) SaveSettings();
    g_eq.drag = -1;
}

}  // namespace

const Ops kEqOps{
    .draw = DrawEq,
    .mouseDown = EqMouseDown,
    .mouseMove = EqMouseMove,
    .mouseUp = EqMouseUp,
    .wheel = EqWheel,
    .key = EqKey,
    .file = [](int purpose, const std::wstring& path) { if (purpose == kFileAutoEq) ImportAutoEq(path); },
    .close = EqClose,
};

}  // namespace lx::panel

namespace lx {
using namespace panel;

void OpenEqualizer() {
    g_kind = Kind::Eq;
    g_eq.drag = -1;
    g.redraw = true;
}

}  // namespace lx
