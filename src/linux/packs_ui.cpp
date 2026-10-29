// Packs on Linux (View > Packs), as the Windows build's app_packs.cpp for what this build can apply: a pack is
// a folder, or a .cordpack (a ZIP), in ~/.local/share/cordinal/packs (docs/plugins.md). They are read and
// checked by packs.cpp (nothing in a pack is ever run); each can be turned on and off, installed from a file or
// a folder, and shown with what it holds and what was left out. While a pack is on, its window title, status
// bar and now playing patterns replace the user's own (which stay saved), its title functions ($define ...)
// and fields join the built-in ones, and its themes with their sounds, auto-playlists, key bindings and EQ
// presets are offered. Columns, global keys, layouts and fonts are read and listed but not applied yet
// (docs/linux-parity.md).
#include "draw.h"
#include "dsp.h"
#include "packs.h"
#include "panels.h"
#include "titleformat.h"

#include <cwctype>

#include <deque>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace lx {
namespace {

constexpr size_t kMaxFile = 8u << 20, kMaxPackBytes = 32u << 20, kMaxFiles = 256, kMaxPacks = 64;

struct Loaded {
    Pack pack;
    bool on = false;
    std::vector<TitleFunction> functions;
};
std::vector<std::unique_ptr<Loaded>> g_packs;

fs::path PacksDir() { return fs::path(compat::Narrow(g.dataDir.c_str())) / "packs"; }

bool ReadBytes(const fs::path& path, size_t max, std::string& out) {
    std::error_code ec;
    auto size = fs::file_size(path, ec);
    if (ec || size > max) return false;
    std::ifstream f(path, std::ios::binary);
    out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return f.good() || f.eof();
}

std::wstring LowerPath(std::string name) {
    std::wstring w = compat::Widen(name.c_str());
    for (auto& c : w) c = (wchar_t)std::towlower(c);
    return w;
}

bool ReadFolder(const fs::path& dir, const std::wstring& prefix, int depth, std::vector<PackEntry>& out, size_t& total, std::wstring& error) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        std::wstring name = LowerPath(entry.path().filename().string());
        if (entry.is_directory(ec)) {
            if (depth < 2 && !ReadFolder(entry.path(), prefix + name + L"/", depth + 1, out, total, error)) return false;
        } else if (out.size() >= kMaxFiles) {
            error = L"it has more than 256 files";
            return false;
        } else {
            PackEntry e{prefix + name, {}};
            if (!ReadBytes(entry.path(), kMaxFile, e.bytes)) {
                error = e.path + L" can't be read, or is larger than 8 MB";
                return false;
            }
            if ((total += e.bytes.size()) > kMaxPackBytes) {
                error = L"it is larger than 32 MB";
                return false;
            }
            out.push_back(std::move(e));
        }
    }
    return true;
}

