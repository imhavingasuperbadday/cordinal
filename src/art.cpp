// Image decoding for album art on top of WIC: DecodeImage scales any stream WIC understands into
// an ArtImage, and LoadAlbumArt prefers a picture embedded in the media file (with a size cap)
// before falling back to the cover files sitting next to it. Requires COM on the calling thread.
#include "art.h"

#include "library.h"
#include "tags.h"

#include <shlwapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>

using Microsoft::WRL::ComPtr;

// Rejects absurd declared sizes and target sizes up front, so a corrupt or hostile image can't
// trigger a huge allocation before it fails.
bool DecodeImage(IStream* stream, int size, ArtImage& out, uint64_t maxPixels) {
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wic->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder))) return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return false;
    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    if (!w || !h || w > 32768 || h > 32768 || (uint64_t)w * h > maxPixels || size <= 0 || size > 4096) return false;
    double scale = std::min(1.0, (double)size / std::max(w, h));
    UINT tw = std::max(1u, (UINT)(w * scale)), th = std::max(1u, (UINT)(h * scale));
    ComPtr<IWICBitmapSource> source = frame;
    if (tw != w || th != h) {
        ComPtr<IWICBitmapScaler> scaler;
        if (FAILED(wic->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(frame.Get(), tw, th, WICBitmapInterpolationModeHighQualityCubic)))
            return false;
        source = scaler;
    }
    // Output is always 32bpp (alpha ignored), matching ArtImage's pixel layout.
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom)))
        return false;
    out.width = (int)tw;
    out.height = (int)th;
    out.pixels.assign((size_t)tw * th, 0);
    if (FAILED(conv->CopyPixels(nullptr, tw * 4, tw * th * 4, reinterpret_cast<BYTE*>(out.pixels.data())))) {
        out.pixels.clear();
        return false;
    }
    for (auto& p : out.pixels) p &= 0xFFFFFF;  // premultiplied on black
    return true;
}

// Embedded art wins over sidecar files; the 32 MB cap rejects id3/vorbis blobs that are really
// junk or a mis-sized tag.
bool LoadAlbumArt(const std::wstring& mediaPath, int size, ArtImage& out) {
    out = ArtImage{};
    std::string bytes;
    if (ReadEmbeddedArt(mediaPath, bytes) && bytes.size() <= (32u << 20)) {
        ComPtr<IWICImagingFactory> wic;
        ComPtr<IWICStream> s;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
            SUCCEEDED(wic->CreateStream(&s)) &&
            SUCCEEDED(s->InitializeFromMemory(reinterpret_cast<BYTE*>(bytes.data()), (DWORD)bytes.size())) &&
            DecodeImage(s.Get(), size, out))
            return true;
        std::string().swap(bytes);
    }
    std::wstring dir = DirectoryOf(mediaPath);
    // Sidecar covers, in the order other players look (jpg before png, "cover" first).
    for (const wchar_t* name : {L"cover.jpg", L"folder.jpg", L"front.jpg", L"cover.png", L"folder.png", L"front.png",
                                L"AlbumArtSmall.jpg", L"cover.jpeg"}) {
        ComPtr<IStream> s;
        if (SUCCEEDED(SHCreateStreamOnFileEx((dir + L"\\" + name).c_str(), STGM_READ | STGM_SHARE_DENY_NONE, 0, FALSE,
                                             nullptr, &s)) &&
            DecodeImage(s.Get(), size, out))
            return true;
    }
    return false;
}
