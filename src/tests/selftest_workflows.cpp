// Seeded properties and feature workflows use independent reference results, not implementation
// details. Failure messages include the seed and case so a rare permutation can be reproduced.
#include "selftest_common.h"
#include "playlist_shuffle.h"
#include "sort.h"
#include <memory>
#include <numeric>

namespace {
struct Random {
    uint32_t value = TestSeed();
    uint32_t Next(uint32_t bound) { value = value * 1664525u + 1013904223u; return bound ? value % bound : 0; }
};
struct LibraryScope {
    Library original = std::move(g_lib);
    LibraryScope() { g_lib = Library(); }
    ~LibraryScope() { g_lib = std::move(original); }
};
}

void TestSortingProperties() {
    struct Owned { unsigned key, serial; std::unique_ptr<std::string> text; };
    Random random;
    testlog::Cases owning, records, albumKeys;
    for (unsigned trial = 0; trial < 120; ++trial) {
        const size_t size = trial < 3 ? trial : random.Next(180);
        std::vector<Owned> actual;
        std::vector<std::pair<unsigned, unsigned>> expected;
        for (unsigned i = 0; i < size; ++i) {
            unsigned key = random.Next(11);
            actual.push_back({key, i, std::make_unique<std::string>(300, (char)('a' + i % 26))});
            expected.emplace_back(key, i);
        }
        bool descending = (trial & 1) != 0;
        std::stable_sort(expected.begin(), expected.end(), [&](auto a, auto b) {
            return descending ? a.first > b.first : a.first < b.first;
        });
        SortObjects(actual.begin(), actual.end(), [&](const Owned& a, const Owned& b) {
            return descending ? a.key > b.key : a.key < b.key;
        });
        bool equal = actual.size() == expected.size();
        for (size_t i = 0; i < size; ++i)
            equal &= actual[i].key == expected[i].first && actual[i].serial == expected[i].second && actual[i].text &&
                *actual[i].text == std::string(300, (char)('a' + actual[i].serial % 26));
        const std::wstring name = L"case " + std::to_wstring(trial) + L" size " + std::to_wstring(size);
        owning.Add(equal, name + (descending ? L" descending" : L""));
        std::vector<unsigned> numbers(size), reference;
        for (auto& number : numbers) number = random.Next(9) ? random.Next(31) : UINT32_MAX;  // the largest key too
        reference = numbers;
        std::sort(reference.begin(), reference.end());
        SortRecords(numbers.begin(), numbers.end());
        records.Add(numbers == reference, name);
        AlbumId a{random.Next(3), random.Next(5), trial % 5 ? random.Next(UINT32_MAX) : UINT32_MAX};
        AlbumId b{random.Next(3), random.Next(5), random.Next(UINT32_MAX)};
        if (trial % 7 == 0) b = a;
        const auto fieldsA = std::tie(a.artist, a.album, a.date), fieldsB = std::tie(b.artist, b.album, b.date);
        albumKeys.Add((a <=> b) == (fieldsA <=> fieldsB) && (a < b) == (fieldsA < fieldsB) &&
                      (a == b) == (fieldsA == fieldsB), name);
    }
    Check(owning.Passed(), L"owning stable sort matches the reference and retains payloads in 120 seeded cases: seed=%u%ls",
        TestSeed(), owning.failed.c_str());
    Check(records.Passed(), L"record sort preserves all values in 120 seeded cases: seed=%u%ls", TestSeed(), records.failed.c_str());
    Check(albumKeys.Passed(), L"album keys preserve tuple ordering and equality in 120 seeded cases: seed=%u%ls",
          TestSeed(), albumKeys.failed.c_str());
    std::vector<std::wstring> words{L"sentinel-first", L"c", L"a", L"b", L"sentinel-last"};
    bool nested = false;
    SortObjects(words.begin() + 1, words.end() - 1, [&](const auto& a, const auto& b) {
        if (!nested) {
            std::vector<int> inner{3, 1, 2};
            SortRecords(inner.begin(), inner.end());
            nested = inner == std::vector<int>{1, 2, 3};
        }
        return a < b;
    });
    Check(nested && words == std::vector<std::wstring>{L"sentinel-first", L"a", L"b", L"c", L"sentinel-last"},
        L"nested comparators keep their context and partial sorts leave surrounding objects untouched");

    LibraryScope scope;
    std::vector<StringPool::Handle> handles{0};
    for (unsigned i = 0; i < 400; ++i) {
        const std::wstring prefix = i % 3 == 0 ? L"雪 " : i % 3 == 1 ? L"Björk " : L"Album ";
        handles.push_back(g_lib.InternW(prefix + std::to_wstring(i)));
        if (i % 5 == 0) handles.push_back(handles.back());
    }
    handles.push_back(g_lib.Intern(std::string(18000, 'x')));
    app::StringRanks ranks(handles);
    bool natural = ranks(0) < UINT32_MAX && ranks(g_lib.Intern("absent")) == UINT32_MAX;
    for (auto a : handles) for (auto b : handles) {
        const auto sa = g_lib.Str(a), sb = g_lib.Str(b);
        int comparison = sa.empty() != sb.empty() ? (sa.empty() ? 1 : -1) : StrCmpLogicalW(sa.c_str(), sb.c_str());
        natural &= comparison < 0 ? ranks(a) < ranks(b) : comparison > 0 ? ranks(a) > ranks(b) : true;
    }
    Check(natural, L"string ranks preserve natural Unicode order, empty-last values, repeated handles and large tags");

    for (unsigned i = 0; i < 8; ++i) {
        auto id = g_lib.Add(L"C:\\Album shuffle\\" + std::to_wstring(i >= 5 ? i / 2 : 0) + L"\\" + std::to_wstring(i) + L".flac");
        auto& t = g_lib[id];
        t.artist = g_lib.Intern(i == 2 ? "Guest" : "Artist");
        t.albumArtist = g_lib.Intern(i == 4 ? "Other artist" : "Various artists");
        t.album = g_lib.Intern("Shared title");
        t.date = g_lib.Intern(i == 3 ? "2025" : "2026");
        t.disc = i == 0 ? 2 : 1;
        t.trackNo = i == 2 || i == 6 ? 2 : 1;
        if (i >= 5) t.album = t.albumArtist = t.artist = t.date = 0;
    }
    const std::vector<TrackId> items{0, 4, 2, 3, 1, 2, 5, 7, 6};
    const std::vector<std::vector<uint32_t>> albums{{4, 2, 5, 0}, {3}, {1}, {6}, {7, 8}};
    testlog::Cases albumShuffle;
    std::mt19937 rng(TestSeed());
    std::vector<uint32_t> initial;
    bool varied = false;
    for (unsigned trial = 0; trial < 40; ++trial) {
        int current = (int)(trial % 10) - 1;
        std::vector<uint32_t> order;
        ShuffleAlbums(order, items, rng, current);
        bool valid = order.size() == items.size();
        std::vector<bool> seen(albums.size());
        for (size_t at = 0; valid && at < order.size();) {
            size_t album = 0;
            while (album < albums.size() && albums[album].front() != order[at]) ++album;
            valid = album < albums.size() && !seen[album];
            if (!valid) break;
            const auto& expected = albums[album];
            valid = at + expected.size() <= order.size() && std::equal(expected.begin(), expected.end(), order.begin() + at);
            if (at == 0 && current >= 0)
                valid &= std::find(expected.begin(), expected.end(), (uint32_t)current) != expected.end();
            seen[album] = true;
            at += expected.size();
        }
        albumShuffle.Add(valid, L"seeded pass " + std::to_wstring(trial));
        if (current < 0) {
            if (initial.empty()) initial = order;
            else varied |= initial != order;
        }
    }
    std::vector<uint32_t> empty{99};
    ShuffleAlbums(empty, {}, rng, -1);
    albumShuffle.Add(varied && empty.empty(), L"varied album orders and empty playlist");
    Check(albumShuffle.Passed(), L"album shuffle keeps compilations, dates, artists, discs, folders and duplicate slots intact%ls",
          albumShuffle.failed.c_str());
}

