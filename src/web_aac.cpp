// Browser data saver: the built-in AAC encoder consumes the existing bounded PCM stream.
// A completed compressed representation has an exact length and supports byte-for-byte resumption.
#include "web_internal.h"
#include "cue.h"
#include "library.h"
#include "aac_encoder.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace web {
using aac::Encoder;
using aac::Sender;

Media AacMedia(const Track& track, double start, const std::atomic<bool>& cancel, unsigned kbps, AacCache* cache) {
    if (kbps != 96 && kbps != 128) {
        Media result; result.error = "Unknown compressed streaming quality."; return result;
    }
    auto pcm = WaveMedia(track, start, cancel);
    if (!pcm.error.empty()) return pcm;
    Media result;
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(AudioFileOf(track.path).c_str(), GetFileExInfoStandard, &info)) {
        result.error = "The music file is unavailable."; return result;
    }
    uint64_t fileSize = ((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow;
    uint64_t fileTime = ((uint64_t)info.ftLastWriteTime.dwHighDateTime << 32) | info.ftLastWriteTime.dwLowDateTime;
    CueSubtrack cue;
    LookupSubtrack(track.path, cue);
    // Scanned CUE tracks carry the later of the audio file and sheet's modification times.
    if ((track.fileSize && track.fileSize != fileSize) ||
        (track.fileTime && track.fileTime != std::max(fileTime, cue.cueTime))) {
        result.error = "The music file changed. Refresh the library and try again."; return result;
    }
    const uint64_t identity[]{fileSize, fileTime, cue.cueTime, pcm.size,
        (uint64_t)std::llround(start * 48000), kbps};
    std::string key = ToUtf8(track.path) + '\0';
    key.append((const char*)identity, sizeof(identity));
    std::shared_ptr<const std::string> bytes;
    if (cache) {
        std::lock_guard lock(cache->mutex);
        if (cache->key == key) bytes = cache->bytes;
    }
    if (!bytes) {
        // iPhone media requests can resume at any byte, even without a JavaScript error. Sending
        // a fresh stream from its beginning repeats music underneath the existing media clock.
        // Prepare only this track's bounded window in memory so every range has a stable length.
        constexpr size_t limit = 64u * 1024 * 1024;
        auto encoded = std::make_shared<std::string>();
        encoded->reserve((size_t)std::min<uint64_t>(limit, (pcm.size - 44) / 4 * kbps / 384 + 4096));
        Encoder encoder;
        if (!encoder.Open(kbps)) {
            result.error = "The AAC encoder couldn't start. Choose Original quality to play.";
            return result;
        }
        auto append = [&](const char* data, size_t size) {
            if (cancel || size > limit - encoded->size()) return false;
            encoded->append(data, size); return true;
        };
        bool complete = pcm.send(44, (size_t)(pcm.size - 44), [&](const char* data, size_t size) {
            return encoder.Write(data, size, append, cancel);
        }) && encoder.Finish(append, cancel);
        if (!complete || encoded->empty()) {
            result.error = cancel ? "Playback was cancelled." : "Compressed audio could not be prepared. Try again, or choose Original quality.";
            return result;
        }
        bytes = std::move(encoded);
        if (cache) {
            std::lock_guard lock(cache->mutex);
            cache->key = std::move(key); cache->bytes = bytes;
        }
    }
    result.size = bytes->size(); result.mime = "audio/aac";
    result.send = [bytes, &cancel](uint64_t at, size_t length, const Sender& send) {
        if (at > bytes->size() || length > bytes->size() - (size_t)at) return false;
        while (length && !cancel) {
            size_t n = std::min<size_t>(length, 65536);
            if (!send(bytes->data() + (size_t)at, n)) return false;
            at += n; length -= n;
        }
        return !cancel && length == 0;
    };
    return result;
}
}  // namespace web
