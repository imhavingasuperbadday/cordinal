// The headphone page of the shared EQ window: recognition, per-output profiles and hardware controls.
// The hardware controller returns copies; only this UI thread reads or changes application state.
#include "app.h"

namespace app {
namespace {
HWND device, remember, status, refresh, apply, sidetone, idle, dnd, preset, sidetoneLabel, idleLabel, curve;
HWND headroom, headroomLabel;
constexpr int presets[] = {-1, 7, 8, 9, 255, 250, 254, 251, 253, 252};
constexpr const wchar_t* presetNames[] = {L"Unchanged", L"Game", L"Music", L"Movie", L"Custom (saved curve)",
    L"Game EQ 1 (stored)", L"Game EQ 2 (stored)", L"Game EQ 3 (stored)", L"Game EQ 4 (stored)", L"Game EQ 5 (stored)"};
int PresetValue(int selection) {
    return selection >= 0 && selection < (int)std::size(presets) ? presets[selection] : -1;
}
void LayoutHeadphones(HWND h) {
    auto move = [](HWND c, int x, int y, int w, int height) { MoveWindow(c, S(x), S(y), S(w), S(height), TRUE); };
    move(device, 16, 16, 658, 56);
    move(remember, 16, 78, 658, 28);
    move(GetDlgItem(h, IDC_EQ_HINT), 16, 112, 658, 50);
    move(status, 16, 168, 658, 70);
    move(sidetoneLabel, 16, 248, 150, 24);
    move(sidetone, 172, 244, 150, 300);
    move(idleLabel, 340, 248, 248, 24);
    move(idle, 596, 244, 78, 28);
    move(dnd, 16, 288, 370, 28);
    move(preset, 410, 284, 264, 240);
    move(headroomLabel, 16, 332, 570, 24);
    move(headroom, 596, 328, 78, 28);
    move(refresh, 16, 374, 130, 30);
    move(apply, 486, 374, 188, 30);
    move(curve, 16, 420, 658, 94);
}

void ApplyControls() {
    HeadsetChanges changes;
    if (g.headset.sidetone >= 0) {
        int selected = (int)SendMessageW(sidetone, CB_GETCURSEL, 0, 0);
        if (selected != g.headset.sidetone) changes.sidetone = selected;
    }
    if (g.headset.idleMinutes >= 0) {
        wchar_t text[16];
        GetWindowTextW(idle, text, (int)std::size(text));
        wchar_t* end = nullptr;
        long minutes = wcstol(text, &end, 10);
        if (!*text || *end || (minutes != 0 && (minutes < 15 || minutes > 60))) {
            SetWindowTextW(status, L"Auto-off must be 0 (never), or 15 to 60 minutes.");
            return;
        }
        if (minutes != g.headset.idleMinutes) changes.idleMinutes = (int)minutes;
    }
    if (g.headset.dnd >= 0) {
        int desired = Button_GetCheck(dnd) == BST_CHECKED;
        if (desired != g.headset.dnd) changes.dnd = desired;
    }
    int desired = PresetValue((int)SendMessageW(preset, CB_GETCURSEL, 0, 0));
    // Reapplying the selected preset also relatches its saved curve into the live DSP.
    // This matters after another application has edited the headset's custom slot.
    if (desired >= 0 && g.headset.preset >= 0) changes.preset = desired;
    wchar_t text[32];
    GetWindowTextW(headroom, text, (int)std::size(text));
    wchar_t* end = nullptr;
    float db = (float)wcstod(text, &end);
    if (!*text || *end || !std::isfinite(db) || db < 0 || db > 24) {
        SetWindowTextW(status, L"ReplayGain headset headroom must be 0 to 24 dB.");
        return;
    }
    const bool gainChanged = db != g.rgHeadphoneHeadroom;
    if (gainChanged) SaveOutputReplayGainHeadroom(db);
    if (changes.sidetone < 0 && changes.idleMinutes < 0 && changes.dnd < 0 && changes.preset < 0) {
        SetWindowTextW(status, gainChanged ? L"ReplayGain headroom saved for this output." : L"No hardware settings changed.");
        return;
    }
    if (g.headsets.Apply(g.headset.id, changes)) {
        g.headset.busy = true;
        HeadphonesSync(false);
    } else SetWindowTextW(status, L"Device changed or is busy. Refresh before applying settings.");
}
}

void HeadphonesTick() {
    HeadsetInfo next = g.headsets.Snapshot();
    bool changed = next.id != g.headset.id || next.id != g.eqOutputId;
    bool values = changed || next.revision != g.headset.revision;
    bool update = values || next.name != g.headset.name || next.busy != g.headset.busy || next.status != g.headset.status;
    g.headset = std::move(next);
    if (changed) { SyncOutputEq(g.headset.id); EqDeviceSync(true); }
    if (changed && g.headphonesWnd && !g.headset.id.empty()) g.headsets.Refresh();
    if (g.headphonesWnd && update) HeadphonesSync(values);
}

void HeadphonesSync(bool values) {
    if (!g.headphonesWnd) return;
    const auto& info = g.headset;
    std::wstring text = info.name.empty() ? L"No active Windows output device." : L"Output: " + info.name;
    if (!info.model.empty()) text += L"\nRecognized: " + info.model;
    else if (!info.name.empty()) text += L"\nModel not identified; per-output sound profiles are still available.";
    SetWindowTextW(device, text.c_str());
    Button_SetCheck(remember, g.rememberOutputEq ? BST_CHECKED : BST_UNCHECKED);
    EnableWindow(remember, !info.id.empty());
    text = info.busy ? L"Reading / applying hardware settings..." : info.status;
    if (!info.busy && info.battery >= 0) {
        text += L"\nBattery: " + std::to_wstring(info.battery) + L"%";
        if (info.charging >= 0) text += info.charging ? L" (charging)" : L" (on battery)";
    }
    if (text.empty()) text = L"Refresh to check for supported hardware controls.";
    SetWindowTextW(status, text.c_str());
    text = L"Active onboard EQ: ";
    const wchar_t* active = L"Unknown";
    for (size_t i = 1; i < std::size(presets); ++i) if (presets[i] == info.preset) active = presetNames[i];
    text += active;
    if (info.eqKnown) {
        constexpr const wchar_t* frequencies[] = {L"31", L"63", L"125", L"250", L"500", L"1k", L"2k", L"4k", L"8k", L"16k"};
        text += info.preset == 7 || info.preset == 8 || info.preset == 9 ?
            L"\nSaved Custom curve (inactive; factory curve isn't readable):" :
            L"\nStored curve read from headset (Hz: dB):";
        for (size_t i = 0; i < info.eqBands.size(); ++i) {
            wchar_t band[32];
            swprintf_s(band, L"%ls: %+d", frequencies[i], (int)info.eqBands[i]);
            text += i % 5 == 0 ? L"\n" : L"    ";
            text += band;
        }
    } else text += L"\nOnboard curve unavailable. Refresh to read the headset.";
    SetWindowTextW(curve, text.c_str());
    EnableWindow(refresh, !info.busy);
    EnableWindow(apply, !info.id.empty() && !info.busy);
    EnableWindow(headroom, !info.id.empty() && !info.busy);
    EnableWindow(sidetone, !info.busy && info.sidetone >= 0);
    EnableWindow(idle, !info.busy && info.idleMinutes >= 0);
    EnableWindow(dnd, !info.busy && info.dnd >= 0);
    EnableWindow(preset, !info.busy && info.preset >= 0);
    if (values) {
        SetWindowTextW(headroom, F2S(g.rgHeadphoneHeadroom).c_str());
        SendMessageW(sidetone, CB_SETCURSEL, info.sidetone, 0);
        SetWindowTextW(idle, info.idleMinutes < 0 ? L"" : std::to_wstring(info.idleMinutes).c_str());
        Button_SetCheck(dnd, info.dnd == 1 ? BST_CHECKED : BST_UNCHECKED);
        int selection = 0;
        for (int i = 1; i < (int)std::size(presets); ++i) if (PresetValue(i) == info.preset) selection = i;
        SendMessageW(preset, CB_SETCURSEL, selection, 0);
    }
}

LRESULT CALLBACK HeadphonesProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
                HWND control = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0,
                    h, (HMENU)(INT_PTR)id, g.inst, nullptr);
                SendMessageW(control, WM_SETFONT, (WPARAM)g.font, TRUE);
                ApplyControlTheme(control, g.dark);
                if (wcscmp(cls, WC_COMBOBOXW) == 0)
                    SetWindowTheme(control, g.dark ? L"DarkMode_CFD" : L"Explorer", nullptr);
                return control;
            };
            device = make(WC_STATICW, L"", SS_LEFT, IDC_EQ_DEVICE);
            remember = make(WC_BUTTONW, L"Remember cordinal EQ and crossfeed for this output", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_EQ_REMEMBER);
            make(WC_STATICW, L"Uncheck to use global EQ. Onboard EQ combines with cordinal EQ.\n"
                L"Reserve ReplayGain headroom below for the headset's own boosts.", SS_LEFT, IDC_EQ_HINT);
            status = make(WC_STATICW, L"", SS_LEFT, IDC_EQ_HW_STATUS);
            sidetoneLabel = make(WC_STATICW, L"Mic sidetone", SS_LEFT, IDC_EQ_SIDETONE_LABEL);
            sidetone = make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, IDC_EQ_SIDETONE);
            for (int i = 0; i <= 10; ++i) {
                std::wstring label = i ? std::to_wstring(i * 10) + L"%" : L"Off";
                SendMessageW(sidetone, CB_ADDSTRING, 0, (LPARAM)label.c_str());
            }
            idleLabel = make(WC_STATICW, L"Auto-off min (0 = never)", SS_LEFT, IDC_EQ_IDLE_LABEL);
            idle = make(WC_EDITW, L"", WS_BORDER | ES_NUMBER | WS_TABSTOP, IDC_EQ_IDLE);
            SendMessageW(idle, EM_SETLIMITTEXT, 3, 0);
            dnd = make(WC_BUTTONW, L"Bluetooth do not disturb", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_EQ_DND);
            preset = make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, IDC_EQ_HW_PRESET);
            for (const wchar_t* name : presetNames) SendMessageW(preset, CB_ADDSTRING, 0, (LPARAM)name);
            refresh = make(WC_BUTTONW, L"Refresh", BS_PUSHBUTTON | WS_TABSTOP, IDC_EQ_HW_REFRESH);
            apply = make(WC_BUTTONW, L"Apply / reapply", BS_PUSHBUTTON | WS_TABSTOP, IDC_EQ_HW_APPLY);
            curve = make(WC_STATICW, L"", SS_LEFT, IDC_EQ_HW_CURVE);
            headroomLabel = make(WC_STATICW, L"ReplayGain headset headroom (0-24 dB)", SS_LEFT, IDC_EQ_RG_HEADROOM_LABEL);
            headroom = make(WC_EDITW, L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, IDC_EQ_RG_HEADROOM);
            SendMessageW(headroom, EM_SETLIMITTEXT, 8, 0);
            ApplyWindowTheme(h, P());
            return 0;
        }
        case WM_SIZE: LayoutHeadphones(h); return 0;
        case WM_THEMECHANGED:
            ApplyWindowTheme(h, P());
            EnumChildWindows(h, [](HWND control, LPARAM) -> BOOL {
                SendMessageW(control, WM_SETFONT, (WPARAM)g.font, TRUE);
                ApplyControlTheme(control, g.dark);
                wchar_t cls[32];
                GetClassNameW(control, cls, (int)std::size(cls));
                if (wcscmp(cls, WC_COMBOBOXW) == 0)
                    SetWindowTheme(control, g.dark ? L"DarkMode_CFD" : L"Explorer", nullptr);
                return TRUE;
            }, 0);
            InvalidateRect(h, nullptr, TRUE);
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDC_EQ_REMEMBER:
                    RememberOutputEq(Button_GetCheck(remember) == BST_CHECKED);
                    EqDeviceSync(true);
                    HeadphonesSync(false);
                    break;
                case IDC_EQ_HW_REFRESH: g.headsets.Refresh(); break;
                case IDC_EQ_HW_APPLY: ApplyControls(); break;
                case IDCANCEL:
                    if (GetWindowLongPtrW(h, GWL_STYLE) & WS_CHILD) SendMessageW(GetParent(h), WM_CLOSE, 0, 0);
                    else DestroyWindow(h);
                    break;
            }
            return 0;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            SetTextColor((HDC)wp, TextColor());
            SetBkColor((HDC)wp, SurfaceColor());
            return (LRESULT)g.surfaceBrush;
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: return FieldControlColor((HDC)wp);
        case WM_ERASEBKGND: {
            RECT rc; GetClientRect(h, &rc);
            FillRect((HDC)wp, &rc, g.surfaceBrush);
            return 1;
        }
        case WM_DESTROY: g.headphonesWnd = nullptr; return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void ShowHeadphones() {
    ShowEqualizer();
    SelectEqPage(true);
}
}
