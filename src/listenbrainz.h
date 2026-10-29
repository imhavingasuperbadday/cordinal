// Scrobbling to ListenBrainz. Off until a user token is set; the token is stored DPAPI-encrypted
// (readable only by this Windows account) and finished listens go to a plain-text queue file.
//
// Finished listens are written to a queue file before anything is sent, so a listen survives
// an offline spell or a closed app. A worker thread sends them; the UI thread only queues work
// and reads the status. Nothing here blocks the UI thread on the network.
#pragma once
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace listenbrainz {

struct Listen {
    uint32_t listenedAt = 0;  // unix seconds; Submit needs it, PlayingNow ignores it
    std::string artist, title, release, releaseArtist;  // UTF-8
    uint32_t durationMs = 0, trackNumber = 0;
};

// Status changes post `msg` to the notify window with wParam = kStatusChanged, lParam = 0.
// Validate() posts `msg` with wParam = 1 (valid), 0 (invalid) or 2 (network error). When valid,
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
void SetToken(const std::wstring& token);
bool Enabled();

// Now-playing notice: sent once, never queued, and only the latest one is kept.
void PlayingNow(const Listen& l);
// A finished listen: queued on disk, then sent. Dropped when sending is off or the
// listen has no artist, title or timestamp.
void Submit(const Listen& l);
// Checks the token with the server (asynchronous, see the message above).
void Validate(const std::wstring& token);

Status GetStatus();

// The JSON object for one listen, or empty when the artist or title is missing. The timestamp
// is included only when withTimestamp is set.
std::string ListenJson(const Listen& l, bool withTimestamp);

// Self-test hook: while disabled, the worker never sends, so tests stay off the network.
void SetSendingDisabledForTest(bool disabled);

}  // namespace listenbrainz
