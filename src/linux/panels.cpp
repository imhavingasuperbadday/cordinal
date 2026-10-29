// The equalizer, the tag editor and Preferences on Linux, drawn over the window with the SDL renderer. They do
// what the Windows build's dialogs do (app_eq.cpp, app_tageditor.cpp) with the same wording:
//
//  - The equalizer edits g.settings.eq and is heard at once: the response curve as the engine
//    builds it, the preamp and ten band sliders (or an imported AutoEq profile's filters), the
//    presets, crossfeed and automatic preamp. Touching a slider turns the EQ on, as on Windows;
//    settings.ini is written when a drag ends or a choice is made.
//  - The tag editor shows every writable field of one or many tracks. Fields that differ start
//    empty with a "(multiple values)" hint, and only the fields changed are written (tagwrite.cpp:
//    a worker, an undo journal, the playing file once playback moves on).
//  - Preferences (File > Preferences, Ctrl+,) lists the settings the Linux build has, as the
//    Windows window lists its Tweaks registry: pages, a search, help, defaults and resets.
//  - The auto-playlist rules (app_autoplaylist.cpp) and Library > Organize files (app_organize.cpp):
//    a folder and naming pattern with a preview of every move, carried out by organize_run.cpp.
//  - Library > Copy to folder or device (app_copy.cpp): a destination, a naming pattern, a format and
//    a preview of every file, copied by copy_run.cpp.
#include "panels.h"

#include "audio_output.h"
#include "cover.h"
#include "listenbrainz.h"
#include "playlog.h"
#include "points.h"
#include "draw.h"
#include "tags.h"
#include "tagwriter.h"
#include "titleformat.h"

#include <cstring>
#include <cwctype>
#include <optional>
#include <sys/statvfs.h>
#include <unordered_map>

