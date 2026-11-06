// Optional online metadata: album covers from keyless web services, chosen by hand or fetched for
// albums that have none, lyrics for songs that carry none, and a short artist biography.
// Everything here is off until the user turns a source on in Preferences.
// The searches run on worker threads the front ends own; this module holds the providers' reply
// parsers, the scoring, the checks a downloaded image must pass, the shared per-host rate limits,
// a small on-disk cache and the atomic folder write. The parsers and the scorer are pure so the
// self-tests can feed them canned replies without touching the network.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace online {

// The header block every request sends: cordinal's real user agent and the JSON it accepts.
extern const char kRequestHeaders[];

// Cancels a search from any thread. On Windows it closes the WinHTTP session the requests share,
// which ends one in flight at once; on Linux HttpFetch watches the flag itself.
class Cancel {
public:
    Cancel();
    ~Cancel();
    Cancel(const Cancel&) = delete;
    Cancel& operator=(const Cancel&) = delete;
    void Stop();
    bool Stopped() const { return stop_.load(); }
    void* Session() const;  // HttpFetch's session argument
    const std::atomic<bool>& Flag() const { return stop_; }

private:
    std::atomic<bool> stop_{false};
    std::atomic<void*> session_{nullptr};
};

// Waits until `host` may be asked again. MusicBrainz and the Cover Art Archive allow one request a
// second and the iTunes Search API about one every three; every worker in the process (the
// album-edition lookup too) shares these slots. False when `stop` was set while waiting.
bool WaitTurn(std::string_view host, const std::atomic<bool>* stop);

struct Reply {
    unsigned status = 0;  // 0 when the request failed, was cancelled or the body was too big
    std::string body;
};
// A GET with kRequestHeaders that follows up to four HTTPS redirects, waiting its turn on
// rate-limited hosts. Plain http:// is refused.
Reply Get(const std::string& url, Cancel& cancel, unsigned timeoutMs, size_t maxBody = 1u << 20);
std::string UrlEncode(std::string_view text);

// ---------------------------------------------------------------------------------------------
// Covers

enum class CoverSource : uint8_t { Deezer, ITunes, MusicBrainz, Count };
constexpr int kCoverSources = (int)CoverSource::Count;
const wchar_t* CoverSourceName(CoverSource source);   // "Deezer"
const char* CoverSourceHost(CoverSource source);      // "api.deezer.com", for the privacy lines

struct CoverQuery {
    std::string artist, album;  // UTF-8; album may be empty (a loose single)
    std::string title;          // the song, which FindCoverUrl searches by when there is no album
};

struct CoverCandidate {
    CoverSource source = CoverSource::Deezer;
    std::string artist, album;  // as the provider names it
    std::string url;            // the image to download
    int width = 0, height = 0;  // as declared (0: unknown), then as decoded
    double score = 0;
    std::string title;          // track searches: the song the cover belongs to
};

// Each provider's search reply, reduced to candidates. Malformed JSON, an error object or an
// empty result yields none; entries without artwork are left out.
std::vector<CoverCandidate> ParseDeezerAlbums(std::string_view json);
std::vector<CoverCandidate> ParseItunesAlbums(std::string_view json);
// Releases scoring at least 80; the image is the Cover Art Archive's 1200-pixel front.
std::vector<CoverCandidate> ParseMusicBrainzReleases(std::string_view json);
// Song searches, for a track with no album: each result's album cover, with the song in `title`.
std::vector<CoverCandidate> ParseDeezerTracks(std::string_view json);
std::vector<CoverCandidate> ParseItunesTracks(std::string_view json);

// A title or name reduced for a forgiving comparison: lower case, with bracketed notes ("(Deluxe Edition)",
// "[2011 Remaster]"), a trailing edition note after " - ", a "feat." credit, "&" and punctuation left out. Two
// names that reduce alike are taken as the same. A name that is nothing but notes keeps them.
std::string LooseTitle(std::string_view text);

// score = provider quality + match + image size, with a small bonus for the user's first choices.
//   match: +0.5 for the same artist, +0.5 for the same album (case-insensitive), -1.5 when both
//          differ; with no album: -1 for another artist, -4 for a compilation or Various Artists,
//          -3 for a generic title ("Greatest Hits") and -0.5 for a soundtrack.
//   image: sqrt(w*h)/500 + (1 - |w-h|/max(w,h)), a guess of 500 pixels while the size is unknown.
double MatchScore(const CoverQuery& query, std::string_view artist, std::string_view album);
double ImageScore(int width, int height);
double ProviderQuality(CoverSource source);
// Scores every candidate and sorts best first; `order` is the user's provider order.
void RankCovers(const CoverQuery& query, std::vector<CoverCandidate>& candidates, const std::vector<CoverSource>& order);
// The batch's bar for a pick that starts checked: the provider names this very artist and album
// and the image is at least 500 pixels square-ish.
bool ConfidentCover(const CoverQuery& query, const CoverCandidate& candidate);