std::string Lowered(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::unique_ptr<Loaded> ReadPackAt(const fs::path& path, const std::wstring& id, bool archive) {
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
    auto loaded = std::make_unique<Loaded>();
    loaded->pack = error.empty() ? ParsePack(id, files) : Pack{};
    loaded->pack.id = id;
    if (!error.empty()) loaded->pack.name = id, loaded->pack.error = error;
    std::vector<std::wstring> problems;
    loaded->functions = ParseTitleFunctions(loaded->pack.functions, problems);
    for (auto& p : problems) loaded->pack.warnings.push_back(L"functions.tf: " + p);
    return loaded;
}

std::vector<std::wstring> SplitList(const std::string& text) {
    std::vector<std::wstring> out;
    for (size_t i = 0; i < text.size();) {
        size_t end = text.find('|', i);
        if (end == std::string::npos) end = text.size();
        if (end > i) out.push_back(compat::Widen(text.substr(i, end - i).c_str()));
        i = end + 1;
    }
    return out;
}

void SaveOn() {
    std::string list;
    for (auto& p : g_packs)
        if (p->on) list += (list.empty() ? "" : "|") + compat::Narrow(p->pack.id.c_str());
    g.settings.packsOn = list;
    SaveSettings();
}

std::vector<std::wstring> g_themeKeys;  // "pack/theme" of each pack theme, in ThemeMode order from Count
std::deque<std::wstring> g_themeNames;  // their menu names
std::vector<const PackTheme*> g_themes;
std::vector<std::pair<std::wstring, std::wstring>> g_fields;  // a pack's %name% and its format; the later pack's wins
std::vector<const PackAutoPlaylist*> g_autoPlaylists;
bool g_themesApplied = false;

std::wstring ThemeKeyOf(ThemeMode mode) {
    int k = (int)mode - (int)ThemeMode::Count;
    return k >= 0 && k < (int)g_themeKeys.size() ? g_themeKeys[k] : L"";
}

ThemeMode ThemeByKey(const std::wstring& key, ThemeMode fallback) {
    for (size_t k = 0; k < g_themeKeys.size(); ++k)
        if (!key.empty() && _wcsicmp(g_themeKeys[k].c_str(), key.c_str()) == 0) return (ThemeMode)((int)ThemeMode::Count + (int)k);
    return fallback;
}

std::wstring EscapeAmp(const std::wstring& text) {
    std::wstring out;
    for (wchar_t c : text) out += c == L'&' ? L"&&" : std::wstring(1, c);
    return out;
}

// What the packs that are on hold becomes current: their title functions, fields, themes and auto-playlists. The
// theme in use, or the default one, stays chosen by name when a pack comes or goes; one whose pack went away
// falls back to following the system.
// A pack's key, as the Windows build reads keys.ini (app_packs.cpp): "Ctrl+Shift+N", "Alt+F4", "F5". A letter,
// digit or other typed key needs Ctrl or Alt, so a pack can't take typing away.
struct Binding {
    SDL_Keycode key = 0;
    bool ctrl = false, shift = false, alt = false;
    std::wstring command;  // "Playback > Next", or the item's name alone
};
std::vector<Binding> g_bindings;
std::vector<std::pair<const Loaded*, std::wstring>> g_keyWarnings;

bool ParseKeys(std::wstring text, Binding& out) {
    std::replace(text.begin(), text.end(), L'_', L' ');  // the ini reader turns spaces in a key into '_'
    for (size_t at = 0;;) {
        size_t plus = text.find(L'+', at);
        if (plus == text.size() - 1) plus = std::wstring::npos;  // "Ctrl++": the key itself is '+'
        std::wstring part = text.substr(at, plus == std::wstring::npos ? std::wstring::npos : plus - at);
        part.erase(0, part.find_first_not_of(L' '));
        part.erase(part.find_last_not_of(L' ') + 1);
        for (auto& c : part) c = (wchar_t)towlower(c);
        if (plus != std::wstring::npos) {
            if (part == L"ctrl" || part == L"control") out.ctrl = true;
            else if (part == L"shift") out.shift = true;
            else if (part == L"alt") out.alt = true;
            else return false;
            at = plus + 1;
            continue;
        }
        static const struct { const wchar_t* name; SDL_Keycode key; } names[] = {
            {L"space", SDLK_SPACE}, {L"enter", SDLK_RETURN}, {L"tab", SDLK_TAB}, {L"esc", SDLK_ESCAPE}, {L"escape", SDLK_ESCAPE},
            {L"backspace", SDLK_BACKSPACE}, {L"delete", SDLK_DELETE}, {L"del", SDLK_DELETE}, {L"insert", SDLK_INSERT},
            {L"ins", SDLK_INSERT}, {L"home", SDLK_HOME}, {L"end", SDLK_END}, {L"pageup", SDLK_PAGEUP}, {L"pgup", SDLK_PAGEUP},
            {L"pagedown", SDLK_PAGEDOWN}, {L"pgdn", SDLK_PAGEDOWN}, {L"up", SDLK_UP}, {L"down", SDLK_DOWN}, {L"left", SDLK_LEFT},
            {L"right", SDLK_RIGHT}, {L"plus", SDLK_EQUALS}, {L"+", SDLK_EQUALS}, {L"minus", SDLK_MINUS}, {L"-", SDLK_MINUS},
            {L"comma", SDLK_COMMA}, {L",", SDLK_COMMA}, {L"period", SDLK_PERIOD}, {L".", SDLK_PERIOD},
            {L"playpause", SDLK_MEDIA_PLAY_PAUSE}, {L"mediaplaypause", SDLK_MEDIA_PLAY_PAUSE}, {L"medianext", SDLK_MEDIA_NEXT_TRACK},
            {L"mediaprevious", SDLK_MEDIA_PREVIOUS_TRACK}, {L"mediaprev", SDLK_MEDIA_PREVIOUS_TRACK}, {L"mediastop", SDLK_MEDIA_STOP},
            {L"volumeup", SDLK_VOLUMEUP}, {L"volumedown", SDLK_VOLUMEDOWN}, {L"volumemute", SDLK_MUTE}};
        bool function = false, media = false;
        if (part.size() == 1 && iswalnum(part[0])) out.key = (SDL_Keycode)part[0];  // SDL names letters in lowercase
        else if (part.size() >= 2 && part[0] == L'f' && iswdigit(part[1])) {
            int n = (int)wcstol(part.c_str() + 1, nullptr, 10);
            if (n >= 1 && n <= 24) {
                out.key = SDL_GetKeyFromName(("F" + std::to_string(n)).c_str());
                function = true;
            }
        }
        for (const auto& n : names)
            if (part == n.name) {
                out.key = n.key;
                media = n.key == SDLK_MEDIA_PLAY_PAUSE || n.key == SDLK_MEDIA_NEXT_TRACK || n.key == SDLK_MEDIA_PREVIOUS_TRACK ||
                        n.key == SDLK_MEDIA_STOP || n.key == SDLK_VOLUMEUP || n.key == SDLK_VOLUMEDOWN || n.key == SDLK_MUTE;
            }
        return out.key && (out.ctrl || out.alt || function || media);
    }
}

// A menu path as the palette compares them: lowercase, without "..." or '&', with " > " between levels.
std::wstring CommandKey(const std::wstring& s) {
    std::wstring out;
    for (wchar_t ch : s) {
        wchar_t c = ch == L'\x203A' ? L'>' : (wchar_t)towlower(ch);
        if (c == L'&' || c == L'.' || c == L'\x2026') continue;
        if (c == L' ' && (out.empty() || out.back() == L' ' || out.back() == L'>')) continue;
        if (c == L'>' && !out.empty() && out.back() == L' ') out.pop_back();
        out += c;
    }
    while (!out.empty() && out.back() == L' ') out.pop_back();
    return out;
}

// The menu command a key names: its full path, or its name alone when only one item has it. -1: none; -2: two.
int FindCommand(const std::vector<PaletteCommand>& commands, const std::wstring& name) {
    std::wstring want = CommandKey(name);
    // Keep existing pack shortcuts valid after the playback-order command was renamed.
    if (want == L"keep playing like this (radio)" || want == L"playback>keep playing like this (radio)")
        want.erase(want.size() - wcslen(L" (radio)"));
    int found = -1;
    for (size_t i = 0; i < commands.size(); ++i) {
        std::wstring label = CommandKey(compat::Widen(commands[i].label.c_str()));
        if (want == CommandKey(compat::Widen((commands[i].path + " > " + commands[i].label).c_str()))) return (int)i;
        if (want == label) found = found == -1 ? (int)i : -2;
    }
    return found;
}

// The bindings of the packs that are on (a later pack wins a key), and what was wrong with the rest.
void ApplyKeys() {
    g_bindings.clear();
    g_keyWarnings.clear();
    const auto commands = MenuCommands();
    for (auto& p : g_packs) {
        if (!p->on) continue;
        for (const PackKey& k : p->pack.keys) {
            Binding b;
            int command = FindCommand(commands, k.command);
            if (!ParseKeys(k.keys, b))
                g_keyWarnings.push_back({p.get(), L"keys.ini: " + k.keys + L" is not a key cordinal can give a pack (letters need Ctrl or Alt)"});
            else if (command < 0)
                g_keyWarnings.push_back({p.get(), L"keys.ini: no menu command is called " + k.command +
                                                      (command == -2 ? L" (two commands share that name; give its menu too)" : L"")});
            else {
                b.command = k.command;
                std::erase_if(g_bindings, [&](const Binding& o) { return o.key == b.key && o.ctrl == b.ctrl && o.shift == b.shift && o.alt == b.alt; });
                g_bindings.push_back(std::move(b));
            }
        }
        for (const PackKey& k : p->pack.globalKeys)
            g_keyWarnings.push_back({p.get(), L"keys.ini: the global key " + k.keys + L" works only in the Windows build"});
    }
}

void ApplyAll() {
    std::wstring now = ThemeKeyOf(g.settings.theme), def = ThemeKeyOf(g.settings.defaultTheme);
    if (!g_themesApplied) {  // at start, the saved default is the theme in use too
        g_themesApplied = true;
        now = def = FromUtf8(g.settings.packThemeKey);
    }
    std::vector<TitleFunction> functions;
    std::vector<Theme> themes;
    g_themeKeys.clear();
    g_themeNames.clear();
    g_themes.clear();
    g_fields.clear();
    g_autoPlaylists.clear();
    for (auto& p : g_packs) {
        if (!p->on) continue;
        for (const PackAutoPlaylist& a : p->pack.autoPlaylists) g_autoPlaylists.push_back(&a);
        for (const auto& field : p->pack.fields) {
            std::erase_if(g_fields, [&](auto& f) { return f.first == field.first; });
            g_fields.push_back(field);
        }
        for (const PackTheme& t : p->pack.themes) {
            if (themes.size() >= 32) break;
            g_themeKeys.push_back(p->pack.id + L"/" + t.name);
            g_themes.push_back(&t);
            g_themeNames.push_back(EscapeAmp(t.name));
            themes.push_back(Theme{g_themeNames.back().c_str(), &t.palette, t.font.empty() ? nullptr : t.font.c_str(), 0, t.fontPx,
                                   t.mark.c_str(), t.prefix.c_str(), false, t.ratingMark.empty() ? L"\x2605" : t.ratingMark.c_str(),
                                   t.wavePlayed, t.waveRest});
        }
        for (const TitleFunction& f : p->functions) {
            std::erase_if(functions, [&](const TitleFunction& o) { return o.name == f.name; });
            functions.push_back(f);
        }
    }
    SetPackThemes(std::move(themes));
    SetTitleFunctions(std::move(functions));
    ApplyKeys();
    auto resolve = [](const std::wstring& key, ThemeMode current) {
        if (!key.empty()) return ThemeByKey(key, ThemeMode::System);
        return PackThemeMode(current) ? ThemeMode::System : current;  // a pack theme with no key is gone
    };
    g.settings.theme = resolve(now, g.settings.theme);
    g.settings.defaultTheme = resolve(def, g.settings.defaultTheme);
    if (g.window) ApplyTheme();
    ClearTextCache();
    g.redraw = true;
}

}  // namespace

