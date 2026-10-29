// The auto-playlist editor: name, rules (ALL/ANY, one level of nested groups), limit and pick
// order, with a live count of matching tracks.
#include "app.h"

namespace app {

namespace {

constexpr int kIdName = 100, kIdMatch = 101, kIdAddRule = 102, kIdAddGroup = 103, kIdLimitOn = 104, kIdLimit = 105,
              kIdLimitUnit = 106, kIdPick = 107, kIdOrdered = 108, kIdRowBase = 1000;  // + row * 8 + part

struct Row {
    int group;  // -1: a rule of the main group; else index into root.groups
    int rule;   // index into the group's rules; -1: the group's header row
    HWND field = nullptr, op = nullptr, value = nullptr, value2 = nullptr, remove = nullptr, add = nullptr, label = nullptr;
};

struct Editor {
    HWND hwnd = nullptr;
    int playlist = -1;  // -1 while creating
    AutoPlaylist model;
    std::vector<Row> rows;
    HWND name = nullptr, nameLabel = nullptr, match = nullptr, matchLabel = nullptr, addRule = nullptr, addGroup = nullptr,
         limitOn = nullptr, limit = nullptr, limitUnit = nullptr, pickLabel = nullptr, pick = nullptr, ordered = nullptr,
         count = nullptr, save = nullptr, cancel = nullptr;
    bool building = false;
} e;

// Small control factory: one id per control, themed for the current theme.
HWND Make(DWORD ex, const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, e.hwnd, (HMENU)(INT_PTR)id, g.inst, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g.font, TRUE);
    if (!wcscmp(cls, WC_COMBOBOXW) || !wcscmp(cls, WC_EDITW)) SetWindowTheme(c, g.dark ? L"DarkMode_CFD" : L"Explorer", nullptr);
    else if (!wcscmp(cls, WC_BUTTONW)) ApplyControlTheme(c, g.dark);
    return c;
}

HWND Combo(int id, const std::vector<std::wstring>& items, int sel) {
    HWND c = Make(0, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, id);
    for (auto& s : items) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)s.c_str());
    SendMessageW(c, CB_SETCURSEL, sel, 0);
    return c;
}

Rule& RuleOf(const Row& r) {
    return r.group < 0 ? e.model.root.rules[r.rule] : e.model.root.groups[r.group].rules[r.rule];
}

void DestroyRows() {
    for (auto& r : e.rows)
        for (HWND h : {r.field, r.op, r.value, r.value2, r.remove, r.add, r.label})
            if (h) DestroyWindow(h);
    e.rows.clear();
}

void UpdateCount();
void LayoutEditor();

// One control set per rule (and a header per nested group), recreated when the structure changes.
void BuildRows() {
    e.building = true;
    DestroyRows();
    std::vector<std::wstring> fields;
    for (int f = 0; f < (int)RuleField::Count; ++f) fields.push_back(RuleFieldName((RuleField)f));
    auto addRule = [&](int group, int index) {
        Row row{group, index};
        int id = kIdRowBase + (int)e.rows.size() * 8;
        const Rule& rule = group < 0 ? e.model.root.rules[index] : e.model.root.groups[group].rules[index];
        row.field = Combo(id, fields, (int)rule.field);
        std::vector<std::wstring> ops;
        auto valid = OpsFor(rule.field);
        int sel = 0;
        for (size_t i = 0; i < valid.size(); ++i) {
            ops.push_back(RuleOpName(valid[i]));
            if (valid[i] == rule.op) sel = (int)i;
        }
        row.op = Combo(id + 1, ops, sel);
        row.value = Make(WS_EX_CLIENTEDGE, WC_EDITW, FromUtf8(rule.value).c_str(), ES_AUTOHSCROLL | WS_TABSTOP, id + 2);
        row.value2 = Make(WS_EX_CLIENTEDGE, WC_EDITW, FromUtf8(rule.value2).c_str(), ES_AUTOHSCROLL | WS_TABSTOP, id + 3);
        row.remove = Make(0, WC_BUTTONW, L"\x2212", BS_PUSHBUTTON | WS_TABSTOP, id + 4);
        e.rows.push_back(row);
    };
    for (int i = 0; i < (int)e.model.root.rules.size(); ++i) addRule(-1, i);
    for (int gi = 0; gi < (int)e.model.root.groups.size(); ++gi) {
        Row header{gi, -1};
        int id = kIdRowBase + (int)e.rows.size() * 8;
        header.field = Combo(id, {L"All", L"Any"}, e.model.root.groups[gi].all ? 0 : 1);
        header.label = Make(0, WC_STATICW, L"of these:", SS_LEFT | SS_NOPREFIX, id + 5);
        header.add = Make(0, WC_BUTTONW, L"+ Rule", BS_PUSHBUTTON | WS_TABSTOP, id + 6);
        header.remove = Make(0, WC_BUTTONW, L"\x2212", BS_PUSHBUTTON | WS_TABSTOP, id + 4);
        e.rows.push_back(header);
        for (int i = 0; i < (int)e.model.root.groups[gi].rules.size(); ++i) addRule(gi, i);
    }
    e.building = false;
    LayoutEditor();
    UpdateCount();
}

