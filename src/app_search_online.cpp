// Windows online search results pane (app_search_online.h): debounced search on a worker,
// instant preview playback or automatic saving, right-click actions, and inline Save.
#include "app.h"

#include "theme_win.h"

#include <cstdio>
#include <memory>
#include <thread>

namespace app {
namespace {

const wchar_t kOnlineSearchClass[] = L"CordinalOnlineSearch";
constexpr size_t kMaxShownResults = 15;

enum class ResolveAction : uint8_t { PlayNow, PlayNext, AddToQueue, SaveToLibrary, SaveAlbum };

struct OnlineRow {
    online::SearchTrack track;
    TrackId previewId = kNone;
    TrackId savedId = kNone;
    bool saved = false;
};

struct SearchWorkerResult {
    uint64_t generation = 0;
    std::wstring query;
    online::SearchReply reply;
};

struct ResolveWorkerResult {
    uint64_t generation = 0;
    ResolveAction action = ResolveAction::PlayNow;
    bool permanent = false;
    bool ok = false;
    WebFetchTrackList albumList;
    std::wstring file, dir;
    std::string error;
};

struct OnlineSearchState {
    std::wstring query;
    std::vector<OnlineRow> rows;
    std::wstring status;
    bool searching = false;
    bool resolving = false;
    bool shuttingDown = false;
    int selected = -1;
    int hoverRow = -1;
    bool hoverSave = false;
    bool hoverPrompt = false;
    int scroll = 0;

    // Library snapshot cached per library revision only while a search is active.
    uint32_t ownedRevision = UINT32_MAX;
    import::OwnedIndex owned;

    online::Generation searchGen;
    std::shared_ptr<online::Cancel> searchCancel;
    std::atomic<bool> searchWorkerBusy{false};
    std::thread searchWorker;

