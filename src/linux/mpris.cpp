// cordinal on the session bus (sd-bus), on a thread of its own:
//  - MPRIS (org.mpris.MediaPlayer2.cordinal): media keys, playerctl and bar widgets (Waybar on
//    Omarchy) see what plays, with its cover, and control it.
//  - The tray icon (StatusNotifierItem with a dbusmenu menu, as the Windows tray icon has).
//  - One cordinal at a time: a second one hands its files or command to the first and leaves
//    (io.github.cordinal.Open on the same name).
// Getters read a snapshot the UI thread keeps current (MprisChanged); methods post Remote
// commands to the UI thread.
#include "app.h"
#include "radio_ui.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstring>

namespace lx {

namespace {

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

sd_bus* g_bus = nullptr;
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

// --- MPRIS ----------------------------------------------------------------------------------------
int AppendVariant(sd_bus_message* m, const char* sig, auto&&... value) {
    int r = sd_bus_message_open_container(m, 'v', sig);
    if (r >= 0) r = sd_bus_message_append(m, sig, value...);
    if (r >= 0) r = sd_bus_message_close_container(m);
    return r;
}

int AppendEntry(sd_bus_message* m, const char* key, const char* sig, auto&&... value) {
    int r = sd_bus_message_open_container(m, 'e', "sv");
    if (r >= 0) r = sd_bus_message_append(m, "s", key);
    if (r >= 0) r = AppendVariant(m, sig, value...);
    if (r >= 0) r = sd_bus_message_close_container(m);
    return r;
}

int AppendStringList(sd_bus_message* m, const char* key, const std::string& value) {
    int r = sd_bus_message_open_container(m, 'e', "sv");
    if (r >= 0) r = sd_bus_message_append(m, "s", key);
    if (r >= 0) r = sd_bus_message_open_container(m, 'v', "as");
    if (r >= 0) r = sd_bus_message_append(m, "as", 1, value.c_str());
    if (r >= 0) r = sd_bus_message_close_container(m);
    if (r >= 0) r = sd_bus_message_close_container(m);
    return r;
}

int GetMetadata(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void*, sd_bus_error*) {
    Snapshot s = Snap();
    int r = sd_bus_message_open_container(reply, 'a', "{sv}");
    if (r < 0) return r;
    if (s.id == kNone) {
        AppendEntry(reply, "mpris:trackid", "o", "/org/mpris/MediaPlayer2/TrackList/NoTrack");
    } else {
        std::string trackId = "/io/github/cordinal/track/" + std::to_string(s.id);
        AppendEntry(reply, "mpris:trackid", "o", trackId.c_str());
        if (s.lengthUs > 0) AppendEntry(reply, "mpris:length", "x", s.lengthUs);
        AppendEntry(reply, "xesam:title", "s", s.title.c_str());
        if (!s.artist.empty()) AppendStringList(reply, "xesam:artist", s.artist);
        if (!s.album.empty()) AppendEntry(reply, "xesam:album", "s", s.album.c_str());
        if (!s.albumArtist.empty()) AppendStringList(reply, "xesam:albumArtist", s.albumArtist);
        if (s.trackNo) AppendEntry(reply, "xesam:trackNumber", "i", s.trackNo);
        if (!s.url.empty()) AppendEntry(reply, "xesam:url", "s", s.url.c_str());
        if (!s.art.empty()) AppendEntry(reply, "mpris:artUrl", "s", s.art.c_str());
    }
    return sd_bus_message_close_container(reply);
}

int64_t PositionUs(const Snapshot& s) {
    double pos = s.position;
    if (s.status == "Playing") pos += (SDL_GetTicks() - s.at) / 1000.0 * s.rate;
    if (s.loopEnd > s.loopStart && pos >= s.loopEnd)
        pos = s.loopStart + std::fmod(pos - s.loopStart, s.loopEnd - s.loopStart);
    if (s.lengthUs > 0) pos = std::min(pos, s.lengthUs / 1e6);
    return (int64_t)(pos * 1e6);
}

int GetProperty(sd_bus*, const char*, const char*, const char* property, sd_bus_message* reply, void*, sd_bus_error*) {
    Snapshot s = Snap();
    std::string_view p = property;
    if (p == "PlaybackStatus") return sd_bus_message_append(reply, "s", s.status.c_str());
    if (p == "LoopStatus")
        return sd_bus_message_append(reply, "s", s.repeat == Repeat::One ? "Track" : s.repeat == Repeat::All ? "Playlist" : "None");
    if (p == "Shuffle") return sd_bus_message_append(reply, "b", (int)s.shuffle);
    if (p == "Volume") return sd_bus_message_append(reply, "d", s.volume);
    if (p == "Position") return sd_bus_message_append(reply, "x", PositionUs(s));
    if (p == "Rate") return sd_bus_message_append(reply, "d", s.rate);
    if (p == "MinimumRate") return sd_bus_message_append(reply, "d", 0.375);
    if (p == "MaximumRate") return sd_bus_message_append(reply, "d", 4.0);
    if (p == "CanGoNext") return sd_bus_message_append(reply, "b", (int)s.canNext);
    if (p == "CanGoPrevious") return sd_bus_message_append(reply, "b", (int)s.canPrevious);
    if (p == "CanSeek") return sd_bus_message_append(reply, "b", (int)(s.id != kNone && s.lengthUs > 0));
    if (p == "CanPlay" || p == "CanPause" || p == "CanControl" || p == "CanQuit" || p == "CanRaise")
        return sd_bus_message_append(reply, "b", 1);
    if (p == "HasTrackList" || p == "Fullscreen" || p == "CanSetFullscreen") return sd_bus_message_append(reply, "b", 0);
    if (p == "Identity") return sd_bus_message_append(reply, "s", "cordinal");
    if (p == "DesktopEntry") return sd_bus_message_append(reply, "s", "cordinal");
    if (p == "SupportedUriSchemes") return sd_bus_message_append(reply, "as", 1, "file");
    if (p == "SupportedMimeTypes") return sd_bus_message_append(reply, "as", 6, "audio/flac", "audio/mpeg", "audio/ogg", "audio/x-wav", "audio/mp4", "audio/opus");
    return -EINVAL;
}

int SetProperty(sd_bus*, const char*, const char*, const char* property, sd_bus_message* value, void*, sd_bus_error*) {
    std::string_view p = property;
    if (p == "Volume") {
        double v = 0;
        int r = sd_bus_message_read(value, "d", &v);
        if (r >= 0) Post(Remote::SetVolume, (LPARAM)std::lround(std::clamp(v, 0.0, 1.0) * 1e6));
        return r;
    }
    if (p == "Shuffle") {
        int b = 0;
        int r = sd_bus_message_read(value, "b", &b);
        if (r >= 0) Post(Remote::SetShuffle, b);
        return r;
    }
    if (p == "LoopStatus") {
        const char* s = nullptr;
        int r = sd_bus_message_read(value, "s", &s);
        if (r >= 0 && s) Post(Remote::SetRepeat, (LPARAM)(!strcmp(s, "Track") ? Repeat::One : !strcmp(s, "Playlist") ? Repeat::All : Repeat::Off));
        return r;
    }
    return sd_bus_reply_method_return(value, nullptr);
}

int Method(sd_bus_message* m, void*, sd_bus_error*) {
    std::string_view name = sd_bus_message_get_member(m);
    static const std::pair<std::string_view, Remote> kSimple[] = {
        {"PlayPause", Remote::PlayPause}, {"Play", Remote::Play}, {"Pause", Remote::Pause}, {"Stop", Remote::Stop},
        {"Next", Remote::Next},           {"Previous", Remote::Previous}, {"Raise", Remote::Raise}, {"Quit", Remote::Quit}};
    for (auto& [n, r] : kSimple)
        if (name == n) {
            Post(r);
            return sd_bus_reply_method_return(m, nullptr);
        }
    if (name == "Seek") {
        int64_t offset = 0;
        sd_bus_message_read(m, "x", &offset);
        Post(Remote::Seek, (LPARAM)offset);
    } else if (name == "SetPosition") {
        const char* track = nullptr;
        int64_t pos = 0;
        sd_bus_message_read(m, "ox", &track, &pos);
        Snapshot s = Snap();
        if (track && s.id != kNone && std::string(track) == "/io/github/cordinal/track/" + std::to_string(s.id))
            Post(Remote::SetPosition, (LPARAM)pos);
    } else if (name == "OpenUri") {
        const char* uri = nullptr;
        sd_bus_message_read(m, "s", &uri);
        if (uri && !strncmp(uri, "file://", 7)) {
            std::wstring path(4096, L'\0');
            DWORD size = (DWORD)path.size();
            if (SUCCEEDED(PathCreateFromUrlW(compat::Widen(uri).c_str(), path.data(), &size, 0))) {
                path.resize(size);
                Post(Remote::Open, (LPARAM) new std::vector<std::wstring>{path});
            }
        }
    }
    return sd_bus_reply_method_return(m, nullptr);
}

// io.github.cordinal.Open(as): files from another cordinal process.
int OpenMethod(sd_bus_message* m, void*, sd_bus_error*) {
    auto* paths = new std::vector<std::wstring>();
    char** list = nullptr;
    if (sd_bus_message_read_strv(m, &list) >= 0 && list) {
        for (char** p = list; *p; ++p) {
            paths->push_back(compat::Widen(*p));
            free(*p);
        }
        free(list);
    }
    if (paths->empty()) {
        delete paths;
        Post(Remote::Raise);
    } else {
        Post(Remote::Open, (LPARAM)paths);
    }
    return sd_bus_reply_method_return(m, nullptr);
}

const sd_bus_vtable kRootVtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Raise", "", "", Method, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Quit", "", "", Method, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_PROPERTY("CanQuit", "b", GetProperty, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("CanRaise", "b", GetProperty, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Fullscreen", "b", GetProperty, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("CanSetFullscreen", "b", GetProperty, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("HasTrackList", "b", GetProperty, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Identity", "s", GetProperty, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("DesktopEntry", "s", GetProperty, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("SupportedUriSchemes", "as", GetProperty, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("SupportedMimeTypes", "as", GetProperty, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_VTABLE_END};

const sd_bus_vtable kPlayerVtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Next", "", "", Method, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Previous", "", "", Method, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Pause", "", "", Method, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("PlayPause", "", "", Method, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Stop", "", "", Method, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Play", "", "", Method, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Seek", "x", "", Method, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SetPosition", "ox", "", Method, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("OpenUri", "s", "", Method, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_SIGNAL("Seeked", "x", 0),
    SD_BUS_PROPERTY("PlaybackStatus", "s", GetProperty, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_WRITABLE_PROPERTY("LoopStatus", "s", GetProperty, SetProperty, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("Rate", "d", GetProperty, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_WRITABLE_PROPERTY("Shuffle", "b", GetProperty, SetProperty, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("Metadata", "a{sv}", GetMetadata, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_WRITABLE_PROPERTY("Volume", "d", GetProperty, SetProperty, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("Position", "x", GetProperty, 0, SD_BUS_VTABLE_PROPERTY_EMITS_INVALIDATION),
    SD_BUS_PROPERTY("MinimumRate", "d", GetProperty, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("MaximumRate", "d", GetProperty, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("CanGoNext", "b", GetProperty, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("CanGoPrevious", "b", GetProperty, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("CanPlay", "b", GetProperty, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("CanPause", "b", GetProperty, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("CanSeek", "b", GetProperty, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("CanControl", "b", GetProperty, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_VTABLE_END};

const sd_bus_vtable kAppVtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Open", "as", "", OpenMethod, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END};

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

int AppendMenuProps(sd_bus_message* m, const MenuEntry& e) {
    int r = sd_bus_message_open_container(m, 'a', "{sv}");
    if (e.separator) {
        AppendEntry(m, "type", "s", "separator");
    } else {
        AppendEntry(m, "label", "s", e.label.c_str());
        AppendEntry(m, "enabled", "b", (int)e.enabled);
        if (e.toggle) {
            AppendEntry(m, "toggle-type", "s", "checkmark");
            AppendEntry(m, "toggle-state", "i", (int)e.checked);
        }
    }
    if (r >= 0) r = sd_bus_message_close_container(m);
    return r;
}

int MenuGetLayout(sd_bus_message* m, void*, sd_bus_error*) {
    sd_bus_message* reply = nullptr;
    int r = sd_bus_message_new_method_return(m, &reply);
    if (r < 0) return r;
    sd_bus_message_append(reply, "u", g_menuRevision);
    sd_bus_message_open_container(reply, 'r', "ia{sv}av");
    sd_bus_message_append(reply, "i", 0);
    sd_bus_message_open_container(reply, 'a', "{sv}");
    AppendEntry(reply, "children-display", "s", "submenu");
    sd_bus_message_close_container(reply);
    sd_bus_message_open_container(reply, 'a', "v");
    for (const MenuEntry& e : MenuEntries()) {
        sd_bus_message_open_container(reply, 'v', "(ia{sv}av)");
        sd_bus_message_open_container(reply, 'r', "ia{sv}av");
        sd_bus_message_append(reply, "i", e.id);
        AppendMenuProps(reply, e);
        sd_bus_message_open_container(reply, 'a', "v");
        sd_bus_message_close_container(reply);
        sd_bus_message_close_container(reply);
        sd_bus_message_close_container(reply);
    }
    sd_bus_message_close_container(reply);
    sd_bus_message_close_container(reply);
    r = sd_bus_send(nullptr, reply, nullptr);
    sd_bus_message_unref(reply);
    return r;
}

int MenuGetGroupProperties(sd_bus_message* m, void*, sd_bus_error*) {
    std::vector<int> ids;
    const void* data = nullptr;
    size_t size = 0;
    if (sd_bus_message_read_array(m, 'i', &data, &size) >= 0)
        ids.assign((const int*)data, (const int*)data + size / sizeof(int));
    sd_bus_message* reply = nullptr;
    int r = sd_bus_message_new_method_return(m, &reply);
    if (r < 0) return r;
    sd_bus_message_open_container(reply, 'a', "(ia{sv})");
    for (const MenuEntry& e : MenuEntries()) {
        if (!ids.empty() && std::find(ids.begin(), ids.end(), e.id) == ids.end()) continue;
        sd_bus_message_open_container(reply, 'r', "ia{sv}");
        sd_bus_message_append(reply, "i", e.id);
        AppendMenuProps(reply, e);
        sd_bus_message_close_container(reply);
    }
    sd_bus_message_close_container(reply);
    r = sd_bus_send(nullptr, reply, nullptr);
    sd_bus_message_unref(reply);
    return r;
}

void MenuClicked(int id) {
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

int MenuEvent(sd_bus_message* m, void*, sd_bus_error*) {
    int id = 0;
    const char* event = nullptr;
    if (sd_bus_message_read(m, "is", &id, &event) >= 0 && event && !strcmp(event, "clicked")) MenuClicked(id);
    return sd_bus_reply_method_return(m, nullptr);
}

int MenuEventGroup(sd_bus_message* m, void*, sd_bus_error*) {
    if (sd_bus_message_enter_container(m, 'a', "(isvu)") >= 0) {
        while (sd_bus_message_enter_container(m, 'r', "isvu") > 0) {
            int id = 0;
            const char* event = nullptr;
            sd_bus_message_read(m, "is", &id, &event);
            if (event && !strcmp(event, "clicked")) MenuClicked(id);
            sd_bus_message_skip(m, "vu");
            sd_bus_message_exit_container(m);
        }
        sd_bus_message_exit_container(m);
    }
    return sd_bus_reply_method_return(m, "ai", 0);
}

int MenuAboutToShow(sd_bus_message* m, void*, sd_bus_error*) { return sd_bus_reply_method_return(m, "b", 0); }
int MenuAboutToShowGroup(sd_bus_message* m, void*, sd_bus_error*) { return sd_bus_reply_method_return(m, "aiai", 0, 0); }

int MenuProperty(sd_bus*, const char*, const char*, const char* property, sd_bus_message* reply, void*, sd_bus_error*) {
    std::string_view p = property;
    if (p == "Version") return sd_bus_message_append(reply, "u", 3u);
    if (p == "TextDirection") return sd_bus_message_append(reply, "s", "ltr");
    if (p == "Status") return sd_bus_message_append(reply, "s", "normal");
    if (p == "IconThemePath") return sd_bus_message_append(reply, "as", 0);
    return -EINVAL;
}

std::string IconThemePath() {
    const char* data = getenv("XDG_DATA_HOME");
    const char* home = getenv("HOME");
    return (data && *data == '/' ? std::string(data) : std::string(home ? home : "/tmp") + "/.local/share") + "/icons";
}

int SniProperty(sd_bus*, const char*, const char*, const char* property, sd_bus_message* reply, void*, sd_bus_error*) {
    std::string_view p = property;
    if (p == "Category") return sd_bus_message_append(reply, "s", "ApplicationStatus");
    if (p == "Id") return sd_bus_message_append(reply, "s", "cordinal");
    if (p == "Title") return sd_bus_message_append(reply, "s", "cordinal");
    if (p == "Status") return sd_bus_message_append(reply, "s", "Active");
    if (p == "IconName") return sd_bus_message_append(reply, "s", "cordinal");
    if (p == "IconThemePath") return sd_bus_message_append(reply, "s", IconThemePath().c_str());
    if (p == "ItemIsMenu") return sd_bus_message_append(reply, "b", 0);
    if (p == "Menu") return sd_bus_message_append(reply, "o", kMenuPath);
    if (p == "ToolTip") {
        Snapshot s = Snap();
        std::string body;
        if (s.id != kNone && s.status != "Stopped") {
            body = s.artist.empty() ? s.title : s.artist + " - " + s.title;
            if (s.status == "Paused") body += " (paused)";
        }
        if (!s.output.empty()) body += (body.empty() ? "" : "\n") + s.output;
        int r = sd_bus_message_open_container(reply, 'r', "sa(iiay)ss");
        if (r >= 0) r = sd_bus_message_append(reply, "s", "cordinal");
        if (r >= 0) r = sd_bus_message_append(reply, "a(iiay)", 0);
        if (r >= 0) r = sd_bus_message_append(reply, "ss", "cordinal", body.c_str());
        if (r >= 0) r = sd_bus_message_close_container(reply);
        return r;
    }
    return -EINVAL;
}

int SniMethod(sd_bus_message* m, void*, sd_bus_error*) {
    std::string_view name = sd_bus_message_get_member(m);
    if (name == "Activate") Post(Remote::Raise);
    else if (name == "SecondaryActivate") Post(Remote::PlayPause);
    return sd_bus_reply_method_return(m, nullptr);
}

const sd_bus_vtable kSniVtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Activate", "ii", "", SniMethod, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SecondaryActivate", "ii", "", SniMethod, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("ContextMenu", "ii", "", SniMethod, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Scroll", "is", "", SniMethod, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_SIGNAL("NewTitle", "", 0),
    SD_BUS_SIGNAL("NewIcon", "", 0),
    SD_BUS_SIGNAL("NewToolTip", "", 0),
    SD_BUS_SIGNAL("NewStatus", "s", 0),
    SD_BUS_PROPERTY("Category", "s", SniProperty, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Id", "s", SniProperty, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Title", "s", SniProperty, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Status", "s", SniProperty, 0, 0),
    SD_BUS_PROPERTY("IconName", "s", SniProperty, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("IconThemePath", "s", SniProperty, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("ToolTip", "(sa(iiay)ss)", SniProperty, 0, 0),
    SD_BUS_PROPERTY("ItemIsMenu", "b", SniProperty, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Menu", "o", SniProperty, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_VTABLE_END};

const sd_bus_vtable kMenuVtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("GetLayout", "iias", "u(ia{sv}av)", MenuGetLayout, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetGroupProperties", "aias", "a(ia{sv})", MenuGetGroupProperties, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Event", "isvu", "", MenuEvent, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("EventGroup", "a(isvu)", "ai", MenuEventGroup, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("AboutToShow", "i", "b", MenuAboutToShow, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("AboutToShowGroup", "ai", "aiai", MenuAboutToShowGroup, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_SIGNAL("ItemsPropertiesUpdated", "a(ia{sv})a(ias)", 0),
    SD_BUS_SIGNAL("LayoutUpdated", "ui", 0),
    SD_BUS_SIGNAL("ItemActivationRequested", "iu", 0),
    SD_BUS_PROPERTY("Version", "u", MenuProperty, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("TextDirection", "s", MenuProperty, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Status", "s", MenuProperty, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("IconThemePath", "as", MenuProperty, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_VTABLE_END};

void RegisterWithWatcher() {
    sd_bus_call_method_async(g_bus, nullptr, kWatcher, "/StatusNotifierWatcher", kWatcher, "RegisterStatusNotifierItem",
                             nullptr, nullptr, "s", g_sniName.c_str());
}

// The bar can start (or restart) after cordinal: register whenever a watcher appears.
int OnNameOwnerChanged(sd_bus_message* m, void*, sd_bus_error*) {
    const char *name = nullptr, *oldOwner = nullptr, *newOwner = nullptr;
    if (sd_bus_message_read(m, "sss", &name, &oldOwner, &newOwner) >= 0 && name && !strcmp(name, kWatcher) && newOwner && *newOwner)
        RegisterWithWatcher();
    return 0;
}

// --- the bus thread -----------------------------------------------------------------------------------------
void Run() {
    Snapshot last;
    std::string lastMenu;
    while (!g_quit) {
        for (int r; (r = sd_bus_process(g_bus, nullptr)) > 0;) {
        }
        pollfd fds[2] = {{sd_bus_get_fd(g_bus), (short)sd_bus_get_events(g_bus), 0}, {g_wake, POLLIN, 0}};
        uint64_t usec = 0;
        int timeout = sd_bus_get_timeout(g_bus, &usec) > 0 && usec != UINT64_MAX ? 100 : -1;
        poll(fds, 2, timeout);
        if (fds[1].revents & POLLIN) {
            uint64_t v;
            if (read(g_wake, &v, sizeof(v)) < 0) {
            }
        }
        if (g_quit) break;
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
        if (!changed.empty()) {
            changed.push_back(nullptr);
            sd_bus_emit_properties_changed_strv(g_bus, kPath, kPlayer, (char**)changed.data());
        }
        if (seeked) sd_bus_emit_signal(g_bus, kPath, kPlayer, "Seeked", "x", PositionUs(now));
        std::string menuKey;
        for (auto& e : MenuEntries()) menuKey += e.label + (e.enabled ? "1" : "0") + (e.checked ? "1" : "0");
        if (menuKey != lastMenu) {
            lastMenu = menuKey;
            ++g_menuRevision;
            sd_bus_emit_signal(g_bus, kMenuPath, kMenu, "LayoutUpdated", "ui", g_menuRevision, 0);
        }
        if (now.status != last.status || now.id != last.id || now.output != last.output)
            sd_bus_emit_signal(g_bus, kSniPath, kSni, "NewToolTip", "");
        last = std::move(now);
        sd_bus_flush(g_bus);
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
    sd_bus* bus = nullptr;
    if (sd_bus_open_user(&bus) < 0) return false;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    int has = 0;
    if (sd_bus_call_method(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner", &err,
                           &reply, "s", kBusName) >= 0)
        sd_bus_message_read(reply, "b", &has);
    sd_bus_message_unref(reply);
    sd_bus_error_free(&err);
    if (!has) {
        sd_bus_unref(bus);
        return false;
    }
    static const std::pair<const char*, const char*> kCommands[] = {
        {"--play-pause", "PlayPause"}, {"--play", "Play"}, {"--pause", "Pause"}, {"--stop", "Stop"},
        {"--next", "Next"},            {"--previous", "Previous"}, {"--quit", "Quit"}};
    if (args.size() == 1)
        for (auto& [arg, method] : kCommands)
            if (args[0] == arg) {
                const char* iface = !strcmp(method, "Quit") ? kRoot : kPlayer;
                sd_bus_call_method(bus, kBusName, kPath, iface, method, nullptr, nullptr, "");
                sd_bus_unref(bus);
                return true;
            }
    sd_bus_message* m = nullptr;
    if (sd_bus_message_new_method_call(bus, &m, kBusName, kPath, kApp, "Open") >= 0) {
        std::vector<const char*> list;
        for (auto& a : args) list.push_back(a.c_str());
        list.push_back(nullptr);
        sd_bus_message_append_strv(m, (char**)list.data());
        sd_bus_call(bus, m, 0, nullptr, nullptr);
        sd_bus_message_unref(m);
    }
    sd_bus_unref(bus);
    return true;
}

void StartMpris() {
    if (sd_bus_open_user(&g_bus) < 0) {
        g_bus = nullptr;
        return;  // no session bus: no media keys or tray, the rest works
    }
    sd_bus_add_object_vtable(g_bus, nullptr, kPath, kRoot, kRootVtable, nullptr);
    sd_bus_add_object_vtable(g_bus, nullptr, kPath, kPlayer, kPlayerVtable, nullptr);
    sd_bus_add_object_vtable(g_bus, nullptr, kPath, kApp, kAppVtable, nullptr);
    sd_bus_add_object_vtable(g_bus, nullptr, kSniPath, kSni, kSniVtable, nullptr);
    sd_bus_add_object_vtable(g_bus, nullptr, kMenuPath, kMenu, kMenuVtable, nullptr);
    sd_bus_request_name(g_bus, kBusName, 0);
    g_sniName = "org.kde.StatusNotifierItem-" + std::to_string(getpid()) + "-1";
    sd_bus_request_name(g_bus, g_sniName.c_str(), 0);
    sd_bus_match_signal(g_bus, nullptr, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                        "NameOwnerChanged", OnNameOwnerChanged, nullptr);
    RegisterWithWatcher();
    g_wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    g_quit = false;
    MprisChanged();
    g_thread = std::thread(Run);
}

void StopMpris() {
    if (!g_bus) return;
    g_quit = true;
    Wake();
    if (g_thread.joinable()) g_thread.join();
    sd_bus_flush_close_unref(g_bus);
    g_bus = nullptr;
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
