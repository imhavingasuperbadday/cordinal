// Declarations of the application's tools, implemented across the app_*.cpp dialog/editor modules:
// the tag, auto-playlist and organize editors, toasts and points, the games, about, and file
// dialogs. Kept apart from app.h so the main window and panels can call them without their
// definitions.
#pragma once
#include "app_state.h"
#include "convert.h"

namespace app {

void ShowAutoPlaylistEditor(int playlist);
bool AutoEditorDialogMessage(MSG* msg);
void AddDefaultAutoPlaylists();
void ShowOrganize(const std::vector<TrackId>& ids);
bool OrganizeDialogMessage(MSG* msg);
bool OrganizeBusy();
bool OrganizeUsing(TrackId id);
void OnOrganizeDone(LPARAM lp);
void UndoOrganize();
void StopOrganizer();
// Converting to .cord and exporting .cord to other formats (app_convert.cpp).
void InitConvert();
void ConvertTracks(const std::vector<TrackId>& ids, CordExport format = CordExport::None);
HMENU CordExportMenu();
bool ConvertBusy();
bool ConvertUsing(TrackId id);
void OnConvertMessage(WPARAM wp, LPARAM lp);
void UndoConversion();
bool ConvertCommand(int id);
void UpdateConvertMenu(HMENU m);
void StopConverter();
// Classic iPod sync (app_ipod.cpp).
void ShowIpod();
bool IpodDialogMessage(MSG* msg);
bool IpodBusy();
void OnIpodMessage(WPARAM wp, LPARAM lp);
void StopIpod();
// Covers (app_coverfetch.cpp): the picker for the selected tracks' album (online results, a file,
// a pasted or dropped image), or with `pick` for the tag editor, which receives the chosen image
// instead of it being saved; the batch search for albums without a cover, reviewed before anything
// is written; and the custom covers shown in cordinal only (customcover.h).
void ShowCoverSearch(const std::vector<TrackId>& ids, std::function<void(const std::string&)> pick = {});
bool HasCustomCover(const std::vector<TrackId>& ids);
void RestoreOriginalCovers(const std::vector<TrackId>& ids);
bool CoverSearchDialogMessage(MSG* msg);
void FetchMissingCovers();
void CancelCoverFetch();
bool CoverFetchBusy();
void OnCoverFetchMessage(WPARAM wp, LPARAM lp);
void StopCoverFetch();
void OnlineSettingsChanged();  // Preferences > Online changed a source
// Artist info (app_artistinfo.cpp): the selected or playing track's artist, from MusicBrainz and Wikipedia.
void ShowArtistInfo();
bool ArtistInfoDialogMessage(MSG* msg);
void StopArtistInfo();
// Copy to folder or device (app_copy.cpp).
bool CopyCommand(int id);
bool CopyBusy();
bool CopyDialogMessage(MSG* msg);
void OnCopyMessage(WPARAM wp, LPARAM lp);
void StopCopier();
// File > Export collection and Import collection (app_collection.cpp). Busy covers the
// choices asked between steps; Running only the worker, which Cancel stops.
void ExportCollection();
void ImportCollection();
void CancelCollection();
bool CollectionBusy();
bool CollectionRunning();
std::wstring CollectionCancelText();
void OnCollectionMessage(WPARAM wp, LPARAM lp);
void StopCollection();
void ShowTagEditor(const std::vector<TrackId>& ids);
bool TagEditorDialogMessage(MSG* msg);
void WriteTagsInBackground(const std::vector<TrackId>& ids, const TagEdit& edit, bool announce = true);
bool CanUndoTagEdit(std::wstring& description);
void UndoLastTagEdit();
// Unix time of the newest tag edit that can be undone, 0 without one.
uint64_t NewestTagEditTime();
bool TagWriteBusy();
bool TagWriteUsing(TrackId id);
bool TagWriterRetainsTracks();
std::wstring TagWriteStatus();
void OnTagWriteMessage(WPARAM wp, LPARAM lp);
void FlushDeferredTags();
// Shutdown only, after the engine has stopped: writes edits that waited for a playing file.
bool FlushDeferredTagWrites(std::wstring& error);
void StopTagWriter();
void AnnouncePoints(const PointsAward& a);
void ShowToast(const std::wstring& title, const std::wstring& body);
bool SubmitTagWrites(const std::vector<std::pair<TrackId, TagEdit>>& items, bool announce, bool undoable,
                     std::wstring journalAfter = {});
void BuildRows();
std::vector<std::wstring> PickPaths(bool folders);
void ImportPlaylistFile();
void ExportPlaylistFile();
void BackUpProfile();
void RestoreProfile();
void ReviewBackupFolder(const std::wstring& folder);
void ImportPlaybackStatistics();
bool BackupDialogMessage(MSG* msg);
void OnBackupMessage(LPARAM lp);
void StopBackup();
bool BackupBusy();
void CancelBackup();
std::wstring BackupStatus();
void FinishProfileRestore();
void ShowInExplorer();
void ShowProperties();
void ShowAbout();
void ShowFormatWindow();
void ShowGames(int which);
void ShowPoints();
void ShowReview();
void CreatePointsChip(HWND parent);
void AnimatePointsChip();
bool FormatDialogMessage(MSG* msg);

}  // namespace app
