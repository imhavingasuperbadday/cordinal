// The main window's shape, shared by the Windows build and the Linux port so both draw the same
// player: the track list's columns, the menu bar's menus, the normal window's measurements at 96 DPI,
// and the layout those measurements produce at a reference size. Portable: no Win32 here.
//
// docs/ui-reference holds screenshots of the Windows window in every theme (the look to match) and
// of the Linux one. Change the window on either side and kLayout changes with it: the Windows "ui"
// self-test measures its real controls against kLayout, and the Linux build checks its own layout
// with `cordinal --check-layout` (CI runs both), so neither side can drift alone.
#pragma once

namespace ui {

// A track list column. `align` uses the list view's values (LVCFMT_LEFT / LVCFMT_RIGHT).
struct Column {
    const wchar_t* name;
    int width;  // at 96 DPI
    const wchar_t* display;
    const wchar_t* sort;
    int align;
};
constexpr int kAlignLeft = 0, kAlignRight = 1;

// Every column the track lists can show. The index is the column's id in settings.ini, so
// entries are only ever appended. Column 0 (now-playing indicator) is always shown first.
#define CD_DISC_TRACK L"[$num(%discnumber%,3)][$num(%tracknumber%,4)]"
inline const Column kColumns[] = {
    {L"",             28,  L"",                    L"",                                                kAlignLeft},
    {L"#",            44,  L"[%tracknumber%]",     CD_DISC_TRACK,                                      kAlignRight},
    {L"Title",        280, L"%title%",             L"%title%",                                         kAlignLeft},
    {L"Artist",       190, L"[%artist%]",          L"[%artist%]|[%date%]|[%album%]|" CD_DISC_TRACK,    kAlignLeft},
    {L"Album",        210, L"[%album%]",           L"[%album%]|" CD_DISC_TRACK,                        kAlignLeft},
    {L"Year",         54,  L"[%year%]",            L"[%date%]|[%album%]|" CD_DISC_TRACK,               kAlignLeft},
    {L"Duration",     70,  L"[%length%]",          L"[%length_seconds%]",                              kAlignRight},
    {L"Codec",        64,  L"[%codec%]",           L"[%codec%]",                                       kAlignLeft},
    {L"Rating",       84,  L"",                    L"[$num(%rating_value%,3)]",                        kAlignLeft},
    {L"Plays",        50,  L"[%play_count%]",      L"[$num(%play_count%,9)]",                          kAlignRight},
    {L"Album artist", 170, L"[%album artist%]",    L"[%album artist%]|[%date%]|[%album%]|" CD_DISC_TRACK, kAlignLeft},
    {L"Genre",        110, L"[%genre%]",           L"[%genre%]",                                       kAlignLeft},
    {L"Composer",     150, L"[%composer%]",        L"[%composer%]",                                    kAlignLeft},
    {L"Disc",         40,  L"[%discnumber%]",      CD_DISC_TRACK,                                      kAlignRight},
    {L"Last played",  120, L"[%last_played%]",     L"[%last_played%]",                                 kAlignLeft},
    {L"Date added",   120, L"[%added%]",           L"[%added%]",                                       kAlignLeft},
    {L"Skips",        50,  L"[%skip_count%]",      L"[$num(%skip_count%,9)]",                          kAlignRight},
    {L"Bitrate",      70,  L"[%bitrate% kbps]",    L"[$num(%bitrate%,6)]",                             kAlignRight},
    {L"Sample rate",  80,  L"[%samplerate% Hz]",   L"[$num(%samplerate%,7)]",                          kAlignRight},
    {L"File name",    200, L"%filename_ext%",      L"%filename_ext%",                                  kAlignLeft},
    {L"Path",         300, L"%path%",              L"%path%",                                          kAlignLeft},
    {L"Comment",      160, L"[%comment%]",         L"[%comment%]",                                     kAlignLeft},
    {L"Grouping",     110, L"[%grouping%]",        L"[%grouping%]",                                    kAlignLeft},
    {L"BPM",          44,  L"[%bpm%]",             L"[$num(%bpm%,4)]",                                 kAlignRight},
    {L"Label",        120, L"[%label%]",           L"[%label%]",                                       kAlignLeft},
    // The foobar2000 theme's playlist columns (app_foobar.cpp): the cover beside each album's tracks,
    // "1.01" disc and track, and the title with the track's artist where it isn't the album's.
    {L"Album cover",  200, L"",                    L"[%album artist%]|[%date%]|[%album%]|" CD_DISC_TRACK, kAlignLeft},
    {L"Track no",     40,  L"[[%discnumber%.]%tracknumber%]", CD_DISC_TRACK,                            kAlignRight},
    {L"Title / track artist", 320, L"%title%[$if($stricmp(%artist%,%album artist%),, // %artist%)]", L"%title%", kAlignLeft},
    // The Apollo theme's playlist columns (app_apollo.cpp), in Apollo's words and formats.
    {L"Artist - title", 300, L"[%artist% - ]%title%", L"[%artist%]|%title%",                          kAlignLeft},
    {L"Bit Rate",     58,  L"[%bitrate% kb/s]",    L"[$num(%bitrate%,6)]",                             kAlignRight},
    {L"Sample Rate",  78,  L"[%samplerate% Hz]",   L"[$num(%samplerate%,7)]",                          kAlignRight},
    {L"Bit Resolution", 84, L"[%apollo_bits%]",    L"[%apollo_bits%]",                                 kAlignLeft},
    {L"File Size",    62,  L"[%apollo_size%]",     L"[$num(%filesize%,12)]",                           kAlignRight},
    {L"ID3 Tag",      56,  L"[%apollo_tag%]",      L"[%apollo_tag%]",                                  kAlignRight},
};
#undef CD_DISC_TRACK
constexpr int kColumnCount = (int)(sizeof(kColumns) / sizeof(kColumns[0]));
constexpr int kColRating = 8;
constexpr int kColCover = 25, kColTrackNo = 26, kColTitleArtist = 27;
constexpr int kColApolloTitle = 28, kColApolloBitrate = 29, kColApolloRate = 30, kColApolloBits = 31, kColApolloSize = 32,
              kColApolloTag = 33;
// The columns a new profile shows, left to right.
inline constexpr int kDefaultColumns[] = {0, 1, 2, 3, 4, 5, kColRating, 6, 7};

// The menu bar, left to right ('&' marks the mnemonic).
inline constexpr const wchar_t* kMenuBar[] = {L"&File", L"&Edit", L"&Playback", L"P&laylist", L"&Library", L"&View",
                                              L"&Help"};

// The normal window, in pixels at 96 DPI. The player band runs across the top: the handle, the
// transport and order buttons, the elapsed time, the waveform seek bar, the remaining time and the
// volume; under it the now playing line (cover, title, artist line), then the Library: the tab
// strip with the search box on its right, the view row (Library tab only), the list, the status bar.
constexpr int kPad = 4;            // the window's side margin
constexpr int kGrip = 12;          // the player's handle, left of the buttons
constexpr int kBandTop = 3;        // the band's distance from the top
constexpr int kButton = 26;        // a transport button (square), and the band's height
inline constexpr int kBandWaveHeights[] = {0, 26, 44, 72, 108, 144};  // 0 follows the layout preset
constexpr int kOrderGap = 6;       // between the transport (prev, play, next) and order buttons
constexpr int kButtonsGap = 8;     // after the buttons
constexpr int kTimeWidth = 58;     // the elapsed and remaining times
constexpr int kTimeHeight = 18;
constexpr int kTimeGap = 6;        // between a time and the seek bar
constexpr int kVolumeWidth = 90;
constexpr int kSpeakerWidth = 20;  // the speaker glyph left of the volume
constexpr int kVolumeGap = 24;     // between the remaining time's room and the speaker
constexpr int kBandGap = 4;        // under the band and under the now playing line
constexpr int kCover = 32;         // the now playing cover
constexpr int kCoverGap = 8;       // between the cover and the title
constexpr int kLineHeight = 16;    // the title and the artist line
constexpr int kTabRow = 24;        // the tab strip
constexpr int kRadioTabWidth = 84;  // Radio and its close box at the right end of the tab strip
constexpr int kSearchWidth = 200;  // the search box (at most a third of the window)
constexpr int kSearchGap = 6;      // between the tabs and the search box
constexpr int kViewRow = 30;       // the Library's view row
constexpr int kViewButton = 22;    // a view button's height, 4 under the tabs
constexpr int kRow = 19;           // a track list row; an album header is two
constexpr int kStatus = 23;        // the status bar
constexpr int kMenuBarHeight = 20; // the menu bar (Windows draws it outside the client area)

// The status bar's parts, from the right edge: counts, then details, then messages.
constexpr int kStatusCounts = 250, kStatusDetails = 600;

// The foobar2000 toolbar's fixed bands; the oscilloscope takes the remaining width.
constexpr int kFoobarPanel = 288, kFoobarButton = 23, kFoobarSpectrum = 90, kFoobarVolume = 70;
constexpr int kFoobarSeek = 190, kFoobarScopeMin = 120, kFoobarThumb = 8, kFoobarThumbHeight = 16;
constexpr int kFoobarFrameMs = 16;

// The reference layout: the normal window's client area under the menu bar, kLayoutWidth by
// kLayoutHeight at 96 DPI, default layout, Library tab, Tracks view without the browser.
struct LayoutRect {
    const char* name;
    int x, y, w, h;
};
constexpr int kLayoutWidth = 1104, kLayoutHeight = 640;
inline constexpr LayoutRect kLayout[] = {
    {"previous", 12, 3, 26, 26},
    {"play", 38, 3, 26, 26},
    {"next", 64, 3, 26, 26},
    {"shuffle", 96, 3, 26, 26},
    {"repeat", 122, 3, 26, 26},
    {"resume", 148, 3, 26, 26},
    {"elapsed", 182, 8, 58, 18},
    {"seek", 246, 3, 676, 26},
    {"remaining", 928, 8, 58, 18},
    {"speaker", 990, 7, 20, 18},
    {"volume", 1010, 3, 90, 26},
    {"cover", 4, 33, 32, 32},
    {"title", 44, 33, 1056, 16},
    {"subtitle", 44, 49, 1056, 16},
    {"tabs", 0, 69, 810, 24},
    {"radio_tab", 810, 69, 84, 24},
    {"search", 900, 70, 200, 22},
    {"list", 0, 123, 1104, 494},
    {"status", 0, 617, 1104, 23},
};

}  // namespace ui
