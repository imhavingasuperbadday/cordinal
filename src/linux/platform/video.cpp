// The core of the platform layer: start-up and the choice of window system, the event queue that
// window systems and worker threads feed, mouse and keyboard state, windows, cursors and the
// clipboard calls, all on the main thread except PushEvent. Also the offscreen "window system" that
// --check-layout and --capture-ui draw with.
#include "platform/platform.h"

#include <poll.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

struct SDL_Cursor {
    lx::platform::Cursor kind;
};

namespace lx::platform {
namespace {

thread_local std::string t_error;

struct Queued {
    SDL_Event e;
    std::string text;
    bool hasText = false;
};

struct State {
    std::mutex mu;  // the queue and the hints; everything else belongs to the main thread
    std::deque<Queued> queue;
    std::map<std::string, std::string> hints;
    int wakeFd = -1;
    std::atomic<uint32_t> nextUser{SDL_EVENT_USER};

    std::unique_ptr<Backend> backend;
    std::string driver;
    bool videoUp = false;
    std::vector<SDL_Window*> windows;
    SDL_WindowID nextWindow = 1;
    std::deque<std::string> held;  // the strings of the events handed out last
    std::vector<std::function<void()>> tasks;  // under mu
    bool inTask = false;

    SDL_Keymod mod = 0;
    SDL_MouseButtonFlags buttons = 0;
    float mx = 0, my = 0;
    SDL_Window* mouseFocus = nullptr;
    SDL_Window* keyFocus = nullptr;
    bool textInput = false;
    SDL_Cursor* cursor = nullptr;
    // Double clicks: SDL's defaults, 500 ms within 32 pixels.
    struct Click {
        uint64_t at = 0;
        float x = 0, y = 0;
        int count = 0;
    } clicks[6];
};

State& S() {
    static State* s = new State;  // never destroyed: worker threads may post during exit
    return *s;
}

uint64_t NowNs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
const uint64_t g_start = NowNs();

void Wake() {
    State& s = S();
    if (s.wakeFd >= 0) {
        uint64_t one = 1;
        (void)!write(s.wakeFd, &one, sizeof one);
    }
}

std::string Hint(const char* name) {
    std::lock_guard lock(S().mu);
    auto it = S().hints.find(name);
    return it == S().hints.end() ? std::string() : it->second;
}

SDL_Event Base(uint32_t type, SDL_Window* w) {
    SDL_Event e{};
    e.type = type;
    e.common.timestamp = NowNs() - g_start;
    if (w) e.window.windowID = w->id;  // every per-window event keeps windowID at the same offset
    return e;
}

// --- offscreen ----------------------------------------------------------------------------------------------------

class OffscreenWindow : public NativeWindow {
public:
    OffscreenWindow(SDL_Window* owner) : owner_(owner) {}
    void SetTitle(const std::string&) override {}
    void SetIcon(int, int, const uint32_t*) override {}
    void SetSize(int w, int h) override { OnWindowResized(owner_, w, h, w, h); }
    void SetMinimumSize(int, int) override {}
    bool SetPosition(int x, int y) override {
        x_ = x, y_ = y;
        return true;
    }
    bool GetPosition(int& x, int& y) override {
        x = x_, y = y_;
        return true;
    }
    void SetResizable(bool) override {}
    void SetBordered(bool) override {}
    void SetAlwaysOnTop(bool) override {}
    bool SetShape(int, int, const uint8_t*) override { return true; }
    void Maximize() override {}
    void Restore() override {}
    void Raise() override {}
    void Hide() override {}
    void Sync() override {}
    uint32_t* Framebuffer(int& w, int& h, int& stride) override {
        w = owner_->pw, h = owner_->ph, stride = w;
        if (px_.size() != (size_t)w * h) px_.assign((size_t)w * h, 0xff000000u);
        return px_.data();
    }
    void Present(const SDL_Rect*, int) override {}
    void SetCursor(Cursor) override {}
    void SetTextInput(bool, const SDL_Rect*) override {}

private:
    SDL_Window* owner_;
    std::vector<uint32_t> px_;
    int x_ = 0, y_ = 0;
};

class Offscreen : public Backend {
public:
    const char* Name() const override { return "offscreen"; }
    std::unique_ptr<NativeWindow> CreateWindow(SDL_Window* owner, const std::string&, int, int, SDL_WindowFlags) override {
        return std::make_unique<OffscreenWindow>(owner);
    }
    int Fd() const override { return -1; }
    void Dispatch() override {}
    bool UsableBounds(SDL_Rect& r) override {
        r = {0, 0, 1920, 1080};
        return true;
    }
    bool GlobalMouse(float& x, float& y) override {
        x = S().mx, y = S().my;
        return true;
    }
    bool SetClipboard(const std::string& text) override {
        clip_ = text;
        return true;
    }
    std::vector<std::string> ClipboardTypes() override {
        if (clip_.empty()) return {};
        return {"text/plain;charset=utf-8"};
    }
    std::optional<std::string> GetClipboard(const std::string& mime) override {
        if (clip_.empty() || mime.rfind("text/plain", 0) != 0) return std::nullopt;
        return clip_;
    }

private:
    std::string clip_;
};

SDL_Window* Find(SDL_WindowID id) {
    for (SDL_Window* w : S().windows)
        if (w->id == id) return w;
    return nullptr;
}

bool Pop(SDL_Event* out) {
    State& s = S();
    std::lock_guard lock(s.mu);
    if (s.queue.empty()) return false;
    Queued q = std::move(s.queue.front());
    s.queue.pop_front();
    if (q.hasText) {
        s.held.push_back(std::move(q.text));
        const char* p = s.held.back().c_str();
        if (q.e.type == SDL_EVENT_TEXT_INPUT) q.e.text.text = p;
        else if (q.e.type == SDL_EVENT_DROP_FILE || q.e.type == SDL_EVENT_DROP_TEXT) q.e.drop.data = p;
    }
    if (out) *out = q.e;
    else s.held.clear();
    return true;
}

void RunTasks() {
    State& s = S();
    if (s.inTask) return;
    std::vector<std::function<void()>> tasks;
    {
        std::lock_guard lock(s.mu);
        tasks.swap(s.tasks);
    }
    s.inTask = true;
    for (auto& t : tasks) t();
    s.inTask = false;
}

void Pump() {
    State& s = S();
    RunTasks();
    if (s.wakeFd >= 0) {
        uint64_t v;
        while (read(s.wakeFd, &v, sizeof v) > 0) {}
    }
    if (s.backend) {
        s.backend->Dispatch();
        s.backend->Flush();
    }
}

}  // namespace

void SetError(const std::string& text) { t_error = text; }
Backend* CurrentBackend() { return S().backend.get(); }
NativeWindow* Native(SDL_Window* w) { return w ? w->native.get() : nullptr; }
bool TextInputActive() { return S().textInput; }
void WakeEventLoop() { Wake(); }

void RunOnMainThread(std::function<void()> fn) {
    {
        std::lock_guard lock(S().mu);
        S().tasks.push_back(std::move(fn));
    }
    Wake();
}

SDL_Window* WindowById(SDL_WindowID id) { return id ? Find(id) : nullptr; }

SDL_Window* WindowFromNative(NativeWindow* n) {
    for (SDL_Window* w : S().windows)
        if (w->native.get() == n) return w;
    return nullptr;
}

void PushEvent(const SDL_Event& e, std::string text) {
    State& s = S();
    {
        std::lock_guard lock(s.mu);
        if (s.queue.size() > 65535) return;  // SDL's cap: a stuck main thread doesn't eat the memory
        Queued q{e, std::move(text), false};
        q.hasText = e.type == SDL_EVENT_TEXT_INPUT || e.type == SDL_EVENT_DROP_FILE || e.type == SDL_EVENT_DROP_TEXT;
        s.queue.push_back(std::move(q));
    }
    Wake();
}

void OnWindowResized(SDL_Window* w, int pointsW, int pointsH, int pixelsW, int pixelsH) {
    if (!w) return;
    pointsW = std::max(pointsW, 1), pointsH = std::max(pointsH, 1);
    pixelsW = std::max(pixelsW, 1), pixelsH = std::max(pixelsH, 1);
    if (pointsW != w->w || pointsH != w->h) {
        w->w = pointsW, w->h = pointsH;
        SDL_Event e = Base(SDL_EVENT_WINDOW_RESIZED, w);
        e.window.data1 = pointsW, e.window.data2 = pointsH;
        PushEvent(e);
    }
    if (pixelsW != w->pw || pixelsH != w->ph) {
        w->pw = pixelsW, w->ph = pixelsH;
        SDL_Event e = Base(SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED, w);
        e.window.data1 = pixelsW, e.window.data2 = pixelsH;
        PushEvent(e);
    }
}

void OnDisplayScale(SDL_Window* w, float scale) {
    if (!w || scale <= 0 || std::fabs(scale - w->displayScale) < 1e-4f) return;
    w->displayScale = scale;
    PushEvent(Base(SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED, w));
}

void OnWindowState(SDL_Window* w, SDL_WindowFlags set, SDL_WindowFlags clear) {
    if (w) w->flags = (w->flags & ~clear) | set;
}

void OnFocus(SDL_Window* w, bool focused) {
    if (!w) return;
    State& s = S();
    if (focused == ((w->flags & SDL_WINDOW_INPUT_FOCUS) != 0)) return;
    OnWindowState(w, focused ? SDL_WINDOW_INPUT_FOCUS : 0, focused ? 0 : SDL_WINDOW_INPUT_FOCUS);
    s.keyFocus = focused ? w : (s.keyFocus == w ? nullptr : s.keyFocus);
    if (!focused) s.mod &= (SDL_KMOD_NUM | SDL_KMOD_CAPS);
    PushEvent(Base(focused ? SDL_EVENT_WINDOW_FOCUS_GAINED : SDL_EVENT_WINDOW_FOCUS_LOST, w));
}

void OnMouseEnterLeave(SDL_Window* w, bool inside) {
    if (!w) return;
    State& s = S();
    if (inside == ((w->flags & SDL_WINDOW_MOUSE_FOCUS) != 0)) return;
    OnWindowState(w, inside ? SDL_WINDOW_MOUSE_FOCUS : 0, inside ? 0 : SDL_WINDOW_MOUSE_FOCUS);
    if (inside) {
        s.mouseFocus = w;
        if (s.cursor && w->native) w->native->SetCursor(s.cursor->kind);
    } else if (s.mouseFocus == w) {
        s.mouseFocus = nullptr;
    }
    PushEvent(Base(inside ? SDL_EVENT_WINDOW_MOUSE_ENTER : SDL_EVENT_WINDOW_MOUSE_LEAVE, w));
}

void OnCloseRequest(SDL_Window* w) {
    if (w) PushEvent(Base(SDL_EVENT_WINDOW_CLOSE_REQUESTED, w));
}

void OnExposed(SDL_Window* w) {
    if (w) PushEvent(Base(SDL_EVENT_WINDOW_EXPOSED, w));
}

void OnMouseMotion(SDL_Window* w, float x, float y) {
    State& s = S();
    if (!w) return;
    if (!(w->flags & SDL_WINDOW_MOUSE_FOCUS)) OnMouseEnterLeave(w, true);
    if (x == s.mx && y == s.my && s.mouseFocus == w) return;
    SDL_Event e = Base(SDL_EVENT_MOUSE_MOTION, w);
    e.motion.state = s.buttons;
    e.motion.x = x, e.motion.y = y;
    e.motion.xrel = x - s.mx, e.motion.yrel = y - s.my;
    s.mx = x, s.my = y;
    PushEvent(e);
}

void OnMouseButton(SDL_Window* w, int button, bool down, float x, float y) {
    State& s = S();
    if (!w || button < 1 || button > 5) return;
    if (x != s.mx || y != s.my) OnMouseMotion(w, x, y);
    const SDL_MouseButtonFlags bit = 1u << (button - 1);
    if (down == ((s.buttons & bit) != 0)) return;
    s.buttons = down ? s.buttons | bit : s.buttons & ~bit;
    SDL_Event e = Base(down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP, w);
    e.button.button = (Uint8)button;
    e.button.down = down;
    e.button.x = x, e.button.y = y;
    auto& c = s.clicks[button];
    if (down) {
        const uint64_t now = NowNs() / 1000000;
        if (c.count > 0 && now - c.at <= 500 && std::fabs(x - c.x) <= 32 && std::fabs(y - c.y) <= 32) ++c.count;
        else c.count = 1;
        c.at = now, c.x = x, c.y = y;
    }
    e.button.clicks = (Uint8)std::min(c.count, 255);
    PushEvent(e);
}

void OnMouseWheel(SDL_Window* w, float dx, float dy) {
    if (!w || (dx == 0 && dy == 0)) return;
    SDL_Event e = Base(SDL_EVENT_MOUSE_WHEEL, w);
    e.wheel.x = dx, e.wheel.y = dy;
    e.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    e.wheel.mouse_x = S().mx, e.wheel.mouse_y = S().my;
    PushEvent(e);
}

void SyncModifiers(bool shift, bool ctrl, bool alt, bool gui, bool caps, bool num) {
    SDL_Keymod& m = S().mod;
    auto group = [&](bool on, SDL_Keymod both, SDL_Keymod left) {
        if (!on) m &= (SDL_Keymod)~both;
        else if (!(m & both)) m |= left;
    };
    group(shift, SDL_KMOD_SHIFT, SDL_KMOD_LSHIFT);
    group(ctrl, SDL_KMOD_CTRL, SDL_KMOD_LCTRL);
    group(alt, SDL_KMOD_ALT, SDL_KMOD_LALT);
    group(gui, SDL_KMOD_GUI, SDL_KMOD_LGUI);
    m = (SDL_Keymod)((m & ~(SDL_KMOD_CAPS | SDL_KMOD_NUM)) | (caps ? SDL_KMOD_CAPS : 0) | (num ? SDL_KMOD_NUM : 0));
}

void OnKey(SDL_Window* w, SDL_Scancode sc, SDL_Keycode key, bool down, bool repeat, const std::string& text) {
    State& s = S();
    if (!w) w = s.keyFocus;
    if (!w) return;
    // Modifier keys change the state before their own event, as in SDL.
    static constexpr SDL_Keymod kSide[8] = {SDL_KMOD_LCTRL, SDL_KMOD_LSHIFT, SDL_KMOD_LALT, SDL_KMOD_LGUI,
                                            SDL_KMOD_RCTRL, SDL_KMOD_RSHIFT, SDL_KMOD_RALT, SDL_KMOD_RGUI};
    if (sc >= 224 && sc <= 231) {
        const SDL_Keymod bit = kSide[sc - 224];
        s.mod = down ? (SDL_Keymod)(s.mod | bit) : (SDL_Keymod)(s.mod & ~bit);
    }
    SDL_Event e = Base(down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP, w);
    e.key.scancode = sc;
    e.key.key = key;
    e.key.mod = s.mod;
    e.key.down = down;
    e.key.repeat = repeat;
    PushEvent(e);
    // As SDL: Ctrl or Alt chords (without AltGr) type nothing.
    const bool chord = (s.mod & SDL_KMOD_CTRL) || ((s.mod & SDL_KMOD_LALT) && !(s.mod & SDL_KMOD_RALT));
    if (down && !text.empty() && !chord) OnText(w, text);
}

void OnText(SDL_Window* w, const std::string& text) {
    if (!S().textInput || text.empty() || !w) return;
    // Control characters are keys, not text.
    std::string clean;
    for (unsigned char c : text)
        if (c >= 0x20 && c != 0x7f) clean.push_back((char)c);
    if (clean.empty()) return;
    PushEvent(Base(SDL_EVENT_TEXT_INPUT, w), std::move(clean));
}

void OnDrop(SDL_Window* w, bool file, const std::string& data, float x, float y) {
    if (!w) return;
    SDL_Event e = Base(file ? SDL_EVENT_DROP_FILE : SDL_EVENT_DROP_TEXT, w);
    e.drop.x = x, e.drop.y = y;
    PushEvent(e, data);
}

void OnThemeChanged() { PushEvent(Base(SDL_EVENT_SYSTEM_THEME_CHANGED, nullptr)); }

std::unique_ptr<Backend> CreateOffscreen() { return std::make_unique<Offscreen>(); }

}  // namespace lx::platform

