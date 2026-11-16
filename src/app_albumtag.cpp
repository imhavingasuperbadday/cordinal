// Tag album from MusicBrainz (Edit menu and the track menu): looks the selected album up on
// MusicBrainz and shows the releases it found, best match first, with every tag each would change.
// Lengths pick the right pressing; when no release matches clearly, none is chosen for you. Apply
// writes the chosen release's tags through the tag writer as one edit, so Edit > Undo last tag edit
// takes the whole album back. The tags are read and the service asked on a worker; the result comes
// back to this window and is dropped if the window was closed or opened again meanwhile. The model
// (album_tagger.h) is shared with the Linux front end.
#include "album_tagger.h"
#include "app.h"
#include "metadata_fetch.h"
#include "tags.h"

#include <memory>
#include <mutex>
#include <thread>

namespace app {

namespace {

const wchar_t kTaggerClass[] = L"CordinalAlbumTagger";
constexpr UINT kFound = WM_APP + 1;  // wParam: the generation the result in Tagger::slot belongs to

enum Id : int { kReleases = 100, kChanges };

struct Found {
    std::vector<albumtag::LocalTrack> local;
    albumtag::SearchResult result;
};
// The worker leaves its result here rather than in the message, so nothing leaks when the window
// closes with the message still queued.
struct Slot {
    std::mutex mutex;
    std::unique_ptr<Found> found;
};

struct Tagger {
    HWND hwnd = nullptr, status = nullptr, releasesLabel = nullptr, releases = nullptr, changesLabel = nullptr,
         changes = nullptr, info = nullptr, apply = nullptr, cancel = nullptr;
    std::vector<albumtag::LocalTrack> local;
    albumtag::SearchResult result;
    std::vector<std::pair<TrackId, TagEdit>> edits;  // the chosen release's
    std::shared_ptr<online::Cancel> stop;
    std::shared_ptr<Slot> slot;
    std::thread worker;
    unsigned generation = 0;
} t;

albumtag::Fetch g_fetch;  // the self-test's canned replies; empty asks MusicBrainz

std::wstring W(const std::string& s) { return FromUtf8(s); }

void StopWorker() {
    if (t.stop) t.stop->Stop();
    if (t.worker.joinable()) t.worker.join();
    t.stop.reset();
    t.slot.reset();
}

std::wstring ReleaseRow(const albumtag::Candidate& c) {
    const auto& r = c.release;
    std::wstring row = std::to_wstring((int)std::lround(c.match.confidence * 100)) + L"%\t" + W(r.title);
    if (!r.artist.empty()) row += L"  \x2014  " + W(r.artist);
    std::wstring details;
    auto add = [&](const std::string& part) {
        if (!part.empty()) details += (details.empty() ? L"" : L", ") + W(part);
    };
    add(r.date);
    add(r.country);
    add(r.format);
    add(std::to_string(r.trackCount) + (r.trackCount == 1 ? " track" : " tracks"));
    add(r.label);
    return row + L"\t" + details;
}

// The chosen release's changes, one row per field, under the track they belong to.
void ShowChanges() {
    const int row = (int)SendMessageW(t.releases, LB_GETCURSEL, 0, 0);
    SendMessageW(t.changes, WM_SETREDRAW, FALSE, 0);
    SendMessageW(t.changes, LB_RESETCONTENT, 0, 0);
    t.edits.clear();
    std::wstring text;
    if (row >= 0 && (size_t)row < t.result.candidates.size()) {
        const auto& c = t.result.candidates[(size_t)row];
        std::vector<albumtag::Change> changes;
        t.edits = albumtag::Propose(t.local, c.release, c.match, &changes);
        for (size_t i = 0; i < t.local.size(); ++i) {
            const auto& l = t.local[i];
            const std::wstring name = W(l.title.empty() ? "(untitled)" : l.title);
            if (c.match.pick[i] < 0) {
                SendMessageW(t.changes, LB_ADDSTRING, 0, (LPARAM)(name + L"\tNot on this release; left as it is").c_str());
                continue;
            }
            bool first = true;
            for (const auto& change : changes) {
                if (change.track != i) continue;
                const std::wstring before = change.before.empty() ? L"(empty)" : W(change.before);
                const std::wstring line = (first ? name : std::wstring()) + L"\t" + W(change.field) + L":  " + before +
                                          L"  \x2192  " + W(change.after);
                SendMessageW(t.changes, LB_ADDSTRING, 0, (LPARAM)line.c_str());
                first = false;
            }
        }
        if (t.edits.empty()) text = L"These tracks already have this release's tags.";
        else
            text = L"Apply writes the tags of " + std::to_wstring(t.edits.size()) +
                   (t.edits.size() == 1 ? L" file" : L" files") + L". Edit > Undo last tag edit takes it back.";
    } else if (!t.result.candidates.empty()) {
        text = L"Choose the release these tracks come from to see what would change.";
    }
    SendMessageW(t.changes, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(t.changes, nullptr, TRUE);
    SetWindowTextW(t.info, text.c_str());
    EnableWindow(t.apply, !t.edits.empty());
}

void ShowFound(Found& found) {
    t.local = std::move(found.local);
    t.result = std::move(found.result);
    SendMessageW(t.releases, LB_RESETCONTENT, 0, 0);
    for (const auto& c : t.result.candidates) SendMessageW(t.releases, LB_ADDSTRING, 0, (LPARAM)ReleaseRow(c).c_str());
    std::wstring status;
    if (t.result.offline) status = L"MusicBrainz couldn't be reached. Check the connection and try again.";
    else if (t.result.candidates.empty()) status = L"MusicBrainz has no release by this name and artist.";
    else if (t.result.confident) status = L"The first release matches these tracks' titles and lengths.";
    else status = L"No release matches clearly. Choose one, or cancel.";
    SetWindowTextW(t.status, status.c_str());
    if (t.result.confident) SendMessageW(t.releases, LB_SETCURSEL, 0, 0);
    ShowChanges();
}

void SetTabs(HWND list, std::initializer_list<int> pixels) {
    LONG units = std::max<LONG>(1, LOWORD(GetDialogBaseUnits()));
    std::vector<int> stops;
    for (int p : pixels) stops.push_back(MulDiv(p, 4, units));
    SendMessageW(list, LB_SETTABSTOPS, (WPARAM)stops.size(), (LPARAM)stops.data());
}

void Layout(HWND h) {
    RECT rc;
    GetClientRect(h, &rc);
    const int pad = S(14), w = rc.right - pad * 2;
    int y = pad;
    MoveWindow(t.status, pad, y, w, S(22), TRUE);
    y += S(30);
    MoveWindow(t.releasesLabel, pad, y, w, S(20), TRUE);
    y += S(22);
    MoveWindow(t.releases, pad, y, w, S(130), TRUE);
    y += S(140);
    MoveWindow(t.changesLabel, pad, y, w, S(20), TRUE);
    y += S(22);
    const int bottom = rc.bottom - pad - S(28) - S(30);
    MoveWindow(t.changes, pad, y, w, std::max(S(60), bottom - y - S(6)), TRUE);
    MoveWindow(t.info, pad, bottom, w, S(22), TRUE);
    MoveWindow(t.apply, rc.right - pad - S(240) - S(8), rc.bottom - pad - S(28), S(140), S(28), TRUE);
    MoveWindow(t.cancel, rc.right - pad - S(100), rc.bottom - pad - S(28), S(100), S(28), TRUE);
    SetTabs(t.releases, {S(44), std::max(S(200), w * 3 / 5)});
    SetTabs(t.changes, {std::max(S(160), w / 3)});
}

LRESULT CALLBACK AlbumTaggerProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            t.hwnd = h;
            auto make = [&](DWORD ex, const wchar_t* cls, const wchar_t* text, DWORD style, int id = 0) {
                HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, h, (HMENU)(INT_PTR)id,
                                         g.inst, nullptr);
                SendMessageW(c, WM_SETFONT, (WPARAM)g.font, TRUE);
                return c;
            };
            const DWORD list = WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_USETABSTOPS;
            t.status = make(0, WC_STATICW, L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS);
            t.releasesLabel = make(0, WC_STATICW, L"&Releases on MusicBrainz", SS_LEFT);
            t.releases = make(WS_EX_CLIENTEDGE, WC_LISTBOXW, L"", list, kReleases);
            t.changesLabel = make(0, WC_STATICW, L"&What changes", SS_LEFT);
            t.changes = make(WS_EX_CLIENTEDGE, WC_LISTBOXW, L"", list | WS_HSCROLL, kChanges);
            for (HWND l : {t.releases, t.changes}) SetWindowTheme(l, g.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
            t.info = make(0, WC_STATICW, L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS);
            t.apply = make(0, WC_BUTTONW, L"&Apply", BS_DEFPUSHBUTTON | WS_TABSTOP | WS_DISABLED, IDOK);
            t.cancel = make(0, WC_BUTTONW, L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL);
            for (HWND c : {t.apply, t.cancel}) ApplyControlTheme(c, g.dark);
            ApplyWindowTheme(h, P());
            return 0;
        }
        case WM_SIZE:
            Layout(h);
            return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {S(560), S(480)};
            return 0;
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(h, &rc);
            FillRect((HDC)wp, &rc, g.surfaceBrush);
            return 1;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            return SurfaceControlColor((HDC)wp, (HWND)lp == t.info ? SubColor() : TextColor());
        case WM_CTLCOLORLISTBOX:
            return FieldControlColor((HDC)wp);
        case kFound: {
            std::unique_ptr<Found> found;
            if (t.slot && (unsigned)wp == t.generation) {
                std::lock_guard lock(t.slot->mutex);
                found = std::move(t.slot->found);
            }
            if (found) ShowFound(*found);
            return 0;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wp), code = HIWORD(wp);
            if (id == kReleases && code == LBN_SELCHANGE) ShowChanges();
            else if (id == IDOK) {
                if (t.edits.empty() || !SubmitTagWrites(t.edits, true, true)) return 0;  // a write still running: stay open
                DestroyWindow(h);
            } else if (id == IDCANCEL) DestroyWindow(h);
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            StopWorker();
            ++t.generation;  // a result still on its way finds no window to show in
            t.hwnd = nullptr;
            t.local.clear();
            t.result = {};
            t.edits.clear();
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

void SetAlbumTaggerFetch(albumtag::Fetch fetch) { g_fetch = std::move(fetch); }

bool AlbumTaggerDialogMessage(MSG* msg) {
    return t.hwnd && IsDialogMessageW(t.hwnd, msg) != FALSE;
}

void ShowAlbumTagger(const std::vector<TrackId>& selection) {
    if (selection.empty()) return;
    if (t.hwnd) DestroyWindow(t.hwnd);
    StopWorker();
    // The worker gets copies: each track as the library knows it, which the file's own tags replace
    // when they can be read, and its file.
    struct Item { albumtag::LocalTrack track; std::wstring path; };
    std::vector<Item> items;
    std::wstring album;
    for (TrackId id : AlbumTracksOf(selection)) {
        const Track& track = g_lib[id];
        albumtag::LocalTrack l;
        l.id = id;
        auto text = [&](StringPool::Handle h) { return h ? ToUtf8(g_lib.Str(h)) : std::string(); };
        l.title = text(track.title);
        l.artist = text(track.artist);
        l.album = text(track.album);
        l.albumArtist = text(track.albumArtist);
        l.date = text(track.date);
        l.label = text(track.label);
        l.track = track.trackNo;
        l.trackTotal = track.trackTotal;
        l.disc = track.disc;
        l.discTotal = track.discTotal;
        l.seconds = track.Duration();
        items.push_back({std::move(l), g_lib.Path(id)});
        if (album.empty() && track.album) album = g_lib.Str(track.album);
    }
    if (album.empty()) {
        MessageBoxW(g.hwnd, L"These tracks have no album name to look up. Give them one with Edit album tags first.",
                    kAppName, MB_OK | MB_ICONINFORMATION);
        return;
    }
    // The lookup may run while it is off, but only after the user agrees to what it sends. The
    // self-test's canned replies send nothing.
    if (!online::Config().albumTags && !g_fetch) {
        if (MessageBoxW(g.hwnd,
            L"Album tags from MusicBrainz is off, so cordinal hasn't contacted anyone.\n\n"
            L"It sends the album's title and artist to musicbrainz.org, then the ids of the few releases "
            L"that match, to read their tracks. Nothing is written until you choose a release and Apply.\n\n"
            L"Turn it on? It can be changed in Preferences > Online.",
            L"Tag album from MusicBrainz", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
        if (const Tweak* tweak = FindTweak("online.album_tags")) SetTweak(*tweak, 1);
        if (!online::Config().albumTags) return;
    }
    RegisterAppClass(kTaggerClass, AlbumTaggerProc);
    RECT r{0, 0, S(760), S(600)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_DLGMODALFRAME, g.dpi);
    RECT owner;
    GetWindowRect(g.hwnd, &owner);
    const int w = r.right - r.left, h = r.bottom - r.top;
    CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kTaggerClass, L"Tag album from MusicBrainz",
                    WS_OVERLAPPEDWINDOW & ~WS_MINIMIZEBOX | WS_CLIPCHILDREN, owner.left + (owner.right - owner.left - w) / 2,
                    owner.top + S(40), w, h, g.hwnd, nullptr, g.inst, nullptr);
    if (!t.hwnd) return;
    SetWindowTextW(t.status, (L"Asking MusicBrainz about \x201C" + album + L"\x201D\x2026").c_str());
    ShowWindow(t.hwnd, SW_SHOW);
    SetFocus(t.releases);
    t.stop = std::make_shared<online::Cancel>();
    t.slot = std::make_shared<Slot>();
    t.worker = std::thread([items = std::move(items), stop = t.stop, slot = t.slot, hwnd = t.hwnd, generation = t.generation,
                            fetch = g_fetch] {
        auto found = std::make_unique<Found>();
        for (const auto& item : items) {
            albumtag::LocalTrack l = item.track;
            TagData d;
            if (!ReadTags(item.path, d)) {
                found->local.push_back(std::move(l));
                continue;
            }
            l.title = d.title;
            l.artist = d.artist;
            l.album = d.album;
            l.albumArtist = d.albumArtist;
            l.date = d.date;
            l.label = d.label;
            l.release = d.musicBrainzRelease;
            l.releaseGroup = d.musicBrainzReleaseGroup;
            l.track = d.track;
            l.trackTotal = d.trackTotal;
            l.disc = d.disc;
            l.discTotal = d.discTotal;
            found->local.push_back(std::move(l));
        }
        found->result = albumtag::Search(found->local, *stop, fetch);
        if (stop->Stopped()) return;
        {
            std::lock_guard lock(slot->mutex);
            slot->found = std::move(found);
        }
        PostMessageW(hwnd, kFound, generation, 0);
    });
}

}  // namespace app
