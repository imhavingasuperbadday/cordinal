// Library > Lint library... (app_lint.cpp): one review screen for split albums, near-duplicate
// artist names, mojibake / legacy encoding in tags, and cover-art issues. Fast metadata checks and
// the disk-backed cover audit run on a worker thread; checked fixes apply atomically through
// SubmitTagWrites so Edit > Undo last tag edit reverses the whole run.
#include "app.h"
#include "library_lint.h"

#include <shlwapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <atomic>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace app {
namespace {

const wchar_t kLintClass[] = L"CordinalLibraryLint";
constexpr UINT kScanDone = WM_APP + 1;
constexpr UINT kScanProgress = WM_APP + 2;

enum Id : int { kList = 100, kSelectAll, kSelectNone, kRescan, kFix, kUndo };

struct ScanResult {
    std::vector<lint::LintFinding> findings;
};

struct Window {
    HWND hwnd = nullptr, status = nullptr, list = nullptr, info = nullptr;
    HWND selectAll = nullptr, selectNone = nullptr, rescan = nullptr, fix = nullptr, undo = nullptr, close = nullptr;
    std::vector<lint::LintFinding> findings;
    bool populating = false;
    std::atomic<bool> cancel{false};
    std::thread worker;
} w;

bool Busy() { return w.worker.joinable(); }

// Re-encodes an oversized PNG/image to a compact JPEG (<= 1500 px) via WIC.
bool ReencodeCoverWic(const std::string& input, std::string& jpegOut) {
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICStream> in;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        FAILED(wic->CreateStream(&in)) ||
        FAILED(in->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(input.data())),
                                        (DWORD)input.size())) ||
        FAILED(wic->CreateDecoderFromStream(in.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(dec->GetFrame(0, &frame)))
        return false;

    UINT cw = 0, ch = 0;
    if (FAILED(frame->GetSize(&cw, &ch)) || !cw || !ch) return false;

    ComPtr<IWICBitmapSource> src = frame;
    constexpr UINT kMaxDim = 1500;
    if (cw > kMaxDim || ch > kMaxDim) {
        double scale = (double)kMaxDim / std::max(cw, ch);
        UINT tw = std::max(1u, (UINT)(cw * scale)), th = std::max(1u, (UINT)(ch * scale));
        ComPtr<IWICBitmapScaler> scaler;
        if (FAILED(wic->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(frame.Get(), tw, th, WICBitmapInterpolationModeHighQualityCubic)))
            return false;
        src = scaler;
    }

    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(src.Get(), fmt, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        return false;

    ComPtr<IStream> out;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> target;
    ComPtr<IPropertyBag2> props;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &out)) ||
        FAILED(wic->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc)) ||
        FAILED(enc->Initialize(out.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(enc->CreateNewFrame(&target, &props)))
        return false;

    PROPBAG2 opt{};
    opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
    VARIANT var{};
    var.vt = VT_R4;
    var.fltVal = 0.90f;
    props->Write(1, &opt, &var);

    UINT ow = 0, oh = 0;
    conv->GetSize(&ow, &oh);
    if (FAILED(target->Initialize(props.Get())) || FAILED(target->SetSize(ow, oh)) ||
        FAILED(target->SetPixelFormat(&fmt)) || FAILED(target->WriteSource(conv.Get(), nullptr)) ||
        FAILED(target->Commit()) || FAILED(enc->Commit()))
        return false;

    STATSTG st{};
    HGLOBAL mem = nullptr;
    if (FAILED(out->Stat(&st, STATFLAG_NONAME)) || FAILED(GetHGlobalFromStream(out.Get(), &mem))) return false;
    const void* p = GlobalLock(mem);
    if (!p) return false;
    jpegOut.assign(static_cast<const char*>(p), (size_t)st.cbSize.QuadPart);
    GlobalUnlock(mem);
    return !jpegOut.empty();
}

void StopWorker() {
    w.cancel.store(true);
    if (w.worker.joinable()) w.worker.join();
    w.cancel.store(false);
}

