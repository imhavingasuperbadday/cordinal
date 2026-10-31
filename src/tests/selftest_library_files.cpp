// Library file checks of the self test (see selftest.cpp): folder scanning, organize, tag undo,
// duplicate audio detection and scan merging. RunSelfTest calls each one inside its own testlog
// group, in the order the groups appear in the log.
#include "selftest_common.h"

// Organize: planned paths, sanitizing, collisions, covers moving along, cleanup, and undo.
void TestOrganize(const std::wstring& media, const std::wstring& scratch) {
    Check(SafePathComponent(L"AC:DC? <live>") == L"AC_DC_ _live_" && SafePathComponent(L"CON") == L"CON_" &&
              SafePathComponent(L"nul.txt") == L"nul_.txt" && SafePathComponent(L"CON", 3) == L"CO_" &&
              SafePathComponent(L"Trailing dots... ") == L"Trailing dots",
          L"ORGANIZE safe path components");
    g_lib = Library();
    std::wstring root = scratch + L".org", in = root + L"\\in", out = root + L"\\out", logs = root + L"\\logs";
    for (auto d : {root, in, in + L"\\A", in + L"\\B", out}) CreateDirectoryW(d.c_str(), nullptr);
    CopyFileW((media + L"\\sine.flac").c_str(), (in + L"\\A\\one.flac").c_str(), FALSE);
    CopyFileW((media + L"\\sine.mp3").c_str(), (in + L"\\A\\two.mp3").c_str(), FALSE);
    CopyFileW((media + L"\\sine.flac").c_str(), (in + L"\\A\\cover.jpg").c_str(), FALSE);  // contents don't matter
    CopyFileW((media + L"\\sine.flac").c_str(), (in + L"\\B\\same.flac").c_str(), FALSE);  // same tags as one.flac
    std::vector<TrackId> ids;
    for (auto f : {in + L"\\A\\one.flac", in + L"\\A\\two.mp3", in + L"\\B\\same.flac"}) {
        TrackId id = g_lib.Add(f);
        TagData t;
        ReadTags(f, t);
        if (f.find(L"two") != std::wstring::npos) t.artist = "AC/DC";  // a slash in a tag can't make a folder
        g_lib.ApplyTags(id, t);
        ids.push_back(id);
    }
    auto plan = PlanOrganize(ids, out, kDefaultOrganizePattern);
    std::wstring blockedLogs = root + L"\\blocked-logs";
    WriteTestBytes(blockedLogs, "a regular file, not a log directory");
    auto refused = ExecuteOrganize(plan, blockedLogs, {in, out});
    Check(refused.moved.empty() && refused.failed.size() == plan.size() &&
              GetFileAttributesW(plan[0].from.c_str()) != INVALID_FILE_ATTRIBUTES,
          L"ORGANIZE leaves every source in place when the undo log cannot be created");
    std::wstring safeDevice = out + L"\\" + SafePathComponent(L"nul.txt");
    Check(WriteTestBytes(safeDevice, "regular file") && ReadTestBytes(safeDevice) == "regular file",
          L"ORGANIZE sanitized device names with extensions create real files");
    DeleteFileW(safeDevice.c_str());
    std::wstring album = out + L"\\Test Artist\\Test Album\\";
    bool planned = plan.size() == 3 && plan[0].to == album + L"07 Sine \x00DCn\x00EF" L"code.flac" &&
                   plan[1].to == out + L"\\AC-DC\\Test Album\\07 Sine \x00DCn\x00EF" L"code.mp3" &&
                   plan[2].to == album + L"07 Sine \x00DCn\x00EF" L"code (2).flac" && plan[2].skip.empty();
    Check(planned, L"ORGANIZE plan: %ls | %ls | %ls", plan.size() > 0 ? plan[0].to.c_str() : L"", plan.size() > 1 ? plan[1].to.c_str() : L"",
          plan.size() > 2 ? plan[2].to.c_str() : L"");
    auto result = ExecuteOrganize(plan, logs, {in, out});
    bool moved = result.moved.size() == 3 && result.failed.empty() && GetFileAttributesW(plan[0].to.c_str()) != INVALID_FILE_ATTRIBUTES &&
                 GetFileAttributesW((in + L"\\A\\one.flac").c_str()) == INVALID_FILE_ATTRIBUTES;
    // A's audio went to two folders (AC-DC and Test Artist), so its cover stays; B moved whole.
    bool cover = GetFileAttributesW((in + L"\\A\\cover.jpg").c_str()) != INVALID_FILE_ATTRIBUTES;
    bool cleaned = GetFileAttributesW((in + L"\\B").c_str()) == INVALID_FILE_ATTRIBUTES &&
                   GetFileAttributesW(in.c_str()) != INVALID_FILE_ATTRIBUTES;
    Check(moved && cover && cleaned, L"ORGANIZE moved=%zu failed=%zu, split folder keeps its cover=%d, emptied folder removed=%d",
          result.moved.size(), result.failed.size(), cover, cleaned);
    std::wstring error;
    auto undone = UndoLastOrganize(logs, {in, out}, error);
    bool back = undone.size() == 3 && GetFileAttributesW((in + L"\\A\\one.flac").c_str()) != INVALID_FILE_ATTRIBUTES &&
                GetFileAttributesW((in + L"\\B\\same.flac").c_str()) != INVALID_FILE_ATTRIBUTES &&
                GetFileAttributesW((out + L"\\Test Artist").c_str()) == INVALID_FILE_ATTRIBUTES;
    Check(back, L"ORGANIZE undo moved %zu file(s) back and removed the empty folders %ls", undone.size(), error.c_str());
    // A whole folder moving to one place takes its cover along.
    DeleteFileW((in + L"\\A\\two.mp3").c_str());  // now A holds only one.flac and its cover
    auto plan2 = PlanOrganize({ids[0]}, out, L"Solo\\%title%");
    auto r2 = ExecuteOrganize(plan2, logs, {in, out});
    Check(r2.moved.size() == 1 && GetFileAttributesW((out + L"\\Solo\\cover.jpg").c_str()) != INVALID_FILE_ATTRIBUTES,
          L"ORGANIZE a folder moved whole takes cover.jpg along");
    SHCreateDirectoryExW(nullptr, DirectoryOf(plan2[0].from).c_str(), nullptr);
    bool blockerCreated = WriteTestBytes(plan2[0].from, "a newly created file must not be overwritten");
    auto partial = UndoLastOrganize(logs, {in, out}, error);
    Check(blockerCreated && !error.empty() && GetFileAttributesW(r2.log.c_str()) != INVALID_FILE_ATTRIBUTES &&
              ReadTestBytes(plan2[0].from) == "a newly created file must not be overwritten" &&
              GetFileAttributesW(plan2[0].to.c_str()) != INVALID_FILE_ATTRIBUTES,
          L"ORGANIZE a blocked undo keeps its log and never overwrites a new file");
    DeleteFileW(plan2[0].from.c_str());
    auto retried = UndoLastOrganize(logs, {in, out}, error);
    Check(error.empty() && retried.size() == 1 && GetFileAttributesW(plan2[0].from.c_str()) != INVALID_FILE_ATTRIBUTES &&
              GetFileAttributesW(r2.log.c_str()) == INVALID_FILE_ATTRIBUTES,
          L"ORGANIZE a partial undo can be retried after resolving the collision");
    auto r3 = ExecuteOrganize(plan2, logs, {in, out});
    std::string firstLog = ReadTestBytes(r3.log);
    OrganizeMove nextMove{ids[0], plan2[0].to, out + L"\\again.flac", L""};
    auto r4 = ExecuteOrganize({nextMove}, logs, {in, out});
    Check(r3.moved.size() == 1 && r4.moved.size() == 1 && r3.log != r4.log &&
              !firstLog.empty() && ReadTestBytes(r3.log) == firstLog,
          L"ORGANIZE rapid successive runs preserve distinct undo logs");
    SHFILEOPSTRUCTW op{};
    std::wstring from = root + L'\0';
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_NO_UI;
    SHFileOperationW(&op);
    g_lib = Library();
}

