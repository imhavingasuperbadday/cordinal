// What album_editions.cpp, album_editions_title.cpp, album_editions_edit.cpp and album_editions_db.cpp
// share: the id counters and track index, the parsed edition title, grouping candidates, and the helpers
// each file uses from the others. album_editions.h is the public contract; this header is private to them.
#pragma once
#include "album_editions.h"

#include <string>
#include <string_view>
#include <vector>

namespace albumed {

extern uint32_t s_revision;
extern uint32_t s_nextGroup, s_nextEdition;
extern bool s_idsDirty;
extern AlbumTrackIndex s_index;

constexpr size_t kMaxField = 65536;

struct EditionText {
    std::wstring base;       // display title with edition markers removed
    EditionKind kind = EditionKind::Original;
    bool live = false, tribute = false, karaoke = false, soundtrack = false, rerecord = false;
    int cues = 0;
};

struct Candidate {
    std::string artist, album, date;
    std::string artistKey, baseKey, displayBase;
    EditionKind kind = EditionKind::Original;
    int cues = 0;
    bool live = false, tribute = false, karaoke = false, soundtrack = false, rerecord = false;
    std::string releaseGroup;
    std::vector<TrackId> tracks;
    TrackId first = kInvalidTrack;
};

void RefreshIds();
uint32_t NewGroupId();
uint32_t NewEditionId();
const AlbumEdition* EditionOf(const AlbumGroup& group, uint32_t id);
AlbumEdition* EditionOf(AlbumGroup& group, uint32_t id);
std::wstring Trim(std::wstring_view text);
bool HasWord(const std::wstring& text, std::wstring_view word);
bool Contains(const std::wstring& text, std::wstring_view part);
int KindRank(EditionKind kind);
void AddCue(EditionText& info, EditionKind kind);
bool ParseCue(const std::wstring& segment, EditionText& info);
void FindCategories(const std::wstring& lower, EditionText& info);
bool IsSeparator(wchar_t c);
std::wstring RemoveCueBrackets(const std::wstring& text, EditionText& info);
std::wstring RemoveTrailingSegments(const std::wstring& text, EditionText& info);
EditionText SplitEditionTitle(std::string_view album);
std::string NormalizeWide(const std::wstring& text);
std::string AlbumKey(const std::string& artist, const std::string& album, const std::string& date);
std::string DisplayArtist(const Track& track);
std::string DisplayAlbum(const Track& track);
std::string DisplayDate(const Track& track);
bool VisibleLibraryTrack(const Track& track);
bool TrackOrderLess(TrackId a, TrackId b);
void SortTracks(std::vector<TrackId>& tracks);
void SortEditions(AlbumGroup& group);
void FixPreferred(AlbumGroup& group);
void FixArt(AlbumGroup& group);
void DropEmpty(std::vector<AlbumGroup>& groups);
const AlbumGroup* FindGroup(const std::vector<AlbumGroup>& groups, uint32_t id);
AlbumGroup* FindGroup(std::vector<AlbumGroup>& groups, uint32_t id);
bool OwnsTrack(const std::vector<AlbumGroup>& groups, TrackId id, uint32_t exceptGroup = 0, uint32_t exceptEdition = 0);
std::vector<TrackId> TracksForAlbum(const AlbumSelection& selection);
std::string BaseTitleDisplay(std::string_view album);
std::string NormalizedTitle(TrackId id);
std::string ReleaseIdForTrack(TrackId id);
std::string ReleaseGroupForTrack(TrackId id);
std::string MostCommon(const std::vector<std::string>& values);
std::vector<Candidate> BuildCandidates();
bool CompatibleCategory(const Candidate& a, const Candidate& b);
bool CompatibleWithEdition(const Candidate& candidate, const AlbumEdition& edition);
AlbumEdition MakeEdition(const Candidate& candidate, EditionSource source, uint8_t confidence);
uint32_t AddGroup(std::vector<AlbumGroup>& groups, const std::vector<Candidate>& candidates, const std::string& releaseGroup,
                  const char* why);
bool RefreshIdentities(std::vector<AlbumGroup>& groups);
bool Reconcile(std::vector<AlbumGroup>& groups, const std::vector<Candidate>& candidates, bool notify);
std::string GroupKey(const AlbumGroup& group);
std::string GroupKey(const Candidate& candidate);
std::string PairKey(TrackId a, TrackId b);
uint32_t EditionOfTrackInGroup(const AlbumGroup& group, TrackId id);

}  // namespace albumed
