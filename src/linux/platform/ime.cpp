// The input method client for X11 windows (ime.h): fcitx5's InputContext1 or ibus's portal
// InputContext, found on the session bus and spoken to from a thread of its own.
#include "platform/ime.h"

#include "platform/platform.h"

#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

namespace lx::platform::ime {
namespace {

using dbus::Reader;
using dbus::Writer;

enum class Kind { None, Fcitx, IBus };

constexpr const char* kFcitx = "org.fcitx.Fcitx5";
constexpr const char* kFcitxPath = "/org/freedesktop/portal/inputmethod";
constexpr const char* kFcitxIm = "org.fcitx.Fcitx.InputMethod1";
constexpr const char* kFcitxIc = "org.fcitx.Fcitx.InputContext1";
constexpr const char* kIBus = "org.freedesktop.portal.IBus";
constexpr const char* kIBusPath = "/org/freedesktop/IBus";
constexpr const char* kIBusPortal = "org.freedesktop.IBus.Portal";
constexpr const char* kIBusIc = "org.freedesktop.IBus.InputContext";
constexpr const char* kDaemonName = "org.freedesktop.DBus";

// fcitx5's capability flags: it may send a composition (Preedit), with its formatting split out
// (FormattedPreedit), instead of drawing the composition in a window of its own.
constexpr uint64_t kFcitxCapabilities = 1u << 1 | 1u << 4;
// ibus: PREEDIT_TEXT and FOCUS.
constexpr uint32_t kIBusCapabilities = 1u << 0 | 1u << 3;
constexpr uint32_t kIBusRelease = 1u << 30;

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// The input method the session asked for: XMODIFIERS first, what X11 programs read, then the
// toolkits' own variables. None: whichever is running.
Kind Asked() {
    for (const char* var : {"XMODIFIERS", "GTK_IM_MODULE", "QT_IM_MODULE", "SDL_IM_MODULE"}) {
        const char* v = getenv(var);
        if (!v) continue;
        if (strstr(v, "fcitx")) return Kind::Fcitx;
        if (strstr(v, "ibus")) return Kind::IBus;
    }
    return Kind::None;
}

struct Command {
    enum Type { kFocus, kCursor, kKey } type;
    bool on = false;
    int x = 0, y = 0, w = 0, h = 0;
    std::string raw;
    bool down = false, repeat = false;
    uint32_t sym = 0, keycode = 0, state = 0, time = 0;
};

struct Pending {  // a key handed to the input method, until it answers
    Command c;
    bool answered = false, taken = false;
};

}  // namespace

struct Client::State {
    // Shared with the window's thread.
    std::mutex mu;
    std::deque<Command> in;
    std::vector<Out> out;
    std::atomic<bool> quit{false}, ready{false}, focused{false};
    std::atomic<int> inFlight{0};  // keys handed over and not yet given back or taken
    std::deque<std::shared_ptr<Pending>> keys;  // in the order they came, under mu
    bool dead = false;  // the thread ended: keys come straight back (under mu)
    int wake = -1;
    std::thread thread;

    // The thread's own.
    std::unique_ptr<dbus::Bus> bus;
    Kind kind = Kind::None;
    std::string ic;  // the input context's object path, empty without one
    bool creating = false, haveFocus = false, shown = false;
    int64_t lastTry = -100000;
    int cx = 0, cy = 0, cw = 0, ch = 0;
    bool haveCursor = false;
    std::string ibusPreedit;  // ibus shows and hides its composition apart from updating it
    int ibusCursor = 0;

