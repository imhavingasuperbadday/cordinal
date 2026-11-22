// Quiet Release Radar (Library > New releases by your favorite artists...): recent MusicBrainz
// albums by the artists the library says someone cares about, offered as a small list to save.
// Nothing is asked until the list is opened, and the first time asks first; the check and every
// album's tracklist read run on a worker, one MusicBrainz request at a time, and a save goes
// through the same queue as a pasted link, landing as tagged files rather than a playlist tab.
#include "app.h"

#include "app_tools.h"
#include "release_radar.h"
#include "webfetch.h"

#include <memory>
#include <thread>

namespace app {

namespace {

const wchar_t kRadarClass[] = L"CordinalReleaseRadar";
constexpr UINT kRows = WM_APP + 1, kNotice = WM_APP + 2, kAlbums = WM_APP + 3, kDone = WM_APP + 4;
enum { kIdList = 100, kIdSave, kIdSaveAll, kIdCheck, kIdStatus, kIdClose };

struct Window {
    HWND hwnd = nullptr, list = nullptr, status = nullptr, save = nullptr, saveAll = nullptr,
         check = nullptr, close = nullptr;
    std::thread worker;
    std::shared_ptr<online::Cancel> cancel;
    bool running = false;
    std::vector<radar::Item> items;
} w;

void StopRun() {
    if (w.cancel) w.cancel->Stop();
    if (w.worker.joinable()) w.worker.join();
    w.cancel.reset();
    w.running = false;
}

void SetStatus(const std::wstring& text) {
    if (w.hwnd) SetWindowTextW(w.status, text.c_str());
}

void SetBusy(bool busy) {
    w.running = busy;
    for (HWND h : {w.save, w.saveAll, w.check}) EnableWindow(h, !busy && w.hwnd);
    if (w.list) EnableWindow(w.list, !busy);
}

const wchar_t* TypeText(const std::string& type) {
    if (type == "EP") return L"EP";
    return L"Album";
}

void AddRow(const radar::Item& item) {
    std::wstring artist = FromUtf8(item.artist), title = FromUtf8(item.title), when = FromUtf8(item.firstDate);
    LVITEMW row{LVIF_TEXT, (int)w.items.size()};
    row.pszText = artist.data();
    int at = ListView_InsertItem(w.list, &row);
    if (at < 0) return;
    ListView_SetItemText(w.list, at, 1, title.data());
    ListView_SetItemText(w.list, at, 2, const_cast<wchar_t*>(TypeText(item.type)));
    ListView_SetItemText(w.list, at, 3, when.data());
}

// The artists and their album titles are read once on the UI thread: the worker never touches the
// library. Each artist is then checked in turn and its new releases posted as they arrive.
void StartCheck() {
    if (!w.hwnd || w.running) return;
    std::vector<radar::Artist> artists = radar::TopArtists(g_lib, 8);
    if (artists.empty()) {
        SetStatus(L"The library has no artists to check yet.");
        return;
    }
    struct Job {
        std::string artist;
        std::vector<std::string> albums;
    };
    auto jobs = std::make_shared<std::vector<Job>>();
    for (const radar::Artist& artist : artists) jobs->push_back({artist.name, online::LibraryAlbumsBy(g_lib, artist.name)});
    w.items.clear();
    ListView_DeleteAllItems(w.list);
    SetBusy(true);
    SetStatus(L"Checking " + FromUtf8(jobs->front().artist) + L"...");
    w.cancel = std::make_shared<online::Cancel>();
    const HWND target = w.hwnd;
    auto cancel = w.cancel;
    const std::string since = radar::SinceDate(std::time(nullptr), 6);
    w.worker = std::thread([jobs, cancel, since, target] {
        auto post = [target](UINT msg, auto* owned) {
            if (!PostMessageW(target, msg, 0, (LPARAM)owned)) delete owned;
        };
        size_t found = 0;
        for (const Job& job : *jobs) {
            if (cancel->Stopped()) break;
            post(kNotice, new std::wstring(L"Checking " + FromUtf8(job.artist) + L"..."));
            std::vector<radar::Item> rows;
            if (!radar::RecentReleases(job.artist, since, rows, *cancel)) {
                post(kNotice, new std::wstring(L"MusicBrainz could not be asked about " + FromUtf8(job.artist) + L"."));
                continue;
            }
            radar::MarkOwned(rows, job.albums);
            std::erase_if(rows, [](const radar::Item& item) { return item.owned; });
            found += rows.size();
            if (!rows.empty()) post(kRows, new std::vector<radar::Item>(std::move(rows)));
        }
        std::wstring done = cancel->Stopped() ? L"Stopped."
                            : found == 0 ? L"Nothing new from those artists right now."
                                         : L"Found " + std::to_wstring(found) + (found == 1 ? L" new release." : L" new releases.");
        post(kDone, new std::wstring(std::move(done)));
    });
}

// Reads the chosen releases' tracklists on a worker, then hands the albums to the save queue.
void SaveItems(std::vector<radar::Item> items) {
    if (!w.hwnd || w.running || items.empty()) return;
    SetBusy(true);
    SetStatus(L"Reading the albums...");
    w.cancel = std::make_shared<online::Cancel>();
    const HWND target = w.hwnd;
    auto cancel = w.cancel;
    w.worker = std::thread([items = std::move(items), cancel, target] {
        auto post = [target](UINT msg, auto* owned) {
            if (!PostMessageW(target, msg, 0, (LPARAM)owned)) delete owned;
        };
        auto albums = new std::vector<radar::Album>();
        size_t missing = 0;
        for (const radar::Item& item : items) {
            if (cancel->Stopped()) break;
            radar::Album album;
            if (radar::FetchAlbum(item, album, *cancel)) albums->push_back(std::move(album));
            else ++missing;
        }
        if (missing)
            post(kNotice, new std::wstring(L"MusicBrainz had no tracklist for " + std::to_wstring(missing) +
                                           (missing == 1 ? L" release." : L" releases.")));
        post(kAlbums, albums);
        post(kDone, new std::wstring(L"Starting the save..."));
    });
}

std::vector<radar::Item> SelectedItems() {
    std::vector<radar::Item> items;
    for (int row = -1; (row = ListView_GetNextItem(w.list, row, LVNI_SELECTED)) >= 0;)
        if (row < (int)w.items.size()) items.push_back(w.items[row]);
    return items;
}

void LayoutWindow() {
    if (!w.hwnd) return;
    RECT rc;
    GetClientRect(w.hwnd, &rc);
    const int pad = S(14), rowH = S(30), bottom = rc.bottom - pad - rowH;
    SetWindowPos(w.list, nullptr, pad, pad, rc.right - pad * 2, std::max(S(80), bottom - pad - S(30)), SWP_NOZORDER);
    SetWindowPos(w.status, nullptr, pad, bottom - S(24), rc.right - pad * 2, S(20), SWP_NOZORDER);
    int x = pad;
    for (HWND b : {w.save, w.saveAll, w.check}) {
        SetWindowPos(b, nullptr, x, bottom, S(140), S(30), SWP_NOZORDER);
        x += S(146);
    }
    SetWindowPos(w.close, nullptr, rc.right - pad - S(100), bottom, S(100), S(30), SWP_NOZORDER);
}

LRESULT CALLBACK RadarProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            w.hwnd = h;
            auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
                HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, h,
                                         (HMENU)(INT_PTR)id, g.inst, nullptr);
                SendMessageW(c, WM_SETFONT, (WPARAM)g.font, TRUE);
                return c;
            };
            w.list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                     WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER |
                                         WS_TABSTOP,
                                     0, 0, 0, 0, h, (HMENU)(INT_PTR)kIdList, g.inst, nullptr);
            SendMessageW(w.list, WM_SETFONT, (WPARAM)g.font, TRUE);
            ListView_SetExtendedListViewStyle(w.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            const wchar_t* columns[] = {L"Artist", L"Release", L"Type", L"First release"};
            const int widths[] = {S(170), S(260), S(70), S(100)};
            for (int c = 0; c < 4; ++c) {
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
            w.status = make(WC_STATICW, L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, kIdStatus);
            w.save = make(WC_BUTTONW, L"&Save selected", BS_PUSHBUTTON | WS_TABSTOP, kIdSave);
            w.saveAll = make(WC_BUTTONW, L"Save &all", BS_PUSHBUTTON | WS_TABSTOP, kIdSaveAll);
            w.check = make(WC_BUTTONW, L"Check a&gain", BS_PUSHBUTTON | WS_TABSTOP, kIdCheck);
            w.close = make(WC_BUTTONW, L"Close", BS_PUSHBUTTON | WS_TABSTOP, kIdClose);
            for (HWND c : {w.save, w.saveAll, w.check, w.close}) ApplyControlTheme(c, g.dark);
            ApplyWindowTheme(h, P());
            LayoutWindow();
            return 0;
        }
        case WM_SIZE:
            LayoutWindow();
            return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {S(560), S(360)};
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
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case kIdSave: SaveItems(SelectedItems()); return 0;
                case kIdSaveAll: SaveItems(w.items); return 0;
                case kIdCheck: StartCheck(); return 0;
                case kIdClose: DestroyWindow(h); return 0;
                default: break;
            }
            break;
        case kRows: {
            std::unique_ptr<std::vector<radar::Item>> rows(reinterpret_cast<std::vector<radar::Item>*>(lp));
            if (!rows) return 0;
            for (const radar::Item& item : *rows) {
                w.items.push_back(item);
                AddRow(item);
            }
            SetBusy(true);
            return 0;
        }
        case kNotice: {
            std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lp));
            if (text) SetStatus(*text);
            return 0;
        }
        case kDone: {
            std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lp));
            if (text) SetStatus(*text);
            if (w.worker.joinable()) w.worker.join();
            w.worker = std::thread();
            w.cancel.reset();
            SetBusy(false);
            return 0;
        }
        case kAlbums: {
            std::unique_ptr<std::vector<radar::Album>> albums(reinterpret_cast<std::vector<radar::Album>*>(lp));
            if (!albums || albums->empty()) return 0;
            std::vector<WebFetchTrackList> lists;
            for (const radar::Album& album : *albums) lists.push_back(radar::AlbumToWebFetchList(album));
            ShowWebFetchLists(std::move(lists));
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            StopRun();
            MSG pending;
            for (UINT drain : {kRows, kNotice, kAlbums, kDone})
                while (PeekMessageW(&pending, h, drain, drain, PM_REMOVE)) {
                    if (drain == kRows) delete reinterpret_cast<std::vector<radar::Item>*>(pending.lParam);
                    else if (drain == kNotice) delete reinterpret_cast<std::wstring*>(pending.lParam);
                    else if (drain == kAlbums) delete reinterpret_cast<std::vector<radar::Album>*>(pending.lParam);
                    else delete reinterpret_cast<std::wstring*>(pending.lParam);
                }
            w.hwnd = nullptr;
            w.items.clear();
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// The first look asks what would be sent, the way the first save does; the answer lives in
// Preferences > Online and can be changed there.
bool EnsureReleaseRadar() {
    if (online::Config().releaseRadar) return true;
    const wchar_t* text =
        L"Release Radar is off, so cordinal hasn't contacted anyone.\n\n"
        L"It asks MusicBrainz about the artists you play and heart most, and lists their albums from "
        L"the last six months so you can save any you want as your own files. No account is needed "
        L"and nothing is downloaded until you choose Save.\n\nTurn Release Radar on? It can be changed "
        L"in Preferences > Online.";
    if (MessageBoxW(g.hwnd, text, L"Release Radar", MB_YESNO | MB_ICONQUESTION) != IDYES) return false;
    if (const Tweak* t = FindTweak("online.release_radar")) SetTweak(*t, 1);
    return online::Config().releaseRadar;
}

}  // namespace

