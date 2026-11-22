// Owning your music: saving tracks from the web into the library as real, tagged files.
// The resolvers (soundcloud.h, ytaudio.h, spotifylink.h) turn a pasted link into
// WebTracks; this queue downloads each one, embeds its tags and cover, and files it as
// <root>\Artist\Album\[NN ]Title.ext. What has been saved - and what failed for good -
// is remembered beside the library, so nothing is fetched twice and dead tracks are
// never retried. The queue runs on a worker thread; every callback it makes belongs to
// that thread.
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <vector>

// One item a link resolved to.
struct WebTrack {
    std::string site;        // "soundcloud", "youtube" or "spotify": where the link came from
    std::string id;         // that site's own id for the item, unique together with `site`
    std::string title;      // the plain title
    std::string artist;     // the artist as the site states it ("" when unknown)
    std::string album;      // the album or set it came from ("" for a single)
    std::string artworkUrl; // a JPEG the site serves ("" when there is none)
    std::string streamUrl;  // direct HTTP audio, ready to fetch ("" when hlsOnly or unavailable)
    std::string ext;        // "mp3", "m4a" or "webm": what the stream is ("" until known)
    bool hls = false;       // streamUrl is an HLS entry: saved by joining its init and media segments
    bool hlsOnly = false;   // only an HLS flavour saving does not take; reported instead of saved broken
    unsigned durationSec = 0;
    unsigned trackNo = 0, trackTotal = 0;  // from an album or playlist listing, else 0
    unsigned discNo = 0, discTotal = 0;    // multi-disc numbering when known, else 0
    bool unavailable = false;  // removed, private or blocked: remembered, never retried
    std::string why;            // the words to show for unavailable tracks
};

// A row of the saving list, from resolution to its final state.
enum class WebState { Resolving, Downloading, Saving, Done, Exists, Failed };
struct WebFetchItem {
    WebTrack track;
    WebState state = WebState::Resolving;
    int percent = -1;        // while Downloading, else -1
    std::wstring file;       // set once Done or Exists
    std::string why;         // set once Failed
    bool permanent = false;  // true when another run would fail the same way
};

// What one run is asked to do. `links` are as pasted; `importLists` are rows a Spotify export
// or a CSV already resolved (playlist_import.h), entering the queue with no link to read.
// `libraryRoot` is the folder the library lives in: Artist\Album folders are made under it.
// `dataDir` is the player's own data folder, where the saved/failed record lives.
struct WebFetchTrackList {
    std::string title;             // the list's own name, for the tab the UI can offer
    std::vector<WebTrack> tracks;  // resolved rows, in the list's exact order
    bool playlist = true;          // false for an album save: it lands as files, not a tab
    std::string albumArtist, date, releaseId, releaseGroupId;  // shared album metadata, if any
};

// One link's span of the run's items: where its tracks sit, the set's own title, and whether
// it is a playlist (a Spotify playlist, a YouTube playlist, a SoundCloud set or likes, or an
// imported list) rather than an album or a single, so the UI can offer it as one cordinal
// playlist once the run is through.
struct WebFetchList {
    std::string title;
    bool playlist = false;
    size_t first = 0, count = 0;
};

struct WebFetchRun {
    std::vector<std::string> links;
    std::vector<WebFetchTrackList> importLists;
    std::wstring libraryRoot, dataDir;
    const std::atomic<bool>* cancel = nullptr;
    std::function<void(const WebFetchItem&)> added;                  // a track joined the list
    std::function<void(size_t index, const WebFetchItem&)> updated;  // state or percent changed
    std::function<void(const std::string&)> notice;                   // "Reading playlist..."

    // Resolvers, overridable so the self-test can run the whole queue offline (the
    // site modules' own tests are parser-level). Null entries use the real modules.
    // A fake link resolver's tracks carry their own `site`, and its listTitle plays
    // the set title (an empty one means the link was not a playlist); a fake match
    // follows MatchSpotifyTrack's convention: false means a search that did not
    // answer, true with an empty stream means gone for good.
    struct Resolvers {
        std::function<bool(const std::string& link, const std::atomic<bool>& cancel,
                           std::vector<WebTrack>& tracks, std::string& listTitle, std::string& error,
                           const std::function<void(const std::string&)>& notice)> link;
        std::function<bool(const WebTrack& want, const std::atomic<bool>& cancel, WebTrack& matched,
                           std::string& why)> matchSpotify;
    } resolvers;
};

