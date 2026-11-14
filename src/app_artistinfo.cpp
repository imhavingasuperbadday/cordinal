// Artist info on Windows (View > Artist info..., or the track list's right-click menu): a small
// modeless window with the selected or playing track's artist as MusicBrainz knows them, the
// opening of their English Wikipedia article, and buttons that open MusicBrainz, Wikipedia and
// the official site in the browser. Below, the artist's albums and EPs as MusicBrainz lists them,
// each marked when the library has it, with a filter for the ones it lacks; double-clicking one opens
// its MusicBrainz page. The lookup (metadata_fetch.h) runs on a worker and is off until
// Preferences > Online turns it on; it identifies the artist only when exactly one MusicBrainz
// artist has that name, so a namesake is never shown.
#include "app.h"

#include "metadata_fetch.h"
#include "theme_win.h"

#include <memory>
#include <thread>

namespace app {

namespace {

const wchar_t kArtistClass[] = L"CordinalArtistInfo";
constexpr UINT WM_APP_ARTIST = WM_APP + 1;  // lParam = Result*, owned by the receiver
enum { kIdName = 100, kIdSummary, kIdStatus, kIdMusicBrainz, kIdWikipedia, kIdOfficial, kIdClose, kIdMissing, kIdAlbums };

struct Result {
    uint64_t generation;
    bool found;
    online::ArtistInfo info;
    std::vector<online::ReleaseGroup> albums;
    bool albumsKnown;  // MusicBrainz answered for the discography (an empty one included)
};

struct Window {
    HWND hwnd = nullptr, name = nullptr, summary = nullptr, status = nullptr, musicbrainz = nullptr,
         wikipedia = nullptr, official = nullptr, close = nullptr, missing = nullptr, albums = nullptr;
    std::string artist;
    online::ArtistInfo info;
    std::vector<online::ReleaseGroup> discography;
    bool discographyKnown = false;
    std::vector<int> shown;  // the list's rows, as indexes into discography
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
    w.discography.clear();
    w.shown.clear();
    ListView_DeleteAllItems(w.albums);
    const uint64_t generation = w.generation.Next();
    w.cancel = std::make_shared<online::Cancel>();
    SetWindowTextW(w.status, L"Asking MusicBrainz...");
    SetWindowTextW(w.summary, L"");
    for (HWND b : {w.musicbrainz, w.wikipedia, w.official}) EnableWindow(b, FALSE);
    const HWND target = w.hwnd;
    w.worker = std::thread([artist = w.artist, generation, target, cancel = w.cancel] {
        auto* result = new Result{generation, false, {}, {}, false};
        result->found = online::FetchArtistInfo(artist, {}, *cancel, result->info);
        if (result->found) result->albumsKnown = online::FetchDiscography(result->info.mbid, *cancel, result->albums);
        if (!PostMessageW(target, WM_APP_ARTIST, 0, (LPARAM)result)) delete result;
    });
}

// The discography's rows: every album, or only those the library lacks.
void FillAlbums() {
    ListView_DeleteAllItems(w.albums);
    w.shown.clear();
    const bool missingOnly = Button_GetCheck(w.missing) == BST_CHECKED;
    for (int i = 0; i < (int)w.discography.size(); ++i)
        if (!missingOnly || !w.discography[i].owned) w.shown.push_back(i);
    for (int row = 0; row < (int)w.shown.size(); ++row) {
        const online::ReleaseGroup& r = w.discography[w.shown[row]];
        std::wstring year = r.year ? std::to_wstring(r.year) : L"", title = FromUtf8(r.title), type = FromUtf8(r.type);
        LVITEMW item{LVIF_TEXT, row};
        item.pszText = year.data();
        ListView_InsertItem(w.albums, &item);
        ListView_SetItemText(w.albums, row, 1, title.data());
        ListView_SetItemText(w.albums, row, 2, type.data());
        ListView_SetItemText(w.albums, row, 3, const_cast<wchar_t*>(r.owned ? L"In library" : L""));
    }
}

std::wstring AlbumCounts() {
    if (!w.discographyKnown) return L"Their albums couldn't be fetched from MusicBrainz.";
    if (w.discography.empty()) return L"MusicBrainz lists no albums or EPs for this artist.";
    size_t missing = std::count_if(w.discography.begin(), w.discography.end(), [](const auto& r) { return !r.owned; });
    return std::to_wstring(w.discography.size()) + (w.discography.size() == 1 ? L" album or EP, " : L" albums and EPs, ") +
           std::to_wstring(missing) + L" not in your library.";
}

void Show(Result& r) {
    w.info = r.info;
    w.discography = std::move(r.albums);
    w.discographyKnown = r.albumsKnown;
    online::MarkOwned(w.discography, online::LibraryAlbumsBy(g_lib, w.artist));
    FillAlbums();
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
    std::wstring status = r.info.summary.empty() ? L"From MusicBrainz. "
                                                 : L"From MusicBrainz and Wikipedia (text under CC BY-SA 4.0). ";
    SetWindowTextW(w.status, (status + AlbumCounts()).c_str());
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
    const int pad = S(14), bottom = rc.bottom - pad - S(30), width = rc.right - pad * 2;
    // The article takes two fifths of the middle, the discography the rest.
    const int top = pad + S(30), middle = std::max(S(160), bottom - S(36) - top);
    const int summary = middle * 2 / 5, listTop = top + summary + S(34);
    SetWindowPos(w.name, nullptr, pad, pad, width, S(24), SWP_NOZORDER);
    SetWindowPos(w.summary, nullptr, pad, top, width, summary, SWP_NOZORDER);
    SetWindowPos(w.missing, nullptr, pad, top + summary + S(6), width, S(22), SWP_NOZORDER);
    SetWindowPos(w.albums, nullptr, pad, listTop, width, std::max(S(40), top + middle - listTop), SWP_NOZORDER);
    const int scrollbar = GetSystemMetricsForDpi(SM_CXVSCROLL, g.dpi);
    ListView_SetColumnWidth(w.albums, 1, std::max(S(120), width - S(52) - S(100) - S(90) - scrollbar - S(4)));
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
            w.missing = make(0, WC_BUTTONW, L"&Only albums not in my library", BS_AUTOCHECKBOX | WS_TABSTOP, kIdMissing);
            w.albums = make(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                            LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER | WS_TABSTOP, kIdAlbums);
            ListView_SetExtendedListViewStyle(w.albums, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            const wchar_t* columns[] = {L"Year", L"Album", L"Type", L"In library"};
            const int widths[] = {S(52), S(220), S(100), S(90)};
            for (int c = 0; c < 4; ++c) {
                LVCOLUMNW col{};
                col.mask = LVCF_TEXT | LVCF_WIDTH;
                col.cx = widths[c];
                col.pszText = const_cast<wchar_t*>(columns[c]);
                ListView_InsertColumn(w.albums, c, &col);
            }
            for (HWND c : {w.albums, ListView_GetHeader(w.albums)}) ApplyControlTheme(c, g.dark);
            ListView_SetBkColor(w.albums, Ref(P().surface2));
            ListView_SetTextBkColor(w.albums, Ref(P().surface2));
            ListView_SetTextColor(w.albums, TextColor());
            w.status = make(0, WC_STATICW, L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, kIdStatus);
            w.musicbrainz = make(0, WC_BUTTONW, L"&MusicBrainz", BS_PUSHBUTTON | WS_TABSTOP, kIdMusicBrainz);
            w.wikipedia = make(0, WC_BUTTONW, L"&Wikipedia", BS_PUSHBUTTON | WS_TABSTOP, kIdWikipedia);
            w.official = make(0, WC_BUTTONW, L"&Official site", BS_PUSHBUTTON | WS_TABSTOP, kIdOfficial);
            w.close = make(0, WC_BUTTONW, L"Close", BS_DEFPUSHBUTTON | WS_TABSTOP, kIdClose);
            for (HWND c : {w.musicbrainz, w.wikipedia, w.official, w.close, w.missing}) ApplyControlTheme(c, g.dark);
            ApplyWindowTheme(h, P());
            LayoutWindow();
            return 0;
        }
        case WM_SIZE:
            LayoutWindow();
            return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {S(480), S(420)};
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
        case WM_NOTIFY: {
            auto* hdr = reinterpret_cast<NMHDR*>(lp);
            if (hdr->idFrom == kIdAlbums && (hdr->code == LVN_ITEMACTIVATE || hdr->code == NM_DBLCLK)) {
                int row = ListView_GetNextItem(w.albums, -1, LVNI_SELECTED);
                if (row >= 0 && row < (int)w.shown.size()) Open(w.discography[w.shown[row]].Url());
            }
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case kIdMissing: FillAlbums(); break;
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
            w.discography.clear();
            w.shown.clear();
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
        L"It sends the artist's name to musicbrainz.org, which also lists their albums, then asks "
        L"wikidata.org and en.wikipedia.org for the article MusicBrainz names.\n\nTurn Artist info on? It can be changed in Preferences > Online.";
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
    RECT r{0, 0, S(600), S(600)};
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
