// Checks of the online metadata core (metadata_fetch.h, coverfetch.h) that need neither the network nor
// a window: every provider's reply parser on canned JSON, the cover scoring, the image checks, the atomic
// folder write, the batch review writing only what was checked, stale-result rejection and embedding a
// cover with undo. The Windows self-test (selftest_metadata.cpp, suite "metadata") and the Linux core
// test (core_test.cpp, "metadata") both run them, so the portable core is held to one standard.
#pragma once
#include "backup.h"
#include "coverfetch.h"
#include "coverupload.h"
#include "customcover.h"
#include "metadata_fetch.h"
#include "tags.h"
#include "tagundo.h"
#include "tagwriter.h"

#include <windows.h>

#include <cmath>
#include <cwctype>
#include <string>
#include <vector>

namespace metadata_checks {

struct Group {
    std::string name;
    int cases = 0;
    std::vector<std::string> failures;
    void Add(bool ok, const std::string& what) {
        ++cases;
        if (!ok) failures.push_back(what);
    }
    bool Passed() const { return cases > 0 && failures.empty(); }
};

inline std::string Hex(const char* hex) {
    std::string out;
    for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) out += (char)std::stoi(std::string(hex + i, 2), nullptr, 16);
    return out;
}

// A 16 x 12 JPEG and an 8 x 8 PNG, made with FFmpeg's color source.
inline std::string Jpeg() {
    return Hex("ffd8ffe000104a46494600010200000100010000fffe000f4c61766336332e312e31303100ffdb004300081010131013"
               "1616161616161a181a1b1b1b1a1a1a1a1b1b1b1d1d1d2222221d1d1d1b1b1d1d20202222252625232322232626282828"
               "30302e2e38383a454553ffc4004b00010100000000000000000000000000000005010100000000000000000000000000"
               "000006100100000000000000000000000000000000110100000000000000000000000000000000ffc0001108000c0010"
               "03012200021100031100ffda000c03010002110311003f008201d8e3ffd9");
}
inline std::string Png() {
    return Hex("89504e470d0a1a0a0000000d49484452000000080000000808020000004b6d29dc000000097048597300000001000000"
               "01004f25c4d60000001449444154789c633c9562c8800db060151db412001ca2017d3e4d71720000000049454e44ae42"
               "6082");
}
// The same picture with one byte of its comment changed: another valid JPEG.
inline std::string OtherJpeg() {
    std::string bytes = Jpeg();
    bytes[24] = 'X';
    return bytes;
}

inline bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }
inline std::string Read(const std::wstring& path) {
    std::string bytes;
    return backup::ReadBytes(path, bytes) ? bytes : std::string();
}

