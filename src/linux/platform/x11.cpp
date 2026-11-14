// X11 through the core protocol on the server's Unix socket, without Xlib or XCB: windows and
// their EWMH/ICCCM properties, pictures through MIT-SHM (PutImage in strips without it), core
// input with the XKB extension asked only for detectable key repeat, the keyboard mapping read
// with GetKeyboardMapping, CLIPBOARD (INCR transfers read, small replies served), XDND drops,
// glyph cursors, SHAPE for the mini player, and the scale from Xft.dpi or XSETTINGS as SDL reads it.
#include "platform/platform.h"
#include "platform/x11_wire.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <bitset>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>

namespace lx::platform {

// The cursor theme's file for a name, the way libXcursor finds it: XCURSOR_PATH (or its default
// directories), each theme's cursors/ directory, then the themes its index.theme inherits.
std::optional<std::string> FindXcursorFile(const std::string& theme, const std::string& name, int depth) {
    if (depth > 8 || theme.empty() || theme.find('/') != std::string::npos) return std::nullopt;
    std::string path;
    if (const char* p = getenv("XCURSOR_PATH"); p && *p) path = p;
    else {
        const char* home = getenv("HOME");
        const char* data = getenv("XDG_DATA_HOME");
        std::string h = home ? home : "";
        path = (data && *data ? std::string(data) : h + "/.local/share") + "/icons:" + h + "/.icons:/usr/share/icons:/usr/share/pixmaps";
    }
    std::vector<std::string> dirs;
    for (size_t at = 0; at <= path.size();) {
        size_t colon = path.find(':', at);
        std::string d = path.substr(at, colon == std::string::npos ? std::string::npos : colon - at);
        if (d.rfind("~/", 0) == 0) d = std::string(getenv("HOME") ? getenv("HOME") : "") + d.substr(1);
        if (!d.empty()) dirs.push_back(d);
        if (colon == std::string::npos) break;
        at = colon + 1;
    }
    auto readFile = [](const std::string& file, size_t limit) -> std::optional<std::string> {
        FILE* f = fopen(file.c_str(), "rb");
        if (!f) return std::nullopt;
        std::string data;
        char buf[16384];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0 && data.size() <= limit) data.append(buf, n);
        fclose(f);
        if (data.size() > limit) return std::nullopt;
        return data;
    };
    for (const std::string& d : dirs)
        if (auto data = readFile(d + "/" + theme + "/cursors/" + name, 4u << 20)) return data;
    for (const std::string& d : dirs) {
        auto index = readFile(d + "/" + theme + "/index.theme", 1u << 16);
        if (!index) continue;
        size_t at = index->find("Inherits");
        if (at == std::string::npos) continue;
        size_t eq = index->find('=', at), nl = index->find('\n', at);
        if (eq == std::string::npos || eq > nl) continue;
        std::string list = index->substr(eq + 1, nl == std::string::npos ? std::string::npos : nl - eq - 1);
        for (size_t p = 0; p < list.size();) {
            size_t end = list.find_first_of(",;: \t\r", p);
            std::string parent = list.substr(p, end == std::string::npos ? std::string::npos : end - p);
            if (!parent.empty() && parent != theme)
                if (auto data = FindXcursorFile(parent, name, depth + 1)) return data;
            if (end == std::string::npos) break;
            p = end + 1;
        }
    }
    return std::nullopt;
}

namespace {

using namespace lx::x11;

uint64_t NowMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

uint16_t R16(const std::string& b, size_t at) { return at + 2 <= b.size() ? (uint16_t)((uint8_t)b[at] | (uint8_t)b[at + 1] << 8) : 0; }
uint32_t R32(const std::string& b, size_t at) {
    return at + 4 <= b.size() ? (uint32_t)(uint8_t)b[at] | (uint32_t)(uint8_t)b[at + 1] << 8 | (uint32_t)(uint8_t)b[at + 2] << 16 |
                                    (uint32_t)(uint8_t)b[at + 3] << 24
                              : 0;
}
int16_t S16(const std::string& b, size_t at) { return (int16_t)R16(b, at); }

// One request: opcode, a data byte, the length (filled in by Done), then the body.
struct Req {
    std::string b;
    Req(uint8_t op, uint8_t data = 0) {
        b.push_back((char)op);
        b.push_back((char)data);
        b.append(2, '\0');
    }
    Req& U8(uint8_t v) { return b.push_back((char)v), *this; }
    Req& U16(uint16_t v) { return b.append((const char*)&v, 2), *this; }
    Req& U32(uint32_t v) { return b.append((const char*)&v, 4), *this; }
    Req& Pad(size_t n) { return b.append(n, '\0'), *this; }
    Req& Str(std::string_view s) {
        b.append(s);
        return Pad((4 - s.size() % 4) % 4);
    }
    std::string Done() {
        b.resize((b.size() + 3) & ~(size_t)3, '\0');
        uint16_t words = (uint16_t)(b.size() / 4);
        memcpy(&b[2], &words, 2);
        return b;
    }
};

enum : uint32_t {
    kKeyPressMask = 1, kKeyReleaseMask = 2, kButtonPressMask = 4, kButtonReleaseMask = 8, kEnterMask = 0x10,
    kLeaveMask = 0x20, kPointerMotionMask = 0x40, kExposureMask = 0x8000, kStructureMask = 0x20000,
    kFocusMask = 0x200000, kPropertyMask = 0x400000,
};

class X11;

class X11Window : public NativeWindow {
public:
    X11Window(X11& x, SDL_Window* owner, uint32_t id) : x_(x), owner_(owner), id_(id) {}
    ~X11Window() override;
    void SetTitle(const std::string& title) override;
    void SetIcon(int w, int h, const uint32_t* argb) override;
    void SetSize(int w, int h) override;
    void SetMinimumSize(int w, int h) override;
    bool SetPosition(int x, int y) override;
    bool GetPosition(int& x, int& y) override;
    void SetResizable(bool on) override;
    void SetBordered(bool on) override;
    void SetAlwaysOnTop(bool on) override;
    bool SetShape(int w, int h, const uint8_t* alpha) override;
    void Maximize() override;
    void Restore() override;
    void Raise() override;
    void Hide() override;
    void Sync() override;
    uint32_t* Framebuffer(int& w, int& h, int& stride) override;
    void Present(const SDL_Rect* rects, int count) override;
    void SetCursor(Cursor c) override;
    void SetTextInput(bool, const SDL_Rect*) override {}
    std::string PortalHandle() override {
        char buf[32];
        snprintf(buf, sizeof buf, "x11:%x", id_);
        return buf;
    }

    uint32_t Id() const { return id_; }
    SDL_Window* Owner() const { return owner_; }
    void WriteHints();
    void FreeBuffer();

    int minW = 0, minH = 0;
    bool resizable = true, mapped = false;
    int lastX = 0, lastY = 0;
    Cursor cursor = Cursor::Arrow;

private:
    X11& x_;
    SDL_Window* owner_;
    uint32_t id_;
    std::vector<uint32_t> plain_;  // without MIT-SHM
    uint32_t* shm_ = nullptr;
    int shmId_ = -1;
    uint32_t shmSeg_ = 0;
    int bw_ = 0, bh_ = 0;
};

class X11 : public Backend {
public:
    ~X11() override {
        if (fd_ >= 0) close(fd_);
    }
    bool Open(std::string& error);
    const char* Name() const override { return "x11"; }
    std::unique_ptr<NativeWindow> CreateWindow(SDL_Window* owner, const std::string& title, int w, int h, SDL_WindowFlags flags) override;
    int Fd() const override { return fd_; }
    void Dispatch() override;
    void Flush() override { FlushOut(); }
    bool UsableBounds(SDL_Rect& r) override;
    bool GlobalMouse(float& x, float& y) override;
    void CaptureMouse(bool on) override;
    bool SetClipboard(const std::string& text) override;
    std::vector<std::string> ClipboardTypes() override;
    std::optional<std::string> GetClipboard(const std::string& mime) override;

