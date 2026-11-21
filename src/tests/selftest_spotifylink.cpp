// Spotify link classification, the embed and album-state parsers, the YouTube query ladder
// and Puto's match ranking, against fabricated pages and search rows with no network.
#include "selftest_common.h"
#include "spotifylink.h"

#include <string>
#include <vector>

namespace {

const char kTrackEmbed[] = R"html(<!doctype html>
<html><head><title>Spotify Embed</title></head><body>
<script id="__NEXT_DATA__" type="application/json">{"props":{"pageProps":{"state":{"data":{"entity":{"type":"track","name":"Never Gonna Give You Up","uri":"spotify:track:4cOdK2wGLETKBW3PvgPWqT","id":"4cOdK2wGLETKBW3PvgPWqT","artists":[{"name":"Rick Astley","uri":"spotify:artist:0gxyHStUsqpMadRV0Di1Qt"},{"name":"Guest Singer","uri":"spotify:artist:guest"}],"duration":213600,"isExplicit":false,"isPlayable":true,"coverArt":{"sources":[{"url":"https://i.scdn.co/image/ab67616d00001e02small","width":64,"height":64},{"url":"https://i.scdn.co/image/ab67616d0000b273big","width":640,"height":640}]},"album":{"type":"album","name":"Whenever You Need Somebody"}}}},"settings":{"theme":"dark"}}},"page":"/embed/track/[id]"}</script>
</body></html>)html";

// The same page after a rework: the attributes use single quotes, the title lives in
// "title", there is no album, cover or clock, and the artist is only a subtitle line.
const char kTrackEmbedMinimal[] = R"html(<html><body><script type='application/json' id='__NEXT_DATA__'>{"props":{"pageProps":{"state":{"data":{"entity":{"title":"Bare Track","uri":"spotify:track:4uLU6hMCjMI75M1A2tKUQC","subtitle":"Bare Artist"}}}}},"page":"/embed/track/[id]"}</script></body></html>)html";

const char kAlbumEmbed[] = R"html(<!doctype html>
<html><head><title>Spotify Embed</title></head><body>
<script id="__NEXT_DATA__" type="application/json">{"props":{"pageProps":{"state":{"data":{"entity":{"type":"album","name":"Test Album","uri":"spotify:album:0JGOiO34nwfUdDrD612dOp","subtitle":"Test Artist","trackCount":3,"coverArt":{"sources":[{"url":"https://i.scdn.co/image/ab67616d0000b273small","width":64,"height":64},{"url":"https://i.scdn.co/image/ab67616d0000b273set","width":640,"height":640}]},"trackList":[{"uri":"spotify:track:4uLU6hMCjMI75M1A2tKUQC","title":"Opening","subtitle":"Test Artist","duration":183000,"discNumber":1,"trackNumber":1},{"uri":"spotify:track:7GhIk7Il098yCjg4BQjzvb","title":"Closing","subtitle":"Test Artist feat. Guest","duration":201000,"discNumber":2,"trackNumber":1,"coverArt":{"sources":[{"url":"https://i.scdn.co/image/ab67616d0000b273row","width":300,"height":300}]}},{"uri":"spotify:track:1301WleyT98MSxVHPZCA6M","title":"Third","subtitle":"Test Artist","duration":99000,"discNumber":2,"trackNumber":2}]}}}}},"page":"/embed/album/[id]"}</script>
</body></html>)html";

const char kPlaylistEmbed[] = R"html(<!doctype html>
<html><head><title>Spotify Embed</title></head><body>
<script id="__NEXT_DATA__" type="application/json">{"props":{"pageProps":{"state":{"data":{"entity":{"type":"playlist","name":"Road Trip","uri":"spotify:playlist:37i9dQZF1DXcBWIGoYBM5M","subtitle":"Made by Someone","visualIdentity":{"image":[{"url":"https://i.scdn.co/image/ab67706f00000002plsmall","width":60,"height":60},{"url":"https://i.scdn.co/image/ab67706f00000002pl","width":300,"height":300}]},"trackList":[{"uri":"spotify:track:4uLU6hMCjMI75M1A2tKUQC","title":"Opening","subtitle":"Artist One","duration":180000},{"uri":"spotify:track:7GhIk7Il098yCjg4BQjzvb","title":"Closing","subtitle":"Artist Two","duration":61000}]}}}}},"page":"/embed/playlist/[id]"}</script>
</body></html>)html";

