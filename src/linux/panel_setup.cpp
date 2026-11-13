// The small windows Preferences and View open on Linux, each as its Windows counterpart: title formatting
// (app_format.cpp), the output device, Customize layout (app_custom.cpp), Packs (app_packs.cpp), Helpers
// (app_helpers.cpp) and the scrobbling services (app_listenbrainz.cpp).
#include "panels_internal.h"

#include "audio_output.h"
#include "listenbrainz.h"
#include "titleformat.h"

namespace lx::panel {
namespace {

// --- title formatting ---------------------------------------------------------------------------------
// Preferences > View > Title formatting: the four patterns that make the now-playing lines, the status bar
// and the window title (app_format.cpp), each with a live preview. They apply as they are typed; Cancel puts
// them back.
constexpr int kFormats = 4;
constexpr float kFmtW = 720, kFmtH = 520;

struct FormatSpec {
    const char* label;
    const wchar_t* def;
    std::wstring Settings::*target;
};
const FormatSpec kFormatSpecs[kFormats] = {
    {"Now playing", L"%title%", &Settings::fmtTitle},
    {"Now playing, second line", L"[%artist%][  \x2022  %album%][  \x2022  %date%]", &Settings::fmtSub},
    {"Status bar", L"[%codec% | ][%bitrate% kbps | ][%samplerate% Hz | ][%channels% | ]%playback_time% / %length%",
     &Settings::fmtStatus},
    {"Window title", L"[%artist% - ]%title% - cordinal", &Settings::fmtWindow},
};

struct FormatPanel {
    TextField fields[kFormats];
    std::wstring original[kFormats];
    int focus = 0;
    TrackId sample = kNone;
    Rect panel, resets[kFormats], resetAll, ok, cancel;
} g_fmt;

// The track the previews show: what's playing, else the first selected one, else the first of the library.
TrackId FormatSample() {
    if (g.playing != kNone) return g.playing;
    for (size_t r = 0; r < g.rows.size() && r < g.selected.size(); ++r)
        if (g.selected[r]) return g.rows[r];
    for (TrackId id = 0; id < (TrackId)g_lib.Count(); ++id)
        if (g_lib[id].InLibrary() && g_lib[id].Loaded()) return id;
    return kNone;
}

void FormatEdited(int i) {
    g.settings.*kFormatSpecs[i].target = g_fmt.fields[i].text;
    g.redraw = true;
}

void FormatFocus(int i) {
    g_fmt.focus = i;
    g_fmt.fields[i].all = !g_fmt.fields[i].text.empty();
    g_fmt.fields[i].caret = g_fmt.fields[i].text.size();
    g.redraw = true;
}

void FormatClose(bool keep) {
    if (keep) {
        SaveSettings();
    } else {
        for (int i = 0; i < kFormats; ++i) g.settings.*kFormatSpecs[i].target = g_fmt.original[i];
    }
    ClosePanel();
}

void DrawFormat(const Palette& p) {
    Rect panel = g_fmt.panel = DrawFrame(p, S(kFmtW), S(kFmtH), "Title formatting");
    const float pad = S(16), x0 = panel.x + pad, w = panel.w - 2 * pad;
    DrawText(Font::Small,
             g_fmt.sample == kNone
                 ? "Add some music to see previews."
                 : ("Previews show: " + Utf8(FormatForTrack(L"%title%[ \x2014 %artist%]", g_fmt.sample, false))).c_str(),
             x0, panel.y + S(46), p.faint, w);
    float y = panel.y + S(68);
    for (int i = 0; i < kFormats; ++i) {
        DrawText(Font::Normal, kFormatSpecs[i].label, x0, y, p.sub, w);
        Rect field{x0, y + S(22), w - S(88), S(28)};
        DrawLineField(g_fmt.fields[i], field, g_fmt.focus == i, true, nullptr, p);
        g_fmt.resets[i] = {field.R() + S(8), field.y, S(80), S(28)};
        DrawButton(g_fmt.resets[i], "Default", p, false, g_fmt.fields[i].text != kFormatSpecs[i].def);
        g_fmt.fields[i].rect = field;
        std::wstring preview =
            g_fmt.sample == kNone ? L"" : FormatForTrack(g_fmt.fields[i].text, g_fmt.sample, g_fmt.sample == g.playing);
        DrawText(Font::Small, preview.empty() ? "(empty)" : Utf8(preview), x0, y + S(54), p.faint, w);
        y += S(86);
    }
    auto help = Wrap(Font::Small,
                     "%field% inserts a field: title, artist, album artist, album, date, year, genre, tracknumber, discnumber, "
                     "length, codec, bitrate, samplerate, channels, rating, play_count, last_played, path, and playback_time / "
                     "playback_time_remaining in the status bar. [ ... ] disappears when its fields are empty. 'text' is "
                     "literal. $if(), $upper(), $num(), $pad() and friends are listed in the README.",
                     w);
    float hy = y + S(2);
    for (const std::string& line : help) {
        DrawText(Font::Small, line, x0, hy, p.sub, w);
        hy += LineHeight(Font::Small);
    }
    const float by = panel.B() - pad - S(28);
    g_fmt.cancel = {panel.R() - pad - S(90), by, S(90), S(28)};
    g_fmt.ok = {g_fmt.cancel.x - S(8) - S(90), by, S(90), S(28)};
    g_fmt.resetAll = {x0, by, S(120), S(28)};
    bool any = false;
    for (int i = 0; i < kFormats; ++i) any = any || g_fmt.fields[i].text != kFormatSpecs[i].def;
    DrawButton(g_fmt.resetAll, "All defaults", p, false, any);
    DrawButton(g_fmt.ok, "OK", p, true);
    DrawButton(g_fmt.cancel, "Cancel", p);
}

void FormatMouseDown(float x, float y) {
    if (!g_fmt.panel.In(x, y)) return;
    if (g_fmt.ok.In(x, y)) return FormatClose(true);
    if (g_fmt.cancel.In(x, y)) return FormatClose(false);
    if (g_fmt.resetAll.In(x, y)) {
        for (int i = 0; i < kFormats; ++i) {
            g_fmt.fields[i].text = kFormatSpecs[i].def;
            g_fmt.fields[i].caret = g_fmt.fields[i].text.size();
            FormatEdited(i);
        }
        return;
    }
    for (int i = 0; i < kFormats; ++i) {
        if (g_fmt.resets[i].In(x, y)) {
            g_fmt.fields[i].text = kFormatSpecs[i].def;
            FormatFocus(i);
            FormatEdited(i);
            return;
        }
        if (g_fmt.fields[i].rect.In(x, y)) return FormatFocus(i);
    }
}

void FormatKey(const SDL_KeyboardEvent& e) {
    if (e.key == SDLK_ESCAPE) return FormatClose(false);
    if (e.key == SDLK_RETURN || e.key == SDLK_KP_ENTER) return FormatClose(true);
    if (e.key == SDLK_TAB) return FormatFocus((g_fmt.focus + ((e.mod & SDL_KMOD_SHIFT) ? kFormats - 1 : 1)) % kFormats);
    if (LineFieldKey(g_fmt.fields[g_fmt.focus], e) == 1) FormatEdited(g_fmt.focus);
}

void FormatText(const char* s) {
    if (LineFieldInsert(g_fmt.fields[g_fmt.focus], Printable(compat::Widen(s)))) FormatEdited(g_fmt.focus);
}

// --- output device ------------------------------------------------------------------------------------
// Preferences > Output > Output device: where the music plays. PipeWire sinks are listed as the server
// reports them; "System default" follows the choice the desktop makes. The windows of the headphone page
// on Windows (headset hardware controls, per-output sound profiles) have no counterpart here.
constexpr int kDeviceRows = 9;
constexpr float kDeviceW = 560, kDeviceH = 470;

struct DevicePanel {
    std::vector<OutputDevice> devices;
    int first = 0;  // the first row shown
    Rect panel, list, refresh, close;
} g_dev;

// Row 0 is the system default, then each sink.
std::string DeviceNodeOf(int row) { return row == 0 ? std::string() : g_dev.devices[row - 1].node; }

void ChooseDevice(int row) {
    g.settings.outputDevice = DeviceNodeOf(row);
    g.engine.SetOutputDevice(g.settings.outputDevice);
    SaveSettings();
    g.redraw = true;
}

void DrawDevice(const Palette& p) {
    Rect panel = g_dev.panel = DrawFrame(p, S(kDeviceW), S(kDeviceH), "Output device");
    const float pad = S(16), x0 = panel.x + pad, w = panel.w - 2 * pad, rowH = S(30);
    DrawText(Font::Normal, "Play the music on:", x0, panel.y + S(50), p.sub, w);
    g_dev.list = {x0, panel.y + S(78), w, rowH * kDeviceRows};
    Fill(g_dev.list, p.surface2);
    Frame(g_dev.list, p.border);
    const int rows = (int)g_dev.devices.size() + 1;
    for (int i = 0; i < kDeviceRows && g_dev.first + i < rows; ++i) {
        const int row = g_dev.first + i;
        Rect r{g_dev.list.x + 1, g_dev.list.y + 1 + i * rowH, g_dev.list.w - 2, rowH};
        const bool chosen = g.settings.outputDevice == DeviceNodeOf(row);
        if (r.In(g_mx, g_my)) Fill(r, p.hover);
        DrawCheck({r.x + S(8), r.y, r.w - S(16), r.h}, row == 0 ? "System default" : g_dev.devices[row - 1].description, chosen, p);
    }
    const bool missing = !g.settings.outputDevice.empty() &&
                         std::none_of(g_dev.devices.begin(), g_dev.devices.end(), [](const OutputDevice& d) { return d.node == g.settings.outputDevice; });
    std::string note = g_dev.devices.empty() ? "No output devices were found (is PipeWire running?)."
                       : missing             ? "The chosen device (" + g.settings.outputDevice + ") isn't connected; the default plays instead."
                                             : "Exclusive output uses the chosen device too.";
    DrawText(Font::Small, note, x0, g_dev.list.B() + S(10), p.faint, w);
    const float by = panel.B() - pad - S(28);
    g_dev.close = {panel.R() - pad - S(90), by, S(90), S(28)};
    g_dev.refresh = {x0, by, S(100), S(28)};
    DrawButton(g_dev.refresh, "Refresh", p);
    DrawButton(g_dev.close, "Close", p, true);
}

void DeviceMouseDown(float x, float y) {
    if (!g_dev.panel.In(x, y) || g_dev.close.In(x, y)) return ClosePanel();
    if (g_dev.refresh.In(x, y)) {
        g_dev.devices = ListOutputDevices();
        g_dev.first = 0;
        return;
    }
    if (!g_dev.list.In(x, y)) return;
    const int row = g_dev.first + (int)((y - g_dev.list.y - 1) / S(30));
    if (row >= 0 && row <= (int)g_dev.devices.size()) ChooseDevice(row);
}

void DeviceKey(const SDL_KeyboardEvent& e) {
    if (e.key == SDLK_ESCAPE || e.key == SDLK_RETURN || e.key == SDLK_KP_ENTER) return ClosePanel();
    if (e.key == SDLK_F5) {
        g_dev.devices = ListOutputDevices();
        return;
    }
    if (e.key != SDLK_UP && e.key != SDLK_DOWN) return;
    int current = 0;
    for (int i = 0; i < (int)g_dev.devices.size(); ++i)
        if (g_dev.devices[i].node == g.settings.outputDevice) current = i + 1;
    const int next = std::clamp(current + (e.key == SDLK_UP ? -1 : 1), 0, (int)g_dev.devices.size());
    ChooseDevice(next);
    if (next < g_dev.first) g_dev.first = next;
    else if (next >= g_dev.first + kDeviceRows) g_dev.first = next - kDeviceRows + 1;
}

// --- customize the layout --------------------------------------------------------------------------------
// View > Customize layout: which parts of the main window show (app_custom.cpp's parts). A hidden part gives its
// room back to the rest; the track list and the menu bar always stay. Choices save as they are made.
struct LayoutPart {
    unsigned bit;
    const char* label;
    bool WindowLayout::*flag = nullptr;
};
const LayoutPart kLayoutParts[] = {
    {kPartTransport, "Previous, play and next"}, {kPartOrder, "Shuffle and repeat"},
    {kPartWave, "Waveform seek bar"}, {kPartTimes, "Times"}, {kPartVolume, "Volume"},
    {kPartNowPlaying, "Now playing"}, {kPartTabs, "Playlist tabs"}, {kPartSearch, "Search box"},
    {kPartViews, "Library view buttons"}, {kPartStatus, "Status bar"},
    {0, "Large cover", &WindowLayout::largeCover}, {0, "Level and points", &WindowLayout::points},
    {0, "Up next panel", &WindowLayout::upNext}, {0, "Menu bar", &WindowLayout::menuBar},
    {0, "Track counts on tabs", &WindowLayout::tabCounts},
    {0, "Audio details in status bar", &WindowLayout::statusDetails},
    {0, "Playback hints", &WindowLayout::hints}, {0, "Waveform on its own row", &WindowLayout::waveOwnRow},
    {0, "Side panels on the left", &WindowLayout::panelsLeft}
};
struct LayoutPanel {
    Rect panel, rows[std::size(kLayoutParts)], dock, wave, reset, close, presets;
    int focus = 0;
} g_layout;

bool PartShown(unsigned bit) { return !(g.settings.defaultLayout.hiddenParts & bit); }

void SetPart(unsigned bit, bool shown) {
    if (shown) g.settings.defaultLayout.hiddenParts &= ~bit;
    else g.settings.defaultLayout.hiddenParts |= bit;
    g.settings.layout = g.settings.defaultLayout;
    SaveSettings();
    g.redraw = true;
}

void LayoutActivate(int index) {
    if (index < (int)std::size(kLayoutParts)) {
        const auto& part = kLayoutParts[index];
        if (part.flag) {
            g.settings.defaultLayout.*part.flag = !(g.settings.defaultLayout.*part.flag);
            g.settings.layout = g.settings.defaultLayout;
            SaveSettings();
            g.redraw = true;
        } else SetPart(part.bit, !PartShown(part.bit));
    } else if (index == (int)std::size(kLayoutParts)) {
        OpenChoice(g_layout.dock.x, g_layout.dock.B(), {"Top", "Bottom", "Left", "Right"},
            g.settings.defaultLayout.dock, [](int i) { g.settings.defaultLayout.dock = i; g.settings.layout = g.settings.defaultLayout; g.redraw = true; SaveSettings(); });
    } else if (index == (int)std::size(kLayoutParts) + 1) {
        OpenChoice(g_layout.wave.x, g_layout.wave.B(), {"Slim", "Medium", "Tall"},
            g.settings.defaultLayout.waveSize, [](int i) { g.settings.defaultLayout.waveSize = i; g.settings.layout = g.settings.defaultLayout; g.redraw = true; SaveSettings(); });
    } else if (index == (int)std::size(kLayoutParts) + 2) {
        g.settings.defaultLayout = WindowLayout{};
        g.settings.layout = g.settings.defaultLayout;
        SaveSettings();
        g.redraw = true;
    } else if (index == (int)std::size(kLayoutParts) + 3) {
        std::vector<std::string> names{"Classic", "Player at the bottom", "Player on the side", "Waveform studio", "Minimal"};
        for (const auto& name : PackLayoutNames()) names.push_back(Utf8(name));
        OpenChoice(g_layout.presets.x, g_layout.presets.B(), names, -1, [](int i) {
            if (i >= 5) { ApplyPackLayout(i - 5); return; }
            auto& l = g.settings.defaultLayout;
            l = WindowLayout{};
            if (i == 1) { l.dock = 1; l.waveSize = 1; l.waveOwnRow = l.largeCover = true; }
            if (i == 2) { l.dock = 2; l.waveSize = 1; l.upNext = true; }
            if (i == 3) { l.waveSize = 2; l.waveOwnRow = l.largeCover = true; }
            if (i == 4) l.hiddenParts = kPartOrder | kPartNowPlaying | kPartViews | kPartStatus;
            g.settings.layout = g.settings.defaultLayout;
            SaveSettings(); g.redraw = true;
        });
    } else ClosePanel();
}

void DrawLayoutPanel(const Palette& p) {
    Rect panel = g_layout.panel = DrawFrame(p, S(680), S(480), "Customize layout");
    float x = panel.x + S(20), y = panel.y + S(50), w = panel.w - S(40);
    DrawText(Font::Normal, "Changes apply now. Keep them for startup from Startup defaults.", x, y, p.sub, w);
    y += S(30);
    const char* docks[] = {"Top", "Bottom", "Left", "Right"};
    const char* sizes[] = {"Slim", "Medium", "Tall"};
    g_layout.dock = {x, y, w / 2 - S(10), S(28)};
    g_layout.wave = {x + w / 2 + S(10), y, w / 2 - S(10), S(28)};
    DrawButton(g_layout.dock, std::string("Player: ") + docks[g.settings.defaultLayout.dock], p);
    DrawButton(g_layout.wave, std::string("Waveform: ") + sizes[g.settings.defaultLayout.waveSize], p);
    y += S(40);
    constexpr int perColumn = (std::size(kLayoutParts) + 1) / 2;
    float rowH = std::min(S(26), std::max(LineHeight(Font::Normal) + S(2),
        (panel.B() - S(46) - y) / perColumn));
    for (size_t i = 0; i < std::size(kLayoutParts); ++i) {
        const auto& part = kLayoutParts[i];
        g_layout.rows[i] = {x + (i / perColumn) * w / 2, y + (i % perColumn) * rowH, w / 2 - S(8), rowH};
        DrawCheck(g_layout.rows[i], part.label, part.flag ? g.settings.defaultLayout.*part.flag : PartShown(part.bit), p);
        if (g_layout.focus == (int)i) Frame(g_layout.rows[i], p.accent);
    }
    float by = panel.B() - S(46);
    g_layout.reset = {x, by, S(160), S(28)};
    g_layout.presets = {x + S(170), by, S(100), S(28)};
    g_layout.close = {panel.R() - S(110), by, S(90), S(28)};
    DrawButton(g_layout.reset, "Reset the layout", p);
    DrawButton(g_layout.presets, "Presets...", p);
    DrawButton(g_layout.close, "Close", p, true);
    const Rect controls[] = {g_layout.dock, g_layout.wave, g_layout.reset, g_layout.presets, g_layout.close};
    for (int i = 0; i < 5; ++i)
        if (g_layout.focus == (int)std::size(kLayoutParts) + i) Frame(controls[i], p.accent);
}

void LayoutMouseDown(float x, float y) {
    if (!g_layout.panel.In(x, y)) return ClosePanel();
    for (size_t i = 0; i < std::size(kLayoutParts); ++i)
        if (g_layout.rows[i].In(x, y)) { g_layout.focus = (int)i; return LayoutActivate(g_layout.focus); }
    const Rect controls[] = {g_layout.dock, g_layout.wave, g_layout.reset, g_layout.presets, g_layout.close};
    for (int i = 0; i < 5; ++i)
        if (controls[i].In(x, y)) { g_layout.focus = (int)std::size(kLayoutParts) + i; return LayoutActivate(g_layout.focus); }
}

void LayoutKey(const SDL_KeyboardEvent& e) {
    constexpr int count = std::size(kLayoutParts) + 5;
    if (e.key == SDLK_ESCAPE) ClosePanel();
    else if (e.key == SDLK_TAB) g_layout.focus = (g_layout.focus + (e.mod & SDL_KMOD_SHIFT ? count - 1 : 1)) % count;
    else if (e.key == SDLK_DOWN || e.key == SDLK_RIGHT) g_layout.focus = (g_layout.focus + 1) % count;
    else if (e.key == SDLK_UP || e.key == SDLK_LEFT) g_layout.focus = (g_layout.focus + count - 1) % count;
    else if (e.key == SDLK_SPACE || e.key == SDLK_RETURN || e.key == SDLK_KP_ENTER) LayoutActivate(g_layout.focus);
}

// --- packs ---------------------------------------------------------------------------------------------------
// View > Packs: the packs in the data folder, each with a switch, what it holds and what was left out of it.
constexpr int kPackRows = 8;
struct PacksPanel {
    int selected = 0, first = 0;
    Rect panel, list, install, installFolder, folder, close;
} g_packsUi;

void DrawPacks(const Palette& p) {
    Rect panel = g_packsUi.panel = DrawFrame(p, S(640), S(540), "Packs");
    const float pad = S(16), x0 = panel.x + pad, w = panel.w - 2 * pad, rowH = S(30);
    const size_t count = PackCount();
    DrawText(Font::Normal, count ? "A pack adds formats and title functions. Turn one on to use it."
                                 : "No packs yet. Install a .cordpack file, or a folder with a pack.ini in it.",
             x0, panel.y + S(48), p.sub, w);
    g_packsUi.list = {x0, panel.y + S(76), w, rowH * kPackRows};
    Fill(g_packsUi.list, p.surface2);
    Frame(g_packsUi.list, p.border);
    for (int i = 0; i < kPackRows && g_packsUi.first + i < (int)count; ++i) {
        const int row = g_packsUi.first + i;
        PackRow pack = PackAt(row);
        Rect r{g_packsUi.list.x + 1, g_packsUi.list.y + 1 + i * rowH, g_packsUi.list.w - 2, rowH};
        if (row == g_packsUi.selected) Fill(r, p.select);
        else if (r.In(g_mx, g_my)) Fill(r, p.hover);
        std::string label = Utf8(pack.name) + (pack.version.empty() ? "" : "  " + Utf8(pack.version));
        DrawCheck({r.x + S(8), r.y, r.w - S(16), r.h}, label, pack.on, p);
    }
    float y = g_packsUi.list.B() + S(10);
    const float limit = panel.B() - pad - S(40);
    if (g_packsUi.selected >= 0 && g_packsUi.selected < (int)count) {
        PackRow pack = PackAt(g_packsUi.selected);
        std::string text = pack.error.empty() ? Utf8(pack.summary) : "Can't be used: " + Utf8(pack.error);
        if (!pack.author.empty()) text = "By " + Utf8(pack.author) + ". " + text;
        if (!pack.description.empty()) text += "  " + Utf8(pack.description);
        for (const std::wstring& warning : pack.warnings) text += "  \xE2\x80\xA2 " + Utf8(warning);
        for (const std::string& line : Wrap(Font::Small, text, w)) {
            if (y + LineHeight(Font::Small) > limit) break;
            DrawText(Font::Small, line, x0, y, p.sub, w);
            y += LineHeight(Font::Small);
        }
    }
    const float by = panel.B() - pad - S(28);
    g_packsUi.close = {panel.R() - pad - S(90), by, S(90), S(28)};
    g_packsUi.folder = {g_packsUi.close.x - S(8) - S(130), by, S(130), S(28)};
    g_packsUi.installFolder = {g_packsUi.folder.x - S(8) - S(150), by, S(150), S(28)};
    g_packsUi.install = {g_packsUi.installFolder.x - S(8) - S(120), by, S(120), S(28)};
    DrawButton(g_packsUi.install, "Install file...", p);
    DrawButton(g_packsUi.installFolder, "Install folder...", p);
    DrawButton(g_packsUi.folder, "Open the folder", p);
    DrawButton(g_packsUi.close, "Close", p, true);
}

void PacksMouseDown(float x, float y) {
    if (!g_packsUi.panel.In(x, y) || g_packsUi.close.In(x, y)) return ClosePanel();
    if (g_packsUi.install.In(x, y)) {
        static const SDL_DialogFileFilter filter{"Packs", "cordpack;zip"};
        return ChooseFile(kFilePack, filter);
    }
    if (g_packsUi.installFolder.In(x, y)) return PickFolder(kFilePack);
    if (g_packsUi.folder.In(x, y)) return OpenPacksFolder();
    if (g_packsUi.list.In(x, y)) {
        int row = g_packsUi.first + (int)((y - g_packsUi.list.y - 1) / S(30));
        if (row < 0 || row >= (int)PackCount()) return;
        g_packsUi.selected = row;
        if (x < g_packsUi.list.x + S(30)) TogglePack(row);
    }
}

void PacksKey(const SDL_KeyboardEvent& e) {
    const int count = (int)PackCount();
    if (e.key == SDLK_ESCAPE) return ClosePanel();
    if (e.key == SDLK_UP) g_packsUi.selected = std::max(0, g_packsUi.selected - 1);
    else if (e.key == SDLK_DOWN) g_packsUi.selected = std::min(count - 1, g_packsUi.selected + 1);
    else if (e.key == SDLK_SPACE || e.key == SDLK_RETURN) TogglePack(g_packsUi.selected);
    if (g_packsUi.selected < g_packsUi.first) g_packsUi.first = g_packsUi.selected;
    else if (g_packsUi.selected >= g_packsUi.first + kPackRows) g_packsUi.first = g_packsUi.selected - kPackRows + 1;
}

// --- helpers -------------------------------------------------------------------------------------------------
// View > Helpers: the helpers in the data folder, each with a switch, what it does, the programs it runs and what
// was left out. Turning one on asks first and records its files; a changed file turns it off again.
constexpr int kHelperRows = 8;
struct HelpersPanel {
    int selected = 0, first = 0;
    Rect panel, list, installFolder, folder, close, visualizers;
} g_helpersUi;

void DrawHelpers(const Palette& p) {
    Rect panel = g_helpersUi.panel = DrawFrame(p, S(680), S(560), "Helpers");
    const float pad = S(16), x0 = panel.x + pad, w = panel.w - 2 * pad, rowH = S(30);
    const size_t count = HelperCount();
    std::string intro = count ? "A helper is a separate program that plays a file type, tags it or runs on tracks. Turn one on to use it."
                              : "No helpers yet. Install a folder with a helper.ini in it (see the README, \"Helpers\").";
    if (count && !HelperAt(0).offAll.empty()) intro = "Helpers are " + Utf8(HelperAt(0).offAll) + ".";
    float y = panel.y + S(48);
    for (const std::string& line : Wrap(Font::Normal, intro, w)) {
        DrawText(Font::Normal, line, x0, y, p.sub, w);
        y += LineHeight(Font::Normal);
    }
    g_helpersUi.list = {x0, panel.y + S(88), w, rowH * kHelperRows};
    Fill(g_helpersUi.list, p.surface2);
    Frame(g_helpersUi.list, p.border);
    for (int i = 0; i < kHelperRows && g_helpersUi.first + i < (int)count; ++i) {
        const int row = g_helpersUi.first + i;
        HelperRow helper = HelperAt(row);
        Rect r{g_helpersUi.list.x + 1, g_helpersUi.list.y + 1 + i * rowH, g_helpersUi.list.w - 2, rowH};
        if (row == g_helpersUi.selected) Fill(r, p.select);
        else if (r.In(g_mx, g_my)) Fill(r, p.hover);
        std::string label = Utf8(helper.name) + (helper.version.empty() ? "" : "  " + Utf8(helper.version));
        DrawCheck({r.x + S(8), r.y, r.w - S(16), r.h}, label, helper.on, p);
    }
    y = g_helpersUi.list.B() + S(10);
    const float limit = panel.B() - pad - S(40);
    if (g_helpersUi.selected >= 0 && g_helpersUi.selected < (int)count) {
        HelperRow helper = HelperAt(g_helpersUi.selected);
        std::string text = helper.error.empty() ? "It " + Utf8(helper.summary) + "." : "Can't be used: " + Utf8(helper.error);
        if (!helper.programs.empty()) text += " It runs " + Utf8(helper.programs) + ".";
        if (!helper.author.empty()) text = "By " + Utf8(helper.author) + ". " + text;
        if (!helper.description.empty()) text += "  " + Utf8(helper.description);
        if (!helper.why.empty()) text += "  It is off: " + Utf8(helper.why) + ".";
        for (const std::wstring& warning : helper.warnings) text += "  \xE2\x80\xA2 " + Utf8(warning);
        for (const std::string& line : Wrap(Font::Small, text, w)) {
            if (y + LineHeight(Font::Small) > limit) break;
            DrawText(Font::Small, line, x0, y, p.sub, w);
            y += LineHeight(Font::Small);
        }
    }
    const float by = panel.B() - pad - S(28);
    const float buttonW = (w - S(24)) / 4;
    g_helpersUi.visualizers = {x0, by, buttonW, S(28)};
    g_helpersUi.installFolder = {x0 + buttonW + S(8), by, buttonW, S(28)};
    g_helpersUi.folder = {x0 + 2 * (buttonW + S(8)), by, buttonW, S(28)};
    g_helpersUi.close = {x0 + 3 * (buttonW + S(8)), by, buttonW, S(28)};
    DrawButton(g_helpersUi.visualizers, "Visualizers...", p, false, !HelperVisualizerLabels().empty());
    DrawButton(g_helpersUi.installFolder, "Install folder...", p);
    DrawButton(g_helpersUi.folder, "Open the folder", p);
    DrawButton(g_helpersUi.close, "Close", p, true);
}

void HelpersMouseDown(float x, float y) {
    if (!g_helpersUi.panel.In(x, y) || g_helpersUi.close.In(x, y)) return ClosePanel();
    if (g_helpersUi.visualizers.In(x, y)) return OpenHelperVisualizers(x, y);
    if (g_helpersUi.installFolder.In(x, y)) return PickFolder(kFileHelper);
    if (g_helpersUi.folder.In(x, y)) return OpenHelpersFolder();
    if (g_helpersUi.list.In(x, y)) {
        int row = g_helpersUi.first + (int)((y - g_helpersUi.list.y - 1) / S(30));
        if (row < 0 || row >= (int)HelperCount()) return;
        g_helpersUi.selected = row;
        if (x < g_helpersUi.list.x + S(30)) ToggleHelper(row);
    }
}

void HelpersKey(const SDL_KeyboardEvent& e) {
    if (e.key == SDLK_V) return OpenHelperVisualizers(g_helpersUi.visualizers.x, g_helpersUi.visualizers.B());
    const int count = (int)HelperCount();
    if (e.key == SDLK_ESCAPE) return ClosePanel();
    if (e.key == SDLK_UP) g_helpersUi.selected = std::max(0, g_helpersUi.selected - 1);
    else if (e.key == SDLK_DOWN) g_helpersUi.selected = std::min(count - 1, g_helpersUi.selected + 1);
    else if (e.key == SDLK_SPACE || e.key == SDLK_RETURN) ToggleHelper(g_helpersUi.selected);
    if (g_helpersUi.selected < g_helpersUi.first) g_helpersUi.first = g_helpersUi.selected;
    else if (g_helpersUi.selected >= g_helpersUi.first + kHelperRows) g_helpersUi.first = g_helpersUi.selected - kHelperRows + 1;
}

// --- ListenBrainz ----------------------------------------------------------------------------------------
// The shared scrobbling setup, with separate account connections for all three services.
struct LbPanel {
    TextField token, key, secret;
    listenbrainz::Service selected = listenbrainz::Service::ListenBrainz;
    bool checking = false;
    int focus = 1;
    std::wstring pendingToken, authKey, authSecret, authToken, message;
    Rect panel, field, keyField, secretField, connect, disconnect, account, close, services[3];
} g_lb;

constexpr float kLbW = 620, kLbH = 440;
bool LbBrainz() { return g_lb.selected == listenbrainz::Service::ListenBrainz; }
TextField& LbEditor() { return LbBrainz() ? g_lb.token : g_lb.focus == 2 ? g_lb.secret : g_lb.key; }
void LbSelect(int selected) {
    listenbrainz::CancelAuthorization(g_lb.selected);
    g_lb = LbPanel{};
    g_lb.selected = (listenbrainz::Service)((selected + 3) % 3);
}
void DrawListenBrainz(const Palette& p) {
    Rect panel = g_lb.panel = DrawFrame(p, S(kLbW), S(kLbH), "Scrobbling");
    const float pad = S(16), lh = LineHeight(Font::Normal);
    const char* names[] = {"ListenBrainz", "Last.fm", "Libre.fm"};
    for (int i = 0; i < 3; ++i) {
        g_lb.services[i] = {panel.x + pad + S(190) * i, panel.y + S(48), S(180), S(28)};
        DrawButton(g_lb.services[i], names[i], p, (int)g_lb.selected == i);
    }
    auto lines = Wrap(Font::Normal, LbBrainz() ?
        "Paste your ListenBrainz user token from its settings. Songs are sent after half their duration or four minutes of listening." :
        "Enter this service's application API key and secret, then Connect. Approve access in your browser and return to Finish sign-in. Tracks must be longer than 30 seconds.",
        panel.w - 2 * pad);
    float y = panel.y + S(90);
    for (const auto& line : lines) {
        DrawText(Font::Normal, line, panel.x + pad, y, p.text, panel.w - 2 * pad); y += lh;
    }
    auto field = [&](TextField& editor, Rect& rect, const char* label, int focus, bool secret) {
        DrawText(Font::Normal, label, panel.x + pad, y + S(6), p.text, S(96));
        rect = {panel.x + pad + S(100), y, panel.w - 2 * pad - S(100), S(28)};
        TextField display = editor;
        if (secret) display.text.assign(editor.text.size(), L'\x2022');
        DrawLineField(display, rect, g_lb.focus == focus, !g_lb.checking, nullptr, p);
        y += S(38);
    };
    y = panel.y + S(170);
    if (LbBrainz()) field(g_lb.token, g_lb.field, "User token", 1, true);
    else {
        field(g_lb.key, g_lb.keyField, "API key", 1, false);
        field(g_lb.secret, g_lb.secretField, "API secret", 2, true);
    }
    g_lb.account = {panel.x + pad, panel.y + S(250), S(140), S(28)};
    DrawButton(g_lb.account, LbBrainz() ? "Get user token" : "Get API key", p, g_lb.focus == 3);
    const auto status = listenbrainz::GetStatus(g_lb.selected);
    std::wstring text = g_lb.checking ? L"Contacting the service..." : g_lb.message;
    if (text.empty()) {
        text = listenbrainz::Enabled(g_lb.selected) ? (status.user.empty() ? L"Connected." : L"Connected as " + status.user + L".") :
               L"Not connected. Scrobbling is off.";
        if (listenbrainz::Paused()) text += L" Scrobbling is paused in Preferences.";
        if (status.queued) text += L" " + std::to_wstring(status.queued) + L" listens waiting.";
        if (!status.lastError.empty()) text += L" " + status.lastError + L".";
    }
    y = panel.y + S(292);
    for (const auto& line : Wrap(Font::Normal, Utf8(text), panel.w - 2 * pad)) {
        DrawText(Font::Normal, line, panel.x + pad, y, p.sub, panel.w - 2 * pad); y += lh;
    }
    DrawText(Font::Small, "Credentials are readable only by you. Songs wait in separate queues while offline.",
        panel.x + pad, panel.B() - S(76), p.faint, panel.w - 2 * pad);
    const float by = panel.B() - pad - S(28);
    g_lb.close = {panel.R() - pad - S(90), by, S(90), S(28)};
    g_lb.disconnect = {g_lb.close.x - S(8) - S(110), by, S(110), S(28)};
    g_lb.connect = {panel.x + pad, by, S(140), S(28)};
    DrawButton(g_lb.connect, g_lb.authToken.empty() ? "Connect" : "Finish sign-in", p, g_lb.focus == 4, !g_lb.checking);
    DrawButton(g_lb.disconnect, "Disconnect", p, g_lb.focus == 5,
        listenbrainz::Enabled(g_lb.selected) || g_lb.checking || !g_lb.authToken.empty());
    DrawButton(g_lb.close, "Close", p, g_lb.focus == 6);
}
void LbConnect() {
    if (g_lb.checking) return;
    g_lb.message.clear();
    if (LbBrainz()) {
        g_lb.pendingToken = g_lb.token.text;
        if (g_lb.pendingToken.empty()) { g_lb.focus = 1; return; }
        g_lb.checking = true;
        listenbrainz::Validate(g_lb.pendingToken);
    } else {
        if (g_lb.authToken.empty()) { g_lb.authKey = g_lb.key.text; g_lb.authSecret = g_lb.secret.text; }
        if (g_lb.authKey.empty() || g_lb.authSecret.empty()) {
            g_lb.message = L"Enter the API key and secret from this service first."; return;
        }
        g_lb.checking = true;
        listenbrainz::Authorize(g_lb.selected, g_lb.authKey, g_lb.authSecret, g_lb.authToken);
    }
}
void LbDisconnect() {
    listenbrainz::CancelAuthorization(g_lb.selected);
    bool saved = listenbrainz::SetToken(L"", g_lb.selected);
    int selected = (int)g_lb.selected;
    LbSelect(selected);
    g_lb.message = saved ? L"Disconnected. Scrobbling is off." : L"Could not remove the saved connection.";
}
void LbAccount() {
    SDL_OpenURL(LbBrainz() ? "https://listenbrainz.org/settings/" :
        g_lb.selected == listenbrainz::Service::LastFm ? "https://www.last.fm/api/account/create" : "https://libre.fm/api-keys.php");
}
void LbMouseDown(float x, float y) {
    if (!g_lb.panel.In(x, y) || g_lb.close.In(x, y)) return ClosePanel();
    for (int i = 0; i < 3; ++i) if (g_lb.services[i].In(x, y)) return LbSelect(i);
    if (g_lb.connect.In(x, y)) { g_lb.focus = 4; return LbConnect(); }
    if (g_lb.disconnect.In(x, y)) return LbDisconnect();
    if (g_lb.account.In(x, y)) { g_lb.focus = 3; return LbAccount(); }
    if (LbBrainz() ? g_lb.field.In(x, y) : g_lb.keyField.In(x, y)) g_lb.focus = 1;
    if (!LbBrainz() && g_lb.secretField.In(x, y)) g_lb.focus = 2;
}
void LbKey(const SDL_KeyboardEvent& e) {
    if (e.key == SDLK_ESCAPE) return ClosePanel();
    if (e.key == SDLK_TAB) {
        do { g_lb.focus = (g_lb.focus + ((e.mod & SDL_KMOD_SHIFT) ? 6 : 1)) % 7; }
        while (LbBrainz() && g_lb.focus == 2);
        return;
    }
    if (g_lb.focus == 0 && (e.key == SDLK_LEFT || e.key == SDLK_RIGHT)) {
        LbSelect((int)g_lb.selected + (e.key == SDLK_LEFT ? -1 : 1)); g_lb.focus = 0; return;
    }
    if (e.key == SDLK_RETURN || e.key == SDLK_KP_ENTER) {
        if (g_lb.focus == 3) return LbAccount();
        if (g_lb.focus == 5) return LbDisconnect();
        if (g_lb.focus == 6) return ClosePanel();
        return LbConnect();
    }
    if (g_lb.checking || (g_lb.focus != 1 && g_lb.focus != 2)) return;
    if ((e.mod & SDL_KMOD_CTRL) && e.key == SDLK_V) {
        if (char* clip = SDL_GetClipboardText()) {
            LineFieldInsert(LbEditor(), compat::Widen(clip)); SDL_free(clip);
        }
        return;
    }
    LineFieldKey(LbEditor(), e);
}
void LbText(const std::wstring& text) {
    if (!g_lb.checking && (g_lb.focus == 1 || g_lb.focus == 2)) LineFieldInsert(LbEditor(), text);
}

}  // namespace

const Ops kFormatOps{
    .draw = DrawFormat,
    .mouseDown = [](float x, float y, int) { FormatMouseDown(x, y); },
    .key = FormatKey,
    .text = FormatText,
    .wantsText = [](float x, float y) {
        for (const TextField& t : g_fmt.fields)
            if (t.rect.In(x, y)) return true;
        return false;
    },
    .typing = true,
};

const Ops kDeviceOps{
    .draw = DrawDevice,
    .mouseDown = [](float x, float y, int) { DeviceMouseDown(x, y); },
    .wheel = [](float, float, float dy) {
        g_dev.first = std::clamp(g_dev.first - (int)std::lround(dy * 2), 0, std::max(0, (int)g_dev.devices.size() + 1 - kDeviceRows));
    },
    .key = DeviceKey,
};

const Ops kLayoutOps{
    .draw = DrawLayoutPanel,
    .mouseDown = [](float x, float y, int) { LayoutMouseDown(x, y); },
    .key = LayoutKey,
};

const Ops kPacksOps{
    .draw = DrawPacks,
    .mouseDown = [](float x, float y, int) { PacksMouseDown(x, y); },
    .wheel = [](float, float, float dy) {
        g_packsUi.first = std::clamp(g_packsUi.first - (int)std::lround(dy * 2), 0, std::max(0, (int)PackCount() - kPackRows));
    },
    .key = PacksKey,
    .file = [](int purpose, const std::wstring& path) { if (purpose == kFilePack) InstallPack(path); },
};

const Ops kHelpersOps{
    .draw = DrawHelpers,
    .mouseDown = [](float x, float y, int) { HelpersMouseDown(x, y); },
    .wheel = [](float, float, float dy) {
        g_helpersUi.first = std::clamp(g_helpersUi.first - (int)std::lround(dy * 2), 0, std::max(0, (int)HelperCount() - kHelperRows));
    },
    .key = HelpersKey,
    .file = [](int purpose, const std::wstring& path) { if (purpose == kFileHelper) InstallHelper(path); },
};

const Ops kListenBrainzOps{
    .draw = DrawListenBrainz,
    .mouseDown = [](float x, float y, int) { LbMouseDown(x, y); },
    .key = LbKey,
    .text = [](const char* s) { LbText(compat::Widen(s)); },
    .wantsText = [](float x, float y) {
        return LbBrainz() ? g_lb.field.In(x, y) : g_lb.keyField.In(x, y) || g_lb.secretField.In(x, y);
    },
    .close = [] {  // a token is not left lying in memory
        listenbrainz::CancelAuthorization(g_lb.selected);
        g_lb = LbPanel{};
    },
    .typing = true,
};

}  // namespace lx::panel

