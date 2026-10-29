// Packs in the running player (docs/plugins.md, Tier 1; packs.cpp reads them). A pack is a folder or
// a .cordpack file in <data>\packs. Dropping one there changes nothing until it is turned on in
// View > Packs ([Packs] On in settings.ini); holding Shift at start, or --safe, starts without any.
// The packs that are on apply in name order, and where only one can win (the window title, the
// group headers) the last one does. Themes join View > Theme, held to the built-in themes' contrast
// rules; columns join the track list's header menu; keys go in front of the built-in shortcuts;
// presets join the Equalizer and Customize layout; fonts load privately for this process; a pack
// theme's sounds replace the sound effects they name; auto-playlists wait in the Playlist menu
// until added; and $define functions reach every title format. A pack dropped on the window (or
// opened with cordinal) is installed after the user has seen what it holds, and View > Packs >
// Make a pack writes the current setup out as a pack to edit.
#include "app.h"
#include "native_audio.h"
#include "app_packfiles.h"

namespace app {

namespace {

constexpr size_t kMaxFile = 8u << 20, kMaxPackBytes = 32u << 20, kMaxFiles = 256;

struct State {
    // Heap-held so the strings and palettes that themes and columns point to never move.
    std::vector<std::unique_ptr<Pack>> packs;
    std::vector<std::unique_ptr<Pack>> retired;  // the packs before a reload, until the window lets go of them
    std::vector<bool> on;
    std::vector<std::vector<TitleFunction>> functions;  // per pack, parsed once
    bool safe = false;
    std::vector<std::wstring> themeKeys;  // "pack/theme" of each pack theme, in ThemeMode order
    std::deque<std::wstring> themeNames;  // their menu names ('&' doubled)
    std::vector<std::wstring> columnKeys; // "pack/column" of each pack column, in type order
    std::vector<Column> columns;
    std::vector<HANDLE> fonts;
    std::vector<std::wstring> keyWarnings;
    std::vector<const PackTheme*> themes;  // each pack theme, in ThemeMode order
    struct Clip {
        bool tried = false;
        unsigned rate = 0;
        std::vector<float> stereo;
    };
    std::vector<std::array<Clip, (size_t)Sfx::Count>> clips;  // a pack theme's sounds, decoded when first played
    std::vector<const PackAutoPlaylist*> autoPlaylists;
    std::vector<const PackColumnSet*> columnSets;
    std::vector<std::pair<std::wstring, std::wstring>> fields;  // the later pack's wins
    std::vector<std::pair<int, int>> hotkeys;                     // registered id, command
} s;

std::wstring PacksDir() { return g.dataDir + L"\\packs"; }

bool ReadBytes(const std::wstring& path, size_t max, std::string& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) && (uint64_t)size.QuadPart <= max;
    if (ok) {
        out.resize((size_t)size.QuadPart);
        DWORD got = 0;
        ok = out.empty() || (ReadFile(f, out.data(), (DWORD)out.size(), &got, nullptr) && got == out.size());
    }
    CloseHandle(f);
    return ok;
}

// A pack folder's files, two folders deep at most, within the size limits.
bool ReadFolder(const std::wstring& dir, const std::wstring& prefix, int depth, std::vector<PackEntry>& out, size_t& total,
                std::wstring& error) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return true;
    bool ok = true;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        std::wstring lower = name;
        for (auto& c : lower) c = (wchar_t)towlower(c);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth < 2) ok = ReadFolder(dir + L"\\" + name, prefix + lower + L"/", depth + 1, out, total, error);
        } else if (out.size() >= kMaxFiles) {
            error = L"it has more than 256 files";
            ok = false;
        } else {
            PackEntry e{prefix + lower, {}};
            if (!ReadBytes(dir + L"\\" + name, kMaxFile, e.bytes)) {
                error = e.path + L" can't be read, or is larger than 8 MB";
                ok = false;
            } else if ((total += e.bytes.size()) > kMaxPackBytes) {
                error = L"it is larger than 32 MB";
                ok = false;
            } else out.push_back(std::move(e));
        }
    } while (ok && FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

// A new file of a pack being made; the folder it is in is renamed into place only when every write
// succeeded.
bool WriteUtf8File(const std::wstring& file, const std::wstring& text) {
    std::string bytes = ToUtf8(text);
    HANDLE f = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(f, bytes.data(), (DWORD)bytes.size(), &written, nullptr) && written == bytes.size();
    return CloseHandle(f) && ok;
}

std::vector<std::wstring> SplitList(const std::wstring& text) {
    std::vector<std::wstring> out;
    for (size_t at = 0; at <= text.size();) {
        size_t bar = std::min(text.find(L'|', at), text.size());
        if (bar > at) out.push_back(text.substr(at, bar - at));
        at = bar + 1;
    }
    return out;
}

std::wstring JoinList(const std::vector<std::wstring>& items) {
    std::wstring out;
    for (const auto& i : items) out += (out.empty() ? L"" : L"|") + i;
    return out;
}

void SaveOn() {
    std::vector<std::wstring> ids;
    for (size_t i = 0; i < s.packs.size(); ++i)
        if (s.on[i]) ids.push_back(s.packs[i]->id);
    IniWrite(L"Packs", L"On", JoinList(ids));
}

bool HasExtension(const std::wstring& path, const wchar_t* ext) {
    size_t n = wcslen(ext);
    return path.size() > n && _wcsicmp(path.c_str() + path.size() - n, ext) == 0;
}

// One pack from a folder, or from a .cordpack (or .zip) file.
std::unique_ptr<Pack> ReadPackAt(const std::wstring& path, const std::wstring& id, bool archive) {
    std::vector<PackEntry> files;
    std::wstring error;
    size_t total = 0;
    if (archive) {
        std::string zip;
        if (!ReadBytes(path, kMaxPackBytes, zip)) error = L"it can't be read, or is larger than 32 MB";
        else ReadPackZip(zip, files, error);
    } else {
        ReadFolder(path, L"", 0, files, total, error);
    }
    auto pack = std::make_unique<Pack>(error.empty() ? ParsePack(id, files) : Pack{});
    pack->id = id;
    if (!error.empty()) pack->name = id, pack->error = error;
    return pack;
}

// Reads the packs folder; returns how many packs it holds that earlier runs had not seen. The pack
// themes, columns and the palette in use point into the old packs, so those stay until Refresh.
int ReadAll() {
    s.retired = std::move(s.packs);
    s.packs.clear();
    s.on.clear();
    s.functions.clear();
    std::vector<std::pair<std::wstring, bool>> found;  // id, is a .cordpack
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((PacksDir() + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = fd.cFileName;
            bool dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (name == L"." || name == L"..") continue;
            if (dir) found.push_back({name, false});
            else if (name.size() > 9 && _wcsicmp(name.c_str() + name.size() - 9, L".cordpack") == 0)
                found.push_back({name.substr(0, name.size() - 9), true});
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(found.begin(), found.end(), [](auto& a, auto& b) { return _wcsicmp(a.first.c_str(), b.first.c_str()) < 0; });
    if (found.size() > (size_t)kMaxPacks) found.resize(kMaxPacks);
    auto onList = SplitList(IniRead(L"Packs", L"On", L"")), known = SplitList(IniRead(L"Packs", L"Known", L""));
    auto listed = [](const std::vector<std::wstring>& list, const std::wstring& id) {
        return std::any_of(list.begin(), list.end(), [&](auto& x) { return _wcsicmp(x.c_str(), id.c_str()) == 0; });
    };
    int fresh = 0;
    std::vector<std::wstring> ids;
    for (const auto& [id, archive] : found) {
        if (!ids.empty() && _wcsicmp(ids.back().c_str(), id.c_str()) == 0) continue;  // a folder and a .cordpack of one name
        ids.push_back(id);
        auto pack = ReadPackAt(PacksDir() + L"\\" + id + (archive ? L".cordpack" : L""), id, archive);
        std::vector<std::wstring> problems;
        s.functions.push_back(ParseTitleFunctions(pack->functions, problems));
        for (auto& p : problems) pack->warnings.push_back(L"functions.tf: " + p);
        for (const auto& t : pack->themes)
            if (!t.legibility.empty()) pack->warnings.push_back(L"The theme " + t.name + L" is hard to read: " + t.legibility);
        s.on.push_back(pack->error.empty() && listed(onList, id));
        fresh += !listed(known, id);
        s.packs.push_back(std::move(pack));
    }
    IniWrite(L"Packs", L"Known", JoinList(ids));
    return fresh;
}

template <class F>
void ForEachOn(F f) {
    for (size_t i = 0; i < s.packs.size(); ++i)
        if (s.on[i]) f(*s.packs[i], i);
}

std::wstring EscapeAmp(const std::wstring& text) {
    std::wstring out;
    for (wchar_t c : text) out += c == L'&' ? L"&&" : std::wstring(1, c);
    return out;
}

std::wstring ThemeKeyOf(ThemeMode mode) {
    int k = (int)mode - (int)ThemeMode::Count;
    return k >= 0 && k < (int)s.themeKeys.size() ? s.themeKeys[k] : L"";
}

ThemeMode ThemeByKey(const std::wstring& key, ThemeMode fallback) {
    for (size_t k = 0; k < s.themeKeys.size(); ++k)
        if (!key.empty() && _wcsicmp(s.themeKeys[k].c_str(), key.c_str()) == 0) return (ThemeMode)((int)ThemeMode::Count + (int)k);
    return fallback;
}

// The current and default themes by name, taken while the pack themes they may be are still loaded.
struct ThemeMemo {
    std::wstring key, defaultKey;
    ThemeMode base, defaultBase;
};
ThemeMemo Remember() { return {ThemeKeyOf(g.themeMode), ThemeKeyOf(g.defaultTheme), BuiltInTheme(g.themeMode), BuiltInTheme(g.defaultTheme)}; }

// Makes what the packs that are on hold current: fonts, themes, functions, columns and formats. The
// window, when there is one, follows in Refresh.
void Apply(const ThemeMemo& memo) {
    for (HANDLE f : s.fonts) RemoveFontMemResourceEx(f);
    s.fonts.clear();
    std::vector<Theme> themes;
    std::vector<TitleFunction> functions;
    s.themeKeys.clear();
    s.themeNames.clear();
    s.columnKeys.clear();
    s.columns.clear();
    s.themes.clear();
    s.clips.clear();
    s.autoPlaylists.clear();
    s.columnSets.clear();
    s.fields.clear();
    ForEachOn([&](Pack& pack, size_t i) {
        for (const PackAutoPlaylist& a : pack.autoPlaylists) s.autoPlaylists.push_back(&a);
        for (const PackColumnSet& set : pack.columnSets) s.columnSets.push_back(&set);
        for (const auto& field : pack.fields) {
            std::erase_if(s.fields, [&](auto& f) { return f.first == field.first; });
            s.fields.push_back(field);
        }
        for (const PackFont& font : pack.fonts) {
            DWORD count = 0;
            if (HANDLE f = AddFontMemResourceEx((void*)font.bytes.data(), (DWORD)font.bytes.size(), nullptr, &count)) s.fonts.push_back(f);
        }
        for (const PackTheme& t : pack.themes) {
            if (themes.size() >= (size_t)kMaxPackThemes) break;
            s.themeKeys.push_back(pack.id + L"/" + t.name);
            s.themes.push_back(&t);
            s.themeNames.push_back(EscapeAmp(t.name));
            Theme theme{s.themeNames.back().c_str(), &t.palette, t.font.empty() ? nullptr : t.font.c_str(), 0, t.fontPx,
                        t.mark.c_str(), t.prefix.c_str(), false, t.ratingMark.empty() ? L"\x2605" : t.ratingMark.c_str(),
                        t.wavePlayed, t.waveRest};
            themes.push_back(theme);
        }
        for (const TitleFunction& f : s.functions[i]) {
            std::erase_if(functions, [&](const TitleFunction& o) { return o.name == f.name; });  // the later pack's wins
            functions.push_back(f);
        }
        for (const PackColumn& c : pack.columns) {
            if (ui::kColumnCount + s.columns.size() >= (size_t)kMaxColumnTypes) break;
            s.columnKeys.push_back(pack.id + L"/" + c.key);
            s.columns.push_back({c.title.c_str(), c.width, c.display.c_str(), c.sort.c_str(), c.right ? ui::kAlignRight : ui::kAlignLeft});
        }
    });
    s.clips.resize(s.themes.size());
    SetPackThemes(std::move(themes));
    SetTitleFunctions(std::move(functions));
    // A pack theme that went away leaves its built-in base behind; one that came back is picked up.
    g.themeMode = memo.key.empty() ? memo.base : ThemeByKey(memo.key, memo.base);
    g.defaultTheme = memo.defaultKey.empty() ? memo.defaultBase : ThemeByKey(memo.defaultKey, memo.defaultBase);
    ApplyPackFormats();
}

// Brings the running window in line after the packs changed.
void Refresh(const ThemeMemo& memo) {
    // The caller saved the columns while the old pack columns could still be named; LoadColumns
    // maps those names onto the new ones.
    Apply(memo);
    if (!g.hwnd) return s.retired.clear();
    ApplyTheme();  // first: the palette in use may be a retired pack's
    // A pack column's number may now name another column, or none: sorts by one fall back to the
    // list's own order.
    for (auto& pl : g.playlists)
        if (pl.sortColumn >= kColumnCount) pl.sortColumn = -1;
    if (g.sortColumn >= kColumnCount) g.sortColumn = -1;
    if (g.list) {
        LoadColumns();
        SetupColumns();
        ClearCellCache();
        RefreshViewKeepingSelection();
    }
    HMENU old = MainMenuBar();
    ReplaceMenuBar(BuildMenu());
    if (old) DestroyMenu(old);
    if (g.accel) DestroyAcceleratorTable(g.accel);
    g.accel = BuildAccelerators();
    UpdateNowPlaying();
    UpdateStatus();
    SavePreferences();
    s.retired.clear();
}

// "Ctrl+Shift+K" -> FCONTROL | FSHIFT, 'K'. A key without Ctrl or Alt must be one that can't be
// typed (F1-F24, the media keys), so a pack never takes letters from the search box.
bool ParseKeys(std::wstring text, ACCEL& out) {
    std::replace(text.begin(), text.end(), L'_', L' ');  // the ini reader turns spaces in a key into '_'
    out = {FVIRTKEY, 0, 0};
    size_t at = 0;
    for (;;) {
        size_t plus = text.find(L'+', at);
        if (plus == text.size() - 1) plus = std::wstring::npos;  // "Ctrl++": the key itself is '+'
        std::wstring part = text.substr(at, plus == std::wstring::npos ? std::wstring::npos : plus - at);
        part.erase(0, part.find_first_not_of(L' '));
        part.erase(part.find_last_not_of(L' ') + 1);
        if (plus == std::wstring::npos) {
            static const struct { const wchar_t* name; WORD vk; } names[] = {
                {L"space", VK_SPACE}, {L"enter", VK_RETURN}, {L"tab", VK_TAB}, {L"esc", VK_ESCAPE}, {L"escape", VK_ESCAPE},
                {L"backspace", VK_BACK}, {L"delete", VK_DELETE}, {L"del", VK_DELETE}, {L"insert", VK_INSERT},
                {L"ins", VK_INSERT}, {L"home", VK_HOME}, {L"end", VK_END}, {L"pageup", VK_PRIOR}, {L"pgup", VK_PRIOR},
                {L"pagedown", VK_NEXT}, {L"pgdn", VK_NEXT}, {L"up", VK_UP}, {L"down", VK_DOWN}, {L"left", VK_LEFT},
                {L"right", VK_RIGHT}, {L"plus", VK_OEM_PLUS}, {L"+", VK_OEM_PLUS}, {L"minus", VK_OEM_MINUS},
                {L"-", VK_OEM_MINUS}, {L"comma", VK_OEM_COMMA}, {L",", VK_OEM_COMMA}, {L"period", VK_OEM_PERIOD},
                {L".", VK_OEM_PERIOD}, {L"playpause", VK_MEDIA_PLAY_PAUSE}, {L"mediaplaypause", VK_MEDIA_PLAY_PAUSE},
                {L"medianext", VK_MEDIA_NEXT_TRACK}, {L"mediaprevious", VK_MEDIA_PREV_TRACK}, {L"mediaprev", VK_MEDIA_PREV_TRACK},
                {L"mediastop", VK_MEDIA_STOP}, {L"volumeup", VK_VOLUME_UP}, {L"volumedown", VK_VOLUME_DOWN},
                {L"volumemute", VK_VOLUME_MUTE}};
            std::wstring k = part;
            for (auto& c : k) c = (wchar_t)towlower(c);
            if (k.size() == 1 && iswalnum(k[0])) out.key = (WORD)towupper(k[0]);
            else if (k.size() >= 2 && k[0] == L'f' && iswdigit(k[1]) && _wtoi(k.c_str() + 1) >= 1 && _wtoi(k.c_str() + 1) <= 24)
                out.key = (WORD)(VK_F1 + _wtoi(k.c_str() + 1) - 1);
            for (const auto& n : names)
                if (k == n.name) out.key = n.vk;
            bool typed = out.key < VK_F1 || out.key > VK_F24;
            bool media = out.key >= VK_VOLUME_MUTE && out.key <= VK_MEDIA_PLAY_PAUSE;
            return out.key && ((out.fVirt & (FCONTROL | FALT)) || !typed || media);
        }
        std::wstring m = part;
        for (auto& c : m) c = (wchar_t)towlower(c);
        if (m == L"ctrl" || m == L"control") out.fVirt |= FCONTROL;
        else if (m == L"shift") out.fVirt |= FSHIFT;
        else if (m == L"alt") out.fVirt |= FALT;
        else return false;
        at = plus + 1;
    }
}

}  // namespace

void LoadPacks(bool safe) {
    ThemeMemo memo = Remember();
    State old = std::move(s);  // what the theme registry and palette point into, until they move on
    s = State{};
    s.safe = safe;
    s.fonts = std::move(old.fonts);  // Apply removes them
    CreateDirectoryW(PacksDir().c_str(), nullptr);
    if (safe) {
        g.message = L"Safe mode: packs are off for this run.";
        g.messageUntil = GetTickCount64() + 15000;
    } else if (int fresh = ReadAll()) {
        g.message = fresh == 1 ? L"Found a new pack. Turn it on in View > Packs." : L"Found new packs. Turn them on in View > Packs.";
        g.messageUntil = GetTickCount64() + 15000;
    }
    Apply(memo);
    s.retired.clear();
    if (g.hwnd) ApplyTheme();
}

void ReloadPacks() {
    if (s.safe) {
        ShowMessage(L"Safe mode: packs stay off until cordinal starts normally.");
        return;
    }
    if (g.list) SaveColumns();  // while the old pack columns are still there to name
    ThemeMemo memo = Remember();
    ReadAll();
    Refresh(memo);
    size_t on = std::count(s.on.begin(), s.on.end(), true);
    ShowMessage(std::to_wstring(s.packs.size()) + (s.packs.size() == 1 ? L" pack, " : L" packs, ") + std::to_wstring(on) + L" on.");
}

HMENU PacksMenu() {
    HMENU m = CreatePopupMenu();
    if (s.safe) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"Off in safe mode");
    for (size_t i = 0; i < s.packs.size(); ++i) {
        const Pack& p = *s.packs[i];
        std::wstring label = EscapeAmp(p.name) + (p.version.empty() ? L"" : L" " + EscapeAmp(p.version));
        if (!p.error.empty()) label += L"  (can't be used: " + EscapeAmp(p.error) + L")";
        AppendMenuW(m, MF_STRING | (s.on[i] ? MF_CHECKED : 0) | (p.error.empty() ? 0 : MF_GRAYED), IDM_PACK_BASE + i, label.c_str());
    }
    if (s.packs.empty() && !s.safe) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"No packs yet");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_PACKS_FOLDER, L"Open the packs &folder");
    AppendMenuW(m, MF_STRING, IDM_PACKS_RELOAD, L"&Reload packs");
    AppendMenuW(m, MF_STRING, IDM_PACKS_REPORT, L"What packs &left out...");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_PACKS_NEW, L"&Make a pack from my setup...");
    return m;
}

