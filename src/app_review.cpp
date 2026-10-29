// Your year in music (View ▸ Games and stats): totals, favorites and listening habits from the
// play log, for one calendar year or all time, plus what you played on this day in earlier years.
#include "app.h"
#include "playlog.h"

namespace app {

namespace {

const wchar_t kReviewClass[] = L"CordinalReview";

struct Review {
    HWND hwnd = nullptr;
    HFONT hero = nullptr, title = nullptr;
    int year = 0;  // 0 = all time
    PeriodStats stats;
    std::vector<TrackId> onThisDay;
    RECT prev{}, next{}, allTime{};
    std::vector<std::pair<RECT, std::wstring>> tips;  // hover readouts for the habit bars
    int hoverTip = -1;
} v;

int CurrentYear() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    return st.wYear;
}

// Unix time of local midnight on January 1st of `year`.
uint32_t YearStart(int year) {
    SYSTEMTIME local{(WORD)year, 1, 0, 1, 0, 0, 0, 0}, utc;
    TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc);
    FILETIME ft;
    SystemTimeToFileTime(&utc, &ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime) / 10000000ull;
    return t > 11644473600ull ? (uint32_t)(t - 11644473600ull) : 0;
}

// The year of the oldest play-log entry (the log is chronological).
int FirstYear() {
    const auto& log = PlayLog();
    if (log.empty()) return CurrentYear();
    FILETIME ft, local;
    uint64_t t = ((uint64_t)log.front().time + 11644473600ull) * 10000000ull;
    ft = {(DWORD)t, (DWORD)(t >> 32)};
    FileTimeToLocalFileTime(&ft, &local);
    SYSTEMTIME st;
    FileTimeToSystemTime(&local, &st);
    return st.wYear;
}

// Recomputes the period stats and the "on this day" picks for the selected year (0 = all time).
void Recompute() {
    uint32_t from = v.year ? YearStart(v.year) : 0, to = v.year ? YearStart(v.year + 1) : UINT32_MAX;
    v.stats = ComputeStats(from, to, 5);
    v.onThisDay = PlayedOnThisDay(UnixNow());
    if (v.onThisDay.size() > 5) v.onThisDay.resize(5);
}

std::wstring Grouped(uint64_t n) {
    std::wstring digits = std::to_wstring(n), out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0) out += L',';
        out += digits[i];
    }
    return out;
}

