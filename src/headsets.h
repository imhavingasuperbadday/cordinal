// Output recognition and optional headset hardware control. The worker owns COM and HID handles;
// callers exchange copied snapshots and explicit commands, never live application state.
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

struct HeadsetInfo {
    uint64_t revision = 0;
    std::wstring id, name, model, status;
    bool hardware = false, busy = false;
    int battery = -1, charging = -1, sidetone = -1, idleMinutes = -1, dnd = -1, preset = -1;
    bool eqKnown = false;
    // Query 0x15 returns Custom for all classic presets (7/8/9/255); game EQs return their own slot.
    std::array<int8_t, 10> eqBands{};  // signed dB, 31 Hz through 16 kHz
};
struct HeadsetChanges {
    int sidetone = -1, idleMinutes = -1, dnd = -1, preset = -1;  // -1 leaves a setting alone
};
std::wstring RecognizeHeadphones(std::wstring_view name);
std::wstring OutputProfileSection(std::wstring_view id);

// BlackShark V2 Pro 2.4 (1532:0555), interface 3, usage FF00:0001 only.
std::array<uint8_t, 64> BlackSharkReport(uint8_t type, uint8_t command, int value = -1);
bool BlackSharkReply(const uint8_t* report, size_t size, uint8_t command, uint8_t& value);
bool BlackSharkReply(const uint8_t* report, size_t size, uint8_t command, uint8_t* values, size_t count);
bool ValidHeadsetChanges(const HeadsetChanges& changes);

class HeadsetController {
public:
    HeadsetController();
    ~HeadsetController();
    void Start();
    void Stop();
    void Refresh();
    HeadsetInfo Snapshot() const;
    bool Apply(const std::wstring& outputId, const HeadsetChanges& changes);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
