// The session-bus client declared in dbus.h: the wire format (D-Bus specification, little-endian out,
// either byte order in), the Unix-socket connection with SASL EXTERNAL, and object-table dispatch.
#include "dbus.h"

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstring>

namespace lx::dbus {

namespace {

int64_t Now() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

size_t AlignOf(char c) {
    switch (c) {
        case 'y': case 'g': case 'v': return 1;
        case 'n': case 'q': return 2;
        case 'x': case 't': case 'd': case '(': case '{': return 8;
        default: return 4;  // b i u s o a
    }
}

bool Basic(char c) { return c && strchr("ybnqiuxtdsog", c); }

// Length of one valid UTF-8 sequence at s[i], or 0.
size_t Utf8Length(std::string_view s, size_t i) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) return 1;
    size_t n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC2 ? 2 : 0;
    if (!n || c > 0xF4 || i + n > s.size()) return 0;
    for (size_t k = 1; k < n; ++k)
        if (((unsigned char)s[i + k] & 0xC0) != 0x80) return 0;
    unsigned char d = (unsigned char)s[i + 1];
    if ((c == 0xE0 && d < 0xA0) || (c == 0xED && d > 0x9F) || (c == 0xF0 && d < 0x90) || (c == 0xF4 && d > 0x8F))
        return 0;  // overlong, surrogate or past U+10FFFF
    return n;
}

constexpr const char* kErr = "org.freedesktop.DBus.Error.";

}  // namespace

// --- Writer ---------------------------------------------------------------------------------------------
void Writer::S(std::string_view v) {
    v = v.substr(0, std::min(v.find('\0'), v.size()));
    std::string clean;
    clean.reserve(v.size());
    for (size_t i = 0; i < v.size();) {
        size_t n = Utf8Length(v, i);
        if (n) clean.append(v.substr(i, n)), i += n;
        else clean += "\xEF\xBF\xBD", ++i;
    }
    U((uint32_t)clean.size());
    b += clean;
    b.push_back('\0');
}

void Writer::G(std::string_view v) {
    Y((uint8_t)v.size());
    b.append(v);
    b.push_back('\0');
}

Writer::Array Writer::Open(size_t elementAlign) {
    U(0);
    Array a{b.size() - 4, 0};
    Align(elementAlign);
    a.start = b.size();
    return a;
}

void Writer::Close(Array a) {
    uint32_t n = (uint32_t)(b.size() - a.start);
    memcpy(&b[a.length], &n, 4);
}

void Writer::Strings(std::initializer_list<std::string_view> list) {
    Array a = Open(4);
    for (std::string_view s : list) S(s);
    Close(a);
}

// --- Reader ---------------------------------------------------------------------------------------------
bool Reader::Align(size_t n) {
    size_t to = (p_ + n - 1) / n * n;
    if (!ok_ || to > d_.size()) return Fail();
    for (; p_ < to; ++p_)
        if (d_[p_]) return Fail();
    return true;
}

template <class T> T Reader::Fixed() {
    if (!Align(sizeof(T)) || d_.size() - p_ < sizeof(T)) return Fail(), T(0);
    T v;
    memcpy(&v, d_.data() + p_, sizeof(T));
    p_ += sizeof(T);
    if (swap_) {
        if constexpr (sizeof(T) == 2) v = __builtin_bswap16(v);
        if constexpr (sizeof(T) == 4) v = __builtin_bswap32(v);
        if constexpr (sizeof(T) == 8) v = __builtin_bswap64(v);
    }
    return v;
}
template uint32_t Reader::Fixed<uint32_t>();  // I, U and X are inline in dbus.h
template uint64_t Reader::Fixed<uint64_t>();

uint8_t Reader::Y() {
    if (!ok_ || p_ >= d_.size()) return Fail(), 0;
    return (uint8_t)d_[p_++];
}

bool Reader::B() {
    uint32_t v = U();
    if (v > 1) Fail();
    return ok_ && v == 1;
}

