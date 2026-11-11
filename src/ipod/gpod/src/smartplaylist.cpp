// Smart playlist rule classification, validation, evaluation and membership
// recalculation. See include/gpod/smartplaylist.hpp for the behaviour contract
// and for the mapping to the reference C implementation in itdb_playlist.c.

#include "gpod/smartplaylist.hpp"

#include <algorithm>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace gpod {
namespace {

// g_utf8_casefold() folds this code point to LATIN SMALL LETTER Y WITH
// DIAERESIS, which is outside the simple offset ranges below.
constexpr std::uint32_t kUpperYDiaeresis = 0x178;
constexpr std::uint32_t kLowerYDiaeresis = 0xFF;

std::uint32_t fold_code_point(std::uint32_t cp) noexcept {
    if (cp >= 'A' && cp <= 'Z') {
        return cp + 0x20;
    }
    if ((cp >= 0xC0 && cp <= 0xD6) || (cp >= 0xD8 && cp <= 0xDE)) {
        return cp + 0x20;
    }
    if (cp == kUpperYDiaeresis) {
        return kLowerYDiaeresis;
    }
    return cp;
}

void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// The rule fields that the reference evaluator reads as strings. Fields outside
// this list (the Sort* family, Category, Description) are never compared; the
// reference switch has no case for them and falls through to "no match".
const std::string* string_field(const Track& track, SmartPlaylistField field) noexcept {
    switch (field) {
    case SmartPlaylistField::SongName:
        return &track.title;
    case SmartPlaylistField::Album:
        return &track.album;
    case SmartPlaylistField::Artist:
        return &track.artist;
    case SmartPlaylistField::Genre:
        return &track.genre;
    case SmartPlaylistField::Kind:
        return &track.filetype;
    case SmartPlaylistField::Comment:
        return &track.comment;
    case SmartPlaylistField::Composer:
        return &track.composer;
    case SmartPlaylistField::Grouping:
        return &track.grouping;
    case SmartPlaylistField::AlbumArtist:
        return &track.albumartist;
    case SmartPlaylistField::TvShow:
        return &track.tvshow;
    default:
        return nullptr;
    }
}

// The rule fields the reference evaluator reads as integers, including the
// binary-and (mediatype) field that shares the same accumulator.
bool int_field(const Track& track, SmartPlaylistField field, std::int64_t& out) noexcept {
    switch (field) {
    case SmartPlaylistField::Bitrate:
        out = track.bitrate;
        return true;
    case SmartPlaylistField::SampleRate:
        out = track.samplerate;
        return true;
    case SmartPlaylistField::Year:
        out = track.year;
        return true;
    case SmartPlaylistField::TrackNumber:
        out = track.track_nr;
        return true;
    case SmartPlaylistField::Size:
        out = track.size;
        return true;
    case SmartPlaylistField::PlayCount:
        out = track.playcount;
        return true;
    case SmartPlaylistField::DiscNumber:
        out = track.cd_nr;
        return true;
    case SmartPlaylistField::Bpm:
        out = track.bpm;
        return true;
    case SmartPlaylistField::Rating:
        out = track.rating;
        return true;
    case SmartPlaylistField::Time:
        out = track.tracklen;
        return true;
    case SmartPlaylistField::SeasonNr:
        out = track.season_nr;
        return true;
    case SmartPlaylistField::SkipCount:
        out = track.skipcount;
        return true;
    case SmartPlaylistField::VideoKind:
        out = static_cast<std::int64_t>(track.mediatype);
        return true;
    default:
        return false;
    }
}

// Boolean fields: only Compilation is read; the reference switch has no case
// for Purchase, so that rule never matches.
bool bool_field(const Track& track, SmartPlaylistField field, bool& out) noexcept {
    if (field == SmartPlaylistField::Compilation) {
        out = track.compilation != 0;
        return true;
    }
    return false;
}

// Date fields, always compared as the raw epoch seconds stored on the track.
bool date_field(const Track& track, SmartPlaylistField field, std::uint64_t& out) noexcept {
    switch (field) {
    case SmartPlaylistField::DateModified:
        out = static_cast<std::uint64_t>(track.time_modified);
        return true;
    case SmartPlaylistField::DateAdded:
        out = static_cast<std::uint64_t>(track.time_added);
        return true;
    case SmartPlaylistField::LastPlayed:
        out = static_cast<std::uint64_t>(track.time_played);
        return true;
    case SmartPlaylistField::LastSkipped:
        out = static_cast<std::uint64_t>(track.last_skipped);
        return true;
    default:
        return false;
    }
}

// The reference code compares a gint64 accumulator against guint64 rule values,
// so the accumulator is promoted to unsigned. Keep that promotion explicit.
std::uint64_t as_unsigned(std::int64_t value) noexcept {
    return static_cast<std::uint64_t>(value);
}

// Inclusive between test that accepts the bounds in either order, matching the
// reference "in the range" expression for both int and date rules.
bool in_range(std::uint64_t value, std::uint64_t from, std::uint64_t to) noexcept {
    return (value <= from && value >= to) || (value >= from && value <= to);
}

// Exclusive "outside the range" test, matching the reference expression.
bool not_in_range(std::uint64_t value, std::uint64_t from, std::uint64_t to) noexcept {
    return (value < from && value < to) || (value > from && value > to);
}

bool matches_string(std::string_view value, std::string_view needle,
                    SmartPlaylistAction action) noexcept {
    switch (action) {
    case SmartPlaylistAction::IsString:
        return value == needle;
    case SmartPlaylistAction::IsNot:
        return value != needle;
    case SmartPlaylistAction::Contains:
        return value.find(needle) != std::string_view::npos;
    case SmartPlaylistAction::DoesNotContain:
        return value.find(needle) == std::string_view::npos;
    case SmartPlaylistAction::StartsWith:
        return value.starts_with(needle);
    case SmartPlaylistAction::DoesNotStartWith:
        return !value.starts_with(needle);
    case SmartPlaylistAction::EndsWith:
        return value.ends_with(needle);
    case SmartPlaylistAction::DoesNotEndWith:
        return !value.ends_with(needle);
    default:
        return false;
    }
}

std::uint32_t default_random_index(std::uint32_t bound) {
    // Not a thread_local engine: its 5,000 bytes would be allocated for every thread in the process.
    std::random_device generator;
    std::uniform_int_distribution<std::uint32_t> distribution(0, bound - 1);
    return distribution(generator);
}

// Moves a random element among the first nr entries to the end of them, which
// is what the reference randomize_glist() does with its GList.
void randomize(std::vector<Track*>& list, const RandomIndexFn& random_index) {
    std::size_t nr = list.size();
    while (nr > 1) {
        const auto bound = static_cast<std::uint32_t>(nr);
        std::uint32_t pick = random_index ? random_index(bound) : default_random_index(bound);
        if (pick >= bound) {
            pick = bound - 1;
        }
        const auto first = list.begin() + static_cast<std::ptrdiff_t>(pick);
        std::rotate(first, first + 1, list.begin() + static_cast<std::ptrdiff_t>(nr));
        --nr;
    }
}

// Applies the limit sort. An unknown sort leaves the selection order alone, as
// the reference implementation only warns.
void sort_selection(std::vector<Track*>& list, SmartPlaylistLimitSort sort,
                    const RandomIndexFn& random_index) {
    const auto less_by = [&list](auto compare) {
        std::stable_sort(list.begin(), list.end(),
                         [&compare](const Track* lhs, const Track* rhs) {
                             return compare(*lhs, *rhs);
                         });
    };
    switch (sort) {
    case SmartPlaylistLimitSort::Random:
        randomize(list, random_index);
        break;
    case SmartPlaylistLimitSort::SongName:
        less_by([](const Track& a, const Track& b) { return a.title < b.title; });
        break;
    case SmartPlaylistLimitSort::Album:
        less_by([](const Track& a, const Track& b) { return a.album < b.album; });
        break;
    case SmartPlaylistLimitSort::Artist:
        less_by([](const Track& a, const Track& b) { return a.artist < b.artist; });
        break;
    case SmartPlaylistLimitSort::Genre:
        less_by([](const Track& a, const Track& b) { return a.genre < b.genre; });
        break;
    case SmartPlaylistLimitSort::MostRecentlyAdded:
        less_by([](const Track& a, const Track& b) { return a.time_added > b.time_added; });
        break;
    case SmartPlaylistLimitSort::LeastRecentlyAdded:
        less_by([](const Track& a, const Track& b) { return a.time_added < b.time_added; });
        break;
    case SmartPlaylistLimitSort::MostOftenPlayed:
        less_by([](const Track& a, const Track& b) { return a.playcount > b.playcount; });
        break;
    case SmartPlaylistLimitSort::LeastOftenPlayed:
        less_by([](const Track& a, const Track& b) { return a.playcount < b.playcount; });
        break;
    case SmartPlaylistLimitSort::MostRecentlyPlayed:
        less_by([](const Track& a, const Track& b) { return a.time_played > b.time_played; });
        break;
    case SmartPlaylistLimitSort::LeastRecentlyPlayed:
        less_by([](const Track& a, const Track& b) { return a.time_played < b.time_played; });
        break;
    case SmartPlaylistLimitSort::HighestRating:
        less_by([](const Track& a, const Track& b) { return a.rating > b.rating; });
        break;
    case SmartPlaylistLimitSort::LowestRating:
        less_by([](const Track& a, const Track& b) { return a.rating < b.rating; });
        break;
    default:
        break;
    }
}

// Value a track contributes to a limit expressed in the given unit. An unknown
// limit type contributes nothing, which the reference implementation also does
// while only emitting a warning.
double limit_value(const Track& track, SmartPlaylistLimitType type) noexcept {
    switch (type) {
    case SmartPlaylistLimitType::Minutes:
        return static_cast<double>(track.tracklen) / (60.0 * 1000.0);
    case SmartPlaylistLimitType::Hours:
        return static_cast<double>(track.tracklen) / (60.0 * 60.0 * 1000.0);
    case SmartPlaylistLimitType::Megabytes:
        return static_cast<double>(track.size) / (1024.0 * 1024.0);
    case SmartPlaylistLimitType::Gigabytes:
        return static_cast<double>(track.size) / (1024.0 * 1024.0 * 1024.0);
    case SmartPlaylistLimitType::Songs:
        return 1.0;
    default:
        return 0.0;
    }
}

}  // namespace

