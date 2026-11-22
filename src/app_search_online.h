// Unified online song search pane in the main window (Step 3 of docs/effortless-library.md).
// Shown below the local track list / library view while a query is typed and online search is on
// (or as a quiet "Search online..." prompt when local results are empty and it is still off).
#pragma once
#include "app_state.h"
#include "online_search.h"

namespace app {

void CreateOnlineSearch(HWND parent);
HWND OnlineSearchWindow();
bool OnlineSearchVisible();
int OnlineSearchWantedHeight(int availH);

void OnlineSearchQueryChanged(const std::wstring& query);
void OnlineSearchTimer();
void OnlineSearchViewUpdated();
void OnlineSearchSettingChanged();
void OnlineSearchThemeChanged();
void OnlineSearchTrimMemory();

bool OnlineSearchFocus();
bool OnlineSearchActivateDefault();

// Plays, queues or saves an online search result (used by both the pane and the song picker).
void OnlineSearchPlay(const online::SearchTrack& track, bool next, bool queueEnd = false);
void OnlineSearchSave(const online::SearchTrack& track);
void OnlineSearchSaveAlbum(const online::SearchTrack& track);

// Shared snapshot helper for the song picker (app_palette.cpp) so the library is scanned at most
// once per library revision during a search.
const import::OwnedIndex& OnlineSearchOwnedSnapshot();

void OnOnlineSearchMessage(WPARAM wp, LPARAM lp);
void OnOnlineResolveMessage(WPARAM wp, LPARAM lp);
void StopOnlineSearch();

// For self-tests (selftest_ui.cpp): inspect pane state without hitting the network.
size_t OnlineSearchRowCount();
void SetOnlineSearchTestRows(const std::vector<online::SearchTrack>& rows);

}  // namespace app