namespace lx {
using namespace panel;

void OpenHelpers() {
    g_helpersUi = HelpersPanel{};
    g_kind = Kind::Helpers;
    g.redraw = true;
}

void OpenPacks() {
    g_packsUi = PacksPanel{};
    g_kind = Kind::Packs;
    g.redraw = true;
}

void OpenLayout() {
    g_kind = Kind::Layout;
    g.redraw = true;
}
void OpenOutputDevice() {
    g_dev = DevicePanel{};
    g_dev.devices = ListOutputDevices();
    g_kind = Kind::Device;
    g.redraw = true;
}

void OpenFormat() {
    for (int i = 0; i < kFormats; ++i) {
        g_fmt.original[i] = g.settings.*kFormatSpecs[i].target;
        g_fmt.fields[i] = TextField{};
        g_fmt.fields[i].text = g_fmt.original[i];
        g_fmt.fields[i].caret = g_fmt.original[i].size();
    }
    g_fmt.sample = FormatSample();
    g_kind = Kind::Format;
    FormatFocus(0);
    SDL_StartTextInput(g.window);
}
void OpenListenBrainz() {
    ClosePanel();
    g_lb = LbPanel{};
    g_kind = Kind::ListenBrainz;
    SDL_StartTextInput(g.window);
    g.redraw = true;
}

// The answer to a token check (wParam 0 refused, 1 accepted, 2 unreachable; lParam the user name when
// accepted) or a status change (3), on the UI thread.
void OnListenBrainzMessage(WPARAM wp, LPARAM lp) {
    auto service = (listenbrainz::Service)((wp >> 4) & 3);
    unsigned result = (unsigned)wp % 16;
    std::unique_ptr<std::wstring> value(reinterpret_cast<std::wstring*>(lp));
    if (result == listenbrainz::kStatusChanged && g_lb.selected == service) g_lb.message.clear();
    if (listenbrainz::CurrentResult(wp) && g_kind == Kind::ListenBrainz && g_lb.selected == service && result != listenbrainz::kStatusChanged && g_lb.checking) {
        g_lb.checking = false;
        if (result == 4 && value) {
            g_lb.authToken = *value;
            SDL_OpenURL(Utf8(listenbrainz::AuthorizationUrl(service, g_lb.authKey, g_lb.authToken)).c_str());
            g_lb.message = L"Approve access in your browser, then choose Finish sign-in.";
        } else if ((result == 1 || result == 5) && value) {
            bool saved = listenbrainz::SetToken(result == 1 ? g_lb.pendingToken : *value, service);
            g_lb.message = saved ? L"Connected. Scrobbling is on." : L"Could not save the connection.";
            if (saved) {
                g_lb.token = TextField{}; g_lb.key = TextField{}; g_lb.secret = TextField{};
                g_lb.authToken.clear(); g_lb.authKey.clear(); g_lb.authSecret.clear(); g_lb.pendingToken.clear();
            }
        } else {
            g_lb.message = result == 2 ? L"Couldn't reach the service. Check your connection and try again." :
                L"Sign-in wasn't accepted. Check the credentials and browser approval, then retry.";
        }
    }
    g.redraw = true;
}

}  // namespace lx
