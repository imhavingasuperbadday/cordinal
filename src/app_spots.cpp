// Coming back to music (part of Resume where you left off): the places in songs left partway, which
// playback keeps as songs are left or played to their end, the songs and albums gone quiet for longer
// than their play history suggests (rediscover.h), and Play history. One small popup shows them.
#include "app.h"
#include "playlog.h"
#include "rediscover.h"

namespace app {

std::vector<Rediscovery> Rediscoveries();

namespace {

constexpr size_t kMaxSpots = 12, kMaxSnoozed = 64, kMaxHistory = 400;

// The place worth keeping: a few seconds in, and not just about to end.
bool WorthKeeping(TrackId id, double pos) {
    if (id >= g_lib.Count() || !std::isfinite(pos) || pos < 5) return false;
    double duration = g_lib[id].Duration();
    return duration <= 0 || pos < duration - 5;
}

void Keep(std::vector<std::pair<TrackId, double>>& spots, TrackId id, double pos) {
    std::erase_if(spots, [id](const auto& s) { return s.first == id; });
    if (!WorthKeeping(id, pos)) return;
    spots.insert(spots.begin(), {id, pos});
    if (spots.size() > kMaxSpots) spots.resize(kMaxSpots);
}

// A rediscovery x'd out stays away until it is played again: the path of its newest play, and when.
std::vector<std::pair<std::wstring, uint32_t>> g_snoozed;
bool g_snoozedLoaded = false;

void LoadSnoozed() {
    if (g_snoozedLoaded) return;
    g_snoozedLoaded = true;
    std::wstring all = IniRead(L"Player", L"RediscoverSnoozed", L"");
    for (size_t at = 0; at < all.size();) {
        size_t end = all.find(L'\t', at);
        if (end == std::wstring::npos) end = all.size();
        std::wstring entry = all.substr(at, end - at);
        if (size_t bar = entry.find(L'|'); bar != std::wstring::npos)
            g_snoozed.emplace_back(entry.substr(bar + 1), (uint32_t)_wtoi64(entry.substr(0, bar).c_str()));
        at = end + 1;
    }
}

void SaveSnoozed() {
    std::wstring all;
    for (const auto& [path, last] : g_snoozed) all += (all.empty() ? L"" : L"\t") + std::to_wstring(last) + L"|" + path;
    IniWrite(L"Player", L"RediscoverSnoozed", all);
}

bool Snoozed(TrackId id) {
    std::wstring path = g_lib.Path(id);
    uint32_t last = g_lib[id].lastPlayed;
    return std::ranges::any_of(g_snoozed, [&](const auto& s) { return s.second == last && s.first == path; });
}

enum class Kind { Header, Spot, Rediscover, History };
struct Row {
    Kind kind;
    size_t index = 0;  // into g.spots, the rediscoveries or the history
    std::wstring title, line;
};

struct Popup {
    HWND hwnd = nullptr;
    bool history = false;
    std::vector<Row> rows;
    std::vector<Rediscovery> found;
    std::vector<TrackId> played;  // Play history, newest first
    int hover = -1, focus = -1, scroll = 0;
    bool overX = false;
} p;

int HeightOf(const Row& r) { return r.kind == Kind::Header ? S(30) : S(46); }
int XWidth() { return S(36); }
int ContentHeight() {
    int h = S(4);
    for (const Row& r : p.rows) h += HeightOf(r);
    return h;
}
int MaxScroll() {
    RECT c;
    GetClientRect(p.hwnd, &c);
    return std::max(0, ContentHeight() - (int)c.bottom);
}

std::wstring TimeText(uint32_t unix, bool date) {
    ULARGE_INTEGER t;
    t.QuadPart = (unix + 11644473600ULL) * 10000000ULL;
    FILETIME ft{t.LowPart, t.HighPart}, local;
    SYSTEMTIME st;
    FileTimeToLocalFileTime(&ft, &local);
    FileTimeToSystemTime(&local, &st);
    wchar_t buf[96] = L"";
    if (date) GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, L"dddd, MMMM d, yyyy", buf, 96, nullptr);
    else GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, nullptr, buf, 96);
    return buf;
}

