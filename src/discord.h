// Discord status ("Listening to cordinal"): the song or the album playing, sent to the Discord app running on
// this computer over its local IPC pipe (Windows) or socket (Linux). No SDK, no network, no extra DLL.
//
// The UI thread hands Update() what is playing; it works out the activity and passes it to a worker only when
// what Discord shows would change. Discord draws the progress itself from the timestamps, so a playing song
// costs nothing between track changes, pauses and seeks. The worker connects only while there is something to
// show, retries quietly while Discord is closed, and keeps within Discord's rate limit.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace discord {

enum class Show : uint8_t { Off, Song, Album };
constexpr int kShowChoices = 3;

struct NowPlaying {
    bool active = false;  // a track or station is loaded (false: stopped)
    bool paused = false;  // nothing is shown while paused
    bool radio = false;   // title is the stream's song (may be empty), artist the station
    std::string title, artist, album, albumArtist;  // UTF-8
    std::string release;                            // MusicBrainz release id from the tags, if any
    std::wstring path;                              // the track's path as the library names it, for its custom cover
    double position = 0, duration = 0, rate = 1;    // seconds; rate is playback speed
};

// False until this build carries a Discord application id: Preferences then hides the setting and Update
// sends nothing, whatever settings.ini says.
bool Available();
Show Mode();
// Takes effect at once: the current status is replaced or, for Off, cleared.
void SetMode(Show mode);
// UI thread. Cheap to call often: it returns at once when Discord's status would not change.
void Update(const NowPlaying& now);
// Album covers: Discord can show only an image on the web, never a local file. When a cover source is turned on in
// Preferences > Online, the album's cover address is looked up once per album (online::FindCoverUrl) on a thread of
// its own, and the status shows cordinal's icon until it is found.
//
// Clears the status and joins the workers; call before online::Stop. Safe when they never started.
void Stop();

// The activity object for SET_ACTIVITY, or empty when nothing should be shown. Song mode ends the bar at the
// song's end; Album mode counts up from albumStartMs and leaves album changes as its only update. With a cover
// address the album's cover is the picture and cordinal's icon its badge; without one, the icon alone. Pure.
std::string ActivityJson(const NowPlaying& now, Show mode, int64_t nowMs, int64_t albumStartMs,
                         std::string_view cover = {});
// The address Discord shows as the picture: the cover chosen in cordinal's own address when it has one, else the
// album's online cover. Empty when neither is usable, and then cordinal's icon stands in.
std::string PreferredCover(std::string_view customUrl, std::string_view onlineUrl);
// One IPC message: little-endian opcode and length, then the JSON.
std::string Frame(uint32_t opcode, std::string_view json);

// Self-test hooks. `endpoint` replaces Discord's pipe or socket name before its trailing digit (Windows
// "\\.\pipe\name-", Linux "/dir/name-"); empty restores Discord's. Set before Update, with the worker stopped.
void SetEndpointForTest(const std::string& endpoint);
uint32_t SentForTest();  // SET_ACTIVITY messages Discord accepted since start

}  // namespace discord
