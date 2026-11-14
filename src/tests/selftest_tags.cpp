// Tag checks of the self test (see selftest.cpp): tag reading, ReplayGain tags, chapters and
// lyrics, Steam soundtracks, crash dumps and ListenBrainz.
#include "selftest_common.h"

#include "tests/tag_suggest_checks.h"

// The tag editor's completion values, shared with the Linux core test (tag_suggest_checks.h).
void TestTagSuggestions() {
    for (const auto& group : tag_suggest_checks::All()) {
        std::wstring detail;
        for (const auto& failure : group.failures) {
            if (!detail.empty()) detail += L" | ";
            detail += FromUtf8(failure);
        }
        const std::wstring name = FromUtf8(group.name);
        Check(group.Passed(), L"%ls%ls", name.c_str(), detail.empty() ? L"" : (L": " + detail).c_str());
    }
}

// Reads tags from every media fixture and checks codec, duration (~8 s), rate, channels and the
// standard title/artist/album/track tags (WAV and Opus carry no tags by design).
void TestTags(const std::wstring& dir) {
    std::vector<std::wstring> files;
    if (!Ready(CollectAudioFiles(dir, files) && !files.empty(), L"the media fixtures can be enumerated"))
        return;
    std::wstring bad;
    for (auto& f : files) {
        TagData t;
        bool ok = ReadTags(f, t);
        std::wstring ext = ExtOf(f);
        bool tagged = ext != L"wav" && ext != L"opus";
        bool good = ok && std::fabs(t.duration - 8.0) < 0.15 && t.sampleRate >= 44100 && t.channels == 2 &&
                    (!tagged || (t.title == "Sine \xC3\x9Cn\xC3\xAF" "code" && t.artist == "Test Artist" &&
                                 t.album == "Test Album" && t.track == 7));
        if (!good) bad += std::wstring(L" ") + FileNameOf(f) + L"(codec=" + CodecName(t.codec) + L" dur=" +
                            std::to_wstring(t.duration) + L")";
    }
    Check(bad.empty(), L"TAGS all %zu media files read their codec, duration, rate, channels and tags%ls", files.size(),
          bad.c_str());
}

// The tags2/ "full" fixtures: composer, comment, grouping, label, track/disc totals, BPM,
// compilation and lyrics, across FLAC, MP3, M4A and Ogg (M4A has no label, MP3 no USLT lyrics).
void TestExtendedTags(const std::wstring& dir) {
    std::wstring bad;
    for (const wchar_t* name : {L"full.flac", L"full.mp3", L"full.m4a", L"full.ogg"}) {
        TagData t;
        std::string lyrics;
        bool tagsOk = ReadTags(dir + L"\\" + name, t);
        bool lyricsOk = ReadLyrics(dir + L"\\" + name, lyrics);
        // M4A doesn't have standard label support in ffmpeg; MP3 doesn't support USLT via ffmpeg metadata
        bool isM4a = (std::wstring(name) == L"full.m4a");
        bool isMp3 = (std::wstring(name) == L"full.mp3");
        bool good = tagsOk && t.composer == "Test Composer" && t.comment == "Test Comment" &&
                    t.grouping == "Test Group" && (isM4a || t.label == "Test Label") &&
                    t.track == 3 && t.trackTotal == 12 &&
                    t.disc == 2 && t.discTotal == 3 &&
                    t.bpm == 128 && t.compilation && (isMp3 || (lyricsOk && lyrics == "La la la"));
        if (!good) bad += std::wstring(L" ") + name;
    }
    Check(bad.empty(), L"EXT TAGS  composer, comment, grouping, label, track/disc totals, BPM, compilation and lyrics in FLAC, MP3, M4A and Ogg%ls",
          bad.c_str());
}

namespace {

std::string Be32(uint32_t v) {
    std::string b(4, '\0');
    for (int i = 3; i >= 0; --i, v >>= 8) b[i] = (char)(v & 255);
    return b;
}

std::string Mp4Box(const char* type, const std::string& body) {
    std::string box = Be32((uint32_t)(8 + body.size()));
    box.append(type, 4);
    box += body;
    return box;
}

std::string Mp4Data(uint32_t kind, const std::string& payload) {
    return Mp4Box("data", Be32(kind) + std::string(4, '\0') + payload);
}

std::string Mp4Item(const char* type, uint32_t kind, const std::string& payload) {
    return Mp4Box(type, Mp4Data(kind, payload));
}

std::string Utf16Be(std::wstring_view text) {
    std::string out;
    for (wchar_t c : text) {
        out += (char)((uint16_t)c >> 8);
        out += (char)((uint16_t)c & 255);
    }
    return out;
}

std::string Mp4Key(const char* key) {
    std::string name(key);
    return Be32((uint32_t)(8 + name.size())) + "mdta" + name;
}

}  // namespace

