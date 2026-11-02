// Artist info on Windows (View > Artist info..., or the track list's right-click menu): a small
// modeless window with the selected or playing track's artist as MusicBrainz knows them, the
// opening of their English Wikipedia article, and buttons that open MusicBrainz, Wikipedia and
// the official site in the browser. The lookup (metadata_fetch.h) runs on a worker and is off
// until Preferences > Online turns it on; it identifies the artist only when exactly one
// MusicBrainz artist has that name, so a namesake is never shown.
#include "app.h"

#include "metadata_fetch.h"
#include "theme_win.h"

#include <memory>
#include <thread>

namespace app {

namespace {

const wchar_t kArtistClass[] = L"CordinalArtistInfo";
constexpr UINT WM_APP_ARTIST = WM_APP + 1;  // lParam = Result*, owned by the receiver
enum { kIdName = 100, kIdSummary, kIdStatus, kIdMusicBrainz, kIdWikipedia, kIdOfficial, kIdClose };

struct Result {
    uint64_t generation;
    bool found;
    online::ArtistInfo info;
};

struct Window {
    HWND hwnd = nullptr, name = nullptr, summary = nullptr, status = nullptr, musicbrainz = nullptr,
         wikipedia = nullptr, official = nullptr, close = nullptr;
    std::string artist;
    online::ArtistInfo info;
    online::Generation generation;
    std::shared_ptr<online::Cancel> cancel;
    std::thread worker;
} w;

void StopLookup() {
    if (w.cancel) w.cancel->Stop();
    if (w.worker.joinable()) w.worker.join();
    w.cancel.reset();
    w.generation.Next();
}

void StartLookup() {
    StopLookup();
    w.info = {};
    const uint64_t generation = w.generation.Next();
    w.cancel = std::make_shared<online::Cancel>();
    SetWindowTextW(w.status, L"Asking MusicBrainz...");
    SetWindowTextW(w.summary, L"");
    for (HWND b : {w.musicbrainz, w.wikipedia, w.official}) EnableWindow(b, FALSE);
    const HWND target = w.hwnd;
    w.worker = std::thread([artist = w.artist, generation, target, cancel = w.cancel] {
        auto* result = new Result{generation, false, {}};
        result->found = online::FetchArtistInfo(artist, {}, *cancel, result->info);
        if (!PostMessageW(target, WM_APP_ARTIST, 0, (LPARAM)result)) delete result;
    });
}

void Show(const Result& r) {
    w.info = r.info;
    if (!r.found) {
        std::wstring text = L"MusicBrainz has no single artist named \x201C" + FromUtf8(w.artist) +
                            L"\x201D, so nothing is shown rather than a namesake.";
        SetWindowTextW(w.status, text.c_str());
        return;
    }
    SetWindowTextW(w.name, FromUtf8(r.info.name).c_str());
    // Edit controls want CR LF line breaks.
    std::wstring summary;
    for (wchar_t c : FromUtf8(r.info.summary)) {
        if (c == L'\n') summary += L'\r';
        summary += c;
    }
    SetWindowTextW(w.summary, summary.empty() ? L"Wikipedia has no English article for this artist." : summary.c_str());
    SetWindowTextW(w.status, r.info.summary.empty() ? L"From MusicBrainz."
                                                   : L"From MusicBrainz and Wikipedia (text under CC BY-SA 4.0).");
    EnableWindow(w.musicbrainz, !r.info.musicbrainz.empty());
    EnableWindow(w.wikipedia, !r.info.wikipedia.empty());
    EnableWindow(w.official, !r.info.official.empty());
}

void Open(const std::string& url) {
    // The addresses came from the network: only web pages are handed to the browser.
    if (url.starts_with("https://") || url.starts_with("http://"))
        ShellExecuteW(w.hwnd, L"open", FromUtf8(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void LayoutWindow() {
    RECT rc;
    GetClientRect(w.hwnd, &rc);
    const int pad = S(14), bottom = rc.bottom - pad - S(30);
    SetWindowPos(w.name, nullptr, pad, pad, rc.right - pad * 2, S(24), SWP_NOZORDER);
    SetWindowPos(w.summary, nullptr, pad, pad + S(30), rc.right - pad * 2, std::max(S(40), bottom - S(36) - pad - S(30)),
                 SWP_NOZORDER);
    SetWindowPos(w.status, nullptr, pad, bottom - S(28), rc.right - pad * 2, S(20), SWP_NOZORDER);
    int x = pad;
    for (HWND b : {w.musicbrainz, w.wikipedia, w.official}) {
        SetWindowPos(b, nullptr, x, bottom, S(108), S(30), SWP_NOZORDER);
        x += S(114);
    }
    SetWindowPos(w.close, nullptr, rc.right - pad - S(100), bottom, S(100), S(30), SWP_NOZORDER);
}

LRESULT CALLBACK ArtistProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            w.hwnd = h;
            auto make = [&](DWORD ex, const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
                HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, h, (HMENU)(INT_PTR)id,
                                         g.inst, nullptr);
                SendMessageW(c, WM_SETFONT, (WPARAM)g.font, TRUE);
                return c;
            };
            w.name = make(0, WC_STATICW, FromUtf8(w.artist).c_str(), SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, kIdName);
            SendMessageW(w.name, WM_SETFONT, (WPARAM)g.fontBold, TRUE);
            w.summary = make(WS_EX_CLIENTEDGE, WC_EDITW, L"",
                             ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP, kIdSummary);
            SetWindowTheme(w.summary, g.dark ? L"DarkMode_CFD" : L"Explorer", nullptr);
            w.status = make(0, WC_STATICW, L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, kIdStatus);
            w.musicbrainz = make(0, WC_BUTTONW, L"&MusicBrainz", BS_PUSHBUTTON | WS_TABSTOP, kIdMusicBrainz);
            w.wikipedia = make(0, WC_BUTTONW, L"&Wikipedia", BS_PUSHBUTTON | WS_TABSTOP, kIdWikipedia);
            w.official = make(0, WC_BUTTONW, L"&Official site", BS_PUSHBUTTON | WS_TABSTOP, kIdOfficial);
            w.close = make(0, WC_BUTTONW, L"Close", BS_DEFPUSHBUTTON | WS_TABSTOP, kIdClose);
            for (HWND c : {w.musicbrainz, w.wikipedia, w.official, w.close}) ApplyControlTheme(c, g.dark);
            ApplyWindowTheme(h, P());
            LayoutWindow();
            return 0;
        }
        case WM_SIZE:
            LayoutWindow();
            return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {S(480), S(300)};
            return 0;
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(h, &rc);
            FillRect((HDC)wp, &rc, g.surfaceBrush);
            return 1;
        }
        case WM_CTLCOLORSTATIC:
            if ((HWND)lp == w.summary) return FieldControlColor((HDC)wp);
            return SurfaceControlColor((HDC)wp, (HWND)lp == w.status ? SubColor() : TextColor());
        case WM_CTLCOLORBTN:
            return SurfaceControlColor((HDC)wp, TextColor());
        case WM_CTLCOLOREDIT:
            return FieldControlColor((HDC)wp);
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case kIdMusicBrainz: Open(w.info.musicbrainz); break;
                case kIdWikipedia: Open(w.info.wikipedia); break;
                case kIdOfficial: Open(w.info.official); break;
                case IDOK:
                case kIdClose:
                case IDCANCEL: DestroyWindow(h); break;
            }
            return 0;
        case WM_APP_ARTIST: {
            std::unique_ptr<Result> result(reinterpret_cast<Result*>(lp));
            if (!w.generation.Accepts(result->generation)) return 0;  // another artist was asked for since
            if (w.worker.joinable()) w.worker.join();
            w.cancel.reset();
            Show(*result);
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY: {
            StopLookup();
            MSG pending;
            while (PeekMessageW(&pending, h, WM_APP_ARTIST, WM_APP_ARTIST, PM_REMOVE))
                delete reinterpret_cast<Result*>(pending.lParam);
            w.hwnd = nullptr;
            w.info = {};
            return 0;
        }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// The lookup may run while it is off, but only after the user agrees to what it sends.
bool EnsureArtistInfo() {
    if (online::Config().artistInfo) return true;
    const wchar_t* text =
        L"Artist info is off, so cordinal hasn't contacted anyone.\n\n"
        L"It sends the artist's name to musicbrainz.org, then asks wikidata.org and en.wikipedia.org "
        L"for the article MusicBrainz names.\n\nTurn Artist info on? It can be changed in Preferences > Online.";
    if (MessageBoxW(g.hwnd, text, L"Artist info", MB_YESNO | MB_ICONQUESTION) != IDYES) return false;
    if (const Tweak* t = FindTweak("online.artist_info")) SetTweak(*t, 1);
    return online::Config().artistInfo;
}

}  // namespace

void ShowArtistInfo() {
    auto ids = SelectedTracks();
    TrackId id = !ids.empty() ? ids.front() : g.playing;
    if (id == kNone || !g_lib.Contains(id)) {
        ShowMessage(L"Select a track, or play one, to see its artist.");
        return;
    }
    const Track& t = g_lib[id];
    std::string artist = ToUtf8(g_lib.Str(t.artist ? t.artist : t.albumArtist));
    if (artist.empty()) {
        ShowMessage(L"This track has no artist in its tags.");
        return;
    }
    if (!EnsureArtistInfo()) return;
    if (w.hwnd && w.artist == artist) {
        ShowWindow(w.hwnd, SW_RESTORE);
        SetForegroundWindow(w.hwnd);
        return;
    }
    if (w.hwnd) DestroyWindow(w.hwnd);
    w.artist = artist;
    RegisterAppClass(kArtistClass, ArtistProc);
    RECT r{0, 0, S(560), S(420)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_DLGMODALFRAME, g.dpi);
    RECT owner;
    GetWindowRect(g.hwnd, &owner);
    const int width = r.right - r.left, height = r.bottom - r.top;
    CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kArtistClass, L"Artist info",
                    WS_OVERLAPPEDWINDOW & ~WS_MINIMIZEBOX & ~WS_MAXIMIZEBOX | WS_CLIPCHILDREN,
                    owner.left + (owner.right - owner.left - width) / 2, owner.top + S(80), width, height, g.hwnd,
                    nullptr, g.inst, nullptr);
    if (!w.hwnd) return;
    ShowWindow(w.hwnd, SW_SHOW);
    StartLookup();
}

bool ArtistInfoDialogMessage(MSG* msg) { return w.hwnd && IsDialogMessageW(w.hwnd, msg); }

void StopArtistInfo() {
    if (w.hwnd) DestroyWindow(w.hwnd);
    StopLookup();
}

}  // namespace app
