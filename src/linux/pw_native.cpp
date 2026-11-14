// PipeWire's native protocol, client side (pw_native.h). Written from PipeWire 1.4's protocol
// (src/modules/module-protocol-native, module-client-node and pipewire-jack's client node, MIT),
// which is stable since 0.3: a message is a 16-byte header {object id, opcode << 24 | size, seq,
// fd count} and one SPA POD Struct, with fds passed as SCM_RIGHTS on the Unix socket.
#include "pw_native.h"

#include "decoder.h"
#include "spa_pod.h"

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
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
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace lx::pw {

namespace {

using spa::Builder;
using spa::Reader;
using spa::Value;
using Dict = std::vector<std::pair<std::string, std::string>>;

constexpr uint32_t kInvalid = 0xffffffffu;
constexpr uint32_t kMaxFrames = 8192;  // PipeWire's default clock.quantum-limit
constexpr size_t kMaxMessage = 16u << 20;  // the header's 24-bit size

// Object ids every connection starts with.
constexpr uint32_t kCoreId = 0, kClientId = 1;

// Core methods and events.
enum : uint8_t { kHello = 1, kSync = 2, kPong = 3, kGetRegistry = 5, kCreateObject = 6, kDestroy = 7 };
enum : uint8_t { kEvInfo = 0, kEvDone, kEvPing, kEvError, kEvRemoveId, kEvBoundId, kEvAddMem, kEvRemoveMem, kEvBoundProps };
constexpr uint8_t kClientUpdateProperties = 2;
constexpr uint8_t kRegistryBind = 1;
enum : uint8_t { kEvGlobal = 0, kEvGlobalRemove = 1 };
constexpr uint8_t kMetadataEvProperty = 0;
// Client node methods and events.
enum : uint8_t { kNodeUpdate = 2, kNodePortUpdate = 3, kNodeSetActive = 4 };
enum : uint8_t {
    kEvTransport = 0, kEvSetParam, kEvSetIo, kEvEvent, kEvCommand, kEvAddPort, kEvRemovePort, kEvPortSetParam,
    kEvPortUseBuffers, kEvPortSetIo, kEvSetActivation, kEvPortSetMixInfo,
};

// SPA ids (spa/param, spa/node, spa/buffer).
constexpr uint32_t kObjectFormat = 0x40003, kObjectParamBuffers = 0x40004, kObjectParamIo = 0x40006;
constexpr uint32_t kCommandNode = 0x30002;
constexpr uint32_t kParamEnumFormat = 3, kParamFormat = 4, kParamBuffers = 5, kParamIo = 7;
constexpr uint32_t kFormatMediaType = 1, kFormatMediaSubtype = 2, kFormatAudioFormat = 0x10001;
constexpr uint32_t kMediaAudio = 1, kSubtypeDsp = 2, kAudioDspF32 = 518;
constexpr uint32_t kIoBuffers = 1, kIoClock = 3, kIoPosition = 7, kIoAsyncBuffers = 10;
constexpr uint32_t kCommandSuspend = 0, kCommandPause = 1, kCommandStart = 2;
constexpr uint32_t kDataMemPtr = 1, kDataMemId = 4;
constexpr int32_t kStatusHaveData = 2;
constexpr uint32_t kParamInfoRead = 2, kParamInfoWrite = 4;
constexpr uint32_t kDirectionOutput = 1;
constexpr uint32_t kMemWritable = 2;  // PW_MEMBLOCK_FLAG_WRITABLE

// pw_node_activation (src/pipewire/private.h, PW_VERSION_NODE_ACTIVATION 1): the offsets this
// client touches, checked against PipeWire 1.4's headers when they were written down.
namespace act {
constexpr size_t kStatus = 0, kPending = 16, kSignalTime = 32, kAwakeTime = 40, kFinishTime = 48;
constexpr size_t kClientVersion = 540, kServerVersion = 544, kActiveDriverId = 548, kPosition = 560, kSize = 2312;
enum : uint32_t { NotTriggered = 0, Triggered = 1, Awake = 2, Finished = 3, Inactive = 4 };
}  // namespace act
// spa_io_position starts with spa_io_clock.
namespace clk {
constexpr size_t kId = 4, kRateDenom = 84, kDuration = 96, kDelay = 104, kCycle = 148, kSize = 160;
}

// CORDINAL_PW_DEBUG=1 traces the protocol to stderr.
bool Tracing() {
    static const bool on = [] {
        const char* v = getenv("CORDINAL_PW_DEBUG");
        return v && *v && *v != '0';
    }();
    return on;
}
#define PW_TRACE(...)                                \
    do {                                             \
        if (Tracing()) {                             \
            fprintf(stderr, "pw: " __VA_ARGS__);     \
            fputc('\n', stderr);                     \
        }                                            \
    } while (0)

uint64_t NowNs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

template <typename T> T Load(const uint8_t* p) {
    T v;
    memcpy(&v, p, sizeof(T));
    return v;
}

uint32_t* U32(uint8_t* base, size_t off) { return reinterpret_cast<uint32_t*>(base + off); }

// ---- Parsed messages (pure: shared by the client and FuzzParse)

struct Global {
    uint32_t id = 0, permissions = 0, version = 0;
    std::string type;
    Dict props;
};

const std::string* Lookup(const Dict& d, std::string_view key) {
    for (const auto& [k, v] : d)
        if (k == key) return &v;
    return nullptr;
}

bool ParseGlobal(const uint8_t* data, size_t size, Global& g) {
    Reader m(data, size);
    Reader s = m.Struct();
    g.id = s.Uint();
    g.permissions = s.Uint();
    g.type = s.Str();
    g.version = s.Uint();
    s.Dict(g.props);
    return s.Ok() && m.Ok();
}

// {Int, Int}: Done, Ping, BoundId; {Int}: RemoveId, GlobalRemove, RemoveMem.
bool ParseInts(const uint8_t* data, size_t size, uint32_t* out, int n) {
    Reader m(data, size);
    Reader s = m.Struct();
    for (int i = 0; i < n; ++i) out[i] = s.Uint();
    return s.Ok() && m.Ok();
}

struct CoreError {
    uint32_t id = 0;
    int32_t seq = 0, res = 0;
    std::string message;
};

bool ParseError(const uint8_t* data, size_t size, CoreError& e) {
    Reader m(data, size);
    Reader s = m.Struct();
    e.id = s.Uint();
    e.seq = s.Int();
    e.res = s.Int();
    e.message = s.Str();
    return s.Ok() && m.Ok();
}

struct AddMem {
    uint32_t id = 0, type = 0, flags = 0;
    int64_t fd = -1;
};

bool ParseAddMem(const uint8_t* data, size_t size, AddMem& a) {
    Reader m(data, size);
    Reader s = m.Struct();
    a.id = s.Uint();
    a.type = s.IdV();
    a.fd = s.FdV();
    a.flags = s.Uint();
    return s.Ok() && m.Ok();
}

struct MetadataProperty {
    uint32_t subject = 0;
    std::string key, type, value;
    bool hasValue = false;
};

bool ParseMetadataProperty(const uint8_t* data, size_t size, MetadataProperty& p) {
    Reader m(data, size);
    Reader s = m.Struct();
    p.subject = s.Uint();
    s.StrOrNull(p.key);
    s.StrOrNull(p.type);
    p.hasValue = s.StrOrNull(p.value);
    return s.Ok() && m.Ok();
}

struct Transport {
    int64_t readFd = -1, writeFd = -1;
    uint32_t memId = 0, offset = 0, size = 0;
};

bool ParseTransport(const uint8_t* data, size_t size, Transport& t) {
    Reader m(data, size);
    Reader s = m.Struct();
    t.readFd = s.FdV();
    t.writeFd = s.FdV();
    t.memId = s.Uint();
    t.offset = s.Uint();
    t.size = s.Uint();
    return s.Ok() && m.Ok();
}

struct SetIo {
    uint32_t direction = 0, port = 0, mix = 0, id = 0, memId = 0, offset = 0, size = 0;
};

bool ParseSetIo(const uint8_t* data, size_t size, SetIo& io, bool port) {
    Reader m(data, size);
    Reader s = m.Struct();
    if (port) {
        io.direction = s.Uint();
        io.port = s.Uint();
        io.mix = s.Uint();
    }
    io.id = s.IdV();
    io.memId = s.Uint();
    io.offset = s.Uint();
    io.size = s.Uint();
    return s.Ok() && m.Ok();
}

struct Activation {
    uint32_t node = 0, memId = 0, offset = 0, size = 0;
    int64_t signalFd = -1;
};

bool ParseActivation(const uint8_t* data, size_t size, Activation& a) {
    Reader m(data, size);
    Reader s = m.Struct();
    a.node = s.Uint();
    a.signalFd = s.FdV();
    a.memId = s.Uint();
    a.offset = s.Uint();
    a.size = s.Uint();
    return s.Ok() && m.Ok();
}

struct MixInfo {
    uint32_t direction = 0, port = 0, mix = 0, peer = 0;
    Dict props;
};

bool ParseMixInfo(const uint8_t* data, size_t size, MixInfo& mi) {
    Reader m(data, size);
    Reader s = m.Struct();
    mi.direction = s.Uint();
    mi.port = s.Uint();
    mi.mix = s.Uint();
    mi.peer = s.Uint();
    s.Dict(mi.props);
    return s.Ok() && m.Ok();
}

// The node command's id (Start, Pause, Suspend), or kInvalid.
bool ParseCommand(const uint8_t* data, size_t size, uint32_t& command) {
    Reader m(data, size);
    Reader s = m.Struct();
    spa::ObjectView o = spa::ParseObject(s.ObjectV());
    command = o.ok && o.type == kCommandNode ? o.id : kInvalid;
    return s.Ok() && m.Ok() && o.ok;
}

struct PortParam {
    uint32_t direction = 0, port = 0, id = 0, flags = 0;
    Value param;  // absent: the param is cleared
};

bool ParsePortSetParam(const uint8_t* data, size_t size, PortParam& p) {
    Reader m(data, size);
    Reader s = m.Struct();
    p.direction = s.Uint();
    p.port = s.Uint();
    p.id = s.IdV();
    p.flags = s.Uint();
    p.param = s.ObjectV();
    return s.Ok() && m.Ok();
}

// A Format param this client can play: audio, DSP, 32-bit float.
bool IsDspF32(const Value& v) {
    spa::ObjectView o = spa::ParseObject(v);
    uint32_t type = 0, subtype = 0, format = 0;
    return o.ok && o.type == kObjectFormat && spa::PropId(o, kFormatMediaType, type) && type == kMediaAudio &&
           spa::PropId(o, kFormatMediaSubtype, subtype) && subtype == kSubtypeDsp &&
           spa::PropId(o, kFormatAudioFormat, format) && format == kAudioDspF32;
}

struct BufferData {
    uint32_t type = 0, dataId = 0, flags = 0, mapOffset = 0, maxSize = 0;
};
struct BufferDesc {
    uint32_t memId = 0, offset = 0, size = 0;
    uint32_t metaBytes = 0;  // the metas' total, each rounded up to 8: the chunks follow
    std::vector<BufferData> datas;
};
struct UseBuffers {
    uint32_t direction = 0, port = 0, mix = 0, flags = 0;
    std::vector<BufferDesc> buffers;
};

bool ParseUseBuffers(const uint8_t* data, size_t size, UseBuffers& u) {
    Reader m(data, size);
    Reader s = m.Struct();
    u.direction = s.Uint();
    u.port = s.Uint();
    u.mix = s.Uint();
    u.flags = s.Uint();
    uint32_t n = s.Uint();
    if (!s.Ok() || n > 64) return false;  // PipeWire allows at most 64 buffers per port
    u.buffers.resize(n);
    for (BufferDesc& b : u.buffers) {
        b.memId = s.Uint();
        b.offset = s.Uint();
        b.size = s.Uint();
        uint32_t metas = s.Uint();
        if (!s.Ok() || metas > 16) return false;
        uint64_t metaBytes = 0;
        for (uint32_t i = 0; i < metas; ++i) {
            s.IdV();
            uint32_t sz = s.Uint();
            metaBytes += ((uint64_t)sz + 7) & ~7ull;
        }
        if (metaBytes > b.size) return false;
        b.metaBytes = (uint32_t)metaBytes;
        uint32_t datas = s.Uint();
        if (!s.Ok() || datas > 64) return false;
        b.datas.resize(datas);
        for (BufferData& d : b.datas) {
            d.type = s.IdV();
            d.dataId = s.Uint();
            d.flags = s.Uint();
            d.mapOffset = s.Uint();
            d.maxSize = s.Uint();
        }
        if (!s.Ok()) return false;
    }
    return s.Ok() && m.Ok();
}

// The "name" of a metadata JSON value such as {"name":"alsa_output.pci-0000_00_1f.3.analog-stereo"}.
std::string JsonName(const std::string& json) {
    size_t i = 0;
    auto skip = [&] {
        while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\n' || json[i] == '\r')) ++i;
    };
    auto string = [&](std::string& out) -> bool {
        out.clear();
        skip();
        if (i >= json.size() || json[i] != '"') return false;
        for (++i; i < json.size(); ++i) {
            char c = json[i];
            if (c == '"') {
                ++i;
                return true;
            }
            if (c == '\\' && i + 1 < json.size()) {
                char e = json[++i];
                out.push_back(e == 'n' ? '\n' : e == 't' ? '\t' : e);
            } else {
                out.push_back(c);
            }
        }
        return false;
    };
    skip();
    if (i >= json.size() || json[i] != '{') return {};
    ++i;
    std::string key, value;
    while (string(key)) {
        skip();
        if (i >= json.size() || json[i] != ':') return {};
        ++i;
        skip();
        if (i < json.size() && json[i] == '"') {
            if (!string(value)) return {};
            if (key == "name") return value;
        } else {
            while (i < json.size() && json[i] != ',' && json[i] != '}') ++i;  // a number or literal
        }
        skip();
        if (i < json.size() && json[i] == ',') ++i;
    }
    return {};
}

// ---- The connection

struct Message {
    uint32_t id = 0, opcode = 0, seq = 0;
    const uint8_t* data = nullptr;
    size_t size = 0;
    std::vector<int> fds;  // owned by the connection until the handler takes one (sets it to -1)
    int TakeFd(int64_t index) {
        if (index < 0 || (size_t)index >= fds.size()) return -1;
        int fd = fds[(size_t)index];
        fds[(size_t)index] = -1;
        return fd;
    }
};

class Connection {
public:
    ~Connection() { Close(); }

    bool Connect(std::string& error) {
        std::string path = SocketPath();
        if (path.empty()) {
            error = "no XDG_RUNTIME_DIR";
            return false;
        }
        fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (fd_ < 0 || path.size() >= sizeof(addr.sun_path)) {
            error = "no socket";
            Close();
            return false;
        }
        memcpy(addr.sun_path, path.c_str(), path.size() + 1);
        if (connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            error = "PipeWire isn't running (" + path + ": " + strerror(errno) + ")";
            Close();
            return false;
        }
        return true;
    }

    void Close() {
        if (fd_ >= 0) close(fd_);
        fd_ = -1;
        for (int fd : pendingFds_) close(fd);
        pendingFds_.clear();
        in_.clear();
    }

    int Fd() const { return fd_; }

    bool Send(uint32_t id, uint8_t opcode, const Builder& b, const std::vector<int>& fds = {}) {
        if (fd_ < 0) return false;
        uint32_t header[4] = {id, ((uint32_t)opcode << 24) | ((uint32_t)b.bytes.size() & 0xffffff), seq_++,
                              (uint32_t)fds.size()};
        iovec iov[2] = {{header, sizeof(header)}, {const_cast<uint8_t*>(b.bytes.data()), b.bytes.size()}};
        msghdr msg{};
        msg.msg_iov = iov;
        msg.msg_iovlen = 2;
        alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * 28)];
        if (!fds.empty()) {
            msg.msg_control = control;
            msg.msg_controllen = CMSG_SPACE(sizeof(int) * fds.size());
            cmsghdr* c = CMSG_FIRSTHDR(&msg);
            c->cmsg_level = SOL_SOCKET;
            c->cmsg_type = SCM_RIGHTS;
            c->cmsg_len = CMSG_LEN(sizeof(int) * fds.size());
            memcpy(CMSG_DATA(c), fds.data(), sizeof(int) * fds.size());
        }
        size_t total = sizeof(header) + b.bytes.size();
        size_t sent = 0;
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
            if (sent >= total) break;
            // A short write: send the rest without the fds again.
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

    // Reads what has arrived and hands each complete message to `handle`. False when the server
    // hung up or sent something unreadable.
    template <typename F> bool Receive(F&& handle) {
        uint8_t buf[65536];
        alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * 64)];
        for (;;) {
            iovec iov{buf, sizeof(buf)};
            msghdr msg{};
            msg.msg_iov = &iov;
            msg.msg_iovlen = 1;
            msg.msg_control = control;
            msg.msg_controllen = sizeof(control);
            ssize_t n = recvmsg(fd_, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN) break;
                return false;
            }
            if (n == 0) return false;
            for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
                if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
                size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                for (size_t i = 0; i < count; ++i) pendingFds_.push_back(Load<int>(CMSG_DATA(c) + i * sizeof(int)));
            }
            in_.insert(in_.end(), buf, buf + n);
            if ((size_t)n < sizeof(buf)) break;
        }
        size_t at = 0;
        bool ok = true;
        while (in_.size() - at >= 16) {
            const uint8_t* h = in_.data() + at;
            uint32_t word = Load<uint32_t>(h + 4), nfds = Load<uint32_t>(h + 12);
            size_t size = word & 0xffffff;
            if (in_.size() - at - 16 < size) break;
            if (nfds > 28 || nfds > pendingFds_.size()) {
                ok = false;
                break;
            }
            Message m;
            m.id = Load<uint32_t>(h);
            m.opcode = word >> 24;
            m.seq = Load<uint32_t>(h + 8);
            m.data = h + 16;
            m.size = size;
            m.fds.assign(pendingFds_.begin(), pendingFds_.begin() + nfds);
            pendingFds_.erase(pendingFds_.begin(), pendingFds_.begin() + nfds);
            handle(m);
            for (int fd : m.fds)
                if (fd >= 0) close(fd);
            at += 16 + size;
        }
        in_.erase(in_.begin(), in_.begin() + (std::ptrdiff_t)at);
        if (in_.size() > kMaxMessage + 16) ok = false;
        return ok;
    }

