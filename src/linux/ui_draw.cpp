// The Linux window's drawing: the shared shapes and glyphs, the menu bar and popups, and every window part.
#include "alternate_ui.h"
#include "big_ui.h"
#include "discart.h"
#include "ui_internal.h"

#include <array>

namespace lx {

void Circle(float cx, float cy, float r, uint32_t rgb) {
    std::vector<SDL_FPoint> pts;
    for (int i = 0; i < 24; ++i) pts.push_back({cx + std::cos(i / 24.f * 6.2831853f) * r, cy + std::sin(i / 24.f * 6.2831853f) * r});
    Polygon(pts, rgb);
}

// The outline of `r` with corners of radius `rad`, clockwise.
std::vector<SDL_FPoint> RoundPath(const Rect& r, float rad) {
    std::vector<SDL_FPoint> pts;
    const float cx[] = {r.R() - rad, r.R() - rad, r.x + rad, r.x + rad}, cy[] = {r.y + rad, r.B() - rad, r.B() - rad, r.y + rad};
    for (int c = 0; c < 4; ++c)
        for (int i = 0; i <= 6; ++i) {
            float a = (c - 1 + i / 6.f) * 1.5707963f;
            pts.push_back({cx[c] + std::cos(a) * rad, cy[c] + std::sin(a) * rad});
        }
    return pts;
}

void FillRound(const Rect& r, float rad, uint32_t rgb, float alpha) { Polygon(RoundPath(r, rad), rgb, alpha); }

void FrameRound(const Rect& r, float rad, uint32_t rgb) {
    auto pts = RoundPath({r.x + 0.5f, r.y + 0.5f, r.w - 1, r.h - 1}, rad);
    pts.push_back(pts.front());
    SetColor(rgb);
    SDL_RenderLines(g.renderer, pts.data(), (int)pts.size());
}

std::string Plain(const wchar_t* label) {  // a Windows menu label without its mnemonic marks
    std::wstring s;
    for (const wchar_t* p = label; *p; ++p)
        if (*p != L'&' || p[1] == L'&') s += *p == L'&' ? *++p : *p;
    return Utf8(s);
}

// The transport glyphs, in the thin line style of Segoe MDL2 Assets. The Windows build draws them
// from that font at 13, 17 or 21 pixels depending on the button height, so match those sizes and
// the even stroke weight instead of drawing the same 12-pixel hairlines in every mode.
void DrawIcon(Icon icon, const Rect& r, uint32_t rgb) {
    float s = S(r.h >= S(46) ? 21 : r.h >= S(34) ? 17 : 13);
    float cx = std::round(r.x + r.w / 2), cy = std::round(r.y + r.h / 2);
    float t = std::max(1.0f, std::round(s * 0.15f));
    auto line = [&](float x1, float y1, float x2, float y2) { ThickLine(x1, y1, x2, y2, t, rgb); };
    auto head = [&](float x, float y, float dx, float dy) {  // a filled head at (x, y), pointing (dx, dy)
        float len = std::max(0.001f, std::hypot(dx, dy)), ux = dx / len, uy = dy / len;
        Polygon({{x, y},
                 {x - ux * s * 0.26f - uy * s * 0.17f, y - uy * s * 0.26f + ux * s * 0.17f},
                 {x - ux * s * 0.26f + uy * s * 0.17f, y - uy * s * 0.26f - ux * s * 0.17f}}, rgb);
    };
    auto ring = [&](float outer, float inner, float from, float to, bool headAtEnd) {  // an open ring with a head
        constexpr int kSteps = 48;
        std::vector<SDL_Vertex> v;
        std::vector<int> idx;
        for (int i = 0; i <= kSteps; ++i) {  // a single annulus, so the stroke has no seams
            float a = from + (to - from) * (float)i / kSteps;
            v.push_back({{cx + std::cos(a) * outer, cy + std::sin(a) * outer}, FColor(rgb), {0, 0}});
            v.push_back({{cx + std::cos(a) * inner, cy + std::sin(a) * inner}, FColor(rgb), {0, 0}});
        }
        for (int i = 0; i < kSteps; ++i) {
            int b = i * 2;
            idx.insert(idx.end(), {b, b + 1, b + 2, b + 2, b + 1, b + 3});
        }
        SDL_RenderGeometry(g.renderer, nullptr, v.data(), (int)v.size(), idx.data(), (int)idx.size());
        float a = headAtEnd ? to : from, dir = headAtEnd ? 1 : -1;
        float mid = (outer + inner) / 2, half = (outer - inner) * 0.95f;
        SDL_FPoint p{cx + std::cos(a) * mid, cy + std::sin(a) * mid};
        SDL_FPoint tan{-std::sin(a) * dir, std::cos(a) * dir}, rad{std::cos(a), std::sin(a)};
        Polygon({{p.x + tan.x * half * 2.0f, p.y + tan.y * half * 2.0f},
                 {p.x + rad.x * half, p.y + rad.y * half},
                 {p.x - rad.x * half, p.y - rad.y * half}}, rgb);
    };
    float h = s * 0.38f;
    switch (icon) {
        case Icon::Prev:
            line(cx - s * 0.34f, cy - h, cx - s * 0.34f, cy + h);
            Polygon({{cx + s * 0.34f, cy - h}, {cx + s * 0.34f, cy + h}, {cx - s * 0.18f, cy}}, rgb);
            break;
        case Icon::Next:
            line(cx + s * 0.34f, cy - h, cx + s * 0.34f, cy + h);
            Polygon({{cx - s * 0.34f, cy - h}, {cx - s * 0.34f, cy + h}, {cx + s * 0.18f, cy}}, rgb);
            break;
        case Icon::Play: {
            float x = cx - s * 0.26f, w = s * 0.64f, hh = s * 0.42f;
            line(x, cy - hh, x + w, cy);
            line(x + w, cy, x, cy + hh);
            line(x, cy + hh, x, cy - hh);
            for (float yy : {cy - hh, cy + hh}) Fill({x - t / 2, yy - t / 2, t, t}, rgb);
            Fill({x + w - t / 2, cy - t / 2, t, t}, rgb);
            break;
        }
        case Icon::Pause:
            line(cx - s * 0.17f, cy - h, cx - s * 0.17f, cy + h);
            line(cx + s * 0.17f, cy - h, cx + s * 0.17f, cy + h);
            break;
        case Icon::Shuffle: {
            float x0 = cx - s * 0.46f, x1 = cx + s * 0.32f, y0 = cy - s * 0.26f, y1 = cy + s * 0.26f;
            line(x0, y0, cx - s * 0.14f, y0);
            line(cx - s * 0.14f, y0, cx + s * 0.12f, y1);
            line(cx + s * 0.12f, y1, x1, y1);
            line(x0, y1, cx - s * 0.14f, y1);
            line(cx - s * 0.14f, y1, cx + s * 0.12f, y0);
            line(cx + s * 0.12f, y0, x1, y0);
            head(x1, y0, 1, 0);
            head(x1, y1, 1, 0);
            break;
        }
        case Icon::Repeat:
        case Icon::RepeatOne:
        case Icon::Resume: {
            // A ring with a head at its gap; Resume (history) runs the other way and adds hands.
            const bool resume = icon == Icon::Resume;
            float mid = s * 0.39f, thick = std::max(1.6f, s * 0.16f);
            float outer = mid + thick / 2, inner = mid - thick / 2;
            if (resume) {
                ring(outer, inner, 0.6f, 5.5f, false);
                line(cx, cy - mid * 0.5f, cx, cy);
                line(cx, cy, cx + mid * 0.4f, cy + mid * 0.28f);
            } else {
                ring(outer, inner, 5.4f, 4.2f + 6.2831853f, true);  // gap at the top, head pointing clockwise
            }
            if (icon == Icon::RepeatOne) {  // a "1" is smaller than a font size here
                ThickLine(cx, cy - mid * 0.5f, cx, cy + mid * 0.5f, 1.0f, rgb);
                ThickLine(cx, cy - mid * 0.5f, cx - mid * 0.3f, cy - mid * 0.24f, 1.0f, rgb);
                ThickLine(cx - mid * 0.18f, cy + mid * 0.5f, cx + mid * 0.18f, cy + mid * 0.5f, 1.0f, rgb);
            }
            break;
        }
        case Icon::Speaker: {
            float sx = cx - s * 0.3f;
            Polygon({{sx - s * 0.2f, cy - s * 0.12f}, {sx, cy - s * 0.12f}, {sx + s * 0.22f, cy - s * 0.3f},
                     {sx + s * 0.22f, cy + s * 0.3f}, {sx, cy + s * 0.12f}, {sx - s * 0.2f, cy + s * 0.12f}}, rgb);
            for (float k : {0.38f, 0.58f}) {
                std::vector<SDL_FPoint> arc;
                for (int i = 0; i <= 16; ++i) {
                    float a = -0.8f + 1.6f * i / 16;
                    arc.push_back({sx + std::cos(a) * s * k, cy + std::sin(a) * s * k});
                }
                for (size_t i = 0; i + 1 < arc.size(); ++i) line(arc[i].x, arc[i].y, arc[i + 1].x, arc[i + 1].y);
            }
            break;
        }
    }
}

// The menu bar: the system's light bar, or in a dark theme the palette's surface (app_ui.cpp,
// HandleDarkMenuBar).
void DrawMenuBar(const Palette& p) {
    bool dark = p.dark;
    Fill(L.menuBar, dark ? p.surface : 0xFFFFFF);
    if (!dark) {
        Fill({0, L.menuBar.B() - S(2), L.menuBar.w, S(1)}, 0xF2F2F2);
        Fill({0, L.menuBar.B() - S(1), L.menuBar.w, S(1)}, 0xF0F0F0);
    }
    auto menus = BarMenus();
    for (size_t i = 0; i < L.barItems.size(); ++i) {
        const Rect& r = L.barItems[i];
        bool hot = (int)i == g_barOpen || (g_popups.empty() && r.In(g_mouseX, g_mouseY));
        if (hot) Fill(r, dark ? p.hover : 0xF9F9F9);
        DrawText(Font::Menu, Plain(ui::kMenuBar[menus[i]]), r.x, TextY(r, Font::Menu), dark ? p.text : 0x000000, r.w, 1);
    }
}

// Windows 11's popup menus, light or dark with the theme.
void DrawPopups(const Palette& p) {
    bool dark = p.dark;
    uint32_t bg = dark ? 0x2C2C2C : 0xF9F9F9, edge = dark ? 0x3E3E3E : 0xE5E5E5, hot = dark ? 0x353535 : 0xF0F0F0;
    uint32_t text = dark ? 0xFFFFFF : 0x000000, off = dark ? 0x787878 : 0xA0A0A0, rule = dark ? 0x3E3E3E : 0xD7D7D7;
    for (size_t k = 0; k < g_popups.size(); ++k) {
        const Popup& pop = g_popups[k];
        Rect box{pop.x, pop.y, pop.w, pop.h};
        for (int i = 1; i <= 4; ++i) FillRound({box.x - i + S(1), box.y - i + S(2), box.w + 2 * i, box.h + 2 * i}, S(8) + i, 0x000000, 0.035f);
        FillRound(box, S(8), bg);
        FrameRound(box, S(8), edge);
        for (size_t i = 0; i < pop.items.size(); ++i) {
            const MenuItem& it = pop.items[i];
            Rect r = PopupItemRect(pop, i);
            if (it.separator) {
                Fill({r.x + S(1), std::round(r.y + r.h / 2), r.w - S(2), S(1)}, rule);
                continue;
            }
            if ((int)i == pop.hover && it.enabled) FillRound({r.x + S(4), r.y, r.w - S(8), r.h}, S(4), hot);
            uint32_t color = it.enabled ? text : off;
            float cy = r.y + r.h / 2;
            if (it.checked && it.radio) {
                Circle(r.x + S(18), cy, S(2.5f), color);
            } else if (it.checked) {
                ThickLine(r.x + S(13), cy, r.x + S(16), cy + S(3), std::max(1.0f, S(1)), color);
                ThickLine(r.x + S(16), cy + S(3), r.x + S(22), cy - S(4), std::max(1.0f, S(1)), color);
            }
            DrawText(Font::Menu, it.label, r.x + S(kLabelX), TextY(r, Font::Menu), color);
            if (!it.shortcut.empty())
                DrawText(Font::Menu, it.shortcut, r.R() - S(kShortcutRight) - TextWidth(Font::Menu, it.shortcut), TextY(r, Font::Menu), color);
            if (!it.sub.empty()) {
                float ax = r.R() - S(16);
                ThickLine(ax - S(3), cy - S(4), ax + S(1), cy, std::max(1.0f, S(1)), color);
                ThickLine(ax + S(1), cy, ax - S(3), cy + S(4), std::max(1.0f, S(1)), color);
            }
        }
    }
}

// A flat button of the band (app_ui.cpp, DrawToolbarButton): hover fills and frames it; shuffle,
// repeat and resume show they are on with an accent glyph raised over a dot.
void DrawBandButton(const Rect& r, Icon icon, const Palette& p, bool active, bool enabled) {
    if (r.w <= 0) return;
    bool hot = enabled && g_popups.empty() && r.In(g_mouseX, g_mouseY);
    bool pressed = hot && g_drag == Drag::None && (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK);
    if (hot) {
        Fill(r, pressed ? p.select : p.hover);
        Frame(r, p.border);
    }
    uint32_t color = !enabled ? p.faint : active ? p.accent : p.text;
    Rect gr = r;
    if (active) gr.y -= S(2);
    DrawIcon(icon, gr, color);
    if (active) {
        float d = std::max(2.0f, S(3));
        Fill({std::round(r.x + r.w / 2 - d / 2), r.B() - S(5) - d / 2, d, d}, color);
    }
}

// A flat slider (app_ui_trackbar.cpp, DrawTrackbar): a slim channel, the accent up to the position, and a
// slim upright thumb in the text color (the accent while hovered or dragged).
void DrawSlider(const Rect& r, double frac, bool enabled, bool hot, const Palette& p, bool vertical, double notch) {
    if (vertical) {
        float inset = S(ui::kSliderInset), width = S(ui::kSliderTrackHeight);
        Rect channel{std::round(r.x + r.w / 2 - width / 2), r.y + inset, width, std::max(0.f, r.h - 2 * inset)};
        Fill(channel, p.border);
        if (frac > 0) Fill({channel.x, channel.y, width, std::round(channel.h * (float)frac)}, enabled ? p.accent : p.faint);
        if (enabled) {
            float th = std::max(2.f, S(ui::kSliderThumbWidth)), tw = std::min(S(ui::kSliderThumbHeight), r.w);
            Fill({r.x + (r.w - tw) / 2, channel.y + channel.h * (float)frac - th / 2, tw, th}, hot ? p.accent : p.text);
        }
        return;
    }
    float inset = S(ui::kSliderInset), h = S(ui::kSliderTrackHeight);
    Rect channel{r.x + inset, std::round(r.y + r.h / 2 - h / 2), r.w - 2 * inset, h};
    Fill(channel, p.border);
    if (frac > 0) Fill({channel.x, channel.y, std::round(channel.w * (float)frac), h}, enabled ? p.accent : p.faint);
    if (!enabled) return;
    // A long track's notches at round times (ui::SeekNotchSeconds), `notch` of the channel apart.
    if (notch > 0 && channel.w * notch >= S(16))
        for (double at = notch; at < 1; at += notch)
            Fill({std::floor(channel.x + channel.w * (float)at), channel.y, std::max(1.f, S(1)), h}, p.surface);
    float tw = std::max(2.0f, S(ui::kSliderThumbWidth)), th = std::min(S(ui::kSliderThumbHeight), r.h);
    float cx = std::round(channel.x + channel.w * (float)frac);
    Fill({cx - std::floor(tw / 2), std::round(r.y + r.h / 2 - th / 2), tw, th}, hot ? p.accent : p.text);
}

bool PeakSource(TrackId id, std::wstring& path, double& start, double& end) {
    if (id == kNone || id >= g_lib.Count()) return false;
    path = g_lib.Path(id);
    start = end = 0;
    if (IsSubtrack(path) && !SubtrackRange(path, start, end)) return false;
    return true;
}

// Queues (or answers at once) the peaks for `id`; a CUE track asks for its range of the file.
bool RequestPeaks(TrackId id, Peaks& peaks, uint32_t& request) {
    std::wstring path;
    double start, end;
    if (!PeakSource(id, path, start, end)) { request = 0; return false; }
    const Track& t = g_lib[id];
    return GetPeaks(path, t.fileSize, t.fileTime, start, end, peaks, request) || GetPeakProgress(request, peaks);
}

// The body and edge rectangles of one paint, grouped by color. A column's rectangles never overlap
// and adjacent columns never touch, so filling each color's rectangles in one SDL_RenderFillRects
// call gives exactly the pixels the per-column Fill calls did, with a fraction of the draw calls.
class WaveFills {
public:
    void Reset() {
        for (int i = 0; i < used_; ++i) groups_[i].rects.clear();
        used_ = 0;
    }