    const char* Service() const { return kind == Kind::Fcitx ? kFcitx : kIBus; }
    const char* Iface() const { return kind == Kind::Fcitx ? kFcitxIc : kIBusIc; }
    void Push(Out o) {
        {
            std::lock_guard lock(mu);
            out.push_back(std::move(o));
        }
        WakeEventLoop();
    }
    void Preedit(std::string text, int cursor) {
        shown = !text.empty();
        Out o;
        o.kind = Out::kPreedit;
        o.text = std::move(text);
        o.cursor = cursor;
        Push(std::move(o));
    }
    void CallIc(const char* member, std::string_view sig = {}, const Writer& body = {}) {
        if (!ic.empty()) bus->Send({Service(), ic, Iface(), member}, sig, body);
    }
    void SendCursor() {
        if (ic.empty() || !haveCursor || !haveFocus) return;
        Writer w;
        w.I(cx), w.I(cy), w.I(cw), w.I(ch);
        CallIc(kind == Kind::Fcitx ? "SetCursorRect" : "SetCursorLocation", "iiii", w);
    }
    void AddMatch(const std::string& rule) {
        Writer w;
        w.S(rule);
        bus->Send({kDaemonName, "/org/freedesktop/DBus", kDaemonName, "AddMatch"}, "s", w);
    }
    // The input method went away: the field types on its own again.
    void Drop() {
        ic.clear();
        ready = false;
        if (shown) Preedit({}, 0);
    }

    // --- finding an input method and making a context ---------------------------------------------------
    void Connect() {
        if (!ic.empty() || creating || NowMs() - lastTry < 3000) return;
        lastTry = NowMs();
        creating = true;
        const Kind asked = Asked();
        std::vector<Kind> order;
        if (asked != Kind::IBus) order.push_back(Kind::Fcitx);
        if (asked != Kind::Fcitx) order.push_back(Kind::IBus);
        Try(order, 0, asked);
    }
    void Try(std::vector<Kind> order, size_t i, Kind asked) {
        if (i >= order.size()) {
            creating = false;
            return;
        }
        kind = order[i];
        // ibus's portal starts on demand, so a session that asked for ibus gets it started; any
        // other input method is only used when it already runs.
        if (kind == Kind::IBus && asked == Kind::IBus) return Create(order, i, asked);
        Writer w;
        w.S(Service());
        bus->Call({kDaemonName, "/org/freedesktop/DBus", kDaemonName, "NameHasOwner"}, "s", w, 2000,
                  [this, order, i, asked](const dbus::Message* m) {
                      if (m && m->type == dbus::kReturn && m->sig == "b" && m->Body().B()) Create(order, i, asked);
                      else Try(order, i + 1, asked);
                  });
    }
    void Create(std::vector<Kind> order, size_t i, Kind asked) {
        Writer w;
        dbus::Target t;
        std::string_view sig;
        if (kind == Kind::Fcitx) {
            Writer::Array a = w.Open(8);
            for (auto [key, value] : {std::pair{"program", "cordinal"}, std::pair{"display", "x11:"}}) {
                w.Align(8);
                w.S(key), w.S(value);
            }
            w.Close(a);
            t = {kFcitx, kFcitxPath, kFcitxIm, "CreateInputContext"};
            sig = "a(ss)";
        } else {
            w.S("cordinal");
            t = {kIBus, kIBusPath, kIBusPortal, "CreateInputContext"};
            sig = "s";
        }
        bus->Call(t, sig, w, 5000, [this, order, i, asked](const dbus::Message* m) {
            const bool ok = m && m->type == dbus::kReturn && (m->sig == "oay" || m->sig == "o");
            if (!ok) return Try(order, i + 1, asked);
            Reader r = m->Body();
            ic = std::string(r.O());
            creating = false;
            AddMatch("type='signal',sender='" + std::string(Service()) + "',path='" + ic + "'");
            AddMatch("type='signal',sender='org.freedesktop.DBus',member='NameOwnerChanged',arg0='" +
                     std::string(Service()) + "'");
            Writer caps;
            if (kind == Kind::Fcitx) caps.X((int64_t)kFcitxCapabilities), CallIc("SetCapability", "t", caps);
            else caps.U(kIBusCapabilities), CallIc("SetCapabilities", "u", caps);
            ready = true;
            if (haveFocus) {
                CallIc("FocusIn");
                SendCursor();
            }
        });
    }