void AppendPackThemes(HMENU themes) {
    if (s.themeKeys.empty()) return;
    AppendMenuW(themes, MF_SEPARATOR, 0, nullptr);
    for (size_t k = 0; k < s.themeKeys.size(); ++k)
        AppendMenuW(themes, MF_STRING, IDM_PACK_THEME_BASE + k, ThemeInfo((ThemeMode)((int)ThemeMode::Count + k)).name);
}

bool PackCommand(int id) {
    if (id >= IDM_PACK_THEME_BASE && id < IDM_PACK_THEME_BASE + (int)s.themeKeys.size()) {
        g.themeMode = (ThemeMode)((int)ThemeMode::Count + id - IDM_PACK_THEME_BASE);
        ApplyTheme();
        SavePreferences();
        // A theme that breaks the contrast rules still loads; the user hears which pair fails.
        std::wstring problem = PaletteLegibility(P());
        if (!problem.empty()) ShowMessage(L"This theme is hard to read in places: " + problem + L".");
        return true;
    }
    if (id >= IDM_PACK_BASE && id < IDM_PACK_BASE + (int)s.packs.size()) {
        size_t i = id - IDM_PACK_BASE;
        if (!s.packs[i]->error.empty()) return true;
        ThemeMemo memo = Remember();
        if (g.list) SaveColumns();
        s.on[i] = !s.on[i];
        SaveOn();
        Refresh(memo);
        ShowMessage((s.on[i] ? L"Turned on " : L"Turned off ") + s.packs[i]->name + L".");
        return true;
    }
    if (id == IDM_PACKS_FOLDER) {
        ShellExecuteW(g.hwnd, L"open", PacksDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return true;
    }
    if (id == IDM_PACKS_RELOAD) {
        ReloadPacks();
        return true;
    }
    if (id >= IDM_PACK_COLSET_BASE && id < IDM_PACK_COLSET_BASE + (int)s.columnSets.size()) {
        // Names match a built-in column's or a pack column's title or key, ignoring case and '&'.
        auto plain = [](std::wstring t) {
            std::erase(t, L'&');
            return t;
        };
        std::vector<int> types{0};
        std::wstring missing;
        for (const std::wstring& name : s.columnSets[id - IDM_PACK_COLSET_BASE]->show) {
            int found = -1;
            for (int t = 1; t < ColumnTotal() && found < 0; ++t)
                if (_wcsicmp(plain(ColumnAt(t).name).c_str(), name.c_str()) == 0 ||
                    (t >= kColumnCount && _wcsicmp(PackColumnName(t).substr(PackColumnName(t).find(L'/') + 1).c_str(), name.c_str()) == 0))
                    found = t;
            if (found > 0 && std::find(types.begin(), types.end(), found) == types.end()) types.push_back(found);
            else if (found < 0) missing += (missing.empty() ? L"" : L", ") + name;
        }
        ShowColumns(types);
        if (!missing.empty()) ShowMessage(L"No column is called " + missing + L".");
        return true;
    }
    if (id == IDM_PACKS_NEW) {
        MakeStarterPack(true);
        return true;
    }
    if (id >= IDM_PACK_AUTO_BASE && id < IDM_PACK_AUTO_BASE + (int)s.autoPlaylists.size()) {
        const PackAutoPlaylist& a = *s.autoPlaylists[id - IDM_PACK_AUTO_BASE];
        g.playlists.push_back(Playlist{UniquePlaylistName(a.name), {}, PlaylistKind::Auto, a.rules});
        g.dirty = true;
        PlaySfx(Sfx::Sparkle);
        RefreshTabs();
        ActivatePlaylist((int)g.playlists.size() - 1);
        return true;
    }
    if (id == IDM_PACKS_REPORT) {
        std::wstring text;
        for (size_t i = 0; i < s.packs.size(); ++i) {
            const Pack& p = *s.packs[i];
            if (p.error.empty() && p.warnings.empty()) continue;
            text += p.name + (s.on[i] ? L"" : L" (off)") + L"\n";
            if (!p.error.empty()) text += L"    Can't be used: " + p.error + L"\n";
            for (const auto& w : p.warnings) text += L"    " + w + L"\n";
        }
        for (const auto& w : s.keyWarnings) text += w + L"\n";
        if (text.empty()) text = s.packs.empty() ? L"There are no packs in " + PacksDir() + L"." : L"Every pack was read in full.";
        MessageBoxW(g.hwnd, text.c_str(), L"Packs", MB_OK | MB_ICONINFORMATION);
        return true;
    }
    return false;
}

ThemeMode ReadSavedTheme() {
    ThemeMode base = AvailableTheme((ThemeMode)std::clamp(IniInt(L"Window", L"Theme", (int)ThemeMode::Light), 0, (int)ThemeMode::Count - 1));
    return ThemeByKey(IniRead(L"Window", L"PackTheme", L""), base);
}

void WriteSavedTheme(ThemeMode mode) {
    IniWrite(L"Window", L"Theme", std::to_wstring((int)BuiltInTheme(mode)));
    IniWrite(L"Window", L"PackTheme", ThemeKeyOf(mode));
}

ThemeMode BuiltInTheme(ThemeMode mode) {
    if (!PackThemeMode(mode)) return mode;
    const Theme& t = ThemeInfo(mode);
    return t.palette && t.palette->dark ? ThemeMode::Dark : ThemeMode::Light;
}

int ColumnTotal() { return ui::kColumnCount + (int)s.columns.size(); }

const Column& ColumnAt(int type) {
    static const Column none{L"", 100, L"", L"", ui::kAlignLeft};
    if (type >= 0 && type < ui::kColumnCount) return kColumns[type];
    size_t k = (size_t)type - ui::kColumnCount;
    return type >= ui::kColumnCount && k < s.columns.size() ? s.columns[k] : none;
}

// A column's name as a column set lists it: the built-in name without its '&', or a pack column's key.
std::wstring PackColumnTitle(int type) {
    if (type >= kColumnCount) {
        std::wstring name = PackColumnName(type);
        return name.substr(name.find(L'/') + 1);
    }
    std::wstring name = ColumnAt(type).name;
    std::erase(name, L'&');
    return name;
}

std::wstring PackColumnName(int type) {
    size_t k = (size_t)type - ui::kColumnCount;
    return type >= ui::kColumnCount && k < s.columnKeys.size() ? s.columnKeys[k] : L"";
}

int PackColumnType(const std::wstring& name) {
    for (size_t k = 0; k < s.columnKeys.size(); ++k)
        if (_wcsicmp(s.columnKeys[k].c_str(), name.c_str()) == 0) return ui::kColumnCount + (int)k;
    return -1;
}

const PackGroups* PackGroupLook() {
    const PackGroups* look = nullptr;
    ForEachOn([&](const Pack& p, size_t) {
        if (p.groups.any) look = &p.groups;
    });
    return look;
}

const Pack* PackFormatOwner(int slot) {
    const Pack* owner = nullptr;
    ForEachOn([&](const Pack& p, size_t) {
        if (slot >= 0 && slot < kPackFormatCount && !p.formats[slot].empty()) owner = &p;
    });
    return owner;
}

void ApplyPackFormats() {
    LoadFormats();
    std::wstring* targets[kPackFormatCount] = {&g.fmtWindow, &g.fmtStatus, &g.fmtTitle, &g.fmtSub};
    for (int i = 0; i < kPackFormatCount; ++i)
        if (const Pack* p = PackFormatOwner(i)) *targets[i] = p->formats[i];
}

// [global keys] work while another window is in front, through RegisterHotKey; a key another
// program already holds is reported, not taken.
void RegisterPackHotkeys() {
    for (const auto& [hotkey, command] : s.hotkeys) UnregisterHotKey(g.hwnd, hotkey);
    s.hotkeys.clear();
    if (!g.hwnd) return;
    ForEachOn([&](const Pack& p, size_t) {
        for (const PackKey& k : p.globalKeys) {
            ACCEL a;
            int command = MenuCommandByName(k.command);
            if (!ParseKeys(k.keys, a) || command <= 0) {
                s.keyWarnings.push_back(p.name + L" keys.ini: the global key " + k.keys + L" = " + k.command + L" can't be used");
                continue;
            }
            UINT mods = MOD_NOREPEAT | (a.fVirt & FCONTROL ? MOD_CONTROL : 0) | (a.fVirt & FALT ? MOD_ALT : 0) |
                        (a.fVirt & FSHIFT ? MOD_SHIFT : 0);
            int id = 0x5000 + (int)s.hotkeys.size();
            if (RegisterHotKey(g.hwnd, id, mods, a.key)) s.hotkeys.push_back({id, command});
            else s.keyWarnings.push_back(p.name + L" keys.ini: another program already uses the global key " + k.keys);
        }
    });
}

bool PackHotkey(int id) {
    for (const auto& [hotkey, command] : s.hotkeys)
        if (hotkey == id) {
            OnCommand(command, 0, nullptr);
            return true;
        }
    return false;
}

bool PackField(TrackId id, const std::wstring& name, std::wstring& out) {
    static int depth = 0;  // a field may use another, not itself forever
    if (depth >= 4) return false;
    for (const auto& [field, format] : s.fields)
        if (field == name) {
            ++depth;
            out = FormatTitle(format, Lookup(id, false));
            --depth;
            return true;
        }
    return false;
}

void AppendPackColumnSets(HMENU menu) {
    if (s.columnSets.empty()) return;
    HMENU sub = CreatePopupMenu();
    for (size_t i = 0; i < s.columnSets.size() && i < (size_t)kMaxPackColumnSets; ++i)
        AppendMenuW(sub, MF_STRING, IDM_PACK_COLSET_BASE + i, EscapeAmp(s.columnSets[i]->name).c_str());
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)sub, L"Column &sets");
}

