// Library checks of the self test (see selftest.cpp): title formatting, the library.cdb save/load
// file, the play log, cordinal points and auto-playlist rules. RunSelfTest calls each one inside
// its own testlog group, in the order the groups appear in the log.
#include "selftest_common.h"

#include "sections.h"
#include "taste.h"

#include <ctime>

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
    testlog::Batch batch;
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
    batch.Next(L"SEARCH cached fields agree with the reference matcher for linguistic, Unicode, phrase, "
               L"path, codec and duplicate-id queries");
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
    batch.Next(L"SEARCH caches require words beyond the 64-bit mask and end with each query, never "
               L"crossing pools");
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
    batch.Next(L"SEARCH a full folder path isolates its tracks even when tags repeat");
    const auto folderWords = FilterWords(L"\"C:\\Search\\Música\\2\"");
    TrackMatcher folder(library, folderWords);
    testlog::Cases folderCases;
    for (TrackId track = 0; track < library.Count(); ++track)
        folderCases.Add(folder.Matches(track) == (track >= 20 && track < 30), library.Path(track));
    Check(folderCases.Passed(), L"SEARCH a full folder path isolates its tracks even when tags repeat%ls",
          folderCases.failed.c_str());
    batch.Next(L"folder trees keep Music, custom, other-drive, UNC and POSIX roots and folder boundaries");
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
    testlog::Batch batch(L"TF conditionals, functions, literals and built-in fields format as expected");
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
    testlog::Batch batch(L"LIBRARYDB audio hashes round trip with renumbered ids");
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
    testlog::Batch batch;
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
    batch.Next(L"LIBRARY DB round trip keeps every stored field, all three playlist kinds and manual "
               L"view order");
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

    batch.Next(L"LIBRARY DB replacement keeps the previous complete file as a backup");
    std::string valid = ReadTestBytes(file);
    if (found) g_lib[a2].playCount = 8;
    bool savedAgain = SaveLibrary(file, back);
    Check(savedAgain && !valid.empty() && ReadTestBytes(file + L".bak") == valid,
          L"LIBRARY DB replacement keeps the previous complete file as backup");

    batch.Next(L"LIBRARY DB a blocked save preserves the file and backup, and its changes survive a close");
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
    batch.Next(L"LIBRARY DB a later save succeeds and a kept copy older than library.cdb is ignored");
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

    batch.Next(L"LIBRARY DB invalid track references and a failed creation cannot overwrite or "
               L"create a file");
    auto invalidLists = back;
    invalidLists.push_back({L"Invalid", {UINT32_MAX}, PlaylistKind::Static});
    before = ReadTestBytes(file);
    std::wstring error;
    Check(!SaveLibrary(file, invalidLists, &error) && !error.empty() && ReadTestBytes(file) == before,
          L"LIBRARY DB invalid track references cannot overwrite the file");

    const std::wstring unavailable = scratch + L"-absent\\library.cdb";
    Check(!SaveLibrary(unavailable, back, &error) && error.find(L"Couldn't create") != std::wstring::npos,
          L"LIBRARY DB failed creation reports its operation and Windows error");

    batch.Next(L"LIBRARY DB find and rename follow the new path and forget the old one");
    const std::wstring moved = L"D:\\Sorted\\One.flac";
    if (found) g_lib.Rename(a2, moved);
    Check(found && g_lib.Find(moved) == a2 && g_lib.Find(pathA) == UINT32_MAX && g_lib.Find(pathB) == b2 &&
              g_lib.Find(L"C:\\Nope.mp3") == UINT32_MAX,
          L"LIBRARY find/rename");

    batch.Next(L"LIBRARY DB truncated, malformed and out-of-range files leave tracks, stats, strings and "
               L"playlists untouched");
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

    batch.Next(L"LIBRARY DB corrupt and unreadable files are set aside and named for the user");
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
    ClearTaste();
}

// Logs a listen, sets the track's play count, and awards it.
PointsAward ListenTest(uint32_t now, TrackId id, uint32_t plays, double seconds, bool counted, bool skip, uint32_t prevLast) {
    g_lib[id].playCount = plays;
    LogPlay(id, now, seconds, counted, skip);
    return AwardListenAt(now, id, seconds, counted, skip, prevLast);
}

// Logs listens back to back from `t` as one sitting, each starting two seconds after the last one
// ended. Hearing less than the counting threshold is a skip by hand. Returns the sitting's end.
uint32_t LogSitting(uint32_t t, std::initializer_list<std::pair<TrackId, int>> listens) {
    for (auto [id, seconds] : listens) {
        t += (uint32_t)seconds;
        bool counted = seconds >= std::clamp(g_lib[id].Duration() * 0.5, 1.0, 240.0);
        LogPlay(id, t, seconds, counted, !counted);
        t += 2;
    }
    return t;
}

Peaks FlatMood(uint8_t bass, uint8_t mid, uint8_t treble) {
    Peaks peaks{};
    for (int i = 0; i < kPeakBuckets; ++i) {
        peaks.mood[kMoodBass][i] = bass;
        peaks.mood[kMoodMid][i] = mid;
        peaks.mood[kMoodTreble][i] = treble;
    }
    return peaks;
}