namespace lx {

namespace {

enum class Kind { None, Eq, Tags, Prefs, Rules, Organize, Copy, ListenBrainz, Format, Device, Points, Review, Palette, Layout, Packs, Helpers, Games };
Kind g_kind = Kind::None;
float g_mx = -1, g_my = -1;

// --- shared pieces ---------------------------------------------------------------------------------
Rect OutputRect() {
    int pw = 0, ph = 0;
    SDL_GetRenderOutputSize(g.renderer, &pw, &ph);
    return {0, 0, (float)pw, (float)ph};
}

}  // namespace

// The dimmed window behind a dialog, and the dialog's own surface, centered.
Rect DrawFrame(const Palette& p, float w, float h, const char* title) {
    Rect out = OutputRect();
    Fill(out, p.bg, 190);
    w = std::min(out.w - S(16), w);
    h = std::min(out.h - S(16), h);
    Rect panel{std::round((out.w - w) / 2), std::round((out.h - h) / 2), w, h};
    Fill(panel, p.surface);
    Frame(panel, p.border);
    DrawText(Font::Title, title, panel.x + S(16), panel.y + S(12), p.text);
    return panel;
}

void DrawButton(const Rect& r, const std::string& label, const Palette& p, bool accent, bool enabled) {
    bool hot = enabled && r.In(g_mx, g_my);
    if (accent && enabled) Fill(r, hot ? p.accent2 : p.accent);
    else {
        Fill(r, hot ? p.hover : p.surface2);
        Frame(r, p.border);
    }
    uint32_t color = !enabled ? p.faint : accent ? TextOn(hot ? p.accent2 : p.accent) : p.text;
    DrawText(Font::Normal, label, r.x, TextY(r), color, r.w, 1);
}

namespace {

// A check box and its label; `state` 2 is mixed (some of the tracks), drawn as a bar.
void DrawCheck(const Rect& r, const std::string& label, int state, const Palette& p) {
    float box = S(14);
    Rect b{r.x, std::round(r.y + (r.h - box) / 2), box, box};
    if (state) Fill(b, p.accent);
    else {
        Fill(b, r.In(g_mx, g_my) ? p.hover : p.surface2);
        Frame(b, p.sub);
    }
    uint32_t mark = TextOn(p.accent);
    if (state == 1) {
        ThickLine(b.x + box * 0.22f, b.y + box * 0.52f, b.x + box * 0.42f, b.y + box * 0.72f, S(1.6f), mark);
        ThickLine(b.x + box * 0.42f, b.y + box * 0.72f, b.x + box * 0.80f, b.y + box * 0.30f, S(1.6f), mark);
    } else if (state == 2) {
        Fill({b.x + box * 0.25f, b.y + box * 0.45f, box * 0.5f, std::max(1.0f, S(2))}, mark);
    }
    DrawText(Font::Normal, label, b.R() + S(8), TextY(r), p.text, r.R() - b.R() - S(8));
}

void ChooseFile(int purpose, const SDL_DialogFileFilter& filter) {
    auto chosen = [](void* purpose, const char* const* list, int) {
        if (!list || !*list) return;
        auto* path = new std::wstring(compat::Widen(*list));
        if (!PostMessageW(nullptr, WM_APP_FILE, (WPARAM)(intptr_t)purpose, (LPARAM)path)) delete path;
    };
    SDL_ShowOpenFileDialog(chosen, (void*)(intptr_t)purpose, g.window, &filter, 1, nullptr, false);
}

std::string ReadSmallFile(const std::wstring& path, size_t max) {
    std::string bytes;
    if (FILE* f = fopen(compat::Narrow(path.c_str()).c_str(), "rb")) {
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && bytes.size() <= max) bytes.append(buf, n);
        fclose(f);
    }
    return bytes.size() > max ? std::string() : bytes;
}

// --- the equalizer ------------------------------------------------------------------------------------
constexpr double kGraphLow = 20, kGraphHigh = 20000, kGraphRange = 18;  // Hz, Hz, +/- dB, as on Windows

struct EqPanel {
    Rect panel, enable, crossfeed, autoPreamp, graph, import, reset, close, useSliders;
    Rect sliders[kEqBands + 1];  // [0] = preamp; each slider's channel
    std::vector<Rect> presets;
    int drag = -1;        // the slider being dragged, or kEqBands + 1 for the curve
    int focus = 1;        // the slider the arrow keys move
} g_eq;

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
    Equalizer eq;
    eq.Configure(curve, EqRate());
    uint32_t color = Eq().enabled ? p.accent : p.faint;
    SDL_Rect clip{(int)r.x, (int)r.y, (int)r.w, (int)r.h};
    SDL_SetRenderClipRect(g.renderer, &clip);
    float lastX = 0, lastY = 0;
    for (float x = r.x + 1; x < r.R() - 1; x += S(2)) {
        double hz = kGraphLow * std::pow(kGraphHigh / kGraphLow, (x - r.x) / r.w);
        float y = (float)GraphY(r, std::clamp(eq.FilterDb(hz), -kGraphRange - 1, kGraphRange + 1));
        if (x > r.x + 1) ThickLine(lastX, lastY, x, y, S(2), color);
        lastX = x;
        lastY = y;
    }
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

// --- the tag editor -------------------------------------------------------------------------------------
enum Field { Title, Artist, AlbumArtist, Album, Date, Genre, TrackNum, TrackTotal, Disc, DiscTotal, Composer, Grouping,
             Label, Bpm, Comment, Lyrics, FieldCount };
const char* const kLabels[] = {"Title", "Artist", "Album artist", "Album", "Year / date", "Genre", "Track", "of", "Disc", "of",
                               "Composer", "Grouping", "Label", "BPM", "Comment", "Lyrics"};
bool Numeric(int f) { return f == TrackNum || f == TrackTotal || f == Disc || f == DiscTotal || f == Bpm; }
bool Multiline(int f) { return f == Comment || f == Lyrics; }

struct TextField {
    std::wstring text;
    size_t caret = 0;
    bool all = false;  // everything selected (Ctrl+A, or entering by Tab): typing replaces it
    bool mixed = false, changed = false, enabled = true;
    Rect rect;
    float scrollX = 0, scrollY = 0;
};

struct TagPanel {
    std::vector<TrackId> ids;
    TextField fields[FieldCount];
    int focus = Title;
    int compilation = 0, initialCompilation = 0;  // 0 off, 1 on, 2 some of the tracks
    std::optional<std::string> cover;             // set when replaced (the image) or removed ("")
    SDL_Texture* preview = nullptr;
    int previewW = 0, previewH = 0;
    std::string info, warning;
    Rect panel, compilationRect, art, choose, remove, save, cancel;
} g_tags;

std::wstring TrackValue(TrackId id, int f) {
    const Track& t = g_lib[id];
    auto num = [](uint32_t v) { return v ? std::to_wstring(v) : std::wstring(); };
    switch (f) {
        case Title: return g_lib.Str(t.title);
        case Artist: return g_lib.Str(t.artist);
        case AlbumArtist: return g_lib.Str(t.albumArtist);
        case Album: return g_lib.Str(t.album);
        case Date: return g_lib.Str(t.date);
        case Genre: return g_lib.Str(t.genre);
        case TrackNum: return num(t.trackNo);
        case TrackTotal: return num(t.trackTotal);
        case Disc: return num(t.disc);
        case DiscTotal: return num(t.discTotal);
        case Composer: return g_lib.Str(t.composer);
        case Grouping: return g_lib.Str(t.grouping);
        case Label: return g_lib.Str(t.label);
        case Bpm: return num(t.bpm);
        case Comment: return g_lib.Str(t.comment);
        default: return {};
    }
}

void SetPreview(const std::string& bytes) {
    if (g_tags.preview) SDL_DestroyTexture(g_tags.preview);
    g_tags.preview = nullptr;
    CoverImage img{};
    if (bytes.empty() || !DecodeCover(bytes, 320, img)) return;
    g_tags.preview = SDL_CreateTexture(g.renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, img.width, img.height);
    if (!g_tags.preview) return;
    SDL_UpdateTexture(g_tags.preview, nullptr, img.rgba.data(), img.width * 4);
    SDL_SetTextureScaleMode(g_tags.preview, SDL_SCALEMODE_LINEAR);
    g_tags.previewW = img.width;
    g_tags.previewH = img.height;
}

void Focus(int f, bool selectAll) {
    if (f < 0 || f >= FieldCount || !g_tags.fields[f].enabled) return;
    g_tags.focus = f;
    TextField& t = g_tags.fields[f];
    t.caret = t.text.size();
    t.all = selectAll && !t.text.empty();
    SDL_StartTextInput(g.window);
}

// Builds the edit from the fields actually changed; untouched fields stay unset so the writers
// leave them alone. An empty edit just closes the editor.
void SaveTags() {
    auto text = [](int f) -> std::optional<std::string> {
        const TextField& t = g_tags.fields[f];
        if (!t.changed) return std::nullopt;
        size_t b = t.text.find_first_not_of(L" \t"), e = t.text.find_last_not_of(L" \t\n");
        return ToUtf8(b == std::wstring::npos ? std::wstring() : t.text.substr(b, e - b + 1));
    };
    auto number = [](int f) -> std::optional<uint32_t> {
        const TextField& t = g_tags.fields[f];
        if (!t.changed) return std::nullopt;
        return (uint32_t)std::max(0l, wcstol(t.text.c_str(), nullptr, 10));
    };
    TagEdit edit;
    edit.title = text(Title);
    edit.artist = text(Artist);
    edit.albumArtist = text(AlbumArtist);
    edit.album = text(Album);
    edit.date = text(Date);
    edit.genre = text(Genre);
    edit.composer = text(Composer);
    edit.grouping = text(Grouping);
    edit.label = text(Label);
    edit.comment = text(Comment);
    edit.lyrics = text(Lyrics);
    edit.track = number(TrackNum);
    edit.trackTotal = number(TrackTotal);
    edit.disc = number(Disc);
    edit.discTotal = number(DiscTotal);
    edit.bpm = number(Bpm);
    if (g_tags.compilation != g_tags.initialCompilation && g_tags.compilation != 2) edit.compilation = g_tags.compilation == 1;
    edit.cover = g_tags.cover;
    std::vector<TrackId> ids = g_tags.ids;
    ClosePanel();
    if (edit.Empty()) return;
    std::vector<std::pair<TrackId, TagEdit>> items;
    for (TrackId id : ids) items.emplace_back(id, edit);
    SubmitTagWrites(items, true);
}

void ChooseCover(const std::wstring& path) {
    std::string bytes = ReadSmallFile(path, 16 * 1024 * 1024);
    bool jpeg = bytes.size() >= 8 && (uint8_t)bytes[0] == 0xFF && (uint8_t)bytes[1] == 0xD8;
    bool png = bytes.size() >= 8 && bytes.compare(0, 4, "\x89PNG") == 0;
    if (!jpeg && !png) {
        ShowMessage(L"Covers have to be JPEG or PNG images.");
        return;
    }
    SetPreview(bytes);
    g_tags.cover = std::move(bytes);
    g.redraw = true;
}

// The text box: the field's surface, its text scrolled to keep the caret in view, and the caret.
void DrawField(int f, const Palette& p) {
    TextField& t = g_tags.fields[f];
    const Rect& r = t.rect;
    bool focused = g_tags.focus == f;
    Fill(r, t.enabled ? p.surface2 : p.surface);
    Frame(r, focused ? p.accent : p.border);
    if (focused) Fill({r.x + S(1), r.B() - S(1), r.w - S(2), S(1)}, p.accent);
    float pad = S(5), lineH = LineHeight(Font::Normal);
    SDL_Rect clip{(int)(r.x + S(2)), (int)(r.y + S(1)), (int)(r.w - S(4)), (int)(r.h - S(2))};
    SDL_SetRenderClipRect(g.renderer, &clip);
    if (t.text.empty() && !focused) {
        const char* cue = !t.enabled ? "(select one track to edit lyrics)" : !t.mixed ? "" : Numeric(f) ? "(mixed)" : "(multiple values)";
        DrawText(Font::Normal, cue, r.x + pad, Multiline(f) ? r.y + S(4) : TextY(r), p.faint);
    }
    if (Multiline(f)) {
        // Lines from the text's line breaks; the caret's line stays visible.
        size_t lineStart = 0, caretLine = 0, line = 0;
        float caretX = 0;
        for (size_t i = 0; i <= t.text.size(); ++i)
            if (i == t.text.size() || t.text[i] == L'\n') {
                if (t.caret >= lineStart && t.caret <= i) {
                    caretLine = line;
                    caretX = TextWidth(Font::Normal, Utf8(t.text.substr(lineStart, t.caret - lineStart)));
                }
                lineStart = i + 1;
                ++line;
            }
        float caretY = caretLine * lineH;
        if (caretY - t.scrollY > r.h - S(8) - lineH) t.scrollY = caretY - (r.h - S(8) - lineH);
        if (caretY < t.scrollY) t.scrollY = caretY;
        lineStart = 0;
        line = 0;
        for (size_t i = 0; i <= t.text.size(); ++i)
            if (i == t.text.size() || t.text[i] == L'\n') {
                float y = r.y + S(4) + line * lineH - t.scrollY;
                if (y > r.y - lineH && y < r.B())
                    DrawText(Font::Normal, Utf8(t.text.substr(lineStart, i - lineStart)), r.x + pad, y, t.all && focused ? p.selectText : p.text);
                lineStart = i + 1;
                ++line;
            }
        if (focused && t.all) Fill({r.x + S(2), r.y + S(2), r.w - S(4), r.h - S(4)}, p.select, 90);
        if (focused && SDL_GetTicks() % 1060 < 530)
            Fill({std::round(r.x + pad + caretX), r.y + S(4) + caretY - t.scrollY, S(1), lineH}, p.text);
    } else {
        std::string all = Utf8(t.text);
        float caretX = TextWidth(Font::Normal, Utf8(t.text.substr(0, t.caret))), room = r.w - 2 * pad;
        if (caretX - t.scrollX > room) t.scrollX = caretX - room;
        if (caretX < t.scrollX) t.scrollX = caretX;
        float tx = r.x + pad - t.scrollX, ty = TextY(r);
        if (focused && t.all) Fill({tx, r.y + S(3), TextWidth(Font::Normal, all), r.h - S(6)}, p.select);
        DrawText(Font::Normal, all, tx, ty, focused && t.all ? p.selectText : p.text);
        if (focused && SDL_GetTicks() % 1060 < 530) Fill({std::round(tx + caretX), ty, S(1), lineH}, p.text);
    }
    SDL_SetRenderClipRect(g.renderer, nullptr);
}

void DrawTags(const Palette& p) {
    std::string title = g_tags.ids.size() == 1 ? "Edit tags" : "Edit tags (" + std::to_string(g_tags.ids.size()) + " tracks)";
    Rect panel = g_tags.panel = DrawFrame(p, S(760), S(640), title.c_str());
    float pad = S(16), labelW = S(92), rowH = S(26), gap = S(6), artSize = S(160);
    float x0 = panel.x + pad, right = panel.R() - pad - artSize - S(18), y = panel.y + S(48);
    auto row = [&](int f, float x, float w, float lw = -1) {
        float labelWidth = lw < 0 ? labelW : lw;
        DrawText(Font::Normal, kLabels[f], x, y + (rowH - LineHeight(Font::Normal)) / 2, p.sub, labelWidth - S(8), 2);
        g_tags.fields[f].rect = {x + labelWidth, y, w - labelWidth, rowH};
        DrawField(f, p);
    };
    for (int f : {Title, Artist, AlbumArtist, Album}) {
        row(f, x0, right - x0);
        y += rowH + gap;
    }
    float half = std::round((right - x0) / 2);
    row(Date, x0, half - S(8));
    row(Genre, x0 + half, half, S(56));
    y += rowH + gap;
    float q = std::round((half - S(8) - labelW) / 2);
    row(TrackNum, x0, labelW + q);
    row(TrackTotal, x0 + labelW + q, q + S(10), S(26));
    row(Disc, x0 + half, S(56) + q);
    row(DiscTotal, x0 + half + S(56) + q, half - S(56) - q, S(26));
    y += rowH + gap;
    for (int f : {Composer, Grouping}) {
        row(f, x0, right - x0);
        y += rowH + gap;
    }
    row(Label, x0, half - S(8));
    row(Bpm, x0 + half, half, S(56));
    y += rowH + gap;
    g_tags.compilationRect = {x0 + labelW, y, S(320), S(24)};
    DrawCheck(g_tags.compilationRect, "Part of a compilation (various artists)", g_tags.compilation, p);
    y += S(30);
    float full = panel.R() - pad - x0;
    DrawText(Font::Normal, kLabels[Comment], x0, y + S(4), p.sub, labelW - S(8), 2);
    g_tags.fields[Comment].rect = {x0 + labelW, y, full - labelW, S(54)};
    DrawField(Comment, p);
    y += S(54) + gap;
    float bottom = panel.B() - pad - S(30) - S(28);
    DrawText(Font::Normal, kLabels[Lyrics], x0, y + S(4), p.sub, labelW - S(8), 2);
    g_tags.fields[Lyrics].rect = {x0 + labelW, y, full - labelW, std::max(S(40), bottom - y)};
    DrawField(Lyrics, p);
    // The cover column, top right.
    float ax = panel.R() - pad - artSize, ay = panel.y + S(48);
    g_tags.art = {ax, ay, artSize, artSize};
    Fill(g_tags.art, p.surface2);
    Frame(g_tags.art, p.border);
    bool removed = g_tags.cover && g_tags.cover->empty();
    if (g_tags.preview && !removed) {
        float s = (artSize - S(2)) / std::max(g_tags.previewW, g_tags.previewH);
        float w = g_tags.previewW * s, h = g_tags.previewH * s;
        SDL_FRect dst{ax + (artSize - w) / 2, ay + (artSize - h) / 2, w, h};
        SDL_RenderTexture(g.renderer, g_tags.preview, nullptr, &dst);
    } else {
        DrawText(Font::Normal, removed ? "(cover removed)" : "No cover", ax, TextY(g_tags.art), p.faint, artSize, 1);
    }
    g_tags.choose = {ax, ay + artSize + S(6), artSize, S(26)};
    g_tags.remove = {ax, ay + artSize + S(36), artSize, S(26)};
    DrawButton(g_tags.choose, "Choose cover...", p);
    DrawButton(g_tags.remove, "Remove cover", p, false, g_tags.cover ? !g_tags.cover->empty() : g_tags.preview != nullptr);
    float by = panel.B() - pad - S(28);
    DrawText(Font::Small, g_tags.warning.empty() ? g_tags.info : g_tags.warning, x0, by - S(24),
             g_tags.warning.empty() ? p.sub : p.error, panel.R() - pad - x0);
    g_tags.save = {panel.R() - pad - S(208), by, S(100), S(28)};
    g_tags.cancel = {panel.R() - pad - S(100), by, S(100), S(28)};
    DrawButton(g_tags.save, "Save", p, true);
    DrawButton(g_tags.cancel, "Cancel", p);
}

void Edited(TextField& t) {
    t.changed = true;
    t.all = false;
    g.redraw = true;
}

void InsertText(const std::wstring& s) {
    TextField& t = g_tags.fields[g_tags.focus];
    if (!t.enabled) return;
    std::wstring in;
    for (wchar_t c : s) {
        if (Numeric(g_tags.focus) ? (c >= L'0' && c <= L'9') : (c >= 32 || (c == L'\n' && Multiline(g_tags.focus)))) in += c;
        else if (c == L'\t' && !Numeric(g_tags.focus)) in += L' ';
    }
    if (in.empty()) return;
    if (t.all) {
        t.text.clear();
        t.caret = 0;
    }
    t.text.insert(t.caret, in);
    t.caret += in.size();
    Edited(t);
}

// The caret position for a click at (x, y) inside the field.
size_t CaretAt(const TextField& t, int f, float x, float y) {
    float pad = S(5);
    size_t start = 0, end = t.text.size();
    if (Multiline(f)) {
        int want = (int)std::floor((y - t.rect.y - S(4) + t.scrollY) / LineHeight(Font::Normal));
        int line = 0;
        for (size_t i = 0; i <= t.text.size(); ++i)
            if (i == t.text.size() || t.text[i] == L'\n') {
                if (line == want || i == t.text.size()) {
                    end = i;
                    break;
                }
                start = i + 1;
                ++line;
            }
    }
    float offset = x - (t.rect.x + pad) + (Multiline(f) ? 0 : t.scrollX);
    for (size_t i = start; i < end; ++i) {
        float a = TextWidth(Font::Normal, Utf8(t.text.substr(start, i - start)));
        float b = TextWidth(Font::Normal, Utf8(t.text.substr(start, i + 1 - start)));
        if (offset < (a + b) / 2) return i;
    }
    return end;
}

void TagsMouseDown(float x, float y) {
    if (!g_tags.panel.In(x, y) || g_tags.cancel.In(x, y)) {
        ClosePanel();
        return;
    }
    if (g_tags.save.In(x, y)) {
        SaveTags();
        return;
    }
    if (g_tags.choose.In(x, y)) {
        static const SDL_DialogFileFilter filter{"Images", "jpg;jpeg;png"};
        ChooseFile(kFileCover, filter);
        return;
    }
    if (g_tags.remove.In(x, y) && (g_tags.cover ? !g_tags.cover->empty() : g_tags.preview != nullptr)) {
        g_tags.cover = std::string();
        return;
    }
    if (g_tags.compilationRect.In(x, y)) {
        g_tags.compilation = g_tags.compilation == 1 ? 0 : 1;
        return;
    }
    for (int f = 0; f < FieldCount; ++f) {
        TextField& t = g_tags.fields[f];
        if (t.rect.In(x, y) && t.enabled) {
            Focus(f, false);
            t.caret = CaretAt(t, f, x, y);
            return;
        }
    }
}

// The previous or next line break around `pos` in a multi-line field, for Up and Down.
size_t LineStart(const std::wstring& s, size_t pos) {
    size_t b = pos ? s.rfind(L'\n', pos - 1) : std::wstring::npos;
    return b == std::wstring::npos ? 0 : b + 1;
}

void TagsKey(const SDL_KeyboardEvent& e) {
    bool ctrl = e.mod & SDL_KMOD_CTRL, shift = e.mod & SDL_KMOD_SHIFT;
    TextField& t = g_tags.fields[g_tags.focus];
    auto step = [&](int dir) {  // Tab order: the fields as laid out, skipping the disabled one
        int f = g_tags.focus;
        for (int i = 0; i < FieldCount; ++i) {
            f = (f + dir + FieldCount) % FieldCount;
            if (g_tags.fields[f].enabled) break;
        }
        Focus(f, true);
    };
    switch (e.key) {
        case SDLK_ESCAPE: ClosePanel(); return;
        case SDLK_TAB: step(shift ? -1 : 1); break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            if (ctrl || !Multiline(g_tags.focus)) SaveTags();
            else InsertText(L"\n");
            return;
        case SDLK_BACKSPACE:
        case SDLK_DELETE:
            if (t.all) {
                t.text.clear();
                t.caret = 0;
                Edited(t);
            } else if (e.key == SDLK_BACKSPACE && t.caret > 0) {
                size_t from = t.caret - 1;
                if (ctrl) {  // the word before the caret
                    while (from > 0 && t.text[from - 1] == L' ') --from;
                    while (from > 0 && t.text[from - 1] != L' ' && t.text[from - 1] != L'\n') --from;
                }
                t.text.erase(from, t.caret - from);
                t.caret = from;
                Edited(t);
            } else if (e.key == SDLK_DELETE && t.caret < t.text.size()) {
                t.text.erase(t.caret, 1);
                Edited(t);
            }
            break;
        case SDLK_LEFT:
            if (t.all) t.caret = 0;
            else if (t.caret > 0) --t.caret;
            t.all = false;
            break;
        case SDLK_RIGHT:
            if (t.all) t.caret = t.text.size();
            else if (t.caret < t.text.size()) ++t.caret;
            t.all = false;
            break;
        case SDLK_HOME:
            t.caret = Multiline(g_tags.focus) && !ctrl ? LineStart(t.text, t.caret) : 0;
            t.all = false;
            break;
        case SDLK_END: {
            size_t end = Multiline(g_tags.focus) && !ctrl ? t.text.find(L'\n', t.caret) : std::wstring::npos;
            t.caret = end == std::wstring::npos ? t.text.size() : end;
            t.all = false;
            break;
        }
        case SDLK_UP:
        case SDLK_DOWN:
            if (Multiline(g_tags.focus)) {
                size_t start = LineStart(t.text, t.caret), column = t.caret - start;
                if (e.key == SDLK_UP && start > 0) {
                    size_t prev = LineStart(t.text, start - 1);
                    t.caret = std::min(prev + column, start - 1);
                } else if (e.key == SDLK_DOWN) {
                    size_t next = t.text.find(L'\n', t.caret);
                    if (next != std::wstring::npos) {
                        size_t after = t.text.find(L'\n', next + 1);
                        t.caret = std::min(next + 1 + column, after == std::wstring::npos ? t.text.size() : after);
                    }
                }
                t.all = false;
            } else step(e.key == SDLK_UP ? -1 : 1);
            break;
        case SDLK_A:
            if (ctrl) t.all = !t.text.empty();
            break;
        case SDLK_C:
            if (ctrl) SDL_SetClipboardText(Utf8(t.text).c_str());
            break;
        case SDLK_X:
            if (ctrl && t.enabled) {
                SDL_SetClipboardText(Utf8(t.text).c_str());
                t.text.clear();
                t.caret = 0;
                Edited(t);
            }
            break;
        case SDLK_V:
            if (ctrl) {
                if (char* clip = SDL_GetClipboardText()) {
                    InsertText(compat::Widen(clip));
                    SDL_free(clip);
                }
            }
            break;
        case SDLK_S:
            if (ctrl) SaveTags();
            return;
        default: break;
    }
    g.redraw = true;
}

void OpenTags(const std::vector<TrackId>& ids) {
    g_tags.ids = ids;
    bool multi = ids.size() > 1;
    for (int f = 0; f < FieldCount; ++f) {
        TextField& t = g_tags.fields[f];
        t = TextField{};
        std::wstring first = f == Lyrics ? std::wstring() : TrackValue(ids[0], f);
        bool mixed = false;
        for (size_t i = 1; i < ids.size() && !mixed && f != Lyrics; ++i) mixed = TrackValue(ids[i], f) != first;
        if (f == Lyrics && !multi) {
            std::string lyrics;
            ReadLyrics(g_lib.Path(ids[0]), lyrics);
            first = compat::Widen(lyrics.c_str());
            std::erase(first, L'\r');
        }
        t.text = mixed ? std::wstring() : first;
        t.mixed = mixed;
        t.enabled = f != Lyrics || !multi;
    }
    int on = 0;
    for (TrackId id : ids) on += (g_lib[id].flags & Track::kCompilation) ? 1 : 0;
    g_tags.compilation = g_tags.initialCompilation = on == 0 ? 0 : on == (int)ids.size() ? 1 : 2;
    g_tags.cover.reset();
    SetPreview(TrackCoverBytes(g_lib.Path(ids[0])));
    std::wstring info = FormatTitle(L"[%codec%][  \x2022  %bitrate% kbps][  \x2022  %samplerate% Hz][  \x2022  %length%]  \x2022  %path%",
                                    [id = ids[0]](const std::wstring& name, std::wstring& out) { return LookupField(id, name, out); });
    if (multi) info = std::to_wstring(ids.size()) + L" tracks  \x2022  first: " + info;
    g_tags.info = Utf8(info);
    int unsupported = 0, cueTracks = 0;
    for (TrackId id : ids) {
        std::wstring path = g_lib.Path(id);
        if (IsSubtrack(path)) ++cueTracks;
        else unsupported += !CanWriteTags(path);
    }
    g_tags.warning.clear();
    if (cueTracks) g_tags.warning = "Tags of CUE tracks come from the cue sheet; editing them isn't supported yet.";
    else if (unsupported)
        g_tags.warning = std::to_string(unsupported) + " of these files can't be written (only MP3, FLAC, Ogg, Opus, M4A and cord can).";
    g_kind = Kind::Tags;
    Focus(Title, false);
}

// --- preferences --------------------------------------------------------------------------------------
// The Windows build's Preferences (app_tweaks.cpp) for the settings this build has: a page per area
// plus All settings, one search across them, and each setting with its help, its default and a reset.
// A change applies at once through the same function its menu item uses, and is saved.
enum class PrefType { Bool, Float, Choice };
struct Pref {
    const char* key;  // in settings.ini; permanent, as renaming one would orphan saved values
    const char* page;
    const char* label;
    const char* help;
    PrefType type;
    float min, max, def, step;
    double (*get)();
    void (*set)(double);
    const char* const* choices = nullptr;
    const char* unit = nullptr;
};

const char* const kRgModes[] = {"Album", "Track", "By playback order"};
const char* const kOffOn[] = {"Off", "On"};
const char* const kLoopNames[] = {"Ignore loop points", "Play the loop once, then fade", "Play the loop twice, then fade",
                                  "Play the loop three times, then fade"};
const char* const kReverbNames[] = {"Off", "Room", "Hall", "Plate"};
const char* const kSavingNames[] = {"Ask before saving", "Save automatically", "Manual only (don't ask)"};
const char* const kNewMusicNames[] = {"Add automatically", "Review in Inbox"};
const char* const kBandWaveSizes[] = {"Use layout size", "Slim (26 px)", "Medium (44 px)", "Tall (72 px)",
                                    "Extra tall (108 px)", "Largest (144 px)"};
static_assert(std::size(kBandWaveSizes) == std::size(ui::kBandWaveHeights));
const char* const kUndoNames[] = {"1 day", "3 days", "1 week", "7 weeks"};
static_assert(std::size(kReverbNames) == (size_t)ReverbPreset::Count);

Settings& Cfg() { return g.settings; }
void Saved() {
    SaveSettings();
    g.redraw = true;
}

const Pref kPrefs[] = {
    {"resume_on", "Playback", "Resume where you left off",
     "When cordinal starts, offer to continue the track that was playing when it closed.", PrefType::Bool,
     0, 1, 1, 1, [] { return (double)Cfg().resumeOn; }, [](double v) { SetResumeOn(v != 0); }},
    {"skip_aware_shuffle", "Playback", "Skip-aware shuffle",
     "Shuffle favors tracks you haven't repeatedly skipped in their first 30 seconds.", PrefType::Bool,
     0, 1, 0, 1, [] { return (double)Cfg().skipAwareShuffle; }, [](double v) { SetSkipAwareShuffle(v != 0); }},
    {"radio", "Playback", "Keep playing like this",
     "Keeps choosing from the playing playlist, favoring what you tend to play after the current track.",
     PrefType::Bool, 0, 1, 0, 1, [] { return (double)Cfg().radio; }, [](double v) { SetRadio(v != 0); }},
    {"loops", "Playback", "Game music loops", "How often a game soundtrack rip with loop points plays its loop before fading out.",
     PrefType::Choice, 0, 3, 2, 1, [] { return (double)Cfg().loops; },
     [](double v) { Cfg().loops = (int)v; ApplyEffects(); Saved(); }, kLoopNames},
    {"exclusive", "Output", "Exclusive output",
     "Each track plays at its own rate, bit-perfect at 100% volume with the EQ and ReplayGain off.", PrefType::Bool, 0, 1, 0,
     1, [] { return (double)Cfg().exclusive; }, [](double v) { SetExclusive(v != 0); }},
    {"rg_enabled", "ReplayGain", "ReplayGain", "Plays every track at a similar loudness, using measured or tagged ReplayGain values.",
     PrefType::Bool, 0, 1, 1, 1, [] { return (double)Cfg().rg.on; }, [](double v) { SetReplayGain(v != 0); }},
    {"rg_mode", "ReplayGain", "Level by",
     "Album preserves album relationships; track levels songs individually. Playback order uses track for track shuffle.",
     PrefType::Choice, 0, 2, 0, 1, [] { return (double)Cfg().rg.mode; },
     [](double v) { Cfg().rg.mode = (ReplayGainMode)(int)v; RefreshGains(); Saved(); }, kRgModes},
    {"rg_keep", "ReplayGain", "Loud songs keep their loudness",
     "0 evens every track out; higher values turn loud songs down only part of the way.", PrefType::Float, 0, 0.9f, 0.4f,
     0.05f, [] { return (double)Cfg().rg.keep; }, [](double v) { Cfg().rg.keep = (float)v; RefreshGains(); Saved(); }},
    {"rg_preamp", "ReplayGain", "Preamp", "Added to every track's ReplayGain.", PrefType::Float, -24, 24, 0, 0.5f,
     [] { return (double)Cfg().rg.preamp; }, [](double v) { Cfg().rg.preamp = (float)v; RefreshGains(); Saved(); }, nullptr, "dB"},
    {"rg_preamp_noinfo", "ReplayGain", "Gain for unmeasured tracks",
     "Used for tracks with no ReplayGain yet, limited so their peaks don't clip.", PrefType::Float, -24, 24, 0, 0.5f,
     [] { return (double)Cfg().rg.preampNoInfo; }, [](double v) { Cfg().rg.preampNoInfo = (float)v; RefreshGains(); Saved(); },
     nullptr, "dB"},
    {"rg_autoscan", "ReplayGain", "Measure new tracks automatically",
     "Measures the loudness of new tracks in the background, giving way to playback and to your own scans.", PrefType::Bool,
     0, 1, 1, 1, [] { return (double)Cfg().rgAutoScan; }, [](double v) { Cfg().rgAutoScan = v != 0; Saved(); }},
    {"rg_estimate", "ReplayGain", "Estimate unmeasured tracks as they start",
     "Guesses the loudness of a track with no ReplayGain from its first moments while it plays.", PrefType::Bool, 0, 1, 1, 1,
     [] { return (double)Cfg().rgEstimate; }, [](double v) { Cfg().rgEstimate = v != 0; Saved(); }},
    {"reverb", "Effects", "Reverb", "A room around the music.", PrefType::Choice, 0, 3, 0, 1,
     [] { return (double)Cfg().reverb.preset; },
     [](double v) { Cfg().reverb.preset = (ReverbPreset)(int)v; ApplyEffects(); Saved(); }, kReverbNames},
    {"reverb_amount", "Effects", "Reverb amount", "How much of the room is mixed in, from 0 (none) to 1 (all of it).",
     PrefType::Float, 0, 1, 0.4f, 0.05f, [] { return (double)Cfg().reverb.mix; },
     [](double v) { Cfg().reverb.mix = (float)v; ApplyEffects(); Saved(); }},
    {"loudness_compensation", "Effects", "Loudness compensation at low volume",
     "Adds bass and treble as the volume goes down, so quiet listening doesn't sound thin.", PrefType::Bool, 0, 1, 0, 1,
     [] { return (double)Cfg().loudnessComp; }, [](double v) { Cfg().loudnessComp = v != 0; ApplyEffects(); Saved(); }},
    {"night_mode", "Effects", "Night mode", "Slowly lifts quiet passages and softens loud ones, for listening at night.",
     PrefType::Bool, 0, 1, 0, 1, [] { return (double)Cfg().nightMode; },
     [](double v) { Cfg().nightMode = v != 0; ApplyEffects(); Saved(); }},
    {"live_waveform", "View", "Live waveform for songs",
     "Show a moving waveform of the audio being heard instead of the whole-song overview. Seeking still works. "
     "Radio always uses the live waveform.", PrefType::Bool, 0, 1, 0, 1,
     [] { return (double)Cfg().liveWaveform; }, [](double v) { Cfg().liveWaveform = v != 0; Saved(); }},
    {"live_fountain", "View", "Live audio for Dark Fountain",
     "In the DELTARUNE theme, Dark Fountain follows the audio being heard, including Radio. "
     "Song seeking still works.", PrefType::Bool, 0, 1, 0, 1,
     [] { return (double)Cfg().liveFountain; }, [](double v) { Cfg().liveFountain = v != 0; Saved(); }},
    {"full_live_waveform", "View", "Full color for live waveforms",
     "Light the whole live waveform. Turn off to show song progress with played and unplayed colors.",
     PrefType::Bool, 0, 1, 1, 1, [] { return (double)Cfg().fullLiveWaveform; },
     [](double v) { Cfg().fullLiveWaveform = v != 0; Saved(); }},
    {"band_wave_size", "View", "Top/bottom waveform size",
     "Sets the waveform's height in the normal player bar. Other player modes keep their own sizes.",
     PrefType::Choice, 0, 5, 0, 1, [] { return (double)Cfg().bandWaveSize; },
     [](double v) { Cfg().bandWaveSize = (int)v; Saved(); }, kBandWaveSizes},
    {"group_albums", "View", "Group tracks by album", "The track list shows a header with the cover above each album's tracks.",
     PrefType::Bool, 0, 1, 1, 1, [] { return (double)Cfg().groupAlbums; },
     [](double v) { Cfg().groupAlbums = v != 0; RebuildRows(); Saved(); }},
    {"points_chip", "View", "Show level and points",
     "Shows your rank, level and points beside the now-playing lines, and announces a new level.", PrefType::Bool, 0, 1, 0, 1,
     [] { return (double)Cfg().pointsChip; }, [](double v) { Cfg().pointsChip = v != 0; Saved(); }},
    {"preference_saving", "Startup", "Preference saving",
     "Whether a new theme, library view or browser becomes how cordinal opens: asked about once it settles, saved as you go, or only "
     "through Startup defaults.", PrefType::Choice, 0, 2, 0, 1, [] { return (double)(int)Cfg().preferenceSaving; },
     [](double v) { SetPreferenceSaving((PreferenceSaving)(int)v); }, kSavingNames},
    {"time_remaining", "Playback", "Show the time remaining",
     "The time beside the seek bar counts down to the end of the track instead of showing its length.", PrefType::Bool, 0, 1, 1, 1,
     [] { return (double)Cfg().timeRemaining; }, [](double v) { Cfg().timeRemaining = v != 0; Saved(); }},
    {"always_on_top", "View", "Always on top", "Keeps cordinal's window above other windows.", PrefType::Bool, 0, 1, 0, 1,
     [] { return (double)Cfg().alwaysOnTop; }, [](double v) { Cfg().alwaysOnTop = v != 0; SDL_SetWindowAlwaysOnTop(g.window, v != 0); Saved(); }},
    {"sound_effects", "View", "Sound effects",
     "Small, quiet sounds for things that otherwise happen silently, such as saving tags or dropping files.", PrefType::Bool, 0, 1,
     0, 1, [] { return (double)Cfg().soundEffects; }, [](double v) { Cfg().soundEffects = v != 0; Saved(); }},
    {"robotic_sounds", "View", "Robotic sounds in the DELTARUNE theme",
     "The theme's sounds come bitcrushed, in Chapter 2's Cyber World voice, and messages are spoken in Queen's blip.",
     PrefType::Bool, 0, 1, 1, 1, [] { return (double)Cfg().roboticSounds; },
     [](double v) { Cfg().roboticSounds = v != 0; Saved(); if (v != 0) MessageBlip(); }},
    {nullptr, "Startup", "Start on login", "Opens cordinal when you log in (an entry in ~/.config/autostart).", PrefType::Bool,
     0, 1, 0, 1, [] { return (double)StartsOnLogin(); }, [](double v) { if ((v != 0) != StartsOnLogin()) ToggleStartOnLogin(); }},
    {"inbox_enabled", "Library", "New music from library folders",
     "Add new music automatically, or review it in the Inbox first. Existing Inbox tracks stay there.",
     PrefType::Choice, 0, 1, 0, 1, [] { return (double)Cfg().inboxEnabled; },
     [](double v) {
         Cfg().inboxEnabled = v != 0;
         Saved();
         ShowMessage(Cfg().inboxEnabled ? L"New music will wait in the Inbox." : L"New music will be added to the library.");
     }, kNewMusicNames},
    {"web_start", "Library", "Start the web server with cordinal",
     "Turns on the web server whenever cordinal starts, so a paired phone or browser can play your library.", PrefType::Bool,
     0, 1, 0, 1, [] { return (double)Cfg().webStart; }, [](double v) { Cfg().webStart = v != 0; Saved(); }},
    {"convert_recycle", "Library", "Send converted originals to the Recycle Bin",
     "After converting to .cord, the original files go to the Recycle Bin instead of staying where they are.", PrefType::Bool,
     0, 1, 1, 1, [] { return (double)Cfg().convertRecycle; }, [](double v) { Cfg().convertRecycle = v != 0; Saved(); }},
    {"convert_show_originals", "Library", "Show kept originals in the library",
     "Originals kept after a conversion appear in the library beside their .cord copies.", PrefType::Bool,
     0, 1, 0, 1, [] { return (double)Cfg().convertShowOriginals; }, [](double v) { SetConvertShowOriginals(v != 0); Saved(); }},
    {"convert_undo_days", "Library", "Conversions can be undone for",
     "How long cordinal remembers a conversion so it can put the originals back.", PrefType::Choice,
     0, 3, 3, 1, [] { return (double)Cfg().convertMemory; }, [](double v) { SetConvertMemory((unsigned)v); Saved(); }, kUndoNames},
};

struct PrefLink {
    const char* label;
    void (*open)();
};
struct PrefPage {
    const char* name;  // null for All settings
    PrefLink links[3];
};
const PrefPage kPrefPages[] = {
    {nullptr, {}},
    {"Playback", {}},
    {"Output", {{"Output device...", OpenOutputDevice}}},
    {"ReplayGain", {}},
    {"Effects", {{"Equalizer...", OpenEqualizer}}},
    {"View", {{"Title formatting...", OpenFormat}, {"Customize layout...", OpenLayout}}},
    {"Startup", {{"Startup defaults...", OpenStartupDefaults}}},
    {"Library", {{"Library folders...", OpenLibraryFolders}, {"Web server...", OpenWebServer},
                {"ListenBrainz scrobbling...", OpenListenBrainz}}},
};

struct PrefRow {
    int pref;
    Rect less, more, toggle, reset;
};
struct PrefsPanel {
    int page = 0;
    int focus = -1;       // the shown setting the keys change (an index into ShownPrefs), or -1
    bool reveal = false;  // scroll the focused setting into view on the next draw
    std::wstring search;
    float scroll = 0, contentH = 0;
    Rect panel, searchBox, list, resetAll, close;
    std::vector<Rect> pages, links;
    std::vector<PrefRow> rows;
} g_prefs;

bool Changed(const Pref& t) { return std::fabs(t.get() - t.def) > 1e-4; }

std::string PrefText(const Pref& t, double v) {
    char buf[48];
    switch (t.type) {
        case PrefType::Bool: return kOffOn[v != 0];
        case PrefType::Choice: return t.choices[std::clamp((int)std::lround(v), 0, (int)t.max)];
        case PrefType::Float:
            if (t.unit) snprintf(buf, sizeof(buf), "%+.1f %s", v, t.unit);
            else snprintf(buf, sizeof(buf), "%.2f", v);
            return buf;
    }
    return {};
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)std::towlower(c);
    return s;
}

// The settings of themes this build leaves out (theme_palette.h) are never shown.
bool PrefShown(const Pref& t) {
    return kExtraThemes || (strcmp(t.key, "live_fountain") && strcmp(t.key, "robotic_sounds"));
}

