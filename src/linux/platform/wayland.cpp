// Wayland through its wire protocol on the compositor's socket, without libwayland: xdg-shell
// windows with server-side decorations where offered (xdg-decoration), wl_shm pictures, fractional
// scale through wp_fractional_scale_v1 + wp_viewporter (integer buffer scales otherwise), cursors
// by cursor-shape-v1 or from the Xcursor theme, the seat's keyboard read with our own XKB keymap
// reader (xkb_keymap.cpp) and repeated client-side, pointer and wheel, the clipboard and drops
// through wl_data_device, input methods through text-input-v3, and xdg-activation for raising.
#include "platform/platform.h"
#include "platform/wayland_wire.h"
#include "platform/x11_wire.h"
#include "platform/xkb_keymap.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <thread>
#include <unordered_map>

namespace lx::platform {
namespace {

using wl::Args;

uint64_t NowMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

// One request: the object, the opcode, then the arguments (the size is filled in by Done).
struct Msg {
    std::string b;
    std::vector<int> fds;
    Msg(uint32_t object, uint16_t opcode) {
        U(object);
        U(opcode);
    }
    Msg& U(uint32_t v) { return b.append((const char*)&v, 4), *this; }
    Msg& I(int32_t v) { return U((uint32_t)v); }
    Msg& F(double v) { return I((int32_t)std::lround(v * 256)); }
    Msg& S(std::string_view s) {
        U((uint32_t)s.size() + 1);
        b.append(s);
        b.append(4 - s.size() % 4, '\0');
        return *this;
    }
    Msg& NullS() { return U(0); }
    Msg& Fd(int fd) { return fds.push_back(fd), *this; }
    void Finish() {
        uint32_t word = (uint32_t)b.size() << 16;
        uint32_t op;
        memcpy(&op, &b[4], 4);
        word |= op & 0xffff;
        memcpy(&b[4], &word, 4);
    }
};

enum class If {
    Display, Registry, Callback, Compositor, Shm, Pool, Buffer, Surface, Region, Seat, Pointer, Keyboard, Output,
    DataManager, DataDevice, DataOffer, DataSource, WmBase, XdgSurface, Toplevel, DecoManager, Deco, Viewporter,
    Viewport, FracManager, Frac, ShapeManager, ShapeDevice, TextInputManager, TextInput, Activation, ActivationToken,
    IconManager, Icon,
    Gone,  // destroyed by us, waiting for delete_id
};

class Wayland;

struct Buffer {
    uint32_t id = 0;
    uint32_t* px = nullptr;
    size_t bytes = 0;
    int w = 0, h = 0;
    bool busy = false, argb = false;
};

class WlWindow : public NativeWindow {
public:
    WlWindow(Wayland& wl, SDL_Window* owner, bool highDensity) : wl_(wl), owner_(owner), highDensity_(highDensity) {}
    ~WlWindow() override;
    void SetTitle(const std::string& title) override;
    void SetIcon(int w, int h, const uint32_t* argb) override;  // xdg-toplevel-icon, where the compositor has it
    void SetSize(int w, int h) override;
    void SetMinimumSize(int w, int h) override;
    bool SetPosition(int, int) override { return false; }  // Wayland clients can't place windows
    bool GetPosition(int& x, int& y) override { return x = y = 0, false; }
    void SetResizable(bool on) override;
    void SetBordered(bool on) override;
    void SetAlwaysOnTop(bool) override {}  // no protocol for it
    bool SetShape(int w, int h, const uint8_t* alpha) override;
    void Maximize() override;
    void Restore() override;
    void Raise() override;
    void Hide() override;
    void Sync() override;
    uint32_t* Framebuffer(int& w, int& h, int& stride) override;
    void Present(const SDL_Rect* rects, int count) override;
    void SetCursor(Cursor c) override;
    void SetTextInput(bool on, const SDL_Rect* area) override;

    bool Create(const std::string& title, int w, int h, SDL_WindowFlags flags);
    void MakeRole();
    void DropRole();
    void OnToplevelConfigure(Args& a);
    void OnConfigure(uint32_t serial);
    void SetFractional(uint32_t scale120);
    void SetIntegerScale(int scale);
    void UpdateSize(int w, int h);
    float Scale() const { return frac120_ ? frac120_ / 120.f : (float)intScale_; }
    SDL_Window* Owner() const { return owner_; }
    uint32_t Surface() const { return surface_; }
    bool TextWanted() const { return textOn_; }
    const SDL_Rect& TextArea() const { return textArea_; }
    std::vector<uint32_t> outputs;  // the outputs the surface is on
    bool configured = false;
    Cursor cursor = Cursor::Arrow;

private:
    void ApplySizeLimits();
    Wayland& wl_;
    SDL_Window* owner_;
    bool highDensity_;
    uint32_t surface_ = 0, xdgSurface_ = 0, toplevel_ = 0, deco_ = 0, viewport_ = 0, frac_ = 0;
    // The window's icon: its pixels, and the xdg_toplevel_icon_v1 with the buffer it must keep.
    std::vector<uint32_t> iconPx_;
    int iconSize_ = 0;
    uint32_t icon_ = 0;
    Buffer* iconBuffer_ = nullptr;
    void ApplyIcon();
    void DropIcon();
    std::string title_;
    int minW_ = 0, minH_ = 0;
    bool resizable_ = true, bordered_ = true, maximized_ = false, hidden_ = false;
    int pendingW_ = 0, pendingH_ = 0;
    bool pendingMax_ = false;
    uint32_t frac120_ = 0;
    int intScale_ = 1;
    std::vector<uint32_t> canvas_;
    int cw_ = 0, ch_ = 0;
    std::vector<Buffer*> buffers_;
    std::vector<uint8_t> shape_;
    int shapeW_ = 0, shapeH_ = 0;
    bool shapeChanged_ = false, viewportDirty_ = true;
    bool textOn_ = false;
    SDL_Rect textArea_{};
};

class Wayland : public Backend {
public:
    ~Wayland() override;
    bool Open(std::string& error);
    const char* Name() const override { return "wayland"; }
    std::unique_ptr<NativeWindow> CreateWindow(SDL_Window* owner, const std::string& title, int w, int h, SDL_WindowFlags flags) override;
    int Fd() const override { return fd_; }
    void Dispatch() override;
    void Flush() override { FlushOut(); }
    int TimeoutMs() const override;
    bool UsableBounds(SDL_Rect& r) override;
    bool GlobalMouse(float&, float&) override { return false; }  // Wayland doesn't tell
    bool SetClipboard(const std::string& text) override;
    std::vector<std::string> ClipboardTypes() override;
    std::optional<std::string> GetClipboard(const std::string& mime) override;

    // Objects and requests.
    uint32_t New(If type, void* data = nullptr) {
        const uint32_t id = nextId_++;
        objs_[id] = {type, data};
        return id;
    }
    void Destroyed(uint32_t id) {
        if (auto it = objs_.find(id); it != objs_.end()) it->second = {If::Gone, nullptr};
    }
    void Send(Msg&& m) {
        m.Finish();
        out_ += m.b;
        for (int fd : m.fds) fdsOut_.push_back(fd);
        if (out_.size() > 32768) FlushOut();
    }
    bool FlushOut();
    bool Roundtrip(int timeoutMs = 2000);
    bool WaitFor(const std::function<bool()>& done, int timeoutMs);
    uint32_t compositor = 0, shm = 0, wmBase = 0, decoManager = 0, viewporter = 0, fracManager = 0, shapeManager = 0, iconManager = 0,
             activation = 0, seat = 0, pointer = 0, dataDevice = 0;
    uint32_t compositorVersion = 0, wmBaseVersion = 0;
    uint32_t inputSerial = 0, pointerSerial = 0;
    void Forget(WlWindow* w);
    void ApplyCursor(WlWindow* w);
    void ApplyTextInput();
    WlWindow* PointerFocus() const { return ptrFocus_; }
    void BufferReleased(uint32_t id);
    void Register(Buffer* b) { buffers_[b->id] = b; }
    void WatchToken(uint32_t token, WlWindow* w) { tokens_[token] = w; }
    void Unregister(uint32_t id) { buffers_.erase(id); }
    int OutputScale(uint32_t output) const {
        auto it = outputs_.find(output);
        return it != outputs_.end() ? it->second.scale : 1;
    }

private:
    bool ReadSome(int timeoutMs);
    void HandleAll();
    void Handle(uint32_t object, If type, void* data, uint16_t opcode, Args& a);
    int TakeFd() {
        if (fdsIn_.empty()) return -1;
        int fd = fdsIn_.front();
        fdsIn_.pop_front();
        return fd;
    }
    void Bind(uint32_t name, const std::string& iface, uint32_t version);
    WlWindow* WindowOf(uint32_t surface) {
        for (WlWindow* w : windows_)
            if (w->Surface() == surface) return w;
        return nullptr;
    }
    void Key(uint32_t key, bool down, bool repeat);
    void Lost();
    void DestroyOffer(uint32_t& id) {
        if (!id) return;
        Send(Msg(id, 2));  // wl_data_offer.destroy
        Destroyed(id);
        offers_.erase(id);
        id = 0;
    }
    std::optional<std::string> Receive(uint32_t offer, const std::string& mime);

