// Keyboard tables: evdev key codes to SDL scancodes (the table SDL uses on Linux, from its
// scancodes_linux.h), X keysyms to characters and names (generated from xorgproto's keysymdef.h),
// and SDL's rule for the keycode a key reports.
#include "platform/platform.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace lx::platform {
namespace {

#include "platform/keysym_table.inc"

// Linux input event codes 0-234 to SDL scancodes (SDL 3.2's src/events/scancodes_linux.h, zlib).
constexpr uint16_t kEvdev[] = {
    0, 41, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 45, 46, 42, 43,
    20, 26, 8, 21, 23, 28, 24, 12, 18, 19, 47, 48, 40, 224, 4, 22,
    7, 9, 10, 11, 13, 14, 15, 51, 52, 53, 225, 49, 29, 27, 6, 25,
    5, 17, 16, 54, 55, 56, 229, 85, 226, 44, 57, 58, 59, 60, 61, 62,
    63, 64, 65, 66, 67, 83, 71, 95, 96, 97, 86, 92, 93, 94, 87, 89,
    90, 91, 98, 99, 0, 148, 100, 68, 69, 135, 146, 147, 138, 136, 139, 139,
    88, 228, 84, 154, 230, 0, 74, 82, 75, 80, 79, 77, 81, 78, 73, 76,
    0, 127, 129, 128, 102, 103, 215, 72, 0, 133, 144, 145, 137, 227, 231, 101,
    120, 121, 279, 122, 0, 124, 274, 125, 126, 123, 117, 118, 0, 0, 258, 259,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 286, 0, 282, 283,
    0, 270, 270, 267, 271, 268, 269, 264, 266, 0, 0, 0, 281, 285, 276, 0,
    0, 0, 0, 182, 183, 273, 121, 104, 105, 106, 107, 108, 109, 110, 111, 112,
    113, 114, 115, 0, 0, 0, 0, 0, 262, 263, 0, 0, 0, 0, 275, 262,
    265, 0, 70, 0, 0, 0, 0, 0, 0, 280, 0, 0, 0, 0, 153, 155,
    0, 0, 272, 0, 0, 0, 0, 0, 0, 0, 277,
};


}  // namespace

std::string Utf8(uint32_t c) {
    std::string s;
    if (c < 0x80) {
        s += (char)c;
    } else if (c < 0x800) {
        s += (char)(0xc0 | c >> 6), s += (char)(0x80 | (c & 0x3f));
    } else if (c < 0x10000) {
        if (c >= 0xd800 && c < 0xe000) return {};
        s += (char)(0xe0 | c >> 12), s += (char)(0x80 | (c >> 6 & 0x3f)), s += (char)(0x80 | (c & 0x3f));
    } else if (c < 0x110000) {
        s += (char)(0xf0 | c >> 18), s += (char)(0x80 | (c >> 12 & 0x3f)), s += (char)(0x80 | (c >> 6 & 0x3f)),
            s += (char)(0x80 | (c & 0x3f));
    }
    return s;
}

SDL_Scancode ScancodeFromEvdev(uint32_t code) {
    return code < sizeof kEvdev / sizeof kEvdev[0] ? (SDL_Scancode)kEvdev[code] : SDL_SCANCODE_UNKNOWN;
}

SDL_Scancode ScancodeFromKeysym(uint32_t ks) {
    if (ks >= 0xffbe && ks <= 0xffc9) return (SDL_Scancode)(58 + (ks - 0xffbe));    // F1-F12
    if (ks >= 0xffca && ks <= 0xffd5) return (SDL_Scancode)(104 + (ks - 0xffca));   // F13-F24
    switch (ks) {
    case 0xff0d: return (SDL_Scancode)40;   // Return
    case 0xff1b: return (SDL_Scancode)41;   // Escape
    case 0xff08: return (SDL_Scancode)42;   // BackSpace
    case 0xff09: return (SDL_Scancode)43;   // Tab
    case 0x20: return (SDL_Scancode)44;     // space
    case 0xff63: return (SDL_Scancode)73;   // Insert
    case 0xff50: return (SDL_Scancode)74;   // Home
    case 0xff55: return (SDL_Scancode)75;   // Prior
    case 0xffff: return (SDL_Scancode)76;   // Delete
    case 0xff57: return (SDL_Scancode)77;   // End
    case 0xff56: return (SDL_Scancode)78;   // Next
    case 0xff53: return (SDL_Scancode)79;   // Right
    case 0xff51: return (SDL_Scancode)80;   // Left
    case 0xff54: return (SDL_Scancode)81;   // Down
    case 0xff52: return (SDL_Scancode)82;   // Up
    case 0xff8d: return (SDL_Scancode)88;   // KP_Enter
    case 0xffe3: return (SDL_Scancode)224;  // Control_L
    case 0xffe1: return (SDL_Scancode)225;  // Shift_L
    case 0xffe9: return (SDL_Scancode)226;  // Alt_L
    case 0xffeb: return (SDL_Scancode)227;  // Super_L
    case 0xffe4: return (SDL_Scancode)228;  // Control_R
    case 0xffe2: return (SDL_Scancode)229;  // Shift_R
    case 0xffea: case 0xfe03: return (SDL_Scancode)230;  // Alt_R, ISO_Level3_Shift
    case 0xffec: return (SDL_Scancode)231;  // Super_R
    case 0x1008ff11: return (SDL_Scancode)129;  // volume down
    case 0x1008ff12: return (SDL_Scancode)127;  // mute
    case 0x1008ff13: return (SDL_Scancode)128;  // volume up
    case 0x1008ff14: case 0x1008ff31: return SDL_SCANCODE_MEDIA_PLAY_PAUSE;
    case 0x1008ff15: return SDL_SCANCODE_MEDIA_STOP;
    case 0x1008ff16: return SDL_SCANCODE_MEDIA_PREVIOUS_TRACK;
    case 0x1008ff17: return SDL_SCANCODE_MEDIA_NEXT_TRACK;
    default: return SDL_SCANCODE_UNKNOWN;
    }
}