// Downloaded images are untrusted. Only complete JPEG and PNG files up to kMaxImageBytes pass;
// the size comes from the header and is confirmed by the platform decoder afterwards.
enum class ImageType : uint8_t { None, Jpeg, Png };
constexpr size_t kMaxImageBytes = 20u << 20;
ImageType InspectImage(std::string_view bytes, int& width, int& height);

// Decodes `bytes` completely (WIC on Windows, stb_image on Linux) to prove it is an image and to
// read its real size; with `thumbSize` > 0 it also scales a copy into `thumb` (0x00RRGGBB pixels).
struct Decoded {
    int width = 0, height = 0;
    int thumbWidth = 0, thumbHeight = 0;
    std::vector<uint32_t> thumb;
};
using ImageDecoder = std::function<bool(const std::string& bytes, int thumbSize, Decoded& out)>;

struct CoverFound {
    std::string origin;  // the user's own image: where it came from ("From a file"); "" when found
    CoverCandidate candidate;
    std::string image;  // the validated JPEG or PNG
    Decoded decoded;
};

struct CoverSearch {
    CoverQuery query;
    std::vector<CoverSource> sources;  // the enabled ones, in the user's order
    bool manual = false;               // every usable result, or only the best one
    int thumbSize = 0;
};
// Blocking: runs on a worker. The providers are asked in parallel and the whole search ends after
// 20 seconds; each image download may take 6. A manual search reports every usable cover through
// `found` as it is ready (one call at a time). A batch search downloads the best candidates in
// rounds of three, rescores them with their real size and reports only the winner. True when
// anything was found.
bool SearchCovers(const CoverSearch& search, Cancel& cancel, const ImageDecoder& decode,
                  const std::function<void(CoverFound&&)>& found);
// For a cover shown elsewhere (the Discord status): the address of the album's cover, without downloading it. The
// artist and album must match as LooseTitle compares them; with no album, the artist and song title are searched
// instead and the song's album cover is used. With MusicBrainz among `sources`, the release id from the tags is tried
// first. A Cover Art Archive address is confirmed with its small thumbnail, as a release may have no art. Blocking, on
// a worker; remembered in the cache like batch results. Empty when nothing qualifies.
std::string FindCoverUrl(const CoverQuery& query, const std::string& release, const std::vector<CoverSource>& sources,
                         Cancel& cancel);

// A cover the user hands over as text, pasted or dropped from a browser: a data: URL holding a
// base64 JPEG or PNG (Google Images' thumbnails), or the https address of one, which is downloaded
// (blocking: runs on a worker; it may take 15 seconds). NotImage when the text is neither; Failed
// when the address gave nothing that passes the image checks above. The platform decoder still
// has to accept `image`.
// When `sourceUrl` is given it receives the https address the image was downloaded from, so the Discord status
// can show the same picture; it is cleared when the text was not an address.
enum class TextImage : uint8_t { NotImage, Got, Failed };
TextImage ImageFromText(std::string_view text, Cancel& cancel, std::string& image, std::string* sourceUrl = nullptr);

// True when the folder already has a cover file art.cpp would show (cover/folder/front .jpg/.png).
bool FolderHasCover(const std::wstring& dir);
// Saves `image` as cover.jpg (cover.png for PNG data) through a temporary file renamed into place,
// so the folder never holds half a cover. An existing cover file is replaced only with `overwrite`.
enum class FolderWrite : uint8_t { Written, Exists, Failed };
FolderWrite WriteFolderCover(const std::wstring& dir, const std::string& image, bool overwrite, std::wstring& error);

// ---------------------------------------------------------------------------------------------
// Lyrics, asked for only when a song has none in its tags or a .lrc file beside it

enum class LyricsSource : uint8_t { LrcLib, LyricsOvh, Count };
constexpr int kLyricsSources = (int)LyricsSource::Count;
const wchar_t* LyricsSourceHost(LyricsSource source);  // "lrclib.net", shown under the lyrics

struct LyricsQuery {
    std::string artist, album, title;  // UTF-8
    unsigned seconds = 0;              // the track's length; LrcLib matches by it
};

struct LyricsFound {
    LyricsSource source = LyricsSource::LrcLib;
    std::string artist, album, title;  // as the provider names the song
    std::string text;                  // LRC when synced, else plain lines
    bool synced = false;
    double score = 0;
};

