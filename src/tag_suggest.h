// Values already in the library, offered as completions in the tag editor's text fields.
//
// Only the fields that name a group (artist, album artist, album, genre, composer) are collected.
// Values are trimmed, deduplicated without regard to case and sorted by their folded form, so a
// prefix lookup can use a binary search. The text as it first appears in the library is kept for
// display; nothing is written until the editor saves.
#pragma once
#include "library.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

enum class TagSuggestField : uint8_t { Artist, AlbumArtist, Album, Genre, Composer, Count };

struct TagSuggestion {
    std::wstring key;   // trimmed and lowercased, used only for matching
    std::wstring text;  // the value as it appears in the library
};

struct TagSuggestionSet {
    std::array<std::vector<TagSuggestion>, static_cast<size_t>(TagSuggestField::Count)> fields;

    const std::vector<TagSuggestion>& operator[](TagSuggestField field) const {
        return fields[static_cast<size_t>(field)];
    }
};

// One pass over the library, including the Inbox but not missing or excluded tracks. Empty values
// and values that differ only in case contribute one suggestion.
TagSuggestionSet BuildTagSuggestions(const Library& library);

// Up to `limit` values that begin with `prefix`, ignoring case. A value equal to the prefix is not
// offered again; typing the whole name should close the list rather than suggest itself.
std::vector<std::wstring> MatchTagSuggestions(const TagSuggestionSet& set, TagSuggestField field,
                                              std::wstring_view prefix, size_t limit = 8);
