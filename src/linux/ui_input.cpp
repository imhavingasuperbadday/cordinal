// The Linux window's input: hit testing, the mouse, the keyboard and the drags that reorder tracks.
#include "ui_internal.h"

namespace lx {

int MenuHit(float x, float y, int* popup) {
    for (int k = (int)g_popups.size() - 1; k >= 0; --k) {
        const Popup& p = g_popups[k];
        if (!Rect{p.x, p.y, p.w, p.h}.In(x, y)) continue;
        *popup = k;
        for (size_t i = 0; i < p.items.size(); ++i)
            if (!p.items[i].separator && PopupItemRect(p, i).In(x, y)) return (int)i;
        return -1;
    }
    *popup = -1;
    return -1;
}

// Hovering an item with a submenu opens it beside the item; hovering another closes it.
void HoverMenu(int popup, int item) {
    if (popup < 0) return;
    Popup& p = g_popups[popup];
    if (p.hover == item) return;
    p.hover = item;
    g_popups.resize(popup + 1);
    if (item >= 0 && !p.items[item].sub.empty() && p.items[item].enabled) {
        Rect r = PopupItemRect(p, item);
        OpenPopup(r.R() - S(4), r.y - S(kMenuPad), p.items[item].sub, true);
    }
    g.redraw = true;
}

int BarItemAt(float x, float y) {
    for (size_t i = 0; i < L.barItems.size(); ++i)
        if (L.barItems[i].In(x, y)) return (int)i;
    return -1;
}

float Density() { return SDL_GetWindowPixelDensity(g.window); }

double SeekValueAt(float x) {
    float inset = S(ui::kSliderInset);
    return std::clamp((double)(x - L.seek.x - inset) / std::max(1.0f, L.seek.w - 2 * inset), 0.0, 1.0) * TrackDuration();
}

std::string SectionTime(double seconds) {
    char text[32];
    int minutes = (int)(seconds / 60);
    snprintf(text, sizeof(text), "%d:%04.1f", minutes, seconds - minutes * 60.0);
    return text;
}

void OpenSectionMenu(float x, float y) {
    double duration = TrackDuration();
    if (g.playing == kNone || duration <= 0) return;
    g_sectionMenuTrack = g.playing;
    g_sectionMenuPos = SeekValueAt(x);
    PlaybackSection section = SectionOf(g.playing);
    bool marked = section.end - section.start > 0.05;
    std::string loop = marked ? "Loop " + SectionTime(section.start) + " to " + SectionTime(section.end) :
        "Loop this section (" + SectionTime(g_sectionMenuPos) + " to " + SectionTime(std::min(g_sectionMenuPos + 30, duration)) + ")";
    g_barOpen = -1;
    OpenPopup(x, y, {Item("Play from " + SectionTime(g_sectionMenuPos), kSectionPlay), Separator(),
                    Item(loop, kSectionLoop, {}, marked && section.on),
                    Item("Section starts here (" + SectionTime(g_sectionMenuPos) + ")", kSectionStart),
                    Item("Section ends here (" + SectionTime(g_sectionMenuPos) + ")", kSectionEnd),
                    Item("Clear section", kSectionClear, {}, false, marked)});
}

float VolumeAt(float x) {
    float inset = S(ui::kSliderInset);
    return std::clamp((x - L.volume.x - inset) / std::max(1.0f, L.volume.w - 2 * inset), 0.0f, 1.0f);
}

int ColumnEdgeAt(float x, float y) {
    if (!L.header.In(x, y)) return -1;
    float cx = 0;
    for (size_t i = 0; i < g.settings.columns.size(); ++i) {
        cx += ColumnWidth(g.settings.columns[i]);
        if (std::fabs(x - cx) <= S(4) && i > 0) return (int)i;
    }
    return -1;
}

int ColumnAt(float x) {
    float cx = 0;
    for (size_t i = 0; i < g.settings.columns.size(); ++i) {
        float w = ColumnWidth(g.settings.columns[i]);
        if (x >= cx && x < cx + w) return (int)i;
        cx += w;
    }
    return -1;
}

void SortBy(int c) {
    Playlist& pl = Active();
    if (c <= 0) return;
    if (pl.sortColumn == c) {
        if (!pl.sortAscending) pl.sortColumn = -1;  // a third click: back to the list's own order
        else pl.sortAscending = false;
    } else {
        pl.sortColumn = c;
        pl.sortAscending = true;
    }
    g.libraryDirty = true;
    RebuildRows();
}

void ClickGroup(int group, bool add) {
    if (group < 0 || group >= (int)g_groups.size()) return;
    const Group& gr = g_groups[group];
    if (!add) std::fill(g.selected.begin(), g.selected.end(), 0);
    for (int r = gr.first; r < gr.first + gr.count; ++r) g.selected[r] = 1;
    g.focus = g.anchor = gr.first;
}

void PrepareTrackDrag(float x, float y, int clicked, bool collapseOnClick) {
    g_trackDrag = {};
    if (!Active().Editable()) return;
    for (size_t i = 0; i < g.selected.size() && i < g.rowItems.size(); ++i)
        if (g.selected[i]) g_trackDrag.sources.push_back(g.rowItems[i]);
    if (g_trackDrag.sources.empty()) return;
    if (!Active().identity) Active().identity = g_nextPlaylistIdentity++;
    g_trackDrag.playlist = Active().identity;
    g_trackDrag.snapshot = Active().items;
    g_trackDrag.pending = true;
    g_trackDrag.collapseOnClick = collapseOnClick;
    g_trackDrag.clicked = clicked;
    g_trackDrag.x = x;
    g_trackDrag.y = y;
}

int TrackDropTarget(float y) {
    if (g.rows.empty() || y < L.rows.y) return 0;
    if (y >= L.rows.B()) return (int)g.rows.size();
    int line = LineAt(y);
    if (line < 0) return (int)g.rows.size();
    if (g_lines[line].row < 0) return g_groups[g_lines[line].group].first;
    float top = L.rows.y + line * L.rowH - g.scroll;
    return std::clamp(g_lines[line].row + (y >= top + L.rowH / 2 ? 1 : 0), 0, (int)g.rows.size());
}

void MoveDraggedTracks() {
    if (!Active().Editable() || g_trackDrag.sources.empty() || g.rowItems.empty() || g_trackDrag.target < 0) return;
    if (Active().identity != g_trackDrag.playlist || Active().items != g_trackDrag.snapshot) {
        ShowMessage(L"The playlist changed while dragging; no tracks were moved.");
        return;
    }
    Playlist& pl = Active();
    const size_t n = pl.items.size();
    std::vector<size_t> order(n);
    std::iota(order.begin(), order.end(), 0);
    if (pl.sortColumn > 0 && pl.sortColumn < ui::kColumnCount) {
        std::unordered_map<TrackId, std::wstring> keys;
        for (TrackId id : pl.items) {
            if (!g_lib.Contains(id)) return;
            keys.emplace(id, FormatTitle(kColumns[pl.sortColumn].sort, Lookup(id, false)));
        }
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            int r = CompareNatural(keys[pl.items[a]], keys[pl.items[b]]);
            return pl.sortAscending ? r < 0 : r > 0;
        });
    }
    std::vector<size_t> sortedPosition(n);
    for (size_t i = 0; i < n; ++i) sortedPosition[order[i]] = i;
    size_t target = g_trackDrag.target < (int)g.rowItems.size() ? sortedPosition[g.rowItems[g_trackDrag.target]]
                                                                : sortedPosition[g.rowItems.back()] + 1;
    std::vector<char> selected(n, 0);
    for (size_t source : g_trackDrag.sources) if (source < n) selected[source] = 1;
    std::vector<size_t> moved, kept;
    size_t insertion = 0;
    for (size_t i = 0; i < n; ++i) {
        if (selected[order[i]]) moved.push_back(order[i]);
        else { if (i < target) ++insertion; kept.push_back(order[i]); }
    }
    if (moved.empty()) return;
    kept.insert(kept.begin() + (ptrdiff_t)insertion, moved.begin(), moved.end());
    if (kept == order) return;
    std::vector<size_t> mapping(n);
    std::vector<char> movedNew(n, 0);
    std::vector<TrackId> items;
    items.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        mapping[kept[i]] = i;
        movedNew[i] = selected[kept[i]];
        items.push_back(pl.items[kept[i]]);
    }
    pl.items.swap(items);
    pl.sortColumn = -1;
    pl.sortAscending = true;
    if (pl.kind == PlaylistKind::NowPlaying) RemapNowPlaying(mapping);
    g.libraryDirty = true;
    g.rows.clear(); g.rowItems.clear(); g.selected.clear(); g.focus = g.anchor = -1;
    RebuildRows();
    for (size_t i = 0; i < g.rowItems.size(); ++i)
        if (movedNew[g.rowItems[i]]) {
            g.selected[i] = 1;
            if (g.focus < 0) g.focus = g.anchor = (int)i;
        }
    if (g.focus >= 0) EnsureVisible(g.focus);
}

