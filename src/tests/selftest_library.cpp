// Library checks of the self test (see selftest.cpp): title formatting, the library.cdb save/load
// file, the play log, cordinal points and auto-playlist rules. RunSelfTest calls each one inside
// its own testlog group, in the order the groups appear in the log.
#include "selftest_common.h"

// The original per-track search is the oracle for both correctness and the search benchmark.
// It deliberately owns no state shared between tracks.
bool ReferenceSearch(const Library& library, TrackId id, const std::vector<std::wstring>& words) {
    if (!library.Contains(id)) return false;
    const auto& t = library[id];
    const StringPool::Handle handles[] = {t.artist,t.title,t.album,t.albumArtist,t.genre,t.date,
                                         t.file,t.dir,t.composer,t.comment,t.grouping,t.label};
    std::wstring fields[13];
    bool loaded[13]{};
    for (const auto& word : words) {
        bool found = false;
        for (size_t i = 0; i < std::size(fields); ++i) {
            if (i < std::size(handles) && !handles[i]) continue;
            if (!loaded[i]) {
                fields[i] = i < std::size(handles) ? library.Str(handles[i]) : CodecName(t.codec);
                loaded[i] = true;
            }
            const auto& field = fields[i];
            if (!field.empty() && FindNLSStringEx(LOCALE_NAME_USER_DEFAULT, FIND_FROMSTART | LINGUISTIC_IGNORECASE,
                field.data(), (int)field.size(), word.data(), (int)word.size(), nullptr,nullptr,nullptr,0) >= 0) {
                found = true; break;
            }
        }
        if (!found) return false;
    }
    return true;
}

void TestSearch() {
    Library library;
    for (unsigned i = 0; i < 100; ++i) {
        TrackId id = library.Add(L"C:\\Search\\Música\\" + std::to_wstring(i / 10) + L"\\Track " + std::to_wstring(i) + L".flac");
        TagData tags;
        tags.title = "Track " + std::to_string(i);
        tags.artist = i % 3 ? "Björk" : "Other";
        tags.album = i % 5 ? "Live 2026" : "A\xCC\x8A";
        tags.genre = i % 7 ? "Rock" : "";
        tags.comment = "東京"; tags.codec = i % 2 ? Codec::FLAC : Codec::MP3;
        library.ApplyTags(id, tags);
    }
    bool same = true;
    for (auto query : {L"", L"track", L"BJÖRK", L"東京", L"Música", L"Å", L"FLAC", L"MP3",
                       L"Rock Björk", L"\"Live 2026\" Track", L"missing", L"Track absent"}) {
        const auto words = FilterWords(query);
        TrackMatcher matcher(library, words);
        // Repeated ids, alternating field handles and an invalid id exercise replacement and reuse.
        for (unsigned i = 0; i < 500; ++i) {
            TrackId id = i % 113;
            same &= matcher.Matches(id) == ReferenceSearch(library, id, words);
        }
    }
    Check(same, L"SEARCH cached fields preserve linguistic, Unicode, phrase, path, codec and duplicate-id matches");
    std::vector<std::wstring> words(70, L"Track");
    TrackMatcher longQuery(library, words);
    bool longOk = longQuery.Matches(0) && longQuery.Matches(1);
    words.back() = L"absent";
    TrackMatcher longMiss(library, words);
    Check(longOk && !longMiss.Matches(0) && !longMiss.Matches(1),
          L"SEARCH words beyond the 64-bit cache mask are still required");
    // Handles are library-local, and edited tags must be visible in the next query.
    auto query = FilterWords(L"Björk");
    TrackMatcher first(library, query);
    bool changed = first.Matches(1);
    TagData edit; edit.artist = "Changed";
    library.ApplyTags(1, edit);
    TrackMatcher next(library, query);
    Library other;
    TrackId id = other.Add(L"C:\\Other.flac");
    other.ApplyTags(id, edit);
    TrackMatcher separate(other, query);
    Check(changed && !next.Matches(1) && !separate.Matches(id),
          L"SEARCH caches end with each query and never cross library pools");
    const auto folderWords = FilterWords(L"\"C:\\Search\\Música\\2\"");
    TrackMatcher folder(library, folderWords);
    testlog::Cases folderCases;
    for (TrackId track = 0; track < library.Count(); ++track)
        folderCases.Add(folder.Matches(track) == (track >= 20 && track < 30), library.Path(track));
    Check(folderCases.Passed(), L"SEARCH a full folder path isolates its tracks even when tags repeat%ls",
          folderCases.failed.c_str());
    testlog::Cases roots;
    auto tree = [&](const wchar_t* path, std::vector<std::wstring> folders, std::vector<std::wstring> expected) {
        roots.Add(FolderTreePath(path, folders) == expected, path);
    };
    tree(L"D:\\Music\\Artist\\Album\\one.flac", {L"D:\\Music"}, {L"D:\\Music", L"Artist", L"Album", L"one.flac"});
    tree(L"C:/Music/Album/one.flac", {L"c:\\Music\\"}, {L"c:\\Music", L"Album", L"one.flac"});
    tree(L"D:\\Music\\Album\\one.flac", {L"D:\\Music", L"D:\\Music\\Album"}, {L"D:\\Music\\Album", L"one.flac"});
    tree(L"D:\\MusicOther\\one.flac", {L"D:\\Music"}, {L"D:\\MusicOther", L"one.flac"});
    tree(L"E:\\one.flac", {}, {L"E:\\", L"one.flac"});
    tree(L"\\\\server\\share\\Music\\one.flac", {L"\\\\server\\share"}, {L"\\\\server\\share", L"Music", L"one.flac"});
    tree(L"/home/user/Music/Album/one.flac", {L"/home/user/Music/"}, {L"/home/user/Music", L"Album", L"one.flac"});
    tree(L"/home/user/music/one.flac", {L"/home/user/Music"}, {L"/home/user/music", L"one.flac"});
    tree(L"/one.flac", {L"/"}, {L"/", L"one.flac"});
    tree(L"C:\\Downloads\\Love the World\\one.flac", {}, {L"C:\\Downloads\\Love the World", L"one.flac"});
    Check(roots.Passed(), L"folder trees keep Music, custom, other-drive, UNC and POSIX roots and folder boundaries%ls",
          roots.failed.c_str());
}

