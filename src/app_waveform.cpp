// The seek bar as a waveform: the playing track's peaks (peaks.h), computed in the background the
// first time a track plays (or during a ReplayGain scan) and cached. A likely next track is
// prepared in advance; an uncached, quickly seekable file shows a sampled preview while decoding.
//
// The played part carries the state: the accent (or the player position's own color, app_fountain.cpp)
// while playing, amber while paused, the error color while Stop after current is armed or the track
// failed. The split between the two shades is the playhead. At every player position the waveform
// can be a Dark Fountain instead (app_fountain.cpp).
//
// Seek bar glow draws a halo of each column's colour around the bar, past its edges and ends, that
// pulses with the beat of what is heard (peaks.h GlowPulse), for every look, whole song or live.
// While it plays, the whole song's bar then repaints on the live waveform's timer.
//
// The moodbar looks (Preferences, Seek bar) colour the bar by its sound instead (peaks.h
// MoodPalette, in the Moodbar colours scheme): a flat stripe, or the waveform's own columns, for the
// whole song or the live audio. Their colours can't carry the state any more, so what has played is
// drawn at full strength, the rest dimmed and desaturated, and the whole song's bar gets a playhead
// line for the state: the text color while playing, amber paused, red for a warning. Smooth seek bar
// softens the whole song's outline and blends its colours. Waveform reflection can drop the mirrored
// lower half (Upper half), or mirror the waveforms as bright below as above (Full height); the moodbar
// stripe keeps the upper half without the reflection.
//
// One state block owns everything the bar needs for the track it shows: the peaks (or the request
// still computing them), the chapters, and what the last paint looked like. The entry points only
// move that block forward, so a paint can never mix two tracks.
#include "app.h"
#include "cue.h"
#include "peaks.h"
#include "taste.h"
#include <array>

namespace app {

namespace {

struct Waveform {
    TrackId track = kNone;  // the track `peaks` belongs to (or is being computed for)
    bool ready = false;     // `peaks` holds that track's complete or in-progress data
    uint32_t request = 0;   // the background request `track` is waiting on (0 = none)
    Peaks peaks{};

    TrackId chapterTrack = kNone;  // the track `chapters` were read for
    std::vector<Chapter> chapters;

