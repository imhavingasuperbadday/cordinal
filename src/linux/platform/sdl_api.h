// The part of SDL3's API the Linux player calls, implemented in this directory instead of linking
// libSDL3: windows and input on Wayland and X11 through their wire protocols (wayland.cpp, x11.cpp),
// a software 2D renderer with SDL_Renderer's semantics (render.cpp), and the desktop services
// (desktop.cpp: file dialogs through the XDG portal, message boxes, URLs, user folders, the colour
// scheme). Names, types and constant values follow SDL3 so the call sites read as before; only what
// the player uses exists, and behaviour is documented where it differs from SDL's.
#pragma once

#include <cstddef>
#include <cstdint>

using Uint8 = uint8_t;
using Uint16 = uint16_t;
using Uint32 = uint32_t;
using Uint64 = uint64_t;
using Sint32 = int32_t;

#define SDL_VERSION_ATLEAST(X, Y, Z) 1  // the API below is SDL 3.4's, PNG saving included

// --- basics ---------------------------------------------------------------------------------------------------------

using SDL_InitFlags = Uint32;
constexpr SDL_InitFlags SDL_INIT_VIDEO = 0x20u, SDL_INIT_EVENTS = 0x4000u;
bool SDL_Init(SDL_InitFlags flags);
void SDL_Quit();
const char* SDL_GetError();
bool SDL_SetError(const char* fmt, ...);
bool SDL_SetAppMetadata(const char* name, const char* version, const char* id);
// Hints: CORDINAL_VIDEO_DRIVER (or this hint) chooses wayland, x11 or offscreen.
#define SDL_HINT_VIDEO_DRIVER "SDL_VIDEO_DRIVER"
#define SDL_HINT_VIDEO_WAYLAND_PREFER_LIBDECOR "SDL_VIDEO_WAYLAND_PREFER_LIBDECOR"
bool SDL_SetHint(const char* name, const char* value);
Uint64 SDL_GetTicks();
void SDL_Delay(Uint32 ms);
void SDL_free(void* p);

// --- geometry and pixels --------------------------------------------------------------------------------------------

struct SDL_Point { int x, y; };
struct SDL_FPoint { float x, y; };
struct SDL_Rect { int x, y, w, h; };
struct SDL_FRect { float x, y, w, h; };
struct SDL_Color { Uint8 r, g, b, a; };
struct SDL_FColor { float r, g, b, a; };
struct SDL_Vertex {
    SDL_FPoint position;
    SDL_FColor color;
    SDL_FPoint tex_coord;
};
bool SDL_GetRectIntersection(const SDL_Rect* a, const SDL_Rect* b, SDL_Rect* result);

enum SDL_PixelFormat : Uint32 {
    SDL_PIXELFORMAT_UNKNOWN = 0,
    SDL_PIXELFORMAT_XRGB8888 = 0x16161804u,
    SDL_PIXELFORMAT_ARGB8888 = 0x16362004u,
    SDL_PIXELFORMAT_ABGR8888 = 0x16762004u,
};
constexpr SDL_PixelFormat SDL_PIXELFORMAT_RGBA32 = SDL_PIXELFORMAT_ABGR8888;  // little-endian

// Pixels in memory: 4 bytes each. XRGB/ARGB8888 are 0xAARRGGBB words, ABGR8888 (RGBA32) is R,G,B,A bytes.
struct SDL_Surface {
    Uint32 flags;
    SDL_PixelFormat format;
    int w, h;
    int pitch;
    void* pixels;
    int refcount;
    void* reserved;
};
SDL_Surface* SDL_CreateSurface(int width, int height, SDL_PixelFormat format);
SDL_Surface* SDL_CreateSurfaceFrom(int width, int height, SDL_PixelFormat format, void* pixels, int pitch);
void SDL_DestroySurface(SDL_Surface* surface);
bool SDL_ClearSurface(SDL_Surface* surface, float r, float g, float b, float a);
bool SDL_WriteSurfacePixel(SDL_Surface* surface, int x, int y, Uint8 r, Uint8 g, Uint8 b, Uint8 a);
bool SDL_SaveBMP(SDL_Surface* surface, const char* file);
bool SDL_SavePNG(SDL_Surface* surface, const char* file);

