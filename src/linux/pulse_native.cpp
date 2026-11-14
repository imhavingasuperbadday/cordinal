// PulseAudio's native protocol, client side (pulse_native.h). Written from PulseAudio 17's
// protocol (src/pulsecore/native-common.h, tagstruct.c, pstream.c and libpulse's context.c,
// stream.c and introspect.c, LGPL-2.1), stable since protocol version 13. A packet is a 20-byte
// big-endian descriptor {length, channel, offset high, offset low, flags} and a payload: on
// channel -1 a tagstruct (typed values, each led by a tag byte) holding a command, a tag that
// pairs it with its reply, and the command's arguments; on a stream's channel, audio.
#include "pulse_native.h"

#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

namespace lx::pulse {

namespace {

using Dict = std::vector<std::pair<std::string, std::string>>;

constexpr uint32_t kInvalid = 0xffffffffu;
constexpr uint32_t kVersion = 32;  // the protocol version this client speaks (PulseAudio 15 and pipewire-pulse speak 35)
constexpr size_t kMaxPacket = 16u << 20;

enum : uint32_t {
    kError = 0, kReply = 2, kCreatePlaybackStream = 3, kAuth = 8, kSetClientName = 9, kGetPlaybackLatency = 14,
    kGetServerInfo = 20, kGetSinkInfoList = 22, kCorkPlaybackStream = 41, kFlushPlaybackStream = 42, kRequest = 61,
    kPlaybackStreamKilled = 64,
};
constexpr uint8_t kSampleFloat32LE = 5;
constexpr uint32_t kVolumeNorm = 0x10000;

// ---- Tagstructs

class Tags {
public:
    std::vector<uint8_t> bytes;
    void U32(uint32_t v) {
        bytes.push_back('L');
        Be32(v);
    }
    void U8(uint8_t v) {
        bytes.push_back('B');
        bytes.push_back(v);
    }
    void Bool(bool v) { bytes.push_back(v ? '1' : '0'); }
    void Str(const std::string& s) {
        bytes.push_back('t');
        bytes.insert(bytes.end(), s.begin(), s.end());
        bytes.push_back(0);
    }
    void Null() { bytes.push_back('N'); }
    void Arbitrary(const void* p, uint32_t n) {
        bytes.push_back('x');
        Be32(n);
        const uint8_t* b = static_cast<const uint8_t*>(p);
        bytes.insert(bytes.end(), b, b + n);
    }
    void SampleSpec(uint8_t format, uint8_t channels, uint32_t rate) {
        bytes.push_back('a');
        bytes.push_back(format);
        bytes.push_back(channels);
        Be32(rate);
    }
    void ChannelMap(std::initializer_list<uint8_t> positions) {
        bytes.push_back('m');
        bytes.push_back((uint8_t)positions.size());
        bytes.insert(bytes.end(), positions.begin(), positions.end());
    }
    void CVolume(uint8_t channels, uint32_t volume) {
        bytes.push_back('v');
        bytes.push_back(channels);
        for (uint8_t i = 0; i < channels; ++i) Be32(volume);
    }
    void Proplist(const Dict& d) {
        bytes.push_back('P');
        for (const auto& [k, v] : d) {
            Str(k);
            U32((uint32_t)v.size() + 1);
            Arbitrary(v.c_str(), (uint32_t)v.size() + 1);
        }
        Null();
    }
    void Timeval(uint32_t sec, uint32_t usec) {
        bytes.push_back('T');
        Be32(sec);
        Be32(usec);
    }
    void FormatInfo(uint8_t encoding, const Dict& props) {
        bytes.push_back('f');
        U8(encoding);
        Proplist(props);
    }

private:
    void Be32(uint32_t v) {
        uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
        bytes.insert(bytes.end(), b, b + 4);
    }
};

uint32_t LoadBe32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

// Reads tagstruct values; a wrong tag or a value past the end fails it (and every read after).
class TagReader {
public:
    TagReader(const uint8_t* p, size_t n) : p_(p), end_(p + n) {}
    bool Ok() const { return ok_; }
    bool AtEnd() const { return p_ >= end_; }

