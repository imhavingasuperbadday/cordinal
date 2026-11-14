// The Linux window's menus: the menu bar, its popups and row menus, and the actions they run.
#include "alternate_ui.h"
#include "big_ui.h"
#include "ui_internal.h"

namespace lx {

MenuItem Item(std::string label, int action, std::string shortcut, bool checked, bool enabled) {
    MenuItem m;
    m.label = std::move(label);
    m.shortcut = std::move(shortcut);
    m.action = action;
    m.checked = checked;
    m.enabled = enabled;
    return m;
}

MenuItem Separator() {
    MenuItem m;
    m.separator = true;
    return m;
}

MenuItem Radio(std::string label, int action, int arg, bool on, std::string shortcut) {
    MenuItem m = Item(std::move(label), action, std::move(shortcut), on);
    m.radio = true;
    m.arg = arg;
    return m;
}

MenuItem Submenu(std::string label, std::vector<MenuItem> items) {
    MenuItem m = Item(std::move(label), kNoAction);
    m.sub = std::move(items);
    return m;
}

// Library > Find missing information: one item per kMissingFields entry, as on Windows.
std::vector<MenuItem> MissingInfoItems() {
    std::vector<MenuItem> items;
    for (size_t i = 0; i < std::size(kMissingFields); ++i) {
        MenuItem item = Item(kMissingFields[i].label, kMissingInfo);
        item.arg = (int)i;
        items.push_back(std::move(item));
    }
    return items;
}

// The search box is the list of gaps: the tracks stay in the Library tab, ready to select and fill
// in with Edit tags (or Edit album tags), and the list shrinks as they are filled.
void ShowMissingInformation(size_t field) {
    if (field >= std::size(kMissingFields)) return;
    int library = -1;
    for (int i = 0; i < (int)g.playlists.size() && library < 0; ++i)
        if (g.playlists[i].kind == PlaylistKind::Library) library = i;
    if (library < 0) return;
    if (g.active != library || InternetRadio().active) SetActiveTab(library);
    g.search = kMissingFields[field].term;
    g.scroll = 0;
    RebuildRows();
    std::wstring what = field ? compat::Widen(kMissingFields[field].label) : L"some basic information";
    if (field) for (wchar_t& c : what) c = (wchar_t)towlower(c);
    const size_t n = g.rows.size();
    ShowMessage(!n ? L"No library track is missing " + what + L"."
                   : std::to_wstring(n) + (n == 1 ? L" track is missing " : L" tracks are missing ") + what +
                         L". Select them and press Alt+Enter to fill it in; clear the search to see everything.");
}

std::vector<MenuItem> CordExportItems() {
    std::vector<MenuItem> items;
    for (unsigned f = 1; f < (unsigned)CordExport::Count; ++f) {
        auto item = Item(ToUtf8(kCordExports[f].label) + "...", kExportCord, {}, false, !CordExportBusy());
        item.arg = (int)f;
        items.push_back(std::move(item));
    }
    return items;
}

std::vector<MenuItem> RateItems(const double* rates, size_t count, double selected, int action) {
    std::vector<MenuItem> items;
    for (size_t i = 0; i < count; ++i) {
        char label[32];
        snprintf(label, sizeof(label), rates[i] == 1 ? "Normal" : "%gx", rates[i]);
        items.push_back(Radio(label, action, (int)i, rates[i] == selected));
    }
    return items;
}

// Playback > ReplayGain, as on Windows: on or off, and what to do to tracks. How it sounds (the
// preamp, how much loud songs keep) is set in Preferences.
std::vector<MenuItem> ReplayGainMenu() {
    bool manual = g.rgRunning && !g.rgAuto, any = !g.rows.empty();
    std::vector<MenuItem> items{Item("Enabled", kRgEnable, {}, g.settings.rg.on), Separator(),
                                Item("Scan selection", kRgScanSel, {}, false, any && !manual),
                                Item("Scan playlist", kRgScanPl, {}, false, any && !manual),
                                Item("Scan library", kRgScanAll, {}, false, replaygain::HasLibraryTracks() && !manual)};
    if (manual) items.push_back(Item("Cancel scan", kRgCancel));  // shown only while one runs
    items.push_back(Separator());
    items.push_back(Item("Clear ReplayGain", kRgClear, {}, false, any && !manual));
    items.push_back(Item("Write ReplayGain tags", kRgWrite, {}, false, any && !manual));
    return items;
}

std::vector<MenuItem> PlaylistDestinations() {
    std::vector<MenuItem> items;
    bool selected = std::any_of(g.selected.begin(), g.selected.end(), [](char value) { return value != 0; });
    for (size_t i = 0; i < g.playlists.size(); ++i)
        if (g.playlists[i].kind == PlaylistKind::Static) {
            MenuItem item = Item(Utf8(g.playlists[i].name), kAddToPlaylist, {}, false, selected);
            item.arg = (int)i;
            items.push_back(std::move(item));
        }
    return items;
}

// The menu bar's menus, with the Windows build's wording, order and shortcuts. `m` indexes ui::kMenuBar.
bool CanvasHasFocusedEdition() {
    uint32_t group = 0, edition = 0;
    return CanvasFocusedEdition(group, edition);
}

std::vector<MenuItem> BarMenu(int m) {
    bool editable = !g.playlists.empty() && Active().Editable(), any = !g.rows.empty();
    switch (m) {
        case 0: {
            std::vector<MenuItem> items{Item("Add files...", kAddFiles, "Ctrl+O"), Item("Import folder once...", kAddFolder, "Ctrl+Shift+O"), Separator(),
                    Item("Open playlist file...", kOpenPlaylistFile), Item("Save playlist as...", kSavePlaylistFile, "Ctrl+S"), Separator(),
                    Item("Export collection...", kExportCollection, {}, false, !CollectionBusy()),
                    Item("Import collection...", kImportCollection, {}, false, !CollectionBusy()), Separator(),
                    Item("Back up library and settings...",kBackup),Item("Restore backup...",kRestore),
                    Item("Import playback statistics...",kStatistics),Separator(),
                    Item("Web server...", kWebServer), Item("Scrobbling...", kListenBrainz),
                    Item("Preferences...", kPreferences, "Ctrl+,"), Separator(), Item("Exit", kExit)};
            if (CollectionRunning()) items.insert(items.begin() + 8, Item(CollectionCancelText(), kCancelCollection));  // shown only while one runs
            return items;
        }
        case 1: {
            bool library = Active().kind == PlaylistKind::Library || Active().kind == PlaylistKind::Inbox;
            std::wstring undo, redo;
            bool canUndo = CanStepPlaylistEdit(false, &undo), canRedo = CanStepPlaylistEdit(true, &redo);
            if (canUndo && CanUndoTagEdit() && Active().history.undo.back().when < NewestTagEditTime()) undo = L"Undo tag edit";
            else if (!canUndo && CanUndoTagEdit()) { canUndo = true; undo = L"Undo tag edit"; }
            std::vector<MenuItem> items{Item(canUndo ? ToUtf8(undo) : "Undo", kUndo, "Ctrl+Z", false, canUndo),
                    Item(canRedo ? ToUtf8(redo) : "Redo", kRedo, "Ctrl+Y", false, canRedo), Separator(),
                    Item("Select all", kSelectAll, "Ctrl+A", false, any), Item("Remove", kRemove, "Del", false, (editable || library) && any),
                    Item("Keep only selected tracks", kKeepSelected, {}, false, editable && !SelectedTracks().empty()),
                    Item("Clear playlist", kClear, {}, false, editable && any), Separator(),
                    Item("Reload selected metadata", kReloadTags, {}, false, !SelectedTracks().empty() && !TagWriteBusy()),
                    Item("Remove duplicates", kRemoveDuplicates, {}, false, editable && any),
                    Item("Remove dead items", kRemoveDead, {}, false, editable && any && !g_playlistFileBusy && !g_playlistImport),
                    Separator(), Item("Search", kFind, "Ctrl+F"), Separator(),
                    Item("Edit tags...", kEditTags, "Alt+Enter", false, g.focus >= 0 && any),
                    Item("Edit album tags...", kEditAlbumTags, {}, false, !SelectedTracks().empty()),
                    Item("Make album...", kMakeAlbum, {}, false, !SelectedTracks().empty()),
                    Item("Undo last tag edit", kUndoTags, {}, false, CanUndoTagEdit())};
            if (g_checkingDead && g_playlistFileBusy) items.insert(items.begin() + 11, Item("Cancel checking dead items", kCancelDead));
            if (auto labels = HelperToolLabels(); !labels.empty()) {  // the tools of the helpers that are on
                std::vector<MenuItem> run;
                for (size_t k = 0; k < labels.size(); ++k) {
                    MenuItem tool = Item(ToUtf8(labels[k]), kHelperTool, {}, false, RunningHelperTool() < 0);
                    tool.arg = (int)k;
                    run.push_back(std::move(tool));
                }
                if (int running = RunningHelperTool(); running >= 0) run.push_back(Item("Cancel " + ToUtf8(labels[running]), kHelperCancel));
                items.push_back(Separator());
                items.push_back(Submenu("Run", std::move(run)));
            }
            return items;
        }
        case 2: {
            Repeat r = g.settings.repeat;
            return {Item("Play / Pause", kPlayPause, "Space"), Item("Stop", kStopAction, {}, false, g.playing != kNone),
                    Item("Mute", kMute, "Ctrl+M", g_muteVolume > 0), Item("Previous", kPrevAction, "Ctrl+Left"),
                    Item("Next", kNextAction, "Ctrl+Right"), Separator(),
                    Item("Shuffle", kShuffleAction, "Ctrl+H", g.settings.shuffle == kShuffleTracks),
                    Item("Shuffle albums", kShuffleAlbumsAction, {}, g.settings.shuffle == kShuffleAlbums),
                    Item("Keep playing like this", kRadio, {}, g.settings.radio),
                    Item("Choose by taste", kTaste, {}, g.settings.taste),
                    Radio("Repeat off", kRepeatSet, (int)Repeat::Off, r == Repeat::Off),
                    Radio("Repeat playlist", kRepeatSet, (int)Repeat::All, r == Repeat::All, "Ctrl+R"),
                    Radio("Repeat track", kRepeatSet, (int)Repeat::One, r == Repeat::One),
                    Item("Stop after current", kStopAfter, {}, g.stopAfterCurrent),
                    Item("Clear the queue", kClearQueue, {}, false, !g.queue.empty()), Separator(),
                    Submenu("ReplayGain", ReplayGainMenu()),
                    Submenu("Speed (pitch follows)", RateItems(kSpeeds, std::size(kSpeeds), g.speed, kSpeed)),
                    Submenu("Tempo (pitch stays)", RateItems(kTempos, std::size(kTempos), g.tempo, kTempo)),
                    Separator(), Item("Equalizer...", kEqualizer, "Ctrl+E"),
                    Submenu("Output", {Radio("Shared (through the sound server)", kShared, 0, !g.settings.exclusive),
                                       Radio("Exclusive (bit-perfect)", kExclusive, 0, g.settings.exclusive)})};
        }
        case 3: {
            auto destinations = PlaylistDestinations();
            std::vector<MenuItem> items{Item("New playlist", kNewPl, "Ctrl+N"),
                    Item("Rename playlist", kRenamePl, "F2", false, Active().kind != PlaylistKind::Library && Active().kind != PlaylistKind::Inbox),
                    Item("Close playlist", kClosePl, "Ctrl+W", false,
                         InternetRadio().active || Active().kind == PlaylistKind::Static || Active().kind == PlaylistKind::Auto ||
                             (Active().kind == PlaylistKind::NowPlaying && g.playing == kNone)),
                    Item("Duplicate playlist", kDuplicatePl, "Ctrl+Shift+D")};
            if (!destinations.empty()) items.push_back(Submenu("Add selection to playlist", std::move(destinations)));
            if (auto packLists = PackAutoPlaylistNames(); !packLists.empty()) {  // the auto-playlists the packs that are on hold
                std::vector<MenuItem> sub;
                for (size_t k = 0; k < packLists.size(); ++k) {
                    MenuItem list = Item(ToUtf8(packLists[k]), kPackPlaylist);
                    list.arg = (int)k;
                    sub.push_back(std::move(list));
                }
                items.push_back(Submenu("Add auto-playlist from a pack", std::move(sub)));
            }
            items.insert(items.end(), {
                    Item("Reopen closed playlist", kReopenPl, "Ctrl+Shift+T", false, !g_closedPlaylists.empty()),
                    Item("Move left", kMovePlLeft, "Ctrl+Shift+PageUp", false, g.active > 1),
                    Item("Move right", kMovePlRight, "Ctrl+Shift+PageDown", false, g.active > 0 && g.active + 1 < (int)g.playlists.size()),
                    Item("Reveal playing track", kReveal, "Ctrl+J", false, g.playing != kNone), Separator(),
                    Item("Next playlist", kNextPl, "Ctrl+Tab"), Item("Previous playlist", kPrevPl, "Ctrl+Shift+Tab"),
                    Separator(), Item("New auto-playlist...", kNewAuto),
                    Item("Edit auto-playlist rules...", kEditAuto, {}, false, Active().kind == PlaylistKind::Auto)});
            return items;
        }
        case 4: {
            std::vector<MenuItem> items{Item("Library folders...", kFolders),
                Item("Rescan folders", kRescan, {}, false, !g.settings.folders.empty()), Separator(),
                Item("Add everything in the Inbox", kAcceptAllInbox, {}, false,
                     std::any_of(g.playlists.begin(), g.playlists.end(), [](const Playlist& p) { return p.kind == PlaylistKind::Inbox; })), Separator(),
                Item("Organize files...", kOrganize, {}, false, !OrganizeBusy()),
                Item("Undo last organize", kOrganizeUndo, {}, false, !OrganizeBusy()), Separator(),
                Item("Convert to .cord...", kConvert, {}, false, !ConvertBusy() && !CordExportBusy() && !TagWriteBusy() && !OrganizeBusy()),
                Submenu("Export .cord", CordExportItems()),
                Item("Undo last conversion", kConvertUndo, {}, false, CanUndoConversion() && !ConvertBusy()), Separator(),
                Item("Copy to folder or device...", kCopyTo, {}, false, !CopyBusy()),
                Item("Sync to iPod...", kIpodSync, {}, false, !IpodBusy()), Separator(),
                Submenu("Album editions", {Item("Group selected albums as editions", kGroupEditions, {}, false,
                                               CanvasSelectedAlbums().size() >= 2),
                                          Item("Album editions...", kAlbumEditions, {}, false,
                                               CanvasHasFocusedEdition()),
                                          Item("Set preferred edition", kPreferredEdition, {}, false,
                                               CanvasHasFocusedEdition()),
                                          Item("Remove edition from group", kRemoveEdition, {}, false,
                                               CanvasHasFocusedEdition()),
                                          Item("Split album group", kSplitEditions, {}, false,
                                               CanvasHasFocusedEdition()),
                                          Item("Other versions...", kEditionVersions, {}, false,
                                               SelectedTracks().size() == 1 &&
                                                   AlbumGroupForTrack(SelectedTracks().front()) != nullptr)}),
                Separator(), Submenu("Find missing information", MissingInfoItems()),
                Item("Fetch missing covers...", kFetchCovers, {}, false, !CoverFetchBusy()),
                Item("Remove recent additions (24 hours)...", kRecentAdditions),
                Item("Find duplicate audio", kDuplicateAudio, {}, false, !g_playlistFileBusy && !g_playlistImport),
                Item("Add Steam soundtracks", kSteam, {}, false, !g_playlistFileBusy && !g_playlistImport)};
            if (ConvertBusy()) items.push_back(Item("Cancel conversion", kCancelConvert));
            if (CopyBusy()) items.push_back(Item("Cancel copy", kCancelCopy));
            if (IpodBusy()) items.push_back(Item("Cancel iPod sync", kCancelIpodSync));
            if (CoverFetchBusy()) items.push_back(Item("Cancel cover search", kCancelFetchCovers));
            if (CordExportBusy()) items.push_back(Item("Cancel export", kCancelExport));
            if (g_checkingDuplicates && g_playlistFileBusy) items.push_back(Item("Cancel duplicate search", kCancelDuplicates));
            if (g_checkingSteam && g_playlistFileBusy) items.push_back(Item("Cancel soundtrack search", kCancelSteam));
            return items;
        }
        case 5: {
            // As View > Theme on Windows: Light and Dark, then following the system, then the themed palettes.
            std::vector<MenuItem> themes;
            for (ThemeMode t : {ThemeMode::Light, ThemeMode::Dark, ThemeMode::System})
                themes.push_back(Radio(t == ThemeMode::System ? "Follow the system" : Plain(ThemeInfo(t).name), kTheme, (int)t,
                                       g.settings.theme == t));
            themes.push_back(Separator());
            for (int t = (int)ThemeMode::Deltarune; t < ThemeTotal(); ++t)
                if (ThemeAvailable((ThemeMode)t)) themes.push_back(Radio(Plain(ThemeInfo((ThemeMode)t).name), kTheme, t, (int)g.settings.theme == t));
            std::vector<MenuItem> view = {Submenu("Theme", std::move(themes)), Item("Radio", kOpenRadio, "Ctrl+Shift+R"), Item("Lyrics panel", kLyrics, "Ctrl+L", LyricsShown()), Item("Up next", kQueuePanel, "Ctrl+Shift+Q", QueueShown()), Item("Artist info...", kArtistInfo), Item("Customize layout...", kLayout),
                    Submenu("Window size", {Radio("Compact", kViewSize, 0, g.settings.size == ViewSize::Compact),
                        Radio("Normal", kViewSize, 1, g.settings.size == ViewSize::Normal),
                        Radio("Big mode plus", kViewSize, 2, g.settings.size == ViewSize::Spotlight),
                        Radio("Big mode", kViewSize, 3, g.settings.size == ViewSize::Big)}), Submenu("Big mode panels", {Item("Library", kBigPanel, {}, BigPanelShown(0)),
                        Radio("Now playing", kBigPanel, 1, BigPanelShown(1)),
                        Radio("Up next", kBigPanel, 2, BigPanelShown(2)),
                        Item("Reset panel layout", kBigReset)}), Item("Mini player", kMini, "Ctrl+Shift+M"), Item("Packs...", kPacks), Item("Helpers...", kHelpers), Item("Command palette...", kPalette, "Ctrl+Shift+P"), Separator(), Item("Group tracks by album", kGroupAlbums, {}, g.settings.groupAlbums),
                    Separator(), Item("Start on login", kStartOnLogin, {}, StartsOnLogin()), Separator(),
                    Submenu("Games and stats", {Item("Name That Track...", kNameThatTrack), Item("Which Came First...", kWhichCameFirst), Separator(),
                                                Item("cordinal points...", kPoints), Item("Year in review...", kReview)})};
            auto appearance = WaveAppearanceMenu();
            if (!appearance.empty()) view.insert(view.begin() + 4, Submenu("Waveform appearance", std::move(appearance)));
            return view;
        }
        case 6: return {Item("About cordinal", kAbout)};
        default: return {};
    }
}

// The menus the bar shows, left to right: all of them.
std::vector<int> BarMenus() {
    std::vector<int> menus;
    for (int m = 0; m < (int)std::size(ui::kMenuBar); ++m) menus.push_back(m);
    return menus;
}

float PopupItemHeight(const MenuItem& it) { return S(it.separator ? kSeparatorH : kItemH); }

void OpenPopup(float x, float y, std::vector<MenuItem> items, bool submenu) {
    if (!submenu) g_popups.clear();
    Popup p;
    float label = 0, shortcut = 0;
    bool arrow = false;
    for (auto& it : items) {
        label = std::max(label, TextWidth(Font::Menu, it.label));
        shortcut = std::max(shortcut, TextWidth(Font::Menu, it.shortcut));
        arrow |= !it.sub.empty();
        p.h += PopupItemHeight(it);
    }
    p.w = S(kLabelX) + label + (shortcut > 0 ? S(36) + shortcut : 0) + S(kShortcutRight) + (arrow ? S(12) : 0);
    p.w = std::max(p.w, S(130));
    p.h += 2 * S(kMenuPad);
    int pw = 0, ph = 0;
    SDL_GetRenderOutputSize(g.renderer, &pw, &ph);
    p.x = std::max(0.0f, std::min(x, pw - p.w - S(2)));
    p.y = std::max(0.0f, std::min(y, ph - p.h - S(2)));
    p.items = std::move(items);
    g_popups.push_back(std::move(p));
}

Rect PopupItemRect(const Popup& p, size_t i) {
    float y = p.y + S(kMenuPad);
    for (size_t k = 0; k < i; ++k) y += PopupItemHeight(p.items[k]);
    return {p.x, y, p.w, PopupItemHeight(p.items[i])};
}

void CloseMenus() {
    g_popups.clear();
    g_barOpen = -1;
    g.redraw = true;
}

void OpenBarMenu(int index) {  // index into L.barItems
    auto menus = BarMenus();
    if (index < 0 || index >= (int)menus.size()) return;
    g_barOpen = index;
    const Rect r = index < (int)L.barItems.size() ? L.barItems[index] : Rect{0, 0, 0, 0};
    OpenPopup(r.x, L.menuBar.B() - S(1), BarMenu(menus[index]));
    g.redraw = true;
}

// The lyrics panel's right-click menu: fetched lyrics are kept only when one of these is chosen.
void OpenLyricsMenu(float x, float y) {
    g_barOpen = -1;
    OpenPopup(x, y, {Item("Save lyrics to tags", kSaveLyricsTags, {}, false, CanSaveFetchedLyrics(true)),
                     Item("Save as .lrc file", kSaveLyricsLrc, {}, false, CanSaveFetchedLyrics(false))});
}

void OpenRowMenu(float x, float y) {
    bool np = Active().kind == PlaylistKind::NowPlaying;
    std::vector<MenuItem> items{Item("Play", kPlay), Item("Play next", kPlayNext, "Ctrl+Enter"), Item("Add to queue", kAddToQueue, "Q"),
                                 Item("Add to Now Playing", kAddToNowPlaying, "Shift+Q")};
    if (np) items.push_back(Item("Remove from Now Playing", kRemoveFromNowPlaying, "Del"));
    if (Active().kind == PlaylistKind::Inbox) items.push_back(Item("Add to library", kAcceptInbox));
    if (Active().kind == PlaylistKind::Inbox || Active().kind == PlaylistKind::Library)
        items.push_back(Item("Remove from library...", kExcludeLibrary, "Del"));
    auto destinations = PlaylistDestinations();
    if (!destinations.empty()) items.push_back(Submenu("Add to playlist", std::move(destinations)));
    items.push_back(Separator());
    std::vector<MenuItem> ratings;
    MenuItem none = Item("No rating", kRating); none.arg = 0; ratings.push_back(std::move(none));
    for (int stars = 1; stars <= 5; ++stars) {
        MenuItem item = Item(std::to_string(stars) + (stars == 1 ? " star" : " stars"), kRating);
        item.arg = stars * 20;
        ratings.push_back(std::move(item));
    }
    ratings.push_back(Separator());
    ratings.push_back(Item("Write ratings and play counts to files", kWriteStatistics, {}, false, !TagWriteBusy()));
    items.push_back(Submenu("Rating", std::move(ratings)));
    bool manual = g.rgRunning && !g.rgAuto;
    items.push_back(Item("Scan ReplayGain", kRgScanSel, {}, false, !manual));
    items.push_back(Item("Write ReplayGain tags", kRgWrite, {}, false, !manual));
    items.push_back(Item("Clear ReplayGain", kRgClear, {}, false, !manual));
    items.push_back(Item("Open containing folder", kOpenFolder));
    items.push_back(Item("Organize files...", kOrganize, {}, false, !OrganizeBusy()));
    items.push_back(Item("Convert to .cord...", kConvert, {}, false, !ConvertBusy() && !CordExportBusy() && !TagWriteBusy()));
    items.push_back(Submenu("Export .cord", CordExportItems()));
    items.push_back(Item("Copy to folder or device...", kCopyTo, {}, false, !CopyBusy()));
    items.push_back(Item("Sync to iPod...", kIpodSync, {}, false, !IpodBusy()));
    items.push_back(Item("Edit tags...", kEditTags, "Alt+Enter"));
    items.push_back(Item("Edit album tags...", kEditAlbumTags));
    items.push_back(Item("Make album...", kMakeAlbum));
    // The cover as a submenu: a file or the clipboard's picture becomes the album's cover at once;
    // only an online search needs the picker. Dropping an image on a row does the same.
    std::vector<MenuItem> cover{Item("Choose image file...", kCoverFile),
                                Item("Paste image from clipboard", kCoverPaste, {}, false, ClipboardMayHoldCover()),
                                Item("Search online...", kFindCover)};
    if (HasCustomCover(SelectedTracks())) {
        cover.push_back(Separator());
        cover.push_back(Item("Restore original cover", kRestoreCover));
    }
    items.push_back(Submenu("Cover", std::move(cover)));
    items.push_back(Item("Artist info...", kArtistInfo));
    g_barOpen = -1;
    OpenPopup(x, y, std::move(items));
}

// The shared auto-playlist tab's menu, under it (app_tabs.cpp ShowAutoTabMenu): every auto-playlist,
// the open one checked, then New auto-playlist and Edit rules.
void OpenAutoTabMenu(int tab) {
    if (tab < 0 || tab >= (int)L.tabRects.size() || !IsAutoTab(tab)) return;
    std::vector<MenuItem> items;
    for (int i = 0; i < (int)g.playlists.size(); ++i)
        if (g.playlists[i].kind == PlaylistKind::Auto) {
            std::string label = Utf8(g.playlists[i].name);
            items.push_back(Radio(label, kChoice, i, i == g.active,
                                  g.settings.layout.tabCounts ? std::to_string(PlaylistTracks(g.playlists[i]).size()) : ""));
        }
    items.push_back(Separator());
    items.push_back(Item("New auto-playlist...", kNewAuto));
    if (L.tabSlots[tab] == g.active) items.push_back(Item("Edit rules...", kEditAuto));
    g_choicePick = [](int index) { SetActiveTab(index); };
    g_barOpen = -1;
    OpenPopup(L.tabRects[tab].x, L.tabRects[tab].B(), std::move(items));
    g.redraw = true;
}

void OpenHeaderMenu(float x, float y) {
    std::vector<MenuItem> items;
    for (int c = 1; c < ColumnTotal(); ++c) {
        if (c >= ui::kColCover && c <= ui::kColApolloTag) continue;  // the private themes' own columns
        bool shown = std::find(ActiveColumnTypes().begin(), ActiveColumnTypes().end(), c) != ActiveColumnTypes().end();
        MenuItem it = Item(Utf8(ColumnAt(c).name), kColumnToggle, {}, shown, c != 2);
        it.arg = c;
        items.push_back(std::move(it));
    }
    const auto sets = PackColumnSetNames();
    for (size_t i = 0; i < sets.size(); ++i) {
        MenuItem it = Item(Utf8(sets[i]), kPackColumnSet);
        it.arg = (int)i;
        items.push_back(std::move(it));
    }
    g_barOpen = -1;
    OpenPopup(x, y, std::move(items));
}

void OpenChoice(float x, float y, const std::vector<std::string>& labels, int selected, std::function<void(int)> pick) {
    std::vector<MenuItem> items;
    for (size_t i = 0; i < labels.size(); ++i) items.push_back(Radio(labels[i], kChoice, (int)i, (int)i == selected));
    g_choicePick = std::move(pick);
    OpenPopup(x, y, std::move(items));
    g.redraw = true;
}

void RunAction(const MenuItem& item) {
    if (RunAlternateAction(item.action, item.arg)) return;
    switch (item.action) {
        case kAddFiles: ChooseFiles(); break;
        case kOpenPlaylistFile: ChoosePlaylistFile(false); break;
        case kSavePlaylistFile: ChoosePlaylistFile(true); break;
        case kBackup: ChooseMigration(1); break;
        case kRestore: ChooseMigration(2); break;
        case kStatistics: ChooseMigration(3); break;
        case kExportCollection: ExportCollection(); break;
        case kImportCollection: ImportCollection(); break;
        case kCancelCollection: CancelCollection(); break;
        case kAddFolder: ChooseImportFolder(); break;
        case kExit: g.quit = true; break;
        case kSelectAll: std::fill(g.selected.begin(), g.selected.end(), 1); break;
        case kRemove:
            if (Active().kind == PlaylistKind::Library || Active().kind == PlaylistKind::Inbox) ExcludeFromLibrary(SelectedTracks());
            else RemoveFromPlaylist(false);
            break;
        case kKeepSelected: RemoveFromPlaylist(false, true); break;
        case kClear: RemoveFromPlaylist(true); break;
        case kReloadTags: {
            auto ids = SelectedTracks();
            if (TagWriteBusy()) { ShowMessage(L"Tags are still being written; try again in a moment."); break; }
            for (TrackId id : ids) g_lib[id].flags &= (uint16_t)~(Track::kLoaded | Track::kTagFailed);
            QueueTags(ids);
            if (std::find(ids.begin(), ids.end(), g.playing) != ids.end()) QueueChapters(g.playing);
            if (!ids.empty()) ShowMessage(L"Reloading metadata...");
            break;
        }
        case kRemoveDuplicates: RemoveDuplicateTracks(); break;
        case kRemoveDead: CheckDeadItems(); break;
        case kCancelDead: g_playlistFileCancel = true; break;
        case kFind:
            g.searchFocused = true;
            SDL_StartTextInput(g.window);
            break;
        case kPlayPause: TogglePause(); break;
        case kStopAction:
            HoldPendingOpens();
            Stop();
            break;
        case kMute: ToggleMute(); break;
        case kPrevAction: Previous(); break;
        case kNextAction: Next(); break;
        case kShuffleAction: SetShuffle(g.settings.shuffle == kShuffleTracks ? kShuffleOff : kShuffleTracks); break;
        case kShuffleAlbumsAction: SetShuffle(g.settings.shuffle == kShuffleAlbums ? kShuffleOff : kShuffleAlbums); break;
        case kRadio: SetRadio(!g.settings.radio); break;
        case kTaste: SetTaste(!g.settings.taste); break;
        case kRepeatSet:
            SetRepeat((Repeat)item.arg);
            break;
        case kSpeed:
            if (item.arg >= 0 && item.arg < (int)std::size(kSpeeds)) SetSpeed(kSpeeds[item.arg]);
            break;
        case kTempo:
            if (item.arg >= 0 && item.arg < (int)std::size(kTempos)) SetTempo(kTempos[item.arg]);
            break;
        case kStopAfter: ToggleStopAfterCurrent(); break;
        case kShared: SetExclusive(false); break;
        case kExclusive: SetExclusive(true); break;
        case kNewPl: NewPlaylist(); break;
        case kRenamePl: BeginRename(g.active); break;
        case kClosePl: if (InternetRadio().active) CloseRadio(); else ClosePlaylist(g.active); break;
        case kDuplicatePl: DuplicatePlaylist(); break;
        case kReopenPl: ReopenPlaylist(); break;
        case kMovePlLeft: ReorderPlaylist(-1); break;
        case kMovePlRight: ReorderPlaylist(1); break;
        case kNextPl: SetActiveTab((g.active + 1) % (int)g.playlists.size()); break;
        case kPrevPl: SetActiveTab((g.active + (int)g.playlists.size() - 1) % (int)g.playlists.size()); break;
        case kReveal: RevealPlaying(); break;
        case kFolders: OpenLibraryFolders(); break;
        case kRescan:
            g.scanner.RescanAll();
            g.scanning = true;
            break;
        case kAcceptInbox: AcceptInbox(SelectedTracks()); break;
        case kAcceptAllInbox: {
            std::vector<TrackId> all;
            for (TrackId id = 0; id < (TrackId)g_lib.Count(); ++id)
                if (g_lib[id].flags & Track::kInbox) all.push_back(id);
            AcceptInbox(all);
            break;
        }
        case kExcludeLibrary: ExcludeFromLibrary(SelectedTracks()); break;
        case kRating: SetRating(SelectedTracks(), (uint8_t)std::clamp(item.arg, 0, 100)); break;
        case kSectionPlay: case kSectionLoop: case kSectionStart: case kSectionEnd: case kSectionClear:
            SectionAction(item.action); break;
        case kDuplicateAudio: FindDuplicateAudio(); break;
        case kCancelDuplicates: g_playlistFileCancel = true; break;
        case kSteam: AddSteamSoundtracks(); break;
        case kFetchCovers: FetchMissingCovers(); break;
        case kRecentAdditions: OpenRecentAdditions(); break;
        case kCancelFetchCovers: CancelCoverFetch(); break;
        case kFindCover: OpenCoverSearch(SelectedTracks()); break;
        case kRestoreCover: RestoreOriginalCovers(SelectedTracks()); break;
        case kCoverFile: ChooseCoverFile(SelectedTracks()); break;
        case kCoverPaste: PasteCover(SelectedTracks()); break;
        case kEditAlbumTags:
            if (auto sel = SelectedTracks(); !sel.empty()) OpenTagEditor(AlbumTracksOf(sel));
            break;
        case kMakeAlbum: OpenAlbumMaker(SelectedTracks()); break;
        case kMissingInfo: ShowMissingInformation((size_t)std::max(0, item.arg)); break;
        case kSaveLyricsTags: SaveFetchedLyrics(true); break;
        case kSaveLyricsLrc: SaveFetchedLyrics(false); break;
        case kCancelSteam: g_playlistFileCancel = true; break;
        case kWebServer: OpenWebServer(); break;
        case kExportCord: ExportCordTracks(SelectedTracks(), (CordExport)item.arg); break;
        case kCancelExport: CancelCordExport(); break;
        case kConvert: ConvertSelected(SelectedTracks()); break;
        case kConvertUndo: UndoConversion(); break;
        case kOrganize: OpenOrganize(SelectedTracks()); break;
        case kOrganizeUndo: UndoOrganize(); break;
        case kCopyTo: {
            // The selection, or the whole playlist when nothing is selected.
            std::vector<TrackId> ids = SelectedTracks();
            if (ids.empty()) OpenCopy(Active().items, Active().name);
            else OpenCopy(ids, L"");
            break;
        }
        case kCancelCopy: CancelCopy(); break;
        case kIpodSync: StartIpodSync(); break;
        case kCancelIpodSync: CancelIpodSync(); break;
        case kCancelConvert: CancelConversion(); break;
        case kNewAuto: OpenRules(-1); break;
        case kEditAuto: OpenRules(g.active); break;
        case kChoice: {
            auto pick = g_choicePick;
            if (pick) pick(item.arg);
            break;
        }
        case kPreferences: OpenPreferences(); break;
        case kListenBrainz: OpenListenBrainz(); break;
        case kTheme: SetTheme((ThemeMode)item.arg); break;
        case kGroupAlbums:
            g.settings.groupAlbums = !g.settings.groupAlbums;
            SaveSettings();
            RebuildRows();
            break;
        case kGroupEditions: {
            auto albums = CanvasSelectedAlbums();
            std::wstring error;
            if (GroupAlbumsAsEditions(albums, &error)) {
                InvalidateCellCache();
                RebuildRows();
                ShowMessage(L"Grouped " + std::to_wstring(albums.size()) + L" albums as editions.");
            } else {
                ShowMessage(error.empty() ? L"Select two or more album tiles to group them." : error);
            }
            break;
        }
        case kAlbumEditions: {
            uint32_t group = 0, edition = 0;
            if (CanvasFocusedEdition(group, edition)) OpenAlbumEditions(group);
            break;
        }
        case kEditionVersions: {
            auto tracks = SelectedTracks();
            if (tracks.size() == 1) {
                if (const AlbumGroup* group = AlbumGroupForTrack(tracks.front()))
                    OpenAlbumEditionComparison(group->id, tracks.front());
            }
            break;
        }

        case kPreferredEdition: {
            uint32_t group = 0, edition = 0;
            if (CanvasFocusedEdition(group, edition) && SetPreferredEdition(group, edition)) {
                CanvasChooseEdition(group, edition);
                InvalidateCellCache();
                RebuildRows();
                ShowMessage(L"Preferred edition set.");
            }
            break;
        }
        case kRemoveEdition: {
            uint32_t group = 0, edition = 0;
            if (CanvasFocusedEdition(group, edition) && RemoveEditionFromGroup(group, edition)) {
                InvalidateCellCache();
                RebuildRows();
            }
            break;
        }
        case kSplitEditions: {
            uint32_t group = 0, edition = 0;
            if (CanvasFocusedEdition(group, edition) && SplitAlbumGroup(group)) {
                InvalidateCellCache();
                RebuildRows();
            }
            break;
        }
        case kStartOnLogin: ToggleStartOnLogin(); break;
        case kAbout: g_aboutOpen = true; break;
        case kNameThatTrack: OpenGames(0); break;
        case kWhichCameFirst: OpenGames(1); break;
        case kPoints: OpenPoints(); break;
        case kPalette: OpenPalette(); break;
        case kLyrics: ToggleLyrics(); break;
        case kQueuePanel: ToggleQueue(); break;
        case kQueueEdit: QueueEdit(item.arg); break;
        case kArtistInfo: OpenArtistInfo(); break;
        case kOpenRadio: OpenRadio(); break;
        case kBigPanel: ToggleBigPanel(item.arg); break;
        case kBigReset: ResetBigPanels(); break;
        case kViewSize: SetViewSize((ViewSize)item.arg); break;
        case kLayout: OpenLayout(); break;
        case kPacks: OpenPacks(); break;
        case kPackPlaylist: AddPackAutoPlaylist((size_t)item.arg); break;
        case kHelpers: OpenHelpers(); break;
        case kHelperTool: RunHelperTool((size_t)item.arg, SelectedTracks()); break;
        case kHelperCancel: CancelHelperTool(); break;
        case kMini: CloseMenus(); ClosePanel(); ToggleMini(); break;
        case kReview: OpenReview(); break;
        case kPlay: {
            auto sel = SelectedTracks();
            if (g.focus >= 0 && sel.size() <= 1) PlayRow(g.focus);
            else
                for (size_t i = 0; i < g.selected.size(); ++i)
                    if (g.selected[i]) {
                        PlayRow((int)i);
                        break;
                    }
            break;
        }
        case kPlayNext: QueueTracks(SelectedTracks(), true); break;
        case kAddToQueue: QueueTracks(SelectedTracks(), false); break;
        case kClearQueue: ClearQueue(); break;
        case kAddToNowPlaying: InsertIntoNowPlaying(SelectedTracks(), false); break;
        case kAddToPlaylist: AddToPlaylist(item.arg, SelectedTracks()); break;
        case kRemoveFromNowPlaying: RemoveFromPlaylist(false); break;
        case kOpenFolder:
            if (g.focus >= 0 && g.focus < (int)g.rows.size()) ShowFolder(g.rows[g.focus]);
            break;
        case kEditTags: OpenTagEditor(SelectedTracks()); break;
        case kUndoTags: UndoLastTagEdit(); break;
        case kUndo: UndoLatest(); break;
        case kRedo: StepPlaylistEdit(true); break;
        case kEqualizer: OpenEqualizer(); break;
        case kRgEnable: SetReplayGain(!g.settings.rg.on); break;
        case kRgScanSel: ScanReplayGain(SelectedTracks()); break;
        case kRgScanPl: ScanReplayGain(PlaylistTracks(Active())); break;
        case kRgScanAll: {
            auto ids = replaygain::LibraryTracks();
            if (ids.empty()) ShowMessage(L"There are no available library tracks to scan.");
            else ScanReplayGain(ids);
            break;
        }
        case kRgCancel: CancelReplayGain(); break;
        case kRgClear: ClearReplayGain(SelectedTracks()); break;
        case kRgWrite: WriteReplayGainTags(SelectedTracks()); break;
        case kWriteStatistics: WriteStatisticsTags(SelectedTracks()); break;
        case kPackColumnSet: ApplyPackColumnSet(item.arg); break;
        case kColumnToggle: {
            auto& cols = ActiveColumnTypes();
            auto it = std::find(cols.begin(), cols.end(), item.arg);
            if (it != cols.end()) cols.erase(it);
            else cols.push_back(item.arg);
            SaveSettings();
            break;
        }
    }
    g.redraw = true;
}

void SectionAction(int action) {
    double duration = TrackDuration();
    if (g.playing != g_sectionMenuTrack || !g_lib.Contains(g.playing) || duration <= 0) return;
    double position = std::clamp(g_sectionMenuPos, 0.0, duration);
    if (action == kSectionPlay) { SeekTo(position); return; }
    PlaybackSection section = SectionOf(g.playing);
    if (action == kSectionLoop) {
        if (section.end - section.start <= 0.05) {
            double end = g.chapterTrack == g.playing ? ChapterStartAfter(g.chapters, position) : 0;
            if (end <= position + 0.5) end = position + 30;
            section = {position, std::min(end, duration), false};
        }
        if (section.end - section.start <= 0.05) { ShowMessage(L"Choose a point before the end of the track."); return; }
        section.on = true;
    } else if (action == kSectionStart) {
        section.start = position;
        if (section.end <= position + 0.05) section.end = std::min(position + 30, duration);
        section.on = false;
    } else if (action == kSectionEnd) {
        section.end = position;
        if (section.start >= position - 0.05) section.start = std::max(0.0, position - 30);
        section.on = false;
    } else if (action == kSectionClear) section = {};
    else return;
    SetSectionFor(g.playing, section);
    double seek = g.engine.Position();
    if (section.on && seek < section.start) seek = section.start;
    SeekTo(seek);
    ShowMessage(section.on ? L"Looping the section." : action == kSectionClear ? L"Section cleared." : L"Section marked.");
}

void ShowFolder(TrackId id) {
    std::wstring path = g_lib.Path(id), file;
    int index = 0;
    if (SplitSubtrack(path, file, index)) path = file;
    std::string url = "file://" + Utf8(DirectoryOf(path));
    SDL_OpenURL(url.c_str());
}

void InsertIntoNowPlaying(const std::vector<TrackId>& ids, bool next) {
    if (ids.empty()) return;
    int np = EnsureNowPlayingTab();
    auto& items = g.playlists[np].items;
    size_t at = next && g.queuePos >= 0 && g.queuePos < (int)items.size() ? (size_t)g.queuePos + 1 : items.size();
    std::vector<size_t> mapping(items.size());
    for (size_t i = 0; i < mapping.size(); ++i) mapping[i] = i < at ? i : i + ids.size();
    std::vector<TrackId> before = items;
    items.insert(items.begin() + (ptrdiff_t)at, ids.begin(), ids.end());
    RecordPlaylistEdit(g.playlists[np], std::move(before), L"add");
    RemapNowPlaying(mapping);
    g.libraryDirty = true;
    if (g.active == np) RebuildRows();
    ShowMessage(std::to_wstring(ids.size()) + (ids.size() == 1 ? L" track" : L" tracks") + (next ? L" will play next." : L" added to Now Playing."));
}

void AddToPlaylist(int index, const std::vector<TrackId>& ids) {
    if (index < 0 || index >= (int)g.playlists.size() || g.playlists[index].kind != PlaylistKind::Static || ids.empty()) return;
    auto& items = g.playlists[index].items;
    std::vector<TrackId> before = items;
    items.insert(items.end(), ids.begin(), ids.end());
    RecordPlaylistEdit(g.playlists[index], std::move(before), L"add");
    g.libraryDirty = true;
    if (index == g.active) RebuildRows();
    ShowMessage(std::to_wstring(ids.size()) + (ids.size() == 1 ? L" track added to " : L" tracks added to ") + g.playlists[index].name + L".");
}

void SetTheme(ThemeMode mode) {
    if (!ThemeAvailable(mode)) return;
    g.settings.theme = mode;
    SaveSettings();
    ApplyTheme();
    RebuildRows();
}

}  // namespace lx
