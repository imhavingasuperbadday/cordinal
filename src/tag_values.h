// Smart multi-value tag splitting with an exception list for band names containing slashes,
// ampersands, or commas (AC/DC, Earth, Wind & Fire, Simon & Garfunkel, ...). Portable: no Win32.
#pragma once
#include <string>
#include <string_view>
#include <vector>

enum class TagFieldKind {
    Artist,
    Genre,
    Composer,
    Generic,
};

// True when `text` (after trimming) is a known single artist/genre name that contains a
// delimiter such as '/', '&', or ',' and must not be split.
bool IsMultiValueException(std::string_view text);

// Splits a tag string into distinct, trimmed, non-empty values. Always splits on ';', splits
// smartly on '/' (respecting exceptions and numeric/initialism tokens), splits 'feat.' / 'ft.' /
// 'featuring' clauses for Artist and Composer, and splits ',' for Genre.
std::vector<std::string> SplitTagValues(std::string_view text, TagFieldKind kind = TagFieldKind::Generic);