private:
    int fd_ = -1;
    uint32_t seq_ = 0;
    std::vector<uint8_t> in_;
    std::deque<int> pendingFds_;
};

// ---- Shared memory the server hands over (AddMem), mapped on demand.

struct Mapping {
    void* base = nullptr;
    size_t length = 0;
    uint8_t* ptr = nullptr;
    void Release() {
        if (base) munmap(base, length);
        base = nullptr;
        ptr = nullptr;
        length = 0;
    }
};

class MemPool {
public:
    ~MemPool() { Clear(); }
    void Add(uint32_t id, int fd, uint32_t flags) {
        Remove(id);
        blocks_[id] = {fd, flags};
    }
    void Remove(uint32_t id) {
        auto it = blocks_.find(id);
        if (it == blocks_.end()) return;
        close(it->second.fd);
        blocks_.erase(it);
    }
    void Clear() {
        for (auto& [id, b] : blocks_) close(b.fd);
        blocks_.clear();
    }
    // [offset, offset + size) of block `id`, or an empty mapping when that isn't inside it.
    Mapping Map(uint32_t id, uint32_t offset, uint32_t size) {
        Mapping m;
        auto it = blocks_.find(id);
        if (it == blocks_.end() || size == 0) return m;
        struct stat st {};
        if (fstat(it->second.fd, &st) < 0 || (uint64_t)offset + size > (uint64_t)st.st_size) return m;
        static const size_t page = (size_t)sysconf(_SC_PAGESIZE);
        size_t aligned = offset & ~(page - 1);
        size_t delta = offset - aligned;
        int prot = PROT_READ | ((it->second.flags & kMemWritable) ? PROT_WRITE : 0);
        void* p = mmap(nullptr, size + delta, prot, MAP_SHARED, it->second.fd, (off_t)aligned);
        if (p == MAP_FAILED) return m;
        m.base = p;
        m.length = size + delta;
        m.ptr = static_cast<uint8_t*>(p) + delta;
        return m;
    }

private:
    struct Block {
        int fd;
        uint32_t flags;
    };
    std::map<uint32_t, Block> blocks_;
};

