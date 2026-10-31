// Settings file (settings.ini), data folder layout and saved state: profile, library
// and window placement load/save. settings.ini goes through the Win32 profile API as
// UTF-16, so InitDataPaths() must run before any other function here.
#include "app.h"
#include "native_audio.h"
#include "backup.h"

namespace app {

namespace { uint64_t iniWriteFailures = 0; }

// Thin wrappers over the Win32 profile API, always targeting g.iniPath. Reads take a
// default; IniWrite creates the section/key as needed. InitDataPaths() must have run. Values can
// be long (the pipe-separated library folder list), so a full small buffer retries with the
// largest string the API returns instead of silently dropping the tail.
std::wstring IniRead(const wchar_t* section, const wchar_t* key, const wchar_t* def) {
    std::vector<wchar_t> buf(2048);
    DWORD n = GetPrivateProfileStringW(section, key, def, buf.data(), (DWORD)buf.size(), g.iniPath.c_str());
    if (n == buf.size() - 1) {
        buf.assign(32768, 0);
        n = GetPrivateProfileStringW(section, key, def, buf.data(), (DWORD)buf.size(), g.iniPath.c_str());
    }
    return std::wstring(buf.data(), n);
}
int IniInt(const wchar_t* section, const wchar_t* key, int def) {
    return (int)GetPrivateProfileIntW(section, key, def, g.iniPath.c_str());
}
float IniFloat(const wchar_t* section, const wchar_t* key, float def) {
    std::wstring v = IniRead(section, key, L"");
    return v.empty() ? def : (float)_wtof(v.c_str());
}
// A failed write (a read-only settings.ini, an unavailable profile folder) is reported once, until a
// write succeeds again, so a whole SaveState fails with one message instead of dozens.
bool IniWrite(const wchar_t* section, const wchar_t* key, const std::wstring& value) {
    static bool reported = false;
    if (WritePrivateProfileStringW(section, key, value.c_str(), g.iniPath.c_str())) {
        reported = false;
        return true;
    }
    ++iniWriteFailures;
    if (!reported) ShowMessage(L"Couldn't save settings to " + g.iniPath + L". Is the file read-only?");
    reported = true;
    return false;
}
// Floats are stored fixed at 2 decimals so saved values round-trip stably and never
// become locale-dependent or scientific through the profile API.
std::wstring F2S(float v) {
    wchar_t b[32];
    swprintf_s(b, L"%.2f", v);
    return b;
}
// Comma-separated integer lists (e.g. window placement / column set). ParseInts stops
// at the first non-numeric token, so a corrupted value yields a short list.
std::vector<int> ParseInts(const std::wstring& s) {
    std::vector<int> r;
    for (const wchar_t* p = s.c_str(); *p;) {
        wchar_t* end = nullptr;
        long v = wcstol(p, &end, 10);
        if (end == p) break;
        r.push_back((int)v);
        p = *end == L',' ? end + 1 : end;
    }
    return r;
}
std::wstring JoinInts(const std::vector<int>& v) {
    std::wstring s;
    for (size_t i = 0; i < v.size(); ++i) s += (i ? L"," : L"") + std::to_wstring(v[i]);
    return s;
}

// Resolves the data folder and the ini/library paths. CORDINAL_DATA overrides the
// location (a separate profile, e.g. for testing); otherwise the roaming AppData
// "cordinal" folder. Migrates a pre-rename "Waveforge" profile exactly once.
void InitDataPaths() {
    std::wstring legacyDir;
    wchar_t custom[MAX_PATH];
    PWSTR appData = nullptr;
    // GetEnvironmentVariableW returns 0 when unset and the required length (>= MAX_PATH)
    // when too long; comparing the unsigned value rejects both cases in one test.
    if (GetEnvironmentVariableW(L"CORDINAL_DATA", custom, MAX_PATH) - 1u < MAX_PATH - 1u) {
        g.dataDir = custom;  // a separate profile, e.g. for testing
    } else if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) {
        g.dataDir = std::wstring(appData) + L"\\cordinal";
        legacyDir = std::wstring(appData) + L"\\Waveforge";
        CoTaskMemFree(appData);
    } else {
        g.dataDir = L".";
    }
    // First run after the rename: copy (never move) the Waveforge profile so its playlists and
    // settings carry over and the old folder stays intact.
    if (!legacyDir.empty() && GetFileAttributesW(g.dataDir.c_str()) == INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW(legacyDir.c_str()) != INVALID_FILE_ATTRIBUTES) {
        CreateDirectoryW(g.dataDir.c_str(), nullptr);
        CopyFileW((legacyDir + L"\\settings.ini").c_str(), (g.dataDir + L"\\settings.ini").c_str(), TRUE);
        CopyFileW((legacyDir + L"\\playlists.wfl").c_str(), (g.dataDir + L"\\playlists.wfl").c_str(), TRUE);
        g.iniPath = g.dataDir + L"\\settings.ini";
        wchar_t title[2048];
        GetPrivateProfileStringW(L"Format", L"WindowTitle", L"", title, 2048, g.iniPath.c_str());
        if (!wcscmp(title, L"[%artist% - ]%title% - Waveforge"))
            WritePrivateProfileStringW(L"Format", L"WindowTitle", L"[%artist% - ]%title% - cordinal", g.iniPath.c_str());
    }
    wchar_t full[32768];
    DWORD length = GetFullPathNameW(g.dataDir.c_str(), (DWORD)std::size(full), full, nullptr);
    if (length && length < std::size(full)) g.dataDir = full;
    std::wstring recoveryError;
    if (!backup::Recover(g.dataDir, recoveryError)) {
        MessageBoxW(nullptr, recoveryError.c_str(), L"Couldn't recover the profile", MB_OK | MB_ICONERROR);
        ExitProcess(1);
    }
    CreateDirectoryW(g.dataDir.c_str(), nullptr);
    g.iniPath = g.dataDir + L"\\settings.ini";
    g.libraryPath = g.dataDir + L"\\library.cdb";
    SetMidiSoundFontFolder(g.dataDir + L"\\soundfonts");
    // The profile API keeps a file in UTF-16 only if it already starts with a BOM.
    HANDLE ini = CreateFileW(g.iniPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (ini != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(ini, "\xFF\xFE", 2, &written, nullptr);
        CloseHandle(ini);
    }
}

// Each user-editable title format: read from [Format]; if missing or blank, use the
// built-in default and persist it so the ini always shows the effective format.
void LoadFormats() {
    struct { const wchar_t* key; const wchar_t* def; std::wstring* target; } formats[] = {
        {L"WindowTitle", L"[%artist% - ]%title% - cordinal", &g.fmtWindow},
        {L"StatusBar", L"[%codec% | ][%bitrate% kbps | ][%samplerate% Hz | ][%channels% | ]%playback_time% / %length%",
         &g.fmtStatus},
        {L"NowPlayingTitle", L"%title%", &g.fmtTitle},
        {L"NowPlayingSubtitle", L"[%artist%][  \x2022  %album%][  \x2022  %date%]", &g.fmtSub},
    };
    for (auto& f : formats) {
        *f.target = IniRead(L"Format", f.key, L"");
        if (f.target->empty()) {
            *f.target = f.def;
            IniWrite(L"Format", f.key, f.def);
        }
    }
}

// Each explicit device profile uses the same bounded settings format as the global EQ.
void WriteEqSettings(const wchar_t* section, const EqSettings& eq) {
    IniWrite(section, L"Enabled", eq.enabled ? L"1" : L"0");
    IniWrite(section, L"Preamp", F2S(eq.preamp));
    std::wstring bands;
    for (int i = 0; i < kEqBands; ++i) bands += (i ? L"," : L"") + F2S(eq.gains[i]);
    IniWrite(section, L"Bands", bands);
    IniWrite(section, L"Parametric", eq.parametric ? L"1" : L"0");
    IniWrite(section, L"Crossfeed", eq.crossfeed ? L"1" : L"0");
    IniWrite(section, L"FilterCount", std::to_wstring(eq.filterCount));
    std::string text = FormatAutoEq(eq);
    size_t start = text.find('\n') + 1;
    for (int i = 0; i < eq.filterCount; ++i) {
        size_t end = text.find('\r', start);
        IniWrite(section, (L"Filter" + std::to_wstring(i + 1)).c_str(), FromUtf8(text.substr(start, end - start)));
        start = text.find('\n', end) + 1;
    }
}

EqSettings ReadEqSettings(const wchar_t* section) {
    EqSettings eq;
    eq.enabled = IniInt(section, L"Enabled", 0) != 0;
    eq.preamp = IniFloat(section, L"Preamp", 0);
    eq.preamp = std::isfinite(eq.preamp) ? std::clamp(eq.preamp, -60.0f, 24.0f) : 0;
    std::wstring bands = IniRead(section, L"Bands", L"");
    const wchar_t* p = bands.c_str();
    for (int i = 0; i < kEqBands && *p; ++i) {
        wchar_t* end = nullptr;
        float gain = (float)wcstod(p, &end);
        eq.gains[i] = std::isfinite(gain) ? std::clamp(gain, -12.0f, 12.0f) : 0;
        p = *end == L',' ? end + 1 : end;
    }
    int count = IniInt(section, L"FilterCount", 0);
    if (count > 0 && count <= kParametricBands) {
        std::string text = "Preamp: " + ToUtf8(F2S(eq.preamp)) + " dB\n";
        for (int i = 0; i < count; ++i)
            text += ToUtf8(IniRead(section, (L"Filter" + std::to_wstring(i + 1)).c_str(), L"")) + "\n";
        EqSettings loaded = eq;
        std::wstring error;
        if (ParseAutoEq(text, loaded, error) && loaded.filterCount == count) {
            loaded.enabled = eq.enabled;
            loaded.parametric = IniInt(section, L"Parametric", 0) != 0;
            eq = loaded;
        }
    }
    eq.crossfeed = IniInt(section, L"Crossfeed", 0) != 0;
    return eq;
}

void SaveEq() {
    if (g.rememberOutputEq && !g.eqOutputId.empty()) {
        auto section = OutputProfileSection(g.eqOutputId);
        WriteEqSettings(section.c_str(), g.eq);
    } else {
        g.globalEq = g.eq;
        WriteEqSettings(L"Equalizer", g.eq);
    }
}

void SyncOutputEq(const std::wstring& id) {
    if (id == g.eqOutputId) return;
    g.eqOutputId = id;
    auto section = OutputProfileSection(id);
    const bool matching = !id.empty() && IniRead(section.c_str(), L"ReplayGainHeadroomOutputId", L"") == id;
    const float headroom = matching ? IniFloat(section.c_str(), L"ReplayGainHeadroom", 0) : 0;
    g.rgHeadphoneHeadroom = std::isfinite(headroom) ? std::clamp(headroom, 0.0f, 24.0f) : 0;
    g.rememberOutputEq = !id.empty() && IniInt(section.c_str(), L"Remember", 0) == 1 &&
        IniRead(section.c_str(), L"OutputId", L"") == id;
    g.eq = g.rememberOutputEq ? ReadEqSettings(section.c_str()) : g.globalEq;
    EqApply();
}

void SaveOutputReplayGainHeadroom(float db) {
    if (g.eqOutputId.empty() || !std::isfinite(db) || db < 0 || db > 24) return;
    g.rgHeadphoneHeadroom = db;
    auto section = OutputProfileSection(g.eqOutputId);
    IniWrite(section.c_str(), L"ReplayGainHeadroomOutputId", g.eqOutputId);
    IniWrite(section.c_str(), L"ReplayGainHeadroom", F2S(db));
    RefreshGains();
}

void RememberOutputEq(bool remember) {
    if (g.eqOutputId.empty()) return;
    auto section = OutputProfileSection(g.eqOutputId);
    if (remember) {
        WriteEqSettings(section.c_str(), g.eq);
        IniWrite(section.c_str(), L"OutputId", g.eqOutputId);
        IniWrite(section.c_str(), L"Remember", L"1");
    } else IniWrite(section.c_str(), L"Remember", L"0");
    g.rememberOutputEq = remember;
    if (!remember) { g.eq = g.globalEq; EqApply(); }
}

// Saves the library, keeping g.dirty set on failure so the autosave retries after a
// temporary file lock or unavailable profile folder.
void SaveLibraryNow() {
    if (g.hwnd && IsWindow(g.list)) CapturePlaylistPosition();
    std::wstring error;
    bool saved = SaveLibrary(g.libraryPath, g.playlists, &error);
    if (!saved) ShowMessage(L"Couldn't save the library to " + g.libraryPath + L". " + error);
    else DeleteFileW(UnsavedLibraryPath().c_str());  // library.cdb now holds everything it held
    g.dirty = !saved;  // keep retrying after a temporary lock or unavailable profile folder
    g.ticksSinceSave = 0;
}

std::wstring UnsavedLibraryPath() { return g.libraryPath + L".unsaved"; }

// A lock on library.cdb (a sync client, a virus scanner) usually leaves its folder writable, so the
// changes go beside it rather than being lost with the process. The new copy is written first; the
// old one only goes once the new one is safely on disk.
bool KeepUnsavedLibrary() {
    const std::wstring unsaved = UnsavedLibraryPath();
    if (!SaveLibrary(unsaved, g.playlists)) return false;
    DeleteFileW((unsaved + L".bak").c_str());  // the previous copy, now superseded
    return true;
}

// Only a copy newer than library.cdb wins: one left behind by a failed delete is older than the save
// that followed it.
bool RestoreUnsavedLibrary(std::wstring* keptName) {
    if (keptName) keptName->clear();
    const std::wstring unsaved = UnsavedLibraryPath();
    WIN32_FILE_ATTRIBUTE_DATA kept{}, main{};
    if (!GetFileAttributesExW(unsaved.c_str(), GetFileExInfoStandard, &kept)) return false;
    if (GetFileAttributesExW(g.libraryPath.c_str(), GetFileExInfoStandard, &main) &&
        CompareFileTime(&kept.ftLastWriteTime, &main.ftLastWriteTime) <= 0)
        return false;
    if (LoadLibrary(unsaved, g.playlists, keptName)) {
        g.dirty = true;  // the autosave puts it back into library.cdb and removes the copy
        return true;
    }
    // A locked copy stays in place rather than being moved aside; still name it so the user knows
    // the newer changes were not used.
    if (keptName && keptName->empty()) *keptName = unsaved;
    g_lib = Library();
    g.playlists.clear();
    return false;
}

static bool SaveLibraryCopy() {
    ComPtr<IFileSaveDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
    COMDLG_FILTERSPEC filters[] = {{L"cordinal library", L"*.cdb"}};
    dlg->SetFileTypes(1, filters);
    dlg->SetDefaultExtension(L"cdb");
    dlg->SetFileName(L"library.cdb");
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dlg->Show(g.hwnd)) || FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        return false;
    bool saved = SaveLibrary(path, g.playlists);
    CoTaskMemFree(path);
    return saved;
}