    // The connection.
    uint64_t Send(const std::string& req);
    std::optional<std::string> Reply(uint64_t seq, int timeoutMs = 5000);
    bool Round() { return Reply(Send(Req(43).Done())).has_value(); }  // GetInputFocus: a round trip
    bool FlushOut();
    uint32_t NewId() {
        uint32_t id = setup_.idBase | ((next_++ << shift_) & setup_.idMask);
        return id;
    }
    uint32_t Atom(const std::string& name);
    void Prop(uint32_t window, uint32_t prop, uint32_t type, int format, const void* data, size_t n);
    void Prop(uint32_t window, const char* prop, const char* type, int format, const void* data, size_t n) {
        Prop(window, Atom(prop), Atom(type), format, data, n);
    }
    void DeleteProp(uint32_t window, uint32_t prop) { Send(Req(19).U32(window).U32(prop).Done()); }
    struct Property {
        uint32_t type = 0;
        int format = 0;
        std::string data;
    };
    std::optional<Property> GetProp(uint32_t window, uint32_t prop, bool remove = false);
    void ClientMessage(uint32_t window, uint32_t type, std::initializer_list<uint32_t> data, uint32_t dest = 0);
    void NetState(X11Window& w, bool add, const char* a, const char* b = nullptr);
    const Screen& Scr() const { return setup_.screens[screen_]; }
    uint32_t Root() const { return Scr().root; }
    uint32_t Gc() const { return gc_; }
    uint8_t Depth() const { return depth_; }
    int ShmMajor() const { return shmMajor_; }
    int ShapeMajor() const { return shapeMajor_; }
    size_t MaxRequestBytes() const { return (size_t)setup_.maxRequest * 4; }
    uint32_t CursorFor(Cursor c);
    uint32_t ThemeCursor(const char* name);
    void Forget(X11Window* w) { std::erase(windows_, w); }
    uint32_t Time() const { return time_; }

private:
    bool ReadSome(int timeoutMs);
    bool TakeUnits();
    void Handle(const std::string& ev);
    X11Window* Find(uint32_t id) {
        for (X11Window* w : windows_)
            if (w->Id() == id) return w;
        return nullptr;
    }
    std::optional<std::string> WaitEvent(int type, uint32_t window, int timeoutMs, uint32_t atom = 0);
    void LoadKeymap();
    void UpdateScale(bool notify);
    void Key(const std::string& ev, bool down);
    uint32_t Lookup(uint8_t keycode, uint16_t state, uint32_t* base, uint32_t* shifted);
    void ServeSelection(const std::string& ev);
    void Dnd(X11Window& w, const std::string& ev);
    void DndData(X11Window& w, const std::string& ev);

    int fd_ = -1;
    Setup setup_;
    int screen_ = 0, shift_ = 0;
    uint32_t next_ = 1;
    uint8_t depth_ = 24;
    uint32_t gc_ = 0;
    uint64_t sent_ = 0;  // requests sent
    std::string in_, out_;
    std::deque<std::string> events_;
    std::map<uint64_t, std::string> replies_;  // by full sequence; an empty string is an error
    std::map<std::string, uint32_t> atoms_;
    std::vector<X11Window*> windows_;
    int shmMajor_ = -1, shapeMajor_ = -1, xkbMajor_ = -1, renderMajor_ = -1;
    uint32_t argbFormat_ = 0;
    bool detectableRepeat_ = false;
    uint32_t time_ = 0;
    // Keyboard.
    int perCode_ = 0;
    std::vector<uint32_t> keysyms_;
    uint16_t numMask_ = 0, altMask_ = 0, superMask_ = 0, level3Mask_ = 0;
    std::bitset<256> down_;
    // Cursors.
    uint32_t cursorFont_ = 0;
    std::map<int, uint32_t> cursors_;
    // Clipboard.
    std::string clip_;
    bool owner_ = false;
    uint32_t ownedAt_ = 0, selWindow_ = 0;
    // Scale.
    float scale_ = 1;
    uint32_t xsettingsOwner_ = 0;
    // XDND.
    struct Drag {
        uint32_t source = 0, type = 0, version = 0;
        float x = 0, y = 0;
        bool dropping = false;
    } drag_;
    bool grabbed_ = false;
};

// --- the connection -------------------------------------------------------------------------------------------

bool X11::Open(std::string& error) {
    const char* env = getenv("DISPLAY");
    if (!env || !*env) return error = "DISPLAY is not set", false;
    std::optional<DisplayName> name = ParseDisplay(env);
    if (!name) return error = std::string("can't connect to DISPLAY ") + env + " (only local displays)", false;
    for (int attempt = 0; attempt < 2 && fd_ < 0; ++attempt) {
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) break;
        sockaddr_un a{};
        a.sun_family = AF_UNIX;
        socklen_t len;
        if (attempt == 0) {  // the abstract name first, as Xlib does on Linux
            if (name->socket.size() + 1 >= sizeof a.sun_path) { close(fd); continue; }
            memcpy(a.sun_path + 1, name->socket.data(), name->socket.size());
            len = (socklen_t)(offsetof(sockaddr_un, sun_path) + 1 + name->socket.size());
        } else {
            if (name->socket.size() >= sizeof a.sun_path) { close(fd); break; }
            memcpy(a.sun_path, name->socket.data(), name->socket.size());
            len = (socklen_t)sizeof a;
        }
        if (connect(fd, (sockaddr*)&a, len) == 0) fd_ = fd;
        else close(fd);
    }
    if (fd_ < 0) return error = std::string("can't connect to the X server at ") + env, false;

    // MIT-MAGIC-COOKIE-1 from XAUTHORITY or ~/.Xauthority, when there is one for this display.
    std::string cookie;
    {
        std::string file;
        if (const char* xa = getenv("XAUTHORITY"); xa && *xa) file = xa;
        else if (const char* home = getenv("HOME")) file = std::string(home) + "/.Xauthority";
        char host[256] = {};
        gethostname(host, sizeof host - 1);
        if (FILE* f = file.empty() ? nullptr : fopen(file.c_str(), "rb")) {
            std::string data;
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, f)) > 0 && data.size() < (1 << 20)) data.append(buf, n);
            fclose(f);
            if (auto c = FindCookie(data, host, name->number)) cookie = *c;
        }
    }
    std::string hello;
    hello += 'l';
    hello += '\0';
    uint16_t v = 11;
    hello.append((const char*)&v, 2);
    v = 0;
    hello.append((const char*)&v, 2);
    const std::string authName = cookie.empty() ? "" : "MIT-MAGIC-COOKIE-1";
    v = (uint16_t)authName.size();
    hello.append((const char*)&v, 2);
    v = (uint16_t)cookie.size();
    hello.append((const char*)&v, 2);
    hello.append(2, '\0');
    hello += authName;
    hello.append((4 - authName.size() % 4) % 4, '\0');
    hello += cookie;
    hello.append((4 - cookie.size() % 4) % 4, '\0');
    out_ = hello;
    if (!FlushOut()) return error = "the X server closed the connection", false;
    // The setup reply: 8 bytes, then its length in words.
    const uint64_t deadline = NowMs() + 5000;
    while (in_.size() < 8 || in_.size() < 8 + (size_t)R16(in_, 6) * 4) {
        int left = (int)std::max<int64_t>(0, (int64_t)(deadline - NowMs()));
        if (!left || !ReadSome(left)) return error = "no reply from the X server", false;
    }
    const size_t total = 8 + (size_t)R16(in_, 6) * 4;
    std::optional<Setup> s = ParseSetup(std::string_view(in_).substr(0, total));
    in_.erase(0, total);
    if (!s) return error = "the X server's setup reply is malformed", false;
    if (s->screens.empty()) return error = "the X server refused the connection: " + s->failure, false;
    setup_ = std::move(*s);
    screen_ = name->screen < (int)setup_.screens.size() ? name->screen : 0;
    while (shift_ < 32 && !((setup_.idMask >> shift_) & 1)) ++shift_;
    // The root's visual must be the 8-8-8 TrueColor (or DirectColor) the framebuffer's words are.
    const Screen& sc = Scr();
    bool visualOk = false;
    for (const auto& [depth, vis] : sc.visuals)
        if (vis.id == sc.rootVisual && (vis.cls == 4 || vis.cls == 5) && vis.red == 0xff0000 && vis.green == 0xff00 && vis.blue == 0xff)
            depth_ = depth, visualOk = true;
    bool bpp32 = false;
    for (auto [depth, bpp] : setup_.formats)
        if (depth == depth_ && bpp == 32) bpp32 = true;
    if (!visualOk || !bpp32 || setup_.imageByteOrder != 0)
        return error = "the X screen's visual isn't 24-bit TrueColor in 32-bit little-endian pixels", false;

    // Extensions: MIT-SHM, SHAPE, XKEYBOARD and RENDER (for theme cursors), asked together.
    const char* names[] = {"MIT-SHM", "SHAPE", "XKEYBOARD", "RENDER"};
    uint64_t q[4];
    for (int i = 0; i < 4; ++i) q[i] = Send(Req(98).U16((uint16_t)strlen(names[i])).Pad(2).Str(names[i]).Done());
    int* majors[] = {&shmMajor_, &shapeMajor_, &xkbMajor_, &renderMajor_};
    for (int i = 0; i < 4; ++i)
        if (auto r = Reply(q[i]); r && (uint8_t)(*r)[8]) *majors[i] = (uint8_t)(*r)[9];
    if (const char* noShm = getenv("CORDINAL_X11_NO_SHM"); noShm && *noShm == '1') shmMajor_ = -1;
    if (xkbMajor_ >= 0) {
        auto use = Reply(Send(Req((uint8_t)xkbMajor_, 0).U16(1).U16(0).Done()));
        if (use && (uint8_t)(*use)[1]) {
            // PerClientFlags: DetectableAutoRepeat, so held keys send presses only.
            auto r = Reply(Send(Req((uint8_t)xkbMajor_, 21).U16(0x100).U16(0).U32(1).U32(1).U32(0).U32(0).U32(0).Done()));
            detectableRepeat_ = r && (R32(*r, 12) & 1);
            // Once a client has used XKB the server sends it core MappingNotify only for the map
            // parts it selected through XkbSelectEvents' MapNotify: key types, symbols, modifiers.
            Send(Req((uint8_t)xkbMajor_, 1).U16(0x100).U16(2).U16(0).U16(0).U16(7).U16(7).Done());
        }
    }
    if (renderMajor_ >= 0) {
        // QueryVersion (required before other requests), then the 32-bit ARGB picture format.
        auto v = Reply(Send(Req((uint8_t)renderMajor_, 0).U32(0).U32(11).Done()));
        auto f = v ? Reply(Send(Req((uint8_t)renderMajor_, 1).Done())) : std::nullopt;
        const uint32_t n = f ? R32(*f, 8) : 0;
        for (uint32_t i = 0; f && i < n && 32 + (size_t)(i + 1) * 28 <= f->size(); ++i) {
            const size_t at = 32 + (size_t)i * 28;
            if ((uint8_t)(*f)[at + 4] == 1 && (uint8_t)(*f)[at + 5] == 32 && R16(*f, at + 8) == 16 && R16(*f, at + 10) == 0xff &&
                R16(*f, at + 12) == 8 && R16(*f, at + 14) == 0xff && R16(*f, at + 16) == 0 && R16(*f, at + 18) == 0xff &&
                R16(*f, at + 20) == 24 && R16(*f, at + 22) == 0xff)
                argbFormat_ = R32(*f, at);
        }
        if (!argbFormat_) renderMajor_ = -1;
    }
    gc_ = NewId();
    Send(Req(55).U32(gc_).U32(Root()).U32(0x10000).U32(0).Done());  // CreateGC, GraphicsExposures off
    // The root's properties tell scale changes (RESOURCE_MANAGER) and the work area.
    Send(Req(2).U32(Root()).U32(0x800).U32(kPropertyMask | kStructureMask).Done());
    selWindow_ = NewId();  // an unmapped window owns selections and receives conversions
    Send(Req(1, 0).U32(selWindow_).U32(Root()).U16(0).U16(0).U16(1).U16(1).U16(0).U16(2).U32(0).U32(0x800).U32(kPropertyMask).Done());
    LoadKeymap();
    UpdateScale(false);
    return Round();
}

