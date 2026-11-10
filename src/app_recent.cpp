// Library > Remove recent additions: a modeless list of the tracks added in the last 24 hours, with
// checkboxes. Checked tracks leave the library the way the Library tab's Remove does; their files
// stay on disk. The list is filled from the library when the window opens and after each removal.
#include "app.h"
#include <algorithm>

namespace app {
namespace {

constexpr uint32_t kRecentSeconds = 24 * 60 * 60;
constexpr int kIdList = 3100, kIdHint = 3101, kIdAll = 3102, kIdRemove = 3103;

struct RecentWindow {
    HWND hwnd = nullptr, list = nullptr, hint = nullptr, all = nullptr, remove = nullptr;
    std::vector<TrackId> ids;  // one per list row, in row order
} w;

std::wstring AgeText(uint32_t added, uint32_t now) {
    uint32_t age = now > added ? now - added : 0;
    if (age < 3600) return std::to_wstring(std::max<uint32_t>(1, age / 60)) + L" min ago";
    return std::to_wstring(age / 3600) + L" h ago";
}

// The rows the window shows: every track added in the last 24 hours, newest first.
void Populate() {
    ListView_DeleteAllItems(w.list);
    const uint32_t now = UnixNow();
    w.ids = TracksAddedSince(now > kRecentSeconds ? now - kRecentSeconds : 0);
    std::reverse(w.ids.begin(), w.ids.end());
    for (size_t row = 0; row < w.ids.size(); ++row) {
        const Track& t = g_lib[w.ids[row]];
        std::wstring title = g_lib.Str(t.title ? t.title : t.file);
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = (int)row;
        item.pszText = title.data();
        ListView_InsertItem(w.list, &item);
        std::wstring artist = g_lib.Str(t.artist), album = g_lib.Str(t.album), age = AgeText(t.added, now);
        ListView_SetItemText(w.list, (int)row, 1, artist.data());
        ListView_SetItemText(w.list, (int)row, 2, album.data());
        ListView_SetItemText(w.list, (int)row, 3, age.data());
    }
    const std::wstring hint =
        w.ids.empty() ? L"No tracks were added in the last 24 hours."
                      : std::to_wstring(w.ids.size()) + (w.ids.size() == 1 ? L" track was" : L" tracks were") +
                            L" added in the last 24 hours. Check the ones to remove from your library; their files stay on disk.";
    SetWindowTextW(w.hint, hint.c_str());
    EnableWindow(w.all, !w.ids.empty());
    EnableWindow(w.remove, !w.ids.empty());
}

void CheckAll() {
    for (int row = 0; row < (int)w.ids.size(); ++row) ListView_SetCheckState(w.list, row, TRUE);
}

void RemoveChecked() {
    std::vector<TrackId> chosen;
    for (int row = 0; row < (int)w.ids.size(); ++row)
        if (ListView_GetCheckState(w.list, row)) chosen.push_back(w.ids[row]);
    if (chosen.empty()) {
        ShowMessage(L"Check the tracks to remove first.");
        return;
    }
    std::wstring q = L"Remove " + std::to_wstring(chosen.size()) + (chosen.size() == 1 ? L" track" : L" tracks") +
                     L" from the library?\n\nThe files stay on disk and won't be added back by folder scans.";
    if (MessageBoxW(w.hwnd, q.c_str(), kAppName, MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    ExcludeTracks(chosen);
    g.dirty = true;
    RebuildView();
    Populate();
}

LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            w.hwnd = h;
            auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
                HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, h,
                                         (HMENU)(INT_PTR)id, g.inst, nullptr);
                SendMessageW(c, WM_SETFONT, (WPARAM)g.font, TRUE);
                return c;
            };
            w.hint = make(WC_STATICW, L"", SS_LEFT, kIdHint);
            w.list = make(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SHOWSELALWAYS | WS_TABSTOP, kIdList);
            ListView_SetExtendedListViewStyle(w.list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            const wchar_t* columns[] = {L"Title", L"Artist", L"Album", L"Added"};
            const int widths[] = {S(240), S(160), S(160), S(90)};
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
            w.all = make(WC_BUTTONW, L"Check &all", WS_TABSTOP, kIdAll);
            w.remove = make(WC_BUTTONW, L"&Remove checked", WS_TABSTOP | BS_DEFPUSHBUTTON, kIdRemove);
            HWND close = make(WC_BUTTONW, L"Close", WS_TABSTOP, IDCANCEL);
            for (HWND c : {w.all, w.remove, close}) ApplyControlTheme(c, g.dark);
            ApplyWindowTheme(h, P());
            Populate();
            return 0;
        }
        case WM_SIZE: {
            RECT r;
            GetClientRect(h, &r);
            int pad = S(12), width = r.right - pad * 2;
            MoveWindow(w.hint, pad, pad, width, S(40), TRUE);
            MoveWindow(w.list, pad, S(58), width, std::max(S(80), (int)r.bottom - S(110)), TRUE);
            MoveWindow(w.all, pad, r.bottom - S(40), S(120), S(28), TRUE);
            MoveWindow(w.remove, r.right - S(266), r.bottom - S(40), S(170), S(28), TRUE);
            MoveWindow(GetDlgItem(h, IDCANCEL), r.right - S(88), r.bottom - S(40), S(76), S(28), TRUE);
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDCANCEL: DestroyWindow(h); break;
                case kIdAll: CheckAll(); break;
                case kIdRemove: RemoveChecked(); break;
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            w.hwnd = nullptr;
            w.ids.clear();
            return 0;
        case WM_ERASEBKGND: {
            RECT r;
            GetClientRect(h, &r);
            FillRect((HDC)wp, &r, g.surfaceBrush);
            return 1;
        }
        case WM_CTLCOLORSTATIC:
            return SurfaceControlColor((HDC)wp, TextColor());
        case WM_GETMINMAXINFO:
            ((MINMAXINFO*)lp)->ptMinTrackSize = {S(560), S(320)};
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

void ShowRecentAdditions() {
    if (w.hwnd) {
        SetForegroundWindow(w.hwnd);
        return;
    }
    RegisterAppClass(L"CordinalRecentAdditions", Proc);
    RECT r{0, 0, S(680), S(420)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_DLGMODALFRAME, g.dpi);
    CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, L"CordinalRecentAdditions",
                    L"Remove recent additions", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                    r.right - r.left, r.bottom - r.top, g.hwnd, nullptr, g.inst, nullptr);
    ShowWindow(w.hwnd, SW_SHOW);
}

bool RecentAdditionsDialogMessage(MSG* msg) { return w.hwnd && IsDialogMessageW(w.hwnd, msg); }

}  // namespace app
