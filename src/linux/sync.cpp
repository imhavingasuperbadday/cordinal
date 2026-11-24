// Library > Sync with other PCs on Linux, as app_sync.cpp on Windows: the folder from Settings, a pass from
// the UI tick about once a minute, the other PCs' changes applied here, and this PC's published at exit.
#include "panels_internal.h"
#include "syncfolder.h"
#include "ui_internal.h"

#include <unistd.h>

#include <fstream>

namespace lx {
namespace {

syncfolder::Sync g_sync;
bool g_loaded = false;

syncfolder::Live LiveData() { return {g_lib, g.playlists, g.spots, PlayLog()}; }

// /etc/machine-id tells this install apart from any other, so a profile copied to another PC is noticed
// there; the profile folder keeps two profiles on one PC apart.
std::string Fingerprint() {
    std::string id;
    for (const char* file : {"/etc/machine-id", "/var/lib/dbus/machine-id"})
        if (std::ifstream in(file); in && std::getline(in, id) && !id.empty()) break;
    const std::string text = id + "|" + ToUtf8(g.dataDir);
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : text) h = (h ^ c) * 1099511628211ull;
    char hex[17];
    snprintf(hex, sizeof hex, "%016llx", (unsigned long long)h);
    return hex;
}

void Configure() {
    if (!g_loaded) {
        g_loaded = true;
        g_sync.stateFile = g.dataDir + L"/sync-state.txt";
        char host[256] = {};
        g_sync.machine = !gethostname(host, sizeof host - 1) && *host ? FromUtf8(host) : L"Linux PC";
        g_sync.fingerprint = Fingerprint();
    }
    g_sync.folder = g.settings.syncFolder;
    g_sync.roots = g.settings.folders;
}

void Apply(syncfolder::Changes& c) {
    for (const PlayEvent& e : c.listens)
        LogPlay(e.id, e.time, e.seconds, e.flags & PlayEvent::kCounted, e.flags & PlayEvent::kSkip);
    if (!c.created.empty()) QueueTags(c.created);
    for (const std::wstring& name : c.playlistsRemoved)
        for (int i = 0; i < (int)g.playlists.size(); ++i)
            if (g.playlists[i].name == name &&
                (g.playlists[i].kind == PlaylistKind::Static || g.playlists[i].kind == PlaylistKind::Auto)) {
                ClosePlaylist(i);  // into the reopen stack, as if closed here
                break;
            }
    if (c.Any()) {
        g.libraryDirty = true;
        RebuildRows();
        ShowMessage(c.Summary());
    }
    if (c.saveNow) SaveLibraryFile(true);  // which writes the sync bookkeeping that goes with it
    g.redraw = true;
}

std::string Ago(uint32_t when) {
    if (!when) return "not yet";
    const uint32_t now = UnixNow(), s = now > when ? now - when : 0;
    if (s < 90) return "just now";
    if (s < 90 * 60) return std::to_string((s + 30) / 60) + " minutes ago";
    if (s < 36 * 3600) return std::to_string((s + 1800) / 3600) + " hours ago";
    return std::to_string((s + 43200) / 86400) + " days ago";
}

}  // namespace

void SyncTick() {
    if (g.settings.syncFolder.empty() && g_sync.folder.empty()) return;  // off: nothing to copy or start
    Configure();
    syncfolder::Changes changes;
    if (g_sync.Tick(LiveData(), changes)) Apply(changes);
}

void SyncLibrarySaved() { g_sync.LibrarySaved(); }

void SyncFinish() {
    Configure();
    syncfolder::Changes changes;
    g_sync.Finish(LiveData(), changes);
    for (const PlayEvent& e : changes.listens)
        LogPlay(e.id, e.time, e.seconds, e.flags & PlayEvent::kCounted, e.flags & PlayEvent::kSkip);
    if (changes.Any() || changes.saveNow) g.libraryDirty = true;  // the closing save writes it, then the bookkeeping
}

void SyncFolderChosen(const std::wstring& folder) {
    if (folder.empty()) return;
    g.settings.syncFolder = folder;
    SaveSettings();
    SaveLibraryFile(true);  // so the first pass starts from what's on disk
    Configure();
    syncfolder::Changes changes;
    if (g_sync.Tick(LiveData(), changes, true)) Apply(changes);  // starts the pass on the worker
    ShowMessage(L"Syncing through " + folder + L".");
}

void OpenSync() {
    Configure();
    const syncfolder::Status status = g_sync.CurrentStatus();
    const bool on = !g.settings.syncFolder.empty();
    std::string text;
    if (!on) {
        text = "Pick a folder that Syncthing, Nextcloud, Dropbox or a USB stick keeps the same on your PCs, then pick "
               "the same folder in cordinal on each PC. Plays, ratings, hearts, playlists and places in songs then "
               "follow you between them.\n\nEach PC writes a small log of its own there and only reads the others', so "
               "the sync tool never has two versions of a file to sort out. Your library file stays where it is.";
    } else {
        text = "Syncing through " + ToUtf8(g.settings.syncFolder) + ".\nLast checked " + Ago(status.lastPass) + ".";
        if (!status.problem.empty()) text += "\n\n" + ToUtf8(status.problem);
        if (status.peers.empty()) text += "\n\nNo other PCs yet. Choose this folder in cordinal on another PC too.";
        else {
            text += "\n\nOther PCs:";
            for (const auto& peer : status.peers) text += "\n" + ToUtf8(peer.name) + "  (last change " + Ago(peer.lastSeen) + ")";
        }
    }
    enum { kClose = 0, kChoose, kNow, kStop };
    std::vector<SDL_MessageBoxButtonData> buttons;
    buttons.push_back({SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, kChoose, on ? "Choose another folder..." : "Choose a folder..."});
    if (on) {
        buttons.push_back({0, kNow, "Sync now"});
        buttons.push_back({0, kStop, "Stop syncing"});
    }
    buttons.push_back({SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, kClose, "Close"});
    SDL_MessageBoxData dialog{SDL_MESSAGEBOX_INFORMATION, g.window, "Sync with other PCs", text.c_str(),
                              (int)buttons.size(), buttons.data(), nullptr};
    int answer = kClose;
    if (!SDL_ShowMessageBox(&dialog, &answer)) return;
    if (answer == kChoose) panel::PickFolder(kFileSyncFolder, ToUtf8(g.settings.syncFolder));
    else if (answer == kNow) {
        SaveLibraryFile(true);  // writes any bookkeeping still waiting, so a pass can start
        syncfolder::Changes changes;
        if (g_sync.Tick(LiveData(), changes, true)) Apply(changes);  // starts the pass on the worker
        ShowMessage(L"Checking the sync folder...");
    } else if (answer == kStop) {
        g.settings.syncFolder.clear();
        SaveSettings();
        Configure();
        ShowMessage(L"Stopped syncing. This PC's changes stay here until you choose a folder again.");
    }
}

}  // namespace lx
