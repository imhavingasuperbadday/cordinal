// The memory budget: the real player, started on a private desktop with a throwaway profile, must
// stay inside kBudgets in each everyday state, and its native image inside kProgramBudget. A
// regression fails at the change that caused it, with the owner that grew. See docs/memory.md.
#include "command_line.h"
#include "memory_report.h"
#include "selftest_common.h"

#include <thread>

#include <tlhelp32.h>

namespace {

constexpr double kMiB = 1048576.0;

// Budgets for the native player, in MiB of private working set (Task Manager's "Memory"). Each sits
// about 10% above what the scenario measured when it was set (November 9, 2026), so ordinary noise
// passes and a real regression does not. Budgets only move down: raising one needs the
// maintainer's agreement and a reason in docs/memory.md. After a saving, lower the budget to keep
// it; the log says when a scenario is far enough below its budget for that.
struct Budget {
    const wchar_t* scenario;
    const wchar_t* what;
    double memory;
};
constexpr Budget kBudgets[] = {
    {L"empty", L"first start: no library, album view", 6.0},
    {L"tracks", L"2,000-track library, track list", 5.5},
    {L"albums", L"2,000-track library, album view with covers", 10.5},
    {L"playing", L"2,000-track library, album view, playing", 12.75},
};

// A packed build expands the whole native image into private memory, so every byte of program
// adds a byte to every packed scenario above: a packed player's figure is the native one plus
// about this much. The native image was 5.21 MiB when the budget was set.
constexpr double kProgramBudget = 5.5;

// Where the budgets are heading: what the packed release player should show in Task Manager while
// playing. docs/memory.md has the plan; the log reports the distance on every run.
constexpr double kTarget = 10.0;

constexpr unsigned kTracks = 2000, kAlbumSize = 10;

std::wstring ExePath() {
    std::wstring path(32768, L'\0');
    path.resize(GetModuleFileNameW(nullptr, path.data(), (DWORD)path.size()));
    return path;
}

// A 600 px cover, as large as a typical embedded or folder image after the resizes most taggers do.
std::string CoverBitmap() {
    BITMAPFILEHEADER file{};
    BITMAPINFOHEADER info{};
    constexpr int side = 600;
    file.bfType = 0x4D42;
    file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + side * side * 4;
    info.biSize = sizeof(info);
    info.biWidth = info.biHeight = side;
    info.biPlanes = 1;
    info.biBitCount = 32;
    std::string image(reinterpret_cast<const char*>(&file), sizeof(file));
    image.append(reinterpret_cast<const char*>(&info), sizeof(info));
    for (int i = 0; i < side * side; ++i) image += {char(i), char(i >> 4), char(i >> 8), '\xff'};
    return image;
}

bool WriteUtf16Ini(const std::wstring& path, const std::wstring& text) {
    std::string bytes("\xFF\xFE", 2);
    bytes.append(reinterpret_cast<const char*>(text.data()), text.size() * sizeof(wchar_t));
    return WriteTestBytes(path, bytes);
}

struct Player {
    PROCESS_INFORMATION process{};
    HWND window = nullptr;
};

HWND MainWindowOf(DWORD pid) {
    struct Search { DWORD pid; HWND found; } search{pid, nullptr};
    EnumWindows([](HWND window, LPARAM data) {
        auto* s = reinterpret_cast<Search*>(data);
        DWORD owner = 0;
        wchar_t name[64];
        GetWindowThreadProcessId(window, &owner);
        if (owner == s->pid && GetClassNameW(window, name, 64) && !wcscmp(name, app::kClassName)) {
            s->found = window;
            return FALSE;
        }
        return TRUE;
    }, (LPARAM)&search);
    return search.found;
}

std::wstring RequestReport(HWND window, const std::wstring& path) {
    DeleteFileW(path.c_str());
    COPYDATASTRUCT data{kCopyDataMemoryReport, (DWORD)(path.size() * sizeof(wchar_t)), (void*)path.data()};
    DWORD_PTR result = 0;
    if (!SendMessageTimeoutW(window, WM_COPYDATA, 0, (LPARAM)&data, SMTO_ABORTIFHUNG, 10000, &result) || !result)
        return {};
    std::string text = ReadTestBytes(path);
    DeleteFileW(path.c_str());
    return FromUtf8(text);
}

double Field(const std::wstring& report, const wchar_t* name) {
    const size_t at = report.find(name);
    return at == std::wstring::npos ? -1 : _wtof(report.c_str() + at + wcslen(name));
}

// Builds a profile in another process: the library globals of this one belong to the other suites.
bool PrepareProfile(const std::wstring& profile, const std::wstring& flac) {
    const std::wstring command = QuoteArg(ExePath()) + L" --memory-profile " + QuoteArg(profile) + L" " + QuoteArg(flac);
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    std::wstring line = command;
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
        return false;
    if (WaitForSingleObject(process.hProcess, 120000) != WAIT_OBJECT_0) TerminateProcess(process.hProcess, 1);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return code == 0;
}

// Starts the player with its own profile and caches on the given desktop; waits for its window.
bool Launch(Player& player, const std::wstring& exe, const std::wstring& profile, const std::wstring& args,
            const std::wstring& desktop) {
    std::wstring environment;
    if (wchar_t* block = GetEnvironmentStringsW()) {
        for (const wchar_t* at = block; *at; at += wcslen(at) + 1)
            if (_wcsnicmp(at, L"CORDINAL_DATA=", 14) && _wcsnicmp(at, L"__COMPAT_LAYER=", 15)) environment.append(at).push_back(L'\0');
        FreeEnvironmentStringsW(block);
    }
    environment.append(L"CORDINAL_DATA=" + profile).push_back(L'\0');
    environment.append(L"__COMPAT_LAYER=RunAsInvoker").push_back(L'\0');
    environment.push_back(L'\0');
    std::wstring line = QuoteArg(exe) + (args.empty() ? L"" : L" " + args);
    std::wstring desk = desktop;
    STARTUPINFOW startup{sizeof(startup)};
    startup.lpDesktop = desk.data();
    if (!CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT,
                        environment.data(), nullptr, &startup, &player.process)) return false;
    for (ULONGLONG until = GetTickCount64() + 20000; !player.window && GetTickCount64() < until; Sleep(50))
        player.window = MainWindowOf(player.process.dwProcessId);
    return player.window != nullptr;
}

