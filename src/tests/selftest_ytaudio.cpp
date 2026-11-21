// YouTube link recognition and InnerTube parsing against fabricated replies (ytaudio.h).
#include "selftest_common.h"
#include "ytaudio.h"

#include <string>
#include <string_view>
#include <vector>

namespace {

const char kAacFirst[] = R"json({
  "playabilityStatus": {"status": "OK", "playableInEmbed": true},
  "streamingData": {
    "expiresInSeconds": "21540",
    "adaptiveFormats": [
      {"itag": 251, "mimeType": "audio/webm; codecs=\"opus\"", "bitrate": 130000,
       "audioSampleRate": "48000", "audioChannels": 2,
       "url": "https://rr1---sn-example.googlevideo.com/videoplayback?itag=251&mime=audio%2Fwebm"},
      {"itag": 140, "mimeType": "audio/mp4; codecs=\"mp4a.40.2\"", "bitrate": 129000,
       "audioSampleRate": "44100", "audioChannels": 2,
       "url": "https://rr1---sn-example.googlevideo.com/videoplayback?itag=140&mime=audio%2Fmp4"}
    ]
  },
  "videoDetails": {"videoId": "dQw4w9WgXcQ", "title": "Never Gonna Give You Up (Official Video)",
                   "lengthSeconds": "213", "author": "Rick Astley", "channelId": "UCuAXFkgsw1L7xaCfnd5JJOw"}
})json";

const char kAacLow[] = R"json({
  "playabilityStatus": {"status": "OK"},
  "streamingData": {"adaptiveFormats": [
    {"itag": 139, "mimeType": "audio/mp4; codecs=\"mp4a.40.5\"", "bitrate": 48000,
     "url": "https://rr1---sn-example.googlevideo.com/videoplayback?itag=139&mime=audio%2Fmp4"}
  ]},
  "videoDetails": {"videoId": "dQw4w9WgXcQ", "title": "Ballad", "lengthSeconds": "200",
                   "author": "Rick Astley"}
})json";

const char kOpusOnly[] = R"json({
  "playabilityStatus": {"status": "OK"},
  "streamingData": {"adaptiveFormats": [
    {"itag": 251, "mimeType": "audio/webm; codecs=\"opus\"", "bitrate": 130000,
     "url": "https://rr1---sn-example.googlevideo.com/videoplayback?itag=251&mime=audio%2Fwebm"}
  ]},
  "videoDetails": {"videoId": "dQw4w9WgXcQ", "title": "Opus Song", "lengthSeconds": "200",
                   "author": "Rick Astley"}
})json";

const char kDrcAudioTrack[] = R"json({
  "playabilityStatus": {"status": "OK"},
  "streamingData": {"adaptiveFormats": [
    {"itag": 140, "mimeType": "audio/mp4; codecs=\"mp4a.40.2\"", "bitrate": 129000,
     "audioTrack": {"displayName": "English (US)", "id": "en.4", "audioIsDefault": false},
     "url": "https://rr1---sn-example.googlevideo.com/videoplayback?itag=140&mime=audio%2Fmp4"},
    {"itag": 139, "mimeType": "audio/mp4; codecs=\"mp4a.40.5\"", "bitrate": 48000,
     "audioTrack": {"displayName": "English original", "id": "en.1", "audioIsDefault": true},
     "url": "https://rr1---sn-example.googlevideo.com/videoplayback?itag=139&mime=audio%2Fmp4"}
  ]},
  "videoDetails": {"videoId": "dQw4w9WgXcQ", "title": "Dubbed Song", "lengthSeconds": "200",
                   "author": "Band"}
})json";

const char kDrcXtags[] = R"json({
  "playabilityStatus": {"status": "OK"},
  "streamingData": {"adaptiveFormats": [
    {"itag": 140, "mimeType": "audio/mp4; codecs=\"mp4a.40.2\"", "bitrate": 129000,
     "url": "https://rr1---sn-example.googlevideo.com/videoplayback?itag=140&xtags=ZHJj"},
    {"itag": 251, "mimeType": "audio/webm; codecs=\"opus\"", "bitrate": 130000,
     "url": "https://rr1---sn-example.googlevideo.com/videoplayback?itag=251"}
  ]},
  "videoDetails": {"videoId": "dQw4w9WgXcQ", "title": "Squashed Song", "lengthSeconds": "200",
                   "author": "Band"}
})json";

