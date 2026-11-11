// The album maker (Edit > Make album..., and the track menu): turns the selected tracks into one
// album, or tidies an existing one. The title, album artist, year, genre, disc, compilation flag and
// cover apply to every track, and the track list sets their order and numbers. Saving is one tag
// write through the tag writer, so Edit > Undo last tag edit takes the whole album back.
//
// The window is modeless: more tracks can be selected in the main window and added with Add
// selected. Its model (album_maker.h) is shared with the Linux album maker.
#include "album_maker.h"
#include "app.h"

namespace app {

namespace {

const wchar_t kAlbumClass[] = L"CordinalAlbumMaker";

enum Id : int {
    kTitle = 100, kArtist, kDate, kGenre, kDisc, kDiscTotal,
    kCompilation = 200, kArtistToTracks, kNumber,
    kList = 300, kUp, kDown, kRemove, kAddSelected, kOrder,
    kCoverFile = 400, kCoverOnline, kCoverRemove,
};

struct Maker {
    HWND hwnd = nullptr;
    AlbumDraft draft;
    HWND labels[6] = {}, edits[6] = {};  // kTitle.. kDiscTotal
    HWND compilation = nullptr, artistToTracks = nullptr, number = nullptr, list = nullptr, tracksLabel = nullptr;
    HWND up = nullptr, down = nullptr, remove = nullptr, addSelected = nullptr, order = nullptr;
    HWND art = nullptr, coverFile = nullptr, coverOnline = nullptr, coverRemove = nullptr;
    HWND info = nullptr, save = nullptr, cancel = nullptr;
    ArtImage preview;  // the cover the tracks will have
    bool loading = false;
} m;

const wchar_t* const kLabels[] = {L"&Album", L"Album a&rtist", L"&Year", L"&Genre", L"&Disc", L"of"};

std::wstring Text(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(n, L'\0');
    if (n) GetWindowTextW(h, s.data(), n + 1);
    return s;
}

uint32_t Number(HWND h) { return (uint32_t)std::max(0, _wtoi(Text(h).c_str())); }

// The fields back into the draft; the list keeps it current itself.
void ReadFields() {
    m.draft.title = Text(m.edits[0]);
    m.draft.artist = Text(m.edits[1]);
    m.draft.date = Text(m.edits[2]);
    m.draft.genre = Text(m.edits[3]);
    m.draft.disc = Number(m.edits[4]);
    m.draft.discTotal = Number(m.edits[5]);
    m.draft.compilation = Button_GetCheck(m.compilation) == BST_CHECKED;
    m.draft.artistToTracks = Button_GetCheck(m.artistToTracks) == BST_CHECKED;
    m.draft.numberTracks = Button_GetCheck(m.number) == BST_CHECKED;
}

std::vector<size_t> ListSelection() {
    std::vector<size_t> out;
    int count = (int)SendMessageW(m.list, LB_GETSELCOUNT, 0, 0);
    if (count <= 0) return out;
    std::vector<int> rows(count);
    SendMessageW(m.list, LB_GETSELITEMS, count, (LPARAM)rows.data());
    for (int r : rows) out.push_back((size_t)r);
    std::sort(out.begin(), out.end());
    return out;
}

// What Save will do, or what keeps it from saving.
void UpdateInfo() {
    ReadFields();
    std::wstring problem = AlbumDraftProblem(m.draft), text;
    if (!problem.empty()) {
        text = problem;
    } else {
        size_t files = AlbumEdits(g_lib, m.draft).size();
        text = files == 0 ? L"These tracks already make this album."
                          : L"Saving writes the tags of " + std::to_wstring(files) + (files == 1 ? L" file" : L" files") +
                                L". Edit > Undo last tag edit takes it back.";
    }
    SetWindowTextW(m.info, text.c_str());
    EnableWindow(m.save, problem.empty());
    auto selection = ListSelection();
    bool any = !selection.empty();
    EnableWindow(m.up, any && selection.front() > 0);
    EnableWindow(m.down, any && selection.back() + 1 < m.draft.tracks.size());
    EnableWindow(m.remove, any);
    EnableWindow(m.coverRemove, m.draft.cover ? !m.draft.cover->empty() : !m.preview.Empty());
    double seconds = 0;
    for (TrackId id : m.draft.tracks) seconds += g_lib[id].Duration();
    std::wstring tracks = L"&Tracks  (" + std::to_wstring(m.draft.tracks.size()) + L", " + FormatTotalDuration(seconds) + L")";
    SetWindowTextW(m.tracksLabel, tracks.c_str());
}

// The list shows the album as it will be numbered.
void FillList(const std::vector<size_t>& select) {
    SendMessageW(m.list, WM_SETREDRAW, FALSE, 0);
    SendMessageW(m.list, LB_RESETCONTENT, 0, 0);
    const bool number = Button_GetCheck(m.number) == BST_CHECKED;
    for (size_t i = 0; i < m.draft.tracks.size(); ++i) {
        const Track& t = g_lib[m.draft.tracks[i]];
        uint32_t n = number ? (uint32_t)i + 1 : t.trackNo;
        std::wstring row = (n ? std::to_wstring(n) + L".\t" : L"\t") +
                           g_lib.Str(t.title ? t.title : t.file);
        if (t.artist) row += L"  \x2014  " + g_lib.Str(t.artist);
        row += L"\t" + FormatDuration(t.Duration());
        SendMessageW(m.list, LB_ADDSTRING, 0, (LPARAM)row.c_str());
    }
    for (size_t i : select) SendMessageW(m.list, LB_SETSEL, TRUE, (LPARAM)i);
    if (!select.empty()) SendMessageW(m.list, LB_SETCARETINDEX, select.front(), FALSE);
    SendMessageW(m.list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(m.list, nullptr, TRUE);
    UpdateInfo();
}

// Tab stops for the number, the title and the length at the right edge.
void SetListTabs() {
    RECT r{};
    GetClientRect(m.list, &r);
    LONG units = LOWORD(GetDialogBaseUnits());
    int width = MulDiv(r.right - S(16), 4, std::max<LONG>(1, units));
    int stops[] = {MulDiv(S(30), 4, std::max<LONG>(1, units)), std::max(40, width - MulDiv(S(40), 4, std::max<LONG>(1, units)))};
    SendMessageW(m.list, LB_SETTABSTOPS, 2, (LPARAM)stops);
}

void Move(int delta) {
    auto moved = MoveAlbumTracks(m.draft, ListSelection(), delta);
    Button_SetCheck(m.number, BST_CHECKED);  // a new order is what the numbers should say
    FillList(moved);
}

void RemoveSelected() {
    auto selection = ListSelection();
    for (size_t k = selection.size(); k-- > 0;) m.draft.tracks.erase(m.draft.tracks.begin() + (ptrdiff_t)selection[k]);
    size_t next = selection.empty() ? 0 : std::min(selection.front(), m.draft.tracks.size());
    FillList(next < m.draft.tracks.size() ? std::vector<size_t>{next} : std::vector<size_t>{});
}

void AddSelected() {
    size_t before = m.draft.tracks.size();
    size_t added = AddAlbumTracks(g_lib, m.draft, SelectedTracks());
    std::vector<size_t> select;
    for (size_t i = before; i < m.draft.tracks.size(); ++i) select.push_back(i);
    if (added) Button_SetCheck(m.number, BST_CHECKED);
    FillList(select);
    if (!added)
        MessageBoxW(m.hwnd, L"Select tracks in the main window first; the ones already in this album are skipped.",
                    kAppName, MB_OK | MB_ICONINFORMATION);
}

void ShowOrderMenu() {
    RECT r{};
    GetWindowRect(m.order, &r);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1 + (int)AlbumOrder::Numbers, L"By &track number");
    AppendMenuW(menu, MF_STRING, 1 + (int)AlbumOrder::Title, L"By t&itle");
    AppendMenuW(menu, MF_STRING, 1 + (int)AlbumOrder::FileName, L"By &file name");
    AppendMenuW(menu, MF_STRING, 1 + (int)AlbumOrder::Reverse, L"&Reverse");
    int picked = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, r.left, r.bottom, 0, m.hwnd, nullptr);
    DestroyMenu(menu);
    if (!picked) return;
    OrderAlbum(g_lib, m.draft, (AlbumOrder)(picked - 1));
    Button_SetCheck(m.number, BST_CHECKED);
    FillList({});
}

void SetCover(std::string bytes) {
    ComPtr<IStream> s;
    s.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(bytes.data()), (UINT)bytes.size()));
    m.preview = ArtImage{};
    if (s) DecodeImage(s.Get(), S(150), m.preview);
    m.draft.cover = std::move(bytes);
    InvalidateRect(m.art, nullptr, FALSE);
    UpdateInfo();
}

