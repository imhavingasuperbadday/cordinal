// Windows and online integration: the system media controls (media keys, the volume flyout,
// lock screen, headset buttons), transport commands ("cordinal --next"), Explorer's folder
// verbs ("Play with cordinal"), the now-playing file for Linux media controls, ListenBrainz
// scrobbling and its window, the Discord status, crash reports, Steam soundtrack folders and the game-music loop
// setting.
#include "app.h"
#include "album_editions.h"
#include "backup.h"
#include "crashdump.h"
#include "customcover.h"
#include "discord.h"
#include "listenbrainz.h"
#include "peaks.h"
#include "smtc.h"
#include "steam.h"

namespace app {

namespace {

bool g_smtc = false;       // the system media controls work; otherwise media keys are hotkeys
TrackId g_smtcTrack = kNone;
unsigned long long g_scrobbleToken = 0;
uint32_t g_scrobbleStarted = 0;
smtc::State g_smtcState = smtc::State::Closed;
ULONGLONG g_smtcTimeline = 0;
std::thread g_steamWorker;
std::atomic<bool> g_steamCancel{false};  // shared only with the discovery worker
bool g_steamAdding = false;            // UI thread: a manual request can adopt an offer in flight

void StartSteamDiscovery(bool offer) {
    if (g.closing) return;
    if (!offer) g_steamAdding = true;
    if (g_steamWorker.joinable()) return;
    g_steamAdding = !offer;
    g_steamCancel = false;
    const HWND hwnd = g.hwnd;
    auto known = offer ? std::vector<SteamSong>() : SnapshotSteamSongs(g_lib);
    g_steamWorker = std::thread([hwnd, offer, known = std::move(known)]() mutable {
        auto result = std::make_unique<SteamImport>();
        WPARAM state = offer ? 1 : 0;
        try {
            result->folders = FindSteamMusicFolders(&g_steamCancel);
            if (!offer && !PrepareSteamImport(*result, std::move(known), &g_steamCancel)) state |= 2;
        } catch (...) {
            state |= 2;
        }
        if (PostMessageW(hwnd, WM_APP_STEAM, state, (LPARAM)result.get())) result.release();
    });
}

// On Linux (Wine) the launcher sets CORDINAL_NOWPLAYING: what is playing is written there for its
// MPRIS bridge (media keys, playerctl, the bar's media widget), with the cover as <file>.cover.
std::wstring g_nowPlayingFile;
std::string g_nowPlaying;  // the last text written, without the position
TrackId g_nowPlayingTrack = kNone;
double g_nowPlayingPosition = 0;
ULONGLONG g_nowPlayingAt = 0;  // when that position was written

const wchar_t* const kTransportCommands[] = {L"--play-pause", L"--play", L"--pause", L"--stop", L"--next", L"--previous", L"--web"};

// A string-pool handle as UTF-8, which is what the network submission needs.
std::string Utf8(StringPool::Handle h) { return std::string(g_lib.View(h)); }

// The cover as encoded bytes, for the media flyout: a cover chosen in cordinal, the embedded art,
// else an image next to the file.
std::string CoverBytes(TrackId id) {
    std::wstring path = g_lib.Path(id), file;
    std::string image;
    if (std::wstring custom = customcover::ImageFor(path); !custom.empty() && backup::ReadBytes(custom, image, 8u << 20))
        return image;
    image.clear();
    int index = 0;
    if (SplitSubtrack(path, file, index)) path = file;
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

// One setup window serves every service; credentials are committed after server validation.
struct LbWindow {
    HWND hwnd = nullptr, service = nullptr, intro = nullptr, token = nullptr, key = nullptr, secret = nullptr,
         keyLabel = nullptr, secretLabel = nullptr, status = nullptr, connect = nullptr, disconnect = nullptr,
         account = nullptr, close = nullptr;
    listenbrainz::Service selected = listenbrainz::Service::ListenBrainz;
    std::wstring pendingToken, authKey, authSecret, authToken, message;
    bool checking = false;
} lb;

void LbStatus() {
    if (!lb.hwnd) return;
    auto s = listenbrainz::GetStatus(lb.selected);
    std::wstring text = lb.checking ? L"Contacting the service..." : lb.message;
    if (text.empty()) {
        text = listenbrainz::Enabled(lb.selected) ? (s.user.empty() ? L"Connected." : L"Connected as " + s.user + L".") :
               L"Not connected. Scrobbling is off.";
        if (listenbrainz::Paused()) text += L" Scrobbling is paused in Preferences.";
        if (s.queued) text += L" " + std::to_wstring(s.queued) + L" listens waiting.";
        if (!s.lastError.empty()) text += L" " + s.lastError + L".";
    }
    SetWindowTextW(lb.status, text.c_str());
    SetWindowTextW(lb.connect, lb.authToken.empty() ? L"&Connect" : L"&Finish sign-in");
    for (HWND c : {lb.connect, lb.token, lb.key, lb.secret}) EnableWindow(c, !lb.checking);
    EnableWindow(lb.disconnect, listenbrainz::Enabled(lb.selected) || lb.checking || !lb.authToken.empty());
}
void LbService() {
    bool brainz = lb.selected == listenbrainz::Service::ListenBrainz;
    SetWindowTextW(lb.intro, brainz ?
        L"ListenBrainz keeps your listening history online. Paste your user token from its settings. "
        L"Songs are sent after half their duration or four minutes of listening." :
        L"Enter this service's application API key and secret, then Connect. Approve cordinal in your browser "
        L"and return here to Finish sign-in. Tracks must be longer than 30 seconds.");
    for (HWND c : {lb.key, lb.secret, lb.keyLabel, lb.secretLabel}) ShowWindow(c, brainz ? SW_HIDE : SW_SHOW);
    ShowWindow(lb.token, brainz ? SW_SHOW : SW_HIDE);
    SetWindowTextW(lb.account, brainz ? L"&Get user token" : L"&Get API key");
    LbStatus();
}
std::wstring LbText(HWND field) {
    wchar_t text[1024]{}; GetWindowTextW(field, text, (int)std::size(text));
    return text;
}
LRESULT CALLBACK LbProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            lb.hwnd = h;
            auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int w, int ht) {
                HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                    S(x), S(y), S(w), S(ht), h, (HMENU)(INT_PTR)id, g.inst, nullptr);
                SendMessageW(c, WM_SETFONT, (WPARAM)g.font, TRUE);
                ApplyControlTheme(c, g.dark);
                return c;
            };
            make(WC_STATICW, L"&Service", SS_LEFT, 0, 14, 18, 74, 22);
            lb.service = make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 102, 92, 14, 374, 160);
            for (auto name : {L"ListenBrainz", L"Last.fm", L"Libre.fm"}) SendMessageW(lb.service, CB_ADDSTRING, 0, (LPARAM)name);
            SendMessageW(lb.service, CB_SETCURSEL, 0, 0);
            lb.intro = make(WC_STATICW, L"", SS_LEFT | SS_NOPREFIX, 0, 14, 52, 452, 64);
            lb.token = make(WC_EDITW, L"", ES_AUTOHSCROLL | ES_PASSWORD | WS_BORDER | WS_TABSTOP, 100, 14, 124, 452, 26);
            lb.keyLabel = make(WC_STATICW, L"API &key", SS_LEFT, 0, 14, 126, 86, 22);
            lb.key = make(WC_EDITW, L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 103, 104, 122, 362, 26);
            lb.secretLabel = make(WC_STATICW, L"API &secret", SS_LEFT, 0, 14, 160, 86, 22);
            lb.secret = make(WC_EDITW, L"", ES_AUTOHSCROLL | ES_PASSWORD | WS_BORDER | WS_TABSTOP, 104, 104, 156, 362, 26);
            lb.account = make(WC_BUTTONW, L"", BS_PUSHBUTTON | WS_TABSTOP, 105, 14, 194, 132, 26);
            lb.status = make(WC_STATICW, L"", SS_LEFT | SS_NOPREFIX, 0, 14, 234, 452, 64);
            lb.connect = make(WC_BUTTONW, L"&Connect", BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK, 14, 310, 124, 28);
            lb.disconnect = make(WC_BUTTONW, L"&Disconnect", BS_PUSHBUTTON | WS_TABSTOP, 101, 146, 310, 116, 28);
            lb.close = make(WC_BUTTONW, L"Close", BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL, 376, 310, 90, 28);
            ApplyWindowTheme(h, P()); LbService(); return 0;
        }
        case WM_ERASEBKGND: {
            RECT rc; GetClientRect(h, &rc); FillRect((HDC)wp, &rc, g.surfaceBrush); return 1;
        }
        case WM_CTLCOLORSTATIC: return SurfaceControlColor((HDC)wp, (HWND)lp == lb.status ? SubColor() : TextColor());
        case WM_CTLCOLOREDIT: return FieldControlColor((HDC)wp);
        case WM_COMMAND:
            if (LOWORD(wp) == 102 && HIWORD(wp) == CBN_SELCHANGE) {
                listenbrainz::CancelAuthorization(lb.selected);
                lb.selected = (listenbrainz::Service)SendMessageW(lb.service, CB_GETCURSEL, 0, 0);
                lb.checking = false; lb.pendingToken.clear(); lb.authKey.clear(); lb.authSecret.clear();
                lb.authToken.clear(); lb.message.clear();
                for (HWND c : {lb.token, lb.key, lb.secret}) SetWindowTextW(c, L"");
                LbService();
            } else if (LOWORD(wp) == IDOK && !lb.checking) {
                lb.message.clear();
                if (lb.selected == listenbrainz::Service::ListenBrainz) {
                    lb.pendingToken = LbText(lb.token);
                    if (lb.pendingToken.empty()) { SetFocus(lb.token); return 0; }
                    lb.checking = true; listenbrainz::Validate(lb.pendingToken);
                } else {
                    if (lb.authToken.empty()) { lb.authKey = LbText(lb.key); lb.authSecret = LbText(lb.secret); }
                    if (lb.authKey.empty() || lb.authSecret.empty()) {
                        lb.message = L"Enter the API key and secret from this service first."; LbStatus(); return 0;
                    }
                    lb.checking = true;
                    listenbrainz::Authorize(lb.selected, lb.authKey, lb.authSecret, lb.authToken);
                }
                LbStatus();
            } else if (LOWORD(wp) == 101) {
                listenbrainz::CancelAuthorization(lb.selected);
                bool saved = listenbrainz::SetToken(L"", lb.selected);
                lb.checking = false; lb.pendingToken.clear(); lb.authToken.clear(); lb.authKey.clear(); lb.authSecret.clear();
                for (HWND c : {lb.token, lb.key, lb.secret}) SetWindowTextW(c, L"");
                lb.message = saved ? L"Disconnected. Scrobbling is off." : L"Could not remove the saved connection.";
                LbStatus();
            } else if (LOWORD(wp) == 105) {
                const wchar_t* url = lb.selected == listenbrainz::Service::ListenBrainz ? L"https://listenbrainz.org/settings/" :
                    lb.selected == listenbrainz::Service::LastFm ? L"https://www.last.fm/api/account/create" : L"https://libre.fm/api-keys.php";
                ShellExecuteW(h, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
            } else if (LOWORD(wp) == IDCANCEL) DestroyWindow(h);
            return 0;
        case WM_CLOSE: DestroyWindow(h); return 0;
        case WM_NCDESTROY:
            listenbrainz::CancelAuthorization(lb.selected); lb = LbWindow{}; return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

// Falls back to the data directory when the known folder is unavailable. CoTaskMemFree is required
// even when the call fails.
std::wstring LocalCacheDir(const wchar_t* name) {
    PWSTR local = nullptr;
    std::wstring dir = g.dataDir + L"\\" + name;
    if (GetEnvironmentVariableW(L"CORDINAL_DATA", nullptr, 0)) return dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) dir = std::wstring(local) + L"\\cordinal\\" + name;
    CoTaskMemFree(local);
    return dir;
}

