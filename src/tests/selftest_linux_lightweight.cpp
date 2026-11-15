// Linux Lightweight mode checks, as on Windows (selftest_ui.cpp, TestLightweightMode): each choice of what
// stays on screen puts the window away and brings it back as it was, a size chosen by hand ends the mode, and
// the song picker lists the transport and finds albums and songs by their words. Runs with --check-layout on
// the offscreen window, against a library of its own; nothing is played or saved.
#ifndef _WIN32
#include "../linux/ui_internal.h"
#include <cstdio>

namespace lx {
int CheckLinuxLightweight() {
    int failed = 0;
    auto check = [&](bool ok, const char* message) {
        printf("%s %s\n", ok ? "ok  " : "FAIL", message);
        fflush(stdout);
        failed += !ok;
    };
    Library savedLib = std::move(g_lib);
    g_lib = Library();
    const auto savedSettings = g.settings;
    const TrackId savedPlaying = g.playing;
    g.playing = kNone;
    g.settings.lightweightTrim = false;  // the check process's memory is the harness's

    // The picker: the transport unasked, then the album and the song a word finds.
    TagData tags;
    tags.title = "Zebra crossing";
    tags.artist = "Picker artist";
    tags.album = "Picker album";
    TrackId zebra = g_lib.Add(L"/checks/lightweight-zebra.flac");
    g_lib.ApplyTags(zebra, tags);
    g_lib[zebra].flags |= Track::kInLibrary;
    g_lib.Touch();
    OpenSongPicker();
    std::vector<std::string> standing = SongPickerRows();
    SetSongPickerQuery(L"zebra");
    std::vector<std::string> found = SongPickerRows();
    ClosePanel();
    auto has = [](const std::vector<std::string>& rows, const char* text) {
        return std::any_of(rows.begin(), rows.end(), [&](const std::string& r) { return r.starts_with(text); });
    };
    check(has(standing, "Play") && has(standing, "Next") && has(standing, "Lightweight mode") && found.size() >= 2 &&
              found[0] == "Picker album | Album \xE2\x80\xBA Picker artist" && found[1] == "Zebra crossing | Picker artist" &&
              !PanelOpen(),
          "lightweight: the song picker lists the transport, then finds the album and the song by a word");

    SetViewSize(ViewSize::Normal);
    g.settings.lightweightShows = (int)LightweightShows::Nothing;
    g.settings.lightweightNumpad = (int)LightweightNumpad::NumLockOff;
    EnterLightweight();
    const bool hidden = g.lightweight && LightweightHidesWindow() && LightweightPausesWork();
    // The numpad reaches cordinal from any program through the desktop's shortcut portal: the keys Num
    // Lock off sends, so Num Lock on gives the numbers back, and the picker's own key.
    auto asked = [](const char* binding) {
        const std::vector<std::string> all = GlobalKeyTriggers();
        return std::find(all.begin(), all.end(), binding) != all.end();
    };
    const bool numLockOff = asked("KP_Begin=Playback > Play / Pause") && asked("KP_Insert=Playback > Play a song...") &&
                            asked("KP_Delete=View > Lightweight mode") && asked("ALT+LOGO+space=Playback > Play a song...") &&
                            !asked("KP_5=Playback > Play / Pause") && !asked("KP_Add=Playback > Play / Pause");
    g.settings.lightweightNumpad = (int)LightweightNumpad::Always;
    ApplyGlobalKeys();
    const bool always = asked("KP_Begin=Playback > Play / Pause") && asked("KP_5=Playback > Play / Pause") &&
                        NumpadTriggers(11, (int)LightweightNumpad::Always) == std::vector<std::string>{"KP_Add"} &&
                        NumpadTriggers(11, (int)LightweightNumpad::NumLockOff).empty() &&
                        NumpadTriggers(5, (int)LightweightNumpad::Off).empty();
    LeaveLightweight();
    const std::vector<std::string> after = GlobalKeyTriggers();
    const bool released = std::none_of(after.begin(), after.end(), [](const std::string& t) {
        return t.starts_with("KP_") || t.starts_with("ALT+LOGO+space");
    });
    check(hidden && !g.lightweight && !LightweightHidesWindow() && !LightweightPausesWork(),
          "lightweight: Nothing hides the window and pauses library work; leaving lifts both");
    check(numLockOff && always && released,
          "lightweight: the numpad and the picker's key reach cordinal from any program, by Num Lock, only while it is on");
    g.settings.lightweightNumpad = (int)LightweightNumpad::NumLockOff;

    g.settings.lightweightShows = (int)LightweightShows::Mini;
    EnterLightweight();
    const bool mini = g.lightweight && MiniActive() && !LightweightHidesWindow();
    LeaveLightweight();
    check(mini && !g.lightweight && !MiniActive(), "lightweight: Mini player opens the mini player and leaving closes it");

    g.settings.lightweightShows = (int)LightweightShows::Compact;
    EnterLightweight();
    const bool compact = g.lightweight && g.settings.size == ViewSize::Compact;
    LeaveLightweight();
    const bool restored = !g.lightweight && g.settings.size == ViewSize::Normal;
    EnterLightweight();
    SetViewSize(ViewSize::Big);  // a bigger size chosen by hand ends the mode and keeps that size
    const bool chosen = !g.lightweight && g.settings.size == ViewSize::Big;
    check(compact && restored && chosen,
          "lightweight: Compact shrinks the window, leaving restores Normal, and a size chosen by hand ends it");

    // Your keys: one given to Shuffle runs it before cordinal's own, a cleared one does nothing, and the
    // panel shows them beside cordinal's.
    keymap::Key ctrlShiftK, ctrlH;
    keymap::Parse(L"Ctrl+Shift+K", ctrlShiftK);
    keymap::Parse(L"Ctrl+H", ctrlH);
    std::vector<keymap::Binding> changes;
    keymap::Assign(changes, DefaultKeyBindings(), keymap::Scope::Window, ctrlShiftK, L"Playback > Shuffle");
    keymap::Assign(changes, DefaultKeyBindings(), keymap::Scope::Window, ctrlH, L"");
    SetKeyChanges(changes);
    auto press = [](SDL_Keycode key, SDL_Keymod mod) {
        SDL_KeyboardEvent e{};
        e.key = key;
        e.mod = mod;
        return UserKeyDown(e);
    };
    const int shuffleBefore = g.settings.shuffle;
    const bool ran = press((SDL_Keycode)'k', SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT) && g.settings.shuffle != shuffleBefore;
    const int shuffleAfter = g.settings.shuffle;
    const bool swallowed = press(SDLK_H, SDL_KMOD_LCTRL) && g.settings.shuffle == shuffleAfter;
    const bool untouched = !press(SDLK_J, SDL_KMOD_LCTRL);  // a key you never changed is cordinal's
    OpenKeyboardShortcuts();
    const std::vector<std::string> shuffleRows = KeyboardShortcutRows(L"playback shuffle");
    const std::vector<std::string> volumeRows = KeyboardShortcutRows(L"volume up");
    ClosePanel();
    check(ran && swallowed && untouched && !shuffleRows.empty() && shuffleRows[0] == "Shuffle | Ctrl+Shift+K | Num 3" &&
              volumeRows.size() == 1 && volumeRows[0] == "Volume up |  | Num 8",
          "keys: your key runs its command, a cleared one does nothing, and the panel shows both kinds");
    SetKeyChanges({});
    SetShuffle(shuffleBefore);
    check(EffectiveKeyBindings(keymap::Scope::Window).size() == DefaultKeyBindings().size() - keymap::DefaultGlobal().size(),
          "keys: Reset all puts back cordinal's own keys");

    SetViewSize(ViewSize::Normal);
    g.settings = savedSettings;
    g.playing = savedPlaying;
    g_lib = std::move(savedLib);
    g_lib.Touch();
    return failed;
}
}  // namespace lx
#endif
