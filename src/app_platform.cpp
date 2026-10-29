// Windows and online integration: the system media controls (media keys, the volume flyout,
// lock screen, headset buttons), transport commands ("cordinal --next"), Explorer's folder
// verbs ("Play with cordinal"), the now-playing file for Linux media controls, ListenBrainz
// scrobbling and its window, crash reports, Steam soundtrack folders and the game-music loop
// setting.
#include "app.h"
#include "crashdump.h"
#include "listenbrainz.h"
#include "peaks.h"
#include "smtc.h"
#include "steam.h"

namespace app {

namespace {

bool g_smtc = false;       // the system media controls work; otherwise media keys are hotkeys
TrackId g_smtcTrack = kNone;
smtc::State g_smtcState = smtc::State::Closed;
ULONGLONG g_smtcTimeline = 0;

// On Linux (Wine) the launcher sets CORDINAL_NOWPLAYING: what is playing is written there for its
// MPRIS bridge (media keys, playerctl, the bar's media widget), with the cover as <file>.cover.
std::wstring g_nowPlayingFile;
std::string g_nowPlaying;  // the last text written, without the position
TrackId g_nowPlayingTrack = kNone;
double g_nowPlayingPosition = 0;
ULONGLONG g_nowPlayingAt = 0;  // when that position was written

const wchar_t* const kTransportCommands[] = {L"--play-pause", L"--play", L"--pause", L"--stop", L"--next", L"--previous", L"--web"};

// A per-machine cache folder under %LOCALAPPDATA%\cordinal, falling back to the data directory
// when the known folder is unavailable. CoTaskMemFree is required even when the call fails.
std::wstring LocalCacheDir(const wchar_t* name) {
    PWSTR local = nullptr;
    std::wstring dir = g.dataDir + L"\\" + name;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) dir = std::wstring(local) + L"\\cordinal\\" + name;
    CoTaskMemFree(local);
    return dir;
}

// A string-pool handle as UTF-8, which is what the network submission needs.
std::string Utf8(StringPool::Handle h) { return std::string(g_lib.View(h)); }

// The cover as encoded bytes, for the media flyout: embedded art, else an image next to the file.
std::string CoverBytes(TrackId id) {
    std::wstring path = g_lib.Path(id), file;
    int index = 0;
    if (SplitSubtrack(path, file, index)) path = file;
    std::string image;
    if (ReadEmbeddedArt(path, image) && !image.empty()) return image;
    std::wstring dir = DirectoryOf(path);
    for (const wchar_t* name : {L"cover.jpg", L"folder.jpg", L"front.jpg", L"cover.png", L"folder.png", L"front.png"}) {
        HANDLE f = CreateFileW((dir + L"\\" + name).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) continue;
        LARGE_INTEGER size{};
        if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < 8 * 1024 * 1024) {
            image.resize((size_t)size.QuadPart);
            DWORD read = 0;
            if (!ReadFile(f, image.data(), (DWORD)image.size(), &read, nullptr) || read != image.size()) image.clear();
        }
        CloseHandle(f);
        if (!image.empty()) return image;
    }
    return {};
}