// The settings shown: the open page's, or with a search, every one whose words match.
std::vector<int> ShownPrefs() {
    std::vector<int> shown;
    std::wstring query = Lower(g_prefs.search);
    for (int i = 0; i < (int)std::size(kPrefs); ++i) {
        const Pref& t = kPrefs[i];
        if (!PrefShown(t)) continue;
        if (!query.empty()) {
            std::string text = std::string(t.label) + " " + t.page + " " + (t.key ? t.key : "") + " " + t.help;
            if (Lower(compat::Widen(text.c_str())).find(query) == std::wstring::npos) continue;
        } else if (g_prefs.page && strcmp(kPrefPages[g_prefs.page].name, t.page)) continue;
        shown.push_back(i);
    }
    return shown;
}

}  // namespace

std::vector<std::string> Wrap(Font font, const std::string& text, float width) {
    std::vector<std::string> lines;
    std::string line;
    size_t i = 0;
    while (i < text.size()) {
        size_t end = text.find(' ', i);
        std::string word = text.substr(i, end == std::string::npos ? std::string::npos : end - i);
        std::string next = line.empty() ? word : line + " " + word;
        if (!line.empty() && TextWidth(font, next) > width) {
            lines.push_back(line);
            line = word;
        } else line = next;
        i = end == std::string::npos ? text.size() : end + 1;
    }
    if (!line.empty()) lines.push_back(line);
    return lines;
}

namespace {

void DrawPrefs(const Palette& p) {
    Rect panel = g_prefs.panel = DrawFrame(p, S(840), S(600), "Preferences");
    float pad = S(16), x0 = panel.x + pad, right = panel.R() - pad, top = panel.y + S(52);
    // The search, top right, as on Windows.
    Rect& box = g_prefs.searchBox = {right - S(260), panel.y + S(10), S(260), S(28)};
    Fill(box, p.surface2);
    Frame(box, p.accent);
    Fill({box.x + S(1), box.B() - S(1), box.w - S(2), S(1)}, p.accent);
    if (g_prefs.search.empty()) DrawText(Font::Normal, "Search settings", box.x + S(6), TextY(box), p.sub, box.w - S(12));
    float w = DrawText(Font::Normal, Utf8(g_prefs.search), box.x + S(6), TextY(box), p.text, box.w - S(12));
    if (SDL_GetTicks() % 1060 < 530) Fill({box.x + S(6) + w, TextY(box), S(1), LineHeight(Font::Normal)}, p.text);
    // The pages, each with how many of its settings differ from their defaults.
    float bottom = panel.B() - pad - S(28) - S(12);
    g_prefs.pages.clear();
    float py = top;
    for (int i = 0; i < (int)std::size(kPrefPages); ++i) {
        Rect r{x0, py, S(160), S(30)};
        bool on = g_prefs.search.empty() && i == g_prefs.page;
        if (on) Fill(r, p.select);
        else if (r.In(g_mx, g_my)) Fill(r, p.hover);
        if (on) Fill({r.x, r.y + S(6), S(3), r.h - S(12)}, p.accent);
        int changed = 0;
        for (const Pref& t : kPrefs)
            if (PrefShown(t) && (!kPrefPages[i].name || !strcmp(kPrefPages[i].name, t.page)) && Changed(t)) ++changed;
        DrawText(Font::Normal, kPrefPages[i].name ? kPrefPages[i].name : "All settings", r.x + S(12), TextY(r),
                 on ? p.selectText : p.text, r.w - S(40));
        if (changed) DrawText(Font::Small, std::to_string(changed), r.x, TextY(r, Font::Small), on ? p.selectText : p.sub, r.w - S(10), 2);
        g_prefs.pages.push_back(r);
        py += S(32);
    }
    Fill({x0 + S(172), top, S(1), bottom - top}, p.border);
    // The settings, scrolled; each is its label, its help, its default and key, and its control.
    Rect list = g_prefs.list = {x0 + S(188), top, right - x0 - S(188), bottom - top};
    auto shown = ShownPrefs();
    float controlW = S(250), textW = list.w - controlW - S(20);
    SDL_Rect clip{(int)list.x, (int)list.y, (int)list.w, (int)list.h};
    SDL_SetRenderClipRect(g.renderer, &clip);
    float y = list.y - g_prefs.scroll;
    g_prefs.rows.clear();
    if (shown.empty())
        DrawText(Font::Normal, "No setting matches the search.", list.x, list.y + S(8), p.sub);
    for (size_t n = 0; n < shown.size(); ++n) {
        int i = shown[n];
        const Pref& t = kPrefs[i];
        auto help = Wrap(Font::Small, t.help, textW);
        std::string meta = "Default: " + PrefText(t, t.def) + (t.key ? std::string("   Key: ") + t.key : std::string());
        float h = S(12) + LineHeight(Font::Normal) + S(4) + (help.size() + 1) * LineHeight(Font::Small) + S(12);
        PrefRow row{i};
        if ((int)n == g_prefs.focus) {
            Fill({list.x - S(10), y + S(8), S(3), h - S(16)}, p.accent);
            if (g_prefs.reveal) {  // corrected on the next frame
                if (y < list.y) g_prefs.scroll -= list.y - y;
                else if (y + h > list.B()) g_prefs.scroll += y + h - list.B();
                g_prefs.reveal = false;
                g.redraw = true;
            }
        }
        float ty = y + S(12);
        DrawText(Font::Normal, t.label, list.x, ty, p.text, textW);
        ty += LineHeight(Font::Normal) + S(4);
        for (auto& line : help) {
            DrawText(Font::Small, line, list.x, ty, p.sub, textW);
            ty += LineHeight(Font::Small);
        }
        DrawText(Font::Small, meta, list.x, ty, p.faint, textW);
        float cx = list.R() - controlW, cy = y + S(10);
        double v = t.get();
        if (t.type == PrefType::Bool) {
            row.toggle = {cx, cy, controlW, S(26)};
            DrawCheck(row.toggle, v != 0 ? "On" : "Off", v != 0, p);
        } else {
            // A stepper: the value between buttons that step it down and up (or through the choices).
            row.less = {cx, cy, S(28), S(26)};
            row.more = {list.R() - S(28), cy, S(28), S(26)};
            DrawButton(row.less, t.type == PrefType::Choice ? "<" : "-", p, false, v > t.min + 1e-4);
            DrawButton(row.more, t.type == PrefType::Choice ? ">" : "+", p, false, v < t.max - 1e-4);
            Rect value{row.less.R(), cy, row.more.x - row.less.R(), S(26)};
            DrawText(Font::Normal, PrefText(t, v), value.x + S(4), TextY(value), p.text, value.w - S(8), 1);
        }
        if (Changed(t)) {
            row.reset = {list.R() - S(60), cy + S(32), S(60), S(20)};
            bool hot = row.reset.In(g_mx, g_my);
            DrawText(Font::Small, "Reset", row.reset.x, TextY(row.reset, Font::Small), hot ? p.accent2 : p.accent, row.reset.w, 2);
        }
        g_prefs.rows.push_back(row);
        y += h;
        Fill({list.x, y - S(1), list.w, S(1)}, p.border);
    }
    g_prefs.contentH = y + g_prefs.scroll - list.y;
    SDL_SetRenderClipRect(g.renderer, nullptr);
    // The page's links to the windows that hold the rest of its settings, and the resets.
    float by = panel.B() - pad - S(28);
    g_prefs.links.clear();
    float lx = x0;
    if (g_prefs.search.empty())
        for (const PrefLink& link : kPrefPages[g_prefs.page].links) {
            if (!link.label) continue;
            Rect r{lx, by, TextWidth(Font::Normal, link.label) + S(28), S(28)};
            DrawButton(r, link.label, p);
            g_prefs.links.push_back(r);
            lx = r.R() + S(8);
        }
    bool any = false;
    for (int i : shown) any = any || Changed(kPrefs[i]);
    const char* resetLabel = !g_prefs.search.empty() ? "Reset these to defaults"
                             : g_prefs.page         ? "Reset page to defaults"
                                                    : "Reset all to defaults";
    g_prefs.close = {right - S(90), by, S(90), S(28)};
    g_prefs.resetAll = {g_prefs.close.x - S(8) - S(190), by, S(190), S(28)};
    DrawButton(g_prefs.resetAll, resetLabel, p, false, any);
    DrawButton(g_prefs.close, "Close", p, true);
}

void SetPref(const Pref& t, double v) {
    v = std::clamp(v, (double)t.min, (double)t.max);
    if (t.type == PrefType::Float) v = std::round(v / t.step) * t.step;
    if (std::fabs(v - t.get()) > 1e-6) t.set(v);
    g.redraw = true;
}

void ClampPrefsScroll() {
    g_prefs.scroll = std::clamp(g_prefs.scroll, 0.0f, std::max(0.0f, g_prefs.contentH - g_prefs.list.h));
}

void PrefsMouseDown(float x, float y) {
    if (!g_prefs.panel.In(x, y) || g_prefs.close.In(x, y)) {
        ClosePanel();
        return;
    }
    for (size_t i = 0; i < g_prefs.pages.size(); ++i)
        if (g_prefs.pages[i].In(x, y)) {
            g_prefs.page = (int)i;
            g_prefs.search.clear();
            g_prefs.scroll = 0;
            g_prefs.focus = -1;
            return;
        }
    if (g_prefs.resetAll.In(x, y)) {
        for (int i : ShownPrefs())
            if (Changed(kPrefs[i])) SetPref(kPrefs[i], kPrefs[i].def);
        return;
    }
    if (g_prefs.search.empty())
        for (size_t i = 0; i < g_prefs.links.size(); ++i)
            if (g_prefs.links[i].In(x, y)) {
                // The link's window takes the place of Preferences, as one panel is open at a time.
                auto open = kPrefPages[g_prefs.page].links[i].open;
                ClosePanel();
                open();
                return;
            }
    if (!g_prefs.list.In(x, y)) return;
    for (const PrefRow& row : g_prefs.rows) {
        const Pref& t = kPrefs[row.pref];
        double v = t.get();
        if (row.toggle.In(x, y)) SetPref(t, v != 0 ? 0 : 1);
        else if (row.less.In(x, y)) SetPref(t, v - t.step);
        else if (row.more.In(x, y)) SetPref(t, v + t.step);
        else if (row.reset.w > 0 && row.reset.In(x, y)) SetPref(t, t.def);
        else continue;
        auto shown = ShownPrefs();
        g_prefs.focus = (int)(std::find(shown.begin(), shown.end(), row.pref) - shown.begin());
        return;
    }
}

// Typing searches; Up and Down pick a setting, Left and Right step it (Space flips an on/off one),
// Delete resets it, Escape clears the search and then closes.
void PrefsKey(const SDL_KeyboardEvent& e) {
    auto shown = ShownPrefs();
    int& focus = g_prefs.focus;
    const Pref* t = focus >= 0 && focus < (int)shown.size() ? &kPrefs[shown[focus]] : nullptr;
    switch (e.key) {
        case SDLK_ESCAPE:
            if (!g_prefs.search.empty()) {
                g_prefs.search.clear();
                focus = -1;
            } else ClosePanel();
            break;
        case SDLK_BACKSPACE:
            if (e.mod & SDL_KMOD_CTRL) g_prefs.search.clear();
            else if (!g_prefs.search.empty()) g_prefs.search.pop_back();
            g_prefs.scroll = 0;
            focus = -1;
            break;
        case SDLK_UP:
        case SDLK_DOWN:
            if (!shown.empty()) focus = std::clamp(focus + (e.key == SDLK_UP ? -1 : 1), 0, (int)shown.size() - 1);
            g_prefs.reveal = true;
            break;
        case SDLK_LEFT:
        case SDLK_RIGHT:
            if (t) SetPref(*t, t->get() + (e.key == SDLK_RIGHT ? t->step : -t->step));
            break;
        case SDLK_SPACE:
            if (t && t->type == PrefType::Bool) SetPref(*t, t->get() != 0 ? 0 : 1);
            break;
        case SDLK_DELETE:
            if (t) SetPref(*t, t->def);
            break;
        case SDLK_PAGEUP: g_prefs.scroll -= g_prefs.list.h; break;
        case SDLK_PAGEDOWN: g_prefs.scroll += g_prefs.list.h; break;
        default: break;
    }
    ClampPrefsScroll();
    g.redraw = true;
}

// --- one-line text fields (the rules and organize panels) ------------------------------------------
// Ctrl+A, deletion and caret movement in `t`. Returns 1 when the text changed, 0 when only the caret
// moved, and -1 for a key the field doesn't use.
int LineFieldKey(TextField& t, const SDL_KeyboardEvent& e) {
    if ((e.mod & SDL_KMOD_CTRL) && e.key == SDLK_A) {
        t.all = !t.text.empty();
        t.caret = t.text.size();
        g.redraw = true;
        return 0;
    }
    int changed = 0;
    if (e.key == SDLK_BACKSPACE || e.key == SDLK_DELETE) {
        if (t.all) t.text.clear();
        else if (e.key == SDLK_BACKSPACE && t.caret) t.text.erase(--t.caret, 1);
        else if (e.key == SDLK_DELETE && t.caret < t.text.size()) t.text.erase(t.caret, 1);
        t.caret = std::min(t.caret, t.text.size());
        changed = 1;
    } else if (e.key == SDLK_LEFT) {
        t.caret = t.all || !t.caret ? 0 : t.caret - 1;
    } else if (e.key == SDLK_RIGHT) {
        t.caret = t.all ? t.text.size() : std::min(t.caret + 1, t.text.size());
    } else if (e.key == SDLK_HOME) {
        t.caret = 0;
    } else if (e.key == SDLK_END) {
        t.caret = t.text.size();
    } else {
        return -1;
    }
    t.all = false;
    g.redraw = true;
    return changed;
}

// Typed text into `t`, replacing a selected whole; false when nothing was typed.
bool LineFieldInsert(TextField& t, const std::wstring& in) {
    if (in.empty()) return false;
    if (t.all) {
        t.text.clear();
        t.caret = 0;
    }
    t.all = false;
    t.text.insert(t.caret, in);
    t.caret += in.size();
    g.redraw = true;
    return true;
}

// --- auto-playlist rules ---------------------------------------------------------------------------
// The auto-playlist editor (Playlist > New auto-playlist, Edit auto-playlist rules): a name, ALL or ANY
// of the rules, the limit and pick order, and a live count of the tracks they select. The rules of
// nested groups are kept as they are; this editor changes the main group only.
struct RuleRow {
    Rule rule;
    TextField value, value2;
};

struct RulesPanel {
    int playlist = -1;   // -1: a new playlist
    AutoPlaylist model;  // what the editor keeps besides the rows: nested groups, pick, order
    std::vector<RuleRow> rows;
    TextField name, limit;
    bool limitOn = false;
    int first = 0;   // the first row shown
    int focus = 0;   // 0 name, 1 limit, 2 + 2 * row the value, 3 + 2 * row the second value; -1 none
    int count = -1;  // tracks the rules select; -1 until counted
    bool countDirty = true;
    std::string warning;
} g_rules;

constexpr float kRulesW = 640, kRulesH = 560;

struct RulesGeometry {
    Rect panel, name, match, add, limitBox, limitField, limitUnit, pick, ordered, count, cancel, save;
    std::vector<Rect> remove, field, op, value, value2;  // one each per visible row
    int visible = 0;
};

float RowH() { return std::round(LineHeight(Font::Normal) + S(12)); }

RulesGeometry Geometry() {
    RulesGeometry G;
    Rect out = OutputRect();
    float w = std::min(S(kRulesW), out.w - S(16)), h = std::min(S(kRulesH), out.h - S(16));
    Rect p{std::round((out.w - w) / 2), std::round((out.h - h) / 2), w, h};
    G.panel = p;
    const float rh = RowH(), px = p.x, pw = p.w, top = p.y + S(48), rowsTop = top + 2 * rh;
    const float footerY = p.B() - S(46), rowsBottom = footerY - S(10) - 3 * rh;
    G.name = {px + S(90), top, pw - S(106), rh - S(4)};
    G.match = {px + S(90), top + rh, S(110), rh - S(6)};
    G.visible = std::max(1, (int)std::floor((rowsBottom - rowsTop) / rh));
    for (int k = 0; k < G.visible; ++k) {
        const float y = rowsTop + k * rh, rowH = rh - S(6);
        const int i = g_rules.first + k;
        const bool between = i < (int)g_rules.rows.size() && g_rules.rows[i].rule.op == RuleOp::Between;
        const float vx = px + S(370), vw = pw - S(370) - S(16) - (between ? S(96) : 0);
        G.remove.push_back({px + S(16), y, S(22), rowH});
        G.field.push_back({px + S(44), y, S(170), rowH});
        G.op.push_back({px + S(222), y, S(140), rowH});
        G.value.push_back({vx, y, vw, rowH});
        G.value2.push_back(between ? Rect{vx + vw + S(6), y, S(90), rowH} : Rect{});
    }
    const float ya = rowsTop + G.visible * rh + S(4);
    G.add = {px + S(16), ya, S(120), rh - S(6)};
    G.limitBox = {px + S(16), ya + rh, S(120), rh - S(6)};
    G.limitField = {px + S(150), ya + rh, S(90), rh - S(4)};
    G.limitUnit = {px + S(250), ya + rh, S(140), rh - S(6)};
    G.pick = {px + S(90), ya + 2 * rh, S(180), rh - S(6)};
    G.ordered = {px + S(290), ya + 2 * rh, S(230), rh - S(6)};
    G.count = {px + S(16), footerY + S(10), pw - S(230), rh};
    G.cancel = {px + pw - S(96), footerY, S(80), rh};
    G.save = {px + pw - S(186), footerY, S(84), rh};
    return G;
}

std::wstring Trimmed(const std::wstring& s) {
    size_t b = s.find_first_not_of(L" \t"), e = s.find_last_not_of(L" \t");
    return b == std::wstring::npos ? std::wstring() : s.substr(b, e - b + 1);
}

// Operators that need no value, and those whose value is a number.
bool NoValue(RuleOp op) { return op == RuleOp::IsEmpty || op == RuleOp::NotEmpty || op == RuleOp::Never; }
bool NeedsNumber(const Rule& r) {
    switch (r.op) {
        case RuleOp::Equals:
        case RuleOp::NotEquals: return !IsTextField(r.field);
        case RuleOp::AtLeast:
        case RuleOp::AtMost:
        case RuleOp::Between:
        case RuleOp::InLastDays:
        case RuleOp::NotInLastDays: return true;
        default: return false;
    }
}
bool IsNumber(const std::wstring& s) {
    if (s.empty()) return false;
    size_t digits = 0, dots = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] >= L'0' && s[i] <= L'9') ++digits;
        else if (s[i] == L'.') ++dots;
        else if (!(s[i] == L'-' && i == 0)) return false;
    }
    return digits > 0 && dots <= 1;
}

// The text field of a focus index (see RulesPanel::focus), or null when it is not shown.
TextField* RulesField(int f) {
    if (f == 0) return &g_rules.name;
    if (f == 1) return g_rules.limitOn ? &g_rules.limit : nullptr;
    if (f < 2) return nullptr;
    const int i = (f - 2) / 2;
    if (i >= (int)g_rules.rows.size()) return nullptr;
    RuleRow& row = g_rules.rows[i];
    if ((f - 2) % 2 == 0) return NoValue(row.rule.op) ? nullptr : &row.value;
    return row.rule.op == RuleOp::Between ? &row.value2 : nullptr;
}

bool RulesNumeric(int f) {
    if (f == 1) return true;
    const int i = (f - 2) / 2;
    return f >= 2 && i < (int)g_rules.rows.size() && NeedsNumber(g_rules.rows[i].rule);
}

void RulesFocus(int f) {
    TextField* t = RulesField(f);
    g_rules.focus = t ? f : -1;
    if (t) {
        t->caret = t->text.size();
        t->all = false;
        SDL_StartTextInput(g.window);
    }
    g.redraw = true;
}

// The next shown field after `f` in tab order (wrapping), or -1.
int RulesStep(int f, int dir) {
    const int slots = 2 + 2 * (int)g_rules.rows.size();
    for (int n = 1; n <= slots; ++n) {
        const int next = ((f + dir * n) % slots + slots) % slots;
        if (RulesField(next)) return next;
    }
    return -1;
}

void RulesEdited() {
    g_rules.countDirty = true;
    g_rules.warning.clear();
    g.redraw = true;
}

// The rules as they would be saved, without checks; the count and the save both come from here.
AutoPlaylist RulesModel() {
    AutoPlaylist a = g_rules.model;
    a.root.rules.clear();
    for (const RuleRow& row : g_rules.rows) {
        Rule r = row.rule;
        r.value = Utf8(Trimmed(row.value.text));
        r.value2 = row.rule.op == RuleOp::Between ? Utf8(Trimmed(row.value2.text)) : std::string();
        if (NoValue(r.op)) r.value.clear();
        a.root.rules.push_back(r);
    }
    a.limit = g_rules.limitOn ? (uint32_t)std::max(0L, std::wcstol(Trimmed(g_rules.limit.text).c_str(), nullptr, 10)) : 0;
    return a;
}

void CountRules() {
    std::vector<TrackId> tracks;
    EvaluateAutoPlaylist(RulesModel(), tracks);
    g_rules.count = (int)tracks.size();
    g_rules.countDirty = false;
}

void RulesSave() {
    const std::wstring name = Trimmed(g_rules.name.text);
    auto fail = [](const std::string& text) {
        g_rules.warning = text;
        g.redraw = true;
    };
    if (name.empty()) return fail("Give the playlist a name.");
    if (g_rules.rows.empty() && g_rules.model.root.groups.empty()) return fail("Add a rule first.");
    for (size_t i = 0; i < g_rules.rows.size(); ++i) {
        const RuleRow& row = g_rules.rows[i];
        const std::string number = "Rule " + std::to_string(i + 1) + " needs a number.";
        if (!NoValue(row.rule.op) && Trimmed(row.value.text).empty()) return fail("Rule " + std::to_string(i + 1) + " needs a value.");
        if (NeedsNumber(row.rule) && !IsNumber(Trimmed(row.value.text))) return fail(number);
        if (row.rule.op == RuleOp::Between && !IsNumber(Trimmed(row.value2.text))) return fail(number);
    }
    if (g_rules.limitOn && !IsNumber(Trimmed(g_rules.limit.text))) return fail("Enter the limit as a number.");
    const std::string text = SerializeAutoPlaylist(RulesModel());
    std::wstring unique = name;
    const int self = g_rules.playlist;
    bool taken = false;
    for (size_t k = 0; k < g.playlists.size(); ++k)
        taken = taken || ((int)k != self && CompareStringOrdinal(g.playlists[k].name.c_str(), -1, name.c_str(), -1, TRUE) == CSTR_EQUAL);
    if (taken) unique = UniquePlaylistName(g.playlists, name);
    if (self < 0) {
        g.playlists.push_back(Playlist{unique, {}, PlaylistKind::Auto, text});
        PlaySfx(Sfx::Sparkle);
        SetActiveTab((int)g.playlists.size() - 1);
    } else if (self < (int)g.playlists.size()) {
        g.playlists[self].name = unique;
        g.playlists[self].rules = text;
    }
    g.libraryDirty = true;
    RebuildRows();
    ClosePanel();
}