bool X11::FlushOut() {
    while (!out_.empty() && fd_ >= 0) {
        ssize_t n = send(fd_, out_.data(), out_.size(), MSG_NOSIGNAL);
        if (n > 0) {
            out_.erase(0, (size_t)n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd p{fd_, POLLOUT, 0};
            poll(&p, 1, 1000);
            continue;
        }
        close(fd_);
        fd_ = -1;
        return false;
    }
    return fd_ >= 0;
}

uint64_t X11::Send(const std::string& req) {
    if (req.size() > MaxRequestBytes() && setup_.maxRequest) return 0;  // callers split what could be too big
    out_ += req;
    if (out_.size() > (1 << 20)) FlushOut();
    return ++sent_;
}

bool X11::ReadSome(int timeoutMs) {
    if (fd_ < 0) return false;
    pollfd p{fd_, POLLIN, 0};
    int r = poll(&p, 1, timeoutMs);
    if (r <= 0) return false;
    char buf[65536];
    ssize_t n = recv(fd_, buf, sizeof buf, MSG_DONTWAIT);
    if (n > 0) {
        in_.append(buf, (size_t)n);
        return true;
    }
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) return true;
    close(fd_);  // the server went away: the player quits, as with SDL's "X connection broken"
    fd_ = -1;
    PushEvent([] {
        SDL_Event e{};
        e.type = SDL_EVENT_QUIT;
        return e;
    }());
    return false;
}

// Moves whole units off the input: replies and errors to `replies_` by sequence, events to events_.
bool X11::TakeUnits() {
    for (;;) {
        size_t n = NextUnit(in_);
        if (n == 0) return true;
        if (n == SIZE_MAX) {
            close(fd_);
            fd_ = -1;
            return false;
        }
        std::string unit = in_.substr(0, n);
        in_.erase(0, n);
        const uint8_t type = (uint8_t)unit[0];
        if (type == 0 || type == 1) {
            // The full sequence: the latest sent whose low 16 bits match.
            uint16_t low = R16(unit, 2);
            uint64_t seq = (sent_ & ~0xffffull) | low;
            if (seq > sent_) seq -= 0x10000;
            if (type == 0 && getenv("CORDINAL_X11_DEBUG"))
                fprintf(stderr, "x11: error %u for request %u.%u (sequence %u)\n", (uint8_t)unit[1], (uint8_t)unit[10], R16(unit, 8), low);
            replies_[seq] = type == 0 ? std::string() : std::move(unit);
            while (replies_.size() > 256) replies_.erase(replies_.begin());
        } else {
            events_.push_back(std::move(unit));
        }
    }
}

std::optional<std::string> X11::Reply(uint64_t seq, int timeoutMs) {
    if (!seq) return std::nullopt;
    if (!FlushOut()) return std::nullopt;
    const uint64_t deadline = NowMs() + (uint64_t)timeoutMs;
    for (;;) {
        if (!TakeUnits()) return std::nullopt;
        if (auto it = replies_.find(seq); it != replies_.end()) {
            std::string r = std::move(it->second);
            replies_.erase(it);
            if (r.empty()) return std::nullopt;
            return r;
        }
        // A later reply or error means this request had neither (it can't have; but don't hang).
        if (!replies_.empty() && replies_.rbegin()->first > seq) return std::nullopt;
        uint64_t now = NowMs();
        if (now >= deadline || !ReadSome((int)(deadline - now))) {
            if (fd_ < 0 || NowMs() >= deadline) return std::nullopt;
        }
    }
}

uint32_t X11::Atom(const std::string& name) {
    if (auto it = atoms_.find(name); it != atoms_.end()) return it->second;
    auto r = Reply(Send(Req(16, 0).U16((uint16_t)name.size()).Pad(2).Str(name).Done()));
    uint32_t a = r ? R32(*r, 8) : 0;
    if (a) atoms_[name] = a;
    return a;
}

void X11::Prop(uint32_t window, uint32_t prop, uint32_t type, int format, const void* data, size_t n) {
    const size_t bytes = n * (size_t)format / 8;
    if (24 + bytes + 4 > MaxRequestBytes()) return;
    Req r(18, 0);
    r.U32(window).U32(prop).U32(type).U8((uint8_t)format).Pad(3).U32((uint32_t)n);
    r.b.append((const char*)data, bytes);
    Send(r.Done());
}

std::optional<X11::Property> X11::GetProp(uint32_t window, uint32_t prop, bool remove) {
    auto r = Reply(Send(Req(20, remove ? 1 : 0).U32(window).U32(prop).U32(0).U32(0).U32((uint32_t)((kMaxReply - 64) / 4)).Done()));
    if (!r) return std::nullopt;
    Property p;
    p.format = (uint8_t)(*r)[1];
    p.type = R32(*r, 8);
    if (!p.type) return std::nullopt;  // no such property
    const size_t units = R32(*r, 16), bytes = p.format == 8 ? units : p.format == 16 ? units * 2 : p.format == 32 ? units * 4 : 0;
    if (32 + bytes > r->size()) return std::nullopt;
    p.data = r->substr(32, bytes);
    return p;
}

void X11::ClientMessage(uint32_t window, uint32_t type, std::initializer_list<uint32_t> data, uint32_t dest) {
    std::string ev(32, '\0');
    ev[0] = 33;
    ev[1] = 32;
    memcpy(&ev[4], &window, 4);
    memcpy(&ev[8], &type, 4);
    size_t i = 0;
    for (uint32_t d : data)
        if (i < 5) memcpy(&ev[12 + 4 * i++], &d, 4);
    Req r(25, 0);
    r.U32(dest ? dest : Root()).U32(dest ? 0 : 0x180000);  // to the root: SubstructureNotify | SubstructureRedirect
    r.b += ev;
    Send(r.Done());
}

void X11::NetState(X11Window& w, bool add, const char* a, const char* b) {
    const uint32_t state = Atom("_NET_WM_STATE"), aa = Atom(a), bb = b ? Atom(b) : 0;
    if (!w.mapped) {  // before mapping, the property itself is the request
        std::vector<uint32_t> list;
        if (auto p = GetProp(w.Id(), state); p && p->format == 32)
            for (size_t i = 0; i + 4 <= p->data.size(); i += 4) list.push_back(R32(p->data, i));
        std::erase_if(list, [&](uint32_t x) { return x == aa || x == bb; });
        if (add) {
            list.push_back(aa);
            if (bb) list.push_back(bb);
        }
        Prop(w.Id(), state, 4 /* ATOM */, 32, list.data(), list.size());
        return;
    }
    ClientMessage(w.Id(), state, {add ? 1u : 0u, aa, bb, 1});
}