using namespace lx::platform;

// --- basics -----------------------------------------------------------------------------------------------------

const char* SDL_GetError() { return t_error.c_str(); }

bool SDL_SetError(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    t_error = buf;
    return false;
}

bool SDL_SetAppMetadata(const char*, const char*, const char*) { return true; }

bool SDL_SetHint(const char* name, const char* value) {
    if (!name) return false;
    std::lock_guard lock(S().mu);
    if (value) S().hints[name] = value;
    else S().hints.erase(name);
    return true;
}

bool SDL_Init(SDL_InitFlags flags) {
    State& s = S();
    if (s.wakeFd < 0) s.wakeFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (!(flags & SDL_INIT_VIDEO) || s.videoUp) return true;
    // CORDINAL_VIDEO_DRIVER (or SDL_VIDEO_DRIVER, the variable users know) picks one; the program's
    // hint wins. Otherwise Wayland when there is a compositor, then X11, as SDL chooses.
    std::string want = Hint(SDL_HINT_VIDEO_DRIVER);
    if (want.empty())
        if (const char* v = getenv("CORDINAL_VIDEO_DRIVER")) want = v;
    if (want.empty())
        if (const char* v = getenv("SDL_VIDEO_DRIVER")) want = v;
    std::vector<std::string> order;
    if (!want.empty()) {
        for (size_t i = 0; i <= want.size();) {  // a comma-separated list, as SDL takes
            size_t j = want.find(',', i);
            if (j == std::string::npos) j = want.size();
            if (j > i) order.push_back(want.substr(i, j - i));
            i = j + 1;
        }
    } else {
        order = {"wayland", "x11"};
    }
    std::string errors;
    for (const std::string& name : order) {
        std::string err;
        std::unique_ptr<Backend> b;
        if (name == "offscreen") b = CreateOffscreen();
        else if (name == "wayland") b = CreateWayland(err);
        else if (name == "x11") b = CreateX11(err);
        else err = "unknown video driver";
        if (b) {
            s.backend = std::move(b);
            s.driver = name;
            s.videoUp = true;
            return true;
        }
        if (!want.empty() || !err.empty()) errors += (errors.empty() ? "" : "; ") + name + ": " + err;
    }
    SetError(errors.empty() ? "No available video device" : "No available video device (" + errors + ")");
    return false;
}