// Every container signature, cut off anywhere inside the first 12 bytes the reader probes. A 4-byte
// "RIFF" or "FORM" once threw std::out_of_range from a form-type comparison, which ended the
// player during a library scan.
void TestShortTagHeaders(const std::wstring& scratch) {
    testlog::Cases shortFiles;
    const std::wstring path = scratch + L"\\short-header.mp3";
    static const std::string_view kMagics[] = {
        {"", 0}, {"RIFF", 4}, {"RF64", 4}, {"BW64", 4}, {"FORM", 4}, {"FORM\0\0\0\0AIFF", 12}, {"FORM\0\0\0\0" "8SVX", 12},
        {"OggS", 4}, {"fLaC", 4}, {"caff", 4}, {".snd", 4}, {"dns.", 4}, {"DSD ", 4}, {"FRM8", 4},
        {"MPCK", 4}, {"MP+\x07", 4}, {"ID3\x04", 4}, {"\x1A\x45\xDF\xA3", 4}, {"Creative Voice File\x1A", 20},
        {"\0\0\0\0ftyp", 8},
    };
    for (std::string_view magic : kMagics) {
        for (size_t size = 0; size < std::max<size_t>(magic.size() + 1, 12); ++size) {
            std::string bytes(magic.substr(0, size));
            bytes.resize(size, '\0');
            if (!Ready(WriteTestBytes(path, bytes), L"the short media header is prepared")) return;
            TagData tags;
            std::string image, lyrics;
            std::vector<Chapter> chapters;
            int64_t loopStart = 0, loopEnd = 0;
            bool tagsOk = ReadTags(path, tags);
            bool artOk = ReadEmbeddedArt(path, image);
            bool lyricsOk = ReadLyrics(path, lyrics);
            bool chaptersOk = ReadChapters(path, chapters);
            bool loopsOk = ReadLoopPoints(path, loopStart, loopEnd);
            std::wstring name(magic.begin(), magic.begin() + std::min<size_t>(magic.size(), 4));
            for (wchar_t& c : name) if (c < 32 || c > 126) c = L'.';
            shortFiles.Add(!tagsOk && !artOk && !lyricsOk && !chaptersOk && !loopsOk,
                           L"\"" + name + L"\" in " + std::to_wstring(size) + L" bytes");
        }
    }
    Check(shortFiles.Passed(), L"short media headers fail safely for tags, covers, lyrics, chapters and loops%ls",
          shortFiles.failed.c_str());
}

