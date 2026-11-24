// Linux File-menu migration uses the same folder format and transaction as Windows. Folder
// dialogs review each root; workers do the disk work and return owned snapshots to the event loop.
#include "app.h"
#include "backup.h"
#include "ui_internal.h"
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
    std::vector<migration::ImportedPlaylist> playlists;
    std::vector<migration::PathMap> maps;
    size_t unlocated = 0;
};
struct TrackStatBackup {
    TrackId id = kInvalidTrack;
    uint32_t playCount = 0, skipCount = 0, added = 0, firstPlayed = 0, lastPlayed = 0;
    uint8_t rating = 0;
};
struct MigrationUndo {
    uint64_t when = 0;
    std::wstring description;
    std::vector<TrackStatBackup> tracks;
    std::vector<std::wstring> createdPlaylists;
} migrationUndo;
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
        auto plPlans = migration::PlanPlaylists(g_lib, job->playlists, job->maps);
        size_t plCount = 0, plTracks = 0;
        for (const auto& p : plPlans) {
            if (!p.tracks.empty()) {
                ++plCount;
                plTracks += p.tracks.size();
            }
        }
        std::wstring summary = std::to_wstring(plan.matched.size()) + L" songs matched; " +
                               std::to_wstring(plan.unmatched + job->unlocated) + L" unmatched.";
        if (!job->playlists.empty())
            summary += L" " + std::to_wstring(plCount) + L" playlists (" + std::to_wstring(plTracks) + L" tracks found).";
        summary += L" Counts keep the higher value, existing ratings stay, "
                   L"and listening events are not invented. A backup is saved first, and Ctrl+Z can undo.";
        if (!Ask("Import playback statistics", summary, "Import", "Cancel")) {
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
        static const SDL_DialogFileFilter filter{"Playback statistics & playlists", "xml;csv;tsv;txt;xspf"};
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
                        j.error = L"Couldn't read the export file.";
                    else {
                        migration::ImportBundle bundle;
                        migration::ImportFormat format = migration::ImportFormat::Unknown;
                        if (migration::ReadImportBytes(bytes, ToUtf8(StemOf(j.folder)), bundle, format, j.error)) {
                            j.statistics = std::move(bundle.statistics);
                            j.playlists = std::move(bundle.playlists);
                            j.unlocated = bundle.unlocated;
                        }
                    }
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
        auto plPlans = migration::PlanPlaylists(g_lib, job->playlists, job->maps);
        MigrationUndo undo;
        undo.when = UnixNow();
        undo.tracks.reserve(plan.matched.size());
        for (const auto& [id, _] : plan.matched) {
            if (!g_lib.Contains(id)) continue;
            const Track& t = g_lib[id];
            undo.tracks.push_back({id, t.playCount, t.skipCount, t.added, t.firstPlayed, t.lastPlayed, t.rating});
        }
        migration::ApplyStatistics(g_lib, plan);
        size_t createdPlaylists = 0;
        for (auto& pl : plPlans) {
            if (pl.tracks.empty()) continue;
            Playlist created{UniquePlaylistName(g.playlists, FromUtf8(pl.name)), {}, PlaylistKind::Static};
            created.identity = g_nextPlaylistIdentity++;
            created.items = std::move(pl.tracks);
            undo.createdPlaylists.push_back(created.name);
            g.playlists.push_back(std::move(created));
            ++createdPlaylists;
        }
        undo.description = L"Undo import (" + std::to_wstring(plan.matched.size()) + L" tracks" +
                           (createdPlaylists ? L", " + std::to_wstring(createdPlaylists) + L" playlists" : L"") +
                           L")";
        migrationUndo = std::move(undo);
        g.libraryDirty = true;
        SaveLibraryFile(true);
        RebuildRows();
        ShowMessage(L"Imported statistics. Press Ctrl+Z to undo. Previous profile: " + job->folder);
        pending.reset();
    } else {
        pending = job;
        Roots();
        Review();
    }
}

bool CanUndoMigration(std::wstring& description) {
    if (migrationUndo.tracks.empty() && migrationUndo.createdPlaylists.empty()) return false;
    description = migrationUndo.description;
    return true;
}
uint64_t NewestMigrationTime() {
    return (migrationUndo.tracks.empty() && migrationUndo.createdPlaylists.empty()) ? 0 : migrationUndo.when;
}
void UndoLastMigration() {
    if (migrationUndo.tracks.empty() && migrationUndo.createdPlaylists.empty()) return;
    for (const auto& s : migrationUndo.tracks) {
        if (!g_lib.Contains(s.id)) continue;
        Track& t = g_lib[s.id];
        t.playCount = s.playCount;
        t.skipCount = s.skipCount;
        t.added = s.added;
        t.firstPlayed = s.firstPlayed;
        t.lastPlayed = s.lastPlayed;
        t.rating = s.rating;
    }
    for (const auto& name : migrationUndo.createdPlaylists) {
        for (int i = (int)g.playlists.size() - 1; i >= 0; --i) {
            if (g.playlists[i].kind == PlaylistKind::Static && g.playlists[i].name == name) {
                g.playlists.erase(g.playlists.begin() + i);
                if (g.active >= (int)g.playlists.size()) g.active = std::max(0, (int)g.playlists.size() - 1);
                break;
            }
        }
    }
    if (!migrationUndo.tracks.empty()) g_lib.Touch();
    const std::wstring desc = migrationUndo.description;
    migrationUndo = {};
    g.libraryDirty = true;
    SaveLibraryFile(true);
    RebuildRows();
    ShowMessage(L"Undid " + desc + L".");
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
