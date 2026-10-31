// Windows server controls, UI-thread snapshot capture and browser listening commits, and the
// away-from-home switch with its consent, status and the QR code that pairs a phone in one scan.
#include "app.h"
#include "app_web.h"
#include "qr.h"
#include "web_library.h"
#include "web_remote.h"

#include <ctime>

namespace app {
namespace {
web::Server server;
web::Remote remote{server};
web::LibraryBuilder builder;
struct WebWindow {
    HWND hwnd = nullptr, port = nullptr, automatic = nullptr, toggle = nullptr, status = nullptr,
        addresses = nullptr, pair = nullptr, code = nullptr, fixed = nullptr, devices = nullptr, revoke = nullptr,
        away = nullptr, awayStatus = nullptr, qr = nullptr, qrCaption = nullptr;
    std::vector<std::string> deviceIds;
    std::vector<std::wstring> deviceRows;
    std::wstring last, notice;  // notice: the answer to the last action, kept past the status refresh
    std::string link, qrText;   // the pairing link token, and the address the QR code carries
    uint64_t linkUntil = 0;
    size_t pairedBefore = 0;    // devices when the link was made, so its use can be noticed
    qr::Code qrCode;
    const Palette* palette = nullptr;
} window;
uint16_t savedPort = 8787;
std::string fixedCode;  // checked by web::CheckFixedCode before it is kept
bool automatic = false, initialized = false, away = false;

constexpr wchar_t kAwayQuestion[] =
    L"Listen away from home?\n\n"
    L"cordinal will ask your router to forward ports 80 and 443 to this computer (UPnP), or tell you which "
    L"ports to forward yourself. When the router doesn't say what this network's internet address is, "
    L"cordinal asks a STUN server (Cloudflare's, or Google's) for it.\n\n"
    L"Phones then connect over HTTPS: cordinal gets a free certificate for that address from Let's Encrypt and "
    L"renews it every few days while it runs. By choosing OK you agree to the Let's Encrypt Subscriber "
    L"Agreement (letsencrypt.org/repository).\n\n"
    L"Away from home, a phone pairs only by scanning a code from this window.";

void CopyText(const std::wstring& value) {
    if (value.empty()) return;
    SIZE_T bytes = (value.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) return;
    void* text = GlobalLock(memory);
    if (!text) { GlobalFree(memory); return; }
    memcpy(text, value.c_str(), bytes); GlobalUnlock(memory);
    if (!OpenClipboard(window.hwnd)) { GlobalFree(memory); return; }
    if (!EmptyClipboard() || !SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
    CloseClipboard();
}

std::string_view FontBytes() {
    HRSRC resource = FindResourceW(g.inst, MAKEINTRESOURCEW(IDR_FONT_DELTARUNE), RT_RCDATA);
    HGLOBAL data = resource ? LoadResource(g.inst, resource) : nullptr;
    return data ? std::string_view((const char*)LockResource(data), SizeofResource(g.inst, resource)) : std::string_view{};
}
// The app icon's 256-pixel image, which the icon resource already stores as a PNG: the home-screen icon.
std::string_view IconBytes() {
    HRSRC group = FindResourceW(g.inst, MAKEINTRESOURCEW(IDI_APP), RT_GROUP_ICON);
    HGLOBAL data = group ? LoadResource(g.inst, group) : nullptr;
    const BYTE* dir = data ? (const BYTE*)LockResource(data) : nullptr;
    DWORD size = group ? SizeofResource(g.inst, group) : 0;
    for (unsigned i = 0; dir && size >= 6 && i < *(const WORD*)(dir + 4) && 6 + 14 * (i + 1) <= size; ++i) {
        const BYTE* entry = dir + 6 + 14 * i;  // GRPICONDIRENTRY: width 0 means 256, the id is the last WORD
        if (entry[0]) continue;
        HRSRC image = FindResourceW(g.inst, MAKEINTRESOURCEW(*(const WORD*)(entry + 12)), RT_ICON);
        HGLOBAL bytes = image ? LoadResource(g.inst, image) : nullptr;
        std::string_view png(bytes ? (const char*)LockResource(bytes) : "", image ? SizeofResource(g.inst, image) : 0);
        if (png.starts_with("\x89PNG")) return png;
    }
    return {};
}
// The certificate's key lives in the user's key store; one per data folder, so test data never
// reuses the real one.
std::wstring KeyName() {
    uint32_t hash = 2166136261u;
    for (wchar_t c : g.dataDir) hash = (hash ^ (uint32_t)towlower(c)) * 16777619u;
    wchar_t name[32];
    swprintf_s(name, L"cordinal web %08x", hash);
    return name;
}
void AcceptListens() {
    for (const auto& e : server.TakeListens()) {
        TrackId id = g_lib.Find(e.path);
        if (g_lib.Contains(id)) RecordListening(id, e.seconds, e.manual, (uint32_t)std::min<uint64_t>(e.time, UINT32_MAX));
    }
}
std::wstring Ago(uint64_t seconds) {
    if (seconds < 60) return L"just now";
    if (seconds < 3600) return std::to_wstring(seconds / 60) + L" min ago";
    return std::to_wstring(seconds / 3600) + L" h ago";
}
void StartRemote() {
    web::RemoteConfig config;
    config.dataDir = g.dataDir; config.keyName = KeyName();
    int port = IniInt(L"Web", L"AwayPort", 443);
    config.httpsPort = port >= 1 && port <= 65535 && port != 80 ? (uint16_t)port : 443;
    HWND notify = g.hwnd;
    config.notify = [notify] { PostMessageW(notify, WM_APP_WEB, 0, 0); };
    remote.Start(config);
}
// A code to type at home and a link the QR code carries for anywhere; both open pairing.
bool PairNow() {
    if (!server.Pair()) return false;
    window.link = server.PairingLink();
    window.linkUntil = GetTickCount64() + 590000;  // a little inside the server's ten minutes
    window.pairedBefore = server.GetStatus().devices.size();
    return true;
}
void DrawCode(HDC dc, RECT r) {
    // Dark modules on a light field from the palette's own pair, so every theme's code scans.
    const Palette& p = P();
    FillSolid(dc, r, SurfaceColor());
    const qr::Code& code = window.qrCode;
    if (!code.size) return;
    int modules = code.size + 8, unit = std::max(1, (int)std::min(r.right - r.left, r.bottom - r.top) / modules);
    int side = unit * modules, left = r.left + (r.right - r.left - side) / 2, top = r.top + (r.bottom - r.top - side) / 2;
    FillSolid(dc, {left, top, left + side, top + side}, Ref(p.dark ? p.text : p.bg));
    SetDCBrushColor(dc, Ref(p.dark ? p.bg : p.text));
    for (int y = 0; y < code.size; ++y) for (int x = 0; x < code.size; ++x) if (code.Dark(x, y)) {
        RECT m{left + (x + 4) * unit, top + (y + 4) * unit, left + (x + 5) * unit, top + (y + 5) * unit};
        FillRect(dc, &m, (HBRUSH)GetStockObject(DC_BRUSH));
    }
}
LRESULT CALLBACK CodeProcedure(HWND h, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_PAINT) { DoubleBufferedPaint(h, DrawCode); return 0; }
    if (message == WM_ERASEBKGND) return 1;
    return DefWindowProcW(h, message, wp, lp);
}
std::wstring AwayText(const web::Status& s) {
    if (!away) return L"Off. Turn this on to listen on a phone anywhere, over HTTPS.";
    auto r = remote.GetStatus();
    std::wstring text = FromUtf8(r.message);
    if (r.expires) {
        time_t when = (time_t)r.expires;
        tm local{};
        wchar_t date[64] = L"";
        if (!localtime_s(&local, &when)) wcsftime(date, 64, L"%b %d, %H:%M", &local);
        text += L" Certificate valid until " + std::wstring(date) + L".";
    }
    if (!s.portError.empty()) text += L" " + FromUtf8(s.portError);
    if (r.state == web::RemoteStatus::State::Ready && !automatic) text += L" Turn on Start with cordinal to keep it available.";
    return text;
}
void Refresh() {
    if (!window.hwnd) return;
    auto s = server.GetStatus();
    std::wstring message = s.running ? L"Server is on. " + std::to_wstring(s.streams) + L" active streams." : L"Server is off.";
    if (!s.error.empty()) message += L" " + FromUtf8(s.error);
    if (!window.notice.empty()) message += L" " + window.notice;
    SetTextIfChanged(window.status, message);
    SetWindowTextW(window.toggle, s.running ? L"Stop server" : L"Start server");
    EnableWindow(window.port, !s.running); EnableWindow(window.pair, s.running);
    EnableWindow(GetDlgItem(window.hwnd, 109), s.running);
    EnableWindow(GetDlgItem(window.hwnd, 110), s.running);
    EnableWindow(GetDlgItem(window.hwnd, 111), !s.pairingCode.empty());
    std::wstring addresses;
    for (const auto& address : s.addresses) if (s.running) addresses += FromUtf8(address) + L"\r\n";
    if (addresses != window.last) { SetWindowTextW(window.addresses, addresses.c_str()); window.last = addresses; }
    std::wstring code = !s.pairingCode.empty() && !s.pairingSeconds ? L"Code: " + FromUtf8(s.pairingCode) + L"  (fixed)"
        : !s.pairingCode.empty() ? L"Code: " + FromUtf8(s.pairingCode) + L"  (" + std::to_wstring(s.pairingSeconds) + L" seconds)"
        : s.running && s.fixedCode ? L"Locked by wrong codes. Choose Pair device."
        : L"Choose Pair device to show a code.";
    SetTextIfChanged(window.code, code);

    SendMessageW(window.away, BM_SETCHECK, away ? BST_CHECKED : BST_UNCHECKED, 0);
    SetTextIfChanged(window.awayStatus, AwayText(s));
    EnableWindow(GetDlgItem(window.hwnd, 116), away && remote.Running());
    EnableWindow(GetDlgItem(window.hwnd, 117), !s.away.empty());

    // The link is spent once a browser pairs with it, and expires after ten minutes.
    if (!window.link.empty() && (!s.running || GetTickCount64() >= window.linkUntil)) window.link.clear();
    if (!window.link.empty() && s.devices.size() > window.pairedBefore) {
        window.notice = L"Paired " + FromUtf8(s.devices.back().name) + L".";
        window.link.clear();
    }
    std::string text = window.link.empty() ? std::string{} :
        (s.away.empty() ? web::PhoneAddress(s) : s.away) + "#pair=" + window.link;
    if (text != window.qrText || window.palette != &P()) {
        window.qrText = text; window.palette = &P();
        window.qrCode = text.empty() ? qr::Code{} : qr::Encode(text);
        InvalidateRect(window.qr, nullptr, FALSE);
    }
    auto r = remote.GetStatus();
    SetTextIfChanged(window.qrCaption, window.link.empty() ? L"Choose Pair device to show a code a phone can scan."
        : s.away.empty() ? L"Scan with the phone's camera while it is on this network. Works once, for ten minutes."
        : r.loopbackKnown && !r.loopback ? L"Turn off the phone's Wi-Fi, then scan with its camera to listen anywhere. "
          L"Works once, for ten minutes."
        : L"Scan with the phone's camera to listen at home and away. Works once, for ten minutes.");

    std::vector<std::string> ids;
    std::vector<std::wstring> rows;
    for (const auto& d : s.devices) {
        ids.push_back(d.id);
        rows.push_back(FromUtf8(d.name) + (d.seen ? (d.away ? L"  \x00B7 away, " : L"  \x00B7 at home, ") + Ago(d.seen - 1) : L""));
    }
    if (ids != window.deviceIds || rows != window.deviceRows) {
        LRESULT selected = SendMessageW(window.devices, LB_GETCURSEL, 0, 0);
        std::string keep = selected >= 0 && (size_t)selected < window.deviceIds.size() ? window.deviceIds[selected] : "";
        window.deviceIds = std::move(ids); window.deviceRows = std::move(rows);
        SendMessageW(window.devices, LB_RESETCONTENT, 0, 0);
        for (size_t i = 0; i < window.deviceRows.size(); ++i) {
            SendMessageW(window.devices, LB_ADDSTRING, 0, (LPARAM)window.deviceRows[i].c_str());
            if (window.deviceIds[i] == keep) SendMessageW(window.devices, LB_SETCURSEL, i, 0);
        }
    }
}
void Start() {
    web::Config config; config.port = savedPort; config.devicesPath = g.dataDir + L"\\web-devices.bin";
    config.offlinePath = g.dataDir + L"\\web-offline.bin";
    config.fixedCode = fixedCode;
    HWND notify = g.hwnd;
    config.notify = [notify] { PostMessageW(notify, WM_APP_WEB, 0, 0); };
    config.font = FontBytes(); config.icon = IconBytes();
    // Names a reverse proxy on this computer answers to (Tailscale Serve, say), from settings.ini.
    std::string names = ToUtf8(IniRead(L"Web", L"Hosts", L""));
    for (size_t at = 0; at < names.size();) {
        size_t end = names.find_first_of(", ", at);
        if (end == names.npos) end = names.size();
        if (end > at) config.names.push_back(names.substr(at, end - at));
        at = end + 1;
    }
    if (server.Start(config)) {
        builder.Reset(); SetTimer(g.hwnd, kWebTimer, 20, nullptr);
        if (away) StartRemote();
    } else ShowMessage(FromUtf8(server.GetStatus().error));
    WebTick(); Refresh();
}
void SetAway(bool on) {
    away = on;
    IniWrite(L"Web", L"Away", on ? L"1" : L"0");
    if (!on) remote.Stop();
    else if (!server.GetStatus().running) { Start(); if (server.GetStatus().running) PairNow(); }
    else if (!remote.Running()) StartRemote();
}
HWND Control(HWND parent, const wchar_t* type, const wchar_t* label, DWORD style,
             int id, int x, int y, int w, int h) {
    HWND child = CreateWindowExW(0, type, label, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h),
        parent, (HMENU)(INT_PTR)id, g.inst, nullptr);
    SendMessageW(child, WM_SETFONT, (WPARAM)g.font, TRUE); ApplyControlTheme(child, g.dark);
    return child;
}
LRESULT CALLBACK Procedure(HWND h, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
        case WM_CREATE:
            window.hwnd = h;
            Control(h, L"STATIC", L"Play your library in a browser on a phone or another computer, at home or away.", 0, 0, 12, 12, 530, 20);
            Control(h, L"STATIC", L"Port", 0, 0, 12, 42, 40, 22);
            window.port = Control(h, L"EDIT", std::to_wstring(savedPort).c_str(), WS_TABSTOP | ES_NUMBER | WS_BORDER, 101, 54, 39, 70, 25);
            window.toggle = Control(h, L"BUTTON", L"Start server", WS_TABSTOP, 102, 138, 38, 112, 27);
            window.automatic = Control(h, L"BUTTON", L"Start with cordinal", WS_TABSTOP | BS_AUTOCHECKBOX, 103, 270, 40, 230, 24);
            SendMessageW(window.automatic, BM_SETCHECK, automatic ? BST_CHECKED : BST_UNCHECKED, 0);
            window.status = Control(h, L"STATIC", L"", 0, 0, 12, 76, 530, 36);
            window.addresses = Control(h, L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_MULTILINE | ES_READONLY | WS_VSCROLL, 104, 12, 112, 530, 56);
            Control(h, L"BUTTON", L"Open web player", WS_TABSTOP, 109, 12, 176, 150, 27);
            Control(h, L"BUTTON", L"Copy phone address", WS_TABSTOP, 110, 174, 176, 176, 27);
            window.pair = Control(h, L"BUTTON", L"Pair device", WS_TABSTOP, 105, 12, 210, 112, 27);
            window.code = Control(h, L"STATIC", L"", 0, 114, 138, 215, 270, 25);
            Control(h, L"BUTTON", L"Copy code", WS_TABSTOP, 111, 414, 210, 128, 27);
            Control(h, L"STATIC", L"Fixed code", 0, 0, 12, 252, 80, 22);
            window.fixed = Control(h, L"EDIT", FromUtf8(fixedCode).c_str(), WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, 112, 96, 248, 140, 25);
            SendMessageW(window.fixed, EM_LIMITTEXT, 24, 0);
            Control(h, L"BUTTON", L"Use this code", WS_TABSTOP, 113, 248, 246, 128, 27);
            Control(h, L"STATIC", L"Leave it empty for a new code each time. A fixed code keeps pairing open while the server is on; five wrong codes lock it until you choose Pair device.", 0, 0, 12, 280, 530, 37);
            window.away = Control(h, L"BUTTON", L"Listen away from home", WS_TABSTOP | BS_AUTOCHECKBOX, 115, 12, 327, 260, 24);
            Control(h, L"BUTTON", L"Check again", WS_TABSTOP, 116, 282, 325, 112, 27);
            Control(h, L"BUTTON", L"Copy internet address", WS_TABSTOP, 117, 404, 325, 138, 27);
            window.awayStatus = Control(h, L"STATIC", L"", 0, 119, 12, 358, 530, 54);
            Control(h, L"STATIC", L"Paired devices", 0, 0, 12, 420, 200, 20);
            window.devices = Control(h, L"LISTBOX", L"", WS_TABSTOP | WS_BORDER | LBS_NOTIFY | WS_VSCROLL, 106, 12, 445, 390, 91);
            window.revoke = Control(h, L"BUTTON", L"Remove selected", WS_TABSTOP, 107, 414, 445, 128, 27);
            Control(h, L"BUTTON", L"Remove all", WS_TABSTOP, 108, 414, 478, 128, 27);
            Control(h, L"BUTTON", L"Close", WS_TABSTOP, IDCANCEL, 414, 511, 128, 27);
            Control(h, L"STATIC", L"At home, open an address on the phone and enter the code, or scan the code.\nAway from home, phones connect over HTTPS with a free Let's Encrypt certificate.", 0, 0, 12, 548, 530, 37);
            RegisterAppClass(L"CordinalWebCode", CodeProcedure, 0, false);
            window.qr = CreateWindowExW(0, L"CordinalWebCode", L"Pairing code to scan", WS_CHILD | WS_VISIBLE,
                S(560), S(42), S(188), S(188), h, nullptr, g.inst, nullptr);
            window.qrCaption = Control(h, L"STATIC", L"", 0, 118, 560, 236, 188, 74);
            ApplyWindowTheme(h, P()); SetTimer(h, 1, 1000, nullptr); Refresh(); return 0;
        case WM_TIMER: Refresh(); return 0;
        case WM_COMMAND:
            if (HIWORD(wp) != BN_CLICKED) return 0;  // edits report changes here too; only buttons act
            window.notice.clear();
            switch (LOWORD(wp)) {
                case 102:
                    if (server.GetStatus().running) StopWebServer();
                    else {
                        wchar_t port[16]{}; GetWindowTextW(window.port, port, 16);
                        wchar_t* end = nullptr; unsigned long value = wcstoul(port, &end, 10);
                        if (!*port || *end || !value || value > 65535) { window.notice = L"Enter a port from 1 to 65535."; break; }
                        savedPort = (uint16_t)value; IniWrite(L"Web", L"Port", std::to_wstring(savedPort)); Start();
                        if (server.GetStatus().running) PairNow();
                    }
                    break;
                case 103:
                    if (const Tweak* t = FindTweak("library.web_start"))
                        SetTweak(*t, SendMessageW(window.automatic, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    break;
                case 105:
                    if (!PairNow()) window.notice = L"Couldn't create a pairing code. Try again.";
                    break;
                case 113: {
                    wchar_t text[32]{}; GetWindowTextW(window.fixed, text, 32);
                    std::string code;
                    if (const char* problem = web::CheckFixedCode(ToUtf8(text), code)) { window.notice = FromUtf8(problem); break; }
                    SetWindowTextW(window.fixed, FromUtf8(code).c_str());
                    if (code == fixedCode) break;
                    fixedCode = code; IniWrite(L"Web", L"Code", FromUtf8(code)); server.SetFixedCode(code);
                    window.notice = code.empty() ? L"Each pairing now gets a new code." : L"The fixed code is saved.";
                    break;
                }
                case 107: {
                    LRESULT row = SendMessageW(window.devices, LB_GETCURSEL, 0, 0);
                    if (row >= 0 && (size_t)row < window.deviceIds.size()) server.Revoke(window.deviceIds[row]);
                    AcceptListens(); break;
                }
                case 108: server.Revoke(""); AcceptListens(); break;
                case 109: {
                    std::wstring url = L"http://127.0.0.1:" + std::to_wstring(server.GetStatus().port) + L"/";
                    ShellExecuteW(h, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
                }
                case 110: CopyText(FromUtf8(web::PhoneAddress(server.GetStatus()))); break;
                case 111: CopyText(FromUtf8(server.GetStatus().pairingCode)); break;
                case 115: {
                    bool on = SendMessageW(window.away, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    // Turning it on opens ports, asks outside services and accepts a CA's terms: ask once.
                    if (on && IniInt(L"Web", L"AwayAgreed", 0) != 1) {
                        if (MessageBoxW(h, kAwayQuestion, L"Listen away from home", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) {
                            SendMessageW(window.away, BM_SETCHECK, BST_UNCHECKED, 0); break;
                        }
                        IniWrite(L"Web", L"AwayAgreed", L"1");
                    }
                    SetAway(on);
                    break;
                }
                case 116: remote.Retry(); window.notice = L"Checking again."; break;
                case 117: CopyText(FromUtf8(server.GetStatus().away)); break;
                case IDCANCEL: DestroyWindow(h); return 0;
                default: return 0;
            }
            Refresh(); return 0;
        case WM_ERASEBKGND: {
            RECT rect; GetClientRect(h, &rect); FillRect((HDC)wp, &rect, g.surfaceBrush); return 1;
        }
        case WM_CTLCOLORSTATIC: return SurfaceControlColor((HDC)wp, TextColor());
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLOREDIT: return FieldControlColor((HDC)wp);
        case WM_CLOSE: DestroyWindow(h); return 0;
        case WM_NCDESTROY: {
            // The link and its code outlive the window only as long as the server keeps them.
            std::string link = window.link; uint64_t until = window.linkUntil; size_t before = window.pairedBefore;
            window = {};
            window.link = link; window.linkUntil = until; window.pairedBefore = before;
            return 0;
        }
    }
    return DefWindowProcW(h, message, wp, lp);
}
}
void InitWebServer() {
    initialized = true;
    int port = IniInt(L"Web", L"Port", 8787);
    savedPort = port >= 1 && port <= 65535 ? (uint16_t)port : 8787;
    // The startup choice is a tweak, already read by LoadPreferences. A stored code that no longer
    // passes the check (edited by hand) is ignored rather than trusted.
    std::string code;
    fixedCode = web::CheckFixedCode(ToUtf8(IniRead(L"Web", L"Code", L"")), code) ? std::string{} : code;
    away = IniInt(L"Web", L"Away", 0) == 1 && IniInt(L"Web", L"AwayAgreed", 0) == 1;
    server.SetFixedCode(fixedCode);
    server.LoadDevices(g.dataDir + L"\\web-devices.bin");
    if (automatic) Start();
}
void StopWebServer() {
    KillTimer(g.hwnd, kWebTimer); remote.Stop(); server.Stop(); AcceptListens(); builder.Reset(); Refresh();
}
void WebTick() {
    AcceptListens();
    if (!server.GetStatus().running) return;
    server.SetTheme(BuiltInTheme(g.themeMode), P().dark);  // the server thread knows only built-in themes
    if (auto snapshot = builder.Tick(g_lib, g.playlists, g.convertShowOriginals)) server.Publish(std::move(snapshot));
    // Capture runs on a 20 ms timer; once the snapshot is ready, checking for changes every second
    // is plenty and leaves the UI thread alone (a browser may stay open all day).
    SetTimer(g.hwnd, kWebTimer, builder.Busy() ? 20 : 1000, nullptr);
}
void InvalidateWebLibrary() { builder.Reset(); }
bool WebDialogMessage(MSG* message) { return window.hwnd && IsDialogMessageW(window.hwnd, message); }
void ShowWebServer() {
    if (!initialized) InitWebServer();
    if (window.hwnd) { SetForegroundWindow(window.hwnd); return; }
    RegisterAppClass(L"CordinalWebServer", Procedure);
    RECT r{0, 0, S(760), S(596)};
    AdjustWindowRectExForDpi(&r, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME, g.dpi);
    RECT owner; GetWindowRect(g.hwnd, &owner);
    HWND h = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, L"CordinalWebServer", L"Web server",
        WS_CAPTION | WS_SYSMENU, owner.left + S(30), owner.top + S(40), r.right - r.left, r.bottom - r.top,
        g.hwnd, nullptr, g.inst, nullptr);
    ShowWindow(h, SW_SHOW); SetFocus(window.toggle);
}
bool WebStartsWithCordinal() { return automatic; }
void SetWebStartsWithCordinal(bool on) { automatic = on; }
void ApplyWebStart() {
    if (window.automatic) SendMessageW(window.automatic, BM_SETCHECK, automatic ? BST_CHECKED : BST_UNCHECKED, 0);
    if (!automatic) return;  // turning it off leaves a running server alone until it is stopped
    if (!initialized) InitWebServer();  // starts it, now that the choice is on
    else if (!server.GetStatus().running) Start();
    Refresh();
}
void SetWebPortForTest(uint16_t port) { savedPort = port; }
void OpenWebPlayer() {
    if (!initialized) InitWebServer();
    if (!server.GetStatus().running) Start();
    if (server.GetStatus().running && (server.GetStatus().pairingCode.empty() || window.link.empty())) PairNow();
    ShowWebServer(); Refresh();
}
}  // namespace app
