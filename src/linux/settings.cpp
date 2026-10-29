// Where cordinal keeps things on Linux, and its settings file.
//   $XDG_DATA_HOME/cordinal/library.cdb      the library (the same format as on Windows)
//   $XDG_CONFIG_HOME/cordinal/settings.ini   settings, as key=value lines
#include "app.h"
#include "backup.h"
#include "points.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <map>

namespace lx {

namespace {

std::wstring EnvDir(const char* xdg, const char* fallback) {
    const char* v = getenv(xdg);
    if (v && *v == '/') return compat::Widen(v);
    const char* home = getenv("HOME");
    return compat::Widen(home ? home : "/tmp") + L"/" + compat::Widen(fallback);
}

std::map<std::string, std::string> ReadIni(const std::wstring& path) {
    std::map<std::string, std::string> values;
    FILE* f = fopen(compat::Narrow(path.c_str()).c_str(), "r");
    if (!f) return values;
    char line[8192];
    while (fgets(line, sizeof(line), f)) {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        size_t eq = s.find('=');
        if (s.empty() || s[0] == '#' || eq == std::string::npos) continue;
        values[s.substr(0, eq)] = s.substr(eq + 1);
    }
    fclose(f);
    return values;
}

int ToInt(const std::map<std::string, std::string>& v, const char* key, int fallback) {
    auto it = v.find(key);
    return it == v.end() || it->second.empty() ? fallback : atoi(it->second.c_str());
}

float ToFloat(const std::map<std::string, std::string>& v, const char* key, float fallback, float lo, float hi) {
    auto it = v.find(key);
    if (it == v.end() || it->second.empty()) return fallback;
    float f = strtof(it->second.c_str(), nullptr);
    return std::isfinite(f) ? std::clamp(f, lo, hi) : fallback;
}

std::string FloatText(float f) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%g", f);
    return buf;
}

// The equalizer as the Windows build keeps it ([Equalizer] in its settings.ini): the sliders, and
// an imported parametric curve as AutoEq filter lines.
void LoadEq(const std::map<std::string, std::string>& v, EqSettings& eq) {
    eq.enabled = ToInt(v, "eq_enabled", 0) != 0;
    eq.preamp = ToFloat(v, "eq_preamp", 0, -60, 24);
    if (auto it = v.find("eq_bands"); it != v.end()) {
        const char* p = it->second.c_str();
        for (int i = 0; i < kEqBands && *p; ++i) {
            char* end = nullptr;
            float gain = strtof(p, &end);
            if (end == p) break;
            eq.gains[i] = std::isfinite(gain) ? std::clamp(gain, -12.0f, 12.0f) : 0;
            p = *end == ',' ? end + 1 : end;
        }
    }
    int count = ToInt(v, "eq_filter_count", 0);
    if (count > 0 && count <= kParametricBands) {
        std::string text = "Preamp: " + FloatText(eq.preamp) + " dB\n";
        for (int i = 0; i < count; ++i)
            if (auto it = v.find("eq_filter" + std::to_string(i + 1)); it != v.end()) text += it->second + "\n";
        EqSettings loaded = eq;
        std::wstring error;
        if (ParseAutoEq(text, loaded, error) && loaded.filterCount == count) {
            loaded.enabled = eq.enabled;
            loaded.parametric = ToInt(v, "eq_parametric", 0) != 0;
            eq = loaded;
        }
    }
    eq.crossfeed = ToInt(v, "eq_crossfeed", 0) != 0;
}

std::string EqText(const EqSettings& eq) {
    std::string text = "eq_enabled=" + std::to_string(eq.enabled) + "\neq_preamp=" + FloatText(eq.preamp) + "\neq_bands=";
    for (int i = 0; i < kEqBands; ++i) text += (i ? "," : "") + FloatText(eq.gains[i]);
    text += "\neq_parametric=" + std::to_string(eq.parametric) + "\neq_crossfeed=" + std::to_string(eq.crossfeed) +
            "\neq_filter_count=" + std::to_string(eq.filterCount) + "\n";
    // FormatAutoEq's first line is the preamp; each filter follows on a line of its own.
    std::string autoEq = FormatAutoEq(eq);
    size_t start = autoEq.find('\n');
    for (int i = 0; i < eq.filterCount && start != std::string::npos; ++i) {
        size_t end = autoEq.find('\n', ++start);
        std::string line = autoEq.substr(start, end == std::string::npos ? std::string::npos : end - start);
        while (!line.empty() && line.back() == '\r') line.pop_back();
        text += "eq_filter" + std::to_string(i + 1) + "=" + line + "\n";
        start = end;
    }
    return text;
}

void EnsureSpecialPlaylists() {
    auto find = [](PlaylistKind kind) {
        for (size_t i = 0; i < g.playlists.size(); ++i)
            if (g.playlists[i].kind == kind) return (int)i;
        return -1;
    };
    if (find(PlaylistKind::Library) < 0) g.playlists.insert(g.playlists.begin(), Playlist{L"Library", {}, PlaylistKind::Library});
    if (find(PlaylistKind::NowPlaying) < 0) {
        int lib = find(PlaylistKind::Library);
        g.playlists.insert(g.playlists.begin() + lib + 1, Playlist{L"Now Playing", {}, PlaylistKind::NowPlaying});
    }
}

}  // namespace