    uint32_t U32() { return Tag('L') && Need(4) ? Take32() : 0; }
    uint8_t U8() { return Tag('B') && Need(1) ? *p_++ : 0; }
    bool Bool() {
        if (!ok_ || p_ >= end_) return Fail();
        uint8_t t = *p_++;
        if (t == '1') return true;
        if (t != '0') Fail();
        return false;
    }
    uint64_t U64(char tag = 'R') {
        if (!Tag(tag) || !Need(8)) return 0;
        uint64_t hi = Take32();
        return hi << 32 | Take32();
    }
    int64_t S64() { return (int64_t)U64('r'); }
    uint64_t Usec() { return U64('U'); }
    void Timeval() {
        if (Tag('T') && Need(8)) p_ += 8;
    }
    // A string, or false for a null string.
    bool Str(std::string& out) {
        out.clear();
        if (!ok_ || p_ >= end_) return Fail();
        uint8_t t = *p_++;
        if (t == 'N') return false;
        if (t != 't') return Fail();
        const uint8_t* nul = static_cast<const uint8_t*>(memchr(p_, 0, (size_t)(end_ - p_)));
        if (!nul) return Fail();
        out.assign(reinterpret_cast<const char*>(p_), (size_t)(nul - p_));
        p_ = nul + 1;
        return true;
    }
    std::string Str() {
        std::string s;
        Str(s);
        return s;
    }
    void SampleSpec(uint8_t& format, uint8_t& channels, uint32_t& rate) {
        format = channels = 0;
        rate = 0;
        if (!Tag('a') || !Need(6)) return;
        format = *p_++;
        channels = *p_++;
        rate = Take32();
    }
    void ChannelMap() {
        if (!Tag('m') || !Need(1)) return;
        uint8_t n = *p_++;
        if (Need(n)) p_ += n;
    }
    void CVolume() {
        if (!Tag('v') || !Need(1)) return;
        uint8_t n = *p_++;
        if (Need((size_t)n * 4)) p_ += (size_t)n * 4;
    }
    void Volume() {
        if (Tag('V') && Need(4)) p_ += 4;
    }
    void Proplist(Dict* out) {
        if (!Tag('P')) return;
        for (int guard = 0; ok_ && guard < 4096; ++guard) {
            if (p_ >= end_) {
                Fail();
                return;
            }
            if (*p_ == 'N') {
                ++p_;
                return;
            }
            std::string key;
            if (!Str(key)) {
                Fail();
                return;
            }
            uint32_t n = U32();
            if (!Tag('x') || !Need(4)) return;
            uint32_t m = Take32();
            if (m != n || !Need(m)) {
                Fail();
                return;
            }
            if (out) {
                std::string v(reinterpret_cast<const char*>(p_), m);
                if (!v.empty() && v.back() == 0) v.pop_back();
                out->emplace_back(std::move(key), std::move(v));
            }
            p_ += m;
        }
        Fail();
    }
    void FormatInfo() {
        if (!Tag('f')) return;
        U8();
        Proplist(nullptr);
    }

private:
    bool Fail() {
        ok_ = false;
        return false;
    }
    bool Need(size_t n) {
        if (ok_ && (size_t)(end_ - p_) >= n) return true;
        return Fail();
    }
    bool Tag(uint8_t t) {
        if (!Need(1)) return false;
        if (*p_ != t) return Fail();
        ++p_;
        return true;
    }
    uint32_t Take32() {
        uint32_t v = LoadBe32(p_);
        p_ += 4;
        return v;
    }
    const uint8_t* p_;
    const uint8_t* end_;
    bool ok_ = true;
};

const std::string* Lookup(const Dict& d, const char* key) {
    for (const auto& [k, v] : d)
        if (k == key) return &v;
    return nullptr;
}

// One GET_SINK_INFO_LIST entry, as the reply of `version` lays it out.
bool ParseSinkInfo(TagReader& t, uint32_t version, sound::Sink& sink) {
    t.U32();  // index
    t.Str(sink.name);
    std::string description;
    t.Str(description);
    sink.description = description.empty() ? sink.name : description;
    uint8_t format, channels;
    uint32_t rate;
    t.SampleSpec(format, channels, rate);
    t.ChannelMap();
    t.U32();  // owner module
    t.CVolume();
    t.Bool();  // mute
    t.U32();   // monitor source
    t.Str();
    t.Usec();
    t.Str();   // driver
    t.U32();   // flags
    if (version >= 13) {
        Dict props;
        t.Proplist(&props);
        if (const std::string* card = Lookup(props, "alsa.card")) sink.alsaCard = *card;
        else if (const std::string* card2 = Lookup(props, "api.alsa.card")) sink.alsaCard = *card2;
        t.Usec();  // configured latency
    }
    if (version >= 15) {
        t.Volume();
        t.U32();  // state
        t.U32();  // volume steps
        t.U32();  // card
    }
    if (version >= 16) {
        uint32_t ports = t.U32();
        if (ports > 1024) return false;
        for (uint32_t i = 0; i < ports && t.Ok(); ++i) {
            t.Str();
            t.Str();
            t.U32();
            if (version >= 24) t.U32();
            if (version >= 34) {
                t.Str();
                t.U32();
            }
        }
        t.Str();  // active port
    }
    if (version >= 21) {
        uint8_t formats = t.U8();
        for (uint8_t i = 0; i < formats && t.Ok(); ++i) t.FormatInfo();
    }
    return t.Ok();
}

struct ServerInfo {
    std::string defaultSink;
    uint32_t rate = 0;
};

bool ParseServerInfo(TagReader& t, uint32_t version, ServerInfo& info) {
    t.Str();  // server name
    t.Str();  // version
    t.Str();  // user
    t.Str();  // host
    uint8_t format, channels;
    t.SampleSpec(format, channels, info.rate);
    t.Str(info.defaultSink);
    t.Str();  // default source
    t.U32();  // cookie
    if (version >= 15) t.ChannelMap();
    return t.Ok();
}

struct CreateReply {
    uint32_t channel = kInvalid, index = kInvalid, requested = 0;
    uint32_t rate = 0;
    std::string sink;
};

bool ParseCreateReply(TagReader& t, uint32_t version, CreateReply& r) {
    r.channel = t.U32();
    r.index = t.U32();
    r.requested = t.U32();
    if (version >= 9) {
        t.U32();  // maxlength
        t.U32();  // tlength
        t.U32();  // prebuf
        t.U32();  // minreq
    }
    if (version >= 12) {
        uint8_t format, channels;
        t.SampleSpec(format, channels, r.rate);
        t.ChannelMap();
        t.U32();  // sink index
        t.Str(r.sink);
        t.Bool();  // suspended
    }
    if (version >= 13) t.Usec();
    if (version >= 21) t.FormatInfo();
    return t.Ok() && r.channel != kInvalid;
}

struct Latency {
    uint64_t sinkUsec = 0;
    int64_t writeIndex = 0, readIndex = 0;
    bool playing = false;
};

bool ParseLatency(TagReader& t, uint32_t version, Latency& l) {
    l.sinkUsec = t.Usec();
    t.Usec();  // source
    l.playing = t.Bool();
    t.Timeval();
    t.Timeval();
    l.writeIndex = t.S64();
    l.readIndex = t.S64();
    if (version >= 13) {
        t.U64();
        t.U64();
    }
    return t.Ok();
}

// ---- The connection

std::string HomeFile(const char* rel) {
    const char* home = getenv("HOME");
    if (!home || !*home) {
        passwd* pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : nullptr;
    }
    return home ? std::string(home) + rel : std::string();
}

// The cookie PulseAudio checks, or zeros (accepted by servers that trust the socket's peer).
std::vector<uint8_t> Cookie() {
    std::vector<std::string> files;
    if (const char* c = getenv("PULSE_COOKIE")) files.push_back(c);
    if (const char* x = getenv("XDG_CONFIG_HOME"); x && *x) files.push_back(std::string(x) + "/pulse/cookie");
    files.push_back(HomeFile("/.config/pulse/cookie"));
    files.push_back(HomeFile("/.pulse-cookie"));
    std::vector<uint8_t> cookie(256, 0);
    for (const std::string& f : files) {
        if (f.empty()) continue;
        int fd = open(f.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        ssize_t n = read(fd, cookie.data(), cookie.size());
        close(fd);
        if (n == (ssize_t)cookie.size()) return cookie;
        std::fill(cookie.begin(), cookie.end(), 0);
    }
    return cookie;
}

struct Packet {
    uint32_t channel = 0;
    std::vector<uint8_t> payload;
};

class Connection {
public:
    ~Connection() { Close(); }
    int Fd() const { return fd_; }
    uint32_t version = 0;  // negotiated
    std::string error;

    void Close() {
        if (fd_ >= 0) close(fd_);
        fd_ = -1;
    }

    bool Connect() {
        std::string path = SocketPath();
        if (path.empty()) {
            error = "PulseAudio: no XDG_RUNTIME_DIR";
            return false;
        }
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (path.size() >= sizeof(addr.sun_path)) return false;
        memcpy(addr.sun_path, path.c_str(), path.size() + 1);
        fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd_ < 0 || connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            error = "PulseAudio isn't running (" + path + ": " + strerror(errno) + ")";
            Close();
            return false;
        }
        return true;
    }

    // Sends a command; returns its tag.
    uint32_t Command(uint32_t command, const Tags& args, bool credentials = false) {
        Tags t;
        t.U32(command);
        uint32_t tag = nextTag_++;
        t.U32(tag);
        t.bytes.insert(t.bytes.end(), args.bytes.begin(), args.bytes.end());
        Send(kInvalid, t.bytes.data(), t.bytes.size(), credentials);
        return tag;
    }

    bool Send(uint32_t channel, const void* data, size_t size, bool credentials = false) {
        if (fd_ < 0) return false;
        uint8_t desc[20] = {};
        auto be = [&](int at, uint32_t v) {
            desc[at] = (uint8_t)(v >> 24);
            desc[at + 1] = (uint8_t)(v >> 16);
            desc[at + 2] = (uint8_t)(v >> 8);
            desc[at + 3] = (uint8_t)v;
        };
        be(0, (uint32_t)size);
        be(4, channel);
        iovec iov[2] = {{desc, sizeof(desc)}, {const_cast<void*>(data), size}};
        msghdr msg{};
        msg.msg_iov = iov;
        msg.msg_iovlen = 2;
        alignas(cmsghdr) char control[CMSG_SPACE(sizeof(ucred))];
        if (credentials) {
            msg.msg_control = control;
            msg.msg_controllen = sizeof(control);
            cmsghdr* c = CMSG_FIRSTHDR(&msg);
            c->cmsg_level = SOL_SOCKET;
            c->cmsg_type = SCM_CREDENTIALS;
            c->cmsg_len = CMSG_LEN(sizeof(ucred));
            ucred cred{getpid(), getuid(), getgid()};
            memcpy(CMSG_DATA(c), &cred, sizeof(cred));
        }
        size_t total = sizeof(desc) + size, sent = 0;
        while (sent < total) {
            ssize_t n = sendmsg(fd_, &msg, MSG_NOSIGNAL);
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN) {
                    pollfd p{fd_, POLLOUT, 0};
                    poll(&p, 1, 1000);
                    continue;
                }
                return false;
            }
            sent += (size_t)n;
            msg.msg_control = nullptr;
            msg.msg_controllen = 0;
            size_t skip = (size_t)n;
            while (msg.msg_iovlen && skip >= msg.msg_iov[0].iov_len) {
                skip -= msg.msg_iov[0].iov_len;
                ++msg.msg_iov;
                --msg.msg_iovlen;
            }
            if (msg.msg_iovlen) {
                msg.msg_iov[0].iov_base = static_cast<uint8_t*>(msg.msg_iov[0].iov_base) + skip;
                msg.msg_iov[0].iov_len -= skip;
            }
        }
        return true;
    }

