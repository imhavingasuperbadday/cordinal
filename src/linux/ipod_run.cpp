// Classic iPod sync on Linux, matching Windows' Library > Sync to iPod. Detection and the file
// database work are the portable src/ipod code; this file owns the worker, the device choice and
// the SDL status/error reporting. Mounted iPods are ordinary folders on Linux.
#include "ui_internal.h"
#include "ipod.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <memory>
#include <string_view>

namespace lx {
namespace {

struct Progress {
    size_t done = 0, total = 0;
    std::wstring title;
};

std::thread worker;
std::atomic<bool> cancel{false};
bool syncing = false;
bool stopping = false;
std::vector<ipod::Device> detected;

std::wstring Size(uint64_t bytes) {
    wchar_t text[32];
    if (bytes >= 1000000000ull) swprintf_s(text, L"%.1f GB", bytes / 1e9);
    else if (bytes >= 1000000ull) swprintf_s(text, L"%.0f MB", bytes / 1e6);
    else swprintf_s(text, L"%.0f KB", bytes / 1e3);
    return text;
}

std::filesystem::path NativePath(const std::wstring& path) {
    return std::filesystem::path(ToUtf8(path));
}

std::vector<ipod::SourceTrack> CaptureTracks() {
    std::vector<TrackId> ids = SelectedTracks();
    if (ids.empty()) ids = Active().items;
    std::vector<ipod::SourceTrack> tracks;
    tracks.reserve(ids.size());
    for (TrackId id : ids) {
        if (!g_lib.Contains(id)) continue;
        const Track& track = g_lib[id];
        ipod::SourceTrack item;
        item.path = NativePath(g_lib.Path(id));
        item.title = ToUtf8(track.title ? g_lib.Str(track.title) : StemOf(g_lib.Str(track.file)));
        item.artist = ToUtf8(track.artist ? g_lib.Str(track.artist) : L"");
        item.album = ToUtf8(track.album ? g_lib.Str(track.album) : L"");
        item.albumArtist = ToUtf8(track.albumArtist ? g_lib.Str(track.albumArtist) : L"");
        item.genre = ToUtf8(track.genre ? g_lib.Str(track.genre) : L"");
        item.composer = ToUtf8(track.composer ? g_lib.Str(track.composer) : L"");
        item.size = track.fileSize;
        item.durationMs = track.durationMs;
        const std::string_view date = g_lib.View(track.date);
        if (date.size() >= 4)
            item.year = (uint32_t)std::clamp(std::atoi(std::string(date.substr(0, 4)).c_str()), 0, 9999);
        item.trackNo = track.trackNo;
        item.discNo = track.disc;
        item.bitrate = track.bitrate;
        item.sampleRate = (uint16_t)std::min<uint32_t>(track.sampleRate, 65535);
        item.playCount = track.playCount;
        item.skipCount = track.skipCount;
        item.rating = track.rating;
        item.missing = (track.flags & Track::kMissing) != 0;
        item.protectedFile = ipod::IsProtectedAudio(item.path);
        std::string extension = item.path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        item.audiobook = extension == ".m4b";
        tracks.push_back(std::move(item));
    }
    return tracks;
}

void BeginSync(ipod::Device device, bool initialize) {
    if (syncing) return;
    auto tracks = CaptureTracks();
    if (tracks.empty()) {
        ShowMessage(L"Nothing to sync.");
        return;
    }
    syncing = true;
    cancel = false;
    if (worker.joinable()) worker.join();
    const std::wstring name = device.name.empty() ? FromUtf8(device.mountpoint.string()) : device.name;
    ShowMessage(L"Syncing " + std::to_wstring(tracks.size()) + L" songs to " + name + L"...");
    worker = std::thread([mount = device.mountpoint, options = ipod::SyncOptions{initialize}, tracks = std::move(tracks)]() mutable {
        auto* result = new ipod::SyncResult;
        try {
            *result = ipod::Sync(tracks, mount, options, [](size_t done, size_t total, const std::wstring& title) {
                auto* progress = new Progress{done, total, title};
                if (!PostMessageW(nullptr, WM_APP_IPOD, 0, (LPARAM)progress)) {
                    delete progress;
                    return false;
                }
                return !cancel.load();
            });
        } catch (const std::exception& exception) {
            result->errors.push_back(exception.what());
        }
        PostMessageW(nullptr, WM_APP_IPOD, 1, (LPARAM)result);
    });
}

void ChooseDevice(const ipod::Device& device) {
    if (!device.classic) {
        ShowMessage(L"This is an iOS or SQLite iPod; its music database cannot be written by cordinal.");
        return;
    }
    bool initialize = false;
    if (!device.initialized) {
        if (!Confirm("This iPod has no iTunes database yet. Write a new one?\n\nDo this only for an iPod whose database "
                     "was erased or never created. Existing files are not indexed by it.",
                     "Sync to iPod"))
            return;
        initialize = true;
    }
    BeginSync(device, initialize);
}

}  // namespace

bool IpodBusy() { return syncing; }

void StartIpodSync() {
    if (syncing) return;
    try {
        detected = ipod::Detect();
    } catch (const std::exception& exception) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Sync to iPod", exception.what(), g.window);
        return;
    }
    if (detected.empty()) {
        ShowMessage(L"No classic iPod found. Connect one with its iPod_Control folder mounted.");
        return;
    }
    if (detected.size() == 1) {
        ChooseDevice(detected.front());
        return;
    }
    std::vector<std::string> labels;
    for (const auto& device : detected) {
        std::string label = ToUtf8(device.name) + "  " + device.mountpoint.string();
        labels.push_back(std::move(label));
    }
    OpenChoice(100, 100, labels, 0, [](int index) {
        if (index >= 0 && index < (int)detected.size()) ChooseDevice(detected[(size_t)index]);
    });
}

