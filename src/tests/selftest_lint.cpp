// Self-tests for Library > Lint library... (library_lint.h): split albums, near-duplicate
// artists, mojibake / legacy encoding repair, cover-art audit, and atomic edit collection.
#include "selftest_common.h"

#include "library_lint.h"
#include "tagundo.h"
#include "tagwriter.h"

#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;

namespace {

std::string MakeMinimalPng(int w, int h, size_t padIdatBytes = 0) {
    auto be32 = [](uint32_t v) {
        std::string s(4, '\0');
        s[0] = (char)(v >> 24);
        s[1] = (char)(v >> 16);
        s[2] = (char)(v >> 8);
        s[3] = (char)v;
        return s;
    };
    auto chunk = [&](const char* type, const std::string& data) {
        return be32((uint32_t)data.size()) + std::string(type, 4) + data + std::string(4, '\0');
    };
    std::string ihdr = be32((uint32_t)w) + be32((uint32_t)h) + std::string("\x08\x02\x00\x00\x00", 5);
    std::string out = "\x89PNG\r\n\x1a\n" + chunk("IHDR", ihdr);
    if (padIdatBytes > 0) out += chunk("IDAT", std::string(padIdatBytes, 'X'));
    out += chunk("IEND", "");
    return out;
}

std::string MakeMinimalJpeg(int w, int h) {
    auto be16 = [](uint16_t v) {
        std::string s(2, '\0');
        s[0] = (char)(v >> 8);
        s[1] = (char)v;
        return s;
    };
    // SOI + APP0 (JFIF, 18 bytes) + SOF0 (13 bytes) + EOI (>= 32 bytes for online::InspectImage)
    std::string app0("\xFF\xE0\x00\x10" "JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00", 18);
    std::string sof = std::string("\xFF\xC0\x00\x0B\x08", 5) + be16((uint16_t)h) + be16((uint16_t)w) +
                      std::string("\x01\x01\x11\x00", 4);
    return std::string("\xFF\xD8", 2) + app0 + sof + std::string("\xFF\xD9", 2);
}

// Encodes raw bytes as if they were Latin-1 characters stored in UTF-8 (classic mojibake).
std::string BytesAsLatin1Utf8(std::string_view raw) {
    std::string out;
    for (unsigned char b : raw) {
        if (b < 0x80) {
            out.push_back((char)b);
        } else {
            out.push_back((char)(0xC0 | (b >> 6)));
            out.push_back((char)(0x80 | (b & 0x3F)));
        }
    }
    return out;
}

TrackId AddLibTrack(Library& lib, const std::wstring& path, const char* title, const char* artist,
                    const char* album, const char* albumArtist = "") {
    TrackId id = lib.Add(path);
    Track& t = lib[id];
    t.flags = Track::kInLibrary | Track::kLoaded;
    t.title = lib.Intern(title);
    t.artist = lib.Intern(artist);
    t.album = lib.Intern(album);
    if (albumArtist && *albumArtist) t.albumArtist = lib.Intern(albumArtist);
    return id;
}

}  // namespace