// LrcLib's reply to /api/get (one object) or /api/search (an array): synced lyrics preferred, then
// plain ones; instrumental entries, entries without lyrics and malformed replies yield none.
std::vector<LyricsFound> ParseLrcLib(std::string_view json);
// lyrics.ovh names nothing but the lyrics, so they are credited to the song that was asked for.
std::vector<LyricsFound> ParseLyricsOvh(std::string_view json, const LyricsQuery& query);
// +0.5 for each of the same artist, album and title (case-insensitive), -1.5 when both artist and
// title differ, and +1 for more than 60 characters of text.
double LyricsScore(const LyricsQuery& query, const LyricsFound& found);
// The best usable result: its title must match and it must score at least 1.5; synced lyrics win
// over plain ones. False when none is usable.
bool BestLyrics(const LyricsQuery& query, std::vector<LyricsFound> found, LyricsFound& best);
// Blocking: runs on a worker. LrcLib first, then lyrics.ovh, each only when enabled; the answer
// (or that there was none) is cached. The search ends after 15 seconds; each request may take 6.
bool FetchLyrics(const LyricsQuery& query, Cancel& cancel, LyricsFound& out);
// Saves `text` as the .lrc file beside `audioPath`, through a temporary file renamed into place.
// An existing .lrc is never replaced (FolderWrite::Exists).
FolderWrite WriteLrcFile(const std::wstring& audioPath, const std::string& text, std::wstring& error);

// ---------------------------------------------------------------------------------------------
// Artist info: a short biography and links, from MusicBrainz and the English Wikipedia

struct ArtistInfo {
    std::string name, mbid;   // as MusicBrainz names the artist
    std::string summary;      // the Wikipedia article's opening, plain text; may be empty
    std::string musicbrainz;  // https://musicbrainz.org/artist/<mbid>
    std::string wikipedia;    // the article, when there is one
    std::string official;     // the official site, when MusicBrainz lists one (https or http only)
    std::string wikidata;     // the Wikidata item id ("Q11649"), used to find the article
};

// A conservative search: the id of the one artist scoring at least 90 whose name is exactly `name`
// (ignoring case). Two such artists, or none, yields empty, so a namesake is never shown.
std::string ParseArtistSearch(std::string_view json, std::string_view name);
// The artist lookup with its URL relations: name, links and the Wikidata item. False when malformed.
bool ParseArtistLookup(std::string_view json, ArtistInfo& info);
// Wikidata's English Wikipedia title for item `qid` (wbgetentities with sitefilter=enwiki).
std::string ParseWikidataTitle(std::string_view json, std::string_view qid);
// The Wikipedia REST summary's extract and page address; a disambiguation page yields false.
bool ParseWikipediaSummary(std::string_view json, std::string& extract, std::string& page);
// Blocking: runs on a worker. Search (or `mbid` when known), lookup, then the summary; cached for
// 30 days (a miss for one). False when the artist couldn't be identified without doubt.
bool FetchArtistInfo(const std::string& name, const std::string& mbid, Cancel& cancel, ArtistInfo& out);

// ---------------------------------------------------------------------------------------------
// Settings shared by both front ends; each loads and saves them with its own preferences.

// Custom keeps the files as they are and shows the cover in cordinal only (customcover.h).
enum class CoverSave : uint8_t { Folder, Embed, Custom };
struct Settings {
    bool covers[kCoverSources] = {};  // per provider, all off
    int coverFirst = 0;               // the provider asked first and preferred on ties
    int coverSave = 2;                // CoverSave
    bool coverOverwrite = false;      // replace an existing cover.jpg
    bool lyrics[kLyricsSources] = {};  // per lyrics provider, all off
    bool artistInfo = false;           // MusicBrainz and Wikipedia for Artist info...
};
Settings& Config();
std::vector<CoverSource> CoverOrder();  // the enabled providers, the preferred one first
bool AnyCoverSource();
bool AnyLyricsSource();

// Starts the on-disk cache in `dataDir` (online-cache.txt); call once before searching.
void Start(const std::wstring& dataDir);
void Stop();  // saves the cache
// The cache itself: answers found stay 30 days, misses one. False when there is no fresh entry.
bool CacheGet(const std::string& key, std::string& value, bool& found);
void CachePut(const std::string& key, const std::string& value, bool found);

// Results arrive by message long after they were asked for. A front end bumps a Generation when
// what the user is looking at changes; a result tagged with an older one is dropped.
class Generation {
public:
    uint64_t Next() { return ++value_; }
    uint64_t Current() const { return value_; }
    bool Accepts(uint64_t tag) const { return tag != 0 && tag == value_; }

private:
    uint64_t value_ = 0;
};

}  // namespace online