void CancelIpodSync() {
    if (!syncing) return;
    cancel = true;
    ShowMessage(L"Cancelling the iPod sync after the current file...");
}

void OnIpodMessage(WPARAM wp, LPARAM lp) {
    if (!wp) {
        std::unique_ptr<Progress> progress(reinterpret_cast<Progress*>(lp));
        if (syncing && progress)
            ShowMessage(L"Syncing " + std::to_wstring(progress->done + 1) + L" of " +
                        std::to_wstring(progress->total) + L": " + progress->title);
        return;
    }
    if (worker.joinable()) worker.join();
    std::unique_ptr<ipod::SyncResult> result(reinterpret_cast<ipod::SyncResult*>(lp));
    syncing = false;
    if (!result || stopping) return;
    size_t skipped = result->skippedExisting + result->skippedMissing + result->skippedProtected +
                     result->skippedUnsupported + result->failed;
    std::wstring text = result->databaseWritten
                            ? L"Synced " + std::to_wstring(result->added) + (result->added == 1 ? L" song" : L" songs")
                            : result->added ? L"Copied " + std::to_wstring(result->added) + L" songs, but the database was not written"
                                            : L"Nothing was added";
    if (result->bytesCopied) text += L" (" + Size(result->bytesCopied) + L")";
    if (skipped) text += L"; skipped " + std::to_wstring(skipped);
    if (result->cancelled) text += L"; cancelled";
    text += L".";
    if (!result->errors.empty()) {
        std::string detail = ToUtf8(text) + "\n\n" + std::to_string(result->errors.size()) + " problem(s):\n\n";
        for (size_t i = 0; i < result->errors.size() && i < 8; ++i) detail += result->errors[i] + "\n";
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "Sync to iPod", detail.c_str(), g.window);
    }
    ShowMessage(text);
    PlaySfx(result->errors.empty() ? Sfx::Organized : Sfx::Oops);
}

void StopIpod() {
    stopping = true;
    cancel = true;
    if (worker.joinable()) worker.join();
}

}  // namespace lx
