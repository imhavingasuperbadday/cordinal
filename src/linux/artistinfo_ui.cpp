// Artist info on Linux (View > Artist info..., or a track's right-click menu), as app_artistinfo.cpp
// does on Windows with the same wording: the selected or playing track's artist as MusicBrainz knows
// them, the opening of their English Wikipedia article, and buttons that open MusicBrainz, Wikipedia
// and the official site in the browser. The lookup (metadata_fetch.h) runs on a worker and is off
// until Preferences > Online turns it on; a namesake is never shown.
#include "panels.h"
#include "ui_internal.h"

#include "draw.h"
#include "metadata_fetch.h"

#include <memory>

namespace lx {

namespace {

struct Result {
    uint64_t generation;
    bool found;
    online::ArtistInfo info;
};

struct ArtistState {
    bool open = false;
    std::string artist, status;
    online::ArtistInfo info;
    bool found = false;
    float scroll = 0;
    online::Generation generation;
    std::shared_ptr<online::Cancel> cancel;
    std::thread worker;
    Rect text, musicbrainz, wikipedia, official, close;
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
                     "It sends the artist's name to musicbrainz.org, then asks wikidata.org and en.wikipedia.org "
                     "for the article MusicBrainz names.\n\nTurn Artist info on? It can be changed in Preferences > Online.",
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
    a.status = "Asking MusicBrainz...";
    const uint64_t generation = a.generation.Next();
    a.cancel = std::make_shared<online::Cancel>();
    a.worker = std::thread([artist, generation, cancel = a.cancel] {
        auto* result = new Result{generation, false, {}};
        result->found = online::FetchArtistInfo(artist, {}, *cancel, result->info);
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
    if (!a.found)
        a.status = "MusicBrainz has no single artist named \xE2\x80\x9C" + a.artist +
                   "\xE2\x80\x9D, so nothing is shown rather than a namesake.";
    else
        a.status = a.info.summary.empty() ? "From MusicBrainz." : "From MusicBrainz and Wikipedia (text under CC BY-SA 4.0).";
    g.redraw = true;
}

void DrawArtistPanel(const Palette& p) {
    Rect panel = DrawFrame(p, S(580), S(440), "Artist info");
    const float pad = S(16);
    DrawText(Font::Bold, a.found ? a.info.name : a.artist, panel.x + pad, panel.y + S(46), p.text, panel.w - pad * 2);
    float bottom = panel.B() - pad - S(30);
    a.text = {panel.x + pad, panel.y + S(74), panel.w - pad * 2, bottom - S(36) - panel.y - S(74)};
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
}

void ArtistPanelKey(const SDL_KeyboardEvent& e) {
    if (e.key == SDLK_ESCAPE || e.key == SDLK_RETURN || e.key == SDLK_KP_ENTER) ClosePanel();
    else if (e.key == SDLK_DOWN) a.scroll += LineHeight(Font::Normal);
    else if (e.key == SDLK_UP) a.scroll -= LineHeight(Font::Normal);
    g.redraw = true;
}

void ArtistPanelWheel(float x, float y, float dy) {
    if (a.text.In(x, y)) a.scroll -= dy * LineHeight(Font::Normal) * 3;
    g.redraw = true;
}

void ArtistPanelClose() {
    StopLookup();
    a.open = false;
    a.info = {};
}

void StopArtistInfo() {
    if (a.open) ClosePanel();
    StopLookup();
}

}  // namespace lx