void Close(Player& player) {
    if (player.window) PostMessageW(player.window, WM_CLOSE, 0, 0);
    if (player.process.hProcess && WaitForSingleObject(player.process.hProcess, 15000) != WAIT_OBJECT_0)
        TerminateProcess(player.process.hProcess, 1);
    if (player.process.hProcess) CloseHandle(player.process.hProcess);
    if (player.process.hThread) CloseHandle(player.process.hThread);
    player = {};
}

struct Measured {
    std::wstring report;
    double memory = -1;
    std::vector<std::wstring> modules;  // every DLL the player had mapped, lower case
};

// System DLLs only some features need. tools/build.ps1 delay-loads them, so a session that never
// uses those features never maps them; each one mapped costs private pages and heap.
constexpr const wchar_t* kOnDemandDlls[] = {L"winhttp.dll", L"ws2_32.dll", L"iphlpapi.dll", L"bcrypt.dll",
                                            L"hid.dll", L"setupapi.dll", L"xmllite.dll"};

std::vector<std::wstring> ModulesOf(DWORD pid) {
    std::vector<std::wstring> names;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snapshot == INVALID_HANDLE_VALUE) return names;
    MODULEENTRY32W entry{sizeof(entry)};
    for (BOOL more = Module32FirstW(snapshot, &entry); more; more = Module32NextW(snapshot, &entry)) {
        std::wstring name = entry.szModule;
        for (wchar_t& c : name) c = towlower(c);
        names.push_back(std::move(name));
    }
    CloseHandle(snapshot);
    return names;
}

// One scenario: a fresh copy of the prepared profile, the player started, settled, then asked.
Measured Measure(const std::wstring& exe, const std::wstring& base, const std::wstring& scratch, const wchar_t* scenario,
                 const std::wstring& desktop) {
    Measured result;
    const std::wstring profile = scratch + L"\\" + scenario;
    std::error_code error;
    std::filesystem::remove_all(profile, error);
    CreateDirectoryW(profile.c_str(), nullptr);
    const bool empty = !wcscmp(scenario, L"empty");
    if (!empty && !CopyFileW((base + L"\\library.cdb").c_str(), (profile + L"\\library.cdb").c_str(), FALSE)) return result;
    // Muted, with no first-run reviews; the track list or the album view as the scenario asks.
    std::wstring ini = L"[Player]\r\nVolume=0\r\n[Library]\r\nView=";
    ini += !wcscmp(scenario, L"tracks") ? L"0" : L"2";
    ini += L"\r\n[Suggestions]\r\nSavingModeChosen=1\r\n";
    if (!WriteUtf16Ini(profile + L"\\settings.ini", ini)) return result;
    Player player;
    const std::wstring args = !wcscmp(scenario, L"playing") ? QuoteArg(base + L"\\media\\album-0\\track-0.flac") : L"";
    if (!Launch(player, exe, profile, args, desktop)) {
        Close(player);
        return result;
    }
    // Startup work (first paint, covers for the visible albums, the device on first play) settles
    // within a few seconds; the second report is the steady state the budget is about.
    Sleep(6000);
    const std::wstring file = scratch + L"\\" + scenario + L"-report.txt";
    RequestReport(player.window, file);
    Sleep(2000);
    result.report = RequestReport(player.window, file);
    result.memory = Field(result.report, L"memory ") * kMiB;
    result.modules = ModulesOf(player.process.dwProcessId);
    Close(player);
    return result;
}