void MouseDown(const SDL_MouseButtonEvent& e) {
    float x = e.x * Density(), y = e.y * Density();
    g_mouseX = x;
    g_mouseY = y;
    g.redraw = true;
    if (g_exclude.open) {
        if (e.button == SDL_BUTTON_LEFT && g_exclude.confirm.In(x, y)) ConfirmExclude(true);
        else if (e.button == SDL_BUTTON_LEFT && g_exclude.cancel.In(x, y)) ConfirmExclude(false);
        return;
    }
    if (g_renameIndex >= 0) {
        if (g_renameIndex < (int)L.tabRects.size() && L.tabRects[g_renameIndex].In(x, y)) return;
        EndRename(true);
    }
    if (PanelOpen()) {
        PanelMouseDown(x, y, e.button, e.clicks);
        return;
    }
    if (g_web.open) { if (e.button == SDL_BUTTON_LEFT) WebClick(x, y); return; }
    if (g_aboutOpen) {
        if (e.button == SDL_BUTTON_LEFT) AboutClick(x, y);
        else g_aboutOpen = false;
        return;
    }
    if (g_folders.open) {
        if (e.button == SDL_BUTTON_LEFT) FoldersClick(x, y);
        return;
    }
    if (!g_popups.empty()) {
        int popup = -1, hit = MenuHit(x, y, &popup);
        int bar = BarItemAt(x, y);
        if (popup >= 0) {
            if (hit >= 0 && g_popups[popup].items[hit].sub.empty() && g_popups[popup].items[hit].enabled) {
                MenuItem item = g_popups[popup].items[hit];
                CloseMenus();
                RunAction(item);
            }
            return;
        }
        bool same = bar >= 0 && bar == g_barOpen;
        CloseMenus();
        if (bar >= 0 && !same) OpenBarMenu(bar);
        return;
    }
    if (int bar = BarItemAt(x, y); bar >= 0) {
        OpenBarMenu(bar);
        return;
    }
    bool wasSearch = g.searchFocused;
    g.searchFocused = L.search.In(x, y);
    if (g.searchFocused && InternetRadio().active) InternetRadio().filterFocus = 0;
    if (g.searchFocused != wasSearch) {
        if (g.searchFocused) SDL_StartTextInput(g.window);
        else SDL_StopTextInput(g.window);
    }
    if (StartupReviewMouseDown(x, y, e.button)) return;
    if (e.button == SDL_BUTTON_LEFT && ToastMouseDown(x, y)) return;
    if (e.button == SDL_BUTTON_LEFT && L.lyrics.In(x, y)) {
        LyricsMouseDown(x, y);
        return;
    }
    if (!L.browser.In(x, y)) BrowserBlur();
    if (e.button == SDL_BUTTON_RIGHT) {
        if (InternetRadio().active && L.list.In(x, y)) return;
        if (L.seek.In(x, y) && g.playing != kNone) { OpenSectionMenu(x, y); return; }
        if (L.browser.In(x, y)) { BrowserMouseDown(L.browser, x, y, e.button, e.clicks, SDL_GetModState()); return; }
        if (CanvasActive() && L.list.In(x, y)) { CanvasMouseDown(L.list, x, y, e.button, e.clicks, SDL_GetModState()); return; }
        int line = LineAt(y);
        if (line >= 0 && g_lines[line].row >= 0) {
            int row = g_lines[line].row;
            if (!g.selected[row]) SelectOnly(row);
            g.focus = row;
            OpenRowMenu(x, y);
        } else if (L.header.In(x, y)) {
            OpenHeaderMenu(x, y);
        }
        return;
    }
    if (e.button == SDL_BUTTON_MIDDLE && L.radioTab.In(x, y)) { CloseRadio(); return; }
    if (e.button != SDL_BUTTON_LEFT) return;
    if (L.buttons[kBtnPrev].In(x, y)) Previous();
    else if (L.buttons[kBtnPlay].In(x, y)) TogglePause();
    else if (L.buttons[kBtnNext].In(x, y)) Next();
    else if (L.buttons[kBtnShuffle].In(x, y)) ToggleShuffle();
    else if (L.buttons[kBtnRepeat].In(x, y)) CycleRepeat();
    else if (L.buttons[kBtnResume].In(x, y)) SetResumeOn(!g.settings.resumeOn);
    else if (L.chip.w > 0 && L.chip.In(x, y)) OpenPoints();
    else if (L.cover.In(x, y) || L.title.In(x, y) || L.subtitle.In(x, y)) {
        if (!Resume()) RevealPlaying();
    }
    else if (Rect{L.speaker.x, L.volume.y, L.speaker.w, L.volume.h}.In(x, y)) ToggleMute();
    else if (L.volume.In(x, y)) {
        g_drag = Drag::Volume;
        g_muteVolume = 0;
        SetVolume(VolumeAt(x));
    } else if (L.seek.In(x, y) && g.playing != kNone) {
        g_drag = Drag::Seek;
        g_seekPreview = SeekValueAt(x);
    } else if (L.radioTab.In(x, y)) {
        Rect close{L.radioTab.R() - S(22), L.radioTab.y + S(3), S(19), L.radioTab.h - S(6)};
        if (close.In(x, y)) CloseRadio(); else OpenRadio();
    } else if (InternetRadio().active && L.list.In(x, y)) {
        auto& radio = InternetRadio();
        if (L.radioSaved.In(x, y)) { radio.filterFocus = 0; RadioSetMode(false); }
        else if (L.radioDiscover.In(x, y)) { radio.filterFocus = 0; RadioSetMode(true); }
        else if (L.radioGenre.In(x, y) || L.radioCountry.In(x, y) || L.radioState.In(x, y)) {
            radio.filterFocus = L.radioGenre.In(x, y) ? 1 : L.radioCountry.In(x, y) ? 2 : 3;
            radio.editSelectAll = false; SDL_StartTextInput(g.window);
        } else if (L.radioSearch.In(x, y)) {
            radio.filterFocus = 0;
            if (!radio.discover) RadioSetMode(true); else RadioSearch();
        } else {
            radio.filterFocus = 0;
            for (int i = 0; i < (int)radio.rows.size(); ++i) {
                Rect r = RadioCard(i);
                if (!L.radioGrid.In(x, y) || !r.In(x, y)) continue;
                radio.selected = i;
                if (x >= r.R() - S(32) && y < r.y + S(32)) { if (e.clicks < 2) RadioToggleFavorite(i); }
                else if (e.clicks >= 2) RadioPlay(i);
                break;
            }
        }
    } else if (L.viewRow.h > 0 && L.viewRow.In(x, y)) {
        for (int i = 0; i < kViewButtons; ++i)
            if (L.views[i].In(x, y)) {
                if (i < (int)LibraryMode::Count) SetLibraryMode((LibraryMode)i);
                else ToggleBrowser();
            }
    } else if (L.splitter.h > 0 && L.splitter.In(x, y)) {
        g_drag = Drag::Splitter;
        g_dragStart = y;
        g_dragStartValue = L.browser.h;
    } else if (L.browser.In(x, y)) {
        BrowserMouseDown(L.browser, x, y, e.button, e.clicks, SDL_GetModState());
    } else if (CanvasActive() && L.list.In(x, y)) {
        CanvasMouseDown(L.list, x, y, e.button, e.clicks, SDL_GetModState());
    } else if (L.scrollbar.w > 0 && L.scrollbar.In(x, y)) {
        g_drag = Drag::Scroll;
        g_dragStart = y;
        g_dragStartValue = g.scroll;
    } else if (int edge = ColumnEdgeAt(x, y); edge >= 0) {
        g_drag = Drag::ColumnEdge;
        g_dragColumn = g.settings.columns[edge];
        g_dragStart = x;
        g_dragStartValue = ColumnWidth(g_dragColumn) / g.scale;
    } else if (L.header.In(x, y)) {
        int col = ColumnAt(x);
        if (col >= 0) SortBy(g.settings.columns[col]);
    } else if (L.tabs.In(x, y)) {
        for (size_t i = 0; i < L.tabRects.size(); ++i)
            if (L.tabRects[i].In(x, y)) {
                Rect close{L.tabRects[i].R() - S(22), L.tabRects[i].y + S(3), S(19), L.tabRects[i].h - S(6)};
                if (close.In(x, y) && (g.playlists[i].kind == PlaylistKind::Static || g.playlists[i].kind == PlaylistKind::Auto))
                    ClosePlaylist((int)i);
                else SetActiveTab((int)i);
                break;
            }
    } else if (L.rows.In(x, y)) {
        int line = LineAt(y);
        SDL_Keymod mod = SDL_GetModState();
        if (line < 0) {
            SelectOnly(-1);
            return;
        }
        if (g_lines[line].row < 0) {
            ClickGroup(g_lines[line].group, mod & SDL_KMOD_CTRL);  // a header selects its album
            PrepareTrackDrag(x, y, -1, false);
            return;
        }
        int row = g_lines[line].row;
        int column = ColumnAt(x);
        if (column >= 0 && g.settings.columns[column] == ui::kColRating && !(mod & (SDL_KMOD_CTRL | SDL_KMOD_SHIFT))) {
            float left = 0;
            for (int i = 0; i < column; ++i) left += ColumnWidth(g.settings.columns[i]);
            float width = std::max(TextWidth(Font::Normal, "\xE2\x98\x85"), 1.0f);
            double stars = (x - left - S(4)) / width;
            if (stars >= -0.2 && stars <= 5.4) {
                TrackId id = g.rows[row];
                uint8_t rating = (uint8_t)(std::clamp((int)std::ceil(stars * 2), 1, 10) * 10);
                if (g_lib[id].rating == rating) rating = 0;
                SelectOnly(row);
                SetRating({id}, rating);
                g_lastClick = 0;
                return;
            }
        }
        bool selected = g.selected[row];
        uint64_t now = SDL_GetTicks();
        if (now - g_lastClick < 400 && g_lastClickLine == line && !(mod & (SDL_KMOD_CTRL | SDL_KMOD_SHIFT))) {
            g_trackDrag = {};
            PlayRow(row);
            g_lastClick = 0;
            return;
        }
        g_lastClick = now;
        g_lastClickLine = line;
        if (mod & SDL_KMOD_SHIFT) {
            SelectRange(g.anchor >= 0 ? g.anchor : row, row);
            g.focus = row;
        } else if (mod & SDL_KMOD_CTRL) {
            g.selected[row] = !g.selected[row];
            g.focus = g.anchor = row;
        } else if (!selected || !Active().Editable()) {
            SelectOnly(row);
        }
        if (g.selected[row]) PrepareTrackDrag(x, y, row, selected && !(mod & (SDL_KMOD_CTRL | SDL_KMOD_SHIFT)));
    }
}

