// Process startup, single-instance forwarding and the message loop.
// Owns wWinMain and defines the one process-wide `g` App instance; the window
// procedure itself is in app_window.cpp. Startup orders theme and common-control
// setup before any window so nothing ever paints with default colors.
#include "app.h"
#include "command_line.h"
#include "packed_image.h"

namespace app {

// The single App instance every module reaches shared state through.
App g;

// Single-instance: a second launch forwards its paths to the running window over
// WM_COPYDATA instead of opening a second one, then brings that window forward.
bool ForwardToRunningInstance(const std::vector<std::wstring>& args, bool addOnly) {
    HWND other = FindWindowW(kClassName, nullptr);
    if (!other) return false;
    if (!args.empty()) {
        // Send absolute paths: they are resolved against this process's working
        // directory, which may differ from the receiver's. Newline-separated
        // UTF-16; an empty join (all paths rejected) is still a valid wake-up.
        std::wstring joined;
        for (const auto& a : args) {
            // Size the buffer first: a deep library path is longer than MAX_PATH * 4, and falling
            // back to the raw argument would resolve it against the receiver's folder instead.
            DWORD needed = GetFullPathNameW(a.c_str(), 0, nullptr, nullptr);
            std::wstring full;
            if (needed) {
                full.resize(needed);
                DWORD got = GetFullPathNameW(a.c_str(), needed, full.data(), nullptr);
                if (got && got < needed) full.resize(got);
                else full.clear();
            }
            joined += (full.empty() ? a : full) + L"\n";
        }
        COPYDATASTRUCT cds{addOnly ? kCopyDataAddPaths : kCopyDataPaths, (DWORD)(joined.size() * sizeof(wchar_t)),
                           joined.data()};
        DWORD_PTR result = 0;
        SendMessageTimeoutW(other, WM_COPYDATA, 0, (LPARAM)&cds, SMTO_ABORTIFHUNG, 5000, &result);
    }
    if (IsIconic(other)) ShowWindow(other, SW_RESTORE);
    SetForegroundWindow(other);
    return true;
}

}  // namespace app

using namespace app;

int APIENTRY wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int showCmd) {
    RestorePackedImageProtection();  // no-op in native builds; see packed_image.h
    // Gather argv (skipping argv[0]) as the raw command line, before anything else
    // can consume it.
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args;
    for (int i = 1; i < argc; ++i) args.push_back(RepairTrailingQuote(argv[i]));
    LocalFree(argv);
    // Explorer's Add verb appends without playing; strip the switch before anything reads paths.
    const bool addOnly = TakeAddSwitch(args);
    // Safe mode starts without packs and helpers: --safe, or Shift held while cordinal starts.
    const bool safe = std::erase(args, std::wstring(L"--safe")) > 0 || GetAsyncKeyState(VK_SHIFT) < 0;
    // Diagnostics run in this same exe (or forward to cordinal-test.exe) and must
    // win over single-instance forwarding so they work while the player is open.
    if (auto testExit = RunTestMode(args)) return *testExit;
    if (auto commandExit = ForwardTransportCommand(args)) return *commandExit;
    bool openWeb = args.size() == 1 && args[0] == L"--web";
    if (openWeb) args.clear();
    if (ForwardToRunningInstance(args, addOnly)) return 0;

    HeapSetInformation(nullptr, HeapEnableTerminationOnCorruption, nullptr, 0);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    // Rich edit / list view / tab / status controls are used throughout, so register
    // their classes once up front.
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_WIN95_CLASSES | ICC_BAR_CLASSES | ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES |
                                              ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    g.inst = inst;
    InitDataPaths();
    InstallCrashReports();
    // Packs come first: their themes, fonts and functions are part of the first frame. Helpers come
    // before the menu bar, which lists them, and before the scanner, which counts their files as audio.
    LoadPacks(safe);
    LoadHelpers(safe);
    // Theme is read before the window exists; invalid values clamp to Light.
    g.themeMode = ReadSavedTheme();
    ApplyTheme();  // brushes and menu theme before any window exists

    // Main window class; the equalizer is a separate top-level class (app_window.cpp).
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
    RegisterAppClass(kEqClass, EqProc);
    RegisterAppClass(kHeadphonesClass, HeadphonesProc);

    // Default size is 1120x720 at 96 DPI, scaled to the system DPI so the first
    // frame is already right on high-DPI monitors.
    UINT sysDpi = GetDpiForSystem();
    HMENU menu = BuildMenu();
    HWND hwnd = CreateWindowExW(0, kClassName, kAppName, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                                CW_USEDEFAULT, MulDiv(1120, sysDpi, 96), MulDiv(720, sysDpi, 96), nullptr, menu,
                                inst, nullptr);
    if (!hwnd) return 1;
    g.accel = BuildAccelerators();
    RestoreWindowPlacement(showCmd);
    UpdateWindow(hwnd);
    if (!args.empty()) {
        g.startupPaths = true;
        AddPaths(args, !addOnly);  // paths from our own command line
    }
    // The first frame is on screen; finish tray/media/headset startup from the message loop.
    PostMessageW(hwnd, WM_APP_STARTUP, 0, 0);
    if (openWeb) PostMessageW(hwnd, WM_APP_WEB, 1, 0);

    // Message loop. Modeless dialogs get the message first so their editing keys and
    // Tab handling keep working; only then are main-window keys and accelerators
    // translated. Order here is significant.
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (PaletteDialogMessage(&msg)) continue;
        if (EditionDialogMessage(&msg)) continue;
        if (PreferencesDialogMessage(&msg)) continue;
        if (EqualizerDialogMessage(&msg)) continue;
        if (WebDialogMessage(&msg)) continue;
        if (TagEditorDialogMessage(&msg)) continue;
        if (AutoEditorDialogMessage(&msg)) continue;
        if (FormatDialogMessage(&msg)) continue;
        if (StartupDefaultsDialogMessage(&msg)) continue;
        if (ListenBrainzDialogMessage(&msg)) continue;
        if (OrganizeDialogMessage(&msg)) continue;
        if (CopyDialogMessage(&msg)) continue;
        if (BackupDialogMessage(&msg)) continue;
        if (RegionMouse(msg)) continue;  // the seek bar's section menu (app_region.cpp)
        if (TranslateMainKey(&msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    // Release process-owned GDI objects after the loop exits (WM_QUIT).
    DestroyAcceleratorTable(g.accel);
    for (HFONT* font : {&g.font, &g.fontBold, &g.fontTitle, &g.fontSymbol, &g.fontArt,
                        &g.fontIcons, &g.fontIconsBig, &g.fontIconsHuge}) {
        if (*font) DeleteObject(*font);
        *font = nullptr;
    }
    if (g.fieldBrush) DeleteObject(g.fieldBrush);
    if (g.surfaceBrush) DeleteObject(g.surfaceBrush);
    if (wc.hIconSm) DestroyIcon(wc.hIconSm);
    CoUninitialize();
    FinishProfileRestore();
    return (int)msg.wParam;
}