// Folder scanner: the first listing, then watched changes (a new file, a deleted file and a
// new subfolder) arriving as debounced re-lists of just the changed folders.
void TestScanner(const std::wstring& media, const std::wstring& scratch) {
    std::wstring root = scratch + L".scan";
    CreateDirectoryW(root.c_str(), nullptr);
    CreateDirectoryW((root + L"\\Album").c_str(), nullptr);
    CopyFileW((media + L"\\sine.flac").c_str(), (root + L"\\Album\\01.flac").c_str(), FALSE);
    CopyFileW((media + L"\\sine.mp3").c_str(), (root + L"\\Album\\02.mp3").c_str(), FALSE);
    CopyFileW((media + L"\\sine.flac").c_str(), (root + L"\\cover.txt").c_str(), FALSE);  // not audio

    std::vector<ScanResult*> results;
    bool firstDone = false;
    static std::vector<ScanResult*>* s_results;
    static bool* s_done;
    s_results = &results;
    s_done = &firstDone;
    WNDCLASSW wc{};
    wc.lpfnWndProc = [](HWND h, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
        if (msg == WM_APP + 9) {
            if (wp == 1) *s_done = true;
            else s_results->push_back(reinterpret_cast<ScanResult*>(lp));
            return 0;
        }
        return DefWindowProcW(h, msg, wp, lp);
    };
    wc.lpszClassName = L"CordinalScanTest";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    FolderScanner scanner;
    scanner.Start(hwnd, WM_APP + 9, {root + L"\\"}, 200);
    Pump(5000, [&] { return firstDone; });
    bool initial = firstDone && results.size() == 1 && results[0]->initial && results[0]->rootOk &&
                   results[0]->files.size() == 2 && results[0]->files[0].size > 0;
    Check(initial, L"SCANNER initial scan: %zu result(s), %zu file(s)", results.size(),
          results.empty() ? (size_t)0 : results[0]->files.size());

    for (auto* r : results) delete r;
    results.clear();
    CopyFileW((media + L"\\sine.ogg").c_str(), (root + L"\\Album\\03.ogg").c_str(), FALSE);
    DeleteFileW((root + L"\\Album\\02.mp3").c_str());
    CreateDirectoryW((root + L"\\New").c_str(), nullptr);
    CopyFileW((media + L"\\sine.flac").c_str(), (root + L"\\New\\a.flac").c_str(), FALSE);
    // Debounced: nothing until the folders have been quiet for a while, then one re-list each.
    Pump(6000, [&] {
        size_t n = 0;
        for (auto* r : results) n += r->files.size();
        return n >= 3;
    });
    std::vector<std::wstring> names;
    for (auto* r : results)
        for (auto& f : r->files) names.push_back(FileNameOf(f.path));
    std::sort(names.begin(), names.end());
    bool watched = names == std::vector<std::wstring>{L"01.flac", L"03.ogg", L"a.flac"};
    Check(watched, L"SCANNER watch: %zu re-list(s) with %zu file(s) after add/delete/new folder", results.size(), names.size());
    scanner.Stop();
    for (auto* r : results) delete r;
    results.clear();

    HANDLE blocked = CreateFileW((root + L"\\Album").c_str(), FILE_LIST_DIRECTORY, 0, nullptr,
                                 OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (Ready(blocked != INVALID_HANDLE_VALUE, L"a scratch child folder is locked against enumeration")) {
        std::vector<ScanFile> partial, recovered;
        bool incomplete = !ListAudioFiles(root, partial);
        CloseHandle(blocked);
        bool complete = ListAudioFiles(root, recovered);
        Check(incomplete && complete && recovered.size() == 3,
              L"SCANNER an unreadable child makes the listing incomplete, and unlocking restores it");
    }

    std::atomic<bool> cancelled{true};
    std::vector<ScanFile> cancelledFiles;
    Check(!ListAudioFiles(root, cancelledFiles, &cancelled) && cancelledFiles.empty(), L"SCANNER cancelled listing stops");

    std::vector<std::wstring> roots;
    for (int i = 0; i < 70; ++i) {
        roots.push_back(root + L"\\Watch" + std::to_wstring(i));
        CreateDirectoryW(roots.back().c_str(), nullptr);
    }
    firstDone = false;
    scanner.Start(hwnd, WM_APP + 9, roots, 200);
    Pump(5000, [&] { return firstDone; });
    Check(firstDone && results.size() == roots.size(), L"SCANNER initial scan of 70 watched folders");
    for (auto* r : results) delete r;
    results.clear();
    CopyFileW((media + L"\\sine.flac").c_str(), (roots.back() + L"\\late.flac").c_str(), FALSE);
    Pump(6000, [&] { return !results.empty(); });
    Check(results.size() == 1 && results[0]->scope == roots.back() && !results[0]->initial &&
              results[0]->files.size() == 1 && FileNameOf(results[0]->files[0].path) == L"late.flac",
          L"SCANNER watches changes beyond the 64-handle wait limit");
    scanner.RescanAll();
    Pump(5000, [&] { return results.size() >= roots.size() + 1; });
    Check(results.size() >= roots.size() + 1, L"SCANNER manual rescan wakes with 70 watched folders");
    scanner.Stop();
    for (auto* r : results) delete r;
    results.clear();
    Pump(100);  // drain any batches posted just before Stop returned
    for (auto* r : results) delete r;
    DestroyWindow(hwnd);
    SHFILEOPSTRUCTW op{};
    std::wstring from = root + L'\0';
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_NO_UI;
    SHFileOperationW(&op);
}

// Watching the system Music folder is off by default. The auto-added folder from the earlier
// build is cleaned up once; a folder list the user chose (including none) stays as it is. MP4 is
// one of the scanner's audio types.
void TestLibraryFolders(const std::wstring& scratch) {
    using namespace app;
    const std::wstring originalIni = g.iniPath;
    const auto originalFolders = g.libraryFolders;
    const std::wstring profile = scratch + L"\\folder-settings.ini";
    DeleteFileW(profile.c_str());
    g.iniPath = profile;
    g.libraryFolders.clear();
    const std::wstring music = MusicFolder();
    LoadLibrarySettings();
    Check(g.libraryFolders.empty(), L"LIBRARY FOLDERS a new profile watches no folder, even the system Music folder");

    WritePrivateProfileStringW(L"Library", L"FoldersChosen", nullptr, g.iniPath.c_str());
    WritePrivateProfileStringW(L"Library", L"Folders", music.c_str(), g.iniPath.c_str());
    LoadLibrarySettings();
    Check(g.libraryFolders.empty() && IniInt(L"Library", L"FoldersChosen", 0) != 0,
          L"LIBRARY FOLDERS the auto-added system Music folder is cleared once and never added again");

    g.libraryFolders = {music};
    SaveLibrarySettings();
    g.libraryFolders.clear();
    LoadLibrarySettings();
    Check(g.libraryFolders == std::vector<std::wstring>{music},
          L"LIBRARY FOLDERS a Music folder the user chose is kept");

    g.libraryFolders = {L"D:\\Custom Music"};
    SaveLibrarySettings();
    g.libraryFolders.clear();
    LoadLibrarySettings();
    Check(g.libraryFolders == std::vector<std::wstring>{L"D:\\Custom Music"},
          L"LIBRARY FOLDERS a chosen folder list is read back without adding a default");

    g.libraryFolders.clear();
    SaveLibrarySettings();
    LoadLibrarySettings();
    Check(g.libraryFolders.empty(), L"LIBRARY FOLDERS removing every folder is remembered");

    Check(IsSupportedAudioFile(L"video.mp4") && OpenFileFilterSpec().find(L"*.mp4") != std::wstring::npos,
          L"LIBRARY FOLDERS MP4 audio is discovered, and the open dialog offers it");

    g.iniPath = originalIni;
    g.libraryFolders = originalFolders;
}

// Tag undo: an edit on a copy is recorded, applied, then reversed from its journal. The audio
// must come back bit-identical and the tags must read as before.
void TestTagUndo(const std::wstring& media, const std::wstring& scratch) {
    using namespace app;
    const std::wstring dir = scratch + L"\\tagundo_test";
    DeleteTestFolder(dir);
    CreateDirectoryW(dir.c_str(), nullptr);
    testlog::Cases journalled, reversed;
    for (const wchar_t* name : {L"sine.flac", L"sine.mp3"}) {
        const std::wstring copy = scratch + L"\\undo_" + name;
        WriteTestBytes(copy, ReadTestBytes(media + L"\\" + name));
        TagData before;
        ReadTags(copy, before);
        unsigned rate = 0;
        const auto audioBefore = DecodeAll(copy, rate);

        TagEdit edit;
        edit.title = "Undo test title";
        edit.artist = "Undo test artist";
        std::wstring error, file;
        uint32_t count = 0;
        TagJournal journal;
        journal.files.emplace_back();
        bool captured = CaptureTagUndo(copy, edit, journal.files[0]);
        bool saved = captured && SaveTagJournal(dir, journal, error);
        bool written = saved && WriteTags(copy, edit, error);
        TagData changed;
        ReadTags(copy, changed);
        bool applied = written && changed.title == "Undo test title" && changed.artist == "Undo test artist";
        bool found = FindNewestTagJournal(dir, file, count) && count == 1;
        TagJournal back;
        bool loaded = found && LoadTagJournal(file, back) && back.files.size() == 1;
        const TagUndoFile* old = loaded ? &back.files[0] : nullptr;
        bool fields = loaded && old->path == copy && old->coverRestorable && old->old.title &&
                      *old->old.title == before.title && old->old.artist && *old->old.artist == before.artist &&
                      !old->old.album && !old->old.cover;
        journalled.Add(applied && fields, std::wstring(name) + L" (old title \"" + FromUtf8(before.title) + L"\")");

        bool restored = loaded && WriteTags(copy, old->old, error);
        TagData after;
        ReadTags(copy, after);
        unsigned rate2 = 0;
        const auto audioAfter = DecodeAll(copy, rate2);
        bool same = restored && after.title == before.title && after.artist == before.artist && !audioBefore.empty() &&
                    audioAfter == audioBefore;
        DeleteTagJournal(file);
        reversed.Add(same && !FindNewestTagJournal(dir, file, count),
                     std::wstring(name) + L" " + std::to_wstring(audioAfter.size()) + L" samples");
    }
    Check(journalled.Passed(), L"TAG UNDO FLAC and MP3 edits are applied and journalled with the old values%ls",
          journalled.failed.c_str());
    Check(reversed.Passed(), L"TAG UNDO restores the tags with the audio bit-identical, and the journal goes%ls",
          reversed.failed.c_str());
    for (int i = 0; i < 12; ++i) {
        TagJournal journal;
        journal.files.push_back(TagUndoFile{L"C:\\x.flac", TagEdit{}, true});
        journal.files[0].old.title = "t";
        std::wstring error;
        SaveTagJournal(dir, journal, error);
    }
    size_t kept = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir))
        kept += entry.path().extension() == L".cdu";
    Check(kept == 10, L"TAG UNDO keeps the newest 10 journals (%zu kept of 12)", kept);

    // A file the engine has open waits for playback, then is written during shutdown. This is the
    // path that used to drop the edit silently when cordinal closed while the song was playing.
    const std::wstring deferredCopy = scratch + L"\\deferred.flac";
    Ready(WriteTestBytes(deferredCopy, ReadTestBytes(media + L"\\sine.flac")), L"deferred tag fixture is written");
    const std::wstring savedDataDir = g.dataDir;
    g.dataDir = scratch;
    g_lib = Library();
    TrackId deferredId = g_lib.Add(deferredCopy);
    g.playing = deferredId;
    TagEdit deferredEdit;
    deferredEdit.title = "Deferred title";
    WriteTagsInBackground({deferredId}, deferredEdit, false);
    TagData deferredBefore;
    ReadTags(deferredCopy, deferredBefore);
    const bool waited = deferredBefore.title != "Deferred title";
    g.playing = kNone;
    std::wstring deferredError;
    const bool flushed = FlushDeferredTagWrites(deferredError);
    TagData deferredAfter;
    ReadTags(deferredCopy, deferredAfter);
    Check(waited && flushed && deferredError.empty() && deferredAfter.title == "Deferred title" &&
              !TagWriterRetainsTracks(),
          L"TAG UNDO a playing file's deferred edit is written when playback lets go and at shutdown");

    // A format cordinal can't rewrite is reported instead of closing as if the edit was saved.
    const std::wstring unsupported = scratch + L"\\unsupported.wav";
    Ready(WriteTestBytes(unsupported, ReadTestBytes(media + L"\\sine.wav")), L"unsupported tag fixture is written");
    g_lib = Library();
    TrackId unsupportedId = g_lib.Add(unsupported);
    TagEdit unsupportedEdit;
    unsupportedEdit.title = "Not writable";
    g.message.clear();
    WriteTagsInBackground({unsupportedId}, unsupportedEdit, false);
    TagData unsupportedAfter;
    ReadTags(unsupported, unsupportedAfter);
    Check(g.message.find(L"can't be written") != std::wstring::npos && unsupportedAfter.title != "Not writable",
          L"TAG UNDO files whose format can't be written are named instead of silently skipped");
    g_lib = Library();
    g.dataDir = savedDataDir;
}

