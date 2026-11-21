// SoundCloud fixtures (soundcloud.h): link classification, api-v2 parsing and the addresses
// Resolve builds, all checked without the network.
#include "selftest_common.h"
#include "soundcloud.h"

#include <string>
#include <string_view>
#include <vector>

// Resolve's pure request builders live beside it in soundcloud.cpp; the contract header leaves
// them out, and these declarations are how this suite pins their spelling down.
namespace soundcloud::detail {
std::string UrlEncode(std::string_view text);
std::string WithClientId(std::string url, std::string_view clientId);
std::string ResolveUrl(std::string_view permalink, std::string_view clientId);
std::string LikesUrl(std::string_view userId, std::string_view clientId);
std::string ArtistTracksUrl(std::string_view userId, std::string_view clientId);
std::string SetTracksUrl(std::string_view setId, std::string_view clientId, size_t offset);
std::vector<std::string> AssetScripts(std::string_view html);
bool FindClientId(std::string_view text, std::string& id);
bool FindWidgetClientId(std::string_view text, std::string& id);
bool ParseHlsVariant(std::string_view playlist, std::string_view baseUrl, std::string& url);
bool ParseHlsSegments(std::string_view playlist, std::string_view baseUrl, std::string& initUrl,
                      std::vector<std::string>& segments);
}  // namespace soundcloud::detail