    int fd_ = -1;
    std::string in_, out_;
    std::deque<int> fdsIn_;
    std::vector<int> fdsOut_;
    uint32_t nextId_ = 2, registry_ = 0;
    struct Obj {
        If type;
        void* data;
    };
    std::unordered_map<uint32_t, Obj> objs_;
    std::unordered_map<uint32_t, Buffer*> buffers_;
    std::vector<WlWindow*> windows_;
    std::map<uint32_t, uint32_t> outputNames_;  // registry name to object
    struct Output {
        int w = 0, h = 0, scale = 1;
    };
    std::map<uint32_t, Output> outputs_;
    uint32_t dataManager_ = 0, textInputManager_ = 0, keyboard_ = 0, textInput_ = 0, shapeDevice_ = 0;
    uint32_t seatVersion_ = 0, dataManagerVersion_ = 0;
    std::vector<uint32_t> waitingCallbacks_;
    // Keyboard.
    XkbKeymap keymap_;
    bool haveKeymap_ = false;
    uint32_t mods_ = 0, group_ = 0;
    const bool debug_ = getenv("CORDINAL_WAYLAND_DEBUG") != nullptr;  // prints keyboard events
    int repeatRate_ = 25, repeatDelay_ = 600;
    uint32_t repeatKey_ = 0;
    uint64_t nextRepeat_ = 0;
    WlWindow* kbdFocus_ = nullptr;
    // Pointer.
    WlWindow* ptrFocus_ = nullptr;
    float px_ = 0, py_ = 0;
    struct Axis {
        int type = 0;  // 0 continuous, 1 discrete, 2 value120 (SDL's AXIS_EVENT_*)
        float v = 0;
    } ax_[2];
    uint32_t cursorSurface_ = 0;
    std::map<int, std::pair<Buffer*, std::pair<int, int>>> cursorImages_;  // by cursor and scale
    // Clipboard and drops.
    std::map<uint32_t, std::vector<std::string>> offers_;  // live offers and their types
    uint32_t selection_ = 0, source_ = 0;
    std::string clip_;
    uint32_t dndOffer_ = 0, dndSerial_ = 0;
    WlWindow* dndWindow_ = nullptr;
    float dndX_ = 0, dndY_ = 0;
    std::string dndMime_;
    // Text input.
    WlWindow* textFocus_ = nullptr;
    std::string commit_;
    bool textEnabled_ = false;
    // Activation.
    std::map<uint32_t, WlWindow*> tokens_;
};

// --- the connection -------------------------------------------------------------------------------------------

bool Wayland::Open(std::string& error) {
    if (const char* s = getenv("WAYLAND_SOCKET"); s && *s) {
        fd_ = atoi(s);
        unsetenv("WAYLAND_SOCKET");
        fcntl(fd_, F_SETFD, FD_CLOEXEC);
    } else {
        const char* display = getenv("WAYLAND_DISPLAY");
        std::string path;
        if (display && display[0] == '/') path = display;
        else {
            const char* run = getenv("XDG_RUNTIME_DIR");
            if (!run || !*run) return error = "XDG_RUNTIME_DIR is not set", false;
            if (!display || !*display) {
                // As SDL: only when a Wayland session says so (no implicit "wayland-0" next to X11).
                return error = "WAYLAND_DISPLAY is not set", false;
            }
            path = std::string(run) + "/" + display;
        }
        sockaddr_un a{};
        a.sun_family = AF_UNIX;
        if (path.size() >= sizeof a.sun_path) return error = "the Wayland socket path is too long", false;
        memcpy(a.sun_path, path.data(), path.size());
        fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd_ < 0 || connect(fd_, (sockaddr*)&a, sizeof a) != 0) {
            error = "can't connect to the Wayland compositor at " + path;
            if (fd_ >= 0) close(fd_);
            fd_ = -1;
            return false;
        }
    }
    objs_[1] = {If::Display, nullptr};
    registry_ = New(If::Registry);
    Send(std::move(Msg(1, 1).U(registry_)));  // wl_display.get_registry
    if (!Roundtrip() || !Roundtrip()) return error = "the Wayland compositor didn't answer", false;  // globals, then their first events
    if (!compositor || !shm || !wmBase) return error = "the Wayland compositor lacks wl_compositor, wl_shm or xdg_wm_base", false;
    return true;
}

Wayland::~Wayland() {
    for (auto& [id, b] : cursorImages_)
        if (b.first) {
            munmap(b.first->px, b.first->bytes);
            delete b.first;
        }
    if (fd_ >= 0) {
        FlushOut();
        close(fd_);
    }
    for (int fd : fdsIn_) close(fd);
    for (int fd : fdsOut_) close(fd);
}

bool Wayland::FlushOut() {
    while ((!out_.empty() || !fdsOut_.empty()) && fd_ >= 0) {
        msghdr mh{};
        iovec iov{out_.data(), out_.size()};
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
        char control[CMSG_SPACE(sizeof(int) * 28)];
        const size_t nfds = std::min<size_t>(fdsOut_.size(), 28);
        if (nfds) {
            mh.msg_control = control;
            mh.msg_controllen = CMSG_SPACE(sizeof(int) * nfds);
            cmsghdr* c = CMSG_FIRSTHDR(&mh);
            c->cmsg_level = SOL_SOCKET;
            c->cmsg_type = SCM_RIGHTS;
            c->cmsg_len = CMSG_LEN(sizeof(int) * nfds);
            memcpy(CMSG_DATA(c), fdsOut_.data(), sizeof(int) * nfds);
        }
        ssize_t n = sendmsg(fd_, &mh, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n >= 0) {
            out_.erase(0, (size_t)n);
            for (size_t i = 0; i < nfds; ++i) close(fdsOut_[i]);
            fdsOut_.erase(fdsOut_.begin(), fdsOut_.begin() + (ptrdiff_t)nfds);
            if (n == 0 && !nfds) break;
            continue;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN) {
            pollfd p{fd_, POLLOUT, 0};
            poll(&p, 1, 1000);
            continue;
        }
        Lost();
        return false;
    }
    return fd_ >= 0;
}

void Wayland::Lost() {
    if (fd_ < 0) return;
    close(fd_);
    fd_ = -1;
    SDL_Event e{};
    e.type = SDL_EVENT_QUIT;
    PushEvent(e);
}

bool Wayland::ReadSome(int timeoutMs) {
    if (fd_ < 0) return false;
    pollfd p{fd_, POLLIN, 0};
    if (poll(&p, 1, timeoutMs) <= 0) return false;
    char buf[65536];
    char control[CMSG_SPACE(sizeof(int) * 28)];
    iovec iov{buf, sizeof buf};
    msghdr mh{};
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = control;
    mh.msg_controllen = sizeof control;
    ssize_t n = recvmsg(fd_, &mh, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) return true;
    if (n <= 0) {
        Lost();
        return false;
    }
    for (cmsghdr* c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            const size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            for (size_t i = 0; i < count; ++i) {
                int fd;
                memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof fd);
                fdsIn_.push_back(fd);
            }
        }
    in_.append(buf, (size_t)n);
    return true;
}

