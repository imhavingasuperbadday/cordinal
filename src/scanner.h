// Library folders: a background thread that lists the audio files under each folder, then
// watches the folders and re-lists only the directories that changed (debounced).
//
// The scanner never touches the library itself: it posts ScanResult batches to the notify
// window and the UI thread compares them with what the library knows. One thread serves every
// root; it stops by setting quit_ and signalling the wake event.
#pragma once
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct ScanFile {
    std::wstring path;
    uint64_t size = 0, time = 0;  // FILETIME of the last write
};

struct ScanResult {
    std::wstring scope;     // everything under this folder, across this scope's chunks
    bool rootOk = true;     // listing is complete enough to infer missing tracks; failures clear it
    bool initial = false;   // part of the first scan after start (files go straight to the library)
    bool first = true;      // the first chunk of this scope's listing: reset any earlier partial listing
    bool complete = true;   // the last chunk: the whole scope is listed, so missing tracks can be told
    std::vector<ScanFile> files;
};

// Lists audio files under `dir` (recursively, skipping links and hidden system folders).
// Returns false if any directory couldn't be fully listed, or the listing was cancelled.
bool ListAudioFiles(const std::wstring& dir, std::vector<ScanFile>& out, const std::atomic<bool>* cancel = nullptr);

// The player's default debounce for watched changes (see FolderScanner::Start's `quietMs`).
constexpr DWORD kScanQuietMs = 1500;

// Scans every root once (ScanResult.initial), then watches them. Posts `msg` with
// lParam = ScanResult* (the receiver deletes it); wParam = 1 once the first scan is done.
// A scope's listing arrives as one or more chunks (ScanResult.first/complete); the last
// chunk carries the files left after the full batches.
class FolderScanner {
public:
    ~FolderScanner() { Stop(); }
    // `quietMs` debounces watched changes: a folder is re-listed once it has been quiet this long.
    // It defaults to the player's interval; tests pass a shorter one.
    void Start(HWND notify, UINT msg, std::vector<std::wstring> roots, DWORD quietMs = kScanQuietMs);
    void Stop();
    void RescanAll();  // a full, non-initial pass (e.g. after a manual "Rescan")
    bool Busy() const { return busy_; }

private:
    void Run();
    void Post(ScanResult* r, WPARAM wp = 0);

    HWND notify_ = nullptr;
    UINT msg_ = 0;
    DWORD quietMs_ = kScanQuietMs;  // debounce after the last watched change (see Start)
    std::vector<std::wstring> roots_;
    std::thread thread_;
    HANDLE wake_ = nullptr;
    std::atomic<bool> quit_{false}, rescan_{false}, busy_{false};
};
