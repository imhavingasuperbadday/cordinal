// The album-edition UI: a compact manager and a track comparison window, plus the main-window
// commands and the debounced automatic grouping pass. Files and tracks are never touched here;
// every action edits album_editions.* and then asks the library view to rebuild.
#include "app.h"

#include "musicbrainz.h"

namespace app {

namespace {

constexpr wchar_t kEditionClass[] = L"CordinalAlbumEditions";
constexpr wchar_t kCompareClass[] = L"CordinalEditionCompare";

enum : int {
    kEditionList = 5001, kEditionPreferred, kEditionRename, kEditionRemove, kEditionSplit, kEditionAuto,
    kEditionCompare, kEditionArt, kEditionClearArt, kEditionClose, kEditionEdit, kEditionHint,
    kCompareList = 5101, kCompareConfirm, kCompareReject, kComparePrefer, kCompareClose, kCompareHint,
};

struct EditionWindow {
    HWND hwnd = nullptr, list = nullptr, edit = nullptr, hint = nullptr;
    HWND preferred = nullptr, rename = nullptr, remove = nullptr, split = nullptr, automatic = nullptr,
         compare = nullptr, art = nullptr, clearArt = nullptr, close = nullptr;
    uint32_t group = 0;
    std::vector<uint32_t> editions;
    bool settingText = false;
};

struct CompareWindow {
    HWND hwnd = nullptr, list = nullptr, hint = nullptr;
    HWND confirm = nullptr, reject = nullptr, prefer = nullptr, close = nullptr;
    uint32_t group = 0, a = 0, b = 0;
    TrackId focus = kNone;
    std::vector<EditionComparisonRow> rows;
};

EditionWindow g_edition;
CompareWindow g_compare;
bool g_albumGroupingPending = false;
ULONGLONG g_albumGroupingAt = 0;

std::wstring WindowText(HWND hwnd) {
    int length = GetWindowTextLengthW(hwnd);
    std::wstring text((size_t)length + 1, L'\0');
    GetWindowTextW(hwnd, text.data(), length + 1);
    text.resize((size_t)length);
    return text;
}

const AlbumGroup* CurrentGroup() {
    return FindAlbumGroupConst(g_edition.group);
}

uint32_t SelectedEditionId() {
    int index = (int)SendMessageW(g_edition.list, LB_GETCURSEL, 0, 0);
    if (index < 0) return 0;
    LRESULT data = SendMessageW(g_edition.list, LB_GETITEMDATA, index, 0);
    return data == LB_ERR ? 0 : (uint32_t)data;
}

const AlbumEdition* SelectedEdition() {
    const AlbumGroup* group = CurrentGroup();
    return group ? FindAlbumEdition(*group, SelectedEditionId()) : nullptr;
}

void SetControlFont(HWND control) {
    if (control && g.font) SendMessageW(control, WM_SETFONT, (WPARAM)g.font, TRUE);
}

HWND MakeControl(HWND parent, DWORD ex, const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
    HWND control = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, parent,
                                   (HMENU)(INT_PTR)id, g.inst, nullptr);
    SetControlFont(control);
    if (control) ApplyControlTheme(control, g.dark);
    return control;
}

std::wstring SourceName(EditionSource source) {
    switch (source) {
        case EditionSource::MusicBrainz: return L"MusicBrainz";
        case EditionSource::Manual: return L"manual";
        default: return L"automatic";
    }
}

std::wstring EditionLine(const AlbumEdition& edition, bool preferred) {
    std::wstring line = preferred ? L"\x2713  " : L"    ";
    line += EditionLabel(edition);
    line += L"  \x2022  " + std::to_wstring(edition.tracks.size()) +
            (edition.tracks.size() == 1 ? L" track" : L" tracks");
    std::wstring date = FromUtf8(edition.date);
    if (date.size() >= 4) line += L"  \x2022  " + date.substr(0, 4);
    line += L"  \x2022  " + SourceName(edition.source);
    return line;
}

void RefreshEditionWindow();

void ApplyEditionSelection() {
    const AlbumGroup* group = CurrentGroup();
    const AlbumEdition* edition = SelectedEdition();
    bool enabled = group && edition;
    for (HWND control : {g_edition.preferred, g_edition.rename, g_edition.remove, g_edition.split,
                         g_edition.compare, g_edition.art, g_edition.clearArt, g_edition.automatic})
        EnableWindow(control, enabled ? TRUE : FALSE);
    if (edition) {
        g_edition.settingText = true;
        SetWindowTextW(g_edition.edit, EditionLabel(*edition).c_str());
        g_edition.settingText = false;
        EnableWindow(g_edition.rename, TRUE);
        EnableWindow(g_edition.compare, group->editions.size() > 1 ? TRUE : FALSE);
        EnableWindow(g_edition.remove, group->editions.size() > 1 ? TRUE : FALSE);
        EnableWindow(g_edition.split, group->editions.size() > 1 ? TRUE : FALSE);
        EnableWindow(g_edition.clearArt, group->artTrack != kInvalidTrack ? TRUE : FALSE);
        EnableWindow(g_edition.automatic, group->manual ? TRUE : FALSE);
    }
    (void)enabled;
}

void RefreshEditionWindow() {
    if (!g_edition.hwnd) return;
    SendMessageW(g_edition.list, LB_RESETCONTENT, 0, 0);
    g_edition.editions.clear();
    const AlbumGroup* group = CurrentGroup();
    if (!group) {
        SetWindowTextW(g_edition.hint, L"That album group is no longer in the library.");
        SetWindowTextW(g_edition.hwnd, L"Album editions");
        return;
    }
    std::wstring title = L"Album editions \x2014 " + FromUtf8(group->title);
    if (!group->artist.empty()) title += L" \x2014 " + FromUtf8(group->artist);
    SetWindowTextW(g_edition.hwnd, title.c_str());
    SetWindowTextW(g_edition.hint,
                   L"Switching editions changes the shown artwork, tracklist and playback. Audio files and "
                   L"playlists are never changed. To add an album, select album tiles in the library and choose "
                   L"Group selected albums as editions.");
    uint32_t preferred = PreferredEditionId(*group);
    for (size_t i = 0; i < group->editions.size(); ++i) {
        const AlbumEdition& edition = group->editions[i];
        int row = (int)SendMessageW(g_edition.list, LB_ADDSTRING, 0, (LPARAM)EditionLine(edition, edition.id == preferred).c_str());
        if (row >= 0) {
            SendMessageW(g_edition.list, LB_SETITEMDATA, row, (LPARAM)edition.id);
            g_edition.editions.push_back(edition.id);
        }
    }
    if (!g_edition.editions.empty()) {
        int row = 0;
        for (size_t i = 0; i < g_edition.editions.size(); ++i)
            if (g_edition.editions[i] == preferred) row = (int)i;
        SendMessageW(g_edition.list, LB_SETCURSEL, row, 0);
    }
    ApplyEditionSelection();
}

void EditionLayout(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int pad = S(12), button = S(152), height = S(28), hint = S(48);
    int rightX = rc.right - pad - button;
    int listW = rightX - pad * 2;
    int bottom = rc.bottom - pad - hint;
    MoveWindow(g_edition.hint, pad, bottom, rc.right - pad * 2, hint, TRUE);
    MoveWindow(g_edition.list, pad, pad, listW, bottom - pad - height - S(8), TRUE);
    MoveWindow(g_edition.edit, pad, bottom - height, listW - S(86), height, TRUE);
    MoveWindow(g_edition.rename, pad + listW - S(78), bottom - height, S(78), height, TRUE);
    int y = pad;
    for (HWND control : {g_edition.preferred, g_edition.compare, g_edition.art, g_edition.clearArt,
                         g_edition.remove, g_edition.split, g_edition.automatic, g_edition.close}) {
        MoveWindow(control, rightX, y, button, height, TRUE);
        y += height + S(7);
    }
}

LRESULT CALLBACK EditionEditSubclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        PostMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(kEditionRename, BN_CLICKED), (LPARAM)g_edition.rename);
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void RenameSelected() {
    const AlbumEdition* edition = SelectedEdition();
    if (!edition) return;
    std::wstring label = WindowText(g_edition.edit);
    if (SetEditionLabel(g_edition.group, edition->id, label)) {
        RefreshEditionWindow();
        AlbumGroupChangedInApp();
    }
}