const char kBotWall[] = R"json({
  "playabilityStatus": {"status": "LOGIN_REQUIRED", "reason": "Sign in to confirm you're not a bot",
    "messages": [{"text": {"runs": [{"text": "Sign in to confirm you're not a bot"}]}}]},
  "videoDetails": {"videoId": "dQw4w9WgXcQ", "title": "Anything", "lengthSeconds": "100",
                   "author": "Someone"}
})json";

const char kAgeRestricted[] = R"json({
  "playabilityStatus": {"status": "LOGIN_REQUIRED", "reason": "Sign in to confirm your age",
    "errorScreen": {"playerErrorMessageRenderer": {
      "reason": {"simpleText": "This video may be inappropriate for some users."},
      "subreason": {"runs": [{"text": "Sign in to confirm your age"}]}}}},
  "videoDetails": {"videoId": "dQw4w9WgXcQ", "title": "Old Song", "lengthSeconds": "180",
                   "author": "Band"}
})json";

const char kMembersOnly[] = R"json({
  "playabilityStatus": {"status": "LOGIN_REQUIRED",
    "reason": "Join this channel to get access to members-only content like this video"},
  "videoDetails": {"videoId": "dQw4w9WgXcQ", "title": "Members Song", "lengthSeconds": "180",
                   "author": "Band"}
})json";

const char kErrorStatus[] = R"json({
  "playabilityStatus": {"status": "ERROR",
    "reason": "This video is no longer available because the uploader has closed their YouTube account."},
  "videoDetails": {"videoId": "dQw4w9WgXcQ", "title": "Gone", "lengthSeconds": "0", "author": "Band"}
})json";

const char kNoStreams[] = R"json({
  "playabilityStatus": {"status": "OK"},
  "videoDetails": {"videoId": "dQw4w9WgXcQ", "title": "Song", "lengthSeconds": "200", "author": "Band"}
})json";

const char kLive[] = R"json({
  "playabilityStatus": {"status": "OK"},
  "streamingData": {"adaptiveFormats": [
    {"itag": 140, "mimeType": "audio/mp4; codecs=\"mp4a.40.2\"", "bitrate": 129000,
     "url": "https://rr1---sn-example.googlevideo.com/videoplayback?itag=140&mime=audio%2Fmp4"}
  ]},
  "videoDetails": {"videoId": "dQw4w9WgXcQ", "title": "Broadcast", "lengthSeconds": "0",
                   "author": "Band", "isLive": true}
})json";

const char kTopicChannel[] = R"json({
  "playabilityStatus": {"status": "OK"},
  "streamingData": {"adaptiveFormats": [
    {"itag": 140, "mimeType": "audio/mp4; codecs=\"mp4a.40.2\"", "bitrate": 129000,
     "url": "https://rr1---sn-example.googlevideo.com/videoplayback?itag=140&mime=audio%2Fmp4"}
  ]},
  "videoDetails": {"videoId": "dQw4w9WgXcQ", "title": "Never Gonna Give You Up",
                   "lengthSeconds": "213", "author": "Rick Astley - Topic"}
})json";

const char kSplitTitle[] = R"json({
  "playabilityStatus": {"status": "OK"},
  "streamingData": {"adaptiveFormats": [
    {"itag": 140, "mimeType": "audio/mp4; codecs=\"mp4a.40.2\"", "bitrate": 129000,
     "url": "https://rr1---sn-example.googlevideo.com/videoplayback?itag=140&mime=audio%2Fmp4"}
  ]},
  "videoDetails": {"videoId": "dQw4w9WgXcQ",
                   "title": "Rick Astley - Never Gonna Give You Up (Official Video)",
                   "lengthSeconds": "213", "author": "Rick Astley"}
})json";

