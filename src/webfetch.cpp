// The queue behind saving a web link into your library: every pasted link is resolved
// by its site module (soundcloud.cpp, ytaudio.cpp, spotifylink.cpp), each track's
// stream is downloaded beside its destination, tagged with its cover, and renamed into
// place as <root>\Artist\Album\[NN ]Title.ext. A small record beside the library
// remembers what has been saved and what failed for good, so nothing is fetched twice
// and dead tracks are left alone until asked for again. Everything here runs on the
// caller's one worker thread; the callbacks it makes belong to that thread too.
#include "webfetch.h"

#include "library.h"    // ToUtf8 / FromUtf8
#include "metadata_fetch.h"
#include "mp4_remux.h"   // RemuxFragmentedMp4, IsFragmentedMp4
#include "organize.h"   // SafePathComponent
#include "radio_http.h"
#include "soundcloud.h"
#include "spotifylink.h"
#include "tagwriter.h"  // WriteTags, ReadFileBytes, ReplaceFileBytes
#include "web_remote.h"  // ParseJson for the record
#include "ytaudio.h"

#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <map>
#include <thread>

namespace {

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool IsCancel(const std::atomic<bool>* cancel) { return cancel && cancel->load(std::memory_order_relaxed); }

// A short pause between requests to one site, so a long list downloads at a steady
// pace instead of all at once - the same caution soundcli applies, because YouTube
// starts refusing downloads that come too fast.
void Pace(int ms, const std::atomic<bool>& cancel) {
    for (int left = ms; left > 0 && !cancel.load(std::memory_order_relaxed); left -= 50)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

std::wstring Join(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + kPathSep + b;
}

// Makes every level of `dir` that does not exist yet.
void EnsureFolder(std::wstring dir) {
    for (size_t slash = 0; ; slash = dir.find(kPathSep, slash + 1)) {
        if (slash == std::wstring::npos) break;
        if (slash + 1 >= dir.size()) break;
        std::wstring parent = dir.substr(0, slash + 1);
        if (parent.size() < 4) continue;  // "C:\", and on Linux the single root slash
        CreateDirectoryW(parent.c_str(), nullptr);
    }
    CreateDirectoryW(dir.c_str(), nullptr);
}

std::wstring FolderOf(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"" : path.substr(0, slash);
}

std::string RecordKey(const WebTrack& t) { return t.site + '\x1f' + t.id; }

// ---------------------------------------------------------------------------
// The saved/failed record: webfetch.json in the data folder, written atomically, so
// a run that is cut off leaves either the old record or the new one, never a torn
// one. Paths are kept relative to the library root, so moving the library keeps the
// record honest.

struct Record {
    std::map<std::string, std::wstring> saved;                       // key -> path under the root
    std::map<std::string, std::pair<std::string, bool>> failed;     // key -> why, permanent
};

std::string JsonEscape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += (char)c;
            }
        }
    }
    return out;
}

bool LoadRecord(const std::wstring& path, Record& record) {
    std::string text;
    if (!ReadFileBytes(path, text) || text.empty()) return false;
    web::JsonValue json;
    if (!web::ParseJson(text, json)) return false;
    for (const web::JsonValue& row : json["saved"].items) {
        if (!row["k"].text.empty()) record.saved[row["k"].text] = FromUtf8(row["f"].text);
    }
    for (const web::JsonValue& row : json["failed"].items) {
        if (!row["k"].text.empty())
            record.failed[row["k"].text] = {row["w"].text, row["p"].text == "true"};
    }
    return true;
}

// Writes `text` beside `path` and moves it into place: the old file or the new one is on
// disk, never a torn one. The first write has no original for ReplaceFile to swap, so the
// temp file is moved in instead - the pattern playlist saves use.
bool WriteAtomic(const std::wstring& path, const std::string& text) {
    std::wstring tmp = path + L".cordinal-tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    for (size_t pos = 0; ok && pos < text.size();) {
        DWORD chunk = (DWORD)std::min<size_t>(text.size() - pos, 1u << 24), written = 0;
        ok = WriteFile(h, text.data() + pos, chunk, &written, nullptr) && written == chunk;
        pos += chunk;
    }
    ok = ok && FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

bool SaveRecord(const std::wstring& path, const Record& record) {
    std::string out = "{\"saved\":[";
    bool first = true;
    for (const auto& [key, rel] : record.saved) {
        out += first ? "" : ",";
        first = false;
        out += "{\"k\":\"" + JsonEscape(key) + "\",\"f\":\"" + JsonEscape(ToUtf8(rel)) + "\"}";
    }
    out += "],\"failed\":[";
    first = true;
    for (const auto& [key, why] : record.failed) {
        out += first ? "" : ",";
        first = false;
        out += "{\"k\":\"" + JsonEscape(key) + "\",\"w\":\"" + JsonEscape(why.first) + "\",\"p\":" +
               (why.second ? "true" : "false") + "}";
    }
    out += "]}";
    return WriteAtomic(path, out);
}

// ---------------------------------------------------------------------------
// The playlist tabs the saves and imports made: webfetch-tabs.json beside the record,
// written atomically like it, so a re-import after a restart updates its own tab.

