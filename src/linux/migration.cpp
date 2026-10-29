// Linux File-menu migration uses the same folder format and transaction as Windows. Folder
// dialogs review each root; workers do the disk work and return owned snapshots to the event loop.
#include "app.h"
#include "backup.h"
#include <set>
#include <functional>
#include <unistd.h>

namespace lx {
namespace {
constexpr UINT kMessage = WM_APP + 32;
enum { Backup = 1, Restore, Statistics, Folder, Ready, Staged, Applied };
struct Job {
    int kind;
    std::wstring folder, error, stage;
    LibraryArchive data;
    std::string settings;
    std::vector<migration::Statistics> statistics;
    std::vector<migration::PathMap> maps;
};
std::shared_ptr<Job> pending;
std::thread worker;
std::atomic<bool> cancel{false};
bool busy = false, stopping = false;
std::wstring restoreStage;
size_t nextRoot = 0;

void Start(std::function<void(Job&)> task, std::shared_ptr<Job> job) {
    if (worker.joinable()) worker.join();
    busy = true;
    cancel = false;
    worker = std::thread([task = std::move(task), job] {
        try {
            task(*job);
        } catch (...) {
            job->error = L"Couldn't finish the backup operation.";
        }
        auto* result = new std::shared_ptr<Job>(job);
        if (!PostMessageW(nullptr, kMessage, 0, (LPARAM)result)) delete result;
    });
}

void Chosen(void* user, const char* const* paths, int) {
    auto path = new std::wstring(paths && *paths ? FromUtf8(*paths) : L"");
    if (!PostMessageW(nullptr, kMessage, (WPARAM)(intptr_t)user, (LPARAM)path)) delete path;
}

int Ask(const char* title, const std::wstring& text, const char* yes, const char* no) {
    SDL_MessageBoxButtonData buttons[] = {{SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, yes},
                                          {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, no}};
    auto utf8 = ToUtf8(text);
    SDL_MessageBoxData dialog{SDL_MESSAGEBOX_INFORMATION, g.window, title, utf8.c_str(), 2, buttons, nullptr};
    int result = 0;
    SDL_ShowMessageBox(&dialog, &result);
    return result;
}

void Roots() {
    auto& job = *pending;
    std::set<std::wstring> roots;
    if (job.kind == Restore) {
        std::string profile;
        if (backup::ReadBytes(job.folder + L"/profile-path.txt", profile, 65536) && !profile.empty())
            job.maps.push_back({FromUtf8(profile), g.dataDir});
        std::string_view ini = job.settings;
        while (!ini.empty()) {
            auto end = std::min(ini.find('\n'), ini.size());
            auto line = ini.substr(0, end);
            if (line.starts_with("folder")) {
                auto equals = line.find('=');
                if (equals != line.npos && equals + 1 < line.size())
                    roots.insert(FromUtf8(line.substr(equals + 1)));
            }
            ini.remove_prefix(std::min(end + 1, ini.size()));
        }
        if (roots.empty())
            for (TrackId id = 0; id < job.data.library.Count(); ++id)
                roots.insert(DirectoryOf(job.data.library.Path(id)));
    } else
        for (const auto& row : job.statistics) roots.insert(DirectoryOf(row.path));
    for (const auto& root : roots) job.maps.push_back({root, root});
    nextRoot = 0;
}

void Review() {
    while (nextRoot < pending->maps.size()) {
        auto& map = pending->maps[nextRoot];
        if (Ask("Music folder location",
                L"Music originally at:\n" + map.from + L"\n\nChoose its location on this computer.",
                "Choose new location", "Keep this path")) {
            auto initial = ToUtf8(map.to);
            SDL_ShowOpenFolderDialog(Chosen, (void*)(intptr_t)Folder, g.window, initial.c_str(), false);
            return;
        }
        ++nextRoot;
    }
    auto job = pending;
    if (job->kind == Restore) {
        if (!migration::RemapLibrary(job->data, job->maps, job->error)) {
            ShowMessage(job->error);
            pending.reset();
            return;
        }
        job->settings = backup::RemapSettings(job->settings, job->maps);
        if (!Ask("Restore backup",
                 std::to_wstring(job->data.library.Count()) + L" songs and " +
                     std::to_wstring(job->data.playlists.size()) +
                     L" playlists will replace this profile. Music is not copied. "
                     L"The previous profile is retained beside it. Restart cordinal after the restore "
                     L"finishes.",
                 "Restore and close", "Cancel")) {
            pending.reset();
            return;
        }
        auto profile = g.dataDir;
        ShowMessage(L"Preparing the restored profile...");
        Start(
            [profile](Job& j) {
                backup::Stage(j.folder, profile, j.settings, j.data, cancel, j.stage, j.error);
                j.kind = Staged;
            },
            job);
    } else {
        auto plan = migration::PlanStatistics(g_lib, job->statistics, job->maps);
        if (!Ask("Import playback statistics",
                 std::to_wstring(plan.matched.size()) + L" songs matched; " +
                     std::to_wstring(plan.unmatched) +
                     L" unmatched. Counts keep the higher value, existing ratings stay, "
                     L"and listening events are not invented. A backup is saved first.",
                 "Import", "Cancel")) {
            pending.reset();
            return;
        }
        SaveSettings();
        job->data = CaptureLibraryArchive(g.playlists);
        if (!backup::ReadBytes(g.settingsPath, job->settings)) {
            ShowMessage(L"Couldn't read settings.");
            pending.reset();
            return;
        }
        auto profile = g.dataDir;
        job->folder = profile + L".before-statistics-" + std::to_wstring(UnixNow());
        Start(
            [profile](Job& j) {
                backup::Create(j.folder, profile, j.settings, j.data, cancel, j.error);
                j.kind = Applied;
            },
            job);
    }
}
} // namespace

void ChooseMigration(int kind) {
    if (busy || pending) {
        ShowMessage(L"A backup operation is already open.");
        return;
    }
    if (kind == Statistics) {
        static const SDL_DialogFileFilter filter{"Playback statistics CSV", "csv"};
        SDL_ShowOpenFileDialog(Chosen, (void*)(intptr_t)kind, g.window, &filter, 1, nullptr, false);
    } else
        SDL_ShowOpenFolderDialog(Chosen, (void*)(intptr_t)kind, g.window, nullptr, false);
}

void OnMigration(WPARAM kind, LPARAM value) {
    if (kind) {
        std::unique_ptr<std::wstring> path((std::wstring*)value);
        if (path->empty() || stopping) {
            pending.reset();
            return;
        }
        if (kind == Folder) {
            pending->maps[nextRoot++].to = *path;
            Review();
            return;
        }
        if (busy || pending) return;
        auto job = std::make_shared<Job>();
        job->kind = (int)kind;
        job->folder = *path;
        if (kind == Backup) {
            SaveSettings();
            job->data = CaptureLibraryArchive(g.playlists);
            if (!backup::ReadBytes(g.settingsPath, job->settings)) {
                ShowMessage(L"Couldn't read settings.");
                return;
            }
            job->folder += L"/cordinal-backup-" + std::to_wstring(UnixNow());
            auto profile = g.dataDir;
            Start(
                [profile](Job& j) { backup::Create(j.folder, profile, j.settings, j.data, cancel, j.error); },
                job);
        } else if (kind == Restore)
            Start(
                [](Job& j) {
                    if (backup::Read(j.folder, j.data, j.settings, j.error) &&
                        !j.settings.starts_with("# cordinal settings\n"))
                        j.error = L"This backup contains Windows settings. Restore it with the Windows "
                                  L"player; CSV statistics can be imported here.";
                },
                job);
        else
            Start(
                [](Job& j) {
                    std::string bytes;
                    if (!backup::ReadBytes(j.folder, bytes))
                        j.error = L"Couldn't read statistics CSV.";
                    else
                        migration::ReadStatisticsCsv(bytes, j.statistics, j.error);
                },
                job);
        ShowMessage(L"Reading or writing the backup...");
        return;
    }
    std::unique_ptr<std::shared_ptr<Job>> delivered((std::shared_ptr<Job>*)value);
    if (worker.joinable()) worker.join();
    busy = false;
    auto job = *delivered;
    if (stopping || cancel) {
        pending.reset();
        return;
    }
    if (!job->error.empty()) {
        ShowMessage(job->error);
        pending.reset();
        return;
    }
    if (job->kind == Backup) {
        ShowMessage(L"Backup saved to " + job->folder);
        pending.reset();
    } else if (job->kind == Staged) {
        restoreStage = job->stage;
        pending.reset();
        g.quit = true;
    } else if (job->kind == Applied) {
        auto plan = migration::PlanStatistics(g_lib, job->statistics, job->maps);
        migration::ApplyStatistics(g_lib, plan);
        g.libraryDirty = true;
        SaveLibraryFile(true);
        RebuildRows();
        ShowMessage(L"Imported statistics. Previous profile: " + job->folder);
        pending.reset();
    } else {
        pending = job;
        Roots();
        Review();
    }
}

void StopMigration() {
    stopping = true;
    cancel = true;
    if (worker.joinable()) worker.join();
    pending.reset();
}

void FinishMigration() {
    if (restoreStage.empty()) return;
    std::wstring previous, error;
    if (!backup::Commit(g.dataDir, restoreStage, previous, error))
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Couldn't restore the backup", ToUtf8(error).c_str(), nullptr);
}
} // namespace lx
