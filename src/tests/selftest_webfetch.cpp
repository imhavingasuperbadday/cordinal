// The saving queue end to end, offline: a fake resolver and fake IO stand in for the
// sites, so every rule the queue owns is checked without the internet - filed names,
// "never download the same song twice", deleted files fetched again, collisions
// numbered instead of replaced, failures remembered (and only the permanent ones),
// the Spotify match flow, fragmented MP4 streams rewritten into plain tagged M4A,
// cancellation, each link's named list span, and imported rows entering the queue.
#include "selftest_common.h"
#include "webfetch.h"

#include "audio_packets.h"
#include "mp4_remux.h"
#include "playlist_import.h"
#include "tagwriter.h"
#include "tags.h"

#include <map>

namespace {

bool FileExists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// The companion's own folder, without depending on another test file's ExePath.
std::wstring ExeFolder() {
    std::wstring path(32768, L'\0');
    path.resize(GetModuleFileNameW(nullptr, path.data(), (DWORD)path.size()));
    const size_t slash = path.find_last_of(L"\\/");
    return slash == path.npos ? L"." : path.substr(0, slash);
}

// A minimal fragmented MP4 matching audio_mp4.cpp's layout: one mp4a track with a real
// esds config, an edit list and two moof/mdat pairs, the first fragment starting at
// `lead` ticks. The payload bytes stand in for AAC frames; only the container is real.
// `lead` above the edit list's playable start is the one timeline the rewrite refuses.
std::string SyntheticFragmentedMp4(uint64_t lead) {
    auto be = [](uint64_t value, int width) {
        std::string s;
        for (int i = width - 1; i >= 0; --i) s += (char)(value >> (8 * i));
        return s;
    };
    auto box = [&](const char* type, const std::string& body) { return be(8 + body.size(), 4) + type + body; };
    auto full = [&](const char* type, uint32_t flags, const std::string& body) {
        return box(type, be(flags, 4) + body);
    };
    auto desc = [&](uint8_t tag, const std::string& body) {
        return std::string(1, (char)tag) + std::string(1, (char)body.size()) + body;
    };
    const std::string matrix = be(0x00010000, 4) + be(0, 4) + be(0, 4) + be(0, 4) +
                               be(0x00010000, 4) + be(0, 4) + be(0, 4) + be(0, 4) +
                               be(0x40000000, 4);

    // esds: an AAC-LC 48 kHz stereo AudioSpecificConfig (0x11 0x90) the parser can walk.
    const std::string esds = full("esds", 0,
        desc(3, be(1, 2) + be(0, 1) +
             desc(4, be(0x40, 1) + be(0x15, 1) + be(0, 11) +
                  desc(5, std::string("\x11\x90", 2)) +
                  desc(6, be(2, 1)))));
    const std::string mp4a = box("mp4a",
        be(0, 6) + be(1, 2) + be(0, 8) + be(2, 2) + be(16, 2) + be(0, 4) +
        be(uint64_t(48000) << 16, 4) + esds);
    const std::string stbl = full("stsd", 0, be(1, 4) + mp4a) + full("stsz", 0, be(0, 8));
    const std::string minf = box("minf", full("smhd", 0, be(0, 4)) +
                                 box("dinf", full("dref", 0, be(1, 4) + full("url ", 1, ""))) +
                                 box("stbl", stbl));
    const std::string mdhd = full("mdhd", 0, be(0, 8) + be(48000, 4) + be(0, 4) + be(0x55C4, 4));
    const std::string hdlr = full("hdlr", 0, be(0, 4) + std::string("soun", 4) + be(0, 12) +
                                  std::string("SoundHandler") + '\0');
    const std::string mdia = box("mdia", mdhd + hdlr + minf);
    // The edit list states the playable start at 2048 ticks.
    const std::string elst = full("elst", 0, be(1, 4) + be(5000, 4) + be(2048, 4) + be(0x10000, 4));
    const std::string tkhd = full("tkhd", 3, be(0, 8) + be(1, 4) + be(0, 4) + be(0, 4) +
                                  be(0, 8) + be(0, 8) + matrix + be(0, 8));
    const std::string trak = box("trak", tkhd + box("edts", elst) + mdia);
    const std::string mvhd = full("mvhd", 0, be(0, 8) + be(1000, 4) + be(0, 4) + be(0x10000, 4) +
                                  be(0x0100, 2) + be(0, 10) + matrix + be(0, 24) + be(2, 4));
    const std::string moov = box("moov", mvhd + trak +
                                  box("mvex", box("trex", be(1, 4) + be(1, 4) + be(0, 12))));

    // Two fragment runs holding five fake frames, 1024 ticks each, sizes varied.
    const unsigned sizes[5] = {13, 17, 11, 19, 7};
    std::string data;
    for (unsigned i = 0; i < 5; ++i)
        for (unsigned b = 0; b < sizes[i]; ++b) data += (char)(0x40 + i * 8 + b);
    auto moofAt = [&](unsigned first, unsigned count, uint64_t dataOffset) {
        std::string runs = be(0x000301, 4) + be(count, 4) + be(dataOffset, 4);
        for (unsigned i = first; i < first + count; ++i) runs += be(1024, 4) + be(sizes[i], 4);
        return box("moof", full("mfhd", 0, be(first / 3 + 1, 4)) +
                   box("traf", full("tfhd", 0x020000, be(1, 4)) +
                               full("tfdt", 0x01000000, be(lead + first * 1024, 8)) +
                               box("trun", runs)));
    };
    // The trun data offset points past its own moof and the mdat header, so each run is
    // built once to measure and again with the real offset.
    std::string moof1 = moofAt(0, 3, 0), moof2 = moofAt(3, 2, 0);
    moof1 = moofAt(0, 3, moof1.size() + 8);
    moof2 = moofAt(3, 2, moof2.size() + 8);
    const std::string ftyp = box("ftyp", std::string("isom", 4) + be(0, 4) + std::string("isomiso2mp41", 12));
    return ftyp + moov + moof1 + box("mdat", data.substr(0, 41)) + moof2 + box("mdat", data.substr(41));
}

// The fake site: what a link "resolves to", and what a stream "downloads".
struct Fake {
    std::vector<WebTrack> tracks;
    std::string listTitle;
    std::map<std::string, std::vector<WebTrack>> byLink;  // links that resolve to their own list
    std::map<std::string, std::string> bodies;  // stream url -> bytes
    std::map<std::string, size_t> fetched;      // stream url -> downloads
    bool failFetch = false;
    std::atomic<bool>* trip = nullptr;           // set once a download starts, for the cancel case
};

}  // namespace

