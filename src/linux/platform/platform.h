// The platform layer's inside: what render.cpp, desktop.cpp and the window systems' backends share.
// sdl_api.h is the face the player sees; this is the plumbing behind it.
#pragma once

#include "platform/sdl_api.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <string>
#include <vector>

namespace lx::platform {

enum class Cursor { Arrow, Text, Pointer, ResizeEW, ResizeNS };

// One top-level window of a backend. Coordinates are window points; the framebuffer is in pixels
// (points times the density, which only Wayland's fractional scaling makes other than 1).
class NativeWindow {
public:
    virtual ~NativeWindow() = default;
    virtual void SetTitle(const std::string& title) = 0;
    virtual void SetIcon(int w, int h, const uint32_t* argb) = 0;  // 0xAARRGGBB
    virtual void SetSize(int w, int h) = 0;
    virtual void SetMinimumSize(int w, int h) = 0;
    virtual bool SetPosition(int x, int y) = 0;  // false where the window system doesn't allow it
    virtual bool GetPosition(int& x, int& y) = 0;
    virtual void SetResizable(bool on) = 0;
    virtual void SetBordered(bool on) = 0;
    virtual void SetAlwaysOnTop(bool on) = 0;
    virtual bool SetShape(int w, int h, const uint8_t* alpha) = 0;  // null: the full rectangle
    virtual void Maximize() = 0;
    virtual void Restore() = 0;
    virtual void Raise() = 0;
    virtual void Hide() = 0;
    virtual void Sync() = 0;
    // The pixels to draw into for the window's current pixel size (0xFFRRGGBB words), stride in words.
    virtual uint32_t* Framebuffer(int& w, int& h, int& stride) = 0;
    virtual void Present(const SDL_Rect* rects, int count) = 0;  // null: everything
    virtual void SetCursor(Cursor c) = 0;
    virtual void SetTextInput(bool on, const SDL_Rect* area) = 0;
    virtual std::string PortalHandle() { return {}; }  // "x11:1a00004" for the portal's parent_window
};

class Backend {
public:
    virtual ~Backend() = default;
    virtual const char* Name() const = 0;
    virtual std::unique_ptr<NativeWindow> CreateWindow(SDL_Window* owner, const std::string& title, int w, int h,
                                                       SDL_WindowFlags flags) = 0;
    virtual int Fd() const = 0;  // -1: nothing to poll (offscreen)
    virtual void Dispatch() = 0;  // reads what is waiting without blocking and queues events
    virtual void Flush() {}
    virtual int TimeoutMs() const { return -1; }  // until a backend timer (key repeat) is due
    virtual bool UsableBounds(SDL_Rect& r) = 0;
    virtual bool GlobalMouse(float& x, float& y) = 0;
    virtual void CaptureMouse(bool on) {}
    virtual bool SetClipboard(const std::string& text) = 0;
    virtual std::vector<std::string> ClipboardTypes() = 0;
    virtual std::optional<std::string> GetClipboard(const std::string& mime) = 0;
};

std::unique_ptr<Backend> CreateWayland(std::string& error);
std::unique_ptr<Backend> CreateX11(std::string& error);
std::unique_ptr<Backend> CreateOffscreen();

// --- the shared state the backends feed ---------------------------------------------------------------------------

// Queues an event (any thread). `text` is copied and becomes e.text.text / e.drop.data when popped.
void PushEvent(const SDL_Event& e, std::string text = {});
void WakeEventLoop();
// Runs `fn` on the main thread at its next event poll outside another such task (any thread).
void RunOnMainThread(std::function<void()> fn);
SDL_Window* WindowFromNative(NativeWindow* w);
SDL_Window* WindowById(SDL_WindowID id);  // main thread

// Window state the backend reports; each sends the matching events.
void OnWindowResized(SDL_Window* w, int pointsW, int pointsH, int pixelsW, int pixelsH);
void OnDisplayScale(SDL_Window* w, float scale);
void OnWindowState(SDL_Window* w, SDL_WindowFlags set, SDL_WindowFlags clear);
void OnFocus(SDL_Window* w, bool focused);
void OnMouseEnterLeave(SDL_Window* w, bool inside);
void OnCloseRequest(SDL_Window* w);
void OnExposed(SDL_Window* w);

// Input, in window points.
void OnMouseMotion(SDL_Window* w, float x, float y);
void OnMouseButton(SDL_Window* w, int button, bool down, float x, float y);
void OnMouseWheel(SDL_Window* w, float dx, float dy);
// A key: SDL keycode and scancode from the backend's keymap, the modifiers after this key, and the
// text it types (empty for none). Repeats come with repeat = true.
void OnKey(SDL_Window* w, SDL_Scancode sc, SDL_Keycode key, bool down, bool repeat, const std::string& text);
// The window system's view of the modifiers (X11's state field, Wayland's modifiers event): groups it
// says are up lose their side bits; a group it says is down without a side gets the left one.
void SyncModifiers(bool shift, bool ctrl, bool alt, bool gui, bool caps, bool num);
void OnText(SDL_Window* w, const std::string& text);  // an input method's committed text
void OnDrop(SDL_Window* w, bool file, const std::string& data, float x, float y);
void OnThemeChanged();
bool TextInputActive();

// Keys (keys.cpp). X11 and XKB share keysyms; both number keys as evdev codes plus 8.
SDL_Scancode ScancodeFromEvdev(uint32_t evdev);
SDL_Scancode ScancodeFromKeysym(uint32_t keysym);  // for keys the evdev table doesn't know
// SDL's keycode for a key: the character its unmodified level types (Latin letters and digits
// kept on layouts whose base level has other scripts, as SDL's default keycode options do), else
// the scancode's own keycode.
SDL_Keycode KeycodeFor(SDL_Scancode sc, uint32_t baseKeysym, uint32_t shiftKeysym);
uint32_t KeysymToUnicode(uint32_t keysym);  // 0 for none
uint32_t KeysymFromUnicode(uint32_t codepoint);  // the legacy keysym when there is one
uint32_t KeysymFromName(std::string_view name);  // 0 for none; "a", "Shift_L", "U0441", "0x1008ff14"
bool IsKeypadKeysym(uint32_t keysym);
std::string Utf8(uint32_t codepoint);

// An Xcursor theme's file for a cursor name, following XCURSOR_PATH and index.theme's Inherits
// (x11.cpp; Wayland uses it without cursor-shape-v1).
std::optional<std::string> FindXcursorFile(const std::string& theme, const std::string& name, int depth = 0);

// The software renderer's hooks for the window (render.cpp).
void RendererWindowGone(SDL_Window* w);

// For message boxes and the fallback file chooser (desktop.cpp): the window's renderer, if any.
SDL_Renderer* RendererForWindow(SDL_Window* w);
NativeWindow* Native(SDL_Window* w);
Backend* CurrentBackend();
void SetError(const std::string& text);

// The in-window modal pieces (overlay.cpp), main thread only. A message box returns the chosen
// button id, or the escape default when the window closes; false when there is no window to draw in.
bool OverlayMessageBox(const SDL_MessageBoxData& data, int& button);
// The file chooser drawn in the window when there is no portal: the chosen paths, empty when
// cancelled; nullopt when it cannot be shown.
enum class ChooserKind { OpenFile, SaveFile, OpenFolder };
struct ChooserRequest {
    ChooserKind kind;
    bool many = false;
    std::string title, location;
    std::vector<std::pair<std::string, std::string>> filters;  // name, "png;jpg"
};
std::optional<std::vector<std::string>> OverlayFileChooser(SDL_Window* window, const ChooserRequest& request);

// Writes an XRGB8888 / ARGB8888 picture (render.cpp).
bool WritePng(const std::string& path, int w, int h, const uint32_t* px, int stride, bool alpha);
bool WriteBmp(const std::string& path, int w, int h, const uint32_t* px, int stride);

}  // namespace lx::platform

struct SDL_Window {
    SDL_WindowID id = 0;
    std::unique_ptr<lx::platform::NativeWindow> native;
    std::string title;
    SDL_WindowFlags flags = 0;
    int w = 0, h = 0;    // points
    int pw = 0, ph = 0;  // pixels
    float displayScale = 1;
    SDL_Renderer* renderer = nullptr;
};