// A cursor from the theme through RENDER, as SDL loads them through libXcursor; 0 without one.
uint32_t X11::ThemeCursor(const char* name) {
    if (renderMajor_ < 0) return 0;
    std::string resources;
    if (auto p = GetProp(Root(), Atom("RESOURCE_MANAGER")); p && p->format == 8) resources = p->data;
    std::string theme = getenv("XCURSOR_THEME") ? getenv("XCURSOR_THEME") : "";
    if (theme.empty()) theme = ResourceString(resources, "Xcursor.theme");
    int size = getenv("XCURSOR_SIZE") ? atoi(getenv("XCURSOR_SIZE")) : 0;
    if (size <= 0) size = atoi(ResourceString(resources, "Xcursor.size").c_str());
    if (size <= 0) size = (int)ResourceDpi(resources) * 16 / 72;
    if (size <= 0) size = std::min(Scr().width, Scr().height) / 48;
    std::optional<std::string> file;
    if (!theme.empty()) file = FindXcursorFile(theme, name);
    if (!file) file = FindXcursorFile("default", name);
    if (!file) return 0;
    std::optional<CursorImage> img = ParseXcursor(*file, size);
    if (!img || (size_t)img->width * img->height * 4 + 64 > MaxRequestBytes()) return 0;
    const uint32_t pixmap = NewId(), gc = NewId(), picture = NewId(), cursor = NewId();
    Send(Req(53, 32).U32(pixmap).U32(Root()).U16((uint16_t)img->width).U16((uint16_t)img->height).Done());
    Send(Req(55).U32(gc).U32(pixmap).U32(0).Done());
    Req put(72, 2);
    put.U32(pixmap).U32(gc).U16((uint16_t)img->width).U16((uint16_t)img->height).U16(0).U16(0).U8(0).U8(32).Pad(2);
    put.b.append((const char*)img->argb.data(), img->argb.size() * 4);
    Send(put.Done());
    Send(Req((uint8_t)renderMajor_, 4).U32(picture).U32(pixmap).U32(argbFormat_).U32(0).Done());
    Send(Req((uint8_t)renderMajor_, 27).U32(cursor).U32(picture).U16((uint16_t)img->xhot).U16((uint16_t)img->yhot).Done());
    Send(Req((uint8_t)renderMajor_, 7).U32(picture).Done());
    Send(Req(60).U32(gc).Done());
    Send(Req(54).U32(pixmap).Done());
    return Round() ? cursor : 0;
}

uint32_t X11::CursorFor(Cursor c) {
    // SDL's names for its system cursors (CSS names) in the theme; else the X cursor font's
    // left_ptr, xterm, hand2, sb_h_double_arrow, sb_v_double_arrow, as SDL falls back to.
    static const char* const names[] = {"default", "text", "pointer", "ew-resize", "ns-resize"};
    static const int glyph[] = {68, 152, 60, 108, 116};
    const int g = glyph[(int)c];
    if (auto it = cursors_.find(g); it != cursors_.end()) return it->second;
    if (uint32_t themed = ThemeCursor(names[(int)c])) return cursors_[g] = themed;
    if (!cursorFont_) {
        cursorFont_ = NewId();
        Send(Req(45).U32(cursorFont_).U16(6).Pad(2).Str("cursor").Done());
    }
    uint32_t id = NewId();
    Send(Req(94).U32(id).U32(cursorFont_).U32(cursorFont_).U16((uint16_t)g).U16((uint16_t)(g + 1)).U16(0).U16(0).U16(0)
             .U16(0xffff).U16(0xffff).U16(0xffff).Done());
    cursors_[g] = id;
    return id;
}

// --- keyboard -------------------------------------------------------------------------------------------------

void X11::LoadKeymap() {
    const int first = setup_.minKeycode, count = setup_.maxKeycode - setup_.minKeycode + 1;
    auto km = Reply(Send(Req(101).U8((uint8_t)first).U8((uint8_t)count).Pad(2).Done()));
    auto mm = Reply(Send(Req(119).Done()));
    keysyms_.clear();
    perCode_ = 0;
    if (km) {
        perCode_ = (uint8_t)(*km)[1];
        for (size_t i = 32; i + 4 <= km->size() && keysyms_.size() < (size_t)perCode_ * count; i += 4) keysyms_.push_back(R32(*km, i));
        if (keysyms_.size() < (size_t)perCode_ * count) keysyms_.resize((size_t)perCode_ * count, 0);
    }
    numMask_ = altMask_ = superMask_ = level3Mask_ = 0;
    if (mm && perCode_) {
        const int per = (uint8_t)(*mm)[1];
        for (int mod = 3; mod < 8; ++mod)  // Mod1-Mod5
            for (int k = 0; k < per; ++k) {
                size_t at = 32 + (size_t)mod * per + k;
                if (at >= mm->size()) break;
                const int code = (uint8_t)(*mm)[at];
                if (code < first || code >= first + count) continue;
                for (int c = 0; c < perCode_; ++c) {
                    uint32_t s = keysyms_[(size_t)(code - first) * perCode_ + c];
                    if (s == 0xff7f) numMask_ |= (uint16_t)(1 << mod);
                    if (s == 0xffe9 || s == 0xffea || s == 0xffe7 || s == 0xffe8) altMask_ |= (uint16_t)(1 << mod);
                    if (s == 0xffeb || s == 0xffec) superMask_ |= (uint16_t)(1 << mod);
                    if (s == 0xfe03 || s == 0xff7e) level3Mask_ |= (uint16_t)(1 << mod);
                }
            }
    }
}

uint32_t Lower(uint32_t s) {
    if ((s >= 'A' && s <= 'Z') || (s >= 0xc0 && s <= 0xde && s != 0xd7)) return s + 0x20;
    return s;
}
uint32_t Upper(uint32_t s) {
    if ((s >= 'a' && s <= 'z') || (s >= 0xe0 && s <= 0xfe && s != 0xf7)) return s - 0x20;
    return s;
}

// The keysym a key types under `state`, the core protocol's rules with XKB's group and level 3.
uint32_t X11::Lookup(uint8_t code, uint16_t state, uint32_t* base, uint32_t* shifted) {
    *base = *shifted = 0;
    if (!perCode_ || code < setup_.minKeycode || code > setup_.maxKeycode) return 0;
    const uint32_t* s = &keysyms_[(size_t)(code - setup_.minKeycode) * perCode_];
    auto at = [&](int i) { return i < perCode_ ? s[i] : 0u; };
    int col = ((state >> 13) & 3) && (at(2) || at(3)) ? 2 : 0;
    if ((state & level3Mask_) && (at(4) || at(5))) col = 4;
    uint32_t a = at(col), b = at(col + 1);
    if (!a && !b) a = at(0), b = at(1);
    if (!b) b = Upper(a), a = Lower(a);
    *base = at(0) ? at(0) : at(1);
    *shifted = at(1) ? at(1) : Upper(at(0));
    const bool shift = state & 1, caps = state & 2;
    if ((state & numMask_) && IsKeypadKeysym(b)) return shift ? a : b;
    const bool letter = Lower(b) == a && Upper(a) == b && a != b;
    if (letter || (Upper(a) != a)) return (shift ^ caps) ? b : a;
    return shift ? b : a;
}

void X11::Key(const std::string& ev, bool down) {
    const uint8_t code = (uint8_t)ev[1];
    const uint16_t state = R16(ev, 28);
    X11Window* w = Find(R32(ev, 12));
    if (!w) return;
    SyncModifiers(state & 1, state & 4, state & altMask_, state & superMask_, state & 2, state & numMask_);
    bool repeat = false;
    if (down) {
        repeat = down_[code];
        down_[code] = true;
    } else {
        // Without detectable repeat, a held key is Release then Press with the same time.
        if (!detectableRepeat_) {
            ReadSome(0);
            TakeUnits();
            if (!events_.empty()) {
                const std::string& next = events_.front();
                if ((next[0] & 0x7f) == 2 && (uint8_t)next[1] == code && R32(next, 4) == R32(ev, 4)) return;  // repeat follows
            }
        }
        down_[code] = false;
    }
    uint32_t base, shifted;
    const uint32_t sym = Lookup(code, state, &base, &shifted);
    if (getenv("CORDINAL_X11_DEBUG")) fprintf(stderr, "x11: key %u state %x down %d sym %x\n", code, state, down, sym);
    SDL_Scancode sc = ScancodeFromEvdev(code >= 8 ? code - 8u : 0u);
    if (sc == SDL_SCANCODE_UNKNOWN) sc = ScancodeFromKeysym(base ? base : sym);
    const SDL_Keycode key = KeycodeFor(sc, base, shifted);
    std::string text;
    if (down) {
        uint32_t c = KeysymToUnicode(sym);
        if (c >= 0x20 && c != 0x7f) text = Utf8(c);
    }
    OnKey(w->Owner(), sc, key, down, repeat, text);
}