void SaveBeforeClose() {
    SaveState();
    while (g.dirty && !KeepUnsavedLibrary()) {
        enum { kRetry = 100, kCopy, kQuit };
        TASKDIALOG_BUTTON buttons[] = {{kRetry, L"Try again"}, {kCopy, L"Save a copy somewhere else..."},
                                       {kQuit, L"Quit without saving"}};
        const std::wstring text = L"cordinal couldn't write " + g.libraryPath + L" or a copy beside it, so changes "
            L"since the last save would be lost. A copy saved elsewhere can replace library.cdb later.";
        TASKDIALOGCONFIG config{sizeof(config)};
        config.hwndParent = g.hwnd;
        config.dwFlags = TDF_USE_COMMAND_LINKS;
        config.pszWindowTitle = kAppName;
        config.pszMainIcon = TD_WARNING_ICON;
        config.pszMainInstruction = L"Your library couldn't be saved";
        config.pszContent = text.c_str();
        config.cButtons = (UINT)std::size(buttons);
        config.pButtons = buttons;
        int choice = kQuit;
        if (FAILED(TaskDialogIndirect(&config, &choice, nullptr, nullptr)) || choice == kQuit) break;
        if (choice == kCopy) {
            if (SaveLibraryCopy()) break;
            continue;
        }
        SaveState();
    }
}

