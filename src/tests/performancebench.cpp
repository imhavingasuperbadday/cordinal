// Production library/search/UI/DSP paths in isolated processes, without user state.
#include "app.h"
#include "peaks.h"
#include <cstdio>
#include <random>

bool ReferenceSearch(const Library& library, TrackId id, const std::vector<std::wstring>& words);

int RunPerformanceBenchmark(const std::vector<std::wstring>& args) {
    if (args.size() < 3) return 2;
    FILE* log = nullptr;
    if (_wfopen_s(&log, args[2].c_str(), L"w, ccs=UTF-8") || !log) return 3;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    auto measure = [&](const wchar_t* name, auto run) {
        PROCESS_MEMORY_COUNTERS_EX before{}, after{};
        GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&before, sizeof(before));
        ULONG64 cycles1, cycles2;
        QueryProcessCycleTime(GetCurrentProcess(), &cycles1);
        LARGE_INTEGER a, b;
        QueryPerformanceCounter(&a);
        run();
        QueryPerformanceCounter(&b);
        QueryProcessCycleTime(GetCurrentProcess(), &cycles2);
        GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&after, sizeof(after));
        fwprintf(log, L"%ls ms=%.3f cycles=%llu private_bytes=%zu peak_private_bytes=%zu working_set=%zu peak_working_set=%zu private_change=%lld\n",
                 name, 1000.0*(b.QuadPart-a.QuadPart)/frequency.QuadPart, (unsigned long long)(cycles2-cycles1),
                 after.PrivateUsage, after.PeakPagefileUsage, after.WorkingSetSize, after.PeakWorkingSetSize,
                 (long long)after.PrivateUsage-(long long)before.PrivateUsage);
        fflush(log);
    };
    bool ok = true;
    unsigned count = args.size() > 3 ? (unsigned)wcstoul(args[3].c_str(), nullptr, 10) : 100000;
    if (!count || count > 2000000) { fclose(log); return 2; }
    const std::wstring workload = args.size() > 4 ? args[4] : L"";
    const bool unique = workload == L"search-unique";
    measure(L"populate", [&] {
        for (unsigned i=0;i<count;++i) {
            auto id = g_lib.Add(L"C:\\Performance\\Album " + std::to_wstring(unique ? i : i/10) + L"\\Track " + std::to_wstring(i) + L".flac");
            TagData tags;
            tags.title="Track " + std::to_string(i); tags.artist="Artist " + std::to_string(i/100);
            tags.album="Album " + std::to_string(i/10); tags.date="2026"; tags.genre="Rock";
            if (unique) {
                const auto suffix = std::to_string(i);
                tags.artist="Artist "+suffix; tags.album="Album "+suffix;
                tags.date="2026 "+suffix; tags.genre="Rock "+suffix;
                tags.albumArtist="Album artist "+suffix; tags.composer="Composer "+suffix;
                tags.comment="Comment "+suffix; tags.grouping="Group "+suffix; tags.label="Label "+suffix;
            }
            tags.duration=180; tags.track=i%10+1;
            g_lib.ApplyTags(id,tags);
            g_lib[id].flags |= Track::kInLibrary;
        }
    });
    std::vector<TrackId> items;
    for (TrackId id=0;id<count;++id) items.push_back(id);
    if (workload == L"ranks") {
        // Artist/album tags repeat, while titles and file names are mostly distinct. This isolates
        // the shared natural-order dictionary used by library sorting and the column browser.
        std::vector<StringPool::Handle> handles;
        handles.reserve(count * 4);
        for (auto id : items) {
            const auto& t = g_lib[id];
            for (auto h : {t.title, t.file, t.artist, t.album}) handles.push_back(h);
        }
        measure(L"string_ranks", [&] {
            app::StringRanks ranks(std::move(handles));
            for (auto id : items) {
                const auto& t = g_lib[id];
                ok &= ranks(t.title) != UINT32_MAX && ranks(t.file) != UINT32_MAX;
            }
        });
        fwprintf(log, L"result=%ls\n", ok ? L"PASS" : L"FAIL");
        fclose(log); CoUninitialize(); return ok ? 0 : 1;
    }
    // A focused comparison avoids unrelated UI/artwork work and alternates the original
    // matcher with the query-local matcher in the same process and on the same track order.
    if (workload == L"search" || unique || workload == L"search-shuffled") {
        if (workload == L"search-shuffled") {
            std::mt19937 random(261020);
            std::shuffle(items.begin(), items.end(), random);
        }
        for (auto query : {L"Track", L"Artist", L"absent value", L"Rock 2026", L"\"Album 12\" Track"}) {
            const auto words = FilterWords(query);
            std::vector<TrackId> expected, actual;
            expected.reserve(count); actual.reserve(count);
            for (int pass = 0; pass < 3; ++pass) {
                auto reference = [&] {
                    expected.clear();
                    measure((L"reference " + std::wstring(query)).c_str(), [&] {
                        for (auto id : items) if (ReferenceSearch(g_lib, id, words)) expected.push_back(id);
                    });
                };
                auto cached = [&] {
                    actual.clear();
                    measure((L"cached " + std::wstring(query)).c_str(), [&] {
                        TrackMatcher matcher(g_lib, words);
                        for (auto id : items) if (matcher.Matches(id)) actual.push_back(id);
                    });
                };
                if (pass & 1) { cached(); reference(); } else { reference(); cached(); }
                ok &= actual == expected;
                std::reverse(items.begin(), items.end());
            }
        }
        fwprintf(log, L"result=%ls\n", ok ? L"PASS" : L"FAIL");
        fclose(log); CoUninitialize(); return ok ? 0 : 1;
    }
    measure(L"library_sort_3", [&] {
        for (int i=0;i<3;++i) { std::reverse(items.begin(),items.end()); app::SortLibraryOrder(items); }
    });
    app::g.playlists={{L"Performance", std::move(items)}};
    app::g.active=0;
    app::g.inst=GetModuleHandleW(nullptr);
    app::g.dpi=96;
    app::g.dataDir=DirectoryOf(args[2]);
    app::g.iniPath=app::g.dataDir+L"\\performance-settings.ini";
    app::g.rgOn=app::g.rgAutoScan=false;
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);
    WNDCLASSW wc{};
    wc.lpfnWndProc=DefWindowProcW; wc.hInstance=app::g.inst; wc.lpszClassName=L"CordinalPerformanceTest";
    RegisterClassW(&wc);
    app::g.hwnd=CreateWindowW(wc.lpszClassName,L"",WS_OVERLAPPEDWINDOW,0,0,1163,752,nullptr,nullptr,wc.hInstance,nullptr);
    ok &= app::g.hwnd!=nullptr;
    app::CreateFonts(); app::CreateControls(app::g.hwnd); app::ApplyFonts(); app::Layout();
    app::g.filter=L"Track";
    measure(L"search_all_3", [&] { for (int i=0;i<3;++i) app::RebuildView(); });
    ok &= app::g.view.size()==count;
    app::g.filter=L"absent value";
    measure(L"search_none_3", [&] { for (int i=0;i<3;++i) app::RebuildView(); });
    ok &= app::g.view.empty();
    app::g.rgAlbumsRevision=g_lib.Revision();
    measure(L"ui_ticks_100", [&] { for (int i=0;i<100;++i) app::MainTick(); });
    {
        // Wide/high-DPI cover columns: scroll through 32 real WIC/thumbnail requests.
        const std::wstring folder=DirectoryOf(args[2])+L"\\performance-covers";
        SHCreateDirectoryExW(nullptr,folder.c_str(),nullptr);
        const std::wstring cover=folder+L"\\cover.png";
        BITMAPFILEHEADER fh{}; BITMAPINFOHEADER ih{};
        fh.bfType=0x4D42; fh.bfOffBits=sizeof(fh)+sizeof(ih); fh.bfSize=fh.bfOffBits+1024*1024*4;
        ih.biSize=sizeof(ih); ih.biWidth=ih.biHeight=1024; ih.biPlanes=1; ih.biBitCount=32;
        FILE* file=nullptr;
        if (!_wfopen_s(&file,cover.c_str(),L"wb") && file) {
            std::vector<uint32_t> pixels(1024*1024,0x00806040);
            fwrite(&fh,sizeof(fh),1,file); fwrite(&ih,sizeof(ih),1,file);
            fwrite(pixels.data(),sizeof(uint32_t),pixels.size(),file); fclose(file);
        } else ok=false;
        app::Active().items.clear(); app::g.view.clear();
        measure(L"scroll_32_large_covers",[&] {
            for (unsigned i=0;i<32;++i) {
                auto id=g_lib.Add(folder+L"\\track-"+std::to_wstring(i)+L".flac");
                app::Active().items.push_back(id); app::g.view.push_back(i);
                app::TrackGroup group{}; group.firstView=i;
                const ULONGLONG deadline=GetTickCount64()+5000;
                bool ready=false;
                do {
                    app::OnGroupArt();
                    const auto* art=app::GroupCover(group,1024);
                    ready=art && !art->Empty();
                    if (!ready) Sleep(1);
                } while (!ready && GetTickCount64()<deadline);
                ok &= ready;
            }
        });
        fwprintf(log,L"cover_cache_bytes=%zu\n",app::GroupArtBytes());
        measure(L"release_cover_cache",[&]{app::StopGroupArt();});
        DeleteFileW(cover.c_str()); RemoveDirectoryW(folder.c_str());
    }
    const auto audio=args[1]+L"\\media\\sine.flac";
    std::wstring error;
    auto dec=OpenDecoder(audio,48000,error);
    if (dec) {
        float pcm[8192];
        auto scan=[&](int mode) {
            for (int pass=0;pass<20;++pass) {
                ok &= dec->Seek(0);
                LoudnessMeter meter(dec->Rate()); TruePeak peak; PeakAccumulator wave;
                wave.Begin((uint64_t)(dec->Duration()*dec->Rate()));
                while (size_t n=dec->Read(pcm,4096)) {
                    if (mode==1 || mode==3) meter.Add(pcm,n);
                    if (mode==2 || mode==3) peak.Add(pcm,n);
                    if (mode==3) wave.Add(pcm,n);
                }
                if (mode==1 || mode==3) ok &= std::isfinite(meter.Integrated());
                if (mode==2 || mode==3) ok &= peak.Peak()>0;
            }
        };
        measure(L"decode_20",[&]{scan(0);});
        measure(L"decode_loudness_20",[&]{scan(1);});
        measure(L"decode_truepeak_20",[&]{scan(2);});
        measure(L"replaygain_20",[&]{scan(3);});
        PipelineTiming timing;
        measure(L"playback_pipeline_20",[&]{ok &= BenchmarkPlayerPipeline(audio,48000,20,timing);});
    } else ok=false;
    DestroyWindow(app::g.hwnd);
    app::g.hwnd=nullptr;
    fwprintf(log,L"result=%ls\n",ok?L"PASS":L"FAIL");
    fclose(log);
    CoUninitialize();
    return ok?0:1;
}