    // Reads what has arrived; `handle` gets each complete packet. False on hang-up or garbage.
    template <typename F> bool Receive(F&& handle) {
        uint8_t buf[65536];
        for (;;) {
            ssize_t n = recv(fd_, buf, sizeof(buf), MSG_DONTWAIT);
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN) break;
                return false;
            }
            if (n == 0) return false;
            in_.insert(in_.end(), buf, buf + n);
            if ((size_t)n < sizeof(buf)) break;
        }
        size_t at = 0;
        while (in_.size() - at >= 20) {
            uint32_t length = LoadBe32(&in_[at]), channel = LoadBe32(&in_[at + 4]);
            if (length > kMaxPacket) return false;
            if (in_.size() - at - 20 < length) break;
            Packet p;
            p.channel = channel;
            p.payload.assign(in_.begin() + (std::ptrdiff_t)at + 20, in_.begin() + (std::ptrdiff_t)(at + 20 + length));
            handle(p);
            at += 20 + length;
        }
        in_.erase(in_.begin(), in_.begin() + (std::ptrdiff_t)at);
        return true;
    }

    // Waits for the reply to `tag` (other packets go to `other`). Returns its tagstruct after the
    // command and tag, or nothing on an error reply, a hang-up or the deadline.
    template <typename F>
    bool WaitReply(uint32_t tag, std::vector<uint8_t>& reply, std::chrono::steady_clock::time_point deadline, F&& other) {
        bool got = false, ok = false;
        while (!got) {
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0) {
                error = "PulseAudio didn't answer";
                return false;
            }
            pollfd p{fd_, POLLIN, 0};
            if (poll(&p, 1, (int)left.count()) <= 0) continue;
            bool alive = Receive([&](Packet& packet) {
                if (got || packet.channel != kInvalid) {
                    other(packet);
                    return;
                }
                TagReader t(packet.payload.data(), packet.payload.size());
                uint32_t command = t.U32(), ptag = t.U32();
                if (!t.Ok() || ptag != tag || (command != kReply && command != kError)) {
                    other(packet);
                    return;
                }
                got = true;
                if (command == kError) {
                    error = "PulseAudio refused (error " + std::to_string(t.U32()) + ")";
                    return;
                }
                ok = true;
                reply.assign(packet.payload.begin() + 10, packet.payload.end());  // 'L' u32, 'L' u32
            });
            if (!alive) {
                error = "PulseAudio went away";
                return false;
            }
        }
        return ok;
    }

    // Authenticates and names the client.
    bool Hello(std::chrono::steady_clock::time_point deadline) {
        Tags auth;
        auth.U32(kVersion);
        std::vector<uint8_t> cookie = Cookie();
        auth.Arbitrary(cookie.data(), (uint32_t)cookie.size());
        std::vector<uint8_t> reply;
        if (!WaitReply(Command(kAuth, auth, true), reply, deadline, [](Packet&) {})) return false;
        TagReader t(reply.data(), reply.size());
        uint32_t server = t.U32() & 0xffff;
        if (!t.Ok() || server < 13) {
            error = "PulseAudio's protocol is too old";
            return false;
        }
        version = std::min(server, kVersion);
        char pid[32];
        snprintf(pid, sizeof(pid), "%d", (int)getpid());
        Tags name;
        name.Proplist({{"application.name", "cordinal"}, {"application.process.id", pid},
                       {"application.process.binary", "cordinal"}});
        return WaitReply(Command(kSetClientName, name), reply, deadline, [](Packet&) {});
    }

