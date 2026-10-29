// The Linux front end's window, drawn to look like the Windows build's normal window in every theme
// (docs/ui-reference/windows holds its screenshots; ui_spec.h its measurements, shared with Windows):
//
//   menu bar: File  Edit  Playback  Playlist  Library  View  Help
//   handle | prev play next  shuffle repeat resume | elapsed | seek bar | remaining | speaker volume
//   cover  title / artist  •  album  •  year
//   tabs (Library, Now Playing, playlists...)                                         | search
//   view row (Library tab): Tracks, Albums & Tracks, Albums, Artists | Browser
//   the column browser (Library tab, when shown)
//   column headers, then the track list with album headers over each album's tracks (or a view, libview.cpp)
//   status: message or playback | work in progress | track count and length
//
// Everything is drawn here with the SDL renderer, laid out in pixels from the display scale. Where
// Windows uses native controls (menus, the search box, the scroll bar) this draws their Windows 11
// look. Features the Linux build doesn't have are left out of the menus rather than shown disabled.
#include "ui.h"

#include "app.h"
#include "archive.h"
#include "cue.h"
#include "draw.h"
#include "libview.h"
#include "panels.h"
#include "qr.h"
#include "peaks.h"
#include "points.h"
#include "radio_ui.h"
#include "steam.h"
#include "text.h"
#include "titleformat.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cwctype>
#include <map>
#include <numeric>
#include <unordered_map>
#include <unordered_set>