bool ListenBrainzDialogMessage(MSG* msg) { return lb.hwnd && IsDialogMessageW(lb.hwnd, msg); }

void ShowListenBrainz() {
    if (lb.hwnd) {
        SetForegroundWindow(lb.hwnd);
        return;
    }
    RegisterAppClass(L"CordinalListenBrainz", LbProc);
    RECT r{0, 0, S(480), S(352)};
    AdjustWindowRectExForDpi(&r, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME, g.dpi);
    RECT owner;
    GetWindowRect(g.hwnd, &owner);
    int w = r.right - r.left, hgt = r.bottom - r.top;
    CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, L"CordinalListenBrainz", L"Scrobbling",
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
    std::wstring exe(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, exe.data(), (DWORD)exe.size());
    exe.resize(n && n < exe.size() ? n : 0);
    return exe;
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
        StartSteamDiscovery(true);
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
    StartSteamDiscovery(false);
    if (!g.closing) ShowMessage(L"Looking for Steam soundtracks...");
}

bool SteamDiscoveryRunning() { return g_steamWorker.joinable(); }

void CancelSteamDiscovery() {
    if (!SteamDiscoveryRunning()) return;
    g_steamCancel = true;
    ShowMessage(L"Cancelling the soundtrack search...");
}

// Apply the result on the UI thread, comparing with the current watched folders in case the
// user edited them while discovery was running. A cancelled search never saves partial roots.
void OnSteamDiscovery(WPARAM state, LPARAM lp) {
    std::unique_ptr<SteamImport> result(reinterpret_cast<SteamImport*>(lp));
    if (g_steamWorker.joinable()) g_steamWorker.join();
    const bool adding = g_steamAdding;
    g_steamAdding = false;
    if (!result || g.closing) return;
    if (g_steamCancel || (state & 2)) {
        if (adding) ShowMessage(g_steamCancel ? L"Soundtrack search cancelled." : L"Couldn't search for Steam soundtracks.");
        return;
    }
    if (adding && (state & 1)) { StartSteamDiscovery(false); return; }
    auto folders = result->folders;
    std::erase_if(folders, [](const std::wstring& f) {
        return std::any_of(g.libraryFolders.begin(), g.libraryFolders.end(), [&](const std::wstring& l) {
            return PathIsUnder(f, l) || _wcsicmp(f.c_str(), l.c_str()) == 0;
        });
    });
    if (!adding) {
        if (!folders.empty()) {
            ShowMessage(L"Found Steam soundtracks: Library \x25B8 Add Steam soundtracks adds them to your library.");
            IniWrite(L"Library", L"SteamOffered", L"1");
        }
        return;
    }
    int formats = IDNO;
    if (result->multipleFormats) {
        formats = MessageBoxW(g.hwnd,
            L"Steam soundtracks include more than one audio format.\n\n"
            L"Keep a copy of each song in every format?\n\n"
            L"Yes keeps each format. No keeps one copy, preferring songs already in your Library.\n"
            L"Cancel leaves the Library unchanged.", L"Add Steam soundtracks",
            MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2);
        if (formats != IDYES && formats != IDNO) return;
    }
    const size_t skipped = SkipSteamDuplicates(g_lib, *result, formats == IDYES);
    g.dirty = true;
    g.compactPending = true;
    RebuildView(true);
    int added = 0;
    for (const std::wstring& f : folders) {
        g.libraryFolders.push_back(f);
        ++added;
    }
    IniWrite(L"Library", L"SteamOffered", L"1");
    if (!added) {
        StartScanner();
        ShowMessage(skipped ? L"Skipped " + std::to_wstring(skipped) + L" duplicate Steam tracks."
                            : L"No Steam soundtracks found that aren't in the library already.");
        return;
    }
    SaveLibrarySettings();
    StartScanner();
    ShowMessage(L"Added " + std::to_wstring(added) + (added == 1 ? L" Steam soundtrack folder" : L" Steam soundtrack folders") +
                L" to the library. Skipped " + std::to_wstring(skipped) + L" duplicate tracks.");
}

