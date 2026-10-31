// The Apollo theme's window: Apollo 37zz's main window arrangement and drawing, used while that
// theme is on in the normal window size (app_apollo.cpp).
#pragma once
#include "app_state.h"

namespace app {

bool ApolloMode();
// Follows a theme or window size change into or out of the Apollo arrangement: its rebar windows,
// the playlist's columns, the menu bar (Apollo has none) and the engine's sample tap.
void ApolloSync();
void LayoutApollo(HDWP& dwp, RECT r);
void PaintApolloChrome(HDC dc);  // the rebar: its background, band grips, labels and borders
void ApolloTheme();              // the window icon and caption, in or out of the theme
void ApolloTick();               // the status window and the window title follow playback
// The status bar: "10 tracks (45:07)" and the selection, or a message in its place.
void ApolloStatus();
std::wstring ApolloWindowTitle();
bool ApolloContextMenu(HWND from, POINT pt);
// Drawing shared with the stock controls: the position and volume bars, the list header, a status
// bar part and the playlist's Title cell (Apollo's track icon before the text).
LRESULT ApolloDrawTrackbar(NMCUSTOMDRAW* cd);
void ApolloPaintHeader(HWND header, HDC dc);
void ApolloDrawStatusPart(const DRAWITEMSTRUCT* di);
void ApolloDrawTrackIcon(HDC dc, const RECT& cell);
// Apollo's own playlist fields: apollo_size ("7510 KB", "11 MB"), apollo_bits ("16 bit stereo")
// and apollo_tag (the ID3 tag version, "v2.3.0"). The last two read the file's header once.
bool ApolloField(TrackId id, const std::wstring& name, std::wstring& out);
// Compaction renumbers tracks: its per-track file-header probes must not be reused by the new ids.
void ForgetApolloProbes();

}  // namespace app
