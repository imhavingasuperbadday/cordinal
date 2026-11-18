// cordinal's own network names (stage 6 of replacing glibc): getaddrinfo and freeaddrinfo, which read
// /etc/hosts and then ask DNS for the nameservers in /etc/resolv.conf; getnameinfo, which turns an
// address back into a name from /etc/hosts or a PTR record; getifaddrs and freeifaddrs over netlink;
// inet_ntop and inet_pton; and gethostname. Hidden like kernel.cpp, so cordinal and the static
// libstdc++ bind to these and glibc keeps its own copies for its own callers.
//
// The sources are the ones /etc/nsswitch.conf names for "hosts", as far as this layer implements
// them: "files" is /etc/hosts and "dns" is the resolver, in the order they appear. Sources it does
// not implement (systemd's mymachines, resolve, myhostname, nis, mdns) are skipped, so the usual
// "hosts: mymachines resolve [!UNAVAIL=return] files myhostname dns" resolves through files and then
// dns. Without an nsswitch.conf the order is files then dns, glibc's own default. The one visible
// effect of skipping a source: nss_myhostname's synthetic localhost (::1 for AF_INET6) is not
// offered, and /etc/hosts answers localhost as it does for every other name. systemd-resolved still
// answers ordinary DNS, because /etc/resolv.conf points at its 127.0.0.53 stub.
//
// DNS follows musl's structure (musl 1.2.5 src/network/getaddrinfo.c, lookup_name.c, lookup_serv.c,
// res_msend.c, dns_parse.c, resolvconf.c, getnameinfo.c, MIT, read November 17, 2026) with glibc's
// answers wherever the two differ, pinned by the libc check against glibc 2.44 (nss/getaddrinfo.c,
// nss/getnameinfo.c, sysdeps/unix/sysv/linux/ifaddrs.c and check_pf.c, nss/nss_files/files-hosts.c,
// read the same day): the hint checks and their EAI_* codes, the socket-type and service table, the
// /etc/hosts family rules (::1 answers AF_INET, a v4-mapped line answers AF_INET, a v4 line answers
// AF_INET6 when AI_V4MAPPED is asked), the end of a failed lookup (EAI_NONAME, EAI_NODATA,
// EAI_AGAIN), and the RFC 3484 order of the results. /etc/gai.conf is not read, so its default label,
// precedence and scope tables apply, and the rules that need interface address flags (deprecated,
// home, native transport, longest prefix) are left out; glibc leaves those out too when it has no
// IPv6 addresses to flag.
//
// Timing: the A and AAAA questions go out together to every nameserver, are repeated on musl's
// schedule (the configured timeout split by the configured attempts, 5 s and 2 by default), and the
// whole DNS step is bounded by the timeout, so a name that resolves nowhere costs at most 5 s rather
// than glibc's timeout times attempts times nameservers. An answer with the truncated bit set is
// fetched again over TCP within the same budget. Callers already run name lookups off the UI thread.
//
// The result of getaddrinfo is one allocation (the entries and the canonical name together), which
// freeaddrinfo frees in one go; getifaddrs' list is one allocation too, with the interface names
// pointing into it as glibc's do.
//
// Three settings exist only for the checks, which cannot edit /etc/hosts or bind port 53:
// CORDINAL_HOSTS names a file read in place of /etc/hosts, CORDINAL_RESOLV_CONF a file read in place
// of /etc/resolv.conf, and CORDINAL_DNS_PORT the port the nameservers are asked on. The program sets
// none of them.
//
// glibc's headers declare the names defined here, and the definitions match them (noexcept where they
// are __THROW), so nothing is repeated by hand.
#include "syscall.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <netdb.h>
#include <netpacket/packet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

// The process-wide allocator (malloc.cpp), the environment (files.cpp) and the C library's strtoul
// (numbers.cpp), declared here because no header for them is included.
extern "C" void* malloc(size_t);
extern "C" void* calloc(size_t, size_t);
extern "C" void* realloc(void*, size_t);
extern "C" void free(void*);
extern "C" char* getenv(const char*);
extern "C" unsigned long strtoul(const char*, char**, int);

using cordlibc::Call;