    online::Generation resolveGen;
    std::shared_ptr<online::Cancel> resolveCancel;
    std::atomic<bool> resolveWorkerBusy{false};
    bool hasPendingResolve = false;
    online::SearchTrack pendingTrack;
    ResolveAction pendingAction = ResolveAction::PlayNow;
    std::thread resolveWorker;
    std::atomic<unsigned> previewSeq{0};
};

HWND s_hwnd = nullptr;
bool s_visibleWanted = false;
int s_lastHeight = 0;
OnlineSearchState* sp = nullptr;

__declspec(noinline) OnlineSearchState& State() {
    if (!sp) sp = new OnlineSearchState();
    return *sp;
}

int HeaderHeight() { return S(26); }
int RowHeight() { return S(24); }
int SaveColWidth() { return S(56); }
int DurationColWidth() { return S(50); }

void RemoveTree(const std::wstring& dir) { DiscoverRemoveTree(dir); }

void CancelSearchWorker() {
    KillTimer(g.hwnd, kOnlineSearchTimer);
    if (!sp) return;
    sp->searchGen.Next();
    if (sp->searchCancel) sp->searchCancel->Stop();
    sp->searching = false;
}

void CancelResolveWorker() {
    if (!sp) return;
    sp->resolveGen.Next();
    if (sp->resolveCancel) sp->resolveCancel->Stop();
    sp->resolving = false;
}

void UpdateLayoutIfNeeded() {
    if (!g.hwnd || !s_hwnd) return;
    RECT rc{};
    GetClientRect(g.hwnd, &rc);
    const bool vis = OnlineSearchVisible();
    const int wanted = OnlineSearchWantedHeight(std::max<int>(1, rc.bottom - rc.top));
    if (vis != s_visibleWanted || wanted != s_lastHeight) {
        s_visibleWanted = vis;
        s_lastHeight = wanted;
        Layout();
    } else {
        InvalidateRect(s_hwnd, nullptr, FALSE);
    }
}

void RefreshOwnedSnapshot() {
    OnlineSearchState& s = State();
    if (s.ownedRevision == g_lib.Revision()) return;
    s.owned = import::OwnedIndex();
    const size_t count = g_lib.Count();
    for (size_t i = 0; i < count; ++i) {
        const Track& t = g_lib[(TrackId)i];
        if ((t.flags & (Track::kInLibrary | Track::kInbox)) == 0) continue;
        if (t.flags & (Track::kMissing | Track::kExcluded | Track::kOriginal)) continue;
        if (!t.artist || !t.title) continue;
        s.owned.Add(g_lib.View(t.artist), g_lib.View(t.title), g_lib.Path((TrackId)i));
    }
    s.ownedRevision = g_lib.Revision();
}

void ClampScroll() {
    if (!s_hwnd || !sp) return;
    RECT rc{};
    GetClientRect(s_hwnd, &rc);
    const int bodyH = std::max(0, (int)rc.bottom - HeaderHeight());
    const int totalH = (int)sp->rows.size() * RowHeight();
    sp->scroll = std::clamp(sp->scroll, 0, std::max(0, totalH - bodyH));
}

void EnsureVisible(int index) {
    if (!s_hwnd || !sp || index < 0 || index >= (int)sp->rows.size()) return;
    RECT rc{};
    GetClientRect(s_hwnd, &rc);
    const int bodyH = std::max(1, (int)rc.bottom - HeaderHeight());
    const int top = index * RowHeight();
    const int bottom = top + RowHeight();
    if (top < sp->scroll) sp->scroll = top;
    else if (bottom > sp->scroll + bodyH) sp->scroll = bottom - bodyH;
    ClampScroll();
}

bool EnsureOnlineSearchEnabled() {
    if (online::Config().searchMusic) return true;
    const wchar_t* text =
        L"Online search is off.\n\n"
        L"When on, your search words are sent to Deezer (or iTunes / YouTube) to show songs not in your "
        L"library.\n\nTurn online search on? (Preferences > Online)";
    if (MessageBoxW(g.hwnd, text, L"Search online", MB_YESNO | MB_ICONQUESTION) != IDYES) return false;
    if (const Tweak* t = FindTweak("online.search_music")) SetTweak(*t, 1);
    else {
        online::Config().searchMusic = true;
        SavePreferencesSoon();
    }
    OnlineSearchSettingChanged();
    return online::Config().searchMusic;
}

RECT PromptButtonRect(const RECT& rc) {
    const int bw = S(124), bh = S(24);
    return {rc.right - S(12) - bw, (rc.bottom - bh) / 2, rc.right - S(12), (rc.bottom + bh) / 2};
}

RECT RowRect(int index, const RECT& rc) {
    const int y = HeaderHeight() + index * RowHeight() - (sp ? sp->scroll : 0);
    return {0, y, rc.right, y + RowHeight()};
}

RECT SaveRect(const RECT& rowRc) {
    return {rowRc.right - SaveColWidth() - S(8), rowRc.top + S(3), rowRc.right - S(8), rowRc.bottom - S(3)};
}

int HitRow(POINT pt, const RECT& rc, bool& onSave) {
    onSave = false;
    if (!sp || pt.y < HeaderHeight() || pt.y >= rc.bottom) return -1;
    const int idx = (pt.y - HeaderHeight() + sp->scroll) / RowHeight();
    if (idx < 0 || idx >= (int)sp->rows.size()) return -1;
    const RECT rr = RowRect(idx, rc);
    const RECT saveRc = SaveRect(rr);
    onSave = PtInRect(&saveRc, pt) != FALSE;
    return idx;
}

void QueueResolved(TrackId id, ResolveAction action) {
    if (action == ResolveAction::PlayNow) {
        if (RadioPlaying()) RadioCancelPlayback();
        const bool playing = g.playing != kNone || RadioPlaying();
        QueueTracks({id}, true);
        if (playing) Next();
    } else if (action == ResolveAction::PlayNext) {
        QueueTracks({id}, true);
    } else if (action == ResolveAction::AddToQueue) {
        QueueTracks({id}, false);
    }
}

void DrawCell(HDC dc, const wchar_t* text, RECT r, UINT align = DT_LEFT) {
    DrawTextW(dc, text, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS | align);
}

void DrawUtf8Cell(HDC dc, const std::string& utf8, RECT r) { DrawCell(dc, FromUtf8(utf8).c_str(), r); }

struct ResolveJob {
    uint64_t generation = 0;
    ResolveAction action = ResolveAction::PlayNow;
    bool permanent = false;
    online::SearchTrack searchTrack;
    std::wstring root, dataDir;
    std::shared_ptr<online::Cancel> cancel;
    HWND target = nullptr;
};

void StartResolve(const online::SearchTrack& track, ResolveAction action) {
    OnlineSearchState& s = State();
    if (s.shuttingDown) return;
    const bool isAlbum = action == ResolveAction::SaveAlbum;
    if (isAlbum && track.album.empty()) return;
    const bool wantSavePermanent =
        action == ResolveAction::SaveToLibrary || (online::Config().searchSave == 1 && !g.libraryFolders.empty());
    if ((action == ResolveAction::SaveToLibrary || isAlbum) && g.libraryFolders.empty()) {
        ShowMessage(isAlbum ? L"Add a library folder first, so the album has somewhere to stay."
                            : L"Add a library folder first, so the song has somewhere to stay.");
        return;
    }

    if (!isAlbum) {
        const WebTrack wt = track.ToWebTrack();
        // Already saved in this session? Reuse the library track immediately.
        for (const OnlineRow& r : s.rows) {
            if (r.track.source == track.source && r.track.id == track.id && r.saved && r.savedId != kNone &&
                g_lib.Contains(r.savedId)) {
                if (action == ResolveAction::SaveToLibrary) {
                    ShowMessage(L"Already in your library: " + FromUtf8(track.title) + L".");
                    return;
                }
                if (action == ResolveAction::PlayNow) {
                    if (RadioPlaying()) RadioCancelPlayback();
                    const bool playing = g.playing != kNone || RadioPlaying();
                    QueueTracks({r.savedId}, true);
                    if (playing) Next();
                } else {
                    QueueTracks({r.savedId}, action == ResolveAction::PlayNext);
                }
                return;
            }
        }

        // Already downloaded as a preview in this session? Reuse without re-fetching from YouTube.
        TrackId previewId = kNone;
        if (DiscoverFindPreview(wt.site, wt.id, previewId)) {
            if (action == ResolveAction::SaveToLibrary || wantSavePermanent) {
                if (!DiscoverKeepTrack(previewId)) {
                    if (action == ResolveAction::SaveToLibrary) return;
                } else {
                    for (OnlineRow& r : s.rows) {
                        if (r.track.source == track.source && r.track.id == track.id) {
                            r.saved = true;
                            r.savedId = previewId;
                        }
                    }
                    if (s_hwnd) InvalidateRect(s_hwnd, nullptr, FALSE);
                    if (action == ResolveAction::SaveToLibrary) return;
                }
            }
            QueueResolved(previewId, action);
            return;
        }
    }

    CancelResolveWorker();
    if (s.resolveWorkerBusy.load()) {
        s.hasPendingResolve = true;
        s.pendingTrack = track;
        s.pendingAction = action;
        return;
    }
    if (s.resolveWorker.joinable()) s.resolveWorker.join();

    auto job = std::make_unique<ResolveJob>();
    job->generation = s.resolveGen.Next();
    job->action = action;
    job->permanent = wantSavePermanent;
    job->searchTrack = track;
    wchar_t tag[48]{};
    swprintf_s(tag, L"\\preview\\%llu-%u", GetTickCount64(), ++s.previewSeq);
    job->root = wantSavePermanent ? g.libraryFolders.front() : g.dataDir + tag;
    job->dataDir = wantSavePermanent ? g.dataDir : g.dataDir + L"\\preview";
    job->cancel = std::make_shared<online::Cancel>();
    job->target = g.hwnd;
    s.resolveCancel = job->cancel;
    s.resolving = true;
    s.resolveWorkerBusy = true;

    ShowMessage(isAlbum ? (L"Looking up album " + FromUtf8(track.album) + L"...")
                        : ((action == ResolveAction::SaveToLibrary ? L"Saving " : L"Finding ") +
                           FromUtf8(track.title) + L"..."));
    if (s_hwnd) InvalidateRect(s_hwnd, nullptr, FALSE);

    OnlineSearchState* state = &s;
    s.resolveWorker = std::thread([job = std::move(job), state] {
        auto res = std::make_unique<ResolveWorkerResult>();
        res->generation = job->generation;
        res->action = job->action;
        res->permanent = job->permanent;
        res->dir = (job->action == ResolveAction::SaveAlbum || job->permanent) ? std::wstring() : job->root;

        if (job->action == ResolveAction::SaveAlbum) {
            res->ok = online::FetchSearchAlbum(job->searchTrack, *job->cancel, res->albumList, res->error);
        } else {
            WebFetchRun run;
            run.libraryRoot = job->root;
            run.dataDir = job->dataDir;
            run.cancel = &job->cancel->Flag();
            WebFetchTrackList list;
            list.title = "Search";
            list.tracks.push_back(job->searchTrack.ToWebTrack());
            list.playlist = false;
            run.importLists.push_back(std::move(list));
            std::vector<WebFetchItem> items;
            std::string error;
            if (RunWebFetch(run, items, error) && !items.empty() &&
                (items[0].state == WebState::Done || items[0].state == WebState::Exists) && !items[0].file.empty()) {
                res->ok = true;
                res->albumList.tracks.push_back(std::move(items[0].track));
                res->file = items[0].file;
            } else {
                res->error = !items.empty() && !items[0].why.empty() ? items[0].why : error;
                if (res->error.empty()) res->error = "could not find or download that song";
                if (!res->dir.empty()) RemoveTree(res->dir);
            }
        }
        state->resolveWorkerBusy = false;
        if (!PostMessageW(job->target, WM_APP_ONLINE_RESOLVE, 0, (LPARAM)res.get())) {
            if (!res->dir.empty()) RemoveTree(res->dir);
            return;
        }
        res.release();
    });
}

void ShowContextMenu(int row, POINT screenPt) {
    if (!sp || row < 0 || row >= (int)sp->rows.size()) return;
    const OnlineRow& r = sp->rows[row];
    enum { kPlay = 1, kNext, kQueue, kSave, kSaveAlbum, kArtist };
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, kPlay, L"&Play now\tEnter");
    SetMenuDefaultItem(m, kPlay, FALSE);
    AppendMenuW(m, MF_STRING, kNext, L"Play ne&xt\tShift+Enter");
    AppendMenuW(m, MF_STRING, kQueue, L"Add to &queue\tCtrl+Q");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (r.saved ? MF_GRAYED : MF_ENABLED), kSave,
                r.saved ? L"&Saved in library" : L"&Save to library");
    AppendMenuW(m, MF_STRING | (r.track.album.empty() ? MF_GRAYED : MF_ENABLED), kSaveAlbum,
                L"Save full &album to library...");
    AppendMenuW(m, MF_STRING | (r.track.artist.empty() ? MF_GRAYED : MF_ENABLED), kArtist, L"&Go to artist");
    const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPt.x, screenPt.y, 0, s_hwnd, nullptr);
    DestroyMenu(m);
    if (!sp || row < 0 || row >= (int)sp->rows.size()) return;
    const online::SearchTrack track = sp->rows[row].track;
    switch (cmd) {
        case kPlay: StartResolve(track, ResolveAction::PlayNow); break;
        case kNext: StartResolve(track, ResolveAction::PlayNext); break;
        case kQueue: StartResolve(track, ResolveAction::AddToQueue); break;
        case kSave: StartResolve(track, ResolveAction::SaveToLibrary); break;
        case kSaveAlbum: StartResolve(track, ResolveAction::SaveAlbum); break;
        case kArtist: ShowArtistInfoFor(track.artist); break;
    }
}

