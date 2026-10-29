// Linux lifecycle and UI-thread bridge for the same optional server as the Windows player, and the
// away-from-home switch (web_remote.h) with its consent, status and the pairing link a QR code carries.
#include "app.h"
#include "web_library.h"
#include "web_remote.h"
#include "text.h"

#include <ctime>

namespace lx {
namespace {
web::Server server;
web::Remote remote{server};
web::LibraryBuilder builder;
bool running = false;
// The pairing link of the last Pair: spent once a browser pairs with it, gone after ten minutes.
std::string link;
uint64_t linkUntil = 0;
size_t pairedBefore = 0;

constexpr char kAwayQuestion[] =
    "cordinal will ask your router to forward ports 80 and 443 to this computer (UPnP), or tell you which "
    "ports to forward yourself. When the router doesn't say what this network's internet address is, "
    "cordinal asks a STUN server (Cloudflare's, or Google's) for it.\n\n"
    "Phones then connect over HTTPS: cordinal gets a free certificate for that address from Let's Encrypt and "
    "renews it every few days while it runs. By choosing OK you agree to the Let's Encrypt Subscriber "
    "Agreement (letsencrypt.org/repository).\n\n"
    "Away from home, a phone pairs only by scanning a code from the Web server window.";

void AcceptListens() {
    for (const auto& e : server.TakeListens()) {
        TrackId id = g_lib.Find(e.path);
        if (g_lib.Contains(id)) RecordListening(id, e.seconds, e.manual, (uint32_t)std::min<uint64_t>(e.time, UINT32_MAX));
    }
}
void StartRemote() {
    web::RemoteConfig config;
    config.dataDir = g.configDir;
    config.keyName = g.configDir + L"/web-key.bin";
    config.httpsPort = g.settings.webAwayPort;
    config.notify = [] { PostMessageW(nullptr, WM_APP_WEB, 0, 0); };
    remote.Start(config);
}
}  // namespace

void InitWeb() {
    server.SetFixedCode(g.settings.webCode);
    server.LoadDevices(g.configDir + L"/web-devices.bin");
    if (g.settings.webStart) StartWeb();
}
void StartWeb() {
    web::Config config;
    config.port = g.settings.webPort; config.devicesPath = g.configDir + L"/web-devices.bin";
    config.offlinePath = g.configDir + L"/web-offline.bin";
    size_t fontSize = 0;
    const unsigned char* font = DeltaruneFont(&fontSize);
    config.font = {(const char*)font, fontSize};
    config.icon = AppIcon(256);  // the home-screen icon
    config.fixedCode = g.settings.webCode;
    const std::string& names = g.settings.webHosts;
    for (size_t at = 0; at < names.size();) {
        size_t end = names.find_first_of(", ", at);
        if (end == names.npos) end = names.size();
        if (end > at) config.names.push_back(names.substr(at, end - at));
        at = end + 1;
    }
    config.notify = [] { PostMessageW(nullptr, WM_APP_WEB, 0, 0); };
    running = server.Start(config);
    builder.Reset();
    if (!running) ShowMessage(compat::Widen(server.GetStatus().error.c_str()));
    else if (WebAway()) StartRemote();
    g.redraw = true;
}
void StopWeb() {
    remote.Stop(); server.Stop(); running = false; link.clear(); AcceptListens(); builder.Reset(); g.redraw = true;
}
bool WebRunning() { return running; }
web::Status WebStatus() { return server.GetStatus(); }
void PairWeb() {
    if (!server.Pair()) { ShowMessage(L"Couldn't create a pairing code. Start the server first."); return; }
    link = server.PairingLink();
    linkUntil = SDL_GetTicks() + 590000;  // a little inside the server's ten minutes
    pairedBefore = server.GetStatus().devices.size();
}
void RevokeWeb(const std::string& id) { server.Revoke(id); AcceptListens(); }
void TickWeb() {
    AcceptListens();
    if (!running) return;
    server.SetTheme(g.settings.theme, CurrentPalette().dark);
    if (auto snapshot = builder.Tick(g_lib, g.playlists)) server.Publish(std::move(snapshot));
}

std::wstring SetWebFixedCode(const std::string& typed) {
    std::string code;
    if (const char* problem = web::CheckFixedCode(typed, code)) return compat::Widen(problem);
    if (code == g.settings.webCode) return {};
    g.settings.webCode = code;
    SaveSettings();
    server.SetFixedCode(code);
    return code.empty() ? L"Each pairing now gets a new code." : L"The fixed code is saved.";
}

bool WebAway() { return g.settings.webAway && g.settings.webAwayAgreed; }
void SetWebAway(bool on) {
    // Turning it on opens ports, asks outside services and accepts a CA's terms: ask once.
    if (on && !g.settings.webAwayAgreed) {
        if (!Confirm(kAwayQuestion, "Listen away from home?")) return;
        g.settings.webAwayAgreed = true;
    }
    g.settings.webAway = on;
    SaveSettings();
    if (!on) remote.Stop();
    else if (!running) { StartWeb(); if (running) PairWeb(); }
    else if (!remote.Running()) StartRemote();
    g.redraw = true;
}
void RetryWebAway() {
    if (!remote.Running()) return;
    remote.Retry();
    ShowMessage(L"Checking again.");
}
std::string WebAwayText() {
    if (!WebAway()) return "Off. Turn this on to listen on a phone anywhere, over HTTPS.";
    if (!running) return "On. Start the server to listen away from home.";
    auto r = remote.GetStatus();
    std::string text = r.message;
    if (r.expires) {
        time_t when = (time_t)r.expires;
        tm local{};
        char date[64] = "";
        if (localtime_r(&when, &local)) strftime(date, sizeof(date), "%b %d, %H:%M", &local);
        text += std::string(" Certificate valid until ") + date + ".";
    }
    auto s = server.GetStatus();
    if (!s.portError.empty()) text += " " + s.portError;
    if (r.state == web::RemoteStatus::State::Ready && !g.settings.webStart) text += " Turn on Start with cordinal to keep it available.";
    return text;
}
WebPairing CurrentWebPairing() {
    auto s = server.GetStatus();
    if (!link.empty() && (!s.running || SDL_GetTicks() >= linkUntil)) link.clear();
    if (!link.empty() && s.devices.size() > pairedBefore) {
        ShowMessage(L"Paired " + compat::Widen(s.devices.back().name.c_str()) + L".");
        link.clear();
    }
    WebPairing p;
    if (link.empty()) {
        p.caption = "Choose Pair device to show a code a phone can scan.";
        return p;
    }
    p.link = (s.away.empty() ? web::PhoneAddress(s) : s.away) + "#pair=" + link;
    auto r = remote.GetStatus();
    p.caption = s.away.empty() ? "Scan with the phone's camera while it is on this network. Works once, for ten minutes."
        : r.loopbackKnown && !r.loopback ? "Turn off the phone's Wi-Fi, then scan with its camera to listen anywhere. Works once, for ten minutes."
        : "Scan with the phone's camera to listen at home and away. Works once, for ten minutes.";
    return p;
}
}  // namespace lx