namespace detail {

std::string casefold(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        std::uint32_t cp = lead;
        std::size_t length = 1;
        if (lead >= 0xC2 && lead <= 0xDF && i + 1 < text.size()) {
            length = 2;
            cp = static_cast<std::uint32_t>(lead & 0x1F) << 6;
            cp |= static_cast<unsigned char>(text[i + 1]) & 0x3F;
        } else if (lead >= 0xE0 && lead <= 0xEF && i + 2 < text.size()) {
            length = 3;
            cp = static_cast<std::uint32_t>(lead & 0x0F) << 12;
            cp |= static_cast<std::uint32_t>(static_cast<unsigned char>(text[i + 1]) & 0x3F) << 6;
            cp |= static_cast<unsigned char>(text[i + 2]) & 0x3F;
        } else if (lead >= 0xF0 && lead <= 0xF4 && i + 3 < text.size()) {
            length = 4;
            cp = static_cast<std::uint32_t>(lead & 0x07) << 18;
            cp |= static_cast<std::uint32_t>(static_cast<unsigned char>(text[i + 1]) & 0x3F) << 12;
            cp |= static_cast<std::uint32_t>(static_cast<unsigned char>(text[i + 2]) & 0x3F) << 6;
            cp |= static_cast<unsigned char>(text[i + 3]) & 0x3F;
        }
        const bool valid_length =
            !(length == 1 && lead >= 0x80) && !(length > 1 && i + length > text.size());
        if (!valid_length) {
            out.push_back(text[i]);
            ++i;
            continue;
        }
        append_utf8(out, fold_code_point(cp));
        i += length;
    }
    return out;
}

}  // namespace detail

