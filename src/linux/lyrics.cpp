// The lyrics panel on Linux (View > Lyrics panel, Ctrl+L), as the Windows build's app_lyrics.cpp: beside the
// track list, the playing song's lyrics. They come from a .lrc file next to it or from the file's tags (what the
// tag editor writes). Timed lyrics ("[01:12.30] line") follow the song: the current line is highlighted and kept
// in the middle, and clicking a line seeks to it. Plain lyrics scroll with the wheel.
#include "cue.h"
#include "draw.h"
#include "tags.h"

#include <thread>

namespace lx {
namespace {

struct Line {
    double time = -1;  // seconds; -1 for plain lyrics
    std::string text;
    float top = 0, height = 0;  // laid out at `layoutWidth`
};

struct Loaded {
    TrackId id;
    uint64_t generation;
    std::vector<Line> lines;
    bool synced = false;
};

struct Panel {
    TrackId loadedFor = kNone, requested = kNone;
    bool loading = false, synced = false, userScrolled = false, stopping = false;
    uint64_t generation = 0;
    std::thread worker;
    std::vector<Line> lines;
    float layoutWidth = -1, contentHeight = 0;
    float scroll = 0, target = 0;  // pixels; synced lyrics glide toward `target`
    int current = -1;
    Rect body;
} p;

// mm:ss(.xx) or mm:ss:xx; -1 when it is not a time.
double ParseStamp(const std::wstring& s) {
    int mm = 0;
    double ss = 0;
    if (swscanf(s.c_str(), L"%d:%lf", &mm, &ss) == 2 && mm >= 0 && ss >= 0 && ss < 60) {
        size_t colon2 = s.find(L':', s.find(L':') + 1);
        if (colon2 != std::wstring::npos) {  // mm:ss:xx (hundredths)
            int sec = 0, hund = 0;
            if (swscanf(s.c_str(), L"%d:%d:%d", &mm, &sec, &hund) == 3) return mm * 60.0 + sec + hund / 100.0;
        }
        return mm * 60.0 + ss;
    }
    return -1;
}

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
            else if (tag.size() > 7 && !wcsncasecmp(tag.c_str(), L"offset:", 7)) offset = _wtoi(tag.c_str() + 7) / 1000.0;
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
    std::stable_sort(timed.begin(), timed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    return timed;
}

std::string ReadTextFile(const std::wstring& path) {
    std::string bytes;
    if (FILE* f = fopen(compat::Narrow(path.c_str()).c_str(), "rb")) {
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && bytes.size() < (1 << 20)) bytes.append(buf, n);
        fclose(f);
    }
    if (bytes.compare(0, 3, "\xEF\xBB\xBF") == 0) bytes.erase(0, 3);
    return bytes;
}

// Runs on a worker: a .lrc next to the file wins (it's usually timed), then the tags.
void Load(TrackId id, std::wstring path, uint64_t generation) {
    auto* out = new Loaded{id, generation, {}, false};
    std::wstring audio = AudioFileOf(path);
    size_t dot = audio.find_last_of(L'.'), slash = audio.find_last_of(L'/');
    std::string text;
    if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash)) text = ReadTextFile(audio.substr(0, dot) + L".lrc");
    if (text.empty()) ReadLyrics(path, text);
    for (auto& [time, line] : ParseLyrics(FromUtf8(text), out->synced)) out->lines.push_back({time, ToUtf8(line)});
    if (!PostMessageW(nullptr, WM_APP_LYRICS, 0, (LPARAM)out)) delete out;
}

void StartLoad() {
    if (p.loading || p.stopping || !LyricsShown() || p.requested == kNone || !g_lib.Contains(p.requested)) return;
    p.loading = true;
    p.worker = std::thread(Load, p.requested, g_lib.Path(p.requested), p.generation);
}

float HeaderHeight() { return S(48); }
float BodyHeight() { return std::max(0.0f, p.body.h); }

// Measures each line once per width (in the bold font, so highlighting a line never changes its wrap).
std::vector<std::string> Wrapped(const std::string& text, float width) {
    std::vector<std::string> lines;
    std::string line;
    size_t i = 0;
    while (i < text.size()) {
        size_t end = text.find(' ', i);
        std::string word = text.substr(i, end == std::string::npos ? std::string::npos : end - i);
        std::string next = line.empty() ? word : line + " " + word;
        if (!line.empty() && TextWidth(Font::Bold, next) > width) {
            lines.push_back(line);
            line = word;
        } else line = next;
        i = end == std::string::npos ? text.size() : end + 1;
    }
    lines.push_back(line);
    return lines;
}

void Layout(float width) {
    if (width == p.layoutWidth) return;
    p.layoutWidth = width;
    float y = 0;
    for (Line& l : p.lines) {
        l.top = y;
        l.height = Wrapped(l.text, width).size() * LineHeight(Font::Bold) + (p.synced ? S(8) : S(2));
        y += l.height;
    }
    p.contentHeight = y;
}

void ClampScroll() {
    float maxScroll = std::max(0.0f, p.contentHeight + S(24) - BodyHeight());
    p.scroll = std::clamp(p.scroll, 0.0f, maxScroll);
    p.target = std::clamp(p.target, 0.0f, maxScroll);
}

}  // namespace

bool LyricsShown() { return g.settings.lyricsPanel; }
float LyricsWidth() { return LyricsShown() ? S(300) : 0; }

void ToggleLyrics() {
    g.settings.lyricsPanel = !g.settings.lyricsPanel;
    if (!g.settings.lyricsPanel) {
        ++p.generation;
        p.requested = p.loadedFor = kNone;
        p.lines.clear();
    }
    SaveSettings();
    g.redraw = true;
}