void ChooseCoverFile() {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    COMDLG_FILTERSPEC filters[] = {{L"Images", L"*.jpg;*.jpeg;*.png"}};
    dlg->SetFileTypes(1, filters);
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dlg->Show(m.hwnd)) || FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        return;
    std::wstring file = path;
    CoTaskMemFree(path);
    std::string bytes;
    if (!ReadFileBytes(file, bytes) || bytes.size() < 8) return;
    bool jpeg = (uint8_t)bytes[0] == 0xFF && (uint8_t)bytes[1] == 0xD8, png = bytes.compare(0, 4, "\x89PNG") == 0;
    if (!jpeg && !png) {
        MessageBoxW(m.hwnd, L"Covers have to be JPEG or PNG images.", kAppName, MB_OK | MB_ICONINFORMATION);
        return;
    }
    SetCover(std::move(bytes));
}

void FindCoverOnline() {
    HWND window = m.hwnd;
    ShowCoverSearch(m.draft.tracks, [window](const std::string& bytes) {
        if (m.hwnd == window) SetCover(bytes);  // unless the maker closed meanwhile
    });
}

void Save() {
    ReadFields();
    if (!AlbumDraftProblem(m.draft).empty()) return;
    auto edits = AlbumEdits(g_lib, m.draft);
    if (!edits.empty() && !SubmitTagWrites(edits, true, true)) return;  // a write still running: stay open
    DestroyWindow(m.hwnd);
}

