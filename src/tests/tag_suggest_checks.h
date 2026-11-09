// Checks of the tag editor's completion values (tag_suggest.h) that need no window or file. The
// Windows self-test (selftest_tags.cpp, suite "tags") and the Linux core test (core_test.cpp,
// "tagsuggest") both run them, so the shared suggestion rule is held to one standard.
#pragma once
#include "library.h"
#include "tag_suggest.h"

#include <string>
#include <vector>

namespace tag_suggest_checks {

struct Group {
    std::string name;
    std::vector<std::string> failures;
    bool Passed() const { return failures.empty(); }
};

inline void Add(Group& group, bool ok, const char* what) {
    if (!ok) group.failures.push_back(what);
}

inline TrackId AddTrack(Library& library, const wchar_t* path, const char* artist, const char* albumArtist,
                        const char* album, const char* genre, const char* composer, uint16_t flags) {
    TrackId id = library.Add(path);
    TagData tags;
    tags.artist = artist;
    tags.albumArtist = albumArtist;
    tags.album = album;
    tags.genre = genre;
    tags.composer = composer;
    library.ApplyTags(id, tags);
    library[id].flags |= flags;
    return id;
}

inline const TagSuggestion* Find(const TagSuggestionSet& set, TagSuggestField field, const std::wstring& text) {
    for (const auto& value : set[field])
        if (value.text == text) return &value;
    return nullptr;
}

inline Group Collected() {
    Group group{"tag suggestions collect library values once, trimmed and without case variants"};
    Library library;
    AddTrack(library, L"C:\\suggest\\one.flac", "Bjork", "Bjork", "Debut", "Pop", "Composer A", Track::kInLibrary);
    AddTrack(library, L"C:\\suggest\\two.flac", "BJORK", "Various Artists", "Post", " Rock ", "Composer B",
             Track::kInLibrary);
    AddTrack(library, L"C:\\suggest\\three.flac", "The Beatles", "The Beatles", "Abbey Road", "Rock",
             "Lennon-McCartney", Track::kInLibrary);
    AddTrack(library, L"C:\\suggest\\inbox.flac", "Inbox Artist", "Inbox Artist", "Inbox Album", "", "",
             Track::kInLibrary | Track::kInbox);
    AddTrack(library, L"C:\\suggest\\excluded.flac", "Excluded Artist", "Excluded Artist", "Excluded Album", "",
             "", Track::kExcluded);
    AddTrack(library, L"C:\\suggest\\missing.flac", "Missing Artist", "Missing Artist", "Missing Album", "", "",
             Track::kInLibrary | Track::kMissing);
    AddTrack(library, L"C:\\suggest\\accent.flac", "Sigur R\xC3\xB3s", "Sigur R\xC3\xB3s",
             "\xC3\x81g\xC3\xA6tis byrjun", "Post-rock", "", Track::kInLibrary);
    const TagSuggestionSet set = BuildTagSuggestions(library);
    Add(group, set[TagSuggestField::Artist].size() == 4, "artist has four distinct names");
    Add(group, set[TagSuggestField::AlbumArtist].size() == 5, "album artist has five distinct names");
    Add(group, set[TagSuggestField::Album].size() == 5, "album has five distinct names");
    Add(group, set[TagSuggestField::Genre].size() == 3, "genre has three distinct values");
    Add(group, set[TagSuggestField::Composer].size() == 3, "composer has three distinct names");
    Add(group, Find(set, TagSuggestField::Artist, L"Bjork") != nullptr, "the first spelling of a name is kept");
    Add(group, Find(set, TagSuggestField::Artist, L"BJORK") == nullptr, "a case variant is not a second value");
    Add(group, Find(set, TagSuggestField::Genre, L"Rock") != nullptr, "surrounding spaces are trimmed");
    Add(group, Find(set, TagSuggestField::Artist, L"Inbox Artist") != nullptr, "an Inbox track contributes values");
    Add(group, Find(set, TagSuggestField::Artist, L"Excluded Artist") == nullptr,
        "an excluded track contributes nothing");
    Add(group, Find(set, TagSuggestField::Artist, L"Missing Artist") == nullptr,
        "a missing track contributes nothing");
    return group;
}

inline Group Matching() {
    Group group{"tag suggestions match a prefix without case and skip the value already typed"};
    Library library;
    AddTrack(library, L"C:\\suggest\\one.flac", "Bjork", "Bjork", "Debut", "Pop", "Composer A", Track::kInLibrary);
    AddTrack(library, L"C:\\suggest\\two.flac", "BJORK", "Various Artists", "Post", " Rock ", "Composer B",
             Track::kInLibrary);
    AddTrack(library, L"C:\\suggest\\three.flac", "The Beatles", "The Beatles", "Abbey Road", "Rock",
             "Lennon-McCartney", Track::kInLibrary);
    AddTrack(library, L"C:\\suggest\\accent.flac", "Sigur R\xC3\xB3s", "Sigur R\xC3\xB3s",
             "\xC3\x81g\xC3\xA6tis byrjun", "Post-rock", "", Track::kInLibrary);
    const TagSuggestionSet set = BuildTagSuggestions(library);
    auto one = [&](TagSuggestField field, const wchar_t* prefix) {
        auto matches = MatchTagSuggestions(set, field, prefix);
        return matches.size() == 1 ? matches[0] : std::wstring();
    };
    Add(group, one(TagSuggestField::Artist, L"bj") == L"Bjork", "an artist prefix ignores case");
    Add(group, one(TagSuggestField::Artist, L"THE") == L"The Beatles", "a later artist matches its prefix");
    Add(group, one(TagSuggestField::Artist, L"the beatles") == L"The Beatles",
        "an exact value in another case is offered");
    Add(group, MatchTagSuggestions(set, TagSuggestField::Artist, L"The Beatles").empty(),
        "an exact value in its own case is not repeated");
    Add(group, one(TagSuggestField::Genre, L"roc") == L"Rock", "a genre prefix finds the trimmed value");
    Add(group, one(TagSuggestField::Genre, L"ROCK") == L"Rock", "a genre in another case is offered");
    Add(group, MatchTagSuggestions(set, TagSuggestField::Genre, L"Rock").empty(),
        "a complete genre is not repeated");
    Add(group, one(TagSuggestField::Composer, L"lennon") == L"Lennon-McCartney", "a composer prefix matches");
    Add(group, one(TagSuggestField::Album, L"\u00C1g") == L"\u00C1g\u00E6tis byrjun", "a non-ASCII value is offered");
    Add(group, MatchTagSuggestions(set, TagSuggestField::Artist, L"").empty(), "an empty prefix offers nothing");
    Add(group, MatchTagSuggestions(set, TagSuggestField::Artist, L"   ").empty(), "a blank prefix offers nothing");
    const auto composers = MatchTagSuggestions(set, TagSuggestField::Composer, L"composer");
    Add(group, composers.size() == 2 && composers[0] == L"Composer A" && composers[1] == L"Composer B",
        "several values come back in folded order");
    return group;
}

inline Group Limit() {
    Group group{"tag suggestions stop at the requested number, in folded order"};
    Library library;
    for (int i = 0; i < 10; ++i) {
        const std::wstring path = L"C:\\suggest\\alpha" + std::to_wstring(i) + L".flac";
        const std::string name = "Alpha " + std::to_string(i);
        AddTrack(library, path.c_str(), name.c_str(), "", "", "", "", Track::kInLibrary);
    }
    const TagSuggestionSet set = BuildTagSuggestions(library);
    const auto eight = MatchTagSuggestions(set, TagSuggestField::Artist, L"alpha", 8);
    const auto three = MatchTagSuggestions(set, TagSuggestField::Artist, L"alpha", 3);
    Add(group, eight.size() == 8 && eight.front() == L"Alpha 0" && eight.back() == L"Alpha 7",
        "eight suggestions are the first eight in order");
    Add(group, three.size() == 3 && three.back() == L"Alpha 2", "the limit is honoured");
    Add(group, MatchTagSuggestions(set, TagSuggestField::Artist, L"alpha", 0).empty(),
        "a zero limit offers nothing");
    return group;
}

inline std::vector<Group> All() { return {Collected(), Matching(), Limit()}; }

}  // namespace tag_suggest_checks