// --- scale ----------------------------------------------------------------------------------------------------

void X11::UpdateScale(bool notify) {
    double scale = 0;
    if (const char* v = getenv("SDL_VIDEO_X11_SCALING_FACTOR")) {
        double f = atof(v);
        if (f >= 1 && f <= 10) scale = f;
    }
    if (scale <= 0)
        if (auto p = GetProp(Root(), Atom("RESOURCE_MANAGER")); p && p->format == 8) {
            int dpi = (int)ResourceDpi(p->data);  // as SDL: whole dots per inch
            if (dpi > 0) scale = dpi / 96.0;
        }
    if (scale <= 0) {
        // XSETTINGS: the settings manager's window owns _XSETTINGS_S<screen>.
        const uint32_t sel = Atom("_XSETTINGS_S" + std::to_string(screen_));
        auto r = Reply(Send(Req(23).U32(sel).Done()));
        uint32_t owner = r ? R32(*r, 8) : 0;
        if (owner && owner != xsettingsOwner_) {
            xsettingsOwner_ = owner;
            Send(Req(2).U32(owner).U32(0x800).U32(kPropertyMask | kStructureMask).Done());
        }
        if (owner)
            if (auto p = GetProp(owner, Atom("_XSETTINGS_SETTINGS")); p && p->format == 8) {
                int gdk = -1, xft = -1;
                for (const auto& [name, v] : ParseXSettings(p->data)) {
                    if (name == "Gdk/WindowScalingFactor") gdk = v;
                    if (name == "Xft/DPI") xft = v;
                }
                if (gdk > 0) scale = gdk;
                else if (xft > 0) scale = xft / 1024.0 / 96.0;
            }
    }
    if (scale <= 0)
        if (const char* g = getenv("GDK_SCALE")) scale = atoi(g);
    if (scale <= 0) scale = 1;
    scale_ = (float)scale;
    for (X11Window* w : windows_) {
        if (notify) OnDisplayScale(w->Owner(), scale_);
        else w->Owner()->displayScale = scale_;
    }
}

// --- windows ----------------------------------------------------------------------------------------------------

std::unique_ptr<NativeWindow> X11::CreateWindow(SDL_Window* owner, const std::string& title, int w, int h, SDL_WindowFlags flags) {
    const uint32_t id = NewId();
    const uint32_t mask = kKeyPressMask | kKeyReleaseMask | kButtonPressMask | kButtonReleaseMask | kEnterMask | kLeaveMask |
                          kPointerMotionMask | kExposureMask | kStructureMask | kFocusMask | kPropertyMask;
    // CreateWindow: black background, NorthWest bit gravity, the events above.
    // Centred on the screen, where SDL puts a window without a position (the window manager may move it).
    const int16_t x = (int16_t)((Scr().width - w) / 2), y = (int16_t)((Scr().height - h) / 2);
    Send(Req(1, 0).U32(id).U32(Root()).U16((uint16_t)x).U16((uint16_t)y).U16((uint16_t)w).U16((uint16_t)h).U16(0).U16(1).U32(0)
             .U32(0x2 | 0x10 | 0x800).U32(Scr().black).U32(1).U32(mask).Done());
    auto win = std::make_unique<X11Window>(*this, owner, id);
    windows_.push_back(win.get());
    win->resizable = flags & SDL_WINDOW_RESIZABLE;
    win->SetTitle(title);
    static const char wmClass[] = "cordinal\0cordinal";
    Prop(id, "WM_CLASS", "STRING", 8, wmClass, sizeof wmClass);
    uint32_t protocols[] = {Atom("WM_DELETE_WINDOW"), Atom("_NET_WM_PING")};
    Prop(id, "WM_PROTOCOLS", "ATOM", 32, protocols, 2);
    uint32_t pid = (uint32_t)getpid();
    Prop(id, "_NET_WM_PID", "CARDINAL", 32, &pid, 1);
    char host[256] = {};
    gethostname(host, sizeof host - 1);
    Prop(id, "WM_CLIENT_MACHINE", "STRING", 8, host, strlen(host));
    uint32_t type = Atom("_NET_WM_WINDOW_TYPE_NORMAL");
    Prop(id, "_NET_WM_WINDOW_TYPE", "ATOM", 32, &type, 1);
    uint32_t wmHints[9] = {1 | 2, 1, 1};  // InputHint | StateHint: input, NormalState
    Prop(id, "WM_HINTS", "WM_HINTS", 32, wmHints, 9);
    uint32_t xdnd = 5;
    Prop(id, "XdndAware", "ATOM", 32, &xdnd, 1);
    win->WriteHints();
    if (flags & SDL_WINDOW_BORDERLESS) win->SetBordered(false);
    if (flags & SDL_WINDOW_MAXIMIZED) NetState(*win, true, "_NET_WM_STATE_MAXIMIZED_VERT", "_NET_WM_STATE_MAXIMIZED_HORZ");
    if (flags & SDL_WINDOW_ALWAYS_ON_TOP) NetState(*win, true, "_NET_WM_STATE_ABOVE");
    owner->displayScale = scale_;
    win->SetCursor(Cursor::Arrow);
    if (!(flags & SDL_WINDOW_HIDDEN)) {
        Send(Req(8).U32(id).Done());  // MapWindow
        win->mapped = true;
        // As SDL: wait (briefly) for the window manager to map it, so the first frame isn't lost.
        WaitEvent(19, id, 1000);
    }
    Round();
    return win;
}

X11Window::~X11Window() {
    FreeBuffer();
    x_.Send(Req(4).U32(id_).Done());  // DestroyWindow
    x_.FlushOut();
    x_.Forget(this);
}

void X11Window::SetTitle(const std::string& title) {
    x_.Prop(id_, "_NET_WM_NAME", "UTF8_STRING", 8, title.data(), title.size());
    std::string latin;  // WM_NAME is Latin-1
    for (size_t i = 0; i < title.size(); ++i) {
        unsigned char c = (unsigned char)title[i];
        if (c < 0x80) latin += (char)c;
        else if ((c & 0xe0) == 0xc0 && i + 1 < title.size()) {
            uint32_t cp = (c & 0x1f) << 6 | ((unsigned char)title[++i] & 0x3f);
            latin += cp < 0x100 ? (char)cp : '?';
        } else {
            latin += '?';
            while (i + 1 < title.size() && ((unsigned char)title[i + 1] & 0xc0) == 0x80) ++i;
        }
    }
    x_.Prop(id_, "WM_NAME", "STRING", 8, latin.data(), latin.size());
}

void X11Window::SetIcon(int w, int h, const uint32_t* argb) {
    std::vector<uint32_t> data{(uint32_t)w, (uint32_t)h};
    data.insert(data.end(), argb, argb + (size_t)w * h);
    x_.Prop(id_, "_NET_WM_ICON", "CARDINAL", 32, data.data(), data.size());
}

void X11Window::WriteHints() {
    // WM_NORMAL_HINTS: minimum size; a fixed-size window has its maximum at the same size.
    uint32_t hints[18] = {};
    hints[0] = 16 | 512;  // PMinSize | PWinGravity
    hints[5] = (uint32_t)std::max(minW, 1), hints[6] = (uint32_t)std::max(minH, 1);
    if (!resizable) {
        hints[0] |= 32;  // PMaxSize
        hints[5] = hints[7] = (uint32_t)owner_->w;
        hints[6] = hints[8] = (uint32_t)owner_->h;
    }
    hints[17] = 1;  // NorthWestGravity
    x_.Prop(id_, "WM_NORMAL_HINTS", "WM_SIZE_HINTS", 32, hints, 18);
}

void X11Window::SetSize(int w, int h) {
    x_.Send(Req(12).U32(id_).U16(4 | 8).Pad(2).U32((uint32_t)w).U32((uint32_t)h).Done());
    if (!resizable) {
        owner_->w = w, owner_->h = h;
        WriteHints();
    }
    x_.FlushOut();
}

void X11Window::SetMinimumSize(int w, int h) {
    minW = w, minH = h;
    WriteHints();
}

bool X11Window::SetPosition(int x, int y) {
    x_.Send(Req(12).U32(id_).U16(1 | 2).Pad(2).U32((uint32_t)x).U32((uint32_t)y).Done());
    x_.FlushOut();
    lastX = x, lastY = y;
    return true;
}