void Draw(HDC dc, RECT rc) {
    const Palette& p = P();
    FillSolid(dc, rc, WindowBg());
    SetBkMode(dc, TRANSPARENT);
    FillSolid(dc, {0, 0, rc.right, 1}, Ref(p.border));
    OnlineSearchState& s = State();

    if (!online::Config().searchMusic) {
        HGDIOBJ old = SelectObject(dc, g.font);
        SetTextColor(dc, SubColor());
        DrawCell(dc, L"No local tracks match.", {S(12), 0, rc.right - S(148), rc.bottom});
        RECT btn = PromptButtonRect(rc);
        FillSolid(dc, btn, Ref(s.hoverPrompt ? p.hover : p.surface2));
        HGDIOBJ oldPen = SelectObject(dc, GetStockObject(DC_PEN));
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        SetDCPenColor(dc, Ref(p.accent));
        Rectangle(dc, btn.left, btn.top, btn.right, btn.bottom);
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        SelectObject(dc, g.fontBold);
        SetTextColor(dc, Ref(p.accent));
        DrawCell(dc, L"Search online...", btn, DT_CENTER);
        SelectObject(dc, old);
        return;
    }

    // Header band.
    const int hdrH = HeaderHeight();
    FillSolid(dc, {0, 1, rc.right, hdrH}, SurfaceColor());
    FillSolid(dc, {0, hdrH - 1, rc.right, hdrH}, Ref(p.border));
    HGDIOBJ old = SelectObject(dc, g.fontBold);
    SetTextColor(dc, TextColor());
    DrawCell(dc, L"Online (not in your library)", {S(10), 1, rc.right / 2, hdrH - 1});

    wchar_t countBuf[32]{};
    const wchar_t* rightText = s.status.c_str();
    if (s.status.empty()) {
        if (s.searching) rightText = L"Searching...";
        else if (!s.rows.empty()) {
            swprintf_s(countBuf, L"%zu song%ls", s.rows.size(), s.rows.size() == 1 ? L"" : L"s");
            rightText = countBuf;
        } else rightText = L"No online matches";
    }
    SelectObject(dc, g.font);
    SetTextColor(dc, SubColor());
    DrawCell(dc, rightText, {rc.right / 2, 1, rc.right - S(10), hdrH - 1}, DT_RIGHT);

    if (s.rows.empty()) {
        DrawCell(dc, s.searching ? L"Looking up songs online..." : rightText,
                 {S(12), hdrH + S(8), rc.right - S(12), rc.bottom}, DT_CENTER);
        SelectObject(dc, old);
        return;
    }

    HRGN clip = CreateRectRgn(0, hdrH, rc.right, rc.bottom);
    SelectClipRgn(dc, clip);

    const bool focused = GetFocus() == s_hwnd;
    const int rowH = RowHeight();
    const int first = std::max(0, s.scroll / rowH);
    const int last = std::min<int>((int)s.rows.size(), (int)((s.scroll + (rc.bottom - hdrH) + rowH - 1) / rowH + 1));
    for (int i = first; i < last; ++i) {
        const OnlineRow& r = s.rows[i];
        const RECT rr = RowRect(i, rc);
        const bool sel = i == s.selected;
        const bool hot = !sel && i == s.hoverRow;
        const COLORREF bg = sel ? Ref(p.select) : hot ? Ref(p.hover) : i % 2 ? Ref(RowStripe(p)) : WindowBg();
        const COLORREF fg = sel ? Ref(p.selectText) : TextColor();
        const COLORREF sub = sel ? Ref(p.selectText) : SubColor();
        FillSolid(dc, rr, bg);

        const RECT saveRc = SaveRect(rr);
        const int durW = DurationColWidth();
        const int durRight = saveRc.left - S(8);
        const int avail = std::max(S(60), durRight - durW - S(16));
        const int titleW = avail * 42 / 100;
        const int artistW = avail * 32 / 100;
        const int albumW = avail - titleW - artistW;

        int x = S(10);
        SelectObject(dc, g.font);
        SetTextColor(dc, fg);
        DrawUtf8Cell(dc, r.track.title, {x, rr.top, x + titleW - S(8), rr.bottom});
        x += titleW;

        SetTextColor(dc, sub);
        DrawUtf8Cell(dc, r.track.artist, {x, rr.top, x + artistW - S(8), rr.bottom});
        x += artistW;

        DrawUtf8Cell(dc, r.track.album, {x, rr.top, x + albumW - S(8), rr.bottom});

        if (r.track.durationSec > 0)
            DrawCell(dc, FormatDuration((double)r.track.durationSec).c_str(),
                     {durRight - durW, rr.top, durRight, rr.bottom}, DT_RIGHT);

        if (r.saved) {
            SetTextColor(dc, sel ? Ref(p.selectText) : Ref(p.ok));
            DrawCell(dc, L"Saved", saveRc, DT_CENTER);
        } else {
            const bool saveHot = i == s.hoverRow && s.hoverSave;
            FillSolid(dc, saveRc, Ref(saveHot ? p.accent : sel ? p.surface2 : p.surface));
            SetTextColor(dc, Ref(saveHot ? TextOn(p.accent) : p.accent));
            DrawCell(dc, L"Save", saveRc, DT_CENTER);
        }
        if (sel && focused && g.showFocusBoxes) {
            SetTextColor(dc, fg);
            SetBkColor(dc, bg);
            DrawFocusRect(dc, &rr);
        }
    }
    SelectClipRgn(dc, nullptr);
    DeleteObject(clip);
    SelectObject(dc, old);
}

