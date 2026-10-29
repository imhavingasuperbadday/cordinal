// Memory changes must preserve byte identity, lifetime, cancellation and library semantics.
// Run as part of cordinal.exe --selftest (group "memory behaviour"); it exercises the library slabs,
// the string pool, the FileSource/FileRewrite recipes, the tag worker and the thumbnail loader with
// scratch files only, so it never touches the user's library or media.
#include "selftest_common.h"
#include "streamfile.h"
#include "testlog.h"
#include "thumbs.h"

#include <winioctl.h>
#include <set>

namespace {
int failures;

void Check(testlog::Condition ok, const wchar_t* text) {
    testlog::Check(ok, L"MEMORY %ls", text);
    if (!ok) ++failures;
}

bool WriteBytes(const std::wstring& path, std::string_view bytes) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(h, bytes.data(), (DWORD)bytes.size(), &written, nullptr) && written == bytes.size();
    CloseHandle(h);
    return ok;
}

std::string ReadBytes(const std::wstring& path) {
    FileSource file(path);
    std::string result;
    if (!file.Read(0, (size_t)file.Size(), result)) return {};
    return result;
}

template <typename Predicate> bool PumpUntil(Predicate done, DWORD ms = 5000) {
    ULONGLONG end = GetTickCount64() + ms;
    do {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (done()) return true;
        Sleep(5);
    } while (GetTickCount64() < end);
    return done();
}

// Receives the tag worker's result batches, optionally holding the first one to simulate a slow UI.
struct Mailbox {
    bool holdTags = true;
    size_t tags = 0, batches = 0;
    std::unique_ptr<std::vector<app::TagResult>> held;
};

