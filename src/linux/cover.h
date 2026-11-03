// Cover art for the Linux front end, decoded on a worker thread (cover.cpp).
#pragma once
#include "library.h"
#include "metadata_fetch.h"

#include <cstdint>
#include <string>
#include <vector>

namespace lx {

struct CoverImage {
    TrackId id;
    int width, height;          // 0 when the track has no cover
    std::vector<uint8_t> rgba;  // width * height * 4
    std::string file;           // the encoded image, saved for MPRIS (mpris:artUrl); "" if none
    int discWidth = 0, discHeight = 0;  // the playing cover's disc art (discart.h), with alpha
    std::vector<uint8_t> discRgba;
};

void StartCovers();
void StopCovers();
// Decodes `id`'s cover; it arrives as WM_APP_COVER with lParam = CoverImage*. A newer request
// replaces one not yet started.
void RequestCover(TrackId id);
// Decodes a small cover for an album header (at most `size` pixels); it arrives as WM_APP_THUMB.
// Requests queue behind the playing track's cover; `ForgetThumbs` drops those not yet started.
void RequestThumb(TrackId id, int size);
void ForgetThumbs();
// On the calling thread, for the tag editor: a track's own cover as encoded bytes ("" if none; a
// cover chosen in cordinal is left out), and encoded bytes decoded to at most `size` pixels (false
// if they aren't a readable image).
std::string TrackCoverBytes(const std::wstring& path);
bool DecodeCover(const std::string& bytes, int size, CoverImage& out);
bool DecodeRadioIcon(const std::string& bytes, int size, CoverImage& out);
// A downloaded cover (metadata_fetch.h): decoded whole to prove it is an image, its real size,
// and a thumbnail of at most `thumbSize` pixels when that is above 0. Any thread.
bool DecodeFetchedCover(const std::string& bytes, int thumbSize, online::Decoded& out);

}  // namespace lx
