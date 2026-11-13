// Collects the tag editor's completion values and answers prefix lookups. See tag_suggest.h.
#include "tag_suggest.h"
#include "textcase.h"

#include <algorithm>
#include <cwctype>
#include <map>
#include <unordered_set>

namespace {

constexpr size_t kFieldCount = static_cast<size_t>(TagSuggestField::Count);

std::string_view TrimAscii(std::string_view text) {
    size_t begin = 0, end = text.size();
    auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (begin < end && space(text[begin])) ++begin;
    while (end > begin && space(text[end - 1])) --end;
    return text.substr(begin, end - begin);
}

std::wstring TrimWide(std::wstring text) {
    size_t begin = 0, end = text.size();
    auto space = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; };
    while (begin < end && space(text[begin])) ++begin;
    while (end > begin && space(text[end - 1])) --end;
    text.erase(end);
    text.erase(0, begin);
    return text;
}

StringPool::Handle ValueOf(const Track& track, TagSuggestField field) {
    switch (field) {
        case TagSuggestField::Artist: return track.artist;
        case TagSuggestField::AlbumArtist: return track.albumArtist;
        case TagSuggestField::Album: return track.album;
        case TagSuggestField::Genre: return track.genre;
        case TagSuggestField::Composer: return track.composer;
        default: return 0;
    }
}

}  // namespace

TagSuggestionSet BuildTagSuggestions(const Library& library) {
    TagSuggestionSet set;
    // The first pass hashes the pool's existing bytes, so repeated artists and albums cost no
    // allocation until the much smaller set of distinct values is turned into wide text.
    std::array<std::unordered_set<std::string_view>, kFieldCount> seen;
    std::array<std::vector<std::string_view>, kFieldCount> firstSeen;
    for (TrackId id = 0; id < library.Count(); ++id) {
        const Track& track = library[id];
        if ((track.flags & Track::kMissing) || !(track.flags & (Track::kInLibrary | Track::kInbox))) continue;
        for (size_t i = 0; i < kFieldCount; ++i) {
            StringPool::Handle handle = ValueOf(track, static_cast<TagSuggestField>(i));
            if (!handle) continue;
            std::string_view value = TrimAscii(library.View(handle));
            if (value.empty()) continue;
            if (seen[i].insert(value).second) firstSeen[i].push_back(value);
        }
    }
    for (size_t i = 0; i < kFieldCount; ++i) {
        std::map<std::wstring, std::wstring> byFold;
        for (std::string_view value : firstSeen[i]) {
            std::wstring text = TrimWide(FromUtf8(value));
            if (text.empty()) continue;
            std::wstring key = textcase::Fold(text);
            byFold.try_emplace(std::move(key), std::move(text));
        }
        set.fields[i].reserve(byFold.size());
        for (auto& [key, text] : byFold) set.fields[i].push_back({std::move(key), std::move(text)});
    }
    return set;
}

std::vector<std::wstring> MatchTagSuggestions(const TagSuggestionSet& set, TagSuggestField field,
                                              std::wstring_view prefix, size_t limit) {
    std::vector<std::wstring> matches;
    if (prefix.empty() || limit == 0) return matches;
    const std::wstring key = textcase::Fold(std::wstring(prefix));
    const auto& values = set[field];
    auto it = std::lower_bound(
        values.begin(), values.end(), key,
        [](const TagSuggestion& value, const std::wstring& wanted) { return value.key < wanted; });
    for (; it != values.end() && matches.size() < limit; ++it) {
        if (it->key.size() < key.size() || it->key.compare(0, key.size(), key) != 0) break;
        // The field already says exactly this value: offer it only when the spelling differs, so a
        // typed lowercase variant can still be corrected to the library's capitalisation.
        if (it->text.size() == prefix.size() &&
            std::equal(it->text.begin(), it->text.end(), prefix.begin())) continue;
        matches.push_back(it->text);
    }
    return matches;
}

FileNameGuess GuessTagsFromFileName(std::wstring_view stem) {
    std::wstring name(stem);
    if (name.find(L' ') == std::wstring::npos)
        for (wchar_t& c : name)
            if (c == L'_') c = L' ';
    // One space between words, none at the ends.
    std::wstring s;
    for (wchar_t c : name) {
        const bool space = std::iswspace(c) != 0;
        if (space && (s.empty() || s.back() == L' ')) continue;
        s += space ? L' ' : c;
    }
    while (!s.empty() && s.back() == L' ') s.pop_back();
    FileNameGuess guess;
    size_t digits = 0;
    while (digits < s.size() && digits < 4 && std::iswdigit(s[digits])) ++digits;
    if (digits >= 1 && digits <= 3 && digits < s.size()) {
        size_t rest = digits;
        const wchar_t after = s[digits];
        if (after == L'.' || after == L'-' || after == L')' || after == L'_' || (after == L' ' && s[0] == L'0')) {
            while (rest < s.size() && (s[rest] == L' ' || s[rest] == L'.' || s[rest] == L'-' || s[rest] == L')' ||
                                       s[rest] == L'_'))
                ++rest;
            if (rest < s.size()) {
                guess.track = (uint32_t)std::stoul(s.substr(0, digits));
                s = s.substr(rest);
            }
        }
    }
    for (const wchar_t* dash : {L" - ", L" \x2013 ", L" \x2014 "}) {
        const size_t at = s.find(dash);
        if (at == std::wstring::npos || at == 0 || at + 3 >= s.size()) continue;
        guess.artist = s.substr(0, at);
        guess.title = s.substr(at + 3);
        return guess;
    }
    guess.title = s;
    return guess;
}
