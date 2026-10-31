// Thumbnail disk cache and decode workers behind ThumbLoader (see thumbs.h). Decoded covers are
// written to the loader's cache dir as small JPEGs and reused on later runs. Missing art is only
// remembered by the view, so a temporary read failure cannot hide covers on later runs.
// Requests are served newest-first with a bounded pending queue, and results are discarded once
// the view has moved on (generation bump).
#include "thumbs.h"

#include <shlobj.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>

using Microsoft::WRL::ComPtr;

namespace {

// Encodes to a ".tmp" file then replaces the target, so a half-written cache entry is never read
// as a valid thumbnail. Quality 0.88 keeps the cache small; these are only ever display thumbnails.
bool SaveJpeg(const std::wstring& file, const ArtImage& img) {
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
    ComPtr<IWICBitmap> bitmap;
    if (FAILED(wic->CreateBitmapFromMemory(img.width, img.height, GUID_WICPixelFormat32bppBGR, img.width * 4,
                                           (UINT)(img.pixels.size() * 4),
                                           reinterpret_cast<BYTE*>(const_cast<uint32_t*>(img.pixels.data())), &bitmap)))
        return false;
    std::wstring tmp = file + L".tmp";
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    bool ok = SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(tmp.c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder)) &&
              SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
              SUCCEEDED(encoder->CreateNewFrame(&frame, &props));
    if (ok) {
        PROPBAG2 option{};
        option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT quality;
        VariantInit(&quality);
        quality.vt = VT_R4;
        quality.fltVal = 0.88f;
        props->Write(1, &option, &quality);
        ok = SUCCEEDED(frame->Initialize(props.Get())) && SUCCEEDED(frame->SetSize(img.width, img.height)) &&
             SUCCEEDED(frame->SetPixelFormat(&format)) && SUCCEEDED(frame->WriteSource(bitmap.Get(), nullptr)) &&
             SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    }
    frame.Reset();
    encoder.Reset();
    stream.Reset();
    if (!ok || !MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

bool LoadJpeg(const std::wstring& file, int size, ArtImage& out) {
    ComPtr<IStream> s;
    return SUCCEEDED(SHCreateStreamOnFileEx(file.c_str(), STGM_READ | STGM_SHARE_DENY_NONE, 0, FALSE, nullptr, &s)) &&
           DecodeImage(s.Get(), size, out);
}

// Every cover is cached at every size it was drawn at, so months of browsing can leave thousands
// of entries for files that moved or were deleted. Keep the newest under a fixed disk budget.
constexpr uint64_t kCacheLimit = 64ull << 20;

struct CacheEntry {
    std::wstring file;
    uint64_t size;
    FILETIME written;
};

void PruneCache(const std::wstring& dir) {
    std::vector<CacheEntry> entries;
    uint64_t total = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.jpg").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        uint64_t size = (uint64_t)fd.nFileSizeHigh << 32 | fd.nFileSizeLow;
        entries.push_back({dir + L"\\" + fd.cFileName, size, fd.ftLastWriteTime});
        total += size;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (total <= kCacheLimit) return;
    std::sort(entries.begin(), entries.end(), [](const CacheEntry& a, const CacheEntry& b) {
        return CompareFileTime(&a.written, &b.written) < 0;
    });
    for (const CacheEntry& e : entries) {
        if (total <= kCacheLimit) break;
        if (DeleteFileW(e.file.c_str())) total -= e.size;
    }
}

}  // namespace

// FNV-1a over the lowercased path, mixed with the file time so a changed file gets a new key.
uint64_t ThumbLoader::Key(const std::wstring& path, uint64_t fileTime) {
    uint64_t h = 1469598103934665603ull;  // FNV-1a over the lowercased path, then the time
    for (wchar_t c : path) h = (h ^ (uint64_t)towlower(c)) * 1099511628211ull;
    return (h ^ fileTime) * 1099511628211ull;
}

// Cache file name encodes key and size, so one track's cover can be cached at several sizes.
std::wstring ThumbLoader::CacheFile(uint64_t key, int size, const wchar_t* ext) const {
    wchar_t name[64];
    swprintf_s(name, L"\\%016llx-%d.%ls", (unsigned long long)key, size, ext);
    return dir_ + name;
}

// Starts `threads` decode workers, clearing any previous run. Each worker sets up its own COM
// apartment, so the caller need not have initialized COM.
void ThumbLoader::Start(HWND notify, UINT msg, std::wstring cacheDir, int threads, bool reducedMemory) {
    Stop();
    notify_ = notify;
    msg_ = msg;
    dir_ = std::move(cacheDir);
    SHCreateDirectoryExW(nullptr, dir_.c_str(), nullptr);
    PruneCache(dir_);
    savedSincePrune_ = 0;
    quit_ = false;
    pendingLimit_ = reducedMemory ? 64 : 128;
    readyLimit_ = reducedMemory ? 1 : 2;
    for (int i = 0; i < threads; ++i) threads_.emplace_back(&ThumbLoader::Run, this);
}

// Bumps the generation so in-flight jobs are dropped, then joins the workers.
void ThumbLoader::Stop() {
    {
        std::lock_guard<std::mutex> lock(mx_);
        quit_ = true;
        ++generation_;
        jobs_.clear();
        results_.clear();
        active_.clear();
    }
    cv_.notify_all();
    for (auto& t : threads_) t.join();
    threads_.clear();
}

// Queues a decode unless the same (key, size) is already pending or done (active_ dedupes). The
// pending queue is bounded: past the limit the oldest request is dropped, since it has scrolled
// away and the newest is what's on screen now.
void ThumbLoader::Request(uint64_t key, const std::wstring& mediaPath, int size) {
    {
        std::lock_guard<std::mutex> lock(mx_);
        if (quit_ || !active_.emplace(key, size).second) return;
        jobs_.push_back({key, mediaPath, size, generation_});
        if (jobs_.size() > pendingLimit_) {
            active_.erase({jobs_.front().key, jobs_.front().size});
            jobs_.pop_front();
        }
    }
    cv_.notify_all();
}

// UI-side drain: returns the oldest ready result and frees its active_ slot so it can be requested
// again after eviction.
std::unique_ptr<ThumbResult> ThumbLoader::TakeResult() {
    std::unique_ptr<ThumbResult> result;
    bool prune = false;
    {
        std::lock_guard<std::mutex> lock(mx_);
        if (results_.empty()) return {};
        result = std::move(results_.front());
        results_.pop_front();
        active_.erase({result->key, result->size});
        prune = ++savedSincePrune_ >= 256;  // a long session can browse past the cache budget
        if (prune) savedSincePrune_ = 0;
    }
    if (prune) PruneCache(dir_);
    cv_.notify_all();
    return result;
}

ThumbLoader::MemoryUsage ThumbLoader::Memory() const {
    std::lock_guard<std::mutex> lock(mx_);
    MemoryUsage usage{jobs_.size(), results_.size(), 0};
    for (const auto& result : results_) usage.readyBytes += result->image.pixels.size() * sizeof(uint32_t);
    return usage;
}

// Invalidates every queued and in-flight request (e.g. the view jumped elsewhere or resized): the
// generation bump makes workers discard their results instead of posting them.
void ThumbLoader::DropPending() {
    {
        std::lock_guard<std::mutex> lock(mx_);
        ++generation_;
        jobs_.clear();
        results_.clear();
        active_.clear();
    }
    cv_.notify_all();
}

// Worker loop. Newest job first (the visible rows). Decode order: disk cache, else embedded/sidecar
// art, then cache it. Legacy ".none" markers are ignored: a miss can mean an unavailable file or
// a cover that has not been added yet. Before queueing, it waits for room and re-checks the
// generation so stale work never reaches the UI.
void ThumbLoader::Run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mx_);
            cv_.wait(lock, [&] { return quit_ || !jobs_.empty(); });
            if (quit_) break;
            job = std::move(jobs_.back());  // newest first
            jobs_.pop_back();
        }
        auto r = std::make_unique<ThumbResult>(ThumbResult{job.key, job.size, {}});
        std::wstring jpg = CacheFile(job.key, job.size, L"jpg");
        if (!LoadJpeg(jpg, job.size, r->image)) {
            if (LoadAlbumArt(job.path, job.size, r->image)) {
                SaveJpeg(jpg, r->image);
            }
        }
        {
            std::unique_lock<std::mutex> lock(mx_);
            cv_.wait(lock, [&] { return quit_ || job.generation != generation_ || results_.size() < readyLimit_; });
            if (quit_) break;
            if (job.generation != generation_) continue;  // resized/hidden/scrolled away
            bool wake = results_.empty();
            results_.push_back(std::move(r));
            if (wake && !PostMessageW(notify_, msg_, 0, 0)) {
                results_.clear();
                active_.erase({job.key, job.size});
            }
        }
    }
    CoUninitialize();
}
