// Quiet Release Radar on Linux, as the Windows build's app_releaseradar.cpp: recent MusicBrainz
// albums by the artists the library says someone cares about, in a small list to save. The check
// and every album's tracklist read run on a worker, one MusicBrainz request at a time; saving
// hands the albums to the web-save panel, so they land as tagged files rather than a playlist.
#include "panels_internal.h"

#include "metadata_fetch.h"
#include "release_radar.h"

#include <memory>
#include <thread>

namespace lx {
namespace panel {
namespace {

struct State {
    bool open = false;
    bool running = false;
    std::string status;
    std::vector<radar::Item> items;
    int selected = -1;
    float scroll = 0;
    Rect panel, list, save, saveAll, check, close;
    std::thread worker;
    std::shared_ptr<online::Cancel> cancel;
} s;

void StopRun() {
    if (s.cancel) s.cancel->Stop();
    if (s.worker.joinable()) s.worker.join();
    s.cancel.reset();
    s.running = false;
}

void SetBusy(bool busy) { s.running = busy; }

// The artists and their album titles are read once on the UI thread; the worker never touches the
// library. Each artist is checked in turn and its new releases posted as they arrive.
void StartCheck() {
    if (!s.open || s.running) return;
    std::vector<radar::Artist> artists = radar::TopArtists(g_lib, 8);
    if (artists.empty()) {
        s.status = "The library has no artists to check yet.";
        g.redraw = true;
        return;
    }
    struct Job {
        std::string artist;
        std::vector<std::string> albums;
    };
    auto jobs = std::make_shared<std::vector<Job>>();
    for (const radar::Artist& artist : artists) jobs->push_back({artist.name, online::LibraryAlbumsBy(g_lib, artist.name)});
    s.items.clear();
    s.selected = -1;
    s.scroll = 0;
    SetBusy(true);
    s.status = "Checking " + jobs->front().artist + "...";
    s.cancel = std::make_shared<online::Cancel>();
    auto cancel = s.cancel;
    const std::string since = radar::SinceDate(std::time(nullptr), 6);
    s.worker = std::thread([jobs, cancel, since] {
        auto post = [](WPARAM kind, auto* owned) {
            if (!PostMessageW(nullptr, WM_APP_RADAR, kind, (LPARAM)owned)) delete owned;
        };
        size_t found = 0;
        for (const Job& job : *jobs) {
            if (cancel->Stopped()) break;
            post(1, new std::wstring(L"Checking " + FromUtf8(job.artist) + L"..."));
            std::vector<radar::Item> rows;
            if (!radar::RecentReleases(job.artist, since, rows, *cancel)) {
                post(1, new std::wstring(L"MusicBrainz could not be asked about " + FromUtf8(job.artist) + L"."));
                continue;
            }
            radar::MarkOwned(rows, job.albums);
            std::erase_if(rows, [](const radar::Item& item) { return item.owned; });
            found += rows.size();
            if (!rows.empty()) post(0, new std::vector<radar::Item>(std::move(rows)));
        }
        std::wstring done = cancel->Stopped() ? L"Stopped."
                            : found == 0 ? L"Nothing new from those artists right now."
                                         : L"Found " + std::to_wstring(found) + (found == 1 ? L" new release." : L" new releases.");
        post(3, new std::wstring(std::move(done)));
    });
    g.redraw = true;
}

// Reads the chosen releases' tracklists on a worker, then hands the albums to the save panel.
void SaveItems(std::vector<radar::Item> items) {
    if (!s.open || s.running || items.empty()) return;
    SetBusy(true);
    s.status = "Reading the albums...";
    s.cancel = std::make_shared<online::Cancel>();
    auto cancel = s.cancel;
    s.worker = std::thread([items = std::move(items), cancel] {
        auto post = [](WPARAM kind, auto* owned) {
            if (!PostMessageW(nullptr, WM_APP_RADAR, kind, (LPARAM)owned)) delete owned;
        };
        auto albums = new std::vector<radar::Album>();
        size_t missing = 0;
        for (const radar::Item& item : items) {
            if (cancel->Stopped()) break;
            radar::Album album;
            if (radar::FetchAlbum(item, album, *cancel)) albums->push_back(std::move(album));
            else ++missing;
        }
        if (missing)
            post(1, new std::wstring(L"MusicBrainz had no tracklist for " + std::to_wstring(missing) +
                                     (missing == 1 ? L" release." : L" releases.")));
        post(2, albums);
        post(3, new std::wstring(L"Starting the save..."));
    });
    g.redraw = true;
}

void Layout(const Palette& p) {
    Rect out = DrawFrame(p, S(640), S(460), "New releases");
    s.panel = out;
    const float pad = S(16), bottom = out.B() - pad - S(30);
    s.list = {out.x + pad, out.y + S(46), out.w - pad * 2, std::max(S(80), bottom - out.y - S(60))};
    float x = out.x + pad;
    s.save = {x, bottom, S(130), S(30)};
    s.saveAll = {x + S(136), bottom, S(100), S(30)};
    s.check = {x + S(242), bottom, S(120), S(30)};
    s.close = {out.R() - pad - S(90), bottom, S(90), S(30)};
}

std::vector<radar::Item> SelectedItems() {
    if (s.selected >= 0 && s.selected < (int)s.items.size()) return {s.items[s.selected]};
    return {};
}

void Open() {
    if (s.open) return;
    if (!online::Config().releaseRadar) {
        if (!Confirm("Release Radar is off, so cordinal hasn't contacted anyone.\n\n"
                     "It asks MusicBrainz about the artists you play and heart most, and lists their albums "
                     "from the last six months so you can save any you want as your own files. No account is "
                     "needed and nothing is downloaded until you choose Save.\n\nTurn Release Radar on? It "
                     "can be changed in Preferences > Online.",
                     "Release Radar"))
            return;
        online::Config().releaseRadar = true;
        SaveSettings();
    }
    ClosePanel();  // no other dialog stays open under this one
    s = State{};
    s.open = true;
    g_kind = Kind::ReleaseRadar;
    StartCheck();
    g.redraw = true;
}

void Close() {
    s.open = false;
    StopRun();
    s.items.clear();
    s.status.clear();
    s.selected = -1;
}

}  // namespace

void OpenReleaseRadar() {
    if (s.open) return;
    Open();
}

void OnReleaseRadarMessage(WPARAM kind, LPARAM lp) {
    if (!s.open) {
        switch (kind) {
            case 0: delete reinterpret_cast<std::vector<radar::Item>*>(lp); break;
            case 1: case 3: delete reinterpret_cast<std::wstring*>(lp); break;
            case 2: delete reinterpret_cast<std::vector<radar::Album>*>(lp); break;
        }
        return;
    }
    if (kind == 0) {
        std::unique_ptr<std::vector<radar::Item>> rows(reinterpret_cast<std::vector<radar::Item>*>(lp));
        if (rows) s.items.insert(s.items.end(), rows->begin(), rows->end());
        g.redraw = true;
    } else if (kind == 1) {
        std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lp));
        if (text) s.status = ToUtf8(*text);
        g.redraw = true;
    } else if (kind == 2) {
        std::unique_ptr<std::vector<radar::Album>> albums(reinterpret_cast<std::vector<radar::Album>*>(lp));
        if (albums && !albums->empty()) {
            std::vector<WebFetchTrackList> lists;
            for (const radar::Album& album : *albums) lists.push_back(radar::AlbumToWebFetchList(album));
            WebFetchLists(std::move(lists));  // closes this panel and opens the save panel
        }
    } else if (kind == 3) {
        std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lp));
        if (text) s.status = ToUtf8(*text);
        if (s.worker.joinable()) s.worker.join();
        s.worker = std::thread();
        s.cancel.reset();
        SetBusy(false);
        g.redraw = true;
    }
}

