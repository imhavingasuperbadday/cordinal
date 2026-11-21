// The saving queue end to end, offline: a fake resolver and fake IO stand in for the
// sites, so every rule the queue owns is checked without the internet - filed names,
// "never download the same song twice", deleted files fetched again, collisions
// numbered instead of replaced, failures remembered (and only the permanent ones),
// the Spotify match flow, and cancellation.
#include "selftest_common.h"
#include "webfetch.h"

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
