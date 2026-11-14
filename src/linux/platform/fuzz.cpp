// Fuzz target for the platform layer's parsers of bytes that come from outside the process: the
// X11 connection setup, reply/event framing, .Xauthority, DISPLAY, XSETTINGS, X resources,
// Xcursor files, text/uri-list; the Wayland message framing and argument reader; and the XKB
// keymap text a compositor or X server hands us (then every key looked up in it).
//
// The first byte picks the parser, the rest is its input. With clang and libFuzzer:
//   clang++ -std=c++20 -g -O1 -fsanitize=fuzzer,address,undefined -Isrc/linux -Isrc \
//     src/linux/platform/fuzz.cpp src/linux/platform/{x11_wire,wayland_wire,xkb_keymap,keys}.cpp -o fuzz-platform
//   ./fuzz-platform -max_total_time=600 corpus/
// Any compiler, replaying files (a corpus or crash reproducers) under the sanitizers:
//   g++ ... -DCORDINAL_FUZZ_REPLAY -fsanitize=address,undefined ...; ./fuzz-platform FILE...
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include "platform/wayland_wire.h"
#include "platform/x11_wire.h"
#include "platform/xkb_keymap.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1) return 0;
    const std::string_view in(reinterpret_cast<const char*>(data) + 1, size - 1);
    volatile size_t sink = 0;
    switch (data[0] % 12) {
    case 0:
        if (auto s = lx::x11::ParseSetup(in)) sink = s->screens.size();
        break;
    case 1: {  // framing: walk the buffer the way the connection does
        std::string_view b = in;
        while (true) {
            const size_t n = lx::x11::NextUnit(b);
            if (n == 0 || n > b.size()) break;
            b.remove_prefix(n);
        }
        break;
    }
    case 2: {
        const size_t cut = in.find('\n');
        const std::string_view host = cut == std::string_view::npos ? "box" : in.substr(0, cut);
        if (auto c = lx::x11::FindCookie(cut == std::string_view::npos ? in : in.substr(cut + 1), host, "0")) sink = c->size();
        break;
    }
    case 3:
        if (auto d = lx::x11::ParseDisplay(in)) sink = d->number.size();
        break;
    case 4:
        sink = lx::x11::ParseXSettings(in).size();
        break;
    case 5:
        sink = (size_t)(int64_t)lx::x11::ResourceDpi(in) + lx::x11::ResourceString(in, "Xcursor.theme").size();
        break;
    case 6:
        if (in.size() >= 1)
            if (auto c = lx::x11::ParseXcursor(in.substr(1), (uint8_t)in[0])) sink = c->argb.size();
        break;
    case 7:
        sink = lx::x11::ParseUriList(in, true).size() + lx::x11::ParseUriList(in, false).size();
        break;
    case 8: {  // Wayland: frame messages, then read each body with a signature taken from the input
        std::string_view b = in;
        size_t round = 0;
        while (round++ < 1000) {
            lx::wl::Message m;
            const size_t n = lx::wl::NextMessage(b, m);
            if (n == 0 || n == SIZE_MAX || n > b.size()) break;
            lx::wl::Args a(m.body);
            for (unsigned i = 0; i < 8 && !a.failed(); ++i) {
                switch ((m.object + m.opcode + i) % 5) {
                case 0: sink = a.Uint(); break;
                case 1: sink = (size_t)(int64_t)a.Fixed(); break;
                case 2: { bool null = false; sink = a.String(&null).size(); break; }
                case 3: sink = a.Array().size(); break;
                default: sink = (size_t)a.Int(); break;
                }
            }
            b.remove_prefix(n);
        }
        break;
    }
    default: {  // XKB keymap text, then every key at a few modifier states and groups
        lx::platform::XkbKeymap k;
        std::string why;
        if (!k.Parse(in, &why)) break;
        for (uint32_t code = 0; code < 300; ++code) {
            for (uint32_t mods : {0u, 1u, 2u, 0x80u, 0x81u, 0xffu, 0xffffffffu})
                for (uint32_t group : {0u, 1u, 7u}) sink = k.Keysym(code, mods, group);
            sink = k.LevelKeysym(code, 0, 3) + k.Repeats(code);
        }
        sink = k.Normalize(0xffffffffu);
        break;
    }
    }
    (void)sink;
    return 0;
}

#ifdef CORDINAL_FUZZ_REPLAY
int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        FILE* f = fopen(argv[i], "rb");
        if (!f) { perror(argv[i]); return 1; }
        std::string d;
        char buf[65536];
        for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) d.append(buf, n);
        fclose(f);
        LLVMFuzzerTestOneInput(reinterpret_cast<const uint8_t*>(d.data()), d.size());
    }
    printf("replayed %d inputs\n", argc - 1);
    return 0;
}
#endif
