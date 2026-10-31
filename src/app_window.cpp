// Route native window messages to the application features.
#include "app.h"

namespace app {

// WM_DPICHANGED: rebuild fonts, rescale the stored column widths by the DPI ratio and
// adopt the rect Windows suggests. Art is reloaded because its pixel size is DPI-based.
void MainDpiChanged(HWND hwnd, WPARAM wp, LPARAM lp) {
    EndRename(true);
    UINT oldDpi = g.dpi;
    g.dpi = HIWORD(wp);
    CreateFonts();
    ApplyFonts();
    for (int i = 0; i < (int)g.columns.size(); ++i)
        ListView_SetColumnWidth(g.list, i, MulDiv(ListView_GetColumnWidth(g.list, i), (int)g.dpi, (int)oldDpi));
    auto* r = reinterpret_cast<RECT*>(lp);
    SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
    g.artFor = kNone;
    LoadArtFor(g.playing);
    LibViewDpiChanged();
    TaskbarDpiChanged();
    TrayDpiChanged();
    Layout();
}

// WM_NOTIFY dispatch by source control; returns the handler's result, or 0 if none.
LRESULT MainNotify(LPARAM lp) {
    auto* hdr = reinterpret_cast<NMHDR*>(lp);
    if (hdr->hwndFrom == g.list) return OnListNotify(hdr);
    for (int i = 0; i < 3; ++i)
        if (hdr->hwndFrom == g.panes[i].list) return OnBrowserNotify(i, hdr);
    if (hdr->hwndFrom == g.tabs && hdr->code == TCN_SELCHANGE) ActivatePlaylist(TabCtrl_GetCurSel(g.tabs));
    if ((hdr->hwndFrom == g.seek || hdr->hwndFrom == g.volume) && hdr->code == NM_CUSTOMDRAW)
        return DrawTrackbar(reinterpret_cast<NMCUSTOMDRAW*>(lp));
    return 0;
}

// WM_HSCROLL for the seek and volume trackbars. Seek positions are in per-mille of the
// current track, so seconds = pos/1000 * duration. A thumb drag previews the time and
// seeks once on TB_ENDTRACK; a channel click seeks immediately.
void MainScroll(WPARAM wp, LPARAM lp) {
    HWND src = (HWND)lp;
    int code = LOWORD(wp);
    if (src == g.volume) {
        SetVolumePercent((int)SendMessageW(g.volume, TBM_GETPOS, 0, 0));
    } else if (src == g.seek) {
        int pos = (int)SendMessageW(g.seek, TBM_GETPOS, 0, 0);
        double target = pos / 1000.0 * CurrentDuration();
        if (code == TB_THUMBTRACK) {
            g.seeking = true;
            ShowPlaybackTime(target, CurrentDuration());
        } else if (code == TB_ENDTRACK) {
            if (g.seeking) RequestSeek(target);  // the drag is over: the one seek it causes
            g.seeking = false;
        } else {
            RequestSeek(target);
        }
    }
}

// WM_DRAWITEM for the owner-drawn controls: library view buttons, toolbar buttons and
// the three status-bar parts (part 1 may hold the clickable status chips).
bool DrawMainItem(LPARAM lp) {
    auto* di = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
    if (di->hwndItem == RadioTab()) { DrawRadioTab(di); return TRUE; }
    if (di->CtlType == ODT_BUTTON && di->CtlID >= IDC_VIEW0 && di->CtlID < IDC_VIEW0 + kViewButtons) {
        DrawViewButton(di);
        return TRUE;
    }
    if (di->CtlType == ODT_BUTTON) {
        DrawToolbarButton(di);
        return TRUE;
    }
    if (di->hwndItem == g.status && ApolloMode()) {
        ApolloDrawStatusPart(di);
        return TRUE;
    }
    if (di->hwndItem == g.status) {
        int part = (int)di->itemData;
        FillSolid(di->hDC, di->rcItem, SurfaceColor());
        // Hairline separator between parts.
        if (part > 0)
            FillSolid(di->hDC, {di->rcItem.left, di->rcItem.top + S(3), di->rcItem.left + 1, di->rcItem.bottom - S(3)},
                      Ref(P().border));
        SetBkMode(di->hDC, TRANSPARENT);
        // Same regular font in both themes so status length/layout never
        // shifts when toggling dark mode; only the color differs.
        bool primary = part == 0;
        SetTextColor(di->hDC, FoobarMode() ? Ref(0xC0C0C0) : primary ? TextColor() : SubColor());
        RECT r = di->rcItem;
        r.left += S(6);
        r.right -= S(4);
        const std::wstring& text = part >= 0 && part < 3 ? g.statusParts[part] : std::wstring();
        if (part == 1 && !g.statusItems.empty()) {
            DrawStatusItems(di->hDC, r);
            return TRUE;
        }
        DrawTextW(di->hDC, text.c_str(), (int)text.size(), &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        return TRUE;
    }
    return false;
}

// What a right-click in the normal window opens the layout menu on: the player, the now playing
// line, the status bar and the window's own background.
bool IsPlayerPart(HWND h) {
    for (HWND b : g.buttons)
        if (h == b) return true;
    for (HWND p : {g.hwnd, g.seek, g.volume, g.speaker, g.time, g.elapsed, g.npTitle, g.npSub, g.pointsChip, g.status})
        if (h == p) return true;
    return false;
}

// WM_CONTEXTMENU routing: browser headers first, then the list header / list / tabs,
// then the compact and big-mode menus (only the normal window has a menu bar). Returns
// false to let the default menu handling run.
bool MainContextMenu(WPARAM wp, LPARAM lp) {
    POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    for (int i = 0; i < 3; ++i)
        if ((HWND)wp == ListView_GetHeader(g.panes[i].list)) {
            ShowBrowserFieldMenu(i, pt);
            return true;
        }
    if ((HWND)wp == g.header) ShowColumnMenu(pt);
    else if ((HWND)wp == g.list || (HWND)wp == g.libView) ShowListContextMenu(pt);
    else if ((HWND)wp == g.tabs) ShowTabContextMenu(pt);
    else if (ApolloContextMenu((HWND)wp, pt)) return true;  // the rebar: Apollo's options
    else if (g.size != ViewSize::Normal && ((HWND)wp == g.hwnd || (HWND)wp == g.npTitle || (HWND)wp == g.npSub ||
                                            (HWND)wp == NowPlayingPanel() || (HWND)wp == UpNextPanel()))
        ShowCompactMenu(pt);  // the menu bar's menus: only the normal window has the bar
    else if (g.size == ViewSize::Normal && IsPlayerPart((HWND)wp))
        ShowLayoutMenu(pt);  // the normal window's player and now playing line: customize them
    else return false;
    return true;
}

// The main window procedure. Cross-cutting hooks (dark menu bar, taskbar/tray/platform,
// big-mode mouse) run before the switch; unmapped messages reach DefWindowProcW.
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT menuResult = 0;
    if (HandleDarkMenuBar(hwnd, msg, wp, lp, menuResult)) return menuResult;
    if (OnTaskbarMessage(msg) || OnTrayMessage(msg, wp, lp) || OnPlatformMessage(msg, wp, lp)) return 0;
    if (g.size == ViewSize::Big) {  // big mode's tabs and splitters (app_big.cpp)
        if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST && msg != WM_MOUSEWHEEL && BigMouse(msg, wp, lp)) return 0;
        if (msg == WM_MOUSELEAVE || msg == WM_CAPTURECHANGED) BigMouse(msg, wp, lp);
    }
    switch (msg) {
        case WM_CREATE: CreateMainWindow(hwnd); return 0;  // builds the whole UI, once
        case WM_CHANGEUISTATE:
            if (!g.showFocusBoxes && LOWORD(wp) == UIS_CLEAR && (HIWORD(wp) & UISF_HIDEFOCUS)) {
                UINT flags = HIWORD(wp) & ~UISF_HIDEFOCUS;
                if (!flags) return 0;
                wp = MAKEWPARAM(UIS_CLEAR, flags);
            }
            break;
        case WM_SHOWWINDOW:
            if (!wp) ReleaseGroupArt();
            break;
        case WM_SIZE:
            if (wp == SIZE_MINIMIZED && !g.closing) {
                TrimMemory();
                return 0;
            }
            Layout();
            return 0;
        case WM_GETMINMAXINFO: {
            // Minimum track size, indexed by the current ViewSize.
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            const POINT mins[] = {{S(360), S(320)}, {S(640), S(360)}, {S(560), S(480)}, {S(800), S(600)}};
            mmi->ptMinTrackSize = mins[(int)g.size];
            return 0;
        }
        case WM_DPICHANGED: MainDpiChanged(hwnd, wp, lp); return 0;
        case WM_SETTINGCHANGE:
            // ImmersiveColorSet is broadcast when the OS light/dark setting changes.
            if (lp && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0) {
                if (g.themeMode == ThemeMode::System) ApplyTheme();
                TrayThemeChanged();  // the taskbar may have switched light or dark
            }
            break;
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect((HDC)wp, &rc, g.surfaceBrush);
            return 1;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            // PrintWindow can supply a DC here without creating a normal update region.
            HDC dc = wp ? (HDC)wp : BeginPaint(hwnd, &ps);
            if (g.size == ViewSize::Big) PaintBig(dc);
            else if (!IsRectEmpty(&g.artRect)) PaintArt(dc);
            if (FoobarMode()) PaintFoobarChrome(dc);  // the toolbar's visualization boxes
            else if (ApolloMode()) PaintApolloChrome(dc);  // the rebar
            else if (g.size == ViewSize::Normal) PaintLayoutChrome(dc);  // dividers and the player's handle
            GdiFlush();
            if (!wp) EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_PRINTCLIENT: {
            HDC dc = (HDC)wp;
            RECT client;
            GetClientRect(hwnd, &client);
            FillRect(dc, &client, g.surfaceBrush);
            if (g.size == ViewSize::Big) PaintBig(dc);
            else if (!IsRectEmpty(&g.artRect)) PaintArt(dc);
            if (FoobarMode()) PaintFoobarChrome(dc);
            else if (ApolloMode()) PaintApolloChrome(dc);
            else if (g.size == ViewSize::Normal) PaintLayoutChrome(dc);
            GdiFlush();
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = (HDC)wp;
            HWND ctl = (HWND)lp;
            SetBkColor(dc, SurfaceColor());
            SetTextColor(dc, ctl == g.npSub || ctl == g.time || ctl == g.elapsed || ctl == g.speaker ? SubColor() : TextColor());
            return (LRESULT)g.surfaceBrush;
        }
        case WM_CTLCOLOREDIT:
            if ((HWND)lp == g.rename) {
                if (!FoobarMode()) return SurfaceControlColor((HDC)wp, TextColor());
                HDC dc = (HDC)wp;
                COLORREF bg = Ref(P().border);
                SetTextColor(dc, Ref(kFoobarChromeText));
                SetBkColor(dc, bg);
                SetDCBrushColor(dc, bg);
                return (LRESULT)GetStockObject(DC_BRUSH);
            }
            return FieldControlColor((HDC)wp);
        case WM_CTLCOLORLISTBOX:
            return FieldControlColor((HDC)wp);
        case WM_DRAWITEM:
            if (DrawMainItem(lp)) return TRUE;
            break;
        case WM_COMMAND:
            OnCommand(LOWORD(wp), HIWORD(wp), (HWND)lp);
            return 0;
        case WM_NOTIFY: return MainNotify(lp);
        case WM_CONTEXTMENU:
            if (MainContextMenu(wp, lp)) return 0;
            break;
        case WM_INITMENUPOPUP: UpdateMainMenu((HMENU)wp); return 0;
        case WM_SYSCOMMAND:
            // Alt on its own, with the normal window's menu bar hidden: its menus as a popup instead.
            if ((wp & 0xFFF0) == SC_KEYMENU && lp == 0 && g.size == ViewSize::Normal && !GetMenu(hwnd)) {
                POINT pt{0, 0};
                ClientToScreen(hwnd, &pt);
                ShowCompactMenu(pt);
                return 0;
            }
            break;
        case WM_HSCROLL: MainScroll(wp, lp); return 0;
        case WM_TIMER:
            if (wp == kWebTimer) { WebTick(); return 0; }
            if (HelperTimer(wp)) return 0;
            if (wp == kSearchTimer) { FlushSearch(); return 0; }
            if (wp == kUiTimer) MainTick();
            else if (wp == kBusyTimer) BusyTick();
            else if (wp == kStripTimer) StripHoverTick();
            return 0;
        case WM_ENGINE:
            OnEngineEvent((EngineEvent)wp, (unsigned long long)lp);
            return 0;
        case WM_APP_STARTUP: FinishStartup(hwnd); return 0;
        case WM_APP_BACKUP: OnBackupMessage(lp); return 0;
        case WM_APP_HELPER: OnHelperMessage(wp, lp); return 0;
        case WM_APP_WEB: if (wp == 1) OpenWebPlayer(); else WebTick(); return 0;
        case WM_APP_IMPORT: OnImportResult(lp); return 0;
        case WM_APP_ART: OnArtResult(lp); return 0;
        case WM_APP_META: {
            auto* batch = reinterpret_cast<std::vector<TagResult>*>(lp);
            AcceptTagBatch(batch);
            RequestAlbumGrouping();
            g.dirty = true;
            g.statsDirty = true;
            if (!g.filter.empty()) RebuildView(true);
            RefreshGains();
            InvalidateRect(g.list, nullptr, FALSE);
            if (g.playing != kNone) UpdateNowPlaying();
            return 0;
        }
        case WM_APP_ORGANIZE:
            OnOrganizeDone(lp);
            return 0;
        case WM_APP_CONVERT:
            OnConvertMessage(wp, lp);
            return 0;
        case WM_APP_COPY:
            OnCopyMessage(wp, lp);
            return 0;
        case WM_APP_GROUP_ART:
            OnGroupArt();
            return 0;
        case WM_APP_TAGWRITE:
            OnTagWriteMessage(wp, lp);
            return 0;
        case WM_APP_BROWSE: {
            MSG pending;  // one rebuild for a burst of selection changes
            while (PeekMessageW(&pending, hwnd, WM_APP_BROWSE, WM_APP_BROWSE, PM_REMOVE)) {}
            OnBrowserSelection();
            return 0;
        }
        case WM_SETCURSOR:
            if ((HWND)wp == hwnd && LOWORD(lp) == HTCLIENT && g.size == ViewSize::Big && BigSetCursor()) return TRUE;
            if ((HWND)wp == hwnd && LOWORD(lp) == HTCLIENT) {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                if (PtInRect(&g.splitRect, pt)) {
                    SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
                    return TRUE;
                }
                if (LayoutSetCursor(pt)) return TRUE;
            }
            break;
        case WM_LBUTTONDOWN: {
            if (LayoutMouse(msg, lp)) return 0;  // the player's handle (app_custom.cpp)
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (PtInRect(&g.artRect, pt)) {
                if (!Resume()) RevealPlayingTrack();
                return 0;
            }
            if (PtInRect(&g.splitRect, pt)) {
                g.splitting = true;
                SetCapture(hwnd);
            }
            return 0;
        }
        case WM_MOUSEMOVE:
            if (LayoutMouse(msg, lp)) return 0;
            if (g.splitting) {
                RECT pane;
                GetWindowRect(g.panes[0].list, &pane);
                MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT*>(&pane), 2);
                g.browserHeight = std::max(60, MulDiv(GET_Y_LPARAM(lp) - pane.top, 96, (int)g.dpi));
                Layout();
            }
            return 0;
        case WM_LBUTTONUP:
        case WM_CAPTURECHANGED:
            if (LayoutMouse(msg, lp)) return 0;
            if (g.splitting) {
                g.splitting = false;
                SaveBrowserSettings();
                if (msg == WM_LBUTTONUP) ReleaseCapture();
            }
            return 0;
        case WM_APP_SCAN:
            OnScanResult(reinterpret_cast<ScanResult*>(lp), wp == 1);
            return 0;
        case WM_APP_RG: FinishReplayGain(wp, lp); return 0;
        case WM_DROPFILES: {
            HDROP drop = (HDROP)wp;
            UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            std::vector<std::wstring> paths;
            for (UINT i = 0; i < count; ++i) {
                UINT len = DragQueryFileW(drop, i, nullptr, 0);
                std::wstring p(len + 1, L'\0');
                DragQueryFileW(drop, i, p.data(), len + 1);
                p.resize(len);
                paths.push_back(p);
            }
            DragFinish(drop);
            PlaySfx(Sfx::Drop, (int)paths.size());
            AddPaths(paths, false);
            return 0;
        }
        // Paths pushed here by a second launch (see ForwardToRunningInstance). Validate
        // the tag, a whole-wchar payload and a sane 16 MB cap before trusting it.
        case WM_COPYDATA: {
            auto* cds = reinterpret_cast<COPYDATASTRUCT*>(lp);
            if (cds && cds->dwData == kCopyDataCommand && cds->lpData && cds->cbData % sizeof(wchar_t) == 0 &&
                cds->cbData <= 64 * sizeof(wchar_t)) {
                RunTransportCommand({static_cast<const wchar_t*>(cds->lpData), cds->cbData / sizeof(wchar_t)});
                return TRUE;
            }
            if (!cds || (cds->dwData != kCopyDataPaths && cds->dwData != kCopyDataAddPaths) || !cds->lpData ||
                cds->cbData % sizeof(wchar_t) || cds->cbData > (16u << 20)) return FALSE;
            std::wstring all(static_cast<const wchar_t*>(cds->lpData), cds->cbData / sizeof(wchar_t));
            std::vector<std::wstring> paths;
            for (size_t start = 0; start < all.size();) {
                size_t end = all.find(L'\n', start);
                if (end == std::wstring::npos) end = all.size();
                if (end > start) paths.push_back(all.substr(start, end - start));
                start = end + 1;
            }
            AddPaths(paths, cds->dwData == kCopyDataPaths);
            return TRUE;
        }
        // Registered global hotkeys: 1 play/pause, 2 next, 3 previous, 4 stop.
        case WM_HOTKEY:
            switch (wp) {
                case 1: TogglePause(); break;
                case 2: Next(); break;
                case 3: Previous(); break;
                case 4: Stop(); break;
                default: PackHotkey((int)wp); break;  // a pack's global key (keys.ini [global keys])
            }
            return 0;
        // Media keys delivered by the shell / keyboards (APPCOMMAND_*).
        case WM_APPCOMMAND:
            switch (GET_APPCOMMAND_LPARAM(lp)) {
                case APPCOMMAND_MEDIA_PLAY_PAUSE: TogglePause(); return TRUE;
                case APPCOMMAND_MEDIA_PLAY: PlayButton(); return TRUE;
                case APPCOMMAND_MEDIA_PAUSE: if (!g.engine.Paused()) TogglePause(); return TRUE;
                case APPCOMMAND_MEDIA_NEXTTRACK: Next(); return TRUE;
                case APPCOMMAND_MEDIA_PREVIOUSTRACK: Previous(); return TRUE;
                case APPCOMMAND_MEDIA_STOP: Stop(); return TRUE;
            }
            break;
        case WM_QUERYENDSESSION:
            // Windows logoff/restart does not send WM_CLOSE. Save while the controls,
            // placement and playback position are still available, even if logoff is cancelled.
            FlushSearch();
            SaveState();
            return TRUE;
        case WM_ENDSESSION:
            if (wp) {
                FinishListening(true);  // the listen in progress counts before the process goes
                StopAppWorkers(hwnd);
                // Release the engine's files before writing tag edits that waited for a playing
                // track. Stop() first records the resume position the saved session should keep.
                if (g.playing != kNone || g.playingToken || g.decodingToken || g.resumeToken) Stop();
                g.engine.Shutdown();
                std::wstring ignored;
                FlushDeferredTagWrites(ignored);
                SaveState();
                if (g.dirty) KeepUnsavedLibrary();  // no time to ask: the next start restores it
            }
            return 0;
        case WM_CLOSE: {
            // Shutdown order matters: end text edits, apply pending folder-window changes
            // (destroying it commits them), stop workers, then release the engine and write any
            // tag edit that waited for a playing file. Only then save, so the saved state
            // reflects the settled library and files. A failed save is kept or asked about
            // before the window goes. Credits the listen in progress before the engine stops.
            EndRename(true);
            FlushSearch();
            if (g.foldersWnd) DestroyWindow(g.foldersWnd);  // applies pending folder changes first
            FinishListening(true);
            StopAppWorkers(hwnd);
            if (g.playing != kNone || g.playingToken || g.decodingToken || g.resumeToken) Stop();
            g.engine.Shutdown();
            std::wstring tagError;
            FlushDeferredTagWrites(tagError);
            SaveBeforeClose();
            if (!tagError.empty()) {
                const std::wstring text =
                    L"Some tag edits for files that were playing couldn't be saved before closing:\n\n" + tagError;
                MessageBoxW(g.hwnd, text.c_str(), kAppName, MB_OK | MB_ICONWARNING);
            }
            if (g.eqWnd) DestroyWindow(g.eqWnd);
            DestroyWindow(hwnd);
            return 0;
        }
        case WM_DESTROY: DestroyMainWindow(hwnd); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace app