void OpenRadarPanel() { Open(); }

void RadarMessage(WPARAM kind, LPARAM lp) { OnReleaseRadarMessage(kind, lp); }

void DrawReleaseRadarPanel(const Palette& p) {
    Layout(p);
    const Rect& panel = s.panel;
    DrawText(Font::Small, "Albums from the last six months by the artists you play and heart most.",
             panel.x + S(16), panel.y + S(26), p.sub, panel.w - S(32));
    Fill(s.list, p.surface2);
    Frame(s.list, p.border);
    const int first = (int)(s.scroll / S(28));
    const int rows = (int)(s.list.h / S(28)) + 1;
    for (int i = first; i < (int)s.items.size() && i < first + rows; ++i) {
        const radar::Item& item = s.items[i];
        const float y = s.list.y + i * S(28) - s.scroll;
        Rect row{s.list.x, y, s.list.w, S(28)};
        if (i == s.selected) Fill(row, p.select);
        const uint32_t color = i == s.selected ? p.selectText : p.text;
        const float dateW = S(90);
        DrawText(Font::Normal, item.artist + "  \xE2\x80\x94  " + item.title, row.x + S(8), TextY(row),
                 color, row.w - dateW - S(16));
        DrawText(Font::Small, item.firstDate, row.R() - dateW, TextY(row, Font::Small), i == s.selected ? p.selectText : p.sub,
                 dateW - S(8), 2);
    }
    if (s.items.empty())
        DrawText(Font::Normal, s.running ? "Checking..." : "Nothing to show yet.", s.list.x + S(10), s.list.y + S(10), p.sub,
                 s.list.w - S(20));
    DrawText(Font::Small, s.status, panel.x + S(16), s.save.y - S(24), p.sub, panel.w - S(32));
    DrawButton(s.save, "Save selected", p, true, !s.running && s.selected >= 0);
    DrawButton(s.saveAll, "Save all", p, false, !s.running && !s.items.empty());
    DrawButton(s.check, s.running ? "Stop" : "Check again", p, false, !s.items.empty() || s.running);
    DrawButton(s.close, "Close", p, false, true);
}