namespace {

constexpr int kSockDccp = 6;
constexpr int kIpProtocolDccp = 33;
constexpr int kIpProtocolSctp = 132;
constexpr int kIpProtocolUdplite = 136;
constexpr int kIpProtocolMptcp = 262;

constexpr int kMaxAddresses = 16;    // addresses one lookup keeps, as musl's MAXADDRS
constexpr int kMaxServers = 4;       // nameservers read from resolv.conf
constexpr size_t kMaxName = 255;     // the longest name a DNS question or an answer carries
constexpr size_t kSearchSize = 256;  // the search list, as musl and glibc keep it
constexpr int kUdpAnswerSize = 4800;
constexpr int kResolvTimeout = 5;   // glibc's and musl's default, in seconds
constexpr int kResolvAttempts = 2;  // the same
constexpr int kResolvNdots = 1;     // the same
constexpr int kMaxServiceTuples = 8;
constexpr int kMaxDumpSize = 1 << 20;

constexpr int kRrA = 1;
constexpr int kRrCname = 5;
constexpr int kRrPtr = 12;
constexpr int kRrAaaa = 28;
constexpr int kDnsClassIn = 1;
constexpr char kScopeDelimiter = '%';
constexpr unsigned long kSiocgifindex = 0x8933;
constexpr unsigned long kSiocgifname = 0x8910;

// RFC 3484's tables, the defaults glibc uses without /etc/gai.conf.
struct PrefixRule {
    const unsigned char prefix[16];
    unsigned bits;
    int value;
};
constexpr PrefixRule kLabelRules[] = {
    {{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, 128, 0},  // ::1
    {{0x20, 0x02}, 16, 2},                                        // 2002::/16 (6to4)
    {{}, 96, 3},                                                 // ::/96 (v4-compatible)
    {{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff}, 96, 4},         // ::ffff:0:0/96 (v4-mapped)
    {{0xfe, 0xc0}, 10, 5},                                        // fec0::/10 (site-local)
    {{0xfc}, 7, 6},                                               // fc00::/7 (unique local)
    {{0x20, 0x01}, 32, 7},                                        // 2001::/32 (Teredo)
    {{}, 0, 1},                                                   // everything else
};
constexpr PrefixRule kPrecedenceRules[] = {
    {{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, 128, 50},  // ::1
    {{0x20, 0x02}, 16, 30},                                          // 2002::/16
    {{}, 96, 20},                                                   // ::/96
    {{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff}, 96, 10},           // ::ffff:0:0/96
    {{}, 0, 40},                                                    // everything else
};
struct ScopeRule {
    const unsigned char address[4];
    uint32_t mask;
    int scope;
};
constexpr ScopeRule kScopeRules[] = {
    {{127, 0, 0, 0}, 0xff000000u, 2},   // loopback, which is treated as link-local
    {{169, 254, 0, 0}, 0xffff0000u, 2},  // link-local
    {{0, 0, 0, 0}, 0, 14},               // everything else
};

// An address one of the lookups found, in network order.
struct FoundAddress {
    int family;
    uint8_t bytes[16];
    uint32_t scope;
};

// What a name lookup found, or why it found nothing.
struct LookupResult {
    int status;  // 0 when addresses were found, otherwise an EAI_* code
    int count;
    FoundAddress addresses[kMaxAddresses];
    char canon[kMaxName + 1];
};

struct ResolverConfig {
    int serverCount;
    struct {
        int family;
        uint8_t bytes[16];
    } servers[kMaxServers];
    char search[kSearchSize];
    int ndots, timeout, attempts;
};

constexpr int kAnswerNone = 0;      // nothing yet
constexpr int kAnswerGot = 1;       // a whole UDP answer
constexpr int kAnswerNeedsTcp = 2;  // truncated; the TCP fetch decides
constexpr int kAnswerServfail = 3;  // the server could not answer

// The questions in flight and the answers they brought.
struct DnsAnswers {
    uint8_t udp[2][kUdpAnswerSize];
    int udpLength[2];
    int truncatedBy[2];  // the server whose truncated answer needs a TCP fetch, or -1
    uint8_t* tcp[2];     // an answer fetched over TCP (malloc'd), or nullptr
    int status[2];
    int rcode[2];
    int questionCount;
};

// One parsed answer: its return code, the addresses of the type asked for, and the name it points at
// (a CNAME's target, or a PTR record's name).
struct ParsedAnswer {
    int rcode;
    int count;
    FoundAddress addresses[kMaxAddresses];
    char name[kMaxName + 1];
};

// Byte order, written out because htons and ntohl are glibc's functions.
constexpr uint16_t Swap16(uint16_t value) { return (uint16_t)(value >> 8 | value << 8); }
constexpr uint16_t Net16(uint16_t value) { return Swap16(value); }
constexpr uint16_t Host16(uint16_t value) { return Swap16(value); }

uint16_t Get16(const uint8_t* at) { return (uint16_t)(at[0] << 8 | at[1]); }
void Put16(uint8_t* at, uint16_t value) {
    at[0] = (uint8_t)(value >> 8);
    at[1] = (uint8_t)value;
}

// Small text helpers.
bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }
int LowerAscii(int c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }
bool EqualsNoCase(const char* a, const char* b) {
    while (*a && LowerAscii((unsigned char)*a) == LowerAscii((unsigned char)*b)) ++a, ++b;
    return *a == *b;
}

// Reads a file a line at a time, handing each line (its newline removed) to `visit`; visit returns
// false to stop. A line longer than the buffer is passed in pieces, which every caller tolerates
// because it reads words. Returns false when the file cannot be opened or read.
template <class Visit>
bool ForEachLine(const char* path, Visit visit) {
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char buffer[8192];
    size_t carried = 0;
    bool readFailed = false;
    for (;;) {
        const long got = read(fd, buffer + carried, sizeof buffer - carried - 1);
        if (got < 0) {
            if (errno == EINTR) continue;
            readFailed = true;
            break;
        }
        if (got == 0) break;
        const size_t total = carried + (size_t)got;
        buffer[total] = 0;
        size_t start = 0;
        for (size_t i = 0; i < total; ++i) {
            if (buffer[i] != '\n') continue;
            buffer[i] = 0;
            if (i > start && buffer[i - 1] == '\r') buffer[i - 1] = 0;
            if (!visit(buffer + start)) {
                close(fd);
                return true;
            }
            start = i + 1;
        }
        carried = total - start;
        if (start != 0) memmove(buffer, buffer + start, carried);
        if (carried >= sizeof buffer - 1) {
            buffer[carried] = 0;
            if (!visit(buffer)) {
                close(fd);
                return true;
            }
            carried = 0;
        }
    }
    if (!readFailed && carried) {
        buffer[carried] = 0;
        visit(buffer);
    }
    close(fd);
    return !readFailed;
}

// The words of a line, in order.
class Fields {
  public:
    explicit Fields(char* line) : at_(line) { Next(); }
    bool Done() const { return current_ == nullptr; }
    char* Current() const { return current_; }
    char* Take() {
        char* word = current_;
        Next();
        return word;
    }
    void Next() {
        while (*at_ && IsSpace(*at_)) ++at_;
        if (!*at_) {
            current_ = nullptr;
            return;
        }
        current_ = at_;
        while (*at_ && !IsSpace(*at_)) ++at_;
        if (*at_) *at_++ = 0;
    }

  private:
    char* at_;
    char* current_ = nullptr;
};

// A copy that reports whether it fitted, as glibc's checked_copy does.
int CopyText(char* out, size_t size, const char* text) {
    const size_t length = strlen(text);
    if (length + 1 > size) return EAI_OVERFLOW;
    memcpy(out, text, length + 1);
    return 0;
}

// A count in decimal, written without a printf.
int FormatCount(char* out, size_t size, unsigned long value) {
    char digits[24];
    int count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    if ((size_t)count + 1 > size) return EAI_OVERFLOW;
    for (int i = 0; i < count; ++i) out[i] = digits[count - 1 - i];
    out[count] = 0;
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Address text: inet_pton and inet_ntop, and the inet_aton grammar getaddrinfo reads a numeric host
// with (glibc's __inet_aton_exact: 010.0.0.1 is 8.0.0.1, 127.1 is 127.0.0.1, 127 is 0.0.0.127).

// A dotted quad: four decimal parts, no leading zeros, as inet_pton(AF_INET) reads it.
bool ParseDottedQuad(const char* text, uint8_t out[4]) {
    for (int part = 0; part < 4; ++part) {
        const char* digits = text;
        unsigned value = 0;
        int count = 0;
        while (*text >= '0' && *text <= '9') {
            value = value * 10 + (unsigned)(*text - '0');
            if (value > 255) return false;
            ++text;
            ++count;
        }
        if (count == 0 || (count > 1 && *digits == '0')) return false;
        out[part] = (uint8_t)value;
        if (part < 3) {
            if (*text != '.') return false;
            ++text;
        }
    }
    return *text == 0;
}

// An inet_aton number: hexadecimal after 0x, octal after a leading 0, decimal otherwise.
bool ParseAtonNumber(const char*& text, uint32_t* value) {
    const char* at = text;
    unsigned base = 10;
    if (at[0] == '0' && (at[1] == 'x' || at[1] == 'X')) {
        base = 16;
        at += 2;
    } else if (at[0] == '0') {
        base = 8;
    }
    const char* digits = at;
    uint64_t result = 0;
    for (;;) {
        const char c = *at;
        unsigned digit;
        if (c >= '0' && c <= '9') digit = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') digit = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') digit = (unsigned)(c - 'A' + 10);
        else break;
        if (digit >= base) return false;
        result = result * base + digit;
        if (result > 0xffffffffull) return false;
        ++at;
    }
    if (at == digits) return false;
    *value = (uint32_t)result;
    text = at;
    return true;
}

bool ParseLenientIPv4(const char* text, uint8_t out[4]) {
    uint32_t parts[4] = {};
    int count = 0;
    for (;;) {
        if (count == 4 || !ParseAtonNumber(text, &parts[count])) return false;
        ++count;
        if (*text != '.') break;
        ++text;
    }
    if (*text != 0) return false;
    for (int i = 0; i < count - 1; ++i)
        if (parts[i] > 0xff) return false;
    uint64_t lastLimit = 1;
    for (int i = count; i <= 4; ++i) lastLimit <<= 8;
    if (parts[count - 1] >= lastLimit) return false;
    // The parts before the last take one byte each; the last one takes every byte left over, so
    // 127.0.0.1 is four bytes and 127.1, 127.0.256 and 127 are one to three.
    uint64_t value = 0;
    for (int i = 0; i < count - 1; ++i) value = value << 8 | parts[i];
    value = value << (8 * (5 - count)) | parts[count - 1];
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
    return true;
}

// The groups of an IPv6 address with "::" and a trailing dotted quad, as inet_pton(AF_INET6) reads
// them: one to four hex digits per group, the quad only last and only where two groups fit.
bool ParseIPv6(const char* text, uint8_t out[16]) {
    uint16_t groups[8] = {};
    int count = 0, gap = -1;
    const char* at = text;
    if (*at == ':') {
        if (at[1] != ':') return false;
        gap = 0;
        at += 2;
    }
    while (*at) {
        if (count == 8) return false;
        const char* colon = strchr(at, ':');
        const char* dot = strchr(at, '.');
        if (dot && (!colon || dot < colon)) {
            uint8_t quad[4];
            if (count > 6 || !ParseDottedQuad(at, quad)) return false;
            groups[count++] = (uint16_t)(quad[0] << 8 | quad[1]);
            groups[count++] = (uint16_t)(quad[2] << 8 | quad[3]);
            break;
        }
        unsigned value = 0;
        int digits = 0;
        while (digits < 5) {
            const char c = at[digits];
            const int digit = c >= '0' && c <= '9' ? c - '0'
                              : c >= 'a' && c <= 'f' ? c - 'a' + 10
                              : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                     : -1;
            if (digit < 0) break;
            value = value * 16 + (unsigned)digit;
            ++digits;
        }
        if (digits < 1 || digits > 4) return false;
        groups[count++] = (uint16_t)value;
        at += digits;
        if (*at == 0) break;
        if (*at != ':') return false;
        ++at;
        if (*at == 0) return false;  // a single trailing colon has no group after it
        if (*at == ':') {
            if (gap >= 0) return false;
            gap = count;
            ++at;
            if (*at == 0) break;
        }
    }
    if (count > 8) return false;
    // "::" has to stand for at least one group, so an address that already holds eight cannot use it.
    if (gap >= 0 && count == 8) return false;
    if (gap < 0) {
        if (count != 8) return false;
        for (int i = 0; i < 8; ++i) {
            out[i * 2] = (uint8_t)(groups[i] >> 8);
            out[i * 2 + 1] = (uint8_t)groups[i];
        }
        return true;
    }
    memset(out, 0, 16);
    for (int i = 0; i < gap; ++i) {
        out[i * 2] = (uint8_t)(groups[i] >> 8);
        out[i * 2 + 1] = (uint8_t)groups[i];
    }
    const int trailing = count - gap;
    for (int i = 0; i < trailing; ++i) {
        const int target = 7 - (trailing - 1 - i);
        out[target * 2] = (uint8_t)(groups[gap + i] >> 8);
        out[target * 2 + 1] = (uint8_t)groups[gap + i];
    }
    return true;
}

bool IsV4Mapped(const uint8_t bytes[16]) {
    for (int i = 0; i < 10; ++i)
        if (bytes[i]) return false;
    return bytes[10] == 0xff && bytes[11] == 0xff;
}
bool IsIPv6Loopback(const uint8_t bytes[16]) {
    for (int i = 0; i < 15; ++i)
        if (bytes[i]) return false;
    return bytes[15] == 1;
}
void MapV4IntoV6(const uint8_t v4[4], uint8_t v6[16]) {
    memset(v6, 0, 16);
    v6[10] = 0xff;
    v6[11] = 0xff;
    memcpy(v6 + 12, v4, 4);
}

// An interface's index from its name, 0 when there is none; and the other way round.
struct IfReq {
    char name[IFNAMSIZ];
    union {
        int index;
        char room[24];
    } value;
};

unsigned IndexOfInterface(const char* name) {
    if (!*name) return 0;
    if (*name >= '0' && *name <= '9') {
        unsigned value = 0;
        for (const char* at = name; *at; ++at) {
            if (*at < '0' || *at > '9' || value > 0x7ffffff) return 0;
            value = value * 10 + (unsigned)(*at - '0');
        }
        return value;
    }
    const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return 0;
    IfReq request = {};
    strncpy(request.name, name, IFNAMSIZ - 1);
    unsigned result = 0;
    if (ioctl(fd, kSiocgifindex, &request) == 0 && request.value.index > 0)
        result = (unsigned)request.value.index;
    close(fd);
    return result;
}

bool NameOfInterface(unsigned index, char out[IFNAMSIZ + 1]) {
    const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    IfReq request = {};
    request.value.index = (int)index;
    const bool found = ioctl(fd, kSiocgifname, &request) == 0 && request.name[0];
    close(fd);
    if (!found) return false;
    memset(out, 0, IFNAMSIZ + 1);
    strncpy(out, request.name, IFNAMSIZ);
    return true;
}

// The text of a v4 address, written whole when it fits: glibc formats into a temporary and answers
// NULL with ENOSPC when the caller's buffer is too small.
int FormatIPv4(const uint8_t* bytes, char* out, size_t size) {
    char text[sizeof "255.255.255.255"];
    size_t at = 0;
    for (int part = 0; part < 4; ++part) {
        unsigned value = bytes[part];
        char digits[3];
        int count = 0;
        do {
            digits[count++] = (char)('0' + value % 10);
            value /= 10;
        } while (value);
        if (part) text[at++] = '.';
        while (count) text[at++] = digits[--count];
    }
    text[at] = 0;
    if (at >= size) {
        errno = ENOSPC;
        return -1;
    }
    memcpy(out, text, at + 1);
    return (int)at;
}

// glibc's own form (inet_ntop6, resolv/inet_ntop.c, read November 18, 2026): lowercase, no leading
// zeros, the first longest run of two or more zero groups compressed, and the last 32 bits dotted
// only where the compression starts the address and leaves exactly the v4-compatible form with a
// nonzero third group (::a.b.c.d) or the v4-mapped one (::ffff:a.b.c.d). So ::1 stays "::1" and
// ::0.0.0.2 prints "::2", where a plain "the first 96 bits are zero" test would print a needless
// dotted quad.
int FormatIPv6(const uint8_t* bytes, char* out, size_t size) {
    char text[sizeof "ffff:ffff:ffff:ffff:ffff:ffff:255.255.255.255"];
    uint16_t groups[8];
    for (int i = 0; i < 8; ++i) groups[i] = (uint16_t)(bytes[i * 2] << 8 | bytes[i * 2 + 1]);
    int bestStart = -1, bestLength = 0;
    for (int i = 0; i < 8;) {
        if (groups[i] != 0) {
            ++i;
            continue;
        }
        int end = i;
        while (end < 8 && groups[end] == 0) ++end;
        if (bestStart < 0 || end - i > bestLength) {
            bestStart = i;
            bestLength = end - i;
        }
        i = end;
    }
    if (bestLength < 2) bestStart = -1;
    static const char kHex[] = "0123456789abcdef";
    size_t at = 0;
    for (int i = 0; i < 8; ++i) {
        if (bestStart >= 0 && i >= bestStart && i < bestStart + bestLength) {
            if (i == bestStart) text[at++] = ':';
            continue;
        }
        if (i != 0) text[at++] = ':';
        if (i == 6 && bestStart == 0 && (bestLength == 6 || (bestLength == 5 && groups[5] == 0xffff))) {
            for (int part = 12; part < 16; ++part) {
                if (part > 12) text[at++] = '.';
                const unsigned value = bytes[part];
                if (value >= 100) text[at++] = (char)('0' + value / 100);
                if (value >= 10) text[at++] = (char)('0' + (value / 10) % 10);
                text[at++] = (char)('0' + value % 10);
            }
            break;
        }
        unsigned value = groups[i];
        char digits[4];
        int count = 0;
        do {
            digits[count++] = kHex[value & 15];
            value >>= 4;
        } while (value);
        while (count) text[at++] = digits[--count];
    }
    if (bestStart >= 0 && bestStart + bestLength == 8) text[at++] = ':';
    text[at] = 0;
    if (at >= size) {
        errno = ENOSPC;
        return -1;
    }
    memcpy(out, text, at + 1);
    return (int)at;
}

// ---------------------------------------------------------------------------------------------
// /etc/hosts.

constexpr const char* kHostsPath = "/etc/hosts";
constexpr const char* kNsswitchPath = "/etc/nsswitch.conf";
constexpr const char* kServicesPath = "/etc/services";
constexpr const char* kResolvPath = "/etc/resolv.conf";

// /etc/hosts, or the file CORDINAL_HOSTS names, which only the checks set: they need lines of their
// own and cannot edit the system's file.
const char* HostsPath() {
    const char* override = getenv("CORDINAL_HOSTS");
    return override && *override ? override : kHostsPath;
}

// The address of a hosts line in the family asked for, with glibc's three translations: a v4 line is
// a v4-mapped address under AF_INET6 with AI_V4MAPPED, a v4-mapped v6 line is its v4 address under
// AF_INET, and a v6 line's ::1 is 127.0.0.1 under AF_INET.
bool HostsLineAddress(const char* text, int family, int flags, int* outFamily, uint8_t out[16]) {
    uint8_t bytes[16] = {};
    if (ParseDottedQuad(text, bytes)) {
        if (family == AF_INET || family == AF_UNSPEC) {
            *outFamily = AF_INET;
            memcpy(out, bytes, 4);
            return true;
        }
        if (family == AF_INET6 && (flags & AI_V4MAPPED)) {
            *outFamily = AF_INET6;
            MapV4IntoV6(bytes, out);
            return true;
        }
        return false;
    }
    if (!ParseIPv6(text, bytes)) return false;
    if (family == AF_INET6 || family == AF_UNSPEC) {
        *outFamily = AF_INET6;
        memcpy(out, bytes, 16);
        return true;
    }
    if (family != AF_INET) return false;
    if (IsV4Mapped(bytes)) {
        *outFamily = AF_INET;
        memcpy(out, bytes + 12, 4);
        return true;
    }
    if (IsIPv6Loopback(bytes)) {
        *outFamily = AF_INET;
        memset(out, 0, 16);
        out[0] = 127;
        out[3] = 1;
        return true;
    }
    return false;
}

// Every hosts line whose canonical name or any alias is `name`, as glibc's gethostbyname4_r reads the
// file: one address per matching line in file order, and the first line's first name as the canonical
// one.
LookupResult LookupHostsFile(const char* name, int family, int flags) {
    LookupResult result = {};
    ForEachLine(HostsPath(), [&](char* line) {
        if (char* comment = strchr(line, '#')) *comment = 0;
        Fields fields(line);
        if (fields.Done()) return true;
        const char* addressText = fields.Take();
        if (fields.Done()) return true;
        int addressFamily = AF_INET;
        uint8_t bytes[16] = {};
        if (!HostsLineAddress(addressText, family, flags, &addressFamily, bytes)) return true;
        const char* canonical = nullptr;
        bool match = false;
        for (; !fields.Done(); fields.Next()) {
            if (!canonical) canonical = fields.Current();
            if (EqualsNoCase(fields.Current(), name)) match = true;
        }
        if (!match) return true;
        if (result.count == 0 && canonical) CopyText(result.canon, sizeof result.canon, canonical);
        if (result.count == kMaxAddresses) return false;
        FoundAddress& found = result.addresses[result.count++];
        found = {};
        found.family = addressFamily;
        memset(found.bytes, 0, 16);
        memcpy(found.bytes, bytes, addressFamily == AF_INET ? 4 : 16);
        return true;
    });
    return result;
}

// The first name on the line whose address is `address`, as glibc's gethostbyaddr reads the file. The
// addresses are compared in their 16-byte form, so a v4 line answers a v4-mapped v6 address and the
// other way round.
bool ReverseHostsFile(const uint8_t* address, int family, char* out, size_t size) {
    uint8_t wanted[16] = {};
    if (family == AF_INET) MapV4IntoV6(address, wanted);
    else memcpy(wanted, address, 16);
    bool found = false;
    ForEachLine(HostsPath(), [&](char* line) {
        if (char* comment = strchr(line, '#')) *comment = 0;
        Fields fields(line);
        if (fields.Done()) return true;
        const char* addressText = fields.Take();
        if (fields.Done()) return true;
        uint8_t bytes[16] = {};
        int lineFamily = AF_INET;
        if (!HostsLineAddress(addressText, AF_UNSPEC, 0, &lineFamily, bytes)) return true;
        uint8_t mapped[16];
        if (lineFamily == AF_INET) {
            MapV4IntoV6(bytes, mapped);
        } else {
            memcpy(mapped, bytes, 16);
        }
        if (memcmp(mapped, wanted, 16)) return true;
        CopyText(out, size, fields.Current());
        found = true;
        return false;
    });
    return found;
}

// ---------------------------------------------------------------------------------------------
// /etc/services: the port of a name and protocol, and the name of a port and protocol.

bool ServicePort(const char* name, const char* protocol, uint16_t* port) {
    bool found = false;
    ForEachLine(kServicesPath, [&](char* line) {
        if (char* comment = strchr(line, '#')) *comment = 0;
        Fields fields(line);
        const char* serviceName = fields.Take();
        char* portAndProtocol = fields.Take();
        if (!serviceName || !portAndProtocol) return true;
        bool named = EqualsNoCase(serviceName, name);
        for (; !named && !fields.Done(); fields.Next()) named = EqualsNoCase(fields.Current(), name);
        if (!named) return true;
        char* slash = strchr(portAndProtocol, '/');
        if (!slash) return true;
        *slash = 0;
        if (!EqualsNoCase(slash + 1, protocol)) return true;
        unsigned value = 0;
        for (const char* at = portAndProtocol; *at; ++at) {
            if (*at < '0' || *at > '9') return true;
            value = value * 10 + (unsigned)(*at - '0');
            if (value > 65535) return true;
        }
        *port = Net16((uint16_t)value);
        found = true;
        return false;
    });
    return found;
}

bool ServiceName(uint16_t port, const char* protocol, char* out, size_t size) {
    bool found = false;
    const unsigned wanted = Host16(port);
    ForEachLine(kServicesPath, [&](char* line) {
        if (char* comment = strchr(line, '#')) *comment = 0;
        Fields fields(line);
        const char* serviceName = fields.Take();
        char* portAndProtocol = fields.Take();
        if (!serviceName || !portAndProtocol) return true;
        char* slash = strchr(portAndProtocol, '/');
        if (!slash) return true;
        *slash = 0;
        if (!EqualsNoCase(slash + 1, protocol)) return true;
        unsigned value = 0;
        for (const char* at = portAndProtocol; *at; ++at) {
            if (*at < '0' || *at > '9') return true;
            value = value * 10 + (unsigned)(*at - '0');
            if (value > 65535) return true;
        }
        if (value != wanted) return true;
        CopyText(out, size, serviceName);
        found = true;
        return false;
    });
    return found;
}

// ---------------------------------------------------------------------------------------------
// /etc/nsswitch.conf: which source answers "hosts", in the order it names. Only files and dns are
// implemented, so a line's other modules are skipped and the two that remain keep their order.

constexpr int kSourceFiles = 1;
constexpr int kSourceDns = 2;

int HostsSourceOrder(int out[2]) {
    int count = 0;
    bool files = false, dns = false;
    ForEachLine(kNsswitchPath, [&](char* line) {
        if (char* comment = strchr(line, '#')) *comment = 0;
        Fields fields(line);
        char* database = fields.Take();
        if (!database) return true;
        const size_t length = strlen(database);
        if (length > 0 && database[length - 1] == ':') database[length - 1] = 0;
        if (strcmp(database, "hosts")) return true;
        if (!fields.Done() && !strcmp(fields.Current(), ":")) fields.Next();
        for (; !fields.Done(); fields.Next()) {
            char* word = fields.Current();
            if (*word == '[' || strchr(word, '=')) continue;  // an action, not a source
            if (!strcmp(word, "files") && !files) {
                files = true;
                out[count++] = kSourceFiles;
            } else if (!strcmp(word, "dns") && !dns) {
                dns = true;
                out[count++] = kSourceDns;
            }
        }
        return false;
    });
    if (count == 0) {
        out[0] = kSourceFiles;
        out[1] = kSourceDns;
        return 2;
    }
    return count;
}

// ---------------------------------------------------------------------------------------------
// /etc/resolv.conf.

uint16_t DnsPort() {
    const char* text = getenv("CORDINAL_DNS_PORT");
    if (!text || !*text) return 53;
    unsigned value = 0;
    for (const char* at = text; *at; ++at) {
        if (*at < '0' || *at > '9') return 53;
        value = value * 10 + (unsigned)(*at - '0');
        if (value > 65535) return 53;
    }
    return (uint16_t)value;
}

// The nameservers, search list, ndots, timeout and attempts, as glibc reads them: nameserver lines
// name an address (v4 or v6, with an optional scope), search and domain set the list (the last one
// wins), and options carries ndots, timeout and attempts. The other options (rotate, edns0,
// single-request, ...) change how queries are sent rather than what they answer, and are not read.
void ReadResolverConfig(ResolverConfig* config) {
    config->serverCount = 0;
    config->search[0] = 0;
    config->ndots = kResolvNdots;
    config->timeout = kResolvTimeout;
    config->attempts = kResolvAttempts;
    const char* override = getenv("CORDINAL_RESOLV_CONF");
    const char* path = override && *override ? override : kResolvPath;
    ForEachLine(path, [&](char* line) {
        char* comment = strchr(line, '#');
        if (!comment) comment = strchr(line, ';');
        if (comment) *comment = 0;
        Fields fields(line);
        const char* keyword = fields.Take();
        if (!keyword) return true;
        if (!strcmp(keyword, "nameserver")) {
            char* text = fields.Take();
            if (!text || config->serverCount >= kMaxServers) return true;
            uint8_t bytes[16] = {};
            char* scope = strchr(text, kScopeDelimiter);
            if (scope) *scope = 0;
            if (ParseDottedQuad(text, bytes)) {
                config->servers[config->serverCount].family = AF_INET;
                memcpy(config->servers[config->serverCount].bytes, bytes, 4);
                ++config->serverCount;
            } else if (ParseIPv6(text, bytes)) {
                config->servers[config->serverCount].family = AF_INET6;
                memcpy(config->servers[config->serverCount].bytes, bytes, 16);
                ++config->serverCount;
            }
            return true;
        }
        if (!strcmp(keyword, "search") || !strcmp(keyword, "domain")) {
            size_t used = 0;
            for (; !fields.Done(); fields.Next()) {
                const char* word = fields.Current();
                const size_t length = strlen(word);
                if (used + length + 2 > sizeof config->search) break;
                if (used) config->search[used++] = ' ';
                memcpy(config->search + used, word, length);
                used += length;
                config->search[used] = 0;
            }
            return true;
        }
        if (!strcmp(keyword, "options")) {
            for (; !fields.Done(); fields.Next()) {
                const char* option = fields.Current();
                int* target = nullptr;
                size_t skip = 0;
                if (!strncmp(option, "ndots:", 6)) {
                    target = &config->ndots;
                    skip = 6;
                } else if (!strncmp(option, "timeout:", 8)) {
                    target = &config->timeout;
                    skip = 8;
                } else if (!strncmp(option, "attempts:", 9)) {
                    target = &config->attempts;
                    skip = 9;
                }
                if (!target) continue;
                const char* at = option + skip;
                if (*at < '0' || *at > '9') continue;
                int value = 0;
                for (; *at >= '0' && *at <= '9'; ++at) {
                    value = value * 10 + (*at - '0');
                    if (value > 3600) value = 3600;
                }
                *target = value;
            }
            return true;
        }
        return true;
    });
    if (config->ndots > 15) config->ndots = 15;
    if (config->timeout < 1) config->timeout = 1;
    if (config->timeout > 60) config->timeout = 60;
    if (config->attempts < 1) config->attempts = 1;
    if (config->attempts > 10) config->attempts = 10;
    if (config->serverCount == 0) {
        config->servers[0].family = AF_INET;
        memset(config->servers[0].bytes, 0, 16);
        config->servers[0].bytes[0] = 127;
        config->servers[0].bytes[3] = 1;
        config->serverCount = 1;
    }
}

// ---------------------------------------------------------------------------------------------
// DNS.

uint64_t MonotonicMs() {
    struct timespec ts = {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

// A question's identifier: two bytes from the kernel, as glibc and musl take theirs.
uint16_t QuestionId() {
    uint16_t value = 0;
    if (Call(__NR_getrandom, &value, sizeof value, 0) != (long)sizeof value) value = (uint16_t)MonotonicMs();
    return value;
}

// A DNS question: the header, the name as labels, then the type and class.
int BuildQuestion(uint8_t* packet, int size, const char* name, int type, uint16_t id) {
    if (size < 17) return -1;
    memset(packet, 0, 12);
    Put16(packet, id);
    packet[2] = 0x01;  // recursion desired
    Put16(packet + 4, 1);
    int at = 12;
    const char* label = name;
    for (;;) {
        const char* dot = strchr(label, '.');
        const size_t length = dot ? (size_t)(dot - label) : strlen(label);
        if (length == 0 || length > 63) return -1;
        if (at + 1 + (int)length + 5 > size) return -1;
        packet[at++] = (uint8_t)length;
        memcpy(packet + at, label, length);
        at += (int)length;
        if (!dot) break;
        label = dot + 1;
    }
    packet[at++] = 0;
    Put16(packet + at, (uint16_t)type);
    at += 2;
    Put16(packet + at, kDnsClassIn);
    at += 2;
    return at;
}

// A name from an answer, following compression pointers. A pointer has to go backwards, so a loop
// cannot form.
bool ReadDnsName(const uint8_t* packet, int length, int& at, char* out, size_t size) {
    int position = at;
    int end = -1;
    size_t used = 0;
    int jumps = 0;
    for (;;) {
        if (position >= length) return false;
        const uint8_t label = packet[position];
        if (label == 0) {
            if (used == 0) out[used++] = '.';
            if (used >= size) return false;
            out[used] = 0;
            at = end < 0 ? position + 1 : end;
            return true;
        }
        if ((label & 0xc0) == 0xc0) {
            if (position + 1 >= length || ++jumps > 16) return false;
            const int target = ((label & 0x3f) << 8) | packet[position + 1];
            if (target >= position) return false;
            if (end < 0) end = position + 2;
            position = target;
            continue;
        }
        if (label > 63 || position + 1 + label > length) return false;
        if (used + label + 2 > size) return false;
        if (used) out[used++] = '.';
        memcpy(out + used, packet + position + 1, label);
        used += label;
        position += 1 + label;
    }
}

// The records of one answer that matter: the addresses of the type asked for, and the name a CNAME or
// a PTR record points at. Every record of the type counts, whatever name it carries, which is how
// both glibc and musl read a chain the server has already followed.
bool ParseDnsAnswer(const uint8_t* packet, int length, int type, ParsedAnswer* parsed) {
    *parsed = {};
    if (length < 12) return false;
    parsed->rcode = packet[3] & 0x0f;
    if (parsed->rcode != 0) return true;
    const int questions = Get16(packet + 4);
    const int records = Get16(packet + 6);
    int at = 12;
    char name[kMaxName + 1];
    for (int i = 0; i < questions; ++i) {
        if (!ReadDnsName(packet, length, at, name, sizeof name)) return false;
        at += 4;
        if (at > length) return false;
    }
    for (int i = 0; i < records; ++i) {
        if (!ReadDnsName(packet, length, at, name, sizeof name)) return false;
        if (at + 10 > length) return false;
        const int recordType = Get16(packet + at);
        const int recordLength = Get16(packet + at + 8);
        const int dataAt = at + 10;
        if (dataAt + recordLength > length) return false;
        if (recordType == kRrCname || recordType == kRrPtr) {
            int position = dataAt;
            char target[kMaxName + 1];
            if (ReadDnsName(packet, length, position, target, sizeof target))
                CopyText(parsed->name, sizeof parsed->name, target);
        } else if (recordType == type && parsed->count < kMaxAddresses) {
            if (type == kRrA && recordLength == 4) {
                FoundAddress& found = parsed->addresses[parsed->count++];
                found.family = AF_INET;
                found.scope = 0;
                memset(found.bytes, 0, 16);
                memcpy(found.bytes, packet + dataAt, 4);
            } else if (type == kRrAaaa && recordLength == 16) {
                FoundAddress& found = parsed->addresses[parsed->count++];
                found.family = AF_INET6;
                found.scope = 0;
                memcpy(found.bytes, packet + dataAt, 16);
            }
        }
        at = dataAt + recordLength;
    }
    return true;
}

// The exchange with the nameservers: every question to every server, repeated on musl's schedule,
// then a truncated answer fetched again over TCP. Everything is kept inside one deadline, counted in
// MonotonicMs' milliseconds.
class DnsSession {
  public:
    DnsSession(const ResolverConfig& config, uint64_t deadline) : config_(config), deadline_(deadline) {}
    ~DnsSession() {
        for (int i = 0; i < 2; ++i)
            if (udp_[i] >= 0) close(udp_[i]);
    }
    void Exchange(const uint8_t questions[2][280], const int lengths[2], int count, DnsAnswers* answers) {
        *answers = {};
        answers->questionCount = count;
        for (int i = 0; i < 2; ++i) answers->truncatedBy[i] = -1;
        const uint16_t port = DnsPort();
        const int interval = config_.timeout * 1000 / config_.attempts;
        uint64_t nextSend = 0;
        int sends = 0;
        int servfailRetries[2] = {2, 2};
        for (;;) {
            bool pending = false;
            for (int i = 0; i < count; ++i)
                if (answers->status[i] == kAnswerNone) pending = true;
            if (!pending) break;
            const uint64_t now = MonotonicMs();
            if (now >= deadline_) break;
            if (now >= nextSend && sends < config_.attempts) {
                for (int i = 0; i < count; ++i)
                    if (answers->status[i] == kAnswerNone)
                        for (int server = 0; server < config_.serverCount; ++server)
                            SendTo(server, questions[i], lengths[i], port);
                ++sends;
                nextSend = now + (uint64_t)interval;
            }
            struct pollfd fds[2] = {};
            int fdCount = 0;
            for (int family = 0; family < 2; ++family)
                if (udp_[family] >= 0) fds[fdCount++] = {udp_[family], POLLIN, 0};
            if (fdCount == 0) break;
            // Wake for the next round while there is one, and otherwise only for the deadline.
            const uint64_t until = sends < config_.attempts && nextSend < deadline_ ? nextSend : deadline_;
            int wait = (int)(until - now);
            if (wait < 0) wait = 0;
            if (poll(fds, (unsigned long)fdCount, wait) <= 0) continue;
            for (int i = 0; i < fdCount; ++i)
                if (fds[i].revents & POLLIN) Receive(fds[i].fd, count, questions, lengths, answers, servfailRetries);
        }
        for (int i = 0; i < count; ++i) {
            if (answers->status[i] != kAnswerNeedsTcp) continue;
            answers->tcp[i] = FetchOverTcp(answers->truncatedBy[i], questions[i], lengths[i], &answers->udpLength[i]);
            answers->status[i] = answers->tcp[i] ? kAnswerGot : kAnswerServfail;
        }
        for (int i = 0; i < count; ++i)
            if (answers->status[i] == kAnswerNone) answers->status[i] = kAnswerServfail;
    }

  private:
    int SocketFor(int server) {
        const int slot = config_.servers[server].family == AF_INET6 ? 1 : 0;
        if (udp_[slot] >= 0) return udp_[slot];
        const int fd = socket(config_.servers[server].family, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (fd >= 0) udp_[slot] = fd;
        return fd;
    }
    unsigned NameserverAddress(int server, uint16_t port, struct sockaddr_storage* out) {
        memset(out, 0, sizeof *out);
        if (config_.servers[server].family == AF_INET) {
            auto* address = (struct sockaddr_in*)out;
            address->sin_family = AF_INET;
            address->sin_port = Net16(port);  // DnsPort counts in host order
            memcpy(&address->sin_addr, config_.servers[server].bytes, 4);
            return sizeof *address;
        }
        auto* address = (struct sockaddr_in6*)out;
        address->sin6_family = AF_INET6;
        address->sin6_port = Net16(port);
        memcpy(&address->sin6_addr, config_.servers[server].bytes, 16);
        return sizeof *address;
    }
    void SendTo(int server, const uint8_t* question, int length, uint16_t port) {
        if (server < 0) return;
        const int fd = SocketFor(server);
        if (fd < 0) return;
        struct sockaddr_storage address;
        const unsigned size = NameserverAddress(server, port, &address);
        sendto(fd, question, (size_t)length, MSG_NOSIGNAL, (struct sockaddr*)&address, size);
    }
    int ServerOf(const struct sockaddr_storage& from, uint16_t port) {
        for (int server = 0; server < config_.serverCount; ++server) {
            struct sockaddr_storage expected;
            NameserverAddress(server, port, &expected);
            if (config_.servers[server].family == AF_INET) {
                const auto* a = (const struct sockaddr_in*)&from;
                const auto* b = (const struct sockaddr_in*)&expected;
                if (a->sin_family == AF_INET && a->sin_port == b->sin_port && !memcmp(&a->sin_addr, &b->sin_addr, 4))
                    return server;
            } else {
                const auto* a = (const struct sockaddr_in6*)&from;
                const auto* b = (const struct sockaddr_in6*)&expected;
                if (a->sin6_family == AF_INET6 && a->sin6_port == b->sin6_port &&
                    !memcmp(&a->sin6_addr, &b->sin6_addr, 16))
                    return server;
            }
        }
        return -1;
    }
    void Receive(int fd, int count, const uint8_t questions[2][280], const int lengths[2], DnsAnswers* answers,
                 int servfailRetries[2]) {
        const uint16_t port = DnsPort();
        for (;;) {
            struct sockaddr_storage from = {};
            unsigned fromLength = sizeof from;
            uint8_t buffer[kUdpAnswerSize];
            const long got = recvfrom(fd, buffer, sizeof buffer, MSG_DONTWAIT, (struct sockaddr*)&from, &fromLength);
            if (got < 0) return;
            if (got < 12) continue;
            if (!(buffer[2] & 0x80)) continue;  // not a response
            const int server = ServerOf(from, port);
            if (server < 0) continue;
            int question = -1;
            for (int i = 0; i < count; ++i)
                if (Get16(buffer) == Get16(questions[i])) question = i;
            if (question < 0 || answers->status[question] != kAnswerNone) continue;
            const int rcode = buffer[3] & 0x0f;
            if (rcode == 2) {  // SERVFAIL: ask once more, then take it as a failure
                if (servfailRetries[question]-- > 0) {
                    SendTo(server, questions[question], lengths[question], port);
                    continue;
                }
                answers->status[question] = kAnswerServfail;
                answers->rcode[question] = rcode;
                continue;
            }
            if (rcode != 0 && rcode != 3) continue;  // REFUSED and the like are ignored
            if (buffer[2] & 0x02) {  // truncated: the answer comes over TCP
                answers->truncatedBy[question] = server;
                answers->rcode[question] = rcode;
                answers->status[question] = kAnswerNeedsTcp;
                continue;
            }
            memcpy(answers->udp[question], buffer, (size_t)got);
            answers->udpLength[question] = (int)got;
            answers->rcode[question] = rcode;
            answers->status[question] = kAnswerGot;
        }
    }
    // The same question and answer over TCP, with the two-byte length prefix both ways.
    uint8_t* FetchOverTcp(int server, const uint8_t* question, int length, int* answerLength) {
        *answerLength = 0;
        if (server < 0 || server >= config_.serverCount) return nullptr;
        const uint16_t port = DnsPort();
        const int fd = socket(config_.servers[server].family, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (fd < 0) return nullptr;
        struct sockaddr_storage address;
        const unsigned size = NameserverAddress(server, port, &address);
        if (connect(fd, (struct sockaddr*)&address, size) < 0 && errno != EINPROGRESS) {
            close(fd);
            return nullptr;
        }
        uint8_t message[282];
        Put16(message, (uint16_t)length);
        memcpy(message + 2, question, (size_t)length);
        const int total = length + 2;
        int sent = 0;
        if (!WaitFor(fd, true)) {
            close(fd);
            return nullptr;
        }
        while (sent < total) {
            const long wrote = sendto(fd, message + sent, (size_t)(total - sent), MSG_NOSIGNAL, nullptr, 0);
            if (wrote < 0) {
                if ((errno == EAGAIN || errno == EINTR) && WaitFor(fd, true)) continue;
                break;
            }
            sent += (int)wrote;
        }
        uint8_t header[2] = {};
        uint8_t* answer = nullptr;
        if (sent == total && ReadExactly(fd, header, 2)) {
            const int wanted = Get16(header);
            if (wanted >= 12) {
                uint8_t* data = (uint8_t*)malloc((size_t)wanted);
                if (data && ReadExactly(fd, data, wanted)) {
                    answer = data;
                    *answerLength = wanted;
                } else {
                    free(data);
                }
            }
        }
        close(fd);
        return answer;
    }
    bool WaitFor(int fd, bool writable) {
        struct pollfd polled = {fd, (short)(writable ? POLLOUT : POLLIN), 0};
        for (;;) {
            const uint64_t now = MonotonicMs();
            if (now >= deadline_) return false;
            const int result = poll(&polled, 1, (int)(deadline_ - now));
            if (result > 0) return true;
            // A poll that ran out of time leaves errno alone, so it must not be read as EINTR in a
            // loop; the deadline at the top of the loop is the answer.
            if (result == 0) continue;
            if (errno != EINTR) return false;
        }
    }
    bool ReadExactly(int fd, uint8_t* buffer, int length) {
        int got = 0;
        while (got < length) {
            const long amount = recvfrom(fd, buffer + got, (size_t)(length - got), 0, nullptr, nullptr);
            if (amount > 0) {
                got += (int)amount;
                continue;
            }
            if (amount < 0 && (errno == EAGAIN || errno == EINTR) && WaitFor(fd, false)) continue;
            return false;
        }
        return true;
    }

    const ResolverConfig& config_;
    uint64_t deadline_;
    int udp_[2] = {-1, -1};
};

// One name, in the family asked for: A for IPv4, AAAA for IPv6, both for AF_UNSPEC.
LookupResult QueryDnsName(const char* name, int family, const ResolverConfig& config, uint64_t deadline) {
    LookupResult result = {};
    int types[2];
    int count = 0;
    if (family != AF_INET6) types[count++] = kRrA;
    if (family != AF_INET) types[count++] = kRrAaaa;
    uint8_t questions[2][280] = {};
    int lengths[2] = {};
    uint16_t ids[2] = {QuestionId(), QuestionId()};
    if (ids[1] == ids[0]) ++ids[1];
    for (int i = 0; i < count; ++i) {
        lengths[i] = BuildQuestion(questions[i], (int)sizeof questions[i], name, types[i], ids[i]);
        if (lengths[i] < 0) {
            result.status = EAI_NONAME;
            return result;
        }
    }
    DnsAnswers answers;
    {
        DnsSession session(config, deadline);
        session.Exchange(questions, lengths, count, &answers);
    }
    ParsedAnswer parsed[2] = {};
    bool answered[2] = {false, false};
    for (int i = 0; i < count; ++i) {
        const uint8_t* data = answers.tcp[i] ? answers.tcp[i] : answers.udp[i];
        const int length = answers.udpLength[i];
        if (answers.status[i] != kAnswerGot || length < 12) continue;
        if (!ParseDnsAnswer(data, length, types[i], &parsed[i])) continue;
        answered[i] = true;
        for (int record = 0; record < parsed[i].count && result.count < kMaxAddresses; ++record)
            result.addresses[result.count++] = parsed[i].addresses[record];
        if (parsed[i].name[0] && !result.canon[0]) CopyText(result.canon, sizeof result.canon, parsed[i].name);
    }
    for (int i = 0; i < count; ++i) free(answers.tcp[i]);
    if (result.count) {
        // Without a CNAME the name asked for is the canonical one, as the resolver reports it.
        if (!result.canon[0]) CopyText(result.canon, sizeof result.canon, name);
        return result;
    }
    // No addresses: the failure is the one glibc reports for the answers it got. A question that timed
    // out or was refused is temporary, an answer with no record of the type is "no data", and
    // NXDOMAIN is "no such name".
    int temporary = 0, noData = 0, noName = 0;
    for (int i = 0; i < count; ++i) {
        if (!answered[i]) {
            ++temporary;
            continue;
        }
        if (parsed[i].rcode == 3) ++noName;
        else if (parsed[i].count == 0) ++noData;
    }
    if (noName == count) result.status = EAI_NONAME;
    else if (temporary == 0 && noData > 0) result.status = EAI_NODATA;
    else if (temporary == 0) result.status = EAI_NONAME;
    else if (noData > 0 && noData + temporary == count) result.status = EAI_NODATA;
    else result.status = EAI_AGAIN;
    return result;
}

// The names a lookup tries: the name itself when it has as many dots as ndots or ends in a dot, and
// the search list's suffixes otherwise. The whole step is bounded by `deadline`, so a dead nameserver
// cannot make the search list multiply the timeout.
LookupResult QueryDns(const char* name, int family, const ResolverConfig& config, uint64_t deadline) {
    LookupResult failure = {};
    failure.status = EAI_NONAME;
    size_t length = strlen(name);
    const bool explicitName = length > 0 && name[length - 1] == '.';
    if (explicitName) --length;
    if (length == 0 || length >= kMaxName) {
        failure.status = EAI_NONAME;
        return failure;
    }
    char candidate[kMaxName + 1];
    memcpy(candidate, name, length);
    candidate[length] = 0;
    int dots = 0;
    for (size_t i = 0; i < length; ++i)
        if (candidate[i] == '.') ++dots;
    const bool bareFirst = explicitName || dots >= config.ndots;
    auto keepFailure = [&](const LookupResult& attempt) {
        if (attempt.status == EAI_AGAIN || attempt.status == EAI_SYSTEM) failure.status = attempt.status;
        else if (failure.status != EAI_AGAIN && attempt.status == EAI_NODATA) failure.status = EAI_NODATA;
    };
    if (bareFirst) {
        LookupResult attempt = QueryDnsName(candidate, family, config, deadline);
        if (attempt.count) return attempt;
        keepFailure(attempt);
    }
    const char* domain = config.search;
    while (*domain && MonotonicMs() < deadline) {
        while (*domain == ' ') ++domain;
        const char* end = domain;
        while (*end && *end != ' ') ++end;
        if (end == domain) break;
        const size_t domainLength = (size_t)(end - domain);
        if (length + 1 + domainLength < sizeof candidate) {
            candidate[length] = '.';
            memcpy(candidate + length + 1, domain, domainLength);
            candidate[length + 1 + domainLength] = 0;
            LookupResult attempt = QueryDnsName(candidate, family, config, deadline);
            if (attempt.count) return attempt;
            keepFailure(attempt);
        }
        domain = end;
    }
    candidate[length] = 0;
    if (!bareFirst) {
        LookupResult attempt = QueryDnsName(candidate, family, config, deadline);
        if (attempt.count) return attempt;
        keepFailure(attempt);
    }
    return failure;
}

// ---------------------------------------------------------------------------------------------
// The addresses of a name: numeric, then /etc/hosts, then DNS, with AI_V4MAPPED's filtering.

void AsV6(const uint8_t* address, int family, uint8_t out[16]) {
    if (family == AF_INET) MapV4IntoV6(address, out);
    else memcpy(out, address, 16);
}

// The value of the most specific RFC 3484 rule whose prefix the address matches.
int MatchPrefix(const uint8_t address[16], const PrefixRule* rules, int count) {
    for (int rule = 0; rule < count; ++rule) {
        unsigned bits = rules[rule].bits;
        const uint8_t* mask = (const uint8_t*)rules[rule].prefix;
        unsigned byte = 0;
        bool same = true;
        while (bits >= 8) {
            if (mask[byte] != address[byte]) {
                same = false;
                break;
            }
            ++byte;
            bits -= 8;
        }
        if (!same) continue;
        if (bits == 0) return rules[rule].value;
        const uint8_t partial = (uint8_t)(0xff00u >> bits);
        if ((mask[byte] & partial) == (address[byte] & partial)) return rules[rule].value;
    }
    return 0;
}

int LabelOf(const uint8_t* address, int family) {
    uint8_t mapped[16];
    AsV6(address, family, mapped);
    return MatchPrefix(mapped, kLabelRules, (int)(sizeof kLabelRules / sizeof kLabelRules[0]));
}

int PrecedenceOf(const uint8_t* address, int family) {
    uint8_t mapped[16];
    AsV6(address, family, mapped);
    return MatchPrefix(mapped, kPrecedenceRules, (int)(sizeof kPrecedenceRules / sizeof kPrecedenceRules[0]));
}

int ScopeOf(const uint8_t* address, int family) {
    if (family == AF_INET) {
        const uint32_t value = (uint32_t)address[0] << 24 | (uint32_t)address[1] << 16 |
                               (uint32_t)address[2] << 8 | address[3];
        for (int rule = 0; rule < 3; ++rule) {
            const uint32_t prefix = (uint32_t)kScopeRules[rule].address[0] << 24 |
                                    (uint32_t)kScopeRules[rule].address[1] << 16 |
                                    (uint32_t)kScopeRules[rule].address[2] << 8 | kScopeRules[rule].address[3];
            if ((value & kScopeRules[rule].mask) == (prefix & kScopeRules[rule].mask)) return kScopeRules[rule].scope;
        }
        return 15;
    }
    if (address[0] == 0xff) return address[1] & 0x0f;                 // multicast
    if (address[0] == 0xfe && (address[1] & 0xc0) == 0x80) return 2;  // link-local
    if (address[0] == 0xfe && (address[1] & 0xc0) == 0xc0) return 5;  // site-local
    if (IsIPv6Loopback(address)) return 2;
    return 14;
}

// Where a destination's traffic would come from, for RFC 3484's first rules: a UDP socket connected
// to it, then getsockname. Nothing is sent.
bool SourceFor(const FoundAddress& address, uint8_t source[16], int* sourceFamily) {
    const int fd = socket(address.family, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    bool usable = false;
    struct sockaddr_storage local = {};
    unsigned size = sizeof local;
    if (address.family == AF_INET) {
        struct sockaddr_in destination = {};
        destination.sin_family = AF_INET;
        memcpy(&destination.sin_addr, address.bytes, 4);
        destination.sin_port = Net16(65535);
        if (connect(fd, (struct sockaddr*)&destination, sizeof destination) == 0 &&
            getsockname(fd, (struct sockaddr*)&local, &size) == 0) {
            memcpy(source, &((struct sockaddr_in*)&local)->sin_addr, 4);
            *sourceFamily = AF_INET;
            usable = true;
        }
    } else {
        struct sockaddr_in6 destination = {};
        destination.sin6_family = AF_INET6;
        memcpy(&destination.sin6_addr, address.bytes, 16);
        destination.sin6_port = Net16(65535);
        destination.sin6_scope_id = address.scope;
        if (connect(fd, (struct sockaddr*)&destination, sizeof destination) == 0 &&
            getsockname(fd, (struct sockaddr*)&local, &size) == 0) {
            memcpy(source, &((struct sockaddr_in6*)&local)->sin6_addr, 16);
            *sourceFamily = AF_INET6;
            usable = true;
        }
    }
    close(fd);
    return usable;
}

// RFC 3484's order over the addresses found: reachable destinations first, then matching scope,
// matching label, higher precedence, smaller scope, and finally the order the sources gave.
void SortAddresses(FoundAddress* addresses, int count) {
    if (count < 2) return;
    struct Rank {
        bool usable;
        uint8_t source[16];
        int sourceFamily;
        int sourceScope, sourceLabel;
        int scope, label, precedence;
    } ranks[kMaxAddresses] = {};
    for (int i = 0; i < count; ++i) {
        Rank& rank = ranks[i];
        rank.usable = SourceFor(addresses[i], rank.source, &rank.sourceFamily);
        if (rank.usable) {
            rank.sourceScope = ScopeOf(rank.source, rank.sourceFamily);
            rank.sourceLabel = LabelOf(rank.source, rank.sourceFamily);
        }
        rank.scope = ScopeOf(addresses[i].bytes, addresses[i].family);
        rank.label = LabelOf(addresses[i].bytes, addresses[i].family);
        rank.precedence = PrecedenceOf(addresses[i].bytes, addresses[i].family);
    }
    int order[kMaxAddresses];
    for (int i = 0; i < count; ++i) order[i] = i;
    auto before = [&](int a, int b) {
        if (ranks[a].usable != ranks[b].usable) return ranks[a].usable;
        if (ranks[a].usable) {
            if ((ranks[a].scope == ranks[a].sourceScope) != (ranks[b].scope == ranks[b].sourceScope))
                return ranks[a].scope == ranks[a].sourceScope;
            if ((ranks[a].label == ranks[a].sourceLabel) != (ranks[b].label == ranks[b].sourceLabel))
                return ranks[a].label == ranks[a].sourceLabel;
        }
        if (ranks[a].precedence != ranks[b].precedence) return ranks[a].precedence > ranks[b].precedence;
        if (ranks[a].scope != ranks[b].scope) return ranks[a].scope < ranks[b].scope;
        return a < b;
    };
    for (int i = 1; i < count; ++i) {
        const int value = order[i];
        int at = i;
        while (at > 0 && before(value, order[at - 1])) {
            order[at] = order[at - 1];
            --at;
        }
        order[at] = value;
    }
    FoundAddress sorted[kMaxAddresses];
    for (int i = 0; i < count; ++i) sorted[i] = addresses[order[i]];
    for (int i = 0; i < count; ++i) addresses[i] = sorted[i];
}

// Whether the machine has a non-loopback address of each family, which is what AI_ADDRCONFIG asks:
// glibc's check_pf reads the same netlink dump and skips only 127.0.0.1 and ::1.
void FamiliesConfigured(bool* ipv4, bool* ipv6);

// A numeric name: an address spelled out, with an optional %scope for IPv6. Returns false when the
// text is not an address at all, which is what sends the caller on to /etc/hosts and DNS.
bool NumericAddress(const char* name, int family, int flags, LookupResult* result) {
    uint8_t bytes[16] = {};
    const char* scopeText = strchr(name, kScopeDelimiter);
    if (!scopeText && ParseLenientIPv4(name, bytes)) {
        FoundAddress& found = result->addresses[0];
        found.scope = 0;
        memset(found.bytes, 0, 16);
        if (family == AF_INET || family == AF_UNSPEC) {
            found.family = AF_INET;
            memcpy(found.bytes, bytes, 4);
        } else if (family == AF_INET6 && (flags & AI_V4MAPPED)) {
            found.family = AF_INET6;
            MapV4IntoV6(bytes, found.bytes);
        } else {
            result->status = EAI_ADDRFAMILY;
            return true;
        }
        result->count = 1;
        if (flags & AI_CANONNAME) CopyText(result->canon, sizeof result->canon, name);
        return true;
    }
    char text[kMaxName + 1];
    const size_t length = scopeText ? (size_t)(scopeText - name) : strlen(name);
    if (length >= sizeof text) return false;
    memcpy(text, name, length);
    text[length] = 0;
    if (!ParseIPv6(text, bytes)) return false;
    FoundAddress& found = result->addresses[0];
    found.scope = 0;
    memset(found.bytes, 0, 16);
    if (family == AF_INET) {
        if (!IsV4Mapped(bytes)) {
            result->status = EAI_ADDRFAMILY;
            return true;
        }
        found.family = AF_INET;
        memcpy(found.bytes, bytes + 12, 4);
    } else {
        found.family = AF_INET6;
        memcpy(found.bytes, bytes, 16);
    }
    if (scopeText) {
        const unsigned index = IndexOfInterface(scopeText + 1);
        if (index == 0) {
            result->status = EAI_NONAME;
            return true;
        }
        found.scope = index;
    }
    result->count = 1;
    if (flags & AI_CANONNAME) CopyText(result->canon, sizeof result->canon, name);
    return true;
}

LookupResult LookupAddresses(const char* name, int family, int flags) {
    LookupResult result = {};
    if (!name) {
        // The local addresses, in glibc's order (gaih_inet): the wildcard pair with AI_PASSIVE, the
        // loopback pair without, and IPv4 first only in the first case. The address selection below
        // is not run over this pair, which is why the loopback answer keeps IPv6 first.
        const bool passive = (flags & AI_PASSIVE) != 0;
        auto add = [&](int addressFamily) {
            FoundAddress& found = result.addresses[result.count++];
            found = {};
            found.family = addressFamily;
            if (!passive) {
                if (addressFamily == AF_INET6) {
                    found.bytes[15] = 1;
                } else {
                    found.bytes[0] = 127;
                    found.bytes[3] = 1;
                }
            }
        };
        if (!passive) {
            if (family == AF_UNSPEC || family == AF_INET6) add(AF_INET6);
            if (family == AF_UNSPEC || family == AF_INET) add(AF_INET);
        } else {
            if (family == AF_UNSPEC || family == AF_INET) add(AF_INET);
            if (family == AF_UNSPEC || family == AF_INET6) add(AF_INET6);
        }
        return result;
    }
    const size_t length = strlen(name);
    if (length == 0 || length > kMaxName) {
        result.status = EAI_NONAME;
        return result;
    }
    // With AI_V4MAPPED a request for IPv6 addresses reads both families and maps the v4 results.
    const int lookupFamily = family == AF_INET6 && (flags & AI_V4MAPPED) ? AF_UNSPEC : family;
    if (NumericAddress(name, lookupFamily, flags, &result)) return result;
    if (flags & AI_NUMERICHOST) {
        result.status = EAI_NONAME;
        return result;
    }
    int sources[2] = {};
    const int sourceCount = HostsSourceOrder(sources);
    for (int i = 0; i < sourceCount; ++i) {
        LookupResult found = {};
        if (sources[i] == kSourceFiles) {
            found = LookupHostsFile(name, lookupFamily, flags);
            if (found.count) return found;
            continue;
        }
        ResolverConfig config;
        ReadResolverConfig(&config);
        found = QueryDns(name, lookupFamily, config, MonotonicMs() + (uint64_t)config.timeout * 1000);
        if (found.count) return found;
        if (found.status == EAI_AGAIN || found.status == EAI_SYSTEM || found.status == EAI_MEMORY ||
            found.status == EAI_NODATA) {
            result.status = found.status;
            return result;
        }
    }
    if (result.status == 0) result.status = EAI_NONAME;
    return result;
}

// ---------------------------------------------------------------------------------------------
// getaddrinfo.

// glibc's socket-type table (nss/getaddrinfo.c): the pairs that have a service name, the raw one that
// cannot have one, and the types whose port is returned when no type is asked for.
struct TypeProto {
    int socktype;
    int protocol;
    bool protocolAny;
    bool noService;
    bool defaultType;
    const char* name;
};
constexpr TypeProto kTypes[] = {
    {0, 0, false, false, false, ""},
    {SOCK_STREAM, IPPROTO_TCP, false, false, true, "tcp"},
    {SOCK_DGRAM, IPPROTO_UDP, false, false, true, "udp"},
    {kSockDccp, kIpProtocolDccp, false, false, false, "dccp"},
    {SOCK_DGRAM, kIpProtocolUdplite, false, false, false, "udplite"},
    {SOCK_STREAM, kIpProtocolSctp, false, false, false, "sctp"},
    {SOCK_SEQPACKET, kIpProtocolSctp, false, false, false, "sctp"},
    {SOCK_STREAM, kIpProtocolMptcp, false, false, false, "mptcp"},
    {SOCK_RAW, 0, true, true, true, "raw"},
    {0, 0, false, false, false, ""},
};

struct ServiceTuple {
    int socktype;
    int protocol;
    uint16_t port;
};

// glibc's get_servtuples: the ports and socket types a request asks for, or the EAI_* code it fails
// with. A named service is looked up in /etc/services for each type that has a name there.
int ServiceTuples(const char* name, int numericPort, bool numeric, int socktype, int protocol, ServiceTuple* out) {
    const TypeProto* type = kTypes;
    if (socktype || protocol) {
        ++type;
        while (type->name[0] && ((socktype && socktype != type->socktype) ||
                                 (protocol && !type->protocolAny && protocol != type->protocol)))
            ++type;
        if (!type->name[0]) return socktype ? EAI_SOCKTYPE : EAI_SERVICE;
    }
    if (name && type->noService) return EAI_SERVICE;
    int count = 0;
    if (!name || numeric) {
        const uint16_t port = Net16((uint16_t)numericPort);
        if (socktype || protocol) {
            out[0].socktype = type->socktype;
            out[0].protocol = type->protocolAny ? protocol : type->protocol;
            out[0].port = port;
            return 1;
        }
        for (const TypeProto* each = ++type; each->name[0]; ++each) {
            if (!each->defaultType) continue;
            out[count].socktype = each->socktype;
            out[count].protocol = each->protocolAny ? protocol : each->protocol;
            out[count].port = port;
            ++count;
        }
        return count;
    }
    if (type->name[0]) {
        uint16_t port = 0;
        if (!ServicePort(name, type->name, &port)) return EAI_SERVICE;
        out[0].socktype = type->socktype;
        out[0].protocol = type->protocolAny ? protocol : type->protocol;
        out[0].port = port;
        return 1;
    }
    for (const TypeProto* each = ++type; each->name[0]; ++each) {
        if (each->noService) continue;
        if (socktype && socktype != each->socktype) continue;
        if (protocol && !each->protocolAny && protocol != each->protocol) continue;
        uint16_t port = 0;
        if (!ServicePort(name, each->name, &port)) continue;
        out[count].socktype = each->socktype;
        out[count].protocol = each->protocolAny ? protocol : each->protocol;
        out[count].port = port;
        ++count;
    }
    return count ? count : EAI_SERVICE;
}

// One entry of the result and the socket address beside it, in the same block.
struct AddressEntry {
    struct addrinfo ai;
    union {
        struct sockaddr_in v4;
        struct sockaddr_in6 v6;
    } address;
};

// The flags glibc accepts, including the former IDN bits, which are ignored here as they are there in
// a library built without libidn.
constexpr int kKnownFlags = AI_PASSIVE | AI_CANONNAME | AI_NUMERICHOST | AI_ADDRCONFIG | AI_V4MAPPED | AI_ALL |
                            AI_IDN | AI_CANONIDN | 0x300 | AI_NUMERICSERV;

}  // namespace

// The definitions below take glibc's names, which its headers declare with the default visibility, so
// they are hidden in assembly as time.cpp's and numbers.cpp's are: cordinal and the static libstdc++
// bind to them, and the program exports none of them.
#define HIDE(name) __asm__(".hidden " #name);
HIDE(inet_pton) HIDE(inet_ntop) HIDE(getaddrinfo) HIDE(freeaddrinfo) HIDE(getnameinfo) HIDE(getifaddrs)
HIDE(freeifaddrs) HIDE(gethostname)
#undef HIDE

extern "C" int inet_pton(int family, const char* __restrict text, void* __restrict out) noexcept {
    // glibc leaves the caller's buffer alone when the text does not parse, so the parse runs into a
    // local first.
    if (family == AF_INET) {
        uint8_t bytes[4] = {};
        if (!ParseDottedQuad(text, bytes)) return 0;
        memcpy(out, bytes, 4);
        return 1;
    }
    if (family == AF_INET6) {
        uint8_t bytes[16] = {};
        if (!ParseIPv6(text, bytes)) return 0;
        memcpy(out, bytes, 16);
        return 1;
    }
    errno = EAFNOSUPPORT;
    return -1;
}

extern "C" const char* inet_ntop(int family, const void* __restrict address, char* __restrict out,
                                 socklen_t size) noexcept {
    if (family == AF_INET) return FormatIPv4((const uint8_t*)address, out, size) < 0 ? nullptr : out;
    if (family == AF_INET6) return FormatIPv6((const uint8_t*)address, out, size) < 0 ? nullptr : out;
    errno = EAFNOSUPPORT;
    return nullptr;
}

extern "C" int getaddrinfo(const char* __restrict name, const char* __restrict service,
                           const struct addrinfo* __restrict hints, struct addrinfo** __restrict result) {
    if (!result) return EAI_NONAME;
    *result = nullptr;
    if (name && !strcmp(name, "*")) name = nullptr;
    if (service && !strcmp(service, "*")) service = nullptr;
    if (!name && !service) return EAI_NONAME;
    static const struct addrinfo kDefaultHints = {AI_V4MAPPED | AI_ADDRCONFIG, AF_UNSPEC, 0, 0, 0, nullptr, nullptr,
                                                  nullptr};
    if (!hints) hints = &kDefaultHints;
    int family = hints->ai_family;
    const int flags = hints->ai_flags;
    const int socktype = hints->ai_socktype;
    const int protocol = hints->ai_protocol;
    if (flags & ~kKnownFlags) return EAI_BADFLAGS;
    if ((flags & AI_CANONNAME) && !name) return EAI_BADFLAGS;
    if (family != AF_UNSPEC && family != AF_INET && family != AF_INET6) return EAI_FAMILY;
    if (flags & AI_ADDRCONFIG) {
        bool ipv4 = true, ipv6 = true;
        FamiliesConfigured(&ipv4, &ipv6);
        if (family == AF_UNSPEC && (ipv4 || ipv6)) {
            if (ipv4 != ipv6) family = ipv4 ? AF_INET : AF_INET6;
        } else if ((family == AF_INET && !ipv4) || (family == AF_INET6 && !ipv6)) {
            return EAI_NONAME;
        }
    }
    const char* serviceName = nullptr;
    int numericPort = 0;
    bool numeric = false;
    if (service && *service) {
        char* end = nullptr;
        const unsigned long value = strtoul(service, &end, 10);
        const int port = (int)(uint32_t)value;
        if (end && *end == 0) {
            if (port >= 0) {
                numericPort = port;
                numeric = true;
            } else {
                serviceName = service;  // a negative number is a name, as glibc treats it
            }
        } else {
            if (flags & AI_NUMERICSERV) return EAI_NONAME;
            serviceName = service;
        }
    }
    ServiceTuple tuples[kMaxServiceTuples] = {};
    const int tupleCount = ServiceTuples(serviceName, numericPort, numeric, socktype, protocol, tuples);
    if (tupleCount < 0) return tupleCount;  // an EAI_* code (-EAI_SERVICE, -EAI_SOCKTYPE)
    LookupResult found = LookupAddresses(name, family, flags);
    if (found.status) return found.status;
    if (found.count == 0) return EAI_NONAME;
    if (family == AF_INET6 && (flags & AI_V4MAPPED)) {
        // glibc's filter: when there is an IPv6 address the v4 ones are dropped unless AI_ALL asks for
        // them, and whatever v4 addresses remain become v4-mapped.
        bool anyV6 = false;
        for (int i = 0; i < found.count; ++i)
            if (found.addresses[i].family == AF_INET6) anyV6 = true;
        int kept = 0;
        for (int i = 0; i < found.count; ++i) {
            FoundAddress address = found.addresses[i];
            if (address.family == AF_INET && anyV6 && !(flags & AI_ALL)) continue;
            if (address.family == AF_INET) {
                uint8_t mapped[16];
                MapV4IntoV6(address.bytes, mapped);
                address.family = AF_INET6;
                memcpy(address.bytes, mapped, 16);
                address.scope = 0;
            }
            found.addresses[kept++] = address;
        }
        found.count = kept;
        if (found.count == 0) return EAI_NONAME;
    }
    // The address selection runs over names; the local pair above keeps gaih_inet's own order.
    if (name) SortAddresses(found.addresses, found.count);
    const int entries = found.count * tupleCount;
    const bool withCanon = (flags & AI_CANONNAME) && found.canon[0] != 0;
    const size_t canonLength = withCanon ? strlen(found.canon) + 1 : 0;
    auto* block = (AddressEntry*)calloc(1, (size_t)entries * sizeof(AddressEntry) + canonLength);
    if (!block) return EAI_MEMORY;
    char* canonAt = canonLength ? (char*)(block + entries) : nullptr;
    if (canonAt) memcpy(canonAt, found.canon, canonLength);
    int written = 0;
    for (int address = 0; address < found.count; ++address) {
        for (int tuple = 0; tuple < tupleCount; ++tuple) {
            AddressEntry& entry = block[written];
            entry.ai.ai_flags = flags;
            entry.ai.ai_family = found.addresses[address].family;
            entry.ai.ai_socktype = tuples[tuple].socktype;
            entry.ai.ai_protocol = tuples[tuple].protocol;
            entry.ai.ai_canonname = written == 0 ? canonAt : nullptr;
            if (found.addresses[address].family == AF_INET) {
                entry.address.v4.sin_family = AF_INET;
                entry.address.v4.sin_port = tuples[tuple].port;
                memcpy(&entry.address.v4.sin_addr, found.addresses[address].bytes, 4);
                entry.ai.ai_addrlen = sizeof(struct sockaddr_in);
            } else {
                entry.address.v6.sin6_family = AF_INET6;
                entry.address.v6.sin6_port = tuples[tuple].port;
                entry.address.v6.sin6_flowinfo = 0;
                memcpy(&entry.address.v6.sin6_addr, found.addresses[address].bytes, 16);
                entry.address.v6.sin6_scope_id = found.addresses[address].scope;
                entry.ai.ai_addrlen = sizeof(struct sockaddr_in6);
            }
            entry.ai.ai_addr = (struct sockaddr*)&entry.address;
            entry.ai.ai_next = nullptr;
            if (written > 0) block[written - 1].ai.ai_next = &entry.ai;
            ++written;
        }
    }
    *result = &block[0].ai;
    return 0;
}

extern "C" void freeaddrinfo(struct addrinfo* list) noexcept { free(list); }

// ---------------------------------------------------------------------------------------------
// getnameinfo.

// The name of an address: /etc/hosts first, then a PTR record, in the order nsswitch.conf names.
// Returns 0 with the name written, EAI_NONAME when there is none, or the temporary failure glibc
// reports (EAI_AGAIN) when the resolver timed out.
int ReverseName(const uint8_t* address, int family, const char* ptrName, char* out, size_t size) {
    int sources[2] = {};
    const int sourceCount = HostsSourceOrder(sources);
    int failure = EAI_NONAME;
    for (int i = 0; i < sourceCount; ++i) {
        if (sources[i] == kSourceFiles) {
            if (ReverseHostsFile(address, family, out, size)) return 0;
            continue;
        }
        ResolverConfig config;
        ReadResolverConfig(&config);
        uint8_t question[280];
        const int length = BuildQuestion(question, (int)sizeof question, ptrName, kRrPtr, QuestionId());
        if (length < 0) continue;
        uint8_t questions[2][280] = {};
        const int lengths[2] = {length, 0};
        memcpy(questions[0], question, (size_t)length);
        DnsAnswers answers;
        {
            DnsSession session(config, MonotonicMs() + (uint64_t)config.timeout * 1000);
            session.Exchange(questions, lengths, 1, &answers);
        }
        ParsedAnswer parsed = {};
        const uint8_t* data = answers.tcp[0] ? answers.tcp[0] : answers.udp[0];
        const bool parsedOk = answers.status[0] == kAnswerGot && answers.udpLength[0] >= 12 &&
                              ParseDnsAnswer(data, answers.udpLength[0], kRrPtr, &parsed);
        free(answers.tcp[0]);
        if (parsedOk && parsed.name[0]) return CopyText(out, size, parsed.name);
        // A negative answer from the server is the end of the search, as it is for glibc; only a
        // timeout or a refusal is worth reporting as temporary.
        if (parsedOk && (parsed.rcode == 0 || parsed.rcode == 3)) continue;
        failure = EAI_AGAIN;
    }
    return failure;
}

// The domain part of the machine's own name, which NI_NOFQDN trims from a name that ends with it.
void TrimLocalDomain(char* name) {
    char local[65] = {};
    if (gethostname(local, sizeof local) != 0) return;
    const char* dot = strchr(local, '.');
    if (!dot || !dot[1]) return;
    const size_t nameLength = strlen(name), domainLength = strlen(dot + 1);
    if (nameLength <= domainLength + 1) return;
    const size_t at = nameLength - domainLength;
    if (name[at - 1] == '.' && EqualsNoCase(name + at, dot + 1)) name[at - 1] = 0;
}

// The host part for the AF_INET and AF_INET6 families: the name from /etc/hosts or a PTR record, or
// the address itself with the scope id IPv6 needs.
int HostPart(const struct sockaddr* address, char* host, socklen_t hostlen, int flags) {
    const int family = address->sa_family;
    uint8_t bytes[16] = {};
    uint32_t scope = 0;
    if (family == AF_INET) {
        memcpy(bytes, &((const struct sockaddr_in*)address)->sin_addr, 4);
    } else {
        memcpy(bytes, &((const struct sockaddr_in6*)address)->sin6_addr, 16);
        scope = ((const struct sockaddr_in6*)address)->sin6_scope_id;
    }
    if (!(flags & NI_NUMERICHOST)) {
        char ptr[kMaxName + 1] = {};
        if (family == AF_INET) {
            // a.b.c.d.in-addr.arpa, the bytes in reverse.
            char* at = ptr;
            for (int part = 3; part >= 0; --part) {
                FormatCount(at, 4, bytes[part]);
                at += strlen(at);
                *at++ = '.';
            }
            memcpy(at, "in-addr.arpa", sizeof "in-addr.arpa");
        } else {
            static const char kHex[] = "0123456789abcdef";
            char* at = ptr;
            for (int part = 15; part >= 0; --part) {
                *at++ = kHex[bytes[part] & 15];
                *at++ = '.';
                *at++ = kHex[bytes[part] >> 4];
                *at++ = '.';
            }
            memcpy(at, "ip6.arpa", sizeof "ip6.arpa");
        }
        char name[kMaxName + 1] = {};
        const int status = ReverseName(bytes, family, ptr, name, sizeof name);
        if (status == 0) {
            if (flags & NI_NOFQDN) TrimLocalDomain(name);
            return CopyText(host, hostlen, name);
        }
        if (status != EAI_NONAME) return status;
    }
    if (flags & NI_NAMEREQD) return EAI_NONAME;
    if (!inet_ntop(family, bytes, host, hostlen)) return EAI_OVERFLOW;
    if (family == AF_INET6 && scope) {
        char suffix[IFNAMSIZ + 16] = "%";
        const bool linkLocal =
            (bytes[0] == 0xfe && (bytes[1] & 0xc0) == 0x80) || (bytes[0] == 0xff && (bytes[1] & 0x0f) == 2);
        char interface[IFNAMSIZ + 1] = {};
        if (linkLocal && NameOfInterface(scope, interface)) {
            memcpy(suffix + 1, interface, strlen(interface) + 1);
        } else {
            FormatCount(suffix + 1, sizeof suffix - 1, scope);
        }
        const size_t used = strlen(host), needed = strlen(suffix);
        if (used + needed + 1 > hostlen) return EAI_OVERFLOW;
        memcpy(host + used, suffix, needed + 1);
    }
    return 0;
}

// The service part: the name /etc/services gives the port and protocol, or the port in decimal.
int ServicePart(const struct sockaddr* address, char* serv, socklen_t servlen, int flags) {
    const uint16_t port = ((const struct sockaddr_in*)address)->sin_port;
    if (!(flags & NI_NUMERICSERV)) {
        char name[64] = {};
        if (ServiceName(port, (flags & NI_DGRAM) ? "udp" : "tcp", name, sizeof name))
            return CopyText(serv, servlen, name);
    }
    char text[12];
    FormatCount(text, sizeof text, Host16(port));
    return CopyText(serv, servlen, text);
}

extern "C" int getnameinfo(const struct sockaddr* __restrict address, socklen_t addressLength,
                           char* __restrict host, socklen_t hostLength, char* __restrict serv,
                           socklen_t servLength, int flags) {
    constexpr int kKnown = NI_NUMERICHOST | NI_NUMERICSERV | NI_NOFQDN | NI_NAMEREQD | NI_DGRAM | NI_IDN | 0xC0;
    if (flags & ~kKnown) return EAI_BADFLAGS;
    if (!address || addressLength < sizeof(sa_family_t)) return EAI_FAMILY;
    if ((flags & NI_NAMEREQD) && !host && !serv) return EAI_NONAME;
    switch (address->sa_family) {
        case AF_UNIX:
            if (addressLength < offsetof(struct sockaddr_un, sun_path)) return EAI_FAMILY;
            break;
        case AF_INET:
            if (addressLength < sizeof(struct sockaddr_in)) return EAI_FAMILY;
            break;
        case AF_INET6:
            if (addressLength < sizeof(struct sockaddr_in6)) return EAI_FAMILY;
            break;
        default:
            return EAI_FAMILY;
    }
    if (host && hostLength > 0) {
        int result;
        if (address->sa_family == AF_UNIX) {
            char local[65] = {};
            if (!(flags & NI_NUMERICHOST) && gethostname(local, sizeof local) == 0)
                result = CopyText(host, hostLength, local);
            else if (flags & NI_NAMEREQD)
                result = EAI_NONAME;
            else
                result = CopyText(host, hostLength, "localhost");
        } else {
            result = HostPart(address, host, hostLength, flags);
        }
        if (result) return result;
    }
    if (serv && servLength > 0) {
        const int result = address->sa_family == AF_UNIX
                               ? CopyText(serv, servLength, ((const struct sockaddr_un*)address)->sun_path)
                               : ServicePart(address, serv, servLength, flags);
        if (result) return result;
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------
// getifaddrs: the kernel's own view of the interfaces, from the two netlink dumps glibc reads.

namespace {

struct NetlinkDump {
    uint8_t* data = nullptr;
    size_t length = 0;
    size_t capacity = 0;
    bool interrupted = false;
};

bool AppendDump(NetlinkDump* dump, const uint8_t* data, size_t length) {
    if (dump->length + length > kMaxDumpSize) return false;
    if (dump->length + length > dump->capacity) {
        size_t wanted = dump->capacity ? dump->capacity * 2 : 16384;
        while (wanted < dump->length + length) wanted *= 2;
        auto* grown = (uint8_t*)realloc(dump->data, wanted);
        if (!grown) return false;
        dump->data = grown;
        dump->capacity = wanted;
    }
    memcpy(dump->data + dump->length, data, length);
    dump->length += length;
    return true;
}

// Calls `visit` for each message of the dump, which holds whole netlink messages back to back.
template <class Visit>
void WalkDump(const uint8_t* data, size_t length, Visit visit) {
    if (!data) return;
    int remaining = (int)length;
    auto* message = (struct nlmsghdr*)(void*)data;
    for (; NLMSG_OK(message, remaining); message = (struct nlmsghdr*)NLMSG_NEXT(message, remaining))
        visit((const uint8_t*)message, (size_t)message->nlmsg_len);
}

// One netlink dump, collecting the messages of one type until the kernel's end marker. The wait is
// bounded by kNetlinkTimeout: the kernel answers a dump at once, so the deadline only matters if a
// reply went missing, and a getifaddrs or AI_ADDRCONFIG check must never hang the caller.
constexpr uint64_t kNetlinkTimeout = 2000;

bool ReadNetlinkDump(int fd, int type, unsigned sequence, NetlinkDump* dump) {
    struct {
        struct nlmsghdr header;
        struct rtgenmsg message;
        char padding[3];
    } request = {};
    request.header.nlmsg_len = sizeof request;
    request.header.nlmsg_type = type;
    request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    request.header.nlmsg_seq = sequence;
    request.message.rtgen_family = AF_UNSPEC;
    struct sockaddr_nl to = {};
    to.nl_family = AF_NETLINK;
    if (sendto(fd, &request, sizeof request, 0, (struct sockaddr*)&to, sizeof to) < 0) return false;
    const uint64_t deadline = MonotonicMs() + kNetlinkTimeout;
    for (;;) {
        const uint64_t now = MonotonicMs();
        if (now >= deadline) {
            errno = EAGAIN;
            return false;
        }
        struct pollfd ready = {fd, POLLIN, 0};
        const int waited = poll(&ready, 1, (int)(deadline - now));
        if (waited < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (waited == 0) continue;
        uint8_t buffer[32768];
        struct sockaddr_nl from = {};
        unsigned fromLength = sizeof from;
        const long got = recvfrom(fd, buffer, sizeof buffer, MSG_DONTWAIT, (struct sockaddr*)&from, &fromLength);
        if (got < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return false;
        }
        if (from.nl_pid != 0) continue;  // not the kernel
        int remaining = (int)got;
        bool done = false;
        for (auto* message = (struct nlmsghdr*)buffer; NLMSG_OK(message, remaining);
             message = (struct nlmsghdr*)NLMSG_NEXT(message, remaining)) {
            // Only the sequence identifies the dump: this kernel fills nlmsg_pid with the asking
            // socket's port id rather than 0, so checking it for 0 skipped every reply, the end
            // marker included, and the dump waited for a datagram that had already arrived.
            if (message->nlmsg_seq != sequence) continue;
            if (message->nlmsg_type == NLMSG_DONE) {
                done = true;
                break;
            }
            if (message->nlmsg_type == NLMSG_ERROR) return false;
            if (message->nlmsg_flags & NLM_F_DUMP_INTR) dump->interrupted = true;
            if (!AppendDump(dump, (const uint8_t*)message, message->nlmsg_len)) return false;
        }
        if (done) {
            if (dump->interrupted) {
                errno = EAGAIN;
                return false;
            }
            return true;
        }
    }
}

// Room for a hardware address longer than sockaddr_ll holds, as glibc keeps it (InfiniBand needs
// more than its eight bytes).
struct SockaddrLlMax {
    unsigned short sll_family, sll_protocol;
    int sll_ifindex;
    unsigned short sll_hatype;
    unsigned char sll_pkttype, sll_halen;
    unsigned char sll_addr[24];
};

union InterfaceSocketAddress {
    struct sockaddr sa;
    SockaddrLlMax link;
    struct sockaddr_in v4;
    struct sockaddr_in6 v6;
};

// One entry of the list in glibc's layout: the ifaddrs, its three socket addresses, and a name the
// address entries point at, all in the one block the list lives in.
struct InterfaceEntry {
    struct ifaddrs ifa;
    union InterfaceSocketAddress address, netmask, broadaddr;
    char name[IFNAMSIZ + 1];
};

// The netmask a prefix length describes, which the kernel does not send.
void FillNetmask(InterfaceEntry* entry, uint8_t prefixLength) {
    struct sockaddr* netmask = &entry->netmask.sa;
    entry->ifa.ifa_netmask = netmask;
    netmask->sa_family = entry->ifa.ifa_addr->sa_family;
    unsigned bits = prefixLength;
    if (entry->ifa.ifa_addr->sa_family == AF_INET) {
        if (bits > 32) bits = 32;
        auto* bytes = (uint8_t*)&entry->netmask.v4.sin_addr;
        for (unsigned i = 0; i < bits / 8; ++i) bytes[i] = 0xff;
        if (bits % 8) bytes[bits / 8] = (uint8_t)(0xff << (8 - bits % 8));
    } else {
        if (bits > 128) bits = 128;
        auto* bytes = (uint8_t*)&entry->netmask.v6.sin6_addr;
        for (unsigned i = 0; i < bits / 8; ++i) bytes[i] = 0xff;
        if (bits % 8) bytes[bits / 8] = (uint8_t)(0xff << (8 - bits % 8));
    }
}

// One address attribute into a socket address, with the scope id a link-local IPv6 address needs.
void FillAddress(struct sockaddr* destination, int family, const void* data, size_t length, int index) {
    destination->sa_family = (sa_family_t)family;
    if (family == AF_INET) {
        if (length == 4) memcpy(&((struct sockaddr_in*)destination)->sin_addr, data, 4);
    } else if (family == AF_INET6) {
        if (length != 16) return;
        auto* address = (struct sockaddr_in6*)destination;
        memcpy(&address->sin6_addr, data, 16);
        const auto* bytes = (const uint8_t*)data;
        if ((bytes[0] == 0xfe && (bytes[1] & 0xc0) == 0x80) || (bytes[0] == 0xff && (bytes[1] & 0x0f) == 2))
            address->sin6_scope_id = (uint32_t)index;
    }
}

// Fills the list from the two dumps: link entries first, in the order the kernel sends them, then one
// entry per address, which is the order glibc's list has.
int BuildInterfaces(const NetlinkDump& links, const NetlinkDump& addresses, struct ifaddrs** out) {
    int linkCount = 0, addressCount = 0;
    size_t statsSize = 0;
    WalkDump(links.data, links.length, [&](const uint8_t* data, size_t) {
        auto* message = (struct nlmsghdr*)(void*)data;
        if (message->nlmsg_type != RTM_NEWLINK) return;
        ++linkCount;
        const auto* info = (const struct ifinfomsg*)NLMSG_DATA(message);
        int remaining = message->nlmsg_len - (int)NLMSG_LENGTH(sizeof *info);
        for (auto* attribute = IFLA_RTA(info); RTA_OK(attribute, remaining); attribute = RTA_NEXT(attribute, remaining))
            if (attribute->rta_type == IFLA_STATS) statsSize += RTA_PAYLOAD(attribute);
    });
    WalkDump(addresses.data, addresses.length, [&](const uint8_t* data, size_t) {
        auto* message = (struct nlmsghdr*)(void*)data;
        if (message->nlmsg_type == RTM_NEWADDR) ++addressCount;
    });
    const int entryCount = linkCount + addressCount;
    if (entryCount == 0) {
        *out = nullptr;
        return 0;
    }
    auto* bytes = (uint8_t*)calloc(1, (size_t)entryCount * sizeof(InterfaceEntry) + statsSize);
    if (!bytes) return -1;
    auto* entries = (InterfaceEntry*)bytes;
    uint8_t* statsAt = bytes + (size_t)entryCount * sizeof(InterfaceEntry);
    auto* slotOfIndex = (int*)malloc(sizeof(int) * (size_t)linkCount);
    if (!slotOfIndex) {
        free(bytes);
        errno = ENOMEM;
        return -1;
    }
    for (int i = 0; i < linkCount; ++i) slotOfIndex[i] = -1;
    int link = 0;
    WalkDump(links.data, links.length, [&](const uint8_t* data, size_t) {
        auto* message = (struct nlmsghdr*)(void*)data;
        if (message->nlmsg_type != RTM_NEWLINK) return;
        const auto* info = (const struct ifinfomsg*)NLMSG_DATA(message);
        InterfaceEntry& entry = entries[link];
        entry.ifa.ifa_flags = info->ifi_flags;
        int remaining = message->nlmsg_len - (int)NLMSG_LENGTH(sizeof *info);
        for (auto* attribute = IFLA_RTA(info); RTA_OK(attribute, remaining); attribute = RTA_NEXT(attribute, remaining)) {
            const void* value = RTA_DATA(attribute);
            const size_t payload = RTA_PAYLOAD(attribute);
            switch (attribute->rta_type) {
                case IFLA_ADDRESS:
                    if (payload <= sizeof(entry.address.link.sll_addr)) {
                        entry.address.link.sll_family = AF_PACKET;
                        memcpy(entry.address.link.sll_addr, value, payload);
                        entry.address.link.sll_halen = (unsigned char)payload;
                        entry.address.link.sll_ifindex = info->ifi_index;
                        entry.address.link.sll_hatype = info->ifi_type;
                        entry.ifa.ifa_addr = &entry.address.sa;
                    }
                    break;
                case IFLA_BROADCAST:
                    if (payload <= sizeof(entry.broadaddr.link.sll_addr)) {
                        entry.broadaddr.link.sll_family = AF_PACKET;
                        memcpy(entry.broadaddr.link.sll_addr, value, payload);
                        entry.broadaddr.link.sll_halen = (unsigned char)payload;
                        entry.broadaddr.link.sll_ifindex = info->ifi_index;
                        entry.broadaddr.link.sll_hatype = info->ifi_type;
                        entry.ifa.ifa_broadaddr = &entry.broadaddr.sa;
                    }
                    break;
                case IFLA_IFNAME:
                    if (payload + 1 <= sizeof(entry.name)) {
                        memcpy(entry.name, value, payload);
                        entry.name[payload] = 0;
                        entry.ifa.ifa_name = entry.name;
                    }
                    break;
                case IFLA_STATS:
                    entry.ifa.ifa_data = statsAt;
                    memcpy(statsAt, value, payload);
                    statsAt += payload;
                    break;
                default:
                    break;
            }
        }
        slotOfIndex[link] = info->ifi_index - 1;
        ++link;
    });
    int placed = 0;
    WalkDump(addresses.data, addresses.length, [&](const uint8_t* data, size_t) {
        auto* message = (struct nlmsghdr*)(void*)data;
        if (message->nlmsg_type != RTM_NEWADDR) return;
        const auto* info = (const struct ifaddrmsg*)NLMSG_DATA(message);
        int slot = -1;
        for (int i = 0; i < linkCount; ++i)
            if (slotOfIndex[i] != -1 && slotOfIndex[i] == info->ifa_index - 1) slot = i;
        if (slot < 0) return;
        InterfaceEntry& entry = entries[linkCount + placed];
        ++placed;
        entry.ifa.ifa_flags = entries[slot].ifa.ifa_flags;
        int remaining = message->nlmsg_len - (int)NLMSG_LENGTH(sizeof *info);
        for (auto* attribute = IFA_RTA(info); RTA_OK(attribute, remaining); attribute = RTA_NEXT(attribute, remaining)) {
            const void* value = RTA_DATA(attribute);
            const size_t payload = RTA_PAYLOAD(attribute);
            switch (attribute->rta_type) {
                case IFA_ADDRESS:
                    // After IFA_LOCAL this is a point-to-point destination, which shares the field
                    // with the broadcast address.
                    if (entry.ifa.ifa_addr) {
                        entry.ifa.ifa_broadaddr = &entry.broadaddr.sa;
                        FillAddress(&entry.broadaddr.sa, info->ifa_family, value, payload, info->ifa_index);
                    } else {
                        entry.ifa.ifa_addr = &entry.address.sa;
                        FillAddress(&entry.address.sa, info->ifa_family, value, payload, info->ifa_index);
                    }
                    break;
                case IFA_LOCAL:
                    if (entry.ifa.ifa_addr) {
                        entry.broadaddr = entry.address;
                        entry.ifa.ifa_broadaddr = &entry.broadaddr.sa;
                        memset(&entry.address, 0, sizeof entry.address);
                    }
                    entry.ifa.ifa_addr = &entry.address.sa;
                    FillAddress(&entry.address.sa, info->ifa_family, value, payload, info->ifa_index);
                    break;
                case IFA_BROADCAST:
                    if (entry.ifa.ifa_broadaddr) memset(&entry.broadaddr, 0, sizeof entry.broadaddr);
                    entry.ifa.ifa_broadaddr = &entry.broadaddr.sa;
                    FillAddress(&entry.broadaddr.sa, info->ifa_family, value, payload, info->ifa_index);
                    break;
                case IFA_LABEL:
                    if (payload + 1 <= sizeof(entry.name)) {
                        memcpy(entry.name, value, payload);
                        entry.name[payload] = 0;
                        entry.ifa.ifa_name = entry.name;
                    }
                    break;
                default:
                    break;
            }
        }
        if (!entry.ifa.ifa_name) entry.ifa.ifa_name = entries[slot].ifa.ifa_name;
        if (entry.ifa.ifa_addr && entry.ifa.ifa_addr->sa_family != AF_UNSPEC &&
            entry.ifa.ifa_addr->sa_family != AF_PACKET)
            FillNetmask(&entry, info->ifa_prefixlen);
    });
    const int filled = linkCount + placed;
    for (int i = 0; i + 1 < filled; ++i) entries[i].ifa.ifa_next = &entries[i + 1].ifa;
    free(slotOfIndex);
    *out = &entries[0].ifa;
    return 0;
}

int CollectInterfaces(struct ifaddrs** out) {
    const int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0) return -1;
    struct sockaddr_nl local = {};
    local.nl_family = AF_NETLINK;
    NetlinkDump links, addresses;
    int result = -1;
    if (bind(fd, (struct sockaddr*)&local, sizeof local) == 0 && ReadNetlinkDump(fd, RTM_GETLINK, 1, &links) &&
        ReadNetlinkDump(fd, RTM_GETADDR, 2, &addresses))
        result = BuildInterfaces(links, addresses, out);
    free(links.data);
    free(addresses.data);
    close(fd);
    return result;
}

// AI_ADDRCONFIG's question: does the machine have an address of each family? glibc's check_pf reads
// the address dump and skips only the loopback addresses, and stays pessimistic when the kernel does
// not answer.
void FamiliesConfigured(bool* ipv4, bool* ipv6) {
    *ipv4 = true;
    *ipv6 = true;
    const int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0) return;
    struct sockaddr_nl local = {};
    local.nl_family = AF_NETLINK;
    NetlinkDump addresses;
    if (bind(fd, (struct sockaddr*)&local, sizeof local) == 0 && ReadNetlinkDump(fd, RTM_GETADDR, 1, &addresses)) {
        bool seen4 = false, seen6 = false;
        WalkDump(addresses.data, addresses.length, [&](const uint8_t* data, size_t) {
            auto* message = (struct nlmsghdr*)(void*)data;
            if (message->nlmsg_type != RTM_NEWADDR) return;
            const auto* info = (const struct ifaddrmsg*)NLMSG_DATA(message);
            if (info->ifa_family != AF_INET && info->ifa_family != AF_INET6) return;
            const void* address = nullptr;
            int remaining = message->nlmsg_len - (int)NLMSG_LENGTH(sizeof *info);
            for (auto* attribute = IFA_RTA(info); RTA_OK(attribute, remaining); attribute = RTA_NEXT(attribute, remaining)) {
                if (attribute->rta_type == IFA_LOCAL) {
                    address = RTA_DATA(attribute);
                } else if (attribute->rta_type == IFA_ADDRESS) {
                    address = RTA_DATA(attribute);
                    break;
                }
            }
            if (!address) return;
            if (info->ifa_family == AF_INET) {
                const auto* bytes = (const uint8_t*)address;
                if (!(bytes[0] == 127 && bytes[1] == 0 && bytes[2] == 0 && bytes[3] == 1)) seen4 = true;
            } else if (!IsIPv6Loopback((const uint8_t*)address)) {
                seen6 = true;
            }
        });
        *ipv4 = seen4;
        *ipv6 = seen6;
    }
    free(addresses.data);
    close(fd);
}

}  // namespace

extern "C" int getifaddrs(struct ifaddrs** out) noexcept {
    if (!out) {
        errno = EINVAL;
        return -1;
    }
    *out = nullptr;
    for (int attempt = 0; attempt < 4; ++attempt) {
        errno = 0;
        const int result = CollectInterfaces(out);
        if (result == 0) return 0;
        if (errno != EAGAIN) return -1;
    }
    return -1;
}

extern "C" void freeifaddrs(struct ifaddrs* list) noexcept { free(list); }

extern "C" int gethostname(char* __restrict name, size_t length) noexcept {
    struct KernelName {
        char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65];
    } info = {};
    if (Call(__NR_uname, &info) < 0) return -1;
    size_t used = 0;
    while (used < sizeof info.nodename && info.nodename[used]) ++used;
    if (used >= length) {
        if (length) memcpy(name, info.nodename, length);
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(name, info.nodename, used + 1);
    return 0;
}
