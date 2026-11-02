// Linux pack integration in the existing --check-layout diagnostic, without screenshots. Real desktop
// fonts provide fixtures; every pack and settings write goes to a temporary profile, removed afterward.
#ifndef _WIN32
#include "../linux/app.h"
#include "../linux/text.h"
#include "../linux/ui_internal.h"
#include "packs.h"

#include <SDL3_ttf/SDL_ttf.h>
#include <fontconfig/fontconfig.h>

#include <cstdio>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <signal.h>
#include <systemd/sd-bus.h>

namespace lx {
namespace {

struct FontFixture {
    std::string bytes;
    std::wstring family;
    int width = 0;
};

FontFixture ReadFont(const char* pattern, bool bold) {
    FontFixture out;
    FcPattern* request = FcNameParse((const FcChar8*)pattern);
    if (!request) return out;
    FcConfigSubstitute(nullptr, request, FcMatchPattern);
    FcDefaultSubstitute(request);
    FcResult result;
    FcPattern* match = FcFontMatch(nullptr, request, &result);
    FcChar8* file = nullptr;
    if (match && FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch) {
        std::ifstream stream((const char*)file, std::ios::binary);
        out.bytes.assign(std::istreambuf_iterator<char>(stream), {});
        if (TTF_Font* font = TTF_OpenFont((const char*)file, 12)) {
            const char* family = TTF_GetFontFamilyName(font);
            if (family) out.family = compat::Widen(family);
            if (bold) TTF_SetFontStyle(font, TTF_STYLE_BOLD);
            TTF_SetFontHinting(font, TTF_HINTING_LIGHT_SUBPIXEL);
            TTF_GetStringSize(font, "Pack sample 123", 0, &out.width, nullptr);
            TTF_CloseFont(font);
        }
    }
    if (match) FcPatternDestroy(match);
    FcPatternDestroy(request);
    return out;
}

bool Write(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(bytes.data(), bytes.size());
    file.close();
    return !file.fail();
}

// A private session bus and portal peer exercise the wire protocol without a desktop consent dialog.
struct ShortcutPortal {
    sd_bus* bus = nullptr;
    sd_bus_slot* slot = nullptr;
    std::thread worker;
    std::atomic<bool> stop{false}, valid{false}, deny{false};
    std::string oldAddress;
    bool hadAddress = false;
    int pid = 0;