    // What the seek bar was last painted with, so a tick that changed neither leaves it alone.
    int drawnPos = -1;
    bool drawnLive = false;
    COLORREF drawnPlayed = CLR_INVALID, drawnRest = CLR_INVALID;
};

Waveform g_wave;
constexpr UINT_PTR kLiveWaveTimer = 0xF0A2;
bool liveTimer = false;

// Repaints are whole-bar: the stock trackbar invalidates only the strip its thumb passed through,
// which leaves stale columns over the rest of the waveform. No background erase either - every
// draw path paints its own, and erasing blinked the waveform out from under the cursor.
void Repaint() {
    if (g.seek) InvalidateRect(g.seek, nullptr, FALSE);
}

// The playing track's chapters, read once per track. Only a plain file has them: a CUE subtrack's
// chapters belong to the sheet's tracks, not to the slice, and game-music songs have none.
const std::vector<Chapter>& Chapters() {
    if (g.playing == g_wave.chapterTrack) return g_wave.chapters;
    g_wave.chapterTrack = g.playing;
    g_wave.chapters.clear();
    if (g.playing != kNone && g.playing < g_lib.Count()) {
        std::wstring path = g_lib.Path(g.playing);
        if (!IsSubtrack(path)) ReadChapters(path, g_wave.chapters);
    }
    return g_wave.chapters;
}

// Queues (or immediately answers) the peaks for `id`. A CUE track asks for its range of the file
// in file seconds, as peaks.h documents; a song of a game-music file has no range to ask for.
bool PeakSource(TrackId id, std::wstring& path, double& start, double& end) {
    if (id == kNone || id >= g_lib.Count()) return false;
    path = g_lib.Path(id);
    start = end = 0;
    if (IsSubtrack(path) && !SubtrackRange(path, start, end)) return false;
    return true;
}

bool Request(TrackId id, Peaks& peaks, uint32_t& request) {
    std::wstring path;
    double start, end;
    if (!PeakSource(id, path, start, end)) { request = 0; return false; }
    const Track& t = g_lib[id];
    if (GetPeaks(path, t.fileSize, t.fileTime, start, end, peaks, request)) {
        NoteMood(id, peaks);
        if (ConsumeTasteSave()) g.dirty = true;
        return true;
    }
    return GetPeakProgress(request, peaks);
}

// The buckets a pixel span [x0, x1) of a channel `width` px wide covers. At least one bucket, so a
// narrow column still reports the loudest sample under it instead of silence.
struct BucketSpan {
    int first, last;
};

BucketSpan BucketsFor(int x0, int x1, int left, int width) {
    width = std::max(1, width);
    int first = std::clamp((int)((int64_t)(x0 - left) * kPeakBuckets / width), 0, kPeakBuckets - 1);
    int last = std::clamp((int)((int64_t)(x1 - left) * kPeakBuckets / width), first + 1, kPeakBuckets);
    return {first, last};
}

int Loudest(const uint8_t* levels, BucketSpan span) {
    int v = 0;
    for (int b = span.first; b < span.last; ++b) v = std::max<int>(v, levels[b]);
    return v;
}

// `percent` of the way from a to b.
COLORREF Mix(COLORREF a, COLORREF b, int percent) {
    auto mix = [percent](int x, int y) { return x + (y - x) * percent / 100; };
    return RGB(mix(GetRValue(a), GetRValue(b)), mix(GetGValue(a), GetGValue(b)), mix(GetBValue(a), GetBValue(b)));
}

// One waveform color worked into its shades. The body is brightest along the center line and
// falls to the plain color at its tips, which gives the columns a lit, rounded look; the lower half
// is the upper one's reflection, dimmer and fading away from the line. Bands rather than a true
// gradient: GDI's GradientFill doesn't follow the upright bar's transposing world transform.
struct WaveShades {
    static constexpr int kBands = 6;
    COLORREF body[kBands], mirror[kBands];  // nearest the center line first
    COLORREF edge, mirrorEdge;              // the peaks above the RMS body
};

WaveShades ShadesOf(COLORREF c, COLORREF bg) {
    WaveShades s{};
    for (int i = 0; i < WaveShades::kBands; ++i) {
        s.body[i] = Mix(c, RGB(255, 255, 255), 24 * (WaveShades::kBands - 1 - i) / (WaveShades::kBands - 1));
        s.mirror[i] = Mix(c, bg, 30 + 40 * i / (WaveShades::kBands - 1));
    }
    s.edge = Mix(c, bg, 35);
    s.mirrorEdge = Mix(c, bg, 72);
    return s;
}

// The rectangles of one paint, grouped by color. A column's rectangles never overlap and adjacent
// columns never touch, so filling each color's rectangles with PolyPolygon gives exactly the pixels
// the per-column FillSolid calls did, with a fraction of the GDI calls. Most columns share a shade;
// the antialiased tips add a few more colors, so a paint costs a handful of calls per color rather
// than one per rectangle.
class WaveFills {
public:
    void Reset() {
        for (int i = 0; i < used_; ++i) {
            points_[i].clear();
            counts_[i].clear();
        }
        used_ = 0;
        overflow_.clear();
    }

    void Add(COLORREF color, const RECT& r) {
        if (r.right <= r.left || r.bottom <= r.top) return;
        int i = 0;
        while (i < used_ && colors_[i] != color) ++i;
        if (i == used_) {
            if (used_ == kGroups) {  // too many tip blends this frame: draw the rest one by one
                overflow_.push_back({color, r});
                return;
            }
            colors_[i] = color;
            ++used_;
        }
        points_[i].push_back({r.left, r.top});
        points_[i].push_back({r.right, r.top});
        points_[i].push_back({r.right, r.bottom});
        points_[i].push_back({r.left, r.bottom});
        counts_[i].push_back(4);
    }

