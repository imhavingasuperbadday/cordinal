// The Library tab's visual views' input (libview_internal.h): hit testing, selection, smooth scrolling,
// clicks, keys, the edition picker and the view's window procedure.
#include "libview_internal.h"

namespace app::lv {

// ---------------------------------------------------------------------------
// Hit testing and selection

struct Hit {
    int item = -1, row = -1;
    bool play = false, close = false, stars = false, editions = false;
    float starsX = 0;
};

// Maps a client point to an item and, inside album rows, to a track row and whether the click hit
// the star rating widget (whose geometry mirrors DrawTrackRow).
Hit HitTest(int x, int y) {
    Hit h;
    float cx = (float)x, cy = (float)(y + v.scroll);
    for (int i = 0; i < (int)v.items.size(); ++i) {
        const Item& it = v.items[i];
        D2D1_RECT_F r = it.rc;
        if (it.kind == ItemKind::Tile || it.kind == ItemKind::ArtistTile) r = {r.left - F(6), r.top - F(6), r.right + F(6), r.bottom};
        if (cx < r.left || cx >= r.right || cy < r.top || cy >= r.bottom) continue;
        h.item = i;
        auto rowAt = [&](D2D1_RECT_F row, int t) {
            if (cx >= row.left && cx < row.right && cy >= row.top && cy < row.bottom) {
                h.row = t;
                // Stars sit 56..120 px from the row's right edge when the row is wide enough to
                // show them (see DrawTrackRow).
                float starsRight = row.right - F(6) - F(56);
                if (RowStars(row) && cx >= starsRight - F(64) && cx < starsRight + F(4)) {
                    h.stars = true;
                    h.starsX = cx - (starsRight - F(64));
                }
            }
        };
        if (it.kind == ItemKind::Tile) {
            D2D1_RECT_F b = PlayButton(it);
            h.play = cx >= b.left && cx < b.right && cy >= b.top && cy < b.bottom;
        } else if (it.kind == ItemKind::Panel) {
            D2D1_RECT_F c = PanelClose(it);
            h.close = cx >= c.left && cx < c.right && cy >= c.top && cy < c.bottom;
            const Album& a = v.albums[it.index];
            if (HasEditionSwitch(a)) {
                D2D1_RECT_F chip = PanelEditionRect(it);
                h.editions = cx >= chip.left && cx < chip.right && cy >= chip.top && cy < chip.bottom;
            }
            D2D1_RECT_F area = PanelArea(it);
            for (int t = 0; t < (int)a.trackCount && h.row < 0; ++t) rowAt(PanelRow(a, area, t), t);
        } else if (it.kind == ItemKind::AlbumGroup) {
            const Album& a = v.albums[it.index];
            for (int t = 0; t < (int)a.trackCount && h.row < 0; ++t) rowAt(GroupRow(it.rc, t), t);
        }
        return h;
    }
    return h;
}

void ClearCanvasSelection() {
    std::fill(v.selAlbum.begin(), v.selAlbum.end(), (char)0);
    std::fill(v.selArtist.begin(), v.selArtist.end(), (char)0);
    v.selTrack.clear();
}

int MaxScroll() { return std::max(0, (int)v.contentHeight - v.height); }

void PlaceScroll(int y) {
    y = std::clamp(y, 0, MaxScroll());
    if (y == v.scroll) return;
    v.scroll = y;
    SetScrollPos(v.hwnd, SB_VERT, v.scroll, TRUE);
    InvalidateRect(v.hwnd, nullptr, FALSE);
}

void StopSmoothScroll() {
    if (v.scrollAnimating && v.hwnd) KillTimer(v.hwnd, kScrollTimer);
    v.scrollAnimating = false;
}

// Jumps at once; scrollbar drags, focus moves and restored positions use this.
void ScrollTo(int y) {
    StopSmoothScroll();
    PlaceScroll(y);
}

// Wheel and paging with Smooth scrolling on: the goal moves now and the timer eases the view to it.
// Repeated wheel notches add to the goal that is already pending, so a fast spin keeps going.
void SmoothScrollTo(int y) {
    if (!g.smoothScroll || !v.hwnd) {
        ScrollTo(y);
        return;
    }
    v.scrollGoal = std::clamp(y, 0, MaxScroll());
    if (v.scrollGoal == v.scroll) {
        StopSmoothScroll();
        return;
    }
    if (!v.scrollAnimating && SetTimer(v.hwnd, kScrollTimer, 16, nullptr)) v.scrollAnimating = true;
}

// One frame: close a quarter of the gap, at least one pixel, so the view lands exactly on the goal.
void StepSmoothScroll() {
    v.scrollGoal = std::clamp(v.scrollGoal, 0, MaxScroll());  // the content may have shrunk meanwhile
    int gap = v.scrollGoal - v.scroll;
    if (gap == 0) {
        StopSmoothScroll();
        return;
    }
    int step = gap / 4;
    if (step == 0) step = gap > 0 ? 1 : -1;
    PlaceScroll(v.scroll + step);
    if (v.scroll == v.scrollGoal) StopSmoothScroll();
}

void EnsureVisible(const D2D1_RECT_F& r) {
    if (r.top - F(10) < v.scroll) ScrollTo((int)(r.top - F(10)));
    else if (r.bottom + F(10) > v.scroll + v.height) ScrollTo((int)(r.bottom + F(10) - v.height));
}

// Rows of Albums & Tracks in order: (album, track) pairs, for keyboard navigation.
std::vector<std::pair<int, int>> AllRows() {
    std::vector<std::pair<int, int>> rows;
    for (int a = 0; a < (int)v.albums.size(); ++a)
        for (int t = 0; t < (int)v.albums[a].trackCount; ++t) rows.emplace_back(a, t);
    return rows;
}

std::vector<TrackId> ViewTracks() { return v.albumTracks; }

void PlayAlbum(int album, int track) {
    if (album < 0 || album >= (int)v.albums.size()) return;
    auto slice = Tracks(v.albums[album]);
    std::vector<TrackId> items(slice.begin(), slice.end());
    PlayList(items, track);
}

void ShowEditionPicker(int album, POINT pt) {
    if (album < 0 || album >= (int)v.albums.size() || !v.albums[album].group) return;
    const Album& shown = v.albums[album];
    const AlbumGroup* group = FindAlbumGroupConst(shown.group);
    if (!group) return;
    HMENU menu = CreatePopupMenu();
    int current = 0;
    for (size_t i = 0; i < group->editions.size(); ++i) {
        const AlbumEdition& edition = group->editions[i];
        std::wstring label = EditionLabel(edition) + L"  (" + std::to_wstring(edition.tracks.size()) +
                             (edition.tracks.size() == 1 ? L" track)" : L" tracks)");
        AppendMenuW(menu, MF_STRING, 100 + (int)i, label.c_str());
        if (edition.id == shown.edition) current = 100 + (int)i;
    }
    if (current) CheckMenuRadioItem(menu, current, current, current, MF_BYCOMMAND);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 200, L"Set as &preferred edition");
    AppendMenuW(menu, MF_STRING, 201, L"Album &editions...");
    AppendMenuW(menu, MF_STRING, 202, L"&Compare editions...");
    int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g.hwnd, nullptr);
    DestroyMenu(menu);
    if (command >= 100 && command < 200) {
        LibViewChooseEdition(shown.group, group->editions[(size_t)(command - 100)].id);
    } else if (command == 200) {
        if (SetPreferredEdition(shown.group, shown.edition)) {
            LibViewChooseEdition(shown.group, shown.edition);
            AlbumGroupChangedInApp();
        }
    } else if (command == 201) {
        ShowAlbumEditionsDialog(shown.group);
    } else if (command == 202) {
        ShowEditionComparisonDialog(shown.group);
    }
}