namespace lx {

namespace {

using ui::kColumns;

// --- geometry ------------------------------------------------------------------------------------
// The window's parts, from DoLayout. The names follow ui::kLayout.
struct Layout {
    Rect menuBar, grip, buttons[6], elapsed, seek, remaining, speaker, volume, cover, title, subtitle, chip, lyrics;
    Rect tabs, radioTab, search, viewRow, browser, splitter, list, header, rows, scrollbar, status, visuals[2];
    Rect radioSaved, radioDiscover, radioGenre, radioCountry, radioState, radioSearch, radioGrid;
    std::vector<Rect> tabRects, barItems;
    Rect views[kViewButtons];
    float rowH = 0;
} L;
enum Button { kBtnPrev, kBtnPlay, kBtnNext, kBtnShuffle, kBtnRepeat, kBtnResume };

// What the mouse is doing.
enum class Drag { None, Seek, Volume, Scroll, ColumnEdge, Tracks, Splitter };
uint64_t g_lastInput = 0;  // when the last key or mouse event arrived (preferences_ui.cpp waits for quiet)
Drag g_drag = Drag::None;
int g_dragColumn = -1;
float g_dragStart = 0, g_dragStartValue = 0;
double g_seekPreview = -1;  // while dragging the seek bar
float g_mouseX = -1, g_mouseY = -1;
uint64_t g_lastClick = 0;
int g_lastClickLine = -1;
bool g_altAlone = false;  // Alt went down with nothing else: its release opens the menu bar
float g_muteVolume = 0;   // the volume before Mute, 0 while not muted
TrackId g_sectionMenuTrack = kNone;
double g_sectionMenuPos = 0;
struct TrackDrag {
    bool pending = false, collapseOnClick = false;
    float x = 0, y = 0;
    int clicked = -1, target = -1;
    uint64_t playlist = 0;
    std::vector<size_t> sources;
    std::vector<TrackId> snapshot;
} g_trackDrag;

// --- album groups ------------------------------------------------------------------------------------
// Consecutive tracks of one album (two or more) under a two-row header, as on Windows
// (app_groups.cpp). A display line is a track row (`row` >= 0) or a header's half (`row` < 0).
struct Group {
    int first = 0, count = 0;
    StringPool::Handle album = 0, artist = 0, date = 0;
    double seconds = 0;
};
struct Line {
    int row, group, part;
};
std::vector<Group> g_groups;
std::vector<Line> g_lines;
std::vector<int> g_lineOfRow;

// Album covers (the list's album headers and the views), by the album, its first track's folder and
// the size asked for. The views can ask for thousands, so the least recently drawn are let go past
// kThumbsKept.
struct Thumb {
    SDL_Texture* texture = nullptr;
    bool done = false;
    uint64_t used = 0;  // the frame it was last drawn in
};
constexpr size_t kThumbsKept = 600;
uint64_t g_frame = 0;
std::unordered_map<uint64_t, Thumb> g_thumbs;
std::unordered_map<TrackId, uint64_t> g_thumbFor;  // requests in flight
int g_thumbsPending = 0;

// Popup menus: the menu bar's, the list's and the header's. A stack: [0] is the menu, and each
// open submenu follows it.
enum Action {
    kNoAction,
    kAddFiles, kAddFolder, kOpenPlaylistFile, kSavePlaylistFile, kExit,
    kBackup, kRestore, kStatistics,
    kSelectAll, kRemove, kKeepSelected, kClear, kReloadTags, kRemoveDuplicates, kRemoveDead, kCancelDead, kFind,
    kPlayPause, kStopAction, kMute, kPrevAction, kNextAction, kShuffleAction, kShuffleAlbumsAction, kRadio, kRepeatSet, kStopAfter, kSpeed, kTempo, kShared, kExclusive,
    kNewPl, kRenamePl, kClosePl, kDuplicatePl, kReopenPl, kMovePlLeft, kMovePlRight, kNextPl, kPrevPl, kReveal,
    kFolders, kRescan, kAcceptInbox, kAcceptAllInbox, kExcludeLibrary, kDuplicateAudio, kCancelDuplicates,
    kSteam, kCancelSteam, kWebServer, kExportCord, kCancelExport, kConvert, kConvertUndo, kCancelConvert, kOrganize, kOrganizeUndo, kCopyTo, kCancelCopy,
    kTheme, kOpenRadio, kGroupAlbums, kStartOnLogin, kNameThatTrack, kWhichCameFirst, kPoints, kReview, kPalette, kLyrics, kLayout, kMini, kPacks, kPackPlaylist, kHelpers, kHelperTool, kHelperCancel,
    kAbout,
    kPlay, kPlayNext, kAddToQueue, kClearQueue, kAddToNowPlaying, kAddToPlaylist, kRemoveFromNowPlaying, kOpenFolder, kColumnToggle,
    kSectionPlay, kSectionLoop, kSectionStart, kSectionEnd, kSectionClear,
    kRating, kEditTags, kUndoTags, kEqualizer, kPreferences, kRgEnable, kRgScanSel, kRgScanPl, kRgCancel, kRgClear, kRgWrite, kWriteStatistics,
    kNewAuto, kEditAuto, kChoice,
};
std::function<void(int)> g_choicePick;  // the choice a panel's OpenChoice menu is for
struct MenuItem {
    std::string label, shortcut;
    int action = kNoAction, arg = 0;
    bool checked = false, radio = false, enabled = true, separator = false;
    std::vector<MenuItem> sub;
};
MenuItem Item(std::string label, int action, std::string shortcut = {}, bool checked = false, bool enabled = true) {
    MenuItem m;
    m.label = std::move(label);
    m.shortcut = std::move(shortcut);
    m.action = action;
    m.checked = checked;
    m.enabled = enabled;
    return m;
}
MenuItem Separator() {
    MenuItem m;
    m.separator = true;
    return m;
}
MenuItem Radio(std::string label, int action, int arg, bool on, std::string shortcut = {}) {
    MenuItem m = Item(std::move(label), action, std::move(shortcut), on);
    m.radio = true;
    m.arg = arg;
    return m;
}
MenuItem Submenu(std::string label, std::vector<MenuItem> items) {
    MenuItem m = Item(std::move(label), kNoAction);
    m.sub = std::move(items);
    return m;
}
std::vector<MenuItem> CordExportItems() {
    std::vector<MenuItem> items;
    for (unsigned f = 1; f < (unsigned)CordExport::Count; ++f) {
        auto item = Item(ToUtf8(kCordExports[f].label) + "...", kExportCord, {}, false, !CordExportBusy());
        item.arg = (int)f;
        items.push_back(std::move(item));
    }
    return items;
}
constexpr double kSpeeds[] = {0.5, 0.75, 0.9, 1.0, 1.1, 1.25, 1.5, 2.0};
constexpr double kTempos[] = {0.75, 0.9, 1.0, 1.1, 1.25, 1.5, 1.75, 2.0};

std::vector<MenuItem> RateItems(const double* rates, size_t count, double selected, int action) {
    std::vector<MenuItem> items;
    for (size_t i = 0; i < count; ++i) {
        char label[32];
        snprintf(label, sizeof(label), rates[i] == 1 ? "Normal" : "%gx", rates[i]);
        items.push_back(Radio(label, action, (int)i, rates[i] == selected));
    }
    return items;
}
struct Popup {
    float x = 0, y = 0, w = 0, h = 0;
    std::vector<MenuItem> items;
    int hover = -1;
};
std::vector<Popup> g_popups;
int g_barOpen = -1;  // the menu bar's open menu, or -1

struct ClosedPlaylist { Playlist playlist; int index; };
std::deque<ClosedPlaylist> g_closedPlaylists;
int g_renameIndex = -1;
std::wstring g_renameText;
bool g_renameSelected = false;
size_t g_renameCaret = 0;
uint64_t g_nextPlaylistIdentity = 1;

struct PlaylistFile {
    bool save = false, ok = false, failed = false, addToExisting = false, checkDead = false;
    bool findDuplicates = false, findSteam = false, cancelled = false;
    std::wstring path;
    std::vector<M3UExportEntry> entries;
    std::vector<std::wstring> paths;
    std::vector<std::wstring> inputs;
    std::vector<std::wstring> watchedFolders;
    std::vector<TrackId> snapshot;
    std::vector<std::pair<TrackId, std::wstring>> checkedPaths, missing;
    std::vector<std::pair<uint64_t, TrackId>> hashes;
    std::vector<std::vector<TrackId>> duplicateGroups;
    size_t done = 0;
    size_t added = 0, failedInputs = 0;
    uint64_t target = 0;
};
std::thread g_playlistFileWorker;
std::thread g_steamOfferWorker;
bool g_playlistFileBusy = false, g_playlistFileStopping = false;
bool g_checkingDead = false, g_checkingDuplicates = false, g_checkingSteam = false;
std::atomic<bool> g_playlistFileCancel{false};
std::atomic<bool> g_steamOfferCancel{false};
std::unique_ptr<PlaylistFile> g_playlistImport;

// The library folders dialog.
struct Folders {
    bool open = false;
    Rect panel, add, music, done;
    std::vector<Rect> remove;
} g_folders;
struct ExcludeConfirm {
    bool open = false;
    std::vector<TrackId> ids;
    Rect panel, confirm, cancel;
} g_exclude;
bool g_aboutOpen = false;
// The Web server window (File > Web server): its controls are 0 the port, 1 Start/Stop, 2 Start with cordinal,
// 3 Pair device, 4 Remove all, 5 Close, 6 Open web player, 7 Copy phone address, 8 Copy code, 9 Listen away
// from home, 10 Check again, 11 Copy internet address, 12 the fixed code and 13 Use this code.
constexpr int kWebControls = 14;
struct WebPanel {
    bool open = false;
    std::string port, fixed;
    int focus = 1, scroll = 0;
    Rect panel, controls[kWebControls];
    std::vector<std::pair<Rect, std::string>> addresses, devices;
    std::string qrText;  // what `qr` encodes
    qr::Code qr;
} g_web;

// The seek bar's waveform (app_waveform.cpp): the playing track's peaks, computed in the background
// the first time it plays and cached; until they arrive the bar is the flat channel.
struct Wave {
    TrackId track = kNone;  // the track `peaks` belongs to (or is being computed for)
    bool ready = false;
    uint32_t request = 0;   // the background request `track` waits on (0 = none)
    Peaks peaks{};
} g_wave;

SDL_Texture* g_cover = nullptr;
TrackId g_coverFor = kNone, g_coverWanted = kNone;
uint32_t g_lastRebuild = 0;
size_t g_sourceCount = 0;  // active tab's tracks before the search
std::unordered_map<uint64_t, std::string> g_cells;  // formatted cells by track and column
uint32_t g_cellsRevision = 0;
AudioVisuals g_visuals;
uint64_t g_visualAt = 0;

// --- drawing ---------------------------------------------------------------------------------------
void Circle(float cx, float cy, float r, uint32_t rgb) {
    std::vector<SDL_FPoint> pts;
    for (int i = 0; i < 24; ++i) pts.push_back({cx + std::cos(i / 24.f * 6.2831853f) * r, cy + std::sin(i / 24.f * 6.2831853f) * r});
    Polygon(pts, rgb);
}
// The outline of `r` with corners of radius `rad`, clockwise.
std::vector<SDL_FPoint> RoundPath(const Rect& r, float rad) {
    std::vector<SDL_FPoint> pts;
    const float cx[] = {r.R() - rad, r.R() - rad, r.x + rad, r.x + rad}, cy[] = {r.y + rad, r.B() - rad, r.B() - rad, r.y + rad};
    for (int c = 0; c < 4; ++c)
        for (int i = 0; i <= 6; ++i) {
            float a = (c - 1 + i / 6.f) * 1.5707963f;
            pts.push_back({cx[c] + std::cos(a) * rad, cy[c] + std::sin(a) * rad});
        }
    return pts;
}
void FillRound(const Rect& r, float rad, uint32_t rgb, float alpha = 1) { Polygon(RoundPath(r, rad), rgb, alpha); }
void FrameRound(const Rect& r, float rad, uint32_t rgb) {
    auto pts = RoundPath({r.x + 0.5f, r.y + 0.5f, r.w - 1, r.h - 1}, rad);
    pts.push_back(pts.front());
    SetColor(rgb);
    SDL_RenderLines(g.renderer, pts.data(), (int)pts.size());
}
std::string Plain(const wchar_t* label) {  // a Windows menu label without its mnemonic marks
    std::wstring s;
    for (const wchar_t* p = label; *p; ++p)
        if (*p != L'&' || p[1] == L'&') s += *p == L'&' ? *++p : *p;
    return Utf8(s);
}

// The transport glyphs, in the thin line style of Segoe MDL2 Assets, `s` pixels tall at the center
// of `r`.
enum class Icon { Prev, Play, Pause, Next, Shuffle, Repeat, RepeatOne, Resume, Speaker };
void DrawIcon(Icon icon, const Rect& r, uint32_t rgb) {
    float s = S(12), cx = std::round(r.x + r.w / 2), cy = std::round(r.y + r.h / 2), t = std::max(1.0f, S(1));
    auto line = [&](float x1, float y1, float x2, float y2) { ThickLine(x1, y1, x2, y2, t, rgb); };
    auto triangle = [&](float x1, float x2, float h) {  // outline, pointing from x1 to x2
        line(x1, cy - h, x2, cy);
        line(x2, cy, x1, cy + h);
        line(x1, cy - h - t / 2, x1, cy + h + t / 2);
    };
    float h = s * 0.38f;
    switch (icon) {
        case Icon::Prev:
            line(cx - s * 0.36f, cy - h, cx - s * 0.36f, cy + h);
            triangle(cx + s * 0.34f, cx - s * 0.22f, h);
            break;
        case Icon::Next:
            line(cx + s * 0.36f, cy - h, cx + s * 0.36f, cy + h);
            triangle(cx - s * 0.34f, cx + s * 0.22f, h);
            break;
        case Icon::Play: triangle(cx - s * 0.3f, cx + s * 0.38f, s * 0.42f); break;
        case Icon::Pause:
            line(cx - s * 0.18f, cy - h, cx - s * 0.18f, cy + h);
            line(cx + s * 0.18f, cy - h, cx + s * 0.18f, cy + h);
            break;
        case Icon::Shuffle: {
            float x0 = cx - s * 0.46f, x1 = cx + s * 0.46f, y0 = cy - s * 0.26f, y1 = cy + s * 0.26f;
            line(x0, y0, cx - s * 0.12f, y0);
            line(cx - s * 0.12f, y0, cx + s * 0.16f, y1);
            line(cx + s * 0.16f, y1, x1, y1);
            line(x0, y1, cx - s * 0.12f, y1);
            line(cx - s * 0.12f, y1, cx + s * 0.16f, y0);
            line(cx + s * 0.16f, y0, x1, y0);
            for (float y : {y0, y1}) {
                line(x1 - s * 0.16f, y - s * 0.16f, x1, y);
                line(x1 - s * 0.16f, y + s * 0.16f, x1, y);
            }
            break;
        }
        case Icon::Repeat:
        case Icon::RepeatOne:
        case Icon::Resume: {
            // A circle of arrows: an open ring with a head at its gap. Resume (history) adds hands.
            float rr = s * 0.42f;
            std::vector<SDL_FPoint> arc;
            float from = icon == Icon::Resume ? -2.3f : -1.2f, to = icon == Icon::Resume ? 3.4f : 4.6f;
            for (int i = 0; i <= 20; ++i) {
                float a = from + (to - from) * i / 20;
                arc.push_back({cx + std::cos(a) * rr, cy + std::sin(a) * rr});
            }
            for (size_t i = 0; i + 1 < arc.size(); ++i) line(arc[i].x, arc[i].y, arc[i + 1].x, arc[i + 1].y);
            SDL_FPoint tip = icon == Icon::Resume ? arc.front() : arc.back();
            float dir = icon == Icon::Resume ? -1 : 1;
            line(tip.x, tip.y, tip.x - dir * s * 0.2f, tip.y - s * 0.02f);
            line(tip.x, tip.y, tip.x + s * 0.02f, tip.y - dir * s * 0.2f);
            if (icon == Icon::Resume) {
                line(cx, cy - rr * 0.6f, cx, cy);
                line(cx, cy, cx + rr * 0.45f, cy + rr * 0.3f);
            }
            if (icon == Icon::RepeatOne) DrawText(Font::Small, "1", cx - TextWidth(Font::Small, "1") / 2, cy - LineHeight(Font::Small) / 2, rgb);
            break;
        }
        case Icon::Speaker: {
            float sx = cx - s * 0.3f;
            Polygon({{sx - s * 0.2f, cy - s * 0.12f}, {sx, cy - s * 0.12f}, {sx + s * 0.22f, cy - s * 0.3f},
                     {sx + s * 0.22f, cy + s * 0.3f}, {sx, cy + s * 0.12f}, {sx - s * 0.2f, cy + s * 0.12f}}, rgb);
            for (float k : {0.38f, 0.58f}) {
                std::vector<SDL_FPoint> arc;
                for (int i = 0; i <= 8; ++i) {
                    float a = -0.8f + 1.6f * i / 8;
                    arc.push_back({sx + std::cos(a) * s * k, cy + std::sin(a) * s * k});
                }
                for (size_t i = 0; i + 1 < arc.size(); ++i) line(arc[i].x, arc[i].y, arc[i + 1].x, arc[i + 1].y);
            }
            break;
        }
    }
}

// --- the track list's content --------------------------------------------------------------------
Playlist& Active() { return g.playlists[g.active]; }

int NowPlayingIndex() {
    for (size_t i = 0; i < g.playlists.size(); ++i)
        if (g.playlists[i].kind == PlaylistKind::NowPlaying) return (int)i;
    return -1;
}

// The title-format fields, with the playing track's position and remaining time.
FieldLookup Lookup(TrackId id, bool withPlayback) {
    return [id, withPlayback](const std::wstring& name, std::wstring& out) {
        if (withPlayback && name == L"playback_time") {
            out = FormatDuration(g.engine.Position());
            return true;
        }
        if (withPlayback && name == L"playback_time_remaining") {
            out = FormatDuration(std::max(0.0, g.engine.Duration() - g.engine.Position()));
            return true;
        }
        return LookupField(id, name, out) || PackField(id, name, out);
    };
}

const std::string& Cell(TrackId id, int column) {
    if (g_cellsRevision != g_lib.Revision() || g_cells.size() > 20000) {
        g_cells.clear();
        g_cellsRevision = g_lib.Revision();
    }
    uint64_t key = (uint64_t)id * ui::kColumnCount + column;
    auto it = g_cells.find(key);
    if (it == g_cells.end()) it = g_cells.emplace(key, Utf8(FormatTitle(kColumns[column].display, Lookup(id, false)))).first;
    return it->second;
}

// Case-insensitive, with runs of digits compared as numbers ("Track 2" before "Track 10").
int CompareNatural(const std::wstring& a, const std::wstring& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (iswdigit(a[i]) && iswdigit(b[j])) {
            size_t i2 = i, j2 = j;
            while (i2 < a.size() && a[i2] == L'0') ++i2;
            while (j2 < b.size() && b[j2] == L'0') ++j2;
            size_t ie = i2, je = j2;
            while (ie < a.size() && iswdigit(a[ie])) ++ie;
            while (je < b.size() && iswdigit(b[je])) ++je;
            if (ie - i2 != je - j2) return ie - i2 < je - j2 ? -1 : 1;
            if (int c = a.compare(i2, ie - i2, b, j2, je - j2)) return c < 0 ? -1 : 1;
            i = ie;
            j = je;
            continue;
        }
        wchar_t x = (wchar_t)towlower(a[i++]), y = (wchar_t)towlower(b[j++]);
        if (x != y) return x < y ? -1 : 1;
    }
    return i < a.size() ? 1 : j < b.size() ? -1 : 0;
}

int CompareText(std::wstring_view a, std::wstring_view b) { return CompareNatural(std::wstring(a), std::wstring(b)); }

// The library's own order: album artist, album, disc, track, then file name.
bool LibraryLess(TrackId x, TrackId y) {
    const Track &a = g_lib[x], &b = g_lib[y];
    auto artist = [](const Track& t) { return g_lib.Str(t.albumArtist ? t.albumArtist : t.artist); };
    if (int r = CompareText(artist(a), artist(b))) return r < 0;
    if (int r = CompareText(g_lib.Str(a.album), g_lib.Str(b.album))) return r < 0;
    if (a.disc != b.disc) return a.disc < b.disc;
    if (a.trackNo != b.trackNo) return a.trackNo < b.trackNo;
    if (int r = CompareText(g_lib.Str(a.dir), g_lib.Str(b.dir))) return r < 0;
    return CompareText(g_lib.Str(a.file), g_lib.Str(b.file)) < 0;
}

std::vector<TrackId> PlaylistTracks(const Playlist& pl) {
    std::vector<TrackId> tracks;
    if (pl.kind == PlaylistKind::Library || pl.kind == PlaylistKind::Inbox) {
        for (TrackId id = 0; id < (TrackId)g_lib.Count(); ++id)
            if (pl.kind == PlaylistKind::Library ? g_lib[id].InLibrary() :
                (g_lib[id].flags & (Track::kInbox | Track::kMissing)) == Track::kInbox) tracks.push_back(id);
        if (pl.sortColumn < 0) std::stable_sort(tracks.begin(), tracks.end(), LibraryLess);
    } else if (pl.kind == PlaylistKind::Auto) {
        AutoPlaylist rules;
        if (ParseAutoPlaylist(pl.rules, rules)) EvaluateAutoPlaylist(rules, tracks);
    } else {
        tracks = pl.items;
    }
    return tracks;
}

void SortPlaylistTracks(std::vector<TrackId>& tracks, const Playlist& pl) {
    if (pl.sortColumn <= 0 || pl.sortColumn >= ui::kColumnCount) return;
    std::unordered_map<TrackId, std::wstring> keys;
    for (TrackId id : tracks) {
        if (!g_lib.Contains(id)) return;
        keys.emplace(id, FormatTitle(kColumns[pl.sortColumn].sort, Lookup(id, false)));
    }
    std::stable_sort(tracks.begin(), tracks.end(), [&](TrackId a, TrackId b) {
        int r = CompareNatural(keys[a], keys[b]);
        return pl.sortAscending ? r < 0 : r > 0;
    });
}

bool SortKeepsAlbums(int column) {  // #, Artist, Album, Year, Album artist, Disc keep an album together
    return column <= 0 || column == 1 || column == 3 || column == 4 || column == 5 || column == 10 || column == 13;
}

bool SameAlbum(const Track& a, const Track& b) {
    if (a.album != b.album) return false;
    // With album artists, they decide; without, the folder does (compilations keep together).
    if (a.albumArtist || b.albumArtist) return a.albumArtist == b.albumArtist;
    return a.dir == b.dir;
}

// The album groups and display lines for g.rows.
void BuildLines() {
    g_groups.clear();
    g_lines.clear();
    g_lineOfRow.assign(g.rows.size(), 0);
    bool grouping = g.settings.groupAlbums && !g.playlists.empty() && SortKeepsAlbums(Active().sortColumn);
    int n = (int)g.rows.size();
    for (int p = 0; p < n;) {
        const Track& first = g_lib[g.rows[p]];
        int q = p + 1;
        if (grouping && first.album)
            while (q < n && SameAlbum(first, g_lib[g.rows[q]])) ++q;
        if (q - p >= 2) {
            Group gr;
            gr.first = p;
            gr.count = q - p;
            gr.album = first.album;
            gr.artist = first.albumArtist;
            gr.date = first.date;
            bool oneArtist = true;
            for (int k = p; k < q; ++k) {
                const Track& t = g_lib[g.rows[k]];
                gr.seconds += t.Duration();
                oneArtist &= t.artist == first.artist;
                if (!gr.date) gr.date = t.date;
            }
            if (!gr.artist) gr.artist = oneArtist ? first.artist : 0;
            int k = (int)g_groups.size();
            g_groups.push_back(gr);
            g_lines.push_back({-1, k, 0});
            g_lines.push_back({-1, k, 1});
        }
        for (int k = p; k < q; ++k) {
            g_lineOfRow[k] = (int)g_lines.size();
            g_lines.push_back({k, q - p >= 2 ? (int)g_groups.size() - 1 : -1, 0});
        }
        p = q;
    }
}

// What a grouped track's cell leaves to its header: the album, and the artist and year that match.
bool GroupHidesCell(const Line& line, int column) {
    if (line.group < 0) return false;
    const Group& gr = g_groups[line.group];
    const Track& t = g_lib[g.rows[line.row]];
    switch (column) {
        case 4: case 10: return true;
        case 3: return gr.artist && t.artist == gr.artist;
        case 5: return t.date == gr.date;
        default: return false;
    }
}

float ColumnWidth(int column) {
    int w = g.settings.widths[column];
    return S((float)(w > 0 ? w : kColumns[column].width));
}

// --- layout ------------------------------------------------------------------------------------------
std::string TabName(size_t i) {
    return (g.playlists[i].kind == PlaylistKind::Auto ? "\xE2\x9A\xA1 " : "") + Utf8(g.playlists[i].name);
}

std::vector<int> BarMenus();

// The normal window as app_custom.cpp (LayoutBand, PlaceBandRow, PlaceNowPlaying) and app_layout.cpp
// (LayoutLibrary) place it, from the shared measurements, under the menu bar.
void DoLayout() {
    int pw = 0, ph = 0;
    SDL_GetRenderOutputSize(g.renderer, &pw, &ph);
    float W = (float)pw, H = (float)ph, pad = S(ui::kPad), tb = S(ui::kButton);
    L.menuBar = {0, 0, W, S(ui::kMenuBarHeight)};
    L.barItems.clear();
    float mx = S(1);
    for (int m : BarMenus()) {
        float w = TextWidth(Font::Menu, Plain(ui::kMenuBar[m])) + S(14);
        L.barItems.push_back({mx, 0, w, S(ui::kMenuBarHeight - 2)});
        mx += w;
    }
    float top = L.menuBar.B();
    // The band: the handle, the buttons, the times around the seek bar, the volume.
    const unsigned hide = g.settings.hiddenParts;  // View > Customize layout: a hidden part's rectangle is empty
    float waveH = hide & kPartWave ? tb : std::max(tb, S(ui::kBandWaveHeights[std::clamp(g.settings.bandWaveSize, 0, 5)]));
    float y = top + S(ui::kBandTop), by = y + (waveH - tb) / 2;
    float ty = by + (tb - S(ui::kTimeHeight)) / 2 + S(1);
    L.grip = {0, y, S(ui::kGrip), waveH};
    float x = S(ui::kGrip);
    for (int i = 0; i < 6; ++i) {
        const bool hidden = hide & (i < 3 ? kPartTransport : kPartOrder);
        if (i == 3 && !(hide & kPartTransport) && !hidden) x += S(ui::kOrderGap);
        L.buttons[i] = hidden ? Rect{} : Rect{x, by, tb, tb};
        if (!hidden) x += tb;
    }
    x += S(ui::kButtonsGap);
    float right = W - pad, timeW = S(ui::kTimeWidth);
    if (hide & kPartVolume) {
        L.volume = L.speaker = {};
    } else {
        L.volume = {right - S(ui::kVolumeWidth), by, S(ui::kVolumeWidth), tb};
        L.speaker = {L.volume.x - S(ui::kSpeakerWidth), by + S(4), S(ui::kSpeakerWidth), S(ui::kTimeHeight)};
        right = L.volume.x - S(ui::kVolumeGap);
    }
    if (hide & kPartTimes) {
        L.elapsed = L.remaining = {};
    } else {
        L.elapsed = {x, ty, timeW, S(ui::kTimeHeight)};
        L.remaining = {right - timeW, ty, timeW, S(ui::kTimeHeight)};
        x += timeW + S(ui::kTimeGap);
        right -= timeW + S(ui::kTimeGap);
    }
    L.seek = (hide & kPartWave) ? Rect{} : Rect{x, y, std::max(0.0f, right - x), waveH};
    const bool band = (hide & (kPartTransport | kPartOrder | kPartWave | kPartTimes | kPartVolume)) !=
                      (kPartTransport | kPartOrder | kPartWave | kPartTimes | kPartVolume);
    y += band ? waveH + S(ui::kBandGap) : 0;
    if (!band) L.grip = {};
    // The now playing line.
    const bool nowPlaying = !(hide & kPartNowPlaying);
    L.cover = nowPlaying ? Rect{pad, y, S(ui::kCover), S(ui::kCover)} : Rect{};
    float x0 = L.cover.R() + S(ui::kCoverGap);
    L.title = nowPlaying ? Rect{x0, y, W - x0 - pad, S(ui::kLineHeight)} : Rect{};
    L.subtitle = nowPlaying ? Rect{x0, y + S(ui::kLineHeight), W - x0 - pad, S(ui::kLineHeight)} : Rect{};
    L.chip = {};
    if (g.settings.pointsChip && nowPlaying) {  // the level and points chip, at the right end of the now-playing lines
        const float chipW = S(220);
        L.chip = {W - pad - chipW, L.cover.y + (L.cover.h - S(32)) / 2, chipW, S(32)};
        L.title.w -= chipW + S(8);
        L.subtitle.w -= chipW + S(8);
    }
    if (nowPlaying) y += S(ui::kCover) + S(ui::kBandGap);
    L.visuals[0] = L.visuals[1] = {};
    if (g.settings.theme == ThemeMode::Foobar) {
        L.visuals[0] = {pad, y, S(ui::kFoobarSpectrum), S(ui::kFoobarButton)};
        L.visuals[1] = {L.visuals[0].R() + S(10), y, std::max(0.0f, W - L.visuals[0].R() - S(10) - pad), S(ui::kFoobarButton)};
        y += S(ui::kFoobarButton) + S(ui::kBandGap);
    }
    // The Library: tabs and search, the view row, the list, the status bar.
    float searchW = std::min(S(ui::kSearchWidth), std::floor(W / 3));
    const bool searchShown = !(hide & kPartSearch), tabsShown = !(hide & kPartTabs);
    L.search = searchShown ? Rect{W - pad - searchW, y + S(1), searchW, S(ui::kTabRow) - S(2)} : Rect{};
    float tabWidth = searchShown ? W - (pad + searchW + S(ui::kSearchGap)) : W;
    float radioWidth = tabsShown && InternetRadio().tabOpen ? std::min(S(ui::kRadioTabWidth), std::max(0.0f, tabWidth)) : 0;
    L.tabs = {0, y, tabWidth - radioWidth, searchShown || tabsShown ? S(ui::kTabRow) : 0};
    L.radioTab = {L.tabs.R(), y, radioWidth, L.tabs.h};
    L.tabRects.clear();
    float tx = S(2);
    for (size_t i = 0; tabsShown && i < g.playlists.size(); ++i) {
        float w = TextWidth(Font::Normal, (int)i == g_renameIndex ? Utf8(g_renameText) : TabName(i)) + S(36);
        L.tabRects.push_back({tx, L.tabs.y, w, L.tabs.h});
        tx += w;
    }
    y += L.tabs.h;
    const float lw = LyricsWidth(), LW = W - lw;  // the lyrics panel takes the right edge
    bool library = !InternetRadio().active && !g.playlists.empty() && Active().kind == PlaylistKind::Library && !(hide & kPartViews);
    L.viewRow = {0, y, LW, library ? S(ui::kViewRow) : 0};
    // The views as one segmented control on the left, the browser toggle on the right (app_layout.cpp).
    for (int i = 0, vx = (int)pad; i < kViewButtons; ++i) {
        float bw = TextWidth(Font::Normal, ViewButtonLabel(i)) + S(16);
        L.views[i] = {i == kViewButtons - 1 ? LW - S(8) - bw : (float)vx, y + S(4), bw, S(ui::kViewButton)};
        vx += (int)(bw + S(4));
    }
    y += L.viewRow.h;
    L.status = (hide & kPartStatus) ? Rect{0, H, W, 0} : Rect{0, H - S(ui::kStatus), W, S(ui::kStatus)};
    L.browser = L.splitter = {};
    if (!InternetRadio().active && BrowserShown()) {
        float h = std::clamp(BrowserHeight(), S(60), std::max(S(60), L.status.y - y - S(120)));
        L.browser = {0, y, LW, h};
        L.splitter = {0, y + h, LW, S(5)};
        y += h + S(5);
    }
    L.list = {0, y, LW, std::max(0.0f, L.status.y - y)};
    float gap = S(6), field = std::max(S(30), std::min(S(150), (LW - S(70) - 5 * gap) / 3));
    L.radioSaved = {gap, y + S(3), S(100), S(25)};
    L.radioDiscover = {S(110), y + S(3), S(90), S(25)};
    L.radioGenre = {gap, y + S(34), field, S(24)};
    L.radioCountry = {gap * 2 + field, y + S(34), field, S(24)};
    L.radioState = {gap * 3 + field * 2, y + S(34), field, S(24)};
    L.radioSearch = {gap * 4 + field * 3, y + S(34), S(70), S(24)};
    L.radioGrid = {0, y + S(68), LW, std::max(0.0f, L.status.y - y - S(68))};
    L.header = {0, y, LW, S(24)};
    L.rowH = S(ui::kRow);
    float sbW = S(17);
    L.rows = {0, L.header.B(), LW, std::max(0.0f, L.status.y - L.header.B())};
    bool scroll = g_lines.size() * L.rowH > L.rows.h;
    if (scroll) L.rows.w -= sbW;
    L.scrollbar = {LW - sbW, L.rows.y, scroll ? sbW : 0, L.rows.h};
    L.lyrics = lw > 0 ? Rect{LW, L.viewRow.y, lw, L.status.y - L.viewRow.y} : Rect{};
}

float ContentHeight() { return g_lines.size() * L.rowH; }
float MaxScroll() { return std::max(0.0f, ContentHeight() - L.rows.h); }
void ClampScroll() { g.scroll = std::clamp(g.scroll, 0.0f, MaxScroll()); }

void EnsureVisible(int row) {
    if (row < 0 || row >= (int)g_lineOfRow.size()) return;
    int line = g_lineOfRow[row];
    // Show an album's header with its first track.
    if (line >= 2 && g_lines[line - 1].row < 0 && g_lines[line - 2].row < 0) line -= 2;
    float top = line * L.rowH, bottom = (g_lineOfRow[row] + 1) * L.rowH;
    if (top < g.scroll) g.scroll = top;
    else if (bottom > g.scroll + L.rows.h) g.scroll = bottom - L.rows.h;
    ClampScroll();
}

int LineAt(float y) {
    if (y < L.rows.y || y >= L.rows.B()) return -1;
    int line = (int)((y - L.rows.y + g.scroll) / L.rowH);
    return line >= 0 && line < (int)g_lines.size() ? line : -1;
}

// --- menus ---------------------------------------------------------------------------------------------
std::vector<TrackId> SelectedTracks() {
    std::vector<TrackId> ids;
    for (size_t i = 0; i < g.rows.size(); ++i)
        if (g.selected[i]) ids.push_back(g.rows[i]);
    return ids;
}

void AcceptInbox(const std::vector<TrackId>& ids) {
    std::vector<TrackId> accepted;
    for (TrackId id : ids) {
        if (!g_lib.Contains(id) || !(g_lib[id].flags & Track::kInbox)) continue;
        g_lib[id].flags &= ~Track::kInbox;
        accepted.push_back(id);
    }
    if (accepted.empty()) return;
    AnnouncePoints(AwardLibraryAdds(accepted));
    g_lib.Touch();
    g.libraryDirty = true;
    UpdateInboxTab();
    RebuildRows();
    ShowMessage(L"Added " + std::to_wstring(accepted.size()) +
                (accepted.size() == 1 ? L" track" : L" tracks") + L" to the library.");
}

void SetRating(const std::vector<TrackId>& ids, uint8_t rating) {
    if (ids.empty()) return;
    for (TrackId id : ids) if (g_lib.Contains(id)) g_lib[id].rating = rating;
    PlaySfx(rating ? Sfx::Rate : Sfx::Unrate, rating / 10);
    g_lib.Touch();
    g.libraryDirty = true;
    RebuildRows();
}

void ExcludeFromLibrary(const std::vector<TrackId>& ids) {
    if (ids.empty()) return;
    g_exclude.ids = ids;
    g_exclude.open = true;
    g.redraw = true;
}

void ConfirmExclude(bool confirm) {
    g_exclude.open = false;
    auto ids = std::move(g_exclude.ids);
    if (!confirm) return;
    for (TrackId id : ids) if (g_lib.Contains(id) && (g_lib[id].flags & (Track::kInLibrary | Track::kInbox)))
        g_lib[id].flags = (uint16_t)((g_lib[id].flags | Track::kExcluded) & ~(Track::kInLibrary | Track::kInbox));
    g_lib.Touch();
    g.libraryDirty = true;
    UpdateInboxTab();
    RebuildRows();
    ShowMessage(L"Removed from the library. The files were kept.");
}

// Playback > ReplayGain, as on Windows: on or off, and what to do to tracks. How it sounds (the
// preamp, how much loud songs keep) is set in Preferences.
std::vector<MenuItem> ReplayGainMenu() {
    bool manual = g.rgRunning && !g.rgAuto, any = !g.rows.empty();
    std::vector<MenuItem> items{Item("Enabled", kRgEnable, {}, g.settings.rg.on), Separator(),
                                Item("Scan selection", kRgScanSel, {}, false, any && !manual),
                                Item("Scan playlist", kRgScanPl, {}, false, any && !manual)};
    if (manual) items.push_back(Item("Cancel scan", kRgCancel));  // shown only while one runs
    items.push_back(Separator());
    items.push_back(Item("Clear ReplayGain", kRgClear, {}, false, any && !manual));
    items.push_back(Item("Write ReplayGain tags", kRgWrite, {}, false, any && !manual));
    return items;
}

std::vector<MenuItem> PlaylistDestinations() {
    std::vector<MenuItem> items;
    bool selected = std::any_of(g.selected.begin(), g.selected.end(), [](char value) { return value != 0; });
    for (size_t i = 0; i < g.playlists.size(); ++i)
        if (g.playlists[i].kind == PlaylistKind::Static) {
            MenuItem item = Item(Utf8(g.playlists[i].name), kAddToPlaylist, {}, false, selected);
            item.arg = (int)i;
            items.push_back(std::move(item));
        }
    return items;
}

// The menu bar's menus, with the Windows build's wording, order and shortcuts. `m` indexes ui::kMenuBar.
std::vector<MenuItem> BarMenu(int m) {
    bool editable = !g.playlists.empty() && Active().Editable(), any = !g.rows.empty();
    switch (m) {
        case 0:
            return {Item("Add files...", kAddFiles, "Ctrl+O"), Item("Import folder once...", kAddFolder, "Ctrl+Shift+O"), Separator(),
                    Item("Open playlist file...", kOpenPlaylistFile), Item("Save playlist as...", kSavePlaylistFile, "Ctrl+S"), Separator(),
                    Item("Back up library and settings...",kBackup),Item("Restore backup...",kRestore),
                    Item("Import playback statistics...",kStatistics),Separator(),
                    Item("Web server...", kWebServer), Item("Preferences...", kPreferences, "Ctrl+,"), Separator(), Item("Exit", kExit)};
        case 1: {
            bool library = Active().kind == PlaylistKind::Library || Active().kind == PlaylistKind::Inbox;
            std::vector<MenuItem> items{Item("Select all", kSelectAll, "Ctrl+A", false, any), Item("Remove", kRemove, "Del", false, (editable || library) && any),
                    Item("Keep only selected tracks", kKeepSelected, {}, false, editable && !SelectedTracks().empty()),
                    Item("Clear playlist", kClear, {}, false, editable && any), Separator(),
                    Item("Reload selected metadata", kReloadTags, {}, false, !SelectedTracks().empty() && !TagWriteBusy()),
                    Item("Remove duplicates", kRemoveDuplicates, {}, false, editable && any),
                    Item("Remove dead items", kRemoveDead, {}, false, editable && any && !g_playlistFileBusy && !g_playlistImport),
                    Separator(), Item("Search", kFind, "Ctrl+F"), Separator(),
                    Item("Edit tags...", kEditTags, "Alt+Enter", false, g.focus >= 0 && any),
                    Item("Undo last tag edit", kUndoTags, "Ctrl+Z", false, CanUndoTagEdit())};
            if (g_checkingDead && g_playlistFileBusy) items.insert(items.begin() + 8, Item("Cancel checking dead items", kCancelDead));
            if (auto labels = HelperToolLabels(); !labels.empty()) {  // the tools of the helpers that are on
                std::vector<MenuItem> run;
                for (size_t k = 0; k < labels.size(); ++k) {
                    MenuItem tool = Item(ToUtf8(labels[k]), kHelperTool, {}, false, RunningHelperTool() < 0);
                    tool.arg = (int)k;
                    run.push_back(std::move(tool));
                }
                if (int running = RunningHelperTool(); running >= 0) run.push_back(Item("Cancel " + ToUtf8(labels[running]), kHelperCancel));
                items.push_back(Separator());
                items.push_back(Submenu("Run", std::move(run)));
            }
            return items;
        }
        case 2: {
            Repeat r = g.settings.repeat;
            return {Item("Play / Pause", kPlayPause, "Space"), Item("Stop", kStopAction, {}, false, g.playing != kNone),
                    Item("Mute", kMute, "Ctrl+M", g_muteVolume > 0), Item("Previous", kPrevAction, "Ctrl+Left"),
                    Item("Next", kNextAction, "Ctrl+Right"), Separator(),
                    Item("Shuffle", kShuffleAction, "Ctrl+H", g.settings.shuffle == kShuffleTracks),
                    Item("Shuffle albums", kShuffleAlbumsAction, {}, g.settings.shuffle == kShuffleAlbums),
                    Item("Keep playing like this", kRadio, {}, g.settings.radio),
                    Radio("Repeat off", kRepeatSet, (int)Repeat::Off, r == Repeat::Off),
                    Radio("Repeat playlist", kRepeatSet, (int)Repeat::All, r == Repeat::All, "Ctrl+R"),
                    Radio("Repeat track", kRepeatSet, (int)Repeat::One, r == Repeat::One),
                    Item("Stop after current", kStopAfter, {}, g.stopAfterCurrent),
                    Item("Clear the queue", kClearQueue, {}, false, !g.queue.empty()), Separator(),
                    Submenu("ReplayGain", ReplayGainMenu()),
                    Submenu("Speed (pitch follows)", RateItems(kSpeeds, std::size(kSpeeds), g.speed, kSpeed)),
                    Submenu("Tempo (pitch stays)", RateItems(kTempos, std::size(kTempos), g.tempo, kTempo)),
                    Separator(), Item("Equalizer...", kEqualizer, "Ctrl+E"),
                    Submenu("Output", {Radio("Shared (through PipeWire)", kShared, 0, !g.settings.exclusive),
                                       Radio("Exclusive (bit-perfect)", kExclusive, 0, g.settings.exclusive)})};
        }
        case 3: {
            auto destinations = PlaylistDestinations();
            std::vector<MenuItem> items{Item("New playlist", kNewPl, "Ctrl+N"),
                    Item("Rename playlist", kRenamePl, "F2", false, Active().kind != PlaylistKind::Library && Active().kind != PlaylistKind::Inbox),
                    Item("Close playlist", kClosePl, "Ctrl+W", false,
                         InternetRadio().active || Active().kind == PlaylistKind::Static || Active().kind == PlaylistKind::Auto ||
                             (Active().kind == PlaylistKind::NowPlaying && g.playing == kNone)),
                    Item("Duplicate playlist", kDuplicatePl, "Ctrl+Shift+D")};
            if (!destinations.empty()) items.push_back(Submenu("Add selection to playlist", std::move(destinations)));
            if (auto packLists = PackAutoPlaylistNames(); !packLists.empty()) {  // the auto-playlists the packs that are on hold
                std::vector<MenuItem> sub;
                for (size_t k = 0; k < packLists.size(); ++k) {
                    MenuItem list = Item(ToUtf8(packLists[k]), kPackPlaylist);
                    list.arg = (int)k;
                    sub.push_back(std::move(list));
                }
                items.push_back(Submenu("Add auto-playlist from a pack", std::move(sub)));
            }
            items.insert(items.end(), {
                    Item("Reopen closed playlist", kReopenPl, "Ctrl+Shift+T", false, !g_closedPlaylists.empty()),
                    Item("Move left", kMovePlLeft, "Ctrl+Shift+PageUp", false, g.active > 1),
                    Item("Move right", kMovePlRight, "Ctrl+Shift+PageDown", false, g.active > 0 && g.active + 1 < (int)g.playlists.size()),
                    Item("Reveal playing track", kReveal, "Ctrl+J", false, g.playing != kNone), Separator(),
                    Item("Next playlist", kNextPl, "Ctrl+Tab"), Item("Previous playlist", kPrevPl, "Ctrl+Shift+Tab"),
                    Separator(), Item("New auto-playlist...", kNewAuto),
                    Item("Edit auto-playlist rules...", kEditAuto, {}, false, Active().kind == PlaylistKind::Auto)});
            return items;
        }
        case 4: {
            std::vector<MenuItem> items{Item("Library folders...", kFolders),
                Item("Rescan folders", kRescan, {}, false, !g.settings.folders.empty()), Separator(),
                Item("Add everything in the Inbox", kAcceptAllInbox, {}, false,
                     std::any_of(g.playlists.begin(), g.playlists.end(), [](const Playlist& p) { return p.kind == PlaylistKind::Inbox; })), Separator(),
                Item("Organize files...", kOrganize, {}, false, !OrganizeBusy()),
                Item("Undo last organize", kOrganizeUndo, {}, false, !OrganizeBusy()), Separator(),
                Item("Convert to .cord...", kConvert, {}, false, !ConvertBusy() && !CordExportBusy() && !TagWriteBusy() && !OrganizeBusy()),
                Submenu("Export .cord", CordExportItems()),
                Item("Undo last conversion", kConvertUndo, {}, false, CanUndoConversion() && !ConvertBusy()), Separator(),
                Item("Copy to folder or device...", kCopyTo, {}, false, !CopyBusy()), Separator(),
                Item("Find duplicate audio", kDuplicateAudio, {}, false, !g_playlistFileBusy && !g_playlistImport),
                Item("Add Steam soundtracks", kSteam, {}, false, !g_playlistFileBusy && !g_playlistImport)};
            if (ConvertBusy()) items.push_back(Item("Cancel conversion", kCancelConvert));
            if (CopyBusy()) items.push_back(Item("Cancel copy", kCancelCopy));
            if (CordExportBusy()) items.push_back(Item("Cancel export", kCancelExport));
            if (g_checkingDuplicates && g_playlistFileBusy) items.push_back(Item("Cancel duplicate search", kCancelDuplicates));
            if (g_checkingSteam && g_playlistFileBusy) items.push_back(Item("Cancel Steam search", kCancelSteam));
            return items;
        }
        case 5: {
            // As View > Theme on Windows: Light and Dark, then following the system, then the themed palettes.
            std::vector<MenuItem> themes;
            for (ThemeMode t : {ThemeMode::Light, ThemeMode::Dark, ThemeMode::System})
                themes.push_back(Radio(t == ThemeMode::System ? "Follow the system" : Plain(ThemeInfo(t).name), kTheme, (int)t,
                                       g.settings.theme == t));
            if (kExtraThemes || ThemeTotal() > (int)ThemeMode::Count)
                themes.push_back(Separator());
            for (int t = (int)ThemeMode::Deltarune; t < ThemeTotal(); ++t)
                if (ThemeAvailable((ThemeMode)t)) themes.push_back(Radio(Plain(ThemeInfo((ThemeMode)t).name), kTheme, t, (int)g.settings.theme == t));
            return {Submenu("Theme", std::move(themes)), Item("Radio", kOpenRadio, "Ctrl+Shift+R"), Item("Lyrics panel", kLyrics, "Ctrl+L", LyricsShown()), Item("Customize layout...", kLayout), Item("Mini player", kMini, "Ctrl+Shift+M"), Item("Packs...", kPacks), Item("Helpers...", kHelpers), Item("Command palette...", kPalette, "Ctrl+Shift+P"), Separator(), Item("Group tracks by album", kGroupAlbums, {}, g.settings.groupAlbums),
                    Separator(), Item("Start on login", kStartOnLogin, {}, StartsOnLogin()), Separator(),
                    Submenu("Games and stats", {Item("Name That Track...", kNameThatTrack), Item("Which Came First...", kWhichCameFirst), Separator(),
                                                Item("cordinal points...", kPoints), Item("Year in review...", kReview)})};
        }
        case 6: return {Item("About cordinal", kAbout)};
        default: return {};
    }
}

// The menus the bar shows, left to right: all of them.
std::vector<int> BarMenus() {
    std::vector<int> menus;
    for (int m = 0; m < (int)std::size(ui::kMenuBar); ++m) menus.push_back(m);
    return menus;
}

constexpr float kItemH = ui::kRow + 3, kSeparatorH = 8, kMenuPad = 4, kLabelX = 37, kShortcutRight = 25;

float PopupItemHeight(const MenuItem& it) { return S(it.separator ? kSeparatorH : kItemH); }

void OpenPopup(float x, float y, std::vector<MenuItem> items, bool submenu = false) {
    if (!submenu) g_popups.clear();
    Popup p;
    float label = 0, shortcut = 0;
    bool arrow = false;
    for (auto& it : items) {
        label = std::max(label, TextWidth(Font::Menu, it.label));
        shortcut = std::max(shortcut, TextWidth(Font::Menu, it.shortcut));
        arrow |= !it.sub.empty();
        p.h += PopupItemHeight(it);
    }
    p.w = S(kLabelX) + label + (shortcut > 0 ? S(36) + shortcut : 0) + S(kShortcutRight) + (arrow ? S(12) : 0);
    p.w = std::max(p.w, S(130));
    p.h += 2 * S(kMenuPad);
    int pw = 0, ph = 0;
    SDL_GetRenderOutputSize(g.renderer, &pw, &ph);
    p.x = std::max(0.0f, std::min(x, pw - p.w - S(2)));
    p.y = std::max(0.0f, std::min(y, ph - p.h - S(2)));
    p.items = std::move(items);
    g_popups.push_back(std::move(p));
}

Rect PopupItemRect(const Popup& p, size_t i) {
    float y = p.y + S(kMenuPad);
    for (size_t k = 0; k < i; ++k) y += PopupItemHeight(p.items[k]);
    return {p.x, y, p.w, PopupItemHeight(p.items[i])};
}

void CloseMenus() {
    g_popups.clear();
    g_barOpen = -1;
    g.redraw = true;
}

void OpenBarMenu(int index) {  // index into L.barItems
    auto menus = BarMenus();
    if (index < 0 || index >= (int)menus.size()) return;
    g_barOpen = index;
    const Rect& r = L.barItems[index];
    OpenPopup(r.x, L.menuBar.B() - S(1), BarMenu(menus[index]));
    g.redraw = true;
}

void OpenRowMenu(float x, float y) {
    bool np = Active().kind == PlaylistKind::NowPlaying;
    std::vector<MenuItem> items{Item("Play", kPlay), Item("Play next", kPlayNext, "Ctrl+Enter"), Item("Add to queue", kAddToQueue, "Q"),
                                 Item("Add to Now Playing", kAddToNowPlaying, "Shift+Q")};
    if (np) items.push_back(Item("Remove from Now Playing", kRemoveFromNowPlaying, "Del"));
    if (Active().kind == PlaylistKind::Inbox) items.push_back(Item("Add to library", kAcceptInbox));
    if (Active().kind == PlaylistKind::Inbox || Active().kind == PlaylistKind::Library)
        items.push_back(Item("Remove from library...", kExcludeLibrary, "Del"));
    auto destinations = PlaylistDestinations();
    if (!destinations.empty()) items.push_back(Submenu("Add to playlist", std::move(destinations)));
    items.push_back(Separator());
    std::vector<MenuItem> ratings;
    MenuItem none = Item("No rating", kRating); none.arg = 0; ratings.push_back(std::move(none));
    for (int stars = 1; stars <= 5; ++stars) {
        MenuItem item = Item(std::to_string(stars) + (stars == 1 ? " star" : " stars"), kRating);
        item.arg = stars * 20;
        ratings.push_back(std::move(item));
    }
    ratings.push_back(Separator());
    ratings.push_back(Item("Write ratings and play counts to files", kWriteStatistics, {}, false, !TagWriteBusy()));
    items.push_back(Submenu("Rating", std::move(ratings)));
    bool manual = g.rgRunning && !g.rgAuto;
    items.push_back(Item("Scan ReplayGain", kRgScanSel, {}, false, !manual));
    items.push_back(Item("Write ReplayGain tags", kRgWrite, {}, false, !manual));
    items.push_back(Item("Clear ReplayGain", kRgClear, {}, false, !manual));
    items.push_back(Item("Open containing folder", kOpenFolder));
    items.push_back(Item("Organize files...", kOrganize, {}, false, !OrganizeBusy()));
    items.push_back(Item("Convert to .cord...", kConvert, {}, false, !ConvertBusy() && !CordExportBusy() && !TagWriteBusy()));
    items.push_back(Submenu("Export .cord", CordExportItems()));
    items.push_back(Item("Copy to folder or device...", kCopyTo, {}, false, !CopyBusy()));
    items.push_back(Item("Edit tags...", kEditTags, "Alt+Enter"));
    g_barOpen = -1;
    OpenPopup(x, y, std::move(items));
}

void OpenHeaderMenu(float x, float y) {
    std::vector<MenuItem> items;
    for (int c = 1; c < ui::kColumnCount && c < ui::kColCover; ++c) {
        bool shown = std::find(g.settings.columns.begin(), g.settings.columns.end(), c) != g.settings.columns.end();
        MenuItem it = Item(Utf8(kColumns[c].name), kColumnToggle, {}, shown, c != 2);
        it.arg = c;
        items.push_back(std::move(it));
    }
    g_barOpen = -1;
    OpenPopup(x, y, std::move(items));
}

void ShowFolder(TrackId id) {
    std::wstring path = g_lib.Path(id), file;
    int index = 0;
    if (SplitSubtrack(path, file, index)) path = file;
    std::string url = "file://" + Utf8(DirectoryOf(path));
    SDL_OpenURL(url.c_str());
}

void InsertIntoNowPlaying(const std::vector<TrackId>& ids, bool next) {
    if (ids.empty()) return;
    int np = EnsureNowPlayingTab();
    auto& items = g.playlists[np].items;
    size_t at = next && g.queuePos >= 0 && g.queuePos < (int)items.size() ? (size_t)g.queuePos + 1 : items.size();
    std::vector<size_t> mapping(items.size());
    for (size_t i = 0; i < mapping.size(); ++i) mapping[i] = i < at ? i : i + ids.size();
    items.insert(items.begin() + (ptrdiff_t)at, ids.begin(), ids.end());
    RemapNowPlaying(mapping);
    g.libraryDirty = true;
    if (g.active == np) RebuildRows();
    ShowMessage(std::to_wstring(ids.size()) + (ids.size() == 1 ? L" track" : L" tracks") + (next ? L" will play next." : L" added to Now Playing."));
}

void AddToPlaylist(int index, const std::vector<TrackId>& ids) {
    if (index < 0 || index >= (int)g.playlists.size() || g.playlists[index].kind != PlaylistKind::Static || ids.empty()) return;
    auto& items = g.playlists[index].items;
    items.insert(items.end(), ids.begin(), ids.end());
    g.libraryDirty = true;
    if (index == g.active) RebuildRows();
    ShowMessage(std::to_wstring(ids.size()) + (ids.size() == 1 ? L" track added to " : L" tracks added to ") + g.playlists[index].name + L".");
}

}  // namespace