void MouseMove(const SDL_MouseMotionEvent& e) {
    float x = e.x * Density(), y = e.y * Density();
    g_mouseX = x;
    g_mouseY = y;
    g.redraw = true;
    if (g_trackDrag.pending && (e.state & SDL_BUTTON_LMASK) &&
        (std::fabs(x - g_trackDrag.x) >= S(4) || std::fabs(y - g_trackDrag.y) >= S(4))) {
        g_trackDrag.pending = false;
        g_drag = Drag::Tracks;
        g_lastClick = 0;
        g_lastClickLine = -1;
        SDL_CaptureMouse(true);
    }
    if (g_drag == Drag::Tracks) {
        g_trackDrag.target = TrackDropTarget(y);
        return;
    }
    if (PanelOpen()) {
        PanelMouseMove(x, y);
        static SDL_Cursor* arrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
        static SDL_Cursor* ibeam = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_TEXT);
        SDL_SetCursor(PanelWantsText(x, y) ? ibeam : arrow);
        return;
    }
    if (!g_popups.empty()) {
        int popup = -1, hit = MenuHit(x, y, &popup);
        if (popup >= 0) HoverMenu(popup, hit);
        else if (int bar = BarItemAt(x, y); bar >= 0 && g_barOpen >= 0 && bar != g_barOpen) OpenBarMenu(bar);
        return;
    }
    switch (g_drag) {
        case Drag::Volume: SetVolume(VolumeAt(x)); break;
        case Drag::Seek: g_seekPreview = SeekValueAt(x); break;
        case Drag::Scroll: {
            float thumbH = std::max(S(24), L.scrollbar.h * L.rows.h / std::max(1.0f, ContentHeight()));
            float room = std::max(1.0f, L.scrollbar.h - S(34) - thumbH);
            g.scroll = g_dragStartValue + (y - g_dragStart) / room * MaxScroll();
            ClampScroll();
            break;
        }
        case Drag::ColumnEdge:
            if (g_dragColumn >= 0) g.settings.widths[g_dragColumn] = std::max(24, (int)std::lround(g_dragStartValue + (x - g_dragStart) / g.scale));
            break;
        case Drag::Tracks: break;
        case Drag::Splitter: SetBrowserHeight(g_dragStartValue + y - g_dragStart); break;
        case Drag::None: {
            if (L.splitter.h > 0 && L.splitter.In(x, y)) {
                static SDL_Cursor* rows = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NS_RESIZE);
                SDL_SetCursor(rows);
                break;
            }
            if (CanvasActive() && L.list.In(x, y)) {
                static SDL_Cursor* hand = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);
                static SDL_Cursor* plain = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
                SDL_SetCursor(CanvasWantsHand(L.list, x, y) ? hand : plain);
                break;
            }
            bool edge = ColumnEdgeAt(x, y) >= 0;
            static SDL_Cursor* resize = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_EW_RESIZE);
            static SDL_Cursor* arrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
            static SDL_Cursor* ibeam = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_TEXT);
            SDL_SetCursor(edge ? resize : L.search.In(x, y) ? ibeam : arrow);
            break;
        }
    }
}

