// cordinal points: the chip in the now-playing strip and the Points window (View ▸ Games and stats).
// The rules and the ledger live in points.cpp; this file only shows them.
#include "app.h"
#include "points.h"

namespace app {

namespace {

const wchar_t kPointsClass[] = L"CordinalPoints";

struct PointsWindow {
    HWND hwnd = nullptr;
    HFONT hero = nullptr, title = nullptr;
    bool rules = false;  // showing "How points work"
    RECT rulesLink{}, chart{};
    int hoverDay = -1;   // index into the chart's days
} w;

// The chip: rank, level and progress; a "+N" rises from it when points arrive.
struct Chip {
    std::wstring pop;   // "+31"
    ULONGLONG popAt = 0;
    int levelUpFrames = 0;
} chip;

std::wstring Grouped(uint64_t v) {
    std::wstring digits = std::to_wstring(v), out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0) out += L',';
        out += digits[i];
    }
    return out;
}

// Centipoints to whole points, for display.
uint64_t Whole(uint64_t centi) { return centi / 100; }

std::wstring Plural(uint64_t n, const wchar_t* one, const wchar_t* many) {
    return Grouped(n) + L" " + (n == 1 ? one : many);
}

// A short relative time such as "3 min ago".
std::wstring Ago(uint32_t t, uint32_t now) {
    uint32_t s = now > t ? now - t : 0;
    if (s < 60) return L"just now";
    if (s < 3600) return std::to_wstring(s / 60) + L" min ago";
    if (s < 86400) return std::to_wstring(s / 3600) + L" h ago";
    return Plural(s / 86400, L"day ago", L"days ago");
}

// Local day number (days since 1970 in local time), matching points.cpp.
uint32_t LocalDay(uint32_t unix) {
    FILETIME ft, local;
    ULARGE_INTEGER u;
    u.QuadPart = ((uint64_t)unix + 11644473600ull) * 10000000ull;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    FileTimeToLocalFileTime(&ft, &local);
    u.LowPart = local.dwLowDateTime;
    u.HighPart = local.dwHighDateTime;
    return (uint32_t)(u.QuadPart / 10000000ull / 86400ull - 134774ull);
}

std::wstring DayLabel(uint32_t day) {
    uint64_t t = ((uint64_t)day + 134774ull) * 86400ull * 10000000ull;
    FILETIME ft{(DWORD)t, (DWORD)(t >> 32)};
    SYSTEMTIME st;
    FileTimeToSystemTime(&ft, &st);
    wchar_t buf[64];
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, L"MMM d", buf, 64, nullptr);
    return buf;
}

void Text(HDC dc, const std::wstring& s, RECT r, HFONT font, COLORREF color, UINT flags) {
    HGDIOBJ old = SelectObject(dc, font);
    SetTextColor(dc, color);
    DrawTextW(dc, s.c_str(), -1, &r, flags | DT_NOPREFIX);
    SelectObject(dc, old);
}

int TextWidth(HDC dc, const std::wstring& s, HFONT font) {
    HGDIOBJ old = SelectObject(dc, font);
    SIZE sz{};
    GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz);
    SelectObject(dc, old);
    return sz.cx;
}

// A bar with 2 px rounded data ends, as in the library views.
void Bar(HDC dc, RECT r, COLORREF color) {
    if (r.right <= r.left || r.bottom <= r.top) return;
    HBRUSH b = CreateSolidBrush(color);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, GetStockObject(NULL_PEN));
    int rad = std::min<int>(S(4), std::min(r.right - r.left, r.bottom - r.top));
    RoundRect(dc, r.left, r.top, r.right + 1, r.bottom + 1, rad, rad);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
}

struct LevelInfo {
    uint64_t total;  // whole points
    int level;
    uint64_t start, next;  // whole points where this level starts and the next begins
};

// Rank, level and the thresholds around it, all in whole points for the UI.
LevelInfo Level() {
    LevelInfo l;
    uint64_t centi = PointsTotal();
    l.total = Whole(centi);
    l.level = PointsLevel(centi);
    l.start = Whole(PointsThreshold(l.level));
    l.next = Whole(PointsThreshold(l.level + 1));
    return l;
}