std::vector<std::string> FieldLabels() {
    std::vector<std::string> labels;
    for (int f = 0; f < (int)RuleField::Count; ++f) labels.push_back(Utf8(RuleFieldName((RuleField)f)));
    return labels;
}

void RulesMouseDown(float x, float y) {
    const RulesGeometry G = Geometry();
    if (G.save.In(x, y)) return RulesSave();
    if (G.cancel.In(x, y)) return ClosePanel();
    if (G.name.In(x, y)) return RulesFocus(0);
    if (G.match.In(x, y)) {
        g_rules.model.root.all = !g_rules.model.root.all;
        return RulesEdited();
    }
    if (G.add.In(x, y)) {
        g_rules.rows.push_back(RuleRow{});
        g_rules.first = std::max(0, (int)g_rules.rows.size() - G.visible);
        RulesEdited();
        return RulesFocus(2 + 2 * ((int)g_rules.rows.size() - 1));
    }
    if (G.limitBox.In(x, y)) {
        g_rules.limitOn = !g_rules.limitOn;
        RulesEdited();
        if (g_rules.limitOn) RulesFocus(1);
        return;
    }
    if (G.limitField.In(x, y)) return g_rules.limitOn ? RulesFocus(1) : void();
    if (G.limitUnit.In(x, y)) {
        std::vector<std::string> labels;
        for (int u = 0; u < (int)LimitUnit::Count; ++u) labels.push_back(Utf8(LimitUnitName((LimitUnit)u)));
        OpenChoice(G.limitUnit.x, G.limitUnit.B(), labels, (int)g_rules.model.unit, [](int u) {
            g_rules.model.unit = (LimitUnit)u;
            RulesEdited();
        });
        return;
    }
    if (G.pick.In(x, y)) {
        std::vector<std::string> labels;
        for (int k = 0; k < (int)PickBy::Count; ++k) labels.push_back(Utf8(PickByName((PickBy)k)));
        OpenChoice(G.pick.x, G.pick.B(), labels, (int)g_rules.model.pick, [](int k) {
            g_rules.model.pick = (PickBy)k;
            RulesEdited();
        });
        return;
    }
    if (G.ordered.In(x, y)) {
        g_rules.model.ordered = !g_rules.model.ordered;
        return RulesEdited();
    }
    for (int k = 0; k < G.visible; ++k) {
        const int i = g_rules.first + k;
        if (i >= (int)g_rules.rows.size()) break;
        RuleRow& row = g_rules.rows[i];
        if (G.remove[k].In(x, y)) {
            g_rules.rows.erase(g_rules.rows.begin() + i);
            g_rules.first = std::clamp(g_rules.first, 0, std::max(0, (int)g_rules.rows.size() - 1));
            g_rules.focus = -1;
            return RulesEdited();
        }
        if (G.field[k].In(x, y)) {
            OpenChoice(G.field[k].x, G.field[k].B(), FieldLabels(), (int)row.rule.field, [i](int f) {
                RuleRow& r = g_rules.rows[i];
                r.rule.field = (RuleField)f;
                const std::vector<RuleOp> ops = OpsFor(r.rule.field);
                if (std::find(ops.begin(), ops.end(), r.rule.op) == ops.end()) r.rule.op = ops.front();
                RulesEdited();
            });
            return;
        }
        if (G.op[k].In(x, y)) {
            const std::vector<RuleOp> ops = OpsFor(row.rule.field);
            std::vector<std::string> labels;
            for (RuleOp op : ops) labels.push_back(Utf8(RuleOpName(op)));
            const int current = (int)(std::find(ops.begin(), ops.end(), row.rule.op) - ops.begin());
            OpenChoice(G.op[k].x, G.op[k].B(), labels, current, [i, ops](int o) {
                g_rules.rows[i].rule.op = ops[o];
                RulesEdited();
            });
            return;
        }
        if (G.value[k].In(x, y) && !NoValue(row.rule.op)) return RulesFocus(2 + 2 * i);
        if (G.value2[k].w > 0 && G.value2[k].In(x, y)) return RulesFocus(3 + 2 * i);
    }
}

void RulesKey(const SDL_KeyboardEvent& e) {
    if (e.key == SDLK_ESCAPE) return ClosePanel();
    if (e.key == SDLK_RETURN || e.key == SDLK_KP_ENTER) return RulesSave();
    if (e.key == SDLK_TAB) return RulesFocus(RulesStep(g_rules.focus, (e.mod & SDL_KMOD_SHIFT) ? -1 : 1));
    if (TextField* t = RulesField(g_rules.focus); t && LineFieldKey(*t, e) == 1) RulesEdited();
}

void RulesText(const std::wstring& s) {
    TextField* t = RulesField(g_rules.focus);
    if (!t) return;
    const bool numeric = RulesNumeric(g_rules.focus);
    std::wstring in;
    for (wchar_t c : s)
        if (c >= 32 && (!numeric || (c >= L'0' && c <= L'9') || c == L'.' || c == L'-')) in += c;
    if (LineFieldInsert(*t, in)) RulesEdited();
}

void DrawRulesEdit(TextField& t, const Rect& r, bool focused, bool enabled, const char* cue, const Palette& p) {
    Fill(r, enabled ? p.surface2 : p.surface);
    Frame(r, focused ? p.accent : p.border);
    const float pad = S(5), lineH = LineHeight(Font::Normal);
    SDL_Rect clip{(int)(r.x + S(2)), (int)(r.y + S(1)), (int)(r.w - S(4)), (int)(r.h - S(2))};
    SDL_SetRenderClipRect(g.renderer, &clip);
    if (t.text.empty() && !focused && cue) DrawText(Font::Normal, cue, r.x + pad, TextY(r), p.faint);
    const std::string all = Utf8(t.text);
    const float caretX = TextWidth(Font::Normal, Utf8(t.text.substr(0, t.caret))), room = r.w - 2 * pad;
    if (caretX - t.scrollX > room) t.scrollX = caretX - room;
    if (caretX < t.scrollX) t.scrollX = caretX;
    const float tx = r.x + pad - t.scrollX, ty = TextY(r);
    if (focused && t.all) Fill({tx, r.y + S(3), TextWidth(Font::Normal, all), r.h - S(6)}, p.select);
    DrawText(Font::Normal, all, tx, ty, focused && t.all ? p.selectText : p.text);
    if (focused && enabled && SDL_GetTicks() % 1060 < 530) Fill({std::round(tx + caretX), ty, S(1), lineH}, p.text);
    SDL_SetRenderClipRect(g.renderer, nullptr);
}

void DrawRules(const Palette& p) {
    if (g_rules.countDirty) CountRules();
    const RulesGeometry G = Geometry();
    DrawFrame(p, S(kRulesW), S(kRulesH), g_rules.playlist < 0 ? "New auto-playlist" : "Edit auto-playlist rules");
    DrawText(Font::Normal, "Name", G.panel.x + S(16), TextY(G.name), p.sub);
    DrawRulesEdit(g_rules.name, G.name, g_rules.focus == 0, true, "Playlist name", p);
    DrawText(Font::Normal, "Match", G.panel.x + S(16), TextY(G.match), p.sub);
    DrawButton(G.match, g_rules.model.root.all ? "All rules" : "Any rule", p);
    DrawText(Font::Normal, "of the following rules:", G.match.R() + S(12), TextY(G.match), p.sub);
    if (g_rules.rows.empty()) DrawText(Font::Normal, "No rules yet. Add one below.", G.panel.x + S(44), G.match.B() + S(10), p.faint);
    for (int k = 0; k < G.visible; ++k) {
        const int i = g_rules.first + k;
        if (i >= (int)g_rules.rows.size()) break;
        RuleRow& row = g_rules.rows[i];
        DrawButton(G.remove[k], "\xC3\x97", p);
        DrawButton(G.field[k], Utf8(RuleFieldName(row.rule.field)), p);
        DrawButton(G.op[k], Utf8(RuleOpName(row.rule.op)), p);
        DrawRulesEdit(row.value, G.value[k], g_rules.focus == 2 + 2 * i, !NoValue(row.rule.op), "value", p);
        if (row.rule.op == RuleOp::Between) DrawRulesEdit(row.value2, G.value2[k], g_rules.focus == 3 + 2 * i, true, "and", p);
    }
    DrawButton(G.add, "Add rule", p);
    DrawCheck(G.limitBox, "Limit to", g_rules.limitOn, p);
    DrawRulesEdit(g_rules.limit, G.limitField, g_rules.focus == 1, g_rules.limitOn, nullptr, p);
    DrawButton(G.limitUnit, Utf8(LimitUnitName(g_rules.model.unit)), p, false, g_rules.limitOn);
    DrawText(Font::Normal, "Pick", G.panel.x + S(16), TextY(G.pick), p.sub);
    DrawButton(G.pick, Utf8(PickByName(g_rules.model.pick)), p);
    DrawCheck(G.ordered, "Keep the pick order", g_rules.model.ordered, p);
    std::string status = g_rules.warning;
    if (status.empty() && g_rules.count >= 0)
        status = std::to_string(g_rules.count) + (g_rules.count == 1 ? " track matches" : " tracks match");
    DrawText(Font::Normal, status, G.count.x, TextY(G.count), g_rules.warning.empty() ? p.sub : p.accent, G.count.w);
    DrawButton(G.cancel, "Cancel", p);
    DrawButton(G.save, "Save", p, true);
}

// --- organize files ------------------------------------------------------------------------------------
// Library > Organize files, as the Windows window: the folder and naming pattern, a preview of every
// move (replanned once typing pauses), and the move itself on organize_run.cpp's worker.
struct OrganizePanel {
    std::vector<TrackId> ids;
    std::vector<OrganizeMove> plan;
    TextField root, pattern;
    int focus = 0;  // 0 the folder, 1 the pattern
    int first = 0;  // the first preview row shown
    size_t moving = 0, inPlace = 0, blocked = 0;
    bool dirty = true;
    uint64_t editedAt = 0;
} g_org;

constexpr float kOrganizeW = 900, kOrganizeH = 560;
constexpr uint64_t kReplanDelay = 300;  // ms after the last keystroke, as the Windows dialog's timer

struct OrganizeGeometry {
    Rect panel, root, browse, pattern, reset, help, list, summary, go, cancel;
    int visible = 0;
};

OrganizeGeometry OrgGeometry() {
    OrganizeGeometry G;
    Rect out = OutputRect();
    float w = std::min(S(kOrganizeW), out.w - S(16)), h = std::min(S(kOrganizeH), out.h - S(16));
    Rect p{std::round((out.w - w) / 2), std::round((out.h - h) / 2), w, h};
    G.panel = p;
    const float rh = RowH(), pad = S(16), lw = S(70), top = p.y + S(48);
    G.root = {p.x + pad + lw, top, p.w - 2 * pad - lw - S(104), rh - S(4)};
    G.browse = {p.R() - pad - S(96), top, S(96), rh - S(4)};
    G.pattern = {G.root.x, top + rh, G.root.w, rh - S(4)};
    G.reset = {G.browse.x, top + rh, S(96), rh - S(4)};
    G.help = {G.root.x, top + 2 * rh, p.R() - pad - G.root.x, 2 * LineHeight(Font::Normal) + S(4)};
    const float footerY = p.B() - S(46), listTop = G.help.B() + S(10) + rh;  // under a heading row
    G.list = {p.x + pad, listTop, p.w - 2 * pad, std::max(rh, footerY - S(10) - listTop)};
    G.visible = std::max(1, (int)std::floor(G.list.h / rh));
    G.summary = {p.x + pad, footerY, p.w - 2 * pad - S(230), rh};
    G.cancel = {p.R() - pad - S(96), footerY, S(96), rh};
    G.go = {G.cancel.x - S(116), footerY, S(108), rh};
    return G;
}

void Replan() {
    g_org.plan = PlanOrganize(g_org.ids, Trimmed(g_org.root.text), g_org.pattern.text);
    g_org.moving = g_org.inPlace = g_org.blocked = 0;
    for (OrganizeMove& m : g_org.plan) {
        if (m.skip.empty() && m.id == g.playing) m.skip = L"playing now";
        if (m.skip.empty()) ++g_org.moving;
        else if (m.skip == L"already in place") ++g_org.inPlace;
        else ++g_org.blocked;
    }
    g_org.first = std::clamp(g_org.first, 0, std::max(0, (int)g_org.plan.size() - 1));
    g_org.dirty = false;
}

void OrganizeEdited() {
    g_org.dirty = true;
    g_org.editedAt = SDL_GetTicks();
    g.redraw = true;
}

void OrganizeFocus(int f) {
    g_org.focus = f;
    TextField& t = f ? g_org.pattern : g_org.root;
    t.caret = t.text.size();
    t.all = false;
    SDL_StartTextInput(g.window);
    g.redraw = true;
}

void OrganizeStart() {
    const std::wstring root = Trimmed(g_org.root.text);
    if (g_org.dirty) Replan();
    if (root.empty() || !g_org.moving || OrganizeBusy()) return;
    g.settings.organizeFolder = root;
    g.settings.organizePattern = g_org.pattern.text;
    SaveSettings();
    StartOrganize(g_org.plan, root);
    if (OrganizeBusy()) ClosePanel();
}

void OrganizeMouseDown(float x, float y) {
    const OrganizeGeometry G = OrgGeometry();
    if (G.go.In(x, y)) return OrganizeStart();
    if (G.cancel.In(x, y)) return ClosePanel();
    if (G.root.In(x, y)) return OrganizeFocus(0);
    if (G.pattern.In(x, y)) return OrganizeFocus(1);
    if (G.reset.In(x, y)) {
        g_org.pattern.text = kDefaultOrganizePattern;
        OrganizeFocus(1);
        return OrganizeEdited();
    }
    if (G.browse.In(x, y)) {
        auto chosen = [](void*, const char* const* list, int) {
            if (!list || !*list) return;
            auto* path = new std::wstring(compat::Widen(*list));
            if (!PostMessageW(nullptr, WM_APP_FILE, (WPARAM)kFileOrganizeRoot, (LPARAM)path)) delete path;
        };
        const std::string start = compat::Narrow(Trimmed(g_org.root.text).c_str());
        SDL_ShowOpenFolderDialog(chosen, nullptr, g.window, start.empty() ? nullptr : start.c_str(), false);
    }
}

void OrganizeKey(const SDL_KeyboardEvent& e) {
    if (e.key == SDLK_ESCAPE) return ClosePanel();
    if (e.key == SDLK_RETURN || e.key == SDLK_KP_ENTER) return OrganizeStart();
    if (e.key == SDLK_TAB) return OrganizeFocus(1 - g_org.focus);
    if (LineFieldKey(g_org.focus ? g_org.pattern : g_org.root, e) == 1) OrganizeEdited();
}

void OrganizeText(const std::wstring& s) {
    std::wstring in;
    for (wchar_t c : s)
        if (c >= 32) in += c;
    if (LineFieldInsert(g_org.focus ? g_org.pattern : g_org.root, in)) OrganizeEdited();
}

void DrawOrganize(const Palette& p) {
    if (g_org.dirty && SDL_GetTicks() - g_org.editedAt >= kReplanDelay) Replan();
    const OrganizeGeometry G = OrgGeometry();
    const size_t n = g_org.ids.size();
    DrawFrame(p, S(kOrganizeW), S(kOrganizeH), ("Organize " + std::to_string(n) + (n == 1 ? " file" : " files")).c_str());
    const float pad = S(16), lh = LineHeight(Font::Normal);
    DrawText(Font::Normal, "Folder", G.panel.x + pad, TextY(G.root), p.sub);
    DrawRulesEdit(g_org.root, G.root, g_org.focus == 0, true, "The folder to organize into", p);
    DrawButton(G.browse, "Browse...", p);
    DrawText(Font::Normal, "Names", G.panel.x + pad, TextY(G.pattern), p.sub);
    DrawRulesEdit(g_org.pattern, G.pattern, g_org.focus == 1, true, nullptr, p);
    DrawButton(G.reset, "Default", p);
    DrawText(Font::Normal, "Title formatting: %album artist%, %album%, %title%, %tracknumber%, %discnumber%, %year%, %genre% ...",
             G.help.x, G.help.y, p.sub, G.help.w);
    DrawText(Font::Normal, "A backslash starts a new folder; [ ] is left out when its fields are empty. The extension is kept.",
             G.help.x, G.help.y + lh + S(2), p.sub, G.help.w);

    // The preview: From, To (shown under the chosen folder) and what happens to each file.
    const float rh = RowH(), statusW = S(130), half = std::floor((G.list.w - statusW) / 2);
    const float cols[] = {G.list.x + S(6), G.list.x + half + S(6), G.list.x + 2 * half + S(6)};
    const Rect heading{G.list.x, G.list.y - rh, G.list.w, rh};
    DrawText(Font::Normal, "From", cols[0], TextY(heading), p.sub);
    DrawText(Font::Normal, "To", cols[1], TextY(heading), p.sub);
    DrawText(Font::Normal, "Status", cols[2], TextY(heading), p.sub);
    Fill(G.list, p.surface2);
    Frame(G.list, p.border);
    const std::wstring root = Trimmed(g_org.root.text);
    for (int k = 0; k < G.visible; ++k) {
        const int i = g_org.first + k;
        if (i >= (int)g_org.plan.size()) break;
        const OrganizeMove& m = g_org.plan[i];
        const Rect row{G.list.x, G.list.y + k * rh, G.list.w, rh};
        std::wstring to = m.to;
        if (!root.empty() && to.size() > root.size() && to.compare(0, root.size(), root) == 0) to = L"\x2026" + to.substr(root.size());
        const uint32_t color = m.skip.empty() ? p.text : p.sub;
        DrawText(Font::Normal, Utf8(m.from), cols[0], TextY(row), color, half - S(12));
        DrawText(Font::Normal, Utf8(to), cols[1], TextY(row), color, half - S(12));
        DrawText(Font::Normal, m.skip.empty() ? std::string("move") : Utf8(m.skip), cols[2], TextY(row), color, statusW - S(12));
    }

    std::string status = std::to_string(g_org.moving) + (g_org.moving == 1 ? " file will move" : " files will move");
    if (g_org.inPlace) status += "  \xE2\x80\xA2  " + std::to_string(g_org.inPlace) + " already in place";
    if (g_org.blocked) status += "  \xE2\x80\xA2  " + std::to_string(g_org.blocked) + " can't move";
    if (root.empty()) status = "Choose the folder to organize into.";
    else if (g_org.dirty) status = "Updating the preview...";
    DrawText(Font::Normal, status, G.summary.x, TextY(G.summary), p.sub, G.summary.w);
    DrawButton(G.go, "Organize", p, true, !g_org.dirty && g_org.moving && !root.empty() && !OrganizeBusy());
    DrawButton(G.cancel, "Cancel", p);
}

// --- copy to folder or device ------------------------------------------------------------------------
// Library > Copy to folder or device, as the Windows window: the destination (a folder, a memory card,
// or a phone's mounted folder), an Organize naming pattern, the format, the playlist's options, and a
// preview of every file, replanned once typing pauses. The copy runs on copy_run.cpp's worker.
struct CopyPanel {
    std::vector<TrackId> ids;
    std::wstring set, playlistName;  // the playlist's name, or kCopySelectionSet for a selection
    std::wstring manifestFor;        // the destination whose manifest is read (once, not per keystroke)
    std::vector<CopyRecord> manifest;
    std::vector<CopyItem> plan;
    TextField dest, pattern;
    CopyFormat format = CopyFormat::Original;
    bool writePlaylist = false, removeStale = false;
    int focus = 0;  // 0 the destination, 1 the pattern
    int first = 0;
    size_t copying = 0, converting = 0, there = 0, blocked = 0, stale = 0;
    uint64_t bytes = 0;
    bool dirty = true;
    uint64_t editedAt = 0;
} g_copy;

const char* const kCopyFormats[] = {"Keep the original files", "Lossless to AAC 192 kb/s (.m4a)", "Lossless to AAC 128 kb/s (.m4a)"};
static_assert(std::size(kCopyFormats) == (size_t)CopyFormat::Count);
constexpr float kCopyW = 940, kCopyH = 580;

bool PlaylistCopy() { return g_copy.set != kCopySelectionSet; }

struct CopyGeometry {
    Rect panel, dest, browse, pattern, reset, format, playlist, stale, help, list, summary, go, cancel;
    int visible = 0;
};

CopyGeometry CopyGeom() {
    CopyGeometry G;
    Rect out = OutputRect();
    float w = std::min(S(kCopyW), out.w - S(16)), h = std::min(S(kCopyH), out.h - S(16));
    Rect p{std::round((out.w - w) / 2), std::round((out.h - h) / 2), w, h};
    G.panel = p;
    const float rh = RowH(), pad = S(16), lw = S(70), top = p.y + S(48), x = p.x + pad + lw;
    G.dest = {x, top, p.w - 2 * pad - lw - S(104), rh - S(4)};
    G.browse = {p.R() - pad - S(96), top, S(96), rh - S(4)};
    G.pattern = {x, top + rh, G.dest.w, rh - S(4)};
    G.reset = {G.browse.x, top + rh, S(96), rh - S(4)};
    G.format = {x, top + 2 * rh, S(260), rh - S(6)};
    G.playlist = {x + S(276), top + 2 * rh, p.R() - pad - x - S(276), rh - S(6)};
    G.stale = {x, top + 3 * rh, p.R() - pad - x, rh - S(6)};
    G.help = {x, top + 4 * rh, p.R() - pad - x, 2 * LineHeight(Font::Normal) + S(4)};
    const float footerY = p.B() - S(46), listTop = G.help.B() + S(10) + rh;  // under a heading row
    G.list = {p.x + pad, listTop, p.w - 2 * pad, std::max(rh, footerY - S(10) - listTop)};
    G.visible = std::max(1, (int)std::floor(G.list.h / rh));
    G.summary = {p.x + pad, footerY, p.w - 2 * pad - S(230), rh};
    G.cancel = {p.R() - pad - S(96), footerY, S(96), rh};
    G.go = {G.cancel.x - S(116), footerY, S(108), rh};
    return G;
}