void RemoveSelected() {
    const AlbumGroup* group = CurrentGroup();
    const AlbumEdition* edition = SelectedEdition();
    if (!group || !edition || group->editions.size() <= 1) return;
    std::wstring question = L"Remove \"" + EditionLabel(*edition) +
                            L"\" from this album group?\n\nIt stays in the library as its own album. Files, "
                            L"playlists and statistics are untouched.";
    if (MessageBoxW(g_edition.hwnd, question.c_str(), kAppName, MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    if (RemoveEditionFromGroup(group->id, edition->id)) {
        RefreshEditionWindow();
        AlbumGroupChangedInApp();
    }
}

void SplitGroup() {
    const AlbumGroup* group = CurrentGroup();
    if (!group || group->editions.size() <= 1) return;
    std::wstring question = L"Split \"" + FromUtf8(group->title) +
                            L"\" into separate albums?\n\nEvery edition keeps its own files and becomes an "
                            L"independent album. Automatic grouping will not join them again.";
    if (MessageBoxW(g_edition.hwnd, question.c_str(), kAppName, MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    uint32_t id = group->id;
    if (SplitAlbumGroup(id)) {
        DestroyWindow(g_edition.hwnd);
        AlbumGroupChangedInApp();
    }
}

LRESULT CALLBACK EditionProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g_edition.hwnd = hwnd;
            g_edition.list = MakeControl(hwnd, WS_EX_CLIENTEDGE, WC_LISTBOXW, L"",
                                         LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP, kEditionList);
            g_edition.preferred = MakeControl(hwnd, 0, WC_BUTTONW, L"Set &preferred", BS_PUSHBUTTON | WS_TABSTOP,
                                              kEditionPreferred);
            g_edition.compare = MakeControl(hwnd, 0, WC_BUTTONW, L"&Compare editions...", BS_PUSHBUTTON | WS_TABSTOP,
                                            kEditionCompare);
            g_edition.remove = MakeControl(hwnd, 0, WC_BUTTONW, L"&Remove from group...", BS_PUSHBUTTON | WS_TABSTOP,
                                           kEditionRemove);
            g_edition.split = MakeControl(hwnd, 0, WC_BUTTONW, L"&Split group", BS_PUSHBUTTON | WS_TABSTOP, kEditionSplit);
            g_edition.art = MakeControl(hwnd, 0, WC_BUTTONW, L"Use this &cover", BS_PUSHBUTTON | WS_TABSTOP, kEditionArt);
            g_edition.clearArt = MakeControl(hwnd, 0, WC_BUTTONW, L"Clear cover choice", BS_PUSHBUTTON | WS_TABSTOP,
                                             kEditionClearArt);
            g_edition.automatic = MakeControl(hwnd, 0, WC_BUTTONW, L"Allow &automatic grouping", BS_PUSHBUTTON | WS_TABSTOP,
                                              kEditionAuto);
            g_edition.close = MakeControl(hwnd, 0, WC_BUTTONW, L"&Close", BS_PUSHBUTTON | WS_TABSTOP, kEditionClose);
            g_edition.edit = MakeControl(hwnd, WS_EX_CLIENTEDGE, WC_EDITW, L"", ES_AUTOHSCROLL | WS_TABSTOP, kEditionEdit);
            g_edition.rename = MakeControl(hwnd, 0, WC_BUTTONW, L"R&ename", BS_PUSHBUTTON | WS_TABSTOP, kEditionRename);
            g_edition.hint = MakeControl(hwnd, 0, WC_STATICW, L"", SS_LEFT | SS_NOPREFIX, kEditionHint);
            SetWindowSubclass(g_edition.edit, EditionEditSubclass, 1, 0);
            ApplyWindowTheme(hwnd, P());
            RefreshEditionWindow();
            return 0;
        }
        case WM_SIZE:
            EditionLayout(hwnd);
            return 0;
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect((HDC)wp, &rc, g.surfaceBrush);
            return 1;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            return SurfaceControlColor((HDC)wp, (HWND)lp == g_edition.hint ? SubColor() : TextColor());
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLOREDIT:
            return FieldControlColor((HDC)wp);
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case kEditionList:
                    if (HIWORD(wp) == LBN_SELCHANGE) ApplyEditionSelection();
                    else if (HIWORD(wp) == LBN_DBLCLK) {
                        const AlbumEdition* edition = SelectedEdition();
                        if (edition && SetPreferredEdition(g_edition.group, edition->id)) {
                            RefreshEditionWindow();
                            LibViewChooseEdition(g_edition.group, edition->id);
                            AlbumGroupChangedInApp();
                        }
                    }
                    break;
                case kEditionEdit:
                    if (HIWORD(wp) == EN_CHANGE && !g_edition.settingText) EnableWindow(g_edition.rename, TRUE);
                    break;
                case kEditionPreferred: {
                    const AlbumEdition* edition = SelectedEdition();
                    if (edition && SetPreferredEdition(g_edition.group, edition->id)) {
                        RefreshEditionWindow();
                        LibViewChooseEdition(g_edition.group, edition->id);
                        AlbumGroupChangedInApp();
                    }
                    break;
                }
                case kEditionRename: RenameSelected(); break;
                case kEditionArt: {
                    const AlbumEdition* edition = SelectedEdition();
                    if (edition && !edition->tracks.empty() && SetAlbumGroupArt(g_edition.group, edition->tracks.front())) {
                        RefreshEditionWindow();
                        AlbumGroupChangedInApp();
                    }
                    break;
                }
                case kEditionClearArt:
                    if (ClearAlbumGroupArt(g_edition.group)) {
                        RefreshEditionWindow();
                        AlbumGroupChangedInApp();
                    }
                    break;
                case kEditionRemove: RemoveSelected(); break;
                case kEditionSplit: SplitGroup(); break;
                case kEditionAuto:
                    if (AllowAutomaticGrouping(g_edition.group)) {
                        if (FindAlbumGroupConst(g_edition.group)) RefreshEditionWindow();
                        else DestroyWindow(hwnd);
                        AlbumGroupChangedInApp();
                    }
                    break;
                case kEditionCompare: {
                    const AlbumGroup* group = CurrentGroup();
                    const AlbumEdition* edition = SelectedEdition();
                    if (group && edition) {
                        uint32_t preferred = PreferredEditionId(*group);
                        uint32_t other = preferred;
                        for (const auto& candidate : group->editions)
                            if (candidate.id != edition->id) { other = candidate.id; break; }
                        if (edition->id != preferred) ShowEditionComparisonDialog(group->id, kNone, edition->id, preferred);
                        else ShowEditionComparisonDialog(group->id, kNone, edition->id, other);
                    }
                    break;
                }
                case kEditionClose: DestroyWindow(hwnd); break;
            }
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) DestroyWindow(hwnd);
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_NCDESTROY:
            RemoveWindowSubclass(g_edition.edit, EditionEditSubclass, 1);
            g_edition = EditionWindow{};
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

std::wstring CompareLine(const EditionComparisonRow& row, const AlbumGroup& group, uint32_t a, uint32_t b) {
    auto title = [&](TrackId id) {
        if (id == kInvalidTrack || !g_lib.Contains(id)) return std::wstring(L"(missing)");
        const Track& track = g_lib[id];
        std::wstring text = g_lib.Str(track.title ? track.title : track.file);
        if (text.empty()) text = StemOf(g_lib.Path(id));
        return text;
    };
    auto editionName = [&](uint32_t id) {
        const AlbumEdition* edition = FindAlbumEdition(group, id);
        return edition ? EditionLabel(*edition) : L"edition";
    };
    if (row.a == kInvalidTrack) return L"      \x2022  " + title(row.b) + L"   (only in " + editionName(b) + L")";
    if (row.b == kInvalidTrack) return title(row.a) + L"   (only in " + editionName(a) + L")";
    const wchar_t* relation = L"unknown";
    switch (row.relation) {
        case TrackRelation::SameRecording: relation = L"same recording"; break;
        case TrackRelation::DifferentMaster: relation = L"different master"; break;
        case TrackRelation::AlternateMix: relation = L"alternate mix"; break;
        case TrackRelation::DifferentPerformance: relation = L"different performance"; break;
        default: break;
    }
    const wchar_t* status = row.status == MatchStatus::Confirmed ? L"confirmed" :
                            row.status == MatchStatus::Rejected ? L"rejected" : L"suggested";
    return title(row.a) + L"   \x2194   " + title(row.b) + L"   \x2022  " + relation + L", " + status;
}

void ApplyCompareSelection() {
    int index = (int)SendMessageW(g_compare.list, LB_GETCURSEL, 0, 0);
    bool pair = index >= 0 && index < (int)g_compare.rows.size() &&
                g_compare.rows[index].a != kInvalidTrack && g_compare.rows[index].b != kInvalidTrack;
    EnableWindow(g_compare.confirm, pair ? TRUE : FALSE);
    EnableWindow(g_compare.reject, pair ? TRUE : FALSE);
    EnableWindow(g_compare.prefer, pair ? TRUE : FALSE);
}

void RefreshCompareWindow() {
    if (!g_compare.hwnd) return;
    const AlbumGroup* group = FindAlbumGroupConst(g_compare.group);
    if (!group) { DestroyWindow(g_compare.hwnd); return; }
    g_compare.rows = CompareEditions(group->id, g_compare.a, g_compare.b);
    SendMessageW(g_compare.list, LB_RESETCONTENT, 0, 0);
    int select = -1;
    for (size_t i = 0; i < g_compare.rows.size(); ++i) {
        const auto& row = g_compare.rows[i];
        int item = (int)SendMessageW(g_compare.list, LB_ADDSTRING, 0,
                                     (LPARAM)CompareLine(row, *group, g_compare.a, g_compare.b).c_str());
        if (item >= 0) SendMessageW(g_compare.list, LB_SETITEMDATA, item, (LPARAM)i);
        if (g_compare.focus != kNone && (row.a == g_compare.focus || row.b == g_compare.focus)) select = (int)i;
    }
    if (select < 0 && !g_compare.rows.empty()) select = 0;
    if (select >= 0) SendMessageW(g_compare.list, LB_SETCURSEL, select, 0);
    const AlbumEdition* a = FindAlbumEdition(*group, g_compare.a);
    const AlbumEdition* b = FindAlbumEdition(*group, g_compare.b);
    std::wstring hint = L"Comparing " + std::wstring(a ? EditionLabel(*a) : L"edition") + L" with " +
                        std::wstring(b ? EditionLabel(*b) : L"edition") +
                        L". Suggested matches are conservative; confirm or reject them to make the relationship explicit. "
                        L"Playback still uses each edition's own files.";
    SetWindowTextW(g_compare.hint, hint.c_str());
    ApplyCompareSelection();
}

void CompareLayout(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int pad = S(12), height = S(28), hint = S(48), button = S(178);
    int bottom = rc.bottom - pad - height;
    MoveWindow(g_compare.hint, pad, pad, rc.right - pad * 2, hint, TRUE);
    MoveWindow(g_compare.list, pad, pad + hint + S(6), rc.right - pad * 2, bottom - (pad + hint + S(6)) - S(8), TRUE);
    int x = pad;
    for (HWND control : {g_compare.confirm, g_compare.reject, g_compare.prefer}) {
        MoveWindow(control, x, bottom, button, height, TRUE);
        x += button + S(8);
    }
    MoveWindow(g_compare.close, rc.right - pad - S(100), bottom, S(100), height, TRUE);
}

void SelectedComparePair(TrackId& a, TrackId& b, TrackRelation* relation = nullptr) {
    int index = (int)SendMessageW(g_compare.list, LB_GETCURSEL, 0, 0);
    a = b = kInvalidTrack;
    if (relation) *relation = TrackRelation::SameRecording;
    if (index < 0 || index >= (int)g_compare.rows.size()) return;
    a = g_compare.rows[index].a;
    b = g_compare.rows[index].b;
    if (relation) *relation = g_compare.rows[index].relation;
}

LRESULT CALLBACK CompareProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g_compare.hwnd = hwnd;
            g_compare.list = MakeControl(hwnd, WS_EX_CLIENTEDGE, WC_LISTBOXW, L"",
                                         LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP, kCompareList);
            g_compare.confirm = MakeControl(hwnd, 0, WC_BUTTONW, L"&Confirm match", BS_PUSHBUTTON | WS_TABSTOP,
                                            kCompareConfirm);
            g_compare.reject = MakeControl(hwnd, 0, WC_BUTTONW, L"&Reject match", BS_PUSHBUTTON | WS_TABSTOP, kCompareReject);
            g_compare.prefer = MakeControl(hwnd, 0, WC_BUTTONW, L"&Prefer second version", BS_PUSHBUTTON | WS_TABSTOP,
                                           kComparePrefer);
            g_compare.close = MakeControl(hwnd, 0, WC_BUTTONW, L"&Close", BS_PUSHBUTTON | WS_TABSTOP, kCompareClose);
            g_compare.hint = MakeControl(hwnd, 0, WC_STATICW, L"", SS_LEFT | SS_NOPREFIX, kCompareHint);
            ApplyWindowTheme(hwnd, P());
            RefreshCompareWindow();
            return 0;
        }
        case WM_SIZE:
            CompareLayout(hwnd);
            return 0;
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect((HDC)wp, &rc, g.surfaceBrush);
            return 1;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            return SurfaceControlColor((HDC)wp, (HWND)lp == g_compare.hint ? SubColor() : TextColor());
        case WM_CTLCOLORLISTBOX:
            return FieldControlColor((HDC)wp);
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case kCompareList:
                    if (HIWORD(wp) == LBN_SELCHANGE) ApplyCompareSelection();
                    break;
                case kCompareConfirm:
                case kCompareReject: {
                    TrackId a, b;
                    TrackRelation relation = TrackRelation::SameRecording;
                    SelectedComparePair(a, b, &relation);
                    if (a != kInvalidTrack && b != kInvalidTrack &&
                        SetTrackMatch(g_compare.group, a, b,
                                      LOWORD(wp) == kCompareConfirm ? MatchStatus::Confirmed : MatchStatus::Rejected,
                                      relation)) {
                        RefreshCompareWindow();
                        AlbumGroupChangedInApp();
                    }
                    break;
                }
                case kComparePrefer: {
                    TrackId a, b;
                    SelectedComparePair(a, b);
                    if (b != kInvalidTrack && SetPreferredTrackVersion(g_compare.group, b)) {
                        RefreshCompareWindow();
                        AlbumGroupChangedInApp();
                    }
                    break;
                }
                case kCompareClose: DestroyWindow(hwnd); break;
            }
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) DestroyWindow(hwnd);
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_NCDESTROY:
            g_compare = CompareWindow{};
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void CenterOnOwner(HWND hwnd, int width, int height) {
    RECT owner{};
    GetWindowRect(g.hwnd, &owner);
    int x = owner.left + (owner.right - owner.left - width) / 2;
    int y = owner.top + (owner.bottom - owner.top - height) / 2;
    SetWindowPos(hwnd, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}

}  // namespace

