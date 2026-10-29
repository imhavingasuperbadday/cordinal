// A loopback station exercises chunked HTTP, ICY metadata and cancellation without the internet.
#include <winsock2.h>
#include <ws2tcpip.h>
#include "selftest_common.h"
#include "radio_stream.h"

namespace {
SOCKET Listener(unsigned short& port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return s;
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (sockaddr*)&address, sizeof(address)) || listen(s, 1)) { closesocket(s); return INVALID_SOCKET; }
    int length = sizeof(address);
    if (getsockname(s, (sockaddr*)&address, &length)) { closesocket(s); return INVALID_SOCKET; }
    port = ntohs(address.sin_port);
    return s;
}
void Serve(SOCKET server, bool stalled) {
    fd_set read; FD_ZERO(&read); FD_SET(server, &read);
    timeval timeout{4, 0};
    SOCKET client = select(0, &read, nullptr, nullptr, &timeout) > 0 ? accept(server, nullptr, nullptr) : INVALID_SOCKET;
    closesocket(server);
    if (client == INVALID_SOCKET) return;
    DWORD wait = 4000; setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char*)&wait, sizeof(wait));
    char request[2048]; recv(client, request, sizeof(request), 0);
    std::string reply = "HTTP/1.1 200 OK\r\nContent-Type: audio/mpeg\r\nIcy-Metaint: 4\r\n"
                        "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
    send(client, reply.data(), (int)reply.size(), 0);
    if (stalled) {
        Sleep(1000);
    } else {
        std::string meta = "StreamTitle='Example - Song';";
        meta.resize(32, '\0');
        std::string body = std::string("abcd") + char(2) + meta + "efgh" + char(0);
        for (size_t at = 0; at < body.size(); at += 3) {
            size_t n = std::min<size_t>(3, body.size() - at);
            std::string chunk = std::to_string(n) + "\r\n" + body.substr(at, n) + "\r\n";
            if (send(client, chunk.data(), (int)chunk.size(), 0) != (int)chunk.size()) break;
        }
        send(client, "0\r\n\r\n", 5, 0);
    }
    closesocket(client);
}
void ServeHls(SOCKET server) {
    for (unsigned step = 0; step < 5; ++step) {
        fd_set ready; FD_ZERO(&ready); FD_SET(server, &ready); timeval timeout{5, 0};
        if (select(0, &ready, nullptr, nullptr, &timeout) <= 0) break;
        SOCKET client = accept(server, nullptr, nullptr); if (client == INVALID_SOCKET) break;
        DWORD wait = 4000; setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char*)&wait, sizeof(wait));
        char buffer[4096]{}; int n = recv(client, buffer, sizeof(buffer) - 1, 0);
        std::string request(buffer, n > 0 ? n : 0), body, type = "application/vnd.apple.mpegurl";
        if (request.starts_with("GET /start ")) {
            const char* redirect = "HTTP/1.1 302 Found\r\nLocation: /live/master.m3u8\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            send(client, redirect, (int)strlen(redirect), 0); closesocket(client); continue;
        }
        if (request.starts_with("GET /live/master.m3u8 "))
            body = "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=128000,CODECS=\"mp4a.40.2\"\naudio/index.m3u8?token=local\n";
        else if (request.starts_with("GET /live/audio/index.m3u8?token=local "))
            body = "#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:7\n#EXT-X-KEY:METHOD=AES-128,URI=\"key\"\n#EXTINF:2,\ntone.mp3\n#EXT-X-ENDLIST\n";
        else if (request.starts_with("GET /live/audio/key ")) {
            for (unsigned i = 0; i < 16; ++i) body += (char)i;
            type = "application/octet-stream";
        } else if (request.starts_with("GET /live/audio/tone.mp3 ")) {
            constexpr uint8_t encrypted[] = {0xf9,0xf9,0xe9,0xa0,0x37,0xa2,0x3c,0x3f,0xb9,0x71,0xd3,0x4e,0x9a,0x06,0x68,0x04,
                0xf5,0x6c,0xb9,0x6e,0xf1,0xfc,0x60,0xbe,0x95,0xc3,0x88,0xe2,0x3d,0x40,0x48,0x53};
            body.assign((const char*)encrypted, sizeof(encrypted)); type = "audio/mpeg";
        }
        std::string reply = "HTTP/1.1 200 OK\r\nContent-Type: " + type + "\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        send(client, reply.data(), (int)reply.size(), 0); closesocket(client);
    }
    closesocket(server);
}
}  // namespace

void TestRadioStream() {
    WSADATA wsa{};
    if (!Ready(WSAStartup(MAKEWORD(2, 2), &wsa) == 0, L"loopback radio sockets")) return;
    unsigned short port = 0;
    SOCKET listener = Listener(port);
    if (!Ready(listener != INVALID_SOCKET, L"loopback radio station")) { WSACleanup(); return; }
    std::thread server(Serve, listener, false);
    auto stream = radio::Stream::Open("http://127.0.0.1:" + std::to_string(port) + "/stream");
    uint8_t bytes[8]{};
    bool received = ReadLiveBytes(*stream, bytes, sizeof(bytes));
    server.join();
    Check(received && !memcmp(bytes, "abcdefgh", sizeof(bytes)) && stream->Title() == "Example - Song",
          L"radio removes ICY metadata from fragmented chunked audio and reports the title");
    stream->Abort();

    listener = Listener(port);
    if (!Ready(listener != INVALID_SOCKET, L"stalled loopback radio station")) { WSACleanup(); return; }
    std::thread stalled(Serve, listener, true);
    auto waiting = radio::Stream::Open("http://127.0.0.1:" + std::to_string(port) + "/stream");
    std::atomic<bool> ended{false};
    std::thread reader([&] { uint8_t c; waiting->Read(&c, 1); ended = true; });
    for (int i = 0; i < 300 && !waiting->Connected(); ++i) Sleep(10);
    bool connected = waiting->Connected();
    waiting->Abort();
    for (int i = 0; i < 50 && !ended; ++i) Sleep(10);
    bool prompt = ended;
    reader.join(); stalled.join();
    Check(connected && prompt, L"stopping a stalled station promptly unblocks the decoder");
    listener = Listener(port);
    if (Ready(listener != INVALID_SOCKET, L"HLS loopback station")) {
        std::thread hlsServer(ServeHls, listener);
        auto hls = radio::Stream::Open("http://127.0.0.1:" + std::to_string(port) + "/start");
        uint8_t audio[8]{};
        bool played = ReadLiveBytes(*hls, audio, sizeof(audio));
        hls->Abort(); hlsServer.join();
        constexpr uint8_t wanted[] = {255,251,144,100,'a','b','c','d'};
        Check(played && !memcmp(audio, wanted, sizeof(wanted)),
              L"HLS follows redirects and relative playlists, decrypts AES-128 with its sequence IV and strips ID3");
    }
    WSACleanup();
}
