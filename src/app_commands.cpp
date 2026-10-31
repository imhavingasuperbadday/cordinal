// Playback button state (shuffle/repeat) and dispatch for every main-window command.
// OnCommand decodes the id ranges declared in app_state.h and runs the matching action.
#include "app.h"

namespace app {

// Tooltips and look of the shuffle, repeat and resume buttons after a change.
// g.buttons[3], [4] and [5] are those three toolbar buttons.
void UpdateOrderButtons() {
    for (int i : {3, 4, 5})
        if (g.buttons[i]) InvalidateRect(g.buttons[i], nullptr, FALSE);
    if (!g.tips) return;
    static const wchar_t* repeatTips[] = {L"Repeat: off (Ctrl+R)", L"Repeat: playlist (Ctrl+R)", L"Repeat: track (Ctrl+R)"};
    static const wchar_t* shuffleTips[] = {L"Shuffle: off (Ctrl+H)", L"Shuffle: tracks (Ctrl+H)", L"Shuffle: albums (Ctrl+H)"};
    const wchar_t* tips[] = {shuffleTips[g.shuffle], repeatTips[(int)g.repeat],
                             g.resumeOn ? L"Resume where you left off: on" : L"Resume where you left off: off"};
    for (int i = 0; i < 3; ++i) {
        SetTextIfChanged(g.buttons[3 + i], tips[i]);
        TTTOOLINFOW ti{sizeof(ti)};
        ti.uFlags = TTF_IDISHWND;
        ti.hwnd = g.hwnd;
        ti.uId = (UINT_PTR)g.buttons[3 + i];
        ti.lpszText = const_cast<wchar_t*>(tips[i]);
        SendMessageW(g.tips, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
    }
}

// Turning shuffle on starts a fresh pass and clears any part-played one, so it restarts
// from the current track rather than resuming an old order.
void SetShuffle(int mode) {
    if (mode && mode != g.shuffle) PlaySfx(Sfx::Dice);
    g.shuffle = mode;
    if (g.rgMode == ReplayGainMode::PlaybackOrder) RefreshGains(false);
    IniWrite(L"Player", L"Shuffle", std::to_wstring(mode));
    g.shuffleQueue.clear();
    g.shuffleStarted = false;  // a new pass starts from the current track
    RefreshNext();
    UpdateOrderButtons();
}

// Repeat is independent of shuffle and applies to the active playlist (see Repeat in
// app_state.h).
void SetRepeat(Repeat repeat) {
    g.repeat = repeat;
    IniWrite(L"Player", L"Repeat", std::to_wstring((int)repeat));
    RefreshNext();
    UpdateOrderButtons();
    UpdateStatus();
}

// Dispatch for every control and menu command. The search box's EN_CHANGE is handled
// first (debounced through kSearchTimer), then the id ranges are decoded arithmetically,
// then the table of stateless commands, then the stateful switch.
void OnCommand(int id, int code, HWND ctl) {
    if (ctl == g.search && code == EN_CHANGE) {
        if (RadioActive()) { if (!g.restoringView) RadioSearchChanged(); return; }
        int len = GetWindowTextLengthW(g.search);
        std::wstring text(len + 1, L'\0');
        GetWindowTextW(g.search, text.data(), len + 1);
        text.resize(len);
        if (g.filter == text) return;  // restoring a tab already set its query
        g.filter = text;
        if (!g.restoringView && !g.closing) {
            g.searchPending = true;
            g.dirty = true;
            if (!SetTimer(g.hwnd, kSearchTimer, 150, nullptr)) FlushSearch();
        }
        return;
    }
    FlushSearch();
    if (id == IDM_SKIP_SHUFFLE || id == IDM_RADIO) {
        if (id == IDM_SKIP_SHUFFLE) {
            g.skipAwareShuffle = !g.skipAwareShuffle;
            IniWrite(L"Player", L"SkipAwareShuffle", g.skipAwareShuffle ? L"1" : L"0");
        } else {
            g.radio = !g.radio;
            IniWrite(L"Player", L"Radio", g.radio ? L"1" : L"0");
        }
        for (auto& [token, info] : g.tokens) info.radioNext = UINT32_MAX;
        g.shuffleQueue.clear();
        g.shuffleStarted = false;
        RefreshNext();
        UpdateStatus();
        return;
    }
    // Ctrl+1..Ctrl+8 select playlists 0..7; Ctrl+9 (offset 8) always selects the last.
    if (id >= IDM_SELECT_PL_BASE && id < IDM_SELECT_PL_BASE + 9) {
        ActivatePlaylist(id == IDM_SELECT_PL_BASE + 8 ? (int)g.playlists.size() - 1 : id - IDM_SELECT_PL_BASE);
        return;
    }
    if (EditionCommand(id)) return;
    if (id == IDC_RENAME && code == EN_KILLFOCUS) {
        EndRename(true);
        return;
    }
    if (id == IDC_SHUFFLE || id == IDM_SHUFFLE || id == IDM_SHUFFLE_ALBUMS) {
        int mode = id == IDM_SHUFFLE_ALBUMS ? kShuffleAlbums : kShuffleTracks;
        SetShuffle(id == IDC_SHUFFLE ? (g.shuffle + 1) % kShuffleCount : g.shuffle == mode ? kShuffleOff : mode);
        return;
    }
    // The button cycles off -> playlist -> track -> off, like every phone player.
    if (id == IDC_REPEAT) {
        SetRepeat((Repeat)(((int)g.repeat + 1) % (int)Repeat::Count));
        return;
    }
    if (id == IDC_RESUME || id == IDM_RESUME) {
        g.resumeOn = !g.resumeOn;
        if (!g.resumeOn) {
            g.resumeTrack = kNone;
            g.resumePl = -1;
            g.resumePos = 0;
            g.resumeToken = 0;
        }
        UpdateOrderButtons();
        SavePreferences();
        UpdateNowPlaying();
        return;
    }
    // Ctrl+R (IDM_REPEAT_BASE + All) toggles repeat; the menu's other items set their mode.
    if (id >= IDM_SIZE_BASE && id < IDM_SIZE_BASE + (int)ViewSize::Count) {
        SetViewSize((ViewSize)(id - IDM_SIZE_BASE));
        return;
    }
    if (LayoutCommand(id) || WaveLookCommand(id) || ConvertCommand(id) || CopyCommand(id)) return;
    if (id >= IDM_PANEL_BASE && id < IDM_PANEL_BASE + (int)kPanelCount) {
        TogglePanel(id - IDM_PANEL_BASE);
        return;
    }
    if (id >= IDM_LOOPS_BASE && id <= IDM_LOOPS_BASE + 3) {
        SetLoops(id - IDM_LOOPS_BASE);
        return;
    }
    if (id >= IDM_SPEED_BASE && id < IDM_SPEED_BASE + (int)std::size(kSpeeds)) {
        SetSpeed(kSpeeds[id - IDM_SPEED_BASE]);
        return;
    }
    if (id >= IDM_TEMPO_BASE && id < IDM_TEMPO_BASE + (int)std::size(kTempos)) {
        SetTempo(kTempos[id - IDM_TEMPO_BASE]);
        return;
    }
    if (id >= IDM_REVERB_BASE && id < IDM_REVERB_BASE + (int)ReverbPreset::Count) {
        g.reverb.preset = (ReverbPreset)(id - IDM_REVERB_BASE);
        ApplyEffects();
        SavePreferences();
        return;
    }
    if (id >= IDM_REVERB_MIX_BASE && id < IDM_REVERB_MIX_BASE + (int)std::size(kReverbMixes)) {
        g.reverb.mix = kReverbMixes[id - IDM_REVERB_MIX_BASE];
        if (g.reverb.preset == ReverbPreset::Off) g.reverb.preset = ReverbPreset::Room;  // choosing an amount means wanting it
        ApplyEffects();
        SavePreferences();
        return;
    }
    if (id >= IDM_REPEAT_BASE && id < IDM_REPEAT_BASE + (int)Repeat::Count) {
        Repeat r = (Repeat)(id - IDM_REPEAT_BASE);
        SetRepeat(r == Repeat::All && g.repeat == Repeat::All ? Repeat::Off : r);
        return;
    }
    if (id >= IDM_COLUMN_BASE && id < IDM_COLUMN_BASE + ColumnTotal()) {
        ToggleColumn(id - IDM_COLUMN_BASE);
        return;
    }
    if (id >= IDM_RATE_BASE && id <= IDM_RATE_BASE + 10) {
        SetRating(SelectedTracks(), (uint8_t)((id - IDM_RATE_BASE) * 10));
        return;
    }
    if (PackCommand(id) || HelperMenuCommand(id)) return;
    if (id == IDM_THEME_ROBOTIC) {
        ToggleRoboticSounds();
        return;
    }
    if (id >= IDM_THEME_BASE && id < IDM_THEME_BASE + (int)ThemeMode::Count && ThemeAvailable((ThemeMode)(id - IDM_THEME_BASE))) {
        g.themeMode = (ThemeMode)(id - IDM_THEME_BASE);
        ApplyTheme();
        SavePreferences();
        return;
    }
    // Commands with no notification-specific state share one dispatch loop.
    static const struct { int id; void (*run)(); } commands[] = {
        {IDM_MUTE, ToggleMute},
        {IDC_PREV, Previous},
        {IDM_PREV, Previous},
        {IDC_PLAY, TogglePause},
        {IDC_PAUSE, TogglePause},
        {IDM_PLAY_PAUSE, TogglePause},
        {IDC_STOP, Stop},
        {IDM_STOP, Stop},
        {IDC_NEXT, Next},
        {IDM_NEXT, Next},
        {IDM_STOP_AFTER, [] { g.stopAfterCurrent = !g.stopAfterCurrent; RefreshNext(); UpdateStatus(); }},
        {IDM_ADD_FILES, [] { AddPaths(PickPaths(false), false); }},
        {IDM_ADD_FOLDER, [] { AddPaths(PickPaths(true), false); }},
        {IDM_IMPORT_M3U, ImportPlaylistFile},
        {IDM_EXPORT_M3U, ExportPlaylistFile},
        {IDM_BACKUP, BackUpProfile},
        {IDM_RESTORE_BACKUP, RestoreProfile},
        {IDM_IMPORT_STATISTICS, ImportPlaybackStatistics},
        {IDM_CANCEL_BACKUP, CancelBackup},
        {IDM_EXIT, [] { SendMessageW(g.hwnd, WM_CLOSE, 0, 0); }},
        {IDM_WEB_SERVER, ShowWebServer},
        {IDM_REMOVE, [] { RemoveTracks(false); }},
        {IDM_CROP, [] { RemoveTracks(true); }},
        {IDM_REMOVE_DUPES, RemoveDuplicates},
        {IDM_REMOVE_DEAD, RemoveDeadItems},
        {IDM_NEW_PL, [] { NewPlaylist(L"New Playlist"); BeginRename(g.active); }},
        {IDM_RENAME_PL, [] { BeginRename(g.active); }},
        {IDM_REMOVE_PL, [] { if (RadioActive()) CloseRadio(); else RemovePlaylist(g.active); }},
        {IDM_NEXT_PL, [] { ActivatePlaylist((g.active + 1) % (int)g.playlists.size()); }},
        {IDM_PREV_PL, [] { ActivatePlaylist((g.active + (int)g.playlists.size() - 1) % (int)g.playlists.size()); }},
        {IDM_TASKBAR_PROGRESS, [] { SetTaskbarProgress(!g.taskbarProgress); }},
        {IDM_MINI_PLAYER, ToggleMiniPlayer},
        {IDM_STARTUP_DEFAULTS, [] { ShowStartupDefaults(); }},
        {IDM_PREFERENCE_ASK, [] { SetPreferenceSaving(PreferenceSaving::Ask); }},
        {IDM_PREFERENCE_AUTO, [] { SetPreferenceSaving(PreferenceSaving::Automatic); }},
        {IDM_PREFERENCE_MANUAL, [] { SetPreferenceSaving(PreferenceSaving::Manual); }},
        {IDM_PANELS_RESET, ResetPanels},
        {IDM_SMALLER, ViewSmaller},
        {IDM_LARGER, ViewLarger},
        {IDM_TRAY_SHOW, ShowMainWindow},
        {IDM_START_WITH_WINDOWS, ToggleStartWithWindows},
        {IDM_LYRICS, ToggleLyricsPanel},
        {IDM_FORMAT, ShowFormatWindow},
        {IDM_PREFERENCES, [] { ShowPreferences(); }},
        {IDM_COMMAND_PALETTE, ShowCommandPalette},
        {IDC_RADIO_TAB, ActivateRadio},
        {IDM_OPEN_RADIO, ActivateRadio},
        {IDM_GAME_NAME_THAT_TRACK, [] { ShowGames(0); }},
        {IDM_POINTS, ShowPoints},
        {IDM_REVIEW, ShowReview},
        {IDM_LISTENBRAINZ, ShowListenBrainz},
        {IDM_STEAM, AddSteamSoundtracks},
        {IDM_WATCH_IMPORTED, WatchImportedFolders},
        {IDM_DUPLICATES, FindDuplicateAudioPlaylist},
        {IDM_GAME_WHICH_CAME_FIRST, [] { ShowGames(1); }},
        {IDM_ABOUT, ShowAbout},
        {IDM_COLUMNS_RESET, ResetColumns},
        {IDM_QUEUE_NEXT, [] { QueueTracks(SelectedTracks(), true); }},
        {IDM_QUEUE_ADD, [] { QueueTracks(SelectedTracks(), false); }},
        {IDM_QUEUE_CLEAR, ClearQueue},
        {IDM_NEW_AUTO, [] { ShowAutoPlaylistEditor(-1); }},
        {IDM_ORGANIZE_UNDO, UndoOrganize},
        {IDM_TAG_UNDO, UndoLastTagEdit},
        {IDC_VIEW4, ToggleBrowser},
        {IDM_LIBRARY_FOLDERS, ShowLibraryFolders},
        {IDM_REMOVE_MISSING, RemoveMissingTracks},
        {IDM_INBOX_ACCEPT, [] { AcceptInbox(SelectedTracks()); }},
        {IDM_EXCLUDE, [] { ExcludeFromLibrary(SelectedTracks()); }},
        {IDM_PLAY_SEL, PlayFocusedOrFirst},
        {IDM_SHOW_IN_EXPLORER, ShowInExplorer},
        {IDM_PROPERTIES, ShowProperties},
        {IDM_EQUALIZER, ShowEqualizer},
        {IDM_HEADPHONES, ShowHeadphones},
        {IDM_RG_ENABLE, [] {
             g.rgOn = !g.rgOn;
             SavePreferences();
             RefreshGains();
             ShowMessage(g.rgOn ? L"ReplayGain on." : L"ReplayGain off.");
         }},
        {IDM_RG_AUTO_SCAN, [] { g.rgAutoScan = !g.rgAutoScan; SavePreferences(); }},
        {IDM_RG_ESTIMATE, [] { g.rgEstimate = !g.rgEstimate; SavePreferences(); }},
        {IDM_LOUDNESS_COMP, [] { g.loudnessComp = !g.loudnessComp; ApplyEffects(); SavePreferences(); }},
        {IDM_NIGHT_MODE, [] {
             g.nightMode = !g.nightMode;
             ApplyEffects();
             SavePreferences();
             ShowMessage(g.nightMode ? L"Night mode on: quiet parts are lifted and loud parts softened." : L"Night mode off.");
         }},
        {IDM_RG_CANCEL, CancelReplayGain},
        {IDM_RG_CLEAR, [] {
             auto sel = SelectedTracks();
             if (sel.empty()) { ShowMessage(L"Select the tracks whose ReplayGain you want to clear."); return; }
             ClearReplayGain(sel);
         }},
        {IDM_RG_WRITE_TAGS, [] {
             auto sel = SelectedTracks();
             if (sel.empty()) { ShowMessage(L"Select the tracks to write ReplayGain tags for."); return; }
             WriteReplayGainTags(sel);
         }},
        {IDM_WRITE_STATISTICS, [] { WriteStatisticsTags(SelectedTracks()); }},
        {IDM_RG_SCAN_SEL, [] { ScanReplayGain(SelectedTracks()); }},
        {IDM_RG_SCAN_PL, [] { ScanReplayGain(Active().items); }},
    };
    for (const auto& command : commands)
        if (id == command.id) {
            command.run();
            return;
        }
    switch (id) {
        case IDM_DUPLICATE_PL: DuplicatePlaylist(); break;
        case IDM_REOPEN_PL: ReopenPlaylist(); break;
        case IDM_MOVE_PL_LEFT: ReorderPlaylist(g.active, g.active - 1); break;
        case IDM_MOVE_PL_RIGHT: ReorderPlaylist(g.active, g.active + 1); break;
        case IDM_PLAYING_PL: RevealPlayingTrack(); break;
        case IDM_CANCEL_IMPORT: {
            std::lock_guard<std::mutex> lock(g.importMx);
            g.importCancel = true;
            g.importQueue.clear();
            ShowMessage(L"Cancelling file import...");
            break;
        }
        case IDM_RELOAD_TAGS: {
            auto ids = SelectedTracks();
            for (TrackId track : ids) if (!g.tagPending.count(track)) g_lib[track].flags &= ~(Track::kLoaded | Track::kTagFailed);
            QueueTags(ids);
            break;
        }
        // The waveform's section menu (app_region.cpp) shares this switch, acting at the playhead.
        case IDM_REGION_LOOP:
        case IDM_REGION_CLEAR:
            if (CommandRegion(id, g.engine.Position())) return;
            break;
        case IDC_NP_TITLE: case IDC_NP_SUB:
            if (code == STN_CLICKED && !Resume()) RevealPlayingTrack();
            break;
        case IDC_SPEAKER:
            if (code == STN_CLICKED) ToggleMute();
            break;
        case IDC_TIME:
            if (code == STN_CLICKED) {
                g.timeRemaining = !g.timeRemaining;
                SavePreferences();
                UpdateStatus();
            }
            break;
        case IDM_SELECT_ALL:
            if (CanvasActive()) LibViewSelectAll();
            else ListView_SetItemState(g.list, -1, LVIS_SELECTED, LVIS_SELECTED);
            break;
        case IDM_CLEAR:
            if (!Active().Editable()) break;
            RemapOccurrences(g.active, std::vector<uint32_t>(Active().items.size(), UINT32_MAX));
            std::vector<TrackId>().swap(Active().items);
            g.compactPending = true;
            ClearSelection();
            RebuildView();
            RefreshNext();
            g.dirty = true;
            break;
        case IDM_FIND:
            if (g.size == ViewSize::Normal && !g.layout.search) {  // a hidden search box comes back to be typed in
                g.layout.search = true;
                ApplyMainLayout();
            }
            SetFocus(g.search);
            SendMessageW(g.search, EM_SETSEL, 0, -1);
            break;
        case IDM_ALWAYS_ON_TOP:
            g.alwaysOnTop = !g.alwaysOnTop;
            SavePreferences();
            SetWindowPos(g.hwnd, g.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            break;
        case IDM_GROUP_ALBUMS:
            g.groupAlbums = !g.groupAlbums;
            SavePreferences();
            RefreshViewKeepingSelection();
            break;
        case IDM_SOUND_EFFECTS:
            g.soundEffects = !g.soundEffects;
            SavePreferences();
            PlaySfx(Sfx::Milestone);  // what-cheer!
            if (g.soundEffects && g.exclusive) ShowMessage(L"Sound effects stay silent while output is exclusive.");
            break;
        case IDM_ORGANIZE: {
            // The selection, or the whole library when nothing is selected.
            auto ids = SelectedTracks();
            if (ids.empty())
                for (TrackId t = 0; t < (TrackId)g_lib.Count(); ++t)
                    if (g_lib[t].InLibrary()) ids.push_back(t);
            ShowOrganize(ids);
            break;
        }
        case IDM_OUTPUT_SHARED:
        case IDM_OUTPUT_EXCLUSIVE:
            g.exclusive = id == IDM_OUTPUT_EXCLUSIVE;
            SavePreferences();
            sfx::SetSuppressed(g.exclusive);  // lets go of the device before exclusive output asks for it
            g.engine.SetExclusive(g.exclusive);
            ShowMessage(g.exclusive ? L"Exclusive output: each track at its own rate, bit-perfect at 100% volume with EQ, "
                                      L"crossfeed and ReplayGain off. Other programs can't play sound meanwhile."
                                    : L"Shared output.");
            break;
        case IDM_EDIT_AUTO:
            if (Active().kind == PlaylistKind::Auto) ShowAutoPlaylistEditor(g.active);
            break;
        case IDC_VIEW0: case IDC_VIEW1: case IDC_VIEW2: case IDC_VIEW3:
            SetLibraryMode((LibraryMode)(id - IDC_VIEW0));
            break;
        case IDM_RESCAN:
            if (g.libraryFolders.empty()) ShowLibraryFolders();
            else g.scanner.RescanAll();
            break;
        case IDM_INBOX_ACCEPT_ALL: {
            std::vector<TrackId> all;
            for (TrackId t = 0; t < (TrackId)g_lib.Count(); ++t)
                if (g_lib[t].flags & Track::kInbox) all.push_back(t);
            AcceptInbox(all);
            break;
        }
    }
}

}  // namespace app
