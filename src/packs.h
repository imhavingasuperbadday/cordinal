// Packs: customization as data (docs/plugins.md, Tier 1). A pack is a folder, or a .cordpack file (a
// ZIP, stored or deflated as Windows' "Compress to ZIP" makes it), in <data>\packs. ParsePack turns
// its files into the plain values below; app_packs.cpp finds packs, turns them on and off and
// applies them. Nothing in a
// pack is ever run: every value is text, a number or a colour, read by a lenient reader that skips
// what it doesn't know and says so in the pack's warnings.
#pragma once
#include "sfx.h"
#include "theme_palette.h"

#include <array>
#include <string>
#include <utility>
#include <vector>

// The pack format this cordinal reads. A file that says `[pack] format = N` for a later N is left
// out with a reason, never half-read.
constexpr int kPackFormat = 1;

// One file of a pack: its path inside the pack (lowercase, '/' between folders) and its bytes.
struct PackEntry {
    std::wstring path;
    std::string bytes;
};

// Lists a ZIP's files, stored or deflated. False, with a reason, for a damaged archive or another
// compression method.
bool ReadPackZip(const std::string& zip, std::vector<PackEntry>& out, std::wstring& error);

// One section of an ini file, its keys lowercase and in file order. A value wrapped in double
// quotes keeps its leading and trailing spaces.
struct IniSection {
    std::wstring name;
    std::vector<std::pair<std::wstring, std::wstring>> values;
    const std::wstring* Get(const wchar_t* key) const;
};
// UTF-8, or UTF-16 with its byte order mark. ';' and '#' start a comment line. Without `unquote`,
// values keep their quotes: a helper's command line can start and end with a quoted argument.
std::vector<IniSection> ParseIni(const std::string& bytes, bool unquote = true);
std::wstring DecodePackText(const std::string& bytes);
bool ParseRgb(const std::wstring& text, Rgb& out);  // #RRGGBB or RRGGBB

struct PackTheme {
    std::wstring name, font, mark, prefix;
    int fontPx = 0;
    Palette palette{};
    std::wstring legibility;  // the contrast rule it breaks (PaletteLegibility), or empty
    std::vector<std::pair<Sfx, std::string>> sounds;  // WAV or FLAC files that replace sound effects
    std::wstring ratingMark;                          // the glyph ratings are drawn with (empty: the star)
    Rgb wavePlayed = kNoRgb, waveRest = kNoRgb;       // the waveform's colours (kNoRgb: the palette's)
};
// The name a theme's [sounds] section gives each sound effect ("rate", "queue_add", ...), or null
// for one a theme can't replace.
const wchar_t* PackSoundName(Sfx sound);
// A PCM or float WAV file as interleaved stereo; false for anything else.
bool DecodePackWav(const std::string& bytes, unsigned& rate, std::vector<float>& stereo);
struct PackColumn {
    std::wstring key, title, display, sort;
    int width = 120;
    bool right = false;
};
// A named set of columns for the track list's header menu: column names left to right.
struct PackColumnSet {
    std::wstring name;
    std::vector<std::wstring> show;
};
// Album group headers: `by` decides which neighbouring tracks share a header; the lines are drawn in
// it. Empty strings keep cordinal's own.
struct PackGroups {
    std::wstring by, title, subtitle, right;
    bool cover = true, any = false;
};
struct PackKey {
    std::wstring keys, command;  // "Ctrl+Shift+N", "Playback > Next"
};
struct PackPreset {
    std::wstring name;
    std::array<float, 10> gains{};
};
struct PackLayout {
    std::wstring name;
    IniSection values;  // read by app_custom.cpp, which owns the layout's parts
};
struct PackAutoPlaylist {
    std::wstring name;
    std::string rules;  // as SerializeAutoPlaylist writes them (autoplaylist.h)
};
struct PackFont {
    std::wstring file;
    std::string bytes;
};

enum PackFormatSlot { kPackWindow, kPackStatus, kPackNowPlaying, kPackNowPlaying2, kPackTray, kPackFormatCount };

struct Pack {
    std::wstring id;  // the folder or file name, which settings.ini remembers
    std::wstring name, author, version, description;
    std::wstring error;                  // why none of it could be used
    std::vector<std::wstring> warnings;  // what was left out, in plain words
    std::vector<PackTheme> themes;
    std::vector<PackColumn> columns;
    std::vector<PackColumnSet> columnSets;
    PackGroups groups;
    std::wstring formats[kPackFormatCount];  // empty: the user's own stays
    std::vector<std::pair<std::wstring, std::wstring>> fields;  // fields.ini: %name% and its format
    std::vector<PackKey> keys, globalKeys;  // globalKeys work while another window is in front
    std::vector<PackPreset> presets;
    std::vector<PackLayout> layouts;
    std::vector<PackAutoPlaylist> autoPlaylists;
    std::vector<PackFont> fonts;
    std::wstring functions;  // functions.tf
    bool Empty() const;
};

// Reads one pack from its files; `id` names it.
Pack ParsePack(const std::wstring& id, const std::vector<PackEntry>& files);
// What a pack holds, in words: "a theme with 3 sounds, 2 columns and the window title".
std::wstring PackSummary(const Pack& pack);
