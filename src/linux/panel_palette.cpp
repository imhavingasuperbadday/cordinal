// The command palette on Linux (View > Command palette, Ctrl+Shift+P), as the Windows build's app_palette.cpp:
// one search over every command of the menu bar, with its checkmark, shortcut and where it lives, and over the
// settings of Preferences (a switch flips, the rest open Preferences at that setting). Enter or a click runs
// the command once the palette has closed.
//
// The same panel is the song picker (OpenSongPicker, Lightweight mode's 0 on the numpad and the tray's "Play a
// song..."), as on Windows: the transport, Up next and the playlists, and as you type, albums and songs from the
// library. Enter plays a song now and then carries on with what was playing; Shift+Enter puts it next. With
// nothing typed, Space pauses or resumes and Left and Right skip.
#include "panels_internal.h"

#include "metadata_fetch.h"
#include "online_search.h"
#include "search_online_ui.h"
#include "textcase.h"

#include <atomic>
#include <memory>
#include <thread>

namespace lx::panel {
namespace {

// What a song picker entry does when it is not a menu command or a setting.
enum class Pick : uint8_t { None, PlayPause, Next, Previous, Shuffle, Lightweight, Song, Album, Playlist };

struct PaletteEntry {
    PaletteCommand command;
    int menu = -1;  // an index for RunMenuCommand, or
    int pref = -1;  // an index of Preferences' settings (PrefCommand), or in the song picker:
    Pick pick = Pick::None;
    TrackId track = kNone;  // a song, or the first song of an album
    int playlist = -1;
    bool online = false;
    online::SearchTrack onlineTrack;
    std::wstring searchable, label;  // folded, as the query is: label is the command's own name
};
struct PickerOnlinePost {
    uint64_t generation = 0;
    std::vector<online::SearchTrack> tracks;
};
struct PickerOnlineState {
    online::Generation gen;
    std::shared_ptr<online::Cancel> cancel;
    std::atomic<bool> busy{false};
    std::thread worker;
    std::wstring query;
    std::vector<online::SearchTrack> rows;
    uint64_t debounceUntil = 0;
} po;
struct PalettePanel {
    TextField query;
    std::vector<PaletteEntry> entries;
    std::vector<int> shown;  // entries matching the query, best first
    int selected = 0, first = 0;
    Rect panel, field, list;
    bool songs = false;  // the song picker
    size_t fixed = 0;    // its entries that are not search results
} g_palette;
constexpr int kPaletteRows = 11;
constexpr size_t kMaxSongs = 60, kMaxAlbums = 6;  // the picker is for choosing, the library for browsing

std::string Narrow(const std::wstring& s) { return compat::Narrow(s.c_str()); }

PaletteEntry PickEntry(Pick pick, const std::string& label, const std::string& path = {}, const std::string& shortcut = {}) {
    PaletteEntry e;
    e.pick = pick;
    e.command.label = label;
    e.command.path = path;
    e.command.shortcut = shortcut;
    e.searchable = textcase::Fold(compat::Widen((label + " " + path).c_str()));
    e.label = textcase::Fold(compat::Widen(label.c_str()));
    return e;
}

PaletteEntry SongEntry(TrackId id, const char* where) {
    std::string artist = Narrow(TrackArtist(id));
    std::string path = !where ? artist : artist.empty() ? std::string(where) : std::string(where) + " \xE2\x80\xBA " + artist;
    PaletteEntry e = PickEntry(Pick::Song, Narrow(TrackTitle(id)), path, Narrow(FormatDuration(g_lib[id].Duration())));
    e.track = id;
    return e;
}

// The picker's standing entries: the transport with what is playing, Up next, the playlists and the way back.
void CollectSongs() {
    g_palette.entries.clear();
    const bool idle = g.playing == kNone && !RadioPlaying();
    std::string playing;
    if (g.playing != kNone && g.playing < g_lib.Count()) {
        playing = Narrow(TrackTitle(g.playing));
        if (std::wstring artist = TrackArtist(g.playing); !artist.empty()) playing += " \xE2\x80\x94 " + Narrow(artist);
    }
    g_palette.entries.push_back(PickEntry(Pick::PlayPause, idle ? "Play" : IsPaused() ? "Resume" : "Pause", playing, "Space"));
    g_palette.entries.push_back(PickEntry(Pick::Next, "Next", {}, "\xE2\x86\x92"));
    g_palette.entries.back().command.enabled = !idle;
    g_palette.entries.push_back(PickEntry(Pick::Previous, "Previous", {}, "\xE2\x86\x90"));
    g_palette.entries.back().command.enabled = !idle;
    for (TrackId id : UpNextTracks())
        if (id < g_lib.Count()) g_palette.entries.push_back(SongEntry(id, "Up next"));
    for (int i = 0; i < (int)g.playlists.size(); ++i) {
        const Playlist& pl = g.playlists[i];
        if (pl.items.empty() || pl.kind == PlaylistKind::Inbox) continue;
        PaletteEntry e = PickEntry(Pick::Playlist, Narrow(pl.name), "Playlist",
                                   std::to_string(pl.items.size()) + (pl.items.size() == 1 ? " song" : " songs"));
        e.playlist = i;
        g_palette.entries.push_back(std::move(e));
    }
    g_palette.entries.push_back(PickEntry(Pick::Shuffle, "Shuffle"));
    g_palette.entries.back().command.checked = g.settings.shuffle == kShuffleTracks;
    g_palette.entries.push_back(
        PickEntry(Pick::Lightweight, g.lightweight ? "Back to the full window" : "Lightweight mode", {}, "Ctrl+Shift+L"));
    g_palette.fixed = g_palette.entries.size();
}

// Albums and songs in the library holding every word typed, in library order; nothing is kept between keys.
void FindSongs() {
    g_palette.entries.resize(g_palette.fixed);
    std::vector<std::wstring> words = FilterWords(g_palette.query.text);
    if (words.empty()) return;
    TrackMatcher matcher(g_lib, words);
    std::vector<PaletteEntry> songs, albums;
    std::vector<StringPool::Handle> seen;
    for (TrackId id = 0; id < (TrackId)g_lib.Count() && songs.size() < kMaxSongs; ++id) {
        const Track& t = g_lib[id];
        if (!t.InLibrary() || !matcher.Matches(id)) continue;
        songs.push_back(SongEntry(id, nullptr));
        if (!t.album || albums.size() >= kMaxAlbums || std::find(seen.begin(), seen.end(), t.album) != seen.end()) continue;
        seen.push_back(t.album);
        std::string artist(g_lib.View(t.albumArtist));
        if (artist.empty()) artist = std::string(g_lib.View(t.artist));
        PaletteEntry a = PickEntry(Pick::Album, std::string(g_lib.View(t.album)),
                                   artist.empty() ? std::string("Album") : "Album \xE2\x80\xBA " + artist);
        a.track = id;
        albums.push_back(std::move(a));
    }
    for (auto* found : {&albums, &songs})
        for (PaletteEntry& e : *found) g_palette.entries.push_back(std::move(e));
    if (online::Config().searchMusic && g_palette.query.text == po.query) {
        for (const online::SearchTrack& t : po.rows) {
            std::string path = t.artist.empty() ? std::string("Online") : "Online \xE2\x80\xBA " + t.artist;
            std::string dur = t.durationSec > 0 ? Narrow(FormatDuration((double)t.durationSec)) : "";
            PaletteEntry e = PickEntry(Pick::Song, t.title, path, dur);
            e.online = true;
            e.onlineTrack = t;
            g_palette.entries.push_back(std::move(e));
        }
    }
}

void CancelPickerOnline() {
    po.debounceUntil = 0;
    po.gen.Next();
    if (po.cancel) po.cancel->Stop();
}

void StartPickerOnline() {
    if (!g_palette.songs || !online::Config().searchMusic || !online::SearchableQuery(po.query)) return;
    if (po.busy.load()) {
        po.debounceUntil = SDL_GetTicks() + 60;
        return;
    }
    if (po.worker.joinable()) po.worker.join();
    const uint64_t gen = po.gen.Next();
    auto cancel = std::make_shared<online::Cancel>();
    po.cancel = cancel;
    po.busy = true;
    const std::string qUtf8 = online::NormalizeSearchQuery(po.query);
    import::OwnedIndex owned = OnlineSearchOwnedSnapshot();
    po.worker = std::thread([gen, qUtf8, owned = std::move(owned), cancel] {
        auto* post = new PickerOnlinePost{gen, {}};
        auto res = online::SearchTracks(qUtf8, owned, *cancel, 5);
        if (res.ok) post->tracks = std::move(res.tracks);
        po.busy = false;
        if (!PostMessageW(nullptr, WM_APP_PICKER_ONLINE, 0, (LPARAM)post)) delete post;
    });
}

void PaletteFilter(bool keepSelection = false) {
    const int prevSel = keepSelection ? g_palette.selected : 0;
    const int prevFirst = keepSelection ? g_palette.first : 0;
    if (g_palette.songs) {
        if (!keepSelection) {
            CancelPickerOnline();
            if (g_palette.query.text != po.query) {
                po.query = g_palette.query.text;
                po.rows.clear();
            }
            if (online::Config().searchMusic && online::SearchableQuery(g_palette.query.text))
                po.debounceUntil = SDL_GetTicks() + 400;
        }
        FindSongs();
    }
    const int ranked = g_palette.songs ? (int)g_palette.fixed : (int)g_palette.entries.size();
    std::wstring q = textcase::Fold(g_palette.query.text);
    std::vector<std::wstring> words;
    for (size_t i = 0; i < q.size();) {
        size_t end = q.find(L' ', i);
        if (end == std::wstring::npos) end = q.size();
        if (end > i) words.push_back(q.substr(i, end - i));
        i = end + 1;
    }
    std::vector<std::pair<int, int>> scored;  // rank, entry
    for (int i = 0; i < ranked; ++i) {
        const PaletteEntry& e = g_palette.entries[i];
        int rank = 0;
        bool all = true;
        const std::wstring& label = e.label;
        for (const std::wstring& w : words) {
            if (e.searchable.find(w) == std::wstring::npos) {
                all = false;
                break;
            }
            rank += label.starts_with(w) ? 0 : label.find(w) != std::wstring::npos ? 1 : 2;
        }
        if (all) scored.push_back({rank, i});
    }
    std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    g_palette.shown.clear();
    // In the picker a search is for music: what the library found leads.
    if (g_palette.songs && !words.empty())
        for (size_t i = g_palette.fixed; i < g_palette.entries.size(); ++i) g_palette.shown.push_back((int)i);
    for (auto& s : scored) g_palette.shown.push_back(s.second);
    g_palette.selected = keepSelection ? std::clamp(prevSel, 0, std::max(0, (int)g_palette.shown.size() - 1)) : 0;
    g_palette.first = keepSelection ? std::min(prevFirst, g_palette.selected) : 0;
}

// The picker after a transport key: rebuilt, so Pause reads Resume, with the same row selected.
void RefreshSongs() {
    const int selected = g_palette.selected, first = g_palette.first;
    CollectSongs();
    PaletteFilter();
    g_palette.selected = std::clamp(selected, 0, std::max(0, (int)g_palette.shown.size() - 1));
    g_palette.first = std::min(first, g_palette.selected);
}

// Plays what was picked: a song now, then on with what was playing (`next` only puts it next); an album or a
// playlist from its start, or a random place in shuffle.
void PlayPicked(const PaletteEntry& e, bool next) {
    if (e.online) {
        OnlineSearchPlay(e.onlineTrack, next);
        return;
    }
    if (e.pick == Pick::Playlist) {
        if (e.playlist < 0 || e.playlist >= (int)g.playlists.size() || g.playlists[e.playlist].items.empty()) return;
        const auto& items = g.playlists[e.playlist].items;
        PlayTracks(items, g.settings.shuffle == kShuffleTracks ? (int)(SDL_GetTicks() % items.size()) : 0);
        return;
    }
    if (e.track >= g_lib.Count()) return;
    if (e.pick == Pick::Album) {
        std::vector<TrackId> tracks = AlbumTracksOf({e.track});
        if (next) QueueTracks(tracks, true);
        else PlayTracks(tracks, 0);
        return;
    }
    const bool playing = g.playing != kNone || RadioPlaying();
    QueueTracks({e.track}, true);  // starts it at once when nothing is playing
    if (!next && playing) Next();
}

// The transport keeps the picker open: pause now, pick a song after.
bool RunTransport(Pick pick) {
    switch (pick) {
        case Pick::PlayPause: TogglePause(); break;
        case Pick::Next: Next(); break;
        case Pick::Previous: Previous(); break;
        case Pick::Shuffle: ToggleShuffle(); break;
        default: return false;
    }
    MprisChanged();
    RefreshSongs();
    g.redraw = true;
    return true;
}

void PaletteRun(int row, bool next = false) {
    if (row < 0 || row >= (int)g_palette.shown.size()) return;
    const PaletteEntry e = g_palette.entries[g_palette.shown[row]];
    if (!e.command.enabled) return;
    if (RunTransport(e.pick)) return;
    ClosePanel();
    if (e.menu >= 0) {
        RunMenuCommand((size_t)e.menu);
    } else if (e.pref >= 0) {
        RunPref(e.pref);
    } else if (e.pick == Pick::Lightweight) {
        ToggleLightweight();
    } else if (e.pick != Pick::None) {
        PlayPicked(e, next);
    }
}

void DrawPalette(const Palette& p) {
    const bool songs = g_palette.songs;
    Rect panel = g_palette.panel = DrawFrame(p, S(640), S(470), songs ? "Play a song" : "Command palette");
    const float pad = S(16), x0 = panel.x + pad, w = panel.w - 2 * pad, rowH = S(30);
    g_palette.field = {x0, panel.y + S(48), w, S(30)};
    DrawLineField(g_palette.query, g_palette.field, true, true,
                  songs ? "Type a song, album or artist (Space pauses)" : "Type a command or a setting", p);
    g_palette.list = {x0, g_palette.field.B() + S(8), w, rowH * kPaletteRows};
    for (int i = 0; i < kPaletteRows && g_palette.first + i < (int)g_palette.shown.size(); ++i) {
        const int row = g_palette.first + i;
        const PaletteEntry& e = g_palette.entries[g_palette.shown[row]];
        Rect r{g_palette.list.x, g_palette.list.y + i * rowH, g_palette.list.w, rowH};
        const bool selected = row == g_palette.selected;
        if (selected) Fill(r, p.select);
        else if (r.In(g_mx, g_my)) Fill(r, p.hover);
        const uint32_t main = !e.command.enabled ? p.faint : selected ? p.selectText : p.text;
        const uint32_t sub = !e.command.enabled ? p.faint : selected ? p.selectText : p.sub;
        std::string label = (e.command.checked ? "\xE2\x9C\x93 " : "") + e.command.label;
        const float shortcutW = e.command.shortcut.empty() ? 0 : TextWidth(Font::Normal, e.command.shortcut) + S(12);
        const float pathW = std::min(TextWidth(Font::Small, e.command.path) + S(12), r.w * 0.4f);
        DrawText(Font::Normal, label, r.x + S(8), TextY(r), main, r.w - shortcutW - pathW - S(16));
        DrawText(Font::Small, e.command.path, r.R() - shortcutW - pathW, TextY(r, Font::Small), sub, pathW - S(8), 2);
        if (shortcutW > 0) DrawText(Font::Normal, e.command.shortcut, r.R() - shortcutW, TextY(r), sub, shortcutW - S(8), 2);
    }
    if (g_palette.shown.empty()) DrawText(Font::Normal, "Nothing matches.", g_palette.list.x + S(8), g_palette.list.y + S(6), p.sub);
}

void PaletteMouseDown(float x, float y) {
    if (!g_palette.panel.In(x, y)) return ClosePanel();
    if (g_palette.list.In(x, y)) PaletteRun(g_palette.first + (int)((y - g_palette.list.y) / S(30)));
}

void PaletteKey(const SDL_KeyboardEvent& e) {
    const int count = (int)g_palette.shown.size();
    auto move = [&](int to) {
        g_palette.selected = std::clamp(to, 0, std::max(0, count - 1));
        if (g_palette.selected < g_palette.first) g_palette.first = g_palette.selected;
        else if (g_palette.selected >= g_palette.first + kPaletteRows) g_palette.first = g_palette.selected - kPaletteRows + 1;
    };
    // The picker with nothing typed is a remote: Space pauses, Left and Right skip.
    if (g_palette.songs && g_palette.query.text.empty() && !(e.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT))) {
        const Pick pick = e.key == SDLK_SPACE ? Pick::PlayPause : e.key == SDLK_LEFT ? Pick::Previous
                          : e.key == SDLK_RIGHT ? Pick::Next : Pick::None;
        if (pick != Pick::None) {
            RunTransport(pick);
            return;
        }
    }
    switch (e.key) {
        case SDLK_ESCAPE: return ClosePanel();
        case SDLK_RETURN: case SDLK_KP_ENTER: return PaletteRun(g_palette.selected, (e.mod & SDL_KMOD_SHIFT) != 0);
        case SDLK_UP: return move(g_palette.selected - 1);
        case SDLK_DOWN: return move(g_palette.selected + 1);
        case SDLK_PAGEUP: return move(g_palette.selected - kPaletteRows);
        case SDLK_PAGEDOWN: return move(g_palette.selected + kPaletteRows);
        default: break;
    }
    if (LineFieldKey(g_palette.query, e) == 1) PaletteFilter();
}

void PaletteText(const char* s) {
    if (g_palette.songs && g_palette.query.text.empty() && !strcmp(s, " ")) return;  // Space paused instead
    if (LineFieldInsert(g_palette.query, Printable(compat::Widen(s)))) PaletteFilter();
}

}  // namespace