// The album page's server-rendered state: the base64 of a small album JSON, written out
// here as Spotify sends it.
const char kAlbumState[] =
    "<html><body><script id='initialState' type='text/plain'>"
    "eyJlbnRpdGllcyI6eyJpdGVtcyI6eyJzcG90aWZ5OmFsYnVtOjBKR09pTzM0bndmVWREckQ2MTJkT3AiOnsidHlwZSI6IkFMQlVN"
    "IiwibmFtZSI6IlRlc3QgQWxidW0iLCJhcnRpc3RzIjp7Iml0ZW1zIjpbeyJwcm9maWxlIjp7Im5hbWUiOiJUZXN0IEFydGlzdCJ9"
    "fV19LCJjb3ZlckFydCI6eyJzb3VyY2VzIjpbeyJ1cmwiOiJodHRwczovL2kuc2Nkbi5jby9pbWFnZS9hYjY3NjE2ZDAwMDBiMjcz"
    "c21hbGwiLCJ3aWR0aCI6NjQsImhlaWdodCI6NjR9LHsidXJsIjoiaHR0cHM6Ly9pLnNjZG4uY28vaW1hZ2UvYWI2NzYxNmQwMDAw"
    "YjI3M2xhcmdlIiwid2lkdGgiOjY0MCwiaGVpZ2h0Ijo2NDB9XX0sInRyYWNrc1YyIjp7InRvdGFsQ291bnQiOjMsIml0ZW1zIjpb"
    "eyJ0cmFjayI6eyJpZCI6IjR1TFU2aE1Dak1JNzVNMUEydEtVUUMiLCJuYW1lIjoiT3BlbmluZyIsImR1cmF0aW9uIjp7InRvdGFs"
    "TWlsbGlzZWNvbmRzIjoxODMwMDB9LCJ0cmFja051bWJlciI6MSwiZGlzY051bWJlciI6MSwiYXJ0aXN0cyI6eyJpdGVtcyI6W3si"
    "cHJvZmlsZSI6eyJuYW1lIjoiVGVzdCBBcnRpc3QifX1dfX19LHsidHJhY2siOnsiaWQiOiI3R2hJazdJbDA5OHlDamc0QlFqenZi"
    "IiwibmFtZSI6IkNsb3NpbmciLCJkdXJhdGlvbiI6eyJ0b3RhbE1pbGxpc2Vjb25kcyI6MjAxMDAwfSwidHJhY2tOdW1iZXIiOjEs"
    "ImRpc2NOdW1iZXIiOjIsImFydGlzdHMiOnsiaXRlbXMiOltdfX19LHsidHJhY2siOnsiaWQiOiIxMzAxV2xleVQ5OE1TeFZIUFpD"
    "QTZNIiwibmFtZSI6IlRoaXJkIiwiZHVyYXRpb24iOnsidG90YWxNaWxsaXNlY29uZHMiOjk5MDAwfSwidHJhY2tOdW1iZXIiOjIs"
    "ImRpc2NOdW1iZXIiOjIsImFydGlzdHMiOnsiaXRlbXMiOltdfX19XX19fX19"
    "</script></body></html>";

yt::Candidate Candidate(const char* id, const char* title, const char* channel, unsigned durationSec,
                       bool topic = false) {
    yt::Candidate candidate;
    candidate.id = id;
    candidate.title = title;
    candidate.channel = channel;
    candidate.durationSec = durationSec;
    candidate.topic = topic;
    return candidate;
}

WebTrack Want(const char* artist, const char* title, unsigned durationSec) {
    WebTrack want;
    want.site = "spotify";
    want.artist = artist;
    want.title = title;
    want.durationSec = durationSec;
    return want;
}

}  // namespace