void TestSoundCloud() {
    using soundcloud::Kind;

    // One row per shape of address the saving box may be given.
    struct Row {
        const char* url;
        Kind kind;
        const wchar_t* name;
    };
    const Row rows[] = {
        {"https://soundcloud.com/night-lines/blue-hour", Kind::Track, L"a two-part permalink is a track"},
        {"http://soundcloud.com/night-lines/blue-hour/", Kind::Track, L"http and a trailing slash still work"},
        {"https://soundcloud.com/night-lines/blue-hour?si=share", Kind::Track, L"a share query is ignored"},
        {"soundcloud.com/night-lines/blue-hour", Kind::Track, L"a bare host is accepted"},
        {"https://www.soundcloud.com/night-lines/blue-hour", Kind::Track, L"www. is accepted"},
        {"https://m.soundcloud.com/night-lines/blue-hour", Kind::Track, L"the mobile site is accepted"},
        {"https://m.soundcloud.com/night-lines/likes", Kind::Likes, L"mobile likes are likes"},
        {"https://soundcloud.com/night-lines/likes", Kind::Likes, L"likes are a list"},
        {"https://soundcloud.com/night-lines/tracks", Kind::ArtistTracks, L"an artist's tracks page is a list"},
        {"https://soundcloud.com/night-lines", Kind::None, L"a bare artist page is not a list"},
        {"https://soundcloud.com/night-lines/", Kind::None, L"a trailing slash stays a bare artist page"},
        {"https://soundcloud.com/night-lines/sets/night-drive", Kind::Playlist, L"an artist's set is a playlist"},
        {"https://soundcloud.com/night-lines/playlists/night-drive", Kind::Playlist,
         L"an artist's playlists link is a playlist"},
        {"https://soundcloud.com/sets/night-drive", Kind::Playlist, L"a bare set path is a playlist"},
        {"https://soundcloud.com/playlists/night-drive", Kind::Playlist, L"a bare playlists path is a playlist"},
        {"https://soundcloud.com/sets", Kind::None, L"the sets hub is not a playlist"},
        {"https://soundcloud.com/night-lines/sets", Kind::None, L"an artist's sets hub is not a playlist"},
        {"https://soundcloud.com/you/likes", Kind::None, L"the you page is reserved"},
        {"https://soundcloud.com/you/tracks", Kind::None, L"the you tracks page is reserved"},
        {"https://soundcloud.com/stream", Kind::None, L"the stream is reserved"},
        {"https://soundcloud.com/charts/top", Kind::None, L"charts are reserved"},
        {"https://soundcloud.com/discover", Kind::None, L"discover is reserved"},
        {"https://soundcloud.com/discover/new", Kind::None, L"discover rows are reserved"},
        {"https://soundcloud.com/upload", Kind::None, L"upload is reserved"},
        {"https://soundcloud.com/popular", Kind::None, L"popular is reserved"},
        {"https://soundcloud.com/stations", Kind::None, L"stations are reserved"},
        {"https://soundcloud.com/night-lines/stream", Kind::None, L"a reserved second segment is not a track"},
        {"https://soundcloud.com/night-lines/popular", Kind::None, L"another reserved second segment"},
        {"https://soundcloud.com/likes/night-lines", Kind::None, L"a reserved first segment is not a track"},
        {"https://soundcloud.com/tracks/1234", Kind::None, L"a numeric tracks path is not a track"},
        {"https://soundcloud.com/night-lines/blue-hour/comments", Kind::None,
         L"a deeper path is neither a track nor a set"},
        {"https://on.soundcloud.com/AbCd1234", Kind::Track, L"a short link is guessed as a track"},
        {"https://on.soundcloud.com", Kind::None, L"the short-link host alone is nothing"},
        {"https://example.com/night-lines/blue-hour", Kind::None, L"another site is not SoundCloud"},
        {"https://soundcloud.com/", Kind::None, L"the front page is nothing"},
    };
    testlog::Cases links;
    for (const Row& row : rows) links.Add(soundcloud::Classify(row.url) == row.kind, row.name);
    Check(links.Passed(),
          L"SoundCloud links classify as tracks, sets, likes and artist pages, and reserved words stay hubs%ls",
          links.failed.c_str());

    const std::string track = R"json({
        "kind": "track",
        "id": 123456789,
        "title": "Blue Hour",
        "duration": 215340,
        "artwork_url": "https://i1.sndcdn.com/artworks-000123456789-abcd12-large.jpg",
        "policy": "ALLOW",
        "streamable": true,
        "snipped": false,
        "downloadable": false,
        "user": {"kind": "user", "id": 42, "username": "Night Lines",
                 "avatar_url": "https://i1.sndcdn.com/avatars-000000000042-xyz-large.jpg"},
        "media": {"transcodings": [
            {"url": "https://api-v2.soundcloud.com/media/soundcloud:tracks:123456789/hls.m3u8",
             "preset": "mp3_0_1", "format": {"protocol": "hls", "mime_type": "audio/mpegurl"}},
            {"url": "https://api-v2.soundcloud.com/media/soundcloud:tracks:123456789/aac.128.mp3",
             "preset": "mp3_0_1", "format": {"protocol": "progressive", "mime_type": "audio/aac"}},
            {"url": "https://api-v2.soundcloud.com/media/soundcloud:tracks:123456789/mp3.128.mp3",
             "preset": "mp3_0_1", "format": {"protocol": "progressive", "mime_type": "audio/mpeg"}}
        ]}
    })json";
    testlog::Cases parsedTracks;
    WebTrack one;
    const bool progressive = soundcloud::ParseTrack(track, one);
    parsedTracks.Add(progressive && one.site == "soundcloud" && one.id == "123456789" &&
                         one.title == "Blue Hour" && one.artist == "Night Lines" && one.durationSec == 215 &&
                         one.album.empty() && !one.hlsOnly && !one.unavailable && one.why.empty(),
                     L"a progressive track keeps its name, artist, length and site");
    parsedTracks.Add(one.streamUrl ==
                             "https://api-v2.soundcloud.com/media/soundcloud:tracks:123456789/mp3.128.mp3" &&
                         one.ext == "mp3",
                     L"the audio/mpeg progressive stream is preferred and called mp3");
    parsedTracks.Add(one.artworkUrl == "https://i1.sndcdn.com/artworks-000123456789-abcd12-t500x500.jpg",
                     L"the artwork's -large token becomes the big JPEG");

    // The api usually nests protocol and mime type under "format"; a flat spelling is tolerated too.
    const std::string flat = R"json({"kind": "track", "id": 7, "title": "Flat", "duration": 1000,
        "user": {"username": "Someone"},
        "media": {"transcodings": [{"url": "https://cf-media.example/flat.mp3",
                                    "protocol": "progressive", "mime_type": "audio/mpeg"}]}})json";
    WebTrack flatTrack;
    parsedTracks.Add(soundcloud::ParseTrack(flat, flatTrack) &&
                         flatTrack.streamUrl == "https://cf-media.example/flat.mp3" && flatTrack.ext == "mp3",
                     L"a flat transcoding spelling is read the same way");

    const std::string hlsOnly = R"json({"kind": "track", "id": 8, "title": "Live take", "duration": 60000,
        "user": {"username": "Night Lines"},
        "media": {"transcodings": [{"url": "https://api-v2.soundcloud.com/media/8/hls.m3u8",
                                    "format": {"protocol": "hls", "mime_type": "audio/mpegurl"}}]}})json";
    WebTrack hls;
    parsedTracks.Add(soundcloud::ParseTrack(hlsOnly, hls) && hls.hlsOnly && !hls.unavailable &&
                         hls.streamUrl.empty() && hls.ext.empty(),
                     L"an HLS-only track is marked hlsOnly and has no stream to save");

    const std::string missingStreams = R"json({"kind": "track", "id": 9, "title": "Gone",
        "user": {"username": "Night Lines"}, "media": {"transcodings": []}})json";
    WebTrack missing;
    parsedTracks.Add(soundcloud::ParseTrack(missingStreams, missing) && missing.unavailable &&
                         missing.why == "SoundCloud has no playable stream for this track" &&
                         missing.streamUrl.empty() && missing.ext.empty(),
                     L"a track with no transcodings is unavailable with a reason");

    const std::string notStreamable = R"json({"kind": "track", "id": 10, "title": "Blocked",
        "policy": "NOT_STREAMABLE", "user": {"username": "Night Lines"},
        "media": {"transcodings": [{"url": "https://cf-media.example/blocked.mp3",
                                    "format": {"protocol": "progressive", "mime_type": "audio/mpeg"}}]}})json";
    WebTrack blocked;
    parsedTracks.Add(soundcloud::ParseTrack(notStreamable, blocked) && blocked.unavailable &&
                         blocked.why == "this track is no longer streamable on SoundCloud" &&
                         blocked.streamUrl.empty() && blocked.ext.empty(),
                     L"a NOT_STREAMABLE policy makes the track unavailable");

    const std::string policyBlock = R"json({"kind": "track", "id": 11, "title": "Blocked twice",
        "policy": "BLOCK", "user": {"username": "Night Lines"}})json";
    WebTrack blockedTwice;
    parsedTracks.Add(soundcloud::ParseTrack(policyBlock, blockedTwice) && blockedTwice.unavailable &&
                         blockedTwice.why == "this track is no longer streamable on SoundCloud",
                     L"a BLOCK policy says the same thing");

    const std::string snipped = R"json({"kind": "track", "id": 12, "title": "Preview",
        "snipped": true, "user": {"username": "Night Lines"},
        "media": {"transcodings": [{"url": "https://cf-media.example/snip.mp3",
                                    "format": {"protocol": "progressive", "mime_type": "audio/mpeg"}}]}})json";
    WebTrack preview;
    parsedTracks.Add(soundcloud::ParseTrack(snipped, preview) && preview.unavailable &&
                         preview.why == "SoundCloud only allows a preview of this track in your region" &&
                         preview.streamUrl.empty(),
                     L"a snipped track reports the regional preview limit");

    const std::string notStreamableFlag = R"json({"kind": "track", "id": 13, "title": "Private",
        "streamable": false, "user": {"username": "Night Lines"}})json";
    WebTrack notStreamableTrack;
    parsedTracks.Add(soundcloud::ParseTrack(notStreamableFlag, notStreamableTrack) &&
                         notStreamableTrack.unavailable &&
                         notStreamableTrack.why == "this track is no longer streamable on SoundCloud",
                     L"streamable false reads as no longer streamable");

    const std::string avatarOnly = R"json({"kind": "track", "id": 14, "title": "Avatar art",
        "artwork_url": null, "duration": 9000,
        "user": {"username": "Night Lines",
                 "avatar_url": "https://i1.sndcdn.com/avatars-000000000014-aaaa-large.jpg"},
        "media": {"transcodings": [{"url": "https://cf-media.example/avatar.mp3",
                                    "format": {"protocol": "progressive", "mime_type": "audio/mpeg"}}]}})json";
    WebTrack avatar;
    parsedTracks.Add(soundcloud::ParseTrack(avatarOnly, avatar) &&
                         avatar.artworkUrl == "https://i1.sndcdn.com/avatars-000000000014-aaaa-t500x500.jpg",
                     L"missing artwork falls back to the artist's avatar, enlarged");

    const std::string noArt = R"json({"kind": "track", "id": 15, "title": "No art", "artwork_url": null,
        "user": {"username": "Night Lines", "avatar_url": null}})json";
    WebTrack bare;
    parsedTracks.Add(soundcloud::ParseTrack(noArt, bare) && bare.artworkUrl.empty(),
                     L"a track with neither artwork nor avatar keeps an empty artwork address");

    WebTrack untouched;
    untouched.title = "keep";
    parsedTracks.Add(!soundcloud::ParseTrack("not json at all", untouched) && untouched.title == "keep",
                     L"torn JSON is refused without changing the caller's track");
    const std::string userPage = R"json({"kind": "user", "id": 42, "username": "Night Lines"})json";
    parsedTracks.Add(!soundcloud::ParseTrack(userPage, one), L"a user object is not a track");
    Check(parsedTracks.Passed(), L"api-v2 tracks parse into titles, artists, streams and unavailability%ls",
          parsedTracks.failed.c_str());

    // Every shape a list link answers with, plus the torn entries a dropped connection can leave.
    const std::string set = R"json({"kind": "playlist", "id": 900, "title": "Night Drive", "track_count": 2,
        "tracks": [
            {"kind": "track", "id": 21, "title": "Blue Hour", "duration": 215000,
             "user": {"username": "Night Lines"}},
            {"kind": "track", "id": 22, "title": "Second Wind", "duration": 180000,
             "user": {"username": "Night Lines"}}
        ]})json";
    const std::string array = R"json([
        {"kind": "track", "id": 31, "title": "First", "user": {"username": "Night Lines"}},
        {"kind": "track", "id": 32, "title": "Last", "user": {"username": "Night Lines"}}
    ])json";
    const std::string likes = R"json({"collection": [
        {"track": {"kind": "track", "id": 41, "title": "Liked", "user": {"username": "Someone"}},
         "created_at": "2026-11-01T00:00:00Z"},
        {"playlist": {"kind": "playlist", "id": 42, "title": "A liked set"}},
        {"track": null}
    ], "next_href": "https://api-v2.soundcloud.com/users/42/likes?limit=50&offset=1"})json";
    const std::string artistTracks = R"json({"collection": [
        {"kind": "track", "id": 51, "title": "Posted", "user": {"username": "Night Lines"}},
        {"kind": "track", "id": 52, "title": "Posted later", "user": {"username": "Night Lines"}}
    ], "next_href": null})json";
    const std::string torn = R"json({"collection": [
        {"track": {"kind": "track", "id": 0, "title": "Placeholder"}},
        {"track": {"kind": "track", "title": "No id"}},
        {"track": {"kind": "track", "id": 61, "title": "Real", "user": {"username": "Night Lines"}}}
    ]})json";

    testlog::Cases lists;
    std::vector<WebTrack> tracks;
    size_t count = soundcloud::ParseTrackList(set, tracks);
    lists.Add(count == 2 && tracks.size() == 2 && tracks[0].title == "Blue Hour" &&
                  tracks[1].id == "22" && tracks[0].site == "soundcloud",
              L"a set's inline tracks are listed in order");
    count = soundcloud::ParseTrackList(array, tracks);
    lists.Add(count == 2 && tracks.size() == 2 && tracks[0].id == "31" && tracks[1].title == "Last",
              L"a plain array of tracks is listed");
    count = soundcloud::ParseTrackList(likes, tracks);
    lists.Add(count == 1 && tracks.size() == 1 && tracks[0].id == "41" && tracks[0].title == "Liked",
              L"a likes page keeps its tracks and drops the liked set and null entries");
    count = soundcloud::ParseTrackList(artistTracks, tracks);
    lists.Add(count == 2 && tracks.size() == 2 && tracks[1].id == "52",
              L"an artist's tracks page lists its collection");
    count = soundcloud::ParseTrackList(torn, tracks);
    lists.Add(count == 1 && tracks.size() == 1 && tracks[0].id == "61",
              L"entries with a missing or zero id are skipped");
    count = soundcloud::ParseTrackList("[]", tracks);
    lists.Add(count == 0 && tracks.empty(), L"an empty list replaces earlier results with nothing");
    count = soundcloud::ParseTrackList("{\"id\": 1}", tracks);
    lists.Add(count == 0 && tracks.empty(), L"an object with no list gives no tracks");
    count = soundcloud::ParseTrackList("torn", tracks);
    lists.Add(count == 0 && tracks.empty(), L"unreadable JSON gives no tracks");
    Check(lists.Passed(), L"playlists, likes pages, artist pages and plain arrays all fill WebTracks%ls",
          lists.failed.c_str());

    testlog::Cases names;
    std::string title = "kept";
    names.Add(soundcloud::ParseSetTitle(set, title) && title == "Night Drive", L"a set names itself");
    title = "kept";
    names.Add(!soundcloud::ParseSetTitle(R"json({"kind": "playlist", "id": 900})json", title) &&
                  title == "kept",
              L"a set without a title leaves the caller's name alone");
    names.Add(!soundcloud::ParseSetTitle("nonsense", title), L"unreadable JSON has no title");
    Check(names.Passed(), L"a set's own title is read only when it states one%ls", names.failed.c_str());

    // The request builders are pure, so their spelling is pinned here instead of on the wire.
    testlog::Cases urls;
    urls.Add(soundcloud::detail::UrlEncode("a b/c&d=e?f") == "a%20b%2Fc%26d%3De%3Ff", L"query text is escaped");
    urls.Add(soundcloud::detail::ResolveUrl("https://soundcloud.com/night-lines/blue-hour", "key123") ==
                 "https://api-v2.soundcloud.com/resolve?url=https%3A%2F%2Fsoundcloud.com%2Fnight-lines%2F"
                 "blue-hour&client_id=key123",
             L"a permalink is resolved through api-v2 with its key");
    urls.Add(soundcloud::detail::LikesUrl("42", "key123") ==
                 "https://api-v2.soundcloud.com/users/42/likes?limit=50&linked_partitioning=1&client_id=key123",
             L"a likes page asks for 50 at a time");
    urls.Add(soundcloud::detail::ArtistTracksUrl("42", "key123") ==
                 "https://api-v2.soundcloud.com/users/42/tracks?limit=50&linked_partitioning=1&client_id=key123",
             L"an artist's tracks page asks for 50 at a time");
    urls.Add(soundcloud::detail::SetTracksUrl("900", "key123", 100) ==
                 "https://api-v2.soundcloud.com/playlists/900/tracks?limit=50&offset=100&client_id=key123",
             L"a set's missing tracks are paged by offset");
    urls.Add(soundcloud::detail::WithClientId("https://api-v2.soundcloud.com/users/42/tracks?limit=50&client_id=old",
                                              "new") ==
                 "https://api-v2.soundcloud.com/users/42/tracks?limit=50&client_id=new",
             L"a stale key on a next page is replaced, not doubled");
    urls.Add(soundcloud::detail::WithClientId("https://cf-media.sndcdn.com/a.mp3?Policy=x&Signature=y", "new") ==
                 "https://cf-media.sndcdn.com/a.mp3?Policy=x&Signature=y&client_id=new",
             L"a media URL gets the key beside its signature");
    urls.Add(soundcloud::detail::WithClientId("https://api-v2.soundcloud.com/media/1/stream", "new") ==
                 "https://api-v2.soundcloud.com/media/1/stream?client_id=new",
             L"a media URL with no query gets one");
    Check(urls.Passed(), L"resolve, likes, tracks and set-page addresses keep their exact query spelling%ls",
          urls.failed.c_str());

    // Client-id discovery, also pure once the page and scripts are in hand.
    testlog::Cases discovery;
    const std::string plainKey = "{\"client_id\":\"" + std::string(32, 'k') + "\"}";
    std::string key;
    discovery.Add(soundcloud::detail::FindClientId(plainKey, key) && key == std::string(32, 'k'),
                  L"a plain JSON key is found");
    key.clear();
    discovery.Add(soundcloud::detail::FindClientId(
                      R"js({"client_id":"short","other":{"client_id":"a1b2c3d4e5f6g7h8i9j0k1l2m3n4o5p6"}})js", key) &&
                      key == "a1b2c3d4e5f6g7h8i9j0k1l2m3n4o5p6",
                  L"a short lookalike is skipped for the real 32-character key");
    key.clear();
    discovery.Add(soundcloud::detail::FindClientId(
                      R"js(window.__sc={"client_id\":\"a1b2c3d4e5f6g7h8i9j0k1l2m3n4o5p6\"})js", key) &&
                      key == "a1b2c3d4e5f6g7h8i9j0k1l2m3n4o5p6",
                  L"an escaped key inside a script string is found");
    const std::string html = R"html(
        <script crossorigin src="https://a-v2.sndcdn.com/assets/49-abc123.js"></script>
        <script crossorigin src="//a-v2.sndcdn.com/assets/50-def456.js"></script>
        <script src="https://a-v2.sndcdn.com/assets/49-abc123.js"></script>
        <link rel="icon" href="https://a-v2.sndcdn.com/assets/favicon.png">
    )html";
    const std::vector<std::string> scripts = soundcloud::detail::AssetScripts(html);
    discovery.Add(scripts.size() == 2 && scripts[0] == "https://a-v2.sndcdn.com/assets/49-abc123.js" &&
                      scripts[1] == "https://a-v2.sndcdn.com/assets/50-def456.js",
                  L"asset scripts are absolute, in page order and not duplicated");
    discovery.Add(soundcloud::detail::AssetScripts("<html>no scripts</html>").empty(),
                  L"a page with no asset scripts names none");
    Check(discovery.Passed(), L"the site's asset scripts and 32-character key are read from page text%ls",
          discovery.failed.c_str());

    {
        testlog::Cases widget;
        std::string widgetKey;
        widget.Add(soundcloud::detail::FindWidgetClientId(
                       R"js(client_application_id:a?90575:43164,client_id:a?"nIjtjiYnjkOhMyh5xrbqEW12DxeJVnic":"gqKBMSuBw5rbN9rDRYPqKNvF17ovlObu",next:"value")js",
                       widgetKey) &&
                       widgetKey == "gqKBMSuBw5rbN9rDRYPqKNvF17ovlObu",
                   L"the last key of the widget pair is the anonymous one");
        widget.Add(soundcloud::detail::FindWidgetClientId(R"js(client_id:"a1b2c3d4e5f6g7h8i9j0k1l2m3n4o5p6")js",
                                                          widgetKey) &&
                       widgetKey == "a1b2c3d4e5f6g7h8i9j0k1l2m3n4o5p6",
                   L"a plain quoted key is still found");
        widget.Add(!soundcloud::detail::FindWidgetClientId("client_id without any quoted value here", widgetKey),
                   L"text without a quoted key answers no key");
        std::string variant;
        widget.Add(soundcloud::detail::ParseHlsVariant(
                       "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=160000,CODECS=\"mp4a.40.2\"\nplaylist_160k.m3u8\n",
                       "https://playback.example/aac_160k/x/playlist.m3u8", variant) &&
                       variant == "https://playback.example/aac_160k/x/playlist_160k.m3u8",
                   L"a master playlist's first variant is made absolute");
        std::string init;
        std::vector<std::string> segments;
        widget.Add(soundcloud::detail::ParseHlsSegments(
                       "#EXTM3U\n#EXT-X-MAP:URI=\"https://playback.example/init.mp4?token=1\"\n"
                       "#EXTINF:10.0,\ndata000.m4s?token=1\n#EXTINF:10.0,\ndata001.m4s?token=1\n",
                       "https://playback.example/x/playlist.m3u8", init, segments) &&
                       init == "https://playback.example/init.mp4?token=1" && segments.size() == 2 &&
                       segments[0] == "https://playback.example/x/data000.m4s?token=1" &&
                       segments[1] == "https://playback.example/x/data001.m4s?token=1",
                   L"a media playlist gives its init and its segments, absolute");
        widget.Add(!soundcloud::detail::ParseHlsSegments("#EXTM3U\n#EXTINF:10,\nseg001.ts\n",
                                                         "https://playback.example/x/playlist.m3u8", init,
                                                         segments),
                   L"a playlist without an init section cannot be joined, so it is refused");
        Check(widget.Passed(), L"widget keys and HLS playlists parse%ls", widget.failed.c_str());

        const std::string hlsTrack = R"js({
            "id": 293, "title": "Flickermood", "duration": 213886, "streamable": true,
            "policy": "MONETIZE", "user": {"username": "Forss"},
            "artwork_url": "https://i1.sndcdn.com/artworks-000067273316-smsiqx-large.jpg",
            "media": {"transcodings": [
                {"url": "https://api-v2.soundcloud.com/media/soundcloud:tracks:293/abc/stream/hls",
                 "format": {"protocol": "hls", "mime_type": "audio/mp4; codecs=\"mp4a.40.2\""}},
                {"url": "https://api-v2.soundcloud.com/media/soundcloud:tracks:293/def/stream/hls",
                 "format": {"protocol": "hls", "mime_type": "audio/mpegurl"}}
            ]}
        })js";
        WebTrack joined;
        Check(soundcloud::ParseTrack(hlsTrack, joined) &&
                  joined.streamUrl.ends_with("soundcloud:tracks:293/abc/stream/hls") && joined.hls &&
                  joined.ext == "m4a" && !joined.hlsOnly && !joined.unavailable,
              L"an HLS audio/mp4 track keeps its entry, flagged for the lossless join, as an M4A");
        const std::string tsOnly = R"js({
            "id": 294, "title": "Preview", "duration": 99000, "streamable": true,
            "user": {"username": "Forss"},
            "media": {"transcodings": [
                {"url": "https://api-v2.soundcloud.com/media/soundcloud:tracks:294/abc/stream/hls",
                 "format": {"protocol": "hls", "mime_type": "audio/mpegurl"}}
            ]}
        })js";
        WebTrack reported;
        Check(soundcloud::ParseTrack(tsOnly, reported) && reported.hlsOnly && !reported.hls &&
                  reported.streamUrl.empty(),
              L"a track SoundCloud only offers as a transport stream is reported, not saved broken");
    }
}
