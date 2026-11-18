// Hidden native UI checks of the self test (see selftest.cpp): real Win32 controls under a test
// window, covering the tabs, playback strip and background jobs, and ending with the recovered
// tab/playback behaviour checks in port_regressions.cpp. No player instance or user state is used.
#include "selftest_common.h"
#include "column_layout.h"
#include "cover_fade.h"
#include "metadata_fetch.h"
#include "radio.h"
#include "soft_canvas.h"
#include "tests/keymap_checks.h"

#include <dwrite.h>
#include <tlhelp32.h>

void TestPersonalization(const std::wstring& scratch);
namespace app {
bool LyricsReadyFor(TrackId id, const std::wstring& line);
bool OpensPlayerMenu(HWND h);  // app_window.cpp
bool ColumnOffered(int type);   // app_columns.cpp
}

namespace {
constexpr int kUiWidth = 1163, kUiHeight = 752;

unsigned ProcessThreads() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    unsigned threads = 0;
    THREADENTRY32 entry{sizeof(entry)};
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry))
        threads += entry.th32OwnerProcessID == GetCurrentProcessId();
    CloseHandle(snapshot);
    return threads;
}

// Bytes allocated from the process heap, where WARP and Direct2D keep their working memory.
size_t HeapBusyBytes() {
    HANDLE heap = GetProcessHeap();
    size_t busy = 0;
    if (!HeapLock(heap)) return 0;
    PROCESS_HEAP_ENTRY entry{};
    while (HeapWalk(heap, &entry))
        if (entry.wFlags & PROCESS_HEAP_ENTRY_BUSY) busy += entry.cbData;
    HeapUnlock(heap);
    return busy;
}

// The now playing cover's change to the next one (cover_fade.h): it starts as the outgoing cover exactly,
// is a black-and-white photograph once drained (or keeps its colour in that style), opens the new cover from
// the middle, and ends on it.
void TestCoverFade() {
    testlog::Batch batch(L"UI: the cover's black-and-white change to the next cover");
    constexpr int side = 32;
    std::vector<uint32_t> from(side * side), to(side * side, 0x2050E0), out(side * side);
    for (size_t i = 0; i < from.size(); ++i) from[i] = 0xC03010 + (uint32_t)(i % 40);
    Check(coverfade::Compose(from.data(), to.data(), 0, side, 0, out.data()) && out == from,
              L"the first frame is the outgoing cover unchanged");
    coverfade::Compose(from.data(), to.data(), 0, side, coverfade::kDrainMs + coverfade::kHoldMs / 2, out.data());
    bool grey = true;
    for (uint32_t px : out) grey = grey && (px >> 16 & 255) == (px >> 8 & 255) && (px >> 8 & 255) == (px & 255);
    Check(grey, L"drained and held, the outgoing cover is black and white");
    Check((out[0] & 255) < (out[side / 2 * side + side / 2] & 255), L"the photograph darkens toward its corners");
    std::vector<uint32_t> tinted(side * side);
    coverfade::Compose(from.data(), to.data(), 0, side, coverfade::kDrainMs + coverfade::kHoldMs / 2, tinted.data(), true);
    const uint32_t middle = tinted[side / 2 * side + side / 2];
    Check((middle >> 16 & 255) > (middle & 255), L"the colour style keeps the outgoing cover's colour");
    // The iris opens block by block from the middle with a ragged edge: early on the corners are still the
    // photograph, by halfway the middle is the new cover.
    const int revealAt = coverfade::kDrainMs + coverfade::kHoldMs;
    coverfade::Compose(from.data(), to.data(), 0, side, revealAt + coverfade::kRevealMs * 3 / 10, out.data());
    const uint32_t corner = out[0];
    coverfade::Compose(from.data(), to.data(), 0, side, revealAt + coverfade::kRevealMs / 2, out.data());
    const uint32_t centre = out[side / 2 * side + side / 2];
    Check((centre & 255) > 0xC0 && (corner >> 16 & 255) == (corner & 255),
          L"the new cover opens from the middle while the corners are still the photograph");
    Check(!coverfade::Compose(from.data(), to.data(), 0, side, coverfade::kTotalMs, out.data()) && out == to,
              L"the change ends on the new cover exactly");
    coverfade::Compose(from.data(), nullptr, 0x123456, side, coverfade::kTotalMs, out.data());
    Check(out[5] == 0x123456, L"without a new cover the change ends on the fill");
    std::vector<uint32_t> wide(4 * 2, 0x808080);
    wide[1] = wide[2] = wide[5] = wide[6] = 0xFF0000;  // the middle two columns, which a centred crop keeps
    auto square = coverfade::Square(wide.data(), 4, 2, 2);
    Check(square.size() == 4 && square[0] == 0xFF0000 && square[3] == 0xFF0000, L"a wide cover is cropped to its centre");
}

// The library view's software canvas: frames reach the DC it is given, a resize keeps the bitmaps
// made on it, and WARP draws on this thread. Direct2D's own software target started a pool of nine
// threads here for a cover grid, each holding 0.9 MB (docs/memory.md).
void TestSoftCanvas() {
    using Microsoft::WRL::ComPtr;
    testlog::Batch batch(L"UI: the library view's software canvas");
    ComPtr<ID2D1Factory> factory;
    ComPtr<IDWriteFactory> dw;
    ComPtr<IDWriteTextFormat> format;
    SoftCanvas canvas;
    ID2D1DeviceContext* context = nullptr;
    if (!Ready(SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf())) &&
                   SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                                 reinterpret_cast<IUnknown**>(dw.GetAddressOf()))) &&
                   SUCCEEDED(dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                                  DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"", &format)) &&
                   (context = canvas.Context(factory.Get())) != nullptr,
               L"a software Direct2D canvas"))
        return;
    std::vector<uint32_t> pixels(256 * 256);
    for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = 0xFF000000u | (uint32_t)(i * 2654435761u >> 8);
    ComPtr<ID2D1Bitmap> cover;
    ComPtr<ID2D1SolidColorBrush> brush;
    context->CreateBitmap(D2D1::SizeU(256, 256), pixels.data(), 256 * 4,
                          D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE)),
                          &cover);
    context->CreateSolidColorBrush(D2D1::ColorF(0x2040C0), &brush);
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(info.bmiHeader), 1400, -900, 1, 32, BI_RGB};
    void* bits = nullptr;
    HDC memory = CreateCompatibleDC(nullptr);
    HBITMAP dib = CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(memory, dib);
    const unsigned before = ProcessThreads();
    // A cover grid at two sizes, as the album view draws it: the second frame reallocates the surface.
    bool drawn = cover && brush && bits;
    for (int frame = 0; drawn && frame < 6; ++frame) {
        const int w = frame < 3 ? 1400 : 1000, h = frame < 3 ? 900 : 700;
        drawn = canvas.Begin(w, h);
        if (!drawn) break;
        context->Clear(D2D1::ColorF(0x102030));
        for (int y = 40; y + 200 < h; y += 220)
            for (int x = 20; x + 180 < w; x += 190) {
                context->DrawBitmap(cover.Get(), D2D1::RectF((float)x, (float)y, x + 170.0f, y + 170.0f));
                context->DrawTextW(L"Album title", 11, format.Get(), D2D1::RectF((float)x, y + 172.0f, x + 170.0f, y + 192.0f),
                                   brush.Get());
            }
        context->FillRectangle(D2D1::RectF(0, 0, 10, 10), brush.Get());
        drawn = SUCCEEDED(canvas.End(memory, 0, 0));
    }
    const unsigned after = ProcessThreads();
    const auto* rows = static_cast<const uint32_t*>(bits);
    const bool painted = drawn && (rows[5 * 1400 + 5] & 0xFFFFFF) == 0x2040C0 && (rows[20 * 1400 + 15] & 0xFFFFFF) == 0x102030;
    const uint32_t coverPixel = rows[125 * 1400 + 105];
    // An idle view gives its surface back and trims what WARP keeps for drawing (about 3 MB of heap in
    // the playing scenario); the next frame builds them again and draws the same picture.
    const size_t busy = HeapBusyBytes();
    canvas.ReleaseSurface();
    const size_t released = HeapBusyBytes();
    bool redrawn = drawn && canvas.Begin(1000, 700);
    if (redrawn) {
        context->Clear(D2D1::ColorF(0x102030));
        context->DrawBitmap(cover.Get(), D2D1::RectF(20.0f, 40.0f, 190.0f, 210.0f));
        context->FillRectangle(D2D1::RectF(0, 0, 10, 10), brush.Get());
        redrawn = SUCCEEDED(canvas.End(memory, 0, 0)) && rows[125 * 1400 + 105] == coverPixel &&
                  (rows[5 * 1400 + 5] & 0xFFFFFF) == 0x2040C0;
    }
    Check(redrawn, L"UI: the software canvas draws the same frame after giving its surface back");
    Check(released + 512 * 1024 < busy, L"UI: giving the surface back trims WARP's heap (%zu KB before, %zu KB after)",
          busy / 1024, released / 1024);
    SelectObject(memory, old);
    DeleteObject(dib);
    DeleteDC(memory);
    canvas.Reset();
    Check(painted, L"UI: the software canvas copies each frame to the DC it is given, across a resize");
    Check(after <= before + 1, L"UI: the software canvas draws on its own thread (threads %u before, %u after)", before, after);
}

// The track list keeps formatted cells in one table and one character buffer. A hit is the first
// string stored for that key, including an empty one, and clearing drops the lookup.
void TestCellCache() {
    using namespace app;
    testlog::Batch batch(L"UI: the track list's cell cache");
    ClearCellCache();
    ShrinkCellCache();
    std::wstring got = L"untouched";
    Check(!LookupCachedCell(1, got) && got == L"untouched", L"UI: a missing cell is a cache miss");
    size_t bytes = 0;
    for (int i = 0; i < 3000; ++i) {
        std::wstring text = L"Track " + std::to_wstring(i);
        bytes += text.size() * sizeof(wchar_t);
        RememberCachedCell((uint64_t)i * 100 + 3, text);
    }
    const uint64_t firstKey = 3, lastKey = 2999ull * 100 + 3, duplicate = 5ull * 100 + 3;
    Check(LookupCachedCell(firstKey, got) && got == L"Track 0" && LookupCachedCell(lastKey, got) && got == L"Track 2999" &&
              CellCacheCount() == 3000 && CellCacheBytes() == bytes,
          L"UI: the cell cache keeps every stored string (count %zu, bytes %zu)", CellCacheCount(), CellCacheBytes());
    const size_t before = CellCacheBytes();
    RememberCachedCell(duplicate, L"replaced");
    Check(LookupCachedCell(duplicate, got) && got == L"Track 5" && CellCacheBytes() == before && CellCacheCount() == 3000,
          L"UI: storing a cell again keeps the first string");
    RememberCachedCell(42, L"");
    Check(LookupCachedCell(42, got) && got.empty() && CellCacheCount() == 3001, L"UI: an empty cell is a cache hit");
    ClearCellCache();
    Check(!LookupCachedCell(firstKey, got) && CellCacheCount() == 0, L"UI: clearing the cell cache drops its lookups");
    ShrinkCellCache();
}

// Themed checkboxes and radios draw their labels in the theme's own color, which ignores WM_CTLCOLORBTN
// and was black on dark surfaces (the Convert window's choices). ApplyControlTheme now routes every such
// button's custom draw, through its parent, to a painter that redraws the label in the palette. The
// parent here is a bare static with no handler of its own, as a new dialog that forgot one would be.
void TestChoiceLabels() {
    using namespace app;
    testlog::Batch batch;
    const auto savedTheme = g.themeMode;
    const auto savedPalette = g.pal;
    const bool savedDark = g.dark;
    const HBRUSH savedSurface = g.surfaceBrush;
    testlog::Cases cases;
    auto distance = [](COLORREF a, COLORREF b) {
        return std::abs(GetRValue(a) - GetRValue(b)) + std::abs(GetGValue(a) - GetGValue(b)) + std::abs(GetBValue(a) - GetBValue(b));
    };
    for (ThemeMode theme : {ThemeMode::Light, ThemeMode::Dark}) {
        // The palette alone, as ApplyTheme would set it, without repainting or laying out the main window.
        g.themeMode = theme; g.pal = &ThemePalette(theme); g.dark = g.pal->dark;
        g.surfaceBrush = CreateSolidBrush(SurfaceColor());
        const int width = S(320), height = S(24);
        HWND parent = CreateWindowExW(0, WC_STATICW, L"", WS_POPUP, 0, 0, width, height * 2, nullptr, nullptr, g.inst, nullptr);
        for (DWORD type : {(DWORD)BS_AUTOCHECKBOX, (DWORD)BS_AUTORADIOBUTTON, (DWORD)(BS_AUTORADIOBUTTON | BS_PUSHLIKE)}) {
            const bool pushLike = type & BS_PUSHLIKE;
            const std::wstring name = std::wstring(theme == ThemeMode::Dark ? L"dark " : L"light ") +
                                      (pushLike ? L"push-like radio" : type == BS_AUTOCHECKBOX ? L"checkbox" : L"radio");
            HWND c = CreateWindowExW(0, WC_BUTTONW, L"Copies, and keep the originals", WS_CHILD | WS_VISIBLE | type, 0, 0,
                                     width, height, parent, (HMENU)(INT_PTR)1, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.font, FALSE);
            ApplyControlTheme(c, g.dark);
            NMCUSTOMDRAW draw{};
            draw.hdr = {c, 1, (UINT)NM_CUSTOMDRAW};
            draw.rc = {0, 0, width, height};
            draw.dwDrawStage = CDDS_PREPAINT;
            const LRESULT pre = SendMessageW(parent, WM_NOTIFY, 1, (LPARAM)&draw);
            if (pushLike) {  // a push-like radio is a button with no separate label: left alone
                cases.Add(pre != CDRF_NOTIFYPOSTPAINT, name);
                DestroyWindow(c);
                continue;
            }
            HDC dc = CreateCompatibleDC(nullptr);
            BITMAPINFO info{};
            info.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB};
            void* pixels = nullptr;
            HBITMAP bitmap = dc ? CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0) : nullptr;
            bool ink = false;
            if (bitmap) {
                HGDIOBJ old = SelectObject(dc, bitmap);
                RECT all{0, 0, width, height};
                FillRect(dc, &all, (HBRUSH)GetStockObject(theme == ThemeMode::Dark ? BLACK_BRUSH : WHITE_BRUSH));
                draw.hdc = dc;
                draw.dwDrawStage = CDDS_POSTPAINT;
                SendMessageW(parent, WM_NOTIFY, 1, (LPARAM)&draw);
                // The label's pixel furthest from the surface is its ink; it must be the palette's text.
                const COLORREF surface = SurfaceColor(), text = TextColor();
                COLORREF inkPx = surface;
                for (int y = 0; y < height; ++y)
                    for (int x = S(20); x < width; ++x)
                        if (COLORREF px = GetPixel(dc, x, y); distance(px, surface) > distance(inkPx, surface)) inkPx = px;
                ink = distance(inkPx, text) <= 96 && distance(inkPx, surface) * 2 > distance(text, surface) &&
                      GetPixel(dc, width - 2, height / 2) == surface;
                SelectObject(dc, old);
                DeleteObject(bitmap);
            }
            if (dc) DeleteDC(dc);
            cases.Add(pre == CDRF_NOTIFYPOSTPAINT && ink, name);
            DestroyWindow(c);
        }
        if (parent) DestroyWindow(parent);
        DeleteObject(g.surfaceBrush);
    }
    g.themeMode = savedTheme; g.pal = savedPalette; g.dark = savedDark; g.surfaceBrush = savedSurface;
    batch.Next(L"UI: every themed checkbox and radio label is painted in the palette's text color, in light and dark");
    Check(cases.Passed(), L"UI: every themed checkbox and radio label is painted in the palette's text color%ls", cases.failed.c_str());
}