void TestSpotifyLink() {
    testlog::Batch batch;
    batch.Next(L"Spotify links are classified by their page, with or without a locale segment and query "
               L"noise, and everything else is refused");
    testlog::Cases links;
    links.Add(spotify::Classify("https://open.spotify.com/track/4cOdK2wGLETKBW3PvgPWqT") == spotify::Kind::Track,
              L"track");
    links.Add(spotify::Classify("https://open.spotify.com/intl-de/album/0JGOiO34nwfUdDrD612dOp?si=abc123") ==
                  spotify::Kind::Album,
              L"album with a locale and ?si=");
    links.Add(spotify::Classify("open.spotify.com/playlist/37i9dQZF1DXcBWIGoYBM5M?si=q#t=10") ==
                  spotify::Kind::Playlist,
              L"playlist without a scheme");
    links.Add(spotify::Classify(" http://OPEN.SPOTIFY.COM/intl-EN/track/4uLU6hMCjMI75M1A2tKUQC/ ") ==
                  spotify::Kind::Track,
              L"uppercase host and a trailing slash");
    links.Add(spotify::Classify("https://open.spotify.com/track/4cOdK2wGLETKBW3PvgPWqT/extra") ==
                  spotify::Kind::None,
              L"an id followed by another path segment");
    links.Add(spotify::Classify("https://open.spotify.com/embed/track/4cOdK2wGLETKBW3PvgPWqT") ==
                  spotify::Kind::None,
              L"an embed page");
    links.Add(spotify::Classify("spotify:track:4cOdK2wGLETKBW3PvgPWqT") == spotify::Kind::None, L"a spotify: URI");
    links.Add(spotify::Classify("https://example.com/track/4cOdK2wGLETKBW3PvgPWqT") == spotify::Kind::None,
              L"another site");
    links.Add(spotify::Classify("https://open.spotify.com/user/someone") == spotify::Kind::None, L"a profile");
    links.Add(spotify::Classify("https://open.spotify.com/track/short") == spotify::Kind::None, L"a short id");
    links.Add(spotify::Classify("") == spotify::Kind::None, L"an empty link");
    Check(links.Passed(), L"Spotify links are classified by their page, with or without a locale segment and "
                          L"query noise, and everything else is refused%ls",
          links.failed.c_str());

    batch.Next(L"a track embed's __NEXT_DATA__ names the recording, its first artist, album, cover and clock");
    WebTrack single;
    bool parsed = spotify::ParseTrackEmbed(kTrackEmbed, single);
    Check(parsed && single.site == "spotify" && single.id == "4cOdK2wGLETKBW3PvgPWqT" &&
              single.title == "Never Gonna Give You Up" && single.artist == "Rick Astley" &&
              single.album == "Whenever You Need Somebody" &&
              single.artworkUrl == "https://i.scdn.co/image/ab67616d0000b273big" && single.durationSec == 214 &&
              single.streamUrl.empty() && single.ext.empty() && !single.hlsOnly,
          L"a track embed names the recording, its first artist, album, largest cover and rounded clock");

    batch.Next(L"a changed track embed still yields what it does name, and a page without __NEXT_DATA__ fails");
    WebTrack bare;
    Check(spotify::ParseTrackEmbed(kTrackEmbedMinimal, bare) && bare.id == "4uLU6hMCjMI75M1A2tKUQC" &&
              bare.title == "Bare Track" && bare.artist == "Bare Artist" && bare.album.empty() &&
              bare.artworkUrl.empty() && bare.durationSec == 0 && bare.ext.empty(),
          L"a track embed without an album, cover or clock still names the track and artist");
    WebTrack untouched;
    Check(!spotify::ParseTrackEmbed("<html><body><h1>Not Spotify</h1></body></html>", untouched) &&
              !spotify::ParseTrackEmbed(R"html(<script id="__NEXT_DATA__">{"props":{}}</script>)html", untouched),
          L"a page without __NEXT_DATA__ or without an entity is a parse failure, not a crash");

    batch.Next(L"an album embed keeps disc and track numbers, numbers a multi-disc set across the album, and "
               L"gives every row the set's cover when a row has none");
    std::vector<WebTrack> list;
    std::string setTitle;
    parsed = spotify::ParseListEmbed(kAlbumEmbed, list, setTitle);
    Check(parsed && setTitle == "Test Album" && list.size() == 3 &&
              list[0].id == "4uLU6hMCjMI75M1A2tKUQC" && list[1].id == "7GhIk7Il098yCjg4BQjzvb" &&
              list[2].id == "1301WleyT98MSxVHPZCA6M" && list[0].title == "Opening" &&
              list[1].title == "Closing" && list[1].artist == "Test Artist" &&
              list[0].durationSec == 183 && list[1].durationSec == 201 && list[2].durationSec == 99 &&
              list[0].trackNo == 1 && list[1].trackNo == 2 && list[2].trackNo == 3 &&
              list[0].trackTotal == 3 && list[2].trackTotal == 3 && list[0].artworkUrl.ends_with("273set") &&
              list[1].artworkUrl.ends_with("273row") && list[2].artworkUrl.ends_with("273set") &&
              list[1].ext.empty(),
          L"an album embed keeps titles, durations and folded numbering, and files every row under the "
          L"album's own artist while the title carries the guests");

    batch.Next(L"a playlist embed keeps the served order, the owner out of the artist field, and leaves the "
               L"numbering to the queue");
    std::vector<WebTrack> playlist;
    std::string playlistTitle;
    parsed = spotify::ParseListEmbed(kPlaylistEmbed, playlist, playlistTitle);
    Check(parsed && playlistTitle == "Road Trip" && playlist.size() == 2 &&
              playlist[0].id == "4uLU6hMCjMI75M1A2tKUQC" && playlist[1].title == "Closing" &&
              playlist[0].artist == "Artist One" && playlist[1].artist == "Artist Two" &&
              playlist[0].album == "Road Trip" && playlist[0].durationSec == 180 && playlist[1].durationSec == 61 &&
              playlist[0].trackNo == 0 && playlist[1].trackNo == 0 && playlist[0].trackTotal == 0 &&
              playlist[0].artworkUrl.ends_with("02pl") && playlist[1].artworkUrl.ends_with("02pl"),
          L"a playlist embed keeps the order, row artists, its cover, and zero numbering");

    batch.Next(L"the album page's base64 initialState gives the real ids, durations and disc numbers, and a "
               L"page without it is a parse failure");
    std::vector<WebTrack> album;
    std::string albumTitle;
    parsed = spotify::ParseAlbumState(kAlbumState, album, albumTitle);
    Check(parsed && albumTitle == "Test Album" && album.size() == 3 &&
              album[0].id == "4uLU6hMCjMI75M1A2tKUQC" && album[1].id == "7GhIk7Il098yCjg4BQjzvb" &&
              album[2].id == "1301WleyT98MSxVHPZCA6M" && album[0].title == "Opening" &&
              album[1].title == "Closing" && album[2].title == "Third" && album[0].artist == "Test Artist" &&
              album[1].artist == "Test Artist" && album[2].artist == "Test Artist" &&
              album[0].durationSec == 183 && album[1].durationSec == 201 && album[2].durationSec == 99 &&
              album[0].trackNo == 1 && album[1].trackNo == 2 && album[2].trackNo == 3 &&
              album[1].trackTotal == 3 && album[0].artworkUrl.ends_with("273large"),
          L"initialState yields the real ids, sorted tracks, album artist fallback and disc-aware numbering");
    std::vector<WebTrack> none;
    std::string noneTitle;
    Check(!spotify::ParseAlbumState("<html><body><h1>No state here</h1></body></html>", none, noneTitle) &&
              none.empty(),
          L"an album page without initialState falls through to an empty set");

    batch.Next(L"the query ladder keeps the decorated title first, then the plain one with and without the artist, "
               L"dropping duplicates and starting at the plain title when there is no artist");
    std::vector<std::string> plain = spotify::BuildQueries(Want("Rick Astley", "Never Gonna Give You Up", 213));
    Check(plain.size() == 2 && plain[0] == "Rick Astley - Never Gonna Give You Up" &&
              plain[1] == "Never Gonna Give You Up",
          L"an undecorated track asks for the artist and title, then the title alone");
    std::vector<std::string> decorated = spotify::BuildQueries(
        Want("Rick Astley", "Never Gonna Give You Up (Official Video) [Live] {HD}", 213));
    Check(decorated.size() == 3 && decorated[0] == "Rick Astley - Never Gonna Give You Up (Official Video) [Live] {HD}" &&
              decorated[1] == "Rick Astley - Never Gonna Give You Up" &&
              decorated[2] == "Never Gonna Give You Up",
          L"a decorated track tries the full title, the stripped title, then the bare title");
    std::vector<std::string> trailing =
        spotify::BuildQueries(Want("Artist", "Song (feat. Guest) - Remastered 2011", 200));
    Check(trailing.size() == 3 && trailing[1] == "Artist - Song", L"feat. and a trailing remaster are decoration");
    std::vector<std::string> episode =
        spotify::BuildQueries(Want("", "Episode One (Official Audio)", 1800));
    Check(episode.size() == 2 && episode[0] == "Episode One" && episode[1] == "Episode One (Official Audio)",
          L"without an artist the ladder starts at the stripped title");

    batch.Next(L"the matcher puts an exact Topic upload first, an official upload of the same recording next, and "
               L"a cover last");
    std::vector<yt::Candidate> found = {
        Candidate("a", "Song", "Artist - Topic", 180, true),
        Candidate("b", "Artist - Song (Official Audio)", "Artist", 181),
        Candidate("c", "Song (Cover)", "Cover Band", 180),
    };
    std::string why = "stale";
    std::vector<int> order = spotify::RankMatches(Want("Artist", "Song", 180), found, why);
    Check(order.size() == 3 && order[0] == 0 && order[1] == 1 && order[2] == 2 && why.empty(),
          L"a Topic upload outranks an official video of the song, which outranks a cover");

    batch.Next(L"a candidate outside two percent of the clock is left out and the closest upload's length is "
               L"named in the reason");
    std::vector<yt::Candidate> different = {Candidate("a", "Song", "Artist - Topic", 252, true)};
    why.clear();
    order = spotify::RankMatches(Want("Artist", "Song", 180), different, why);
    Check(order.empty() && why == "The closest YouTube upload is 4:12, which is not the same recording.",
          L"a four-minute upload is excluded from a three-minute song with its length in the reason");

    batch.Next(L"the two percent rule keeps a recording a few seconds either side and empty results say so");
    std::vector<yt::Candidate> within = {Candidate("a", "Song", "Artist - Topic", 183, true)};
    std::vector<yt::Candidate> outside = {Candidate("a", "Song", "Artist - Topic", 184, true)};
    why.clear();
    std::vector<int> withinOrder = spotify::RankMatches(Want("Artist", "Song", 180), within, why);
    std::vector<int> outsideOrder = spotify::RankMatches(Want("Artist", "Song", 180), outside, why);
    std::vector<yt::Candidate> nothing;
    why.clear();
    std::vector<int> noneOrder = spotify::RankMatches(Want("Artist", "Song", 180), nothing, why);
    Check(withinOrder.size() == 1 && outsideOrder.empty() && noneOrder.empty() && !why.empty(),
          L"two percent keeps 183 s and drops 184 s, and no search rows leave a reason");

    batch.Next(L"a search row without a clock is kept but ranks behind every row that can be checked now");
    std::vector<yt::Candidate> unclocked = {
        Candidate("a", "Song", "Artist - Topic", 0, true),
        Candidate("b", "Song (Official Audio)", "Indie Label", 180),
    };
    why.clear();
    order = spotify::RankMatches(Want("Artist", "Song", 180), unclocked, why);
    Check(order.size() == 2 && order[0] == 1 && order[1] == 0 && why.empty(),
          L"a clocked official upload comes before a better-scoring row with no duration");

    batch.Next(L"an episode only ranks uploads that name it, and a different programme with one shared word "
               L"does not count");
    std::vector<yt::Candidate> show = {
        Candidate("a", "Episode 42 Highlights", "Radio Clips", 1800),
        Candidate("b", "The Daily: Episode 42", "The Daily", 1795),
    };
    why.clear();
    order = spotify::RankMatches(Want("", "The Daily: Episode 42", 1800), show, why);
    std::vector<yt::Candidate> otherShow = {Candidate("a", "Something Else Entirely", "Radio Clips", 1800)};
    std::string episodeWhy;
    std::vector<int> otherOrder = spotify::RankMatches(Want("", "The Daily: Episode 42", 1800), otherShow, episodeWhy);
    Check(order.size() == 1 && order[0] == 1 && otherOrder.empty() &&
              episodeWhy == "No YouTube upload names that episode.",
          L"only an upload naming the episode ranks, and none naming it leaves an episode-specific reason");

    batch.Next(L"a lyric channel loses to the artist's own Topic upload even when both name the song");
    std::vector<yt::Candidate> lyric = {
        Candidate("a", "Song (Lyric Video)", "Lyric Channel", 180),
        Candidate("b", "Song", "Artist - Topic", 180, true),
    };
    why.clear();
    order = spotify::RankMatches(Want("Artist", "Song", 180), lyric, why);
    Check(order.size() == 2 && order[0] == 1 && order[1] == 0, L"the Topic upload comes first, the lyric upload second");
}
