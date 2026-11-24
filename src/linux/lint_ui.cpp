// Library > Lint library on Linux (lint_ui.cpp, as app_lint.cpp on Windows): one review screen
// for split albums, near-duplicate artist names, mojibake / legacy encoding in tags, and cover-art
// issues. Fast metadata checks and the disk-backed cover audit run on a worker thread; checked fixes
// apply atomically through SubmitTagWrites so Edit > Undo last tag edit reverses the whole run.
#include "panels_internal.h"

#include "library_lint.h"

#include <atomic>
#include <memory>
#include <thread>

namespace lx::panel {
namespace {

struct Line {
    int kind = -1;     // >= 0 for section header
    int finding = -1;  // >= 0 for finding row
};

struct LintPanel {
    std::vector<lint::LintFinding> findings;
    std::vector<Line> lines;
    int first = 0;
    size_t progressDone = 0, progressTotal = 0;
    Rect list, selectAll, selectNone, rescan, undo, fix, close;
} g_lint;

std::atomic<bool> g_cancel{false};
std::unique_ptr<std::vector<lint::LintFinding>> g_scanResult;
std::thread g_worker;

constexpr float kRow = 24;

bool Busy() { return g_worker.joinable(); }

void StopWorker() {
    g_cancel.store(true);
    if (g_worker.joinable()) g_worker.join();
    g_cancel.store(false);
}

void RebuildLines() {
    g_lint.lines.clear();
    static constexpr std::pair<lint::LintKind, int> kOrder[] = {
        {lint::LintKind::SplitAlbum, 0},
        {lint::LintKind::NearDuplicateArtist, 1},
        {lint::LintKind::Mojibake, 2},
        {lint::LintKind::CoverArt, 3},
    };
    for (auto [kind, kidx] : kOrder) {
        bool any = false;
        for (const auto& f : g_lint.findings)
            if (f.kind == kind) {
                any = true;
                break;
            }
        if (!any) continue;
        g_lint.lines.push_back({kidx, -1});
        for (size_t i = 0; i < g_lint.findings.size(); ++i)
            if (g_lint.findings[i].kind == kind) g_lint.lines.push_back({-1, (int)i});
    }
    g_lint.first = 0;
    g.redraw = true;
}

void StartScan() {
    StopWorker();
    g_lint.findings.clear();
    g_lint.lines.clear();
    g_lint.progressDone = 0;
    g_lint.progressTotal = 0;
    g.redraw = true;

    std::vector<lint::LintFinding> meta;
    lint::FindMetadataIssues(g_lib, meta);
    auto coverTracks = lint::SnapshotCoverTracks(g_lib);

    g_worker = std::thread([meta = std::move(meta), coverTracks = std::move(coverTracks)]() mutable {
        auto res = std::make_unique<std::vector<lint::LintFinding>>(std::move(meta));
        lint::AuditCoverTracks(coverTracks, {}, *res, &g_cancel);
        if (!g_cancel.load()) {
            g_scanResult = std::move(res);
            PostMessageW(nullptr, WM_APP_LINT, 0, 0);
        }
    });
}

void DrawLint(const Palette& p) {
    auto& d = g_lint;
    Rect panel = DrawFrame(p, S(920), S(600), "Lint my library");
    const float pad = S(16), x0 = panel.x + pad, w = panel.w - 2 * pad;
    float y = panel.y + S(48);

    size_t checked = 0, fixable = 0;
    for (const auto& f : d.findings) {
        if (!f.fixable) continue;
        ++fixable;
        if (f.checked) ++checked;
    }

    std::string status = Busy() ? "Scanning library tags and embedded covers..."
                       : d.findings.empty()
                           ? "No library issues found."
                           : std::to_string(d.findings.size()) +
                                 (d.findings.size() == 1 ? " issue found (" : " issues found (") +
                                 std::to_string(checked) + " checked to fix).";
    DrawText(Font::Normal, status, x0, y, p.text, w);
    y += S(30);

    const float by = panel.B() - pad - S(28);
    d.list = {x0, y, w, std::max(S(80), by - S(38) - y)};
    Fill(d.list, p.bg);
    Frame(d.list, p.border);

    const float row = S(kRow);
    const int visible = std::max(1, (int)((d.list.h - S(4)) / row));
    d.first = std::clamp(d.first, 0, std::max(0, (int)d.lines.size() - visible));
    SDL_Rect clip{(int)d.list.x + 1, (int)d.list.y + 1, (int)d.list.w - 2, (int)d.list.h - 2};
    SDL_SetRenderClipRect(g.renderer, &clip);

    static const char* const kHeaders[] = {"Split albums", "Near-duplicate artists", "Garbled tags (encoding)",
                                           "Cover-art audit"};
    const float col1 = w * 0.44f, col2 = w * 0.27f;
    for (int i = d.first; i < (int)d.lines.size() && i < d.first + visible; ++i) {
        const Line& line = d.lines[(size_t)i];
        Rect r{x0 + S(1), y + S(2) + (i - d.first) * row, w - S(2), row};
        if (line.kind >= 0) {
            DrawText(Font::Normal, kHeaders[line.kind], r.x + S(8), TextY(r), p.sub, r.w - S(16));
            continue;
        }
        const auto& f = d.findings[(size_t)line.finding];
        float x = r.x + S(10);
        if (f.fixable) DrawCheck({x, r.y, S(16), r.h}, "", f.checked ? 1 : 0, p);
        x += S(24);
        DrawText(Font::Normal, Utf8(f.summary), x, TextY(r), p.text, col1 - S(30));
        x = r.x + col1;
        DrawText(Font::Normal, Utf8(f.before), x, TextY(r), p.sub, col2 - S(8));
        x += col2;
        std::string after = Utf8(f.fixable ? f.after : (f.after + L" (review)"));
        DrawText(Font::Normal, after, x, TextY(r), p.text, std::max(S(40), r.R() - x - S(8)));
    }
    SDL_SetRenderClipRect(g.renderer, nullptr);

    DrawText(Font::Small, "Checked fixes apply atomically and can be reversed with Edit > Undo last tag edit.", x0,
             by - S(24), p.sub, w);

    d.selectAll = {x0, by, S(96), S(28)};
    d.selectNone = {x0 + S(104), by, S(96), S(28)};
    d.rescan = {x0 + S(208), by, S(84), S(28)};
    d.close = {panel.R() - pad - S(84), by, S(84), S(28)};
    d.fix = {d.close.x - S(8) - S(136), by, S(136), S(28)};
    d.undo = {d.fix.x - S(8) - S(116), by, S(116), S(28)};

    std::string fixLabel = checked ? ("Fix checked (" + std::to_string(checked) + ")") : "Fix checked";
    DrawButton(d.selectAll, "Select all", p, false, !Busy() && fixable > 0);
    DrawButton(d.selectNone, "Select none", p, false, !Busy() && checked > 0);
    DrawButton(d.rescan, "Rescan", p, false, !Busy());
    DrawButton(d.undo, "Undo last edit", p, false, !Busy() && !TagWriteBusy() && CanUndoTagEdit());
    DrawButton(d.fix, fixLabel, p, true, !Busy() && checked > 0 && !TagWriteBusy());
    DrawButton(d.close, "Close", p);
}

void LintMouseDown(float x, float y, int) {
    auto& d = g_lint;
    if (d.close.In(x, y)) return ClosePanel();
    if (d.rescan.In(x, y) && !Busy()) return StartScan();
    if (d.selectAll.In(x, y) && !Busy()) {
        for (auto& f : d.findings)
            if (f.fixable) f.checked = true;
        g.redraw = true;
        return;
    }
    if (d.selectNone.In(x, y) && !Busy()) {
        for (auto& f : d.findings)
            if (f.fixable) f.checked = false;
        g.redraw = true;
        return;
    }
    if (d.undo.In(x, y) && !Busy() && !TagWriteBusy()) {
        if (CanUndoTagEdit()) {
            UndoLastTagEdit();
            StartScan();
        }
        return;
    }
    if (d.fix.In(x, y) && !Busy() && !TagWriteBusy()) {
        auto edits = lint::CollectCheckedEdits(d.findings);
        if (!edits.empty() && SubmitTagWrites(edits, true)) ClosePanel();
        return;
    }
    if (!d.list.In(x, y)) return;
    const int i = d.first + (int)((y - d.list.y - S(2)) / S(kRow));
    if (i < 0 || i >= (int)d.lines.size() || d.lines[(size_t)i].finding < 0) return;
    auto& f = d.findings[(size_t)d.lines[(size_t)i].finding];
    if (!f.fixable) return;
    f.checked = !f.checked;
    g.redraw = true;
}

void LintWheel(float x, float y, float dy) {
    if (!g_lint.list.In(x, y)) return;
    g_lint.first = std::max(0, g_lint.first - (int)std::lround(dy * 3));
    g.redraw = true;
}

void LintKey(const SDL_KeyboardEvent& e) {
    if (e.key == SDLK_ESCAPE) ClosePanel();
}

void LintClose() {
    StopWorker();
    g_lint = LintPanel{};
}

}  // namespace

const Ops kLintOps{
    .draw = DrawLint,
    .mouseDown = LintMouseDown,
    .wheel = LintWheel,
    .key = LintKey,
    .close = LintClose,
};

}  // namespace lx::panel

namespace lx {
using namespace panel;

void OnLintMessage() {
    StopWorker();
    if (g_kind == Kind::Lint && g_scanResult) {
        g_lint.findings = std::move(*g_scanResult);
        g_scanResult.reset();
        RebuildLines();
    }
}

void StopLint() { StopWorker(); }

void OpenLint() {
    ClosePanel();
    g_kind = Kind::Lint;
    StartScan();
}

}  // namespace lx
