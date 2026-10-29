// The Organize files window: folder + naming pattern, a live preview of every move, and the move
// itself on a worker thread (folder watching paused meanwhile, so the moves aren't mistaken for
// new and missing files). Library > Undo last organize reverses the last run.
#include "app.h"
#include "organize.h"

namespace app {

namespace {

const wchar_t kOrganizeClass[] = L"CordinalOrganize";
// Long preview rebuilds are debounced while the user types in the folder/pattern fields.
constexpr UINT_PTR kPreviewTimer = 7;
enum { kIdRoot = 100, kIdBrowse, kIdPattern, kIdList, kIdReset };

// Single dialog instance, so its controls and its planned moves survive across messages. Owns the
// current track set, the plan shown in the list, and the worker that performs the moves.
struct Organizer {
    HWND hwnd = nullptr, rootLabel = nullptr, root = nullptr, browse = nullptr, patternLabel = nullptr, pattern = nullptr,
         reset = nullptr, help = nullptr, list = nullptr, summary = nullptr, go = nullptr, cancel = nullptr;
    std::vector<TrackId> ids;
    std::vector<OrganizeMove> plan;
} o;

std::thread g_mover;
std::unordered_set<TrackId> g_movingIds;  // the running move's tracks
bool g_moving = false;  // UI-thread flag: a run is in flight, so the dialog and undo are blocked

std::wstring LogDir() { return g.dataDir + L"\\organize"; }

// Rebuilds the whole plan for the current selection and field values, tallying the outcomes. The
// list view is owner-data, so only the count changes here; the rows are filled on demand.
void Replan() {
    o.plan = PlanOrganize(o.ids, WindowText(o.root), WindowText(o.pattern));
    size_t moving = 0, inPlace = 0, blocked = 0;
    for (auto& m : o.plan) {
        if (m.skip.empty() && (m.id == g.playing)) m.skip = L"playing now";
        if (m.skip.empty()) ++moving;
        else if (m.skip == L"already in place") ++inPlace;
        else ++blocked;
    }
    ListView_SetItemCountEx(o.list, (int)o.plan.size(), LVSICF_NOSCROLL);
    InvalidateRect(o.list, nullptr, FALSE);
    std::wstring s = std::to_wstring(moving) + (moving == 1 ? L" file will move" : L" files will move");
    if (inPlace) s += L"  \x2022  " + std::to_wstring(inPlace) + L" already in place";
    if (blocked) s += L"  \x2022  " + std::to_wstring(blocked) + L" can't move";
    if (WindowText(o.root).empty()) s = L"Choose the folder to organize into.";
    SetWindowTextW(o.summary, s.c_str());
    EnableWindow(o.go, moving > 0 && !WindowText(o.root).empty() && !g_moving);
}

void LayoutOrganizer() {
    RECT rc;
    GetClientRect(o.hwnd, &rc);
    int pad = S(14), lw = S(60), W = rc.right - pad * 2;
    MoveWindow(o.rootLabel, pad, pad + S(4), lw, S(20), TRUE);
    MoveWindow(o.root, pad + lw, pad, W - lw - S(104), S(24), TRUE);
    MoveWindow(o.browse, rc.right - pad - S(96), pad, S(96), S(26), TRUE);
    int y = pad + S(34);
    MoveWindow(o.patternLabel, pad, y + S(4), lw, S(20), TRUE);
    MoveWindow(o.pattern, pad + lw, y, W - lw - S(104), S(24), TRUE);
    MoveWindow(o.reset, rc.right - pad - S(96), y, S(96), S(26), TRUE);
    y += S(30);
    MoveWindow(o.help, pad + lw, y, W - lw, S(34), TRUE);
    y += S(40);
    int bottom = rc.bottom - pad - S(28) - S(10);
    MoveWindow(o.list, pad, y, W, bottom - y, TRUE);
    int half = (W - GetSystemMetrics(SM_CXVSCROLL) - S(110)) / 2;
    ListView_SetColumnWidth(o.list, 0, half);
    ListView_SetColumnWidth(o.list, 1, half);
    ListView_SetColumnWidth(o.list, 2, S(110));
    MoveWindow(o.summary, pad, rc.bottom - pad - S(24), W - S(230), S(22), TRUE);
    MoveWindow(o.go, rc.right - pad - S(216), rc.bottom - pad - S(28), S(108), S(28), TRUE);
    MoveWindow(o.cancel, rc.right - pad - S(100), rc.bottom - pad - S(28), S(100), S(28), TRUE);
}

void BrowseRoot() {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    DWORD options = 0;
    dlg->GetOptions(&options);
    dlg->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"Organize into this folder");
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dlg->Show(o.hwnd)) || FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        return;
    SetWindowTextW(o.root, path);
    CoTaskMemFree(path);
}