void InitPaths() {
    g.dataDir = EnvDir("XDG_DATA_HOME", ".local/share") + L"/cordinal";
    g.configDir = EnvDir("XDG_CONFIG_HOME", ".config") + L"/cordinal";
    std::wstring error;
    if (!backup::Recover(g.dataDir,error)) { fprintf(stderr,"%s\n",ToUtf8(error).c_str()); exit(1); }
    SHCreateDirectoryExW(nullptr, g.dataDir.c_str(), nullptr);
    SHCreateDirectoryExW(nullptr, g.configDir.c_str(), nullptr);
    g.libraryPath = g.dataDir + L"/library.cdb";
    g.settingsPath = g.configDir + L"/settings.ini";
    // A restored profile carries its settings with it; the previous config file remains intact.
    if (GetFileAttributesW((g.dataDir+L"/settings.ini").c_str()) != INVALID_FILE_ATTRIBUTES)
        g.settingsPath = g.dataDir+L"/settings.ini";
}

void LoadSettings() {
    auto v = ReadIni(g.settingsPath);
    Settings& s = g.settings;
    int port = ToInt(v, "web_port", 8787);
    s.webPort = port >= 1 && port <= 65535 ? (uint16_t)port : 8787;
    s.webStart = ToInt(v, "web_start", 0) != 0;
    s.webAway = ToInt(v, "web_away", 0) != 0;
    s.webAwayAgreed = ToInt(v, "web_away_agreed", 0) != 0;
    int awayPort = ToInt(v, "web_away_port", 443);
    s.webAwayPort = awayPort >= 1 && awayPort <= 65535 && awayPort != 80 ? (uint16_t)awayPort : 443;
    // A stored code that no longer passes the check (edited by hand) is ignored rather than trusted.
    s.webCode.clear();
    if (auto it = v.find("web_code"); it != v.end() && web::CheckFixedCode(it->second, s.webCode)) s.webCode.clear();
    if (auto it = v.find("web_hosts"); it != v.end()) s.webHosts = it->second;
    if (auto it = v.find("output_device"); it != v.end()) s.outputDevice = it->second;
    s.pointsChip = ToInt(v, "points_chip", 0) != 0;
    s.lyricsPanel = ToInt(v, "lyrics_panel", 0) != 0;
    s.nttGuesses.clear();
    if (auto it = v.find("games_ntt_guesses"); it != v.end())
        for (const char* c = it->second.c_str(); *c;) {
            char* end = nullptr;
            long n = strtol(c, &end, 10);
            if (end == c) break;
            s.nttGuesses.push_back((int)std::max(0L, n));
            c = *end == ',' ? end + 1 : end;
        }
    s.nttStreak = ToInt(v, "games_ntt_streak", 0);
    s.nttBest = ToInt(v, "games_ntt_best", 0);
    s.wcfStreak = ToInt(v, "games_wcf_streak", 0);
    s.wcfBest = ToInt(v, "games_wcf_best", 0);
    if (auto it = v.find("packs_on"); it != v.end()) s.packsOn = it->second;
    if (auto it = v.find("ui_theme_pack"); it != v.end()) s.packThemeKey = it->second;
    if (auto it = v.find("helpers_on"); it != v.end()) s.helpersOn = it->second;
    for (const auto& [key, value] : v)
        if (key.starts_with("helper_trust.")) s.helperTrust[key.substr(13)] = value;
    s.hiddenParts = (unsigned)std::clamp(ToInt(v, "layout_hidden", 0), 0, 1023);
    s.timeRemaining = ToInt(v, "time_remaining", 1) != 0;
    s.liveWaveform = ToInt(v, "live_waveform", 0) != 0;
    s.liveFountain = ToInt(v, "live_fountain", 0) != 0;
    s.fullLiveWaveform = ToInt(v, "full_live_waveform", 1) != 0;
    s.bandWaveSize = std::clamp(ToInt(v, "band_wave_size", 0), 0, 5);
    s.alwaysOnTop = ToInt(v, "always_on_top", 0) != 0;
    for (auto [key, target] : {std::pair{"format_title", &s.fmtTitle}, std::pair{"format_subtitle", &s.fmtSub},
                               std::pair{"format_status", &s.fmtStatus}, std::pair{"format_window", &s.fmtWindow}})
        if (auto it = v.find(key); it != v.end() && !it->second.empty()) *target = compat::Widen(it->second.c_str());
    s.soundEffects = ToInt(v, "sound_effects", 0) != 0;
    s.roboticSounds = ToInt(v, "robotic_sounds", 1) != 0;
    for (int i = 0;; ++i) {
        auto it = v.find("folder" + std::to_string(i));
        if (it == v.end()) break;
        if (!it->second.empty()) s.folders.push_back(compat::Widen(it->second.c_str()));
    }
    s.inboxEnabled = ToInt(v, "inbox_enabled", 0) != 0;
    s.steamOffered = ToInt(v, "steam_offered", 0) != 0;
    s.volume = std::clamp(ToInt(v, "volume", 100), 0, 100) / 100.0f;
    s.shuffle = std::clamp(ToInt(v, "shuffle", 0), 0, kShuffleCount - 1);
    s.skipAwareShuffle = ToInt(v, "skip_aware_shuffle", 0) != 0;
    s.radio = ToInt(v, "radio", 0) != 0;
    s.resumeOn = ToInt(v, "resume_on", 1) != 0;
    if (auto it = v.find("resume_path"); it != v.end()) s.resumePath = compat::Widen(it->second.c_str());
    s.resumePosition = ToFloat(v, "resume_position", 0, 0, 1e9f);
    s.resumeIndex = ToInt(v, "resume_index", -1);
    for (const auto& [key, value] : v) {
        if (!key.starts_with("section_") || key.size() != 24 ||
            !std::all_of(key.begin() + 8, key.end(), [](unsigned char c) { return std::isxdigit(c) != 0; })) continue;
        size_t a = value.find(','), b = a == std::string::npos ? a : value.find(',', a + 1);
        if (b == std::string::npos) continue;
        auto integer = [](std::string_view text, int64_t& out) {
            auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
            return error == std::errc{} && end == text.data() + text.size();
        };
        int64_t start = 0, end = 0;
        std::string_view text(value), on = text.substr(b + 1);
        if (!integer(text.substr(0, a), start) || !integer(text.substr(a + 1, b - a - 1), end) ||
            start < 0 || start > 1000000000000LL || end <= start || end > 1000000000000LL ||
            end - start <= 50 || (on != "0" && on != "1")) continue;
        s.sections[FromUtf8(key.substr(8))] = {start / 1000.0, end / 1000.0, on == "1"};
    }
    s.exclusive = ToInt(v, "exclusive", 0) != 0;
    s.repeat = (Repeat)std::clamp(ToInt(v, "repeat", 0), 0, 2);
    // ui_theme counts in the shared ThemeMode; the older theme key only knew four of them.
    static const ThemeMode kOldThemes[] = {ThemeMode::System, ThemeMode::Light, ThemeMode::Dark, ThemeMode::Foobar};
    s.theme = AvailableTheme(v.count("ui_theme") ? (ThemeMode)std::clamp(ToInt(v, "ui_theme", 0), 0, (int)ThemeMode::Count - 1)
                                                 : kOldThemes[std::clamp(ToInt(v, "theme", 0), 0, 3)]);
    s.groupAlbums = ToInt(v, "group_albums", 1) != 0;
    s.convertRecycle = ToInt(v, "convert_recycle", 1) != 0;
    s.convertShowOriginals = ToInt(v, "convert_show_originals", 0) != 0;
    s.convertMemory = (uint8_t)std::clamp(ToInt(v, "convert_undo_days", 3), 0, (int)std::size(kConvertMemoryDays) - 1);
    if (auto it = v.find("organize_folder"); it != v.end()) s.organizeFolder = compat::Widen(it->second.c_str());
    if (auto it = v.find("organize_pattern"); it != v.end()) s.organizePattern = compat::Widen(it->second.c_str());
    if (auto it = v.find("copy_folder"); it != v.end()) s.copyFolder = compat::Widen(it->second.c_str());
    if (auto it = v.find("copy_pattern"); it != v.end()) s.copyPattern = compat::Widen(it->second.c_str());
    s.copyFormat = (uint8_t)std::clamp(ToInt(v, "copy_format", 0), 0, (int)CopyFormat::Count - 1);
    s.libraryMode = (LibraryMode)std::clamp(ToInt(v, "library_view", (int)LibraryMode::Albums), 0, (int)LibraryMode::Count - 1);
    s.browserVisible = ToInt(v, "library_browser", 1) != 0;
    // What the file holds is how cordinal opens: the approved startup defaults (a profile from before
    // the choice existed keeps its saved presentation as them).
    s.defaultTheme = s.theme;
    s.defaultLibraryMode = s.libraryMode;
    s.defaultBrowserVisible = s.browserVisible;
    int saving = ToInt(v, "preference_saving", 0);
    s.preferenceSaving = saving >= 0 && saving < (int)PreferenceSaving::Count ? (PreferenceSaving)saving : PreferenceSaving::Ask;
    s.preferenceSavingChosen = ToInt(v, "preference_saving_chosen", 0) != 0;
    s.suggestionLastReview = (uint64_t)std::max(0, ToInt(v, "suggestion_last_review", 0));
    if (auto it = v.find("browser_fields"); it != v.end()) {
        int f[3] = {-1, -1, -1};
        sscanf(it->second.c_str(), "%d,%d,%d", &f[0], &f[1], &f[2]);
        for (int i = 0; i < 3; ++i)
            if (f[i] >= 0 && f[i] < (int)BrowseField::Count) s.browserFields[i] = (uint8_t)f[i];
    }
    s.browserHeight = std::clamp(ToInt(v, "browser_height", 150), 60, 2000);
    s.coverSize = std::clamp(ToInt(v, "cover_size", 160), 80, 300);
    s.windowW = std::max(480, ToInt(v, "window_width", s.windowW));
    s.windowH = std::max(320, ToInt(v, "window_height", s.windowH));
    s.maximized = ToInt(v, "maximized", 0) != 0;
    s.activeTab = ToInt(v, "active_tab", 0);
    s.rg.on = ToInt(v, "rg_enabled", 1) != 0;
    s.rg.mode = (ReplayGainMode)std::clamp(ToInt(v, "rg_mode", 0), 0, 2);
    s.rg.preamp = ToFloat(v, "rg_preamp", 0, -24, 24);
    s.rg.preampNoInfo = ToFloat(v, "rg_preamp_noinfo", 0, -24, 24);
    s.rg.keep = ToFloat(v, "rg_keep", 0.4f, 0, 0.9f);
    s.rgAutoScan = ToInt(v, "rg_autoscan", 1) != 0;
    s.rgEstimate = ToInt(v, "rg_estimate", 1) != 0;
    LoadEq(v, s.eq);
    s.reverb.preset = (ReverbPreset)std::clamp(ToInt(v, "reverb", 0), 0, (int)ReverbPreset::Count - 1);
    s.reverb.mix = ToFloat(v, "reverb_amount", 0.4f, 0, 1);
    s.loudnessComp = ToInt(v, "loudness_compensation", 0) != 0;
    s.nightMode = ToInt(v, "night_mode", 0) != 0;
    s.loops = std::clamp(ToInt(v, "loops", 2), 0, 3);
    // "column:width,column:width" in ui::kColumns ids (the older "columns" key used ids of its own,
    // so it is left for the defaults). Column 0, the playing mark, always comes first.
    if (auto it = v.find("list_columns"); it != v.end() && !it->second.empty()) {
        std::vector<int> columns{0};
        for (size_t i = 0; i < it->second.size();) {
            size_t j = it->second.find(',', i);
            if (j == std::string::npos) j = it->second.size();
            std::string part = it->second.substr(i, j - i);
            int c = atoi(part.c_str()), w = 0;
            if (size_t colon = part.find(':'); colon != std::string::npos) w = atoi(part.c_str() + colon + 1);
            if (c >= 0 && c < ui::kColumnCount) {
                if (c && std::find(columns.begin(), columns.end(), c) == columns.end()) columns.push_back(c);
                s.widths[c] = std::clamp(w, 0, 2000);
            }
            i = j + 1;
        }
        if (columns.size() > 1) s.columns = columns;
    }
}

