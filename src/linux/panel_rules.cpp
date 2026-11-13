// The auto-playlist editor on Linux (Playlist > New auto-playlist, Edit auto-playlist rules), as the Windows
// build's app_autoplaylist.cpp: a name, ALL or ANY of the rules, the limit and pick order, and a live count
// of the tracks they select. The rules of nested groups are kept as they are; this editor changes the main
// group only.
#include "panels_internal.h"

#include "autoplaylist.h"

namespace lx::panel {
namespace {

struct RuleRow {
    Rule rule;
    TextField value, value2;
};

struct RulesPanel {
    int playlist = -1;   // -1: a new playlist
    AutoPlaylist model;  // what the editor keeps besides the rows: nested groups, pick, order
    std::vector<RuleRow> rows;
    TextField name, limit;
    bool limitOn = false;
    int first = 0;   // the first row shown
    int focus = 0;   // 0 name, 1 limit, 2 + 2 * row the value, 3 + 2 * row the second value; -1 none
    int count = -1;  // tracks the rules select; -1 until counted
    bool countDirty = true;
    std::string warning;
} g_rules;

constexpr float kRulesW = 640, kRulesH = 560;

struct RulesGeometry {
    Rect panel, name, match, add, limitBox, limitField, limitUnit, pick, ordered, count, cancel, save;
    std::vector<Rect> remove, field, op, value, value2;  // one each per visible row
    int visible = 0;
};


RulesGeometry Geometry() {
    RulesGeometry G;
    Rect out = OutputRect();
    float w = std::min(S(kRulesW), out.w - S(16)), h = std::min(S(kRulesH), out.h - S(16));
    Rect p{std::round((out.w - w) / 2), std::round((out.h - h) / 2), w, h};
    G.panel = p;
    const float rh = RowH(), px = p.x, pw = p.w, top = p.y + S(48), rowsTop = top + 2 * rh;
    const float footerY = p.B() - S(46), rowsBottom = footerY - S(10) - 3 * rh;
    G.name = {px + S(90), top, pw - S(106), rh - S(4)};
    G.match = {px + S(90), top + rh, S(110), rh - S(6)};
    G.visible = std::max(1, (int)std::floor((rowsBottom - rowsTop) / rh));
    for (int k = 0; k < G.visible; ++k) {
        const float y = rowsTop + k * rh, rowH = rh - S(6);
        const int i = g_rules.first + k;
        const bool between = i < (int)g_rules.rows.size() && g_rules.rows[i].rule.op == RuleOp::Between;
        const float vx = px + S(370), vw = pw - S(370) - S(16) - (between ? S(96) : 0);
        G.remove.push_back({px + S(16), y, S(22), rowH});
        G.field.push_back({px + S(44), y, S(170), rowH});
        G.op.push_back({px + S(222), y, S(140), rowH});
        G.value.push_back({vx, y, vw, rowH});
        G.value2.push_back(between ? Rect{vx + vw + S(6), y, S(90), rowH} : Rect{});
    }
    const float ya = rowsTop + G.visible * rh + S(4);
    G.add = {px + S(16), ya, S(120), rh - S(6)};
    G.limitBox = {px + S(16), ya + rh, S(120), rh - S(6)};
    G.limitField = {px + S(150), ya + rh, S(90), rh - S(4)};
    G.limitUnit = {px + S(250), ya + rh, S(140), rh - S(6)};
    G.pick = {px + S(90), ya + 2 * rh, S(180), rh - S(6)};
    G.ordered = {px + S(290), ya + 2 * rh, S(230), rh - S(6)};
    G.count = {px + S(16), footerY + S(10), pw - S(230), rh};
    G.cancel = {px + pw - S(96), footerY, S(80), rh};
    G.save = {px + pw - S(186), footerY, S(84), rh};
    return G;
}


// Operators that need no value, and those whose value is a number.
bool NoValue(RuleOp op) { return op == RuleOp::IsEmpty || op == RuleOp::NotEmpty || op == RuleOp::Never; }
bool NeedsNumber(const Rule& r) {
    switch (r.op) {
        case RuleOp::Equals:
        case RuleOp::NotEquals: return !IsTextField(r.field);
        case RuleOp::AtLeast:
        case RuleOp::AtMost:
        case RuleOp::Between:
        case RuleOp::InLastDays:
        case RuleOp::NotInLastDays: return true;
        default: return false;
    }
}
bool IsNumber(const std::wstring& s) {
    if (s.empty()) return false;
    size_t digits = 0, dots = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] >= L'0' && s[i] <= L'9') ++digits;
        else if (s[i] == L'.') ++dots;
        else if (!(s[i] == L'-' && i == 0)) return false;
    }
    return digits > 0 && dots <= 1;
}