void Wayland::HandleAll() {
    size_t at = 0;
    for (;;) {
        wl::Message m;
        const size_t n = wl::NextMessage(std::string_view(in_).substr(at), m);
        if (n == 0) break;
        if (n == SIZE_MAX) {
            fprintf(stderr, "cordinal: malformed message from the Wayland compositor\n");
            Lost();
            return;
        }
        at += n;
        auto it = objs_.find(m.object);
        Args a(m.body);
        if (it == objs_.end()) continue;
        Handle(m.object, it->second.type, it->second.data, m.opcode, a);
        if (fd_ < 0) return;
    }
    in_.erase(0, at);
}

bool Wayland::WaitFor(const std::function<bool()>& done, int timeoutMs) {
    const uint64_t deadline = NowMs() + (uint64_t)timeoutMs;
    while (!done()) {
        if (!FlushOut()) return false;
        const uint64_t now = NowMs();
        if (now >= deadline) return false;
        if (ReadSome((int)(deadline - now))) HandleAll();
        if (fd_ < 0) return false;
    }
    return true;
}

bool Wayland::Roundtrip(int timeoutMs) {
    const uint32_t cb = New(If::Callback);
    waitingCallbacks_.push_back(cb);
    Send(std::move(Msg(1, 0).U(cb)));  // wl_display.sync
    return WaitFor([&] { return std::find(waitingCallbacks_.begin(), waitingCallbacks_.end(), cb) == waitingCallbacks_.end(); }, timeoutMs);
}

void Wayland::Dispatch() {
    while (ReadSome(0)) HandleAll();
    HandleAll();
    // Key repeat, client-side as Wayland asks.
    if (repeatKey_ && kbdFocus_ && repeatRate_ > 0) {
        const uint64_t now = NowMs();
        int guard = 0;
        while (repeatKey_ && now >= nextRepeat_ && guard++ < 50) {
            Key(repeatKey_, true, true);
            nextRepeat_ += (uint64_t)std::max(1, 1000 / repeatRate_);
        }
        if (now >= nextRepeat_) nextRepeat_ = now + (uint64_t)std::max(1, 1000 / repeatRate_);
    }
    FlushOut();
}

int Wayland::TimeoutMs() const {
    if (!repeatKey_ || !kbdFocus_ || repeatRate_ <= 0) return -1;
    const uint64_t now = NowMs();
    return now >= nextRepeat_ ? 0 : (int)std::min<uint64_t>(nextRepeat_ - now, 1000);
}

void Wayland::Bind(uint32_t name, const std::string& iface, uint32_t version) {
    struct Want {
        const char* name;
        uint32_t max;
        If type;
        uint32_t* slot;
    };
    uint32_t outputDummy = 0;
    const Want wants[] = {
        {"wl_compositor", 6, If::Compositor, &compositor},
        {"wl_shm", 1, If::Shm, &shm},
        {"xdg_wm_base", 6, If::WmBase, &wmBase},
        {"wl_seat", 8, If::Seat, &seat},
        {"wl_data_device_manager", 3, If::DataManager, &dataManager_},
        {"zxdg_decoration_manager_v1", 1, If::DecoManager, &decoManager},
        {"wp_viewporter", 1, If::Viewporter, &viewporter},
        {"wp_fractional_scale_manager_v1", 1, If::FracManager, &fracManager},
        {"wp_cursor_shape_manager_v1", 1, If::ShapeManager, &shapeManager},
        {"xdg_toplevel_icon_manager_v1", 1, If::IconManager, &iconManager},
        {"zwp_text_input_manager_v3", 1, If::TextInputManager, &textInputManager_},
        {"xdg_activation_v1", 1, If::Activation, &activation},
        {"wl_output", 4, If::Output, &outputDummy},
    };
    for (const Want& w : wants) {
        if (iface != w.name) continue;
        if (w.slot != &outputDummy && *w.slot) return;  // the first of each
        if (getenv("CORDINAL_WAYLAND_NO") && strstr(getenv("CORDINAL_WAYLAND_NO"), w.name)) return;  // tests: pretend it's absent
        const uint32_t v = std::min(version, w.max);
        const uint32_t id = New(w.type);
        Send(std::move(Msg(registry_, 0).U(name).S(w.name).U(v).U(id)));  // wl_registry.bind
        if (w.slot == &outputDummy) {
            outputNames_[name] = id;
            outputs_[id] = {};
        } else {
            *w.slot = id;
        }
        if (w.type == If::Compositor) compositorVersion = v;
        if (w.type == If::WmBase) wmBaseVersion = v;
        if (w.type == If::Seat) seatVersion_ = v;
        if (w.type == If::DataManager) dataManagerVersion_ = v;
        // What depends on two globals: the data device and text input need the seat.
        if ((w.type == If::Seat || w.type == If::DataManager) && seat && dataManager_ && !dataDevice) {
            dataDevice = New(If::DataDevice);
            Send(std::move(Msg(dataManager_, 1).U(dataDevice).U(seat)));
        }
        if ((w.type == If::Seat || w.type == If::TextInputManager) && seat && textInputManager_ && !textInput_) {
            textInput_ = New(If::TextInput);
            Send(std::move(Msg(textInputManager_, 1).U(textInput_).U(seat)));
        }
        return;
    }
}

// --- events -----------------------------------------------------------------------------------------------------