    void Flush(HDC dc) {
        if (used_) {
            HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(DC_BRUSH));
            HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
            for (int i = 0; i < used_; ++i) {
                SetDCBrushColor(dc, colors_[i]);
                // PolyPolygon's cost grows faster than its polygon count. Batches of 32 avoid one
                // giant call while keeping the call count far below one per rectangle.
                const int n = (int)counts_[i].size();
                for (int at = 0; at < n; at += kBatch)
                    PolyPolygon(dc, points_[i].data() + at * 4, counts_[i].data() + at, std::min(kBatch, n - at));
            }
            SelectObject(dc, oldPen);
            SelectObject(dc, oldBrush);
        }
        for (const auto& [color, rect] : overflow_) FillSolid(dc, rect, color);
        Reset();
    }

private:
    static constexpr int kGroups = 256;  // the two color schemes' shades and their blended tips
    static constexpr int kBatch = 32;    // rectangles per PolyPolygon call
    std::array<COLORREF, kGroups> colors_{};
    std::array<std::vector<POINT>, kGroups> points_;
    std::array<std::vector<INT>, kGroups> counts_;
    struct Overflow {
        COLORREF color;
        RECT rect;
    };
    std::vector<Overflow> overflow_;
    int used_ = 0;
};

// The playhead line over a moodbar: the state color (paused, a warning) or else the text color, which
// stands out from every mood hue.
COLORREF MoodPlayhead() {
    const bool warn = g.playing != kNone && (g_lib[g.playing].Failed() || g.stopAfterCurrent);
    return warn || g.engine.Paused() ? WaveformPlayedColor() : Ref(P().text);
}

// How far the Smooth seek bar setting blurs the whole song, in buckets either side (of 600).
constexpr int kSmoothRadius = 8;

Rgb RgbOf(COLORREF c) { return (Rgb)GetRValue(c) << 16 | (Rgb)GetGValue(c) << 8 | GetBValue(c); }

// What the moodbar's colours turn from on `background`: the chosen Waveform colour, else the accent.
Rgb MoodBase(Rgb background) {
    const Rgb chosen = WaveColorOn(g.waveColor, background);
    return chosen != kNoRgb ? chosen : P().accent;
}

uint32_t DibColor(COLORREF c) {
    return (uint32_t)GetRValue(c) << 16 | (uint32_t)GetGValue(c) << 8 | GetBValue(c);
}

// The direct path writes device pixels, so it takes an identity world transform or the upright seek
// bar's transpose (x' = y, y' = x, app_ui_trackbar.cpp), whose columns it writes as rows. Every other
// target, such as a scaled print DC, keeps GDI.
enum class DibTarget { None, Plain, Transposed };
DibTarget DibTargetOf(HDC dc) {
    if (GetMapMode(dc) != MM_TEXT) return DibTarget::None;
    if (GetGraphicsMode(dc) != GM_ADVANCED) return DibTarget::Plain;
    XFORM t{};
    if (!GetWorldTransform(dc, &t) || t.eDx != 0 || t.eDy != 0) return DibTarget::None;
    if (t.eM11 == 1 && t.eM12 == 0 && t.eM21 == 0 && t.eM22 == 1) return DibTarget::Plain;
    if (t.eM11 == 0 && t.eM12 == 1 && t.eM21 == 1 && t.eM22 == 0) return DibTarget::Transposed;
    return DibTarget::None;
}

// A reusable top-down 32-bit surface for the seek bar's waveform. Every frame is built by direct pixel
// writes and copied once: the same rectangles through PolyPolygon cost many GDI calls per frame.
struct WaveDib {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;
    uint32_t* bits = nullptr;
    int width = 0, height = 0;

    bool Ready(HDC target, int w, int h) {
        if (dc && width == w && height == h && bits) return true;
        Release();
        HDC surface = CreateCompatibleDC(target);
        if (!surface) return false;
        BITMAPINFO info{};
        info.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
        void* pixels = nullptr;
        HBITMAP created = CreateDIBSection(target, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!created || !pixels) {
            if (created) DeleteObject(created);
            DeleteDC(surface);
            return false;
        }
        dc = surface;
        bitmap = created;
        bits = static_cast<uint32_t*>(pixels);
        width = w;
        height = h;
        previous = SelectObject(dc, bitmap);
        return true;
    }

    // `rc` is the logical rectangle the frame covers. A transposed target takes the surface in
    // device coordinates, already turned, so the transform is lifted for the copy.
    void Present(HDC target, const RECT& rc, bool transposed) {
        if (!transposed) {
            BitBlt(target, rc.left, rc.top, width, height, dc, 0, 0, SRCCOPY);
            return;
        }
        XFORM saved{};
        GetWorldTransform(target, &saved);
        ModifyWorldTransform(target, nullptr, MWT_IDENTITY);
        BitBlt(target, rc.top, rc.left, width, height, dc, 0, 0, SRCCOPY);
        SetWorldTransform(target, &saved);
    }

    void Release() {
        if (dc && previous) SelectObject(dc, previous);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        dc = nullptr;
        bitmap = nullptr;
        previous = nullptr;
        bits = nullptr;
        width = height = 0;
    }
};

// The same rectangles WaveFills receives, written straight into a top-down DIB. FillRect excludes
// the right and bottom edges, so each span ends at r.right/r.bottom exactly. `width` and `height`
// are the logical rectangle's; a transposed surface stores logical column x as device row x, so
// the upright bar's pixels land where the transposed GDI calls would have put them.
class PixelFills {
public:
    PixelFills(uint32_t* bits, int width, int height, int left, int top, COLORREF background, bool transposed)
        : bits_(bits), width_(width), height_(height), left_(left), top_(top), transposed_(transposed) {
        std::fill_n(bits_, (size_t)width_ * height_, DibColor(background));
    }