// Full state save to settings.ini: library first (placement/resume reference playlist
// state), then window, player, ReplayGain, EQ and per-view column/browser settings.
void SaveState() {
    FlushAutomaticPreferences();
    SaveLibraryNow();
    WINDOWPLACEMENT wp{sizeof(wp)};
    if (GetWindowPlacement(g.hwnd, &wp)) {
        if (g.size == ViewSize::Compact) wp = g.fullPlacement;  // the full size it goes back to
        else if (!IsWindowVisible(g.hwnd)) wp.showCmd = SW_SHOWNORMAL;  // hidden behind the mini player
        RECT& r = wp.rcNormalPosition;
        IniWrite(L"Window", L"Placement", JoinInts({(int)r.left, (int)r.top, (int)r.right, (int)r.bottom, (int)wp.showCmd}));
    }
    SaveViewSize();
    SavePreferences();
    IniWrite(L"Player", L"ActivePlaylist", std::to_wstring(g.active));
    // Quitting while a track plays (or after Stop): the next start offers to continue it.
    bool playing = g.playing != kNone && g.resumeOn;
    TrackId resume = playing ? g.playing : g.resumeOn ? g.resumeTrack : kNone;
    bool known = resume != kNone && resume < g_lib.Count();
    IniWrite(L"Player", L"ResumePath", known ? g_lib.Path(resume) : L"");
    IniWrite(L"Player", L"ResumePosition", F2S(known ? (float)(playing ? g.engine.Position() : g.resumePos) : 0.0f));
    IniWrite(L"Player", L"ResumePlaylist", std::to_wstring(playing ? g.playingPl : g.resumePl));
    SaveColumns();
    SaveBrowserSettings();
}

