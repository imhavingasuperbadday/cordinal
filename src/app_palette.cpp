// Command palette (Ctrl+Shift+P, View ▸ Command palette): one search over every command of the menu
// bar, with its checkmark, its shortcut and where it lives, plus the settings and windows of
// Preferences that have no menu item of their own. It reads the live menu bar, so a new menu item, a
// plugin's included, appears here with no extra work. Enter or a double-click runs the command after
// the palette has closed.
#include "app.h"

#include <cwctype>

namespace app {

namespace {

struct Entry {
    std::wstring label, path, shortcut;
    int command = 0;   // a menu command, or
    int tweak = -1;    // a Tweaks() index
    bool enabled = true, checked = false;
};

struct Palette {
    HWND hwnd = nullptr, edit = nullptr, list = nullptr;
    std::vector<Entry> entries;
} p;

// "&Add files...\tCtrl+O" -> "Add files..." and "Ctrl+O"; "&&" stays a literal ampersand.
void SplitMenuText(const std::wstring& text, std::wstring& label, std::wstring& shortcut) {
    size_t tab = text.find(L'\t');
    shortcut = tab == text.npos ? L"" : text.substr(tab + 1);
    label.clear();
    for (size_t i = 0, end = std::min(tab, text.size()); i < end; ++i) {
        if (text[i] == L'&' && i + 1 < end) ++i;
        label += text[i];
    }
}

void CollectMenu(HMENU menu, const std::wstring& path) {
    for (int i = 0, n = GetMenuItemCount(menu); i < n; ++i) {
        wchar_t text[256]{};
        MENUITEMINFOW mi{sizeof(mi)};
        mi.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_ID | MIIM_SUBMENU | MIIM_STRING;
        mi.dwTypeData = text;
        mi.cch = (UINT)std::size(text);
        if (!GetMenuItemInfoW(menu, i, TRUE, &mi) || (mi.fType & MFT_SEPARATOR) || !text[0]) continue;
        Entry e;
        SplitMenuText(text, e.label, e.shortcut);
        if (mi.hSubMenu) {
            CollectMenu(mi.hSubMenu, path.empty() ? e.label : path + L" \x203A " + e.label);
            continue;
        }
        if (mi.wID == IDM_COMMAND_PALETTE) continue;
        e.path = path;
        e.command = (int)mi.wID;
        e.enabled = !(mi.fState & MFS_DISABLED);
        e.checked = (mi.fState & MFS_CHECKED) != 0;
        p.entries.push_back(std::move(e));
    }
}

void Collect() {
    p.entries.clear();
    if (HMENU bar = MainMenuBar()) {
        UpdateMainMenu(bar);  // checkmarks and enabled state as the menus would show them now
        CollectMenu(bar, L"");
    }
    auto inMenus = [](int command) {
        return std::any_of(p.entries.begin(), p.entries.end(), [&](const Entry& e) { return e.command == command; });
    };
    // A tweak that is also a menu toggle is already listed; the rest show their current value.
    auto tweaks = Tweaks();
    for (int i = 0; i < (int)tweaks.size(); ++i) {
        const Tweak& t = tweaks[i];
        if (!TweakShown(t) || (t.command && !(t.flags & kTweakRadio) && inMenus(t.command))) continue;
        Entry e;
        e.label = t.label;
        e.path = std::wstring(L"Preferences \x203A ") + TweakPage(t);
        e.shortcut = TweakText(t, t.get());
        e.tweak = i;
        p.entries.push_back(std::move(e));
    }
    // The windows Preferences opens (Title formatting, ListenBrainz...) that no menu holds.
    for (const PreferenceLink& link : PreferenceLinks()) {
        if (inMenus(link.command)) continue;
        Entry e;
        SplitMenuText(link.label, e.label, e.shortcut);
        e.path = std::wstring(L"Preferences \x203A ") + link.page;
        e.command = link.command;
        p.entries.push_back(std::move(e));
    }
}

// Lowercase, without mnemonics or dots ("..."), single spaces, and '>' for the palette's separator.
std::wstring CommandKey(const std::wstring& s) {
    std::wstring out;
    for (wchar_t ch : s) {
        wchar_t c = ch == L'\x203A' ? L'>' : (wchar_t)towlower(ch);
        if (c == L'&' || c == L'.') continue;
        if (c == L' ' && (out.empty() || out.back() == L' ' || out.back() == L'>')) continue;
        if (c == L'>' && !out.empty() && out.back() == L' ') out.pop_back();
        out += c;
    }
    while (!out.empty() && out.back() == L' ') out.pop_back();
    return out;
}

void Filter() {
    std::vector<std::wstring> texts;
    for (const Entry& e : p.entries) texts.push_back(e.label + L" " + e.path);
    SendMessageW(p.list, WM_SETREDRAW, FALSE, 0);
    SendMessageW(p.list, LB_RESETCONTENT, 0, 0);
    // Commands that can't run right now (nothing selected, say) follow the ones that can.
    std::vector<int> order = RankMatches(texts, WindowText(p.edit));
    std::stable_partition(order.begin(), order.end(), [](int i) { return p.entries[i].enabled; });
    for (int i : order) {
        const Entry& e = p.entries[i];
        // The item text is what a screen reader announces.
        std::wstring spoken = e.label + (e.checked ? L", checked" : L"") + (e.path.empty() ? L"" : L", " + e.path) +
                              (e.shortcut.empty() ? L"" : L", " + e.shortcut) + (e.enabled ? L"" : L", unavailable");
        int row = (int)SendMessageW(p.list, LB_ADDSTRING, 0, (LPARAM)spoken.c_str());
        SendMessageW(p.list, LB_SETITEMDATA, row, i);
    }
    SendMessageW(p.list, LB_SETCURSEL, 0, 0);
    SendMessageW(p.list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(p.list, nullptr, TRUE);
}

void Close() {
    if (p.hwnd) DestroyWindow(p.hwnd);
}

// Closes first, so the command runs against the main window as if chosen from its menu.
void Run() {
    int row = (int)SendMessageW(p.list, LB_GETCURSEL, 0, 0);
    if (row < 0) return;
    Entry e = p.entries[(size_t)SendMessageW(p.list, LB_GETITEMDATA, row, 0)];
    if (!e.enabled) {
        MessageBeep(MB_OK);
        return;
    }
    Close();
    if (e.command) {
        PostMessageW(g.hwnd, WM_COMMAND, MAKEWPARAM(e.command, 0), 0);
        return;
    }
    const Tweak& t = Tweaks()[e.tweak];
    if (t.type == TweakType::Bool) {
        SetTweak(t, t.get() == 0);
        ShowMessage(std::wstring(t.label) + L": " + TweakText(t, t.get()) + L".");
    } else {
        ShowPreferences(t.key);
    }
}

void Move(int delta) {
    int count = (int)SendMessageW(p.list, LB_GETCOUNT, 0, 0);
    if (!count) return;
    int row = (int)SendMessageW(p.list, LB_GETCURSEL, 0, 0);
    row = std::clamp(row + delta, 0, count - 1);
    SendMessageW(p.list, LB_SETCURSEL, row, 0);
}

// The search box keeps the focus; the arrow keys, Enter and Escape act on the list.
LRESULT CALLBACK EditSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_KEYDOWN) {
        int page = std::max(1, (int)(SendMessageW(p.list, LB_GETLISTBOXINFO, 0, 0)) - 1);
        switch (wp) {
            case VK_UP: Move(-1); return 0;
            case VK_DOWN: Move(1); return 0;
            case VK_PRIOR: Move(-page); return 0;
            case VK_NEXT: Move(page); return 0;
            case VK_RETURN: Run(); return 0;
            case VK_ESCAPE: Close(); return 0;
        }
    }
    if (msg == WM_CHAR && (wp == L'\r' || wp == 27)) return 0;  // no beep for the keys handled above
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(h, EditSubclass, 0);
    return DefSubclassProc(h, msg, wp, lp);
}

void DrawEntry(const DRAWITEMSTRUCT* di) {
    if (di->itemID == (UINT)-1) return;
    const Entry& e = p.entries[(size_t)di->itemData];
    const bool selected = di->itemState & ODS_SELECTED;
    HDC dc = di->hDC;
    RECT r = di->rcItem;
    FillSolid(dc, r, Ref(selected ? P().select : P().surface2));
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ old = SelectObject(dc, g.font);
    COLORREF main = Ref(!e.enabled ? P().faint : selected ? P().selectText : P().text);
    COLORREF sub = Ref(!e.enabled ? P().faint : selected ? P().selectText : P().sub);
    r.left += S(10);
    r.right -= S(10);
    RECT mark = r;
    mark.right = mark.left + S(18);
    if (e.checked) {
        SetTextColor(dc, main);
        DrawTextW(dc, L"\x2713", 1, &mark, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    }
    r.left = mark.right;
    SIZE extent{};
    if (!e.shortcut.empty()) {
        SetTextColor(dc, sub);
        GetTextExtentPoint32W(dc, e.shortcut.c_str(), (int)e.shortcut.size(), &extent);
        DrawTextW(dc, e.shortcut.c_str(), -1, &r, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
        r.right -= extent.cx + S(16);
    }
    SetTextColor(dc, main);
    DrawTextW(dc, e.label.c_str(), -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    GetTextExtentPoint32W(dc, e.label.c_str(), (int)e.label.size(), &extent);
    if (!e.path.empty() && r.left + extent.cx + S(12) < r.right) {
        r.left += extent.cx + S(12);
        SetTextColor(dc, sub);
        DrawTextW(dc, e.path.c_str(), -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    SelectObject(dc, old);
}

LRESULT CALLBACK PaletteProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            p.hwnd = h;
            p.edit = CreateWindowExW(0, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, h, nullptr,
                                     g.inst, nullptr);
            SendMessageW(p.edit, WM_SETFONT, (WPARAM)g.font, TRUE);
            SendMessageW(p.edit, EM_SETCUEBANNER, TRUE, (LPARAM)L"Type a command or setting");
            SetWindowSubclass(p.edit, EditSubclass, 0, 0);
            SetWindowTheme(p.edit, g.dark ? L"DarkMode_CFD" : L"Explorer", nullptr);
            p.list = CreateWindowExW(0, WC_LISTBOXW, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY |
                                     LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
                                     0, 0, 0, 0, h, nullptr, g.inst, nullptr);
            SetWindowTextW(p.list, L"Commands");
            ApplyControlTheme(p.list, g.dark);
            return 0;
        }
        case WM_SIZE: {
            int width = LOWORD(lp), height = HIWORD(lp), pad = S(8), editH = S(26);
            MoveWindow(p.edit, pad, pad, width - pad * 2, editH, TRUE);
            MoveWindow(p.list, 1, pad * 2 + editH, width - 2, height - pad * 2 - editH - 1, TRUE);
            return 0;
        }
        case WM_MEASUREITEM:
            reinterpret_cast<MEASUREITEMSTRUCT*>(lp)->itemHeight = S(28);
            return TRUE;
        case WM_DRAWITEM:
            DrawEntry(reinterpret_cast<DRAWITEMSTRUCT*>(lp));
            return TRUE;
        case WM_ERASEBKGND: {  // a hairline in the border color around the surface
            RECT r;
            GetClientRect(h, &r);
            FillSolid((HDC)wp, r, Ref(P().border));
            InflateRect(&r, -1, -1);
            FillRect((HDC)wp, &r, g.surfaceBrush);
            return 1;
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            return FieldControlColor((HDC)wp);
        case WM_COMMAND:
            if ((HWND)lp == p.edit && HIWORD(wp) == EN_CHANGE) Filter();
            else if ((HWND)lp == p.list && HIWORD(wp) == LBN_DBLCLK) Run();
            else if ((HWND)lp == p.list && HIWORD(wp) == LBN_SELCHANGE) SetFocus(p.edit);
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) PostMessageW(h, WM_CLOSE, 0, 0);  // clicking elsewhere dismisses it
            return 0;
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_NCDESTROY:
            p = Palette{};
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

std::vector<int> RankMatches(const std::vector<std::wstring>& texts, const std::wstring& query) {
    auto lower = [](std::wstring s) {
        for (wchar_t& c : s) c = (wchar_t)std::towlower(c);
        return s;
    };
    std::vector<std::wstring> words;
    std::wstring q = lower(query);
    for (size_t at = 0; at < q.size();) {
        size_t end = q.find(L' ', at);
        if (end == q.npos) end = q.size();
        if (end > at) words.push_back(q.substr(at, end - at));
        at = end + 1;
    }
    while (!q.empty() && q.back() == L' ') q.pop_back();
    while (!q.empty() && q.front() == L' ') q.erase(0, 1);
    std::vector<std::pair<int, int>> ranked;  // (score, index)
    for (int i = 0; i < (int)texts.size(); ++i) {
        std::wstring text = lower(texts[i]);
        bool all = true;
        for (const auto& word : words) all = all && text.find(word) != text.npos;
        if (!all) continue;
        int score = 2;
        if (words.empty() || text.starts_with(q)) score = 0;
        else
            for (size_t at = text.find(words[0]); at != text.npos; at = text.find(words[0], at + 1))
                if (at == 0 || !std::iswalnum(text[at - 1])) {
                    score = 1;
                    break;
                }
        ranked.push_back({score, i});
    }
    std::vector<int> result;
    result.reserve(ranked.size());
    // There are only three ranks. Scanning each rank keeps ties in menu order without a sort buffer.
    for (int score = 0; score < 3; ++score)
        for (const auto& item : ranked)
            if (item.first == score) result.push_back(item.second);
    return result;
}

int MenuCommandByName(const std::wstring& name) {
    std::vector<Entry> saved = std::move(p.entries);  // the palette may be open
    Collect();
    std::wstring want = CommandKey(name);
    // Packs can keep the previous name of the playback-order command after Internet Radio arrived.
    if (want == L"keep playing like this (radio)" || want == L"playback>keep playing like this (radio)")
        want.erase(want.size() - wcslen(L" (radio)"));
    int found = 0;
    for (const Entry& e : p.entries) {
        // A setting that is a plain toggle runs its command, so a key bound to it keeps working
        // after it moves from a menu to Preferences.
        const Tweak* t = e.tweak >= 0 ? &Tweaks()[e.tweak] : nullptr;
        const int command = e.command ? e.command : t && !(t->flags & kTweakRadio) ? t->command : 0;
        if (!command) continue;
        std::wstring label = CommandKey(e.label), path = CommandKey(e.path + L" > " + e.label);
        if (want == path) {
            found = command;
            break;
        }
        if (want == label) found = found && found != command ? -1 : command;
    }
    p.entries = std::move(saved);
    return found;
}

// Typing goes to the palette, never to the main window's accelerators (Ctrl+Left is "previous
// track" there, but moves by a word here).
bool PaletteDialogMessage(MSG* msg) {
    if (!p.hwnd || (msg->hwnd != p.hwnd && !IsChild(p.hwnd, msg->hwnd))) return false;
    TranslateMessage(msg);
    DispatchMessageW(msg);
    return true;
}

void ShowCommandPalette() {
    if (p.hwnd) {
        SetFocus(p.edit);
        return;
    }
    RegisterAppClass(L"CordinalPalette", PaletteProc, CS_DROPSHADOW);
    Collect();
    RECT owner;
    GetWindowRect(g.hwnd, &owner);
    int width = std::min(S(620), std::max((int)(owner.right - owner.left) - S(40), S(360)));
    int height = S(8) * 2 + S(26) + S(28) * 12 + 1;
    HWND h = CreateWindowExW(WS_EX_TOOLWINDOW, L"CordinalPalette", L"Command palette", WS_POPUP | WS_CLIPCHILDREN,
                             owner.left + (owner.right - owner.left - width) / 2, owner.top + S(64), width, height, g.hwnd,
                             nullptr, g.inst, nullptr);
    if (!h) return;
    Filter();
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
    SetFocus(p.edit);
}

}  // namespace app