    void Add(COLORREF color, const RECT& r) {
        int x0 = std::clamp((int)r.left - left_, 0, width_);
        int x1 = std::clamp((int)r.right - left_, 0, width_);
        int y0 = std::clamp((int)r.top - top_, 0, height_);
        int y1 = std::clamp((int)r.bottom - top_, 0, height_);
        if (x0 >= x1 || y0 >= y1) return;
        const uint32_t c = DibColor(color);
        if (transposed_) {
            for (int x = x0; x < x1; ++x) std::fill_n(bits_ + (size_t)x * height_ + y0, y1 - y0, c);
        } else {
            for (int y = y0; y < y1; ++y) std::fill_n(bits_ + (size_t)y * width_ + x0, x1 - x0, c);
        }
    }

    // One column's glow, the rows DrawWaveform's `halo` would Add one at a time: row top - d and
    // row bottom + d - 1 for d = 1..glow, `tint` mixed into `bg` by percent * weights[d] / scale (halved
    // below unless `even`), exactly as Mix rounds. A wide upright bar's halo runs to tens of thousands
    // of rows a frame, so they are written here without a rectangle each. A fade of 0 is the
    // background the surface already holds, and the fade only falls with d, so the loop stops there.
    void Halo(int x, int end, COLORREF bg, COLORREF tint, int percent, const int* weights, int scale, int glow, int top,
              int bottom, bool even) {
        const int x0 = std::clamp(x - left_, 0, width_), x1 = std::clamp(end - left_, 0, width_);
        if (x0 >= x1 || percent <= 0) return;
        const int r = GetRValue(bg), gr = GetGValue(bg), b = GetBValue(bg);
        const int dr = GetRValue(tint) - r, dg = GetGValue(tint) - gr, db = GetBValue(tint) - b;
        auto put = [&](int row, int fade) {
            if (fade <= 0 || row < 0 || row >= height_) return;
            const uint32_t c = (uint32_t)(r + dr * fade / 100) << 16 | (uint32_t)(gr + dg * fade / 100) << 8 |
                               (uint32_t)(b + db * fade / 100);
            if (transposed_) {
                for (int i = x0; i < x1; ++i) bits_[(size_t)i * height_ + row] = c;
            } else {
                std::fill_n(bits_ + (size_t)row * width_ + x0, x1 - x0, c);
            }
        };
        for (int d = 1; d <= glow; ++d) {
            const int fade = percent * weights[d] / scale;
            if (fade <= 0) break;
            put(top - d - top_, fade);
            put(bottom + d - 1 - top_, even ? fade : fade / 2);
        }
    }

private:
    uint32_t* bits_;
    int width_, height_, left_, top_;
    bool transposed_;
};

}  // namespace

// Start as soon as playback is requested, before the engine announces the first audible sample.
void PrepareWaveform(TrackId id) {
    if (!g.waveform || g.liveWaveform) return;
    Peaks peaks{};
    uint32_t request = 0;
    Request(id, peaks, request);
}

// The engine already chose this track for its gapless hand-off, so its exact peaks can be ready
// by the time it is heard without guessing from the playlist order.
void PrefetchWaveform(TrackId id) {
    if (!g.waveform || id == kNone || g.liveWaveform) { CancelPeakPrefetch(); return; }
    std::wstring path;
    double start, end;
    if (!PeakSource(id, path, start, end)) return;
    const Track& t = g_lib[id];
    PrefetchPeaks(path, t.fileSize, t.fileTime, start, end);
}

// A track change or explicit refresh asks for the current peaks. A theme change also refreshes:
// Apollo's plain slider can leave a pending request whose completed data is already cached.
// Reconcile it without replaying the track or changing the seek position.
void WaveformTrackChanged(bool refresh) {
    // Startup can announce playback before the peak service starts. With no queued request,
    // the same track still needs another chance when UpdateNowPlaying runs after startup.
    TrackId track = g.waveform ? g.playing : kNone;
    if (!refresh && track == g_wave.track && (g_wave.track == kNone || g_wave.ready || g_wave.request)) return;
    if (track == kNone) CancelPeaks();
    if (!g.waveform) CancelPeakPrefetch();
    g_wave.track = track;
    g_wave.request = 0;
    g_wave.ready = g_wave.track != kNone && Request(g_wave.track, g_wave.peaks, g_wave.request);
    g_wave.drawnPos = -1;
    Repaint();
}

// The worker has more peaks or has finished: take them only for the request we still want.
void OnPeaksReady(uint32_t requestId) {
    if (g_wave.track == kNone || !requestId || requestId != g_wave.request) return;
    g_wave.ready = Request(g_wave.track, g_wave.peaks, g_wave.request);
    g_wave.drawnPos = -1;
    Repaint();
}

