// A Direct2D target rasterized in software on the calling thread, copied to a GDI DC after each
// frame. It replaces Direct2D's own software targets, which run WARP with a pool of worker threads
// that each keep about 0.9 MB of work buffers, and an HWND target that holds extra window-sized
// surfaces besides. docs/memory.md has the measurements.
#pragma once

#include <windows.h>
#include <d2d1_1.h>
#include <wrl/client.h>

class SoftCanvas {
public:
    // Makes the device and its drawing context on first use; null when Direct3D's WARP is unavailable.
    // Bitmaps created on the context last until Reset, across size changes.
    ID2D1DeviceContext* Context(ID2D1Factory* factory);
    // Starts a frame of w x h pixels (96 DPI), reallocating the surface only when the size changes.
    bool Begin(int w, int h);
    // Ends the frame and copies it to `dc` at (x, y). Returns EndDraw's result; on
    // D2DERR_RECREATE_TARGET the caller resets the canvas and anything made on its context.
    HRESULT End(HDC dc, int x, int y);
    void Reset();

private:
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> target_;
    int width_ = 0, height_ = 0;
};
