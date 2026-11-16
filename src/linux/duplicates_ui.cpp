// Library > Find duplicates on Linux, as app_duplicates.cpp on Windows: the copies of each song,
// grouped (duplicates.h), with the ones to remove checked. Identical audio is checked for you, keeping
// the best copy; the same song in other files is only suggested. Removing moves the checked files to
// the desktop trash on a worker, then their plays, ratings and playlist places go to the copy kept;
// Undo clean-up brings them back with their history. The worker's result is applied when its
// WM_APP_DUPLICATES arrives, or by StopDuplicates at shutdown, whichever comes first, so files that
// moved always take their history with them, even when the panel was closed meanwhile.
#include "panels_internal.h"

#include "convert.h"
#include "duplicates.h"

#include <memory>
#include <thread>

namespace lx::panel {
namespace {

// A clean-up or its undo. The worker fills `done` (which files moved); the UI thread reads it only
// after joining the worker.
struct Work {
    bool undo = false;
    std::vector<duplicates::Move> moves;  // undo: g_cleanup.merged's, in its order
    std::vector<bool> done;
};

struct Line {
    size_t group;
    int copy;  // -1: the group's heading
};

struct DuplicatesPanel {
    std::vector<duplicates::Group> groups;
    std::vector<std::vector<bool>> ticks;
    std::vector<Line> lines;
    size_t unmeasured = 0;
    bool measuring = false;
    int first = 0;
    Rect list, measure, undo, remove, close;
} g_dup;

duplicates::Cleanup g_cleanup;  // the last clean-up, for Undo clean-up (this session only)
std::unique_ptr<Work> g_work;
std::thread g_worker;

constexpr float kRow = 24;

std::string Count(size_t n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); }

void Populate() {
    auto& d = g_dup;
    d.groups = duplicates::Find(g_lib);
    d.unmeasured = duplicates::Unmeasured(g_lib).size();
    d.ticks.clear();
    d.lines.clear();
    for (size_t gi = 0; gi < d.groups.size(); ++gi) {
        const auto& group = d.groups[gi];
        d.lines.push_back({gi, -1});
        // Identical copies are safe to remove; another encode of the song is the listener's call.
        d.ticks.emplace_back(group.tracks.size(), group.identical);
        d.ticks.back()[0] = false;
        for (size_t ci = 0; ci < group.tracks.size(); ++ci) d.lines.push_back({gi, (int)ci});
    }
    d.first = 0;
    g.redraw = true;
}

void ApplyWork(Work& work) {
    const size_t done = (size_t)std::count(work.done.begin(), work.done.end(), true);
    std::wstring text;
    if (!work.undo) {
        g_cleanup = duplicates::Merge(g_lib, g.playlists, work.moves, work.done);
        text = std::to_wstring(done) + (done == 1 ? L" copy" : L" copies") + L" moved to the trash.";
    } else {
        size_t skipped = 0;
        QueueTags(duplicates::Undo(g_lib, g.playlists, g_cleanup, work.done, &skipped));
        g_cleanup = {};
        text = std::to_wstring(done) + (done == 1 ? L" copy" : L" copies") + L" restored.";
        if (skipped) text += L" " + std::to_wstring(skipped) + (skipped == 1 ? L" playlist" : L" playlists") +
                             L" changed since.";
    }
    if (done < work.done.size())
        text += L" " + std::to_wstring(work.done.size() - done) + (work.done.size() - done == 1 ? L" file" : L" files") +
                (work.undo ? L" stayed in the trash." : L" couldn't be moved.");
    g.libraryDirty = true;
    g.redraw = true;
    UpdateInboxTab();
    ShowMessage(text);
}

void Finish() {
    if (!g_worker.joinable()) return;
    g_worker.join();
    std::unique_ptr<Work> work = std::move(g_work);
    if (work) ApplyWork(*work);
}

bool Busy() { return g_worker.joinable(); }

void Start(std::unique_ptr<Work> work) {
    g_work = std::move(work);
    g_worker = std::thread([work = g_work.get()] {
        work->done.assign(work->moves.size(), false);
        for (size_t i = 0; i < work->moves.size(); ++i)
            work->done[i] = work->undo ? RestoreFromRecycleBin(work->moves[i].extra) : RecycleFile(work->moves[i].extra);
        PostMessageW(nullptr, WM_APP_DUPLICATES, 0, 0);
    });
    g.redraw = true;
}

void RemoveTicked() {
    if (Busy()) return;
    if (TagWriteBusy() || ConvertBusy()) {
        ShowMessage(L"Tags are still being written; try again in a moment.");  // a conversion rewrites them too
        return;
    }
    size_t whole = 0;
    auto work = std::make_unique<Work>();
    work->moves = duplicates::Plan(g_lib, g_dup.groups, g_dup.ticks, g.playing, &whole);
    if (work->moves.empty()) {
        ShowMessage(whole ? L"Leave one copy of each song unchecked." : L"Check the tracks to remove first.");
        return;
    }
    std::string q = "Move " + Count(work->moves.size(), "file", "files") +
                    " to the trash?\n\nTheir plays, ratings and playlist places move to the copy kept.";
    if (whole) q += " " + Count(whole, "song", "songs") + " with every copy checked stay.";
    if (!Confirm(q, "Find duplicates")) return;
    Start(std::move(work));
}

void Undo() {
    if (Busy() || g_cleanup.Empty()) return;
    auto work = std::make_unique<Work>();
    work->undo = true;
    for (const auto& m : g_cleanup.merged) work->moves.push_back({m.extra, m.keep});
    Start(std::move(work));
}

void Measure() {
    if (g.rgRunning && !g.rgAuto) {
        ShowMessage(L"Wait for the ReplayGain scan to finish.");
        return;
    }
    ScanReplayGain(duplicates::Unmeasured(g_lib));
    g_dup.measuring = g.rgRunning;
    g.redraw = true;
}

void DrawDuplicates(const Palette& p) {
    auto& d = g_dup;
    Rect panel = DrawFrame(p, S(880), S(600), "Find duplicates");
    const float pad = S(16), x0 = panel.x + pad, w = panel.w - 2 * pad;
    float y = panel.y + S(48);
    const std::string status =
        d.groups.empty() ? "No duplicates found." : Count(d.groups.size(), "song has", "songs have") + " more than one copy.";
    d.measure = {panel.R() - pad - S(140), y - S(4), S(140), S(28)};
    DrawText(Font::Normal, status, x0, y, p.text, w - S(150));
    DrawButton(d.measure, "Measure them", p, false, !Busy() && !d.measuring && d.unmeasured);
    y += S(34);
    const float by = panel.B() - pad - S(28);
    d.list = {x0, y, w, std::max(S(80), by - S(40) - y)};
    Fill(d.list, p.bg);
    Frame(d.list, p.border);
    const float row = S(kRow);
    const int visible = std::max(1, (int)((d.list.h - S(4)) / row));
    d.first = std::clamp(d.first, 0, std::max(0, (int)d.lines.size() - visible));
    SDL_Rect clip{(int)d.list.x + 1, (int)d.list.y + 1, (int)d.list.w - 2, (int)d.list.h - 2};
    SDL_SetRenderClipRect(g.renderer, &clip);
    const float song = w * 0.32f, format = S(130), length = S(64), plays = S(56);
    for (int i = d.first; i < (int)d.lines.size() && i < d.first + visible; ++i) {
        const Line& line = d.lines[(size_t)i];
        const auto& group = d.groups[line.group];
        Rect r{x0 + S(1), y + S(2) + (i - d.first) * row, w - S(2), row};
        if (line.copy < 0) {
            const Track& best = g_lib[group.tracks[0]];
            std::string title(g_lib.View(best.title ? best.title : best.file));
            if (best.artist) title += "  \xE2\x80\x94  " + std::string(g_lib.View(best.artist));
            DrawText(Font::Normal, (group.identical ? "Identical audio: " : "Same song: ") + title, r.x + S(6),
                     TextY(r), p.sub, r.w - S(12));
            continue;
        }
        const TrackId id = group.tracks[(size_t)line.copy];
        const Track& t = g_lib[id];
        std::string name(g_lib.View(t.title ? t.title : t.file));
        if (line.copy == 0) name += "  (best copy)";
        float x = r.x + S(10);
        DrawCheck({x, r.y, S(16), r.h}, "", d.ticks[line.group][(size_t)line.copy] ? 1 : 0, p);
        x += S(24);
        DrawText(Font::Normal, name, x, TextY(r), p.text, song - S(8));
        x += song;
        DrawText(Font::Normal, Utf8(duplicates::FormatText(t)), x, TextY(r), p.text, format - S(8));
        x += format;
        DrawText(Font::Normal, Utf8(FormatDuration(t.Duration())), x, TextY(r), p.text, length - S(8));
        x += length;
        DrawText(Font::Normal, std::to_string(t.playCount) + " plays", x, TextY(r), p.sub, plays + S(8));
        x += plays + S(16);
        DrawText(Font::Normal, std::string(g_lib.View(t.dir)), x, TextY(r), p.sub, std::max(S(40), r.R() - x - S(6)));
    }
    SDL_SetRenderClipRect(g.renderer, nullptr);
    std::string info = d.measuring ? "Measuring\xE2\x80\xA6"
                     : d.unmeasured ? Count(d.unmeasured, "song", "songs") + " not measured yet: Measure them to compare by sound."
                                    : "Checked copies go to the trash; their history moves to the copy kept.";
    DrawText(Font::Small, info, x0, by - S(26), p.sub, w);
    d.undo = {x0, by, S(130), S(28)};
    d.remove = {panel.R() - pad - S(300), by, S(192), S(28)};
    d.close = {panel.R() - pad - S(100), by, S(100), S(28)};
    DrawButton(d.undo, "Undo clean-up", p, false, !Busy() && !g_cleanup.Empty());
    DrawButton(d.remove, "Move checked to trash", p, true, !Busy() && !d.lines.empty());
    DrawButton(d.close, "Close", p);
}

void DuplicatesMouseDown(float x, float y, int) {
    auto& d = g_dup;
    if (d.close.In(x, y)) return ClosePanel();
    if (d.remove.In(x, y)) return RemoveTicked();
    if (d.undo.In(x, y)) return Undo();
    if (d.measure.In(x, y) && d.unmeasured && !d.measuring && !Busy()) return Measure();
    if (!d.list.In(x, y)) return;
    const int i = d.first + (int)((y - d.list.y - S(2)) / S(kRow));
    if (i < 0 || i >= (int)d.lines.size() || d.lines[(size_t)i].copy < 0) return;
    auto&& tick = d.ticks[d.lines[(size_t)i].group][(size_t)d.lines[(size_t)i].copy];
    tick = !tick;
    g.redraw = true;
}

void DuplicatesKey(const SDL_KeyboardEvent& e) {
    if (e.key == SDLK_ESCAPE) ClosePanel();
}

void DuplicatesWheel(float x, float y, float dy) {
    if (!g_dup.list.In(x, y)) return;
    g_dup.first = std::max(0, g_dup.first - (int)std::lround(dy * 3));
    g.redraw = true;
}

void DuplicatesClose() { g_dup = DuplicatesPanel{}; }

}  // namespace

const Ops kDuplicatesOps{
    .draw = DrawDuplicates,
    .mouseDown = DuplicatesMouseDown,
    .wheel = DuplicatesWheel,
    .key = DuplicatesKey,
    .close = DuplicatesClose,
};

}  // namespace lx::panel

namespace lx {
using namespace panel;

void OnDuplicatesMessage() {
    Finish();
    if (g_kind == Kind::Duplicates) Populate();
}

void StopDuplicates() { Finish(); }

void DuplicatesMeasured() {
    if (g_kind != Kind::Duplicates || !g_dup.measuring) return;
    g_dup.measuring = false;
    Populate();
}

void OpenDuplicates() {
    ClosePanel();
    Populate();
    g_kind = Kind::Duplicates;
    g.redraw = true;
}

}  // namespace lx
