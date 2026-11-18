// cordinal's network names (net.cpp) against glibc's, for checks.h: inet_pton and inet_ntop over a
// table of valid and malformed forms and every buffer size, gethostname at each length, getaddrinfo's
// numeric forms, /etc/hosts hits and error answers compared entry for entry with glibc's, getnameinfo's
// numeric and reverse forms, getifaddrs' whole list beside glibc's, and DNS through a responder this
// check starts on 127.0.0.1 and points net.cpp at with CORDINAL_RESOLV_CONF and CORDINAL_DNS_PORT (the
// two settings net.cpp documents): A and AAAA answers, a CNAME whose target is compressed, a truncated
// answer fetched again over TCP, NXDOMAIN, SERVFAIL, silence with its bounded retry schedule, and the
// search list with ndots. Nothing leaves the machine: the responder is a socket in this process.
#pragma once
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <ifaddrs.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netpacket/packet.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace libc_checks {
namespace net_detail {

// The layouts net.cpp fills from the kernel's and glibc's own structures.
static_assert(sizeof(struct addrinfo) == 48, "net.cpp hands out glibc's struct addrinfo");
static_assert(offsetof(struct addrinfo, ai_flags) == 0 && offsetof(struct addrinfo, ai_family) == 4 &&
                  offsetof(struct addrinfo, ai_socktype) == 8 && offsetof(struct addrinfo, ai_protocol) == 12 &&
                  offsetof(struct addrinfo, ai_addrlen) == 16 && offsetof(struct addrinfo, ai_addr) == 24 &&
                  offsetof(struct addrinfo, ai_canonname) == 32 && offsetof(struct addrinfo, ai_next) == 40,
              "net.cpp writes the fields of glibc's struct addrinfo where glibc's have them");
static_assert(sizeof(struct ifaddrs) == 56, "net.cpp hands out glibc's struct ifaddrs");
static_assert(offsetof(struct ifaddrs, ifa_next) == 0 && offsetof(struct ifaddrs, ifa_name) == 8 &&
                  offsetof(struct ifaddrs, ifa_flags) == 16 && offsetof(struct ifaddrs, ifa_addr) == 24 &&
                  offsetof(struct ifaddrs, ifa_netmask) == 32 && offsetof(struct ifaddrs, ifa_data) == 48,
              "net.cpp writes the fields of glibc's struct ifaddrs where glibc's have them");
static_assert(sizeof(struct sockaddr) == 16 && sizeof(struct sockaddr_in) == 16 &&
                  offsetof(struct sockaddr_in, sin_port) == 2 && offsetof(struct sockaddr_in, sin_addr) == 4,
              "net.cpp writes glibc's struct sockaddr_in");
static_assert(sizeof(struct sockaddr_in6) == 28 && offsetof(struct sockaddr_in6, sin6_flowinfo) == 4 &&
                  offsetof(struct sockaddr_in6, sin6_addr) == 8 && offsetof(struct sockaddr_in6, sin6_scope_id) == 24,
              "net.cpp writes glibc's struct sockaddr_in6");
static_assert(sizeof(struct sockaddr_storage) == 128 && sizeof(struct sockaddr_un) == 110 &&
                  offsetof(struct sockaddr_un, sun_path) == 2 && sizeof(struct sockaddr_ll) == 20,
              "net.cpp reads the socket address families getnameinfo and getifaddrs take");
static_assert(sizeof(struct nlmsghdr) == 16 && sizeof(struct ifaddrmsg) == 8 && sizeof(struct ifinfomsg) == 16,
              "net.cpp walks the kernel's netlink records");
static_assert(INET_ADDRSTRLEN == 16 && INET6_ADDRSTRLEN == 46 && IFNAMSIZ == 16, "net.cpp's text buffers");

// x86-64 keeps short integers little-endian, so one swap turns a port into network order and back.
inline uint16_t SwapPort(uint16_t port) { return (uint16_t)(port >> 8 | port << 8); }

// glibc's own copies, reached past cordinal's hidden definitions.
struct GlibcNet {
    int (*getaddrinfo)(const char*, const char*, const struct addrinfo*, struct addrinfo**);
    void (*freeaddrinfo)(struct addrinfo*);
    int (*getnameinfo)(const struct sockaddr*, socklen_t, char*, socklen_t, char*, socklen_t, int);
    int (*getifaddrs)(struct ifaddrs**);
    void (*freeifaddrs)(struct ifaddrs*);
    int (*gethostname)(char*, size_t);
    const char* (*inet_ntop)(int, const void*, char*, socklen_t);
    int (*inet_pton)(int, const char*, void*);
};

inline bool FindGlibc(GlibcNet* glibc) {
    glibc->getaddrinfo = (int (*)(const char*, const char*, const struct addrinfo*, struct addrinfo**))
        dlsym(RTLD_DEFAULT, "getaddrinfo");
    glibc->freeaddrinfo = (void (*)(struct addrinfo*))dlsym(RTLD_DEFAULT, "freeaddrinfo");
    glibc->getnameinfo = (int (*)(const struct sockaddr*, socklen_t, char*, socklen_t, char*, socklen_t, int))
        dlsym(RTLD_DEFAULT, "getnameinfo");
    glibc->getifaddrs = (int (*)(struct ifaddrs**))dlsym(RTLD_DEFAULT, "getifaddrs");
    glibc->freeifaddrs = (void (*)(struct ifaddrs*))dlsym(RTLD_DEFAULT, "freeifaddrs");
    glibc->gethostname = (int (*)(char*, size_t))dlsym(RTLD_DEFAULT, "gethostname");
    glibc->inet_ntop = (const char* (*)(int, const void*, char*, socklen_t))dlsym(RTLD_DEFAULT, "inet_ntop");
    glibc->inet_pton = (int (*)(int, const char*, void*))dlsym(RTLD_DEFAULT, "inet_pton");
    return glibc->getaddrinfo && glibc->freeaddrinfo && glibc->getnameinfo && glibc->getifaddrs &&
           glibc->freeifaddrs && glibc->gethostname && glibc->inet_ntop && glibc->inet_pton;
}

// The address text of a family, printed by glibc so the comparison never leans on the code under test.
inline std::string AddressText(const GlibcNet& glibc, const void* bytes, int family) {
    char out[INET6_ADDRSTRLEN] = {};
    if (!glibc.inet_ntop(family, bytes, out, sizeof out)) return "?";
    return out;
}

// ---------------------------------------------------------------------------------------------
// Address text.

inline void CheckAddressText(const GlibcNet& glibc) {
    static const char* const kQuad[] = {
        "127.0.0.1", "0.0.0.0", "255.255.255.255", "8.8.4.4", "1.2.3.4",  "127.0.0.01", "1",         "1.2",
        "1.2.3",     "1.2.3.4.5", "256.1.1.1",     "01.2.3.4", "1.2.3.04", "1.2.3.4 ",   " 1.2.3.4",  "",
        ".",         "1..2.3",   "1.2.3.4x",       "1.2.3.-4", "0x7f.0.0.1", "127.0.0.1\n", "1.2.3.4.",
        "1111.2.3.4",
    };
    static const char* const kSix[] = {
        "::",            "::1",           "::2",           "::ffff:1.2.3.4", "::1.2.3.4",     "::0.0.0.2",
        "1:2:3:4:5:6:7:8", "1:2:3:4:5:6:1.2.3.4", "2001:db8::ff00:42:8329", "fe80::1", "::ffff:0:1.2.3.4",
        "1::",           "::ffff:0:0:1", "64:ff9b::1.2.3.4", "100::1",      "1:2:3:4:5:6:7::", "::255.255.255.255",
        "2001:DB8::1",   "0:0:0:0:0:0:0:1", "",           ":",             ":::",           "1:2:3:4:5:6:7:8:9",
        "::ffff:1.2.3.4.5", "1::2::3",   "1:2:3:4:5:6:7", "::ffff:1.2.3",   "1:2:3:4:5:6:7:8::", ":1:2:3:4:5:6:7",
        "1:2:3:4:5:6:7:8:", "::1%1",    "12345::",       "fffff::",        "::g",           "1:2:3:4:5:6:7:8 ",
    };
    for (const char* text : kQuad) {
        unsigned char mine[16], theirs[16];
        memset(mine, 0xaa, sizeof mine);
        memset(theirs, 0xaa, sizeof theirs);
        errno = 0;
        const int mineResult = inet_pton(AF_INET, text, mine);
        const int mineError = errno;
        errno = 0;
        const int theirResult = glibc.inet_pton(AF_INET, text, theirs);
        const int theirError = errno;
        const bool same = mineResult == theirResult && (mineResult != 1 || !memcmp(mine, theirs, 4)) &&
                          (theirResult == 1 || !memcmp(mine, theirs, sizeof mine));
        if (!same || (theirResult != 1 && mineError != theirError)) {
            if (failures < 40)
                fprintf(stderr, "FAIL inet_pton(AF_INET, \"%s\") = %d errno %d, glibc %d errno %d\n", text, mineResult,
                        mineError, theirResult, theirError);
            ++failures;
        }
    }
    for (const char* text : kSix) {
        unsigned char mine[16], theirs[16];
        memset(mine, 0xaa, sizeof mine);
        memset(theirs, 0xaa, sizeof theirs);
        errno = 0;
        const int mineResult = inet_pton(AF_INET6, text, mine);
        const int mineError = errno;
        errno = 0;
        const int theirResult = glibc.inet_pton(AF_INET6, text, theirs);
        const int theirError = errno;
        const bool same = mineResult == theirResult && (mineResult != 1 || !memcmp(mine, theirs, sizeof mine)) &&
                          (theirResult == 1 || !memcmp(mine, theirs, sizeof mine));
        if (!same || (theirResult != 1 && mineError != theirError)) {
            if (failures < 40)
                fprintf(stderr, "FAIL inet_pton(AF_INET6, \"%s\") = %d errno %d, glibc %d errno %d\n", text, mineResult,
                        mineError, theirResult, theirError);
            ++failures;
        }
    }
    errno = 0;
    Expect(inet_pton(99, "1.2.3.4", nullptr) == -1 && errno == EAFNOSUPPORT, "inet_pton of an unknown family is EAFNOSUPPORT");
    errno = 0;
    Expect(inet_ntop(99, "\0\0\0\0", nullptr, 0) == nullptr && errno == EAFNOSUPPORT,
           "inet_ntop of an unknown family is EAFNOSUPPORT");

    // The text of every address the parser produced, plus patterns only the printer reaches.
    static const uint8_t kAddresses[][16] = {
        {},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 1, 2, 3, 4},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
        {0xfe, 0xc0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
        {0xfc, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
        {0x20, 1, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0xff, 0, 0x42, 0, 0x83, 0x29},
        {0x20, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
        {0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        {0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16},
        {0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        {0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0},
        {0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
        {0x00, 0x64, 0xff, 0x9b, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xfe, 0x80, 0, 0, 1},
    };
    for (const auto& address : kAddresses) {
        char mine[INET6_ADDRSTRLEN + 4] = {}, theirs[INET6_ADDRSTRLEN + 4] = {};
        errno = 0;
        const char* mineResult = inet_ntop(AF_INET6, address, mine, sizeof mine);
        const int mineError = errno;
        errno = 0;
        const char* theirResult = glibc.inet_ntop(AF_INET6, address, theirs, sizeof theirs);
        const int theirError = errno;
        if (!mineResult || !theirResult || strcmp(mine, theirs) || mineError != theirError) {
            if (failures < 40)
                fprintf(stderr, "FAIL inet_ntop(AF_INET6) = %s (errno %d), glibc %s\n", mineResult ? mine : "(null)",
                        mineError, theirResult ? theirs : "(null)");
            ++failures;
        }
    }
    for (int part = 0; part < 256; ++part) {
        const uint8_t address[4] = {127, 0, 0, (uint8_t)part};
        char mine[INET_ADDRSTRLEN + 4] = {}, theirs[INET_ADDRSTRLEN + 4] = {};
        const char* mineResult = inet_ntop(AF_INET, address, mine, sizeof mine);
        const char* theirResult = glibc.inet_ntop(AF_INET, address, theirs, sizeof theirs);
        if (!mineResult || !theirResult || strcmp(mine, theirs)) {
            if (failures < 40)
                fprintf(stderr, "FAIL inet_ntop(AF_INET, 127.0.0.%d) = %s, glibc %s\n", part,
                        mineResult ? mine : "(null)", theirResult ? theirs : "(null)");
            ++failures;
        }
    }
    // A buffer too small fails the same way: NULL, ENOSPC, and nothing written.
    const uint8_t six[16] = {0x20, 1, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    const uint8_t four[4] = {192, 168, 0, 1};
    for (socklen_t size = 0; size <= INET6_ADDRSTRLEN + 1; ++size) {
        char mine[INET6_ADDRSTRLEN + 2] = {}, theirs[INET6_ADDRSTRLEN + 2] = {};
        memset(mine, 0x5a, sizeof mine);
        memset(theirs, 0x5a, sizeof theirs);
        errno = 0;
        const char* mineResult = inet_ntop(AF_INET6, six, mine, size);
        const int mineError = errno;
        errno = 0;
        const char* theirResult = glibc.inet_ntop(AF_INET6, six, theirs, size);
        const int theirError = errno;
        const bool same = (mineResult == nullptr) == (theirResult == nullptr) &&
                          (mineResult != nullptr || (mineError == ENOSPC && theirError == ENOSPC)) &&
                          (mineResult == nullptr || !strcmp(mine, theirs));
        if (!same) {
            if (failures < 40)
                fprintf(stderr, "FAIL inet_ntop(AF_INET6, size %u) = %s errno %d, glibc %s errno %d\n", (unsigned)size,
                        mineResult ? mine : "(null)", mineError, theirResult ? theirs : "(null)", theirError);
            ++failures;
        }
        char mineQuad[INET_ADDRSTRLEN + 2] = {}, theirQuad[INET_ADDRSTRLEN + 2] = {};
        errno = 0;
        const char* mineText = inet_ntop(AF_INET, four, mineQuad, size < 20 ? size : 20);
        const int mineQuadError = errno;
        errno = 0;
        const char* theirText = glibc.inet_ntop(AF_INET, four, theirQuad, size < 20 ? size : 20);
        const int theirQuadError = errno;
        if ((mineText == nullptr) != (theirText == nullptr) ||
            (mineText != nullptr && strcmp(mineText, theirText)) ||
            (mineText == nullptr && (mineQuadError != theirQuadError || mineQuadError != ENOSPC))) {
            if (failures < 40)
                fprintf(stderr, "FAIL inet_ntop(AF_INET, size %u) = %s, glibc %s\n", (unsigned)size,
                        mineText ? mineQuad : "(null)", theirText ? theirQuad : "(null)");
            ++failures;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// gethostname.

inline void CheckHostname(const GlibcNet& glibc) {
    const size_t sizes[] = {0, 1, 2, 8, 63, 64, 65, 128, 260, 300};
    for (size_t size : sizes) {
        char mine[320], theirs[320];
        memset(mine, 0x5a, sizeof mine);
        memset(theirs, 0x5a, sizeof theirs);
        errno = 0;
        const int mineResult = gethostname(mine, size);
        const int mineError = errno;
        errno = 0;
        const int theirResult = glibc.gethostname(theirs, size);
        const int theirError = errno;
        if (mineResult != theirResult || mineError != theirError || memcmp(mine, theirs, sizeof mine)) {
            if (failures < 40)
                fprintf(stderr, "FAIL gethostname(%zu) = %d errno %d, glibc %d errno %d\n", size, mineResult,
                        mineError, theirResult, theirError);
            ++failures;
        }
    }
    char name[65] = {};
    Expect(gethostname(name, sizeof name) == 0 && name[0], "gethostname names this machine");
}

// ---------------------------------------------------------------------------------------------
// getaddrinfo, entry for entry.

inline std::string DescribeEntry(const GlibcNet& glibc, const struct addrinfo* entry) {
    char line[320];
    if (entry->ai_family == AF_INET && entry->ai_addrlen >= sizeof(struct sockaddr_in)) {
        const auto* address = (const struct sockaddr_in*)entry->ai_addr;
        snprintf(line, sizeof line, "v4 %s port %u type %d proto %d canon %s",
                 AddressText(glibc, &address->sin_addr, AF_INET).c_str(), SwapPort(address->sin_port),
                 entry->ai_socktype, entry->ai_protocol, entry->ai_canonname ? entry->ai_canonname : "-");
    } else if (entry->ai_family == AF_INET6 && entry->ai_addrlen >= sizeof(struct sockaddr_in6)) {
        const auto* address = (const struct sockaddr_in6*)entry->ai_addr;
        snprintf(line, sizeof line, "v6 %s port %u type %d proto %d scope %u canon %s",
                 AddressText(glibc, &address->sin6_addr, AF_INET6).c_str(), SwapPort(address->sin6_port),
                 entry->ai_socktype, entry->ai_protocol, address->sin6_scope_id,
                 entry->ai_canonname ? entry->ai_canonname : "-");
    } else {
        snprintf(line, sizeof line, "family %d type %d proto %d", entry->ai_family, entry->ai_socktype,
                 entry->ai_protocol);
    }
    return line;
}

struct LookupShape {
    int status = 0;
    std::vector<std::string> entries;
};

inline void QueryLookup(const GlibcNet& glibc, bool theirs, const char* node, const char* service,
                        const struct addrinfo* hints, LookupShape* shape) {
    struct addrinfo* list = nullptr;
    shape->status = theirs ? glibc.getaddrinfo(node, service, hints, &list)
                           : getaddrinfo(node, service, hints, &list);
    for (const struct addrinfo* entry = list; entry; entry = entry->ai_next)
        shape->entries.push_back(DescribeEntry(glibc, entry));
    if (list) {
        if (theirs) {
            glibc.freeaddrinfo(list);
        } else {
            freeaddrinfo(list);
        }
    }
}

inline void CompareLookup(const GlibcNet& glibc, const char* node, const char* service, int family, int flags,
                          int socktype, int protocol) {
    struct addrinfo hints = {};
    hints.ai_family = family;
    hints.ai_flags = flags;
    hints.ai_socktype = socktype;
    hints.ai_protocol = protocol;
    LookupShape mine, theirs;
    QueryLookup(glibc, false, node, service, &hints, &mine);
    QueryLookup(glibc, true, node, service, &hints, &theirs);
    if (mine.status != theirs.status || mine.entries != theirs.entries) {
        if (failures < 40) {
            fprintf(stderr, "FAIL getaddrinfo(%s, %s, family %d, flags %#x, type %d, proto %d): %d", node ? node : "NULL",
                    service ? service : "NULL", family, flags, socktype, protocol, mine.status);
            for (const auto& entry : mine.entries) fprintf(stderr, " [%s]", entry.c_str());
            fprintf(stderr, ", glibc %d", theirs.status);
            for (const auto& entry : theirs.entries) fprintf(stderr, " [%s]", entry.c_str());
            fprintf(stderr, "\n");
        }
        ++failures;
    }
}

inline void CheckAddressLookup(const GlibcNet& glibc) {
    // The numeric forms, with the four ways of writing a v4 address and the families each answers.
    CompareLookup(glibc, "127.0.0.1", nullptr, AF_UNSPEC, 0, 0, 0);
    CompareLookup(glibc, "127.0.0.1", "80", AF_UNSPEC, 0, 0, 0);
    CompareLookup(glibc, "127.0.0.1", "80", AF_INET, 0, SOCK_STREAM, IPPROTO_TCP);
    CompareLookup(glibc, "127.0.0.1", nullptr, AF_INET6, 0, 0, 0);
    CompareLookup(glibc, "127.0.0.1", nullptr, AF_INET6, AI_V4MAPPED, 0, 0);
    CompareLookup(glibc, "127.0.0.1", nullptr, AF_INET6, AI_V4MAPPED | AI_ALL, 0, 0);
    CompareLookup(glibc, "127.0.0.1", nullptr, AF_UNSPEC, AI_CANONNAME, 0, 0);
    CompareLookup(glibc, "127.1", nullptr, AF_UNSPEC, 0, 0, 0);
    CompareLookup(glibc, "127", nullptr, AF_UNSPEC, 0, 0, 0);
    CompareLookup(glibc, "0x7f.1", nullptr, AF_UNSPEC, 0, 0, 0);
    CompareLookup(glibc, "010.0.0.1", nullptr, AF_UNSPEC, 0, 0, 0);
    CompareLookup(glibc, "255.255.255.255", nullptr, AF_UNSPEC, 0, 0, 0);
    CompareLookup(glibc, "::1", nullptr, AF_UNSPEC, 0, 0, 0);
    CompareLookup(glibc, "::1", nullptr, AF_INET, 0, 0, 0);
    CompareLookup(glibc, "::ffff:127.0.0.1", nullptr, AF_INET, 0, 0, 0);
    CompareLookup(glibc, "::ffff:127.0.0.1", nullptr, AF_UNSPEC, 0, 0, 0);
    CompareLookup(glibc, "fe80::1%lo", nullptr, AF_UNSPEC, 0, 0, 0);
    CompareLookup(glibc, "::1", "80", AF_INET6, 0, 0, 0);
    // The wildcard and loopback pairs for a null name.
    CompareLookup(glibc, nullptr, "80", AF_UNSPEC, AI_PASSIVE, 0, 0);
    CompareLookup(glibc, nullptr, "80", AF_UNSPEC, 0, 0, 0);
    CompareLookup(glibc, nullptr, "80", AF_INET6, 0, 0, 0);
    CompareLookup(glibc, nullptr, "80", AF_UNSPEC, AI_V4MAPPED, 0, 0);
    CompareLookup(glibc, nullptr, "80", AF_INET, AI_PASSIVE | AI_CANONNAME, 0, 0);
    CompareLookup(glibc, nullptr, nullptr, AF_UNSPEC, AI_PASSIVE, 0, 0);
    // /etc/hosts: the first line a name appears on answers, in the family asked for.
    CompareLookup(glibc, "localhost", "80", AF_INET, 0, 0, 0);
    CompareLookup(glibc, "localhost", nullptr, AF_UNSPEC, AI_ADDRCONFIG, 0, 0);
    CompareLookup(glibc, "LOCALHOST", nullptr, AF_INET, 0, 0, 0);
    CompareLookup(glibc, "ip6-localhost", nullptr, AF_UNSPEC, 0, 0, 0);
    CompareLookup(glibc, "ip6-localhost", nullptr, AF_INET, 0, 0, 0);
    CompareLookup(glibc, "localhost", nullptr, AF_INET, AI_CANONNAME, 0, 0);
    CompareLookup(glibc, "localhost", nullptr, AF_INET6, AI_V4MAPPED, 0, 0);
    // /etc/services: a name that has a port for more than one socket type, and the type and protocol
    // filters over it.
    CompareLookup(glibc, "localhost", "http", AF_INET, 0, 0, 0);
    CompareLookup(glibc, "localhost", "http", AF_INET, 0, SOCK_DGRAM, IPPROTO_UDP);
    CompareLookup(glibc, "localhost", "http", AF_INET, 0, SOCK_RAW, 0);
    CompareLookup(glibc, "localhost", "http", AF_INET, 0, SOCK_DGRAM, IPPROTO_TCP);
    CompareLookup(glibc, "localhost", "http", AF_INET, AI_PASSIVE, 0, 0);
    CompareLookup(glibc, "", nullptr, AF_INET, 0, 0, 0);
    CompareLookup(glibc, "localhost", "", AF_INET, 0, 0, 0);
    CompareLookup(glibc, "localhost", "65536", AF_INET, 0, 0, 0);
    CompareLookup(glibc, "localhost", "4294967296", AF_INET, 0, 0, 0);
    CompareLookup(glibc, "localhost", "-1", AF_INET, 0, 0, 0);
    CompareLookup(glibc, "localhost", "+80", AF_INET, 0, 0, 0);
    CompareLookup(glibc, "localhost", " 80", AF_INET, 0, 0, 0);
    CompareLookup(glibc, "localhost", "80 ", AF_INET, 0, 0, 0);
    CompareLookup(glibc, "localhost", "0x10", AF_INET, 0, 0, 0);
    CompareLookup(glibc, "localhost", "nosuchservice", AF_INET, 0, 0, 0);
    CompareLookup(glibc, "localhost", "http", AF_INET, AI_NUMERICSERV, 0, 0);
    CompareLookup(glibc, "localhost", "80", AF_INET, AI_NUMERICSERV, 0, 0);
    // The answers that never reach a source.
    CompareLookup(glibc, "localhost", nullptr, AF_UNSPEC, AI_NUMERICHOST, 0, 0);
    CompareLookup(glibc, "localhost", nullptr, 99, 0, 0, 0);
    CompareLookup(glibc, "localhost", nullptr, AF_INET, 0x800000, 0, 0);
    CompareLookup(glibc, "localhost", nullptr, AF_INET, 0x300, 0, 0);
    CompareLookup(glibc, "127.0.0.1", "80", AF_INET, AI_CANONNAME | AI_NUMERICHOST, 0, 0);
    Expect(InProgram((const void*)&getaddrinfo) && !InProgram((const void*)glibc.getaddrinfo) &&
               !InProgram((const void*)glibc.freeaddrinfo),
           "getaddrinfo and freeaddrinfo are cordinal's own");
}

// ---------------------------------------------------------------------------------------------
// getnameinfo.

struct NameShape {
    int status = 0;
    std::string host;
    std::string serv;
};

inline void QueryName(const GlibcNet& glibc, bool theirs, const struct sockaddr* address, socklen_t length,
                      socklen_t hostLength, socklen_t servLength, int flags, NameShape* shape) {
    char host[1120], serv[128];
    memset(host, 0x5a, sizeof host);
    memset(serv, 0x5a, sizeof serv);
    shape->status = theirs ? glibc.getnameinfo(address, length, hostLength ? host : nullptr, hostLength,
                                               servLength ? serv : nullptr, servLength, flags)
                           : getnameinfo(address, length, hostLength ? host : nullptr, hostLength,
                                         servLength ? serv : nullptr, servLength, flags);
    shape->host.assign(hostLength ? host : "", hostLength ? strnlen(host, hostLength) : 0);
    shape->serv.assign(servLength ? serv : "", servLength ? strnlen(serv, servLength) : 0);
}

inline void CompareName(const GlibcNet& glibc, const struct sockaddr* address, socklen_t length, socklen_t hostLength,
                        socklen_t servLength, int flags, const char* what) {
    NameShape mine, theirs;
    QueryName(glibc, false, address, length, hostLength, servLength, flags, &mine);
    QueryName(glibc, true, address, length, hostLength, servLength, flags, &theirs);
    if (mine.status != theirs.status || mine.host != theirs.host || mine.serv != theirs.serv) {
        if (failures < 40)
            fprintf(stderr, "FAIL getnameinfo(%s): %d [%s] [%s], glibc %d [%s] [%s]\n", what, mine.status,
                    mine.host.c_str(), mine.serv.c_str(), theirs.status, theirs.host.c_str(), theirs.serv.c_str());
        ++failures;
    }
}

inline void CheckNameLookup(const GlibcNet& glibc) {
    struct sockaddr_in v4 = {};
    v4.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &v4.sin_addr);
    v4.sin_port = SwapPort(80);
    CompareName(glibc, (const struct sockaddr*)&v4, sizeof v4, 64, 32, 0, "127.0.0.1:80");
    CompareName(glibc, (const struct sockaddr*)&v4, sizeof v4, 64, 32, NI_NUMERICHOST, "127.0.0.1 numeric host");
    CompareName(glibc, (const struct sockaddr*)&v4, sizeof v4, 64, 32, NI_NUMERICSERV, "127.0.0.1 numeric serv");
    CompareName(glibc, (const struct sockaddr*)&v4, sizeof v4, 64, 32, NI_NUMERICHOST | NI_NUMERICSERV, "both numeric");
    CompareName(glibc, (const struct sockaddr*)&v4, sizeof v4, 64, 32, NI_DGRAM, "NI_DGRAM");
    CompareName(glibc, (const struct sockaddr*)&v4, sizeof v4, 64, 32, NI_NOFQDN, "NI_NOFQDN");
    CompareName(glibc, (const struct sockaddr*)&v4, sizeof v4, 4, 32, NI_NUMERICHOST, "short host buffer");
    CompareName(glibc, (const struct sockaddr*)&v4, sizeof v4, 64, 3, 0, "short serv buffer");
    CompareName(glibc, (const struct sockaddr*)&v4, sizeof v4, 0, 0, 0, "no buffers");
    CompareName(glibc, (const struct sockaddr*)&v4, sizeof v4, 64, 32, NI_NAMEREQD, "NI_NAMEREQD");
    CompareName(glibc, (const struct sockaddr*)&v4, sizeof(struct sockaddr_in) - 2, 64, 32, 0, "short address");
    CompareName(glibc, (const struct sockaddr*)&v4, sizeof v4, 64, 32, 0x8000, "bad flags");
    for (uint16_t port : {(uint16_t)0, (uint16_t)22, (uint16_t)443, (uint16_t)631, (uint16_t)4464, (uint16_t)65535,
                          (uint16_t)5353}) {
        v4.sin_port = SwapPort(port);
        CompareName(glibc, (const struct sockaddr*)&v4, sizeof v4, 64, 32, NI_NUMERICHOST, "a port with a name");
        CompareName(glibc, (const struct sockaddr*)&v4, sizeof v4, 64, 32, NI_NUMERICHOST | NI_DGRAM, "a udp port");
    }

    struct sockaddr_in6 v6 = {};
    v6.sin6_family = AF_INET6;
    v6.sin6_port = SwapPort(80);
    inet_pton(AF_INET6, "::1", &v6.sin6_addr);
    CompareName(glibc, (const struct sockaddr*)&v6, sizeof v6, 64, 32, 0, "::1");
    CompareName(glibc, (const struct sockaddr*)&v6, sizeof v6, 64, 32, NI_NUMERICHOST, "::1 numeric");
    inet_pton(AF_INET6, "fe80::1", &v6.sin6_addr);
    v6.sin6_scope_id = 1;
    CompareName(glibc, (const struct sockaddr*)&v6, sizeof v6, 64, 32, NI_NUMERICHOST, "link-local with the loopback's index");
    v6.sin6_scope_id = 7;
    CompareName(glibc, (const struct sockaddr*)&v6, sizeof v6, 64, 32, NI_NUMERICHOST, "link-local with no interface name");
    inet_pton(AF_INET6, "2001:db8::1", &v6.sin6_addr);
    v6.sin6_scope_id = 3;
    CompareName(glibc, (const struct sockaddr*)&v6, sizeof v6, 64, 32, NI_NUMERICHOST, "global with a scope id");
    v6.sin6_scope_id = 0;
    CompareName(glibc, (const struct sockaddr*)&v6, sizeof v6, 6, 32, NI_NUMERICHOST, "short v6 host buffer");

    struct sockaddr_un un = {};
    un.sun_family = AF_UNIX;
    strcpy(un.sun_path, "/tmp/cordinal-check.sock");
    CompareName(glibc, (const struct sockaddr*)&un, sizeof un, 64, 64, 0, "AF_UNIX");
    CompareName(glibc, (const struct sockaddr*)&un, sizeof un, 64, 64, NI_NAMEREQD, "AF_UNIX with NI_NAMEREQD");
    CompareName(glibc, (const struct sockaddr*)&un, sizeof un, 64, 64, NI_NUMERICHOST, "AF_UNIX numeric");

    struct sockaddr other = {};
    other.sa_family = 99;
    CompareName(glibc, &other, sizeof other, 64, 32, 0, "an unknown family");
    CompareName(glibc, &other, 1, 64, 32, 0, "an address too short to hold a family");
    NameShape none, theirsNone;
    QueryName(glibc, false, nullptr, 0, 64, 32, 0, &none);
    QueryName(glibc, true, nullptr, 0, 64, 32, 0, &theirsNone);
    Expect(none.status == theirsNone.status && none.status == EAI_FAMILY, "getnameinfo of a null address is EAI_FAMILY");
    Expect(InProgram((const void*)&getnameinfo) && !InProgram((const void*)glibc.getnameinfo),
           "getnameinfo is cordinal's own");
}

// ---------------------------------------------------------------------------------------------
// getifaddrs.

inline std::string DescribeInterface(const GlibcNet& glibc, const struct ifaddrs* entry) {
    char line[512] = {};
    char addressText[INET6_ADDRSTRLEN + 16] = "-";
    char maskText[INET6_ADDRSTRLEN + 4] = "-", broadText[INET6_ADDRSTRLEN + 4] = "-";
    const int family = entry->ifa_addr ? entry->ifa_addr->sa_family : -1;
    auto describe = [&glibc](const struct sockaddr* address, char* out, size_t size) {
        if (!address) return;
        if (address->sa_family == AF_INET) {
            const auto* v4 = (const struct sockaddr_in*)address;
            snprintf(out, size, "%s", AddressText(glibc, &v4->sin_addr, AF_INET).c_str());
        } else if (address->sa_family == AF_INET6) {
            const auto* v6 = (const struct sockaddr_in6*)address;
            snprintf(out, size, "%s", AddressText(glibc, &v6->sin6_addr, AF_INET6).c_str());
        } else if (address->sa_family == AF_PACKET) {
            const auto* link = (const struct sockaddr_ll*)address;
            snprintf(out, size, "ll ifindex %d hw %u type %u %02x:%02x:%02x:%02x:%02x:%02x", link->sll_ifindex,
                     link->sll_halen, link->sll_hatype, link->sll_addr[0], link->sll_addr[1], link->sll_addr[2],
                     link->sll_addr[3], link->sll_addr[4], link->sll_addr[5]);
        } else {
            snprintf(out, size, "family %d", address->sa_family);
        }
    };
    describe(entry->ifa_addr, addressText, sizeof addressText);
    describe(entry->ifa_netmask, maskText, sizeof maskText);
    describe(entry->ifa_broadaddr, broadText, sizeof broadText);
    snprintf(line, sizeof line, "%s family %d flags %#x addr %s mask %s broad %s data %s",
             entry->ifa_name ? entry->ifa_name : "-", family, entry->ifa_flags, addressText, maskText, broadText,
             entry->ifa_data ? "yes" : "no");
    return line;
}

inline void CheckInterfaces(const GlibcNet& glibc) {
    struct ifaddrs* mine = nullptr;
    struct ifaddrs* theirs = nullptr;
    const int mineResult = getifaddrs(&mine);
    const int theirResult = glibc.getifaddrs(&theirs);
    Expect(mineResult == 0 && theirResult == 0, "getifaddrs answers, as glibc's does");
    if (mineResult == 0 && theirResult == 0) {
        std::vector<std::string> mineLines, theirLines;
        for (const struct ifaddrs* entry = mine; entry; entry = entry->ifa_next)
            mineLines.push_back(DescribeInterface(glibc, entry));
        for (const struct ifaddrs* entry = theirs; entry; entry = entry->ifa_next)
            theirLines.push_back(DescribeInterface(glibc, entry));
        if (mineLines != theirLines) {
            if (failures < 40) {
                fprintf(stderr, "FAIL getifaddrs: %zu entries, glibc %zu\n", mineLines.size(), theirLines.size());
                for (size_t i = 0; i < mineLines.size() || i < theirLines.size(); ++i)
                    fprintf(stderr, "  [%zu] %s | glibc %s\n", i, i < mineLines.size() ? mineLines[i].c_str() : "-",
                            i < theirLines.size() ? theirLines[i].c_str() : "-");
            }
            ++failures;
        }
        // The loopback address and its netmask, which every Linux machine has.
        // The loopback address and its netmask, which every Linux machine has. 0x0100007f is the four
        // bytes of 127.0.0.1 as sin_addr keeps them on x86-64.
        bool loopback = false;
        for (const struct ifaddrs* entry = mine; entry; entry = entry->ifa_next) {
            const auto* address = (const struct sockaddr_in*)entry->ifa_addr;
            if (address && address->sin_family == AF_INET && address->sin_addr.s_addr == 0x0100007f)
                loopback = entry->ifa_netmask && entry->ifa_netmask->sa_family == AF_INET;
        }
        Expect(loopback, "getifaddrs names the loopback with its netmask");
        freeifaddrs(mine);
        glibc.freeifaddrs(theirs);
    }
    Expect(if_nametoindex("lo") > 0, "if_nametoindex finds the loopback");
    Expect(InProgram((const void*)&getifaddrs) && !InProgram((const void*)glibc.getifaddrs) &&
               !InProgram((const void*)glibc.freeifaddrs),
           "getifaddrs and freeifaddrs are cordinal's own");
}

// ---------------------------------------------------------------------------------------------
// DNS, through a responder on 127.0.0.1. net.cpp reads CORDINAL_RESOLV_CONF in place of
// /etc/resolv.conf and asks CORDINAL_DNS_PORT instead of 53, so the check needs neither the system's
// resolver nor a privileged port; it writes its own resolv.conf into the check's directory on every
// case, so the pacing of each answer is the one under test.

class DnsResponder {
  public:
    enum Mode {
        kAddresses,     // A 127.0.0.2 and AAAA 2001:db8::53, one record per question
        kCname,         // CNAME target.cordinal.test whose target is a compression pointer, then A 127.0.0.3
        kTruncated,     // the truncated bit over UDP, A 127.0.0.9 over TCP
        kNoSuchName,    // rcode 3
        kServerFailure, // rcode 2
        kEmpty,         // rcode 0 and no answers
        kSilent,        // nothing at all
        kSearchOnly,    // A 127.0.0.4 under cordinal.test, rcode 3 for anything else
    };

    DnsResponder() = default;
    ~DnsResponder() { Stop(); }
    DnsResponder(const DnsResponder&) = delete;
    DnsResponder& operator=(const DnsResponder&) = delete;

    bool Start() {
        udp_ = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (udp_ < 0) return false;
        struct sockaddr_in address = {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = 0x0100007f;  // 127.0.0.1, as the four bytes sin_addr keeps
        address.sin_port = 0;
        if (bind(udp_, (struct sockaddr*)&address, sizeof address) != 0) return false;
        socklen_t length = sizeof address;
        if (getsockname(udp_, (struct sockaddr*)&address, &length) != 0) return false;
        port_ = SwapPort(address.sin_port);
        tcp_ = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (tcp_ < 0) return false;
        const int yes = 1;
        setsockopt(tcp_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
        if (bind(tcp_, (struct sockaddr*)&address, sizeof address) != 0 || listen(tcp_, 4) != 0) return false;
        thread_ = std::thread([this] { Run(); });
        return true;
    }

    void Stop() {
        if (quit_.exchange(true)) return;
        if (thread_.joinable()) thread_.join();
        if (udp_ >= 0) close(udp_);
        if (tcp_ >= 0) close(tcp_);
        udp_ = tcp_ = -1;
    }

    int port() const { return port_; }
    void SetMode(Mode mode) { mode_.store(mode); }
    // The one name kSearchOnly answers, so the search list's order is visible: everything else is
    // NXDOMAIN.
    void SetAccept(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex_);
        accept_ = name;
    }
    void Reset() {
        udpQueries_.store(0);
        tcpQueries_.store(0);
        std::lock_guard<std::mutex> lock(mutex_);
        questions_.clear();
    }
    int udpQueries() const { return udpQueries_.load(); }
    int tcpQueries() const { return tcpQueries_.load(); }
    std::vector<std::string> Questions() {
        std::lock_guard<std::mutex> lock(mutex_);
        return questions_;
    }

  private:
    struct Query {
        std::string name;
        int type = 0;
        int questionEnd = 0;
        int suffix = -1;  // where "cordinal" starts in the question, for a compression pointer
    };

    void Run() {
        while (!quit_.load()) {
            struct pollfd fds[2] = {{udp_, POLLIN, 0}, {tcp_, POLLIN, 0}};
            if (poll(fds, 2, 10) <= 0) continue;
            if (fds[0].revents & POLLIN) {
                uint8_t query[512];
                struct sockaddr_storage from = {};
                socklen_t fromLength = sizeof from;
                const long got = recvfrom(udp_, query, sizeof query, 0, (struct sockaddr*)&from, &fromLength);
                if (got > 12) {
                    udpQueries_.fetch_add(1);
                    std::vector<uint8_t> reply;
                    if (Answer(query, (int)got, false, &reply))
                        sendto(udp_, reply.data(), reply.size(), 0, (struct sockaddr*)&from, fromLength);
                }
            }
            if (fds[1].revents & POLLIN) {
                const int client = accept(tcp_, nullptr, nullptr);
                if (client >= 0) {
                    ServeTcp(client);
                    close(client);
                }
            }
        }
    }

    void ServeTcp(int client) {
        struct timeval timeout = {2, 0};
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
        uint8_t header[2] = {};
        if (ReadExactly(client, header, 2) != 2) return;
        const int wanted = (header[0] << 8) | header[1];
        if (wanted < 12 || wanted > 512) return;
        uint8_t query[512];
        if (ReadExactly(client, query, wanted) != wanted) return;
        tcpQueries_.fetch_add(1);
        std::vector<uint8_t> reply;
        if (!Answer(query, wanted, true, &reply)) return;
        uint8_t prefix[2] = {(uint8_t)(reply.size() >> 8), (uint8_t)reply.size()};
        if (write(client, prefix, 2) == 2) write(client, reply.data(), reply.size());
    }

    static long ReadExactly(int fd, uint8_t* buffer, int length) {
        int got = 0;
        while (got < length) {
            const long amount = read(fd, buffer + got, (size_t)(length - got));
            if (amount <= 0) return amount < 0 && errno == EINTR ? 0 : got;
            got += (int)amount;
        }
        return got;
    }

    static bool ParseQuery(const uint8_t* query, int length, Query* out) {
        if (length < 13) return false;
        int at = 12;
        for (;;) {
            if (at >= length) return false;
            const int label = query[at];
            if (label == 0) {
                ++at;
                break;
            }
            if (label > 63 || at + 1 + label > length) return false;
            if (out->suffix < 0 && label == 8 && !memcmp(query + at + 1, "cordinal", 8)) out->suffix = at;
            if (!out->name.empty()) out->name += '.';
            out->name.append((const char*)query + at + 1, (size_t)label);
            at += 1 + label;
        }
        if (at + 4 > length) return false;
        out->type = (query[at] << 8) | query[at + 1];
        out->questionEnd = at + 4;
        return true;
    }

    static void AddRawRecord(std::vector<uint8_t>* reply, int type, const uint8_t* data, size_t length) {
        static const uint8_t kOwner[] = {0xc0, 0x0c};  // the question's own name
        reply->insert(reply->end(), kOwner, kOwner + 2);
        reply->push_back(0);
        reply->push_back((uint8_t)type);
        reply->push_back(0);
        reply->push_back(1);  // class IN
        reply->push_back(0);
        reply->push_back(0);
        reply->push_back(0);
        reply->push_back(60);  // a one-minute ttl
        reply->push_back((uint8_t)(length >> 8));
        reply->push_back((uint8_t)length);
        reply->insert(reply->end(), data, data + length);
        reply->at(7) += 1;  // ancount, low byte
    }

    static void AddRecord(std::vector<uint8_t>* reply, int type, std::initializer_list<uint8_t> data) {
        AddRawRecord(reply, type, data.begin(), data.size());
    }

    bool Answer(const uint8_t* query, int length, bool tcp, std::vector<uint8_t>* reply) {
        Query parsed;
        if (!ParseQuery(query, length, &parsed)) return false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            questions_.push_back(parsed.name);
        }
        const Mode mode = mode_.load();
        if (mode == kSilent) return false;
        reply->assign(query, query + parsed.questionEnd);
        (*reply)[2] = 0x81;  // a response, recursion desired
        (*reply)[3] = 0x80;  // recursion available
        (*reply)[6] = 0;
        (*reply)[7] = 0;
        switch (mode) {
            case kAddresses:
                if (parsed.type == 1) {
                    AddRecord(reply, 1, {127, 0, 0, 2});
                } else if (parsed.type == 28) {
                    AddRecord(reply, 28, {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x53});
                }
                break;
            case kCname: {
                // "target" then a pointer to "cordinal.test" in the question: a name whose middle is
                // compressed, which only a reader that follows pointers can spell out.
                std::vector<uint8_t> target = {6, 't', 'a', 'r', 'g', 'e', 't'};
                if (parsed.suffix >= 0 && parsed.suffix < 0x4000) {
                    target.push_back((uint8_t)(0xc0 | (parsed.suffix >> 8)));
                    target.push_back((uint8_t)parsed.suffix);
                } else {
                    target.insert(target.end(), {8, 'c', 'o', 'r', 'd', 'i', 'n', 'a', 'l', 4, 't', 'e', 's', 't', 0});
                }
                AddRawRecord(reply, 5, target.data(), target.size());
                AddRecord(reply, 1, {127, 0, 0, 3});
                break;
            }
            case kTruncated:
                if (tcp) {
                    AddRecord(reply, 1, {127, 0, 0, 9});
                } else {
                    (*reply)[2] |= 0x02;  // truncated: the caller fetches it again over TCP
                }
                break;
            case kNoSuchName:
                (*reply)[3] |= 3;
                break;
            case kServerFailure:
                (*reply)[3] |= 2;
                break;
            case kEmpty:
                break;
            case kSearchOnly:
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (parsed.name == accept_ && parsed.type == 1) AddRecord(reply, 1, {127, 0, 0, 4});
                }
                if (reply->at(7) == 0) (*reply)[3] |= 3;
                break;
            case kSilent:
                break;
        }
        return true;
    }

    std::atomic<Mode> mode_{kAddresses};
    std::atomic<bool> quit_{false};
    std::atomic<int> udpQueries_{0};
    std::atomic<int> tcpQueries_{0};
    std::mutex mutex_;
    std::vector<std::string> questions_;
    std::string accept_;
    int udp_ = -1;
    int tcp_ = -1;
    int port_ = 0;
    std::thread thread_;
};

inline bool WriteCheckFile(const std::string& path, const std::string& text) {
    FILE* file = fopen(path.c_str(), "w");
    if (!file) return false;
    const bool wrote = fwrite(text.data(), 1, text.size(), file) == text.size();
    return fclose(file) == 0 && wrote;
}

struct DnsLookup {
    int status = 0;
    std::vector<std::string> addresses;
    std::string canon;
};

// A DNS assertion that says what the resolver answered when it fails.
inline void ExpectLookup(bool ok, const DnsLookup& found, const std::vector<std::string>& questions, const char* what) {
    if (ok) return;
    if (failures < 40) {
        fprintf(stderr, "FAIL %s: status %d, addresses [", what, found.status);
        for (const auto& address : found.addresses) fprintf(stderr, " %s", address.c_str());
        fprintf(stderr, " ], canon [%s], questions [", found.canon.c_str());
        for (const auto& question : questions) fprintf(stderr, " %s", question.c_str());
        fprintf(stderr, " ]\n");
    }
    ++failures;
}

inline DnsLookup LookupName(const GlibcNet& glibc, const char* name, int family, int flags) {
    struct addrinfo hints = {};
    hints.ai_family = family;
    hints.ai_flags = flags;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* list = nullptr;
    DnsLookup found;
    found.status = getaddrinfo(name, "80", &hints, &list);
    for (const struct addrinfo* entry = list; entry; entry = entry->ai_next) {
        if (entry->ai_family == AF_INET) {
            const auto* address = (const struct sockaddr_in*)entry->ai_addr;
            found.addresses.push_back(AddressText(glibc, &address->sin_addr, AF_INET));
        } else if (entry->ai_family == AF_INET6) {
            const auto* address = (const struct sockaddr_in6*)entry->ai_addr;
            found.addresses.push_back(AddressText(glibc, &address->sin6_addr, AF_INET6));
        }
        if (entry->ai_canonname && found.canon.empty()) found.canon = entry->ai_canonname;
    }
    if (list) freeaddrinfo(list);
    return found;
}

// A hosts file of the check's own, through CORDINAL_HOSTS: the rules /etc/hosts is read by that a
// machine's own file may not exercise. glibc cannot read this file, so the answers here are written
// out rather than compared.
inline void CheckHostsFile(const std::string& dir, const GlibcNet& glibc) {
    const std::string path = dir + "/hosts";
    const std::string text =
        "# a comment\n"
        "# 10.99.0.1 commented.test\n"
        "10.10.0.1 dup.test dupalias\n"
        "10.10.0.2\tdup.test\n"
        "10.10.0.3 v4only.test\n"
        "::ffff:10.10.0.4 mapped.test\n"
        "::1\tv6only.test\n"
        "2001:db8::1 v6host.test\n"
        "10.10.0.6\n";
    if (!WriteCheckFile(path, text)) {
        Expect(false, "the check writes its own hosts file");
        return;
    }
    setenv("CORDINAL_HOSTS", path.c_str(), 1);
    {  // Every matching line answers, in file order, with the first line's first name as the canon.
        DnsLookup found = LookupName(glibc, "dup.test", AF_UNSPEC, AI_CANONNAME);
        Expect(found.status == 0 && found.addresses == std::vector<std::string>{"10.10.0.1", "10.10.0.2"},
               "both lines a name appears on answer");
        Expect(found.canon == "dup.test", "the first matching line's first name is the canonical one");
    }
    {
        DnsLookup found = LookupName(glibc, "dupalias", AF_INET, 0);
        Expect(found.status == 0 && found.addresses == std::vector<std::string>{"10.10.0.1"},
               "an alias answers as its line's address");
    }
    {
        DnsLookup found = LookupName(glibc, "DUP.TEST", AF_INET, 0);
        Expect(found.status == 0 && found.addresses.size() == 2, "a name matches whatever its case");
    }
    {
        DnsLookup found = LookupName(glibc, "mapped.test", AF_INET, 0);
        Expect(found.status == 0 && found.addresses == std::vector<std::string>{"10.10.0.4"},
               "a v4-mapped line answers AF_INET with its v4 address");
    }
    {
        DnsLookup found = LookupName(glibc, "mapped.test", AF_INET6, 0);
        Expect(found.status == 0 && found.addresses == std::vector<std::string>{"::ffff:10.10.0.4"},
               "a v4-mapped line answers AF_INET6 as it stands");
    }
    {
        DnsLookup found = LookupName(glibc, "v4only.test", AF_INET6, AI_V4MAPPED);
        Expect(found.status == 0 && found.addresses == std::vector<std::string>{"::ffff:10.10.0.3"},
               "under AI_V4MAPPED a v4 line becomes its v4-mapped address");
    }
    {
        DnsLookup found = LookupName(glibc, "v6only.test", AF_INET, 0);
        Expect(found.status == 0 && found.addresses == std::vector<std::string>{"127.0.0.1"},
               "a v6 line's ::1 answers AF_INET as 127.0.0.1");
    }
    {
        DnsLookup found = LookupName(glibc, "v6only.test", AF_INET6, 0);
        Expect(found.status == 0 && found.addresses == std::vector<std::string>{"::1"}, "a v6 line answers AF_INET6");
    }
    {  // The reverse name, which reads the same file.
        char host[64] = {};
        struct sockaddr_in address = {};
        address.sin_family = AF_INET;
        address.sin_port = SwapPort(80);
        inet_pton(AF_INET, "10.10.0.2", &address.sin_addr);
        const int status = getnameinfo((const struct sockaddr*)&address, sizeof address, host, sizeof host, nullptr,
                                       0, NI_NUMERICHOST);
        Expect(status == 0 && !strcmp(host, "10.10.0.2"), "a numeric lookup skips the hosts file");
        const int named = getnameinfo((const struct sockaddr*)&address, sizeof address, host, sizeof host, nullptr, 0, 0);
        Expect(named == 0 && !strcmp(host, "dup.test"), "the hosts file names the address back");
    }
    unsetenv("CORDINAL_HOSTS");
    unlink(path.c_str());
}

inline void CheckDns(const std::string& dir, const GlibcNet& glibc) {
    DnsResponder responder;
    if (!responder.Start()) {
        Expect(false, "the check's DNS responder binds 127.0.0.1");
        return;
    }
    const std::string path = dir + "/resolv.conf";
    const std::string port = std::to_string(responder.port());
    setenv("CORDINAL_RESOLV_CONF", path.c_str(), 1);
    setenv("CORDINAL_DNS_PORT", port.c_str(), 1);
    const std::string quick = "nameserver 127.0.0.1\noptions timeout:1 attempts:2\n";
    Expect(WriteCheckFile(path, quick), "the check writes its own resolv.conf");

    // A and AAAA together, then each family on its own.
    responder.SetMode(DnsResponder::kAddresses);
    responder.Reset();
    {
        DnsLookup found = LookupName(glibc, "a.cordinal.test", AF_UNSPEC, AI_CANONNAME);
        std::vector<std::string> addresses = found.addresses;
        std::sort(addresses.begin(), addresses.end());
        Expect(found.status == 0 && addresses == std::vector<std::string>{"127.0.0.2", "2001:db8::53"},
               "getaddrinfo answers a name with both an A and an AAAA record");
        Expect(responder.Questions().size() == 2 && responder.Questions()[0] == "a.cordinal.test",
               "one lookup asks the A and AAAA questions together");
        Expect(found.canon == "a.cordinal.test", "without a CNAME the name asked for is the canonical one");
    }
    responder.Reset();
    {
        DnsLookup found = LookupName(glibc, "aaaa.cordinal.test", AF_INET6, 0);
        Expect(found.status == 0 && found.addresses == std::vector<std::string>{"2001:db8::53"} &&
                   responder.Questions().size() == 1,
               "a v6-only lookup asks one AAAA question and keeps its address");
    }
    responder.Reset();
    {
        DnsLookup found = LookupName(glibc, "v4.cordinal.test", AF_INET, 0);
        Expect(found.status == 0 && found.addresses == std::vector<std::string>{"127.0.0.2"} &&
                   responder.Questions().size() == 1,
               "a v4-only lookup asks one A question and keeps its address");
    }
    // A CNAME whose target is compressed in the answer, then the address of that target.
    responder.SetMode(DnsResponder::kCname);
    responder.Reset();
    {
        DnsLookup found = LookupName(glibc, "cname.cordinal.test", AF_INET, AI_CANONNAME);
        const std::vector<std::string> questions = responder.Questions();
        Expect(found.status == 0 && found.addresses == std::vector<std::string>{"127.0.0.3"},
               "a CNAME chain's address comes back");
        ExpectLookup(found.canon == "target.cordinal.test", found, questions,
                     "the canonical name is the CNAME's target, read through its pointer");
    }
    // A truncated UDP answer, fetched again over TCP.
    responder.SetMode(DnsResponder::kTruncated);
    responder.Reset();
    {
        DnsLookup found = LookupName(glibc, "tcp.cordinal.test", AF_INET, 0);
        Expect(found.status == 0 && found.addresses == std::vector<std::string>{"127.0.0.9"},
               "a truncated answer is fetched again over TCP");
        Expect(responder.tcpQueries() == 1 && responder.udpQueries() == 1,
               "the TCP fetch follows the truncated UDP answer");
    }
    // A nameserver that answers nothing at all costs the timeout, once, and is a temporary failure.
    responder.SetMode(DnsResponder::kSilent);
    responder.Reset();
    {
        const auto started = std::chrono::steady_clock::now();
        DnsLookup found = LookupName(glibc, "silent.cordinal.test", AF_INET, 0);
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
        Expect(found.status == EAI_AGAIN, "a name no server answers is EAI_AGAIN");
        Expect(elapsed.count() >= 900 && elapsed.count() < 2500,
               "the silence costs the configured timeout, not the timeout times the attempts");
        Expect(responder.udpQueries() == 2, "one question is sent once per attempt");
    }
    // The rest of the return codes.
    responder.SetMode(DnsResponder::kNoSuchName);
    responder.Reset();
    {
        DnsLookup found = LookupName(glibc, "missing.cordinal.test", AF_INET, 0);
        Expect(found.status == EAI_NONAME, "NXDOMAIN is EAI_NONAME");
        Expect(responder.udpQueries() == 1, "a negative answer is not asked again");
    }
    responder.SetMode(DnsResponder::kServerFailure);
    responder.Reset();
    {
        DnsLookup found = LookupName(glibc, "broken.cordinal.test", AF_INET, 0);
        Expect(found.status == EAI_AGAIN, "SERVFAIL is EAI_AGAIN");
        Expect(responder.udpQueries() >= 3 && responder.udpQueries() <= 4,
               "a SERVFAIL is asked again before it is taken as a failure");
    }
    responder.SetMode(DnsResponder::kEmpty);
    responder.Reset();
    {
        DnsLookup found = LookupName(glibc, "empty.cordinal.test", AF_INET, 0);
        Expect(found.status == EAI_NODATA, "an answer with no record of the type is EAI_NODATA");
    }
    // The search list: a name with fewer dots than ndots tries the suffix first, a name with as many
    // dots as ndots tries itself first, and a trailing dot asks for the name exactly.
    responder.SetMode(DnsResponder::kSearchOnly);
    responder.Reset();
    Expect(WriteCheckFile(path, "nameserver 127.0.0.1\nsearch cordinal.test\noptions ndots:2 timeout:1 attempts:1\n"),
           "the check writes a resolv.conf with a search list");
    responder.SetAccept("host.cordinal.test");
    {
        DnsLookup found = LookupName(glibc, "host", AF_INET, 0);
        const std::vector<std::string> questions = responder.Questions();
        ExpectLookup(found.status == 0 && found.addresses == std::vector<std::string>{"127.0.0.4"} &&
                         questions.size() == 1 && questions[0] == "host.cordinal.test",
                     found, questions, "the search suffix answers a name with no dots");
    }
    responder.Reset();
    responder.SetAccept("a.b.cordinal.test");
    {
        DnsLookup found = LookupName(glibc, "a.b", AF_INET, 0);
        const std::vector<std::string> questions = responder.Questions();
        ExpectLookup(found.status == 0 && found.addresses == std::vector<std::string>{"127.0.0.4"} &&
                         questions.size() == 1 && questions[0] == "a.b.cordinal.test",
                     found, questions, "a name with fewer dots than ndots asks the suffix first");
    }
    responder.Reset();
    responder.SetAccept("a.b.c.cordinal.test");
    {  // Two dots are as many as ndots asks for, so the name itself goes first.
        DnsLookup found = LookupName(glibc, "a.b.c", AF_INET, 0);
        const std::vector<std::string> questions = responder.Questions();
        ExpectLookup(found.status == 0 && found.addresses == std::vector<std::string>{"127.0.0.4"} &&
                         questions.size() == 2 && questions[0] == "a.b.c" && questions[1] == "a.b.c.cordinal.test",
                     found, questions, "a name with as many dots as ndots is asked on its own before the suffix");
    }
    responder.Reset();
    responder.SetAccept("only.cordinal.test");
    {
        DnsLookup found = LookupName(glibc, "only.cordinal.test.", AF_INET, 0);
        const std::vector<std::string> questions = responder.Questions();
        ExpectLookup(found.status == 0 && found.addresses == std::vector<std::string>{"127.0.0.4"} &&
                         questions.size() == 1 && questions[0] == "only.cordinal.test",
                     found, questions, "a name with a trailing dot is asked as it stands, without the suffix");
    }
    responder.Reset();
    responder.SetAccept("nothing.cordinal.test");
    {
        DnsLookup found = LookupName(glibc, "absent", AF_INET, 0);
        const std::vector<std::string> questions = responder.Questions();
        ExpectLookup(found.status == EAI_NONAME && questions.size() == 2 &&
                         questions[0] == "absent.cordinal.test" && questions[1] == "absent",
                     found, questions, "a name no source knows is EAI_NONAME, after the suffix and the name");
    }
    // The numeric hosts never reach the resolver, even with a resolver that answers nothing.
    responder.SetMode(DnsResponder::kSilent);
    responder.Reset();
    {
        DnsLookup found = LookupName(glibc, "127.0.0.1", AF_INET, 0);
        Expect(found.status == 0 && found.addresses == std::vector<std::string>{"127.0.0.1"} &&
                   responder.udpQueries() == 0 && responder.tcpQueries() == 0,
               "a numeric host asks no question");
    }
    // A resolver that answers no question is bounded even when the search list is longer.
    responder.SetMode(DnsResponder::kSilent);
    responder.Reset();
    {
        const auto started = std::chrono::steady_clock::now();
        DnsLookup found = LookupName(glibc, "slow", AF_INET, 0);
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
        Expect(found.status == EAI_AGAIN, "a dead resolver over a search list is EAI_AGAIN");
        Expect(elapsed.count() < 2500, "the search list does not multiply the timeout");
    }

    responder.Stop();
    unsetenv("CORDINAL_RESOLV_CONF");
    unsetenv("CORDINAL_DNS_PORT");
    unlink(path.c_str());
}

}  // namespace net_detail

inline void CheckNetwork(const std::string& dir) {
    net_detail::GlibcNet glibc{};
    if (!net_detail::FindGlibc(&glibc)) {
        Expect(false, "glibc's network name functions are found through dlsym");
        return;
    }
    Expect(InProgram((const void*)&inet_pton) && InProgram((const void*)&inet_ntop) &&
               InProgram((const void*)&gethostname) && !InProgram((const void*)glibc.inet_pton) &&
               !InProgram((const void*)glibc.gethostname),
           "inet_pton, inet_ntop and gethostname are cordinal's own");
    net_detail::CheckAddressText(glibc);
    net_detail::CheckHostname(glibc);
    net_detail::CheckAddressLookup(glibc);
    net_detail::CheckNameLookup(glibc);
    net_detail::CheckInterfaces(glibc);
    net_detail::CheckHostsFile(dir, glibc);
    net_detail::CheckDns(dir, glibc);
}

}  // namespace libc_checks