// UpdateStatus calls this every tick. The bar repaints only when the playhead or the color
// actually changed; the fountain wakes first, since it animates on its own clock.
void WaveformTick() {
    if (!g.seek) return;
    FountainWake();
    // Seek bar glow pulses with the music, so a whole song's bar moves too while it plays.
    const bool pulsing = g.seekGlow && WaveformShown() && g.playing != kNone;
    bool animate = (LiveWaveformShown() || pulsing) && !FountainShown() && !g.engine.Paused() && IsWindowVisible(g.seek) &&
                   !IsIconic(g.hwnd);
    if (animate && !liveTimer) liveTimer = SetTimer(g.seek, kLiveWaveTimer, 33, nullptr) != 0;
    else if (!animate && liveTimer) { KillTimer(g.seek, kLiveWaveTimer); liveTimer = false; }
    int pos = (int)SendMessageW(g.seek, TBM_GETPOS, 0, 0);
    COLORREF played = WaveformPlayedColor(), rest = WaveformRestColor();
    bool live = LiveWaveformShown();
    if (pos == g_wave.drawnPos && played == g_wave.drawnPlayed && rest == g_wave.drawnRest && live == g_wave.drawnLive) return;
    g_wave.drawnPos = pos;
    g_wave.drawnLive = live;
    g_wave.drawnPlayed = played;
    g_wave.drawnRest = rest;
    Repaint();
}

bool WaveformStep(UINT_PTR id) {
    if (id != kLiveWaveTimer) return false;
    WaveformTick();
    return true;
}

int ChapterCount() {
    return (int)Chapters().size();
}

// The next chapter start past `pos` (0 when there is none): what a section marked on the seek bar
// runs to by default, so "Loop this section" follows what the file already says.
double ChapterStartAfter(double pos) {
    return ::ChapterStartAfter(Chapters(), pos);
}

// The chapter playing now as "3/12: Title" ("" without chapters).
std::wstring CurrentChapterText() {
    return ChapterTextAt(Chapters(), g.engine.Position());
}

bool LiveWaveformShown() { return g.waveform && (RadioPlaying() || (g.liveWaveform && g.playing != kNone)); }
bool WaveformShown() {
    return g.waveform && (LiveWaveformShown() ||
           (g_wave.track != kNone && g.playing == g_wave.track && (g_wave.ready || g_wave.request)));
}

// The loudest peak, 0..255, between fractions `from` and `to` of the track (0 while none is known).
int WaveformPeak(double from, double to) {
    if (LiveWaveformShown() || !g_wave.ready || !WaveformShown() || !(to > from)) return 0;
    int first = std::clamp((int)(from * kPeakBuckets), 0, kPeakBuckets - 1);
    int last = std::clamp((int)(to * kPeakBuckets), first + 1, kPeakBuckets);
    return Loudest(g_wave.peaks.v, {first, last});
}

