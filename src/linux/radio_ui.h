// Shared state for the Linux radio tab. The UI owns this; network workers return copied results.
#pragma once
#include "engine.h"
#include "radio_browser.h"
#include "radio_stream.h"

namespace lx {
struct RadioState {
    std::unique_ptr<radio::Browser> browser;
    std::shared_ptr<radio::Stream> stream;
    std::vector<radio::Station> rows;
    radio::Station playing;
    std::wstring query, genre, country = L"US", region, oldSearch, lastTitle;
    unsigned long long token = 0;
    bool active = false, discover = false, started = false;
    bool tabOpen = true;
    bool editSelectAll = false;
    int selected = 0, filterFocus = 0;
    float scroll = 0;
};
RadioState& InternetRadio();
void InitRadio();
void ShutdownRadio();
void OpenRadio();
void CloseRadio();
void LeaveRadio();
void RadioRebuild();
void RadioSearch();
void RadioSetMode(bool discover);
void RadioToggleFavorite(int index);
void RadioPlay(int index);
void RadioBegin(const radio::Station& station, unsigned long long token);
void RadioCancelPlayback();
bool RadioEngineEvent(EngineEvent event, unsigned long long token);
bool RadioPlaying();
void RadioTick();
void StartRadio(const radio::Station& station);
}  // namespace lx
