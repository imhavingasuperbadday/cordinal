// Shell-facing commands that don't belong to a panel: the common file/folder open dialog, m3u
// playlist import and export, "Show in Explorer" / Properties on the selection, and the About box
// (with its clickable cardinal, installed via a one-shot CBT hook).
#include "app.h"

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

HHOOK g_aboutHook = nullptr;

// Clicking the cardinal in the About box makes it sing (sound effects on or off). The icon is a
// static control, which lets clicks fall through to the box itself.
LRESULT CALLBACK AboutSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        for (HWND c = GetWindow(h, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
            wchar_t cls[16];
            if (!GetClassNameW(c, cls, 16) || _wcsicmp(cls, L"Static") || (GetWindowLongW(c, GWL_STYLE) & SS_TYPEMASK) != SS_ICON)
                continue;
            RECT r;
            GetWindowRect(c, &r);
            MapWindowPoints(nullptr, h, reinterpret_cast<POINT*>(&r), 2);
            if (PtInRect(&r, pt)) PlaySfx(Sfx::CardinalSong, 0, true);
        }
    } else if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(h, AboutSubclass, 1);
    }
    return DefSubclassProc(h, msg, wp, lp);
}

LRESULT CALLBACK AboutHook(int code, WPARAM wp, LPARAM lp) {
    wchar_t cls[16];
    if (code == HCBT_ACTIVATE && GetClassNameW((HWND)wp, cls, 16) && !wcscmp(cls, L"#32770")) {
        SetWindowSubclass((HWND)wp, AboutSubclass, 1, 0);
        UnhookWindowsHookEx(g_aboutHook);
        g_aboutHook = nullptr;
        return 0;
    }
    return CallNextHookEx(g_aboutHook, code, wp, lp);
}

}  // namespace

void ShowAbout() {
    PROCESS_MEMORY_COUNTERS_EX pmc{sizeof(pmc)};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
    wchar_t mem[200];
    swprintf_s(mem, L"Memory: %.1f MB committed, %.1f MB working set\nLibrary: %zu tracks in %.1f KB",
               pmc.PrivateUsage / 1048576.0, pmc.WorkingSetSize / 1048576.0, g_lib.Count(), g_lib.MemoryBytes() / 1024.0);
    std::wstring text = std::wstring(L"cordinal 0.1.1\n\nA fast, lightweight music player and library for Windows and Linux.\n\n"
                                     L"Watched library folders with an Inbox, a column browser, album and artist views, "
                                     L"auto-playlists, ratings and play counts, a tag editor and file organizer.\n"
                                     L"Cordinal decoders for FLAC, WAV, AIFF, MPEG audio, Vorbis, AAC-LC, ALAC, "
                                     L"WMA Standard, Opus, APE, WavPack and AC3, with Matroska audio support.\n"
                                     L"Gapless playback, ReplayGain with an EBU R128 scanner, 10-band equalizer, "
                                     L"exclusive (bit-perfect) output, a waveform seek bar.\n"
                                     L"A play log with cordinal points and a year in review; two games played with "
                                     L"your own music.\n\n") +
                        mem;
    MSGBOXPARAMSW mb{sizeof(mb)};
    mb.hwndOwner = g.hwnd;
    mb.hInstance = g.inst;
    mb.lpszText = text.c_str();
    mb.lpszCaption = L"About cordinal";
    mb.dwStyle = MB_OK | MB_USERICON;
    mb.lpszIcon = MAKEINTRESOURCEW(IDI_APP);
    g_aboutHook = SetWindowsHookExW(WH_CBT, AboutHook, nullptr, GetCurrentThreadId());
    MessageBoxIndirectW(&mb);
    if (g_aboutHook) UnhookWindowsHookEx(g_aboutHook);  // the box never activated
    g_aboutHook = nullptr;
}

}  // namespace app
