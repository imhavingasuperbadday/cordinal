// Album art thumbnails for the library views, decoded on worker threads.
//
// Each successful thumbnail is cached on disk as a small JPEG, keyed
// by the caller's key (path + file time), so a library's covers are decoded once, not on every
// scroll or start. Misses are not persisted: files may be temporarily unavailable or gain a cover.
// The newest requests are served first: whatever is on screen right now.
#pragma once
#include "art.h"

#include <windows.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

struct ThumbResult {
    uint64_t key = 0;
    int size = 0;
    ArtImage image;  // empty: the track has no art
};

class ThumbLoader {
public:
    ~ThumbLoader() { Stop(); }
    // A payload-free message wakes the UI, which drains TakeResult(). At most two completed
    // images wait here (one in reduced memory mode); decoded images cannot accumulate in the
    // Windows message queue. Reduced mode also halves the pending request limit.
    void Start(HWND notify, UINT msg, std::wstring cacheDir, int threads = 2, bool reducedMemory = false);
    void Stop();
    void Request(uint64_t key, const std::wstring& mediaPath, int size);
    std::unique_ptr<ThumbResult> TakeResult();
    struct MemoryUsage { size_t pending, ready, readyBytes; };
    MemoryUsage Memory() const;
    void DropPending();  // e.g. after the view jumped somewhere else
    static uint64_t Key(const std::wstring& path, uint64_t fileTime);

private:
    struct Job {
        uint64_t key;
        std::wstring path;
        int size;
        uint64_t generation;
    };
    void Run();
    std::wstring CacheFile(uint64_t key, int size, const wchar_t* ext) const;

    HWND notify_ = nullptr;
    UINT msg_ = 0;
    std::wstring dir_;
    std::vector<std::thread> threads_;
    mutable std::mutex mx_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;                             // pending requests, newest first when taken
    std::deque<std::unique_ptr<ThumbResult>> results_; // completed images waiting for the UI
    std::set<std::pair<uint64_t, int>> active_;        // (key, size) queued or ready, to dedupe
    uint64_t generation_ = 0;                          // bumped to invalidate in-flight work
    bool quit_ = false;
    size_t pendingLimit_ = 128, readyLimit_ = 2;
};
