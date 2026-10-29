// Model recognition, isolated device profiles and BlackShark wire validation. Live hardware tests
// are opt-in: preset switching is followed by restoration, and no stored EQ bands are overwritten.
#include "selftest_common.h"

void TestHeadsets() {
    Check(RecognizeHeadphones(L"Speakers (RAZER BlackShark V2 Pro 2.4)") == L"Razer BlackShark V2 Pro" &&
          RecognizeHeadphones(L"Headphones (WH-1000XM5)") == L"Sony WH-1000XM5" &&
          RecognizeHeadphones(L"Headphones (Arctis Nova 7X)") == L"SteelSeries Arctis Nova 7 family" &&
          RecognizeHeadphones(L"HyperX Cloud III Wireless") == L"HyperX Cloud III family",
          L"headphone recognition tolerates case, punctuation and connection suffixes without confusing generations");
    Check(RecognizeHeadphones(L"Speakers (USB Audio Device)").empty() &&
          RecognizeHeadphones(L"THX Spatial Audio").empty() && RecognizeHeadphones(L"Razer Basilisk V3").empty(),
          L"generic outputs, virtual devices and other Razer hardware do not acquire a guessed headset model");
    Check(OutputProfileSection(L"endpoint-a") == OutputProfileSection(L"endpoint-a") &&
          OutputProfileSection(L"endpoint-a") != OutputProfileSection(L"endpoint-b"),
          L"output profile keys are stable and keep distinct endpoints separate");
    auto report = BlackSharkReport(3, 0x21);
    Check(report[0] == 2 && report[1] == 0x80 && report[2] == 8 && report[5] == 'P' && report[6] == 'A' &&
          report[7] == 8 && report[9] == 3 && report[10] == 0x21 && report[12] == 0,
          L"BlackShark battery query has the correct 64-byte MXIC envelope");
    report = BlackSharkReport(2, 0xe1, 1);
    Check(report[2] == 7 && report[7] == 0x0e && report[11] == 1 && report[12] == 0,
          L"remote handshake uses its distinct flag layout");
    report = BlackSharkReport(4, 0x99, 5);
    Check(report[2] == 9 && report[12] == 1 && report[13] == 5,
          L"hardware setting commands put values in the payload, not the flag");
    report = {};
    report[0] = 2; report[12] = 0x21; report[13] = 1; report[14] = 1; report[15] = 72;
    uint8_t value = 0;
    bool valid = BlackSharkReply(report.data(), report.size(), 0x21, value) && value == 72;
    bool rejected = !BlackSharkReply(report.data(), 15, 0x21, value) &&
                    !BlackSharkReply(report.data(), report.size(), 0x18, value);
    report[13] = 0;
    rejected &= !BlackSharkReply(report.data(), report.size(), 0x21, value);
    report[13] = 1; report[14] = 63;
    rejected &= !BlackSharkReply(report.data(), report.size(), 0x21, value);
    Check(valid && rejected, L"only complete matching ACK replies are accepted; telemetry and truncated payloads are ignored");
    report = {};
    report[0] = 2; report[12] = 0x15; report[13] = 1; report[14] = 10;
    for (int i = 0; i < 10; ++i) report[15 + i] = (uint8_t)(i - 5);
    std::array<uint8_t, 10> bands{};
    bool curve = BlackSharkReply(report.data(), report.size(), 0x15, bands.data(), bands.size()) &&
        (int8_t)bands[0] == -5 && bands[9] == 4;
    const auto originalBands = bands;
    report[14] = 9;
    curve &= !BlackSharkReply(report.data(), report.size(), 0x15, bands.data(), bands.size()) && bands == originalBands;
    report[14] = 10;
    curve &= !BlackSharkReply(report.data(), 24, 0x15, bands.data(), bands.size()) && bands == originalBands;
    Check(curve, L"hardware EQ requires all ten signed bands; short replies cannot replace a complete curve");
    HeadsetChanges changes;
    changes.sidetone = 10; changes.idleMinutes = 15; changes.dnd = 1; changes.preset = 255;
    bool ranges = ValidHeadsetChanges(changes);
    changes.sidetone = 11; ranges &= !ValidHeadsetChanges(changes);
    changes.sidetone = 0; changes.idleMinutes = 1; ranges &= !ValidHeadsetChanges(changes);
    changes.idleMinutes = 0; changes.preset = 0; ranges &= !ValidHeadsetChanges(changes);
    for (int preset = 250; preset <= 254; ++preset) {
        changes.preset = preset; ranges &= ValidHeadsetChanges(changes);
    }
    Check(ranges, L"hardware controls reject invalid sidetone, idle-time and preset values before any I/O");
    HeadsetController inactive;
    Check(!inactive.Apply(L"unrecognized-output", {}), L"hardware writes require a discovered matching output");

    wchar_t live[4];
    if (HeadlessTests() || GetEnvironmentVariableW(L"CORDINAL_TEST_HEADSET", live, (DWORD)std::size(live)) != 1 || live[0] != L'1') {
        testlog::Skip(L"live headset I/O: requires a device, CORDINAL_TEST_HEADSET=1 and a run without --headless");
        return;
    }
    HeadsetController controller;
    controller.Start();
    controller.Refresh();
    auto finish = [&] {
        HeadsetInfo info;
        ULONGLONG deadline = GetTickCount64() + 10000;
        do {
            Sleep(20);
            info = controller.Snapshot();
        } while (GetTickCount64() < deadline && (info.busy || info.status.empty()));
        return info;
    };
    auto info = finish();
    fwprintf(g_log, L"LIVE headset: %ls, battery %d%%, sidetone %d, auto-off %d min, DND %d, preset %d\n",
        info.name.c_str(), info.battery, info.sidetone, info.idleMinutes, info.dnd, info.preset);
    Check(info.hardware && info.battery >= 0 && !info.busy, L"LIVE headset read: %ls, battery %d%%; %ls",
          info.name.c_str(), info.battery, info.status.c_str());
    if (info.hardware && info.battery >= 0 && !info.busy) {
        auto logCurve = [&](const HeadsetInfo& read) {
            fwprintf(g_log, L"LIVE EQ preset %d, stored slot %ls:", read.preset, read.eqKnown ? L"read" : L"unavailable");
            for (auto band : read.eqBands) fwprintf(g_log, L" %+d", (int)band);
            fwprintf(g_log, L"; %ls\n", read.status.c_str());
        };
        logCurve(info);
        HeadsetChanges same;
        same.sidetone = info.sidetone;
        same.idleMinutes = info.idleMinutes;
        same.dnd = info.dnd;
        // Preset application is also tested without changing the selected curve or custom bands.
        if (info.preset == 7 || info.preset == 8 || info.preset == 9 || info.preset == 255) same.preset = info.preset;
        bool accepted = controller.Apply(info.id, same);
        Check(!controller.Apply(info.id, same), L"a second hardware command cannot interleave with an in-flight change");
        auto after = finish();
        Check(accepted && after.status == L"Hardware settings applied and read back from the headset." &&
              after.sidetone == info.sidetone && after.idleMinutes == info.idleMinutes && after.dnd == info.dnd &&
              (same.preset < 0 || after.preset == info.preset),
              L"LIVE unchanged settings read back: sidetone %d, auto-off %d, DND %d, preset %d; %ls",
              after.sidetone, after.idleMinutes, after.dnd, after.preset, after.status.c_str());
        if (info.eqKnown && ValidHeadsetChanges(HeadsetChanges{-1, -1, -1, info.preset}) && info.preset >= 0) {
            bool switched = true, distinct = false;
            auto previous = info.eqBands;
            for (int target : {7, 8, 9, 250}) {
                HeadsetChanges select;
                select.preset = target;
                bool switchAccepted = controller.Apply(info.id, select);
                auto selected = finish();
                logCurve(selected);
                switched &= switchAccepted && selected.preset == target && selected.eqKnown &&
                    selected.status == L"Hardware settings applied and read back from the headset.";
                if (selected.eqKnown) { distinct |= selected.eqBands != previous; previous = selected.eqBands; }
                if (!switched) break;
            }
            // Restore even after a failed intermediate read. Selector-only writes never change
            // the saved custom/esports curves, including the user's original custom slot.
            HeadsetChanges restore;
            restore.preset = info.preset;
            HeadsetInfo restored;
            bool restoredOk = false;
            for (int attempt = 0; attempt < 2 && !restoredOk; ++attempt) {
                bool restoreAccepted = controller.Apply(info.id, restore);
                restored = finish();
                restoredOk = restoreAccepted && restored.preset == info.preset && restored.eqKnown && restored.eqBands == info.eqBands;
            }
            logCurve(restored);
            Check(switched && distinct, L"LIVE presets switch within/across families and Custom/game EQ slots have distinct readbacks");
            Check(restoredOk, L"LIVE original preset %d and all ten original EQ bands restored", info.preset);
        } else Check(false, L"LIVE hardware EQ curve must be readable before a reversible preset-switch test");
    }
    controller.Stop();
}
