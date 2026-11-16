// Coming back to music on Linux, as the Windows build's app_spots.cpp: Pick up where you left off (songs
// left partway, each at its place), Haven't heard in a while (rediscover.h) and Play history, in one panel.
// Playback ▸ Pick up where you left off and Play history open it; so does a right-click on the Resume button.
#include "panels_internal.h"
#include "playlog.h"
#include "rediscover.h"

#include <ctime>

namespace lx {

namespace {
constexpr size_t kMaxSpots = 12, kMaxSnoozed = 64, kMaxHistory = 400;

bool WorthKeeping(TrackId id, double pos) {
    if (!g_lib.Contains(id) || !std::isfinite(pos) || pos < 5) return false;
    double duration = g_lib[id].Duration();
    return duration <= 0 || pos < duration - 5;
}

bool Snoozed(TrackId id) {
    std::wstring path = g_lib.Path(id);
    uint32_t last = g_lib[id].lastPlayed;
    return std::ranges::any_of(g.settings.snoozed, [&](const auto& s) { return s.second == last && s.first == path; });
}

std::vector<Rediscovery> Rediscoveries() {
    return FindRediscoveries(g_lib, UnixNow(), 3, 4, [](TrackId id) {
        return Snoozed(id) || std::ranges::any_of(g.spots, [id](const auto& s) { return s.first == id; });
    });
}
}  // namespace

void RememberSpot(TrackId id, double pos) {
    ForgetSpot(id);
    if (!WorthKeeping(id, pos)) return;
    g.spots.insert(g.spots.begin(), {id, pos});
    if (g.spots.size() > kMaxSpots) g.spots.resize(kMaxSpots);
}

void ForgetSpot(TrackId id) {
    std::erase_if(g.spots, [id](const auto& s) { return s.first == id; });
}

// Something to offer at start, as on Windows: not when all there is is the song just reopened. Windows
// loads that song paused (g.playing); here it waits stopped as the resume track.
bool HasComebacks() {
    return g.settings.resumeOn && (std::ranges::any_of(g.spots, [](const auto& s) {
                                       return s.first != g.playing && s.first != g.resumeTrack;
                                   }) ||
                                   !Rediscoveries().empty());
}

namespace panel {
namespace {

enum class Row { Header, Spot, Rediscover, History };
struct Line {
    Row kind;
    size_t index = 0;
    std::string title, sub;
};
struct ComebackPanel {
    bool history = false;
    std::vector<Line> lines;
    std::vector<Rediscovery> found;
    std::vector<TrackId> played;
    int selected = -1;
    float scroll = 0;
    Rect panel, list;
} g_cb;

std::string Narrow(const std::wstring& s) { return compat::Narrow(s.c_str()); }
float HeightOf(const Line& l) { return l.kind == Row::Header ? S(30) : S(44); }
bool Selectable(int i) { return i >= 0 && i < (int)g_cb.lines.size() && g_cb.lines[i].kind != Row::Header; }

std::string When(uint32_t seconds, const char* format) {
    time_t t = seconds;
    tm local{};
    localtime_r(&t, &local);
    char buf[96];
    strftime(buf, sizeof buf, format, &local);
    return buf;
}

void Build() {
    auto with = [](std::string a, const std::wstring& b) { return b.empty() ? a : a + "  \xC2\xB7  " + Narrow(b); };
    g_cb.lines.clear();
    g_cb.played.clear();
    if (g_cb.history) {
        std::string day;
        const auto& log = PlayLog();
        for (size_t i = log.size(); i-- > 0 && g_cb.played.size() < kMaxHistory;) {
            const PlayEvent& e = log[i];
            if (!(e.flags & PlayEvent::kCounted) || !g_lib.Contains(e.id)) continue;
            if (std::string d = When(e.time, "%A, %B %e, %Y"); d != day) g_cb.lines.push_back({Row::Header, 0, day = d});
            g_cb.lines.push_back({Row::History, g_cb.played.size(), Narrow(TrackTitle(e.id)), with(When(e.time, "%H:%M"), TrackArtist(e.id))});
            g_cb.played.push_back(e.id);
        }
        if (g_cb.played.empty()) g_cb.lines.push_back({Row::Header, 0, "Songs you play to the end show up here."});
        return;
    }
    if (!g.spots.empty()) g_cb.lines.push_back({Row::Header, 0, "Pick up where you left off"});
    for (size_t i = 0; i < g.spots.size(); ++i) {
        auto [id, pos] = g.spots[i];
        g_cb.lines.push_back({Row::Spot, i, Narrow(TrackTitle(id)),
                              with(Narrow(FormatDuration(pos) + L" / " + FormatDuration(g_lib[id].Duration())), TrackArtist(id))});
    }
    if (!g_cb.found.empty()) g_cb.lines.push_back({Row::Header, 0, "Haven't heard in a while"});
    for (size_t i = 0; i < g_cb.found.size(); ++i) {
        const Rediscovery& r = g_cb.found[i];
        TrackId id = r.tracks.front();
        int days = (int)((UnixNow() - r.lastPlayed) / 86400);
        std::string ago = days >= 60 ? std::to_string(days / 30) + " months ago" : std::to_string(days) + " days ago";
        std::string plays = std::to_string((int)std::lround(r.plays)) + (r.album ? " plays a track" : " plays");
        const Track& t = g_lib[id];
        std::wstring who = r.album && t.albumArtist ? g_lib.Str(t.albumArtist) : TrackArtist(id);
        std::string title = r.album ? "Album: " + Narrow(g_lib.Str(t.album)) : Narrow(TrackTitle(id));
        std::string sub = who.empty() ? "last played " + ago + ", " + plays : Narrow(who) + "  \xC2\xB7  last played " + ago + ", " + plays;
        g_cb.lines.push_back({Row::Rediscover, i, title, sub});
    }
}

int LineAt(float x, float y, bool* overX) {
    if (!g_cb.list.In(x, y)) return -1;
    float top = g_cb.list.y - g_cb.scroll;
    for (int i = 0; i < (int)g_cb.lines.size(); ++i) {
        float h = HeightOf(g_cb.lines[i]);
        if (y >= top && y < top + h) {
            if (!Selectable(i)) return -1;
            if (overX) *overX = g_cb.lines[i].kind != Row::History && x >= g_cb.list.R() - S(36);
            return i;
        }
        top += h;
    }
    return -1;
}

float MaxScroll() {
    float h = 0;
    for (const Line& l : g_cb.lines) h += HeightOf(l);
    return std::max(0.0f, h - g_cb.list.h);
}

void Refresh() {
    Build();
    if (g_cb.lines.empty()) return ClosePanel();
    while (g_cb.selected >= 0 && !Selectable(g_cb.selected)) --g_cb.selected;
    g_cb.scroll = std::clamp(g_cb.scroll, 0.0f, MaxScroll());
    g.redraw = true;
}

void Remove(int i) {
    if (!Selectable(i)) return;
    const Line& l = g_cb.lines[i];
    if (l.kind == Row::Spot) {
        g.spots.erase(g.spots.begin() + l.index);
    } else if (l.kind == Row::Rediscover) {
        const Rediscovery& r = g_cb.found[l.index];
        TrackId newest = r.tracks.front();
        for (TrackId id : r.tracks)
            if (g_lib[id].lastPlayed > g_lib[newest].lastPlayed) newest = id;
        auto& snoozed = g.settings.snoozed;
        snoozed.emplace_back(g_lib.Path(newest), g_lib[newest].lastPlayed);
        if (snoozed.size() > kMaxSnoozed) snoozed.erase(snoozed.begin());
        g_cb.found.erase(g_cb.found.begin() + l.index);
        SaveSettings();
    } else {
        return;
    }
    Refresh();
}

void Choose(int i) {
    if (!Selectable(i)) return;
    Line l = g_cb.lines[i];
    std::vector<TrackId> album = l.kind == Row::Rediscover ? g_cb.found[l.index].tracks : std::vector<TrackId>{};
    TrackId played = l.kind == Row::History ? g_cb.played[l.index] : kNone;
    ClosePanel();
    if (l.kind == Row::Spot) PlaySpot(l.index);
    else if (!album.empty()) PlayTracks(album, 0);
    else if (g_lib.Contains(played)) PlayTracks({played}, 0);
}

void Draw(const Palette& p) {
    Rect panel = g_cb.panel = DrawFrame(p, S(520), S(560), g_cb.history ? "Play history" : "Coming back");
    g_cb.list = {panel.x + S(8), panel.y + S(48), panel.w - S(16), panel.h - S(56)};
    SDL_Rect clip{(int)g_cb.list.x, (int)g_cb.list.y, (int)g_cb.list.w, (int)g_cb.list.h};
    SDL_SetRenderClipRect(g.renderer, &clip);
    float y = g_cb.list.y - g_cb.scroll;
    for (int i = 0; i < (int)g_cb.lines.size(); ++i) {
        const Line& l = g_cb.lines[i];
        Rect r{g_cb.list.x, y, g_cb.list.w, HeightOf(l)};
        y += r.h;
        if (r.B() < g_cb.list.y || r.y > g_cb.list.B()) continue;
        if (l.kind == Row::Header) {
            DrawText(Font::Bold, l.title, r.x + S(8), r.y + S(10), p.sub, r.w - S(16));
            continue;
        }
        const bool hot = r.In(g_mx, g_my), lit = hot || i == g_cb.selected;
        if (lit) Fill(r, i == g_cb.selected ? p.select : p.hover);
        const uint32_t main = i == g_cb.selected ? p.selectText : p.text, sub = i == g_cb.selected ? p.selectText : p.sub;
        const bool x = l.kind != Row::History;
        const float textW = r.w - S(16) - (x ? S(36) : 0);
        DrawText(Font::Normal, l.title, r.x + S(8), r.y + S(4), main, textW);
        DrawText(Font::Small, l.sub, r.x + S(8), r.y + S(23), sub, textW);
        if (!x) continue;
        Rect box{r.R() - S(36), r.y, S(36), r.h};
        const bool overX = hot && box.In(g_mx, g_my);
        if (overX) Fill({box.x + S(6), box.y + S(8), box.w - S(12), box.h - S(16)}, p.surface2);
        DrawText(Font::Normal, "\xE2\x9C\x95", box.x, TextY(box), overX ? p.text : sub, box.w, 1);
    }
    SDL_SetRenderClipRect(g.renderer, nullptr);
}

void MoveTo(int step) {
    int i = g_cb.selected;
    do i += step;
    while (i >= 0 && i < (int)g_cb.lines.size() && !Selectable(i));
    if (!Selectable(i)) return;
    g_cb.selected = i;
    float top = 0;
    for (int k = 0; k < i; ++k) top += HeightOf(g_cb.lines[k]);
    if (top - S(30) < g_cb.scroll) g_cb.scroll = std::max(0.0f, top - S(30));
    else if (top + HeightOf(g_cb.lines[i]) > g_cb.scroll + g_cb.list.h) g_cb.scroll = top + HeightOf(g_cb.lines[i]) - g_cb.list.h;
    g_cb.scroll = std::clamp(g_cb.scroll, 0.0f, MaxScroll());
    g.redraw = true;
}

void Key(const SDL_KeyboardEvent& e) {
    switch (e.key) {
        case SDLK_ESCAPE: ClosePanel(); break;
        case SDLK_UP: MoveTo(-1); break;
        case SDLK_DOWN: MoveTo(1); break;
        case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: Choose(g_cb.selected); break;
        case SDLK_DELETE: case SDLK_BACKSPACE: Remove(g_cb.selected); break;
        default: break;
    }
}

}  // namespace

const Ops kComebackOps{
    .draw = Draw,
    .mouseDown = [](float x, float y, int) {
        if (!g_cb.panel.In(x, y)) return ClosePanel();
        bool overX = false;
        int i = LineAt(x, y, &overX);
        if (overX) Remove(i);
        else Choose(i);
    },
    .wheel = [](float, float, float dy) {
        g_cb.scroll = std::clamp(g_cb.scroll - dy * S(44) * 2, 0.0f, MaxScroll());
        g.redraw = true;
    },
    .key = Key,
};

}  // namespace panel

void OpenComebacks(bool history) {
    using namespace panel;
    if (!history && !g.settings.resumeOn) return;
    if (PanelOpen()) ClosePanel();
    g_cb = ComebackPanel{};
    g_cb.history = history;
    if (!history) g_cb.found = Rediscoveries();
    Build();
    if (g_cb.lines.empty()) return;
    g_kind = Kind::Comeback;
    g.redraw = true;
}

}  // namespace lx