uint32_t KeysymToUnicode(uint32_t ks) {
    if ((ks >= 0x20 && ks <= 0x7e) || (ks >= 0xa0 && ks <= 0xff)) return ks;
    if (ks >= 0x01000100 && ks <= 0x0110ffff) return ks - 0x01000000;
    const KeysymChar* end = kKeysymChars + sizeof kKeysymChars / sizeof kKeysymChars[0];
    const KeysymChar* it = std::lower_bound(kKeysymChars, end, ks, [](const KeysymChar& c, uint32_t v) { return c.keysym < v; });
    return it != end && it->keysym == ks ? it->ucs : 0;
}

uint32_t KeysymFromUnicode(uint32_t cp) {
    if ((cp >= 0x20 && cp <= 0x7e) || (cp >= 0xa0 && cp <= 0xff)) return cp;
    // The legacy keysym for the character when there is one (keysymdef's own order: the lowest).
    uint32_t best = 0;
    for (const KeysymChar& c : kKeysymChars)
        if (c.ucs == cp && (!best || c.keysym < best)) best = c.keysym;
    return best ? best : cp ? 0x01000000 + cp : 0;
}

bool IsKeypadKeysym(uint32_t ks) { return ks >= 0xff80 && ks <= 0xffbd; }

uint32_t KeysymFromName(std::string_view name) {
    if (name.empty() || name.size() > 64) return 0;
    std::string n(name);
    const KeysymName* end = kKeysymNames + sizeof kKeysymNames / sizeof kKeysymNames[0];
    const KeysymName* it =
        std::lower_bound(kKeysymNames, end, n.c_str(), [](const KeysymName& k, const char* v) { return strcmp(k.name, v) < 0; });
    if (it != end && n == it->name) return it->keysym;
    auto hex = [](std::string_view h, uint32_t& out) {
        if (h.empty() || h.size() > 8) return false;
        out = 0;
        for (char c : h) {
            int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0) return false;
            out = out << 4 | (uint32_t)d;
        }
        return true;
    };
    uint32_t v = 0;
    if (n.size() >= 5 && n[0] == 'U' && hex(name.substr(1), v) && v <= 0x10ffff)  // "U0441": a character
        return v < 0x100 && (v >= 0x20 && v != 0x7f) && !(v > 0x7e && v < 0xa0) ? v : 0x01000000 + v;
    if (n.size() > 2 && n[0] == '0' && (n[1] == 'x' || n[1] == 'X') && hex(name.substr(2), v)) return v;
    return 0;
}

SDL_Keycode KeycodeFor(SDL_Scancode sc, uint32_t base, uint32_t shifted) {
    const int s = (int)sc;
    const bool keypad = s >= 84 && s <= 99;  // SDL's keypad scancodes report keypad keycodes
    if (!keypad) {
        uint32_t c = KeysymToUnicode(base);
        if (s >= 4 && s <= 29 && !(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z')) c = 'a' + (uint32_t)(s - 4);  // latin_letters
        if (s >= 30 && s <= 39 && !(c >= '0' && c <= '9')) {  // french_numbers
            uint32_t d = KeysymToUnicode(shifted);
            if (d >= '0' && d <= '9') c = d;
        }
        if (c >= 'A' && c <= 'Z') c += 32;
        // Characters, and the control characters Return, Tab, Escape and BackSpace type, as SDL.
        if (c >= 0x20 || c == 0x08 || c == 0x09 || c == 0x0d || c == 0x1b) return c;
    }
    if (sc == SDL_SCANCODE_UNKNOWN) return SDLK_UNKNOWN;
    return SDLK_SCANCODE_MASK | (uint32_t)s;
}

}  // namespace lx::platform