const char kNotSplit[] = R"json({
  "playabilityStatus": {"status": "OK"},
  "streamingData": {"adaptiveFormats": [
    {"itag": 140, "mimeType": "audio/mp4; codecs=\"mp4a.40.2\"", "bitrate": 129000,
     "url": "https://rr1---sn-example.googlevideo.com/videoplayback?itag=140&mime=audio%2Fmp4"}
  ]},
  "videoDetails": {"videoId": "dQw4w9WgXcQ", "title": "Live - At Wembley", "lengthSeconds": "300",
                   "author": "Some Artist"}
})json";

const char kPlaylistBrowse[] = R"json({
  "contents": {
    "twoColumnBrowseResultsRenderer": {
      "tabs": [{"tabRenderer": {"content": {
        "sectionListRenderer": {"contents": [{"itemSectionRenderer": {"contents": [
          {"playlistVideoListRenderer": {"contents": [
            {"playlistVideoRenderer": {"videoId": "aaaaaaaaaaa",
              "title": {"runs": [{"text": "First Song"}]}, "lengthSeconds": "215"}},
            {"playlistVideoRenderer": {"videoId": "bbbbbbbbbbb",
              "title": {"runs": [{"text": "Second Song"}]},
              "lengthText": {"simpleText": "1:02:03"}}},
            {"playlistVideoRenderer": {"title": {"simpleText": "[Deleted video]"}}}
          ]}}
        ]}}]}
      }}}],
      "header": {"playlistHeaderRenderer": {"title": {"runs": [{"text": "Test List"}]}}}
    }
  }
})json";

const char kPlaylistPanel[] = R"json({
  "contents": {"twoColumnWatchNextResults": {"playlist": {"playlist": {"contents": [
    {"playlistPanelVideoRenderer": {"videoId": "ccccccccccc", "title": {"simpleText": "Panel Song"},
      "lengthText": {"runs": [{"text": "3:42"}]}}},
    {"playlistPanelVideoRenderer": {"videoId": "ddddddddddd",
      "title": {"runs": [{"text": "Panel Two"}]}, "lengthSeconds": "42"}}
  ]}}}}
})json";

const char kSearch[] = R"json({
  "contents": {"twoColumnSearchResultsRenderer": {"primaryContents": {"sectionListRenderer": {
    "contents": [{"itemSectionRenderer": {"contents": [
      {"videoRenderer": {"videoId": "eeeeeeeeeee",
        "title": {"runs": [{"text": "Never Gonna Give You Up"}]},
        "ownerText": {"runs": [{"text": "Rick Astley - Topic"}]},
        "lengthText": {"simpleText": "3:42"},
        "ownerBadges": [{"metadataBadgeRenderer": {"tooltip": "Official Artist Channel"}}]}},
      {"videoRenderer": {"videoId": "fffffffffff",
        "title": {"runs": [{"text": "Live Cover"}]},
        "ownerText": {"runs": [{"text": "Some Channel"}]}}},
      {"compactVideoRenderer": {"videoId": "ggggggggggg", "title": {"simpleText": "Compact Row"},
        "shortBylineText": {"runs": [{"text": "Another Channel"}]},
        "lengthText": {"simpleText": "1:02:03"},
        "ownerBadges": [{"musicTopicBadgeRenderer": {"tooltip": "Artist"}}]}}
    ]}}]}
  }}}}
)json";

bool Has(const std::string& text, std::string_view part) { return text.find(part) != std::string::npos; }

}  // namespace