void OnLyricsLoaded(LPARAM lp) {
    std::unique_ptr<Loaded> r(reinterpret_cast<Loaded*>(lp));
    if (p.worker.joinable()) p.worker.join();
    p.loading = false;
    if (p.stopping || !LyricsShown() || r->generation != p.generation || r->id != p.requested) {
        StartLoad();
        return;
    }
    p.loadedFor = r->id;
    p.lines = std::move(r->lines);
    p.synced = r->synced;
    p.layoutWidth = -1;
    p.scroll = p.target = 0;
    p.current = -1;
    p.userScrolled = false;
    g.redraw = true;
}

// Each frame: a new song loads its lyrics; timed lyrics find their current line and glide to it.
void LyricsTick() {
    if (!LyricsShown()) return;
    if (g.playing != p.requested) {
        p.requested = g.playing;
        ++p.generation;
        p.lines.clear();
        p.loadedFor = g.playing;
        p.current = -1;
        p.scroll = p.target = 0;
        StartLoad();
    }
    if (!p.synced || p.lines.empty() || p.loadedFor != g.playing || p.layoutWidth < 0) return;
    double pos = g.engine.Position() + 0.15;  // a hair early reads as on time
    int cur = -1;
    while (cur + 1 < (int)p.lines.size() && p.lines[cur + 1].time <= pos) ++cur;
    if (cur != p.current) {
        p.current = cur;
        p.userScrolled = false;
    }
    if (!p.userScrolled && cur >= 0) {
        const Line& l = p.lines[cur];
        p.target = l.top + l.height / 2 + S(12) - BodyHeight() / 2;
    }
    ClampScroll();
    float step = (p.target - p.scroll) * 0.25f;
    if (std::fabs(step) >= 0.5f) p.scroll += step;
    else p.scroll = p.target;
}

void StopLyrics() {
    p.stopping = true;
    if (p.worker.joinable()) p.worker.join();
    p.loading = false;
}

bool LyricsAnimating() { return LyricsShown() && g.playing != kNone && p.synced && !g.engine.Paused(); }

void DrawLyrics(const Rect& area, const Palette& p2) {
    p.body = {area.x, area.y + HeaderHeight(), area.w, area.h - HeaderHeight()};
    Fill(area, p2.bg);
    Fill({area.x, area.y, 1, area.h}, p2.border);
    const float pad = S(16), width = area.w - 2 * pad;
    DrawText(Font::Bold, "Lyrics", area.x + pad, area.y + S(8), p2.text, width);
    std::wstring song = p.loadedFor != kNone && g_lib.Contains(p.loadedFor) ? FormatForTrack(L"%title%[ \x00B7 %artist%]", p.loadedFor, false) : L"";
    DrawText(Font::Normal, Utf8(song), area.x + pad, area.y + S(26), p2.sub, width);
    Fill({area.x + pad, area.y + HeaderHeight() - 1, width, 1}, p2.border);
    SDL_Rect clip{(int)p.body.x, (int)p.body.y, (int)p.body.w, (int)p.body.h};
    SDL_SetRenderClipRect(g.renderer, &clip);
    if (p.lines.empty()) {
        const char* hint = g.playing == kNone ? "Lyrics of the playing song show here."
                           : p.loading        ? ""
                                              : "No lyrics for this song.\n\nAdd them in the tag editor (Alt+Enter), or put a .lrc file "
                                                "with the same name next to it for lyrics that follow the song.";
        float y = p.body.y + S(24);
        std::string text = hint;
        size_t start = 0;
        for (;;) {  // paragraphs, each wrapped to the panel
            size_t end = text.find('\n', start);
            std::string paragraph = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
            for (const std::string& line : Wrapped(paragraph, width)) {
                DrawText(Font::Bold, line, area.x + pad, y, p2.sub, width, 1);
                y += LineHeight(Font::Bold);
            }
            if (paragraph.empty()) y += LineHeight(Font::Bold);
            if (end == std::string::npos) break;
            start = end + 1;
        }
    } else {
        Layout(width);
        ClampScroll();
        float y0 = p.body.y + S(12) - p.scroll;
        for (int i = 0; i < (int)p.lines.size(); ++i) {
            const Line& l = p.lines[i];
            float top = y0 + l.top;
            if (top + l.height < p.body.y || top > p.body.B()) continue;
            bool cur = p.synced && i == p.current;
            uint32_t color = !p.synced ? p2.text : cur ? p2.accent : i < p.current ? p2.sub : p2.text;
            float y = top;
            for (const std::string& line : Wrapped(l.text.empty() ? " " : l.text, width)) {
                DrawText(cur ? Font::Bold : Font::Normal, line, area.x + pad, y, color, width, p.synced ? 1 : 0);
                y += LineHeight(Font::Bold);
            }
        }
    }
    SDL_SetRenderClipRect(g.renderer, nullptr);
}

void LyricsWheel(float dy) {
    p.scroll -= dy * S(48);
    p.target = p.scroll;
    p.userScrolled = true;
    ClampScroll();
    g.redraw = true;
}

// A timed line seeks there.
bool LyricsMouseDown(float x, float y) {
    if (!p.body.In(x, y) || !p.synced || g.playing != p.loadedFor || p.loadedFor == kNone) return false;
    float at = y - p.body.y - S(12) + p.scroll;
    for (const Line& l : p.lines)
        if (at >= l.top && at < l.top + l.height) {
            SeekTo(l.time);
            p.userScrolled = false;
            return true;
        }
    return false;
}

}  // namespace lx