private:
    int fd_ = -1;
    uint32_t nextTag_ = 0;
    std::vector<uint8_t> in_;
};

// ---- The playback stream

class PulseStream final : public sound::Stream {
public:
    PulseStream(const sound::Params& p, sound::Callbacks cb, std::function<void()> onFailure)
        : params_(p), cb_(std::move(cb)), onFailure_(std::move(onFailure)) {
        active_ = p.active;
        wakeFd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        scratch_.resize(kChunkFrames * 2);
    }
    ~PulseStream() override {
        quit_ = true;
        Wake();
        if (thread_.joinable()) thread_.join();
        if (wakeFd_ >= 0) close(wakeFd_);
    }

    bool Start(std::string& error) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        if (!conn_.Connect() || !conn_.Hello(deadline) || !Create(deadline)) {
            error = conn_.error;
            return false;
        }
        thread_ = std::thread([this] { Run(); });
        return true;
    }

    void SetActive(bool active) override {
        active_ = active;
        activeDirty_ = true;
        Wake();
    }
    void Flush() override {
        flush_ = true;
        Wake();
    }
    bool Failed() const override { return failed_; }
    const char* Backend() const override { return "PulseAudio"; }

private:
    static constexpr uint32_t kChunkFrames = 4096;
    static constexpr size_t kFrameBytes = 8;

    void Wake() {
        uint64_t one = 1;
        if (wakeFd_ >= 0) (void)!write(wakeFd_, &one, sizeof(one));
    }

    bool Create(std::chrono::steady_clock::time_point deadline) {
        const uint32_t v = conn_.version;
        const uint32_t period = params_.latencyFrames * (uint32_t)kFrameBytes;
        Tags t;
        t.SampleSpec(kSampleFloat32LE, 2, params_.rate);
        t.ChannelMap({1, 2});  // front left, front right
        t.U32(kInvalid);       // sink index
        if (params_.target.empty()) t.Null();
        else t.Str(params_.target);
        t.U32(kInvalid);  // maxlength: the server's
        t.Bool(!active_);  // corked
        // tlength: two periods queued, but at least 60 ms. The server asks for data from its own
        // thread, often without real-time priority; with less queued a busy moment (a server
        // that has just started, say) underruns: tens of milliseconds of silence in the music.
        const uint32_t floor = params_.rate * 60 / 1000 * (uint32_t)kFrameBytes;
        t.U32(std::max(period * 2, floor));
        t.U32(kInvalid);    // prebuf: the server's (a full tlength)
        t.U32(period / 2);  // minreq
        t.U32(0);           // sync id
        t.CVolume(2, kVolumeNorm);
        for (int i = 0; i < 7; ++i) t.Bool(false);  // no remap, no remix, fix format/rate/channels, don't move, variable rate
        t.Bool(false);  // start muted
        t.Bool(true);   // adjust latency: the sink runs at about tlength
        Dict props = {{"media.name", params_.name}, {"application.name", "cordinal"},
                      {"media.role", params_.role == "Music" ? "music" : "event"}};
        t.Proplist(props);
        if (v >= 14) {
            t.Bool(false);  // volume set
            t.Bool(false);  // early requests
        }
        if (v >= 15) {
            t.Bool(false);  // muted set
            t.Bool(false);  // don't inhibit auto suspend
            t.Bool(false);  // fail on suspend
        }
        if (v >= 17) t.Bool(false);  // relative volume
        if (v >= 18) t.Bool(false);  // passthrough
        if (v >= 21) t.U8(0);         // no format list: the sample spec says it
        std::vector<uint8_t> reply;
        if (!conn_.WaitReply(conn_.Command(kCreatePlaybackStream, t), reply, deadline, [&](Packet& p) { OnPacket(p); }))
            return false;
        TagReader r(reply.data(), reply.size());
        CreateReply c;
        if (!ParseCreateReply(r, v, c)) {
            conn_.error = "PulseAudio: unreadable stream reply";
            return false;
        }
        channel_ = c.channel;
        requested_ += c.requested;
        serverRate_ = c.rate ? c.rate : params_.rate;
        activeSent_ = active_;
        return true;
    }

    void OnPacket(Packet& p) {
        if (p.channel != kInvalid) return;  // no record streams here
        TagReader t(p.payload.data(), p.payload.size());
        uint32_t command = t.U32(), tag = t.U32();
        if (!t.Ok()) return;
        if (command == kRequest) {
            uint32_t channel = t.U32(), bytes = t.U32();
            if (t.Ok() && channel == channel_) requested_ += bytes;
        } else if (command == kPlaybackStreamKilled) {
            killed_ = true;
        } else if ((command == kReply || command == kError) && tag == latencyTag_) {
            latencyTag_ = kInvalid;
            Latency l;
            if (command == kReply && ParseLatency(t, conn_.version, l)) {
                int64_t sinkFrames = (int64_t)(l.sinkUsec * params_.rate / 1000000);
                int64_t played = l.readIndex / (int64_t)kFrameBytes - sinkFrames;
                played_ = (uint64_t)std::max<int64_t>(0, std::min<int64_t>(played, (int64_t)written_));
            }
        }
    }

    void Run() {
        auto lastLatency = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        while (!quit_) {
            pollfd fds[2] = {{conn_.Fd(), POLLIN, 0}, {wakeFd_, POLLIN, 0}};
            int n = poll(fds, 2, 20);
            if (n < 0 && errno != EINTR) break;
            if (quit_) break;
            if (fds[1].revents & POLLIN) {
                uint64_t v;
                (void)!read(wakeFd_, &v, sizeof(v));
            }
            if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
                if (!conn_.Receive([&](Packet& p) { OnPacket(p); }) || killed_) {
                    Fail();
                    return;
                }
            }
            if (activeDirty_.exchange(false) && activeSent_ != active_) {
                activeSent_ = active_;
                Tags t;
                t.U32(channel_);
                t.Bool(!activeSent_);
                conn_.Command(kCorkPlaybackStream, t);
            }
            if (flush_.exchange(false)) {
                Tags t;
                t.U32(channel_);
                conn_.Command(kFlushPlaybackStream, t);
            }
            auto now = std::chrono::steady_clock::now();
            if (latencyTag_ == kInvalid && now - lastLatency > std::chrono::milliseconds(20)) {
                lastLatency = now;
                timeval tv;
                gettimeofday(&tv, nullptr);
                Tags t;
                t.U32(channel_);
                t.Timeval((uint32_t)tv.tv_sec, (uint32_t)tv.tv_usec);
                latencyTag_ = conn_.Command(kGetPlaybackLatency, t);
            }
            // Feed what the server asked for.
            while (activeSent_ && requested_ >= kFrameBytes) {
                uint32_t frames = (uint32_t)std::min<uint64_t>(requested_ / kFrameBytes, kChunkFrames);
                if (cb_.timing) cb_.timing(played_, serverRate_);
                if (cb_.render) cb_.render(scratch_.data(), frames);
                else std::fill(scratch_.begin(), scratch_.begin() + frames * 2, 0.0f);
                if (!conn_.Send(channel_, scratch_.data(), (size_t)frames * kFrameBytes)) {
                    Fail();
                    return;
                }
                written_ += frames;
                requested_ -= (uint64_t)frames * kFrameBytes;
            }
        }
    }

    void Fail() {
        if (!failed_.exchange(true) && onFailure_) onFailure_();
    }

    sound::Params params_;
    sound::Callbacks cb_;
    std::function<void()> onFailure_;
    Connection conn_;
    std::thread thread_;
    int wakeFd_ = -1;
    std::atomic<bool> quit_{false}, failed_{false}, active_{true}, activeDirty_{false}, flush_{false};
    bool activeSent_ = true, killed_ = false;
    uint32_t channel_ = kInvalid, latencyTag_ = kInvalid, serverRate_ = 0;
    uint64_t requested_ = 0, written_ = 0, played_ = 0;
    std::vector<float> scratch_;
};

}  // namespace

