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
// The song's file exactly as stored, in any format, for saving on the device. A song that is part of
// a larger file (a cue sheet, a game rip's subsong) has none.
Media FileMedia(const Track& track);
// `name` without path separators, reserved or control characters, at most about 120 bytes, never empty.
std::string SafeFileName(std::string name);
// "Artist - Title.flac", or "07 Title.flac" for a numbered entry of a ZIP; safe as a file name.
std::string DownloadName(const Track& track, unsigned number = 0);
// The songs' files in one stored ZIP, under `folder`; `numbered` keeps the list's order in the names.
Media ZipMedia(const std::vector<const Track*>& tracks, const std::string& folder, bool numbered);
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
// The tracks a catalog request's filters select (playlist, album, artist, date, search), in order.
std::vector<const Track*> CatalogTracks(const Snapshot& snapshot, const Request& request);
// XML for UPnP (web_dlna.cpp, speakers.cpp): text escaped for an element or attribute, entities and
// character references decoded, and the text of the first <name> element with or without a prefix.
std::string XmlEscape(std::string_view text);
std::string XmlUnescape(std::string_view text);
std::string XmlElement(std::string_view xml, std::string_view name);
// This computer's IPv4 addresses on networks that are up, loopback left out.
std::vector<std::string> LocalIPv4();
// The DLNA media server (Share): its device description, its services' descriptions, and a SOAP
// action's reply (`status` 200, or 500 carrying a UPnP error). `base` is "http://host:port" as the
// player reached the server.
std::string DlnaDescription(const Share& share, bool icon);
std::string_view DlnaScpd(bool contentDirectory);
std::string DlnaControl(const Snapshot& snapshot, bool contentDirectory, std::string_view action, std::string_view body,
                        const std::string& base, int& status);
// SSDP: the replies to one M-SEARCH (none when it isn't one or asks for something else), and the
// announcements of the server arriving (alive) or leaving.
std::vector<std::string> SsdpAnswers(std::string_view request, const std::string& udn, const std::string& location);
std::vector<std::string> SsdpNotices(const std::string& udn, const std::string& location, bool alive);
// Announces the media server and answers searches on UDP 1900, on a thread of its own.
class Announcer {
public:
    Announcer();
    ~Announcer();
    void Start(uint16_t port, const std::string& udn);
    void Stop();  // says goodbye on every network first
    bool Running() const;
    std::string Error() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
// Small private files: DPAPI-protected on Windows, owner-only on Linux, replaced atomically.
// A missing file reads as empty; one that can't be read or decrypted reads as false.
bool ReadRecords(const std::wstring& path, std::string& bytes);
bool SaveRecords(const std::wstring& path, std::string bytes);

}  // namespace web
