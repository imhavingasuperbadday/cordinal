// Multiple-edition albums: automatic and manual grouping, preferred editions, track
// correspondence, persistence, migration and search. The suite owns g_lib and resets the album
// database before and after itself so it can run alone or after another suite.
#include "album_editions.h"
#include "library_archive.h"
#include "musicbrainz.h"
#include "selftest_common.h"

namespace {

TrackId AddTrack(const std::wstring& path, const char* artist, const char* album, const char* title,
                 const char* date = "1999", uint32_t trackNo = 1, uint32_t durationMs = 200000,
                 const char* releaseGroup = "", const char* release = "") {
    TrackId id = g_lib.Add(path);
    TagData tags;
    tags.artist = artist;
    tags.albumArtist = artist;
    tags.album = album;
    tags.title = title;
    tags.date = date;
    tags.track = trackNo;
    tags.duration = durationMs / 1000.0;
    tags.codec = Codec::FLAC;
    tags.musicBrainzReleaseGroup = releaseGroup;
    tags.musicBrainzRelease = release;
    g_lib.ApplyTags(id, tags);
    g_lib[id].durationMs = durationMs;
    g_lib[id].flags |= Track::kInLibrary | Track::kLoaded;
    return id;
}

std::wstring TempFile(const std::wstring& scratch, const wchar_t* name) {
    return scratch + L"-" + name + L".cdb";
}

void DeleteDatabase(const std::wstring& file) {
    DeleteFileW(file.c_str());
    DeleteFileW((file + L".bak").c_str());
}

void ResetLibrary() {
    g_lib = Library();
    ResetAlbumGroups();
}

const AlbumGroup* GroupOf(const std::vector<TrackId>& tracks) {
    for (TrackId id : tracks) if (const AlbumGroup* group = AlbumGroupForTrack(id)) return group;
    return nullptr;
}

bool SamePaths(const std::vector<TrackId>& a, const std::vector<TrackId>& b) {
    if (a.size() != b.size()) return false;
    std::vector<std::wstring> x, y;
    for (TrackId id : a) if (g_lib.Contains(id)) x.push_back(g_lib.Path(id));
    for (TrackId id : b) if (g_lib.Contains(id)) y.push_back(g_lib.Path(id));
    std::sort(x.begin(), x.end());
    std::sort(y.begin(), y.end());
    return x == y;
}

AlbumSelection SelectionFor(const std::vector<TrackId>& tracks) {
    AlbumSelection selection;
    if (tracks.empty()) return selection;
    const Track& first = g_lib[tracks.front()];
    selection.artist = first.albumArtist ? g_lib.View(first.albumArtist) : g_lib.View(first.artist);
    selection.album = g_lib.View(first.album);
    selection.date = g_lib.View(first.date);
    selection.tracks = tracks;
    if (const AlbumGroup* group = AlbumGroupForTrack(tracks.front())) {
        selection.groupId = group->id;
        if (const AlbumEdition* edition = AlbumEditionForTrack(tracks.front())) selection.editionId = edition->id;
    }
    return selection;
}

void PutU(std::string& out, uint64_t value) {
    do {
        uint8_t b = (uint8_t)(value & 0x7F);
        value >>= 7;
        out += (char)(b | (value ? 0x80 : 0));
    } while (value);
}
void PutI(std::string& out, int64_t value) { PutU(out, ((uint64_t)value << 1) ^ (uint64_t)-(value < 0)); }
void PutBytes(std::string& out, std::string_view bytes) {
    PutU(out, bytes.size());
    out.append(bytes);
}

// A minimal version-1 library (before extension sections existed), so the migration path is tested
// against the real reader rather than a hand-made model.
std::string LegacyLibrary() {
    // String 0 is the implicit empty string; these are indices 1..9.
    const std::vector<std::string> strings = {
        "C:\\Legacy", "one.flac", "two.flac", "One", "Two", "Legacy Band", "Legacy Album",
        "Legacy Album (Remastered)", "1999",
    };
    std::string out = "CDLB";
    out.append("\x01\0\0\0", 4);
    PutU(out, strings.size());
    for (const auto& text : strings) PutBytes(out, text);
    PutU(out, 2);
    auto track = [&](uint32_t file, uint32_t title, uint32_t album, uint32_t trackNo) {
        for (uint32_t field : {1u, file, title, 6u, album, 6u, 0u, 9u, 0u, 0u, 0u, 0u}) PutU(out, field);
        for (uint64_t value : {uint64_t(1000), uint64_t(1), uint64_t(200000), uint64_t(44100), uint64_t(1),
                               uint64_t(0), uint64_t(0), uint64_t(0), uint64_t(900), uint64_t(trackNo),
                               uint64_t(2), uint64_t(0), uint64_t(1), uint64_t(1), uint64_t(2), uint64_t(0),
                               uint64_t(0), uint64_t(Track::kInLibrary | Track::kLoaded), uint64_t(0), uint64_t(0)})
            PutU(out, value);
        PutI(out, kNoGain);
        PutI(out, kNoGain);
    };
    track(2, 4, 7, 1);
    track(3, 5, 8, 1);
    PutU(out, 0);  // no playlists; version 1 ends here
    return out;
}

}  // namespace

