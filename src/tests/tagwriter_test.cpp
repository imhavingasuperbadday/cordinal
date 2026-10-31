// Self-test for the tag writers: every check runs on copies, never on the user's files.
// Run as part of cordinal.exe --selftest ("tag writing: MP3 and MP4" / "FLAC, Ogg and Opus" groups)
// or standalone. It edits each format through the real WriteTags path, reads every field back, and
// re-decodes the audio after each step to prove the edit left the samples bit-identical.
#include "tagwriter.h"

#include "decoder.h"
#include "reference_audio.h"
#include "library.h"
#include "tags.h"
#include "testlog.h"

#include <cmath>
#include <cstdarg>
#include <initializer_list>
#include <string_view>
#include <utility>
#include <vector>

// Defined in selftest.cpp beside testlog::Check: a failed setup step is reported but not counted.
bool Ready(testlog::Condition ok, const wchar_t* what);

namespace {

// Byte access for the FLAC metadata scan in the FLAC/Ogg driver (same helpers the writers use).
using sv = std::string_view;
inline uint8_t U8(sv s, size_t i) { return i < s.size() ? (uint8_t)s[i] : 0; }
inline uint32_t BE24(sv s, size_t i) { return (uint32_t)U8(s, i) << 16 | (uint32_t)U8(s, i + 1) << 8 | U8(s, i + 2); }

// The edit steps every file goes through, each one check across all the files: a failing step names
// its files and what went wrong in them.
struct TestLog {
    testlog::Cases setAll, partial, grow, shrink, clear, audio;
};

// Decodes the whole file to interleaved float; the tag-undo checks compare these samples.
std::vector<float> DecodePcm(const std::wstring& path) {
    std::vector<float> all;
    std::wstring error;
    auto d = OpenDecoder(path, 48000, error);
    if (!d) return all;
    float buf[4096 * 2];
    while (size_t n = d->Read(buf, 4096)) all.insert(all.end(), buf, buf + n * 2);
    return all;
}

// Runs ffmpeg synchronously (output discarded) to make fixture variants; false if ffmpeg is absent.
bool RunFFmpeg(const std::wstring& args) {
    const FFmpegTools* ff = FindFFmpeg();
    if (!ff) return false;
    ChildProcess p;
    if (!p.Start(ff->ffmpeg, L"-hide_banner -loglevel error -y " + args)) return false;
    p.ReadAll();
    return true;
}

// Size in bytes, or 0 if the file cannot be queried.
uint64_t FileSize(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return 0;
    return (uint64_t)a.nFileSizeHigh << 32 | a.nFileSizeLow;
}

// A valid 2x2 orange PNG (built with zlib, CRCs included).
const std::string kPng = std::string(
    "\x89" "PNG\r\n\x1a\n" "\x00\x00\x00\x0d" "IHDR" "\x00\x00\x00\x02\x00\x00\x00\x02\x08\x02\x00\x00\x00"
    "\xfd\xd4\x9a\x73" "\x00\x00\x00\x14" "IDAT" "\x78\x9c\x63\xf8\xdf\xc0\xf0\xbf\x81\x01\x84\xff\x37\x30\x00\x00"
    "\x2d\xee\x05\xfd" "\x2e\xeb\x32\xac" "\x00\x00\x00\x00" "IEND" "\xae\x42\x60\x82",
    77);

// Runs the full edit cycle on one file: set every field, a partial edit, a 100 KB lyrics + 300 KB
// cover grow, a shrink, then a clear. After each step it re-decodes the audio (and re-reads the MP4
// gapless fields) to confirm the samples and the container's skip/valid pair are unchanged.
void TestFile(TestLog& t, const std::wstring& path) {
    std::wstring name = FileNameOf(path);
    auto pcm = DecodePcm(path);
    GaplessInfo gap0;
    std::wstring ext = ExtOf(path);
    bool mp4 = ext == L"m4a" || ext == L"mp4" || ext == L"m4b";
    bool padded = ext == L"mp3" || ext == L"flac" || ext == L"cord";  // formats edited in place when the tag shrinks
    if (mp4) ReadMp4Gapless(path, gap0);
    std::wstring audioChanged;  // the edit steps after which the audio no longer matched
    auto same = [&](const wchar_t* step) {
        auto now = DecodePcm(path);
        GaplessInfo gap;
        if (mp4) ReadMp4Gapless(path, gap);
        if (pcm.empty() || now != pcm || gap.skip != gap0.skip || gap.valid != gap0.valid)
            audioChanged += std::wstring(L" ") + step;
    };
    const std::string u = "\xC3\x9Cn\xC3\xAF" "c\xC3\xB6" "d\xC3\xA9 \xE2\x9C\x93";  // "Ünïcödé ✓"

    TagEdit all;
    all.title = "Title " + u;
    all.artist = "Artist " + u;
    all.album = "Album " + u;
    all.albumArtist = "Album Artist " + u;
    all.genre = "Genre " + u;
    all.date = "2024-01-15";
    all.composer = "Composer " + u;
    all.comment = "Comment " + u;
    all.grouping = "Grouping " + u;
    all.label = "Label " + u;
    all.lyrics = "Lyrics " + u + "\nsecond line";
    all.track = 3;
    all.trackTotal = 12;
    all.disc = 2;
    all.discTotal = 3;
    all.bpm = 128;
    all.compilation = true;
    all.cover = kPng;
    all.rating = "0.8";
    all.playCount = "12";
    std::wstring err;
    bool ok = WriteTags(path, all, err);
    TagData d;
    std::string lyrics, cover;
    ReadTags(path, d);
    ReadLyrics(path, lyrics);
    ReadEmbeddedArt(path, cover);
    std::wstring wrong;  // names of the fields that didn't read back
    auto expect = [&](bool good, const wchar_t* field) {
        if (!good) wrong += std::wstring(L" ") + field;
    };
    expect(d.title == *all.title, L"title");
    expect(d.artist == *all.artist, L"artist");
    expect(d.album == *all.album, L"album");
    expect(d.albumArtist == *all.albumArtist, L"albumartist");
    expect(d.genre == *all.genre, L"genre");
    expect(d.date == "2024-01-15" || (ext == L"mp3" && d.date == "2024"), L"date");  // ID3v2.3 keeps the year
    expect(d.composer == *all.composer, L"composer");
    expect(d.comment == *all.comment, L"comment");
    expect(d.grouping == *all.grouping, L"grouping");
    expect(d.label == *all.label, L"label");
    expect(lyrics == *all.lyrics, L"lyrics");
    expect(d.track == 3 && d.trackTotal == 12, L"track");
    expect(d.disc == 2 && d.discTotal == 3, L"disc");
    expect(d.bpm == 128, L"bpm");
    expect(d.compilation, L"compilation");
    expect(cover == kPng, L"cover");
    expect(d.rating == "0.8" && d.playCount == "12", L"statistics");
    t.setAll.Add(ok && wrong.empty(), name + (wrong.empty() ? L"" : L" wrong:" + wrong) + L" " + err);
    same(L"set all");

    TagEdit some;
    some.title = "Only the title";
    some.track = 5;  // the total (12) stays
    ok = WriteTags(path, some, err);
    d = TagData();
    ReadTags(path, d);
    t.partial.Add(ok && d.title == "Only the title" && d.artist == *all.artist && d.comment == *all.comment && d.track == 5 &&
                      d.trackTotal == 12 && d.disc == 2,
                  name + L" " + err);

    TagEdit grow;
    grow.lyrics = std::string(100000, 'l');
    grow.cover = kPng + std::string(300000, '\0');  // PNG readers stop at IEND; size is the point
    ok = WriteTags(path, grow, err);
    uint64_t grownSize = FileSize(path);
    lyrics.clear();
    ReadLyrics(path, lyrics);
    t.grow.Add(ok && lyrics.size() == 100000, name + L" " + err);
    same(L"grow");

    TagEdit shrink;
    shrink.lyrics = "";
    shrink.cover = "";
    ok = WriteTags(path, shrink, err);
    lyrics.clear();
    cover.clear();
    ReadLyrics(path, lyrics);
    bool noCover = !ReadEmbeddedArt(path, cover);
    bool inPlace = FileSize(path) == grownSize;
    t.shrink.Add(ok && lyrics.empty() && noCover && (!padded || inPlace),
                 name + (inPlace ? L" in place " : L" rewritten ") + err);
    same(L"shrink");

    TagEdit clear;
    clear.title = clear.artist = clear.comment = clear.label = clear.grouping = "";
    clear.track = clear.trackTotal = clear.bpm = 0;
    clear.compilation = false;
    clear.rating = clear.playCount = "";
    ok = WriteTags(path, clear, err);
    d = TagData();
    ReadTags(path, d);
    t.clear.Add(ok && d.artist.empty() && d.comment.empty() && d.label.empty() && d.grouping.empty() && !d.track &&
                    !d.trackTotal && !d.bpm && !d.compilation && d.rating.empty() && d.playCount.empty() &&
                    d.album == *all.album,
                name + L" " + err);
    same(L"clear");
    t.audio.Add(audioChanged.empty(), name + L" after" + audioChanged);

    TagEdit final;  // leave a fully tagged file behind (useful with CORDINAL_KEEP_TEST_FILES)
    final.title = all.title;
    final.cover = kPng;
    final.lyrics = all.lyrics;
    WriteTags(path, final, err);
}

}  // namespace

