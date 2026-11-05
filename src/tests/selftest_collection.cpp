// Export collection and Import collection (collection.h): the ZIP writer read back by the
// archive reader (ZIP64 too) and cut back when a file can't be taken whole; an export's layout,
// companions, playlists and skipped files; an import's byte-exact unpacking, kept times, re-import,
// name collisions and playlists; the archives import refuses; damage; cancelling; and that the
// scanner never lists an export as a pack of music.
#include "selftest_common.h"

#include "archive.h"
#include "collection.h"
#include "streamfile.h"

namespace {

constexpr uint64_t kEpoch = 116444736000000000ull;

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

// The temp names a run uses: the archive's ".part" and an unpacked file's ".cordinal-part".
bool NoLeftovers(const std::wstring& dir) {
    std::error_code ec;
    for (const auto& e : std::filesystem::recursive_directory_iterator(dir, ec)) {
        const std::wstring name = e.path().filename().wstring();
        if (name.ends_with(L".part") || name.ends_with(L".cordinal-part")) return false;
    }
    return true;
}

std::vector<ZipEntry> Entries(const std::wstring& zip) {
    FileSource src(zip);
    std::vector<ZipEntry> out;
    if (src.Valid()) ReadZipDirectory(src, out);
    return out;
}

const ZipEntry* Find(const std::vector<ZipEntry>& entries, std::string_view name) {
    for (const auto& e : entries)
        if (e.name == name) return &e;
    return nullptr;
}

std::string Noise(size_t n, uint32_t seed) {
    std::string s(n, '\0');
    for (char& c : s) {
        seed = seed * 1664525u + 1013904223u;
        c = (char)(seed >> 24);
    }
    return s;
}

void SetTime(const std::wstring& path, uint32_t unixTime) {
    HANDLE h = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    const uint64_t ft = kEpoch + (uint64_t)unixTime * 10000000ull;
    FILETIME t{(DWORD)ft, (DWORD)(ft >> 32)};
    SetFileTime(h, nullptr, nullptr, &t);
    CloseHandle(h);
}

uint32_t TimeOf(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return 0;
    const uint64_t ft = (uint64_t)a.ftLastWriteTime.dwHighDateTime << 32 | a.ftLastWriteTime.dwLowDateTime;
    return (uint32_t)((ft - kEpoch) / 10000000ull);
}

// A ZIP holding `files`, and a collection manifest when one is given.
std::wstring MakeZip(const std::wstring& path, const std::string& manifest, const std::vector<std::pair<std::string, std::string>>& files) {
    collection::ZipWriter w(path);
    for (const auto& [name, bytes] : files) w.Add(name, bytes, 1600000000);
    if (!manifest.empty()) w.Add(kCollectionManifest, manifest, 1600000000);
    w.Finish();
    return path;
}

// The lines of a playlist that name songs.
std::vector<std::string> Songs(const std::string& text) {
    std::vector<std::string> out;
    for (size_t start = 0; start < text.size();) {
        size_t end = std::min(text.find('\n', start), text.size());
        std::string line = text.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line[0] != '#' && line[0] != '\xEF') out.push_back(line);
    }
    return out;
}

}  // namespace

