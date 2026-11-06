// Checks of the Discord status (discord.h) without Discord: the activity for each setting, the IPC framing, and
// the worker against a fake Discord endpoint (a named pipe on Windows, a Unix socket on Linux) that answers as
// the app does. The Windows self-test (selftest_scrobbling.cpp, suite "listenbrainz") and the Linux core test
// (core_test.cpp, "discord") both run them.
#pragma once
#include "discord.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace discord_checks {

using Results = std::vector<std::pair<bool, std::string>>;

// Answers the handshake with READY and every command with its reply, and keeps what it was sent.
class FakeDiscord {
public:
    // Windows: `where` is unused and a pipe named for this process is served. Linux: a socket in folder `where`.
    explicit FakeDiscord(const std::string& where) {
#ifdef _WIN32
        endpoint_ = "\\\\.\\pipe\\cordinal-discord-test-" + std::to_string(GetCurrentProcessId()) + "-";
#else
        endpoint_ = where + "/discord-ipc-";
        std::string path = endpoint_ + "0";
        unlink(path.c_str());
        listen_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (path.size() < sizeof(addr.sun_path)) memcpy(addr.sun_path, path.c_str(), path.size() + 1);
        if (bind(listen_, (sockaddr*)&addr, sizeof(addr)) != 0 || ::listen(listen_, 4) != 0) {
            close(listen_);
            listen_ = -1;
        }
#endif
        (void)where;
        thread_ = std::thread([this] { Serve(); });
    }
    ~FakeDiscord() {
        stop_ = true;
#ifdef _WIN32
        // Wakes a waiting ConnectNamedPipe with a connection of our own.
        std::wstring name(endpoint_.begin(), endpoint_.end());
        HANDLE h = CreateFileW((name + L"0").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
#else
        if (listen_ >= 0) shutdown(listen_, SHUT_RDWR);
#endif
        thread_.join();
#ifndef _WIN32
        if (listen_ >= 0) close(listen_);
        unlink((endpoint_ + "0").c_str());
#endif
    }
    const std::string& Endpoint() const { return endpoint_; }
    std::vector<std::pair<uint32_t, std::string>> Received() {
        std::lock_guard lock(mu_);
        return received_;
    }

private:
#ifdef _WIN32
    using Handle = HANDLE;
    static bool ReadAll(Handle h, char* p, size_t n) {
        while (n) {
            DWORD got = 0;
            if (!ReadFile(h, p, (DWORD)n, &got, nullptr) || !got) return false;
            p += got;
            n -= got;
        }
        return true;
    }
    static void WriteAll(Handle h, const std::string& s) {
        DWORD n = 0;
        WriteFile(h, s.data(), (DWORD)s.size(), &n, nullptr);
    }
#else
    using Handle = int;
    static bool ReadAll(Handle h, char* p, size_t n) {
        while (n) {
            ssize_t got = recv(h, p, n, 0);
            if (got <= 0) return false;
            p += got;
            n -= (size_t)got;
        }
        return true;
    }
    static void WriteAll(Handle h, const std::string& s) { (void)!send(h, s.data(), s.size(), MSG_NOSIGNAL); }
#endif

    void Talk(Handle h) {
        for (;;) {
            char header[8];
            uint32_t op = 0, length = 0;
            if (!ReadAll(h, header, 8)) return;
            memcpy(&op, header, 4);
            memcpy(&length, header + 4, 4);
            std::string body(length, '\0');
            if (length > (1u << 20) || (length && !ReadAll(h, body.data(), length))) return;
            {
                std::lock_guard lock(mu_);
                received_.emplace_back(op, body);
            }
            WriteAll(h, discord::Frame(1, op == 0 ? R"({"cmd":"DISPATCH","evt":"READY","data":{"v":1}})"
                                                  : R"({"cmd":"SET_ACTIVITY","evt":null,"data":{}})"));
        }
    }

    void Serve() {
        while (!stop_) {
#ifdef _WIN32
            std::wstring name(endpoint_.begin(), endpoint_.end());
            HANDLE h = CreateNamedPipeW((name + L"0").c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 4096, 4096,
                                        0, nullptr);
            if (h == INVALID_HANDLE_VALUE) return;
            if (ConnectNamedPipe(h, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
                if (!stop_) Talk(h);
                DisconnectNamedPipe(h);
            }
            CloseHandle(h);
#else
            if (listen_ < 0) return;
            int c = accept(listen_, nullptr, nullptr);
            if (c < 0) return;
            Talk(c);
            close(c);
#endif
        }
    }

    std::string endpoint_;
    std::thread thread_;
    std::mutex mu_;
    std::vector<std::pair<uint32_t, std::string>> received_;
    std::atomic<bool> stop_{false};
#ifndef _WIN32
    int listen_ = -1;
#endif
};

inline bool Has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

inline bool WaitForSent(uint32_t count) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (discord::SentForTest() < count && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return discord::SentForTest() == count;
}

// What should not be sent has had time not to be.
inline bool StillSent(uint32_t count) {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    return discord::SentForTest() == count;
}

inline Results All(const std::string& socketFolder) {
    using namespace discord;
    Results r;
    auto check = [&](bool ok, const char* what) { r.emplace_back(ok, what); };

    check(Frame(1, "{}") == std::string("\x01\0\0\0\x02\0\0\0{}", 10), "a message is a little-endian opcode and length, then JSON");

    NowPlaying song;
    song.active = true;
    song.title = "Say \"Hi\"";
    song.artist = "Artist";
    song.album = "Album";
    song.albumArtist = "Various";
    song.position = 30;
    song.duration = 200;
    const int64_t at = 1700000000000;
    std::string json = ActivityJson(song, Show::Song, at, 0);
    check(Has(json, R"("type":2)") && Has(json, R"("details":"Say \"Hi\"")") && Has(json, R"("state":"Artist")") &&
          Has(json, R"("large_text":"Album")"), "Song shows the title by the artist, the album on the cover");
    check(Has(json, R"("start":1699999970000,"end":1700000170000)"), "Song's bar runs from the song's start to its end");
    song.rate = 2;
    check(Has(ActivityJson(song, Show::Song, at, 0), R"("start":1699999985000,"end":1700000085000)"),
          "the bar follows the playback speed");
    song.rate = 1;
    json = ActivityJson(song, Show::Album, at, 1699999000000);
    check(Has(json, R"("details":"Album")") && Has(json, R"("state":"Various")") &&
          Has(json, R"("timestamps":{"start":1699999000000})") && !Has(json, "Say"),
          "Album shows the album by its artist, counting up from the album's start");
    NowPlaying single = song;
    single.album.clear();
    check(Has(ActivityJson(single, Show::Album, at, 0), R"("details":"Say \"Hi\"")"), "a track with no album shows as a song");
    NowPlaying paused = song;
    paused.paused = true;
    check(ActivityJson(paused, Show::Song, at, 0).empty() && ActivityJson(song, Show::Off, at, 0).empty() &&
          ActivityJson(NowPlaying{}, Show::Song, at, 0).empty(), "nothing is shown while paused, stopped or off");
    NowPlaying radio;
    radio.active = radio.radio = true;
    radio.artist = "Station FM";
    json = ActivityJson(radio, Show::Album, at, 0);
    check(Has(json, R"("details":"Station FM")") && !Has(json, "timestamps"), "a station shows its name, with no bar");
    radio.title = "Song on air";
    check(Has(ActivityJson(radio, Show::Song, at, 0), R"("details":"Song on air","state":"Station FM")"),
          "a station's current song shows by the station");
    const std::string cover = "https://e-cdns-images.dzcdn.net/images/cover/abc/1000x1000-000000-80-0-0.jpg";
    json = ActivityJson(song, Show::Song, at, 0, cover);
    check(Has(json, "\"large_image\":\"" + cover + "\"") && Has(json, R"("small_image":"https://)") &&
          Has(json, R"("small_text":"cordinal")"), "a found cover is the picture, with cordinal's icon as its badge");
    json = ActivityJson(song, Show::Song, at, 0, "https://example.com/\"quoted\".jpg");
    check(!Has(json, "example.com") && !Has(json, "small_image") && Has(json, R"("large_image":"https://)"),
          "an address Discord could not take falls back to the icon");
    check(!Has(ActivityJson(song, Show::Song, at, 0, "http://example.com/a.jpg"), "example.com") &&
          !Has(ActivityJson(song, Show::Song, at, 0, "https://example.com/" + std::string(250, 'a')), "example.com"),
          "only https addresses of up to 256 characters are used");
    // Which cover wins: the one chosen in cordinal first, then the album's online cover, then the icon.
    check(PreferredCover(cover, "https://example.com/other.jpg") == cover,
          "the cover chosen in cordinal is the picture Discord shows");
    check(PreferredCover("", "https://example.com/other.jpg") == "https://example.com/other.jpg",
          "without a chosen cover the album's found cover is shown");
    check(PreferredCover("http://example.com/a.jpg", "https://example.com/other.jpg") == "https://example.com/other.jpg",
          "a chosen cover Discord cannot take gives way to the found one");
    check(PreferredCover("", "").empty(), "with neither address cordinal's icon stands in");
    NowPlaying odd = song;
    odd.title = "X";
    odd.artist = std::string(126, 'a') + "\xE2\x82\xAC";  // the euro sign straddles the 128-byte limit
    odd.album = "tab\there";
    json = ActivityJson(odd, Show::Song, at, 0);
    check(Has(json, R"("details":"X\)" "u200b\"") && Has(json, "\"state\":\"" + std::string(126, 'a') + "\"") &&
          Has(json, R"(tab\u0009here)"), "short, long and control-character text stays within Discord's rules");

    // The worker against a fake Discord.
    FakeDiscord fake(socketFolder);
    SetEndpointForTest(fake.Endpoint());
    SetMode(Show::Song);
    Update(song);
    check(WaitForSent(1), "the song is sent once Discord answers");
    auto got = fake.Received();
    check(got.size() >= 2 && got[0].first == 0 && Has(got[0].second, R"("v":1)") && Has(got[0].second, "client_id") &&
          got[1].first == 1 && Has(got[1].second, R"("cmd":"SET_ACTIVITY")") && Has(got[1].second, R"("pid":)") &&
          Has(got[1].second, R"("details":"Say \"Hi\"")"), "a handshake, then SET_ACTIVITY with the song");
    Update(song);
    check(StillSent(1), "the same song is not sent again");
    NowPlaying seeked = song;
    seeked.position = 120;
    Update(seeked);
    check(WaitForSent(2), "a seek moves the bar");
    SetMode(Show::Album);
    check(WaitForSent(3), "changing the setting takes effect at once");
    NowPlaying next = seeked;
    next.title = "Next track";
    next.position = 0;
    Update(next);
    check(StillSent(3), "Album sends nothing when the next track is on the same album");
    NowPlaying other = next;
    other.album = "Another album";
    Update(other);
    check(WaitForSent(4), "Album updates for a new album");
    got = fake.Received();
    check(!got.empty() && Has(got.back().second, R"("details":"Another album")"), "the new album is what Discord shows");
    other.paused = true;
    Update(other);
    check(WaitForSent(5), "pausing clears the status");
    got = fake.Received();
    check(!got.empty() && Has(got.back().second, R"("cmd":"SET_ACTIVITY")") && !Has(got.back().second, "activity\":"),
          "a clear is SET_ACTIVITY without an activity");
    Stop();
    SetEndpointForTest({});
    if (!Available()) {  // this build has no application id: the setting is inert even if a profile turns it on
        Update(song);
        check(StillSent(0), "without an application id nothing is sent, whatever the setting says");
        Stop();
    }
    SetMode(Show::Off);
    return r;
}

}  // namespace discord_checks