void AppendPackAccelerators(std::vector<ACCEL>& keys) {
    s.keyWarnings.clear();
    ForEachOn([&](const Pack& p, size_t) {
        for (const PackKey& k : p.keys) {
            ACCEL a;
            int command = MenuCommandByName(k.command);
            if (!ParseKeys(k.keys, a))
                s.keyWarnings.push_back(p.name + L" keys.ini: " + k.keys + L" is not a key cordinal can give a pack (letters need Ctrl or Alt)");
            else if (command <= 0)
                s.keyWarnings.push_back(p.name + L" keys.ini: no menu command is called " + k.command +
                                        (command < 0 ? L" (two commands share that name; give its menu too)" : L""));
            else {
                a.cmd = (WORD)command;
                std::erase_if(keys, [&](const ACCEL& o) { return o.fVirt == a.fVirt && o.key == a.key; });
                keys.insert(keys.begin(), a);
            }
        }
    });
}

std::vector<const PackPreset*> PackPresets() {
    std::vector<const PackPreset*> out;
    ForEachOn([&](const Pack& p, size_t) {
        for (const auto& preset : p.presets) out.push_back(&preset);
    });
    return out;
}

void AppendPackAutoPlaylists(HMENU menu) {
    if (s.autoPlaylists.empty()) return;
    HMENU sub = CreatePopupMenu();
    for (size_t i = 0; i < s.autoPlaylists.size() && i < (size_t)kMaxPackAutoPlaylists; ++i)
        AppendMenuW(sub, MF_STRING, IDM_PACK_AUTO_BASE + i, EscapeAmp(s.autoPlaylists[i]->name).c_str());
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)sub, L"Add auto-playlist from a &pack");
}