// ---- Registry state both the sink list and a stream keep

struct Node {
    std::string name, mediaClass, description, card;
};
struct PortInfo {
    uint32_t node = kInvalid, portId = kInvalid;
    bool input = false, monitor = false;
    std::string channel;
};

class Session {
public:
    Connection conn;
    std::map<uint32_t, Node> nodes;
    std::map<uint32_t, PortInfo> ports;
    std::string defaultSink;  // node.name from default.audio.sink
    std::map<uint32_t, std::string> targets;  // target.object metadata, per subject
    uint32_t registryId = kInvalid, metadataId = kInvalid;
    bool metadataBound = false;
    std::string error;
    bool fatal = false;

    uint32_t NewId() {
        uint32_t id = 2;
        while (usedIds_.count(id)) ++id;
        usedIds_.insert(id);
        return id;
    }

    bool Start(const Dict& clientProps) {
        if (!conn.Connect(error)) return false;
        Builder hello;
        hello.BeginStruct();
        hello.Int(3);  // PW_VERSION_CORE 3 and later share this exchange
        hello.End();
        conn.Send(kCoreId, kHello, hello);
        Builder props;
        props.BeginStruct();
        props.Dict(clientProps);
        props.End();
        conn.Send(kClientId, kClientUpdateProperties, props);
        registryId = NewId();
        Builder reg;
        reg.BeginStruct();
        reg.Int(3);
        reg.Int((int32_t)registryId);
        reg.End();
        return conn.Send(kCoreId, kGetRegistry, reg);
    }

    void Sync(int32_t seq) {
        Builder b;
        b.BeginStruct();
        b.Int(kCoreId);
        b.Int(seq);
        b.End();
        conn.Send(kCoreId, kSync, b);
    }

    void Destroy(uint32_t id) {
        Builder b;
        b.BeginStruct();
        b.Int((int32_t)id);
        b.End();
        conn.Send(kCoreId, kDestroy, b);
    }