std::wstring CopyDestination() {
    std::wstring d = Trimmed(g_copy.dest.text);
    while (d.size() > 1 && d.back() == L'/') d.pop_back();
    return d;
}

// The plan and the summary's counts: what will be copied and converted, roughly how much space it
// takes, and what earlier copies the playlist no longer wants.
void CopyReplan() {
    const std::wstring dest = CopyDestination();
    if (dest != g_copy.manifestFor) {
        g_copy.manifestFor = dest;
        g_copy.manifest.clear();
        const DWORD a = GetFileAttributesW(dest.c_str());
        if (!dest.empty() && a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) ReadCopyManifest(dest, false, g_copy.manifest);
    }
    g_copy.plan = PlanCopy(g_copy.ids, dest, false, g_copy.pattern.text, g_copy.format, g_copy.manifest);
    g_copy.copying = g_copy.converting = g_copy.there = g_copy.blocked = 0;
    g_copy.bytes = 0;
    for (const CopyItem& item : g_copy.plan) {
        if (!item.skip.empty()) ++g_copy.blocked;
        else if (item.done) ++g_copy.there;
        else {
            ++g_copy.copying;
            g_copy.converting += item.convert;
            g_copy.bytes += item.estimate;
        }
    }
    g_copy.stale = PlaylistCopy() ? StaleCopies(g_copy.plan, g_copy.manifest, g_copy.set).size() : 0;
    g_copy.first = std::clamp(g_copy.first, 0, std::max(0, (int)g_copy.plan.size() - 1));
    g_copy.dirty = false;
}

void CopyEdited() {
    g_copy.dirty = true;
    g_copy.editedAt = SDL_GetTicks();
    g.redraw = true;
}

void CopyFocus(int f) {
    g_copy.focus = f;
    TextField& t = f ? g_copy.pattern : g_copy.dest;
    t.caret = t.text.size();
    t.all = false;
    SDL_StartTextInput(g.window);
    g.redraw = true;
}

bool CanStartCopy() {
    return !CopyDestination().empty() && !CopyBusy() && (g_copy.copying || (g_copy.stale && g_copy.removeStale));
}

// True when `dir` is a library folder or inside one: copies there would be scanned in as duplicates.
bool InLibraryFolder(const std::wstring& dir) {
    for (std::wstring root : g.settings.folders) {
        while (root.size() > 1 && root.back() == L'/') root.pop_back();
        if (dir.compare(0, root.size(), root) == 0 && (dir.size() == root.size() || dir[root.size()] == L'/')) return true;
    }
    return false;
}

// Saves the fields, checks the destination, asks before anything would be removed, and hands the
// plan to the worker; the panel closes and the result arrives as WM_APP_COPY.
void CopyStart() {
    const std::wstring dest = CopyDestination();
    if (dest.empty() || CopyBusy()) return;
    if (OrganizeBusy()) {
        ShowMessage(L"Wait for the running organize to finish.");
        return;
    }
    g.settings.copyFolder = dest;
    g.settings.copyPattern = g_copy.pattern.text;
    g.settings.copyFormat = (uint8_t)g_copy.format;
    SaveSettings();
    if (InLibraryFolder(dest)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Copy to folder or device",
                                 "That folder is in your library, so its copies would show up as duplicates. Choose a folder outside it.",
                                 g.window);
        return;
    }
    SHCreateDirectoryExW(nullptr, dest.c_str(), nullptr);
    const DWORD a = GetFileAttributesW(dest.c_str());
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "Copy to folder or device", ("Couldn't open or create " + Utf8(dest)).c_str(), g.window);
        return;
    }
    g_copy.manifestFor.clear();  // read it again: another program may have changed the destination
    CopyReplan();
    if (!CanStartCopy()) return;
    const bool removeStale = g_copy.stale && g_copy.removeStale;
    if (removeStale) {
        std::wstring q = L"Remove " + std::to_wstring(g_copy.stale) + (g_copy.stale == 1 ? L" earlier copy" : L" earlier copies") +
                         L" that \x201C" + g_copy.playlistName + L"\x201D no longer holds?\n\nOnly files cordinal copied there are removed. "
                         L"Memory cards and phones have no trash, so they are deleted for good (the originals on this computer stay).";
        if (!Confirm(Utf8(q), "Copy to folder or device")) return;
    }
    CopySettings settings;
    settings.dest = dest;
    settings.set = g_copy.set;
    settings.removeStale = removeStale;
    settings.kbps = CopyKbps(g_copy.format);
    if (PlaylistCopy() && g_copy.writePlaylist) {
        // Every occurrence, in the playlist's order, by its path relative to the playlist file.
        std::unordered_map<TrackId, std::string> where;
        for (const CopyItem& item : g_copy.plan)
            if (item.skip.empty()) where[item.id] = Utf8(item.rel);
        std::vector<std::pair<TrackId, std::string>> entries;
        for (TrackId id : g_copy.ids)
            if (auto w = where.find(id); w != where.end()) entries.push_back({id, w->second});
        settings.playlistName = g_copy.playlistName;
        settings.playlist = M3UText(entries);
    }
    StartCopy(std::move(g_copy.plan), std::move(settings), g_copy.manifest);
    ClosePanel();
}

void CopyMouseDown(float x, float y) {
    const CopyGeometry G = CopyGeom();
    if (G.go.In(x, y)) return CanStartCopy() && !g_copy.dirty ? CopyStart() : void();
    if (G.cancel.In(x, y)) return ClosePanel();
    if (G.dest.In(x, y)) return CopyFocus(0);
    if (G.pattern.In(x, y)) return CopyFocus(1);
    if (G.reset.In(x, y)) {
        g_copy.pattern.text = kDefaultOrganizePattern;
        CopyFocus(1);
        return CopyEdited();
    }
    if (G.format.In(x, y)) {
        OpenChoice(G.format.x, G.format.B(), {std::begin(kCopyFormats), std::end(kCopyFormats)}, (int)g_copy.format, [](int f) {
            g_copy.format = (CopyFormat)f;
            CopyReplan();
            g.redraw = true;
        });
        return;
    }
    if (G.playlist.In(x, y) && PlaylistCopy()) {
        g_copy.writePlaylist = !g_copy.writePlaylist;
        return;
    }
    if (G.stale.In(x, y) && PlaylistCopy()) {
        g_copy.removeStale = !g_copy.removeStale;
        return;
    }
    if (G.browse.In(x, y)) {
        auto chosen = [](void*, const char* const* list, int) {
            if (!list || !*list) return;
            auto* path = new std::wstring(compat::Widen(*list));
            if (!PostMessageW(nullptr, WM_APP_FILE, (WPARAM)kFileCopyDest, (LPARAM)path)) delete path;
        };
        const std::string start = compat::Narrow(CopyDestination().c_str());
        SDL_ShowOpenFolderDialog(chosen, nullptr, g.window, start.empty() ? nullptr : start.c_str(), false);
    }
}

void CopyKey(const SDL_KeyboardEvent& e) {
    if (e.key == SDLK_ESCAPE) return ClosePanel();
    if (e.key == SDLK_RETURN || e.key == SDLK_KP_ENTER) {
        if (g_copy.dirty) CopyReplan();
        return CanStartCopy() ? CopyStart() : void();
    }
    if (e.key == SDLK_TAB) return CopyFocus(1 - g_copy.focus);
    if (LineFieldKey(g_copy.focus ? g_copy.pattern : g_copy.dest, e) == 1) CopyEdited();
}

void CopyText(const std::wstring& s) {
    std::wstring in;
    for (wchar_t c : s)
        if (c >= 32) in += c;
    if (LineFieldInsert(g_copy.focus ? g_copy.pattern : g_copy.dest, in)) CopyEdited();
}

void DrawCopy(const Palette& p) {
    if (g_copy.dirty && SDL_GetTicks() - g_copy.editedAt >= kReplanDelay) CopyReplan();
    const CopyGeometry G = CopyGeom();
    const std::string what = PlaylistCopy() ? "\xE2\x80\x9C" + Utf8(g_copy.playlistName) + "\xE2\x80\x9D"
                                            : std::to_string(g_copy.ids.size()) + (g_copy.ids.size() == 1 ? " song" : " songs");
    DrawFrame(p, S(kCopyW), S(kCopyH), ("Copy " + what + " to a folder or device").c_str());
    const float pad = S(16), lh = LineHeight(Font::Normal);
    DrawText(Font::Normal, "Copy to", G.panel.x + pad, TextY(G.dest), p.sub);
    DrawRulesEdit(g_copy.dest, G.dest, g_copy.focus == 0, true, "A folder, a memory card or a phone's folder", p);
    DrawButton(G.browse, "Browse...", p);
    DrawText(Font::Normal, "Names", G.panel.x + pad, TextY(G.pattern), p.sub);
    DrawRulesEdit(g_copy.pattern, G.pattern, g_copy.focus == 1, true, nullptr, p);
    DrawButton(G.reset, "Default", p);
    DrawText(Font::Normal, "Format", G.panel.x + pad, TextY(G.format), p.sub);
    DrawButton(G.format, kCopyFormats[(int)g_copy.format], p);
    DrawCheck(G.playlist, "Write a playlist (.m3u8)", PlaylistCopy() && g_copy.writePlaylist, p);
    DrawCheck(G.stale, "Remove songs no longer in the playlist", PlaylistCopy() && g_copy.removeStale, p);
    DrawText(Font::Normal, "Names as in Organize files: a backslash starts a new folder. Lossy files are copied as they are.",
             G.help.x, G.help.y, p.sub, G.help.w);
    DrawText(Font::Normal, "ReplayGain goes into the copies, so players that read it (foobar2000, Poweramp) level the music.",
             G.help.x, G.help.y + lh + S(2), p.sub, G.help.w);

    const float rh = RowH(), statusW = S(130), half = std::floor((G.list.w - statusW) / 2);
    const float cols[] = {G.list.x + S(6), G.list.x + half + S(6), G.list.x + 2 * half + S(6)};
    const Rect heading{G.list.x, G.list.y - rh, G.list.w, rh};
    DrawText(Font::Normal, "From", cols[0], TextY(heading), p.sub);
    DrawText(Font::Normal, "To", cols[1], TextY(heading), p.sub);
    DrawText(Font::Normal, "Status", cols[2], TextY(heading), p.sub);
    Fill(G.list, p.surface2);
    Frame(G.list, p.border);
    for (int k = 0; k < G.visible; ++k) {
        const int i = g_copy.first + k;
        if (i >= (int)g_copy.plan.size()) break;
        const CopyItem& item = g_copy.plan[i];
        const Rect row{G.list.x, G.list.y + k * rh, G.list.w, rh};
        const std::wstring status = !item.skip.empty() ? item.skip : item.done ? L"already there" : item.convert ? L"convert to AAC" : L"copy";
        const uint32_t color = item.skip.empty() && !item.done ? p.text : p.sub;
        DrawText(Font::Normal, Utf8(item.from), cols[0], TextY(row), color, half - S(12));
        DrawText(Font::Normal, Utf8(item.rel), cols[1], TextY(row), color, half - S(12));
        DrawText(Font::Normal, Utf8(status), cols[2], TextY(row), color, statusW - S(12));
    }

    const std::wstring dest = CopyDestination();
    std::wstring s = std::to_wstring(g_copy.copying) + (g_copy.copying == 1 ? L" song" : L" songs") + L" to copy";
    if (g_copy.converting) s += L" (" + std::to_wstring(g_copy.converting) + L" as AAC)";
    if (g_copy.copying) s += L", about " + CopySize(g_copy.bytes);
    struct statvfs fs;
    if (!dest.empty() && statvfs(compat::Narrow(dest.c_str()).c_str(), &fs) == 0) {
        const uint64_t free = (uint64_t)fs.f_bavail * fs.f_frsize;
        s += (g_copy.bytes > free ? L" \x2014 more than the " : L" of ") + CopySize(free) + L" free";
    }
    if (g_copy.there) s += L"  \x2022  " + std::to_wstring(g_copy.there) + L" already there";
    if (g_copy.blocked) s += L"  \x2022  " + std::to_wstring(g_copy.blocked) + L" can't be copied";
    if (g_copy.stale && g_copy.removeStale) s += L"  \x2022  " + std::to_wstring(g_copy.stale) + L" earlier copies to remove";
    if (dest.empty()) s = L"Choose a folder, a memory card or a phone to copy to.";
    else if (g_copy.dirty) s = L"Updating the preview...";
    DrawText(Font::Normal, Utf8(s), G.summary.x, TextY(G.summary), p.sub, G.summary.w);
    DrawButton(G.go, "Copy", p, true, !g_copy.dirty && CanStartCopy());
    DrawButton(G.cancel, "Cancel", p);
}

// --- title formatting ---------------------------------------------------------------------------------
// Preferences > View > Title formatting: the four patterns that make the now-playing lines, the status bar
// and the window title (app_format.cpp), each with a live preview. They apply as they are typed; Cancel puts
// them back.
constexpr int kFormats = 4;
constexpr float kFmtW = 720, kFmtH = 520;

struct FormatSpec {
    const char* label;
    const wchar_t* def;
    std::wstring Settings::*target;
};
const FormatSpec kFormatSpecs[kFormats] = {
    {"Now playing", L"%title%", &Settings::fmtTitle},
    {"Now playing, second line", L"[%artist%][  \x2022  %album%][  \x2022  %date%]", &Settings::fmtSub},
    {"Status bar", L"[%codec% | ][%bitrate% kbps | ][%samplerate% Hz | ][%channels% | ]%playback_time% / %length%",
     &Settings::fmtStatus},
    {"Window title", L"[%artist% - ]%title% - cordinal", &Settings::fmtWindow},
};

struct FormatPanel {
    TextField fields[kFormats];
    std::wstring original[kFormats];
    int focus = 0;
    TrackId sample = kNone;
    Rect panel, resets[kFormats], resetAll, ok, cancel;
} g_fmt;

// The track the previews show: what's playing, else the first selected one, else the first of the library.
TrackId FormatSample() {
    if (g.playing != kNone) return g.playing;
    for (size_t r = 0; r < g.rows.size() && r < g.selected.size(); ++r)
        if (g.selected[r]) return g.rows[r];
    for (TrackId id = 0; id < (TrackId)g_lib.Count(); ++id)
        if (g_lib[id].InLibrary() && g_lib[id].Loaded()) return id;
    return kNone;
}

void FormatEdited(int i) {
    g.settings.*kFormatSpecs[i].target = g_fmt.fields[i].text;
    g.redraw = true;
}

void FormatFocus(int i) {
    g_fmt.focus = i;
    g_fmt.fields[i].all = !g_fmt.fields[i].text.empty();
    g_fmt.fields[i].caret = g_fmt.fields[i].text.size();
    g.redraw = true;
}

void FormatClose(bool keep) {
    if (keep) {
        SaveSettings();
    } else {
        for (int i = 0; i < kFormats; ++i) g.settings.*kFormatSpecs[i].target = g_fmt.original[i];
    }
    ClosePanel();
}

void DrawFormat(const Palette& p) {
    Rect panel = g_fmt.panel = DrawFrame(p, S(kFmtW), S(kFmtH), "Title formatting");
    const float pad = S(16), x0 = panel.x + pad, w = panel.w - 2 * pad;
    DrawText(Font::Small,
             g_fmt.sample == kNone
                 ? "Add some music to see previews."
                 : ("Previews show: " + Utf8(FormatForTrack(L"%title%[ \x2014 %artist%]", g_fmt.sample, false))).c_str(),
             x0, panel.y + S(46), p.faint, w);
    float y = panel.y + S(68);
    for (int i = 0; i < kFormats; ++i) {
        DrawText(Font::Normal, kFormatSpecs[i].label, x0, y, p.sub, w);
        Rect field{x0, y + S(22), w - S(88), S(28)};
        DrawRulesEdit(g_fmt.fields[i], field, g_fmt.focus == i, true, nullptr, p);
        g_fmt.resets[i] = {field.R() + S(8), field.y, S(80), S(28)};
        DrawButton(g_fmt.resets[i], "Default", p, false, g_fmt.fields[i].text != kFormatSpecs[i].def);
        g_fmt.fields[i].rect = field;
        std::wstring preview =
            g_fmt.sample == kNone ? L"" : FormatForTrack(g_fmt.fields[i].text, g_fmt.sample, g_fmt.sample == g.playing);
        DrawText(Font::Small, preview.empty() ? "(empty)" : Utf8(preview), x0, y + S(54), p.faint, w);
        y += S(86);
    }
    auto help = Wrap(Font::Small,
                     "%field% inserts a field: title, artist, album artist, album, date, year, genre, tracknumber, discnumber, "
                     "length, codec, bitrate, samplerate, channels, rating, play_count, last_played, path, and playback_time / "
                     "playback_time_remaining in the status bar. [ ... ] disappears when its fields are empty. 'text' is "
                     "literal. $if(), $upper(), $num(), $pad() and friends are listed in the README.",
                     w);
    float hy = y + S(2);
    for (const std::string& line : help) {
        DrawText(Font::Small, line, x0, hy, p.sub, w);
        hy += LineHeight(Font::Small);
    }
    const float by = panel.B() - pad - S(28);
    g_fmt.cancel = {panel.R() - pad - S(90), by, S(90), S(28)};
    g_fmt.ok = {g_fmt.cancel.x - S(8) - S(90), by, S(90), S(28)};
    g_fmt.resetAll = {x0, by, S(120), S(28)};
    bool any = false;
    for (int i = 0; i < kFormats; ++i) any = any || g_fmt.fields[i].text != kFormatSpecs[i].def;
    DrawButton(g_fmt.resetAll, "All defaults", p, false, any);
    DrawButton(g_fmt.ok, "OK", p, true);
    DrawButton(g_fmt.cancel, "Cancel", p);
}

void FormatMouseDown(float x, float y) {
    if (!g_fmt.panel.In(x, y)) return;
    if (g_fmt.ok.In(x, y)) return FormatClose(true);
    if (g_fmt.cancel.In(x, y)) return FormatClose(false);
    if (g_fmt.resetAll.In(x, y)) {
        for (int i = 0; i < kFormats; ++i) {
            g_fmt.fields[i].text = kFormatSpecs[i].def;
            g_fmt.fields[i].caret = g_fmt.fields[i].text.size();
            FormatEdited(i);
        }
        return;
    }
    for (int i = 0; i < kFormats; ++i) {
        if (g_fmt.resets[i].In(x, y)) {
            g_fmt.fields[i].text = kFormatSpecs[i].def;
            FormatFocus(i);
            FormatEdited(i);
            return;
        }
        if (g_fmt.fields[i].rect.In(x, y)) return FormatFocus(i);
    }
}

void FormatKey(const SDL_KeyboardEvent& e) {
    if (e.key == SDLK_ESCAPE) return FormatClose(false);
    if (e.key == SDLK_RETURN || e.key == SDLK_KP_ENTER) return FormatClose(true);
    if (e.key == SDLK_TAB) return FormatFocus((g_fmt.focus + ((e.mod & SDL_KMOD_SHIFT) ? kFormats - 1 : 1)) % kFormats);
    if (LineFieldKey(g_fmt.fields[g_fmt.focus], e) == 1) FormatEdited(g_fmt.focus);
}

void FormatText(const std::wstring& s) {
    std::wstring in;
    for (wchar_t c : s)
        if (c >= 32) in += c;
    if (LineFieldInsert(g_fmt.fields[g_fmt.focus], in)) FormatEdited(g_fmt.focus);
}

// --- output device ------------------------------------------------------------------------------------
// Preferences > Output > Output device: where the music plays. PipeWire sinks are listed as the server
// reports them; "System default" follows the choice the desktop makes. The windows of the headphone page
// on Windows (headset hardware controls, per-output sound profiles) have no counterpart here.
constexpr int kDeviceRows = 9;
constexpr float kDeviceW = 560, kDeviceH = 470;

struct DevicePanel {
    std::vector<OutputDevice> devices;
    int first = 0;  // the first row shown
    Rect panel, list, refresh, close;
} g_dev;

// Row 0 is the system default, then each sink.
std::string DeviceNodeOf(int row) { return row == 0 ? std::string() : g_dev.devices[row - 1].node; }

void ChooseDevice(int row) {
    g.settings.outputDevice = DeviceNodeOf(row);
    g.engine.SetOutputDevice(g.settings.outputDevice);
    SaveSettings();
    g.redraw = true;
}

void DrawDevice(const Palette& p) {
    Rect panel = g_dev.panel = DrawFrame(p, S(kDeviceW), S(kDeviceH), "Output device");
    const float pad = S(16), x0 = panel.x + pad, w = panel.w - 2 * pad, rowH = S(30);
    DrawText(Font::Normal, "Play the music on:", x0, panel.y + S(50), p.sub, w);
    g_dev.list = {x0, panel.y + S(78), w, rowH * kDeviceRows};
    Fill(g_dev.list, p.surface2);
    Frame(g_dev.list, p.border);
    const int rows = (int)g_dev.devices.size() + 1;
    for (int i = 0; i < kDeviceRows && g_dev.first + i < rows; ++i) {
        const int row = g_dev.first + i;
        Rect r{g_dev.list.x + 1, g_dev.list.y + 1 + i * rowH, g_dev.list.w - 2, rowH};
        const bool chosen = g.settings.outputDevice == DeviceNodeOf(row);
        if (r.In(g_mx, g_my)) Fill(r, p.hover);
        DrawCheck({r.x + S(8), r.y, r.w - S(16), r.h}, row == 0 ? "System default" : g_dev.devices[row - 1].description, chosen, p);
    }
    const bool missing = !g.settings.outputDevice.empty() &&
                         std::none_of(g_dev.devices.begin(), g_dev.devices.end(), [](const OutputDevice& d) { return d.node == g.settings.outputDevice; });
    std::string note = g_dev.devices.empty() ? "No output devices were found (is PipeWire running?)."
                       : missing             ? "The chosen device (" + g.settings.outputDevice + ") isn't connected; the default plays instead."
                                             : "Exclusive output uses the chosen device too.";
    DrawText(Font::Small, note, x0, g_dev.list.B() + S(10), p.faint, w);
    const float by = panel.B() - pad - S(28);
    g_dev.close = {panel.R() - pad - S(90), by, S(90), S(28)};
    g_dev.refresh = {x0, by, S(100), S(28)};
    DrawButton(g_dev.refresh, "Refresh", p);
    DrawButton(g_dev.close, "Close", p, true);
}

