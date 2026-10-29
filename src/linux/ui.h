// The Linux front end's window contents, all drawn with the SDL renderer (ui.cpp).
#pragma once
#include "cover.h"

#include <string>

#include <SDL3/SDL.h>

namespace lx {

void InitUi();
void ShutdownUi();
void Render();
void HandleEvent(const SDL_Event& e);
void SetCover(CoverImage* image);  // takes ownership
void SetThumb(CoverImage* image);  // an album header's cover; takes ownership
void RefreshTrackArt(TrackId id);  // a file's size or modification time changed
void OnFoldersChosen(std::vector<std::wstring>* folders);  // takes ownership
void OnFileChosen(int purpose, std::wstring* path);        // WM_APP_FILE; takes ownership
void OnPlaylistFile(WPARAM kind, LPARAM value);
void TickPlaylistImport();
bool PlaylistImportPending();
void StopPlaylistFiles();
void MaybeOfferSteamSoundtracks();
void OnScaleChanged();
void ApplyTheme();  // the fonts follow the theme
bool UiAnimating();  // live visualizations request a short frame interval

// `cordinal --check-layout`: lays the window out at ui::kLayoutWidth x kLayoutHeight (under the menu
// bar) and compares every part with ui::kLayout, the Windows build's measured layout. Returns the
// number of parts out of place, printing each.
int CheckLayout();
// `cordinal --capture-ui DIR`: renders the window in every theme, with each menu open, into DIR as
// PNG files named like docs/ui-reference/windows/ (linux/capture-ui.sh). Returns nonzero on failure.
int CaptureUi(const std::string& dir);
bool UiBusy();       // album covers or the waveform still computing
void OnPeaksReady(uint32_t request);  // WM_APP_PEAKS: the seek bar's waveform may be ready
void CapturePump();  // main.cpp: handles what workers posted until the library and covers settle

}  // namespace lx