    // Handles what every connection gets; true when the message was one of those. `done` is
    // called for Done replies to Sync.
    template <typename Done, typename Changed> bool Handle(Message& m, MemPool* pool, Done&& done, Changed&& changed) {
        if (m.id == kCoreId) {
            uint32_t v[2];
            switch (m.opcode) {
                case kEvDone:
                    if (ParseInts(m.data, m.size, v, 2)) done((int32_t)v[1]);
                    break;
                case kEvPing:
                    if (ParseInts(m.data, m.size, v, 2)) {
                        Builder b;
                        b.BeginStruct();
                        b.Int((int32_t)v[0]);
                        b.Int((int32_t)v[1]);
                        b.End();
                        conn.Send(kCoreId, kPong, b);
                    }
                    break;
                case kEvError: {
                    CoreError e;
                    if (ParseError(m.data, m.size, e)) {
                        error = e.message;
                        errorId = e.id;
                        fatal = fatal || e.id == kCoreId || e.id == watchedId;
                    }
                    break;
                }
                case kEvRemoveId:
                    if (ParseInts(m.data, m.size, v, 1)) usedIds_.erase(v[0]);
                    break;
                case kEvBoundId:
                    if (ParseInts(m.data, m.size, v, 2)) boundIds[v[0]] = v[1];
                    break;
                case kEvAddMem: {
                    AddMem a;
                    if (ParseAddMem(m.data, m.size, a) && pool) {
                        int fd = m.TakeFd(a.fd);
                        if (fd >= 0) pool->Add(a.id, fd, a.flags);
                    }
                    break;
                }
                case kEvRemoveMem:
                    if (ParseInts(m.data, m.size, v, 1) && pool) pool->Remove(v[0]);
                    break;
                default:
                    break;
            }
            return true;
        }
        if (m.id == registryId) {
            if (m.opcode == kEvGlobal) {
                Global g;
                if (ParseGlobal(m.data, m.size, g)) OnGlobal(g, changed);
            } else if (m.opcode == kEvGlobalRemove) {
                uint32_t id;
                if (ParseInts(m.data, m.size, &id, 1)) {
                    nodes.erase(id);
                    ports.erase(id);
                    changed();
                }
            }
            return true;
        }
        if (m.id == metadataId) {
            MetadataProperty p;
            if (m.opcode == kMetadataEvProperty && ParseMetadataProperty(m.data, m.size, p)) {
                if (p.subject == kCoreId && p.key == "default.audio.sink") {
                    defaultSink = p.hasValue ? JsonName(p.value) : "";
                    changed();
                } else if (p.key == "target.object" || p.key == "target.node") {
                    if (p.hasValue && !p.value.empty() && p.value != "-1") targets[p.subject] = p.value;
                    else targets.erase(p.subject);
                    changed();
                } else if (p.key.empty()) {  // everything about the subject was cleared
                    if (p.subject == kCoreId) defaultSink.clear();
                    targets.erase(p.subject);
                    changed();
                }
            }
            return true;
        }
        return false;
    }

    std::map<uint32_t, uint32_t> boundIds;  // proxy id -> global id
    uint32_t watchedId = kInvalid;          // an object whose error ends the session
    uint32_t errorId = kInvalid;

    // A node by name, or by id or serial when the name is a number.
    uint32_t FindNode(const std::string& name) const {
        for (const auto& [id, n] : nodes)
            if (n.name == name) return id;
        char* end = nullptr;
        unsigned long v = strtoul(name.c_str(), &end, 10);
        if (!name.empty() && end && !*end) {
            for (const auto& [id, n] : nodes)
                if (id == v) return id;
            auto s = serials_.find((uint64_t)v);
            if (s != serials_.end()) return s->second;
        }
        return kInvalid;
    }

private:
    template <typename Changed> void OnGlobal(const Global& g, Changed&& changed) {
        if (g.type == "PipeWire:Interface:Node") {
            Node n;
            if (const std::string* s = Lookup(g.props, "node.name")) n.name = *s;
            if (const std::string* s = Lookup(g.props, "media.class")) n.mediaClass = *s;
            if (const std::string* s = Lookup(g.props, "node.description")) n.description = *s;
            else if (const std::string* s2 = Lookup(g.props, "node.nick")) n.description = *s2;
            if (const std::string* s = Lookup(g.props, "api.alsa.pcm.card")) n.card = *s;
            else if (const std::string* s2 = Lookup(g.props, "api.alsa.card")) n.card = *s2;
            if (const std::string* s = Lookup(g.props, "object.serial")) serials_[strtoull(s->c_str(), nullptr, 10)] = g.id;
            nodes[g.id] = std::move(n);
            changed();
        } else if (g.type == "PipeWire:Interface:Port") {
            PortInfo p;
            if (const std::string* s = Lookup(g.props, "node.id")) p.node = (uint32_t)strtoul(s->c_str(), nullptr, 10);
            if (const std::string* s = Lookup(g.props, "port.id")) p.portId = (uint32_t)strtoul(s->c_str(), nullptr, 10);
            if (const std::string* s = Lookup(g.props, "port.direction")) p.input = *s == "in";
            if (const std::string* s = Lookup(g.props, "port.monitor")) p.monitor = *s == "true";
            if (const std::string* s = Lookup(g.props, "audio.channel")) p.channel = *s;
            ports[g.id] = std::move(p);
            changed();
        } else if (g.type == "PipeWire:Interface:Metadata" && !metadataBound) {
            const std::string* name = Lookup(g.props, "metadata.name");
            if (!name || *name != "default") return;
            metadataBound = true;
            metadataId = NewId();
            Builder b;
            b.BeginStruct();
            b.Int((int32_t)g.id);
            b.Str("PipeWire:Interface:Metadata");
            b.Int(3);
            b.Int((int32_t)metadataId);
            b.End();
            conn.Send(registryId, kRegistryBind, b);
        }
    }

    std::set<uint32_t> usedIds_;
    std::map<uint64_t, uint32_t> serials_;
};

Dict ClientProps() {
    char pid[32];
    snprintf(pid, sizeof(pid), "%d", (int)getpid());
    return {{"application.name", "cordinal"}, {"application.process.id", pid}, {"application.process.binary", "cordinal"},
            {"client.api", "cordinal"}};
}

// The default sink's node id: the metadata's, or else the first sink.
uint32_t DefaultSinkId(const Session& s) {
    if (!s.defaultSink.empty()) {
        uint32_t id = s.FindNode(s.defaultSink);
        if (id != kInvalid && s.nodes.at(id).mediaClass == "Audio/Sink") return id;
    }
    for (const auto& [id, n] : s.nodes)
        if (n.mediaClass == "Audio/Sink") return id;
    return kInvalid;
}

// ---- Resampling for a graph that runs at another rate than the stream

class RateAdapter {
public:
    explicit RateAdapter(unsigned rate) : rate_(rate) {
        scratch_.resize(kMaxFrames * 2 * 3);
        fifo_.reserve(kMaxFrames * 2 * 4);
        out_.reserve(kMaxFrames * 2 * 4);
    }

    // `frames` at `graphRate` into `out`, rendering at the stream's rate. Returns the frames
    // rendered at the stream rate on this call.
    template <typename Render> uint32_t Pull(float* out, uint32_t frames, unsigned graphRate, Render&& render) {
        if (graphRate == rate_ || !graphRate) {
            render(out, frames);
            return frames;
        }
        if (graphRate != rsOut_) {
            rs_.Init(rate_, graphRate);
            rsOut_ = graphRate;
            fifo_.clear();
        }
        uint32_t rendered = 0;
        while (fifo_.size() / 2 < frames) {
            size_t need = frames - fifo_.size() / 2;
            uint32_t in = (uint32_t)std::min<uint64_t>((uint64_t)need * rate_ / graphRate + 16, kMaxFrames);
            render(scratch_.data(), in);
            rendered += in;
            rs_.Process(scratch_.data(), in, out_);
            fifo_.insert(fifo_.end(), out_.begin(), out_.end());
        }
        memcpy(out, fifo_.data(), (size_t)frames * 2 * sizeof(float));
        fifo_.erase(fifo_.begin(), fifo_.begin() + (std::ptrdiff_t)frames * 2);
        return rendered;
    }

    // Stream-rate frames rendered but still inside the adapter.
    uint64_t Held(unsigned graphRate) const {
        if (graphRate == rate_ || !graphRate || graphRate != rsOut_) return 0;
        return (uint64_t)(fifo_.size() / 2) * rate_ / graphRate + 64;  // + the filter's half length
    }

    void Reset() {
        fifo_.clear();
        if (rsOut_) rs_.Reset();
    }

private:
    unsigned rate_, rsOut_ = 0;
    Resampler rs_;
    std::vector<float> scratch_, fifo_, out_;
};