std::wstring Field(TrackId id, const wchar_t* format) { return FormatTitle(format, Lookup(id, false)); }

void Build() {
    p.rows.clear();
    auto with = [](const std::wstring& a, const std::wstring& b) { return b.empty() ? a : a + L"  \x00B7  " + b; };
    if (p.history) {
        p.rows.push_back({Kind::Header, 0, L"Play history"});
        std::wstring day;
        const auto& log = PlayLog();
        for (size_t i = log.size(); i-- > 0 && p.played.size() < kMaxHistory;) {
            const PlayEvent& e = log[i];
            if (!(e.flags & PlayEvent::kCounted) || e.id >= g_lib.Count()) continue;
            std::wstring d = TimeText(e.time, true);
            if (d != day) p.rows.push_back({Kind::Header, 0, day = d});
            p.rows.push_back({Kind::History, p.played.size(), Field(e.id, L"%title%"),
                              with(TimeText(e.time, false), Field(e.id, L"[%artist%]"))});
            p.played.push_back(e.id);
        }
        if (p.played.empty()) p.rows.push_back({Kind::Header, 0, L"Songs you play to the end show up here."});
        return;
    }
    if (!g.spots.empty()) p.rows.push_back({Kind::Header, 0, L"Pick up where you left off"});
    for (size_t i = 0; i < g.spots.size(); ++i) {
        auto [id, pos] = g.spots[i];
        p.rows.push_back({Kind::Spot, i, Field(id, L"%title%"),
                          with(FormatDuration(pos) + L" / " + Field(id, L"[%length%]"), Field(id, L"[%artist%]"))});
    }
    if (!p.found.empty()) p.rows.push_back({Kind::Header, 0, L"Haven't heard in a while"});
    for (size_t i = 0; i < p.found.size(); ++i) {
        const Rediscovery& r = p.found[i];
        TrackId id = r.tracks.front();
        int days = (int)((UnixNow() - r.lastPlayed) / 86400);
        std::wstring ago = days >= 60 ? std::to_wstring(days / 30) + L" months ago" : std::to_wstring(days) + L" days ago";
        std::wstring plays = std::to_wstring((int)std::lround(r.plays)) + (r.album ? L" plays a track" : L" plays");
        p.rows.push_back({Kind::Rediscover, i, r.album ? L"Album: " + Field(id, L"%album%") : Field(id, L"%title%"),
                          with(Field(id, r.album ? L"[%album artist%]" : L"[%artist%]"), L"last played " + ago + L", " + plays)});
    }
}

bool Selectable(int i) { return i >= 0 && i < (int)p.rows.size() && p.rows[i].kind != Kind::Header; }

int RowAt(POINT pt, bool* overX) {
    RECT c;
    GetClientRect(p.hwnd, &c);
    int y = S(2) - p.scroll;
    for (int i = 0; i < (int)p.rows.size(); ++i) {
        int h = HeightOf(p.rows[i]);
        if (pt.y >= y && pt.y < y + h) {
            if (!Selectable(i)) return -1;
            if (overX) *overX = p.rows[i].kind != Kind::History && pt.x >= c.right - XWidth();
            return i;
        }
        y += h;
    }
    return -1;
}