void WriteFileReplacing(const std::wstring& path, const std::string& bytes) {
    std::wstring temp = path + L".tmp";
    HANDLE f = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    bool ok = WriteFile(f, bytes.data(), (DWORD)bytes.size(), &written, nullptr) && written == bytes.size();
    CloseHandle(f);
    if (!ok || !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(temp.c_str());
}

std::string OneLine(std::string s) {
    std::replace(s.begin(), s.end(), '\n', ' ');
    std::replace(s.begin(), s.end(), '\r', ' ');
    return s;
}

// Rewritten when the track or play state changes, or the position strays from where playing
// would have put it (a seek); the bridge counts the position on from the file's time.
void WriteNowPlaying() {
    if (g_nowPlayingFile.empty()) return;
    bool playing = (g.playing != kNone || RadioPlaying()) && !g.engine.Paused();
    double position = g.playing == kNone && !RadioPlaying() ? 0 : g.engine.Position();
    std::string text = "status=Stopped\n";
    if (RadioPlaying()) {
        if (g_nowPlayingTrack != kNone - 1) DeleteFileW((g_nowPlayingFile + L".cover").c_str());
        g_nowPlayingTrack = kNone - 1;
        std::wstring song = RadioSongTitle();
        text = std::string("status=") + (playing ? "Playing" : "Paused") + "\nid=radio\ntitle=" +
            OneLine(ToUtf8(song.empty() ? RadioStationName() : song)) + "\nartist=" +
            OneLine(ToUtf8(song.empty() ? L"" : RadioStationName())) + "\nlength=0\n";
    } else if (g.playing != kNone) {
        const Track& t = g_lib[g.playing];
        if (g.playing != g_nowPlayingTrack) {
            std::string cover = CoverBytes(g.playing);
            if (cover.empty()) DeleteFileW((g_nowPlayingFile + L".cover").c_str());
            else WriteFileReplacing(g_nowPlayingFile + L".cover", cover);
            g_nowPlayingTrack = g.playing;
        }
        text = std::string("status=") + (playing ? "Playing" : "Paused") + "\nid=" + std::to_string(g.playing) +
               "\ntitle=" + OneLine(t.title ? Utf8(t.title) : ToUtf8(StemOf(g_lib.Str(t.file)))) +
               "\nartist=" + OneLine(Utf8(t.artist ? t.artist : t.albumArtist)) + "\nalbum=" + OneLine(Utf8(t.album)) +
               "\nalbumartist=" + OneLine(Utf8(t.albumArtist)) + "\nlength=" + std::to_string(CurrentDuration()) + "\n";
    }
    ULONGLONG now = GetTickCount64();
    double expected = g_nowPlayingPosition + (playing ? (now - g_nowPlayingAt) / 1000.0 : 0);
    if (text == g_nowPlaying && std::abs(position - expected) < 1.5) return;
    g_nowPlaying = text;
    g_nowPlayingPosition = position;
    g_nowPlayingAt = now;
    WriteFileReplacing(g_nowPlayingFile, text + "position=" + std::to_string(position) + "\n");
}

listenbrainz::Listen MakeListen(TrackId id) {
    const Track& t = g_lib[id];
    listenbrainz::Listen l;
    l.artist = Utf8(t.artist ? t.artist : t.albumArtist);
    l.title = Utf8(t.title);
    l.release = Utf8(t.album);
    l.releaseArtist = Utf8(t.albumArtist);
    l.durationMs = t.durationMs;
    l.trackNumber = t.trackNo;
    return l;
}

// ---------------------------------------------------------------------------
// ListenBrainz window

// The single ListenBrainz dialog, created on demand and destroyed by closing it. `checking` is
// true while a token validation is in flight.
struct LbWindow {
    HWND hwnd = nullptr, intro = nullptr, token = nullptr, status = nullptr, connect = nullptr, disconnect = nullptr,
         close = nullptr;
    std::wstring pendingToken;  // the submitted token, not whatever the edit box holds when the reply arrives
    bool checking = false;
} lb;

// Refreshes the status line and the Turn off button from the current scrobbling state.
void LbStatus() {
    if (!lb.hwnd) return;
    auto s = listenbrainz::GetStatus();
    std::wstring text;
    if (lb.checking) {
        text = L"Checking the token\x2026";
    } else if (listenbrainz::Enabled()) {
        text = s.user.empty() ? L"Scrobbling is on." : L"Scrobbling as " + s.user + L".";
        if (s.queued) text += L"  " + std::to_wstring(s.queued) + (s.queued == 1 ? L" listen waits to be sent." : L" listens wait to be sent.");
        if (!s.lastError.empty()) text += L"  Last problem: " + s.lastError + L".";
    } else {
        text = L"Scrobbling is off.";
    }
    SetWindowTextW(lb.status, text.c_str());
    EnableWindow(lb.disconnect, listenbrainz::Enabled());
}

// Window procedure of the ListenBrainz dialog; lays out the controls on WM_CREATE and clears the
// singleton on WM_NCDESTROY.
LRESULT CALLBACK LbProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            lb.hwnd = h;
            auto make = [&](DWORD ex, const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
                HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, h, (HMENU)(INT_PTR)id, g.inst, nullptr);
                SendMessageW(c, WM_SETFONT, (WPARAM)g.font, TRUE);
                return c;
            };
            int pad = S(14), w = S(460) - pad * 2;
            lb.intro = make(0, WC_STATICW,
                            L"ListenBrainz keeps a public history of what you listen to. cordinal sends a song when it counts as "
                            L"played (half of it, or 4 minutes). Paste your user token from listenbrainz.org/settings:",
                            SS_LEFT | SS_NOPREFIX, 0);
            lb.token = make(WS_EX_CLIENTEDGE, WC_EDITW, L"", ES_AUTOHSCROLL | ES_PASSWORD | WS_TABSTOP, 100);
            lb.status = make(0, WC_STATICW, L"", SS_LEFT | SS_NOPREFIX, 0);
            lb.connect = make(0, WC_BUTTONW, L"&Connect", BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK);
            lb.disconnect = make(0, WC_BUTTONW, L"&Turn off", BS_PUSHBUTTON | WS_TABSTOP, 101);
            lb.close = make(0, WC_BUTTONW, L"Close", BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL);
            MoveWindow(lb.intro, pad, pad, w, S(52), TRUE);
            MoveWindow(lb.token, pad, pad + S(58), w, S(24), TRUE);
            MoveWindow(lb.status, pad, pad + S(92), w, S(36), TRUE);
            int by = pad + S(136);
            MoveWindow(lb.connect, pad, by, S(90), S(26), TRUE);
            MoveWindow(lb.disconnect, pad + S(98), by, S(90), S(26), TRUE);
            MoveWindow(lb.close, pad + w - S(90), by, S(90), S(26), TRUE);
            SetWindowTheme(lb.token, g.dark ? L"DarkMode_CFD" : L"Explorer", nullptr);
            for (HWND c : {lb.connect, lb.disconnect, lb.close}) ApplyControlTheme(c, g.dark);
            ApplyWindowTheme(h, P());
            LbStatus();
            return 0;
        }
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(h, &rc);
            FillRect((HDC)wp, &rc, g.surfaceBrush);
            return 1;
        }
        case WM_CTLCOLORSTATIC:
            return SurfaceControlColor((HDC)wp, (HWND)lp == lb.status ? SubColor() : TextColor());
        case WM_CTLCOLOREDIT:
            return FieldControlColor((HDC)wp);
        case WM_COMMAND:
            if (LOWORD(wp) == IDOK) {
                if (lb.checking) return 0;
                wchar_t token[256];
                GetWindowTextW(lb.token, token, 256);
                std::wstring t = token;
                std::erase_if(t, iswspace);
                if (t.empty()) return 0;
                lb.checking = true;
                lb.pendingToken = t;
                listenbrainz::Validate(t);
                LbStatus();
            } else if (LOWORD(wp) == 101) {
                listenbrainz::SetToken(L"");
                lb.pendingToken.clear();
                LbStatus();
            } else if (LOWORD(wp) == IDCANCEL) {
                DestroyWindow(h);
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_NCDESTROY:
            lb = LbWindow{};
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

bool ListenBrainzDialogMessage(MSG* msg) { return lb.hwnd && IsDialogMessageW(lb.hwnd, msg); }

void ShowListenBrainz() {
    if (lb.hwnd) {
        SetForegroundWindow(lb.hwnd);
        return;
    }
    RegisterAppClass(L"CordinalListenBrainz", LbProc);
    RECT r{0, 0, S(460), S(192)};
    AdjustWindowRectExForDpi(&r, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME, g.dpi);
    RECT owner;
    GetWindowRect(g.hwnd, &owner);
    int w = r.right - r.left, hgt = r.bottom - r.top;
    CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, L"CordinalListenBrainz", L"ListenBrainz scrobbling",
                    WS_CAPTION | WS_SYSMENU, owner.left + (owner.right - owner.left - w) / 2, owner.top + S(100), w, hgt, g.hwnd,
                    nullptr, g.inst, nullptr);
    ShowWindow(lb.hwnd, SW_SHOW);
    SetFocus(lb.token);
}

