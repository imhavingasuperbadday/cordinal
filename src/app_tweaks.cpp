// The Tweaks registry (app_tweaks.h) and the Preferences window built from it (File ▸ Preferences,
// Ctrl+,): a page per area of the registry plus All settings, one search across every page, and each
// setting with its help, its default and a reset. A change applies at once and goes through the same
// menu command the setting already has, so the window and the menus can never disagree about what a
// change does. Pages also link to the windows that hold the rest of their area's settings.
#include "app.h"

namespace app {

namespace {

const wchar_t* const kRgModes[] = {L"Album", L"Track", L"By playback order"};
const wchar_t* const kOffOn[] = {L"Off", L"On"};
const wchar_t* const kLoopNames[] = {L"Ignore loop points", L"Play the loop once, then fade",
                                     L"Play the loop twice, then fade", L"Play the loop three times, then fade"};
const wchar_t* const kReverbNames[] = {L"Off", L"Room", L"Hall", L"Plate"};
const wchar_t* const kUndoNames[] = {L"1 day", L"3 days", L"1 week", L"7 weeks"};
const wchar_t* const kSavingNames[] = {L"Ask before saving", L"Save automatically", L"Manual only (don't ask)"};
const wchar_t* const kNewMusicNames[] = {L"Add automatically", L"Review in Inbox"};
const wchar_t* const kBandWaveSizes[] = {L"Use layout size", L"Slim (26 px)", L"Medium (44 px)", L"Tall (72 px)",
                                       L"Extra tall (108 px)", L"Largest (144 px)"};
static_assert(std::size(kBandWaveSizes) == std::size(ui::kBandWaveHeights));
static_assert(std::size(kLoopNames) == 4 && std::size(kReverbNames) == (size_t)ReverbPreset::Count &&
              std::size(kUndoNames) == std::size(kConvertMemoryDays) &&
              std::size(kSavingNames) == (size_t)PreferenceSaving::Count);

// Grouped as the window shows them. Keys are permanent: renaming one would orphan saved values.
const Tweak kTweaks[] = {
    {"playback.resume", L"Player", L"ResumeOn", L"Resume where you left off",
     L"When cordinal starts, offer to continue the track that was playing when it closed.", TweakType::Bool, 0, 1, 1,
     [] { return (double)g.resumeOn; }, [](double v) { g.resumeOn = v != 0; }, IDM_RESUME},
    {"playback.time_remaining", L"Player", L"TimeRemaining", L"Show the time remaining",
     L"The time beside the seek bar counts down to the end of the track instead of up from its start.",
     TweakType::Bool, 0, 1, 1, [] { return (double)g.timeRemaining; }, [](double v) { g.timeRemaining = v != 0; }, 0,
     UpdateStatus},
    {"playback.skip_aware_shuffle", L"Player", L"SkipAwareShuffle", L"Skip-aware shuffle",
     L"Shuffle favors tracks you haven't repeatedly skipped in their first 30 seconds.", TweakType::Bool, 0, 1, 0,
     [] { return (double)g.skipAwareShuffle; }, [](double v) { g.skipAwareShuffle = v != 0; }, IDM_SKIP_SHUFFLE},
    {"playback.radio", L"Player", L"Radio", L"Keep playing like this",
     L"Keeps choosing from the playing playlist, favoring what you tend to play after the current track.",
     TweakType::Bool, 0, 1, 0, [] { return (double)g.radio; }, [](double v) { g.radio = v != 0; }, IDM_RADIO},
    {"playback.game_loops", L"Player", L"Loops", L"Game music loops",
     L"How often a game soundtrack rip with loop points plays its loop before fading out.", TweakType::Choice, 0, 3, 2,
     [] { return (double)g.loops; }, [](double v) { g.loops = (int)v; }, IDM_LOOPS_BASE, nullptr, kLoopNames,
     nullptr, kTweakRadio},
    {"output.exclusive", L"Player", L"Exclusive", L"Exclusive output",
     L"Each track plays at its own rate, bit-perfect at 100% volume, and other programs can't play sound meanwhile.",
     TweakType::Bool, 0, 1, 0, [] { return (double)g.exclusive; }, [](double v) { g.exclusive = v != 0; },
     IDM_OUTPUT_SHARED, nullptr, nullptr, nullptr, kTweakRadio},
    {"replaygain.enabled", L"ReplayGain", L"Enabled", L"ReplayGain",
     L"Plays every track at a similar loudness, using measured or tagged ReplayGain values.", TweakType::Bool, 0, 1, 1,
     [] { return (double)g.rgOn; }, [](double v) { g.rgOn = v != 0; }, IDM_RG_ENABLE, nullptr, nullptr, nullptr,
     kTweakLegacyLoad},
    {"replaygain.mode", L"ReplayGain", L"SourceMode", L"Level by",
     L"Album preserves album relationships; track levels songs individually. Playback order uses track for track shuffle.",
     TweakType::Choice, 0, 2, 0, [] { return (double)g.rgMode; },
     [](double v) { g.rgMode = (ReplayGainMode)(int)v; }, 0, [] { RefreshGains(); }, kRgModes},
    {"replaygain.keep_loud", L"ReplayGain", L"KeepLoud", L"Loud songs keep their loudness",
     L"0 evens every track out; higher values turn loud songs down only part of the way.", TweakType::Float, 0, 0.9f,
     0.4f, [] { return (double)g.rgKeep; }, [](double v) { g.rgKeep = (float)v; }, 0, [] { RefreshGains(); }},
    {"replaygain.preamp", L"ReplayGain", L"Preamp", L"Preamp",
     L"Added to every track's ReplayGain.", TweakType::Float, -24, 24, 0, [] { return (double)g.rgPreamp; },
     [](double v) { g.rgPreamp = (float)v; }, 0, [] { RefreshGains(); }, nullptr, L"dB"},
    {"replaygain.preamp_without_info", L"ReplayGain", L"PreampWithoutInfo", L"Gain for unmeasured tracks",
     L"Used for tracks with no ReplayGain yet, limited so their peaks don't clip.", TweakType::Float, -24, 24, 0,
     [] { return (double)g.rgPreampNoInfo; }, [](double v) { g.rgPreampNoInfo = (float)v; }, 0,
     [] { RefreshGains(); }, nullptr, L"dB"},
    {"replaygain.auto_measure", L"ReplayGain", L"AutoMeasure", L"Measure new tracks automatically",
     L"Measures the loudness of new tracks in the background, giving way to playback and to your own scans.",
     TweakType::Bool, 0, 1, 1, [] { return (double)g.rgAutoScan; }, [](double v) { g.rgAutoScan = v != 0; },
     IDM_RG_AUTO_SCAN},
    {"replaygain.estimate", L"ReplayGain", L"EstimateUnmeasured", L"Estimate unmeasured tracks as they start",
     L"Guesses the loudness of a track with no ReplayGain from its first moments while it plays.", TweakType::Bool, 0,
     1, 1, [] { return (double)g.rgEstimate; }, [](double v) { g.rgEstimate = v != 0; }, IDM_RG_ESTIMATE},
    {"effects.reverb", L"Effects", L"Reverb", L"Reverb", L"A room around the music.", TweakType::Choice, 0, 3, 0,
     [] { return (double)g.reverb.preset; }, [](double v) { g.reverb.preset = (ReverbPreset)(int)v; },
     IDM_REVERB_BASE, nullptr, kReverbNames, nullptr, kTweakRadio},
    {"effects.reverb_amount", L"Effects", L"ReverbAmount", L"Reverb amount",
     L"How much of the room is mixed in, from 0 (none) to 1 (all of it).", TweakType::Float, 0, 1, 0.4f,
     [] { return (double)g.reverb.mix; }, [](double v) { g.reverb.mix = (float)v; }, 0, ApplyEffects},
    {"effects.loudness_compensation", L"Effects", L"LoudnessCompensation", L"Loudness compensation at low volume",
     L"Adds bass and treble as the volume goes down, so quiet listening doesn't sound thin.", TweakType::Bool, 0, 1, 0,
     [] { return (double)g.loudnessComp; }, [](double v) { g.loudnessComp = v != 0; }, IDM_LOUDNESS_COMP},
    {"effects.night_mode", L"Effects", L"NightMode", L"Night mode",
     L"Slowly lifts quiet passages and softens loud ones, for listening at night.", TweakType::Bool, 0, 1, 0,
     [] { return (double)g.nightMode; }, [](double v) { g.nightMode = v != 0; }, IDM_NIGHT_MODE},
    {"view.live_waveform", L"Window", L"LiveWaveform", L"Live waveform for songs",
     L"Show a moving waveform of the audio being heard instead of the whole-song overview. Seeking still works. "
     L"Radio always uses the live waveform.", TweakType::Bool, 0, 1, 0,
     [] { return (double)g.liveWaveform; }, [](double v) { g.liveWaveform = v != 0; }, 0,
     [] { WaveformTrackChanged(true); WaveformTick(); }},
    {"view.live_fountain", L"Window", L"LiveFountain", L"Live audio for Dark Fountain",
     L"In the DELTARUNE theme, the selected fountain style follows the audio being heard, including Radio. "
     L"Song seeking still works.", TweakType::Bool, 0, 1, 0,
     [] { return (double)g.liveFountain; }, [](double v) { g.liveFountain = v != 0; }, 0,
     [] { WaveformTrackChanged(true); WaveformTick(); }},
    {"view.full_live_waveform", L"Window", L"FullLiveWaveform", L"Full color for live waveforms",
     L"Light the whole live waveform. Turn off to show song progress with played and "
     L"unplayed colors.", TweakType::Bool, 0, 1, 1,
     [] { return (double)g.fullLiveWaveform; }, [](double v) { g.fullLiveWaveform = v != 0; }, 0,
     [] { WaveformTick(); if (g.seek) InvalidateRect(g.seek, nullptr, FALSE); }},
    {"view.band_wave_size", L"Window", L"BandWaveSize", L"Top/bottom waveform size",
     L"Sets the waveform's height only in the normal top or bottom player bar. "
     L"Big, compact and side players keep their own sizes.", TweakType::Choice, 0, 5, 0,
     [] { return (double)g.bandWaveSize; }, [](double v) { g.bandWaveSize = (int)v; }, 0,
     [] { Layout(); WaveformTrackChanged(true); WaveformTick(); }, kBandWaveSizes},
    {"view.group_albums", L"Window", L"GroupAlbums", L"Group tracks by album",
     L"The track list shows a header with the cover above each album's tracks.", TweakType::Bool, 0, 1, 1,
     [] { return (double)g.groupAlbums; }, [](double v) { g.groupAlbums = v != 0; }, IDM_GROUP_ALBUMS},
    {"view.taskbar_progress", L"Window", L"TaskbarProgress", L"Show progress on the taskbar",
     L"The taskbar button fills as the track plays.", TweakType::Bool, 0, 1, 1,
     [] { return (double)g.taskbarProgress; }, [](double v) { g.taskbarProgress = v != 0; }, IDM_TASKBAR_PROGRESS},
    {"view.focus_boxes", L"Window", L"ShowFocusBoxes", L"Show focus boxes",
     L"Show outlines around the focused control for keyboard navigation.", TweakType::Bool, 0, 1, 0,
     [] { return (double)g.showFocusBoxes; }, [](double v) { g.showFocusBoxes = v != 0; }, 0, [] {
         if (g.hwnd) {
             SendMessageW(g.hwnd, WM_CHANGEUISTATE,
                          MAKEWPARAM(g.showFocusBoxes ? UIS_CLEAR : UIS_SET, UISF_HIDEFOCUS), 0);
             RedrawWindow(g.hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
         }
         if (g.eqWnd) RedrawWindow(g.eqWnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
     }},
    {"view.always_on_top", L"Window", L"AlwaysOnTop", L"Always on top",
     L"Keeps cordinal's window above other windows.", TweakType::Bool, 0, 1, 0, [] { return (double)g.alwaysOnTop; },
     [](double v) { g.alwaysOnTop = v != 0; }, IDM_ALWAYS_ON_TOP},
    {"view.sound_effects", L"Player", L"SoundEffects", L"Sound effects",
     L"Small, quiet sounds for things that otherwise happen silently, such as saving tags or dropping files.",
     TweakType::Bool, 0, 1, 0, [] { return (double)g.soundEffects; }, [](double v) { g.soundEffects = v != 0; },
     IDM_SOUND_EFFECTS},
    {"view.robotic_sounds", L"Window", L"RoboticSounds", L"Robotic sounds in the DELTARUNE theme",
     L"The theme's sounds come bitcrushed, in Chapter 2's Cyber World voice, and messages are spoken in Queen's blip.",
     TweakType::Bool, 0, 1, 1, [] { return (double)RoboticSounds(); }, [](double v) { SetRoboticSounds(v != 0); },
     IDM_THEME_ROBOTIC},
    {"startup.with_windows", nullptr, nullptr, L"Start with Windows", L"Opens cordinal when you sign in to Windows.",
     TweakType::Bool, 0, 1, 0, [] { return (double)StartWithWindows(); }, [](double) {}, IDM_START_WITH_WINDOWS,
     nullptr, nullptr, nullptr, kTweakOutside},
    {"startup.saving", L"Suggestions", L"SavingMode", L"Preference saving",
     L"Whether a new window size, theme, library view, browser or layout becomes how cordinal opens: asked about once "
     L"it settles, saved as you go, or only through Startup defaults.", TweakType::Choice, 0, 2, 0,
     [] { return (double)g.preferenceSaving; }, [](double v) { g.preferenceSaving = (PreferenceSaving)(int)v; },
     IDM_PREFERENCE_ASK, nullptr, kSavingNames, nullptr, kTweakRadio | kTweakLegacyLoad},
    {"convert.recycle", L"Convert", L"Recycle", L"Send converted originals to the Recycle Bin",
     L"After converting to .cord, the original files go to the Recycle Bin instead of staying where they are.",
     TweakType::Bool, 0, 1, 1, [] { return (double)g.convertRecycle; }, [](double v) { g.convertRecycle = v != 0; },
     IDM_CONVERT_RECYCLE},
    {"convert.show_originals", L"Convert", L"ShowOriginals", L"Show kept originals in the library",
     L"Originals kept after a conversion appear in the library beside their .cord copies.", TweakType::Bool, 0, 1, 0,
     [] { return (double)g.convertShowOriginals; }, [](double v) { g.convertShowOriginals = v != 0; },
     IDM_CONVERT_SHOW_ORIGINALS},
    {"convert.undo_days", L"Convert", L"Memory", L"Conversions can be undone for",
     L"How long cordinal remembers a conversion so it can put the originals back.", TweakType::Choice, 0, 3, 3,
     [] { return (double)g.convertMemory; }, [](double v) { g.convertMemory = (uint8_t)v; }, IDM_CONVERT_MEMORY_BASE,
     nullptr, kUndoNames, nullptr, kTweakRadio},
    {"library.new_music", L"Library", L"Inbox", L"New music from library folders",
     L"Add music automatically or send it to the Inbox; existing Inbox tracks stay.",
     TweakType::Choice, 0, 1, 0, [] { return (double)g.inboxEnabled; }, [](double v) { g.inboxEnabled = v != 0; },
     0, [] { ShowMessage(g.inboxEnabled ? L"New music will wait in the Inbox." : L"New music will be added to the library."); },
     kNewMusicNames},
    {"library.explorer_menu", L"Explorer", L"FolderMenu", L"Play folders from Explorer",
     L"Right-clicking a folder offers Play with cordinal and Add to cordinal; a folder plays its tracks like a "
     L"command line does.", TweakType::Bool, 0, 1, 1, [] { return (double)g.explorerMenu; },
     [](double v) { g.explorerMenu = v != 0; }, 0, ApplyExplorerMenu},
    {"library.web_start", L"Web", L"Start", L"Start the web server with cordinal",
     L"Turns on the web server whenever cordinal starts, so a paired phone or browser can play your library.",
     TweakType::Bool, 0, 1, 0, [] { return (double)WebStartsWithCordinal(); },
     [](double v) { SetWebStartsWithCordinal(v != 0); }, 0, ApplyWebStart},
};

// The window's pages: All settings, then one per key prefix in registry order. A page may also point
// to the windows that hold the rest of its area's settings (Library links to its own windows).
struct Link {
    int command;
    const wchar_t* label;
};
struct Page {
    const char* prefix;  // null: every setting
    const wchar_t* name;
    Link links[3];
};
const Page kPages[] = {
    {nullptr, L"All settings", {}},
    {"playback.", L"Playback", {}},
    {"output.", L"Output", {{IDM_HEADPHONES, L"&Headphones and devices..."}}},
    {"replaygain.", L"ReplayGain", {}},
    {"effects.", L"Effects", {{IDM_EQUALIZER, L"&Equalizer..."}}},
    {"view.", L"View", {{IDM_FORMAT, L"Title &formatting..."}}},
    {"startup.", L"Startup", {{IDM_STARTUP_DEFAULTS, L"&Startup defaults..."}}},
    {"convert.", L"Conversion", {}},
    {"library.", L"Library",
     {{IDM_LIBRARY_FOLDERS, L"Library &folders..."}, {IDM_WEB_SERVER, L"&Web server..."},
      {IDM_LISTENBRAINZ, L"&ListenBrainz scrobbling..."}}},
};
constexpr int kLinks = (int)std::size(kPages[0].links);

bool OnPage(const Tweak& t, int page) {
    const char* prefix = kPages[page].prefix;
    return !prefix || !strncmp(t.key, prefix, strlen(prefix));
}

int PageOf(const Tweak& t) {
    for (int i = 1; i < (int)std::size(kPages); ++i)
        if (OnPage(t, i)) return i;
    return 0;
}

std::wstring KeyText(const Tweak& t) { return std::wstring(t.key, t.key + strlen(t.key)); }

constexpr int kIdSearch = 100, kIdFilter = 101, kIdList = 102, kIdChoice = 103, kIdNumber = 104, kIdReset = 105,
              kIdResetAll = 106, kIdPages = 107, kIdLink0 = 110;

struct PreferencesWindow {
    HWND hwnd = nullptr, pages = nullptr, search = nullptr, filter = nullptr, list = nullptr, title = nullptr;
    HWND help = nullptr, choice = nullptr, number = nullptr, reset = nullptr, resetAll = nullptr, close = nullptr;
    HWND links[kLinks] = {};
    std::vector<int> rows;  // kTweaks indices shown, in order
    int page = 0;           // kPages index
    int current = -1;       // the kTweaks index being edited
    bool filling = false;   // the editor is being filled in, so its notifications are not changes
} w;

bool Searching() { return GetWindowTextLengthW(w.search) > 0; }

// A page with only links shows them in place of an empty list.
bool LinksOnly() {
    if (Searching()) return false;
    for (const Tweak& t : kTweaks)
        if (TweakShown(t) && OnPage(t, w.page)) return false;
    return true;
}

void LayoutPreferences();

void ShowEditor() {
    const bool any = w.current >= 0;
    const Tweak* t = any ? &kTweaks[w.current] : nullptr;
    const bool number = any && t->type == TweakType::Float;
    w.filling = true;
    std::wstring title = any ? std::wstring(t->label)
                       : Searching() ? L"No setting matches the search."
                       : LinksOnly() ? L"These settings have windows of their own."
                                     : L"Nothing has been changed here.";
    SetTextIfChanged(w.title, title);
    std::wstring help;
    if (any) help = std::wstring(t->help) + L"\nDefault: " + TweakText(*t, t->def) + L".  Key: " + KeyText(*t);
    SetTextIfChanged(w.help, help);
    ShowWindow(w.choice, any && !number ? SW_SHOWNA : SW_HIDE);
    ShowWindow(w.number, number ? SW_SHOWNA : SW_HIDE);
    ShowWindow(w.reset, any ? SW_SHOWNA : SW_HIDE);
    if (any && !number) {
        SendMessageW(w.choice, CB_RESETCONTENT, 0, 0);
        for (int i = 0; i <= (int)t->max; ++i)
            SendMessageW(w.choice, CB_ADDSTRING, 0, (LPARAM)(t->choices ? t->choices[i] : kOffOn[i]));
        SendMessageW(w.choice, CB_SETCURSEL, (WPARAM)(int)t->get(), 0);
    } else if (number) {
        wchar_t text[32];
        swprintf_s(text, L"%.2f", t->get());
        if (GetFocus() != w.number) SetTextIfChanged(w.number, text);
    }
    EnableWindow(w.reset, any && TweakChanged(*t));
    // The bottom reset covers what the list shows: everything, this page, or the search results.
    bool changed = false;
    for (int i : w.rows) changed = changed || TweakChanged(kTweaks[i]);
    SetTextIfChanged(w.resetAll, Searching() ? L"&Reset these to defaults" : w.page ? L"&Reset page to defaults"
                                                                               : L"&Reset all to defaults");
    EnableWindow(w.resetAll, changed);
    ShowWindow(w.list, LinksOnly() ? SW_HIDE : SW_SHOWNA);
    ShowWindow(w.resetAll, LinksOnly() ? SW_HIDE : SW_SHOWNA);
    for (int i = 0; i < kLinks; ++i) {
        const Link& link = kPages[w.page].links[i];
        if (link.label) SetTextIfChanged(w.links[i], link.label);
        ShowWindow(w.links[i], link.label && !Searching() ? SW_SHOWNA : SW_HIDE);
    }
    LayoutPreferences();
    w.filling = false;
}

// Values may have changed from the menus or the palette while the window was open.
void RefreshValues() {
    if (!w.hwnd) return;
    InvalidateRect(w.list, nullptr, FALSE);
    InvalidateRect(w.pages, nullptr, FALSE);  // the changed counts
    ShowEditor();
}

void Rebuild(int keep) {
    std::wstring query = WindowText(w.search);
    const bool changedOnly = SendMessageW(w.filter, CB_GETCURSEL, 0, 0) == 1;
    // A search covers every page, so the page list says so.
    if (!query.empty() && w.page != 0) {
        w.page = 0;
        SendMessageW(w.pages, LB_SETCURSEL, 0, 0);
    }
    std::vector<std::wstring> texts;
    for (const Tweak& t : kTweaks)
        texts.push_back(std::wstring(t.label) + L" " + kPages[PageOf(t)].name + L" " + KeyText(t) + L" " + t.help);
    w.rows.clear();
    for (int i : RankMatches(texts, query))
        if (TweakShown(kTweaks[i]) && OnPage(kTweaks[i], w.page) && (!changedOnly || TweakChanged(kTweaks[i]))) w.rows.push_back(i);
    ListView_SetItemCountEx(w.list, (int)w.rows.size(), 0);
    auto it = std::find(w.rows.begin(), w.rows.end(), keep);
    int row = it != w.rows.end() ? (int)(it - w.rows.begin()) : w.rows.empty() ? -1 : 0;
    w.current = row < 0 ? -1 : w.rows[row];
    if (row >= 0) {
        ListView_SetItemState(w.list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(w.list, row, FALSE);
    }
    InvalidateRect(w.list, nullptr, FALSE);
    ShowEditor();
}

void CommitNumber() {
    if (w.current < 0 || kTweaks[w.current].type != TweakType::Float) return;
    std::wstring text = WindowText(w.number);
    wchar_t* end = nullptr;
    double v = wcstod(text.c_str(), &end);
    if (end != text.c_str()) SetTweak(kTweaks[w.current], v);
    wchar_t shown[32];
    swprintf_s(shown, L"%.2f", kTweaks[w.current].get());
    SetWindowTextW(w.number, shown);  // a value out of range shows where it was clamped to
    RefreshValues();
}

void LayoutPreferences() {
    RECT rc;
    GetClientRect(w.hwnd, &rc);
    const int pad = S(14), navW = S(160), buttonH = S(26), bottom = rc.bottom - pad - buttonH;
    MoveWindow(w.pages, pad, pad, navW, std::max(bottom - S(12) - pad, S(80)), TRUE);
    int x = pad + navW + S(14), width = rc.right - x - pad, y = pad, filterW = S(170);
    MoveWindow(w.search, x, y, width - filterW - S(8), S(24), TRUE);
    MoveWindow(w.filter, x + width - filterW, y, filterW, S(200), TRUE);
    y += S(34);
    const bool linked = IsWindowVisible(w.links[0]) != 0;
    if (w.hwnd && !LinksOnly()) {
        int listH = std::max(bottom - y - S(178) - (linked ? S(36) : 0), S(80));
        MoveWindow(w.list, x, y, width, listH, TRUE);
        RECT client;
        GetClientRect(w.list, &client);  // inside the border and the scroll bar, so no horizontal scrolling
        int listW = client.right - GetSystemMetricsForDpi(SM_CXVSCROLL, g.dpi);
        ListView_SetColumnWidth(w.list, 0, std::max(listW - S(300), S(160)));
        y += listH + S(12);
    }
    MoveWindow(w.title, x, y, width, S(20), TRUE);
    y += S(22);
    MoveWindow(w.help, x, y, width, S(40), TRUE);
    y += S(46);
    // Default sits just after whichever editor is showing.
    const bool number = w.current >= 0 && kTweaks[w.current].type == TweakType::Float;
    int resetW = S(86), editW = std::min(width - resetW - S(8), S(320)), numberW = S(120);
    MoveWindow(w.choice, x, y, editW, S(240), TRUE);
    MoveWindow(w.number, x, y, numberW, buttonH, TRUE);
    MoveWindow(w.reset, x + (number ? numberW : editW) + S(8), y, resetW, buttonH, TRUE);
    y = w.hwnd && LinksOnly() ? pad + S(34) + S(30) : y + S(40);  // under the title, with no setting to edit
    for (int i = 0, lx = x; i < kLinks; ++i) {
        int linkW = S(190);
        MoveWindow(w.links[i], lx, y, linkW, buttonH, TRUE);
        lx += linkW + S(8);
    }
    MoveWindow(w.resetAll, x, bottom, S(170), buttonH, TRUE);
    MoveWindow(w.close, rc.right - pad - S(86), bottom, S(86), buttonH, TRUE);
}

void DrawPage(const DRAWITEMSTRUCT* di) {
    if (di->itemID == (UINT)-1) return;
    const bool selected = di->itemState & ODS_SELECTED;
    RECT r = di->rcItem;
    FillSolid(di->hDC, r, Ref(selected ? P().select : P().surface));
    SetBkMode(di->hDC, TRANSPARENT);
    HGDIOBJ old = SelectObject(di->hDC, selected ? g.fontBold : g.font);
    r.left += S(10);
    r.right -= S(8);
    int changed = 0;
    for (const Tweak& t : kTweaks) changed += TweakShown(t) && OnPage(t, (int)di->itemID) && TweakChanged(t);
    if (changed) {  // how many settings here differ from their defaults
        SetTextColor(di->hDC, Ref(selected ? P().selectText : P().sub));
        std::wstring count = std::to_wstring(changed);
        DrawTextW(di->hDC, count.c_str(), -1, &r, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
    }
    SetTextColor(di->hDC, Ref(selected ? P().selectText : P().text));
    DrawTextW(di->hDC, kPages[di->itemID].name, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(di->hDC, old);
}

LRESULT CALLBACK PreferencesProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            w.hwnd = h;
            auto make = [&](DWORD ex, const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
                HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, h, (HMENU)(INT_PTR)id,
                                         g.inst, nullptr);
                SendMessageW(c, WM_SETFONT, (WPARAM)g.font, TRUE);
                return c;
            };
            w.pages = make(0, WC_LISTBOXW, L"Pages", LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS |
                           LBS_NOINTEGRALHEIGHT | WS_TABSTOP, kIdPages);
            for (const Page& page : kPages) SendMessageW(w.pages, LB_ADDSTRING, 0, (LPARAM)page.name);
            SendMessageW(w.pages, LB_SETCURSEL, 0, 0);
            w.search = make(WS_EX_CLIENTEDGE, WC_EDITW, L"", ES_AUTOHSCROLL | WS_TABSTOP, kIdSearch);
            SendMessageW(w.search, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search every page");
            w.filter = make(0, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, kIdFilter);
            SendMessageW(w.filter, CB_ADDSTRING, 0, (LPARAM)L"All settings");
            SendMessageW(w.filter, CB_ADDSTRING, 0, (LPARAM)L"Changed from default");
            SendMessageW(w.filter, CB_SETCURSEL, 0, 0);
            w.list = make(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                          LVS_REPORT | LVS_OWNERDATA | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER | WS_TABSTOP,
                          kIdList);
            ListView_SetExtendedListViewStyle(w.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
            const struct { const wchar_t* name; int width; } cols[] = {{L"Setting", 260}, {L"Value", 200}, {L"Page", 100}};
            for (int i = 0; i < 3; ++i) {
                LVCOLUMNW c{};
                c.mask = LVCF_TEXT | LVCF_WIDTH;
                c.pszText = const_cast<wchar_t*>(cols[i].name);
                c.cx = S(cols[i].width);
                ListView_InsertColumn(w.list, i, &c);
            }
            w.title = make(0, WC_STATICW, L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, 0);
            SendMessageW(w.title, WM_SETFONT, (WPARAM)g.fontBold, TRUE);
            w.help = make(0, WC_STATICW, L"", SS_LEFT | SS_NOPREFIX, 0);
            w.choice = make(0, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, kIdChoice);
            w.number = make(WS_EX_CLIENTEDGE, WC_EDITW, L"", ES_AUTOHSCROLL | WS_TABSTOP, kIdNumber);
            w.reset = make(0, WC_BUTTONW, L"&Default", BS_PUSHBUTTON | WS_TABSTOP, kIdReset);
            for (int i = 0; i < kLinks; ++i)
                w.links[i] = make(0, WC_BUTTONW, L"", BS_PUSHBUTTON | WS_TABSTOP, kIdLink0 + i);
            w.resetAll = make(0, WC_BUTTONW, L"&Reset all to defaults", BS_PUSHBUTTON | WS_TABSTOP, kIdResetAll);
            w.close = make(0, WC_BUTTONW, L"Close", BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL);
            for (HWND c : {w.search, w.filter, w.choice, w.number})
                SetWindowTheme(c, g.dark ? L"DarkMode_CFD" : L"Explorer", nullptr);
            for (HWND c : {w.reset, w.resetAll, w.close, w.list, w.pages}) ApplyControlTheme(c, g.dark);
            for (HWND c : w.links) ApplyControlTheme(c, g.dark);
            ListView_SetBkColor(w.list, WindowBg());
            ListView_SetTextBkColor(w.list, WindowBg());
            ListView_SetTextColor(w.list, TextColor());
            SetWindowSubclass(ListView_GetHeader(w.list), HeaderSubclass, 0, 0);  // the track list's flat header
            ApplyWindowTheme(h, P());
            LayoutPreferences();  // now, not at the first WM_SIZE: the list must have its height before it scrolls
            return 0;
        }
        case WM_SIZE:
            LayoutPreferences();
            return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {S(700), S(520)};
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) != WA_INACTIVE) RefreshValues();
            break;
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(h, &rc);
            FillRect((HDC)wp, &rc, g.surfaceBrush);
            return 1;
        }
        case WM_MEASUREITEM:
            reinterpret_cast<MEASUREITEMSTRUCT*>(lp)->itemHeight = S(30);
            return TRUE;
        case WM_DRAWITEM:
            DrawPage(reinterpret_cast<DRAWITEMSTRUCT*>(lp));
            return TRUE;
        case WM_CTLCOLORSTATIC:
            return SurfaceControlColor((HDC)wp, (HWND)lp == w.help ? SubColor() : TextColor());
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            if ((HWND)lp == w.pages) return SurfaceControlColor((HDC)wp, TextColor());
            return FieldControlColor((HDC)wp);
        case WM_NOTIFY: {
            auto* hdr = reinterpret_cast<NMHDR*>(lp);
            if (hdr->hwndFrom != w.list) break;
            if (hdr->code == NM_CUSTOMDRAW) {  // palette selection, as the column browser draws it
                auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(hdr);
                if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT || cd->nmcd.dwDrawStage == (CDDS_SUBITEM | CDDS_ITEMPREPAINT)) {
                    bool selected = ListView_GetItemState(w.list, (int)cd->nmcd.dwItemSpec, LVIS_SELECTED) != 0;
                    cd->nmcd.uItemState &= ~(CDIS_SELECTED | CDIS_FOCUS | CDIS_HOT);
                    cd->clrTextBk = selected ? Ref(P().select) : WindowBg();
                    cd->clrText = selected ? Ref(P().selectText) : cd->iSubItem == 2 ? SubColor() : TextColor();
                    return cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT ? CDRF_NOTIFYSUBITEMDRAW : CDRF_DODEFAULT;
                }
                return CDRF_DODEFAULT;
            }
            if (hdr->code == LVN_GETDISPINFOW) {
                auto* di = reinterpret_cast<NMLVDISPINFOW*>(hdr);
                if (!(di->item.mask & LVIF_TEXT) || di->item.iItem >= (int)w.rows.size()) return 0;
                const Tweak& t = kTweaks[w.rows[di->item.iItem]];
                std::wstring text = di->item.iSubItem == 0 ? std::wstring(t.label)
                                  : di->item.iSubItem == 1 ? TweakText(t, t.get()) + (TweakChanged(t) ? L"  (changed)" : L"")
                                                           : std::wstring(kPages[PageOf(t)].name);
                wcsncpy_s(di->item.pszText, di->item.cchTextMax, text.c_str(), _TRUNCATE);
                return 0;
            }
            if (hdr->code == LVN_ITEMCHANGED) {
                auto* change = reinterpret_cast<NMLISTVIEW*>(hdr);
                if ((change->uNewState & LVIS_SELECTED) && change->iItem >= 0 && change->iItem < (int)w.rows.size()) {
                    w.current = w.rows[change->iItem];
                    ShowEditor();
                }
                return 0;
            }
            // Double-click or Space flips an on/off setting; anything else takes you to its editor.
            const bool space = hdr->code == LVN_KEYDOWN && reinterpret_cast<NMLVKEYDOWN*>(hdr)->wVKey == VK_SPACE;
            if ((space || hdr->code == NM_DBLCLK) && w.current >= 0) {
                const Tweak& t = kTweaks[w.current];
                if (t.type == TweakType::Bool) SetTweak(t, t.get() == 0);
                else SetFocus(t.type == TweakType::Float ? w.number : w.choice);
                RefreshValues();
            }
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wp), code = HIWORD(wp);
            if (id == kIdPages && code == LBN_SELCHANGE) {
                int page = (int)SendMessageW(w.pages, LB_GETCURSEL, 0, 0);
                if (page >= 0 && page != w.page) {
                    w.page = page;
                    if (Searching()) SetWindowTextW(w.search, L"");  // choosing a page ends the search
                    Rebuild(w.current);
                }
            } else if (id == kIdSearch && code == EN_CHANGE) {
                Rebuild(w.current);
            } else if (id == kIdFilter && code == CBN_SELCHANGE) {
                Rebuild(w.current);
            } else if (id == kIdChoice && code == CBN_SELCHANGE && !w.filling && w.current >= 0) {
                SetTweak(kTweaks[w.current], (double)SendMessageW(w.choice, CB_GETCURSEL, 0, 0));
                RefreshValues();
            } else if (id == kIdNumber && code == EN_KILLFOCUS && !w.filling) {
                CommitNumber();
            } else if (id == kIdReset && w.current >= 0) {
                SetTweak(kTweaks[w.current], kTweaks[w.current].def);
                RefreshValues();
            } else if (id >= kIdLink0 && id < kIdLink0 + kLinks) {
                if (int command = kPages[w.page].links[id - kIdLink0].command)
                    PostMessageW(g.hwnd, WM_COMMAND, MAKEWPARAM(command, 0), 0);
            } else if (id == kIdResetAll) {
                int count = 0;
                for (int i : std::vector<int>(w.rows))  // SetTweak refreshes the window as it goes
                    if (TweakChanged(kTweaks[i])) {
                        SetTweak(kTweaks[i], kTweaks[i].def);
                        ++count;
                    }
                ShowMessage(count == 1 ? L"Reset 1 setting to its default." :
                            L"Reset " + std::to_wstring(count) + L" settings to their defaults.");
                Rebuild(w.current);
            } else if (id == IDOK) {  // Enter: commit a typed value, or go from the list to the editor
                if (GetFocus() == w.number) CommitNumber();
                else if (GetFocus() == w.list && w.current >= 0)
                    SetFocus(kTweaks[w.current].type == TweakType::Float ? w.number : w.choice);
            } else if (id == IDCANCEL) {
                DestroyWindow(h);
            }
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_NCDESTROY:
            w = PreferencesWindow{};
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

std::span<const Tweak> Tweaks() { return kTweaks; }

bool TweakShown(const Tweak& t) {
    return kExtraThemes || (strcmp(t.key, "view.live_fountain") && strcmp(t.key, "view.robotic_sounds"));
}

const Tweak* FindTweak(std::string_view key) {
    for (const Tweak& t : kTweaks)
        if (key == t.key) return &t;
    return nullptr;
}

const wchar_t* TweakPage(const Tweak& t) { return kPages[PageOf(t)].name; }

// Floats are saved with two decimals, so anything closer than that is the default.
bool TweakChanged(const Tweak& t) { return std::fabs(t.get() - t.def) > 0.004; }

std::wstring TweakText(const Tweak& t, double value) {
    if (t.type == TweakType::Float) {
        wchar_t text[48];
        swprintf_s(text, t.unit ? L"%.2f %ls" : L"%.2f", value, t.unit);
        return text;
    }
    int i = std::clamp((int)value, 0, (int)t.max);
    return t.choices ? t.choices[i] : kOffOn[i];
}

void SetTweak(const Tweak& t, double value) {
    if (!std::isfinite(value)) return;
    value = std::clamp(value, (double)t.min, (double)t.max);
    if (t.type != TweakType::Float) value = std::round(value);
    if (value == t.get()) return;
    if (t.flags & kTweakRadio) OnCommand(t.command + (int)value, 0, nullptr);
    else if (t.command) OnCommand(t.command, 0, nullptr);
    else {
        t.put(value);
        if (t.apply) t.apply();
        SavePreferences();
    }
    if (w.hwnd) RefreshValues();
}

// Reads each tweak as LoadPreferences always has: an integer is clamped into range, anything not a
// finite number keeps the default.
void LoadTweaks() {
    for (const Tweak& t : kTweaks) {
        if (t.flags & (kTweakLegacyLoad | kTweakOutside)) continue;
        double v = t.type == TweakType::Float ? IniFloat(t.section, t.iniKey, t.def)
                                              : (double)IniInt(t.section, t.iniKey, (int)t.def);
        if (!std::isfinite(v)) v = t.def;
        if (t.type == TweakType::Bool) v = v != 0;
        t.put(std::clamp(v, (double)t.min, (double)t.max));
    }
}

void SaveTweaks() {
    for (const Tweak& t : kTweaks) {
        if (t.flags & kTweakOutside) continue;
        double v = t.get();
        IniWrite(t.section, t.iniKey, t.type == TweakType::Float ? F2S((float)v) : std::to_wstring((int)v));
    }
}

bool PreferencesDialogMessage(MSG* msg) { return w.hwnd && IsDialogMessageW(w.hwnd, msg); }

std::vector<PreferenceLink> PreferenceLinks() {
    std::vector<PreferenceLink> links;
    for (const Page& page : kPages)
        for (const Link& link : page.links)
            if (link.label) links.push_back({link.command, link.label, page.name});
    return links;
}

void ShowPreferences(const char* key) {
    const Tweak* wanted = key ? FindTweak(key) : nullptr;
    if (!w.hwnd) {
        RegisterAppClass(L"CordinalPreferences", PreferencesProc);
        RECT r{0, 0, S(800), S(600)};
        AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_DLGMODALFRAME, g.dpi);
        RECT owner;
        GetWindowRect(g.hwnd, &owner);
        int width = r.right - r.left, height = r.bottom - r.top;
        CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, L"CordinalPreferences", L"Preferences",
                        WS_OVERLAPPEDWINDOW & ~WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                        owner.left + (owner.right - owner.left - width) / 2, owner.top + S(60), width, height, g.hwnd,
                        nullptr, g.inst, nullptr);
        if (!w.hwnd) return;
    }
    int keep = w.current;
    if (wanted) {  // open on the setting's own page, with nothing hiding it
        keep = (int)(wanted - kTweaks);
        SetWindowTextW(w.search, L"");
        SendMessageW(w.filter, CB_SETCURSEL, 0, 0);
        w.page = PageOf(*wanted);
        SendMessageW(w.pages, LB_SETCURSEL, w.page, 0);
    }
    Rebuild(keep);
    ShowWindow(w.hwnd, SW_SHOW);
    SetForegroundWindow(w.hwnd);
    SetFocus(w.list);
}

}  // namespace app