void SDL_Quit() {
    State& s = S();
    while (!s.windows.empty()) SDL_DestroyWindow(s.windows.back());
    s.backend.reset();
    s.videoUp = false;
}

const char* SDL_GetCurrentVideoDriver() { return S().videoUp ? S().driver.c_str() : nullptr; }

Uint64 SDL_GetTicks() { return (NowNs() - g_start) / 1000000; }

void SDL_Delay(Uint32 ms) {
    timespec ts{(time_t)(ms / 1000), (long)(ms % 1000) * 1000000};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
}

void SDL_free(void* p) { free(p); }

bool SDL_GetRectIntersection(const SDL_Rect* a, const SDL_Rect* b, SDL_Rect* result) {
    if (!a || !b || !result) return false;
    if (a->w <= 0 || a->h <= 0 || b->w <= 0 || b->h <= 0) {
        result->w = result->h = 0;
        return false;
    }
    int x0 = std::max(a->x, b->x), x1 = std::min(a->x + a->w, b->x + b->w);
    int y0 = std::max(a->y, b->y), y1 = std::min(a->y + a->h, b->y + b->h);
    *result = {x0, y0, x1 - x0, y1 - y0};
    return result->w > 0 && result->h > 0;
}

// --- events -----------------------------------------------------------------------------------------------------

