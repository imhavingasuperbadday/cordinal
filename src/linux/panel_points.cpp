// The Points window (View > Games and stats > cordinal points, or a click on the chip) and Your year in
// music on Linux, as the Windows build's app_points.cpp and app_review.cpp. The rules and the ledger are
// points.cpp and playlog.cpp; these only show them.
#include "panels_internal.h"

#include "playlog.h"
#include "points.h"

#include <ctime>

namespace lx::panel {
namespace {

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

}  // namespace

const Ops kPointsOps{
    .draw = DrawPointsPanel,
    .mouseDown = [](float x, float y, int) { PointsMouseDown(x, y); },
    .mouseMove = PointsMouseMove,
    .key = [](const SDL_KeyboardEvent& e) { if (e.key == SDLK_ESCAPE) ClosePanel(); },
};

const Ops kReviewOps{
    .draw = DrawReview,
    .mouseDown = [](float x, float y, int) { ReviewMouseDown(x, y); },
    .mouseMove = ReviewMouseMove,
    .key = ReviewKey,
};

}  // namespace lx::panel

namespace lx {
using namespace panel;

void OpenPoints() {
    g_points = PointsPanel{};
    g_kind = Kind::Points;
    g.redraw = true;
}
void OpenReview() {
    g_review = ReviewPanel{};
    g_review.year = CurrentYear();
    ReviewRecompute();
    g_kind = Kind::Review;
    g.redraw = true;
}

}  // namespace lx