void TestSearchProperties() {
    Random random;
    Library library;
    const char* vocabulary[] = {"Quiet", "LOUD", "Björk", "雪", "Two Words", "Live 2026", "", "café"};
    for (unsigned i = 0; i < 160; ++i) {
        TrackId id = library.Add(L"C:\\Seeded search\\" + std::to_wstring(i) + L".flac");
        TagData tags;
        tags.title = vocabulary[random.Next(8)]; tags.artist = vocabulary[random.Next(8)];
        tags.album = vocabulary[random.Next(8)]; tags.comment = vocabulary[random.Next(8)];
        tags.genre = vocabulary[random.Next(8)]; tags.codec = Codec::FLAC;
        library.ApplyTags(id, tags);
    }
    const wchar_t* queries[] = {L"", L"quiet", L"LOUD", L"BJÖRK", L"雪", L"\"Two Words\"", L"2026", L"café",
        L"flac", L"seeded", L"quiet loud", L"absent-value", L"\"Live 2026\" 雪", L"quiet\u2003LOUD"};
    testlog::Cases cases;
    for (unsigned trial = 0; trial < 60; ++trial) {
        const wchar_t* query = queries[random.Next((uint32_t)std::size(queries))];
        auto words = FilterWords(query);
        TrackMatcher matcher(library, words);
        bool same = true;
        for (unsigned i = 0; i < 240; ++i) {
            TrackId id = random.Next((uint32_t)library.Count());
            same &= matcher.Matches(id) == ReferenceSearch(library, id, words);
        }
        cases.Add(same, L"case " + std::to_wstring(trial) + L" '" + query + L"'");
    }
    Check(cases.Passed(), L"cached search equals independent field lookup after reordered/repeated tracks in 60 seeded "
        L"cases: seed=%u%ls", TestSeed(), cases.failed.c_str());
    const std::vector<std::wstring> replacement{L"replaced"};
    TrackMatcher matcher(library, replacement);
    bool absent = !matcher.Matches(0);
    TagData tags; tags.title = "Replaced";
    library.ApplyTags(0, tags);
    Check(absent && matcher.Matches(0), L"cached search notices metadata edited on an already visited track");
}