bool SDL_PollEvent(SDL_Event* event) {
    S().held.clear();
    if (Pop(event)) return true;
    Pump();
    return Pop(event);
}

bool SDL_WaitEventTimeout(SDL_Event* event, Sint32 timeoutMs) {
    State& s = S();
    s.held.clear();
    const uint64_t deadline = timeoutMs < 0 ? UINT64_MAX : SDL_GetTicks() + (uint64_t)timeoutMs;
    for (;;) {
        if (Pop(event)) return true;
        Pump();
        if (Pop(event)) return true;
        const uint64_t now = SDL_GetTicks();
        if (now >= deadline) return false;
        int wait = deadline == UINT64_MAX ? -1 : (int)std::min<uint64_t>(deadline - now, 1 << 30);
        const int backendWait = s.backend ? s.backend->TimeoutMs() : -1;
        if (backendWait >= 0 && (wait < 0 || backendWait < wait)) wait = backendWait;
        pollfd fds[2];
        int n = 0;
        if (s.wakeFd >= 0) fds[n++] = {s.wakeFd, POLLIN, 0};
        if (s.backend && s.backend->Fd() >= 0) fds[n++] = {s.backend->Fd(), POLLIN, 0};
        if (poll(fds, (nfds_t)n, wait) < 0 && errno != EINTR) return false;
    }
}