void Wayland::Handle(uint32_t object, If type, void* data, uint16_t op, Args& a) {
    switch (type) {
    case If::Display:
        if (op == 0) {  // error
            const uint32_t obj = a.Uint(), code = a.Uint();
            std::string_view msg = a.String();
            fprintf(stderr, "cordinal: Wayland protocol error %u on object %u: %.*s\n", code, obj, (int)msg.size(), msg.data());
            Lost();
        } else if (op == 1) {  // delete_id
            const uint32_t id = a.Uint();
            auto it = objs_.find(id);
            if (it != objs_.end() && (it->second.type == If::Gone || it->second.type == If::Callback)) objs_.erase(it);
        }
        break;
    case If::Registry:
        if (op == 0) {
            const uint32_t name = a.Uint();
            std::string iface(a.String());
            const uint32_t version = a.Uint();
            if (!a.failed()) Bind(name, iface, version);
        } else if (op == 1) {
            const uint32_t name = a.Uint();
            if (auto it = outputNames_.find(name); it != outputNames_.end()) {
                outputs_.erase(it->second);
                for (WlWindow* w : windows_) std::erase(w->outputs, it->second);
                outputNames_.erase(it);
            }
        }
        break;
    case If::Callback:
        if (op == 0) {
            std::erase(waitingCallbacks_, object);
            Destroyed(object);
        }
        break;
    case If::WmBase:
        if (op == 0) Send(std::move(Msg(wmBase, 3).U(a.Uint())));  // ping: pong
        break;
    case If::Buffer:
        if (op == 0) BufferReleased(object);
        break;
    case If::Output: {
        Output& o = outputs_[object];
        if (op == 1) {  // mode
            const uint32_t flags = a.Uint();
            const int w = a.Int(), h = a.Int();
            if (flags & 1) o.w = w, o.h = h;
        } else if (op == 3) {  // scale
            o.scale = std::max(1, a.Int());
            for (WlWindow* w : windows_)
                if (std::find(w->outputs.begin(), w->outputs.end(), object) != w->outputs.end()) w->SetIntegerScale(-1);
        }
        break;
    }
    case If::Surface: {
        auto* w = (WlWindow*)data;
        if (!w) break;
        if (op == 0 || op == 1) {  // enter, leave
            const uint32_t output = a.Uint();
            if (op == 0) w->outputs.push_back(output);
            else std::erase(w->outputs, output);
            w->SetIntegerScale(-1);
        } else if (op == 2) {  // preferred_buffer_scale
            w->SetIntegerScale(std::max(1, a.Int()));
        }
        break;
    }
    case If::XdgSurface:
        if (op == 0 && data) ((WlWindow*)data)->OnConfigure(a.Uint());
        break;
    case If::Toplevel:
        if (!data) break;
        if (op == 0) ((WlWindow*)data)->OnToplevelConfigure(a);
        else if (op == 1) OnCloseRequest(((WlWindow*)data)->Owner());
        break;
    case If::Frac:
        if (op == 0 && data) ((WlWindow*)data)->SetFractional(a.Uint());
        break;
    case If::Deco:
        break;  // the mode the compositor chose: without server-side decorations the window has none
    case If::Seat:
        if (op == 0) {
            const uint32_t caps = a.Uint();
            if ((caps & 1) && !pointer) {
                pointer = New(If::Pointer);
                Send(std::move(Msg(seat, 0).U(pointer)));
                if (shapeManager) {
                    shapeDevice_ = New(If::ShapeDevice);
                    Send(std::move(Msg(shapeManager, 1).U(shapeDevice_).U(pointer)));
                }
            }
            if ((caps & 2) && !keyboard_) {
                keyboard_ = New(If::Keyboard);
                Send(std::move(Msg(seat, 1).U(keyboard_)));
            }
            // A capability that went away (a keyboard unplugged, a virtual one closed): let its object
            // go, so a new one is asked for when it comes back.
            if (!(caps & 2) && keyboard_) {
                if (seatVersion_ >= 3) Send(Msg(keyboard_, 0));
                Destroyed(keyboard_);
                keyboard_ = 0;
                repeatKey_ = 0;
                if (kbdFocus_) OnFocus(kbdFocus_->Owner(), false);
                kbdFocus_ = nullptr;
            }
            if (!(caps & 1) && pointer) {
                if (shapeDevice_) {
                    Send(Msg(shapeDevice_, 0));
                    Destroyed(shapeDevice_);
                    shapeDevice_ = 0;
                }
                if (seatVersion_ >= 3) Send(Msg(pointer, 1));
                Destroyed(pointer);
                pointer = 0;
                if (ptrFocus_) OnMouseEnterLeave(ptrFocus_->Owner(), false);
                ptrFocus_ = nullptr;
            }
        }
        break;
    case If::Pointer:
        switch (op) {
        case 0: {  // enter
            pointerSerial = a.Uint();
            WlWindow* w = WindowOf(a.Uint());
            px_ = (float)a.Fixed(), py_ = (float)a.Fixed();
            ptrFocus_ = w;
            if (w) {
                OnMouseEnterLeave(w->Owner(), true);
                OnMouseMotion(w->Owner(), px_, py_);
                ApplyCursor(w);
            }
            break;
        }
        case 1:  // leave
            a.Uint();
            if (ptrFocus_) OnMouseEnterLeave(ptrFocus_->Owner(), false);
            ptrFocus_ = nullptr;
            break;
        case 2:  // motion
            a.Uint();
            px_ = (float)a.Fixed(), py_ = (float)a.Fixed();
            if (ptrFocus_) OnMouseMotion(ptrFocus_->Owner(), px_, py_);
            break;
        case 3: {  // button
            inputSerial = a.Uint();
            a.Uint();
            const uint32_t button = a.Uint(), state = a.Uint();
            // BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA as SDL numbers them.
            int b = 0;
            switch (button) {
            case 0x110: b = 1; break;
            case 0x111: b = 3; break;
            case 0x112: b = 2; break;
            case 0x113: b = 4; break;
            case 0x114: b = 5; break;
            default: break;
            }
            if (b && ptrFocus_) OnMouseButton(ptrFocus_->Owner(), b, state == 1, px_, py_);
            break;
        }
        case 4: {  // axis (continuous)
            a.Uint();
            const uint32_t axis = a.Uint();
            const double v = a.Fixed();
            if (axis > 1) break;
            const float sv = axis == 0 ? (float)-v : (float)v;
            if (seatVersion_ < 5) {  // no frames: one wheel event each
                if (ptrFocus_) OnMouseWheel(ptrFocus_->Owner(), axis ? sv / 10 : 0, axis ? 0 : sv / 10);
            } else if (ax_[axis].type == 0) {
                ax_[axis].v = sv;
            }
            break;
        }
        case 5: {  // frame
            // SDL's units: continuous values over 10, discrete steps as they are, value120 over 120.
            auto value = [](const Axis& ax) { return ax.type == 0 ? ax.v / 10 : ax.type == 1 ? ax.v : ax.v / 120; };
            const float y = value(ax_[0]), x = value(ax_[1]);
            ax_[0] = ax_[1] = Axis{};
            if ((x != 0 || y != 0) && ptrFocus_) OnMouseWheel(ptrFocus_->Owner(), x, y);
            break;
        }
        case 8: {  // axis_discrete
            const uint32_t axis = a.Uint();
            const int v = a.Int();
            if (axis > 1) break;
            if (ax_[axis].type != 1 && ax_[axis].type != 2) ax_[axis] = {1, axis == 0 ? (float)-v : (float)v};
            break;
        }
        case 9: {  // axis_value120
            const uint32_t axis = a.Uint();
            const int v = a.Int();
            if (axis > 1) break;
            if (ax_[axis].type != 2) ax_[axis] = {2, 0};
            ax_[axis].v += axis == 0 ? (float)-v : (float)v;
            break;
        }
        default: break;
        }
        break;
    case If::Keyboard:
        switch (op) {
        case 0: {  // keymap
            const uint32_t format = a.Uint();
            const int fd = TakeFd();
            const uint32_t size = a.Uint();
            if (fd < 0) break;
            if (format == 1 && size > 0 && size < (64u << 20)) {
                void* p = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
                if (p != MAP_FAILED) {
                    std::string_view text((const char*)p, strnlen((const char*)p, size));
                    XkbKeymap k;
                    std::string why;
                    if (k.Parse(text, &why)) keymap_ = std::move(k), haveKeymap_ = true;
                    else fprintf(stderr, "cordinal: can't read the Wayland keymap (%s)\n", why.c_str());
                    munmap(p, size);
                }
            }
            close(fd);
            break;
        }
        case 1: {  // enter
            inputSerial = a.Uint();
            WlWindow* w = WindowOf(a.Uint());
            kbdFocus_ = w;
            repeatKey_ = 0;
            if (w) OnFocus(w->Owner(), true);
            break;
        }
        case 2: {  // leave
            a.Uint();
            WlWindow* w = WindowOf(a.Uint());
            repeatKey_ = 0;
            if (w) OnFocus(w->Owner(), false);
            if (kbdFocus_ == w) kbdFocus_ = nullptr;
            break;
        }
        case 3: {  // key
            inputSerial = a.Uint();
            a.Uint();
            const uint32_t key = a.Uint(), state = a.Uint();
            if (a.failed()) break;
            if (debug_) fprintf(stderr, "wayland: key %u %u (mods %#x)\n", key, state, mods_);
            Key(key, state == 1, false);
            if (state == 1 && haveKeymap_ && keymap_.Repeats(key + 8) && repeatRate_ > 0) {
                repeatKey_ = key;
                nextRepeat_ = NowMs() + (uint64_t)std::max(0, repeatDelay_);
            } else if (state != 1 && repeatKey_ == key) {
                repeatKey_ = 0;
            }
            break;
        }
        case 4: {  // modifiers
            a.Uint();
            const uint32_t dep = a.Uint(), lat = a.Uint(), lock = a.Uint(), group = a.Uint();
            mods_ = dep | lat | lock;
            group_ = group;
            if (debug_) fprintf(stderr, "wayland: modifiers %#x %#x %#x group %u\n", dep, lat, lock, group);
            if (haveKeymap_) {
                const uint32_t m = keymap_.Normalize(mods_);
                SyncModifiers(m & keymap_.shift, m & keymap_.control, m & keymap_.alt, m & keymap_.super,
                              keymap_.Normalize(lock) & keymap_.lock, keymap_.Normalize(lock) & keymap_.numLock);
            }
            break;
        }
        case 5:  // repeat_info
            repeatRate_ = a.Int();
            repeatDelay_ = a.Int();
            break;
        default: break;
        }
        break;
    case If::DataDevice:
        switch (op) {
        case 0: {  // data_offer
            const uint32_t id = a.Uint();
            objs_[id] = {If::DataOffer, nullptr};
            offers_[id] = {};
            break;
        }
        case 1: {  // enter
            dndSerial_ = a.Uint();
            dndWindow_ = WindowOf(a.Uint());
            dndX_ = (float)a.Fixed(), dndY_ = (float)a.Fixed();
            const uint32_t offer = a.Uint();
            if (dndOffer_ && dndOffer_ != offer) DestroyOffer(dndOffer_);
            dndOffer_ = offer;
            dndMime_.clear();
            if (auto it = offers_.find(offer); it != offers_.end())
                for (const char* want : {"text/uri-list", "text/plain;charset=utf-8", "UTF8_STRING", "text/plain"})
                    if (dndMime_.empty() && std::find(it->second.begin(), it->second.end(), want) != it->second.end()) dndMime_ = want;
            if (offer) {
                Msg m(offer, 0);  // accept
                m.U(dndSerial_);
                if (dndMime_.empty()) m.NullS();
                else m.S(dndMime_);
                Send(std::move(m));
                if (dataManagerVersion_ >= 3) Send(std::move(Msg(offer, 4).U(dndMime_.empty() ? 0 : 1).U(dndMime_.empty() ? 0 : 1)));
            }
            break;
        }
        case 2:  // leave
            DestroyOffer(dndOffer_);
            dndWindow_ = nullptr;
            break;
        case 3:  // motion
            a.Uint();
            dndX_ = (float)a.Fixed(), dndY_ = (float)a.Fixed();
            break;
        case 4: {  // drop
            const uint32_t offer = dndOffer_;
            WlWindow* w = dndWindow_;
            if (offer && w && !dndMime_.empty()) {
                std::optional<std::string> data = Receive(offer, dndMime_);
                if (data) {
                    if (dndMime_ == "text/uri-list") {
                        for (const std::string& path : x11::ParseUriList(*data, false))
                            OnDrop(w->Owner(), !path.empty() && path[0] == '/', path, dndX_, dndY_);
                    } else {
                        OnDrop(w->Owner(), false, *data, dndX_, dndY_);
                    }
                }
                if (dataManagerVersion_ >= 3) Send(Msg(offer, 3));  // finish
            }
            DestroyOffer(dndOffer_);
            dndWindow_ = nullptr;
            break;
        }
        case 5: {  // selection
            const uint32_t offer = a.Uint();
            if (selection_ && selection_ != offer) DestroyOffer(selection_);
            selection_ = offer;
            break;
        }
        default: break;
        }
        break;
    case If::DataOffer:
        if (op == 0) {
            std::string_view mime = a.String();
            auto& list = offers_[object];
            if (!a.failed() && list.size() < 256) list.emplace_back(mime);
        }
        break;
    case If::DataSource:
        if (op == 1) {  // send: write the text on its own thread, so a slow reader can't stall the UI
            std::string_view mime = a.String();
            (void)mime;
            const int fd = TakeFd();
            if (fd < 0) break;
            std::thread([fd, text = clip_] {
                int flags = fcntl(fd, F_GETFL);
                fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
                size_t at = 0;
                while (at < text.size()) {
                    ssize_t n = write(fd, text.data() + at, text.size() - at);
                    if (n < 0 && errno == EINTR) continue;
                    if (n <= 0) break;
                    at += (size_t)n;
                }
                close(fd);
            }).detach();
        } else if (op == 2) {  // cancelled: another client owns the clipboard
            if (object == source_) source_ = 0, clip_.clear();
            Send(Msg(object, 1));  // destroy
            Destroyed(object);
        } else if (op == 0 || op == 3 || op == 4 || op == 5) {
            if (op == 0) a.String();
        }
        break;
    case If::TextInput:
        switch (op) {
        case 0:  // enter
            textFocus_ = WindowOf(a.Uint());
            textEnabled_ = false;
            ApplyTextInput();
            break;
        case 1:  // leave
            a.Uint();
            if (textEnabled_) {
                Send(Msg(textInput_, 2));  // disable
                Send(Msg(textInput_, 7));  // commit
            }
            textFocus_ = nullptr;
            textEnabled_ = false;
            break;
        case 3:  // commit_string
            commit_ = std::string(a.String());
            break;
        case 5:  // done
            if (!commit_.empty() && textFocus_) OnText(textFocus_->Owner(), commit_);
            commit_.clear();
            break;
        default: break;
        }
        break;
    case If::ActivationToken:
        if (op == 0) {
            std::string token(a.String());
            WlWindow* w = tokens_.count(object) ? tokens_[object] : nullptr;
            tokens_.erase(object);
            if (w && std::find(windows_.begin(), windows_.end(), w) != windows_.end())
                Send(std::move(Msg(activation, 2).S(token).U(w->Surface())));  // activate
            Send(Msg(object, 4));  // destroy
            Destroyed(object);
        }
        break;
    default: break;
    }
}