std::string SocketPath() {
    if (const char* server = getenv("PULSE_SERVER"); server && *server) {
        // A list of servers: the first local one.
        std::string list = server;
        size_t at = 0;
        while (at < list.size()) {
            size_t end = list.find(' ', at);
            std::string s = list.substr(at, end == std::string::npos ? std::string::npos : end - at);
            at = end == std::string::npos ? list.size() : end + 1;
            if (s.rfind("unix:", 0) == 0) return s.substr(5);
            if (!s.empty() && s[0] == '/') return s;
        }
        return {};
    }
    if (const char* rt = getenv("PULSE_RUNTIME_PATH"); rt && *rt) return std::string(rt) + "/native";
    const char* dir = getenv("XDG_RUNTIME_DIR");
    if (!dir || !*dir) return {};
    return std::string(dir) + "/pulse/native";
}

std::unique_ptr<sound::Stream> OpenStream(const sound::Params& params, sound::Callbacks callbacks,
                                          std::function<void()> onFailure, std::string& error) {
    auto s = std::make_unique<PulseStream>(params, std::move(callbacks), std::move(onFailure));
    if (!s->Start(error)) return nullptr;
    return s;
}

bool ListSinks(std::vector<sound::Sink>& out) {
    out.clear();
    Connection c;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    if (!c.Connect() || !c.Hello(deadline)) return false;
    std::vector<uint8_t> reply;
    ServerInfo info;
    if (c.WaitReply(c.Command(kGetServerInfo, Tags()), reply, deadline, [](Packet&) {})) {
        TagReader t(reply.data(), reply.size());
        ParseServerInfo(t, c.version, info);
    }
    if (!c.WaitReply(c.Command(kGetSinkInfoList, Tags()), reply, deadline, [](Packet&) {})) return false;
    TagReader t(reply.data(), reply.size());
    while (!t.AtEnd()) {
        sound::Sink s;
        if (!ParseSinkInfo(t, c.version, s)) break;
        s.systemDefault = s.name == info.defaultSink;
        out.push_back(std::move(s));
    }
    return true;
}