bool RunFFmpegForTest(const std::wstring& args) { return RunFFmpeg(args); }

// One group for the whole run, one check per edit step across every file; a failing step names the
// files it failed in.
int TestTagWriterFiles(const std::vector<std::wstring>& files, FILE* log, const wchar_t* label,
                       bool checkOpusR128) {
    testlog::SetLog(log);
    const int before = testlog::Failures();
    TestLog t;
    {
        testlog::Group checkGroup(label);
        for (auto& f : files) TestFile(t, f);
        testlog::Check(t.setAll.Passed(), L"setting every field reads each one back in all %zu files%ls", files.size(),
                       t.setAll.failed.c_str());
        testlog::Check(t.partial.Passed(), L"a partial edit keeps the other fields and the track total%ls",
                       t.partial.failed.c_str());
        testlog::Check(t.grow.Passed(), L"100 KB of lyrics and a 300 KB cover are written%ls", t.grow.failed.c_str());
        testlog::Check(t.shrink.Passed(), L"lyrics and cover are removed again, in place where the format pads%ls",
                       t.shrink.failed.c_str());
        testlog::Check(t.clear.Passed(), L"cleared fields are removed while the others stay%ls", t.clear.failed.c_str());
        testlog::Check(t.audio.Passed(), L"the audio is unchanged after every edit%ls", t.audio.failed.c_str());
        // rg.opus: the R128 gain the edits didn't touch must survive.
        if (checkOpusR128)
            for (auto& f : files)
                if (FileNameOf(f) == L"rg.opus") {
                    TagData tags;
                    ReadTags(f, tags);
                    testlog::Check(std::fabs(tags.rgTrackGain - (-1408 / 256.0f + 5)) < 0.01f,
                                   L"TAGWRITE rg.opus untouched R128 gain kept (%.2f)", tags.rgTrackGain);
                }
    }
    return testlog::Failures() - before;
}

