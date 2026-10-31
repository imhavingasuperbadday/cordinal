// Album edition UI: the compact edition manager, track-version comparison, commands and the
// debounced background grouping pass. The data model and all edits live in album_editions.*.
#pragma once
#include "app_state.h"

namespace app {

// Opens (or focuses) the edition manager for one group. The group id is stable for the profile's
// lifetime; the dialog looks it up again every time it refreshes.
void ShowAlbumEditionsDialog(uint32_t groupId);
// Compares two editions in a group. `focusTrack` selects the row for a specific song's versions
// when it is nonzero; otherwise the preferred and first other edition are compared.
void ShowEditionComparisonDialog(uint32_t groupId, TrackId focusTrack = kNone, uint32_t editionA = 0,
                                 uint32_t editionB = 0);
bool EditionDialogMessage(MSG* msg);
bool EditionCommand(int id);
void UpdateEditionMenu(HMENU menu);
void AppendEditionMenu(HMENU menu);
void RequestAlbumGrouping();
void AlbumGroupingTick();
void MusicBrainzTick();
// Called after an edit in the dialog or canvas: rebuild the view and mark the profile dirty.
void AlbumGroupChangedInApp();

}  // namespace app
