// Native ListenBrainz, Last.fm and Libre.fm scrobbling. Off until connected; credentials are DPAPI-encrypted
// on Windows and owner-only on Linux. Finished listens go to a separate queue file per service.
//
// Finished listens are written to queue files before anything is sent, so a listen survives
// an offline spell or a closed app. A worker per service sends them; the UI thread only queues work
// and reads the status. Nothing here blocks the UI thread on the network.
#pragma once
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <map>
namespace web { struct HttpReply; }

namespace listenbrainz {
enum class Service { ListenBrainz, LastFm, LibreFm };
// Messages pack the request generation above bit 7 and service in bits 4-5; the low four bits
// identify the result. Check CurrentResult before accepting an authentication reply. Results 4 and 5
// carry a heap std::wstring with a browser token and stored session credentials, respectively.
std::wstring AuthorizationUrl(Service service, const std::wstring& key, const std::wstring& token);
void Authorize(Service service, const std::wstring& key, const std::wstring& secret,
               const std::wstring& token = {});
void CancelAuthorization(Service service);
bool CurrentResult(WPARAM message);
bool Paused();
void SetPaused(bool paused);
bool SendNowPlaying();
void SetSendNowPlaying(bool on);
// Protocol helpers are pure so signatures and API error handling can be checked offline.
std::string SignedForm(const std::map<std::string, std::string>& params, const std::string& secret);
unsigned SubmissionStatus(unsigned http, const std::string& body);


struct Listen {
    uint32_t listenedAt = 0;  // unix seconds; Submit needs it, PlayingNow ignores it
    std::string artist, title, release, releaseArtist;  // UTF-8
    uint32_t durationMs = 0, trackNumber = 0;
};

// Status changes post `msg` with the low wParam bits = kStatusChanged and lParam = 0.
// Validate() posts `msg` with low bits = 1 (valid), 0 (invalid) or 2 (network error). When valid,
// lParam is a heap std::wstring* holding the user name, and the receiver must delete it.
constexpr WPARAM kStatusChanged = 3;

struct Status {
    size_t queued = 0;     // listens waiting to be sent
    uint32_t lastSent = 0;  // unix seconds of the last accepted submission
    std::wstring lastError;  // empty when all is well
    std::wstring user;      // from the last successful Validate
};

// Loads the token and the queue from dataDir and starts the worker. Call once at startup.
void Start(const std::wstring& dataDir, HWND notify, UINT msg);
// Cancels requests in flight and joins the worker. Returns quickly; the queue is already on disk.
void Stop();
// Stores the token encrypted with DPAPI, or turns sending off when empty. Needs Start first.
bool SetToken(const std::wstring& token, Service service = Service::ListenBrainz);
bool Enabled(Service service = Service::ListenBrainz);

// Now-playing notice: sent once, never queued, and only the latest one is kept.
void PlayingNow(const Listen& l);
// A finished listen: queued on disk, then sent. Dropped when sending is off or the
// listen has no artist, title or timestamp.
void Submit(const Listen& l);
// Checks the token with the server (asynchronous, see the message above).
void Validate(const std::wstring& token);

Status GetStatus(Service service = Service::ListenBrainz);

// The JSON object for one listen, or empty when the artist or title is missing. The timestamp
// is included only when withTimestamp is set.
std::string ListenJson(const Listen& l, bool withTimestamp);

// Self-test hook: while disabled, the worker never sends, so tests stay off the network.
void SetSendingDisabledForTest(bool disabled);
using HttpForTest = web::HttpReply (*)(Service, const std::string& url, const char* method,
                                      const std::string& body, const std::string& headers);
// Set only around an isolated test, before Start and after Stop. Null restores native HTTPS.
void SetHttpForTest(HttpForTest request);

}  // namespace listenbrainz