// FNV-1a over each file's UTF-8 bytes and a newline, in order. 0 marks "no list": a saved
// tab always has files.
uint64_t FingerprintFiles(const std::vector<std::wstring>& files) {
    if (files.empty()) return 0;
    uint64_t hash = 1469598103934665603ull;
    for (const std::wstring& file : files) {
        for (unsigned char c : ToUtf8(file)) {
            hash ^= c;
            hash *= 1099511628211ull;
        }
        hash ^= '\n';
        hash *= 1099511628211ull;
    }
    return hash ? hash : 1;
}

std::string Hex64(uint64_t value) {
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long)value);
    return buf;
}

// ---------------------------------------------------------------------------
// Where a track files itself: <root>\Artist\Album\[NN ]Title.ext, every part made
// safe. A single lands directly in the artist's folder; what the site did not name
// goes to "Unknown artist" so it is never lost in the root.

std::wstring ExtOfTrack(const WebTrack& t) {
    if (!t.ext.empty()) return FromUtf8(t.ext);
    // The stream itself names the format: YouTube puts the itag and mime in the URL,
    // SoundCloud's progressive stream is always the MP3 the site plays.
    if (t.site == "youtube" || !t.streamUrl.empty()) {
        if (t.streamUrl.find("itag=140") != std::string::npos || t.streamUrl.find("itag=139") != std::string::npos ||
            t.streamUrl.find("audio%2Fmp4") != std::string::npos || t.streamUrl.find("audio/mp4") != std::string::npos)
            return L"m4a";
        if (t.streamUrl.find("audio%2Fwebm") != std::string::npos || t.streamUrl.find("audio/webm") != std::string::npos)
            return L"webm";
    }
    return L"mp3";
}

std::wstring TargetPath(const WebTrack& t, const std::wstring& root, std::string_view albumArtist = {}) {
    std::vector<std::wstring> parts;
    const std::string_view folderArtist =
        !albumArtist.empty() ? albumArtist : (t.artist.empty() ? std::string_view("Unknown artist") : std::string_view(t.artist));
    parts.push_back(SafePathComponent(FromUtf8(std::string(folderArtist))));
    if (!t.album.empty()) parts.push_back(SafePathComponent(FromUtf8(t.album)));
    std::wstring name = FromUtf8(t.title.empty() ? t.id : t.title);
    if (t.trackNo > 0 && t.trackNo <= 999) {
        wchar_t no[16];
        if (t.discTotal > 1 && t.discNo > 0 && t.discNo <= 99)
            std::swprintf(no, 16, L"%u-%02u ", t.discNo, t.trackNo);
        else
            std::swprintf(no, 16, L"%02u ", t.trackNo);
        name = no + name;
    }
    parts.push_back(SafePathComponent(name) + L"." + ExtOfTrack(t));
    std::wstring path = root;
    for (const std::wstring& part : parts) path = Join(path, part);
    return path;
}

// Another file already sits at the name, and the record does not own it: a second
// recording with the same name is numbered the way organize numbers collisions, so
// one never replaces the other.
std::wstring FirstFreePath(std::wstring path) {
    if (!Exists(path)) return path;
    size_t dot = path.find_last_of(L'.');
    std::wstring stem = dot == std::wstring::npos ? path : path.substr(0, dot);
    std::wstring ext = dot == std::wstring::npos ? L"" : path.substr(dot);
    for (unsigned n = 2; n < 100; ++n) {
        std::wstring tryPath = stem + L" (" + std::to_wstring(n) + L")" + ext;
        if (!Exists(tryPath)) return tryPath;
    }
    return path;
}

// ---------------------------------------------------------------------------
// Production IO: one streamed download through the platform reader. Bytes land on
// disk as they arrive, so a cancelled run never leaves a complete-looking file.