void MouseUp(const SDL_MouseButtonEvent& e) {
    if (PanelOpen()) {
        PanelMouseUp();
        return;
    }
    if (e.button == SDL_BUTTON_LEFT && g_drag == Drag::Tracks) {
        g_trackDrag.target = TrackDropTarget(e.y * Density());
        MoveDraggedTracks();
        SDL_CaptureMouse(false);
        g_trackDrag = {};
    } else if (e.button == SDL_BUTTON_LEFT && g_trackDrag.pending) {
        if (g_trackDrag.collapseOnClick && g_trackDrag.clicked >= 0) SelectOnly(g_trackDrag.clicked);
        g_trackDrag = {};
    }
    if (g_drag == Drag::Seek && g_seekPreview >= 0) SeekTo(g_seekPreview);
    if (g_drag == Drag::Volume || g_drag == Drag::ColumnEdge || g_drag == Drag::Splitter) SaveSettings();
    g_seekPreview = -1;
    g_drag = Drag::None;
    g.redraw = true;
}

void Wheel(const SDL_MouseWheelEvent& e) {
    if (g_exclude.open) return;
    if (PanelOpen()) {
        PanelWheel(e.mouse_x * Density(), e.mouse_y * Density(), e.y);
        return;
    }
    if (g_web.open) { g_web.scroll = std::max(0, g_web.scroll - (int)e.y); g.redraw = true; return; }
    float x = e.mouse_x * Density(), y = e.mouse_y * Density();
    if (Rect{L.speaker.x, L.volume.y, L.volume.R() - L.speaker.x, L.volume.h}.In(x, y)) {
        SetVolume(g.settings.volume + e.y * 0.05f);
        SaveSettings();
        return;
    }
    if (!g_popups.empty() || g_folders.open) return;
    if (InternetRadio().active && L.radioGrid.In(x, y)) {
        InternetRadio().scroll = std::clamp(InternetRadio().scroll - e.y * S(82), 0.0f, RadioMaxScroll());
        g.redraw = true; return;
    }
    if (L.lyrics.w > 0 && L.lyrics.In(x, y)) {
        LyricsWheel(e.y);
        return;
    }
    if (L.browser.In(x, y)) {
        BrowserWheel(L.browser, x, y, e.y);
        return;
    }
    if (CanvasActive() && L.list.In(x, y)) {
        CanvasWheel(L.list, e.y, SDL_GetModState() & SDL_KMOD_CTRL);
        return;
    }
    g.scroll -= e.y * L.rowH * 3;
    ClampScroll();
    g.redraw = true;
}