// The pack theme in use replaces the sound effects it has a sound for; the rest stay cordinal's.
bool PlayPackThemeSound(Sfx sound, float gain) {
    size_t k = (size_t)g.themeMode - (size_t)ThemeMode::Count;
    if (!PackThemeMode(g.themeMode) || k >= s.themes.size()) return false;
    const auto& sounds = s.themes[k]->sounds;
    auto it = std::find_if(sounds.begin(), sounds.end(), [&](auto& x) { return x.first == sound; });
    if (it == sounds.end()) return false;
    State::Clip& c = s.clips[k][(size_t)sound];
    if (!c.tried) {
        c.tried = true;
        if (!DecodePackWav(it->second, c.rate, c.stereo))
            if (auto d = OpenFlacDecoder((const uint8_t*)it->second.data(), it->second.size())) {
                c.rate = d->Rate();
                c.stereo.resize(2 * (size_t)std::llround(std::min(d->Duration(), 10.0) * c.rate));
                c.stereo.resize(2 * d->Read(c.stereo.data(), c.stereo.size() / 2));
            }
    }
    return !c.stereo.empty() && PlayThemeClip(c.stereo, c.rate, gain);
}

bool PackThemeHasSound(Sfx sound) {
    size_t k = (size_t)g.themeMode - (size_t)ThemeMode::Count;
    if (!PackThemeMode(g.themeMode) || k >= s.themes.size()) return false;
    for (const auto& x : s.themes[k]->sounds)
        if (x.first == sound) return true;
    return false;
}

