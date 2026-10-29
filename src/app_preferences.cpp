// Patient startup-default suggestions: collect settled presentation choices, then offer one
// modeless checklist. The review owns a snapshot so later edits cannot change what a check saves.
#include "app.h"

namespace app {
namespace {

constexpr unsigned kChoiceCount = 5;
constexpr ULONGLONG kSettleMs = 5 * 60 * 1000, kReviewInterval = 24 * 60 * 60;
StartupPreferences observed;
ULONGLONG changedAt = 0, lastReview = 0;
bool reviewed = false, trackingReady = false;
struct Review {
    HWND hwnd = nullptr, heading = nullptr, body = nullptr, footer = nullptr, save = nullptr, later = nullptr;
    HWND checks[kChoiceCount]{};
    HWND automaticChoice = nullptr, askingChoice = nullptr;
    StartupPreferences choices;
    unsigned mask = 0;
    UINT dpi = 96;
    HFONT font = nullptr;
    const Palette* palette = nullptr;
    bool policy = false, automatic = false;
} review;

ULONGLONG WallSeconds() {
    FILETIME time;
    GetSystemTimeAsFileTime(&time);
    return (((ULONGLONG)time.dwHighDateTime << 32) | time.dwLowDateTime) / 10000000;
}

unsigned Difference(const StartupPreferences& a, const StartupPreferences& b) {
    return ((a.mini != b.mini || (!a.mini && a.size != b.size)) ? kDefaultWindowSize : 0) |
           (a.theme != b.theme ? kDefaultTheme : 0) | (a.library != b.library ? kDefaultLibraryView : 0) |
           (a.browser != b.browser ? kDefaultBrowser : 0) | (a.layout != b.layout ? kDefaultLayout : 0);
}

int D(int px) { return MulDiv(px, review.dpi, 96); }

std::wstring ChoiceText(unsigned index) {
    const auto& p = review.choices;
    static const wchar_t* sizes[] = {L"Compact", L"Normal", L"Big mode plus", L"Big mode"};
    static const wchar_t* views[] = {L"Tracks", L"Albums and tracks", L"Albums", L"Artists"};
    static const wchar_t* docks[] = {L"top", L"bottom", L"left", L"right"};
    switch (index) {
        case 0: return L"Window size: " + std::wstring(p.mini ? L"Circular mini player" : sizes[(int)p.size]);
        case 1: return L"Theme: " + std::wstring(ThemeInfo(p.theme).name);
        case 2: return L"Library view: " + std::wstring(views[(int)p.library]);
        case 3: return p.browser ? L"Library browser: shown" : L"Library browser: hidden";
        default: return L"Player layout and controls: " + std::wstring(docks[(int)p.layout.player]) + L" dock";
    }
}

unsigned Selection() {
    unsigned result = 0;
    for (unsigned i = 0; i < kChoiceCount; ++i)
        if (review.checks[i] && Button_GetCheck(review.checks[i]) == BST_CHECKED) result |= 1u << i;
    return result;
}

void ThemeReview() {
    if (!review.hwnd || review.palette == g.pal) return;
    review.palette = g.pal;
    ApplyWindowTheme(review.hwnd, P());
    for (HWND check : review.checks) if (check) ApplyControlTheme(check, g.dark);
    for (HWND button : {review.save, review.later, review.automaticChoice, review.askingChoice})
        if (button) ApplyControlTheme(button, g.dark);
    RedrawWindow(review.hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

void LayoutReview() {
    RECT r;
    GetClientRect(review.hwnd, &r);
    int pad = D(18), width = std::max(0, (int)r.right - pad * 2), y = D(96);
    MoveWindow(review.heading, pad, D(16), width, D(26), TRUE);
    MoveWindow(review.body, pad, D(46), width, D(42), TRUE);
    for (HWND check : review.checks) if (check) {
        MoveWindow(check, pad, y, width, D(28), TRUE);
        y += D(30);
    }
    for (HWND radio : {review.automaticChoice, review.askingChoice}) if (radio) {
        MoveWindow(radio, pad, y, width, D(28), TRUE);
        y += D(30);
    }
    MoveWindow(review.footer, pad, y + D(8), width, D(34), TRUE);
    int bottom = r.bottom - D(46);
    MoveWindow(review.save, r.right - pad - D(226), bottom, D(132), D(28), TRUE);
    MoveWindow(review.later, r.right - pad - D(86), bottom, D(86), D(28), TRUE);
}

void ShowSavingQuestion() {
    review.policy = true;
    for (HWND& check : review.checks) { if (check) DestroyWindow(check); check = nullptr; }
    SetWindowTextW(review.heading, L"How should I remember future choices?");
    SetWindowTextW(review.body,
        L"Save future presentation changes automatically,\nor keep asking which choices to keep?");
    SetWindowTextW(review.footer, L"Future changes only. Change this anytime in Preferences > Startup.");
    auto radio = [&](const wchar_t* text, int id, DWORD style) {
        HWND c = CreateWindowExW(0, WC_BUTTONW, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON | style,
                                0, 0, 0, 0, review.hwnd, (HMENU)(INT_PTR)id, g.inst, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)review.font, TRUE);
        ApplyControlTheme(c, g.dark);
        return c;
    };
    review.automaticChoice = radio(L"Save future preferences &automatically", 105, WS_GROUP);
    review.askingChoice = radio(L"Continue &asking me", 106, 0);
    Button_SetCheck(review.askingChoice, BST_CHECKED);
    SetWindowTextW(review.save, L"&Use this choice");
    SetWindowTextW(review.later, L"&Not now");
    EnableWindow(review.save, TRUE);
    RECT r{0, 0, D(440), D(252)};
    AdjustWindowRectExForDpi(&r, (DWORD)GetWindowLongPtrW(review.hwnd, GWL_STYLE), FALSE,
                             (DWORD)GetWindowLongPtrW(review.hwnd, GWL_EXSTYLE), review.dpi);
    SetWindowPos(review.hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    SetFocus(review.askingChoice);
    InvalidateRect(review.hwnd, nullptr, TRUE);
}

LRESULT CALLBACK ReviewProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            review.hwnd = h;
            review.dpi = GetDpiForWindow(h);
            LOGFONTW font{};
            GetObjectW(g.font, sizeof(font), &font);
            review.font = CreateFontIndirectW(&font);
            auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
                HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0,
                                         h, (HMENU)(INT_PTR)id, g.inst, nullptr);
                SendMessageW(c, WM_SETFONT, (WPARAM)review.font, TRUE);
                if (!wcscmp(cls, WC_BUTTONW)) ApplyControlTheme(c, g.dark);
                return c;
            };
            review.heading = make(WC_STATICW, L"Keep these for next time?", SS_LEFT | SS_NOPREFIX, 0);
            review.body = make(WC_STATICW, review.mask ?
                L"You've made cordinal your own. Pick the choices you'd like to use when it opens." :
                L"Your current choices already match your saved startup defaults.", SS_LEFT | SS_NOPREFIX, 0);
            for (unsigned i = 0; i < kChoiceCount; ++i) if (review.mask & (1u << i))
                review.checks[i] = make(WC_BUTTONW, ChoiceText(i).c_str(), BS_AUTOCHECKBOX | WS_TABSTOP, 100 + i);
            review.footer = make(WC_STATICW, L"Only checked choices are saved. Your current session stays as it is.",
                                 SS_LEFT | SS_NOPREFIX, 0);
            review.save = make(WC_BUTTONW, L"Save &selected", BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK);
            review.later = make(WC_BUTTONW, L"&Later", BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL);
            EnableWindow(review.save, FALSE);
            ThemeReview();
            return 0;
        }
        case WM_SIZE: LayoutReview(); return 0;
        case WM_ERASEBKGND: {
            RECT r; GetClientRect(h, &r); FillRect((HDC)wp, &r, g.surfaceBrush); return 1;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            return SurfaceControlColor((HDC)wp, TextColor());
        case WM_NOTIFY: {
            auto* draw = (NMCUSTOMDRAW*)lp;
            if (draw->hdr.code != NM_CUSTOMDRAW || GetDlgCtrlID(draw->hdr.hwndFrom) < 100 ||
                GetDlgCtrlID(draw->hdr.hwndFrom) > 106) break;
            if (draw->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYPOSTPAINT;
            if (draw->dwDrawStage == CDDS_POSTPAINT) {
                // Explorer's dark checkbox/radio theme can keep black text. Retain the native
                // glyph, input and accessibility, but paint its label with the active palette.
                RECT r = draw->rc;
                r.left += D(16);
                FillRect(draw->hdc, &r, g.surfaceBrush);
                ++r.left;
                HGDIOBJ font = SelectObject(draw->hdc, review.font);
                SetBkMode(draw->hdc, TRANSPARENT);
                SetTextColor(draw->hdc, TextColor());
                auto text = WindowText(draw->hdr.hwndFrom);
                UINT state = (UINT)SendMessageW(draw->hdr.hwndFrom, WM_QUERYUISTATE, 0, 0);
                DrawTextW(draw->hdc, text.c_str(), -1, &r,
                          DT_SINGLELINE | DT_VCENTER | ((state & UISF_HIDEACCEL) ? DT_HIDEPREFIX : 0));
                if (g.showFocusBoxes && (draw->uItemState & CDIS_FOCUS) && !(state & UISF_HIDEFOCUS))
                    DrawFocusRect(draw->hdc, &r);
                SelectObject(draw->hdc, font);
            }
            return CDRF_DODEFAULT;
        }
        case WM_COMMAND:
            if (LOWORD(wp) == IDCANCEL) DestroyWindow(h);
            else if (LOWORD(wp) == IDOK && review.policy) {
                SetPreferenceSaving(Button_GetCheck(review.automaticChoice) == BST_CHECKED ?
                                    PreferenceSaving::Automatic : PreferenceSaving::Ask);
                if (IsWindow(h)) DestroyWindow(h);
            } else if (LOWORD(wp) == IDOK && Selection()) {
                SaveStartupDefaults(review.choices, Selection());
                if (g.preferenceSaving == PreferenceSaving::Ask && !g.preferenceSavingChosen) ShowSavingQuestion();
                else DestroyWindow(h);
            } else if (HIWORD(wp) == BN_CLICKED) EnableWindow(review.save, review.policy || Selection() != 0);
            return 0;
        case WM_DPICHANGED: {
            UINT oldDpi = review.dpi;
            review.dpi = HIWORD(wp);
            LOGFONTW font{};
            GetObjectW(review.font, sizeof(font), &font);
            font.lfHeight = MulDiv(font.lfHeight, review.dpi, oldDpi);
            HFONT previous = review.font;
            review.font = CreateFontIndirectW(&font);
            for (HWND c = GetWindow(h, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
                SendMessageW(c, WM_SETFONT, (WPARAM)review.font, TRUE);
            DeleteObject(previous);
            auto* r = (RECT*)lp;
            SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_CLOSE: DestroyWindow(h); return 0;
        case WM_DESTROY:
            DeleteObject(review.font);
            review = Review{};
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

StartupPreferences CaptureStartupPreferences() {
    return {g.size, MiniPlayerWindow() != nullptr, g.themeMode, g.libMode, g.browserVisible, g.layout};
}

unsigned PendingStartupDefaults() {
    StartupPreferences approved{g.defaultSize, g.defaultMiniPlayer, g.defaultTheme,
        g.defaultLibraryMode, g.defaultBrowserVisible, g.defaultLayout};
    return Difference(CaptureStartupPreferences(), approved);
}

void SaveStartupDefaults(const StartupPreferences& choices, unsigned selected) {
    if (!selected) return;
    if (selected & kDefaultWindowSize) { g.defaultSize = choices.size; g.defaultMiniPlayer = choices.mini; }
    if (selected & kDefaultTheme) g.defaultTheme = choices.theme;
    if (selected & kDefaultLibraryView) g.defaultLibraryMode = choices.library;
    if (selected & kDefaultBrowser) g.defaultBrowserVisible = choices.browser;
    if (selected & kDefaultLayout) g.defaultLayout = choices.layout;
    SavePreferences();
    SaveBrowserSettings();
}

void ResetPreferenceSuggestions() {
    observed = CaptureStartupPreferences();
    changedAt = 0;
    reviewed = false;
    trackingReady = true;
    lastReview = _wcstoui64(IniRead(L"Suggestions", L"LastReview", L"0").c_str(), nullptr, 10);
}

void ObservePreferenceChanges(ULONGLONG now) {
    if (!trackingReady) return;
    auto choices = CaptureStartupPreferences();
    if (choices != observed) {
        unsigned changes = Difference(choices, observed);
        observed = choices;
        changedAt = now;
        if (g.preferenceSaving == PreferenceSaving::Automatic) SaveStartupDefaults(choices, changes);
    }
}

void FlushAutomaticPreferences() {
    if (g.preferenceSaving == PreferenceSaving::Automatic) ObservePreferenceChanges(GetTickCount64());
}

void SetPreferenceSaving(PreferenceSaving mode) {
    if (mode >= PreferenceSaving::Count) return;
    FlushAutomaticPreferences();
    g.preferenceSaving = mode;
    g.preferenceSavingChosen = true;
    // Automatic saving starts from this moment: unchecked choices in a review stay unapproved.
    observed = CaptureStartupPreferences();
    changedAt = mode == PreferenceSaving::Ask && PendingStartupDefaults() ? GetTickCount64() : 0;
    SavePreferences();
    if (mode != PreferenceSaving::Ask && review.automatic && review.hwnd) DestroyWindow(review.hwnd);
}

bool PreferenceSuggestionsDue(ULONGLONG now) {
    return g.preferenceSaving == PreferenceSaving::Ask && !reviewed && !review.hwnd && PendingStartupDefaults() && changedAt &&
           now >= changedAt && now - changedAt >= kSettleMs &&
           (!lastReview || (WallSeconds() >= lastReview && WallSeconds() - lastReview >= kReviewInterval));
}

void MaybeOfferPreferenceSuggestions() {
    ThemeReview();
    const ULONGLONG now = GetTickCount64();
    ObservePreferenceChanges(now);
    if (g.closing || !PreferenceSuggestionsDue(now)) return;
    HWND owner = MiniPlayerWindow() ? MiniPlayerWindow() : g.hwnd;
    if (!IsWindowVisible(owner) || IsIconic(owner) || !IsWindowEnabled(owner) || GetForegroundWindow() != owner) return;
    LASTINPUTINFO input{sizeof(input)};
    if (GetLastInputInfo(&input) && DWORD(GetTickCount() - input.dwTime) < 15000) return;
    ShowStartupDefaults(true);
}

void ShowStartupDefaults(bool automatic) {
    if (automatic && g.preferenceSaving != PreferenceSaving::Ask) return;
    if (review.hwnd) {
        if (!automatic) SetForegroundWindow(review.hwnd);
        return;
    }
    review.choices = CaptureStartupPreferences();
    review.automatic = automatic;
    review.mask = PendingStartupDefaults();
    if (automatic && !review.mask) return;
    RegisterAppClass(L"CordinalStartupDefaults", ReviewProc);
    HWND owner = MiniPlayerWindow() ? MiniPlayerWindow() : g.hwnd;
    unsigned count = 0;
    for (unsigned i = 0; i < kChoiceCount; ++i) if (review.mask & (1u << i)) ++count;
    RECT r{0, 0, S(440), S(192 + count * 30)}, parent{};
    constexpr DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    constexpr DWORD ex = WS_EX_TOOLWINDOW | WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
    AdjustWindowRectExForDpi(&r, style, FALSE, ex, g.dpi);
    GetWindowRect(owner, &parent);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor);
    int w = std::min((int)(r.right - r.left), (int)(monitor.rcWork.right - monitor.rcWork.left));
    int h = std::min((int)(r.bottom - r.top), (int)(monitor.rcWork.bottom - monitor.rcWork.top));
    int x = std::clamp((int)(parent.left + parent.right - w) / 2, (int)monitor.rcWork.left, (int)monitor.rcWork.right - w);
    int y = std::clamp((int)(parent.top + parent.bottom - h) / 2, (int)monitor.rcWork.top, (int)monitor.rcWork.bottom - h);
    HWND window = CreateWindowExW(ex, L"CordinalStartupDefaults", L"cordinal \x00B7 Startup defaults",
                                  style, x, y, w, h, owner, nullptr, g.inst, nullptr);
    if (!window) { review = Review{}; return; }
    reviewed = true;
    lastReview = WallSeconds();
    IniWrite(L"Suggestions", L"LastReview", std::to_wstring(lastReview));
    ShowWindow(window, automatic ? SW_SHOWNOACTIVATE : SW_SHOW);
    if (!automatic) SetFocus(review.later);
}

HWND StartupDefaultsWindow() { return review.hwnd; }

// Destroying the mini player also destroys windows it owns. Re-own an open review to the main
// window first, so closing the mini player cannot silently discard an unanswered review.
void DetachStartupDefaultsFromMiniPlayer() {
    if (review.hwnd) SetWindowLongPtrW(review.hwnd, GWLP_HWNDPARENT, (LONG_PTR)g.hwnd);
}

bool StartupDefaultsDialogMessage(MSG* msg) {
    return review.hwnd && IsDialogMessageW(review.hwnd, msg);
}

}  // namespace app
