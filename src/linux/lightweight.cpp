// Lightweight mode on Linux (View > Lightweight mode, Ctrl+Shift+L, or the tray icon), as the Windows
// build's app_lightweight.cpp: cordinal keeps playing, effects and all, with its window put away, so it can
// run in the background on little memory and CPU. What stays on screen is a preference: nothing but the
// tray icon, the mini player, or the compact window. With nothing shown the window is hidden, so nothing
// draws and the loop wakes once a second; library work waits and, a moment in, the allocator gives its
// free memory back. The tray icon's "Play a song..." (or a click on it) brings up the song picker, the
// window shown just for it; Show cordinal, or starting cordinal again, ends the mode.
//
// The numpad is a remote from any program: Wayland lets no program read keys meant for another, so its keys
// are asked of the desktop's Global Shortcuts portal while the mode is on (packs_ui.cpp, NumpadTriggers),
// by the names Num Lock off gives them, as pack keys are. LightweightKey below takes them in cordinal's own
// window, and wherever no portal answers.
#include "ui_internal.h"

namespace lx {

namespace {

struct State {
    bool openedMini = false;   // the mode opened the mini player, so leaving closes it
    bool changedSize = false;  // the mode made the window compact from `sizeBefore`
    ViewSize sizeBefore = ViewSize::Normal;
    bool changingSize = false;  // the mode itself is switching the size
    bool shownForPicker = false;  // the hidden window shows only while the song picker is open
} s;

void SetSizeQuietly(ViewSize size) {
    s.changingSize = true;
    SetViewSize(size);
    s.changingSize = false;
}

}  // namespace

bool LightweightHidesWindow() {
    return g.lightweight && g.settings.lightweightShows == (int)LightweightShows::Nothing && !s.shownForPicker;
}

bool LightweightPausesWork() { return g.lightweight && g.settings.lightweightPause; }

void EnterLightweight() {
    if (g.lightweight || !g.window) return;
    if (PanelOpen()) ClosePanel();
    g.lightweight = true;
    s = State{};
    const auto shows = (LightweightShows)g.settings.lightweightShows;
    if (shows == LightweightShows::Mini) {
        if (!MiniActive()) {
            ToggleMini();
            s.openedMini = MiniActive();
        }
    } else {
        if (MiniActive()) {  // the mode shows something else: the mini player goes too
            g.lightweight = false;
            LeaveMini();
            g.lightweight = true;
        }
        if (shows == LightweightShows::Compact && g.settings.size != ViewSize::Compact) {
            s.sizeBefore = g.settings.size;
            s.changedSize = true;
            SetSizeQuietly(ViewSize::Compact);
        } else if (shows == LightweightShows::Nothing) {
            SDL_HideWindow(g.window);
        }
    }
    InvalidateCellCache();
    GiveMemoryBackSoon();  // what the caches freed goes back to the system once the window is away
    MprisChanged();  // the tray menu's checkmark
    ApplyGlobalKeys();  // cordinal's own keys from any program, held while the mode is on
    g.redraw = true;
}

// `show` false is for quitting: the window's size goes back for next time, but nothing appears.
void LeaveLightweight(bool show) {
    if (!g.lightweight) return;
    g.lightweight = false;  // first, so leaving the mini player below doesn't come back here
    if (PanelOpen()) ClosePanel();
    if (s.openedMini && MiniActive()) LeaveMini();
    if (s.changedSize && g.settings.size == ViewSize::Compact) SetSizeQuietly(s.sizeBefore);
    s = State{};
    if (show) SDL_RaiseWindow(g.window);  // maps a hidden window again, keeping it maximized if it was
    ApplyGlobalKeys();
    MprisChanged();
    g.redraw = true;
}

void ToggleLightweight() {
    if (g.lightweight) LeaveLightweight();
    else EnterLightweight();
}

// A size other than compact was chosen by hand (Ctrl+=, a menu): the mode is over, and that size stays.
void LightweightSizeChanged(ViewSize size) {
    if (!g.lightweight || s.changingSize || size == ViewSize::Compact) return;
    s.changedSize = s.openedMini = false;
    LeaveLightweight();
}

void LightweightPreferencesChanged() {
    if (!g.lightweight) return;
    LeaveLightweight(false);
    EnterLightweight();
}

// The tray's "Play a song...": the picker in the window, shown for it alone when the mode hid it.
void OpenSongPickerFromTray() {
    if (LightweightHidesWindow()) {
        s.shownForPicker = true;
    }
    SDL_RaiseWindow(g.window);
    OpenSongPicker();
}

void LightweightPickerClosed() {
    if (!s.shownForPicker) return;
    s.shownForPicker = false;
    if (g.lightweight && g.settings.lightweightShows == (int)LightweightShows::Nothing) SDL_HideWindow(g.window);
}

// The numpad keys, by scan code, with Num Lock off (or always, as Preferences says): each runs its
// "from any program" binding (keys.cpp), 5 pausing and 0 picking a song unless you changed them. Keys
// with no command, modified keys and the picker's own typing pass through.
bool LightweightKey(const SDL_KeyboardEvent& e) {
    const int mode = g.settings.lightweightNumpad;
    if (!g.lightweight || mode == (int)LightweightNumpad::Off) return false;
    if (e.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) return false;
    if ((e.mod & SDL_KMOD_NUM) && mode != (int)LightweightNumpad::Always) return false;
    if (PanelOpen()) return false;
    keymap::Key key;
    if (!KeyFromEvent(e, key)) return false;
    key.mods = 0;  // Shift with Num Lock off is still the numpad's key
    const int index = keymap::NumpadIndex(key);
    // The operators and Enter type the same with Num Lock on or off: they are taken only when Always.
    if (index < 0 || (!keymap::NumLockKey(index) && mode != (int)LightweightNumpad::Always)) return false;
    if (!NumpadCommand(index, e.repeat)) return false;
    MprisChanged();
    g.redraw = true;
    return true;
}

}  // namespace lx
