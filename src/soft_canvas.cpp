// SoftCanvas (see soft_canvas.h): a WARP device that may not start threads, one GDI-compatible
// target bitmap, and a BitBlt to the window after each frame.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "soft_canvas.h"

#include <d3d11.h>

using Microsoft::WRL::ComPtr;

ID2D1DeviceContext* SoftCanvas::Context(ID2D1Factory* factory) {
    if (context_) return context_.Get();
    // Loaded when first drawn, so a start that never draws the canvas keeps Direct3D out of memory.
    using Create = decltype(&D3D11CreateDevice);
    static const auto create = [] {
        HMODULE d3d = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return d3d ? reinterpret_cast<Create>(GetProcAddress(d3d, "D3D11CreateDevice")) : nullptr;
    }();
    ComPtr<ID2D1Factory1> factory1;
    if (!create || FAILED(factory->QueryInterface(IID_PPV_ARGS(&factory1)))) return nullptr;
    // PREVENT_INTERNAL_THREADING_OPTIMIZATIONS keeps WARP on this thread. Its worker pool paints a
    // full cover grid faster (4.5 ms against 7.9 ms at 2560 x 1400, 16 hardware threads), well inside
    // a frame either way, but it starts 9 to 15 threads and each worker keeps 0.9 MB of work buffers.
    ComPtr<ID3D11Device> device;
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_SINGLETHREADED |
                       D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS;
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<ID2D1Device> d2d;
    if (FAILED(create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr,
                      nullptr)) ||
        FAILED(device.As(&dxgi)) || FAILED(factory1->CreateDevice(dxgi.Get(), &d2d)) ||
        FAILED(d2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context_)))
        return nullptr;
    device_ = device;
    context_->SetDpi(96.0f, 96.0f);  // work in pixels; S() does the scaling
    return context_.Get();
}

bool SoftCanvas::Begin(int w, int h) {
    if (!context_ || w <= 0 || h <= 0) return false;
    if (!target_ || w != width_ || h != height_) {
        context_->SetTarget(nullptr);
        target_.Reset();
        const auto props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW | D2D1_BITMAP_OPTIONS_GDI_COMPATIBLE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
        if (FAILED(context_->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, props, &target_))) return false;
        width_ = w;
        height_ = h;
        context_->SetTarget(target_.Get());
    }
    context_->BeginDraw();
    return true;
}

HRESULT SoftCanvas::End(HDC dc, int x, int y) {
    const HRESULT result = context_->EndDraw();
    if (FAILED(result)) return result;
    ComPtr<IDXGISurface> surface;
    ComPtr<IDXGISurface1> gdi;
    HDC source = nullptr;
    if (SUCCEEDED(target_->GetSurface(&surface)) && SUCCEEDED(surface.As(&gdi)) && SUCCEEDED(gdi->GetDC(FALSE, &source))) {
        BitBlt(dc, x, y, width_, height_, source, 0, 0, SRCCOPY);
        RECT unchanged{};
        gdi->ReleaseDC(&unchanged);
    }
    return result;
}

void SoftCanvas::ReleaseSurface() {
    if (!target_) return;
    context_->SetTarget(nullptr);
    target_.Reset();
    width_ = height_ = 0;
    // Direct3D frees a released texture at its next flush, which an idle canvas would never reach.
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> immediate;
    if (device_ && SUCCEEDED(device_.As(&device))) device->GetImmediateContext(&immediate);
    if (immediate) immediate->Flush();
}

void SoftCanvas::Reset() {
    if (context_) context_->SetTarget(nullptr);
    target_.Reset();
    context_.Reset();
    device_.Reset();
    width_ = height_ = 0;
}
