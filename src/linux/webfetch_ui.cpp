// Save music from the web on Linux (Library > Save music from the web...), as app_webfetch.cpp does on
// Windows with the same wording: pasted SoundCloud, YouTube and Spotify links are resolved and saved one
// at a time on a worker into the first library folder, tagged and filed as Artist/Album/[NN ]Title. The
// panel lists each track as it is read, downloaded and saved, and remembers what already belongs to you
// and what failed for good, so nothing is fetched twice and "Forgot what failed" asks for the dead ones
// again. webfetch.h does the work; this file only posts its callbacks to the UI thread.
#include "panels_internal.h"

#include "draw.h"

#include "webfetch.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <memory>
#include <thread>

namespace lx {

namespace {

// One message to the UI thread: which run it came from, the row it is about, and a notice or the
// summary's words. A closed panel has moved on to a newer generation and drops what it gets.
struct Posted {
    uint64_t generation = 0;
    WebFetchItem item;
    std::string text;
};

struct WebFetchState {
    bool open = false;
    bool running = false;
    std::string status;
    panel::TextField paste;
    std::vector<WebFetchItem> items;  // the UI thread's mirror of the queue's list
    float scroll = 0;
    uint64_t generation = 0;
    std::thread worker;
} f;

// The worker's cancel flag outlives the state reset: a thread still winding down reads it.
std::atomic<bool> g_cancel{false};

void StopRun() {
    g_cancel = true;
    if (f.worker.joinable()) f.worker.join();
    f.running = false;
    g_cancel = false;
}

std::string TrackText(const WebFetchItem& it) {
    const std::string& title = it.track.title;
    const std::string& artist = it.track.artist;
    if (!artist.empty()) return artist + " - " + title;
    return title.empty() ? it.track.id : title;
}

// The status column's words are the Windows window's (app_webfetch.cpp); a failure says its own reason.
std::string StateText(const WebFetchItem& it) {
    switch (it.state) {
    case WebState::Resolving: return "Reading...";
    case WebState::Downloading:
        return it.percent >= 0 ? "Downloading... " + std::to_string(it.percent) + "%" : "Downloading...";
    case WebState::Saving: return "Saving...";
    case WebState::Done: return "Saved";
    case WebState::Exists: return "Already yours";
    case WebState::Failed: return it.why;
    }
    return "";
}

// One paste can hold several links: the box takes whatever is separated by spaces or line breaks, the
// same rule the Windows window's LinksOf uses.
std::vector<std::string> LinksOf(const std::wstring& pasted) {
    std::vector<std::string> links;
    std::string all = ToUtf8(pasted);
    size_t at = 0;
    while (at < all.size()) {
        while (at < all.size() && std::isspace((unsigned char)all[at])) at++;
        size_t end = at;
        while (end < all.size() && !std::isspace((unsigned char)all[end])) end++;
        if (end > at) links.push_back(all.substr(at, end - at));
        at = end;
    }
    return links;
}

void StartRun() {
    if (!f.open || f.running) return;
    std::vector<std::string> links = LinksOf(f.paste.text);
    if (links.empty()) {
        f.status = "Paste a link first: a SoundCloud, YouTube or Spotify address.";
        g.redraw = true;
        return;
    }
    if (g.settings.folders.empty()) {
        f.status = "Add a library folder first (Library > Library folders...).";
        g.redraw = true;
        return;
    }
    const std::wstring root = g.settings.folders.front(), dataDir = g.dataDir;
    f.status = "Reading the links...";
    f.items.clear();
    f.scroll = 0;
    f.running = true;
    g_cancel = false;
    const uint64_t generation = f.generation;
    f.worker = std::thread([links, root, dataDir, generation] {
        auto post = [generation](UINT msg, WPARAM wp, Posted* owned) {
            if (!PostMessageW(nullptr, msg, wp, (LPARAM)owned)) delete owned;
        };
        WebFetchRun run;
        run.links = links;
        run.libraryRoot = root;
        run.dataDir = dataDir;
        run.cancel = &g_cancel;
        run.added = [&](const WebFetchItem& item) {
            post(WM_APP_WEBFETCH_ADDED, 0, new Posted{generation, item, {}});
        };
        run.updated = [&](size_t index, const WebFetchItem& item) {
            post(WM_APP_WEBFETCH_UPDATED, (WPARAM)index, new Posted{generation, item, {}});
        };
        run.notice = [&](const std::string& text) {
            post(WM_APP_WEBFETCH_NOTICE, 0, new Posted{generation, {}, text});
        };
        std::vector<WebFetchItem> items;
        std::string error;
        RunWebFetch(run, items, error);
        // The summary the window shows once the queue is through, in the Windows window's words.
        unsigned saved = 0, had = 0, failed = 0;
        for (const WebFetchItem& it : items) {
            if (it.state == WebState::Done) saved++;
            else if (it.state == WebState::Exists) had++;
            else if (it.state == WebState::Failed) failed++;
        }
        std::string summary = std::to_string(saved) + " saved, " + std::to_string(had) + " already yours, " +
                              std::to_string(failed) + " failed.";
        if (!error.empty()) summary += "  (" + error + ")";
        post(WM_APP_WEBFETCH_DONE, 0, new Posted{generation, {}, summary});
    });
    g.redraw = true;
}

struct WebGeometry {
    Rect field, save, list, status, stop, forget, close;
};

WebGeometry Geom() {
    WebGeometry G;
    const Rect out = panel::OutputRect();
    const float w = std::min(S(640), out.w - S(16)), h = std::min(S(480), out.h - S(16));
    const Rect p{std::round((out.w - w) / 2), std::round((out.h - h) / 2), w, h};
    const float pad = S(16), rh = panel::RowH(), top = p.y + S(48);
    G.field = {p.x + pad, top, p.w - 2 * pad - S(108), rh - S(4)};
    G.save = {p.R() - pad - S(100), top, S(100), rh - S(4)};
    const float footer = p.B() - pad - rh, statusY = footer - S(24);
    G.status = {p.x + pad, statusY, p.w - 2 * pad, S(20)};
    G.list = {p.x + pad, top + rh + S(8), p.w - 2 * pad,
              std::max(S(60), statusY - S(6) - (top + rh + S(8)))};
    G.stop = {p.x + pad, footer, S(100), rh};
    G.forget = {G.stop.R() + S(8), footer, S(150), rh};
    G.close = {p.R() - pad - S(100), footer, S(100), rh};
    return G;
}

// The run's cancellable work and the status line, from the panel's buttons.
void StopSave() {
    if (!f.running) return;
    g_cancel = true;
    f.status = "Stopping after the track in flight...";
    g.redraw = true;
}

void Forget() {
    if (WebFetchForgetFailures(g.dataDir)) f.status = "Forgotten. The next save tries those tracks again.";
    g.redraw = true;
}

}  // namespace

void OpenWebFetch() {
    if (f.open) {  // already there: bring it forward, as the Windows window does
        ShowWebFetchPanel();
        return;
    }
    f.paste = panel::TextField{};
    f.status.clear();
    f.items.clear();
    f.scroll = 0;
    f.running = false;
    f.open = true;
    ++f.generation;
    ShowWebFetchPanel();
    g.redraw = true;
}

void OnWebFetchAdded(LPARAM lp) {
    std::unique_ptr<Posted> post(reinterpret_cast<Posted*>(lp));
    if (!post || !f.open || post->generation != f.generation) return;  // a closed panel drops it
    f.items.push_back(post->item);
    g.redraw = true;
}

void OnWebFetchUpdated(WPARAM index, LPARAM lp) {
    std::unique_ptr<Posted> post(reinterpret_cast<Posted*>(lp));
    if (!post || !f.open || post->generation != f.generation) return;
    if (index < f.items.size()) f.items[index] = post->item;
    g.redraw = true;
}

void OnWebFetchNotice(LPARAM lp) {
    std::unique_ptr<Posted> post(reinterpret_cast<Posted*>(lp));
    if (!post || !f.open || post->generation != f.generation) return;
    f.status = post->text;
    g.redraw = true;
}

void OnWebFetchDone(LPARAM lp) {
    std::unique_ptr<Posted> post(reinterpret_cast<Posted*>(lp));
    if (!post || !f.open || post->generation != f.generation) return;
    if (f.worker.joinable()) f.worker.join();
    f.running = false;
    g_cancel = false;
    f.status = post->text;
    g.redraw = true;
}

void DrawWebFetchPanel(const Palette& p) {
    const WebGeometry G = Geom();
    DrawFrame(p, S(640), S(480), "Save music from the web");
    panel::DrawLineField(f.paste, G.field, true, true, "", p);
    DrawButton(G.save, "Save", p, true, !f.running);

    // The list: the track's name and where saving it stands, as the Windows list's columns.
    const Rect& list = G.list;
    Fill(list, p.surface2);
    Frame(list, p.border);
    const float row = panel::RowH();
    const float stateW = std::min(S(240), std::max(S(120), list.w * 0.4f));
    const float trackX = list.x + S(8), trackW = list.w - S(16) - stateW;
    const float stateX = trackX + trackW, hy = list.y + S(3);
    DrawText(Font::Small, "Track", trackX, hy, p.sub, trackW);
    DrawText(Font::Small, "Status", stateX, hy, p.sub, stateW);
    const float rowsTop = list.y + row, rowsH = list.B() - rowsTop - 1;
    f.scroll = std::clamp(f.scroll, 0.0f, std::max(0.0f, (float)f.items.size() * row - rowsH));
    SDL_Rect clip{(int)list.x + 1, (int)rowsTop, (int)list.w - 2, (int)rowsH};
    SDL_SetRenderClipRect(g.renderer, &clip);
    for (int i = 0; i < (int)f.items.size(); ++i) {
        const float ry = rowsTop + i * row - f.scroll;
        if (ry + row < rowsTop || ry > list.B()) continue;
        const WebFetchItem& it = f.items[(size_t)i];
        const float ty = std::round(ry + (row - LineHeight(Font::Normal)) / 2);
        DrawText(Font::Normal, TrackText(it), trackX, ty, p.text, trackW - S(8));
        DrawText(Font::Normal, StateText(it), stateX, ty, p.sub, stateW - S(4));
    }
    SDL_SetRenderClipRect(g.renderer, nullptr);

    DrawText(Font::Small, f.status, G.status.x, TextY(G.status), p.sub, G.status.w);
    DrawButton(G.stop, "Stop", p, false, f.running);
    DrawButton(G.forget, "Forgot what failed", p);
    DrawButton(G.close, "Close", p);
}

void WebFetchPanelMouseDown(float x, float y) {
    const WebGeometry G = Geom();
    if (G.save.In(x, y)) return StartRun();
    if (G.stop.In(x, y)) return StopSave();
    if (G.forget.In(x, y)) return Forget();
    if (G.close.In(x, y)) return ClosePanel();
    if (G.field.In(x, y)) {
        f.paste.caret = f.paste.text.size();
        f.paste.all = false;
        SDL_StartTextInput(g.window);
        g.redraw = true;
    }
}

void WebFetchPanelKey(const SDL_KeyboardEvent& e) {
    if (e.key == SDLK_ESCAPE) return ClosePanel();
    if (e.key == SDLK_RETURN || e.key == SDLK_KP_ENTER) return StartRun();
    if ((e.mod & SDL_KMOD_CTRL) && e.key == SDLK_V) {
        // Fields paste their own text on Linux: several links copied from a page arrive on separate
        // lines, which the box keeps as spaces so the paste still splits into links.
        if (char* clip = SDL_GetClipboardText()) {
            std::wstring text = compat::Widen(clip);
            SDL_free(clip);
            for (wchar_t& c : text)
                if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
            if (panel::LineFieldInsert(f.paste, panel::Printable(text))) g.redraw = true;
        }
        return;
    }
    const float page = std::max(panel::RowH(), Geom().list.h - panel::RowH() * 2);
    if (e.key == SDLK_DOWN) f.scroll += panel::RowH();
    else if (e.key == SDLK_UP) f.scroll -= panel::RowH();
    else if (e.key == SDLK_PAGEDOWN) f.scroll += page;
    else if (e.key == SDLK_PAGEUP) f.scroll -= page;
    else {
        if (panel::LineFieldKey(f.paste, e) == 1) g.redraw = true;
        return;
    }
    g.redraw = true;
}

void WebFetchPanelText(const char* text) {
    if (panel::LineFieldInsert(f.paste, panel::Printable(compat::Widen(text)))) g.redraw = true;
}

void WebFetchPanelWheel(float x, float y, float dy) {
    if (!Geom().list.In(x, y)) return;
    f.scroll -= dy * panel::RowH() * 3;
    g.redraw = true;
}

bool WebFetchPanelWantsText(float x, float y) { return Geom().field.In(x, y); }

void WebFetchPanelClose() {
    f.open = false;
    ++f.generation;  // a run still finishing finds no panel to show in
    StopRun();
    f.paste = panel::TextField{};
    f.status.clear();
    f.items.clear();
    f.scroll = 0;
}

void StopWebFetch() {
    if (f.open) ClosePanel();  // WebFetchPanelClose joins the worker
    StopRun();
}

}  // namespace lx
