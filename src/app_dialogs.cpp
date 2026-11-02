// Shell-facing commands that don't belong to a panel: the common file/folder open dialog, m3u
// playlist import and export, "Show in Explorer" / Properties on the selection, and the About box
// (a small themed window with its clickable cardinal).
#include "app.h"

#include "theme_win.h"

namespace app {

std::vector<std::wstring> PickPaths(bool folders) {
    std::vector<std::wstring> result;
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return result;
    DWORD options = 0;
    dlg->GetOptions(&options);
    dlg->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_ALLOWMULTISELECT | (folders ? FOS_PICKFOLDERS : 0));
    std::wstring spec = OpenFileFilterSpec();
    COMDLG_FILTERSPEC filters[] = {{L"Audio files", spec.c_str()}, {L"Playlists", L"*.m3u;*.m3u8"}, {L"All files", L"*.*"}};
    if (!folders) dlg->SetFileTypes((UINT)std::size(filters), filters);
    dlg->SetTitle(folders ? L"Add folders to playlist" : L"Add files to playlist");
    if (FAILED(dlg->Show(g.hwnd))) return result;
    ComPtr<IShellItemArray> items;
    if (FAILED(dlg->GetResults(&items))) return result;
    DWORD count = 0;
    items->GetCount(&count);
    for (DWORD i = 0; i < count; ++i) {
        ComPtr<IShellItem> item;
        PWSTR path = nullptr;
        if (SUCCEEDED(items->GetItemAt(i, &item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            result.push_back(path);
            CoTaskMemFree(path);
        }
    }
    return result;
}

void ImportPlaylistFile() {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    COMDLG_FILTERSPEC filters[] = {{L"M3U playlists", L"*.m3u;*.m3u8"}};
    dlg->SetFileTypes(1, filters);
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dlg->Show(g.hwnd)) || FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        return;
    std::wstring file = path;
    CoTaskMemFree(path);
    NewPlaylist(StemOf(file));
    AddPaths({file}, false);
}

void ExportPlaylistFile() {
    ComPtr<IFileSaveDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    COMDLG_FILTERSPEC filters[] = {{L"M3U8 playlist", L"*.m3u8"}};
    dlg->SetFileTypes(1, filters);
    dlg->SetDefaultExtension(L"m3u8");
    dlg->SetFileName(Active().name.c_str());
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dlg->Show(g.hwnd)) || FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        return;
    bool ok = ExportM3U(path, Active());
    ShowMessage(ok ? L"Saved playlist to " + std::wstring(path)
                   : L"Couldn't save the playlist to " + std::wstring(path) +
                         L". Check that the file isn't open in another program and that every entry still exists.");
    CoTaskMemFree(path);
}

void ShowInExplorer() {
    auto sel = SelectedTracks();
    if (sel.empty()) return;
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(AudioFileOf(g_lib.Path(sel[0])).c_str());  // a CUE track: its file
    if (pidl) {
        SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        ILFree(pidl);
    }
}

void ShowProperties() {
    auto sel = SelectedTracks();
    if (sel.empty()) return;
    ShowTagEditor(sel);
}

