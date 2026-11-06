// Native scrobbling on Linux: the shared submission module (src/listenbrainz.cpp) started with the profile,
// a "playing now" notice for each track that starts and a listen for each play that counts, as the Windows
// build does from app_platform.cpp. The connections window is a panel (panels.cpp). The Discord status
// (src/discord.cpp) is fed from here too.
#include "app.h"
#include "album_editions.h"
#include "discord.h"
#include "listenbrainz.h"
#include "radio_ui.h"

namespace lx {
namespace {

uint32_t g_startedAt = 0;
TrackId g_startedTrack = kNone;
unsigned long long g_announced = 0;  // the engine token whose "playing now" was sent

listenbrainz::Listen MakeListen(TrackId id) {
    const Track& t = g_lib[id];
    listenbrainz::Listen l;
    l.artist = std::string(g_lib.View(t.artist ? t.artist : t.albumArtist));
    l.title = std::string(g_lib.View(t.title));
    l.release = std::string(g_lib.View(t.album));
    l.releaseArtist = std::string(g_lib.View(t.albumArtist));
    l.durationMs = t.durationMs;
    l.trackNumber = t.trackNo;
    return l;
}

}  // namespace

void InitScrobble() { listenbrainz::Start(g.configDir, nullptr, WM_APP_LISTENBRAINZ); }

void StopScrobble() { listenbrainz::Stop(); }

void StopDiscord() { discord::Stop(); }

// As the Windows build's DiscordTick: a new track or a pause at once, a seek within a second. Off, nothing.
void DiscordTick() {
    static unsigned long long token = 0;
    static bool paused = false;
    static uint64_t checked = 0;
    if (!discord::Available() || discord::Mode() == discord::Show::Off) return;
    discord::NowPlaying now;
    now.radio = RadioPlaying();
    now.active = now.radio || g_lib.Contains(g.playing);
    now.paused = now.active && IsPaused();
    uint64_t tick = SDL_GetTicks();
    if (token == g.token && paused == now.paused && tick - checked < 1000) return;
    token = g.token;
    paused = now.paused;
    checked = tick;
    if (now.radio) {
        const auto& radio = InternetRadio();
        now.title = ToUtf8(radio.lastTitle);
        now.artist = radio.playing.name;
    } else if (now.active) {
        const Track& t = g_lib[g.playing];
        now.title = compat::Narrow(TrackTitle(g.playing).c_str());
        now.artist = compat::Narrow(TrackArtist(g.playing).c_str());
        now.album = std::string(g_lib.View(t.album));
        now.albumArtist = std::string(g_lib.View(t.albumArtist));
        if (const AlbumEdition* edition = AlbumEditionForTrack(g.playing)) now.release = edition->musicBrainzRelease;
        now.path = g_lib.Path(g.playing);  // the track's custom cover, when it has one
        now.position = g.engine.Position();
        now.duration = g.engine.Duration() > 0 ? g.engine.Duration() : t.Duration();
        now.rate = g.speed * g.tempo;
    }
    discord::Update(now);
}

void ScrobbleTrackStarted(unsigned long long token, TrackId id) {
    if (token == g_announced || !g_lib.Contains(id)) return;
    g_announced = token;
    g_startedAt = UnixNow();
    g_startedTrack = id;
    listenbrainz::PlayingNow(MakeListen(id));
}

void ScrobbleCounted(TrackId id, uint32_t when, double seconds, bool local) {
    if (!g_lib.Contains(id)) return;
    listenbrainz::Listen l = MakeListen(id);
    l.listenedAt = local && g_startedTrack == id && g_startedAt ? g_startedAt :
        when > seconds ? when - (uint32_t)seconds : 1;
    listenbrainz::Submit(l);
}

}  // namespace lx
