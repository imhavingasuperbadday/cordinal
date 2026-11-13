// Linux pack integration in the existing --check-layout diagnostic, without screenshots. Real desktop
// fonts provide fixtures; every pack and settings write goes to a temporary profile, removed afterward.
#ifndef _WIN32
#include "../linux/app.h"
#include "../linux/dbus.h"
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
#include <poll.h>

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
// The peer speaks through the same client as the player, on its own connection and thread.
struct ShortcutPortal {
    static constexpr const char* kPath = "/org/freedesktop/portal/desktop";
    static constexpr const char* kSession = "/org/freedesktop/portal/desktop/session/mock";
    std::unique_ptr<dbus::Bus> bus;
    std::thread worker;
    std::atomic<bool> stop{false}, valid{false}, deny{false};
    std::string oldAddress;
    bool hadAddress = false;
    int pid = 0;
    std::string shortcut;  // worker only

    void Request(dbus::Bus& b, const dbus::Message& call) {
        bool create = call.member == "CreateSession";
        if (call.path != kPath || call.iface != "org.freedesktop.portal.GlobalShortcuts" ||
            call.sig != (create ? "a{sv}" : "oa(sa{sv})sa{sv}"))
            return b.Error(call, "org.freedesktop.DBus.Error.UnknownMethod", "the mock portal has no such method");
        std::string sender = call.sender.substr(1);
        std::replace(sender.begin(), sender.end(), '.', '_');
        std::string path = "/org/freedesktop/portal/desktop/request/" + sender + (create ? "/create" : "/bind");
        if (!create) {
            dbus::Reader r = call.Body();
            bool ok = r.O() == kSession, trigger = false;
            shortcut.clear();
            size_t list = r.Open(8);
            if (ok && r.In(list)) {
                r.Struct();
                shortcut = r.S();
                for (size_t end = r.Open(8); r.In(end);) {
                    r.Struct();
                    std::string_view key = r.S(), type = r.G();
                    if (key == "preferred_trigger" && type == "s") trigger = r.S() == "CTRL+ALT+k";
                    else r.Skip(type);
                }
            }
            valid = ok && r.ok() && !shortcut.empty() && trigger;
        }
        dbus::Writer handle;
        handle.O(path);
        b.Reply(call, "o", handle);
        dbus::Writer response;
        response.U(deny ? 1 : 0);
        dbus::Writer::Array results = response.Open(8);
        if (create) {
            response.Entry("session_handle", "s");
            response.S(kSession);
        } else {
            response.Entry("shortcuts", "a(sa{sv})");
            dbus::Writer::Array list = response.Open(8);
            response.Align(8);
            response.S(shortcut);
            response.Close(response.Open(8));
            response.Close(list);
        }
        response.Close(results);
        b.Signal(path, "org.freedesktop.portal.Request", "Response", "ua{sv}", response);
        if (!create && !deny) {
            dbus::Writer activated;
            activated.O(kSession);
            activated.S(shortcut);
            activated.X(1);  // the timestamp, a uint64 on the wire
            activated.Close(activated.Open(8));
            b.Signal(kPath, "org.freedesktop.portal.GlobalShortcuts", "Activated", "osta{sv}", activated);
        }
    }

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
        bus = dbus::Bus::OpenUser();
        if (!bus) return false;
        dbus::Writer name;
        name.S("org.freedesktop.portal.Desktop");
        name.U(0);
        auto owned = bus->CallSync({"org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName"},
                                   "su", name, 5000);
        if (!owned || owned->type != dbus::kReturn || owned->Body().U() != 1) return false;  // primary owner
        bus->onMessage = [this](dbus::Bus& b, const dbus::Message& m) {
            if (m.type == dbus::kCall) Request(b, m);
        };
        worker = std::thread([this] {
            while (!stop && bus->Process(64) >= 0) {
                pollfd p{bus->Fd(), bus->Events(), 0};
                poll(&p, 1, 10);
            }
        });
        return true;
    }
    ~ShortcutPortal() {
        StopPackGlobalKeys();
        stop = true;
        if (worker.joinable()) worker.join();
        bus.reset();
        if (pid > 0) kill(pid, SIGTERM);
        if (hadAddress) setenv("DBUS_SESSION_BUS_ADDRESS", oldAddress.c_str(), 1);
        else unsetenv("DBUS_SESSION_BUS_ADDRESS");
    }
};