void ShowAlbumEditionsDialog(uint32_t groupId) {
    if (!FindAlbumGroupConst(groupId)) return;
    if (g_edition.hwnd) {
        if (g_edition.group == groupId) {
            SetForegroundWindow(g_edition.hwnd);
            return;
        }
        DestroyWindow(g_edition.hwnd);
    }
    RegisterAppClass(kEditionClass, EditionProc, CS_DBLCLKS);
    RECT r{0, 0, S(620), S(490)};
    AdjustWindowRectExForDpi(&r, WS_CAPTION | WS_SYSMENU | WS_THICKFRAME, FALSE, WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
                             g.dpi);
    g_edition.group = groupId;
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kEditionClass, L"Album editions",
                                WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_POPUP | WS_CLIPCHILDREN, 0, 0,
                                r.right - r.left, r.bottom - r.top, g.hwnd, nullptr, g.inst, nullptr);
    if (!hwnd) { g_edition = EditionWindow{}; return; }
    CenterOnOwner(hwnd, r.right - r.left, r.bottom - r.top);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    SetFocus(g_edition.list);
}

void ShowEditionComparisonDialog(uint32_t groupId, TrackId focusTrack, uint32_t editionA, uint32_t editionB) {
    const AlbumGroup* group = FindAlbumGroupConst(groupId);
    if (!group || group->editions.size() < 2) return;
    if (g_compare.hwnd) DestroyWindow(g_compare.hwnd);
    uint32_t a = editionA ? editionA : PreferredEditionId(*group);
    if (!FindAlbumEdition(*group, a)) a = group->editions.front().id;
    uint32_t b = editionB;
    if (!b || b == a) {
        b = 0;
        for (const auto& edition : group->editions)
            if (edition.id != a) { b = edition.id; break; }
    }
    if (!b) return;
    RegisterAppClass(kCompareClass, CompareProc, CS_DBLCLKS);
    RECT r{0, 0, S(780), S(460)};
    AdjustWindowRectExForDpi(&r, WS_CAPTION | WS_SYSMENU | WS_THICKFRAME, FALSE, WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
                             g.dpi);
    g_compare.group = groupId;
    g_compare.a = a;
    g_compare.b = b;
    g_compare.focus = focusTrack;
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kCompareClass, L"Compare editions",
                                WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_POPUP | WS_CLIPCHILDREN, 0, 0,
                                r.right - r.left, r.bottom - r.top, g.hwnd, nullptr, g.inst, nullptr);
    if (!hwnd) { g_compare = CompareWindow{}; return; }
    CenterOnOwner(hwnd, r.right - r.left, r.bottom - r.top);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    SetFocus(g_compare.list);
}

