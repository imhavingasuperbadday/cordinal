// Sections on the waveform (the seek bar, app_waveform.cpp): right-click it to play from a spot,
// mark where a section starts and ends, and loop that section while it plays. The point of marking
// one is that cordinal keeps it: it is stored in settings.ini against the file's audio identity
// (peaks.h PeaksKeyName), so it comes back the next time the track plays, in any playlist.
//
// The section is drawn over the peaks as a stippled band with a mark either end. "Loop" makes the
// section a loop in the track's own timeline (engine.h PlayItem): the join is exact and the
// playhead never leaves the section. The rest of the waveform stays available for editing.
#include "app.h"
#include "peaks.h"

namespace app {

namespace {

const wchar_t kIniSection[] = L"Sections";
// What "Loop this section" runs to when the track has no chapter to stop at.
constexpr double kDefaultSection = 30.0;
TrackId g_regionFor = kNone;  // the track g.regionStart/End/On belong to

// A section needs a real span; a mark pair closer than 50 ms is treated as no section.
bool HasRegion() { return g.regionEnd - g.regionStart > 0.05; }

// The file's audio identity as 16 hex digits: a section belongs to the audio, not to a spot in a
// playlist, so it follows the file and comes back in whatever playlist plays it.
std::wstring IdentityOf(TrackId id) {
    if (id == kNone || id >= g_lib.Count()) return L"";
    const Track& t = g_lib[id];
    return PeaksKeyName(g_lib.Path(id), t.fileSize, t.fileTime);
}

Region ReadRegion(TrackId id) {
    Region r;
    std::wstring key = IdentityOf(id);
    if (key.empty()) return r;
    std::vector<int> v = ParseInts(IniRead(kIniSection, key.c_str(), L""));
    if (v.size() == 3 && v[0] >= 0 && v[1] > v[0]) {
        r.start = v[0] / 1000.0;
        r.end = v[1] / 1000.0;
        r.on = v[2] != 0 && r.end - r.start > 0.05;
    }
    return r;
}

// Only a playing-track change updates the UI's section. Prefetching another track's item
// reads its saved section separately, so it cannot erase the current track's marks.
void LoadRegion(TrackId id) {
    g_regionFor = id;
    Region r = ReadRegion(id);
    g.regionStart = r.start;
    g.regionEnd = r.end;
    g.regionOn = r.on;
}

// "m:ss.s" - short, and fine enough to tell two nearby marks apart.
std::wstring ShortTime(double seconds) {
    wchar_t b[32];
    swprintf_s(b, L"%d:%04.1f", (int)(seconds / 60), seconds - 60 * (int)(seconds / 60));
    return b;
}

// The section is stored as "startMs,endMs,on" under its file's identity, so an edited or replaced
// file has no section rather than a section in the wrong place.
void Store(TrackId id) {
    std::wstring key = IdentityOf(id);
    if (key.empty()) return;
    if (!HasRegion()) {
        IniWrite(kIniSection, key.c_str(), L"");  // an empty value removes the line
        return;
    }
    IniWrite(kIniSection, key.c_str(),
             std::to_wstring((long long)std::llround(g.regionStart * 1000)) + L"," +
                 std::to_wstring((long long)std::llround(g.regionEnd * 1000)) + L"," + (g.regionOn ? L"1" : L"0"));
}

// A change to the section repaints the bar that shows it.
void Changed() {
    if (g.seek) InvalidateRect(g.seek, nullptr, FALSE);
}

// The section a mark at `pos` would make: up to the next chapter when the file names one, or 30 s.
void SectionAt(double pos, double dur) {
    double end = ChapterStartAfter(pos);
    if (end <= pos + 0.5) end = pos + kDefaultSection;
    g.regionStart = pos;
    g.regionEnd = std::min(end, dur > 0 ? dur : end);
    g.regionOn = false;
}

}  // namespace

// The seek bar's own mapping (BarFracAt, shared with DragBarTo), so the section under the cursor is
// the one the click would seek to; the self-test checks that it is.
double RegionTimeAt(HWND bar, POINT pt) { return BarFracAt(bar, pt) * CurrentDuration(); }

// The section as the rest of the app sees it, but only for the track it was loaded for, so a
// stale section can't leak onto another track.
Region CurrentRegion() {
    Region r;
    if (g.playing == kNone || g.playing != g_regionFor) return r;
    r.start = g.regionStart;
    r.end = g.regionEnd;
    r.on = g.regionOn && HasRegion();
    return r;
}

// `id`'s section, reading it in when it isn't the one loaded (MakeItem asks this for the track
// about to play, so the engine's item can arm the loop). g.playing itself is not disturbed.
Region RegionOf(TrackId id) {
    return id == g_regionFor ? Region{g.regionStart, g.regionEnd, g.regionOn && HasRegion()} : ReadRegion(id);
}

// The playing track changed: swap in the section that belongs to it.
void RegionTrackChanged() {
    if (g.playing == g_regionFor) return;
    LoadRegion(g.playing);
    Changed();
}

// The seek bar's right-click menu. Every item acts on where the cursor is. Items that can't do
// anything are grayed rather than hidden, so the menu keeps its shape and the cursor can aim
// without the item under it moving.
bool CommandRegion(int id, double pos);

bool ShowRegionMenu(POINT pt) {
    if (g.playing == kNone || g.playing != g_regionFor) return false;
    if (pt.x == -1 && pt.y == -1) GetCursorPos(&pt);
    POINT client = pt;
    ScreenToClient(g.seek, &client);
    double dur = CurrentDuration();
    if (dur <= 0) return false;
    double pos = std::clamp(RegionTimeAt(g.seek, client), 0.0, dur);
    bool on = HasRegion();
    std::wstring loop =
        on ? L"&Loop " + ShortTime(g.regionStart) + L" to " + ShortTime(g.regionEnd)
           : L"&Loop this section (" + ShortTime(pos) + L" to " + ShortTime(std::min(pos + kDefaultSection, dur)) + L")";
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_REGION_PLAY, (L"&Play from " + ShortTime(pos)).c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (on && g.regionOn ? MF_CHECKED : 0), IDM_REGION_LOOP, loop.c_str());
    AppendMenuW(m, MF_STRING, IDM_REGION_START, (L"Section &starts here (" + ShortTime(pos) + L")").c_str());
    AppendMenuW(m, MF_STRING, IDM_REGION_END, (L"Section &ends here (" + ShortTime(pos) + L")").c_str());
    AppendMenuW(m, MF_STRING | (on ? MF_ENABLED : MF_GRAYED), IDM_REGION_CLEAR, L"&Clear section");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    // The big views have no menu bar, so their own menu has to stay reachable from here; the normal
    // window's player is customized from here too (app_custom.cpp).
    if (g.size != ViewSize::Normal || !GetMenu(g.hwnd)) AppendMenuW(m, MF_STRING, IDM_REGION_PLAYER_MENU, L"&Player menu...");
    if (g.size == ViewSize::Normal) {
        HMENU layout = LayoutMenu();
        UpdateLayoutMenu(layout);
        AppendMenuW(m, MF_POPUP, (UINT_PTR)layout, L"Customize la&yout");
    }
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g.hwnd, nullptr);
    DestroyMenu(m);
    if (CommandRegion(cmd, pos)) return true;
    if (cmd && cmd != IDM_REGION_PLAYER_MENU) {
        OnCommand(cmd, 0, nullptr);
        return true;
    }
    switch (cmd) {
        case IDM_REGION_PLAYER_MENU:
            ShowCompactMenu(pt);
            break;
        default:
            break;
    }
    return true;
}