// The text field of a focus index (see RulesPanel::focus), or null when it is not shown.
TextField* RulesField(int f) {
    if (f == 0) return &g_rules.name;
    if (f == 1) return g_rules.limitOn ? &g_rules.limit : nullptr;
    if (f < 2) return nullptr;
    const int i = (f - 2) / 2;
    if (i >= (int)g_rules.rows.size()) return nullptr;
    RuleRow& row = g_rules.rows[i];
    if ((f - 2) % 2 == 0) return NoValue(row.rule.op) ? nullptr : &row.value;
    return row.rule.op == RuleOp::Between ? &row.value2 : nullptr;
}

bool RulesNumeric(int f) {
    if (f == 1) return true;
    const int i = (f - 2) / 2;
    return f >= 2 && i < (int)g_rules.rows.size() && NeedsNumber(g_rules.rows[i].rule);
}

void RulesFocus(int f) {
    TextField* t = RulesField(f);
    g_rules.focus = t ? f : -1;
    if (t) {
        t->caret = t->text.size();
        t->all = false;
        SDL_StartTextInput(g.window);
    }
    g.redraw = true;
}

// The next shown field after `f` in tab order (wrapping), or -1.
int RulesStep(int f, int dir) {
    const int slots = 2 + 2 * (int)g_rules.rows.size();
    for (int n = 1; n <= slots; ++n) {
        const int next = ((f + dir * n) % slots + slots) % slots;
        if (RulesField(next)) return next;
    }
    return -1;
}

void RulesEdited() {
    g_rules.countDirty = true;
    g_rules.warning.clear();
    g.redraw = true;
}

// The rules as they would be saved, without checks; the count and the save both come from here.
AutoPlaylist RulesModel() {
    AutoPlaylist a = g_rules.model;
    a.root.rules.clear();
    for (const RuleRow& row : g_rules.rows) {
        Rule r = row.rule;
        r.value = Utf8(Trimmed(row.value.text));
        r.value2 = row.rule.op == RuleOp::Between ? Utf8(Trimmed(row.value2.text)) : std::string();
        if (NoValue(r.op)) r.value.clear();
        a.root.rules.push_back(r);
    }
    a.limit = g_rules.limitOn ? (uint32_t)std::max(0L, std::wcstol(Trimmed(g_rules.limit.text).c_str(), nullptr, 10)) : 0;
    return a;
}

void CountRules() {
    std::vector<TrackId> tracks;
    EvaluateAutoPlaylist(RulesModel(), tracks);
    g_rules.count = (int)tracks.size();
    g_rules.countDirty = false;
}

