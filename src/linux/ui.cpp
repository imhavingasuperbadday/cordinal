// The Linux front end's public surface, the half main.cpp drives (ui.h): the state one window shares,
// the theme and title, covers and waveform requests, and the messages workers post. The parts live in
// ui_layout.cpp, ui_model.cpp, ui_menus.cpp, ui_dialogs.cpp, ui_draw.cpp, ui_input.cpp, ui_files.cpp and
// ui_checks.cpp; ui_internal.h holds the types, state and declarations they share. The window looks like
// the Windows build's normal window in every theme (docs/ui-reference/windows holds its screenshots;
// ui_spec.h its measurements, shared with Windows):
//
//   menu bar: File  Edit  Playback  Playlist  Library  View  Help
//   handle | prev play next  shuffle repeat resume | elapsed | seek bar | remaining | speaker volume
//   cover  title / artist  •  album  •  year
//   tabs (Library, Now Playing, playlists...)                                         | search
//   view row (Library tab): Tracks, Albums & Tracks, Albums, Artists | Browser
//   the column browser (Library tab, when shown)
//   column headers, then the track list with album headers over each album's tracks (or a view, libview_canvas.cpp)
//   status: message or playback | work in progress | track count and length
//
// Everything is drawn with the SDL renderer, laid out in pixels from the display scale. Where Windows
// uses native controls (menus, the search box, the scroll bar) this draws their Windows 11 look.
// Seek and volume tracks and handles stay centred together, using ui_spec.h's slider measurements.
// Features the Linux build doesn't have are left out of the menus rather than shown disabled.
#include "alternate_ui.h"
#include "ui.h"
#include "ui_internal.h"

#include "app.h"
#include "archive.h"
#include "cue.h"
#include "draw.h"
#include "libview.h"
#include "panels.h"
#include "qr.h"
#include "peaks.h"
#include "points.h"
#include "radio_ui.h"
#include "steam.h"

#include "text.h"
#include "titleformat.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cwctype>
#include <map>
#include <numeric>
#include <unordered_map>
#include <unordered_set>