bool DownloadToFile(const std::string& url, const std::wstring& file, const std::atomic<bool>& cancel,
                    const std::function<void(int)>& progress) {
    HANDLE out = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) return false;
    // The file is fetched in ranged pieces, the way Puto fetches googlevideo: a single
    // un-ranged GET is throttled to a crawl, while range requests run at full speed. A
    // server that answers 206 is ranged; one that answers 200 to the first piece has
    // ignored ranges and sent the whole file in that one stream already.
    constexpr unsigned long long kPiece = 8u << 20;
    unsigned long long got = 0, total = 0;
    int lastPercent = -1;
    bool done = false, ok = false;
    while (!done) {
        if (cancel.load(std::memory_order_relaxed)) break;
        const unsigned long long from = got;
        char request[80];
        std::snprintf(request, sizeof request, "Range: bytes=%llu-%llu\r\n", from, from + kPiece - 1);
        unsigned status = 0;
        unsigned long long pieceBytes = 0;
        auto onHeaders = [&](unsigned code, const radio::Headers& headers) {
            status = code;
            if (code != 200 && code != 206) return false;
            // Ranges ignored in mid-file would restart the bytes; only the first piece
            // may come back as a whole-file 200.
            if (code == 200 && from > 0) return false;
            for (const auto& [name, value] : headers) {
                std::string low = name;
                std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return (char)std::tolower(c); });
                if (low == "content-range" && total == 0) {
                    const size_t slash = value.find('/');
                    if (slash != std::string::npos)
                        total = std::strtoull(value.c_str() + slash + 1, nullptr, 10);
                }
            }
            return true;
        };
        auto onData = [&](std::string_view bytes) {
            DWORD at = 0;
            while (at < bytes.size()) {
                DWORD chunk = (DWORD)std::min<size_t>(bytes.size() - at, 1u << 20);
                if (!WriteFile(out, bytes.data() + at, chunk, &chunk, nullptr)) return false;
                at += chunk;
            }
            pieceBytes += bytes.size();
            got += bytes.size();
            if (progress && total > 0) {
                int percent = (int)(got * 100 / total);
                if (percent != lastPercent) {
                    lastPercent = percent;
                    progress(percent);
                }
            }
            return true;
        };
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (attempt == 1) {
                // A torn piece rewinds to its start, so the retry cannot leave a gap.
                got = from;
                pieceBytes = 0;
                LARGE_INTEGER rewind{};
                rewind.QuadPart = (LONGLONG)from;
                if (!SetFilePointerEx(out, rewind, nullptr, FILE_BEGIN)) break;
                SetEndOfFile(out);
            }
            ok = radio::HttpStream(url, cancel, onHeaders, onData, request);
            if (ok || cancel.load(std::memory_order_relaxed)) break;
        }
        if (!ok || pieceBytes == 0) break;   // nothing came, or nothing more to come
        if (status == 200) {
            done = true;  // the server ignored ranges and sent everything in one stream
            break;
        }
        if (total != 0 && got >= total) done = true;
    }
    CloseHandle(out);
    if (!done) {
        DeleteFileW(file.c_str());
        return false;
    }
    return true;
}

bool FetchBytes(const std::string& url, std::string& bytes) {
    if (url.starts_with("https://")) {
        online::Cancel cancel;
        online::Reply reply = online::Get(url, cancel, 30000, 8u << 20);
        int w = 0, h = 0;
        if (reply.status != 200 || online::InspectImage(reply.body, w, h) == online::ImageType::None) return false;
        bytes = std::move(reply.body);
        return true;
    }
    web::HttpReply reply = web::HttpFetch(nullptr, url, "GET", {}, {}, 30000, 8u << 20);
    if (reply.status != 200) return false;
    bytes = std::move(reply.body);
    return true;
}

WebFetchIO g_io;  // empty = production

WebFetchIO IO(const std::atomic<bool>* cancel) {
    WebFetchIO io = g_io;
    if (!io.fetch) io.fetch = [cancel](const std::string& url, const std::wstring& file,
                                       const std::function<void(int)>& progress) {
        // The production download follows the run's cancel flag; a null one only ever
        // happens in the self-test, which never asks to cancel.
        std::atomic<bool> live = false;
        return DownloadToFile(url, file, cancel ? *cancel : live, progress);
    };
    if (!io.fetchHls) io.fetchHls = [cancel](const WebTrack& track, const std::wstring& file,
                                             const std::function<void(int)>& progress) {
        std::atomic<bool> live = false;
        std::string error;
        return soundcloud::DownloadHlsTrack(track, cancel ? *cancel : live, file, progress, error);
    };
    if (!io.fetchBytes) io.fetchBytes = FetchBytes;
    return io;
}

}  // namespace

std::wstring WebFetchRecordPath(const std::wstring& dataDir) { return Join(dataDir, L"webfetch.json"); }

bool WebFetchForgetFailures(const std::wstring& dataDir) {
    Record record;
    LoadRecord(WebFetchRecordPath(dataDir), record);
    record.failed.clear();
    return SaveRecord(WebFetchRecordPath(dataDir), record);
}

std::wstring WebTabMarkPath(const std::wstring& dataDir) { return Join(dataDir, L"webfetch-tabs.json"); }

bool LoadWebTabMarks(const std::wstring& dataDir, std::vector<WebTabMark>& marks) {
    marks.clear();
    std::string text;
    if (!ReadFileBytes(WebTabMarkPath(dataDir), text) || text.empty()) return false;
    web::JsonValue json;
    if (!web::ParseJson(text, json)) return false;
    for (const web::JsonValue& row : json["tabs"].items) {
        WebTabMark mark;
        mark.title = FromUtf8(row["t"].text);
        mark.name = FromUtf8(row["n"].text);
        mark.seen = std::strtoull(row["s"].text.c_str(), nullptr, 16);
        if (!mark.title.empty() && !mark.name.empty()) marks.push_back(std::move(mark));
    }
    return true;
}

bool SaveWebTabMarks(const std::wstring& dataDir, const std::vector<WebTabMark>& marks) {
    std::string out = "{\"tabs\":[";
    bool first = true;
    for (const WebTabMark& mark : marks) {
        out += first ? "" : ",";
        first = false;
        out += "{\"t\":\"" + JsonEscape(ToUtf8(mark.title)) + "\",\"n\":\"" + JsonEscape(ToUtf8(mark.name)) +
               "\",\"s\":\"" + Hex64(mark.seen) + "\"}";
    }
    out += "]}";
    return WriteAtomic(WebTabMarkPath(dataDir), out);
}

