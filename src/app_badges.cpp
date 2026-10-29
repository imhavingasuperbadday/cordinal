// Small badges in the track list: the codec with its quality ("FLAC 96", "MP3 320"), "MIX" on
// long recordings (an hour or more: DJ sets, ambience videos, game-music medleys), and a spinner
// before the title of a track cordinal is working on (measuring it, reading or writing its tags,
// moving its file).
#include "app.h"

namespace app {

namespace {

// A recording this long (seconds) counts as a mix: DJ set, ambience or medley.
constexpr double kMixSeconds = 3600;

// Codec families the badge logic cares about.
bool Lossless(Codec c) { return c == Codec::FLAC || c == Codec::ALAC || c == Codec::PCM || c == Codec::Cord || c == Codec::DSD; }
bool GameMusic(Codec c) { return c == Codec::SPC || c == Codec::NSF || c == Codec::GBS || c == Codec::VGM; }

// The quality half of the badge: the sample rate in kHz for high-resolution lossless audio (CD and
// DVD rates say nothing a plain "FLAC" doesn't), the bitrate for lossy.
std::wstring Quality(const Track& t) {
    wchar_t buf[32] = {};
    if (GameMusic(t.codec)) return L"";
    if (Lossless(t.codec) && t.sampleRate > 48000) swprintf_s(buf, L"%g", t.sampleRate / 1000.0);
    else if (!Lossless(t.codec) && t.bitrate) swprintf_s(buf, L"%u", (unsigned)t.bitrate);
    return buf;
}

// Width of the text in the given font, leaving the DC's previous font in place.
int TextWidth(HDC dc, HFONT font, const std::wstring& s) {
    HGDIOBJ old = SelectObject(dc, font);
    SIZE ext{};
    GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &ext);
    SelectObject(dc, old);
    return ext.cx;
}

// `a` moved `percent`% of the way toward `b`.
COLORREF Blend(COLORREF a, COLORREF b, int percent) {
    auto mix = [&](int x, int y) { return (BYTE)(x + (y - x) * percent / 100); };
    return RGB(mix(GetRValue(a), GetRValue(b)), mix(GetGValue(a), GetGValue(b)), mix(GetBValue(a), GetBValue(b)));
}

// The spinner's frame: one step every 90 ms, shared by every spinner on screen.
constexpr int kSpinnerDots = 8;
int SpinnerPhase() { return (int)(GetTickCount64() / 90 % kSpinnerDots); }
constexpr int kSpinnerWidth = 16;  // the room it takes before a title, at 96 DPI

// A square-cornered filled tag, no border; returns its width.
int Pill(HDC dc, int x, const RECT& cell, const std::wstring& text, COLORREF fill, COLORREF color) {
    int w = TextWidth(dc, g.font, text) + S(10);
    int h = std::min<int>(cell.bottom - cell.top - S(4), S(16));
    int top = cell.top + (cell.bottom - cell.top - h) / 2;
    RECT tr{x, top, x + w, top + h};
    FillSolid(dc, tr, fill);
    HGDIOBJ of = SelectObject(dc, g.font);
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), (int)text.size(), &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, of);
    return w;
}

}  // namespace

// True when the cell's track earns a badge: the codec column (when known), or the title of a
// long recording or of a busy track.
bool HasBadge(int column, TrackId id) {
    if (column == 7) return g_lib[id].codec != Codec::Unknown;
    return column == 2 && (g_lib[id].Duration() >= kMixSeconds || TrackBusy(id));
}

// A track something in cordinal is using right now: the ReplayGain scanner measuring it, the tag
// reader's current batch, a tag write, or an organize run moving its file. The playing track
// isn't "busy": the list already marks it.
bool TrackBusy(TrackId id) {
    return id != kNone && (id == g.rgCurrent.load() || g.tagReading.count(id) || TagWriteUsing(id) || OrganizeUsing(id) || ConvertUsing(id));
}

bool AnyTrackBusy() {
    return g.rgCurrent.load() != kNone || !g.tagReading.empty() || TagWriteBusy() || OrganizeBusy();
}

// A ring of small square dots centred on (cx, cy), the brightest stepping round each frame and the
// rest fading out behind it. Drawn over `bg`.
void DrawBusySpinner(HDC dc, int cx, int cy, COLORREF color, COLORREF bg) {
    const int phase = SpinnerPhase(), dot = std::max(2, S(3));
    const double radius = S(5);
    for (int i = 0; i < kSpinnerDots; ++i) {
        const int age = (phase - i + kSpinnerDots) % kSpinnerDots;  // 0 = the leading dot
        const double angle = i * 2 * 3.14159265358979 / kSpinnerDots - 3.14159265358979 / 2;
        const int x = cx + (int)std::lround(radius * std::cos(angle)) - dot / 2;
        const int y = cy + (int)std::lround(radius * std::sin(angle)) - dot / 2;
        FillSolid(dc, {x, y, x + dot, y + dot}, Blend(color, bg, age * 11));
    }
}