// Save preferences as they change, without rewriting the library or capturing a session.
void SavePreferences() {
    IniWrite(L"Suggestions", L"SavingModeChosen", g.preferenceSavingChosen ? L"1" : L"0");
    IniWrite(L"Window", L"DefaultSize", std::to_wstring((int)g.defaultSize));
    IniWrite(L"Window", L"DefaultMiniPlayer", g.defaultMiniPlayer ? L"1" : L"0");
    WriteSavedTheme(g.defaultTheme);  // a pack theme by name, beside its built-in fallback
    IniWrite(L"Player", L"Volume", std::to_wstring(g.volumePercent));
    IniWrite(L"Player", L"LastAudibleVolume", std::to_wstring(g.lastAudibleVolume));
    IniWrite(L"Player", L"Shuffle", std::to_wstring(g.shuffle));
    IniWrite(L"Player", L"Repeat", std::to_wstring((int)g.repeat));
    SaveTweaks();  // everything listed in the Tweaks registry (app_tweaks.cpp)
    SaveEq();
    SaveMainLayout();
}

bool SaveBackupSettings(std::string& bytes) {
    uint64_t before = iniWriteFailures;
    SaveState();
    return before == iniWriteFailures && WritePrivateProfileStringW(nullptr, nullptr, nullptr, g.iniPath.c_str()) &&
           backup::ReadBytes(g.iniPath, bytes);
}

