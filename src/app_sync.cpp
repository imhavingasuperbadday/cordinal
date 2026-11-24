// Library > Sync with other PCs: the Windows side of syncfolder.h. Picks the folder, runs a pass from the
// UI tick about once a minute, applies what other PCs changed, and publishes this PC's changes at exit.
#include "app.h"
#include "syncfolder.h"

namespace app {
namespace {

syncfolder::Sync g_sync;
bool g_loaded = false;

syncfolder::Live LiveData() { return {g_lib, g.playlists, g.spots, PlayLog()}; }

// The registry's MachineGuid tells this Windows install apart from any other, so a profile copied to
// another PC is noticed there; the profile folder keeps two profiles on one PC apart.
std::string Fingerprint() {
    wchar_t guid[64] = {};
    DWORD size = sizeof guid;
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", L"MachineGuid",
                 RRF_RT_REG_SZ, nullptr, guid, &size);
    std::wstring folder = g.dataDir;
    for (wchar_t& c : folder) c = (wchar_t)towlower(c);
    const std::string text = ToUtf8(guid) + "|" + ToUtf8(folder);
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : text) h = (h ^ c) * 1099511628211ull;
    wchar_t hex[17];
    swprintf(hex, 17, L"%016llx", (unsigned long long)h);
    return ToUtf8(hex);
}

void Configure() {
    if (!g_loaded) {
        g_loaded = true;
        g_sync.folder = IniRead(L"Sync", L"Folder", L"");
        g_sync.stateFile = g.dataDir + L"\\sync-state.txt";
        wchar_t computer[MAX_COMPUTERNAME_LENGTH + 1] = {};
        DWORD size = (DWORD)std::size(computer);
        g_sync.machine = GetComputerNameW(computer, &size) && *computer ? computer : L"Windows PC";
        g_sync.fingerprint = Fingerprint();
    }
    g_sync.roots = g.libraryFolders;
}

void Apply(syncfolder::Changes& c) {
    for (const PlayEvent& e : c.listens)
        LogPlay(e.id, e.time, e.seconds, e.flags & PlayEvent::kCounted, e.flags & PlayEvent::kSkip);
    if (!c.created.empty()) QueueTags(c.created);
    bool activeEdited = false;
    for (int index : c.playlistsEdited) activeEdited |= index == g.active;
    if (c.playlistsAdded) RefreshTabs();  // gives the new playlists their tabs (and identities)
    if (activeEdited && IsWindow(g.list)) RefreshViewKeepingSelection();
    if (!c.playlistsEdited.empty()) RefreshNext();
    for (const std::wstring& name : c.playlistsRemoved)
        for (int i = 1; i < (int)g.playlists.size(); ++i)
            if (g.playlists[i].name == name &&
                (g.playlists[i].kind == PlaylistKind::Static || g.playlists[i].kind == PlaylistKind::Auto)) {
                RemovePlaylist(i);  // into the reopen stack, as if closed here
                break;
            }
    if (c.Any()) {
        g.dirty = true;
        ShowMessage(c.Summary());
    }
    if (c.saveNow) SaveLibraryNow();  // which writes the sync bookkeeping that goes with it
}

std::wstring Ago(uint32_t when) {
    if (!when) return L"not yet";
    const uint32_t now = UnixNow(), s = now > when ? now - when : 0;
    if (s < 90) return L"just now";
    if (s < 90 * 60) return std::to_wstring((s + 30) / 60) + L" minutes ago";
    if (s < 36 * 3600) return std::to_wstring((s + 1800) / 3600) + L" hours ago";
    return std::to_wstring((s + 43200) / 86400) + L" days ago";
}

std::wstring PickSyncFolder() {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return {};
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(L"Choose the folder your PCs share");
    ComPtr<IShellItem> start;
    if (!g_sync.folder.empty() && SUCCEEDED(SHCreateItemFromParsingName(g_sync.folder.c_str(), nullptr, IID_PPV_ARGS(&start))))
        dialog->SetFolder(start.Get());
    ComPtr<IShellItem> item;
    PWSTR raw = nullptr;
    if (FAILED(dialog->Show(g.hwnd)) || FAILED(dialog->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)))
        return {};
    std::wstring path = raw;
    CoTaskMemFree(raw);
    return path;
}