    static int Request(sd_bus_message* call, void* data, sd_bus_error*) {
        auto& self = *(ShortcutPortal*)data;
        bool create = std::string_view(sd_bus_message_get_member(call)) == "CreateSession";
        std::string sender = sd_bus_message_get_sender(call) + 1;
        std::replace(sender.begin(), sender.end(), '.', '_');
        std::string path = "/org/freedesktop/portal/desktop/request/" + sender + (create ? "/create" : "/bind");
        if (!create) {
            const char* session = nullptr;
            const char* id = nullptr;
            bool ok = sd_bus_message_read(call, "o", &session) > 0 &&
                std::string_view(session) == "/org/freedesktop/portal/desktop/session/mock" &&
                sd_bus_message_enter_container(call, 'a', "(sa{sv})") > 0 &&
                sd_bus_message_enter_container(call, 'r', "sa{sv}") > 0 &&
                sd_bus_message_read(call, "s", &id) > 0 && id && *id;
            self.shortcut = id ? id : "";
            bool trigger = false;
            if (ok && sd_bus_message_enter_container(call, 'a', "{sv}") > 0) {
                while (sd_bus_message_enter_container(call, 'e', "sv") > 0) {
                    const char* key = nullptr;
                    sd_bus_message_read(call, "s", &key);
                    if (std::string_view(key) == "preferred_trigger") {
                        const char* value = nullptr;
                        if (sd_bus_message_enter_container(call, 'v', "s") > 0 &&
                            sd_bus_message_read(call, "s", &value) > 0) {
                            trigger = std::string_view(value) == "CTRL+ALT+k";
                            sd_bus_message_exit_container(call);
                        }
                    } else sd_bus_message_skip(call, "v");
                    sd_bus_message_exit_container(call);
                }
            }
            self.valid = ok && trigger;
        }
        sd_bus_reply_method_return(call, "o", path.c_str());
        sd_bus_message* response = nullptr;
        sd_bus_message_new_signal(self.bus, &response, path.c_str(), "org.freedesktop.portal.Request", "Response");
        sd_bus_message_append(response, "u", self.deny ? 1u : 0u);
        sd_bus_message_open_container(response, 'a', "{sv}");
        if (create) {
            sd_bus_message_open_container(response, 'e', "sv");
            sd_bus_message_append(response, "s", "session_handle");
            sd_bus_message_open_container(response, 'v', "s");
            sd_bus_message_append(response, "s", "/org/freedesktop/portal/desktop/session/mock");
            sd_bus_message_close_container(response);
            sd_bus_message_close_container(response);
        } else {
            sd_bus_message_open_container(response, 'e', "sv");
            sd_bus_message_append(response, "s", "shortcuts");
            sd_bus_message_open_container(response, 'v', "a(sa{sv})");
            sd_bus_message_open_container(response, 'a', "(sa{sv})");
            sd_bus_message_open_container(response, 'r', "sa{sv}");
            sd_bus_message_append(response, "s", self.shortcut.c_str());
            sd_bus_message_open_container(response, 'a', "{sv}");
            sd_bus_message_close_container(response);
            sd_bus_message_close_container(response);
            sd_bus_message_close_container(response);
            sd_bus_message_close_container(response);
            sd_bus_message_close_container(response);
        }
        sd_bus_message_close_container(response);
        sd_bus_send(self.bus, response, nullptr);
        sd_bus_message_unref(response);
        if (!create && !self.deny)
            sd_bus_emit_signal(self.bus, "/org/freedesktop/portal/desktop", "org.freedesktop.portal.GlobalShortcuts",
                "Activated", "osta{sv}", "/org/freedesktop/portal/desktop/session/mock", self.shortcut.c_str(), uint64_t(1), 0);
        return 1;
    }
    std::string shortcut;  // worker only