// --- windows --------------------------------------------------------------------------------------------------------

struct SDL_Window;
using SDL_WindowID = Uint32;
using SDL_DisplayID = Uint32;
using SDL_WindowFlags = Uint64;
constexpr SDL_WindowFlags SDL_WINDOW_FULLSCREEN = 0x1, SDL_WINDOW_HIDDEN = 0x8, SDL_WINDOW_BORDERLESS = 0x10,
                          SDL_WINDOW_RESIZABLE = 0x20, SDL_WINDOW_MINIMIZED = 0x40, SDL_WINDOW_MAXIMIZED = 0x80,
                          SDL_WINDOW_INPUT_FOCUS = 0x200, SDL_WINDOW_MOUSE_FOCUS = 0x400,
                          SDL_WINDOW_HIGH_PIXEL_DENSITY = 0x2000, SDL_WINDOW_ALWAYS_ON_TOP = 0x10000,
                          SDL_WINDOW_TRANSPARENT = 0x40000000;
constexpr int SDL_WINDOWPOS_UNDEFINED = 0x1FFF0000;
#define SDL_WINDOWPOS_ISUNDEFINED(X) (((X) & 0xFFFF0000) == 0x1FFF0000)

SDL_Window* SDL_CreateWindow(const char* title, int w, int h, SDL_WindowFlags flags);
void SDL_DestroyWindow(SDL_Window* window);
const char* SDL_GetCurrentVideoDriver();
SDL_WindowFlags SDL_GetWindowFlags(SDL_Window* window);
bool SDL_SetWindowTitle(SDL_Window* window, const char* title);
bool SDL_SetWindowIcon(SDL_Window* window, SDL_Surface* icon);
bool SDL_SetWindowSize(SDL_Window* window, int w, int h);
bool SDL_GetWindowSize(SDL_Window* window, int* w, int* h);
bool SDL_GetWindowSizeInPixels(SDL_Window* window, int* w, int* h);
bool SDL_SetWindowMinimumSize(SDL_Window* window, int w, int h);
bool SDL_SetWindowPosition(SDL_Window* window, int x, int y);
bool SDL_GetWindowPosition(SDL_Window* window, int* x, int* y);
bool SDL_SetWindowResizable(SDL_Window* window, bool resizable);
bool SDL_SetWindowBordered(SDL_Window* window, bool bordered);
bool SDL_SetWindowAlwaysOnTop(SDL_Window* window, bool onTop);
// The mini player's rounded corners: pixels with alpha 0 are cut out (X11 SHAPE; Wayland keeps the
// rectangle, as SDL's Wayland backend does without a transparent window). Null restores the rectangle.
bool SDL_SetWindowShape(SDL_Window* window, SDL_Surface* shape);
bool SDL_MaximizeWindow(SDL_Window* window);
bool SDL_RestoreWindow(SDL_Window* window);
bool SDL_RaiseWindow(SDL_Window* window);
bool SDL_HideWindow(SDL_Window* window);
bool SDL_SyncWindow(SDL_Window* window);
float SDL_GetWindowPixelDensity(SDL_Window* window);  // pixels per window coordinate
float SDL_GetWindowDisplayScale(SDL_Window* window);  // the desktop's content scale (Xft.dpi, Wayland scale)
SDL_DisplayID SDL_GetDisplayForWindow(SDL_Window* window);
bool SDL_GetDisplayUsableBounds(SDL_DisplayID display, SDL_Rect* rect);
bool SDL_UpdateWindowSurfaceRects(SDL_Window* window, const SDL_Rect* rects, int numrects);

// --- rendering ------------------------------------------------------------------------------------------------------

struct SDL_Renderer;
struct SDL_Texture;
#define SDL_SOFTWARE_RENDERER "software"
enum SDL_BlendMode : Uint32 { SDL_BLENDMODE_NONE = 0, SDL_BLENDMODE_BLEND = 1 };
enum SDL_ScaleMode : int { SDL_SCALEMODE_NEAREST = 0, SDL_SCALEMODE_LINEAR = 1 };
enum SDL_TextureAccess : int { SDL_TEXTUREACCESS_STATIC = 0, SDL_TEXTUREACCESS_STREAMING = 1 };