uint64_t WebTabFingerprint(const std::vector<std::wstring>& files) { return FingerprintFiles(files); }

const WebTabMark* FindWebTabMark(const std::vector<WebTabMark>& marks, const std::wstring& title) {
    for (const WebTabMark& mark : marks)
        if (CompareStringOrdinal(mark.title.c_str(), -1, title.c_str(), -1, TRUE) == CSTR_EQUAL) return &mark;
    return nullptr;
}

void SetWebTabMark(std::vector<WebTabMark>& marks, const std::wstring& title, const std::wstring& name,
                   uint64_t seen) {
    for (WebTabMark& mark : marks) {
        if (CompareStringOrdinal(mark.title.c_str(), -1, title.c_str(), -1, TRUE) != CSTR_EQUAL) continue;
        mark.name = name;
        mark.seen = seen;
        return;
    }
    marks.push_back({title, name, seen});
}

void SetWebFetchIO(const WebFetchIO& io) { g_io = io; }

namespace {

// ---------------------------------------------------------------------------
// Link reading: one pasted link becomes the tracks it holds.

enum class Site { None, SoundCloud, YouTube, Spotify };

Site SiteOf(const std::string& link) {
    if (link.find("soundcloud.com") != std::string::npos) return Site::SoundCloud;
    if (link.find("youtu.be") != std::string::npos || link.find("youtube.com") != std::string::npos)
        return Site::YouTube;
    if (link.find("spotify.com") != std::string::npos) return Site::Spotify;
    return Site::None;
}

Site SiteByName(const std::string& site) {
    if (site == "soundcloud") return Site::SoundCloud;
    if (site == "youtube") return Site::YouTube;
    if (site == "spotify") return Site::Spotify;
    return Site::None;
}

// A list's tracks take the set's name as their album and a 1..n number; a single keeps
// neither, so it files as the artist's own plain song.
void NameList(std::vector<WebTrack>& tracks, const std::string& setTitle, bool asAlbum, bool numbered) {
    for (size_t i = 0; i < tracks.size(); ++i) {
        if (asAlbum && tracks[i].album.empty()) tracks[i].album = setTitle;
        if (numbered && tracks[i].trackNo == 0) tracks[i].trackNo = (unsigned)(i + 1);
        if (numbered) tracks[i].trackTotal = (unsigned)tracks.size();
    }
}

// Reads one pasted link into the tracks it holds. `isPlaylist` says the link names a list the
// UI can offer as one cordinal playlist (a Spotify playlist, a YouTube playlist, a SoundCloud
// set or likes), not an album or a single.
bool ReadLink(const WebFetchRun& run, const std::string& link, const std::atomic<bool>& cancel,
              Site& site, std::vector<WebTrack>& tracks, std::string& listTitle, bool& isPlaylist,
              std::string& error, const std::function<void(const std::string&)>& notice) {
    site = SiteOf(link);
    isPlaylist = false;
    if (run.resolvers.link) {
        // The self-test's fake stands in for every site at once: the tracks it returns
        // say their own site, and its listTitle plays the set title.
        bool ok = run.resolvers.link(link, cancel, tracks, listTitle, error, notice);
        if (ok) {
            site = SiteByName(tracks.empty() ? "" : tracks[0].site);
            isPlaylist = !listTitle.empty();
        }
        return ok;
    }
    switch (site) {
    case Site::SoundCloud: {
        const soundcloud::Kind kind = soundcloud::Classify(link);
        isPlaylist = kind == soundcloud::Kind::Playlist || kind == soundcloud::Kind::Likes;
        return soundcloud::Resolve(link, cancel, tracks, listTitle, error, notice);
    }
    case Site::YouTube: {
        if (yt::Classify(link) == yt::Kind::Playlist) {
            isPlaylist = true;
            std::string listId = yt::PlaylistIdOf(link);
            if (listId.empty()) {
                error = "that link's playlist id could not be read";
                return false;
            }
            return yt::ResolvePlaylist(listId, cancel, tracks, listTitle, error, notice);
        }
        // A link naming both a video and a list takes the whole list, the way soundcli's
        // add takes what a link holds.
        std::string id = yt::VideoIdOf(link);
        if (id.empty()) {
            error = "that link does not name a video";
            return false;
        }
        WebTrack one;
        if (!yt::ResolveVideo(id, cancel, one, error)) return false;
        tracks.push_back(one);
        return true;
    }
    case Site::Spotify: {
        spotify::Kind kind = spotify::Classify(link);
        if (kind == spotify::Kind::None) {
            error = "that Spotify page is not a track, album or playlist";
            return false;
        }
        isPlaylist = kind == spotify::Kind::Playlist;
        // The id is the last path run before any query noise.
        std::string id = link;
        size_t cut = id.find_first_of("?#");
        if (cut != std::string::npos) id = id.substr(0, cut);
        while (id.size() > 1 && (id.back() == '/')) id.pop_back();
        size_t slash = id.find_last_of('/');
        if (slash == std::string::npos) {
            error = "that Spotify link is missing its id";
            return false;
        }
        id = id.substr(slash + 1);
        if (notice) notice("Reading the Spotify page...");
        std::string html, embedError;
        if (kind == spotify::Kind::Album) {
            // The album page server-renders the complete, numbered list; the embed page
            // is the fallback when it cannot be read or holds no list.
            std::string pageHtml, pageError;
            if (spotify::ReadAlbumPage(id, cancel, pageHtml, pageError) &&
                spotify::ParseAlbumState(pageHtml, tracks, listTitle)) {
                NameList(tracks, listTitle, true, true);
                return true;
            }
        }
        if (!spotify::ReadEmbed(kind, id, cancel, html, embedError)) {
            error = embedError;
            return false;
        }
        if (kind == spotify::Kind::Track) {
            WebTrack one;
            if (!spotify::ParseTrackEmbed(html, one)) {
                error = "Spotify's page did not say what this track is";
                return false;
            }
            tracks.push_back(one);
            return true;
        }
        if (!spotify::ParseListEmbed(html, tracks, listTitle)) {
            error = "Spotify's page did not list what this " +
                    (kind == spotify::Kind::Album ? std::string("album") : std::string("playlist")) + " holds";
            return false;
        }
        NameList(tracks, listTitle, kind == spotify::Kind::Album, true);
        return true;
    }
    default:
        error = "that link is not from SoundCloud, YouTube or Spotify";
        return false;
    }
}

// A Spotify link names a recording, not a stream: the match runs one query at a time
// until YouTube offers the same recording. The tags stay Spotify's (title, artist,
// album, numbering, cover - the real item); only the audio bytes and their format come
// from the match, and the file is labelled as a match in its comment. A candidate the
// search row showed without a clock is only trusted after its own player reply agrees.
bool MatchSpotifyTrack(const WebFetchRun& run, const WebTrack& want, const std::atomic<bool>& cancel,
                       WebTrack& out, std::string& why) {
    if (run.resolvers.matchSpotify) return run.resolvers.matchSpotify(want, cancel, out, why);
    for (const std::string& query : spotify::BuildQueries(want)) {
        if (cancel) {
            why = "cancelled";
            return false;
        }
        std::vector<yt::Candidate> candidates;
        std::string searchError;
        if (!yt::Search(query, cancel, candidates, searchError)) {
            why = searchError;  // a transport failure stops the whole match
            return false;
        }
        std::string matchWhy;
        std::vector<int> order = spotify::RankMatches(want, candidates, matchWhy);
        for (int index : order) {
            if (cancel) {
                why = "cancelled";
                return false;
            }
            WebTrack got;
            std::string playerError;
            if (!yt::ResolveVideo(candidates[index].id, cancel, got, playerError)) {
                why = playerError;
                return false;
            }
            if (got.unavailable) continue;
            if (want.durationSec > 0 && got.durationSec > 0) {
                double off = std::abs((double)got.durationSec - (double)want.durationSec) / (double)want.durationSec;
                if (off > 0.02) continue;  // a different edit of the song, not the same recording
            }
            out = got;  // stream, format, duration and thumbnail from YouTube
            out.site = want.site;
            out.id = want.id;
            out.title = want.title;
            out.artist = want.artist;
            out.album = want.album;
            // Spotify's own cover is the real item's; an imported row that knows no cover
            // keeps the match's, so an imported file still lands with art of its own.
            if (!want.artworkUrl.empty()) out.artworkUrl = want.artworkUrl;
            out.trackNo = want.trackNo;
            out.trackTotal = want.trackTotal;
            out.discNo = want.discNo;
            out.discTotal = want.discTotal;
            out.unavailable = false;
            out.why.clear();
            return true;
        }
        // Nothing scored under this query: the next one is simpler, the way Puto retries.
    }
    why = "no YouTube upload of \"" + want.title + "\" is the same recording";
    return true;  // not a transport failure: the track is gone for good
}

}  // namespace

