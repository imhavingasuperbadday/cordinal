// Folders and files handed to cordinal (the command line, a file manager, drag and drop) are walked on
// one worker, in the order they arrive. Walking a large folder takes seconds, so the UI thread only
// receives the audio files it is to add, through WM_APP_EXPAND; the results wait here under a lock.
#include "app.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>

namespace lx {
namespace {

struct Request {
    Opening how;
    uint64_t serial;
    std::vector<std::wstring> paths;
};
struct Done {
    Opening how;
    uint64_t serial;
    std::vector<std::wstring> files;
};

std::mutex g_mx;
std::condition_variable g_cv;
std::deque<Request> g_requests;
std::deque<Done> g_done;
bool g_quit = false;
std::atomic<bool> g_cancel{false};
std::thread g_worker;
uint64_t g_nextSerial = 0;  // UI thread: requests are numbered as they arrive
uint64_t g_heldBefore = 0;  // UI thread: Play requests numbered below this add their files without playing

void Work() {
    for (;;) {
        Request r;
        {
            std::unique_lock lock(g_mx);
            g_cv.wait(lock, [] { return g_quit || !g_requests.empty(); });
            if (g_cancel || g_requests.empty()) return;
            r = std::move(g_requests.front());
            g_requests.pop_front();
        }
        Done d{r.how, r.serial, {}};
        for (const auto& path : r.paths) {
            if (g_cancel) break;
            CollectAudioFiles(path, d.files, &g_cancel);
        }
        if (g_cancel) return;
        {
            std::lock_guard lock(g_mx);
            g_done.push_back(std::move(d));
        }
        PostMessageW(nullptr, WM_APP_EXPAND, 0, 0);
    }
}

}  // namespace

void ExpandPaths(std::vector<std::wstring> paths, Opening how) {
    if (paths.empty()) return;
    {
        std::lock_guard lock(g_mx);
        g_requests.push_back({how, g_nextSerial++, std::move(paths)});
        if (!g_worker.joinable()) g_worker = std::thread(Work);
    }
    g_cv.notify_one();
}

// UI thread, from WM_APP_EXPAND: each finished expansion, in the order it was requested.
void OnExpanded() {
    for (;;) {
        Done d;
        {
            std::lock_guard lock(g_mx);
            if (g_done.empty()) return;
            d = std::move(g_done.front());
            g_done.pop_front();
        }
        if (d.how == Opening::Play) OpenFiles(d.files, d.serial >= g_heldBefore);
        else AddDropped(d.files);
    }
}

// A stop the user asked for wins over files opened before it that are still being walked: they
// arrive in Now Playing but don't start playing.
void HoldPendingOpens() { g_heldBefore = g_nextSerial; }

// Shutdown: an expansion in progress is abandoned, since its files would not be added anyway.
void StopExpanders() {
    {
        std::lock_guard lock(g_mx);
        g_quit = true;
        g_cancel = true;
    }
    g_cv.notify_all();
    if (g_worker.joinable()) g_worker.join();
}

}  // namespace lx