void TestWebFetch(const std::wstring& scratch, const std::wstring& media) {
    testlog::Batch batch;
    const std::wstring root = scratch + L"\\webfetch";
    const std::wstring data = scratch + L"\\webfetch-data";
    CreateDirectoryW(root.c_str(), nullptr);
    CreateDirectoryW(data.c_str(), nullptr);
    Ready(FileExists(root) && FileExists(data), L"queue folders");
    const std::string mp3 = ReadTestBytes(media + L"\\sine.mp3");
    if (!Ready(!mp3.empty(), L"the sine.mp3 fixture")) return;

    Fake fake;
    fake.bodies["https://example.test/a.mp3"] = mp3;
    fake.bodies["https://example.test/b.mp3"] = mp3;
    fake.bodies["https://example.test/matched.mp3"] = mp3;
    std::atomic<bool> stop = false;

    // The production download never runs here: this fake is the whole network.
    WebFetchIO io;
    io.fetch = [&fake](const std::string& url, const std::wstring& file,
                       const std::function<void(int)>& progress) {
        fake.fetched[url]++;
        if (fake.trip) fake.trip->store(true);
        if (fake.failFetch) return false;
        auto body = fake.bodies.find(url);
        if (body == fake.bodies.end() || !WriteTestBytes(file, body->second)) return false;
        progress(100);
        return true;
    };
    io.fetchBytes = [&fake](const std::string& url, std::string& bytes) {
        auto body = fake.bodies.find(url);
        if (body == fake.bodies.end()) return false;
        bytes = body->second;
        return true;
    };
    io.fetchHls = [&fake](const WebTrack& track, const std::wstring& file,
                          const std::function<void(int)>& progress) {
        fake.fetched[track.streamUrl]++;
        if (fake.failFetch) return false;
        auto body = fake.bodies.find(track.streamUrl);
        if (body == fake.bodies.end() || !WriteTestBytes(file, body->second)) return false;
        progress(100);
        return true;
    };
    SetWebFetchIO(io);
    struct RestoreIO {
        ~RestoreIO() { SetWebFetchIO(WebFetchIO{}); }
    } restore;

    auto makeRun = [&](std::vector<WebTrack> tracks, const std::string& listTitle) {
        fake.tracks = std::move(tracks);
        fake.listTitle = listTitle;
        WebFetchRun run;
        run.links = {"https://soundcloud.com/mira/sets/sunsets"};
        run.libraryRoot = root;
        run.dataDir = data;
        run.resolvers.link = [&fake](const std::string&, const std::atomic<bool>&,
                                     std::vector<WebTrack>& tracks, std::string& listTitle, std::string&,
                                     const std::function<void(const std::string&)>&) {
            tracks = fake.tracks;
            listTitle = fake.listTitle;
            return true;
        };
        return run;
    };

    auto Make = [&](const char* id, const char* title) {
        WebTrack t;
        t.site = "soundcloud";
        t.id = id;
        t.title = title;
        t.artist = "Mira";
        t.streamUrl = std::string("https://example.test/") + id + ".mp3";
        t.ext = "mp3";
        fake.bodies[t.streamUrl] = mp3;
        return t;
    };

    batch.Next(L"a set downloads once, files as Artist\\Album\\NN Title, is tagged, a second run reports it "
               L"without downloading, and a deleted file is fetched again");
    {
        WebFetchRun run = makeRun({Make("777", "Sunset")}, "Sunsets");
        std::vector<WebFetchItem> items;
        std::string error;
        Check(RunWebFetch(run, items, error) && items.size() == 1,
              L"one pasted set link resolves to its track");
        if (Ready(items.size() == 1, L"the first resolved track")) {
            const WebFetchItem& done = items[0];
            Check(done.state == WebState::Done && !done.file.empty() &&
                      done.file.find(L"Mira\\Sunsets\\01 Sunset.mp3") != done.file.npos,
                  L"a set's track is filed as Artist\\Album\\01 Title");
            std::string saved = ReadTestBytes(done.file);
            Check(saved.size() >= mp3.size() && saved.compare(0, 3, "ID3") == 0,
                  L"the saved file is a real MP3 with an ID3 tag written into it");
            Check(FileExists(WebFetchRecordPath(data)), L"the saved record exists beside the library");
            size_t downloads = fake.fetched["https://example.test/777.mp3"];
            std::vector<WebFetchItem> again;
            RunWebFetch(run, again, error);
            Check(again.size() == 1 && again[0].state == WebState::Exists &&
                      fake.fetched["https://example.test/777.mp3"] == downloads,
                  L"a second run reports the track without downloading it again");
            DeleteFileW(again[0].file.c_str());
            std::vector<WebFetchItem> refetched;
            RunWebFetch(run, refetched, error);
            Check(refetched[0].state == WebState::Done &&
                      fake.fetched["https://example.test/777.mp3"] == downloads + 1,
                  L"owning means the bytes: a deleted file is fetched again");
        }
    }

    batch.Next(L"a second recording with the same name is numbered, never replaced, and failures are "
               L"remembered only when they are permanent");
    {
        // Two different sets, each holding one "Sunset": the second meets the first's
        // finished file at the same name, and is numbered instead of replacing it. (Two
        // same-titled tracks inside one set are 01 and 02, which never collide.)
        fake.byLink["https://soundcloud.com/mira/sets/sunsets"] = {Make("777", "Sunset")};
        fake.byLink["https://soundcloud.com/mira/sets/sunsets-again"] = {Make("778", "Sunset")};
        WebFetchRun run = makeRun({}, "Sunsets");
        run.links = {"https://soundcloud.com/mira/sets/sunsets",
                     "https://soundcloud.com/mira/sets/sunsets-again"};
        run.resolvers.link = [&fake](const std::string& link, const std::atomic<bool>&,
                                     std::vector<WebTrack>& tracks, std::string& listTitle, std::string&,
                                     const std::function<void(const std::string&)>&) {
            tracks = fake.byLink[link];
            listTitle = "Sunsets";
            return true;
        };
        std::vector<WebFetchItem> items;
        std::string error;
        RunWebFetch(run, items, error);
        Check(items.size() == 2 && items[0].state == WebState::Exists && items[1].state == WebState::Done &&
                  items[1].file.find(L"01 Sunset (2).mp3") != items[1].file.npos &&
                  FileExists(items[0].file),
              L"a name collision is numbered, never replaced");

        WebTrack dead = Make("779", "Gone");
        dead.streamUrl.clear();
        dead.unavailable = true;
        dead.why = "this track is no longer streamable on SoundCloud";
        run = makeRun({dead}, "Sunsets");
        std::vector<WebFetchItem> failed;
        RunWebFetch(run, failed, error);
        Check(failed.size() == 1 && failed[0].state == WebState::Failed && failed[0].permanent &&
                  failed[0].why == dead.why,
              L"an unavailable track fails once with its reason");
        // Flip the fake healthy: the record still owns the verdict, so it is not retried.
        WebTrack healthy = Make("779", "Gone");
        run = makeRun({healthy}, "Sunsets");
        std::vector<WebFetchItem> remembered;
        RunWebFetch(run, remembered, error);
        Check(remembered[0].state == WebState::Failed && remembered[0].permanent &&
                  remembered[0].why == dead.why && fake.fetched["https://example.test/779.mp3"] == 0,
              L"a track that failed for good is remembered and never retried");
        std::string record = ReadTestBytes(WebFetchRecordPath(data));
        Check(record.find("779") != record.npos, L"the permanent failure is written to the record");
        Check(WebFetchForgetFailures(data) &&
                  ReadTestBytes(WebFetchRecordPath(data)).find("779") == std::string::npos,
              L"forgetting failures clears the record, so trying again is a button");
        std::vector<WebFetchItem> retried;
        RunWebFetch(run, retried, error);
        Check(retried[0].state == WebState::Done, L"a forgotten failure is tried again on the next run");

        WebTrack hls = Make("780", "Preview");
        hls.streamUrl.clear();
        hls.ext.clear();
        hls.hlsOnly = true;
        hls.why = "only offered as an HLS stream";
        run = makeRun({hls}, "Sunsets");
        std::vector<WebFetchItem> streamed;
        RunWebFetch(run, streamed, error);
        Check(streamed[0].state == WebState::Failed && streamed[0].permanent &&
                  streamed[0].why == hls.why,
              L"an HLS-only track is reported, not saved broken");
    }

    batch.Next(L"a Spotify track is matched to YouTube's audio but keeps Spotify's tags, a missing match is "
               L"gone for good, and a search that does not answer is tried again next run");
    {
        auto MakeSpotify = [&](const char* id) {
            WebTrack t;
            t.site = "spotify";
            t.id = id;
            t.title = "North";
            t.artist = "Mira";
            t.album = "Northbound";
            t.artworkUrl = "https://example.test/cover.jpg";
            t.durationSec = 180;
            t.trackNo = 1;
            t.trackTotal = 1;
            return t;
        };
        fake.bodies["https://example.test/cover.jpg"] = mp3;  // any bytes: the writer embeds what it is given
        fake.bodies["https://example.test/matched.mp3"] = mp3;

        WebTrack matched;
        matched.site = "youtube";
        matched.id = "yt1";
        matched.title = "North";
        matched.artist = "Mira - Topic";
        matched.streamUrl = "https://example.test/matched.mp3";
        matched.ext = "mp3";
        matched.durationSec = 180;
        WebFetchRun run = makeRun({MakeSpotify("sp1")}, "");
        run.links = {"https://open.spotify.com/track/sp1"};
        run.resolvers.matchSpotify = [&matched](const WebTrack& want, const std::atomic<bool>&,
                                                 WebTrack& out, std::string&) {
            if (want.id != "sp1") return false;
            out = matched;
            // The queue keeps the real item's identity, not the match's.
            Check(want.album == "Northbound", L"the matcher is asked for the real item");
            return true;
        };
        std::vector<WebFetchItem> items;
        std::string error;
        RunWebFetch(run, items, error);
        Check(items.size() == 1 && items[0].state == WebState::Done &&
                  items[0].file.find(L"Mira\\Northbound\\01 North.mp3") != items[0].file.npos,
              L"a matched track keeps Spotify's title, artist, album and numbering");
        std::string saved = ReadTestBytes(items[0].file);
        Check(saved.compare(0, 3, "ID3") == 0 && saved.size() > mp3.size(),
              L"the cover is embedded along with the tags");

        run = makeRun({MakeSpotify("sp2")}, "");
        run.links = {"https://open.spotify.com/track/sp2"};
        run.resolvers.matchSpotify = [](const WebTrack&, const std::atomic<bool>&, WebTrack&, std::string& why) {
            why = "no YouTube upload of \"North\" is the same recording";
            return true;  // true with an empty match: gone for good
        };
        std::vector<WebFetchItem> missing;
        RunWebFetch(run, missing, error);
        Check(missing[0].state == WebState::Failed && missing[0].permanent &&
                  missing[0].why == "no YouTube upload of \"North\" is the same recording",
              L"a recording YouTube does not have fails once, for good");

        run = makeRun({MakeSpotify("sp3")}, "");
        run.links = {"https://open.spotify.com/track/sp3"};
        run.resolvers.matchSpotify = [](const WebTrack&, const std::atomic<bool>&, WebTrack&, std::string& why) {
            why = "YouTube did not answer the search";
            return false;  // transport trouble: next run tries again
        };
        std::vector<WebFetchItem> unreachable;
        RunWebFetch(run, unreachable, error);
        Check(unreachable[0].state == WebState::Failed && !unreachable[0].permanent,
              L"a search that did not answer is tried again on the next run");
    }

    batch.Next(L"an HLS track is joined through the HLS path, not fetched as a plain file");
    {
        WebTrack hlsTrack = Make("784", "Joined");
        hlsTrack.hls = true;
        hlsTrack.ext = "m4a";
        hlsTrack.streamUrl = "https://example.test/hls/784";
        hlsTrack.artworkUrl = "https://example.test/cover.jpg";
        fake.bodies["https://example.test/hls/784"] = mp3;  // any bytes stand in for the joined file
        WebFetchRun run = makeRun({hlsTrack}, "Sunsets");
        std::vector<WebFetchItem> items;
        std::string error;
        RunWebFetch(run, items, error);
        Check(items.size() == 1 && items[0].state == WebState::Done &&
                  items[0].file.find(L"01 Joined.m4a") != items[0].file.npos &&
                  fake.fetched["https://example.test/hls/784"] == 1 &&
                  FileExists(root + L"\\Mira\\Sunsets\\cover.jpg"),
              L"an HLS track saves through the site's join, filed as an M4A, and its cover becomes the "
              L"album folder's picture");
    }

    batch.Next(L"a fragmented MP4 stream is rewritten into one standard tagged M4A before it is filed, "
               L"and one the rewrite cannot express is kept as it arrived");
    {
        // The parser's own view of a minimal fragmented file, and the rewrite's result.
        const std::wstring fragPath = scratch + L"\\mp4-remux-frag.m4a";
        const std::wstring remuxPath = scratch + L"\\mp4-remux-out.m4a";
        std::wstring error;
        if (Ready(WriteTestBytes(fragPath, SyntheticFragmentedMp4(1024)),
                  L"the synthetic fragmented MP4")) {
            Check(nativeaudio::IsFragmentedMp4(fragPath), L"a moof-run file is recognised as fragmented");
            nativeaudio::Mp4Fragments frag;
            Check(nativeaudio::ReadMp4Fragments(fragPath, frag, error) && frag.runs.size() == 5 &&
                      frag.lead == 1024 && frag.hasEdit && frag.editMediaTime == 2048 &&
                      frag.mediaScale == 48000 && frag.movieScale == 1000 && frag.mediaDuration == 5120,
                  L"the fragment reader measures the runs, the lead and the edit list");
            Check(nativeaudio::RemuxFragmentedMp4(fragPath, remuxPath, error) &&
                      !nativeaudio::IsFragmentedMp4(remuxPath),
                  L"the rewrite turns the fragmented file into one standard M4A");
            nativeaudio::PacketFile plain;
            const bool parsed = plain.OpenMp4(remuxPath) && plain.Count() == 5 && plain.skip == 1024 &&
                                plain.mp4TimeScale == 48000 && plain.config == frag.config;
            Check(parsed, L"the rewritten M4A parses with the same samples and timeline");
            if (parsed) {
                bool runsMatch = true;
                const unsigned sizes[5] = {13, 17, 11, 19, 7};
                for (size_t i = 0; runsMatch && i < plain.Count(); ++i) {
                    const nativeaudio::Packet p = plain.At(i);
                    runsMatch = p.bytes == sizes[i] && p.frames == 1024;
                }
                Check(runsMatch, L"every frame keeps its size and duration through the rewrite");
            }
            // What the tag writer refused before the rewrite, it now writes and reads back.
            const std::string cover = std::string("\xFF\xD8\xFF\xE0", 4) + std::string(60, 'c');
            TagEdit edit;
            edit.title = "Synthetic";
            edit.artist = "Mira";
            edit.album = "Fragments";
            edit.track = 2;
            edit.trackTotal = 5;
            edit.comment = "audio matched on YouTube; tags from Spotify";
            edit.cover = cover;
            std::wstring tagError;
            Check(WriteTags(remuxPath, edit, tagError), L"the MP4 tag writer takes the rewritten file");
            TagData tags;
            std::string art;
            Check(ReadTags(remuxPath, tags) && tags.title == "Synthetic" && tags.artist == "Mira" &&
                      tags.album == "Fragments" && tags.track == 2 && tags.trackTotal == 5 &&
                      tags.comment == "audio matched on YouTube; tags from Spotify" &&
                      ReadEmbeddedArt(remuxPath, art) && art == cover,
                  L"title, artist, album, numbering, comment and cover all read back");
        }

        // A lead after the playable start is the one shape the rewrite refuses; the
        // refusal must leave nothing behind and say why.
        const std::wstring brokenPath = scratch + L"\\mp4-remux-broken.m4a";
        const std::wstring refusePath = scratch + L"\\mp4-remux-refused.m4a";
        if (Ready(WriteTestBytes(brokenPath, SyntheticFragmentedMp4(4096)),
                  L"the unsuitable fragmented MP4")) {
            Check(!nativeaudio::RemuxFragmentedMp4(brokenPath, refusePath, error) && !error.empty() &&
                      !FileExists(refusePath),
                  L"a timeline the rewrite cannot express is refused in words, leaving no file");
        }

        // The whole queue on a real fragmented body: sine.m4a's own packets, restated as
        // moof/mdat runs the way the site modules join an HLS stream.
        const std::string sineM4a = ReadTestBytes(media + L"\\sine.m4a");
        if (Ready(!sineM4a.empty(), L"the sine.m4a fixture")) {
            std::string fragSine;
            const std::wstring plainPath = scratch + L"\\mp4-remux-plain.m4a";
            if (Ready(WriteTestBytes(plainPath, sineM4a) &&
                      FragmentMp4Fixture(plainPath, sineM4a, fragSine),
                      L"the fragmented sine fixture")) {
                WebTrack fragTrack = Make("785", "Rewritten");
                fragTrack.ext = "m4a";
                fragTrack.streamUrl = "https://example.test/frag/785";
                fragTrack.artworkUrl = "https://example.test/cover.jpg";
                fake.bodies["https://example.test/frag/785"] = fragSine;
                WebFetchRun run = makeRun({fragTrack}, "Sunsets");
                std::vector<WebFetchItem> items;
                std::string runError;
                RunWebFetch(run, items, runError);
                Check(items.size() == 1 && items[0].state == WebState::Done &&
                          items[0].file.find(L"01 Rewritten.m4a") != items[0].file.npos &&
                          !nativeaudio::IsFragmentedMp4(items[0].file) &&
                          !FileExists(root + L"\\Mira\\Sunsets\\01 Rewritten.remux.m4a") &&
                          !FileExists(root + L"\\Mira\\Sunsets\\01 Rewritten.part.m4a"),
                      L"a fragmented stream is saved as one standard M4A, leaving no temporary file");
                TagData tags;
                std::string art;
                Check(ReadTags(items[0].file, tags) && tags.title == "Rewritten" && tags.artist == "Mira" &&
                          tags.album == "Sunsets" && tags.track == 1 &&
                          ReadEmbeddedArt(items[0].file, art) && !art.empty(),
                      L"the rewritten file carries its tags and cover in the file itself");
            }

            // The refused shape still saves, as downloaded, with a note rather than a failure.
            WebTrack kept = Make("786", "Kept");
            kept.ext = "m4a";
            kept.streamUrl = "https://example.test/frag/786";
            fake.bodies["https://example.test/frag/786"] = SyntheticFragmentedMp4(4096);
            WebFetchRun run = makeRun({kept}, "Sunsets");
            std::vector<std::string> notices;
            run.notice = [&](const std::string& text) { notices.push_back(text); };
            std::vector<WebFetchItem> items;
            std::string runError;
            RunWebFetch(run, items, runError);
            bool noted = false;
            for (const std::string& n : notices) noted = noted || n.find("saved as downloaded") != n.npos;
            Check(items.size() == 1 && items[0].state == WebState::Done &&
                      nativeaudio::IsFragmentedMp4(items[0].file) && noted,
                  L"a stream the rewrite cannot express is saved as it arrived, with a note");
        }
    }

    batch.Next(L"a download that breaks is tried again next run, cancellation stops between tracks, and a "
               L"link from nowhere is refused without touching the network");
    {
        fake.failFetch = true;
        WebFetchRun run = makeRun({Make("781", "Halfway")}, "Sunsets");
        std::vector<WebFetchItem> broken;
        std::string error;
        RunWebFetch(run, broken, error);
        Check(broken[0].state == WebState::Failed && !broken[0].permanent &&
                  ReadTestBytes(WebFetchRecordPath(data)).find("781") == std::string::npos,
              L"a broken download is not remembered as a failure");
        fake.failFetch = false;

        fake.trip = &stop;
        run = makeRun({Make("782", "First"), Make("783", "Second")}, "Sunsets");
        run.cancel = &stop;
        std::vector<WebFetchItem> cancelled;
        RunWebFetch(run, cancelled, error);
        Check(cancelled.size() == 2 && cancelled[0].state == WebState::Done &&
                  cancelled[1].state == WebState::Resolving,
              L"cancellation finishes the track in flight and leaves the rest on the list");
        fake.trip = nullptr;
        stop = false;

        WebFetchRun nowhere;
        nowhere.links = {"https://example.com/not-music"};
        nowhere.libraryRoot = root;
        nowhere.dataDir = data;
        std::vector<WebFetchItem> none;
        std::string noneError;
        Check(!RunWebFetch(nowhere, none, noneError) && none.empty() &&
                  noneError.find("not from SoundCloud, YouTube or Spotify") != noneError.npos,
              L"a link from nowhere is refused in words, without touching the network");
    }

    batch.Next(L"a run names each link's list and its span of items, marking playlists apart "
               L"from albums and singles, and the span rebuilds the playlist's order from "
               L"mixed saved and already-owned rows");
    {
        // One playlist link with a row the record already knows and a fresh one, then a
        // single link after it: only the first is a list, and its order survives.
        fake.byLink["https://soundcloud.com/mira/sets/sunsets"] = {Make("901", "Old Sunset"),
                                                                  Make("902", "New Sunset")};
        fake.byLink["https://soundcloud.com/mira/storm"] = {Make("903", "Storm")};
        WebFetchRun run = makeRun({}, "Sunsets");
        run.links = {"https://soundcloud.com/mira/sets/sunsets", "https://soundcloud.com/mira/storm"};
        run.resolvers.link = [&fake](const std::string& link, const std::atomic<bool>&,
                                     std::vector<WebTrack>& tracks, std::string& listTitle, std::string&,
                                     const std::function<void(const std::string&)>&) {
            tracks = fake.byLink[link];
            listTitle = link.find("/sets/") != std::string::npos ? "Sunsets" : "";
            return true;
        };
        // The first run saves both rows; the second is the resume: one Exists, one Done.
        std::vector<WebFetchItem> first;
        std::vector<WebFetchList> firstLists;
        std::string error;
        Check(RunWebFetch(run, first, error, &firstLists) && first.size() == 3,
              L"the run read the playlist and the single after it");
        if (Ready(first.size() == 3, L"the first run's rows")) {
            Check(first[0].state == WebState::Done && first[1].state == WebState::Done,
                  L"the playlist's rows are saved");
            DeleteFileW(first[1].file.c_str());  // pretend the run was stopped before row two
        }
        std::vector<WebFetchItem> items;
        std::vector<WebFetchList> lists;
        Check(RunWebFetch(run, items, error, &lists) && items.size() == 3 && lists.size() == 2,
              L"the run read both links again");
        if (!Ready(items.size() == 3 && lists.size() == 2, L"the second run's rows")) return;
        Check(lists[0].title == "Sunsets" && lists[0].playlist && lists[0].first == 0 && lists[0].count == 2,
              L"the set is named, marked a playlist, and owns both rows");
        Check(lists[1].title.empty() && !lists[1].playlist && lists[1].first == 2 && lists[1].count == 1,
              L"a single link is named as no playlist, and owns only its own row");
        Check(items[0].state == WebState::Exists && items[1].state == WebState::Done,
              L"the ledger owns the finished row; the stopped one is fetched again");
        const std::vector<std::wstring> files =
            import::AssembleFiles(items, lists[0].first, import::RangeSlots(2), {});
        Check(files.size() == 2 && files[0] == items[0].file && files[1] == items[1].file,
              L"the playlist's order is the list's own, whatever each row's state was");
    }

    batch.Next(L"imported rows enter the queue as their own lists, match on YouTube, keep "
               L"their tags, and are remembered so a re-import is a resume, not a re-download");
    {
        WebFetchRun run = makeRun({}, "");
        run.links.clear();
        run.resolvers.matchSpotify = [](const WebTrack& want, const std::atomic<bool>&, WebTrack& matched,
                                        std::string& why) {
            if (want.id == "imp3") {
                // The fake-match convention: true with an empty stream is gone for good.
                why = "no YouTube upload of \"Import Gone\" is the same recording";
                return true;
            }
            matched = want;
            matched.streamUrl = "https://example.test/matched.mp3";
            matched.ext = "mp3";
            return true;
        };
        WebTrack one;
        one.site = "spotify";
        one.id = "imp1";
        one.title = "Import One";
        one.artist = "Mira";
        one.album = "Imported";
        WebTrack two = one;
        two.id = "imp2";
        two.title = "Import Two";
        WebTrack gone = one;
        gone.id = "imp3";
        gone.title = "Import Gone";
        run.importLists.push_back({"Imported", {one, two, gone}});
        std::vector<WebFetchItem> items;
        std::vector<WebFetchList> lists;
        std::string error;
        Check(RunWebFetch(run, items, error, &lists) && items.size() == 3 && lists.size() == 1,
              L"the imported rows run without a link");
        if (!Ready(items.size() == 3 && lists.size() == 1, L"the imported rows")) return;
        Check(lists[0].title == "Imported" && lists[0].playlist && lists[0].first == 0 && lists[0].count == 3,
              L"the import is one named playlist of its own");
        Check(items[0].state == WebState::Done && items[1].state == WebState::Done &&
                  items[0].file.find(L"Mira\\Imported\\Import One.mp3") != std::wstring::npos,
              L"an imported row keeps its own tags and files unnumbered under its album");
        Check(items[2].state == WebState::Failed && items[2].permanent,
              L"a row with no same-recording upload is gone for good, remembered like a removed track");
        const size_t downloads = fake.fetched["https://example.test/matched.mp3"];
        std::vector<WebFetchItem> again;
        std::vector<WebFetchList> againLists;
        RunWebFetch(run, again, error, &againLists);
        if (!Ready(again.size() == 3 && againLists.size() == 1, L"the re-imported rows")) return;
        Check(again[0].state == WebState::Exists && again[1].state == WebState::Exists &&
                  again[2].state == WebState::Failed &&
                  fake.fetched["https://example.test/matched.mp3"] == downloads,
              L"a re-import resumes: saved rows are reported, nothing is fetched again");
        const std::vector<std::wstring> files =
            import::AssembleFiles(again, againLists[0].first, import::RangeSlots(3), {});
        Check(files.size() == 2 && files[0] == again[0].file && files[1] == again[1].file,
              L"the playlist keeps the saved rows in the import's order and drops the gone one");
    }

    batch.Next(L"the tabs saves made are remembered across a restart, and a tab edited since no longer "
               L"matches, so it is never written over");
    {
        std::vector<WebTabMark> marks;
        Check(!LoadWebTabMarks(data, marks) && marks.empty(), L"a profile with no marks loads none");
        const std::vector<std::wstring> files = {root + L"\\Mira\\Sunsets\\01 Sunset.mp3",
                                                 root + L"\\Mira\\Sunsets\\02 Dusk.mp3"};
        const uint64_t seen = WebTabFingerprint(files);
        Check(seen != 0 && seen != WebTabFingerprint({files[1], files[0]}) &&
                  seen != WebTabFingerprint({files[0]}) && WebTabFingerprint({}) == 0,
              L"the fingerprint follows the files and their order, and no list is 0");
        SetWebTabMark(marks, L"Liked Songs", L"Liked Songs 2", seen);
        Check(marks.size() == 1 && SaveWebTabMarks(data, marks) && FileExists(WebTabMarkPath(data)),
              L"a mark is written beside the saved/failed record");
        std::vector<WebTabMark> loaded;
        Check(LoadWebTabMarks(data, loaded) && loaded.size() == 1 && loaded[0].title == L"Liked Songs" &&
                  loaded[0].name == L"Liked Songs 2" && loaded[0].seen == seen,
              L"the mark reads back with its list, tab and fingerprint");
        Check(FindWebTabMark(loaded, L"liked songs") != nullptr &&
                  FindWebTabMark(loaded, L"Other list") == nullptr,
              L"a mark is found by its list title, in any case");
        SetWebTabMark(loaded, L"Liked Songs", L"Liked Songs 3", seen ^ 1);
        Check(loaded.size() == 1 && loaded[0].name == L"Liked Songs 3" && loaded[0].seen == (seen ^ 1),
              L"writing the same list again replaces its tab and fingerprint");
        SetWebTabMark(loaded, L"Sunsets", L"Sunsets", 7);
        std::vector<WebTabMark> both;
        Check(loaded.size() == 2 && SaveWebTabMarks(data, loaded) && LoadWebTabMarks(data, both) &&
                  both.size() == 2 && FindWebTabMark(both, L"Sunsets") != nullptr,
              L"a second list's mark sits beside the first");
        Check(WebTabMarkPath(data) == data + L"\\webfetch-tabs.json",
              L"the marks live in the data folder as webfetch-tabs.json");
    }

    batch.Next(L"a Deezer or iTunes search track resolves through YouTube match and writes its provider comment");
    {
        WebTrack dz;
        dz.site = "deezer";
        dz.id = "dz-42";
        dz.title = "Nightfall";
        dz.artist = "Mira";
        dz.album = "Northbound";
        dz.durationSec = 180;
        WebTrack it;
        it.site = "itunes";
        it.id = "it-43";
        it.title = "Dawn";
        it.artist = "Mira";
        it.album = "Northbound";
        it.durationSec = 180;

        fake.bodies["https://example.test/dz-matched.mp3"] = mp3;
        WebFetchRun run = makeRun({}, "");
        WebFetchTrackList list;
        list.title = "Search";
        list.playlist = false;
        list.tracks = {dz, it};
        run.importLists.push_back(std::move(list));
        run.resolvers.matchSpotify = [](const WebTrack&, const std::atomic<bool>&, WebTrack& out, std::string&) {
            out.site = "youtube";
            out.id = "yt-dz";
            out.title = "Matched";
            out.artist = "Mira";
            out.streamUrl = "https://example.test/dz-matched.mp3";
            out.ext = "mp3";
            out.durationSec = 180;
            return true;
        };
        std::vector<WebFetchItem> items;
        std::string error;
        Check(RunWebFetch(run, items, error) && items.size() == 2 && items[0].state == WebState::Done &&
                  items[1].state == WebState::Done &&
                  items[0].file.find(L"Nightfall.mp3") != std::wstring::npos &&
                  items[1].file.find(L"Dawn.mp3") != std::wstring::npos,
              L"Deezer and iTunes tracks both resolve via YouTube match and keep their own tags");
        TagData tags0, tags1;
        Check(ReadTags(items[0].file, tags0) &&
                  tags0.comment.find("tags from Deezer") != std::string::npos &&
                  ReadTags(items[1].file, tags1) &&
                  tags1.comment.find("tags from iTunes") != std::string::npos,
              L"resolved search tracks record Deezer/iTunes provenance in comment");
    }

    batch.Next(L"a MusicBrainz album list files multi-disc tracks under AlbumArtist, caches its shared cover, "
               L"and writes full album metadata");
    {
        const std::string coverUrl = "https://example.test/mb-cover.jpg";
        size_t coverHits = 0;
        WebFetchIO mbIo = io;
        mbIo.fetchBytes = [&coverUrl, &coverHits](const std::string& url, std::string& bytes) {
            if (url == coverUrl) {
                ++coverHits;
                bytes = "\xff\xd8\xff\xe0\x00\x10JFIF";
                return true;
            }
            return false;
        };
        SetWebFetchIO(mbIo);

        WebTrack t1;
        t1.site = "musicbrainz";
        t1.id = "name:Mira feat. Guest\x1f" "First";
        t1.title = "First";
        t1.artist = "Mira feat. Guest";
        t1.album = "Double";
        t1.artworkUrl = coverUrl;
        t1.trackNo = 1;
        t1.trackTotal = 1;
        t1.discNo = 1;
        t1.discTotal = 2;
        t1.durationSec = 180;

        WebTrack t2 = t1;
        t2.id = "name:Mira\x1fSecond";
        t2.title = "Second";
        t2.artist = "Mira";
        t2.discNo = 2;
        t2.trackNo = 1;
        t2.trackTotal = 1;

        fake.bodies["https://example.test/mb.mp3"] = mp3;
        WebFetchRun run = makeRun({}, "");
        run.links.clear();
        WebFetchTrackList list;
        list.title = "Double";
        list.playlist = false;
        list.albumArtist = "Mira";
        list.date = "2025-04-10";
        list.releaseId = "66666666-6666-6666-6666-666666666666";
        list.releaseGroupId = "22222222-2222-2222-2222-222222222222";
        list.tracks = {t1, t2};
        run.importLists.push_back(std::move(list));
        run.resolvers.matchSpotify = [](const WebTrack&, const std::atomic<bool>&, WebTrack& out, std::string&) {
            out.site = "youtube";
            out.id = "yt-mb";
            out.title = "Matched";
            out.artist = "Mira";
            out.streamUrl = "https://example.test/mb.mp3";
            out.ext = "mp3";
            out.durationSec = 180;
            return true;
        };
        std::vector<WebFetchItem> items;
        std::vector<WebFetchList> lists;
        std::string error;
        Check(RunWebFetch(run, items, error, &lists) && items.size() == 2 &&
                  items[0].state == WebState::Done && items[1].state == WebState::Done &&
                  lists.size() == 1 && !lists[0].playlist && coverHits == 1,
              L"MusicBrainz album list saves all tracks with playlist=false and fetches shared cover once");
        Check(items[0].file.find(L"\\Mira\\Double\\1-01 First.mp3") != std::wstring::npos &&
                  items[1].file.find(L"\\Mira\\Double\\2-01 Second.mp3") != std::wstring::npos,
              L"multi-disc album tracks land under AlbumArtist\\Album\\D-NN Title.ext");
        TagData tags0, tags1;
        std::string embeddedCover;
        Check(ReadTags(items[0].file, tags0) && tags0.albumArtist == "Mira" && tags0.date.starts_with("2025") &&
                  tags0.disc == 1 && tags0.discTotal == 2 &&
                  tags0.musicBrainzRelease == "66666666-6666-6666-6666-666666666666" &&
                  tags0.musicBrainzReleaseGroup == "22222222-2222-2222-2222-222222222222" &&
                  ReadEmbeddedArt(items[0].file, embeddedCover) && !embeddedCover.empty() &&
                  tags0.comment.find("tags from MusicBrainz") != std::string::npos &&
                  ReadTags(items[1].file, tags1) && tags1.disc == 2 && tags1.discTotal == 2,
              L"MusicBrainz album tracks carry album artist, date, disc totals, MBIDs and cover art");
        SetWebFetchIO(io);
    }

    batch.Next(L"the record lives where its owner can find it");
    Check(WebFetchRecordPath(data) == data + L"\\webfetch.json", L"the record path is data\\webfetch.json");
}

