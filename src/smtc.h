// Windows System Media Transport Controls: the media flyout with cover art, the lock screen
// controls, Bluetooth headset buttons and the Game Bar media widget all read from here.
//
// Every call is made on the UI thread. Button presses arrive on a system thread and are only
// posted to the window as messages.
#pragma once
#include <windows.h>

#include <string>

namespace smtc {

// Button values double as the wParam of the posted message (see Init).
enum class Button : WPARAM { Play = 1, Pause, Stop, Next, Previous };
enum class State { Closed, Stopped, Playing, Paused };

// False if the controls are not available (Windows 8.1 or older, or no shell): the other calls
// then do nothing. Button presses post `msg` to `hwnd` with wParam = Button.
bool Init(HWND hwnd, UINT msg);
void SetState(State s);
// `image` is encoded JPEG or PNG bytes for the cover art; empty means none.
void SetMetadata(const std::wstring& title, const std::wstring& artist, const std::wstring& album,
                 const std::string& image);
// Seconds. Cheap enough to call once a second.
void SetTimeline(double position, double duration);
void Shutdown();

}  // namespace smtc
