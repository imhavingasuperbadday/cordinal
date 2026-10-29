// Album art for the now-playing pane: a single replaceable request handed to one worker thread.
// Each request carries a generation, so a decode that finishes after the track or DPI changed is
// thrown away rather than painting the wrong cover.
#include "app.h"

namespace app {

// Worker thread. Sets up its own COM apartment for WIC decoding, idles until a request is ready
// and no earlier result is still pending, then posts an owned ArtResult for the UI to take.
void ArtWorker(HWND hwnd) {
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        ArtRequest request;
        {
            std::unique_lock<std::mutex> lock(g.artMx);
            g.artCv.wait(lock, [] { return g.artQuit || (g.artReady && !g.artOutstanding); });
            if (g.artQuit) break;
            request = std::move(g.artRequest);
            g.artReady = false;
        }
        auto result = std::make_unique<ArtResult>();
        result->generation = request.generation;
        LoadAlbumArt(request.path, request.size, result->image);
        {
            std::lock_guard<std::mutex> lock(g.artMx);
            if (g.artQuit || (g.artReady && g.artRequest.generation > request.generation)) continue;
        }
        {
            std::lock_guard<std::mutex> lock(g.artMx);
            g.artOutstanding = true;
        }
        if (PostMessageW(hwnd, WM_APP_ART, 0, (LPARAM)result.get())) result.release();
        else { std::lock_guard<std::mutex> lock(g.artMx); g.artOutstanding = false; }
    }
    if (SUCCEEDED(com)) CoUninitialize();
}

// UI thread. Retargets the art slot, starting the worker lazily on the first real request. Bumping
// the generation makes any decode already running for the previous track obsolete.
void LoadArtFor(TrackId id, bool refresh) {
    if (g.artFor == id && !refresh) return;
    if (id != kNone && !g.closing && !g.artWorker.joinable()) g.artWorker = std::thread(ArtWorker, g.hwnd);
    g.artFor = id;
    g.art = ArtImage{};
    ++g.artGeneration;
    {
        std::lock_guard<std::mutex> lock(g.artMx);
        g.artRequest = {g.artGeneration, id == kNone ? L"" : g_lib.Path(id), S(176)};
        g.artReady = id != kNone && !g.closing;
    }
    g.artCv.notify_one();
    InvalidateRect(g.hwnd, &g.artRect, FALSE);
}

// UI thread, from WM_APP_ART. Frees the slot, then adopts the bitmap only if it belongs to the
// still-current generation; either way, repaints everything that shows art.
void OnArtResult(LPARAM lp) {
    std::unique_ptr<ArtResult> result(reinterpret_cast<ArtResult*>(lp));
    { std::lock_guard<std::mutex> lock(g.artMx); g.artOutstanding = false; }
    g.artCv.notify_one();
    if (!result || result->generation != g.artGeneration || g.closing) return;
    g.art = std::move(result->image);
    InvalidateRect(g.hwnd, &g.artRect, FALSE);
    PlatformTrackChanged();
    MiniPlayerTick();
    PanelsTick();
}

}  // namespace app
