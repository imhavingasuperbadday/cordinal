// Album art: embedded pictures (ID3 APIC, FLAC/Vorbis PICTURE, MP4 covr, ASF WM/Picture)
// or cover/folder/front images next to the file, decoded once to a small square thumbnail.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

// A decoded image: 0x00RRGGBB pixels (alpha already flattened onto black), packed rows top-down.
struct ArtImage {
    int width = 0, height = 0;
    std::vector<uint32_t> pixels;  // 0x00RRGGBB, composited on black
    bool Empty() const { return pixels.empty(); }
};

// Decodes and scales to fit `size` x `size`. Requires COM (WIC) on the calling thread. A cover
// chosen in cordinal (customcover.h) comes first unless `custom` is false, as for the tag editor,
// which shows what the file itself holds.
bool LoadAlbumArt(const std::wstring& mediaPath, int size, ArtImage& out, bool custom = true);
// Decodes any image WIC understands from a stream, scaled to fit `size` x `size`. Requires COM.
// `sourceWidth`/`sourceHeight`, when given, receive the image's own size.
struct IStream;
bool DecodeImage(IStream* stream, int size, ArtImage& out, uint64_t maxPixels = 100000000, int* sourceWidth = nullptr,
                 int* sourceHeight = nullptr);