// ---------------------------------------------------------------------------
// Explorer folder menu

namespace {

// Right-clicking a folder is how Apollo played one, and it is still the shortest path from a
// folder to sound. The verbs live under HKCU\Software\Classes: per user, no elevation, and
// Explorer honours them exactly like machine-wide ones.
constexpr wchar_t kPlayVerb[] = L"Directory\\shell\\Cordinal.Play";
constexpr wchar_t kAddVerb[] = L"Directory\\shell\\Cordinal.Add";
constexpr wchar_t kBackgroundVerb[] = L"Directory\\Background\\shell\\Cordinal.Play";
constexpr wchar_t kPlayLabel[] = L"Play with cordinal";
constexpr wchar_t kAddLabel[] = L"Add to cordinal";

// This executable's own path: the verbs must start the same build the user is running, even
// though cordinal is portable and moves.
std::wstring ExePath() {
    wchar_t exe[MAX_PATH * 2]{};
    DWORD n = GetModuleFileNameW(nullptr, exe, (DWORD)std::size(exe));
    return std::wstring(exe, n);
}

std::wstring QuotedExe() { return L"\"" + ExePath() + L"\""; }

bool WriteValue(HKEY key, const wchar_t* name, const std::wstring& value) {
    return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                          (DWORD)((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

// One verb: its label, the icon Explorer shows and the command it runs.
bool WriteVerb(const wchar_t* base, const wchar_t* subkey, const wchar_t* label, const std::wstring& command) {
    const std::wstring path = std::wstring(base) + L"\\" + subkey;
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS)
        return false;
    const bool labelled = WriteValue(key, nullptr, label) && WriteValue(key, L"Icon", QuotedExe() + L",0");
    RegCloseKey(key);
    if (!labelled) return false;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, (path + L"\\command").c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS)
        return false;
    const bool written = WriteValue(key, nullptr, command);
    RegCloseKey(key);
    return written;
}

// A verb's command line, or empty when the key or value is missing.
std::wstring ReadCommand(const wchar_t* base, const wchar_t* subkey) {
    const std::wstring path = std::wstring(base) + L"\\" + subkey + L"\\command";
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return {};
    wchar_t value[2048]{};
    DWORD size = sizeof(value), type = 0;
    const LSTATUS status = RegQueryValueExW(key, nullptr, nullptr, &type, reinterpret_cast<BYTE*>(value), &size);
    RegCloseKey(key);
    return status == ERROR_SUCCESS && type == REG_SZ ? std::wstring(value) : std::wstring();
}

// Removes a verb and everything under it. A key that is already gone counts as removed.
bool DeleteVerb(const wchar_t* base, const wchar_t* subkey) {
    const std::wstring path = std::wstring(base) + L"\\" + subkey;
    const LSTATUS status = RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
}

std::wstring BackgroundCommand() { return QuotedExe() + L" \"%V\""; }

// The first quoted token of a verb command is the program it starts.
bool CommandRunsThisExe(const std::wstring& command) {
    if (command.size() < 2 || command[0] != L'\"') return false;
    const size_t end = command.find(L'\"', 1);
    if (end == std::wstring::npos) return false;
    return _wcsicmp(command.substr(1, end - 1).c_str(), ExePath().c_str()) == 0;
}

}  // namespace

std::wstring ExplorerPlayCommand() { return QuotedExe() + L" \"%1\""; }
std::wstring ExplorerAddCommand() { return QuotedExe() + L" --add \"%1\""; }

bool InstallExplorerMenu(const wchar_t* base) {
    return WriteVerb(base, kPlayVerb, kPlayLabel, ExplorerPlayCommand()) &&
           WriteVerb(base, kAddVerb, kAddLabel, ExplorerAddCommand()) &&
           WriteVerb(base, kBackgroundVerb, kPlayLabel, BackgroundCommand());
}

bool RemoveExplorerMenu(const wchar_t* base) {
    const bool play = DeleteVerb(base, kPlayVerb), add = DeleteVerb(base, kAddVerb),
               background = DeleteVerb(base, kBackgroundVerb);
    return play && add && background;
}

bool ExplorerMenuInstalled(const wchar_t* base) {
    return ReadCommand(base, kPlayVerb) == ExplorerPlayCommand() &&
           ReadCommand(base, kAddVerb) == ExplorerAddCommand() &&
           ReadCommand(base, kBackgroundVerb) == BackgroundCommand();
}

// A live change from Preferences: the value already says what the user wants.
void ApplyExplorerMenu() {
    if (g.explorerMenu) InstallExplorerMenu();
    else RemoveExplorerMenu();
}

// Startup. Re-registering is what keeps the verbs pointing at this build after the folder is
// moved, and a disabled menu is only cleaned up when it is ours: another portable copy may have
// registered its own path.
void EnsureExplorerMenu() {
    if (g.explorerMenu) {
        if (!ExplorerMenuInstalled()) InstallExplorerMenu();
    } else if (CommandRunsThisExe(ReadCommand(kExplorerBase, kPlayVerb))) {
        RemoveExplorerMenu();
    }
}

// ---------------------------------------------------------------------------

// Crash dumps go in the local cache: they are machine-specific, not part of the user's data.
void InstallCrashReports() { InstallCrashHandler(LocalCacheDir(L"crashes")); }

// Startup: system media controls (falling back to media-key hotkeys), scrobbling, peaks and the
// repeat count, then the one-time crash notice and Steam soundtrack offer.
void InitPlatform(HWND hwnd) {
    g_smtc = smtc::Init(hwnd, WM_APP_SMTC);
    EnsureExplorerMenu();  // the folder verbs follow the preference and this build's path
    wchar_t nowPlaying[MAX_PATH * 4];
    DWORD n = GetEnvironmentVariableW(L"CORDINAL_NOWPLAYING", nowPlaying, (DWORD)std::size(nowPlaying));
    if (n && n < std::size(nowPlaying)) g_nowPlayingFile = nowPlaying;
    if (!g_smtc) {  // older systems: media keys as global hotkeys, as before
        RegisterHotKey(hwnd, 1, MOD_NOREPEAT, VK_MEDIA_PLAY_PAUSE);
        RegisterHotKey(hwnd, 2, MOD_NOREPEAT, VK_MEDIA_NEXT_TRACK);
        RegisterHotKey(hwnd, 3, MOD_NOREPEAT, VK_MEDIA_PREV_TRACK);
        RegisterHotKey(hwnd, 4, MOD_NOREPEAT, VK_MEDIA_STOP);
    }
    listenbrainz::Start(g.dataDir, hwnd, WM_APP_LISTENBRAINZ);
    StartPeaks(hwnd, WM_APP_PEAKS, LocalCacheDir(L"peaks"));
    std::wstring dump;
    if (TakeNewCrashDump(dump)) {
        ShowMessage(L"cordinal closed unexpectedly last time. A crash report was saved in " + DirectoryOf(dump) + L".");
    } else if (!IniInt(L"Library", L"SteamOffered", 0)) {
        // Once: point out bought soundtracks that Steam keeps outside the music folder.
        auto steam = FindSteamMusicFolders();
        std::erase_if(steam, [](const std::wstring& f) {
            return std::any_of(g.libraryFolders.begin(), g.libraryFolders.end(), [&](const std::wstring& l) { return PathIsUnder(f, l) || _wcsicmp(f.c_str(), l.c_str()) == 0; });
        });
        if (!steam.empty()) {
            ShowMessage(L"Found Steam soundtracks: Library \x25B8 Add Steam soundtracks adds them to your library.");
            IniWrite(L"Library", L"SteamOffered", L"1");
        }
    }
}

// Mirrors InitPlatform: stops each subsystem in the reverse order it was set up.
void ShutdownPlatform() {
    if (g_smtc) smtc::Shutdown();
    g_smtc = false;
    for (int i = 1; i <= 4; ++i) UnregisterHotKey(g.hwnd, i);
    if (!g_nowPlayingFile.empty()) {  // tells the bridge cordinal has closed
        DeleteFileW(g_nowPlayingFile.c_str());
        DeleteFileW((g_nowPlayingFile + L".cover").c_str());
    }
    listenbrainz::Stop();
    StopPeaks();
}

// Library > Add Steam soundtracks: appends every uncovered Steam soundtrack folder, then rescans.
void AddSteamSoundtracks() {
    int added = 0;
    for (const std::wstring& f : FindSteamMusicFolders()) {
        bool covered = std::any_of(g.libraryFolders.begin(), g.libraryFolders.end(), [&](const std::wstring& l) {
            return PathIsUnder(f, l) || _wcsicmp(f.c_str(), l.c_str()) == 0;
        });
        if (covered) continue;
        g.libraryFolders.push_back(f);
        ++added;
    }
    IniWrite(L"Library", L"SteamOffered", L"1");
    if (!added) {
        ShowMessage(L"No Steam soundtracks found that aren't in the library already.");
        return;
    }
    SaveLibrarySettings();
    StartScanner();
    ShowMessage(L"Added " + std::to_wstring(added) + (added == 1 ? L" Steam soundtrack folder" : L" Steam soundtrack folders") +
                L" to the library.");
}

// Player > Loop: 0-3 repeats of the current track, applied with an 8 s crossfade.
void SetLoops(int count) {
    g.loops = std::clamp(count, 0, 3);
    g.engine.SetLoops(g.loops, 8.0);
    IniWrite(L"Player", L"Loops", std::to_wstring(g.loops));
}

// The playing track changed (or its tags): the flyout and "playing now" follow.
void PlatformTrackChanged() {
    if (RadioPlaying()) {
        static std::wstring station, title;
        std::wstring currentStation = RadioStationName(), currentTitle = RadioSongTitle();
        if (g_smtcTrack != kNone - 1 || station != currentStation || title != currentTitle) {
            station = std::move(currentStation); title = std::move(currentTitle);
            if (g_smtc) smtc::SetMetadata(title.empty() ? station : title, title.empty() ? L"" : station, L"", {});
        }
        g_smtcTrack = kNone - 1;
        return;
    }
    if (g.playing == g_smtcTrack) return;
    g_smtcTrack = g.playing;
    if (g.playing == kNone) return;
    const Track& t = g_lib[g.playing];
    if (g_smtc)
        smtc::SetMetadata(t.title ? g_lib.Str(t.title) : StemOf(g_lib.Str(t.file)), g_lib.Str(t.artist ? t.artist : t.albumArtist),
                          g_lib.Str(t.album), CoverBytes(g.playing));
    if (listenbrainz::Enabled()) listenbrainz::PlayingNow(MakeListen(g.playing));
}

// With every status update: play state at once, the timeline once a second.
void PlatformTick() {
    WriteNowPlaying();
    if (!g_smtc) return;
    smtc::State s = g.playing == kNone && !RadioPlaying() ? smtc::State::Stopped : g.engine.Paused() ? smtc::State::Paused : smtc::State::Playing;
    if (s != g_smtcState) {
        g_smtcState = s;
        smtc::SetState(s);
        g_smtcTimeline = 0;
    }
    ULONGLONG now = GetTickCount64();
    if (g.playing != kNone && now - g_smtcTimeline >= 1000) {
        g_smtcTimeline = now;
        smtc::SetTimeline(g.engine.Position(), CurrentDuration());
    }
}

// A play that crossed the scrobbling threshold: submit it with the current time.
void PlatformCountedPlay(TrackId id, uint32_t when) {
    if (!listenbrainz::Enabled() || id >= g_lib.Count()) return;
    listenbrainz::Listen l = MakeListen(id);
    l.listenedAt = when;  // ListenBrainz takes past listens at their own time
    listenbrainz::Submit(l);
}

// `--add` is Explorer's "Add to cordinal": the paths that follow join the active playlist and
// playback stays where it is. Removed here so no later reader treats it as a path.
bool TakeAddSwitch(std::vector<std::wstring>& args) {
    const size_t before = args.size();
    std::erase(args, L"--add");
    return args.size() != before;
}

// "cordinal --next" and the like control a running cordinal (Linux media keys use them); with
// none running they do nothing. Anything else on the command line is not a transport command.
std::optional<int> ForwardTransportCommand(const std::vector<std::wstring>& args) {
    if (args.size() != 1 || std::find(std::begin(kTransportCommands), std::end(kTransportCommands), args[0]) == std::end(kTransportCommands))
        return std::nullopt;
    HWND other = FindWindowW(kClassName, nullptr);
    if (!other) return args[0] == L"--web" ? std::nullopt : std::optional<int>(1);
    COPYDATASTRUCT cds{kCopyDataCommand, (DWORD)(args[0].size() * sizeof(wchar_t)), (void*)args[0].data()};
    DWORD_PTR result = 0;
    return SendMessageTimeoutW(other, WM_COPYDATA, 0, (LPARAM)&cds, SMTO_ABORTIFHUNG, 5000, &result) && result ? 0 : 1;
}

void RunTransportCommand(std::wstring_view command) {
    bool playing = (g.playing != kNone || RadioPlaying()) && !g.engine.Paused();
    if (command == L"--play-pause" || (command == L"--play" && !playing) || (command == L"--pause" && playing)) TogglePause();
    else if (command == L"--stop") Stop();
    else if (command == L"--next") Next();
    else if (command == L"--previous") Previous();
    else if (command == L"--web") OpenWebPlayer();
}

// Routes the platform's window messages; returns true when consumed.
bool OnPlatformMessage(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_APP_SMTC) {
        switch ((smtc::Button)wp) {
            case smtc::Button::Play: if ((g.playing == kNone && !RadioPlaying()) || g.engine.Paused()) TogglePause(); break;
            case smtc::Button::Pause: if ((g.playing != kNone || RadioPlaying()) && !g.engine.Paused()) TogglePause(); break;
            case smtc::Button::Stop: Stop(); break;
            case smtc::Button::Next: Next(); break;
            case smtc::Button::Previous: Previous(); break;
        }
        return true;
    }
    if (msg == WM_APP_LISTENBRAINZ) {
        if (wp == 0 || wp == 1 || wp == 2) {  // a Validate result
            std::unique_ptr<std::wstring> user(reinterpret_cast<std::wstring*>(lp));
            lb.checking = false;
            if (wp == 1 && lb.hwnd && !lb.pendingToken.empty()) {
                listenbrainz::SetToken(lb.pendingToken);
                SetWindowTextW(lb.token, L"");
                ShowMessage(L"ListenBrainz: scrobbling as " + (user ? *user : std::wstring(L"you")) + L".");
            } else if (wp == 0) {
                ShowMessage(L"ListenBrainz didn't accept that token.");
            } else if (wp == 2) {
                ShowMessage(L"Couldn't reach ListenBrainz. Check the connection and try again.");
            }
            lb.pendingToken.clear();
        }
        LbStatus();
        return true;
    }
    if (msg == WM_APP_PEAKS) {
        OnPeaksReady((uint32_t)wp);
        return true;
    }
    return false;
}

}  // namespace app

namespace app {

// Library > Find duplicate audio: tracks whose decoded audio is identical (FLAC's MD5), whatever
// their tags or file names, side by side in a playlist.
void FindDuplicateAudioPlaylist() {
    auto groups = g_lib.FindDuplicateAudio();
    if (groups.empty()) {
        ShowMessage(L"No duplicate audio found. (cordinal recognizes identical audio in FLAC files.)");
        return;
    }
    std::vector<TrackId> items;
    for (auto& group : groups) items.insert(items.end(), group.begin(), group.end());
    NewPlaylist(UniquePlaylistName(L"Duplicate audio"));
    Active().items = std::move(items);
    g.dirty = true;
    RebuildView();
    ShowMessage(std::to_wstring(groups.size()) + (groups.size() == 1 ? L" song is" : L" songs are") +
                L" in the library more than once; the copies are listed together.");
}

}  // namespace app
