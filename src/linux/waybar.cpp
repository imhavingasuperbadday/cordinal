// cordinal's own icon on Waybar, the top bar of Omarchy (and of other Hyprland or Sway desktops that run it).
// Omarchy keeps tray icons behind an expander, so cordinal's tray icon is out of sight until it is
// opened; this module puts a note on the bar itself. Two halves:
//  - `cordinal --waybar` is the module's process: Waybar starts it and reads a JSON line whenever what
//    plays changes. It asks the running cordinal over the session bus (io.github.cordinal.Bar, answered
//    in mpris.cpp) and waits for its BarChanged signal or for the player's name to come or go, so it costs
//    nothing between changes. With no cordinal running it prints empty text and Waybar hides the module.
//  - Preferences > Show on the top bar adds the module to the user's Waybar config (or takes it out again)
//    and asks Waybar to reload. The edit is plain text between two comment markers, so the rest of the file,
//    its comments and its layout stay as they were, and the file is replaced atomically.
#include "waybar.h"

#include "dbus.h"

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace lx::waybar {

namespace {

using dbus::Reader;
using dbus::Writer;

constexpr const char* kBusName = "org.mpris.MediaPlayer2.cordinal";
constexpr const char* kPath = "/org/mpris/MediaPlayer2";
constexpr const char* kApp = "io.github.cordinal";
constexpr dbus::Target kDaemon{"org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", {}};

constexpr std::string_view kModule = "\"custom/cordinal\"";
// Block comments, which (unlike //) also fit in a config written on one line.
constexpr std::string_view kBegin = "/* cordinal's icon (cordinal's Preferences > Show on the top bar adds and removes it) */";
constexpr std::string_view kEnd = "/* end of cordinal's icon */";
constexpr size_t kMaxConfig = 1 << 20;

std::string Json(std::string_view s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') out += '\\', out += (char)c;
        else if (c == '\n') out += "\\n";
        else if (c < 0x20) {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
        } else out += (char)c;
    }
    return out + "\"";
}

bool Space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

size_t SkipSpace(std::string_view s, size_t i) {
    while (i < s.size() && Space(s[i])) ++i;
    return i;
}

// Where the line holding `at` starts, when only spaces come before `at` on it; npos otherwise.
size_t LineStartIfBlank(std::string_view s, size_t at) {
    size_t start = s.rfind('\n', at == 0 ? 0 : at - 1);
    start = start == std::string_view::npos || at == 0 ? 0 : start + 1;
    for (size_t i = start; i < at; ++i)
        if (s[i] != ' ' && s[i] != '\t') return std::string_view::npos;
    return start;
}

// A key such as "modules-right" followed by its colon and an array: the key's offset and the '['.
bool FindArrayKey(std::string_view s, std::string_view name, size_t& key, size_t& bracket) {
    std::string quoted = "\"" + std::string(name) + "\"";
    for (size_t at = s.find(quoted); at != std::string_view::npos; at = s.find(quoted, at + 1)) {
        size_t i = SkipSpace(s, at + quoted.size());
        if (i >= s.size() || s[i] != ':') continue;
        i = SkipSpace(s, i + 1);
        if (i >= s.size() || s[i] != '[') continue;
        key = at, bracket = i;
        return true;
    }
    return false;
}

std::string ShellQuote(std::string_view s) {
    std::string out = "'";
    for (char c : s) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return out + "'";
}

// The command that starts this cordinal again: the AppImage itself when it runs from one (the mounted
// copy goes away when it exits), else this executable.
std::string SelfCommand() {
    if (const char* image = getenv("APPIMAGE"); image && *image == '/') return ShellQuote(image);
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    return n > 0 ? ShellQuote(std::string_view(exe, (size_t)n)) : "cordinal";
}

bool ReadFile(const std::string& path, std::string& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[16384];
    size_t n;
    out.clear();
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && out.size() <= kMaxConfig) out.append(buf, n);
    bool ok = !ferror(f) && out.size() <= kMaxConfig;
    fclose(f);
    return ok;
}

// Written beside the original and renamed over it, so a failure leaves the original untouched; a link to
// the config (a dotfiles checkout) is followed, not replaced.
bool WriteAtomic(const std::string& path, std::string_view text) {
    char real[PATH_MAX];
    if (!realpath(path.c_str(), real)) return false;
    struct stat st{};
    if (stat(real, &st) != 0) return false;
    std::string tmp = std::string(real) + ".cordinal-new";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, st.st_mode & 07777);
    if (fd < 0) return false;
    bool ok = true;
    for (size_t done = 0; ok && done < text.size();) {
        ssize_t n = write(fd, text.data() + done, text.size() - done);
        if (n < 0 && errno == EINTR) continue;
        ok = n > 0;
        if (ok) done += (size_t)n;
    }
    ok = ok && fsync(fd) == 0;
    ok = close(fd) == 0 && ok;
    if (ok && rename(tmp.c_str(), real) == 0) return true;
    unlink(tmp.c_str());
    return false;
}