// Title-format mini-language against one synthetic track: conditionals, function calls, digit and
// case helpers, literals, and the built-in fields (filename, ext, directory, album artist).
void TestTitleFormat() {
    TrackId id = g_lib.Add(L"C:\\Music\\Band\\03 Song Name.flac");
    TagData tags;
    tags.artist = "Band";
    tags.title = "Song";
    tags.track = 3;
    tags.duration = 245.4;
    tags.codec = Codec::FLAC;
    g_lib.ApplyTags(id, tags);
    testlog::Cases patterns;
    auto check = [&](const wchar_t* pattern, const std::wstring& expected) {
        std::wstring got = FormatTitle(pattern, [&](const std::wstring& n, std::wstring& o) { return LookupField(id, n, o); });
        patterns.Add(got == expected, std::wstring(pattern) + L" => " + got + L" not " + expected);
    };
    check(L"[%artist% - ]%title%", L"Band - Song");
    check(L"[%album% - ]%title%", L"Song");
    check(L"%album%", L"?");
    check(L"%tracknumber%. %title% (%length%) %codec%", L"03. Song (4:05) FLAC");
    check(L"$if(%album%,yes,no) $if2(%album%,%artist%)", L"no Band");
    check(L"$upper(%artist%) $num(%tracknumber%,4) $caps(hello wORLD)", L"BAND 0003 Hello World");
    check(L"'[lit]' %% 'it'''s'", L"[lit] % it's");
    check(L"%filename%|%ext%|%directory%|[%album artist%]", L"03 Song Name|flac|Band|Band");
    check(L"$pad(%artist%,6)|$add(2,3)|$replace(%title%,o,0)", L"Band  |5|S0ng");
    Check(patterns.Passed(), L"TF conditionals, functions, literals and built-in fields format as expected%ls",
          patterns.failed.c_str());
}

// library.cdb round trip: every field, playlist kinds, path lookups, and a corrupt file is set
// aside instead of being loaded or overwritten.
// The FLAC audio hash survives a save and load, also when saved ids are renumbered.
void TestAudioHashSection(const std::wstring& scratch) {
    std::wstring file = scratch + L"-hash.cdb";
    g_lib = Library();
    TrackId gone = g_lib.Add(L"C:\\Music\\gone.flac");  // not saved: shifts the saved ids
    TrackId a = g_lib.Add(L"C:\\Music\\a.flac"), b = g_lib.Add(L"C:\\Music\\b.wav"), c = g_lib.Add(L"C:\\Music\\c.flac");
    for (TrackId id : {a, b, c}) g_lib[id].flags |= Track::kInLibrary;
    g_lib[gone].audioHash = 1;
    g_lib[a].audioHash = 0x29d1034ca5a06312ull;
    g_lib[c].audioHash = 0xFFFFFFFFFFFFFFFFull;
    std::vector<Playlist> playlists;
    bool saved = SaveLibrary(file, playlists);
    g_lib = Library();
    bool loaded = LoadLibrary(file, playlists);
    TrackId a2 = g_lib.Find(L"C:\\Music\\a.flac"), b2 = g_lib.Find(L"C:\\Music\\b.wav"), c2 = g_lib.Find(L"C:\\Music\\c.flac");
    bool ok = saved && loaded && a2 != UINT32_MAX && b2 != UINT32_MAX && c2 != UINT32_MAX && g_lib.Count() == 3 &&
              g_lib[a2].audioHash == 0x29d1034ca5a06312ull && g_lib[b2].audioHash == 0 && g_lib[c2].audioHash == 0xFFFFFFFFFFFFFFFFull;
    Check(ok, L"LIBRARYDB audio hashes round trip with renumbered ids (saved=%d loaded=%d)", saved, loaded);
    DeleteFileW(file.c_str());
    DeleteFileW((file + L".bak").c_str());
    g_lib = Library();
}