    void Add(const Rect& r, uint32_t color) {
        if (!(r.w > 0) || !(r.h > 0)) return;
        int i = 0;
        while (i < used_ && groups_[i].color != color) ++i;
        if (i == used_) {
            if (used_ == kGroups) {  // more shades than the two color schemes can name
                Fill(r, color);
                return;
            }
            groups_[i].color = color;
            ++used_;
        }
        groups_[i].rects.push_back({r.x, r.y, r.w, r.h});
    }

    void Flush() {
        for (int i = 0; i < used_; ++i) {
            SetColor(groups_[i].color);
#if SDL_VERSION_ATLEAST(3, 2, 0)
            SDL_RenderFillRects(g.renderer, groups_[i].rects.data(), (int)groups_[i].rects.size());
#else
            for (const SDL_FRect& r : groups_[i].rects) SDL_RenderFillRect(g.renderer, &r);
#endif
        }
        Reset();
    }

private:
    // Played and rest, each six body and mirror bands and two edges; a moodbar's colours overflow
    // into single fills.
    static constexpr int kGroups = 32;
    struct Group {
        uint32_t color = 0;
        std::vector<SDL_FRect> rects;
    };
    std::array<Group, kGroups> groups_{};
    int used_ = 0;
};

// The waveform over the seek bar's channel: columns two pixels wide a pixel apart, each the RMS
// level under it as a solid body with the loudest peak as a fainter edge (a live waveform has peaks
// only), the played part in the accent (amber while paused), the rest faint. Shading and the
// reflected lower half follow WaveShades (app_waveform.cpp). The split is the playhead. The
// moodbar styles colour the columns by their sound and draw a playhead line in the state's colour,
// as app_waveform.cpp does. False while there are no peaks to draw.
bool LiveWaveformShown() {
    return g.settings.waveform && (RadioPlaying() || (g.settings.liveWaveform && g.playing != kNone));
}

bool SeekGlowPulsing() {
    return g.settings.waveform && g.settings.seekGlow && g.playing != kNone && g_wave.track == g.playing &&
           (g_wave.ready || g_wave.request) && !FountainStyleShown();
}

bool LiveFountainShown() {
    return g.settings.waveform && g.settings.liveFountain && g.settings.theme == ThemeMode::Deltarune;
}

// What the moodbar's colours turn from on `background`: the chosen Waveform colour, else the accent.
static Rgb MoodBase(const Palette& p, Rgb background) {
    const Rgb chosen = WaveColorOn(g.settings.waveColor, background);
    return chosen != kNoRgb ? chosen : p.accent;
}

bool DrawWaveform(const Rect& r, double frac, const Palette& p) {
    if (!g.settings.waveform) return false;
    if (FountainStyleShown() && DrawFountain(r, frac, p, g_wave.ready ? g_wave.peaks.v : nullptr,
        kPeakBuckets, LiveFountainShown(), SeekVertical())) return true;
    bool live = LiveWaveformShown();
    if (!live && (g_wave.track != g.playing || g.playing == kNone || (!g_wave.ready && !g_wave.request))) return false;
    std::array<uint8_t, 256> livePeaks{};
    // The shared engine shapes live heights around audible note attacks, as on Windows.
    if (live) g.engine.WaveformPeaks(livePeaks.data(), livePeaks.size());
    if (RadioPlaying()) frac = 1;
    bool vertical = SeekVertical();
    float left = (vertical ? r.y : r.x) + S(9), width = (vertical ? r.h : r.w) - S(18), right = left + width;
    float cy = std::floor(vertical ? r.x + r.w / 2 : r.y + r.h / 2);
    float half = std::max(1.0f, std::floor((vertical ? r.w : r.h) / 2) - S(2));
    // The whole song's peaks, softened when Smooth seek bar is on (app_waveform.cpp's radius).
    const bool song = !live && g_wave.ready;
    static Peaks smoothed;
    if (song && g.settings.smoothSeek) SmoothPeaks(g_wave.peaks, smoothed, 8);
    const Peaks& peaks = song && g.settings.smoothSeek ? smoothed : g_wave.peaks;
    // A track whose peaks are still unknown has no mood yet; a live waveform takes its colours from
    // the audio being heard.
    const int style = live || song ? std::clamp(g.settings.seekStyle, 0, kSeekStyles - 1) : kSeekWaveform;
    const bool mood = style != kSeekWaveform, stripe = style == kSeekMoodbar;
    static MoodPalette moods;
    static Peaks liveMood;
    static LiveMood liveScale;
    float center = 0;
    if (mood) {
        // The mood waveform can take its own scheme; the stripe always takes the moodbar's.
        const int scheme = std::clamp(!stripe && g.settings.moodWaveColors > 0 ? g.settings.moodWaveColors - 1
                                                                               : g.settings.moodColors,
                                      0, kMoodColorSchemes - 1);
        const Rgb base = MoodBase(p, p.surface);
        if (!moods.For(base, p.surface, scheme)) moods.Make(base, p.surface, scheme);
        if (live) {
            std::array<float, 256 * kMoodBands> bands{};
            g.engine.WaveformBands(bands.data(), livePeaks.size());
            center = liveScale.Scale(bands.data(), (int)livePeaks.size(), liveMood);
        } else {
            center = MoodCenter(peaks);
        }
    }
    // The stripe's columns touch; the waveform's stand a pixel apart.
    float col = std::max(2.0f, S(2)), step = stripe ? col : col + std::max(1.0f, S(1));
    // The reflection is the dimmer lower half; off draws the upper half alone. Full height mirrors a
    // waveform as bright below the center line as above it, as on Windows; the moodbar stripe keeps
    // the upper half.
    const bool fullHeight = !g.settings.waveReflection && g.settings.waveFullHeight && !stripe;
    const bool reflect = g.settings.waveReflection || fullHeight;
    // Seek bar glow, as app_waveform.cpp draws it: a halo of each column's colour around the bar,
    // fading out from its edges and ends, pulsing with the beat of what is heard (GlowPulse),
    // stronger where the song is loud and on what has played. The stripe stands shorter to leave
    // the halo room before the bar's edge.
    const float acrossTop = vertical ? r.x : r.y, acrossBottom = acrossTop + (vertical ? r.w : r.h);
    const float alongStart = vertical ? r.y : r.x, alongEnd = alongStart + (vertical ? r.h : r.w);
    const int glow = g.settings.seekGlow ? std::max((int)S(4), (int)half / 2) : 0;
    const float room = std::min(cy - acrossTop, acrossBottom - cy - 1) - glow - 1;  // a pixel short of the edge
    const float stripeHalf = stripe && glow ? std::clamp(room, 1.0f, half) : half;
    static GlowPulse pulse;
    if (glow && (g.playing != kNone || RadioPlaying()) && !IsPaused()) {
        std::array<uint8_t, 256> heard{};
        if (live) heard = livePeaks;
        else g.engine.WaveformPeaks(heard.data(), heard.size());
        pulse.Step(heard.data(), (int)heard.size(), SDL_GetTicks() / 1000.0);
    }
    const float beat = pulse.Value();
    float playedX = left + std::floor(width * (float)frac);
    uint32_t played = WavePlayedColor(p), rest = WaveRestColor(p, live);
    if (live && g.settings.fullLiveWaveform) rest = played;
    // Brightest along the center line, a dimmer reflection below it; bands as on Windows.
    constexpr int kBands = 6;
    struct Shades { uint32_t body[kBands], mirror[kBands], edge, mirrorEdge; };
    auto shadesOf = [&](uint32_t c) {
        Shades s{};
        for (int i = 0; i < kBands; ++i) {
            s.body[i] = Mix(c, 0xFFFFFF, 24 * (kBands - 1 - i) / (kBands - 1));
            s.mirror[i] = Mix(c, p.surface, 30 + 40 * i / (kBands - 1));
        }
        s.edge = Mix(c, p.surface, 35);
        s.mirrorEdge = Mix(c, p.surface, 72);
        return s;
    };
    const Shades playedShades = shadesOf(played), restShades = shadesOf(rest);
    static WaveFills fills;
    fills.Reset();
    const int span = (int)half + 1;
    auto bandStart = [span](int i) { return (float)(i * span / kBands); };
    // A run of `rows` rows starting `from` rows out from the center line, above it or below.
    auto rows = [&](float x, float barWidth, float from, float to, bool below, uint32_t color) {
        if (to <= from) return;
        float y = below ? cy + 1 + from : cy - to + 1;
        fills.Add(vertical ? Rect{y, x, to - from, barWidth} : Rect{x, y, barWidth, to - from}, color);
    };
    // `glow` rows of `tint` at `percent` strength, fading softly, above row `top` and, mirrored at
    // half strength unless `even`, from row `bottom` down (the reflection's first row past its edge).
    auto fadeAt = [&](int percent, int d) { return percent * (glow + 1 - d) * (glow + 1 - d) / ((glow + 1) * (glow + 1)); };
    auto halo = [&](float x, float barWidth, uint32_t tint, int percent, float top, float bottom, bool even) {
        for (int d = 1; d <= glow && percent > 0; ++d) {
            const int fade = fadeAt(percent, d);
            auto row = [&](float y, int amount) {
                fills.Add(vertical ? Rect{y, x, 1, barWidth} : Rect{x, y, barWidth, 1}, Mix(p.surface, tint, amount));
            };
            if (top - d >= acrossTop) row(top - d, fade);
            if (bottom + d <= acrossBottom) row(bottom + d - 1, even ? fade : fade / 2);
        }
    };
    auto glowPercent = [beat](float level, bool heard) {
        level = std::clamp(level, 0.0f, 1.0f);
        return level > 0 ? (int)((25 + 70 * beat) * (0.6f + 0.4f * level) * (heard ? 1.0f : 0.55f)) : 0;
    };
    // The stripe's two ends glow sideways too, so the halo goes all the way around.
    struct EndGlow { uint32_t tint = 0; int percent = 0; };
    EndGlow firstEnd, lastEnd;
    for (float x = left; x < right; x += step) {
        int count = live ? (int)livePeaks.size() : kPeakBuckets;
        int first = std::clamp((int)((x - left) * count / width), 0, count - 1);
        int last = std::clamp((int)((x + step - left) * count / width), first + 1, count);
        int v = 0, rms = 0;
        for (int b = first; b < last; ++b) {
            v = std::max<int>(v, live ? livePeaks[b] : g_wave.ready ? peaks.v[b] : 0);
            rms = std::max<int>(rms, live ? livePeaks[b] : g_wave.ready ? peaks.rms[b] : 0);
        }
        float h = std::max(1.0f, std::floor(half * v / 255));
        float body = std::clamp(std::floor(half * rms / 255), 1.0f, h);
        const Shades* shades = x < playedX ? &playedShades : &restShades;
        float barWidth = std::min(x + col, right) - x;
        // Live audio counts as heard unless its progress is split off, as `rest` already says.
        const bool heard = x < playedX || (live && g.settings.fullLiveWaveform);
        uint32_t tint = x < playedX ? played : rest;
        Shades moodShades;
        if (mood) {
            const MoodTone tone = live ? MoodOf(liveMood, first, last, center, count) : MoodOf(peaks, first, last, center);
            tint = moods.Pick(tone, heard);
            if (stripe) {
                // Without the reflection the stripe stands on the center line, and glows below it.
                float top = cy - stripeHalf, bottom = reflect ? cy + stripeHalf + 1 : cy + 1, end = bottom;
                fills.Add(vertical ? Rect{top, x, end - top, barWidth} : Rect{x, top, barWidth, end - top}, tint);
                const int percent = glowPercent(tone.level, heard);
                halo(x, barWidth, tint, percent, top, bottom, true);
                if (x == left) firstEnd = {tint, percent};
                lastEnd = {tint, percent};
                continue;
            }
            moodShades = shadesOf(tint);
            shades = &moodShades;
        }
        const Shades& s = *shades;
        for (int i = 0; i < kBands && bandStart(i) <= body; ++i) {
            float d0 = bandStart(i), d1 = std::min(body + 1, bandStart(i + 1));
            rows(x, barWidth, d0, d1, false, s.body[i]);
            if (reflect) rows(x, barWidth, d0, std::min(d1, body), true, fullHeight ? s.body[i] : s.mirror[i]);
        }
        rows(x, barWidth, body + 1, h + 1, false, s.edge);
        if (reflect) rows(x, barWidth, body, h, true, fullHeight ? s.edge : s.mirrorEdge);
        uint8_t alpha = live ? (uint8_t)std::max(0.f, half * v - h * 255) : 0;
        if (alpha) {
            int tip = std::min(kBands - 1, (int)h * kBands / span);
            Fill(vertical ? Rect{cy - h - 1, x, 1, barWidth} : Rect{x, cy - h - 1, barWidth, 1}, s.body[tip], alpha);
            if (reflect)
                Fill(vertical ? Rect{cy + h + 1, x, 1, barWidth} : Rect{x, cy + h + 1, barWidth, 1}, fullHeight ? s.body[tip] : s.mirror[tip], alpha);
        }
        // The glow starts past the column's tip, and past its fractional end pixel when it has one.
        halo(x, barWidth, tint, glowPercent(v / 255.0f, heard), alpha ? cy - h - 1 : cy - h,
             !reflect ? cy + 1 : alpha ? cy + h + 2 : cy + h + 1, fullHeight);
    }
    if (stripe && glow) {
        const float top = cy - stripeHalf, end = reflect ? cy + stripeHalf + 1 : cy + 1;
        auto side = [&](float x, const EndGlow& e, int d) {
            if (e.percent) fills.Add(vertical ? Rect{top, x, end - top, 1} : Rect{x, top, 1, end - top}, Mix(p.surface, e.tint, fadeAt(e.percent, d)));
        };
        for (int d = 1; d <= glow; ++d) {
            if (left - d >= alongStart) side(left - d, firstEnd, d);
            if (right + d <= alongEnd) side(right + d - 1, lastEnd, d);
        }
    }
    fills.Flush();
    if (mood && !live) {
        // The state the mood colours can't carry: the text colour while playing, else amber or red.
        bool warn = g_lib[g.playing].Failed() || g.stopAfterCurrent;
        float w = std::max(2.0f, S(2)), at = playedX - std::floor(w / 2);
        float bandHeight = reflect ? 2 * stripeHalf + 1 : stripeHalf + 1;
        Fill(vertical ? Rect{cy - stripeHalf, at, bandHeight, w} : Rect{at, cy - stripeHalf, w, bandHeight},
             warn || IsPaused() ? played : p.text);
    }
    return true;
}

// The Mood column: the track's moodbar as a stripe in the Moodbar colours scheme, from ListPeaks'
// memory so a paint never decodes; the cell stays empty until the track is known (OnPeaksReady).
void DrawMoodCell(const Rect& cell, TrackId id, const Palette& p) {
    std::wstring path;
    double start, end;
    if (!PeakSource(id, path, start, end)) return;
    const Track& t = g_lib[id];
    static Peaks peaks;
    if (!ListPeaks(path, t.fileSize, t.fileTime, start, end, peaks)) return;
    const Rect r{cell.x - S(2), cell.y + S(3), cell.w + S(4), cell.h - S(6)};
    const int w = (int)r.w;
    if (w <= 0 || r.h <= 0) return;
    static MoodPalette moods;
    const int scheme = std::clamp(g.settings.moodColors, 0, kMoodColorSchemes - 1);
    const Rgb base = MoodBase(p, p.bg);
    if (!moods.For(base, p.bg, scheme)) moods.Make(base, p.bg, scheme);
    const float center = MoodCenter(peaks);
    for (int x = 0; x < w; x += 2) {
        int first = std::clamp(x * kPeakBuckets / w, 0, kPeakBuckets - 1);
        int last = std::clamp((x + 2) * kPeakBuckets / w, first + 1, kPeakBuckets);
        MoodTone tone = MoodOf(peaks, first, last, center);
        if (tone.level > 0) Fill({r.x + x, r.y, (float)std::min(2, w - x), r.h}, moods.Pick(tone, true));
    }
}

void DrawSection(const Rect& r, double duration, const Palette& p) {
    if (g.playing == kNone || duration <= 0) return;
    PlaybackSection section = SectionOf(g.playing);
    if (section.end - section.start <= 0.05) return;
    bool vertical = SeekVertical();
    float left = (vertical ? r.y : r.x) + S(8);
    float width = std::max(0.0f, (vertical ? r.h : r.w) - S(16));
    float first = left + width * (float)std::clamp(section.start / duration, 0.0, 1.0);
    float last = left + width * (float)std::clamp(section.end / duration, 0.0, 1.0);
    if (last <= first) return;
    if (vertical) {
        Fill({r.x + S(2), first, r.w - S(4), last - first}, p.accent, 45);
        Fill({r.x + S(2), first, r.w - S(4), S(2)}, p.accent);
        Fill({r.x + S(2), last - S(2), r.w - S(4), S(2)}, p.accent);
        return;
    }
    Fill({first, r.y + S(2), last - first, r.h - S(4)}, p.accent, 45);
    Fill({first, r.y + S(2), std::max(1.0f, S(2)), r.h - S(4)}, p.accent);
    Fill({last - std::max(1.0f, S(2)), r.y + S(2), std::max(1.0f, S(2)), r.h - S(4)}, p.accent);
}

double TrackDuration() {
    double dur = g.playing != kNone ? g.engine.Duration() : 0;
    if (dur <= 0 && g.playing != kNone) dur = g_lib[g.playing].Duration();
    return dur;
}

// The palette the player draws with: in the normal window its surface is the player's own
// background, washed with the cover's hue (PlayerSurface; app_playback.cpp, RefreshPlayerTint).
Palette PlayerPalette(const Palette& base) {
    Palette p = base;
    TrackId shown = g.playing != kNone ? g.playing : g.settings.resumeOn ? g.resumeTrack : kNone;
    if (g.settings.size == ViewSize::Normal)
        p.surface = PlayerSurface(base, g_lib.Contains(shown) && !RadioPlaying() ? g_coverHue : -1);
    return p;
}

// The seek bar alone: the live waveform/fountain, or the regular slider. It fills its own background
// first, so a partial frame repaints every pixel in this rectangle and leaves the rest untouched.
void DrawLiveSeek(const Palette& base) {
    if (L.seek.w <= 0) return;
    const Palette p = PlayerPalette(base);
    bool live = RadioPlaying(), playing = g.playing != kNone || live;
    double dur = TrackDuration();
    double pos = playing ? (g_seekPreview >= 0 ? g_seekPreview : g.engine.Position()) : 0;
    Fill(L.seek, p.surface);
    bool seekHot = g_popups.empty() && (L.seek.In(g_mouseX, g_mouseY) || g_drag == Drag::Seek);
    double frac = dur > 0 ? std::clamp(pos / dur, 0.0, 1.0) : 0;
    double notch = !live && playing && dur > 0 ? ui::SeekNotchSeconds(dur) / dur : 0;
    if (!DrawWaveform(L.seek, frac, p)) DrawSlider(L.seek, frac, !live && playing && dur > 0, seekHot, p, SeekVertical(), notch);
    DrawSection(L.seek, dur, p);
}

void DrawBand(const Palette& base) {
    const Palette p = PlayerPalette(base);
    Fill(L.player, p.surface);
    // The handle: a few dots.
    float dot = std::max(2.0f, S(2)), step = dot + std::max(2.0f, S(2));
    float gx = std::round(L.grip.x + (L.grip.w - (2 * step - (step - dot))) / 2), gy = std::round(L.grip.y + (L.grip.h - (3 * step - (step - dot))) / 2);
    for (int i = 0; L.grip.w > 0 && L.grip.h > 0 && i < 2; ++i)
        for (int j = 0; j < 3; ++j) Fill({gx + i * step, gy + j * step, dot, dot}, p.faint);
    bool live = RadioPlaying(), playing = g.playing != kNone || live;
    DrawBandButton(L.buttons[kBtnPrev], Icon::Prev, p);
    DrawBandButton(L.buttons[kBtnPlay], playing && !IsPaused() ? Icon::Pause : Icon::Play, p);
    DrawBandButton(L.buttons[kBtnNext], Icon::Next, p);
    DrawBandButton(L.buttons[kBtnShuffle], Icon::Shuffle, p, g.settings.shuffle);
    DrawBandButton(L.buttons[kBtnRepeat], g.settings.repeat == Repeat::One ? Icon::RepeatOne : Icon::Repeat, p, g.settings.repeat != Repeat::Off);
    DrawBandButton(L.buttons[kBtnResume], Icon::Resume, p, g.settings.resumeOn);
    // The times read toward the seek bar: elapsed on the right of its box, remaining on the left.
    double dur = TrackDuration();
    double pos = playing ? (g_seekPreview >= 0 ? g_seekPreview : g.engine.Position()) : 0;
    if (playing && L.elapsed.w > 0) {
        DrawText(Font::Normal, live ? "LIVE" : Utf8(FormatDuration(pos)), L.elapsed.x, TextY(L.elapsed), p.text, L.elapsed.w, 2);
        if (dur > 0) DrawText(Font::Normal, (g.settings.timeRemaining ? "-" + Utf8(FormatDuration(std::max(0.0, dur - pos))) : Utf8(FormatDuration(dur))), L.remaining.x, TextY(L.remaining), p.text, L.remaining.w, 0);
    }
    DrawLiveSeek(p);
    if (L.volume.w > 0) {
        DrawIcon(Icon::Speaker, L.speaker, p.text);
        bool volumeHot = g_popups.empty() && (L.volume.In(g_mouseX, g_mouseY) || g_drag == Drag::Volume);
        DrawSlider(L.volume, g.settings.volume, true, volumeHot, p);
    }
    if (L.cover.w <= 0) return;  // the now playing line is hidden

    // The now playing line: the cover, the title in bold over the artist line.
    Fill(L.cover, p.surface2);
    TrackId shown = playing ? g.playing : g.settings.resumeOn ? g.resumeTrack : kNone;
    if (!live && g_lib.Contains(shown) && g_cover && g_coverFor == shown) {
        SDL_FRect dst{L.cover.x, L.cover.y, L.cover.w, L.cover.h};
        // Disc art peeks out from behind the cover, which moves left and shrinks to make room.
        if (int side = DiscCoverSide((int)L.cover.w); side && g_disc) {
            Fill(L.cover, p.surface);  // the player's own background, under the disc's corners
            float c = (float)side, top = L.cover.y + (L.cover.h - c) / 2;
            float tw = 0, th = 0;
            SDL_GetTextureSize(g_disc, &tw, &th);
            float w = tw >= th ? c : c * tw / th, h = th >= tw ? c : c * th / tw;
            SDL_FRect disc{L.cover.R() - c + (c - w) / 2, top + (c - h) / 2, w, h};
            SDL_RenderTexture(g.renderer, g_disc, nullptr, &disc);
            dst = {L.cover.x, top, c, c};
        }
        SDL_RenderTexture(g.renderer, g_cover, nullptr, &dst);
    }
    std::string title, sub;
    if (live) {
        title = InternetRadio().playing.name;
        sub = InternetRadio().stream ? InternetRadio().stream->Title() : std::string();
        if (sub.empty()) sub = "Live radio";
    } else if (playing) {
        auto lookup = Lookup(g.playing, true);
        title = Utf8(FormatTitle(EffectiveFormat(kPackNowPlaying, g.settings.fmtTitle), lookup));
        sub = Utf8(FormatTitle(EffectiveFormat(kPackNowPlaying2, g.settings.fmtSub), lookup));
        if (g.chapterTrack == g.playing) {
            std::wstring chapter = ChapterTextAt(g.chapters, pos);
            if (!chapter.empty()) sub += (sub.empty() ? "" : "  \xE2\x80\xA2  ") + Utf8(chapter);
        }
    } else if (g_lib.Contains(shown)) {
        auto lookup = Lookup(shown, false);
        title = "Continue: " + Utf8(FormatTitle(L"%title%", lookup));
        sub = Utf8(FormatTitle(L"[%artist%][  \x2022  %album%]", lookup));
        if (!sub.empty()) sub += "  \xE2\x80\xA2  ";
        sub += Utf8(FormatDuration(g.resumePosition)) + " / " + Utf8(FormatDuration(g_lib[shown].Duration()));
    } else {
        title = "cordinal";
        if (Active().kind == PlaylistKind::Library && g.settings.folders.empty() && g.rows.empty())
            sub = "Your library is empty: add your music folder with Library \xE2\x96\xB8 Library folders, or drop folders here.";
    }
    DrawText(Font::Bold, title, L.title.x, TextY(L.title, Font::Bold), p.text, L.title.w);
    if (g.settings.layout.hints && !playing) sub += "  Space: play / pause";
    DrawText(Font::Normal, sub, L.subtitle.x, TextY(L.subtitle), p.sub, L.subtitle.w);
    if (L.chip.w > 0) DrawPointsChip(L.chip, p);
}

int PlayingTab() { return g.playing != kNone ? NowPlayingIndex() : -1; }

// The tab strip (app_tabstrip.cpp): flat tabs on the surface, the open one underlined in the accent,
// the playing playlist's name in the playing color, a close box on the open and hovered tabs.
void DrawTabs(const Palette& p) {
    if (L.tabs.h <= 0) return;
    Fill(L.tabs, p.surface);
    Fill({L.tabs.R(), L.tabs.y, std::max(0.f, L.search.R() - L.tabs.R()), L.tabs.h}, p.surface);
    Fill({L.tabs.x, L.tabs.B() - S(1), L.tabs.w, S(1)}, p.border);  // the rule the open tab's underline sits on
    SDL_Rect clip{(int)L.tabs.x, (int)L.tabs.y, (int)L.tabs.w, (int)L.tabs.h};
    SDL_SetRenderClipRect(g.renderer, &clip);
    for (size_t i = 0; i < L.tabRects.size(); ++i) {
        Rect r = L.tabRects[i];
        if (r.x >= L.tabs.R()) break;
        bool sel = !InternetRadio().active && (int)i == g.active, hover = g_popups.empty() && r.In(g_mouseX, g_mouseY);
        bool closable = g.playlists[i].kind == PlaylistKind::Static || g.playlists[i].kind == PlaylistKind::Auto ||
                        g.playlists[i].kind == PlaylistKind::NowPlaying;
        if (sel) Fill({r.x + S(10), r.B() - S(2), r.w - S(20), S(2)}, p.accent);
        // The label keeps its place whether or not the close box shows over its end.
        float left = r.x + S(6), right = r.R() - S(6);
        if ((int)i == g_renameIndex) {
            Rect field{left, r.y + S(2), std::max(S(1), right - left), r.h - S(4)};
            Fill(field, g_renameSelected ? p.select : p.bg);
            Frame(field, p.accent);
            DrawText(Font::Normal, Utf8(g_renameText), field.x + S(3), TextY(field),
                     g_renameSelected ? p.selectText : p.text, field.w - S(6));
            if (!g_renameSelected) {
                float caret = field.x + S(3) + TextWidth(Font::Normal, Utf8(g_renameText.substr(0, g_renameCaret)));
                Fill({std::min(caret, field.R() - S(2)), field.y + S(3), std::max(1.f, S(1)), field.h - S(6)}, p.text);
            }
        } else {
            std::string name = TabName(i);
            float w = TextWidth(Font::Normal, name), x = std::max(left, std::round((left + right - w) / 2));
            uint32_t color = (int)i == PlayingTab() ? p.playing : sel || hover ? p.text : p.sub;
            DrawText(Font::Normal, name, x, TextY(r), color, right - x);
        }
        if (closable && hover && (int)i != g_renameIndex) {  // only under the mouse; Ctrl+W and a middle click close too
            Rect close{r.R() - S(22), r.y + S(3), S(19), r.h - S(6)};
            Fill(close, p.surface);
            DrawText(Font::Normal, "\xC3\x97", close.x, TextY(close), p.text, close.w, 1);
        }
    }
    SDL_SetRenderClipRect(g.renderer, nullptr);
    if (L.radioTab.w > 0) {
        Fill(L.radioTab, p.surface);
        Fill({L.radioTab.x, L.radioTab.B() - S(1), L.radioTab.w, S(1)}, p.border);
        if (InternetRadio().active) Fill({L.radioTab.x + S(8), L.radioTab.B() - S(2), L.radioTab.w - S(16), S(2)}, p.accent);
        DrawText(Font::Normal, "Radio", L.radioTab.x, TextY(L.radioTab),
                 RadioPlaying() ? p.playing : InternetRadio().active ? p.text : p.sub, L.radioTab.w - S(22), 1);
        Rect close{L.radioTab.R() - S(22), L.radioTab.y + S(3), S(19), L.radioTab.h - S(6)};
        bool hover = g_popups.empty() && close.In(g_mouseX, g_mouseY);
        if (hover) Fill(close, p.hover);
        if (g_popups.empty() && L.radioTab.In(g_mouseX, g_mouseY))  // the × only while over the tab
            DrawText(Font::Normal, "\xC3\x97", close.x, TextY(close), hover ? p.text : p.sub, close.w, 1);
    }
    // The search box: a field with Windows 11's darker bottom edge, and its cue while empty.
    if (L.search.w > 0) {
    Fill(L.search, p.surface2);
    Frame(L.search, g.searchFocused ? p.accent : p.border);
    Fill({L.search.x + S(1), L.search.B() - S(1), L.search.w - S(2), S(1)}, g.searchFocused ? p.accent : p.dark ? p.sub : 0x838383);
    float tx = L.search.x + S(4), ty = TextY(L.search);
    std::string cue = InternetRadio().active ? "Name, call sign or FM/AM number" : Active().kind == PlaylistKind::Library ? "Search library (Ctrl+F)" : "Search " + Utf8(Active().name) + " (Ctrl+F)";
    if (g.search.empty()) DrawText(Font::Normal, cue, tx, ty, p.sub, L.search.w - S(8));
    float w = DrawText(Font::Normal, Utf8(g.search), tx, ty, p.text, L.search.R() - tx - S(4));
    if (g.searchFocused && (SDL_GetTicks() / 530) % 2 == 0) Fill({tx + w + 1, ty, std::max(1.0f, S(1)), LineHeight(Font::Normal)}, p.text);
    }
    // The Library's view row: the current view (and the shown browser) in a square raised box, so it
    // never reads as another tab; hovering brightens a label.
    if (L.viewRow.h > 0) {
        Fill(L.viewRow, p.surface);
        for (int i = 0; i < kViewButtons; ++i) {
            const Rect& v = L.views[i];
            bool on = i < (int)LibraryMode::Count ? (int)g.settings.libraryMode == i : g.settings.browserVisible;
            if (on) {
                Fill(v, p.surface2);
                Frame(v, p.border);
            }
            DrawText(Font::Normal, ViewButtonLabel(i), v.x, TextY(v), on || v.In(g_mouseX, g_mouseY) ? p.text : p.sub, v.w, 1);
        }
    }
}

int RadioColumns() { return std::clamp((int)((L.radioGrid.w - S(12)) / S(250)), 1, 3); }

float RadioMaxScroll() {
    int columns = RadioColumns();
    return std::max(0.0f, (float)((InternetRadio().rows.size() + columns - 1) / columns) * S(82) - L.radioGrid.h);
}

Rect RadioCard(int index) {
    int columns = RadioColumns(); float gap = S(6), pad = S(6);
    float width = std::max(1.0f, (L.radioGrid.w - 2 * pad - gap * (columns - 1)) / columns);
    return {L.radioGrid.x + pad + (index % columns) * (width + gap), L.radioGrid.y + (index / columns) * S(82) - InternetRadio().scroll,
            width, S(76)};
}

void DrawRadio(const Palette& p) {
    auto& radio = InternetRadio();
    Fill(L.list, p.bg);
    Fill({L.list.x, L.list.y, L.list.w, S(68)}, p.surface);
    auto button = [&](const Rect& r, const char* label, bool selected) {
        if (r.In(g_mouseX, g_mouseY)) Fill(r, p.hover);
        if (selected) Fill({r.x + S(6), r.B() - S(2), r.w - S(12), S(2)}, p.accent);
        DrawText(selected ? Font::Bold : Font::Normal, label, r.x, TextY(r), p.text, r.w, 1);
    };
    button(L.radioSaved, "My stations", !radio.discover);
    button(L.radioDiscover, "Discover", radio.discover);
    auto field = [&](const Rect& r, const std::wstring& value, const char* hint, int focus) {
        Fill(r, p.surface2); Frame(r, radio.filterFocus == focus ? p.accent : p.border);
        DrawText(Font::Normal, value.empty() ? hint : Utf8(value), r.x + S(4), TextY(r),
                 value.empty() ? p.sub : p.text, r.w - S(8));
    };
    field(L.radioGenre, radio.genre, "Genre (e.g. indie)", 1);
    field(L.radioCountry, radio.country, "Country (blank = all)", 2);
    field(L.radioState, radio.region, "State (e.g. Indiana)", 3);
    button(L.radioSearch, "Search", false);
    radio.scroll = std::clamp(radio.scroll, 0.0f, RadioMaxScroll());
    SDL_Rect clip{(int)L.radioGrid.x, (int)L.radioGrid.y, (int)L.radioGrid.w, (int)L.radioGrid.h};
    SDL_SetRenderClipRect(g.renderer, &clip);
    if (radio.rows.empty()) {
        std::wstring message = radio.discover && radio.browser && radio.browser->Searching() ? L"Finding stations..." :
            radio.browser && !radio.browser->Problem().empty() ? radio.browser->Problem() :
            radio.discover ? L"No stations found. Try another name, genre or country." :
                             L"No saved stations yet. Choose Discover to find stations.";
        DrawText(Font::Normal, Utf8(message), S(16), L.radioGrid.y + S(24), p.sub, L.radioGrid.w - S(32));
    }
    for (int i = 0; i < (int)radio.rows.size(); ++i) {
        Rect r = RadioCard(i);
        if (r.B() <= L.radioGrid.y || r.y >= L.radioGrid.B()) continue;
        const auto& station = radio.rows[(size_t)i];
        bool active = radio.token && station.id == radio.playing.id;
        bool selected = i == radio.selected && !g.searchFocused && !radio.filterFocus;
        Fill(r, selected ? p.select : p.surface);
        if (active) Fill({r.x, r.y, S(3), r.h}, p.accent);
        Rect art{r.x + S(8), r.y + S(8), S(50), S(50)};
        SDL_Texture* texture = nullptr;
        if (radio.browser && !station.icon.empty()) {
            radio.browser->RequestIcon(station.icon);
            if (const std::string* bytes = radio.browser->Icon(station.icon)) {
                auto found = g_radioArt.find(station.icon);
                if (found == g_radioArt.end()) {
                    if (g_radioArt.size() >= 100) {
                        auto oldest = g_radioArt.begin();
                        if (oldest->second) SDL_DestroyTexture(oldest->second);
                        g_radioArt.erase(oldest);
                    }
                    CoverImage image{}; SDL_Texture* created = nullptr;
                    if (DecodeRadioIcon(*bytes, (int)S(64), image)) {
                        created = SDL_CreateTexture(g.renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, image.width, image.height);
                        if (created) { SDL_UpdateTexture(created, nullptr, image.rgba.data(), image.width * 4); SDL_SetTextureBlendMode(created, SDL_BLENDMODE_BLEND); }
                    }
                    found = g_radioArt.emplace(station.icon, created).first;
                }
                texture = found->second;
            }
        }
        if (texture) {
            SDL_FRect dest{art.x, art.y, art.w, art.h}; SDL_RenderTexture(g.renderer, texture, nullptr, &dest);
        } else {
            Fill(art, p.surface2);
            std::wstring name = FromUtf8(station.name), initial;
            for (wchar_t c : name) if (iswalnum(c)) { initial.assign(1, (wchar_t)towupper(c)); break; }
            if (initial.empty()) initial = L"\x266B";
            DrawText(Font::Bold, Utf8(initial), art.x, TextY(art, Font::Bold), p.sub, art.w, 1);
        }
        Frame(art, p.border);
        bool favorite = radio.browser && radio.browser->Favorite(station);
        Rect star{r.R() - S(30), r.y + S(4), S(26), S(26)};
        DrawText(Font::Normal, favorite ? "\xE2\x98\x85" : "\xE2\x98\x86", star.x, TextY(star), selected ? p.selectText : favorite ? p.accent : p.sub, star.w, 1);
        float x = art.R() + S(8), width = std::max(0.0f, star.x - x - S(2));
        DrawText(Font::Bold, station.name, x, r.y + S(7), selected ? p.selectText : p.text, width);
        std::string tags = station.tags.substr(0, station.tags.find(','));
        std::string location = station.country + (station.country.empty() || station.state.empty() ? "" : " / ") + station.state;
        std::string detail = location + (location.empty() || tags.empty() ? "" : "  \xE2\x80\xA2  ") + tags;
        DrawText(Font::Normal, detail, x, r.y + S(27), selected ? p.selectText : p.sub, width);
        std::string format = station.codec;
        if (station.bitrate) format += (format.empty() ? "" : "  \xC2\xB7  ") + std::to_string(station.bitrate) + " kbps";
        if (station.hls) format += (format.empty() ? "" : "  \xC2\xB7  ") + std::string("HLS");
        DrawText(Font::Normal, format, x, r.y + S(45), selected ? p.selectText : p.sub, width);
    }
    SDL_SetRenderClipRect(g.renderer, nullptr);
}

void DrawVisuals(const Palette& p) {
    if (g.settings.theme != ThemeMode::Foobar && g.settings.theme != ThemeMode::Apollo) return;
    uint64_t now = SDL_GetTicks();
    if (now - g_visualAt >= ui::kFoobarFrameMs) {
        bool active = g.playing != kNone && !IsPaused();
        unsigned rate = g.engine.Rate();
        size_t frames = g_visuals.Prepare(rate);
        if (active) active = g.engine.VisualSamples(g_visuals.wave.data(), frames, g_visuals.time) == frames;
        if (g.playing == kNone || !IsPaused())
            g_visuals.Update(active, rate, g_visualAt ? (now - g_visualAt) / 1000.0 : 0.016);
        g_visualAt = now;
    }
    for (Rect r : L.visuals) { Fill(r, p.bg); Frame(r, p.border); }
    Rect bars = L.visuals[0], scope = L.visuals[1];
    for (size_t i = 0; i < g_visuals.levels.size(); ++i) {
        float w = (bars.w - S(4)) / g_visuals.levels.size(), h = (bars.h - S(4)) * g_visuals.levels[i];
        if (h > 0) Fill({bars.x + S(2) + i * w, bars.B() - S(2) - h, std::max(1.0f, w - S(1)), h}, p.text);
        float peak = (bars.h - S(4)) * g_visuals.peaks[i];
        if (peak > h) Fill({bars.x + S(2) + i * w, bars.B() - S(2) - peak, std::max(1.0f, w - S(1)), S(1)}, Mix(p.bg, p.text, 50));
    }
    int w = (int)(scope.w - S(4));
    if (w <= 1) return;
    const auto curve = g_visuals.Curve(w);
    std::vector<SDL_FPoint> line(w);
    for (int x = 0; x < w; ++x) {
        line[x] = {scope.x + S(2) + x + 0.5f, scope.y + S(2) + (scope.h - S(4)) * (0.5f - curve[x] * 0.5f)};
    }
    // SDL lines have hard edges. A transparent fringe gives this one-DIP trace the same smooth
    // profile as Windows' Direct2D trace, including fractional sample heights.
    std::vector<SDL_Vertex> vertices;
    vertices.reserve((w - 1) * 18);
    float inner = std::max(0.0f, (S(1) - 1) * 0.5f), outer = inner + 1;
    for (int x = 1; x < w; ++x) {
        SDL_FPoint a = line[x - 1], b = line[x];
        float dy = b.y - a.y, length = std::hypot(1.0f, dy), nx = -dy / length, ny = 1 / length;
        float offsets[] = {-outer, -inner, inner, outer}, alpha[] = {0, 1, 1, 0};
        SDL_Vertex edge[8];
        for (int i = 0; i < 4; ++i) {
            edge[i * 2] = {{a.x + nx * offsets[i], a.y + ny * offsets[i]}, FColor(p.text, alpha[i]), {0, 0}};
            edge[i * 2 + 1] = {{b.x + nx * offsets[i], b.y + ny * offsets[i]}, FColor(p.text, alpha[i]), {0, 0}};
        }
        for (int i = 0; i < 3; ++i) {
            int at = i * 2;
            for (int k : {at, at + 1, at + 2, at + 1, at + 3, at + 2}) vertices.push_back(edge[k]);
        }
    }
    SDL_Rect clip{(int)(scope.x + S(2)), (int)(scope.y + S(2)), w, (int)(scope.h - S(4))};
    SDL_SetRenderClipRect(g.renderer, &clip);
    SDL_RenderGeometry(g.renderer, nullptr, vertices.data(), (int)vertices.size(), nullptr, 0);
    SDL_SetRenderClipRect(g.renderer, nullptr);
}

// An album header's cover: the decoded picture, or while it loads the surface color, or for an
// album without one a tile in a color of its own with the album's initial (app_ui.cpp).
void DrawGroupCover(const Group& gr, const Rect& r, const Palette& p) {
    DrawAlbumCover(g.rows[gr.first], gr.album, g_lib.Str(gr.album), r, p);
}

void DrawGroupHeader(const Group& gr, int index, float top, float right, const Palette& p) {
    float h = L.rowH;
    if (index > 0) Fill({L.rows.x, top, right - L.rows.x, std::max(1.0f, S(1) / 2)},
                        g.settings.theme == ThemeMode::Foobar ? Mix(p.bg, p.text, 50) : p.border);
    float size = 2 * h - S(8);
    Rect cover{L.rows.x + S(8), top + S(5), size, size};
    DrawGroupCover(gr, cover, p);
    float x = cover.R() + S(10), r = right - S(10);
    std::wstring artist = gr.artist ? g_lib.Str(gr.artist) : L"Various artists";
    std::wstring year = g_lib.Str(gr.date).substr(0, 4);
    std::wstring edition = TrackEditionLabel(g.rows[gr.first]);
    if (!edition.empty()) artist += L"  \x00B7  " + edition;
    std::string stats = std::to_string(gr.count) + " tracks  \xC2\xB7  " + Utf8(FormatDuration(gr.seconds));
    Rect line1{x, top + S(3), r - x, h - S(2)}, line2{x, top + h - S(1), r - x, h - S(2)};
    float sw = TextWidth(Font::Normal, stats);
    DrawText(Font::Normal, stats, std::max(x, r - sw), TextY(line1), p.sub);
    DrawText(Font::Bold, Utf8(g_lib.Str(gr.album)), x, TextY(line1, Font::Bold), p.text, std::max(0.0f, r - sw - S(12) - x));
    DrawText(Font::Normal, Utf8(year.empty() ? artist : artist + L"  \x00B7  " + year), x, TextY(line2), p.sub, r - x);
}

// The list (app_playlists.cpp, DrawTrackRow; app_ui.cpp, HeaderSubclass): the header on the list's
// own background with quiet labels and short dividers, then the rows: selection, hover or nothing
// behind them, and each column's text with the album header's parts left out.
void DrawList(const Palette& p) {
    SDL_Rect listClip{(int)L.list.x, (int)L.list.y, (int)L.list.w, (int)L.list.h};
    SDL_SetRenderClipRect(g.renderer, &listClip);
    Fill(L.list, p.bg);
    std::vector<float> xs;
    float x = L.rows.x;
    for (int c : ActiveColumnTypes()) {
        xs.push_back(x);
        x += ColumnWidth(c);
    }
    float total = x;
    const Playlist& pl = Active();
    uint32_t divider = Mix(p.bg, p.border, 50);
    for (size_t i = 0; i < xs.size(); ++i) {
        int c = ActiveColumnTypes()[i];
        float w = ColumnWidth(c);
        Fill({xs[i] + w - 1, L.header.y + S(6), 1, L.header.h - S(12)}, divider);
        bool sorted = pl.sortColumn == c;
        Rect tr{xs[i] + S(5), L.header.y, w - S(10) - (sorted ? S(14) : 0), L.header.h};
        std::string name = Utf8(ActiveColumnHeading(c));
        DrawText(Font::Normal, name, tr.x, TextY(tr), sorted ? p.text : p.sub, tr.w, ColumnAt(c).align == ui::kAlignRight ? 2 : 0);
        if (sorted) {
            float cx = xs[i] + w - S(12), cy = L.header.y + L.header.h / 2;
            if (pl.sortAscending) Polygon({{cx - S(4), cy + S(2)}, {cx, cy - S(3)}, {cx + S(4), cy + S(2)}}, p.text);
            else Polygon({{cx - S(4), cy - S(2)}, {cx + S(4), cy - S(2)}, {cx, cy + S(3)}}, p.text);
        }
    }
    Fill({L.header.x, L.header.B() - S(1), L.header.w, S(1)}, p.border);

    SDL_Rect clip{(int)L.rows.x, (int)L.rows.y, (int)L.rows.w, (int)L.rows.h};
    SDL_SetRenderClipRect(g.renderer, &clip);
    int first = (int)(g.scroll / L.rowH), last = std::min((int)g_lines.size(), first + (int)(L.rows.h / L.rowH) + 2);
    int hover = g_popups.empty() ? LineAt(g_mouseY) : -1;
    bool npTab = pl.kind == PlaylistKind::NowPlaying;
    const char* mark = g.playing != kNone && IsPaused() ? "\xE2\x9D\x99\xE2\x9D\x99" : nullptr;
    std::string playingMark = Utf8(ThemeInfo(g.settings.theme).playingMark);
    for (int line = std::max(0, first - 1); line < last; ++line) {
        const Line& ln = g_lines[line];
        float y = L.rows.y + line * L.rowH - g.scroll;
        if (ln.row < 0) {
            if (ln.part == 0) DrawGroupHeader(g_groups[ln.group], ln.group, y, std::min(total, L.rows.R()), p);
            continue;
        }
        TrackId id = g.rows[ln.row];
        Rect r{L.rows.x, y, total - L.rows.x, L.rowH};
        bool sel = ln.row < (int)g.selected.size() && g.selected[ln.row];
        if (sel) Fill(r, p.select);
        else if (line == hover) Fill(r, p.hover);
        else if (line % 2) Fill(r, RowStripe(p));  // every other row a shade off (DrawTrackRow)
        bool isPlaying = g.playing == id && npTab && ln.row < (int)g.rowItems.size() && g.rowItems[ln.row] == (size_t)g.queuePos;
        const Track& t = g_lib[id];
        uint32_t color = sel ? p.selectText : isPlaying ? p.playing : (t.flags & (Track::kMissing | Track::kFailed)) ? p.faint : p.text;
        Font font = isPlaying ? Font::Bold : Font::Normal;
        if (isPlaying && g.settings.theme == ThemeMode::Foobar) {
            Fill(r, p.playing);
            color = TextOn(p.playing);
            font = Font::Normal;
        } else if (isPlaying) {
            // The playing row underlines how far the track has got (app_playlists.cpp, DrawTrackRow).
            Fill({r.x, r.B() - S(2), (float)ui::PlayedWidth((int)r.w, g.engine.Position(), TrackDuration()), S(2)}, p.playing);
        }
        for (size_t i = 0; i < xs.size(); ++i) {
            int c = ActiveColumnTypes()[i];
            float w = ColumnWidth(c);
            Rect cell{xs[i] + S(6), y, w - S(12), L.rowH};
            if (DrawAlternateCell(id, c, cell, font, p, ln.row)) continue;
            if (c == 0) {
                if (isPlaying) DrawText(font, mark ? mark : playingMark, cell.x, TextY(cell, font), color, cell.w);
                else if (int queued = QueuePosition(id)) DrawText(font, "[" + std::to_string(queued) + "]", cell.x, TextY(cell, font), color, cell.w);
                continue;
            }
            if (c == ui::kColMood) {
                DrawMoodCell(cell, id, p);
                continue;
            }
            if (c == ui::kColRating) {
                // Five stars: the rating's in the accent over the rest in the faint color; an unrated
                // track's only while its row is hovered or selected.
                if (!ui::ShowEmptyStars(t.rating, line == hover, sel)) continue;
                std::string star = "\xE2\x98\x85";
                float sw = TextWidth(Font::Normal, star), sx = xs[i] + S(4);
                for (int k = 0; k < 5; ++k, sx += sw) {
                    int fill = std::clamp((int)t.rating - k * 20, 0, 20);
                    DrawText(Font::Normal, star, sx, TextY(cell), sel ? (fill ? p.selectText : p.sub) : fill ? p.accent : p.faint);
                }
                continue;
            }
            if (GroupHidesCell(ln, c)) continue;
            const std::string& text = Cell(id, c);
            if (text.empty()) continue;
            bool right = ColumnAt(c).align == ui::kAlignRight;
            if (!right) cell.w = CellTextWidth(ln, i, cell.w);
            DrawText(font, text, cell.x, TextY(cell, font), color, cell.w, right ? 2 : 0);
        }
    }
    if (g_drag == Drag::Tracks && g_trackDrag.target >= 0 && !g_lineOfRow.empty()) {
        int row = std::min(g_trackDrag.target, (int)g_lineOfRow.size() - 1);
        float y = L.rows.y + (g_lineOfRow[row] + (g_trackDrag.target >= (int)g_lineOfRow.size() ? 1 : 0)) * L.rowH - g.scroll;
        Fill({L.rows.x, std::clamp(y - S(1), L.rows.y, L.rows.B() - S(2)), L.rows.w, S(2)}, p.accent);
    }
    SDL_SetRenderClipRect(g.renderer, nullptr);

    if (g.rows.empty()) {
        std::string hint = !g.search.empty()                                                  ? "No tracks match the search."
                           : pl.kind == PlaylistKind::Library && g.settings.folders.empty() ? "Your library is empty: add your music folder with Library \xE2\x96\xB8 Library folders, or drop folders here."
                           : pl.kind == PlaylistKind::Library && g.scanning                 ? "Looking for music..."
                           : pl.kind == PlaylistKind::NowPlaying                            ? "Drop music here, or press Ctrl+O to add files."
                                                                                            : "Nothing here yet.";
        DrawText(Font::Normal, hint, L.rows.x, L.rows.y + S(40), p.sub, L.rows.w, 1);
    }
    // Windows 11's overlay scroll bar at rest: a slim thumb, wider with arrows while hovered.
    if (L.scrollbar.w > 0) {
        Fill(L.scrollbar, p.bg);
        bool hot = L.scrollbar.In(g_mouseX, g_mouseY) || g_drag == Drag::Scroll;
        float thumbH = std::max(S(24), L.scrollbar.h * L.rows.h / std::max(1.0f, ContentHeight()));
        float room = L.scrollbar.h - S(hot ? 34 : 4) - thumbH;
        float thumbY = L.scrollbar.y + S(hot ? 17 : 2) + room * (MaxScroll() > 0 ? g.scroll / MaxScroll() : 0);
        float tw = hot ? S(6) : S(2);
        FillRound({std::round(L.scrollbar.x + L.scrollbar.w / 2 - tw / 2), thumbY, tw, thumbH}, tw / 2, hot ? p.sub : p.faint);
        if (hot) {
            float cx = L.scrollbar.x + L.scrollbar.w / 2;
            Polygon({{cx - S(4), L.scrollbar.y + S(10)}, {cx, L.scrollbar.y + S(6)}, {cx + S(4), L.scrollbar.y + S(10)}}, p.sub);
            Polygon({{cx - S(4), L.scrollbar.B() - S(10)}, {cx + S(4), L.scrollbar.B() - S(10)}, {cx, L.scrollbar.B() - S(6)}}, p.sub);
        }
    }
}

std::wstring StatusMessage() {
    if (!g.message.empty() && SDL_GetTicks() < g.messageUntil) return ThemeInfo(g.settings.theme).messagePrefix + g.message;
    if (RadioPlaying()) return IsPaused() ? L"Radio paused." : L"Live radio: " + FromUtf8(InternetRadio().playing.name);
    if (g.playing == kNone) return L"Stopped.";
    std::wstring status = (IsPaused() ? L"Paused | " : L"") +
        FormatTitle(EffectiveFormat(kPackStatus, g.settings.fmtStatus), Lookup(g.playing, true));
    if (g.speed != 1 || g.tempo != 1) {
        wchar_t rate[64];
        if (g.speed != 1 && g.tempo != 1) swprintf(rate, std::size(rate), L" | %gx speed, %gx tempo", g.speed, g.tempo);
        else swprintf(rate, std::size(rate), L" | %gx %ls", g.speed != 1 ? g.speed : g.tempo, g.speed != 1 ? L"speed" : L"tempo");
        status += rate;
    }
    if (g.settings.layout.statusDetails) {
        const auto output = g.engine.Output();
        status += g.settings.eq.enabled ? L" | EQ" : L"";
        if (g.settings.eq.crossfeed) status += L" + crossfeed";
        if (g.settings.rg.on) status += L" | ReplayGain";
        if (output.ready) status += L" | " + std::wstring(output.exclusive ? L"Exclusive " : L"Shared ") +
            std::to_wstring(output.rate) + L" Hz";
    }
    if (g.stopAfterCurrent) status += L" | stop after current";
    if (!g.queue.empty()) status += L"  \x2022  " + std::to_wstring(g.queue.size()) + L" queued";
    // When a long track will be over, on the local clock (app_playback.cpp, EndsAtText).
    double dur = TrackDuration();
    if (dur >= ui::kLongTrackSeconds && !IsPaused() && g.settings.repeat != Repeat::One) {
        double left = std::max(0.0, dur - g.engine.Position()) / std::max(0.05, g.speed * g.tempo);
        time_t end = time(nullptr) + (time_t)left;
        tm local{};
        char clock[64];
        if (localtime_r(&end, &local) && strftime(clock, sizeof(clock), "%H:%M", &local))
            status += L"  \x2022  ends at " + FromUtf8(clock);
    }
    return status;
}

// The status bar (app_window.cpp, DrawMainItem): three parts on the surface split by hairlines:
// the message or what's playing, work in progress, and the count and length of the list or selection.
void DrawStatus(const Palette& p) {
    if (DrawAlternateStatus(p)) return;
    if (L.status.h <= 0) return;
    Fill(L.status, p.surface);
    float W = L.status.w;
    float parts[] = {0, std::max(S(100), W - S(ui::kStatusDetails)), std::max(S(200), W - S(ui::kStatusCounts)), W};
    std::wstring progress = TagWriteStatus();
    if (progress.empty() && g_checkingDead) progress = L"Checking for missing files...";
    if (progress.empty() && g_checkingDuplicates) progress = L"Finding duplicate audio...";
    if (progress.empty() && g.scanning) progress = L"Adding audio files...";
    if (progress.empty()) {
        std::lock_guard lock(g.tagMx);
        if (!g.tagQueue.empty()) progress = L"Reading metadata: " + std::to_wstring(g.tagQueue.size()) + L" remaining";
    }
    if (progress.empty()) progress = ReplayGainStatus();
    double total = 0;
    size_t sel = 0;
    for (size_t i = 0; i < g.rows.size(); ++i)
        if (i < g.selected.size() && g.selected[i]) {
            ++sel;
            total += g_lib[g.rows[i]].Duration();
        }
    std::wstring counts;
    if (InternetRadio().active) counts = std::to_wstring(InternetRadio().rows.size()) + L" stations";
    else if (sel) counts = std::to_wstring(sel) + L" selected  \x2022  " + FormatTotalDuration(total);
    else {
        for (TrackId id : g.rows) total += g_lib[id].Duration();
        counts = std::to_wstring(g.rows.size()) + (g.search.empty() ? L"" : L" of " + std::to_wstring(g_sourceCount)) +
                 L" tracks  \x2022  " + FormatTotalDuration(total);
    }
    std::wstring text[] = {StatusMessage(), progress, counts};
    for (int i = 0; i < 3; ++i) {
        Rect r{parts[i], L.status.y, parts[i + 1] - parts[i], L.status.h};
        if (i > 0) Fill({r.x, r.y + S(3), S(1), r.h - S(6)}, p.border);
        DrawText(Font::Normal, Utf8(text[i]), r.x + S(6), TextY(r), i == 0 ? p.text : p.sub, r.w - S(10));
    }
}

void DrawUpNext(const Palette& p) {
    if (L.upNext.w <= 0 || L.upNext.h <= 0) return;
    SDL_Rect clip{(int)L.upNext.x, (int)L.upNext.y, (int)L.upNext.w, (int)L.upNext.h};
    SDL_SetRenderClipRect(g.renderer, &clip);
    Fill(L.upNext, p.surface);
    Frame(L.upNext, p.border);
    DrawText(Font::Bold, "Up next", L.upNext.x + S(12), L.upNext.y + S(10), p.text, L.upNext.w - S(24));
    float y = L.upNext.y + S(38);
    const auto tracks = UpNextTracks();
    for (TrackId id : tracks) {
        if (y + S(38) > L.upNext.B()) break;
        DrawText(Font::Normal, Utf8(TrackTitle(id)), L.upNext.x + S(12), y, p.text, L.upNext.w - S(24));
        DrawText(Font::Small, Utf8(g_lib.Str(g_lib[id].artist)), L.upNext.x + S(12), y + S(16),
            p.sub, L.upNext.w - S(24));
        y += S(42);
    }
    if (tracks.empty()) DrawText(Font::Normal, "Nothing queued.", L.upNext.x + S(12), y, p.sub, L.upNext.w - S(24));
    SDL_SetRenderClipRect(g.renderer, nullptr);
}

void Draw() {
    ++g_frame;
    // The library changed (a scan, tags arriving): rebuild what's shown, at most twice a second.
    if (g.rowsRevision != g_lib.Revision() && SDL_GetTicks() - g_lastRebuild > 500) RebuildRows();
    if (g.chapterTrack != g.playing) QueueChapters(g.playing);
    TrackId shown = g.playing != kNone ? g.playing : g.settings.resumeOn ? g.resumeTrack : kNone;
    if (g_lib.Contains(shown) && g_coverWanted != shown) {
        g_coverWanted = shown;
        RequestCover(shown);
    }
    TrackId waveTrack = g.settings.waveform ? g.playing : kNone;
    if (waveTrack != g_wave.track) {  // a new track or display choice: ask only for peaks we will show
        if (waveTrack == kNone) CancelPeaks();
        if (!g.settings.waveform) CancelPeakPrefetch();
        g_wave.track = waveTrack;
        g_wave.request = 0;
        g_wave.ready = waveTrack != kNone && RequestPeaks(waveTrack, g_wave.peaks, g_wave.request);
    }
    UpdateWindowTitle();
    const Palette& p = CurrentPalette();
    if (MiniActive()) {
        DrawMini(p);
        return;
    }
    LyricsTick();
    DoLayout();
    ClampScroll();
    SetColor(p.bg);
    SDL_RenderClear(g.renderer);
    if (InternetRadio().active && L.list.w > 0 && L.list.h >= S(68)) DrawRadio(p);
    else if (CanvasActive() && L.list.w > 0 && L.list.h > 0) DrawCanvas(L.list, p, g_popups.empty() ? g_mouseX : -1, g_popups.empty() ? g_mouseY : -1);
    else if (L.list.w > 0 && L.list.h > 0) DrawList(p);
    if (L.browser.h > 0) {
        DrawBrowser(L.browser, p, g_popups.empty() ? g_mouseX : -1, g_popups.empty() ? g_mouseY : -1);
        Fill(L.splitter, p.surface);
    }
    if (L.lyrics.w > 0) DrawLyrics(L.lyrics, p);
    DrawTabs(p);
    DrawBigPanels(p);
    DrawUpNext(p);
    if (!DrawAlternate(p)) DrawBand(p);
    DrawVisuals(p);
    DrawStatus(p);
    DrawMenuBar(p);
    if (g_folders.open) DrawFoldersDialog(p);
    if (g_aboutOpen) DrawAbout(p);
    if (g_web.open) DrawWeb(p);
    if (PanelOpen()) DrawPanel(p, g_mouseX, g_mouseY);
    if (g_exclude.open) DrawExcludeConfirm(p);
    if (g.settings.layout.points) {
        StepPointsChip();
        DrawToast(L.chip, p);
    }
    DrawStartupReview(p, g_popups.empty() ? g_mouseX : -1, g_popups.empty() ? g_mouseY : -1);
    if (g_drag == Drag::Dock && std::hypot(g_mouseX - g_dragStart, g_mouseY - g_dragStartValue) >= S(4)) {
        float w = L.menuBar.w, h = L.status.y;
        double distance[] = {g_mouseY / std::max(1.f, h), (h - g_mouseY) / std::max(1.f, h),
            1.6 * g_mouseX / std::max(1.f, w), 1.6 * (w - g_mouseX) / std::max(1.f, w)};
        int dock = (int)(std::min_element(std::begin(distance), std::end(distance)) - distance);
        Rect target = dock == 0 ? Rect{0, 0, w, S(76)} : dock == 1 ? Rect{0, h - S(76), w, S(76)} :
            dock == 2 ? Rect{0, 0, S(216), h} : Rect{w - S(216), 0, S(216), h};
        Fill(target, p.accent, 35);
        Frame(target, p.accent);
    }
    DrawAlternateOverlay(p);
    DrawPopups(p);
}

// An album's cover filling `r` (cropped to square), or while it loads the surface color, or for an
// album without one a tile in a color of its own with the initial of `title` (app_ui.cpp).
void DrawAlbumCover(TrackId first, StringPool::Handle album, const std::wstring& title, const Rect& r, const Palette& p) {
    const int size = (int)std::max(r.w, S(64));
    // The track id is part of the key: a group cover override can switch between two files in the
    // same folder and album, where the folder identity alone would keep the old texture.
    const uint64_t key = (((uint64_t)first << 32) ^ (uint64_t)album ^ g_lib[first].dir) * 0x9E3779B97F4A7C15ull ^
                         (uint64_t)size;
    auto it = g_thumbs.find(key);
    if (it == g_thumbs.end()) {
        if (g_thumbs.size() >= kThumbsKept) {  // let the least recently drawn half go
            std::vector<std::pair<uint64_t, uint64_t>> done;
            for (auto& [k, t] : g_thumbs)
                if (t.done && t.used < g_frame) done.emplace_back(t.used, k);
            std::sort(done.begin(), done.end());
            for (size_t i = 0; i < done.size() / 2; ++i) {
                auto old = g_thumbs.find(done[i].second);
                if (old->second.texture) SDL_DestroyTexture(old->second.texture);
                g_thumbs.erase(old);
            }
        }
        it = g_thumbs.emplace(key, Thumb{}).first;
        if (!g_thumbFor.count(first)) {
            g_thumbFor[first] = key;
            ++g_thumbsPending;
            RequestThumb(first, size);
        } else {
            g_thumbs.erase(it);  // the track's cover is on its way at another size: ask again after
            Fill(r, p.surface2);
            return;
        }
    }
    it->second.used = g_frame;
    if (it->second.texture) {
        SDL_FRect dst{r.x, r.y, r.w, r.h};
        float tw = 0, th = 0;
        SDL_GetTextureSize(it->second.texture, &tw, &th);
        float side = std::min(tw, th);
        SDL_FRect src{(tw - side) / 2, (th - side) / 2, side, side};
        SDL_RenderTexture(g.renderer, it->second.texture, &src, &dst);
    } else if (it->second.done) {
        uint32_t hash = 2166136261u;
        for (wchar_t c : title) hash = (hash ^ (uint32_t)towlower(c)) * 16777619u;
        double hue = hash % 360;
        auto hsv = [](double h, double s, double v) {
            double c = v * s, x = c * (1 - std::fabs(std::fmod(h / 60, 2) - 1)), m = v - c, rr = 0, gg = 0, bb = 0;
            if (h < 60) rr = c, gg = x;
            else if (h < 120) rr = x, gg = c;
            else if (h < 180) gg = c, bb = x;
            else if (h < 240) gg = x, bb = c;
            else if (h < 300) rr = x, bb = c;
            else rr = c, bb = x;
            auto byte = [&](double u) { return (uint32_t)std::lround((u + m) * 255); };
            return byte(rr) << 16 | byte(gg) << 8 | byte(bb);
        };
        uint32_t tile = p.dark ? hsv(hue, 0.38, 0.30) : hsv(hue, 0.20, 0.90), ink = p.dark ? hsv(hue, 0.28, 0.86) : hsv(hue, 0.50, 0.42);
        Fill(r, tile);
        auto initial = std::find_if(title.begin(), title.end(), [](wchar_t c) { return iswalnum(c) != 0; });
        std::string text = initial == title.end() ? "\xE2\x99\xAB" : Utf8(std::wstring(1, (wchar_t)towupper(*initial)));
        Font font = r.w >= S(80) ? Font::Title : Font::Bold;
        DrawText(font, text, r.x, TextY(r, font), ink, r.w, 1);
    } else {
        Fill(r, p.surface2);
    }
    Frame({r.x - 1, r.y - 1, r.w + 2, r.h + 2}, Mix(p.bg, p.text, 14));
}

}  // namespace lx