// Two copies of one FLAC with different tags share the audio MD5 in STREAMINFO, so they group.
void TestAudioHash(const std::wstring& media, const std::wstring& scratch) {
    const std::wstring a = scratch + L"\\hash_a.flac", b = scratch + L"\\hash_b.flac", w = scratch + L"\\hash_c.wav";
    WriteTestBytes(a, ReadTestBytes(media + L"\\sine.flac"));
    WriteTestBytes(b, ReadTestBytes(media + L"\\sine.flac"));
    WriteTestBytes(w, ReadTestBytes(media + L"\\sine.wav"));
    TagEdit edit;
    edit.title = "Same audio, other tags";
    edit.artist = "Other artist";
    std::wstring error;
    bool retagged = WriteTags(b, edit, error);
    TagData ta, tb, tw;
    bool read = ReadTags(a, ta) && ReadTags(b, tb) && ReadTags(w, tw);
    TrackId ia = g_lib.Add(a), ib = g_lib.Add(b), iw = g_lib.Add(w);
    for (TrackId id : {ia, ib, iw}) g_lib[id].flags |= Track::kInLibrary;
    g_lib.ApplyTags(ia, ta);
    g_lib.ApplyTags(ib, tb);
    g_lib.ApplyTags(iw, tw);
    bool sameHash = ta.audioHash != 0 && ta.audioHash == tb.audioHash && tb.title != ta.title;
    Check(retagged && read && sameHash, L"DUPLICATES two FLAC copies, different tags: audio hash %016llx / %016llx",
          (unsigned long long)ta.audioHash, (unsigned long long)tb.audioHash);

    auto groups = g_lib.FindDuplicateAudio();
    bool grouped = false, wavGrouped = false;
    for (const auto& group : groups) {
        bool hasA = std::find(group.begin(), group.end(), ia) != group.end();
        bool hasB = std::find(group.begin(), group.end(), ib) != group.end();
        grouped |= hasA && hasB;
        wavGrouped |= std::find(group.begin(), group.end(), iw) != group.end();
    }
    Check(grouped && !wavGrouped && tw.audioHash == 0, L"DUPLICATES FindDuplicateAudio groups the copies (%zu group(s)), WAV without an MD5 is not grouped",
          groups.size());
}

