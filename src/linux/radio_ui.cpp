// Radio tab's directory, favorites and playback state for the Linux front end.
#include "app.h"
#include "radio_ui.h"

namespace lx {
namespace {
RadioState state;
}
RadioState& InternetRadio() { return state; }
void InitRadio() { state.browser = std::make_unique<radio::Browser>(g.dataDir); RadioRebuild(); }
void ShutdownRadio() { RadioCancelPlayback(); state.browser.reset(); }
void OpenRadio() {
    state.tabOpen = true;
    if (state.active) return;
    state.oldSearch = g.search; g.search = state.query;
    state.active = true; state.filterFocus = 0; RadioRebuild(); g.redraw = true;
}
void CloseRadio() {
    state.tabOpen = false;
    if (state.active) SetActiveTab(g.active);
    g.redraw = true;
    ShowMessage(L"Radio closed. Ctrl+Shift+R reopens it.");
}
void LeaveRadio() {
    if (!state.active) return;
    state.query = g.search; g.search = state.oldSearch; state.active = false;
    state.filterFocus = 0; g.searchFocused = false; SDL_StopTextInput(g.window); g.redraw = true;
}
void RadioRebuild() {
    if (!state.browser) return;
    state.rows.clear();
    const auto& source = state.discover ? state.browser->Results() : state.browser->Favorites();
    radio::Search filter{ToUtf8(state.query), {}, {}, 0, {}};
    for (const auto& station : source) {
        if (!state.discover && !radio::StationMatches(station, filter)) continue;
        state.rows.push_back(station);
    }
    state.selected = std::clamp(state.selected, 0, std::max(0, (int)state.rows.size() - 1));
    g.redraw = true;
}
void RadioSearch() {
    if (!state.browser) return;
    state.query = g.search;
    if (state.discover) {
        // Drop the previous view before the worker starts so Discover never shows My stations.
        state.rows.clear();
        state.browser->SearchDirectory({ToUtf8(state.query), ToUtf8(state.genre), ToUtf8(state.country), 0, ToUtf8(state.region)});
        if (!state.browser->Searching() && !state.browser->Problem().empty()) ShowMessage(state.browser->Problem());
        state.scroll = 0;
    } else RadioRebuild();
    g.redraw = true;
}
void RadioSetMode(bool discover) {
    if (state.discover == discover && (!discover || state.browser->Searching())) return;
    state.discover = discover; state.selected = 0; state.scroll = 0;
    if (discover) RadioSearch(); else RadioRebuild();
}
void RadioToggleFavorite(int index) {
    if (!state.browser || index < 0 || index >= (int)state.rows.size()) return;
    auto station = state.rows[(size_t)index]; bool was = state.browser->Favorite(station);
    if (!state.browser->ToggleFavorite(station)) ShowMessage(state.browser->Problem());
    else ShowMessage(was ? L"Station removed from favorites." : L"Station saved to My stations.");
    RadioRebuild();
}
void RadioPlay(int index) {
    if (index < 0 || index >= (int)state.rows.size()) return;
    radio::Station station = state.rows[(size_t)index];
    StartRadio(station);
}
void RadioBegin(const radio::Station& station, unsigned long long token) {
    state.playing = station; state.token = token; state.started = false; state.lastTitle.clear();
    if (state.browser) state.browser->RecordPlay(station.id);
    state.stream = radio::Stream::Open(station.url);
    PlayItem item; item.token = token; item.live = state.stream;
    g.engine.Play(item); g.engine.SetPaused(false);
    ShowMessage(L"Connecting to " + FromUtf8(station.name) + L"...");
    g.redraw = true; MprisChanged();
}
void RadioCancelPlayback() {
    if (state.stream) state.stream->Abort();
    state.stream.reset(); state.token = 0; state.playing = {}; state.started = false; state.lastTitle.clear();
    g.redraw = true;
}
bool RadioPlaying() { return state.token != 0; }
bool RadioEngineEvent(EngineEvent event, unsigned long long token) {
    if (!state.token || (token && token != state.token)) return false;
    if (event == EngineEvent::Ended && !state.started) return true;
    if (event == EngineEvent::NeedNext) { g.engine.SetNext(state.token, nullptr); return true; }
    if (event == EngineEvent::TrackStarted) {
        state.started = true; ShowMessage(L"Playing " + FromUtf8(state.playing.name) + L".");
        g.redraw = true; MprisChanged(); return true;
    }
    if (event == EngineEvent::Error || event == EngineEvent::Damaged || event == EngineEvent::Ended) {
        std::wstring error = event == EngineEvent::Ended ? L"The station stopped streaming." : g.engine.TakeError(token);
        if (error.empty()) error = L"Couldn't play this station. Try again.";
        RadioCancelPlayback(); g.engine.Stop(); ShowMessage(error); MprisChanged(); return true;
    }
    return true;
}
void RadioTick() {
    if (state.browser) {
        bool searching = state.browser->Searching();
        if (state.browser->Poll()) {
            RadioRebuild();
            if (searching && !state.browser->Searching() && !state.browser->Problem().empty())
                ShowMessage(state.browser->Problem());
        }
    }
    if (state.token && state.stream) {
        std::wstring title = FromUtf8(state.stream->Title());
        if (title != state.lastTitle) { state.lastTitle = std::move(title); g.redraw = true; MprisChanged(); }
    }
}
}  // namespace lx