void RulesSave() {
    const std::wstring name = Trimmed(g_rules.name.text);
    auto fail = [](const std::string& text) {
        g_rules.warning = text;
        g.redraw = true;
    };
    if (name.empty()) return fail("Give the playlist a name.");
    if (g_rules.rows.empty() && g_rules.model.root.groups.empty()) return fail("Add a rule first.");
    for (size_t i = 0; i < g_rules.rows.size(); ++i) {
        const RuleRow& row = g_rules.rows[i];
        const std::string number = "Rule " + std::to_string(i + 1) + " needs a number.";
        if (!NoValue(row.rule.op) && Trimmed(row.value.text).empty()) return fail("Rule " + std::to_string(i + 1) + " needs a value.");
        if (NeedsNumber(row.rule) && !IsNumber(Trimmed(row.value.text))) return fail(number);
        if (row.rule.op == RuleOp::Between && !IsNumber(Trimmed(row.value2.text))) return fail(number);
        uint32_t day;
        if ((row.rule.op == RuleOp::Before || row.rule.op == RuleOp::OnOrAfter) &&
            !ParseRuleDate(Utf8(Trimmed(row.value.text)), day))
            return fail("Rule " + std::to_string(i + 1) + " needs a date such as 2026-01-31.");
    }
    if (g_rules.limitOn && !IsNumber(Trimmed(g_rules.limit.text))) return fail("Enter the limit as a number.");
    const std::string text = SerializeAutoPlaylist(RulesModel());
    std::wstring unique = name;
    const int self = g_rules.playlist;
    bool taken = false;
    for (size_t k = 0; k < g.playlists.size(); ++k)
        taken = taken || ((int)k != self && CompareStringOrdinal(g.playlists[k].name.c_str(), -1, name.c_str(), -1, TRUE) == CSTR_EQUAL);
    if (taken) unique = UniquePlaylistName(g.playlists, name);
    if (self < 0) {
        g.playlists.push_back(Playlist{unique, {}, PlaylistKind::Auto, text});
        PlaySfx(Sfx::Sparkle);
        SetActiveTab((int)g.playlists.size() - 1);
    } else if (self < (int)g.playlists.size()) {
        g.playlists[self].name = unique;
        g.playlists[self].rules = text;
    }
    g.libraryDirty = true;
    RebuildRows();
    ClosePanel();
}

std::vector<std::string> FieldLabels() {
    std::vector<std::string> labels;
    for (int f = 0; f < (int)RuleField::Count; ++f) labels.push_back(Utf8(RuleFieldName((RuleField)f)));
    return labels;
}

void RulesMouseDown(float x, float y) {
    const RulesGeometry G = Geometry();
    if (G.save.In(x, y)) return RulesSave();
    if (G.cancel.In(x, y)) return ClosePanel();
    if (G.name.In(x, y)) return RulesFocus(0);
    if (G.match.In(x, y)) {
        g_rules.model.root.all = !g_rules.model.root.all;
        return RulesEdited();
    }
    if (G.add.In(x, y)) {
        g_rules.rows.push_back(RuleRow{});
        g_rules.first = std::max(0, (int)g_rules.rows.size() - G.visible);
        RulesEdited();
        return RulesFocus(2 + 2 * ((int)g_rules.rows.size() - 1));
    }
    if (G.limitBox.In(x, y)) {
        g_rules.limitOn = !g_rules.limitOn;
        RulesEdited();
        if (g_rules.limitOn) RulesFocus(1);
        return;
    }
    if (G.limitField.In(x, y)) return g_rules.limitOn ? RulesFocus(1) : void();
    if (G.limitUnit.In(x, y)) {
        std::vector<std::string> labels;
        for (int u = 0; u < (int)LimitUnit::Count; ++u) labels.push_back(Utf8(LimitUnitName((LimitUnit)u)));
        OpenChoice(G.limitUnit.x, G.limitUnit.B(), labels, (int)g_rules.model.unit, [](int u) {
            g_rules.model.unit = (LimitUnit)u;
            RulesEdited();
        });
        return;
    }
    if (G.pick.In(x, y)) {
        std::vector<std::string> labels;
        for (int k = 0; k < (int)PickBy::Count; ++k) labels.push_back(Utf8(PickByName((PickBy)k)));
        OpenChoice(G.pick.x, G.pick.B(), labels, (int)g_rules.model.pick, [](int k) {
            g_rules.model.pick = (PickBy)k;
            RulesEdited();
        });
        return;
    }
    if (G.ordered.In(x, y)) {
        g_rules.model.ordered = !g_rules.model.ordered;
        return RulesEdited();
    }
    for (int k = 0; k < G.visible; ++k) {
        const int i = g_rules.first + k;
        if (i >= (int)g_rules.rows.size()) break;
        RuleRow& row = g_rules.rows[i];
        if (G.remove[k].In(x, y)) {
            g_rules.rows.erase(g_rules.rows.begin() + i);
            g_rules.first = std::clamp(g_rules.first, 0, std::max(0, (int)g_rules.rows.size() - 1));
            g_rules.focus = -1;
            return RulesEdited();
        }
        if (G.field[k].In(x, y)) {
            OpenChoice(G.field[k].x, G.field[k].B(), FieldLabels(), (int)row.rule.field, [i](int f) {
                RuleRow& r = g_rules.rows[i];
                r.rule.field = (RuleField)f;
                const std::vector<RuleOp> ops = OpsFor(r.rule.field);
                if (std::find(ops.begin(), ops.end(), r.rule.op) == ops.end()) r.rule.op = ops.front();
                RulesEdited();
            });
            return;
        }
        if (G.op[k].In(x, y)) {
            const std::vector<RuleOp> ops = OpsFor(row.rule.field);
            std::vector<std::string> labels;
            for (RuleOp op : ops) labels.push_back(Utf8(RuleOpName(op)));
            const int current = (int)(std::find(ops.begin(), ops.end(), row.rule.op) - ops.begin());
            OpenChoice(G.op[k].x, G.op[k].B(), labels, current, [i, ops](int o) {
                g_rules.rows[i].rule.op = ops[o];
                RulesEdited();
            });
            return;
        }
        if (G.value[k].In(x, y) && !NoValue(row.rule.op)) return RulesFocus(2 + 2 * i);
        if (G.value2[k].w > 0 && G.value2[k].In(x, y)) return RulesFocus(3 + 2 * i);
    }
}