bool SDL_PushEvent(SDL_Event* event) {
    if (!event) return false;
    SDL_Event e = *event;
    if (!e.common.timestamp) e.common.timestamp = NowNs() - g_start;
    PushEvent(e);
    return true;
}

Uint32 SDL_RegisterEvents(int count) {
    if (count <= 0) return 0;
    uint32_t first = S().nextUser.fetch_add((uint32_t)count);
    if (first + (uint32_t)count > SDL_EVENT_LAST + 1u) return 0;
    return first;
}

// --- windows ----------------------------------------------------------------------------------------------------

SDL_Window* SDL_CreateWindow(const char* title, int w, int h, SDL_WindowFlags flags) {
    State& s = S();
    if (!s.backend) {
        SetError("Video subsystem has not been initialized");
        return nullptr;
    }
    auto* win = new SDL_Window;
    win->id = s.nextWindow++;
    win->title = title ? title : "";
    win->flags = flags & ~(SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS);
    win->w = win->pw = std::max(w, 1);
    win->h = win->ph = std::max(h, 1);
    s.windows.push_back(win);
    win->native = s.backend->CreateWindow(win, win->title, win->w, win->h, flags);
    if (!win->native) {
        s.windows.pop_back();
        delete win;
        if (t_error.empty()) SetError("Couldn't create the window");
        return nullptr;
    }
    return win;
}