void TestAlbumEditions(const std::wstring& scratch) {
    ResetLibrary();

    // --- Edition title parsing --------------------------------------------------------------
    {
        struct Case { const char* title; const char* base; EditionKind kind; };
        const Case cases[] = {
            {"Album (Remastered)", "album", EditionKind::Remaster},
            {"Album [Deluxe Edition]", "album", EditionKind::Deluxe},
            {"Album - 2011 Remaster", "album", EditionKind::Remaster},
            {"ALBUM: Instrumental", "album", EditionKind::Instrumental},
            {"Album (Japanese Edition)", "album", EditionKind::Regional},
            {"Album (Mono)", "album", EditionKind::Mono},
            {"Album (Live)", "album live", EditionKind::Original},
            {"The Album", "album", EditionKind::Original},
            {"Album (2009 Remaster)", "album", EditionKind::Remaster},
        };
        testlog::Cases parsed;
        for (const auto& item : cases) {
            EditionKind kind = EditionKind::Original;
            std::string base = AlbumBaseTitle(item.title, &kind);
            parsed.Add(base == item.base && kind == item.kind,
                       FromUtf8(std::string(item.title) + " => " + base + "/" + std::to_string((int)kind)));
        }
        Check(parsed.Passed(), L"EDITIONS edition titles handle brackets, years, case, mono, regional and live labels%ls",
              parsed.failed.c_str());
    }

    // --- Conservative automatic grouping ---------------------------------------------------
    std::vector<TrackId> original = {
        AddTrack(scratch + L"\\editions\\original1.flac", "Album Artist", "Album", "Opening", "1999", 1, 180000),
        AddTrack(scratch + L"\\editions\\original2.flac", "Album Artist", "Album", "Closing", "1999", 2, 200000),
    };
    std::vector<TrackId> remaster = {
        AddTrack(scratch + L"\\editions\\remaster1.flac", "Album Artist", "Album (Remastered)", "Opening", "2011", 1, 180200),
        AddTrack(scratch + L"\\editions\\remaster2.flac", "Album Artist", "Album (Remastered)", "Closing", "2011", 2, 200300),
    };
    std::vector<TrackId> deluxe = {
        AddTrack(scratch + L"\\editions\\deluxe1.flac", "Album Artist", "Album (Deluxe Edition)", "Opening", "2019", 1, 180100),
        AddTrack(scratch + L"\\editions\\deluxe2.flac", "Album Artist", "Album (Deluxe Edition)", "Closing", "2019", 2, 200400),
        AddTrack(scratch + L"\\editions\\deluxe3.flac", "Album Artist", "Album (Deluxe Edition)", "Bonus", "2019", 3, 95000),
    };
    std::vector<TrackId> otherArtist = {
        AddTrack(scratch + L"\\editions\\other1.flac", "Other Artist", "Album", "Opening", "1999", 1, 180000),
    };
    std::vector<TrackId> live = {
        AddTrack(scratch + L"\\editions\\live1.flac", "Album Artist", "Album (Live)", "Opening", "2001", 1, 190000),
    };
    std::vector<TrackId> tribute = {
        AddTrack(scratch + L"\\editions\\tribute1.flac", "Album Artist", "Album (Tribute)", "Opening", "2002", 1, 180000),
    };
    bool grouped = AutoGroupAlbums();
    const AlbumGroup* group = GroupOf(original);
    bool oneGroup = group && group->editions.size() == 3 && group->editions[0].tracks.size() == 2 &&
                    group->editions[1].tracks.size() == 2 && group->editions[2].tracks.size() == 3;
    bool separate = GroupOf(otherArtist) == nullptr && GroupOf(live) == nullptr && GroupOf(tribute) == nullptr;
    bool preferred = group && PreferredEditionId(*group) == group->editions[0].id &&
                     group->editions[0].kind == EditionKind::Original;
    Check(grouped && oneGroup && separate && preferred,
          L"EDITIONS heuristic grouping finds original/remaster/deluxe, keeps other artists, live and tribute separate");
    if (group) {
        Check(EditionLabel(group->editions[1]).find(L"remastered") != std::wstring::npos &&
                  EditionLabel(group->editions[2]).find(L"deluxe") != std::wstring::npos,
              L"EDITIONS automatic labels name the release kind");
    }

    // --- MusicBrainz identifiers are authoritative; missing ids do not group -----------------
    ResetLibrary();
    std::vector<TrackId> mbA = {
        AddTrack(scratch + L"\\editions\\mbA.flac", "Different Artist", "First Name", "Song", "2001", 1, 100000,
                 "release-group-42", "release-a"),
    };
    std::vector<TrackId> mbB = {
        AddTrack(scratch + L"\\editions\\mbB.flac", "Another Artist", "Another Name", "Song", "2020", 1, 100000,
                 "release-group-42", "release-b"),
    };
    std::vector<TrackId> mbC = {
        AddTrack(scratch + L"\\editions\\mbC.flac", "Different Artist", "First Name (Remastered)", "Song", "2021", 1, 100000,
                 "release-group-other", "release-c"),
    };
    AutoGroupAlbums();
    const AlbumGroup* mbGroup = GroupOf(mbA);
    bool mbOk = mbGroup && mbGroup->editions.size() == 2 && GroupOf(mbB) == mbGroup &&
                mbGroup->musicBrainzReleaseGroup == "release-group-42" &&
                mbGroup->editions[0].source == EditionSource::MusicBrainz && GroupOf(mbC) == nullptr;
    Check(mbOk, L"EDITIONS a shared MusicBrainz release-group id groups different titles, and a different id stays separate");

    // --- Optional MusicBrainz provider results ----------------------------------------------
    ResetLibrary();
    std::vector<TrackId> lookupA = {
        AddTrack(scratch + L"\\editions\\lookupA.flac", "Lookup Band", "Lookup Album", "One", "2001", 1, 100000),
    };
    std::vector<TrackId> lookupB = {
        AddTrack(scratch + L"\\editions\\lookupB.flac", "Lookup Band", "Lookup Album (Remastered)", "One", "2011", 1, 100000),
    };
    auto lookup = AlbumLookupCandidates();
    std::string parsed = musicbrainz::ParseReleaseGroupSearch(
        "{\"release-groups\":[{\"id\":\"rg-unique\",\"title\":\"Lookup Album\",\"score\":\"100\","
        "\"artist-credit\":[{\"name\":\"Lookup Band\"}]}]}",
        "Lookup Band", "Lookup Album");
    std::string ambiguousResult = musicbrainz::ParseReleaseGroupSearch(
        "{\"release-groups\":[{\"id\":\"rg-a\",\"title\":\"Lookup Album\",\"score\":\"100\","
        "\"artist-credit\":[{\"name\":\"Lookup Band\"}]},{\"id\":\"rg-b\",\"title\":\"Lookup Album\","
        "\"score\":\"99\",\"artist-credit\":[{\"name\":\"Lookup Band\"}]}]}",
        "Lookup Band", "Lookup Album");
    std::string wrongArtist = musicbrainz::ParseReleaseGroupSearch(
        "{\"release-groups\":[{\"id\":\"rg-c\",\"title\":\"Lookup Album\",\"score\":\"100\","
        "\"artist-credit\":[{\"name\":\"Someone Else\"}]}]}",
        "Lookup Band", "Lookup Album");
    bool applied = lookup.size() == 1 && lookup[0].artist == "Lookup Band" && lookup[0].title == "Lookup Album" &&
                   parsed == "rg-unique" && ambiguousResult.empty() && wrongArtist.empty() &&
                   ApplyMusicBrainzReleaseGroup("Lookup Band", "Lookup Album", "rg-unique");
    AutoGroupAlbums();
    const AlbumGroup* lookupGroup = GroupOf(lookupA);
    bool providerOk = applied && lookupGroup && lookupGroup->editions.size() == 2 &&
                      lookupGroup->musicBrainzReleaseGroup == "rg-unique" && GroupOf(lookupB) == lookupGroup;
    Check(providerOk,
          L"EDITIONS MusicBrainz candidates, conservative search parsing and provider ids group without overriding tags");

    // --- Retagging an album refreshes its identity without losing editions -------------------
    const AlbumGroup* retagGroup = lookupGroup;
    for (TrackId id : lookupA) {
        TagData tags;
        tags.artist = "Lookup Band";
        tags.albumArtist = "Lookup Band";
        tags.album = "Renamed Album";
        tags.title = "One";
        tags.date = "2001";
        tags.track = 1;
        g_lib.ApplyTags(id, tags);
    }
    TrackId renamedTrack = AddTrack(scratch + L"\\editions\\renamed2.flac", "Lookup Band", "Renamed Album", "Two",
                                    "2001", 2, 100000);
    ReconcileAlbumGroups();
    retagGroup = retagGroup ? FindAlbumGroupConst(retagGroup->id) : nullptr;
    bool retagged = retagGroup && retagGroup->title == "Renamed Album" &&
                    std::any_of(retagGroup->editions.begin(), retagGroup->editions.end(), [&](const AlbumEdition& edition) {
                        return edition.album == "Renamed Album" &&
                               std::find(edition.tracks.begin(), edition.tracks.end(), renamedTrack) != edition.tracks.end();
                    });
    std::wstring editionField;
    bool editionLabel = !TrackEditionLabel(lookupA[0]).empty() &&
                        LookupField(lookupA[0], L"edition", editionField) &&
                        editionField == TrackEditionLabel(lookupA[0]);
    Check(retagged && editionLabel,
          L"EDITIONS retagging refreshes the album key, joins new tracks, and exposes the edition label");

    // --- Manual grouping, labels and rescan safety ------------------------------------------
    ResetLibrary();
    std::vector<TrackId> manualA = {AddTrack(scratch + L"\\editions\\manualA.flac", "Band", "First Record", "A", "1990")};
    std::vector<TrackId> manualB = {AddTrack(scratch + L"\\editions\\manualB.flac", "Band", "Second Record", "B", "1992")};
    std::vector<TrackId> manualC = {AddTrack(scratch + L"\\editions\\manualC.flac", "Band", "Third Record", "C", "1994")};
    std::wstring error;
    bool manual = GroupAlbumsAsEditions({SelectionFor(manualA), SelectionFor(manualB), SelectionFor(manualC)}, &error);
    const AlbumGroup* manualGroup = GroupOf(manualA);
    bool manualOk = manual && manualGroup && manualGroup->manual && manualGroup->editions.size() == 3;
    uint32_t manualId = manualGroup ? manualGroup->id : 0;
    uint32_t labelEdition = manualGroup ? manualGroup->editions[1].id : 0;
    bool renamed = manualGroup && SetEditionLabel(manualGroup->id, labelEdition, L"Band's second pressing");
    bool chose = manualGroup && SetPreferredEdition(manualGroup->id, labelEdition);
    TrackId manualD = AddTrack(scratch + L"\\editions\\manualD.flac", "Band", "Fourth Record", "D", "2000");
    bool added = GroupAlbumsAsEditions({SelectionFor(manualA), SelectionFor({manualD})}, &error);
    manualGroup = FindAlbumGroupConst(manualId);
    bool addedOk = added && manualGroup && manualGroup->editions.size() == 4;
    bool beforeCount = g_albumGroups.size() == 1;
    AutoGroupAlbums();
    AutoGroupAlbums();
    manualGroup = FindAlbumGroupConst(manualId);
    bool survived = manualGroup && manualGroup->editions.size() == 4 && manualGroup->preferred == labelEdition &&
                    EditionLabel(*FindAlbumEdition(*manualGroup, labelEdition)) == L"Band's second pressing";
    Check(manualOk && renamed && chose && addedOk && beforeCount && survived,
          L"EDITIONS manual grouping, renaming and preferred choice survive repeated automatic passes");
    // A new file for an existing manual edition joins it, without adding a group.
    TrackId newA = AddTrack(scratch + L"\\editions\\manualA2.flac", "Band", "First Record", "A2", "1990", 2);
    ReconcileAlbumGroups();
    manualGroup = FindAlbumGroupConst(manualId);
    bool joined = manualGroup && manualGroup->editions[0].tracks.size() == 2 &&
                  std::find(manualGroup->editions[0].tracks.begin(), manualGroup->editions[0].tracks.end(), newA) !=
                      manualGroup->editions[0].tracks.end();
    bool removed = manualGroup && RemoveEditionFromGroup(manualGroup->id, manualGroup->editions[2].id);
    size_t groupsAfterRemove = g_albumGroups.size();
    AutoGroupAlbums();
    const AlbumGroup* removedGroup = GroupOf(manualC);
    bool noRejoin = g_albumGroups.size() == groupsAfterRemove && removedGroup && removedGroup->manual &&
                    removedGroup->id != manualId && removedGroup->editions.size() == 1;
    manualGroup = FindAlbumGroupConst(manualId);
    bool split = manualGroup && SplitAlbumGroup(manualGroup->id);
    size_t groupsAfterSplit = g_albumGroups.size();
    AutoGroupAlbums();
    const AlbumGroup* splitA = GroupOf(manualA);
    const AlbumGroup* splitB = GroupOf(manualB);
    Check(joined && removed && noRejoin && split && g_albumGroups.size() == groupsAfterSplit && splitA && splitB &&
              splitA->id != splitB->id && splitA->editions.size() == 1 && splitB->editions.size() == 1,
          L"EDITIONS rescans join new files, removal makes a separate album and split is not silently undone");

    // --- Track correspondence ---------------------------------------------------------------
    ResetLibrary();
    std::vector<TrackId> core = {
        AddTrack(scratch + L"\\editions\\cmpA1.flac", "Band", "Core", "Song One", "2000", 1, 200000),
        AddTrack(scratch + L"\\editions\\cmpA2.flac", "Band", "Core", "Song Two", "2000", 2, 210000),
        AddTrack(scratch + L"\\editions\\cmpA3.flac", "Band", "Core", "Exclusive A", "2000", 3, 100000),
    };
    std::vector<TrackId> expanded = {
        AddTrack(scratch + L"\\editions\\cmpB1.flac", "Band", "Core (Expanded)", "Song One", "2005", 1, 200400),
        AddTrack(scratch + L"\\editions\\cmpB2.flac", "Band", "Core (Expanded)", "Song Two", "2005", 2, 280000),
        AddTrack(scratch + L"\\editions\\cmpB3.flac", "Band", "Core (Expanded)", "Exclusive B", "2005", 3, 90000),
    };
    std::vector<TrackId> ambiguous = {
        AddTrack(scratch + L"\\editions\\cmpC1.flac", "Band", "Core (Alternate Mix)", "Song One", "2010", 1, 200100),
        AddTrack(scratch + L"\\editions\\cmpC2.flac", "Band", "Core (Alternate Mix)", "Song One", "2010", 1, 199900),
    };
    AutoGroupAlbums();
    const AlbumGroup* compareGroup = GroupOf(core);
    bool compareOk = compareGroup && compareGroup->editions.size() == 3;
    uint32_t coreId = compareGroup ? compareGroup->editions[0].id : 0;
    uint32_t expandedId = compareGroup ? compareGroup->editions[1].id : 0;
    uint32_t ambiguousId = compareGroup ? compareGroup->editions[2].id : 0;
    auto rows = compareOk ? CompareEditions(compareGroup->id, coreId, expandedId) : std::vector<EditionComparisonRow>{};
    auto findRow = [](const std::vector<EditionComparisonRow>& list, TrackId a, TrackId b) {
        for (const auto& row : list) if (row.a == a && row.b == b) return &row;
        return (const EditionComparisonRow*)nullptr;
    };
    const EditionComparisonRow* same = findRow(rows, core[0], expanded[0]);
    const EditionComparisonRow* master = findRow(rows, core[1], expanded[1]);
    bool exclusives = !rows.empty() && std::any_of(rows.begin(), rows.end(), [&](const auto& row) {
        return row.a == core[2] && row.b == kInvalidTrack;
    }) && std::any_of(rows.begin(), rows.end(), [&](const auto& row) {
        return row.a == kInvalidTrack && row.b == expanded[2];
    });
    Check(compareOk && same && same->relation == TrackRelation::SameRecording && same->status == MatchStatus::Suggested &&
              master && master->relation == TrackRelation::DifferentMaster && exclusives,
          L"EDITIONS comparison identifies shared, different-master and exclusive tracks");
    auto ambiguousRows = compareOk ? CompareEditions(compareGroup->id, coreId, ambiguousId) : std::vector<EditionComparisonRow>{};
    bool ambiguousPair = findRow(ambiguousRows, core[0], ambiguous[0]) || findRow(ambiguousRows, core[0], ambiguous[1]);
    Check(ambiguousRows.size() >= 2 && !ambiguousPair,
          L"EDITIONS duplicate titles are left unknown instead of being paired arbitrarily");
    bool confirmed = SetTrackMatch(compareGroup->id, core[0], expanded[1], MatchStatus::Confirmed,
                                   TrackRelation::DifferentPerformance);
    auto confirmedRows = CompareEditions(compareGroup->id, coreId, expandedId);
    const EditionComparisonRow* manualMatch = findRow(confirmedRows, core[0], expanded[1]);
    auto versionList = OtherVersions(compareGroup->id, core[0]);
    bool versions = std::any_of(versionList.begin(), versionList.end(),
                                [&](const TrackVersion& version) { return version.track == expanded[1]; });
    bool preferredTrack = SetPreferredTrackVersion(compareGroup->id, expanded[0]);
    bool cleared = ClearPreferredTrackVersion(compareGroup->id, expanded[0]);
    bool rejected = SetTrackMatch(compareGroup->id, core[0], expanded[0], MatchStatus::Rejected);
    auto rejectedRows = CompareEditions(compareGroup->id, coreId, expandedId);
    bool rejectedGone = !findRow(rejectedRows, core[0], expanded[0]);
    Check(confirmed && manualMatch && manualMatch->status == MatchStatus::Confirmed && versions && preferredTrack && cleared &&
              rejected && rejectedGone,
          L"EDITIONS manual confirm/reject and preferred track versions are explicit and take effect");

    // --- Search -----------------------------------------------------------------------------
    ResetLibrary();
    std::vector<TrackId> searchOriginal = {
        AddTrack(scratch + L"\\editions\\search1.flac", "Search Band", "Search Album", "Only Song", "1999", 1, 100000),
    };
    std::vector<TrackId> searchDeluxe = {
        AddTrack(scratch + L"\\editions\\search2.flac", "Search Band", "Search Album (Deluxe Edition)", "Only Song", "2019", 1, 100000),
    };
    AutoGroupAlbums();
    auto words = FilterWords(L"deluxe");
    TrackMatcher deluxeMatcher(g_lib, words);
    bool deluxeMatches = deluxeMatcher.Matches(searchDeluxe[0]) && !deluxeMatcher.Matches(searchOriginal[0]);
    auto titleWords = FilterWords(L"search album");
    TrackMatcher titleMatcher(g_lib, titleWords);
    Check(deluxeMatches && titleMatcher.Matches(searchOriginal[0]) && titleMatcher.Matches(searchDeluxe[0]),
          L"EDITIONS search finds an edition label and the canonical album title without duplicate-looking results");

    // --- Persistence, playlists and migration ------------------------------------------------
    ResetLibrary();
    std::vector<TrackId> saveA = {
        AddTrack(scratch + L"\\editions\\saveA.flac", "Save Band", "Saved Album", "One", "1998", 1, 180000),
        AddTrack(scratch + L"\\editions\\saveA2.flac", "Save Band", "Saved Album", "Two", "1998", 2, 190000),
    };
    std::vector<TrackId> saveB = {
        AddTrack(scratch + L"\\editions\\saveB.flac", "Save Band", "Saved Album (Remastered)", "One", "2011", 1, 180100),
        AddTrack(scratch + L"\\editions\\saveB2.flac", "Save Band", "Saved Album (Remastered)", "Two", "2011", 2, 190100),
    };
    AutoGroupAlbums();
    const AlbumGroup* savedGroup = GroupOf(saveA);
    bool setup = savedGroup && savedGroup->editions.size() == 2;
    uint32_t savedGroupId = savedGroup ? savedGroup->id : 0;
    uint32_t savedPreferred = savedGroup ? savedGroup->editions[0].id : 0;
    bool labelled = savedGroup && SetEditionLabel(savedGroupId, savedGroup->editions[1].id, L"Remaster label");
    bool matched = savedGroup && SetTrackMatch(savedGroupId, saveA[0], saveB[0], MatchStatus::Confirmed,
                                               TrackRelation::SameRecording);
    bool art = savedGroup && SetAlbumGroupArt(savedGroupId, saveB[0]);
    std::vector<Playlist> playlists{{L"Library", {}, PlaylistKind::Library},
                                    {L"Saved mix", {saveA[1], saveB[0]}, PlaylistKind::Static}};
    const std::wstring file = TempFile(scratch, L"editions");
    DeleteDatabase(file);
    bool wrote = setup && labelled && matched && art && SaveLibrary(file, playlists);
    ResetLibrary();
    std::vector<Playlist> back;
    bool read = LoadLibrary(file, back);
    const AlbumGroup* loaded = FindAlbumGroupConst(savedGroupId);
    TrackId loadedA = g_lib.Find(scratch + L"\\editions\\saveA.flac");
    TrackId loadedA2 = g_lib.Find(scratch + L"\\editions\\saveA2.flac");
    TrackId loadedB = g_lib.Find(scratch + L"\\editions\\saveB.flac");
    bool ids = loadedA != kInvalidTrack && loadedA2 != kInvalidTrack && loadedB != kInvalidTrack;
    bool groupBack = loaded && loaded->editions.size() == 2 && loaded->preferred == savedPreferred &&
                     EditionLabel(*FindAlbumEdition(*loaded, loaded->editions[1].id)) == L"Remaster label";
    bool artBack = loaded && loaded->artTrack == loadedB;
    bool matchBack = loaded && loaded->matches.size() == 1 && loaded->matches[0].a == loadedA &&
                     loaded->matches[0].b == loadedB && loaded->matches[0].status == MatchStatus::Confirmed;
    bool playlistsBack = back.size() == 2 && back[1].items == std::vector<TrackId>{loadedA2, loadedB};
    Check(wrote && read && ids && groupBack && artBack && matchBack && playlistsBack,
          L"EDITIONS save/load preserves groups, labels, preferred choice, group cover, matches and playlists");
    // The backup archive uses the same section; a copied profile must keep the organization too.
    const std::wstring archiveFile = TempFile(scratch, L"archive");
    DeleteDatabase(archiveFile);
    LibraryArchive snapshot = CaptureLibraryArchive(back);
    std::wstring archiveError;
    bool archiveWrote = WriteLibraryArchive(archiveFile, snapshot, archiveError);
    LibraryArchive archiveBack;
    bool archiveRead = ReadLibraryArchive(archiveFile, archiveBack);
    bool archiveGroup = archiveBack.albumGroups.size() == 1 && archiveBack.albumGroups[0].editions.size() == 2 &&
                        archiveBack.albumGroups[0].preferred == savedPreferred &&
                        EditionLabel(archiveBack.albumGroups[0].editions[1]) == L"Remaster label";
    Check(archiveWrote && archiveRead && archiveGroup,
          L"EDITIONS a library archive copy keeps editions, labels and the preferred choice");
    DeleteDatabase(archiveFile);
    // Idempotent rescan and a second round trip.
    AutoGroupAlbums();
    const AlbumGroup* stableGroup = FindAlbumGroupConst(savedGroupId);
    bool stable = g_albumGroups.size() == 1 && stableGroup && stableGroup->editions.size() == 2;
    bool wroteAgain = SaveLibrary(file, back);
    ResetLibrary();
    std::vector<Playlist> backAgain;
    bool readAgain = LoadLibrary(file, backAgain);
    Check(stable && wroteAgain && readAgain && g_albumGroups.size() == 1 &&
              FindAlbumGroupConst(savedGroupId)->editions.size() == 2,
          L"EDITIONS rescan and a repeated save/load do not duplicate groups or editions");
    DeleteDatabase(file);

    // A version-1 file predates the section; loading migrates it into editions without losing tracks.
    ResetLibrary();
    const std::wstring legacy = TempFile(scratch, L"legacy");
    DeleteDatabase(legacy);
    bool legacyWritten = WriteTestBytes(legacy, LegacyLibrary());
    std::vector<Playlist> legacyPlaylists;
    bool legacyLoaded = legacyWritten && LoadLibrary(legacy, legacyPlaylists);
    bool legacyTracks = legacyLoaded && g_lib.Count() == 2;
    const AlbumGroup* migrated = legacyTracks ? AlbumGroupForTrack(0) : nullptr;
    bool migratedOk = migrated && migrated->editions.size() == 2 && migrated->editions[0].tracks.size() == 1 &&
                      migrated->editions[1].tracks.size() == 1 && g_albumGroups.size() == 1;
    Check(legacyLoaded && legacyTracks && migratedOk,
          L"EDITIONS a version-1 library migrates to editions without losing its tracks");
    DeleteDatabase(legacy);

    // --- Compaction remaps edition ids -------------------------------------------------------
    ResetLibrary();
    std::vector<TrackId> compactA = {AddTrack(scratch + L"\\editions\\compactA.flac", "Compact", "Record", "A", "2000")};
    std::vector<TrackId> compactB = {AddTrack(scratch + L"\\editions\\compactB.flac", "Compact", "Record (Remastered)", "A", "2010")};
    AddTrack(scratch + L"\\editions\\unused.flac", "Unused", "Nothing", "Unused", "2020");
    AutoGroupAlbums();
    const AlbumGroup* compactGroup = GroupOf(compactA);
    uint32_t compactGroupId = compactGroup ? compactGroup->id : 0;
    std::wstring pathA = g_lib.Path(compactA[0]), pathB = g_lib.Path(compactB[0]);
    std::vector<TrackId> compactLive{compactA[0], compactB[0]};
    auto mapping = g_lib.Compact(compactLive);
    const AlbumGroup* after = FindAlbumGroupConst(compactGroupId);
    bool remapped = after && after->editions.size() == 2 && after->editions[0].tracks.size() == 1 &&
                    g_lib.Path(after->editions[0].tracks[0]) == pathA && g_lib.Path(after->editions[1].tracks[0]) == pathB;
    Check(remapped, L"EDITIONS library compaction keeps album membership on the remapped track ids");

    // --- Empty, incomplete and ambiguous metadata --------------------------------------------
    ResetLibrary();
    AddTrack(scratch + L"\\editions\\untagged.flac", "", "", "", "", 0, 0);
    AddTrack(scratch + L"\\editions\\untagged2.flac", "", "Shared", "", "", 0, 0);
    bool noCrash = !AutoGroupAlbums() && g_albumGroups.empty();
    ResetLibrary();
    AddTrack(scratch + L"\\editions\\twin1.flac", "Twin Band", "Same (Deluxe Edition)", "Song", "2001", 1, 100000);
    AddTrack(scratch + L"\\editions\\twin2.flac", "Twin Band", "Same (Deluxe Edition)", "Song", "2011", 1, 100000);
    bool ambiguousCopies = !AutoGroupAlbums() && g_albumGroups.empty();
    Check(noCrash && ambiguousCopies,
          L"EDITIONS empty metadata and two copies of one full title create no speculative group");

    ResetLibrary();
}