void Wayland::Key(uint32_t key, bool down, bool repeat) {
    WlWindow* w = kbdFocus_;
    if (!w) return;
    const uint32_t code = key + 8;
    uint32_t sym = 0, base = 0, shifted = 0;
    if (haveKeymap_) {
        sym = keymap_.Keysym(code, mods_, group_);
        base = keymap_.LevelKeysym(code, group_, 0);
        shifted = keymap_.LevelKeysym(code, group_, 1);
    }
    SDL_Scancode sc = ScancodeFromEvdev(key);
    if (sc == SDL_SCANCODE_UNKNOWN) sc = ScancodeFromKeysym(base ? base : sym);
    const SDL_Keycode keycode = KeycodeFor(sc, base, shifted);
    std::string text;
    if (down) {
        const uint32_t c = KeysymToUnicode(sym);
        if (c >= 0x20 && c != 0x7f) text = Utf8(c);
    }
    OnKey(w->Owner(), sc, keycode, down, repeat, text);
}

void Wayland::ApplyTextInput() {
    if (!textInput_ || !textFocus_) return;
    const bool want = textFocus_->TextWanted();
    if (want) {
        Send(Msg(textInput_, 1));  // enable
        const SDL_Rect& r = textFocus_->TextArea();
        Send(std::move(Msg(textInput_, 5).U(0).U(0)));  // content type: normal
        Send(std::move(Msg(textInput_, 6).I(r.x).I(r.y).I(std::max(r.w, 1)).I(std::max(r.h, 1))));
        Send(Msg(textInput_, 7));  // commit
        textEnabled_ = true;
    } else if (textEnabled_) {
        Send(Msg(textInput_, 2));
        Send(Msg(textInput_, 7));
        textEnabled_ = false;
    }
    FlushOut();
}

void Wayland::Forget(WlWindow* w) {
    std::erase(windows_, w);
    if (kbdFocus_ == w) kbdFocus_ = nullptr, repeatKey_ = 0;
    if (ptrFocus_ == w) ptrFocus_ = nullptr;
    if (textFocus_ == w) textFocus_ = nullptr;
    if (dndWindow_ == w) dndWindow_ = nullptr;
    for (auto& [id, tw] : tokens_)
        if (tw == w) tw = nullptr;
}

void Wayland::BufferReleased(uint32_t id) {
    if (auto it = buffers_.find(id); it != buffers_.end()) it->second->busy = false;
}

// --- cursors ----------------------------------------------------------------------------------------------------

Buffer* NewShmBuffer(Wayland& wl, int w, int h, bool argb) {
    const size_t bytes = (size_t)w * h * 4;
    int fd = memfd_create("cordinal-buffer", MFD_CLOEXEC);
    if (fd < 0) return nullptr;
    if (ftruncate(fd, (off_t)bytes) != 0) {
        close(fd);
        return nullptr;
    }
    void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        close(fd);
        return nullptr;
    }
    auto* b = new Buffer;
    b->px = (uint32_t*)p;
    b->bytes = bytes;
    b->w = w, b->h = h, b->argb = argb;
    const uint32_t pool = wl.New(If::Pool);
    wl.Send(std::move(Msg(wl.shm, 0).U(pool).Fd(fd).I((int32_t)bytes)));  // create_pool (the fd is closed once sent)
    b->id = wl.New(If::Buffer);
    wl.Send(std::move(Msg(pool, 0).U(b->id).I(0).I(w).I(h).I(w * 4).U(argb ? 0 : 1)));  // create_buffer
    wl.Send(Msg(pool, 1));  // the pool can go; the buffer keeps the memory
    wl.Destroyed(pool);
    wl.Register(b);
    return b;
}