void RulesKey(const SDL_KeyboardEvent& e) {
    if (e.key == SDLK_ESCAPE) return ClosePanel();
    if (e.key == SDLK_RETURN || e.key == SDLK_KP_ENTER) return RulesSave();
    if (e.key == SDLK_TAB) return RulesFocus(RulesStep(g_rules.focus, (e.mod & SDL_KMOD_SHIFT) ? -1 : 1));
    if (TextField* t = RulesField(g_rules.focus); t && LineFieldKey(*t, e) == 1) RulesEdited();
}

void RulesText(const std::wstring& s) {
    TextField* t = RulesField(g_rules.focus);
    if (!t) return;
    const bool numeric = RulesNumeric(g_rules.focus);
    std::wstring in;
    for (wchar_t c : s)
        if (c >= 32 && (!numeric || (c >= L'0' && c <= L'9') || c == L'.' || c == L'-')) in += c;
    if (LineFieldInsert(*t, in)) RulesEdited();
}


void DrawRules(const Palette& p) {
    if (g_rules.countDirty) CountRules();
    const RulesGeometry G = Geometry();
    DrawFrame(p, S(kRulesW), S(kRulesH), g_rules.playlist < 0 ? "New auto-playlist" : "Edit auto-playlist rules");
    DrawText(Font::Normal, "Name", G.panel.x + S(16), TextY(G.name), p.sub);
    DrawLineField(g_rules.name, G.name, g_rules.focus == 0, true, "Playlist name", p);
    DrawText(Font::Normal, "Match", G.panel.x + S(16), TextY(G.match), p.sub);
    DrawButton(G.match, g_rules.model.root.all ? "All rules" : "Any rule", p);
    DrawText(Font::Normal, "of the following rules:", G.match.R() + S(12), TextY(G.match), p.sub);
    if (g_rules.rows.empty()) DrawText(Font::Normal, "No rules yet. Add one below.", G.panel.x + S(44), G.match.B() + S(10), p.faint);
    for (int k = 0; k < G.visible; ++k) {
        const int i = g_rules.first + k;
        if (i >= (int)g_rules.rows.size()) break;
        RuleRow& row = g_rules.rows[i];
        DrawButton(G.remove[k], "\xC3\x97", p);
        DrawButton(G.field[k], Utf8(RuleFieldName(row.rule.field)), p);
        DrawButton(G.op[k], Utf8(RuleOpName(row.rule.op)), p);
        DrawLineField(row.value, G.value[k], g_rules.focus == 2 + 2 * i, !NoValue(row.rule.op), "value", p);
        if (row.rule.op == RuleOp::Between) DrawLineField(row.value2, G.value2[k], g_rules.focus == 3 + 2 * i, true, "and", p);
    }
    DrawButton(G.add, "Add rule", p);
    DrawCheck(G.limitBox, "Limit to", g_rules.limitOn, p);
    DrawLineField(g_rules.limit, G.limitField, g_rules.focus == 1, g_rules.limitOn, nullptr, p);
    DrawButton(G.limitUnit, Utf8(LimitUnitName(g_rules.model.unit)), p, false, g_rules.limitOn);
    DrawText(Font::Normal, "Pick", G.panel.x + S(16), TextY(G.pick), p.sub);
    DrawButton(G.pick, Utf8(PickByName(g_rules.model.pick)), p);
    DrawCheck(G.ordered, "Keep the pick order", g_rules.model.ordered, p);
    std::string status = g_rules.warning;
    if (status.empty() && g_rules.count >= 0)
        status = std::to_string(g_rules.count) + (g_rules.count == 1 ? " track matches" : " tracks match");
    DrawText(Font::Normal, status, G.count.x, TextY(G.count), g_rules.warning.empty() ? p.sub : p.accent, G.count.w);
    DrawButton(G.cancel, "Cancel", p);
    DrawButton(G.save, "Save", p, true);
}

