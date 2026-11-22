// Album art for the now-playing pane: a single replaceable request handed to one worker thread.
// Each request carries a generation, so a decode that finishes after the track or DPI changed is
// thrown away rather than painting the wrong cover.
#include "app.h"
#include "cover_fade.h"
#include "thumbs.h"

namespace app {

namespace {
// The change from one cover to the next (cover_fade.h). It begins when the next track's art arrives, not
// when it is asked for, so a track on the same album (the same picture) changes nothing; until then the
// outgoing cover stays up instead of flashing the placeholder.
constexpr UINT_PTR kArtFadeTimer = 149;  // on g.hwnd, beside the web (147) and helper (148) timers
struct ArtFade {
    ArtImage from;                // the outgoing cover, as decoded; empty when no change is wanted
    bool waiting = false;         // the next cover has not arrived yet
    ULONGLONG start = 0;          // when the frames began
    std::vector<uint32_t> square;  // `from` at the side last painted
    int side = 0;
} s_fade;

bool AnimationsOn() {
    BOOL on = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0);
    return on != FALSE;
}

void EndFade() {
    if (s_fade.from.Empty() && !s_fade.waiting) return;
    s_fade = ArtFade{};
    if (g.hwnd) KillTimer(g.hwnd, kArtFadeTimer);
}

void BeginFrames() {
    s_fade.waiting = false;
    s_fade.start = GetTickCount64();
    if (!SetTimer(g.hwnd, kArtFadeTimer, 15, nullptr)) EndFade();
}
}  // namespace

bool ArtFadeActive() { return !s_fade.from.Empty(); }

bool ArtFadeTimer(WPARAM id) {
    if (id != kArtFadeTimer) return false;
    if (s_fade.from.Empty() || s_fade.waiting || GetTickCount64() - s_fade.start >= (ULONGLONG)coverfade::kTotalMs)
        EndFade();
    InvalidateRect(g.hwnd, &g.artRect, FALSE);
    return true;
}

bool PaintArtFade(HDC dc, const RECT& r, const std::wstring& tile) {
    if (s_fade.from.Empty()) return false;
    if (s_fade.waiting) {
        DrawCoverArt(dc, r, s_fade.from);
        return true;
    }
    const int ms = (int)std::min<ULONGLONG>(GetTickCount64() - s_fade.start, coverfade::kTotalMs);
    const int w = r.right - r.left, h = r.bottom - r.top, side = std::min(w, h);
    if (side <= 0) return false;
    if (s_fade.side != side) {
        s_fade.square = coverfade::Square(s_fade.from.pixels.data(), s_fade.from.width, s_fade.from.height, side);
        s_fade.side = side;
    }
    // The incoming cover (or its placeholder) is drawn as it will rest, then the frame is composed over it
    // in the same pixels and put up in one blit, so nothing flickers between the two.
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(bi.bmiHeader), side, -side, 1, 32};
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) return false;
    HDC mem = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(mem, bmp);
    const RECT square{0, 0, side, side};
    if (!g.art.Empty()) DrawCoverArt(mem, square, g.art);
    else DrawCoverPlaceholder(mem, square, tile);
    GdiFlush();
    auto* px = static_cast<uint32_t*>(bits);
    coverfade::Compose(s_fade.square.data(), px, 0, side, ms, px, g.coverFade == coverfade::kColour);
    BitBlt(dc, r.left + (w - side) / 2, r.top + (h - side) / 2, side, side, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteDC(mem);
    DeleteObject(bmp);
    return true;
}

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
        result->jpeg = JpegBytes(result->image);
        if (!request.path.empty()) LoadDiscArt(request.path, request.disc, request.discTotal, request.size, result->disc);
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
    // With Cover change chosen in Preferences, another track's cover replacing a real one changes over
    // (cover_fade.h); a refresh of the same track's cover (a new one chosen) just appears. Skipping on again
    // before the next arrives keeps the cover that was showing as the one to change from.
    if (g.artFor != id && !g.closing && g.coverFade != coverfade::kOff && !IsRectEmpty(&g.artRect) &&
        AnimationsOn() && (!g.art.Empty() || !s_fade.from.Empty())) {
        if (!g.art.Empty()) {
            KillTimer(g.hwnd, kArtFadeTimer);
            s_fade = ArtFade{};
            // g.art is cleared below, so the fade's copy can take the pixels instead of duplicating them.
            s_fade.from = std::move(g.art);
        }
        s_fade.waiting = true;
        if (id == kNone) BeginFrames();  // nothing to wait for: straight to the placeholder
    } else if (g.artFor != id) {
        EndFade();
    }
    if (id != kNone && !g.closing && !g.artWorker.joinable()) g.artWorker = std::thread(ArtWorker, g.hwnd);
    g.artFor = id;
    g.art = g.disc = ArtImage{};
    g.artJpeg.clear();
    ++g.artGeneration;
    {
        std::lock_guard<std::mutex> lock(g.artMx);
        g.artRequest = {g.artGeneration, id == kNone ? L"" : g_lib.Path(id), S(176)};
        if (id != kNone) {
            g.artRequest.disc = g_lib[id].disc;
            g.artRequest.discTotal = g_lib[id].discTotal;
        }
        g.artReady = id != kNone && !g.closing;
    }
    g.artCv.notify_one();
    InvalidateRect(g.hwnd, &g.artRect, FALSE);
    if (id == kNone) RefreshPlayerTint();  // otherwise once the art arrives, to change only once
}

// UI thread, from WM_APP_ART. Frees the slot, then adopts the bitmap only if it belongs to the
// still-current generation; either way, repaints everything that shows art.
void OnArtResult(LPARAM lp) {
    std::unique_ptr<ArtResult> result(reinterpret_cast<ArtResult*>(lp));
    { std::lock_guard<std::mutex> lock(g.artMx); g.artOutstanding = false; }
    g.artCv.notify_one();
    if (!result || result->generation != g.artGeneration || g.closing) return;
    g.art = std::move(result->image);
    g.disc = std::move(result->disc);
    g.artJpeg = std::move(result->jpeg);
    if (s_fade.waiting) {
        const bool same = g.art.width == s_fade.from.width && g.art.height == s_fade.from.height &&
                          g.art.pixels == s_fade.from.pixels;
        if (same) EndFade();
        else BeginFrames();
    }
    InvalidateRect(g.hwnd, &g.artRect, FALSE);
    RefreshPlayerTint();
    PlatformTrackChanged();
    PlatformArtChanged();
    MiniPlayerTick();
    PanelsTick();
}

}  // namespace app
