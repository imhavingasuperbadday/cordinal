// The desktop's fonts without fontconfig: an index of the installed TTF, OTF and TTC files, read
// in-repo from their name, OS/2, head, post, fvar and cmap tables and cached in
// $XDG_CACHE_HOME/cordinal, and a matcher for the fontconfig-style patterns the front end asks
// for ("Segoe UI,Selawik,sans-serif:weight=bold", "sans-serif:lang=ja", "emoji"). The desktop's
// fontconfig configuration (/etc/fonts/fonts.conf and what it includes) is read as plain XML when
// present, for its font directories, rejected files, aliases and family and language rules, so a
// pattern picks what fc-match picks; without one, built-in aliases stand in for the generic names.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lx::fonts {

struct Range {
    uint32_t first, last;  // inclusive code points
};

// One face of a font file. Weights, widths and slants are on fontconfig's scales (regular 80,
// bold 200; normal width 100; italic 100, oblique 110); a variable font gives its axis's range.
struct Face {
    int index = 0;                       // face number in a collection
    std::vector<std::string> families;   // every family name the name table gives (IDs 1, 16 and 21)
    std::string family;                  // the English family, as fontconfig reports first
    std::string style;                   // the English subfamily
    float weightMin = 80, weightMax = 80, widthMin = 100, widthMax = 100, slantMin = 0, slantMax = 0;
    int spacing = 0;                     // 0 proportional, 90 dual width, 100 monospace
    bool color = false, symbol = false;  // color glyph tables; a symbol-encoded cmap
    bool decorative = false;             // a style fontconfig calls decorative ("Shadow", "Caps")
    bool variableOnly = false;           // variable axes without named instances
    uint32_t version = 0;                // head's fontRevision, 16.16: fontconfig prefers the newest
    uint64_t langs = 0;                  // the languages (Languages()) whose letters it covers
    std::vector<Range> coverage;         // the code points its cmap maps to a glyph
    bool Covers(uint32_t ch) const;
    bool Mono() const { return spacing >= 100; }
    bool Italic() const { return slantMax > 0 && slantMin > 0; }
};

// The faces of a TTF, OTF, TTC or OTC file in memory; none for anything else. Malformed tables
// are read only within the buffer: a bad table is skipped and a face without a usable cmap or
// family name is left out.
std::vector<Face> ParseFont(const unsigned char* data, size_t size);

// The languages Face::langs counts, in bit order ("en", "ja", "zh-cn", "und-zsye"...).
const std::vector<std::string>& Languages();

struct Match {
    std::string file;
    int index = 0;
    std::string family;
};

// The best installed face for a pattern, as FcFontMatch: some face whenever any font is
// installed, false only without one. The index is built on first use and kept for the process.
bool FindFont(const std::string& pattern, Match* out);

// For the checks: the directories scanned, the index's size, and where its cache lives.
struct IndexInfo {
    std::vector<std::string> dirs;
    std::string cache;
    size_t files = 0, faces = 0, parsed = 0;  // parsed: files read this time rather than from the cache
    bool config = false;                       // a fontconfig configuration file was read
};
IndexInfo Info();
std::vector<std::string> Families(const std::string& pattern);  // the pattern's families and languages after the rules
void Reload();  // forget the in-memory index and configuration; the next lookup rescans

}  // namespace lx::fonts