    // --- the window's requests --------------------------------------------------------------------------
    void Handle(Command& c) {
        switch (c.type) {
        case Command::kFocus:
            haveFocus = c.on;
            if (c.on) {
                if (ic.empty()) Connect();
                else CallIc("FocusIn"), SendCursor();
            } else if (!ic.empty()) {
                // A composition left behind is dropped with the field, as a click away drops it.
                CallIc("Reset");
                CallIc("FocusOut");
                if (shown) Preedit({}, 0);
            }
            break;
        case Command::kCursor:
            if (haveCursor && c.x == cx && c.y == cy && c.w == cw && c.h == ch) break;
            cx = c.x, cy = c.y, cw = c.w, ch = c.h, haveCursor = true;
            SendCursor();
            break;
        case Command::kKey: {
            // Keys come back in the order they went: one the input method answers late holds back
            // the ones after it, so a fast typist's keys never overtake each other.
            auto key = std::make_shared<Pending>();
            key->c = std::move(c);
            keys.push_back(key);
            if (ic.empty() || !haveFocus) {
                key->answered = true;
                Flush();
                break;
            }
            const Command& k = key->c;
            Writer w;
            std::string_view sig;
            if (kind == Kind::Fcitx) {
                w.U(k.sym), w.U(k.keycode), w.U(k.state), w.B(!k.down), w.U(k.time);
                sig = "uuubu";
            } else {
                // ibus counts keycodes from evdev's zero, and marks a release in the state.
                w.U(k.sym), w.U(k.keycode >= 8 ? k.keycode - 8 : 0), w.U(k.state | (k.down ? 0 : kIBusRelease));
                sig = "uuu";
            }
            bus->Call({Service(), ic, Iface(), "ProcessKeyEvent"}, sig, w, 1500, [this, key](const dbus::Message* m) {
                key->taken = m && m->type == dbus::kReturn && m->sig == "b" && m->Body().B();
                if (m && m->type == dbus::kError) Drop();  // the context or its service is gone
                key->answered = true;
                Flush();
            });
            break;
        }
        }
    }
    // Hands back every answered key at the front of the line that the input method didn't take.
    void Flush() {
        bool any = false;
        {
            std::lock_guard lock(mu);
            while (!keys.empty() && keys.front()->answered) {
                Pending& p = *keys.front();
                if (!p.taken) {
                    Out o;
                    o.kind = Out::kReplay;
                    o.raw = std::move(p.c.raw);
                    o.down = p.c.down, o.repeat = p.c.repeat;
                    out.push_back(std::move(o));
                    any = true;
                }
                keys.pop_front();
                --inFlight;
            }
        }
        if (any) WakeEventLoop();
    }

    // --- what the input method says --------------------------------------------------------------------
    void Signal(const dbus::Message& m) {
        if (m.member == "NameOwnerChanged" && m.sig == "sss") {
            Reader r = m.Body();
            std::string_view name = r.S(), oldOwner = r.S(), newOwner = r.S();
            (void)oldOwner;
            if (name == Service() && newOwner.empty() && !ic.empty()) Drop();
            return;
        }
        if (ic.empty() || m.path != ic) return;
        Reader r = m.Body();
        if (m.member == "CommitString" && m.sig == "s") {  // fcitx5
            Commit(std::string(r.S()));
        } else if (m.member == "UpdateFormattedPreedit" && m.sig == "a(si)i") {
            std::string text;
            int cursor = 0;
            if (FcitxPreedit(r, text, cursor)) Preedit(std::move(text), cursor);
        } else if (m.member == "ForwardKey" && m.sig == "uub") {
            const uint32_t sym = r.U(), state = r.U();
            Synthetic(sym, state, !r.B());
        } else if (m.member == "CommitText" && m.sig == "v") {  // ibus
            std::string text;
            if (IBusText(r, text)) Commit(std::move(text));
        } else if (m.member == "UpdatePreeditText" && m.sig == "vub") {
            std::string text;
            if (!IBusText(r, text)) return;
            const uint32_t chars = r.U();
            const bool visible = r.B();
            ibusCursor = CharsToBytes(text, chars);
            ibusPreedit = std::move(text);
            if (visible || shown) Preedit(visible ? ibusPreedit : std::string(), visible ? ibusCursor : 0);
        } else if (m.member == "ShowPreeditText") {
            Preedit(ibusPreedit, ibusCursor);
        } else if (m.member == "HidePreeditText") {
            if (shown) Preedit({}, 0);
        } else if (m.member == "ForwardKeyEvent" && m.sig == "uuu") {
            const uint32_t sym = r.U();
            r.U();
            const uint32_t state = r.U();
            Synthetic(sym, state & ~kIBusRelease, !(state & kIBusRelease));
        }
    }
    void Commit(std::string text) {
        if (shown) Preedit({}, 0);  // the commit takes the composition's place
        if (text.empty()) return;
        Out o;
        o.kind = Out::kCommit;
        o.text = std::move(text);
        Push(std::move(o));
    }
    void Synthetic(uint32_t sym, uint32_t state, bool down) {
        Out o;
        o.kind = Out::kKey;
        o.sym = sym, o.state = state, o.down = down;
        Push(std::move(o));
    }