// ---- The playback stream: a client node with two output ports

class PwStream final : public sound::Stream {
public:
    PwStream(const sound::Params& params, sound::Callbacks cb, std::function<void()> onFailure)
        : params_(params), cb_(std::move(cb)), onFailure_(std::move(onFailure)), adapter_(params.rate) {
        active_ = params.active;
        mix_.resize(kMaxFrames * 2);
        wakeFd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    }

    ~PwStream() override {
        quit_ = true;
        Wake();
        if (thread_.joinable()) thread_.join();
        if (wakeFd_ >= 0) close(wakeFd_);
    }

    bool Start(std::string& error) {
        thread_ = std::thread([this] { Run(); });
        std::unique_lock lock(mx_);
        cv_.wait_for(lock, std::chrono::seconds(3), [&] { return state_ != Opening; });
        if (state_ == Ready) return true;
        error = error_.empty() ? "PipeWire didn't answer" : error_;
        return false;
    }

    void SetActive(bool active) override {
        active_ = active;
        activeDirty_ = true;
        Wake();
    }

    void Flush() override { flush_ = true; }

    bool Failed() const override { return failed_; }
    const char* Backend() const override { return "PipeWire"; }

private:
    enum State { Opening, Ready, Broken };

    struct Buffer {
        Mapping map;
        std::vector<Mapping> dataMaps;
        float* data = nullptr;
        uint32_t maxSize = 0;
        uint8_t* chunk = nullptr;  // spa_chunk {offset, size, stride, flags}
    };
    struct Mix {
        Mapping ioMap;
        uint8_t* io = nullptr;  // spa_io_buffers {status, buffer_id}, two of them when async
        bool async = false;
        std::vector<Buffer> buffers;
        uint32_t next = 0;
        bool peer = false;
    };
    struct Port {
        std::map<uint32_t, Mix> mixes;  // by mix id; kInvalid is the port's own
        bool hasFormat = false;
    };
    struct Target {
        Mapping map;
        int fd = -1;
    };

    void Wake() {
        uint64_t one = 1;
        if (wakeFd_ >= 0) (void)!write(wakeFd_, &one, sizeof(one));
    }

    void Finish(State s, const std::string& error = {}) {
        std::lock_guard lock(mx_);
        if (state_ == Opening || s == Broken) {
            if (state_ != Broken) state_ = s;
            if (!error.empty() && error_.empty()) error_ = error;
        }
        cv_.notify_all();
    }

    Dict NodeProps() const {
        char latency[32], rate[32];
        snprintf(latency, sizeof(latency), "%u/%u", params_.latencyFrames, params_.rate);
        snprintf(rate, sizeof(rate), "1/%u", params_.rate);
        Dict d = {{"media.type", "Audio"},
                  {"media.category", "Playback"},
                  {"media.role", params_.role},
                  {"media.class", "Stream/Output/Audio"},
                  {"media.name", params_.name},
                  {"application.name", "cordinal"},
                  {"node.name", params_.name},
                  {"node.description", params_.name},
                  {"node.latency", latency},
                  {"node.autoconnect", "false"},  // it links itself
                  {"node.want-driver", "true"},
                  {"audio.channels", "2"},
                  {"audio.position", "FL,FR"}};
        if (params_.exclusive) d.push_back({"node.rate", rate});  // ask the graph to run at our rate
        return d;
    }

    void SendNodeUpdate() {
        Dict props = NodeProps();
        Builder b;
        b.BeginStruct();
        b.Int(2);  // PW_CLIENT_NODE_UPDATE_INFO
        b.Int(0);
        b.BeginStruct();
        b.Int(0);   // max input ports
        b.Int(2);   // max output ports
        b.LongV(1 | 2);  // flags, props
        b.LongV(1);      // SPA_NODE_FLAG_RT
        b.Int((int32_t)props.size());
        for (const auto& [k, v] : props) {
            b.Str(k);
            b.Str(v);
        }
        b.Int(0);
        b.End();
        b.End();
        s_.conn.Send(nodeProxy_, kNodeUpdate, b);
    }

    static void DspFormat(Builder& b, uint32_t id) {
        b.BeginObject(kObjectFormat, id);
        b.Prop(kFormatMediaType);
        b.IdV(kMediaAudio);
        b.Prop(kFormatMediaSubtype);
        b.IdV(kSubtypeDsp);
        b.Prop(kFormatAudioFormat);
        b.IdV(kAudioDspF32);
        b.End();
    }

    void SendPortUpdate(uint32_t port) {
        const bool format = ports_[port].hasFormat;
        const char* channel = port == 0 ? "FL" : "FR";
        Dict props = {{"format.dsp", "32 bit float mono audio"},
                      {"port.name", std::string("output_") + channel},
                      {"port.alias", params_.name + ":output_" + channel},
                      {"audio.channel", channel}};
        Builder b;
        b.BeginStruct();
        b.Int((int32_t)kDirectionOutput);
        b.Int((int32_t)port);
        b.Int(1 | 2);  // params, info
        b.Int(format ? 4 : 3);
        DspFormat(b, kParamEnumFormat);
        if (format) DspFormat(b, kParamFormat);
        b.BeginObject(kObjectParamBuffers, kParamBuffers);
        b.Prop(1);  // buffers
        b.ChoiceInt(spa::ChoiceRange, {2, 1, 8});
        b.Prop(2);  // blocks
        b.Int(1);
        b.Prop(3);  // size
        b.ChoiceInt(spa::ChoiceStep, {(int32_t)(kMaxFrames * sizeof(float)), (int32_t)sizeof(float), INT32_MAX,
                                      (int32_t)sizeof(float)});
        b.Prop(4);  // stride
        b.Int((int32_t)sizeof(float));
        b.End();
        b.BeginObject(kObjectParamIo, kParamIo);
        b.Prop(1);
        b.IdV(kIoBuffers);
        b.Prop(2);
        b.Int(8);
        b.End();
        b.BeginStruct();
        b.LongV(1 | 4 | 8);  // flags, props, params
        b.LongV(0);
        b.Int(0);
        b.Int(1);
        b.Int((int32_t)props.size());
        for (const auto& [k, v] : props) {
            b.Str(k);
            b.Str(v);
        }
        b.Int(4);
        b.IdV(kParamEnumFormat);
        b.Int((int32_t)kParamInfoRead);
        b.IdV(kParamFormat);
        b.Int((int32_t)(format ? kParamInfoRead | kParamInfoWrite : kParamInfoWrite));
        b.IdV(kParamBuffers);
        b.Int((int32_t)kParamInfoRead);
        b.IdV(kParamIo);
        b.Int((int32_t)kParamInfoRead);
        b.End();
        b.End();
        s_.conn.Send(nodeProxy_, kNodePortUpdate, b);
    }

    void SendActive(bool active) {
        Builder b;
        b.BeginStruct();
        b.BoolV(active);
        b.End();
        s_.conn.Send(nodeProxy_, kNodeSetActive, b);
    }