void TestPlaylistFiles(const std::wstring& scratch) {
    LibraryScope restore;
    TrackId a = g_lib.Add(scratch + L"\\雪 and spaces.flac"), b = g_lib.Add(scratch + L"\\second.mp3");
    TagData tags; tags.title = "Title\n#not another entry"; tags.artist = "Björk\r\nArtist"; tags.duration = 2;
    g_lib.ApplyTags(a, tags);
    Playlist playlist{L"Mixed", {a, b, a}};
    const auto file = scratch + L"\\export.m3u8";
    Check(ExportM3U(file, playlist), L"Unicode playlist with duplicate occurrences exports successfully");
    std::vector<std::wstring> paths{L"existing"};
    Check(ImportM3U(file, paths) && paths == std::vector<std::wstring>{L"existing", g_lib.Path(a), g_lib.Path(b), g_lib.Path(a)},
        L"M3U round trip preserves order, duplicates and Unicode; metadata cannot insert entries");
    const std::string original = ReadTestBytes(file);
    playlist.items.push_back(kInvalidTrack);
    Check(!ExportM3U(file, playlist) && ReadTestBytes(file) == original,
        L"export with an invalid track leaves the prior playlist byte-identical");
    playlist.items.pop_back();
    HANDLE locked = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    Check(locked != INVALID_HANDLE_VALUE && !ExportM3U(file, playlist) && ReadTestBytes(file) == original,
        L"a locked playlist refuses replacement and retains every original byte");
    if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
    WIN32_FIND_DATAW found{};
    HANDLE temporary = FindFirstFileW((file + L".tmp.*").c_str(), &found);
    Check(temporary == INVALID_HANDLE_VALUE, L"failed playlist replacement cleans its temporary output");
    if (temporary != INVALID_HANDLE_VALUE) FindClose(temporary);
    Check(ExportM3U(file, playlist), L"playlist export succeeds after the lock is released");
    std::vector<M3UExportEntry> snapshot;
    Playlist changed = playlist;
    const auto captured = scratch + L"\\captured.m3u8";
    bool saved = CaptureM3U(changed, snapshot);
    changed.items.clear();
    paths.clear();
    Check(saved && ExportM3U(captured, snapshot) && ImportM3U(captured, paths) &&
          paths == std::vector<std::wstring>{g_lib.Path(a), g_lib.Path(b), g_lib.Path(a)},
          L"a captured playlist export keeps its original order and duplicates when the playlist changes");

    const auto relative = scratch + L"\\relative.m3u8";
    const std::string localUrl = "file:///" + ToUtf8(g_lib.Path(a));
    Ready(WriteTestBytes(relative, "#EXTM3U\r\n \"second.mp3\" \r\nhttps://example.invalid/song.mp3\n" + localUrl + "\n"),
        L"relative path and file URL fixture is written");
    paths.clear();
    Check(ImportM3U(relative, paths) && paths == std::vector<std::wstring>{g_lib.Path(b), g_lib.Path(a)},
        L"playlist-relative paths and file URLs resolve locally while network entries are ignored");
    paths = {L"sentinel"};
    Check(WriteTestBytes(relative, "second.mp3\n" + std::string("bad\0name.flac", 13)) &&
        !ImportM3U(relative, paths) && paths == std::vector<std::wstring>{L"sentinel"},
        L"invalid input after a valid entry never appends partial import results");
}