void UpdateSummary() {
    size_t checked = 0, fixable = 0;
    for (const auto& f : w.findings) {
        if (!f.fixable) continue;
        ++fixable;
        if (f.checked) ++checked;
    }
    std::wstring fixLabel = checked ? (L"Fix checked (" + std::to_wstring(checked) + L")") : L"Fix checked";
    SetWindowTextW(w.fix, fixLabel.c_str());
    EnableWindow(w.fix, !Busy() && checked > 0 && !TagWriteBusy());
    EnableWindow(w.selectAll, !Busy() && fixable > 0);
    EnableWindow(w.selectNone, !Busy() && checked > 0);
    EnableWindow(w.rescan, !Busy());
    std::wstring undoDesc;
    EnableWindow(w.undo, !Busy() && !TagWriteBusy() && CanUndoTagEdit(undoDesc));
    if (!Busy()) {
        std::wstring msg = w.findings.empty()
                               ? L"No library issues found."
                               : std::to_wstring(w.findings.size()) +
                                     (w.findings.size() == 1 ? L" issue found (" : L" issues found (") +
                                     std::to_wstring(checked) + L" checked to fix).";
        SetWindowTextW(w.status, msg.c_str());
    }
}

void PopulateList() {
    w.populating = true;
    SendMessageW(w.list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(w.list);
    ListView_RemoveAllGroups(w.list);

    struct GroupDef {
        lint::LintKind kind;
        int id;
        const wchar_t* title;
    };
    static constexpr GroupDef kGroups[] = {
        {lint::LintKind::SplitAlbum, 0, L"Split albums"},
        {lint::LintKind::NearDuplicateArtist, 1, L"Near-duplicate artists"},
        {lint::LintKind::Mojibake, 2, L"Garbled tags (encoding)"},
        {lint::LintKind::CoverArt, 3, L"Cover-art audit"},
    };
    for (const auto& gdef : kGroups) {
        LVGROUP lg{sizeof lg};
        lg.mask = LVGF_HEADER | LVGF_GROUPID;
        lg.pszHeader = const_cast<LPWSTR>(gdef.title);
        lg.iGroupId = gdef.id;
        ListView_InsertGroup(w.list, -1, &lg);
    }

    for (size_t i = 0; i < w.findings.size(); ++i) {
        const auto& f = w.findings[i];
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_GROUPID;
        item.iItem = (int)i;
        item.iGroupId = (int)f.kind;
        item.pszText = const_cast<LPWSTR>(f.summary.c_str());
        int row = ListView_InsertItem(w.list, &item);
        std::wstring before = f.before;
        std::wstring after = f.fixable ? f.after : (f.after + L" (review)");
        ListView_SetItemText(w.list, row, 1, before.data());
        ListView_SetItemText(w.list, row, 2, after.data());
        ListView_SetCheckState(w.list, row, f.fixable && f.checked ? TRUE : FALSE);
    }

    SendMessageW(w.list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(w.list, nullptr, TRUE);
    w.populating = false;
    UpdateSummary();
}

void StartScan() {
    StopWorker();
    w.findings.clear();
    PopulateList();
    SetWindowTextW(w.status, L"Scanning library tags and embedded covers...");
    EnableWindow(w.rescan, FALSE);
    EnableWindow(w.fix, FALSE);
    EnableWindow(w.selectAll, FALSE);
    EnableWindow(w.selectNone, FALSE);

    // Run fast in-memory metadata checks on the UI thread, then snapshot paths for the cover audit.
    std::vector<lint::LintFinding> meta;
    lint::FindMetadataIssues(g_lib, meta);
    auto coverTracks = lint::SnapshotCoverTracks(g_lib);
    HWND target = w.hwnd;

    w.worker = std::thread([target, meta = std::move(meta), coverTracks = std::move(coverTracks)]() mutable {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        auto* res = new ScanResult();
        res->findings = std::move(meta);
        lint::AuditCoverTracks(
            coverTracks, ReencodeCoverWic, res->findings, &w.cancel, [target](size_t done, size_t total) {
                if (!w.cancel.load()) PostMessageW(target, kScanProgress, (WPARAM)done, (LPARAM)total);
            });
        if (SUCCEEDED(hr)) CoUninitialize();
        if (!PostMessageW(target, kScanDone, 0, (LPARAM)res)) delete res;
    });
}

void SetAllChecks(bool check) {
    w.populating = true;
    for (size_t i = 0; i < w.findings.size(); ++i) {
        if (!w.findings[i].fixable) continue;
        w.findings[i].checked = check;
        ListView_SetCheckState(w.list, (int)i, check ? TRUE : FALSE);
    }
    w.populating = false;
    UpdateSummary();
}

void ApplyFixes() {
    auto edits = lint::CollectCheckedEdits(w.findings);
    if (edits.empty()) return;
    if (!SubmitTagWrites(edits, true, true)) return;
    DestroyWindow(w.hwnd);
}

void Layout(HWND h) {
    RECT rc;
    GetClientRect(h, &rc);
    int pad = S(14), btnW = S(112), btnH = S(28), gap = S(8);
    int topH = S(22), botH = btnH + S(26);
    MoveWindow(w.status, pad, pad, rc.right - pad * 2, topH, TRUE);
    int listY = pad + topH + S(6);
    int listH = std::max(S(120), (int)(rc.bottom - listY - botH - pad));
    MoveWindow(w.list, pad, listY, rc.right - pad * 2, listH, TRUE);
    int infoY = listY + listH + S(6);
    MoveWindow(w.info, pad, infoY, rc.right - pad * 2, S(18), TRUE);
    int by = rc.bottom - pad - btnH;
    MoveWindow(w.selectAll, pad, by, S(96), btnH, TRUE);
    MoveWindow(w.selectNone, pad + S(96) + gap, by, S(96), btnH, TRUE);
    MoveWindow(w.rescan, pad + S(192) + gap * 2, by, S(84), btnH, TRUE);
    MoveWindow(w.close, rc.right - pad - S(84), by, S(84), btnH, TRUE);
    MoveWindow(w.fix, rc.right - pad - S(84) - gap - S(130), by, S(130), btnH, TRUE);
    MoveWindow(w.undo, rc.right - pad - S(84) - gap * 2 - S(130) - btnW, by, btnW, btnH, TRUE);

    int lw = std::max(S(300), (int)(rc.right - pad * 2 - S(24)));
    ListView_SetColumnWidth(w.list, 0, lw * 44 / 100);
    ListView_SetColumnWidth(w.list, 1, lw * 28 / 100);
    ListView_SetColumnWidth(w.list, 2, lw * 28 / 100);
}

LRESULT CALLBACK LintProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            w.hwnd = h;
            w.status = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, h, nullptr, g.inst, nullptr);
            w.list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS |
                                         LVS_NOSORTHEADER,
                                     0, 0, 0, 0, h, (HMENU)(UINT_PTR)kList, g.inst, nullptr);
            ListView_SetExtendedListViewStyle(w.list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            ListView_EnableGroupView(w.list, TRUE);
            for (int c = 0; c < 3; ++c) {
                const wchar_t* names[] = {L"Issue", L"Before", L"Proposed fix"};
                LVCOLUMNW col{LVCF_TEXT | LVCF_WIDTH, 0, S(200), const_cast<LPWSTR>(names[c])};
                ListView_InsertColumn(w.list, c, &col);
            }
            w.info = CreateWindowW(
                L"STATIC", L"Checked fixes apply atomically and can be reversed with Edit > Undo last tag edit.",
                WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, h, nullptr, g.inst, nullptr);
            auto btn = [&](const wchar_t* text, int id, DWORD style = 0) {
                return CreateWindowW(L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, 0, 0, 0, 0, h,
                                     (HMENU)(UINT_PTR)id, g.inst, nullptr);
            };
            w.selectAll = btn(L"Select all", kSelectAll);
            w.selectNone = btn(L"Select none", kSelectNone);
            w.rescan = btn(L"Rescan", kRescan);
            w.undo = btn(L"Undo last edit", kUndo);
            w.fix = btn(L"Fix checked", kFix, BS_DEFPUSHBUTTON);
            w.close = btn(L"Close", IDCANCEL);
            for (HWND c : {w.status, w.list, w.info, w.selectAll, w.selectNone, w.rescan, w.undo, w.fix, w.close})
                SendMessageW(c, WM_SETFONT, (WPARAM)g.font, TRUE);
            for (HWND c : {w.list, ListView_GetHeader(w.list)}) ApplyControlTheme(c, g.dark);
            ListView_SetBkColor(w.list, Ref(P().surface2));
            ListView_SetTextBkColor(w.list, Ref(P().surface2));
            ListView_SetTextColor(w.list, TextColor());
            for (HWND c : {w.selectAll, w.selectNone, w.rescan, w.undo, w.fix, w.close}) ApplyControlTheme(c, g.dark);
            ApplyWindowTheme(h, P());
            Layout(h);
            return 0;
        }
        case WM_SIZE:
            Layout(h);
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
        case WM_NOTIFY: {
            auto* nm = reinterpret_cast<NMHDR*>(lp);
            if (nm->idFrom == kList && nm->code == LVN_ITEMCHANGED && !w.populating) {
                auto* lv = reinterpret_cast<NMLISTVIEW*>(lp);
                if ((lv->uChanged & LVIF_STATE) &&
                    ((lv->uNewState ^ lv->uOldState) & LVIS_STATEIMAGEMASK)) {
                    size_t idx = (size_t)lv->iItem;
                    if (idx < w.findings.size()) {
                        bool want = ListView_GetCheckState(w.list, lv->iItem) != FALSE;
                        if (!w.findings[idx].fixable && want) {
                            w.populating = true;
                            ListView_SetCheckState(w.list, lv->iItem, FALSE);
                            w.populating = false;
                        } else {
                            w.findings[idx].checked = want;
                        }
                        UpdateSummary();
                    }
                }
            }
            return 0;
        }
        case kScanProgress:
            SetWindowTextW(w.status, (L"Auditing covers (" + std::to_wstring((size_t)wp) + L" / " +
                                      std::to_wstring((size_t)lp) + L")...")
                                         .c_str());
            return 0;
        case kScanDone: {
            std::unique_ptr<ScanResult> res(reinterpret_cast<ScanResult*>(lp));
            StopWorker();
            if (res) w.findings = std::move(res->findings);
            PopulateList();
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case kSelectAll: SetAllChecks(true); break;
                case kSelectNone: SetAllChecks(false); break;
                case kRescan: StartScan(); break;
                case kFix: ApplyFixes(); break;
                case kUndo:
                    UndoLastTagEdit();
                    StartScan();
                    break;
                case IDCANCEL: DestroyWindow(h); break;
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            StopWorker();
            w.hwnd = nullptr;
            w.findings.clear();
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

bool LintDialogMessage(MSG* msg) { return w.hwnd && IsDialogMessageW(w.hwnd, msg) != FALSE; }

void StopLint() { StopWorker(); }

void ShowLint() {
    if (w.hwnd) {
        SetForegroundWindow(w.hwnd);
        return;
    }
    RegisterAppClass(kLintClass, LintProc);
    RECT r{0, 0, S(920), S(580)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_DLGMODALFRAME, g.dpi);
    RECT owner;
    GetWindowRect(g.hwnd, &owner);
    const int width = r.right - r.left, height = r.bottom - r.top;
    CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kLintClass, L"Lint my library",
                    WS_OVERLAPPEDWINDOW & ~WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                    owner.left + (owner.right - owner.left - width) / 2, owner.top + S(40), width, height, g.hwnd,
                    nullptr, g.inst, nullptr);
    if (!w.hwnd) return;
    ShowWindow(w.hwnd, SW_SHOW);
    SetFocus(w.list);
    StartScan();
}

}  // namespace app
