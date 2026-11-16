// Tag album from MusicBrainz on Linux, as app_albumtag.cpp on Windows: the releases MusicBrainz has for
// the selected album, best match first, and every tag the chosen one would change. Lengths pick the
// right pressing; when no release matches clearly, none is chosen for you. Apply writes the tags as
// one edit, so Edit > Undo last tag edit takes the whole album back. Tags are read and the service
// asked on a worker, whose result comes back as WM_APP_ALBUMTAG and is dropped when it belongs to a
// panel since closed. The model is album_tagger.h.
#include "panels_internal.h"

#include "album_tagger.h"
#include "tags.h"

#include <memory>
#include <thread>

namespace lx::panel {
namespace {

struct Found {
    uint32_t generation = 0;
    std::vector<albumtag::LocalTrack> local;
    albumtag::SearchResult result;
};

struct TaggerPanel {
    std::wstring album;
    std::vector<albumtag::LocalTrack> local;
    albumtag::SearchResult result;
    bool searching = false;
    int chosen = -1;                 // the release shown, or -1
    std::vector<std::string> lines;  // its changes, as drawn
    std::vector<std::pair<TrackId, TagEdit>> edits;
    int first = 0;                   // the first change line shown
    std::shared_ptr<online::Cancel> stop;
    std::thread worker;
    uint32_t generation = 0;
    Rect releases, changes, apply, cancel;
} g_tagger;

constexpr float kReleaseRow = 26, kChangeRow = 22;

void StopWorker() {
    if (g_tagger.stop) g_tagger.stop->Stop();
    if (g_tagger.worker.joinable()) g_tagger.worker.join();
    g_tagger.stop.reset();
}

void Choose(int row) {
    auto& t = g_tagger;
    t.chosen = row >= 0 && row < (int)t.result.candidates.size() ? row : -1;
    t.lines.clear();
    t.edits.clear();
    t.first = 0;
    if (t.chosen >= 0) {
        const auto& c = t.result.candidates[(size_t)t.chosen];
        std::vector<albumtag::Change> changes;
        t.edits = albumtag::Propose(t.local, c.release, c.match, &changes);
        for (size_t i = 0; i < t.local.size(); ++i) {
            const std::string name = t.local[i].title.empty() ? "(untitled)" : t.local[i].title;
            if (c.match.pick[i] < 0) {
                t.lines.push_back(name + ":  not on this release; left as it is");
                continue;
            }
            bool first = true;
            for (const auto& change : changes) {
                if (change.track != i) continue;
                if (first) t.lines.push_back(name);
                first = false;
                t.lines.push_back("    " + change.field + ":  " + (change.before.empty() ? "(empty)" : change.before) +
                                  "  \xE2\x86\x92  " + change.after);
            }
        }
    }
    g.redraw = true;
}

void Apply() {
    if (g_tagger.edits.empty()) return;
    if (TagWriteBusy()) {
        ShowMessage(L"Tags are still being written; try again in a moment.");
        return;
    }
    auto edits = std::move(g_tagger.edits);
    ClosePanel();
    SubmitTagWrites(edits, true);
}

std::string ReleaseRow(const albumtag::Candidate& c) {
    const auto& r = c.release;
    std::string row = std::to_string((int)std::lround(c.match.confidence * 100)) + "%   " + r.title;
    if (!r.artist.empty()) row += "  \xE2\x80\x94  " + r.artist;
    std::string details;
    for (const std::string& part : {r.date, r.country, r.format,
                                    std::to_string(r.trackCount) + (r.trackCount == 1 ? " track" : " tracks"), r.label})
        if (!part.empty()) details += (details.empty() ? "" : ", ") + part;
    return row + "   (" + details + ")";
}

void DrawTagger(const Palette& p) {
    auto& t = g_tagger;
    Rect panel = DrawFrame(p, S(780), S(600), "Tag album from MusicBrainz");
    const float pad = S(16), x0 = panel.x + pad, w = panel.w - 2 * pad;
    float y = panel.y + S(48);
    std::string status;
    if (t.searching) status = "Asking MusicBrainz about \xE2\x80\x9C" + Utf8(t.album) + "\xE2\x80\x9D\xE2\x80\xA6";
    else if (t.result.offline) status = "MusicBrainz couldn't be reached. Check the connection and try again.";
    else if (t.result.candidates.empty()) status = "MusicBrainz has no release by this name and artist.";
    else if (t.result.confident) status = "The first release matches these tracks' titles and lengths.";
    else status = "No release matches clearly. Choose one, or cancel.";
    DrawText(Font::Normal, status, x0, y, p.text, w);
    y += S(30);
    DrawText(Font::Normal, "Releases on MusicBrainz", x0, y, p.sub, w);
    y += S(22);
    const float row = S(kReleaseRow);
    t.releases = {x0, y, w, row * 5 + S(4)};
    Fill(t.releases, p.bg);
    Frame(t.releases, p.border);
    for (int i = 0; i < (int)t.result.candidates.size() && i < 5; ++i) {
        Rect r{x0 + S(1), y + S(2) + i * row, w - S(2), row};
        const bool sel = i == t.chosen;
        if (sel) Fill(r, p.select);
        DrawText(Font::Normal, ReleaseRow(t.result.candidates[(size_t)i]), r.x + S(8), TextY(r), sel ? p.selectText : p.text,
                 r.w - S(16));
    }
    y = t.releases.B() + S(10);
    DrawText(Font::Normal, "What changes", x0, y, p.sub, w);
    y += S(22);
    const float bottom = panel.B() - pad - S(28) - S(32);
    t.changes = {x0, y, w, std::max(S(60), bottom - y)};
    Fill(t.changes, p.bg);
    Frame(t.changes, p.border);
    const float line = S(kChangeRow);
    const int visible = std::max(1, (int)((t.changes.h - S(4)) / line));
    t.first = std::clamp(t.first, 0, std::max(0, (int)t.lines.size() - visible));
    SDL_Rect clip{(int)t.changes.x + 1, (int)t.changes.y + 1, (int)t.changes.w - 2, (int)t.changes.h - 2};
    SDL_SetRenderClipRect(g.renderer, &clip);
    for (int i = t.first; i < (int)t.lines.size() && i < t.first + visible; ++i) {
        Rect r{x0 + S(1), y + S(2) + (i - t.first) * line, w - S(2), line};
        const bool heading = t.lines[(size_t)i].rfind("    ", 0) != 0;
        DrawText(Font::Normal, t.lines[(size_t)i], r.x + S(8), TextY(r), heading ? p.text : p.sub, r.w - S(16));
    }
    SDL_SetRenderClipRect(g.renderer, nullptr);
    std::string info;
    if (t.chosen >= 0)
        info = t.edits.empty() ? "These tracks already have this release's tags."
                               : "Apply writes the tags of " + std::to_string(t.edits.size()) +
                                     (t.edits.size() == 1 ? " file" : " files") + ". Edit > Undo last tag edit takes it back.";
    else if (!t.result.candidates.empty()) info = "Choose the release these tracks come from to see what would change.";
    const float by = panel.B() - pad - S(28);
    DrawText(Font::Small, info, x0, by - S(26), p.sub, w);
    t.apply = {panel.R() - pad - S(248), by, S(140), S(28)};
    t.cancel = {panel.R() - pad - S(100), by, S(100), S(28)};
    DrawButton(t.apply, "Apply", p, true, !t.edits.empty());
    DrawButton(t.cancel, "Cancel", p);
}

void TaggerMouseDown(float x, float y, int) {
    auto& t = g_tagger;
    if (t.apply.In(x, y)) return Apply();
    if (t.cancel.In(x, y)) return ClosePanel();
    if (t.releases.In(x, y)) {
        const int i = (int)((y - t.releases.y - S(2)) / S(kReleaseRow));
        if (i >= 0 && i < (int)t.result.candidates.size()) Choose(i);
    }
}

void TaggerKey(const SDL_KeyboardEvent& e) {
    auto& t = g_tagger;
    if (e.key == SDLK_ESCAPE) return ClosePanel();
    if (e.key == SDLK_RETURN || e.key == SDLK_KP_ENTER) return Apply();
    const int count = std::min(5, (int)t.result.candidates.size());
    if ((e.key == SDLK_UP || e.key == SDLK_DOWN) && count)
        Choose(std::clamp(t.chosen + (e.key == SDLK_DOWN ? 1 : -1), 0, count - 1));
}

void TaggerWheel(float x, float y, float dy) {
    if (!g_tagger.changes.In(x, y)) return;
    g_tagger.first = std::max(0, g_tagger.first - (int)std::lround(dy * 3));
    g.redraw = true;
}

void TaggerClose() {
    StopWorker();
    const uint32_t generation = g_tagger.generation + 1;  // a result still on its way is dropped
    g_tagger = TaggerPanel{};
    g_tagger.generation = generation;
}

}  // namespace

const Ops kAlbumTagOps{
    .draw = DrawTagger,
    .mouseDown = TaggerMouseDown,
    .wheel = TaggerWheel,
    .key = TaggerKey,
    .close = TaggerClose,
};

}  // namespace lx::panel