std::unique_ptr<Playlist> make_playlist(std::string name, bool smart) {
    auto playlist = std::make_unique<Playlist>();
    playlist->name = std::move(name);
    playlist->type = PlaylistType::Normal;
    playlist->sort_order = PlaylistSortOrder::Manual;
    playlist->timestamp = std::time(nullptr);
    playlist->is_smart = smart;
    if (smart) {
        playlist->spl_prefs.live_update = 1;
        playlist->spl_prefs.check_rules = 1;
        playlist->spl_prefs.check_limits = 0;
        playlist->spl_prefs.limit_type =
            static_cast<std::uint32_t>(SmartPlaylistLimitType::Hours);
        playlist->spl_prefs.limit_sort =
            static_cast<std::uint32_t>(SmartPlaylistLimitSort::Random);
        playlist->spl_prefs.limit_value = 2;
        playlist->spl_prefs.match_checked_only = 0;
        playlist->spl_rules.match_operator = SmartPlaylistMatch::And;
        playlist->spl_rules.rules.push_back(make_rule());
    }
    return playlist;
}

SmartPlaylistFieldType field_type(const SmartPlaylistRule& rule) noexcept {
    switch (rule.field) {
    case SmartPlaylistField::SongName:
    case SmartPlaylistField::Album:
    case SmartPlaylistField::AlbumArtist:
    case SmartPlaylistField::Artist:
    case SmartPlaylistField::Genre:
    case SmartPlaylistField::Kind:
    case SmartPlaylistField::Comment:
    case SmartPlaylistField::Composer:
    case SmartPlaylistField::Grouping:
    case SmartPlaylistField::TvShow:
    case SmartPlaylistField::Category:
    case SmartPlaylistField::Description:
    case SmartPlaylistField::SortSongName:
    case SmartPlaylistField::SortAlbum:
    case SmartPlaylistField::SortAlbumArtist:
    case SmartPlaylistField::SortArtist:
    case SmartPlaylistField::SortComposer:
    case SmartPlaylistField::SortTvShow:
        return SmartPlaylistFieldType::String;
    case SmartPlaylistField::Bitrate:
    case SmartPlaylistField::SampleRate:
    case SmartPlaylistField::Year:
    case SmartPlaylistField::TrackNumber:
    case SmartPlaylistField::Size:
    case SmartPlaylistField::PlayCount:
    case SmartPlaylistField::DiscNumber:
    case SmartPlaylistField::Bpm:
    case SmartPlaylistField::Rating:
    case SmartPlaylistField::AlbumRating:
    case SmartPlaylistField::Time:  // track length in milliseconds
    case SmartPlaylistField::SeasonNr:
    case SmartPlaylistField::SkipCount:
    case SmartPlaylistField::Podcast:
        return SmartPlaylistFieldType::Int;
    case SmartPlaylistField::Compilation:
    case SmartPlaylistField::Purchase:
        return SmartPlaylistFieldType::Boolean;
    case SmartPlaylistField::DateModified:
    case SmartPlaylistField::DateAdded:
    case SmartPlaylistField::LastPlayed:
    case SmartPlaylistField::LastSkipped:
        return SmartPlaylistFieldType::Date;
    case SmartPlaylistField::Playlist:
        return SmartPlaylistFieldType::Playlist;
    case SmartPlaylistField::VideoKind:
        return SmartPlaylistFieldType::BinaryAnd;
    default:
        return SmartPlaylistFieldType::Unknown;
    }
}