// Files dropped on the window, once expanded: into the active playlist, or Now Playing from another tab.
void AddDropped(const std::vector<std::wstring>& files) {
    std::vector<TrackId> ids;
    for (auto& f : files) ids.push_back(g_lib.Add(f));
    std::vector<TrackId> unread;
    for (TrackId id : ids)
        if (!g_lib[id].Loaded()) unread.push_back(id);
    QueueTags(unread);
    if (Active().kind == PlaylistKind::Static) AddToPlaylist(g.active, ids);
    else InsertIntoNowPlaying(ids, false);
}

void OpenChoice(float x, float y, const std::vector<std::string>& labels, int selected, std::function<void(int)> pick) {
    std::vector<MenuItem> items;
    for (size_t i = 0; i < labels.size(); ++i) items.push_back(Radio(labels[i], kChoice, (int)i, (int)i == selected));
    g_choicePick = std::move(pick);
    OpenPopup(x, y, std::move(items));
    g.redraw = true;
}

namespace {

size_t RemoveOccurrences(const std::vector<char>& remove, int index = -1) {
    if (index < 0) index = g.active;
    if (index < 0 || index >= (int)g.playlists.size() || !g.playlists[index].Editable()) return 0;
    auto& playlist = g.playlists[index];
    auto& items = playlist.items;
    if (remove.size() != items.size()) return 0;
    std::vector<size_t> mapping(items.size(), SIZE_MAX);
    std::vector<TrackId> kept;
    kept.reserve(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        if (remove[i]) continue;
        mapping[i] = kept.size();
        kept.push_back(items[i]);
    }
    size_t removed = items.size() - kept.size();
    if (!removed) return 0;
    items.swap(kept);
    if (playlist.kind == PlaylistKind::NowPlaying) RemapNowPlaying(mapping);
    g.libraryDirty = true;
    if (index == g.active) {
        g.rows.clear(); g.rowItems.clear(); g.selected.clear(); g.focus = g.anchor = -1;
        RebuildRows();
    }
    return removed;
}

void RemoveFromPlaylist(bool all, bool keepSelected = false) {
    if (!Active().Editable()) return;
    std::vector<char> remove(Active().items.size(), (all || keepSelected) ? 1 : 0);
    bool anySelected = false;
    for (size_t i = 0; i < g.selected.size() && i < g.rowItems.size(); ++i)
        if (g.selected[i] && g.rowItems[i] < remove.size()) {
            anySelected = true;
            if (!all) remove[g.rowItems[i]] = keepSelected ? 0 : 1;
        }
    if (keepSelected && !anySelected) return;
    RemoveOccurrences(remove);
}

void RemoveDuplicateTracks() {
    if (!Active().Editable()) return;
    std::unordered_set<TrackId> seen;
    std::vector<char> remove(Active().items.size(), 0);
    for (size_t i = 0; i < remove.size(); ++i) remove[i] = !seen.insert(Active().items[i]).second;
    size_t count = RemoveOccurrences(remove);
    ShowMessage(L"Removed " + std::to_wstring(count) + L" duplicate(s).");
}

void CheckDeadItems() {
    if (!Active().Editable() || g_playlistFileBusy || g_playlistImport) return;
    if (!Active().identity) Active().identity = g_nextPlaylistIdentity++;
    auto request = std::make_unique<PlaylistFile>();
    request->checkDead = true;
    request->target = Active().identity;
    request->snapshot = Active().items;
    std::unordered_set<TrackId> seen;
    for (TrackId id : request->snapshot)
        if (g_lib.Contains(id) && seen.insert(id).second) request->checkedPaths.push_back({id, g_lib.Path(id)});
    OnPlaylistFile(0, (LPARAM)request.release());
}

void FindDuplicateAudio() {
    if (g_playlistFileBusy || g_playlistImport) return;
    auto request = std::make_unique<PlaylistFile>();
    request->findDuplicates = true;
    for (TrackId id = 0; id < (TrackId)g_lib.Count(); ++id) {
        const Track& t = g_lib[id];
        if (t.audioHash && !(t.flags & Track::kMissing) && (t.flags & (Track::kInLibrary | Track::kInbox)))
            request->hashes.push_back({t.audioHash, id});
    }
    OnPlaylistFile(0, (LPARAM)request.release());
}

void AddSteamSoundtracks() {
    if (g_playlistFileBusy || g_playlistImport) return;
    auto request = std::make_unique<PlaylistFile>();
    request->findSteam = true;
    request->watchedFolders = g.settings.folders;
    OnPlaylistFile(0, (LPARAM)request.release());
}

std::vector<std::wstring> NewSteamFolders(const std::vector<std::wstring>& folders, const std::atomic<bool>& cancel) {
    auto roots = FindSteamMusicFolders(&cancel);
    std::vector<std::wstring> watched, result;
    for (const auto& folder : folders) {
        if (cancel) return {};
        char* resolved = realpath(compat::Narrow(folder.c_str()).c_str(), nullptr);
        watched.push_back(resolved ? compat::Widen(resolved) : folder);
        std::free(resolved);
    }
    for (const auto& root : roots) {
        if (cancel) return {};
        bool covered = std::any_of(watched.begin(), watched.end(), [&](const auto& folder) {
            return root == folder || (!folder.empty() && root.size() > folder.size() &&
                   root.compare(0, folder.size(), folder) == 0 &&
                   (root[folder.size()] == L'/' || folder.back() == L'/'));
        });
        if (!covered) result.push_back(root);
    }
    return result;
}

void SetTheme(ThemeMode mode) {
    if (!ThemeAvailable(mode)) return;
    g.settings.theme = mode;
    SaveSettings();
    ApplyTheme();
}

void MoveFocus(int row, bool extend);

void RevealPlaying() {
    int np = NowPlayingIndex();
    if (g.playing == kNone || np < 0) return;
    if (g.active != np) SetActiveTab(np);
    for (size_t i = 0; i < g.rowItems.size(); ++i)
        if (g.queuePos >= 0 && g.rowItems[i] == (size_t)g.queuePos) { MoveFocus((int)i, false); break; }
}

void FilesChosen(void* data, const char* const* list, int) {
    auto* request = static_cast<PlaylistFile*>(data);
    if (list && !*list) { delete request; return; }
    if (!list) request->failed = true;
    else for (; *list; ++list) request->inputs.push_back(compat::Widen(*list));
    if (!PostMessageW(nullptr, WM_APP_PLAYLIST_FILE, 0, (LPARAM)request)) delete request;
}

void FolderDialogCallback(void*, const char* const* list, int) {
    auto* folders = new std::vector<std::wstring>();
    if (list)
        for (; *list; ++list) folders->push_back(compat::Widen(*list));
    if (!PostMessageW(nullptr, WM_APP + 30, 0, (LPARAM)folders)) delete folders;
}

std::string MusicFolder() {
    if (const char* music = SDL_GetUserFolder(SDL_FOLDER_MUSIC); music && *music) return music;
    const char* home = getenv("HOME");
    return std::string(home ? home : "/") + "/Music";
}

void ChooseFolder() {
    std::string music = MusicFolder();
    SDL_ShowOpenFolderDialog(FolderDialogCallback, nullptr, g.window, music.c_str(), true);
}

std::unique_ptr<PlaylistFile> NewPathImport() {
    if (g_playlistFileBusy || g_playlistImport) { ShowMessage(L"A playlist file is already being processed."); return {}; }
    int target = g.active;
    if (!Active().Editable()) {
        auto it = std::find_if(g.playlists.begin(), g.playlists.end(), [](const Playlist& p) { return p.kind == PlaylistKind::Library; });
        if (it == g.playlists.end()) return {};
        target = (int)(it - g.playlists.begin());
        SetActiveTab(target);
    }
    if (!g.playlists[target].identity) g.playlists[target].identity = g_nextPlaylistIdentity++;
    auto request = std::make_unique<PlaylistFile>();
    request->addToExisting = true;
    request->target = g.playlists[target].identity;
    return request;
}

void ChooseImportFolder() {
    auto request = NewPathImport();
    if (!request) return;
    std::string music = MusicFolder();
    SDL_ShowOpenFolderDialog(FilesChosen, request.release(), g.window, music.c_str(), true);
}

void ChooseFiles() {
    auto request = NewPathImport();
    if (!request) return;
    static const SDL_DialogFileFilter filter{"Audio files", "flac;mp3;m4a;mp4;aac;ogg;oga;opus;wav;aiff;aif;wv;ape;mka;mpc;cord;cue;m3u;m3u8"};
    std::string music = MusicFolder();
    SDL_ShowOpenFileDialog(FilesChosen, request.release(), g.window, &filter, 1, music.c_str(), true);
}

void PlaylistFileChosen(void* data, const char* const* list, int) {
    auto* request = static_cast<PlaylistFile*>(data);
    if (list && !*list) { delete request; return; }
    if (!list) request->failed = true;
    else request->path = compat::Widen(*list);
    if (!PostMessageW(nullptr, WM_APP_PLAYLIST_FILE, 0, (LPARAM)request)) delete request;
}

void ChoosePlaylistFile(bool save) {
    if (g_playlistFileBusy || g_playlistImport) { ShowMessage(L"A playlist file is already being processed."); return; }
    auto request = std::make_unique<PlaylistFile>();
    request->save = save;
    if (save) {
        Playlist snapshot = Active();
        snapshot.items = PlaylistTracks(snapshot);
        SortPlaylistTracks(snapshot.items, snapshot);
        if (!CaptureM3U(snapshot, request->entries)) { ShowMessage(L"The playlist contains an unknown track."); return; }
    }
    static const SDL_DialogFileFilter filter{"M3U playlists", "m3u;m3u8"};
    std::string location = MusicFolder();
    if (save) {
        location += "/playlist.m3u8";
        SDL_ShowSaveFileDialog(PlaylistFileChosen, request.release(), g.window, &filter, 1, location.c_str());
    } else SDL_ShowOpenFileDialog(PlaylistFileChosen, request.release(), g.window, &filter, 1, location.c_str(), false);
}

void EndRename(bool commit) {
    if (g_renameIndex < 0) return;
    if (commit && g_renameIndex < (int)g.playlists.size() && g_renameText.find_first_not_of(L" \t") != std::wstring::npos) {
        std::wstring name = CleanPlaylistName(g_renameText);
        bool duplicate = false;
        for (int i = 0; i < (int)g.playlists.size(); ++i)
            if (i != g_renameIndex && CompareStringOrdinal(g.playlists[i].name.c_str(), -1, name.c_str(), -1, TRUE) == CSTR_EQUAL)
                duplicate = true;
        g.playlists[g_renameIndex].name = duplicate ? UniquePlaylistName(g.playlists, name) : name;
        g.libraryDirty = true;
    }
    g_renameIndex = -1;
    g_renameText.clear();
    SDL_StopTextInput(g.window);
    g.redraw = true;
}

void BeginRename(int index) {
    if (index < 0 || index >= (int)g.playlists.size()) return;
    if (g.playlists[index].kind == PlaylistKind::Library || g.playlists[index].kind == PlaylistKind::Inbox) return;
    EndRename(true);
    g.searchFocused = false;
    g_renameIndex = index;
    g_renameText = g.playlists[index].name;
    g_renameSelected = true;
    g_renameCaret = g_renameText.size();
    SDL_StartTextInput(g.window);
    g.redraw = true;
}

void NewPlaylist() {
    EndRename(true);
    g.playlists.push_back(Playlist{UniquePlaylistName(g.playlists, L"New Playlist"), {}, PlaylistKind::Static});
    g.libraryDirty = true;
    SetActiveTab((int)g.playlists.size() - 1);
    BeginRename(g.active);
}

void DuplicatePlaylist() {
    EndRename(true);
    Playlist copy = Active();
    copy.items = PlaylistTracks(copy);
    copy.name = UniquePlaylistName(g.playlists, copy.name + L" copy");
    copy.kind = PlaylistKind::Static;
    copy.rules.clear();
    copy.position = {};
    copy.identity = 0;
    int index = g.active + 1;
    g.playlists.insert(g.playlists.begin() + index, std::move(copy));
    g.libraryDirty = true;
    SetActiveTab(index);
}

void ClosePlaylist(int index) {
    if (index < 0 || index >= (int)g.playlists.size()) return;
    PlaylistKind kind = g.playlists[index].kind;
    if (kind != PlaylistKind::Static && kind != PlaylistKind::Auto && kind != PlaylistKind::NowPlaying) return;
    if (kind == PlaylistKind::NowPlaying && g.playing != kNone) {
        ShowMessage(L"Stop playback before closing Now Playing.");
        return;
    }
    EndRename(true);
    if (kind == PlaylistKind::NowPlaying) {
        g.queuePos = -1;
        g.shuffleOrder.clear();
        g.shufflePos = -1;
    }
    g_closedPlaylists.push_back({std::move(g.playlists[index]), index});
    size_t retained = 0;
    for (const auto& closed : g_closedPlaylists) retained += closed.playlist.items.size();
    while (g_closedPlaylists.size() > 8 || (retained > 200000 && g_closedPlaylists.size() > 1)) {
        retained -= g_closedPlaylists.front().playlist.items.size();
        g_closedPlaylists.pop_front();
    }
    g.playlists.erase(g.playlists.begin() + index);
    int active = g.active > index ? g.active - 1 : std::min(g.active, (int)g.playlists.size() - 1);
    g.libraryDirty = true;
    SetActiveTab(active);
    ShowMessage(L"Playlist closed. Ctrl+Shift+T reopens it.");
}

void ReopenPlaylist() {
    if (g_closedPlaylists.empty()) return;
    EndRename(true);
    ClosedPlaylist closed = std::move(g_closedPlaylists.back());
    g_closedPlaylists.pop_back();
    if (closed.playlist.kind == PlaylistKind::NowPlaying && NowPlayingIndex() >= 0) {
        // Now Playing was made again since it closed: the old tracks go after the new ones.
        const int np = NowPlayingIndex();
        auto& items = g.playlists[np].items;
        items.insert(items.end(), closed.playlist.items.begin(), closed.playlist.items.end());
        g.libraryDirty = true;
        SetActiveTab(np);
        ShowMessage(L"Now Playing reopened; its earlier tracks are after the new ones.");
        return;
    }
    int index = std::clamp(closed.index, 1, (int)g.playlists.size());
    closed.playlist.name = UniquePlaylistName(g.playlists, closed.playlist.name);
    g.playlists.insert(g.playlists.begin() + index, std::move(closed.playlist));
    g.libraryDirty = true;
    SetActiveTab(index);
}

void ReorderPlaylist(int direction) {
    EndRename(true);
    int target = g.active + direction;
    if (g.active <= 0 || target <= 0 || !MovePlaylist(g.playlists, g.active, target)) return;
    g.active = target;
    g.libraryDirty = true;
    g.redraw = true;
}

void ToggleMute() {
    if (g_muteVolume > 0) {
        SetVolume(g_muteVolume);
        g_muteVolume = 0;
    } else if (g.settings.volume > 0) {
        g_muteVolume = g.settings.volume;
        SetVolume(0);
    }
}

void SectionAction(int action);

void RunAction(const MenuItem& item) {
    switch (item.action) {
        case kAddFiles: ChooseFiles(); break;
        case kOpenPlaylistFile: ChoosePlaylistFile(false); break;
        case kSavePlaylistFile: ChoosePlaylistFile(true); break;
        case kBackup: ChooseMigration(1); break;
        case kRestore: ChooseMigration(2); break;
        case kStatistics: ChooseMigration(3); break;
        case kAddFolder: ChooseImportFolder(); break;
        case kExit: g.quit = true; break;
        case kSelectAll: std::fill(g.selected.begin(), g.selected.end(), 1); break;
        case kRemove:
            if (Active().kind == PlaylistKind::Library || Active().kind == PlaylistKind::Inbox) ExcludeFromLibrary(SelectedTracks());
            else RemoveFromPlaylist(false);
            break;
        case kKeepSelected: RemoveFromPlaylist(false, true); break;
        case kClear: RemoveFromPlaylist(true); break;
        case kReloadTags: {
            auto ids = SelectedTracks();
            if (TagWriteBusy()) { ShowMessage(L"Tags are still being written; try again in a moment."); break; }
            for (TrackId id : ids) g_lib[id].flags &= (uint16_t)~(Track::kLoaded | Track::kTagFailed);
            QueueTags(ids);
            if (std::find(ids.begin(), ids.end(), g.playing) != ids.end()) QueueChapters(g.playing);
            if (!ids.empty()) ShowMessage(L"Reloading metadata...");
            break;
        }
        case kRemoveDuplicates: RemoveDuplicateTracks(); break;
        case kRemoveDead: CheckDeadItems(); break;
        case kCancelDead: g_playlistFileCancel = true; break;
        case kFind:
            g.searchFocused = true;
            SDL_StartTextInput(g.window);
            break;
        case kPlayPause: TogglePause(); break;
        case kStopAction:
            HoldPendingOpens();
            Stop();
            break;
        case kMute: ToggleMute(); break;
        case kPrevAction: Previous(); break;
        case kNextAction: Next(); break;
        case kShuffleAction: SetShuffle(g.settings.shuffle == kShuffleTracks ? kShuffleOff : kShuffleTracks); break;
        case kShuffleAlbumsAction: SetShuffle(g.settings.shuffle == kShuffleAlbums ? kShuffleOff : kShuffleAlbums); break;
        case kRadio: SetRadio(!g.settings.radio); break;
        case kRepeatSet:
            SetRepeat((Repeat)item.arg);
            break;
        case kSpeed:
            if (item.arg >= 0 && item.arg < (int)std::size(kSpeeds)) SetSpeed(kSpeeds[item.arg]);
            break;
        case kTempo:
            if (item.arg >= 0 && item.arg < (int)std::size(kTempos)) SetTempo(kTempos[item.arg]);
            break;
        case kStopAfter: ToggleStopAfterCurrent(); break;
        case kShared: SetExclusive(false); break;
        case kExclusive: SetExclusive(true); break;
        case kNewPl: NewPlaylist(); break;
        case kRenamePl: BeginRename(g.active); break;
        case kClosePl: if (InternetRadio().active) CloseRadio(); else ClosePlaylist(g.active); break;
        case kDuplicatePl: DuplicatePlaylist(); break;
        case kReopenPl: ReopenPlaylist(); break;
        case kMovePlLeft: ReorderPlaylist(-1); break;
        case kMovePlRight: ReorderPlaylist(1); break;
        case kNextPl: SetActiveTab((g.active + 1) % (int)g.playlists.size()); break;
        case kPrevPl: SetActiveTab((g.active + (int)g.playlists.size() - 1) % (int)g.playlists.size()); break;
        case kReveal: RevealPlaying(); break;
        case kFolders: OpenLibraryFolders(); break;
        case kRescan:
            g.scanner.RescanAll();
            g.scanning = true;
            break;
        case kAcceptInbox: AcceptInbox(SelectedTracks()); break;
        case kAcceptAllInbox: {
            std::vector<TrackId> all;
            for (TrackId id = 0; id < (TrackId)g_lib.Count(); ++id)
                if (g_lib[id].flags & Track::kInbox) all.push_back(id);
            AcceptInbox(all);
            break;
        }
        case kExcludeLibrary: ExcludeFromLibrary(SelectedTracks()); break;
        case kRating: SetRating(SelectedTracks(), (uint8_t)std::clamp(item.arg, 0, 100)); break;
        case kSectionPlay: case kSectionLoop: case kSectionStart: case kSectionEnd: case kSectionClear:
            SectionAction(item.action); break;
        case kDuplicateAudio: FindDuplicateAudio(); break;
        case kCancelDuplicates: g_playlistFileCancel = true; break;
        case kSteam: AddSteamSoundtracks(); break;
        case kCancelSteam: g_playlistFileCancel = true; break;
        case kWebServer: OpenWebServer(); break;
        case kExportCord: ExportCordTracks(SelectedTracks(), (CordExport)item.arg); break;
        case kCancelExport: CancelCordExport(); break;
        case kConvert: ConvertSelected(SelectedTracks()); break;
        case kConvertUndo: UndoConversion(); break;
        case kOrganize: OpenOrganize(SelectedTracks()); break;
        case kOrganizeUndo: UndoOrganize(); break;
        case kCopyTo: {
            // The selection, or the whole playlist when nothing is selected.
            std::vector<TrackId> ids = SelectedTracks();
            if (ids.empty()) OpenCopy(Active().items, Active().name);
            else OpenCopy(ids, L"");
            break;
        }
        case kCancelCopy: CancelCopy(); break;
        case kCancelConvert: CancelConversion(); break;
        case kNewAuto: OpenRules(-1); break;
        case kEditAuto: OpenRules(g.active); break;
        case kChoice: {
            auto pick = g_choicePick;
            if (pick) pick(item.arg);
            break;
        }
        case kPreferences: OpenPreferences(); break;
        case kTheme: SetTheme((ThemeMode)item.arg); break;
        case kGroupAlbums:
            g.settings.groupAlbums = !g.settings.groupAlbums;
            SaveSettings();
            RebuildRows();
            break;
        case kStartOnLogin: ToggleStartOnLogin(); break;
        case kAbout: g_aboutOpen = true; break;
        case kNameThatTrack: OpenGames(0); break;
        case kWhichCameFirst: OpenGames(1); break;
        case kPoints: OpenPoints(); break;
        case kPalette: OpenPalette(); break;
        case kLyrics: ToggleLyrics(); break;
        case kOpenRadio: OpenRadio(); break;
        case kLayout: OpenLayout(); break;
        case kPacks: OpenPacks(); break;
        case kPackPlaylist: AddPackAutoPlaylist((size_t)item.arg); break;
        case kHelpers: OpenHelpers(); break;
        case kHelperTool: RunHelperTool((size_t)item.arg, SelectedTracks()); break;
        case kHelperCancel: CancelHelperTool(); break;
        case kMini: CloseMenus(); ClosePanel(); ToggleMini(); break;
        case kReview: OpenReview(); break;
        case kPlay: {
            auto sel = SelectedTracks();
            if (g.focus >= 0 && sel.size() <= 1) PlayRow(g.focus);
            else
                for (size_t i = 0; i < g.selected.size(); ++i)
                    if (g.selected[i]) {
                        PlayRow((int)i);
                        break;
                    }
            break;
        }
        case kPlayNext: QueueTracks(SelectedTracks(), true); break;
        case kAddToQueue: QueueTracks(SelectedTracks(), false); break;
        case kClearQueue: ClearQueue(); break;
        case kAddToNowPlaying: InsertIntoNowPlaying(SelectedTracks(), false); break;
        case kAddToPlaylist: AddToPlaylist(item.arg, SelectedTracks()); break;
        case kRemoveFromNowPlaying: RemoveFromPlaylist(false); break;
        case kOpenFolder:
            if (g.focus >= 0 && g.focus < (int)g.rows.size()) ShowFolder(g.rows[g.focus]);
            break;
        case kEditTags: OpenTagEditor(SelectedTracks()); break;
        case kUndoTags: UndoLastTagEdit(); break;
        case kEqualizer: OpenEqualizer(); break;
        case kRgEnable: SetReplayGain(!g.settings.rg.on); break;
        case kRgScanSel: ScanReplayGain(SelectedTracks()); break;
        case kRgScanPl: ScanReplayGain(g.rows); break;
        case kRgCancel: CancelReplayGain(); break;
        case kRgClear: ClearReplayGain(SelectedTracks()); break;
        case kRgWrite: WriteReplayGainTags(SelectedTracks()); break;
        case kWriteStatistics: WriteStatisticsTags(SelectedTracks()); break;
        case kColumnToggle: {
            auto& cols = g.settings.columns;
            auto it = std::find(cols.begin(), cols.end(), item.arg);
            if (it != cols.end()) cols.erase(it);
            else cols.push_back(item.arg);
            SaveSettings();
            break;
        }
    }
    g.redraw = true;
}

// --- selection -----------------------------------------------------------------------------------------
void SelectOnly(int row) {
    std::fill(g.selected.begin(), g.selected.end(), 0);
    if (row >= 0 && row < (int)g.selected.size()) g.selected[row] = 1;
    g.focus = g.anchor = row;
}

void SelectRange(int from, int to) {
    std::fill(g.selected.begin(), g.selected.end(), 0);
    if (from > to) std::swap(from, to);
    for (int i = std::max(0, from); i <= to && i < (int)g.selected.size(); ++i) g.selected[i] = 1;
}

void MoveFocus(int row, bool extend) {
    if (g.rows.empty()) return;
    row = std::clamp(row, 0, (int)g.rows.size() - 1);
    if (extend) {
        if (g.anchor < 0) g.anchor = std::max(g.focus, 0);
        SelectRange(g.anchor, row);
        g.focus = row;
    } else {
        SelectOnly(row);
    }
    EnsureVisible(row);
}

// --- the library folders dialog and the about box ---------------------------------------------------
void DrawFoldersDialog(const Palette& p) {
    int pw = 0, ph = 0;
    SDL_GetRenderOutputSize(g.renderer, &pw, &ph);
    Fill({0, 0, (float)pw, (float)ph}, 0x000000, 110);
    float w = std::min((float)pw - S(40), S(560)), rowH = S(30);
    float h = S(144) + std::max<size_t>(1, g.settings.folders.size()) * rowH;
    Rect panel{((float)pw - w) / 2, std::max(S(20), ((float)ph - h) / 2), w, h};
    g_folders.panel = panel;
    Fill(panel, p.surface);
    Frame(panel, p.border);
    float x = panel.x + S(16), y = panel.y + S(14);
    DrawText(Font::Title, "Library folders", x, y, p.text);
    y += S(32);
    g_folders.remove.clear();
    if (g.settings.folders.empty()) {
        DrawText(Font::Normal, "No folders yet. Add the folders your music is in.", x, y + S(6), p.sub);
        y += rowH;
    }
    for (auto& f : g.settings.folders) {
        Rect remove{panel.R() - S(16) - S(80), y + S(3), S(80), rowH - S(6)};
        DrawText(Font::Normal, Utf8(f), x, y + (rowH - LineHeight(Font::Normal)) / 2, p.text, remove.x - x - S(8));
        bool hover = remove.In(g_mouseX, g_mouseY);
        Fill(remove, hover ? p.hover : p.surface2);
        Frame(remove, p.border);
        DrawText(Font::Normal, "Remove", remove.x, remove.y + (remove.h - LineHeight(Font::Normal)) / 2, p.text, remove.w, 1);
        g_folders.remove.push_back(remove);
        y += rowH;
    }
    y += S(8);
    DrawText(Font::Normal, "Folders are watched while cordinal runs.", x, y, p.sub, panel.w - S(32));
    y += S(24);
    g_folders.add = {x, y, S(130), S(30)};
    g_folders.music = {x + S(138), y, S(140), S(30)};
    g_folders.done = {panel.R() - S(16) - S(90), y, S(90), S(30)};
    for (auto [r, label, accent] : {std::tuple{g_folders.add, "Add folder...", true},
                                  std::tuple{g_folders.music, "Add Music folder", false},
                                  std::tuple{g_folders.done, "Done", false}}) {
        bool hover = r.In(g_mouseX, g_mouseY);
        Fill(r, accent ? p.accent : (hover ? p.hover : p.surface2));
        if (!accent) Frame(r, p.border);
        DrawText(Font::Normal, label, r.x, r.y + (r.h - LineHeight(Font::Normal)) / 2, accent ? TextOn(p.accent) : p.text, r.w, 1);
    }
}

void DrawExcludeConfirm(const Palette& p) {
    int pw = 0, ph = 0;
    SDL_GetRenderOutputSize(g.renderer, &pw, &ph);
    Fill({0, 0, (float)pw, (float)ph}, p.bg, 110);
    float w = std::min((float)pw - S(40), S(510)), h = S(172);
    Rect panel{((float)pw - w) / 2, ((float)ph - h) / 2, w, h};
    g_exclude.panel = panel;
    Fill(panel, p.surface);
    Frame(panel, p.border);
    float x = panel.x + S(16), y = panel.y + S(15);
    DrawText(Font::Title, "Remove from library", x, y, p.text);
    y += S(36);
    std::string prompt = "Remove " + std::to_string(g_exclude.ids.size()) +
                         (g_exclude.ids.size() == 1 ? " track" : " tracks") + " from the library?";
    DrawText(Font::Normal, prompt, x, y, p.text, panel.w - S(32));
    y += S(26);
    DrawText(Font::Normal, "The files stay on disk and won't be added back by scans.", x, y, p.sub, panel.w - S(32));
    g_exclude.confirm = {panel.R() - S(206), panel.B() - S(44), S(90), S(28)};
    g_exclude.cancel = {panel.R() - S(106), panel.B() - S(44), S(90), S(28)};
    for (auto [r, label] : {std::pair{g_exclude.confirm, "Remove"}, std::pair{g_exclude.cancel, "Cancel"}}) {
        Fill(r, r.In(g_mouseX, g_mouseY) ? p.hover : p.surface2);
        Frame(r, p.border);
        DrawText(Font::Normal, label, r.x, r.y + (r.h - LineHeight(Font::Normal)) / 2, p.text, r.w, 1);
    }
}

bool FoldersClick(float x, float y) {
    if (!g_folders.panel.In(x, y) || g_folders.done.In(x, y)) {
        g_folders.open = false;
        return true;
    }
    if (g_folders.add.In(x, y)) {
        ChooseFolder();
        return true;
    }
    if (g_folders.music.In(x, y)) {
        AddFolder(compat::Widen(MusicFolder().c_str()));
        return true;
    }
    for (size_t i = 0; i < g_folders.remove.size(); ++i)
        if (g_folders.remove[i].In(x, y)) {
            RemoveFolder(i);
            return true;
        }
    return true;
}

void WebAction(int control) {
    if (control == 1) {
        if (WebRunning()) StopWeb();
        else {
            unsigned long port = strtoul(g_web.port.c_str(), nullptr, 10);
            if (g_web.port.empty() || !port || port > 65535) { ShowMessage(L"Enter a port from 1 to 65535."); return; }
            g.settings.webPort = (uint16_t)port; SaveSettings(); StartWeb();
            if (WebRunning()) PairWeb();
        }
    } else if (control == 2) { g.settings.webStart = !g.settings.webStart; SaveSettings(); }
    else if (control == 3) PairWeb();
    else if (control == 4) RevokeWeb("");
    else if (control == 5) g_web.open = false;
    else if (control == 6 && WebRunning()) {
        auto url = "http://127.0.0.1:" + std::to_string(WebStatus().port) + '/'; SDL_OpenURL(url.c_str());
    } else if (control == 7 && WebRunning()) {
        SDL_SetClipboardText(web::PhoneAddress(WebStatus()).c_str()); ShowMessage(L"Phone address copied.");
    } else if (control == 8) SDL_SetClipboardText(WebStatus().pairingCode.c_str());
    else if (control == 9) SetWebAway(!WebAway());
    else if (control == 10) RetryWebAway();
    else if (control == 11 && !WebStatus().away.empty()) {
        SDL_SetClipboardText(WebStatus().away.c_str()); ShowMessage(L"Internet address copied.");
    } else if (control == 12 || control == 13) {
        std::wstring notice = SetWebFixedCode(g_web.fixed);
        g_web.fixed = g.settings.webCode;  // as kept: spaces and dashes go
        if (!notice.empty()) ShowMessage(notice);
    }
    g.redraw = true;
}

// The pairing link as a QR code: dark modules on a light field from the palette's own pair, so every theme's
// code scans, with the four-module quiet zone around it.
void DrawPairingCode(const Rect& r, const Palette& p) {
    Fill(r, p.surface2);
    const qr::Code& code = g_web.qr;
    if (!code.size) return;
    const int modules = code.size + 8;
    const float unit = std::max(1.0f, std::floor(std::min(r.w, r.h) / modules)), side = unit * modules;
    const float left = std::round(r.x + (r.w - side) / 2), top = std::round(r.y + (r.h - side) / 2);
    Fill({left, top, side, side}, p.dark ? p.text : p.bg);
    for (int y = 0; y < code.size; ++y)
        for (int x = 0; x < code.size; ++x)
            if (code.Dark(x, y)) Fill({left + (x + 4) * unit, top + (y + 4) * unit, unit, unit}, p.dark ? p.bg : p.text);
}

void DrawWeb(const Palette& p) {
    int pw = 0, ph = 0; SDL_GetRenderOutputSize(g.renderer, &pw, &ph);
    Fill({0, 0, (float)pw, (float)ph}, p.bg, 190);
    float w = std::min((float)pw - S(24), S(840)), h = std::min((float)ph - S(24), S(720));
    auto& panel = g_web.panel; panel = {((float)pw - w) / 2, ((float)ph - h) / 2, w, h};
    Fill(panel, p.surface); Frame(panel, p.border);
    // The QR code and how to use it take the right-hand column, as in the Windows window.
    const float column = S(204);
    float x = panel.x + S(16), y = panel.y + S(14), width = w - S(32) - column;
    WebPairing pairing = CurrentWebPairing();
    if (pairing.link != g_web.qrText) {
        g_web.qrText = pairing.link;
        g_web.qr = pairing.link.empty() ? qr::Code{} : qr::Encode(pairing.link);
    }
    {
        Rect code{panel.R() - S(16) - S(188), panel.y + S(56), S(188), S(188)};
        DrawPairingCode(code, p);
        float cy = code.B() + S(8);
        for (const std::string& line : Wrap(Font::Small, pairing.caption, code.w)) {
            DrawText(Font::Small, line, code.x, cy, p.sub, code.w); cy += LineHeight(Font::Small);
        }
    }
    DrawText(Font::Title, "Web server", x, y, p.text); y += S(32);
    auto status = WebStatus();
    std::string message = status.error.empty() ? status.running ? "On: play on a phone using an address below." : "Off: start to share your library." : status.error;
    DrawText(Font::Normal, message, x, y, status.error.empty() ? p.sub : p.error, width); y += S(30);
    g_web.controls[0] = {x + S(35), y, S(70), S(28)};
    DrawText(Font::Normal, "Port", x, y + S(5), p.sub);
    Fill(g_web.controls[0], p.surface2); Frame(g_web.controls[0], g_web.focus == 0 ? p.accent : p.border);
    DrawText(Font::Normal, g_web.port, x + S(41), y + S(5), p.text);
    g_web.controls[1] = {x + S(118), y, S(112), S(28)};
    g_web.controls[2] = {x + S(242), y, width - S(242), S(28)};
    y += S(38);
    g_web.controls[6] = {x, y, S(150), S(28)};
    g_web.controls[7] = {x + S(162), y, S(176), S(28)}; y += S(38);
    DrawText(Font::Small, "Click an address to copy it. Open it on the phone.", x, y, p.sub); y += S(24);
    g_web.addresses.clear();
    size_t shown = 0;
    for (const auto& address : status.addresses) {
        if (!status.running || address.find("127.0.0.1") != address.npos || address.find("localhost") != address.npos ||
            address.find("[::1]") != address.npos) continue;
        if (shown++ >= 3) break;
        Rect r{x, y, width, S(24)}; g_web.addresses.emplace_back(r, address);
        if (r.In(g_mouseX, g_mouseY)) Fill(r, p.hover);
        DrawText(Font::Normal, address, r.x, r.y + S(2), p.text, r.w); y += S(24);
    }
    if (!shown) { DrawText(Font::Normal, status.running ? "No network address was found." : "Server is off.", x, y, p.sub); y += S(24); }
    y += S(8); g_web.controls[3] = {x, y, S(112), S(28)};
    g_web.controls[8] = {x + width - S(90), y, S(90), S(28)};
    std::string code = !status.pairingCode.empty() && !status.pairingSeconds ? "Code: " + status.pairingCode + "  (fixed)"
        : !status.pairingCode.empty() ? "Code: " + status.pairingCode + " (" + std::to_string(status.pairingSeconds) + " s)"
        : status.running && status.fixedCode ? "Locked by wrong codes. Choose Pair device."
        : "Generate a temporary pairing code.";
    DrawText(Font::Normal, code, x + S(126), y + S(5), p.sub, width - S(226)); y += S(38);
    // A fixed code: typed once on each phone at home, open while the server is on.
    DrawText(Font::Normal, "Fixed code", x, y + S(5), p.sub);
    g_web.controls[12] = {x + S(84), y, S(140), S(28)};
    Fill(g_web.controls[12], p.surface2); Frame(g_web.controls[12], g_web.focus == 12 ? p.accent : p.border);
    if (g_web.fixed.empty() && g_web.focus != 12) DrawText(Font::Normal, "none", g_web.controls[12].x + S(6), y + S(5), p.faint);
    float typed = DrawText(Font::Normal, g_web.fixed, g_web.controls[12].x + S(6), y + S(5), p.text, g_web.controls[12].w - S(12));
    if (g_web.focus == 12) Fill({g_web.controls[12].x + S(6) + typed, y + S(6), S(1), LineHeight(Font::Normal)}, p.text);
    g_web.controls[13] = {g_web.controls[12].R() + S(12), y, S(128), S(28)}; y += S(32);
    for (const std::string& line : Wrap(Font::Small, "Leave it empty for a new code each time. A fixed code keeps pairing open while the "
                                                     "server is on; five wrong codes lock it until you choose Pair device.", width)) {
        DrawText(Font::Small, line, x, y, p.sub, width); y += LineHeight(Font::Small);
    }
    y += S(12);
    // Away from home: the switch, Check again and the internet address, then what is happening, in words.
    g_web.controls[9] = {x, y, S(220), S(28)};
    g_web.controls[10] = {x + S(230), y, S(112), S(28)};
    g_web.controls[11] = {x + width - S(176), y, S(176), S(28)}; y += S(34);
    for (const std::string& line : Wrap(Font::Normal, WebAwayText(), width)) {
        DrawText(Font::Normal, line, x, y, p.sub, width); y += LineHeight(Font::Normal);
    }
    y += S(10);
    DrawText(Font::Normal, "Paired devices (Up / Down to scroll)", x, y, p.sub); y += S(24);
    g_web.devices.clear();
    int visible = std::max(1, (int)((panel.B() - S(96) - y) / S(28)));
    g_web.scroll = std::clamp(g_web.scroll, 0, std::max(0, (int)status.devices.size() - visible));
    for (int i = g_web.scroll; i < (int)status.devices.size() && i < g_web.scroll + visible; ++i) {
        const auto& device = status.devices[i];
        DrawText(Font::Normal, device.name + (device.seen ? device.away ? "  \xC2\xB7 away" : "  \xC2\xB7 at home" : ""), x, y + S(5),
                 p.text, width - S(100));
        Rect remove{x + width - S(90), y, S(90), S(26)}; Frame(remove, p.border);
        DrawText(Font::Normal, "Remove", remove.x, remove.y + S(4), p.text, remove.w, 1);
        g_web.devices.emplace_back(remove, device.id); y += S(28);
    }
    if (status.devices.empty()) DrawText(Font::Normal, "No paired devices.", x, y, p.sub);
    y = panel.B() - S(80); g_web.controls[4] = {x, y, S(112), S(28)};
    g_web.controls[5] = {x + width - S(90), y, S(90), S(28)};
    const char* labels[kWebControls] = {"", status.running ? "Stop server" : "Start server",
        g.settings.webStart ? "[x] Start with cordinal" : "[ ] Start with cordinal", "Pair device", "Remove all", "Close",
        "Open web player", "Copy phone address", "Copy code",
        WebAway() ? "[x] Listen away from home" : "[ ] Listen away from home", "Check again", "Copy internet address", "",
        "Use this code"};
    const bool enabled[kWebControls] = {true, true, true, true, true, true, true, true, true, true, WebAway() && status.running,
                                        !status.away.empty(), true, true};
    for (int i = 1; i < kWebControls; ++i) {
        if (i == 12) continue;  // the fixed code's field, drawn above
        auto r = g_web.controls[i];
        Fill(r, enabled[i] && r.In(g_mouseX, g_mouseY) ? p.hover : p.surface2);
        Frame(r, g_web.focus == i ? p.accent : p.border);
        DrawText(Font::Normal, labels[i], r.x, r.y + S(5), enabled[i] ? p.text : p.faint, r.w, 1);
    }
    float fy = panel.B() - S(44);
    for (const char* line : {"At home, open an address on the phone and enter the code, or scan the code.",
                             "Away from home, phones connect over HTTPS with a free Let's Encrypt certificate."}) {
        DrawText(Font::Small, line, x, fy, p.sub, w - S(32)); fy += LineHeight(Font::Small);
    }
}

void WebClick(float x, float y) {
    for (int i = 0; i < kWebControls; ++i) if (g_web.controls[i].In(x, y)) {
        g_web.focus = i;
        if (i == 0 || i == 12) SDL_StartTextInput(g.window);
        else WebAction(i);
        return;
    }
    for (const auto& [rect, address] : g_web.addresses) if (rect.In(x, y)) {
        SDL_SetClipboardText(address.c_str()); ShowMessage(L"Server address copied."); return;
    }
    for (const auto& [rect, id] : g_web.devices) if (rect.In(x, y)) { RevokeWeb(id); return; }
}

// Help > About cordinal, as the Windows box says it (app_dialogs.cpp): the cardinal, which sings when
// clicked (with sound effects on or off), what cordinal is, and what it is using now.
struct AboutBox {
    Rect icon;
    SDL_Texture* texture = nullptr;
    int textureSize = 0;
} g_about;

std::string AboutMemory() {
    long rss = 0;
    if (FILE* f = fopen("/proc/self/status", "r")) {
        char line[128];
        while (fgets(line, sizeof(line), f))
            if (!strncmp(line, "VmRSS:", 6)) rss = atol(line + 6);
        fclose(f);
    }
    char text[200];
    snprintf(text, sizeof(text), "Memory: %.1f MB resident\nLibrary: %zu tracks in %.1f KB", rss / 1024.0, g_lib.Count(),
             g_lib.MemoryBytes() / 1024.0);
    return text;
}

void DrawAbout(const Palette& p) {
    int pw = 0, ph = 0;
    SDL_GetRenderOutputSize(g.renderer, &pw, &ph);
    Fill({0, 0, (float)pw, (float)ph}, 0x000000, 110);
    const float w = std::min((float)pw - S(16), S(560));
    const float textW = w - S(18) - S(64) - S(18) - S(18);
    std::vector<std::string> lines;
    for (const char* paragraph : {"A fast, lightweight music player and library for Windows and Linux.", "",
             "Watched library folders with an Inbox, a column browser, album and artist views, auto-playlists, ratings and "
             "play counts, a tag editor and file organizer.",
             "Cordinal decoders for FLAC, WAV, AIFF, MPEG audio, Vorbis, AAC-LC, ALAC, WMA Standard, Opus, APE, WavPack and "
             "AC3, with Matroska audio support.",
             "Gapless playback, ReplayGain with an EBU R128 scanner, 10-band equalizer, exclusive (bit-perfect) output, a "
             "waveform seek bar.",
             "A play log with cordinal points and a year in review; two games played with your own music.", ""}) {
        if (!*paragraph) { lines.emplace_back(); continue; }
        for (std::string& line : Wrap(Font::Normal, paragraph, textW)) lines.push_back(std::move(line));
    }
    const std::string memory = AboutMemory();
    for (size_t at = 0; at <= memory.size();) {
        size_t end = std::min(memory.find('\n', at), memory.size());
        lines.push_back(memory.substr(at, end - at));
        at = end + 1;
    }
    const float lineH = LineHeight(Font::Normal);
    const float h = std::min((float)ph - S(16), S(18) + S(34) + lineH * lines.size() + S(40));
    Rect panel{std::round(((float)pw - w) / 2), std::round(((float)ph - h) / 2), w, h};
    Fill(panel, p.surface);
    Frame(panel, p.border);
    g_about.icon = {panel.x + S(18), panel.y + S(18), S(64), S(64)};
    if (!g_about.texture || g_about.textureSize != (int)g_about.icon.w) {
        if (g_about.texture) SDL_DestroyTexture(g_about.texture);
        g_about.texture = nullptr;
        g_about.textureSize = (int)g_about.icon.w;
        CoverImage img{};
        if (DecodeCover(std::string(AppIcon(g_about.textureSize)), g_about.textureSize, img) &&
            (g_about.texture = SDL_CreateTexture(g.renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, img.width, img.height))) {
            SDL_UpdateTexture(g_about.texture, nullptr, img.rgba.data(), img.width * 4);
            SDL_SetTextureBlendMode(g_about.texture, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(g_about.texture, SDL_SCALEMODE_LINEAR);
        }
    }
    if (g_about.texture) {
        SDL_FRect dst{g_about.icon.x, g_about.icon.y, g_about.icon.w, g_about.icon.h};
        SDL_RenderTexture(g.renderer, g_about.texture, nullptr, &dst);
    }
    float x = g_about.icon.R() + S(18), y = panel.y + S(18);
    DrawText(Font::Title, "cordinal 0.1.0", x, y, p.text);
    y += S(34);
    for (const std::string& line : lines) {
        if (y + lineH > panel.B() - S(30)) break;
        DrawText(Font::Normal, line, x, y, p.text, textW);
        y += lineH;
    }
    DrawText(Font::Small, "Click the cardinal to hear it. Click anywhere else to close.", x, panel.B() - S(26), p.faint, textW);
}

// A click on the About box: the cardinal sings; anywhere else closes it.
void AboutClick(float x, float y) {
    if (g_about.icon.In(x, y)) PlaySfx(Sfx::CardinalSong, 0, true);
    else g_aboutOpen = false;
}

// --- drawing the window ----------------------------------------------------------------------------------

// The menu bar: the system's light bar, or in a dark theme the palette's surface (app_ui.cpp,
// HandleDarkMenuBar).
void DrawMenuBar(const Palette& p) {
    bool dark = p.dark;
    Fill(L.menuBar, dark ? p.surface : 0xFFFFFF);
    if (!dark) {
        Fill({0, L.menuBar.B() - S(2), L.menuBar.w, S(1)}, 0xF2F2F2);
        Fill({0, L.menuBar.B() - S(1), L.menuBar.w, S(1)}, 0xF0F0F0);
    }
    auto menus = BarMenus();
    for (size_t i = 0; i < L.barItems.size(); ++i) {
        const Rect& r = L.barItems[i];
        bool hot = (int)i == g_barOpen || (g_popups.empty() && r.In(g_mouseX, g_mouseY));
        if (hot) Fill(r, dark ? p.hover : 0xF9F9F9);
        DrawText(Font::Menu, Plain(ui::kMenuBar[menus[i]]), r.x, TextY(r, Font::Menu), dark ? p.text : 0x000000, r.w, 1);
    }
}

// Windows 11's popup menus, light or dark with the theme.
void DrawPopups(const Palette& p) {
    bool dark = p.dark;
    uint32_t bg = dark ? 0x2C2C2C : 0xF9F9F9, edge = dark ? 0x3E3E3E : 0xE5E5E5, hot = dark ? 0x353535 : 0xF0F0F0;
    uint32_t text = dark ? 0xFFFFFF : 0x000000, off = dark ? 0x787878 : 0xA0A0A0, rule = dark ? 0x3E3E3E : 0xD7D7D7;
    for (size_t k = 0; k < g_popups.size(); ++k) {
        const Popup& pop = g_popups[k];
        Rect box{pop.x, pop.y, pop.w, pop.h};
        for (int i = 1; i <= 4; ++i) FillRound({box.x - i + S(1), box.y - i + S(2), box.w + 2 * i, box.h + 2 * i}, S(8) + i, 0x000000, 0.035f);
        FillRound(box, S(8), bg);
        FrameRound(box, S(8), edge);
        for (size_t i = 0; i < pop.items.size(); ++i) {
            const MenuItem& it = pop.items[i];
            Rect r = PopupItemRect(pop, i);
            if (it.separator) {
                Fill({r.x + S(1), std::round(r.y + r.h / 2), r.w - S(2), S(1)}, rule);
                continue;
            }
            if ((int)i == pop.hover && it.enabled) FillRound({r.x + S(4), r.y, r.w - S(8), r.h}, S(4), hot);
            uint32_t color = it.enabled ? text : off;
            float cy = r.y + r.h / 2;
            if (it.checked && it.radio) {
                Circle(r.x + S(18), cy, S(2.5f), color);
            } else if (it.checked) {
                ThickLine(r.x + S(13), cy, r.x + S(16), cy + S(3), std::max(1.0f, S(1)), color);
                ThickLine(r.x + S(16), cy + S(3), r.x + S(22), cy - S(4), std::max(1.0f, S(1)), color);
            }
            DrawText(Font::Menu, it.label, r.x + S(kLabelX), TextY(r, Font::Menu), color);
            if (!it.shortcut.empty())
                DrawText(Font::Menu, it.shortcut, r.R() - S(kShortcutRight) - TextWidth(Font::Menu, it.shortcut), TextY(r, Font::Menu), color);
            if (!it.sub.empty()) {
                float ax = r.R() - S(16);
                ThickLine(ax - S(3), cy - S(4), ax + S(1), cy, std::max(1.0f, S(1)), color);
                ThickLine(ax + S(1), cy, ax - S(3), cy + S(4), std::max(1.0f, S(1)), color);
            }
        }
    }
}

// A flat button of the band (app_ui.cpp, DrawToolbarButton): hover fills and frames it; shuffle,
// repeat and resume show they are on with an accent glyph raised over a dot.
void DrawBandButton(const Rect& r, Icon icon, const Palette& p, bool active = false, bool enabled = true) {
    if (r.w <= 0) return;
    bool hot = enabled && g_popups.empty() && r.In(g_mouseX, g_mouseY);
    bool pressed = hot && g_drag == Drag::None && (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK);
    if (hot) {
        Fill(r, pressed ? p.select : p.hover);
        Frame(r, p.border);
    }
    uint32_t color = !enabled ? p.faint : active ? p.accent : p.text;
    Rect gr = r;
    if (active) gr.y -= S(2);
    DrawIcon(icon, gr, color);
    if (active) {
        float d = std::max(2.0f, S(3));
        Fill({std::round(r.x + r.w / 2 - d / 2), r.B() - S(5) - d / 2, d, d}, color);
    }
}

// A flat slider (app_ui.cpp, DrawTrackbar): a slim channel, the accent up to the position, and a
// slim upright thumb in the text color (the accent while hovered or dragged).
void DrawSlider(const Rect& r, double frac, bool enabled, bool hot, const Palette& p) {
    float inset = S(8), h = S(4);
    Rect channel{r.x + inset, std::round(r.y + r.h / 2 - h / 2), r.w - 2 * inset, h};
    Fill(channel, p.border);
    if (frac > 0) Fill({channel.x, channel.y, std::round(channel.w * (float)frac), h}, enabled ? p.accent : p.faint);
    if (!enabled) return;
    float tw = std::max(2.0f, S(3)), th = std::min(S(14), r.h), cx = std::round(channel.x + channel.w * (float)frac);
    Fill({cx - std::floor(tw / 2), std::round(r.y + r.h / 2 - th / 2), tw, th}, hot ? p.accent : p.text);
}

// Queues (or answers at once) the peaks for `id`; a CUE track asks for its range of the file.
bool RequestPeaks(TrackId id) {
    if (id == kNone || id >= g_lib.Count()) return false;
    std::wstring path = g_lib.Path(id);
    double start = 0, end = 0;
    if (IsSubtrack(path) && !SubtrackRange(path, start, end)) return false;
    const Track& t = g_lib[id];
    return GetPeaks(path, t.fileSize, t.fileTime, start, end, g_wave.peaks, g_wave.request);
}

// The waveform over the seek bar's channel: columns two pixels wide a pixel apart, each the loudest
// peak under it, the played part in the accent (amber while paused), the rest faint. The split is
// the playhead. False while there are no peaks to draw.
bool LiveWaveformShown() { return RadioPlaying() || (g.settings.liveWaveform && g.playing != kNone); }
bool LiveFountainShown() { return g.settings.liveFountain && g.settings.theme == ThemeMode::Deltarune; }
bool DrawWaveform(const Rect& r, double frac, const Palette& p) {
    if (LiveFountainShown() && DrawLiveFountain(r, frac, p)) return true;
    bool live = LiveWaveformShown();
    if (!live && (!g_wave.ready || g_wave.track != g.playing || g.playing == kNone)) return false;
    std::array<uint8_t, 256> livePeaks{};
    // The shared engine shapes live heights around audible note attacks, as on Windows.
    if (live) g.engine.WaveformPeaks(livePeaks.data(), livePeaks.size());
    if (RadioPlaying()) frac = 1;
    float left = r.x + S(9), width = r.w - S(18), right = left + width;
    float cy = std::floor(r.y + r.h / 2), half = std::max(1.0f, std::floor(r.h / 2) - S(2));
    float col = std::max(2.0f, S(2)), step = col + std::max(1.0f, S(1)), playedX = left + std::floor(width * (float)frac);
    uint32_t played = IsPaused() ? p.amber : p.accent, rest = p.dark ? p.faint : p.border;
    if (live && g.settings.fullLiveWaveform) rest = played;
    for (float x = left; x < right; x += step) {
        int count = live ? (int)livePeaks.size() : kPeakBuckets;
        int first = std::clamp((int)((x - left) * count / width), 0, count - 1);
        int last = std::clamp((int)((x + step - left) * count / width), first + 1, count);
        int v = 0;
        for (int b = first; b < last; ++b) v = std::max<int>(v, live ? livePeaks[b] : g_wave.peaks.v[b]);
        float h = std::max(1.0f, std::floor(half * v / 255));
        uint32_t color = x < playedX ? played : rest;
        float barWidth = std::min(x + col, right) - x;
        Fill({x, cy - h, barWidth, 2 * h + 1}, color);
        uint8_t alpha = live ? (uint8_t)std::max(0.f, half * v - h * 255) : 0;
        if (alpha) {
            Fill({x, cy - h - 1, barWidth, 1}, color, alpha);
            Fill({x, cy + h + 1, barWidth, 1}, color, alpha);
        }
    }
    return true;
}

void DrawSection(const Rect& r, double duration, const Palette& p) {
    if (g.playing == kNone || duration <= 0) return;
    PlaybackSection section = SectionOf(g.playing);
    if (section.end - section.start <= 0.05) return;
    float left = r.x + S(8), width = std::max(0.0f, r.w - S(16));
    float first = left + width * (float)std::clamp(section.start / duration, 0.0, 1.0);
    float last = left + width * (float)std::clamp(section.end / duration, 0.0, 1.0);
    if (last <= first) return;
    Fill({first, r.y + S(2), last - first, r.h - S(4)}, p.accent, 45);
    Fill({first, r.y + S(2), std::max(1.0f, S(2)), r.h - S(4)}, p.accent);
    Fill({last - std::max(1.0f, S(2)), r.y + S(2), std::max(1.0f, S(2)), r.h - S(4)}, p.accent);
}

double TrackDuration() {
    double dur = g.playing != kNone ? g.engine.Duration() : 0;
    if (dur <= 0 && g.playing != kNone) dur = g_lib[g.playing].Duration();
    return dur;
}

void DrawBand(const Palette& p) {
    Fill({0, L.menuBar.B(), L.menuBar.w, L.tabs.y - L.menuBar.B()}, p.surface);
    // The handle: a few dots.
    float dot = std::max(2.0f, S(2)), step = dot + std::max(2.0f, S(2));
    float gx = std::round(L.grip.x + (L.grip.w - (2 * step - (step - dot))) / 2), gy = std::round(L.grip.y + (L.grip.h - (3 * step - (step - dot))) / 2);
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 3; ++j) Fill({gx + i * step, gy + j * step, dot, dot}, p.faint);
    bool live = RadioPlaying(), playing = g.playing != kNone || live;
    DrawBandButton(L.buttons[kBtnPrev], Icon::Prev, p);
    DrawBandButton(L.buttons[kBtnPlay], playing && !IsPaused() ? Icon::Pause : Icon::Play, p);
    DrawBandButton(L.buttons[kBtnNext], Icon::Next, p);
    DrawBandButton(L.buttons[kBtnShuffle], Icon::Shuffle, p, g.settings.shuffle);
    DrawBandButton(L.buttons[kBtnRepeat], g.settings.repeat == Repeat::One ? Icon::RepeatOne : Icon::Repeat, p, g.settings.repeat != Repeat::Off);
    DrawBandButton(L.buttons[kBtnResume], Icon::Resume, p, g.settings.resumeOn);
    // The times read toward the seek bar: elapsed on the right of its box, remaining on the left.
    double dur = TrackDuration();
    double pos = playing ? (g_seekPreview >= 0 ? g_seekPreview : g.engine.Position()) : 0;
    if (playing && L.elapsed.w > 0) {
        DrawText(Font::Normal, live ? "LIVE" : Utf8(FormatDuration(pos)), L.elapsed.x, TextY(L.elapsed), p.text, L.elapsed.w, 2);
        if (dur > 0) DrawText(Font::Normal, (g.settings.timeRemaining ? "-" + Utf8(FormatDuration(std::max(0.0, dur - pos))) : Utf8(FormatDuration(dur))), L.remaining.x, TextY(L.remaining), p.text, L.remaining.w, 0);
    }
    bool seekHot = g_popups.empty() && (L.seek.In(g_mouseX, g_mouseY) || g_drag == Drag::Seek);
    double frac = dur > 0 ? std::clamp(pos / dur, 0.0, 1.0) : 0;
    if (L.seek.w > 0) {
        if (!DrawWaveform(L.seek, frac, p)) DrawSlider(L.seek, frac, !live && playing && dur > 0, seekHot, p);
        DrawSection(L.seek, dur, p);
    }
    if (L.volume.w > 0) {
        DrawIcon(Icon::Speaker, L.speaker, p.text);
        bool volumeHot = g_popups.empty() && (L.volume.In(g_mouseX, g_mouseY) || g_drag == Drag::Volume);
        DrawSlider(L.volume, g.settings.volume, true, volumeHot, p);
    }
    if (L.cover.w <= 0) return;  // the now playing line is hidden

    // The now playing line: the cover, the title in bold over the artist line.
    Fill(L.cover, p.surface2);
    TrackId shown = playing ? g.playing : g.settings.resumeOn ? g.resumeTrack : kNone;
    if (!live && g_lib.Contains(shown) && g_cover && g_coverFor == shown) {
        SDL_FRect dst{L.cover.x, L.cover.y, L.cover.w, L.cover.h};
        SDL_RenderTexture(g.renderer, g_cover, nullptr, &dst);
    }
    std::string title, sub;
    if (live) {
        title = InternetRadio().playing.name;
        sub = InternetRadio().stream ? InternetRadio().stream->Title() : std::string();
        if (sub.empty()) sub = "Live radio";
    } else if (playing) {
        auto lookup = Lookup(g.playing, true);
        title = Utf8(FormatTitle(EffectiveFormat(kPackNowPlaying, g.settings.fmtTitle), lookup));
        sub = Utf8(FormatTitle(EffectiveFormat(kPackNowPlaying2, g.settings.fmtSub), lookup));
        if (g.chapterTrack == g.playing) {
            std::wstring chapter = ChapterTextAt(g.chapters, pos);
            if (!chapter.empty()) sub += (sub.empty() ? "" : "  \xE2\x80\xA2  ") + Utf8(chapter);
        }
    } else if (g_lib.Contains(shown)) {
        auto lookup = Lookup(shown, false);
        title = "Continue: " + Utf8(FormatTitle(L"%title%", lookup));
        sub = Utf8(FormatTitle(L"[%artist%][  \x2022  %album%]", lookup));
        if (!sub.empty()) sub += "  \xE2\x80\xA2  ";
        sub += Utf8(FormatDuration(g.resumePosition)) + " / " + Utf8(FormatDuration(g_lib[shown].Duration()));
    } else {
        title = "cordinal";
        if (Active().kind == PlaylistKind::Library && g.settings.folders.empty() && g.rows.empty())
            sub = "Your library is empty: add your music folder with Library \xE2\x96\xB8 Library folders, or drop folders here.";
    }
    DrawText(Font::Bold, title, L.title.x, TextY(L.title, Font::Bold), p.text, L.title.w);
    DrawText(Font::Normal, sub, L.subtitle.x, TextY(L.subtitle), p.sub, L.subtitle.w);
    if (L.chip.w > 0) DrawPointsChip(L.chip, p);
}

int PlayingTab() { return g.playing != kNone ? NowPlayingIndex() : -1; }

// The tab strip (app_tabstrip.cpp): flat tabs on the surface, the open one underlined in the accent,
// the playing playlist's name in the playing color, a close box on the open and hovered tabs.
void DrawTabs(const Palette& p) {
    Fill(L.tabs, p.surface);
    Fill({L.tabs.R(), L.tabs.y, L.menuBar.w - L.tabs.R(), L.tabs.h}, p.surface);
    Fill({L.tabs.x, L.tabs.B() - S(1), L.tabs.w, S(1)}, p.border);  // the rule the open tab's underline sits on
    SDL_Rect clip{(int)L.tabs.x, (int)L.tabs.y, (int)L.tabs.w, (int)L.tabs.h};
    SDL_SetRenderClipRect(g.renderer, &clip);
    for (size_t i = 0; i < L.tabRects.size(); ++i) {
        Rect r = L.tabRects[i];
        if (r.x >= L.tabs.R()) break;
        bool sel = !InternetRadio().active && (int)i == g.active, hover = g_popups.empty() && r.In(g_mouseX, g_mouseY);
        bool closable = g.playlists[i].kind == PlaylistKind::Static || g.playlists[i].kind == PlaylistKind::Auto ||
                        g.playlists[i].kind == PlaylistKind::NowPlaying;
        if (sel) Fill({r.x + S(10), r.B() - S(2), r.w - S(20), S(2)}, p.accent);
        float left = r.x + S(6), right = r.R() - S(closable ? 24 : 6);
        if ((int)i == g_renameIndex) {
            Rect field{left, r.y + S(2), std::max(S(1), right - left), r.h - S(4)};
            Fill(field, g_renameSelected ? p.select : p.bg);
            Frame(field, p.accent);
            DrawText(Font::Normal, Utf8(g_renameText), field.x + S(3), TextY(field),
                     g_renameSelected ? p.selectText : p.text, field.w - S(6));
            if (!g_renameSelected) {
                float caret = field.x + S(3) + TextWidth(Font::Normal, Utf8(g_renameText.substr(0, g_renameCaret)));
                Fill({std::min(caret, field.R() - S(2)), field.y + S(3), std::max(1.f, S(1)), field.h - S(6)}, p.text);
            }
        } else {
            std::string name = TabName(i);
            float w = TextWidth(Font::Normal, name), x = std::max(left, std::round((left + right - w) / 2));
            uint32_t color = (int)i == PlayingTab() ? p.playing : sel || hover ? p.text : p.sub;
            DrawText(Font::Normal, name, x, TextY(r), color, right - x);
        }
        if (closable && (sel || hover)) {
            Rect close{r.R() - S(22), r.y + S(3), S(19), r.h - S(6)};
            DrawText(Font::Normal, "\xC3\x97", close.x, TextY(close), hover ? p.text : p.faint, close.w, 1);
        }
    }
    SDL_SetRenderClipRect(g.renderer, nullptr);
    if (L.radioTab.w > 0) {
        Fill(L.radioTab, p.surface);
        Fill({L.radioTab.x, L.radioTab.B() - S(1), L.radioTab.w, S(1)}, p.border);
        if (InternetRadio().active) Fill({L.radioTab.x + S(8), L.radioTab.B() - S(2), L.radioTab.w - S(16), S(2)}, p.accent);
        DrawText(Font::Normal, "Radio", L.radioTab.x, TextY(L.radioTab),
                 RadioPlaying() ? p.playing : InternetRadio().active ? p.text : p.sub, L.radioTab.w - S(22), 1);
        Rect close{L.radioTab.R() - S(22), L.radioTab.y + S(3), S(19), L.radioTab.h - S(6)};
        bool hover = g_popups.empty() && close.In(g_mouseX, g_mouseY);
        if (hover) Fill(close, p.hover);
        DrawText(Font::Normal, "\xC3\x97", close.x, TextY(close), hover ? p.text : p.sub, close.w, 1);
    }
    // The search box: a field with Windows 11's darker bottom edge, and its cue while empty.
    if (L.search.w > 0) {
    Fill(L.search, p.surface2);
    Frame(L.search, g.searchFocused ? p.accent : p.border);
    Fill({L.search.x + S(1), L.search.B() - S(1), L.search.w - S(2), S(1)}, g.searchFocused ? p.accent : p.dark ? p.sub : 0x838383);
    float tx = L.search.x + S(4), ty = TextY(L.search);
    std::string cue = InternetRadio().active ? "Name, call sign or FM/AM number" : Active().kind == PlaylistKind::Library ? "Search library (Ctrl+F)" : "Search " + Utf8(Active().name) + " (Ctrl+F)";
    if (g.search.empty()) DrawText(Font::Normal, cue, tx, ty, p.sub, L.search.w - S(8));
    float w = DrawText(Font::Normal, Utf8(g.search), tx, ty, p.text, L.search.R() - tx - S(4));
    if (g.searchFocused && (SDL_GetTicks() / 530) % 2 == 0) Fill({tx + w + 1, ty, std::max(1.0f, S(1)), LineHeight(Font::Normal)}, p.text);
    }
    // The Library's view row: the current view (and the shown browser) in a square raised box, so it
    // never reads as another tab; hovering brightens a label.
    if (L.viewRow.h > 0) {
        Fill(L.viewRow, p.surface);
        for (int i = 0; i < kViewButtons; ++i) {
            const Rect& v = L.views[i];
            bool on = i < (int)LibraryMode::Count ? (int)g.settings.libraryMode == i : g.settings.browserVisible;
            if (on) {
                Fill(v, p.surface2);
                Frame(v, p.border);
            }
            DrawText(Font::Normal, ViewButtonLabel(i), v.x, TextY(v), on || v.In(g_mouseX, g_mouseY) ? p.text : p.sub, v.w, 1);
        }
    }
}

std::map<std::string, SDL_Texture*> g_radioArt;
int RadioColumns() { return std::clamp((int)((L.radioGrid.w - S(12)) / S(250)), 1, 3); }
float RadioMaxScroll() {
    int columns = RadioColumns();
    return std::max(0.0f, (float)((InternetRadio().rows.size() + columns - 1) / columns) * S(82) - L.radioGrid.h);
}
Rect RadioCard(int index) {
    int columns = RadioColumns(); float gap = S(6), pad = S(6);
    float width = std::max(1.0f, (L.radioGrid.w - 2 * pad - gap * (columns - 1)) / columns);
    return {pad + (index % columns) * (width + gap), L.radioGrid.y + (index / columns) * S(82) - InternetRadio().scroll,
            width, S(76)};
}
void DrawRadio(const Palette& p) {
    auto& radio = InternetRadio();
    Fill(L.list, p.bg);
    Fill({L.list.x, L.list.y, L.list.w, S(68)}, p.surface);
    auto button = [&](const Rect& r, const char* label, bool selected) {
        if (r.In(g_mouseX, g_mouseY)) Fill(r, p.hover);
        if (selected) Fill({r.x + S(6), r.B() - S(2), r.w - S(12), S(2)}, p.accent);
        DrawText(selected ? Font::Bold : Font::Normal, label, r.x, TextY(r), p.text, r.w, 1);
    };
    button(L.radioSaved, "My stations", !radio.discover);
    button(L.radioDiscover, "Discover", radio.discover);
    auto field = [&](const Rect& r, const std::wstring& value, const char* hint, int focus) {
        Fill(r, p.surface2); Frame(r, radio.filterFocus == focus ? p.accent : p.border);
        DrawText(Font::Normal, value.empty() ? hint : Utf8(value), r.x + S(4), TextY(r),
                 value.empty() ? p.sub : p.text, r.w - S(8));
    };
    field(L.radioGenre, radio.genre, "Genre (e.g. indie)", 1);
    field(L.radioCountry, radio.country, "Country (blank = all)", 2);
    field(L.radioState, radio.region, "State (e.g. Indiana)", 3);
    button(L.radioSearch, "Search", false);
    radio.scroll = std::clamp(radio.scroll, 0.0f, RadioMaxScroll());
    SDL_Rect clip{(int)L.radioGrid.x, (int)L.radioGrid.y, (int)L.radioGrid.w, (int)L.radioGrid.h};
    SDL_SetRenderClipRect(g.renderer, &clip);
    if (radio.rows.empty()) {
        std::wstring message = radio.browser && radio.browser->Searching() ? L"Finding stations..." :
            radio.browser && !radio.browser->Problem().empty() ? radio.browser->Problem() :
            radio.discover ? L"No stations found. Try another name, genre or country." :
                             L"No saved stations yet. Choose Discover to find stations.";
        DrawText(Font::Normal, Utf8(message), S(16), L.radioGrid.y + S(24), p.sub, L.radioGrid.w - S(32));
    }
    for (int i = 0; i < (int)radio.rows.size(); ++i) {
        Rect r = RadioCard(i);
        if (r.B() <= L.radioGrid.y || r.y >= L.radioGrid.B()) continue;
        const auto& station = radio.rows[(size_t)i];
        bool active = radio.token && station.id == radio.playing.id;
        bool selected = i == radio.selected && !g.searchFocused && !radio.filterFocus;
        Fill(r, selected ? p.select : p.surface);
        if (active) Fill({r.x, r.y, S(3), r.h}, p.accent);
        Rect art{r.x + S(8), r.y + S(8), S(50), S(50)};
        SDL_Texture* texture = nullptr;
        if (radio.browser && !station.icon.empty()) {
            radio.browser->RequestIcon(station.icon);
            if (const std::string* bytes = radio.browser->Icon(station.icon)) {
                auto found = g_radioArt.find(station.icon);
                if (found == g_radioArt.end()) {
                    if (g_radioArt.size() >= 100) {
                        auto oldest = g_radioArt.begin();
                        if (oldest->second) SDL_DestroyTexture(oldest->second);
                        g_radioArt.erase(oldest);
                    }
                    CoverImage image{}; SDL_Texture* created = nullptr;
                    if (DecodeRadioIcon(*bytes, (int)S(64), image)) {
                        created = SDL_CreateTexture(g.renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, image.width, image.height);
                        if (created) { SDL_UpdateTexture(created, nullptr, image.rgba.data(), image.width * 4); SDL_SetTextureBlendMode(created, SDL_BLENDMODE_BLEND); }
                    }
                    found = g_radioArt.emplace(station.icon, created).first;
                }
                texture = found->second;
            }
        }
        if (texture) {
            SDL_FRect dest{art.x, art.y, art.w, art.h}; SDL_RenderTexture(g.renderer, texture, nullptr, &dest);
        } else {
            Fill(art, p.surface2);
            std::wstring name = FromUtf8(station.name), initial;
            for (wchar_t c : name) if (iswalnum(c)) { initial.assign(1, (wchar_t)towupper(c)); break; }
            if (initial.empty()) initial = L"\x266B";
            DrawText(Font::Bold, Utf8(initial), art.x, TextY(art, Font::Bold), p.sub, art.w, 1);
        }
        Frame(art, p.border);
        bool favorite = radio.browser && radio.browser->Favorite(station);
        Rect star{r.R() - S(30), r.y + S(4), S(26), S(26)};
        DrawText(Font::Normal, favorite ? "\xE2\x98\x85" : "\xE2\x98\x86", star.x, TextY(star), selected ? p.selectText : favorite ? p.accent : p.sub, star.w, 1);
        float x = art.R() + S(8), width = std::max(0.0f, star.x - x - S(2));
        DrawText(Font::Bold, station.name, x, r.y + S(7), selected ? p.selectText : p.text, width);
        std::string tags = station.tags.substr(0, station.tags.find(','));
        std::string location = station.country + (station.country.empty() || station.state.empty() ? "" : " / ") + station.state;
        std::string detail = location + (location.empty() || tags.empty() ? "" : "  \xE2\x80\xA2  ") + tags;
        DrawText(Font::Normal, detail, x, r.y + S(27), selected ? p.selectText : p.sub, width);
        std::string format = station.codec;
        if (station.bitrate) format += (format.empty() ? "" : "  \xC2\xB7  ") + std::to_string(station.bitrate) + " kbps";
        if (station.hls) format += (format.empty() ? "" : "  \xC2\xB7  ") + std::string("HLS");
        DrawText(Font::Normal, format, x, r.y + S(45), selected ? p.selectText : p.sub, width);
    }
    SDL_SetRenderClipRect(g.renderer, nullptr);
}

void DrawVisuals(const Palette& p) {
    if (g.settings.theme != ThemeMode::Foobar) return;
    uint64_t now = SDL_GetTicks();
    if (now - g_visualAt >= ui::kFoobarFrameMs) {
        bool active = g.playing != kNone && !IsPaused();
        unsigned rate = g.engine.Rate();
        size_t frames = g_visuals.Prepare(rate);
        if (active) active = g.engine.VisualSamples(g_visuals.wave.data(), frames, g_visuals.time) == frames;
        if (g.playing == kNone || !IsPaused())
            g_visuals.Update(active, rate, g_visualAt ? (now - g_visualAt) / 1000.0 : 0.016);
        g_visualAt = now;
    }
    for (Rect r : L.visuals) { Fill(r, p.bg); Frame(r, p.border); }
    Rect bars = L.visuals[0], scope = L.visuals[1];
    for (size_t i = 0; i < g_visuals.levels.size(); ++i) {
        float w = (bars.w - S(4)) / g_visuals.levels.size(), h = (bars.h - S(4)) * g_visuals.levels[i];
        if (h > 0) Fill({bars.x + S(2) + i * w, bars.B() - S(2) - h, std::max(1.0f, w - S(1)), h}, p.text);
        float peak = (bars.h - S(4)) * g_visuals.peaks[i];
        if (peak > h) Fill({bars.x + S(2) + i * w, bars.B() - S(2) - peak, std::max(1.0f, w - S(1)), S(1)}, Mix(p.bg, p.text, 50));
    }
    int w = (int)(scope.w - S(4));
    if (w <= 1) return;
    const auto curve = g_visuals.Curve(w);
    std::vector<SDL_FPoint> line(w);
    for (int x = 0; x < w; ++x) {
        line[x] = {scope.x + S(2) + x + 0.5f, scope.y + S(2) + (scope.h - S(4)) * (0.5f - curve[x] * 0.5f)};
    }
    // SDL lines have hard edges. A transparent fringe gives this one-DIP trace the same smooth
    // profile as Windows' Direct2D trace, including fractional sample heights.
    std::vector<SDL_Vertex> vertices;
    vertices.reserve((w - 1) * 18);
    float inner = std::max(0.0f, (S(1) - 1) * 0.5f), outer = inner + 1;
    for (int x = 1; x < w; ++x) {
        SDL_FPoint a = line[x - 1], b = line[x];
        float dy = b.y - a.y, length = std::hypot(1.0f, dy), nx = -dy / length, ny = 1 / length;
        float offsets[] = {-outer, -inner, inner, outer}, alpha[] = {0, 1, 1, 0};
        SDL_Vertex edge[8];
        for (int i = 0; i < 4; ++i) {
            edge[i * 2] = {{a.x + nx * offsets[i], a.y + ny * offsets[i]}, FColor(p.text, alpha[i]), {0, 0}};
            edge[i * 2 + 1] = {{b.x + nx * offsets[i], b.y + ny * offsets[i]}, FColor(p.text, alpha[i]), {0, 0}};
        }
        for (int i = 0; i < 3; ++i) {
            int at = i * 2;
            for (int k : {at, at + 1, at + 2, at + 1, at + 3, at + 2}) vertices.push_back(edge[k]);
        }
    }
    SDL_Rect clip{(int)(scope.x + S(2)), (int)(scope.y + S(2)), w, (int)(scope.h - S(4))};
    SDL_SetRenderClipRect(g.renderer, &clip);
    SDL_RenderGeometry(g.renderer, nullptr, vertices.data(), (int)vertices.size(), nullptr, 0);
    SDL_SetRenderClipRect(g.renderer, nullptr);
}

// An album header's cover: the decoded picture, or while it loads the surface color, or for an
// album without one a tile in a color of its own with the album's initial (app_ui.cpp).
void DrawGroupCover(const Group& gr, const Rect& r, const Palette& p) {
    DrawAlbumCover(g.rows[gr.first], gr.album, g_lib.Str(gr.album), r, p);
}

void DrawGroupHeader(const Group& gr, int index, float top, float right, const Palette& p) {
    float h = L.rowH;
    if (index > 0) Fill({0, top, right, std::max(1.0f, S(1) / 2)},
                        g.settings.theme == ThemeMode::Foobar ? Mix(p.bg, p.text, 50) : p.border);
    float size = 2 * h - S(8);
    Rect cover{S(8), top + S(5), size, size};
    DrawGroupCover(gr, cover, p);
    float x = cover.R() + S(10), r = right - S(10);
    std::wstring artist = gr.artist ? g_lib.Str(gr.artist) : L"Various artists";
    std::wstring year = g_lib.Str(gr.date).substr(0, 4);
    std::string stats = std::to_string(gr.count) + " tracks  \xC2\xB7  " + Utf8(FormatDuration(gr.seconds));
    Rect line1{x, top + S(3), r - x, h - S(2)}, line2{x, top + h - S(1), r - x, h - S(2)};
    float sw = TextWidth(Font::Normal, stats);
    DrawText(Font::Normal, stats, std::max(x, r - sw), TextY(line1), p.sub);
    DrawText(Font::Bold, Utf8(g_lib.Str(gr.album)), x, TextY(line1, Font::Bold), p.text, std::max(0.0f, r - sw - S(12) - x));
    DrawText(Font::Normal, Utf8(year.empty() ? artist : artist + L"  \x00B7  " + year), x, TextY(line2), p.sub, r - x);
}

// The list (app_playlists.cpp, DrawTrackRow; app_ui.cpp, HeaderSubclass): the header on the list's
// own background with quiet labels and short dividers, then the rows: selection, hover or nothing
// behind them, and each column's text with the album header's parts left out.
void DrawList(const Palette& p) {
    Fill(L.list, p.bg);
    std::vector<float> xs;
    float x = 0;
    for (int c : g.settings.columns) {
        xs.push_back(x);
        x += ColumnWidth(c);
    }
    float total = x;
    const Playlist& pl = Active();
    uint32_t divider = Mix(p.bg, p.border, 50);
    for (size_t i = 0; i < xs.size(); ++i) {
        int c = g.settings.columns[i];
        float w = ColumnWidth(c);
        Fill({xs[i] + w - 1, L.header.y + S(6), 1, L.header.h - S(12)}, divider);
        bool sorted = pl.sortColumn == c;
        Rect tr{xs[i] + S(5), L.header.y, w - S(10) - (sorted ? S(14) : 0), L.header.h};
        std::string name = Utf8(kColumns[c].name);
        DrawText(Font::Normal, name, tr.x, TextY(tr), sorted ? p.text : p.sub, tr.w, kColumns[c].align == ui::kAlignRight ? 2 : 0);
        if (sorted) {
            float cx = xs[i] + w - S(12), cy = L.header.y + L.header.h / 2;
            if (pl.sortAscending) Polygon({{cx - S(4), cy + S(2)}, {cx, cy - S(3)}, {cx + S(4), cy + S(2)}}, p.text);
            else Polygon({{cx - S(4), cy - S(2)}, {cx + S(4), cy - S(2)}, {cx, cy + S(3)}}, p.text);
        }
    }
    Fill({0, L.header.B() - S(1), L.header.w, S(1)}, p.border);

    SDL_Rect clip{(int)L.rows.x, (int)L.rows.y, (int)L.rows.w, (int)L.rows.h};
    SDL_SetRenderClipRect(g.renderer, &clip);
    int first = (int)(g.scroll / L.rowH), last = std::min((int)g_lines.size(), first + (int)(L.rows.h / L.rowH) + 2);
    int hover = g_popups.empty() ? LineAt(g_mouseY) : -1;
    bool npTab = pl.kind == PlaylistKind::NowPlaying;
    const char* mark = g.playing != kNone && IsPaused() ? "\xE2\x9D\x99\xE2\x9D\x99" : nullptr;
    std::string playingMark = Utf8(ThemeInfo(g.settings.theme).playingMark);
    for (int line = std::max(0, first - 1); line < last; ++line) {
        const Line& ln = g_lines[line];
        float y = L.rows.y + line * L.rowH - g.scroll;
        if (ln.row < 0) {
            if (ln.part == 0) DrawGroupHeader(g_groups[ln.group], ln.group, y, std::min(total, L.rows.w), p);
            continue;
        }
        TrackId id = g.rows[ln.row];
        Rect r{0, y, total, L.rowH};
        bool sel = ln.row < (int)g.selected.size() && g.selected[ln.row];
        if (sel) Fill(r, p.select);
        else if (line == hover) Fill(r, p.hover);
        bool isPlaying = g.playing == id && npTab && ln.row < (int)g.rowItems.size() && g.rowItems[ln.row] == (size_t)g.queuePos;
        const Track& t = g_lib[id];
        uint32_t color = sel ? p.selectText : isPlaying ? p.playing : (t.flags & (Track::kMissing | Track::kFailed)) ? p.faint : p.text;
        Font font = isPlaying ? Font::Bold : Font::Normal;
        if (isPlaying && g.settings.theme == ThemeMode::Foobar) {
            Fill(r, p.playing);
            color = TextOn(p.playing);
            font = Font::Normal;
        }
        for (size_t i = 0; i < xs.size(); ++i) {
            int c = g.settings.columns[i];
            float w = ColumnWidth(c);
            Rect cell{xs[i] + S(6), y, w - S(12), L.rowH};
            if (c == 0) {
                if (isPlaying) DrawText(font, mark ? mark : playingMark, cell.x, TextY(cell, font), color, cell.w);
                else if (int queued = QueuePosition(id)) DrawText(font, "[" + std::to_string(queued) + "]", cell.x, TextY(cell, font), color, cell.w);
                continue;
            }
            if (c == ui::kColRating) {
                // Five stars: the rating's in the accent over the rest in the faint color.
                std::string star = "\xE2\x98\x85";
                float sw = TextWidth(Font::Normal, star), sx = xs[i] + S(4);
                for (int k = 0; k < 5; ++k, sx += sw) {
                    int fill = std::clamp((int)t.rating - k * 20, 0, 20);
                    DrawText(Font::Normal, star, sx, TextY(cell), sel ? (fill ? p.selectText : p.sub) : fill ? p.accent : p.faint);
                }
                continue;
            }
            if (GroupHidesCell(ln, c)) continue;
            const std::string& text = Cell(id, c);
            if (text.empty()) continue;
            DrawText(font, text, cell.x, TextY(cell, font), color, cell.w, kColumns[c].align == ui::kAlignRight ? 2 : 0);
        }
    }
    if (g_drag == Drag::Tracks && g_trackDrag.target >= 0 && !g_lineOfRow.empty()) {
        int row = std::min(g_trackDrag.target, (int)g_lineOfRow.size() - 1);
        float y = L.rows.y + (g_lineOfRow[row] + (g_trackDrag.target >= (int)g_lineOfRow.size() ? 1 : 0)) * L.rowH - g.scroll;
        Fill({L.rows.x, std::clamp(y - S(1), L.rows.y, L.rows.B() - S(2)), L.rows.w, S(2)}, p.accent);
    }
    SDL_SetRenderClipRect(g.renderer, nullptr);

    if (g.rows.empty()) {
        std::string hint = !g.search.empty()                                                  ? "No tracks match the search."
                           : pl.kind == PlaylistKind::Library && g.settings.folders.empty() ? "Your library is empty: add your music folder with Library \xE2\x96\xB8 Library folders, or drop folders here."
                           : pl.kind == PlaylistKind::Library && g.scanning                 ? "Looking for music..."
                           : pl.kind == PlaylistKind::NowPlaying                            ? "Drop music here, or press Ctrl+O to add files."
                                                                                            : "Nothing here yet.";
        DrawText(Font::Normal, hint, L.rows.x, L.rows.y + S(40), p.sub, L.rows.w, 1);
    }
    // Windows 11's overlay scroll bar at rest: a slim thumb, wider with arrows while hovered.
    if (L.scrollbar.w > 0) {
        Fill(L.scrollbar, p.bg);
        bool hot = L.scrollbar.In(g_mouseX, g_mouseY) || g_drag == Drag::Scroll;
        float thumbH = std::max(S(24), L.scrollbar.h * L.rows.h / std::max(1.0f, ContentHeight()));
        float room = L.scrollbar.h - S(hot ? 34 : 4) - thumbH;
        float thumbY = L.scrollbar.y + S(hot ? 17 : 2) + room * (MaxScroll() > 0 ? g.scroll / MaxScroll() : 0);
        float tw = hot ? S(6) : S(2);
        FillRound({std::round(L.scrollbar.x + L.scrollbar.w / 2 - tw / 2), thumbY, tw, thumbH}, tw / 2, hot ? p.sub : p.faint);
        if (hot) {
            float cx = L.scrollbar.x + L.scrollbar.w / 2;
            Polygon({{cx - S(4), L.scrollbar.y + S(10)}, {cx, L.scrollbar.y + S(6)}, {cx + S(4), L.scrollbar.y + S(10)}}, p.sub);
            Polygon({{cx - S(4), L.scrollbar.B() - S(10)}, {cx + S(4), L.scrollbar.B() - S(10)}, {cx, L.scrollbar.B() - S(6)}}, p.sub);
        }
    }
}

std::wstring StatusMessage() {
    if (!g.message.empty() && SDL_GetTicks() < g.messageUntil) return ThemeInfo(g.settings.theme).messagePrefix + g.message;
    if (RadioPlaying()) return IsPaused() ? L"Radio paused." : L"Live radio: " + FromUtf8(InternetRadio().playing.name);
    if (g.playing == kNone) return L"Stopped.";
    std::wstring status = (IsPaused() ? L"Paused | " : L"") +
        FormatTitle(EffectiveFormat(kPackStatus, g.settings.fmtStatus), Lookup(g.playing, true));
    if (g.speed != 1 || g.tempo != 1) {
        wchar_t rate[64];
        if (g.speed != 1 && g.tempo != 1) swprintf(rate, std::size(rate), L" | %gx speed, %gx tempo", g.speed, g.tempo);
        else swprintf(rate, std::size(rate), L" | %gx %ls", g.speed != 1 ? g.speed : g.tempo, g.speed != 1 ? L"speed" : L"tempo");
        status += rate;
    }
    if (g.stopAfterCurrent) status += L" | stop after current";
    if (!g.queue.empty()) status += L"  \x2022  " + std::to_wstring(g.queue.size()) + L" queued";
    return status;
}

// The status bar (app_window.cpp, DrawMainItem): three parts on the surface split by hairlines:
// the message or what's playing, work in progress, and the count and length of the list or selection.
void DrawStatus(const Palette& p) {
    if (L.status.h <= 0) return;
    Fill(L.status, p.surface);
    float W = L.status.w;
    float parts[] = {0, std::max(S(100), W - S(ui::kStatusDetails)), std::max(S(200), W - S(ui::kStatusCounts)), W};
    std::wstring progress = TagWriteStatus();
    if (progress.empty() && g_checkingDead) progress = L"Checking for missing files...";
    if (progress.empty() && g_checkingDuplicates) progress = L"Finding duplicate audio...";
    if (progress.empty() && g.scanning) progress = L"Adding audio files...";
    if (progress.empty()) {
        std::lock_guard lock(g.tagMx);
        if (!g.tagQueue.empty()) progress = L"Reading metadata: " + std::to_wstring(g.tagQueue.size()) + L" remaining";
    }
    if (progress.empty()) progress = ReplayGainStatus();
    double total = 0;
    size_t sel = 0;
    for (size_t i = 0; i < g.rows.size(); ++i)
        if (i < g.selected.size() && g.selected[i]) {
            ++sel;
            total += g_lib[g.rows[i]].Duration();
        }
    std::wstring counts;
    if (InternetRadio().active) counts = std::to_wstring(InternetRadio().rows.size()) + L" stations";
    else if (sel) counts = std::to_wstring(sel) + L" selected  \x2022  " + FormatTotalDuration(total);
    else {
        for (TrackId id : g.rows) total += g_lib[id].Duration();
        counts = std::to_wstring(g.rows.size()) + (g.search.empty() ? L"" : L" of " + std::to_wstring(g_sourceCount)) +
                 L" tracks  \x2022  " + FormatTotalDuration(total);
    }
    std::wstring text[] = {StatusMessage(), progress, counts};
    for (int i = 0; i < 3; ++i) {
        Rect r{parts[i], L.status.y, parts[i + 1] - parts[i], L.status.h};
        if (i > 0) Fill({r.x, r.y + S(3), S(1), r.h - S(6)}, p.border);
        DrawText(Font::Normal, Utf8(text[i]), r.x + S(6), TextY(r), i == 0 ? p.text : p.sub, r.w - S(10));
    }
}

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

// --- input ---------------------------------------------------------------------------------------------------
float Density() { return SDL_GetWindowPixelDensity(g.window); }

double SeekValueAt(float x) {
    float inset = S(8);
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

void SectionAction(int action) {
    double duration = TrackDuration();
    if (g.playing != g_sectionMenuTrack || !g_lib.Contains(g.playing) || duration <= 0) return;
    double position = std::clamp(g_sectionMenuPos, 0.0, duration);
    if (action == kSectionPlay) { SeekTo(position); return; }
    PlaybackSection section = SectionOf(g.playing);
    if (action == kSectionLoop) {
        if (section.end - section.start <= 0.05) {
            double end = g.chapterTrack == g.playing ? ChapterStartAfter(g.chapters, position) : 0;
            if (end <= position + 0.5) end = position + 30;
            section = {position, std::min(end, duration), false};
        }
        if (section.end - section.start <= 0.05) { ShowMessage(L"Choose a point before the end of the track."); return; }
        section.on = true;
    } else if (action == kSectionStart) {
        section.start = position;
        if (section.end <= position + 0.05) section.end = std::min(position + 30, duration);
        section.on = false;
    } else if (action == kSectionEnd) {
        section.end = position;
        if (section.start >= position - 0.05) section.start = std::max(0.0, position - 30);
        section.on = false;
    } else if (action == kSectionClear) section = {};
    else return;
    SetSectionFor(g.playing, section);
    double seek = g.engine.Position();
    if (section.on && seek < section.start) seek = section.start;
    SeekTo(seek);
    ShowMessage(section.on ? L"Looping the section." : action == kSectionClear ? L"Section cleared." : L"Section marked.");
}

float VolumeAt(float x) { return std::clamp((x - L.volume.x - S(8)) / std::max(1.0f, L.volume.w - S(16)), 0.0f, 1.0f); }

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
            radio.filterFocus = 0; radio.discover = true; RadioSearch();
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

void Draw() {
    ++g_frame;
    // The library changed (a scan, tags arriving): rebuild what's shown, at most twice a second.
    if (g.rowsRevision != g_lib.Revision() && SDL_GetTicks() - g_lastRebuild > 500) RebuildRows();
    if (g.chapterTrack != g.playing) QueueChapters(g.playing);
    TrackId shown = g.playing != kNone ? g.playing : g.settings.resumeOn ? g.resumeTrack : kNone;
    if (g_lib.Contains(shown) && g_coverWanted != shown) {
        g_coverWanted = shown;
        RequestCover(shown);
    }
    if (g.playing != g_wave.track) {  // a new track: drop the old peaks and ask for the new ones
        CancelPeaks();
        g_wave.track = g.playing;
        g_wave.request = 0;
        g_wave.ready = g.playing != kNone && RequestPeaks(g.playing);
    }
    UpdateWindowTitle();
    const Palette& p = CurrentPalette();
    if (MiniActive()) {
        DrawMini(p);
        return;
    }
    LyricsTick();
    DoLayout();
    ClampScroll();
    SetColor(p.bg);
    SDL_RenderClear(g.renderer);
    if (InternetRadio().active) DrawRadio(p);
    else if (CanvasActive()) DrawCanvas(L.list, p, g_popups.empty() ? g_mouseX : -1, g_popups.empty() ? g_mouseY : -1);
    else DrawList(p);
    if (L.browser.h > 0) {
        DrawBrowser(L.browser, p, g_popups.empty() ? g_mouseX : -1, g_popups.empty() ? g_mouseY : -1);
        Fill(L.splitter, p.surface);
    }
    if (L.lyrics.w > 0) DrawLyrics(L.lyrics, p);
    DrawTabs(p);
    DrawBand(p);
    DrawVisuals(p);
    DrawStatus(p);
    DrawMenuBar(p);
    if (g_folders.open) DrawFoldersDialog(p);
    if (g_aboutOpen) DrawAbout(p);
    if (g_web.open) DrawWeb(p);
    if (PanelOpen()) DrawPanel(p, g_mouseX, g_mouseY);
    if (g_exclude.open) DrawExcludeConfirm(p);
    if (g.settings.pointsChip) {
        StepPointsChip();
        DrawToast(L.chip, p);
    }
    DrawStartupReview(p, g_popups.empty() ? g_mouseX : -1, g_popups.empty() ? g_mouseY : -1);
    DrawPopups(p);
}

}  // namespace

// --- public ------------------------------------------------------------------------------------------------
namespace {
std::vector<MenuItem> g_paletteItems;  // the items MenuCommands listed, for RunMenuCommand

void FlattenMenu(const std::vector<MenuItem>& items, const std::string& path, std::vector<PaletteCommand>& out) {
    for (const MenuItem& item : items) {
        if (item.separator) continue;
        if (!item.sub.empty()) {
            FlattenMenu(item.sub, path.empty() ? item.label : path + " \xE2\x80\xBA " + item.label, out);
        } else if (item.action != kNoAction && item.action != kPalette) {
            out.push_back({item.label, path, item.shortcut, item.enabled, item.checked});
            g_paletteItems.push_back(item);
        }
    }
}
}  // namespace

std::vector<PaletteCommand> MenuCommands() {
    std::vector<PaletteCommand> out;
    g_paletteItems.clear();
    for (int m : BarMenus()) FlattenMenu(BarMenu(m), Plain(ui::kMenuBar[m]), out);
    return out;
}

void RunMenuCommand(size_t index) {
    if (index < g_paletteItems.size()) RunAction(g_paletteItems[index]);
}

uint64_t LastInputTicks() { return g_lastInput; }

SDL_Texture* MiniCover() {
    TrackId shown = g.playing != kNone ? g.playing : g.settings.resumeOn ? g.resumeTrack : kNone;
    return g_lib.Contains(shown) && g_cover && g_coverFor == shown ? g_cover : nullptr;
}

std::wstring FormatForTrack(const std::wstring& pattern, TrackId id, bool withPlayback) {
    return FormatTitle(pattern, Lookup(id, withPlayback));
}

// The window title follows the playing track (the pattern has no time fields, so it changes only with it).
void UpdateWindowTitle() {
    static TrackId shown = kNone - 1;
    static uint32_t revision = 0;
    static std::wstring pattern;
    static std::string title;
    if (!g.window) return;
    if (RadioPlaying()) {
        shown = kNone - 1;
        std::string next = InternetRadio().playing.name + " - cordinal";
        if (next != title) SDL_SetWindowTitle(g.window, (title = next).c_str());
        return;
    }
    if (shown == g.playing && revision == g_lib.Revision() && pattern == EffectiveFormat(kPackWindow, g.settings.fmtWindow)) return;
    shown = g.playing;
    revision = g_lib.Revision();
    pattern = EffectiveFormat(kPackWindow, g.settings.fmtWindow);
    std::string next = "cordinal";
    if (g.playing != kNone && g_lib.Contains(g.playing)) {
        std::string text = Utf8(FormatTitle(pattern, Lookup(g.playing, true)));
        if (!text.empty()) next = text;
    }
    if (next != title) SDL_SetWindowTitle(g.window, (title = next).c_str());
}

void MaybeOfferSteamSoundtracks() {
    if (g.settings.steamOffered || g_steamOfferWorker.joinable()) return;
    auto request = std::make_unique<PlaylistFile>();
    request->watchedFolders = g.settings.folders;
    g_steamOfferCancel = false;
    g_steamOfferWorker = std::thread([request = std::move(request)]() mutable {
        request->paths = NewSteamFolders(request->watchedFolders, g_steamOfferCancel);
        request->cancelled = g_steamOfferCancel;
        auto* result = request.release();
        if (!PostMessageW(nullptr, WM_APP_PLAYLIST_FILE, 2, (LPARAM)result)) delete result;
    });
}

void UpdateInboxTab() {
    size_t count = 0;
    for (TrackId id = 0; id < (TrackId)g_lib.Count(); ++id)
        if ((g_lib[id].flags & (Track::kInbox | Track::kMissing)) == Track::kInbox) ++count;
    auto it = std::find_if(g.playlists.begin(), g.playlists.end(), [](const Playlist& p) { return p.kind == PlaylistKind::Inbox; });
    int index = it == g.playlists.end() ? -1 : (int)(it - g.playlists.begin());
    if (count && index < 0) {
        CloseMenus();
        index = std::min(1, (int)g.playlists.size());
        g.playlists.insert(g.playlists.begin() + index, Playlist{L"Inbox", {}, PlaylistKind::Inbox});
        if (g.active >= index) ++g.active;
        if (g_renameIndex >= index) ++g_renameIndex;
        for (auto& closed : g_closedPlaylists) if (closed.index >= index) ++closed.index;
        g.libraryDirty = true;
    } else if (!count && index >= 0 && index != g.active) {
        CloseMenus();
        g.playlists.erase(g.playlists.begin() + index);
        if (g.active > index) --g.active;
        if (g_renameIndex > index) --g_renameIndex;
        for (auto& closed : g_closedPlaylists) if (closed.index > index) --closed.index;
        g.libraryDirty = true;
        index = -1;
    }
    if (index >= 0) g.playlists[index].name = count ? L"Inbox (" + std::to_wstring(count) + L")" : L"Inbox";
    g.redraw = true;
}

void OpenLibraryFolders() {
    g_folders.open = true;
    g.redraw = true;
}

void OpenWebServer() {
    g_web.open = true;
    g_web.port = std::to_string(g.settings.webPort);
    g_web.fixed = g.settings.webCode;
    g_web.focus = 1;
    g.redraw = true;
}

const Palette& CurrentPalette() {
    if (g.settings.theme == ThemeMode::System) return SDL_GetSystemTheme() == SDL_SYSTEM_THEME_DARK ? DarkPalette() : LightPalette();
    return ThemePalette(g.settings.theme);
}

void ApplyTheme() {
    InitText(g.renderer, g.scale, ThemeInfo(g.settings.theme));
    g.engine.SetScope(false);
    g_visuals = {};
    g.redraw = true;
}

bool UiAnimating() {
    return ((LiveWaveformShown() || LiveFountainShown()) && (g.playing != kNone || RadioPlaying()) &&
            !IsPaused() && L.seek.w > 0) ||
           (MiniActive() && g.playing != kNone && !IsPaused()) || PointsChipAnimating() || LyricsAnimating() ||
           GamesAnimating() || (g.settings.theme == ThemeMode::Foobar &&
           (g.playing != kNone ? !IsPaused() : g_visuals.Moving()));
}

void ShowMessage(const std::wstring& text) {
    g.message = text;
    g.messageUntil = SDL_GetTicks() + 6000;
    g.redraw = true;
    MessageBlip();
}

bool Confirm(const std::string& question, const char* title) {
    SDL_MessageBoxButtonData buttons[] = {{SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "OK"},
                                          {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel"}};
    SDL_MessageBoxData dialog{SDL_MESSAGEBOX_INFORMATION, g.window, title, question.c_str(), 2, buttons, nullptr};
    int answer = 0;
    SDL_ShowMessageBox(&dialog, &answer);
    return answer == 1;
}

void OnPlaylistFile(WPARAM kind, LPARAM value) {
    std::unique_ptr<PlaylistFile> request((PlaylistFile*)value);
    if (g_playlistFileStopping) return;
    if (kind == 2) {
        if (g_steamOfferWorker.joinable()) g_steamOfferWorker.join();
        if (!request->cancelled && !g.settings.steamOffered && request->watchedFolders == g.settings.folders &&
            !request->paths.empty()) {
            g.settings.steamOffered = true;
            SaveSettings();
            ShowMessage(L"Found Steam soundtracks: Library \x25B8 Add Steam soundtracks adds them to your library.");
        }
        return;
    }
    if (!kind) {
        if (request->failed) { ShowMessage(L"Couldn't open the playlist file dialog."); return; }
        if (g_playlistFileBusy || g_playlistImport) { ShowMessage(L"A playlist file is already being processed."); return; }
        if (g_playlistFileWorker.joinable()) g_playlistFileWorker.join();
        g_playlistFileBusy = true;
        g_checkingDead = request->checkDead;
        g_checkingDuplicates = request->findDuplicates;
        g_checkingSteam = request->findSteam;
        g_playlistFileCancel = false;
        ShowMessage(request->checkDead ? L"Checking for missing files..." : request->findDuplicates ? L"Finding duplicate audio..." :
                    request->findSteam ? L"Looking for Steam soundtracks..." :
                    request->addToExisting ? L"Finding audio files..." :
                    request->save ? L"Saving playlist..." : L"Opening playlist...");
        g_playlistFileWorker = std::thread([request = std::move(request)]() mutable {
            if (request->findSteam) {
                request->paths = NewSteamFolders(request->watchedFolders, g_playlistFileCancel);
                request->cancelled = g_playlistFileCancel;
                request->ok = true;
            } else if (request->findDuplicates) {
                request->duplicateGroups = GroupDuplicateAudio(std::move(request->hashes));
                request->cancelled = g_playlistFileCancel;
                request->ok = true;
            } else if (request->checkDead) {
                for (const auto& [id, path] : request->checkedPaths) {
                    if (g_playlistFileCancel) { request->cancelled = true; break; }
                    if (GetFileAttributesW(ContainerOf(AudioFileOf(path)).c_str()) == INVALID_FILE_ATTRIBUTES) {
                        DWORD error = GetLastError();
                        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) request->missing.push_back({id, path});
                    }
                }
                if (g_playlistFileCancel) request->cancelled = true;
                request->ok = true;
            } else if (request->addToExisting) {
                for (const auto& path : request->inputs) {
                    if (g_playlistFileCancel) { request->cancelled = true; break; }
                    if (!CollectAudioFiles(path, request->paths, &g_playlistFileCancel)) ++request->failedInputs;
                }
                request->ok = !request->paths.empty() || !request->failedInputs;
            } else request->ok = request->save ? ExportM3U(request->path, request->entries) : ImportM3U(request->path, request->paths);
            auto* result = request.release();
            if (!PostMessageW(nullptr, WM_APP_PLAYLIST_FILE, 1, (LPARAM)result)) delete result;
        });
        return;
    }
    if (g_playlistFileWorker.joinable()) g_playlistFileWorker.join();
    g_playlistFileBusy = false;
    g_checkingDead = false;
    g_checkingDuplicates = false;
    g_checkingSteam = false;
    if ((request->checkDead || request->findDuplicates || request->findSteam) && g_playlistFileCancel) request->cancelled = true;
    if (request->cancelled) { ShowMessage(L"Search or import cancelled."); return; }
    if (!request->ok) {
        ShowMessage(request->addToExisting ? L"Couldn't read the selected files or folders." :
                    request->save ? L"Couldn't save the playlist. The original file was kept." : L"Couldn't open the playlist file.");
        return;
    }
    if (request->save) { ShowMessage(L"Playlist saved."); return; }
    if (request->findSteam) {
        if (request->watchedFolders != g.settings.folders) {
            ShowMessage(L"Library folders changed while searching for Steam soundtracks. Try again.");
            return;
        }
        g.settings.steamOffered = true;
        size_t count = request->paths.size();
        if (!count) { SaveSettings(); ShowMessage(L"No new Steam soundtrack folders found."); return; }
        g.settings.folders.insert(g.settings.folders.end(), request->paths.begin(), request->paths.end());
        SaveSettings();
        StartScanner();
        ShowMessage(L"Added " + std::to_wstring(count) + (count == 1 ? L" Steam soundtrack folder." : L" Steam soundtrack folders."));
        return;
    }
    if (request->findDuplicates) {
        std::vector<TrackId> tracks;
        for (const auto& group : request->duplicateGroups) {
            uint64_t hash = 0;
            for (TrackId id : group) {
                if (!g_lib.Contains(id) || !g_lib[id].audioHash || (g_lib[id].flags & Track::kMissing) ||
                    !(g_lib[id].flags & (Track::kInLibrary | Track::kInbox)) || (hash && hash != g_lib[id].audioHash)) {
                    ShowMessage(L"The library changed while finding duplicates. Try again.");
                    return;
                }
                hash = g_lib[id].audioHash;
                tracks.push_back(id);
            }
        }
        if (tracks.empty()) { ShowMessage(L"No duplicate audio found. cordinal recognizes identical audio in FLAC files."); return; }
        size_t groups = request->duplicateGroups.size();
        g.playlists.push_back(Playlist{UniquePlaylistName(g.playlists, L"Duplicate audio"), std::move(tracks), PlaylistKind::Static});
        g.libraryDirty = true;
        SetActiveTab((int)g.playlists.size() - 1);
        ShowMessage(std::to_wstring(groups) + (groups == 1 ? L" song is" : L" songs are") + L" in the library more than once.");
        return;
    }
    if (request->checkDead) {
        auto target = std::find_if(g.playlists.begin(), g.playlists.end(), [&](const Playlist& p) { return p.identity == request->target; });
        bool unchanged = target != g.playlists.end() && target->items == request->snapshot;
        for (const auto& [id, path] : request->checkedPaths)
            if (!g_lib.Contains(id) || g_lib.Path(id) != path) unchanged = false;
        if (!unchanged) { ShowMessage(L"The playlist changed while checking; no tracks were removed. Try again."); return; }
        std::unordered_set<TrackId> missing;
        for (const auto& [id, path] : request->missing) missing.insert(id);
        std::vector<char> remove(target->items.size(), 0);
        for (size_t i = 0; i < remove.size(); ++i) remove[i] = missing.contains(target->items[i]);
        size_t count = RemoveOccurrences(remove, (int)(target - g.playlists.begin()));
        ShowMessage(L"Removed " + std::to_wstring(count) + L" dead item(s).");
        return;
    }
    if (request->addToExisting) { g_playlistImport = std::move(request); return; }
    Playlist playlist{UniquePlaylistName(g.playlists, StemOf(request->path)), {}, PlaylistKind::Static};
    playlist.identity = g_nextPlaylistIdentity++;
    request->target = playlist.identity;
    g.playlists.push_back(std::move(playlist));
    g.libraryDirty = true;
    SetActiveTab((int)g.playlists.size() - 1);
    g_playlistImport = std::move(request);
}

bool PlaylistImportPending() { return g_playlistImport != nullptr; }

void TickPlaylistImport() {
    if (!g_playlistImport) return;
    auto& request = *g_playlistImport;
    auto found = std::find_if(g.playlists.begin(), g.playlists.end(), [&](const Playlist& p) { return p.identity == request.target; });
    if (found == g.playlists.end()) {
        g_playlistImport.reset();
        ShowMessage(L"Playlist import stopped because its tab was closed.");
        return;
    }
    size_t end = std::min(request.paths.size(), request.done + 256);
    std::vector<TrackId> unread;
    std::vector<TrackId> joined;
    bool inboxChanged = false;
    for (; request.done < end; ++request.done) {
        TrackId id = g_lib.Add(request.paths[request.done]);
        if (id == kInvalidTrack) continue;
        if (found->kind == PlaylistKind::Library) {
            Track& t = g_lib[id];
            inboxChanged |= (t.flags & Track::kInbox) != 0;
            t.flags = (uint16_t)((t.flags | Track::kInLibrary) & ~(Track::kExcluded | Track::kMissing | Track::kInbox));
            if (!(t.flags & Track::kCollected)) joined.push_back(id);
            g_lib.Touch();
        } else found->items.push_back(id);
        ++request.added;
        if (!g_lib[id].Loaded()) unread.push_back(id);
    }
    QueueTags(unread);
    AnnouncePoints(AwardLibraryAdds(joined));
    g.libraryDirty = true;
    if (request.done == request.paths.size()) {
        size_t count = request.added, failed = request.failedInputs;
        bool added = request.addToExisting;
        if ((int)(found - g.playlists.begin()) == g.active) RebuildRows();
        g_playlistImport.reset();
        ShowMessage((added ? L"Added " : L"Opened playlist with ") + std::to_wstring(count) +
                    (count == 1 ? L" track." : L" tracks.") +
                    (failed ? L" Couldn't read " + std::to_wstring(failed) + L" input(s)." : L""));
    } else if (request.done % 4096 == 0) {
        ShowMessage(L"Opening playlist: " + std::to_wstring(request.done) + L" of " + std::to_wstring(request.paths.size()) + L" tracks.");
    }
    if (inboxChanged) UpdateInboxTab();
}

void StopPlaylistFiles() {
    g_playlistFileStopping = true;
    g_playlistFileCancel = true;
    g_steamOfferCancel = true;
    if (g_playlistFileWorker.joinable()) g_playlistFileWorker.join();
    if (g_steamOfferWorker.joinable()) g_steamOfferWorker.join();
    g_playlistImport.reset();
}

void RebuildRows() {
    if (g.playlists.empty()) return;
    g.active = std::clamp(g.active, 0, (int)g.playlists.size() - 1);
    Playlist& pl = Active();
    std::vector<TrackId> source = PlaylistTracks(pl);
    if (pl.kind == PlaylistKind::Library) FilterLibraryTracks(source);  // the column browser, before the search
    g_sourceCount = source.size();
    // Use the Windows search matcher, including containing-folder paths and quoted phrases.
    const auto words = FilterWords(g.search);
    TrackMatcher matcher(g_lib, words);
    std::vector<size_t> occurrences;
    occurrences.reserve(source.size());
    for (size_t i = 0; i < source.size(); ++i)
        if (source[i] < g_lib.Count() && matcher.Matches(source[i])) occurrences.push_back(i);
    // A column's sort: its sort format compared naturally (ui_spec.h, as on Windows).
    if (pl.sortColumn > 0 && pl.sortColumn < ui::kColumnCount) {
        const wchar_t* format = kColumns[pl.sortColumn].sort;
        std::unordered_map<TrackId, std::wstring> keys;
        for (size_t i : occurrences) keys.emplace(source[i], FormatTitle(format, Lookup(source[i], false)));
        bool asc = pl.sortAscending;
        std::stable_sort(occurrences.begin(), occurrences.end(), [&](size_t a, size_t b) {
            int r = CompareNatural(keys[source[a]], keys[source[b]]);
            return asc ? r < 0 : r > 0;
        });
    }
    // Editable playlists distinguish duplicate occurrences; library views use track IDs.
    std::vector<size_t> sel;
    bool editable = pl.Editable() && g.rowItems.size() == g.rows.size();
    if (g.selected.size() == g.rows.size())
        for (size_t i = 0; i < g.rows.size(); ++i)
            if (g.selected[i]) sel.push_back(editable ? g.rowItems[i] : g.rows[i]);
    size_t focusKey = g.focus >= 0 && g.focus < (int)g.rows.size() ?
                      (editable ? g.rowItems[g.focus] : g.rows[g.focus]) : SIZE_MAX;
    std::sort(sel.begin(), sel.end());
    g.rows.clear();
    g.rows.reserve(occurrences.size());
    for (size_t i : occurrences) g.rows.push_back(source[i]);
    g.rowItems.swap(occurrences);
    g.selected.assign(g.rows.size(), 0);
    g.focus = -1;
    for (size_t i = 0; i < g.rows.size(); ++i) {
        size_t key = pl.Editable() ? g.rowItems[i] : g.rows[i];
        if (std::binary_search(sel.begin(), sel.end(), key)) g.selected[i] = 1;
        if (key == focusKey && g.focus < 0) g.focus = (int)i;
    }
    if (g.anchor >= (int)g.rows.size()) g.anchor = -1;
    BuildLines();
    CanvasRebuild();
    g.rowsRevision = g_lib.Revision();
    g_lastRebuild = SDL_GetTicks();
    ClampScroll();
    g.redraw = true;
}

void SetActiveTab(int tab) {
    if (tab < 0 || tab >= (int)g.playlists.size()) return;
    LeaveRadio();
    bool leavingInbox = !g.playlists.empty() && Active().kind == PlaylistKind::Inbox && tab != g.active;
    if (g_drag == Drag::Tracks) SDL_CaptureMouse(false);
    if (g_drag == Drag::Tracks) g_drag = Drag::None;
    g_trackDrag = {};
    EndRename(true);
    g.active = tab;
    if (leavingInbox) UpdateInboxTab();
    g.search.clear();
    g.selected.clear();
    g.rows.clear();
    g.rowItems.clear();
    g.focus = g.anchor = -1;
    g.scroll = 0;
    RebuildRows();
    if (Active().kind == PlaylistKind::NowPlaying && g.queuePos >= 0)
        for (size_t i = 0; i < g.rowItems.size(); ++i)
            if (g.rowItems[i] == (size_t)g.queuePos) { EnsureVisible((int)i); break; }
}

void InitUi() { UpdateInboxTab(); RebuildRows(); }

void ShutdownUi() {
    for (auto& [key, texture] : g_radioArt) if (texture) SDL_DestroyTexture(texture);
    g_radioArt.clear();
    if (g_cover) SDL_DestroyTexture(g_cover);
    g_cover = nullptr;
    for (auto& [key, t] : g_thumbs)
        if (t.texture) SDL_DestroyTexture(t.texture);
    g_thumbs.clear();
}

void OnScaleChanged() {
    g.scale = SDL_GetWindowDisplayScale(g.window);
    if (g.scale <= 0) g.scale = 1;
    ApplyTheme();
}

void RefreshTrackArt(TrackId id) {
    if (g_coverWanted == id) g_coverWanted = kNone;
    if (g_wave.track == id) g_wave.track = kNone;
    g.redraw = true;
}

void SetCover(CoverImage* image) {
    std::unique_ptr<CoverImage> owned(image);
    if (image->id != g_coverWanted) return;
    if (g_cover) SDL_DestroyTexture(g_cover);
    g_cover = nullptr;
    g_coverFor = image->id;
    MprisSetArt(image->id, image->file);
    if (image->width > 0) {
        g_cover = SDL_CreateTexture(g.renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, image->width, image->height);
        if (g_cover) {
            SDL_UpdateTexture(g_cover, nullptr, image->rgba.data(), image->width * 4);
            SDL_SetTextureScaleMode(g_cover, SDL_SCALEMODE_LINEAR);
        }
    }
    g.redraw = true;
}

void SetThumb(CoverImage* image) {
    std::unique_ptr<CoverImage> owned(image);
    auto f = g_thumbFor.find(image->id);
    if (f == g_thumbFor.end()) return;
    uint64_t key = f->second;
    g_thumbFor.erase(f);
    --g_thumbsPending;
    auto it = g_thumbs.find(key);
    if (it == g_thumbs.end()) return;
    it->second.done = true;
    if (image->width > 0) {
        SDL_Texture* t = SDL_CreateTexture(g.renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, image->width, image->height);
        if (t) {
            SDL_UpdateTexture(t, nullptr, image->rgba.data(), image->width * 4);
            SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
            it->second.texture = t;
        }
    }
    g.redraw = true;
}

void OnFileChosen(int purpose, std::wstring* path) {
    std::unique_ptr<std::wstring> owned(path);
    PanelFileChosen(purpose, *path);
}

void OnFoldersChosen(std::vector<std::wstring>* folders) {
    std::unique_ptr<std::vector<std::wstring>> owned(folders);
    for (auto& f : *folders) AddFolder(f);
    g.redraw = true;
}

void Render() {
    Draw();
    SDL_RenderPresent(g.renderer);
    g.redraw = false;
}

void HandleEvent(const SDL_Event& e) {
    switch (e.type) {
        case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: case SDL_EVENT_MOUSE_WHEEL:
        case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: g_lastInput = SDL_GetTicks(); break;
        default: break;
    }
    if (MiniActive() && MiniEvent(e)) return;
    switch (e.type) {
        case SDL_EVENT_MOUSE_BUTTON_DOWN: MouseDown(e.button); break;
        case SDL_EVENT_MOUSE_BUTTON_UP: MouseUp(e.button); break;
        case SDL_EVENT_MOUSE_MOTION: MouseMove(e.motion); break;
        case SDL_EVENT_MOUSE_WHEEL: Wheel(e.wheel); break;
        case SDL_EVENT_KEY_DOWN: Key(e.key); break;
        case SDL_EVENT_KEY_UP: KeyUp(e.key); break;
        case SDL_EVENT_TEXT_INPUT: TextInput(e.text); break;
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
            g_mouseX = g_mouseY = -1;
            g.redraw = true;
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            CloseMenus();
            break;
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED: OnScaleChanged(); break;
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_EXPOSED:
        case SDL_EVENT_SYSTEM_THEME_CHANGED: g.redraw = true; break;
        case SDL_EVENT_DROP_FILE:
            if (e.drop.data) {
                static uint64_t lastDrop = 0;  // a drop of many files arrives as many events: one plop
                if (SDL_GetTicks() - lastDrop > 500) PlaySfx(Sfx::Drop, 1);
                lastDrop = SDL_GetTicks();
                ExpandPaths({compat::Widen(e.drop.data)}, Opening::Drop);
            }
            break;
        default: break;
    }
}

// --- the parity checks ---------------------------------------------------------------------------------------------
int CheckLayout() {
    int pw = 0, ph = 0;
    SDL_GetRenderOutputSize(g.renderer, &pw, &ph);
    // ui::kLayout is the Tracks view without the browser.
    g.settings.libraryMode = LibraryMode::Tracks;
    g.settings.browserVisible = false;
    DoLayout();
    float top = L.menuBar.B();
    const struct {
        const char* name;
        Rect r;
    } parts[] = {{"previous", L.buttons[kBtnPrev]}, {"play", L.buttons[kBtnPlay]}, {"next", L.buttons[kBtnNext]},
                 {"shuffle", L.buttons[kBtnShuffle]}, {"repeat", L.buttons[kBtnRepeat]}, {"resume", L.buttons[kBtnResume]},
                 {"elapsed", L.elapsed}, {"seek", L.seek}, {"remaining", L.remaining}, {"speaker", L.speaker},
                 {"volume", L.volume}, {"cover", L.cover}, {"title", L.title}, {"subtitle", L.subtitle},
                 {"tabs", L.tabs}, {"radio_tab", L.radioTab}, {"search", L.search}, {"list", L.list}, {"status", L.status}};
    int off = 0;
    if (!UiSymbolsAvailable()) {
        fprintf(stderr, "FAIL fonts: rating stars or the auto-playlist symbol have no glyph\n");
        ++off;
    }
    if (pw != ui::kLayoutWidth || ph != ui::kLayoutHeight + ui::kMenuBarHeight || L.menuBar.h != ui::kMenuBarHeight) {
        printf("FAIL window: %dx%d, menu bar %g (want %dx%d, %d)\n", pw, ph, L.menuBar.h, ui::kLayoutWidth,
               ui::kLayoutHeight + ui::kMenuBarHeight, ui::kMenuBarHeight);
        ++off;
    }
    for (const ui::LayoutRect& want : ui::kLayout) {
        const Rect* got = nullptr;
        for (auto& p : parts)
            if (!strcmp(p.name, want.name)) got = &p.r;
        if (!got) {
            printf("FAIL %s: not laid out\n", want.name);
            ++off;
            continue;
        }
        int x = (int)got->x, y = (int)(got->y - top), w = (int)got->w, h = (int)got->h;
        bool ok = x == want.x && y == want.y && w == want.w && h == want.h;
        printf("%s %s: %d,%d %dx%d", ok ? "ok  " : "FAIL", want.name, x, y, w, h);
        if (!ok) printf(" (Windows: %d,%d %dx%d)", want.x, want.y, want.w, want.h);
        printf("\n");
        off += !ok;
    }
    return off;
}

bool UiBusy() { return g_thumbsPending > 0 || (g_wave.track != kNone && !g_wave.ready && g_wave.request); }

void OnPeaksReady(uint32_t request) {
    if (g_wave.track == kNone || g_wave.ready || request != g_wave.request) return;
    g_wave.request = 0;
    g_wave.ready = RequestPeaks(g_wave.track);
    g.redraw = true;
}

int CaptureUi(const std::string& dir) {
    // The Windows references show the Library's Tracks view with the first track playing from
    // Now Playing; without an audio device here it is shown as playing without being heard.
    int np = NowPlayingIndex();
    if (np >= 0 && g.playing == kNone && !g.rows.empty()) {
        g.playlists[np].items = g.rows;
        g.queuePos = 0;
        g.playing = g.rows[0];
    }
    // The theme images show the Tracks view without the browser; the view images set their own.
    auto view = [](LibraryMode mode, bool browser) {
        g.settings.libraryMode = mode;
        g.settings.browserVisible = browser;
        RebuildRows();
    };
    view(LibraryMode::Tracks, false);
    SetActiveTab(0);
    int failures = 0;
    auto save = [&](const std::string& name) {
        // Covers asked for by this frame arrive as messages; the caller pumps them (CapturePump).
        Draw();
        SDL_Surface* s = SDL_RenderReadPixels(g.renderer, nullptr);
        std::string path = dir + "/" + name + ".png";
        if (!s || !SDL_SavePNG(s, path.c_str())) {
            fprintf(stderr, "capture: %s: %s\n", path.c_str(), SDL_GetError());
            ++failures;
        } else {
            printf("captured %s\n", path.c_str());
        }
        if (s) SDL_DestroySurface(s);
    };
    static const char* names[] = {"light", "dark", "deltarune", "foobar2000", "paper", "midnight", "terminal", "apollo"};
    static_assert(std::size(names) == (size_t)ThemeMode::Count - 1);
    for (int t = (int)ThemeMode::Light; t < (int)ThemeMode::Count; ++t) {
        std::string name = names[t - 1];
        g.settings.theme = (ThemeMode)t;
        ApplyTheme();
        CloseMenus();
        if (!UiSymbolsAvailable()) {
            fprintf(stderr, "capture: %s: missing rating or auto-playlist glyphs\n", name.c_str());
            ++failures;
        }
        Draw();
        CapturePump();
        save(name);
        if (t <= (int)ThemeMode::Dark && !g.rows.empty()) {
            // Folder access uses search on this port; include a path-only phrase in the reference.
            std::wstring dir = g_lib.Str(g_lib[g.rows.front()].dir);
            g.search = L"\"" + FileNameOf(DirectoryOf(dir)) + L"/" + FileNameOf(dir) + L"\"";
            RebuildRows();
            if (g.rows.empty()) {
                fprintf(stderr, "capture: folder path search returned no tracks\n");
                ++failures;
            }
            save(name + "-folders");
            g.search.clear();
            RebuildRows();
        }
        if (t == (int)ThemeMode::Foobar) {
            g_folders.open = true;
            save(name + "-library-folders");
            g_folders.open = false;
        }
        auto menus = BarMenus();
        for (size_t i = 0; i < menus.size(); ++i) {
            bool all = t <= (int)ThemeMode::Dark;
            if (!all && menus[i] != 0 && menus[i] != 5) continue;
            OpenBarMenu((int)i);
            std::string menu = Plain(ui::kMenuBar[menus[i]]);
            std::transform(menu.begin(), menu.end(), menu.begin(), ::tolower);
            save(name + "-menu-" + menu);
            if (menus[i] == 5) {  // View > Theme
                HoverMenu(0, 0);
                save(name + "-menu-view-theme");
            }
            if (menus[i] == 2 && all) {  // Playback > ReplayGain
                const auto& items = g_popups[0].items;
                for (size_t k = 0; k < items.size(); ++k)
                    if (items[k].label == "ReplayGain") HoverMenu(0, (int)k);
                save(name + "-menu-playback-replaygain");
            }
            CloseMenus();
        }
        if (t <= (int)ThemeMode::Dark && !g.rows.empty()) {  // the dialogs the Linux build adds
            OpenEqualizer();
            save(name + "-equalizer");
            ClosePanel();
            SelectOnly(0);
            OpenTagEditor({g.rows[0]});
            save(name + "-edit-tags");
            ClosePanel();
            OpenPreferences();
            save(name + "-preferences");
            ClosePanel();
        }
        // The first-run Library (Albums with the browser), and in light and dark the other two views,
        // as tools/capture-ui.ps1 takes them; foobar2000 and Apollo have layouts of their own.
        if (t != (int)ThemeMode::Foobar && t != (int)ThemeMode::Apollo) {
            const struct { LibraryMode mode; bool browser; const char* suffix; } views[] = {
                {LibraryMode::Albums, true, "-albums"}, {LibraryMode::AlbumTracks, false, "-album-tracks"},
                {LibraryMode::Artists, false, "-artists"}};
            for (const auto& v : views) {
                if (v.mode != LibraryMode::Albums && t > (int)ThemeMode::Dark) continue;
                g.selected.assign(g.rows.size(), 0);  // as a fresh start: nothing selected
                view(v.mode, v.browser);
                Draw();
                CapturePump();  // the covers at the views' sizes
                Draw();
                CapturePump();
                save(name + v.suffix);
            }
            view(LibraryMode::Tracks, false);
        }
    }
    return failures;
}

// An album's cover filling `r` (cropped to square), or while it loads the surface color, or for an
// album without one a tile in a color of its own with the initial of `title` (app_ui.cpp).
void DrawAlbumCover(TrackId first, StringPool::Handle album, const std::wstring& title, const Rect& r, const Palette& p) {
    const int size = (int)std::max(r.w, S(64));
    const uint64_t key = ((uint64_t)album << 32 | g_lib[first].dir) * 0x9E3779B97F4A7C15ull ^ (uint64_t)size;
    auto it = g_thumbs.find(key);
    if (it == g_thumbs.end()) {
        if (g_thumbs.size() >= kThumbsKept) {  // let the least recently drawn half go
            std::vector<std::pair<uint64_t, uint64_t>> done;
            for (auto& [k, t] : g_thumbs)
                if (t.done && t.used < g_frame) done.emplace_back(t.used, k);
            std::sort(done.begin(), done.end());
            for (size_t i = 0; i < done.size() / 2; ++i) {
                auto old = g_thumbs.find(done[i].second);
                if (old->second.texture) SDL_DestroyTexture(old->second.texture);
                g_thumbs.erase(old);
            }
        }
        it = g_thumbs.emplace(key, Thumb{}).first;
        if (!g_thumbFor.count(first)) {
            g_thumbFor[first] = key;
            ++g_thumbsPending;
            RequestThumb(first, size);
        } else {
            g_thumbs.erase(it);  // the track's cover is on its way at another size: ask again after
            Fill(r, p.surface2);
            return;
        }
    }
    it->second.used = g_frame;
    if (it->second.texture) {
        SDL_FRect dst{r.x, r.y, r.w, r.h};
        float tw = 0, th = 0;
        SDL_GetTextureSize(it->second.texture, &tw, &th);
        float side = std::min(tw, th);
        SDL_FRect src{(tw - side) / 2, (th - side) / 2, side, side};
        SDL_RenderTexture(g.renderer, it->second.texture, &src, &dst);
    } else if (it->second.done) {
        uint32_t hash = 2166136261u;
        for (wchar_t c : title) hash = (hash ^ (uint32_t)towlower(c)) * 16777619u;
        double hue = hash % 360;
        auto hsv = [](double h, double s, double v) {
            double c = v * s, x = c * (1 - std::fabs(std::fmod(h / 60, 2) - 1)), m = v - c, rr = 0, gg = 0, bb = 0;
            if (h < 60) rr = c, gg = x;
            else if (h < 120) rr = x, gg = c;
            else if (h < 180) gg = c, bb = x;
            else if (h < 240) gg = x, bb = c;
            else if (h < 300) rr = x, bb = c;
            else rr = c, bb = x;
            auto byte = [&](double u) { return (uint32_t)std::lround((u + m) * 255); };
            return byte(rr) << 16 | byte(gg) << 8 | byte(bb);
        };
        uint32_t tile = p.dark ? hsv(hue, 0.38, 0.30) : hsv(hue, 0.20, 0.90), ink = p.dark ? hsv(hue, 0.28, 0.86) : hsv(hue, 0.50, 0.42);
        Fill(r, tile);
        auto initial = std::find_if(title.begin(), title.end(), [](wchar_t c) { return iswalnum(c) != 0; });
        std::string text = initial == title.end() ? "\xE2\x99\xAB" : Utf8(std::wstring(1, (wchar_t)towupper(*initial)));
        Font font = r.w >= S(80) ? Font::Title : Font::Bold;
        DrawText(font, text, r.x, TextY(r, font), ink, r.w, 1);
    } else {
        Fill(r, p.surface2);
    }
    Frame({r.x - 1, r.y - 1, r.w + 2, r.h + 2}, Mix(p.bg, p.text, 14));
}

void OpenTrackMenu(float x, float y) { OpenRowMenu(x, y); }
void RateTracks(const std::vector<TrackId>& ids, uint8_t rating) { SetRating(ids, rating); }

}  // namespace lx