void DeviceMouseDown(float x, float y) {
    if (!g_dev.panel.In(x, y) || g_dev.close.In(x, y)) return ClosePanel();
    if (g_dev.refresh.In(x, y)) {
        g_dev.devices = ListOutputDevices();
        g_dev.first = 0;
        return;
    }
    if (!g_dev.list.In(x, y)) return;
    const int row = g_dev.first + (int)((y - g_dev.list.y - 1) / S(30));
    if (row >= 0 && row <= (int)g_dev.devices.size()) ChooseDevice(row);
}

void DeviceKey(const SDL_KeyboardEvent& e) {
    if (e.key == SDLK_ESCAPE || e.key == SDLK_RETURN || e.key == SDLK_KP_ENTER) return ClosePanel();
    if (e.key == SDLK_F5) {
        g_dev.devices = ListOutputDevices();
        return;
    }
    if (e.key != SDLK_UP && e.key != SDLK_DOWN) return;
    int current = 0;
    for (int i = 0; i < (int)g_dev.devices.size(); ++i)
        if (g_dev.devices[i].node == g.settings.outputDevice) current = i + 1;
    const int next = std::clamp(current + (e.key == SDLK_UP ? -1 : 1), 0, (int)g_dev.devices.size());
    ChooseDevice(next);
    if (next < g_dev.first) g_dev.first = next;
    else if (next >= g_dev.first + kDeviceRows) g_dev.first = next - kDeviceRows + 1;
}

// --- cordinal points and the year in review -------------------------------------------------------------
// The Points window (View > Games and stats > cordinal points, or a click on the chip) and Your year in music
// (app_points.cpp, app_review.cpp): the rules and the ledger are points.cpp and playlog.cpp; these only show
// them.
struct PointsPanel {
    bool rules = false;  // showing "How points work"
    Rect panel, rulesLink, chart, close;
    int hoverDay = -1;   // index into the chart's days
} g_points;

std::string AgoText(uint32_t t, uint32_t now) {
    uint32_t s = now > t ? now - t : 0;
    if (s < 60) return "just now";
    if (s < 3600) return std::to_string(s / 60) + " min ago";
    if (s < 86400) return std::to_string(s / 3600) + " h ago";
    uint32_t days = s / 86400;
    return std::to_string(days) + (days == 1 ? " day ago" : " days ago");
}

// "Mar 4" for a local day number (days since 1970).
std::string DayText(int64_t day, const char* format = "%b %e") {
    time_t noon = (time_t)(day * 86400 + 43200);
    tm local{};
    gmtime_r(&noon, &local);  // the day is already local: its calendar date is read as is
    char buf[64];
    strftime(buf, sizeof(buf), format, &local);
    return buf;
}

// One recent-event line: the kind, plus the track (or album for album events) it was about.
std::wstring PointsEventText(const PointsEvent& e) {
    std::wstring what = PointsKindName(e.kind);
    if (e.id != UINT32_MAX && e.id < g_lib.Count()) {
        const Track& t = g_lib[e.id];
        std::wstring name = t.title ? g_lib.Str(t.title) : g_lib.Str(t.file);
        if (e.kind == PointsKind::AlbumRun || e.kind == PointsKind::FullAlbum || e.kind == PointsKind::NewAlbum)
            name = t.album ? g_lib.Str(t.album) : name;
        if (!name.empty()) what += L"  \x00B7  " + name;
    }
    return what;
}

// The "How points work" text; keep it in sync with the rules in points.cpp and app_points.cpp.
const char* const kPointsRules =
    "Listening: 2 points for every minute you actually listen (seeking doesn't count).\n"
    "Plays: 5 points for a play, more each time you come back to a song: the 16th play of a song is worth 21, "
    "the 100th 31. After ten plays of one song in a day, its play points are halved for the rest of the day.\n"
    "Discovery: +10 the first time a song counts as played.  Rediscovery: +15 for a song you hadn't played in a year.\n"
    "Milestones: +20 at a song's 10th play, +50 at 25, +100 at 50, +250 at 100, +500 at 250, +1,000 at 500, "
    "+2,500 at every 1,000.\n"
    "Album runs: four or more songs of one album in order, +30 and +5 per song after that; a whole album +50 more.\n"
    "Collecting: +2 for each song added to the library, +20 for each album new to it.\n"
    "Streak: a day with 10 minutes of listening keeps the streak going; each day adds 5% to listening and play "
    "points, up to +50%.\n\n"
    "Points are never taken away. Skips still earn their listening minutes. The games don't earn points.";

void FillBar(Rect r, uint32_t color) {
    if (r.w <= 0 || r.h <= 0) return;
    Fill(r, color);
}

void DrawPointsPanel(const Palette& p) {
    Rect panel = g_points.panel = DrawFrame(p, S(520), S(640), "cordinal points");
    const float pad = S(24), x0 = panel.x + pad, w = panel.w - 2 * pad;
    const uint32_t now = UnixNow();
    PointsLevelInfo l = CurrentPointsLevel();
    float y = panel.y + S(52);
    // Hero: the total, then the rank.
    std::string total = Utf8(GroupedNumber(l.total));
    float tw = DrawText(Font::Title, total, x0, y, p.text);
    DrawText(Font::Normal, "points", x0 + tw + S(8), y + S(6), p.sub);
    DrawText(Font::Bold, Utf8(PointsRank(l.level)), x0, y, p.accent, w, 2);
    DrawText(Font::Normal, "Level " + Utf8(GroupedNumber(l.level)), x0, y + S(22), p.sub, w, 2);
    y += S(52);
    // Progress to the next level.
    Rect track{x0, y, w, S(8)};
    Fill(track, p.surface2);
    double frac = l.next > l.start ? (double)(l.total - l.start) / (double)(l.next - l.start) : 0;
    FillBar({track.x, track.y, std::floor(track.w * (float)std::clamp(frac, 0.0, 1.0)), track.h}, p.accent);
    y += S(14);
    int nextRank = PointsNextRankLevel(l.level);
    DrawText(Font::Normal, Utf8(GroupedNumber(l.next - l.total) + L" to level " + GroupedNumber(l.level + 1)), x0, y, p.sub, w);
    DrawText(Font::Normal, nextRank ? Utf8(std::wstring(PointsRank(nextRank)) + L" at level " + std::to_wstring(nextRank)) : "The highest rank",
             x0, y, p.sub, w, 2);
    y += S(30);
    // Streak.
    int streak = PointsStreak(now);
    char streakText[160];
    if (streak > 0) snprintf(streakText, sizeof(streakText), "%d-day streak  \xC2\xB7  listening and plays earn \xC3\x97%.2f", streak, PointsMultiplier(now));
    else snprintf(streakText, sizeof(streakText), "No streak yet: 10 minutes of music a day starts one.");
    DrawText(Font::Bold, streakText, x0, y, p.text, w);
    y += S(34);

    const float bottom = panel.B() - S(46);
    if (g_points.rules) {
        DrawText(Font::Title, "How points work", x0, y, p.text);
        y += S(34);
        for (const std::string& line : Wrap(Font::Normal, kPointsRules, w)) {
            if (y + LineHeight(Font::Normal) > bottom) break;
            DrawText(Font::Normal, line, x0, y, p.text, w);
            y += LineHeight(Font::Normal);
        }
    } else {
        // The last 30 days, one bar per day; the best day is labeled, the rest on hover.
        DrawText(Font::Bold, "Last 30 days", x0, y, p.text);
        y += S(26);
        const int64_t today = LocalDay(now);
        uint64_t days[30] = {};
        for (auto& [day, centi] : PointsByDay())
            if ((int64_t)day <= today && today - (int64_t)day < 30) days[29 - (today - (int64_t)day)] = centi / 100;
        uint64_t maxv = std::max<uint64_t>(1, *std::max_element(std::begin(days), std::end(days)));
        const float chartH = S(90);
        g_points.chart = {x0, y, w, chartH};
        const float slot = std::floor(w / 30);
        int best = (int)(std::max_element(std::begin(days), std::end(days)) - days);
        Fill({x0, y + chartH, slot * 30, 1}, p.border);  // baseline
        for (int i = 0; i < 30; ++i) {
            float h = std::floor(chartH * (float)days[i] / (float)maxv);
            if (days[i] && h < S(2)) h = S(2);
            FillBar({x0 + i * slot + S(1), y + chartH - h, slot - S(2), h}, i == g_points.hoverDay ? p.accent2 : p.accent);
        }
        int label = g_points.hoverDay >= 0 ? g_points.hoverDay : days[best] ? best : -1;
        if (label >= 0) {
            std::string t = DayText(today - (29 - label)) + ": " + Utf8(GroupedNumber(days[label]));
            float lw = TextWidth(Font::Normal, t), cx = x0 + label * slot + slot / 2;
            DrawText(Font::Normal, t, std::clamp(cx - lw / 2, x0, x0 + w - lw), y + chartH + S(4), p.text);
        }
        y += chartH + S(34);
        DrawText(Font::Bold, "Recent", x0, y, p.text);
        y += S(26);
        const auto& recent = PointsRecent();
        const float rowH = S(22);
        for (auto it = recent.rbegin(); it != recent.rend() && y + rowH < bottom; ++it, y += rowH) {
            Rect row{x0, y, w, rowH};
            DrawText(Font::Bold, "+" + Utf8(GroupedNumber(std::max<uint64_t>(1, it->centi / 100))), x0, TextY(row, Font::Bold), p.accent, S(56), 2);
            std::string when = AgoText(it->time, now);
            float ww = TextWidth(Font::Normal, when);
            DrawText(Font::Normal, Utf8(PointsEventText(*it)), x0 + S(68), TextY(row), p.text, w - S(68) - ww - S(12));
            DrawText(Font::Normal, when, x0, TextY(row), p.sub, w, 2);
        }
        if (recent.empty()) DrawText(Font::Normal, "Play something: points arrive when a song ends.", x0, y, p.sub, w);
    }
    const float by = panel.B() - S(16) - S(28);
    std::string link = g_points.rules ? "\xE2\x86\x90 Back" : "How points work";
    g_points.rulesLink = {x0, by, TextWidth(Font::Normal, link) + S(8), S(28)};
    DrawText(Font::Normal, link, x0, TextY(g_points.rulesLink), g_points.rulesLink.In(g_mx, g_my) ? p.accent2 : p.accent);
    g_points.close = {panel.R() - S(16) - S(90), by, S(90), S(28)};
    DrawButton(g_points.close, "Close", p, true);
}

void PointsMouseDown(float x, float y) {
    if (!g_points.panel.In(x, y) || g_points.close.In(x, y)) return ClosePanel();
    if (g_points.rulesLink.In(x, y)) g_points.rules = !g_points.rules;
}

void PointsMouseMove(float x, float y) {
    int hover = -1;
    if (!g_points.rules && g_points.chart.In(x, y))
        hover = std::clamp((int)((x - g_points.chart.x) * 30 / std::max(1.0f, g_points.chart.w)), 0, 29);
    g_points.hoverDay = hover;
}

// Your year in music: totals, favorites and listening habits from the play log, for one calendar year or
// all time, plus what you played on this day in earlier years.
struct ReviewPanel {
    int year = 0;  // 0 = all time
    PeriodStats stats;
    std::vector<TrackId> onThisDay;
    Rect panel, prev, next, allTime, close;
    std::vector<std::pair<Rect, std::string>> tips;  // hover readouts for the habit bars
    int hoverTip = -1;
} g_review;

int CurrentYear() {
    time_t now = time(nullptr);
    tm local{};
    localtime_r(&now, &local);
    return local.tm_year + 1900;
}

// Unix time of local midnight on January 1st of `year`.
uint32_t YearStart(int year) {
    tm local{};
    local.tm_year = year - 1900;
    local.tm_mday = 1;
    local.tm_isdst = -1;
    time_t t = mktime(&local);
    return t > 0 ? (uint32_t)t : 0;
}

// The year of the oldest play-log entry (the log is chronological).
int FirstYear() {
    const auto& log = PlayLog();
    if (log.empty()) return CurrentYear();
    time_t t = log.front().time;
    tm local{};
    localtime_r(&t, &local);
    return local.tm_year + 1900;
}

void ReviewRecompute() {
    uint32_t from = g_review.year ? YearStart(g_review.year) : 0, to = g_review.year ? YearStart(g_review.year + 1) : UINT32_MAX;
    g_review.stats = ComputeStats(from, to, 5);
    g_review.onThisDay = PlayedOnThisDay(UnixNow());
    if (g_review.onThisDay.size() > 5) g_review.onThisDay.resize(5);
}

std::string HoursText(uint64_t seconds) {
    uint64_t minutes = seconds / 60;
    if (minutes < 120) return Utf8(GroupedNumber(minutes)) + " min";
    return Utf8(GroupedNumber(minutes / 60)) + " h " + std::to_string(minutes % 60) + " min";
}

std::string ReviewTrackName(TrackId id) {
    if (id >= g_lib.Count()) return {};
    const Track& t = g_lib[id];
    std::wstring s = t.title ? g_lib.Str(t.title) : g_lib.Str(t.file);
    StringPool::Handle artist = t.artist ? t.artist : t.albumArtist;
    if (artist) s += L" \x2014 " + g_lib.Str(artist);
    return Utf8(s);
}

// A ranked list: name, then a thin bar for plays with the count after it.
float ReviewTop(float x, float y, float width, const char* heading, const std::vector<TopEntry>& entries, bool tracks, const Palette& p) {
    DrawText(Font::Bold, heading, x, y, p.text);
    y += S(24);
    if (entries.empty()) {
        DrawText(Font::Normal, "Nothing yet", x, y, p.sub);
        return y + S(28);
    }
    uint32_t maxPlays = std::max<uint32_t>(1, entries[0].plays);
    for (size_t i = 0; i < entries.size(); ++i) {
        const TopEntry& e = entries[i];
        std::string name = tracks ? ReviewTrackName(e.track) : Utf8(g_lib.Str(e.key));
        if (name.empty()) name = "(unknown)";
        DrawText(Font::Normal, std::to_string(i + 1), x, y, p.sub);
        DrawText(Font::Normal, name, x + S(18), y, p.text, width - S(18));
        std::string count = Utf8(GroupedNumber(e.plays));
        float cw = TextWidth(Font::Normal, count), barMax = width - S(18) - cw - S(8);
        float bw = std::max(S(3), std::floor(barMax * e.plays / maxPlays));
        Fill({x + S(18), y + S(21), bw, S(4)}, p.accent);
        DrawText(Font::Normal, count, x + S(18) + bw + S(6), y + S(15), p.sub);
        y += S(36);
    }
    return y + S(6);
}

// Vertical bars for a habit (hours or weekdays): magnitude in one hue, the peak labeled.
float ReviewHabit(float x, float y, float width, const char* heading, const uint32_t* values, int count, const char* const* labels,
                  int labelEvery, const Palette& p) {
    DrawText(Font::Bold, heading, x, y, p.text);
    y += S(26);
    const float h = S(64), slot = std::floor(width / count);
    uint32_t maxv = std::max<uint32_t>(1, *std::max_element(values, values + count));
    int peak = (int)(std::max_element(values, values + count) - values);
    Fill({x, y + h, slot * count, 1}, p.border);
    for (int i = 0; i < count; ++i) {
        float bh = std::floor(h * values[i] / maxv);
        if (values[i] && bh < S(2)) bh = S(2);
        int tipIndex = (int)g_review.tips.size();
        Fill({x + i * slot + S(1), y + h - bh, slot - S(2), bh}, tipIndex == g_review.hoverTip ? p.accent2 : p.accent);
        g_review.tips.push_back({{x + i * slot, y, slot, h}, std::string(labels[i]) + ": " + Utf8(GroupedNumber(values[i])) + " plays"});
        if (i % labelEvery == 0) DrawText(Font::Normal, labels[i], x + i * slot - S(10), y + h + S(3), p.sub, slot + S(20), 1);
    }
    if (values[peak] && g_review.hoverTip < 0)
        DrawText(Font::Normal, std::string("Most: ") + labels[peak], x, y - S(22), p.sub, slot * count, 2);
    return y + h + S(30);
}

void DrawReview(const Palette& p) {
    Rect panel = g_review.panel = DrawFrame(p, S(820), S(760), "Year in review");
    g_review.tips.clear();
    const float pad = S(24), x0 = panel.x + pad, w = panel.w - 2 * pad;
    const PeriodStats& s = g_review.stats;
    float y = panel.y + S(52);
    // The period, with links to step through the years.
    DrawText(Font::Title, (g_review.year ? std::to_string(g_review.year) : std::string("All time")) + " in music", x0, y, p.text);
    float ax = x0 + w;
    auto link = [&](const char* text, Rect& r, bool enabled) {
        float lw = TextWidth(Font::Normal, text);
        r = enabled ? Rect{ax - lw, y + S(4), lw, S(22)} : Rect{};
        DrawText(Font::Normal, text, ax - lw, y + S(4), enabled ? (r.In(g_mx, g_my) ? p.accent2 : p.accent) : p.faint);
        ax -= lw + S(16);
    };
    link("All time", g_review.allTime, g_review.year != 0);
    link("Next \xE2\x80\xBA", g_review.next, g_review.year && g_review.year < CurrentYear());
    link("\xE2\x80\xB9 Previous", g_review.prev, g_review.year == 0 || g_review.year > FirstYear());
    y += S(46);
    const float by = panel.B() - S(16) - S(28);
    g_review.close = {panel.R() - S(16) - S(90), by, S(90), S(28)};
    DrawButton(g_review.close, "Close", p, true);

    if (PlayLog().empty()) {
        for (const std::string& line : Wrap(Font::Normal,
                                            "cordinal keeps a play log from now on. Come back after some listening: every song you finish or "
                                            "skip ends up here.",
                                            w)) {
            DrawText(Font::Normal, line, x0, y, p.sub, w);
            y += LineHeight(Font::Normal);
        }
        return;
    }
    // Four stat tiles.
    struct Tile { std::string value, label; } tiles[] = {
        {HoursText(s.seconds), "listened"},
        {Utf8(GroupedNumber(s.plays)), s.plays == 1 ? "play" : "plays"},
        {Utf8(GroupedNumber(s.distinctTracks)), s.distinctTracks == 1 ? "song" : "different songs"},
        {Utf8(GroupedNumber(s.discoveries)), "heard for the first time"},
    };
    const float tileW = std::floor((w - S(12) * 3) / 4);
    for (int i = 0; i < 4; ++i) {
        Rect tile{x0 + i * (tileW + S(12)), y, tileW, S(74)};
        Fill(tile, p.surface2);
        DrawText(Font::Title, tiles[i].value, tile.x + S(12), tile.y + S(10), p.text, tile.w - S(20));
        DrawText(Font::Normal, tiles[i].label, tile.x + S(12), tile.y + S(46), p.sub, tile.w - S(20));
    }
    y += S(98);
    // Left: favorites. Right: habits and facts.
    const float colW = std::floor((w - pad) / 2), lx = x0, rx = x0 + colW + pad;
    float ly = ReviewTop(lx, y, colW, "Top songs", s.topTracks, true, p);
    ly = ReviewTop(lx, ly, colW, "Top artists", s.topArtists, false, p);
    ReviewTop(lx, ly, colW, "Top albums", s.topAlbums, false, p);
    static const char* const hours[24] = {"12 am", "1 am", "2 am", "3 am", "4 am", "5 am", "6 am", "7 am", "8 am", "9 am", "10 am", "11 am",
                                          "12 pm", "1 pm", "2 pm", "3 pm", "4 pm", "5 pm", "6 pm", "7 pm", "8 pm", "9 pm", "10 pm", "11 pm"};
    static const char* const days[7] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    float ry = ReviewHabit(rx, y, colW, "Time of day", s.byHour, 24, hours, 6, p);
    ry = ReviewHabit(rx, ry, colW, "Day of the week", s.byWeekday, 7, days, 1, p);
    if (s.longestStreak)
        DrawText(Font::Normal, "Longest streak: " + Utf8(GroupedNumber(s.longestStreak)) + (s.longestStreak == 1 ? " day" : " days in a row"), rx, ry, p.text, colW);
    ry += S(22);
    if (s.busiestDaySeconds)
        DrawText(Font::Normal, "Busiest day: " + DayText(s.busiestDay, "%A, %B %e, %Y") + " (" + HoursText(s.busiestDaySeconds) + ")", rx, ry, p.text, colW);
    ry += S(36);
    if (!g_review.onThisDay.empty()) {
        DrawText(Font::Bold, "On this day, in earlier years", rx, ry, p.text);
        ry += S(24);
        for (TrackId id : g_review.onThisDay) {
            DrawText(Font::Normal, ReviewTrackName(id), rx, ry, p.text, colW);
            ry += S(20);
        }
    }
    if (g_review.hoverTip >= 0 && g_review.hoverTip < (int)g_review.tips.size()) {
        auto& [r, t] = g_review.tips[g_review.hoverTip];
        float tw = TextWidth(Font::Normal, t) + S(12);
        float x = std::clamp(r.x + r.w / 2 - tw / 2, x0, x0 + w - tw);
        Rect box{x, r.y - S(26), tw, S(22)};
        Fill(box, p.text);
        DrawText(Font::Normal, t, box.x, TextY(box), p.bg, box.w, 1);
    }
}

void ReviewMouseDown(float x, float y) {
    if (!g_review.panel.In(x, y) || g_review.close.In(x, y)) return ClosePanel();
    int year = g_review.year;
    if (g_review.prev.w > 0 && g_review.prev.In(x, y)) year = g_review.year ? g_review.year - 1 : CurrentYear();
    else if (g_review.next.w > 0 && g_review.next.In(x, y)) year = g_review.year + 1;
    else if (g_review.allTime.w > 0 && g_review.allTime.In(x, y)) year = 0;
    if (year != g_review.year) {
        g_review.year = year;
        ReviewRecompute();
    }
}

void ReviewMouseMove(float x, float y) {
    g_review.hoverTip = -1;
    for (size_t i = 0; i < g_review.tips.size(); ++i)
        if (g_review.tips[i].first.In(x, y)) g_review.hoverTip = (int)i;
}