// What a menu item does, with the spot the menu was opened at. Shared with the command table
// (IDM_REGION_LOOP / IDM_REGION_CLEAR, app.cpp) and the self-test, so all of them arm a section
// the same way the menu does.
bool CommandRegion(int id, double pos) {
    double dur = CurrentDuration();
    if (g.playing == kNone || g.playing != g_regionFor || dur <= 0) return false;
    pos = std::clamp(pos, 0.0, dur);
    switch (id) {
        case IDM_REGION_PLAY:
            RequestSeek(pos);
            return true;
        case IDM_REGION_LOOP:
            // A section that is already marked keeps its marks: arming it must not throw away what
            // it is. Only a menu click on a track with no marks makes one, from the spot it opened
            // at (up to the next chapter).
            if (g.regionEnd <= g.regionStart + 0.05) SectionAt(pos, dur);
            g.regionOn = true;
            Store(g.playing);
            Changed();
            PlaySfx(Sfx::PlayNext, 0, true);  // there is no other sign that the wrap is armed
            // Seek to where the playhead already is: the engine rebuilds the item from the section,
            // so arming takes effect now rather than at the next track.
            RequestSeek(g.engine.Position() < g.regionStart ? g.regionStart : g.engine.Position());
            return true;
        case IDM_REGION_START:
            g.regionStart = pos;
            if (g.regionEnd <= pos + 0.05) g.regionEnd = std::min(pos + kDefaultSection, dur);
            g.regionOn = false;  // moving a mark is still editing the section: re-arm it with Loop
            Store(g.playing);
            Changed();
            RequestSeek(g.engine.Position());
            return true;
        case IDM_REGION_END:
            g.regionEnd = pos;
            if (g.regionStart >= pos - 0.05) g.regionStart = std::max(0.0, pos - kDefaultSection);
            g.regionOn = false;
            Store(g.playing);
            Changed();
            RequestSeek(g.engine.Position());
            return true;
        case IDM_REGION_CLEAR:
            g.regionStart = g.regionEnd = 0;
            g.regionOn = false;
            Store(g.playing);
            Changed();
            // The same rebuild as arming: the decoded item no longer loops, and seeks outside the
            // section are free again.
            RequestSeek(g.engine.Position());
            return true;
        default:
            return false;
    }
}

