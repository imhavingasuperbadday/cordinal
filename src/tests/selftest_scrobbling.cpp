// Offline protocol and worker checks for the native scrobbling services. The injected transport
// returns service-shaped replies, so no account credentials or internet connection are needed.
#include "selftest_common.h"
#include "web_remote.h"
#include <mutex>

namespace {
struct Server {
    std::mutex mutex;
    std::string lastBody[3], lastUrl[3];
    unsigned sent[3]{}, now[3]{};
    std::atomic<unsigned> error{0};
    std::vector<std::pair<WPARAM, std::wstring>> replies;  // message thread only
} server;

web::HttpReply Request(listenbrainz::Service service, const std::string& url, const char*,
                       const std::string& body, const std::string&) {
    unsigned i = (unsigned)service;
    std::lock_guard lock(server.mutex);
    server.lastBody[i] = body; server.lastUrl[i] = url;
    web::HttpReply reply; reply.status = 200;
    if (url.ends_with("validate-token")) reply.body = R"({"valid":true,"user_name":"listener"})";
    else if (body.find("auth.getToken") != body.npos) reply.body = R"({"token":"browser-token"})";
    else if (body.find("auth.getSession") != body.npos)
        reply.body = R"({"session":{"key":"session-secret","name":"listener"}})";
    else {
        bool now = body.find("playing_now") != body.npos || body.find("track.updateNowPlaying") != body.npos;
        if (now) ++server.now[i]; else ++server.sent[i];
        if (server.error) reply.body = "{\"error\":" + std::to_string(server.error.load()) + "}";
        else reply.body = i == 0 ? R"({"status":"ok"})" : now ? R"({"nowplaying":{}})" :
            R"({"scrobbles":{"@attr":{"accepted":"1","ignored":"0"}}})";
    }
    return reply;
}
LRESULT CALLBACK Replies(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_APP + 1) {
        std::unique_ptr<std::wstring> value(reinterpret_cast<std::wstring*>(lp));
        if ((wp & 15) != listenbrainz::kStatusChanged)
            server.replies.emplace_back(wp, value ? *value : L"");
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
}

void TestScrobbling(const std::wstring& scratch) {
    using namespace listenbrainz;
    testlog::Batch batch;
    batch.Next(L"scrobbling requests sign sorted raw parameters, escape UTF-8 metadata in the browser URL, map "
               L"HTTP-200 API errors to reauthentication, backoff or rejection, and keep the queue on an incomplete reply");
    std::map<std::string, std::string> parameters{{"api_key", "key"}, {"method", "auth.getSession"}, {"token", "token"}};
    Check(SignedForm(parameters, "secret") ==
        "api_key=key&method=auth.getSession&token=token&api_sig=9ac306496295a8866c4a8673395540eb&format=json",
        L"SCROBBLING API signatures sort raw parameters before hashing and form encoding");
    parameters["artist"] = "Björk & A+B";
    Check(SignedForm(parameters, "secret").find("artist=Bj%C3%B6rk%20%26%20A%2BB") != std::string::npos &&
        AuthorizationUrl(Service::LibreFm, L"a&b", L"x+y") == L"https://libre.fm/api/auth/?api_key=a%26b&token=x%2By",
        L"SCROBBLING metadata and browser authorization escape UTF-8 and reserved characters");
    testlog::Cases errors;
    for (auto [code, status] : {std::pair{9, 401}, {10, 401}, {13, 401}, {11, 503}, {16, 503}, {29, 429}, {6, 400}})
        errors.Add(SubmissionStatus(200, "{\"error\":" + std::to_string(code) + "}") == (unsigned)status, std::to_wstring(code));
    Check(errors.Passed(), L"SCROBBLING HTTP 200 API errors request reauthentication, backoff or rejection%ls", errors.failed.c_str());
    Check(SubmissionStatus(200, "<html>unavailable</html>") == 503 && SubmissionStatus(200, "{}") == 503,
        L"SCROBBLING incomplete success replies keep the queue for retry");

    batch.End();
    WNDCLASSW cls{}; cls.lpfnWndProc = Replies; cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"CordinalScrobblingTest"; RegisterClassW(&cls);
    HWND window = CreateWindowW(cls.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, cls.hInstance, nullptr);
    if (!Ready(window != nullptr, L"SCROBBLING message-only result window")) return;
    batch.Next(L"both services authorize in the browser and protect their saved sessions, and a cancelled "
               L"authorization cannot accept a late reply");
    std::wstring dir = scratch + L"\\scrobbling"; CreateDirectoryW(dir.c_str(), nullptr);
    server.replies.clear(); server.error = 0;
    for (unsigned i = 0; i < 3; ++i) { server.sent[i] = server.now[i] = 0; server.lastBody[i].clear(); }
    SetHttpForTest(Request); SetSendingDisabledForTest(true); SetPaused(false); SetSendNowPlaying(true);
    Start(dir, window, WM_APP + 1);
    testlog::Cases accounts;
    for (Service service : {Service::LastFm, Service::LibreFm}) {
        size_t previous = server.replies.size();
        Authorize(service, L"key", L"secret");
        Pump(3000, [&] { return server.replies.size() > previous; });
        bool token = server.replies.size() > previous && (server.replies.back().first & 15) == 4 &&
            server.replies.back().second == L"browser-token";
        previous = server.replies.size();
        Authorize(service, L"key", L"secret", L"browser-token");
        Pump(3000, [&] { return server.replies.size() > previous; });
        bool session = server.replies.size() > previous && (server.replies.back().first & 15) == 5 &&
            SetToken(server.replies.back().second, service);
        auto path = dir + (service == Service::LastFm ? L"\\lastfm.token" : L"\\librefm.token");
        auto bytes = ReadTestBytes(path);
        accounts.Add(token && session && Enabled(service) && GetStatus(service).user == L"listener" &&
            !bytes.empty() && bytes.find("session-secret") == bytes.npos, service == Service::LastFm ? L"Last.fm" : L"Libre.fm");
    }
    Check(accounts.Passed(), L"SCROBBLING both services authorize in the browser and protect their saved sessions%ls", accounts.failed.c_str());
    WPARAM oldResult = server.replies.empty() ? 0 : server.replies.back().first;
    CancelAuthorization(Service::LibreFm);
    Check(oldResult && !CurrentResult(oldResult), L"SCROBBLING cancelled authorization cannot accept a late reply");
    batch.Next(L"separate service queues honor the thirty-second minimum, waiting listens cannot be sent "
               L"to another account, and pause stops collecting without losing queued work");
    SetToken(L"brainz-token");
    Listen listen; listen.artist = "Björk & A+B"; listen.title = "Song"; listen.release = "Album";
    listen.releaseArtist = "Album artist"; listen.trackNumber = 7; listen.listenedAt = 1700000000; listen.durationMs = 30000;
    Submit(listen);
    bool shortTrack = GetStatus().queued == 1 && GetStatus(Service::LastFm).queued == 0 && GetStatus(Service::LibreFm).queued == 0;
    listen.durationMs = 30001; Submit(listen);
    Check(shortTrack && GetStatus().queued == 2 && GetStatus(Service::LastFm).queued == 1 && GetStatus(Service::LibreFm).queued == 1,
        L"SCROBBLING separate service queues honor Last.fm and Libre.fm's thirty-second minimum");
    const std::wstring other = L"{\"key\":\"key\",\"secret\":\"secret\",\"session\":\"other-session\",\"user\":\"another-listener\"}";
    Check(!SetToken(other, Service::LastFm) && GetStatus(Service::LastFm).queued == 1 &&
          GetStatus(Service::LastFm).user == L"listener",
          L"SCROBBLING waiting listens cannot be sent to a different account");
    SetPaused(true); Submit(listen); PlayingNow(listen);
    Check(GetStatus().queued == 2 && GetStatus(Service::LastFm).queued == 1,
        L"SCROBBLING pause stops collecting listens without losing queued work");
    batch.Next(L"both services reload their credentials and offline queues, receive signed UTF-8 batches and "
               L"now-playing directly, and retain a listen when a session expires");
    SetPaused(false); SetSendNowPlaying(false); PlayingNow(listen);
    Stop(); Start(dir, window, WM_APP + 1);
    Check(Enabled(Service::LastFm) && Enabled(Service::LibreFm) && GetStatus(Service::LibreFm).queued == 1,
        L"SCROBBLING both new services reload their credentials and offline queues");
    SetSendingDisabledForTest(false);
    Pump(3000, [&] { return GetStatus().queued == 0 && GetStatus(Service::LastFm).queued == 0 && GetStatus(Service::LibreFm).queued == 0; });
    bool submitted = GetStatus(Service::LastFm).lastSent && GetStatus(Service::LibreFm).lastSent;
    {
        std::lock_guard lock(server.mutex);
        submitted = submitted && server.lastUrl[1] == "https://ws.audioscrobbler.com/2.0/" &&
            server.lastUrl[2] == "https://libre.fm/2.0/" && server.lastBody[1].find("artist%5B0%5D=Bj%C3%B6rk%20%26%20A%2BB") != std::string::npos &&
            server.lastBody[2].find("timestamp%5B0%5D=1700000000") != std::string::npos && server.now[1] == 0 && server.now[2] == 0;
    }
    Check(submitted, L"SCROBBLING sends signed UTF-8 batches directly to each service while now-playing is disabled");
    SetSendNowPlaying(true); PlayingNow(listen);
    Pump(3000, [&] {
        std::lock_guard lock(server.mutex); return server.now[0] && server.now[1] && server.now[2];
    });
    {
        std::lock_guard lock(server.mutex);
        Check(server.now[0] == 1 && server.now[1] == 1 && server.now[2] == 1 &&
            server.lastBody[1].find("timestamp") == std::string::npos,
            L"SCROBBLING now-playing reaches every connected service without adding a completed listen");
    }
    server.error = 9; Submit(listen);
    Pump(3000, [&] { return !GetStatus(Service::LastFm).lastError.empty(); });
    Check(GetStatus(Service::LastFm).queued == 1 && !GetStatus(Service::LastFm).lastError.empty(),
        L"SCROBBLING an expired session stops sending and retains the listen for reconnecting");
    for (Service service : {Service::ListenBrainz, Service::LastFm, Service::LibreFm}) SetToken(L"", service);
    Stop(); SetHttpForTest(nullptr); SetSendNowPlaying(true); SetPaused(false); server.error = 0;
    Pump(100, [] { return false; }); DestroyWindow(window);
}
