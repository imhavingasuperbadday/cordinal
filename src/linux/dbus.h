// A small session-bus client written to the D-Bus specification, in place of libsystemd's sd-bus.
// It covers what the player uses (MPRIS, the tray icon and its menu, second-instance forwarding and
// the GlobalShortcuts portal) and nothing more: the session bus only, a Unix socket only, SASL
// EXTERNAL as the current user, no fd passing, no daemon start-up and no shelling out.
//
// Every incoming message is checked in full against its signature before anything reads it, with
// fixed caps on size and nesting; a message that fails closes the connection, because a stream the
// parser disagrees with cannot be framed again. A Bus is not thread-safe: one thread owns it.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lx::dbus {

constexpr size_t kMaxMessage = 1 << 20;  // a peer's message, header and body together
constexpr size_t kMaxHeader = 1 << 16;

enum Type : uint8_t { kCall = 1, kReturn = 2, kError = 3, kSignal = 4 };
enum Flag : uint8_t { kNoReply = 1 };

// Builds a little-endian body (or header); alignment is relative to the start, which the message
// layout keeps on an 8-byte boundary.
class Writer {
public:
    std::string b;
    void Align(size_t n) { b.resize((b.size() + n - 1) / n * n, '\0'); }
    void Y(uint8_t v) { b.push_back((char)v); }
    void B(bool v) { U(v ? 1 : 0); }
    void I(int32_t v) { Put(v); }
    void U(uint32_t v) { Put(v); }
    void X(int64_t v) { Put(v); }
    void D(double v) { Put(v); }
    void S(std::string_view v);  // invalid UTF-8 becomes U+FFFD and a NUL ends it: the bus drops either
    void O(std::string_view v) { S(v); }
    void G(std::string_view v);
    struct Array { size_t length, start; };
    Array Open(size_t elementAlign);
    void Close(Array a);
    void Entry(std::string_view key, std::string_view sig) { Align(8), S(key), G(sig); }  // {sv}: then the value
    void Strings(std::initializer_list<std::string_view> list);

private:
    template <class T> void Put(T v) {
        Align(sizeof(T));
        b.append((const char*)&v, sizeof(T));
    }
};

// Reads a body that has already passed Check, so a reader only goes wrong when a handler reads a type
// its signature does not have; then ok() turns false and every read returns zero.
class Reader {
public:
    Reader(std::string_view data, bool swap = false, size_t pos = 0) : d_(data), p_(pos), swap_(swap) {}
    bool ok() const { return ok_; }
    size_t pos() const { return p_; }
    bool Align(size_t n);  // padding must be zero
    uint8_t Y();
    bool B();
    int32_t I() { return (int32_t)Fixed<uint32_t>(); }
    uint32_t U() { return Fixed<uint32_t>(); }
    int64_t X() { return (int64_t)Fixed<uint64_t>(); }
    double D();
    std::string_view S();
    std::string_view O();
    std::string_view G();
    size_t Open(size_t elementAlign);  // returns where the array ends
    bool In(size_t end) const { return ok_ && p_ < end; }
    void Struct() { Align(8); }
    bool Skip(std::string_view sig);  // walks complete types, checking them as it goes

private:
    template <class T> T Fixed();
    bool Walk(std::string_view& sig, int depth);
    bool Fail() { return ok_ = false; }
    std::string_view d_;
    size_t p_;
    bool swap_, ok_ = true;
};

size_t TypeLength(std::string_view sig, size_t at = 0, int depth = 0);  // one complete type, 0 if invalid
bool ValidSignature(std::string_view sig);
bool ValidPath(std::string_view path);

struct Message {
    uint8_t type = 0, flags = 0;
    bool swap = false;
    uint32_t serial = 0, replySerial = 0;
    std::string path, iface, member, error, dest, sender, sig, body;
    Reader Body() const { return Reader(body, swap); }
};