void ActivateRow(int album, int track) {
    if (g.libMode == LibraryMode::AlbumTracks) {
        // A track row in Albums & Tracks plays the whole view from there, like the track list.
        auto all = ViewTracks();
        int index = 0;
        for (int a = 0; a < album; ++a) index += (int)v.albums[a].trackCount;
        PlayList(all, index + track);
    } else {
        PlayAlbum(album, track);
    }
}

// Double-click an artist tile: reveal the browser, point its album-artist pane at this artist, and
// switch to Albums. Falls back to the second pane if no pane currently shows album artist.
void ShowArtist(int artist) {
    if (artist < 0 || artist >= (int)v.artists.size()) return;
    StringPool::Handle name = v.artists[artist].name;
    if (!g.browserVisible) {
        g.browserVisible = true;
        Layout();
    }
    // Show the artist through the browser pane listing album artists (or the second pane).
    int pane = 1;
    for (int i = 0; i < 3; ++i)
        if (g.panes[i].field == BrowseField::AlbumArtist) pane = i;
    g.panes[pane].field = BrowseField::AlbumArtist;
    SelectBrowserValue(BrowseField::AlbumArtist, name ? Split1(name) : 0);
    SetLibraryMode(LibraryMode::Albums);
}

void OnClick(int x, int y, bool dbl, WPARAM keys) {
    SetFocus(v.hwnd);
    Hit h = HitTest(x, y);
    bool ctrl = keys & MK_CONTROL, shift = keys & MK_SHIFT;
    if (h.item < 0) {
        if (!ctrl && !shift) ClearCanvasSelection();
        InvalidateRect(v.hwnd, nullptr, FALSE);
        g.statsDirty = true;
        return;
    }
    const Item& it = v.items[h.item];
    switch (it.kind) {
        case ItemKind::Section: break;
        case ItemKind::Tile: {
            int a = it.index;
            if (h.play) {
                PlayAlbum(a, 0);
                break;
            }
            if (dbl) {
                PlayAlbum(a, 0);
                break;
            }
            if (shift && v.focus >= 0 && v.focus < (int)v.albums.size()) {
                if (!ctrl) ClearCanvasSelection();
                for (int i = std::min(v.focus, a); i <= std::max(v.focus, a); ++i) v.selAlbum[i] = 1;
            } else if (ctrl) {
                v.selAlbum[a] = !v.selAlbum[a];
                v.focus = a;
            } else {
                bool wasOpen = v.expanded == a;
                ClearCanvasSelection();
                v.selAlbum[a] = 1;
                v.focus = a;
                v.expanded = wasOpen ? -1 : a;
                const float tileTop = it.rc.top;
                LayoutItems();
                if (v.expanded >= 0)
                    for (auto& p : v.items)
                        if (p.kind == ItemKind::Panel) EnsureVisible({p.rc.left, tileTop, p.rc.right, p.rc.bottom});
            }
            break;
        }
        case ItemKind::Panel:
        case ItemKind::AlbumGroup: {
            const Album& a = v.albums[it.index];
            if (h.close) {
                v.expanded = -1;
                LayoutItems();
                break;
            }
            if (h.editions) {
                POINT pt{x, y};
                ClientToScreen(v.hwnd, &pt);
                ShowEditionPicker(it.index, pt);
                break;
            }
            if (h.row < 0) {
                if (it.kind == ItemKind::AlbumGroup && !dbl) {  // the header/cover selects the album
                    if (!ctrl) ClearCanvasSelection();
                    for (TrackId id : Tracks(a)) v.selTrack.insert(id);
                }
                if (dbl) PlayAlbum(it.index, 0);
                break;
            }
            TrackId id = Tracks(a)[h.row];
            if (dbl) {
                ActivateRow(it.index, h.row);
                break;
            }
            if (h.stars && !ctrl && !shift) {
                double stars = h.starsX / F(12);
                int halves = std::clamp((int)std::ceil(stars * 2), 1, 10);
                uint8_t rating = (uint8_t)(halves * 10);
                SetRating({id}, rating == g_lib[id].rating ? 0 : rating);
                break;
            }
            if (shift && v.anchorTrack != kNone) {
                auto all = ViewTracks();
                auto p1 = std::find(all.begin(), all.end(), v.anchorTrack), p2 = std::find(all.begin(), all.end(), id);
                if (p1 != all.end() && p2 != all.end()) {
                    if (!ctrl) ClearCanvasSelection();
                    if (p1 > p2) std::swap(p1, p2);
                    for (auto p = p1; p <= p2; ++p) v.selTrack.insert(*p);
                }
            } else if (ctrl) {
                if (!v.selTrack.erase(id)) v.selTrack.insert(id);
                v.anchorTrack = id;
            } else {
                ClearCanvasSelection();
                v.selTrack.insert(id);
                v.anchorTrack = id;
            }
            break;
        }
        case ItemKind::ArtistTile: {
            if (dbl) {
                ShowArtist(it.index);
                return;
            }
            if (!ctrl) ClearCanvasSelection();
            v.selArtist[it.index] = ctrl ? !v.selArtist[it.index] : 1;
            v.focus = it.index;
            break;
        }
    }
    g.statsDirty = true;
    InvalidateRect(v.hwnd, nullptr, FALSE);
}