bool X11Window::GetPosition(int& x, int& y) {
    // TranslateCoordinates to the root: where the client area is, frames and reparenting aside.
    auto r = x_.Reply(x_.Send(Req(40).U32(id_).U32(x_.Root()).U16(0).U16(0).Done()));
    if (!r) {
        x = lastX, y = lastY;
        return false;
    }
    x = S16(*r, 12), y = S16(*r, 14);
    return true;
}

void X11Window::SetResizable(bool on) {
    resizable = on;
    WriteHints();
}

void X11Window::SetBordered(bool on) {
    uint32_t motif[5] = {2, 0, on ? 1u : 0u, 0, 0};  // MWM_HINTS_DECORATIONS
    x_.Prop(id_, "_MOTIF_WM_HINTS", "_MOTIF_WM_HINTS", 32, motif, 5);
    x_.FlushOut();
}

void X11Window::SetAlwaysOnTop(bool on) {
    x_.NetState(*this, on, "_NET_WM_STATE_ABOVE");
    x_.FlushOut();
}

bool X11Window::SetShape(int w, int h, const uint8_t* alpha) {
    if (x_.ShapeMajor() < 0) return false;
    if (!alpha) {  // ShapeMask with no pixmap: the whole window again
        x_.Send(Req((uint8_t)x_.ShapeMajor(), 2).U8(0).U8(0).Pad(2).U32(id_).U16(0).U16(0).U32(0).Done());
        x_.FlushOut();
        return true;
    }
    // ShapeRectangles: the runs of pixels whose alpha isn't 0, row by row.
    std::vector<uint16_t> rects;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w;) {
            while (x < w && !alpha[(size_t)y * w + x]) ++x;
            int start = x;
            while (x < w && alpha[(size_t)y * w + x]) ++x;
            if (x > start) rects.insert(rects.end(), {(uint16_t)start, (uint16_t)y, (uint16_t)(x - start), 1});
        }
    Req r((uint8_t)x_.ShapeMajor(), 1);
    r.U8(0).U8(0).U8(0).Pad(1).U32(id_).U16(0).U16(0);
    r.b.append((const char*)rects.data(), rects.size() * 2);
    if (r.b.size() > x_.MaxRequestBytes()) return false;
    x_.Send(r.Done());
    x_.FlushOut();
    return true;
}

void X11Window::Maximize() {
    x_.NetState(*this, true, "_NET_WM_STATE_MAXIMIZED_VERT", "_NET_WM_STATE_MAXIMIZED_HORZ");
    x_.FlushOut();
}

void X11Window::Restore() {
    if (!mapped) {
        x_.Send(Req(8).U32(id_).Done());
        mapped = true;
    }
    x_.NetState(*this, false, "_NET_WM_STATE_MAXIMIZED_VERT", "_NET_WM_STATE_MAXIMIZED_HORZ");
    x_.NetState(*this, false, "_NET_WM_STATE_HIDDEN");
    x_.FlushOut();
}

void X11Window::Raise() {
    if (!mapped) {
        x_.Send(Req(8).U32(id_).Done());
        mapped = true;
    }
    x_.ClientMessage(id_, x_.Atom("_NET_ACTIVE_WINDOW"), {1, x_.Time(), 0});
    x_.Send(Req(12).U32(id_).U16(64).Pad(2).U32(0).Done());  // stack Above
    x_.FlushOut();
}

void X11Window::Hide() {
    x_.Send(Req(10).U32(id_).Done());  // UnmapWindow, and the synthetic one ICCCM asks of a withdrawal
    std::string ev(32, '\0');
    ev[0] = 18;
    uint32_t root = x_.Root();
    memcpy(&ev[4], &root, 4);
    memcpy(&ev[8], &id_, 4);
    Req r(25, 0);
    r.U32(root).U32(0x180000);
    r.b += ev;
    x_.Send(r.Done());
    mapped = false;
    x_.FlushOut();
}

void X11Window::Sync() { x_.Round(); }

void X11Window::FreeBuffer() {
    if (shm_) {
        x_.Send(Req((uint8_t)x_.ShmMajor(), 2).U32(shmSeg_).Done());
        x_.Round();  // the server lets go before the memory does
        shmdt(shm_);
        shm_ = nullptr;
    }
    plain_.clear();
    bw_ = bh_ = 0;
}

uint32_t* X11Window::Framebuffer(int& w, int& h, int& stride) {
    w = owner_->pw, h = owner_->ph, stride = w;
    if (w == bw_ && h == bh_ && (shm_ || !plain_.empty())) return shm_ ? shm_ : plain_.data();
    FreeBuffer();
    bw_ = w, bh_ = h;
    const size_t bytes = (size_t)w * h * 4;
    if (x_.ShmMajor() >= 0) {
        shmId_ = shmget(IPC_PRIVATE, bytes, IPC_CREAT | 0600);
        if (shmId_ >= 0) {
            void* p = shmat(shmId_, nullptr, 0);
            if (p != (void*)-1) {
                shmSeg_ = x_.NewId();
                x_.Send(Req((uint8_t)x_.ShmMajor(), 1).U32(shmSeg_).U32((uint32_t)shmId_).U8(0).Pad(3).Done());
                if (x_.Round()) {
                    // The attach is checked by the round trip: an error reply (another machine's
                    // server, a sandbox) would have made it fail; the segment then goes when both detach.
                    shm_ = (uint32_t*)p;
                } else {
                    shmdt(p);
                }
            }
            shmctl(shmId_, IPC_RMID, nullptr);
        }
    }
    if (!shm_) plain_.assign((size_t)w * h, 0xff000000u);
    return shm_ ? shm_ : plain_.data();
}

void X11Window::Present(const SDL_Rect* rects, int count) {
    if (!bw_ || !bh_) return;
    SDL_Rect full{0, 0, bw_, bh_};
    if (!rects) rects = &full, count = 1;
    for (int i = 0; i < count; ++i) {
        SDL_Rect r;
        if (!SDL_GetRectIntersection(&rects[i], &full, &r)) continue;
        if (shm_) {
            x_.Send(Req((uint8_t)x_.ShmMajor(), 3).U32(id_).U32(x_.Gc()).U16((uint16_t)bw_).U16((uint16_t)bh_).U16((uint16_t)r.x)
                        .U16((uint16_t)r.y).U16((uint16_t)r.w).U16((uint16_t)r.h).U16((uint16_t)r.x).U16((uint16_t)r.y)
                        .U8(x_.Depth()).U8(2).U8(0).Pad(1).U32(shmSeg_).U32(0).Done());
            continue;
        }
        // PutImage in strips that fit the request size.
        const size_t rowBytes = (size_t)r.w * 4;
        const int rowsPer = std::max<int>(1, (int)((x_.MaxRequestBytes() - 64) / rowBytes));
        for (int y = r.y; y < r.y + r.h; y += rowsPer) {
            const int n = std::min(rowsPer, r.y + r.h - y);
            Req q(72, 2);
            q.U32(id_).U32(x_.Gc()).U16((uint16_t)r.w).U16((uint16_t)n).U16((uint16_t)r.x).U16((uint16_t)y).U8(0).U8(x_.Depth()).Pad(2);
            for (int k = 0; k < n; ++k) q.b.append((const char*)&plain_[(size_t)(y + k) * bw_ + r.x], rowBytes);
            x_.Send(q.Done());
        }
    }
    // With shared memory the next frame draws into the same pixels: wait until the server has them.
    if (shm_) x_.Round();
    else x_.FlushOut();
}

void X11Window::SetCursor(Cursor c) {
    cursor = c;
    x_.Send(Req(2).U32(id_).U32(0x4000).U32(x_.CursorFor(c)).Done());
    x_.FlushOut();
}

// --- events -----------------------------------------------------------------------------------------------------

std::optional<std::string> X11::WaitEvent(int type, uint32_t window, int timeoutMs, uint32_t atom) {
    FlushOut();
    const uint64_t deadline = NowMs() + (uint64_t)timeoutMs;
    size_t scanned = 0;
    for (;;) {
        if (!TakeUnits()) return std::nullopt;
        for (; scanned < events_.size(); ++scanned) {
            const std::string& e = events_[scanned];
            const int t = e[0] & 0x7f;
            if (t != type) continue;
            uint32_t win = t == 31 ? R32(e, 8) : t == 19 ? R32(e, 8) : R32(e, 4);  // SelectionNotify's requestor; MapNotify's window
            if (win != window) continue;
            if (atom && t == 28 && (R32(e, 8) != atom || (uint8_t)e[16] != 0)) continue;  // PropertyNotify NewValue of `atom`
            std::string found = e;
            events_.erase(events_.begin() + (ptrdiff_t)scanned);
            return found;
        }
        // SelectionRequests for what this client owns must not wait behind its own conversion.
        for (size_t i = 0; i < events_.size(); ++i)
            if ((events_[i][0] & 0x7f) == 30) {
                ServeSelection(events_[i]);
                events_.erase(events_.begin() + (ptrdiff_t)i);
                if (i < scanned) --scanned;
                --i;
            }
        uint64_t now = NowMs();
        if (now >= deadline) return std::nullopt;
        ReadSome((int)(deadline - now));
        if (fd_ < 0) return std::nullopt;
    }
}

