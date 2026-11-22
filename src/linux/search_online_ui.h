// Unified online song search pane on Linux (Step 3 of docs/effortless-library.md, mirroring
// src/app_search_online.h). Shown below the track list / canvas while a query is typed and online
// search is on, or as an inline "Search online..." prompt when local results are empty and it is off.
#pragma once
#include "app.h"
#include "online_search.h"

namespace lx {

struct Rect;

bool OnlineSearchVisible();
float OnlineSearchWantedHeight(float availH);

void OnlineSearchQueryChanged(const std::wstring& query);
void OnlineSearchViewUpdated();
void OnlineSearchSettingChanged();
void OnlineSearchTrimMemory();

bool OnlineSearchFocused();
void OnlineSearchBlur();
bool OnlineSearchActivateDefault();

void OnlineSearchPlay(const online::SearchTrack& track, bool next, bool queueEnd = false);
void OnlineSearchSave(const online::SearchTrack& track);
void OnlineSearchSaveAlbum(const online::SearchTrack& track);
void OnlineSearchMenuAction(int arg);

const import::OwnedIndex& OnlineSearchOwnedSnapshot();

void DrawOnlineSearch(const Rect& r, const Palette& p, float mx, float my);
bool OnlineSearchMouseDown(float x, float y, int button, int clicks);
bool OnlineSearchWheel(float x, float y, float dy);
bool OnlineSearchKey(const SDL_KeyboardEvent& e);

void OnlineSearchTick();
void OnOnlineSearchMessage(WPARAM wp, LPARAM lp);
void OnOnlineResolveMessage(WPARAM wp, LPARAM lp);
void StopOnlineSearch();

// For self-tests (selftest_ui.cpp): inspect pane state without hitting the network.
size_t OnlineSearchRowCount();
void SetOnlineSearchTestRows(const std::vector<online::SearchTrack>& rows);

}  // namespace lx