bool EditionDialogMessage(MSG* msg) {
    if (!msg) return false;
    HWND dialog = nullptr;
    if (g_edition.hwnd && (msg->hwnd == g_edition.hwnd || IsChild(g_edition.hwnd, msg->hwnd))) dialog = g_edition.hwnd;
    else if (g_compare.hwnd && (msg->hwnd == g_compare.hwnd || IsChild(g_compare.hwnd, msg->hwnd))) dialog = g_compare.hwnd;
    if (!dialog) return false;
    if (IsDialogMessageW(dialog, msg)) return true;
    TranslateMessage(msg);
    DispatchMessageW(msg);
    return true;
}

void AlbumGroupChangedInApp() {
    g.dirty = true;
    g.statsDirty = true;
    if (g.hwnd) {
        // A renamed edition is searchable immediately; title-format cells may also say its label.
        ClearCellCache();
        if (!g.filter.empty()) RebuildView(true);
        else LibViewRebuild();
        if (g.list) InvalidateRect(g.list, nullptr, FALSE);
    }
}

void RequestAlbumGrouping() {
    g_albumGroupingPending = true;
    g_albumGroupingAt = GetTickCount64();
}

void AlbumGroupingTick() {
    if (!g_albumGroupingPending || GetTickCount64() - g_albumGroupingAt < 750) return;
    // Let scans and tag reads settle: grouping reads album tags, so a half-applied batch would
    // create a group from the old title and then another one after the new title lands.
    if (ScanInProgress() || g.scanner.Busy() || !g.tagPending.empty() || TagWriterRetainsTracks()) return;
    g_albumGroupingPending = false;
    if (AutoGroupAlbums()) AlbumGroupChangedInApp();
}

