// Lyrics panel: beside the track list, the playing song's lyrics. They come from the file's tags
// (what the tag editor writes) or a .lrc file next to it. Timed lyrics ("[01:12.30] line", LRC)
// follow the song: the current line is highlighted and kept in the middle, and clicking a line
// seeks to it. Plain lyrics scroll with the wheel.
#include "app.h"

namespace app {

namespace {

const wchar_t kLyricsClass[] = L"CordinalLyrics";
constexpr UINT WM_APP_LYRICS = WM_APP + 1;  // lParam = Loaded* (owned by the receiver)

struct Line {
    double time = -1;  // seconds; -1 for plain lyrics
    std::wstring text;
    int top = 0, height = 0;  // laid out at `layoutWidth`
};

struct Loaded {
    TrackId id;
    uint64_t generation;
    std::vector<Line> lines;
    bool synced = false;
};

struct Panel {
    HWND hwnd = nullptr;
    bool shown = false;
    TrackId loadedFor = kNone, requested = kNone;
    bool loading = false, stopping = false;
    uint64_t generation = 0;
    std::thread worker;
    std::vector<Line> lines;
    bool synced = false;
    int layoutWidth = -1, contentHeight = 0;
    double scroll = 0, target = 0;  // pixels; synced lyrics glide toward `target`
    int current = -1;
    bool userScrolled = false;  // plain lyrics: the wheel moved them
} p;

double ParseStamp(const std::wstring& s) {
    // mm:ss(.xx) or mm:ss:xx
    int mm = 0;
    double ss = 0;
    if (swscanf_s(s.c_str(), L"%d:%lf", &mm, &ss) == 2 && mm >= 0 && ss >= 0 && ss < 60) {
        size_t colon2 = s.find(L':', s.find(L':') + 1);
        if (colon2 != std::wstring::npos) {  // mm:ss:xx (hundredths)
            int sec = 0, hund = 0;
            if (swscanf_s(s.c_str(), L"%d:%d:%d", &mm, &sec, &hund) == 3) return mm * 60.0 + sec + hund / 100.0;
        }
        return mm * 60.0 + ss;
    }
    return -1;
}

}  // namespace

// LRC: a line may carry several stamps; [ar:], [ti:]... are metadata; [offset:+500] shifts all.
std::vector<std::pair<double, std::wstring>> ParseLyrics(const std::wstring& text, bool& synced) {
    std::vector<std::pair<double, std::wstring>> timed, plain;
    double offset = 0;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(L'\n', start);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        std::vector<double> stamps;
        size_t pos = 0;
        while (pos < line.size() && line[pos] == L'[') {
            size_t close = line.find(L']', pos);
            if (close == std::wstring::npos) break;
            std::wstring tag = line.substr(pos + 1, close - pos - 1);
            double t = ParseStamp(tag);
            if (t >= 0) stamps.push_back(t);
            else if (_wcsnicmp(tag.c_str(), L"offset:", 7) == 0) offset = _wtoi(tag.c_str() + 7) / 1000.0;
            else if (tag.find(L':') == std::wstring::npos) break;  // not a tag: part of the text
            pos = close + 1;
        }
        std::wstring rest = line.substr(pos);
        while (!rest.empty() && (rest.front() == L' ' || rest.front() == L'\t')) rest.erase(rest.begin());
        if (!stamps.empty())
            for (double t : stamps) timed.emplace_back(t, rest);
        else if (pos == 0)
            plain.emplace_back(-1, line);
        if (end == text.size()) break;
    }
    synced = timed.size() >= 3;
    if (!synced) {
        while (!plain.empty() && plain.back().second.empty()) plain.pop_back();
        return plain;
    }
    for (auto& l : timed) l.first = std::max(0.0, l.first - offset);
    SortObjects(timed.begin(), timed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    return timed;
}

namespace {

std::wstring ReadTextFile(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return L"";
    LARGE_INTEGER size{};
    std::string bytes;
    if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (1 << 20)) {
        bytes.resize((size_t)size.QuadPart);
        DWORD got = 0;
        if (!ReadFile(f, bytes.data(), (DWORD)bytes.size(), &got, nullptr)) got = 0;
        bytes.resize(got);
    }
    CloseHandle(f);
    if (bytes.size() >= 2 && (uint8_t)bytes[0] == 0xFF && (uint8_t)bytes[1] == 0xFE)  // UTF-16 LE
        return std::wstring((const wchar_t*)(bytes.data() + 2), (bytes.size() - 2) / 2);
    if (bytes.size() >= 3 && bytes.compare(0, 3, "\xEF\xBB\xBF") == 0) bytes.erase(0, 3);
    return FromUtf8(bytes);
}

// Runs on a worker: a .lrc next to the file wins (it's usually timed), then the tags.
void Load(TrackId id, std::wstring path, HWND notify, uint64_t generation) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto* out = new Loaded{id, generation, {}, false};
    std::wstring audio = AudioFileOf(path);
    size_t dot = audio.find_last_of(L'.'), slash = audio.find_last_of(L"\\/");
    std::wstring text;
    if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash)) text = ReadTextFile(audio.substr(0, dot) + L".lrc");
    if (text.empty()) {
        std::string lyrics;
        if (ReadLyrics(path, lyrics)) text = FromUtf8(lyrics);
    }
    for (auto& [time, line] : ParseLyrics(text, out->synced)) out->lines.push_back({time, std::move(line)});
    CoUninitialize();
    if (!PostMessageW(notify, WM_APP_LYRICS, 0, (LPARAM)out)) delete out;
}