void SDL_DestroyWindow(SDL_Window* w) {
    if (!w) return;
    State& s = S();
    RendererWindowGone(w);
    w->native.reset();
    std::erase(s.windows, w);
    if (s.mouseFocus == w) s.mouseFocus = nullptr;
    if (s.keyFocus == w) s.keyFocus = nullptr;
    {  // events for it go too
        std::lock_guard lock(s.mu);
        std::erase_if(s.queue, [&](const Queued& q) { return q.e.type >= 0x200 && q.e.type < 0x1100 && q.e.window.windowID == w->id; });
    }
    delete w;
}

SDL_WindowFlags SDL_GetWindowFlags(SDL_Window* w) { return w ? w->flags : 0; }

bool SDL_SetWindowTitle(SDL_Window* w, const char* title) {
    if (!w || !w->native) return false;
    w->title = title ? title : "";
    w->native->SetTitle(w->title);
    return true;
}

bool SDL_SetWindowIcon(SDL_Window* w, SDL_Surface* icon) {
    if (!w || !w->native || !icon) return false;
    std::vector<uint32_t> argb((size_t)icon->w * icon->h);
    for (int y = 0; y < icon->h; ++y)
        for (int x = 0; x < icon->w; ++x) {
            uint32_t p = *(const uint32_t*)((const uint8_t*)icon->pixels + (size_t)y * icon->pitch + x * 4);
            if (icon->format == SDL_PIXELFORMAT_ABGR8888) p = (p & 0xff00ff00u) | (p & 0xff) << 16 | (p >> 16 & 0xff);
            else if (icon->format == SDL_PIXELFORMAT_XRGB8888) p |= 0xff000000u;
            argb[(size_t)y * icon->w + x] = p;
        }
    w->native->SetIcon(icon->w, icon->h, argb.data());
    return true;
}

bool SDL_SetWindowSize(SDL_Window* w, int width, int height) {
    if (!w || !w->native || width <= 0 || height <= 0) return false;
    w->native->SetSize(width, height);
    return true;
}

bool SDL_GetWindowSize(SDL_Window* w, int* width, int* height) {
    if (!w) return false;
    if (width) *width = w->w;
    if (height) *height = w->h;
    return true;
}

bool SDL_GetWindowSizeInPixels(SDL_Window* w, int* width, int* height) {
    if (!w) return false;
    if (width) *width = w->pw;
    if (height) *height = w->ph;
    return true;
}

bool SDL_SetWindowMinimumSize(SDL_Window* w, int width, int height) {
    if (!w || !w->native) return false;
    w->native->SetMinimumSize(width, height);
    return true;
}

bool SDL_SetWindowPosition(SDL_Window* w, int x, int y) {
    if (!w || !w->native) return false;
    if (SDL_WINDOWPOS_ISUNDEFINED(x) || SDL_WINDOWPOS_ISUNDEFINED(y)) return true;
    if (!w->native->SetPosition(x, y)) return SDL_SetError("That operation is not supported");
    return true;
}

bool SDL_GetWindowPosition(SDL_Window* w, int* x, int* y) {
    if (!w || !w->native) return false;
    int px = 0, py = 0;
    bool ok = w->native->GetPosition(px, py);
    if (x) *x = px;
    if (y) *y = py;
    return ok;
}

bool SDL_SetWindowResizable(SDL_Window* w, bool on) {
    if (!w || !w->native) return false;
    OnWindowState(w, on ? SDL_WINDOW_RESIZABLE : 0, on ? 0 : SDL_WINDOW_RESIZABLE);
    w->native->SetResizable(on);
    return true;
}

bool SDL_SetWindowBordered(SDL_Window* w, bool on) {
    if (!w || !w->native) return false;
    OnWindowState(w, on ? 0 : SDL_WINDOW_BORDERLESS, on ? SDL_WINDOW_BORDERLESS : 0);
    w->native->SetBordered(on);
    return true;
}

bool SDL_SetWindowAlwaysOnTop(SDL_Window* w, bool on) {
    if (!w || !w->native) return false;
    OnWindowState(w, on ? SDL_WINDOW_ALWAYS_ON_TOP : 0, on ? 0 : SDL_WINDOW_ALWAYS_ON_TOP);
    w->native->SetAlwaysOnTop(on);
    return true;
}