void FreeShmBuffer(Wayland& wl, Buffer* b) {
    if (!b) return;
    wl.Send(Msg(b->id, 0));  // wl_buffer.destroy
    wl.Destroyed(b->id);
    wl.Unregister(b->id);
    munmap(b->px, b->bytes);
    delete b;
}

void Wayland::ApplyCursor(WlWindow* w) {
    if (!pointer || !w || ptrFocus_ != w) return;
    // cursor-shape-v1: default, text, pointer, ew-resize, ns-resize.
    static const uint32_t shapes[] = {1, 9, 4, 26, 27};
    if (shapeDevice_) {
        Send(std::move(Msg(shapeDevice_, 1).U(pointerSerial).U(shapes[(int)w->cursor])));
        FlushOut();
        return;
    }
    // The Xcursor theme's image at the window's (integer) scale.
    static const char* const names[] = {"default", "text", "pointer", "ew-resize", "ns-resize"};
    static const char* const fallback[] = {"left_ptr", "xterm", "hand2", "sb_h_double_arrow", "sb_v_double_arrow"};
    const int scale = std::max(1, (int)std::ceil(w->Scale() - 0.01f));
    const int key = (int)w->cursor * 16 + std::min(scale, 15);
    auto it = cursorImages_.find(key);
    if (it == cursorImages_.end()) {
        Buffer* buf = nullptr;
        std::pair<int, int> hot{0, 0};
        const char* themeEnv = getenv("XCURSOR_THEME");
        const std::string theme = themeEnv && *themeEnv ? themeEnv : "default";
        int size = getenv("XCURSOR_SIZE") ? atoi(getenv("XCURSOR_SIZE")) : 0;
        if (size <= 0) size = 24;
        std::optional<std::string> file = FindXcursorFile(theme, names[(int)w->cursor]);
        if (!file) file = FindXcursorFile(theme, fallback[(int)w->cursor]);
        if (!file && theme != "default") file = FindXcursorFile("default", names[(int)w->cursor]);
        if (file)
            if (auto img = x11::ParseXcursor(*file, size * scale)) {
                // The buffer's size must be a multiple of its scale.
                const int bw = ((int)img->width + scale - 1) / scale * scale, bh = ((int)img->height + scale - 1) / scale * scale;
                buf = NewShmBuffer(*this, bw, bh, true);
                if (buf) {
                    memset(buf->px, 0, buf->bytes);
                    for (uint32_t y = 0; y < img->height; ++y)
                        memcpy(buf->px + (size_t)y * bw, img->argb.data() + (size_t)y * img->width, img->width * 4);
                    hot = {(int)img->xhot / scale, (int)img->yhot / scale};
                }
            }
        it = cursorImages_.emplace(key, std::make_pair(buf, hot)).first;
    }
    if (!it->second.first) return;  // no theme: the compositor's cursor stays
    if (!cursorSurface_) {
        cursorSurface_ = New(If::Surface);
        Send(std::move(Msg(compositor, 0).U(cursorSurface_)));
    }
    Buffer* b = it->second.first;
    Send(std::move(Msg(cursorSurface_, 1).U(b->id).I(0).I(0)));  // attach
    if (compositorVersion >= 3) Send(std::move(Msg(cursorSurface_, 8).I(scale)));
    Send(std::move(Msg(cursorSurface_, 2).I(0).I(0).I(b->w).I(b->h)));  // damage
    Send(Msg(cursorSurface_, 6));  // commit
    Send(std::move(Msg(pointer, 0).U(pointerSerial).U(cursorSurface_).I(it->second.second.first).I(it->second.second.second)));
    FlushOut();
}

// --- clipboard ------------------------------------------------------------------------------------------------

bool Wayland::SetClipboard(const std::string& text) {
    if (!dataDevice) return false;
    if (source_) {
        Send(Msg(source_, 1));
        Destroyed(source_);
    }
    clip_ = text;
    source_ = New(If::DataSource);
    Send(std::move(Msg(dataManager_, 0).U(source_)));
    for (const char* mime : {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "STRING", "TEXT"})
        Send(std::move(Msg(source_, 0).S(mime)));
    Send(std::move(Msg(dataDevice, 1).U(source_).U(inputSerial)));  // set_selection
    FlushOut();
    return true;
}

std::vector<std::string> Wayland::ClipboardTypes() {
    if (source_) return {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "STRING", "TEXT"};
    Roundtrip(500);  // a selection event in flight
    if (auto it = offers_.find(selection_); selection_ && it != offers_.end()) return it->second;
    return {};
}

std::optional<std::string> Wayland::Receive(uint32_t offer, const std::string& mime) {
    int p[2];
    if (pipe2(p, O_CLOEXEC) != 0) return std::nullopt;
    Send(std::move(Msg(offer, 1).S(mime).Fd(p[1])));  // receive (our end of the write side goes once sent)
    FlushOut();
    std::string data;
    const uint64_t deadline = NowMs() + 3000;
    for (;;) {
        const uint64_t now = NowMs();
        if (now >= deadline) break;
        pollfd pf[2] = {{p[0], POLLIN, 0}, {fd_, POLLIN, 0}};
        if (poll(pf, fd_ >= 0 ? 2 : 1, (int)(deadline - now)) <= 0) break;
        if (pf[0].revents) {
            char buf[65536];
            ssize_t n = read(p[0], buf, sizeof buf);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                close(p[0]);
                return data;
            }
            if (data.size() + (size_t)n > (256u << 20)) break;
            data.append(buf, (size_t)n);
        }
        if (fd_ >= 0 && pf[1].revents) {  // keep the compositor's messages moving (they queue, not run, here)
            if (!ReadSome(0)) break;
        }
    }
    close(p[0]);
    return std::nullopt;
}

std::optional<std::string> Wayland::GetClipboard(const std::string& mime) {
    if (source_) {
        if (mime.rfind("text/plain", 0) == 0 || mime == "UTF8_STRING" || mime == "STRING" || mime == "TEXT") return clip_;
        return std::nullopt;
    }
    Roundtrip(500);
    auto it = offers_.find(selection_);
    if (!selection_ || it == offers_.end() || std::find(it->second.begin(), it->second.end(), mime) == it->second.end()) return std::nullopt;
    auto data = Receive(selection_, mime);
    HandleAll();  // what arrived meanwhile
    return data;
}

// --- the rest -----------------------------------------------------------------------------------------------------

bool Wayland::UsableBounds(SDL_Rect& r) {
    for (const auto& [id, o] : outputs_)
        if (o.w > 0 && o.h > 0) {
            const int s = std::max(1, o.scale);
            r = {0, 0, o.w / s, o.h / s};
            return true;
        }
    r = {0, 0, 1920, 1080};
    return true;
}

std::unique_ptr<NativeWindow> Wayland::CreateWindow(SDL_Window* owner, const std::string& title, int w, int h, SDL_WindowFlags flags) {
    auto win = std::make_unique<WlWindow>(*this, owner, (flags & SDL_WINDOW_HIGH_PIXEL_DENSITY) != 0);
    windows_.push_back(win.get());
    if (!win->Create(title, w, h, flags)) {
        SetError("the Wayland compositor didn't configure the window");
        return nullptr;
    }
    return win;
}

// --- windows ----------------------------------------------------------------------------------------------------

bool WlWindow::Create(const std::string& title, int w, int h, SDL_WindowFlags flags) {
    title_ = title;
    resizable_ = flags & SDL_WINDOW_RESIZABLE;
    bordered_ = !(flags & SDL_WINDOW_BORDERLESS);
    maximized_ = flags & SDL_WINDOW_MAXIMIZED;
    surface_ = wl_.New(If::Surface, this);
    wl_.Send(std::move(Msg(wl_.compositor, 0).U(surface_)));
    if (wl_.viewporter) {
        viewport_ = wl_.New(If::Viewport);
        wl_.Send(std::move(Msg(wl_.viewporter, 1).U(viewport_).U(surface_)));
    }
    if (wl_.fracManager && viewport_) {
        frac_ = wl_.New(If::Frac, this);
        wl_.Send(std::move(Msg(wl_.fracManager, 1).U(frac_).U(surface_)));
    }
    owner_->w = w, owner_->h = h;
    owner_->pw = w, owner_->ph = h;
    if (flags & SDL_WINDOW_HIDDEN) {
        hidden_ = true;
        return true;
    }
    MakeRole();
    // As SDL: wait for the first configure, so the window has its size before the first frame.
    return wl_.WaitFor([&] { return configured; }, 3000);
}

