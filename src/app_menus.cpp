// Builds the menu bar and its ReplayGain submenu, the accelerator table, and the routine that keeps
// menu checkmarks and enabled state in sync with the app globals. The menus hold actions and the few
// toggles changed while a song plays; every setting lives in Preferences (app_tweaks.cpp). Command
// ids share IDM_*_BASE ranges so a whole group can be checked or enabled in one call.
#include "app.h"

namespace app {

namespace {

// The popup of `menu` (or `menu` itself) that holds command `id`, and the item's position in it.
HMENU FindCommand(HMENU menu, UINT id, int& position) {
    for (int i = 0, n = GetMenuItemCount(menu); i < n; ++i) {
        if (GetMenuItemID(menu, i) == id) {
            position = i;
            return menu;
        }
        if (HMENU sub = GetSubMenu(menu, i))
            if (HMENU found = FindCommand(sub, id, position)) return found;
    }
    return nullptr;
}

// A cancel command is in the menus only while there is something to cancel: it appears right after
// the command that starts the work and goes again when the work is done.
void ShowWhileRunning(HMENU menu, UINT after, UINT id, const wchar_t* text, bool running) {
    int position = 0;
    if (HMENU shown = FindCommand(menu, id, position)) {
        if (!running) DeleteMenu(shown, position, MF_BYPOSITION);
    } else if (running) {
        if (HMENU host = FindCommand(menu, after, position))
            InsertMenuW(host, position + 1, MF_BYPOSITION | MF_STRING, id, text);
    }
}

}  // namespace

// ReplayGain on or off, and what to do to tracks. How it sounds (the preamp, how much loud songs
// keep, measuring and estimating) is set in Preferences.
HMENU ReplayGainMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_RG_ENABLE, L"&Enabled");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_RG_SCAN_SEL, L"&Scan selection");
    AppendMenuW(m, MF_STRING, IDM_RG_SCAN_PL, L"Scan &playlist");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_RG_CLEAR, L"Clear ReplayGain");
    AppendMenuW(m, MF_STRING, IDM_RG_WRITE_TAGS, L"&Write ReplayGain tags");
    return m;
}

