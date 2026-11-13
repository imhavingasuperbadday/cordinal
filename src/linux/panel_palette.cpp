// The command palette on Linux (View > Command palette, Ctrl+Shift+P), as the Windows build's app_palette.cpp:
// one search over every command of the menu bar, with its checkmark, shortcut and where it lives, and over the
// settings of Preferences (a switch flips, the rest open Preferences at that setting). Enter or a click runs
// the command once the palette has closed.
#include "panels_internal.h"

#include "textcase.h"

namespace lx::panel {
namespace {

struct PaletteEntry {
    PaletteCommand command;
    int menu = -1;  // an index for RunMenuCommand, or
    int pref = -1;  // an index of Preferences' settings (PrefCommand)
    std::wstring searchable, label;  // folded, as the query is: label is the command's own name
};
struct PalettePanel {
    TextField query;
    std::vector<PaletteEntry> entries;
    std::vector<int> shown;  // entries matching the query, best first
    int selected = 0, first = 0;
    Rect panel, field, list;
} g_palette;
constexpr int kPaletteRows = 11;

void PaletteFilter() {
    std::wstring q = textcase::Fold(g_palette.query.text);
    std::vector<std::wstring> words;
    for (size_t i = 0; i < q.size();) {
        size_t end = q.find(L' ', i);
        if (end == std::wstring::npos) end = q.size();
        if (end > i) words.push_back(q.substr(i, end - i));
        i = end + 1;
    }
    std::vector<std::pair<int, int>> scored;  // rank, entry
    for (int i = 0; i < (int)g_palette.entries.size(); ++i) {
        const PaletteEntry& e = g_palette.entries[i];
        int rank = 0;
        bool all = true;
        const std::wstring& label = e.label;
        for (const std::wstring& w : words) {
            if (e.searchable.find(w) == std::wstring::npos) {
                all = false;
                break;
            }
            rank += label.starts_with(w) ? 0 : label.find(w) != std::wstring::npos ? 1 : 2;
        }
        if (all) scored.push_back({rank, i});
    }
    std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    g_palette.shown.clear();
    for (auto& s : scored) g_palette.shown.push_back(s.second);
    g_palette.selected = 0;
    g_palette.first = 0;
}

void PaletteRun(int row) {
    if (row < 0 || row >= (int)g_palette.shown.size()) return;
    const PaletteEntry e = g_palette.entries[g_palette.shown[row]];
    if (!e.command.enabled) return;
    ClosePanel();
    if (e.menu >= 0) {
        RunMenuCommand((size_t)e.menu);
    } else if (e.pref >= 0) {
        RunPref(e.pref);
    }
}

void DrawPalette(const Palette& p) {
    Rect panel = g_palette.panel = DrawFrame(p, S(640), S(470), "Command palette");
    const float pad = S(16), x0 = panel.x + pad, w = panel.w - 2 * pad, rowH = S(30);
    g_palette.field = {x0, panel.y + S(48), w, S(30)};
    DrawLineField(g_palette.query, g_palette.field, true, true, "Type a command or a setting", p);
    g_palette.list = {x0, g_palette.field.B() + S(8), w, rowH * kPaletteRows};
    for (int i = 0; i < kPaletteRows && g_palette.first + i < (int)g_palette.shown.size(); ++i) {
        const int row = g_palette.first + i;
        const PaletteEntry& e = g_palette.entries[g_palette.shown[row]];
        Rect r{g_palette.list.x, g_palette.list.y + i * rowH, g_palette.list.w, rowH};
        const bool selected = row == g_palette.selected;
        if (selected) Fill(r, p.select);
        else if (r.In(g_mx, g_my)) Fill(r, p.hover);
        const uint32_t main = !e.command.enabled ? p.faint : selected ? p.selectText : p.text;
        const uint32_t sub = !e.command.enabled ? p.faint : selected ? p.selectText : p.sub;
        std::string label = (e.command.checked ? "\xE2\x9C\x93 " : "") + e.command.label;
        const float shortcutW = e.command.shortcut.empty() ? 0 : TextWidth(Font::Normal, e.command.shortcut) + S(12);
        const float pathW = std::min(TextWidth(Font::Small, e.command.path) + S(12), r.w * 0.4f);
        DrawText(Font::Normal, label, r.x + S(8), TextY(r), main, r.w - shortcutW - pathW - S(16));
        DrawText(Font::Small, e.command.path, r.R() - shortcutW - pathW, TextY(r, Font::Small), sub, pathW - S(8), 2);
        if (shortcutW > 0) DrawText(Font::Normal, e.command.shortcut, r.R() - shortcutW, TextY(r), sub, shortcutW - S(8), 2);
    }
    if (g_palette.shown.empty()) DrawText(Font::Normal, "Nothing matches.", g_palette.list.x + S(8), g_palette.list.y + S(6), p.sub);
}

void PaletteMouseDown(float x, float y) {
    if (!g_palette.panel.In(x, y)) return ClosePanel();
    if (g_palette.list.In(x, y)) PaletteRun(g_palette.first + (int)((y - g_palette.list.y) / S(30)));
}

void PaletteKey(const SDL_KeyboardEvent& e) {
    const int count = (int)g_palette.shown.size();
    auto move = [&](int to) {
        g_palette.selected = std::clamp(to, 0, std::max(0, count - 1));
        if (g_palette.selected < g_palette.first) g_palette.first = g_palette.selected;
        else if (g_palette.selected >= g_palette.first + kPaletteRows) g_palette.first = g_palette.selected - kPaletteRows + 1;
    };
    switch (e.key) {
        case SDLK_ESCAPE: return ClosePanel();
        case SDLK_RETURN: case SDLK_KP_ENTER: return PaletteRun(g_palette.selected);
        case SDLK_UP: return move(g_palette.selected - 1);
        case SDLK_DOWN: return move(g_palette.selected + 1);
        case SDLK_PAGEUP: return move(g_palette.selected - kPaletteRows);
        case SDLK_PAGEDOWN: return move(g_palette.selected + kPaletteRows);
        default: break;
    }
    if (LineFieldKey(g_palette.query, e) == 1) PaletteFilter();
}

void PaletteText(const char* s) {
    if (LineFieldInsert(g_palette.query, Printable(compat::Widen(s)))) PaletteFilter();
}

}  // namespace

const Ops kPaletteOps{
    .draw = DrawPalette,
    .mouseDown = [](float x, float y, int) { PaletteMouseDown(x, y); },
    .wheel = [](float, float, float dy) {
        g_palette.first = std::clamp(g_palette.first - (int)std::lround(dy * 2), 0, std::max(0, (int)g_palette.shown.size() - kPaletteRows));
    },
    .key = PaletteKey,
    .text = PaletteText,
    .wantsText = [](float x, float y) { return g_palette.field.In(x, y); },
    .typing = true,
};

}  // namespace lx::panel

namespace lx {
using namespace panel;

void OpenPalette() {
    g_palette = PalettePanel{};
    auto commands = MenuCommands();
    for (size_t i = 0; i < commands.size(); ++i) {
        PaletteEntry e;
        e.command = commands[i];
        e.menu = (int)i;
        e.searchable = textcase::Fold(compat::Widen((e.command.label + " " + e.command.path).c_str()));
        g_palette.entries.push_back(std::move(e));
    }
    for (int i = 0; i < (int)PrefCount(); ++i) {
        if (!PrefListed(i)) continue;
        PaletteEntry e;
        e.command = PrefCommand(i);
        e.pref = i;
        e.searchable = PrefSearchText(i);
        g_palette.entries.push_back(std::move(e));
    }
    for (PaletteEntry& e : g_palette.entries) e.label = textcase::Fold(compat::Widen(e.command.label.c_str()));
    PaletteFilter();
    g_kind = Kind::Palette;
    SDL_StartTextInput(g.window);
    g.redraw = true;
}

}  // namespace lx