void Layout(HWND h) {
    RECT rc;
    GetClientRect(h, &rc);
    int pad = S(14), labelW = S(92), rowH = S(30), editH = S(24), artSize = S(150);
    int right = rc.right - pad - artSize - S(16), x1 = pad + labelW;
    int y = pad;
    auto row = [&](int f, int x, int w, int lw) {
        MoveWindow(m.labels[f], x, y + S(4), lw - S(6), S(20), TRUE);
        MoveWindow(m.edits[f], x + lw, y, w - lw, editH, TRUE);
    };
    row(0, pad, right - pad, labelW);
    y += rowH;
    row(1, pad, right - pad, labelW);
    y += rowH;
    int half = (right - pad) / 2;
    row(2, pad, half - S(8), labelW);
    row(3, pad + half, half, S(56));
    y += rowH;
    row(4, pad, labelW + S(50), labelW);
    row(5, pad + labelW + S(50), S(80), S(28));
    y += rowH;
    MoveWindow(m.compilation, x1, y, right - x1, S(22), TRUE);
    y += S(26);
    MoveWindow(m.artistToTracks, x1, y, right - x1, S(22), TRUE);
    y += S(32);
    // The cover, top right.
    int ax = rc.right - pad - artSize;
    MoveWindow(m.art, ax, pad, artSize, artSize, TRUE);
    MoveWindow(m.coverFile, ax, pad + artSize + S(6), artSize, S(26), TRUE);
    MoveWindow(m.coverOnline, ax, pad + artSize + S(36), artSize, S(26), TRUE);
    MoveWindow(m.coverRemove, ax, pad + artSize + S(66), artSize, S(26), TRUE);
    y = std::max(y, pad + artSize + S(100));
    // The tracks, with their buttons beside them.
    int buttonsW = S(128), listRight = rc.right - pad - buttonsW - S(10);
    MoveWindow(m.tracksLabel, pad, y, listRight - pad, S(20), TRUE);
    y += S(22);
    int bottom = rc.bottom - pad - S(30) - S(30) - S(28);
    MoveWindow(m.list, pad, y, listRight - pad, std::max(S(60), bottom - y), TRUE);
    int bx = rc.right - pad - buttonsW, by = y;
    for (HWND b : {m.up, m.down, m.remove, m.addSelected, m.order}) {
        MoveWindow(b, bx, by, buttonsW, S(26), TRUE);
        by += S(30);
    }
    MoveWindow(m.number, pad, std::max(S(60), bottom - y) + y + S(6), listRight - pad, S(22), TRUE);
    MoveWindow(m.info, pad, rc.bottom - pad - S(28) - S(30), rc.right - pad * 2, S(22), TRUE);
    MoveWindow(m.save, rc.right - pad - S(240) - S(8), rc.bottom - pad - S(28), S(140), S(28), TRUE);
    MoveWindow(m.cancel, rc.right - pad - S(100), rc.bottom - pad - S(28), S(100), S(28), TRUE);
    SetListTabs();
}