// Reads the packs folder and applies the ones that are on.
void LoadPacks() {
    g_packs.clear();
    std::error_code ec;
    fs::create_directories(PacksDir(), ec);
    std::vector<std::pair<std::string, bool>> found;  // name, a .cordpack
    for (const auto& entry : fs::directory_iterator(PacksDir(), ec)) {
        std::string name = entry.path().filename().string();
        if (entry.is_directory(ec)) found.push_back({name, false});
        else if (Lowered(entry.path().extension().string()) == ".cordpack") found.push_back({entry.path().stem().string(), true});
    }
    std::sort(found.begin(), found.end(), [](auto& a, auto& b) { return Lowered(a.first) < Lowered(b.first); });
    if (found.size() > kMaxPacks) found.resize(kMaxPacks);
    auto on = SplitList(g.settings.packsOn);
    std::string previous;
    for (const auto& [name, archive] : found) {
        if (Lowered(name) == previous) continue;  // a folder and a .cordpack of one name
        previous = Lowered(name);
        std::wstring id = compat::Widen(name.c_str());
        auto pack = ReadPackAt(PacksDir() / (archive ? name + ".cordpack" : name), id, archive);
        pack->on = pack->pack.error.empty() && std::find(on.begin(), on.end(), id) != on.end();
        g_packs.push_back(std::move(pack));
    }
    ApplyAll();
}