// A message as the client frames one, with a body supplied byte for byte.
std::string Frame(std::string_view sig, const dbus::Writer& body, int fds = -1) {
    dbus::Writer h;
    h.Y('l'), h.Y(dbus::kSignal), h.Y(0), h.Y(1);
    h.U((uint32_t)body.b.size());
    h.U(7);
    dbus::Writer::Array fields = h.Open(8);
    auto field = [&](uint8_t code, const char* type, std::string_view value) {
        h.Align(8), h.Y(code), h.G(type);
        *type == 'g' ? h.G(value) : h.S(value);
    };
    field(1, "o", "/io/github/cordinal");
    field(2, "s", "io.github.cordinal.Test");
    field(3, "s", "Probe");
    if (!sig.empty()) field(8, "g", sig);
    if (fds >= 0) h.Align(8), h.Y(9), h.G("u"), h.U((uint32_t)fds);
    h.Close(fields);
    h.Align(8);
    return h.b + body.b;
}

int Parse(const std::string& bytes, dbus::Message& m) {
    size_t used = 0;
    return dbus::ParseMessage(bytes, m, used);
}

// The session-bus codec: what it writes reads back, a partial message waits, and every malformed form
// the parser has to refuse closes the stream instead of reaching a handler.
void CheckBusCodec(const std::function<void(bool, const char*)>& check) {
    dbus::Writer body;
    body.S("org.mpris.MediaPlayer2.Player");
    dbus::Writer::Array dict = body.Open(8);
    body.Entry("Volume", "d"), body.D(0.5);
    body.Entry("xesam:artist", "as"), body.Strings({"Artist \xff"});
    body.Close(dict);
    body.Close(body.Open(4));
    std::string bytes = Frame("sa{sv}as", body);
    dbus::Message m;
    size_t used = 0;
    bool parsed = dbus::ParseMessage(bytes, m, used) == 1 && used == bytes.size();
    dbus::Reader r = m.Body();
    std::string_view iface = r.S();
    size_t end = r.Open(8);
    r.Struct();
    std::string_view key = r.S(), type = r.G();
    double volume = r.D();
    r.Struct();
    r.S(), r.G();
    r.Open(4);
    std::string_view artist = r.S();
    check(parsed && m.path == "/io/github/cordinal" && m.member == "Probe" && m.serial == 7 && iface == "org.mpris.MediaPlayer2.Player" &&
              key == "Volume" && type == "d" && volume == 0.5 && artist == "Artist \xEF\xBF\xBD" && r.pos() == end && r.ok(),
          "session-bus messages read back as written, with invalid UTF-8 replaced");
    dbus::Message partial;
    check(Parse(bytes.substr(0, bytes.size() - 1), partial) == 0, "a session-bus message cut short waits for the rest");

    // dbus-daemon's reply to Hello and its NameAcquired signal, byte for byte: every header field a reply
    // or a signal from the bus carries, in the daemon's own order.
    static const char kDaemonHex[] =
        "6c02010109000000010000003d00000006017300040000003a312e30000000000501750001000000080167000173000007017300"
        "140000006f72672e667265656465736b746f702e4442757300000000040000003a312e30006c04010109000000020000008d0000"
        "0001016f00150000002f6f72672f667265656465736b746f702f4442757300000002017300140000006f72672e66726565646573"
        "6b746f702e4442757300000000030173000c0000004e616d6541637175697265640000000006017300040000003a312e30000000"
        "00080167000173000007017300140000006f72672e667265656465736b746f702e4442757300000000040000003a312e3000";
    std::string daemon;
    for (size_t i = 0; kDaemonHex[i] && kDaemonHex[i + 1]; i += 2) daemon += (char)std::stoi(std::string(kDaemonHex + i, 2), nullptr, 16);
    dbus::Message reply, acquired;
    size_t first = 0, second = 0;
    bool both = dbus::ParseMessage(daemon, reply, first) == 1 &&
                dbus::ParseMessage(std::string_view(daemon).substr(first), acquired, second) == 1 && first + second == daemon.size();
    check(both && reply.type == dbus::kReturn && reply.replySerial == 1 && reply.dest == ":1.0" &&
              reply.sender == "org.freedesktop.DBus" && reply.Body().S() == ":1.0" && acquired.type == dbus::kSignal &&
              acquired.member == "NameAcquired" && acquired.path == "/org/freedesktop/DBus",
          "the bus daemon's Hello reply and NameAcquired signal parse field by field");

    auto refused = [&](std::string_view sig, std::string raw, const char* what) {
        dbus::Writer w;
        w.b = std::move(raw);
        dbus::Message out;
        check(Parse(Frame(sig, w), out) < 0, what);
    };
    refused("b", std::string("\x02\0\0\0", 4), "a boolean other than 0 or 1 closes the session bus");
    refused("s", std::string("\x03\0\0\0abcd", 8), "a string without its NUL closes the session bus");
    refused("ay", std::string("\x64\0\0\0\x01", 5), "an array longer than its message closes the session bus");
    refused("yu", std::string("\x01\x01\0\0\x05\0\0\0", 8), "non-zero padding closes the session bus");
    refused("y", std::string("\x01\x02", 2), "bytes past the signature close the session bus");
    refused("h", std::string("\0\0\0\0", 4), "a file descriptor in a signature closes the session bus");
    refused(std::string(70, 'a') + "y", std::string("\0\0\0\0", 4), "a signature nested too deeply closes the session bus");
    dbus::Writer deep, shallow;
    for (int i = 0; i < 70; ++i) deep.G("v");
    deep.G("y"), deep.Y(1);
    for (int i = 0; i < 3; ++i) shallow.G("v");
    shallow.G("y"), shallow.Y(1);
    dbus::Message out;
    check(Parse(Frame("v", deep), out) < 0 && Parse(Frame("v", shallow), out) == 1,
          "variants nested too deeply close the session bus and shallow ones read");
    check(Parse(Frame("", {}, 1), out) < 0 && Parse(Frame("", {}, 0), out) == 1,
          "a message carrying file descriptors closes the session bus; GDBus's count of none does not");
    std::string big = Frame("", {});
    uint32_t huge = 2u << 20;
    memcpy(&big[4], &huge, 4);
    check(Parse(big, out) < 0, "a message over the size cap closes the session bus before its body arrives");
    std::string endian = Frame("", {});
    endian[0] = 'x';
    check(Parse(endian, out) < 0, "an unknown byte order closes the session bus");

    auto addresses = dbus::ParseAddresses("tcp:host=localhost,port=1;unixexec:path=/bin/sh;autolaunch:;unix:dir=/tmp;"
                                          "unix:path=/run/a%zz;unix:path=/run/user/1000/bus,guid=00;unix:abstract=%2Fcordinal");
    check(addresses.size() == 2 && !addresses[0].abstract && addresses[0].path == "/run/user/1000/bus" &&
              addresses[1].abstract && addresses[1].path == "/cordinal",
          "the session bus connects to unix paths and abstract sockets only, never tcp or an exec address");
}

}  // namespace

int CheckLinuxPacks() {
    int failed = 0;
    auto check = [&](bool ok, const char* message) {
        printf("%s %s\n", ok ? "ok  " : "FAIL", message);
        fflush(stdout);
        failed += !ok;
    };
    CheckBusCodec(check);
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