void ReleaseRadarPanelMouseDown(float x, float y, int clicks) {
    if (s.close.In(x, y)) {
        ClosePanel();
        return;
    }
    if (s.running && s.check.In(x, y)) {
        if (s.cancel) s.cancel->Stop();
        return;
    }
    if (s.save.In(x, y) && !s.running) {
        SaveItems(SelectedItems());
        return;
    }
    if (s.saveAll.In(x, y) && !s.running) {
        SaveItems(s.items);
        return;
    }
    if (s.check.In(x, y) && !s.running) {
        StartCheck();
        return;
    }
    if (s.list.In(x, y)) {
        const int row = (int)((y - s.list.y + s.scroll) / S(28));
        if (row >= 0 && row < (int)s.items.size()) {
            s.selected = row;
            if (clicks >= 2) SaveItems(SelectedItems());
        }
        g.redraw = true;
    }
}

void ReleaseRadarPanelWheel(float, float, float dy) {
    if (s.items.empty()) return;
    const float total = (float)s.items.size() * S(28);
    s.scroll = std::clamp(s.scroll - dy * S(28) * 3, 0.0f, std::max(0.0f, total - s.list.h));
    g.redraw = true;
}

void ReleaseRadarPanelKey(const SDL_KeyboardEvent& e) {
    if (e.key == SDLK_ESCAPE) {
        ClosePanel();
        return;
    }
    if (e.key == SDLK_UP || e.key == SDLK_DOWN) {
        if (s.items.empty()) return;
        s.selected = std::clamp(s.selected + (e.key == SDLK_DOWN ? 1 : -1), 0, (int)s.items.size() - 1);
        const float top = s.selected * S(28), bottom = top + S(28);
        if (top < s.scroll) s.scroll = top;
        if (bottom > s.scroll + s.list.h) s.scroll = bottom - s.list.h;
        g.redraw = true;
        return;
    }
    if (e.key == SDLK_RETURN || e.key == SDLK_KP_ENTER) {
        if (!s.running) SaveItems(SelectedItems());
        return;
    }
    if (e.key == SDLK_A && !s.running) {
        SaveItems(s.items);
        return;
    }
    if (e.key == SDLK_R && !s.running) StartCheck();
}

void ReleaseRadarPanelClose() { Close(); }

const Ops kRadarOps{
    .draw = DrawReleaseRadarPanel,
    .mouseDown = [](float x, float y, int clicks) { ReleaseRadarPanelMouseDown(x, y, clicks); },
    .wheel = [](float x, float y, float dy) { ReleaseRadarPanelWheel(x, y, dy); },
    .key = ReleaseRadarPanelKey,
    .close = ReleaseRadarPanelClose,
};

}  // namespace panel

void OpenReleaseRadar() { panel::OpenRadarPanel(); }

void OnReleaseRadarMessage(WPARAM kind, LPARAM lp) { panel::RadarMessage(kind, lp); }

}  // namespace lx