bool SDL_SetWindowShape(SDL_Window* w, SDL_Surface* shape) {
    if (!w || !w->native) return false;
    if (!shape) return w->native->SetShape(0, 0, nullptr);
    std::vector<uint8_t> alpha((size_t)shape->w * shape->h);
    for (int y = 0; y < shape->h; ++y)
        for (int x = 0; x < shape->w; ++x) {
            uint32_t p = *(const uint32_t*)((const uint8_t*)shape->pixels + (size_t)y * shape->pitch + x * 4);
            alpha[(size_t)y * shape->w + x] = shape->format == SDL_PIXELFORMAT_XRGB8888 ? 255 : (uint8_t)(p >> 24);
        }
    if (!w->native->SetShape(shape->w, shape->h, alpha.data())) return SDL_SetError("Window shapes need a transparent window");
    return true;
}

bool SDL_MaximizeWindow(SDL_Window* w) {
    if (!w || !w->native) return false;
    w->native->Maximize();
    return true;
}

bool SDL_RestoreWindow(SDL_Window* w) {
    if (!w || !w->native) return false;
    w->native->Restore();
    return true;
}

bool SDL_RaiseWindow(SDL_Window* w) {
    if (!w || !w->native) return false;
    w->native->Raise();
    return true;
}

bool SDL_HideWindow(SDL_Window* w) {
    if (!w || !w->native) return false;
    w->native->Hide();
    OnWindowState(w, SDL_WINDOW_HIDDEN, 0);
    return true;
}

bool SDL_SyncWindow(SDL_Window* w) {
    if (!w || !w->native) return false;
    w->native->Sync();
    Pump();
    return true;
}

float SDL_GetWindowPixelDensity(SDL_Window* w) {
    if (!w || w->w <= 0) return 0;
    return (float)w->pw / (float)w->w;
}

float SDL_GetWindowDisplayScale(SDL_Window* w) { return w ? w->displayScale : 0; }

SDL_DisplayID SDL_GetDisplayForWindow(SDL_Window* w) { return w ? 1 : 0; }

bool SDL_GetDisplayUsableBounds(SDL_DisplayID display, SDL_Rect* rect) {
    if (display != 1 || !rect || !S().backend) return SDL_SetError("Invalid display");
    return S().backend->UsableBounds(*rect);
}

// --- input ------------------------------------------------------------------------------------------------------

SDL_Keymod SDL_GetModState() { return S().mod; }

SDL_MouseButtonFlags SDL_GetMouseState(float* x, float* y) {
    if (x) *x = S().mx;
    if (y) *y = S().my;
    return S().buttons;
}

SDL_MouseButtonFlags SDL_GetGlobalMouseState(float* x, float* y) {
    float gx = 0, gy = 0;
    if (S().backend) S().backend->GlobalMouse(gx, gy);
    if (x) *x = gx;
    if (y) *y = gy;
    return S().buttons;
}

bool SDL_CaptureMouse(bool on) {
    if (S().backend) S().backend->CaptureMouse(on);
    return true;
}

bool SDL_StartTextInput(SDL_Window* w) {
    if (!w) return false;
    S().textInput = true;
    if (w->native) w->native->SetTextInput(true, nullptr);
    return true;
}

bool SDL_StopTextInput(SDL_Window* w) {
    if (!w) return false;
    S().textInput = false;
    if (w->native) w->native->SetTextInput(false, nullptr);
    return true;
}

SDL_Cursor* SDL_CreateSystemCursor(SDL_SystemCursor id) {
    Cursor kind = Cursor::Arrow;
    switch (id) {
    case SDL_SYSTEM_CURSOR_TEXT: kind = Cursor::Text; break;
    case SDL_SYSTEM_CURSOR_POINTER: kind = Cursor::Pointer; break;
    case SDL_SYSTEM_CURSOR_EW_RESIZE: kind = Cursor::ResizeEW; break;
    case SDL_SYSTEM_CURSOR_NS_RESIZE: kind = Cursor::ResizeNS; break;
    default: break;
    }
    return new SDL_Cursor{kind};
}

bool SDL_SetCursor(SDL_Cursor* c) {
    State& s = S();
    if (c) s.cursor = c;
    for (SDL_Window* w : s.windows)
        if (w->native && s.cursor) w->native->SetCursor(s.cursor->kind);
    return true;
}