void TestCollection(const std::wstring& scratch) {
    testlog::Batch batch;
    const std::wstring root = scratch;
    std::filesystem::remove_all(root);
    const std::wstring music = root + L"\\Music", loose = root + L"\\Loose", out = root + L"\\out", in = root + L"\\in";
    for (const auto& d : {music + L"\\Artist\\Album", music + L"\\Artist\\Album 2", music + L"\\Artist\\Live",
                          music + L"\\Artist\\Packs", loose, out, in})
        SHCreateDirectoryExW(nullptr, d.c_str(), nullptr);
    std::atomic<bool> cancel{false};
    const std::string big = Noise(2500000, 7);  // more than two of the writer's copy chunks
    WriteTestBytes(root + L"\\big.bin", big);

    batch.Next(L"COLLECTION ZIP writer round trip, plain and ZIP64");
    for (bool zip64 : {false, true}) {
        const std::wstring zip = root + (zip64 ? L"\\w64.zip" : L"\\w32.zip");
        bool wrote = false;
        {
            collection::ZipWriter w(zip, zip64);
            std::wstring error;
            wrote = w.Valid() && w.Add("a/\xC3\x9C.txt", "hello", 1600000000) &&
                    w.AddFile("b/big.spc", root + L"\\big.bin", {}, error) && w.Add("empty.txt", "", 1600000000) && w.Finish();
        }
        const auto entries = Entries(zip);
        const ZipEntry* u = Find(entries, "a/\xC3\x9C.txt");
        const ZipEntry* b = Find(entries, "b/big.spc");
        const auto text = ReadArchiveMember(zip + L"\\a\\\x00DC.txt");
        const auto member = ReadArchiveMember(zip + L"\\b\\big.spc");
        std::vector<std::wstring> listed;
        Check(wrote && entries.size() == 3 && u && (u->flags & 0x800) && u->unixTime == 1600000000 && b && b->method == 0 &&
                  b->size == big.size() && text && *text == "hello" && member && *member == big && ListArchive(zip, listed) &&
                  listed.size() == 3,
              L"COLLECTION ZIP writer%ls: every entry reads back exactly, with its UTF-8 name and time",
              zip64 ? L" with ZIP64 fields" : L"");
    }

    batch.Next(L"COLLECTION a file the writer can't take whole leaves no trace in the archive");
    {
        const std::wstring zip = root + L"\\cut.zip";
        bool ok = false;
        {
            collection::ZipWriter w(zip);
            std::wstring error;
            const bool first = w.Add("keep.txt", "kept", 1600000000);
            const bool stopped = !w.AddFile("big.spc", root + L"\\big.bin", [](uint64_t) { return false; }, error);
            const bool missing = !w.AddFile("gone.spc", root + L"\\nothing-here.bin", {}, error);
            ok = first && stopped && missing && w.Valid() && w.Add("after.txt", "after", 1600000000) && w.Finish();
        }
        const auto entries = Entries(zip);
        const auto after = ReadArchiveMember(zip + L"\\after.txt");
        Check(ok && entries.size() == 2 && Find(entries, "keep.txt") && after && *after == "after" &&
                  std::filesystem::file_size(zip) < 1000,
              L"COLLECTION a stopped or unreadable file is cut back out and the archive goes on");
    }

    // Two albums (cover, and a file that isn't the collection's), a CUE image with two songs, a pack
    // of game music, a loose song only a playlist holds, and a library song whose file is gone.
    const std::wstring one = music + L"\\Artist\\Album\\01 One.flac", two = music + L"\\Artist\\Album 2\\Zwei \x00DC.mp3",
                       live = music + L"\\Artist\\Live\\live.flac", pack = music + L"\\Artist\\Packs\\Pack.zip",
                       song = loose + L"\\Loose Song.ogg", gone = music + L"\\Artist\\Album\\gone.flac";
    WriteTestBytes(one, Noise(1500000, 1));
    WriteTestBytes(music + L"\\Artist\\Album\\cover.jpg", Noise(5000, 2));
    WriteTestBytes(music + L"\\Artist\\Album\\notes.txt", "not part of the collection");
    WriteTestBytes(two, Noise(40000, 3));
    WriteTestBytes(live, Noise(80000, 4));
    WriteTestBytes(music + L"\\Artist\\Live\\live.cue",
                   "FILE \"live.flac\" WAVE\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n  TRACK 02 AUDIO\n    INDEX 01 00:01:00\n");
    MakeZip(pack, "", {{"song.spc", Noise(3000, 5)}});
    WriteTestBytes(song, Noise(20000, 6));
    SetTime(one, 1577934245);  // 2020-01-02 03:04:05 UTC
    const std::string packBefore = ReadTestBytes(pack);
    Library lib;
    auto add = [&](const std::wstring& path, bool inLibrary) {
        const TrackId id = lib.Add(path);
        if (inLibrary) lib[id].flags |= Track::kInLibrary;
        return id;
    };
    const TrackId a = add(one, true);
    add(two, true);
    add(live + L"|1", true);
    const TrackId l2 = add(live + L"|2", true), p = add(pack + L"\\song.spc", true), s = add(song, false);
    add(gone, true);
    const std::vector<Playlist> playlists{Playlist{L"Library", {}, PlaylistKind::Library},
                                          Playlist{L"Road: trip", {s, l2, a, p, a}, PlaylistKind::Static},
                                          Playlist{L"Rules", {}, PlaylistKind::Auto}};

    batch.Next(L"COLLECTION export: layout, companions, playlists and what was skipped");
    const auto source = collection::CaptureExport(lib, playlists, {music}, false);
    const std::wstring zip = out + L"\\collection.zip";
    const auto r = collection::Export(source, zip, cancel, {});
    const auto entries = Entries(zip);
    const bool layout = Find(entries, "Music/Artist/Album/01 One.flac") && Find(entries, "Music/Artist/Album/cover.jpg") &&
                        !Find(entries, "Music/Artist/Album/notes.txt") && Find(entries, ToUtf8(L"Music/Artist/Album 2/Zwei \x00DC.mp3")) &&
                        Find(entries, "Music/Artist/Live/live.flac") && Find(entries, "Music/Artist/Live/live.cue") &&
                        Find(entries, "Music/Artist/Packs/Pack.zip") && Find(entries, "Other/Loose/Loose Song.ogg") &&
                        Find(entries, "Playlists/Road_ trip.m3u8") && Find(entries, "README.txt") && Find(entries, kCollectionManifest);
    Check(r.ok && r.songs == 6 && r.playlists == 1 && r.skipped.size() == 1 && r.skipped[0].first == gone && layout &&
              entries.size() == 10 && NoLeftovers(out),
          L"COLLECTION export holds each library folder's files as arranged, covers and cue sheets, the playlist's other song, "
          L"and reports the missing file (%zu songs, %zu entries, %zu skipped: %ls)",
          r.songs, entries.size(), r.skipped.size(), r.error.c_str());
    const auto playlist = ReadArchiveMember(zip + L"\\Playlists\\Road_ trip.m3u8");
    const auto manifest = ReadArchiveMember(zip + L"\\" + FromUtf8(kCollectionManifest));
    const std::vector<std::string> lines{"../Other/Loose/Loose Song.ogg", "../Music/Artist/Live/live.flac|2",
                                         "../Music/Artist/Album/01 One.flac", "../Music/Artist/Packs/Pack.zip/song.spc",
                                         "../Music/Artist/Album/01 One.flac"};
    Check(playlist && Songs(*playlist) == lines && manifest && manifest->starts_with("cordinal-collection 1\n") &&
              manifest->find("\nsongs 6\n") != std::string::npos,
          L"COLLECTION the playlist names each song relative to its folder, a CUE song and a pack's song included");
    std::vector<std::wstring> members, packed;
    Check(ListArchive(zip, members) && members.empty() && ListArchive(pack, packed) && packed.size() == 1,
          L"COLLECTION the scanner lists a pack's music but never an export's (%zu listed)", members.size());

    batch.Next(L"COLLECTION select what to export: artist > album > song, and only the ticked songs and playlists");
    {
        Library tagged;
        auto put = [&](const std::wstring& path, const char* artist, const char* albumArtist, const char* album,
                       const char* title, uint16_t trackNo, bool inLibrary = true) {
            const TrackId id = tagged.Add(path);
            Track& t = tagged[id];
            if (inLibrary) t.flags |= Track::kInLibrary;
            t.artist = *artist ? tagged.Intern(artist) : 0;
            t.albumArtist = *albumArtist ? tagged.Intern(albumArtist) : 0;
            t.album = *album ? tagged.Intern(album) : 0;
            t.title = *title ? tagged.Intern(title) : 0;
            t.trackNo = trackNo;
            return id;
        };
        put(two, "zed", "", "Second", "B side", 2);
        const TrackId first = put(one, "Guest", "Zed", "Second", "A side", 1);  // album artist wins, ignoring case
        put(live + L"|1", "alpha", "", "", "", 0);
        put(pack + L"\\song.spc", "", "", "", "Chip", 0);
        const TrackId loner = put(song, "Outside", "", "", "Loose", 0, false);  // not in the library tab
        const std::vector<Playlist> lists{Playlist{L"Mix", {loner, first}, PlaylistKind::Static},
                                          Playlist{L"Other mix", {first}, PlaylistKind::Static},
                                          Playlist{L"Rules", {}, PlaylistKind::Auto}};
        const auto c = collection::ListChoices(tagged, lists, false);
        const bool grouped = c.artists.size() == 3 && c.artists[0].name == L"alpha" && c.artists[1].name == L"zed" &&
                             c.artists[2].name.empty() && c.artists[1].songs == 2 && c.artists[1].albums.size() == 1 &&
                             c.artists[1].albums[0].songs.size() == 2 && c.artists[1].albums[0].songs[0].title == L"A side" &&
                             c.artists[1].albums[0].songs[0].path == one && c.artists[2].albums[0].songs[0].title == L"Chip" &&
                             c.playlists.size() == 2 && c.playlists[0].name == L"Mix" && c.playlists[0].songs == 2;
        Check(grouped, L"COLLECTION the choice lists artists by name (the unnamed last), their albums and songs in track order, "
                       L"and the static playlists (%zu artists, %zu playlists)",
              c.artists.size(), c.playlists.size());

        collection::ExportSelection pick{{two}, {L"Mix"}};
        const auto chosen = collection::CaptureExport(tagged, lists, {music}, false, &pick);
        const bool songs = chosen.tracks.size() == 3 && chosen.tracks[0] == two &&
                           std::count(chosen.tracks.begin(), chosen.tracks.end(), song) == 1 &&
                           std::count(chosen.tracks.begin(), chosen.tracks.end(), one) == 1;
        Check(songs && chosen.playlists.size() == 1 && chosen.playlists[0].name == L"Mix" && chosen.playlists[0].paths.size() == 2,
              L"COLLECTION a selection keeps the ticked song and the ticked playlist with all its songs, nothing else (%zu songs)",
              chosen.tracks.size());
        collection::ExportSelection nothing;
        Check(collection::CaptureExport(tagged, lists, {music}, false, &nothing).tracks.empty() &&
                  collection::CaptureExport(tagged, lists, {music}, false).tracks.size() == 5,
              L"COLLECTION an empty selection exports nothing, and no selection exports everything");
        const std::wstring picked = out + L"\\picked.zip";
        const auto pr = collection::Export(chosen, picked, cancel, {});
        const auto pe = Entries(picked);
        Check(pr.ok && pr.songs == 3 && pr.playlists == 1 && Find(pe, ToUtf8(L"Music/Artist/Album 2/Zwei \x00DC.mp3")) &&
                  !Find(pe, "Music/Artist/Live/live.flac") && !Find(pe, "Music/Artist/Packs/Pack.zip") && Find(pe, "Playlists/Mix.m3u8") &&
                  !Find(pe, "Playlists/Other mix.m3u8"),
              L"COLLECTION the selected archive holds only what was ticked (%zu songs, %zu entries)", pr.songs, pe.size());
        DeleteFileW(picked.c_str());
    }

    batch.Next(L"COLLECTION import unpacks byte for byte, keeps times and points the playlists at the new files");
    collection::ImportPlan plan;
    std::wstring error;
    const bool read = collection::ReadCollection(zip, plan, error);
    const auto ir = read ? collection::Import(plan, in, cancel, {}) : collection::ImportResult{};
    const std::wstring got = in + L"\\Music\\Artist\\Album\\01 One.flac", beside = in + L"\\Music\\Artist\\Album\\01 One (2).flac";
    const bool exact = ReadTestBytes(got) == ReadTestBytes(one) &&
                       ReadTestBytes(in + L"\\Music\\Artist\\Album 2\\Zwei \x00DC.mp3") == ReadTestBytes(two) &&
                       ReadTestBytes(in + L"\\Other\\Loose\\Loose Song.ogg") == ReadTestBytes(song) &&
                       ReadTestBytes(in + L"\\Music\\Artist\\Packs\\Pack.zip") == packBefore &&
                       ReadTestBytes(in + L"\\Music\\Artist\\Album\\cover.jpg") == ReadTestBytes(music + L"\\Artist\\Album\\cover.jpg");
    Check(read && plan.songs == 6 && plan.playlists == 1 && ir.error.empty() && !ir.cancelled && ir.written == 7 && ir.same == 0 &&
              ir.failed.empty() && exact && TimeOf(got) == 1577934245 && ir.folders.size() == 2 &&
              ir.folders[0] == in + L"\\Music" && ir.folders[1] == in + L"\\Other" && NoLeftovers(in) &&
              !Exists(in + L"\\README.txt") && !Exists(in + L"\\" + FromUtf8(kCollectionManifest)),
          L"COLLECTION import writes %zu files exactly, with their times, and names the folders to watch (%ls%ls)",
          ir.written, error.c_str(), ir.error.c_str());
    const std::vector<std::wstring> want{in + L"\\Other\\Loose\\Loose Song.ogg", in + L"\\Music\\Artist\\Live\\live.flac|2", got,
                                         in + L"\\Music\\Artist\\Packs\\Pack.zip\\song.spc", got};
    const std::string written = ReadTestBytes(in + L"\\Playlists\\Road_ trip.m3u8");
    Check(ir.playlists.size() == 1 && ir.playlists[0].name == L"Road_ trip" && ir.playlists[0].paths == want && Songs(written) == lines,
          L"COLLECTION import resolves the playlist to the unpacked files and writes it beside them");

    batch.Next(L"COLLECTION importing again finds every file already there");
    const auto again = collection::Import(plan, in, cancel, {});
    Check(again.written == 0 && again.same == 7 && again.renamed == 0 && again.failed.empty() && !Exists(beside) &&
              !Exists(in + L"\\Playlists\\Road_ trip (2).m3u8") && again.playlists.size() == 1,
          L"COLLECTION a second import writes nothing and makes no copies (%zu written, %zu same)", again.written, again.same);

    batch.Next(L"COLLECTION a different file with the same name keeps it; the collection's copy goes beside it");
    WriteTestBytes(got, "the user's own, different file");
    const auto collide = collection::Import(plan, in, cancel, {});
    const auto third = collection::Import(plan, in, cancel, {});
    Check(collide.written == 1 && collide.renamed == 1 && collide.same == 6 && ReadTestBytes(got) == "the user's own, different file" &&
              ReadTestBytes(beside) == ReadTestBytes(one) && collide.playlists.size() == 1 && collide.playlists[0].paths[2] == beside &&
              third.written == 0 && third.same == 7 && !Exists(in + L"\\Music\\Artist\\Album\\01 One (3).flac"),
          L"COLLECTION a name already taken by other bytes is never replaced, and the copy beside it is found again later");

    batch.Next(L"COLLECTION import refuses archives cordinal didn't export, or couldn't unpack safely");
    collection::ImportPlan bad;
    std::wstring e1, e2, e3, e4, e5;
    const bool foreign = !collection::ReadCollection(MakeZip(root + L"\\foreign.zip", "", {{"Music/a.mp3", "x"}}), bad, e1) &&
                         e1.find(L"isn't a collection") != std::wstring::npos;
    const bool newer =
        !collection::ReadCollection(MakeZip(root + L"\\newer.zip", "cordinal-collection 2\n", {{"Music/a.mp3", "x"}}), bad, e2) &&
        e2.find(L"newer") != std::wstring::npos;
    const bool climbs =
        !collection::ReadCollection(MakeZip(root + L"\\climb.zip", "cordinal-collection 1\n", {{"Music/../../evil.mp3", "x"}}), bad, e3) &&
        e3.find(L"outside") != std::wstring::npos;
    // Zipped again by another program: an entry compressed.
    const std::wstring repacked = MakeZip(root + L"\\packed.zip", "cordinal-collection 1\n", {{"Music/a.mp3", "x"}});
    std::string bytes = ReadTestBytes(repacked);
    if (const size_t cd = bytes.find("PK\x01\x02"); cd != std::string::npos) bytes[cd + 10] = 8;
    WriteTestBytes(repacked, bytes);
    const bool compressed = !collection::ReadCollection(repacked, bad, e4) && e4.find(L"compressed") != std::wstring::npos;
    const bool notZip = !collection::ReadCollection(one, bad, e5) && !e5.empty();
    Check(foreign && newer && climbs && compressed && notZip,
          L"COLLECTION refused: another program's ZIP, a newer format, a name that climbs out, a recompressed entry, a non-archive");

    batch.Next(L"COLLECTION a damaged file is reported and never gets its real name");
    const std::wstring damaged = out + L"\\damaged.zip", fresh = root + L"\\in2";
    std::string zipBytes = ReadTestBytes(zip);
    {
        FileSource src(zip);
        const ZipEntry* e = Find(entries, ToUtf8(L"Music/Artist/Album 2/Zwei \x00DC.mp3"));
        const uint64_t at = e && src.Valid() ? ZipDataStart(src, *e) : 0;
        if (at) zipBytes[(size_t)at + 100] ^= 0x5A;
    }
    WriteTestBytes(damaged, zipBytes);
    collection::ImportPlan dp;
    const bool readDamaged = collection::ReadCollection(damaged, dp, error);
    const auto dr = readDamaged ? collection::Import(dp, fresh, cancel, {}) : collection::ImportResult{};
    Check(readDamaged && dr.failed.size() == 1 && dr.written == 6 && !Exists(fresh + L"\\Music\\Artist\\Album 2\\Zwei \x00DC.mp3") &&
              NoLeftovers(fresh),
          L"COLLECTION a file whose check value doesn't match is reported and left out (%zu failed, %zu written)", dr.failed.size(),
          dr.written);

    batch.Next(L"COLLECTION cancelling saves no archive and leaves no half-written file");
    auto stop = [&](const collection::Progress& progress) {
        if (progress.bytes) cancel = true;
    };
    const auto cr = collection::Export(source, out + L"\\cancelled.zip", cancel, stop);
    cancel = false;
    const std::wstring cut = root + L"\\in3";
    const auto ci = collection::Import(plan, cut, cancel, stop);
    cancel = false;
    Check(cr.cancelled && !cr.ok && !Exists(out + L"\\cancelled.zip") && !Exists(out + L"\\cancelled.zip.part") && ci.cancelled &&
              ci.written == 0 && ci.playlists.empty() && NoLeftovers(root),
          L"COLLECTION a cancelled export or import leaves nothing under a real name");

    batch.Next(L"COLLECTION export never writes over one of the collection's own files");
    const auto over = collection::Export(source, pack, cancel, {});
    const auto none = collection::Export(collection::CaptureExport(Library(), {}, {music}, false), out + L"\\empty.zip", cancel, {});
    Check(!over.ok && over.error.find(L"part of the collection") != std::wstring::npos && ReadTestBytes(pack) == packBefore &&
              !none.ok && !Exists(out + L"\\empty.zip"),
          L"COLLECTION an export over one of its own files, or of an empty library, is refused");
    if (!GetEnvironmentVariableW(L"CORDINAL_KEEP_TEST_FILES", nullptr, 0)) std::filesystem::remove_all(root);  // kept for other tools
}