SmartPlaylistActionType action_type(const SmartPlaylistRule& rule) noexcept {
    const SmartPlaylistFieldType type = field_type(rule);
    switch (type) {
    case SmartPlaylistFieldType::String:
        switch (rule.action) {
        case SmartPlaylistAction::IsString:
        case SmartPlaylistAction::IsNot:
        case SmartPlaylistAction::Contains:
        case SmartPlaylistAction::DoesNotContain:
        case SmartPlaylistAction::StartsWith:
        case SmartPlaylistAction::DoesNotStartWith:
        case SmartPlaylistAction::EndsWith:
        case SmartPlaylistAction::DoesNotEndWith:
            return SmartPlaylistActionType::String;
        case SmartPlaylistAction::IsInt:
        case SmartPlaylistAction::IsNotInt:
        case SmartPlaylistAction::IsGreaterThan:
        case SmartPlaylistAction::IsNotGreaterThan:
        case SmartPlaylistAction::IsLessThan:
        case SmartPlaylistAction::IsNotLessThan:
        case SmartPlaylistAction::IsInTheRange:
        case SmartPlaylistAction::IsNotInTheRange:
        case SmartPlaylistAction::IsInTheLast:
        case SmartPlaylistAction::IsNotInTheLast:
        case SmartPlaylistAction::BinaryAnd:
        case SmartPlaylistAction::NotBinaryAnd:
        case SmartPlaylistAction::BinaryUnknown1:
        case SmartPlaylistAction::BinaryUnknown2:
            return SmartPlaylistActionType::Invalid;
        default:
            return SmartPlaylistActionType::Unknown;
        }

    case SmartPlaylistFieldType::Int:
        switch (rule.action) {
        case SmartPlaylistAction::IsInt:
        case SmartPlaylistAction::IsNotInt:
        case SmartPlaylistAction::IsGreaterThan:
        case SmartPlaylistAction::IsNotGreaterThan:
        case SmartPlaylistAction::IsLessThan:
        case SmartPlaylistAction::IsNotLessThan:
            return SmartPlaylistActionType::Int;
        case SmartPlaylistAction::IsInTheRange:
        case SmartPlaylistAction::IsNotInTheRange:
            return SmartPlaylistActionType::RangeInt;
        case SmartPlaylistAction::BinaryAnd:
        case SmartPlaylistAction::NotBinaryAnd:
        case SmartPlaylistAction::BinaryUnknown1:
        case SmartPlaylistAction::BinaryUnknown2:
        case SmartPlaylistAction::IsString:
        case SmartPlaylistAction::Contains:
        case SmartPlaylistAction::StartsWith:
        case SmartPlaylistAction::DoesNotStartWith:
        case SmartPlaylistAction::EndsWith:
        case SmartPlaylistAction::DoesNotEndWith:
        case SmartPlaylistAction::IsInTheLast:
        case SmartPlaylistAction::IsNotInTheLast:
        case SmartPlaylistAction::IsNot:
        case SmartPlaylistAction::DoesNotContain:
            return SmartPlaylistActionType::Invalid;
        default:
            return SmartPlaylistActionType::Unknown;
        }

    case SmartPlaylistFieldType::Boolean:
        return SmartPlaylistActionType::None;

    case SmartPlaylistFieldType::Date:
        switch (rule.action) {
        case SmartPlaylistAction::IsInt:
        case SmartPlaylistAction::IsNotInt:
        case SmartPlaylistAction::IsGreaterThan:
        case SmartPlaylistAction::IsNotGreaterThan:
        case SmartPlaylistAction::IsLessThan:
        case SmartPlaylistAction::IsNotLessThan:
            return SmartPlaylistActionType::Date;
        case SmartPlaylistAction::IsInTheLast:
        case SmartPlaylistAction::IsNotInTheLast:
            return SmartPlaylistActionType::InTheLast;
        case SmartPlaylistAction::IsInTheRange:
        case SmartPlaylistAction::IsNotInTheRange:
            return SmartPlaylistActionType::RangeDate;
        case SmartPlaylistAction::IsString:
        case SmartPlaylistAction::Contains:
        case SmartPlaylistAction::StartsWith:
        case SmartPlaylistAction::DoesNotStartWith:
        case SmartPlaylistAction::EndsWith:
        case SmartPlaylistAction::DoesNotEndWith:
        case SmartPlaylistAction::IsNot:
        case SmartPlaylistAction::DoesNotContain:
        case SmartPlaylistAction::BinaryAnd:
        case SmartPlaylistAction::NotBinaryAnd:
        case SmartPlaylistAction::BinaryUnknown1:
        case SmartPlaylistAction::BinaryUnknown2:
            return SmartPlaylistActionType::Invalid;
        default:
            return SmartPlaylistActionType::Unknown;
        }

    case SmartPlaylistFieldType::BinaryAnd:
        switch (rule.action) {
        case SmartPlaylistAction::BinaryUnknown1:
        case SmartPlaylistAction::BinaryUnknown2:
            return SmartPlaylistActionType::Unknown;
        case SmartPlaylistAction::BinaryAnd:
        case SmartPlaylistAction::NotBinaryAnd:
            return SmartPlaylistActionType::BinaryAnd;
        case SmartPlaylistAction::IsInt:
        case SmartPlaylistAction::IsNotInt:
        case SmartPlaylistAction::IsGreaterThan:
        case SmartPlaylistAction::IsNotGreaterThan:
        case SmartPlaylistAction::IsLessThan:
        case SmartPlaylistAction::IsNotLessThan:
        case SmartPlaylistAction::IsInTheLast:
        case SmartPlaylistAction::IsNotInTheLast:
        case SmartPlaylistAction::IsInTheRange:
        case SmartPlaylistAction::IsNotInTheRange:
        case SmartPlaylistAction::IsString:
        case SmartPlaylistAction::Contains:
        case SmartPlaylistAction::StartsWith:
        case SmartPlaylistAction::DoesNotStartWith:
        case SmartPlaylistAction::EndsWith:
        case SmartPlaylistAction::DoesNotEndWith:
        case SmartPlaylistAction::IsNot:
        case SmartPlaylistAction::DoesNotContain:
            return SmartPlaylistActionType::Invalid;
        default:
            return SmartPlaylistActionType::Unknown;
        }

    case SmartPlaylistFieldType::Playlist:
        switch (rule.action) {
        case SmartPlaylistAction::IsInt:
        case SmartPlaylistAction::IsNotInt:
            return SmartPlaylistActionType::Playlist;
        case SmartPlaylistAction::IsGreaterThan:
        case SmartPlaylistAction::IsNotGreaterThan:
        case SmartPlaylistAction::IsLessThan:
        case SmartPlaylistAction::IsNotLessThan:
        case SmartPlaylistAction::IsInTheLast:
        case SmartPlaylistAction::IsNotInTheLast:
        case SmartPlaylistAction::IsInTheRange:
        case SmartPlaylistAction::IsNotInTheRange:
        case SmartPlaylistAction::IsString:
        case SmartPlaylistAction::Contains:
        case SmartPlaylistAction::StartsWith:
        case SmartPlaylistAction::DoesNotStartWith:
        case SmartPlaylistAction::EndsWith:
        case SmartPlaylistAction::DoesNotEndWith:
        case SmartPlaylistAction::IsNot:
        case SmartPlaylistAction::DoesNotContain:
        case SmartPlaylistAction::BinaryAnd:
        case SmartPlaylistAction::NotBinaryAnd:
        case SmartPlaylistAction::BinaryUnknown1:
        case SmartPlaylistAction::BinaryUnknown2:
            return SmartPlaylistActionType::Invalid;
        default:
            return SmartPlaylistActionType::Unknown;
        }

    case SmartPlaylistFieldType::Unknown:
    default:
        return SmartPlaylistActionType::Unknown;
    }
}

