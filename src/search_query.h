// Inline search query helpers: autocomplete suggestions and one-click conversion to an
// auto-playlist. Portable across Windows and Linux.
#pragma once
#include "autoplaylist.h"
#include "library.h"

#include <string>
#include <string_view>
#include <vector>

struct SearchSuggestion {
    std::wstring insert;  // token text to insert (e.g. L"year:" or L"format:flac")
    std::wstring label;   // primary display text
    std::wstring hint;    // secondary example/count hint
};

// Suggests inline filter prefixes (when typing a keyword) or values (after "key:").
std::vector<SearchSuggestion> SearchSuggestions(const Library& library, std::wstring_view query, size_t maxItems = 8);

// Replaces the trailing token in `query` with `insert` (appending a space when `insert` is a
// complete filter value rather than a "key:" prefix).
std::wstring ApplySearchSuggestion(std::wstring_view query, std::wstring_view insert);

// Converts an inline search query into an AutoPlaylist. Returns false if the query has no terms.
bool SearchToAutoPlaylist(std::wstring_view query, AutoPlaylist& out);