namespace lx {
using namespace panel;

void OnAlbumTagMessage(LPARAM lp) {
    std::unique_ptr<Found> found(reinterpret_cast<Found*>(lp));
    if (!found || found->generation != g_tagger.generation || g_kind != Kind::AlbumTag) return;
    if (g_tagger.worker.joinable()) g_tagger.worker.join();
    g_tagger.searching = false;
    g_tagger.local = std::move(found->local);
    g_tagger.result = std::move(found->result);
    Choose(g_tagger.result.confident ? 0 : -1);
}

void OpenAlbumTagger(const std::vector<TrackId>& selection) {
    if (selection.empty()) return;
    // The worker gets copies: each track as the library knows it, which the file's own tags replace
    // when they can be read, and its file.
    struct Item { albumtag::LocalTrack track; std::wstring path; };
    std::vector<Item> items;
    std::wstring album;
    for (TrackId id : AlbumTracksOf(selection)) {
        const Track& track = g_lib[id];
        albumtag::LocalTrack l;
        l.id = id;
        auto text = [&](StringPool::Handle h) { return h ? Utf8(g_lib.Str(h)) : std::string(); };
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
        ShowMessage(L"These tracks have no album name to look up. Give them one with Edit album tags first.");
        return;
    }
    // The lookup may run while it is off, but only after the user agrees to what it sends.
    if (!online::Config().albumTags) {
        if (!Confirm("Album tags from MusicBrainz is off, so cordinal hasn't contacted anyone.\n\n"
                     "It sends the album's title and artist to musicbrainz.org, then the ids of the few releases that "
                     "match, to read their tracks. Nothing is written until you choose a release and Apply.\n\n"
                     "Turn it on? It can be changed in Preferences > Online.",
                     "Tag album from MusicBrainz"))
            return;
        online::Config().albumTags = true;
        SaveSettings();
    }
    ClosePanel();
    TaggerClose();
    auto& t = g_tagger;
    t.album = album;
    t.searching = true;
    t.stop = std::make_shared<online::Cancel>();
    t.worker = std::thread([items = std::move(items), stop = t.stop, generation = t.generation] {
        auto found = std::make_unique<Found>();
        found->generation = generation;
        for (const auto& item : items) {
            albumtag::LocalTrack l = item.track;
            TagData d;
            if (ReadTags(item.path, d)) {
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
            }
            found->local.push_back(std::move(l));
        }
        found->result = albumtag::Search(found->local, *stop);
        if (stop->Stopped()) return;
        PostMessageW(nullptr, WM_APP_ALBUMTAG, 0, (LPARAM)found.release());
    });
    g_kind = Kind::AlbumTag;
    g.redraw = true;
}

}  // namespace lx