void SaveSettings() {
    const Settings& s = g.settings;
    std::string text = "# cordinal settings\n";
    text += "web_port=" + std::to_string(s.webPort) + "\nweb_start=" + std::to_string(s.webStart) + "\n";
    text += "web_away=" + std::to_string(s.webAway) + "\nweb_away_agreed=" + std::to_string(s.webAwayAgreed) +
            "\nweb_away_port=" + std::to_string(s.webAwayPort) + "\nweb_code=" + s.webCode + "\nweb_hosts=" + s.webHosts + "\n";
    for (size_t i = 0; i < s.folders.size(); ++i)
        text += "folder" + std::to_string(i) + "=" + compat::Narrow(s.folders[i].c_str()) + "\n";
    text += "inbox_enabled=" + std::to_string(s.inboxEnabled) + "\n";
    text += "steam_offered=" + std::to_string(s.steamOffered) + "\n";
    text += "volume=" + std::to_string((int)std::lround(s.volume * 100)) + "\n";
    text += "shuffle=" + std::to_string(s.shuffle) + "\nexclusive=" + std::to_string(s.exclusive) + "\n";
    text += "skip_aware_shuffle=" + std::to_string(s.skipAwareShuffle) + "\n";
    text += "radio=" + std::to_string(s.radio) + "\n";
    TrackId resume = s.resumeOn ? (g.playing != kNone ? g.playing : g.resumeTrack) : kNone;
    bool haveResume = g_lib.Contains(resume);
    double resumePosition = g.playing != kNone ? g.engine.Position() : g.resumePosition;
    int resumeIndex = g.playing != kNone ? g.queuePos : g.resumeIndex;
    text += "resume_on=" + std::to_string(s.resumeOn) + "\nresume_path=" +
            (haveResume ? compat::Narrow(g_lib.Path(resume).c_str()) : std::string()) +
            "\nresume_position=" + FloatText(haveResume && std::isfinite(resumePosition) ? (float)std::max(0.0, resumePosition) : 0) +
            "\nresume_index=" + std::to_string(haveResume ? resumeIndex : -1) + "\n";
    for (const auto& [key, section] : s.sections)
        text += "section_" + ToUtf8(key) + "=" + std::to_string((int64_t)std::llround(section.start * 1000)) + "," +
                std::to_string((int64_t)std::llround(section.end * 1000)) + "," + std::to_string(section.on) + "\n";
    text += "repeat=" + std::to_string((int)s.repeat) + "\nui_theme=" + std::to_string(PackThemeMode(s.defaultTheme) ? 0 : (int)s.defaultTheme) + "\n";
    text += "ui_theme_pack=" + PackThemeKey(s.defaultTheme) + "\n";
    text += "preference_saving=" + std::to_string((int)s.preferenceSaving) + "\npreference_saving_chosen=" + std::to_string(s.preferenceSavingChosen) +
            "\nsuggestion_last_review=" + std::to_string(s.suggestionLastReview) + "\n";
    text += "group_albums=" + std::to_string(s.groupAlbums) + "\n";
    text += "output_device=" + s.outputDevice + "\n";
    text += "layout_hidden=" + std::to_string(s.hiddenParts) + "\n";
    text += "packs_on=" + s.packsOn + "\nhelpers_on=" + s.helpersOn + "\n";
    for (const auto& [id, record] : s.helperTrust) text += "helper_trust." + id + "=" + record + "\n";
    text += "lyrics_panel=" + std::to_string(s.lyricsPanel) + "\n";
    text += "points_chip=" + std::to_string(s.pointsChip) + "\n";
    text += "games_ntt_guesses=";
    for (size_t i = 0; i < s.nttGuesses.size(); ++i) text += (i ? "," : "") + std::to_string(s.nttGuesses[i]);
    text += "\ngames_ntt_streak=" + std::to_string(s.nttStreak) + "\ngames_ntt_best=" + std::to_string(s.nttBest) +
            "\ngames_wcf_streak=" + std::to_string(s.wcfStreak) + "\ngames_wcf_best=" + std::to_string(s.wcfBest) + "\n";
    text += "time_remaining=" + std::to_string(s.timeRemaining) + "\nalways_on_top=" + std::to_string(s.alwaysOnTop) + "\n";
    text += "live_waveform=" + std::to_string(s.liveWaveform) + "\n";
    text += "live_fountain=" + std::to_string(s.liveFountain) + "\n";
    text += "full_live_waveform=" + std::to_string(s.fullLiveWaveform) + "\n";
    text += "band_wave_size=" + std::to_string(s.bandWaveSize) + "\n";
    text += "format_title=" + ToUtf8(s.fmtTitle) + "\nformat_subtitle=" + ToUtf8(s.fmtSub) + "\nformat_status=" + ToUtf8(s.fmtStatus) +
            "\nformat_window=" + ToUtf8(s.fmtWindow) + "\n";
    text += "sound_effects=" + std::to_string(s.soundEffects) + "\nrobotic_sounds=" + std::to_string(s.roboticSounds) + "\n";
    text += "convert_recycle=" + std::to_string(s.convertRecycle) + "\nconvert_show_originals=" + std::to_string(s.convertShowOriginals) +
            "\nconvert_undo_days=" + std::to_string(s.convertMemory) + "\n";
    text += "organize_folder=" + compat::Narrow(s.organizeFolder.c_str()) + "\norganize_pattern=" +
            compat::Narrow(s.organizePattern.c_str()) + "\n";
    text += "copy_folder=" + compat::Narrow(s.copyFolder.c_str()) + "\ncopy_pattern=" + compat::Narrow(s.copyPattern.c_str()) +
            "\ncopy_format=" + std::to_string(s.copyFormat) + "\n";
    text += "library_view=" + std::to_string((int)s.defaultLibraryMode) + "\nlibrary_browser=" + std::to_string(s.defaultBrowserVisible) +
            "\nbrowser_fields=" + std::to_string(s.browserFields[0]) + "," + std::to_string(s.browserFields[1]) + "," +
            std::to_string(s.browserFields[2]) + "\nbrowser_height=" + std::to_string(s.browserHeight) +
            "\ncover_size=" + std::to_string(s.coverSize) + "\n";
    text += "window_width=" + std::to_string(s.windowW) + "\nwindow_height=" + std::to_string(s.windowH) + "\n";
    text += "maximized=" + std::to_string(s.maximized) + "\nactive_tab=" + std::to_string(g.active) + "\n";
    text += "rg_enabled=" + std::to_string(s.rg.on) + "\nrg_mode=" + std::to_string((int)s.rg.mode) + "\nrg_preamp=" + FloatText(s.rg.preamp) +
            "\nrg_preamp_noinfo=" + FloatText(s.rg.preampNoInfo) + "\nrg_keep=" + FloatText(s.rg.keep) +
            "\nrg_autoscan=" + std::to_string(s.rgAutoScan) + "\nrg_estimate=" + std::to_string(s.rgEstimate) + "\n";
    text += EqText(s.eq);
    text += "reverb=" + std::to_string((int)s.reverb.preset) + "\nreverb_amount=" + FloatText(s.reverb.mix) +
            "\nloudness_compensation=" + std::to_string(s.loudnessComp) + "\nnight_mode=" + std::to_string(s.nightMode) +
            "\nloops=" + std::to_string(s.loops) + "\n";
    text += "list_columns=";
    for (size_t i = 0; i < s.columns.size(); ++i)
        text += (i ? "," : "") + std::to_string(s.columns[i]) + ":" + std::to_string(s.widths[s.columns[i]]);
    text += "\n";
    std::string path = compat::Narrow(g.settingsPath.c_str()), temp = path + ".tmp";
    if (FILE* f = fopen(temp.c_str(), "w")) {
        bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
        ok = fclose(f) == 0 && ok;
        if (ok) rename(temp.c_str(), path.c_str());
        else remove(temp.c_str());
    }
}