// The numbers below are inequalities around the model in taste.cpp. A later tuning can move them;
// a reversal means the signal stopped mattering.
void TestTasteImpl(const std::wstring& scratch) {
    testlog::Batch batch;
    const uint32_t afternoon = LocalAt(2026, 6, 15, 15);
    auto score = [](std::initializer_list<TrackId> ids, TrackId from, uint32_t now) {
        return TasteWeights(g_lib, std::vector<TrackId>(ids), from, now);
    };

    batch.Next(L"unheard tracks with nothing in common weigh the same");
    ResetPlayState();
    TrackId ann = AddTestTrack("Ann song", "Ann", "", 1, 1, 200, true);
    TrackId bob = AddTestTrack("Bob song", "Bob", "", 1, 1, 200, true);
    TrackId cy = AddTestTrack("Cy song", "Cy", "", 1, 1, 200, true);
    auto equal = score({ann, bob, cy}, kInvalidTrack, afternoon);
    Check(equal.size() == 3 && equal[0] == equal[1] && equal[1] == equal[2] && equal[0] > 0.2,
          L"unheard tracks with nothing in common weigh the same (%.3f)", equal.empty() ? 0.0 : equal[0]);

    batch.Next(L"ratings outweigh an unheard song, and a top rating outweighs a half star");
    ResetPlayState();
    TrackId loved = AddTestTrack("Loved", "Rita", "Rates", 1, 1, 200, true);
    TrackId mild = AddTestTrack("Mild", "Rita", "Rates", 1, 2, 200, true);
    TrackId fresh = AddTestTrack("Fresh", "Rita", "Rates", 1, 3, 200, true);
    g_lib[loved].rating = 100;
    g_lib[mild].rating = 10;
    g_lib.Touch();
    auto rated = score({loved, mild, fresh}, kInvalidTrack, afternoon);
    Check(rated.size() == 3 && rated[0] > rated[1] * 8 && rated[0] > rated[2] * 2,
          L"a five-star song beats a half star and an unheard song (%.3f, %.3f, %.3f)",
          rated.size() == 3 ? rated[0] : 0, rated.size() == 3 ? rated[1] : 0, rated.size() == 3 ? rated[2] : 0);

    batch.Next(L"finishing beats an unheard song, which beats a run of early skips");
    ResetPlayState();
    TrackId finished = AddTestTrack("Finished", "Finny", "", 1, 1, 200, true);
    TrackId unheard = AddTestTrack("Unheard", "Newbie", "", 1, 1, 200, true);
    TrackId skipped = AddTestTrack("Skipped", "Skippy", "", 1, 1, 200, true);
    for (int i = 0; i < 10; ++i) LogPlay(skipped, LocalAt(2026, 2, 1, 12, i), 5, false, true);
    for (int i = 0; i < 2; ++i) LogPlay(finished, LocalAt(2026, 2, 2, 12, i), 200, true, false);
    g_lib.Touch();
    auto plays = score({finished, unheard, skipped}, kInvalidTrack, LocalAt(2026, 2, 3, 12));
    Check(plays.size() == 3 && plays[0] > plays[1] && plays[1] > plays[2],
          L"a finished song beats an unheard one, and an unheard one beats early skips (%.3f, %.3f, %.3f)",
          plays.size() == 3 ? plays[0] : 0, plays.size() == 3 ? plays[1] : 0, plays.size() == 3 ? plays[2] : 0);

    batch.Next(L"coming back on later days outweighs the same number of plays in one sitting");
    ResetPlayState();
    TrackId loyal = AddTestTrack("Loyal", "Lana", "Days", 1, 1, 180, true);
    TrackId binge = AddTestTrack("Binge", "Lana", "Days", 1, 2, 180, true);
    for (int i = 0; i < 5; ++i) LogPlay(loyal, LocalAt(2026, 4, 1 + i, 15), 180, true, false);
    for (int i = 0; i < 5; ++i) LogPlay(binge, LocalAt(2026, 4, 10, 15, i), 180, true, false);
    g_lib.Touch();
    auto days = score({loyal, binge}, kInvalidTrack, LocalAt(2026, 4, 11, 12));
    Check(days.size() == 2 && days[0] > days[1] * 1.8,
          L"five days of a song outweigh five plays on one day (%.3f vs %.3f)",
          days.size() == 2 ? days[0] : 0, days.size() == 2 ? days[1] : 0);

    batch.Next(L"an artist heard through at night but skipped by day leads at night, and the reverse by day");
    ResetPlayState();
    TrackId nightPlay = AddTestTrack("Night play", "Ann", "Nights", 1, 1, 200, true);
    TrackId dayPlay = AddTestTrack("Day play", "Bob", "Days", 1, 1, 200, true);
    TrackId nightNew = AddTestTrack("Night new", "Ann", "Nights", 1, 2, 200, true);
    TrackId dayNew = AddTestTrack("Day new", "Bob", "Days", 1, 2, 200, true);
    for (int day = 1; day <= 3; ++day) {
        LogSitting(LocalAt(2026, 3, day, 14), {{dayPlay, 200}, {nightPlay, 5}, {dayPlay, 200}});
        LogSitting(LocalAt(2026, 3, day, 22), {{nightPlay, 200}, {dayPlay, 5}, {nightPlay, 200}});
    }
    g_lib.Touch();
    auto atNight = score({nightNew, dayNew}, kInvalidTrack, LocalAt(2026, 3, 5, 22));
    auto atDay = score({nightNew, dayNew}, kInvalidTrack, LocalAt(2026, 3, 5, 14));
    Check(atNight.size() == 2 && atNight[0] > atNight[1] * 1.25,
          L"at night an unheard song by the night artist wins (%.3f vs %.3f)",
          atNight.size() == 2 ? atNight[0] : 0, atNight.size() == 2 ? atNight[1] : 0);
    Check(atDay.size() == 2 && atDay[1] > atDay[0] * 1.25,
          L"by day an unheard song by the day artist wins (%.3f vs %.3f)",
          atDay.size() == 2 ? atDay[1] : 0, atDay.size() == 2 ? atDay[0] : 0);

    batch.Next(L"an artist whose songs you finish beats one you only hear more of");
    ResetPlayState();
    std::vector<TrackId> big;
    for (int i = 0; i < 6; ++i) big.push_back(AddTestTrack(("Big " + std::to_string(i)).c_str(), "Big", "", 1, 1, 200, true));
    TrackId fewSong = AddTestTrack("Small", "Small", "", 1, 1, 200, true);
    TrackId bigNew = AddTestTrack("Big new", "Big", "", 1, 1, 200, true);
    TrackId smallNew = AddTestTrack("Small new", "Small", "", 1, 1, 200, true);
    // Six songs heard once each, half of them skipped, against one song finished twice. Totals would
    // favor Big for owning more; per listen, Small does better.
    LogSitting(LocalAt(2026, 3, 10, 15), {{big[0], 200}, {big[1], 5}, {big[2], 200}, {fewSong, 200}});
    LogSitting(LocalAt(2026, 3, 11, 15), {{big[3], 5}, {big[4], 200}, {big[5], 5}, {fewSong, 200}});
    g_lib.Touch();
    auto owned = score({bigNew, smallNew}, kInvalidTrack, LocalAt(2026, 3, 12, 15));
    Check(owned.size() == 2 && owned[1] > owned[0] * 1.2,
          L"the artist finished more often wins over the one with more songs (%.3f vs %.3f)",
          owned.size() == 2 ? owned[1] : 0, owned.size() == 2 ? owned[0] : 0);

    batch.Next(L"Keep playing like this follows a song you actually played next");
    ResetPlayState();
    TrackId from = AddTestTrack("From", "Ada", "", 1, 1, 200, true);
    TrackId followed = AddTestTrack("Followed", "Bea", "", 1, 1, 200, true);
    TrackId other = AddTestTrack("Other", "Cy", "", 1, 1, 200, true);
    // The same gaps the play log's radio test counts as two transitions, in the afternoon so time of
    // day does not also favor one of them. The third song's two plays sit outside those gaps.
    const uint32_t t0 = LocalAt(2026, 5, 4, 15);
    LogPlay(from, t0, 200, true, false);
    LogPlay(followed, t0 + 200, 200, true, false);
    LogPlay(from, t0 + 400, 200, true, false);
    LogPlay(followed, t0 + 600, 200, true, false);
    LogPlay(other, t0 + 5000, 200, true, false);
    LogPlay(other, t0 + 5400, 200, true, false);
    g_lib.Touch();
    auto hops = score({followed, other}, from, t0 + 6000);
    Check(hops.size() == 2 && hops[0] > hops[1] * 2,
          L"a song that followed this one twice ranks above one that never did (%.3f vs %.3f)",
          hops.size() == 2 ? hops[0] : 0, hops.size() == 2 ? hops[1] : 0);

    batch.Next(L"the next track of the same album ranks above an unrelated song");
    ResetPlayState();
    TrackId first = AddTestTrack("First", "Ada", "Album", 1, 1, 200, true);
    TrackId second = AddTestTrack("Second", "Ada", "Album", 1, 2, 200, true);
    TrackId elsewhere = AddTestTrack("Elsewhere", "Bea", "Other", 1, 1, 200, true);
    auto album = score({second, elsewhere}, first, afternoon);
    Check(album.size() == 2 && album[0] > album[1] * 2,
          L"the next track on the album ranks above an unrelated song (%.3f vs %.3f)",
          album.size() == 2 ? album[0] : 0, album.size() == 2 ? album[1] : 0);

    batch.Next(L"ReplayGain loudness and dynamics pull the next song toward the one playing");
    ResetPlayState();
    TrackId quietSeed = AddTestTrack("Quiet seed", "Que", "", 1, 1, 200, true);
    TrackId quiet = AddTestTrack("Quiet", "Match", "", 1, 1, 200, true);
    TrackId hot = AddTestTrack("Hot", "Hotty", "", 1, 1, 200, true);
    g_lib[quietSeed].rgTrackGain = 700;
    g_lib[quietSeed].rgHigh = 36;
    g_lib[quiet].rgTrackGain = 700;
    g_lib[quiet].rgHigh = 36;
    g_lib[hot].rgTrackGain = -900;
    g_lib[hot].rgHigh = 3;
    g_lib.Touch();
    auto gain = score({quiet, hot}, quietSeed, afternoon);
    Check(gain.size() == 2 && gain[0] > gain[1] * 1.4,
          L"a song at the same loudness and dynamics beats a hot master (%.3f vs %.3f)",
          gain.size() == 2 ? gain[0] : 0, gain.size() == 2 ? gain[1] : 0);

    batch.Next(L"a bright song prefers another bright song over a bass-heavy one");
    ResetPlayState();
    TrackId brightSeed = AddTestTrack("Bright seed", "Ada", "", 1, 1, 200, true);
    TrackId bright = AddTestTrack("Bright", "Bea", "", 1, 1, 200, true);
    TrackId bassy = AddTestTrack("Bassy", "Cy", "", 1, 1, 200, true);
    Peaks airy = FlatMood(8, 30, 230), low = FlatMood(230, 30, 8);
    NoteMood(brightSeed, airy);
    NoteMood(bright, airy);
    NoteMood(bassy, low);
    auto mood = score({bright, bassy}, brightSeed, afternoon);
    Check(mood.size() == 2 && mood[0] > mood[1] * 1.6,
          L"a bright song prefers another bright song (%.3f vs %.3f)",
          mood.size() == 2 ? mood[0] : 0, mood.size() == 2 ? mood[1] : 0);

    batch.Next(L"rewinding into a passage lifts that song before it counts as a play");
    ResetPlayState();
    TrackId replayed = AddTestTrack("Replayed", "Pat", "Parts", 1, 1, 200, true);
    TrackId plain = AddTestTrack("Plain", "Pat", "Parts", 1, 2, 200, true);
    ObservePlayback(replayed, 40, 20, 200);
    auto parts = score({replayed, plain}, kInvalidTrack, afternoon);
    Check(parts.size() == 2 && parts[0] > parts[1] * 1.5,
          L"rewinding into a passage lifts that song (%.3f vs %.3f)",
          parts.size() == 2 ? parts[0] : 0, parts.size() == 2 ? parts[1] : 0);

    batch.Next(L"leaving a song partway gives it no favorite part and counts for less than finishing");
    ResetPlayState();
    TrackId leftEarly = AddTestTrack("Left early", "Lee", "", 1, 1, 200, true);
    TrackId heardOut = AddTestTrack("Heard out", "Dot", "", 1, 1, 200, true);
    TrackId notYet = AddTestTrack("Not yet", "Nia", "", 1, 1, 200, true);
    TrackId after = AddTestTrack("After", "Ned", "", 1, 1, 200, true);
    for (double at = 0; at < 45; at += 0.25) ObservePlayback(leftEarly, at, at + 0.25, 200);
    const uint32_t stopDay = LocalAt(2026, 7, 1, 15);
    LogSitting(stopDay, {{heardOut, 200}, {leftEarly, 45}, {after, 200}});
    g_lib.Touch();
    auto stopped = score({leftEarly, heardOut, notYet}, kInvalidTrack, stopDay + 3600);
    Check(CurrentTaste().parts.empty() && stopped.size() == 3 && stopped[1] > stopped[0] &&
              stopped[0] <= stopped[2] * 1.001,
          L"45 seconds of a song marks no part and ranks below finishing it (%.3f vs %.3f; unheard %.3f)",
          stopped.size() == 3 ? stopped[0] : 0, stopped.size() == 3 ? stopped[1] : 0, stopped.size() == 3 ? stopped[2] : 0);

    batch.Next(L"a skip that ended the listening is not held against the song");
    ResetPlayState();
    TrackId lastOne = AddTestTrack("Last one", "Lou", "", 1, 1, 200, true);
    TrackId midway = AddTestTrack("Midway", "Mo", "", 1, 1, 200, true);
    TrackId untouched = AddTestTrack("Untouched", "Fay", "", 1, 1, 200, true);
    TrackId filler = AddTestTrack("Filler", "Fil", "", 1, 1, 200, true);
    // Each sitting ends on a skip of the same song: Stop, closing the player or shutting down.
    for (int day = 1; day <= 3; ++day)
        LogSitting(LocalAt(2026, 8, day, 15), {{filler, 200}, {midway, 5}, {filler, 200}, {lastOne, 5}});
    g_lib.Touch();
    auto ended = score({lastOne, midway, untouched}, kInvalidTrack, LocalAt(2026, 8, 5, 15));
    auto endedPlain = ListeningWeights({lastOne, midway, untouched});
    Check(ended.size() == 3 && ended[0] == ended[2] && ended[1] < ended[2] && endedPlain[0] == endedPlain[2] &&
              endedPlain[1] < endedPlain[2],
          L"skips that ended each sitting leave a song level with an unheard one (%.3f vs %.3f; mid-sitting %.3f)",
          ended.size() == 3 ? ended[0] : 0, ended.size() == 3 ? ended[2] : 0, ended.size() == 3 ? ended[1] : 0);

    batch.Next(L"a skip in a run of skips counts for less than one right after a song that was heard");
    ResetPlayState();
    TrackId inRun = AddTestTrack("In a run", "Bea", "", 1, 1, 200, true);
    TrackId rightAfter = AddTestTrack("Right after", "Pip", "", 1, 1, 200, true);
    std::vector<TrackId> others;
    for (int i = 0; i < 6; ++i)
        others.push_back(AddTestTrack(("Other " + std::to_string(i)).c_str(), ("Oz " + std::to_string(i)).c_str(), "", 1, 1,
                                      200, true));
    LogSitting(LocalAt(2026, 9, 1, 15), {{others[0], 200}, {others[1], 200}, {others[2], 200}, {others[3], 200}});
    LogSitting(LocalAt(2026, 9, 2, 15), {{others[4], 5}, {others[5], 5}, {others[0], 5}, {inRun, 5}, {others[1], 200}});
    LogSitting(LocalAt(2026, 9, 3, 15), {{others[2], 200}, {rightAfter, 5}, {others[3], 200}});
    g_lib.Touch();
    auto runs = score({inRun, rightAfter}, kInvalidTrack, LocalAt(2026, 9, 4, 15));
    auto runsPlain = ListeningWeights({inRun, rightAfter});
    Check(runs.size() == 2 && runs[0] > runs[1] * 1.1 && runsPlain[0] > runsPlain[1] * 1.1,
          L"the skip while hunting weighs less (%.3f vs %.3f; taste off %.3f vs %.3f)", runs.size() == 2 ? runs[0] : 0,
          runs.size() == 2 ? runs[1] : 0, runsPlain.size() == 2 ? runsPlain[0] : 0, runsPlain.size() == 2 ? runsPlain[1] : 0);

    batch.Next(L"a song is judged against songs of its genre in the same moments");
    ResetPlayState();
    std::vector<TrackId> rock, calm;
    for (int i = 0; i < 4; ++i) {
        rock.push_back(AddTestTrack(("Rock " + std::to_string(i)).c_str(), ("Rocker " + std::to_string(i)).c_str(), "", 1, 1,
                                    200, true));
        calm.push_back(AddTestTrack(("Calm " + std::to_string(i)).c_str(), ("Drifter " + std::to_string(i)).c_str(), "", 1, 1,
                                    200, true));
    }
    TrackId rockTest = AddTestTrack("Rock test", "Rock tester", "", 1, 1, 200, true);
    TrackId calmTest = AddTestTrack("Calm test", "Calm tester", "", 1, 1, 200, true);
    for (TrackId id : rock) g_lib[id].genre = g_lib.Intern("Rock");
    for (TrackId id : calm) g_lib[id].genre = g_lib.Intern("Ambient");
    g_lib[rockTest].genre = g_lib.Intern("Rock");
    g_lib[calmTest].genre = g_lib.Intern("Ambient");
    // After rock, rock is heard and ambient is skipped. Both test songs then get one skip and one
    // finish there: normal for an ambient song, out of character for a rock one.
    for (int day = 1; day <= 4; ++day)
        LogSitting(LocalAt(2026, 10, day, 15), {{rock[day % 4], 200}, {calm[day % 4], 5}, {rock[(day + 1) % 4], 200},
                                                {rock[(day + 2) % 4], 200}, {calm[(day + 1) % 4], 5}, {rock[(day + 3) % 4], 200}});
    LogSitting(LocalAt(2026, 10, 5, 15), {{rock[0], 200}, {calmTest, 5}, {rock[1], 200}, {rockTest, 5}, {rock[2], 200},
                                          {calmTest, 200}, {rock[3], 200}, {rockTest, 200}});
    g_lib.Touch();
    auto kinds = score({calmTest, rockTest}, kInvalidTrack, LocalAt(2026, 10, 6, 15));
    Check(kinds.size() == 2 && kinds[0] > kinds[1] * 1.2,
          L"the same skip and finish weigh for the ambient song and against the rock one (%.3f vs %.3f)",
          kinds.size() == 2 ? kinds[0] : 0, kinds.size() == 2 ? kinds[1] : 0);

    batch.Next(L"a shuffle by taste keeps a plain slice of the draw; Keep playing like this does not");
    ResetPlayState();
    TrackId favorite = AddTestTrack("Favorite", "Fran", "", 1, 1, 200, true);
    TrackId passedOver = AddTestTrack("Passed over", "Pat", "", 1, 1, 200, true);
    TrackId spacer = AddTestTrack("Spacer", "Sid", "", 1, 1, 200, true);
    g_lib[favorite].rating = 100;
    for (int day = 1; day <= 12; ++day)
        LogSitting(LocalAt(2026, 11, day, 15), {{spacer, 200}, {passedOver, 3}, {spacer, 200}});
    g_lib.Touch();
    const uint32_t sliceDay = LocalAt(2026, 11, 20, 15);
    auto pure = score({favorite, passedOver}, kInvalidTrack, sliceDay);
    auto mixed = OrderWeights(g_lib, {favorite, passedOver}, kInvalidTrack, sliceDay, true);
    double pureShare = pure.size() == 2 ? pure[1] / (pure[0] + pure[1]) : 0;
    double mixedShare = mixed.size() == 2 ? mixed[1] / (mixed[0] + mixed[1]) : 0;
    Check(pureShare < kPlainShare / 2 && mixedShare >= kPlainShare / 2 - 1e-9,
          L"the passed-over song keeps a plain share of the draw (%.3f, from %.3f)", mixedShare, pureShare);
    Check(OrderWeights(g_lib, {favorite, passedOver}, spacer, sliceDay, true) ==
              score({favorite, passedOver}, spacer, sliceDay),
          L"Keep playing like this keeps its own weights");

    batch.Next(L"the songs just played make another by that artist less likely");
    ResetPlayState();
    TrackId heard = AddTestTrack("Heard", "Ann", "Session", 1, 1, 200, true);
    TrackId again = AddTestTrack("Again", "Ann", "Session", 1, 2, 200, true);
    TrackId someone = AddTestTrack("Someone", "Bob", "Session", 1, 1, 200, true);
    NoteSession(heard);
    auto session = score({again, someone}, kInvalidTrack, afternoon);
    Check(session.size() == 2 && session[1] > session[0] * 1.5,
          L"after Ann, Bob outranks another Ann song (%.3f vs %.3f)",
          session.size() == 2 ? session[1] : 0, session.size() == 2 ? session[0] : 0);

    batch.Next(L"taste off still uses the play log's skip and transition weights");
    ResetPlayState();
    TrackId left = AddTestTrack("Left", "Ann", "", 1, 1, 200, true);
    TrackId known = AddTestTrack("Known", "Bea", "", 1, 1, 200, true);
    TrackId right = AddTestTrack("Right", "Cy", "", 1, 1, 200, true);
    for (unsigned i = 0; i < 12; ++i) LogPlay(known, 1000 + i * 100, 5, false, true);
    auto plainWeights = ListeningWeights({left, known, right});
    auto ordered = OrderWeights(g_lib, {left, known, right}, kInvalidTrack, afternoon, false);
    Check(plainWeights == ordered, L"taste off keeps the skip-aware weights");

    batch.Next(L"rewinds and mood sketches survive a library save, and a bad section does not");
    ResetPlayState();
    TrackId seed = AddTestTrack("Seed", "Ada", "Kept", 1, 1, 200, true);
    TrackId keptBright = AddTestTrack("Kept bright", "Bea", "Kept", 1, 1, 200, true);
    TrackId keptHeavy = AddTestTrack("Kept heavy", "Cy", "Kept", 1, 1, 200, true);
    TrackId keptReplay = AddTestTrack("Kept replay", "Dee", "Kept", 1, 1, 200, true);
    TrackId keptPlain = AddTestTrack("Kept plain", "Eve", "Kept", 1, 1, 200, true);
    NoteMood(seed, airy);
    NoteMood(keptBright, airy);
    NoteMood(keptHeavy, low);
    ObservePlayback(keptReplay, 40, 20, 200);
    const std::wstring seedPath = g_lib.Path(seed), brightPath = g_lib.Path(keptBright);
    const std::wstring heavyPath = g_lib.Path(keptHeavy), replayPath = g_lib.Path(keptReplay);
    const std::wstring plainPath = g_lib.Path(keptPlain);
    std::vector<Playlist> playlists;
    std::wstring error;
    const std::wstring file = scratch + L"\\taste.cdb";
    if (!Ready(SaveLibrary(file, playlists, &error), L"save a library with taste")) {
        ResetPlayState();
        return;
    }
    ClearTaste();
    g_lib = Library();
    if (!Ready(LoadLibrary(file, playlists), L"load a library with taste")) {
        ResetPlayState();
        return;
    }
    seed = g_lib.Find(seedPath);
    keptBright = g_lib.Find(brightPath);
    keptHeavy = g_lib.Find(heavyPath);
    keptReplay = g_lib.Find(replayPath);
    keptPlain = g_lib.Find(plainPath);
    auto keptMood = score({keptBright, keptHeavy}, seed, afternoon);
    auto keptParts = score({keptReplay, keptPlain}, kInvalidTrack, afternoon);
    Check(keptMood.size() == 2 && keptMood[0] > keptMood[1] * 1.6 && keptParts.size() == 2 &&
              keptParts[0] > keptParts[1] * 1.5,
          L"a saved library still prefers the bright song and the rewound one (%.3f vs %.3f, %.3f vs %.3f)",
          keptMood.size() == 2 ? keptMood[0] : 0, keptMood.size() == 2 ? keptMood[1] : 0,
          keptParts.size() == 2 ? keptParts[0] : 0, keptParts.size() == 2 ? keptParts[1] : 0);

    batch.Next(L"compaction moves a rewind onto the track's new id");
    ResetPlayState();
    TrackId oldFirst = AddTestTrack("Old first", "Pat", "Remap", 1, 1, 200, true);
    TrackId oldSecond = AddTestTrack("Old second", "Pat", "Remap", 1, 2, 200, true);
    ObservePlayback(oldFirst, 40, 20, 200);
    RemapTaste({oldSecond, oldFirst});
    auto remapped = score({oldFirst, oldSecond}, kInvalidTrack, afternoon);
    Check(remapped.size() == 2 && remapped[1] > remapped[0] * 1.5,
          L"after remapping, the rewind belongs to the new id (%.3f vs %.3f)",
          remapped.size() == 2 ? remapped[1] : 0, remapped.size() == 2 ? remapped[0] : 0);

    std::string broken;
    ByteWriter writer(broken);
    writer.U(2);
    writer.U(0);
    for (int i = 0; i < 8; ++i) writer.U(1);
    writer.U(0);
    for (int i = 0; i < 8; ++i) writer.U(1);
    writer.U(0);
    std::string empty = EncodeTaste({}, {0});
    std::string extra = empty;
    extra.push_back('\x1');
    Check(DecodeTaste(empty, 1, nullptr, nullptr) && !DecodeTaste(extra, 1, nullptr, nullptr) &&
              !DecodeTaste(broken, 4, nullptr, nullptr),
          L"an empty taste section loads, and trailing bytes or a repeated row reject it");
    std::string brokenParts = broken.substr(0, broken.size() - 1);  // the same two rows, no mood list
    std::string emptyParts = EncodeTasteParts({}, {0});
    std::string extraParts = emptyParts + '\x1';
    Check(DecodeTasteParts(emptyParts, 1, nullptr, nullptr) && !DecodeTasteParts(extraParts, 1, nullptr, nullptr) &&
              !DecodeTasteParts(brokenParts, 4, nullptr, nullptr),
          L"an empty part section loads, and trailing bytes or a repeated row reject it");
    // Part rows in the taste section date from when plain listening filled them; they are dropped.
    std::string legacy;
    ByteWriter legacyWriter(legacy);
    legacyWriter.U(1);
    legacyWriter.U(0);
    for (int i = 0; i < 8; ++i) legacyWriter.U(i ? 0 : 900);
    legacyWriter.U(1);
    legacyWriter.U(0);
    for (int value : {10, 20, 30, 40, 50, 60}) legacyWriter.U(value);
    TasteState legacyState;
    Check(DecodeTaste(legacy, 1, nullptr, &legacyState) && legacyState.parts.empty() && legacyState.moods.size() == 1,
          L"an older taste section keeps its mood sketch and drops its listening-filled part bins");
    ResetPlayState();
}
}  // namespace

