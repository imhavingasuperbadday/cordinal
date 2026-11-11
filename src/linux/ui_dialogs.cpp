// The Linux window's dialogs: library folders, the exclude confirmation, the web server window and About.
#include "ui_internal.h"

namespace lx {

void DrawFoldersDialog(const Palette& p) {
    int pw = 0, ph = 0;
    SDL_GetRenderOutputSize(g.renderer, &pw, &ph);
    Fill({0, 0, (float)pw, (float)ph}, 0x000000, 110);
    float w = std::min((float)pw - S(40), S(560)), rowH = S(30);
    float h = S(144) + std::max<size_t>(1, g.settings.folders.size()) * rowH;
    Rect panel{((float)pw - w) / 2, std::max(S(20), ((float)ph - h) / 2), w, h};
    g_folders.panel = panel;
    Fill(panel, p.surface);
    Frame(panel, p.border);
    float x = panel.x + S(16), y = panel.y + S(14);
    DrawText(Font::Title, "Library folders", x, y, p.text);
    y += S(32);
    g_folders.remove.clear();
    if (g.settings.folders.empty()) {
        DrawText(Font::Normal, "No folders yet. Add the folders your music is in.", x, y + S(6), p.sub);
        y += rowH;
    }
    for (auto& f : g.settings.folders) {
        Rect remove{panel.R() - S(16) - S(80), y + S(3), S(80), rowH - S(6)};
        DrawText(Font::Normal, Utf8(f), x, y + (rowH - LineHeight(Font::Normal)) / 2, p.text, remove.x - x - S(8));
        bool hover = remove.In(g_mouseX, g_mouseY);
        Fill(remove, hover ? p.hover : p.surface2);
        Frame(remove, p.border);
        DrawText(Font::Normal, "Remove", remove.x, remove.y + (remove.h - LineHeight(Font::Normal)) / 2, p.text, remove.w, 1);
        g_folders.remove.push_back(remove);
        y += rowH;
    }
    y += S(8);
    DrawText(Font::Normal, "Folders are watched while cordinal runs.", x, y, p.sub, panel.w - S(32));
    y += S(24);
    g_folders.add = {x, y, S(130), S(30)};
    g_folders.music = {x + S(138), y, S(140), S(30)};
    g_folders.done = {panel.R() - S(16) - S(90), y, S(90), S(30)};
    for (auto [r, label, accent] : {std::tuple{g_folders.add, "Add folder...", true},
                                  std::tuple{g_folders.music, "Add Music folder", false},
                                  std::tuple{g_folders.done, "Done", false}}) {
        bool hover = r.In(g_mouseX, g_mouseY);
        Fill(r, accent ? p.accent : (hover ? p.hover : p.surface2));
        if (!accent) Frame(r, p.border);
        DrawText(Font::Normal, label, r.x, r.y + (r.h - LineHeight(Font::Normal)) / 2, accent ? TextOn(p.accent) : p.text, r.w, 1);
    }
}

void DrawExcludeConfirm(const Palette& p) {
    int pw = 0, ph = 0;
    SDL_GetRenderOutputSize(g.renderer, &pw, &ph);
    Fill({0, 0, (float)pw, (float)ph}, p.bg, 110);
    float w = std::min((float)pw - S(40), S(510)), h = S(172);
    Rect panel{((float)pw - w) / 2, ((float)ph - h) / 2, w, h};
    g_exclude.panel = panel;
    Fill(panel, p.surface);
    Frame(panel, p.border);
    float x = panel.x + S(16), y = panel.y + S(15);
    DrawText(Font::Title, "Remove from library", x, y, p.text);
    y += S(36);
    std::string prompt = "Remove " + std::to_string(g_exclude.ids.size()) +
                         (g_exclude.ids.size() == 1 ? " track" : " tracks") + " from the library?";
    DrawText(Font::Normal, prompt, x, y, p.text, panel.w - S(32));
    y += S(26);
    DrawText(Font::Normal, "The files stay on disk and won't be added back by scans.", x, y, p.sub, panel.w - S(32));
    g_exclude.confirm = {panel.R() - S(206), panel.B() - S(44), S(90), S(28)};
    g_exclude.cancel = {panel.R() - S(106), panel.B() - S(44), S(90), S(28)};
    for (auto [r, label] : {std::pair{g_exclude.confirm, "Remove"}, std::pair{g_exclude.cancel, "Cancel"}}) {
        Fill(r, r.In(g_mouseX, g_mouseY) ? p.hover : p.surface2);
        Frame(r, p.border);
        DrawText(Font::Normal, label, r.x, r.y + (r.h - LineHeight(Font::Normal)) / 2, p.text, r.w, 1);
    }
}

bool FoldersClick(float x, float y) {
    if (!g_folders.panel.In(x, y) || g_folders.done.In(x, y)) {
        g_folders.open = false;
        return true;
    }
    if (g_folders.add.In(x, y)) {
        ChooseFolder();
        return true;
    }
    if (g_folders.music.In(x, y)) {
        AddFolder(compat::Widen(MusicFolder().c_str()));
        return true;
    }
    for (size_t i = 0; i < g_folders.remove.size(); ++i)
        if (g_folders.remove[i].In(x, y)) {
            RemoveFolder(i);
            return true;
        }
    return true;
}

void WebAction(int control) {
    if (control == 1) {
        if (WebRunning()) StopWeb();
        else {
            unsigned long port = strtoul(g_web.port.c_str(), nullptr, 10);
            if (g_web.port.empty() || !port || port > 65535) { ShowMessage(L"Enter a port from 1 to 65535."); return; }
            g.settings.webPort = (uint16_t)port; SaveSettings(); StartWeb();
            if (WebRunning()) PairWeb();
        }
    } else if (control == 2) { g.settings.webStart = !g.settings.webStart; SaveSettings(); }
    else if (control == 3) PairWeb();
    else if (control == 4) RevokeWeb("");
    else if (control == 5) g_web.open = false;
    else if (control == 6 && WebRunning()) {
        auto url = "http://127.0.0.1:" + std::to_string(WebStatus().port) + '/'; SDL_OpenURL(url.c_str());
    } else if (control == 7 && WebRunning()) {
        SDL_SetClipboardText(web::PhoneAddress(WebStatus()).c_str()); ShowMessage(L"Phone address copied.");
    } else if (control == 8) SDL_SetClipboardText(WebStatus().pairingCode.c_str());
    else if (control == 9) SetWebAway(!WebAway());
    else if (control == 10) RetryWebAway();
    else if (control == 11 && !WebStatus().away.empty()) {
        SDL_SetClipboardText(WebStatus().away.c_str()); ShowMessage(L"Internet address copied.");
    } else if (control == 12 || control == 13) {
        std::wstring notice = SetWebFixedCode(g_web.fixed);
        g_web.fixed = g.settings.webCode;  // as kept: spaces and dashes go
        if (!notice.empty()) ShowMessage(notice);
    }
    g.redraw = true;
}

// The pairing link as a QR code: dark modules on a light field from the palette's own pair, so every theme's
// code scans, with the four-module quiet zone around it.
void DrawPairingCode(const Rect& r, const Palette& p) {
    Fill(r, p.surface2);
    const qr::Code& code = g_web.qr;
    if (!code.size) return;
    const int modules = code.size + 8;
    const float unit = std::max(1.0f, std::floor(std::min(r.w, r.h) / modules)), side = unit * modules;
    const float left = std::round(r.x + (r.w - side) / 2), top = std::round(r.y + (r.h - side) / 2);
    Fill({left, top, side, side}, p.dark ? p.text : p.bg);
    for (int y = 0; y < code.size; ++y)
        for (int x = 0; x < code.size; ++x)
            if (code.Dark(x, y)) Fill({left + (x + 4) * unit, top + (y + 4) * unit, unit, unit}, p.dark ? p.bg : p.text);
}

void DrawWeb(const Palette& p) {
    int pw = 0, ph = 0; SDL_GetRenderOutputSize(g.renderer, &pw, &ph);
    Fill({0, 0, (float)pw, (float)ph}, p.bg, 190);
    float w = std::min((float)pw - S(24), S(840)), h = std::min((float)ph - S(24), S(720));
    auto& panel = g_web.panel; panel = {((float)pw - w) / 2, ((float)ph - h) / 2, w, h};
    Fill(panel, p.surface); Frame(panel, p.border);
    // The QR code and how to use it take the right-hand column, as in the Windows window.
    const float column = S(204);
    float x = panel.x + S(16), y = panel.y + S(14), width = w - S(32) - column;
    WebPairing pairing = CurrentWebPairing();
    if (pairing.link != g_web.qrText) {
        g_web.qrText = pairing.link;
        g_web.qr = pairing.link.empty() ? qr::Code{} : qr::Encode(pairing.link);
    }
    {
        Rect code{panel.R() - S(16) - S(188), panel.y + S(56), S(188), S(188)};
        DrawPairingCode(code, p);
        float cy = code.B() + S(8);
        for (const std::string& line : Wrap(Font::Small, pairing.caption, code.w)) {
            DrawText(Font::Small, line, code.x, cy, p.sub, code.w); cy += LineHeight(Font::Small);
        }
    }
    DrawText(Font::Title, "Web server", x, y, p.text); y += S(32);
    auto status = WebStatus();
    std::string message = status.error.empty() ? status.running ? "On: play on a phone using an address below." : "Off: start to share your library." : status.error;
    DrawText(Font::Normal, message, x, y, status.error.empty() ? p.sub : p.error, width); y += S(30);
    g_web.controls[0] = {x + S(35), y, S(70), S(28)};
    DrawText(Font::Normal, "Port", x, y + S(5), p.sub);
    Fill(g_web.controls[0], p.surface2); Frame(g_web.controls[0], g_web.focus == 0 ? p.accent : p.border);
    DrawText(Font::Normal, g_web.port, x + S(41), y + S(5), p.text);
    g_web.controls[1] = {x + S(118), y, S(112), S(28)};
    g_web.controls[2] = {x + S(242), y, width - S(242), S(28)};
    y += S(38);
    g_web.controls[6] = {x, y, S(150), S(28)};
    g_web.controls[7] = {x + S(162), y, S(176), S(28)}; y += S(38);
    DrawText(Font::Small, "Click an address to copy it. Open it on the phone.", x, y, p.sub); y += S(24);
    g_web.addresses.clear();
    size_t shown = 0;
    for (const auto& address : status.addresses) {
        if (!status.running || address.find("127.0.0.1") != address.npos || address.find("localhost") != address.npos ||
            address.find("[::1]") != address.npos) continue;
        if (shown++ >= 3) break;
        Rect r{x, y, width, S(24)}; g_web.addresses.emplace_back(r, address);
        if (r.In(g_mouseX, g_mouseY)) Fill(r, p.hover);
        DrawText(Font::Normal, address, r.x, r.y + S(2), p.text, r.w); y += S(24);
    }
    if (!shown) { DrawText(Font::Normal, status.running ? "No network address was found." : "Server is off.", x, y, p.sub); y += S(24); }
    y += S(8); g_web.controls[3] = {x, y, S(112), S(28)};
    g_web.controls[8] = {x + width - S(90), y, S(90), S(28)};
    std::string code = !status.pairingCode.empty() && !status.pairingSeconds ? "Code: " + status.pairingCode + "  (fixed)"
        : !status.pairingCode.empty() ? "Code: " + status.pairingCode + " (" + std::to_string(status.pairingSeconds) + " s)"
        : status.running && status.fixedCode ? "Locked by wrong codes. Choose Pair device."
        : "Generate a temporary pairing code.";
    DrawText(Font::Normal, code, x + S(126), y + S(5), p.sub, width - S(226)); y += S(38);
    // A fixed code: typed once on each phone at home, open while the server is on.
    DrawText(Font::Normal, "Fixed code", x, y + S(5), p.sub);
    g_web.controls[12] = {x + S(84), y, S(140), S(28)};
    Fill(g_web.controls[12], p.surface2); Frame(g_web.controls[12], g_web.focus == 12 ? p.accent : p.border);
    if (g_web.fixed.empty() && g_web.focus != 12) DrawText(Font::Normal, "none", g_web.controls[12].x + S(6), y + S(5), p.faint);
    float typed = DrawText(Font::Normal, g_web.fixed, g_web.controls[12].x + S(6), y + S(5), p.text, g_web.controls[12].w - S(12));
    if (g_web.focus == 12) Fill({g_web.controls[12].x + S(6) + typed, y + S(6), S(1), LineHeight(Font::Normal)}, p.text);
    g_web.controls[13] = {g_web.controls[12].R() + S(12), y, S(128), S(28)}; y += S(32);
    for (const std::string& line : Wrap(Font::Small, "Leave it empty for a new code each time. A fixed code keeps pairing open while the "
                                                     "server is on; five wrong codes lock it until you choose Pair device.", width)) {
        DrawText(Font::Small, line, x, y, p.sub, width); y += LineHeight(Font::Small);
    }
    y += S(12);
    // Away from home: the switch, Check again and the internet address, then what is happening, in words.
    g_web.controls[9] = {x, y, S(220), S(28)};
    g_web.controls[10] = {x + S(230), y, S(112), S(28)};
    g_web.controls[11] = {x + width - S(176), y, S(176), S(28)}; y += S(34);
    for (const std::string& line : Wrap(Font::Normal, WebAwayText(), width)) {
        DrawText(Font::Normal, line, x, y, p.sub, width); y += LineHeight(Font::Normal);
    }
    y += S(10);
    DrawText(Font::Normal, "Paired devices (Up / Down to scroll)", x, y, p.sub); y += S(24);
    g_web.devices.clear();
    int visible = std::max(1, (int)((panel.B() - S(96) - y) / S(28)));
    g_web.scroll = std::clamp(g_web.scroll, 0, std::max(0, (int)status.devices.size() - visible));
    for (int i = g_web.scroll; i < (int)status.devices.size() && i < g_web.scroll + visible; ++i) {
        const auto& device = status.devices[i];
        DrawText(Font::Normal, device.name + (device.seen ? device.away ? "  \xC2\xB7 away" : "  \xC2\xB7 at home" : ""), x, y + S(5),
                 p.text, width - S(100));
        Rect remove{x + width - S(90), y, S(90), S(26)}; Frame(remove, p.border);
        DrawText(Font::Normal, "Remove", remove.x, remove.y + S(4), p.text, remove.w, 1);
        g_web.devices.emplace_back(remove, device.id); y += S(28);
    }
    if (status.devices.empty()) DrawText(Font::Normal, "No paired devices.", x, y, p.sub);
    y = panel.B() - S(80); g_web.controls[4] = {x, y, S(112), S(28)};
    g_web.controls[5] = {x + width - S(90), y, S(90), S(28)};
    const char* labels[kWebControls] = {"", status.running ? "Stop server" : "Start server",
        g.settings.webStart ? "[x] Start with cordinal" : "[ ] Start with cordinal", "Pair device", "Remove all", "Close",
        "Open web player", "Copy phone address", "Copy code",
        WebAway() ? "[x] Listen away from home" : "[ ] Listen away from home", "Check again", "Copy internet address", "",
        "Use this code"};
    const bool enabled[kWebControls] = {true, true, true, true, true, true, true, true, true, true, WebAway() && status.running,
                                        !status.away.empty(), true, true};
    for (int i = 1; i < kWebControls; ++i) {
        if (i == 12) continue;  // the fixed code's field, drawn above
        auto r = g_web.controls[i];
        Fill(r, enabled[i] && r.In(g_mouseX, g_mouseY) ? p.hover : p.surface2);
        Frame(r, g_web.focus == i ? p.accent : p.border);
        DrawText(Font::Normal, labels[i], r.x, r.y + S(5), enabled[i] ? p.text : p.faint, r.w, 1);
    }
    float fy = panel.B() - S(44);
    for (const char* line : {"At home, open an address on the phone and enter the code, or scan the code.",
                             "Away from home, phones connect over HTTPS with a free Let's Encrypt certificate."}) {
        DrawText(Font::Small, line, x, fy, p.sub, w - S(32)); fy += LineHeight(Font::Small);
    }
}

void WebClick(float x, float y) {
    for (int i = 0; i < kWebControls; ++i) if (g_web.controls[i].In(x, y)) {
        g_web.focus = i;
        if (i == 0 || i == 12) SDL_StartTextInput(g.window);
        else WebAction(i);
        return;
    }
    for (const auto& [rect, address] : g_web.addresses) if (rect.In(x, y)) {
        SDL_SetClipboardText(address.c_str()); ShowMessage(L"Server address copied."); return;
    }
    for (const auto& [rect, id] : g_web.devices) if (rect.In(x, y)) { RevokeWeb(id); return; }
}

std::string AboutMemory() {
    long rss = 0;
    if (FILE* f = fopen("/proc/self/status", "r")) {
        char line[128];
        while (fgets(line, sizeof(line), f))
            if (!strncmp(line, "VmRSS:", 6)) rss = atol(line + 6);
        fclose(f);
    }
    char text[200];
    snprintf(text, sizeof(text), "Memory: %.1f MB resident\nLibrary: %zu tracks in %.1f KB", rss / 1024.0, g_lib.Count(),
             g_lib.MemoryBytes() / 1024.0);
    return text;
}

void DrawAbout(const Palette& p) {
    int pw = 0, ph = 0;
    SDL_GetRenderOutputSize(g.renderer, &pw, &ph);
    Fill({0, 0, (float)pw, (float)ph}, 0x000000, 110);
    const float w = std::min((float)pw - S(16), S(560));
    const float textW = w - S(18) - S(64) - S(18) - S(18);
    std::vector<std::string> lines;
    for (const char* paragraph : {"A fast, lightweight music player and library for Windows and Linux.", "",
             "Watched library folders with an Inbox, a column browser, album and artist views, auto-playlists, ratings and "
             "play counts, a tag editor and file organizer.",
             "Cordinal decoders for FLAC, WAV, AIFF, MPEG audio, Vorbis, AAC-LC, ALAC, WMA Standard, Opus, APE, WavPack and "
             "AC3, with Matroska audio support.",
             "Gapless playback, ReplayGain with an EBU R128 scanner, 10-band equalizer, exclusive (bit-perfect) output, a "
             "waveform seek bar.",
             "A play log with cordinal points and a year in review; two games played with your own music.", ""}) {
        if (!*paragraph) { lines.emplace_back(); continue; }
        for (std::string& line : Wrap(Font::Normal, paragraph, textW)) lines.push_back(std::move(line));
    }
    const std::string memory = AboutMemory();
    for (size_t at = 0; at <= memory.size();) {
        size_t end = std::min(memory.find('\n', at), memory.size());
        lines.push_back(memory.substr(at, end - at));
        at = end + 1;
    }
    const float lineH = LineHeight(Font::Normal);
    const float h = std::min((float)ph - S(16), S(18) + S(34) + lineH * lines.size() + S(40));
    Rect panel{std::round(((float)pw - w) / 2), std::round(((float)ph - h) / 2), w, h};
    Fill(panel, p.surface);
    Frame(panel, p.border);
    g_about.icon = {panel.x + S(18), panel.y + S(18), S(64), S(64)};
    if (!g_about.texture || g_about.textureSize != (int)g_about.icon.w) {
        if (g_about.texture) SDL_DestroyTexture(g_about.texture);
        g_about.texture = nullptr;
        g_about.textureSize = (int)g_about.icon.w;
        CoverImage img{};
        if (DecodeCover(std::string(AppIcon(g_about.textureSize)), g_about.textureSize, img) &&
            (g_about.texture = SDL_CreateTexture(g.renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, img.width, img.height))) {
            SDL_UpdateTexture(g_about.texture, nullptr, img.rgba.data(), img.width * 4);
            SDL_SetTextureBlendMode(g_about.texture, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(g_about.texture, SDL_SCALEMODE_LINEAR);
        }
    }
    if (g_about.texture) {
        SDL_FRect dst{g_about.icon.x, g_about.icon.y, g_about.icon.w, g_about.icon.h};
        SDL_RenderTexture(g.renderer, g_about.texture, nullptr, &dst);
    }
    float x = g_about.icon.R() + S(18), y = panel.y + S(18);
    DrawText(Font::Title, "cordinal 0.1.10", x, y, p.text);
    y += S(34);
    for (const std::string& line : lines) {
        if (y + lineH > panel.B() - S(30)) break;
        DrawText(Font::Normal, line, x, y, p.text, textW);
        y += lineH;
    }
    DrawText(Font::Small, "Click the cardinal to hear it. Click anywhere else to close.", x, panel.B() - S(26), p.faint, textW);
}

// A click on the About box: the cardinal sings; anywhere else closes it.
void AboutClick(float x, float y) {
    if (g_about.icon.In(x, y)) PlaySfx(Sfx::CardinalSong, 0, true);
    else g_aboutOpen = false;
}


// ---------------------------------------------------------------------------
// Library > Remove recent additions

namespace {
constexpr uint32_t kRecentSeconds = 24 * 60 * 60;

std::string RecentAge(uint32_t added, uint32_t now) {
    uint32_t age = now > added ? now - added : 0;
    if (age < 3600) return std::to_string(std::max<uint32_t>(1, age / 60)) + " min ago";
    return std::to_string(age / 3600) + " h ago";
}

// The buttons sit on the panel's bottom edge; the rows fill the space above them.
struct RecentLayout {
    float rowH = 0;
    float top = 0, bottom = 0;
};
RecentLayout RecentRows(const Rect& panel) {
    RecentLayout l;
    l.rowH = S(26);
    l.top = panel.y + S(70);
    l.bottom = panel.B() - S(56);
    return l;
}
}  // namespace

void RefreshRecentAdditions() {
    const uint32_t now = UnixNow();
    g_recent.ids = TracksAddedSince(now > kRecentSeconds ? now - kRecentSeconds : 0);
    std::reverse(g_recent.ids.begin(), g_recent.ids.end());
    g_recent.checked.assign(g_recent.ids.size(), 0);
    g_recent.scroll = 0;
    g.redraw = true;
}

void OpenRecentAdditions() {
    g_recent.open = true;
    RefreshRecentAdditions();
}

void DrawRecentAdditions(const Palette& p) {
    int pw = 0, ph = 0;
    SDL_GetRenderOutputSize(g.renderer, &pw, &ph);
    Fill({0, 0, (float)pw, (float)ph}, p.bg, 110);
    const float w = std::min((float)pw - S(40), S(680));
    const float rowH = S(26), fixedH = S(130);
    const size_t count = g_recent.ids.size();
    // As many rows as fit on the window; the rest scroll with the mouse wheel.
    const float fit = std::max(1.0f, std::floor(((float)ph - S(40) - fixedH) / rowH));
    const float rows = std::min<float>(fit, (float)std::max<size_t>(1, count));
    g_recent.visible = (int)rows;
    const int maxScroll = std::max(0, (int)count - g_recent.visible);
    g_recent.scroll = std::clamp(g_recent.scroll, 0, maxScroll);
    const float h = fixedH + rows * rowH;
    Rect panel{((float)pw - w) / 2, std::max(S(20), ((float)ph - h) / 2), w, h};
    g_recent.panel = panel;
    Fill(panel, p.surface);
    Frame(panel, p.border);
    float x = panel.x + S(16), y = panel.y + S(14);
    std::string title = "Remove recent additions (" + std::to_string(count) + ")";
    DrawText(Font::Title, title, x, y, p.text, panel.w - S(32));
    y += S(34);
    std::string hint = count == 0 ? "No tracks were added in the last 24 hours."
                                  : "Check the tracks to remove. Their files stay on disk.";
    DrawText(Font::Normal, hint, x, y, p.sub, panel.w - S(32));

    const RecentLayout l = RecentRows(panel);
    g_recent.rows.clear();
    const uint32_t now = UnixNow();
    float ry = l.top;
    for (size_t i = (size_t)g_recent.scroll; i < count && ry + rowH <= l.bottom + 0.5f; ++i, ry += rowH) {
        Rect row{panel.x + S(8), ry, panel.w - S(16), rowH};
        g_recent.rows.push_back(row);
        const bool on = g_recent.checked[i] != 0;
        if (row.In(g_mouseX, g_mouseY)) Fill(row, p.hover);
        Rect box{row.x + S(6), row.y + (rowH - S(14)) / 2, S(14), S(14)};
        Fill(box, on ? p.accent : p.surface2);
        Frame(box, p.border);
        const Track& t = g_lib[g_recent.ids[i]];
        std::string name = Utf8(g_lib.Str(t.title ? t.title : t.file));
        std::string artist = Utf8(g_lib.Str(t.artist));
        std::string age = RecentAge(t.added, now);
        float ageW = TextWidth(Font::Normal, age);
        DrawText(Font::Normal, age, row.R() - S(8) - ageW, TextY(row), p.sub);
        float textX = row.x + S(30), textW = row.R() - S(16) - ageW - textX;
        DrawText(Font::Normal, artist.empty() ? name : name + "  -  " + artist, textX, TextY(row), p.text, textW);
    }

    g_recent.close = {panel.R() - S(96), panel.B() - S(44), S(80), S(28)};
    g_recent.remove = {g_recent.close.x - S(180), panel.B() - S(44), S(170), S(28)};
    g_recent.all = {panel.x + S(16), panel.B() - S(44), S(110), S(28)};
    for (auto [r, label, accent] : {std::tuple{g_recent.all, "Check all", false},
                                    std::tuple{g_recent.remove, "Remove checked", true},
                                    std::tuple{g_recent.close, "Close", false}}) {
        bool enabled = r.x == g_recent.close.x || count > 0;  // Close always works
        bool hover = r.In(g_mouseX, g_mouseY) && enabled;
        Fill(r, accent ? p.accent : (hover ? p.hover : p.surface2));
        if (!accent) Frame(r, p.border);
        uint32_t ink = accent ? TextOn(p.accent) : p.text;
        DrawText(Font::Normal, label, r.x, r.y + (r.h - LineHeight(Font::Normal)) / 2,
                 enabled ? ink : p.sub, r.w, 1);
    }
}

bool RecentAdditionsClick(float x, float y) {
    if (!g_recent.panel.In(x, y) || g_recent.close.In(x, y)) {
        g_recent.open = false;
        return true;
    }
    if (g_recent.all.In(x, y)) {
        std::fill(g_recent.checked.begin(), g_recent.checked.end(), 1);
    } else if (g_recent.remove.In(x, y)) {
        std::vector<TrackId> chosen;
        for (size_t i = 0; i < g_recent.ids.size(); ++i)
            if (g_recent.checked[i]) chosen.push_back(g_recent.ids[i]);
        if (chosen.empty()) ShowMessage(L"Check the tracks to remove first.");
        else ExcludeFromLibrary(chosen);  // asks first; ConfirmExclude refreshes this list
    } else {
        for (size_t k = 0; k < g_recent.rows.size(); ++k)
            if (g_recent.rows[k].In(x, y)) {
                size_t i = (size_t)g_recent.scroll + k;
                if (i < g_recent.checked.size()) g_recent.checked[i] = !g_recent.checked[i];
                break;
            }
    }
    g.redraw = true;
    return true;
}

void RecentAdditionsWheel(float dy) {
    const int maxScroll = std::max(0, (int)g_recent.ids.size() - g_recent.visible);
    g_recent.scroll = std::clamp(g_recent.scroll - (int)std::lround(dy * 3), 0, maxScroll);
    g.redraw = true;
}

}  // namespace lx
