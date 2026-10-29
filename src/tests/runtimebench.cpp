// Real app startup/background work in an isolated profile. The optional window-modes scenario
// measures each size on a private desktop; append "playing" to exercise muted continuous playback.
// "switching" measures 80 muted track changes; append "lyrics" to open the lyrics panel too.
#include "app.h"
#include "peaks.h"
#include <cstdio>

bool MeasureWebRuntime(FILE* log);

namespace {
bool RenderReview(HWND window, const std::wstring& path) {
    if (!window) return false;
    RECT client{}; GetClientRect(window, &client);
    if (client.right <= 0 || client.bottom <= 0) return false;
    HDC screen=GetDC(window), dc=CreateCompatibleDC(screen);
    BITMAPINFO image{};
    image.bmiHeader={sizeof(BITMAPINFOHEADER),client.right,-client.bottom,1,32,BI_RGB};
    void* pixels=nullptr;
    HBITMAP bitmap=CreateDIBSection(screen,&image,DIB_RGB_COLORS,&pixels,nullptr,0);
    HGDIOBJ old=SelectObject(dc,bitmap);
    UpdateWindow(window);
    bool rendered=bitmap && PrintWindow(window,dc,PW_CLIENTONLY);
    GdiFlush();
    BITMAPFILEHEADER header{};
    header.bfType=0x4D42; header.bfOffBits=sizeof(header)+sizeof(image.bmiHeader);
    DWORD pixelBytes=client.right*client.bottom*4;
    header.bfSize=header.bfOffBits+pixelBytes;
    FILE* preview=nullptr;
    if (rendered && !_wfopen_s(&preview,path.c_str(),L"wb") && preview) {
        rendered=fwrite(&header,1,sizeof(header),preview)==sizeof(header) &&
                 fwrite(&image.bmiHeader,1,sizeof(image.bmiHeader),preview)==sizeof(image.bmiHeader) &&
                 fwrite(pixels,1,pixelBytes,preview)==pixelBytes;
        fclose(preview);
    } else rendered=false;
    SelectObject(dc,old); DeleteObject(bitmap); DeleteDC(dc); ReleaseDC(window,screen);
    return rendered;
}
}