void MoveFocus(int dx, int dy) {
    if (g.libMode == LibraryMode::AlbumTracks) {
        auto rows = AllRows();
        if (rows.empty()) return;
        int cur = -1;
        for (int i = 0; i < (int)rows.size(); ++i)
            if (Tracks(v.albums[rows[i].first])[rows[i].second] == v.anchorTrack) cur = i;
        int next = std::clamp(cur < 0 ? 0 : cur + dy, 0, (int)rows.size() - 1);
        auto [a, t] = rows[next];
        ClearCanvasSelection();
        v.anchorTrack = Tracks(v.albums[a])[t];
        v.selTrack.insert(v.anchorTrack);
        const Item& it = v.items[v.albumItem[a]];
        D2D1_RECT_F row = GroupRow(it.rc, t);
        EnsureVisible({it.rc.left, t == 0 ? it.rc.top : row.top, it.rc.right, row.bottom});
        InvalidateRect(v.hwnd, nullptr, FALSE);
        g.statsDirty = true;
        return;
    }
    bool artists = g.libMode == LibraryMode::Artists;
    int count = artists ? (int)v.artists.size() : (int)v.albums.size();
    if (!count) return;
    int cols;
    float gap;
    GridMetrics(artists ? F(140) : TileArt(), cols, gap);
    int cur = v.focus < 0 ? 0 : v.focus;
    int next = std::clamp(v.focus < 0 ? 0 : cur + dx + dy * cols, 0, count - 1);
    ClearCanvasSelection();
    (artists ? v.selArtist : v.selAlbum)[next] = 1;
    v.focus = next;
    for (auto& it : v.items)
        if ((it.kind == (artists ? ItemKind::ArtistTile : ItemKind::Tile)) && it.index == next) EnsureVisible(it.rc);
    g.statsDirty = true;
    InvalidateRect(v.hwnd, nullptr, FALSE);
}