// Every renderer draws in software into the window's pixels; a request for another (name null or
// a GPU renderer) gets the software one too, so GetRendererName always says "software".
SDL_Renderer* SDL_CreateRenderer(SDL_Window* window, const char* name);
void SDL_DestroyRenderer(SDL_Renderer* renderer);
const char* SDL_GetRendererName(SDL_Renderer* renderer);
bool SDL_SetRenderVSync(SDL_Renderer* renderer, int vsync);
bool SDL_GetRenderOutputSize(SDL_Renderer* renderer, int* w, int* h);
bool SDL_SetRenderDrawColor(SDL_Renderer* renderer, Uint8 r, Uint8 g, Uint8 b, Uint8 a);
bool SDL_SetRenderDrawBlendMode(SDL_Renderer* renderer, SDL_BlendMode mode);
bool SDL_SetRenderClipRect(SDL_Renderer* renderer, const SDL_Rect* rect);
bool SDL_GetRenderClipRect(SDL_Renderer* renderer, SDL_Rect* rect);
bool SDL_RenderClipEnabled(SDL_Renderer* renderer);
bool SDL_RenderClear(SDL_Renderer* renderer);
bool SDL_RenderFillRect(SDL_Renderer* renderer, const SDL_FRect* rect);
bool SDL_RenderFillRects(SDL_Renderer* renderer, const SDL_FRect* rects, int count);
bool SDL_RenderRect(SDL_Renderer* renderer, const SDL_FRect* rect);
bool SDL_RenderLines(SDL_Renderer* renderer, const SDL_FPoint* points, int count);
bool SDL_RenderTexture(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_FRect* src, const SDL_FRect* dst);
bool SDL_RenderGeometry(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_Vertex* vertices, int numVertices,
                        const int* indices, int numIndices);
SDL_Surface* SDL_RenderReadPixels(SDL_Renderer* renderer, const SDL_Rect* rect);  // XRGB8888
bool SDL_FlushRenderer(SDL_Renderer* renderer);
bool SDL_RenderPresent(SDL_Renderer* renderer);

SDL_Texture* SDL_CreateTexture(SDL_Renderer* renderer, SDL_PixelFormat format, SDL_TextureAccess access, int w, int h);
void SDL_DestroyTexture(SDL_Texture* texture);
bool SDL_UpdateTexture(SDL_Texture* texture, const SDL_Rect* rect, const void* pixels, int pitch);
bool SDL_GetTextureSize(SDL_Texture* texture, float* w, float* h);
bool SDL_SetTextureBlendMode(SDL_Texture* texture, SDL_BlendMode mode);
bool SDL_SetTextureScaleMode(SDL_Texture* texture, SDL_ScaleMode mode);
bool SDL_SetTextureAlphaMod(SDL_Texture* texture, Uint8 alpha);

// --- input ----------------------------------------------------------------------------------------------------------

using SDL_Keycode = Uint32;
using SDL_Keymod = Uint16;
enum SDL_Scancode : int {
    SDL_SCANCODE_UNKNOWN = 0,
    SDL_SCANCODE_MEDIA_NEXT_TRACK = 267,
    SDL_SCANCODE_MEDIA_PREVIOUS_TRACK = 268,
    SDL_SCANCODE_MEDIA_STOP = 269,
    SDL_SCANCODE_MEDIA_PLAY_PAUSE = 271,
};
constexpr SDL_Keymod SDL_KMOD_NONE = 0, SDL_KMOD_LSHIFT = 0x1, SDL_KMOD_RSHIFT = 0x2, SDL_KMOD_LCTRL = 0x40,
                     SDL_KMOD_RCTRL = 0x80, SDL_KMOD_LALT = 0x100, SDL_KMOD_RALT = 0x200, SDL_KMOD_LGUI = 0x400,
                     SDL_KMOD_RGUI = 0x800, SDL_KMOD_NUM = 0x1000, SDL_KMOD_CAPS = 0x2000,
                     SDL_KMOD_CTRL = SDL_KMOD_LCTRL | SDL_KMOD_RCTRL, SDL_KMOD_SHIFT = SDL_KMOD_LSHIFT | SDL_KMOD_RSHIFT,
                     SDL_KMOD_ALT = SDL_KMOD_LALT | SDL_KMOD_RALT, SDL_KMOD_GUI = SDL_KMOD_LGUI | SDL_KMOD_RGUI;