    void Run() {
        RaisePriority();
        if (!s_.Start(ClientProps())) {
            failed_ = true;
            Finish(Broken, s_.error);
            return;
        }
        nodeProxy_ = s_.NewId();
        s_.watchedId = nodeProxy_;
        {
            Builder b;
            b.BeginStruct();
            b.Str("client-node");
            b.Str("PipeWire:Interface:ClientNode");
            b.Int(6);  // PW_VERSION_CLIENT_NODE
            b.Dict(NodeProps());
            b.Int((int32_t)nodeProxy_);
            b.End();
            s_.conn.Send(kCoreId, kCreateObject, b);
        }
        SendNodeUpdate();
        SendPortUpdate(0);
        SendPortUpdate(1);
        SendActive(active_);
        activeDirty_ = false;
        s_.Sync(1);

        while (!quit_) {
            pollfd fds[3] = {{s_.conn.Fd(), POLLIN, 0}, {wakeFd_, POLLIN, 0}, {rtFd_, POLLIN, 0}};
            int n = poll(fds, rtFd_ >= 0 && started_ ? 3 : 2, 1000);
            if (n < 0 && errno != EINTR) break;
            if (quit_) break;
            if (fds[2].revents & POLLIN) Process();
            if (fds[1].revents & POLLIN) {
                uint64_t v;
                (void)!read(wakeFd_, &v, sizeof(v));
                if (activeDirty_.exchange(false)) SendActive(active_);
            }
            if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
                bool ok = s_.conn.Receive([&](Message& m) { Dispatch(m); });
                if (!ok || s_.fatal) {
                    Fail(ok ? s_.error : "PipeWire went away");
                    break;
                }
            }
        }
        Teardown();
    }

    void Fail(const std::string& why) {
        bool was = failed_.exchange(true);
        Finish(Broken, why);
        if (!was && onFailure_) onFailure_();
    }

    // Leaves the graph cleanly: stop taking part in cycles, then let the server drop everything
    // with the connection.
    void Teardown() {
        if (started_ && activation_) Unprepare();
        s_.conn.Close();
        for (auto& port : ports_)
            for (auto& [id, mix] : port.mixes) ReleaseMix(mix);
        for (auto& [id, t] : targets_) {
            t.map.Release();
            if (t.fd >= 0) close(t.fd);
        }
        targets_.clear();
        activationMap_.Release();
        positionMap_.Release();
        activation_ = nullptr;
        position_ = nullptr;
        if (rtFd_ >= 0) close(rtFd_);
        rtFd_ = -1;
        pool_.Clear();
    }

    static void ReleaseMix(Mix& mix) {
        mix.ioMap.Release();
        mix.io = nullptr;
        for (Buffer& b : mix.buffers) {
            b.map.Release();
            for (Mapping& d : b.dataMaps) d.Release();
        }
        mix.buffers.clear();
    }

    // The audio thread runs at real-time priority when the system lets it (RLIMIT_RTPRIO).
    static void RaisePriority() {
        sched_param p{};
        p.sched_priority = 20;
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO | SCHED_RESET_ON_FORK, &p) != 0) {
            p.sched_priority = 0;
            (void)!nice(-10);
        }
    }

    void Dispatch(Message& m) {
        PW_TRACE("<- id %u op %u size %zu fds %zu", m.id, m.opcode, m.size, m.fds.size());
        if (s_.Handle(m, &pool_, [&](int32_t seq) { OnDone(seq); }, [&] { Relink(); })) return;
        if (m.id != nodeProxy_) return;
        switch (m.opcode) {
            case kEvTransport: {
                Transport t;
                if (!ParseTransport(m.data, m.size, t)) break;
                int readFd = m.TakeFd(t.readFd);
                if (readFd < 0 || t.size < act::kSize) {
                    if (readFd >= 0) close(readFd);
                    break;
                }
                activationMap_.Release();
                activationMap_ = pool_.Map(t.memId, t.offset, t.size);
                activation_ = activationMap_.ptr;
                if (!activation_) {
                    close(readFd);
                    break;
                }
                *U32(activation_, act::kClientVersion) = 1;
                if (rtFd_ >= 0) close(rtFd_);
                rtFd_ = readFd;
                break;
            }
            case kEvSetIo: {
                SetIo io;
                if (!ParseSetIo(m.data, m.size, io, false)) break;
                if (io.id == kIoPosition) {
                    positionMap_.Release();
                    position_ = nullptr;
                    if (io.memId != kInvalid && io.size >= clk::kSize) {
                        positionMap_ = pool_.Map(io.memId, io.offset, io.size);
                        position_ = positionMap_.ptr;
                    }
                    // The driver only schedules a node that says it follows that driver.
                    if (activation_)
                        __atomic_store_n(U32(activation_, act::kActiveDriverId),
                                         position_ ? Load<uint32_t>(position_ + clk::kId) : kInvalid, __ATOMIC_SEQ_CST);
                }
                break;
            }
            case kEvCommand: {
                uint32_t command;
                if (!ParseCommand(m.data, m.size, command)) break;
                PW_TRACE("command %u (activation %p)", command, (void*)activation_);
                if (command == kCommandStart && !started_ && activation_) {
                    __atomic_store_n(U32(activation_, act::kStatus), act::Finished, __ATOMIC_SEQ_CST);
                    started_ = true;
                } else if ((command == kCommandPause || command == kCommandSuspend) && started_) {
                    Unprepare();
                }
                break;
            }
            case kEvPortSetParam: {
                PortParam p;
                if (!ParsePortSetParam(m.data, m.size, p) || p.direction != kDirectionOutput || p.port > 1) break;
                if (p.id == kParamFormat) {
                    ports_[p.port].hasFormat = p.param.Valid() && IsDspF32(p.param);
                    if (!p.param.Valid())
                        for (auto& [id, mix] : ports_[p.port].mixes) {
                            for (Buffer& b : mix.buffers) {
                                b.map.Release();
                                for (Mapping& d : b.dataMaps) d.Release();
                            }
                            mix.buffers.clear();
                        }
                    SendPortUpdate(p.port);
                }
                break;
            }
            case kEvPortUseBuffers: {
                UseBuffers u;
                if (!ParseUseBuffers(m.data, m.size, u) || u.direction != kDirectionOutput || u.port > 1) break;
                Mix& mix = ports_[u.port].mixes[u.mix];
                for (Buffer& b : mix.buffers) {
                    b.map.Release();
                    for (Mapping& d : b.dataMaps) d.Release();
                }
                mix.buffers.clear();
                mix.next = 0;
                PW_TRACE("use_buffers port %u mix %u: %zu buffers", u.port, u.mix, u.buffers.size());
                for (const BufferDesc& d : u.buffers) {
                    if (!d.datas.empty())
                        PW_TRACE("  mem %u off %u size %u metas %u data type %u id %u maxsize %u", d.memId, d.offset, d.size,
                                 d.metaBytes, d.datas[0].type, d.datas[0].dataId, d.datas[0].maxSize);
                    Buffer b;
                    b.map = pool_.Map(d.memId, d.offset, d.size);
                    if (!b.map.ptr || d.datas.empty() || (uint64_t)d.metaBytes + 16 * d.datas.size() > d.size) {
                        b.map.Release();
                        continue;
                    }
                    b.chunk = b.map.ptr + d.metaBytes;
                    const BufferData& data = d.datas[0];
                    if (data.type == kDataMemId) {
                        Mapping dm = pool_.Map(data.dataId, data.mapOffset, data.maxSize);
                        b.data = reinterpret_cast<float*>(dm.ptr);
                        if (dm.ptr) b.dataMaps.push_back(dm);
                    } else if (data.type == kDataMemPtr && (uint64_t)data.dataId + data.maxSize <= d.size) {
                        b.data = reinterpret_cast<float*>(b.map.ptr + data.dataId);
                    }
                    b.maxSize = data.maxSize;
                    if (!b.data) {
                        b.map.Release();
                        for (Mapping& dm : b.dataMaps) dm.Release();
                        continue;
                    }
                    mix.buffers.push_back(std::move(b));
                }
                break;
            }
            case kEvPortSetIo: {
                SetIo io;
                if (!ParseSetIo(m.data, m.size, io, true) || io.direction != kDirectionOutput || io.port > 1) break;
                if (io.id != kIoBuffers && io.id != kIoAsyncBuffers) break;
                PW_TRACE("port_set_io port %u mix %u id %u mem %u size %u", io.port, io.mix, io.id, io.memId, io.size);
                Mix& mix = ports_[io.port].mixes[io.mix];
                mix.ioMap.Release();
                mix.io = nullptr;
                if (io.memId != kInvalid && io.size >= 8) {
                    mix.ioMap = pool_.Map(io.memId, io.offset, io.size);
                    mix.io = mix.ioMap.ptr;
                    mix.async = io.id == kIoAsyncBuffers && io.size >= 16;
                }
                break;
            }
            case kEvSetActivation: {
                Activation a;
                if (!ParseActivation(m.data, m.size, a)) break;
                int fd = m.TakeFd(a.signalFd);
                auto old = targets_.find(a.node);
                if (old != targets_.end()) {
                    old->second.map.Release();
                    if (old->second.fd >= 0) close(old->second.fd);
                    targets_.erase(old);
                }
                if (a.memId == kInvalid || a.size < act::kSize) {
                    if (fd >= 0) close(fd);
                    break;
                }
                Target t;
                t.map = pool_.Map(a.memId, a.offset, a.size);
                t.fd = fd;
                if (!t.map.ptr || fd < 0) {
                    t.map.Release();
                    if (fd >= 0) close(fd);
                    break;
                }
                PW_TRACE("target node %u (ours %u)", a.node, nodeId_);
                targets_[a.node] = std::move(t);
                break;
            }
            case kEvPortSetMixInfo: {
                MixInfo mi;
                if (!ParseMixInfo(m.data, m.size, mi) || mi.direction != kDirectionOutput || mi.port > 1) break;
                auto& mixes = ports_[mi.port].mixes;
                PW_TRACE("mix_info port %u mix %u peer %u", mi.port, mi.mix, mi.peer);
                if (mi.peer == kInvalid) {
                    auto it = mixes.find(mi.mix);
                    if (it != mixes.end()) {
                        ReleaseMix(it->second);
                        mixes.erase(it);
                    }
                } else {
                    mixes[mi.mix].peer = true;
                }
                break;
            }
            default:  // SetParam, Event, AddPort, RemovePort: nothing to do for this node
                break;
        }
    }

    void OnDone(int32_t seq) {
        if (seq == 1) {
            auto it = s_.boundIds.find(nodeProxy_);
            if (s_.fatal) {
                Fail(s_.error);
                return;
            }
            if (it != s_.boundIds.end()) nodeId_ = it->second;
            Finish(Ready);
            Relink();
        }
    }

    // Our two output ports' global ids, once the registry has shown them.
    bool OurPorts(uint32_t out[2]) const {
        out[0] = out[1] = kInvalid;
        for (const auto& [id, p] : s_.ports)
            if (p.node == nodeId_ && !p.input && p.portId < 2) out[p.portId] = id;
        return out[0] != kInvalid && out[1] != kInvalid;
    }

    uint32_t WantedSink() const {
        std::string target = params_.target;
        auto moved = s_.targets.find(nodeId_);  // a mixer moved the stream
        if (moved != s_.targets.end()) target = moved->second;
        if (!target.empty()) {
            uint32_t id = s_.FindNode(target);
            if (id != kInvalid && s_.nodes.at(id).mediaClass == "Audio/Sink") return id;
        }
        return DefaultSinkId(s_);  // the asked-for sink is gone: the default plays it meanwhile
    }

    // Points the ports at the wanted sink, replacing the links to another one.
    void Relink() {
        if (nodeId_ == kInvalid) {
            auto it = s_.boundIds.find(nodeProxy_);
            if (it == s_.boundIds.end()) return;
            nodeId_ = it->second;
        }
        uint32_t ours[2];
        if (!OurPorts(ours)) return;
        uint32_t sink = WantedSink();
        // The sink's playback ports, by channel.
        std::vector<std::pair<std::string, uint32_t>> inputs;
        for (const auto& [id, p] : s_.ports)
            if (p.node == sink && p.input && !p.monitor) inputs.push_back({p.channel, id});
        std::sort(inputs.begin(), inputs.end(), [&](const auto& a, const auto& b) {
            return s_.ports.at(a.second).portId < s_.ports.at(b.second).portId;
        });
        uint32_t peer[2] = {kInvalid, kInvalid};
        for (int c = 0; c < 2; ++c) {
            const char* want = c == 0 ? "FL" : "FR";
            for (const auto& [ch, id] : inputs)
                if (ch == want) peer[c] = id;
            if (peer[c] == kInvalid) {
                for (const auto& [ch, id] : inputs)
                    if (ch == "MONO") peer[c] = id;
            }
            if (peer[c] == kInvalid && inputs.size() > (size_t)c) peer[c] = inputs[(size_t)c].second;
            if (peer[c] == kInvalid && !inputs.empty()) peer[c] = inputs[0].second;
        }
        if (sink == linkedSink_ && peer[0] == linkedPorts_[0] && peer[1] == linkedPorts_[1]) return;
        for (uint32_t link : links_) s_.Destroy(link);
        links_.clear();
        linkedSink_ = sink;
        linkedPorts_[0] = peer[0];
        linkedPorts_[1] = peer[1];
        if (sink == kInvalid || peer[0] == kInvalid) return;
        for (int c = 0; c < 2; ++c) {
            if (peer[c] == kInvalid) continue;
            uint32_t id = s_.NewId();
            Builder b;
            b.BeginStruct();
            b.Str("link-factory");
            b.Str("PipeWire:Interface:Link");
            b.Int(3);
            b.Dict({{"link.output.node", std::to_string(nodeId_)},
                    {"link.output.port", std::to_string(ours[c])},
                    {"link.input.node", std::to_string(sink)},
                    {"link.input.port", std::to_string(peer[c])},
                    {"object.linger", "false"}});
            b.Int((int32_t)id);
            b.End();
            s_.conn.Send(kCoreId, kCreateObject, b);
            links_.push_back(id);
        }
    }

    // Stops taking part in the graph (Pause or Suspend): releases the nodes waiting on this one.
    void Unprepare() {
        uint32_t old = __atomic_exchange_n(U32(activation_, act::kStatus), act::Inactive, __ATOMIC_SEQ_CST);
        if (old != act::Finished) TriggerTargets(NowNs());
        started_ = false;
    }

    void TriggerTargets(uint64_t now) {
        for (auto& [id, t] : targets_) {
            uint8_t* a = t.map.ptr;
            int32_t* pending = reinterpret_cast<int32_t*>(a + act::kPending);
            if (__atomic_sub_fetch(pending, 1, __ATOMIC_SEQ_CST) != 0) continue;
            uint32_t* status = U32(a, act::kStatus);
            bool fire;
            if (*U32(a, act::kServerVersion) < 1) {
                __atomic_store_n(status, act::Triggered, __ATOMIC_SEQ_CST);
                fire = true;
            } else {
                uint32_t expected = act::NotTriggered;
                fire = __atomic_compare_exchange_n(status, &expected, act::Triggered, false, __ATOMIC_SEQ_CST,
                                                   __ATOMIC_SEQ_CST);
            }
            if (!fire) continue;
            memcpy(a + act::kSignalTime, &now, 8);
            uint64_t one = 1;
            (void)!write(t.fd, &one, sizeof(one));
        }
    }

    // One graph cycle: the server woke this node to fill its output buffers.
    void Process() {
        uint64_t count;
        if (read(rtFd_, &count, sizeof(count)) != sizeof(count) || !activation_) return;
        uint32_t expected = act::Triggered;
        if (!__atomic_compare_exchange_n(U32(activation_, act::kStatus), &expected, act::Awake, false,
                                         __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            PW_TRACE("woken in status %u", expected);
            return;
        }
        if (Tracing() && ++traced_ < 5) PW_TRACE("cycle: %zu targets, %p position", targets_.size(), (void*)position_);
        uint64_t now = NowNs();
        memcpy(activation_ + act::kAwakeTime, &now, 8);
        const uint8_t* pos = position_ ? position_ : activation_ + act::kPosition;
        uint32_t graphRate = Load<uint32_t>(pos + clk::kRateDenom);
        uint64_t duration = Load<uint64_t>(pos + clk::kDuration);
        int64_t delay = Load<int64_t>(pos + clk::kDelay);
        uint32_t cycle = Load<uint32_t>(pos + clk::kCycle);
        uint32_t frames = (uint32_t)std::min<uint64_t>(duration, kMaxFrames);
        if (flush_.exchange(false)) adapter_.Reset();  // what it held counts as played

        if (cb_.timing) {
            double queued = graphRate ? (double)std::max<int64_t>(delay, 0) * params_.rate / graphRate : 0;
            uint64_t held = adapter_.Held(graphRate) + (uint64_t)queued;
            cb_.timing(rendered_ > held ? rendered_ - held : 0, graphRate);
        }
        if (frames) {
            rendered_ += adapter_.Pull(mix_.data(), frames, graphRate, [&](float* out, uint32_t n) {
                if (cb_.render) cb_.render(out, n);
                else memset(out, 0, (size_t)n * 2 * sizeof(float));
            });
            for (uint32_t c = 0; c < 2; ++c) {
                // The server usually hands the buffers to the port as a whole (mix id SPA_ID_INVALID) and
                // gives each link its own io area: every mix points at the same buffer, filled once.
                auto& mixes = ports_[c].mixes;
                auto shared = mixes.find(kInvalid);
                Mix* portMix = shared != mixes.end() && !shared->second.buffers.empty() ? &shared->second : nullptr;
                int32_t portIndex = -1;
                for (auto& [id, mix] : mixes) {
                    if (!mix.io) continue;
                    Mix* owner = !mix.buffers.empty() ? &mix : portMix;
                    if (!owner) continue;
                    uint32_t index;
                    if (owner == portMix && portIndex >= 0) {
                        index = (uint32_t)portIndex;
                    } else {
                        index = owner->next++ % (uint32_t)owner->buffers.size();
                        Buffer& b = owner->buffers[index];
                        uint32_t n = std::min<uint32_t>(frames, b.maxSize / sizeof(float));
                        for (uint32_t i = 0; i < n; ++i) b.data[i] = mix_[i * 2 + c];
                        uint32_t chunk[3] = {0, n * (uint32_t)sizeof(float), (uint32_t)sizeof(float)};
                        memcpy(b.chunk, chunk, sizeof(chunk));
                        if (owner == portMix) portIndex = (int32_t)index;
                    }
                    uint8_t* io = mix.io + (mix.async ? (cycle & 1) * 8 : 0);
                    memcpy(io + 4, &index, 4);
                    __atomic_store_n(reinterpret_cast<int32_t*>(io), kStatusHaveData, __ATOMIC_SEQ_CST);
                }
            }
        }
        now = NowNs();
        uint32_t old = __atomic_exchange_n(U32(activation_, act::kStatus), act::Finished, __ATOMIC_SEQ_CST);
        memcpy(activation_ + act::kFinishTime, &now, 8);
        if (old == act::Awake) TriggerTargets(now);
    }

    sound::Params params_;
    sound::Callbacks cb_;
    std::function<void()> onFailure_;
    std::thread thread_;
    int wakeFd_ = -1;
    std::atomic<bool> quit_{false}, failed_{false}, active_{true}, activeDirty_{false}, flush_{false};
    std::mutex mx_;
    std::condition_variable cv_;
    State state_ = Opening;
    std::string error_;

    // Thread-owned from here on.
    Session s_;
    MemPool pool_;
    uint32_t nodeProxy_ = kInvalid, nodeId_ = kInvalid;
    Mapping activationMap_, positionMap_;
    uint8_t* activation_ = nullptr;
    const uint8_t* position_ = nullptr;
    int rtFd_ = -1;
    bool started_ = false;
    Port ports_[2];
    std::map<uint32_t, Target> targets_;
    std::vector<uint32_t> links_;
    uint32_t linkedSink_ = kInvalid, linkedPorts_[2] = {kInvalid, kInvalid};
    RateAdapter adapter_;
    std::vector<float> mix_;
    uint64_t rendered_ = 0;
    int traced_ = 0;
};

}  // namespace