bool is_action_known(SmartPlaylistAction action) noexcept {
    switch (action) {
    case SmartPlaylistAction::IsInt:
    case SmartPlaylistAction::IsGreaterThan:
    case SmartPlaylistAction::IsNotGreaterThan:
    case SmartPlaylistAction::IsLessThan:
    case SmartPlaylistAction::IsNotLessThan:
    case SmartPlaylistAction::IsInTheRange:
    case SmartPlaylistAction::IsNotInTheRange:
    case SmartPlaylistAction::IsInTheLast:
    case SmartPlaylistAction::IsString:
    case SmartPlaylistAction::Contains:
    case SmartPlaylistAction::StartsWith:
    case SmartPlaylistAction::DoesNotStartWith:
    case SmartPlaylistAction::EndsWith:
    case SmartPlaylistAction::DoesNotEndWith:
    case SmartPlaylistAction::IsNotInt:
    case SmartPlaylistAction::IsNotInTheLast:
    case SmartPlaylistAction::IsNot:
    case SmartPlaylistAction::DoesNotContain:
    case SmartPlaylistAction::BinaryAnd:
    case SmartPlaylistAction::NotBinaryAnd:
    case SmartPlaylistAction::BinaryUnknown1:
    case SmartPlaylistAction::BinaryUnknown2:
        return true;
    default:
        return false;
    }
}