// kBusyTimer: repaints just the list rows whose spinner turns (and those whose spinner just
// went away), and the library view while it shows one. Stops itself once nothing is busy;
// MainTick starts it again when something is.
void BusyTick() {
    const bool any = AnyTrackBusy();
    std::vector<int> rows;
    if (any && !CanvasActive() && IsWindowVisible(g.list)) {
        const int top = ListView_GetTopIndex(g.list);
        const int end = std::min(ListView_GetItemCount(g.list), top + ListView_GetCountPerPage(g.list) + 1);
        for (int row = std::max(top, 0); row < end; ++row)
            if (TrackBusy(RowTrack(row))) rows.push_back(row);
    }
    auto repaint = [](int row) {
        RECT r{};
        if (ListView_GetItemRect(g.list, row, &r, LVIR_BOUNDS)) InvalidateRect(g.list, &r, FALSE);
    };
    for (int row : rows) repaint(row);
    for (int row : g.busyRows)
        if (std::find(rows.begin(), rows.end(), row) == rows.end()) repaint(row);
    g.busyRows.swap(rows);
    LibViewBusyTick(any);
    if (!any) {
        KillTimer(g.hwnd, kBusyTimer);
        g.busyTimer = false;
    }
}

// The pill's label: the codec name plus its quality where that makes sense.
std::wstring CodecBadgeText(TrackId id) {
    const Track& t = g_lib[id];
    std::wstring q = Quality(t);
    return std::wstring(CodecName(t.codec)) + (q.empty() ? L"" : L" " + q);
}

// Paints a Codec or Title cell that carries a badge (HasBadge), background included.
void DrawBadgeCell(HDC dc, int row, int subitem, TrackId id, COLORREF text, COLORREF bg, HFONT font, bool selected) {
    RECT r{};
    ListView_GetSubItemRect(g.list, row, subitem, LVIR_BOUNDS, &r);
    FillSolid(dc, r, bg);
    SetBkMode(dc, TRANSPARENT);
    RECT cell{r.left + S(6), r.top, r.right - S(4), r.bottom};
    const Track& t = g_lib[id];
    if (ColumnType(subitem) == 7) {
        // Plain text, its shade saying the quality at a glance: lossy recedes, lossless reads in
        // the secondary text color, and high-resolution lossless (above 48 kHz) in the accent.
        std::wstring label = CodecBadgeText(id);
        COLORREF color = selected ? text
                         : !Lossless(t.codec) ? Ref(P().faint)
                         : t.sampleRate > 48000 ? Ref(P().accent) : SubColor();
        if (TextWidth(dc, g.font, label) > cell.right - cell.left) label = CodecName(t.codec);
        HGDIOBJ old = SelectObject(dc, g.font);
        SetTextColor(dc, color);
        DrawTextW(dc, label.c_str(), (int)label.size(), &cell, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        SelectObject(dc, old);
        return;
    }
    // MIX: a square tag in the accent's tint.
    COLORREF tag = selected ? Blend(bg, text, 22) : Blend(bg, Ref(P().accent), 18);
    COLORREF tagText = selected ? text : Blend(Ref(P().accent), TextColor(), 35);
    // The spinner, the title, then "MIX" right after it (or at the cell's end when the title is
    // cut short).
    if (TrackBusy(id)) {
        int save = SaveDC(dc);
        IntersectClipRect(dc, r.left, r.top, r.right, r.bottom);
        DrawBusySpinner(dc, cell.left + S(kSpinnerWidth / 2 - 2), (cell.top + cell.bottom) / 2,
                        selected ? text : Ref(P().accent), bg);
        RestoreDC(dc, save);
        cell.left += S(kSpinnerWidth);
    }
    std::wstring title = FormatTitle(kColumns[2].display, Lookup(id, false));
    const bool mix = t.Duration() >= kMixSeconds;
    int pillW = mix ? TextWidth(dc, g.font, L"MIX") + S(10) : 0;
    RECT tr = cell;
    tr.right = std::max<int>(tr.left, (int)cell.right - pillW - (mix ? S(6) : 0));
    HGDIOBJ old = SelectObject(dc, font);
    SetTextColor(dc, text);
    DrawTextW(dc, title.c_str(), (int)title.size(), &tr,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS | DT_CALCRECT);
    int textRight = std::min<int>(tr.right, (int)cell.right - pillW - (mix ? S(6) : 0));
    tr = {cell.left, cell.top, textRight, cell.bottom};
    DrawTextW(dc, title.c_str(), (int)title.size(), &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, old);
    if (mix && cell.right - cell.left > pillW + S(24)) Pill(dc, textRight + S(6), cell, L"MIX", tag, tagText);
}

}  // namespace app