void X11::Dispatch() {
    while (ReadSome(0)) {}
    if (!TakeUnits()) return;
    while (!events_.empty()) {
        std::string ev = std::move(events_.front());
        events_.pop_front();
        Handle(ev);
    }
    FlushOut();
}

void X11::Handle(const std::string& ev) {
    const int type = ev[0] & 0x7f;
    if (type >= 2 && type <= 8) time_ = R32(ev, 4);
    switch (type) {
    case 2: case 3: Key(ev, type == 2); break;
    case 4: case 5: {  // ButtonPress, ButtonRelease
        X11Window* w = Find(R32(ev, 12));
        if (!w) break;
        const int b = (uint8_t)ev[1];
        const float x = S16(ev, 24), y = S16(ev, 26);
        SyncModifiers(R16(ev, 28) & 1, R16(ev, 28) & 4, R16(ev, 28) & altMask_, R16(ev, 28) & superMask_, R16(ev, 28) & 2, R16(ev, 28) & numMask_);
        if (b >= 4 && b <= 7) {  // the wheel: presses only
            if (type == 4) {
                OnMouseMotion(w->Owner(), x, y);
                OnMouseWheel(w->Owner(), b == 6 ? -1.f : b == 7 ? 1.f : 0.f, b == 4 ? 1.f : b == 5 ? -1.f : 0.f);
            }
            break;
        }
        // X numbers middle 2 and right 3, as SDL; 8 and up are back, forward and beyond (SDL's X1, X2...).
        const int sdl = b <= 3 ? b : b - 4;
        OnMouseButton(w->Owner(), sdl, type == 4, x, y);
        break;
    }
    case 6: {  // MotionNotify
        X11Window* w = Find(R32(ev, 12));
        if (w) OnMouseMotion(w->Owner(), S16(ev, 24), S16(ev, 26));
        break;
    }
    case 7: case 8: {  // EnterNotify, LeaveNotify
        X11Window* w = Find(R32(ev, 12));
        const uint8_t detail = (uint8_t)ev[1], mode = (uint8_t)ev[30];
        if (!w || detail == 2 /* Inferior */) break;
        if (type == 7) {
            OnMouseEnterLeave(w->Owner(), true);
            OnMouseMotion(w->Owner(), S16(ev, 24), S16(ev, 26));
        } else if (mode != 1 /* Grab */ && !grabbed_) {
            OnMouseEnterLeave(w->Owner(), false);
        }
        break;
    }
    case 9: case 10: {  // FocusIn, FocusOut
        X11Window* w = Find(R32(ev, 4));
        const uint8_t detail = (uint8_t)ev[1], mode = (uint8_t)ev[8];
        if (!w || detail == 5 /* Pointer */ || detail == 7 /* None */ || mode == 1 /* Grab */ || mode == 2 /* Ungrab */) break;
        if (type == 10) down_.reset();
        OnFocus(w->Owner(), type == 9);
        break;
    }
    case 12: {  // Expose: once the last of a series arrives
        X11Window* w = Find(R32(ev, 4));
        if (w && R16(ev, 16) == 0) OnExposed(w->Owner());
        break;
    }
    case 18: case 19: {  // UnmapNotify, MapNotify
        X11Window* w = Find(R32(ev, 8));
        if (!w) break;
        if (type == 19) {
            w->mapped = true;
            OnWindowState(w->Owner(), 0, SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED);
            OnExposed(w->Owner());
        } else {
            OnWindowState(w->Owner(), SDL_WINDOW_MINIMIZED, 0);
        }
        break;
    }
    case 22: {  // ConfigureNotify
        X11Window* w = Find(R32(ev, 8));
        if (!w) break;
        if (ev[0] & 0x80) w->lastX = S16(ev, 16), w->lastY = S16(ev, 18);  // synthetic: root coordinates
        const int width = R16(ev, 20), height = R16(ev, 22);
        OnWindowResized(w->Owner(), width, height, width, height);
        break;
    }
    case 28: {  // PropertyNotify
        const uint32_t win = R32(ev, 4), atom = R32(ev, 8);
        if (win == Root() && atom == Atom("RESOURCE_MANAGER")) UpdateScale(true);
        else if (win == xsettingsOwner_ && atom == Atom("_XSETTINGS_SETTINGS")) UpdateScale(true);
        else if (X11Window* w = Find(win); w && atom == Atom("_NET_WM_STATE")) {
            bool maxV = false, maxH = false, hidden = false;
            if (auto p = GetProp(win, atom); p && p->format == 32)
                for (size_t i = 0; i + 4 <= p->data.size(); i += 4) {
                    uint32_t a = R32(p->data, i);
                    maxV |= a == Atom("_NET_WM_STATE_MAXIMIZED_VERT");
                    maxH |= a == Atom("_NET_WM_STATE_MAXIMIZED_HORZ");
                    hidden |= a == Atom("_NET_WM_STATE_HIDDEN");
                }
            OnWindowState(w->Owner(), (maxV && maxH ? SDL_WINDOW_MAXIMIZED : 0) | (hidden ? SDL_WINDOW_MINIMIZED : 0),
                          (maxV && maxH ? 0 : SDL_WINDOW_MAXIMIZED) | (hidden ? 0 : SDL_WINDOW_MINIMIZED));
        }
        break;
    }
    case 29:  // SelectionClear
        if (R32(ev, 12) == Atom("CLIPBOARD") && R32(ev, 8) == selWindow_) owner_ = false, clip_.clear();
        break;
    case 30: ServeSelection(ev); break;
    case 31:  // SelectionNotify not waited for: an XDND drop's data
        for (X11Window* w : windows_)
            if (R32(ev, 8) == w->Id()) DndData(*w, ev);
        break;
    case 33: {  // ClientMessage
        X11Window* w = Find(R32(ev, 4));
        const uint32_t mt = R32(ev, 8);
        if (mt == Atom("WM_PROTOCOLS")) {
            const uint32_t proto = R32(ev, 12);
            if (w && proto == Atom("WM_DELETE_WINDOW")) OnCloseRequest(w->Owner());
            else if (proto == Atom("_NET_WM_PING")) {  // send it back to the root: "still alive"
                std::string pong = ev;
                pong[0] = 33;
                uint32_t root = Root();
                memcpy(&pong[4], &root, 4);
                Req r(25, 0);
                r.U32(root).U32(0x180000);
                r.b += pong;
                Send(r.Done());
            }
        } else if (w) {
            Dnd(*w, ev);
        }
        break;
    }
    case 34:  // MappingNotify: the keyboard (or modifiers) changed
        if (getenv("CORDINAL_X11_DEBUG")) fprintf(stderr, "x11: mapping notify %u %u %u\n", (uint8_t)ev[4], (uint8_t)ev[5], (uint8_t)ev[6]);
        if ((uint8_t)ev[4] <= 1) LoadKeymap();
        break;
    default: break;
    }
}

// --- clipboard ------------------------------------------------------------------------------------------------

bool X11::SetClipboard(const std::string& text) {
    clip_ = text;
    ownedAt_ = time_;
    Send(Req(22).U32(selWindow_).U32(Atom("CLIPBOARD")).U32(time_).Done());
    auto r = Reply(Send(Req(23).U32(Atom("CLIPBOARD")).Done()));
    owner_ = r && R32(*r, 8) == selWindow_;
    return owner_;
}

void X11::ServeSelection(const std::string& ev) {
    const uint32_t time = R32(ev, 4), requestor = R32(ev, 12), selection = R32(ev, 16), target = R32(ev, 20);
    uint32_t property = R32(ev, 24);
    if (!property) property = target;  // an obsolete client
    bool ok = false;
    if (owner_ && selection == Atom("CLIPBOARD")) {
        const uint32_t utf8 = Atom("UTF8_STRING"), mime = Atom("text/plain;charset=utf-8"), plain = Atom("text/plain"),
                       str = Atom("STRING"), text = Atom("TEXT");
        if (target == Atom("TARGETS")) {
            uint32_t list[] = {Atom("TARGETS"), Atom("TIMESTAMP"), utf8, mime, plain, str, text};
            Prop(requestor, property, 4 /* ATOM */, 32, list, 7);
            ok = true;
        } else if (target == Atom("TIMESTAMP")) {
            Prop(requestor, property, 19 /* INTEGER */, 32, &ownedAt_, 1);
            ok = true;
        } else if ((target == utf8 || target == mime || target == plain || target == str || target == text) &&
                   clip_.size() + 64 < MaxRequestBytes()) {  // larger would need INCR, which isn't served
            Prop(requestor, property, target == str ? 31u /* STRING */ : utf8, 8, clip_.data(), clip_.size());
            ok = true;
        }
    }
    std::string reply(32, '\0');
    reply[0] = 31;
    const uint32_t prop = ok ? property : 0;
    memcpy(&reply[4], &time, 4);
    memcpy(&reply[8], &requestor, 4);
    memcpy(&reply[12], &selection, 4);
    memcpy(&reply[16], &target, 4);
    memcpy(&reply[20], &prop, 4);
    Req r(25, 0);
    r.U32(requestor).U32(0);
    r.b += reply;
    Send(r.Done());
    FlushOut();
}