bool validate(SmartPlaylistRule& rule) {
    switch (action_type(rule)) {
    case SmartPlaylistActionType::Int:
    case SmartPlaylistActionType::Playlist:
    case SmartPlaylistActionType::Date:
    case SmartPlaylistActionType::BinaryAnd:
        rule.from_date = 0;
        rule.from_units = 1;
        rule.to_value = rule.from_value;
        rule.to_date = 0;
        rule.to_units = 1;
        return true;
    case SmartPlaylistActionType::RangeInt:
    case SmartPlaylistActionType::RangeDate:
        rule.from_date = 0;
        rule.from_units = 1;
        rule.to_date = 0;
        rule.to_units = 1;
        return true;
    case SmartPlaylistActionType::InTheLast:
        rule.from_value = kSplDateIdentifier;
        rule.to_value = kSplDateIdentifier;
        rule.to_units = 1;
        return true;
    case SmartPlaylistActionType::None:
    case SmartPlaylistActionType::String:
        rule.from_value = 0;
        rule.from_date = 0;
        rule.from_units = 0;
        rule.to_value = 0;
        rule.to_date = 0;
        rule.to_units = 0;
        return true;
    case SmartPlaylistActionType::Invalid:
    case SmartPlaylistActionType::Unknown:
    default:
        return false;
    }
}

std::time_t in_the_last_threshold(const SmartPlaylistRule& rule, std::time_t now) noexcept {
    // The reference code adds the unsigned product (fromdate * fromunits) to a
    // time_t; both operands are signed here so that a negative count of units
    // simply moves the threshold backwards.
    const auto offset = static_cast<std::int64_t>(rule.from_date) *
                        static_cast<std::int64_t>(rule.from_units);
    return static_cast<std::time_t>(static_cast<std::int64_t>(now) + offset);
}

