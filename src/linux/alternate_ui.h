// Private theme layouts and waveform appearance on Linux. Transient theme views keep the user's
// normal columns and library filters intact; settings text contains only the optional theme views.
#pragma once
#include "draw.h"
#include <map>

class Library;
namespace lx {
struct MenuItem;
struct AlbumListNode {
    std::wstring label, key;
    std::vector<TrackId> tracks;
    std::vector<AlbumListNode> children;
};
std::vector<AlbumListNode> BuildAlbumList(const Library& library, int view, const std::wstring& filter);
struct WaveAppearance {
    int style = 2;  // Bars, Fountain bars, Dark Fountain; the Windows Waveform/Left contract
    bool motion = true, drColors = true, glow = true, black = true;
    Rgb played[4] = {kNoRgb, kNoRgb, kNoRgb, kNoRgb};
    Rgb rest[4] = {kNoRgb, kNoRgb, kNoRgb, kNoRgb};
};
WaveAppearance& FountainAppearance();
bool FountainStyleShown();
bool DrawFountain(const Rect& area, double progress, const Palette& palette,
                  const uint8_t* peaks, size_t count, bool live, bool vertical);
Rgb WavePlayedColor(const Palette& palette);
Rgb WaveRestingColor(const Palette& palette);
Rgb WaveRestColor(const Palette& palette, bool live);
void LoadAlternateSettings(const std::map<std::string, std::string>& values);
std::string AlternateSettingsText();
struct ApolloFileInfo {
    uint64_t size = 0;
    uint8_t bits = 0, id3 = 0, id3rev = 0, id3v1 = 0;
};
ApolloFileInfo ProbeApolloFile(const std::wstring& path, Codec codec);
bool AlternateField(TrackId id, const std::wstring& name, std::wstring& out);
void TickAlternate();
void StopAlternate();
bool AlternateMode();
void ApplyAlternateLayout();
void AlternateThemeChanged();
void AlternateCatalogChanged();
bool DrawAlternate(const Palette& palette);
bool DrawAlternateCell(TrackId id, int column, const Rect& cell, Font font, const Palette& palette, int row);
bool DrawAlternateStatus(const Palette& palette);
void DrawAlternateOverlay(const Palette& palette);
bool AlternateMouseDown(float x, float y, int button, int clicks);
bool AlternateWheel(float x, float y, float delta);
bool AlternateKey(const SDL_KeyboardEvent& event);
bool AlternateText(const char* text);
void FilterAlternateLibraryTracks(std::vector<TrackId>& tracks);
const wchar_t* ActiveColumnHeading(int column);
std::vector<int>& ActiveColumnTypes();
std::vector<int>& ActiveColumnWidths();
std::vector<MenuItem> WaveAppearanceMenu();
bool RunAlternateAction(int action, int argument);
void ReleaseAlternateTextures();
int CheckLinuxAlternateThemes();
}
