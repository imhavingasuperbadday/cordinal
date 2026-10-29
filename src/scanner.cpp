// The library folder scanner: recursive listing, per-root directory watches and the debounce
// loop that turns change notifications into re-lists. It runs on its own thread and never
// mutates the library; results leave as posted ScanResult batches.
#include "sort.h"
#include "scanner.h"

#include "archive.h"
#include "cue.h"
#include "library.h"

#include <algorithm>
#include <functional>

#ifndef _WIN32
#include <sys/inotify.h>
#include <unistd.h>

#include <unordered_map>
#endif

namespace {

constexpr DWORD kRetryMs = 30000;      // a missing library folder is looked for again this often
#ifdef _WIN32
constexpr DWORD kWatchPollMs = 100;    // fallback when roots exceed Win32's wait-handle limit
constexpr DWORD kNotifyFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE;
#else
constexpr DWORD kWatchPollMs = 200;    // how often the watch loop reads inotify's queue
#endif
constexpr size_t kScanChunk = 4096;    // audio files per posted chunk; the tail always closes a scope

// A missing drive is not an empty folder: "unreachable" suppresses missing-track marking.
bool Reachable(const std::wstring& dir) {
    DWORD a = GetFileAttributesW(dir.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// True when `path` is strictly inside `dir` (case-insensitive ordinal prefix comparison).
bool IsUnder(const std::wstring& path, const std::wstring& dir) {
    return !dir.empty() && path.size() > dir.size() && (path[dir.size()] == kPathSep || dir.back() == kPathSep) &&
           CompareStringOrdinal(path.c_str(), (int)dir.size(), dir.c_str(), (int)dir.size(), TRUE) == CSTR_EQUAL;
}

#ifdef _WIN32
// One root's ReadDirectoryChangesW state: an overlapped read kept armed so changes queue up even
// while the scanner is busy. Arm() (re)issues the read; Close() cancels it and drops the buffer.
struct Watch {
    std::wstring root;
    HANDLE dir = INVALID_HANDLE_VALUE;
    OVERLAPPED ov{};
    std::vector<DWORD> buf;  // 16 KB, DWORD aligned as the API requires; allocated only while armed
    bool armed = false;

    bool Arm() {
        if (dir == INVALID_HANDLE_VALUE) {
            dir = CreateFileW(root.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
            if (dir == INVALID_HANDLE_VALUE) return armed = false;
            if (!ov.hEvent) ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!ov.hEvent) {
                CloseHandle(dir);
                dir = INVALID_HANDLE_VALUE;
                return armed = false;
            }
        }
        if (buf.empty()) buf.resize(4096);
        ResetEvent(ov.hEvent);
        armed = ReadDirectoryChangesW(dir, buf.data(), (DWORD)(buf.size() * sizeof(DWORD)), TRUE, kNotifyFilter, nullptr,
                                      &ov, nullptr) != FALSE;
        if (!armed) Close();
        return armed;
    }
    void Close() {
        if (dir != INVALID_HANDLE_VALUE) {
            CancelIoEx(dir, &ov);
            DWORD n;
            GetOverlappedResult(dir, &ov, &n, TRUE);
            CloseHandle(dir);
            dir = INVALID_HANDLE_VALUE;
        }
        std::vector<DWORD>().swap(buf);
        armed = false;
    }
    ~Watch() {
        Close();
        if (ov.hEvent) CloseHandle(ov.hEvent);
    }
};
#endif

// Lists the audio files under `dir` (recursively) in batches: `full` receives every complete
// batch of kScanChunk files for the caller to post and release, and `tail` receives the files
// left over (always the last, smaller batch), so a folder of a few thousand files is one batch.
// Returns false if cancelled or any directory could not be fully listed. Files already found
// are still useful, but an incomplete listing cannot establish that unseen tracks are missing.
bool ListAudioFilesChunked(const std::wstring& dir, const std::function<void(std::vector<ScanFile>&)>& full,
                           std::vector<ScanFile>& tail, const std::atomic<bool>* cancel) {
    std::vector<std::wstring> stack{dir};
    bool opened = false, complete = true;
    std::vector<ScanFile> batch;
    auto add = [&](ScanFile&& f) {
        batch.push_back(std::move(f));
        if (batch.size() >= kScanChunk) {
            full(batch);
            batch.clear();
        }
    };
    while (!stack.empty()) {
        if (cancel && *cancel) return false;
        std::wstring cur = std::move(stack.back());
        stack.pop_back();
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW((cur + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                    FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) {
            complete = false;
            continue;
        }
        opened |= cur.size() == dir.size();
        std::vector<ScanFile> files;  // this folder's audio files, before their cue sheets are applied
        do {
            if (cancel && *cancel) {
                FindClose(h);
                return false;
            }
            const wchar_t* name = fd.cFileName;
            if (name[0] == L'.' && (!name[1] || (name[1] == L'.' && !name[2]))) continue;
            std::wstring path = cur + kPathSep + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                // No links (they can loop) and no hidden system folders ($RECYCLE.BIN, ...).
                bool hiddenSystem = (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) ==
                                    (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && !hiddenSystem) stack.push_back(std::move(path));
            } else if (IsSupportedAudioFile(path)) {
                files.push_back({std::move(path), (uint64_t)fd.nFileSizeHigh << 32 | fd.nFileSizeLow,
                                 (uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32 | fd.ftLastWriteTime.dwLowDateTime});
            } else if (IsArchiveFile(path)) {
                // A ZIP's songs carry the archive's size and time: rewriting it re-reads them all.
                std::vector<std::wstring> members;
                ListArchive(path, members);
                for (auto& m : members)
                    if (IsSupportedAudioFile(m))
                        files.push_back({std::move(m), (uint64_t)fd.nFileSizeHigh << 32 | fd.nFileSizeLow,
                                         (uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32 | fd.ftLastWriteTime.dwLowDateTime});
            }
        } while (FindNextFileW(h, &fd));
        if (GetLastError() != ERROR_NO_MORE_FILES) complete = false;
        FindClose(h);
        if (files.empty()) continue;
        // A file covered by a .cue (or with an embedded sheet) is listed as its tracks only. A track
        // carries the newer of the audio's and the cue's write time, so editing the cue makes the
        // next rescan re-read it.
        CueFolder folder(cur);
        for (ScanFile& f : files) {
            uint64_t cueTime = 0;
            std::vector<std::wstring> tracks = folder.Expand(f.path, &cueTime);
            if (tracks.empty()) {
                add(std::move(f));
                continue;
            }
            for (auto& track : tracks) add({std::move(track), f.size, std::max(f.time, cueTime)});
        }
    }
    tail.insert(tail.end(), std::make_move_iterator(batch.begin()), std::make_move_iterator(batch.end()));
    return opened && complete;
}

}  // namespace

bool ListAudioFiles(const std::wstring& dir, std::vector<ScanFile>& out, const std::atomic<bool>* cancel) {
    return ListAudioFilesChunked(
        dir,
        [&](std::vector<ScanFile>& batch) {
            out.insert(out.end(), std::make_move_iterator(batch.begin()), std::make_move_iterator(batch.end()));
            batch.clear();
        },
        out, cancel);
}

void FolderScanner::Start(HWND notify, UINT msg, std::vector<std::wstring> roots, DWORD quietMs) {
    Stop();
    notify_ = notify;
    msg_ = msg;
    quietMs_ = quietMs ? quietMs : kScanQuietMs;
    roots_ = std::move(roots);
    for (auto& r : roots_)
        while (r.size() > (kPathSep == L'/' ? 1u : 3u) && (r.back() == L'\\' || r.back() == L'/')) r.pop_back();
    quit_ = false;
    rescan_ = false;
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!wake_) return;
    busy_ = true;
    thread_ = std::thread(&FolderScanner::Run, this);
}

void FolderScanner::Stop() {
    if (!thread_.joinable()) return;
    quit_ = true;
    SetEvent(wake_);
    thread_.join();
    CloseHandle(wake_);
    wake_ = nullptr;
    busy_ = false;
}

void FolderScanner::RescanAll() {
    rescan_ = true;
    if (wake_) SetEvent(wake_);
}

// Ownership of `r` passes to the receiving window; if posting fails, free it here so it can't leak.
void FolderScanner::Post(ScanResult* r, WPARAM wp) {
    if (!PostMessageW(notify_, msg_, wp, (LPARAM)r)) delete r;
}

#ifdef _WIN32
void FolderScanner::Run() {
    // Watch before listing so changes during a long initial scan are caught afterwards.
    std::vector<std::unique_ptr<Watch>> watches;
    for (auto& root : roots_) {
        if (quit_) return;
        watches.push_back(std::make_unique<Watch>());
        watches.back()->root = root;
        watches.back()->Arm();
    }
    auto scan = [&](const std::wstring& scope, const std::wstring& root, bool initial) {
        bool rootOk = Reachable(root);
        bool any = false;
        auto post = [&](bool complete, std::vector<ScanFile>&& files) {
            auto* r = new ScanResult{scope, rootOk, initial, !any, complete, std::move(files)};
            any = true;
            if (quit_) delete r;
            else Post(r);
        };
        if (!rootOk) {
            post(true, {});
            return;
        }
        std::vector<ScanFile> tail;
        const bool listed = ListAudioFilesChunked(scope, [&](std::vector<ScanFile>& batch) { post(false, std::move(batch)); }, tail, &quit_);
        if (quit_) return;  // stopped: a later listing of this scope starts over (ScanResult.first)
        // A folder can be reachable but still fail to enumerate (permissions, a transient lock):
        // treat that like an unreachable root so the empty listing cannot mark its tracks missing.
        rootOk = listed;
        post(true, std::move(tail));
    };
    for (auto& root : roots_) scan(root, root, true);
    busy_ = false;
    Post(nullptr, 1);

    std::vector<std::wstring> dirty;
    ULONGLONG lastChange = 0, lastRetry = GetTickCount64();
    while (!quit_) {
        std::vector<HANDLE> handles{wake_};
        std::vector<Watch*> armed;
        bool anyDown = false;
        for (auto& w : watches) {
            if (w->armed) {
                handles.push_back(w->ov.hEvent);
                armed.push_back(w.get());
            } else {
                anyDown = true;
            }
        }
        DWORD timeout = !dirty.empty() ? quietMs_ : anyDown ? kRetryMs : INFINITE;
        size_t changed = armed.size();
        if (handles.size() <= MAXIMUM_WAIT_OBJECTS) {
            DWORD r = WaitForMultipleObjects((DWORD)handles.size(), handles.data(), FALSE, timeout);
            if (r > WAIT_OBJECT_0 && r < WAIT_OBJECT_0 + handles.size()) changed = r - WAIT_OBJECT_0 - 1;
        } else {
            // WaitForMultipleObjects fails above 64 handles. Keep every root watched and
            // check their events periodically while still waking promptly for Stop/Rescan.
            DWORD r = WaitForSingleObject(wake_, std::min(timeout, kWatchPollMs));
            if (r == WAIT_TIMEOUT)
                for (size_t i = 0; i < armed.size(); ++i)
                    if (WaitForSingleObject(armed[i]->ov.hEvent, 0) == WAIT_OBJECT_0) {
                        changed = i;
                        break;
                    }
        }
        if (quit_) break;
        if (rescan_.exchange(false)) {
            busy_ = true;
            for (auto& root : roots_) scan(root, root, false);
            busy_ = false;
            dirty.clear();
            continue;
        }
        if (changed < armed.size()) {
            Watch& w = *armed[changed];
            DWORD bytes = 0;
            if (!GetOverlappedResult(w.dir, &w.ov, &bytes, FALSE)) {
                w.Close();  // the folder went away (drive unplugged, folder deleted)
                dirty.push_back(w.root);
            } else if (bytes == 0) {
                dirty.push_back(w.root);  // the change buffer overflowed: re-list everything
                w.Arm();
            } else {
                auto* p = reinterpret_cast<const BYTE*>(w.buf.data());
                for (;;) {
                    auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(p);
                    std::wstring full = w.root + L"\\" + std::wstring(info->FileName, info->FileNameLength / sizeof(wchar_t));
                    DWORD attrs = GetFileAttributesW(full.c_str());
                    bool gone = info->Action == FILE_ACTION_REMOVED || info->Action == FILE_ACTION_RENAMED_OLD_NAME;
                    // Audio files dirty their folder; so does a folder appearing or disappearing.
                    if (IsSupportedAudioFile(full) || IsCueFile(full) || IsArchiveFile(full) ||
                        (attrs != INVALID_FILE_ATTRIBUTES ? (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0 : gone))
                        dirty.push_back(DirectoryOf(full));
                    if (!info->NextEntryOffset) break;
                    p += info->NextEntryOffset;
                }
                w.Arm();
            }
            lastChange = GetTickCount64();
            continue;
        }
        ULONGLONG now = GetTickCount64();
        if (anyDown && now - lastRetry >= kRetryMs) {
            lastRetry = now;
            for (auto& w : watches)
                if (!w->armed && w->Arm()) dirty.push_back(w->root);  // it's back: catch up
        }
        if (dirty.empty() || now - lastChange < quietMs_) continue;
        // Re-list each changed folder once, skipping folders inside another changed folder.
        SortObjects(dirty.begin(), dirty.end());
        dirty.erase(std::unique(dirty.begin(), dirty.end()), dirty.end());
        std::vector<std::wstring> scopes;
        for (auto& d : dirty)
            if (std::none_of(scopes.begin(), scopes.end(), [&](const std::wstring& s) { return IsUnder(d, s); }))
                scopes.push_back(d);
        dirty.clear();
        for (auto& scope : scopes) {
            auto root = std::find_if(roots_.begin(), roots_.end(), [&](const std::wstring& rt) {
                return IsUnder(scope, rt) || CompareStringOrdinal(scope.c_str(), -1, rt.c_str(), -1, TRUE) == CSTR_EQUAL;
            });
            if (root != roots_.end()) scan(scope, *root, false);
        }
    }
}
#else
// Linux: inotify watches each directory under the roots (it isn't recursive), added as
// directories appear. The listing, the debounce and the re-listing match the Windows loop.
void FolderScanner::Run() {
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    std::unordered_map<int, std::wstring> dirs;  // watch descriptor -> directory
    std::vector<bool> up(roots_.size(), false);
    constexpr uint32_t kMask = IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE | IN_DELETE_SELF |
                               IN_MOVE_SELF | IN_ONLYDIR;
    std::function<void(const std::wstring&)> watchTree = [&](const std::wstring& dir) {
        if (fd < 0 || quit_) return;
        int wd = inotify_add_watch(fd, compat::Narrow(dir.c_str()).c_str(), kMask);
        if (wd < 0) return;
        dirs[wd] = dir;
        WIN32_FIND_DATAW data;
        HANDLE h = FindFirstFileW((dir + L"/*").c_str(), &data);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            const wchar_t* name = data.cFileName;
            if (name[0] == L'.') continue;  // ".", "..", hidden folders
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                watchTree(dir + L"/" + name);
        } while (FindNextFileW(h, &data));
        FindClose(h);
    };
    for (size_t i = 0; i < roots_.size(); ++i)
        if (Reachable(roots_[i])) {
            watchTree(roots_[i]);
            up[i] = true;
        }

    auto scan = [&](const std::wstring& scope, const std::wstring& root, bool initial) {
        bool rootOk = Reachable(root);
        bool any = false;
        auto post = [&](bool complete, std::vector<ScanFile>&& files) {
            auto* r = new ScanResult{scope, rootOk, initial, !any, complete, std::move(files)};
            any = true;
            if (quit_) delete r;
            else Post(r);
        };
        if (!rootOk) {
            post(true, {});
            return;
        }
        std::vector<ScanFile> tail;
        const bool listed = ListAudioFilesChunked(scope, [&](std::vector<ScanFile>& batch) { post(false, std::move(batch)); }, tail, &quit_);
        if (quit_) return;
        rootOk = listed;
        post(true, std::move(tail));
    };
    for (auto& root : roots_) scan(root, root, true);
    busy_ = false;
    Post(nullptr, 1);

    std::vector<std::wstring> dirty;
    ULONGLONG lastChange = 0, lastRetry = GetTickCount64();
    alignas(inotify_event) char buf[64 * 1024];
    while (!quit_) {
        WaitForSingleObject(wake_, kWatchPollMs);
        if (quit_) break;
        if (rescan_.exchange(false)) {
            busy_ = true;
            for (auto& root : roots_) scan(root, root, false);
            busy_ = false;
            dirty.clear();
            continue;
        }
        bool changed = false;
        for (ssize_t n; fd >= 0 && (n = read(fd, buf, sizeof(buf))) > 0;) {
            for (char* p = buf; p < buf + n;) {
                auto* e = reinterpret_cast<inotify_event*>(p);
                p += sizeof(inotify_event) + e->len;
                changed = true;
                if (e->mask & IN_Q_OVERFLOW) {  // events were lost: re-list everything
                    dirty.insert(dirty.end(), roots_.begin(), roots_.end());
                    continue;
                }
                auto it = dirs.find(e->wd);
                if (it == dirs.end()) continue;
                if (e->mask & IN_IGNORED) {  // the directory went away (or was moved)
                    dirty.push_back(DirectoryOf(it->second));
                    dirs.erase(it);
                    continue;
                }
                if (!e->len) continue;
                std::wstring full = it->second + L"/" + compat::Widen(e->name);
                bool isDir = e->mask & IN_ISDIR;
                if (isDir && (e->mask & (IN_CREATE | IN_MOVED_TO))) watchTree(full);
                // Audio files dirty their folder; so does a folder appearing or disappearing.
                if (isDir || IsSupportedAudioFile(full) || IsCueFile(full) || IsArchiveFile(full)) dirty.push_back(it->second);
            }
        }
        if (changed) lastChange = GetTickCount64();
        ULONGLONG now = GetTickCount64();
        if (now - lastRetry >= kRetryMs) {  // a library folder that was missing may be back
            lastRetry = now;
            for (size_t i = 0; i < roots_.size(); ++i) {
                bool reachable = Reachable(roots_[i]);
                if (reachable && !up[i]) {
                    watchTree(roots_[i]);
                    dirty.push_back(roots_[i]);
                }
                up[i] = reachable;
            }
        }
        if (dirty.empty() || now - lastChange < kScanQuietMs) continue;
        SortObjects(dirty.begin(), dirty.end());
        dirty.erase(std::unique(dirty.begin(), dirty.end()), dirty.end());
        std::vector<std::wstring> scopes;
        for (auto& d : dirty)
            if (std::none_of(scopes.begin(), scopes.end(), [&](const std::wstring& s) { return IsUnder(d, s); }))
                scopes.push_back(d);
        dirty.clear();
        for (auto& scope : scopes) {
            auto root = std::find_if(roots_.begin(), roots_.end(), [&](const std::wstring& rt) {
                return IsUnder(scope, rt) || scope == rt;
            });
            if (root != roots_.end()) scan(scope, *root, false);
        }
    }
    if (fd >= 0) close(fd);
}
#endif
