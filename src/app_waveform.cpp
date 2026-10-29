// The seek bar as a waveform: the playing track's peaks (peaks.h), computed in the background the
// first time a track plays (or during a ReplayGain scan) and cached. Until they arrive, or if
// the file can't be decoded, the seek bar stays the plain flat channel.
//
// The played part carries the state: the accent (or the player position's own color, app_fountain.cpp)
// while playing, amber while paused, the error color while Stop after current is armed or the track
// failed. The split between the two shades is the playhead. At every player position the waveform
// can be a Dark Fountain instead (app_fountain.cpp).
//
// One state block owns everything the bar needs for the track it shows: the peaks (or the request
// still computing them), the chapters, and what the last paint looked like. The entry points only
// move that block forward, so a paint can never mix two tracks.
#include "app.h"
#include "cue.h"
#include "peaks.h"
#include <array>

namespace app {

namespace {

struct Waveform {
    TrackId track = kNone;  // the track `peaks` belongs to (or is being computed for)
    bool ready = false;     // `peaks` holds that track's data
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
bool Request(TrackId id) {
    if (id == kNone || id >= g_lib.Count()) return false;
    std::wstring path = g_lib.Path(id);
    double start = 0, end = 0;
    if (IsSubtrack(path) && !SubtrackRange(path, start, end)) return false;
    const Track& t = g_lib[id];
    return GetPeaks(path, t.fileSize, t.fileTime, start, end, g_wave.peaks, g_wave.request);
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

int Loudest(const Peaks& peaks, BucketSpan span) {
    int v = 0;
    for (int b = span.first; b < span.last; ++b) v = std::max<int>(v, peaks.v[b]);
    return v;
}

}  // namespace

// A track change or explicit refresh asks for the current peaks. A theme change also refreshes:
// Apollo's plain slider can leave a pending request whose completed data is already cached.
// Reconcile it without replaying the track or changing the seek position.
void WaveformTrackChanged(bool refresh) {
    // Startup can announce playback before the peak service starts. With no queued request,
    // the same track still needs another chance when UpdateNowPlaying runs after startup.
    if (!refresh && g.playing == g_wave.track && (g_wave.track == kNone || g_wave.ready || g_wave.request)) return;
    if (g_wave.track != g.playing) CancelPeaks();
    g_wave.track = g.playing;
    g_wave.request = 0;
    g_wave.ready = g_wave.track != kNone && Request(g_wave.track);
    g_wave.drawnPos = -1;
    Repaint();
}

// The background worker finished a request: take the peaks only if it is still the one we want.
void OnPeaksReady(uint32_t requestId) {
    if (g_wave.track == kNone || g_wave.ready || requestId != g_wave.request) return;
    g_wave.request = 0;
    g_wave.ready = Request(g_wave.track);
    if (g_wave.ready) {
        g_wave.drawnPos = -1;  // force the next tick to paint the columns
        Repaint();
    }
}

// UpdateStatus calls this every tick. The bar repaints only when the playhead or the color
// actually changed; the fountain wakes first, since it animates on its own clock.
void WaveformTick() {
    if (!g.seek) return;
    FountainWake();
    bool animate = LiveWaveformShown() && !FountainShown() && !g.engine.Paused() && IsWindowVisible(g.seek) && !IsIconic(g.hwnd);
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

bool LiveWaveformShown() { return RadioPlaying() || (g.liveWaveform && g.playing != kNone); }
bool WaveformShown() { return LiveWaveformShown() || (g_wave.ready && g_wave.track != kNone && g.playing == g_wave.track); }

// The loudest peak, 0..255, between fractions `from` and `to` of the track (0 while none is known).
int WaveformPeak(double from, double to) {
    if (LiveWaveformShown() || !WaveformShown() || !(to > from)) return 0;
    int first = std::clamp((int)(from * kPeakBuckets), 0, kPeakBuckets - 1);
    int last = std::clamp((int)(to * kPeakBuckets), first + 1, kPeakBuckets);
    return Loudest(g_wave.peaks, {first, last});
}

// Draws the waveform across the channel's width and the whole bar's height (`rc`, transposed for the
// vertical seek bar). Columns 2 px wide with a 1 px gap; what has played carries the state color
// (WaveformPlayedColor, app_fountain.cpp). The spotlight, big mode and the vertical bar show only a
// waveform: until it's known, its columns lie flat.
bool DrawWaveform(HDC dc, const RECT& rc, const RECT& channel, double frac) {
    const bool live = LiveWaveformShown();
    std::array<uint8_t, 256> livePeaks{};
    if (live) g.engine.WaveformPeaks(livePeaks.data(), livePeaks.size());
    if (RadioPlaying()) frac = 1;
    bool known = WaveformShown();
    if (!known && g.size < ViewSize::Spotlight && !SeekVertical()) return false;
    // The bar isn't erased before this any more (WaveformTick), so the waveform paints the whole
    // control itself: the gaps between the columns are the bar's own background.
    FillSolid(dc, rc, SurfaceColor());
    int left = channel.left, width = channel.right - channel.left;
    if (width <= 0) return false;
    int right = left + width;
    int cy = (rc.top + rc.bottom) / 2, half = std::max(1, (int)(rc.bottom - rc.top) / 2 - S(2));
    int col = std::max(2, S(2)), step = col + std::max(1, S(1));
    int playedX = left + (int)(width * frac);
    COLORREF played = WaveformPlayedColor(), rest = WaveformRestColor();
    for (int x = left; x < right; x += step) {
        // The loudest bucket under this column (up to the next one), so short peaks aren't lost
        // when columns are wide. The last column is clipped to the channel, leaving no gap there.
        int end = std::min(x + col, right);
        int v = 0;
        if (live) {
            int first = std::clamp((x - left) * 256 / width, 0, 255);
            int last = std::clamp((x + step - left) * 256 / width, first + 1, 256);
            for (int b = first; b < last; ++b) v = std::max<int>(v, livePeaks[b]);
        } else if (known) v = Loudest(g_wave.peaks, BucketsFor(x, x + step, left, width));
        int h = std::max(1, half * v / 255);
        COLORREF color = x < playedX ? played : rest;
        FillSolid(dc, {x, cy - h, end, cy + h + 1}, color);
        // Fractional end pixels keep a quiet note's smooth fall visible in the slim player.
        int alpha = live ? std::max(0, half * v - h * 255) : 0;
        if (alpha) {
            COLORREF bg = SurfaceColor();
            COLORREF edge = RGB((GetRValue(color) * alpha + GetRValue(bg) * (255 - alpha)) / 255,
                                (GetGValue(color) * alpha + GetGValue(bg) * (255 - alpha)) / 255,
                                (GetBValue(color) * alpha + GetBValue(bg) * (255 - alpha)) / 255);
            FillSolid(dc, {x, cy - h - 1, end, cy - h}, edge);
            FillSolid(dc, {x, cy + h + 1, end, cy + h + 2}, edge);
        }
    }
    return true;
}

}  // namespace app
