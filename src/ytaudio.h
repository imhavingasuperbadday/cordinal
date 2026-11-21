// YouTube audio, saved as YouTube's own audio-only stream (never a re-encode): watch,
// youtu.be, /shorts and music.youtube links, and playlists, resolved through InnerTube
// as the visionOS app - the client that still answers with direct googlevideo URLs, no
// signature cipher, no n-parameter, no PO token (the recipe Puto's youtube.js and
// yt-dlp's 2026.08 default use, with a visitor id so YouTube does not ask the player
// to prove it is not a bot). AAC (itag 140, then 139) is preferred - an .m4a that
// plays and tags everywhere; Opus (251/250/249) is the fallback.
#pragma once
#include "webfetch.h"
#include <atomic>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace yt {

enum class Kind { None, Video, Playlist };
// youtube.com/watch, youtu.be/<id>, /shorts, music.youtube.com/watch and ?list= links.
Kind Classify(std::string_view url);

// The ids those links carry: "" when there is none to read.
std::string VideoIdOf(std::string_view url);
std::string PlaylistIdOf(std::string_view url);

// Pure parsers over InnerTube replies, fixture-tested and never on the network.
// ParsePlayer fills one track: its best audio-only stream, title, the channel as
// artist, duration and the hqdefault thumbnail. A video that is gone, age-restricted,
// members-only or live answers true with `unavailable` and `why` set, so the queue
// remembers it instead of failing; only YouTube's bot refusal (worded as temporary so
// the run can be tried again later), a request that did not answer and a torn reply
// are false with `error`.
bool ParsePlayer(std::string_view json, WebTrack& out, std::string& error);

// A /browse reply for a playlist, in order.
struct PlaylistEntry { std::string id, title; unsigned durationSec = 0; };
size_t ParsePlaylist(std::string_view json, std::vector<PlaylistEntry>& out);

// A /search reply's video rows, in order; `topic` marks a "- Topic" upload.
struct Candidate { std::string id, title, channel; unsigned durationSec = 0; bool topic = false; };
size_t ParseSearch(std::string_view json, std::vector<Candidate>& out);

// Network, on the caller's worker thread, one request at a time with `cancel` polled
// around each. ResolveVideo runs the visitor-id handshake once per process and reuses
// the id. `error` is a full sentence for the user.
bool ResolveVideo(const std::string& videoId, const std::atomic<bool>& cancel,
                  WebTrack& out, std::string& error);
bool Search(const std::string& query, const std::atomic<bool>& cancel,
            std::vector<Candidate>& out, std::string& error);
// The whole list a playlist link holds, each entry resolved with its own stream and
// numbered 1..n; paced between entries the way Puto paces an album, so YouTube is not
// asked too fast.
bool ResolvePlaylist(const std::string& playlistId, const std::atomic<bool>& cancel,
                     std::vector<WebTrack>& tracks, std::string& listTitle, std::string& error,
                     const std::function<void(const std::string&)>& notice = {});

}  // namespace yt