LRESULT CALLBACK TestWindow(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE)
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
    auto* box = reinterpret_cast<Mailbox*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (msg == app::WM_APP_META && box) {
        auto* batch = reinterpret_cast<std::vector<app::TagResult>*>(lp);
        box->tags += batch->size();
        ++box->batches;
        if (box->holdTags) box->held.reset(batch);
        else app::AcceptTagBatch(batch);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void TestAutoMeasureSelection() {
    g_lib = Library();
    for (unsigned i = 0; i < 4096; ++i) {
        TrackId id = g_lib.Add(L"C:\\Memory\\scheduler-" + std::to_wstring(i) + L".flac");
        Track& t = g_lib[id];
        t.flags = Track::kLoaded;
        t.album = g_lib.Intern("Album " + std::to_string((i * 37) % 97));
        if (i % 11 == 0) t.flags |= Track::kMissing;
        if (i % 13 == 0) t.flags |= Track::kRgScanFailed;
        if (i % 17 == 0) { t.rgTrackGain = -100; t.flags |= Track::kRgScanned; }
    }
    auto needs = [](TrackId id) {
        const Track& t = g_lib[id];
        return t.Loaded() && !(t.flags & (Track::kMissing | Track::kFailed | Track::kRgScanFailed)) &&
               (t.rgTrackGain == kNoGain || (!t.rgHigh && !(t.flags & Track::kRgScanned)));
    };
    bool matches = true;
    for (auto upcoming : {std::vector<TrackId>{}, std::vector<TrackId>{4001, 2, 4001, UINT32_MAX, 11, 17, 13, 3}}) {
        std::vector<TrackId> expected;
        std::set<AlbumId> soon;
        for (TrackId id : upcoming) {
            if (id >= g_lib.Count() || !needs(id) || std::find(expected.begin(), expected.end(), id) != expected.end()) continue;
            expected.push_back(id);
            soon.insert(AlbumKeyOf(g_lib[id]));
        }
        std::vector<TrackId> rest;
        for (TrackId id = 0; id < g_lib.Count(); ++id)
            if (needs(id) && std::find(expected.begin(), expected.end(), id) == expected.end()) rest.push_back(id);
        std::sort(rest.begin(), rest.end(), [&](TrackId a, TrackId b) {
            auto ka = std::make_tuple(soon.count(AlbumKeyOf(g_lib[a])) == 0, AlbumKeyOf(g_lib[a]), a);
            auto kb = std::make_tuple(soon.count(AlbumKeyOf(g_lib[b])) == 0, AlbumKeyOf(g_lib[b]), b);
            return ka < kb;
        });
        expected.insert(expected.end(), rest.begin(), rest.end());
        for (size_t limit : {size_t(0), size_t(1), size_t(3), size_t(300), size_t(5000)}) {
            auto wanted = expected;
            wanted.resize(std::min(limit, wanted.size()));
            matches &= app::AutoMeasureTracks(upcoming, limit) == wanted;
        }
    }
    Check(matches, L"bounded ReplayGain batches match full-sort priority, omit duplicates/invalid/failed/measured tracks, and obey limits");
    g_lib = Library();
}

// Library and string-pool stability: track addresses and stats must survive slab growth and
// renames, and a borrowed path snapshot must keep its original bytes while the pool grows under it.
void TestStores(const std::wstring& scratch) {
    Library library;
    TrackId first = library.Add(L"C:\\Memory\\first.flac");
    Track* address = &library[first];
    address->rating = 90;
    TrackPath snapshot = library.SnapshotPath(first);
    std::atomic<bool> snapshotStable{true};
    std::thread reader([&] {
        for (unsigned i = 0; i < 10000; ++i)
            if (snapshot.Path() != L"C:\\Memory\\first.flac") snapshotStable = false;
    });
    for (unsigned i = 0; i < 8193; ++i) library.Add(L"C:\\Memory\\Track " + std::to_wstring(i) + L".flac");
    reader.join();
    bool stable = &library[first] == address && address->rating == 90;
    for (TrackId i = 0; i < library.Count(); i += 17) {
        std::wstring previous = library.Path(i);
        std::wstring renamed = L"D:\\Moved\\Track " + std::to_wstring(i) + L".flac";
        library.Rename(i, renamed);
        stable &= library.Find(renamed) == i && library.Find(previous) == UINT32_MAX;
    }
    Check(stable, L"track addresses/stats remain stable through slab growth and path-index renames");
    Check(snapshotStable && snapshot.Path() == L"C:\\Memory\\first.flac",
          L"borrowed path snapshots retain their original bytes through concurrent pool growth and renames");

    StringPool pool;
    std::vector<StringPool::Handle> handles;
    for (unsigned i = 0; i < 5000; ++i) handles.push_back(pool.Intern("Cross-block string " + std::to_string(i)));
    bool strings = true;
    for (unsigned i = 0; i < handles.size(); ++i)
        strings &= pool.Get(handles[i]) == "Cross-block string " + std::to_string(i) &&
                   pool.Intern(pool.Get(handles[i])) == handles[i];
    std::string large(150000, 'x');
    large[65535] = '\0';
    auto handle = pool.Intern(large);
    Check(strings && pool.Get(handle) == large && pool.Lookup(large) == handle,
          L"UTF-8 handles survive block boundaries, deduplication and oversized strings containing NUL");

    // This exceeds both the string slab and the database reader's 64 KiB window.
    g_lib = Library();
    TrackId id = g_lib.Add(L"C:\\Memory\\large-comment.flac");
    TagData tags;
    tags.comment = large;
    tags.title = "Before and after the read window";
    g_lib.ApplyTags(id, tags);
    g_lib[id].flags |= Track::kInLibrary;
    std::vector<Playlist> playlists{{L"Long metadata", {id, id}}};
    std::wstring db = scratch + L"\\large.cdb";
    bool saved = SaveLibrary(db, playlists);
    g_lib = Library();
    bool loaded = LoadLibrary(db, playlists);
    id = g_lib.Find(L"C:\\Memory\\large-comment.flac");
    Check(saved && loaded && id != UINT32_MAX && g_lib.View(g_lib[id].comment) == large &&
              playlists.size() == 1 && playlists[0].items == std::vector<TrackId>{id, id},
          L"streamed database round-trip retains large metadata and duplicate playlist entries");
    DeleteFileW(db.c_str());
    DeleteFileW((db + L".bak").c_str());
    g_lib = Library();
}

// FileRewrite/FileSource recipes: generated, copied and zeroed ranges are byte-identical across
// boundaries; an invalid range or a blocked commit leaves the original untouched and leaves no
// temp files. The last case writes one word at a 4 GiB+ offset through a sparse file.
void TestRecipes(const std::wstring& scratch) {
    std::wstring path = scratch + L"\\recipe.bin";
    std::string original(200003, '\0');
    for (size_t i = 0; i < original.size(); ++i) original[i] = (char)(i * 31 + 7);
    bool ok = WriteBytes(path, original);
    std::wstring error;
    {
        FileSource source(path);
        FileRewrite edit(path, source, error);
        ok = ok && edit.Write("header") && edit.Copy(65531, 70003) && edit.Zeros(70001) &&
             edit.Copy(199990, 13) && edit.Write("tail") && edit.Commit();
    }
    std::string expected = "header" + original.substr(65531, 70003) + std::string(70001, '\0') +
                           original.substr(199990, 13) + "tail";
    Check(ok && ReadBytes(path) == expected, L"generated bytes, zero padding and copied ranges are byte-identical across copy boundaries");

    {
        FileSource source(path);
        FileRewrite edit(path, source, error);
        ok = edit.Write("uncommitted") && !edit.Copy(source.Size(), 1) && !edit.Commit();
    }
    Check(ok && ReadBytes(path) == expected, L"invalid source ranges abort without modifying the original");
    HANDLE locked = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    {
        FileSource source(path);
        FileRewrite edit(path, source, error);
        ok = edit.Write("replacement") && !edit.Commit();
    }
    if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
    WIN32_FIND_DATAW data{};
    HANDLE leftover = FindFirstFileW((path + L".cordinal-tmp-*").c_str(), &data);
    Check(locked != INVALID_HANDLE_VALUE && ok && ReadBytes(path) == expected && leftover == INVALID_HANDLE_VALUE,
          L"a blocked replacement preserves the original and cleans every temporary file");
    if (leftover != INVALID_HANDLE_VALUE) FindClose(leftover);
    DeleteFileW(path.c_str());

    // A sparse fixture validates 64-bit reads without consuming gigabytes of disk or RAM.
    HANDLE sparse = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD written = 0;
    bool supported = sparse != INVALID_HANDLE_VALUE &&
                     DeviceIoControl(sparse, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &written, nullptr);
    LARGE_INTEGER at{};
    at.QuadPart = (1ll << 32) + 19;
    ok = supported && SetFilePointerEx(sparse, at, nullptr, FILE_BEGIN) &&
         WriteFile(sparse, "wide", 4, &written, nullptr) && written == 4;
    if (sparse != INVALID_HANDLE_VALUE) CloseHandle(sparse);
    {
        FileSource source(path);
        std::string bytes;
        if (supported)
            Check(ok && source.Size() == (uint64_t)at.QuadPart + 4 && source.Read(at.QuadPart, 4, bytes) && bytes == "wide",
                L"file offsets above 4 GiB are read exactly");
        else testlog::Skip(L"64-bit sparse reads: filesystem lacks sparse-file support");
    }
    DeleteFileW(path.c_str());
}

// The tag worker and the thumbnail loader under adversarial scheduling: batching to a slow UI,
// stale reads after a rename, cancellation of queued work, backpressure on pending/ready images,
// and shutdown while producers are blocked.
void TestWorkers(const std::wstring& scratch) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = TestWindow;
    wc.lpszClassName = L"CordinalMemoryTest";
    RegisterClassW(&wc);
    Mailbox box;
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, &box);
    g_lib = Library();
    std::vector<TrackId> ids;
    for (unsigned i = 0; i < 300; ++i) ids.push_back(g_lib.Add(scratch + L"\\missing-" + std::to_wstring(i) + L".flac"));
    app::g.tagQuit = false;
    app::QueueTags(ids);
    std::thread worker(app::TagWorker, hwnd);
    bool arrived = PumpUntil([&] { return !!box.held; });
    PumpUntil([] { return false; }, 100);
    Check(arrived && box.batches == 1 && box.tags == 64, L"a slow UI receives only one metadata batch, with all remaining IDs retained");
    app::QueueTags({ids[1], ids[1]});
    app::QueueTags({ids[1]});
    if (box.held) {
        (*box.held)[1].success = true;
        (*box.held)[1].tags.title = "Stale snapshot";
    }
    g_lib.Rename(ids[0], scratch + L"\\moved.flac");
    box.holdTags = false;
    if (box.held) app::AcceptTagBatch(box.held.release());
    Check(!g_lib[ids[1]].Loaded() && app::g.tagPending.contains(ids[1]),
          L"a changed same-path file rejects its held metadata snapshot and retains a fresh read");
    bool complete = PumpUntil([&] { return box.tags == ids.size() + 1; });
    Check(complete && !g_lib[ids[0]].Loaded() && !g_lib[ids.back()].Loaded() && (g_lib[ids.back()].flags & Track::kTagFailed), L"all queued jobs complete and a stale read cannot overwrite a moved track");
    app::QueueTags({ids[0]});
    Check(PumpUntil([&] { return box.tags == ids.size() + 2; }) && (g_lib[ids[0]].flags & Track::kTagFailed), L"a moved track can be queued again using its current path");
    const std::wstring stamped = scratch + L"\\metadata-stamp.wav";
    bool written = WriteBytes(stamped, MakeSineWav(0.01, 0.1));
    TrackId stampedId = g_lib.Add(stamped);
    app::QueueTags({stampedId});
    WIN32_FILE_ATTRIBUTE_DATA attrs{};
    bool measured = PumpUntil([&] { return box.tags == ids.size() + 3; }) &&
                    GetFileAttributesExW(stamped.c_str(), GetFileExInfoStandard, &attrs);
    Check(written && measured && g_lib[stampedId].Loaded() && g_lib[stampedId].fileSize == attrs.nFileSizeLow &&
              g_lib[stampedId].fileTime == ((uint64_t)attrs.ftLastWriteTime.dwHighDateTime << 32 | attrs.ftLastWriteTime.dwLowDateTime),
          L"metadata readback refreshes the source stamp for files outside watched library folders");
    {
        std::lock_guard<std::mutex> lock(app::g.tagMx);
        app::g.tagQuit = true;
    }
    app::g.tagCv.notify_all();
    worker.join();
    app::g.tagQuit = false;
    app::g.tagBatchInFlight = false;
    app::g.tagQueue.clear();
    app::g.tagBatch.clear();
    g_lib = Library();

    // WIC identifies the image from its bytes; using BMP keeps this fixture self-contained.
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + 512 * 512 * 4;
    ih.biSize = sizeof(ih);
    ih.biWidth = ih.biHeight = 512;
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    std::string image(reinterpret_cast<const char*>(&fh), sizeof(fh));
    image.append(reinterpret_cast<const char*>(&ih), sizeof(ih));
    image.append(512 * 512 * 4, '\x40');
    ThumbLoader thumbs;
    const std::wstring cacheDir = scratch + L"\\thumbs";
    const std::wstring legacyMiss = cacheDir + L"\\00000000000dbba2-0.none";
    auto requestCover = [&] {
        thumbs.Request(900002, scratch + L"\\missing.flac", 128);
        std::unique_ptr<ThumbResult> result;
        PumpUntil([&] { result = thumbs.TakeResult(); return !!result; });
        return result;
    };
    thumbs.Start(hwnd, WM_APP + 50, cacheDir);
    auto missing = requestCover();
    Check(missing && missing->image.Empty() && GetFileAttributesW(legacyMiss.c_str()) == INVALID_FILE_ATTRIBUTES,
          L"missing artwork is returned without a persistent failure marker");
    thumbs.Stop();
    bool fixture = WriteBytes(scratch + L"\\cover.png", image);
    bool legacy = WriteBytes(legacyMiss, {});
    thumbs.Start(hwnd, WM_APP + 50, cacheDir);
    auto recovered = requestCover();
    Check(fixture && legacy && recovered && !recovered->image.Empty(),
          L"artwork added after a miss loads across restarts despite a legacy failure marker");
    thumbs.Stop();
    DeleteFileW((scratch + L"\\cover.png").c_str());
    thumbs.Start(hwnd, WM_APP + 50, cacheDir);
    auto cached = requestCover();
    Check(cached && !cached->image.Empty(),
          L"a legacy failure marker cannot hide a valid cached cover when its source is unavailable");
    fixture = WriteBytes(scratch + L"\\cover.png", image) && fixture;
    for (uint64_t i = 1; i <= 300; ++i) thumbs.Request(i, scratch + L"\\missing.flac", 128);
    bool ready = PumpUntil([&] { return thumbs.Memory().ready == 2; });
    auto usage = thumbs.Memory();
    Check(fixture && ready && usage.pending <= 128 && usage.readyBytes == 2 * 128 * 128 * 4,
          L"thumbnail backpressure bounds pending jobs and completed image bytes while the UI is busy");
    thumbs.DropPending();
    thumbs.Request(900001, scratch + L"\\missing.flac", 128);
    bool newest = false, stale = false;
    PumpUntil([&] {
        while (auto result = thumbs.TakeResult()) {
            stale |= result->key != 900001;
            newest |= result->key == 900001 && !result->image.Empty();
        }
        return newest;
    });
    Check(newest && !stale, L"dropping pending work cancels old generations and permits a fresh cover request");
    for (uint64_t i = 1; i <= 20; ++i) thumbs.Request(i, scratch + L"\\missing.flac", 128);
    PumpUntil([&] { return thumbs.Memory().ready == 2; });
    ULONGLONG start = GetTickCount64();
    thumbs.Stop();
    Check(GetTickCount64() - start < 2000 && thumbs.Memory().ready == 0 && thumbs.Memory().pending == 0,
          L"stopping thumbnail workers wakes blocked producers and releases all undelivered images");
    thumbs.Start(hwnd, WM_APP + 50, cacheDir, 1, true);
    for (uint64_t i=1;i<=300;++i) thumbs.Request(i,scratch+L"\\missing.flac",128);
    ready=PumpUntil([&] { return thumbs.Memory().ready==1; });
    usage=thumbs.Memory();
    Check(ready && usage.pending<=64 && usage.readyBytes==128*128*4,
          L"reduced thumbnail queues hold half the pending jobs and completed pixels");
    thumbs.DropPending();
    thumbs.Request(900003,scratch+L"\\missing.flac",128);
    newest=PumpUntil([&] { auto result=thumbs.TakeResult(); return result && result->key==900003 && !result->image.Empty(); });
    Check(newest,L"reduced thumbnail workers discard old work and accept a fresh request");
    thumbs.Stop();
    {
        using namespace app;
        auto oldHwnd=g.hwnd;
        auto playlists=std::move(g.playlists);
        auto view=std::move(g.view);
        auto oldActive=g.active;
        g.hwnd=hwnd;
        g.playlists={{L"Cover memory",{}}}; g.active=0;
        bool loaded=true, bounded=true;
        for (unsigned i=0;i<20;++i) {
            TrackId id=g_lib.Add(scratch+L"\\cover-track-"+std::to_wstring(i)+L".flac");
            Active().items.push_back(id); g.view.push_back(i);
            TrackGroup group{}; group.firstView=i;
            loaded &= PumpUntil([&] { OnGroupArt(); const auto* art=GroupCover(group,512); return art && !art->Empty(); });
            bounded &= GroupArtBytes()<=(8u<<20);
        }
        Check(loaded && bounded,L"large track-list covers stay within the 8 MiB pixel budget while scrolling");
        ReleaseGroupArt();
        TrackGroup first{};
        bool reloaded=PumpUntil([&] { OnGroupArt(); const auto* art=GroupCover(first,512); return art && !art->Empty(); });
        Check(reloaded && GroupArtBytes()==512*512*4,L"releasing hidden cover pixels permits a fresh request from the disk cache");
        ViewSize oldSize=g.size;
        g.size=ViewSize::Compact;
        UpdateViewMemory();
        Check(ReducedMemoryMode() && ViewMemoryBudget(8u<<20)==(4u<<20) && GroupArtBytes()==0,
              L"compact mode immediately releases old covers and halves UI cache budgets");
        loaded=bounded=true;
        for (unsigned i=0;i<20;++i) {
            TrackGroup group{}; group.firstView=i;
            loaded &= PumpUntil([&] { OnGroupArt(); const auto* art=GroupCover(group,512); return art && !art->Empty(); });
            bounded &= GroupArtBytes()<=(4u<<20);
        }
        Check(loaded && bounded && GroupArtBytes()==(4u<<20),
              L"compact scrolling retains at most half the normal cover pixels");
        bool restored=true;
        for (ViewSize size : {ViewSize::Normal,ViewSize::Spotlight,ViewSize::Big}) {
            g.size=size;
            restored &= !ReducedMemoryMode() && ViewMemoryBudget(8u<<20)==(8u<<20);
        }
        Check(restored,L"normal and both big modes restore the normal cache allowance");
        g.size=oldSize;
        StopGroupArt();
        Check(GroupArtBytes()==0,L"stopping group artwork releases all retained pixel allocations");
        g.hwnd=oldHwnd; g.playlists=std::move(playlists); g.view=std::move(view); g.active=oldActive;
        g_lib=Library();
    }
    DestroyWindow(hwnd);
    DeleteTestFolder(scratch + L"\\thumbs");
    DeleteFileW((scratch + L"\\cover.png").c_str());
}
}  // namespace

int TestMemoryBehavior(const std::wstring& scratchDir, FILE* log) {
    testlog::SetLog(log);
    failures = 0;
    std::wstring scratch = scratchDir + L"\\memory-tests";
    CreateDirectoryW(scratch.c_str(), nullptr);
    testlog::Group checkGroup(L"memory behaviour");
    TestAutoMeasureSelection();
    TestStores(scratch);
    TestRecipes(scratch);
    TestWorkers(scratch);
    RemoveDirectoryW(scratch.c_str());
    return failures;
}
