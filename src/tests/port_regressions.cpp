// Regression coverage for improvements recovered from the Waveforge-updated working tree.
// Called from selftest.cpp: TestPortedStorage guards storage/playlist robustness, TestPortedUi
// guards tab, playback and background-job behaviour. Every check runs in a testlog group and writes
// only scratch files, under a hidden window, so no user state is touched.
#include "app.h"
#include "playlog.h"
#include "testlog.h"

#include <limits>
#include <stdexcept>

namespace {
using testlog::Check;

// Writes raw bytes to a scratch file (CREATE_ALWAYS), for building fixtures in place.
bool WriteBytes(const std::wstring& path, const std::string& bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(file, bytes.data(), (DWORD)bytes.size(), &written, nullptr) && written == bytes.size();
    CloseHandle(file);
    return ok;
}

// Pumps the message queue until `done` returns true or 10 s elapse, so worker threads that post
// messages can make progress while the test waits.
bool PumpUntil(const std::function<bool()>& done) {
    ULONGLONG start = GetTickCount64();
    do {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (done()) return true;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    } while (GetTickCount64() - start < 10000);
    return false;
}

}

// Storage and playlist protections: string-pool bounds/aliases, metadata clamping, CORDINAL3 and
// legacy playlist import, M3U encoding, saved per-tab view state, and title-format nesting.
void TestPortedStorage(const std::wstring& directory) {
    testlog::Group group(L"recovered storage and playlist protections");
    Library previous = std::move(g_lib);
    g_lib = Library();
    {
        TrackId id=g_lib.Add(L"C:\\Search\\Música\\Track.flac");
        TagData tags;
        tags.title="Quiet Track"; tags.artist="Björk"; tags.album="Live 2026";
        tags.genre="Rock"; tags.comment="東京"; tags.codec=Codec::FLAC;
        g_lib.ApplyTags(id,tags);
        testlog::Cases years;
        for (auto [date, positive] : {std::pair{"9999999999999999999", true},
                                     std::pair{"-9999999999999999999", false},
                                     std::pair{"2026-10-25", true}}) {
            tags.date = date;
            g_lib.ApplyTags(id, tags);
            Rule rule{RuleField::Year, positive ? RuleOp::AtLeast : RuleOp::AtMost, positive ? "2026" : "0"};
            years.Add(RuleMatches(rule, id), FromUtf8(date));
        }
        Check(years.Passed(), L"numeric year prefixes retain their sign and ordering when tags exceed 64 bits%ls",
              years.failed.c_str());
        tags.date.clear();
        g_lib.ApplyTags(id, tags);
        g_lib=Library();
    }
    StringPool pool;
    std::string longText(20000, 'a');
    auto first = pool.Intern(longText);
    auto borrowed = pool.Get(first);
    for (int i = 0; i < 100; ++i) pool.Intern(longText + std::to_string(i));
    Check(borrowed == longText && pool.Get(first) == longText, L"large pooled string views survive later insertions");
    Check(pool.Get(UINT32_MAX).empty() && pool.Get(0x7FFFFFFF).empty(), L"invalid pool handles are rejected");
    StringPool compactPool;
    for (int i = 0; i < 47; ++i) compactPool.Intern(std::to_string(i));
    size_t bytes = compactPool.Bytes();
    compactPool.Intern("0");
    Check(compactPool.Bytes() == bytes, L"duplicate interning does not grow the string hash table");
    bool bounded = false;
    try { pool.Intern(std::string((1u << 20) + 1, 'x')); }
    catch (const std::length_error&) { bounded = true; }
    Check(bounded, L"library string sizes are bounded");
    std::wstring path = directory + L"\\Case-Test.flac";
    TrackId id = g_lib.Add(path);
    Check(g_lib.Add(directory + L"\\case-test.FLAC") == id && g_lib.Find(L"\\\\?\\" + path) == id,
          L"case and extended-path aliases share one track");
    size_t strings = g_lib.StringCount();
    Check(g_lib.Find(directory + L"\\absent.flac") == kInvalidTrack && g_lib.StringCount() == strings,
          L"unsuccessful path lookups allocate no strings");
    Check(g_lib.Add(L"") == kInvalidTrack && g_lib.Add(L"bad\npath.flac") == kInvalidTrack &&
          g_lib.Path(UINT32_MAX).empty(), L"invalid paths and track lookups fail safely");
    TagData tags;
    tags.title = std::string(70000, 't');
    tags.duration = NAN;
    tags.rgTrackGain = 1e30f;
    g_lib.ApplyTags(id, tags);
    Check(g_lib.View(g_lib[id].title).size() == 65536 && !g_lib[id].durationMs && g_lib[id].rgTrackGain == 32000,
          L"metadata text, duration and gain conversions are bounded");
    g_lib[id].flags |= Track::kRgScanned;
    g_lib[id].rgTrackGain = -650;
    tags.rgTrackGain = 3;
    g_lib.ApplyTags(id, tags);
    Check(g_lib[id].rgTrackGain == -650, L"late metadata cannot replace measured ReplayGain");
    Check(FilterWords(L"\"Test artist\"\u2003album") == std::vector<std::wstring>{L"Test artist", L"album"},
          L"search supports phrases and Unicode whitespace");
    Check(CleanPlaylistName(L"  A\nB  ") == L"A B" && CleanPlaylistName(std::wstring(200, L'a')).size() == 128,
          L"playlist names are trimmed, sanitized and bounded");
    std::vector<Playlist> lists{{L"Mix", {id}}};
    Check(::UniquePlaylistName(lists, L"mix") == L"mix (2)", L"playlist names are unique without regard to case");
    std::wstring cache = directory + L"\\recovered-v3.wfl";
    std::string escapedPath;
    for (char c : ToUtf8(path)) { if (c == '\\') escapedPath += '\\'; escapedPath += c; }
    std::string fields = "\tArtist\tImported\tAlbum\tArtist\t2026\tGenre\t1\t3\t7\t900\t48000\t2\t90000\t-650\t20000\t-700\t20000\n";
    std::string record = "T\t" + ToUtf8(path) + fields;
    Check(WriteBytes(cache, "CORDINAL3\nT\t" + escapedPath + fields + "P\tRecovered\nI\t0\nI\t0\nV\tImported\t6\t0\t1\t1\nS\t1\n") &&
          LoadPlaylists(cache, lists) && lists.size() == 1 && lists[0].items.size() == 2 &&
          g_lib.Path(lists[0].items[0]) == path &&
          lists[0].position.selected == std::vector<TrackId>{1} && lists[0].position.search == L"Imported" &&
          lists[0].sortColumn == 6, L"CORDINAL3 imports cached tracks, duplicate occurrences and saved views");
    size_t count = g_lib.Count();
    Check(WriteBytes(cache, "CORDINAL3\nP\tBroken\nI\t99\n") && !LoadPlaylists(cache, lists) &&
          lists[0].name == L"Recovered" && g_lib.Count() == count, L"bad cache references leave live state untouched");
    bool legacy = true;
    for (const char* header : {"CORDINAL1", "CORDINAL2", "WAVEFORGE1", "WAVEFORGE2"})
        legacy &= WriteBytes(cache, std::string(header) + "\nP\tLegacy\n" + record) && LoadPlaylists(cache, lists) &&
                  lists[0].items.size() == 1;
    Check(legacy, L"legacy CORDINAL1/2 and WAVEFORGE1/2 playlists remain readable");
    std::wstring m3u = directory + L"\\unicode.m3u8";
    bool spansWindows = true, atomicImports = true;
    for (bool bigEndian : {false, true}) {
        std::wstring text = L"#" + std::wstring(40000, L'x') + L"\n\"" + path + L"\"\n";
        std::string encoded = bigEndian ? "\xFE\xFF" : "\xFF\xFE";
        for (wchar_t c : text) {
            encoded += (char)(bigEndian ? c >> 8 : c & 255);
            encoded += (char)(bigEndian ? c & 255 : c >> 8);
        }
        std::vector<std::wstring> paths;
        spansWindows &= WriteBytes(m3u, encoded) && ImportM3U(m3u, paths) && paths == std::vector<std::wstring>{path};
        encoded += '\0';
        paths = {L"sentinel"};
        atomicImports &= WriteBytes(m3u, encoded) && !ImportM3U(m3u, paths) && paths == std::vector<std::wstring>{L"sentinel"};
    }
    Check(spansWindows, L"UTF-16 LE and BE playlist lines span input windows");
    Check(atomicImports, L"truncated UTF-16 imports are atomic");
    std::vector<std::wstring> paths;
    Check(WriteBytes(m3u, "good.flac\n\xFF\n") && !ImportM3U(m3u, paths) && paths.empty(),
          L"M3U8 rejects malformed UTF-8 without partial results");
    lists = {{L"Saved view", {id, id}}};
    lists[0].sortColumn = 6;
    lists[0].sortAscending = false;
    lists[0].position.search = L"query";
    lists[0].position.selected = {1};
    lists[0].position.focus = lists[0].position.top = 1;
    std::wstring database = directory + L"\\recovered-views.cdb";
    std::vector<Playlist> restored;
    Check(SaveLibrary(database, lists) && LoadLibrary(database, restored) && restored.size() == 1 &&
          restored[0].sortColumn == 6 && !restored[0].sortAscending && restored[0].position.search == L"query" &&
          restored[0].position.selected == std::vector<TrackId>{1} && restored[0].position.focus == 1,
          L"native database preserves per-tab search, sort and duplicate selection");
    Check(FormatTitle(std::wstring(1000, L'[') + L"%title%", [](const auto&, auto&) { return false; }).size() <= 65536,
          L"title-format nesting remains bounded");
    TrackId sortA = g_lib.Add(directory + L"\\sort-a.flac"), sortB = g_lib.Add(directory + L"\\sort-b.flac"),
            sortC = g_lib.Add(directory + L"\\sort-c.flac");
    g_lib[sortA].durationMs = 3000;
    g_lib[sortB].durationMs = g_lib[sortC].durationMs = 1000;
    std::vector<TrackId> numericItems{sortA, sortB, sortC};
    std::vector<uint32_t> numericMapping;
    app::SortItems(numericItems, 6, true, &numericMapping);
    bool numericAscending = numericItems == std::vector<TrackId>{sortB, sortC, sortA} &&
                            numericMapping == std::vector<uint32_t>{2, 0, 1};
    app::SortItems(numericItems, 6, false, nullptr);
    Check(numericAscending && numericItems == std::vector<TrackId>{sortA, sortB, sortC},
          L"numeric sorting preserves stable ties and maps every original occurrence in both directions");
    DeleteFileW(cache.c_str());
    DeleteFileW(m3u.c_str());
    DeleteFileW(database.c_str());
    DeleteFileW((database + L".bak").c_str());
    g_lib = std::move(previous);
}

