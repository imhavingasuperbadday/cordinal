// Headphones page on Linux: explicit headset commands, copied worker snapshots, and isolated sound
// profiles following the actual PipeWire sink. Only the UI thread touches application state.
#include "headphones_ui.h"
#include "output_profiles.h"
#include "panels.h"
#include "ui_internal.h"

#include <filesystem>

namespace lx {
namespace {
HeadsetController g_controller;
HeadsetInfo g_headset;
OutputProfiles g_profiles;
bool g_loaded = false;   // profiles read; Global() is valid
bool g_started = false;  // the output poller is running
struct HeadphonesPanel {
    Rect panel, viewport;
    float scroll = 0, maxScroll = 0;
    bool reveal = false;
    std::array<Rect, 10> controls{};
    HeadsetChanges changes;
    float headroom = 0;
    int focus = 0;
    uint64_t revision = UINT64_MAX;
    std::wstring id;
} H;
constexpr int kPresets[] = {-1, 7, 8, 9, 255, 250, 254, 251, 253, 252};
const char* kPresetNames[] = {"Unchanged", "Game", "Music", "Movie", "Custom (saved curve)",
    "Game EQ 1 (stored)", "Game EQ 2 (stored)", "Game EQ 3 (stored)", "Game EQ 4 (stored)", "Game EQ 5 (stored)"};
int PresetIndex(int value) {
    for (int i = 0; i < (int)std::size(kPresets); ++i) if (kPresets[i] == value) return i;
    return 0;
}
void SyncPanel() {
    if (H.id == g_headset.id && H.revision == g_headset.revision) return;
    if (H.id != g_headset.id) H.scroll = 0;
    H.id = g_headset.id;
    H.revision = g_headset.revision;
    H.changes = {g_headset.sidetone, g_headset.idleMinutes, g_headset.dnd, g_headset.preset};
    H.headroom = g_profiles.Headroom();
}
bool Enabled(int index) {
    if (index >= 8) return true;
    if (g_headset.busy) return false;
    if (index == 6) return true;
    if (g_headset.id.empty()) return false;
    if (index == 1) return g_headset.sidetone >= 0;
    if (index == 2) return g_headset.idleMinutes >= 0;
    if (index == 3) return g_headset.dnd >= 0;
    if (index == 4) return g_headset.preset >= 0;
    return true;
}
void Apply() {
    if (!g_started || !Enabled(7)) return;
    g_profiles.SetHeadroom(H.headroom);
    SaveSettings();
    RefreshGains();
    auto changes = H.changes;
    if (changes.sidetone == g_headset.sidetone) changes.sidetone = -1;
    if (changes.idleMinutes == g_headset.idleMinutes) changes.idleMinutes = -1;
    if (changes.dnd == g_headset.dnd) changes.dnd = -1;
    if (changes.sidetone < 0 && changes.idleMinutes < 0 && changes.dnd < 0 && changes.preset < 0) {
        ShowMessage(L"ReplayGain headroom saved for this output.");
        return;
    }
    if (!g_controller.Apply(g_headset.id, changes)) ShowMessage(L"Device changed or is busy. Refresh before applying settings.");
    else { g_headset.busy = true; g.redraw = true; }
}
void Activate(int index) {
    if (!Enabled(index)) return;
    H.focus = index;
    if (index == 0) {
        g.settings.eq = g_profiles.Remember(!g_profiles.Remembered(), g.settings.eq);
        ApplyEq();
        SaveSettings();
    } else if (index == 1) {
        std::vector<std::string> names;
        for (int i = 0; i <= 10; ++i) names.push_back(i ? std::to_string(i * 10) + "%" : "Off");
        OpenChoice(H.controls[index].x, H.controls[index].B(), names, H.changes.sidetone,
            [](int value) { H.changes.sidetone = value; });
    } else if (index == 2) {
        std::vector<std::string> names{"Never"};
        for (int i = 15; i <= 60; ++i) names.push_back(std::to_string(i) + " minutes");
        int selected = H.changes.idleMinutes == 0 ? 0 : H.changes.idleMinutes - 14;
        OpenChoice(H.controls[index].x, H.controls[index].B(), names, selected,
            [](int value) { H.changes.idleMinutes = value == 0 ? 0 : value + 14; });
    } else if (index == 3) H.changes.dnd = !H.changes.dnd;
    else if (index == 4) {
        std::vector<std::string> names(std::begin(kPresetNames), std::end(kPresetNames));
        OpenChoice(H.controls[index].x, H.controls[index].B(), names, PresetIndex(H.changes.preset),
            [](int value) { H.changes.preset = kPresets[value]; });
    } else if (index == 5) {
        std::vector<std::string> names;
        for (int i = 0; i <= 48; ++i) names.push_back(std::to_string(i / 2) + (i % 2 ? ".5 dB" : " dB"));
        OpenChoice(H.controls[index].x, H.controls[index].B(), names, (int)std::lround(H.headroom * 2),
            [](int value) { H.headroom = value / 2.0f; });
    } else if (index == 6) g_controller.Refresh();
    else if (index == 7) Apply();
    else if (index == 8) OpenEqualizer();
    else ClosePanel();
    g.redraw = true;
}
}

void EnsureProfilesLoaded() {
    if (g_loaded) return;
    g_profiles.Load(std::filesystem::path(ToUtf8(g.dataDir)) / "output-profiles.ini", g.settings.eq);
    g_loaded = true;
}

// SelectOutput before Start: the first loop reads the selected node, so the lost wake does
// not matter. A second call does nothing once the poller is running.
void EnsureHeadsetPolling() {
    if (g_started) return;
    g_controller.SelectOutput(g.settings.outputDevice);
    g_started = true;
    g_controller.Start();
}

void StartHeadphones() {
    EnsureProfilesLoaded();
    // No saved profile means nothing to apply when the output changes, so the poller stays down
    // until the headphones page opens.
    if (g_profiles.Any()) EnsureHeadsetPolling();
}
void StopHeadphones() { g_controller.Stop(); }
void HeadphonesTick() {
    if (!g_started) return;
    g_controller.SelectOutput(g.settings.outputDevice);
    auto next = g_controller.Snapshot();
    if (next.id != g_headset.id) {
        g.settings.eq = g_profiles.Switch(ToUtf8(next.id), g.settings.eq);
        ApplyEq();
        // The last edit is captured before switching; only explicit edits opt an output into profiles.
        SaveHeadphoneProfiles();
    }
    if (next.id != g_headset.id || next.revision != g_headset.revision || next.busy != g_headset.busy ||
        next.status != g_headset.status) g.redraw = true;
    g_headset = std::move(next);
}
void SaveHeadphoneProfiles() {
    if (g_started && !g_profiles.Save(g.settings.eq)) ShowMessage(L"Couldn't save sound profiles; the previous file was kept.");
}
const EqSettings& HeadphonesGlobalEq() { return g_started ? g_profiles.Global() : g.settings.eq; }
float HeadphonesHeadroom() { return g_started ? g_profiles.Headroom() : 0; }
void PrepareHeadphonesPanel() {
    EnsureProfilesLoaded();
    EnsureHeadsetPolling();
    H = HeadphonesPanel{};
    SyncPanel();
    g_controller.Refresh();
}
void DrawHeadphones(const Palette& p) {
    SyncPanel();
    H.panel = DrawFrame(p, S(690), S(570), "Headphones");
    H.viewport = {H.panel.x + S(16), H.panel.y + S(46), H.panel.w - S(32), std::max(S(20), H.panel.h - S(100))};
    SDL_Rect oldClip{};
    bool clipped = SDL_RenderClipEnabled(g.renderer);
    SDL_GetRenderClipRect(g.renderer, &oldClip);
    SDL_Rect clip{(int)H.viewport.x, (int)H.viewport.y, (int)H.viewport.w, (int)H.viewport.h};
    SDL_SetRenderClipRect(g.renderer, &clip);
    float x = H.panel.x + S(20), y = H.panel.y + S(50) - H.scroll, width = H.panel.w - S(40);
    auto line = [&](const std::string& text, uint32_t color) {
        DrawText(Font::Normal, text, x, y, color, width);
        y += S(22);
    };
    line(g_headset.name.empty() ? "No active PipeWire output device." : "Output: " + ToUtf8(g_headset.name), p.text);
    line(g_headset.model.empty() ? "Per-output sound profiles are available for every output." : ToUtf8(g_headset.model), p.sub);
    H.controls[0] = {x, y + S(4), width, S(28)};
    DrawButton(H.controls[0], std::string(g_profiles.Remembered() ? "[x] " : "[ ] ") +
        "Remember cordinal EQ and crossfeed for this output", p, false, Enabled(0));
    y += S(40);
    line("Onboard EQ combines with cordinal EQ. Hardware changes affect every application.", p.sub);
    std::string status = g_headset.busy ? "Reading / applying hardware settings..." : ToUtf8(g_headset.status);
    if (status.empty()) status = "Refresh to check for supported hardware controls.";
    for (const auto& text : Wrap(Font::Normal, status, width)) line(text, p.sub);
    if (g_headset.battery >= 0) line("Battery: " + std::to_string(g_headset.battery) + "%" +
        (g_headset.charging == 1 ? " (charging)" : g_headset.charging == 0 ? " (on battery)" : ""), p.text);
    y = std::max(y, H.panel.y + S(230) - H.scroll);
    const std::string values[] = {
        "Mic sidetone: " + (H.changes.sidetone < 0 ? std::string("Unavailable") : std::to_string(H.changes.sidetone * 10) + "%"),
        "Auto-off: " + (H.changes.idleMinutes < 0 ? std::string("Unavailable") : H.changes.idleMinutes == 0 ? "Never" : std::to_string(H.changes.idleMinutes) + " minutes"),
        "Bluetooth do not disturb: " + std::string(H.changes.dnd < 0 ? "Unavailable" : H.changes.dnd ? "On" : "Off"),
        "Onboard EQ: " + std::string(kPresetNames[PresetIndex(H.changes.preset)]),
        "ReplayGain headset headroom: " + std::to_string(H.headroom) + " dB",
    };
    for (int i = 1; i <= 5; ++i) {
        H.controls[i] = {x, y, width, S(28)};
        DrawButton(H.controls[i], values[i - 1], p, false, Enabled(i));
        y += S(34);
    }
    if (g_headset.eqKnown) {
        line(g_headset.preset == 7 || g_headset.preset == 8 || g_headset.preset == 9 ?
            "Saved Custom curve (inactive; the factory curve isn't readable):" : "Stored curve (31 Hz through 16 kHz, dB):", p.sub);
        std::string bands;
        for (int band : g_headset.eqBands) bands += (bands.empty() ? "" : "   ") + std::to_string(band);
        line(bands, p.text);
    }
    H.maxScroll = std::max(0.0f, y + H.scroll - H.viewport.B());
    if (H.reveal && H.focus < 6) {
        const Rect& focus = H.controls[H.focus];
        if (focus.y < H.viewport.y) H.scroll -= H.viewport.y - focus.y;
        if (focus.B() > H.viewport.B()) H.scroll += focus.B() - H.viewport.B();
        H.scroll = std::clamp(H.scroll, 0.0f, H.maxScroll);
        H.reveal = false;
        g.redraw = true;
    }
    if (Enabled(H.focus) && H.focus < 6) Frame(H.controls[H.focus], p.accent);
    SDL_SetRenderClipRect(g.renderer, clipped ? &oldClip : nullptr);
    float bottom = H.panel.B() - S(45), gap = S(8), button = (width - 3 * gap) / 4;
    const char* labels[] = {"Refresh", "Apply / reapply", "cordinal EQ", "Close"};
    for (int i = 6; i < 10; ++i) {
        H.controls[i] = {x + (i - 6) * (button + gap), bottom, button, S(28)};
        DrawButton(H.controls[i], labels[i - 6], p, i == 9, Enabled(i));
    }
    if (Enabled(H.focus) && H.focus >= 6) Frame(H.controls[H.focus], p.accent);
}
void HeadphonesMouseDown(float x, float y) {
    if (!H.panel.In(x, y)) return ClosePanel();
    for (int i = 0; i < (int)H.controls.size(); ++i) if (H.controls[i].In(x, y) && (i >= 6 || H.viewport.In(x, y))) return Activate(i);
}
void HeadphonesWheel(float dy) {
    H.scroll = std::clamp(H.scroll - dy * S(50), 0.0f, H.maxScroll);
    g.redraw = true;
}
void HeadphonesKey(const SDL_KeyboardEvent& e) {
    if (e.key == SDLK_ESCAPE) return ClosePanel();
    if (e.key == SDLK_TAB) {
        for (int i = 0; i < 10; ++i) {
            H.focus = (H.focus + (e.mod & SDL_KMOD_SHIFT ? 9 : 1)) % 10;
            if (Enabled(H.focus)) break;
        }
        H.reveal = true;
    } else if (e.key == SDLK_RETURN || e.key == SDLK_SPACE) Activate(H.focus);
    else if (e.key == SDLK_PAGEDOWN) HeadphonesWheel(-3);
    else if (e.key == SDLK_PAGEUP) HeadphonesWheel(3);
    g.redraw = true;
}
int CheckHeadphonesPanel() {
    const auto savedPanel = H;
    const auto savedHeadset = g_headset;
    const float scale = g.scale;
    bool contained = true, keyboard = true;
    g_headset = HeadsetInfo{};
    g_headset.id = L"diagnostic-output";
    g_headset.name = L"Diagnostic headphones";
    g_headset.revision = 7;
    g_headset.hardware = true;
    g_headset.sidetone = 3;
    g_headset.idleMinutes = 30;
    g_headset.dnd = 0;
    g_headset.preset = 8;
    g_headset.battery = 75;
    g_headset.eqKnown = true;
    for (float dpi : {1.0f, 2.0f}) {
        g.scale = dpi;
        H = HeadphonesPanel{};
        SyncPanel();
        DrawHeadphones(CurrentPalette());
        for (int i = 6; i < 10; ++i) {
            const auto& rect = H.controls[i];
            contained &= rect.x >= H.panel.x && rect.R() <= H.panel.R() &&
                rect.y >= H.panel.y && rect.B() <= H.panel.B();
        }
        SDL_KeyboardEvent key{};
        key.key = SDLK_TAB;
        for (int i = 1; i <= 5; ++i) {
            HeadphonesKey(key);
            DrawHeadphones(CurrentPalette());
            DrawHeadphones(CurrentPalette());
            const auto& rect = H.controls[H.focus];
            keyboard &= H.focus == i && rect.y >= H.viewport.y - 1 && rect.B() <= H.viewport.B() + 1;
        }
    }
    H = savedPanel;
    g_headset = savedHeadset;
    g.scale = scale;
    printf("%s Headphones controls remain within the panel at 100 and 200 percent scaling\n", contained ? "ok  " : "FAIL");
    printf("%s Headphones Tab navigation reveals each editable setting in short windows\n", keyboard ? "ok  " : "FAIL");
    return !contained + !keyboard;
}

}