void WlWindow::MakeRole() {
    configured = false;
    xdgSurface_ = wl_.New(If::XdgSurface, this);
    wl_.Send(std::move(Msg(wl_.wmBase, 2).U(xdgSurface_).U(surface_)));
    toplevel_ = wl_.New(If::Toplevel, this);
    wl_.Send(std::move(Msg(xdgSurface_, 1).U(toplevel_)));
    wl_.Send(std::move(Msg(toplevel_, 2).S(title_)));
    wl_.Send(std::move(Msg(toplevel_, 3).S("cordinal")));  // app_id: the .desktop file's name
    ApplySizeLimits();
    ApplyIcon();
    if (wl_.decoManager) {
        deco_ = wl_.New(If::Deco, this);
        wl_.Send(std::move(Msg(wl_.decoManager, 1).U(deco_).U(toplevel_)));
        wl_.Send(std::move(Msg(deco_, 1).U(bordered_ ? 2 : 1)));  // server_side, or client_side (none drawn)
    }
    if (maximized_) wl_.Send(Msg(toplevel_, 9));
    wl_.Send(Msg(surface_, 6));  // the initial commit, without a buffer
    wl_.FlushOut();
}

void WlWindow::DropRole() {
    if (deco_) {
        wl_.Send(Msg(deco_, 0));
        wl_.Destroyed(deco_);
        deco_ = 0;
    }
    if (toplevel_) {
        wl_.Send(Msg(toplevel_, 0));
        wl_.Destroyed(toplevel_);
        toplevel_ = 0;
    }
    if (xdgSurface_) {
        wl_.Send(Msg(xdgSurface_, 0));
        wl_.Destroyed(xdgSurface_);
        xdgSurface_ = 0;
    }
    wl_.Send(std::move(Msg(surface_, 1).U(0).I(0).I(0)));  // attach null: unmapped
    wl_.Send(Msg(surface_, 6));
    configured = false;
}

WlWindow::~WlWindow() {
    DropRole();
    DropIcon();
    for (Buffer* b : buffers_) FreeShmBuffer(wl_, b);
    if (frac_) wl_.Send(Msg(frac_, 0)), wl_.Destroyed(frac_);
    if (viewport_) wl_.Send(Msg(viewport_, 0)), wl_.Destroyed(viewport_);
    wl_.Send(Msg(surface_, 0));
    wl_.Destroyed(surface_);
    wl_.FlushOut();
    wl_.Forget(this);
}

void WlWindow::SetIcon(int w, int h, const uint32_t* argb) {
    if (w <= 0 || w != h || w > 1024 || !argb) return;  // the protocol takes square icons
    iconPx_.assign(argb, argb + (size_t)w * h);
    iconSize_ = w;
    ApplyIcon();
}

// xdg-toplevel-icon-v1: an icon named for the installed theme icon (the .desktop file's), with the
// pixels too for a compositor that has no such icon. An icon is immutable once set, so a new one
// replaces it; the buffer lives as long as its icon.
void WlWindow::ApplyIcon() {
    if (!wl_.iconManager || !toplevel_ || !iconSize_) return;
    DropIcon();
    icon_ = wl_.New(If::Icon, this);
    wl_.Send(std::move(Msg(wl_.iconManager, 1).U(icon_)));  // create_icon
    wl_.Send(std::move(Msg(icon_, 1).S("cordinal")));       // set_name
    if ((iconBuffer_ = NewShmBuffer(wl_, iconSize_, iconSize_, true))) {
        for (size_t i = 0; i < iconPx_.size(); ++i) {  // wl_shm's ARGB is premultiplied
            const uint32_t p = iconPx_[i], a = p >> 24;
            auto mul = [a](uint32_t c) { return (c * a + 127) / 255; };
            iconBuffer_->px[i] = a << 24 | mul((p >> 16) & 0xFF) << 16 | mul((p >> 8) & 0xFF) << 8 | mul(p & 0xFF);
        }
        wl_.Send(std::move(Msg(icon_, 2).U(iconBuffer_->id).I(1)));  // add_buffer, scale 1
    }
    wl_.Send(std::move(Msg(wl_.iconManager, 2).U(toplevel_).U(icon_)));  // set_icon, from the next commit
}

void WlWindow::DropIcon() {
    if (icon_) {
        wl_.Send(Msg(icon_, 0));  // destroy
        wl_.Destroyed(icon_);
        icon_ = 0;
    }
    if (iconBuffer_) FreeShmBuffer(wl_, iconBuffer_);
    iconBuffer_ = nullptr;
}

void WlWindow::OnToplevelConfigure(Args& a) {
    pendingW_ = a.Int();
    pendingH_ = a.Int();
    std::string_view states = a.Array();
    pendingMax_ = false;
    for (size_t i = 0; i + 4 <= states.size(); i += 4) {
        uint32_t s;
        memcpy(&s, states.data() + i, 4);
        if (s == 1) pendingMax_ = true;
    }
}

void WlWindow::OnConfigure(uint32_t serial) {
    wl_.Send(std::move(Msg(xdgSurface_, 4).U(serial)));  // ack_configure
    maximized_ = pendingMax_;
    OnWindowState(owner_, maximized_ ? SDL_WINDOW_MAXIMIZED : 0, maximized_ ? 0 : SDL_WINDOW_MAXIMIZED);
    int w = pendingW_ > 0 ? pendingW_ : owner_->w, h = pendingH_ > 0 ? pendingH_ : owner_->h;
    if (!maximized_ && pendingW_ > 0) w = std::max(w, minW_), h = std::max(h, minH_);
    const bool first = !configured;
    configured = true;
    UpdateSize(w, h);
    if (first) OnExposed(owner_);
}

void WlWindow::UpdateSize(int w, int h) {
    const float scale = highDensity_ ? Scale() : 1.f;
    const int pw = frac120_ && highDensity_ ? (int)std::lround(w * frac120_ / 120.0) : (int)std::lround(w * scale);
    const int ph = frac120_ && highDensity_ ? (int)std::lround(h * frac120_ / 120.0) : (int)std::lround(h * scale);
    if (w != owner_->w || h != owner_->h || pw != owner_->pw || ph != owner_->ph) viewportDirty_ = true;
    OnWindowResized(owner_, w, h, pw, ph);
}

void WlWindow::SetFractional(uint32_t scale120) {
    if (!scale120 || scale120 == frac120_) return;
    frac120_ = scale120;
    OnDisplayScale(owner_, Scale());
    viewportDirty_ = true;
    UpdateSize(owner_->w, owner_->h);
    if (wl_.PointerFocus() == this) wl_.ApplyCursor(this);
}

void WlWindow::SetIntegerScale(int scale) {
    if (frac120_) return;  // fractional scale wins
    if (scale < 0) {  // from the outputs the surface is on (before wl_surface v6)
        if (wl_.compositorVersion >= 6) return;
        scale = 1;
        for (uint32_t o : outputs) scale = std::max(scale, wl_.OutputScale(o));
    }
    if (scale == intScale_) return;
    intScale_ = scale;
    OnDisplayScale(owner_, Scale());
    viewportDirty_ = true;
    UpdateSize(owner_->w, owner_->h);
    if (wl_.PointerFocus() == this) wl_.ApplyCursor(this);
}

void WlWindow::SetTitle(const std::string& title) {
    title_ = title;
    if (toplevel_) wl_.Send(std::move(Msg(toplevel_, 2).S(title)));
    wl_.FlushOut();
}

void WlWindow::ApplySizeLimits() {
    if (!toplevel_) return;
    if (resizable_) {
        wl_.Send(std::move(Msg(toplevel_, 8).I(minW_).I(minH_)));
        wl_.Send(std::move(Msg(toplevel_, 7).I(0).I(0)));
    } else {
        wl_.Send(std::move(Msg(toplevel_, 8).I(owner_->w).I(owner_->h)));
        wl_.Send(std::move(Msg(toplevel_, 7).I(owner_->w).I(owner_->h)));
    }
}