double Reader::D() {
    uint64_t bits = Fixed<uint64_t>();
    double v;
    memcpy(&v, &bits, sizeof(v));
    return v;
}

std::string_view Reader::S() {
    uint32_t n = U();
    if (!ok_ || n >= d_.size() - p_ || d_[p_ + n] || memchr(d_.data() + p_, 0, n)) return Fail(), std::string_view();
    std::string_view s = d_.substr(p_, n);
    p_ += n + 1;
    return s;
}

std::string_view Reader::O() {
    std::string_view s = S();
    if (ok_ && !ValidPath(s)) Fail();
    return ok_ ? s : std::string_view();
}

std::string_view Reader::G() {
    size_t n = Y();
    if (!ok_ || n >= d_.size() - p_ || d_[p_ + n]) return Fail(), std::string_view();
    std::string_view s = d_.substr(p_, n);
    p_ += n + 1;
    if (!ValidSignature(s)) return Fail(), std::string_view();
    return s;
}

size_t Reader::Open(size_t elementAlign) {
    uint32_t n = U();
    if (n > (1u << 26)) Fail();  // the specification's array limit
    Align(elementAlign);
    if (!ok_ || n > d_.size() - p_) return Fail(), p_;
    return p_ + n;
}

bool Reader::Skip(std::string_view sig) {
    while (ok_ && !sig.empty())
        if (!Walk(sig, 0)) return false;
    return ok_;
}

// Consumes one complete type from `sig` and its value from the data. Depth is bounded by TypeLength's
// limit on the signature and by the variant check here, so recursion cannot run away.
bool Reader::Walk(std::string_view& sig, int depth) {
    if (depth > 64 || sig.empty()) return Fail();
    char c = sig[0];
    switch (c) {
        case 'y': Y(); break;
        case 'b': B(); break;
        case 'n': case 'q': Fixed<uint16_t>(); break;
        case 'i': case 'u': Fixed<uint32_t>(); break;
        case 'x': case 't': case 'd': Fixed<uint64_t>(); break;
        case 's': S(); break;
        case 'o': O(); break;
        case 'g': G(); break;
        case 'v': {
            std::string_view inner = G();
            if (!ok_ || inner.empty() || TypeLength(inner) != inner.size()) return Fail();
            if (!Walk(inner, depth + 1)) return false;
            break;
        }
        case 'a': {
            size_t n = TypeLength(sig, 0, depth);  // the whole array type, so a{..} keeps its braces
            if (n < 2) return Fail();
            --n;
            std::string_view element = sig.substr(1, n);
            size_t end = Open(AlignOf(element[0]));
            while (In(end)) {
                std::string_view e = element;
                if (!Walk(e, depth + 1)) return false;
            }
            if (!ok_ || p_ != end) return Fail();
            sig.remove_prefix(1 + n);
            return true;
        }
        case '(': case '{': {
            char close = c == '(' ? ')' : '}';
            Struct();
            sig.remove_prefix(1);
            while (ok_ && !sig.empty() && sig[0] != close)
                if (!Walk(sig, depth + 1)) return false;
            if (sig.empty()) return Fail();
            break;
        }
        default: return Fail();  // 'h' among them: this client takes no file descriptors
    }
    sig.remove_prefix(1);
    return ok_;
}

size_t TypeLength(std::string_view s, size_t i, int depth) {
    if (i >= s.size() || depth > 64) return 0;
    char c = s[i];
    if (Basic(c) || c == 'v') return 1;
    if (c == 'a') {
        if (i + 1 < s.size() && s[i + 1] == '{') {  // dict entry: a basic key and one value
            size_t j = i + 2;
            if (j >= s.size() || !Basic(s[j])) return 0;
            size_t v = TypeLength(s, j + 1, depth + 1);
            if (!v || j + 1 + v >= s.size() || s[j + 1 + v] != '}') return 0;
            return j + 2 + v - i;
        }
        size_t e = TypeLength(s, i + 1, depth + 1);
        return e ? e + 1 : 0;
    }
    if (c == '(') {
        size_t j = i + 1;
        if (j < s.size() && s[j] == ')') return 0;
        while (j < s.size() && s[j] != ')') {
            size_t e = TypeLength(s, j, depth + 1);
            if (!e) return 0;
            j += e;
        }
        return j < s.size() ? j + 1 - i : 0;
    }
    return 0;
}