// Play log and statistics: counted/skipped events, period stats, streaks, per-hour/weekday totals,
// top tracks/artists/albums, "on this day", save/load renumbering, and the plays-in-N-days rules.
void TestListeningModel() {
    testlog::Batch batch;
    auto original = PlayLog();
    SetPlayLog({});
    batch.Next(L"listening weights punish early skips, recover with counted listens and keep unknown "
               L"tracks equal");
    Check(ListeningWeights({0, 1, 2}) == std::vector<double>({1, 1, 1}), L"unknown tracks have equal weights");
    for (unsigned i = 0; i < 12; ++i) LogPlay(1, 1000 + i * 100, 5, false, true);
    LogPlay(2, 3000, 60, false, true);  // late skips don't train early-skip aversion
    auto weights = ListeningWeights({0, 1, 2});
    Check(weights[1] >= 0.05 && weights[1] < weights[0] * 0.1 && weights[2] == weights[0],
          L"early skips lower weight without excluding a track; late skips do not");
    for (unsigned i = 0; i < 30; ++i) LogPlay(1, 4000 + i * 300, 200, true, false);
    Check(ListeningWeights({1})[0] > weights[1] * 5, L"later counted listens recover a skipped track's weight");
    batch.Next(L"radio learns counted next-track transitions, excludes session gaps and breaks chains "
               L"at skips");
    SetPlayLog({{0, 1000, 200, PlayEvent::kCounted}, {1, 1200, 200, PlayEvent::kCounted},
                {0, 1400, 200, PlayEvent::kCounted}, {1, 1600, 200, PlayEvent::kCounted},
                {2, 9000, 200, PlayEvent::kCounted}, {0, 9200, 200, PlayEvent::kCounted},
                {3, 9205, 5, PlayEvent::kSkip}, {2, 9405, 200, PlayEvent::kCounted}});
    auto radio = ListeningWeights({0, 1, 2, 3}, 0);
    Check(radio[1] == 17 && radio[2] == 1 && radio[3] < 1,
          L"radio learns counted next-track transitions, excludes session gaps and breaks chains at skips");
    batch.Next(L"compaction rebuilds radio transitions with remapped track IDs");
    RemapPlayLog({2, 0, 1, UINT32_MAX});
    Check(ListeningWeights({0, 1}, 2)[0] == 17, L"compaction rebuilds radio transitions with remapped track IDs");
    batch.Next(L"a skip that ended the listening lowers nothing until another listen follows it");
    SetPlayLog({{0, 1000, 200, PlayEvent::kCounted}, {1, 1205, 5, PlayEvent::kSkip}, {0, 1407, 200, PlayEvent::kCounted},
                {2, 1612, 5, PlayEvent::kSkip}});
    auto ending = ListeningWeights({1, 2});
    LogPlay(3, 1900, 200, true, false);
    auto followed = ListeningWeights({1, 2});
    Check(ending[0] < 1 && ending[1] == 1 && followed[1] < 1,
          L"the last skip counts only once something follows it (%.3f, then %.3f)", ending[1], followed[1]);
    SetPlayLog(std::move(original));
}