int RunRuntimeBenchmark(const std::vector<std::wstring>& args) {
    if (args.size()<3) return 2;
    FILE* log=nullptr;
    if (_wfopen_s(&log,args[2].c_str(),L"w, ccs=UTF-8") || !log) return 3;
    const bool modes=args.size()>3 && args[3]==L"window-modes";
    // The mode benchmark renders real windows on a private desktop, without changing the user's UI.
    HDESK original=GetThreadDesktop(GetCurrentThreadId());
    HDESK desktop=modes?CreateDesktopW((L"CordinalMemory-"+std::to_wstring(GetCurrentProcessId())).c_str(),
                                      nullptr,nullptr,0,GENERIC_ALL,nullptr):nullptr;
    if (modes && (!desktop || !SetThreadDesktop(desktop))) {
        fwprintf(log,L"private_desktop_error=%lu\n",GetLastError());
        if (desktop) CloseDesktop(desktop);
        fclose(log);
        return 4;
    }
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    wchar_t absolute[32768];
    GetFullPathNameW(args[1].c_str(),32768,absolute,nullptr);
    const std::wstring source=std::wstring(absolute)+L"\\media\\sine.flac";
    GetFullPathNameW(args[2].c_str(),32768,absolute,nullptr);
    const std::wstring profile=DirectoryOf(absolute)+L"\\runtime-profile-"+std::to_wstring(GetCurrentProcessId());
    SHCreateDirectoryExW(nullptr,profile.c_str(),nullptr);
    app::g.dataDir=profile; app::g.iniPath=profile+L"\\settings.ini"; app::g.libraryPath=profile+L"\\library.cdb";
    const bool scanning=args.size()>3 && args[3]==L"scan";
    const bool steadyPlayback=args.size()>3 && args[3]==L"playing";
    const bool switching=args.size()>3 && args[3]==L"switching";
    app::IniWrite(L"Window",L"Theme",L"2");
    app::IniWrite(L"Library",L"View",modes?L"2":L"0");
    app::IniWrite(L"ReplayGain",L"AutoMeasure",L"1");
    if (switching && args.size()>4 && args[4]==L"lyrics") app::IniWrite(L"Window",L"Lyrics",L"1");
    TagData tags;
    bool ok=ReadTags(source,tags);
    if (modes || switching) {
        BITMAPFILEHEADER fh{};
        BITMAPINFOHEADER ih{};
        fh.bfType=0x4D42; fh.bfOffBits=sizeof(fh)+sizeof(ih); fh.bfSize=fh.bfOffBits+512*512*4;
        ih.biSize=sizeof(ih); ih.biWidth=ih.biHeight=512; ih.biPlanes=1; ih.biBitCount=32;
        std::string image(reinterpret_cast<const char*>(&fh),sizeof(fh));
        image.append(reinterpret_cast<const char*>(&ih),sizeof(ih)); image.append(512*512*4,'\x40');
        FILE* cover=nullptr;
        if (!_wfopen_s(&cover,(profile+L"\\cover.png").c_str(),L"wb") && cover) {
            ok=ok && fwrite(image.data(),1,image.size(),cover)==image.size();
            fclose(cover);
        } else ok=false;
    }
    for (unsigned i=0;i<256 && ok;++i) {
        const std::wstring path=profile+L"\\track-"+std::to_wstring(i)+L".flac";
        ok=CreateHardLinkW(path.c_str(),source.c_str(),nullptr) || CopyFileW(source.c_str(),path.c_str(),TRUE);
        TrackId id=g_lib.Add(path);
        tags.album="Runtime album "+std::to_string(i/10);
        g_lib.ApplyTags(id,tags); g_lib[id].flags|=Track::kInLibrary;
        if (!scanning) { g_lib[id].rgTrackGain=-600; g_lib[id].flags|=Track::kRgScanned; }
    }
    std::vector<Playlist> playlists{{L"Library",{},PlaylistKind::Library}};
    ok=ok && SaveLibrary(app::g.libraryPath,playlists);
    app::g.inst=GetModuleHandleW(nullptr);
    INITCOMMONCONTROLSEX icc{sizeof(icc),ICC_WIN95_CLASSES|ICC_BAR_CLASSES|ICC_LISTVIEW_CLASSES|ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    app::g.themeMode=ThemeMode::Dark;
    app::ApplyTheme();
    auto cpu=[] {
        FILETIME create,exit,kernel,user;
        GetProcessTimes(GetCurrentProcess(),&create,&exit,&kernel,&user);
        return ((uint64_t(kernel.dwHighDateTime)<<32)|kernel.dwLowDateTime)+
               ((uint64_t(user.dwHighDateTime)<<32)|user.dwLowDateTime);
    };
    const uint64_t startupCpu=cpu();
    const ULONGLONG startup=GetTickCount64();
    WNDCLASSW wc{};
    wc.hInstance=app::g.inst; wc.lpszClassName=L"CordinalRuntimeBenchmark"; wc.lpfnWndProc=app::WndProc;
    RegisterClassW(&wc);
    HWND window=CreateWindowW(wc.lpszClassName,L"",WS_OVERLAPPEDWINDOW,-20000,-20000,1120,720,
                              nullptr,app::BuildMenu(),wc.hInstance,nullptr);
    ok &= window!=nullptr;
    ShowWindow(window,SW_SHOWNOACTIVATE); UpdateWindow(window);
    if (modes) {
        const bool playing=args.size()>4 && args[4]==L"playing";
        if (playing) {
            app::g.engine.SetVolume(0);
            app::PlayTrack(0,0,false);
        }
        const wchar_t* stages[]={L"normal",L"compact",L"mini",L"compact-return",L"normal-return",L"big-plus",L"big"};
        for (unsigned phase=0;phase<std::size(stages);++phase) {
            if (phase==1) app::SetViewSize(app::ViewSize::Compact);
            else if (phase==2 || phase==3) app::ToggleMiniPlayer();
            else if (phase==4) app::SetViewSize(app::ViewSize::Normal);
            else if (phase==5) app::SetViewSize(app::ViewSize::Spotlight);
            else if (phase==6) app::SetViewSize(app::ViewSize::Big);
            ULONGLONG until=GetTickCount64()+2000;
            do {
                MSG message;
                while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
                MsgWaitForMultipleObjects(0,nullptr,FALSE,10,QS_ALLINPUT);
            } while (GetTickCount64()<until);
            PROCESS_MEMORY_COUNTERS_EX2 memory{};
            ok=GetProcessMemoryInfo(GetCurrentProcess(),(PROCESS_MEMORY_COUNTERS*)&memory,sizeof(memory)) && ok;
            const bool reduced=phase>=1 && phase<=3;
            ok=ok && app::ReducedMemoryMode()==reduced && app::g.view.size()==256 && g_lib.Count()==256;
            if (playing) ok=ok && app::g.playing!=app::kNone && app::g.engine.Rate()!=0 && !app::g.engine.Paused();
            fwprintf(log,L"mode=%ls private_bytes=%zu working_set=%zu private_working_set=%zu reduced=%d playing=%d\n",
                     stages[phase],memory.PrivateUsage,memory.WorkingSetSize,memory.PrivateWorkingSetSize,
                     (int)app::ReducedMemoryMode(),(int)playing);
            {
                // Private bytes include the graphics device and its surfaces. Reporting what the
                // process heaps hold says whether a footprint change belongs to this code at all.
                HANDLE heaps[16]{};
                const DWORD count=GetProcessHeaps(16,heaps);
                size_t heapBytes=0;
                for (DWORD h=0;h<count;++h) {
                    if (!HeapLock(heaps[h])) continue;
                    PROCESS_HEAP_ENTRY entry{};
                    while (HeapWalk(heaps[h],&entry))
                        if (entry.wFlags&PROCESS_HEAP_ENTRY_BUSY) heapBytes+=entry.cbData;
                    HeapUnlock(heaps[h]);
                }
                fwprintf(log,L"heap_committed=%zu\n",heapBytes);
            }

            fflush(log);
        }
        // Opening mini directly from a large size must also reduce memory. Changing the hidden
        // main window's size keeps that policy until the mini player is actually closed.
        for (app::ViewSize size : {app::ViewSize::Normal,app::ViewSize::Spotlight,app::ViewSize::Big}) {
            app::SetViewSize(size);
            app::ToggleMiniPlayer();
            ok=ok && app::MiniPlayerWindow() && app::ReducedMemoryMode();
            app::SetViewSize(app::ViewSize::Normal);
            ok=ok && app::ReducedMemoryMode();
            app::ToggleMiniPlayer();
            ok=ok && !app::MiniPlayerWindow() && !app::ReducedMemoryMode();
        }
        app::SetViewSize(app::ViewSize::Compact);
        app::ToggleMiniPlayer();
        app::SaveStartupDefaults(app::CaptureStartupPreferences(), app::kDefaultWindowSize);
        ok=ok && app::g.defaultMiniPlayer && app::g.defaultSize==app::ViewSize::Compact;
        app::ToggleMiniPlayer();
        app::SaveViewSize(); app::SavePreferences(); app::LoadPreferences();
        app::RestoreWindowPlacement(SW_SHOWNOACTIVATE);
        ok=ok && app::MiniPlayerWindow() && app::ReducedMemoryMode() && app::g.size==app::ViewSize::Compact;
        ok=ok && !app::PreferenceSuggestionsDue(GetTickCount64()+300000);
        app::ToggleMiniPlayer();
        app::SaveStartupDefaults(app::CaptureStartupPreferences(), 0);
        app::SavePreferences(); app::LoadPreferences();
        ok=ok && app::g.defaultMiniPlayer;
        app::SetViewSize(app::ViewSize::Normal);
        app::SaveStartupDefaults(app::CaptureStartupPreferences(), app::kDefaultWindowSize);
        app::LoadPreferences(); app::RestoreWindowPlacement(SW_HIDE);
        ok=ok && !app::g.defaultMiniPlayer && !app::MiniPlayerWindow() && app::g.size==app::ViewSize::Normal;
        fwprintf(log,L"startup_size_preferences=%ls\n",ok?L"PASS":L"FAIL");
        app::SetViewSize(app::ViewSize::Compact);
        app::g.themeMode=app::g.defaultTheme==ThemeMode::Dark?ThemeMode::Light:ThemeMode::Dark;
        app::g.libMode=app::LibraryMode::Tracks;
        app::g.browserVisible=!app::g.defaultBrowserVisible;
        app::g.layout.player=app::Dock::Left;
        app::ApplyTheme();
        const auto offered=app::CaptureStartupPreferences();
        HWND focus=GetFocus();
        app::ShowStartupDefaults(true);
        HWND review=app::StartupDefaultsWindow();
        ok=ok && review && GetFocus()==focus && IsWindowEnabled(window) && !IsWindowEnabled(GetDlgItem(review,IDOK));
        RECT client{}; GetClientRect(review,&client);
        ok=RenderReview(review,DirectoryOf(args[2])+L"\\defaults-review.bmp") && ok;
        for (int i=0;i<5;++i) {
            HWND check=GetDlgItem(review,100+i);
            RECT bounds{}; GetWindowRect(check,&bounds); MapWindowPoints(nullptr,review,(POINT*)&bounds,2);
            ok=ok && check && Button_GetCheck(check)==BST_UNCHECKED && bounds.left>=0 && bounds.top>=0 &&
                     bounds.right<=client.right && bounds.bottom<=client.bottom;
        }
        for (int i : {0,1,3}) Button_SetCheck(GetDlgItem(review,100+i),BST_CHECKED);
        SendMessageW(review,WM_COMMAND,MAKEWPARAM(100,BN_CLICKED),(LPARAM)GetDlgItem(review,100));
        ok=ok && IsWindowEnabled(GetDlgItem(review,IDOK));
        app::SetViewSize(app::ViewSize::Normal);
        app::g.themeMode=ThemeMode::Deltarune;
        app::ApplyTheme();
        app::MaybeOfferPreferenceSuggestions();
        SendMessageW(review,WM_COMMAND,IDOK,0);
        ok=ok && app::StartupDefaultsWindow()==review && GetDlgItem(review,105) && GetDlgItem(review,106) &&
                 Button_GetCheck(GetDlgItem(review,106))==BST_CHECKED &&
                 Button_GetCheck(GetDlgItem(review,105))==BST_UNCHECKED && app::g.defaultSize==offered.size &&
                 app::g.defaultTheme==offered.theme && app::g.defaultBrowserVisible==offered.browser &&
                 app::g.defaultLibraryMode==app::LibraryMode::Albums && app::g.defaultLayout.player==app::Dock::Top &&
                 app::g.size==app::ViewSize::Normal && app::g.themeMode==ThemeMode::Deltarune;
        ok=RenderReview(review,DirectoryOf(args[2])+L"\\defaults-saving-policy.bmp") && ok;
        SendMessageW(review,WM_COMMAND,IDCANCEL,0);
        ok=ok && !app::StartupDefaultsWindow() && app::g.preferenceSaving==app::PreferenceSaving::Ask;
        app::ResetPreferenceSuggestions();
        app::g.layout.player=app::Dock::Right;
        app::ObservePreferenceChanges(GetTickCount64());
        ok=ok && !app::PreferenceSuggestionsDue(GetTickCount64()+300000);
        const auto live=app::CaptureStartupPreferences();
        app::ShowStartupDefaults();
        SendMessageW(app::StartupDefaultsWindow(),WM_COMMAND,IDCANCEL,0);
        ok=ok && !app::StartupDefaultsWindow() && app::CaptureStartupPreferences()==live &&
                 app::g.defaultSize==offered.size && app::g.defaultTheme==offered.theme;
        fwprintf(log,L"grouped_startup_review=%ls\n",ok?L"PASS":L"FAIL");
        app::ShowStartupDefaults();
        review=app::StartupDefaultsWindow();
        Button_SetCheck(GetDlgItem(review,102),BST_CHECKED);
        SendMessageW(review,WM_COMMAND,IDOK,0);
        ok=ok && app::StartupDefaultsWindow()==review && GetDlgItem(review,105);
        Button_SetCheck(GetDlgItem(review,105),BST_CHECKED);
        Button_SetCheck(GetDlgItem(review,106),BST_UNCHECKED);
        SendMessageW(review,WM_COMMAND,IDOK,0);
        ok=ok && !app::StartupDefaultsWindow() && app::g.preferenceSaving==app::PreferenceSaving::Automatic &&
                 app::g.defaultLayout.player==app::Dock::Top && app::g.defaultTheme==offered.theme;
        app::g.browserVisible=!offered.browser;
        app::FlushAutomaticPreferences();
        ok=ok && app::g.defaultBrowserVisible==!offered.browser && app::g.defaultLayout.player==app::Dock::Top &&
                 app::g.defaultTheme==offered.theme;
        app::OnCommand(app::IDM_PREFERENCE_ASK,0,nullptr);
        app::ShowStartupDefaults();
        review=app::StartupDefaultsWindow();
        Button_SetCheck(GetDlgItem(review,100),BST_CHECKED);
        SendMessageW(review,WM_COMMAND,IDOK,0);
        ok=ok && !app::StartupDefaultsWindow() && app::g.preferenceSaving==app::PreferenceSaving::Ask &&
                 app::g.preferenceSavingChosen;
        app::OnCommand(app::IDM_PREFERENCE_MANUAL,0,nullptr);
        app::ShowStartupDefaults(true);
        ok=ok && !app::StartupDefaultsWindow();
        app::ShowStartupDefaults();
        review=app::StartupDefaultsWindow();
        Button_SetCheck(GetDlgItem(review,104),BST_CHECKED);
        SendMessageW(review,WM_COMMAND,IDOK,0);
        ok=ok && !app::StartupDefaultsWindow() && app::g.defaultLayout.player==app::Dock::Right;
        // Exercise the other first-review answer with an undecided policy.
        app::OnCommand(app::IDM_PREFERENCE_ASK,0,nullptr);
        app::g.preferenceSavingChosen=false;
        app::ShowStartupDefaults();
        review=app::StartupDefaultsWindow();
        Button_SetCheck(GetDlgItem(review,101),BST_CHECKED);
        SendMessageW(review,WM_COMMAND,IDOK,0);
        ok=ok && GetDlgItem(review,106) && Button_GetCheck(GetDlgItem(review,106))==BST_CHECKED;
        SendMessageW(review,WM_COMMAND,IDOK,0);
        ok=ok && !app::StartupDefaultsWindow() && app::g.preferenceSaving==app::PreferenceSaving::Ask &&
                 app::g.preferenceSavingChosen && app::IniInt(L"Suggestions",L"SavingModeChosen",0)==1;
        fwprintf(log,L"preference_saving_policy=%ls\n",ok?L"PASS":L"FAIL");
        DestroyWindow(window);
        fwprintf(log,L"result=%ls\n",ok?L"PASS":L"FAIL");
        fclose(log); CoUninitialize();
        SetThreadDesktop(original); CloseDesktop(desktop);
        return ok?0:1;
    }
    if (args.size()>3 && args[3]==L"web") {
        ok = MeasureWebRuntime(log) && ok;
        DestroyWindow(window);
        fwprintf(log,L"result=%ls\n",ok?L"PASS":L"FAIL");
        fclose(log); CoUninitialize(); return ok?0:1;
    }
    if (switching) {
        app::g.engine.SetVolume(0);
        auto pump = [](DWORD ms) {
            const ULONGLONG until = GetTickCount64() + ms;
            do {
                MSG message;
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&message); DispatchMessageW(&message);
                }
                MsgWaitForMultipleObjects(0, nullptr, FALSE, 5, QS_ALLINPUT);
            } while (GetTickCount64() < until);
        };
        auto snapshot = [&](unsigned count) {
            PROCESS_MEMORY_COUNTERS_EX memory{};
            ok = GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&memory, sizeof(memory)) && ok;
            fwprintf(log, L"switches=%u cpu_ms=%.3f private_bytes=%zu working_set=%zu gdi=%lu handles=%lu\n",
                     count, (cpu() - startupCpu) / 10000.0, memory.PrivateUsage, memory.WorkingSetSize,
                     GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS), [] {
                         DWORD handles = 0; GetProcessHandleCount(GetCurrentProcess(), &handles); return handles;
                     }());
            fflush(log);
        };
        pump(500);
        // Keep waveform cache activity inside this benchmark's throwaway profile.
        StopPeaks();
        StartPeaks(window, app::WM_APP_PEAKS, profile + L"\\peaks");
        snapshot(0);
        for (unsigned i = 0; i < 80 && ok; ++i) {
            TrackId wanted = app::g.playlists[0].items[i];
            app::PlayTrack(0, (int)i, false);
            const ULONGLONG until = GetTickCount64() + 3000;
            while (app::g.playing != wanted && GetTickCount64() < until) pump(5);
            ok = app::g.playing == wanted;
            pump(100);
            if ((i + 1) % 20 == 0) snapshot(i + 1);
        }
        pump(2000);
        snapshot(80);
        DestroyWindow(window);
        fwprintf(log, L"result=%ls\n", ok ? L"PASS" : L"FAIL");
        fclose(log); CoUninitialize(); return ok ? 0 : 1;
    }
    SYSTEM_INFO info{}; GetSystemInfo(&info);
    // The playing scenario isolates steady playback plus the UI timer: every track is already
    // measured, so no background analysis competes with the decode thread.
    if (steadyPlayback) {
        app::g.engine.SetVolume(0);
        app::PlayTrack(0,0,false);
        // Let the device open and the first blocks render, then record that playback really runs:
        // a scenario that silently failed to start would report a meaningless idle cost.
        const ULONGLONG settle=GetTickCount64()+1000;
        do {
            MSG message;
            while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
            MsgWaitForMultipleObjects(0,nullptr,FALSE,10,QS_ALLINPUT);
        } while (GetTickCount64()<settle);
        const bool live=app::g.playing!=app::kNone && app::g.engine.Rate()!=0 && !app::g.engine.Paused();
        ok=ok && live;
        fwprintf(log,L"playback_live=%d rate=%u\n",(int)live,app::g.engine.Rate());
    }
    fwprintf(log,L"scenario=%ls processors=%u startup_ms=%llu startup_cpu_ms=%.3f\n",
             steadyPlayback?L"playing":scanning?L"scan":L"idle",
             info.dwNumberOfProcessors,GetTickCount64()-startup,(cpu()-startupCpu)/10000.0);
    ULONGLONG start=GetTickCount64(), previous=start;
    uint64_t lastCpu=cpu();
    double maximum=0,total=0; unsigned samples=0;
    while (GetTickCount64()-start<16000) {
        MSG message;
        while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        const ULONGLONG now=GetTickCount64();
        if (now-previous>=1000) {
            const uint64_t current=cpu();
            double percent=100.0*(current-lastCpu)/10000.0/(now-previous)/info.dwNumberOfProcessors;
            maximum=std::max(maximum,percent); total+=percent; ++samples;
            PROCESS_MEMORY_COUNTERS_EX memory{};
            GetProcessMemoryInfo(GetCurrentProcess(),(PROCESS_MEMORY_COUNTERS*)&memory,sizeof(memory));
            fwprintf(log,L"second=%llu cpu_percent=%.3f private_bytes=%zu working_set=%zu scanning=%d measured=%d pos=%.2f playing=%d\n",
                     (now-start)/1000,percent,memory.PrivateUsage,memory.WorkingSetSize,(int)app::g.rgRunning,app::g.rgDone.load(),
                     app::g.engine.Position(),(int)app::g.playing);
            fflush(log); previous=now; lastCpu=current;
        }
        MsgWaitForMultipleObjects(0,nullptr,FALSE,10,QS_ALLINPUT);
    }
    DestroyWindow(window);
    fwprintf(log,L"average_cpu_percent=%.3f maximum_cpu_percent=%.3f result=%ls\n",samples?total/samples:0,maximum,ok?L"PASS":L"FAIL");
    fclose(log); CoUninitialize();
    return ok?0:1;
}