// Reads every row back into the model (operators depend on the field, so they are mapped).
void SyncModel() {
    if (e.building) return;
    for (auto& r : e.rows) {
        if (r.rule < 0) {
            e.model.root.groups[r.group].all = SendMessageW(r.field, CB_GETCURSEL, 0, 0) == 0;
            continue;
        }
        Rule& rule = RuleOf(r);
        rule.field = (RuleField)SendMessageW(r.field, CB_GETCURSEL, 0, 0);
        auto ops = OpsFor(rule.field);
        int op = (int)SendMessageW(r.op, CB_GETCURSEL, 0, 0);
        rule.op = ops[std::clamp(op, 0, (int)ops.size() - 1)];
        rule.value = ToUtf8(WindowText(r.value));
        rule.value2 = ToUtf8(WindowText(r.value2));
    }
    e.model.root.all = SendMessageW(e.match, CB_GETCURSEL, 0, 0) == 0;
    bool limited = Button_GetCheck(e.limitOn) == BST_CHECKED;
    e.model.limit = limited ? (uint32_t)std::max(1, _wtoi(WindowText(e.limit).c_str())) : 0;
    e.model.unit = (LimitUnit)std::clamp((int)SendMessageW(e.limitUnit, CB_GETCURSEL, 0, 0), 0, (int)LimitUnit::Count - 1);
    e.model.pick = (PickBy)SendMessageW(e.pick, CB_GETCURSEL, 0, 0);
    e.model.ordered = Button_GetCheck(e.ordered) == BST_CHECKED;
}

void UpdateCount() {
    SyncModel();
    std::vector<TrackId> items;
    EvaluateAutoPlaylist(e.model, items);
    double seconds = 0;
    for (TrackId id : items) seconds += g_lib[id].Duration();
    std::wstring text = L"Matches " + std::to_wstring(items.size()) + (items.size() == 1 ? L" track" : L" tracks") +
                        L"  \x2022  " + FormatTotalDuration(seconds);
    SetWindowTextW(e.count, text.c_str());
    for (auto& r : e.rows) {
        if (r.rule < 0) continue;
        RuleOp op = RuleOf(r).op;
        bool none = op == RuleOp::IsEmpty || op == RuleOp::NotEmpty || op == RuleOp::Never;
        ShowWindow(r.value, none ? SW_HIDE : SW_SHOW);
        ShowWindow(r.value2, op == RuleOp::Between ? SW_SHOW : SW_HIDE);
    }
    bool limited = Button_GetCheck(e.limitOn) == BST_CHECKED;
    for (HWND h : {e.limit, e.limitUnit, e.pick, e.ordered}) EnableWindow(h, limited);
}

