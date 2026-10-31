// Album groups and their editions: one conceptual album can hold several releases (original,
// remaster, deluxe, regional, instrumental...) without merging their tracks or files.
//
// Tracks stay the single source of truth. An edition owns a list of track ids, and a track belongs
// to at most one edition; albums with one edition keep working exactly as before. The database
// section (library.cdb tag 8) stores the groups, manual labels, preferred choices and confirmed
// track correspondences, so rescans never need to guess again.
#pragma once
#include "library.h"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// How an edition differs from the plain release. Values are stored, so append only.
enum class EditionKind : uint8_t {
    Original, Remaster, Deluxe, Expanded, Anniversary, Reissue, Special, Bonus, Regional, Mono, Stereo,
    AlternateMix, Instrumental, Drumless, Other, Count,
};
const wchar_t* EditionKindName(EditionKind kind);

// Where the grouping came from. Manual is never undone by a rescan or a metadata refresh.
enum class EditionSource : uint8_t { MusicBrainz, Heuristic, Manual, Count };

// What a track pair means. Unknown is honest: do not invent a relationship.
enum class TrackRelation : uint8_t { SameRecording, DifferentMaster, AlternateMix, DifferentPerformance, Unknown, Count };
enum class MatchStatus : uint8_t { Suggested, Confirmed, Rejected, Count };

struct TrackMusicBrainz {
    std::string releaseGroup;  // MUSICBRAINZ_RELEASEGROUPID
    std::string release;       // MUSICBRAINZ_ALBUMID / RELEASEID
};

struct TrackMatch {
    TrackId a = kInvalidTrack, b = kInvalidTrack;
    TrackRelation relation = TrackRelation::Unknown;
    MatchStatus status = MatchStatus::Suggested;
    uint8_t confidence = 0;
};

struct PreferredTrackVersion {
    std::string key;  // normalized recording/title key
    TrackId track = kInvalidTrack;
};

struct AlbumEdition {
    uint32_t id = 0;  // stable, globally unique while the profile lives
    // The album key this edition was built from, kept for rescans. UTF-8.
    std::string artist, album, date;
    std::string label;  // user label; empty means the automatic kind + year label
    EditionKind kind = EditionKind::Original;
    EditionSource source = EditionSource::Heuristic;
    std::string musicBrainzRelease;
    uint8_t confidence = 0;
    uint32_t created = 0, updated = 0;
    std::vector<TrackId> tracks;  // album order: disc, track number, then id
};

struct AlbumGroup {
    uint32_t id = 0;
    std::string title, artist;  // canonical display values, UTF-8
    std::string musicBrainzReleaseGroup;
    TrackId artTrack = kInvalidTrack;  // explicit group cover; invalid falls back to the preferred edition
    uint32_t preferred = 0;  // edition id, 0 only while a group is being built
    bool manual = false;     // a user grouped, split or edited this group: automatic passes leave it alone
    uint32_t created = 0, updated = 0;
    std::vector<AlbumEdition> editions;
    std::vector<TrackMatch> matches;                // confirmed/suggested/rejected pairs across editions
    std::vector<PreferredTrackVersion> preferredTracks;
};

// The one album database. UI-thread owned like g_lib; workers get snapshots through LibraryArchive.
extern std::vector<AlbumGroup> g_albumGroups;
extern std::map<TrackId, TrackMusicBrainz> g_trackMusicBrainz;

// A track's group/edition, or 0. Rebuilt lazily; a handful of editions costs less than a map.
struct AlbumTrackIndex {
    std::vector<uint32_t> group;    // by TrackId, 0 = not in an explicit edition
    std::vector<uint32_t> edition;
    uint32_t revision = 0;
};
const AlbumTrackIndex& TrackAlbumIndex();

uint32_t AlbumGroupsRevision();
void AlbumGroupsChanged();
void ResetAlbumGroups();
AlbumGroup* FindAlbumGroup(uint32_t id);
const AlbumGroup* FindAlbumGroupConst(uint32_t id);
AlbumEdition* FindAlbumEdition(AlbumGroup& group, uint32_t editionId);
const AlbumEdition* FindAlbumEdition(const AlbumGroup& group, uint32_t editionId);
const AlbumGroup* AlbumGroupForTrack(TrackId id);
const AlbumEdition* AlbumEditionForTrack(TrackId id);
uint32_t PreferredEditionId(const AlbumGroup& group);