void WlWindow::SetSize(int w, int h) {
    // A client sets its own size whenever it isn't maximized; the next frame shows it.
    if (maximized_) return;
    UpdateSize(std::max(w, 1), std::max(h, 1));
    if (!resizable_) ApplySizeLimits();
    wl_.FlushOut();
}

void WlWindow::SetMinimumSize(int w, int h) {
    minW_ = w, minH_ = h;
    ApplySizeLimits();
    wl_.FlushOut();
}

void WlWindow::SetResizable(bool on) {
    resizable_ = on;
    ApplySizeLimits();
    wl_.FlushOut();
}

void WlWindow::SetBordered(bool on) {
    bordered_ = on;
    if (deco_) wl_.Send(std::move(Msg(deco_, 1).U(on ? 2 : 1)));
    wl_.FlushOut();
}

bool WlWindow::SetShape(int w, int h, const uint8_t* alpha) {
    if (!alpha) {
        shape_.clear();
        shapeW_ = shapeH_ = 0;
    } else {
        shape_.assign(alpha, alpha + (size_t)w * h);
        shapeW_ = w, shapeH_ = h;
    }
    shapeChanged_ = true;
    // The input region follows the shape: runs of opaque pixels, row by row, in points.
    const uint32_t region = wl_.New(If::Region);
    wl_.Send(std::move(Msg(wl_.compositor, 1).U(region)));
    if (alpha) {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w;) {
                while (x < w && !alpha[(size_t)y * w + x]) ++x;
                const int start = x;
                while (x < w && alpha[(size_t)y * w + x]) ++x;
                if (x > start) wl_.Send(std::move(Msg(region, 1).I(start).I(y).I(x - start).I(1)));
            }
        wl_.Send(std::move(Msg(surface_, 5).U(region)));  // set_input_region
    } else {
        wl_.Send(std::move(Msg(surface_, 5).U(0)));  // the whole surface again
    }
    wl_.Send(Msg(region, 0));
    wl_.Destroyed(region);
    wl_.FlushOut();
    return true;
}

void WlWindow::Maximize() {
    maximized_ = true;
    if (toplevel_) wl_.Send(Msg(toplevel_, 9));
    wl_.FlushOut();
}

void WlWindow::Restore() {
    if (hidden_) {
        hidden_ = false;
        MakeRole();
        wl_.WaitFor([&] { return configured; }, 2000);
    }
    if (toplevel_ && maximized_) wl_.Send(Msg(toplevel_, 10));
    wl_.FlushOut();
}

void WlWindow::Raise() {
    if (hidden_) Restore();
    if (wl_.activation) {
        const uint32_t token = wl_.New(If::ActivationToken);
        wl_.Send(std::move(Msg(wl_.activation, 1).U(token)));
        if (wl_.seat && wl_.inputSerial) wl_.Send(std::move(Msg(token, 0).U(wl_.inputSerial).U(wl_.seat)));
        wl_.Send(std::move(Msg(token, 1).S("cordinal")));
        wl_.Send(std::move(Msg(token, 2).U(surface_)));
        wl_.Send(Msg(token, 3));  // commit
        wl_.WatchToken(token, this);
    }
    wl_.FlushOut();
}

void WlWindow::Hide() {
    if (hidden_) return;
    hidden_ = true;
    DropRole();
    OnWindowState(owner_, SDL_WINDOW_HIDDEN, 0);
    wl_.FlushOut();
}

void WlWindow::Sync() { wl_.Roundtrip(); }

uint32_t* WlWindow::Framebuffer(int& w, int& h, int& stride) {
    w = owner_->pw, h = owner_->ph, stride = w;
    if (w != cw_ || h != ch_) {
        canvas_.assign((size_t)w * h, 0xff000000u);
        cw_ = w, ch_ = h;
    }
    return canvas_.data();
}

void WlWindow::Present(const SDL_Rect* rects, int count) {
    if (hidden_ || !configured || cw_ <= 0 || ch_ <= 0) return;
    // A free buffer of the canvas's size (the compositor may still be reading the others).
    const bool argb = !shape_.empty();
    Buffer* buf = nullptr;
    for (Buffer* b : buffers_)
        if (!b->busy && b->w == cw_ && b->h == ch_ && b->argb == argb) buf = b;
    if (!buf) {
        // Drop free buffers of another size; wait a little when three are in use.
        for (auto it = buffers_.begin(); it != buffers_.end();)
            if (!(*it)->busy && ((*it)->w != cw_ || (*it)->h != ch_ || (*it)->argb != argb)) {
                FreeShmBuffer(wl_, *it);
                it = buffers_.erase(it);
            } else {
                ++it;
            }
        if (buffers_.size() >= 3) wl_.WaitFor([&] {
            for (Buffer* b : buffers_)
                if (!b->busy) return true;
            return false;
        }, 100);
        for (Buffer* b : buffers_)
            if (!b->busy && b->w == cw_ && b->h == ch_ && b->argb == argb) buf = b;
        if (!buf) {
            buf = NewShmBuffer(wl_, cw_, ch_, argb);
            if (!buf) return;
            buffers_.push_back(buf);
        }
    }
    // The whole canvas each time: the buffer may hold any earlier frame.
    if (!argb) {
        memcpy(buf->px, canvas_.data(), (size_t)cw_ * ch_ * 4);
    } else {
        for (int y = 0; y < ch_; ++y) {
            const int sy = std::min(shapeH_ - 1, (int)((int64_t)y * shapeH_ / ch_));
            for (int x = 0; x < cw_; ++x) {
                const int sx = std::min(shapeW_ - 1, (int)((int64_t)x * shapeW_ / cw_));
                const bool on = sy >= 0 && sx >= 0 && shape_[(size_t)sy * shapeW_ + sx];
                buf->px[(size_t)y * cw_ + x] = on ? canvas_[(size_t)y * cw_ + x] | 0xff000000u : 0;
            }
        }
    }
    buf->busy = true;
    wl_.Send(std::move(Msg(surface_, 1).U(buf->id).I(0).I(0)));  // attach
    if (viewportDirty_) {
        if (viewport_ && frac120_ && highDensity_) {
            wl_.Send(std::move(Msg(viewport_, 2).I(owner_->w).I(owner_->h)));  // set_destination
            if (wl_.compositorVersion >= 3) wl_.Send(std::move(Msg(surface_, 8).I(1)));
        } else {
            if (viewport_) wl_.Send(std::move(Msg(viewport_, 2).I(-1).I(-1)));
            if (wl_.compositorVersion >= 3) wl_.Send(std::move(Msg(surface_, 8).I(highDensity_ && !frac120_ ? intScale_ : 1)));
        }
        viewportDirty_ = false;
        rects = nullptr;
    }
    if (shapeChanged_) rects = nullptr, shapeChanged_ = false;
    SDL_Rect full{0, 0, cw_, ch_};
    if (!rects) rects = &full, count = 1;
    for (int i = 0; i < count; ++i) {
        if (wl_.compositorVersion >= 4) wl_.Send(std::move(Msg(surface_, 9).I(rects[i].x).I(rects[i].y).I(rects[i].w).I(rects[i].h)));
        else wl_.Send(std::move(Msg(surface_, 2).I(0).I(0).I(INT32_MAX).I(INT32_MAX)));
    }
    wl_.Send(Msg(surface_, 6));  // commit
    wl_.FlushOut();
}

void WlWindow::SetCursor(Cursor c) {
    if (c == cursor) return;
    cursor = c;
    wl_.ApplyCursor(this);
}

void WlWindow::SetTextInput(bool on, const SDL_Rect* area) {
    textOn_ = on;
    if (area) textArea_ = *area;
    wl_.ApplyTextInput();
}

}  // namespace

std::unique_ptr<Backend> CreateWayland(std::string& error) {
    auto w = std::make_unique<Wayland>();
    if (!w->Open(error)) return nullptr;
    return w;
}

}  // namespace lx::platform