void ReviewKey(const SDL_KeyboardEvent& e) {
    if (e.key == SDLK_ESCAPE) return ClosePanel();
    int year = g_review.year;
    if (e.key == SDLK_LEFT) year = g_review.year ? (g_review.year > FirstYear() ? g_review.year - 1 : g_review.year) : CurrentYear();
    else if (e.key == SDLK_RIGHT) year = g_review.year && g_review.year < CurrentYear() ? g_review.year + 1 : g_review.year;
    else if (e.key == SDLK_HOME) year = 0;
    if (year != g_review.year) {
        g_review.year = year;
        ReviewRecompute();
    }
}

// --- command palette -----------------------------------------------------------------------------------
// View > Command palette (Ctrl+Shift+P), as app_palette.cpp: one search over every command of the menu bar,
// with its checkmark, shortcut and where it lives, and over the settings of Preferences (a switch flips, the
// rest open Preferences at that setting). Enter or a click runs the command once the palette has closed.
struct PaletteEntry {
    PaletteCommand command;
    int menu = -1;  // an index for RunMenuCommand, or
    int pref = -1;  // an index into kPrefs
    std::wstring searchable;
};
struct PalettePanel {
    TextField query;
    std::vector<PaletteEntry> entries;
    std::vector<int> shown;  // entries matching the query, best first
    int selected = 0, first = 0;
    Rect panel, field, list;
} g_palette;
constexpr int kPaletteRows = 11;

void PaletteFilter() {
    std::wstring q = Lower(g_palette.query.text);
    std::vector<std::wstring> words;
    for (size_t i = 0; i < q.size();) {
        size_t end = q.find(L' ', i);
        if (end == std::wstring::npos) end = q.size();
        if (end > i) words.push_back(q.substr(i, end - i));
        i = end + 1;
    }
    std::vector<std::pair<int, int>> scored;  // rank, entry
    for (int i = 0; i < (int)g_palette.entries.size(); ++i) {
        const PaletteEntry& e = g_palette.entries[i];
        int rank = 0;
        bool all = true;
        std::wstring label = Lower(compat::Widen(e.command.label.c_str()));
        for (const std::wstring& w : words) {
            if (e.searchable.find(w) == std::wstring::npos) {
                all = false;
                break;
            }
            rank += label.starts_with(w) ? 0 : label.find(w) != std::wstring::npos ? 1 : 2;
        }
        if (all) scored.push_back({rank, i});
    }
    std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    g_palette.shown.clear();
    for (auto& s : scored) g_palette.shown.push_back(s.second);
    g_palette.selected = 0;
    g_palette.first = 0;
}

void PaletteRun(int row) {
    if (row < 0 || row >= (int)g_palette.shown.size()) return;
    const PaletteEntry e = g_palette.entries[g_palette.shown[row]];
    if (!e.command.enabled) return;
    ClosePanel();
    if (e.menu >= 0) {
        RunMenuCommand((size_t)e.menu);
    } else if (e.pref >= 0) {
        const Pref& t = kPrefs[e.pref];
        if (t.type == PrefType::Bool) {
            SetPref(t, t.get() != 0 ? 0 : 1);
            ShowMessage(compat::Widen(t.label) + (t.get() != 0 ? L": on." : L": off."));
        } else {
            OpenPreferences();
            g_prefs.search = compat::Widen(t.label);
        }
    }
}

void DrawPalette(const Palette& p) {
    Rect panel = g_palette.panel = DrawFrame(p, S(640), S(470), "Command palette");
    const float pad = S(16), x0 = panel.x + pad, w = panel.w - 2 * pad, rowH = S(30);
    g_palette.field = {x0, panel.y + S(48), w, S(30)};
    DrawRulesEdit(g_palette.query, g_palette.field, true, true, "Type a command or a setting", p);
    g_palette.list = {x0, g_palette.field.B() + S(8), w, rowH * kPaletteRows};
    for (int i = 0; i < kPaletteRows && g_palette.first + i < (int)g_palette.shown.size(); ++i) {
        const int row = g_palette.first + i;
        const PaletteEntry& e = g_palette.entries[g_palette.shown[row]];
        Rect r{g_palette.list.x, g_palette.list.y + i * rowH, g_palette.list.w, rowH};
        const bool selected = row == g_palette.selected;
        if (selected) Fill(r, p.select);
        else if (r.In(g_mx, g_my)) Fill(r, p.hover);
        const uint32_t main = !e.command.enabled ? p.faint : selected ? p.selectText : p.text;
        const uint32_t sub = !e.command.enabled ? p.faint : selected ? p.selectText : p.sub;
        std::string label = (e.command.checked ? "\xE2\x9C\x93 " : "") + e.command.label;
        const float shortcutW = e.command.shortcut.empty() ? 0 : TextWidth(Font::Normal, e.command.shortcut) + S(12);
        const float pathW = std::min(TextWidth(Font::Small, e.command.path) + S(12), r.w * 0.4f);
        DrawText(Font::Normal, label, r.x + S(8), TextY(r), main, r.w - shortcutW - pathW - S(16));
        DrawText(Font::Small, e.command.path, r.R() - shortcutW - pathW, TextY(r, Font::Small), sub, pathW - S(8), 2);
        if (shortcutW > 0) DrawText(Font::Normal, e.command.shortcut, r.R() - shortcutW, TextY(r), sub, shortcutW - S(8), 2);
    }
    if (g_palette.shown.empty()) DrawText(Font::Normal, "Nothing matches.", g_palette.list.x + S(8), g_palette.list.y + S(6), p.sub);
}

void PaletteMouseDown(float x, float y) {
    if (!g_palette.panel.In(x, y)) return ClosePanel();
    if (g_palette.list.In(x, y)) PaletteRun(g_palette.first + (int)((y - g_palette.list.y) / S(30)));
}

void PaletteKey(const SDL_KeyboardEvent& e) {
    const int count = (int)g_palette.shown.size();
    auto move = [&](int to) {
        g_palette.selected = std::clamp(to, 0, std::max(0, count - 1));
        if (g_palette.selected < g_palette.first) g_palette.first = g_palette.selected;
        else if (g_palette.selected >= g_palette.first + kPaletteRows) g_palette.first = g_palette.selected - kPaletteRows + 1;
    };
    switch (e.key) {
        case SDLK_ESCAPE: return ClosePanel();
        case SDLK_RETURN: case SDLK_KP_ENTER: return PaletteRun(g_palette.selected);
        case SDLK_UP: return move(g_palette.selected - 1);
        case SDLK_DOWN: return move(g_palette.selected + 1);
        case SDLK_PAGEUP: return move(g_palette.selected - kPaletteRows);
        case SDLK_PAGEDOWN: return move(g_palette.selected + kPaletteRows);
        default: break;
    }
    if (LineFieldKey(g_palette.query, e) == 1) PaletteFilter();
}

void PaletteText(const std::wstring& s) {
    std::wstring in;
    for (wchar_t c : s)
        if (c >= 32) in += c;
    if (LineFieldInsert(g_palette.query, in)) PaletteFilter();
}

// --- customize the layout --------------------------------------------------------------------------------
// View > Customize layout: which parts of the main window show (app_custom.cpp's parts). A hidden part gives its
// room back to the rest; the track list and the menu bar always stay. Choices save as they are made.
struct LayoutPart {
    unsigned bit;
    const char* label;
};
const LayoutPart kLayoutParts[] = {
    {kPartTransport, "Previous, play and next"}, {kPartOrder, "Shuffle and repeat"}, {kPartWave, "Waveform seek bar"},
    {kPartTimes, "Times"}, {kPartVolume, "Volume"}, {kPartNowPlaying, "Now playing (cover, title and artist)"},
    {kPartTabs, "Playlist tabs"}, {kPartSearch, "Search box"}, {kPartViews, "Library view buttons"}, {kPartStatus, "Status bar"},
};
struct LayoutPanel {
    Rect panel, rows[std::size(kLayoutParts) + 1], reset, close;
} g_layout;

bool PartShown(unsigned bit) { return !(g.settings.hiddenParts & bit); }

void SetPart(unsigned bit, bool shown) {
    if (shown) g.settings.hiddenParts &= ~bit;
    else g.settings.hiddenParts |= bit;
    SaveSettings();
    g.redraw = true;
}

void DrawLayoutPanel(const Palette& p) {
    Rect panel = g_layout.panel = DrawFrame(p, S(460), S(520), "Customize layout");
    const float pad = S(20), x0 = panel.x + pad, w = panel.w - 2 * pad, rowH = S(30);
    DrawText(Font::Normal, "Show these parts of the window:", x0, panel.y + S(48), p.sub, w);
    float y = panel.y + S(76);
    for (size_t i = 0; i < std::size(kLayoutParts); ++i, y += rowH) {
        g_layout.rows[i] = {x0, y, w, rowH};
        DrawCheck(g_layout.rows[i], kLayoutParts[i].label, PartShown(kLayoutParts[i].bit), p);
    }
    g_layout.rows[std::size(kLayoutParts)] = {x0, y, w, rowH};
    DrawCheck(g_layout.rows[std::size(kLayoutParts)], "Level and points", g.settings.pointsChip, p);
    const float by = panel.B() - pad - S(28);
    g_layout.close = {panel.R() - pad - S(90), by, S(90), S(28)};
    g_layout.reset = {x0, by, S(160), S(28)};
    DrawButton(g_layout.reset, "Reset the layout", p, false, g.settings.hiddenParts != 0 || g.settings.pointsChip);
    DrawButton(g_layout.close, "Close", p, true);
}

void LayoutMouseDown(float x, float y) {
    if (!g_layout.panel.In(x, y) || g_layout.close.In(x, y)) return ClosePanel();
    if (g_layout.reset.In(x, y)) {
        g.settings.hiddenParts = 0;
        g.settings.pointsChip = false;
        SaveSettings();
        return;
    }
    for (size_t i = 0; i < std::size(kLayoutParts); ++i)
        if (g_layout.rows[i].In(x, y)) return SetPart(kLayoutParts[i].bit, !PartShown(kLayoutParts[i].bit));
    if (g_layout.rows[std::size(kLayoutParts)].In(x, y)) {
        g.settings.pointsChip = !g.settings.pointsChip;
        SaveSettings();
    }
}

void LayoutKey(const SDL_KeyboardEvent& e) {
    if (e.key == SDLK_ESCAPE || e.key == SDLK_RETURN || e.key == SDLK_KP_ENTER) ClosePanel();
}

// --- packs ---------------------------------------------------------------------------------------------------
// View > Packs: the packs in the data folder, each with a switch, what it holds and what was left out of it.
constexpr int kPackRows = 8;
struct PacksPanel {
    int selected = 0, first = 0;
    Rect panel, list, install, installFolder, folder, close;
} g_packsUi;

void DrawPacks(const Palette& p) {
    Rect panel = g_packsUi.panel = DrawFrame(p, S(640), S(540), "Packs");
    const float pad = S(16), x0 = panel.x + pad, w = panel.w - 2 * pad, rowH = S(30);
    const size_t count = PackCount();
    DrawText(Font::Normal, count ? "A pack adds formats and title functions. Turn one on to use it."
                                 : "No packs yet. Install a .cordpack file, or a folder with a pack.ini in it.",
             x0, panel.y + S(48), p.sub, w);
    g_packsUi.list = {x0, panel.y + S(76), w, rowH * kPackRows};
    Fill(g_packsUi.list, p.surface2);
    Frame(g_packsUi.list, p.border);
    for (int i = 0; i < kPackRows && g_packsUi.first + i < (int)count; ++i) {
        const int row = g_packsUi.first + i;
        PackRow pack = PackAt(row);
        Rect r{g_packsUi.list.x + 1, g_packsUi.list.y + 1 + i * rowH, g_packsUi.list.w - 2, rowH};
        if (row == g_packsUi.selected) Fill(r, p.select);
        else if (r.In(g_mx, g_my)) Fill(r, p.hover);
        std::string label = Utf8(pack.name) + (pack.version.empty() ? "" : "  " + Utf8(pack.version));
        DrawCheck({r.x + S(8), r.y, r.w - S(16), r.h}, label, pack.on, p);
    }
    float y = g_packsUi.list.B() + S(10);
    const float limit = panel.B() - pad - S(40);
    if (g_packsUi.selected >= 0 && g_packsUi.selected < (int)count) {
        PackRow pack = PackAt(g_packsUi.selected);
        std::string text = pack.error.empty() ? Utf8(pack.summary) : "Can't be used: " + Utf8(pack.error);
        if (!pack.author.empty()) text = "By " + Utf8(pack.author) + ". " + text;
        if (!pack.description.empty()) text += "  " + Utf8(pack.description);
        for (const std::wstring& warning : pack.warnings) text += "  \xE2\x80\xA2 " + Utf8(warning);
        for (const std::string& line : Wrap(Font::Small, text, w)) {
            if (y + LineHeight(Font::Small) > limit) break;
            DrawText(Font::Small, line, x0, y, p.sub, w);
            y += LineHeight(Font::Small);
        }
    }
    const float by = panel.B() - pad - S(28);
    g_packsUi.close = {panel.R() - pad - S(90), by, S(90), S(28)};
    g_packsUi.folder = {g_packsUi.close.x - S(8) - S(130), by, S(130), S(28)};
    g_packsUi.installFolder = {g_packsUi.folder.x - S(8) - S(150), by, S(150), S(28)};
    g_packsUi.install = {g_packsUi.installFolder.x - S(8) - S(120), by, S(120), S(28)};
    DrawButton(g_packsUi.install, "Install file...", p);
    DrawButton(g_packsUi.installFolder, "Install folder...", p);
    DrawButton(g_packsUi.folder, "Open the folder", p);
    DrawButton(g_packsUi.close, "Close", p, true);
}

void PacksMouseDown(float x, float y) {
    if (!g_packsUi.panel.In(x, y) || g_packsUi.close.In(x, y)) return ClosePanel();
    if (g_packsUi.install.In(x, y)) {
        static const SDL_DialogFileFilter filter{"Packs", "cordpack;zip"};
        return ChooseFile(kFilePack, filter);
    }
    if (g_packsUi.installFolder.In(x, y)) {
        auto chosen = [](void*, const char* const* list, int) {
            if (!list || !*list) return;
            auto* path = new std::wstring(compat::Widen(*list));
            if (!PostMessageW(nullptr, WM_APP_FILE, (WPARAM)kFilePack, (LPARAM)path)) delete path;
        };
        return SDL_ShowOpenFolderDialog(chosen, nullptr, g.window, nullptr, false);
    }
    if (g_packsUi.folder.In(x, y)) return OpenPacksFolder();
    if (g_packsUi.list.In(x, y)) {
        int row = g_packsUi.first + (int)((y - g_packsUi.list.y - 1) / S(30));
        if (row < 0 || row >= (int)PackCount()) return;
        g_packsUi.selected = row;
        if (x < g_packsUi.list.x + S(30)) TogglePack(row);
    }
}

void PacksKey(const SDL_KeyboardEvent& e) {
    const int count = (int)PackCount();
    if (e.key == SDLK_ESCAPE) return ClosePanel();
    if (e.key == SDLK_UP) g_packsUi.selected = std::max(0, g_packsUi.selected - 1);
    else if (e.key == SDLK_DOWN) g_packsUi.selected = std::min(count - 1, g_packsUi.selected + 1);
    else if (e.key == SDLK_SPACE || e.key == SDLK_RETURN) TogglePack(g_packsUi.selected);
    if (g_packsUi.selected < g_packsUi.first) g_packsUi.first = g_packsUi.selected;
    else if (g_packsUi.selected >= g_packsUi.first + kPackRows) g_packsUi.first = g_packsUi.selected - kPackRows + 1;
}

// --- helpers -------------------------------------------------------------------------------------------------
// View > Helpers: the helpers in the data folder, each with a switch, what it does, the programs it runs and what
// was left out. Turning one on asks first and records its files; a changed file turns it off again.
constexpr int kHelperRows = 8;
struct HelpersPanel {
    int selected = 0, first = 0;
    Rect panel, list, installFolder, folder, close;
} g_helpersUi;

void DrawHelpers(const Palette& p) {
    Rect panel = g_helpersUi.panel = DrawFrame(p, S(680), S(560), "Helpers");
    const float pad = S(16), x0 = panel.x + pad, w = panel.w - 2 * pad, rowH = S(30);
    const size_t count = HelperCount();
    std::string intro = count ? "A helper is a separate program that plays a file type, tags it or runs on tracks. Turn one on to use it."
                              : "No helpers yet. Install a folder with a helper.ini in it (see the README, \"Helpers\").";
    if (count && !HelperAt(0).offAll.empty()) intro = "Helpers are " + Utf8(HelperAt(0).offAll) + ".";
    float y = panel.y + S(48);
    for (const std::string& line : Wrap(Font::Normal, intro, w)) {
        DrawText(Font::Normal, line, x0, y, p.sub, w);
        y += LineHeight(Font::Normal);
    }
    g_helpersUi.list = {x0, panel.y + S(88), w, rowH * kHelperRows};
    Fill(g_helpersUi.list, p.surface2);
    Frame(g_helpersUi.list, p.border);
    for (int i = 0; i < kHelperRows && g_helpersUi.first + i < (int)count; ++i) {
        const int row = g_helpersUi.first + i;
        HelperRow helper = HelperAt(row);
        Rect r{g_helpersUi.list.x + 1, g_helpersUi.list.y + 1 + i * rowH, g_helpersUi.list.w - 2, rowH};
        if (row == g_helpersUi.selected) Fill(r, p.select);
        else if (r.In(g_mx, g_my)) Fill(r, p.hover);
        std::string label = Utf8(helper.name) + (helper.version.empty() ? "" : "  " + Utf8(helper.version));
        DrawCheck({r.x + S(8), r.y, r.w - S(16), r.h}, label, helper.on, p);
    }
    y = g_helpersUi.list.B() + S(10);
    const float limit = panel.B() - pad - S(40);
    if (g_helpersUi.selected >= 0 && g_helpersUi.selected < (int)count) {
        HelperRow helper = HelperAt(g_helpersUi.selected);
        std::string text = helper.error.empty() ? "It " + Utf8(helper.summary) + "." : "Can't be used: " + Utf8(helper.error);
        if (!helper.programs.empty()) text += " It runs " + Utf8(helper.programs) + ".";
        if (!helper.author.empty()) text = "By " + Utf8(helper.author) + ". " + text;
        if (!helper.description.empty()) text += "  " + Utf8(helper.description);
        if (!helper.why.empty()) text += "  It is off: " + Utf8(helper.why) + ".";
        for (const std::wstring& warning : helper.warnings) text += "  \xE2\x80\xA2 " + Utf8(warning);
        for (const std::string& line : Wrap(Font::Small, text, w)) {
            if (y + LineHeight(Font::Small) > limit) break;
            DrawText(Font::Small, line, x0, y, p.sub, w);
            y += LineHeight(Font::Small);
        }
    }
    const float by = panel.B() - pad - S(28);
    g_helpersUi.close = {panel.R() - pad - S(90), by, S(90), S(28)};
    g_helpersUi.folder = {g_helpersUi.close.x - S(8) - S(130), by, S(130), S(28)};
    g_helpersUi.installFolder = {g_helpersUi.folder.x - S(8) - S(150), by, S(150), S(28)};
    DrawButton(g_helpersUi.installFolder, "Install folder...", p);
    DrawButton(g_helpersUi.folder, "Open the folder", p);
    DrawButton(g_helpersUi.close, "Close", p, true);
}

void HelpersMouseDown(float x, float y) {
    if (!g_helpersUi.panel.In(x, y) || g_helpersUi.close.In(x, y)) return ClosePanel();
    if (g_helpersUi.installFolder.In(x, y)) {
        auto chosen = [](void*, const char* const* list, int) {
            if (!list || !*list) return;
            auto* path = new std::wstring(compat::Widen(*list));
            if (!PostMessageW(nullptr, WM_APP_FILE, (WPARAM)kFileHelper, (LPARAM)path)) delete path;
        };
        return SDL_ShowOpenFolderDialog(chosen, nullptr, g.window, nullptr, false);
    }
    if (g_helpersUi.folder.In(x, y)) return OpenHelpersFolder();
    if (g_helpersUi.list.In(x, y)) {
        int row = g_helpersUi.first + (int)((y - g_helpersUi.list.y - 1) / S(30));
        if (row < 0 || row >= (int)HelperCount()) return;
        g_helpersUi.selected = row;
        if (x < g_helpersUi.list.x + S(30)) ToggleHelper(row);
    }
}

void HelpersKey(const SDL_KeyboardEvent& e) {
    const int count = (int)HelperCount();
    if (e.key == SDLK_ESCAPE) return ClosePanel();
    if (e.key == SDLK_UP) g_helpersUi.selected = std::max(0, g_helpersUi.selected - 1);
    else if (e.key == SDLK_DOWN) g_helpersUi.selected = std::min(count - 1, g_helpersUi.selected + 1);
    else if (e.key == SDLK_SPACE || e.key == SDLK_RETURN) ToggleHelper(g_helpersUi.selected);
    if (g_helpersUi.selected < g_helpersUi.first) g_helpersUi.first = g_helpersUi.selected;
    else if (g_helpersUi.selected >= g_helpersUi.first + kHelperRows) g_helpersUi.first = g_helpersUi.selected - kHelperRows + 1;
}

// --- ListenBrainz ----------------------------------------------------------------------------------------
// Preferences > Library > ListenBrainz scrobbling: the user token, checked with the server before it is
// kept (listenbrainz.cpp), as the Windows dialog in app_platform.cpp does.
struct LbPanel {
    TextField token;
    bool checking = false;
    std::wstring pendingToken, message;
    Rect panel, field, connect, disconnect, close;
} g_lb;

constexpr float kLbW = 620, kLbH = 330;

std::wstring LbToken() {
    std::wstring t = g_lb.token.text;
    while (!t.empty() && iswspace(t.back())) t.pop_back();
    size_t a = 0;
    while (a < t.size() && iswspace(t[a])) ++a;
    return t.substr(a);
}

