// HLS syntax, URL resolution and segment safety checks without an internet connection.
#include "selftest_common.h"
#include "radio_hls.h"
using namespace std::string_literals;

void TestHls() {
    const std::string base = "https://radio.example/live/master.m3u8?auth=one";
    testlog::Cases urls;
    urls.Add(radio::ResolveRadioUrl(base, "../audio/list.m3u8?x=1") == "https://radio.example/audio/list.m3u8?x=1", L"parent path");
    urls.Add(radio::ResolveRadioUrl(base, "//cdn.example/a.aac") == "https://cdn.example/a.aac", L"new host");
    urls.Add(radio::ResolveRadioUrl(base, "?auth=two") == "https://radio.example/live/master.m3u8?auth=two", L"query");
    urls.Add(radio::ResolveRadioUrl(base, "bad\r\nHeader: value").empty(), L"header injection");
    Check(urls.Passed(), L"HLS links preserve relative paths and query strings and reject control bytes%ls", urls.failed.c_str());
    radio::HlsPlaylist playlist; std::wstring problem;
    const std::string master = "#EXTM3U\n#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"radio\",DEFAULT=YES,URI=\"audio.m3u8\"\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=2000000,RESOLUTION=1280x720,AUDIO=\"radio\"\nvideo.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=128000,CODECS=\"mp4a.40.2\"\nlow.m3u8\n";
    Check(radio::ParseHls(master, base, playlist, problem) && playlist.variant == "https://radio.example/live/low.m3u8",
          L"HLS selects an audio variant ahead of a video stream");
    const std::string rendition = "#EXTM3U\n#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"radio\",DEFAULT=YES,URI=\"audio.m3u8\"\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=2000000,RESOLUTION=1280x720,AUDIO=\"radio\"\nvideo.m3u8\n";
    Check(radio::ParseHls(rendition, base, playlist, problem) && playlist.variant == "https://radio.example/live/audio.m3u8",
          L"HLS follows the selected variant's separate audio rendition");
    const std::string media = "#EXTM3U\n#EXT-X-TARGETDURATION:4\n#EXT-X-MEDIA-SEQUENCE:21\n"
        "#EXT-X-KEY:METHOD=AES-128,URI=\"key\",IV=0x1234\n#EXT-X-MAP:URI=\"init.mp4\"\n"
        "#EXT-X-BYTERANGE:100@0\n#EXTINF:4,\nall.m4s\n#EXT-X-DISCONTINUITY\n"
        "#EXT-X-BYTERANGE:90\n#EXTINF:4,\nall.m4s\n#EXT-X-ENDLIST\n";
    bool parsed = radio::ParseHls(media, base, playlist, problem);
    Check(parsed && playlist.ended && playlist.target == 4 && playlist.segments.size() == 2 &&
              playlist.segments[0].sequence == 21 && playlist.segments[1].range.offset == 100 &&
              playlist.segments[1].discontinuity && playlist.segments[0].iv[14] == 0x12 && playlist.segments[0].iv[15] == 0x34,
          L"HLS preserves sequence numbers, byte ranges, keys, initialization and discontinuities");
    Check(!radio::ParseHls("#EXTM3U\n#EXT-X-KEY:METHOD=SAMPLE-AES,URI=\"key\"\na.aac\n", base, playlist, problem) &&
              !problem.empty() && !radio::ParseHls("#EXTM3U\n#EXT-X-BYTERANGE:500@18446744073709551615\na\n", base, playlist, problem),
          L"HLS rejects unsupported encryption and overflowing ranges before fetching audio");
    radio::HlsAudio demux; std::string output;
    std::string packed = "ID3\4\0\0\0\0\0\0"s + "\xFF\xFB\x90\x64"s;
    Check(demux.Segment(packed, output) && output == "\xFF\xFB\x90\x64"s && !demux.Segment("not an audio segment", output),
          L"packed HLS audio strips its timestamp tag and rejects non-audio data");
}