void OnKey(WPARAM key) {
    bool ctrl = GetKeyState(VK_CONTROL) < 0;
    auto cycleEdition = [&](int direction) {
        if (v.expanded < 0 || v.expanded >= (int)v.albums.size()) return false;
        Album& album = v.albums[v.expanded];
        const AlbumGroup* group = album.group ? FindAlbumGroupConst(album.group) : nullptr;
        if (!group || group->editions.size() < 2) return false;
        int current = 0;
        for (size_t i = 0; i < group->editions.size(); ++i)
            if (group->editions[i].id == album.edition) current = (int)i;
        int next = (current + direction + (int)group->editions.size()) % (int)group->editions.size();
        LibViewChooseEdition(group->id, group->editions[(size_t)next].id);
        return true;
    };
    switch (key) {
        case VK_OEM_4:
            if (cycleEdition(-1)) return;
            break;
        case VK_OEM_6:
            if (cycleEdition(1)) return;
            break;
        case VK_LEFT: MoveFocus(-1, 0); break;
        case VK_RIGHT: MoveFocus(1, 0); break;
        case VK_UP: MoveFocus(0, -1); break;
        case VK_DOWN: MoveFocus(0, 1); break;
        case VK_HOME: SmoothScrollTo(0); break;
        case VK_END: SmoothScrollTo((int)v.contentHeight); break;
        case VK_PRIOR: SmoothScrollTo((v.scrollAnimating ? v.scrollGoal : v.scroll) - v.height + (int)F(40)); break;
        case VK_NEXT: SmoothScrollTo((v.scrollAnimating ? v.scrollGoal : v.scroll) + v.height - (int)F(40)); break;
        case VK_ESCAPE:
            if (v.expanded >= 0) {
                v.expanded = -1;
                LayoutItems();
                InvalidateRect(v.hwnd, nullptr, FALSE);
            }
            break;
        case VK_RETURN:
            if (g.libMode == LibraryMode::Artists && v.focus >= 0) ShowArtist(v.focus);
            else if (g.libMode == LibraryMode::Albums && v.focus >= 0 && v.selTrack.empty()) PlayAlbum(v.focus, 0);
            else PlayFocusedOrFirst();
            break;
        case VK_SPACE: TogglePause(); break;
        case VK_DELETE: ExcludeFromLibrary(SelectedTracks()); break;
        case 'A':
            if (ctrl) LibViewSelectAll();
            break;
    }
}

void UpdateHover(int x, int y) {
    Hit h = HitTest(x, y);
    if (h.editions) h.row = -100;  // the edition chip's own hover state
    if (h.item != v.hoverItem || h.row != v.hoverRow || h.play != v.hoverPlay || h.close != v.hoverClose) {
        v.hoverItem = h.item;
        v.hoverRow = h.row;
        v.hoverPlay = h.play;
        v.hoverClose = h.close;
        InvalidateRect(v.hwnd, nullptr, FALSE);
    }
    SetCursor(LoadCursorW(nullptr, h.play || h.close || h.stars ? IDC_HAND : IDC_ARROW));
}