// A pack is a .cordpack, a .zip with a pack.ini in it, or a folder with a pack.ini at its top.
bool LooksLikePack(const std::wstring& path, bool& archive) {
    DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return false;
    archive = !(attributes & FILE_ATTRIBUTE_DIRECTORY);
    if (!archive) return GetFileAttributesW((path + L"\\pack.ini").c_str()) != INVALID_FILE_ATTRIBUTES;
    if (HasExtension(path, L".cordpack")) return true;
    if (!HasExtension(path, L".zip")) return false;
    std::string zip;
    std::vector<PackEntry> files;
    std::wstring error;
    return ReadBytes(path, kMaxPackBytes, zip) && ReadPackZip(zip, files, error) &&
           std::any_of(files.begin(), files.end(), [](auto& f) { return f.path == L"pack.ini"; });
}

bool InstallPack(const std::wstring& source, bool ask) {
    namespace fs = packfiles;
    bool archive = false;
    if (!LooksLikePack(source, archive)) return false;
    std::wstring id = archive ? StemOf(source) : FileNameOf(source);
    auto pack = ReadPackAt(source, id, archive);
    auto say = [&](const std::wstring& text) {
        if (ask && g.hwnd) MessageBoxW(g.hwnd, text.c_str(), L"Packs", MB_OK | MB_ICONINFORMATION);
        else ShowMessage(text);
    };
    if (!pack->error.empty()) {
        say(L"The pack " + id + L" can't be installed: " + pack->error + L".");
        return true;
    }
    DWORD ec = ERROR_SUCCESS;
    const std::wstring dir = PacksDir(), dest = dir + L"\\" + id + (archive ? L".cordpack" : L""),
                       other = dir + L"\\" + id + (archive ? L"" : L".cordpack");
    bool already = fs::Same(DirectoryOf(source), dir);  // dropped from the packs folder itself
    if (ask) {
        std::wstring text = L"Install the pack \"" + pack->name + L"\"" + (pack->version.empty() ? L"" : L" " + pack->version) +
                            (pack->author.empty() ? L"" : L" by " + pack->author) + L"?\n\nIt has " + PackSummary(*pack) + L".";
        if (!pack->description.empty()) text += L"\n\n" + pack->description;
        if (!pack->warnings.empty()) text += L"\n\n" + std::to_wstring(pack->warnings.size()) + L" things in it will be left out (View > Packs > What packs left out).";
        if (!already && (fs::Exists(dest) || fs::Exists(other))) text += L"\n\nIt replaces the pack of the same name you have now.";
        text += L"\n\nPacks are data: they can't run code. This one is copied to your packs folder and turned on; "
                L"View > Packs turns it off again.";
        if (MessageBoxW(g.hwnd, text.c_str(), L"Install a pack", MB_YESNO | MB_ICONQUESTION) != IDYES) return true;
    }
    if (!already) {
        // Copied beside the old one first and swapped in by renames, so a failed copy leaves the
        // installed pack as it was.
        ec = fs::CreateDirectories(dir);
        const std::wstring temp = dir + L"\\~install-" + id, old = dir + L"\\~old-" + id,
                           oldOther = dir + L"\\~old2-" + id;
        if (!ec) ec = fs::Remove(temp);
        if (!ec) ec = fs::Remove(old);
        if (!ec) ec = fs::Remove(oldOther);
        if (!ec) ec = fs::Copy(source, temp);
        bool had = fs::Exists(dest), hadOther = fs::Exists(other);
        if (!ec && had) ec = fs::Move(dest, old);
        if (!ec && hadOther) ec = fs::Move(other, oldOther);
        if (!ec) ec = fs::Move(temp, dest);
        if (ec) {
            if (had && !fs::Exists(dest)) fs::Move(old, dest);
            if (hadOther && !fs::Exists(other)) fs::Move(oldOther, other);
            fs::Remove(temp);
            wchar_t error[512]{};
            FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, ec, 0,
                           error, (DWORD)std::size(error), nullptr);
            size_t length = wcslen(error);
            while (length && (error[length - 1] == L'\r' || error[length - 1] == L'\n')) error[--length] = 0;
            say(L"Couldn't copy the pack " + id + L" to " + PacksDir() + L": " + error);
            return true;
        }
        fs::Remove(old);
        fs::Remove(oldOther);
    }
    auto on = SplitList(IniRead(L"Packs", L"On", L""));
    std::erase_if(on, [&](auto& x) { return _wcsicmp(x.c_str(), id.c_str()) == 0; });
    on.push_back(id);
    IniWrite(L"Packs", L"On", JoinList(on));
    if (s.safe) {
        ShowMessage(L"Installed " + pack->name + L". It turns on when cordinal starts normally.");
        return true;
    }
    ReloadPacks();
    ShowMessage(L"Installed and turned on " + pack->name + L": " + PackSummary(*pack) + L".");
    return true;
}