// The pack that sets a format slot, or null: the later pack that is on wins.
static const Pack* FormatOwner(int slot) {
    const Pack* owner = nullptr;
    for (auto& p : g_packs)
        if (p->on && !p->pack.formats[slot].empty()) owner = &p->pack;
    return owner;
}

const std::wstring& EffectiveFormat(PackFormatSlot slot, const std::wstring& own) {
    const Pack* owner = FormatOwner(slot);
    return owner ? owner->formats[slot] : own;
}

const wchar_t* PackOwningFormat(PackFormatSlot slot) {
    const Pack* owner = FormatOwner(slot);
    return owner ? owner->name.c_str() : nullptr;
}

// The key of a pack theme, for settings.ini ("pack/theme"), or empty for a built-in one.
std::string PackThemeKey(ThemeMode mode) { return ToUtf8(ThemeKeyOf(mode)); }

const PackTheme* PackThemeFor(ThemeMode mode) {
    size_t k = (size_t)mode - (size_t)ThemeMode::Count;
    return PackThemeMode(mode) && k < g_themes.size() ? g_themes[k] : nullptr;
}

bool PackField(TrackId id, const std::wstring& name, std::wstring& out) {
    static int depth = 0;  // a field may use another, not itself forever
    if (depth >= 4) return false;
    for (const auto& [field, format] : g_fields)
        if (field == name) {
            ++depth;
            out = FormatForTrack(format, id, false);
            --depth;
            return true;
        }
    return false;
}