void PaintArt(HDC dc, RECT r) {
    FillSolid(dc, r, Ref(P().border));
    RECT in{r.left + 1, r.top + 1, r.right - 1, r.bottom - 1};
    FillSolid(dc, in, Ref(P().surface2));
    bool removed = m.draft.cover && m.draft.cover->empty();
    if (m.preview.Empty() || removed) {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, Ref(P().faint));
        DrawTextW(dc, removed ? L"(cover removed)" : L"No cover", -1, &in, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    int size = in.right - in.left;
    int w = m.preview.width >= m.preview.height ? size : MulDiv(size, m.preview.width, m.preview.height);
    int h = m.preview.height >= m.preview.width ? size : MulDiv(size, m.preview.height, m.preview.width);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = m.preview.width;
    bi.bmiHeader.biHeight = -m.preview.height;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    SetStretchBltMode(dc, HALFTONE);
    StretchDIBits(dc, in.left + (size - w) / 2, in.top + (size - h) / 2, w, h, 0, 0, m.preview.width, m.preview.height,
                  m.preview.pixels.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
}

// The draft's values into the controls, as the window opens.
void Fill() {
    m.loading = true;
    SetWindowTextW(m.edits[0], m.draft.title.c_str());
    SetWindowTextW(m.edits[1], m.draft.artist.c_str());
    SetWindowTextW(m.edits[2], m.draft.date.c_str());
    SetWindowTextW(m.edits[3], m.draft.genre.c_str());
    SetWindowTextW(m.edits[4], m.draft.disc ? std::to_wstring(m.draft.disc).c_str() : L"");
    SetWindowTextW(m.edits[5], m.draft.discTotal ? std::to_wstring(m.draft.discTotal).c_str() : L"");
    SendMessageW(m.edits[2], EM_SETCUEBANNER, TRUE, (LPARAM)L"(each keeps its own)");
    SendMessageW(m.edits[3], EM_SETCUEBANNER, TRUE, (LPARAM)L"(each keeps its own)");
    SendMessageW(m.edits[4], EM_SETCUEBANNER, TRUE, (LPARAM)L"(keep)");
    Button_SetCheck(m.compilation, m.draft.compilation ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(m.artistToTracks, m.draft.artistToTracks ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(m.number, m.draft.numberTracks ? BST_CHECKED : BST_UNCHECKED);
    m.preview = ArtImage{};
    for (TrackId id : m.draft.tracks)
        if (LoadAlbumArt(g_lib.Path(id), S(150), m.preview, false) && !m.preview.Empty()) break;
    m.loading = false;
    FillList({});
}

LRESULT CALLBACK AlbumMakerProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            m.hwnd = h;
            auto make = [&](DWORD ex, const wchar_t* cls, const wchar_t* text, DWORD style, int id = 0) {
                HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, h, (HMENU)(INT_PTR)id,
                                         g.inst, nullptr);
                SendMessageW(c, WM_SETFONT, (WPARAM)g.font, TRUE);
                return c;
            };
            for (int f = 0; f < 6; ++f) {
                m.labels[f] = make(0, WC_STATICW, kLabels[f], f == 5 ? SS_CENTER : SS_RIGHT);
                DWORD style = WS_TABSTOP | ES_AUTOHSCROLL | (f >= 4 ? ES_NUMBER : 0);
                m.edits[f] = make(WS_EX_CLIENTEDGE, WC_EDITW, L"", style, kTitle + f);
                SetWindowTheme(m.edits[f], g.dark ? L"DarkMode_CFD" : L"Explorer", nullptr);
            }
            m.compilation = make(0, WC_BUTTONW, L"Part of a &compilation (various artists)", BS_AUTOCHECKBOX | WS_TABSTOP, kCompilation);
            m.artistToTracks = make(0, WC_BUTTONW, L"Set every track's artist to the a&lbum artist",
                                    BS_AUTOCHECKBOX | WS_TABSTOP, kArtistToTracks);
            m.art = make(0, WC_STATICW, L"", SS_OWNERDRAW);
            m.coverFile = make(0, WC_BUTTONW, L"C&hoose cover...", BS_PUSHBUTTON | WS_TABSTOP, kCoverFile);
            m.coverOnline = make(0, WC_BUTTONW, L"Find cover onl&ine...", BS_PUSHBUTTON | WS_TABSTOP, kCoverOnline);
            m.coverRemove = make(0, WC_BUTTONW, L"Remo&ve cover", BS_PUSHBUTTON | WS_TABSTOP, kCoverRemove);
            m.tracksLabel = make(0, WC_STATICW, L"&Tracks", SS_LEFT);
            m.list = make(WS_EX_CLIENTEDGE, WC_LISTBOXW, L"",
                          WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_EXTENDEDSEL | LBS_NOINTEGRALHEIGHT | LBS_USETABSTOPS, kList);
            SetWindowTheme(m.list, g.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
            m.up = make(0, WC_BUTTONW, L"Move &up", BS_PUSHBUTTON | WS_TABSTOP, kUp);
            m.down = make(0, WC_BUTTONW, L"Move do&wn", BS_PUSHBUTTON | WS_TABSTOP, kDown);
            m.remove = make(0, WC_BUTTONW, L"Re&move", BS_PUSHBUTTON | WS_TABSTOP, kRemove);
            m.addSelected = make(0, WC_BUTTONW, L"Add &selected", BS_PUSHBUTTON | WS_TABSTOP, kAddSelected);
            m.order = make(0, WC_BUTTONW, L"&Order \x25BE", BS_PUSHBUTTON | WS_TABSTOP, kOrder);
            m.number = make(0, WC_BUTTONW, L"&Number the tracks in this order", BS_AUTOCHECKBOX | WS_TABSTOP, kNumber);
            m.info = make(0, WC_STATICW, L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS);
            m.save = make(0, WC_BUTTONW, L"Make album", BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK);
            m.cancel = make(0, WC_BUTTONW, L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL);
            for (HWND c : {m.compilation, m.artistToTracks, m.coverFile, m.coverOnline, m.coverRemove, m.up, m.down, m.remove,
                           m.addSelected, m.order, m.number, m.save, m.cancel})
                ApplyControlTheme(c, g.dark);
            ApplyWindowTheme(h, P());
            return 0;
        }
        case WM_SIZE:
            Layout(h);
            return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {S(600), S(600)};
            return 0;
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(h, &rc);
            FillRect((HDC)wp, &rc, g.surfaceBrush);
            return 1;
        }
        case WM_DRAWITEM: {
            auto* di = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
            if (di->hwndItem == m.art) {
                PaintArt(di->hDC, di->rcItem);
                return TRUE;
            }
            break;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            return SurfaceControlColor((HDC)wp, (HWND)lp == m.info ? SubColor() : TextColor());
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            return FieldControlColor((HDC)wp);
        case WM_COMMAND: {
            int id = LOWORD(wp), code = HIWORD(wp);
            if (id >= kTitle && id <= kDiscTotal && code == EN_CHANGE && !m.loading) UpdateInfo();
            else if ((id == kCompilation || id == kArtistToTracks) && code == BN_CLICKED) UpdateInfo();
            else if (id == kNumber && code == BN_CLICKED) FillList(ListSelection());
            else if (id == kList && code == LBN_SELCHANGE) UpdateInfo();
            else if (id == kUp) Move(-1);
            else if (id == kDown) Move(1);
            else if (id == kRemove) RemoveSelected();
            else if (id == kAddSelected) AddSelected();
            else if (id == kOrder) ShowOrderMenu();
            else if (id == kCoverFile) ChooseCoverFile();
            else if (id == kCoverOnline) FindCoverOnline();
            else if (id == kCoverRemove) {
                m.draft.cover = std::string();
                InvalidateRect(m.art, nullptr, FALSE);
                UpdateInfo();
            } else if (id == IDOK) Save();
            else if (id == IDCANCEL) DestroyWindow(h);
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            m.hwnd = nullptr;
            m.preview = ArtImage{};
            m.draft = AlbumDraft{};
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

bool AlbumMakerDialogMessage(MSG* msg) {
    if (!m.hwnd) return false;
    // In the track list: Alt+Up / Alt+Down move the selection, Delete takes it out of the album.
    if (msg->hwnd == m.list && (msg->message == WM_SYSKEYDOWN || msg->message == WM_KEYDOWN)) {
        bool alt = GetKeyState(VK_MENU) < 0;
        if (alt && (msg->wParam == VK_UP || msg->wParam == VK_DOWN)) {
            Move(msg->wParam == VK_UP ? -1 : 1);
            return true;
        }
        if (!alt && msg->wParam == VK_DELETE) {
            RemoveSelected();
            return true;
        }
    }
    return IsDialogMessageW(m.hwnd, msg) != FALSE;
}

bool AlbumMakerOpen() { return m.hwnd != nullptr; }

void ShowAlbumMaker(const std::vector<TrackId>& ids) {
    if (ids.empty()) return;
    if (m.hwnd) DestroyWindow(m.hwnd);
    RegisterAppClass(kAlbumClass, AlbumMakerProc);
    m.draft = DraftAlbum(g_lib, ids);
    RECT r{0, 0, S(680), S(660)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_DLGMODALFRAME, g.dpi);
    RECT owner;
    GetWindowRect(g.hwnd, &owner);
    int w = r.right - r.left, h = r.bottom - r.top;
    CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kAlbumClass, L"Make album",
                    WS_OVERLAPPEDWINDOW & ~WS_MINIMIZEBOX | WS_CLIPCHILDREN, owner.left + (owner.right - owner.left - w) / 2,
                    owner.top + S(40), w, h, g.hwnd, nullptr, g.inst, nullptr);
    Fill();
    ShowWindow(m.hwnd, SW_SHOW);
    SetFocus(m.edits[0]);
    SendMessageW(m.edits[0], EM_SETSEL, 0, -1);
}

}  // namespace app