namespace {

// Help > About: a small themed window (not a message box), modal to the main window. The header is
// the cardinal, the name and version, and what the program is; below it a few rows say what it
// does, and the footer, on the surface color, holds the live memory and library figures and OK.
// Clicking the cardinal makes it sing (sound effects on or off).
const wchar_t kAboutClass[] = L"CordinalAbout";
const wchar_t kAboutTagline[] = L"A fast, lightweight music player and library for Windows and Linux.";

struct AboutRow {
    const wchar_t *label, *text;
    int height = 0;
};

struct About {
    HWND hwnd = nullptr, ok = nullptr;
    HICON icon = nullptr;
    HFONT titleFont = nullptr;
    std::wstring stats;
    AboutRow rows[4] = {
        {L"Library", L"Watched folders with an Inbox, a column browser, album and artist views, auto-playlists, "
                     L"ratings and play counts, a tag editor and a file organizer."},
        {L"Formats", L"Cordinal's own decoders for FLAC, WAV, AIFF, MPEG audio, Vorbis, AAC-LC, ALAC, WMA Standard, "
                     L"Opus, APE, WavPack and AC3, with Matroska audio support."},
        {L"Playback", L"Gapless playback, ReplayGain with an EBU R128 scanner, a 10-band equalizer, exclusive "
                      L"(bit-perfect) output and a waveform seek bar."},
        {L"Extras", L"A play log with cordinal points and a year in review, and two games played with your own music."},
    };
    // Layout in pixels, filled by Measure().
    int width = 0, height = 0, pad = 0, iconSize = 0, textLeft = 0, labelWidth = 0, bodyLeft = 0;
    int titleTop = 0, versionTop = 0, taglineTop = 0, ruleY = 0, rowsTop = 0, rowGap = 0, footerTop = 0;
    RECT iconRect{};
} a;

int TextHeight(HDC dc, HFONT font, const wchar_t* text, int width) {
    HGDIOBJ old = SelectObject(dc, font);
    RECT r{0, 0, width, 0};
    DrawTextW(dc, text, -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    return r.bottom;
}

// Fixes every position for the current DPI and text. The window is a fixed width; its height is
// whatever the wrapped text needs.
void Measure() {
    HDC dc = GetDC(nullptr);
    a.pad = S(24);
    a.width = S(500);
    a.iconSize = S(64);
    a.textLeft = a.pad + a.iconSize + S(18);
    a.titleTop = a.pad;
    a.versionTop = a.titleTop + S(36);
    a.taglineTop = a.versionTop + S(28);
    const int taglineHeight = TextHeight(dc, g.font, kAboutTagline, a.width - a.pad - a.textLeft);
    a.iconRect = {a.pad, a.pad + S(2), a.pad + a.iconSize, a.pad + S(2) + a.iconSize};
    a.ruleY = std::max<int>(a.taglineTop + taglineHeight, a.iconRect.bottom) + S(22);
    a.rowsTop = a.ruleY + S(20);
    a.labelWidth = S(76);
    a.bodyLeft = a.pad + a.labelWidth + S(12);
    a.rowGap = S(14);
    int y = a.rowsTop;
    for (AboutRow& row : a.rows) {
        row.height = std::max(TextHeight(dc, g.font, row.text, a.width - a.pad - a.bodyLeft),
                              TextHeight(dc, g.fontBold, row.label, a.labelWidth));
        y += row.height + a.rowGap;
    }
    a.footerTop = y - a.rowGap + S(22);
    a.height = a.footerTop + S(64);
    ReleaseDC(nullptr, dc);
}

void DrawAboutText(HDC dc, HFONT font, COLORREF color, const wchar_t* text, int x, int y, int width, UINT flags = 0) {
    HGDIOBJ old = SelectObject(dc, font);
    SetTextColor(dc, color);
    RECT r{x, y, x + width, y + S(400)};
    DrawTextW(dc, text, -1, &r, DT_NOPREFIX | flags);
    SelectObject(dc, old);
}

void PaintAbout(HDC dc, RECT rc) {
    FillSolid(dc, rc, WindowBg());
    SetBkMode(dc, TRANSPARENT);
    const int right = a.width - a.pad;
    if (a.icon) DrawIconEx(dc, a.iconRect.left, a.iconRect.top, a.icon, a.iconSize, a.iconSize, 0, nullptr, DI_NORMAL);
    DrawAboutText(dc, a.titleFont, TextColor(), L"cordinal", a.textLeft, a.titleTop, right - a.textLeft, DT_SINGLELINE);
    DrawAboutText(dc, g.font, SubColor(), L"Version 0.1.3", a.textLeft, a.versionTop, right - a.textLeft, DT_SINGLELINE);
    DrawAboutText(dc, g.font, TextColor(), kAboutTagline, a.textLeft, a.taglineTop, right - a.textLeft, DT_WORDBREAK);
    FillSolid(dc, {a.pad, a.ruleY, right, a.ruleY + 1}, Ref(P().border));
    int y = a.rowsTop;
    for (const AboutRow& row : a.rows) {
        DrawAboutText(dc, g.fontBold, TextColor(), row.label, a.pad, y, a.labelWidth);
        DrawAboutText(dc, g.font, SubColor(), row.text, a.bodyLeft, y, right - a.bodyLeft, DT_WORDBREAK);
        y += row.height + a.rowGap;
    }
    FillSolid(dc, {0, a.footerTop, rc.right, rc.bottom}, SurfaceColor());
    FillSolid(dc, {0, a.footerTop, rc.right, a.footerTop + 1}, Ref(P().border));
    DrawAboutText(dc, g.font, SubColor(), a.stats.c_str(), a.pad, a.footerTop + S(14), right - a.pad - S(100), DT_WORDBREAK);
}

void CloseAbout(HWND h) {
    EnableWindow(g.hwnd, TRUE);  // before the box goes, so the main window is what becomes active
    DestroyWindow(h);
}

LRESULT CALLBACK AboutProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            a.hwnd = h;
            a.ok = CreateWindowExW(0, WC_BUTTONW, L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, a.width - a.pad - S(88),
                                   a.footerTop + S(17), S(88), S(30), h, (HMENU)(INT_PTR)IDOK, g.inst, nullptr);
            SendMessageW(a.ok, WM_SETFONT, (WPARAM)g.font, TRUE);
            ApplyControlTheme(a.ok, g.dark);
            ApplyWindowTheme(h, P());
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            DoubleBufferedPaint(h, PaintAbout);
            return 0;
        case WM_CTLCOLORBTN:
            return SurfaceControlColor((HDC)wp, TextColor());
        case WM_SETCURSOR: {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(h, &pt);
            if (LOWORD(lp) == HTCLIENT && PtInRect(&a.iconRect, pt)) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        }
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (PtInRect(&a.iconRect, pt)) PlaySfx(Sfx::CardinalSong, 0, true);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) CloseAbout(h);
            return 0;
        case WM_CLOSE:
            CloseAbout(h);
            return 0;
        case WM_DESTROY:
            a.hwnd = nullptr;
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

void ShowAbout() {
    if (a.hwnd) {
        SetForegroundWindow(a.hwnd);
        return;
    }
    PROCESS_MEMORY_COUNTERS_EX pmc{sizeof(pmc)};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
    wchar_t stats[200];
    swprintf_s(stats, L"Library: %zu tracks in %.1f KB\nMemory: %.1f MB committed, %.1f MB working set", g_lib.Count(),
               g_lib.MemoryBytes() / 1024.0, pmc.PrivateUsage / 1048576.0, pmc.WorkingSetSize / 1048576.0);
    a.stats = stats;
    a.icon = (HICON)LoadImageW(g.inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, S(64), S(64), 0);
    LOGFONTW lf{};
    GetObjectW(g.fontBold, sizeof(lf), &lf);
    lf.lfHeight = -S(28);
    a.titleFont = CreateFontIndirectW(&lf);
    Measure();

    RegisterAppClass(kAboutClass, AboutProc, CS_DBLCLKS);
    constexpr DWORD style = WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, exStyle = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
    RECT r{0, 0, a.width, a.height};
    AdjustWindowRectExForDpi(&r, style, FALSE, exStyle, g.dpi);
    RECT owner;
    GetWindowRect(g.hwnd, &owner);
    const int width = r.right - r.left, height = r.bottom - r.top;
    EnableWindow(g.hwnd, FALSE);
    CreateWindowExW(exStyle, kAboutClass, L"About cordinal", style, owner.left + (owner.right - owner.left - width) / 2,
                    owner.top + std::max<int>(S(40), (owner.bottom - owner.top - height) / 3), width, height, g.hwnd, nullptr,
                    g.inst, nullptr);
    if (a.hwnd) {
        ShowWindow(a.hwnd, SW_SHOW);
        SetFocus(a.ok);
        MSG msg{};
        while (a.hwnd && GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (IsDialogMessageW(a.hwnd, &msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (a.hwnd) {  // WM_QUIT arrived while the box was open: let the main loop see it too
            PostQuitMessage((int)msg.wParam);
            DestroyWindow(a.hwnd);
        }
    }
    EnableWindow(g.hwnd, TRUE);
    SetForegroundWindow(g.hwnd);
    if (a.icon) DestroyIcon(a.icon);
    if (a.titleFont) DeleteObject(a.titleFont);
    a.icon = nullptr;
    a.titleFont = nullptr;
}

}  // namespace app