void TestPlayerSliders() {
    using namespace app;
    testlog::Batch batch;
    const UINT savedDpi = g.dpi;
    const int savedBandSize = g.bandWaveSize;
    const auto savedTheme = g.themeMode;
    const auto savedPalette = g.pal;
    const auto savedLayout = g.layout;
    const auto savedSize = g.size;
    const int savedSeek = (int)SendMessageW(g.seek, TBM_GETPOS, 0, 0);
    const int savedVolume = (int)SendMessageW(g.volume, TBM_GETPOS, 0, 0);
    const bool enabled = IsWindowEnabled(g.seek) != 0;
    EnableWindow(g.seek, TRUE);
    g.layout = MainLayout{};
    g.size = ViewSize::Normal;
    testlog::Cases aligned, positions, input;
    for (UINT dpi : {96u, 144u, 192u, 288u}) {
        g.dpi = dpi;
        SetWindowPos(g.hwnd, nullptr, 0, 0, S(kUiWidth), S(kUiHeight), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        for (ThemeMode theme : {ThemeMode::Light, ThemeMode::Dark}) {
            g.themeMode = theme; g.pal = &ThemePalette(theme);
            for (int band : {1, 2, 3, 5}) {
                g.bandWaveSize = band; Layout();
                RECT play{}; GetWindowRect(g.buttons[1], &play);
                for (HWND bar : {g.seek, g.volume}) {
                    RECT client{}, window{};
                    GetClientRect(bar, &client); GetWindowRect(bar, &window);
                    const std::wstring name = (bar == g.seek ? L"seek " : L"volume ") + std::to_wstring(dpi) +
                        L"/" + std::to_wstring((int)theme) + L"/" + std::to_wstring(band);
                    aligned.Add(std::abs((window.top + window.bottom) / 2 - (play.top + play.bottom) / 2) <= 1, name);
                    HDC dc = CreateCompatibleDC(nullptr);
                    BITMAPINFO info{};
                    info.bmiHeader = {sizeof(BITMAPINFOHEADER), client.right, -client.bottom, 1, 32, BI_RGB};
                    void* pixels = nullptr;
                    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
                    HGDIOBJ old = bitmap ? SelectObject(dc, bitmap) : nullptr;
                    bool drawn = dc && bitmap;
                    const int hi = (int)SendMessageW(bar, TBM_GETRANGEMAX, 0, 0), cy = client.bottom / 2;
                    const int left = S(ui::kSliderInset), right = client.right - left;
                    for (int percent : {0, 25, 50, 75, 100}) {
                        // FALSE used to leave the native thumb rectangle at an earlier position.
                        SendMessageW(bar, TBM_SETPOS, FALSE, hi * percent / 100);
                        if (!drawn) continue;
                        SendMessageW(bar, WM_PRINTCLIENT, (WPARAM)dc, PRF_CLIENT);
                        const int cx = left + (int)std::lround((right - left) * percent / 100.0);
                        const int y = cy - S(ui::kSliderThumbHeight) / 2, w = std::max(2, S(ui::kSliderThumbWidth));
                        int first = client.right, last = -1;
                        for (int x = 0; x < client.right; ++x)
                            if (GetPixel(dc, x, y) != ControlBg(bar)) { first = std::min(first, x); last = x; }
                        bool fill = percent == 0 || GetPixel(dc, cx - w, cy) == Ref(P().accent);
                        bool rest = percent == 100 || GetPixel(dc, cx + w, cy) == Ref(P().border);
                        positions.Add(first == cx - w / 2 && last == first + w - 1 && fill && rest &&
                                          std::fabs(BarFracAt(bar, {cx, cy}) - percent / 100.0) <= 1.0 / (right - left),
                                      name + L" at " + std::to_wstring(percent));
                    }
                    positions.Add(drawn, name + L" drawing setup");
                    if (old) SelectObject(dc, old);
                    if (bitmap) DeleteObject(bitmap);
                    if (dc) DeleteDC(dc);
                    SendMessageW(bar, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(left, cy));
                    SendMessageW(bar, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(right, cy));
                    SendMessageW(bar, WM_LBUTTONUP, 0, MAKELPARAM(right, cy));
                    bool dragged = SendMessageW(bar, TBM_GETPOS, 0, 0) == hi && GetCapture() != bar;
                    SendMessageW(bar, WM_KEYDOWN, VK_HOME, 0);
                    input.Add(dragged && SendMessageW(bar, TBM_GETPOS, 0, 0) == 0, name);
                }
            }
        }
    }
    batch.Next(L"UI: seek and volume tracks and handles stay aligned at every band height and DPI");
    Check(aligned.Passed() && positions.Passed(), L"UI: slider drawing stays centred and follows progress%ls%ls",
          aligned.failed.c_str(), positions.failed.c_str());
    Check(input.Passed(), L"UI: centred slider handles drag to the end and retain keyboard control%ls", input.failed.c_str());
    g.dpi = savedDpi; g.bandWaveSize = savedBandSize; g.layout = savedLayout; g.size = savedSize;
    g.themeMode = savedTheme; g.pal = savedPalette;
    SetWindowPos(g.hwnd, nullptr, 0, 0, S(kUiWidth), S(kUiHeight), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    EnableWindow(g.seek, enabled);
    SendMessageW(g.seek, TBM_SETPOS, TRUE, savedSeek);
    SendMessageW(g.volume, TBM_SETPOS, TRUE, savedVolume);
    Layout();
}

// Real scanner results survive saving and reopening, including silence (no usable LUFS),
// older measurements that predate rgHigh, and a measured album's gain. No user library or
// settings are opened.
void TestReplayGainRestart(const std::wstring& scratch) {
    using namespace app;
    testlog::Group group(L"ReplayGain across restarts");
    testlog::Batch batch;
    const std::wstring sine = scratch + L"\\rg-restart-sine.wav", silent = scratch + L"\\rg-restart-silent.wav";
    const std::wstring albumA = scratch + L"\\rg-restart-album-a.wav", albumB = scratch + L"\\rg-restart-album-b.wav";
    const std::wstring albumC = scratch + L"\\rg-restart-album-c.wav";
    const std::wstring file = scratch + L"\\rg-restart.cdb";
    WriteTestBytes(sine, MakeSineWav(2, 2));
    WriteTestBytes(silent, MakeSineWav(2, 0));
    WriteTestBytes(albumA, MakeSineWav(2, 2));
    WriteTestBytes(albumB, MakeSineWav(2, 2));
    g_lib = Library();
    ForgetAlbumDynamics();
    g.playlists = {{L"Library", {}, PlaylistKind::Library}};
    g.active = 0;
    g.compactPending = false;
    g.rgOn = g.rgAutoScan = true;
    g_lib.Add(scratch + L"\\rg-unsaved.wav");  // saving renumbers the retained tracks
    auto add = [](const std::wstring& path) {
        TrackId id = g_lib.Add(path);
        TagData tags;
        ReadTags(path, tags);
        g_lib.ApplyTags(id, tags);
        g_lib[id].flags |= Track::kInLibrary;
        return id;
    };
    auto album = [](TrackId id) {
        g_lib[id].album = g_lib.Intern("Restart album");
        g_lib[id].artist = g_lib.Intern("Restart artist");
        g_lib[id].date = g_lib.Intern("2020");
    };
    TrackId measured = add(sine), failed = add(silent), a = add(albumA), b = add(albumB);
    album(a);
    album(b);
    const std::wstring oldPath = scratch + L"\\rg-older-scan.wav";
    TrackId older = add(oldPath);
    g_lib[older].rgTrackGain = -650;
    g_lib[older].flags |= Track::kRgScanned;
    auto finish = [] {
        const ULONGLONG start = GetTickCount64();
        do {
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (!g.rgRunning) return true;
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        } while (GetTickCount64() - start < 10000);
        return false;
    };
    ScanReplayGain({measured, failed, a, b});
    const bool scanned = finish();
    auto stored = g_albumMeasurements.find(AlbumKeyOf(g_lib[a]));
    batch.Next(L"RG measurements save, including unusable files, and survive reopening with renumbered IDs");
    Check(scanned && g_lib[measured].rgHigh && (g_lib[measured].flags & Track::kRgScanned) &&
              (g_lib[failed].flags & Track::kRgScanFailed) && stored != g_albumMeasurements.end() &&
              stored->second.members.size() == 2 && g_lib[a].rgAlbumGain == g_lib[b].rgAlbumGain &&
              g_lib[a].rgAlbumGain != kNoGain && (g_lib[a].flags & Track::kRgAlbumMeasured) &&
              (g_lib[b].flags & Track::kRgAlbumMeasured),
          L"RG saves completed, unusable and album measurements");
    const auto gain = g_lib[measured].rgTrackGain;
    const auto peak = g_lib[measured].rgTrackPeak;
    const auto high = g_lib[measured].rgHigh;
    const auto albumGain = g_lib[a].rgAlbumGain;
    const auto albumPeak = g_lib[a].rgAlbumPeak;
    bool saved = SaveLibrary(file, g.playlists);
    g_lib = Library();
    bool loaded = LoadLibrary(file, g.playlists);
    measured = g_lib.Find(sine);
    failed = g_lib.Find(silent);
    older = g_lib.Find(oldPath);
    a = g_lib.Find(albumA);
    b = g_lib.Find(albumB);
    bool found = g_lib.Contains(measured) && g_lib.Contains(failed) && g_lib.Contains(older) &&
                 g_lib.Contains(a) && g_lib.Contains(b);
    Check(saved && loaded && found && measured == 0 && g_lib[measured].rgTrackGain == gain &&
              g_lib[measured].rgTrackPeak == peak && g_lib[measured].rgHigh == high &&
              (g_lib[failed].flags & Track::kRgScanFailed) && !g_lib[older].rgHigh,
          L"RG results and failed attempts survive reopening with renumbered track IDs");
    auto reloaded = found ? g_albumMeasurements.find(AlbumKeyOf(g_lib[a])) : g_albumMeasurements.end();
    Check(found && reloaded != g_albumMeasurements.end() && reloaded->second.members.size() == 2 &&
              reloaded->second.gain == albumGain && reloaded->second.peak == albumPeak &&
              g_lib[a].rgAlbumGain == albumGain && (g_lib[a].flags & Track::kRgAlbumMeasured),
          L"RG a measured album comes back with its gain, peak and members");
    if (found) {
        g.rgAlbumsRevision = 0;
        g.rgNextAuto = 1;
        unsigned generation = g.rgGen;
        MaybeAutoMeasure(false);
        batch.Next(L"RG startup keeps saved measurements instead of working them out again");
        Check(g.rgGen == generation && !g.rgRunning,
              L"RG startup does not rescan completed, older or unsuccessful measurements");
        // Make the saved album a lift, so its stored levelling decides the gain: a fresh
        // calculation would ignore a dynamics value that did not come from its tracks.
        g_albumMeasurements[AlbumKeyOf(g_lib[a])].gain = g_lib[a].rgAlbumGain = g_lib[b].rgAlbumGain = 500;
        g_lib.Touch();
        g.rgNextAuto = 1;
        MaybeAutoMeasure(false);
        const float plain = RgGainFor(a);
        g_albumMeasurements[AlbumKeyOf(g_lib[a])].dynamics = 60;  // 6 dB: the most a lift is held back
        g_lib.Touch();
        g.rgNextAuto = 1;
        MaybeAutoMeasure(false);
        Check(!g.rgRunning && plain > 1.5f && RgGainFor(a) < plain - 0.01f,
              L"RG a saved album's levelling is kept and applied, not worked out again");
        // A track added to the album does not take the measured album's gain away while it waits.
        g.rgAutoScan = false;
        WriteTestBytes(albumC, MakeSineWav(2, 2));
        TrackId c = add(albumC);
        album(c);
        g_lib.Touch();
        g.rgNextAuto = 1;
        MaybeAutoMeasure(false);
        Check(g_lib[a].rgAlbumGain == 500 && (g_lib[a].flags & Track::kRgAlbumMeasured) && g_lib[c].rgAlbumGain == kNoGain,
              L"RG a measured album keeps its gain while a track added since is unmeasured");
        const float partialGain = RgGainFor(a);
        batch.Next(L"RG an incomplete album restores its saved levelling after restart and compaction");
        saved = SaveLibrary(file, g.playlists);
        ForgetAlbumDynamics();
        loaded = LoadLibrary(file, g.playlists);
        g.rgNextAuto = 1;
        MaybeAutoMeasure(false);
        Check(saved && loaded && std::fabs(RgGainFor(a) - partialGain) < 0.0001f,
              L"RG an incomplete album restores its saved levelling after restart");
        // Compaction can renumber both handles and IDs; it must preserve the same measurement.
        std::vector<TrackId> live;
        for (TrackId id = 0; id < g_lib.Count(); ++id) live.push_back(id);
        std::reverse(live.begin(), live.end());
        auto mapping = g_lib.Compact(live);
        a = mapping[a];
        failed = mapping[failed];
        ForgetAlbumDynamics(true);
        MaybeAutoMeasure(false);
        auto compacted = g_albumMeasurements.find(AlbumKeyOf(g_lib[a]));
        Check(compacted != g_albumMeasurements.end() && compacted->second.members.size() == 2 &&
                  std::fabs(RgGainFor(a) - partialGain) < 0.0001f,
              L"RG compaction remaps saved members and keeps an incomplete album's levelling");
        g.rgAutoScan = true;
        finish();
        ScanReplayGain({failed});
        batch.Next(L"RG an explicit scan retries a saved failure and clearing forgets the attempt");
        Check(g.rgGen > generation && finish() && (g_lib[failed].flags & Track::kRgScanFailed),
              L"RG an explicit scan retries a saved failure");
        ClearReplayGain({failed});
        Check(!(g_lib[failed].flags & Track::kRgScanFailed), L"RG clearing information also clears the failed attempt");
        g.rgNextAuto = 1;
        generation = g.rgGen;
        MaybeAutoMeasure(false);
        batch.Next(L"RG unmeasured tracks still scan automatically");
        Check(g.rgGen > generation && finish(), L"RG unmeasured tracks still scan automatically");
    }
    ForgetAlbumDynamics();
    DeleteFileW(file.c_str());
    DeleteFileW((file + L".bak").c_str());
    DeleteFileW(sine.c_str());
    DeleteFileW(silent.c_str());
    DeleteFileW(albumA.c_str());
    DeleteFileW(albumB.c_str());
    DeleteFileW(albumC.c_str());
}

void TestFolderBrowser() {
    using namespace app;
    testlog::Group group(L"Folder browser");
    testlog::Batch batch;
    BrowserPane saved[3];
    const BrowseField fields[] = {BrowseField::Folder, BrowseField::Genre, BrowseField::Album};
    for (int i = 0; i < 3; ++i) {
        saved[i] = g.panes[i];
        g.panes[i].field = fields[i];
        g.panes[i].selected.clear();
    }
    auto add = [](const wchar_t* path, const char* genre) {
        TrackId id = g_lib.Add(path);
        TagData tags;
        tags.genre = genre;
        tags.album = "Folder test";
        g_lib.ApplyTags(id, tags);
        return id;
    };
    TrackId a = add(L"C:\\folder-browser\\Música; live\\Disc 2\\one.flac", "Rock");
    TrackId b = add(L"C:\\folder-browser\\Música; live\\Disc 2\\two.flac", "Jazz");
    TrackId c = add(L"C:\\folder-browser\\Other\\Disc 2\\three.flac", "Rock");
    TrackId child = add(L"C:\\folder-browser\\Música; live\\Disc 2\\Extras\\four.flac", "Rock");
    TrackId cue = add(L"C:\\folder-browser\\Música; live\\Disc 2\\image.flac|2", "Rock");
    TrackId root = add(L"C:\\folder-browser-root.flac", "Rock");
    TrackId unc = add(L"\\\\server\\music\\Disc 2\\five.flac", "Rock");
    const std::vector<TrackId> source{a, b, c, child, cue, root, unc};
    testlog::Cases paths;
    std::vector<StringPool::Handle> values;
    for (TrackId id : source) {
        BrowserValues(id, BrowseField::Folder, values);
        paths.Add(values == std::vector<StringPool::Handle>{g_lib[id].dir} &&
                      g_lib.Str(values.front()) == DirectoryOf(g_lib.Path(id)), g_lib.Path(id));
    }
    batch.Next(L"folders show full paths and counted rows, including Unicode, semicolons, CUE, roots and UNC");
    Check(paths.Passed(), L"folders use full paths, including Unicode, semicolons, CUE, drive roots and UNC%ls",
          paths.failed.c_str());
    auto items = source;
    FilterLibrary(items);
    auto& folder = g.panes[0];
    auto count = std::find(folder.values.begin(), folder.values.end(), g_lib[a].dir);
    Check(items == source && folder.values.size() == 5 && count != folder.values.end() &&
              folder.counts[count - folder.values.begin()] == 3,
          L"folder rows count their tracks and keep namesakes and subfolders separate");
    NMLVDISPINFOW row{};
    wchar_t text[256]{};
    row.hdr.code = LVN_GETDISPINFOW;
    row.item.mask = LVIF_TEXT;
    row.item.pszText = text;
    row.item.cchTextMax = (int)std::size(text);
    OnBrowserNotify(0, &row.hdr);
    Check(!wcscmp(text, L"All (5 folders)"), L"the Folder pane names its All row");
    folder.selected = {g_lib[a].dir};
    items = source;
    FilterLibrary(items);
    batch.Next(L"folder and tag selections filter the view, combine, drop absent folders and clear");
    Check(items == std::vector<TrackId>{a, b, cue} && g.panes[1].values.size() == 2 &&
              g.panes[2].counts == std::vector<uint32_t>{3},
          L"selecting a folder filters tracks and later panes without including subfolders");
    g.panes[1].selected = {g_lib[a].genre};
    items = source;
    FilterLibrary(items);
    Check(items == std::vector<TrackId>{a, cue}, L"folder and tag selections narrow the same library view");
    g.panes[1].selected.clear();
    folder.selected.push_back(g_lib[c].dir);
    items = source;
    FilterLibrary(items);
    Check(items == std::vector<TrackId>{a, b, c, cue}, L"several selected folders combine their tracks");
    // A changed search or an earlier pane can remove a selected folder from the input.
    items = {child, unc};
    FilterLibrary(items);
    Check(folder.selected.empty() && items == std::vector<TrackId>{child, unc},
          L"folders no longer present upstream drop out of the selection");
    folder.selected.clear();
    items = source;
    FilterLibrary(items);
    Check(items == source && ListView_GetItemState(folder.list, 0, LVIS_SELECTED),
          L"All clears folder filtering and selects the All row");
    if (!kExtraThemes) {
        testlog::Skip(L"the foobar2000 layout: this build leaves out the extra themes");
    } else {
        const ThemeMode theme = g.themeMode;
        const auto selectedFolder = g_lib[0].dir;
        SelectBrowserValue(BrowseField::Folder, selectedFolder);
        OnCommand(IDM_THEME_BASE + (int)ThemeMode::Foobar, 0, nullptr);
        batch.Next(L"the foobar2000 layout keeps and restores the column browser's folder choice");
        Check(FoobarMode() && folder.field == BrowseField::Folder &&
                  folder.selected == std::vector<StringPool::Handle>{selectedFolder},
              L"the foobar2000 layout keeps the column browser's folder choice and selection");
        OnCommand(IDM_THEME_BASE + (int)theme, 0, nullptr);
        Check(folder.field == BrowseField::Folder && folder.selected == std::vector<StringPool::Handle>{selectedFolder} &&
                  g.playlists[0].items == std::vector<TrackId>{0, 1, 2},
              L"returning from foobar2000 restores the selected folder's tracks");
    }
    for (int i = 0; i < 3; ++i) g.panes[i] = std::move(saved[i]);
    SaveBrowserSettings();
    g.playlists[0].builtRevision = 0;
    RebuildView();
}
void TestFoobarControls() {
    using namespace app;
    testlog::Group group(L"Foobar folders and transport controls");
    testlog::Batch batch;
    if (!kExtraThemes) {
        testlog::Skip(L"the foobar2000 controls: this build leaves out the extra themes");
        return;
    }
    const auto theme = g.themeMode;
    const UINT dpi = g.dpi;
    auto folders = g.libraryFolders;
    const int active = g.active;
    const ViewSize size = g.size;
    g.size = ViewSize::Normal;
    OnCommand(IDM_THEME_BASE + (int)ThemeMode::Foobar, 0, nullptr);
    Layout();  // the hidden test window does not route the real window's theme/size messages
    std::vector<TrackId> added;
    for (const wchar_t* path : {L"D:\\Custom Music\\Band\\Album\\one.flac",
                                L"E:\\Custom Music\\Band\\Album\\two.flac",
                                L"\\\\wsl.localhost\\Linux\\home\\user\\Music\\three.flac"}) {
        TrackId id = g_lib.Add(path);
        g_lib[id].flags |= Track::kInLibrary | Track::kLoaded;
        added.push_back(id);
    }
    g.libraryFolders = {L"D:\\Custom Music", L"E:\\Custom Music", L"\\\\wsl.localhost\\Linux\\home\\user\\Music"};
    SaveLibrarySettings();
    g.libraryFolders.clear();
    LoadLibrarySettings();
    batch.Next(L"custom roots on other drives and a Linux share survive saving and expand in the native Album List");
    Check(g.libraryFolders.size() == 3 && g.libraryFolders[0] == L"D:\\Custom Music" &&
              g.libraryFolders[2] == L"\\\\wsl.localhost\\Linux\\home\\user\\Music",
          L"custom roots on other drives and a Linux share survive saving and loading");
    HWND panel = FindWindowExW(g.hwnd, nullptr, L"CordinalFoobarPanel", nullptr);
    HWND tree = GetDlgItem(panel, IDC_FB_TREE);
    int customRoots = 0;
    HTREEITEM first = nullptr;
    for (HTREEITEM item = TreeView_GetRoot(tree); item; item = TreeView_GetNextSibling(tree, item)) {
        wchar_t text[512]{};
        TVITEMW row{};
        row.mask = TVIF_TEXT;
        row.hItem = item;
        row.pszText = text;
        row.cchTextMax = (int)std::size(text);
        TreeView_GetItem(tree, &row);
        if (wcsstr(text, L"Custom Music") || wcsstr(text, L"wsl.localhost")) {
            ++customRoots;
            if (!first) first = item;
        }
    }
    Check(tree && (GetWindowLongW(tree, GWL_STYLE) & TVS_LINESATROOT) && customRoots == 3 &&
              first && TreeView_GetChild(tree, first),
          L"native Album List exposes expandable, separate custom and Linux roots with their first level open (tree=%p roots=%d)",
          tree, customRoots);
    auto token = g.nextToken;
    if (first) TreeView_SelectItem(tree, first);
    UINT before = first ? TreeView_GetItemState(tree, first, TVIS_EXPANDED) : 0;
    NMHDR dbl{tree, IDC_FB_TREE, NM_DBLCLK};
    SendMessageW(panel, WM_NOTIFY, IDC_FB_TREE, (LPARAM)&dbl);
    Check(first && TreeView_GetItemState(tree, first, TVIS_EXPANDED) != before && g.nextToken == token,
          L"double-clicking a folder expands or collapses it without starting playback");
    ShowLibraryFolders();
    batch.Next(L"Library folders offers the system Music folder and a custom folder picker");
    Check(GetDlgItem(g.foldersWnd, IDC_FOLDERS_MUSIC) && GetDlgItem(g.foldersWnd, IDC_FOLDERS_ADD),
          L"Library folders offers the system Music folder and a custom folder picker in every theme");
    DestroyWindow(g.foldersWnd);

    RECT originalWindow{};
    GetWindowRect(g.hwnd, &originalWindow);
    const Dock dock = g.layout.player;
    g.layout.player = Dock::Left;
    Layout();
    batch.Next(L"foobar2000 transport keeps horizontal sliders, fits every size and DPI and agrees on seek positions");
    Check(!SeekVertical(), L"foobar2000 keeps horizontal sliders when the standard layout is side-docked");
    bool enabled = IsWindowEnabled(g.seek) != 0;
    EnableWindow(g.seek, TRUE);
    testlog::Cases layout, positions;
    for (UINT scale : {96u, 192u}) {
        g.dpi = scale;
        CreateFonts();
        ApplyFonts();
        for (int width : {640, 1120, 1440}) {
            SetWindowPos(g.hwnd, nullptr, 0, 0, S(width), S(700), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            Layout();
            RECT seek{}, volume{}, client{};
            GetWindowRect(g.seek, &seek);
            GetWindowRect(g.volume, &volume);
            MapWindowPoints(nullptr, g.hwnd, (POINT*)&seek, 2);
            MapWindowPoints(nullptr, g.hwnd, (POINT*)&volume, 2);
            GetClientRect(g.hwnd, &client);
            layout.Add(volume.right < seek.left && seek.top == volume.top && seek.bottom == volume.bottom &&
                           seek.right + S(14 + ui::kFoobarScopeMin) <= client.right,
                       std::to_wstring(scale) + L"/" + std::to_wstring(width));
        }
        RECT rc{}, track = FoobarSliderTrack(g.seek);
        GetClientRect(g.seek, &rc);
        for (int pos : {0, 250, 750, 1000}) {
            SendMessageW(g.seek, TBM_SETPOS, FALSE, pos);
            POINT at{track.left + (track.right - track.left) * pos / 1000, rc.bottom / 2};
            positions.Add(SendMessageW(g.seek, TBM_GETPOS, 0, 0) == pos &&
                          std::fabs(BarFracAt(g.seek, at) - pos / 1000.0) < 0.02,
                          std::to_wstring(scale) + L"/" + std::to_wstring(pos));
        }
    }
    Check(layout.Passed(), L"foobar transport, sliders and wide scope fit at narrow and wide sizes and 200%% DPI%ls", layout.failed.c_str());
    Check(positions.Passed(), L"seek positions and click coordinates agree at both DPIs%ls", positions.failed.c_str());
    EnableWindow(g.seek, enabled);
    for (TrackId id : added) g_lib[id].flags &= ~Track::kInLibrary;
    g_lib.Touch();
    g.libraryFolders = std::move(folders);
    SaveLibrarySettings();
    g.dpi = dpi;
    g.size = size;
    g.layout.player = dock;
    CreateFonts();
    ApplyFonts();
    OnCommand(IDM_THEME_BASE + (int)theme, 0, nullptr);
    SetWindowPos(g.hwnd, nullptr, 0, 0, originalWindow.right - originalWindow.left,
                 originalWindow.bottom - originalWindow.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    ActivatePlaylist(active);
    Layout();
}
}  // namespace

// Real Win32 controls under a hidden test window: no player instance or user settings are touched.
// The queue editor and Choose by taste's picks: yours stay ahead of the picks, edits keep that
// order, picks top themselves up from the shuffle pass or Keep playing like this without repeats,
// and the Up next panel's keys and drags edit the queue.
void TestQueueEditor(const std::wstring& scratch) {
    using namespace app;
    testlog::Group group(L"queue editor and taste picks");
    testlog::Batch batch;
    const auto savedQueue = g.queue;
    const auto savedTokens = g.tokens;
    const MainLayout savedLayout = g.layout;
    const TrackId savedPlaying = g.playing;
    const auto savedPlayingToken = g.playingToken, savedDecoding = g.decodingToken;
    const int savedShuffle = g.shuffle;
    const bool savedTaste = g.taste, savedRadio = g.radio;
    const Repeat savedRepeat = g.repeat;
    std::vector<TrackId> ids;
    for (int i = 0; i < 7; ++i) ids.push_back(g_lib.Add(scratch + L"\\queue-" + std::to_wstring(i) + L".flac"));
    NewPlaylist(L"Queue editor");
    Active().items = ids;
    RebuildView();
    g.queue.clear();
    g.queuePicks = 0;
    g.repeat = Repeat::Off;
    g.radio = false;
    g.shuffle = kShuffleTracks;
    g.shuffleQueue.clear();
    g.shuffleStarted = false;
    g.decodingToken = 0;
    g.playing = ids[0];
    g.playingToken = NewToken(ids[0], g.active, 0);
    g.taste = true;
    auto yours = [] { return std::vector<TrackId>(g.queue.begin(), g.queue.end() - (ptrdiff_t)g.queuePicks); };
    auto distinct = [&] {
        std::vector<TrackId> q(g.queue.begin(), g.queue.end());
        std::sort(q.begin(), q.end());
        return std::adjacent_find(q.begin(), q.end()) == q.end() && !std::count(q.begin(), q.end(), ids[0]);
    };

    batch.Next(L"queue: Choose by taste in track shuffle fills the queue from the pass, behind your own tracks");
    RefreshNext();
    Check(g.queuePicks == 5 && g.queue.size() == 5 && distinct() && g.shuffleQueue.size() == 1,
          L"queue: taste picks five tracks from the shuffle pass (%zu picks, %zu left)", g.queuePicks, g.shuffleQueue.size());
    TrackId mine = g_lib.Add(scratch + L"\\queue-mine.flac");
    QueueTracks({mine}, false);
    Check(g.queue.front() == mine && g.queuePicks == 5, L"queue: Add to queue goes ahead of the picks");

    batch.Next(L"queue: moving, keeping and removing keep yours ahead of the picks and top the picks up");
    TrackId pick = g.queue[1];
    MoveInQueue({1}, 0);
    Check(g.queue.front() == pick && yours().size() == 2 && g.queuePicks == 5 && g.shuffleQueue.empty() && distinct(),
          L"queue: a pick moved among yours becomes yours and the last of the pass replaces it");
    int at = MoveInQueue({0}, (int)g.queue.size());
    Check(at == 1 && g.queue[1] == pick && yours().size() == 2, L"queue: your track never moves below the picks");
    RemoveFromQueue({(int)g.queue.size() - 1});
    Check(g.queuePicks == 4 && yours().size() == 2, L"queue: a removed pick isn't replaced once the pass is done");
    std::vector<TrackId> before(g.queue.begin() + 2, g.queue.end());
    RepickTaste();
    std::vector<TrackId> after(g.queue.begin() + 2, g.queue.end());
    std::sort(before.begin(), before.end());
    std::sort(after.begin(), after.end());
    Check(g.queuePicks == 4 && before == after, L"queue: New picks returns the old picks to the pass");
    MoveInQueue({2}, 2, true);
    Check(yours().size() == 3 && g.queuePicks == 3, L"queue: Keep makes a pick yours where it stands");

    batch.Next(L"queue: picks follow the order, and go when taste no longer chooses");
    g.radio = true;
    RefreshNext();
    bool clean = distinct();
    for (size_t i = yours().size(); i < g.queue.size(); ++i)
        clean &= std::find(ids.begin(), ids.end(), g.queue[i]) != ids.end();
    Check(g.picksMode == 2 && g.queuePicks >= 1 && clean && yours().size() == 3,
          L"queue: Keep playing like this picks new tracks from the playlist, none twice (%zu picks)", g.queuePicks);
    g.repeat = Repeat::One;
    RefreshNext();
    Check(g.queuePicks == 0 && g.queue.size() == 3, L"queue: Repeat track drops the picks and keeps yours");
    g.repeat = Repeat::Off;
    g.taste = false;
    RefreshNext();
    Check(g.queuePicks == 0, L"queue: no picks while taste is off");

    batch.Next(L"queue: the Up next panel deletes and drags queued tracks and View > Up next shows it");
    g.radio = false;
    g.shuffle = kShuffleOff;
    g.queue = {ids[1], ids[2], ids[3], ids[4]};
    g.layout.upNext = true;
    if (!UpNextPanel()) CreatePanels(g.hwnd);  // the test window builds only the main controls
    Layout();
    HWND panel = UpNextPanel();
    RECT client{};
    GetClientRect(panel, &client);
    auto rowY = [](int i) { return S(40) + S(44) * i + S(22); };
    SendMessageW(panel, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(S(60), rowY(0)));
    SendMessageW(panel, WM_LBUTTONUP, 0, MAKELPARAM(S(60), rowY(0)));
    SendMessageW(panel, WM_KEYDOWN, VK_DELETE, 0);
    Check(g.queue == std::deque<TrackId>{ids[2], ids[3], ids[4]}, L"queue: Delete in the panel removes the selected track (%zu left, panel %dx%d)",
          g.queue.size(), (int)client.right, (int)client.bottom);
    SendMessageW(panel, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(S(60), rowY(2)));
    SendMessageW(panel, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(S(60), rowY(1)));
    SendMessageW(panel, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(S(60), rowY(0) - S(10)));
    SendMessageW(panel, WM_LBUTTONUP, 0, MAKELPARAM(S(60), rowY(0) - S(10)));
    Check(client.bottom > rowY(3) && g.queue == std::deque<TrackId>{ids[4], ids[2], ids[3]},
          L"queue: dragging the last queued track to the top moves it there");
    HMENU menu = BuildMenu();
    g.layout.upNext = false;
    Layout();
    UpdateMainMenu(menu);
    UINT state = GetMenuState(menu, IDM_QUEUE_SHOW, MF_BYCOMMAND);
    DestroyMenu(menu);
    Check(state != (UINT)-1 && !(state & MF_CHECKED),
          L"queue: View > Up next is in the menu and unchecked while the panel is closed");

    g.queue = savedQueue;
    g.queuePicks = 0;
    g.tokens = savedTokens;
    g.layout = savedLayout;
    Layout();
    g.playing = savedPlaying;
    g.playingToken = savedPlayingToken;
    g.decodingToken = savedDecoding;
    g.shuffle = savedShuffle;
    g.taste = savedTaste;
    g.radio = savedRadio;
    g.repeat = savedRepeat;
    g.shuffleQueue.clear();
    g.shuffleStarted = false;
    RemovePlaylist(g.active);
    batch.End();
}

// Lightweight mode (app_lightweight.cpp) and its song picker (app_palette.cpp): the numpad's keys
// under each Num Lock choice, putting the window away and bringing it back as each preference says,
// and finding music by typing. The keyboard hook and hotkey are held only while the mode is on.
void TestLightweightMode() {
    using namespace app;
    testlog::Batch batch;
    batch.Next(L"lightweight: the numpad is a remote with Num Lock off, and leaves the arrow keys alone");
    const int off = (int)LightweightNumpad::Off, numLockOff = (int)LightweightNumpad::NumLockOff,
              always = (int)LightweightNumpad::Always;
    testlog::Cases keys;
    struct KeyCase { DWORD vk; bool extended; int mode; int want; const wchar_t* name; };
    const KeyCase cases[] = {
        {VK_CLEAR, false, numLockOff, 5, L"5 with Num Lock off"},
        {VK_LEFT, false, numLockOff, 4, L"4 with Num Lock off"},
        {VK_INSERT, false, numLockOff, 0, L"0 with Num Lock off"},
        {VK_DELETE, false, numLockOff, 10, L". with Num Lock off"},
        {VK_LEFT, true, numLockOff, -1, L"the arrow block's Left is not the numpad's"},
        {VK_DELETE, true, always, -1, L"the Delete key is not the numpad's ."},
        {VK_NUMPAD5, false, numLockOff, -1, L"Num Lock on gives the digits back"},
        {VK_ADD, false, numLockOff, -1, L"Num + types unless Preferences says Always"},
        {VK_NUMPAD5, false, always, 5, L"Always takes the digits too"},
        {VK_ADD, false, always, 11, L"Always takes Num + too"},
        {VK_RETURN, true, always, 15, L"the numpad's Enter"},
        {VK_RETURN, false, always, -1, L"the main Enter is never taken"},
        {VK_CLEAR, false, off, -1, L"Off takes nothing"},
        {'5', false, always, -1, L"the top row's 5 is never taken"},
    };
    for (const KeyCase& c : cases) keys.Add(NumpadKeyIndex(c.vk, c.extended, c.mode) == c.want, c.name);
    // Unless changed, the keys do what Preferences says: 5 pauses, 0 picks a song, . brings the window back.
    RegisterGlobalKeys();
    const uint32_t bound = NumpadKeysBound();
    keys.Add((bound & 0x7FF) == 0x7FF && !(bound & ~0x7FFu), L"the eleven Num Lock keys are bound, the rest type");
    const std::pair<int, int> numpadCommands[] = {{5, IDM_PLAY_PAUSE}, {0, IDM_QUICK_PICK}, {10, IDM_LIGHTWEIGHT},
                                                   {8, IDM_VOLUME_UP}};
    const std::vector<keymap::Binding> global = EffectiveBindings(keymap::Scope::Global);
    for (auto [index, command] : numpadCommands) {
        const std::vector<keymap::Key> held = keymap::KeysOf(global, CommandName(command));
        keys.Add(!held.empty() && held.front() == keymap::Key{0, keymap::NumpadName(index)}, keymap::NumpadName(index));
    }
    Check(keys.Passed(), L"lightweight: the numpad's keys map as Preferences describes%ls", keys.failed.c_str());

    batch.Next(L"lightweight: Preferences has a page for it, with the defaults a background player wants");
    const Tweak* shows = FindTweak("lightweight.shows");
    const Tweak* numpad = FindTweak("lightweight.numpad");
    const Tweak* pause = FindTweak("lightweight.pause_work");
    const Tweak* trim = FindTweak("lightweight.trim");
    Check(shows && numpad && pause && trim && shows->def == 0 && numpad->def == 1 && pause->def == 1 && trim->def == 1 &&
              !wcscmp(TweakPage(*shows), L"Lightweight mode") && !wcscmp(TweakPage(*trim), L"Lightweight mode"),
          L"lightweight: its four settings are on their own page, showing nothing and taking the numpad with Num Lock off");

    // The picker finds music by any word, albums first, and lists the transport and playlists unasked.
    batch.Next(L"lightweight: the song picker lists the transport, then finds albums and songs by their words");
    TagData tags;
    tags.title = "Zebra crossing";
    tags.artist = "Picker artist";
    tags.album = "Picker album";
    TrackId zebra = g_lib.Add(L"C:\\ui-test\\lightweight-zebra.flac");
    g_lib.ApplyTags(zebra, tags);
    g_lib[zebra].flags |= Track::kInLibrary;
    g_lib.Touch();
    ShowQuickPick(false);
    std::vector<std::wstring> standing = QuickPickRows();
    SetQuickPickQuery(L"zebra");
    std::vector<std::wstring> found = QuickPickRows();
    SetQuickPickQuery(L"picker artist");
    const size_t byArtist = QuickPickRows().size();
    const bool open = PaletteOpen();
    ClosePalette();
    auto has = [](const std::vector<std::wstring>& rows, const wchar_t* text) {
        return std::any_of(rows.begin(), rows.end(), [&](const std::wstring& r) { return r.starts_with(text); });
    };
    Check(open && has(standing, L"Play") && has(standing, L"Next") && has(standing, L"Lightweight mode") &&
              found.size() == 2 && found[0] == L"Picker album | Album \x203A Picker artist" &&
              found[1] == L"Zebra crossing | Picker artist" && byArtist == 2 && !PaletteOpen(),
          L"lightweight: the picker lists the transport (%zu rows) and finds the album then the song (%zu found)",
          standing.size(), found.size());

    // Each choice of what stays on screen, from the normal window and back.
    const ViewSize savedSize = g.size;
    const int savedShows = g.lightweightShows, savedNumpad = g.lightweightNumpad;
    const bool savedTrim = g.lightweightTrim;
    g.lightweightTrim = false;  // the test process's memory is the harness's, not the player's
    SetViewSize(ViewSize::Normal);
    ShowWindow(g.hwnd, SW_SHOWNOACTIVATE);
    batch.Next(L"lightweight: Nothing puts the window away and the way back brings it as it was");
    g.lightweightShows = (int)LightweightShows::Nothing;
    g.lightweightNumpad = numLockOff;  // starts and stops the hook's thread
    EnterLightweight();
    const bool hidden = g.lightweight && !IsWindowVisible(g.hwnd) && ReducedMemoryMode() && LightweightHidesWindow() &&
                        LightweightPausesWork() && g.size == ViewSize::Normal;
    LeaveLightweight();
    Check(hidden && !g.lightweight && IsWindowVisible(g.hwnd) && !ReducedMemoryMode() && !LightweightPausesWork(),
          L"lightweight: Nothing hides the window and reduces memory; leaving shows it and lifts both");

    batch.Next(L"lightweight: Compact keeps a small window and gives the full size back");
    g.lightweightShows = (int)LightweightShows::Compact;
    g.lightweightNumpad = off;
    EnterLightweight();
    const bool compact = g.lightweight && g.size == ViewSize::Compact && IsWindowVisible(g.hwnd) &&
                         !LightweightHidesWindow() && ReducedMemoryMode();
    LeaveLightweight();
    const bool restored = !g.lightweight && g.size == ViewSize::Normal;
    EnterLightweight();
    SetViewSize(ViewSize::Big);  // choosing a bigger size by hand ends the mode and keeps that size
    const bool chosen = !g.lightweight && g.size == ViewSize::Big;
    Check(compact && restored && chosen,
          L"lightweight: Compact shrinks the window, leaving restores Normal, and a size chosen by hand ends it");

    batch.Next(L"lightweight: the icon's Mini player item opens and closes it while the mode stays on");
    SetViewSize(ViewSize::Normal);
    g.lightweightShows = (int)LightweightShows::Nothing;
    EnterLightweight();
    TrayMiniPlayer();
    const bool trayOpened = g.lightweight && MiniPlayerWindow() && !IsWindowVisible(g.hwnd);
    TrayMiniPlayer();
    const bool trayClosed = g.lightweight && !MiniPlayerWindow() && !IsWindowVisible(g.hwnd);
    LeaveLightweight();
    g.lightweightShows = (int)LightweightShows::Mini;
    EnterLightweight();
    const bool modeMini = g.lightweight && MiniPlayerWindow();
    TrayMiniPlayer();  // the one the mode opened closes the same way
    const bool modeMiniClosed = g.lightweight && !MiniPlayerWindow() && !IsWindowVisible(g.hwnd);
    LeaveLightweight();
    Check(trayOpened && trayClosed && modeMini && modeMiniClosed && !g.lightweight && IsWindowVisible(g.hwnd),
          L"lightweight: the icon's Mini player opens it (%d), closes it with the mode still on (%d), and closes the "
          L"mode's own mini player the same way (%d, %d)", trayOpened, trayClosed, modeMini, modeMiniClosed);

    // The mini player draws its disc itself (app_mini.cpp): see-through corners, an opaque puck, and no
    // threads of a graphics rasterizer, which Direct2D's software target started (a dozen on this machine).
    batch.Next(L"mini player: drawn by hand, with clear corners, a solid disc and no new threads");
    const unsigned threadsBefore = ProcessThreads();
    ToggleMiniPlayer();
    MiniPlayerTick();
    const int d = S(196);
    const uint32_t corner = MiniPlayerPixel(0, 0), middle = MiniPlayerPixel(d / 2, S(30)), ring = MiniPlayerPixel(d / 2, S(9));
    const unsigned threadsAfter = ProcessThreads();
    const bool drawn = MiniPlayerWindow() != nullptr;
    ToggleMiniPlayer();
    Check(drawn && corner == 0 && (middle >> 24) == 0xFF && (ring >> 24) == 0xFF && threadsAfter <= threadsBefore + 2,
          L"mini player: corner %08X is clear, the disc %08X and the ring %08X are opaque, threads %u -> %u",
          corner, middle, ring, threadsBefore, threadsAfter);

    ShowWindow(g.hwnd, SW_HIDE);
    KillTimer(g.hwnd, kUiTimer);
    SetViewSize(savedSize);
    g.lightweightShows = savedShows;
    g.lightweightNumpad = savedNumpad;
    g.lightweightTrim = savedTrim;
    batch.End();
}

// Your own keys (app_keys.cpp, keymap.h): the shared rules, then a key given to a command and one of
// cordinal's taken away, as the accelerator table, settings.ini and the menus show them, the window you
// set them in, and Reset all.
void TestKeyboardShortcuts() {
    using namespace app;
    testlog::Batch batch;
    batch.Next(L"keys: the shared rules for naming, allowing and laying keys over cordinal's own");
    testlog::Cases shared;
    for (const auto& group : keymap_checks::All())
        for (const std::string& failure : group.failures) shared.Add(false, FromUtf8(group.name + ": " + failure));
    Check(shared.Passed(), L"keys: the shared key rules hold%ls", shared.failed.c_str());

    HMENU bar = MainMenuBar() ? nullptr : BuildMenu();  // the test window is created without one
    if (bar) SetMenu(g.hwnd, bar);
    auto accelerator = [](BYTE mods, WORD key) {
        int n = CopyAcceleratorTableW(g.accel, nullptr, 0);
        std::vector<ACCEL> table(n);
        CopyAcceleratorTableW(g.accel, table.data(), n);
        for (const ACCEL& a : table)
            if ((a.fVirt & ~FNOINVERT) == (mods | FVIRTKEY) && a.key == key) return (int)a.cmd;
        return 0;
    };
    auto label = [](int command) {
        wchar_t text[128]{};
        GetMenuStringW(MainMenuBar(), command, text, (int)std::size(text), MF_BYCOMMAND);
        const wchar_t* tab = wcschr(text, L'\t');
        return std::wstring(tab ? tab + 1 : L"");
    };
    if (!g.accel) g.accel = BuildAccelerators();
    const int ctrlH = accelerator(FCONTROL, 'H');
    const std::wstring shuffleLabel = label(IDM_SHUFFLE);

    batch.Next(L"keys: a key you set runs its command, one you clear stops, and both are saved");
    keymap::Key ctrlShiftK, ctrlH_;
    keymap::Parse(L"Ctrl+Shift+K", ctrlShiftK);
    keymap::Parse(L"Ctrl+H", ctrlH_);
    std::vector<keymap::Binding> changes;
    keymap::Assign(changes, DefaultBindings(), keymap::Scope::Window, ctrlShiftK, L"Playback > Next");
    keymap::Assign(changes, DefaultBindings(), keymap::Scope::Window, ctrlH_, L"");
    SetKeyChanges(changes);
    Check(ctrlH == IDC_SHUFFLE && shuffleLabel == L"Ctrl+H" && accelerator(FCONTROL | FSHIFT, 'K') == IDM_NEXT &&
              !accelerator(FCONTROL, 'H') && IniRead(L"Keys", L"Ctrl+Shift+K", L"") == L"Playback > Next" &&
              IniRead(L"Keys", L"Ctrl+H", L"(none)").empty(),
          L"keys: Ctrl+Shift+K runs Next and Ctrl+H no longer shuffles, in the table and settings.ini (Ctrl+H was %d, "
          L"label %ls; Ctrl+Shift+K runs %d, Ctrl+H %d; %zu changes; ini \"%ls\")",
          ctrlH, shuffleLabel.c_str(), accelerator(FCONTROL | FSHIFT, 'K'), accelerator(FCONTROL, 'H'), KeyChanges().size(),
          IniRead(L"Keys", L"Ctrl+Shift+K", L"").c_str());
    Check(label(IDM_NEXT) == L"Ctrl+Shift+K" && label(IDM_SHUFFLE).empty(),
          L"keys: the menus show the key you set and drop the one you cleared (Next: %ls, Shuffle: %ls)",
          label(IDM_NEXT).c_str(), label(IDM_SHUFFLE).c_str());

    batch.Next(L"keys: the window lists every command with its keys, and Reset all brings cordinal's back");
    ShowKeyboardShortcuts();
    HWND window = FindWindowW(L"CordinalKeys", nullptr);
    HWND list = window ? FindWindowExW(window, nullptr, WC_LISTBOXW, nullptr) : nullptr;
    const int rows = list ? (int)SendMessageW(list, LB_GETCOUNT, 0, 0) : 0;
    HWND search = window ? FindWindowExW(window, nullptr, WC_EDITW, nullptr) : nullptr;
    if (search) SetWindowTextW(search, L"volume up");
    const int found = list ? (int)SendMessageW(list, LB_GETCOUNT, 0, 0) : 0;
    if (window) DestroyWindow(window);
    Check(rows > 80 && found == 1, L"keys: the window lists %d commands and finds Volume up by name (%d found)", rows, found);
    SetKeyChanges({});
    Check(accelerator(FCONTROL, 'H') == IDC_SHUFFLE && !accelerator(FCONTROL | FSHIFT, 'K') && label(IDM_SHUFFLE) == L"Ctrl+H" &&
              IniRead(L"Keys", L"Ctrl+Shift+K", L"(none)") == L"(none)",
          L"keys: Reset all puts back cordinal's own keys and empties settings.ini's [Keys]");
    batch.End();
}

// Every menu answers the keyboard: in the menu bar and the row and tab right-click menus, no two items
// of one popup share an access key (a shared key only moves between them, so neither runs), and every
// item the menus always show has one. The commands only a right-click held are in the palette too, and
// a key saved under a command's old name still finds it.
void TestMenuAccessKeys() {
    using namespace app;
    testlog::Batch batch;
    batch.Next(L"menus: one access key per item in every popup");
    HMENU bar = MainMenuBar() ? nullptr : BuildMenu();  // the test window is created without one
    if (bar) SetMenu(g.hwnd, bar);
    UpdateMainMenu(MainMenuBar());
    testlog::Cases cases;
    // `fixed`: the popup's items are cordinal's own, not themes, packs, helpers or numbers.
    std::function<void(HMENU, const std::wstring&, bool)> walk = [&](HMENU menu, const std::wstring& path, bool fixed) {
        std::map<wchar_t, std::wstring> keys;
        for (int i = 0, n = GetMenuItemCount(menu); i < n; ++i) {
            wchar_t text[256]{};
            MENUITEMINFOW mi{sizeof(mi)};
            mi.fMask = MIIM_FTYPE | MIIM_SUBMENU | MIIM_STRING | MIIM_ID;
            mi.dwTypeData = text;
            mi.cch = (UINT)std::size(text);
            if (!GetMenuItemInfoW(menu, i, TRUE, &mi) || (mi.fType & MFT_SEPARATOR) || !text[0]) continue;
            std::wstring label = text;
            label = label.substr(0, label.find(L'\t'));
            wchar_t key = 0;
            for (size_t k = 0; k + 1 < label.size(); ++k) {
                if (label[k] != L'&') continue;
                if (label[k + 1] != L'&') key = (wchar_t)towlower(label[k + 1]);
                ++k;
            }
            const std::wstring where = path + L" > " + label;
            if (key) {
                auto [at, added] = keys.emplace(key, where);
                cases.Add(added, where + L" shares its key with " + at->second);
            } else {
                cases.Add(!fixed, where + L" has no access key");
            }
            // Themes, packs, helpers, speeds, ratings and the gaps to find are lists of
            // names, where a missing key is no fault.
            if (mi.hSubMenu) {
                std::wstring name;
                for (wchar_t c : label)
                    if (c != L'&') name += c;
                bool list = false;
                for (const wchar_t* named : {L"Theme", L"Packs", L"Helpers", L"Run", L"Speed", L"Tempo", L"Rating",
                                             L"Find missing information"})
                    list = list || name.rfind(named, 0) == 0;
                walk(mi.hSubMenu, where, fixed && !list);
            }
        }
    };
    walk(MainMenuBar(), L"Menu bar", true);
    if (!Active().items.empty()) ListView_SetItemState(g.list, 0, LVIS_SELECTED, LVIS_SELECTED);
    HMENU row = BuildListContextMenu(), tab = BuildTabContextMenu(g.active);
    walk(row, L"Row menu", true);
    walk(tab, L"Tab menu", true);
    DestroyMenu(row);
    DestroyMenu(tab);
    Check(cases.Passed(), L"menus: every item has its own access key%ls", cases.failed.c_str());

    batch.Next(L"menus: the menu bar builds before the window's list view exists");
    // wWinMain builds the bar before CreateWindowExW, and the editions submenu asks for the selection
    // on the way. Asking a missing list (it answered 0 for ever) used to stop the player starting.
    HWND list = g.list, canvas = g.libView;
    g.list = nullptr;
    g.libView = nullptr;
    HMENU startup = BuildMenu();
    g.list = list;
    g.libView = canvas;
    Check(startup != nullptr, L"menus: BuildMenu with no list view still leaves a menu bar");
    if (startup) DestroyMenu(startup);

    batch.Next(L"menus: right-click commands are in the palette, and old names still find their command");
    bool playNext = false, explorer = false;
    for (const MenuCommandInfo& c : AllMenuCommands()) {
        playNext = playNext || (c.command == IDM_QUEUE_NEXT && c.label == L"Play next");
        explorer = explorer || (c.command == IDM_SHOW_IN_EXPLORER && c.label == L"Show in Explorer");
    }
    Check(playNext && explorer, L"menus: the palette lists Play next (%d) and Show in Explorer (%d)", playNext, explorer);
    Check(MenuCommandByName(L"Edit > Reload selected metadata") == IDM_RELOAD_TAGS &&
              MenuCommandByName(L"Edit > Reload tags from files") == IDM_RELOAD_TAGS,
          L"menus: a key saved as Edit > Reload selected metadata still runs Reload tags from files");
    batch.End();
}

void TestUiControls(const std::wstring& scratch) {
    using namespace app;
    testlog::Batch batch;
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES | ICC_TAB_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);
    g.inst = GetModuleHandleW(nullptr);
    g.iniPath = scratch + L"\\ui-test-settings.ini";
    g.dataDir = scratch;
    radio::Station station;
    station.id = "local-station"; station.name = "Test rock station"; station.url = "http://127.0.0.1:1/";
    radio::Station frequencyStation;
    frequencyStation.id = "frequency-station"; frequencyStation.name = "98.5 Test FM";
    frequencyStation.url = "https://example.test/test.m3u8"; frequencyStation.hls = true;
    const std::wstring radioDir = scratch + L"\\radio";
    bool radioFolder = CreateDirectoryW(radioDir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
    if (!Ready(radioFolder && radio::SaveFavorites(radioDir + L"\\stations.json", {station, frequencyStation}),
               L"saved stations for UI")) return;
    g_lib = Library();
    g.playlists = {{L"Library", {}, PlaylistKind::Library}, {L"Test playlist", {}},
                   {L"Test auto-playlist", {}, PlaylistKind::Auto}};
    g.active = 0;
    g.playing = kNone;
    for (int i = 0; i < 3; ++i) {
        TrackId id = g_lib.Add(L"C:\\ui-test\\track" + std::to_wstring(i) + L".flac");
        TagData tags;
        tags.title = "Track " + std::to_string(i);
        tags.artist = "Test artist";
        tags.album = "Test album";
        g_lib.ApplyTags(id, tags);
        g_lib[id].flags |= Track::kInLibrary;
    }
    g_lib.Touch();
    WNDCLASSW wc{};
    wc.hInstance = g.inst;
    wc.lpszClassName = L"CordinalUiSelfTest";
    wc.lpfnWndProc = [](HWND h, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
        if (msg == WM_GETMINMAXINFO) {
            // Hidden DPI checks need their requested geometry even on a small CI desktop.
            auto* limits = reinterpret_cast<MINMAXINFO*>(lp);
            limits->ptMaxTrackSize = {S(kUiWidth), S(kUiHeight)};
            return 0;
        }
        if (msg == WM_APP_IMPORT) { OnImportResult(lp); return 0; }
        if (msg == WM_APP_STEAM) { OnSteamDiscovery(wp, lp); return 0; }
        if (msg == WM_APP_ART) { OnArtResult(lp); return 0; }
        if (msg == WM_APP_META) { AcceptTagBatch(reinterpret_cast<std::vector<TagResult>*>(lp)); return 0; }
        if (msg == WM_APP_RG) { FinishReplayGain(wp, lp); return 0; }
        if (msg == WM_APP_HELPER) { OnHelperMessage(wp, lp); return 0; }
        if (msg == WM_TIMER && wp == kSearchTimer) { FlushSearch(); return 0; }
        if (msg == WM_COMMAND) { OnCommand(LOWORD(wp), HIWORD(wp), (HWND)lp); return 0; }
        if (msg == WM_NOTIFY && ((NMHDR*)lp)->hwndFrom == g.list) return OnListNotify((NMHDR*)lp);
        return DefWindowProcW(h, msg, wp, lp);
    };
    RegisterClassW(&wc);
    g.hwnd = CreateWindowW(wc.lpszClassName, L"UI self-test", WS_OVERLAPPEDWINDOW, 0, 0, kUiWidth, kUiHeight,
                           nullptr, nullptr, g.inst, nullptr);
    if (!Ready(g.hwnd != nullptr, L"the hidden test window is created")) return;
    CreateFonts();
    CreateControls(g.hwnd);
    g.pointsChip = CreateWindowW(WC_STATICW, L"", WS_CHILD, 0, 0, 0, 0, g.hwnd, nullptr, g.inst, nullptr);
    ApplyFonts();
    RefreshTabs();
    Layout();
    {
        // The tag editor offers values already in the library; this is the field wiring, while the
        // shared matching rule is covered by the "tags" suite (tag_suggest_checks.h).
        ShowTagEditor({0});
        HWND tag = FindWindowW(L"CordinalTagEditor", nullptr);
        HWND artist = tag ? GetDlgItem(tag, 101) : nullptr;  // 100 + Artist
        HWND list = tag ? FindWindowExW(tag, nullptr, WC_LISTBOXW, nullptr) : nullptr;
        const bool opened = tag && artist && list;
        bool matched = false, accepted = false;
        if (opened) {
            ShowWindow(tag, SW_HIDE);  // the checks need no visible window
            SetWindowTextW(artist, L"Test a");
            wchar_t text[128]{};
            const int count = (int)SendMessageW(list, LB_GETCOUNT, 0, 0);
            if (count > 0) SendMessageW(list, LB_GETTEXT, 0, (LPARAM)text);
            const bool shown = (GetWindowLongPtrW(list, GWL_STYLE) & WS_VISIBLE) != 0;
            matched = shown && count == 1 && !wcscmp(text, L"Test artist");
            SendMessageW(list, WM_LBUTTONUP, 0, MAKELPARAM(S(6), S(6)));  // click the first row
            accepted = WindowText(artist) == L"Test artist" &&
                       (GetWindowLongPtrW(list, GWL_STYLE) & WS_VISIBLE) == 0;
        }
        if (tag) DestroyWindow(tag);
        batch.Next(L"UI: the tag editor completes an artist from the library and accepts it");
        Check(opened && matched && accepted,
              L"UI: the tag editor completes an artist from the library and accepts it");
    }
    {
        // Library > Convert...: the summary follows the format and the mode, and the copies' folder
        // row is only for copies. Nothing is converted here (the cord-conversion suite runs it).
        TrackId song = g_lib.Add(L"C:\\UiChecks\\Convert\\song.flac");
        ShowConvert({song});
        HWND window = FindWindowW(L"CordinalConvert", nullptr);
        HWND format = window ? GetDlgItem(window, 100) : nullptr, replace = window ? GetDlgItem(window, 102) : nullptr;
        HWND folder = window ? GetDlgItem(window, 104) : nullptr, summary = window ? GetDlgItem(window, 107) : nullptr;
        HWND go = window ? GetDlgItem(window, IDOK) : nullptr;
        std::wstring asMp3, asFlac;
        bool mp3Ready = false, flacBlocked = false, folderOff = false;
        if (window && format && replace && folder && summary && go) {
            ShowWindow(window, SW_HIDE);
            auto choose = [&](CordExport f) {
                SendMessageW(format, CB_SETCURSEL, (WPARAM)f, 0);
                SendMessageW(window, WM_COMMAND, MAKEWPARAM(100, CBN_SELCHANGE), (LPARAM)format);
                return WindowText(summary);
            };
            asMp3 = choose(CordExport::Mp3_320);
            mp3Ready = IsWindowEnabled(go) != FALSE;
            asFlac = choose(CordExport::Flac);
            flacBlocked = !IsWindowEnabled(go);
            SendMessageW(replace, BM_CLICK, 0, 0);
            folderOff = !IsWindowEnabled(folder);
        }
        if (window) DestroyWindow(window);
        batch.Next(L"UI: the Convert window counts what each format converts and keeps the folder choice to copies");
        Check(mp3Ready && flacBlocked && folderOff && asMp3 == L"1 song to convert" && asFlac.find(L"already .flac") != std::wstring::npos,
              L"UI: the Convert window counts what each format converts (MP3: \"%ls\", FLAC: \"%ls\") and keeps the folder choice "
              L"to copies", asMp3.c_str(), asFlac.c_str());
    }
    {
        // A track with no title, artist or number: the editor offers its file name's, shown as the
        // empty fields' hints, and turning the box off takes the hints away (nothing is saved here).
        TrackId bare = g_lib.Add(L"C:\\UiChecks\\07 - Name Band - Name Song.flac");
        ShowTagEditor({bare});
        HWND tag = FindWindowW(L"CordinalTagEditor", nullptr);
        HWND fill = tag ? GetDlgItem(tag, 204) : nullptr;
        auto cue = [&](int field) {
            wchar_t text[128]{};
            HWND edit = tag ? GetDlgItem(tag, 100 + field) : nullptr;
            if (edit) SendMessageW(edit, EM_GETCUEBANNER, (WPARAM)text, (LPARAM)std::size(text));
            return std::wstring(text);
        };
        bool offered = false, withdrawn = false;
        if (tag && fill) {
            ShowWindow(tag, SW_HIDE);
            offered = (GetWindowLongPtrW(fill, GWL_STYLE) & WS_VISIBLE) && Button_GetCheck(fill) == BST_CHECKED &&
                      cue(0) == L"Name Song" && cue(1) == L"Name Band" && cue(6) == L"7";  // Title, Artist, Track
            Button_SetCheck(fill, BST_UNCHECKED);
            SendMessageW(tag, WM_COMMAND, MAKEWPARAM(204, BN_CLICKED), (LPARAM)fill);
            withdrawn = Button_GetCheck(fill) == BST_UNCHECKED && cue(0).empty() && cue(1).empty();
        }
        if (tag) DestroyWindow(tag);
        batch.Next(L"UI: the tag editor offers an untagged track's title, artist and number from its file name");
        Check(offered && withdrawn,
              L"UI: the tag editor offers an untagged track's title, artist and number from its file name");
    }
    TestPlayerSliders();
    TestChoiceLabels();
    TestSoftCanvas();
    TestCellCache();
    TestCoverFade();
    {
        SetWindowTextW(g.search, L"Track 1");
        FlushSearch();
        ActivateRadio();
        bool shown = RadioActive() && (GetWindowLongPtrW(RadioView(), GWL_STYLE) & WS_VISIBLE) &&
                     !(GetWindowLongPtrW(g.list, GWL_STYLE) & WS_VISIBLE);
        SetWindowTextW(g.search, L"indie");
        bool separate = g.filter == L"Track 1";
        ActivatePlaylist(0);
        batch.Next(L"UI: Radio has its own view and search without changing the library search");
        Check(shown && separate && !RadioActive() && WindowText(g.search) == L"Track 1",
              L"UI: Radio has its own view and search without changing the library search");
        SetWindowTextW(g.search, L""); FlushSearch();
    }
    {
        ActivateRadio();
        SetWindowTextW(g.search, L"");
        TogglePause();
        bool played = RadioPlaying() && RadioStationName() == L"Test rock station";
        bool wave = WaveformShown() && !IsWindowEnabled(g.seek);
        RECT radioTab{}; GetClientRect(RadioTab(), &radioTab);
        LPARAM close = MAKELPARAM(radioTab.right - S(12), radioTab.bottom / 2);
        SendMessageW(RadioTab(), WM_LBUTTONDOWN, MK_LBUTTON, close);
        SendMessageW(RadioTab(), WM_LBUTTONUP, 0, close);
        bool closed = !RadioActive() && !RadioTabOpen() &&
            !(GetWindowLongPtrW(RadioTab(), GWL_STYLE) & WS_VISIBLE) && RadioPlaying();
        OnCommand(IDM_OPEN_RADIO, 0, nullptr);
        batch.Next(L"UI: Radio's tab closes without interrupting playback, Ctrl+W leaves the playlist, and Play retries");
        Check(wave && closed && RadioActive() && RadioTabOpen() && RadioPlaying(),
              L"UI: Radio's close box hides its tab without interrupting live playback, and View reopens it");
        size_t playlists = g.playlists.size();
        OnCommand(IDM_REMOVE_PL, 0, nullptr);
        bool shortcut = !RadioActive() && !RadioTabOpen() && g.playlists.size() == playlists;
        ActivateRadio();
        Check(shortcut && RadioActive() && RadioTabOpen(),
              L"UI: Ctrl+W closes Radio without deleting the playlist underneath it");
        Stop();
        TogglePause();
        Check(played && RadioPlaying() && RadioStationName() == L"Test rock station",
              L"UI: Play starts the selected station and retries it after Stop");
        Stop();
        ActivatePlaylist(0);
    }
    {
        ActivateRadio();
        int before = (int)RadioCount();
        bool savedShown = before > 0;
        HWND discover = FindWindowExW(RadioView(), nullptr, WC_BUTTONW, L"Discover");
        HWND savedButton = FindWindowExW(RadioView(), nullptr, WC_BUTTONW, L"My stations");
        if (discover) SendMessageW(discover, BM_CLICK, 0, 0);
        bool cleared = RadioCount() == 0 && RadioActive();
        if (savedButton) SendMessageW(savedButton, BM_CLICK, 0, 0);
        int after = (int)RadioCount();
        bool restored = after == before;
        testlog::Cases discovery;
        discovery.Add(savedShown, L"My stations shown (" + std::to_wstring(before) + L")");
        discovery.Add(discover != nullptr, L"Discover button");
        discovery.Add(savedButton != nullptr, L"My stations button");
        discovery.Add(cleared, L"Discover starts empty");
        discovery.Add(restored, L"My stations restore (" + std::to_wstring(after) + L")");
        batch.Next(L"UI: Discover clears My stations, My stations restores them, and F removes a filtered favorite");
        Check(discovery.Passed(), L"UI: Discover clears My stations immediately while the directory loads%ls",
              discovery.failed.c_str());
        ActivatePlaylist(0);
    }
    {
        ActivateRadio();
        SetWindowTextW(g.search, L"98.5 FM");
        bool frequencyFound = RadioCount() == 1;
        SendMessageW(RadioView(), WM_KEYDOWN, 'F', 0);
        std::vector<radio::Station> saved;
        Check(frequencyFound && RadioCount() == 0 &&
                  radio::LoadFavorites(radioDir + L"\\stations.json", saved) && saved.size() == 1 &&
                  saved[0].name == "Test rock station",
              L"UI: F removes a frequency-filtered favorite from the grid and disk");
        SetWindowTextW(g.search, L"");
        ActivatePlaylist(0);
    }
    {
        // Hold the UI queue while changing songs: only one lyric result should be outstanding,
        // then the worker must load the latest request when that result is handled.
        const bool wasShown = LyricsPanelShown();
        std::wstring oldPaths[3];
        bool lyricsFiles = true;
        for (TrackId id = 0; id < 3; ++id) {
            oldPaths[id] = g_lib.Path(id);
            const auto name = scratch + L"\\lyric-switch-" + std::to_wstring(id);
            g_lib.Rename(id, name + L".flac");
            lyricsFiles &= WriteTestBytes(name + L".lrc", id == 2 ? "Latest lyric" : "Previous lyric");
        }
        if (!wasShown) ToggleLyricsPanel();
        for (int i = 0; i < 200; ++i) {
            g.playing = (TrackId)(i % 2);
            LyricsTrackChanged();
        }
        g.playing = 2;
        LyricsTrackChanged();
        HWND lyrics = GetDlgItem(g.hwnd, IDC_LYRICS);
        MSG message;
        ULONGLONG until = GetTickCount64() + 5000;
        while (!PeekMessageW(&message, lyrics, WM_APP + 1, WM_APP + 1, PM_NOREMOVE) && GetTickCount64() < until) Sleep(1);
        std::vector<MSG> results;
        while (PeekMessageW(&message, lyrics, WM_APP + 1, WM_APP + 1, PM_REMOVE)) results.push_back(message);
        for (const MSG& result : results) DispatchMessageW(&result);
        until = GetTickCount64() + 5000;
        while (!LyricsReadyFor(2, L"Latest lyric") && GetTickCount64() < until) {
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
            Sleep(1);
        }
        batch.Next(L"UI: rapid song changes keep one lyric read in flight and load the newest lyrics");
        Check(lyricsFiles && results.size() == 1 && LyricsReadyFor(2, L"Latest lyric"),
              L"UI: rapid song changes keep one lyric read in flight and load the newest lyrics");
        g.playing = kNone;
        LyricsTrackChanged();
        if (!wasShown) ToggleLyricsPanel();
        for (TrackId id = 0; id < 3; ++id) {
            g_lib.Rename(id, oldPaths[id]);
            DeleteFileW((scratch + L"\\lyric-switch-" + std::to_wstring(id) + L".lrc").c_str());
        }
    }
    BYTE savedKeys[256]{}, editKeys[256]{};
    GetKeyboardState(savedKeys);
    editKeys[VK_CONTROL] = 0x80;
    SetKeyboardState(editKeys);
    testlog::Cases editorKeys;
    for (WPARAM key : {WPARAM('Z'), WPARAM(VK_LEFT), WPARAM(VK_RIGHT), WPARAM(VK_RETURN)}) {
        MSG msg{};
        msg.hwnd = g.search;
        msg.message = WM_KEYDOWN;
        msg.wParam = key;
        editorKeys.Add(!TranslateMainKey(&msg), L"Ctrl+" + std::to_wstring((unsigned)key));
    }
    batch.Next(L"UI: Ctrl+Z, arrows and Ctrl+A stay in the search and inline-rename editors");
    Check(editorKeys.Passed(), L"UI: Ctrl+Z, Ctrl+Left/Right and Ctrl+Enter stay in the search editor%ls",
          editorKeys.failed.c_str());
    SetWindowTextW(g.search, L"test query"); FlushSearch();
    SendMessageW(g.search, WM_KEYDOWN, 'A', 0);
    DWORD start = 0, end = 0;
    SendMessageW(g.search, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
    testlog::Cases selectAll;
    selectAll.Add(start == 0 && end == 10, L"the search query");
    BeginRename(1);
    SendMessageW(g.rename, EM_SETSEL, 2, 2);
    SendMessageW(g.rename, WM_KEYDOWN, 'A', 0);
    SendMessageW(g.rename, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
    selectAll.Add(start == 0 && end == 13, L"the playlist name");
    Check(selectAll.Passed(), L"UI: Ctrl+A selects the whole search query and playlist name%ls",
          selectAll.failed.c_str());
    EndRename(false);
    SetKeyboardState(savedKeys);
    {
        RECT tab{};
        bool hasTab = TabCtrl_GetItemRect(g.tabs, 1, &tab) != FALSE;
        LPARAM point = MAKELPARAM(tab.left + S(8), (tab.top + tab.bottom) / 2);
        SendMessageW(g.tabs, WM_KEYDOWN, VK_HOME, 0);
        bool keyboardFocus = !(SendMessageW(g.tabs, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS);
        SendMessageW(g.tabs, WM_LBUTTONDOWN, MK_LBUTTON, point);
        SendMessageW(g.tabs, WM_LBUTTONUP, 0, point);
        bool mouseFocus = (SendMessageW(g.tabs, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) != 0;
        batch.Next(L"UI: clicking a tab switches it without a focus box and leaves keyboard focus visible");
        Check(hasTab && keyboardFocus && mouseFocus && !g.rename && TabCtrl_GetCurSel(g.tabs) == 1,
              L"UI: clicking a tab switches it without a focus box; keyboard focus remains visible");

        SendMessageW(g.tabs, WM_LBUTTONDBLCLK, MK_LBUTTON, point);
        SendMessageW(g.tabs, WM_LBUTTONUP, 0, point);
        bool borderless = g.rename && !(GetWindowLongPtrW(g.rename, GWL_EXSTYLE) & WS_EX_CLIENTEDGE) &&
                          !(GetWindowLongPtrW(g.rename, GWL_STYLE) & WS_BORDER);
        DWORD first = 0, last = 0;
        SendMessageW(g.rename, EM_GETSEL, (WPARAM)&first, (LPARAM)&last);
        HDC dc = GetDC(g.hwnd);
        LRESULT brush = WndProc(g.hwnd, WM_CTLCOLOREDIT, (WPARAM)dc, (LPARAM)g.rename);
        bool surface = dc && brush == (LRESULT)g.surfaceBrush && GetBkColor(dc) == SurfaceColor();
        // The foobar2000 window is not linked when this build leaves the extra themes out; its own
        // test above skips there too.
        bool foobarSurface = !kExtraThemes;
        if (kExtraThemes) {
            ThemeMode savedTheme = g.themeMode;
            ViewSize savedSize = g.size;
            const Palette* savedPalette = g.pal;
            g.themeMode = ThemeMode::Foobar;
            g.size = ViewSize::Normal;
            g.pal = &ThemePalette(ThemeMode::Foobar);
            brush = WndProc(g.hwnd, WM_CTLCOLOREDIT, (WPARAM)dc, (LPARAM)g.rename);
            foobarSurface = dc && brush == (LRESULT)GetStockObject(DC_BRUSH) && GetBkColor(dc) == Ref(P().border);
            g.themeMode = savedTheme;
            g.size = savedSize;
            g.pal = savedPalette;
        }
        if (dc) ReleaseDC(g.hwnd, dc);
        batch.Next(L"UI: double-click opens a borderless editor on the tab surface and Enter commits the rename");
        Check(borderless && surface && foobarSurface && first == 0 && last == 13,
              L"UI: double-click selects the name in a borderless editor matching the tab surface");
        if (g.rename) {
            SetWindowTextW(g.rename, L"Renamed playlist");
            SendMessageW(g.rename, WM_KEYDOWN, VK_RETURN, 0);
        }
        Check(!g.rename && g.playlists[1].name == L"Renamed playlist",
              L"UI: Enter still commits the inline playlist rename");
        g.playlists[1].name = L"Test playlist";
        RefreshTabs();
    }
    SetWindowTextW(g.search, L""); FlushSearch();
    auto rect = [](HWND h) {
        RECT r{};
        GetWindowRect(h, &r);
        MapWindowPoints(nullptr, g.hwnd, (POINT*)&r, 2);
        return r;
    };
    testlog::Cases reachable;
    for (UINT dpi : {96u, 144u, 192u}) {
        g.dpi = dpi;
        CreateFonts();
        ApplyFonts();
        ThemeBrowser();
        for (int width : {640, kUiWidth}) {
            SetWindowPos(g.hwnd, nullptr, 0, 0, S(width), S(kUiHeight), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            RECT sized{};
            GetWindowRect(g.hwnd, &sized);
            Layout();
            RECT tabs = rect(g.tabs), views = rect(g.viewButtons[0]), search = rect(g.search);
            RECT toggle = rect(g.viewButtons[kViewButtons - 1]), last = rect(g.viewButtons[kViewButtons - 2]);
            // The hidden test window must really have the size asked for, or the layout says nothing.
            reachable.Add(sized.right - sized.left == S(width) && sized.bottom - sized.top == S(kUiHeight) &&
                              tabs.right - tabs.left >= S(200) && tabs.right <= search.left && views.top >= tabs.bottom &&
                              last.right <= toggle.left,
                          std::to_wstring(width) + L" px at " + std::to_wstring(dpi) + L" DPI");
        }
    }
    batch.Next(L"UI: tabs, search and views remain reachable at 640/1163 px and 96/144/192 DPI");
    Check(reachable.Passed(), L"UI: tabs, search and views remain reachable at 640/1163 px and 96/144/192 DPI%ls",
          reachable.failed.c_str());
    // Customized layouts (app_custom.cpp): wherever the player docks and however tall its waveform,
    // the list keeps its room, nothing of the player lies over it, and a side dock turns the
    // waveform to run down its column.
    std::wstring layoutBad;
    MainLayout savedLayout = g.layout;
    auto shown = [](HWND h) { return (GetWindowLongPtrW(h, GWL_STYLE) & WS_VISIBLE) != 0; };  // the test window itself is hidden
    for (int dock = 0; dock < (int)Dock::Count; ++dock)
        for (int wave = 0; wave < 3; ++wave) {
            g.layout = MainLayout{};
            g.layout.player = (Dock)dock;
            g.layout.waveSize = (uint8_t)wave;
            g.layout.waveOwnRow = wave == 2;
            g.layout.upNext = wave == 1;
            Layout();
            RECT client{}, overlap{};
            GetClientRect(g.hwnd, &client);
            RECT list = rect(g.list), seek = rect(g.seek), play = rect(g.buttons[1]), volume = rect(g.volume);
            bool vertical = dock >= (int)Dock::Left;
            bool ok = !IntersectRect(&overlap, &list, &seek) && !IntersectRect(&overlap, &list, &play) &&
                      !IntersectRect(&overlap, &list, &volume) && !IntersectRect(&overlap, &seek, &play) &&
                      seek.left >= 0 && seek.top >= 0 && seek.right <= client.right && seek.bottom <= client.bottom &&
                      list.right - list.left >= S(200) && SeekVertical() == vertical &&
                      (vertical ? seek.bottom - seek.top >= S(80) : seek.right - seek.left > seek.bottom - seek.top);
            if (!ok) layoutBad += L" " + std::to_wstring(dock) + L"/" + std::to_wstring(wave);
        }
    batch.Next(L"UI: the player docks at every edge in every waveform size");
    Check(layoutBad.empty(), L"UI: the player docks at every edge in every waveform size%s", layoutBad.c_str());
    {
        const int savedBandSize = g.bandWaveSize;
        const UINT savedDpi = g.dpi;
        const ViewSize savedSize = g.size;
        testlog::Cases sizing;
        for (UINT dpi : {96u, 192u}) {
            g.dpi = dpi; CreateFonts(); ApplyFonts();
            SetWindowPos(g.hwnd, nullptr, 0, 0, S(kUiWidth), S(kUiHeight), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            for (ViewSize mode : {ViewSize::Normal, ViewSize::Compact, ViewSize::Spotlight, ViewSize::Big})
                for (Dock dock : {Dock::Top, Dock::Bottom, Dock::Left, Dock::Right})
                    for (bool ownRow : {false, true}) {
                        g.size = mode; g.layout = MainLayout{}; g.layout.player = dock; g.layout.waveOwnRow = ownRow;
                        g.bandWaveSize = 0; Layout();
                        RECT before = rect(g.seek), play = rect(g.buttons[1]), volume = rect(g.volume);
                        g.bandWaveSize = 3; Layout();
                        RECT after = rect(g.seek), playAfter = rect(g.buttons[1]), volumeAfter = rect(g.volume);
                        bool target = mode == ViewSize::Normal && (dock == Dock::Top || dock == Dock::Bottom);
                        bool same = EqualRect(&before, &after) != 0;
                        bool controls = play.bottom - play.top == playAfter.bottom - playAfter.top &&
                            volume.bottom - volume.top == volumeAfter.bottom - volumeAfter.top;
                        sizing.Add(before.bottom > before.top && controls &&
                                       (target ? after.bottom - after.top == S(72) : same),
                                   std::to_wstring(dpi) + L"/" + std::to_wstring((int)mode) + L"/" +
                                       std::to_wstring((int)dock) + L"/" + std::to_wstring(ownRow));
                    }
        }
        batch.Next(L"UI: waveform height affects only normal top/bottom bars at 100% and 200% DPI");
        Check(sizing.Passed(), L"UI: waveform height affects only normal top/bottom bars at 100%% and 200%% DPI%ls",
              sizing.failed.c_str());
        g.bandWaveSize = savedBandSize; g.size = savedSize; g.dpi = savedDpi;
        CreateFonts(); ApplyFonts();
        g.layout = savedLayout;
        SetWindowPos(g.hwnd, nullptr, 0, 0, S(kUiWidth), S(kUiHeight), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
    }
    {
        // Right-clicks that used to open nothing. A list view keeps its header's right-click, so the
        // header names itself to the window (the column menu, with Mood); and the big views' strip
        // opens the player menu like their panels do, while the normal window's strip keeps its
        // layout menu.
        static HWND contextFrom = nullptr;
        contextFrom = nullptr;
        HHOOK hook = SetWindowsHookExW(WH_CALLWNDPROC, [](int code, WPARAM wp, LPARAM lp) -> LRESULT {
            const auto* sent = reinterpret_cast<const CWPSTRUCT*>(lp);
            if (code == HC_ACTION && sent->message == WM_CONTEXTMENU && sent->hwnd == g.hwnd) contextFrom = (HWND)sent->wParam;
            return CallNextHookEx(nullptr, code, wp, lp);
        }, nullptr, GetCurrentThreadId());
        SendMessageW(g.header, WM_RBUTTONDOWN, MK_RBUTTON, MAKELPARAM(S(20), S(8)));
        SendMessageW(g.header, WM_RBUTTONUP, 0, MAKELPARAM(S(20), S(8)));
        if (hook) UnhookWindowsHookEx(hook);
        const ViewSize savedSize = g.size;
        testlog::Cases strip;
        for (ViewSize mode : {ViewSize::Normal, ViewSize::Compact, ViewSize::Spotlight, ViewSize::Big}) {
            g.size = mode;
            const bool big = mode != ViewSize::Normal;
            int index = 0;
            for (HWND part : {g.seek, g.volume, g.buttons[1]})  // the test window has no panels
                strip.Add(part && OpensPlayerMenu(part) == big, std::to_wstring((int)mode) + L"/" + std::to_wstring(index++));
            strip.Add(!OpensPlayerMenu(g.list) && !OpensPlayerMenu(g.header), std::to_wstring((int)mode) + L" list");
        }
        g.size = savedSize;
        // The column menu offers Mood and the usual columns, but not the foobar2000 or Apollo themes'
        // own columns outside those themes (this test runs in neither).
        const bool themed = FoobarMode() || ApolloMode();
        strip.Add(themed || (ColumnOffered(kColMood) && ColumnOffered(kColRating) && !ColumnOffered(0) &&
                             !ColumnOffered(kColCover) && !ColumnOffered(kColTitleArtist) &&
                             !ColumnOffered(kColApolloTitle) && !ColumnOffered(kColApolloTag)),
                  L"column menu");
        batch.Next(L"UI: right-clicks on the list header and the big views' strip reach their menus");
        Check(hook && contextFrom == g.header, L"UI: a right-click on the track list header asks for the column menu");
        Check(strip.Passed(), L"UI: the big views' strip opens the player menu and the normal one doesn't%ls",
              strip.failed.c_str());
    }
    {
        // A compact size remembered at the full window's width (an earlier build saved one) is not a
        // compact window: Smaller opens the 440 x 600 default from the full window's corner instead, and
        // Larger gives back the full window.
        batch.Next(L"UI: an oversized remembered compact size gives way to the compact default");
        const ViewSize savedSize = g.size;
        const WINDOWPLACEMENT savedFull = g.fullPlacement;
        SetViewSize(ViewSize::Normal);
        SetWindowPos(g.hwnd, nullptr, 120, 80, S(900), S(600), SWP_NOZORDER | SWP_NOACTIVATE);
        RECT full{}, compact{}, back{};
        GetWindowRect(g.hwnd, &full);
        IniWrite(L"Window", L"CompactPlacement", JoinInts({120, 80, 120 + S(1300), 80 + S(700)}));
        SetViewSize(ViewSize::Compact);
        GetWindowRect(g.hwnd, &compact);
        const bool own = g.size == ViewSize::Compact && compact.left == full.left && compact.top == full.top &&
                         compact.right - compact.left == S(440) && compact.bottom - compact.top == S(600);
        SetViewSize(ViewSize::Normal);
        GetWindowRect(g.hwnd, &back);
        Check(own && EqualRect(&back, &full),
              L"UI: the compact window opens at its default size, not a remembered full-width one, and Larger "
              L"restores the full window");
        g.fullPlacement = savedFull;
        SetViewSize(savedSize);
        SetWindowPos(g.hwnd, nullptr, 0, 0, S(kUiWidth), S(kUiHeight), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
    }
    g.layout = MainLayout{};
    g.layout.player = Dock::Right;
    Layout();
    RECT seekClient{};
    GetClientRect(g.seek, &seekClient);
    batch.Next(L"UI: a vertical waveform seeks from top to bottom and hidden layout parts give their room to the list");
    Check(BarFracAt(g.seek, {seekClient.right / 2, 0}) == 0.0 && BarFracAt(g.seek, {seekClient.right / 2, seekClient.bottom}) == 1.0 &&
              std::fabs(BarFracAt(g.seek, {0, seekClient.bottom / 2}) - 0.5) < 0.02,
          L"UI: a vertical waveform seeks from its top to its bottom");
    g.layout = MainLayout{};
    Layout();
    HWND body = CanvasActive() ? g.libView : g.list;  // whichever view the Library shows
    LONG classicTop = rect(body).top;
    g.layout.tabs = g.layout.search = g.layout.status = g.layout.transport = g.layout.nowPlaying = false;
    Layout();
    Check(!shown(g.tabs) && !shown(g.search) && !shown(g.status) && !shown(g.buttons[1]) && !shown(g.npTitle) &&
              shown(g.buttons[3]) && shown(g.seek) && shown(body) && rect(body).top < classicTop - S(40),
          L"UI: hidden layout parts hide and the list takes their room");
    g.layout = savedLayout;
    Layout();
    std::wstring emptyBad, escapeBad, downBad, selectBad;
    for (LibraryMode mode : {LibraryMode::Tracks, LibraryMode::Albums, LibraryMode::AlbumTracks, LibraryMode::Artists}) {
        auto note = [&](std::wstring& list, bool ok) { if (!ok) list += L" " + std::to_wstring((int)mode); };
        SetLibraryMode(mode);
        SetWindowTextW(g.search, L"no-such-track"); FlushSearch();
        note(emptyBad, g.view.empty() && EmptyViewText().find(L"search") != std::wstring::npos);
        SendMessageW(g.search, WM_KEYDOWN, VK_ESCAPE, 0);
        note(escapeBad, g.filter.empty() && g.view.size() == 3);
        SendMessageW(g.search, WM_KEYDOWN, VK_DOWN, 0);
        note(downBad, !SelectedTracks().empty());
        if (CanvasActive()) {
            OnCommand(IDM_SELECT_ALL, 0, nullptr);
            note(selectBad, SelectedTracks().size() == 3);
        }
    }
    batch.Next(L"UI: search explains no results, Escape restores results, Down selects the view and Select all works");
    Check(emptyBad.empty(), L"UI: search explains no results in every library view%s", emptyBad.c_str());
    Check(escapeBad.empty(), L"UI: Escape restores search results in every library view%s", escapeBad.c_str());
    Check(downBad.empty(), L"UI: Down from search selects the visible view%s", downBad.c_str());
    Check(selectBad.empty(), L"UI: Select all menu works in every canvas view%s", selectBad.c_str());
    // Albums & Tracks: the cover is the grid's 160 a quarter larger and the rows start under a 56 px
    // heading, so clicking the third row's middle picks that one track (the old layout had no row there).
    SetLibraryMode(LibraryMode::AlbumTracks);
    batch.Next(L"UI: Albums & Tracks rows sit beside the larger cover, under the album heading");
    if (CanvasActive() && g.view.size() == 3) {
        LPARAM third = MAKELPARAM(S(18 + 200 + 20 + 60), S(8 + 56 + 2 * 26 + 13));
        SendMessageW(g.libView, WM_LBUTTONDOWN, MK_LBUTTON, third);
        SendMessageW(g.libView, WM_LBUTTONUP, 0, third);
        auto picked = SelectedTracks();
        Check(picked.size() == 1 && picked[0] == Active().items[g.view[2]],
              L"UI: Albums & Tracks rows sit beside the larger cover, under the album heading");
    } else {
        Check(false, L"UI: Albums & Tracks rows sit beside the larger cover, under the album heading");
    }
    // At the compact window's width the full-size cover left the rows no room, so their titles were
    // drawn backwards over it: the cover shrinks instead and the rows start right of it.
    batch.Next(L"UI: a narrow Albums & Tracks view shrinks the cover to keep its rows");
    if (CanvasActive() && g.view.size() == 3) {
        RECT saved = rect(g.libView);
        SetWindowPos(g.libView, nullptr, 0, 0, S(340), saved.bottom - saved.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        LPARAM third = MAKELPARAM(S(150), S(8 + 56 + 2 * 26 + 13));  // left of where the 200 px cover ended
        SendMessageW(g.libView, WM_LBUTTONDOWN, MK_LBUTTON, third);
        SendMessageW(g.libView, WM_LBUTTONUP, 0, third);
        auto picked = SelectedTracks();
        Check(picked.size() == 1 && picked[0] == Active().items[g.view[2]],
              L"UI: a narrow Albums & Tracks view shrinks the cover to keep its rows");
        Layout();
    } else {
        Check(false, L"UI: a narrow Albums & Tracks view shrinks the cover to keep its rows");
    }
    // Album group headers: the three tracks of "Test album" sit under one two-row header.
    ActivatePlaylist(0);
    SetLibraryMode(LibraryMode::Tracks);
    SetWindowTextW(g.search, L""); FlushSearch();
    batch.Next(L"UI: an album's tracks sit under a two-row header that selects them and holds their shared cells");
    Check(g.groups.size() == 1 && RowCount() == 5 && RowTrack(0) == kNone && RowTrack(1) == kNone && RowTrack(2) != kNone &&
              ViewPosOfRow(4) == 2 && RowOfViewPos(0) == 2 && ViewPosNear(0) == 0,
          L"UI: an album's tracks sit under a two-row header");
    SelectGroup(0, false);
    Check(SelectedTracks().size() == 3, L"UI: clicking an album header selects its tracks");
    Check(GroupHidesCell(0, 4, RowTrack(2)) && GroupHidesCell(0, 3, RowTrack(2)) && !GroupHidesCell(0, 2, RowTrack(2)),
          L"UI: grouped tracks leave the album and its artist to the header");
    {
        // The default columns run Title, Artist, Album, Year, Rating: the title's text may use the
        // Artist and Album cells the header holds, but never reaches the rating.
        RECT title{}, album{}, rating{};
        ListView_GetSubItemRect(g.list, 2, SubitemOf(2), LVIR_BOUNDS, &title);
        ListView_GetSubItemRect(g.list, 2, SubitemOf(4), LVIR_BOUNDS, &album);
        ListView_GetSubItemRect(g.list, 2, SubitemOf(kColRating), LVIR_BOUNDS, &rating);
        LONG right = CellTextRight(2, SubitemOf(2), title.right);
        Check(SubitemOf(4) > SubitemOf(2) && right >= album.right && right <= rating.left,
              L"UI: a grouped title runs over the blank Artist and Album cells up to the rating");
    }
    Check(!ui::ShowEmptyStars(0, false, false) && ui::ShowEmptyStars(0, true, false) &&
              ui::ShowEmptyStars(0, false, true) && ui::ShowEmptyStars(20, false, false),
          L"UI: unrated tracks show empty stars only on hover or selection");
    Check(ui::SeekNotchSeconds(19 * 60) == 0 && ui::SeekNotchSeconds(30 * 60 + 16) == 5 * 60 &&
              ui::SeekNotchSeconds(2 * 3600 + 46 * 60) == 30 * 60 && ui::SeekNotchSeconds(6 * 3600 + 3 * 60) == 3600 &&
              ui::SeekNotchSeconds(40 * 3600) == 0,
          L"UI: long tracks notch the seek bar at round times, at most eight");
    Check(ui::PlayedWidth(900, 0, 60) == 0 && ui::PlayedWidth(900, 30, 60) == 450 && ui::PlayedWidth(900, 90, 60) == 900 &&
              ui::PlayedWidth(900, 10, 0) == 0,
          L"UI: the playing row's underline follows the position");
    {
        // At 1.1x tempo a 31:35 mix at 148 BPM is heard as 28:43 at 163; the normal rate and other
        // columns keep the tags' own values, and an untagged BPM stays blank.
        Track mix;
        mix.durationMs = (31 * 60 + 35) * 1000;
        mix.bpm = 148;
        std::wstring duration, bpm, title = L"kept", untagged = L"x";
        Track bare;
        bool heard = HeardCell(ui::kColDuration, mix, 1.1, duration) && duration == L"28:43" &&
                     HeardCell(ui::kColBpm, mix, 1.1, bpm) && bpm == L"163" &&
                     HeardCell(ui::kColBpm, bare, 1.1, untagged) && untagged.empty() &&
                     !HeardCell(ui::kColDuration, mix, 1.0, duration) && !HeardCell(2, mix, 1.1, title) &&
                     title == L"kept" && !HeardCell(ui::kColBpm, mix, 0.0, bpm) &&
                     HeardSeconds(3600, 2.0) == 1800 && HeardSeconds(3600, 1.0) == 3600;
        Check(heard, L"UI: Duration, BPM and totals say what is heard at the playback rate");
    }
    Check(ui::FitTabPadding(400, 5, 600, 18, 8) == 18 && ui::FitTabPadding(400, 5, 500, 18, 8) == 8 &&
              ui::FitTabPadding(400, 5, 450, 18, 8) == -1,
          L"UI: crowded tabs tighten, then share the strip, before scrolling");
    g.groupAlbums = false;
    RebuildView();
    batch.Next(L"UI: grouping by album can be turned off");
    Check(g.groups.empty() && RowCount() == 3 && RowTrack(0) != kNone, L"UI: grouping by album can be turned off");
    g.groupAlbums = true;
    RebuildView();
    {
        // Owner-drawn tracks fill every column. Exercise the actual native mouse messages,
        // then cover a notification that omits its item index explicitly.
        g.playlists[1].items = {0, 1, 2};
        ActivatePlaylist(1);
        auto doubleClick = [](POINT pt) {
            LPARAM at = MAKELPARAM(pt.x, pt.y);
            PostMessageW(g.list, WM_LBUTTONDOWN, MK_LBUTTON, at);
            PostMessageW(g.list, WM_LBUTTONUP, 0, at);
            PostMessageW(g.list, WM_LBUTTONDBLCLK, MK_LBUTTON, at);
            PostMessageW(g.list, WM_LBUTTONUP, 0, at);
            Pump(10);
        };
        testlog::Cases columns, blank;
        for (bool grouped : {false, true}) {
            g.groupAlbums = grouped;
            RebuildView();
            for (int sub = 0; sub < (int)g.columns.size(); ++sub) {
                int pos = sub % 3, row = RowOfViewPos(pos);
                RECT cell{};
                ListView_GetSubItemRect(g.list, row, sub, LVIR_BOUNDS, &cell);
                POINT pt{cell.left + S(4), (cell.top + cell.bottom) / 2};
                auto before = g.nextToken;
                doubleClick(pt);
                auto found = g.tokens.find(g.decodingToken);
                columns.Add(g.nextToken == before + 1 && found != g.tokens.end() && found->second.pl == 1 &&
                                found->second.index == (uint32_t)pos,
                            L"column " + std::to_wstring(sub) + (grouped ? L" grouped" : L""));
            }
            RECT last{}, client{};
            ListView_GetItemRect(g.list, RowCount() - 1, &last, LVIR_BOUNDS);
            GetClientRect(g.list, &client);
            auto before = g.nextToken;
            doubleClick({S(100), last.bottom + S(10)});
            doubleClick({client.right - 1, (last.top + last.bottom) / 2});
            blank.Add(g.nextToken == before, grouped ? L"grouped" : L"ungrouped");
        }
        batch.Next(L"UI: double-click starts the clicked track once in every column, grouped or not, and "
                   L"blank space does nothing");
        Check(columns.Passed(), L"UI: double-click starts the clicked track once in every column, grouped or not%ls",
              columns.failed.c_str());
        Check(blank.Passed(), L"UI: double-clicking blank list space leaves playback alone%ls", blank.failed.c_str());
        RECT title{};
        ListView_GetSubItemRect(g.list, RowOfViewPos(2), SubitemOf(2), LVIR_BOUNDS, &title);
        NMITEMACTIVATE activation{};
        activation.hdr.hwndFrom = g.list;
        activation.hdr.code = NM_DBLCLK;
        activation.iItem = -1;
        activation.ptAction = {title.left + S(4), (title.top + title.bottom) / 2};
        auto before = g.nextToken;
        OnListNotify(&activation.hdr);
        batch.Next(L"UI: a double-click without a native item index still plays the clicked track");
        Check(g.nextToken == before + 1 && TokenIndex(g.decodingToken, 1) == 2,
              L"UI: a double-click without a native item index still plays the clicked track");
        g.engine.Stop();
        g.tokens.clear();
        g.decodingToken = 0;
        g.playlists[1].items.clear();
        ActivatePlaylist(0);
    }
    // The reference layout the Linux port is held to (ui_spec.h): the real controls at 96 DPI.
    {
        g.dpi = 96;
        CreateFonts();
        ApplyFonts();
        g.layout = MainLayout{};
        g.browserVisible = false;
        RECT frame{0, 0, ui::kLayoutWidth, ui::kLayoutHeight};
        AdjustWindowRectExForDpi(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0, 96);
        SetWindowPos(g.hwnd, nullptr, 0, 0, frame.right - frame.left, frame.bottom - frame.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
        auto spec = [&](const char* name) -> RECT {
            static const struct { const char* name; HWND* h; } controls[] = {
                {"previous", &g.buttons[0]}, {"play", &g.buttons[1]}, {"next", &g.buttons[2]}, {"shuffle", &g.buttons[3]},
                {"repeat", &g.buttons[4]}, {"resume", &g.buttons[5]}, {"elapsed", &g.elapsed}, {"seek", &g.seek},
                {"remaining", &g.time}, {"speaker", &g.speaker}, {"volume", &g.volume}, {"title", &g.npTitle},
                {"subtitle", &g.npSub}, {"tabs", &g.tabs}, {"search", &g.search}, {"list", &g.list}, {"status", &g.status}};
            if (!strcmp(name, "cover")) return g.artRect;
            if (!strcmp(name, "radio_tab")) return rect(RadioTab());
            for (const auto& c : controls)
                if (!strcmp(name, c.name)) return rect(*c.h);
            return {};
        };
        std::wstring off;
        RECT client{};
        GetClientRect(g.hwnd, &client);
        if (client.right != ui::kLayoutWidth || client.bottom != ui::kLayoutHeight) off += L" client";
        for (const ui::LayoutRect& want : ui::kLayout) {
            RECT r = spec(want.name);
            if (r.left != want.x || r.top != want.y || r.right - r.left != want.w || r.bottom - r.top != want.h) {
                wchar_t buf[96];
                swprintf_s(buf, L" %hs=%d,%d,%d,%d", want.name, (int)r.left, (int)r.top, (int)(r.right - r.left), (int)(r.bottom - r.top));
                off += buf;
            }
        }
        batch.Next(L"UI: the window matches the layout the Linux port follows (ui_spec.h kLayout)");
        Check(off.empty(), L"UI: the window matches the layout the Linux port follows (ui_spec.h kLayout)%s", off.c_str());
        g.layout = savedLayout;
        Layout();
    }
    {
        ThemeMode original = g.themeMode;
        testlog::Cases layouts;
        for (ThemeMode theme : {ThemeMode::Foobar, ThemeMode::Apollo}) {
            if (!ThemeAvailable(theme)) continue;
            OnCommand(IDM_THEME_BASE + (int)theme, 0, nullptr);
            ActivateRadio();
            bool alternateRadioVisible = RadioActive() && (GetWindowLongPtrW(RadioView(), GWL_STYLE) & WS_VISIBLE) &&
                                         (GetWindowLongPtrW(RadioTab(), GWL_STYLE) & WS_VISIBLE);
            ActivatePlaylist(0);
            layouts.Add(alternateRadioVisible && (theme == ThemeMode::Foobar ? FoobarMode() : ApolloMode()),
                        theme == ThemeMode::Foobar ? L"foobar2000" : L"Apollo");
        }
        OnCommand(IDM_THEME_BASE + (int)original, 0, nullptr);
        batch.Next(L"UI: Radio opens from either alternate theme and restores its layout");
        Check(layouts.Passed(), L"UI: Radio opens from either alternate theme and restores its layout%ls",
              layouts.failed.c_str());
    }
    batch.End();
    TestFolderBrowser();
    TestFoobarControls();
    batch.Next(L"UI: the search box names the Library or the playlist it searches");
    wchar_t cue[128] = {};
    SendMessageW(g.search, EM_GETCUEBANNER, (WPARAM)cue, (LPARAM)std::size(cue));
    testlog::Cases cueNames;
    cueNames.Add(wcscmp(cue, L"Search library (Ctrl+F)") == 0, L"the Library");
    ActivatePlaylist(2);
    SendMessageW(g.search, EM_GETCUEBANNER, (WPARAM)cue, (LPARAM)std::size(cue));
    cueNames.Add(wcscmp(cue, L"Search Test auto-playlist (Ctrl+F)") == 0, L"the auto-playlist");
    Check(cueNames.Passed(), L"UI: the search box names the Library or the playlist it searches%ls",
          cueNames.failed.c_str());
    g.timeRemaining = true;
    ShowPlaybackTime(72, 199);
    wchar_t left[32] = {}, right[32] = {};
    GetWindowTextW(g.elapsed, left, 32);
    GetWindowTextW(g.time, right, 32);
    batch.Next(L"UI: the seek bar shows elapsed and remaining time and clicking the time shows the length");
    Check(wcscmp(left, L"1:12") == 0 && wcscmp(right, L"-2:07") == 0, L"UI: the seek bar shows elapsed and remaining time");
    OnCommand(IDC_TIME, STN_CLICKED, g.time);
    ShowPlaybackTime(72, 199);
    GetWindowTextW(g.time, right, 32);
    Check(!g.timeRemaining && wcscmp(right, L"3:19") == 0, L"UI: clicking the time shows the length instead");
    g.timeRemaining = true;
    {
        // The cover picker's footer: save choices sized to their labels and painted in the palette
        // (Explorer's dark radio theme otherwise draws them black), above the dialog's buttons.
        batch.Next(L"UI: the cover picker's save choices fit their labels, read in the palette and sit above its buttons");
        auto& covers = online::Config().covers;
        bool savedCovers[std::size(covers)];
        std::copy(std::begin(covers), std::end(covers), savedCovers);
        std::fill(std::begin(covers), std::end(covers), false);  // no search, so nothing goes online
        std::vector<TrackId> all;
        for (TrackId id = 0; id < (TrackId)g_lib.Count(); ++id) all.push_back(id);
        ShowCoverSearch(all);
        HWND picker = nullptr;
        for (HWND w = nullptr; (w = FindWindowExW(nullptr, w, L"CordinalCoverSearch", nullptr));) {
            DWORD process = 0;
            GetWindowThreadProcessId(w, &process);
            if (process == GetCurrentProcessId()) picker = w;
        }
        if (Ready(picker != nullptr, L"the cover picker opens for a library album")) {
            std::vector<HWND> radios, pushes;
            for (HWND c = GetWindow(picker, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
                wchar_t cls[32] = {};
                GetClassNameW(c, cls, 32);
                if (_wcsicmp(cls, WC_BUTTONW)) continue;
                LONG type = GetWindowLongW(c, GWL_STYLE) & BS_TYPEMASK;
                (type == BS_AUTORADIOBUTTON ? radios : pushes).push_back(c);
            }
            auto inPicker = [&](HWND c) {
                RECT r{};
                GetWindowRect(c, &r);
                MapWindowPoints(nullptr, picker, reinterpret_cast<POINT*>(&r), 2);
                return r;
            };
            RECT client;
            GetClientRect(picker, &client);
            HDC dc = GetDC(picker);
            HGDIOBJ old = SelectObject(dc, g.font);
            bool fit = radios.size() == 3, painted = fit;
            int previousRight = 0, radiosBottom = 0, buttonsTop = INT_MAX;
            for (HWND radio : radios) {
                RECT r = inPicker(radio), text{};
                std::wstring label = WindowText(radio);
                DrawTextW(dc, label.c_str(), -1, &text, DT_CALCRECT | DT_SINGLELINE);
                fit = fit && r.right - r.left >= S(16) + text.right && r.left >= previousRight && r.right <= client.right;
                previousRight = r.right;
                radiosBottom = std::max(radiosBottom, (int)r.bottom);
                NMCUSTOMDRAW draw{};
                draw.hdr = {radio, (UINT_PTR)GetDlgCtrlID(radio), (UINT)NM_CUSTOMDRAW};
                draw.dwDrawStage = CDDS_PREPAINT;
                painted = painted && SendMessageW(picker, WM_NOTIFY, draw.hdr.idFrom, (LPARAM)&draw) == CDRF_NOTIFYPOSTPAINT;
            }
            SelectObject(dc, old);
            ReleaseDC(picker, dc);
            bool useIsDefault = false;
            for (HWND button : pushes) {
                if (WindowText(button) != L"Search" && WindowText(button) != L"&Search")
                    buttonsTop = std::min(buttonsTop, (int)inPicker(button).top);
                if (WindowText(button) == L"&Use cover")
                    useIsDefault = (GetWindowLongW(button, GWL_STYLE) & BS_TYPEMASK) == BS_DEFPUSHBUTTON;
            }
            Check(fit, L"UI: each cover save choice is at least as wide as its label and none overlap");
            Check(painted, L"UI: the cover save choices repaint their labels in the palette's text color");
            Check(radiosBottom <= buttonsTop && useIsDefault,
                  L"UI: the save choices sit above the picker's buttons and Use cover is the default button");
            DestroyWindow(picker);
        }
        std::copy(std::begin(savedCovers), std::end(savedCovers), std::begin(covers));
    }
    ActivatePlaylist(1);
    HMENU menu = BuildMenu();
    batch.Next(L"UI: Library offers classic iPod sync");
    Check(GetMenuState(GetSubMenu(menu, 4), IDM_IPOD_SYNC, MF_BYCOMMAND) != (UINT)-1,
          L"UI: Library has the Sync to iPod command");
    WndProc(g.hwnd, WM_INITMENUPOPUP, (WPARAM)GetSubMenu(menu, 1), 0);
    batch.Next(L"UI: playlist commands that do not apply are disabled");
    Check((GetMenuState(GetSubMenu(menu, 1), IDM_CLEAR, MF_BYCOMMAND) & MF_GRAYED) != 0,
          L"UI: empty playlist cannot be cleared");
    ActivatePlaylist(0);
    WndProc(g.hwnd, WM_INITMENUPOPUP, (WPARAM)GetSubMenu(menu, 3), 0);
    Check((GetMenuState(GetSubMenu(menu, 3), IDM_RENAME_PL, MF_BYCOMMAND) & MF_GRAYED) != 0 &&
              (GetMenuState(GetSubMenu(menu, 3), IDM_REMOVE_PL, MF_BYCOMMAND) & MF_GRAYED) != 0,
          L"UI: Library cannot be renamed or removed");
    const auto watchedBeforeSteam = g.libraryFolders;
    AddSteamSoundtracks();
    HMENU libraryMenu = GetSubMenu(menu, 4);
    UpdateMainMenu(libraryMenu);
    batch.Next(L"UI: Steam discovery runs off the UI thread, offers cancellation and restores the menu");
    Check(SteamDiscoveryRunning() && (GetMenuState(libraryMenu, IDM_STEAM, MF_BYCOMMAND) & MF_GRAYED) &&
              GetMenuState(libraryMenu, IDM_CANCEL_STEAM, MF_BYCOMMAND) != (UINT)-1,
          L"UI: Steam discovery runs off the UI thread and offers cancellation in the menu");
    CancelSteamDiscovery();
    MSG steamResult;
    bool completedSteam = false;
    const ULONGLONG steamStarted = GetTickCount64();
    while (GetTickCount64() - steamStarted < 5000) {
        if (PeekMessageW(&steamResult, g.hwnd, WM_APP_STEAM, WM_APP_STEAM, PM_REMOVE)) {
            DispatchMessageW(&steamResult);
            completedSteam = true;
            break;
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_POSTMESSAGE);
    }
    UpdateMainMenu(libraryMenu);
    Check(completedSteam && !SteamDiscoveryRunning() && g.libraryFolders == watchedBeforeSteam &&
              GetMenuState(libraryMenu, IDM_CANCEL_STEAM, MF_BYCOMMAND) == (UINT)-1 &&
              !(GetMenuState(libraryMenu, IDM_STEAM, MF_BYCOMMAND) & MF_GRAYED),
          L"UI: cancelling Steam discovery leaves watched folders unchanged and restores the menu");
    StopSteamDiscovery();
    DestroyMenu(menu);
    ActivatePlaylist(1);
    NMLVEMPTYMARKUP empty{};
    empty.hdr.hwndFrom = g.list;
    empty.hdr.code = LVN_GETEMPTYMARKUP;
    batch.Next(L"UI: empty playlists explain how to add music and empty auto-playlists explain their rules");
    Check(OnListNotify(&empty.hdr) == TRUE && std::wstring(empty.szMarkup).find(L"Ctrl+O") != std::wstring::npos,
          L"UI: empty playlist explains how to add music");
    ActivatePlaylist(2);
    Check(EmptyViewText().find(L"rules") != std::wstring::npos, L"UI: empty auto-playlist explains its rules");
    {
        // A view sorts with the query that is in the search box now. It used to restore the position
        // last captured for it, so a query already cleared came back on a column header click.
        SetWindowTextW(g.search, L"mid"); FlushSearch();
        CapturePlaylistPosition();  // captures the query the way switching tabs or saving does
        SetWindowTextW(g.search, L""); FlushSearch();
        SortByColumn(SubitemOf(2));  // clicking Title in an auto-playlist such as Recently added
        batch.Next(L"UI: sorting a view keeps the search box's query, not one saved earlier");
        Check(WindowText(g.search).empty() && g.filter.empty() && Active().position.search.empty(),
              L"UI: sorting a view keeps the search box's query rather than the last one saved for it");
    }

    // Keep a long, sorted playlist in place while visiting each Library view.
    g.playlists[1].items = {0, 1, 2};
    for (int i = 3; i < 70; ++i) {
        TrackId id = g_lib.Add(L"C:\\ui-test\\track" + std::to_wstring(i) + L".flac");
        TagData tags;
        tags.title = "Track " + std::to_string(i);
        g_lib.ApplyTags(id, tags);
        g.playlists[1].items.push_back(id);
    }
    g.playlists[1].presort.clear();
    const auto unsortedOrder = g.playlists[1].items;
    ActivatePlaylist(1);
    SetWindowTextW(g.search, L"Track"); FlushSearch();
    SortByColumn(SubitemOf(2));
    SortByColumn(SubitemOf(2));
    {
        // A third click on the same header drops the sort and brings back the order from before it.
        SortByColumn(SubitemOf(2));
        batch.Next(L"UI: a third click on a sorted header restores the original order");
        Check(g.sortColumn == -1 && Active().sortColumn == -1 && Active().items == unsortedOrder,
              L"UI: a third click on the sorted header clears the sort and restores the original order");
        SortByColumn(SubitemOf(2));
        SortByColumn(SubitemOf(2));
    }
    {
        // The column model both platforms share (column_layout.h).
        testlog::Cases model;
        const std::vector<ui::ColumnSpec> cols{{0, 28, 20, true}, {1, 44, 32, false}, {2, 280, 80, false},
                                               {3, 190, 40, false}, {6, 70, 40, false}};
        const auto fit = ui::FitColumns(cols);
        model.Add(fit == std::vector<int>{28, 44, 280, 190, 70}, L"each column keeps its own width");
        const auto apollo = ui::FitColumns(cols, 1000, 2);
        model.Add(apollo[2] == 1000 - 28 - 44 - 190 - 70 && ui::FitColumns(cols, 100, 2)[2] == 80,
                  L"Apollo's stretch column takes the room left, never below its least");
        const auto squeezed = ui::DragDivider(cols, fit, 1, -500);
        model.Add(squeezed[1] == 32 && squeezed[2] == fit[2], L"a divider stops at the least width and moves only its column");
        model.Add(ui::DragDivider(cols, fit, 4, 30)[4] == 100, L"the last column widens into the empty room");
        const auto traded = ui::DragDivider(cols, apollo, 2, 20, 2);
        model.Add(traded[3] == 170 && traded[2] == apollo[2] + 20, L"Apollo's Title divider trades with the next column");
        model.Add(!ui::DividerMoves(cols, 0) && ui::DividerMoves(cols, 4) && ui::DividerMoves(cols, 1),
                  L"the gutter's divider stays put");
        model.Add(ui::DividerAt(cols, fit, 28 + 44 + 3, 5) == 1 && ui::DividerAt(cols, fit, 28 + 2, 5) == -1,
                  L"divider hit testing");
        model.Add(ui::DropSlot(cols, fit, 0) == 1 && ui::DropSlot(cols, fit, 5000) == 5, L"nothing lands before the gutter");
        std::vector<int> types{0, 1, 2, 3, 6};
        model.Add(ui::MoveColumn(types, 4, 2) && types == std::vector<int>{0, 1, 6, 2, 3}, L"a column lands before its slot");
        model.Add(!ui::MoveColumn(types, 2, 3) && !ui::MoveColumn(types, 2, 2), L"dropping a column beside itself moves nothing");
        ui::SortState sort{-1, true};
        sort = ui::NextSort(sort, 3, true);
        const bool first = sort.column == 3 && sort.ascending;
        sort = ui::NextSort(sort, 3, true);
        const bool second = sort.column == 3 && !sort.ascending;
        model.Add(first && second && ui::NextSort(sort, 3, true).column == -1 && ui::NextSort(sort, 3, false).ascending &&
                      ui::NextSort(sort, 1, true).column == 3,
                  L"header clicks sort up, down, then not at all; # never sorts");
        batch.Next(L"UI: the shared column model sizes, resizes, moves and sorts");
        Check(model.Passed(), L"UI: the shared column model sizes, resizes, moves and sorts%ls", model.failed.c_str());
    }
    {
        // The header's own mouse (app_columns.cpp), through its real messages: the columns keep their own
        // widths with the room past them empty, a divider dragged far left stops at the least width and
        // moves only its column, a dragged column lands where it is dropped, and the playing gutter stays first.
        const auto savedColumns = g.columns;
        ResetColumns();
        auto send = [](UINT msg, int x) {
            SendMessageW(g.header, msg, msg == WM_LBUTTONUP ? 0 : MK_LBUTTON, MAKELPARAM(x, S(8)));
        };
        auto drag = [&](int from, int to) {
            send(WM_LBUTTONDOWN, from);
            send(WM_MOUSEMOVE, (from + to) / 2);
            send(WM_MOUSEMOVE, to);
            send(WM_LBUTTONUP, to);
        };
        auto item = [](int i) {
            RECT r{};
            Header_GetItemRect(g.header, i, &r);
            return r;
        };
        auto total = [] {
            int sum = 0;
            for (int i = 0; i < (int)g.columns.size(); ++i) sum += ListView_GetColumnWidth(g.list, i);
            return sum;
        };
        const int before = total();
        const bool own = ListView_GetColumnWidth(g.list, SubitemOf(2)) == S(ColumnAt(2).width);
        const int number = SubitemOf(1), title = ListView_GetColumnWidth(g.list, SubitemOf(2));
        drag(item(number).right, item(number).left - S(200));
        const int squeezed = ListView_GetColumnWidth(g.list, number);
        const bool least = std::abs(squeezed - S(ui::ColumnLeast(1))) <= 1 &&
                           ListView_GetColumnWidth(g.list, SubitemOf(2)) == title && total() == before - (S(44) - squeezed);
        const int year = SubitemOf(5), artist = SubitemOf(3);
        drag((item(year).left + item(year).right) / 2, item(artist).left + S(2));
        const bool moved = SubitemOf(5) == artist && SubitemOf(3) == artist + 1;
        drag(item(0).left + S(4), item(3).right);
        const bool gutter = g.columns[0] == 0;
        batch.Next(L"UI: the track list header resizes and moves columns by its own rules");
        Check(own && least && moved && gutter,
              L"UI: the header's columns keep their own widths (%d), a squeezed # keeps its least width (%d), a "
              L"dropped column lands there (%d), the playing gutter stays first (%d)",
              own, least, moved, gutter);
        ShowColumns(savedColumns);
    }
    {
        // # shows each track's number, which only means something inside its album: clicking it
        // must not sort the whole list by it and scatter the albums.
        const int sort = g.sortColumn;
        const bool ascending = g.sortAscending;
        const auto items = Active().items;
        const auto view = g.view;
        SortByColumn(SubitemOf(1));
        batch.Next(L"UI: the # header does not sort the list");
        Check(SubitemOf(1) >= 0 && g.sortColumn == sort && g.sortAscending == ascending &&
                  Active().items == items && g.view == view,
              L"UI: clicking the # header leaves the list's sort and order alone");
    }
    ListView_SetItemState(g.list, 35, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(g.list, 35, FALSE);
    const auto remembered = SelectedTracks();
    int top = ListView_GetTopIndex(g.list);
    batch.Next(L"UI: visiting every library view keeps a playlist's search, selection, scroll and sort");
    Check(top > 0, L"UI: navigation test starts with a scrolled playlist");
    std::wstring restoreBad, rememberBad, refreshBad;
    for (LibraryMode mode : {LibraryMode::Tracks, LibraryMode::Albums, LibraryMode::AlbumTracks, LibraryMode::Artists}) {
        auto note = [&](std::wstring& list, bool ok) { if (!ok) list += L" " + std::to_wstring((int)mode); };
        ActivatePlaylist(0);
        SetLibraryMode(mode);
        SetWindowTextW(g.search, L"Track"); FlushSearch();
        SendMessageW(g.search, WM_KEYDOWN, VK_DOWN, 0);
        const auto librarySelection = SelectedTracks();
        ActivatePlaylist(1);
        note(restoreBad, g.filter == L"Track" && SelectedTracks() == remembered && ListView_GetTopIndex(g.list) == top &&
                         g.sortColumn == 2 && !g.sortAscending);
        ActivatePlaylist(0);
        note(rememberBad, g.filter == L"Track" && SelectedTracks() == librarySelection);
        g_lib.Touch();
        RefreshViewKeepingSelection();
        note(refreshBad, SelectedTracks() == librarySelection);
        ActivatePlaylist(1);
    }
    Check(restoreBad.empty(), L"UI: switching from every library view restores playlist search, selection, scroll and sort%s",
          restoreBad.c_str());
    Check(rememberBad.empty(), L"UI: every library view remembers its search and selection%s", rememberBad.c_str());
    Check(refreshBad.empty(), L"UI: refresh keeps selection in every library view%s", refreshBad.c_str());
    SetWindowTextW(g.search, L"no-such-track"); FlushSearch();
    ActivatePlaylist(0);
    g.playing = remembered.front();
    g.playingPl = 1;
    OnCommand(IDC_NP_TITLE, STN_CLICKED, g.npTitle);
    batch.Next(L"UI: clicking the playing title reveals and selects the track, and while stopped it does nothing");
    Check(g.active == 1 && g.filter.empty() && SelectedTracks() == remembered,
          L"UI: clicking the playing title reveals and selects the track through a search filter");
    g.playing = kNone;
    g.playingPl = -1;
    g.resumeTrack = kNone;
    OnCommand(IDC_NP_TITLE, STN_CLICKED, g.npTitle);
    Check(g.active == 1 && SelectedTracks() == remembered, L"UI: clicking the title while stopped does nothing");
    g.resumeTrack = remembered.front();
    g.resumePos = 72;
    UpdateNowPlaying();
    wchar_t npText[256] = {};
    GetWindowTextW(g.npTitle, npText, (int)std::size(npText));
    testlog::Cases stoppedStrip;
    stoppedStrip.Add(std::wstring(npText).rfind(L"Continue: ", 0) == 0, L"it offers to continue the track");
    GetWindowTextW(g.npSub, npText, (int)std::size(npText));
    stoppedStrip.Add(std::wstring(npText).find(L"1:12") != std::wstring::npos,
                     L"it shows where playback stopped");
    batch.Next(L"UI: the stopped strip offers to continue the stopped track and shows where it stopped");
    Check(stoppedStrip.Passed(), L"UI: the stopped strip offers to continue the stopped track and shows where "
          L"it stopped%ls", stoppedStrip.failed.c_str());
    g.resumeTrack = kNone;
    UpdateNowPlaying();

    // The waveform's section menu (app_region.cpp): a section is marked where the cursor is, kept
    // in settings.ini against the file's audio, and loaded back for the track that plays next.
    g.playing = remembered.front();
    g.playingPl = 1;
    g_lib[g.playing].durationMs = 180000;  // 3:00, so the section below has a track to sit in
    RegionTrackChanged();
    double secDur = CurrentDuration();
    bool noSectionYet = !CurrentRegion().on && CurrentRegion().end == 0;
    bool started = CommandRegion(IDM_REGION_START, 12.5);
    bool ended = CommandRegion(IDM_REGION_END, 40.0);
    bool armed = CommandRegion(IDM_REGION_LOOP, 20.0);
    Region loaded = CurrentRegion();
    batch.Next(L"UI: a section marked on the waveform is 12.5 s to 40 s, loops and keeps the full timeline");
    Check(noSectionYet && started && ended && armed && loaded.on && loaded.start == 12.5 && loaded.end == 40.0,
          L"UI: a section marked on the waveform is 12.5 s to 40 s and loops");
    PlayItem marked = MakeItem(123, g.playing);
    TrackId upcoming = g_lib.Add(scratch + L"\\section-upcoming.wav");
    MakeItem(124, upcoming);
    Check(marked.start == 0 && marked.end == 0 && marked.loopStart == 12.5 && marked.loopEnd == 40 &&
              marked.duration == 180 && CurrentRegion().on && CurrentRegion().start == 12.5,
          L"UI: playback sections keep the full timeline and prefetch leaves the playing section intact");
    // The section was stored when it was marked: dropping the copy and asking for the track again
    // reads it back from settings.ini, which is what a restart does.
    g.playing = kNone;
    RegionTrackChanged();
    g.playing = remembered.front();
    RegionTrackChanged();
    Region again = CurrentRegion();
    // A message that isn't a right-click on the seek bar is passed on, never swallowed.
    HWND realSeek = g.seek;
    g.seek = nullptr;
    bool notSwallowed = !RegionMouse(MSG{});
    g.seek = realSeek;
    batch.Next(L"UI: the section returns with its track, marks where the seek bar would seek, and clearing drops it");
    Check(notSwallowed && again.on && again.start == 12.5 && again.end == 40.0,
          L"UI: the section is kept for the file and comes back with the track");
    // The section follows the same x -> time mapping the seek bar seeks by, so it lands under the
    // cursor rather than beside it.
    RECT secThumb{}, secChannel{};
    SendMessageW(g.seek, TBM_GETTHUMBRECT, 0, (LPARAM)&secThumb);
    SendMessageW(g.seek, TBM_GETCHANNELRECT, 0, (LPARAM)&secChannel);
    int secMid = secChannel.left + (secThumb.right - secThumb.left) / 2 +
                 (int)((secChannel.right - secChannel.left - (secThumb.right - secThumb.left)) / 2);
    Check(std::fabs(RegionTimeAt(g.seek, {secMid, 0}) - secDur / 2) < secDur * 0.02,
          L"UI: a mark lands where the seek bar would have seeked to");
    bool cleared = CommandRegion(IDM_REGION_CLEAR, 0) && !CurrentRegion().on && CurrentRegion().end == 0;
    g.playing = kNone;
    RegionTrackChanged();
    g.playing = remembered.front();
    RegionTrackChanged();
    Check(cleared && !CurrentRegion().on && CurrentRegion().end == 0,
          L"UI: clearing a section drops it from the file's settings");
    CommandRegion(IDM_REGION_START, 8);
    CommandRegion(IDM_REGION_END, 18);
    g.playing = kNone;
    RegionTrackChanged();
    g.playing = remembered.front();
    RegionTrackChanged();
    batch.Next(L"UI: unfinished section edits survive a track change and a CUE sub-track keeps its own range");
    Check(!CurrentRegion().on && CurrentRegion().start == 8 && CurrentRegion().end == 18,
          L"UI: unfinished section edits survive a track change without enabling the loop");
    CommandRegion(IDM_REGION_CLEAR, 0);
    std::wstring cueAudio = scratch + L"\\section-cue.wav", cueSheet = scratch + L"\\section-cue.cue";
    bool cueReady = WriteTestBytes(cueAudio, MakeSineWav(4, 1)) &&
        WriteTestBytes(cueSheet, "FILE \"section-cue.wav\" WAVE\nTRACK 01 AUDIO\nINDEX 01 00:00:00\nTRACK 02 AUDIO\nINDEX 01 00:02:00\nTRACK 03 AUDIO\nINDEX 01 00:03:00\n");
    TrackId cueId = g_lib.Add(SubtrackPath(cueAudio, 2));
    g_lib[cueId].durationMs = 1000;
    g.playing = cueId;
    RegionTrackChanged();
    CommandRegion(IDM_REGION_START, 0.25);
    CommandRegion(IDM_REGION_END, 0.75);
    CommandRegion(IDM_REGION_LOOP, 0.25);
    PlayItem cueItem = MakeItem(125, cueId);
    Check(cueReady && cueItem.start == 2 && cueItem.end == 3 && cueItem.loopStart == 0.25 && cueItem.loopEnd == 0.75,
          L"UI: section marks of a later CUE track stay relative to its range (ready=%d range=%.2f-%.2f section=%.2f-%.2f)",
          cueReady, cueItem.start, cueItem.end, cueItem.loopStart, cueItem.loopEnd);
    CommandRegion(IDM_REGION_CLEAR, 0);
    DeleteFileW(cueAudio.c_str());
    DeleteFileW(cueSheet.c_str());
    g.playing = kNone;
    g.playingPl = -1;

    SetVolumePercent(37);
    OnCommand(IDC_SPEAKER, STN_CLICKED, g.speaker);
    batch.Next(L"UI: the speaker mutes and restores the volume, and the slider sets the next unmute level");
    Check(g.volumePercent == 0 && SendMessageW(g.volume, TBM_GETPOS, 0, 0) == 0,
          L"UI: clicking the speaker mutes and updates the volume slider");
    OnCommand(IDM_MUTE, 0, nullptr);
    Check(g.volumePercent == 37 && SendMessageW(g.volume, TBM_GETPOS, 0, 0) == 37,
          L"UI: unmute restores the previous volume");
    SetVolumePercent(0);
    SetVolumePercent(19);
    ToggleMute();
    ToggleMute();
    Check(g.volumePercent == 19, L"UI: moving the slider establishes the next unmute volume");
    g.playlists[1].items = {0, 0, 1};
    ClearSelection();
    RebuildView();
    int second = RowOfViewPos(1);
    ListView_SetItemState(g.list, second, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ActivatePlaylist(0);
    ActivatePlaylist(1);
    batch.Next(L"UI: returning to a playlist keeps the selected duplicate, and a Now Playing album groups under a header");
    Check(ListView_GetSelectedCount(g.list) == 1 && ListView_GetNextItem(g.list, -1, LVNI_FOCUSED) == second,
          L"UI: returning to a playlist keeps the selected occurrence of a duplicate track");
    // Album headers, and so their covers, are not a Library-only thing: any list groups an album
    // whose tracks sit together, Now Playing and auto-playlists (Recently added) included.
    {
        const int np = (int)g.playlists.size();
        InsertPlaylist(np, Playlist{L"Now Playing", {}, PlaylistKind::NowPlaying});
        g.playlists[np].items = {0, 1, 2};
        ActivatePlaylist(np);
        Check(g.groups.size() == 1 && RowCount() == 5 && RowTrack(2) != kNone && ViewPosOfRow(4) == 2,
              L"UI: a Now Playing album groups under a header like the Library (groups=%zu rows=%d)", g.groups.size(),
              RowCount());
        // Playing from a view replaces Now Playing's rows with that view's, in that view's order: its
        // header arrow must follow the source, not keep the sort an earlier snapshot was given.
        g.playlists[np].sortColumn = 2;
        g.playlists[np].sortAscending = true;
        const int oldLibrarySort = g.playlists[0].sortColumn;
        const bool oldLibraryAscending = g.playlists[0].sortAscending;
        g.playlists[0].sortColumn = 6;
        g.playlists[0].sortAscending = false;
        int snapshotIndex = 0;
        int snapshotNp = SnapshotToNowPlaying(0, snapshotIndex);
        Check(snapshotNp == np && g.playlists[np].sortColumn == 6 && !g.playlists[np].sortAscending &&
                  g.sortColumn == 6 && !g.sortAscending,
              L"UI: a Now Playing snapshot takes the sort of the view it came from (column %d)",
              g.playlists[np].sortColumn);
        g.playlists[0].sortColumn = -1;
        snapshotIndex = 0;
        SnapshotToNowPlaying(0, snapshotIndex);
        Check(g.playlists[np].sortColumn == -1 && g.sortColumn == -1,
              L"UI: a snapshot of an unsorted view leaves Now Playing without a sort arrow");
        g.playlists[0].sortColumn = oldLibrarySort;
        g.playlists[0].sortAscending = oldLibraryAscending;
        // A search that hides the chosen track (Previous from a stopped player starts at the first) snapshots
        // the whole view, so the index still names a real Now Playing entry instead of one past the end.
        {
            const int savedActive = g.active;
            const std::wstring savedFilter = g.filter;
            const std::vector<uint32_t> savedView = g.view;
            g.active = 0;
            g.filter = L"hidden";
            g.view = {1};
            int hiddenIndex = 0;
            const TrackId hidden = g.playlists[0].items[0];
            const int hiddenNp = SnapshotToNowPlaying(0, hiddenIndex);
            const auto& hiddenItems = g.playlists[hiddenNp].items;
            Check(hiddenIndex >= 0 && hiddenIndex < (int)hiddenItems.size() && hiddenItems[hiddenIndex] == hidden,
                  L"UI: playing a track the search hides snapshots a valid Now Playing index (%d of %zu)", hiddenIndex,
                  hiddenItems.size());
            g.active = savedActive;
            g.filter = savedFilter;
            g.view = savedView;
        }
        // Taste picks remember the playlist they came from; adding or closing a tab in front of it must not
        // point them at another list.
        {
            const int savedPicks = g.picksPl;
            g.picksPl = np;
            InsertPlaylist(1, Playlist{L"Shifted", {}, PlaylistKind::Static});
            const bool shifted = g.picksPl == np + 1;
            ErasePlaylist(1);
            Check(shifted && g.picksPl == np, L"UI: taste picks follow their playlist when tabs move (%d)", g.picksPl);
            g.picksPl = savedPicks;
        }
        // Now Playing shows the whole of the playing album, even when playback began from a list that held
        // only one of its tracks (Top rated): the rest joins the list, once, right behind the playing track.
        g.playlists[np].items = {1};
        g.npCompleted = kNone;
        g.playing = 1;
        g.playingPl = np;
        g.playingToken = 0;
        RebuildView();
        const auto& npItems = g.playlists[np].items;
        size_t albumTracks = 0;
        for (TrackId id = 0; id < (TrackId)g_lib.Count(); ++id)
            if ((g_lib[id].flags & Track::kInLibrary) && SameAlbum(g_lib[id], g_lib[1])) ++albumTracks;
        Check(g_lib[1].album && albumTracks >= 3 && npItems.size() == albumTracks && npItems[0] == 1 &&
                  std::count(npItems.begin(), npItems.end(), (TrackId)0) == 1 &&
                  std::count(npItems.begin(), npItems.end(), (TrackId)2) == 1 && g.view.size() == albumTracks,
              L"UI: Now Playing gains the rest of the playing album, behind the playing track (items=%zu of %zu)",
              npItems.size(), albumTracks);
        g.playlists[np].items.pop_back();
        RebuildView();
        Check(g.playlists[np].items.size() == albumTracks - 1,
              L"UI: a track removed from the playing album doesn't come back with the next rebuild");
        g.playing = kNone;
        g.playingPl = -1;
        g.npCompleted = kNone;
        g.playlists[np].items = {0, 1, 2};
        RebuildView();
    }
    batch.End();
    TestQueueEditor(scratch);
    TestPortedUi(scratch);
    TestReplayGainRestart(scratch);
    TestPersonalization(scratch);
    TestWebPreferences(scratch);
    TestLightweightMode();
    TestKeyboardShortcuts();
    TestMenuAccessKeys();
    batch.Next(L"UI: shutdown joins every application worker before destroying the test window");
    StopAppWorkers(g.hwnd);
    Check(!g.rgWorker.joinable() && !g.tagWorker.joinable() && !g.importWorker.joinable() && !g.artWorker.joinable(),
          L"UI: shutdown joins every application worker before destroying the test window");
    DestroyWindow(g.tips);
    DestroyWindow(g.hwnd);
    g.hwnd = nullptr;
    g.libView = nullptr;
    g.pointsChip = nullptr;
    g.closing = false;
    ClearBrowserCaches();
}