// SIGUSR2 makes Waybar read its config again; only this user's Waybar processes get it.
void ReloadWaybar() {
    DIR* proc = opendir("/proc");
    if (!proc) return;
    while (dirent* e = readdir(proc)) {
        char* end = nullptr;
        long pid = strtol(e->d_name, &end, 10);
        if (pid <= 0 || *end) continue;
        std::string dir = std::string("/proc/") + e->d_name;
        struct stat st{};
        if (stat(dir.c_str(), &st) != 0 || st.st_uid != getuid()) continue;
        std::string comm;
        if (!ReadFile(dir + "/comm", comm) || comm != "waybar\n") continue;
        kill((pid_t)pid, SIGUSR2);
    }
    closedir(proc);
}

// One line for Waybar: the note while cordinal runs, nothing (which hides the module) while it doesn't.
std::string Query(dbus::Bus& bus) {
    auto reply = bus.CallSync({kBusName, kPath, kApp, "Bar"}, "", {}, 5000);
    if (!reply || reply->type != dbus::kReturn || reply->sig != "ss") return "{\"text\":\"\"}";
    Reader r = reply->Body();
    std::string state(r.S()), tip(r.S());
    std::string cls = state == "Playing" ? "playing" : state == "Paused" ? "paused" : "stopped";
    // Nerd Font glyphs, which Omarchy's bar font has: a note (md-music), or a pause sign while paused.
    const char* glyph = state == "Paused" ? "\U000F03E4" : "\U000F075A";
    return "{\"text\":" + Json(glyph) + ",\"alt\":" + Json(cls) + ",\"class\":" + Json(cls) +
           ",\"tooltip\":" + Json(tip.empty() ? std::string("cordinal") : "cordinal\n" + tip) + "}";
}

}  // namespace

int RunModule() {
    signal(SIGPIPE, SIG_DFL);  // Waybar gone: so is the module
    auto bus = dbus::Bus::OpenUser();
    if (!bus) {
        puts("{\"text\":\"\"}");
        return 1;  // the config's restart-interval tries again
    }
    bool dirty = true;
    bus->onMessage = [&](dbus::Bus&, const dbus::Message& m) {
        if (m.type == dbus::kSignal) dirty = true;  // re-asked, so a forged signal only costs a call
    };
    dbus::Target match = kDaemon;
    match.member = "AddMatch";
    for (const char* rule : {"type='signal',sender='org.freedesktop.DBus',member='NameOwnerChanged',"
                             "arg0='org.mpris.MediaPlayer2.cordinal'",
                             "type='signal',interface='io.github.cordinal',member='BarChanged'"}) {
        Writer w;
        w.S(rule);
        bus->Send(match, "s", w);
    }
    std::string last;
    for (;;) {
        int more = bus->Process(64);
        if (more < 0) {
            puts("{\"text\":\"\"}");
            return 1;
        }
        if (dirty) {
            dirty = false;
            std::string line = Query(*bus);
            if (line != last) {
                last = line;
                if (puts(line.c_str()) < 0 || fflush(stdout) != 0) return 0;
            }
            continue;  // the call may have queued signals
        }
        if (more > 0) continue;
        bus->Flush();
        pollfd p{bus->Fd(), bus->Events(), 0};
        poll(&p, 1, bus->TimeoutMs());
    }
}

std::string ConfigPath() {
    const char* cfg = getenv("XDG_CONFIG_HOME");
    const char* home = getenv("HOME");
    std::string dir = (cfg && *cfg == '/' ? std::string(cfg) : std::string(home ? home : "/tmp") + "/.config") + "/waybar/";
    for (const char* name : {"config", "config.jsonc"})  // Waybar's own order
        if (access((dir + name).c_str(), R_OK) == 0) return dir + name;
    return {};
}

bool Has(std::string_view config) { return config.find(kModule) != std::string_view::npos; }