// Keys while a menu is open: Up and Down move through the items, Right opens a submenu (or the next
// menu on the bar), Left closes one (or the previous menu), Enter chooses, Escape closes.
void MenuKey(SDL_Keycode key) {
    Popup& p = g_popups.back();
    int n = (int)p.items.size();
    auto step = [&](int dir) {
        int i = p.hover >= 0 ? p.hover : dir > 0 ? -1 : n;  // with nothing chosen, Up starts from the last item
        for (int k = 0; k < n; ++k) {
            i = (i + dir + n) % n;
            if (!p.items[i].separator && p.items[i].enabled) break;
        }
        p.hover = i;
    };
    if (key == SDLK_ESCAPE) {
        if (g_popups.size() > 1) g_popups.pop_back();
        else CloseMenus();
    } else if (key == SDLK_DOWN) step(1);
    else if (key == SDLK_UP) step(-1);
    else if (key == SDLK_RIGHT && p.hover >= 0 && !p.items[p.hover].sub.empty()) {
        int popup = (int)g_popups.size() - 1, item = p.hover;
        g_popups[popup].hover = -1;
        HoverMenu(popup, item);
        g_popups.back().hover = 0;
    } else if ((key == SDLK_RIGHT || key == SDLK_LEFT) && g_barOpen >= 0 && (g_popups.size() == 1 || key == SDLK_RIGHT)) {
        int count = (int)L.barItems.size();
        OpenBarMenu((g_barOpen + (key == SDLK_RIGHT ? 1 : count - 1)) % count);
    } else if (key == SDLK_LEFT && g_popups.size() > 1) g_popups.pop_back();
    else if ((key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE) && p.hover >= 0) {
        const MenuItem& it = p.items[p.hover];
        if (!it.sub.empty()) MenuKey(SDLK_RIGHT);
        else if (it.enabled) {
            MenuItem item = it;
            CloseMenus();
            RunAction(item);
        }
    }
    g.redraw = true;
}