void RulesWheel(float, float, float dy) {
    const int rows = (int)g_rules.rows.size(), visible = Geometry().visible;
    g_rules.first = std::clamp(g_rules.first - (int)std::lround(dy), 0, std::max(0, rows - visible));
}

bool RulesWantsText(float x, float y) {
    const RulesGeometry G = Geometry();
    if (G.name.In(x, y) || G.limitField.In(x, y)) return true;
    for (size_t k = 0; k < G.value.size(); ++k)
        if (G.value[k].In(x, y) || G.value2[k].In(x, y)) return true;
    return false;
}

}  // namespace

const Ops kRulesOps{
    .draw = DrawRules,
    .mouseDown = [](float x, float y, int) { RulesMouseDown(x, y); },
    .wheel = RulesWheel,
    .key = RulesKey,
    .text = [](const char* s) { RulesText(compat::Widen(s)); },
    .wantsText = RulesWantsText,
    .typing = true,
};

}  // namespace lx::panel

namespace lx {
using namespace panel;

// Playlist > New auto-playlist (playlist -1) or Edit auto-playlist rules (an auto-playlist's index).
void OpenRules(int playlist) {
    if (playlist >= (int)g.playlists.size()) return;
    g_rules = RulesPanel{};
    g_rules.playlist = playlist;
    std::wstring name = L"Auto-playlist";
    AutoPlaylist model;
    if (playlist >= 0) {
        if (!ParseAutoPlaylist(g.playlists[playlist].rules, model)) {
            ShowMessage(L"That playlist's rules can't be read.");
            return;
        }
        name = g.playlists[playlist].name;
    }
    for (const Rule& r : model.root.rules) {
        RuleRow row;
        row.rule = r;
        row.value.text = compat::Widen(r.value.c_str(), r.value.size());
        row.value2.text = compat::Widen(r.value2.c_str(), r.value2.size());
        g_rules.rows.push_back(row);
    }
    if (g_rules.rows.empty() && model.root.groups.empty()) g_rules.rows.push_back(RuleRow{});
    model.root.rules.clear();
    g_rules.model = model;
    g_rules.name.text = name;
    g_rules.limitOn = model.limit > 0;
    if (model.limit) g_rules.limit.text = std::to_wstring(model.limit);
    g_kind = Kind::Rules;
    RulesFocus(0);
    g.redraw = true;
}

}  // namespace lx