bool RunWebFetch(const WebFetchRun& run, std::vector<WebFetchItem>& items, std::string& error,
                 std::vector<WebFetchList>* lists) {
    items.clear();
    if (lists) lists->clear();
    if (run.links.empty() && run.importLists.empty()) {
        error = "no links to read";
        return false;
    }
    std::atomic<bool> liveFalse = false;
    const std::atomic<bool>& cancel = run.cancel ? *run.cancel : liveFalse;
    WebFetchIO io = IO(run.cancel);
    Record record;
    LoadRecord(WebFetchRecordPath(run.dataDir), record);
    auto notice = [&](const std::string& text) {
        if (run.notice) run.notice(text);
    };

    // Pass 1: read every link into the list. A link that cannot be read is a notice and
    // a remembered failure of the run, not the end of the run. An import's rows are already
    // resolved, so they come first and every one of them is a playlist of its own.
    bool anyRead = false;
    struct ListMeta { size_t first, count; const WebFetchTrackList* list; };
    std::vector<ListMeta> listMeta;
    for (const WebFetchTrackList& pre : run.importLists) {
        if (IsCancel(run.cancel)) break;
        anyRead = true;
        const size_t first = items.size();
        for (const WebTrack& track : pre.tracks) {
            WebFetchItem item;
            item.track = track;
            items.push_back(item);
            if (run.added) run.added(items.back());
        }
        if (!pre.albumArtist.empty() || !pre.date.empty() || !pre.releaseId.empty() || !pre.releaseGroupId.empty())
            listMeta.push_back({first, items.size() - first, &pre});
        if (lists) lists->push_back({pre.title, pre.playlist, first, items.size() - first});
    }
    for (const std::string& raw : run.links) {
        if (IsCancel(run.cancel)) break;
        std::string link = raw;
        while (!link.empty() && (link.front() == ' ' || link.front() == '\t' || link.front() == '\r' ||
                                 link.front() == '\n'))
            link.erase(link.begin());
        while (!link.empty() && (link.back() == ' ' || link.back() == '\r' || link.back() == '\n'))
            link.pop_back();
        if (link.empty()) continue;
        Site site = Site::None;
        std::vector<WebTrack> tracks;
        std::string listTitle, linkError;
        bool isPlaylist = false;
        if (!ReadLink(run, link, cancel, site, tracks, listTitle, isPlaylist, linkError, notice)) {
            notice(linkError);
            if (error.empty()) error = linkError;
            continue;
        }
        anyRead = true;
        if (site == Site::SoundCloud) {
            // A set is the album its tracks name; likes and an artist's page only
            // number, so liked songs never gain an album tag they never had; a single
            // link is neither. A fake resolver says all of it with its listTitle.
            const soundcloud::Kind kind = soundcloud::Classify(link);
            const bool faked = run.resolvers.link != nullptr;
            NameList(tracks, listTitle,
                     faked ? !listTitle.empty() : kind == soundcloud::Kind::Playlist,
                     faked ? !listTitle.empty() : kind != soundcloud::Kind::Track);
        }
        const size_t first = items.size();
        for (const WebTrack& track : tracks) {
            WebFetchItem item;
            item.track = track;
            items.push_back(item);
            if (run.added) run.added(items.back());
        }
        if (lists) lists->push_back({listTitle, isPlaylist, first, items.size() - first});
    }
    if (!anyRead && items.empty()) return false;

    // Pass 2: save each item, one at a time, in order.
    std::string lastCoverUrl, lastCoverBytes;
    for (size_t i = 0; i < items.size(); ++i) {
        if (IsCancel(run.cancel)) break;
        WebFetchItem& it = items[i];
        auto upd = [&]() {
            if (run.updated) run.updated(i, it);
        };
        const std::string key = RecordKey(it.track);

        if (it.track.unavailable || it.track.hlsOnly) {
            it.state = WebState::Failed;
            it.permanent = true;
            it.why = it.track.why.empty()
                         ? (it.track.hlsOnly ? "this one is only offered as an HLS stream, which saving does not take"
                                            : "the track is no longer available")
                         : it.track.why;
            record.failed[key] = {it.why, true};
            SaveRecord(WebFetchRecordPath(run.dataDir), record);
            upd();
            continue;
        }

        // What failed for good is left alone, the way soundcli leaves it alone, until
        // the failures are asked for again.
        auto failed = record.failed.find(key);
        if (failed != record.failed.end() && failed->second.second) {
            it.state = WebState::Failed;
            it.permanent = true;
            it.why = failed->second.first;
            upd();
            continue;
        }

        // Saved already: report it, but only while the file is still on the drive - a
        // deleted file is downloaded again, because owning means the bytes are there.
        auto saved = record.saved.find(key);
        if (saved != record.saved.end()) {
            std::wstring have = Join(run.libraryRoot, saved->second);
            if (Exists(have)) {
                it.state = WebState::Exists;
                it.file = have;
                upd();
                continue;
            }
            record.saved.erase(saved);  // the file went away; fetch it again
        }

        // A Spotify, MusicBrainz or online-search track resolves to a stream right before its
        // download, so the list appears at once and the searches stay paced.
        if ((it.track.site == "spotify" || it.track.site == "musicbrainz" || it.track.site == "deezer" ||
             it.track.site == "itunes") &&
            it.track.streamUrl.empty()) {
            notice("Matching \"" + it.track.title + "\" on YouTube...");
            WebTrack matched;
            std::string matchWhy;
            if (IsCancel(run.cancel)) break;
            bool matchOk = MatchSpotifyTrack(run, it.track, cancel, matched, matchWhy);
            if (!matchOk) {
                if (matchWhy == "cancelled" || IsCancel(run.cancel)) break;
                it.state = WebState::Failed;
                it.why = matchWhy;  // a search that did not answer: try again on the next run
                upd();
                continue;
            }
            if (matched.streamUrl.empty()) {
                // The match itself decided there is no same-recording upload: gone for
                // good, remembered like a removed track is.
                it.state = WebState::Failed;
                it.permanent = true;
                it.why = matchWhy;
                record.failed[key] = {it.why, true};
                SaveRecord(WebFetchRecordPath(run.dataDir), record);
                upd();
                continue;
            }
            it.track.streamUrl = matched.streamUrl;
            it.track.ext = matched.ext;
            if (it.track.durationSec == 0) it.track.durationSec = matched.durationSec;
        }

        const WebFetchTrackList* meta = nullptr;
        for (const ListMeta& lm : listMeta)
            if (i >= lm.first && i < lm.first + lm.count) { meta = lm.list; break; }
        std::wstring target = TargetPath(it.track, run.libraryRoot, meta ? meta->albumArtist : std::string_view{});
        EnsureFolder(FolderOf(target));
        // Reaching here means the record does not map this track to that name, so a
        // file already sitting there is a different recording: it gets numbered, not
        // replaced, the way organize numbers collisions.
        if (Exists(target)) target = FirstFreePath(target);

        // The stream lands next to its destination with its real extension, so the tag
        // writer can see what it is writing, and is renamed only once it is whole.
        const std::wstring ext = ExtOfTrack(it.track);
        std::wstring part = ext.empty() ? target + L".part"
                                        : target.substr(0, target.size() - ext.size() - 1) + L".part." + ext;
        it.state = WebState::Downloading;
        it.percent = 0;
        upd();
        auto reportProgress = [&](int percent) {
            it.percent = percent;
            upd();
        };
        auto save = [&]() {
            // An HLS stream joins its init and segments through the site module; a plain
            // one is a single streamed file.
            return it.track.hls ? (io.fetchHls && io.fetchHls(it.track, part, reportProgress))
                                : io.fetch(it.track.streamUrl, part, reportProgress);
        };
        bool ok = save();
        // Stream addresses expire mid-list (SoundCloud's are signed for minutes):
        // one fresh resolution per site, one more try.
        if (!ok && !IsCancel(run.cancel) && !it.track.id.empty()) {
            DeleteFileW(part.c_str());
            std::string freshError;
            bool refreshed = false;
            if (it.track.site == "youtube") {
                WebTrack fresh;
                refreshed = yt::ResolveVideo(it.track.id, cancel, fresh, freshError);
                if (refreshed) {
                    it.track.streamUrl = fresh.streamUrl;
                    it.track.ext = fresh.ext;
                    it.track.hls = fresh.hls;
                }
            } else if (it.track.site == "soundcloud") {
                // SoundCloud's refresh keeps the list's identity fields in place.
                refreshed = soundcloud::RefreshTrack(it.track, cancel, freshError);
            }
            if (refreshed) ok = save();
        }
        if (!ok && !IsCancel(run.cancel)) {
            DeleteFileW(part.c_str());
            ok = save();
        }
        if (!ok) {
            it.state = WebState::Failed;
            it.percent = -1;
            it.why = IsCancel(run.cancel) ? "cancelled" : "the download did not finish; it will be tried again";
            upd();
            DeleteFileW(part.c_str());
            continue;
        }

        it.state = WebState::Saving;
        upd();

        // YouTube's and SoundCloud's newer streams arrive as fragmented MP4, which no tag
        // writer takes: rewrite it into one standard fast-start M4A first — the same
        // compressed frames, byte for byte — so the song lands as a normal tagged library
        // file. A stream the rewrite cannot represent is saved exactly as it arrived.
        bool fragmentedKept = false;
        if (ext == L"m4a" && nativeaudio::IsFragmentedMp4(part)) {
            const std::wstring remux = target.substr(0, target.size() - ext.size() - 1) + L".remux." + ext;
            std::wstring remuxError;
            if (nativeaudio::RemuxFragmentedMp4(part, remux, remuxError)) {
                DeleteFileW(part.c_str());
                part = remux;
            } else {
                DeleteFileW(remux.c_str());
                fragmentedKept = true;
                notice("saved as downloaded (" + ToUtf8(remuxError) + ")");
            }
        }

        std::string coverBytes;
        if (!it.track.artworkUrl.empty()) {
            if (it.track.artworkUrl == lastCoverUrl) {
                coverBytes = lastCoverBytes;
            } else {
                if (!io.fetchBytes(it.track.artworkUrl, coverBytes)) coverBytes.clear();
                lastCoverUrl = it.track.artworkUrl;
                lastCoverBytes = coverBytes;
            }
        }
        bool tagged = false;
        const bool canTag = ext == L"mp3" || (ext == L"m4a" && !fragmentedKept);
        if (canTag) {
            TagEdit edit;
            edit.title = it.track.title;
            edit.artist = it.track.artist;
            if (!it.track.album.empty()) edit.album = it.track.album;
            if (meta && !meta->albumArtist.empty()) edit.albumArtist = meta->albumArtist;
            if (meta && !meta->date.empty()) edit.date = meta->date;
            if (it.track.trackNo > 0) edit.track = it.track.trackNo;
            if (it.track.trackTotal > 0) edit.trackTotal = it.track.trackTotal;
            if (it.track.discNo > 0) edit.disc = it.track.discNo;
            if (it.track.discTotal > 0) edit.discTotal = it.track.discTotal;
            if (meta && !meta->releaseId.empty()) edit.musicBrainzRelease = meta->releaseId;
            if (meta && !meta->releaseGroupId.empty()) edit.musicBrainzReleaseGroup = meta->releaseGroupId;
            if (it.track.site == "spotify")
                edit.comment = "audio matched on YouTube; tags from Spotify";  // labelled, never a secret
            else if (it.track.site == "musicbrainz")
                edit.comment = "audio matched on YouTube; tags from MusicBrainz";
            else if (it.track.site == "deezer")
                edit.comment = "audio matched on YouTube; tags from Deezer";
            else if (it.track.site == "itunes")
                edit.comment = "audio matched on YouTube; tags from iTunes";
            if (!coverBytes.empty()) edit.cover = coverBytes;
            std::wstring tagError;
            tagged = WriteTags(part, edit, tagError);
            if (!tagged) notice("saved without embedded tags (" + ToUtf8(tagError) + ")");
        } else if (!fragmentedKept) {
            notice("saved without embedded tags (a " + ToUtf8(ext) + " stream carries no tags cordinal can write)");
        }
        // A stream that cannot carry tags still gets its art: the album folder's own
        // picture is what the library shows beside those files (art.cpp's names). An
        // existing picture is never replaced, and the write is moved into place.
        if (!tagged && !coverBytes.empty()) {
            const std::wstring folder = FolderOf(target);
            bool hasCover = false;
            for (const wchar_t* name :
                 {L"cover.jpg", L"folder.jpg", L"front.jpg", L"cover.png", L"folder.png", L"front.png"})
                if (Exists(Join(folder, name))) {
                    hasCover = true;
                    break;
                }
            if (!hasCover) {
                const std::wstring cover = Join(folder, L"cover.jpg");
                const std::wstring tmp = cover + L".cordinal-tmp";
                HANDLE h =
                    CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                bool coverOk = h != INVALID_HANDLE_VALUE;
                if (coverOk) {
                    DWORD wrote = 0;
                    coverOk = WriteFile(h, coverBytes.data(), (DWORD)coverBytes.size(), &wrote, nullptr) &&
                              wrote == coverBytes.size();
                    CloseHandle(h);
                }
                if (!coverOk || !MoveFileExW(tmp.c_str(), cover.c_str(), MOVEFILE_WRITE_THROUGH)) DeleteFileW(tmp.c_str());
            }
        }

        if (!MoveFileExW(part.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
            it.state = WebState::Failed;
            it.percent = -1;
            it.why = "the file could not be moved into place";
            upd();
            DeleteFileW(part.c_str());
            continue;
        }
        it.state = WebState::Done;
        it.percent = -1;
        it.file = target;
        std::wstring rel = target.size() > run.libraryRoot.size() &&
                                    _wcsicmp(target.substr(0, run.libraryRoot.size()).c_str(), run.libraryRoot.c_str()) == 0
                                ? target.substr(run.libraryRoot.size() +
                                                (target[run.libraryRoot.size()] == kPathSep ? 1 : 0))
                                : target;
        record.saved[key] = rel;
        SaveRecord(WebFetchRecordPath(run.dataDir), record);
        upd();

        // A steady pace between requests to the same site.
        Pace(it.track.site == "soundcloud" ? 250 : 600, cancel);
    }
    return true;
}

bool WebFetchKeep(const WebTrack& track, const std::wstring& previewFile, const std::wstring& libraryRoot,
                  const std::wstring& dataDir, std::wstring& keptFile, std::string& error) {
    keptFile.clear();
    error.clear();
    if (!Exists(previewFile)) {
        error = "the preview is gone";
        return false;
    }
    if (libraryRoot.empty()) {
        error = "no library folder is set";
        return false;
    }
    WebTrack shaped = track;
    if (shaped.ext.empty()) shaped.ext = ToUtf8(ExtOfTrack(shaped));
    std::wstring target = TargetPath(shaped, libraryRoot);
    EnsureFolder(FolderOf(target));
    if (Exists(target)) target = FirstFreePath(target);
    // A temporary beside the destination, so a failed copy leaves no half file in the library.
    const std::wstring part = target + L".cordinal-tmp";
    if (!CopyFileW(previewFile.c_str(), part.c_str(), FALSE) ||
        !MoveFileExW(part.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(part.c_str());
        error = "the file could not be copied into the library";
        return false;
    }
    Record record;
    LoadRecord(WebFetchRecordPath(dataDir), record);
    record.failed.erase(RecordKey(shaped));
    std::wstring rel = target.size() > libraryRoot.size() &&
                               _wcsicmp(target.substr(0, libraryRoot.size()).c_str(), libraryRoot.c_str()) == 0
                           ? target.substr(libraryRoot.size() +
                                           (target[libraryRoot.size()] == kPathSep ? 1 : 0))
                           : target;
    record.saved[RecordKey(shaped)] = rel;
    SaveRecord(WebFetchRecordPath(dataDir), record);
    keptFile = std::move(target);
    return true;
}
