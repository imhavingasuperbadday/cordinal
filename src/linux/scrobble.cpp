// Native scrobbling on Linux: the shared submission module (src/listenbrainz.cpp) started with the profile,
// a "playing now" notice for each track that starts and a listen for each play that counts, as the Windows
// build does from app_platform.cpp. The connections window is a panel (panels.cpp).
#include "app.h"
#include "listenbrainz.h"

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