// Only one read may be in flight. While it runs, requested/generation keep the newest track;
// the UI starts that read when it takes the previous result.
void StartLoad() {
    if (p.loading || p.stopping || !p.shown || p.requested == kNone || p.requested >= g_lib.Count()) return;
    p.loading = true;
    p.worker = std::thread(Load, p.requested, g_lib.Path(p.requested), p.hwnd, p.generation);
}

// Measures each line once per width (using the bold font, so highlighting a line never changes its
// wrap) and caches the resulting heights on the Line; later paints reuse them until the width
// changes.
void Layout(HDC dc, int width) {
    if (width == p.layoutWidth) return;
    p.layoutWidth = width;
    HGDIOBJ old = SelectObject(dc, g.fontBold);  // measured bold so the highlighted line never rewraps
    int y = 0;
    for (auto& l : p.lines) {
        RECT r{0, 0, width, 0};
        std::wstring s = l.text.empty() ? L" " : l.text;
        DrawTextW(dc, s.c_str(), (int)s.size(), &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | (p.synced ? DT_CENTER : DT_LEFT));
        l.top = y;
        l.height = r.bottom - r.top + (p.synced ? S(8) : S(2));
        y += l.height;
    }
    SelectObject(dc, old);
    p.contentHeight = y;
}

int HeaderHeight() { return S(48); }

int BodyHeight();
void ClampScroll();

// One step of the scroll toward its target; false once it's there.
bool Glide() {
    ClampScroll();
    double step = (p.target - p.scroll) * 0.25;
    bool moving = std::abs(step) >= 0.5;
    p.scroll = moving ? p.scroll + step : p.target;
    InvalidateRect(p.hwnd, nullptr, FALSE);
    return moving;
}

