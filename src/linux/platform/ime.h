// The desktop's input method for X11 windows, over the session bus: fcitx5's own frontend
// (org.fcitx.Fcitx5) or ibus's portal (org.freedesktop.portal.IBus), the two input methods Linux
// desktops run. Both hand text over as UTF-8. XIM would hand it over as COMPOUND_TEXT, which puts
// Japanese, Chinese and Korean in their national charsets and would need their whole conversion
// tables here. Wayland windows use zwp_text_input_v3 in wayland.cpp instead.
//
// The client lives on a thread of its own with its own bus connection. While a text field has
// the keys, the window hands each key to it and forgets it; the input method either takes it (and
// later commits text or updates its composition) or gives it back, and the window then handles it
// the usual way. Everything comes back through Take() on the window's thread, in order.
#pragma once

#include "dbus.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace lx::platform::ime {

// What the input method hands back to the window.
struct Out {
    enum Kind : uint8_t { kReplay, kKey, kCommit, kPreedit } kind = kReplay;
    std::string raw;                // kReplay: the X key event it did not take
    bool down = false, repeat = false;
    uint32_t sym = 0, state = 0;    // kKey: a key the input method sends of its own (keysym, X state)
    std::string text;               // kCommit: finished text; kPreedit: the composition, empty when none
    int cursor = 0;                 // kPreedit: the byte offset of its cursor in `text`
};

class Client {
public:
    // Starts the thread, which looks for an input method itself; without one Taking() stays false.
    static std::unique_ptr<Client> Start();
    ~Client();

    void Focus(bool on);                      // a text field has the keys, or no longer has
    void Cursor(int x, int y, int w, int h);  // the caret, in root-window pixels
    // True while the input method should see the keys: a field has them and a context exists.
    bool Taking() const;
    // True while keys handed over wait for an answer: later keys follow them through the client,
    // even after the field lets go, so none overtakes another.
    bool Busy() const;
    // Hands a key over while Taking(); it comes back as kReplay unless the input method takes it.
    void Key(std::string raw, bool down, bool repeat, uint32_t sym, uint32_t keycode, uint32_t state, uint32_t time);
    std::vector<Out> Take();

    struct State;

private:
    explicit Client(std::unique_ptr<State> s);
    std::unique_ptr<State> s_;
};

// The message parsing, apart so the layout checks can feed it bodies the services send.
bool FcitxPreedit(dbus::Reader& r, std::string& text, int& cursor);  // UpdateFormattedPreedit's a(si)i
bool IBusText(dbus::Reader& r, std::string& text);                    // an IBusText in its variant
int CharsToBytes(std::string_view utf8, uint32_t chars);              // ibus counts its cursor in characters
int CheckParsing();  // the layout checks for the above; returns the failures

}  // namespace lx::platform::ime