std::string SocketPath() {
    const char* remote = getenv("PIPEWIRE_REMOTE");
    std::string name = remote && *remote ? remote : "pipewire-0";
    if (name[0] == '/') return name;
    const char* dir = getenv("PIPEWIRE_RUNTIME_DIR");
    if (!dir || !*dir) dir = getenv("XDG_RUNTIME_DIR");
    if (!dir || !*dir) return {};
    return std::string(dir) + "/" + name;
}

std::unique_ptr<sound::Stream> OpenStream(const sound::Params& params, sound::Callbacks callbacks,
                                          std::function<void()> onFailure, std::string& error) {
    auto stream = std::make_unique<PwStream>(params, std::move(callbacks), std::move(onFailure));
    if (!stream->Start(error)) return nullptr;
    return stream;
}

bool ListSinks(std::vector<sound::Sink>& out) {
    out.clear();
    Session s;
    if (!s.Start(ClientProps())) return false;
    // The registry announces every global at once and a sync says when that is all; the metadata
    // bound meanwhile then needs one more round trip for its properties.
    int32_t pending = 1;
    bool finished = false;
    s.Sync(pending);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!finished) {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) break;  // a server that never answers must not hang the window
        pollfd p{s.conn.Fd(), POLLIN, 0};
        if (poll(&p, 1, (int)left.count()) <= 0) continue;
        bool ok = s.conn.Receive([&](Message& m) {
            s.Handle(m, nullptr,
                     [&](int32_t seq) {
                         if (seq != pending) return;
                         if (s.metadataBound && pending == 1) s.Sync(pending = 2);
                         else finished = true;
                     },
                     [] {});
        });
        if (!ok) break;
    }
    if (!finished && s.nodes.empty()) return false;
    for (const auto& [id, n] : s.nodes) {
        if (n.mediaClass != "Audio/Sink" || n.name.empty()) continue;
        out.push_back({n.name, n.description.empty() ? n.name : n.description, n.card, n.name == s.defaultSink});
    }
    return true;
}