void ShowReleaseRadar() {
    if (w.hwnd) {
        ShowWindow(w.hwnd, SW_RESTORE);
        SetForegroundWindow(w.hwnd);
        return;
    }
    if (!EnsureReleaseRadar()) return;
    RegisterAppClass(kRadarClass, RadarProc);
    RECT r{0, 0, S(680), S(460)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_DLGMODALFRAME, g.dpi);
    RECT owner;
    GetWindowRect(g.hwnd, &owner);
    const int width = r.right - r.left, height = r.bottom - r.top;
    CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kRadarClass, L"New releases",
                    WS_OVERLAPPEDWINDOW & ~WS_MINIMIZEBOX & ~WS_MAXIMIZEBOX | WS_CLIPCHILDREN,
                    owner.left + (owner.right - owner.left - width) / 2, owner.top + S(80), width, height, g.hwnd,
                    nullptr, g.inst, nullptr);
    if (!w.hwnd) return;
    ShowWindow(w.hwnd, SW_SHOW);
    StartCheck();
}

bool ReleaseRadarDialogMessage(MSG* msg) { return w.hwnd && IsDialogMessageW(w.hwnd, msg); }

void StopReleaseRadar() {
    if (w.hwnd) DestroyWindow(w.hwnd);
    StopRun();
}

}  // namespace app