HMENU BuildMenu() {
    HMENU bar = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, IDM_ADD_FILES, L"&Add files...\tCtrl+O");
    AppendMenuW(file, MF_STRING, IDM_ADD_FOLDER, L"Import &folder once...\tCtrl+Shift+O");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_IMPORT_M3U, L"&Open playlist file...");
    AppendMenuW(file, MF_STRING, IDM_EXPORT_M3U, L"&Save playlist as...\tCtrl+S");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_BACKUP, L"Back up library and settings...");
    AppendMenuW(file, MF_STRING, IDM_RESTORE_BACKUP, L"Restore backup...");
    AppendMenuW(file, MF_STRING, IDM_IMPORT_STATISTICS, L"Import playback statistics...");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_WEB_SERVER, L"&Web server...");
    AppendMenuW(file, MF_STRING, IDM_PREFERENCES, L"&Preferences...\tCtrl+,");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_EXIT, L"E&xit");

    HMENU edit = CreatePopupMenu();
    AppendMenuW(edit, MF_STRING, IDM_SELECT_ALL, L"Select &all\tCtrl+A");
    AppendMenuW(edit, MF_STRING, IDM_REMOVE, L"&Remove\tDel");
    AppendMenuW(edit, MF_STRING, IDM_CROP, L"&Keep only selected tracks");
    AppendMenuW(edit, MF_STRING, IDM_CLEAR, L"C&lear playlist");
    AppendMenuW(edit, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(edit, MF_STRING, IDM_RELOAD_TAGS, L"Reload selected metadata");
    AppendMenuW(edit, MF_STRING, IDM_REMOVE_DUPES, L"Remove &duplicates");
    AppendMenuW(edit, MF_STRING, IDM_REMOVE_DEAD, L"Remove dead &items");
    AppendMenuW(edit, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(edit, MF_STRING, IDM_FIND, L"&Search\tCtrl+F");
    AppendMenuW(edit, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(edit, MF_STRING, IDM_PROPERTIES, L"Edit &tags...\tAlt+Enter");
    AppendMenuW(edit, MF_STRING, IDM_TAG_UNDO, L"&Undo last tag edit\tCtrl+Z");
    AppendHelperTools(edit, true);  // the tools of the helpers that are on, when there are any

    HMENU playback = CreatePopupMenu();
    AppendMenuW(playback, MF_STRING, IDM_PLAY_PAUSE, L"&Play / Pause\tSpace");
    AppendMenuW(playback, MF_STRING, IDM_STOP, L"&Stop");
    AppendMenuW(playback, MF_STRING, IDM_MUTE, L"&Mute\tCtrl+M");
    AppendMenuW(playback, MF_STRING, IDM_PREV, L"Pre&vious\tCtrl+Left");
    AppendMenuW(playback, MF_STRING, IDM_NEXT, L"&Next\tCtrl+Right");
    AppendMenuW(playback, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(playback, MF_STRING, IDM_SHUFFLE, L"S&huffle\tCtrl+H");
    AppendMenuW(playback, MF_STRING, IDM_SHUFFLE_ALBUMS, L"Shuffle al&bums");
    AppendMenuW(playback, MF_STRING, IDM_RADIO, L"Keep playing like this");
    AppendMenuW(playback, MF_STRING, IDM_REPEAT_BASE + (int)Repeat::Off, L"Repeat o&ff");
    AppendMenuW(playback, MF_STRING, IDM_REPEAT_BASE + (int)Repeat::All, L"&Repeat playlist\tCtrl+R");
    AppendMenuW(playback, MF_STRING, IDM_REPEAT_BASE + (int)Repeat::One, L"Repeat &track");
    AppendMenuW(playback, MF_STRING, IDM_STOP_AFTER, L"Stop &after current");
    AppendMenuW(playback, MF_STRING, IDM_QUEUE_CLEAR, L"&Clear the queue");
    AppendMenuW(playback, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(playback, MF_POPUP, (UINT_PTR)ReplayGainMenu(), L"&ReplayGain");
    HMENU speed = CreatePopupMenu(), tempo = CreatePopupMenu();
    for (int i = 0; i < (int)std::size(kSpeeds); ++i) {
        wchar_t buf[32];
        swprintf_s(buf, kSpeeds[i] == 1 ? L"&Normal" : L"%gx", kSpeeds[i]);
        AppendMenuW(speed, MF_STRING, IDM_SPEED_BASE + i, buf);
    }
    for (int i = 0; i < (int)std::size(kTempos); ++i) {
        wchar_t buf[32];
        swprintf_s(buf, kTempos[i] == 1 ? L"&Normal" : L"%gx", kTempos[i]);
        AppendMenuW(tempo, MF_STRING, IDM_TEMPO_BASE + i, buf);
    }
    AppendMenuW(playback, MF_POPUP, (UINT_PTR)speed, L"Spee&d (pitch follows)");
    AppendMenuW(playback, MF_POPUP, (UINT_PTR)tempo, L"Temp&o (pitch stays)");
    HMENU reverb = CreatePopupMenu();
    static const wchar_t* const reverbNames[] = {L"&Off", L"&Room", L"&Hall", L"&Plate"};
    for (int i = 0; i < (int)ReverbPreset::Count; ++i) AppendMenuW(reverb, MF_STRING, IDM_REVERB_BASE + i, reverbNames[i]);
    AppendMenuW(reverb, MF_SEPARATOR, 0, nullptr);
    static const wchar_t* const mixNames[] = {L"&Subtle (25%)", L"&Light (40%)", L"&Medium (55%)", L"S&trong (75%)"};
    for (int i = 0; i < (int)std::size(kReverbMixes); ++i) AppendMenuW(reverb, MF_STRING, IDM_REVERB_MIX_BASE + i, mixNames[i]);
    AppendMenuW(playback, MF_POPUP, (UINT_PTR)reverb, L"Rever&b");
    AppendMenuW(playback, MF_STRING, IDM_NIGHT_MODE, L"Ni&ght mode");
    AppendMenuW(playback, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(playback, MF_STRING, IDM_EQUALIZER, L"&Equalizer...\tCtrl+E");
    AppendMenuW(playback, MF_STRING, IDM_HEADPHONES, L"&Headphones and device settings...");

    HMENU playlist = CreatePopupMenu();
    AppendMenuW(playlist, MF_STRING, IDM_NEW_PL, L"&New playlist\tCtrl+N");
    AppendMenuW(playlist, MF_STRING, IDM_NEW_AUTO, L"New &auto-playlist...");
    AppendMenuW(playlist, MF_STRING, IDM_EDIT_AUTO, L"&Edit auto-playlist rules...");
    AppendPackAutoPlaylists(playlist);
    AppendMenuW(playlist, MF_STRING, IDM_RENAME_PL, L"&Rename playlist\tF2");
    AppendMenuW(playlist, MF_STRING, IDM_REMOVE_PL, L"&Close playlist\tCtrl+W");
    AppendMenuW(playlist, MF_STRING, IDM_DUPLICATE_PL, L"&Duplicate playlist\tCtrl+Shift+D");
    AppendMenuW(playlist, MF_STRING, IDM_REOPEN_PL, L"Reopen closed playlist\tCtrl+Shift+T");
    AppendMenuW(playlist, MF_STRING, IDM_MOVE_PL_LEFT, L"Move left\tCtrl+Shift+PageUp");
    AppendMenuW(playlist, MF_STRING, IDM_MOVE_PL_RIGHT, L"Move right\tCtrl+Shift+PageDown");
    AppendMenuW(playlist, MF_STRING, IDM_PLAYING_PL, L"Reveal playing track\tCtrl+J");
    AppendMenuW(playlist, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(playlist, MF_STRING, IDM_NEXT_PL, L"Ne&xt playlist\tCtrl+Tab");
    AppendMenuW(playlist, MF_STRING, IDM_PREV_PL, L"Pre&vious playlist\tCtrl+Shift+Tab");

    HMENU library = CreatePopupMenu();
    AppendMenuW(library, MF_STRING, IDM_LIBRARY_FOLDERS, L"Library &folders...");
    AppendMenuW(library, MF_STRING, IDM_RESCAN, L"&Rescan folders");
    AppendMenuW(library, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(library, MF_STRING, IDM_INBOX_ACCEPT_ALL, L"Add everything in the &Inbox");
    AppendMenuW(library, MF_STRING, IDM_REMOVE_MISSING, L"Forget &missing files...");
    AppendMenuW(library, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(library, MF_STRING, IDM_ORGANIZE, L"&Organize files...");
    AppendMenuW(library, MF_STRING, IDM_ORGANIZE_UNDO, L"&Undo last organize");
    AppendMenuW(library, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(library, MF_STRING, IDM_CONVERT, L"Con&vert to .cord...");
    AppendMenuW(library, MF_POPUP, (UINT_PTR)CordExportMenu(), L"&Export .cord");
    AppendMenuW(library, MF_STRING, IDM_CONVERT_UNDO, L"Undo last conversion");
    AppendMenuW(library, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(library, MF_STRING, IDM_COPY_TO, L"Cop&y to folder or device...");
    AppendMenuW(library, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(library, MF_STRING, IDM_DUPLICATES, L"Find &duplicate audio");
    AppendMenuW(library, MF_STRING, IDM_STEAM, L"Add &Steam soundtracks");

    HMENU view = CreatePopupMenu();
    // Ids follow ThemeMode so a single radio-check call can index them. Light and Dark lead, then
    // Follow Windows, then the themed palettes (theme_palette.cpp ThemeInfo).
    HMENU themes = CreatePopupMenu();
    for (ThemeMode t : {ThemeMode::Light, ThemeMode::Dark, ThemeMode::System})
        AppendMenuW(themes, MF_STRING, IDM_THEME_BASE + (int)t, ThemeInfo(t).name);
    if (kExtraThemes) {
        AppendMenuW(themes, MF_SEPARATOR, 0, nullptr);
        for (int t = (int)ThemeMode::Deltarune; t < (int)ThemeMode::Count; ++t)
            AppendMenuW(themes, MF_STRING, IDM_THEME_BASE + t, ThemeInfo((ThemeMode)t).name);
    }
    AppendPackThemes(themes);
    AppendMenuW(view, MF_POPUP, (UINT_PTR)themes, L"&Theme");
    AppendMenuW(view, MF_POPUP, (UINT_PTR)PacksMenu(), L"Pac&ks");
    AppendMenuW(view, MF_POPUP, (UINT_PTR)HelpersMenu(), L"&Helpers");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, IDM_OPEN_RADIO, L"&Radio\tCtrl+Shift+R");
    AppendMenuW(view, MF_STRING, IDM_GROUP_ALBUMS, L"&Group tracks by album");
    AppendMenuW(view, MF_STRING, IDM_LYRICS, L"L&yrics panel\tCtrl+L");
    AppendMenuW(view, MF_POPUP, (UINT_PTR)LayoutMenu(), L"Customize la&yout");
    HMENU sizes = CreatePopupMenu();
    const wchar_t* sizeNames[] = {L"&Compact", L"&Normal", L"Big mode &plus", L"&Big mode"};
    for (int i = 0; i < (int)ViewSize::Count; ++i) AppendMenuW(sizes, MF_STRING, IDM_SIZE_BASE + i, sizeNames[i]);
    AppendMenuW(sizes, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(sizes, MF_STRING, IDM_SMALLER, L"S&maller\tCtrl+-");
    AppendMenuW(sizes, MF_STRING, IDM_LARGER, L"&Larger\tCtrl+=");
    AppendMenuW(view, MF_POPUP, (UINT_PTR)sizes, L"Window si&ze");
    HMENU panels = CreatePopupMenu();
    const wchar_t* panelNames[] = {L"&Library", L"&Now playing", L"&Up next"};
    for (int i = 0; i < kPanelCount; ++i) AppendMenuW(panels, MF_STRING, IDM_PANEL_BASE + i, panelNames[i]);
    AppendMenuW(panels, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(panels, MF_STRING, IDM_PANELS_RESET, L"&Reset the layout");
    AppendMenuW(view, MF_POPUP, (UINT_PTR)panels, L"Big mode pa&nels");
    AppendMenuW(view, MF_STRING, IDM_MINI_PLAYER, L"&Mini player\tCtrl+Shift+M");
    AppendMenuW(view, MF_STRING, IDM_ALWAYS_ON_TOP, L"&Always on top");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, IDM_COMMAND_PALETTE, L"&Command palette...\tCtrl+Shift+P");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    // The extras that play with your library and your listening, behind one entry.
    HMENU extras = CreatePopupMenu();
    AppendMenuW(extras, MF_STRING, IDM_GAME_NAME_THAT_TRACK, L"&Name That Track...");
    AppendMenuW(extras, MF_STRING, IDM_GAME_WHICH_CAME_FIRST, L"&Which Came First...");
    AppendMenuW(extras, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(extras, MF_STRING, IDM_POINTS, L"cordinal &points...");
    AppendMenuW(extras, MF_STRING, IDM_REVIEW, L"&Year in review...");
    AppendMenuW(view, MF_POPUP, (UINT_PTR)extras, L"Games and &stats");
    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, IDM_ABOUT, L"&About cordinal");

    // The titles are shared with the Linux port's menu bar (ui_spec.h).
    HMENU menus[] = {file, edit, playback, playlist, library, view, help};
    static_assert(std::size(menus) == std::size(ui::kMenuBar));
    for (size_t i = 0; i < std::size(menus); ++i) AppendMenuW(bar, MF_POPUP, (UINT_PTR)menus[i], ui::kMenuBar[i]);
    return bar;
}

// Built and installed once. Ctrl+= / Ctrl+- and their keypad and shifted variants all map to the
// same size commands; Ctrl+R jumps straight to Repeat All (the command handler cycles onward).
HACCEL BuildAccelerators() {
    std::vector<ACCEL> keys = {
        {FCONTROL | FSHIFT | FVIRTKEY, 'R', IDM_OPEN_RADIO},
        {FCONTROL | FSHIFT | FVIRTKEY, 'T', IDM_REOPEN_PL},
        {FCONTROL | FSHIFT | FVIRTKEY, 'D', IDM_DUPLICATE_PL},
        {FCONTROL | FSHIFT | FVIRTKEY, VK_PRIOR, IDM_MOVE_PL_LEFT},
        {FCONTROL | FSHIFT | FVIRTKEY, VK_NEXT, IDM_MOVE_PL_RIGHT},
        {FCONTROL | FVIRTKEY, 'J', IDM_PLAYING_PL},
        {FCONTROL | FVIRTKEY, '1', IDM_SELECT_PL_BASE + 0},
        {FCONTROL | FVIRTKEY, '2', IDM_SELECT_PL_BASE + 1},
        {FCONTROL | FVIRTKEY, '3', IDM_SELECT_PL_BASE + 2},
        {FCONTROL | FVIRTKEY, '4', IDM_SELECT_PL_BASE + 3},
        {FCONTROL | FVIRTKEY, '5', IDM_SELECT_PL_BASE + 4},
        {FCONTROL | FVIRTKEY, '6', IDM_SELECT_PL_BASE + 5},
        {FCONTROL | FVIRTKEY, '7', IDM_SELECT_PL_BASE + 6},
        {FCONTROL | FVIRTKEY, '8', IDM_SELECT_PL_BASE + 7},
        {FCONTROL | FVIRTKEY, '9', IDM_SELECT_PL_BASE + 8},
        {FCONTROL | FVIRTKEY, 'O', IDM_ADD_FILES},
        {FALT | FVIRTKEY, VK_RETURN, IDM_PROPERTIES},
        {FCONTROL | FVIRTKEY, 'Z', IDM_TAG_UNDO},
        {FCONTROL | FVIRTKEY, VK_RETURN, IDM_QUEUE_NEXT},
        {FCONTROL | FVIRTKEY, 'Q', IDM_QUEUE_ADD},
        {FCONTROL | FSHIFT | FVIRTKEY, 'O', IDM_ADD_FOLDER},
        {FCONTROL | FVIRTKEY, 'S', IDM_EXPORT_M3U},
        {FCONTROL | FVIRTKEY, 'F', IDM_FIND},
        {FCONTROL | FVIRTKEY, 'N', IDM_NEW_PL},
        {FCONTROL | FVIRTKEY, 'W', IDM_REMOVE_PL},
        {FCONTROL | FVIRTKEY, 'E', IDM_EQUALIZER},
        {FCONTROL | FVIRTKEY, 'M', IDM_MUTE},
        {FCONTROL | FSHIFT | FVIRTKEY, 'M', IDM_MINI_PLAYER},
        {FCONTROL | FVIRTKEY, VK_OEM_MINUS, IDM_SMALLER},
        {FCONTROL | FVIRTKEY, VK_SUBTRACT, IDM_SMALLER},
        {FCONTROL | FVIRTKEY, VK_OEM_PLUS, IDM_LARGER},
        {FCONTROL | FSHIFT | FVIRTKEY, VK_OEM_PLUS, IDM_LARGER},
        {FCONTROL | FVIRTKEY, VK_ADD, IDM_LARGER},
        {FCONTROL | FVIRTKEY, 'L', IDM_LYRICS},
        {FCONTROL | FVIRTKEY, VK_OEM_COMMA, IDM_PREFERENCES},
        {FCONTROL | FSHIFT | FVIRTKEY, 'P', IDM_COMMAND_PALETTE},
        {FCONTROL | FVIRTKEY, 'H', IDC_SHUFFLE},
        {FCONTROL | FVIRTKEY, 'R', IDM_REPEAT_BASE + (int)Repeat::All},
        {FVIRTKEY, VK_F2, IDM_RENAME_PL},
        {FCONTROL | FVIRTKEY, VK_TAB, IDM_NEXT_PL},
        {FCONTROL | FSHIFT | FVIRTKEY, VK_TAB, IDM_PREV_PL},
        {FCONTROL | FVIRTKEY, VK_PRIOR, IDM_PREV_PL},
        {FCONTROL | FVIRTKEY, VK_NEXT, IDM_NEXT_PL},
        {FCONTROL | FVIRTKEY, VK_RIGHT, IDM_NEXT},
        {FCONTROL | FVIRTKEY, VK_LEFT, IDM_PREV},
    };
    AppendPackAccelerators(keys);  // in front, so a pack's key wins
    RegisterPackHotkeys();
    return CreateAcceleratorTableW(keys.data(), (int)keys.size());
}

// Syncs checkmarks and enabled state after any state change, for the menu bar or one of its popups.
// Radio groups use their IDM_*_BASE offsets (repeat, reverb, theme, size). The cancel commands come
// and go with the work they stop.
void UpdateMainMenu(HMENU m) {
    const struct { UINT id; bool checked; } checks[] = {
        {IDM_SHUFFLE, g.shuffle == kShuffleTracks}, {IDM_SHUFFLE_ALBUMS, g.shuffle == kShuffleAlbums},
        {IDM_RADIO, g.radio}, {IDM_MUTE, g.volumePercent == 0},
        {IDM_RG_ENABLE, g.rgOn}, {IDM_STOP_AFTER, g.stopAfterCurrent}, {IDM_NIGHT_MODE, g.nightMode},
        {IDM_EQUALIZER, g.eq.enabled || g.eq.crossfeed}, {IDM_ALWAYS_ON_TOP, g.alwaysOnTop},
        {IDM_GROUP_ALBUMS, g.groupAlbums}, {IDM_LYRICS, LyricsPanelShown()},
    };
    for (const auto& item : checks) CheckMenuItem(m, item.id, MF_BYCOMMAND | (item.checked ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuRadioItem(m, IDM_REVERB_BASE, IDM_REVERB_BASE + (int)ReverbPreset::Count - 1,
                       IDM_REVERB_BASE + (int)g.reverb.preset, MF_BYCOMMAND);
    for (int i = 0; i < (int)std::size(kReverbMixes); ++i)
        CheckMenuItem(m, IDM_REVERB_MIX_BASE + i, MF_BYCOMMAND | (std::fabs(g.reverb.mix - kReverbMixes[i]) < 0.005f ? MF_CHECKED : MF_UNCHECKED));
    for (int i = 0; i < (int)std::size(kSpeeds); ++i)
        CheckMenuItem(m, IDM_SPEED_BASE + i, MF_BYCOMMAND | (g.speed == kSpeeds[i] ? MF_CHECKED : MF_UNCHECKED));
    for (int i = 0; i < (int)std::size(kTempos); ++i)
        CheckMenuItem(m, IDM_TEMPO_BASE + i, MF_BYCOMMAND | (g.tempo == kTempos[i] ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuRadioItem(m, IDM_REPEAT_BASE, IDM_REPEAT_BASE + (int)Repeat::Count - 1, IDM_REPEAT_BASE + (int)g.repeat, MF_BYCOMMAND);
    // Built-in and pack themes are two id ranges of one radio group.
    for (int t = 0; t < ThemeTotal(); ++t) {
        int item = t < (int)ThemeMode::Count ? IDM_THEME_BASE + t : IDM_PACK_THEME_BASE + t - (int)ThemeMode::Count;
        if (t == (int)g.themeMode) CheckMenuRadioItem(m, item, item, item, MF_BYCOMMAND);
        else CheckMenuItem(m, item, MF_BYCOMMAND | MF_UNCHECKED);
    }
    CheckMenuRadioItem(m, IDM_SIZE_BASE, IDM_SIZE_BASE + (int)ViewSize::Count - 1, IDM_SIZE_BASE + (int)g.size, MF_BYCOMMAND);
    for (int i = 0; i < kPanelCount; ++i)
        CheckMenuItem(m, IDM_PANEL_BASE + i, MF_BYCOMMAND | (PanelOpen(i) ? MF_CHECKED : MF_UNCHECKED));
    UpdateLayoutMenu(m);
    UpdateConvertMenu(m);
    bool selected = !SelectedTracks().empty(), editable = Active().Editable();
    const bool manualScan = g.rgRunning && !g.rgAuto;  // the background measurer gives way to the user
    bool removable = Active().kind != PlaylistKind::Library && Active().kind != PlaylistKind::Inbox;
    bool populated = editable && !Active().items.empty();
    std::wstring undoDescription;
    const struct { UINT id; bool enabled; } enables[] = {
        {IDM_REOPEN_PL, !g.closedPlaylists.empty()}, {IDM_RELOAD_TAGS, selected},
        {IDM_QUEUE_CLEAR, !g.queue.empty()}, {IDM_EDIT_AUTO, Active().kind == PlaylistKind::Auto},
        {IDM_RENAME_PL, removable}, {IDM_REMOVE_PL, RadioActive() || removable},
        {IDM_REMOVE, selected && (editable || LibraryActive())}, {IDM_CROP, selected && editable},
        {IDM_CLEAR, populated}, {IDM_REMOVE_DUPES, populated}, {IDM_REMOVE_DEAD, populated},
        {IDM_PROPERTIES, selected}, {IDM_TAG_UNDO, CanUndoTagEdit(undoDescription)},
        {IDM_RG_SCAN_PL, !manualScan}, {IDM_RG_SCAN_SEL, selected && !manualScan},
        {IDM_RG_CLEAR, selected && !manualScan},
        {IDM_RG_WRITE_TAGS, selected && !manualScan},
    };
    for (const auto& item : enables) EnableMenuItem(m, item.id, MF_BYCOMMAND | (item.enabled ? MF_ENABLED : MF_GRAYED));
    ShowWhileRunning(m, IDM_ADD_FOLDER, IDM_CANCEL_IMPORT, L"&Cancel import", g.importRunning);
    ShowWhileRunning(m, IDM_RESCAN, IDM_WATCH_IMPORTED, L"&Watch imported folders", !g.importedFolders.empty());
    ShowWhileRunning(m, IDM_RG_SCAN_PL, IDM_RG_CANCEL, L"&Cancel scan", manualScan);
    ShowWhileRunning(m, IDM_CONVERT, IDM_CONVERT_CANCEL, L"Cancel conversion", ConvertBusy());
    ShowWhileRunning(m, IDM_COPY_TO, IDM_COPY_CANCEL, L"Cancel copy", CopyBusy());
    ShowWhileRunning(m, IDM_BACKUP, IDM_CANCEL_BACKUP, L"Cancel backup operation", BackupBusy());
    const int tool = HelperToolIndex();
    ShowWhileRunning(m, IDM_HELPER_TOOL_BASE + std::max(tool, 0), IDM_HELPER_CANCEL, HelperCancelText().c_str(), tool >= 0);
    UpdateHelperMenu(m);
    EnableMenuItem(m, IDM_COPY_TO, MF_BYCOMMAND | (CopyBusy() ? MF_GRAYED : MF_ENABLED));
}

}  // namespace app