std::vector<std::wstring> TakePacks(const std::vector<std::wstring>& paths) {
    std::vector<std::wstring> rest;
    for (const auto& p : paths)
        if (!InstallPack(p, true)) rest.push_back(p);
    return rest;
}

std::wstring MakeStarterPack(bool open) {
    namespace fs = packfiles;
    std::wstring id = L"My pack";
    for (int n = 2; fs::Exists(PacksDir() + L"\\" + id) || fs::Exists(PacksDir() + L"\\" + id + L".cordpack"); ++n)
        id = L"My pack " + std::to_wstring(n);
    const std::wstring dir = PacksDir(), temp = dir + L"\\~new-" + id, dest = dir + L"\\" + id;
    DWORD ec = fs::Remove(temp);
    if (!ec) ec = fs::CreateDirectories(temp + L"\\autoplaylists");
    if (!ec) ec = fs::CreateDirectories(temp + L"\\sounds");
    bool ok = !ec;
    auto write = [&](const std::wstring& file, const std::wstring& text) { ok = ok && WriteUtf8File(temp + L"\\" + file, text); };
    auto hex = [](Rgb c) {
        wchar_t b[8];
        swprintf_s(b, L"#%06X", c);
        return std::wstring(b);
    };
    const Palette& p = P();
    const Theme& theme = ThemeInfo(g.themeMode);
    write(L"pack.ini",
          L"; About this pack. Edit any file here, then View > Packs > Reload packs. The README's Packs\n"
          L"; section lists every file and key. Delete the files you don't need: each one is optional.\n"
          L"[pack]\nformat = 1\nname = " + id + L"\nversion = 1.0\nauthor =\ndescription = Made from my cordinal setup.\n");
    std::wstring palette;
    const struct { const wchar_t* key; Rgb Palette::*field; } roles[] = {
        {L"bg", &Palette::bg}, {L"surface", &Palette::surface}, {L"surface2", &Palette::surface2}, {L"border", &Palette::border},
        {L"text", &Palette::text}, {L"sub", &Palette::sub}, {L"faint", &Palette::faint}, {L"accent", &Palette::accent},
        {L"accent2", &Palette::accent2}, {L"hover", &Palette::hover}, {L"select", &Palette::select},
        {L"selectText", &Palette::selectText}, {L"playing", &Palette::playing}, {L"error", &Palette::error},
        {L"amber", &Palette::amber}, {L"ok", &Palette::ok}};
    for (const auto& r : roles) palette += std::wstring(r.key) + L" = " + hex(p.*r.field) + L"\n";
    std::wstring sounds;
    for (int i = 0; i < (int)Sfx::CardinalSong; ++i) sounds += L"; " + std::wstring(PackSoundName((Sfx)i)) + L" = sounds/" + PackSoundName((Sfx)i) + L".wav\n";
    write(L"theme.ini",
          L"; A theme for View > Theme: your current colours. It must stay readable: body text at 7:1 against\n"
          L"; bg and surface, sub and selectText at 4.5:1, accent and the status colours at 3:1.\n"
          L"[theme]\nname = My theme\nbase = " + std::wstring(p.dark ? L"dark" : L"light") + L"\n" +
          (theme.font ? L"font = " + std::wstring(theme.font) + L"\n" : L"; font = Segoe UI\n") +
          (theme.fontPx ? L"size = " + std::to_wstring(theme.fontPx) + L"\n" : L"; size = 13\n") +
          L"playing mark = " + theme.playingMark + L"\n; message prefix = \"> \"\nrating mark = " + theme.ratingMark +
          L"\n\n[palette]\n" + palette +
          L"\n; The waveform's colours; without them it uses accent for what has played.\n[waveform]\n; played = " +
          hex(p.accent) + L"\n; unplayed = " + hex(g.dark ? p.faint : p.border) + L"\n" +
          L"\n; Sounds in place of cordinal's sound effects, as WAV or FLAC files in this pack (at most ten\n"
          L"; seconds each). A file at sounds/<name>.wav is used without a line here.\n[sounds]\n" + sounds);
    write(L"formats.ini",
          L"; Title formats while the pack is on: your current ones. Title formatting (Preferences > View) lists the fields.\n"
          L"[formats]\nwindow title = " + g.fmtWindow + L"\nstatus bar = " + g.fmtStatus + L"\nnow playing = " + g.fmtTitle +
          L"\nnow playing 2 = " + g.fmtSub + L"\n; the notification area icon's tooltip\n; tray tip = [%artist% - ]%title%\n");
    write(L"fields.ini",
          L"; Fields of your own: %decade% then works in columns, formats, group headers and sorting.\n"
          L"[fields]\ndecade = $if(%year%,$div(%year%,10)0s)\n");
    std::wstring shown;
    for (int t : g.columns)
        if (t > 0) shown += (shown.empty() ? L"" : L", ") + PackColumnTitle(t);
    write(L"columns.ini",
          L"; Track list columns: right-click the list's header to show one. Each section is a column.\n"
          L"[stars]\ntitle = Stars\nformat = $stars(%rating_value%)\nsort = [$num(%rating_value%,3)]\nwidth = 80\n\n"
          L"; [bitrate]\n; title = kbps\n; format = [%bitrate%]\n; align = right\n; width = 60\n\n"
          L"; A column set: the header's right-click menu > Column sets shows these columns, left to right.\n"
          L"[set.mine]\nname = My columns\nshow = " + shown + L"\n");
    write(L"functions.tf",
          L"// Your own title-format functions. Inside the body, %n% is the argument called n.\n"
          L"$define(stars, n, $repeat(\x2605, $div(%n%, 20)))\n");
    write(L"groups.ini",
          L"; Album group headers. Remove the ; in front of a line to use it.\n[groups]\n"
          L"; by = %album artist%|%album%\n; title = %album%\n; subtitle = [%album artist%][  \x00B7  %year%]\n"
          L"; right = %group_tracks% tracks  \x00B7  %group_length%\n; cover = yes\n");
    write(L"keys.ini",
          L"; Shortcuts: a key, then a command as the command palette (Ctrl+Shift+P) names it. Letters need\n"
          L"; Ctrl or Alt; F-keys and media keys can stand alone.\n[keys]\n; Ctrl+Alt+N = Playback > Next\n; F9 = Shuffle\n\n"
          L"; Global keys work while another program is in front.\n[global keys]\n; Ctrl+Alt+Right = Playback > Next\n"
          L"; Ctrl+Alt+Space = Play or pause\n");
    std::wstring bands;
    for (int i = 0; i < kEqBands; ++i) bands += (i ? L", " : L"") + F2S(g.eq.gains[i]);
    write(L"eq.ini", L"; Equalizer presets: ten gains in dB, 31 Hz to 16 kHz.\n[my eq]\nname = My EQ\nbands = " + bands + L"\n");
    write(L"layout.ini", L"; A Customize layout preset: your current layout. 1 shows a part, 0 hides it.\n[layout]\nname = My layout\n" +
                             LayoutIniText(g.layout));
    for (const auto& pl : g.playlists) {
        if (pl.kind != PlaylistKind::Auto || !ok) continue;
        std::wstring file = pl.name;
        for (auto& c : file)
            if (wcschr(L"\\/:*?\"<>|", c)) c = L'_';
        write(L"autoplaylists\\" + file + L".txt", L"name " + pl.name + L"\n" + FromUtf8(pl.rules));
    }
    if (ok) ec = fs::Move(temp, dest);
    if (!ok || ec) {
        fs::Remove(temp);
        ShowMessage(L"Couldn't make a pack in " + PacksDir() + L".");
        return L"";
    }
    if (!s.safe) {
        if (g.list) SaveColumns();
        ThemeMemo memo = Remember();
        ReadAll();
        Refresh(memo);
    }
    if (open) ShellExecuteW(g.hwnd, L"open", dest.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    ShowMessage(L"Made the pack \"" + id + L"\" from your setup. Edit its files, then turn it on in View > Packs.");
    return dest;
}

std::vector<const PackLayout*> PackLayoutPresets() {
    std::vector<const PackLayout*> out;
    ForEachOn([&](const Pack& p, size_t) {
        for (const auto& layout : p.layouts) out.push_back(&layout);
    });
    return out;
}

}  // namespace app