void TestYtAudio() {
    testlog::Batch batch;

    batch.Next(L"YouTube links are recognised as a video, a playlist or nothing, and their ids read");
    {
        struct Row {
            const wchar_t* name;
            const char* url;
            yt::Kind kind;
            const char* video;
            const char* list;
        };
        const Row rows[] = {
            {L"watch page", "https://www.youtube.com/watch?v=dQw4w9WgXcQ", yt::Kind::Video,
             "dQw4w9WgXcQ", ""},
            {L"link without a scheme", "youtube.com/watch?v=dQw4w9WgXcQ&t=42s", yt::Kind::Video,
             "dQw4w9WgXcQ", ""},
            {L"youtu.be short link", "https://youtu.be/dQw4w9WgXcQ?si=abc", yt::Kind::Video,
             "dQw4w9WgXcQ", ""},
            {L"shorts link", "https://www.youtube.com/shorts/dQw4w9WgXcQ?feature=share", yt::Kind::Video,
             "dQw4w9WgXcQ", ""},
            {L"music.youtube watch with a list",
             "https://music.youtube.com/watch?v=dQw4w9WgXcQ&list=RDAMVMdQw4w9WgXcQ", yt::Kind::Playlist,
             "dQw4w9WgXcQ", "RDAMVMdQw4w9WgXcQ"},
            {L"list without a video", "https://www.youtube.com/watch?list=PL1234567890abcdef",
             yt::Kind::Playlist, "", "PL1234567890abcdef"},
            {L"short link with a list", "https://youtu.be/dQw4w9WgXcQ?list=PL1234567890abcdef",
             yt::Kind::Playlist, "dQw4w9WgXcQ", "PL1234567890abcdef"},
            {L"uppercase scheme and host", "HTTPS://WWW.YOUTUBE.COM/watch?v=dQw4w9WgXcQ", yt::Kind::Video,
             "dQw4w9WgXcQ", ""},
            {L"watch page without a video", "https://www.youtube.com/watch", yt::Kind::None, "", ""},
            {L"video id of the wrong length", "https://www.youtube.com/watch?v=dQw4w9WgXc", yt::Kind::None,
             "", ""},
            {L"shorts path with more after the id",
             "https://www.youtube.com/shorts/dQw4w9WgXcQ/extra", yt::Kind::None, "", ""},
            {L"another site", "https://vimeo.com/123456", yt::Kind::None, "", ""},
            {L"plain text", "not a link", yt::Kind::None, "", ""},
        };
        testlog::Cases links;
        for (const Row& row : rows)
            links.Add(yt::Classify(row.url) == row.kind && yt::VideoIdOf(row.url) == row.video &&
                          yt::PlaylistIdOf(row.url) == row.list,
                      row.name);
        Check(links.Passed(),
              L"YouTube links are recognised as a video, a playlist or nothing, and their ids read%ls",
              links.failed.c_str());
    }

    batch.Next(L"the player reply yields its AAC stream, title, channel, length, thumbnail and kind");
    {
        WebTrack track;
        std::string error;
        const bool ok = yt::ParsePlayer(kAacFirst, track, error);
        Check(ok && !track.unavailable && error.empty() && track.site == "youtube" &&
                  track.streamUrl ==
                      "https://rr1---sn-example.googlevideo.com/videoplayback?itag=140&mime=audio%2Fmp4" &&
                  track.ext == "m4a" && track.id == "dQw4w9WgXcQ" &&
                  track.title == "Never Gonna Give You Up (Official Video)" &&
                  track.artist == "Rick Astley" && track.durationSec == 213 &&
                  track.artworkUrl == "https://i.ytimg.com/vi/dQw4w9WgXcQ/hqdefault.jpg",
              L"the player reply yields its AAC stream, title, channel, length, thumbnail and kind");
        Check(yt::ParsePlayer(kAacLow, track, error) && track.ext == "m4a" &&
                  Has(track.streamUrl, "itag=139"),
              L"a reply offering only the lower AAC itag still saves AAC");
        Check(yt::ParsePlayer(kOpusOnly, track, error) && track.ext == "webm" &&
                  Has(track.streamUrl, "itag=251"),
              L"Opus is the fallback when no AAC stream is offered");
        Check(yt::ParsePlayer(kDrcAudioTrack, track, error) && Has(track.streamUrl, "itag=139") &&
                  track.ext == "m4a",
              L"a dubbed or DRC audio track is skipped for the default one at a lower itag");
        Check(yt::ParsePlayer(kDrcXtags, track, error) && Has(track.streamUrl, "itag=251") &&
                  track.ext == "webm",
              L"a DRC copy marked only in xtags is skipped as well");
        Check(!yt::ParsePlayer("not a player reply", track, error) && !error.empty() && !track.unavailable,
              L"a reply that is not JSON is a failed resolution, not a dead track");
    }

    batch.Next(L"refusals say what happened and only the temporary ones are retried later");
    {
        WebTrack track;
        std::string error;
        const bool bot = yt::ParsePlayer(kBotWall, track, error);
        Check(!bot && !track.unavailable && Has(error, "temporarily") && Has(error, "not a bot") &&
                  Has(error, "signing in"),
              L"the bot check is refused as temporary and is never remembered as a dead track");
        Check(yt::ParsePlayer(kAgeRestricted, track, error) && track.unavailable && track.streamUrl.empty() &&
                  track.why == "This video needs a YouTube account, so it cannot be saved.",
              L"an age check is remembered as needing a YouTube account");
        Check(yt::ParsePlayer(kMembersOnly, track, error) && track.unavailable &&
                  Has(track.why, "YouTube account"),
              L"a members-only video is remembered the same way");
        Check(yt::ParsePlayer(kErrorStatus, track, error) && track.unavailable &&
                  track.why == "This video is no longer available because the uploader has closed their "
                               "YouTube account.",
              L"a removed video is remembered with YouTube's own words");
        Check(yt::ParsePlayer(kNoStreams, track, error) && track.unavailable &&
                  track.why == "This video has no audio stream.",
              L"a reply without a stream is remembered as having no audio");
        Check(yt::ParsePlayer(kLive, track, error) && track.unavailable && Has(track.why, "live stream"),
              L"a live broadcast is refused until it has ended");
    }

    batch.Next(L"titles stay whole, and the channel becomes the artist");
    {
        WebTrack track;
        std::string error;
        Check(yt::ParsePlayer(kTopicChannel, track, error) && track.artist == "Rick Astley" &&
                  track.title == "Never Gonna Give You Up",
              L"a \"- Topic\" channel loses the suffix and leaves a song title whole");
        Check(yt::ParsePlayer(kSplitTitle, track, error) && track.artist == "Rick Astley" &&
                  track.title == "Never Gonna Give You Up (Official Video)",
              L"a title repeating the channel's own name drops that prefix");
        Check(yt::ParsePlayer(kNotSplit, track, error) && track.artist == "Some Artist" &&
                  track.title == "Live - At Wembley",
              L"a title whose prefix is not the channel is left alone");
    }

    batch.Next(L"a playlist reply reads its rows in order, from a browse page or a watch page's panel");
    {
        std::vector<yt::PlaylistEntry> entries;
        Check(yt::ParsePlaylist(kPlaylistBrowse, entries) == 2 && entries[0].id == "aaaaaaaaaaa" &&
                  entries[0].title == "First Song" && entries[0].durationSec == 215 &&
                  entries[1].id == "bbbbbbbbbbb" && entries[1].title == "Second Song" &&
                  entries[1].durationSec == 3723,
              L"a playlist reply reads its rows in order, with both length shapes");
        Check(yt::ParsePlaylist(kPlaylistPanel, entries) == 2 && entries[0].id == "ccccccccccc" &&
                  entries[0].title == "Panel Song" && entries[0].durationSec == 222 &&
                  entries[1].id == "ddddddddddd" && entries[1].durationSec == 42,
              L"the playlist panel on a watch page is read too");
    }

    batch.Next(L"search rows read titles, channels, lengths and Topic channels");
    {
        std::vector<yt::Candidate> found;
        Check(yt::ParseSearch(kSearch, found) == 3 && found[0].id == "eeeeeeeeeee" &&
                  found[0].title == "Never Gonna Give You Up" && found[0].channel == "Rick Astley - Topic" &&
                  found[0].durationSec == 222 && found[0].topic && found[1].id == "fffffffffff" &&
                  found[1].title == "Live Cover" && found[1].channel == "Some Channel" &&
                  found[1].durationSec == 0 && !found[1].topic && found[2].id == "ggggggggggg" &&
                  found[2].title == "Compact Row" && found[2].channel == "Another Channel" &&
                  found[2].durationSec == 3723 && found[2].topic,
              L"search rows read titles, channels, lengths and Topic channels");
    }
}