bool ValidSignature(std::string_view sig) {
    if (sig.size() > 255) return false;
    for (size_t i = 0; i < sig.size();) {
        size_t n = TypeLength(sig, i);
        if (!n) return false;
        i += n;
    }
    return true;
}

bool ValidPath(std::string_view p) {
    if (p.empty() || p[0] != '/' || p.size() > 4096) return false;
    if (p.size() == 1) return true;
    if (p.back() == '/') return false;
    for (size_t i = 1; i < p.size(); ++i) {
        char c = p[i];
        if (c == '/') {
            if (p[i - 1] == '/') return false;
        } else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return true;
}

// --- messages -------------------------------------------------------------------------------------------
int ParseMessage(std::string_view buf, Message& out, size_t& used) {
    if (buf.size() < 16) return 0;
    if (buf[0] != 'l' && buf[0] != 'B') return -1;
    bool swap = buf[0] == 'B';  // the published player runs on little-endian hosts
    Reader fixed(buf.substr(0, 16), swap, 4);
    uint32_t bodyLength = fixed.U(), serial = fixed.U(), fields = fixed.U();
    uint8_t type = (uint8_t)buf[1];
    if (type < kCall || type > kSignal || buf[3] != 1 || !serial || fields > kMaxHeader || bodyLength > kMaxMessage)
        return -1;
    size_t start = (16 + (size_t)fields + 7) / 8 * 8;
    if (start + bodyLength > kMaxMessage) return -1;
    if (buf.size() < start + bodyLength) return 0;

    out = Message();  // a field the message lacks must not keep a value from an earlier one
    out.type = type;
    out.flags = (uint8_t)buf[2];
    out.swap = swap;
    out.serial = serial;
    Reader h(buf.substr(0, start), swap, 12);
    size_t end = h.Open(8);
    // path, interface, member, error name, reply serial, destination, sender, signature, unix fds
    static constexpr char kFieldType[] = "\0osssussgu";
    while (h.In(end)) {
        h.Struct();
        uint8_t code = h.Y();
        std::string_view sig = h.G();
        if (!h.ok() || !code) return -1;
        if (code <= 9 && (sig.size() != 1 || sig[0] != kFieldType[code])) return -1;
        switch (code) {
            case 1: out.path = h.O(); break;
            case 2: out.iface = h.S(); break;
            case 3: out.member = h.S(); break;
            case 4: out.error = h.S(); break;
            case 5: out.replySerial = h.U(); break;
            case 6: out.dest = h.S(); break;
            case 7: out.sender = h.S(); break;
            case 8: out.sig = h.G(); break;
            case 9:  // GDBus states a count of 0; this client never negotiated fd passing
                if (h.U() != 0) return -1;
                break;
            default:            // unknown fields are ignored, but still have to be well formed
                if (sig.empty() || TypeLength(sig) != sig.size() || !h.Skip(sig)) return -1;
        }
    }
    if (!h.ok() || h.pos() != end || !h.Align(8) || h.pos() != start) return -1;
    for (const std::string* name : {&out.iface, &out.member, &out.error, &out.dest, &out.sender})
        if (name->size() > 255) return -1;
    bool complete = type == kCall     ? !out.path.empty() && !out.member.empty()
                    : type == kSignal ? !out.path.empty() && !out.iface.empty() && !out.member.empty()
                    : type == kError  ? out.replySerial && !out.error.empty()
                                      : out.replySerial != 0;
    if (!complete || (bodyLength && out.sig.empty())) return -1;
    out.body.assign(buf.substr(start, bodyLength));
    Reader body = out.Body();
    if (!body.Skip(out.sig) || body.pos() != bodyLength) return -1;
    used = start + bodyLength;
    return 1;
}

std::vector<Address> ParseAddresses(std::string_view text) {
    std::vector<Address> out;
    auto decode = [](std::string_view v, std::string& to) {
        auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
        for (size_t i = 0; i < v.size(); ++i) {
            if (v[i] != '%') {
                to += v[i];
                continue;
            }
            if (i + 2 >= v.size() || hex(v[i + 1]) < 0 || hex(v[i + 2]) < 0) return false;
            char c = (char)(hex(v[i + 1]) * 16 + hex(v[i + 2]));
            if (!c) return false;
            to += c;
            i += 2;
        }
        return true;
    };
    while (!text.empty()) {
        std::string_view entry = text.substr(0, text.find(';'));
        text.remove_prefix(std::min(text.size(), entry.size() + 1));
        if (entry.substr(0, 5) != "unix:") continue;
        entry.remove_prefix(5);
        Address a{false, {}};
        bool have = false, bad = false;
        while (!entry.empty() && !bad) {
            std::string_view kv = entry.substr(0, entry.find(','));
            entry.remove_prefix(std::min(entry.size(), kv.size() + 1));
            size_t eq = kv.find('=');
            if (eq == std::string_view::npos) {
                bad = true;
                break;
            }
            std::string_view key = kv.substr(0, eq);
            std::string value;
            if (!decode(kv.substr(eq + 1), value)) bad = true;
            else if (key == "path" || key == "abstract") {
                bad = have;
                have = true;
                a.abstract = key == "abstract";
                a.path = std::move(value);
            } else if (key != "guid") {
                bad = true;  // dir=, tmpdir= and runtime= are for servers
            }
        }
        if (have && !bad && !a.path.empty() && a.path.size() < sizeof(sockaddr_un::sun_path)) out.push_back(std::move(a));
    }
    return out;
}

// --- the connection ---------------------------------------------------------------------------------------
std::unique_ptr<Bus> Bus::OpenUser(int timeoutMs) {
    int64_t deadline = Now() + timeoutMs;
    std::vector<Address> addresses;
    const char* env = getenv("DBUS_SESSION_BUS_ADDRESS");
    if (env && *env) {
        addresses = ParseAddresses(env);
    } else if (const char* run = getenv("XDG_RUNTIME_DIR"); run && run[0] == '/') {
        std::string path = std::string(run) + "/bus";
        if (path.size() < sizeof(sockaddr_un::sun_path)) addresses.push_back({false, path});
    }
    for (const Address& a : addresses) {
        std::unique_ptr<Bus> bus(new Bus);
        if (!bus->Connect(a, deadline) || !bus->Authenticate(deadline)) continue;
        int left = (int)std::max<int64_t>(1, deadline - Now());
        auto hello = bus->CallSync({"org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "Hello"}, "", {}, left);
        if (!hello || hello->type != kReturn || hello->sig != "s") continue;
        bus->unique_ = std::string(hello->Body().S());
        if (bus->unique_.empty()) continue;
        return bus;
    }
    return nullptr;
}

Bus::~Bus() {
    if (fd_ >= 0) Flush();
    Close();
}

void Bus::Close() {
    if (fd_ >= 0) close(fd_);
    fd_ = -1;
    pending_.clear();
    queued_.clear();
}

bool Bus::WaitFor(short events, int64_t deadline) {
    for (;;) {
        int64_t left = deadline - Now();
        if (left <= 0 || fd_ < 0) return false;
        pollfd p{fd_, events, 0};
        int r = poll(&p, 1, (int)std::min<int64_t>(left, INT_MAX));
        if (r > 0) return true;
        if (r < 0 && errno != EINTR) return false;
    }
}

bool Bus::Connect(const Address& a, int64_t deadline) {
    fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd_ < 0) return false;
    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    size_t offset = a.abstract ? 1 : 0;  // the abstract namespace starts with a NUL
    memcpy(sa.sun_path + offset, a.path.data(), a.path.size());
    socklen_t length = (socklen_t)(offsetof(sockaddr_un, sun_path) + offset + a.path.size() + (a.abstract ? 0 : 1));
    if (connect(fd_, (sockaddr*)&sa, length) == 0) return true;
    int error = 0;
    socklen_t size = sizeof(error);
    if ((errno == EINPROGRESS || errno == EAGAIN) && WaitFor(POLLOUT, deadline) &&
        getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &size) == 0 && !error)
        return true;
    Close();
    return false;
}

// SASL EXTERNAL as this process's uid; the broker checks it against the socket's peer credentials. One DATA
// challenge is answered; anything else, REJECTED included, ends the attempt instead of trying another
// mechanism.
bool Bus::Authenticate(int64_t deadline) {
    std::string hex;
    for (char c : std::to_string(getuid())) {
        static const char digits[] = "0123456789abcdef";
        hex += digits[(unsigned char)c >> 4];
        hex += digits[c & 15];
    }
    out_.assign(1, '\0');
    out_ += "AUTH EXTERNAL " + hex + "\r\n";
    bool answered = false;
    size_t total = 0;
    for (;;) {
        if (!Flush()) return false;
        size_t eol = in_.find("\r\n");
        if (eol == std::string::npos) {
            if (in_.size() > 16384 || !WaitFor(POLLIN | (out_.empty() ? 0 : POLLOUT), deadline)) return Close(), false;
            size_t before = in_.size();
            if (!Read()) return false;
            if ((total += in_.size() - before) > 65536) return Close(), false;
            continue;
        }
        std::string line = in_.substr(0, eol);
        in_.erase(0, eol + 2);
        if (line.rfind("OK ", 0) == 0) {
            out_ += "BEGIN\r\n";  // sent with Hello
            return true;
        }
        if (line.rfind("DATA", 0) == 0 && !answered) {
            answered = true;
            out_ += "DATA " + hex + "\r\n";
            continue;
        }
        Close();
        return false;
    }
}

bool Bus::Read() {
    while (fd_ >= 0 && in_.size() < kMaxMessage + 16) {
        char buf[16384];
        alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * 8)];
        iovec io{buf, sizeof(buf)};
        msghdr mh{};
        mh.msg_iov = &io;
        mh.msg_iovlen = 1;
        mh.msg_control = control;
        mh.msg_controllen = sizeof(control);
        ssize_t n = recvmsg(fd_, &mh, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
        bool fds = false;
        if (n > 0)
            for (cmsghdr* c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c))
                if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
                    fds = true;
                    for (size_t i = 0; i < (c->cmsg_len - CMSG_LEN(0)) / sizeof(int); ++i) {
                        int fd;
                        memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof(fd));
                        close(fd);
                    }
                }
        if (n <= 0 || fds || (mh.msg_flags & MSG_CTRUNC)) {
            Close();
            return false;
        }
        in_.append(buf, (size_t)n);
        if ((size_t)n < sizeof(buf)) return true;
    }
    return fd_ >= 0;
}