void DrawListenBrainz(const Palette& p) {
    Rect panel = g_lb.panel = DrawFrame(p, S(kLbW), S(kLbH), "ListenBrainz scrobbling");
    const float pad = S(16), lh = LineHeight(Font::Normal);
    auto lines = Wrap(Font::Normal,
                      "ListenBrainz keeps a public history of what you listen to. cordinal sends a song when it counts as played "
                      "(half of it, or 4 minutes). Paste your user token from listenbrainz.org/settings:",
                      panel.w - 2 * pad);
    float y = panel.y + S(52);
    for (const std::string& line : lines) {
        DrawText(Font::Normal, line, panel.x + pad, y, p.text, panel.w - 2 * pad);
        y += lh;
    }
    g_lb.field = {panel.x + pad, y + S(10), panel.w - 2 * pad, S(28)};
    DrawRulesEdit(g_lb.token, g_lb.field, true, !g_lb.checking, "Your user token", p);
    const auto status = listenbrainz::GetStatus();
    std::wstring text;
    if (g_lb.checking) text = L"Checking the token...";
    else if (!g_lb.message.empty()) text = g_lb.message;
    else if (listenbrainz::Enabled()) {
        text = status.user.empty() ? L"Scrobbling is on." : L"Scrobbling as " + status.user + L".";
        if (status.queued) text += L" " + std::to_wstring(status.queued) + L" waiting to be sent.";
        if (!status.lastError.empty()) text += L" " + status.lastError + L".";
    } else text = L"Not connected. Scrobbling is off.";
    auto statusLines = Wrap(Font::Normal, Utf8(text), panel.w - 2 * pad);
    y = g_lb.field.B() + S(14);
    for (const std::string& line : statusLines) {
        DrawText(Font::Normal, line, panel.x + pad, y, p.sub, panel.w - 2 * pad);
        y += lh;
    }
    DrawText(Font::Small, "The token is kept in a file only you can read. Songs wait in a queue while offline.", panel.x + pad,
             panel.B() - pad - S(28) - S(8) - LineHeight(Font::Small), p.faint, panel.w - 2 * pad);
    const float by = panel.B() - pad - S(28);
    g_lb.close = {panel.R() - pad - S(90), by, S(90), S(28)};
    g_lb.disconnect = {g_lb.close.x - S(8) - S(110), by, S(110), S(28)};
    g_lb.connect = {g_lb.disconnect.x - S(8) - S(110), by, S(110), S(28)};
    DrawButton(g_lb.connect, "Connect", p, true, !g_lb.checking && !LbToken().empty());
    DrawButton(g_lb.disconnect, "Disconnect", p, false, listenbrainz::Enabled());
    DrawButton(g_lb.close, "Close", p);
}

void LbConnect() {
    if (g_lb.checking || LbToken().empty()) return;
    g_lb.pendingToken = LbToken();
    g_lb.message.clear();
    g_lb.checking = true;
    listenbrainz::Validate(g_lb.pendingToken);
}

void LbMouseDown(float x, float y) {
    if (!g_lb.panel.In(x, y) || g_lb.close.In(x, y)) return ClosePanel();
    if (g_lb.connect.In(x, y)) return LbConnect();
    if (g_lb.disconnect.In(x, y) && listenbrainz::Enabled()) {
        listenbrainz::SetToken(L"");
        g_lb.message = L"Disconnected. Scrobbling is off.";
    }
}

void LbKey(const SDL_KeyboardEvent& e) {
    if (e.key == SDLK_ESCAPE) return ClosePanel();
    if (e.key == SDLK_RETURN || e.key == SDLK_KP_ENTER) return LbConnect();
    if (g_lb.checking) return;
    if ((e.mod & SDL_KMOD_CTRL) && e.key == SDLK_V) {  // a token is pasted, not typed
        if (char* clip = SDL_GetClipboardText()) {
            std::wstring in;
            for (wchar_t c : compat::Widen(clip))
                if (c > 32) in += c;
            SDL_free(clip);
            LineFieldInsert(g_lb.token, in);
        }
        return;
    }
    LineFieldKey(g_lb.token, e);
}

void LbText(const std::wstring& s) {
    std::wstring in;
    for (wchar_t c : s)
        if (c > 32) in += c;
    if (!g_lb.checking) LineFieldInsert(g_lb.token, in);
}

}  // namespace

// Playlist > New auto-playlist (playlist -1) or Edit auto-playlist rules (an auto-playlist's index).
void OpenRules(int playlist) {
    if (playlist >= (int)g.playlists.size()) return;
    g_rules = RulesPanel{};
    g_rules.playlist = playlist;
    std::wstring name = L"Auto-playlist";
    AutoPlaylist model;
    if (playlist >= 0) {
        if (!ParseAutoPlaylist(g.playlists[playlist].rules, model)) {
            ShowMessage(L"That playlist's rules can't be read.");
            return;
        }
        name = g.playlists[playlist].name;
    }
    for (const Rule& r : model.root.rules) {
        RuleRow row;
        row.rule = r;
        row.value.text = compat::Widen(r.value.c_str(), r.value.size());
        row.value2.text = compat::Widen(r.value2.c_str(), r.value2.size());
        g_rules.rows.push_back(row);
    }
    if (g_rules.rows.empty() && model.root.groups.empty()) g_rules.rows.push_back(RuleRow{});
    model.root.rules.clear();
    g_rules.model = model;
    g_rules.name.text = name;
    g_rules.limitOn = model.limit > 0;
    if (model.limit) g_rules.limit.text = std::to_wstring(model.limit);
    g_kind = Kind::Rules;
    RulesFocus(0);
    g.redraw = true;
}

// Library > Organize files: the given tracks, or the whole library when there are none.
void OpenOrganize(std::vector<TrackId> ids) {
    if (OrganizeBusy()) return;
    if (ids.empty())
        for (TrackId t = 0; t < (TrackId)g_lib.Count(); ++t)
            if (g_lib[t].InLibrary()) ids.push_back(t);
    if (ids.empty()) {
        ShowMessage(L"Nothing to organize.");
        return;
    }
    g_org = OrganizePanel{};
    g_org.ids = std::move(ids);
    g_org.root.text = !g.settings.organizeFolder.empty() ? g.settings.organizeFolder
                      : g.settings.folders.empty()       ? std::wstring()
                                                         : g.settings.folders[0];
    g_org.pattern.text = g.settings.organizePattern.empty() ? std::wstring(kDefaultOrganizePattern) : g.settings.organizePattern;
    Replan();
    g_kind = Kind::Organize;
    OrganizeFocus(0);
}

void OpenCopy(const std::vector<TrackId>& ids, const std::wstring& playlist) {
    if (ids.empty()) {
        ShowMessage(L"Nothing to copy.");
        return;
    }
    if (CopyBusy()) {
        ShowMessage(L"A copy is already running.");
        return;
    }
    g_copy = CopyPanel{};
    g_copy.ids = ids;
    g_copy.set = playlist.empty() ? kCopySelectionSet : playlist;
    g_copy.playlistName = playlist;
    g_copy.writePlaylist = !playlist.empty();
    g_copy.dest.text = g.settings.copyFolder;
    g_copy.pattern.text = g.settings.copyPattern.empty() ? std::wstring(kDefaultOrganizePattern) : g.settings.copyPattern;
    g_copy.format = (CopyFormat)std::min<int>(g.settings.copyFormat, (int)CopyFormat::Count - 1);
    CopyReplan();
    g_kind = Kind::Copy;
    CopyFocus(0);
}

bool PanelReplanPending() { return (g_kind == Kind::Organize && g_org.dirty) || (g_kind == Kind::Copy && g_copy.dirty); }

// --- public ------------------------------------------------------------------------------------------------
void OpenEqualizer() {
    g_kind = Kind::Eq;
    g_eq.drag = -1;
    g.redraw = true;
}

void OpenTagEditor(const std::vector<TrackId>& ids) {
    if (ids.empty()) {
        ShowMessage(L"Select the tracks whose tags you want to edit.");
        return;
    }
    OpenTags(ids);
    g.redraw = true;
}

void OpenPreferences() {
    g_kind = Kind::Prefs;
    g_prefs.search.clear();
    g_prefs.scroll = 0;
    g_prefs.focus = -1;
    SDL_StartTextInput(g.window);  // typing searches
    g.redraw = true;
}

void OpenHelpers() {
    g_helpersUi = HelpersPanel{};
    g_kind = Kind::Helpers;
    g.redraw = true;
}

void OpenPacks() {
    g_packsUi = PacksPanel{};
    g_kind = Kind::Packs;
    g.redraw = true;
}

void OpenLayout() {
    g_kind = Kind::Layout;
    g.redraw = true;
}

void OpenPalette() {
    g_palette = PalettePanel{};
    auto commands = MenuCommands();
    for (size_t i = 0; i < commands.size(); ++i) {
        PaletteEntry e;
        e.command = commands[i];
        e.menu = (int)i;
        e.searchable = Lower(compat::Widen((e.command.label + " " + e.command.path).c_str()));
        g_palette.entries.push_back(std::move(e));
    }
    for (int i = 0; i < (int)std::size(kPrefs); ++i) {
        const Pref& t = kPrefs[i];
        if (!PrefShown(t)) continue;
        PaletteEntry e;
        e.command = {t.label, std::string("Preferences \xE2\x80\xBA ") + t.page, PrefText(t, t.get()), true, false};
        e.pref = i;
        e.searchable = Lower(compat::Widen((std::string(t.label) + " " + t.page + " preferences " + (t.key ? t.key : "")).c_str()));
        g_palette.entries.push_back(std::move(e));
    }
    PaletteFilter();
    g_kind = Kind::Palette;
    SDL_StartTextInput(g.window);
    g.redraw = true;
}

void OpenPoints() {
    g_points = PointsPanel{};
    g_kind = Kind::Points;
    g.redraw = true;
}

void OpenGames(int which) {
    if (g_kind != Kind::Games) {
        ClosePanel();
        g_kind = Kind::Games;
        SDL_StartTextInput(g.window);  // typing guesses
    }
    ShowGame(which);
    g.redraw = true;
}

bool GamesOpen() { return g_kind == Kind::Games; }

void OpenReview() {
    g_review = ReviewPanel{};
    g_review.year = CurrentYear();
    ReviewRecompute();
    g_kind = Kind::Review;
    g.redraw = true;
}

void OpenOutputDevice() {
    g_dev = DevicePanel{};
    g_dev.devices = ListOutputDevices();
    g_kind = Kind::Device;
    g.redraw = true;
}

void OpenFormat() {
    for (int i = 0; i < kFormats; ++i) {
        g_fmt.original[i] = g.settings.*kFormatSpecs[i].target;
        g_fmt.fields[i] = TextField{};
        g_fmt.fields[i].text = g_fmt.original[i];
        g_fmt.fields[i].caret = g_fmt.original[i].size();
    }
    g_fmt.sample = FormatSample();
    g_kind = Kind::Format;
    FormatFocus(0);
    SDL_StartTextInput(g.window);
}

void OpenListenBrainz() {
    g_lb = LbPanel{};
    g_kind = Kind::ListenBrainz;
    SDL_StartTextInput(g.window);
    g.redraw = true;
}

// The answer to a token check (wParam 0 refused, 1 accepted, 2 unreachable; lParam the user name when
// accepted) or a status change (3), on the UI thread.
void OnListenBrainzMessage(WPARAM wp, LPARAM lp) {
    std::unique_ptr<std::wstring> user(reinterpret_cast<std::wstring*>(lp));
    if (wp <= 2) {
        std::wstring message;
        if (wp == 1 && !g_lb.pendingToken.empty()) {
            listenbrainz::SetToken(g_lb.pendingToken);
            g_lb.token = TextField{};
            message = L"Scrobbling as " + (user ? *user : std::wstring(L"you")) + L".";
        } else if (wp == 0) {
            message = L"ListenBrainz didn't accept that token.";
        } else if (wp == 2) {
            message = L"Couldn't reach ListenBrainz. Check the connection (it needs curl installed) and try again.";
        }
        g_lb.checking = false;
        g_lb.pendingToken.clear();
        g_lb.message = message;
        if (g_kind != Kind::ListenBrainz) ShowMessage(message);
    }
    g.redraw = true;
}

bool PanelOpen() { return g_kind != Kind::None; }

void ClosePanel() {
    if (g_kind == Kind::Eq && g_eq.drag >= 0) SaveSettings();
    if (g_kind == Kind::Tags) {
        if (g_tags.preview) SDL_DestroyTexture(g_tags.preview);
        g_tags.preview = nullptr;
        g_tags.ids.clear();
    }
    if (g_kind == Kind::Organize) {
        // The preview holds one entry per file, which can be the whole library: let it go now.
        std::vector<OrganizeMove>().swap(g_org.plan);
        std::vector<TrackId>().swap(g_org.ids);
    }
    if (g_kind == Kind::Copy) g_copy = CopyPanel{};
    if (g_kind == Kind::Games) CloseGames();
    if (g_kind == Kind::Tags || g_kind == Kind::Prefs || g_kind == Kind::Rules || g_kind == Kind::Organize || g_kind == Kind::Copy ||
        g_kind == Kind::ListenBrainz || g_kind == Kind::Format || g_kind == Kind::Palette || g_kind == Kind::Games)
        SDL_StopTextInput(g.window);
    if (g_kind == Kind::ListenBrainz) g_lb.token = TextField{};  // a token is not left lying in memory
    g_kind = Kind::None;
    g_eq.drag = -1;
    g.redraw = true;
}

void DrawPanel(const Palette& p, float mouseX, float mouseY) {
    g_mx = mouseX;
    g_my = mouseY;
    if (g_kind == Kind::Eq) DrawEq(p);
    else if (g_kind == Kind::Tags) DrawTags(p);
    else if (g_kind == Kind::Prefs) DrawPrefs(p);
    else if (g_kind == Kind::Rules) DrawRules(p);
    else if (g_kind == Kind::Organize) DrawOrganize(p);
    else if (g_kind == Kind::Copy) DrawCopy(p);
    else if (g_kind == Kind::ListenBrainz) DrawListenBrainz(p);
    else if (g_kind == Kind::Format) DrawFormat(p);
    else if (g_kind == Kind::Device) DrawDevice(p);
    else if (g_kind == Kind::Points) DrawPointsPanel(p);
    else if (g_kind == Kind::Review) DrawReview(p);
    else if (g_kind == Kind::Palette) DrawPalette(p);
    else if (g_kind == Kind::Layout) DrawLayoutPanel(p);
    else if (g_kind == Kind::Packs) DrawPacks(p);
    else if (g_kind == Kind::Helpers) DrawHelpers(p);
    else if (g_kind == Kind::Games) DrawGames(p, mouseX, mouseY);
}

void PanelMouseDown(float x, float y, int button, int clicks) {
    g_mx = x;
    g_my = y;
    g.redraw = true;
    if (button != SDL_BUTTON_LEFT) return;
    if (g_kind == Kind::Eq) EqMouseDown(x, y, clicks);
    else if (g_kind == Kind::Tags) TagsMouseDown(x, y);
    else if (g_kind == Kind::Prefs) PrefsMouseDown(x, y);
    else if (g_kind == Kind::Rules) RulesMouseDown(x, y);
    else if (g_kind == Kind::Organize) OrganizeMouseDown(x, y);
    else if (g_kind == Kind::Copy) CopyMouseDown(x, y);
    else if (g_kind == Kind::ListenBrainz) LbMouseDown(x, y);
    else if (g_kind == Kind::Format) FormatMouseDown(x, y);
    else if (g_kind == Kind::Device) DeviceMouseDown(x, y);
    else if (g_kind == Kind::Points) PointsMouseDown(x, y);
    else if (g_kind == Kind::Review) ReviewMouseDown(x, y);
    else if (g_kind == Kind::Palette) PaletteMouseDown(x, y);
    else if (g_kind == Kind::Layout) LayoutMouseDown(x, y);
    else if (g_kind == Kind::Packs) PacksMouseDown(x, y);
    else if (g_kind == Kind::Helpers) HelpersMouseDown(x, y);
    else if (g_kind == Kind::Games) GamesMouseDown(x, y);
}

void PanelMouseMove(float x, float y) {
    g_mx = x;
    g_my = y;
    g.redraw = true;
    if (g_kind == Kind::Points) PointsMouseMove(x, y);
    if (g_kind == Kind::Review) ReviewMouseMove(x, y);
    if (g_kind != Kind::Eq || g_eq.drag < 0) return;
    if (g_eq.drag == kEqBands + 1) EqCurveAt(x, y, false);
    else SetEqValue(g_eq.drag, SliderValueAt(g_eq.drag, y), false);
}

void PanelMouseUp() {
    if (g_kind == Kind::Eq && g_eq.drag >= 0) {
        g_eq.drag = -1;
        SaveSettings();
    }
    g.redraw = true;
}

void PanelWheel(float x, float y, float dy) {
    if (g_kind == Kind::Eq) {
        if (int i = EqSliderAt(x, y); i >= 0) {
            g_eq.focus = i;
            SetEqValue(i, *EqValue(i) + (dy > 0 ? 0.5 : -0.5), true);
        }
    } else if (g_kind == Kind::Tags) {
        for (int f : {Comment, Lyrics}) {
            TextField& t = g_tags.fields[f];
            if (t.rect.In(x, y)) t.scrollY = std::max(0.0f, t.scrollY - dy * LineHeight(Font::Normal) * 3);
        }
    } else if (g_kind == Kind::Helpers) {
        g_helpersUi.first = std::clamp(g_helpersUi.first - (int)std::lround(dy * 2), 0, std::max(0, (int)HelperCount() - kHelperRows));
    } else if (g_kind == Kind::Packs) {
        g_packsUi.first = std::clamp(g_packsUi.first - (int)std::lround(dy * 2), 0, std::max(0, (int)PackCount() - kPackRows));
    } else if (g_kind == Kind::Palette) {
        g_palette.first = std::clamp(g_palette.first - (int)std::lround(dy * 2), 0, std::max(0, (int)g_palette.shown.size() - kPaletteRows));
    } else if (g_kind == Kind::Device) {
        g_dev.first = std::clamp(g_dev.first - (int)std::lround(dy * 2), 0, std::max(0, (int)g_dev.devices.size() + 1 - kDeviceRows));
    } else if (g_kind == Kind::Prefs) {
        g_prefs.scroll -= dy * S(60);
        ClampPrefsScroll();
    } else if (g_kind == Kind::Rules) {
        const int rows = (int)g_rules.rows.size(), visible = Geometry().visible;
        g_rules.first = std::clamp(g_rules.first - (int)std::lround(dy), 0, std::max(0, rows - visible));
    } else if (g_kind == Kind::Organize) {
        const int rows = (int)g_org.plan.size(), visible = OrgGeometry().visible;
        g_org.first = std::clamp(g_org.first - (int)std::lround(dy * 3), 0, std::max(0, rows - visible));
    } else if (g_kind == Kind::Copy) {
        const int rows = (int)g_copy.plan.size(), visible = CopyGeom().visible;
        g_copy.first = std::clamp(g_copy.first - (int)std::lround(dy * 3), 0, std::max(0, rows - visible));
    }
    g.redraw = true;
}

void PanelKey(const SDL_KeyboardEvent& e) {
    if (g_kind == Kind::Eq) EqKey(e);
    else if (g_kind == Kind::Tags) TagsKey(e);
    else if (g_kind == Kind::Prefs) PrefsKey(e);
    else if (g_kind == Kind::Rules) RulesKey(e);
    else if (g_kind == Kind::Organize) OrganizeKey(e);
    else if (g_kind == Kind::Copy) CopyKey(e);
    else if (g_kind == Kind::ListenBrainz) LbKey(e);
    else if (g_kind == Kind::Format) FormatKey(e);
    else if (g_kind == Kind::Device) DeviceKey(e);
    else if (g_kind == Kind::Points) {
        if (e.key == SDLK_ESCAPE) ClosePanel();
    } else if (g_kind == Kind::Review) ReviewKey(e);
    else if (g_kind == Kind::Palette) PaletteKey(e);
    else if (g_kind == Kind::Layout) LayoutKey(e);
    else if (g_kind == Kind::Packs) PacksKey(e);
    else if (g_kind == Kind::Helpers) HelpersKey(e);
    else if (g_kind == Kind::Games) GamesKey(e);
}

void PanelText(const char* text) {
    if (g_kind == Kind::Rules && text) RulesText(compat::Widen(text));
    if (g_kind == Kind::Organize && text) OrganizeText(compat::Widen(text));
    if (g_kind == Kind::Copy && text) CopyText(compat::Widen(text));
    if (g_kind == Kind::ListenBrainz && text) LbText(compat::Widen(text));
    if (g_kind == Kind::Format && text) FormatText(compat::Widen(text));
    if (g_kind == Kind::Palette && text) PaletteText(compat::Widen(text));
    if (g_kind == Kind::Tags && text) InsertText(compat::Widen(text));
    if (g_kind == Kind::Games && text) GamesText(compat::Widen(text));
    if (g_kind == Kind::Prefs && text && !(g_prefs.focus >= 0 && !strcmp(text, " "))) {
        g_prefs.search += compat::Widen(text);
        g_prefs.scroll = 0;
        g_prefs.focus = -1;
        g.redraw = true;
    }
}

void PanelFileChosen(int purpose, const std::wstring& path) {
    if (purpose == kFileHelper && g_kind == Kind::Helpers) InstallHelper(path);
    else if (purpose == kFilePack && g_kind == Kind::Packs) InstallPack(path);
    else if (purpose == kFileCover && g_kind == Kind::Tags) ChooseCover(path);
    else if (purpose == kFileAutoEq && g_kind == Kind::Eq) ImportAutoEq(path);
    else if (purpose == kFileOrganizeRoot && g_kind == Kind::Organize) {
        g_org.root.text = path;
        OrganizeFocus(0);
        OrganizeEdited();
    } else if (purpose == kFileCopyDest && g_kind == Kind::Copy) {
        g_copy.dest.text = path;
        CopyFocus(0);
        CopyEdited();
    }
    g.redraw = true;
}

bool PanelWantsText(float x, float y) {
    if (g_kind == Kind::ListenBrainz) return g_lb.field.In(x, y);
    if (g_kind == Kind::Palette) return g_palette.field.In(x, y);
    if (g_kind == Kind::Games) return GamesWantsText(x, y);
    if (g_kind == Kind::Format) {
        for (const TextField& t : g_fmt.fields)
            if (t.rect.In(x, y)) return true;
        return false;
    }
    if (g_kind == Kind::Organize) {
        const OrganizeGeometry G = OrgGeometry();
        return G.root.In(x, y) || G.pattern.In(x, y);
    }
    if (g_kind == Kind::Copy) {
        const CopyGeometry G = CopyGeom();
        return G.dest.In(x, y) || G.pattern.In(x, y);
    }
    if (g_kind == Kind::Rules) {
        const RulesGeometry G = Geometry();
        if (G.name.In(x, y) || G.limitField.In(x, y)) return true;
        for (size_t k = 0; k < G.value.size(); ++k)
            if (G.value[k].In(x, y) || G.value2[k].In(x, y)) return true;
        return false;
    }
    if (g_kind != Kind::Tags) return false;
    for (const TextField& t : g_tags.fields)
        if (t.enabled && t.rect.In(x, y)) return true;
    return false;
}

}  // namespace lx
