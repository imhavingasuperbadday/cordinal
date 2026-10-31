// Title formatting window (Preferences ▸ View): the four [Format] patterns of settings.ini,
// each with a live preview. Changes show in the main window as you type; Cancel puts them back. A
// pattern that a pack sets while it is on is shown read-only, and the user's own stays saved.
#include "app.h"

namespace app {

namespace {

constexpr int kFormats = 4;
constexpr int kIdEdit0 = 100, kIdReset0 = 110, kIdResetAll = 120;

struct FormatSpec {
    const wchar_t* key;
    const wchar_t* label;
    const wchar_t* def;
    std::wstring App::*target;
    int packSlot;  // PackFormatSlot
};
const FormatSpec kSpecs[kFormats] = {
    {L"NowPlayingTitle", L"Now playing", L"%title%", &App::fmtTitle, kPackNowPlaying},
    {L"NowPlayingSubtitle", L"Now playing, second line", L"[%artist%][  \x2022  %album%][  \x2022  %date%]", &App::fmtSub,
     kPackNowPlaying2},
    {L"StatusBar", L"Status bar",
     L"[%codec% | ][%bitrate% kbps | ][%samplerate% Hz | ][%channels% | ]%playback_time% / %length%", &App::fmtStatus,
     kPackStatus},
    {L"WindowTitle", L"Window title", L"[%artist% - ]%title% - cordinal", &App::fmtWindow, kPackWindow},
};

bool PackOwned(int i) { return PackFormatOwner(kSpecs[i].packSlot) != nullptr; }

struct FormatWindow {
    HWND hwnd = nullptr;
    HWND labels[kFormats] = {}, edits[kFormats] = {}, previews[kFormats] = {}, resets[kFormats] = {};
    HWND help = nullptr, source = nullptr, resetAll = nullptr, ok = nullptr, cancel = nullptr;
    std::wstring original[kFormats];
    TrackId sample = kNone;
    HFONT mono = nullptr;
    bool ready = false;  // edits send EN_CHANGE while they are being created
} f;

// The track the previews show: what's playing, else the focused or first selected track, else
// the first track of the library.
TrackId SampleTrack() {
    if (g.playing != kNone) return g.playing;
    auto sel = SelectedTracks();
    if (!sel.empty()) return sel[0];
    for (TrackId id = 0; id < (TrackId)g_lib.Count(); ++id)
        if (g_lib[id].InLibrary() && g_lib[id].Loaded()) return id;
    return kNone;
}

void UpdatePreviews() {
    for (int i = 0; i < kFormats; ++i) {
        std::wstring text = f.sample == kNone ? L"" : FormatTitle(WindowText(f.edits[i]), Lookup(f.sample, f.sample == g.playing));
        SetTextIfChanged(f.previews[i], text.empty() ? L"(empty)" : text);
    }
}

// Shows the patterns in the main window right away.
void ApplyLive() {
    for (int i = 0; i < kFormats; ++i)
        if (!PackOwned(i)) g.*kSpecs[i].target = WindowText(f.edits[i]);
    UpdateNowPlaying();
    UpdateStatus();
}

// OK: keep the edited patterns, save them to settings.ini, and leave them live in the main window.
void Commit() {
    for (int i = 0; i < kFormats; ++i) {
        if (PackOwned(i)) continue;
        std::wstring v = WindowText(f.edits[i]);
        g.*kSpecs[i].target = v;
        IniWrite(L"Format", kSpecs[i].key, v);
    }
    UpdateNowPlaying();
    UpdateStatus();
}

// Cancel or close: put back the patterns as they were when the window opened.
void Revert() {
    for (int i = 0; i < kFormats; ++i) g.*kSpecs[i].target = f.original[i];
    UpdateNowPlaying();
    UpdateStatus();
}

void LayoutFormat() {
    RECT rc;
    GetClientRect(f.hwnd, &rc);
    int pad = S(14), w = rc.right - pad * 2, y = pad, resetW = S(64);
    MoveWindow(f.source, pad, y, w, S(18), TRUE);
    y += S(28);
    for (int i = 0; i < kFormats; ++i) {
        MoveWindow(f.labels[i], pad, y, w, S(18), TRUE);
        y += S(20);
        MoveWindow(f.edits[i], pad, y, w - resetW - S(8), S(24), TRUE);
        MoveWindow(f.resets[i], pad + w - resetW, y, resetW, S(24), TRUE);
        y += S(27);
        MoveWindow(f.previews[i], pad + S(2), y, w - S(2), S(18), TRUE);
        y += S(30);
    }
    int helpH = rc.bottom - y - S(48);
    MoveWindow(f.help, pad, y, w, std::max(helpH, 0), TRUE);
    int by = rc.bottom - pad - S(26), bw = S(86);
    MoveWindow(f.resetAll, pad, by, S(110), S(26), TRUE);
    MoveWindow(f.cancel, rc.right - pad - bw, by, bw, S(26), TRUE);
    MoveWindow(f.ok, rc.right - pad - bw * 2 - S(8), by, bw, S(26), TRUE);
}

LRESULT CALLBACK FormatProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            f.hwnd = h;
            auto make = [&](DWORD ex, const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
                HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, h, (HMENU)(INT_PTR)id, g.inst, nullptr);
                SendMessageW(c, WM_SETFONT, (WPARAM)g.font, TRUE);
                return c;
            };
            LOGFONTW lf{};
            GetObjectW(g.font, sizeof(lf), &lf);
            wcscpy_s(lf.lfFaceName, L"Consolas");
            f.mono = CreateFontIndirectW(&lf);
            f.sample = SampleTrack();
            std::wstring source = f.sample == kNone ? L"Add some music to see previews."
                                                    : L"Previews show: " + FormatTitle(L"%title%[ \x2014 %artist%]", Lookup(f.sample, false));
            f.source = make(0, WC_STATICW, source.c_str(), SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, 0);
            for (int i = 0; i < kFormats; ++i) f.original[i] = g.*kSpecs[i].target;
            for (int i = 0; i < kFormats; ++i) {
                const Pack* owner = PackFormatOwner(kSpecs[i].packSlot);
                std::wstring label = kSpecs[i].label + (owner ? L"  (set by the pack " + owner->name + L" while it is on)" : L"");
                f.labels[i] = make(0, WC_STATICW, label.c_str(), SS_LEFT | SS_NOPREFIX, 0);
                f.edits[i] = make(WS_EX_CLIENTEDGE, WC_EDITW, f.original[i].c_str(),
                                  ES_AUTOHSCROLL | WS_TABSTOP | (owner ? ES_READONLY : 0), kIdEdit0 + i);
                SendMessageW(f.edits[i], WM_SETFONT, (WPARAM)f.mono, TRUE);
                f.resets[i] = make(0, WC_BUTTONW, L"Default", BS_PUSHBUTTON | WS_TABSTOP, kIdReset0 + i);
                if (owner) EnableWindow(f.resets[i], FALSE);
                f.previews[i] = make(0, WC_STATICW, L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, 0);
            }
            f.help = make(0, WC_STATICW,
                          L"%field% inserts a field: title, artist, album artist, album, date, year, genre, edition, edition_kind, "
                          L"musicbrainz_albumid, musicbrainz_releasegroupid, tracknumber, "
                          L"discnumber, length, codec, bitrate, samplerate, channels, rating, play_count, last_played, path, "
                          L"and playback_time / playback_time_remaining in the status bar.  [ ... ] disappears when its fields "
                          L"are empty.  'text' is literal.  $if(), $upper(), $num(), $pad() and friends are listed in the README.",
                          SS_LEFT | SS_NOPREFIX, 0);
            f.resetAll = make(0, WC_BUTTONW, L"&All defaults", BS_PUSHBUTTON | WS_TABSTOP, kIdResetAll);
            f.ok = make(0, WC_BUTTONW, L"OK", BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK);
            f.cancel = make(0, WC_BUTTONW, L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL);
            for (HWND c : f.edits) SetWindowTheme(c, g.dark ? L"DarkMode_CFD" : L"Explorer", nullptr);
            for (HWND c : f.resets) ApplyControlTheme(c, g.dark);
            for (HWND c : {f.resetAll, f.ok, f.cancel}) ApplyControlTheme(c, g.dark);
            ApplyWindowTheme(h, P());
            f.ready = true;
            UpdatePreviews();
            return 0;
        }
        case WM_SIZE:
            LayoutFormat();
            return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {S(520), S(480)};
            return 0;
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(h, &rc);
            FillRect((HDC)wp, &rc, g.surfaceBrush);
            return 1;
        }
        case WM_CTLCOLORSTATIC: {
            HWND c = (HWND)lp;
            bool preview = std::find(std::begin(f.previews), std::end(f.previews), c) != std::end(f.previews);
            return SurfaceControlColor((HDC)wp, preview ? Ref(P().accent) : c == f.help || c == f.source ? SubColor() : TextColor());
        }
        case WM_CTLCOLOREDIT:
            return FieldControlColor((HDC)wp);
        case WM_COMMAND: {
            // A "Default" button only rewrites the edit; the EN_CHANGE it raises re-previews live.
            int id = LOWORD(wp), code = HIWORD(wp);
            if (id >= kIdEdit0 && id < kIdEdit0 + kFormats && code == EN_CHANGE) {
                if (!f.ready) return 0;
                UpdatePreviews();
                ApplyLive();
            } else if (id >= kIdReset0 && id < kIdReset0 + kFormats) {
                SetWindowTextW(f.edits[id - kIdReset0], kSpecs[id - kIdReset0].def);
            } else if (id == kIdResetAll) {
                for (int i = 0; i < kFormats; ++i)
                    if (!PackOwned(i)) SetWindowTextW(f.edits[i], kSpecs[i].def);
            } else if (id == IDOK) {
                Commit();
                DestroyWindow(h);
            } else if (id == IDCANCEL) {
                Revert();
                DestroyWindow(h);
            }
            return 0;
        }
        case WM_CLOSE:
            Revert();
            DestroyWindow(h);
            return 0;
        case WM_NCDESTROY:  // after the children, which use the font
            if (f.mono) DeleteObject(f.mono);
            f = FormatWindow{};
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

bool FormatDialogMessage(MSG* msg) { return f.hwnd && IsDialogMessageW(f.hwnd, msg); }

void ShowFormatWindow() {
    if (f.hwnd) {
        SetForegroundWindow(f.hwnd);
        return;
    }
    RegisterAppClass(L"CordinalFormat", FormatProc);
    RECT r{0, 0, S(640), S(500)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_DLGMODALFRAME, g.dpi);
    RECT owner;
    GetWindowRect(g.hwnd, &owner);
    int w = r.right - r.left, hgt = r.bottom - r.top;
    CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, L"CordinalFormat", L"Title formatting",
                    WS_OVERLAPPEDWINDOW & ~WS_MINIMIZEBOX | WS_CLIPCHILDREN, owner.left + (owner.right - owner.left - w) / 2,
                    owner.top + S(60), w, hgt, g.hwnd, nullptr, g.inst, nullptr);
    ShowWindow(f.hwnd, SW_SHOW);
    SetFocus(f.edits[0]);
}

}  // namespace app