#define SDLK_SCANCODE_MASK (1u << 30)
constexpr SDL_Keycode SDLK_UNKNOWN = 0, SDLK_RETURN = 0x0d, SDLK_ESCAPE = 0x1b, SDLK_BACKSPACE = 0x08, SDLK_TAB = 0x09,
                      SDLK_SPACE = 0x20, SDLK_COMMA = 0x2c, SDLK_MINUS = 0x2d, SDLK_PERIOD = 0x2e, SDLK_0 = 0x30,
                      SDLK_9 = 0x39, SDLK_EQUALS = 0x3d, SDLK_LEFTBRACKET = 0x5b, SDLK_RIGHTBRACKET = 0x5d,
                      SDLK_A = 0x61, SDLK_C = 0x63, SDLK_D = 0x64, SDLK_E = 0x65, SDLK_F = 0x66, SDLK_H = 0x68,
                      SDLK_J = 0x6a, SDLK_L = 0x6c, SDLK_M = 0x6d, SDLK_N = 0x6e, SDLK_O = 0x6f, SDLK_P = 0x70,
                      SDLK_Q = 0x71, SDLK_R = 0x72, SDLK_S = 0x73, SDLK_T = 0x74, SDLK_V = 0x76, SDLK_W = 0x77,
                      SDLK_X = 0x78, SDLK_Y = 0x79, SDLK_Z = 0x7a, SDLK_DELETE = 0x7f,
                      SDLK_F1 = 0x4000003a, SDLK_F2 = 0x4000003b, SDLK_F5 = 0x4000003e, SDLK_F6 = 0x4000003f,
                      SDLK_F10 = 0x40000043, SDLK_F12 = 0x40000045, SDLK_INSERT = 0x40000049, SDLK_HOME = 0x4000004a,
                      SDLK_PAGEUP = 0x4000004b, SDLK_END = 0x4000004d, SDLK_PAGEDOWN = 0x4000004e,
                      SDLK_RIGHT = 0x4000004f, SDLK_LEFT = 0x40000050, SDLK_DOWN = 0x40000051, SDLK_UP = 0x40000052,
                      SDLK_KP_ENTER = 0x40000058, SDLK_KP_9 = 0x40000061, SDLK_KP_0 = 0x40000062,
                      SDLK_MUTE = 0x4000007f, SDLK_VOLUMEUP = 0x40000080, SDLK_VOLUMEDOWN = 0x40000081,
                      SDLK_LALT = 0x400000e2, SDLK_RALT = 0x400000e6, SDLK_MEDIA_NEXT_TRACK = 0x4000010b,
                      SDLK_MEDIA_PREVIOUS_TRACK = 0x4000010c, SDLK_MEDIA_STOP = 0x4000010d,
                      SDLK_MEDIA_PLAY_PAUSE = 0x4000010f;
SDL_Keymod SDL_GetModState();
const char* SDL_GetKeyName(SDL_Keycode key);  // "F13" for function keys, the character otherwise
SDL_Keycode SDL_GetKeyFromName(const char* name);
bool SDL_StartTextInput(SDL_Window* window);
bool SDL_StopTextInput(SDL_Window* window);

using SDL_MouseButtonFlags = Uint32;
constexpr Uint8 SDL_BUTTON_LEFT = 1, SDL_BUTTON_MIDDLE = 2, SDL_BUTTON_RIGHT = 3, SDL_BUTTON_X1 = 4, SDL_BUTTON_X2 = 5;
constexpr SDL_MouseButtonFlags SDL_BUTTON_LMASK = 1u << 0;
SDL_MouseButtonFlags SDL_GetMouseState(float* x, float* y);
SDL_MouseButtonFlags SDL_GetGlobalMouseState(float* x, float* y);
bool SDL_CaptureMouse(bool enabled);