bool evaluate(const SmartPlaylistRule& rule, const Track& track, std::time_t now) {
    const SmartPlaylistFieldType type = field_type(rule);
    const SmartPlaylistActionType action = action_type(rule);
    if (action == SmartPlaylistActionType::Invalid) {
        return false;
    }

    switch (type) {
    case SmartPlaylistFieldType::String: {
        const std::string* value = string_field(track, rule.field);
        if (value == nullptr) {
            return false;
        }
        return matches_string(detail::casefold(*value), detail::casefold(rule.string),
                              rule.action);
    }

    case SmartPlaylistFieldType::Int: {
        std::int64_t value = 0;
        if (!int_field(track, rule.field, value)) {
            return false;
        }
        const std::uint64_t lhs = as_unsigned(value);
        switch (rule.action) {
        case SmartPlaylistAction::IsInt:
            return lhs == rule.from_value;
        case SmartPlaylistAction::IsNotInt:
            return lhs != rule.from_value;
        case SmartPlaylistAction::IsGreaterThan:
            return lhs > rule.from_value;
        case SmartPlaylistAction::IsNotGreaterThan:
            return lhs <= rule.from_value;
        case SmartPlaylistAction::IsLessThan:
            return lhs < rule.from_value;
        case SmartPlaylistAction::IsNotLessThan:
            return lhs >= rule.from_value;
        case SmartPlaylistAction::IsInTheRange:
            return in_range(lhs, rule.from_value, rule.to_value);
        case SmartPlaylistAction::IsNotInTheRange:
            return not_in_range(lhs, rule.from_value, rule.to_value);
        default:
            return false;
        }
    }

    case SmartPlaylistFieldType::BinaryAnd: {
        std::int64_t value = 0;
        if (!int_field(track, rule.field, value)) {
            return false;
        }
        const std::uint64_t lhs = as_unsigned(value);
        switch (rule.action) {
        case SmartPlaylistAction::BinaryAnd:
            return (lhs & rule.from_value) != 0;
        case SmartPlaylistAction::NotBinaryAnd:
            return (lhs & rule.from_value) == 0;
        default:
            return false;
        }
    }

    case SmartPlaylistFieldType::Boolean: {
        bool value = false;
        if (!bool_field(track, rule.field, value)) {
            return false;
        }
        switch (rule.action) {
        case SmartPlaylistAction::IsInt:  // "is set"
            return value;
        case SmartPlaylistAction::IsNotInt:  // "is not set"
            return !value;
        default:
            return false;
        }
    }

    case SmartPlaylistFieldType::Date: {
        std::uint64_t value = 0;
        if (!date_field(track, rule.field, value)) {
            return false;
        }
        switch (rule.action) {
        case SmartPlaylistAction::IsInt:
            return value == rule.from_value;
        case SmartPlaylistAction::IsNotInt:
            return value != rule.from_value;
        case SmartPlaylistAction::IsGreaterThan:
            return value > rule.from_value;
        case SmartPlaylistAction::IsNotGreaterThan:
            return value <= rule.from_value;
        case SmartPlaylistAction::IsLessThan:
            return value < rule.from_value;
        case SmartPlaylistAction::IsNotLessThan:
            return value >= rule.from_value;
        case SmartPlaylistAction::IsInTheLast:
            return value > static_cast<std::uint64_t>(in_the_last_threshold(rule, now));
        case SmartPlaylistAction::IsNotInTheLast:
            return value <= static_cast<std::uint64_t>(in_the_last_threshold(rule, now));
        case SmartPlaylistAction::IsInTheRange:
            return in_range(value, rule.from_value, rule.to_value);
        case SmartPlaylistAction::IsNotInTheRange:
            return not_in_range(value, rule.from_value, rule.to_value);
        default:
            return false;
        }
    }

    case SmartPlaylistFieldType::Playlist: {
        if (track.itdb == nullptr) {
            return false;
        }
        const Playlist* member_of = track.itdb->playlist_by_id(rule.from_value);
        if (member_of == nullptr) {
            return false;
        }
        switch (rule.action) {
        case SmartPlaylistAction::IsInt:
            return member_of->contains(&track);
        case SmartPlaylistAction::IsNotInt:
            return !member_of->contains(&track);
        default:
            return false;
        }
    }

    case SmartPlaylistFieldType::Unknown:
    default:
        return false;
    }
}

bool evaluate(const SmartPlaylistRule& rule, const Track& track) {
    return evaluate(rule, track, std::time(nullptr));
}

bool evaluate(const SmartPlaylistRules& rules, const Track& track, std::time_t now) {
    // "Match all" starts true, "match any" starts false, and an empty rule set
    // matches everything.
    bool matched = rules.match_operator == SmartPlaylistMatch::And;
    if (rules.rules.empty()) {
        return true;
    }
    for (const SmartPlaylistRule& rule : rules.rules) {
        const bool truth = evaluate(rule, track, now);
        if (rules.match_operator == SmartPlaylistMatch::And) {
            if (!truth) {
                return false;
            }
        } else if (rules.match_operator == SmartPlaylistMatch::Or) {
            if (truth) {
                return true;
            }
        }
    }
    return matched;
}