    bool Start() {
        if (const char* address = getenv("DBUS_SESSION_BUS_ADDRESS")) hadAddress = true, oldAddress = address;
        FILE* pipe = popen("dbus-daemon --session --fork --print-address=1 --print-pid=1", "r");
        if (!pipe) return false;
        char address[1024]{}, number[64]{};
        bool read = fgets(address, sizeof(address), pipe) && fgets(number, sizeof(number), pipe);
        pclose(pipe);
        if (!read) return false;
        pid = atoi(number);
        address[strcspn(address, "\n")] = 0;
        setenv("DBUS_SESSION_BUS_ADDRESS", address, 1);
        if (sd_bus_open_user(&bus) < 0 || sd_bus_request_name(bus, "org.freedesktop.portal.Desktop", 0) < 0) return false;
        static const sd_bus_vtable table[] = {
            SD_BUS_VTABLE_START(0),
            SD_BUS_METHOD("CreateSession", "a{sv}", "o", Request, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("BindShortcuts", "oa(sa{sv})sa{sv}", "o", Request, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_VTABLE_END
        };
        if (sd_bus_add_object_vtable(bus, &slot, "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.GlobalShortcuts", table, this) < 0) return false;
        worker = std::thread([this] {
            while (!stop) {
                while (sd_bus_process(bus, nullptr) > 0) {}
                sd_bus_wait(bus, 10000);
            }
        });
        return true;
    }
    ~ShortcutPortal() {
        StopPackGlobalKeys();
        stop = true;
        if (worker.joinable()) worker.join();
        sd_bus_slot_unref(slot);
        sd_bus_flush_close_unref(bus);
        if (pid > 0) kill(pid, SIGTERM);
        if (hadAddress) setenv("DBUS_SESSION_BUS_ADDRESS", oldAddress.c_str(), 1);
        else unsetenv("DBUS_SESSION_BUS_ADDRESS");
    }
};

}  // namespace

int CheckLinuxPacks() {
    int failed = 0;
    auto check = [&](bool ok, const char* message) {
        printf("%s %s\n", ok ? "ok  " : "FAIL", message);
        fflush(stdout);
        failed += !ok;
    };
    const FontFixture regular = ReadFont("monospace:weight=regular", false);
    const FontFixture bold = ReadFont("monospace:weight=bold", true);
    if (regular.bytes.empty() || regular.family.empty() || regular.family.size() >= LF_FACESIZE ||
        regular.width <= 0 || bold.bytes.empty() || bold.family != regular.family) {
        check(false, "pack font fixtures are available from the desktop fonts");
        return failed;
    }
    char temporary[] = "/tmp/cordinal-packs-XXXXXX";
    if (!mkdtemp(temporary)) {
        check(false, "pack checks have an isolated temporary profile");
        return failed;
    }
    namespace fs = std::filesystem;
    const fs::path root(temporary), pack = root / "packs" / "test";
    std::error_code error;
    fs::create_directories(pack / "fonts", error);
    bool ready = !error && Write(pack / "pack.ini", "[pack]\nname = Test\n") &&
        Write(pack / "theme.ini", "[theme]\nname = Test\nbase = dark\nfont = " + ToUtf8(regular.family) + "\n") &&
        Write(pack / "fonts/regular.ttf", regular.bytes) && Write(pack / "fonts/bold.otf", bold.bytes) &&
        Write(pack / "fonts/bad.ttf", "invalid font") &&
        Write(pack / "keys.ini", "[keys]\nCtrl+Alt+J = Playback > Shuffle\n") &&
        Write(pack / "columns.ini", "[custom]\ntitle = Pack value\nformat = 42\nsort = 0042\nwidth = 76\nalign = right\n"
            "[set.pack]\nname = Pack columns\nshow = Title, custom\n") &&
        Write(pack / "layout.ini", "[layout]\nname = Quiet\norder = off\nnow_playing = no\npoints = yes\nwaveform_size = tall\n") &&
        Write(pack / "eq.ini", "[curve]\nname = Pack curve\nbands = 3,2,1,0,0,0,0,-1,-2,-3\n");
    check(ready, "pack checks prepare fonts, a shortcut and an EQ preset");
    if (!ready) {
        fs::remove_all(root, error);
        return failed;
    }

    const Settings saved = g.settings;
    const std::wstring dataDir = g.dataDir, settingsPath = g.settingsPath;
    const float scale = g.scale;
    g.settings = Settings{};
    g.dataDir = compat::Widen(temporary);
    g.settingsPath = g.dataDir + L"/settings.ini";
    const size_t builtinPresets = EqPresets().size();
    LoadPacks();
    check(PackCount() == 1 && !PackAt(0).on && !UsesPackFont(Font::Normal) &&
              AllEqPresets().size() == builtinPresets,
          "a discovered pack leaves fonts and EQ presets off until enabled");
    TogglePack(0);
    g.settings.theme = ThemeMode::Count;
    ApplyTheme();
    check(UsesPackFont(Font::Normal) && UsesPackFont(Font::Bold) && UsesPackFont(Font::Small) &&
              UsesPackFont(Font::Title) && !UsesPackFont(Font::Menu) && UiSymbolsAvailable(),
          "enabled pack fonts cover UI text and headings while menus keep the system font and symbols fall back");
    check(TextWidth(Font::Normal, "Pack sample 123") == regular.width &&
              TextWidth(Font::Bold, "Pack sample 123") == bold.width,
          "the pack's regular and bold faces retain their own metrics");
    check(PackAt(0).warnings.size() == 1 && PackAt(0).warnings[0].find(L"fonts/bad.ttf") != std::wstring::npos,
          "an invalid font is reported without preventing the rest of the pack from loading");
    const auto presets = AllEqPresets();
    check(presets.size() == builtinPresets + 1 && std::wstring(presets.back().name) == L"Pack curve" &&
              presets.back().gains[0] == 3 && presets.back().gains[9] == -3,
          "an enabled pack offers its EQ curve after the built-in presets");
    SDL_KeyboardEvent key{};
    key.key = SDLK_J;
    key.mod = SDL_KMOD_CTRL | SDL_KMOD_ALT;
    check(PackKeyDown(key) && g.settings.shuffle == kShuffleTracks,
          "an enabled pack shortcut dispatches the named menu action");

    const int custom = ui::kColumnCount;
    check(ColumnTotal() == ui::kColumnCount + 1 && std::wstring(ColumnAt(custom).name) == L"Pack value" &&
              ColumnAt(custom).align == ui::kAlignRight && ColumnWidth(custom) == S(76) &&
              Cell(0, custom) == "42" && std::wstring(ColumnAt(custom).sort) == L"0042",
          "pack columns provide their heading, DPI width, alignment, display and sort formats");
    check(PackColumnSetNames() == std::vector<std::wstring>{L"Pack columns"} &&
              PackLayoutNames() == std::vector<std::wstring>{L"Quiet"},
          "enabled packs offer named column sets and layout presets");
    ApplyPackColumnSet(0);
    check(g.settings.columns == std::vector<int>({0, 2, custom}),
          "a column set resolves built-in and pack column names in order");
    g.settings.widths.resize(std::max(ColumnTotal(), custom + 1), 0);
    g.settings.widths[custom] = 157;
    SaveSettings();
    const std::string columnKey = PackColumnKey(custom);
    g.settings = Settings{};
    LoadSettings();
    LoadPacks();
    check(g.settings.columns == std::vector<int>({0, 2, PackColumnType(columnKey)}) &&
              PackColumnType(columnKey) >= 0 && g.settings.widths[PackColumnType(columnKey)] == 157,
          "named pack columns and resized widths survive saving and restarting");
    ApplyPackLayout(0);
    DoLayout();
    check(g.settings.layout.hiddenParts == (kPartOrder | kPartNowPlaying) && g.settings.layout.points &&
              g.settings.layout.waveSize == 2 && L.title.w == 0 && L.buttons[kBtnShuffle].w == 0,
          "a chosen pack layout applies visibility, points and waveform size to the real geometry");
    g.settings.layout.hiddenParts = 0;
    g.settings.layout.points = false;
    g.settings.bandWaveSize = 0;
    // A newly enabled earlier pack changes numeric ids; the existing column must retain its identity.
    const fs::path earlier = root / "packs" / "aaa";
    fs::create_directories(earlier, error);
    check(Write(earlier / "pack.ini", "[pack]\nname = Earlier\n") &&
              Write(earlier / "columns.ini", "[other]\nformat = 7\n"),
          "a second pack prepares a different column before the first one");
    g.settings.packsOn = "aaa|test";
    LoadPacks();
    const int remapped = PackColumnType(columnKey);
    check(remapped == custom + 1 && g.settings.columns == std::vector<int>({0, 2, remapped}) &&
              g.settings.widths[remapped] == 157 && Cell(0, remapped) == "42",
          "reloading packs remaps columns and widths by name and invalidates formatted cells");
    fs::remove_all(earlier, error);
    g.settings.packsOn = "test";
    LoadPacks();
    g.settings.theme = ThemeMode::Count;  // restart restores the default theme; select the pack again
    g.scale = 2;
    ApplyTheme();
    check(UsesPackFont(Font::Normal) && TextWidth(Font::Normal, "Pack sample 123") > regular.width &&
              DrawText(Font::Bold, "Pack sample 123", 0, 0, CurrentPalette().text) > 0,
          "pack fonts reopen and shape text at 200 percent scaling");
    g.scale = scale;
    LoadPacks();
    check(g.settings.theme == ThemeMode::Count && UsesPackFont(Font::Normal) &&
              DrawText(Font::Normal, "After reload", 0, 0, CurrentPalette().text) > 0,
          "reloading an active pack safely replaces font bytes and retains the chosen theme");
    TogglePack(0);
    const int shuffle = g.settings.shuffle;
    check(g.settings.theme == ThemeMode::System && !UsesPackFont(Font::Normal) && !UsesPackFont(Font::Bold) &&
              AllEqPresets().size() == builtinPresets && !PackKeyDown(key) && g.settings.shuffle == shuffle &&
              PackAt(0).warnings.empty() && UiSymbolsAvailable() && ColumnTotal() == ui::kColumnCount &&
              g.settings.columns == std::vector<int>({0, 2}) && PackColumnSetNames().empty() && PackLayoutNames().empty(),
          "disabling a pack releases its fonts, shortcut and presets and restores the system theme");

    {
        ShortcutPortal portal;
        const bool started = portal.Start();
        check(started, "global shortcut checks start an isolated session bus and portal peer");
        if (started) {
            check(Write(pack / "keys.ini",
                "[global keys]\nCtrl+Alt+K = Playback > Shuffle\n"),
                "a pack supplies a desktop-wide shortcut");
            g.settings.packsOn = "test";
            g.settings.shuffle = kShuffleOff;
            LoadPacks();
            const uint64_t deadline = SDL_GetTicks() + 3000;
            while (g.settings.shuffle == kShuffleOff && SDL_GetTicks() < deadline) {
                TickPackGlobalKeys();
                SDL_Delay(5);
            }
            check(portal.valid && g.settings.shuffle == kShuffleTracks && PackAt(0).warnings.size() == 1,
                "portal session creation, binding and activation dispatch the pack command");
            TogglePack(0);
            check(!PackGlobalKeysActive(), "disabling the pack disconnects its global shortcut session");
            portal.deny = true;
            TogglePack(0);
            const uint64_t deniedDeadline = SDL_GetTicks() + 3000;
            while (PackAt(0).warnings.size() == 1 && SDL_GetTicks() < deniedDeadline) {
                TickPackGlobalKeys();
                SDL_Delay(5);
            }
            check(PackAt(0).warnings.size() == 2 && g.settings.shuffle == kShuffleTracks,
                "denied portal requests are reported and do not dispatch a command");
            TogglePack(0);
        }
    }

    // A single supplied face still gives headings bold text, and catalog bytes outlive the caller's copy.
    ResetPackFonts();
    {
        std::string bytes = regular.bytes;
        check(AddPackFont(bytes), "a single private font face loads");
    }
    std::wstring family = regular.family;
    for (wchar_t& c : family) c = (wchar_t)towlower(c);
    Theme theme = ThemeInfo(ThemeMode::Dark);
    theme.font = family.c_str();
    check(InitText(g.renderer, scale, theme) && UsesPackFont(Font::Normal) && UsesPackFont(Font::Bold) &&
              DrawText(Font::Bold, "Owned font bytes", 0, 0, DarkPalette().text) > 0,
          "private font matching ignores family case and owns bytes while synthesizing a missing bold face");
    theme.font = L"cordinal absent font";
    check(InitText(g.renderer, scale, theme) && !UsesPackFont(Font::Normal) && UiSymbolsAvailable(),
          "an unavailable theme font falls back to an installed face");
    ResetPackFonts();
    theme.font = regular.family.c_str();
    check(InitText(g.renderer, scale, theme) && !UsesPackFont(Font::Normal) &&
              TextWidth(Font::Normal, "Pack sample 123") == regular.width,
          "an installed custom theme face is selected by its family name without a pack font");

    g.settings = Settings{};
    g.dataDir = dataDir;
    g.settingsPath = settingsPath;
    LoadPacks();
    g.settings = saved;
    ApplyTheme();
    fs::remove_all(root, error);
    check(!error, "the temporary pack profile is removed");
    return failed;
}

}  // namespace lx
#endif