void Key(const SDL_KeyboardEvent& e) {
    g.redraw = true;
    SDL_Keymod mod = e.mod;
    bool ctrl = mod & SDL_KMOD_CTRL, shift = mod & SDL_KMOD_SHIFT;
    if (g_exclude.open) {
        if (e.key == SDLK_ESCAPE) ConfirmExclude(false);
        else if (e.key == SDLK_RETURN || e.key == SDLK_KP_ENTER) ConfirmExclude(true);
        return;
    }
    if (e.key == SDLK_ESCAPE && g_drag == Drag::Tracks) {
        SDL_CaptureMouse(false);
        g_drag = Drag::None;
        g_trackDrag = {};
        return;
    }
    if (g_renameIndex >= 0) {
        if (e.key == SDLK_RETURN || e.key == SDLK_KP_ENTER) EndRename(true);
        else if (e.key == SDLK_ESCAPE) EndRename(false);
        else if (ctrl && e.key == SDLK_A) g_renameSelected = true;
        else if (e.key == SDLK_BACKSPACE || e.key == SDLK_DELETE) {
            if (g_renameSelected) { g_renameText.clear(); g_renameCaret = 0; }
            else if (e.key == SDLK_BACKSPACE && g_renameCaret) g_renameText.erase(--g_renameCaret, 1);
            else if (e.key == SDLK_DELETE && g_renameCaret < g_renameText.size()) g_renameText.erase(g_renameCaret, 1);
            g_renameSelected = false;
        } else if (e.key == SDLK_LEFT || e.key == SDLK_HOME) {
            g_renameCaret = e.key == SDLK_HOME || g_renameSelected ? 0 : g_renameCaret ? g_renameCaret - 1 : 0;
            g_renameSelected = false;
        } else if (e.key == SDLK_RIGHT || e.key == SDLK_END) {
            g_renameCaret = e.key == SDLK_END || g_renameSelected ? g_renameText.size() : std::min(g_renameCaret + 1, g_renameText.size());
            g_renameSelected = false;
        }
        return;
    }
    g_altAlone = (e.key == SDLK_LALT || e.key == SDLK_RALT) && !e.repeat;
    if (PanelOpen()) {
        PanelKey(e);
        return;
    }
    if (g_web.open) {
        if (e.key == SDLK_ESCAPE) g_web.open = false;
        else if (e.key == SDLK_TAB) { g_web.focus = (g_web.focus + (shift ? kWebControls - 1 : 1)) % kWebControls; SDL_StartTextInput(g.window); }
        else if (e.key == SDLK_RETURN || (e.key == SDLK_SPACE && g_web.focus != 12)) WebAction(g_web.focus);
        else if (e.key == SDLK_UP) g_web.scroll = std::max(0, g_web.scroll - 1);
        else if (e.key == SDLK_DOWN) ++g_web.scroll;
        else if (e.key == SDLK_BACKSPACE && g_web.focus == 0 && !WebRunning() && !g_web.port.empty()) g_web.port.pop_back();
        else if (e.key == SDLK_BACKSPACE && g_web.focus == 12 && !g_web.fixed.empty()) g_web.fixed.pop_back();
        return;
    }
    if (g_aboutOpen) {
        g_aboutOpen = false;
        return;
    }
    if (g_folders.open) {
        if (e.key == SDLK_ESCAPE || e.key == SDLK_RETURN) g_folders.open = false;
        else if (!(mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI))) {
            if (e.key == SDLK_F) ChooseFolder();
            else if (e.key == SDLK_M) AddFolder(compat::Widen(MusicFolder().c_str()));
        }
        return;
    }
    if (!g_popups.empty()) {
        MenuKey(e.key);
        return;
    }
    if (PackKeyDown(e)) return;  // a pack's binding takes the key from cordinal's own
    if (e.key == SDLK_F10) {
        OpenBarMenu(0);
        return;
    }
    switch (e.scancode) {  // media keys, when the window has the focus (MPRIS covers the rest)
        case SDL_SCANCODE_MEDIA_PLAY_PAUSE: TogglePause(); return;
        case SDL_SCANCODE_MEDIA_NEXT_TRACK: Next(); return;
        case SDL_SCANCODE_MEDIA_PREVIOUS_TRACK: Previous(); return;
        case SDL_SCANCODE_MEDIA_STOP:
            HoldPendingOpens();
            Stop();
            return;
        default: break;
    }
    if (ctrl) {  // the menus' shortcuts
        switch (e.key) {
            case SDLK_F:
                if (InternetRadio().active) InternetRadio().filterFocus = 0;
                g.searchFocused = true;
                SDL_StartTextInput(g.window);
                return;
            case SDLK_Q: g.quit = true; return;
            case SDLK_O:
                if (shift) ChooseImportFolder();
                else ChooseFiles();
                return;
            case SDLK_S: ChoosePlaylistFile(true); return;
            case SDLK_M:
                if (shift) {
                    ToggleMini();
                    return;
                }
                ToggleMute();
                return;
            case SDLK_H: ToggleShuffle(); return;
            case SDLK_R:
                if (shift) OpenRadio(); else SetRepeat(Repeat::All);
                return;
            case SDLK_TAB:
                if (InternetRadio().active && !g.playlists.empty())
                    SetActiveTab((g.active + (shift ? -1 : 1) + (int)g.playlists.size()) % (int)g.playlists.size());
                else break;
                return;
            case SDLK_J: RevealPlaying(); return;
            case SDLK_N: NewPlaylist(); return;
            case SDLK_W: if (InternetRadio().active) CloseRadio(); else ClosePlaylist(g.active); return;
            case SDLK_D: if (shift) { DuplicatePlaylist(); return; } break;
            case SDLK_T: if (shift) { ReopenPlaylist(); return; } break;
            case SDLK_PAGEUP: if (shift) { ReorderPlaylist(-1); return; } break;
            case SDLK_PAGEDOWN: if (shift) { ReorderPlaylist(1); return; } break;
            case SDLK_L: ToggleLyrics(); return;
            case SDLK_P:
                if (shift) { OpenPalette(); return; }
                break;
            case SDLK_E: OpenEqualizer(); return;
            case SDLK_COMMA: OpenPreferences(); return;
            case SDLK_Z:
                if (!g.searchFocused) {
                    UndoLastTagEdit();
                    return;
                }
                break;
            default: break;
        }
    }
    if ((mod & SDL_KMOD_ALT) && (e.key == SDLK_RETURN || e.key == SDLK_KP_ENTER) && !g.searchFocused) {
        OpenTagEditor(SelectedTracks());
        return;
    }
    if (InternetRadio().active && InternetRadio().filterFocus && !g.searchFocused) {
        auto& radio = InternetRadio();
        std::wstring* field = radio.filterFocus == 1 ? &radio.genre : radio.filterFocus == 2 ? &radio.country : &radio.region;
        if (e.key == SDLK_ESCAPE) { radio.filterFocus = 0; SDL_StopTextInput(g.window); }
        else if (e.key == SDLK_BACKSPACE && field) {
            if (ctrl || radio.editSelectAll) field->clear(); else if (!field->empty()) field->pop_back();
            radio.editSelectAll = false;
        } else if (ctrl && e.key == SDLK_A) radio.editSelectAll = true;
        else if (ctrl && e.key == SDLK_V && field) {
            char* pasted = SDL_GetClipboardText();
            if (pasted) {
                std::wstring text = FromUtf8(pasted); SDL_free(pasted);
                if (radio.editSelectAll) field->clear();
                size_t limit = 512;
                field->append(text.substr(0, limit > field->size() ? limit - field->size() : 0)); radio.editSelectAll = false;
            }
        } else if (e.key == SDLK_RETURN || e.key == SDLK_KP_ENTER) RadioSearch();
        else if (e.key == SDLK_TAB) {
            radio.filterFocus = (radio.filterFocus + (shift ? 3 : 1)) % 4;
            radio.editSelectAll = false;
        }
        g.redraw = true; return;
    }
    if (g.searchFocused && InternetRadio().active) {
        if (e.key == SDLK_ESCAPE) {
            if (!g.search.empty()) { g.search.clear(); InternetRadio().query.clear(); if (!InternetRadio().discover) RadioRebuild(); }
            else { g.searchFocused = false; SDL_StopTextInput(g.window); }
        } else if (e.key == SDLK_BACKSPACE && !g.search.empty()) {
            if (ctrl) g.search.clear(); else g.search.pop_back();
            InternetRadio().query = g.search; if (!InternetRadio().discover) RadioRebuild();
        } else if (e.key == SDLK_RETURN || e.key == SDLK_KP_ENTER) RadioSearch();
        else if (e.key == SDLK_DOWN) { g.searchFocused = false; SDL_StopTextInput(g.window); }
        g.redraw = true; return;
    }
    if (g.searchFocused) {
        if (e.key == SDLK_ESCAPE) {
            if (!g.search.empty()) {
                g.search.clear();
                RebuildRows();
            } else {
                g.searchFocused = false;
                SDL_StopTextInput(g.window);
            }
        } else if (e.key == SDLK_BACKSPACE && !g.search.empty()) {
            if (ctrl) g.search.clear();
            else g.search.pop_back();
            RebuildRows();
        } else if (e.key == SDLK_RETURN || e.key == SDLK_DOWN) {
            g.searchFocused = false;
            SDL_StopTextInput(g.window);
            if (!g.rows.empty()) MoveFocus(0, false);
            if (e.key == SDLK_RETURN && !g.rows.empty()) PlayRow(0);
        }
        return;
    }
    if (InternetRadio().active) {
        auto& radio = InternetRadio();
        if (e.key == SDLK_SPACE) TogglePause();
        else if (e.key == SDLK_RETURN || e.key == SDLK_KP_ENTER) RadioPlay(radio.selected);
        else if (e.key == SDLK_F) RadioToggleFavorite(radio.selected);
        else if (e.key == SDLK_TAB) { radio.filterFocus = 1; SDL_StartTextInput(g.window); }
        else if (e.key == SDLK_LEFT || e.key == SDLK_RIGHT || e.key == SDLK_UP || e.key == SDLK_DOWN) {
            int columns = RadioColumns();
            int delta = e.key == SDLK_LEFT ? -1 : e.key == SDLK_RIGHT ? 1 : e.key == SDLK_UP ? -columns : columns;
            radio.selected = std::clamp(radio.selected + delta, 0, std::max(0, (int)radio.rows.size() - 1));
            Rect r = RadioCard(radio.selected);
            if (r.y < L.radioGrid.y) radio.scroll -= L.radioGrid.y - r.y;
            if (r.B() > L.radioGrid.B()) radio.scroll += r.B() - L.radioGrid.B();
            radio.scroll = std::clamp(radio.scroll, 0.0f, RadioMaxScroll());
        }
        g.redraw = true; return;
    }
    if (BrowserKey(e)) return;
    if (CanvasActive() && CanvasKey(e)) return;
    int page =std::max(1, (int)(L.rows.h / L.rowH) - 1);
    switch (e.key) {
        case SDLK_F2: BeginRename(g.active); break;
        case SDLK_SPACE: TogglePause(); break;
        case SDLK_RETURN: case SDLK_KP_ENTER:
            if (g.focus >= 0) {
                if (ctrl) QueueTracks(SelectedTracks(), true);
                else PlayRow(g.focus);
            }
            break;
        case SDLK_UP: MoveFocus(g.focus - 1, shift); break;
        case SDLK_DOWN: MoveFocus(g.focus + 1, shift); break;
        case SDLK_PAGEUP: MoveFocus(g.focus - page, shift); break;
        case SDLK_PAGEDOWN: MoveFocus(g.focus + page, shift); break;
        case SDLK_HOME: MoveFocus(0, shift); break;
        case SDLK_END: MoveFocus((int)g.rows.size() - 1, shift); break;
        case SDLK_A:
            if (ctrl) std::fill(g.selected.begin(), g.selected.end(), 1);
            break;
        case SDLK_Q:
            if (!ctrl) {
                if (shift) InsertIntoNowPlaying(SelectedTracks(), false);
                else QueueTracks(SelectedTracks(), false);
            }
            break;
        case SDLK_DELETE:
            if (Active().kind == PlaylistKind::Library || Active().kind == PlaylistKind::Inbox) ExcludeFromLibrary(SelectedTracks());
            else RemoveFromPlaylist(false);
            break;
        case SDLK_RIGHT:
            if (ctrl) Next();
            else if (g.playing != kNone) SeekTo(g.engine.Position() + 5);
            break;
        case SDLK_LEFT:
            if (ctrl) Previous();
            else if (g.playing != kNone) SeekTo(g.engine.Position() - 5);
            break;
        case SDLK_TAB:
            if (ctrl) SetActiveTab((g.active + (shift ? -1 : 1) + (int)g.playlists.size()) % (int)g.playlists.size());
            break;
        default: break;
    }
}