// Splits one message off the front of `buffer`: 1 and `used` set, 0 when more bytes are needed, -1 when
// the stream is malformed and must be closed.
int ParseMessage(std::string_view buffer, Message& out, size_t& used);

// The session bus addresses worth connecting to: unix path= and abstract= only, in order. Every other
// transport (tcp, unixexec, autolaunch, launchd, systemd, unix dir= or tmpdir=) is skipped.
struct Address {
    bool abstract;
    std::string path;
};
std::vector<Address> ParseAddresses(std::string_view text);

struct Target {
    std::string_view dest, path, iface, member;
};

class Bus {
public:
    // Connects, authenticates and says Hello within `timeoutMs`; null when there is no usable bus.
    static std::unique_ptr<Bus> OpenUser(int timeoutMs = 5000);
    ~Bus();

    using Handler = std::function<void(Bus&, const Message&)>;
    using ReplyFn = std::function<void(const Message*)>;  // null on timeout
    Handler onMessage;  // method calls and signals; replies go to their caller

    int Fd() const { return fd_; }
    short Events() const;
    int TimeoutMs() const;  // until the earliest pending call times out, -1 with none
    const std::string& Unique() const { return unique_; }

    // Reads what is waiting without blocking and handles up to `budget` messages. Returns -1 once the
    // connection is closed, 1 when more is ready and 0 when idle.
    int Process(int budget = 32);
    bool Flush();

    void Call(const Target& t, std::string_view sig, const Writer& body, int timeoutMs, ReplyFn reply);
    void Send(const Target& t, std::string_view sig, const Writer& body) { Call(t, sig, body, 0, nullptr); }
    // Blocks for the reply (or error reply); other messages wait for the next Process.
    std::optional<Message> CallSync(const Target& t, std::string_view sig, const Writer& body, int timeoutMs);
    void Signal(std::string_view path, std::string_view iface, std::string_view member, std::string_view sig,
                const Writer& body);
    void Reply(const Message& call, std::string_view sig, const Writer& body);
    void Error(const Message& call, std::string_view name, std::string_view text);

private:
    Bus() = default;
    uint32_t Queue(uint8_t type, uint8_t flags, const Target& t, std::string_view sig, const Writer& body,
                   uint32_t replyTo = 0, std::string_view error = {});
    bool Read();
    int Next(Message& m);
    void Deliver(Message& m);
    void Close();
    bool Connect(const Address& a, int64_t deadline);
    bool Authenticate(int64_t deadline);
    bool WaitFor(short events, int64_t deadline);

    struct Pending {
        uint32_t serial;
        int64_t deadline;
        ReplyFn fn;
    };
    int fd_ = -1;
    uint32_t serial_ = 0;
    std::string in_, out_, unique_;
    std::vector<Message> queued_;
    std::vector<Pending> pending_;
};

// Exported objects, described by tables: Properties (Get, GetAll, Set), Introspectable and Peer.Ping are
// answered from them, so a handler sees only a call whose body already matches its signature.
struct Call {
    Bus& bus;
    const Message& msg;
    Reader in;
    Writer out;  // the reply body, in the method's output signature
};
struct Method {
    const char *name, *in, *out;
    void (*fn)(Call&);
};
struct Property {
    const char *name, *sig;
    bool writable = false;
};
struct SignalInfo {
    const char *name, *sig;
};
struct Interface {
    const char* path;
    const char* name;
    std::span<const Method> methods;
    std::span<const Property> properties;
    std::span<const SignalInfo> signals;
    void (*get)(std::string_view property, Writer& value) = nullptr;
    void (*set)(std::string_view property, Reader& value) = nullptr;  // value already has the right type
};

void Dispatch(Bus& bus, const Message& m, std::span<const Interface> objects);
// PropertiesChanged with the named properties' current values and nothing invalidated.
void EmitChanged(Bus& bus, const Interface& object, std::span<const char* const> names);

}  // namespace lx::dbus