const Ops kPaletteOps{
    .draw = DrawPalette,
    .mouseDown = [](float x, float y, int) { PaletteMouseDown(x, y); },
    .wheel = [](float, float, float dy) {
        g_palette.first = std::clamp(g_palette.first - (int)std::lround(dy * 2), 0, std::max(0, (int)g_palette.shown.size() - kPaletteRows));
    },
    .key = PaletteKey,
    .text = PaletteText,
    .wantsText = [](float x, float y) { return g_palette.field.In(x, y); },
    .close = [] {
        CancelPickerOnline();
        po.rows.clear();
        po.query.clear();
        if (g_palette.songs) LightweightPickerClosed();  // a window shown only for the picker goes away again
    },
    .typing = true,
};

}  // namespace lx::panel

namespace lx {
using namespace panel;

void OpenPalette() {
    g_palette = PalettePanel{};
    auto commands = MenuCommands();
    for (size_t i = 0; i < commands.size(); ++i) {
        PaletteEntry e;
        e.command = commands[i];
        e.menu = (int)i;
        e.searchable = textcase::Fold(compat::Widen((e.command.label + " " + e.command.path).c_str()));
        g_palette.entries.push_back(std::move(e));
    }
    for (int i = 0; i < (int)PrefCount(); ++i) {
        if (!PrefListed(i)) continue;
        PaletteEntry e;
        e.command = PrefCommand(i);
        e.pref = i;
        e.searchable = PrefSearchText(i);
        g_palette.entries.push_back(std::move(e));
    }
    for (PaletteEntry& e : g_palette.entries) e.label = textcase::Fold(compat::Widen(e.command.label.c_str()));
    PaletteFilter();
    g_kind = Kind::Palette;
    SDL_StartTextInput(g.window);
    g.redraw = true;
}

void OpenSongPicker() {
    if (PanelOpen()) ClosePanel();
    g_palette = PalettePanel{};
    po.query.clear();
    po.rows.clear();
    CancelPickerOnline();
    g_palette.songs = true;
    CollectSongs();
    PaletteFilter();
    g_kind = Kind::Palette;
    SDL_StartTextInput(g.window);
    g.redraw = true;
}

bool PickerOnlinePending() { return PanelOpen() && g_palette.songs && po.debounceUntil > 0; }

void PickerOnlineTick() {
    if (PanelOpen() && g_palette.songs && po.debounceUntil > 0 && SDL_GetTicks() >= po.debounceUntil) {
        po.debounceUntil = 0;
        StartPickerOnline();
    }
}

void OnPickerOnlineMessage(WPARAM, LPARAM lp) {
    std::unique_ptr<PickerOnlinePost> post(reinterpret_cast<PickerOnlinePost*>(lp));
    if (!post || !PanelOpen() || !g_palette.songs || !po.gen.Accepts(post->generation)) return;
    po.rows = std::move(post->tracks);
    PaletteFilter(true);
    g.redraw = true;
}

void StopPickerOnline() {
    CancelPickerOnline();
    if (po.worker.joinable()) po.worker.join();
    po.rows.clear();
    po.query.clear();
}

// For the self-test: the shown rows, "label | path", and a query typed into the open picker.
std::vector<std::string> SongPickerRows() {
    std::vector<std::string> rows;
    for (int i : g_palette.shown) rows.push_back(g_palette.entries[i].command.label + " | " + g_palette.entries[i].command.path);
    return rows;
}

void SetSongPickerQuery(const std::wstring& query) {
    g_palette.query.text = query;
    g_palette.query.caret = query.size();
    PaletteFilter();
}

}  // namespace lx
