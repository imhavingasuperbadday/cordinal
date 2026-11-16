// Command palette (Ctrl+Shift+P, View ▸ Command palette): one search over every command of the menu
// bar, with its checkmark, its shortcut and where it lives, plus the settings and windows of
// Preferences that have no menu item of their own. It reads the live menu bar, so a new menu item, a
// plugin's included, appears here with no extra work. Enter or a double-click runs the command after
// the palette has closed.
//
// The same window is the song picker (ShowQuickPick), Lightweight mode's way to choose music
// without the main window: the transport, what plays next and the playlists, and as you type,
// albums and songs from the library. Enter plays a song now and then carries on with what was
// playing; Shift+Enter puts it next. With nothing typed, Space pauses or resumes and Left and Right
// skip, so the picker is a remote too.
#include "app.h"

#include <cwctype>

namespace app {

namespace {

struct Entry {
    std::wstring label, path, shortcut;
    int command = 0;   // a menu command, or
    int tweak = -1;    // a Tweaks() index, or in the song picker:
    TrackId track = kNone;  // a song (with `album`, its whole album)
    int playlist = -1;      // a playlist to play
    int queued = -1;        // g.queue's index of an Up next song; -1 when it comes from the playlist
    bool album = false;
    bool enabled = true, checked = false;
};

struct Palette {
    HWND hwnd = nullptr, edit = nullptr, list = nullptr;
    std::vector<Entry> entries;
    bool songs = false;  // the song picker
    bool raising = false;  // BringForward is handing it the front: activation flickers, it stays open
    size_t fixed = 0;    // the picker's entries that are not search results
} p;

// A search lists this many at most: the picker is for choosing, the library view for browsing.
constexpr size_t kMaxSongs = 60, kMaxAlbums = 6;

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
    // The commands only keys and right-click menus reach (Volume up, Play next...: app_keys.cpp), under
    // the menu they belong to.
    const bool selected = !SelectedTracks().empty();
    for (const ExtraCommand& x : ExtraCommands()) {
        if (inMenus(x.command)) continue;
        Entry e;
        e.label = x.label;
        e.path = x.path;
        e.command = x.command;
        e.enabled = selected || !x.selection;
        p.entries.push_back(std::move(e));
    }
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

std::wstring SongTitle(TrackId id) {
    std::wstring title = g_lib.Str(g_lib[id].title);
    return title.empty() ? g_lib.Str(g_lib[id].file) : title;
}

std::wstring SongArtist(TrackId id) {
    std::wstring artist = g_lib.Str(g_lib[id].artist);
    return artist.empty() ? g_lib.Str(g_lib[id].albumArtist) : artist;
}

Entry SongEntry(TrackId id, const wchar_t* where) {
    Entry e;
    e.track = id;
    e.label = SongTitle(id);
    std::wstring artist = SongArtist(id);
    e.path = !where ? artist : artist.empty() ? where : where + (L" \x203A " + artist);
    e.shortcut = FormatDuration(g_lib[id].Duration());
    return e;
}

// The picker's standing entries: the transport with what is playing, Up next, the playlists and
// the way back to the full window. A search adds albums and songs after them (FindSongs).
void CollectSongs() {
    p.entries.clear();
    const bool idle = g.playing == kNone && !RadioPlaying();
    const bool paused = !idle && g.engine.Paused();
    Entry play;
    play.label = idle ? L"Play" : paused ? L"Resume" : L"Pause";
    if (g.playing != kNone && g.playing < g_lib.Count()) {
        play.path = SongTitle(g.playing);
        if (std::wstring artist = SongArtist(g.playing); !artist.empty()) play.path += L" \x2014 " + artist;
    }
    play.shortcut = L"Space";
    play.command = IDM_PLAY_PAUSE;
    p.entries.push_back(play);
    Entry next;
    next.label = L"Next";
    next.shortcut = L"\x2192";
    next.command = IDM_NEXT;
    next.enabled = !idle;
    p.entries.push_back(next);
    Entry previous = next;
    previous.label = L"Previous";
    previous.shortcut = L"\x2190";
    previous.command = IDM_PREV;
    p.entries.push_back(previous);
    // Up next: the queue first, then the playing playlist's order.
    std::vector<TrackId> upcoming = UpcomingTracks();
    for (size_t i = 0; i < upcoming.size(); ++i) {
        if (upcoming[i] >= g_lib.Count()) continue;
        Entry e = SongEntry(upcoming[i], L"Up next");
        e.queued = i < g.queue.size() && g.queue[i] == upcoming[i] ? (int)i : -1;
        p.entries.push_back(std::move(e));
    }
    for (int i = 0; i < (int)g.playlists.size(); ++i) {
        const Playlist& pl = g.playlists[i];
        if (pl.items.empty() || pl.kind == PlaylistKind::Inbox) continue;
        Entry e;
        e.label = pl.name;
        e.path = L"Playlist";
        e.shortcut = std::to_wstring(pl.items.size()) + (pl.items.size() == 1 ? L" song" : L" songs");
        e.playlist = i;
        p.entries.push_back(std::move(e));
    }
    Entry shuffle;
    shuffle.label = L"Shuffle";
    shuffle.command = IDM_SHUFFLE;
    shuffle.checked = g.shuffle == kShuffleTracks;
    p.entries.push_back(shuffle);
    Entry back;
    back.label = g.lightweight ? L"Back to the full window" : L"Lightweight mode";
    back.shortcut = g.lightweight ? GlobalKeyText(IDM_QUICK_PICK) : L"Ctrl+Shift+L";
    back.command = IDM_LIGHTWEIGHT;
    p.entries.push_back(back);
    p.fixed = p.entries.size();
}

// Albums and songs in the library that hold every word typed, in library order. Nothing is kept
// between keystrokes, so a search costs no memory once the picker closes.
void FindSongs(const std::wstring& query) {
    p.entries.resize(p.fixed);
    std::vector<std::wstring> words = FilterWords(query);
    if (words.empty()) return;
    TrackMatcher matcher(g_lib, words);
    std::vector<Entry> songs, albums;
    std::vector<StringPool::Handle> albumsSeen;
    for (TrackId id = 0; id < (TrackId)g_lib.Count() && songs.size() < kMaxSongs; ++id) {
        const Track& t = g_lib[id];
        if (!t.InLibrary() || !matcher.Matches(id)) continue;
        songs.push_back(SongEntry(id, nullptr));
        if (!t.album || albums.size() >= kMaxAlbums ||
            std::find(albumsSeen.begin(), albumsSeen.end(), t.album) != albumsSeen.end())
            continue;
        albumsSeen.push_back(t.album);
        Entry a;
        a.track = id;
        a.album = true;
        a.label = g_lib.Str(t.album);
        a.path = L"Album";
        std::wstring artist = g_lib.Str(t.albumArtist);
        if (artist.empty()) artist = g_lib.Str(t.artist);
        if (!artist.empty()) a.path += L" \x203A " + artist;
        albums.push_back(std::move(a));
    }
    for (auto* found : {&albums, &songs})
        for (Entry& e : *found) p.entries.push_back(std::move(e));
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
    const std::wstring query = WindowText(p.edit);
    if (p.songs) FindSongs(query);
    std::vector<std::wstring> texts;
    for (size_t i = 0, n = p.songs ? p.fixed : p.entries.size(); i < n; ++i)
        texts.push_back(p.entries[i].label + L" " + p.entries[i].path);
    SendMessageW(p.list, WM_SETREDRAW, FALSE, 0);
    SendMessageW(p.list, LB_RESETCONTENT, 0, 0);
    // Commands that can't run right now (nothing selected, say) follow the ones that can.
    std::vector<int> order = RankMatches(texts, query);
    std::stable_partition(order.begin(), order.end(), [](int i) { return p.entries[i].enabled; });
    // In the picker a search is for music: what the library found leads.
    if (p.songs && !query.empty())
        for (size_t i = p.entries.size(); i-- > p.fixed;) order.insert(order.begin(), (int)i);
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

// The picker after a transport key: rebuilt, so Pause reads Resume, with the same row selected.
void RefreshSongs() {
    int row = (int)SendMessageW(p.list, LB_GETCURSEL, 0, 0);
    CollectSongs();
    Filter();
    if (row > 0) SendMessageW(p.list, LB_SETCURSEL, row, 0);
}

// Plays what was picked: a song now, then on with what was playing (`next` only puts it next); an
// album or a playlist from its start, or a random place in shuffle.
void PlayPicked(const Entry& e, bool next) {
    if (e.playlist >= 0) {
        if (e.playlist >= (int)g.playlists.size() || g.playlists[e.playlist].items.empty()) return;
        const size_t count = g.playlists[e.playlist].items.size();
        PlayTrack(e.playlist, g.shuffle == kShuffleTracks ? (int)(GetTickCount64() % count) : 0, true);
        return;
    }
    if (e.track >= g_lib.Count()) return;
    if (e.album) {
        std::vector<TrackId> tracks = AlbumTracksOf({e.track});
        if (next) QueueTracks(tracks, true);
        else PlayList(tracks, 0);
        return;
    }
    if (!next && e.queued >= 0 && e.queued < (int)g.queue.size() && g.queue[e.queued] == e.track) {
        PlayQueued(e.queued);
        return;
    }
    const bool playing = g.playing != kNone || RadioPlaying();
    QueueTracks({e.track}, true);  // starts it at once when nothing is playing
    if (!next && playing) Next();
}

// Closes first, so the command runs against the main window as if chosen from its menu.
void Run(bool next = false) {
    int row = (int)SendMessageW(p.list, LB_GETCURSEL, 0, 0);
    if (row < 0) return;
    Entry e = p.entries[(size_t)SendMessageW(p.list, LB_GETITEMDATA, row, 0)];
    if (!e.enabled) {
        MessageBeep(MB_OK);
        return;
    }
    if (p.songs) {
        // The transport keeps the picker open: pause now, pick a song after.
        if (e.command == IDM_PLAY_PAUSE || e.command == IDM_NEXT || e.command == IDM_PREV || e.command == IDM_SHUFFLE) {
            OnCommand(e.command, 0, nullptr);
            RefreshSongs();
            return;
        }
        if (!e.command) {
            Close();
            PlayPicked(e, next);
            return;
        }
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
            case VK_RETURN: Run(GetKeyState(VK_SHIFT) < 0); return 0;
            case VK_ESCAPE: Close(); return 0;
        }
        // The picker with nothing typed is a remote: Space pauses, Left and Right skip.
        if (p.songs && GetWindowTextLengthW(h) == 0 && GetKeyState(VK_CONTROL) >= 0 && GetKeyState(VK_MENU) >= 0) {
            const int command = wp == VK_SPACE ? IDM_PLAY_PAUSE : wp == VK_LEFT ? IDM_PREV : wp == VK_RIGHT ? IDM_NEXT : 0;
            if (command) {
                OnCommand(command, 0, nullptr);
                RefreshSongs();
                return 0;
            }
        }
    }
    if (msg == WM_CHAR && (wp == L'\r' || wp == 27)) return 0;  // no beep for the keys handled above
    if (msg == WM_CHAR && wp == L' ' && p.songs && GetWindowTextLengthW(h) == 0) return 0;  // Space paused
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
            SendMessageW(p.edit, EM_SETCUEBANNER, TRUE,
                         (LPARAM)(p.songs ? L"Type a song, album or artist (Space pauses)" : L"Type a command or setting"));
            SetWindowSubclass(p.edit, EditSubclass, 0, 0);
            SetWindowTheme(p.edit, g.dark ? L"DarkMode_CFD" : L"Explorer", nullptr);
            p.list = CreateWindowExW(0, WC_LISTBOXW, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY |
                                     LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
                                     0, 0, 0, 0, h, nullptr, g.inst, nullptr);
            SetWindowTextW(p.list, p.songs ? L"Songs" : L"Commands");
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
            if (LOWORD(wp) == WA_INACTIVE && !p.raising) PostMessageW(h, WM_CLOSE, 0, 0);  // clicking elsewhere dismisses it
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
    // Keys and packs saved before Edit > Reload selected metadata took the tags' name.
    if (want == L"reload selected metadata" || want == L"edit>reload selected metadata")
        want = L"edit>reload tags from files";
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
    if (p.hwnd && p.songs) Close();
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

// Windows lets only the program being typed into bring another window to the front. A numpad key
// the Lightweight mode hook kept never reached any window, so the picker borrows the front
// window's input state for the moment it takes its place, as launchers do; without that it would
// open behind the program in use and never get the keys.
static void BringForward(HWND h) {
    HWND front = GetForegroundWindow();
    const DWORD frontThread = front ? GetWindowThreadProcessId(front, nullptr) : 0, me = GetCurrentThreadId();
    p.raising = true;
    const bool attached = frontThread && frontThread != me && AttachThreadInput(me, frontThread, TRUE);
    SetForegroundWindow(h);
    SetFocus(p.edit);
    if (attached) AttachThreadInput(me, frontThread, FALSE);
    p.raising = false;
}

// The song picker. Over the main window while it shows; otherwise a third of the way down the
// screen the mouse is on, where a launcher appears, or by the clock when it was opened from the
// notification-area icon.
void ShowQuickPick(bool nearTray) {
    if (p.hwnd && !p.songs) Close();
    if (p.hwnd) {
        BringForward(p.hwnd);
        SetFocus(p.edit);
        return;
    }
    RegisterAppClass(L"CordinalPalette", PaletteProc, CS_DROPSHADOW);
    p.songs = true;
    CollectSongs();
    POINT cursor{};
    GetCursorPos(&cursor);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& work = mi.rcWork;
    const int width = std::min(S(560), (int)(work.right - work.left) - S(16));
    const int height = std::min(S(8) * 2 + S(26) + S(28) * 12 + 1, (int)(work.bottom - work.top) - S(16));
    int x = work.left + (work.right - work.left - width) / 2, y = work.top + (work.bottom - work.top) / 3 - S(40);
    if (nearTray) {
        x = work.right - width - S(12);
        y = work.bottom - height - S(12);
    } else if (IsWindowVisible(g.hwnd) && !IsIconic(g.hwnd)) {
        RECT owner;
        GetWindowRect(g.hwnd, &owner);
        x = owner.left + (owner.right - owner.left - width) / 2;
        y = owner.top + S(64);
    }
    x = std::clamp(x, (int)work.left, std::max((int)work.left, (int)work.right - width));
    y = std::clamp(y, (int)work.top, std::max((int)work.top, (int)work.bottom - height));
    HWND h = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"CordinalPalette", L"Play a song",
                             WS_POPUP | WS_CLIPCHILDREN, x, y, width, height, g.hwnd, nullptr, g.inst, nullptr);
    if (!h) {
        p = Palette{};
        return;
    }
    Filter();
    ShowWindow(h, SW_SHOWNA);  // BringForward activates it
    BringForward(h);
    SetFocus(p.edit);
}

bool PaletteOpen() { return p.hwnd != nullptr; }
bool QuickPickOpen() { return p.hwnd && p.songs; }

// Every command the palette lists that isn't a setting: the menu bar's and the extra ones.
std::vector<MenuCommandInfo> AllMenuCommands() {
    std::vector<Entry> saved = std::move(p.entries);  // the palette may be open
    Collect();
    std::vector<MenuCommandInfo> out;
    for (const Entry& e : p.entries)
        if (e.command && e.tweak < 0) out.push_back({e.label, e.path, e.command});
    p.entries = std::move(saved);
    return out;
}

// "Playback > Next" for IDM_NEXT, as bindings and packs name it; empty when nothing lists it.
std::wstring CommandName(int command) {
    for (const MenuCommandInfo& c : AllMenuCommands()) {
        if (c.command != command) continue;
        std::wstring path = c.path;
        std::replace(path.begin(), path.end(), L'\x203A', L'>');
        return path.empty() ? c.label : path + L" > " + c.label;
    }
    return {};
}
void ClosePalette() { Close(); }

// For the self-test: the picker's rows as shown, label then path.
std::vector<std::wstring> QuickPickRows() {
    std::vector<std::wstring> rows;
    if (!p.hwnd) return rows;
    for (int i = 0, n = (int)SendMessageW(p.list, LB_GETCOUNT, 0, 0); i < n; ++i) {
        const Entry& e = p.entries[(size_t)SendMessageW(p.list, LB_GETITEMDATA, i, 0)];
        rows.push_back(e.label + L" | " + e.path);
    }
    return rows;
}

void SetQuickPickQuery(const std::wstring& query) {
    if (p.edit) SetWindowTextW(p.edit, query.c_str());  // EN_CHANGE filters
}

}  // namespace app