void TestLibraryLint(const std::wstring& fixturesMedia, const std::wstring& scratch) {
    testlog::Batch b(L"library-lint");
    std::error_code ec;
    fs::create_directories(scratch, ec);

    // 1. Mojibake detection & repair (and no false positives on valid accented text).
    {
        std::string fixed;
        std::wstring enc;

        // UTF-8 "Café" misread as Latin-1: "CafÃ©"
        std::string utf8Garbled = BytesAsLatin1Utf8("Caf\xC3\xA9");
        Check(lint::DetectMojibake(utf8Garbled, fixed, enc) && fixed == "Caf\xC3\xA9",
              L"repairs UTF-8 misread as Latin-1");

        // Shift-JIS "あいう" (0x82 0xA0 0x82 0xA2 0x82 0xA4) misread as Latin-1
        std::string sjisGarbled = BytesAsLatin1Utf8("\x82\xA0\x82\xA2\x82\xA4");
        Check(lint::DetectMojibake(sjisGarbled, fixed, enc) &&
                  fixed == "\xE3\x81\x82\xE3\x81\x84\xE3\x81\x86" && enc == L"Shift-JIS",
              L"repairs Shift-JIS misread as Latin-1");

        // CP1251 "Привет" (0xCF 0xF0 0xE8 0xE2 0xE5 0xF2) misread as Latin-1
        std::string cp1251Garbled = BytesAsLatin1Utf8("\xCF\xF0\xE8\xE2\xE5\xF2");
        Check(lint::DetectMojibake(cp1251Garbled, fixed, enc) &&
                  fixed == "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82" && enc == L"CP1251",
              L"repairs CP1251 misread as Latin-1");

        // Valid Western European words must NOT be flagged.
        Check(!lint::DetectMojibake("Bj\xC3\xB6rk", fixed, enc), L"does not flag Björk");
        Check(!lint::DetectMojibake("Caf\xC3\xA9 \xC3\x9C" "ber", fixed, enc), L"does not flag Café Über");
    }

    // 2. Split albums & Near-duplicate artists & CollectCheckedEdits merging.
    {
        Library lib;
        std::wstring dir = scratch + L"\\Album1";
        TrackId t1 = AddLibTrack(lib, dir + L"\\01.mp3", "One", "Daft Punk", "Discovery", "Daft Punk");
        TrackId t2 = AddLibTrack(lib, dir + L"\\02.mp3", "Two", "Daft Punk feat. Romanthony", "Discovery",
                                 "Daft Punk feat. Romanthony");
        TrackId t3 = AddLibTrack(lib, dir + L"\\03.mp3", "Three", "Daft Punk", "Discovery", "Daft Punk");
        TrackId t4 = AddLibTrack(lib, scratch + L"\\Other\\01.mp3", "Four", "Bj\xC3\xB6rk", "Homogenic");
        TrackId t5 = AddLibTrack(lib, scratch + L"\\Other\\02.mp3", "Five", "Bj\xC3\xB6rk", "Homogenic");
        TrackId t6 = AddLibTrack(lib, scratch + L"\\Other\\03.mp3", "Six", "Bjork", "Debut");
        TrackId t7 = AddLibTrack(lib, scratch + L"\\Beatles\\01.mp3", "Help", "The Beatles", "Help!");
        TrackId t8 = AddLibTrack(lib, scratch + L"\\Beatles\\02.mp3", "Yesterday", "The Beatles", "Help!");
        TrackId t9 = AddLibTrack(lib, scratch + L"\\Beatles\\03.mp3", "Run", "Beatles", "Rubber Soul");
        (void)t1;
        (void)t3;
        (void)t4;
        (void)t5;
        (void)t7;
        (void)t8;

        std::vector<lint::LintFinding> findings;
        lint::FindMetadataIssues(lib, findings);

        bool sawSplit = false, sawBjork = false, sawBeatles = false;
        for (const auto& f : findings) {
            if (f.kind == lint::LintKind::SplitAlbum && f.fixes.size() == 1 && f.fixes[0].track == t2 &&
                f.fixes[0].edit.albumArtist == "Daft Punk")
                sawSplit = true;
            if (f.kind == lint::LintKind::NearDuplicateArtist && f.fixes.size() == 1 && f.fixes[0].track == t6 &&
                f.fixes[0].edit.artist == "Bj\xC3\xB6rk")
                sawBjork = true;
            if (f.kind == lint::LintKind::NearDuplicateArtist && f.fixes.size() == 1 && f.fixes[0].track == t9 &&
                f.fixes[0].edit.artist == "The Beatles")
                sawBeatles = true;
        }
        Check(sawSplit, L"detects split album and unifies Album Artist to majority");
        Check(sawBjork, L"detects Bjork vs Björk and prefers diacritic canonical spelling");
        Check(sawBeatles, L"detects Beatles vs The Beatles and unifies");

        auto edits = lint::CollectCheckedEdits(findings);
        Check(edits.size() == 3, L"collects one merged TagEdit per affected track");
    }

    // 3. Cover-art audit on real copied media files + WriteTags + TagUndo round-trip.
    std::wstring sampleMp3 = fixturesMedia + L"\\sine.mp3";
    Check(fs::exists(sampleMp3), L"media\\sine.mp3 fixture exists for cover-art audit");
    if (fs::exists(sampleMp3)) {
        std::wstring covDir = scratch + L"\\CovAlbum";
        fs::create_directories(covDir, ec);
        std::wstring p1 = covDir + L"\\01.mp3";
        std::wstring p2 = covDir + L"\\02.mp3";
        std::wstring p3 = covDir + L"\\03.mp3";
        fs::copy_file(sampleMp3, p1, fs::copy_options::overwrite_existing, ec);
        fs::copy_file(sampleMp3, p2, fs::copy_options::overwrite_existing, ec);
        fs::copy_file(sampleMp3, p3, fs::copy_options::overwrite_existing, ec);

        std::wstring err;
        // p1: bloated PNG (600x600, >1.5 MB)
        TagEdit e1;
        e1.cover = MakeMinimalPng(600, 600, 1600u << 10);
        Check(WriteTags(p1, e1, err), L"writes bloated PNG to test mp3");

        // p2: no cover (inconsistent album!)
        // p3: low-res non-square JPEG (300x200)
        TagEdit e3;
        e3.cover = MakeMinimalJpeg(300, 200);
        Check(WriteTags(p3, e3, err), L"writes low-res non-square JPEG to test mp3");

        Library lib;
        AddLibTrack(lib, p1, "One", "Artist", "CoverTest");
        AddLibTrack(lib, p2, "Two", "Artist", "CoverTest");
        AddLibTrack(lib, p3, "Three", "Artist", "CoverTest");

        std::string fakeCompactJpeg = MakeMinimalJpeg(600, 600);
        auto reenc = [&](const std::string&, std::string& out) {
            out = fakeCompactJpeg;
            return true;
        };

        std::vector<lint::LintFinding> findings;
        lint::AuditCoverArt(lib, reenc, findings);

        bool sawBloated = false, sawLowRes = false, sawNonSquare = false, sawInconsistent = false;
        for (const auto& f : findings) {
            if (f.coverIssue == lint::CoverIssue::BloatedPng && f.fixable && f.checked) sawBloated = true;
            if (f.coverIssue == lint::CoverIssue::LowRes && !f.fixable && !f.checked) sawLowRes = true;
            if (f.coverIssue == lint::CoverIssue::NonSquare && !f.fixable && !f.checked) sawNonSquare = true;
            if (f.coverIssue == lint::CoverIssue::Inconsistent && f.fixable && f.checked) sawInconsistent = true;
        }
        Check(sawBloated, L"flags bloated embedded PNG and marks fixable");
        Check(sawLowRes, L"flags low-res (<500px) cover");
        Check(sawNonSquare, L"flags non-square cover");
        Check(sawInconsistent, L"flags inconsistent album covers");

        auto edits = lint::CollectCheckedEdits(findings);
        Check(edits.size() == 3, L"collects cover fixes for all 3 tracks");

        // Verify undo journal captures before writing and restores original state.
        TagJournal journal;
        for (const auto& [tid, edit] : edits) {
            TagUndoFile u;
            if (CaptureTagUndo(lib.Path(tid), edit, u)) journal.files.push_back(std::move(u));
            Check(WriteTags(lib.Path(tid), edit, err), L"applies cover lint fix");
        }
        std::string artAfter;
        Check(ReadEmbeddedArt(p2, artAfter) && artAfter == fakeCompactJpeg, L"inconsistent track received cover");

        // Replay undo on p2.
        for (const auto& u : journal.files) {
            if (u.path == p2) Check(WriteTags(u.path, u.old, err), L"undoes cover fix on track 2");
        }
        std::string artUndone;
        Check(!ReadEmbeddedArt(p2, artUndone) || artUndone.empty(), L"track 2 cover removed again after undo");
    }

    DeleteTestFolder(scratch);
}
