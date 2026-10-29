// Reaching home from outside (web_remote.h): WinHTTP requests on Windows (Linux has its own in
// linux/tls_openssl.cpp), the router's UPnP port mappings, the internet address from the router or
// STUN, and this computer's route to the internet.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/random.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include "web_remote.h"
#include "web_internal.h"
#include "library.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace web {
namespace {
#ifndef _WIN32
using SOCKET = int;
constexpr SOCKET INVALID_SOCKET = -1;
void closesocket(SOCKET s) { close(s); }
#endif
std::string Trim(std::string_view text) {
    while (!text.empty() && (unsigned char)text.front() <= ' ') text.remove_prefix(1);
    while (!text.empty() && (unsigned char)text.back() <= ' ') text.remove_suffix(1);
    return std::string(text);
}
// The text of the first <name> element, with or without a namespace prefix.
std::string Tag(std::string_view xml, std::string_view name) {
    for (size_t at = 0; (at = xml.find(name, at)) != xml.npos; at += name.size()) {
        if (!at || (xml[at - 1] != '<' && xml[at - 1] != ':') || xml.substr(at + name.size(), 1) != ">") continue;
        size_t start = at + name.size() + 1, end = xml.find("</", start);
        return end == xml.npos ? std::string{} : Trim(xml.substr(start, end - start));
    }
    return {};
}
HttpReply Soap(void* session, const Gateway& g, const std::string& action, const std::string& arguments) {
    std::string body = "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" + action + " xmlns:u=\"" + g.service +
        "\">" + arguments + "</u:" + action + "></s:Body></s:Envelope>";
    return HttpFetch(session, g.control, "POST", body, "Content-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"" +
        g.service + '#' + action + "\"\r\n", 4000);
}
bool Wait(SOCKET s, uint64_t until) {
    uint64_t now = ClockMs();
    if (now >= until) return false;
    fd_set fds; FD_ZERO(&fds); FD_SET(s, &fds);
    timeval time{(long)((until - now) / 1000), (long)((until - now) % 1000) * 1000};
    return select((int)s + 1, &fds, nullptr, nullptr, &time) > 0;
}
}  // namespace

#ifdef _WIN32
HttpReply HttpFetch(void* session, const std::string& url, const char* method, const std::string& body,
                    const std::string& headers, unsigned timeoutMs) {
    HttpReply reply;
    std::wstring wide = FromUtf8(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts) || !parts.lpszHostName) return reply;
    std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring target = std::wstring(parts.lpszUrlPath ? parts.lpszUrlPath : L"", parts.dwUrlPathLength) +
        std::wstring(parts.lpszExtraInfo ? parts.lpszExtraInfo : L"", parts.dwExtraInfoLength);
    if (target.empty()) target = L"/";
    HINTERNET own = nullptr, s = (HINTERNET)session;
    if (!s) s = own = WinHttpOpen(L"cordinal", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connection = s ? WinHttpConnect(s, host.c_str(), parts.nPort, 0) : nullptr;
    HINTERNET request = connection ? WinHttpOpenRequest(connection, FromUtf8(method).c_str(), target.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    if (request) {
        DWORD disable = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_REDIRECTS;
        WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable));
        WinHttpSetTimeouts(request, (int)timeoutMs, (int)timeoutMs, (int)timeoutMs, (int)timeoutMs);
        std::wstring extra = FromUtf8(headers);
        if (WinHttpSendRequest(request, extra.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : extra.c_str(), (DWORD)-1L,
                               body.empty() ? WINHTTP_NO_REQUEST_DATA : (void*)body.data(), (DWORD)body.size(),
                               (DWORD)body.size(), 0) &&
            WinHttpReceiveResponse(request, nullptr)) {
            DWORD status = 0, size = sizeof(status);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &size, nullptr);
            size = 0;
            WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, nullptr, nullptr, &size, nullptr);
            std::wstring raw(size / sizeof(wchar_t), L'\0');
            if (size && WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, nullptr, raw.data(), &size, nullptr)) {
                std::string text = ToUtf8(raw);
                for (size_t at = text.find("\r\n"); at != text.npos && at + 2 < text.size();) {
                    size_t end = text.find("\r\n", at + 2), colon = text.find(':', at + 2);
                    if (colon < end) reply.headers[Lower(text.substr(at + 2, colon - at - 2))] = Trim(text.substr(colon + 1, end - colon - 1));
                    at = end;
                }
            }
            bool complete = true;
            for (;;) {
                char bytes[16384];
                DWORD read = 0;
                if (!WinHttpReadData(request, bytes, sizeof(bytes), &read)) { complete = false; break; }
                if (!read) break;
                reply.body.append(bytes, read);
                if (reply.body.size() > (1u << 20)) { complete = false; break; }
            }
            if (complete) reply.status = status;
        }
        WinHttpCloseHandle(request);
    }
    if (connection) WinHttpCloseHandle(connection);
    if (own) WinHttpCloseHandle(own);
    return reply;
}
#endif