void LayoutEditor() {
    if (!e.hwnd) return;
    RECT rc;
    GetClientRect(e.hwnd, &rc);
    int pad = S(14), rowH = S(30), W = rc.right - pad * 2;
    int y = pad;
    MoveWindow(e.nameLabel, pad, y + S(4), S(50), S(20), TRUE);
    MoveWindow(e.name, pad + S(54), y, W - S(54), S(24), TRUE);
    y += rowH + S(6);
    MoveWindow(e.matchLabel, pad, y + S(4), S(44), S(20), TRUE);
    MoveWindow(e.match, pad + S(46), y, S(70), S(300), TRUE);
    SetWindowPos(e.matchLabel, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
    y += rowH;
    for (auto& r : e.rows) {
        int x = pad + (r.group >= 0 ? S(28) : 0);
        int w = rc.right - pad - x;
        if (r.rule < 0) {  // group header
            MoveWindow(r.field, x - S(28), y, S(70), S(300), TRUE);
            MoveWindow(r.label, x - S(28) + S(76), y + S(4), S(70), S(20), TRUE);
            MoveWindow(r.add, rc.right - pad - S(30) - S(6) - S(70), y, S(70), S(24), TRUE);
            MoveWindow(r.remove, rc.right - pad - S(30), y, S(30), S(24), TRUE);
        } else {
            int fieldW = S(150), opW = S(170), removeW = S(30), gap = S(6);
            int valueW = w - fieldW - opW - removeW - gap * 3;
            bool between = RuleOf(r).op == RuleOp::Between;
            MoveWindow(r.field, x, y, fieldW, S(400), TRUE);
            MoveWindow(r.op, x + fieldW + gap, y, opW, S(400), TRUE);
            int vx = x + fieldW + opW + gap * 2;
            MoveWindow(r.value, vx, y, between ? valueW / 2 - S(3) : valueW, S(24), TRUE);
            MoveWindow(r.value2, vx + valueW / 2 + S(3), y, valueW / 2 - S(3), S(24), TRUE);
            MoveWindow(r.remove, rc.right - pad - removeW, y, removeW, S(24), TRUE);
        }
        y += rowH;
    }
    MoveWindow(e.addRule, pad, y + S(2), S(90), S(26), TRUE);
    MoveWindow(e.addGroup, pad + S(96), y + S(2), S(100), S(26), TRUE);
    y += rowH + S(14);
    MoveWindow(e.limitOn, pad, y + S(2), S(70), S(22), TRUE);
    MoveWindow(e.limit, pad + S(72), y, S(56), S(24), TRUE);
    MoveWindow(e.limitUnit, pad + S(134), y, S(90), S(300), TRUE);
    MoveWindow(e.pickLabel, pad + S(232), y + S(4), S(68), S(20), TRUE);
    MoveWindow(e.pick, pad + S(302), y, S(170), S(300), TRUE);
    y += rowH;
    MoveWindow(e.ordered, pad + S(72), y, S(320), S(22), TRUE);
    MoveWindow(e.count, pad, rc.bottom - pad - S(24), W - S(220), S(22), TRUE);
    MoveWindow(e.save, rc.right - pad - S(208), rc.bottom - pad - S(28), S(100), S(28), TRUE);
    MoveWindow(e.cancel, rc.right - pad - S(100), rc.bottom - pad - S(28), S(100), S(28), TRUE);
    // Grow the window to fit the rules (never shrinking it below what the user chose).
    int needed = y + rowH + S(50) + pad;
    if (needed > rc.bottom) {
        RECT wr;
        GetWindowRect(e.hwnd, &wr);
        SetWindowPos(e.hwnd, nullptr, 0, 0, wr.right - wr.left, wr.bottom - wr.top + needed - rc.bottom,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    InvalidateRect(e.hwnd, nullptr, TRUE);
}

// Writes the edited controls back into the playlist: renames it if the name is taken, then saves,
// refreshes the tabs and activates it.
void Save() {
    SyncModel();
    std::wstring name = WindowText(e.name);
    size_t b = name.find_first_not_of(L" \t"), end = name.find_last_not_of(L" \t");
    name = b == std::wstring::npos ? L"Auto-playlist" : name.substr(b, end - b + 1);
    std::string rules = SerializeAutoPlaylist(e.model);
    int index = e.playlist;
    if (index >= 0 && index < (int)g.playlists.size() && g.playlists[index].kind == PlaylistKind::Auto) {
        Playlist& pl = g.playlists[index];
        if (pl.name != name) pl.name = UniquePlaylistName(name);
        pl.rules = rules;
        pl.builtRevision = 0;
    } else {
        Playlist pl{UniquePlaylistName(name), {}, PlaylistKind::Auto, rules};
        g.playlists.push_back(std::move(pl));
        index = (int)g.playlists.size() - 1;
        PlaySfx(Sfx::Sparkle);
    }
    g.dirty = true;
    RefreshTabs();
    ActivatePlaylist(index);
    DestroyWindow(e.hwnd);
}

constexpr UINT WM_REBUILD = WM_APP + 30;

// Rows are rebuilt after the notification that changed the model has returned (the control that
// sent it is among those destroyed). Until then the model is ahead of the controls: no syncing.
void RebuildLater() {
    e.building = true;
    PostMessageW(e.hwnd, WM_REBUILD, 0, 0);
}

LRESULT CALLBACK AutoProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_REBUILD:
            BuildRows();
            return 0;
        case WM_CREATE: {
            e.hwnd = h;
            e.nameLabel = Make(0, WC_STATICW, L"Name", SS_LEFT, 0);
            std::wstring name = e.playlist >= 0 ? g.playlists[e.playlist].name : L"New auto-playlist";
            e.name = Make(WS_EX_CLIENTEDGE, WC_EDITW, name.c_str(), ES_AUTOHSCROLL | WS_TABSTOP, kIdName);
            e.matchLabel = Make(0, WC_STATICW, L"Match", SS_LEFT, 0);
            e.match = Combo(kIdMatch, {L"all", L"any"}, e.model.root.all ? 0 : 1);
            e.addRule = Make(0, WC_BUTTONW, L"+ &Rule", BS_PUSHBUTTON | WS_TABSTOP, kIdAddRule);
            e.addGroup = Make(0, WC_BUTTONW, L"+ &Group", BS_PUSHBUTTON | WS_TABSTOP, kIdAddGroup);
            e.limitOn = Make(0, WC_BUTTONW, L"&Limit to", BS_AUTOCHECKBOX | WS_TABSTOP, kIdLimitOn);
            Button_SetCheck(e.limitOn, e.model.limit ? BST_CHECKED : BST_UNCHECKED);
            e.limit = Make(WS_EX_CLIENTEDGE, WC_EDITW, std::to_wstring(e.model.limit ? e.model.limit : 25).c_str(),
                           ES_NUMBER | WS_TABSTOP, kIdLimit);
            std::vector<std::wstring> units;
            for (int u = 0; u < (int)LimitUnit::Count; ++u) units.push_back(LimitUnitName((LimitUnit)u));
            e.limitUnit = Combo(kIdLimitUnit, units, (int)e.model.unit);
            e.pickLabel = Make(0, WC_STATICW, L"picked by", SS_LEFT, 0);
            std::vector<std::wstring> picks;
            for (int p = 0; p < (int)PickBy::Count; ++p) picks.push_back(PickByName((PickBy)p));
            e.pick = Combo(kIdPick, picks, (int)e.model.pick);
            e.ordered = Make(0, WC_BUTTONW, L"&Keep that order (instead of library order)", BS_AUTOCHECKBOX | WS_TABSTOP, kIdOrdered);
            Button_SetCheck(e.ordered, e.model.ordered ? BST_CHECKED : BST_UNCHECKED);
            e.count = Make(0, WC_STATICW, L"", SS_LEFT | SS_NOPREFIX, 0);
            e.save = Make(0, WC_BUTTONW, L"&Save", BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK);
            e.cancel = Make(0, WC_BUTTONW, L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL);
            ApplyWindowTheme(h, P());
            BuildRows();
            return 0;
        }
        case WM_SIZE:
            LayoutEditor();
            return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {S(640), S(300)};
            return 0;
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(h, &rc);
            FillRect((HDC)wp, &rc, g.surfaceBrush);
            return 1;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            return SurfaceControlColor((HDC)wp, (HWND)lp == e.count ? SubColor() : TextColor());
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            return FieldControlColor((HDC)wp);
        case WM_COMMAND: {
            int id = LOWORD(wp), code = HIWORD(wp);
            if (e.building) return 0;
            if (id == IDOK) {
                Save();
                return 0;
            }
            if (id == IDCANCEL) {
                DestroyWindow(h);
                return 0;
            }
            if (id == kIdAddRule) {
                SyncModel();
                e.model.root.rules.push_back({RuleField::Artist, RuleOp::Contains, "", ""});
                RebuildLater();
                return 0;
            }
            if (id == kIdAddGroup) {
                SyncModel();
                RuleGroup group;
                group.all = false;
                group.rules.push_back({RuleField::Genre, RuleOp::Is, "", ""});
                e.model.root.groups.push_back(group);
                RebuildLater();
                return 0;
            }
            if (id >= kIdRowBase) {
                int index = (id - kIdRowBase) / 8, part = (id - kIdRowBase) % 8;
                if (index >= (int)e.rows.size()) return 0;
                Row row = e.rows[index];
                if (part == 4 && code == BN_CLICKED) {  // remove
                    SyncModel();
                    if (row.rule < 0) e.model.root.groups.erase(e.model.root.groups.begin() + row.group);
                    else if (row.group < 0) e.model.root.rules.erase(e.model.root.rules.begin() + row.rule);
                    else {
                        auto& rules = e.model.root.groups[row.group].rules;
                        rules.erase(rules.begin() + row.rule);
                        if (rules.empty()) e.model.root.groups.erase(e.model.root.groups.begin() + row.group);
                    }
                    RebuildLater();
                    return 0;
                }
                if (part == 6 && code == BN_CLICKED) {  // + rule in a group
                    SyncModel();
                    e.model.root.groups[row.group].rules.push_back({RuleField::Genre, RuleOp::Is, "", ""});
                    RebuildLater();
                    return 0;
                }
                if (part == 0 && code == CBN_SELCHANGE && row.rule >= 0) {
                    // A new field: its first sensible operator, and rebuild the operator list.
                    SyncModel();
                    Rule& rule = RuleOf(row);
                    auto ops = OpsFor(rule.field);
                    if (std::find(ops.begin(), ops.end(), rule.op) == ops.end()) rule.op = ops[0];
                    RebuildLater();
                    return 0;
                }
                if ((part == 1 && code == CBN_SELCHANGE) || (part == 0 && code == CBN_SELCHANGE)) {
                    UpdateCount();
                    LayoutEditor();
                    return 0;
                }
                if ((part == 2 || part == 3) && code == EN_CHANGE) UpdateCount();
                return 0;
            }
            if (code == EN_CHANGE || code == CBN_SELCHANGE || code == BN_CLICKED) UpdateCount();
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            e.rows.clear();
            e.hwnd = nullptr;
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

bool AutoEditorDialogMessage(MSG* msg) { return e.hwnd && IsDialogMessageW(e.hwnd, msg); }

// Opens the editor for the playlist at index, or for a new one when index < 0.
void ShowAutoPlaylistEditor(int playlist) {
    if (e.hwnd) DestroyWindow(e.hwnd);
    RegisterAppClass(kAutoClass, AutoProc);
    e.playlist = playlist;
    e.model = AutoPlaylist{};
    if (playlist >= 0 && !ParseAutoPlaylist(g.playlists[playlist].rules, e.model)) e.model = AutoPlaylist{};
    if (playlist < 0) e.model.root.rules.push_back({RuleField::Artist, RuleOp::Contains, "", ""});
    RECT r{0, 0, S(720), S(330)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_DLGMODALFRAME, g.dpi);
    RECT owner;
    GetWindowRect(g.hwnd, &owner);
    int w = r.right - r.left, h = r.bottom - r.top;
    CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kAutoClass,
                    playlist < 0 ? L"New auto-playlist" : L"Edit auto-playlist", WS_OVERLAPPEDWINDOW & ~WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                    owner.left + (owner.right - owner.left - w) / 2, owner.top + S(80), w, h, g.hwnd, nullptr, g.inst, nullptr);
    ShowWindow(e.hwnd, SW_SHOW);
    SetFocus(e.name);
    SendMessageW(e.name, EM_SETSEL, 0, -1);
}

// The starter auto-playlists created on first run.
void AddDefaultAutoPlaylists() {
    for (auto& [name, a] : DefaultAutoPlaylists())
        g.playlists.push_back(Playlist{UniquePlaylistName(name), {}, PlaylistKind::Auto, SerializeAutoPlaylist(a)});
    g.dirty = true;
}

}  // namespace app