void StopSteamDiscovery() {
    g_steamCancel = true;
    if (g_steamWorker.joinable()) g_steamWorker.join();
    g_steamAdding = false;
    MSG pending;
    while (PeekMessageW(&pending, g.hwnd, WM_APP_STEAM, WM_APP_STEAM, PM_REMOVE))
        delete reinterpret_cast<SteamImport*>(pending.lParam);
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
    if (g.playing == g_smtcTrack && g.playingToken == g_scrobbleToken) return;
    g_scrobbleToken = g.playingToken;
    g_scrobbleStarted = g.playing == kNone ? 0 : UnixNow();
    g_smtcTrack = g.playing;
    if (g.playing == kNone) return;
    const Track& t = g_lib[g.playing];
    if (g_smtc)
        smtc::SetMetadata(t.title ? g_lib.Str(t.title) : StemOf(g_lib.Str(t.file)), g_lib.Str(t.artist ? t.artist : t.albumArtist),
                          g_lib.Str(t.album), CoverBytes(g.playing));
    listenbrainz::PlayingNow(MakeListen(g.playing));
}

// The Discord status follows a new track or a pause at once, and checks for a seek once a second. Off, it costs
// one comparison.
void DiscordTick() {
    static unsigned long long token = 0;
    static bool paused = false;
    static ULONGLONG checked = 0;
    if (!discord::Available() || discord::Mode() == discord::Show::Off) return;
    discord::NowPlaying now;
    now.radio = RadioPlaying();
    now.active = now.radio || g.playing != kNone;
    now.paused = now.active && g.engine.Paused();
    ULONGLONG tick = GetTickCount64();
    if (token == g.playingToken && paused == now.paused && tick - checked < 1000) return;
    token = g.playingToken;
    paused = now.paused;
    checked = tick;
    if (now.radio) {
        now.title = ToUtf8(RadioSongTitle());
        now.artist = ToUtf8(RadioStationName());
    } else if (now.active) {
        const Track& t = g_lib[g.playing];
        now.title = t.title ? Utf8(t.title) : ToUtf8(StemOf(g_lib.Str(t.file)));
        now.artist = Utf8(t.artist ? t.artist : t.albumArtist);
        now.album = Utf8(t.album);
        now.albumArtist = Utf8(t.albumArtist);
        if (const AlbumEdition* edition = AlbumEditionForTrack(g.playing)) now.release = edition->musicBrainzRelease;
        now.path = g_lib.Path(g.playing);  // the track's custom cover, when it has one
        now.position = g.engine.Position();
        now.duration = CurrentDuration();
        now.rate = g.speed * g.tempo;
    }
    discord::Update(now);
}