// Loads everything from disk. Order matters: the library must exist before playlists,
// the active index and the resume track can be resolved against it.
void LoadState() {
    // Fallback chain for the library: the changes that couldn't be saved at the last close, then
    // library.cdb, then its .bak. An unreadable file is kept aside and named to the user. A
    // one-time import of Waveforge's playlists.wfl follows when there is no library at all.
    const bool hadOriginal = GetFileAttributesW(g.libraryPath.c_str()) != INVALID_FILE_ATTRIBUTES;
    std::wstring preserved, failedUnsaved;
    const bool restored = RestoreUnsavedLibrary(&failedUnsaved);
    bool loaded = restored || LoadLibrary(g.libraryPath, g.playlists, &preserved);
    bool fromBackup = false;
    if (!loaded && GetFileAttributesW((g.libraryPath + L".bak").c_str()) != INVALID_FILE_ATTRIBUTES) {
        g_lib = Library();
        std::wstring backupPreserved;
        fromBackup = LoadLibrary(g.libraryPath + L".bak", g.playlists, &backupPreserved);
        if (fromBackup) loaded = true;
        else if (!backupPreserved.empty()) preserved = std::move(backupPreserved);
    }
    std::wstring warning;
    if (!failedUnsaved.empty())
        warning = L"The changes kept from the last close couldn't be read; they were kept as " +
                  FileNameOf(failedUnsaved) + L". ";
    if (!restored && !loaded) {
        if (fromBackup) warning += L"Couldn't read library.cdb; using the previous save. ";
        else if (hadOriginal) warning += L"Couldn't read library.cdb; the library starts empty. ";
        else if (!failedUnsaved.empty() || !preserved.empty()) warning += L"The library starts empty. ";
        if (!preserved.empty()) warning += L"The unreadable file was kept as " + FileNameOf(preserved) + L".";
        else if (hadOriginal && failedUnsaved.empty()) warning += L"The file is still in place.";
    } else if (fromBackup) {
        warning += L"library.cdb couldn't be read; using the previous save. ";
        if (!preserved.empty()) warning += L"The unreadable file was kept as " + FileNameOf(preserved) + L".";
    } else if (!preserved.empty()) {
        warning += L"The unreadable library file was kept as " + FileNameOf(preserved) + L".";
    }
    if (!warning.empty()) {
        g.message = warning;
        g.messageUntil = GetTickCount64() + 15000;
    }
    int active = IniInt(L"Player", L"ActivePlaylist", 0);
    if (restored) {
        g.message = L"Restored changes that couldn't be saved when cordinal last closed.";
        g.messageUntil = GetTickCount64() + 15000;
    }
    if (!loaded) {
        g_lib = Library();
        if (LoadPlaylists(g.dataDir + L"\\playlists.wfl", g.playlists)) {
            ++active;  // the Library tab is inserted in front
            g.dirty = true;
        }
    }
    EnsureSpecialPlaylists();
    LoadLibrarySettings();
    if (!PointsInitialized()) {  // a library saved before points existed: back pay, once
        // Runs inside WM_CREATE, before the status bar exists, so no level-up message here.
        GrantBackPayIfNeeded();
        g.dirty = true;
    }
    ClearBrowserCaches();
    if (IniInt(L"Library", L"DefaultAutoPlaylists", 0) == 0) {
        AddDefaultAutoPlaylists();
        IniWrite(L"Library", L"DefaultAutoPlaylists", L"1");
    }
    std::vector<TrackId> unloaded;
    for (TrackId id = 0; id < (TrackId)g_lib.Count(); ++id)
        if (!g_lib[id].Loaded()) unloaded.push_back(id);
    QueueTags(unloaded);
    g.active = std::clamp(active, 0, (int)g.playlists.size() - 1);
    if (std::wstring path = IniRead(L"Player", L"ResumePath", L""); !path.empty() &&
        IniInt(L"Player", L"ResumeOn", 1) != 0) {
        TrackId id = g_lib.Find(path);
        if (id < g_lib.Count()) {
            g.resumeTrack = id;
            g.resumePos = std::max(0.0f, IniFloat(L"Player", L"ResumePosition", 0));
            g.resumePl = IniInt(L"Player", L"ResumePlaylist", -1);
            if (g.resumePl >= (int)g.playlists.size()) g.resumePl = -1;
        }
    }
    LoadPreferences();
    ApplyPackFormats();  // the user's own patterns, then those of the packs that are on
}