// An imported folder that no library folder covers is remembered once, so Library > Watch imported
// folders can offer to keep it up to date; one inside a library folder is already watched.
void TestImportedFolders(const std::wstring& scratch) {
    const auto folders = app::g.libraryFolders, imported = app::g.importedFolders;
    app::g.libraryFolders = {scratch + L"\\watched"};
    app::g.importedFolders.clear();
    const bool covered = !app::NoteUnwatchedFolders({scratch + L"\\watched\\Album", scratch + L"\\watched"});
    const bool noted = app::NoteUnwatchedFolders({scratch + L"\\new\\", scratch + L"\\NEW"});
    Check(covered && noted && app::g.importedFolders == std::vector<std::wstring>{scratch + L"\\new"},
          L"imported folders outside the library folders are offered for watching, once each");
    app::g.libraryFolders = folders;
    app::g.importedFolders = imported;
}

void TestLibraryWorkflow(const std::wstring& scratch) {
    LibraryScope restore;
    const std::wstring db = scratch + L"\\workflow.cdb", exported = scratch + L"\\favorites.m3u8";
    // An unreferenced track forces the save to compact IDs. Comparisons below use paths.
    g_lib.Add(scratch + L"\\not-retained.flac");
    std::vector<std::wstring> expected;
    std::vector<TrackId> ids;
    for (unsigned i = 0; i < 12; ++i) {
        TrackId id = g_lib.Add(scratch + L"\\曲 " + std::to_wstring(i) + L".flac");
        TagData tags; tags.title = "Song " + std::to_string(i); tags.artist = "Workflow Artist";
        tags.album = "Workflow Album"; tags.codec = Codec::FLAC; tags.duration = 90 + i;
        g_lib.ApplyTags(id, tags);
        g_lib[id].flags |= Track::kInLibrary; g_lib[id].rating = i % 2 ? 100 : 40; g_lib[id].playCount = i;
        ids.push_back(id);
        if (i % 2) expected.push_back(g_lib.Path(id));
    }
    AutoPlaylist favorites; favorites.root.rules.push_back({RuleField::Rating, RuleOp::AtLeast, "4", {}});
    std::vector<TrackId> before;
    EvaluateAutoPlaylist(favorites, before, TestSeed());
    std::vector<Playlist> playlists{{L"Library", {}, PlaylistKind::Library}, {L"Duplicates", {ids[5], ids[1], ids[5]}},
        {L"Favorites", {}, PlaylistKind::Auto}};
    playlists[2].rules = SerializeAutoPlaylist(favorites);
    playlists[1].position.selected = {2}; playlists[1].position.focus = 2;
    Check(before.size() == expected.size() && SaveLibrary(db, playlists),
        L"rated library, static duplicate occurrences and automatic rules save together");
    g_lib = Library(); playlists.clear();
    if (!LoadLibrary(db, playlists) || playlists.size() != 3) {
        Check(false, L"saved workflow reopens all three playlists"); return;
    }
    AutoPlaylist loaded;
    bool parsed = ParseAutoPlaylist(playlists[2].rules, loaded);
    std::vector<TrackId> after;
    EvaluateAutoPlaylist(loaded, after, TestSeed());
    std::vector<std::wstring> actual;
    for (TrackId id : after) actual.push_back(g_lib.Path(id));
    Check(parsed && actual == expected && g_lib.Count() == 12,
        L"saved automatic rules still select the same paths after track-ID compaction");
    Check(playlists[1].items.size() == 3 && playlists[1].items[0] == playlists[1].items[2] &&
        g_lib.Path(playlists[1].items[1]) == scratch + L"\\曲 1.flac" &&
        playlists[1].position.selected == std::vector<TrackId>{2} && playlists[1].position.focus == 2,
        L"duplicate occurrence selection survives compaction and reopening");
    TrackId edited = g_lib.Find(expected.front());
    g_lib[edited].rating = 0; g_lib[edited].playCount += 9; g_lib.Touch();
    EvaluateAutoPlaylist(loaded, after, TestSeed());
    Playlist exportList{L"Favorites", after};
    std::vector<std::wstring> imported;
    Check(after.size() + 1 == expected.size() && ExportM3U(exported, exportList) && ImportM3U(exported, imported) &&
        imported.size() == after.size() && std::find(imported.begin(), imported.end(), expected.front()) == imported.end(),
        L"rating edits rebuild automatic results and export only the current matching tracks");
    Check(SaveLibrary(db, playlists), L"edited workflow saves a second time");
    g_lib = Library(); playlists.clear();
    bool reopened = LoadLibrary(db, playlists);
    edited = g_lib.Find(expected.front());
    Check(reopened && g_lib.Contains(edited) && g_lib[edited].rating == 0 && g_lib[edited].playCount == 10,
        L"rating and listening edits survive a second close and reopen");
}