std::string SsdpLocation(std::string_view reply) {
    for (size_t at = 0; at < reply.size();) {
        size_t end = reply.find("\r\n", at);
        if (end == reply.npos) end = reply.size();
        auto line = reply.substr(at, end - at);
        if (line.size() > 9 && Lower(std::string(line.substr(0, 9))) == "location:") return Trim(line.substr(9));
        at = end + 2;
    }
    return {};
}
std::vector<std::string> DiscoverGateways(const std::string& local, unsigned ms) {
    std::vector<std::string> found;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return found;
    sockaddr_in from{};
    from.sin_family = AF_INET;
    if (inet_pton(AF_INET, local.c_str(), &from.sin_addr) == 1 && !bind(s, (sockaddr*)&from, sizeof(from))) {
        // Ask on the interface that leads to the internet, and keep the question on the local network.
        setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&from.sin_addr, sizeof(from.sin_addr));
        int ttl = 2;
        setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof(ttl));
        sockaddr_in group{};
        group.sin_family = AF_INET; group.sin_port = htons(1900);
        inet_pton(AF_INET, "239.255.255.250", &group.sin_addr);
        for (const char* target : {"urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                                   "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
                                   "urn:schemas-upnp-org:service:WANIPConnection:1"}) {
            std::string search = std::string("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n"
                "MAN: \"ssdp:discover\"\r\nMX: 1\r\nST: ") + target + "\r\n\r\n";
            sendto(s, search.data(), (int)search.size(), 0, (sockaddr*)&group, sizeof(group));
        }
        for (uint64_t until = ClockMs() + ms; Wait(s, until);) {
            char reply[2048];
            int n = recv(s, reply, sizeof(reply) - 1, 0);
            if (n <= 0) continue;
            std::string location = SsdpLocation({reply, (size_t)n});
            if (location.starts_with("http://") && std::find(found.begin(), found.end(), location) == found.end())
                found.push_back(location);
        }
    }
    closesocket(s);
    return found;
}
bool ReadGateway(void* session, const std::string& location, Gateway& gateway) {
    auto reply = HttpFetch(session, location, "GET", {}, {}, 4000);
    if (reply.status != 200) return false;
    const std::string& xml = reply.body;
    std::string base = Tag(xml, "URLBase");
    if (base.empty()) base = location.substr(0, location.find('/', location.find("//") + 2));
    while (base.ends_with('/')) base.pop_back();
    for (size_t at = 0; (at = xml.find("<serviceType>", at)) != xml.npos;) {
        size_t end = xml.find("</serviceType>", at), close = xml.find("</service>", at);
        if (end == xml.npos) break;
        std::string type = Trim(std::string_view(xml).substr(at + 13, end - at - 13));
        size_t control = xml.find("<controlURL>", end);
        if ((type.find(":WANIPConnection:") != type.npos || type.find(":WANPPPConnection:") != type.npos) &&
            control != xml.npos && control < close) {
            std::string path = Trim(std::string_view(xml).substr(control + 12, xml.find("</controlURL>", control) - control - 12));
            gateway.service = type;
            gateway.control = path.starts_with("http") ? path : base + (path.starts_with('/') ? "" : "/") + path;
            return true;
        }
        at = end;
    }
    return false;
}
std::string GatewayAddress(void* session, const Gateway& gateway) {
    auto reply = Soap(session, gateway, "GetExternalIPAddress", {});
    return reply.status == 200 ? Tag(reply.body, "NewExternalIPAddress") : std::string{};
}
int MapPort(void* session, const Gateway& gateway, uint16_t external, uint16_t internal, unsigned lease) {
    auto add = [&](unsigned seconds) {
        auto reply = Soap(session, gateway, "AddPortMapping", "<NewRemoteHost></NewRemoteHost><NewExternalPort>" +
            std::to_string(external) + "</NewExternalPort><NewProtocol>TCP</NewProtocol><NewInternalPort>" +
            std::to_string(internal) + "</NewInternalPort><NewInternalClient>" + gateway.local +
            "</NewInternalClient><NewEnabled>1</NewEnabled><NewPortMappingDescription>cordinal</NewPortMappingDescription>"
            "<NewLeaseDuration>" + std::to_string(seconds) + "</NewLeaseDuration>");
        if (reply.status == 200) return 0;
        int code = atoi(Tag(reply.body, "errorCode").c_str());
        return code ? code : -1;
    };
    int code = add(lease);
    if (code == 725 && lease) code = add(0);  // OnlyPermanentLeasesSupported
    return code;
}
bool UnmapPort(void* session, const Gateway& gateway, uint16_t external) {
    return Soap(session, gateway, "DeletePortMapping", "<NewRemoteHost></NewRemoteHost><NewExternalPort>" +
        std::to_string(external) + "</NewExternalPort><NewProtocol>TCP</NewProtocol>").status == 200;
}