namespace lx {

// --- geometry ------------------------------------------------------------------------------------
Layout L;

// What the mouse is doing.
uint64_t g_lastInput = 0;  // when the last key or mouse event arrived (preferences_ui.cpp waits for quiet)
Drag g_drag = Drag::None;
int g_dragColumn = -1;
float g_dragStart = 0, g_dragStartValue = 0;
double g_seekPreview = -1;  // while dragging the seek bar
float g_mouseX = -1, g_mouseY = -1;
uint64_t g_lastClick = 0;
int g_lastClickLine = -1;
bool g_altAlone = false;  // Alt went down with nothing else: its release opens the menu bar
float g_muteVolume = 0;   // the volume before Mute, 0 while not muted
TrackId g_sectionMenuTrack = kNone;
double g_sectionMenuPos = 0;
TrackDrag g_trackDrag;

// --- album groups ------------------------------------------------------------------------------------
// Consecutive tracks of one album (two or more) under a two-row header, as on Windows
// (app_groups.cpp). A display line is a track row (`row` >= 0) or a header's half (`row` < 0).
std::vector<Group> g_groups;
std::vector<Line> g_lines;
std::vector<int> g_lineOfRow;

// Album covers (the list's album headers and the views), by the album, its first track's folder and
// the size asked for. The views can ask for thousands, so the least recently drawn are let go past
// kThumbsKept.
uint64_t g_frame = 0;
std::unordered_map<uint64_t, Thumb> g_thumbs;
std::unordered_map<TrackId, uint64_t> g_thumbFor;  // requests in flight
int g_thumbsPending = 0;

// Popup menus: the menu bar's, the list's and the header's. A stack: [0] is the menu, and each
// open submenu follows it.
std::function<void(int)> g_choicePick;  // the choice a panel's OpenChoice menu is for
std::vector<Popup> g_popups;
int g_barOpen = -1;  // the menu bar's open menu, or -1

std::deque<ClosedPlaylist> g_closedPlaylists;
int g_renameIndex = -1;
std::wstring g_renameText;
bool g_renameSelected = false;
size_t g_renameCaret = 0;
uint64_t g_nextPlaylistIdentity = 1;

std::thread g_playlistFileWorker;
std::thread g_steamOfferWorker;
bool g_playlistFileBusy = false, g_playlistFileStopping = false;
bool g_checkingDead = false, g_checkingDuplicates = false, g_checkingSteam = false;
std::atomic<bool> g_playlistFileCancel{false};
std::atomic<bool> g_steamOfferCancel{false};
std::unique_ptr<PlaylistFile> g_playlistImport;

// The library folders dialog.
Folders g_folders;
ExcludeConfirm g_exclude;
RecentPanel g_recent;
bool g_aboutOpen = false;
// The Web server window (File > Web server): its controls are 0 the port, 1 Start/Stop, 2 Start with cordinal,
// 3 Pair device, 4 Remove all, 5 Close, 6 Open web player, 7 Copy phone address, 8 Copy code, 9 Listen away
// from home, 10 Check again, 11 Copy internet address, 12 the fixed code and 13 Use this code.
WebPanel g_web;

// The seek bar's waveform (app_waveform.cpp): the playing track's peaks, computed in the background
// the first time it plays and cached; its columns fill as the worker decodes an uncached track.
Wave g_wave;

SDL_Texture* g_cover = nullptr;
SDL_Texture* g_disc = nullptr;  // the cover's disc art, when its album has some
TrackId g_coverFor = kNone, g_coverWanted = kNone;
double g_coverHue = -1;  // the cover's hue (CoverHue), which the player's background takes on
uint32_t g_lastRebuild = 0;
size_t g_sourceCount = 0;  // active tab's tracks before the search
std::unordered_map<uint64_t, std::string> g_cells;  // formatted cells by track and column
uint32_t g_cellsRevision = 0;
AudioVisuals g_visuals;
uint64_t g_visualAt = 0;

// Help > About cordinal, as the Windows box says it (app_dialogs.cpp): the cardinal, which sings when
// clicked (with sound effects on or off), what cordinal is, and what it is using now.
AboutBox g_about;

std::map<std::string, SDL_Texture*> g_radioArt;  // radio station icons by URL

// --- public ------------------------------------------------------------------------------------------------
namespace {
std::vector<MenuItem> g_paletteItems;  // the items MenuCommands listed, for RunMenuCommand

void FlattenMenu(const std::vector<MenuItem>& items, const std::string& path, std::vector<PaletteCommand>& out) {
    for (const MenuItem& item : items) {
        if (item.separator) continue;
        if (!item.sub.empty()) {
            FlattenMenu(item.sub, path.empty() ? item.label : path + " \xE2\x80\xBA " + item.label, out);
        } else if (item.action != kNoAction && item.action != kPalette) {
            out.push_back({item.label, path, item.shortcut, item.enabled, item.checked});
            g_paletteItems.push_back(item);
        }
    }
}
}  // namespace

std::vector<PaletteCommand> MenuCommands() {
    std::vector<PaletteCommand> out;
    g_paletteItems.clear();
    for (int m : BarMenus()) FlattenMenu(BarMenu(m), Plain(ui::kMenuBar[m]), out);
    return out;
}

void RunMenuCommand(size_t index) {
    if (index < g_paletteItems.size()) RunAction(g_paletteItems[index]);
}

uint64_t LastInputTicks() { return g_lastInput; }

SDL_Texture* MiniCover() {
    TrackId shown = g.playing != kNone ? g.playing : g.settings.resumeOn ? g.resumeTrack : kNone;
    return g_lib.Contains(shown) && g_cover && g_coverFor == shown ? g_cover : nullptr;
}

std::wstring FormatForTrack(const std::wstring& pattern, TrackId id, bool withPlayback) {
    return FormatTitle(pattern, Lookup(id, withPlayback));
}

// The window title follows the playing track (the pattern has no time fields, so it changes only with it).
void UpdateWindowTitle() {
    static TrackId shown = kNone - 1;
    static uint32_t revision = 0;
    static std::wstring pattern;
    static std::string title;
    if (!g.window) return;
    if (RadioPlaying()) {
        shown = kNone - 1;
        std::string next = InternetRadio().playing.name + " - cordinal";
        if (next != title) SDL_SetWindowTitle(g.window, (title = next).c_str());
        return;
    }
    if (shown == g.playing && revision == g_lib.Revision() && pattern == EffectiveFormat(kPackWindow, g.settings.fmtWindow)) return;
    shown = g.playing;
    revision = g_lib.Revision();
    pattern = EffectiveFormat(kPackWindow, g.settings.fmtWindow);
    std::string next = "cordinal";
    if (g.playing != kNone && g_lib.Contains(g.playing)) {
        std::string text = Utf8(FormatTitle(pattern, Lookup(g.playing, true)));
        if (!text.empty()) next = text;
    }
    if (next != title) SDL_SetWindowTitle(g.window, (title = next).c_str());
}

const Palette& CurrentPalette() {
    if (g.settings.theme == ThemeMode::System) return SDL_GetSystemTheme() == SDL_SYSTEM_THEME_DARK ? DarkPalette() : LightPalette();
    return ThemePalette(g.settings.theme);
}

void ApplyTheme() {
    InitText(g.renderer, g.scale, ThemeInfo(g.settings.theme));
    AlternateThemeChanged();
    g.engine.SetScope(false);
    g_visuals = {};
    g.redraw = true;
}

bool UiAnimating() {
    return ((LiveWaveformShown() || LiveFountainShown() || SeekGlowPulsing()) && (g.playing != kNone || RadioPlaying()) &&
            !IsPaused() && L.seek.w > 0) || PauseFading() ||
           (MiniActive() && g.playing != kNone && !IsPaused()) || PointsChipAnimating() || LyricsAnimating() ||
           (FountainStyleShown() && FountainAppearance().motion && g.playing != kNone && !IsPaused()) ||
           GamesAnimating() || ListWindAnimating() || ((g.settings.theme == ThemeMode::Foobar || g.settings.theme == ThemeMode::Apollo) &&
           (g.playing != kNone ? !IsPaused() : g_visuals.Moving()));
}

// True when the live seek bar is the only thing moving and the software renderer keeps its backbuffer
// between presents. A full frame then costs one rectangle instead of the whole window; overlays and
// every other animation still force a full frame, so nothing is drawn over.
bool SeekAnimationOnly() {
    if (g.settings.gpuDrawing) return false;
    if (!((LiveWaveformShown() || LiveFountainShown() || SeekGlowPulsing()) && (g.playing != kNone || RadioPlaying()) &&
          !IsPaused() && L.seek.w > 0)) return false;
    if (g_drag != Drag::None || g.searchFocused || !g_popups.empty() || g_folders.open || g_aboutOpen ||
        g_web.open || g_exclude.open || PanelOpen() || StartupReviewOpen() || MiniActive()) return false;
    if (PointsChipAnimating() || LyricsAnimating() || GamesAnimating() || ListWindAnimating()) return false;
    if (FountainStyleShown() && FountainAppearance().motion && g.playing != kNone && !IsPaused()) return false;
    return !((g.settings.theme == ThemeMode::Foobar || g.settings.theme == ThemeMode::Apollo) &&
             (g.playing != kNone ? !IsPaused() : g_visuals.Moving()));
}

void ShowMessage(const std::wstring& text) {
    g.message = text;
    g.messageUntil = SDL_GetTicks() + 6000;
    g.redraw = true;
    MessageBlip();
}

bool Confirm(const std::string& question, const char* title) {
    SDL_MessageBoxButtonData buttons[] = {{SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "OK"},
                                          {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel"}};
    SDL_MessageBoxData dialog{SDL_MESSAGEBOX_INFORMATION, g.window, title, question.c_str(), 2, buttons, nullptr};
    int answer = 0;
    SDL_ShowMessageBox(&dialog, &answer);
    return answer == 1;
}

void InitUi() { UpdateInboxTab(); RebuildRows(); }

void ShutdownUi() {
    for (auto& [key, texture] : g_radioArt) if (texture) SDL_DestroyTexture(texture);
    g_radioArt.clear();
    if (g_cover) SDL_DestroyTexture(g_cover);
    if (g_disc) SDL_DestroyTexture(g_disc);
    g_cover = g_disc = nullptr;
    for (auto& [key, t] : g_thumbs)
        if (t.texture) SDL_DestroyTexture(t.texture);
    g_thumbs.clear();
}

// Release renderer-owned caches before replacing the renderer. CPU copies retain open editors and
// games across the switch, and worker results upload against the current renderer on the UI thread.
bool SetGpuDrawing(bool enabled) {
    const bool previous = g.settings.gpuDrawing;
    ShutdownText();
    ResetPanelRenderer(false);
    ShutdownUi();
    ReleaseFountainTexture();
    ReleaseAlternateTextures();
    if (g_about.texture) SDL_DestroyTexture(g_about.texture);
    g_about.texture = nullptr;
    g_about.textureSize = 0;
    g_coverWanted = g_coverFor = kNone;
    g_thumbFor.clear();
    g_thumbsPending = 0;
    SDL_DestroyRenderer(g.renderer);
    g.renderer = SDL_CreateRenderer(g.window, enabled ? nullptr : SDL_SOFTWARE_RENDERER);
    bool applied = g.renderer && (!enabled || std::string_view(SDL_GetRendererName(g.renderer)) != SDL_SOFTWARE_RENDERER);
    if (!applied) {
        if (g.renderer) SDL_DestroyRenderer(g.renderer);
        g.renderer = SDL_CreateRenderer(g.window, previous ? nullptr : SDL_SOFTWARE_RENDERER);
        if (!g.renderer) g.renderer = SDL_CreateRenderer(g.window, SDL_SOFTWARE_RENDERER);
    }
    if (!g.renderer) {
        // The display was lost; let normal shutdown preserve the library and settings.
        g.quit = true;
        return false;
    }
    SDL_SetRenderVSync(g.renderer, 1);
    SDL_SetRenderDrawBlendMode(g.renderer, SDL_BLENDMODE_BLEND);
    g.settings.gpuDrawing = std::string_view(SDL_GetRendererName(g.renderer)) != SDL_SOFTWARE_RENDERER;
    if (!InitText(g.renderer, g.scale, ThemeInfo(g.settings.theme))) {
        g.quit = true;
        return false;
    }
    ResetPanelRenderer(true);
    g.redraw = true;
    if (!applied) ShowMessage(L"The graphics renderer is unavailable; kept the working drawing mode.");
    return applied;
}

void OnScaleChanged() {
    g.scale = SDL_GetWindowDisplayScale(g.window);
    if (g.scale <= 0) g.scale = 1;
    ApplyTheme();
}

void RefreshTrackArt(TrackId id) {
    if (g_coverWanted == id) g_coverWanted = kNone;
    if (g_wave.track == id) g_wave.track = kNone;
    g.redraw = true;
}

void SetCover(CoverImage* image) {
    std::unique_ptr<CoverImage> owned(image);
    if (image->id != g_coverWanted) return;
    if (g_cover) SDL_DestroyTexture(g_cover);
    if (g_disc) SDL_DestroyTexture(g_disc);
    g_cover = g_disc = nullptr;
    g_coverFor = image->id;
    g_coverHue = image->width > 0 ? CoverHue(image->rgba.data(), (size_t)image->width * image->height, false) : -1;
    MprisSetArt(image->id, image->file);
    if (image->width > 0) {
        g_cover = SDL_CreateTexture(g.renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, image->width, image->height);
        if (g_cover) {
            SDL_UpdateTexture(g_cover, nullptr, image->rgba.data(), image->width * 4);
            SDL_SetTextureScaleMode(g_cover, SDL_SCALEMODE_LINEAR);
        }
    }
    if (image->discWidth > 0) {
        g_disc = SDL_CreateTexture(g.renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, image->discWidth, image->discHeight);
        if (g_disc) {
            SDL_UpdateTexture(g_disc, nullptr, image->discRgba.data(), image->discWidth * 4);
            SDL_SetTextureScaleMode(g_disc, SDL_SCALEMODE_LINEAR);
            SDL_SetTextureBlendMode(g_disc, SDL_BLENDMODE_BLEND);
        }
    }
    g.redraw = true;
}

void ForgetCoverArt() {
    // Covers still on their way keep their slots; SetThumb fills them as usual.
    for (auto it = g_thumbs.begin(); it != g_thumbs.end();) {
        if (!it->second.done) { ++it; continue; }
        if (it->second.texture) SDL_DestroyTexture(it->second.texture);
        it = g_thumbs.erase(it);
    }
    g_coverWanted = kNone;  // the next frame asks for the now-playing cover again
    g.redraw = true;
}

void SetThumb(CoverImage* image) {
    std::unique_ptr<CoverImage> owned(image);
    auto f = g_thumbFor.find(image->id);
    if (f == g_thumbFor.end()) return;
    uint64_t key = f->second;
    g_thumbFor.erase(f);
    --g_thumbsPending;
    auto it = g_thumbs.find(key);
    if (it == g_thumbs.end()) return;
    it->second.done = true;
    if (image->width > 0) {
        SDL_Texture* t = SDL_CreateTexture(g.renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, image->width, image->height);
        if (t) {
            SDL_UpdateTexture(t, nullptr, image->rgba.data(), image->width * 4);
            SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
            it->second.texture = t;
            it->second.hue = CoverHue(image->rgba.data(), (size_t)image->width * image->height, false);
        }
    }
    g.redraw = true;
}

void OnFileChosen(int purpose, std::wstring* path) {
    std::unique_ptr<std::wstring> owned(path);
    PanelFileChosen(purpose, *path);
}

void OnFoldersChosen(std::vector<std::wstring>* folders) {
    std::unique_ptr<std::vector<std::wstring>> owned(folders);
    for (auto& f : *folders) AddFolder(f);
    g.redraw = true;
}

void Render() {
    Draw();
    SDL_RenderPresent(g.renderer);
    g.redraw = false;
}

// The live seek bar's 60 Hz frame: the rest of the software backbuffer is still the last full frame,
// so copy just this rectangle to the window instead of letting SDL update all of it.
void RenderSeek() {
    SDL_SetRenderClipRect(g.renderer, nullptr);
    DrawLiveSeek(CurrentPalette());
#if SDL_VERSION_ATLEAST(3, 1, 3)
    SDL_Rect rect{(int)std::floor(L.seek.x), (int)std::floor(L.seek.y),
                  (int)std::ceil(L.seek.w), (int)std::ceil(L.seek.h)};
    int width = 0, height = 0;
    SDL_GetWindowSizeInPixels(g.window, &width, &height);
    if (rect.x < 0) rect.w += rect.x, rect.x = 0;
    if (rect.y < 0) rect.h += rect.y, rect.y = 0;
    rect.w = std::min(rect.w, width - rect.x);
    rect.h = std::min(rect.h, height - rect.y);
    if (rect.w > 0 && rect.h > 0 && SDL_FlushRenderer(g.renderer) &&
        SDL_UpdateWindowSurfaceRects(g.window, &rect, 1)) return;
#endif
    SDL_RenderPresent(g.renderer);
}

void HandleEvent(const SDL_Event& e) {
    switch (e.type) {
        case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: case SDL_EVENT_MOUSE_WHEEL:
        case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: g_lastInput = SDL_GetTicks(); break;
        default: break;
    }
    if (MiniActive() && MiniEvent(e)) return;
    switch (e.type) {
        case SDL_EVENT_MOUSE_BUTTON_DOWN: MouseDown(e.button); break;
        case SDL_EVENT_MOUSE_BUTTON_UP: MouseUp(e.button); break;
        case SDL_EVENT_MOUSE_MOTION: MouseMove(e.motion); break;
        case SDL_EVENT_MOUSE_WHEEL: Wheel(e.wheel); break;
        case SDL_EVENT_KEY_DOWN: Key(e.key); break;
        case SDL_EVENT_KEY_UP: KeyUp(e.key); break;
        case SDL_EVENT_TEXT_INPUT: TextInput(e.text); break;
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
            g_mouseX = g_mouseY = -1;
            g.redraw = true;
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            CloseMenus();
            break;
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED: OnScaleChanged(); break;
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_EXPOSED:
        case SDL_EVENT_SYSTEM_THEME_CHANGED: g.redraw = true; break;
        case SDL_EVENT_DROP_TEXT:
            CoverPanelDrop(e.drop.data, false);
            break;
        case SDL_EVENT_DROP_FILE:
            if (CoverPanelDrop(e.drop.data, true)) break;  // the cover picker takes it as an image
            // One picture dropped on its own is a cover for the album it lands on, not music to add.
            if (DropCover(e.drop.data, e.drop.x * Density(), e.drop.y * Density())) break;
            if (e.drop.data) {
                static uint64_t lastDrop = 0;  // a drop of many files arrives as many events: one plop
                if (SDL_GetTicks() - lastDrop > 500) PlaySfx(Sfx::Drop, 1);
                lastDrop = SDL_GetTicks();
                ExpandPaths({compat::Widen(e.drop.data)}, Opening::Drop);
            }
            break;
        default: break;
    }
}

// --- the parity checks ---------------------------------------------------------------------------------------------

bool UiBusy() { return g_thumbsPending > 0 || (g_wave.track != kNone && g_wave.request); }

void PrepareWaveform(TrackId id) {
    if (!g.settings.waveform || g.settings.liveWaveform) return;
    Peaks peaks{};
    uint32_t request = 0;
    RequestPeaks(id, peaks, request);
}

void PrefetchWaveform(TrackId id) {
    if (!g.settings.waveform || id == kNone || g.settings.liveWaveform) { CancelPeakPrefetch(); return; }
    std::wstring path;
    double start, end;
    if (!PeakSource(id, path, start, end)) return;
    const Track& t = g_lib[id];
    PrefetchPeaks(path, t.fileSize, t.fileTime, start, end);
}

void OnPeaksReady(uint32_t request) {
    if (!request) {  // a Mood column track (peaks.h ListPeaks)
        g.redraw = true;
        return;
    }
    if (!g.settings.waveform || g_wave.track == kNone || !request || request != g_wave.request) return;
    g_wave.ready = RequestPeaks(g_wave.track, g_wave.peaks, g_wave.request);
    g.redraw = true;
}

void OpenTrackMenu(float x, float y) { OpenRowMenu(x, y); }
void RateTracks(const std::vector<TrackId>& ids, uint8_t rating) { SetRating(ids, rating); }

}  // namespace lx