// Album-level MusicBrainz ids travel with the track tags. They are persisted separately so the
// compact Track record keeps its size and older databases stay readable.
void SetTrackMusicBrainz(TrackId id, std::string_view releaseGroup, std::string_view release);

// Rescan/migration support. AutoGroupAlbums is conservative and idempotent: it only creates a
// group when MusicBrainz ids or edition markers make the relation reliable, and never touches a
// group a user made or edited.
bool AutoGroupAlbums();
bool ReconcileAlbumGroups();
void MarkAlbumTracks(std::vector<bool>& retained);
void RemapAlbumGroups(const std::vector<TrackId>& mapping);

// A snapshot of one album shown in the library, for manual grouping. `groupId`/`editionId` are 0
// for an ordinary album; `tracks` may be empty when the caller only knows its identity.
struct AlbumSelection {
    std::string artist, album, date;
    std::vector<TrackId> tracks;
    uint32_t groupId = 0, editionId = 0;
};
bool GroupAlbumsAsEditions(const std::vector<AlbumSelection>& albums, std::wstring* error = nullptr);
bool RemoveEditionFromGroup(uint32_t groupId, uint32_t editionId);
bool SplitAlbumGroup(uint32_t groupId);
bool SetPreferredEdition(uint32_t groupId, uint32_t editionId);
bool SetEditionLabel(uint32_t groupId, uint32_t editionId, std::wstring_view label);
bool SetAlbumGroupArt(uint32_t groupId, TrackId track);
bool ClearAlbumGroupArt(uint32_t groupId);
bool AllowAutomaticGrouping(uint32_t groupId);

// Edition comparison. Rows with kInvalidTrack on one side are exclusive to the other edition.
struct EditionComparisonRow {
    TrackId a = kInvalidTrack, b = kInvalidTrack;
    TrackRelation relation = TrackRelation::Unknown;
    MatchStatus status = MatchStatus::Suggested;
    uint8_t confidence = 0;
};
std::vector<EditionComparisonRow> CompareEditions(uint32_t groupId, uint32_t editionA, uint32_t editionB);
struct TrackVersion {
    TrackId track = kInvalidTrack;
    uint32_t edition = 0;
};
std::vector<TrackVersion> OtherVersions(uint32_t groupId, TrackId track);
bool SetTrackMatch(uint32_t groupId, TrackId a, TrackId b, MatchStatus status, TrackRelation relation = TrackRelation::SameRecording);
bool SetPreferredTrackVersion(uint32_t groupId, TrackId track);
bool ClearPreferredTrackVersion(uint32_t groupId, TrackId track);

// Text helpers shared by the views and the tests.
std::wstring EditionLabel(const AlbumEdition& edition);
std::wstring EditionKindLabel(EditionKind kind);
std::wstring AlbumEditionSearchText(TrackId id);
std::wstring TrackEditionLabel(TrackId id);  // empty for an ordinary, ungrouped album

// One album identity worth asking MusicBrainz about. `title` is the base title, without an edition
// suffix, so an original and its remaster share one lookup and one release-group result.
struct AlbumLookupCandidate {
    std::string artist, title;
};
std::vector<AlbumLookupCandidate> AlbumLookupCandidates();
// Applies an authoritative release-group id to every local album with this artist/base title, but
// only where no tag already supplied an id. Returns true when a track's metadata changed.
bool ApplyMusicBrainzReleaseGroup(std::string_view artist, std::string_view title, std::string_view releaseGroup);
std::string NormalizeAlbumText(std::string_view text);
std::string AlbumBaseTitle(std::string_view album, EditionKind* kind = nullptr);

// The extension section codec (librarydb.cpp). savedToId maps a saved track index to a live id;
// newId maps a live id to its saved index (UINT32_MAX when it is not written).
std::string EncodeAlbumDatabase(const std::vector<AlbumGroup>& groups,
                                const std::map<TrackId, TrackMusicBrainz>& musicBrainz,
                                const std::vector<uint32_t>& newId);
bool DecodeAlbumDatabase(std::string_view data, uint64_t trackCount, const std::vector<TrackId>* savedToId,
                         std::vector<AlbumGroup>* groups, std::map<TrackId, TrackMusicBrainz>* musicBrainz);
std::string EncodeAlbumArt(const std::vector<AlbumGroup>& groups, const std::vector<uint32_t>& newId);
bool DecodeAlbumArt(std::string_view data, uint64_t trackCount, const std::vector<TrackId>* savedToId,
                    std::vector<AlbumGroup>* groups);