struct SDL_Cursor;
enum SDL_SystemCursor : int {
    SDL_SYSTEM_CURSOR_DEFAULT = 0,
    SDL_SYSTEM_CURSOR_TEXT = 1,
    SDL_SYSTEM_CURSOR_POINTER = 6,
    SDL_SYSTEM_CURSOR_EW_RESIZE = 9,
    SDL_SYSTEM_CURSOR_NS_RESIZE = 10,
};
SDL_Cursor* SDL_CreateSystemCursor(SDL_SystemCursor id);
bool SDL_SetCursor(SDL_Cursor* cursor);

// --- events ---------------------------------------------------------------------------------------------------------

enum SDL_EventType : Uint32 {
    SDL_EVENT_FIRST = 0,
    SDL_EVENT_QUIT = 0x100,
    SDL_EVENT_SYSTEM_THEME_CHANGED = 0x154,
    SDL_EVENT_WINDOW_EXPOSED = 0x204,
    SDL_EVENT_WINDOW_RESIZED = 0x206,
    SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED = 0x207,
    SDL_EVENT_WINDOW_MOUSE_ENTER = 0x20C,
    SDL_EVENT_WINDOW_MOUSE_LEAVE = 0x20D,
    SDL_EVENT_WINDOW_FOCUS_GAINED = 0x20E,
    SDL_EVENT_WINDOW_FOCUS_LOST = 0x20F,
    SDL_EVENT_WINDOW_CLOSE_REQUESTED = 0x210,
    SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED = 0x214,
    SDL_EVENT_KEY_DOWN = 0x300,
    SDL_EVENT_KEY_UP = 0x301,
    SDL_EVENT_TEXT_INPUT = 0x303,
    SDL_EVENT_MOUSE_MOTION = 0x400,
    SDL_EVENT_MOUSE_BUTTON_DOWN = 0x401,
    SDL_EVENT_MOUSE_BUTTON_UP = 0x402,
    SDL_EVENT_MOUSE_WHEEL = 0x403,
    SDL_EVENT_DROP_FILE = 0x1000,
    SDL_EVENT_DROP_TEXT = 0x1001,
    SDL_EVENT_USER = 0x8000,
    SDL_EVENT_LAST = 0xFFFF,
};

struct SDL_CommonEvent { Uint32 type, reserved; Uint64 timestamp; };
struct SDL_WindowEvent { Uint32 type, reserved; Uint64 timestamp; SDL_WindowID windowID; Sint32 data1, data2; };
struct SDL_KeyboardEvent {
    Uint32 type, reserved;
    Uint64 timestamp;
    SDL_WindowID windowID;
    Uint32 which;
    SDL_Scancode scancode;
    SDL_Keycode key;
    SDL_Keymod mod;
    Uint16 raw;
    bool down, repeat;
};
struct SDL_TextInputEvent { Uint32 type, reserved; Uint64 timestamp; SDL_WindowID windowID; const char* text; };
struct SDL_MouseMotionEvent {
    Uint32 type, reserved;
    Uint64 timestamp;
    SDL_WindowID windowID;
    Uint32 which;
    SDL_MouseButtonFlags state;
    float x, y, xrel, yrel;
};
struct SDL_MouseButtonEvent {
    Uint32 type, reserved;
    Uint64 timestamp;
    SDL_WindowID windowID;
    Uint32 which;
    Uint8 button;
    bool down;
    Uint8 clicks, padding;
    float x, y;
};
enum SDL_MouseWheelDirection { SDL_MOUSEWHEEL_NORMAL, SDL_MOUSEWHEEL_FLIPPED };
struct SDL_MouseWheelEvent {
    Uint32 type, reserved;
    Uint64 timestamp;
    SDL_WindowID windowID;
    Uint32 which;
    float x, y;
    SDL_MouseWheelDirection direction;
    float mouse_x, mouse_y;
};
struct SDL_DropEvent {
    Uint32 type, reserved;
    Uint64 timestamp;
    SDL_WindowID windowID;
    float x, y;
    const char* source;
    const char* data;
};
struct SDL_UserEvent { Uint32 type, reserved; Uint64 timestamp; SDL_WindowID windowID; Sint32 code; void* data1; void* data2; };
union SDL_Event {
    Uint32 type;
    SDL_CommonEvent common;
    SDL_WindowEvent window;
    SDL_KeyboardEvent key;
    SDL_TextInputEvent text;
    SDL_MouseMotionEvent motion;
    SDL_MouseButtonEvent button;
    SDL_MouseWheelEvent wheel;
    SDL_DropEvent drop;
    SDL_UserEvent user;
    Uint8 padding[128];
};
// Event strings (text input, drops) stay valid until the next PollEvent or WaitEvent call, as SDL's do.
bool SDL_PollEvent(SDL_Event* event);
bool SDL_WaitEventTimeout(SDL_Event* event, Sint32 timeoutMs);
bool SDL_PushEvent(SDL_Event* event);  // any thread
Uint32 SDL_RegisterEvents(int count);