// MP4 text atoms say whether they are UTF-8 (1), UTF-16BE (2) or implicit; mdta metadata refers
// to fields by key index. A file cut off after ftyp is not a successful read with empty tags.
void TestMp4Metadata(const std::wstring& scratch) {
    std::string ftyp = Mp4Box("ftyp", std::string("M4A \0\0\0\0M4A mp42isom", 20));
    std::string ilst = Mp4Box("ilst", Mp4Item("\xA9nam", 2, Utf16Be(L"\u00DCnicode \u2713")) +
                                       Mp4Item("\xA9" "ART", 2, Utf16Be(L"Art\u00EDst")));
    std::string moov = Mp4Box("moov", Mp4Box("udta", Mp4Box("meta", std::string(4, '\0') + ilst)));
    const std::wstring utf16 = scratch + L"\\mp4-utf16.mp4";
    Ready(WriteTestBytes(utf16, ftyp + moov), L"the UTF-16 MP4 fixture is prepared");
    TagData t;
    bool utf16Ok = ReadTags(utf16, t);
    testlog::Batch batch;
    batch.Next(L"MP4 text atoms decode in their stated encoding, mdta metadata resolves its numeric keys, and a "
               L"truncated file is unreadable rather than empty tags");
    Check(utf16Ok && t.title == "\xC3\x9Cnicode \xE2\x9C\x93" && t.artist == "Art\xC3\xADst",
          L"MP4 UTF-16BE text atoms decode to the title and artist the file names");

    const char* keyTitle = "com.apple.quicktime.title";
    const char* keyArtist = "com.apple.quicktime.artist";
    std::string keys = Mp4Box("keys", std::string(4, '\0') + Be32(2) + Mp4Key(keyTitle) + Mp4Key(keyArtist));
    std::string numeric = Mp4Box("ilst", Mp4Item("\0\0\0\1", 1, "QuickTime title") +
                                          Mp4Item("\0\0\0\2", 1, "QuickTime artist"));
    std::string mdta = Mp4Box("moov", Mp4Box("udta", Mp4Box("meta", std::string(4, '\0') + keys + numeric)));
    const std::wstring quicktime = scratch + L"\\mp4-mdta.mp4";
    Ready(WriteTestBytes(quicktime, ftyp + mdta), L"the mdta MP4 fixture is prepared");
    TagData q;
    bool mdtaOk = ReadTags(quicktime, q);
    Check(mdtaOk && q.title == "QuickTime title" && q.artist == "QuickTime artist",
          L"MP4 mdta metadata resolves its numeric keys to title and artist");

    const std::wstring truncated = scratch + L"\\mp4-truncated.mp4";
    Ready(WriteTestBytes(truncated, ftyp), L"the truncated MP4 fixture is prepared");
    TagData cut;
    Check(!ReadTags(truncated, cut), L"a truncated MP4 is reported as unreadable, not as empty tags");
}

// An ID3v2 tag of up to 64 KiB is read whole and parsed in memory; a larger one (a big APIC) is streamed
// frame by frame. Both must give the same text, the cover byte for byte, and the MPEG stream after them.
void TestId3ReadPaths(const std::wstring& scratch) {
    auto frame = [](const char* id, const std::string& body) {  // an ID3v2.3 frame: id, size, two flag bytes
        return std::string(id, 4) + Be32((uint32_t)body.size()) + std::string(2, '\0') + body;
    };
    // MPEG-1 Layer III frames (128 kb/s, 44.1 kHz, 417 bytes each): each one's sync confirms the one before.
    std::string audio;
    for (int i = 0; i < 3; ++i) audio += std::string("\xFF\xFB\x90\x00", 4) + std::string(413, '\0');
    testlog::Cases paths;
    for (size_t coverBytes : {size_t(2000), size_t(150000)}) {
        std::string cover = "\x89PNG";
        for (size_t i = cover.size(); i < coverBytes; ++i) cover += (char)(i * 31 % 251);
        std::string frames = frame("TIT2", std::string("\0Id3 title", 10)) +
                             frame("TPE1", std::string("\0Id3 artist", 11)) + frame("TRCK", std::string("\0" "5/9", 4)) +
                             frame("APIC", std::string("\0image/png\0\x03\0", 13) + cover);
        frames.append(512, '\0');  // padding, as taggers leave it
        const uint32_t n = (uint32_t)frames.size();
        std::string tag = std::string("ID3\x03\0\0", 6);  // version 2.3, no flags, then the syncsafe size
        for (int shift : {21, 14, 7, 0}) tag += (char)(n >> shift & 127);
        const std::wstring path = scratch + L"\\id3-" + std::to_wstring(coverBytes) + L".mp3";
        if (!Ready(WriteTestBytes(path, tag + frames + audio), L"the ID3 fixture is prepared")) return;
        TagData t;
        std::string image;
        const bool ok = ReadTags(path, t) && ReadEmbeddedArt(path, image);
        paths.Add(ok && t.title == "Id3 title" && t.artist == "Id3 artist" && t.track == 5 && t.trackTotal == 9 &&
                      t.codec == Codec::MP3 && t.sampleRate == 44100 && image == cover,
                  (coverBytes < 65536 ? L"a tag read whole" : L"a streamed tag"));
    }
    Check(paths.Passed(), L"ID3v2 reads the same tags and cover whether the tag is read whole or streamed%ls",
          paths.failed.c_str());
}