bool ParseStun(std::string_view reply, const uint8_t transaction[12], std::string& address) {
    static const uint8_t cookie[4] = {0x21, 0x12, 0xA4, 0x42};
    auto u16 = [&](size_t i) { return (unsigned)((uint8_t)reply[i] << 8 | (uint8_t)reply[i + 1]); };
    if (reply.size() < 20 || u16(0) != 0x0101 || memcmp(reply.data() + 4, cookie, 4) ||
        memcmp(reply.data() + 8, transaction, 12)) return false;
    size_t end = std::min(reply.size(), 20 + (size_t)u16(2));
    address.clear();
    for (size_t at = 20; at + 4 <= end;) {
        unsigned type = u16(at), length = u16(at + 2);
        size_t value = at + 4;
        if (value + length > end) break;
        // XOR-MAPPED-ADDRESS (0x0020) is what current servers send; MAPPED-ADDRESS (0x0001) is the old form.
        if ((type == 0x0020 || type == 0x0001) && length >= 8 && reply[value + 1] == 1) {
            uint8_t a[4];
            memcpy(a, reply.data() + value + 4, 4);
            if (type == 0x0020) for (int i = 0; i < 4; ++i) a[i] ^= cookie[i];
            char text[16];
            snprintf(text, sizeof(text), "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
            address = text;
            if (type == 0x0020) return true;
        }
        at = value + ((length + 3) & ~3u);
    }
    return !address.empty();
}
std::string StunAddress(const std::string& host, uint16_t port, unsigned ms) {
    addrinfo hints{};
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM; hints.ai_protocol = IPPROTO_UDP;
    addrinfo* list = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &list) || !list) return {};
    std::string address;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    uint8_t request[20] = {0, 1, 0, 0, 0x21, 0x12, 0xA4, 0x42};
#ifdef _WIN32
    bool random = BCRYPT_SUCCESS(BCryptGenRandom(nullptr, request + 8, 12, BCRYPT_USE_SYSTEM_PREFERRED_RNG));
#else
    bool random = getrandom(request + 8, 12, 0) == 12;
#endif
    if (s != INVALID_SOCKET && random) {
        for (int attempt = 0; attempt < 2 && address.empty(); ++attempt) {
            sendto(s, (const char*)request, sizeof(request), 0, list->ai_addr, (int)list->ai_addrlen);
            for (uint64_t until = ClockMs() + ms / 2; address.empty() && Wait(s, until);) {
                char reply[548];
                int n = recv(s, reply, sizeof(reply), 0);
                if (n > 0) ParseStun({reply, (size_t)n}, request + 8, address);
            }
        }
    }
    if (s != INVALID_SOCKET) closesocket(s);
    freeaddrinfo(list);
    return address;
}
bool DefaultRoute(std::string& local, std::string& gateway) {
    local.clear(); gateway.clear();
    // Connecting a UDP socket only picks a route and a source address; nothing is sent.
    sockaddr_in outside{};
    outside.sin_family = AF_INET; outside.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &outside.sin_addr);
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in self{};
#ifdef _WIN32
    int size = sizeof(self);
#else
    socklen_t size = sizeof(self);
#endif
    char text[INET_ADDRSTRLEN]{};
    if (s != INVALID_SOCKET && !connect(s, (sockaddr*)&outside, sizeof(outside)) && !getsockname(s, (sockaddr*)&self, &size) &&
        inet_ntop(AF_INET, &self.sin_addr, text, sizeof(text))) local = text;
    if (s != INVALID_SOCKET) closesocket(s);
    in_addr hop{};
#ifdef _WIN32
    MIB_IPFORWARDROW row{};
    if (GetBestRoute(outside.sin_addr.s_addr, 0, &row) == NO_ERROR) hop.s_addr = row.dwForwardNextHop;
#else
    // The kernel's IPv4 routes: the default one (destination 0) names the gateway, in network byte order.
    if (FILE* routes = fopen("/proc/net/route", "r")) {
        char line[256], device[64];
        unsigned destination = 0, next = 0, flags = 0;
        while (!hop.s_addr && fgets(line, sizeof(line), routes))
            if (sscanf(line, "%63s %x %x %x", device, &destination, &next, &flags) == 4 && !destination && (flags & 2)) hop.s_addr = next;
        fclose(routes);
    }
#endif
    if (hop.s_addr && inet_ntop(AF_INET, &hop, text, sizeof(text))) gateway = text;
    return !local.empty();
}
bool PublicIPv4(const std::string& address) {
    in_addr a{};
    if (inet_pton(AF_INET, address.c_str(), &a) != 1) return false;
    const uint8_t* v = (const uint8_t*)&a;
    return !(v[0] == 0 || v[0] == 10 || v[0] == 127 || (v[0] == 100 && (v[1] & 0xC0) == 64) ||
             (v[0] == 169 && v[1] == 254) || (v[0] == 172 && (v[1] & 0xF0) == 16) ||
             (v[0] == 192 && v[1] == 0 && v[2] == 0) || (v[0] == 192 && v[1] == 168) ||
             (v[0] == 198 && (v[1] & 0xFE) == 18) || v[0] >= 224);
}
}  // namespace web