// The scanner sends a folder's listing in chunks (ScanResult.first/complete); the merge must
// behave exactly as with one result per folder. Synthetic chunks drive OnScanResult directly:
// moves found across chunks, files still listed later, missing tracks, the Inbox, and the
// first-scan fast path that does not defer new files.
void TestScanMerge() {
    using namespace app;
    Check(!PathIsUnder(L"C:\\Music\\a.flac", L"") && PathIsUnder(L"C:\\Music\\a.flac", L"C:\\Music") &&
              !PathIsUnder(L"C:\\MusicOther\\a.flac", L"C:\\Music"),
          L"SCANMERGE folder membership rejects empty roots and partial folder names");
    const std::wstring scope = L"C:\\ScanMerge";
    auto add = [](const wchar_t* path, uint64_t size, uint64_t time, uint32_t plays) {
        TrackId id = g_lib.Add(path);
        Track& t = g_lib[id];
        t.flags = (uint16_t)(Track::kInLibrary | Track::kLoaded);
        t.fileSize = size;
        t.fileTime = time;
        t.playCount = plays;
        return id;
    };
    auto post = [&](bool initial, bool first, bool complete, bool rootOk, std::vector<ScanFile> files) {
        OnScanResult(new ScanResult{scope, rootOk, initial, first, complete, std::move(files)}, false);
    };
    auto reset = [&] {
        g_lib = Library();
        g.playlists = {{L"Library", {}, PlaylistKind::Library}};
        g.active = 0;
    };
    std::vector<ScanFile> none;

    // A move found across chunks keeps the track, its stats and its playlists: the new file
    // waits for the whole listing before it may take a gone track's place.
    reset();
    g.inboxEnabled = true;
    TrackId t2 = add(L"C:\\ScanMerge\\Album\\01 Song.flac", 111, 222, 7);
    post(false, true, false, true, {{L"C:\\ScanMerge\\Other\\01 Song.flac", 111, 222}});
    bool held = g_lib.Count() == 1 && g_lib.Find(L"C:\\ScanMerge\\Album\\01 Song.flac") == t2 && g_lib[t2].playCount == 7;
    post(false, false, true, true, {});
    Check(held && g_lib.Count() == 1 && g_lib.Find(L"C:\\ScanMerge\\Other\\01 Song.flac") == t2 && g_lib[t2].playCount == 7,
          L"SCANMERGE a move found across chunks renames the track, no duplicate (%zu tracks)", g_lib.Count());

    // A destination indexed by another root or import keeps its row; the stale row is hidden as
    // missing instead of remaining live beside it.
    reset();
    TrackId stale = add(L"C:\\ScanMerge\\Z\\Song.flac", 111, 222, 7);
    post(false, true, false, true, {{L"C:\\ScanMerge\\A\\Song.flac", 111, 222}});
    TrackId destination = add(L"C:\\ScanMerge\\A\\Song.flac", 111, 222, 0);
    post(false, false, true, true, {});
    Check(g_lib.Count() == 2 && (g_lib[stale].flags & Track::kMissing) &&
              !(g_lib[destination].flags & Track::kMissing) && g_lib[destination].InLibrary() &&
              g_lib[stale].playCount == 7,
          L"SCANMERGE a move onto an already indexed path leaves one visible track");

    // The track's old file appears in a later chunk: the new file is a new track, nothing moves.
    reset();
    TrackId t3 = add(L"C:\\ScanMerge\\Album\\01 Song.flac", 111, 222, 7);
    post(false, true, false, true, {{L"C:\\ScanMerge\\Other\\01 Song.flac", 111, 222}});
    post(false, false, true, true, {{L"C:\\ScanMerge\\Album\\01 Song.flac", 111, 222}});
    Check(g_lib.Count() == 2 && g_lib.Find(L"C:\\ScanMerge\\Album\\01 Song.flac") == t3 &&
              g_lib.Find(L"C:\\ScanMerge\\Other\\01 Song.flac") != UINT32_MAX,
          L"SCANMERGE a still-listed file is not taken as moved (%zu tracks)", g_lib.Count());

    // Tracks gone from the scope are missing; tracks outside it are untouched.
    reset();
    TrackId t4 = add(L"C:\\ScanMerge\\Album\\Gone.flac", 1, 2, 0);
    TrackId outside = add(L"C:\\Elsewhere\\Kept.flac", 1, 2, 0);
    post(false, true, true, true, {});
    Check((g_lib[t4].flags & Track::kMissing) && !(g_lib[outside].flags & Track::kMissing),
          L"SCANMERGE a deleted file is missing, one outside the folder is not");

    // Automatic additions still respect existing Inbox tracks and exclusions.
    reset();
    g.inboxEnabled = false;
    TrackId kept = add(L"C:\\ScanMerge\\Album\\Kept.flac", 1, 2, 0);
    TrackId waiting = add(L"C:\\ScanMerge\\Album\\Waiting.flac", 3, 4, 0);
    g_lib[waiting].flags |= Track::kInbox;
    TrackId excluded = add(L"C:\\ScanMerge\\Album\\Excluded.flac", 5, 6, 0);
    g_lib[excluded].flags = Track::kExcluded | Track::kLoaded;
    post(false, true, false, true, {{L"C:\\ScanMerge\\Album\\Kept.flac", 1, 2},
                                   {L"C:\\ScanMerge\\Album\\Waiting.flac", 3, 4},
                                   {L"C:\\ScanMerge\\Album\\Excluded.flac", 5, 6},
                                   {L"C:\\ScanMerge\\Album\\Automatic.flac", 7, 8}});
    post(false, false, true, true, {});
    TrackId automatic = g_lib.Find(L"C:\\ScanMerge\\Album\\Automatic.flac");
    Check(automatic != kNone && g_lib[automatic].InLibrary() && !(g_lib[automatic].flags & Track::kInbox) &&
              !(g_lib[kept].flags & Track::kMissing) && (g_lib[waiting].flags & Track::kInbox) &&
              (g_lib[excluded].flags & Track::kExcluded) && !g_lib[excluded].InLibrary(),
          L"SCANMERGE automatic additions enter the library without accepting Inbox tracks or restoring exclusions");

    // A new file in a folder whose tracks may have moved waits and joins the Inbox when requested.
    reset();
    g.inboxEnabled = true;
    TrackId t5 = add(L"C:\\ScanMerge\\Album\\Kept.flac", 1, 2, 0);
    post(false, true, false, true, {{L"C:\\ScanMerge\\Album\\Kept.flac", 1, 2}, {L"C:\\ScanMerge\\Album\\New.flac", 3, 4}});
    post(false, false, true, true, {});
    TrackId added = g_lib.Find(L"C:\\ScanMerge\\Album\\New.flac");
    Check(added != UINT32_MAX && (g_lib[added].flags & Track::kInbox) && !(g_lib[t5].flags & Track::kMissing),
          L"SCANMERGE a new file waits for the listing and joins the Inbox");

    // The first scan of an empty library adds files as their chunks arrive.
    reset();
    post(true, true, false, true, {{L"C:\\ScanMerge\\A.flac", 1, 2}, {L"C:\\ScanMerge\\B.flac", 3, 4}});
    bool early = g_lib.Count() == 2;
    post(true, false, true, true, {{L"C:\\ScanMerge\\C.flac", 5, 6}});
    Check(early && g_lib.Count() == 3 && (g_lib[g_lib.Find(L"C:\\ScanMerge\\A.flac")].flags & Track::kInLibrary),
          L"SCANMERGE an empty library adds files as they arrive (%zu tracks)", g_lib.Count());

    // A malformed cue entry or an over-long path can reach a scan batch without being a legal
    // library path. It is skipped instead of indexing past the end of the track store.
    reset();
    std::wstring badPath = L"C:\\ScanMerge\\Bad";
    badPath += L'\x01';
    badPath += L"Name.flac";
    post(true, true, true, true, {{badPath, 1, 2}});
    Check(g_lib.Count() == 0, L"SCANMERGE a path the library can't represent is skipped (%zu tracks)", g_lib.Count());

    // An unreachable root marks nothing missing.
    reset();
    TrackId t7 = add(L"C:\\ScanMerge\\Album\\Kept.flac", 1, 2, 0);
    post(false, true, true, false, {});
    Check(!(g_lib[t7].flags & Track::kMissing), L"SCANMERGE an unreachable root marks nothing missing");

    reset();
    TrackId locked = add(L"C:\\ScanMerge\\Locked\\Song.flac", 11, 22, 7);
    post(false, true, false, true, {{L"C:\\ScanMerge\\Other\\Song.flac", 11, 22}});
    post(false, false, true, false, {});
    Check(!(g_lib[locked].flags & Track::kMissing) && g_lib.Path(locked) == L"C:\\ScanMerge\\Locked\\Song.flac" &&
              g_lib.Count() == 2 && g_lib[locked].playCount == 7,
          L"SCANMERGE a late incomplete chunk cannot infer a deletion or move from an unreadable folder");

    reset();
    TrackId retry = add(L"C:\\ScanMerge\\Retry.wav", 1, 2, 0);
    g_lib[retry].flags |= Track::kRgScanFailed;
    post(false, true, true, true, {{L"C:\\ScanMerge\\Retry.wav", 1, 2}});
    bool remembered = (g_lib[retry].flags & Track::kRgScanFailed) != 0;
    post(false, true, true, true, {{L"C:\\ScanMerge\\Retry.wav", 3, 4}});
    Check(remembered && !(g_lib[retry].flags & Track::kRgScanFailed),
          L"SCANMERGE failed ReplayGain attempts are retried only when the file changes");

    // The tag reads queued above must not leak into a later run.
    {
        std::lock_guard<std::mutex> lock(g.tagMx);
        g.tagQueue.clear();
        g.tagBatch.clear();
        g.tagBatchInFlight = false;
    }
    g_lib = Library();
    g.playlists = {{L"Library", {}, PlaylistKind::Library}};
    g.active = 0;
}