// ReplayGain tags across FLAC/MP3/Ogg (+ Opus's native R128), then an export round trip on copies:
// writing the four gain/peak fields reads back exactly, and clearing them removes every tag again.
void TestReplayGainTags(const std::wstring& root, const std::wstring& scratchDir) {
    const std::wstring dir = root + L"\\rg";
    bool rgTags = true;
    for (const wchar_t* name : {L"rg.flac", L"rg.mp3", L"rg.ogg"}) {
        TagData t;
        ReadTags(dir + L"\\" + name, t);
        rgTags &= std::fabs(t.rgTrackGain + 6.5f) < 0.01f && std::fabs(t.rgTrackPeak - 0.988f) < 0.001f &&
                  std::fabs(t.rgAlbumGain + 7.25f) < 0.01f;
    }
    testlog::Batch batch;
    batch.Next(L"ReplayGain tags read back in FLAC, MP3 and Ogg, and written gains and peaks read back and clear "
               L"again on FLAC, MP3, Ogg, Opus (with its R128) and M4A copies");
    Check(rgTags, L"RG TAGS flac, mp3 and ogg track gain -6.50 dB, peak 0.988 and album gain -7.25 dB");
    // The native R128 -> track gain of rg.opus is checked on an edited copy by the tag-writing-ogg suite.

    // Export round trip: write these values to copies and read them back. Opus also gets its
    // native R128 tags; clearing the fields removes every tag again.
    std::wstring out = scratchDir + L"\\rg-export";
    CreateDirectoryW(out.c_str(), nullptr);
    const std::wstring sources[] = {dir + L"\\rg.flac", dir + L"\\rg.mp3", dir + L"\\rg.ogg", dir + L"\\rg.opus",
                                    root + L"\\gap2\\a.m4a"};
    testlog::Cases written, cleared;
    for (const std::wstring& src : sources) {
        if (GetFileAttributesW(src.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        std::wstring name = src.substr(src.find_last_of(L'\\') + 1);
        bool opus = name == L"rg.opus";
        std::wstring dst = out + L"\\" + name;
        if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
            written.Add(false, name + L" could not be copied");
            continue;
        }
        TagEdit e;
        e.rgTrackGain = "+1.25 dB";
        e.rgTrackPeak = "0.987654";
        e.rgAlbumGain = "-2.50 dB";
        e.rgAlbumPeak = "1.012345";
        e.r128TrackGain = "-960";   // +1.25 dB relative to -18 is -3.75 relative to -23
        e.r128AlbumGain = "-1920";  // -2.50 dB relative to -18 is -7.50 relative to -23
        std::wstring error;
        const bool wrote = WriteTags(dst, e, error);
        TagData r;
        ReadTags(dst, r);
        bool ok = wrote && std::fabs(r.rgTrackGain - 1.25f) < 0.01f && std::fabs(r.rgTrackPeak - 0.987654f) < 0.001f &&
                  std::fabs(r.rgAlbumGain + 2.50f) < 0.01f && std::fabs(r.rgAlbumPeak - 1.012345f) < 0.001f;
        if (opus) ok = ok && r.r128TrackGain == -960 && r.r128AlbumGain == -1920;
        wchar_t detail[128];
        swprintf_s(detail, L"%ls wrote=%d track=%.2f peak=%.4f album=%.2f r128=%d", name.c_str(), wrote, r.rgTrackGain,
                   r.rgTrackPeak, r.rgAlbumGain, r.r128TrackGain);
        written.Add(ok, detail);

        TagEdit clear;
        clear.rgTrackGain = clear.rgTrackPeak = clear.rgAlbumGain = clear.rgAlbumPeak = "";
        clear.r128TrackGain = clear.r128AlbumGain = "";
        std::wstring error2;
        WriteTags(dst, clear, error2);
        TagData gone;
        ReadTags(dst, gone);
        cleared.Add(std::isnan(gone.rgTrackGain) && std::isnan(gone.rgAlbumGain), name);
        DeleteFileW(dst.c_str());
    }
    Check(written.Passed(), L"RG EXPORT FLAC, MP3, Ogg, Opus (with R128) and M4A copies read back the written gains and "
          L"peaks%ls", written.failed.c_str());
    Check(cleared.Passed(), L"RG EXPORT clearing the fields removes every ReplayGain tag again%ls", cleared.failed.c_str());
    RemoveDirectoryW(out.c_str());
}

// Steam library folders: both the 2021+ VDF format (escaped backslashes, nested apps blocks) and
// the older numbered-key format, plus discovery of music albums and in-game soundtracks from a
// synthetic Steam tree (SFX-only folders and stray wavs must not be mistaken for soundtracks).
void TestSteam(const std::wstring& scratch) {
    // Two libraries as Steam 2021+ writes them, with escaped backslashes and a nested apps block.
    const std::string vdf =
        "\"libraryfolders\"\n"
        "{\n"
        "\t\"contentstatsid\"\t\t\"-1234567890\"\n"
        "\t\"0\"\n"
        "\t{\n"
        "\t\t\"path\"\t\t\"C:\\\\Program Files (x86)\\\\Steam\"\n"
        "\t\t\"label\"\t\t\"\"\n"
        "\t\t\"apps\"\n"
        "\t\t{\n"
        "\t\t\t\"228980\"\t\t\"123456\"\n"
        "\t\t}\n"
        "\t}\n"
        "\t\"1\"\n"
        "\t{\n"
        "// extra library on another drive\n"
        "\t\t\"path\"\t\t\"D:\\\\SteamLibrary\"\n"
        "\t\t\"apps\"\n"
        "\t\t{\n"
        "\t\t\t\"570\"\t\t\"987\"\n"
        "\t\t}\n"
        "\t}\n"
        "}\n";
    auto libs = ParseSteamLibraryFolders(vdf);
    testlog::Batch batch;
    batch.Next(L"Steam's 2021+ and older library-folder formats both read their libraries, and empty or "
               L"truncated input does not crash");
    Check(libs.size() == 2 && libs[0] == L"C:\\Program Files (x86)\\Steam" && libs[1] == L"D:\\SteamLibrary",
          L"STEAM vdf two libraries: %zu, first %ls, second %ls", libs.size(), libs.empty() ? L"" : libs[0].c_str(),
          libs.size() < 2 ? L"" : libs[1].c_str());

    // Old format: numbered keys hold the folder directly; forward slashes and a trailing slash.
    auto old = ParseSteamLibraryFolders("\"LibraryFolders\"\n{\n\t\"TimeNextStatsReport\"\t\"1\"\n"
                                        "\t\"1\"\t\t\"E:\\\\Old Games\"\n\t\"2\"\t\t\"F:/Games/Steam/\"\n}\n");
    Check(old.size() == 2 && old[0] == L"E:\\Old Games" && old[1] == L"F:\\Games\\Steam",
          L"STEAM vdf old format: %zu folders", old.size());
    Check(ParseSteamLibraryFolders("\"libraryfolders\" { }").empty() && ParseSteamLibraryFolders("").empty() &&
              ParseSteamLibraryFolders("\"libraryfolders\" { \"0\" { \"path\" \"C:\\\\Dangling").size() == 1,
          L"STEAM vdf empty and truncated input do not crash");

    auto music = FindSteamMusicFolders();
    if (g_log) fwprintf(g_log, L"STEAM music folders on this machine: %zu%ls\n", music.size(),
                        music.empty() ? L"" : (L" (" + music[0] + L")").c_str());

    // A synthetic Steam library: an album under steamapps\music, a soundtrack inside a game, an
    // SFX-only folder and a stray wav that must not be mistaken for a soundtrack.
    const std::wstring lib = scratch + L"\\steamtest_lib";
    DeleteTestFolder(lib);
    const std::wstring album = lib + L"\\steamapps\\music\\Album Name";
    const std::wstring game = lib + L"\\steamapps\\common\\Some Game";
    const std::wstring ost = game + L"\\Soundtrack";
    const std::wstring sfx = game + L"\\sfx";
    const std::wstring stray = lib + L"\\steamapps\\common\\Lone Game";
    const std::wstring placeholders = game + L"\\loadedsoundeffects";
    const std::wstring emptyMusic = stray + L"\\Music";
    std::error_code ec;
    for (const std::wstring& d : {album, ost, sfx, stray, placeholders, emptyMusic})
        std::filesystem::create_directories(d, ec);
    const std::string wav = MakeSineWav(0.01, 0.5);
    bool wrote = WriteTestBytes(album + L"\\song.wav", wav) && WriteTestBytes(ost + L"\\track1.wav", wav) &&
                 WriteTestBytes(ost + L"\\broken.mp3", "") && WriteTestBytes(sfx + L"\\hit.wav", wav) &&
                 WriteTestBytes(stray + L"\\stray.wav", wav) && WriteTestBytes(emptyMusic + L"\\empty.wav", "");
    for (int i = 0; i < 8; ++i) {
        wrote &= WriteTestBytes(placeholders + L"\\" + std::to_wstring(i) + L".wav", "");
        wrote &= WriteTestBytes(sfx + L"\\" + std::to_wstring(i) + L".wav", wav);
    }
    if (!Ready(wrote, L"STEAM fixture written")) {
        DeleteTestFolder(lib);
        return;
    }
    batch.Next(L"a synthetic Steam library finds the music album root and an in-game soundtrack while effects, "
               L"empty placeholders and stray wavs stay out, cancellation discards everything, and the soundtrack scans");

    auto found = FindSteamMusicFoldersIn(lib);
    auto has = [&](const std::wstring& p) {
        return std::any_of(found.begin(), found.end(), [&](const std::wstring& f) { return _wcsicmp(f.c_str(), p.c_str()) == 0; });
    };
    Check(has(lib + L"\\steamapps\\music") && has(lib + L"\\steamapps\\common\\Some Game\\Soundtrack"),
          L"STEAM library: found music album root and in-game soundtrack (%zu roots)", found.size());
    Check(!has(sfx) && !has(stray) && !has(placeholders) && !has(emptyMusic) && found.size() == 2,
          L"STEAM library: effects, empty placeholders and stray wavs are not soundtracks (%zu roots)", found.size());
    std::atomic<bool> cancelled{true};
    Check(FindSteamMusicFoldersIn(lib, &cancelled).empty(), L"STEAM cancellation discards all discovered roots");
    std::vector<ScanFile> files;
    bool imported = ListAudioFiles(ost, files) && files.size() == 1;
    int read = 0, rejected = 0;
    for (const auto& file : files) {
        TagData tags;
        if (ReadTags(file.path, tags)) ++read; else ++rejected;
    }
    Check(imported && read == 1 && rejected == 0,
          L"STEAM a soundtrack scan keeps valid audio and leaves out its empty placeholder");
    const std::wstring copy = ost + L"\\track1-copy.wav";
    if (!Ready(WriteTestBytes(copy, wav), L"the copied soundtrack file is prepared")) return;
    Library library;
    library[library.Add(copy)].flags |= Track::kInLibrary;
    TrackId original = library.Add(album + L"\\song.wav");
    library[original].flags |= Track::kInLibrary;
    library[original].playCount = 7;
    SteamImport plan;
    plan.folders = {ost};
    Check(PrepareSteamImport(plan, SnapshotSteamSongs(library)) && plan.oneCopy.size() == 2 &&
              plan.eachFormat.size() == 2 && SkipSteamDuplicates(library, plan, false) == 2 &&
              library[original].InLibrary() && library[original].playCount == 7 &&
              !library[library.Find(copy)].InLibrary() && ReadTestBytes(copy) == wav,
          L"STEAM copied files stay out of the Library while the existing song, history and files survive");
    plan.eachFormat.clear();
    SkipSteamDuplicates(library, plan, true);
    Check(library.Contains(library.Find(copy)) && library[library.Find(copy)].InLibrary(),
          L"STEAM choosing additional formats later can include an automatically skipped copy again");
    DeleteTestFolder(lib);
}

// ID3 CHAP frames, as yt-dlp writes them into an MP3.
void TestChapters(const std::wstring& scratch) {
    auto be32 = [](uint32_t v) {
        return std::string{(char)(v >> 24), (char)(v >> 16), (char)(v >> 8), (char)v};
    };
    auto chap = [&](const char* element, uint32_t startMs, uint32_t endMs, const std::string& title) {
        std::string tit2 = std::string("\0", 1) + title;
        std::string body = std::string(element) + std::string("\0", 1) + be32(startMs) + be32(endMs) + be32(0xFFFFFFFF) +
                           be32(0xFFFFFFFF) + "TIT2" + be32((uint32_t)tit2.size()) + std::string("\0\0", 2) + tit2;
        return "CHAP" + be32((uint32_t)body.size()) + std::string("\0\0", 2) + body;
    };
    // Out of order on purpose: chapters come back sorted by start.
    std::string frames = chap("ch1", 95500, 300000, "Second part") + chap("ch0", 0, 95500, "Intro");
    uint32_t n = (uint32_t)frames.size();
    std::string tag = std::string("ID3\x03\0\0", 6) + std::string{(char)((n >> 21) & 0x7F), (char)((n >> 14) & 0x7F),
                                                                  (char)((n >> 7) & 0x7F), (char)(n & 0x7F)} + frames;
    std::wstring path = scratch + L"\\chapters.mp3";
    WriteTestBytes(path, tag + std::string(4096, '\0'));
    std::vector<Chapter> chapters;
    bool read = ReadChapters(path, chapters);
    Check(read && chapters.size() == 2 && chapters[0].title == "Intro" && chapters[0].start == 0 &&
              chapters[1].title == "Second part" && std::abs(chapters[1].start - 95.5) < 1e-9 &&
              ChapterTextAt(chapters, 0) == L"Chapter 1/2: Intro" &&
              ChapterTextAt(chapters, 95.5) == L"Chapter 2/2: Second part" && ChapterStartAfter(chapters, 0) == 95.5,
          L"CHAPTERS ID3 CHAP frames: %zu read, sorted, with titles", chapters.size());
    DeleteFileW(path.c_str());
}

// LRC lyrics: timestamps, [ar:]/[offset:] metadata, a line with several stamps, and that untimed
// "plain" lyrics are passed through untouched.
void TestLyrics() {
    testlog::Batch batch;
    batch.Next(L"LRC stamps, metadata and offsets parse into timed lines, and plain lyrics pass through untouched");
    bool timed = false;
    auto lines = app::ParseLyrics(L"[ar:Toby Fox]\r\n[offset:+500]\r\n[00:12.50]Second\r\n[00:01.00][01:02:50]First and last\r\n"
                                  L"[00:30.00]  Third\r\n", timed);
    Check(timed && lines.size() == 4 && lines[0].second == L"First and last" && std::abs(lines[0].first - 0.5) < 1e-9 &&
              lines[1].second == L"Second" && std::abs(lines[1].first - 12.0) < 1e-9 && lines[2].second == L"Third" &&
              std::abs(lines[3].first - 62.0) < 1e-9,
          L"LYRICS LRC: stamps, metadata, offset and repeated lines");
    lines = app::ParseLyrics(L"[Chorus]\nLa la la\n\n", timed);
    Check(!timed && lines.size() == 2 && lines[0].second == L"[Chorus]" && lines[0].first < 0,
          L"LYRICS plain lyrics stay as they are");
}

void TestCrashDump(const std::wstring& scratch) {
    testlog::Batch batch;
    batch.Next(L"a crash dump is written and pruning keeps the newest five, and the newest dump is reported once");
    // No real crash: the writer runs on this thread. Seven dumps must leave the newest five.
    const std::wstring dir = scratch + L"\\crashes";
    bool written = true;
    std::wstring last;
    for (int i = 0; i < 7; ++i) {
        std::wstring path;
        written = WriteCrashDumpNow(dir, nullptr, &path) && written;
        last = path;
    }
    int count = 0;
    WIN32_FIND_DATAW fd;
    if (HANDLE h = FindFirstFileW((dir + L"\\cordinal-*.dmp").c_str(), &fd); h != INVALID_HANDLE_VALUE) {
        do ++count;
        while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    Check(written && GetFileAttributesW(last.c_str()) != INVALID_FILE_ATTRIBUTES && count == 5,
          L"CRASH dump written %ls, pruning keeps %d of the newest dumps", last.c_str(), count);

    // Install the handlers, then the UI's "closed unexpectedly" check reports the newest dump once.
    InstallCrashHandler(dir);
    std::wstring reported;
    bool first = TakeNewCrashDump(reported);
    bool again = TakeNewCrashDump(reported);
    Check(first && !again && reported == last, L"CRASH new dump reported once (first=%d again=%d)", first, again);
}

// ListenBrainz: JSON escaping and UTF-8 handling, rejection of artistless listens, the encrypted
// token, the on-disk queue surviving a restart, and that nothing is sent while it is off. Sending is
// disabled for the test, so no request ever leaves the machine.
void TestListenBrainz(const std::wstring& scratch) {
    using namespace listenbrainz;
    const std::string info = "\"media_player\":\"cordinal\",\"submission_client\":\"cordinal\","
                             "\"submission_client_version\":\"0.1.12\"";

    Listen a;
    a.listenedAt = 1700000000;
    a.artist = "A \"quoted\" \\ band";
    a.title = "Line\nBreak";
    a.durationMs = 215000;
    a.trackNumber = 3;
    std::string want = "{\"listened_at\":1700000000,\"track_metadata\":{\"artist_name\":\"A \\\"quoted\\\" \\\\ band\","
                       "\"track_name\":\"Line\\u000aBreak\",\"additional_info\":{\"duration_ms\":215000,"
                       "\"tracknumber\":3," + info + "}}}";
    std::string got = ListenJson(a, true);
    testlog::Batch batch;
    batch.Next(L"ListenBrainz JSON escapes quotes, backslashes and newlines, passes UTF-8 through, "
               L"replaces invalid bytes with U+FFFD, skips artistless listens and omits zero fields");
    Check(got == want, L"LISTENBRAINZ json escapes quotes, backslash and newline: %ls", got == want ? L"ok" : L"MISMATCH");

    Listen e;  // UTF-8 passes through; no timestamp, release present, zero fields omitted
    e.artist = "Band \xF0\x9F\x8E\xB5";
    e.title = "Song";
    e.release = "Album";
    std::string wantE = "{\"track_metadata\":{\"artist_name\":\"Band \xF0\x9F\x8E\xB5\",\"track_name\":\"Song\","
                        "\"release_name\":\"Album\",\"additional_info\":{" + info + "}}}";
    Check(ListenJson(e, false) == wantE, L"LISTENBRAINZ UTF-8 passes through and playing-now has no timestamp");
    Listen bad = e;
    bad.title = "Caf\xE9 \xFF";  // Latin-1 bytes, not UTF-8
    std::string badJson = ListenJson(bad, false);
    Check(badJson.find("Caf\xEF\xBF\xBD \xEF\xBF\xBD") != std::string::npos, L"LISTENBRAINZ invalid UTF-8 becomes U+FFFD");

    Listen m;
    m.listenedAt = 5;
    m.title = "B";
    Listen n = m;
    n.artist = "A";
    Check(ListenJson(m, true).empty() && ListenJson(n, true) ==
              "{\"listened_at\":5,\"track_metadata\":{\"artist_name\":\"A\",\"track_name\":\"B\","
              "\"additional_info\":{" + info + "}}}",
          L"LISTENBRAINZ listens without an artist are skipped; zero fields are omitted");

    // Queue round trip and token encryption, with sending disabled so no request leaves the machine.
    const std::wstring dir = scratch + L"\\listenbrainz";
    CreateDirectoryW(dir.c_str(), nullptr);
    DeleteFileW((dir + L"\\listenbrainz-queue.txt").c_str());
    DeleteFileW((dir + L"\\listenbrainz.token").c_str());
    SetSendingDisabledForTest(true);
    Start(dir, nullptr, 0);
    const std::wstring fakeToken = L"fake-token-0123456789";
    SetToken(fakeToken);
    std::string tokenBytes = ReadTestBytes(dir + L"\\listenbrainz.token");
    batch.Next(L"the ListenBrainz token is stored encrypted, the queue survives a restart, and nothing is queued "
               L"while sending is off");
    Check(Enabled() && !tokenBytes.empty() && tokenBytes.find("fake-token") == std::string::npos,
          L"LISTENBRAINZ token stored encrypted (%zu bytes on disk)", tokenBytes.size());

    Listen s;
    s.artist = "Artist";
    s.title = "Track";
    for (uint32_t t = 1; t <= 3; ++t) {
        s.listenedAt = 1700000000 + t;
        Submit(s);
    }
    Listen noTime = s;
    noTime.listenedAt = 0;
    Submit(noTime);  // no timestamp: refused
    PlayingNow(s);   // never queued
    Check(GetStatus().queued == 3, L"LISTENBRAINZ queue holds 3 listens (queued=%zu)", GetStatus().queued);
    Stop();

    Start(dir, nullptr, 0);  // the queue survives a restart
    Check(GetStatus().queued == 3 && Enabled(), L"LISTENBRAINZ queue and token reload from disk (queued=%zu)",
          GetStatus().queued);
    SetToken(L"");
    Submit(s);  // off: nothing is queued
    Check(!Enabled() && GetStatus().queued == 3 && GetFileAttributesW((dir + L"\\listenbrainz.token").c_str()) ==
                            INVALID_FILE_ATTRIBUTES,
          L"LISTENBRAINZ token cleared, sending off, nothing queued while off");
    Stop();
    SetSendingDisabledForTest(false);
}