// Resolves every link, then saves each track the record does not already know. False
// only when nothing could be read at all; individual failures land in `items` with
// their `why`, and a track that only streams as HLS is reported, not saved broken.
// `lists`, when given, receives one WebFetchList per read link and import list, in
// run order, naming the span of `items` each one owns.
bool RunWebFetch(const WebFetchRun& run, std::vector<WebFetchItem>& items, std::string& error,
                 std::vector<WebFetchList>* lists = nullptr);

// The saved/failed record's path, for the self-test and the "forget what failed"
// command, which clear it.
std::wstring WebFetchRecordPath(const std::wstring& dataDir);

// Keeps a preview downloaded for Keep playing like this (discovery.h): copies the already-tagged
// file to <root>\Artist\Album\[NN ]Title.ext, never over an existing file, and records it in the
// saved/failed record so a later save or import sees it as already yours. The preview itself is
// left alone; the caller deletes it once nothing plays or queues it.
bool WebFetchKeep(const WebTrack& track, const std::wstring& previewFile, const std::wstring& libraryRoot,
                  const std::wstring& dataDir, std::wstring& keptFile, std::string& error);

// Clears the remembered failures, so "try those again" is a button, not a file edit:
// the next run downloads them instead of leaving them alone.
bool WebFetchForgetFailures(const std::wstring& dataDir);

// The playlist tabs web saves and imports made, remembered in webfetch-tabs.json beside
// the record so re-importing the same list after a restart updates its own tab rather
// than making a second one. `seen` is a fingerprint of the tab's files when cordinal
// last wrote it: a tab edited since no longer matches, so it is never overwritten and a
// new tab is made beside it instead. The UI layer loads the marks, adopts the tab whose
// name and fingerprint still match, and saves the mark it rewrote.
struct WebTabMark {
    std::wstring title;  // the list's own name, as the site or export gave it
    std::wstring name;   // the tab cordinal made (a free name when the title was taken)
    uint64_t seen = 0;
};
std::wstring WebTabMarkPath(const std::wstring& dataDir);  // ...\webfetch-tabs.json
bool LoadWebTabMarks(const std::wstring& dataDir, std::vector<WebTabMark>& marks);
bool SaveWebTabMarks(const std::wstring& dataDir, const std::vector<WebTabMark>& marks);
// A stable fingerprint of an ordered file list, 0 when it is empty: what `seen` holds.
uint64_t WebTabFingerprint(const std::vector<std::wstring>& files);
// The mark for a list, by title (case-insensitive, like playlist names on screen), or null.
const WebTabMark* FindWebTabMark(const std::vector<WebTabMark>& marks, const std::wstring& title);
// Records (or replaces) the mark for `title`, so the tab it names is found again next time.
void SetWebTabMark(std::vector<WebTabMark>& marks, const std::wstring& title, const std::wstring& name,
                   uint64_t seen);

// For the self-test: how the queue turns a resolved stream URL into bytes on disk and
// how it fetches a cover. Production (an empty WebFetchIO) downloads over HTTP through
// the platform reader, which the radio suite already exercises against a loopback
// server, so a fake here is for forcing failure paths, not for replaying HTTP.
// `progress` gets 0-100 while the stream arrives, already de-duplicated. `fetchHls`
// takes the whole track because an HLS save follows the entry's own signed redirects.
struct WebFetchIO {
    std::function<bool(const std::string& url, const std::wstring& file,
                       const std::function<void(int)>& progress)> fetch;
    std::function<bool(const WebTrack& track, const std::wstring& file,
                       const std::function<void(int)>& progress)> fetchHls;
    std::function<bool(const std::string& url, std::string& bytes)> fetchBytes;
};
void SetWebFetchIO(const WebFetchIO& io);  // empty functions restore production