void LoadLibraryFile() {
    bool loaded = LoadLibrary(g.libraryPath, g.playlists);
    if (!loaded && GetFileAttributesW((g.libraryPath + L".bak").c_str()) != INVALID_FILE_ATTRIBUTES) {
        g_lib = Library();
        g.playlists.clear();
        loaded = LoadLibrary(g.libraryPath + L".bak", g.playlists);
    }
    if (!loaded) {
        g_lib = Library();
        g.playlists.clear();
        EnsureSpecialPlaylists();
        for (auto& [name, a] : DefaultAutoPlaylists()) {
            Playlist p{name, {}, PlaylistKind::Auto};
            p.rules = SerializeAutoPlaylist(a);
            g.playlists.push_back(std::move(p));
        }
        g.libraryDirty = true;
    }
    EnsureSpecialPlaylists();
    if (g.settings.resumeOn && !g.settings.resumePath.empty()) {
        TrackId id = g_lib.Find(g.settings.resumePath);
        if (g_lib.Contains(id)) {
            g.resumeTrack = id;
            g.resumePosition = g.settings.resumePosition;
            g.resumeIndex = g.settings.resumeIndex;
        }
    }
    if (!PointsInitialized()) {
        GrantBackPayIfNeeded();
        g.libraryDirty = true;
    }
    std::vector<TrackId> unloaded;
    for (TrackId id = 0; id < (TrackId)g_lib.Count(); ++id)
        if (!g_lib[id].Loaded() && (g_lib[id].InLibrary() ||
            (g_lib[id].flags & (Track::kInbox | Track::kMissing)) == Track::kInbox)) unloaded.push_back(id);
    QueueTags(unloaded);
}

void SaveLibraryFile(bool force) {
    if (!force && !g.libraryDirty) return;
    if (SaveLibrary(g.libraryPath, g.playlists)) g.libraryDirty = false;
    g.lastSave = SDL_GetTicks();
}

}  // namespace lx