// --webfetch-live <link>: the live check for the moving targets - one real link
// through the production queue, printed as it goes and saved into webfetch-live beside
// the companion, so a site that changed its streaming shape is caught from the command
// line without driving the window (the shipping player forwards its diagnostics here).
int RunWebFetchLive(const std::vector<std::wstring>& args) {
    if (args.size() < 2) {
        fwprintf(stderr, L"usage: cordinal-test.exe --webfetch-live <link>\n");
        return 2;
    }
    const std::wstring root = ExeFolder() + L"\\webfetch-live";
    CreateDirectoryW(root.c_str(), nullptr);
    std::atomic<bool> cancel = false;
    WebFetchRun run;
    run.links = {ToUtf8(args[1])};
    run.libraryRoot = root;
    run.dataDir = root;
    run.cancel = &cancel;
    run.added = [](const WebFetchItem& it) {
        std::wstring who = FromUtf8(it.track.artist.empty() ? it.track.title
                                                            : it.track.artist + " - " + it.track.title);
        fwprintf(stdout, L"found  %s\n", who.c_str());
    };
    run.updated = [](size_t, const WebFetchItem& it) {
        if (it.state == WebState::Done) fwprintf(stdout, L"saved  %s\n", it.file.c_str());
        else if (it.state == WebState::Exists) fwprintf(stdout, L"had    %s\n", it.file.c_str());
        else if (it.state == WebState::Failed) fwprintf(stdout, L"failed %s\n", FromUtf8(it.why).c_str());
    };
    run.notice = [](const std::string& text) { fwprintf(stdout, L"...    %s\n", FromUtf8(text).c_str()); };
    std::vector<WebFetchItem> items;
    std::string error;
    bool ok = RunWebFetch(run, items, error);
    unsigned saved = 0, had = 0, failed = 0;
    for (const WebFetchItem& it : items) {
        if (it.state == WebState::Done) saved++;
        else if (it.state == WebState::Exists) had++;
        else if (it.state == WebState::Failed) failed++;
    }
    fwprintf(stdout, L"%u saved, %u already there, %u failed%s\n", saved, had, failed,
             error.empty() ? L"" : FromUtf8("  (" + error + ")").c_str());
    return ok && failed == 0 ? 0 : 1;
}