void KeyUp(const SDL_KeyboardEvent& e) {
    // Alt pressed and released alone opens the menu bar, as on Windows.
    if ((e.key == SDLK_LALT || e.key == SDLK_RALT) && g_altAlone && g_popups.empty() && !g_web.open && !g_folders.open && !PanelOpen())
        OpenBarMenu(0);
    g_altAlone = false;
}

void TextInput(const SDL_TextInputEvent& e) {
    if (g_renameIndex >= 0 && e.text) {
        std::wstring text = compat::Widen(e.text);
        if (g_renameSelected) { g_renameText.clear(); g_renameCaret = 0; }
        g_renameSelected = false;
        if (g_renameText.size() + text.size() > 128) text.resize(128 - g_renameText.size());
        g_renameText.insert(g_renameCaret, text);
        g_renameCaret += text.size();
        g.redraw = true;
        return;
    }
    if (PanelOpen()) {
        PanelText(e.text);
        return;
    }
    if (g_web.open) {
        if (g_web.focus == 0 && !WebRunning() && e.text)
            for (const char* p = e.text; *p; ++p) if (*p >= '0' && *p <= '9' && g_web.port.size() < 5) g_web.port += *p;
        if (g_web.focus == 12 && e.text)
            for (const char* p = e.text; *p; ++p)
                if (((*p >= '0' && *p <= '9') || *p == ' ' || *p == '-') && g_web.fixed.size() < 24) g_web.fixed += *p;
        g.redraw = true; return;
    }
    if (InternetRadio().active && InternetRadio().filterFocus && !g.searchFocused && e.text) {
        auto& radio = InternetRadio();
        std::wstring* field = radio.filterFocus == 1 ? &radio.genre : radio.filterFocus == 2 ? &radio.country : &radio.region;
        if (radio.editSelectAll) field->clear();
        size_t limit = 512;
        std::wstring text = FromUtf8(e.text);
        field->append(text.substr(0, limit > field->size() ? limit - field->size() : 0)); radio.editSelectAll = false;
        g.redraw = true; return;
    }
    if (!g.searchFocused && BrowserFocused()) {
        BrowserText(e.text);
        return;
    }
    if (!g.searchFocused || !e.text) return;
    g.search += compat::Widen(e.text);
    if (InternetRadio().active) { InternetRadio().query = g.search; if (!InternetRadio().discover) RadioRebuild(); }
    else RebuildRows();
    g.redraw = true;
}

}  // namespace lx
