// HTTP and media primitives shared with the companion's server checks.
#pragma once
#include "web_server.h"

#include <atomic>
#include <map>
#include <mutex>

class Decoder;

namespace web {

struct Request {
    std::string method, target, path, body;
    std::map<std::string, std::string> headers, query;
};
// Only one bounded HTTP/1.x request per connection; ambiguous framing is refused.
bool ParseRequest(std::string_view bytes, Request& request, size_t& required);
std::string Json(std::string_view value);
std::string RandomHex(size_t bytes);
std::string Lower(std::string value);
std::string Mime(const std::wstring& path, const std::string& codec);
uint64_t ClockMs();
struct ByteRange { uint64_t first = 0, size = 0; bool partial = false; };
bool ParseRange(std::string_view header, uint64_t size, ByteRange& range);

struct Media {
    uint64_t size = 0;
    std::string mime, error;
    std::function<bool(uint64_t, size_t, const std::function<bool(const char*, size_t)>&)> send;
};
Media OriginalMedia(const Track& track);
Media WaveMedia(const Track& track, double start, const std::atomic<bool>& cancel);
Media WaveMedia(std::shared_ptr<Decoder> decoder, double start, const std::atomic<bool>& cancel);
// Keep one completed compressed representation for repeated browser range requests. Only the
// cache fields share a lock; encoding and sending run on the requesting worker without it.
struct AacCache {
    std::mutex mutex;
    std::string key;
    std::shared_ptr<const std::string> bytes;
};
Media AacMedia(const Track& track, double start, const std::atomic<bool>& cancel,
               unsigned kbps = 96, AacCache* cache = nullptr);
std::string Artwork(const Track& track, std::string& mime);
std::string Waveform(const Track& track, const std::atomic<bool>& cancel);
std::string Themes(ThemeMode current, bool systemDark);
std::string TrackJson(const Track& track);
std::string Catalog(const Snapshot& snapshot, const Request& request);
// Small private files: DPAPI-protected on Windows, owner-only on Linux, replaced atomically.
// A missing file reads as empty; one that can't be read or decrypted reads as false.
bool ReadRecords(const std::wstring& path, std::string& bytes);
bool SaveRecords(const std::wstring& path, std::string bytes);

}  // namespace web