// --- clipboard ------------------------------------------------------------------------------------------------------

bool SDL_SetClipboardText(const char* text);
char* SDL_GetClipboardText();  // never null: "" when there is none (free with SDL_free)
bool SDL_HasClipboardText();
void* SDL_GetClipboardData(const char* mimeType, size_t* size);  // null-terminated beyond size; SDL_free
bool SDL_HasClipboardData(const char* mimeType);

// --- desktop services -----------------------------------------------------------------------------------------------

using SDL_MessageBoxFlags = Uint32;
constexpr SDL_MessageBoxFlags SDL_MESSAGEBOX_ERROR = 0x10, SDL_MESSAGEBOX_WARNING = 0x20,
                              SDL_MESSAGEBOX_INFORMATION = 0x40;
using SDL_MessageBoxButtonFlags = Uint32;
constexpr SDL_MessageBoxButtonFlags SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT = 0x1,
                                    SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT = 0x2;
struct SDL_MessageBoxButtonData {
    SDL_MessageBoxButtonFlags flags;
    int buttonID;
    const char* text;
};
struct SDL_MessageBoxColorScheme;
struct SDL_MessageBoxData {
    SDL_MessageBoxFlags flags;
    SDL_Window* window;
    const char* title;
    const char* message;
    int numbuttons;
    const SDL_MessageBoxButtonData* buttons;
    const SDL_MessageBoxColorScheme* colorScheme;
};
// Modal: drawn over the window (or printed to stderr without one) while the call waits.
bool SDL_ShowMessageBox(const SDL_MessageBoxData* data, int* buttonid);
bool SDL_ShowSimpleMessageBox(SDL_MessageBoxFlags flags, const char* title, const char* message, SDL_Window* window);

struct SDL_DialogFileFilter {
    const char* name;
    const char* pattern;  // "png;jpg"
};
// Called once, from any thread: a null-terminated list, an empty list when cancelled, null on error.
using SDL_DialogFileCallback = void (*)(void* userdata, const char* const* filelist, int filter);
void SDL_ShowOpenFileDialog(SDL_DialogFileCallback callback, void* userdata, SDL_Window* window,
                            const SDL_DialogFileFilter* filters, int nfilters, const char* defaultLocation,
                            bool allowMany);
void SDL_ShowSaveFileDialog(SDL_DialogFileCallback callback, void* userdata, SDL_Window* window,
                            const SDL_DialogFileFilter* filters, int nfilters, const char* defaultLocation);
void SDL_ShowOpenFolderDialog(SDL_DialogFileCallback callback, void* userdata, SDL_Window* window,
                              const char* defaultLocation, bool allowMany);

bool SDL_OpenURL(const char* url);
enum SDL_Folder { SDL_FOLDER_HOME, SDL_FOLDER_DESKTOP, SDL_FOLDER_DOCUMENTS, SDL_FOLDER_DOWNLOADS, SDL_FOLDER_MUSIC,
                  SDL_FOLDER_PICTURES, SDL_FOLDER_PUBLICSHARE, SDL_FOLDER_SAVEDGAMES, SDL_FOLDER_SCREENSHOTS,
                  SDL_FOLDER_TEMPLATES, SDL_FOLDER_VIDEOS };
const char* SDL_GetUserFolder(SDL_Folder folder);  // with a trailing slash, as SDL's
enum SDL_SystemTheme { SDL_SYSTEM_THEME_UNKNOWN, SDL_SYSTEM_THEME_LIGHT, SDL_SYSTEM_THEME_DARK };
SDL_SystemTheme SDL_GetSystemTheme();