void SetFolder(const std::wstring& folder) {
    g_sync.folder = folder;
    IniWrite(L"Sync", L"Folder", folder);
    SavePreferencesSoon();
}

}  // namespace

void SyncTick(bool now) {
    if (g_loaded && g_sync.folder.empty()) return;  // off: nothing to copy or start
    Configure();
    syncfolder::Changes changes;
    if (g_sync.Tick(LiveData(), changes, now)) Apply(changes);
}

void SyncLibrarySaved() { g_sync.LibrarySaved(); }

// At close, after the last listen is counted and before the final save: publishes everything.
void SyncFinish() {
    Configure();
    syncfolder::Changes changes;
    g_sync.Finish(LiveData(), changes);
    for (const PlayEvent& e : changes.listens)
        LogPlay(e.id, e.time, e.seconds, e.flags & PlayEvent::kCounted, e.flags & PlayEvent::kSkip);
    if (changes.Any() || changes.saveNow) g.dirty = true;  // the closing save writes it, then the bookkeeping
}

void ShowSync() {
    Configure();
    enum { kChoose = 100, kNow, kStop };
    while (true) {
        const syncfolder::Status status = g_sync.CurrentStatus();
        const bool on = !g_sync.folder.empty();
        std::wstring text;
        if (!on) {
            text = L"Pick a folder that Syncthing, Nextcloud, Dropbox or a USB stick keeps the same on your PCs, then "
                   L"pick the same folder in cordinal on each PC. Plays, ratings, hearts, playlists and places in "
                   L"songs then follow you between them.\n\n"
                   L"Each PC writes a small log of its own there and only reads the others', so the sync tool never "
                   L"has two versions of a file to sort out. Your library file stays where it is.";
        } else {
            text = L"Syncing through " + g_sync.folder + L".\nLast checked " + Ago(status.lastPass) + L".";
            if (!status.problem.empty()) text += L"\n\n" + status.problem;
            if (status.peers.empty()) text += L"\n\nNo other PCs yet. Choose this folder in cordinal on another PC too.";
            else {
                text += L"\n\nOther PCs:";
                for (const auto& peer : status.peers) text += L"\n" + peer.name + L"  (last change " + Ago(peer.lastSeen) + L")";
            }
        }
        std::vector<TASKDIALOG_BUTTON> buttons;
        buttons.push_back({kChoose, on ? L"Choose another folder..." : L"Choose a folder..."});
        if (on) {
            buttons.push_back({kNow, L"Sync now"});
            buttons.push_back({kStop, L"Stop syncing\nThe logs stay in the folder; choosing it again carries on."});
        }
        TASKDIALOGCONFIG config{sizeof(config)};
        config.hwndParent = g.hwnd;
        config.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
        config.dwCommonButtons = TDCBF_CLOSE_BUTTON;
        config.pszWindowTitle = L"Sync with other PCs";
        config.pszMainInstruction = L"Keep your plays, ratings and playlists the same on every PC";
        config.pszContent = text.c_str();
        config.cButtons = (UINT)buttons.size();
        config.pButtons = buttons.data();
        int choice = IDCLOSE;
        if (FAILED(TaskDialogIndirect(&config, &choice, nullptr, nullptr))) return;
        if (choice == kChoose) {
            const std::wstring folder = PickSyncFolder();
            if (folder.empty()) continue;
            SetFolder(folder);
            SaveLibraryNow();  // so the first pass starts from what's on disk
            SyncTick(true);
            ShowMessage(L"Syncing through " + folder + L".");
            return;
        }
        if (choice == kNow) {
            SaveLibraryNow();  // writes any bookkeeping still waiting, so a pass can start
            SyncTick(true);    // on the worker; what other PCs changed shows in the status bar
            ShowMessage(L"Checking the sync folder...");
            return;
        }
        if (choice == kStop) {
            SetFolder(L"");
            ShowMessage(L"Stopped syncing. This PC's changes stay here until you choose a folder again.");
            continue;
        }
        return;
    }
}

}  // namespace app