size_t NativeImageSize(const std::wstring& exe) {
    const std::string bytes = ReadTestBytes(exe);
    if (bytes.size() < sizeof(IMAGE_DOS_HEADER)) return 0;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > bytes.size()) return 0;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(bytes.data() + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE ? nt->OptionalHeader.SizeOfImage : 0;
}

// Runs every scenario against the player; the callback sees each result as it arrives. The player
// runs on a private desktop so nothing appears on screen, and only a thread on that desktop can
// find its window and send it a message: this thread may own windows already, so a new one does.
bool RunScenarios(const std::wstring& exe, const std::wstring& flac, const std::wstring& scratch,
                  const std::function<void(const Budget&, const Measured&)>& each) {
    SHCreateDirectoryExW(nullptr, scratch.c_str(), nullptr);
    const std::wstring base = scratch + L"\\library";
    if (!PrepareProfile(base, flac)) return false;
    const std::wstring name = L"CordinalMemoryBudget-" + std::to_wstring(GetCurrentProcessId());
    HDESK desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desktop) return false;
    std::vector<Measured> results(std::size(kBudgets));
    bool entered = false;
    std::thread worker([&] {
        if (!(entered = SetThreadDesktop(desktop))) return;
        for (size_t i = 0; i < std::size(kBudgets); ++i)
            results[i] = Measure(exe, base, scratch, kBudgets[i].scenario, name);
    });
    worker.join();
    CloseDesktop(desktop);
    for (size_t i = 0; entered && i < std::size(kBudgets); ++i) each(kBudgets[i], results[i]);
    return entered;
}

}  // namespace

// --memory-profile <dir> <flac>: the scenarios' library, built in its own process. Albums of ten
// distinct tracks, each a hard link to one fixture, with a folder cover per album; every track is
// already measured, so the steady state is not an automatic ReplayGain scan.
int RunMemoryProfile(const std::vector<std::wstring>& args) {
    if (args.size() < 3) return 2;
    const std::wstring profile = args[1], media = profile + L"\\media";
    std::error_code error;
    std::filesystem::remove_all(profile, error);
    SHCreateDirectoryExW(nullptr, media.c_str(), nullptr);
    TagData tags;
    if (!ReadTags(args[2], tags)) return 3;
    // One cover file, linked into every album folder: the views decode each folder's copy on its own.
    const std::wstring cover = media + L"\\cover.bmp";
    if (!WriteTestBytes(cover, CoverBitmap())) return 4;
    for (unsigned i = 0; i < kTracks; ++i) {
        const unsigned album = i / kAlbumSize;
        const std::wstring folder = media + L"\\album-" + std::to_wstring(album);
        if (i % kAlbumSize == 0) {
            CreateDirectoryW(folder.c_str(), nullptr);
            const std::wstring link = folder + L"\\cover.png";
            if (!CreateHardLinkW(link.c_str(), cover.c_str(), nullptr) && !CopyFileW(cover.c_str(), link.c_str(), TRUE))
                return 4;
        }
        const std::wstring path = folder + L"\\track-" + std::to_wstring(i % kAlbumSize) + L".flac";
        if (!CreateHardLinkW(path.c_str(), args[2].c_str(), nullptr) && !CopyFileW(args[2].c_str(), path.c_str(), TRUE))
            return 5;
        const TrackId id = g_lib.Add(path);
        tags.title = "Song " + std::to_string(i);
        tags.album = "Album " + std::to_string(album);
        tags.artist = "Artist " + std::to_string(album % 37);
        tags.genre = album % 3 ? "Rock" : "Jazz";
        g_lib.ApplyTags(id, tags);
        g_lib[id].flags |= Track::kInLibrary | Track::kRgScanned;
        g_lib[id].rgTrackGain = -600;
    }
    std::vector<Playlist> playlists{{L"Library", {}, PlaylistKind::Library}};
    return SaveLibrary(profile + L"\\library.cdb", playlists) ? 0 : 6;
}

