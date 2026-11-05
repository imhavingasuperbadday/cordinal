// The dark menu bar: the undocumented UAH messages Windows uses to draw the bar and its items.
#include "app_ui_internal.h"

namespace app {

constexpr UINT WM_UAHDRAWMENU = 0x0091, WM_UAHDRAWMENUITEM = 0x0092;
struct UahMenu {
    HMENU hmenu;
    HDC hdc;
    DWORD flags;
};
struct UahMenuItem {
    int position;
    DWORD metrics[8];
    DWORD popupMetrics[5];
};
struct UahDrawMenuItem {
    DRAWITEMSTRUCT dis;
    UahMenu um;
    UahMenuItem umi;
};

bool HandleDarkMenuBar(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT& result) {
    if (!g.dark) return false;
    switch (msg) {
        case WM_UAHDRAWMENU: {
            auto* um = reinterpret_cast<UahMenu*>(lp);
            MENUBARINFO mbi{sizeof(mbi)};
            GetMenuBarInfo(hwnd, OBJID_MENU, 0, &mbi);
            RECT wr;
            GetWindowRect(hwnd, &wr);
            RECT rc = mbi.rcBar;
            OffsetRect(&rc, -wr.left, -wr.top);
            FillRect(um->hdc, &rc, g.surfaceBrush);
            result = TRUE;
            return true;
        }
        case WM_UAHDRAWMENUITEM: {
            auto* item = reinterpret_cast<UahDrawMenuItem*>(lp);
            wchar_t text[128] = {};
            MENUITEMINFOW mii{sizeof(mii)};
            mii.fMask = MIIM_STRING;
            mii.dwTypeData = text;
            mii.cch = (UINT)std::size(text) - 1;
            GetMenuItemInfoW(item->um.hmenu, item->umi.position, TRUE, &mii);
            UINT state = item->dis.itemState;
            bool hot = state & (ODS_HOTLIGHT | ODS_SELECTED);
            FillSolid(item->um.hdc, item->dis.rcItem, hot ? Ref(P().hover) : Ref(P().surface));
            SetBkMode(item->um.hdc, TRANSPARENT);
            SetTextColor(item->um.hdc, (state & (ODS_INACTIVE | ODS_GRAYED)) ? Ref(P().faint)
                                       : g.themeMode == ThemeMode::Foobar ? Ref(kFoobarChromeText) : Ref(P().text));
            HFONT menuFont = MenuFont();
            HGDIOBJ old = SelectObject(item->um.hdc, menuFont ? menuFont : g.font);
            DrawTextW(item->um.hdc, text, -1, &item->dis.rcItem,
                      DT_CENTER | DT_SINGLELINE | DT_VCENTER | ((state & ODS_NOACCEL) ? DT_HIDEPREFIX : 0));
            SelectObject(item->um.hdc, old);
            result = TRUE;
            return true;
        }
        case WM_NCPAINT:
        case WM_NCACTIVATE: {
            result = DefWindowProcW(hwnd, msg, wp, lp);
            // Paint over the light line Windows draws under the menu bar.
            MENUBARINFO mbi{sizeof(mbi)};
            if (!GetMenuBarInfo(hwnd, OBJID_MENU, 0, &mbi)) return true;
            RECT client, wr;
            GetClientRect(hwnd, &client);
            MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&client), 2);
            GetWindowRect(hwnd, &wr);
            OffsetRect(&client, -wr.left, -wr.top);
            RECT line = client;
            line.bottom = line.top;
            line.top--;
            HDC dc = GetWindowDC(hwnd);
            FillRect(dc, &line, g.surfaceBrush);
            ReleaseDC(hwnd, dc);
            return true;
        }
    }
    return false;
}

}  // namespace app
