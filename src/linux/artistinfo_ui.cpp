// Artist info on Linux (View > Artist info..., or a track's right-click menu), as app_artistinfo.cpp
// does on Windows with the same wording: the selected or playing track's artist as MusicBrainz knows
// them, the opening of their English Wikipedia article, their albums and EPs with the ones the library
// has marked (and a filter for the ones it lacks; clicking one opens its MusicBrainz page), and buttons
// that open MusicBrainz, Wikipedia and the official site in the browser. The lookup (metadata_fetch.h)
// runs on a worker and is off until Preferences > Online turns it on; a namesake is never shown.
#include "panels_internal.h"

#include "draw.h"
#include "metadata_fetch.h"

#include <algorithm>
#include <memory>

namespace lx {

namespace {

struct Result {
    uint64_t generation;
    bool found;
    online::ArtistInfo info;
    std::vector<online::ReleaseGroup> albums;
    bool albumsKnown;  // MusicBrainz answered for the discography (an empty one included)
};

struct ArtistState {
    bool open = false;
    std::string artist, status;
    online::ArtistInfo info;
    bool found = false;
    float scroll = 0, listScroll = 0;
    std::vector<online::ReleaseGroup> discography;
    bool discographyKnown = false;
    bool missingOnly = false;
    std::vector<int> shown;  // the list's rows, as indexes into discography
    online::Generation generation;
    std::shared_ptr<online::Cancel> cancel;
    std::thread worker;
    Rect text, missing, list, musicbrainz, wikipedia, official, close;
} a;

void StopLookup() {
    if (a.cancel) a.cancel->Stop();
    if (a.worker.joinable()) a.worker.join();
    a.cancel.reset();
    a.generation.Next();
}

void OpenUrl(const std::string& url) {
    // The addresses came from the network: only web pages are handed to the browser.
    if (url.starts_with("https://") || url.starts_with("http://")) SDL_OpenURL(url.c_str());
}

void FillAlbums() {
    a.shown.clear();
    for (int i = 0; i < (int)a.discography.size(); ++i)
        if (!a.missingOnly || !a.discography[i].owned) a.shown.push_back(i);
    a.listScroll = 0;
}

std::string AlbumCounts() {
    if (!a.discographyKnown) return "Their albums couldn't be fetched from MusicBrainz.";
    if (a.discography.empty()) return "MusicBrainz lists no albums or EPs for this artist.";
    size_t missing = std::count_if(a.discography.begin(), a.discography.end(), [](const auto& r) { return !r.owned; });
    return std::to_string(a.discography.size()) + (a.discography.size() == 1 ? " album or EP, " : " albums and EPs, ") +
           std::to_string(missing) + " not in your library.";
}

float RowHeight() { return LineHeight(Font::Normal) + S(6); }

}  // namespace

void OpenArtistInfo() {
    auto ids = SelectedTracks();
    TrackId id = !ids.empty() ? ids.front() : g.playing;
    if (id == kNone || !g_lib.Contains(id)) {
        ShowMessage(L"Select a track, or play one, to see its artist.");
        return;
    }
    const Track& t = g_lib[id];
    std::string artist = ToUtf8(g_lib.Str(t.artist ? t.artist : t.albumArtist));
    if (artist.empty()) {
        ShowMessage(L"This track has no artist in its tags.");
        return;
    }
    if (!online::Config().artistInfo) {
        if (!Confirm("Artist info is off, so cordinal hasn't contacted anyone.\n\n"
                     "It sends the artist's name to musicbrainz.org, which also lists their albums, then asks "
                     "wikidata.org and en.wikipedia.org for the article MusicBrainz names.\n\nTurn Artist info on? "
                     "It can be changed in Preferences > Online.",
                     "Artist info"))
            return;
        online::Config().artistInfo = true;
        SaveSettings();
    }
    StopLookup();
    a.open = true;
    a.artist = artist;
    a.info = {};
    a.found = false;
    a.scroll = 0;
    a.discography.clear();
    FillAlbums();
    a.status = "Asking MusicBrainz...";
    const uint64_t generation = a.generation.Next();
    a.cancel = std::make_shared<online::Cancel>();
    a.worker = std::thread([artist, generation, cancel = a.cancel] {
        auto* result = new Result{generation, false, {}, {}, false};
        result->found = online::FetchArtistInfo(artist, {}, *cancel, result->info);
        if (result->found) result->albumsKnown = online::FetchDiscography(result->info.mbid, *cancel, result->albums);
        if (!PostMessageW(nullptr, WM_APP_ARTISTINFO, 0, (LPARAM)result)) delete result;
    });
    ShowArtistPanel();
}

void OnArtistInfoMessage(LPARAM lp) {
    std::unique_ptr<Result> result(reinterpret_cast<Result*>(lp));
    if (!a.open || !a.generation.Accepts(result->generation)) return;  // closed, or another artist since
    if (a.worker.joinable()) a.worker.join();
    a.cancel.reset();
    a.found = result->found;
    a.info = std::move(result->info);
    a.discography = std::move(result->albums);
    a.discographyKnown = result->albumsKnown;
    online::MarkOwned(a.discography, online::LibraryAlbumsBy(g_lib, a.artist));
    FillAlbums();
    if (!a.found)
        a.status = "MusicBrainz has no single artist named \xE2\x80\x9C" + a.artist +
                   "\xE2\x80\x9D, so nothing is shown rather than a namesake.";
    else
        a.status = (a.info.summary.empty() ? "From MusicBrainz. " : "From MusicBrainz and Wikipedia (text under CC BY-SA 4.0). ") +
                   AlbumCounts();
    g.redraw = true;
}

void DrawArtistPanel(const Palette& p) {
    Rect panel = DrawFrame(p, S(620), S(600), "Artist info");
    const float pad = S(16);
    DrawText(Font::Bold, a.found ? a.info.name : a.artist, panel.x + pad, panel.y + S(46), p.text, panel.w - pad * 2);
    float bottom = panel.B() - pad - S(30);
    // The article takes two fifths of the middle, the discography the rest.
    const float top = panel.y + S(74), middle = bottom - S(36) - top, summary = std::floor(middle * 2 / 5);
    a.text = {panel.x + pad, top, panel.w - pad * 2, summary};
    Fill(a.text, p.surface2);
    Frame(a.text, p.border);
    std::string body = !a.found ? std::string()
                       : a.info.summary.empty() ? std::string("Wikipedia has no English article for this artist.")
                                                : a.info.summary;
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= body.size() && !body.empty()) {
        size_t end = body.find('\n', start);
        std::string paragraph = body.substr(start, end == std::string::npos ? std::string::npos : end - start);
        for (const std::string& line : Wrap(Font::Normal, paragraph, a.text.w - S(16))) lines.push_back(line);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    const float line = LineHeight(Font::Normal);
    a.scroll = std::clamp(a.scroll, 0.0f, std::max(0.0f, lines.size() * line + S(16) - a.text.h));
    SDL_Rect clip{(int)a.text.x + 1, (int)a.text.y + 1, (int)a.text.w - 2, (int)a.text.h - 2};
    SDL_SetRenderClipRect(g.renderer, &clip);
    float y = a.text.y + S(8) - a.scroll;
    for (const std::string& text : lines) {
        if (y + line >= a.text.y && y <= a.text.B()) DrawText(Font::Normal, text, a.text.x + S(8), y, p.text, a.text.w - S(16));
        y += line;
    }
    SDL_SetRenderClipRect(g.renderer, nullptr);

    // The discography: Year, Album, Type and In library, as the Windows list's columns.
    a.missing = {panel.x + pad, a.text.B() + S(6), panel.w - pad * 2, S(22)};
    panel::DrawCheck(a.missing,"Only albums not in my library", a.missingOnly ? 1 : 0, p);
    a.list = {panel.x + pad, a.missing.B() + S(6), panel.w - pad * 2, top + middle - a.missing.B() - S(6)};
    Fill(a.list, p.surface2);
    Frame(a.list, p.border);
    const float row = RowHeight(), year = S(52), type = S(100), owned = S(90);
    const float titleX = a.list.x + S(8) + year, titleW = a.list.w - S(16) - year - type - owned;
    float hy = a.list.y + S(3);
    DrawText(Font::Small, "Year", a.list.x + S(8), hy, p.sub, year);
    DrawText(Font::Small, "Album", titleX, hy, p.sub, titleW);
    DrawText(Font::Small, "Type", titleX + titleW, hy, p.sub, type);
    DrawText(Font::Small, "In library", titleX + titleW + type, hy, p.sub, owned);
    const float rowsTop = a.list.y + row, rowsH = a.list.B() - rowsTop - 1;
    a.listScroll = std::clamp(a.listScroll, 0.0f, std::max(0.0f, a.shown.size() * row - rowsH));
    SDL_Rect rows{(int)a.list.x + 1, (int)rowsTop, (int)a.list.w - 2, (int)rowsH};
    SDL_SetRenderClipRect(g.renderer, &rows);
    for (int i = 0; i < (int)a.shown.size(); ++i) {
        const float ry = rowsTop + i * row - a.listScroll;
        if (ry + row < rowsTop || ry > a.list.B()) continue;
        const online::ReleaseGroup& r = a.discography[a.shown[i]];
        Rect band{a.list.x + 1, ry, a.list.w - 2, row};
        if (band.In(panel::g_mx, panel::g_my) && a.list.In(panel::g_mx, panel::g_my)) Fill(band, p.hover);
        const float ty = ry + S(3);
        if (r.year) DrawText(Font::Normal, std::to_string(r.year), a.list.x + S(8), ty, p.sub, year);
        DrawText(Font::Normal, r.title, titleX, ty, p.text, titleW - S(8));
        DrawText(Font::Normal, r.type, titleX + titleW, ty, p.sub, type - S(8));
        if (r.owned) DrawText(Font::Normal, "In library", titleX + titleW + type, ty, p.accent, owned);
    }
    SDL_SetRenderClipRect(g.renderer, nullptr);

    DrawText(Font::Small, a.status, panel.x + pad, bottom - S(26), p.sub, panel.w - pad * 2);
    a.musicbrainz = {panel.x + pad, bottom, S(108), S(30)};
    a.wikipedia = {a.musicbrainz.R() + S(6), bottom, S(108), S(30)};
    a.official = {a.wikipedia.R() + S(6), bottom, S(108), S(30)};
    a.close = {panel.R() - pad - S(100), bottom, S(100), S(30)};
    DrawButton(a.musicbrainz, "MusicBrainz", p, false, !a.info.musicbrainz.empty());
    DrawButton(a.wikipedia, "Wikipedia", p, false, !a.info.wikipedia.empty());
    DrawButton(a.official, "Official site", p, false, !a.info.official.empty());
    DrawButton(a.close, "Close", p, true);
}

void ArtistPanelMouseDown(float x, float y) {
    if (a.close.In(x, y)) ClosePanel();
    else if (a.musicbrainz.In(x, y)) OpenUrl(a.info.musicbrainz);
    else if (a.wikipedia.In(x, y)) OpenUrl(a.info.wikipedia);
    else if (a.official.In(x, y)) OpenUrl(a.info.official);
    else if (a.missing.In(x, y)) {
        a.missingOnly = !a.missingOnly;
        FillAlbums();
    } else if (a.list.In(x, y)) {
        const float row = RowHeight();
        const int i = (int)std::floor((y - a.list.y - row + a.listScroll) / row);
        if (y >= a.list.y + row && i >= 0 && i < (int)a.shown.size()) OpenUrl(a.discography[a.shown[i]].Url());
    }
    g.redraw = true;
}

void ArtistPanelKey(const SDL_KeyboardEvent& e) {
    if (e.key == SDLK_ESCAPE || e.key == SDLK_RETURN || e.key == SDLK_KP_ENTER) ClosePanel();
    else if (e.key == SDLK_DOWN) a.scroll += LineHeight(Font::Normal);
    else if (e.key == SDLK_UP) a.scroll -= LineHeight(Font::Normal);
    else if (e.key == SDLK_PAGEDOWN) a.listScroll += a.list.h - RowHeight() * 2;
    else if (e.key == SDLK_PAGEUP) a.listScroll -= a.list.h - RowHeight() * 2;
    g.redraw = true;
}

void ArtistPanelWheel(float x, float y, float dy) {
    if (a.text.In(x, y)) a.scroll -= dy * LineHeight(Font::Normal) * 3;
    else if (a.list.In(x, y)) a.listScroll -= dy * RowHeight() * 3;
    g.redraw = true;
}

void ArtistPanelClose() {
    StopLookup();
    a.open = false;
    a.info = {};
    a.discography.clear();
    a.shown.clear();
}

void StopArtistInfo() {
    if (a.open) ClosePanel();
    StopLookup();
}

}  // namespace lx