bool evaluate(const SmartPlaylistRules& rules, const Track& track) {
    return evaluate(rules, track, std::time(nullptr));
}

SmartPlaylistRule make_rule() noexcept {
    SmartPlaylistRule rule;
    rule.field = SmartPlaylistField::Artist;
    rule.action = SmartPlaylistAction::Contains;
    rule.from_value = 0;
    rule.from_date = 0;
    rule.from_units = 0;
    rule.to_value = 0;
    rule.to_date = 0;
    rule.to_units = 0;
    return rule;
}

SmartPlaylistRule& add_rule(Playlist& playlist, const SmartPlaylistRule& rule,
                            std::optional<std::size_t> position) {
    auto& rules = playlist.spl_rules.rules;
    if (position && *position <= rules.size()) {
        return *rules.insert(rules.begin() + static_cast<std::ptrdiff_t>(*position), rule);
    }
    rules.push_back(rule);
    return rules.back();
}

SmartPlaylistRule& add_new_rule(Playlist& playlist, std::optional<std::size_t> position) {
    return add_rule(playlist, make_rule(), position);
}

bool remove_rule(Playlist& playlist, std::size_t index) {
    auto& rules = playlist.spl_rules.rules;
    if (index >= rules.size()) {
        return false;
    }
    rules.erase(rules.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

bool copy_rules(Playlist& dest, const Playlist& src) {
    if (!dest.is_smart || !src.is_smart) {
        return false;
    }
    dest.spl_prefs = src.spl_prefs;
    dest.spl_rules = src.spl_rules;  // value copies the rule list as well
    return true;
}

bool update(Playlist& playlist, std::time_t now, const RandomIndexFn& random_index) {
    if (!playlist.is_smart) {
        return false;
    }
    Itdb* itdb = playlist.itdb;
    if (itdb == nullptr) {
        return false;
    }

    const SmartPlaylistPrefs& prefs = playlist.spl_prefs;

    playlist.members.clear();

    std::vector<Track*> selected;
    for (const auto& owned : itdb->tracks) {
        Track* track = owned.get();
        if (track == nullptr) {
            continue;
        }
        // A checked byte of 0 means "checked"; only checked songs are kept when
        // the playlist asks for it.
        if (prefs.match_checked_only != 0 && track->checked != 0) {
            continue;
        }
        if (prefs.check_rules != 0 && !evaluate(playlist.spl_rules, *track, now)) {
            continue;
        }
        selected.push_back(track);
    }

    if (selected.empty()) {
        return true;
    }

    if (prefs.check_limits == 0) {
        playlist.members = std::move(selected);
        return true;
    }

    sort_selection(selected, static_cast<SmartPlaylistLimitSort>(prefs.limit_sort),
                   random_index);

    const auto limit = static_cast<double>(prefs.limit_value);
    const auto type = static_cast<SmartPlaylistLimitType>(prefs.limit_type);
    double running_total = 0.0;
    std::size_t counter = 0;
    while (running_total < limit && counter < selected.size()) {
        Track* track = selected[counter];
        const double current = limit_value(*track, type);
        if (running_total + current <= limit) {
            running_total += current;
            playlist.members.push_back(track);
        }
        ++counter;
    }
    return true;
}

bool update(Playlist& playlist, const RandomIndexFn& random_index) {
    return update(playlist, std::time(nullptr), random_index);
}

std::size_t update_all(Itdb& itdb, std::time_t now) {
    std::size_t updated = 0;
    for (const auto& playlist : itdb.playlists) {
        if (playlist && update(*playlist, now)) {
            ++updated;
        }
    }
    return updated;
}

std::size_t update_all(Itdb& itdb) {
    return update_all(itdb, std::time(nullptr));
}

std::size_t update_live(Itdb& itdb, std::time_t now) {
    std::size_t updated = 0;
    for (const auto& playlist : itdb.playlists) {
        if (playlist && playlist->is_smart && playlist->spl_prefs.live_update != 0 &&
            update(*playlist, now)) {
            ++updated;
        }
    }
    return updated;
}

std::size_t update_live(Itdb& itdb) {
    return update_live(itdb, std::time(nullptr));
}

}  // namespace gpod
