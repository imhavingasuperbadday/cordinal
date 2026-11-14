// The XKB text keymap a Wayland compositor sends (wl_keyboard.keymap, xkb_v1), read without
// libxkbcommon: key names and codes, key types and their level maps, the virtual modifiers'
// real mapping (from the compatibility interprets and modifier_map), and every key's symbols by
// group and level, with xkbcommon's automatic types for keys that name none. Kept apart from the
// connection so the fuzz harness can feed it anything.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace lx::platform {

class XkbKeymap {
public:
    // False (with a reason) when the text isn't a keymap this reader understands.
    bool Parse(std::string_view text, std::string* error = nullptr);

    // The keysym `code` (an XKB keycode: evdev + 8) types under the compositor's modifier state
    // (depressed | latched | locked, as wl_keyboard.modifiers sends them) in `group`, with the
    // Caps Lock capitalisation xkbcommon applies when the key's type doesn't use Lock. 0 for none.
    uint32_t Keysym(uint32_t code, uint32_t mods, uint32_t group) const;
    uint32_t LevelKeysym(uint32_t code, uint32_t group, uint32_t level) const;  // level 0 = base
    bool Repeats(uint32_t code) const;
    size_t KeyCount() const { return keys_.size(); }

    // Real-modifier masks for the modifier state SDL reports.
    uint32_t shift = 1, lock = 2, control = 4, alt = 0, super = 0, numLock = 0, level3 = 0;
    // Folds bits of virtual modifiers (index 8 and up, which newer xkbcommon can send) into real ones.
    uint32_t Normalize(uint32_t mods) const;

private:
    struct Entry {
        uint32_t mask = 0, preserve = 0;
        uint32_t level = 0;
    };
    struct Type {
        uint32_t mods = 0;
        std::vector<Entry> map;
        uint32_t levels = 1;
    };
    struct Group {
        int type = -1;  // into types_
        std::vector<uint32_t> syms;
    };
    struct Key {
        std::vector<Group> groups;
        bool repeat = true;
    };
    std::vector<Type> types_;
    std::map<uint32_t, Key> keys_;
    std::vector<uint32_t> vmodReal_;  // per virtual modifier: its real mask (or its own bit, 1 << (8 + i))
};

}  // namespace lx::platform