// --memory-report [player.exe] [fixtures]: with no arguments, the running player's report. With a
// player, the budget scenarios against it (a packed build, an older build) without failing on them.
int RunMemoryReport(const std::vector<std::wstring>& args) {
    if (args.size() < 2) {
        HWND window = FindWindowW(app::kClassName, nullptr);
        if (!window) {
            fwprintf(stderr, L"cordinal is not running.\n");
            return 1;
        }
        wchar_t temp[MAX_PATH];
        GetTempPathW(MAX_PATH, temp);
        const std::wstring report = RequestReport(window, std::wstring(temp) + L"cordinal-memory-" +
                                                              std::to_wstring(GetCurrentProcessId()) + L".txt");
        fwprintf(stdout, L"%ls", report.empty() ? L"The player did not answer.\n" : report.c_str());
        return report.empty();
    }
    wchar_t full[32768];
    GetFullPathNameW(args[1].c_str(), 32768, full, nullptr);
    const std::wstring exe = full;
    GetFullPathNameW((args.size() > 2 ? args[2] : L"build\\fixtures").c_str(), 32768, full, nullptr);
    const std::wstring flac = std::wstring(full) + L"\\media\\sine.flac";
    const std::wstring scratch = DirectoryOf(ExePath()) + L"\\memory-report-" + std::to_wstring(GetCurrentProcessId());
    fwprintf(stdout, L"%ls\nnative image %.2f MB\n\n", exe.c_str(), NativeImageSize(exe) / kMiB);
    const bool ran = RunScenarios(exe, flac, scratch, [](const Budget& budget, const Measured& measured) {
        fwprintf(stdout, L"[%ls] %ls\n%ls\n", budget.scenario, budget.what,
                 measured.report.empty() ? L"no report\n" : measured.report.c_str());
        fflush(stdout);
    });
    std::error_code error;
    std::filesystem::remove_all(scratch, error);
    return ran ? 0 : 1;
}

void TestMemoryBudget(const std::wstring& root, const std::wstring& scratch) {
    const std::wstring exe = DirectoryOf(ExePath()) + L"\\cordinal.exe";
    if (!Ready(GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES, L"the player is built beside the test companion"))
        return;
    const size_t image = NativeImageSize(exe);
    // Only a native player measures what the code itself holds; a packed one adds its whole image.
    if (!Ready(image > 0 && image < 2 * ReadTestBytes(exe).size(), L"the player beside the companion is a native build"))
        return;
    const double imageMb = image / kMiB;
    Check(imageMb <= kProgramBudget, L"native image %.2f MB is within its %.2f MB budget: a packed build holds all of "
          L"it in private memory, so trim code or raise kProgramBudget with a reason in docs/memory.md", imageMb, kProgramBudget);
    const bool ran = RunScenarios(exe, root + L"\\media\\sine.flac", scratch + L"\\memory-budget",
                                  [imageMb](const Budget& budget, const Measured& measured) {
        fwprintf(g_log, L"memory-budget %ls (%ls):\n%ls", budget.scenario, budget.what, measured.report.c_str());
        const double mb = measured.memory / kMiB;
        if (!Ready(measured.memory > 0, L"the player answered a memory report")) return;
        Check(mb <= budget.memory, L"%ls: %.2f MB of private working set is within its %.2f MB budget; the report "
              L"above names the owner that grew", budget.scenario, mb, budget.memory);
        // Nothing in these two states looks anything up online, serves the web player or talks to a device.
        const bool quiet = !wcscmp(budget.scenario, L"empty") || !wcscmp(budget.scenario, L"tracks");
        if (quiet && Ready(!measured.modules.empty(), L"the player's module list")) {
            for (const wchar_t* dll : kOnDemandDlls)
                Check(std::find(measured.modules.begin(), measured.modules.end(), dll) == measured.modules.end(),
                      L"%ls: %ls is not mapped until a feature uses it (/DELAYLOAD in tools/build.ps1)",
                      budget.scenario, dll);
        }
        if (mb < budget.memory * 0.85)
            fwprintf(g_log, L"memory-budget %ls: %.2f MB is well under %.2f MB; lower the budget to keep the saving\n",
                     budget.scenario, mb, budget.memory);
        if (!wcscmp(budget.scenario, L"playing"))
            fwprintf(g_log, L"memory-budget target: a packed player would show about %.1f MB while playing (native "
                     L"%.2f + image %.2f); the target is %.1f MB\n", mb + imageMb, mb, imageMb, kTarget);
    });
    Ready(ran, L"memory-budget profile and private desktop");
}