// library.cdb round trip: every stored field, the three playlist kinds, path lookups, backup on
// replacement, blocked/corrupt saves, and that truncated or malformed files never overwrite state.
void TestLibraryDb(const std::wstring& scratch) {
    std::wstring file = scratch + L".cdb";
    const std::wstring pathA = L"C:\\Music\\\x00C4rtist\\01 One.flac", pathB = L"C:\\Music\\B\\02 Two.mp3";
    TrackId a = g_lib.Add(pathA), b = g_lib.Add(pathB);
    TagData tags;
    tags.title = "One";
    tags.artist = "\xC3\x84rtist";
    tags.composer = "Comp";
    tags.disc = 2;
    tags.trackTotal = 12;
    tags.compilation = true;
    g_lib.ApplyTags(a, tags);
    Track& t = g_lib[a];
    t.flags |= Track::kInLibrary;
    t.playCount = 7;
    t.skipCount = 2;
    t.rating = 90;
    t.lastPlayed = 1700000000;
    t.rgTrackGain = -650;
    t.fileTime = 133000000000000000ull;
    std::vector<Playlist> pls{{L"Library", {}, PlaylistKind::Library},
                              {L"Mix", {b, a, b}, PlaylistKind::Static},
                              {L"Fresh", {a}, PlaylistKind::Auto, "rules"}};
    Playlist manualAuto{L"Arranged auto", {b, a}, PlaylistKind::Auto, "rules"};
    Playlist manualInbox{L"Inbox", {a, b}, PlaylistKind::Inbox};
    manualAuto.sortColumn = manualInbox.sortColumn = -2;
    pls.push_back(std::move(manualAuto));
    pls.push_back(std::move(manualInbox));
    bool saved = SaveLibrary(file, pls);
    g_lib = Library();
    std::vector<Playlist> back;
    bool loaded = LoadLibrary(file, back);
    TrackId a2 = g_lib.Find(pathA), b2 = g_lib.Find(pathB);
    bool found = a2 != UINT32_MAX && b2 != UINT32_MAX;
    const Track empty;
    const Track& r = found ? g_lib[a2] : empty;
    bool fields = found && g_lib.View(r.artist) == "\xC3\x84rtist" && g_lib.View(r.composer) == "Comp" && r.disc == 2 &&
                  r.trackTotal == 12 && (r.flags & Track::kCompilation) && r.InLibrary() && r.playCount == 7 &&
                  r.skipCount == 2 && r.rating == 90 && r.lastPlayed == 1700000000 && r.rgTrackGain == -650 &&
                  r.fileTime == 133000000000000000ull;
    bool lists = back.size() == 5 && back[0].kind == PlaylistKind::Library && back[1].name == L"Mix" &&
                 back[1].items == std::vector<TrackId>{b2, a2, b2} && back[2].kind == PlaylistKind::Auto &&
                 back[2].rules == "rules" && back[2].items.empty();
    Check(saved && loaded && fields && lists, L"LIBRARY DB round trip saved=%d loaded=%d fields=%d playlists=%d", saved,
          loaded, fields, lists);
    Check(loaded && back.size() == 5 && back[3].sortColumn == -2 && back[3].items == std::vector<TrackId>{b2, a2} &&
              back[4].sortColumn == -2 && back[4].items == std::vector<TrackId>{a2, b2},
          L"LIBRARY DB keeps the manual order of auto and Inbox views across a restart");

    std::string valid = ReadTestBytes(file);
    if (found) g_lib[a2].playCount = 8;
    bool savedAgain = SaveLibrary(file, back);
    Check(savedAgain && !valid.empty() && ReadTestBytes(file + L".bak") == valid,
          L"LIBRARY DB replacement keeps the previous complete file as backup");

    HANDLE locked = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    app::g.libraryPath = file;
    app::g.playlists = back;
    app::g.dirty = true;
    app::g.ticksSinceSave = 80;
    std::string before = ReadTestBytes(file);
    app::SaveLibraryNow();
    Check(locked != INVALID_HANDLE_VALUE && app::g.dirty && app::g.ticksSinceSave == 0 &&
              app::g.message.find(L"Couldn't replace") != std::wstring::npos &&
              ReadTestBytes(file) == before && ReadTestBytes(file + L".bak") == valid &&
              GetFileAttributesW((file + L".tmp").c_str()) == INVALID_FILE_ATTRIBUTES,
          L"LIBRARY DB blocked save preserves data and backup, cleans temp, and stays pending");
    // What closing does when the save fails: the changes go beside the locked file and come back first.
    if (found) g_lib[a2].playCount = 9;
    const std::wstring unsaved = app::UnsavedLibraryPath();
    bool kept = app::KeepUnsavedLibrary();
    if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
    if (found) g_lib[a2].playCount = 0;
    bool restored = app::RestoreUnsavedLibrary();
    TrackId back9 = g_lib.Find(pathA);
    Check(kept && restored && app::g.dirty && back9 == a2 && found && g_lib[a2].playCount == 9 &&
              ReadTestBytes(file) == before,
          L"LIBRARY DB changes that couldn't be saved at close are kept beside it and restored at the next start");
    if (found) g_lib[a2].playCount = 8;  // what the checks below expect
    app::SaveLibraryNow();
    Check(!app::g.dirty && app::g.ticksSinceSave == 0 && GetFileAttributesW(unsaved.c_str()) == INVALID_FILE_ATTRIBUTES,
          L"LIBRARY DB save succeeds after the lock is released and removes the kept copy");
    // A copy whose delete failed is older than the library.cdb saved after it.
    app::KeepUnsavedLibrary();
    HANDLE old = CreateFileW(unsaved.c_str(), FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    FILETIME past{0, 1};
    if (old != INVALID_HANDLE_VALUE) { SetFileTime(old, nullptr, nullptr, &past); CloseHandle(old); }
    Check(!app::RestoreUnsavedLibrary(), L"LIBRARY DB a kept copy older than library.cdb is ignored");
    DeleteFileW(unsaved.c_str());
    app::g.playlists.clear();
    app::g.libraryPath.clear();

    auto invalidLists = back;
    invalidLists.push_back({L"Invalid", {UINT32_MAX}, PlaylistKind::Static});
    before = ReadTestBytes(file);
    std::wstring error;
    Check(!SaveLibrary(file, invalidLists, &error) && !error.empty() && ReadTestBytes(file) == before,
          L"LIBRARY DB invalid track references cannot overwrite the file");

    const std::wstring unavailable = scratch + L"-absent\\library.cdb";
    Check(!SaveLibrary(unavailable, back, &error) && error.find(L"Couldn't create") != std::wstring::npos,
          L"LIBRARY DB failed creation reports its operation and Windows error");

    const std::wstring moved = L"D:\\Sorted\\One.flac";
    if (found) g_lib.Rename(a2, moved);
    Check(found && g_lib.Find(moved) == a2 && g_lib.Find(pathA) == UINT32_MAX && g_lib.Find(pathB) == b2 &&
              g_lib.Find(L"C:\\Nope.mp3") == UINT32_MAX,
          L"LIBRARY find/rename");

    size_t memory = g_lib.MemoryBytes(), count = g_lib.Count();
    uint32_t revision = g_lib.Revision();
    auto reject = [&](std::string_view bytes) {
        return WriteTestBytes(file, bytes) && !LoadLibrary(file, back) && g_lib.Count() == count &&
               g_lib.MemoryBytes() == memory && g_lib.Revision() == revision && g_lib.Find(moved) == a2 &&
               a2 < g_lib.Count() && g_lib[a2].playCount == 8 && g_lib[a2].rating == 90 &&
               back.size() == 5 && back[1].items == std::vector<TrackId>{b2, a2, b2};
    };
    bool truncations = !valid.empty();
    for (size_t n = 0; n < valid.size(); ++n) {
        bool rejectedPrefix = reject(std::string_view(valid).substr(0, n));
        truncations &= rejectedPrefix;
    }
    Check(truncations, L"LIBRARY DB all %zu truncated prefixes leave tracks, stats, strings and playlists untouched",
          valid.size());

    const std::string header("CDLB\x01\0\0\0", 8);
    std::string overflow = header + std::string(9, '\x80') + '\x02' + std::string(3, '\0');
    std::string hugeCount = header + std::string(9, '\xFF') + '\x01' + std::string(3, '\0');
    std::string badKind = header + std::string("\0\0\1\xFF\0\0\0", 7);
    Check(reject(overflow) && reject(hugeCount) && reject(badKind) && reject(valid + '\0'),
          L"LIBRARY DB rejects overflowing varints, impossible counts, invalid kinds and trailing bytes");

    auto badField = [&](int field, uint64_t value) {
        std::string bytes = header + std::string("\0\1", 2) + std::string(12, '\0');
        auto u = [&](uint64_t v) {
            do {
                uint8_t b = (uint8_t)(v & 0x7F);
                v >>= 7;
                bytes += (char)(b | (v ? 0x80 : 0));
            } while (v);
        };
        for (int i = 0; i < 20; ++i) u(i == field ? value : 0);
        bytes.append(3, '\0');  // two gains and no playlists
        return reject(bytes);
    };
    Check(badField(6, 1ull << 32) && badField(8, 65536) && badField(14, 256) && badField(15, 255) && badField(16, 101),
          L"LIBRARY DB rejects out-of-range stats, bitrate, channels, codecs and ratings");

    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD written = 0;
    WriteFile(h, "CDLB\x01\0\0\0\xFF\xFF", 10, &written, nullptr);
    CloseHandle(h);
    std::vector<Playlist> none;
    std::wstring preserved;
    bool rejected = !LoadLibrary(file, none, &preserved) && GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES;
    Check(rejected && !preserved.empty() && GetFileAttributesW(preserved.c_str()) != INVALID_FILE_ATTRIBUTES,
          L"LIBRARY DB corrupt file rejected, kept, and named for the user");

    // Recovery changes written at the last close are named when they cannot be read either.
    app::g.libraryPath = file;
    const std::wstring recovery = app::UnsavedLibraryPath();
    std::wstring failedRecovery;
    bool wroteRecovery = WriteTestBytes(recovery, "not a library");
    bool noRecovery = !app::RestoreUnsavedLibrary(&failedRecovery);
    Check(wroteRecovery && noRecovery && !failedRecovery.empty() &&
              GetFileAttributesW(recovery.c_str()) == INVALID_FILE_ATTRIBUTES &&
              GetFileAttributesW(failedRecovery.c_str()) != INVALID_FILE_ATTRIBUTES,
          L"LIBRARY DB unreadable recovery changes are kept and named for the user");
    app::g.libraryPath.clear();
    app::g.playlists.clear();

    WIN32_FIND_DATAW fd;
    std::wstring dir = DirectoryOf(file);
    HANDLE find = FindFirstFileW((file + L"*").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
        while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    g_lib = Library();
}

// ---------------------------------------------------------------------------
// Play log, points and library.cdb version 2 sections.

namespace {

uint32_t LocalAt(int year, int month, int day, int hour = 0, int minute = 0) {
    tm t{};
    t.tm_year = year - 1900;
    t.tm_mon = month - 1;
    t.tm_mday = day;
    t.tm_hour = hour;
    t.tm_min = minute;
    t.tm_isdst = -1;
    return (uint32_t)mktime(&t);
}

// A library track with the given tags; the path is unique per title.
TrackId AddTestTrack(const char* title, const char* artist, const char* album, uint8_t disc, uint16_t trackNo,
                     double seconds, bool inLibrary) {
    TrackId id = g_lib.Add(L"C:\\Music\\Points\\" + FromUtf8(title) + L".flac");
    Track& t = g_lib[id];
    t.title = g_lib.Intern(title);
    t.artist = g_lib.Intern(artist);
    if (*album) t.album = g_lib.Intern(album);
    t.disc = disc;
    t.trackNo = trackNo;
    t.durationMs = (uint32_t)(seconds * 1000);
    if (inLibrary) t.flags |= Track::kInLibrary;
    return id;
}

void ResetPlayState() {
    g_lib = Library();
    SetPlayLog({});
    ResetPoints();
}

// Logs a listen, sets the track's play count, and awards it.
PointsAward ListenTest(uint32_t now, TrackId id, uint32_t plays, double seconds, bool counted, bool skip, uint32_t prevLast) {
    g_lib[id].playCount = plays;
    LogPlay(id, now, seconds, counted, skip);
    return AwardListenAt(now, id, seconds, counted, skip, prevLast);
}
}  // namespace

// Play log and statistics: counted/skipped events, period stats, streaks, per-hour/weekday totals,
// top tracks/artists/albums, "on this day", save/load renumbering, and the plays-in-N-days rules.
void TestListeningModel() {
    auto original = PlayLog();
    SetPlayLog({});
    Check(ListeningWeights({0, 1, 2}) == std::vector<double>({1, 1, 1}), L"unknown tracks have equal weights");
    for (unsigned i = 0; i < 12; ++i) LogPlay(1, 1000 + i * 100, 5, false, true);
    LogPlay(2, 3000, 60, false, true);  // late skips don't train early-skip aversion
    auto weights = ListeningWeights({0, 1, 2});
    Check(weights[1] >= 0.05 && weights[1] < weights[0] * 0.1 && weights[2] == weights[0],
          L"early skips lower weight without excluding a track; late skips do not");
    for (unsigned i = 0; i < 30; ++i) LogPlay(1, 4000 + i * 300, 200, true, false);
    Check(ListeningWeights({1})[0] > weights[1] * 5, L"later counted listens recover a skipped track's weight");
    SetPlayLog({{0, 1000, 200, PlayEvent::kCounted}, {1, 1200, 200, PlayEvent::kCounted},
                {0, 1400, 200, PlayEvent::kCounted}, {1, 1600, 200, PlayEvent::kCounted},
                {2, 9000, 200, PlayEvent::kCounted}, {0, 9200, 200, PlayEvent::kCounted},
                {3, 9205, 5, PlayEvent::kSkip}, {2, 9405, 200, PlayEvent::kCounted}});
    auto radio = ListeningWeights({0, 1, 2, 3}, 0);
    Check(radio[1] == 17 && radio[2] == 1 && radio[3] < 1,
          L"radio learns counted next-track transitions, excludes session gaps and breaks chains at skips");
    RemapPlayLog({2, 0, 1, UINT32_MAX});
    Check(ListeningWeights({0, 1}, 2)[0] == 17, L"compaction rebuilds radio transitions with remapped track IDs");
    SetPlayLog(std::move(original));
}

void TestPlayLog(const std::wstring& scratch) {
    ResetPlayState();
    TrackId a = AddTestTrack("One", "Ann", "Alpha", 1, 1, 200, true);
    TrackId b = AddTestTrack("Two", "Ann", "Alpha", 1, 2, 200, true);
    TrackId c = AddTestTrack("Other", "Bob", "Beta", 1, 1, 100, true);
    LogPlay(a, LocalAt(2026, 1, 5, 10), 200.4, true, false);
    LogPlay(b, LocalAt(2026, 1, 6, 11), 100, true, false);
    LogPlay(a, LocalAt(2026, 1, 7, 20), 210, true, false);
    LogPlay(b, LocalAt(2026, 1, 9, 8), 30, false, true);
    LogPlay(c, LocalAt(2026, 1, 9, 9), 0, false, false);  // neither counted nor skipped: not logged
    Check(PlayLog().size() == 4 && PlayLog()[3].flags == PlayEvent::kSkip && PlayLog()[0].seconds == 200,
          L"PLAY LOG logs counted plays and skips only, seconds clamped");

    PeriodStats s = ComputeStats(LocalAt(2026, 1, 1), LocalAt(2027, 1, 1), 5);
    Check(s.plays == 3 && s.skips == 1 && s.seconds == 540 && s.distinctTracks == 2 && s.discoveries == 2,
          L"PLAY LOG stats: plays %u skips %u seconds %llu tracks %u discoveries %u", s.plays, s.skips,
          (unsigned long long)s.seconds, s.distinctTracks, s.discoveries);
    Check(s.longestStreak == 3 && s.busiestDay == (uint32_t)LocalDay(LocalAt(2026, 1, 7, 12)) && s.busiestDaySeconds == 210,
          L"PLAY LOG streak of three days (Jan 5-7), busiest day is Jan 7 with 210 s");
    Check(s.byHour[10] == 1 && s.byHour[11] == 1 && s.byHour[20] == 1 && s.byHour[8] == 0 && s.byWeekday[0] == 1 &&
              s.byWeekday[1] == 1 && s.byWeekday[2] == 1,
          L"PLAY LOG hours and weekdays count counted plays only (Jan 5 2026 is a Monday)");
    Check(s.topTracks.size() == 2 && s.topTracks[0].track == a && s.topTracks[0].plays == 2 &&
              s.topTracks[0].seconds == 410 && s.topTracks[1].track == b && s.topTracks[1].seconds == 130,
          L"PLAY LOG top tracks are ranked by plays, with listened seconds");
    PeriodStats topOne = ComputeStats(LocalAt(2026, 1, 1), LocalAt(2027, 1, 1), 1);
    PeriodStats noTop = ComputeStats(LocalAt(2026, 1, 1), LocalAt(2027, 1, 1), 0);
    Check(topOne.topTracks.size() == 1 && topOne.topTracks[0].track == s.topTracks[0].track &&
              noTop.topTracks.empty() && noTop.topArtists.empty() && noTop.topAlbums.empty() && noTop.plays == s.plays,
          L"PLAY LOG bounded rankings return the same best entry and a zero limit preserves totals");
    Check(s.topArtists.size() == 1 && s.topArtists[0].plays == 3 && s.topArtists[0].key == g_lib[a].artist &&
              s.topAlbums.size() == 1 && s.topAlbums[0].plays == 3 && s.topAlbums[0].key == g_lib[a].album,
          L"PLAY LOG top artists and albums group their tracks");

    PeriodStats part = ComputeStats(LocalAt(2026, 1, 6), LocalAt(2026, 1, 8), 5);
    Check(part.plays == 2 && part.skips == 0 && part.seconds == 310 && part.discoveries == 1,
          L"PLAY LOG periods are [from, to): Jan 6 to Jan 7, and Tracks first played before the period are not discoveries");

    LogPlay(a, LocalAt(2025, 1, 5, 9), 100, true, false);
    LogPlay(c, LocalAt(2025, 1, 5, 21), 100, true, false);
    LogPlay(a, LocalAt(2024, 1, 5, 9), 200, true, false);
    LogPlay(b, LocalAt(2025, 1, 6, 9), 100, true, false);
    std::vector<TrackId> onThisDay = PlayedOnThisDay(LocalAt(2026, 1, 5, 12));
    Check(onThisDay == std::vector<TrackId>{a, c}, L"PLAY LOG on this day: earlier years only, most played first");

    // Listening history keeps a track even after it leaves the library and every playlist.
    ResetPlayState();
    TrackId gone = AddTestTrack("Gone", "Bob", "", 0, 0, 60, false);
    TrackId kept = AddTestTrack("Kept", "Ann", "Alpha", 1, 1, 200, true);
    TrackId kept2 = AddTestTrack("Kept Two", "Ann", "Alpha", 1, 2, 200, true);
    const std::wstring keptPath = g_lib.Path(kept), kept2Path = g_lib.Path(kept2);
    uint32_t t1 = LocalAt(2026, 3, 2, 9);
    ResetPoints();
    GrantBackPayAt(t1 - 3600);
    ListenTest(t1 - 100, gone, 1, 200, true, false, 0);
    ListenTest(t1, kept, 1, 200, true, false, 0);
    ListenTest(t1 + 60, kept2, 1, 90, false, true, 0);
    std::vector<Playlist> none;
    std::wstring file = scratch + L"-log.cdb";
    uint64_t total = PointsTotal();
    size_t recent = PointsRecent().size();
    bool saved = SaveLibrary(file, none);
    ResetPlayState();
    bool loaded = LoadLibrary(file, none);
    Check(saved && loaded && PlayLog().size() == 3 && PlayLog()[1].id == g_lib.Find(keptPath) &&
              PlayLog()[2].id == g_lib.Find(kept2Path) && PlayLog()[2].flags == PlayEvent::kSkip,
          L"PLAY LOG round trip retains tracks referenced only by listening history");
    Check(PointsTotal() == total && PointsInitialized() && PointsRecent().size() == recent &&
              PointsRecent().back().id == g_lib.Find(kept2Path),
          L"POINTS round trip keeps the total, initialization and recent events of removed tracks");

    std::string valid = ReadTestBytes(file);
    bool truncations = !valid.empty();
    size_t loadedCount = g_lib.Count();
    for (size_t n = 0; n < valid.size(); ++n) {
        WriteTestBytes(file, std::string_view(valid).substr(0, n));
        truncations &= !LoadLibrary(file, none) && g_lib.Count() == loadedCount;
    }
    Check(truncations,
          L"LIBRARY DB v2 all %zu truncated prefixes with sections are rejected", valid.size());

    ResetPoints();
    SaveLibrary(file, none);
    ResetPlayState();
    loaded = LoadLibrary(file, none);
    Check(loaded && !PointsInitialized() && PointsTotal() == 0, L"POINTS a library saved before back pay stays uninitialized");

    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((file + L"*").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do DeleteFileW((DirectoryOf(file) + L"\\" + fd.cFileName).c_str());
        while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    // Auto-playlist rules on the play count of the last 30 and 365 days (skips don't count).
    ResetPlayState();
    TrackId r1 = AddTestTrack("Recent", "Rae", "", 0, 0, 200, true);
    TrackId r2 = AddTestTrack("Old", "Ola", "", 0, 0, 200, true);
    TrackId r3 = AddTestTrack("Skipped", "Sky", "", 0, 0, 200, true);
    uint32_t now = UnixNow();
    LogPlay(r1, now - 5 * 86400, 200, true, false);
    LogPlay(r1, now - 40 * 86400, 200, true, false);
    LogPlay(r2, now - 200 * 86400, 200, true, false);
    LogPlay(r3, now - 86400, 30, false, true);
    auto evaluate = [](const char* field, const char* value) {
        AutoPlaylist a;
        a.root.rules.push_back({RuleField::Plays30, RuleOp::AtLeast, "1", ""});
        a.root.rules[0].field = std::string(field) == "30" ? RuleField::Plays30 : RuleField::Plays365;
        a.root.rules[0].value = value;
        std::vector<TrackId> out;
        EvaluateAutoPlaylist(a, out);
        return out;
    };
    Check(evaluate("30", "1") == std::vector<TrackId>{r1}, L"PLAY LOG rule: plays in the last 30 days");
    Check(evaluate("365", "2") == std::vector<TrackId>{r1} && evaluate("365", "1") == (std::vector<TrackId>{r1, r2}),
          L"PLAY LOG rule: plays in the last 365 days, counted per track");
    ResetPlayState();
}

// Cordinal points: collection/album bonuses, level thresholds and ranks, listening/play/discovery
// awards, the day-streak multiplier, per-track fatigue, album runs, rediscovery and back pay.
void TestPoints() {

    // Collection: once per track, a new album once per album (case-insensitive).
    ResetPlayState();
    TrackId t1 = AddTestTrack("Alpha One", "Ann", "Alpha", 1, 1, 200, true);
    TrackId t2 = AddTestTrack("Alpha Two", "Ann", "Alpha", 1, 2, 200, true);
    TrackId xb = AddTestTrack("Beta One", "Bob", "Beta", 1, 1, 100, true);
    TrackId z = AddTestTrack("Alpha Six", "Ann", "Alpha", 1, 6, 200, false);
    TrackId w = AddTestTrack("Alpha Caps", "ANN", "alpha", 1, 3, 200, true);
    uint32_t t0 = LocalAt(2026, 3, 1, 9);
    PointsAward added = AwardLibraryAddsAt(t0, {t1, t2, xb, z});
    Check(added.centi == 4600 && added.events.size() == 5 && added.events[1].kind == PointsKind::NewAlbum &&
              added.events[4].kind == PointsKind::NewAlbum,
          L"POINTS collection: 2 each, 20 for a new album, 4600 centipoints for three tracks and two albums");
    Check(AwardLibraryAddsAt(t0, {t1, t2, xb, z}).centi == 0, L"POINTS a track is collected only once");
    Check(AwardLibraryAddsAt(t0, {w}).centi == 200 && PointsTotal() == 4800,
          L"POINTS a second track of an album (any case) is not a new album");
    Check(PointsThreshold(1) == 0 && PointsThreshold(2) == 5000 && PointsThreshold(3) == 28300 &&
              PointsThreshold(4) == 77900 && PointsLevel(0) == 1 && PointsLevel(4999) == 1 && PointsLevel(5000) == 2 &&
              PointsLevel(28299) == 2 && PointsLevel(28300) == 3,
          L"POINTS level thresholds: round(50 * (L-1)^2.5) points");
    Check(PointsRank(1) == std::wstring(L"Egg") && PointsRank(14) == std::wstring(L"Fledgling") &&
              PointsRank(15) == std::wstring(L"Songbird") && PointsNextRankLevel(15) == 20 && PointsNextRankLevel(100) == 0,
          L"POINTS ranks and the next rank");

    // Listening, plays, discoveries and the streak multiplier across day boundaries.
    ResetPlayState();
    std::vector<TrackId> s;
    for (const char* title : {"S1", "S2", "S3", "S4", "S5", "S6"}) s.push_back(AddTestTrack(title, "Sam", "", 0, 0, 200, true));
    PointsAward a1 = ListenTest(LocalAt(2026, 3, 2, 9), s[0], 1, 200, true, false, 0);
    Check(a1.centi == 2167 && a1.events.size() == 3 && a1.events[2].kind == PointsKind::Discovery,
          L"POINTS 200 s listened: 667 listening + 500 play + 1000 discovery");
    PointsAward a2 = ListenTest(LocalAt(2026, 3, 2, 10), s[1], 1, 200, true, false, 0);
    Check(a2.centi == 2167, L"POINTS a second track the same day");
    PointsAward a3 = ListenTest(LocalAt(2026, 3, 2, 10, 30), s[0], 2, 200, true, false, 0);
    Check(a3.centi == 1567 && PointsStreak(LocalAt(2026, 3, 2, 11)) == 1,
          L"POINTS a replay: 667 + 900; ten minutes make the day count (streak 1)");
    PointsAward a4 = ListenTest(LocalAt(2026, 3, 3, 9), s[2], 1, 200, true, false, 0);
    Check(a4.centi == 2167 && PointsMultiplier(LocalAt(2026, 3, 3, 9)) == 1.0,
          L"POINTS yesterday counted: the streak of 1 gives no bonus yet");
    ListenTest(LocalAt(2026, 3, 3, 9, 10), s[3], 1, 200, true, false, 0);
    PointsAward a6 = ListenTest(LocalAt(2026, 3, 3, 9, 20), s[4], 1, 200, true, false, 0);
    Check(a6.centi == 2167 && PointsStreak(LocalAt(2026, 3, 3, 9, 30)) == 2 &&
              PointsMultiplier(LocalAt(2026, 3, 3, 9, 30)) == 1.05,
          L"POINTS two counting days in a row: streak 2, multiplier 1.05");
    PointsAward a7 = ListenTest(LocalAt(2026, 3, 4, 9), s[0], 3, 200, true, false, 0);
    Check(a7.centi == 1855 && a7.events[0].centi == 700 && a7.events[1].centi == 1155,
          L"POINTS streak 2 multiplies listening (700) and play (1155): 1855");
    PointsAward a8 = ListenTest(LocalAt(2026, 3, 6, 9), s[1], 2, 200, true, false, 0);
    Check(a8.centi == 1567 && PointsStreak(LocalAt(2026, 3, 6, 9)) == 0,
          L"POINTS missing a day resets the streak: no bonus");

    // Eleven counting days in a row: streak 11, multiplier 1.5.
    for (int day = 1; day <= 11; ++day)
        ListenTest(LocalAt(2026, 4, day, 9), s[5], (uint32_t)day, 600, true, false, 0);
    Check(PointsStreak(LocalAt(2026, 4, 12, 9)) == 11 && PointsMultiplier(LocalAt(2026, 4, 12, 9)) == 1.5,
          L"POINTS eleven counting days: streak 11, multiplier 1.5");
    PointsAward a12 = ListenTest(LocalAt(2026, 4, 12, 9), s[5], 12, 600, true, false, 0);
    Check(a12.centi == 5850, L"POINTS at x1.5: 600 s = 3000 listening, play 1900 -> 2850");
    Check(PointsStreak(LocalAt(2026, 4, 14, 9)) == 0 && PointsMultiplier(LocalAt(2026, 4, 14, 9)) == 1.0,
          L"POINTS the streak ends after a missed day (Apr 13), even after twelve days");

    // Fatigue: from the 11th counted play of a track on one day, the play points are halved.
    ResetPlayState();
    TrackId f = AddTestTrack("Fatigue", "Fay", "", 0, 0, 60, true);
    PointsAward first, tenth, eleventh;
    for (uint32_t k = 1; k <= 11; ++k) {
        PointsAward p = ListenTest(LocalAt(2026, 5, 1, 8) + k * 60, f, k, 60, true, false, 0);
        if (k == 1) first = p;
        if (k == 10) tenth = p;
        if (k == 11) eleventh = p;
    }
    Check(first.centi == 1700 && tenth.centi == 4000 && eleventh.centi == 1100 && eleventh.events[1].centi == 900,
          L"POINTS fatigue: the 11th play the same day gets half the play points (900)");
    PointsAward nextDay = ListenTest(LocalAt(2026, 5, 2, 8), f, 12, 60, true, false, 0);
    Check(nextDay.centi == 2100, L"POINTS fatigue ends at midnight");

    // Album runs: four tracks in order, the full album, and runs broken by other tracks.
    ResetPlayState();
    std::vector<TrackId> al;
    for (int n = 1; n <= 5; ++n) {
        char title[16];
        snprintf(title, sizeof title, "Album %d", n);
        al.push_back(AddTestTrack(title, "Ann", "Alpha", 1, (uint16_t)n, 200, true));
    }
    TrackId beta = AddTestTrack("Beta", "Bob", "Beta", 1, 1, 200, true);
    uint32_t j = LocalAt(2026, 6, 1, 10);
    std::vector<PointsAward> run;
    for (int i = 0; i < 5; ++i) run.push_back(ListenTest(j + i * 300, al[i], 2, 200, true, false, 0));
    Check(run[0].centi == 1567 && run[1].centi == 1567 && run[2].centi == 1567, L"POINTS album run of 1 to 3 tracks: nothing extra");
    Check(run[3].centi == 4567 && run[3].events.size() == 3 && run[3].events[2].kind == PointsKind::AlbumRun,
          L"POINTS album run of 4 tracks: +30");
    Check(run[4].centi == 7067 && run[4].events.size() == 4 && run[4].events[2].kind == PointsKind::AlbumRun &&
              run[4].events[2].centi == 500 && run[4].events[3].kind == PointsKind::FullAlbum && run[4].events[3].centi == 5000,
          L"POINTS full album run: +5 for the fifth track and +50 for the whole album");
    PointsAward broke = ListenTest(j + 1800, beta, 2, 200, true, false, 0);
    Check(broke.centi == 1567, L"POINTS another album's track is a track in between");
    // Plays are 3 here: 667 listening + 1100 play.
    PointsAward r1 = ListenTest(j + 2100, al[0], 3, 200, true, false, 0);
    PointsAward r2 = ListenTest(j + 2400, al[1], 3, 200, true, false, 0);
    PointsAward r3 = ListenTest(j + 2700, al[2], 3, 200, true, false, 0);
    ListenTest(j + 3000, beta, 3, 200, true, false, 0);
    PointsAward r4 = ListenTest(j + 3300, al[3], 3, 200, true, false, 0);
    Check(r1.centi == 1767 && r2.centi == 1767 && r3.centi == 1767 && r4.centi == 1767,
          L"POINTS runs restart: a track out of order or another album breaks the run");
    PointsAward skipped = ListenTest(j + 3600, al[4], 3, 30, false, true, 0);
    Check(skipped.centi == 100 && skipped.events.size() == 1, L"POINTS a skip earns only its listening (30 s = 100)");
    // Tracks still waiting in the Inbox are not in the library yet: a run of them must not award
    // the full-album bonus for an album that has no library tracks to complete.
    ResetPlayState();
    std::vector<TrackId> inboxRun;
    for (int n = 1; n <= 4; ++n) {
        char title[16];
        snprintf(title, sizeof title, "Inbox %d", n);
        inboxRun.push_back(AddTestTrack(title, "Ivy", "Inbox album", 1, (uint16_t)n, 200, false));
    }
    PointsAward inboxAward;
    for (TrackId id : inboxRun) inboxAward = ListenTest(j + 4000 + (uint32_t)id * 300, id, 1, 200, true, false, 0);
    Check(std::none_of(inboxAward.events.begin(), inboxAward.events.end(),
                       [](const PointsEvent& e) { return e.kind == PointsKind::FullAlbum; }),
          L"POINTS an Inbox-only album run does not earn the full-album bonus");

    // Rediscovery: a counted play after 365 days or more.
    ResetPlayState();
    TrackId old = AddTestTrack("Old", "Ola", "", 0, 0, 200, true);
    uint32_t now = LocalAt(2026, 6, 10, 9);
    PointsAward rediscovered = ListenTest(now, old, 5, 200, true, false, now - 400 * 86400u);
    PointsAward notYet = ListenTest(now, old, 6, 200, true, false, now - 364 * 86400u);
    Check(rediscovered.centi == 3567 && rediscovered.events.back().kind == PointsKind::Rediscovery && notYet.centi == 2167,
          L"POINTS rediscovery after 365 days: +15, not before");

    // Back pay: granted once, for what the existing stats are worth.
    ResetPlayState();
    TrackId p1 = AddTestTrack("Back One", "Gil", "Gamma", 1, 1, 100, true);
    TrackId p2 = AddTestTrack("Back Two", "Gil", "Gamma", 1, 2, 100, true);
    TrackId p3 = AddTestTrack("Back Three", "Gil", "", 0, 0, 60, false);
    TrackId p5 = AddTestTrack("Back Five", "Gil", "", 0, 0, 0, false);
    g_lib[p1].playCount = 3;
    g_lib[p3].playCount = 2;
    g_lib[p5].playCount = 10;
    Check(!PointsInitialized(), L"POINTS back pay not granted before the first run");
    PointsAward back = GrantBackPayAt(LocalAt(2026, 7, 1, 9));
    Check(back.centi == 26200 && back.events.size() == 1 && back.events[0].kind == PointsKind::BackPay && back.levelAfter == 2 &&
              PointsTotal() == 26200 && PointsInitialized(),
          L"POINTS back pay: 26200 centipoints, one ledger event, level 2");
    Check(GrantBackPayAt(LocalAt(2026, 7, 1, 9)).centi == 0 && AwardLibraryAddsAt(LocalAt(2026, 7, 1, 9), {p1, p2}).centi == 0,
          L"POINTS back pay and collection are never granted twice");
    ResetPlayState();
}
// Auto-playlist rules over a small synthetic library.
void TestAutoPlaylists() {
    g_lib = Library();
    struct Spec {
        const char *title, *artist, *genre, *date;
        uint32_t plays, rating, ageDays, seconds;
    } specs[] = {
        {"Alpha", "The Strokes", "Rock", "2005", 12, 90, 2, 200},
        {"Beta", "The Strokes", "Rock", "2001", 3, 60, 40, 180},
        {"Gamma", "Toby Fox", "Soundtrack", "2018", 0, 0, 1, 120},
        {"Delta", "Fever Ray", "Electronic", "2009", 7, 100, 400, 330},
        {"Epsilon", "Ted Chiang", "", "", 1, 20, 10, 3600},
    };
    std::vector<TrackId> ids;
    uint32_t now = UnixNow();
    for (auto& s : specs) {
        TrackId id = g_lib.Add(L"C:\\Lib\\" + FromUtf8(s.title) + L".flac");
        TagData t;
        t.title = s.title;
        t.artist = s.artist;
        t.genre = s.genre;
        t.date = s.date;
        t.duration = s.seconds;
        g_lib.ApplyTags(id, t);
        Track& tr = g_lib[id];
        tr.flags |= Track::kInLibrary;
        tr.playCount = s.plays;
        tr.rating = (uint8_t)s.rating;
        tr.added = now - s.ageDays * 86400;
        tr.lastPlayed = s.plays ? now - s.ageDays * 86400 : 0;
        tr.fileSize = s.seconds * 100000ull;  // 100 kB a second
        ids.push_back(id);
    }
    auto titles = [&](const AutoPlaylist& a) {
        std::vector<TrackId> out;
        EvaluateAutoPlaylist(a, out);
        std::vector<std::wstring> r;
        for (TrackId id : out) {
            std::wstring t;
            LookupField(id, L"title", t);
            r.push_back(t);
        }
        if (!a.ordered) std::sort(r.begin(), r.end());
        return r;
    };
    testlog::Cases rules;
    auto check = [&](const wchar_t* what, const AutoPlaylist& a, std::vector<std::wstring> expected) {
        auto got = titles(a);
        std::wstring list;
        for (auto& t : got) list += L" " + t;
        rules.Add(got == expected, std::wstring(what) + L" ->" + list);
    };
    AutoPlaylist a;
    a.root.rules = {{RuleField::Artist, RuleOp::Contains, "strokes", ""}};
    check(L"artist contains (any case)", a, {L"Alpha", L"Beta"});
    a.root.rules = {{RuleField::Genre, RuleOp::IsEmpty, "", ""}};
    check(L"genre is empty", a, {L"Epsilon"});
    a.root.rules = {{RuleField::Rating, RuleOp::AtLeast, "4.5", ""}};
    check(L"rating >= 4.5 stars", a, {L"Alpha", L"Delta"});
    a.root.rules = {{RuleField::Year, RuleOp::Between, "2005", "2018"}};
    check(L"year between", a, {L"Alpha", L"Delta", L"Gamma"});
    a.root.rules = {{RuleField::LastPlayed, RuleOp::Never, "", ""}};
    check(L"never played", a, {L"Gamma"});
    a.root.rules = {{RuleField::DateAdded, RuleOp::InLastDays, "14", ""}};
    check(L"added in the last 14 days", a, {L"Alpha", L"Epsilon", L"Gamma"});
    for (const char* value : {"-1", "nan", "inf"}) {
        a.root.rules = {{RuleField::DateAdded, RuleOp::NotInLastDays, value, ""}};
        check(L"invalid date interval is rejected", a, {});
    }
    a.root.rules = {{RuleField::Length, RuleOp::AtLeast, "5:00", ""}};
    check(L"length >= 5:00", a, {L"Delta", L"Epsilon"});
    // ALL of (genre Rock) and ANY of (plays >= 10, rating 3 exactly)
    a.root.all = true;
    a.root.rules = {{RuleField::Genre, RuleOp::Is, "rock", ""}};
    RuleGroup any;
    any.all = false;
    any.rules = {{RuleField::PlayCount, RuleOp::AtLeast, "10", ""}, {RuleField::Rating, RuleOp::Equals, "3", ""}};
    a.root.groups = {any};
    check(L"nested ALL / ANY", a, {L"Alpha", L"Beta"});
    AutoPlaylist top;
    top.root.rules = {{RuleField::PlayCount, RuleOp::AtLeast, "1", ""}};
    top.limit = 2;
    top.pick = PickBy::MostPlayed;
    top.ordered = true;
    check(L"top 2 most played (ordered)", top, {L"Alpha", L"Delta"});
    AutoPlaylist minutes;
    minutes.limit = 9;
    minutes.unit = LimitUnit::Minutes;
    minutes.pick = PickBy::RecentlyAdded;
    minutes.ordered = true;
    // Gamma (2:00) and Alpha (3:20) fit. Epsilon (an hour) is skipped rather than ending the fill,
    // so Beta (3:00) still fits (8:20 total) and Delta (5:30 more) does not.
    check(L"9 minutes, most recently added (a long track is skipped, not the rest)", minutes,
          {L"Gamma", L"Alpha", L"Beta"});
    AutoPlaylist card;
    card.limit = 40;
    card.unit = LimitUnit::Megabytes;
    card.pick = PickBy::MostPlayed;
    card.ordered = true;
    // Alpha (20 MB) fits, Delta (33 MB) would pass 40 MB and is skipped, Beta (18 MB) still fits.
    check(L"fill 40 MB, most played", card, {L"Alpha", L"Beta"});
    Check(rules.Passed(), L"AUTOPLAYLIST rules on every field kind, nested ALL/ANY groups and limits select the expected "
          L"tracks, and invalid intervals select none%ls", rules.failed.c_str());

    std::string text = SerializeAutoPlaylist(a);
    AutoPlaylist back;
    bool parsed = ParseAutoPlaylist(text, back);
    Check(parsed && SerializeAutoPlaylist(back) == text && titles(back) == titles(a), L"AUTOPLAYLIST round trip through text");
    AutoPlaylist future;
    Check(ParseAutoPlaylist("cordinal-auto 1\ngroup any\nrule mood is happy\t\nrule title contains a\t\nend\n", future) &&
              future.root.rules.size() == 1 && !future.root.all,
          L"AUTOPLAYLIST unknown rule fields are skipped");
    g_lib = Library();
}
