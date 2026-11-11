// The Linux queue editor, the Up next panel beside the list and in Big mode, as on Windows
// (app_panels.cpp): what you queued, Choose by taste's picks, then what Now Playing plays after them.
// Queued tracks can be selected, dragged into a new order, removed and played, and tracks dragged
// from the list land in the queue. The queue itself and its edits live in playback.cpp.
#include "big_ui.h"
#include "ui_internal.h"

namespace lx {

namespace {

enum Kind { kYours, kPick, kThen, kLabelPicks, kLabelThen };

// The right-click menu's commands, the arg of its kQueueEdit items.
enum { kQueuePlayNow = 1, kQueueTop, kQueueBottom, kQueueKeep, kQueueRemove, kQueueThenNext, kQueueThenAdd, kQueueRepick,
       kQueueClear };
TrackId g_queueMenuTrack = kNone;  // the Now Playing track a menu's Play next / Add to queue is for

struct QueueRow {
    TrackId id;
    int index;  // queued rows: the queue position; kThen: -1
    Kind kind;
};

struct QueueEditor {
    std::vector<char> sel;  // per queue position
    std::deque<TrackId> seen;
    int focus = -1, anchor = -1, deferSelect = -1, dropSlot = -1;
    float scroll = 0, pressX = 0, pressY = 0;
    bool focused = false, press = false, dragging = false;
} q;

float HeaderH() { return S(40); }
float RowH(const QueueRow& row) { return row.kind >= kLabelPicks ? S(26) : S(44); }
bool Queued(const QueueRow& row) { return row.kind == kYours || row.kind == kPick; }

std::vector<QueueRow> Rows() {
    std::vector<QueueRow> rows;
    size_t yours = QueuedByYou();
    for (size_t i = 0; i < g.queue.size(); ++i) {
        if (i == yours) rows.push_back({kNone, -1, kLabelPicks});
        rows.push_back({g.queue[i], (int)i, i < yours ? kYours : kPick});
    }
    if (g.queuePicks || g.playing == kNone) return rows;
    bool labelled = false;
    for (TrackId id : UpcomingTracks()) {
        if (!g_lib.Contains(id)) continue;
        if (!labelled) rows.push_back({kNone, -1, kLabelThen});
        labelled = true;
        rows.push_back({id, -1, kThen});
    }
    return rows;
}

// The selection follows the queue: its head leaving (it started playing) shifts it, anything else
// keeps it by position.
void Sync() {
    size_t n = g.queue.size(), seen = q.seen.size();
    if (n < seen && std::equal(g.queue.begin(), g.queue.end(), q.seen.begin() + (seen - n))) {
        int k = (int)(seen - n);
        q.sel.erase(q.sel.begin(), q.sel.begin() + std::min<size_t>(k, q.sel.size()));
        q.focus = std::max(-1, q.focus - k);
        q.anchor = std::max(-1, q.anchor - k);
    }
    q.sel.resize(n, 0);
    if (q.focus >= (int)n) q.focus = (int)n - 1;
    if (q.anchor >= (int)n) q.anchor = q.focus;
    q.seen = g.queue;
}

float ContentH(const std::vector<QueueRow>& rows) {
    float h = 0;
    for (const auto& row : rows) h += RowH(row);
    return h;
}

void ClampScroll(const std::vector<QueueRow>& rows) {
    q.scroll = std::clamp(q.scroll, 0.f, std::max(0.f, ContentH(rows) - (L.upNext.h - HeaderH())));
}

int RowAt(const std::vector<QueueRow>& rows, float y) {
    float top = L.upNext.y + HeaderH() - q.scroll;
    if (y < L.upNext.y + HeaderH()) return -1;
    for (int i = 0; i < (int)rows.size(); ++i) {
        float h = RowH(rows[i]);
        if (y < top + h) return i;
        top += h;
    }
    return -1;
}

float RowTop(const std::vector<QueueRow>& rows, int row) {
    float top = L.upNext.y + HeaderH() - q.scroll;
    for (int i = 0; i < row && i < (int)rows.size(); ++i) top += RowH(rows[i]);
    return top;
}

int RowOfQueued(const std::vector<QueueRow>& rows, int index) {
    for (int i = 0; i < (int)rows.size(); ++i)
        if (Queued(rows[i]) && rows[i].index == index) return i;
    return -1;
}

void ShowQueued(int index) {
    auto rows = Rows();
    int row = RowOfQueued(rows, index);
    if (row < 0) return;
    float top = RowTop(rows, row) - (L.upNext.y + HeaderH()) + q.scroll, h = RowH(rows[row]);
    if (top < q.scroll) q.scroll = top;
    else if (top + h > q.scroll + L.upNext.h - HeaderH()) q.scroll = top + h - (L.upNext.h - HeaderH());
    ClampScroll(rows);
}

// The queue position a drag at `y` would land tracks before; yours (and drops from the list) stay
// above the picks.
int SlotAt(float y, bool amongYours) {
    auto rows = Rows();
    int slot = (int)g.queue.size();
    float top = L.upNext.y + HeaderH() - q.scroll;
    for (const auto& row : rows) {
        float h = RowH(row);
        if (Queued(row) && y < top + h / 2) {
            slot = row.index;
            break;
        }
        top += h;
    }
    return amongYours ? std::min(slot, (int)QueuedByYou()) : slot;
}

std::vector<int> Selected() {
    std::vector<int> out;
    for (int i = 0; i < (int)q.sel.size() && i < (int)g.queue.size(); ++i)
        if (q.sel[i]) out.push_back(i);
    return out;
}

void SelectQueued(int index) {
    q.sel.assign(g.queue.size(), 0);
    if (index >= 0 && index < (int)q.sel.size()) q.sel[index] = 1;
    q.focus = q.anchor = index;
}

void MoveSelection(int to, bool keep = false) {
    auto picked = Selected();
    if (picked.empty()) return;
    int first = MoveInQueue(picked, to, keep);
    if (first < 0) return;
    q.seen = g.queue;
    q.sel.assign(g.queue.size(), 0);
    for (int i = first; i < first + (int)picked.size() && i < (int)q.sel.size(); ++i) q.sel[i] = 1;
    q.focus = q.anchor = first;
    ShowQueued(first);
}

void RemoveSelection() {
    auto picked = Selected();
    if (picked.empty()) return;
    RemoveFromQueue(picked);
    q.seen = g.queue;
    SelectQueued(g.queue.empty() ? -1 : std::min(picked.front(), (int)g.queue.size() - 1));
}

// The header's links, right-aligned: Clear with anything queued, New picks while taste picks.
Rect ClearLink() {
    if (g.queue.empty()) return {};
    float w = TextWidth(Font::Normal, "Clear");
    return {L.upNext.R() - S(12) - w, L.upNext.y, w, HeaderH()};
}

Rect RepickLink() {
    if (!g.queuePicks || !TastePicking()) return {};
    Rect clear = ClearLink();
    float w = TextWidth(Font::Normal, "New picks"), right = clear.w > 0 ? clear.x - S(14) : L.upNext.R() - S(12);
    return {right - w, L.upNext.y, w, HeaderH()};
}

void PlayRow(const QueueRow& row) {
    if (Queued(row)) {
        PlayQueuedAt(row.index);
    } else if (row.kind == kThen) {
        // Its first place in Now Playing after the playing one.
        for (const auto& pl : g.playlists) {
            if (pl.kind != PlaylistKind::NowPlaying) continue;
            int n = (int)pl.items.size();
            for (int i = 1; i <= n; ++i) {
                int k = (std::max(g.queuePos, 0) + i) % n;
                if (pl.items[k] == row.id) {
                    PlayFromQueue(k);
                    break;
                }
            }
            break;
        }
    }
    SelectQueued(-1);
}

}  // namespace

bool QueueFocused() { return q.focused && L.upNext.w > 0; }

// Paints the header (Up next, the queue's length and the links), then the visible rows: your tracks numbered
// in the accent, the taste picks under their own label, then what Now Playing plays after them.
void DrawUpNext(const Palette& p) {
    if (L.upNext.w <= 0 || L.upNext.h <= 0) return;
    Sync();
    auto rows = Rows();
    ClampScroll(rows);
    SDL_Rect clip{(int)L.upNext.x, (int)L.upNext.y, (int)L.upNext.w, (int)L.upNext.h};
    SDL_SetRenderClipRect(g.renderer, &clip);
    Fill(L.upNext, p.surface);
    Frame(L.upNext, p.border);
    float pad = S(12), x = L.upNext.x;
    double total = 0;
    for (TrackId id : g.queue) total += g_lib.Contains(id) ? g_lib[id].Duration() : 0;
    Rect header{x, L.upNext.y, L.upNext.w, HeaderH()};
    float titleW = DrawText(Font::Bold, "Up next", x + pad, TextY(header, Font::Bold), p.text);
    std::string sub = g.queue.empty() ? "nothing queued"
                                      : std::to_string(g.queue.size()) + " queued, " + Utf8(FormatDuration(total));
    Rect clear = ClearLink(), repick = RepickLink();
    float linksLeft = repick.w > 0 ? repick.x : clear.w > 0 ? clear.x : L.upNext.R();
    DrawText(Font::Normal, sub, x + pad + titleW + S(8), TextY(header), p.faint, linksLeft - S(8) - (x + pad + titleW + S(8)));
    auto link = [&](const Rect& r, const char* text) {
        if (r.w <= 0) return;
        DrawText(Font::Normal, text, r.x, TextY(r), r.In(g_mouseX, g_mouseY) ? p.text : p.accent);
    };
    link(clear, "Clear");
    link(repick, "New picks");
    Fill({x, header.B() - S(1), L.upNext.w, S(1)}, p.border);

    SDL_Rect list{(int)x, (int)header.B(), (int)L.upNext.w, (int)(L.upNext.B() - header.B())};
    SDL_SetRenderClipRect(g.renderer, &list);
    if (rows.empty())
        DrawText(Font::Normal, g.playing == kNone ? "Play something, or drag tracks here." : "Nothing comes next. Drag tracks here.",
                 x + pad, header.B() + S(12), p.faint, L.upNext.w - 2 * pad);
    float y = header.B() - q.scroll, numW = S(30), timeW = S(52);
    for (const auto& row : rows) {
        float h = RowH(row);
        Rect r{x, y, L.upNext.w, h};
        y += h;
        if (r.B() <= header.B()) continue;
        if (r.y >= L.upNext.B()) break;
        if (row.kind == kLabelPicks || row.kind == kLabelThen) {
            std::string label = row.kind == kLabelPicks ? "Picked by taste" : "Then from Now Playing";
            DrawText(Font::Small, label, x + pad, TextY(r, Font::Small), p.faint, L.upNext.w - 2 * pad);
            continue;
        }
        bool queued = Queued(row), selected = queued && row.index < (int)q.sel.size() && q.sel[row.index];
        uint32_t text = p.text, sub2 = p.sub;
        if (selected) {
            Fill(r, p.select);
            text = sub2 = p.selectText;
        } else if (r.In(g_mouseX, g_mouseY) && !q.dragging && L.upNext.In(g_mouseX, g_mouseY)) {
            Fill(r, p.hover);
        }
        if (q.dragging && selected) text = sub2 = p.faint;
        if (QueueFocused() && queued && row.index == q.focus) Fill({x, r.y, S(3), h}, p.accent);
        if (!selected) Fill({x + pad, r.B() - S(1), L.upNext.w - 2 * pad, S(1)}, p.border);
        if (queued)
            DrawText(row.kind == kYours ? Font::Bold : Font::Normal, std::to_string(row.index + 1), x + pad, TextY(r),
                     selected ? text : row.kind == kYours ? p.accent : p.faint, numW);
        float tx = x + pad + numW, tw = L.upNext.w - 2 * pad - numW - timeW;
        bool then = row.kind == kThen;
        std::wstring artist = TrackArtist(row.id), album = g_lib.Str(g_lib[row.id].album);
        if (!album.empty()) artist += (artist.empty() ? L"" : L"  \x2022  ") + album;
        DrawText(then ? Font::Normal : Font::Bold, Utf8(TrackTitle(row.id)), tx, r.y + S(5), then && !selected ? p.sub : text, tw);
        DrawText(Font::Small, Utf8(artist), tx, r.y + S(24), then && !selected ? p.faint : sub2, tw);
        DrawText(Font::Normal, Utf8(FormatDuration(g_lib[row.id].Duration())), x + L.upNext.w - pad - timeW, TextY(r), sub2, timeW, 2);
    }
    if (q.dropSlot >= 0) {
        int before = q.dropSlot > 0 ? RowOfQueued(rows, q.dropSlot - 1) : RowOfQueued(rows, 0);
        float ly = before < 0 ? header.B() : RowTop(rows, before) + (q.dropSlot > 0 ? RowH(rows[before]) : 0);
        Fill({x + pad, ly - S(1), L.upNext.w - 2 * pad, S(2)}, p.accent);
        Fill({x + pad, ly - S(4), S(2), S(8)}, p.accent);
    }
    SDL_SetRenderClipRect(g.renderer, nullptr);
}

// A press anywhere: inside the panel it selects (Ctrl toggles, Shift extends), starts a possible
// drag, follows a header link or plays on a double click, and puts the keyboard there; outside it
// takes the keyboard away. True when the panel took the press.
bool QueueMouseDown(float x, float y, int button, int clicks, SDL_Keymod mod) {
    q.focused = L.upNext.w > 0 && L.upNext.In(x, y);
    if (!q.focused) return false;
    Sync();
    auto rows = Rows();
    int r = RowAt(rows, y);
    if (button == SDL_BUTTON_LEFT) {
        if (ClearLink().In(x, y)) { ClearQueue(); SelectQueued(-1); return true; }
        if (RepickLink().In(x, y)) { RepickTaste(); SelectQueued(-1); return true; }
        if (clicks >= 2 && r >= 0 && rows[r].kind <= kThen) { PlayRow(rows[r]); return true; }
    }
    bool queued = r >= 0 && Queued(rows[r]);
    if (button == SDL_BUTTON_RIGHT) {
        if (queued && !q.sel[rows[r].index]) SelectQueued(rows[r].index);
        OpenQueueMenu(x, y, r >= 0 && rows[r].kind == kThen ? rows[r].id : kNone);
        return true;
    }
    if (button != SDL_BUTTON_LEFT) return true;
    if (!queued) {
        if (!(mod & SDL_KMOD_CTRL)) SelectQueued(-1);
        return true;
    }
    int index = rows[r].index;
    q.deferSelect = -1;
    if ((mod & SDL_KMOD_SHIFT) && q.anchor >= 0) {
        if (!(mod & SDL_KMOD_CTRL)) q.sel.assign(g.queue.size(), 0);
        for (int i = std::min(q.anchor, index); i <= std::max(q.anchor, index); ++i) q.sel[i] = 1;
    } else if (mod & SDL_KMOD_CTRL) {
        q.sel[index] = !q.sel[index];
        q.anchor = index;
    } else if (q.sel[index]) {
        q.deferSelect = index;  // a drag takes the whole selection; a click only this one
        q.anchor = index;
    } else {
        SelectQueued(index);
    }
    q.focus = index;
    q.press = true;
    q.pressX = x;
    q.pressY = y;
    SDL_CaptureMouse(true);
    return true;
}

bool QueueMouseMove(float x, float y) {
    if (!q.press) return false;
    if (!q.dragging && (std::fabs(x - q.pressX) >= S(4) || std::fabs(y - q.pressY) >= S(4))) q.dragging = !Selected().empty();
    if (q.dragging) {
        if (y < L.upNext.y + HeaderH() + S(18)) q.scroll -= S(10);
        else if (y > L.upNext.B() - S(18)) q.scroll += S(10);
        ClampScroll(Rows());
        auto picked = Selected();
        bool allPicks = !picked.empty() && (size_t)picked.front() >= QueuedByYou();
        q.dropSlot = SlotAt(y, !allPicks);
    }
    g.redraw = true;
    return true;
}

bool QueueMouseUp() {
    if (!q.press) return false;
    bool dragging = q.dragging;
    int slot = q.dropSlot, defer = q.deferSelect;
    q.press = q.dragging = false;
    q.dropSlot = q.deferSelect = -1;
    SDL_CaptureMouse(false);
    if (dragging && slot >= 0) MoveSelection(slot);
    else if (!dragging && defer >= 0) SelectQueued(defer);
    g.redraw = true;
    return true;
}

bool QueueWheel(float x, float y, float dy) {
    if (L.upNext.w <= 0 || !L.upNext.In(x, y)) return false;
    q.scroll -= dy * S(44) * 3;
    ClampScroll(Rows());
    g.redraw = true;
    return true;
}

// A track drag from the list over the panel: where the tracks would land, or -1 off it.
int QueueDropSlot(float x, float y) {
    q.dropSlot = L.upNext.w > 0 && L.upNext.In(x, y) ? SlotAt(y, true) : -1;
    return q.dropSlot;
}

void QueueDropEnd() { q.dropSlot = -1; }

// Arrow keys move the focus (Shift extends), Alt or Ctrl with them move the selected tracks, Home
// and End jump, Delete removes, Enter plays, Ctrl+A selects the whole queue, Escape cancels a drag
// and then leaves the panel.
bool QueueKey(const SDL_KeyboardEvent& e) {
    if (!QueueFocused()) return false;
    Sync();
    bool ctrl = e.mod & SDL_KMOD_CTRL, shift = e.mod & SDL_KMOD_SHIFT, alt = e.mod & SDL_KMOD_ALT;
    int n = (int)g.queue.size();
    g.redraw = true;
    if (e.key == SDLK_ESCAPE) {
        if (q.press) {
            q.press = q.dragging = false;
            q.dropSlot = -1;
            SDL_CaptureMouse(false);
        } else {
            q.focused = false;
        }
        return true;
    }
    if (n == 0) return false;
    if ((e.key == SDLK_UP || e.key == SDLK_DOWN) && (alt || ctrl)) {
        auto picked = Selected();
        if (picked.empty()) return true;
        if (e.key == SDLK_UP && picked.front() > 0) MoveSelection(picked.front() - 1);
        else if (e.key == SDLK_DOWN && picked.back() < n - 1) MoveSelection(picked.front() + 2);
        return true;
    }
    int to = -1;
    if (e.key == SDLK_UP) to = std::max(0, q.focus - 1);
    else if (e.key == SDLK_DOWN) to = std::min(n - 1, q.focus + 1);
    else if (e.key == SDLK_HOME) to = 0;
    else if (e.key == SDLK_END) to = n - 1;
    if (to >= 0) {
        if (shift && q.anchor >= 0) {
            q.sel.assign(n, 0);
            for (int i = std::min(q.anchor, to); i <= std::max(q.anchor, to); ++i) q.sel[i] = 1;
            q.focus = to;
        } else {
            SelectQueued(to);
        }
        ShowQueued(to);
        return true;
    }
    if (e.key == SDLK_DELETE || e.key == SDLK_BACKSPACE) {
        RemoveSelection();
        return true;
    }
    if ((e.key == SDLK_RETURN || e.key == SDLK_KP_ENTER) && !ctrl && q.focus >= 0) {
        PlayQueuedAt(q.focus);
        SelectQueued(-1);
        return true;
    }
    if (e.key == SDLK_A && ctrl) {
        q.sel.assign(n, 1);
        return true;
    }
    return false;
}

// The right-click menu: for queued tracks the moves, Keep for picks and Remove; for what Now Playing
// plays then, Play next and Add to queue; and Clear and New picks for the whole queue.
void OpenQueueMenu(float x, float y, TrackId then) {
    g_barOpen = -1;
    auto picked = Selected();
    std::vector<MenuItem> items;
    auto edit = [](std::string label, int arg, std::string shortcut = {}, bool enabled = true) {
        MenuItem item = Item(std::move(label), kQueueEdit, std::move(shortcut), false, enabled);
        item.arg = arg;
        return item;
    };
    if (!picked.empty()) {
        bool anyPick = std::any_of(picked.begin(), picked.end(), [](int i) { return (size_t)i >= QueuedByYou(); });
        items.push_back(edit("Play now", kQueuePlayNow, "Enter", picked.size() == 1));
        items.push_back(Separator());
        items.push_back(edit("Move to the top", kQueueTop));
        items.push_back(edit("Move to the bottom", kQueueBottom));
        if (anyPick) items.push_back(edit("Keep (play before the picks)", kQueueKeep));
        items.push_back(edit("Remove from the queue", kQueueRemove, "Del"));
        items.push_back(Separator());
    } else if (then != kNone) {
        items.push_back(edit("Play next", kQueueThenNext));
        items.push_back(edit("Add to queue", kQueueThenAdd));
        items.push_back(Separator());
        g_queueMenuTrack = then;
    }
    if (g.queuePicks && TastePicking()) items.push_back(edit("New picks", kQueueRepick));
    items.push_back(edit("Clear the queue", kQueueClear, "", !g.queue.empty()));
    OpenPopup(x, y, std::move(items));
}

void QueueEdit(int arg) {
    auto picked = Selected();
    switch (arg) {
        case kQueuePlayNow:
            if (picked.size() == 1) PlayQueuedAt(picked.front());
            SelectQueued(-1);
            break;
        case kQueueTop: MoveSelection(0); break;
        case kQueueBottom: MoveSelection((int)g.queue.size()); break;
        case kQueueKeep: MoveSelection((int)QueuedByYou(), true); break;
        case kQueueRemove: RemoveSelection(); break;
        case kQueueThenNext: if (g_lib.Contains(g_queueMenuTrack)) QueueTracks({g_queueMenuTrack}, true); break;
        case kQueueThenAdd: if (g_lib.Contains(g_queueMenuTrack)) QueueTracks({g_queueMenuTrack}, false); break;
        case kQueueRepick: RepickTaste(); SelectQueued(-1); break;
        case kQueueClear: ClearQueue(); SelectQueued(-1); break;
    }
    g.redraw = true;
}

bool QueueShown() { return L.upNext.w > 0 && L.upNext.h > 0; }

// View > Up next: the side panel, or Big mode's Up next panel; opening it puts the keyboard there.
void ToggleQueue() {
    if (g.settings.size == ViewSize::Spotlight) {
        q.focused = true;
        return;
    }
    bool show = !QueueShown();
    if (g.settings.size == ViewSize::Big) {
        ToggleBigPanel(2);
    } else {
        if (g.settings.size == ViewSize::Compact) SetViewSize(ViewSize::Normal);
        g.settings.layout.upNext = show;
        SaveSettings();
    }
    q.focused = show;
    if (show && q.focus < 0 && !g.queue.empty()) SelectQueued(0);
    g.redraw = true;
}

}  // namespace lx