LRESULT CALLBACK OnlineSearchProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT:
            DoubleBufferedPaint(h, Draw);
            return 0;
        case WM_SIZE:
            ClampScroll();
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_MOUSEMOVE: {
            RECT rc{};
            GetClientRect(h, &rc);
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            OnlineSearchState& s = State();
            if (!online::Config().searchMusic) {
                const RECT btnRc = PromptButtonRect(rc);
                const bool over = PtInRect(&btnRc, pt) != FALSE;
                if (over != s.hoverPrompt) {
                    s.hoverPrompt = over;
                    InvalidateRect(h, nullptr, FALSE);
                }
                return 0;
            }
            bool onSave = false;
            const int row = HitRow(pt, rc, onSave);
            if (row != s.hoverRow || onSave != s.hoverSave) {
                s.hoverRow = row;
                s.hoverSave = onSave;
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            if (sp) {
                sp->hoverRow = -1;
                sp->hoverSave = sp->hoverPrompt = false;
            }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN: {
            RECT rc{};
            GetClientRect(h, &rc);
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (!online::Config().searchMusic) {
                const RECT btnRc = PromptButtonRect(rc);
                if (PtInRect(&btnRc, pt)) EnsureOnlineSearchEnabled();
                return 0;
            }
            SetFocus(h);
            bool onSave = false;
            const int row = HitRow(pt, rc, onSave);
            if (row >= 0 && sp) {
                sp->selected = row;
                InvalidateRect(h, nullptr, FALSE);
                if (onSave && !sp->rows[row].saved) StartResolve(sp->rows[row].track, ResolveAction::SaveToLibrary);
            }
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            if (!online::Config().searchMusic || !sp) return 0;
            RECT rc{};
            GetClientRect(h, &rc);
            bool onSave = false;
            const int row = HitRow({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}, rc, onSave);
            if (row >= 0 && !onSave) {
                sp->selected = row;
                StartResolve(sp->rows[row].track, ResolveAction::PlayNow);
            }
            return 0;
        }
        case WM_RBUTTONUP: {
            if (!online::Config().searchMusic || !sp) return 0;
            RECT rc{};
            GetClientRect(h, &rc);
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            bool onSave = false;
            const int row = HitRow(pt, rc, onSave);
            if (row >= 0) {
                sp->selected = row;
                InvalidateRect(h, nullptr, FALSE);
                ClientToScreen(h, &pt);
                ShowContextMenu(row, pt);
            }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            if (!sp) return 0;
            const int delta = GET_WHEEL_DELTA_WPARAM(wp);
            sp->scroll -= MulDiv(delta, RowHeight() * 3, WHEEL_DELTA);
            ClampScroll();
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_KEYDOWN: {
            if (!online::Config().searchMusic) {
                if (wp == VK_RETURN || wp == VK_SPACE) EnsureOnlineSearchEnabled();
                else if (wp == VK_ESCAPE) SetFocus(g.search);
                return 0;
            }
            if (!sp) return 0;
            OnlineSearchState& s = *sp;
            const int n = (int)s.rows.size();
            if (wp == VK_ESCAPE) {
                SetFocus(g.search);
                return 0;
            }
            if (!n) return 0;
            if (wp == VK_UP && s.selected <= 0) {
                SetFocus(!g.view.empty() ? (CanvasActive() ? g.libView : g.list) : g.search);
                return 0;
            }
            const int moveTo = wp == VK_UP ? s.selected - 1 : wp == VK_DOWN ? s.selected + 1
                             : wp == VK_HOME ? 0 : wp == VK_END ? n - 1 : -1;
            if (moveTo >= 0) {
                s.selected = std::clamp(moveTo, 0, n - 1);
                EnsureVisible(s.selected);
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if (wp == VK_RETURN && s.selected >= 0 && s.selected < n) {
                StartResolve(s.rows[s.selected].track,
                             GetKeyState(VK_SHIFT) < 0 ? ResolveAction::PlayNext : ResolveAction::PlayNow);
                return 0;
            }
            if (wp == 'Q' && GetKeyState(VK_CONTROL) < 0 && s.selected >= 0 && s.selected < n) {
                StartResolve(s.rows[s.selected].track, ResolveAction::AddToQueue);
                return 0;
            }
            if ((wp == VK_APPS || (wp == VK_F10 && GetKeyState(VK_SHIFT) < 0)) && s.selected >= 0 && s.selected < n) {
                RECT rc{};
                GetClientRect(h, &rc);
                RECT rr = RowRect(s.selected, rc);
                POINT pt{rr.left + S(24), rr.bottom};
                ClientToScreen(h, &pt);
                ShowContextMenu(s.selected, pt);
                return 0;
            }
            return 0;
        }
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            InvalidateRect(h, nullptr, FALSE);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

void CreateOnlineSearch(HWND parent) {
    RegisterAppClass(kOnlineSearchClass, OnlineSearchProc, CS_DBLCLKS, false);
    s_hwnd = CreateWindowExW(0, kOnlineSearchClass, L"Online search results", WS_CHILD | WS_TABSTOP, 0, 0, 0, 0,
                             parent, nullptr, g.inst, nullptr);
    if (sp) sp->shuttingDown = false;
}

HWND OnlineSearchWindow() { return s_hwnd; }

bool OnlineSearchVisible() {
    if (!s_hwnd || RadioActive() || g.filter.empty()) return false;
    if (!online::SearchableQuery(g.filter)) return false;
    return online::Config().searchMusic || g.view.empty();
}

int OnlineSearchWantedHeight(int availH) {
    if (!OnlineSearchVisible() || availH <= 0) return 0;
    if (!online::Config().searchMusic) return std::min(availH, S(34));
    const int rows = std::max(1, sp ? (int)sp->rows.size() : 0);
    const int desired = HeaderHeight() + rows * RowHeight() + S(4);
    const int minH = std::min(availH, HeaderHeight() + RowHeight() * 2);
    const int maxH = g.view.empty() ? std::max(minH, availH - S(72)) : std::max(minH, availH * 45 / 100);
    return std::clamp(desired, minH, maxH);
}

void OnlineSearchQueryChanged(const std::wstring& query) {
    if (!sp && !online::SearchableQuery(query)) {
        UpdateLayoutIfNeeded();
        return;
    }
    OnlineSearchState& s = State();
    s.query = query;
    CancelSearchWorker();
    if (!online::SearchableQuery(query)) {
        s.rows.clear();
        s.status.clear();
        s.selected = -1;
        s.scroll = 0;
        if (query.empty()) {
            s.ownedRevision = UINT32_MAX;
            s.owned = import::OwnedIndex();
        }
        UpdateLayoutIfNeeded();
        return;
    }
    if (online::Config().searchMusic) {
        s.status = L"Searching...";
        s.searching = true;
        if (!SetTimer(g.hwnd, kOnlineSearchTimer, 400, nullptr)) OnlineSearchTimer();
    }
    UpdateLayoutIfNeeded();
}

void OnlineSearchTimer() {
    KillTimer(g.hwnd, kOnlineSearchTimer);
    if (!sp) {
        UpdateLayoutIfNeeded();
        return;
    }
    OnlineSearchState& s = *sp;
    if (s.shuttingDown || !online::Config().searchMusic || !online::SearchableQuery(s.query)) {
        s.searching = false;
        UpdateLayoutIfNeeded();
        return;
    }
    if (s.searchWorkerBusy.load()) {
        SetTimer(g.hwnd, kOnlineSearchTimer, 80, nullptr);
        return;
    }
    if (s.searchWorker.joinable()) s.searchWorker.join();

    RefreshOwnedSnapshot();
    const std::string qUtf8 = online::NormalizeSearchQuery(s.query);
    if (qUtf8.size() < 2) {
        s.searching = false;
        UpdateLayoutIfNeeded();
        return;
    }
    const uint64_t gen = s.searchGen.Next();
    auto cancel = std::make_shared<online::Cancel>();
    s.searchCancel = cancel;
    s.searching = true;
    s.searchWorkerBusy = true;
    s.status = L"Searching...";
    if (s_hwnd) InvalidateRect(s_hwnd, nullptr, FALSE);

    const HWND target = g.hwnd;
    const std::wstring queryW = s.query;
    import::OwnedIndex ownedCopy = s.owned;
    OnlineSearchState* state = &s;
    s.searchWorker = std::thread([gen, queryW, qUtf8, ownedCopy = std::move(ownedCopy), cancel, target, state] {
        auto res = std::make_unique<SearchWorkerResult>();
        res->generation = gen;
        res->query = queryW;
        res->reply = online::SearchTracks(qUtf8, ownedCopy, *cancel, kMaxShownResults);
        state->searchWorkerBusy = false;
        if (!PostMessageW(target, WM_APP_ONLINE_SEARCH, 0, (LPARAM)res.get())) return;
        res.release();
    });
}

void OnlineSearchViewUpdated() { UpdateLayoutIfNeeded(); }

void OnlineSearchSettingChanged() {
    if (!online::Config().searchMusic) {
        CancelSearchWorker();
        if (sp) {
            sp->rows.clear();
            sp->status.clear();
            sp->selected = -1;
            sp->scroll = 0;
        }
        UpdateLayoutIfNeeded();
    } else if (sp && online::SearchableQuery(sp->query)) {
        OnlineSearchQueryChanged(sp->query);
    } else {
        UpdateLayoutIfNeeded();
    }
}

void OnlineSearchThemeChanged() {
    if (s_hwnd) InvalidateRect(s_hwnd, nullptr, FALSE);
}

void OnlineSearchTrimMemory() {
    if (sp) {
        sp->ownedRevision = UINT32_MAX;
        sp->owned = import::OwnedIndex();
    }
    online::ClearSearchCache();
}

bool OnlineSearchFocus() {
    if (!OnlineSearchVisible() || !s_hwnd) return false;
    SetFocus(s_hwnd);
    if (online::Config().searchMusic && sp && !sp->rows.empty() && sp->selected < 0) {
        sp->selected = 0;
        EnsureVisible(0);
    }
    InvalidateRect(s_hwnd, nullptr, FALSE);
    return true;
}

bool OnlineSearchActivateDefault() {
    if (!OnlineSearchVisible()) return false;
    if (!online::Config().searchMusic) return EnsureOnlineSearchEnabled();
    if (!sp || sp->rows.empty()) return false;
    const int idx = (sp->selected >= 0 && sp->selected < (int)sp->rows.size()) ? sp->selected : 0;
    StartResolve(sp->rows[idx].track, ResolveAction::PlayNow);
    return true;
}

void OnlineSearchPlay(const online::SearchTrack& track, bool next, bool queueEnd) {
    StartResolve(track, queueEnd ? ResolveAction::AddToQueue
                                 : (next ? ResolveAction::PlayNext : ResolveAction::PlayNow));
}

void OnlineSearchSave(const online::SearchTrack& track) { StartResolve(track, ResolveAction::SaveToLibrary); }

void OnlineSearchSaveAlbum(const online::SearchTrack& track) { StartResolve(track, ResolveAction::SaveAlbum); }

const import::OwnedIndex& OnlineSearchOwnedSnapshot() {
    RefreshOwnedSnapshot();
    return State().owned;
}

void OnOnlineSearchMessage(WPARAM, LPARAM lp) {
    std::unique_ptr<SearchWorkerResult> res(reinterpret_cast<SearchWorkerResult*>(lp));
    if (!res || !sp) return;
    OnlineSearchState& s = *sp;
    if (s.shuttingDown || !s.searchGen.Accepts(res->generation)) return;
    s.searching = false;
    s.rows.clear();
    s.selected = -1;
    s.scroll = 0;
    if (!res->reply.ok) {
        s.status = FromUtf8(res->reply.error);
        UpdateLayoutIfNeeded();
        return;
    }
    for (const auto& t : res->reply.tracks) s.rows.push_back({t});
    s.status.clear();
    if (!s.rows.empty()) s.selected = 0;
    UpdateLayoutIfNeeded();
}

void OnOnlineResolveMessage(WPARAM, LPARAM lp) {
    std::unique_ptr<ResolveWorkerResult> res(reinterpret_cast<ResolveWorkerResult*>(lp));
    if (!sp) {
        if (res && !res->dir.empty()) RemoveTree(res->dir);
        return;
    }
    OnlineSearchState& s = *sp;
    auto runPending = [&] {
        if (!s.shuttingDown && s.hasPendingResolve) {
            s.hasPendingResolve = false;
            online::SearchTrack nextTrack = std::move(s.pendingTrack);
            ResolveAction nextAction = s.pendingAction;
            StartResolve(nextTrack, nextAction);
        }
    };
    if (!res) return;
    if (s.shuttingDown || !s.resolveGen.Accepts(res->generation)) {
        if (!res->dir.empty()) RemoveTree(res->dir);
        runPending();
        return;
    }
    s.resolving = false;
    if (res->action == ResolveAction::SaveAlbum) {
        if (!res->ok || res->albumList.tracks.empty()) {
            ShowMessage(L"Couldn't get album: " + FromUtf8(res->error.empty() ? "no tracks found" : res->error) + L".");
        } else {
            ShowWebFetchLists({std::move(res->albumList)});
        }
        if (s_hwnd) InvalidateRect(s_hwnd, nullptr, FALSE);
        runPending();
        return;
    }
    if (!res->ok || res->albumList.tracks.empty()) {
        if (!res->dir.empty()) RemoveTree(res->dir);
        ShowMessage(L"Couldn't get song: " + FromUtf8(res->error) + L".");
        if (s_hwnd) InvalidateRect(s_hwnd, nullptr, FALSE);
        runPending();
        return;
    }
    WebTrack& resolved = res->albumList.tracks[0];
    const TrackId id = DiscoverAddTrack(res->file, resolved, res->permanent, res->dir);
    if (id == kInvalidTrack) {
        ShowMessage(L"Couldn't open downloaded song.");
        return;
    }
    if (res->permanent) {
        for (OnlineRow& r : s.rows) {
            if (r.track.source == resolved.site && r.track.id == resolved.id) {
                r.saved = true;
                r.savedId = id;
            }
        }
        if (res->action == ResolveAction::SaveToLibrary)
            ShowMessage(L"Saved to your library: " + FromUtf8(resolved.title) + L".");
    } else {
        for (OnlineRow& r : s.rows) {
            if (r.track.source == resolved.site && r.track.id == resolved.id) r.previewId = id;
        }
    }

    QueueResolved(id, res->action);
    if (s_hwnd) InvalidateRect(s_hwnd, nullptr, FALSE);
    runPending();
}

void StopOnlineSearch() {
    if (sp) sp->shuttingDown = true;
    CancelSearchWorker();
    CancelResolveWorker();
    if (sp) {
        sp->hasPendingResolve = false;
        if (sp->searchWorker.joinable()) sp->searchWorker.join();
        if (sp->resolveWorker.joinable()) sp->resolveWorker.join();
    }
    if (g.hwnd) {
        MSG pending{};
        while (PeekMessageW(&pending, g.hwnd, WM_APP_ONLINE_SEARCH, WM_APP_ONLINE_SEARCH, PM_REMOVE))
            delete reinterpret_cast<SearchWorkerResult*>(pending.lParam);
        while (PeekMessageW(&pending, g.hwnd, WM_APP_ONLINE_RESOLVE, WM_APP_ONLINE_RESOLVE, PM_REMOVE)) {
            auto* r = reinterpret_cast<ResolveWorkerResult*>(pending.lParam);
            if (r && !r->dir.empty()) RemoveTree(r->dir);
            delete r;
        }
    }
    delete sp;
    sp = nullptr;
    online::ClearSearchCache();
}

size_t OnlineSearchRowCount() { return sp ? sp->rows.size() : 0; }

void SetOnlineSearchTestRows(const std::vector<online::SearchTrack>& rows) {
    OnlineSearchState& s = State();
    s.rows.clear();
    s.searching = false;
    s.status.clear();
    for (const auto& t : rows) s.rows.push_back({t});
    s.selected = s.rows.empty() ? -1 : 0;
    s.scroll = 0;
    UpdateLayoutIfNeeded();
}

}  // namespace app