// Tabs, playback and background jobs: search debouncing, shuffle over duplicate occurrences,
// removing the playing track, tab reopen/duplicate/reorder, import destination tracking, scan
// scoping, and library compaction preserving every reference.
void TestPortedUi(const std::wstring& directory) {
    using namespace app;
    testlog::Group group(L"recovered tabs, playback and background jobs");
    g.groupAlbums = false;
    g.shuffle = false;
    g.repeat = Repeat::Off;
    g.statTrack = kNone;
    g.closing = g.tagQuit = g.importQuit = g.artQuit = false;
    g.tokens.clear();
    NewPlaylist(L"Recovered workflow");
    uint64_t identity = Active().identity;
    Active().items = {0, 0, 1};
    RebuildView();
    {
        // Reordering a block must remap tokens for every surviving occurrence, not just the moved
        // rows. A stale slot hides the playing marker and makes Next start from the wrong track.
        const std::wstring oldFilter = g.filter;
        const auto oldItems = Active().items;
        std::vector<TrackId> dragItems{0, 1, 2, g_lib.Add(directory + L"\\drag-remap.flac")};
        g.filter.clear();
        Active().items = dragItems;
        RebuildView();
        unsigned long long first = NewToken(dragItems[0], g.active, 0);
        unsigned long long last = NewToken(dragItems[3], g.active, 3);
        MoveTrackBlock({2}, 0);
        Check(Active().items == std::vector<TrackId>{dragItems[2], dragItems[0], dragItems[1], dragItems[3]} &&
                  TokenIndex(first, g.active) == 1 && TokenIndex(last, g.active) == 3,
              L"moving a track block remaps every surviving playback occurrence to its final slot");
        g.tokens.clear();
        Active().items = dragItems;
        RebuildView();
        MoveTrackBlock({0, 2}, 1);
        Check(Active().items == std::vector<TrackId>{dragItems[0], dragItems[2], dragItems[1], dragItems[3]},
              L"a sparse drag gathers selected occurrences before an unselected row inside their span");
        Active().items = dragItems;
        RebuildView();
        MoveTrackBlock({1, 2}, 2);
        Check(Active().items == dragItems, L"dropping a contiguous block inside itself leaves its order intact");
        const auto oldTheme = g.themeMode;
        const auto oldSize = g.size;
        const auto oldSort = g.sortColumn;
        std::vector<TrackId> grouped;
        for (int i = 0; i < 4; ++i) {
            TrackId id = g_lib.Add(directory + L"\\header-drag-" + std::to_wstring(i) + L".flac");
            TagData tags;
            tags.title = i ? "Header drag visible" : "Hidden";
            tags.album = i == 3 ? "Second" : "First";
            g_lib.ApplyTags(id, tags);
            grouped.push_back(id);
        }
        Active().items = grouped;
        g.filter = L"Header drag visible";
        g.groupAlbums = true;
        g.themeMode = ThemeMode::Foobar;
        g.size = ViewSize::Normal;
        Active().sortColumn = g.sortColumn = -1;
        RebuildView();
        RECT from{}, to{};
        bool ready = g.view == std::vector<uint32_t>{1, 2, 3} && !g.groups.empty() &&
                     ListView_GetSubItemRect(g.list, RowOfViewPos(2), SubitemOf(2), LVIR_BOUNDS, &from) &&
                     ListView_GetItemRect(g.list, (int)g.groups[0].firstRow, &to, LVIR_BOUNDS);
        if (ready) {
            ClearSelection();
            ListView_SetItemState(g.list, RowOfViewPos(2), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            NMLISTVIEW drag{};
            drag.hdr = {g.list, (UINT_PTR)GetDlgCtrlID(g.list), LVN_BEGINDRAG};
            drag.iItem = RowOfViewPos(2);
            drag.ptAction = {from.left + S(4), (from.top + from.bottom) / 2};
            SendMessageW(g.hwnd, WM_NOTIFY, drag.hdr.idFrom, (LPARAM)&drag);
            SendMessageW(g.list, WM_LBUTTONUP, 0, MAKELPARAM(drag.ptAction.x, (to.top + to.bottom) / 2));
        }
        Check(ready && Active().items == std::vector<TrackId>{grouped[0], grouped[3], grouped[1], grouped[2]},
              L"a filtered group-header drop uses the underlying playlist position");
        g.themeMode = oldTheme;
        g.size = oldSize;
        g.groupAlbums = false;
        Active().sortColumn = g.sortColumn = oldSort;
        Active().items = oldItems;
        g.filter = oldFilter;
        g.tokens.clear();
        RebuildView();
    }
    {
        // Smart views still choose their members from their rules after a manual drag.
        int original = g.active;
        std::vector<TrackId> tracks;
        for (int i = 0; i < 4; ++i) {
            TrackId id = g_lib.Add(directory + L"\\view-drag-" + std::to_wstring(i) + L".flac");
            TagData tags;
            tags.title = "View drag sample";
            g_lib.ApplyTags(id, tags);
            g_lib[id].flags |= Track::kInLibrary;
            g_lib[id].rating = i < 3 ? (uint8_t)(90 - 10 * i) : 0;
            tracks.push_back(id);
        }
        g_lib.Touch();
        AutoPlaylist rules;
        rules.root.rules.push_back({RuleField::Title, RuleOp::Contains, "View drag sample", ""});
        rules.root.rules.push_back({RuleField::Rating, RuleOp::AtLeast, "1", ""});
        rules.limit = 3;
        rules.pick = PickBy::HighestRated;
        rules.ordered = true;
        int automatic = (int)g.playlists.size();
        InsertPlaylist(automatic, Playlist{L"View drag auto", {}, PlaylistKind::Auto, SerializeAutoPlaylist(rules)});
        ActivatePlaylist(automatic);
        auto dragThirdToFirst = [&] {
            if (g.view.size() < 3) return false;
            int source = RowOfViewPos(2), target = RowOfViewPos(0);
            RECT from{}, to{};
            bool ready = ListView_GetSubItemRect(g.list, source, SubitemOf(2), LVIR_BOUNDS, &from) &&
                         ListView_GetItemRect(g.list, target, &to, LVIR_BOUNDS);
            if (!ready) return false;
            ClearSelection();
            ListView_SetItemState(g.list, source, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            NMLISTVIEW drag{};
            drag.hdr = {g.list, (UINT_PTR)GetDlgCtrlID(g.list), LVN_BEGINDRAG};
            drag.iItem = source;
            drag.ptAction = {from.left + S(4), (from.top + from.bottom) / 2};
            SendMessageW(g.hwnd, WM_NOTIFY, drag.hdr.idFrom, (LPARAM)&drag);
            SendMessageW(g.list, WM_LBUTTONUP, 0, MAKELPARAM(drag.ptAction.x, to.top + 1));
            return true;
        };
        bool autoReady = Active().items == std::vector<TrackId>{tracks[0], tracks[1], tracks[2]};
        bool autoDragged = dragThirdToFirst();
        Check(autoReady && autoDragged && Active().sortColumn == -2 &&
                  Active().items == std::vector<TrackId>{tracks[2], tracks[0], tracks[1]},
              L"a smart playlist accepts a track drag and switches to manual order");
        g_lib[tracks[3]].rating = 95;
        g_lib.Touch();
        RebuildView();
        Check(Active().items == std::vector<TrackId>{tracks[0], tracks[1], tracks[3]},
              L"a manually ordered smart playlist keeps its rules and limit when membership changes");
        g.dirty = false;
        SortByColumn(SubitemOf(2));
        Check(Active().sortColumn == 2 && g.dirty,
              L"sorting a manually arranged smart playlist saves the restored column order");

        for (TrackId id : tracks) g_lib[id].flags |= Track::kInbox;
        g_lib.Touch();
        int inbox = (int)g.playlists.size();
        InsertPlaylist(inbox, Playlist{L"View drag inbox", {}, PlaylistKind::Inbox});
        ActivatePlaylist(inbox);
        auto before = Active().items;
        auto expected = before;
        if (expected.size() >= 3) std::rotate(expected.begin(), expected.begin() + 2, expected.begin() + 3);
        bool inboxDragged = dragThirdToFirst();
        g_lib.Touch();
        RebuildView();
        Check(inboxDragged && Active().sortColumn == -2 && Active().items == expected,
              L"the Inbox accepts track drags and retains their order on rebuild");
        ActivatePlaylist(original);
        ErasePlaylist(inbox);
        ErasePlaylist(automatic);
        for (TrackId id : tracks) g_lib[id].flags &= ~(Track::kInLibrary | Track::kInbox);
        g_lib.Touch();
        RebuildView();
    }
    {
        auto original = g_lib[0];
        LoadArtFor(0);
        auto generation = g.artGeneration;
        auto update = [&] {
            auto* results = new std::vector<TagResult>(1);
            auto& r = results->front();
            r.id = 0;
            r.path = g_lib.Path(0);
            r.success = r.haveStats = true;
            r.tags.title = "Track 0";
            r.tags.artist = "Test artist";
            r.tags.album = "Test album";
            r.fileTime = 123;
            r.fileSize = 456;
            AcceptTagBatch(results);
        };
        g_lib[0].fileTime = 1;
        update();
        bool refreshed = g.artGeneration == generation + 1 && g_lib[0].fileTime == 123;
        update();
        Check(refreshed && g.artGeneration == generation + 1,
              L"same-track metadata readback refreshes changed artwork once and retains an unchanged cache stamp");
        g_lib[0] = original;
    }
    SetWindowTextW(g.search, L"Track");
    Check(g.searchPending, L"query edits are debounced");
    FlushSearch();
    Check(!g.searchPending && g.view.size() == 3, L"immediate actions can flush a pending search");
    auto token = NewToken(0, g.active, 1);
    g.playing = 0;
    g.playingToken = token;
    g.playingPl = g.active;
    OnEngineEvent(EngineEvent::Damaged, token);
    Check(g_lib[0].Failed() && g.message.find(L"Track 0: Playback failed") != std::wstring::npos &&
              g.playing == 0 && g.playingToken == token,
          L"a damaged track is marked failed and named in the notice while its queued audio plays out");
    g_lib[0].flags &= ~Track::kFailed;
    int source = -1;
    Check(ComputeNext(token, false, source) == 2, L"playback advances beyond the second duplicate occurrence");
    Active().items = {0, 1, 0};
    auto duplicate = NewToken(0, g.active, 2);
    g.playingToken = duplicate;
    Previous();
    Check(g.tokens[g.decodingToken].index == 1, L"Previous steps back from the playing duplicate occurrence");
    Active().items = {0, 0, 1};
    g.playingToken = token;
    g.shuffle = true;
    g.shufflePl = -1;
    testlog::Cases shuffle;
    int shuffled = ComputeNext(token, false, source);
    auto next = NewToken(Active().items[shuffled], g.active, (uint32_t)shuffled);
    int last = ComputeNext(next, false, source);
    shuffle.Add(shuffled != 1 && last != 1 && last != shuffled && shuffled >= 0 && last >= 0,
                L"duplicate occurrences");
    {
        auto log = PlayLog();
        SetPlayLog({});
        for (unsigned i = 0; i < 20; ++i) LogPlay(1, 1000 + i * 100, 3, false, true);
        g.skipAwareShuffle = true;
        g.rng.seed(12345);
        int favored = 0;
        bool complete = true;
        for (int i = 0; i < 300; ++i) {
            g.shuffleQueue.clear();
            g.shuffleStarted = false;
            int pick = ComputeNext(token, false, source);
            favored += pick == 0;
            auto following = NewToken(Active().items[pick], g.active, (uint32_t)pick);
            int remaining = ComputeNext(following, false, source);
            complete &= remaining >= 0 && remaining != pick && remaining != 1;
        }
        shuffle.Add(favored > 250 && complete, L"skip-aware weighting without omissions");
        g.skipAwareShuffle = false;
        g.radio = true;
        for (auto& [key, info] : g.tokens) info.radioNext = UINT32_MAX;
        g.repeat = Repeat::Off;
        int pick = ComputeNext(token, false, source);
        testlog::Cases radio;
        radio.Add(pick == 2 && ComputeNext(token, false, source) == pick && ComputeNext(token, true, source) == pick,
                  L"cached gapless/manual next avoids current duplicates");
        auto following = NewToken(1, g.active, 2);
        int nextRadio = ComputeNext(following, false, source);
        radio.Add(nextRadio >= 0 && nextRadio < 2, L"continues with repeat off");
        radio.Add(ComputeNext(token, false, source) == pick, L"earlier lookahead stays stable");
        g.repeat = Repeat::One;
        radio.Add(ComputeNext(following, false, source) == 2 && ComputeNext(following, true, source) != 2,
                  L"repeat track and manual Next");
        Check(radio.Passed(), L"radio advances continuously with stable choices and respects repeat track%ls", radio.failed.c_str());
        g.radio = false;
        g.repeat = Repeat::Off;
        SetPlayLog(std::move(log));
    }
    {
        const auto items = Active().items;
        const Track saved[] = {g_lib[0], g_lib[1], g_lib[2]};
        auto decoding = g.decodingToken;
        g.decodingToken = 0;
        for (TrackId id : {0u, 1u, 2u}) {
            g_lib[id].albumArtist = g_lib.Intern("Album shuffle artist");
            g_lib[id].album = g_lib.Intern(id == 2 ? "Other album" : "First album");
            g_lib[id].date = 0;
            g_lib[id].disc = 1;
            g_lib[id].trackNo = id == 0 ? 2 : 1;
        }
        Active().items = {0, 2, 1, 0};
        SetShuffle(kShuffleOff);
        OnCommand(IDM_SHUFFLE_ALBUMS, 0, nullptr);
        HMENU menu = BuildMenu();
        UpdateMainMenu(menu);
        shuffle.Add(g.shuffle == kShuffleAlbums && (GetMenuState(menu, IDM_SHUFFLE_ALBUMS, MF_BYCOMMAND) & MF_CHECKED) &&
                    !(GetMenuState(menu, IDM_SHUFFLE, MF_BYCOMMAND) & MF_CHECKED), L"album menu and checkmarks");
        DestroyMenu(menu);
        g.skipAwareShuffle = true;  // track weights must never split an album
        auto from = NewToken(1, g.active, 2);
        for (int expected : {0, 3, 1, -1}) {
            int pick = ComputeNext(from, false, source);
            shuffle.Add(pick == expected && ComputeNext(from, false, source) == pick && source == g.active,
                        L"album continuation, duplicate occurrences, stable lookahead and stop");
            if (pick < 0) break;
            from = NewToken(Active().items[pick], g.active, (uint32_t)pick);
        }
        g.repeat = Repeat::All;
        std::vector<int> pass;
        for (int i = 0; i < 4; ++i) {
            int pick = ComputeNext(from, false, source);
            shuffle.Add(pick >= 0 && pick < 4 && ComputeNext(from, false, source) == pick, L"repeat album lookahead");
            if (pick < 0 || pick >= 4) break;
            pass.push_back(pick);
            from = NewToken(Active().items[pick], g.active, (uint32_t)pick);
        }
        auto sorted = pass;
        std::sort(sorted.begin(), sorted.end());
        shuffle.Add(sorted == std::vector<int>{0, 1, 2, 3} &&
                    (pass == std::vector<int>{2, 0, 3, 1} || pass == std::vector<int>{1, 2, 0, 3}),
                    L"repeat begins a complete pass of intact albums");
        from = NewToken(1, g.active, 2);  // a manual jump back to the first album's first track
        shuffle.Add(ComputeNext(from, false, source) == 0, L"manual jump continues the chosen album");
        SetShuffle(kShuffleAlbums);
        g.repeat = Repeat::One;
        shuffle.Add(ComputeNext(from, false, source) == 2 && ComputeNext(from, true, source) == 0,
                    L"album repeat track and manual Next");
        g.repeat = Repeat::Off;
        auto queued = g.queue;
        g.queue = {2};
        auto interruption = NewToken(2, g.active);
        g.tokens[interruption].anchor = 1;
        g.tokens[interruption].anchorIndex = 2;
        g.tokens[interruption].queued = true;
        shuffle.Add(ComputeNext(interruption, false, source) == 0 && ComputeNext(from, false, source) == 0,
                    L"explicit queue resumes at the same album position");
        g.queue = std::move(queued);
        OnCommand(IDC_SHUFFLE, 0, nullptr);
        shuffle.Add(g.shuffle == kShuffleOff, L"cycle albums to off");
        OnCommand(IDC_SHUFFLE, 0, nullptr);
        shuffle.Add(g.shuffle == kShuffleTracks, L"cycle off to tracks");
        OnCommand(IDC_SHUFFLE, 0, nullptr);
        shuffle.Add(g.shuffle == kShuffleAlbums, L"cycle tracks to albums");
        SetShuffle(kShuffleOff);
        g.skipAwareShuffle = false;
        g.repeat = Repeat::Off;
        Active().items = items;
        for (TrackId id : {0u, 1u, 2u}) g_lib[id] = saved[id];
        g.decodingToken = decoding;
    }
    Check(shuffle.Passed(), L"track, weighted and album shuffle preserve occurrences, order and playback controls%ls",
          shuffle.failed.c_str());
    g.shuffle = false;
    ListView_SetItemState(g.list, 1, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    RemoveTracks(false);
    Check(TokenIndex(token, g.active) == -1 && ComputeNext(token, false, source) == 1,
          L"removing the playing occurrence advances to its surviving successor");
    int removed = g.active;
    RemovePlaylist(removed);
    Check(g.playingPl == -1 && ComputeNext(token, false, source) == -1, L"closed playback sources never advance into another tab");
    ReopenPlaylist();
    Check(Active().identity == identity && g.playingPl == g.active, L"reopening reconnects the original playback source");
    Stop();
    DuplicatePlaylist();
    Check(Active().identity != identity && Active().items.size() == 2, L"duplicates have independent contents and identity");
    Active().items.clear();
    Check(g.playlists[g.active - 1].items.size() == 2, L"editing a duplicate leaves its source untouched");
    int viewIndex = (int)g.playlists.size();
    Playlist sourceView{L"View with non-positional IDs", {2, 0}, PlaylistKind::Auto};
    sourceView.builtRevision = g_lib.Revision();
    InsertPlaylist(viewIndex, std::move(sourceView));
    ActivatePlaylist(viewIndex);
    ClearSelection();
    ListView_SetItemState(g.list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    const auto viewSelection = SelectedTracks();
    DuplicatePlaylist();
    Check(viewSelection == std::vector<TrackId>{2} && Active().kind == PlaylistKind::Static &&
              SelectedTracks() == viewSelection && Active().position.selected == std::vector<TrackId>{0},
          L"duplicating a computed view translates track selection to static occurrence indices");
    BeginRename(g.active);
    SetWindowTextW(g.rename, L"Moved edit");
    ReorderPlaylist(g.active, 1);
    Check(!g.rename && Active().name == L"Moved edit" && g.active == 1, L"moving a tab commits its pending name");
    int library = g.active;
    ReorderPlaylist(0, 1);
    Check(g.active == library && g.playlists[0].kind == PlaylistKind::Library, L"the Library remains the first tab");
    std::wstring fixture = directory + L"\\import-target.flac";
    Check(WriteBytes(fixture, "fLaC"), L"isolated import fixture created");
    NewPlaylist(L"Import destination");
    identity = Active().identity;
    AddPaths({fixture}, false);
    NewPlaylist(L"Other destination");
    uint64_t other = Active().identity;
    ReorderPlaylist(g.active - 1, 1);
    if (!g.tagWorker.joinable()) g.tagWorker = std::thread(TagWorker, g.hwnd);
    auto idle = [] {
        std::lock_guard<std::mutex> lock(g.importMx);
        return !g.importRunning && g.importQueue.empty() && !g.importOutstanding && g.tagPending.empty();
    };
    Check(PumpUntil(idle), L"background imports and metadata delivery finish");
    auto target = std::find_if(g.playlists.begin(), g.playlists.end(), [&](const Playlist& pl) { return pl.identity == identity; });
    Check(target != g.playlists.end() && target->items.size() == 1 && Active().identity == other && Active().items.empty(),
          L"imports retain their destination through tab activation and reorder");
    TrackId imported = g_lib.Find(fixture);
    Check(g_lib.Contains(imported) && !g_lib[imported].Loaded() && (g_lib[imported].flags & Track::kTagFailed),
          L"metadata failures stay unloaded and retryable");
    NewPlaylist(L"Closing destination");
    AddPaths({fixture}, false);
    RemovePlaylist(g.active);
    Check(PumpUntil(idle), L"closing a destination cancels queued imports without blocking");
    for (int i = 0; i < 12; ++i) { NewPlaylist(L"Bounded history"); RemovePlaylist(g.active); }
    Check(g.closedPlaylists.size() <= 8, L"recently closed tab history is bounded");
    auto* scope = new ScanResult();
    scope->scope = directory;
    scope->rootOk = false;
    scope->complete = false;
    OnScanResult(scope, false);
    bool incomplete = ScanInProgress();
    auto roots = std::move(g.libraryFolders);
    g.libraryFolders.clear();
    StartScanner();
    g.libraryFolders = std::move(roots);
    Check(incomplete && !ScanInProgress(), L"restarting folder scans releases incomplete scopes and stale payloads");
    g_lib.Add(directory + L"\\gap-before-retained.flac");
    std::wstring retainedPath = directory + L"\\retained-history.flac";
    TrackId originalId = g_lib.Add(retainedPath);
    g_lib[originalId].rating = 90;
    g_lib[originalId].playCount = 7;
    g.queue.push_back(originalId);
    g.history.push_back(originalId);
    g.resumeTrack = originalId;
    auto retainedToken = NewToken(originalId, g.active);
    g.tokens[retainedToken].anchor = originalId;
    LogPlay(originalId, UnixNow(), 90, true, false);
    PointsState points;
    points.total = 1234;
    points.recent.push_back({UnixNow(), PointsKind::Play, originalId, 1234});
    SetPointsState(std::move(points));
    size_t retained = g_lib.Count();
    g.cellCache[UINT64_MAX] = L"stale";
    for (int i = 0; i < 200; ++i) g_lib.Add(directory + L"\\orphan-" + std::to_wstring(i) + L".flac");
    CompactLibrary();
    Check(!g.compactPending && g_lib.Count() <= retained && g_lib.Find(directory + L"\\orphan-0.flac") == kNone && !g.cellCache.count(UINT64_MAX),
          L"idle cleanup removes orphan records and invalidates pooled display caches (pending=%d count=%zu retained=%zu tags=%zu import=%d)",
          g.compactPending, g_lib.Count(), retained, g.tagPending.size(), (int)g.importRunning.load());
    TrackId remapped = g_lib.Find(retainedPath);
    Check(remapped != kNone && remapped != originalId && g_lib[remapped].rating == 90 && g_lib[remapped].playCount == 7 &&
          g.queue.back() == remapped && g.history.back() == remapped && g.resumeTrack == remapped &&
          g.tokens[retainedToken].id == remapped && g.tokens[retainedToken].anchor == remapped &&
          PlayLog().back().id == remapped && PointsRecent().back().id == remapped && PointsTotal() == 1234,
          L"compaction preserves stats, queue, tokens, resume, listening history and point references while IDs change");
    g.tokens.clear();
    g.queue.clear();
    g.history.clear();
    g.resumeTrack = kNone;
    SetPlayLog({});
    ResetPoints();
    StopBackgroundJobs();
    Check(!g.importWorker.joinable() && !g.artWorker.joinable() && !g.importOutstanding,
          L"shutdown wakes blocked producers and drains owned import/artwork results");
    { std::lock_guard<std::mutex> lock(g.tagMx); g.tagQuit = true; }
    g.tagCv.notify_all();
    if (g.tagWorker.joinable()) g.tagWorker.join();
    g.tagPending.clear();
    g.closing = false;
    DeleteFileW(fixture.c_str());
}