bool Bus::Flush() {
    while (fd_ >= 0 && !out_.empty()) {
        ssize_t n = send(fd_, out_.data(), out_.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
        if (n <= 0) {
            Close();
            return false;
        }
        out_.erase(0, (size_t)n);
    }
    return fd_ >= 0;
}

int Bus::Next(Message& m) {
    if (fd_ < 0) return -1;
    size_t used = 0;
    int r = ParseMessage(in_, m, used);
    if (r < 0) Close();
    else if (r > 0) in_.erase(0, used);
    return r;
}

short Bus::Events() const { return (short)(POLLIN | (out_.empty() ? 0 : POLLOUT)); }

int Bus::TimeoutMs() const {
    if (pending_.empty()) return -1;
    int64_t first = INT64_MAX;
    for (const Pending& p : pending_) first = std::min(first, p.deadline);
    return (int)std::clamp<int64_t>(first - Now(), 0, INT_MAX);
}

void Bus::Deliver(Message& m) {
    if (m.type == kReturn || m.type == kError) {
        for (size_t i = 0; i < pending_.size(); ++i)
            if (pending_[i].serial == m.replySerial) {
                ReplyFn fn = std::move(pending_[i].fn);
                pending_.erase(pending_.begin() + i);
                if (fn) fn(&m);
                return;
            }
        return;  // unknown or repeated: ignored
    }
    if (onMessage) onMessage(*this, m);
    else if (m.type == kCall) Error(m, "org.freedesktop.DBus.Error.UnknownMethod", "no such method");
}

int Bus::Process(int budget) {
    if (!Flush()) return -1;
    int handled = 0;
    while (!queued_.empty() && handled < budget) {
        Message m = std::move(queued_.front());
        queued_.erase(queued_.begin());
        Deliver(m);
        ++handled;
        if (fd_ < 0) return -1;
    }
    if (!Read()) return -1;
    for (; handled < budget; ++handled) {
        Message m;
        int r = Next(m);
        if (r < 0) return -1;
        if (!r) break;
        Deliver(m);
        if (fd_ < 0) return -1;
    }
    int64_t now = Now();
    for (size_t i = 0; i < pending_.size();) {
        if (pending_[i].deadline > now) {
            ++i;
            continue;
        }
        ReplyFn fn = std::move(pending_[i].fn);
        pending_.erase(pending_.begin() + i);
        if (fn) fn(nullptr);
        if (fd_ < 0) return -1;
    }
    if (!Flush()) return -1;
    return handled >= budget ? 1 : 0;
}

uint32_t Bus::Queue(uint8_t type, uint8_t flags, const Target& t, std::string_view sig, const Writer& body,
                    uint32_t replyTo, std::string_view error) {
    if (fd_ < 0) return 0;
    if (++serial_ == 0) serial_ = 1;
    Writer h;
    h.Y('l'), h.Y(type), h.Y(flags), h.Y(1);
    h.U((uint32_t)body.b.size());
    h.U(serial_);
    Writer::Array fields = h.Open(8);
    auto field = [&](uint8_t code, const char* s, std::string_view v) {
        if (v.empty()) return;
        h.Align(8), h.Y(code), h.G(s);
        *s == 'g' ? h.G(v) : h.S(v);
    };
    field(1, "o", t.path);
    field(2, "s", t.iface);
    field(3, "s", t.member);
    field(4, "s", error);
    if (replyTo) h.Align(8), h.Y(5), h.G("u"), h.U(replyTo);
    field(6, "s", t.dest);
    field(8, "g", sig);
    h.Close(fields);
    h.Align(8);
    if (h.b.size() + body.b.size() > kMaxMessage || out_.size() + h.b.size() + body.b.size() > kMaxMessage) {
        Close();  // a peer that stops reading does not get to grow this buffer
        return 0;
    }
    out_ += h.b;
    out_ += body.b;
    return serial_;
}

void Bus::Call(const Target& t, std::string_view sig, const Writer& body, int timeoutMs, ReplyFn reply) {
    uint32_t serial = Queue(kCall, reply ? 0 : kNoReply, t, sig, body);
    if (serial && reply) pending_.push_back({serial, Now() + (timeoutMs > 0 ? timeoutMs : 25000), std::move(reply)});
    Flush();
}

std::optional<Message> Bus::CallSync(const Target& t, std::string_view sig, const Writer& body, int timeoutMs) {
    uint32_t serial = Queue(kCall, 0, t, sig, body);
    if (!serial) return std::nullopt;
    int64_t deadline = Now() + timeoutMs;
    for (;;) {
        if (!Flush()) return std::nullopt;
        for (;;) {
            Message m;
            int r = Next(m);
            if (r < 0) return std::nullopt;
            if (!r) break;
            if ((m.type == kReturn || m.type == kError) && m.replySerial == serial) return m;
            if (queued_.size() >= 256) return Close(), std::nullopt;
            queued_.push_back(std::move(m));
        }
        if (!WaitFor(Events(), deadline) || !Read()) return std::nullopt;
    }
}

void Bus::Signal(std::string_view path, std::string_view iface, std::string_view member, std::string_view sig,
                 const Writer& body) {
    Queue(kSignal, kNoReply, {{}, path, iface, member}, sig, body);
}

void Bus::Reply(const Message& call, std::string_view sig, const Writer& body) {
    if (!(call.flags & kNoReply)) Queue(kReturn, kNoReply, {call.sender, {}, {}, {}}, sig, body, call.serial);
}

void Bus::Error(const Message& call, std::string_view name, std::string_view text) {
    if (call.flags & kNoReply) return;
    Writer w;
    w.S(text);
    Queue(kError, kNoReply, {call.sender, {}, {}, {}}, "s", w, call.serial, name);
}

// --- exported objects ---------------------------------------------------------------------------------
namespace {

void Args(std::string& x, std::string_view sig, const char* direction) {
    for (size_t i = 0, n; i < sig.size() && (n = TypeLength(sig, i)); i += n) {
        x += "<arg type=\"";
        x += sig.substr(i, n);
        x += direction ? "\" direction=\"" : "";
        x += direction ? direction : "";
        x += "\"/>";
    }
}

std::string Introspect(std::string_view path, std::span<const Interface> objects) {
    std::string x = "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" "
                    "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n<node>\n";
    bool any = false;
    for (const Interface& o : objects) {
        if (path != o.path) continue;
        any = true;
        x += std::string("<interface name=\"") + o.name + "\">\n";
        for (const Method& m : o.methods) {
            x += std::string("<method name=\"") + m.name + "\">";
            Args(x, m.in, "in");
            Args(x, m.out, "out");
            x += "</method>\n";
        }
        for (const SignalInfo& s : o.signals) {
            x += std::string("<signal name=\"") + s.name + "\">";
            Args(x, s.sig, nullptr);
            x += "</signal>\n";
        }
        for (const Property& p : o.properties)
            x += std::string("<property name=\"") + p.name + "\" type=\"" + p.sig + "\" access=\"" +
                 (p.writable ? "readwrite" : "read") + "\"/>\n";
        x += "</interface>\n";
    }
    if (any)
        x += "<interface name=\"org.freedesktop.DBus.Peer\"><method name=\"Ping\"/></interface>\n"
             "<interface name=\"org.freedesktop.DBus.Introspectable\"><method name=\"Introspect\">"
             "<arg type=\"s\" direction=\"out\"/></method></interface>\n"
             "<interface name=\"org.freedesktop.DBus.Properties\">"
             "<method name=\"Get\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"v\" direction=\"out\"/></method>"
             "<method name=\"GetAll\"><arg type=\"s\" direction=\"in\"/><arg type=\"a{sv}\" direction=\"out\"/></method>"
             "<method name=\"Set\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"v\" direction=\"in\"/></method>"
             "<signal name=\"PropertiesChanged\"><arg type=\"s\"/><arg type=\"a{sv}\"/><arg type=\"as\"/></signal>"
             "</interface>\n";
    std::string prefix = path == "/" ? "/" : std::string(path) + "/";
    std::vector<std::string_view> children;
    for (const Interface& o : objects) {
        std::string_view p = o.path;
        if (p.size() <= prefix.size() || p.substr(0, prefix.size()) != prefix) continue;
        std::string_view child = p.substr(prefix.size());
        child = child.substr(0, child.find('/'));
        if (std::find(children.begin(), children.end(), child) == children.end()) {
            children.push_back(child);
            x += "<node name=\"" + std::string(child) + "\"/>\n";
        }
    }
    return x + "</node>\n";
}

const Interface* Find(std::span<const Interface> objects, std::string_view path, std::string_view name) {
    for (const Interface& o : objects)
        if (path == o.path && name == o.name) return &o;
    return nullptr;
}

const Property* FindProperty(const Interface& o, std::string_view name) {
    for (const Property& p : o.properties)
        if (name == p.name) return &p;
    return nullptr;
}

}  // namespace

void Dispatch(Bus& bus, const Message& m, std::span<const Interface> objects) {
    if (m.type != kCall) return;
    auto error = [&](const char* name, const char* text) { bus.Error(m, std::string(kErr) + name, text); };
    std::string_view iface = m.iface, member = m.member;
    if (iface == "org.freedesktop.DBus.Peer") {
        if (member == "Ping" && m.sig.empty()) bus.Reply(m, "", {});
        else error("UnknownMethod", "only Ping is answered");
        return;
    }
    if ((iface == "org.freedesktop.DBus.Introspectable" || iface.empty()) && member == "Introspect" && m.sig.empty()) {
        Writer w;
        w.S(Introspect(m.path, objects));
        bus.Reply(m, "s", w);
        return;
    }
    if (std::none_of(objects.begin(), objects.end(), [&](const Interface& o) { return m.path == o.path; }))
        return error("UnknownObject", "no such object");
    if (iface == "org.freedesktop.DBus.Properties") {
        Reader in = m.Body();
        bool get = member == "Get" && m.sig == "ss", all = member == "GetAll" && m.sig == "s",
             set = member == "Set" && m.sig == "ssv";
        if (!get && !all && !set) return error("UnknownMethod", "no such method");
        const Interface* o = Find(objects, m.path, in.S());
        if (!o) return error("UnknownInterface", "no such interface");
        Writer w;
        if (all) {
            Writer::Array a = w.Open(8);
            for (const Property& p : o->properties) w.Entry(p.name, p.sig), o->get(p.name, w);
            w.Close(a);
            return bus.Reply(m, "a{sv}", w);
        }
        std::string_view name = in.S();
        const Property* p = FindProperty(*o, name);
        if (!p) return error("UnknownProperty", "no such property");
        if (get) {
            w.G(p->sig);
            o->get(name, w);
            return bus.Reply(m, "v", w);
        }
        if (!p->writable || !o->set) return error("PropertyReadOnly", "this property is read-only");
        if (in.G() != p->sig) return error("InvalidArgs", "wrong type for this property");
        o->set(name, in);
        return bus.Reply(m, "", w);
    }
    for (const Interface& o : objects) {
        if (m.path != o.path || (!iface.empty() && iface != o.name)) continue;
        for (const Method& method : o.methods) {
            if (member != method.name) continue;
            if (m.sig != method.in) return error("InvalidArgs", "wrong arguments for this method");
            Call c{bus, m, m.Body(), {}};
            method.fn(c);
            return bus.Reply(m, method.out, c.out);
        }
        if (!iface.empty()) return error("UnknownMethod", "no such method");
    }
    error(Find(objects, m.path, iface) || iface.empty() ? "UnknownMethod" : "UnknownInterface", "no such method");
}

void EmitChanged(Bus& bus, const Interface& o, std::span<const char* const> names) {
    Writer w;
    w.S(o.name);
    Writer::Array a = w.Open(8);
    for (const char* name : names)
        if (const Property* p = FindProperty(o, name)) w.Entry(p->name, p->sig), o.get(p->name, w);
    w.Close(a);
    w.Close(w.Open(4));  // nothing invalidated
    bus.Signal(o.path, "org.freedesktop.DBus.Properties", "PropertiesChanged", "sa{sv}as", w);
}

}  // namespace lx::dbus
