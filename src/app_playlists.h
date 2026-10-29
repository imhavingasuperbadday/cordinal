// Playlists shared by the application modules: the list view and its columns, selection and
// grouped row mapping, the canvas library view, and playlist (tab) management.
#pragma once
#include "app_state.h"

namespace app {

// Sort ranks for a set of library strings: natural, case-insensitive order, empty values last.
// Sorting by rank is an integer compare, which keeps sorting a large library fast.
class StringRanks {
public:
    explicit StringRanks(std::vector<StringPool::Handle> handles);
    uint32_t operator()(StringPool::Handle h) const;

private:
    std::vector<std::pair<StringPool::Handle, uint32_t>> rank_;
};


// Column type (kCol*) ↔ list subitem index; the shown columns and their order come from the
// saved header layout, so a subitem index is not a fixed type.
int ColumnType(int subitem);
int SubitemOf(int type);
void LoadColumns();
void SetupColumns();
void SaveColumns();
void ToggleColumn(int type);
void ResetColumns();
void ShowColumns(const std::vector<int>& types);  // exactly these, left to right (a pack's column set)
void ShowColumnMenu(POINT pt);
void DrawStars(HDC dc, RECT r, uint8_t rating, bool selected);
uint8_t RatingAt(int x, uint8_t current);
void SetRating(const std::vector<TrackId>& ids, uint8_t rating);
HMENU RatingMenu();
void BrowserValues(TrackId id, BrowseField field, std::vector<StringPool::Handle>& out);
void FilterLibrary(std::vector<TrackId>& items);
void ClearBrowserCaches();
void OnBrowserSelection();
void ResetBrowser();
void SelectBrowserValue(BrowseField field, StringPool::Handle value);
LRESULT OnBrowserNotify(int index, NMHDR* hdr);
void ShowBrowserFieldMenu(int index, POINT pt);
void CreateBrowser(HWND parent);
void LoadBrowserSettings();
void SaveBrowserSettings();
void ThemeBrowser();
// The Library tab's canvas (album/browse) view, an alternative to the flat track list.
bool LibraryActive();
bool CanvasActive();
void SetLibraryMode(LibraryMode mode);
void ToggleBrowser();
void DrawViewButton(const DRAWITEMSTRUCT* di);
int ViewButtonWidth(int i);
StringPool::Handle Split1(StringPool::Handle h);
void CreateLibView(HWND parent);
void LibViewRebuild();
void LibViewInvalidate();
void LibViewBusyTick(bool any);
void LibViewTheme();
void LibViewDpiChanged();
void LibViewReleaseGraphics();
std::vector<TrackId> LibViewSelection();
void LibViewPlaySelection();
void SortLibraryOrder(std::vector<TrackId>& items);
void SortItems(std::vector<TrackId>& items, int column, bool ascending, std::vector<uint32_t>* mapping = nullptr);
void RebuildViewItems(Playlist& pl);
void EnsureSpecialPlaylists();
int FindPlaylist(PlaylistKind kind);
void InsertPlaylist(int at, Playlist pl);
void ErasePlaylist(int index);
void RefreshViewKeepingSelection();
void ExcludeFromLibrary(const std::vector<TrackId>& ids);
void StartScanner();
void OnScanResult(ScanResult* r, bool firstScanDone);
void UpdateInboxTab();
void AcceptInbox(const std::vector<TrackId>& ids);
void RemoveMissingTracks();
void ShowLibraryFolders();
std::wstring MusicFolder();
void LoadLibrarySettings();
void SaveLibrarySettings();
bool PathIsUnder(const std::wstring& path, const std::wstring& dir);
void ClearSelection();
void LibViewSelectAll();
void LibViewFocusFirst();
void LibViewRememberPosition(PlaylistPosition& position);
void LibViewRestorePosition(const PlaylistPosition& position);
bool MatchesFilter(TrackId id, const std::vector<std::wstring>& words);
void RebuildView(bool preserveSelection = false);
// Row ↔ view-position mapping. Album grouping inserts two header rows per album, so a list row is
// a view position plus the groups before it; nothing is stored per track.
bool GroupingActive();
int RowCount();
int ViewPosOfRow(int row);
int ViewPosNear(int row);
int RowOfViewPos(int pos);
int HeaderGroupOfRow(int row);
int HeaderRows();
int GroupOfRow(int row);
const ArtImage* GroupCover(const TrackGroup& gr, int size);
TrackId RowTrack(int row);
bool GroupHidesCell(int pos, int column, TrackId id);
void SelectGroup(int group, bool add);
void DrawGroupRow(HDC dc, int row, RECT rc);
void OnGroupArt();
void StopGroupArt();
size_t GroupArtBytes();
void ReleaseGroupArt();
bool HasBadge(int column, TrackId id);
bool TrackBusy(TrackId id);
bool AnyTrackBusy();
void DrawBusySpinner(HDC dc, int cx, int cy, COLORREF color, COLORREF bg);
void BusyTick();
std::wstring CodecBadgeText(TrackId id);
void DrawBadgeCell(HDC dc, int row, int subitem, TrackId id, COLORREF text, COLORREF bg, HFONT font, bool selected);
// Selection: indices into the active playlist's items, or the tracks themselves.
std::vector<int> SelectedIndices();
std::vector<TrackId> SelectedTracks();
int IndexIn(int pl, TrackId id);
void SetSortArrow(int column, bool ascending);
void UpdateSearchCue();
void SortByColumn(int column);
void RefreshTabs();
void ActivatePlaylist(int index);
std::wstring UniquePlaylistName(const std::wstring& base);
void NewPlaylist(const std::wstring& name);
void RemovePlaylist(int index);
void EndRename(bool commit);
LRESULT CALLBACK RenameSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR);
void BeginRename(int index);
// Editing the active list: remove / keep only selected, drop duplicates and dead items.
void RemoveTracks(bool keepSelected);
void MoveTrackBlock(const std::vector<int>& sources, int target);
void RemoveDuplicates();
void RemoveDeadItems();
void ShowListContextMenu(POINT pt);
void ShowTabContextMenu(POINT pt);
LRESULT OnListNotify(NMHDR* hdr);
LRESULT CALLBACK ListSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR);
// Playlist persistence bookkeeping: stable identities and remembered cursor positions.
void EnsurePlaylistIdentities();
void ClearCellCache();
void FlushSearch();
void CapturePlaylistPosition();
void RestorePlaylistPosition();
void ReorderPlaylist(int from, int to);
void DuplicatePlaylist();
void ReopenPlaylist();

}  // namespace app