void MusicBrainzTick() {
    if (musicbrainz::Tick()) AlbumGroupChangedInApp();
}

bool EditionCommand(int id) {
    uint32_t group = 0, edition = 0;
    bool focused = LibViewFocusedEdition(group, edition);
    switch (id) {
        case IDM_EDITION_GROUP: {
            auto albums = LibViewSelectedAlbums();
            std::wstring error;
            if (!GroupAlbumsAsEditions(albums, &error)) {
                ShowMessage(error.empty() ? L"Select two or more album tiles to group them." : error);
                return true;
            }
            AlbumGroupChangedInApp();
            ShowMessage(L"Grouped " + std::to_wstring(albums.size()) + L" albums as editions.");
            return true;
        }
        case IDM_EDITION_MANAGER:
            if (focused) ShowAlbumEditionsDialog(group);
            else ShowMessage(L"Open an album with editions first.");
            return true;
        case IDM_EDITION_PREFERRED:
            if (focused && SetPreferredEdition(group, edition)) {
                LibViewChooseEdition(group, edition);
                AlbumGroupChangedInApp();
            }
            return true;
        case IDM_EDITION_REMOVE:
            if (focused) {
                const AlbumGroup* found = FindAlbumGroupConst(group);
                const AlbumEdition* selected = found ? FindAlbumEdition(*found, edition) : nullptr;
                if (found && selected && found->editions.size() > 1) {
                    std::wstring question = L"Remove \"" + EditionLabel(*selected) +
                                            L"\" from this album group?\n\nIt stays in the library as its own album.";
                    if (MessageBoxW(g.hwnd, question.c_str(), kAppName, MB_YESNO | MB_ICONQUESTION) == IDYES &&
                        RemoveEditionFromGroup(group, edition))
                        AlbumGroupChangedInApp();
                }
            }
            return true;
        case IDM_EDITION_SPLIT:
            if (focused) {
                const AlbumGroup* found = FindAlbumGroupConst(group);
                if (found && found->editions.size() > 1) {
                    std::wstring question = L"Split \"" + FromUtf8(found->title) +
                                            L"\" into separate albums?\n\nEvery edition keeps its own files.";
                    if (MessageBoxW(g.hwnd, question.c_str(), kAppName, MB_YESNO | MB_ICONQUESTION) == IDYES &&
                        SplitAlbumGroup(group))
                        AlbumGroupChangedInApp();
                }
            }
            return true;
        case IDM_EDITION_ART:
            if (focused) {
                const AlbumGroup* found = FindAlbumGroupConst(group);
                const AlbumEdition* selected = found ? FindAlbumEdition(*found, edition) : nullptr;
                if (selected && !selected->tracks.empty() && SetAlbumGroupArt(group, selected->tracks.front()))
                    AlbumGroupChangedInApp();
            }
            return true;
        case IDM_EDITION_CLEAR_ART:
            if (focused && ClearAlbumGroupArt(group)) AlbumGroupChangedInApp();
            return true;
        case IDM_EDITION_COMPARE:
            if (focused) ShowEditionComparisonDialog(group);
            return true;
        case IDM_EDITION_VERSIONS: {
            auto selected = SelectedTracks();
            if (selected.size() == 1) {
                const AlbumGroup* found = AlbumGroupForTrack(selected.front());
                if (found) ShowEditionComparisonDialog(found->id, selected.front());
            }
            return true;
        }
    }
    return false;
}