LRESULT CALLBACK LibViewProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE: {
            v.width = LOWORD(lp);
            v.height = HIWORD(lp);
            if (v.gpu) v.gpu->Resize(D2D1::SizeU(v.width, v.height));
            LayoutItems();
            TrimCovers();
            return 0;
        }
        case WM_SHOWWINDOW:
            if (!wp) {
                StopSmoothScroll();
                LibViewReleaseGraphics();
                std::vector<Album>().swap(v.albums);
                std::vector<TrackId>().swap(v.albumTracks);
                std::vector<Artist>().swap(v.artists);
                std::vector<Item>().swap(v.items);
                std::vector<int>().swap(v.albumItem);
                std::vector<char>().swap(v.selAlbum);
                std::vector<char>().swap(v.selArtist);
                decltype(v.selTrack)().swap(v.selTrack);
            } else if (v.albums.empty() && !g.view.empty() && CanvasActive()) {
                LibViewRebuild();
            }
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(h, &ps);
            Paint(ps.hdc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_VSCROLL: {
            SCROLLINFO si{sizeof(si), SIF_ALL};
            GetScrollInfo(h, SB_VERT, &si);
            int y = v.scroll;
            switch (LOWORD(wp)) {
                case SB_LINEUP: y -= (int)F(40); break;
                case SB_LINEDOWN: y += (int)F(40); break;
                case SB_PAGEUP: y -= v.height; break;
                case SB_PAGEDOWN: y += v.height; break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: y = si.nTrackPos; break;
                case SB_TOP: y = 0; break;
                case SB_BOTTOM: y = (int)v.contentHeight; break;
            }
            ScrollTo(y);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            if (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) {
                // Ctrl+wheel: bigger or smaller covers.
                // A view change for this session; keeping it for startup is the review's job.
                v.tile = std::clamp(v.tile + (delta > 0 ? 20 : -20), 80, 300);
                g.coverSize = v.tile;
                DropBitmaps();
                LayoutItems();
                InvalidateRect(h, nullptr, FALSE);
            } else {
                SmoothScrollTo((v.scrollAnimating ? v.scrollGoal : v.scroll) - MulDiv(delta, (int)F(120), WHEEL_DELTA));
            }
            return 0;
        }
        case WM_MOUSEMOVE:
            if (!v.tracking) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
                v.tracking = TrackMouseEvent(&tme);
            }
            UpdateHover(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        case WM_MOUSELEAVE:
            v.tracking = false;
            v.hoverItem = v.hoverRow = -1;
            v.hoverPlay = v.hoverClose = false;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) return TRUE;  // set in WM_MOUSEMOVE
            break;
        case WM_LBUTTONDOWN:
            OnClick(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), false, wp);
            return 0;
        case WM_LBUTTONDBLCLK:
            OnClick(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), true, wp);
            return 0;
        case WM_RBUTTONDOWN: {
            // Right-clicking outside the selection selects what's under the mouse first.
            Hit hit = HitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (hit.item >= 0) {
                const Item& it = v.items[hit.item];
                bool selected = (it.kind == ItemKind::Tile && v.selAlbum[it.index]) ||
                                (it.kind == ItemKind::ArtistTile && v.selArtist[it.index]) ||
                                (hit.row >= 0 && v.selTrack.count(Tracks(v.albums[it.index])[hit.row]));
                if (!selected && it.kind != ItemKind::Section) OnClick(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), false, 0);
            }
            return 0;
        }
        case WM_CONTEXTMENU:
            return SendMessageW(g.hwnd, msg, wp, lp);
        case WM_KEYDOWN:
            OnKey(wp);
            return 0;
        case WM_TIMER:
            if (wp == kScrollTimer) {
                StepSmoothScroll();
                return 0;
            }
            if (wp == kSurfaceTimer) {
                KillTimer(h, kSurfaceTimer);
                v.soft.ReleaseSurface();
                return 0;
            }
            break;
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS | DLGC_WANTCHARS;
        case WM_APP_THUMB:
            // The loader posts one message while results are waiting; drain every ready bitmap.
            while (auto result = v.loader.TakeResult()) OnThumb(result.release());
            return 0;
        case WM_DESTROY:
            StopSmoothScroll();
            LibViewReleaseGraphics();
            v.hwnd = nullptr;
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace app::lv