// Draws the waveform across the channel's width and the whole bar's height (`rc`, transposed for the
// vertical seek bar). Columns 2 px wide with a 1 px gap; what has played carries the state color
// (WaveformPlayedColor, app_fountain.cpp). Each column is the RMS level as a solid body with the
// peak as a fainter edge around it (a live waveform has peaks only), shaded by WaveShades. The
// split between the two colors is the playhead; a live waveform draws no line of its own. With
// waveforms enabled, the spotlight, big mode and the vertical bar keep flat columns until peaks
// are known. The moodbar styles color each column by its sound instead and draw the playhead.
// Waveform reflection's Upper half drops the lower half and Full height draws it as bright as the
// upper one, for the whole song and the live waveform alike.
bool DrawWaveform(HDC dc, const RECT& rc, const RECT& channel, double frac) {
    if (!g.waveform) return false;
    const bool live = LiveWaveformShown();
    std::array<uint8_t, 256> livePeaks{};
    if (live) g.engine.WaveformPeaks(livePeaks.data(), livePeaks.size());
    if (RadioPlaying()) frac = 1;
    bool known = WaveformShown();
    if (!known && g.size < ViewSize::Spotlight && !SeekVertical()) return false;
    // The bar isn't erased before this any more (WaveformTick), so the waveform paints the whole
    // control itself: the gaps between the columns are the bar's own background.
    COLORREF bg = PlayerColor();
    int left = channel.left, width = channel.right - channel.left;
    if (width <= 0) {
        FillSolid(dc, rc, bg);
        return false;
    }
    int right = left + width;
    int cy = (rc.top + rc.bottom) / 2, half = std::max(1, (int)(rc.bottom - rc.top) / 2 - S(2));
    // The whole song's peaks, softened when Smooth seek bar is on.
    const bool song = !live && known && g_wave.ready;
    static Peaks smoothed;
    if (song && g.smoothSeek) SmoothPeaks(g_wave.peaks, smoothed, kSmoothRadius);
    const Peaks& peaks = song && g.smoothSeek ? smoothed : g_wave.peaks;
    // A track whose peaks are still unknown has no mood yet; a live waveform takes its colours from
    // the audio being heard.
    const int style = live || song ? std::clamp(g.seekStyle, 0, kSeekStyles - 1) : kSeekWaveform;
    const bool mood = style != kSeekWaveform, stripe = style == kSeekMoodbar;
    static MoodPalette moods;
    static Peaks liveMood;
    static LiveMood liveScale;
    float center = 0;
    if (mood) {
        // The mood waveform can take its own scheme; the stripe always takes the moodbar's.
        const int scheme = std::clamp(!stripe && g.moodWaveColors > 0 ? g.moodWaveColors - 1 : g.moodColors, 0,
                                      kMoodColorSchemes - 1);
        const Rgb base = MoodBase(RgbOf(bg));
        if (!moods.For(base, RgbOf(bg), scheme)) moods.Make(base, RgbOf(bg), scheme);
        if (live) {
            std::array<float, 256 * kMoodBands> bands{};
            g.engine.WaveformBands(bands.data(), livePeaks.size());
            center = liveScale.Scale(bands.data(), (int)livePeaks.size(), liveMood);
        } else {
            center = MoodCenter(peaks);
        }
    }
    // Live audio counts as heard unless its progress is split off (Seek bar shows, with progress).
    const bool allPlayed = live && g.fullLiveWaveform;
    // The reflection is the dimmer lower half; off draws the upper half alone. Full height mirrors a
    // waveform as bright below the center line as above it, with no darker underside; the moodbar
    // stripe keeps the upper half.
    const bool fullHeight = !g.waveReflection && g.waveFullHeight && !stripe;
    const bool reflect = g.waveReflection || fullHeight;
    // The stripe's columns touch; the waveform's stand a pixel apart.
    int col = std::max(2, S(2)), step = stripe ? col : col + std::max(1, S(1));
    // Seek bar glow: a halo of each column's colour around the bar, fading out from its edge, that
    // pulses with the beat of what is heard (GlowPulse) and is stronger where the song is loud and
    // on what has played. The stripe stands shorter to leave the halo room before the bar's edge;
    // a waveform's columns already have room above their tips.
    const int glow = g.seekGlow ? std::max(S(4), half / 2) : 0;
    const int room = std::min(cy - (int)rc.top, (int)rc.bottom - cy - 1) - glow - 1;  // a pixel short of the edge
    const int stripeHalf = stripe && glow ? std::clamp(room, 1, half) : half;
    static GlowPulse pulse;
    if (glow && (g.playing != kNone || RadioPlaying()) && !g.engine.Paused()) {
        std::array<uint8_t, 256> heard{};
        if (live) heard = livePeaks;
        else g.engine.WaveformPeaks(heard.data(), heard.size());
        pulse.Step(heard.data(), (int)heard.size(), GetTickCount64() / 1000.0);
    }
    const float beat = pulse.Value();
    int playedX = left + (int)(width * frac);
    // `glow` rows of `tint` at `percent` strength, fading softly, above row `top` and, at half
    // strength unless `even`, from row `bottom` down: the first row past the reflection's edge, or
    // past the center line when the upper half is drawn alone, so the halo goes all the way round.
    const int glowScale = (glow + 1) * (glow + 1);
    static std::vector<int> glowWeights;  // (glow + 1 - d)^2 for row d of the fade
    glowWeights.resize(glow + 1);
    for (int d = 1; d <= glow; ++d) glowWeights[d] = (glow + 1 - d) * (glow + 1 - d);
    auto fadeAt = [&](int percent, int d) { return percent * glowWeights[d] / glowScale; };
    auto halo = [&](auto& fills, int x, int end, COLORREF tint, int percent, int top, int bottom, bool even) {
        if constexpr (std::is_same_v<std::decay_t<decltype(fills)>, PixelFills>) {
            fills.Halo(x, end, bg, tint, percent, glowWeights.data(), glowScale, glow, top, bottom, even);
        } else {
            for (int d = 1; d <= glow && percent > 0; ++d) {
                const int fade = fadeAt(percent, d);
                if (top - d >= rc.top) fills.Add(Mix(bg, tint, fade), {x, top - d, end, top - d + 1});
                if (bottom + d <= rc.bottom)
                    fills.Add(Mix(bg, tint, even ? fade : fade / 2), {x, bottom + d - 1, end, bottom + d});
            }
        }
    };
    // How strongly a column glows: the beat lifts every column at once, its own loudness (0..1)
    // a little more, and what has played glows brighter than what is still to come.
    auto glowPercent = [beat](float level, bool played) {
        level = std::clamp(level, 0.0f, 1.0f);
        return level > 0 ? (int)((25 + 70 * beat) * (0.6f + 0.4f * level) * (played ? 1.0f : 0.55f)) : 0;
    };
    // The stripe's two ends glow sideways too, so the halo goes all the way around.
    struct EndGlow { COLORREF tint = 0; int percent = 0; };
    EndGlow firstEnd, lastEnd;
    const COLORREF playedTint = WaveformPlayedColor(), restTint = WaveformRestColor();
    const WaveShades playedShades = ShadesOf(playedTint, bg), restShades = ShadesOf(restTint, bg);
    // The rows of band `i`, counted from the center line out.
    const int span = half + 1;
    auto bandStart = [span](int i) { return i * span / WaveShades::kBands; };
    auto columns = [&](auto& fills) {
        for (int x = left; x < right; x += step) {
            // The loudest bucket under this column (up to the next one), so short peaks aren't lost
            // when columns are wide. The last column is clipped to the channel, leaving no gap there.
            int end = std::min(x + col, right);
            int v = 0, rms = 0;
            BucketSpan buckets{0, 1};
            if (live) {
                buckets.first = std::clamp((x - left) * 256 / width, 0, 255);
                buckets.last = std::clamp((x + step - left) * 256 / width, buckets.first + 1, 256);
                for (int b = buckets.first; b < buckets.last; ++b) v = std::max<int>(v, livePeaks[b]);
                rms = v;
            } else if (song) {
                buckets = BucketsFor(x, x + step, left, width);
                for (int b = buckets.first; b < buckets.last; ++b) {
                    v = std::max<int>(v, peaks.v[b]);
                    rms = std::max<int>(rms, peaks.rms[b]);
                }
            }
            int h = std::max(1, half * v / 255), body = std::clamp(half * rms / 255, 1, h);
            const bool played = allPlayed || x < playedX;
            const WaveShades* shades = played ? &playedShades : &restShades;
            COLORREF tint = played ? playedTint : restTint;
            WaveShades moodShades;
            if (mood) {
                const MoodTone tone = live ? MoodOf(liveMood, buckets.first, buckets.last, center, (int)livePeaks.size())
                                           : MoodOf(peaks, buckets.first, buckets.last, center);
                tint = Ref(moods.Pick(tone, played));
                if (stripe) {
                    // Without the reflection the stripe stands on the center line, and glows below it.
                    const int top = cy - stripeHalf, bottom = reflect ? cy + stripeHalf + 1 : cy + 1;
                    fills.Add(tint, {x, top, end, bottom});
                    const int percent = glowPercent(tone.level, played);
                    halo(fills, x, end, tint, percent, top, bottom, true);
                    if (x == left) firstEnd = {tint, percent};
                    lastEnd = {tint, percent};
                    continue;
                }
                moodShades = ShadesOf(tint, bg);
                shades = &moodShades;
            }
            const WaveShades& s = *shades;
            // Upper half: row cy - d for d = 0..h. Lower half, the reflection: row cy + 1 + d for d < h.
            for (int i = 0; i < WaveShades::kBands && bandStart(i) <= body; ++i) {
                int d0 = bandStart(i), d1 = std::min(body + 1, bandStart(i + 1));
                fills.Add(s.body[i], {x, cy - d1 + 1, end, cy - d0 + 1});
                if (reflect) fills.Add(fullHeight ? s.body[i] : s.mirror[i], {x, cy + 1 + d0, end, cy + 1 + std::min(d1, body)});
            }
            if (h > body) {
                fills.Add(s.edge, {x, cy - h, end, cy - body});
                if (reflect) fills.Add(fullHeight ? s.edge : s.mirrorEdge, {x, cy + 1 + body, end, cy + 1 + h});
            }
            // Fractional end pixels keep a quiet note's smooth fall visible in the slim player.
            int alpha = live ? std::max(0, half * v - h * 255) : 0;
            if (alpha) {
                int tip = std::min(WaveShades::kBands - 1, h * WaveShades::kBands / span);
                fills.Add(Mix(bg, s.body[tip], alpha * 100 / 255), {x, cy - h - 1, end, cy - h});
                if (reflect) fills.Add(Mix(bg, fullHeight ? s.body[tip] : s.mirror[tip], alpha * 100 / 255), {x, cy + h + 1, end, cy + h + 2});
            }
            // The glow starts past the column's tip, and past its fractional end pixel when it has one.
            const int tipRow = alpha ? cy - h - 1 : cy - h;
            const int mirrorRow = !reflect ? cy + 1 : alpha ? cy + h + 2 : cy + h + 1;
            halo(fills, x, end, tint, glowPercent(v / 255.0f, played), tipRow, mirrorRow, fullHeight);
        }
        if (stripe && glow) {
            const int top = cy - stripeHalf, bottom = reflect ? cy + stripeHalf + 1 : cy + 1;
            for (int d = 1; d <= glow; ++d) {
                if (left - d >= rc.left && firstEnd.percent)
                    fills.Add(Mix(bg, firstEnd.tint, fadeAt(firstEnd.percent, d)), {left - d, top, left - d + 1, bottom});
                if (right + d <= rc.right && lastEnd.percent)
                    fills.Add(Mix(bg, lastEnd.tint, fadeAt(lastEnd.percent, d)), {right + d - 1, top, right + d, bottom});
            }
        }
    };
    const int headWidth = std::max(2, S(2));
    const RECT head{playedX - headWidth / 2, cy - stripeHalf, playedX - headWidth / 2 + headWidth,
                    reflect ? cy + stripeHalf + 1 : cy + 1};
    // Every look can repaint 30 times a second (live, or a whole song's glow), and the moodbar's and
    // the glow's blended colours overflow WaveFills' groups into one GDI call per rectangle, so
    // the bar is built in pixels whenever the target allows, upright or not. Other targets keep
    // the GDI path below.
    if (const DibTarget target = DibTargetOf(dc); target != DibTarget::None) {
        const bool transposed = target == DibTarget::Transposed;
        const int w = rc.right - rc.left, h = rc.bottom - rc.top;
        static WaveDib dib;
        if (w > 0 && h > 0 && dib.Ready(dc, transposed ? h : w, transposed ? w : h)) {
            GdiFlush();  // the previous frame's BitBlt may still be reading these bits
            PixelFills pixels(dib.bits, w, h, rc.left, rc.top, bg, transposed);
            columns(pixels);
            if (mood && !live) pixels.Add(MoodPlayhead(), head);
            dib.Present(dc, rc, transposed);
            return true;
        }
    }
    FillSolid(dc, rc, bg);
    static WaveFills fills;
    fills.Reset();
    columns(fills);
    fills.Flush(dc);
    if (mood && !live) FillSolid(dc, head, MoodPlayhead());
    return true;
}

