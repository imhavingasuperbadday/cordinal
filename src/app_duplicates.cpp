// Library > Find duplicates: the copies of each song in the library, grouped (duplicates.h), with the
// ones to remove checked. Identical audio is checked for you, keeping the best copy; the same song in
// other files is only suggested, since a lossy copy may be wanted on purpose. Removing sends the
// checked files to the Recycle Bin on a worker, then moves their plays, ratings and playlist places to
// the copy kept; Undo clean-up brings the files back with their history. Songs not measured yet have
// no audio hash, so the window offers to measure them (ReplayGain's scan, which hashes on the way).
// The Linux front end has the same window in src/linux/duplicates_ui.cpp.
#include "app.h"
#include "convert.h"
#include "duplicates.h"

#include <memory>
#include <thread>

namespace app {

namespace {

const wchar_t kDuplicatesClass[] = L"CordinalDuplicates";
constexpr UINT kDone = WM_APP + 1;

enum Id : int { kList = 100, kMeasure, kRemove, kUndo };

// A clean-up or its undo. The worker fills `done` (which files moved); the UI thread reads it only
// after joining the worker.
struct Work {
    bool undo = false;
    std::vector<duplicates::Move> moves;  // undo: g_cleanup.merged's, in its order
    std::vector<bool> done;
};

struct Window {
    HWND hwnd = nullptr, status = nullptr, measure = nullptr, list = nullptr, info = nullptr, remove = nullptr, undo = nullptr,
         close = nullptr;
    std::vector<duplicates::Group> groups;
    std::vector<std::pair<size_t, size_t>> rows;  // list row -> (group, copy)
    size_t unmeasured = 0;
    bool measuring = false;
    std::unique_ptr<Work> work;
    std::thread worker;
} w;

duplicates::Cleanup g_cleanup;  // the last clean-up, for Undo clean-up (this session only)

std::wstring Count(size_t n, const wchar_t* one, const wchar_t* many) {
    return std::to_wstring(n) + L" " + (n == 1 ? one : many);
}

bool Busy() { return w.worker.joinable(); }

void EnableButtons() {
    EnableWindow(w.remove, !Busy() && !w.rows.empty());
    EnableWindow(w.undo, !Busy() && !g_cleanup.Empty());
    EnableWindow(w.measure, !Busy() && !w.measuring && w.unmeasured);
}

void Populate() {
    w.groups = duplicates::Find(g_lib);
    w.unmeasured = duplicates::Unmeasured(g_lib).size();
    w.rows.clear();
    SendMessageW(w.list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(w.list);
    ListView_RemoveAllGroups(w.list);
    for (size_t gi = 0; gi < w.groups.size(); ++gi) {
        const auto& group = w.groups[gi];
        const Track& best = g_lib[group.tracks[0]];
        std::wstring title = g_lib.Str(best.title ? best.title : best.file);
        if (best.artist) title += L"  \x2014  " + g_lib.Str(best.artist);
        std::wstring header = (group.identical ? L"Identical audio: " : L"Same song: ") + title;
        LVGROUP lg{sizeof lg};
        lg.mask = LVGF_HEADER | LVGF_GROUPID;
        lg.pszHeader = header.data();
        lg.iGroupId = (int)gi;
        ListView_InsertGroup(w.list, -1, &lg);
        for (size_t ci = 0; ci < group.tracks.size(); ++ci) {
            const TrackId id = group.tracks[ci];
            const Track& t = g_lib[id];
            std::wstring song = g_lib.Str(t.title ? t.title : t.file);
            if (ci == 0) song += L"  (best copy)";
            LVITEMW item{};
            item.mask = LVIF_TEXT | LVIF_GROUPID;
            item.iItem = (int)w.rows.size();
            item.iGroupId = (int)gi;
            item.pszText = song.data();
            const int row = ListView_InsertItem(w.list, &item);
            std::wstring format = duplicates::FormatText(t), length = FormatDuration(t.Duration()),
                         plays = std::to_wstring(t.playCount), folder = g_lib.Str(t.dir);
            ListView_SetItemText(w.list, row, 1, format.data());
            ListView_SetItemText(w.list, row, 2, length.data());
            ListView_SetItemText(w.list, row, 3, plays.data());
            ListView_SetItemText(w.list, row, 4, folder.data());
            // Identical copies are safe to remove; another encode of the song is the listener's call.
            ListView_SetCheckState(w.list, row, group.identical && ci > 0);
            w.rows.push_back({gi, ci});
        }
    }
    SendMessageW(w.list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(w.list, nullptr, TRUE);
    SetWindowTextW(w.status, w.groups.empty() ? L"No duplicates found."
                                              : (Count(w.groups.size(), L"song has", L"songs have") + L" more than one copy.").c_str());
    std::wstring measure;
    if (w.measuring) measure = L"Measuring\x2026";
    else if (w.unmeasured) measure = Count(w.unmeasured, L"song", L"songs") + L" not measured yet: Measure them to compare by sound.";
    SetWindowTextW(w.info, measure.empty() ? L"Checked copies go to the Recycle Bin; their history moves to the copy kept."
                                           : measure.c_str());
    EnableButtons();
}

void ApplyWork(Work& work) {
    const size_t done = (size_t)std::count(work.done.begin(), work.done.end(), true);
    std::wstring text;
    if (!work.undo) {
        g_cleanup = duplicates::Merge(g_lib, g.playlists, work.moves, work.done);
        text = Count(done, L"copy", L"copies") + L" moved to the Recycle Bin.";
    } else {
        size_t skipped = 0;
        QueueTags(duplicates::Undo(g_lib, g.playlists, g_cleanup, work.done, &skipped));
        g_cleanup = {};
        text = Count(done, L"copy", L"copies") + L" restored.";
        if (skipped) text += L" " + Count(skipped, L"playlist", L"playlists") + L" changed since.";
    }
    if (done < work.done.size())
        text += L" " + Count(work.done.size() - done, L"file", L"files") +
                (work.undo ? L" stayed in the Recycle Bin." : L" couldn't be moved.");
    g.dirty = true;
    if (g.closing) return;  // shutting down: the library is updated and saved next
    ClearCellCache();
    RebuildView(true);
    ShowMessage(text);
}

// The worker's result: applied here, or in WM_DESTROY when the window closes first, so files that
// already moved always take their history with them.
void Finish() {
    if (!w.worker.joinable()) return;
    w.worker.join();
    std::unique_ptr<Work> work = std::move(w.work);
    if (work) ApplyWork(*work);
}

void Start(std::unique_ptr<Work> work) {
    w.work = std::move(work);
    w.worker = std::thread([work = w.work.get(), hwnd = w.hwnd] {
        work->done.assign(work->moves.size(), false);
        for (size_t i = 0; i < work->moves.size(); ++i)
            work->done[i] = work->undo ? RestoreFromRecycleBin(work->moves[i].extra) : RecycleFile(work->moves[i].extra);
        PostMessageW(hwnd, kDone, 0, 0);
    });
    EnableButtons();
}

void RemoveTicked() {
    if (Busy()) return;
    if (TagWriteBusy() || ConvertBusy()) {
        ShowMessage(L"Tags are still being written; try again in a moment.");  // a conversion rewrites them too
        return;
    }
    std::vector<std::vector<bool>> ticks(w.groups.size());
    for (size_t gi = 0; gi < w.groups.size(); ++gi) ticks[gi].assign(w.groups[gi].tracks.size(), false);
    for (size_t row = 0; row < w.rows.size(); ++row)
        ticks[w.rows[row].first][w.rows[row].second] = ListView_GetCheckState(w.list, (int)row) != FALSE;
    size_t whole = 0;
    auto work = std::make_unique<Work>();
    work->moves = duplicates::Plan(g_lib, w.groups, ticks, g.playing, &whole);
    if (work->moves.empty()) {
        ShowMessage(whole ? L"Leave one copy of each song unchecked." : L"Check the tracks to remove first.");
        return;
    }
    std::wstring q = L"Move " + Count(work->moves.size(), L"file", L"files") +
                     L" to the Recycle Bin?\n\nTheir plays, ratings and playlist places move to the copy kept.";
    if (whole) q += L" " + Count(whole, L"song", L"songs") + L" with every copy checked stay.";
    if (MessageBoxW(w.hwnd, q.c_str(), L"Find duplicates", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
    Start(std::move(work));
}

void Layout(HWND h) {
    RECT rc;
    GetClientRect(h, &rc);
    const int pad = S(14), width = rc.right - pad * 2, by = rc.bottom - pad - S(28);
    MoveWindow(w.status, pad, pad, width - S(150), S(22), TRUE);
    MoveWindow(w.measure, rc.right - pad - S(140), pad - S(3), S(140), S(28), TRUE);
    MoveWindow(w.list, pad, pad + S(32), width, std::max(S(80), by - S(40) - pad - S(32)), TRUE);
    MoveWindow(w.info, pad, by - S(32), width, S(22), TRUE);
    MoveWindow(w.undo, pad, by, S(130), S(28), TRUE);
    MoveWindow(w.remove, rc.right - pad - S(300), by, S(192), S(28), TRUE);
    MoveWindow(w.close, rc.right - pad - S(100), by, S(100), S(28), TRUE);
    const int widths[] = {width * 2 / 7, S(120), S(64), S(52)};
    int used = 0;
    for (int c = 0; c < 4; ++c) {
        ListView_SetColumnWidth(w.list, c, widths[c]);
        used += widths[c];
    }
    ListView_SetColumnWidth(w.list, 4, std::max(S(120), width - used - GetSystemMetrics(SM_CXVSCROLL) - S(6)));
}

LRESULT CALLBACK DuplicatesProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            w.hwnd = h;
            auto make = [&](DWORD ex, const wchar_t* cls, const wchar_t* text, DWORD style, int id = 0) {
                HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, h, (HMENU)(INT_PTR)id,
                                         g.inst, nullptr);
                SendMessageW(c, WM_SETFONT, (WPARAM)g.font, TRUE);
                return c;
            };
            w.status = make(0, WC_STATICW, L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS);
            w.measure = make(0, WC_BUTTONW, L"&Measure them", BS_PUSHBUTTON | WS_TABSTOP, kMeasure);
            w.list = make(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER | WS_TABSTOP, kList);
            ListView_SetExtendedListViewStyle(w.list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            ListView_EnableGroupView(w.list, TRUE);
            const wchar_t* columns[] = {L"Song", L"Format", L"Length", L"Plays", L"Folder"};
            for (int c = 0; c < 5; ++c) {
                LVCOLUMNW col{};
                col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
                col.fmt = c == 2 || c == 3 ? LVCFMT_RIGHT : LVCFMT_LEFT;
                col.cx = S(100);
                col.pszText = const_cast<wchar_t*>(columns[c]);
                ListView_InsertColumn(w.list, c, &col);
            }
            for (HWND c : {w.list, ListView_GetHeader(w.list)}) ApplyControlTheme(c, g.dark);
            ListView_SetBkColor(w.list, Ref(P().surface2));
            ListView_SetTextBkColor(w.list, Ref(P().surface2));
            ListView_SetTextColor(w.list, TextColor());
            w.info = make(0, WC_STATICW, L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS);
            w.undo = make(0, WC_BUTTONW, L"&Undo clean-up", BS_PUSHBUTTON | WS_TABSTOP, kUndo);
            w.remove = make(0, WC_BUTTONW, L"Move checked to &Recycle Bin", BS_DEFPUSHBUTTON | WS_TABSTOP, kRemove);
            w.close = make(0, WC_BUTTONW, L"Close", BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL);
            for (HWND c : {w.measure, w.undo, w.remove, w.close}) ApplyControlTheme(c, g.dark);
            ApplyWindowTheme(h, P());
            return 0;
        }
        case WM_SIZE:
            Layout(h);
            return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {S(640), S(400)};
            return 0;
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(h, &rc);
            FillRect((HDC)wp, &rc, g.surfaceBrush);
            return 1;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            return SurfaceControlColor((HDC)wp, (HWND)lp == w.info ? SubColor() : TextColor());
        case kDone:
            Finish();
            Populate();
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case kMeasure: {
                    if (g.rgRunning && !g.rgAuto) {
                        ShowMessage(L"Wait for the ReplayGain scan to finish.");
                        break;
                    }
                    ScanReplayGain(duplicates::Unmeasured(g_lib));
                    w.measuring = g.rgRunning;
                    Populate();
                    break;
                }
                case kRemove: RemoveTicked(); break;
                case kUndo:
                    if (!Busy() && !g_cleanup.Empty()) {
                        auto work = std::make_unique<Work>();
                        work->undo = true;
                        for (const auto& m : g_cleanup.merged) work->moves.push_back({m.extra, m.keep});
                        Start(std::move(work));
                    }
                    break;
                case IDCANCEL: DestroyWindow(h); break;
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            Finish();  // files already moved take their history with them
            w.hwnd = nullptr;
            w.groups.clear();
            w.rows.clear();
            w.measuring = false;
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

bool DuplicatesDialogMessage(MSG* msg) { return w.hwnd && IsDialogMessageW(w.hwnd, msg) != FALSE; }

void StopDuplicates() { Finish(); }

void DuplicatesMeasured() {
    if (!w.hwnd || !w.measuring) return;
    w.measuring = false;
    Populate();
}

void ShowDuplicates() {
    if (w.hwnd) {
        SetForegroundWindow(w.hwnd);
        return;
    }
    RegisterAppClass(kDuplicatesClass, DuplicatesProc);
    RECT r{0, 0, S(860), S(560)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_DLGMODALFRAME, g.dpi);
    RECT owner;
    GetWindowRect(g.hwnd, &owner);
    const int width = r.right - r.left, height = r.bottom - r.top;
    CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kDuplicatesClass, L"Find duplicates",
                    WS_OVERLAPPEDWINDOW & ~WS_MINIMIZEBOX | WS_CLIPCHILDREN, owner.left + (owner.right - owner.left - width) / 2,
                    owner.top + S(40), width, height, g.hwnd, nullptr, g.inst, nullptr);
    if (!w.hwnd) return;
    Populate();
    ShowWindow(w.hwnd, SW_SHOW);
    SetFocus(w.list);
}

}  // namespace app