std::vector<std::wstring> PackAutoPlaylistNames() {
    std::vector<std::wstring> names;
    for (const PackAutoPlaylist* a : g_autoPlaylists) names.push_back(a->name);
    return names;
}

void AddPackAutoPlaylist(size_t index) {
    if (index >= g_autoPlaylists.size()) return;
    const PackAutoPlaylist& a = *g_autoPlaylists[index];
    g.playlists.push_back(Playlist{UniquePlaylistName(g.playlists, a.name), {}, PlaylistKind::Auto, a.rules});
    PlaySfx(Sfx::Sparkle);
    g.libraryDirty = true;
    SetActiveTab((int)g.playlists.size() - 1);
    RebuildRows();
}

bool PackKeyDown(const SDL_KeyboardEvent& e) {
    const bool ctrl = e.mod & SDL_KMOD_CTRL, shift = e.mod & SDL_KMOD_SHIFT, alt = e.mod & SDL_KMOD_ALT;
    for (const Binding& b : g_bindings) {
        if (b.key != e.key || b.ctrl != ctrl || b.shift != shift || b.alt != alt) continue;
        const auto commands = MenuCommands();  // the menus as they are now: an item may be off
        int index = FindCommand(commands, b.command);
        if (index >= 0 && commands[index].enabled) RunMenuCommand((size_t)index);
        return true;
    }
    return false;
}

std::vector<EqPreset> AllEqPresets() {
    std::vector<EqPreset> presets = EqPresets();
    for (auto& p : g_packs)
        if (p->on)
            for (const PackPreset& preset : p->pack.presets) presets.push_back({preset.name.c_str(), preset.gains});
    return presets;
}

size_t PackCount() { return g_packs.size(); }

PackRow PackAt(size_t index) {
    PackRow row;
    if (index >= g_packs.size()) return row;
    const Loaded& l = *g_packs[index];
    row.name = l.pack.name.empty() ? l.pack.id : l.pack.name;
    row.version = l.pack.version;
    row.author = l.pack.author;
    row.description = l.pack.description;
    row.summary = l.pack.error.empty() ? PackSummary(l.pack) : L"";
    row.error = l.pack.error;
    row.warnings = l.pack.warnings;
    for (const auto& [pack, warning] : g_keyWarnings)
        if (pack == &l) row.warnings.push_back(warning);
    row.on = l.on;
    return row;
}

void TogglePack(size_t index) {
    if (index >= g_packs.size()) return;
    Loaded& p = *g_packs[index];
    if (!p.pack.error.empty()) return;
    p.on = !p.on;
    SaveOn();
    ApplyAll();
}

bool InstallPack(const std::wstring& source) {
    std::error_code ec;
    fs::path from(compat::Narrow(source.c_str()));
    bool archive = !fs::is_directory(from, ec);
    if (archive && Lowered(from.extension().string()) != ".cordpack" && Lowered(from.extension().string()) != ".zip") {
        ShowMessage(L"A pack is a .cordpack file or a folder with a pack.ini in it.");
        return false;
    }
    fs::create_directories(PacksDir(), ec);
    fs::path target = PacksDir() / (archive ? from.stem().string() + ".cordpack" : from.filename().string());
    if (fs::exists(target, ec)) {
        ShowMessage(L"A pack of that name is already installed.");
        return false;
    }
    if (archive) fs::copy_file(from, target, ec);
    else fs::copy(from, target, fs::copy_options::recursive, ec);
    if (ec) {
        ShowMessage(L"Couldn't install the pack.");
        return false;
    }
    LoadPacks();
    ShowMessage(L"Installed the pack. Turn it on in View > Packs.");
    return true;
}

void OpenPacksFolder() {
    std::error_code ec;
    fs::create_directories(PacksDir(), ec);
    std::string url = "file://" + PacksDir().string();
    SDL_OpenURL(url.c_str());
}

}  // namespace lx