void PaintContent(HDC mem, RECT rc) {
    FillSolid(mem, rc, WindowBg());
    FillSolid(mem, {0, 0, 1, rc.bottom}, Ref(P().border));
    SetBkMode(mem, TRANSPARENT);
    int pad = S(16), width = rc.right - 2 * pad;

    // Header: "Lyrics", and the song.
    RECT head{pad, S(8), rc.right - pad, S(26)};
    HGDIOBJ old = SelectObject(mem, g.fontBold);
    SetTextColor(mem, TextColor());
    DrawTextW(mem, L"Lyrics", -1, &head, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    std::wstring song = p.loadedFor != kNone && p.loadedFor < g_lib.Count()
                            ? FormatTitle(L"%title%[ \x00B7 %artist%]", Lookup(p.loadedFor, false))
                            : L"";
    RECT songRect{pad, S(26), rc.right - pad, S(44)};
    SelectObject(mem, g.font);
    SetTextColor(mem, SubColor());
    DrawTextW(mem, song.c_str(), -1, &songRect, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    FillSolid(mem, {pad, HeaderHeight() - 1, rc.right - pad, HeaderHeight()}, Ref(P().border));

    RECT body{0, HeaderHeight(), rc.right, rc.bottom};
    IntersectClipRect(mem, body.left, body.top, body.right, body.bottom);
    if (p.lines.empty()) {
        const wchar_t* hint = g.playing == kNone ? L"Lyrics of the playing song show here."
                              : p.loading        ? L""
                                                 : L"No lyrics for this song.\n\nAdd them in the tag editor (Alt+Enter), or put a "
                                                   L".lrc file with the same name next to it for lyrics that follow the song.";
        RECT hr{pad, body.top + S(24), rc.right - pad, rc.bottom};
        SetTextColor(mem, SubColor());
        DrawTextW(mem, hint, -1, &hr, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
    } else {
        Layout(mem, width);
        int y0 = body.top + S(12) - (int)p.scroll;
        for (int i = 0; i < (int)p.lines.size(); ++i) {
            const Line& l = p.lines[i];
            int top = y0 + l.top;
            if (top + l.height < body.top || top > body.bottom) continue;
            RECT r{pad, top, pad + width, top + l.height};
            bool cur = p.synced && i == p.current;
            SelectObject(mem, cur ? g.fontBold : g.font);
            SetTextColor(mem, !p.synced ? TextColor() : cur ? Ref(P().accent) : i < p.current ? SubColor() : TextColor());
            DrawTextW(mem, l.text.c_str(), (int)l.text.size(), &r, DT_WORDBREAK | DT_NOPREFIX | (p.synced ? DT_CENTER : DT_LEFT));
        }
    }
    SelectObject(mem, old);
}

int BodyHeight() {
    RECT rc;
    GetClientRect(p.hwnd, &rc);
    return std::max(0, (int)rc.bottom - HeaderHeight());
}

void ClampScroll() {
    double maxScroll = std::max(0, p.contentHeight + S(24) - BodyHeight());
    p.scroll = std::clamp(p.scroll, 0.0, maxScroll);
    p.target = std::clamp(p.target, 0.0, maxScroll);
}

LRESULT CALLBACK LyricsProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT:
            DoubleBufferedPaint(h, PaintContent);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            p.layoutWidth = -1;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_MOUSEWHEEL:
            p.scroll -= GET_WHEEL_DELTA_WPARAM(wp) / (double)WHEEL_DELTA * S(48);
            p.target = p.scroll;
            p.userScrolled = true;
            ClampScroll();
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP: {
            // A timed line seeks there.
            if (!p.synced || g.playing != p.loadedFor || p.loadedFor == kNone) return 0;
            int y = GET_Y_LPARAM(lp) - HeaderHeight() - S(12) + (int)p.scroll;
            for (const Line& l : p.lines)
                if (y >= l.top && y < l.top + l.height) {
                    g.engine.Seek(l.time);
                    p.userScrolled = false;
                    break;
                }
            return 0;
        }
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, p.synced && g.playing == p.loadedFor ? IDC_HAND : IDC_ARROW));
            return TRUE;
        case WM_TIMER:
            if (!Glide()) KillTimer(h, 1);
            return 0;
        case WM_APP_LYRICS: {
            std::unique_ptr<Loaded> r(reinterpret_cast<Loaded*>(lp));
            if (p.worker.joinable()) p.worker.join();
            p.loading = false;
            if (p.stopping || !p.shown || r->generation != p.generation || r->id != p.requested) {
                StartLoad();  // the newest request replaces all the songs skipped in between
                return 0;
            }
            p.loadedFor = r->id;
            p.lines = std::move(r->lines);
            p.synced = r->synced;
            p.layoutWidth = -1;
            p.scroll = p.target = 0;
            p.current = -1;
            p.userScrolled = false;
            LyricsTick();
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

void CreateLyricsPanel(HWND parent) {
    RegisterAppClass(kLyricsClass, LyricsProc, 0, false);
    p.stopping = false;
    p.requested = p.loadedFor = kNone;
    p.shown = IniInt(L"Window", L"Lyrics", 0) != 0;
    p.hwnd = CreateWindowExW(0, kLyricsClass, L"Lyrics", WS_CHILD | (p.shown ? WS_VISIBLE : 0), 0, 0, 0, 0, parent,
                             (HMENU)(INT_PTR)IDC_LYRICS, g.inst, nullptr);
}

bool LyricsPanelShown() { return p.hwnd && p.shown; }
int LyricsPanelWidth() { return LyricsPanelShown() ? S(300) : 0; }

void ToggleLyricsPanel() {
    p.shown = !p.shown;
    if (!p.shown) {
        ++p.generation;
        p.requested = p.loadedFor = kNone;
        p.lines.clear();
    }
    IniWrite(L"Window", L"Lyrics", p.shown ? L"1" : L"0");
    ShowWindow(p.hwnd, p.shown ? SW_SHOWNA : SW_HIDE);
    Layout();
    LyricsTrackChanged();
}

// Library compaction renumbers tracks: the panel's loaded id and lines belong to the old order.
void LyricsLibraryChanged() {
    if (!p.hwnd) return;
    ++p.generation;
    p.requested = p.loadedFor = kNone;
    p.lines.clear();
    p.synced = false;
    p.current = -1;
    p.layoutWidth = -1;
}

void LyricsTrackChanged() {
    if (!LyricsPanelShown() || g.playing == p.requested) return;
    p.requested = g.playing;
    ++p.generation;
    p.lines.clear();
    p.loadedFor = g.playing;
    p.current = -1;
    p.scroll = p.target = 0;
    InvalidateRect(p.hwnd, nullptr, FALSE);
    if (g.playing == kNone) {
        return;
    }
    StartLoad();
}

void StopLyrics() {
    p.stopping = true;
    if (p.worker.joinable()) p.worker.join();
    MSG message;
    while (p.hwnd && PeekMessageW(&message, p.hwnd, WM_APP_LYRICS, WM_APP_LYRICS, PM_REMOVE))
        delete reinterpret_cast<Loaded*>(message.lParam);
    p.loading = false;
}

bool LyricsReadyFor(TrackId id, const std::wstring& line) {
    return p.requested == id && p.loadedFor == id && !p.loading &&
           std::any_of(p.lines.begin(), p.lines.end(), [&](const Line& item) { return item.text == line; });
}

// With the UI timer: the current timed line, and the glide toward it.
void LyricsTick() {
    if (!LyricsPanelShown()) return;
    LyricsTrackChanged();
    if (!p.synced || p.lines.empty() || p.loadedFor != g.playing || p.layoutWidth < 0) {
        if (p.layoutWidth < 0 && !p.lines.empty()) InvalidateRect(p.hwnd, nullptr, FALSE);
        return;
    }
    double pos = g.engine.Position() + 0.15;  // a hair early reads as on time
    int cur = -1;
    while (cur + 1 < (int)p.lines.size() && p.lines[cur + 1].time <= pos) ++cur;
    if (cur != p.current) {
        p.current = cur;
        p.userScrolled = false;
    }
    if (!p.userScrolled && cur >= 0) {
        const Line& l = p.lines[cur];
        p.target = l.top + l.height / 2.0 + S(12) - BodyHeight() / 2.0;
    }
    if (Glide()) SetTimer(p.hwnd, 1, 16, nullptr);
}

void LyricsTheme() {
    if (p.hwnd) InvalidateRect(p.hwnd, nullptr, FALSE);
}

}  // namespace app