namespace {
struct KeyName {
    SDL_Keycode key;
    const char* name;
};
// The names SDL gives the keys the player binds or shows (SDL_keyboard.c's table).
constexpr KeyName kNames[] = {
    {SDLK_RETURN, "Return"}, {SDLK_ESCAPE, "Escape"}, {SDLK_BACKSPACE, "Backspace"}, {SDLK_TAB, "Tab"},
    {SDLK_SPACE, "Space"}, {SDLK_DELETE, "Delete"}, {SDLK_INSERT, "Insert"}, {SDLK_HOME, "Home"},
    {SDLK_PAGEUP, "PageUp"}, {SDLK_END, "End"}, {SDLK_PAGEDOWN, "PageDown"}, {SDLK_RIGHT, "Right"},
    {SDLK_LEFT, "Left"}, {SDLK_DOWN, "Down"}, {SDLK_UP, "Up"}, {SDLK_KP_ENTER, "Keypad Enter"},
    {SDLK_MUTE, "Mute"}, {SDLK_VOLUMEUP, "VolumeUp"}, {SDLK_VOLUMEDOWN, "VolumeDown"},
    {SDLK_MEDIA_NEXT_TRACK, "MediaNextTrack"}, {SDLK_MEDIA_PREVIOUS_TRACK, "MediaPreviousTrack"},
    {SDLK_MEDIA_STOP, "MediaStop"}, {SDLK_MEDIA_PLAY_PAUSE, "MediaPlayPause"},
};
SDL_Keycode FunctionKey(int n) {  // F1-F12 and F13-F24 sit in two runs of SDL's scancodes
    if (n >= 1 && n <= 12) return SDLK_F1 + (SDL_Keycode)(n - 1);
    if (n >= 13 && n <= 24) return 0x40000068u + (SDL_Keycode)(n - 13);
    return SDLK_UNKNOWN;
}
}  // namespace

const char* SDL_GetKeyName(SDL_Keycode key) {
    static thread_local std::string name;
    for (int n = 1; n <= 24; ++n)
        if (FunctionKey(n) == key) return (name = "F" + std::to_string(n)).c_str();
    for (const KeyName& k : kNames)
        if (k.key == key) return k.name;
    if (key >= 0x20 && key < 0x110000 && !(key & SDLK_SCANCODE_MASK)) {  // the character, upper case
        uint32_t c = key;
        if (c >= 'a' && c <= 'z') c -= 32;
        return (name = Utf8(c)).c_str();
    }
    return "";
}

SDL_Keycode SDL_GetKeyFromName(const char* text) {
    if (!text || !*text) return SDLK_UNKNOWN;
    std::string n = text;
    if ((n[0] == 'F' || n[0] == 'f') && n.size() > 1 && n.size() <= 3 && n.find_first_not_of("0123456789", 1) == std::string::npos)
        return FunctionKey(atoi(n.c_str() + 1));
    for (const KeyName& k : kNames)
        if (strcasecmp(k.name, text) == 0) return k.key;
    if (n.size() == 1) {
        unsigned char c = (unsigned char)n[0];
        return c >= 'A' && c <= 'Z' ? (SDL_Keycode)(c + 32) : (SDL_Keycode)c;
    }
    return SDLK_UNKNOWN;
}

// --- clipboard --------------------------------------------------------------------------------------------------

bool SDL_SetClipboardText(const char* text) {
    if (!S().backend) return SDL_SetError("Video subsystem has not been initialized");
    return S().backend->SetClipboard(text ? text : "");
}

namespace {
std::optional<std::string> ClipboardText() {
    if (!S().backend) return std::nullopt;
    for (const char* mime : {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "TEXT", "STRING"})
        if (auto t = S().backend->GetClipboard(mime)) return t;
    return std::nullopt;
}
}  // namespace

char* SDL_GetClipboardText() {
    std::string t = ClipboardText().value_or("");
    return strdup(t.c_str());
}

bool SDL_HasClipboardText() {
    if (!S().backend) return false;
    for (const std::string& t : S().backend->ClipboardTypes())
        if (t.rfind("text/plain", 0) == 0 || t == "UTF8_STRING" || t == "TEXT" || t == "STRING") return true;
    return false;
}

void* SDL_GetClipboardData(const char* mime, size_t* size) {
    if (size) *size = 0;
    if (!mime || !S().backend) return nullptr;
    std::optional<std::string> d = std::string_view(mime).rfind("text/plain", 0) == 0 ? ClipboardText() : S().backend->GetClipboard(mime);
    if (!d) return nullptr;
    char* p = (char*)malloc(d->size() + 1);
    if (!p) return nullptr;
    memcpy(p, d->data(), d->size());
    p[d->size()] = 0;
    if (size) *size = d->size();
    return p;
}

bool SDL_HasClipboardData(const char* mime) {
    if (!mime || !S().backend) return false;
    if (std::string_view(mime).rfind("text/plain", 0) == 0) return SDL_HasClipboardText();
    for (const std::string& t : S().backend->ClipboardTypes())
        if (t == mime) return true;
    return false;
}
