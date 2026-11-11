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

// Writes `img` to `stream` as a JPEG. Quality 0.88 keeps it small; these are only ever display
// thumbnails.
bool WriteJpeg(IWICImagingFactory* wic, IStream* stream, const ArtImage& img) {
    if (img.Empty()) return false;
    ComPtr<IWICBitmap> bitmap;
    if (FAILED(wic->CreateBitmapFromMemory(img.width, img.height, GUID_WICPixelFormat32bppBGR, img.width * 4,
                                           (UINT)(img.pixels.size() * 4),
                                           reinterpret_cast<BYTE*>(const_cast<uint32_t*>(img.pixels.data())), &bitmap)))
        return false;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    if (FAILED(wic->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) || FAILED(encoder->CreateNewFrame(&frame, &props)))
        return false;
    PROPBAG2 option{};
    option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
    VARIANT quality;
    VariantInit(&quality);
    quality.vt = VT_R4;
    quality.fltVal = 0.88f;
    props->Write(1, &option, &quality);
    return SUCCEEDED(frame->Initialize(props.Get())) && SUCCEEDED(frame->SetSize(img.width, img.height)) &&
           SUCCEEDED(frame->SetPixelFormat(&format)) && SUCCEEDED(frame->WriteSource(bitmap.Get(), nullptr)) &&
           SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

// Encodes to a ".tmp" file then replaces the target, so a half-written cache entry is never read
// as a valid thumbnail. Returns the bytes the entry takes on disk, or 0 when it could not be written.
uint64_t SaveJpeg(const std::wstring& file, const ArtImage& img) {
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return 0;
    std::wstring tmp = file + L".tmp";
    ComPtr<IWICStream> stream;
    bool ok = SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(tmp.c_str(), GENERIC_WRITE)) &&
              WriteJpeg(wic.Get(), stream.Get(), img);
    stream.Reset();
    if (!ok || !MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return 0;
    }
    WIN32_FILE_ATTRIBUTE_DATA written{};
    return GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &written)
               ? (uint64_t)written.nFileSizeHigh << 32 | written.nFileSizeLow
               : 0;
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

// Deletes the oldest .jpg files until the folder is under kCacheLimit, and returns what is left.
uint64_t PruneCache(const std::wstring& dir) {
    std::vector<CacheEntry> entries;
    uint64_t total = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.jpg").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        uint64_t size = (uint64_t)fd.nFileSizeHigh << 32 | fd.nFileSizeLow;
        entries.push_back({dir + L"\\" + fd.cFileName, size, fd.ftLastWriteTime});
        total += size;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (total <= kCacheLimit) return total;
    std::sort(entries.begin(), entries.end(), [](const CacheEntry& a, const CacheEntry& b) {
        return CompareFileTime(&a.written, &b.written) < 0;
    });
    for (const CacheEntry& e : entries) {
        if (total <= kCacheLimit) break;
        if (DeleteFileW(e.file.c_str())) total -= e.size;
    }
    return total;
}

}  // namespace

std::string JpegBytes(const ArtImage& img) {
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(nullptr, 0));
    ULARGE_INTEGER size{};
    std::string bytes;
    if (!stream || FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        !WriteJpeg(wic.Get(), stream.Get(), img) || FAILED(IStream_Size(stream.Get(), &size)) || size.QuadPart > (64u << 20))
        return {};
    bytes.resize((size_t)size.QuadPart);
    if (FAILED(IStream_Reset(stream.Get())) || FAILED(IStream_Read(stream.Get(), bytes.data(), (ULONG)bytes.size()))) return {};
    return bytes;
}

// FNV-1a over the lowercased path, mixed with the file time so a changed file gets a new key.
uint64_t ThumbLoader::Key(const std::wstring& path, uint64_t fileTime) {
    uint64_t h = 1469598103934665603ull;  // FNV-1a over the lowercased path, then the time
    for (wchar_t c : path) h = (h ^ (uint64_t)towlower(c)) * 1099511628211ull;
    return (h ^ fileTime) * 1099511628211ull;
}

void ThumbLoader::Forget(uint64_t key) {
    if (dir_.empty()) return;
    wchar_t pattern[64];
    swprintf_s(pattern, L"\\%016llx-*.jpg", (unsigned long long)key);
    WIN32_FIND_DATAW found;
    HANDLE find = FindFirstFileW((dir_ + pattern).c_str(), &found);
    if (find == INVALID_HANDLE_VALUE) return;
    do DeleteFileW((dir_ + L"\\" + found.cFileName).c_str());
    while (FindNextFileW(find, &found));
    FindClose(find);
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
    cacheBytes_ = PruneCache(dir_);
    quit_ = false;
    pendingLimit_ = reducedMemory ? 64 : 128;
    readyLimit_ = reducedMemory ? 1 : 2;
    threadCount_ = threads;  // started by the first Request: an empty library never needs them
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
        cacheBytes_ = 0;
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
        // Each new worker waits for mx_ before it looks for a job.
        while (threads_.size() < (size_t)threadCount_) threads_.emplace_back(&ThumbLoader::Run, this);
    }
    cv_.notify_all();
}

// UI-side drain: returns the oldest ready result and frees its active_ slot so it can be requested
// again after eviction. The cache budget is kept by the workers (WroteCacheFile), so no scroll ever
// waits for a directory listing here.
std::unique_ptr<ThumbResult> ThumbLoader::TakeResult() {
    std::unique_ptr<ThumbResult> result;
    {
        std::lock_guard<std::mutex> lock(mx_);
        if (results_.empty()) return {};
        result = std::move(results_.front());
        results_.pop_front();
        active_.erase({result->key, result->size});
    }
    cv_.notify_all();
    return result;
}

ThumbLoader::MemoryUsage ThumbLoader::Memory() const {
    std::lock_guard<std::mutex> lock(mx_);
    MemoryUsage usage{jobs_.size(), results_.size(), 0, threads_.size()};
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
                if (uint64_t bytes = SaveJpeg(jpg, r->image)) WroteCacheFile(bytes);
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

// Called by a worker after it wrote a cache entry. The folder holds thousands of names, so it is
// listed only once the bytes kept there reach the limit; the old code listed and sorted it on the
// UI thread after every few hundred covers, for a folder that was usually still under budget.
void ThumbLoader::WroteCacheFile(uint64_t bytes) {
    {
        std::lock_guard<std::mutex> lock(mx_);
        cacheBytes_ += bytes;
        if (cacheBytes_ <= kCacheLimit) return;
    }
    const uint64_t left = PruneCache(dir_);
    std::lock_guard<std::mutex> lock(mx_);
    // A folder that would not shrink (a file another program holds open) is retried after another
    // megabyte of covers rather than on every write.
    cacheBytes_ = left > kCacheLimit ? kCacheLimit - (1u << 20) : left;
}
