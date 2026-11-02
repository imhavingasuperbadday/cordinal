// Smart playlist rule classification, validation, evaluation and membership
// recalculation.
//
// This is a dependency-free port of the smart playlist half of libgpod's
// src/itdb_playlist.c (itdb_splr_get_field_type, itdb_splr_get_action_type,
// itdb_spl_action_known, itdb_splr_validate, itdb_splr_eval, itdb_spl_update,
// itdb_spl_update_all, itdb_spl_update_live, itdb_splr_new/add/remove and
// itdb_spl_copy_rules). The behaviour deliberately follows the reference
// implementation, quirks included, so that databases round-trip identically:
//
//  * Integer and date comparisons are performed as unsigned 64-bit values,
//    which is what the C implicit conversions in the reference code do
//    (gint64 operand promoted to guint64 by the guint64 rule fields).
//  * String comparisons are case folded first and then compared bytewise,
//    i.e. they are a match of the folded UTF-8 bytes.
//  * Rules on fields the reference evaluator never looks at (Category,
//    Description, Purchase, AlbumRating and the Sort* fields) evaluate to
//    false, because the reference field extraction switch has no case for
//    them. This is intentional parity, not an oversight.
//  * A date rule of the "in the last" kind uses the threshold
//    `now + from_date * from_units` (both stored as a negative count and a
//    number of seconds), and matches strictly newer timestamps.
#ifndef GPOD_SMARTPLAYLIST_HPP
#define GPOD_SMARTPLAYLIST_HPP

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "gpod/model.hpp"
#include "gpod/types.hpp"

namespace gpod {

// Creates a playlist like itdb_playlist_new(): normal type, manual sort,
// timestamp set, and (for a smart playlist) the reference default preferences
// and one empty rule.
[[nodiscard]] std::unique_ptr<Playlist> make_playlist(std::string name, bool smart = false);

// Returns the type of the field a rule inspects (string, int, boolean, date,
// playlist, binary-and or unknown). Equivalent to itdb_splr_get_field_type().
[[nodiscard]] SmartPlaylistFieldType field_type(const SmartPlaylistRule& rule) noexcept;

// Returns the type of the action of a rule once the field type is taken into
// account (int, range, in-the-last, string, playlist, binary-and, none,
// invalid or unknown). Equivalent to itdb_splr_get_action_type().
[[nodiscard]] SmartPlaylistActionType action_type(const SmartPlaylistRule& rule) noexcept;

// True when the raw action value is one the reference implementation knows.
// Equivalent to itdb_spl_action_known().
[[nodiscard]] bool is_action_known(SmartPlaylistAction action) noexcept;

// Normalises the unused parts of a rule for its action type, exactly like
// itdb_splr_validate(): the values that a given action does not use are reset
// to fixed defaults so that files written back to the device stay consistent.
// Returns false (leaving the rule untouched) when the rule is invalid or
// unknown; the reference implementation asserts in that case.
bool validate(SmartPlaylistRule& rule);

// Threshold used by "in the last" date rules: now + from_date * from_units.
[[nodiscard]] std::time_t in_the_last_threshold(const SmartPlaylistRule& rule,
                                                std::time_t now) noexcept;

// Evaluates one rule against one track. `now` supplies the current time for
// "in the last" date rules. Playlist membership rules consult `track.itdb`;
// without a database such a rule is false. Equivalent to itdb_splr_eval().
[[nodiscard]] bool evaluate(const SmartPlaylistRule& rule, const Track& track,
                            std::time_t now);
[[nodiscard]] bool evaluate(const SmartPlaylistRule& rule, const Track& track);

// Evaluates a whole rule set: all rules for the And operator, any rule for Or,
// and true when the set is empty. Equivalent to the rule loop of
// itdb_spl_update().
[[nodiscard]] bool evaluate(const SmartPlaylistRules& rules, const Track& track,
                            std::time_t now);
[[nodiscard]] bool evaluate(const SmartPlaylistRules& rules, const Track& track);

// A rule with the reference defaults: Artist / Contains and all values zero.
// Equivalent to itdb_splr_new().
[[nodiscard]] SmartPlaylistRule make_rule() noexcept;

// Inserts a copy of `rule` at `position` (appended when unset) and returns the
// stored rule. Equivalent to itdb_splr_add() / itdb_splr_add_new().
SmartPlaylistRule& add_rule(Playlist& playlist, const SmartPlaylistRule& rule,
                            std::optional<std::size_t> position = std::nullopt);
SmartPlaylistRule& add_new_rule(Playlist& playlist,
                                std::optional<std::size_t> position = std::nullopt);

// Removes the rule at `index`; returns false when the index is out of range.
// Equivalent to itdb_splr_remove().
bool remove_rule(Playlist& playlist, std::size_t index);

// Copies prefs and rules from `src` to `dest`, replacing whatever `dest` had.
// Both playlists must be smart; returns false and changes nothing otherwise.
// Equivalent to itdb_spl_copy_rules().
bool copy_rules(Playlist& dest, const Playlist& src);

// Rebuilds playlist.members from playlist.itdb->tracks.
//
// `random_index(bound)` must return a value in [0, bound) and is used only for
// ITDB_LIMITSORT_RANDOM; a uniform generator is used when it is empty.
// Returns false and leaves the playlist untouched when the playlist is not a
// smart playlist or has no database. Equivalent to itdb_spl_update().
using RandomIndexFn = std::function<std::uint32_t(std::uint32_t)>;
bool update(Playlist& playlist, std::time_t now, const RandomIndexFn& random_index = {});
bool update(Playlist& playlist, const RandomIndexFn& random_index = {});

// Updates every smart playlist of the database. Equivalent to
// itdb_spl_update_all(); returns how many playlists were recalculated.
std::size_t update_all(Itdb& itdb, std::time_t now);
std::size_t update_all(Itdb& itdb);

// Updates only the smart playlists whose prefs.live_update is set. Equivalent
// to itdb_spl_update_live(); returns how many playlists were recalculated.
std::size_t update_live(Itdb& itdb, std::time_t now);
std::size_t update_live(Itdb& itdb);

namespace detail {

// g_utf8_casefold() replacement. ASCII, Latin-1 and U+0178 (LATIN CAPITAL
// LETTER Y WITH DIAERESIS) are folded; other scripts are compared as-is and
// invalid UTF-8 is passed through unchanged.
[[nodiscard]] std::string casefold(std::string_view text);

}  // namespace detail

}  // namespace gpod

#endif  // GPOD_SMARTPLAYLIST_HPP