// WM_CONTEXTMENU on the seek bar, from either the mouse (screen coordinates) or the keyboard
// (-1, -1), wherever it came from. False lets it through to the window.
bool RegionMouse(const MSG& msg) {
    if (msg.message != WM_CONTEXTMENU || (HWND)msg.wParam != g.seek) return false;
    POINT pt{GET_X_LPARAM(msg.lParam), GET_Y_LPARAM(msg.lParam)};
    return ShowRegionMenu(pt);
}

// The section, over whatever the waveform drew: a stippled band between the two marks, and the
// marks themselves in the accent. GDI has no alpha fill, and a checker keeps the peaks visible
// under the band, so a section reads the same over loud and quiet passages.
void DrawRegion(HDC dc, const RECT& rc, const RECT& channel) {
    if (!HasRegion() || g.playing == kNone || g.playing != g_regionFor) return;
    double dur = CurrentDuration();
    if (dur <= 0) return;
    int left = channel.left, width = channel.right - channel.left;
    if (width <= 0) return;
    int x0 = std::clamp((int)left + (int)(width * (g.regionStart / dur)), (int)left, (int)channel.right);
    int x1 = std::clamp((int)left + (int)(width * (g.regionEnd / dur)), (int)left, (int)channel.right);
    if (x1 <= x0) return;
    RECT band{x0, rc.top, x1, rc.bottom};
    WORD bits[8] = {0x5555, 0xAAAA, 0x5555, 0xAAAA, 0x5555, 0xAAAA, 0x5555, 0xAAAA};
    HBITMAP tile = CreateBitmap(8, 8, 1, 1, bits);
    HBRUSH brush = CreatePatternBrush(tile);
    HBRUSH oldBrush = (HBRUSH)SelectObject(dc, brush);
    COLORREF oldText = SetTextColor(dc, TextColor());
    int oldBk = SetBkMode(dc, OPAQUE);
    COLORREF oldBkColor = SetBkColor(dc, SurfaceColor());
    FillRect(dc, &band, brush);
    SetBkMode(dc, oldBk);
    SetBkColor(dc, oldBkColor);
    SetTextColor(dc, oldText);
    SelectObject(dc, oldBrush);
    DeleteObject(brush);
    DeleteObject(tile);
    int w = std::max(1, S(1));
    COLORREF accent = Ref(P().accent);
    FillSolid(dc, {x0, rc.top, x0 + w, rc.bottom}, accent);
    FillSolid(dc, {x1 - w, rc.top, x1, rc.bottom}, accent);
}

}  // namespace app