// Validates and confirms, then hands the plan to the worker and closes the dialog immediately -
// the result arrives later as WM_APP_ORGANIZE. The folder is added to `keep` so it survives the
// empty-folder cleanup even if all its files moved away.
void Start() {
    if (g_moving) return;
    std::wstring root = WindowText(o.root), pattern = WindowText(o.pattern);
    IniWrite(L"Organize", L"Folder", root);
    IniWrite(L"Organize", L"Pattern", pattern);
    Replan();
    size_t moving = std::count_if(o.plan.begin(), o.plan.end(), [](const OrganizeMove& m) { return m.skip.empty(); });
    std::wstring q = L"Move and rename " + std::to_wstring(moving) + (moving == 1 ? L" file" : L" files") + L" into\n" + root +
                     L"?\n\nYou can undo this with Library > Undo last organize.";
    if (MessageBoxW(o.hwnd, q.c_str(), kAppName, MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
    // The watcher would see the moves as files vanishing and appearing: pause it until done.
    g.scanner.Stop();
    g_moving = true;
    for (const auto& m : o.plan)
        if (m.skip.empty()) g_movingIds.insert(m.id);
    std::vector<std::wstring> keep = g.libraryFolders;
    keep.push_back(root);
    if (g_mover.joinable()) g_mover.join();
    g_mover = std::thread([hwnd = g.hwnd, plan = o.plan, logDir = LogDir(), keep]() {
        auto* r = new OrganizeResult(ExecuteOrganize(plan, logDir, keep));
        if (!PostMessageW(hwnd, WM_APP_ORGANIZE, 0, (LPARAM)r)) delete r;
    });
    UpdateStatus();
    DestroyWindow(o.hwnd);
}

LRESULT CALLBACK OrganizeProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            o.hwnd = h;
            auto make = [&](DWORD ex, const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
                HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, h, (HMENU)(INT_PTR)id, g.inst, nullptr);
                SendMessageW(c, WM_SETFONT, (WPARAM)g.font, TRUE);
                return c;
            };
            std::wstring root = IniRead(L"Organize", L"Folder", g.libraryFolders.empty() ? L"" : g.libraryFolders[0].c_str());
            std::wstring pattern = IniRead(L"Organize", L"Pattern", kDefaultOrganizePattern);
            o.rootLabel = make(0, WC_STATICW, L"Folder", SS_LEFT, 0);
            o.root = make(WS_EX_CLIENTEDGE, WC_EDITW, root.c_str(), ES_AUTOHSCROLL | WS_TABSTOP, kIdRoot);
            o.browse = make(0, WC_BUTTONW, L"&Browse...", BS_PUSHBUTTON | WS_TABSTOP, kIdBrowse);
            o.patternLabel = make(0, WC_STATICW, L"Names", SS_LEFT, 0);
            o.pattern = make(WS_EX_CLIENTEDGE, WC_EDITW, pattern.c_str(), ES_AUTOHSCROLL | WS_TABSTOP, kIdPattern);
            o.reset = make(0, WC_BUTTONW, L"&Default", BS_PUSHBUTTON | WS_TABSTOP, kIdReset);
            o.help = make(0, WC_STATICW,
                          L"Title formatting: %album artist%, %album%, %title%, %tracknumber%, %discnumber%, %year%, %genre% ...  "
                          L"A backslash starts a new folder; [ ] is left out when its fields are empty. The extension is kept.",
                          SS_LEFT | SS_NOPREFIX, 0);
            o.list = make(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", LVS_REPORT | LVS_OWNERDATA | LVS_NOSORTHEADER | WS_TABSTOP, kIdList);
            ListView_SetExtendedListViewStyle(o.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
            const wchar_t* cols[] = {L"From", L"To", L"Status"};
            for (int i = 0; i < 3; ++i) {
                LVCOLUMNW c{};
                c.mask = LVCF_TEXT | LVCF_WIDTH;
                c.pszText = const_cast<wchar_t*>(cols[i]);
                c.cx = S(200);
                ListView_InsertColumn(o.list, i, &c);
            }
            o.summary = make(0, WC_STATICW, L"", SS_LEFT | SS_NOPREFIX, 0);
            o.go = make(0, WC_BUTTONW, L"&Organize", BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK);
            o.cancel = make(0, WC_BUTTONW, L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL);
            for (HWND c : {o.root, o.pattern}) SetWindowTheme(c, g.dark ? L"DarkMode_CFD" : L"Explorer", nullptr);
            for (HWND c : {o.browse, o.reset, o.go, o.cancel, o.list}) ApplyControlTheme(c, g.dark);
            ListView_SetBkColor(o.list, Ref(P().surface2));
            ListView_SetTextBkColor(o.list, Ref(P().surface2));
            ListView_SetTextColor(o.list, TextColor());
            ApplyControlTheme(ListView_GetHeader(o.list), g.dark);
            ApplyWindowTheme(h, P());
            Replan();
            return 0;
        }
        case WM_SIZE:
            LayoutOrganizer();
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
            return SurfaceControlColor((HDC)wp, (HWND)lp == o.help || (HWND)lp == o.summary ? SubColor() : TextColor());
        case WM_CTLCOLOREDIT:
            return FieldControlColor((HDC)wp);
        case WM_NOTIFY: {
            auto* hdr = reinterpret_cast<NMHDR*>(lp);
            if (hdr->hwndFrom == o.list && hdr->code == LVN_GETDISPINFOW) {
                auto* di = reinterpret_cast<NMLVDISPINFOW*>(hdr);
                int row = di->item.iItem;
                if ((di->item.mask & LVIF_TEXT) && row >= 0 && row < (int)o.plan.size()) {
                    const auto& m = o.plan[row];
                    std::wstring root = WindowText(o.root);
                    // Long paths are shown relative to the chosen folder, with the root itself elided.
                    auto rel = [&](const std::wstring& p) {
                        return p.size() > root.size() && _wcsnicmp(p.c_str(), root.c_str(), root.size()) == 0 ? L"\x2026" + p.substr(root.size()) : p;
                    };
                    std::wstring text = di->item.iSubItem == 0 ? m.from : di->item.iSubItem == 1 ? rel(m.to) : m.skip.empty() ? L"move" : m.skip;
                    wcsncpy_s(di->item.pszText, di->item.cchTextMax, text.c_str(), _TRUNCATE);
                }
            }
            return 0;
        }
        case WM_TIMER:
            if (wp == kPreviewTimer) {
                KillTimer(h, kPreviewTimer);
                Replan();
            }
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case kIdRoot:
                case kIdPattern:
                    if (HIWORD(wp) == EN_CHANGE) SetTimer(h, kPreviewTimer, 300, nullptr);  // replan once typing pauses
                    break;
                case kIdBrowse: BrowseRoot(); break;
                case kIdReset: SetWindowTextW(o.pattern, kDefaultOrganizePattern); break;
                case IDOK: Start(); break;
                case IDCANCEL: DestroyWindow(h); break;
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            o.hwnd = nullptr;
            // The preview plan can hold one entry per library file (~tens of MB on a large
            // library when nothing is selected). Release it, and the id list, now that the
            // window is gone instead of keeping them until the process exits.
            std::vector<OrganizeMove>().swap(o.plan);
            std::vector<TrackId>().swap(o.ids);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

bool OrganizeDialogMessage(MSG* msg) { return o.hwnd && IsDialogMessageW(o.hwnd, msg); }
bool OrganizeBusy() { return g_moving; }
bool OrganizeUsing(TrackId id) { return g_movingIds.count(id) != 0; }

void ShowOrganize(const std::vector<TrackId>& ids) {
    if (ids.empty()) {
        ShowMessage(L"Nothing to organize.");
        return;
    }
    if (g_moving) return;
    if (o.hwnd) DestroyWindow(o.hwnd);
    RegisterAppClass(kOrganizeClass, OrganizeProc);
    o.ids = ids;
    RECT r{0, 0, S(900), S(560)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_DLGMODALFRAME, g.dpi);
    RECT owner;
    GetWindowRect(g.hwnd, &owner);
    int w = r.right - r.left, h = r.bottom - r.top;
    std::wstring title = L"Organize " + std::to_wstring(ids.size()) + (ids.size() == 1 ? L" file" : L" files");
    CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kOrganizeClass, title.c_str(),
                    WS_OVERLAPPEDWINDOW & ~WS_MINIMIZEBOX | WS_CLIPCHILDREN, owner.left + (owner.right - owner.left - w) / 2,
                    owner.top + S(60), w, h, g.hwnd, nullptr, g.inst, nullptr);
    ShowWindow(o.hwnd, SW_SHOW);
}

// UI thread, from WM_APP_ORGANIZE. Joins the finished worker, applies the new paths to the library,
// rebuilds the views, and restarts the folder watcher that Start() paused.
void OnOrganizeDone(LPARAM lp) {
    std::unique_ptr<OrganizeResult> r(reinterpret_cast<OrganizeResult*>(lp));
    if (g_mover.joinable()) g_mover.join();
    g_moving = false;
    std::unordered_set<TrackId>().swap(g_movingIds);
    for (auto& [id, path] : r->moved) g_lib.Rename(id, path);
    g.dirty = true;
    RebuildView();
    StartScanner();
    if (!r->failed.empty()) {
        std::wstring text = std::to_wstring(r->failed.size()) + L" file(s) couldn't be moved:\n\n";
        for (size_t i = 0; i < r->failed.size() && i < 8; ++i) text += r->failed[i].first + L"\n    " + r->failed[i].second + L"\n";
        MessageBoxW(g.hwnd, text.c_str(), kAppName, MB_OK | MB_ICONWARNING);
    } else if (!r->moved.empty()) {
        PlaySfx(Sfx::Organized);
    }
    ShowMessage(L"Organized " + std::to_wstring(r->moved.size()) + L" file(s).");
    UpdateStatus();
}

// Reverses the newest undo log and renames the affected tracks back. The watcher is paused for the
// duration, as in Start(), so the reverse moves don't read as new/missing files.
void UndoOrganize() {
    if (g_moving) return;
    if (MessageBoxW(g.hwnd, L"Move the files of the last organize back where they were?", kAppName,
                    MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        return;
    g.scanner.Stop();
    std::wstring error;
    auto undone = UndoLastOrganize(LogDir(), g.libraryFolders, error);
    for (auto& [from, to] : undone)
        if (TrackId id = g_lib.Find(to); id != kNone) g_lib.Rename(id, from);
    g.dirty = true;
    RebuildView();
    StartScanner();
    if (!error.empty()) PlaySfx(Sfx::Oops);
    else if (!undone.empty()) PlaySfx(Sfx::Unorganized);
    ShowMessage(error.empty() ? L"Moved " + std::to_wstring(undone.size()) + L" file(s) back." : error);
}

// Waits for any in-flight move to finish so the worker thread is gone before shutdown.
void StopOrganizer() {
    if (g_mover.joinable()) g_mover.join();
}

}  // namespace app