void UpdateEditionMenu(HMENU menu) {
    if (!menu) return;
    auto albums = LibViewSelectedAlbums();
    uint32_t group = 0, edition = 0;
    bool focused = LibViewFocusedEdition(group, edition);
    const AlbumGroup* found = focused ? FindAlbumGroupConst(group) : nullptr;
    const size_t editions = found ? found->editions.size() : 0;
    auto selected = SelectedTracks();
    bool versions = selected.size() == 1 && AlbumGroupForTrack(selected.front()) != nullptr;
    auto enable = [&](int id, bool on) { EnableMenuItem(menu, id, MF_BYCOMMAND | (on ? MF_ENABLED : MF_GRAYED)); };
    enable(IDM_EDITION_GROUP, albums.size() >= 2);
    enable(IDM_EDITION_MANAGER, focused);
    enable(IDM_EDITION_PREFERRED, focused && editions > 1);
    enable(IDM_EDITION_REMOVE, focused && editions > 1);
    enable(IDM_EDITION_SPLIT, focused && editions > 1);
    enable(IDM_EDITION_COMPARE, focused && editions > 1);
    enable(IDM_EDITION_ART, focused);
    enable(IDM_EDITION_CLEAR_ART, focused && found && found->artTrack != kInvalidTrack);
    enable(IDM_EDITION_VERSIONS, versions);
}

void AppendEditionMenu(HMENU menu) {
    HMENU editions = CreatePopupMenu();
    AppendMenuW(editions, MF_STRING, IDM_EDITION_GROUP, L"&Group selected albums as editions");
    AppendMenuW(editions, MF_STRING, IDM_EDITION_MANAGER, L"Album &editions...");
    AppendMenuW(editions, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(editions, MF_STRING, IDM_EDITION_PREFERRED, L"Set &preferred edition");
    AppendMenuW(editions, MF_STRING, IDM_EDITION_REMOVE, L"&Remove edition from group");
    AppendMenuW(editions, MF_STRING, IDM_EDITION_SPLIT, L"&Split album group");
    AppendMenuW(editions, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(editions, MF_STRING, IDM_EDITION_ART, L"Use this edition's &cover for the group");
    AppendMenuW(editions, MF_STRING, IDM_EDITION_CLEAR_ART, L"&Clear group cover choice");
    AppendMenuW(editions, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(editions, MF_STRING, IDM_EDITION_COMPARE, L"&Compare editions...");
    AppendMenuW(editions, MF_STRING, IDM_EDITION_VERSIONS, L"Other &versions...");
    UpdateEditionMenu(editions);
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)editions, L"Album &editions");
}

}  // namespace app
