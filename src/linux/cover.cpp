// Cover art on Linux: the embedded picture, else cover/folder/front.jpg|png beside the file,
// decoded with stb_image on a worker thread and scaled down to at most 512 pixels.
#include "cover.h"

#include "app.h"

#include <sys/stat.h>
#include <unistd.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#include "third_party/stb_image.h"

namespace lx {

namespace {

std::mutex g_mx;
std::condition_variable g_cv;
TrackId g_wanted = kNone;
std::wstring g_wantedPath;
struct Thumb {
    TrackId id;
    std::wstring path;
    int size;
};
std::deque<Thumb> g_thumbs;
bool g_quit = false;
std::thread g_thread;

std::string ReadWhole(const std::wstring& path) {
    std::string data;
    FILE* f = fopen(compat::Narrow(path.c_str()).c_str(), "rb");
    if (!f) return data;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size > 0 && size < 32 * 1024 * 1024) {
        data.resize((size_t)size);
        fseek(f, 0, SEEK_SET);
        if (fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
    }
    fclose(f);
    return data;
}

std::string CoverBytes(const std::wstring& trackPath) {
    std::wstring path = trackPath, file;
    int index = 0;
    if (SplitSubtrack(path, file, index)) path = file;
    std::string image;
    if (ReadEmbeddedArt(path, image) && !image.empty()) return image;
    std::wstring dir = DirectoryOf(path);
    for (const wchar_t* name : {L"cover.jpg", L"folder.jpg", L"front.jpg", L"cover.png", L"folder.png", L"front.png",
                                L"Cover.jpg", L"Folder.jpg", L"Front.jpg", L"Cover.png", L"Folder.png", L"Front.png"}) {
        image = ReadWhole(dir + L"/" + name);
        if (!image.empty()) return image;
    }
    return {};
}

// Bar widgets and notifications take the cover as a file: one at a time, in the runtime directory.
std::string SaveForWidgets(const std::string& bytes) {
    const char* runtime = getenv("XDG_RUNTIME_DIR");
    std::string dir = std::string(runtime && *runtime ? runtime : "/tmp") + "/cordinal";
    mkdir(dir.c_str(), 0700);
    static int serial = 0;
    static std::string last;
    // A new name for each cover, so widgets that cache by URL show the new one.
    std::string path = dir + "/cover-" + std::to_string(getpid()) + "-" + std::to_string(++serial);
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return {};
    bool ok = fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    ok = fclose(f) == 0 && ok;
    if (!last.empty()) unlink(last.c_str());
    last = ok ? path : std::string();
    return last;
}

// Box filter to fit within `max` pixels: covers are shown small, and big ones would waste memory.
void Shrink(CoverImage& img, int max) {
    int scale = 1;
    while (img.width / (scale * 2) >= max || img.height / (scale * 2) >= max) scale *= 2;
    if (scale == 1) return;
    int w = img.width / scale, h = img.height / scale;
    std::vector<uint8_t> out((size_t)w * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            unsigned sum[4] = {};
            for (int dy = 0; dy < scale; ++dy) {
                const uint8_t* p = &img.rgba[(((size_t)(y * scale + dy) * img.width) + (size_t)x * scale) * 4];
                for (int dx = 0; dx < scale; ++dx, p += 4)
                    for (int c = 0; c < 4; ++c) sum[c] += p[c];
            }
            for (int c = 0; c < 4; ++c) out[((size_t)y * w + x) * 4 + c] = (uint8_t)(sum[c] / (scale * scale));
        }
    img.rgba.swap(out);
    img.width = w;
    img.height = h;
}

// Decodes `bytes` into `img`, shrunk to at most `max` pixels.
void Decode(const std::string& bytes, CoverImage& img, int max) {
    int w = 0, h = 0, n = 0;
    if (bytes.empty()) return;
    if (stbi_uc* px = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), (int)bytes.size(), &w, &h, &n, 4)) {
        img.width = w;
        img.height = h;
        img.rgba.assign(px, px + (size_t)w * h * 4);
        stbi_image_free(px);
        Shrink(img, max);
    }
}

// The playing track's cover first, then album headers' in the order asked.
void Worker() {
    for (;;) {
        TrackId id;
        std::wstring path;
        int thumb = 0;
        {
            std::unique_lock lock(g_mx);
            g_cv.wait(lock, [] { return g_quit || g_wanted != kNone || !g_thumbs.empty(); });
            if (g_quit) return;
            if (g_wanted != kNone) {
                id = std::exchange(g_wanted, kNone);
                path = g_wantedPath;
            } else {
                id = g_thumbs.front().id;
                path = std::move(g_thumbs.front().path);
                thumb = g_thumbs.front().size;
                g_thumbs.pop_front();
            }
        }
        auto* img = new CoverImage{id, 0, 0, {}, {}};
        std::string bytes = CoverBytes(path);
        if (!thumb && !bytes.empty()) img->file = SaveForWidgets(bytes);
        Decode(bytes, *img, thumb ? thumb : 512);
        if (!PostMessageW(nullptr, thumb ? WM_APP_THUMB : WM_APP_COVER, 0, (LPARAM)img)) delete img;
    }
}

}  // namespace

void StartCovers() { g_thread = std::thread(Worker); }

std::string TrackCoverBytes(const std::wstring& path) { return CoverBytes(path); }

bool DecodeCover(const std::string& bytes, int size, CoverImage& out) {
    out.width = out.height = 0;
    out.rgba.clear();
    Decode(bytes, out, size);
    return out.width > 0;
}

bool DecodeRadioIcon(const std::string& bytes, int size, CoverImage& out) {
    int w = 0, h = 0, channels = 0;
    if (bytes.empty() || bytes.size() > 256 * 1024 ||
        !stbi_info_from_memory((const stbi_uc*)bytes.data(), (int)bytes.size(), &w, &h, &channels) ||
        w <= 0 || h <= 0 || (uint64_t)w * h > 4000000) return false;
    return DecodeCover(bytes, size, out);
}

void StopCovers() {
    {
        std::lock_guard lock(g_mx);
        g_quit = true;
    }
    g_cv.notify_all();
    if (g_thread.joinable()) g_thread.join();
}

void RequestThumb(TrackId id, int size) {
    {
        std::lock_guard lock(g_mx);
        g_thumbs.push_back({id, g_lib.Path(id), size});
    }
    g_cv.notify_one();
}

void ForgetThumbs() {
    std::lock_guard lock(g_mx);
    g_thumbs.clear();
}

void RequestCover(TrackId id) {
    {
        std::lock_guard lock(g_mx);
        g_wanted = id;
        g_wantedPath = g_lib.Path(id);
    }
    g_cv.notify_one();
}

}  // namespace lx
