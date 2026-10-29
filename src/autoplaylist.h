// Auto-playlists: rules over the library (MusicBee's auto-playlists / iTunes' smart playlists).
//
// A playlist is a group of rules matched ALL or ANY, groups can nest, and the result can be
// limited to N tracks or minutes, picked at random or by plays, rating, date added, ... The
// definition is stored as text in Playlist::rules.
#pragma once
#include "library.h"

#include <string>
#include <vector>

// The enum order matches the key/name tables in autoplaylist.cpp; append only, never reorder.
enum class RuleField : uint8_t {
    Any, Title, Artist, AlbumArtist, Album, Genre, Composer, Comment, Grouping, Label, Path, Codec,  // text
    Year, Rating, PlayCount, SkipCount, Length, Bitrate, TrackNo, Disc, Bpm,                         // numbers
    LastPlayed, DateAdded,                                                                          // dates
    Plays30, Plays365,                                                                              // numbers from the play log (playlog.h)
    Count
};

// The order matches kOpKeys in autoplaylist.cpp; append only, never reorder.
enum class RuleOp : uint8_t {
    Contains, NotContains, Is, IsNot, StartsWith, EndsWith, IsEmpty, NotEmpty,  // text
    Equals, NotEquals, AtLeast, AtMost, Between,                               // numbers (also IsEmpty / NotEmpty)
    InLastDays, NotInLastDays, Never,                                          // dates
    Count
};

struct Rule {
    RuleField field = RuleField::Artist;
    RuleOp op = RuleOp::Contains;
    std::string value, value2;  // UTF-8; numbers as text. Ratings are stars (0..5, halves allowed).
};

struct RuleGroup {
    bool all = true;
    std::vector<Rule> rules;
    std::vector<RuleGroup> groups;
};

// The order matches kPickKeys in autoplaylist.cpp; append only, never reorder.
enum class PickBy : uint8_t {
    Random, MostPlayed, LeastPlayed, HighestRated, LowestRated, RecentlyAdded, RecentlyPlayed, LeastRecentlyPlayed, Count
};

// What a limit counts. The order matches kUnitKeys in autoplaylist.cpp; append only, never reorder.
// Sizes are the library files' sizes in decimal units, like the label on a memory card.
enum class LimitUnit : uint8_t { Tracks, Minutes, Megabytes, Gigabytes, Count };

struct AutoPlaylist {
    RuleGroup root;
    uint32_t limit = 0;  // 0 = no limit
    LimitUnit unit = LimitUnit::Tracks;
    PickBy pick = PickBy::Random;
    bool ordered = false;  // keep the pick order instead of library order
};

const wchar_t* RuleFieldName(RuleField f);
const wchar_t* RuleOpName(RuleOp op);
const wchar_t* PickByName(PickBy p);
const wchar_t* LimitUnitName(LimitUnit u);
bool IsTextField(RuleField f);
bool IsDateField(RuleField f);
// Operators that make sense for a field, in menu order.
std::vector<RuleOp> OpsFor(RuleField f);

std::string SerializeAutoPlaylist(const AutoPlaylist& a);
bool ParseAutoPlaylist(const std::string& text, AutoPlaylist& out);

// The library tracks the playlist selects (in library order, or the pick order when ordered).
// `seed` makes random picks stable between rebuilds of the same view.
void EvaluateAutoPlaylist(const AutoPlaylist& a, std::vector<TrackId>& out, uint32_t seed = 1);
// An owned copy of the library/log can be evaluated on a worker without reading UI state.
struct PlayEvent;
void EvaluateAutoPlaylist(const AutoPlaylist& a, const Library& library, const std::vector<PlayEvent>& log,
                          std::vector<TrackId>& out, uint32_t seed = 1);
bool RuleMatches(const Rule& r, TrackId id);

// The default set created on first run: Recently added, Recently played, Top 25 most played,
// Top rated.
std::vector<std::pair<std::wstring, AutoPlaylist>> DefaultAutoPlaylists();