void TestTaste(const std::wstring& scratch) { TestTasteImpl(scratch); }

void TestPlayLog(const std::wstring& scratch) {
    testlog::Batch batch;
    ResetPlayState();
    TrackId a = AddTestTrack("One", "Ann", "Alpha", 1, 1, 200, true);
    TrackId b = AddTestTrack("Two", "Ann", "Alpha", 1, 2, 200, true);
    TrackId c = AddTestTrack("Other", "Bob", "Beta", 1, 1, 100, true);
    batch.Next(L"PLAY LOG records counted plays and skips only, with listened seconds clamped");
    LogPlay(a, LocalAt(2026, 1, 5, 10), 200.4, true, false);
    LogPlay(b, LocalAt(2026, 1, 6, 11), 100, true, false);
    LogPlay(a, LocalAt(2026, 1, 7, 20), 210, true, false);
    LogPlay(b, LocalAt(2026, 1, 9, 8), 30, false, true);
    LogPlay(c, LocalAt(2026, 1, 9, 9), 0, false, false);  // neither counted nor skipped: not logged
    Check(PlayLog().size() == 4 && PlayLog()[3].flags == PlayEvent::kSkip && PlayLog()[0].seconds == 200,
          L"PLAY LOG logs counted plays and skips only, seconds clamped");

    batch.Next(L"PLAY LOG period totals, streaks and per-hour and weekday counts follow the logged events");
    PeriodStats s = ComputeStats(LocalAt(2026, 1, 1), LocalAt(2027, 1, 1), 5);
    Check(s.plays == 3 && s.skips == 1 && s.seconds == 540 && s.distinctTracks == 2 && s.discoveries == 2,
          L"PLAY LOG stats: plays %u skips %u seconds %llu tracks %u discoveries %u", s.plays, s.skips,
          (unsigned long long)s.seconds, s.distinctTracks, s.discoveries);
    Check(s.longestStreak == 3 && s.busiestDay == (uint32_t)LocalDay(LocalAt(2026, 1, 7, 12)) && s.busiestDaySeconds == 210,
          L"PLAY LOG streak of three days (Jan 5-7), busiest day is Jan 7 with 210 s");
    Check(s.byHour[10] == 1 && s.byHour[11] == 1 && s.byHour[20] == 1 && s.byHour[8] == 0 && s.byWeekday[0] == 1 &&
              s.byWeekday[1] == 1 && s.byWeekday[2] == 1,
          L"PLAY LOG hours and weekdays count counted plays only (Jan 5 2026 is a Monday)");
    batch.Next(L"PLAY LOG rankings of tracks, artists and albums respect their limit");
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

    batch.Next(L"PLAY LOG periods are [from, to), and tracks first played before them are not discoveries");
    PeriodStats part = ComputeStats(LocalAt(2026, 1, 6), LocalAt(2026, 1, 8), 5);
    Check(part.plays == 2 && part.skips == 0 && part.seconds == 310 && part.discoveries == 1,
          L"PLAY LOG periods are [from, to): Jan 6 to Jan 7, and Tracks first played before the period are not discoveries");

    batch.Next(L"PLAY LOG on this day counts earlier years only and lists the most played first");
    LogPlay(a, LocalAt(2025, 1, 5, 9), 100, true, false);
    LogPlay(c, LocalAt(2025, 1, 5, 21), 100, true, false);
    LogPlay(a, LocalAt(2024, 1, 5, 9), 200, true, false);
    LogPlay(b, LocalAt(2025, 1, 6, 9), 100, true, false);
    std::vector<TrackId> onThisDay = PlayedOnThisDay(LocalAt(2026, 1, 5, 12));
    Check(onThisDay == std::vector<TrackId>{a, c}, L"PLAY LOG on this day: earlier years only, most played first");

    batch.Next(L"PLAY LOG and points round trip retain tracks referenced only by listening history");
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

    batch.Next(L"LIBRARY DB v2 every truncated prefix of a file with sections is rejected");
    std::string valid = ReadTestBytes(file);
    bool truncations = !valid.empty();
    size_t loadedCount = g_lib.Count();
    for (size_t n = 0; n < valid.size(); ++n) {
        WriteTestBytes(file, std::string_view(valid).substr(0, n));
        truncations &= !LoadLibrary(file, none) && g_lib.Count() == loadedCount;
    }
    Check(truncations,
          L"LIBRARY DB v2 all %zu truncated prefixes with sections are rejected", valid.size());

    batch.Next(L"POINTS a library saved before back pay stays uninitialized");
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
    batch.Next(L"PLAY LOG plays-in-30-and-365-days rules count the plays in each window per track");
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
    testlog::Batch batch;

    // Collection: once per track, a new album once per album (case-insensitive).
    ResetPlayState();
    TrackId t1 = AddTestTrack("Alpha One", "Ann", "Alpha", 1, 1, 200, true);
    TrackId t2 = AddTestTrack("Alpha Two", "Ann", "Alpha", 1, 2, 200, true);
    TrackId xb = AddTestTrack("Beta One", "Bob", "Beta", 1, 1, 100, true);
    TrackId z = AddTestTrack("Alpha Six", "Ann", "Alpha", 1, 6, 200, false);
    TrackId w = AddTestTrack("Alpha Caps", "ANN", "alpha", 1, 3, 200, true);
    uint32_t t0 = LocalAt(2026, 3, 1, 9);
    batch.Next(L"POINTS collection awards once per track and once per album, ignoring case");
    PointsAward added = AwardLibraryAddsAt(t0, {t1, t2, xb, z});
    Check(added.centi == 4600 && added.events.size() == 5 && added.events[1].kind == PointsKind::NewAlbum &&
              added.events[4].kind == PointsKind::NewAlbum,
          L"POINTS collection: 2 each, 20 for a new album, 4600 centipoints for three tracks and two albums");
    Check(AwardLibraryAddsAt(t0, {t1, t2, xb, z}).centi == 0, L"POINTS a track is collected only once");
    Check(AwardLibraryAddsAt(t0, {w}).centi == 200 && PointsTotal() == 4800,
          L"POINTS a second track of an album (any case) is not a new album");
    batch.Next(L"POINTS level thresholds, ranks and the next rank follow the published curve");
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
    batch.Next(L"POINTS a first listen, a second track and a replay earn listening, play and "
               L"discovery points");
    PointsAward a1 = ListenTest(LocalAt(2026, 3, 2, 9), s[0], 1, 200, true, false, 0);
    Check(a1.centi == 2167 && a1.events.size() == 3 && a1.events[2].kind == PointsKind::Discovery,
          L"POINTS 200 s listened: 667 listening + 500 play + 1000 discovery");
    PointsAward a2 = ListenTest(LocalAt(2026, 3, 2, 10), s[1], 1, 200, true, false, 0);
    Check(a2.centi == 2167, L"POINTS a second track the same day");
    PointsAward a3 = ListenTest(LocalAt(2026, 3, 2, 10, 30), s[0], 2, 200, true, false, 0);
    Check(a3.centi == 1567 && PointsStreak(LocalAt(2026, 3, 2, 11)) == 1,
          L"POINTS a replay: 667 + 900; ten minutes make the day count (streak 1)");
    batch.Next(L"POINTS the day streak multiplies awards, and a missed day resets it");
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

    batch.Next(L"POINTS a long day streak raises the multiplier to 1.5 and ends after a missed day");
    // Eleven counting days in a row: streak 11, multiplier 1.5.
    for (int day = 1; day <= 11; ++day)
        ListenTest(LocalAt(2026, 4, day, 9), s[5], (uint32_t)day, 600, true, false, 0);
    Check(PointsStreak(LocalAt(2026, 4, 12, 9)) == 11 && PointsMultiplier(LocalAt(2026, 4, 12, 9)) == 1.5,
          L"POINTS eleven counting days: streak 11, multiplier 1.5");
    PointsAward a12 = ListenTest(LocalAt(2026, 4, 12, 9), s[5], 12, 600, true, false, 0);
    Check(a12.centi == 5850, L"POINTS at x1.5: 600 s = 3000 listening, play 1900 -> 2850");
    Check(PointsStreak(LocalAt(2026, 4, 14, 9)) == 0 && PointsMultiplier(LocalAt(2026, 4, 14, 9)) == 1.0,
          L"POINTS the streak ends after a missed day (Apr 13), even after twelve days");

    batch.Next(L"POINTS fatigue halves the play points from the eleventh play of a day until midnight");
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
    batch.Next(L"POINTS an album run awards the fourth track and the whole album in order");
    uint32_t j = LocalAt(2026, 6, 1, 10);
    std::vector<PointsAward> run;
    for (int i = 0; i < 5; ++i) run.push_back(ListenTest(j + i * 300, al[i], 2, 200, true, false, 0));
    Check(run[0].centi == 1567 && run[1].centi == 1567 && run[2].centi == 1567, L"POINTS album run of 1 to 3 tracks: nothing extra");
    Check(run[3].centi == 4567 && run[3].events.size() == 3 && run[3].events[2].kind == PointsKind::AlbumRun,
          L"POINTS album run of 4 tracks: +30");
    Check(run[4].centi == 7067 && run[4].events.size() == 4 && run[4].events[2].kind == PointsKind::AlbumRun &&
              run[4].events[2].centi == 500 && run[4].events[3].kind == PointsKind::FullAlbum && run[4].events[3].centi == 5000,
          L"POINTS full album run: +5 for the fifth track and +50 for the whole album");
    batch.Next(L"POINTS an out-of-order track or another album breaks and restarts a run");
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
    batch.Next(L"POINTS a skipped track earns only its listening time");
    PointsAward skipped = ListenTest(j + 3600, al[4], 3, 30, false, true, 0);
    Check(skipped.centi == 100 && skipped.events.size() == 1, L"POINTS a skip earns only its listening (30 s = 100)");
    batch.Next(L"POINTS an Inbox-only album run does not earn the full-album bonus");
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

    batch.Next(L"POINTS a counted play after 365 days is a rediscovery, not before");
    // Rediscovery: a counted play after 365 days or more.
    ResetPlayState();
    TrackId old = AddTestTrack("Old", "Ola", "", 0, 0, 200, true);
    uint32_t now = LocalAt(2026, 6, 10, 9);
    PointsAward rediscovered = ListenTest(now, old, 5, 200, true, false, now - 400 * 86400u);
    PointsAward notYet = ListenTest(now, old, 6, 200, true, false, now - 364 * 86400u);
    Check(rediscovered.centi == 3567 && rediscovered.events.back().kind == PointsKind::Rediscovery && notYet.centi == 2167,
          L"POINTS rediscovery after 365 days: +15, not before");

    batch.Next(L"POINTS back pay is granted once, for what the existing stats are worth");
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
    testlog::Batch batch;
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
        tr.firstPlayed = tr.lastPlayed;
        tr.sampleRate = ids.empty() ? 96000 : 44100;
        tr.channels = s.seconds == 3600 ? 1 : 2;
        tr.rgTrackGain = ids.empty() ? -750 : ids.size() == 1 ? 0 : kNoGain;  // Alpha -7.5 dB, Beta 0 dB
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
    batch.Next(L"AUTOPLAYLIST rules on every field kind, nested ALL/ANY groups and limits select the "
               L"expected tracks, and invalid intervals select none");
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
    a.root.rules = {{RuleField::SampleRate, RuleOp::AtLeast, "88.2", ""}};
    check(L"sample rate >= 88.2 kHz", a, {L"Alpha"});
    a.root.rules = {{RuleField::Channels, RuleOp::Equals, "1", ""}};
    check(L"mono", a, {L"Epsilon"});
    a.root.rules = {{RuleField::FileSize, RuleOp::AtLeast, "300", ""}};
    check(L"file size >= 300 MB", a, {L"Epsilon"});
    a.root.rules = {{RuleField::TrackGain, RuleOp::IsEmpty, "", ""}};
    check(L"unmeasured track gain (0 dB counts as measured)", a, {L"Delta", L"Epsilon", L"Gamma"});
    a.root.rules = {{RuleField::TrackGain, RuleOp::AtMost, "-5", ""}};
    check(L"track gain <= -5 dB skips unmeasured tracks", a, {L"Alpha"});
    a.root.rules = {{RuleField::FirstPlayed, RuleOp::Never, "", ""}};
    check(L"never first played", a, {L"Gamma"});
    {
        // Twenty days ago as a local calendar date: tracks added 1-10 days ago are on or after it.
        std::time_t then = (std::time_t)now - 20 * 86400;
        std::tm local{};
        localtime_s(&local, &then);
        char day[16];
        std::strftime(day, sizeof day, "%Y-%m-%d", &local);
        a.root.rules = {{RuleField::DateAdded, RuleOp::OnOrAfter, day, ""}};
        check(L"added on or after a date", a, {L"Alpha", L"Epsilon", L"Gamma"});
        a.root.rules = {{RuleField::DateAdded, RuleOp::Before, day, ""}};
        check(L"added before a date", a, {L"Beta", L"Delta"});
        a.root.rules = {{RuleField::LastPlayed, RuleOp::Before, "2200", ""}};
        check(L"an invalid date selects nothing", a, {});
        a.root.rules = {{RuleField::LastPlayed, RuleOp::Before, std::to_string(local.tm_year + 1901), ""}};
        check(L"a year means its first day; never-played tracks have no date", a,
              {L"Alpha", L"Beta", L"Delta", L"Epsilon"});
    }
    uint32_t day = 0;
    rules.Add(ParseRuleDate("2026", day) && ParseRuleDate("2026-03", day) && ParseRuleDate(" 2026/3/14 ", day) &&
                  !ParseRuleDate("2026-02-31", day) && !ParseRuleDate("2026-13", day) && !ParseRuleDate("26-1-1", day) &&
                  !ParseRuleDate("2026-", day) && !ParseRuleDate("yesterday", day) && !ParseRuleDate("", day),
              L"rule dates accept YYYY[-MM[-DD]] and reject impossible or partial dates");
    {
        AutoPlaylist saved, loaded;
        saved.root.rules = {{RuleField::FirstPlayed, RuleOp::OnOrAfter, "2026-01-01", ""},
                            {RuleField::TrackGain, RuleOp::Between, "-9", "-6"}};
        rules.Add(ParseAutoPlaylist(SerializeAutoPlaylist(saved), loaded) && loaded.root.rules.size() == 2 &&
                      loaded.root.rules[0].field == RuleField::FirstPlayed && loaded.root.rules[0].op == RuleOp::OnOrAfter &&
                      loaded.root.rules[1].field == RuleField::TrackGain && loaded.root.rules[1].value2 == "-6",
                  L"new fields and date operators survive saving");
    }
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

    batch.Next(L"AUTOPLAYLIST text round trip keeps the rules, and unknown rule fields are skipped");
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