    void Run() {
        bus = dbus::Bus::OpenUser(2000);
        if (bus) Serve();  // without a session bus the fields type on their own, as before
        Finish();
    }
    void Serve() {
        bus->onMessage = [this](dbus::Bus&, const dbus::Message& m) {
            if (m.type == dbus::kSignal) Signal(m);
        };
        while (!quit) {
            std::deque<Command> todo;
            {
                std::lock_guard lock(mu);
                todo.swap(in);
            }
            for (Command& c : todo) Handle(c);
            bus->Flush();
            const int more = bus->Process(64);
            if (more < 0) break;  // the bus closed: Taking() turns false and keys go straight through
            pollfd fds[2] = {{wake, POLLIN, 0}, {bus->Fd(), bus->Events(), 0}};
            if (poll(fds, 2, more > 0 ? 0 : bus->TimeoutMs()) < 0 && errno != EINTR) break;
            if (fds[0].revents & POLLIN) {
                uint64_t v;
                (void)!read(wake, &v, sizeof v);
            }
        }
    }
    void Finish() {
        ready = false;
        // Keys handed over and not yet answered come back, in order, so none is lost.
        {
            std::lock_guard lock(mu);
            for (auto& p : keys)
                if (!(p->answered && p->taken)) {
                    Out o;
                    o.kind = Out::kReplay;
                    o.raw = std::move(p->c.raw);
                    o.down = p->c.down, o.repeat = p->c.repeat;
                    out.push_back(std::move(o));
                }
            keys.clear();
            for (Command& c : in)
                if (c.type == Command::kKey) {
                    Out o;
                    o.kind = Out::kReplay;
                    o.raw = std::move(c.raw);
                    o.down = c.down, o.repeat = c.repeat;
                    out.push_back(std::move(o));
                }
            in.clear();
            inFlight = 0;
            dead = true;
        }
        WakeEventLoop();
    }
};

std::unique_ptr<Client> Client::Start() {
    auto s = std::make_unique<State>();
    s->wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (s->wake < 0) return nullptr;
    State* p = s.get();
    s->thread = std::thread([p] { p->Run(); });
    return std::unique_ptr<Client>(new Client(std::move(s)));
}

Client::Client(std::unique_ptr<State> s) : s_(std::move(s)) {}

Client::~Client() {
    s_->quit = true;
    uint64_t one = 1;
    (void)!write(s_->wake, &one, sizeof one);
    if (s_->thread.joinable()) s_->thread.join();
    close(s_->wake);
}

namespace {
void Post(Client::State& s, Command c) {
    {
        std::lock_guard lock(s.mu);
        s.in.push_back(std::move(c));
    }
    uint64_t one = 1;
    (void)!write(s.wake, &one, sizeof one);
}
}  // namespace

void Client::Focus(bool on) {
    if (s_->focused.exchange(on) == on) return;
    Command c{Command::kFocus};
    c.on = on;
    Post(*s_, std::move(c));
}

void Client::Cursor(int x, int y, int w, int h) {
    Command c{Command::kCursor};
    c.x = x, c.y = y, c.w = w, c.h = h;
    Post(*s_, std::move(c));
}

bool Client::Taking() const { return s_->focused && s_->ready; }

bool Client::Busy() const { return s_->inFlight > 0; }

void Client::Key(std::string raw, bool down, bool repeat, uint32_t sym, uint32_t keycode, uint32_t state, uint32_t time) {
    {
        std::lock_guard lock(s_->mu);
        if (s_->dead) {  // nobody to ask: the key goes back as it came
            Out o;
            o.kind = Out::kReplay;
            o.raw = std::move(raw);
            o.down = down, o.repeat = repeat;
            s_->out.push_back(std::move(o));
            WakeEventLoop();
            return;
        }
        ++s_->inFlight;
    }
    Command c{Command::kKey};
    c.raw = std::move(raw);
    c.down = down, c.repeat = repeat;
    c.sym = sym, c.keycode = keycode, c.state = state, c.time = time;
    Post(*s_, std::move(c));
}

std::vector<Out> Client::Take() {
    std::lock_guard lock(s_->mu);
    return std::exchange(s_->out, {});
}

// --- parsing ------------------------------------------------------------------------------------------------

bool FcitxPreedit(Reader& r, std::string& text, int& cursor) {
    // Segments of (text, format flags) and the cursor's byte offset in their concatenation, -1 for none.
    text.clear();
    for (size_t end = r.Open(8); r.In(end);) {
        r.Struct();
        text += r.S();
        r.I();
    }
    const int at = r.I();
    cursor = at < 0 || at > (int)text.size() ? (int)text.size() : at;
    return r.ok();
}

bool IBusText(Reader& r, std::string& text) {
    // IBusText serializes as ("IBusText", attachments, text, attributes) inside a variant.
    if (r.G() != "(sa{sv}sv)") return false;
    r.Struct();
    if (r.S() != "IBusText") return false;
    r.Skip("a{sv}");
    text = std::string(r.S());
    r.Skip("v");
    return r.ok();
}

int CharsToBytes(std::string_view s, uint32_t chars) {
    size_t i = 0;
    for (uint32_t n = 0; i < s.size() && n < chars; ++n)
        do ++i;
        while (i < s.size() && ((uint8_t)s[i] & 0xC0) == 0x80);
    return (int)i;
}

int CheckParsing() {
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        printf("%s %s\r\n", ok ? "ok  " : "FAIL", what);
        failures += !ok;
    };
    {
        Writer w;  // fcitx5: "とう" underlined, "きょう" highlighted, the cursor after both
        Writer::Array a = w.Open(8);
        w.Align(8), w.S("\xE3\x81\xA8\xE3\x81\x86"), w.I(8);
        w.Align(8), w.S("\xE3\x81\x8D\xE3\x82\x87\xE3\x81\x86"), w.I(16);
        w.Close(a);
        w.I(15);
        Reader r(w.b);
        std::string text;
        int cursor = -2;
        check(FcitxPreedit(r, text, cursor) && text == "\xE3\x81\xA8\xE3\x81\x86\xE3\x81\x8D\xE3\x82\x87\xE3\x81\x86" &&
                  cursor == 15,
              "fcitx5's formatted composition joins its segments and keeps the cursor's byte offset");
        Writer none;
        Writer::Array b = none.Open(8);
        none.Close(b);
        none.I(-1);
        Reader empty(none.b);
        check(FcitxPreedit(empty, text, cursor) && text.empty() && cursor == 0, "an empty fcitx5 composition ends it");
    }
    {
        Writer w;  // ibus: an IBusText holding "한국" with an empty attribute list
        w.G("(sa{sv}sv)");
        w.Align(8);
        w.S("IBusText");
        Writer::Array attachments = w.Open(8);
        w.Close(attachments);
        w.S("\xED\x95\x9C\xEA\xB5\xAD");
        w.G("(sa{sv}av)");
        w.Align(8);
        w.S("IBusAttrList");
        Writer::Array a2 = w.Open(8);
        w.Close(a2);
        Writer::Array list = w.Open(1);
        w.Close(list);
        Reader r(w.b);
        std::string text;
        check(IBusText(r, text) && text == "\xED\x95\x9C\xEA\xB5\xAD", "an IBusText gives its UTF-8 text");
        check(CharsToBytes(text, 1) == 3 && CharsToBytes(text, 2) == 6 && CharsToBytes(text, 9) == 6,
              "ibus's character cursor becomes a byte offset");
        Writer other;
        other.G("s");
        other.S("plain");
        Reader wrong(other.b);
        check(!IBusText(wrong, text), "a variant that is not an IBusText is refused");
    }
    return failures;
}

}  // namespace lx::platform::ime
