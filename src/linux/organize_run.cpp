// Organizing files on Linux, as app_organize.cpp does on Windows: the confirmed plan from the Organize
// panel (panels.cpp) moves on a worker with folder scanning paused, so the moves aren't mistaken for
// files vanishing and appearing, and the library follows each moved file. Library > Undo last organize
// reverses the newest run. The filesystem work is organize.cpp's.
#include "app.h"

#include <memory>

namespace lx {
namespace {

std::thread worker;
bool moving = false;    // a run is in flight
bool stopping = false;  // shutting down: the library is updated, nothing is said
OrganizeResult* finished = nullptr;  // the worker's result until the UI thread takes it

}  // namespace

bool OrganizeBusy() { return moving; }
std::wstring OrganizeLogDir() { return g.dataDir + L"/organize"; }

// Asks first, then hands the plan to the worker; the result arrives as WM_APP_ORGANIZE. `root` is
// kept through the empty-folder cleanup even if all its files moved away.
void StartOrganize(const std::vector<OrganizeMove>& plan, const std::wstring& root) {
    if (moving) return;
    if (ConvertBusy() || CordExportBusy() || TagWriteBusy() || CopyBusy()) {
        ShowMessage(L"Wait for the running conversion, export, tag write or copy to finish.");
        return;
    }
    size_t count = 0;
    for (const OrganizeMove& m : plan) count += m.skip.empty();
    if (!count) return;
    std::wstring q = L"Move and rename " + std::to_wstring(count) + (count == 1 ? L" file" : L" files") + L" into\n" + root +
                     L"?\n\nYou can undo this with Library > Undo last organize.";
    if (!Confirm(ToUtf8(q), "Organize files")) return;
    g.scanner.Stop();
    moving = true;
    std::vector<std::wstring> keep = g.settings.folders;
    keep.push_back(root);
    if (worker.joinable()) worker.join();
    worker = std::thread([plan, logDir = OrganizeLogDir(), keep]() {
        finished = new OrganizeResult(ExecuteOrganize(plan, logDir, keep));
        PostMessageW(nullptr, WM_APP_ORGANIZE, 0, 0);
    });
    ShowMessage(L"Organizing " + std::to_wstring(count) + (count == 1 ? L" file..." : L" files..."));
}

// On the UI thread: the moved tracks take their new paths (keeping their ids, plays and playlists)
// and folder scanning starts again.
void OnOrganizeDone() {
    if (worker.joinable()) worker.join();
    std::unique_ptr<OrganizeResult> r(finished);
    finished = nullptr;
    if (!r) return;
    moving = false;
    for (auto& [id, path] : r->moved)
        if (g_lib.Contains(id)) g_lib.Rename(id, path);
    g_lib.Touch();
    g.libraryDirty = true;
    RebuildRows();
    if (stopping) return;
    StartScanner();
    if (!r->failed.empty()) {
        std::string text = std::to_string(r->failed.size()) + " file(s) couldn't be moved:\n\n";
        for (size_t i = 0; i < r->failed.size() && i < 8; ++i)
            text += ToUtf8(r->failed[i].first) + "\n    " + ToUtf8(r->failed[i].second) + "\n";
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "Some files couldn't be moved", text.c_str(), g.window);
    }
    ShowMessage(L"Organized " + std::to_wstring(r->moved.size()) + L" file(s).");
    PlaySfx(r->failed.empty() ? Sfx::Organized : Sfx::Oops);
}

// Reverses the newest undo log and renames the affected tracks back, with scanning paused as above.
void UndoOrganize() {
    if (moving) return;
    if (!Confirm("Move the files of the last organize back where they were?", "Undo organize")) return;
    g.scanner.Stop();
    std::wstring error;
    const auto undone = UndoLastOrganize(OrganizeLogDir(), g.settings.folders, error);
    for (auto& [from, to] : undone)
        if (TrackId id = g_lib.Find(to); id != kNone) g_lib.Rename(id, from);
    g_lib.Touch();
    g.libraryDirty = true;
    RebuildRows();
    StartScanner();
    ShowMessage(error.empty() ? L"Moved " + std::to_wstring(undone.size()) + L" file(s) back." : error);
}

// Shutdown: a run in progress finishes (each move is logged as it goes) and the library follows it
// before it is saved.
void StopOrganizer() {
    stopping = true;
    if (worker.joinable()) worker.join();
    if (finished) OnOrganizeDone();
}

}  // namespace lx
