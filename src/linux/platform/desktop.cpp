// Desktop services: file dialogs through the XDG desktop portal's FileChooser (with the in-window
// chooser when there is no portal), message boxes, opening URLs (the portal's OpenURI, then
// xdg-open), the XDG user folders and the portal's colour-scheme setting.
#include "platform/platform.h"

#include "dbus.h"

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

extern char** environ;

namespace lx::platform {
namespace {

constexpr const char* kPortal = "org.freedesktop.portal.Desktop";
constexpr const char* kPortalPath = "/org/freedesktop/portal/desktop";
const dbus::Target kAddMatch{"org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "AddMatch"};

void AddMatch(dbus::Bus& bus, const std::string& rule) {
    dbus::Writer w;
    w.S(rule);
    bus.Send(kAddMatch, "s", w);
}

std::string RequestPrefix(const dbus::Bus& bus) {
    std::string sender = bus.Unique().substr(1);
    for (char& c : sender)
        if (c == '.') c = '_';
    return std::string("/org/freedesktop/portal/desktop/request/") + sender + "/";
}

// --- file dialogs ---------------------------------------------------------------------------------------------

struct DialogJob {
    SDL_DialogFileCallback callback;
    void* userdata;
    ChooserRequest request;
    SDL_WindowID window = 0;
    std::string parent;  // the portal's parent_window
};

void Deliver(const DialogJob& job, const std::vector<std::string>* paths) {
    if (!paths) {
        job.callback(job.userdata, nullptr, -1);
        return;
    }
    std::vector<const char*> list;
    for (const std::string& p : *paths) list.push_back(p.c_str());
    list.push_back(nullptr);
    job.callback(job.userdata, list.data(), -1);
}

int HexDigit(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

// file:///a%20b -> /a b; nullopt for anything but a local file.
std::optional<std::string> UriToPath(std::string_view uri) {
    if (uri.rfind("file://", 0) != 0) return std::nullopt;
    uri.remove_prefix(7);
    if (uri.rfind("localhost/", 0) == 0) uri.remove_prefix(9);
    if (uri.empty() || uri[0] != '/') return std::nullopt;
    std::string out;
    for (size_t i = 0; i < uri.size(); ++i) {
        if (uri[i] == '%') {
            if (i + 2 >= uri.size()) return std::nullopt;
            int hi = HexDigit(uri[i + 1]), lo = HexDigit(uri[i + 2]);
            if (hi < 0 || lo < 0) return std::nullopt;
            char c = (char)(hi * 16 + lo);
            if (c == 0) return std::nullopt;
            out += c;
            i += 2;
        } else {
            out += uri[i];
        }
    }
    return out;
}

void FallBack(std::shared_ptr<DialogJob> job) {
    RunOnMainThread([job] {
        SDL_Window* w = WindowById(job->window);  // it may have closed meanwhile
        std::optional<std::vector<std::string>> chosen = OverlayFileChooser(w, job->request);
        if (!chosen) {
            SetError("No file dialog is available (no desktop portal, and no window to draw one in)");
            Deliver(*job, nullptr);
        } else {
            Deliver(*job, &*chosen);
        }
    });
}

// One dialog on its own thread and bus connection: FileChooser call, then its Request's Response.
void PortalDialog(std::shared_ptr<DialogJob> job) {
    std::unique_ptr<dbus::Bus> bus = dbus::Bus::OpenUser(3000);
    if (!bus) return FallBack(job);
    static std::atomic<unsigned> counter{0};
    const std::string token = "cordinal" + std::to_string(++counter);
    std::string handle = RequestPrefix(*bus) + token;
    AddMatch(*bus, "type='signal',interface='org.freedesktop.portal.Request',member='Response'");

    const ChooserRequest& rq = job->request;
    dbus::Writer w;
    w.S(job->parent);
    w.S(rq.title);
    dbus::Writer::Array options = w.Open(8);
    w.Entry("handle_token", "s");
    w.S(token);
    w.Entry("modal", "b");
    w.B(job->window != 0);
    if (rq.many) w.Entry("multiple", "b"), w.B(true);
    if (rq.kind == ChooserKind::OpenFolder) w.Entry("directory", "b"), w.B(true);
    if (!rq.filters.empty()) {
        w.Entry("filters", "a(sa(us))");
        dbus::Writer::Array list = w.Open(8);
        for (const auto& [name, pattern] : rq.filters) {
            w.Align(8);
            w.S(name);
            dbus::Writer::Array globs = w.Open(8);
            for (size_t i = 0; i <= pattern.size();) {
                size_t j = pattern.find(';', i);
                if (j == std::string::npos) j = pattern.size();
                std::string one = pattern.substr(i, j - i);
                i = j + 1;
                if (one.empty()) continue;
                w.Align(8);
                w.U(0);
                w.S(one == "*" ? "*" : "*." + one);
            }
            w.Close(globs);
        }
        w.Close(list);
    }
    if (!rq.location.empty()) {
        w.Entry("current_folder", "ay");
        dbus::Writer::Array bytes = w.Open(1);
        for (char c : rq.location) w.Y((uint8_t)c);
        w.Y(0);
        w.Close(bytes);
    }
    w.Close(options);
    const char* method = rq.kind == ChooserKind::SaveFile ? "SaveFile" : "OpenFile";
    std::optional<dbus::Message> reply =
        bus->CallSync({kPortal, kPortalPath, "org.freedesktop.portal.FileChooser", method}, "ssa{sv}", w, 30000);
    if (!reply || reply->type != dbus::kReturn || reply->sig != "o") return FallBack(job);
    const std::string portal = reply->sender;
    if (std::string_view h = reply->Body().O(); !h.empty()) handle = std::string(h);

    bool done = false;
    bus->onMessage = [&](dbus::Bus&, const dbus::Message& m) {
        if (done || m.type != dbus::kSignal || m.sender != portal || m.path != handle || m.member != "Response" ||
            m.iface != "org.freedesktop.portal.Request" || m.sig != "ua{sv}")
            return;
        done = true;
        dbus::Reader r = m.Body();
        uint32_t response = r.U();
        if (response == 1 || response == 2) {  // cancelled, or ended some other way: as SDL, an empty list
            std::vector<std::string> none;
            return Deliver(*job, &none);
        }
        if (response != 0) {
            SetError("The file dialog failed");
            return Deliver(*job, nullptr);
        }
        std::vector<std::string> paths;
        for (size_t end = r.Open(8); r.In(end);) {
            r.Struct();
            std::string_view key = r.S(), type = r.G();
            if (key == "uris" && type == "as") {
                for (size_t list = r.Open(4); r.In(list);) {
                    std::optional<std::string> p = UriToPath(r.S());
                    if (!p) {
                        SetError("Portal dialogs: Unsupported protocol");
                        return Deliver(*job, nullptr);
                    }
                    paths.push_back(*p);
                }
            } else {
                r.Skip(type);
            }
        }
        if (!r.ok()) return Deliver(*job, nullptr);
        Deliver(*job, &paths);
    };
    while (!done) {
        if (bus->Process(64) < 0) {
            SetError("The desktop portal went away");
            return Deliver(*job, nullptr);
        }
        pollfd p{bus->Fd(), bus->Events(), 0};
        if (poll(&p, 1, -1) < 0 && errno != EINTR) return Deliver(*job, nullptr);
    }
}

std::mutex g_portalMx;
int g_portalChecked = 0;  // 0 unknown, 1 there, -1 not

// Whether the portal offers FileChooser (it starts on demand, so ask it rather than look for it).
bool PortalFileChooser() {
    std::lock_guard lock(g_portalMx);
    if (g_portalChecked) return g_portalChecked > 0;
    g_portalChecked = -1;
    if (std::unique_ptr<dbus::Bus> bus = dbus::Bus::OpenUser(2000)) {
        dbus::Writer w;
        w.S("org.freedesktop.portal.FileChooser");
        w.S("version");
        auto r = bus->CallSync({kPortal, kPortalPath, "org.freedesktop.DBus.Properties", "Get"}, "ss", w, 5000);
        if (r && r->type == dbus::kReturn) g_portalChecked = 1;
    }
    return g_portalChecked > 0;
}

// The integration test (linux/e2e-test.sh) stands in for a person at the dialogs through files:
// CORDINAL_TEST_DIALOG_FILE's first line is the path a file dialog returns (none: cancelled), given a
// second later while FILE.delay exists; FILE.button's first line is the label of the message-box button
// to press, else the Return-key default is.
const char* TestDialogFile() {
    const char* file = getenv("CORDINAL_TEST_DIALOG_FILE");
    return file && *file ? file : nullptr;
}

std::string FirstLine(const std::string& path) {
    std::string line;
    if (FILE* f = fopen(path.c_str(), "rb")) {
        char buf[4096];
        if (fgets(buf, sizeof buf, f)) line = buf;
        fclose(f);
    }
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    return line;
}

void TestDialog(std::shared_ptr<DialogJob> job, std::string file) {
    std::thread([job, file] {
        if (access((file + ".delay").c_str(), F_OK) == 0) std::this_thread::sleep_for(std::chrono::seconds(1));
        std::string path = FirstLine(file);
        if (path.empty()) return Deliver(*job, nullptr);
        std::vector<std::string> paths{path};
        Deliver(*job, &paths);
    }).detach();
}

void StartDialog(SDL_DialogFileCallback callback, void* userdata, SDL_Window* window, ChooserRequest request) {
    if (!callback) return;
    auto job = std::make_shared<DialogJob>();
    job->callback = callback;
    job->userdata = userdata;
    job->request = std::move(request);
    job->window = window ? window->id : 0;
    if (window && window->native) job->parent = window->native->PortalHandle();
    if (const char* test = TestDialogFile()) return TestDialog(job, test);
    if (const char* v = getenv("CORDINAL_FILE_DIALOG"); v && strcmp(v, "builtin") == 0) return FallBack(job);
    std::thread([job] {
        if (PortalFileChooser()) PortalDialog(job);
        else FallBack(job);
    }).detach();
}

ChooserRequest Request(ChooserKind kind, const char* title, const SDL_DialogFileFilter* filters, int n,
                       const char* location, bool many) {
    ChooserRequest r;
    r.kind = kind;
    r.title = title;
    r.many = many;
    if (location) r.location = location;
    for (int i = 0; filters && i < n; ++i)
        if (filters[i].name && filters[i].pattern) r.filters.emplace_back(filters[i].name, filters[i].pattern);
    return r;
}

// --- the colour scheme ----------------------------------------------------------------------------------------

std::atomic<int> g_scheme{-1};  // the portal's value (0 none, 1 dark, 2 light); -1 before the first read
std::once_flag g_schemeOnce;

// Settings.ReadOne's reply is v; the older Read's, which some portals still only have, is v(v).
int ReadScheme(dbus::Bus& bus) {
    for (const char* method : {"ReadOne", "Read"}) {
        dbus::Writer w;
        w.S("org.freedesktop.appearance");
        w.S("color-scheme");
        auto r = bus.CallSync({kPortal, kPortalPath, "org.freedesktop.portal.Settings", method}, "ss", w, 2000);
        if (!r || r->type != dbus::kReturn || r->sig != "v") continue;
        dbus::Reader rd = r->Body();
        std::string_view sig = rd.G();
        if (sig == "v") sig = rd.G();
        if (sig == "u") {
            uint32_t v = rd.U();
            return rd.ok() && v <= 2 ? (int)v : 0;
        }
    }
    return 0;
}

void WatchScheme() {
    std::unique_ptr<dbus::Bus> bus = dbus::Bus::OpenUser(2000);
    if (!bus) {
        g_scheme = 0;
        return;
    }
    g_scheme = ReadScheme(*bus);
    std::thread([b = bus.release()] {
        std::unique_ptr<dbus::Bus> bus(b);
        AddMatch(*bus, "type='signal',interface='org.freedesktop.portal.Settings',member='SettingChanged',"
                       "arg0='org.freedesktop.appearance',arg1='color-scheme'");
        bus->onMessage = [](dbus::Bus&, const dbus::Message& m) {
            if (m.type != dbus::kSignal || m.iface != "org.freedesktop.portal.Settings" || m.member != "SettingChanged" ||
                m.path != kPortalPath || m.sig != "ssv")
                return;
            dbus::Reader r = m.Body();
            if (r.S() != "org.freedesktop.appearance" || r.S() != "color-scheme" || r.G() != "u") return;
            uint32_t v = r.U();
            if (!r.ok()) return;
            int now = v <= 2 ? (int)v : 0;
            if (g_scheme.exchange(now) != now) OnThemeChanged();
        };
        for (;;) {
            if (bus->Process(64) < 0) return;
            pollfd p{bus->Fd(), bus->Events(), 0};
            if (poll(&p, 1, -1) < 0 && errno != EINTR) return;
        }
    }).detach();
}

// --- URLs -----------------------------------------------------------------------------------------------------

bool SpawnXdgOpen(const std::string& url) {
    // Not with the player's preload (an audio or overlay library meant for this process).
    std::vector<std::string> env;
    for (char** e = environ; e && *e; ++e)
        if (strncmp(*e, "LD_PRELOAD=", 11) != 0) env.emplace_back(*e);
    std::vector<char*> envp;
    for (std::string& e : env) envp.push_back(e.data());
    envp.push_back(nullptr);
    std::string prog = "xdg-open", arg = url;
    char* argv[] = {prog.data(), arg.data(), nullptr};
    pid_t pid = 0;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    int rc = posix_spawnp(&pid, "xdg-open", &fa, nullptr, argv, envp.data());
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) return false;
    std::thread([pid] {
        int status;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }).detach();
    return true;
}

bool PortalOpenUri(const std::string& url, const std::string& parent) {
    std::unique_ptr<dbus::Bus> bus = dbus::Bus::OpenUser(2000);
    if (!bus) return false;
    dbus::Writer w;
    w.S(parent);
    w.S(url);
    dbus::Writer::Array options = w.Open(8);
    w.Entry("handle_token", "s");
    w.S("cordinalurl");
    w.Close(options);
    auto r = bus->CallSync({kPortal, kPortalPath, "org.freedesktop.portal.OpenURI", "OpenURI"}, "ssa{sv}", w, 10000);
    return r && r->type == dbus::kReturn;
}

}  // namespace
}  // namespace lx::platform

using namespace lx::platform;

void SDL_ShowOpenFileDialog(SDL_DialogFileCallback callback, void* userdata, SDL_Window* window,
                            const SDL_DialogFileFilter* filters, int nfilters, const char* location, bool many) {
    StartDialog(callback, userdata, window, Request(ChooserKind::OpenFile, "Open File", filters, nfilters, location, many));
}

void SDL_ShowSaveFileDialog(SDL_DialogFileCallback callback, void* userdata, SDL_Window* window,
                            const SDL_DialogFileFilter* filters, int nfilters, const char* location) {
    StartDialog(callback, userdata, window, Request(ChooserKind::SaveFile, "Save File", filters, nfilters, location, false));
}

void SDL_ShowOpenFolderDialog(SDL_DialogFileCallback callback, void* userdata, SDL_Window* window, const char* location,
                              bool many) {
    StartDialog(callback, userdata, window, Request(ChooserKind::OpenFolder, "Open Folder", nullptr, 0, location, many));
}

bool SDL_ShowMessageBox(const SDL_MessageBoxData* data, int* buttonid) {
    if (!data) return SDL_SetError("Parameter 'messageboxdata' is invalid");
    int chosen = -1;
    if (const char* test = TestDialogFile()) {
        const std::string label = FirstLine(std::string(test) + ".button");
        for (int i = 0; i < data->numbuttons && chosen < 0; ++i)
            if (data->buttons[i].text && label == data->buttons[i].text) chosen = data->buttons[i].buttonID;
        for (int i = 0; i < data->numbuttons && chosen < 0; ++i)
            if (data->buttons[i].flags & SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT) chosen = data->buttons[i].buttonID;
        if (chosen < 0 && data->numbuttons > 0) chosen = data->buttons[0].buttonID;
        if (buttonid) *buttonid = chosen;
        return true;
    }
    if (OverlayMessageBox(*data, chosen)) {
        if (buttonid) *buttonid = chosen;
        return true;
    }
    // No window to draw in: say it on stderr, as the message is all that's left.
    fprintf(stderr, "%s: %s\n", data->title ? data->title : "cordinal", data->message ? data->message : "");
    return SDL_SetError("No window to show a message box in");
}

bool SDL_ShowSimpleMessageBox(SDL_MessageBoxFlags flags, const char* title, const char* message, SDL_Window* window) {
    SDL_MessageBoxButtonData ok{SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "OK"};
    SDL_MessageBoxData data{flags, window, title, message, 1, &ok, nullptr};
    int button = 0;
    return SDL_ShowMessageBox(&data, &button);
}

bool SDL_OpenURL(const char* url) {
    if (!url || !*url) return SDL_SetError("Parameter 'url' is invalid");
    std::string u = url, parent;
    if (SDL_Window* w = WindowById(1); w && w->native) parent = w->native->PortalHandle();
    // The portal can't open file:// URIs (that takes a file descriptor); xdg-open can.
    const bool file = u.rfind("file:", 0) == 0;
    if (getenv("CORDINAL_OPEN_URL_LOG")) {  // the tests' way to see what would open, without opening it
        if (FILE* f = fopen(getenv("CORDINAL_OPEN_URL_LOG"), "a")) {
            fprintf(f, "%s\n", url);
            fclose(f);
        }
        return true;
    }
    std::thread([u, parent, file] {
        if (!file && PortalOpenUri(u, parent)) return;
        if (!SpawnXdgOpen(u)) fprintf(stderr, "cordinal: couldn't open %s (no desktop portal and no xdg-open)\n", u.c_str());
    }).detach();
    return true;
}

const char* SDL_GetUserFolder(SDL_Folder folder) {
    static std::string result;  // as SDL's, valid until the next call
    const char* home = getenv("HOME");
    const char* type = nullptr;
    switch (folder) {
    case SDL_FOLDER_HOME:
        if (!home) return SDL_SetError("No $HOME environment variable available"), nullptr;
        return (result = std::string(home) + "/").c_str();
    case SDL_FOLDER_DESKTOP: type = "DESKTOP"; break;
    case SDL_FOLDER_DOCUMENTS: type = "DOCUMENTS"; break;
    case SDL_FOLDER_DOWNLOADS: type = "DOWNLOAD"; break;
    case SDL_FOLDER_MUSIC: type = "MUSIC"; break;
    case SDL_FOLDER_PICTURES: type = "PICTURES"; break;
    case SDL_FOLDER_PUBLICSHARE: type = "PUBLICSHARE"; break;
    case SDL_FOLDER_TEMPLATES: type = "TEMPLATES"; break;
    case SDL_FOLDER_VIDEOS: type = "VIDEOS"; break;
    default: return SDL_SetError("Folder unavailable on XDG"), nullptr;
    }
    if (!home) return SDL_SetError("XDG directory not available"), nullptr;
    // user-dirs.dirs, read as xdg-user-dirs does: XDG_<TYPE>_DIR="$HOME/..." or an absolute path,
    // backslash escapes, the last match wins.
    const char* config = getenv("XDG_CONFIG_HOME");
    std::string file = config && *config ? std::string(config) + "/user-dirs.dirs" : std::string(home) + "/.config/user-dirs.dirs";
    std::optional<std::string> found;
    if (FILE* f = fopen(file.c_str(), "r")) {
        char line[512];
        const std::string key = std::string("XDG_") + type + "_DIR";
        while (fgets(line, sizeof line, f)) {
            std::string_view p = line;
            if (!p.empty() && p.back() == '\n') p.remove_suffix(1);
            while (!p.empty() && (p[0] == ' ' || p[0] == '\t')) p.remove_prefix(1);
            if (p.rfind(key, 0) != 0) continue;
            p.remove_prefix(key.size());
            while (!p.empty() && (p[0] == ' ' || p[0] == '\t')) p.remove_prefix(1);
            if (p.empty() || p[0] != '=') continue;
            p.remove_prefix(1);
            while (!p.empty() && (p[0] == ' ' || p[0] == '\t')) p.remove_prefix(1);
            if (p.empty() || p[0] != '"') continue;
            p.remove_prefix(1);
            std::string dir;
            if (p.rfind("$HOME/", 0) == 0) {
                dir = std::string(home) + "/";
                p.remove_prefix(6);
            } else if (p.empty() || p[0] != '/') {
                continue;
            }
            for (size_t i = 0; i < p.size() && p[i] != '"'; ++i) {
                if (p[i] == '\\' && i + 1 < p.size()) ++i;
                dir += p[i];
            }
            found = dir;
        }
        fclose(f);
    }
    if (!found && strcmp(type, "DESKTOP") == 0) found = std::string(home) + "/Desktop";
    if (!found) return SDL_SetError("XDG directory not available"), nullptr;
    return (result = *found + "/").c_str();
}

SDL_SystemTheme SDL_GetSystemTheme() {
    if (const char* v = getenv("CORDINAL_COLOR_SCHEME")) {  // tests: "dark" or "light"
        if (strcmp(v, "dark") == 0) return SDL_SYSTEM_THEME_DARK;
        if (strcmp(v, "light") == 0) return SDL_SYSTEM_THEME_LIGHT;
    }
    std::call_once(g_schemeOnce, WatchScheme);
    int v = g_scheme.load();
    return v == 1 ? SDL_SYSTEM_THEME_DARK : v == 2 ? SDL_SYSTEM_THEME_LIGHT : SDL_SYSTEM_THEME_UNKNOWN;
}