std::wstring Hours(uint64_t seconds) {
    uint64_t minutes = seconds / 60;
    if (minutes < 120) return Grouped(minutes) + L" min";
    return Grouped(minutes / 60) + L" h " + std::to_wstring(minutes % 60) + L" min";
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

std::wstring TrackName(TrackId id) {
    if (id >= g_lib.Count()) return L"";
    const Track& t = g_lib[id];
    std::wstring s = t.title ? g_lib.Str(t.title) : g_lib.Str(t.file);
    StringPool::Handle artist = t.artist ? t.artist : t.albumArtist;
    if (artist) s += L" \x2014 " + g_lib.Str(artist);
    return s;
}

// A ranked list: name, then a thin bar for plays with the count after it.
int PaintTop(HDC dc, int x, int y, int width, const wchar_t* heading, const std::vector<TopEntry>& entries, bool tracks) {
    Text(dc, heading, {x, y, x + width, y + S(20)}, g.fontBold, TextColor(), DT_LEFT | DT_SINGLELINE);
    y += S(24);
    if (entries.empty()) {
        Text(dc, L"Nothing yet", {x, y, x + width, y + S(18)}, g.font, SubColor(), DT_LEFT | DT_SINGLELINE);
        return y + S(28);
    }
    uint32_t maxPlays = std::max<uint32_t>(1, entries[0].plays);
    for (size_t i = 0; i < entries.size(); ++i) {
        const TopEntry& e = entries[i];
        std::wstring name = tracks ? TrackName(e.track) : g_lib.Str(e.key);
        if (name.empty()) name = L"(unknown)";
        Text(dc, std::to_wstring(i + 1), {x, y, x + S(16), y + S(18)}, g.font, SubColor(), DT_LEFT | DT_SINGLELINE);
        Text(dc, name, {x + S(18), y, x + width, y + S(18)}, g.font, TextColor(), DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        std::wstring count = Grouped(e.plays);
        int cw = TextWidth(dc, count, g.font), barMax = width - S(18) - cw - S(8);
        int bw = std::max(S(3), (int)((int64_t)barMax * e.plays / maxPlays));
        Bar(dc, {x + S(18), y + S(21), x + S(18) + bw, y + S(25)}, Ref(P().accent));
        Text(dc, count, {x + S(18) + bw + S(6), y + S(15), x + width, y + S(31)}, g.font, SubColor(), DT_LEFT | DT_SINGLELINE);
        y += S(36);
    }
    return y + S(6);
}

// Vertical bars for a habit (hours or weekdays): magnitude in one hue, the peak labeled.
int PaintHabit(HDC dc, int x, int y, int width, const wchar_t* heading, const uint32_t* values, int count,
               const wchar_t* const* labels, int labelEvery) {
    Text(dc, heading, {x, y, x + width, y + S(20)}, g.fontBold, TextColor(), DT_LEFT | DT_SINGLELINE);
    y += S(26);
    int h = S(64), slot = width / count;
    uint32_t maxv = std::max<uint32_t>(1, *std::max_element(values, values + count));
    int peak = (int)(std::max_element(values, values + count) - values);
    FillSolid(dc, {x, y + h, x + slot * count, y + h + 1}, Ref(P().border));
    for (int i = 0; i < count; ++i) {
        int bh = (int)((int64_t)h * values[i] / maxv);
        if (values[i] && bh < S(2)) bh = S(2);
        RECT r{x + i * slot + S(1), y + h - bh, x + (i + 1) * slot - S(1), y + h};
        int tipIndex = (int)v.tips.size();
        Bar(dc, r, Ref(tipIndex == v.hoverTip ? P().accent2 : P().accent));
        v.tips.push_back({{x + i * slot, y, x + (i + 1) * slot, y + h}, std::wstring(labels[i]) + L": " + Grouped(values[i]) + L" plays"});
        if (i % labelEvery == 0)
            Text(dc, labels[i], {x + i * slot - S(10), y + h + S(3), x + i * slot + slot + S(10), y + h + S(19)}, g.font,
                 SubColor(), DT_CENTER | DT_SINGLELINE);
    }
    if (values[peak] && v.hoverTip < 0)
        Text(dc, L"Most: " + std::wstring(labels[peak]), {x, y - S(22), x + slot * count, y - S(4)}, g.font, SubColor(),
             DT_RIGHT | DT_SINGLELINE);
    return y + h + S(30);
}

// Paints the window: period header, stat tiles, favorites, habit bars and the facts column.
void PaintReview(HDC dc, RECT rc) {
    FillSolid(dc, rc, WindowBg());
    SetBkMode(dc, TRANSPARENT);
    v.tips.clear();
    int pad = S(24), y = S(18);
    const PeriodStats& s = v.stats;

    // Header: the period, with arrows to step through the years.
    std::wstring period = v.year ? std::to_wstring(v.year) : L"All time";
    Text(dc, period + L" in music", {pad, y, rc.right - pad, y + S(36)}, v.title, TextColor(), DT_LEFT | DT_SINGLELINE);
    int ax = rc.right - pad;
    auto link = [&](const std::wstring& t, RECT& r, bool enabled) {
        int lw = TextWidth(dc, t, g.font);
        r = {ax - lw, y + S(8), ax, y + S(28)};
        Text(dc, t, r, g.font, enabled ? Ref(P().accent) : Ref(P().faint), DT_LEFT | DT_SINGLELINE);
        if (!enabled) r = {};
        ax -= lw + S(16);
    };
    link(L"All time", v.allTime, v.year != 0);
    link(L"Next \x203A", v.next, v.year && v.year < CurrentYear());
    link(L"\x2039 Previous", v.prev, v.year == 0 || v.year > FirstYear());
    y += S(52);

    if (PlayLog().empty()) {
        Text(dc, L"cordinal keeps a play log from now on. Come back after some listening: every song you finish or skip "
                 L"ends up here.",
             {pad, y, rc.right - pad, y + S(80)}, g.font, SubColor(), DT_LEFT | DT_WORDBREAK);
        return;
    }

    // Four stat tiles.
    struct Tile { std::wstring value, label; } tiles[] = {
        {Hours(s.seconds), L"listened"},
        {Grouped(s.plays), s.plays == 1 ? L"play" : L"plays"},
        {Grouped(s.distinctTracks), s.distinctTracks == 1 ? L"song" : L"different songs"},
        {Grouped(s.discoveries), L"heard for the first time"},
    };
    int tileW = (rc.right - pad * 2 - S(12) * 3) / 4;
    for (int i = 0; i < 4; ++i) {
        int x = pad + i * (tileW + S(12));
        RECT tile{x, y, x + tileW, y + S(74)};
        FillSolid(dc, tile, Ref(P().surface));
        Text(dc, tiles[i].value, {x + S(12), y + S(10), tile.right - S(8), y + S(44)}, v.hero, TextColor(), DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        Text(dc, tiles[i].label, {x + S(12), y + S(46), tile.right - S(8), y + S(66)}, g.font, SubColor(), DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    y += S(98);

    // Left: favorites. Right: habits and facts.
    int colW = (rc.right - pad * 3) / 2, lx = pad, rx = pad * 2 + colW;
    int ly = PaintTop(dc, lx, y, colW, L"Top songs", s.topTracks, true);
    ly = PaintTop(dc, lx, ly, colW, L"Top artists", s.topArtists, false);
    PaintTop(dc, lx, ly, colW, L"Top albums", s.topAlbums, false);

    static const wchar_t* const hours[24] = {L"12 am", L"1 am", L"2 am", L"3 am", L"4 am", L"5 am", L"6 am", L"7 am",
                                             L"8 am", L"9 am", L"10 am", L"11 am", L"12 pm", L"1 pm", L"2 pm", L"3 pm",
                                             L"4 pm", L"5 pm", L"6 pm", L"7 pm", L"8 pm", L"9 pm", L"10 pm", L"11 pm"};
    static const wchar_t* const days[7] = {L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat", L"Sun"};
    int ry = PaintHabit(dc, rx, y, colW, L"Time of day", s.byHour, 24, hours, 6);
    ry = PaintHabit(dc, rx, ry, colW, L"Day of the week", s.byWeekday, 7, days, 1);
    if (s.longestStreak)
        Text(dc, L"Longest streak: " + Grouped(s.longestStreak) + (s.longestStreak == 1 ? L" day" : L" days in a row"),
             {rx, ry, rx + colW, ry + S(20)}, g.font, TextColor(), DT_LEFT | DT_SINGLELINE);
    ry += S(22);
    if (s.busiestDaySeconds) {
        uint64_t t = ((uint64_t)s.busiestDay + 134774ull) * 86400ull * 10000000ull;
        FILETIME ft{(DWORD)t, (DWORD)(t >> 32)};
        SYSTEMTIME st;
        FileTimeToSystemTime(&ft, &st);
        wchar_t date[64];
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, &st, nullptr, date, 64, nullptr);
        Text(dc, L"Busiest day: " + std::wstring(date) + L" (" + Hours(s.busiestDaySeconds) + L")", {rx, ry, rx + colW, ry + S(20)},
             g.font, TextColor(), DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    ry += S(36);
    if (!v.onThisDay.empty()) {
        Text(dc, L"On this day, in earlier years", {rx, ry, rx + colW, ry + S(20)}, g.fontBold, TextColor(), DT_LEFT | DT_SINGLELINE);
        ry += S(24);
        for (TrackId id : v.onThisDay) {
            Text(dc, TrackName(id), {rx, ry, rx + colW, ry + S(18)}, g.font, TextColor(), DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            ry += S(20);
        }
    }
    if (v.hoverTip >= 0 && v.hoverTip < (int)v.tips.size()) {
        auto& [r, t] = v.tips[v.hoverTip];
        int tw = TextWidth(dc, t, g.font) + S(12);
        int x = std::clamp((int)(r.left + r.right) / 2 - tw / 2, pad, (int)rc.right - pad - tw);
        RECT box{x, r.top - S(26), x + tw, r.top - S(4)};
        FillSolid(dc, box, Ref(P().text));
        Text(dc, t, box, g.font, WindowBg(), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

LRESULT CALLBACK ReviewProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            v.hwnd = h;
            LOGFONTW lf{};
            GetObjectW(g.font, sizeof(lf), &lf);
            LONG base = lf.lfHeight;
            wcscpy_s(lf.lfFaceName, L"Segoe UI Semibold");
            lf.lfHeight = base * 22 / 9;
            v.hero = CreateFontIndirectW(&lf);
            lf.lfHeight = base * 20 / 9;
            v.title = CreateFontIndirectW(&lf);
            ApplyWindowTheme(h, P());
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            DoubleBufferedPaint(h, PaintReview);
            return 0;
        case WM_SIZE:
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {S(700), S(700)};
            return 0;
        case WM_MOUSEMOVE: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int hover = -1;
            for (size_t i = 0; i < v.tips.size(); ++i)
                if (PtInRect(&v.tips[i].first, pt)) hover = (int)i;
            bool link = PtInRect(&v.prev, pt) || PtInRect(&v.next, pt) || PtInRect(&v.allTime, pt);
            SetCursor(LoadCursorW(nullptr, link ? IDC_HAND : IDC_ARROW));
            if (hover != v.hoverTip) {
                v.hoverTip = hover;
                InvalidateRect(h, nullptr, FALSE);
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
                TrackMouseEvent(&tme);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            v.hoverTip = -1;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) return TRUE;
            break;
        case WM_LBUTTONUP: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int year = v.year;
            if (PtInRect(&v.prev, pt)) year = v.year ? v.year - 1 : CurrentYear();
            else if (PtInRect(&v.next, pt)) year = v.year + 1;
            else if (PtInRect(&v.allTime, pt)) year = 0;
            if (year != v.year) {
                v.year = year;
                Recompute();
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) DestroyWindow(h);
            return 0;
        case WM_NCDESTROY:
            for (HFONT f : {v.hero, v.title})
                if (f) DeleteObject(f);
            v = Review{};
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

void ShowReview() {
    if (!v.hwnd) {
        RegisterAppClass(kReviewClass, ReviewProc);
        v.year = CurrentYear();
        Recompute();
        RECT r{0, 0, S(820), S(760)};
        AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, g.dpi);
        RECT owner;
        GetWindowRect(g.hwnd, &owner);
        int ww = r.right - r.left, hh = r.bottom - r.top;
        CreateWindowExW(0, kReviewClass, L"Year in review", WS_OVERLAPPEDWINDOW, owner.left + (owner.right - owner.left - ww) / 2,
                        owner.top + S(30), ww, hh, g.hwnd, nullptr, g.inst, nullptr);
        ShowWindow(v.hwnd, SW_SHOW);
    }
    SetForegroundWindow(v.hwnd);
}

}  // namespace app
