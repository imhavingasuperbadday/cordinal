// Linux Headphones regressions without a physical device: exact HID identity, wire guards,
// isolated per-output EQ/headroom and atomic persistence in a throwaway profile.
#ifndef _WIN32
#include "../linux/headphones_ui.h"
#include "../linux/headsets_native.h"
#include "../linux/output_profiles.h"
#include "library.h"

#include <filesystem>
#include <fstream>
#include <limits>
#include <unistd.h>

namespace lx {
namespace {
bool Write(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary);
    out << bytes;
    out.close();
    return !out.fail();
}
std::string Read(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {(std::istreambuf_iterator<char>(in)), {}};
}
}
int CheckLinuxHeadsets() {
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        printf("%s %s\n", ok ? "ok  " : "FAIL", label);
        failures += !ok;
    };
    check(RecognizeHeadphones(L"Headphones (RAZER BlackShark V2 Pro 2.4)") == L"Razer BlackShark V2 Pro" &&
        RecognizeHeadphones(L"WH-1000XM5") == L"Sony WH-1000XM5" &&
        RecognizeHeadphones(L"USB Audio Device").empty(), "Linux recognizes model names without guessing generic outputs");
    auto query = BlackSharkReport(3, 0x21);
    auto set = BlackSharkReport(4, 0x99, 5);
    auto remote = BlackSharkReport(2, 0xe1, 1);
    check(query[0] == 2 && query[2] == 8 && query[5] == 'P' && query[6] == 'A' && query[10] == 0x21 &&
        set[2] == 9 && set[12] == 1 && set[13] == 5 && remote[7] == 0x0e && remote[11] == 1,
        "Linux HID commands share the exact query, setting and remote envelope with Windows");
    std::array<uint8_t, 64> reply{};
    reply[0] = 2; reply[12] = 0x15; reply[13] = 1; reply[14] = 10;
    for (int i = 0; i < 10; ++i) reply[15 + i] = (uint8_t)(i - 5);
    std::array<uint8_t, 10> bands{};
    bool complete = BlackSharkReply(reply.data(), reply.size(), 0x15, bands.data(), bands.size()) && (int8_t)bands[0] == -5;
    const auto old = bands;
    reply[14] = 9;
    check(complete && !BlackSharkReply(reply.data(), reply.size(), 0x15, bands.data(), bands.size()) && bands == old &&
        !BlackSharkReply(reply.data(), 15, 0x15, bands.data(), bands.size()),
        "short or mismatched HID replies cannot replace the complete stored EQ curve");
    const uint8_t descriptor[] = {0x06,0x00,0xff,0x09,0x01,0xa1,0x01,0x85,0x02,0x75,0x08,0x95,0x3f,0x81,0x02,0x91,0x02,0xc0};
    auto changed = std::vector<uint8_t>(std::begin(descriptor), std::end(descriptor));
    bool valid = BlackSharkDescriptor(descriptor);
    changed[8] = 1;
    bool wrongReport = !BlackSharkDescriptor(changed);
    changed.assign(std::begin(descriptor), std::end(descriptor));
    changed[12] = 64;
    check(valid && wrongReport && !BlackSharkDescriptor(changed) &&
        !BlackSharkDescriptor(std::span(descriptor).first(12)),
        "hidraw matching requires the vendor collection and exact numbered 64-byte input/output reports");
    HeadsetController inactive;
    check(ValidHeadsetChanges({10,15,1,255}) && !ValidHeadsetChanges({11,15,1,255}) &&
        !ValidHeadsetChanges({0,1,1,255}) && !ValidHeadsetChanges({0,15,1,0}) &&
        !inactive.Apply(L"unselected-output", {}), "headset writes require valid ranges and a discovered idle matching output");
    char temporary[] = "/tmp/cordinal-headsets-XXXXXX";
    if (!mkdtemp(temporary)) { check(false, "headphone checks create an isolated profile"); return failures; }
    namespace fs = std::filesystem;
    const fs::path root(temporary), usb = root / "usb1/1-2", iface = usb / "1-2:1.3";
    std::error_code error;
    fs::create_directories(iface / "hid/device", error);
    fs::create_directories(usb / "1-2:1.0/sound/card7", error);
    fs::create_directories(root / "usb1/1-3/audio", error);
    bool ready = !error && Write(usb / "idVendor", "1532\n") && Write(usb / "idProduct", "0555\n") &&
        Write(iface / "bInterfaceNumber", "03\n") && Write(root / "usb1/1-3/idVendor", "1532\n") &&
        Write(root / "usb1/1-3/idProduct", "0555\n");
    bool sameUsb = MatchingHeadsetUsb(iface / "hid/device", usb / "1-2:1.0/sound/card7");
    bool otherUsb = !MatchingHeadsetUsb(iface / "hid/device", root / "usb1/1-3/audio");
    Write(iface / "bInterfaceNumber", "02\n");
    check(ready && sameUsb && otherUsb && !MatchingHeadsetUsb(iface / "hid/device", usb / "1-2:1.0/sound/card7"),
        "headset HID and audio must share one exact USB parent and interface 03");

    EqSettings global;
    global.enabled = true; global.preamp = -2; global.gains[1] = 3;
    OutputProfiles profiles;
    const auto path = root / "output-profiles.ini";
    profiles.Load(path, global);
    EqSettings active = profiles.Switch("sink-a", global);
    active = profiles.Remember(true, active);
    active.parametric = true; active.crossfeed = true; active.filterCount = 1;
    active.filters[0] = {EqFilterType::LowShelf, 120, 4, 0.8f, true};
    active.gains[3] = -3;
    bool saved = profiles.SetHeadroom(6) && profiles.Save(active);
    const auto device = active;
    active = profiles.Switch("sink-b", active);
    bool isolated = active == global && !profiles.Remembered() && profiles.Headroom() == 0;
    active.preamp = -5;
    active = profiles.Switch("sink-a", active);
    bool restored = active == device && profiles.Headroom() == 6 && profiles.Global().preamp == -5;
    active = profiles.Remember(false, active);
    check(saved && isolated && restored && active.preamp == -5 && !profiles.Remembered(),
        "device EQ and crossfeed stay isolated while unremembered outputs share the global EQ");
    active = profiles.Remember(true, device);
    bool roundtrip = profiles.Save(active);
    OutputProfiles loaded;
    loaded.Load(path, profiles.Global());
    auto reloaded = loaded.Switch("sink-a", loaded.Global());
    check(roundtrip && reloaded == device && loaded.Remembered() && loaded.Headroom() == 6 &&
        !loaded.SetHeadroom(std::numeric_limits<float>::quiet_NaN()) && !loaded.SetHeadroom(25),
        "atomic sound profiles round-trip graphic/parametric EQ, crossfeed and independent validated headroom");
    const auto before = Read(path);
    // A non-directory parent makes staging fail; it must not alter any pre-existing bytes.
    OutputProfiles denied;
    denied.Load(path / "unwritable", global);
    denied.Switch("sink", global);
    denied.Remember(true, global);
    check(!denied.Save(global) && Read(path) == before,
        "failed profile staging preserves the original profile bytes");
    OutputProfiles names;
    names.Load(root / "names.ini", global);
    const std::string name = "sink-],:\nUnicode-\xc3\xa9";
    names.Switch(name, global);
    names.Remember(true, device);
    bool escaped = names.Save(device);
    names.Load(root / "names.ini", global);
    check(escaped && names.Switch(name, global) == device && names.Remembered(),
        "full output names cannot collide or escape the profile file format");
    fs::remove_all(root, error);
    check(!error, "headphone checks remove their temporary profile");
    return failures + CheckHeadphonesPanel();
}
}
#endif
