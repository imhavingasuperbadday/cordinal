// A sequential source of encoded radio audio. Reads may be short; zero means the source ended
// or was cancelled. Implementations must unblock a pending Read when Abort is called.
#pragma once

#include "decoder.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>

class LiveAudioSource {
public:
    virtual ~LiveAudioSource() = default;
    virtual size_t Read(uint8_t* out, size_t bytes) = 0;
    virtual void Abort() = 0;
    virtual std::wstring Problem() const = 0;
};

inline bool ReadLiveBytes(LiveAudioSource& source, uint8_t* out, size_t bytes) {
    while (bytes) {
        size_t n = source.Read(out, bytes);
        if (!n || n > bytes) return false;
        out += n;
        bytes -= n;
    }
    return true;
}

// Skip a leading ID3 tag before looking for sync words: text in a tag can itself resemble an
// MPEG frame. Refuse implausibly large tags instead of buffering or scanning through them.
inline bool SkipLiveId3(LiveAudioSource& source, uint8_t* prefix, size_t bytes) {
    if (bytes < 3 || memcmp(prefix, "ID3", 3)) return true;
    if (bytes > 10) return false;
    uint8_t header[10];
    memcpy(header, prefix, bytes);
    if (!ReadLiveBytes(source, header + bytes, sizeof(header) - bytes) ||
        ((header[6] | header[7] | header[8] | header[9]) & 0x80)) return false;
    size_t size = (size_t(header[6]) << 21) | (size_t(header[7]) << 14) |
                  (size_t(header[8]) << 7) | header[9];
    if (header[5] & 16) size += 10;
    if (size > (1u << 20)) return false;
    uint8_t discard[4096];
    while (size) {
        size_t n = std::min(size, sizeof(discard));
        if (!ReadLiveBytes(source, discard, n)) return false;
        size -= n;
    }
    return ReadLiveBytes(source, prefix, bytes);
}

// Both decoders keep codec state across frames, report an unknown duration and cannot seek.
std::unique_ptr<Decoder> OpenLiveMpeg(std::shared_ptr<LiveAudioSource> source);
std::unique_ptr<Decoder> OpenLiveAac(std::shared_ptr<LiveAudioSource> source);
std::unique_ptr<Decoder> OpenLiveAudio(std::shared_ptr<LiveAudioSource> source);
