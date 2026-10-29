// Text for the Linux front end through SDL3_ttf, with fallback fonts for scripts the main one lacks,
// and a cache of shaped strings so a redraw reuses them. The UI font follows the theme the way the
// Windows build's does (theme_palette.cpp): the system face at 9 pt, the theme's own face, or the
// DELTARUNE pixel font built into the executable. Menus keep the system face in every theme, as
// the Windows menu bar does.
#pragma once
#include <SDL3/SDL.h>

#include <string>
#include <string_view>

struct Theme;

namespace lx {

// Normal and Bold are the UI font; Small is the same face for secondary dialog text; Title is the
// dialogs' heading; Menu is the menu bar's and popup menus' system face.
enum class Font { Normal, Bold, Small, Title, Menu, Count };

bool InitText(SDL_Renderer* renderer, float scale, const Theme& theme);  // again when either changes
void ShutdownText();
void ClearTextCache();
bool UiSymbolsAvailable();  // the rating stars and auto-playlist mark, including fallback fonts

// Draws `text` (UTF-8) at (x, y) top-left. With maxWidth > 0, a string that doesn't fit is cut
// with an ellipsis. align: 0 left, 1 center, 2 right (within maxWidth). Returns the width drawn.
float DrawText(Font font, const std::string& text, float x, float y, uint32_t rgb, float maxWidth = 0, int align = 0);
float TextWidth(Font font, const std::string& text);
float LineHeight(Font font);

// The DELTARUNE font (the web player serves it too).
const unsigned char* DeltaruneFont(size_t* size);
// The cardinal icon as PNG bytes: the smallest of src/cordinal.ico's images at least `size` pixels wide
// (the largest when none is).
std::string_view AppIcon(int size);

std::string Utf8(const std::wstring& s);

}  // namespace lx
