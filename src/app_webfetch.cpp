// Save music from the web (Library > Save music from the web...): a small modeless
// window where pasted links become files you own. SoundCloud tracks, sets, likes and
// artists' pages, YouTube videos and playlists, and Spotify tracks, albums and
// playlists (matched to the same recording on YouTube, never decrypted from Spotify's
// stream) download one at a time on a worker, are tagged with their cover, and land in
// the first library folder as Artist\Album\[NN ]Title. What has been saved and what
// failed for good is remembered in the data folder, so nothing is fetched twice and
// dead tracks are left alone until "Forget what failed" asks for them again. The folder
// scanner picks the finished files up as they land, so they simply appear in the
// library.
#include "app.h"

#include "metadata_fetch.h"
#include "theme_win.h"
#include "webfetch.h"

#include <cctype>
#include <memory>
#include <thread>

namespace app {

namespace {

const wchar_t kWebFetchClass[] = L"CordinalWebFetch";
constexpr UINT kAdded = WM_APP + 1, kUpdated = WM_APP + 2, kNotice = WM_APP + 3, kDone = WM_APP + 4;
enum { kIdPaste = 100, kIdList, kIdStatus, kIdSave, kIdStop, kIdForget, kIdClose };

struct Window {
    HWND hwnd = nullptr, paste = nullptr, list = nullptr, status = nullptr, save = nullptr,
         stop = nullptr, forget = nullptr, close = nullptr;
    std::thread worker;
    std::atomic<bool> cancel{false};
    bool running = false;
    std::vector<WebFetchItem> items;  // the UI thread's mirror of the queue's list
} w;

void StopRun() {
    w.cancel = true;
    if (w.worker.joinable()) w.worker.join();
    w.running = false;
}

std::wstring TrackText(const WebFetchItem& it) {
    std::wstring title = FromUtf8(it.track.title);
    std::wstring artist = FromUtf8(it.track.artist);
    if (!artist.empty()) return artist + L" - " + title;
    return title.empty() ? FromUtf8(it.track.id) : title;
}

std::wstring StateText(const WebFetchItem& it) {
    switch (it.state) {
    case WebState::Resolving: return L"Reading...";
    case WebState::Downloading:
        return it.percent >= 0 ? L"Downloading... " + std::to_wstring(it.percent) + L"%" : L"Downloading...";
    case WebState::Saving: return L"Saving...";
    case WebState::Done: return L"Saved";
    case WebState::Exists: return L"Already yours";
    case WebState::Failed: return FromUtf8(it.why);
    }
    return L"";
}

void SetRow(int row, const WebFetchItem& it) {
    std::wstring track = TrackText(it), state = StateText(it);
    ListView_SetItemText(w.list, row, 0, track.data());
    ListView_SetItemText(w.list, row, 1, state.data());
}

int AddRow(const WebFetchItem& it) {
    std::wstring track = TrackText(it);
    LVITEMW item{LVIF_TEXT, (int)w.items.size() - 1};
    item.pszText = track.data();
    int row = ListView_InsertItem(w.list, &item);
    if (row >= 0) SetRow(row, it);
    return row;
}

// One paste can hold several links: the box takes whatever is separated by spaces or
// line breaks, the same way soundcli's add takes its arguments.
std::vector<std::string> LinksOf(const std::wstring& pasted) {
    std::vector<std::string> links;
    std::string all = ToUtf8(pasted);
    size_t at = 0;
    while (at < all.size()) {
        while (at < all.size() && std::isspace((unsigned char)all[at])) at++;
        size_t end = at;
        while (end < all.size() && !std::isspace((unsigned char)all[end])) end++;
        if (end > at) links.push_back(all.substr(at, end - at));
        at = end;
    }
    return links;
}

// The first save asks what would be sent, the same way Artist info asks before its
// first lookup; the answer lives in Preferences > Online and can be changed there.
bool EnsureWebSave() {
    if (online::Config().saveMusic) return true;
    const wchar_t* text =
        L"Save music from the web is off, so cordinal hasn't contacted anyone.\n\n"
        L"It downloads the link you paste from that site: the audio, its cover and its details, "
        L"saved as your own file in the library. A Spotify link's audio is the same recording "
        L"matched on YouTube.\n\nTurn Save music from the web on? It can be changed in Preferences > Online.";
    if (MessageBoxW(g.hwnd, text, L"Save music from the web", MB_YESNO | MB_ICONQUESTION) != IDYES) return false;
    if (const Tweak* t = FindTweak("online.save_music")) SetTweak(*t, 1);
    return online::Config().saveMusic;
}

void TrySave() {
    if (w.running || !w.hwnd) return;
    const int length = GetWindowTextLengthW(w.paste);
    std::wstring pasted((size_t)std::max(0, length), L'\0');
    GetWindowTextW(w.paste, pasted.data(), length + 1);
    std::vector<std::string> links = LinksOf(pasted);
    if (links.empty()) {
        SetWindowTextW(w.status, L"Paste a link first: a SoundCloud, YouTube or Spotify address.");
        return;
    }
    if (!EnsureWebSave()) return;
    if (g.libraryFolders.empty()) {
        SetWindowTextW(w.status, L"Add a library folder first (Library > Library folders...).");
        return;
    }
    const std::wstring root = g.libraryFolders.front(), dataDir = g.dataDir;
    SetWindowTextW(w.status, L"Reading the links...");
    EnableWindow(w.save, FALSE);
    EnableWindow(w.stop, TRUE);
    w.cancel = false;
    w.running = true;
    const HWND target = w.hwnd;
    w.worker = std::thread([links, root, dataDir, target] {
        auto post = [target](UINT msg, WPARAM wp, void* owned) {
            if (!PostMessageW(target, msg, wp, (LPARAM)owned)) delete reinterpret_cast<WebFetchItem*>(owned);
        };
        WebFetchRun run;
        run.links = links;
        run.libraryRoot = root;
        run.dataDir = dataDir;
        run.cancel = &w.cancel;
        run.added = [&](const WebFetchItem& item) { post(kAdded, 0, new WebFetchItem(item)); };
        run.updated = [&](size_t index, const WebFetchItem& item) {
            post(kUpdated, (WPARAM)index, new WebFetchItem(item));
        };
        run.notice = [&](const std::string& text) { post(kNotice, 0, new std::string(text)); };
        std::vector<WebFetchItem> items;
        std::string error;
        RunWebFetch(run, items, error);
        // The summary the window shows once the queue is through: what was saved, what
        // was already owned, and what failed, in words.
        unsigned saved = 0, had = 0, failed = 0;
        for (const WebFetchItem& it : items) {
            if (it.state == WebState::Done) saved++;
            else if (it.state == WebState::Exists) had++;
            else if (it.state == WebState::Failed) failed++;
        }
        std::wstring summary = std::to_wstring(saved) + L" saved, " + std::to_wstring(had) +
                               L" already yours, " + std::to_wstring(failed) + L" failed.";
        if (!error.empty()) summary += L"  (" + FromUtf8(error) + L")";
        post(kDone, 0, new std::wstring(summary));
    });
}

void LayoutWindow() {
    RECT rc;
    GetClientRect(w.hwnd, &rc);
    const int pad = S(14);
    const int width = rc.right - pad * 2;
    const int rowH = S(30), bottom = rc.bottom - pad - rowH;
    SetWindowPos(w.paste, nullptr, pad, pad, std::max(S(100), width - S(110)), S(26), SWP_NOZORDER);
    SetWindowPos(w.save, nullptr, rc.right - pad - S(100), pad, S(100), S(26), SWP_NOZORDER);
    SetWindowPos(w.list, nullptr, pad, pad + S(34), std::max(S(100), width),
                 std::max(S(60), bottom - S(34) - pad - S(34)), SWP_NOZORDER);
    SetWindowPos(w.status, nullptr, pad, bottom - S(26), std::max(S(100), width), S(20), SWP_NOZORDER);
    int x = pad;
    for (HWND b : {w.stop, w.forget}) {
        SetWindowPos(b, nullptr, x, bottom, S(130), S(30), SWP_NOZORDER);
        x += S(136);
    }
    SetWindowPos(w.close, nullptr, rc.right - pad - S(100), bottom, S(100), S(30), SWP_NOZORDER);
    const int scrollbar = GetSystemMetricsForDpi(SM_CXVSCROLL, g.dpi);
    ListView_SetColumnWidth(w.list, 1, std::max(S(120), width - S(320) - scrollbar));
}

LRESULT CALLBACK WebFetchProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            w.hwnd = h;
            auto make = [&](DWORD ex, const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
                HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, h,
                                         (HMENU)(INT_PTR)id, g.inst, nullptr);
                SendMessageW(c, WM_SETFONT, (WPARAM)g.font, TRUE);
                return c;
            };
            w.paste = make(WS_EX_CLIENTEDGE, WC_EDITW, L"", ES_AUTOHSCROLL | WS_TABSTOP, kIdPaste);
            SetWindowTheme(w.paste, g.dark ? L"DarkMode_CFD" : L"Explorer", nullptr);
            w.save = make(0, WC_BUTTONW, L"&Save", BS_DEFPUSHBUTTON | WS_TABSTOP, kIdSave);
            w.list = make(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                          LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER | WS_TABSTOP, kIdList);
            ListView_SetExtendedListViewStyle(w.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            const wchar_t* columns[] = {L"Track", L"Status"};
            const int widths[] = {S(320), S(160)};
            for (int c = 0; c < 2; ++c) {
                LVCOLUMNW col{};
                col.mask = LVCF_TEXT | LVCF_WIDTH;
                col.cx = widths[c];
                col.pszText = const_cast<wchar_t*>(columns[c]);
                ListView_InsertColumn(w.list, c, &col);
            }
            for (HWND c : {w.list, ListView_GetHeader(w.list)}) ApplyControlTheme(c, g.dark);
            ListView_SetBkColor(w.list, Ref(P().surface2));
            ListView_SetTextBkColor(w.list, Ref(P().surface2));
            ListView_SetTextColor(w.list, TextColor());
            w.status = make(0, WC_STATICW, L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, kIdStatus);
            w.stop = make(0, WC_BUTTONW, L"&Stop", BS_PUSHBUTTON | WS_TABSTOP, kIdStop);
            w.forget = make(0, WC_BUTTONW, L"Forgot what &failed", BS_PUSHBUTTON | WS_TABSTOP, kIdForget);
            w.close = make(0, WC_BUTTONW, L"Close", BS_PUSHBUTTON | WS_TABSTOP, kIdClose);
            for (HWND c : {w.save, w.stop, w.forget, w.close}) ApplyControlTheme(c, g.dark);
            EnableWindow(w.stop, FALSE);
            ApplyWindowTheme(h, P());
            LayoutWindow();
            return 0;
        }
        case WM_SIZE:
            LayoutWindow();
            return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {S(520), S(360)};
            return 0;
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(h, &rc);
            FillRect((HDC)wp, &rc, g.surfaceBrush);
            return 1;
        }
        case WM_CTLCOLORSTATIC:
            return SurfaceControlColor((HDC)wp, (HWND)lp == w.status ? SubColor() : TextColor());
        case WM_CTLCOLORBTN:
            return SurfaceControlColor((HDC)wp, TextColor());
        case WM_CTLCOLOREDIT:
            return FieldControlColor((HDC)wp);
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDOK:
                case kIdSave: TrySave(); break;
                case kIdStop:
                    w.cancel = true;
                    SetWindowTextW(w.status, L"Stopping after the track in flight...");
                    break;
                case kIdForget:
                    if (WebFetchForgetFailures(g.dataDir))
                        SetWindowTextW(w.status, L"Forgotten. The next save tries those tracks again.");
                    break;
                case kIdClose:
                case IDCANCEL: DestroyWindow(h); break;
            }
            return 0;
        case kAdded: {
            std::unique_ptr<WebFetchItem> item(reinterpret_cast<WebFetchItem*>(lp));
            w.items.push_back(*item);
            AddRow(*item);
            return 0;
        }
        case kUpdated: {
            std::unique_ptr<WebFetchItem> item(reinterpret_cast<WebFetchItem*>(lp));
            if (wp < w.items.size()) {
                w.items[wp] = *item;
                SetRow((int)wp, *item);
            }
            return 0;
        }
        case kNotice: {
            std::unique_ptr<std::string> text(reinterpret_cast<std::string*>(lp));
            SetWindowTextW(w.status, FromUtf8(*text).c_str());
            return 0;
        }
        case kDone: {
            std::unique_ptr<std::wstring> summary(reinterpret_cast<std::wstring*>(lp));
            if (w.worker.joinable()) w.worker.join();
            w.running = false;
            EnableWindow(w.save, TRUE);
            EnableWindow(w.stop, FALSE);
            SetWindowTextW(w.status, summary->c_str());
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY: {
            StopRun();
            MSG pending;
            for (UINT drain : {kAdded, kUpdated, kNotice, kDone})
                while (PeekMessageW(&pending, h, drain, drain, PM_REMOVE)) {
                    if (drain == kNotice) delete reinterpret_cast<std::string*>(pending.lParam);
                    else if (drain == kDone) delete reinterpret_cast<std::wstring*>(pending.lParam);
                    else delete reinterpret_cast<WebFetchItem*>(pending.lParam);
                }
            w.hwnd = nullptr;
            w.items.clear();
            ListView_DeleteAllItems(w.list);
            return 0;
        }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

void ShowWebFetch() {
    if (w.hwnd) {
        ShowWindow(w.hwnd, SW_RESTORE);
        SetForegroundWindow(w.hwnd);
        SetFocus(w.paste);
        return;
    }
    RegisterAppClass(kWebFetchClass, WebFetchProc);
    RECT r{0, 0, S(640), S(480)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_DLGMODALFRAME, g.dpi);
    RECT owner;
    GetWindowRect(g.hwnd, &owner);
    const int width = r.right - r.left, height = r.bottom - r.top;
    CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kWebFetchClass, L"Save music from the web",
                    WS_OVERLAPPEDWINDOW & ~WS_MINIMIZEBOX & ~WS_MAXIMIZEBOX | WS_CLIPCHILDREN,
                    owner.left + (owner.right - owner.left - width) / 2, owner.top + S(80), width, height, g.hwnd,
                    nullptr, g.inst, nullptr);
    if (!w.hwnd) return;
    ShowWindow(w.hwnd, SW_SHOW);
    SetFocus(w.paste);
}

bool WebFetchDialogMessage(MSG* msg) { return w.hwnd && IsDialogMessageW(w.hwnd, msg); }

void StopWebFetch() {
    if (w.hwnd) DestroyWindow(w.hwnd);
    StopRun();
}

}  // namespace app