// A check line for a file that could not be prepared because FFmpeg is missing.
void LogSkipped(FILE*, const std::wstring& name) {
    if (FindFFmpeg()) testlog::Check(false, L"FFmpeg failed to prepare required tag-writer variant: %ls", name.c_str());
    else testlog::Skip((L"tag writer variant " + name + L": FFmpeg unavailable").c_str());
}

// Removes a scratch folder, unless CORDINAL_KEEP_TEST_FILES keeps the files for other tools.
void DeleteTestFolder(const std::wstring& dir) {
    if (GetEnvironmentVariableW(L"CORDINAL_KEEP_TEST_FILES", nullptr, 0)) return;  // for checks with other tools
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
        while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

// The per-format drivers: they belong to the test exe, not the shipping tag writers.
// MP3/MP4 driver: copies the fixtures, adds FFmpeg-made variants (ID3v2.4, no tag, faststart M4A),
// then runs TestFile on each. Exit value is the number of failing checks.
int TestTagWriterMp3Mp4(const std::wstring& fixtures, const std::wstring& scratchDir, FILE* log) {
    const int before = testlog::Failures();
    std::wstring dir = scratchDir + L"\\tw-mp3mp4-test";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::vector<std::wstring> files;
    testlog::Cases copies;
    for (auto [from, to] : std::initializer_list<std::pair<const wchar_t*, const wchar_t*>>{
             {L"media\\sine.mp3", L"sine.mp3"}, {L"tags2\\full.mp3", L"full.mp3"}, {L"media\\sine.m4a", L"sine.m4a"},
             {L"media\\alac.m4a", L"alac.m4a"}, {L"tags2\\full.m4a", L"full.m4a"}})
    {
        bool copied = CopyFileW((fixtures + L"\\" + from).c_str(), (dir + L"\\" + to).c_str(), FALSE) != FALSE;
        copies.Add(copied, std::wstring(from) + L" error " + std::to_wstring(copied ? 0 : GetLastError()));
        if (copied) files.push_back(dir + L"\\" + to);
    }
    Ready(copies.Passed(), L"scratch copies of the MP3 and MP4 fixtures are prepared");
    // A frame that claims more bytes than the tag holds means the tag is damaged. Writing it would
    // silently drop every later frame, so the writer must refuse and leave the file byte-identical.
    std::string damaged;
    const std::wstring damagedPath = dir + L"\\damaged-tag.mp3";
    if (ReadFileBytes(dir + L"\\sine.mp3", damaged) && damaged.size() > 20 && damaged.compare(0, 3, "ID3") == 0) {
        const uint8_t version = (uint8_t)damaged[3];
        damaged[14] = (char)(version == 4 ? 0x7F : 0xFF);
        damaged[15] = (char)(version == 4 ? 0x7F : 0xFF);
        damaged[16] = (char)(version == 4 ? 0x7F : 0xFF);
        damaged[17] = (char)(version == 4 ? 0x7F : 0x7F);
        HANDLE h = CreateFileW(damagedPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD written = 0;
        const bool kept = h != INVALID_HANDLE_VALUE && WriteFile(h, damaged.data(), (DWORD)damaged.size(), &written, nullptr) &&
                          written == damaged.size();
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        TagEdit edit;
        edit.title = "Must not be written";
        std::wstring error;
        std::string after;
        const bool refused = kept && !WriteTags(damagedPath, edit, error) && !error.empty() &&
                             ReadFileBytes(damagedPath, after) && after == damaged;
        testlog::Check(refused, L"tag writer refuses a damaged ID3 frame instead of dropping the frames after it");
        DeleteFileW(damagedPath.c_str());
    }
    // Variants FFmpeg makes: ID3v2.4, no ID3 tag at all, and an M4A with moov before mdat
    // (edits then have to move the audio, so chunk offsets must be fixed).
    std::wstring src = QuoteArg(fixtures + L"\\media\\sine.mp3"), srcM4a = QuoteArg(fixtures + L"\\media\\sine.m4a");
    std::pair<std::wstring, std::wstring> variants[] = {
        {L"-i " + src + L" -c copy -id3v2_version 4 ", L"v24.mp3"},
        {L"-i " + src + L" -c copy -write_id3v2 0 -map_metadata -1 ", L"notag.mp3"},
        {L"-i " + srcM4a + L" -c copy -movflags +faststart ", L"fast.m4a"},
    };
    for (auto& [args, name] : variants) {
        std::wstring out = dir + L"\\" + name;
        if (RunFFmpegForTest(args + QuoteArg(out)) && FileSize(out)) files.push_back(out);
        else LogSkipped(log, name);
    }
    TestTagWriterFiles(files, log, L"tag writing: MP3 and MP4");
    DeleteTestFolder(dir);
    return testlog::Failures() - before;
}

// FLAC/Ogg/Opus driver: copies the fixtures, adds a FLAC with its padding block stripped (so the
// first edit must rebuild the file), and also checks rg.opus keeps its R128 gain.
int TestTagWriterFlacOgg(const std::wstring& fixtures, const std::wstring& scratchDir, FILE* log) {
    const int before = testlog::Failures();
    std::wstring dir = scratchDir + L"\\tw-flacogg-test";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::vector<std::wstring> files;
    testlog::Cases copies;
    for (auto [from, to] : std::initializer_list<std::pair<const wchar_t*, const wchar_t*>>{
             {L"media\\sine.flac", L"sine.flac"}, {L"tags2\\full.flac", L"full.flac"}, {L"media\\sine.ogg", L"sine.ogg"},
             {L"tags2\\full.ogg", L"full.ogg"}, {L"media\\sine.opus", L"sine.opus"}, {L"rg\\rg.opus", L"rg.opus"}})
    {
        bool copied = CopyFileW((fixtures + L"\\" + from).c_str(), (dir + L"\\" + to).c_str(), FALSE) != FALSE;
        copies.Add(copied, std::wstring(from) + L" error " + std::to_wstring(copied ? 0 : GetLastError()));
        if (copied) files.push_back(dir + L"\\" + to);
    }
    // A FLAC with no padding at all (FFmpeg always writes some, so strip it here): the first
    // edit has to rebuild the file.
    std::string flac, stripped = "fLaC";
    bool prepared = false;
    if (ReadFileBytes(fixtures + L"\\media\\sine.flac", flac)) {
        std::vector<std::string> blocks;
        for (size_t p = 4, last = 0; !last && p + 4 <= flac.size();) {
            last = U8(flac, p) & 0x80;
            uint32_t len = BE24(flac, p + 1);
            if ((U8(flac, p) & 0x7F) != 1) blocks.push_back(flac.substr(p, 4 + len));
            p += 4 + len;
            if (last) {
                for (size_t i = 0; i < blocks.size(); ++i) {
                    blocks[i][0] = (char)((blocks[i][0] & 0x7F) | (i + 1 == blocks.size() ? 0x80 : 0));
                    stripped += blocks[i];
                }
                stripped.append(flac, p, std::string::npos);
            }
        }
        std::wstring out = dir + L"\\nopad.flac";
        HANDLE h = CreateFileW(out.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD written = 0;
        bool ok = h != INVALID_HANDLE_VALUE && WriteFile(h, stripped.data(), (DWORD)stripped.size(), &written, nullptr) &&
                  written == stripped.size();
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        if (ok) { files.push_back(out); prepared = true; }
    }
    copies.Add(prepared, L"a FLAC without padding");
    Ready(copies.Passed(), L"scratch copies of the FLAC, Ogg and Opus fixtures and a FLAC without padding "
                           L"are prepared");
    TestTagWriterFiles(files, log, L"tag writing: FLAC, Ogg and Opus", true);
    DeleteTestFolder(dir);
    return testlog::Failures() - before;
}