// The Mood column: the track's moodbar as a stripe in the Moodbar colours scheme, one row of
// pixels stretched to the cell's height. The peaks come from ListPeaks' memory, so a paint never
// decodes; a track not known yet leaves its cell empty until OnListPeaksReady.
void DrawMoodCell(HDC dc, const RECT& cell, TrackId id, COLORREF bg) {
    std::wstring path;
    double start, end;
    if (!PeakSource(id, path, start, end)) return;
    const Track& t = g_lib[id];
    static Peaks peaks;
    if (!ListPeaks(path, t.fileSize, t.fileTime, start, end, peaks)) return;
    const RECT r{cell.left + S(4), cell.top + S(3), cell.right - S(4), cell.bottom - S(3)};
    const int w = r.right - r.left;
    if (w <= 0 || r.bottom <= r.top) return;
    static MoodPalette moods;
    const int scheme = std::clamp(g.moodColors, 0, kMoodColorSchemes - 1);
    const Rgb background = RgbOf(WindowBg()), base = MoodBase(background);
    if (!moods.For(base, background, scheme)) moods.Make(base, background, scheme);
    const float center = MoodCenter(peaks);
    static std::vector<uint32_t> row;
    row.resize(w);
    for (int x = 0; x < w; ++x) {
        BucketSpan buckets = BucketsFor(x, x + 1, 0, w);
        MoodTone tone = MoodOf(peaks, buckets.first, buckets.last, center);
        row[x] = DibColor(tone.level > 0 ? Ref(moods.Pick(tone, true)) : bg);  // silence shows the row
    }
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), w, 1, 1, 32, BI_RGB};
    StretchDIBits(dc, r.left, r.top, w, r.bottom - r.top, 0, 0, w, 1, row.data(), &info, DIB_RGB_COLORS, SRCCOPY);
}

Rgb PreferredWaveColor() {
    return WaveColorOn(g.waveColor, RgbOf(PlayerColor()));
}

Rgb PreferredWaveRest(Rgb played) {
    return g.waveRestColor == kWaveRestFaded ? FadedWaveColor(played, RgbOf(PlayerColor())) : kNoRgb;
}

void OnListPeaksReady() {
    if (g.list && SubitemOf(kColMood) >= 0) InvalidateRect(g.list, nullptr, FALSE);
}

}  // namespace app