// One recent-event line: the kind, plus the track (or album for album events) it was about.
std::wstring EventText(const PointsEvent& e) {
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

// ---------------------------------------------------------------------------
// Points window

// The "How points work" text; keep it in sync with the rules in points.cpp.
const wchar_t* const kRules =
    L"Listening: 2 points for every minute you actually listen (seeking doesn't count).\n"
    L"Plays: 5 points for a play, more each time you come back to a song: the 16th play of a song is worth 21, "
    L"the 100th 31. After ten plays of one song in a day, its play points are halved for the rest of the day.\n"
    L"Discovery: +10 the first time a song counts as played.  Rediscovery: +15 for a song you hadn't played in a year.\n"
    L"Milestones: +20 at a song's 10th play, +50 at 25, +100 at 50, +250 at 100, +500 at 250, +1,000 at 500, "
    L"+2,500 at every 1,000.\n"
    L"Album runs: four or more songs of one album in order, +30 and +5 per song after that; a whole album +50 more.\n"
    L"Collecting: +2 for each song added to the library, +20 for each album new to it.\n"
    L"Streak: a day with 10 minutes of listening keeps the streak going; each day adds 5% to listening and play "
    L"points, up to +50%.\n\n"
    L"Points are never taken away. Skips still earn their listening minutes. The games don't earn points.";

// Paints the window: total and level, the level progress bar, the streak, then the 30-day chart
// and the recent list (or the rules page).
void PaintPoints(HDC dc, RECT rc) {
    int pad = S(24), y = S(20);
    uint32_t now = UnixNow();
    LevelInfo l = Level();
    FillSolid(dc, rc, WindowBg());
    SetBkMode(dc, TRANSPARENT);

    // Hero: the total, then the rank.
    std::wstring total = Grouped(l.total);
    int tw = TextWidth(dc, total, w.hero);
    Text(dc, total, {pad, y, pad + tw + S(4), y + S(56)}, w.hero, TextColor(), DT_LEFT | DT_TOP | DT_SINGLELINE);
    Text(dc, L"points", {pad + tw + S(8), y + S(28), rc.right - pad, y + S(52)}, g.font, SubColor(), DT_LEFT | DT_SINGLELINE);
    std::wstring rank = PointsRank(l.level);
    Text(dc, rank, {pad, y, rc.right - pad, y + S(28)}, w.title, Ref(P().accent), DT_RIGHT | DT_SINGLELINE);
    Text(dc, L"Level " + Grouped(l.level), {pad, y + S(30), rc.right - pad, y + S(50)}, g.font, SubColor(), DT_RIGHT | DT_SINGLELINE);
    y += S(68);

    // Progress to the next level.
    RECT track{pad, y, rc.right - pad, y + S(8)};
    Bar(dc, track, Ref(P().surface));
    double frac = l.next > l.start ? (double)(l.total - l.start) / (double)(l.next - l.start) : 0;
    Bar(dc, {track.left, track.top, track.left + (int)((track.right - track.left) * std::clamp(frac, 0.0, 1.0)), track.bottom},
        Ref(P().accent));
    y += S(14);
    std::wstring toNext = Grouped(l.next - l.total) + L" to level " + Grouped(l.level + 1);
    int nextRank = PointsNextRankLevel(l.level);
    std::wstring rankNote = nextRank ? std::wstring(PointsRank(nextRank)) + L" at level " + std::to_wstring(nextRank) : L"The highest rank";
    Text(dc, toNext, {pad, y, rc.right - pad, y + S(18)}, g.font, SubColor(), DT_LEFT | DT_SINGLELINE);
    Text(dc, rankNote, {pad, y, rc.right - pad, y + S(18)}, g.font, SubColor(), DT_RIGHT | DT_SINGLELINE);
    y += S(30);

    // Streak.
    int streak = PointsStreak(now);
    double mult = PointsMultiplier(now);
    wchar_t streakText[160];
    if (streak > 0)
        swprintf_s(streakText, L"%d-day streak  \x00B7  listening and plays earn \x00D7%.2f", streak, mult);
    else
        wcscpy_s(streakText, L"No streak yet: 10 minutes of music a day starts one.");
    Text(dc, streakText, {pad, y, rc.right - pad, y + S(20)}, g.fontBold, TextColor(), DT_LEFT | DT_SINGLELINE);
    y += S(34);

    if (w.rules) {
        Text(dc, L"How points work", {pad, y, rc.right - pad, y + S(24)}, w.title, TextColor(), DT_LEFT | DT_SINGLELINE);
        y += S(34);
        Text(dc, kRules, {pad, y, rc.right - pad, rc.bottom - S(50)}, g.font, TextColor(), DT_LEFT | DT_WORDBREAK);
    } else {
        // The last 30 days, one bar per day; the best day is labeled, the rest on hover.
        Text(dc, L"Last 30 days", {pad, y, rc.right - pad, y + S(20)}, g.fontBold, TextColor(), DT_LEFT | DT_SINGLELINE);
        y += S(26);
        uint32_t today = LocalDay(now);
        uint64_t days[30] = {};
        for (auto& [day, centi] : PointsByDay())
            if (day <= today && today - day < 30) days[29 - (today - day)] = Whole(centi);
        uint64_t maxv = std::max<uint64_t>(1, *std::max_element(std::begin(days), std::end(days)));
        int chartH = S(90);
        w.chart = {pad, y, rc.right - pad, y + chartH};
        int slot = (w.chart.right - w.chart.left) / 30, best = (int)(std::max_element(std::begin(days), std::end(days)) - days);
        FillSolid(dc, {w.chart.left, w.chart.bottom, w.chart.right, w.chart.bottom + 1}, Ref(P().border));  // baseline
        for (int i = 0; i < 30; ++i) {
            int x = w.chart.left + i * slot, h = (int)(chartH * days[i] / maxv);
            if (days[i] && h < S(2)) h = S(2);
            COLORREF c = i == w.hoverDay ? Ref(P().accent2) : Ref(P().accent);
            Bar(dc, {x + S(1), w.chart.bottom - h, x + slot - S(1), w.chart.bottom}, c);  // 2 px gap between bars
        }
        int label = w.hoverDay >= 0 ? w.hoverDay : days[best] ? best : -1;
        if (label >= 0) {
            std::wstring t = DayLabel(today - (29 - label)) + L": " + Grouped(days[label]);
            int x = w.chart.left + label * slot + slot / 2, lw = TextWidth(dc, t, g.font);
            int lx = std::clamp(x - lw / 2, (int)w.chart.left, (int)w.chart.right - lw);
            Text(dc, t, {lx, w.chart.bottom + S(4), lx + lw, w.chart.bottom + S(22)}, g.font, TextColor(), DT_LEFT | DT_SINGLELINE);
        }
        y = w.chart.bottom + S(34);

        Text(dc, L"Recent", {pad, y, rc.right - pad, y + S(20)}, g.fontBold, TextColor(), DT_LEFT | DT_SINGLELINE);
        y += S(26);
        const auto& recent = PointsRecent();
        int rowH = S(22);
        for (auto it = recent.rbegin(); it != recent.rend() && y + rowH < rc.bottom - S(44); ++it, y += rowH) {
            std::wstring amount = L"+" + Grouped(std::max<uint64_t>(1, Whole(it->centi)));
            Text(dc, amount, {pad, y, pad + S(56), y + rowH}, g.fontBold, Ref(P().accent), DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            std::wstring when = Ago(it->time, now);
            int ww = TextWidth(dc, when, g.font);
            Text(dc, EventText(*it), {pad + S(68), y, rc.right - pad - ww - S(12), y + rowH}, g.font, TextColor(),
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            Text(dc, when, {rc.right - pad - ww, y, rc.right - pad, y + rowH}, g.font, SubColor(), DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
        if (recent.empty())
            Text(dc, L"Play something: points arrive when a song ends.", {pad, y, rc.right - pad, y + rowH}, g.font, SubColor(),
                 DT_LEFT | DT_SINGLELINE);
    }

    std::wstring link = w.rules ? L"\x2190 Back" : L"How points work";
    int lw = TextWidth(dc, link, g.font);
    w.rulesLink = {pad, rc.bottom - S(34), pad + lw, rc.bottom - S(14)};
    Text(dc, link, w.rulesLink, g.font, Ref(P().accent), DT_LEFT | DT_SINGLELINE);
}

LRESULT CALLBACK PointsProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            w.hwnd = h;
            LOGFONTW lf{};
            GetObjectW(g.font, sizeof(lf), &lf);
            LONG base = lf.lfHeight;
            wcscpy_s(lf.lfFaceName, L"Segoe UI Semibold");
            lf.lfHeight = base * 40 / 9;
            w.hero = CreateFontIndirectW(&lf);
            lf.lfHeight = base * 15 / 9;
            w.title = CreateFontIndirectW(&lf);
            ApplyWindowTheme(h, P());
            SetTimer(h, 1, 1000, nullptr);  // "3 min ago" keeps up, and new points show up
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            DoubleBufferedPaint(h, PaintPoints);
            return 0;
        case WM_SIZE:
        case WM_TIMER:
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {S(460), S(560)};
            return 0;
        case WM_MOUSEMOVE: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int hover = -1;
            if (!w.rules && PtInRect(&w.chart, pt)) hover = std::clamp((int)((pt.x - w.chart.left) * 30 / std::max(1L, w.chart.right - w.chart.left)), 0, 29);
            SetCursor(LoadCursorW(nullptr, PtInRect(&w.rulesLink, pt) ? IDC_HAND : IDC_ARROW));
            if (hover != w.hoverDay) {
                w.hoverDay = hover;
                InvalidateRect(h, nullptr, FALSE);
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
                TrackMouseEvent(&tme);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            w.hoverDay = -1;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) return TRUE;
            break;
        case WM_LBUTTONUP: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (PtInRect(&w.rulesLink, pt)) {
                w.rules = !w.rules;
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) DestroyWindow(h);
            return 0;
        case WM_NCDESTROY:
            for (HFONT f : {w.hero, w.title})
                if (f) DeleteObject(f);
            w = PointsWindow{};
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

void ShowPoints() {
    if (w.hwnd) {
        SetForegroundWindow(w.hwnd);
        return;
    }
    RegisterAppClass(kPointsClass, PointsProc);
    RECT r{0, 0, S(520), S(640)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, g.dpi);
    RECT owner;
    GetWindowRect(g.hwnd, &owner);
    int ww = r.right - r.left, hh = r.bottom - r.top;
    CreateWindowExW(0, kPointsClass, L"cordinal points", WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX, owner.right - ww - S(40),
                    owner.top + S(60), ww, hh, g.hwnd, nullptr, g.inst, nullptr);
    ShowWindow(w.hwnd, SW_SHOW);
}

// ---------------------------------------------------------------------------
// The chip in the now-playing strip

// Called after every award: the window and chip update, a "+N" rises, a level-up flashes.
void AnnouncePoints(const PointsAward& award) {
    if (award.centi >= 100) {
        chip.pop = L"+" + Grouped(Whole(award.centi));
        chip.popAt = GetTickCount64();
    }
    // With the chip hidden, points keep adding up quietly: no level-up message, toast or sound.
    if (award.levelAfter > award.levelBefore && g.layout.points) {
        chip.levelUpFrames = 40;
        bool newRank = PointsRank(award.levelAfter) != std::wstring(PointsRank(award.levelBefore));
        std::wstring text = L"Level " + std::to_wstring(award.levelAfter) + L": " + PointsRank(award.levelAfter);
        if (newRank) text += L". New rank!";
        ShowMessage(text);
        ShowToast(L"Level " + std::to_wstring(award.levelAfter) + L"!",
                  newRank ? std::wstring(L"New rank: ") + PointsRank(award.levelAfter)
                          : std::wstring(PointsRank(award.levelAfter)) + L" \x00B7 click for your progress");
        PlaySfx(Sfx::Milestone);
    }
    if (g.pointsChip) InvalidateRect(g.pointsChip, nullptr, FALSE);
    if (w.hwnd) InvalidateRect(w.hwnd, nullptr, FALSE);
}

// Ticks with the UI timer: animates the "+N" and the level-up glow.
void AnimatePointsChip() {
    if (!g.pointsChip) return;
    bool animating = chip.levelUpFrames > 0 || (chip.popAt && GetTickCount64() - chip.popAt < 2200);
    if (chip.levelUpFrames > 0) --chip.levelUpFrames;
    if (animating || chip.popAt) InvalidateRect(g.pointsChip, nullptr, FALSE);
    if (!animating) chip.popAt = 0;
}

void DrawPointsChip(HDC dc, RECT r) {
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, r.right - r.left, r.bottom - r.top);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    RECT rc{0, 0, r.right - r.left, r.bottom - r.top};
    FillSolid(mem, rc, SurfaceColor());
    SetBkMode(mem, TRANSPARENT);
    LevelInfo l = Level();
    bool glow = chip.levelUpFrames > 0 && (chip.levelUpFrames / 5) % 2 == 0;
    // Line 1: rank and level. Line 2: points and a thin progress bar.
    std::wstring rank = std::wstring(PointsRank(l.level)) + L"  \x00B7  level " + std::to_wstring(l.level);
    Text(mem, rank, {0, 0, rc.right, S(16)}, g.fontBold, glow ? Ref(P().accent) : TextColor(), DT_RIGHT | DT_SINGLELINE);
    std::wstring pts = Grouped(l.total) + L" cp";
    double since = chip.popAt ? (GetTickCount64() - chip.popAt) / 2200.0 : 1.0;
    if (since < 1.0) {
        // "+31" in the accent while it rises, then the total again.
        int lift = (int)(S(6) * since);
        Text(mem, chip.pop, {0, S(16) - lift, rc.right, S(32) - lift}, g.fontBold, Ref(P().accent), DT_RIGHT | DT_SINGLELINE);
    } else {
        Text(mem, pts, {0, S(16), rc.right, S(32)}, g.font, SubColor(), DT_RIGHT | DT_SINGLELINE);
    }
    double frac = l.next > l.start ? (double)(l.total - l.start) / (double)(l.next - l.start) : 0;
    RECT bar{rc.right - S(90), rc.bottom - S(3), rc.right, rc.bottom - S(1)};
    FillSolid(mem, bar, Ref(P().border));
    FillSolid(mem, {bar.left, bar.top, bar.left + (int)((bar.right - bar.left) * std::clamp(frac, 0.0, 1.0)), bar.bottom},
              Ref(P().accent));
    BitBlt(dc, r.left, r.top, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

LRESULT CALLBACK PointsChipProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            DrawPointsChip(dc, rc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        case WM_LBUTTONUP:
            ShowPoints();
            return 0;
        case WM_NCDESTROY:
            RemoveWindowSubclass(h, PointsChipProc, 1);
            break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

void CreatePointsChip(HWND parent) {
    g.pointsChip = CreateWindowExW(0, WC_STATICW, L"", WS_CHILD | WS_VISIBLE | SS_NOTIFY, 0, 0, 0, 0, parent, nullptr, g.inst, nullptr);
    SetWindowSubclass(g.pointsChip, PointsChipProc, 1, 0);
    if (g.tips) {
        TTTOOLINFOW ti{sizeof(ti)};
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = parent;
        ti.uId = (UINT_PTR)g.pointsChip;
        ti.lpszText = const_cast<wchar_t*>(L"cordinal points: click for your rank, streak and recent points");
        SendMessageW(g.tips, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    }
}

}  // namespace app