int FuzzParse(const uint8_t* data, size_t size) {
    int ok = 0;
    Global g;
    ok += ParseGlobal(data, size, g);
    uint32_t v[2];
    ok += ParseInts(data, size, v, 2);
    CoreError e;
    ok += ParseError(data, size, e);
    AddMem a;
    ok += ParseAddMem(data, size, a);
    MetadataProperty p;
    if (ParseMetadataProperty(data, size, p)) {
        ++ok;
        (void)JsonName(p.value);
    }
    Transport t;
    ok += ParseTransport(data, size, t);
    SetIo io;
    ok += ParseSetIo(data, size, io, false);
    ok += ParseSetIo(data, size, io, true);
    Activation act;
    ok += ParseActivation(data, size, act);
    MixInfo mi;
    ok += ParseMixInfo(data, size, mi);
    uint32_t command;
    ok += ParseCommand(data, size, command);
    PortParam pp;
    if (ParsePortSetParam(data, size, pp)) {
        ++ok;
        (void)IsDspF32(pp.param);
    }
    UseBuffers u;
    ok += ParseUseBuffers(data, size, u);
    // The raw POD walk too: every value, nested structs and objects' properties.
    struct Walk {
        static void Go(Reader r, int depth) {
            while (r.Ok() && !r.AtEnd()) {
                Value v = r.Next();
                if (!r.Ok()) break;
                if (v.type == spa::Struct && depth < 16) Go(Reader(v), depth + 1);
                if (v.type == spa::Object) {
                    spa::ObjectView o = spa::ParseObject(v);
                    int32_t i;
                    uint32_t id;
                    for (uint32_t key = 0; key < 8; ++key) {
                        spa::PropInt(o, key, i);
                        spa::PropId(o, key, id);
                    }
                }
            }
        }
    };
    Walk::Go(Reader(data, size), 0);
    (void)JsonName(std::string(reinterpret_cast<const char*>(data), size));
    return ok;
}

}  // namespace lx::pw