inline Group Parsers() {
    Group g{"provider replies: results, empty, malformed and missing artwork"};
    using online::CoverSource;
    auto deezer = online::ParseDeezerAlbums(
        R"({"data":[{"id":1,"title":"Blue Train","cover_xl":"https://cdn.example/1000x1000.jpg",)"
        R"("cover_big":"https://cdn.example/500x500.jpg","artist":{"name":"John Coltrane"}},)"
        R"({"id":2,"title":"No Art","artist":{"name":"X"}},)"
        R"({"id":3,"title":"Big Only","cover_big":"https://cdn.example/b500.jpg","artist":{"name":"Y"}},)"
        R"({"id":4,"title":"Plain","cover_xl":"http://insecure.example/1.jpg","artist":{"name":"Z"}}],"total":4})");
    g.Add(deezer.size() == 2 && deezer[0].source == CoverSource::Deezer && deezer[0].artist == "John Coltrane" &&
              deezer[0].album == "Blue Train" && deezer[0].width == 1000 && deezer[1].width == 500 &&
              deezer[1].url == "https://cdn.example/b500.jpg",
          "Deezer keeps the two albums with HTTPS artwork, the largest size first");
    g.Add(online::ParseDeezerAlbums(R"({"data":[],"total":0})").empty(), "Deezer empty result");
    g.Add(online::ParseDeezerAlbums(R"({"data":[{"title":)").empty(), "Deezer malformed JSON");
    g.Add(online::ParseDeezerAlbums(R"({"error":{"type":"Exception","message":"Quota limit exceeded","code":4}})").empty(),
          "Deezer error object");
    auto itunes = online::ParseItunesAlbums(
        R"({"resultCount":2,"results":[{"collectionName":"Blue Train","artistName":"John Coltrane",)"
        R"("artworkUrl100":"https://is1.example/image/thumb/source/100x100bb.jpg"},)"
        R"({"collectionName":"No Art","artistName":"X"}]})");
    g.Add(itunes.size() == 1 && itunes[0].url == "https://is1.example/image/thumb/source/1200x1200bb.jpg" &&
              itunes[0].source == CoverSource::ITunes && itunes[0].width == 1200,
          "iTunes asks for 1200-pixel artwork and skips results without any");
    g.Add(online::ParseItunesAlbums(R"({"resultCount":0,"results":[]})").empty(), "iTunes empty result");
    g.Add(online::ParseItunesAlbums("<html>Service unavailable</html>").empty(), "iTunes non-JSON reply");
    auto mb = online::ParseMusicBrainzReleases(
        R"({"releases":[{"id":"0a1b2c3d-1111-2222-3333-444455556666","score":100,"title":"Blue Train",)"
        R"("artist-credit":[{"name":"John Coltrane"}]},)"
        R"({"id":"0a1b2c3d-1111-2222-3333-444455556667","score":60,"title":"Low score"},)"
        R"({"id":"../../etc/passwd","score":100,"title":"Bad id"},)"
        R"({"id":"0a1b2c3d-1111-2222-3333-444455556668","score":95,"title":"No front","cover-art-archive":{"front":false}}]})");
    g.Add(mb.size() == 1 && mb[0].artist == "John Coltrane" &&
              mb[0].url == "https://coverartarchive.org/release/0a1b2c3d-1111-2222-3333-444455556666/front-1200",
          "MusicBrainz keeps confident releases with a usable id and a front image");
    g.Add(online::ParseMusicBrainzReleases(R"({"releases":[]})").empty() &&
              online::ParseMusicBrainzReleases(R"({"releases":[{"id":)").empty(),
          "MusicBrainz empty and malformed replies");
    return g;
}

inline Group Scoring() {
    Group g{"cover scoring: matches, compilations, generic titles and image size"};
    using online::CoverCandidate;
    using online::CoverSource;
    online::CoverQuery q{"John Coltrane", "Blue Train"};
    g.Add(online::MatchScore(q, "john coltrane", "BLUE TRAIN") == 1.0, "artist and album match ignoring case: +1");
    g.Add(online::MatchScore(q, "John Coltrane", "Giant Steps") == 0.5, "only the artist matches: +0.5");
    g.Add(online::MatchScore(q, "Miles Davis", "Kind of Blue") == -1.5, "both differ: -1.5");
    online::CoverQuery loose{"John Coltrane", ""};
    g.Add(online::MatchScore(loose, "Various Artists", "Jazz Compilation") == -5, "no album: a compilation by others, -5");
    g.Add(online::MatchScore(loose, "John Coltrane", "Greatest Hits") == -3, "no album: a generic title, -3");
    g.Add(online::MatchScore(loose, "John Coltrane", "Original Motion Picture Soundtrack") == -0.5,
          "no album: a soundtrack, -0.5");
    g.Add(online::MatchScore(loose, "John Coltrane", "Blue Train") == 0, "no album: a plain album by the artist, 0");
    g.Add(std::fabs(online::ImageScore(1000, 1000) - 3) < 1e-9 && std::fabs(online::ImageScore(500, 500) - 2) < 1e-9 &&
              online::ImageScore(1000, 500) < online::ImageScore(800, 800) && online::ImageScore(0, 0) == online::ImageScore(500, 500),
          "bigger and squarer images score higher; an unknown size counts as 500 pixels");
    std::vector<CoverCandidate> c(3);
    c[0] = {CoverSource::ITunes, "Miles Davis", "Kind of Blue", "https://a", 1200, 1200, 0};
    c[1] = {CoverSource::MusicBrainz, "John Coltrane", "Blue Train", "https://b", 0, 0, 0};
    c[2] = {CoverSource::Deezer, "John Coltrane", "Blue Train", "https://c", 1000, 1000, 0};
    online::RankCovers(q, c, {CoverSource::Deezer, CoverSource::ITunes, CoverSource::MusicBrainz});
    g.Add(c[0].source == CoverSource::Deezer && c[1].source == CoverSource::MusicBrainz && c[2].source == CoverSource::ITunes &&
              c[0].score > c[1].score && c[1].score > c[2].score,
          "ranking puts the right album first and a larger wrong one last");
    CoverCandidate good{CoverSource::Deezer, "John Coltrane", "Blue Train", "https://c", 1000, 1000, 0};
    CoverCandidate tiny = good, wide = good, wrong = good;
    tiny.width = tiny.height = 300;
    wide.height = 500;
    wrong.album = "Giant Steps";
    g.Add(online::ConfidentCover(q, good) && !online::ConfidentCover(q, tiny) && !online::ConfidentCover(q, wide) &&
              !online::ConfidentCover(q, wrong) && !online::ConfidentCover(loose, good),
          "only an exact, large, square match starts checked");
    return g;
}

inline Group Images() {
    Group g{"downloaded images: complete JPEG and PNG only, capped in size"};
    int w = 0, h = 0;
    g.Add(online::InspectImage(Jpeg(), w, h) == online::ImageType::Jpeg && w == 16 && h == 12, "a JPEG and its size");
    g.Add(online::InspectImage(Png(), w, h) == online::ImageType::Png && w == 8 && h == 8, "a PNG and its size");
    std::string jpeg = Jpeg(), png = Png();
    g.Add(online::InspectImage(jpeg.substr(0, jpeg.size() / 2), w, h) == online::ImageType::None && !w && !h,
          "a truncated JPEG");
    g.Add(online::InspectImage(png.substr(0, png.size() - 12), w, h) == online::ImageType::None, "a PNG cut before IEND");
    g.Add(online::InspectImage("<html><body>Not found. This page is not an image at all.</body></html>", w, h) ==
              online::ImageType::None,
          "an HTML error page");
    std::string huge(online::kMaxImageBytes + 1, '\0');
    huge.replace(0, jpeg.size() - 2, jpeg.substr(0, jpeg.size() - 2));
    huge[huge.size() - 2] = '\xFF';
    huge[huge.size() - 1] = '\xD9';
    g.Add(online::InspectImage(huge, w, h) == online::ImageType::None, "an image over the size cap");
    return g;
}

inline Group FolderWrites(const std::wstring& scratch) {
    Group g{"folder covers: atomic write, never over an existing cover unless asked"};
    const wchar_t sep = kPathSep;
    std::wstring dir = scratch + sep + L"covers-write", other = scratch + sep + L"covers-folder",
                 bad = scratch + sep + L"covers-bad";
    for (const auto& d : {dir, other, bad}) CreateDirectoryW(d.c_str(), nullptr);
    std::wstring cover = dir + sep + L"cover.jpg", error;
    DeleteFileW(cover.c_str());
    DeleteFileW((dir + sep + L"cover.png").c_str());
    g.Add(online::WriteFolderCover(dir, Jpeg(), false, error) == online::FolderWrite::Written && Read(cover) == Jpeg(),
          "a new cover.jpg holds exactly the image");
    std::wstring temp = dir + sep + L".cordinal-cover-" + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    g.Add(!Exists(temp), "no temporary file is left behind");
    g.Add(online::WriteFolderCover(dir, OtherJpeg(), false, error) == online::FolderWrite::Exists && Read(cover) == Jpeg(),
          "an existing cover stays byte-identical when replacing is off");
    g.Add(online::WriteFolderCover(dir, Png(), true, error) == online::FolderWrite::Failed && Read(cover) == Jpeg() &&
              !Exists(dir + sep + L"cover.png"),
          "a PNG is refused where an old JPEG would still be shown first");
    g.Add(online::WriteFolderCover(dir, OtherJpeg(), true, error) == online::FolderWrite::Written && Read(cover) == OtherJpeg(),
          "replacing, when asked, swaps in the new image whole");
    std::wstring folderJpg = other + sep + L"folder.jpg";
    backup::WriteBytes(folderJpg, "existing");
    DeleteFileW((other + sep + L"cover.jpg").c_str());
    g.Add(online::WriteFolderCover(other, Jpeg(), false, error) == online::FolderWrite::Exists &&
              !Exists(other + sep + L"cover.jpg") && Read(folderJpg) == "existing",
          "folder.jpg counts as a cover the album already has");
    DeleteFileW((bad + sep + L"cover.jpg").c_str());
    g.Add(online::WriteFolderCover(bad, "not an image, only some text pretending", false, error) == online::FolderWrite::Failed &&
              !Exists(bad + sep + L"cover.jpg") && !error.empty(),
          "something that isn't an image is never written");
    return g;
}

inline Group Review(const std::wstring& scratch) {
    Group g{"batch review: only the checked covers are written"};
    std::vector<coverfetch::Proposal> proposals(3);
    for (int i = 0; i < 3; ++i) {
        std::wstring dir = scratch + kPathSep + L"covers-review" + std::to_wstring(i);
        CreateDirectoryW(dir.c_str(), nullptr);
        DeleteFileW((dir + kPathSep + L"cover.jpg").c_str());
        proposals[(size_t)i].album.dir = dir;
        proposals[(size_t)i].album.tracks = {(TrackId)(10 + i)};
        proposals[(size_t)i].album.album = "Album " + std::to_string(i);
        proposals[(size_t)i].cover.image = Jpeg();
    }
    auto choices = coverfetch::CheckedChoices(proposals, {true, false, true});
    auto result = coverfetch::SaveToFolders(choices, false);
    g.Add(choices.size() == 2 && result.written == 2 && result.failed == 0, "two of three checked, two written");
    g.Add(Exists(proposals[0].album.dir + kPathSep + L"cover.jpg") && !Exists(proposals[1].album.dir + kPathSep + L"cover.jpg") &&
              Exists(proposals[2].album.dir + kPathSep + L"cover.jpg"),
          "the unchecked album's folder is untouched");
    g.Add(result.changed == std::vector<TrackId>{10, 12}, "the changed tracks are those of the written albums");
    g.Add(coverfetch::CheckedChoices(proposals, {false, false, false}).empty() &&
              coverfetch::CheckedChoices(proposals, {true}).size() == 1,
          "nothing checked writes nothing, and a short list never reaches past its end");
    return g;
}

inline Group Generations() {
    Group g{"stale results: a newer search drops what an older one sends"};
    online::Generation generation;
    g.Add(!generation.Accepts(0), "an untagged result is never applied");
    uint64_t first = generation.Next();
    g.Add(generation.Accepts(first), "the current search's result is applied");
    uint64_t second = generation.Next();
    g.Add(!generation.Accepts(first) && generation.Accepts(second), "after a new search the old result is dropped");
    return g;
}

// Embeds a cover into copies of `media` (formats the tag writer supports) as one journal, then
// undoes it. The copies must already exist; `journal` is an empty folder for the undo record.
inline Group EmbedUndo(const std::vector<std::wstring>& copies, const std::wstring& journalDir) {
    Group g{"embedded covers: one undo record restores every file"};
    std::vector<std::string> before;
    for (const auto& path : copies) before.push_back(Read(path));
    TagEdit edit;
    edit.cover = Jpeg();
    TagJournal journal;
    bool captured = true;
    for (const auto& path : copies) {
        journal.files.emplace_back();
        captured = captured && CaptureTagUndo(path, edit, journal.files.back());
    }
    std::wstring error, file;
    bool saved = captured && SaveTagJournal(journalDir, journal, error);
    bool written = saved;
    for (const auto& path : copies) written = written && WriteTags(path, edit, error);
    bool embedded = written;
    for (const auto& path : copies) {
        std::string art;
        embedded = embedded && ReadEmbeddedArt(path, art) && art == Jpeg();
    }
    g.Add(embedded, "the cover is embedded in every file");
    uint32_t count = 0;
    TagJournal back;
    bool loaded = FindNewestTagJournal(journalDir, file, count) && count == copies.size() && LoadTagJournal(file, back) &&
                  back.files.size() == copies.size();
    g.Add(loaded, "a single undo record covers every file");
    bool restored = loaded;
    for (const auto& undo : back.files) restored = restored && WriteTags(undo.path, undo.old, error);
    for (size_t i = 0; i < copies.size(); ++i) {
        std::string art;
        std::string name = ToUtf8(FileNameOf(copies[i]));
        g.Add(restored && !(ReadEmbeddedArt(copies[i], art) && !art.empty()), "undo removes the cover from " + name);
        // An MP3's ID3v2 block may keep the padding the write added; everything after it, and every
        // byte of the other formats, must come back exactly.
        auto body = [](const std::string& bytes) {
            if (bytes.size() < 10 || bytes.compare(0, 3, "ID3")) return bytes;
            size_t size = 10;
            for (int k = 6; k < 10; ++k) size += (size_t)((unsigned char)bytes[(size_t)k] & 0x7F) << (7 * (9 - k));
            return size <= bytes.size() ? bytes.substr(size) : std::string();
        };
        std::string after = Read(copies[i]);
        g.Add(restored && !after.empty() && body(after) == body(before[i]),
              "undo leaves " + name + " byte-identical outside its ID3 padding (" + std::to_string(before[i].size()) +
                  " bytes before, " + std::to_string(after.size()) + " after)");
    }
    if (loaded) DeleteTagJournal(file);
    return g;
}

inline Group Lyrics() {
    Group g{"lyrics replies and scoring: synced first, another song never"};
    using online::LyricsSource;
    const std::string synced = "[00:01.00] First line of the song\n[00:04.50] Second line of the song\n"
                               "[00:08.00] Third line of the song";
    auto lrclib = online::ParseLrcLib(
        R"({"id":1,"trackName":"Blue Train","artistName":"John Coltrane","albumName":"Blue Train","duration":643,)"
        R"("instrumental":false,"plainLyrics":"First line of the song\nSecond line of the song\nThird line of the song",)"
        R"("syncedLyrics":"[00:01.00] First line of the song\n[00:04.50] Second line of the song\n[00:08.00] Third line of the song"})");
    g.Add(lrclib.size() == 1 && lrclib[0].synced && lrclib[0].text == synced && lrclib[0].title == "Blue Train" &&
              lrclib[0].source == LyricsSource::LrcLib,
          "LrcLib prefers the synced lyrics");
    auto plain = online::ParseLrcLib(
        R"([{"trackName":"Song","artistName":"A","plainLyrics":"Only plain words here","syncedLyrics":null},)"
        R"({"trackName":"Interlude","artistName":"A","instrumental":true,"plainLyrics":null,"syncedLyrics":null},)"
        R"({"trackName":"Empty","artistName":"A","plainLyrics":"   ","syncedLyrics":""}])");
    g.Add(plain.size() == 1 && !plain[0].synced && plain[0].text == "Only plain words here",
          "LrcLib falls back to plain lyrics and leaves out instrumental and empty entries");
    g.Add(online::ParseLrcLib(R"({"code":404,"name":"TrackNotFound","message":"Failed to find specified track"})").empty() &&
              online::ParseLrcLib(R"({"trackName":)").empty(),
          "LrcLib not-found and malformed replies");
    online::LyricsQuery q{"John Coltrane", "Blue Train", "Blue Train", 643};
    auto ovh = online::ParseLyricsOvh(R"({"lyrics":"Line one\r\nLine two\r\nLine three"})", q);
    g.Add(ovh.size() == 1 && ovh[0].text == "Line one\nLine two\nLine three" && ovh[0].artist == "John Coltrane" &&
              ovh[0].source == LyricsSource::LyricsOvh,
          "lyrics.ovh credits the asked-for song and drops carriage returns");
    g.Add(online::ParseLyricsOvh(R"({"error":"No lyrics found"})", q).empty() && online::ParseLyricsOvh("not json", q).empty(),
          "lyrics.ovh error and malformed replies");
    online::LyricsFound f{LyricsSource::LrcLib, "john coltrane", "BLUE TRAIN", "Blue Train", synced, true, 0};
    g.Add(online::LyricsScore(q, f) == 2.5, "artist, album and title match and long text: 2.5");
    f.text = "short";
    g.Add(online::LyricsScore(q, f) == 1.5, "short text loses the length point");
    online::LyricsFound other{LyricsSource::LrcLib, "Miles Davis", "Kind of Blue", "So What", synced, true, 0};
    g.Add(online::LyricsScore(q, other) == -0.5, "artist and title both differ: -1.5");
    online::LyricsFound best;
    g.Add(!online::BestLyrics(q, {other}, best), "another song's lyrics are never used");
    online::LyricsFound sameArtist{LyricsSource::LrcLib, "John Coltrane", "Blue Train", "Moment's Notice", synced, true, 0};
    g.Add(!online::BestLyrics(q, {sameArtist}, best), "the artist's other song is not used either");
    online::LyricsFound plainOvh = ovh.empty() ? online::LyricsFound{} : ovh[0];
    plainOvh.text = "A plain verse that is long enough to count as real lyrics for this song.";
    online::LyricsFound timed{LyricsSource::LrcLib, "John Coltrane", "", "Blue Train", synced, true, 0};
    g.Add(online::BestLyrics(q, {plainOvh, timed}, best) && best.synced && best.source == LyricsSource::LrcLib,
          "synced lyrics win over plain ones");
    return g;
}

inline Group LrcWrite(const std::wstring& scratch) {
    Group g{".lrc files: atomic, never over an existing one"};
    std::wstring dir = scratch + kPathSep + L"lyrics-write";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring audio = dir + kPathSep + L"01 Song.flac", lrc = dir + kPathSep + L"01 Song.lrc", error;
    DeleteFileW(lrc.c_str());
    g.Add(online::WriteLrcFile(audio, "[00:01.00] words", error) == online::FolderWrite::Written &&
              Read(lrc) == "[00:01.00] words",
          "a new .lrc holds exactly the lyrics");
    g.Add(online::WriteLrcFile(audio, "other words", error) == online::FolderWrite::Exists && Read(lrc) == "[00:01.00] words",
          "an existing .lrc stays byte-identical");
    g.Add(!Exists(lrc + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp"), "no temporary file is left behind");
    g.Add(online::WriteLrcFile(dir + kPathSep + L"noextension", "words", error) == online::FolderWrite::Failed,
          "a path without a file extension is refused");
    return g;
}

inline Group Artist() {
    Group g{"artist info: one unambiguous artist, links and the article"};
    const char* one = R"j({"artists":[{"id":"5b11f4ce-a62d-471e-81fc-a69a8278c7da","name":"Nirvana","score":100},)j"
                      R"j({"id":"aaaaaaaa-1111-2222-3333-444444444444","name":"Nirvana Tribute","score":95},)j"
                      R"j({"id":"bbbbbbbb-1111-2222-3333-444444444444","name":"NIRVANA","score":60}]})j";
    g.Add(online::ParseArtistSearch(one, "nirvana") == "5b11f4ce-a62d-471e-81fc-a69a8278c7da",
          "the one confident artist with exactly this name is chosen");
    const char* two = R"j({"artists":[{"id":"5b11f4ce-a62d-471e-81fc-a69a8278c7da","name":"Nirvana","score":100},)j"
                      R"j({"id":"cccccccc-1111-2222-3333-444444444444","name":"Nirvana","score":100}]})j";
    g.Add(online::ParseArtistSearch(two, "Nirvana").empty(), "two artists of that name are ambiguous: nothing is chosen");
    g.Add(online::ParseArtistSearch(R"j({"artists":[{"id":"5b11f4ce-a62d-471e-81fc-a69a8278c7da","name":"Nirvana UK","score":100}]})j",
                                    "Nirvana").empty(),
          "a different name is never chosen");
    g.Add(online::ParseArtistSearch(R"j({"artists":[{"id":"../x","name":"Nirvana","score":100}]})j", "Nirvana").empty() &&
              online::ParseArtistSearch(R"j({"artists":[)j", "Nirvana").empty() &&
              online::ParseArtistSearch(R"j({"artists":[]})j", "Nirvana").empty(),
          "bad ids, malformed and empty replies");
    online::ArtistInfo info;
    bool parsed = online::ParseArtistLookup(
        R"j({"id":"5b11f4ce-a62d-471e-81fc-a69a8278c7da","name":"Nirvana","relations":[)j"
        R"j({"type":"wikidata","url":{"resource":"https://www.wikidata.org/wiki/Q11649"}},)j"
        R"j({"type":"official homepage","url":{"resource":"https://www.nirvana.com/"}},)j"
        R"j({"type":"wikipedia","url":{"resource":"https://de.wikipedia.org/wiki/Nirvana_(US-amerikanische_Band)"}},)j"
        R"j({"type":"social network","url":{"resource":"javascript:alert(1)"}}]})j", info);
    g.Add(parsed && info.name == "Nirvana" && info.wikidata == "Q11649" && info.official == "https://www.nirvana.com/" &&
              info.wikipedia.empty() && info.musicbrainz == "https://musicbrainz.org/artist/5b11f4ce-a62d-471e-81fc-a69a8278c7da",
          "the lookup keeps web links only, and the English Wikipedia only");
    online::ArtistInfo bad;
    g.Add(!online::ParseArtistLookup(R"j({"id":"x","name":"Nirvana"})j", bad) &&
              online::ParseArtistLookup(R"j({"id":"5b11f4ce-a62d-471e-81fc-a69a8278c7da","name":"N"})j", bad) && bad.official.empty(),
          "a lookup without a usable id fails; one without relations has no links");
    g.Add(online::ParseWikidataTitle(R"j({"entities":{"Q11649":{"id":"Q11649","sitelinks":{"enwiki":{"site":"enwiki","title":"Nirvana (band)"}}}}})j",
                                     "Q11649") == "Nirvana (band)" &&
              online::ParseWikidataTitle(R"j({"entities":{"Q11649":{"sitelinks":{}}}})j", "Q11649").empty(),
          "Wikidata gives the English article's title, or nothing");
    std::string extract, page;
    g.Add(online::ParseWikipediaSummary(R"j({"type":"standard","title":"Nirvana (band)","extract":"Nirvana was an American rock band.",)j"
                                        R"j("content_urls":{"desktop":{"page":"https://en.wikipedia.org/wiki/Nirvana_(band)"}}})j",
                                        extract, page) &&
              extract == "Nirvana was an American rock band." && page == "https://en.wikipedia.org/wiki/Nirvana_(band)",
          "the summary's text and page");
    g.Add(!online::ParseWikipediaSummary(R"j({"type":"disambiguation","extract":"Nirvana may refer to:"})j", extract, page) &&
              !online::ParseWikipediaSummary("<html>", extract, page),
          "a disambiguation page or a malformed reply gives no summary");
    return g;
}

// Pasted or dropped text: a data: URL holds the image itself; anything but an image address is
// refused without a request (an https address would be downloaded, so none is tried here).
inline Group TextImages() {
    Group g{"pasted text: data: URLs give their image, other text none"};
    online::Cancel cancel;
    std::string image;
    const std::string png64 = "iVBORw0KGgoAAAANSUhEUgAAAAgAAAAICAIAAABLbSncAAAACXBIWXMAAAABAAAAAQBPJcTWAAAAFElEQVR4nGM8lWLIgA2wYBUd"
                              "tBIAHKIBfT5NcXIAAAAASUVORK5CYII=";
    g.Add(online::ImageFromText("  data:image/png;base64," + png64 + "\r\n", cancel, image) == online::TextImage::Got &&
              image == Png(),
          "a base64 PNG data: URL, with spaces around it, gives the PNG byte for byte");
    g.Add(online::ImageFromText("data:image/png;base64,bm90IGFuIGltYWdl", cancel, image) == online::TextImage::Failed &&
              image.empty(),
          "a data: URL holding something else is refused");
    g.Add(online::ImageFromText("data:image/svg+xml,<svg/>", cancel, image) == online::TextImage::Failed,
          "a data: URL that isn't base64 is refused");
    g.Add(online::ImageFromText("Blue Train cover", cancel, image) == online::TextImage::NotImage && image.empty(),
          "plain text isn't an image");
    g.Add(online::ImageFromText("http://example.com/cover.jpg", cancel, image) == online::TextImage::Failed,
          "a plain http address is refused");
    return g;
}

// The user's own upload tool: it names a cover Discord can show, and its output is read defensively because it is
// somebody else's program.
inline Group CoverUpload() {
    Group g{"cover upload: the user's own tool turns a local image into an address Discord can show"};
#ifdef _WIN32
    const std::wstring prints = L"cmd.exe /c echo https://example.com/cover.png";
    const std::wstring silent = L"cmd.exe /c exit 0";
    const std::wstring unsafe = L"cmd.exe /c echo http://example.com/cover.png";
#else
    const std::wstring prints = L"echo https://example.com/cover.png";
    const std::wstring silent = L"true";
    const std::wstring unsafe = L"echo http://example.com/cover.png";
#endif
    coverupload::SetCommand(prints);
    g.Add(coverupload::Configured() && coverupload::Run(L"/covers/image.png") == "https://example.com/cover.png",
          "the address the tool prints is taken, whatever it prints after it");
    coverupload::SetCommand(silent);
    g.Add(coverupload::Run(L"/covers/image.png").empty(), "a tool that prints no address yields none");
    coverupload::SetCommand(unsafe);
    g.Add(coverupload::Run(L"/covers/image.png").empty(), "an address Discord cannot render is refused");
    coverupload::SetCommand(L"");
    g.Add(!coverupload::Configured() && coverupload::Run(L"/covers/image.png").empty(),
          "with no command configured nothing runs at all");
    return g;
}

// Custom covers: per track, kept once per image, persisted, restored, and unused images removed.
inline Group CustomCovers(const std::wstring& scratch) {
    Group g{"custom covers: shown per track, persisted, restored without touching the tracks"};
    const wchar_t sep = kPathSep;
    std::wstring data = scratch + sep + L"custom-covers", covers = data + sep + L"covers";
    for (const wchar_t* name : {L"covers.txt", L"covers.txt.tmp"}) DeleteFileW((covers + sep + name).c_str());
    CreateDirectoryW(data.c_str(), nullptr);
    std::wstring a = data + sep + L"Album" + sep + L"01 One.flac", b = data + sep + L"Album" + sep + L"02 Two.flac",
                 c = data + sep + L"Album" + sep + L"03 Three.flac";
    customcover::Start(data);
    g.Add(customcover::ImageFor(a).empty() && customcover::Count({a, b, c}) == 0, "no track starts with a custom cover");
    std::wstring error;
    g.Add(customcover::Set({a, b}, Jpeg(), error), "a JPEG is given to two tracks");
    std::wstring jpg = customcover::ImageFor(a);
    g.Add(!jpg.empty() && jpg == customcover::ImageFor(b) && Read(jpg) == Jpeg() && customcover::ImageFor(c).empty(),
          "both tracks show one stored copy of it; the third keeps its own");
#ifdef _WIN32
    std::wstring upper = a;
    for (wchar_t& ch : upper) ch = (wchar_t)towupper(ch);
    g.Add(customcover::ImageFor(upper) == jpg, "Windows paths match without case");
#endif
    g.Add(customcover::Set({b}, Png(), error), "one track then gets a PNG");
    std::wstring png = customcover::ImageFor(b);
    g.Add(png != jpg && Read(png) == Png() && customcover::ImageFor(a) == jpg && Exists(jpg),
          "only that track changes; the JPEG stays for the other");
    g.Add(!customcover::Set({c}, "not an image, only some text pretending", error) && !error.empty() &&
              customcover::ImageFor(c).empty(),
          "something that isn't an image is refused and nothing changes");
    g.Add(customcover::Set({std::wstring(L"bad\tpath")}, Jpeg(), error) && customcover::Count({L"bad\tpath"}) == 0,
          "a path the list can't hold is left out");
    customcover::Start(data);
    g.Add(customcover::ImageFor(a) == jpg && customcover::ImageFor(b) == png && customcover::Count({a, b, c}) == 2,
          "the choices are read back after a restart");
    g.Add(customcover::Set({a}, Png(), error) && customcover::ImageFor(a) == png && !Exists(jpg),
          "an image no track uses any more is deleted");
    g.Add(customcover::Clear({a, c}) == 1 && customcover::ImageFor(a).empty() && Exists(png),
          "restoring one track keeps the image the other still uses");
    g.Add(customcover::Clear({b}) == 1 && !Exists(png) && Read(covers + sep + L"covers.txt") == "1\n" &&
              !Exists(covers + sep + L"covers.txt.tmp"),
          "restoring the last one removes the image and leaves an empty list behind");
    // The address a downloaded cover came from travels with it, for the Discord status; a picture the user picked
    // from a file has none, and an address Discord could not render is not kept.
    const std::string address = "https://e-cdns-images.dzcdn.net/images/cover/abc/1000x1000-000000-80-0-0.jpg";
    g.Add(customcover::Set({a}, Jpeg(), error, address) && customcover::SourceUrlFor(a) == FromUtf8(address) &&
              customcover::SourceUrlFor(b).empty(),
          "a downloaded cover keeps the address it came from, for the track given it");
    g.Add(customcover::Set({b}, Png(), error, "http://example.com/insecure.png") && customcover::SourceUrlFor(b).empty(),
          "an address Discord could not take is not kept; the cover still is");
    customcover::Start(data);
    g.Add(customcover::SourceUrlFor(a) == FromUtf8(address) && customcover::SourceUrlFor(b).empty(),
          "the address is read back after a restart");
    g.Add(customcover::Clear({a, b}) == 2 && customcover::SourceUrlFor(a).empty(),
          "restoring the covers forgets their addresses too");
    customcover::Start(L"");  // nothing else in the test run sees these choices
    return g;
}

// The Discord status's cover lookup: stores' edition notes and credits compare loosely, and a track with no album
// finds its song's album cover.
inline Group CoverAddresses() {
    Group g{"cover addresses: loose names and song searches"};
    using online::LooseTitle;
    g.Add(LooseTitle("Abbey Road (Remastered)") == LooseTitle("abbey road") &&
          LooseTitle("Rumours [2004 Remaster]") == "rumours" &&
          LooseTitle("Back in Black - 2003 Remaster") == "back in black", "edition notes are left out");
    g.Add(LooseTitle("Simon & Garfunkel") == LooseTitle("Simon and Garfunkel") &&
          LooseTitle("Don't Stop Me Now") == LooseTitle("Dont Stop Me Now") &&
          LooseTitle("AC/DC") == LooseTitle("AC DC"), "\"&\", apostrophes and punctuation do not matter");
    g.Add(LooseTitle("Daft Punk feat. Pharrell Williams") == "daft punk", "a featured artist is left out");
    g.Add(LooseTitle("Pumpkin Waltz (1 hour)") == "pumpkin waltz" && LooseTitle("(untitled)") == "(untitled)" &&
          LooseTitle("Mr. Brightside - The Killers") == "mr brightside the killers",
          "a title of nothing but notes keeps them, and a dash without an edition note stays");
    g.Add(LooseTitle("Sigur R\xC3\xB3s") == "sigur r\xC3\xB3s" && LooseTitle("Abbey Road") != LooseTitle("Let It Be"),
          "accented letters stay and different names differ");
    auto deezer = online::ParseDeezerTracks(
        R"({"data":[{"title":"Pumpkin Waltz","artist":{"name":"Peter Palmer"},"album":{"title":"Autumn",)"
        R"("cover_xl":"https://cdn-images.dzcdn.net/images/cover/x/1000x1000.jpg"}},)"
        R"({"title":"No Art","artist":{"name":"A"},"album":{"title":"B"}}]})");
    g.Add(deezer.size() == 1 && deezer[0].title == "Pumpkin Waltz" && deezer[0].album == "Autumn" &&
          deezer[0].artist == "Peter Palmer" && deezer[0].width == 1000 && deezer[0].url.ends_with("1000x1000.jpg"),
          "Deezer song results carry their album's cover; ones without art are left out");
    auto itunes = online::ParseItunesTracks(
        R"({"resultCount":1,"results":[{"trackName":"Song","artistName":"Artist","collectionName":"Album",)"
        R"("artworkUrl100":"https://is1-ssl.mzstatic.com/image/thumb/a/100x100bb.jpg"}]})");
    g.Add(itunes.size() == 1 && itunes[0].title == "Song" && itunes[0].url.ends_with("/1200x1200bb.jpg"),
          "iTunes song results ask for the 1200-pixel artwork");
    g.Add(online::ParseDeezerTracks(R"({"error":{"code":4}})").empty() && online::ParseItunesTracks("not json").empty(),
          "an error or malformed reply yields nothing");
    return g;
}

inline std::vector<Group> All(const std::wstring& scratch) {
    return {Parsers(), Scoring(), Images(), TextImages(), FolderWrites(scratch), CustomCovers(scratch), CoverUpload(),
            Review(scratch),
            Generations(), Lyrics(), LrcWrite(scratch), Artist(), CoverAddresses()};
}

}  // namespace metadata_checks