// Opening, it goes where it was last dragged to (kept relative to the main window, so it follows the
// window), else centered over the window a little above the middle. Afterwards it keeps its place and
// only changes height as rows go.
void Place(bool opening) {
    int width = S(420), height = std::min(ContentHeight(), S(560));
    RECT owner, now;
    GetWindowRect(g.hwnd, &owner);
    GetWindowRect(p.hwnd, &now);
    int x = now.left, y = now.top;
    if (opening) {
        std::vector<int> at = ParseInts(IniRead(L"Player", L"SpotsAt", L""));
        if (at.size() == 2) {
            x = owner.left + at[0];
            y = owner.top + at[1];
        } else {
            x = owner.left + (owner.right - owner.left - width) / 2;
            y = owner.top + std::max(S(96), (int)(owner.bottom - owner.top - height) / 3);
        }
    }
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromRect(&owner, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& work = mi.rcWork;
    x = std::clamp(x, (int)work.left, std::max((int)work.left, (int)work.right - width));
    y = std::clamp(y, (int)work.top, std::max((int)work.top, (int)work.bottom - height));
    SetWindowPos(p.hwnd, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    p.scroll = std::clamp(p.scroll, 0, MaxScroll());
    InvalidateRect(p.hwnd, nullptr, FALSE);
}

void Refresh() {
    p.hover = -1;
    Build();
    if (p.rows.empty()) {
        PostMessageW(p.hwnd, WM_CLOSE, 0, 0);
        return;
    }
    while (p.focus >= (int)p.rows.size() || (p.focus >= 0 && !Selectable(p.focus))) --p.focus;
    Place(false);
}

void Remove(int i) {
    if (!Selectable(i)) return;
    const Row& r = p.rows[i];
    if (r.kind == Kind::Spot) {
        g.spots.erase(g.spots.begin() + r.index);
        if (g.list) InvalidateRect(g.list, nullptr, FALSE);  // its row loses its left-off mark
    } else if (r.kind == Kind::Rediscover) {
        const Rediscovery& found = p.found[r.index];
        TrackId newest = found.tracks.front();
        for (TrackId id : found.tracks)
            if (g_lib[id].lastPlayed > g_lib[newest].lastPlayed) newest = id;
        g_snoozed.emplace_back(g_lib.Path(newest), g_lib[newest].lastPlayed);
        if (g_snoozed.size() > kMaxSnoozed) g_snoozed.erase(g_snoozed.begin());
        SaveSnoozed();
        p.found.erase(p.found.begin() + r.index);
    } else {
        return;
    }
    Refresh();
}

void Choose(int i) {
    if (!Selectable(i)) return;
    Row r = p.rows[i];
    std::vector<TrackId> album;
    if (r.kind == Kind::Rediscover) album = p.found[r.index].tracks;
    TrackId played = r.kind == Kind::History ? p.played[r.index] : kNone;
    DestroyWindow(p.hwnd);
    if (r.kind == Kind::Spot) PlaySpot(r.index);
    else if (!album.empty()) PlayList(album, 0);
    else if (played < g_lib.Count()) PlayList({played}, 0);
}

void Paint(HDC dc) {
    RECT client;
    GetClientRect(p.hwnd, &client);
    FillSolid(dc, client, Ref(P().border));  // a hairline around the surface
    InflateRect(&client, -1, -1);
    FillSolid(dc, client, Ref(P().surface2));
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ old = SelectObject(dc, g.font);
    int y = S(2) - p.scroll;
    for (int i = 0; i < (int)p.rows.size(); ++i) {
        const Row& r = p.rows[i];
        RECT row{client.left, y, client.right, y + HeightOf(r)};
        y = row.bottom;
        if (row.bottom < 0 || row.top > client.bottom) continue;
        if (r.kind == Kind::Header) {
            RECT text{row.left + S(12), row.top + S(6), row.right - S(12), row.bottom};
            SelectObject(dc, g.fontBold ? g.fontBold : g.font);
            SetTextColor(dc, Ref(P().sub));
            if (i == 0) {  // the grip says the popup can be dragged by its headings
                SelectObject(dc, g.font);
                DrawTextW(dc, L"\x22EE\x22EE", -1, &text, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
                text.right -= S(24);
                SelectObject(dc, g.fontBold ? g.fontBold : g.font);
            }
            DrawTextW(dc, r.title.c_str(), -1, &text, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            continue;
        }
        bool lit = i == p.hover || (p.hover < 0 && i == p.focus);
        if (lit) FillSolid(dc, row, Ref(P().select));
        COLORREF main = Ref(lit ? P().selectText : P().text), sub = Ref(lit ? P().selectText : P().sub);
        bool x = r.kind != Kind::History;
        RECT text{row.left + S(12), row.top + S(5), row.right - (x ? XWidth() + S(4) : S(12)), row.top + HeightOf(r) / 2};
        SelectObject(dc, g.font);
        SetTextColor(dc, main);
        DrawTextW(dc, r.title.c_str(), -1, &text, DT_SINGLELINE | DT_BOTTOM | DT_END_ELLIPSIS | DT_NOPREFIX);
        text.top = text.bottom + S(1);
        text.bottom = row.bottom - S(5);
        SetTextColor(dc, sub);
        DrawTextW(dc, r.line.c_str(), -1, &text, DT_SINGLELINE | DT_TOP | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (!x) continue;
        // The x that takes this one off the list; it lights up on its own under the mouse.
        RECT box{row.right - XWidth(), row.top, row.right, row.bottom};
        if (i == p.hover && p.overX) {
            RECT dot{box.left + S(6), box.top + S(9), box.right - S(6), box.bottom - S(9)};
            FillSolid(dc, dot, Ref(P().surface2));
            SetTextColor(dc, Ref(P().text));
        }
        DrawTextW(dc, L"\x2715", -1, &box, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
    }
    SelectObject(dc, old);
}

void MoveFocus(int step) {
    int i = p.focus;
    do i += step;
    while (i >= 0 && i < (int)p.rows.size() && !Selectable(i));
    if (!Selectable(i)) return;
    p.focus = i;
    p.hover = -1;
    // Keep the focused row in view.
    int top = S(2);
    for (int k = 0; k < i; ++k) top += HeightOf(p.rows[k]);
    RECT c;
    GetClientRect(p.hwnd, &c);
    if (top - S(30) < p.scroll) p.scroll = std::max(0, top - S(30));
    else if (top + HeightOf(p.rows[i]) > p.scroll + c.bottom) p.scroll = top + HeightOf(p.rows[i]) - c.bottom;
    p.scroll = std::clamp(p.scroll, 0, MaxScroll());
    InvalidateRect(p.hwnd, nullptr, FALSE);
}

LRESULT CALLBACK SpotsProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            Paint(dc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_NCHITTEST: {  // a heading moves the popup, as a title bar would
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(h, &pt);
            return RowAt(pt, nullptr) < 0 ? HTCAPTION : HTCLIENT;
        }
        case WM_NCLBUTTONDBLCLK:  // not a real title bar: a double-click there must not maximize it
            return 0;
        case WM_EXITSIZEMOVE: {  // kept relative to the main window, so it follows the window next time
            RECT r, owner;
            GetWindowRect(h, &r);
            GetWindowRect(g.hwnd, &owner);
            IniWrite(L"Player", L"SpotsAt", JoinInts({(int)(r.left - owner.left), (int)(r.top - owner.top)}));
            return 0;
        }
        case WM_MOUSEMOVE: {
            bool overX = false;
            int row = RowAt({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}, &overX);
            if (row != p.hover || overX != p.overX) {
                p.hover = row;
                p.overX = overX;
                InvalidateRect(h, nullptr, FALSE);
            }
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
            return 0;
        }
        case WM_MOUSELEAVE:
            p.hover = -1;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_MOUSEWHEEL:
            p.scroll = std::clamp(p.scroll - GET_WHEEL_DELTA_WPARAM(wp) * S(46) * 3 / WHEEL_DELTA, 0, MaxScroll());
            p.hover = -1;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP: {
            bool overX = false;
            int row = RowAt({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}, &overX);
            if (overX) Remove(row);
            else Choose(row);
            return 0;
        }
        case WM_KEYDOWN:
            switch (wp) {
                case VK_ESCAPE: DestroyWindow(h); break;
                case VK_UP: MoveFocus(-1); break;
                case VK_DOWN: MoveFocus(1); break;
                case VK_RETURN: case VK_SPACE: Choose(p.focus); break;
                case VK_DELETE: case VK_BACK: Remove(p.focus); break;
            }
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) PostMessageW(h, WM_CLOSE, 0, 0);  // clicking elsewhere dismisses it
            return 0;
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_NCDESTROY:
            p = Popup{};
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void Open(bool history) {
    if (p.hwnd) DestroyWindow(p.hwnd);
    p.history = history;
    if (!history) p.found = Rediscoveries();
    Build();
    if (p.rows.empty()) return;
    RegisterAppClass(L"CordinalSpots", SpotsProc, CS_DROPSHADOW);
    p.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"CordinalSpots", history ? L"Play history" : L"Pick up where you left off",
                             WS_POPUP, 0, 0, 0, 0, g.hwnd, nullptr, g.inst, nullptr);
    if (!p.hwnd) return;
    Place(true);
    ShowWindow(p.hwnd, SW_SHOW);
    SetForegroundWindow(p.hwnd);
    SetFocus(p.hwnd);
}

}  // namespace

// Called just before the playing song is left on purpose (another song chosen, Next, Stop): its
// place joins the list, newest first. A song that plays to its end leaves through ForgetSpot instead.
void RememberSpot() {
    if (!g.resumeOn || g.playing == kNone || RadioPlaying()) return;
    Keep(g.spots, g.playing, g.engine.Position());
    g.spotLeft = g.playing;
}

void ForgetSpot(TrackId id) {
    std::erase_if(g.spots, [id](const auto& s) { return s.first == id; });
}

// Plays a remembered song from its place: in Now Playing if it is there, else on its own.
void PlaySpot(size_t i) {
    if (i >= g.spots.size() || g.spots[i].first >= g_lib.Count()) return;
    auto [id, pos] = g.spots[i];
    int np = FindPlaylist(PlaylistKind::NowPlaying), k = IndexIn(np, id);
    if (k >= 0) PlayTrack(np, k, true, pos);
    else PlayList({id}, 0, pos);
}

// Up to three albums and four songs, leaving out any x'd out since their last play and songs with a
// remembered place (they are listed above).
std::vector<Rediscovery> Rediscoveries() {
    LoadSnoozed();
    return FindRediscoveries(g_lib, UnixNow(), 3, 4, [](TrackId id) {
        return Snoozed(id) || std::ranges::any_of(g.spots, [id](const auto& s) { return s.first == id; });
    });
}

bool HasComebacks() {
    return g.resumeOn && (std::ranges::any_of(g.spots, [](const auto& s) { return s.first != g.playing; }) ||
                          !Rediscoveries().empty());
}

void ShowSpots() {
    if (g.resumeOn) Open(false);
}

void ShowPlayHistory() { Open(true); }

// At close: the playing song counts as left where it is, so it is offered next time too.
void SaveSpots() {
    auto spots = g.spots;
    if (g.resumeOn && g.playing != kNone && !RadioPlaying()) Keep(spots, g.playing, g.engine.Position());
    if (!g.resumeOn) spots.clear();
    IniWrite(L"Player", L"Spots", std::to_wstring(spots.size()));
    for (size_t i = 0; i < spots.size(); ++i)
        IniWrite(L"Player", (L"Spot" + std::to_wstring(i)).c_str(),
                 F2S((float)spots[i].second) + L"|" + g_lib.Path(spots[i].first));
}

void LoadSpots() {
    g.spots.clear();
    int count = std::clamp(IniInt(L"Player", L"Spots", 0), 0, (int)kMaxSpots);
    for (int i = 0; i < count; ++i) {
        std::wstring value = IniRead(L"Player", (L"Spot" + std::to_wstring(i)).c_str(), L"");
        size_t bar = value.find(L'|');
        if (bar == std::wstring::npos) continue;
        TrackId id = g_lib.Find(value.substr(bar + 1));
        double pos = _wtof(value.substr(0, bar).c_str());
        if (id < g_lib.Count() && WorthKeeping(id, pos)) g.spots.emplace_back(id, pos);
    }
}

}  // namespace app
