// The X11 wire format pieces that read what a server or another client wrote: kept apart from the
// connection so the fuzz harness (src/linux/platform/fuzz.cpp) can drive them with arbitrary bytes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lx::x11 {

constexpr size_t kMaxReply = 64u << 20;  // a reply or generic event past this closes the connection

struct Visual {
    uint32_t id = 0;
    uint8_t cls = 0;
    uint32_t red = 0, green = 0, blue = 0;
};
struct Screen {
    uint32_t root = 0, colormap = 0, white = 0, black = 0, rootVisual = 0;
    uint16_t width = 0, height = 0;
    uint8_t rootDepth = 0;
    std::vector<std::pair<uint8_t, Visual>> visuals;  // depth, visual
};
struct Setup {
    uint32_t idBase = 0, idMask = 0;
    uint16_t maxRequest = 0;  // in 4-byte units
    uint8_t minKeycode = 0, maxKeycode = 0, imageByteOrder = 0;
    std::vector<std::pair<uint8_t, uint8_t>> formats;  // depth, bits per pixel
    std::vector<Screen> screens;
    std::string failure;  // the server's reason when it refused
};
// The reply to the connection setup (everything after the first 8 bytes is checked against its
// lengths). nullopt when malformed; a refusal comes back with `failure` set and no screens.
std::optional<Setup> ParseSetup(std::string_view bytes);

// The length of the next whole unit in an incoming stream (32 for errors and events, more for
// replies and generic events); 0 when more bytes are needed, SIZE_MAX when it is malformed.
size_t NextUnit(std::string_view buffer);

// ~/.Xauthority: the MIT-MAGIC-COOKIE-1 data for display `number` on this host, if any.
std::optional<std::string> FindCookie(std::string_view file, std::string_view host, std::string_view number);

// Display names: ":0", ":1.0", "unix:2", "/tmp/.X11-unix/X0" (a socket path). nullopt for TCP and
// anything else this client doesn't connect to.
struct DisplayName {
    std::string socket;  // the path
    std::string number;
    int screen = 0;
};
std::optional<DisplayName> ParseDisplay(std::string_view name);

// The XSETTINGS property (the settings manager's): the integer values by name.
std::vector<std::pair<std::string, int32_t>> ParseXSettings(std::string_view data);
// Xft.dpi from the RESOURCE_MANAGER string; 0 when absent or unusable.
double ResourceDpi(std::string_view resources);
// Any other resource's value ("Xcursor.theme"), whitespace trimmed; empty when absent.
std::string ResourceString(std::string_view resources, std::string_view name);

// One image of an Xcursor file (premultiplied ARGB, as RENDER cursors take it): the first of the
// nominal size nearest `size`, as libXcursor picks. nullopt when the file is malformed.
struct CursorImage {
    uint32_t width = 0, height = 0, xhot = 0, yhot = 0;
    std::vector<uint32_t> argb;
};
std::optional<CursorImage> ParseXcursor(std::string_view file, int size);

// text/uri-list: the local paths (file:// URIs, %-decoded), other URIs as they are.
std::vector<std::string> ParseUriList(std::string_view data, bool localOnly);

}  // namespace lx::x11