void LoadPreferences() {
    int saving = IniInt(L"Suggestions", L"SavingMode", (int)PreferenceSaving::Ask);
    g.preferenceSaving = saving >= 0 && saving < (int)PreferenceSaving::Count ?
        (PreferenceSaving)saving : PreferenceSaving::Ask;
    g.preferenceSavingChosen = saving >= 0 && saving < (int)PreferenceSaving::Count &&
        IniInt(L"Suggestions", L"SavingModeChosen", 0) != 0;
    // Older profiles remembered the last size; keep it until the user approves a new default.
    g.defaultSize = (ViewSize)std::clamp(IniInt(L"Window", L"DefaultSize",
        IniInt(L"Window", L"Size", (int)ViewSize::Normal)), 0, (int)ViewSize::Count - 1);
    g.defaultMiniPlayer = IniInt(L"Window", L"DefaultMiniPlayer", 0) != 0;
    g.themeMode = ReadSavedTheme();
    g.defaultTheme = g.themeMode;
    // Before 2.1 one "Order" held both: 1 repeat playlist, 2 repeat track, 3 random, 4 shuffle.
    // Random and shuffle never ended, so they become shuffle with repeat.
    int order = IniInt(L"Player", L"Order", 0);
    g.shuffle = std::clamp(IniInt(L"Player", L"Shuffle", order == 3 || order == 4), 0, kShuffleCount - 1);
    g.repeat = (Repeat)std::clamp(IniInt(L"Player", L"Repeat", order == 2 ? 2 : order ? 1 : 0), 0, (int)Repeat::Count - 1);
    g.volumePercent = std::clamp(IniInt(L"Player", L"Volume", 80), 0, 100);
    g.lastAudibleVolume = g.volumePercent > 0 ? g.volumePercent :
        std::clamp(IniInt(L"Player", L"LastAudibleVolume", 80), 1, 100);
    // Skip-aware shuffle, radio, output, ReplayGain, effects, conversion and the view toggles. The
    // reverb amount is not the old ReverbMix key: its 8-30% amounts were too faint to hear.
    LoadTweaks();
    for (auto& [token, info] : g.tokens) info.radioNext = UINT32_MAX;
    // Settings from before the single model kept a Mode (0 = off); any other mode means on.
    g.rgOn = IniInt(L"ReplayGain", L"Enabled", IniInt(L"ReplayGain", L"Mode", 1) != 0) != 0;
    g.globalEq = g.eq = ReadEqSettings(L"Equalizer");
    g.rgHeadphoneHeadroom = 0;
    g.eqOutputId.clear();
    g.rememberOutputEq = false;
    LoadMainLayout();
}

// Applies the saved window placement only if it is plausibly usable: both dimensions
// pass a minimum and the rect intersects a monitor. Otherwise keeps the shell's
// showCmd. Delegates the actual restore to RestoreViewSize (handles compact mode).
void RestoreWindowPlacement(int showCmd) {
    WINDOWPLACEMENT wp{sizeof(wp)};
    bool saved = false;
    auto v = ParseInts(IniRead(L"Window", L"Placement", L""));
    if (v.size() == 5) {
        RECT r{v[0], v[1], v[2], v[3]};
        if (r.right - r.left > 200 && r.bottom - r.top > 150 && MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) {
            wp.rcNormalPosition = r;
            wp.showCmd = v[4] == SW_SHOWMAXIMIZED ? SW_SHOWMAXIMIZED : showCmd;
            saved = true;
        }
    }
    if (!saved) {
        GetWindowPlacement(g.hwnd, &wp);
        wp.showCmd = showCmd;
    }
    RestoreViewSize(wp, showCmd);
}

}  // namespace app