std::vector<std::string> X11::ClipboardTypes() {
    if (owner_) return {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "STRING", "TEXT"};
    const uint32_t prop = Atom("CORDINAL_SELECTION"), clipboard = Atom("CLIPBOARD"), targets = Atom("TARGETS");
    DeleteProp(selWindow_, prop);
    Send(Req(24).U32(selWindow_).U32(clipboard).U32(targets).U32(prop).U32(time_).Done());
    auto notify = WaitEvent(31, selWindow_, 1000);
    std::vector<std::string> out;
    if (!notify || R32(*notify, 20) == 0) return out;
    auto p = GetProp(selWindow_, prop, true);
    if (!p || p->format != 32) return out;
    std::vector<uint64_t> seqs;
    std::vector<uint32_t> unknown;
    for (size_t i = 0; i + 4 <= p->data.size() && i < 4 * 512; i += 4) {
        const uint32_t a = R32(p->data, i);
        bool known = false;
        for (const auto& [name, atom] : atoms_)
            if (atom == a) out.push_back(name), known = true;
        if (!known) unknown.push_back(a), seqs.push_back(Send(Req(17).U32(a).Done()));
    }
    for (size_t i = 0; i < seqs.size(); ++i)
        if (auto r = Reply(seqs[i]); r && 32 + (size_t)R16(*r, 8) <= r->size()) {
            std::string name = r->substr(32, R16(*r, 8));
            atoms_[name] = unknown[i];
            out.push_back(name);
        }
    return out;
}

std::optional<std::string> X11::GetClipboard(const std::string& mime) {
    if (owner_) {
        if (mime.rfind("text/plain", 0) == 0 || mime == "UTF8_STRING" || mime == "STRING" || mime == "TEXT") return clip_;
        return std::nullopt;
    }
    const uint32_t prop = Atom("CORDINAL_SELECTION"), target = Atom(mime);
    if (!target) return std::nullopt;
    DeleteProp(selWindow_, prop);
    Send(Req(24).U32(selWindow_).U32(Atom("CLIPBOARD")).U32(target).U32(prop).U32(time_).Done());
    auto notify = WaitEvent(31, selWindow_, 1500);
    if (!notify || R32(*notify, 20) == 0) return std::nullopt;
    auto p = GetProp(selWindow_, prop, true);
    if (!p) return std::nullopt;
    if (p->type != Atom("INCR")) return p->data;
    // INCR: the owner writes the value in pieces; each read (with delete) asks for the next, and an
    // empty piece ends it.
    std::string all;
    for (;;) {
        if (!WaitEvent(28, selWindow_, 2000, prop)) return std::nullopt;
        auto piece = GetProp(selWindow_, prop, true);
        if (!piece) return std::nullopt;
        if (piece->data.empty()) return all;
        if (all.size() + piece->data.size() > kMaxReply) return std::nullopt;
        all += piece->data;
    }
}

// --- drag and drop (XDND 5) -----------------------------------------------------------------------------------

void X11::Dnd(X11Window& w, const std::string& ev) {
    const uint32_t mt = R32(ev, 8), source = R32(ev, 12);
    if (mt == Atom("XdndEnter")) {
        drag_ = {};
        drag_.source = source;
        drag_.version = R32(ev, 16) >> 24;
        std::vector<uint32_t> types;
        if (R32(ev, 16) & 1) {
            if (auto p = GetProp(source, Atom("XdndTypeList")); p && p->format == 32)
                for (size_t i = 0; i + 4 <= p->data.size() && i < 4 * 256; i += 4) types.push_back(R32(p->data, i));
        } else {
            for (int i = 0; i < 3; ++i)
                if (uint32_t t = R32(ev, 20 + 4 * i)) types.push_back(t);
        }
        for (const char* want : {"text/uri-list", "UTF8_STRING", "text/plain;charset=utf-8", "text/plain"}) {
            const uint32_t a = Atom(want);
            if (std::find(types.begin(), types.end(), a) != types.end()) {
                drag_.type = a;
                break;
            }
        }
    } else if (mt == Atom("XdndPosition") && source == drag_.source) {
        const uint32_t xy = R32(ev, 20);
        auto r = Reply(Send(Req(40).U32(Root()).U32(w.Id()).U16((uint16_t)(xy >> 16)).U16((uint16_t)(xy & 0xffff)).Done()));
        if (r) drag_.x = S16(*r, 12), drag_.y = S16(*r, 14);
        ClientMessage(w.Id(), Atom("XdndStatus"), {w.Id(), drag_.type ? 3u : 0u, 0, 0, drag_.type ? Atom("XdndActionCopy") : 0}, source);
        FlushOut();
    } else if (mt == Atom("XdndLeave") && source == drag_.source) {
        drag_ = {};
    } else if (mt == Atom("XdndDrop") && source == drag_.source) {
        if (!drag_.type) {
            ClientMessage(w.Id(), Atom("XdndFinished"), {w.Id(), 0, 0}, source);
            drag_ = {};
            return;
        }
        drag_.dropping = true;
        const uint32_t time = R32(ev, 20);
        Send(Req(24).U32(w.Id()).U32(Atom("XdndSelection")).U32(drag_.type).U32(Atom("CORDINAL_DROP")).U32(time ? time : time_).Done());
        FlushOut();
    }
}

void X11::DndData(X11Window& w, const std::string& ev) {
    if (!drag_.dropping || R32(ev, 12) != Atom("XdndSelection")) return;
    bool ok = false;
    if (R32(ev, 20)) {
        if (auto p = GetProp(w.Id(), Atom("CORDINAL_DROP"), true); p && p->format == 8) {
            ok = true;
            if (drag_.type == Atom("text/uri-list")) {
                for (const std::string& path : ParseUriList(p->data, false))
                    OnDrop(w.Owner(), !path.empty() && path[0] == '/', path, drag_.x, drag_.y);
            } else {
                OnDrop(w.Owner(), false, p->data, drag_.x, drag_.y);
            }
        }
    }
    ClientMessage(w.Id(), Atom("XdndFinished"), {w.Id(), ok ? 1u : 0u, ok ? Atom("XdndActionCopy") : 0}, drag_.source);
    FlushOut();
    drag_ = {};
}

// --- the rest -----------------------------------------------------------------------------------------------------

bool X11::UsableBounds(SDL_Rect& r) {
    r = {0, 0, Scr().width, Scr().height};
    auto area = GetProp(Root(), Atom("_NET_WORKAREA"));
    if (!area || area->format != 32 || area->data.size() < 16) return true;
    size_t desk = 0;
    if (auto cur = GetProp(Root(), Atom("_NET_CURRENT_DESKTOP")); cur && cur->format == 32 && cur->data.size() >= 4)
        desk = R32(cur->data, 0);
    if ((desk + 1) * 16 > area->data.size()) desk = 0;
    r = {(int)R32(area->data, desk * 16), (int)R32(area->data, desk * 16 + 4), (int)R32(area->data, desk * 16 + 8),
         (int)R32(area->data, desk * 16 + 12)};
    return true;
}

bool X11::GlobalMouse(float& x, float& y) {
    auto r = Reply(Send(Req(38).U32(Root()).Done()));
    if (!r) return false;
    x = S16(*r, 16), y = S16(*r, 18);
    return true;
}

void X11::CaptureMouse(bool on) {
    if (on == grabbed_) return;
    X11Window* w = nullptr;
    for (X11Window* c : windows_)
        if (c->Owner()->flags & SDL_WINDOW_MOUSE_FOCUS) w = c;
    if (on && !w) return;
    if (on) {
        auto r = Reply(Send(Req(26, 1).U32(w->Id()).U16(kButtonPressMask | kButtonReleaseMask | kPointerMotionMask).U8(1).U8(1)
                                .U32(0).U32(0).U32(time_).Done()));
        grabbed_ = r && (uint8_t)(*r)[1] == 0;
    } else {
        Send(Req(27).U32(time_).Done());
        FlushOut();
        grabbed_ = false;
    }
}

}  // namespace

std::unique_ptr<Backend> CreateX11(std::string& error) {
    auto x = std::make_unique<X11>();
    if (!x->Open(error)) return nullptr;
    return x;
}

}  // namespace lx::platform