std::vector<uint8_t> FuzzSeed(int which) {
    Tags t;
    if (which == 0) {
        for (int i = 0; i < 2; ++i) {
            t.U32((uint32_t)i);
            t.Str(i ? "test-sink-b" : "test-sink-a");
            t.Str("Test Sink");
            t.SampleSpec(kSampleFloat32LE, 2, 48000);
            t.ChannelMap({1, 2});
            t.U32(3);
            t.CVolume(2, kVolumeNorm);
            t.Bool(false);
            t.U32(4);
            t.Str("monitor");
            t.bytes.push_back('U');
            for (int k = 0; k < 8; ++k) t.bytes.push_back(0);
            t.Str("driver");
            t.U32(0);
            t.Proplist({{"alsa.card", "1"}, {"device.description", "x"}});
            t.bytes.push_back('U');
            for (int k = 0; k < 8; ++k) t.bytes.push_back(0);
            t.bytes.push_back('V');
            t.bytes.insert(t.bytes.end(), {0, 1, 0, 0});
            t.U32(0);
            t.U32(65537);
            t.U32(0);
            t.U32(1);
            t.Str("analog-output");
            t.Str("Speakers");
            t.U32(100);
            t.U32(2);
            t.Str("analog-output");
            t.U8(1);
            t.FormatInfo(1, {{"format.rate", "48000"}});
        }
    } else if (which == 1) {
        t.U32(0);
        t.U32(12);
        t.U32(15360);
        for (int i = 0; i < 4; ++i) t.U32(7680);
        t.SampleSpec(kSampleFloat32LE, 2, 48000);
        t.ChannelMap({1, 2});
        t.U32(1);
        t.Str("test-sink-a");
        t.Bool(false);
        t.bytes.push_back('U');
        for (int k = 0; k < 8; ++k) t.bytes.push_back(0);
        t.FormatInfo(1, {});
    } else {
        t.Str("pulseaudio");
        t.Str("17.0");
        t.Str("user");
        t.Str("host");
        t.SampleSpec(kSampleFloat32LE, 2, 48000);
        t.Str("test-sink-a");
        t.Null();
        t.U32(1);
        t.ChannelMap({1, 2});
    }
    return t.bytes;
}

int FuzzParse(const uint8_t* data, size_t size) {
    int ok = 0;
    for (uint32_t version : {13u, 32u, 35u}) {
        TagReader a(data, size);
        sound::Sink s;
        while (!a.AtEnd() && ParseSinkInfo(a, version, s)) ++ok;
        TagReader b(data, size);
        CreateReply c;
        ok += ParseCreateReply(b, version, c);
        TagReader d(data, size);
        ServerInfo si;
        ok += ParseServerInfo(d, version, si);
        TagReader e(data, size);
        Latency l;
        ok += ParseLatency(e, version, l);
    }
    return ok;
}

}  // namespace lx::pulse
