// cordinal on the session bus (the built-in client in dbus.h), on a thread of its own:
//  - MPRIS (org.mpris.MediaPlayer2.cordinal): media keys, playerctl and bar widgets (Waybar on
//    Omarchy) see what plays, with its cover, and control it.
//  - The tray icon (StatusNotifierItem with a dbusmenu menu, as the Windows tray icon has).
//  - One cordinal at a time: a second one hands its files or command to the first and leaves
//    (io.github.cordinal.Open on the same name).
// Getters read a snapshot the UI thread keeps current (MprisChanged); methods post Remote
// commands to the UI thread. Only the bus thread touches the connection once it has started.
#include "app.h"
#include "dbus.h"
#include "radio_ui.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstring>

namespace lx {

namespace {

using dbus::Reader;
using dbus::Writer;

constexpr const char* kBusName = "org.mpris.MediaPlayer2.cordinal";
constexpr const char* kPath = "/org/mpris/MediaPlayer2";
constexpr const char* kRoot = "org.mpris.MediaPlayer2";
constexpr const char* kPlayer = "org.mpris.MediaPlayer2.Player";
constexpr const char* kApp = "io.github.cordinal";
constexpr const char* kSniPath = "/StatusNotifierItem";
constexpr const char* kMenuPath = "/MenuBar";
constexpr const char* kSni = "org.kde.StatusNotifierItem";
constexpr const char* kMenu = "com.canonical.dbusmenu";
constexpr const char* kWatcher = "org.kde.StatusNotifierWatcher";
constexpr dbus::Target kDaemon{"org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", {}};

// What the bus side shows, copied from the UI thread.
struct Snapshot {
    std::string status = "Stopped";  // Playing, Paused, Stopped
    TrackId id = kNone;
    std::string title, artist, album, albumArtist, art, url;
    int64_t lengthUs = 0;
    double loopStart = 0, loopEnd = 0;
    int trackNo = 0;
    double position = 0;  // seconds, at `at`
    double rate = 1;
    uint64_t at = 0;      // SDL_GetTicks()
    double volume = 1;
    bool shuffle = false;
    Repeat repeat = Repeat::Off;
    bool canNext = false, canPrevious = false;
    std::string output;
};
std::mutex g_mx;
Snapshot g_snap;
std::string g_artPath;
TrackId g_artFor = kNone;
bool g_seeked = false;

std::unique_ptr<dbus::Bus> g_bus;
std::thread g_thread;
int g_wake = -1;  // eventfd: the snapshot changed, or quit
std::atomic<bool> g_quit{false};
std::string g_sniName;
uint32_t g_menuRevision = 1;

Snapshot Snap() {
    std::lock_guard lock(g_mx);
    return g_snap;
}

void Post(Remote r, LPARAM value = 0) { PostMessageW(nullptr, WM_APP_REMOTE, (WPARAM)r, value); }

std::string AutostartPath() {
    const char* cfg = getenv("XDG_CONFIG_HOME");
    const char* home = getenv("HOME");
    std::string dir = cfg && *cfg == '/' ? cfg : std::string(home ? home : "/tmp") + "/.config";
    return dir + "/autostart/cordinal.desktop";
}

std::string TrackPath(TrackId id) { return "/io/github/cordinal/track/" + std::to_string(id); }

// --- MPRIS ----------------------------------------------------------------------------------------
void WriteMetadata(Writer& w) {
    Snapshot s = Snap();
    Writer::Array a = w.Open(8);
    w.Entry("mpris:trackid", "o");
    if (s.id == kNone) {
        w.O("/org/mpris/MediaPlayer2/TrackList/NoTrack");
    } else {
        w.O(TrackPath(s.id));
        if (s.lengthUs > 0) w.Entry("mpris:length", "x"), w.X(s.lengthUs);
        w.Entry("xesam:title", "s"), w.S(s.title);
        if (!s.artist.empty()) w.Entry("xesam:artist", "as"), w.Strings({s.artist});
        if (!s.album.empty()) w.Entry("xesam:album", "s"), w.S(s.album);
        if (!s.albumArtist.empty()) w.Entry("xesam:albumArtist", "as"), w.Strings({s.albumArtist});
        if (s.trackNo) w.Entry("xesam:trackNumber", "i"), w.I(s.trackNo);
        if (!s.url.empty()) w.Entry("xesam:url", "s"), w.S(s.url);
        if (!s.art.empty()) w.Entry("mpris:artUrl", "s"), w.S(s.art);
    }
    w.Close(a);
}

int64_t PositionUs(const Snapshot& s) {
    double pos = s.position;
    if (s.status == "Playing") pos += (SDL_GetTicks() - s.at) / 1000.0 * s.rate;
    if (s.loopEnd > s.loopStart && pos >= s.loopEnd)
        pos = s.loopStart + std::fmod(pos - s.loopStart, s.loopEnd - s.loopStart);
    if (s.lengthUs > 0) pos = std::min(pos, s.lengthUs / 1e6);
    return (int64_t)(pos * 1e6);
}

// The root and Player interfaces' properties; the tables below give each one's type.
void GetProperty(std::string_view p, Writer& w) {
    if (p == "Metadata") return WriteMetadata(w);
    Snapshot s = Snap();
    if (p == "PlaybackStatus") w.S(s.status);
    else if (p == "LoopStatus") w.S(s.repeat == Repeat::One ? "Track" : s.repeat == Repeat::All ? "Playlist" : "None");
    else if (p == "Shuffle") w.B(s.shuffle);
    else if (p == "Volume") w.D(s.volume);
    else if (p == "Position") w.X(PositionUs(s));
    else if (p == "Rate") w.D(s.rate);
    else if (p == "MinimumRate") w.D(0.375);
    else if (p == "MaximumRate") w.D(4.0);
    else if (p == "CanGoNext") w.B(s.canNext);
    else if (p == "CanGoPrevious") w.B(s.canPrevious);
    else if (p == "CanSeek") w.B(s.id != kNone && s.lengthUs > 0);
    else if (p == "HasTrackList" || p == "Fullscreen" || p == "CanSetFullscreen") w.B(false);
    else if (p == "Identity" || p == "DesktopEntry") w.S("cordinal");
    else if (p == "SupportedUriSchemes") w.Strings({"file"});
    else if (p == "SupportedMimeTypes") w.Strings({"audio/flac", "audio/mpeg", "audio/ogg", "audio/x-wav", "audio/mp4", "audio/opus"});
    else w.B(true);  // CanPlay, CanPause, CanControl, CanQuit, CanRaise
}

void SetProperty(std::string_view p, Reader& r) {
    if (p == "Volume") Post(Remote::SetVolume, (LPARAM)std::lround(std::clamp(r.D(), 0.0, 1.0) * 1e6));
    else if (p == "Shuffle") Post(Remote::SetShuffle, r.B());
    else if (p == "LoopStatus") {
        std::string_view s = r.S();
        Post(Remote::SetRepeat, (LPARAM)(s == "Track" ? Repeat::One : s == "Playlist" ? Repeat::All : Repeat::Off));
    }
}

void Simple(dbus::Call& c) {
    static const std::pair<std::string_view, Remote> kSimple[] = {
        {"PlayPause", Remote::PlayPause}, {"Play", Remote::Play}, {"Pause", Remote::Pause}, {"Stop", Remote::Stop},
        {"Next", Remote::Next},           {"Previous", Remote::Previous}, {"Raise", Remote::Raise}, {"Quit", Remote::Quit}};
    for (auto& [n, r] : kSimple)
        if (c.msg.member == n) Post(r);
}

void Seek(dbus::Call& c) { Post(Remote::Seek, (LPARAM)c.in.X()); }

void SetPosition(dbus::Call& c) {
    std::string_view track = c.in.O();
    int64_t pos = c.in.X();
    Snapshot s = Snap();
    if (s.id != kNone && track == TrackPath(s.id)) Post(Remote::SetPosition, (LPARAM)pos);
}

void OpenUri(dbus::Call& c) {
    std::string uri(c.in.S());
    if (uri.rfind("file://", 0) != 0) return;
    std::wstring path(4096, L'\0');
    DWORD size = (DWORD)path.size();
    if (SUCCEEDED(PathCreateFromUrlW(compat::Widen(uri.c_str()).c_str(), path.data(), &size, 0))) {
        path.resize(size);
        Post(Remote::Open, (LPARAM) new std::vector<std::wstring>{path});
    }
}

// io.github.cordinal.Open(as): files from another cordinal process.
void OpenMethod(dbus::Call& c) {
    auto* paths = new std::vector<std::wstring>();
    for (size_t end = c.in.Open(4); c.in.In(end);) paths->push_back(compat::Widen(std::string(c.in.S()).c_str()));
    if (paths->empty()) {
        delete paths;
        Post(Remote::Raise);
    } else {
        Post(Remote::Open, (LPARAM)paths);
    }
}

constexpr dbus::Method kRootMethods[] = {{"Raise", "", "", Simple}, {"Quit", "", "", Simple}};
constexpr dbus::Property kRootProperties[] = {
    {"CanQuit", "b"}, {"CanRaise", "b"}, {"Fullscreen", "b"}, {"CanSetFullscreen", "b"}, {"HasTrackList", "b"},
    {"Identity", "s"}, {"DesktopEntry", "s"}, {"SupportedUriSchemes", "as"}, {"SupportedMimeTypes", "as"}};
constexpr dbus::Method kPlayerMethods[] = {
    {"Next", "", "", Simple}, {"Previous", "", "", Simple}, {"Pause", "", "", Simple}, {"PlayPause", "", "", Simple},
    {"Stop", "", "", Simple}, {"Play", "", "", Simple},     {"Seek", "x", "", Seek},    {"SetPosition", "ox", "", SetPosition},
    {"OpenUri", "s", "", OpenUri}};
constexpr dbus::SignalInfo kPlayerSignals[] = {{"Seeked", "x"}};
// Position is polled by clients, never announced, as the specification allows.
constexpr dbus::Property kPlayerProperties[] = {
    {"PlaybackStatus", "s"}, {"LoopStatus", "s", true}, {"Rate", "d"},       {"Shuffle", "b", true},
    {"Metadata", "a{sv}"},   {"Volume", "d", true},     {"Position", "x"},   {"MinimumRate", "d"},
    {"MaximumRate", "d"},    {"CanGoNext", "b"},        {"CanGoPrevious", "b"}, {"CanPlay", "b"},
    {"CanPause", "b"},       {"CanSeek", "b"},          {"CanControl", "b"}};
constexpr dbus::Method kAppMethods[] = {{"Open", "as", "", OpenMethod}};

// --- the tray icon (StatusNotifierItem) and its menu (dbusmenu) ------------------------------------------
enum MenuId { kPlayPause = 1, kPrevious, kNext, kStop, kSep1, kShow, kSep2, kLogin, kSep3, kExit };

struct MenuEntry {
    int id;
    std::string label;
    bool enabled = true, separator = false, toggle = false, checked = false;
};

std::vector<MenuEntry> MenuEntries() {
    Snapshot s = Snap();
    bool idle = s.status == "Stopped";
    return {{kPlayPause, s.status == "Paused" ? "_Resume" : idle ? "_Play" : "_Pause"},
            {kPrevious, "Pre_vious", !idle},
            {kNext, "_Next", !idle},
            {kStop, "_Stop", !idle},
            {kSep1, "", true, true},
            {kShow, "Show _cordinal"},
            {kSep2, "", true, true},
            {kLogin, "Start on _login", true, false, true, StartsOnLogin()},
            {kSep3, "", true, true},
            {kExit, "E_xit"}};
}

void WriteMenuProps(Writer& w, const MenuEntry& e) {
    Writer::Array a = w.Open(8);
    if (e.separator) {
        w.Entry("type", "s"), w.S("separator");
    } else {
        w.Entry("label", "s"), w.S(e.label);
        w.Entry("enabled", "b"), w.B(e.enabled);
        if (e.toggle) {
            w.Entry("toggle-type", "s"), w.S("checkmark");
            w.Entry("toggle-state", "i"), w.I(e.checked);
        }
    }
    w.Close(a);
}

// The whole menu whatever the arguments ask for: it is one level of ten items.
void MenuGetLayout(dbus::Call& c) {
    Writer& w = c.out;
    w.U(g_menuRevision);
    w.Align(8);
    w.I(0);
    Writer::Array props = w.Open(8);
    w.Entry("children-display", "s"), w.S("submenu");
    w.Close(props);
    Writer::Array children = w.Open(1);
    for (const MenuEntry& e : MenuEntries()) {
        w.G("(ia{sv}av)");
        w.Align(8);
        w.I(e.id);
        WriteMenuProps(w, e);
        w.Close(w.Open(1));
    }
    w.Close(children);
}

void MenuGetGroupProperties(dbus::Call& c) {
    std::vector<int> ids;
    for (size_t end = c.in.Open(4); c.in.In(end);) ids.push_back(c.in.I());
    Writer::Array rows = c.out.Open(8);
    for (const MenuEntry& e : MenuEntries()) {
        if (!ids.empty() && std::find(ids.begin(), ids.end(), e.id) == ids.end()) continue;
        c.out.Align(8);
        c.out.I(e.id);
        WriteMenuProps(c.out, e);
    }
    c.out.Close(rows);
}

void MenuClicked(int id, std::string_view event) {
    if (event != "clicked") return;
    switch (id) {
        case kPlayPause: Post(Remote::PlayPause); break;
        case kPrevious: Post(Remote::Previous); break;
        case kNext: Post(Remote::Next); break;
        case kStop: Post(Remote::Stop); break;
        case kShow: Post(Remote::Raise); break;
        case kLogin: Post(Remote::ToggleStartOnLogin); break;
        case kExit: Post(Remote::Quit); break;
        default: break;
    }
}

void MenuEvent(dbus::Call& c) {
    int id = c.in.I();
    MenuClicked(id, c.in.S());
}

void MenuEventGroup(dbus::Call& c) {
    for (size_t end = c.in.Open(8); c.in.In(end);) {
        c.in.Struct();
        int id = c.in.I();
        MenuClicked(id, c.in.S());
        c.in.Skip("vu");
    }
    c.out.Close(c.out.Open(4));  // no ids were unknown
}

void MenuAboutToShow(dbus::Call& c) { c.out.B(false); }
void MenuAboutToShowGroup(dbus::Call& c) { c.out.Close(c.out.Open(4)), c.out.Close(c.out.Open(4)); }

void MenuProperty(std::string_view p, Writer& w) {
    if (p == "Version") w.U(3);
    else if (p == "TextDirection") w.S("ltr");
    else if (p == "Status") w.S("normal");
    else w.Strings({});  // IconThemePath
}

std::string IconThemePath() {
    const char* data = getenv("XDG_DATA_HOME");
    const char* home = getenv("HOME");
    return (data && *data == '/' ? std::string(data) : std::string(home ? home : "/tmp") + "/.local/share") + "/icons";
}

void SniProperty(std::string_view p, Writer& w) {
    if (p == "Category") w.S("ApplicationStatus");
    else if (p == "Id" || p == "Title" || p == "IconName") w.S("cordinal");
    else if (p == "Status") w.S("Active");
    else if (p == "IconThemePath") w.S(IconThemePath());
    else if (p == "ItemIsMenu") w.B(false);
    else if (p == "Menu") w.O(kMenuPath);
    else if (p == "ToolTip") {
        Snapshot s = Snap();
        std::string body;
        if (s.id != kNone && s.status != "Stopped") {
            body = s.artist.empty() ? s.title : s.artist + " - " + s.title;
            if (s.status == "Paused") body += " (paused)";
        }
        if (!s.output.empty()) body += (body.empty() ? "" : "\n") + s.output;
        w.Align(8);
        w.S("cordinal");
        w.Close(w.Open(8));  // no pixmaps
        w.S("cordinal");
        w.S(body);
    }
}

void SniMethod(dbus::Call& c) {
    if (c.msg.member == "Activate") Post(Remote::Raise);
    else if (c.msg.member == "SecondaryActivate") Post(Remote::PlayPause);
}

constexpr dbus::Method kSniMethods[] = {
    {"Activate", "ii", "", SniMethod}, {"SecondaryActivate", "ii", "", SniMethod},
    {"ContextMenu", "ii", "", SniMethod}, {"Scroll", "is", "", SniMethod}};
constexpr dbus::SignalInfo kSniSignals[] = {{"NewTitle", ""}, {"NewIcon", ""}, {"NewToolTip", ""}, {"NewStatus", "s"}};
constexpr dbus::Property kSniProperties[] = {
    {"Category", "s"}, {"Id", "s"}, {"Title", "s"}, {"Status", "s"}, {"IconName", "s"}, {"IconThemePath", "s"},
    {"ToolTip", "(sa(iiay)ss)"}, {"ItemIsMenu", "b"}, {"Menu", "o"}};
constexpr dbus::Method kMenuMethods[] = {
    {"GetLayout", "iias", "u(ia{sv}av)", MenuGetLayout},
    {"GetGroupProperties", "aias", "a(ia{sv})", MenuGetGroupProperties},
    {"Event", "isvu", "", MenuEvent},
    {"EventGroup", "a(isvu)", "ai", MenuEventGroup},
    {"AboutToShow", "i", "b", MenuAboutToShow},
    {"AboutToShowGroup", "ai", "aiai", MenuAboutToShowGroup}};
constexpr dbus::SignalInfo kMenuSignals[] = {
    {"ItemsPropertiesUpdated", "a(ia{sv})a(ias)"}, {"LayoutUpdated", "ui"}, {"ItemActivationRequested", "iu"}};
constexpr dbus::Property kMenuProperties[] = {{"Version", "u"}, {"TextDirection", "s"}, {"Status", "s"}, {"IconThemePath", "as"}};

const dbus::Interface kObjects[] = {
    {kPath, kRoot, kRootMethods, kRootProperties, {}, GetProperty},
    {kPath, kPlayer, kPlayerMethods, kPlayerProperties, kPlayerSignals, GetProperty, SetProperty},
    {kPath, kApp, kAppMethods, {}, {}},
    {kSniPath, kSni, kSniMethods, kSniProperties, kSniSignals, SniProperty},
    {kMenuPath, kMenu, kMenuMethods, kMenuProperties, kMenuSignals, MenuProperty}};

void RegisterWithWatcher() {
    Writer w;
    w.S(g_sniName);
    g_bus->Send({kWatcher, "/StatusNotifierWatcher", kWatcher, "RegisterStatusNotifierItem"}, "s", w);
}

// The bar can start (or restart) after cordinal: register whenever a watcher appears. Only the bus
// daemon's own NameOwnerChanged counts; any peer could send one that looks like it.
void OnMessage(dbus::Bus& bus, const dbus::Message& m) {
    if (m.type == dbus::kSignal) {
        if (m.sender == "org.freedesktop.DBus" && m.member == "NameOwnerChanged" && m.sig == "sss") {
            Reader r = m.Body();
            std::string_view name = r.S(), oldOwner = r.S(), newOwner = r.S();
            (void)oldOwner;
            if (name == kWatcher && !newOwner.empty()) RegisterWithWatcher();
        }
        return;
    }
    dbus::Dispatch(bus, m, kObjects);
}

void Emit(const dbus::Interface& o, const char* member, const char* sig, const Writer& w) {
    g_bus->Signal(o.path, o.name, member, sig, w);
}

// --- the bus thread -----------------------------------------------------------------------------------------
void Run() {
    Snapshot last;
    std::string lastMenu;
    while (!g_quit) {
        int more = g_bus ? g_bus->Process(64) : 0;
        if (more < 0) {
            g_bus.reset();  // closed or malformed: stop, do not spin on a dead socket, do not reconnect
            fputs("cordinal: session bus closed; media keys and tray are off\n", stderr);
        }
        pollfd fds[2] = {{g_wake, POLLIN, 0}, {g_bus ? g_bus->Fd() : -1, g_bus ? g_bus->Events() : (short)0, 0}};
        poll(fds, 2, more > 0 ? 0 : g_bus ? g_bus->TimeoutMs() : -1);
        if (fds[0].revents & POLLIN) {
            uint64_t v;
            if (read(g_wake, &v, sizeof(v)) < 0) {
            }
        }
        if (g_quit) break;
        if (!g_bus) continue;
        // Tell listeners what changed since the last look.
        Snapshot now;
        bool seeked;
        {
            std::lock_guard lock(g_mx);
            now = g_snap;
            seeked = std::exchange(g_seeked, false);
        }
        std::vector<const char*> changed;
        if (now.status != last.status) changed.push_back("PlaybackStatus");
        if (now.id != last.id || now.title != last.title || now.artist != last.artist || now.album != last.album ||
            now.art != last.art || now.lengthUs != last.lengthUs)
            changed.push_back("Metadata");
        if (now.volume != last.volume) changed.push_back("Volume");
        if (now.rate != last.rate) changed.push_back("Rate");
        if (now.shuffle != last.shuffle) changed.push_back("Shuffle");
        if (now.repeat != last.repeat) changed.push_back("LoopStatus");
        if (now.canNext != last.canNext) changed.push_back("CanGoNext");
        if (now.canPrevious != last.canPrevious) changed.push_back("CanGoPrevious");
        if (now.id != last.id || now.lengthUs != last.lengthUs) changed.push_back("CanSeek");
        if (!changed.empty()) dbus::EmitChanged(*g_bus, kObjects[1], changed);
        if (seeked) {
            Writer w;
            w.X(PositionUs(now));
            Emit(kObjects[1], "Seeked", "x", w);
        }
        std::string menuKey;
        for (auto& e : MenuEntries()) menuKey += e.label + (e.enabled ? "1" : "0") + (e.checked ? "1" : "0");
        if (menuKey != lastMenu) {
            lastMenu = menuKey;
            Writer w;
            w.U(++g_menuRevision);
            w.I(0);
            Emit(kObjects[4], "LayoutUpdated", "ui", w);
        }
        if (now.status != last.status || now.id != last.id || now.output != last.output)
            Emit(kObjects[3], "NewToolTip", "", {});
        last = std::move(now);
        g_bus->Flush();
    }
}

void Wake() {
    if (g_wake < 0) return;
    uint64_t one = 1;
    if (write(g_wake, &one, sizeof(one)) < 0) {
    }
}

}  // namespace

bool ForwardToRunning(const std::vector<std::string>& args) {
    auto bus = dbus::Bus::OpenUser();
    if (!bus) return false;
    Writer name;
    name.S(kBusName);
    dbus::Target hasOwner = kDaemon;
    hasOwner.member = "NameHasOwner";
    auto has = bus->CallSync(hasOwner, "s", name, 25000);
    if (!has || has->type != dbus::kReturn || has->sig != "b" || !has->Body().B()) return false;
    static const std::pair<const char*, const char*> kCommands[] = {
        {"--play-pause", "PlayPause"}, {"--play", "Play"}, {"--pause", "Pause"}, {"--stop", "Stop"},
        {"--next", "Next"},            {"--previous", "Previous"}, {"--quit", "Quit"}};
    if (args.size() == 1)
        for (auto& [arg, method] : kCommands)
            if (args[0] == arg) {
                bus->CallSync({kBusName, kPath, !strcmp(method, "Quit") ? kRoot : kPlayer, method}, "", {}, 25000);
                return true;
            }
    Writer list;
    Writer::Array a = list.Open(4);
    for (auto& arg : args) list.S(arg);
    list.Close(a);
    bus->CallSync({kBusName, kPath, kApp, "Open"}, "as", list, 25000);
    return true;
}

void StartMpris() {
    g_bus = dbus::Bus::OpenUser();
    if (!g_bus) {
        fputs("cordinal: no session bus; media keys and tray are off\n", stderr);
        return;  // the rest works
    }
    g_bus->onMessage = OnMessage;
    // The player's name is taken before anything else, so a second cordinal started at once finds it.
    dbus::Target request = kDaemon;
    request.member = "RequestName";
    Writer name;
    name.S(kBusName);
    name.U(0);
    g_bus->CallSync(request, "su", name, 5000);
    g_sniName = "org.kde.StatusNotifierItem-" + std::to_string(getpid()) + "-1";
    Writer sni;
    sni.S(g_sniName);
    sni.U(0);
    g_bus->Send(request, "su", sni);
    dbus::Target match = kDaemon;
    match.member = "AddMatch";
    Writer rule;
    rule.S("type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',"
           "interface='org.freedesktop.DBus',member='NameOwnerChanged'");
    g_bus->Send(match, "s", rule);
    RegisterWithWatcher();
    g_wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    g_quit = false;
    MprisChanged();
    g_thread = std::thread(Run);
}

void StopMpris() {
    if (!g_thread.joinable()) {
        g_bus.reset();
        return;
    }
    g_quit = true;
    Wake();
    g_thread.join();
    g_bus.reset();
    if (g_wake >= 0) close(g_wake);
    g_wake = -1;
}

void MprisChanged(bool seeked) {
    Snapshot s;
    TrackId id = g.playing;
    bool radio = RadioPlaying();
    s.status = id == kNone && !radio ? "Stopped" : IsPaused() ? "Paused" : "Playing";
    if (radio) {
        s.id = kNone - 1;
        s.title = InternetRadio().lastTitle.empty() ? InternetRadio().playing.name : ToUtf8(InternetRadio().lastTitle);
        s.artist = InternetRadio().lastTitle.empty() ? "" : InternetRadio().playing.name;
        s.position = 0;
    } else if (id != kNone) {
        const Track& t = g_lib[id];
        s.id = id;
        s.title = compat::Narrow(TrackTitle(id).c_str());
        s.artist = compat::Narrow(TrackArtist(id).c_str());
        s.album = std::string(g_lib.View(t.album));
        s.albumArtist = std::string(g_lib.View(t.albumArtist));
        s.trackNo = t.trackNo;
        double dur = g.engine.Duration() > 0 ? g.engine.Duration() : t.Duration();
        s.lengthUs = (int64_t)(dur * 1e6);
        s.position = g.engine.Position();
        PlaybackSection section = SectionOf(id);
        if (section.on && section.end - section.start > 0.05 && (dur <= 0 || section.end <= dur)) {
            s.loopStart = section.start;
            s.loopEnd = section.end;
        }
        std::wstring path = g_lib.Path(id), file;
        int index = 0;
        if (SplitSubtrack(path, file, index)) path = file;
        s.url = "file://" + compat::Narrow(path.c_str());
    }
    s.at = SDL_GetTicks();
    s.rate = g.speed * g.tempo;
    s.volume = g.settings.volume;
    s.shuffle = g.settings.shuffle;
    s.repeat = g.settings.repeat;
    s.canNext = s.canPrevious = id != kNone;
    Engine::OutputInfo o = g.engine.Output();
    if (o.exclusive) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Exclusive %g kHz%s", o.rate / 1000.0, o.bitPerfect && id != kNone ? ", bit-perfect" : "");
        s.output = buf;
    }
    {
        std::lock_guard lock(g_mx);
        s.art = g_artFor == id ? g_artPath : std::string();
        g_snap = std::move(s);
        g_seeked |= seeked;
    }
    Wake();
}

void MprisSetArt(TrackId id, const std::string& path) {
    {
        std::lock_guard lock(g_mx);
        g_artFor = id;
        g_artPath = path.empty() ? std::string() : "file://" + path;
        if (g_snap.id == id) g_snap.art = g_artPath;
    }
    Wake();
}

bool StartsOnLogin() { return access(AutostartPath().c_str(), F_OK) == 0; }

void ToggleStartOnLogin() {
    std::string path = AutostartPath();
    if (StartsOnLogin()) {
        unlink(path.c_str());
    } else {
        std::string dir = path.substr(0, path.rfind('/'));
        SHCreateDirectoryExW(nullptr, compat::Widen(dir.c_str()).c_str(), nullptr);
        char exe[4096];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        std::string cmd = n > 0 ? std::string(exe, (size_t)n) : "cordinal";
        if (FILE* f = fopen(path.c_str(), "w")) {
            fprintf(f, "[Desktop Entry]\nType=Application\nName=cordinal\nExec=%s\nIcon=cordinal\nX-GNOME-Autostart-enabled=true\n", cmd.c_str());
            fclose(f);
        }
    }
    MprisChanged();
}

}  // namespace lx