std::optional<std::string> Add(std::string_view config, std::string_view command) {
    if (Has(config)) return std::string(config);
    size_t key = 0, bracket = 0;
    if (!FindArrayKey(config, "modules-right", key, bracket) && !FindArrayKey(config, "modules-center", key, bracket) &&
        !FindArrayKey(config, "modules-left", key, bracket))
        return std::nullopt;
    std::string out(config);
    // First in the list, beside Omarchy's tray expander, in the list's own layout.
    size_t first = SkipSpace(config, bracket + 1);
    std::string entry;
    if (first < config.size() && config[first] == ']') entry = std::string(kModule);
    else if (config.substr(bracket + 1, first - bracket - 1).find('\n') != std::string_view::npos)
        entry = "\n" + std::string(config.substr(config.rfind('\n', first) + 1, first - config.rfind('\n', first) - 1)) +
                std::string(kModule) + ",";
    else entry = std::string(kModule) + ", ";
    out.insert(bracket + 1, entry);
    // The definition just before the list's key, at its indent (or inline, when the key shares its line).
    size_t lineStart = LineStartIfBlank(config, key);
    std::string indent = lineStart == std::string_view::npos ? std::string() : std::string(config.substr(lineStart, key - lineStart));
    std::string in = indent + (indent.find('\t') != std::string::npos ? "\t" : "  ");
    std::string q = std::string(command);
    std::string def = std::string(kModule) + ": {\n" +
                      in + "\"exec\": " + Json(q + " --waybar") + ",\n" +
                      in + "\"return-type\": \"json\",\n" +
                      in + "\"restart-interval\": 10,\n" +
                      in + "\"hide-empty-text\": true,\n" +
                      in + "\"escape\": true,\n" +
                      in + "\"format\": \" {} \",\n" +
                      in + "\"on-click\": " + Json(q + " --show") + ",\n" +
                      in + "\"on-click-middle\": " + Json(q + " --next") + ",\n" +
                      in + "\"on-click-right\": " + Json(q + " --play-pause") + "\n" +
                      indent + "},";
    if (lineStart == std::string_view::npos)
        out.insert(key, std::string(kBegin) + " " + def + " " + std::string(kEnd) + " ");
    else
        out.insert(lineStart, indent + std::string(kBegin) + "\n" + indent + def + "\n" + indent + std::string(kEnd) + "\n");
    return out;
}

std::string Remove(std::string_view config) {
    std::string s(config);
    // The definition, markers included.
    for (size_t b = s.find(kBegin); b != std::string::npos; b = s.find(kBegin)) {
        size_t e = s.find(kEnd, b);
        if (e == std::string::npos) break;
        size_t start = LineStartIfBlank(s, b);
        size_t stop = e + kEnd.size();
        if (start != std::string::npos && stop < s.size() && s[stop] == '\n') ++stop;  // its own lines
        else if (stop < s.size() && s[stop] == ' ') ++stop;                            // inline
        if (start == std::string::npos) start = b;
        s.erase(start, stop - start);
    }
    // The list entries: whatever is left named "custom/cordinal" that isn't a key.
    for (size_t p = s.find(kModule); p != std::string::npos; p = s.find(kModule, p)) {
        size_t after = SkipSpace(s, p + kModule.size());
        if (after < s.size() && s[after] == ':') {
            p += kModule.size();
            continue;
        }
        if (after < s.size() && s[after] == ',') {
            size_t line = LineStartIfBlank(s, p);
            size_t rest = after + 1;
            while (rest < s.size() && (s[rest] == ' ' || s[rest] == '\t' || s[rest] == '\r')) ++rest;
            if (line != std::string::npos && rest < s.size() && s[rest] == '\n' && line > 0) {
                s.erase(line - 1, rest - line + 1);  // the entry's whole line, with the newline before it
                p = line - 1;
            } else {
                s.erase(p, rest - p);
            }
            continue;
        }
        size_t comma = p;  // the last entry: the comma before it goes
        while (comma > 0 && Space(s[comma - 1])) --comma;
        if (comma > 0 && s[comma - 1] == ',') --comma;
        else comma = p;
        s.erase(comma, p + kModule.size() - comma);
        p = comma;
    }
    return s;
}

bool Shown() {
    std::string path = ConfigPath(), text;
    return !path.empty() && ReadFile(path, text) && Has(text);
}

bool SetShown(bool shown) {
    std::string path = ConfigPath(), text;
    if (path.empty() || !ReadFile(path, text)) return false;
    std::optional<std::string> next = shown ? Add(text, SelfCommand()) : std::optional<std::string>(Remove(text));
    if (!next) return false;
    if (*next == text) return true;
    if (!WriteAtomic(path, *next)) return false;
    ReloadWaybar();
    return true;
}

}  // namespace lx::waybar