// With every status update: play state at once, the timeline once a second.
void PlatformTick() {
    WriteNowPlaying();
    DiscordTick();
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

// A counted play carries its start time; a paired browser supplies its own credited interval.
void PlatformCountedPlay(TrackId id, uint32_t when, double seconds, bool local) {
    if (id >= g_lib.Count()) return;
    listenbrainz::Listen l = MakeListen(id);
    l.listenedAt = local && g_smtcTrack == id && g_scrobbleStarted ? g_scrobbleStarted :
        when > seconds ? when - (uint32_t)seconds : 1;
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
        auto service = (listenbrainz::Service)((wp >> 4) & 3);
        unsigned result = (unsigned)wp % 16;
        std::unique_ptr<std::wstring> value(reinterpret_cast<std::wstring*>(lp));
        if (result == listenbrainz::kStatusChanged && lb.selected == service) lb.message.clear();
        if (listenbrainz::CurrentResult(wp) && lb.hwnd && lb.selected == service && result != listenbrainz::kStatusChanged && lb.checking) {
            lb.checking = false;
            if (result == 4 && value) {
                lb.authToken = *value;
                auto url = listenbrainz::AuthorizationUrl(service, lb.authKey, lb.authToken);
                ShellExecuteW(lb.hwnd, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                lb.message = L"Approve access in your browser, then choose Finish sign-in.";
            } else if ((result == 1 || result == 5) && value) {
                bool saved = listenbrainz::SetToken(result == 1 ? lb.pendingToken : *value, service);
                lb.message = saved ? L"Connected. Scrobbling is on." : L"Could not save the connection.";
                if (saved) {
                    for (HWND c : {lb.token, lb.key, lb.secret}) SetWindowTextW(c, L"");
                    lb.authToken.clear(); lb.authKey.clear(); lb.authSecret.clear(); lb.pendingToken.clear();
                }
            } else {
                lb.message = result == 2 ? L"Couldn't reach the service. Check your connection and try again." :
                    L"Sign-in wasn't accepted. Check the credentials and browser approval, then retry.";
            }
        }
        LbStatus();
        return true;
    }
    if (msg == WM_APP_PEAKS) {
        if (wp) OnPeaksReady((uint32_t)wp);
        else OnListPeaksReady();  // request 0: a Mood column track (peaks.h ListPeaks)
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
