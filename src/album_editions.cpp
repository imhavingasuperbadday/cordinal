// Album groups and editions: normalization, conservative identification, manual edits, track
// correspondence and the library.cdb section. See album_editions.h for the contract.
// This file holds the grouping itself and the lookups; titles, manual edits and the library.cdb section
// have their own files (album_editions_internal.h).
#include "album_editions_internal.h"

#include "sort.h"

#include <algorithm>
#include <cstdlib>
#include <cwctype>
#include <limits>
#include <set>
#include <unordered_map>
#include <unordered_set>

std::vector<AlbumGroup> g_albumGroups;
TrackMusicBrainzTable g_trackMusicBrainz;

void TrackMusicBrainzTable::Set(TrackId id, std::string_view releaseGroup, std::string_view release) {
    if (releaseGroup.empty() && release.empty()) {
        ids_.erase(id);
        return;
    }
    ids_[id] = {pool_.Intern(releaseGroup), pool_.Intern(release)};
}

TrackMusicBrainzTable::Ids TrackMusicBrainzTable::Get(TrackId id) const {
    const Handles* handles = ids_.find(id);
    return handles ? Ids{pool_.Get(handles->releaseGroup), pool_.Get(handles->release)} : Ids{};
}

size_t TrackMusicBrainzTable::MemoryBytes() const {
    return ids_.size() * sizeof(TrackIdMap<Handles>::Entry) + pool_.Bytes();
}

namespace albumed {

uint32_t s_revision = 1;
uint32_t s_nextGroup = 1, s_nextEdition = 1;
bool s_idsDirty = true;
AlbumTrackIndex s_index;

void RefreshIds() {
    if (!s_idsDirty) return;
    uint32_t groupMax = 0, editionMax = 0;
    for (const auto& group : g_albumGroups) {
        groupMax = std::max(groupMax, group.id);
        for (const auto& edition : group.editions) editionMax = std::max(editionMax, edition.id);
    }
    s_nextGroup = groupMax + 1;
    s_nextEdition = editionMax + 1;
    s_idsDirty = false;
}

uint32_t NewGroupId() {
    RefreshIds();
    return s_nextGroup++;
}
uint32_t NewEditionId() {
    RefreshIds();
    return s_nextEdition++;
}

const AlbumEdition* EditionOf(const AlbumGroup& group, uint32_t id) {
    for (const auto& edition : group.editions) if (edition.id == id) return &edition;
    return nullptr;
}
AlbumEdition* EditionOf(AlbumGroup& group, uint32_t id) {
    for (auto& edition : group.editions) if (edition.id == id) return &edition;
    return nullptr;
}


std::string DisplayArtist(const Track& track) {
    return std::string(track.albumArtist ? g_lib.View(track.albumArtist) : g_lib.View(track.artist));
}
std::string DisplayAlbum(const Track& track) { return std::string(g_lib.View(track.album)); }
std::string DisplayDate(const Track& track) { return std::string(g_lib.View(track.date)); }

bool VisibleLibraryTrack(const Track& track) {
    return (track.flags & (Track::kInLibrary | Track::kInbox)) && !(track.flags & (Track::kMissing | Track::kExcluded));
}

bool TrackOrderLess(TrackId a, TrackId b) {
    const Track& x = g_lib[a];
    const Track& y = g_lib[b];
    if (x.disc != y.disc) return x.disc < y.disc;
    if (x.trackNo != y.trackNo) return x.trackNo < y.trackNo;
    return a < b;
}

void SortTracks(std::vector<TrackId>& tracks) {
    std::sort(tracks.begin(), tracks.end(), TrackOrderLess);
    tracks.erase(std::unique(tracks.begin(), tracks.end()), tracks.end());
}

void SortEditions(AlbumGroup& group) {
    std::stable_sort(group.editions.begin(), group.editions.end(), [](const AlbumEdition& a, const AlbumEdition& b) {
        if (a.date.empty() != b.date.empty()) return !a.date.empty();
        if (a.date != b.date) return a.date < b.date;
        int ka = KindRank(a.kind), kb = KindRank(b.kind);
        if (ka != kb) return ka < kb;
        return a.id < b.id;
    });
}

void FixPreferred(AlbumGroup& group) {
    if (EditionOf(group, group.preferred)) return;
    group.preferred = 0;
    for (const auto& edition : group.editions) {
        if (edition.kind == EditionKind::Original) { group.preferred = edition.id; return; }
    }
    if (!group.editions.empty()) group.preferred = group.editions.front().id;
}

void FixArt(AlbumGroup& group) {
    if (group.artTrack == kInvalidTrack) return;
    for (const auto& edition : group.editions)
        if (std::find(edition.tracks.begin(), edition.tracks.end(), group.artTrack) != edition.tracks.end()) return;
    group.artTrack = kInvalidTrack;
}

void DropEmpty(std::vector<AlbumGroup>& groups) {
    for (auto& group : groups) {
        std::erase_if(group.editions, [](const AlbumEdition& edition) { return edition.tracks.empty(); });
        FixPreferred(group);
        FixArt(group);
    }
    std::erase_if(groups, [](const AlbumGroup& group) { return group.editions.empty(); });
}

const AlbumGroup* FindGroup(const std::vector<AlbumGroup>& groups, uint32_t id) {
    auto it = std::lower_bound(groups.begin(), groups.end(), id, [](const AlbumGroup& group, uint32_t value) {
        return group.id < value;
    });
    return it != groups.end() && it->id == id ? &*it : nullptr;
}
AlbumGroup* FindGroup(std::vector<AlbumGroup>& groups, uint32_t id) {
    auto it = std::lower_bound(groups.begin(), groups.end(), id, [](const AlbumGroup& group, uint32_t value) {
        return group.id < value;
    });
    return it != groups.end() && it->id == id ? &*it : nullptr;
}

bool OwnsTrack(const std::vector<AlbumGroup>& groups, TrackId id, uint32_t exceptGroup, uint32_t exceptEdition) {
    for (const auto& group : groups)
        for (const auto& edition : group.editions)
            if (!(group.id == exceptGroup && edition.id == exceptEdition) &&
                std::find(edition.tracks.begin(), edition.tracks.end(), id) != edition.tracks.end())
                return true;
    return false;
}

std::vector<TrackId> TracksForAlbum(const AlbumSelection& selection) {
    std::vector<TrackId> tracks;
    std::string artist = NormalizeAlbumText(selection.artist);
    std::string album = NormalizeAlbumText(selection.album);
    if (!artist.empty() && !album.empty()) {
        for (TrackId id = 0; id < (TrackId)g_lib.Count(); ++id) {
            const Track& track = g_lib[id];
            if (!VisibleLibraryTrack(track)) continue;
            if (NormalizeAlbumText(DisplayArtist(track)) == artist && NormalizeAlbumText(DisplayAlbum(track)) == album)
                tracks.push_back(id);
        }
    }
    if (tracks.empty()) tracks = selection.tracks;
    std::erase_if(tracks, [&](TrackId id) { return !g_lib.Contains(id); });
    return tracks;
}

std::string BaseTitleDisplay(std::string_view album) {
    EditionText info = SplitEditionTitle(album);
    return ToUtf8(info.base);
}

std::string NormalizedTitle(TrackId id) {
    if (!g_lib.Contains(id)) return {};
    const Track& track = g_lib[id];
    if (track.title) return NormalizeAlbumText(g_lib.View(track.title));
    return NormalizeAlbumText(ToUtf8(StemOf(g_lib.Path(id))));
}

std::string ReleaseIdForTrack(TrackId id) { return std::string(g_trackMusicBrainz.Get(id).release); }

std::string ReleaseGroupForTrack(TrackId id) { return std::string(g_trackMusicBrainz.Get(id).releaseGroup); }

std::string MostCommon(const std::vector<std::string>& values) {
    std::map<std::string, size_t> counts;
    for (const auto& value : values) if (!value.empty()) ++counts[value];
    std::string best;
    size_t bestCount = 0;
    for (const auto& [value, count] : counts) {
        if (count > bestCount || (count == bestCount && value < best)) {
            best = value;
            bestCount = count;
        }
    }
    return best;
}

std::vector<Candidate> BuildCandidates() {
    std::map<std::string, size_t> byKey;
    std::vector<Candidate> candidates;
    for (TrackId id = 0; id < (TrackId)g_lib.Count(); ++id) {
        const Track& track = g_lib[id];
        if (!VisibleLibraryTrack(track) || !track.album) continue;
        std::string artist = DisplayArtist(track), album = DisplayAlbum(track), date = DisplayDate(track);
        if (NormalizeAlbumText(artist).empty() || NormalizeAlbumText(album).empty()) continue;
        std::string key = AlbumKey(artist, album, date);
        auto [it, added] = byKey.try_emplace(key, candidates.size());
        if (added) {
            Candidate candidate;
            candidate.artist = artist;
            candidate.album = album;
            candidate.date = date;
            candidate.artistKey = NormalizeAlbumText(artist);
            EditionText info = SplitEditionTitle(album);
            candidate.baseKey = NormalizeAlbumText(ToUtf8(info.base));
            candidate.displayBase = BaseTitleDisplay(album);
            candidate.kind = info.kind;
            candidate.cues = info.cues;
            candidate.live = info.live;
            candidate.tribute = info.tribute;
            candidate.karaoke = info.karaoke;
            candidate.soundtrack = info.soundtrack;
            candidate.rerecord = info.rerecord;
            candidate.first = id;
            candidates.push_back(std::move(candidate));
        }
        Candidate& candidate = candidates[it->second];
        candidate.tracks.push_back(id);
    }
    for (auto& candidate : candidates) {
        SortTracks(candidate.tracks);
        std::vector<std::string> groups;
        for (TrackId id : candidate.tracks) groups.push_back(ReleaseGroupForTrack(id));
        candidate.releaseGroup = MostCommon(groups);
    }
    return candidates;
}

bool CompatibleCategory(const Candidate& a, const Candidate& b) {
    if (a.tribute || a.karaoke || b.tribute || b.karaoke) return false;
    if (a.live != b.live) return false;
    if (a.soundtrack != b.soundtrack) return false;
    if (a.rerecord != b.rerecord) return false;
    return true;
}

bool CompatibleWithEdition(const Candidate& candidate, const AlbumEdition& edition) {
    EditionText info = SplitEditionTitle(edition.album);
    if (candidate.tribute || candidate.karaoke || info.tribute || info.karaoke) return false;
    if (candidate.live != info.live || candidate.soundtrack != info.soundtrack || candidate.rerecord != info.rerecord)
        return false;
    return true;
}

AlbumEdition MakeEdition(const Candidate& candidate, EditionSource source, uint8_t confidence) {
    AlbumEdition edition;
    edition.id = NewEditionId();
    edition.artist = candidate.artist;
    edition.album = candidate.album;
    edition.date = candidate.date;
    edition.kind = candidate.kind;
    edition.source = source;
    std::vector<std::string> releases;
    for (TrackId id : candidate.tracks) releases.push_back(ReleaseIdForTrack(id));
    edition.musicBrainzRelease = MostCommon(releases);
    edition.confidence = confidence;
    edition.created = edition.updated = UnixNow();
    edition.tracks = candidate.tracks;
    return edition;
}

uint32_t AddGroup(std::vector<AlbumGroup>& groups, const std::vector<Candidate>& candidates, const std::string& releaseGroup,
                  const char* why) {
    (void)why;
    AlbumGroup group;
    group.id = NewGroupId();
    group.musicBrainzReleaseGroup = releaseGroup;
    group.created = group.updated = UnixNow();
    group.manual = false;
    // Prefer the plain release's title and artist for the group heading.
    const Candidate* canonical = nullptr;
    for (const auto& candidate : candidates)
        if (!canonical || (candidate.kind == EditionKind::Original && canonical->kind != EditionKind::Original) ||
            (candidate.kind == canonical->kind && candidate.date < canonical->date))
            canonical = &candidate;
    if (!canonical && !candidates.empty()) canonical = &candidates.front();
    group.title = canonical ? canonical->displayBase : std::string{};
    group.artist = canonical ? canonical->artist : std::string{};
    for (const auto& candidate : candidates) {
        bool mb = !releaseGroup.empty() && candidate.releaseGroup == releaseGroup;
        AlbumEdition edition = MakeEdition(candidate, mb ? EditionSource::MusicBrainz : EditionSource::Heuristic,
                                           (uint8_t)(mb ? 100 : std::min(90, 55 + candidate.cues * 10)));
        group.editions.push_back(std::move(edition));
    }
    SortEditions(group);
    FixPreferred(group);
    groups.push_back(std::move(group));
    std::sort(groups.begin(), groups.end(), [](const AlbumGroup& a, const AlbumGroup& b) { return a.id < b.id; });
    return groups.back().id;
}

// A tag edit changes an album's identity. Refresh each edition's cached key from its live tracks
// before grouping, so a renamed album keeps its editions and later tracks join the new name instead
// of creating a second group.
bool RefreshIdentities(std::vector<AlbumGroup>& groups) {
    bool changed = false;
    for (auto& group : groups) {
        bool groupChanged = false;
        for (auto& edition : group.editions) {
            std::vector<std::string> artists, albums, dates, releases;
            for (TrackId id : edition.tracks) {
                if (!g_lib.Contains(id)) continue;
                artists.push_back(DisplayArtist(g_lib[id]));
                albums.push_back(DisplayAlbum(g_lib[id]));
                dates.push_back(DisplayDate(g_lib[id]));
                releases.push_back(ReleaseIdForTrack(id));
            }
            std::string artist = MostCommon(artists), album = MostCommon(albums), date = MostCommon(dates);
            std::string release = MostCommon(releases);
            bool editionChanged = false;
            if (!artist.empty() && artist != edition.artist) {
                edition.artist = std::move(artist);
                editionChanged = true;
            }
            if (!album.empty() && album != edition.album) {
                edition.album = std::move(album);
                if (edition.source != EditionSource::Manual) {
                    EditionKind kind = edition.kind;
                    AlbumBaseTitle(edition.album, &kind);
                    edition.kind = kind;
                }
                editionChanged = true;
            }
            if (!date.empty() && date != edition.date) {
                edition.date = std::move(date);
                editionChanged = true;
            }
            if (!release.empty() && release != edition.musicBrainzRelease) {
                edition.musicBrainzRelease = std::move(release);
                editionChanged = true;
            }
            if (editionChanged) {
                edition.updated = UnixNow();
                groupChanged = true;
            }
        }
        FixPreferred(group);
        const AlbumEdition* preferred = EditionOf(group, PreferredEditionId(group));
        if (preferred && !preferred->album.empty()) {
            std::string title = BaseTitleDisplay(preferred->album);
            if (!title.empty() && title != group.title) { group.title = std::move(title); groupChanged = true; }
            if (!preferred->artist.empty() && preferred->artist != group.artist) {
                group.artist = preferred->artist;
                groupChanged = true;
            }
            std::vector<std::string> appGroups;
            for (TrackId id : preferred->tracks) appGroups.push_back(ReleaseGroupForTrack(id));
            std::string appGroup = MostCommon(appGroups);
            if (!appGroup.empty() && appGroup != group.musicBrainzReleaseGroup) {
                group.musicBrainzReleaseGroup = std::move(appGroup);
                groupChanged = true;
            }
        }
        if (groupChanged) {
            group.updated = UnixNow();
            changed = true;
        }
    }
    return changed;
}

bool Reconcile(std::vector<AlbumGroup>& groups, const std::vector<Candidate>& candidates, bool notify) {
    bool changed = RefreshIdentities(groups);
    std::unordered_map<TrackId, std::pair<uint32_t, uint32_t>> owner;
    for (auto& group : groups) {
        for (auto& edition : group.editions) {
            std::vector<TrackId> kept;
            kept.reserve(edition.tracks.size());
            for (TrackId id : edition.tracks) {
                // A missing file keeps its edition membership: an unplugged drive must not silently
                // dissolve the album organization. Only a record that no longer exists is dropped.
                if (!g_lib.Contains(id)) { changed = true; continue; }
                auto [it, added] = owner.try_emplace(id, std::make_pair(group.id, edition.id));
                if (!added) { changed = true; continue; }
                kept.push_back(id);
            }
            SortTracks(kept);
            if (kept != edition.tracks) { edition.tracks = std::move(kept); changed = true; }
        }
    }
    // New files that belong to an album key already in a group join that edition. One lookup per
    // candidate keeps a large library linear; exact-key duplicates are ambiguous and left alone.
    std::unordered_map<std::string, std::pair<AlbumGroup*, AlbumEdition*>> byKey;
    std::unordered_set<std::string> ambiguous;
    for (auto& group : groups)
        for (auto& edition : group.editions) {
            std::string key = AlbumKey(edition.artist, edition.album, edition.date);
            if (!byKey.emplace(std::move(key), std::make_pair(&group, &edition)).second)
                ambiguous.insert(AlbumKey(edition.artist, edition.album, edition.date));
        }
    for (const auto& candidate : candidates) {
        std::string key = AlbumKey(candidate.artist, candidate.album, candidate.date);
        if (ambiguous.count(key)) continue;
        auto found = byKey.find(key);
        if (found == byKey.end()) continue;
        AlbumGroup& group = *found->second.first;
        AlbumEdition& edition = *found->second.second;
        for (TrackId id : candidate.tracks) {
            if (owner.count(id)) continue;
            owner[id] = {group.id, edition.id};
            edition.tracks.push_back(id);
            changed = true;
        }
        SortTracks(edition.tracks);
    }
    size_t before = groups.size();
    DropEmpty(groups);
    if (groups.size() != before) changed = true;
    if (changed && notify) AlbumGroupsChanged();
    return changed;
}

std::string GroupKey(const AlbumGroup& group) {
    return NormalizeAlbumText(group.artist) + '\x1f' + NormalizeAlbumText(group.title);
}

std::string GroupKey(const Candidate& candidate) {
    return candidate.artistKey + '\x1f' + candidate.baseKey;
}

std::string PairKey(TrackId a, TrackId b) {
    if (a > b) std::swap(a, b);
    return std::to_string(a) + ":" + std::to_string(b);
}

uint32_t EditionOfTrackInGroup(const AlbumGroup& group, TrackId id) {
    for (const auto& edition : group.editions)
        if (std::find(edition.tracks.begin(), edition.tracks.end(), id) != edition.tracks.end()) return edition.id;
    return 0;
}

}  // namespace albumed

using namespace albumed;

const wchar_t* EditionKindName(EditionKind kind) {
    switch (kind) {
        case EditionKind::Original: return L"original release";
        case EditionKind::Remaster: return L"remastered";
        case EditionKind::Deluxe: return L"deluxe edition";
        case EditionKind::Expanded: return L"expanded edition";
        case EditionKind::Anniversary: return L"anniversary edition";
        case EditionKind::Reissue: return L"reissue";
        case EditionKind::Special: return L"special edition";
        case EditionKind::Bonus: return L"bonus tracks";
        case EditionKind::Regional: return L"regional edition";
        case EditionKind::Mono: return L"mono";
        case EditionKind::Stereo: return L"stereo";
        case EditionKind::AlternateMix: return L"alternate mix";
        case EditionKind::Instrumental: return L"instrumental";
        case EditionKind::Drumless: return L"drumless";
        case EditionKind::Other: return L"other edition";
        default: return L"edition";
    }
}

std::wstring EditionKindLabel(EditionKind kind) { return EditionKindName(kind); }

std::wstring EditionLabel(const AlbumEdition& edition) {
    if (!edition.label.empty()) return FromUtf8(edition.label);
    std::wstring label = EditionKindName(edition.kind);
    std::wstring date = FromUtf8(edition.date);
    if (date.size() >= 4 && iswdigit(date[0]) && iswdigit(date[1]) && iswdigit(date[2]) && iswdigit(date[3]))
        label += L" (" + date.substr(0, 4) + L")";
    return label;
}

std::string NormalizeAlbumText(std::string_view text) { return NormalizeWide(FromUtf8(text)); }

std::string AlbumBaseTitle(std::string_view album, EditionKind* kind) {
    EditionText info = SplitEditionTitle(album);
    if (kind) *kind = info.kind;
    return NormalizeAlbumText(ToUtf8(info.base));
}

uint32_t AlbumGroupsRevision() { return s_revision; }

void AlbumGroupsChanged() {
    ++s_revision;
    s_idsDirty = true;
    s_index.revision = 0;
}

void ResetAlbumGroups() {
    std::vector<AlbumGroup>().swap(g_albumGroups);
    g_trackMusicBrainz = {};
    s_index = AlbumTrackIndex{};
    s_nextGroup = s_nextEdition = 1;
    s_idsDirty = true;
    AlbumGroupsChanged();
}

const AlbumTrackIndex& TrackAlbumIndex() {
    if (s_index.revision == s_revision) return s_index;
    s_index.group.assign(g_lib.Count(), 0);
    s_index.edition.assign(g_lib.Count(), 0);
    for (const auto& group : g_albumGroups) {
        for (const auto& edition : group.editions) {
            for (TrackId id : edition.tracks) {
                if (id >= s_index.group.size() || s_index.group[id]) continue;
                s_index.group[id] = group.id;
                s_index.edition[id] = edition.id;
            }
        }
    }
    s_index.revision = s_revision;
    return s_index;
}

AlbumGroup* FindAlbumGroup(uint32_t id) {
    if (!id) return nullptr;
    return FindGroup(g_albumGroups, id);
}
const AlbumGroup* FindAlbumGroupConst(uint32_t id) {
    if (!id) return nullptr;
    return FindGroup(static_cast<const std::vector<AlbumGroup>&>(g_albumGroups), id);
}
AlbumEdition* FindAlbumEdition(AlbumGroup& group, uint32_t editionId) { return EditionOf(group, editionId); }
const AlbumEdition* FindAlbumEdition(const AlbumGroup& group, uint32_t editionId) { return EditionOf(group, editionId); }

const AlbumGroup* AlbumGroupForTrack(TrackId id) {
    const auto& index = TrackAlbumIndex();
    return id < index.group.size() ? FindAlbumGroupConst(index.group[id]) : nullptr;
}
const AlbumEdition* AlbumEditionForTrack(TrackId id) {
    const auto& index = TrackAlbumIndex();
    if (id >= index.group.size() || !index.group[id]) return nullptr;
    const AlbumGroup* group = FindAlbumGroupConst(index.group[id]);
    return group ? FindAlbumEdition(*group, index.edition[id]) : nullptr;
}

uint32_t PreferredEditionId(const AlbumGroup& group) {
    if (EditionOf(group, group.preferred)) return group.preferred;
    for (const auto& edition : group.editions)
        if (edition.kind == EditionKind::Original) return edition.id;
    return group.editions.empty() ? 0 : group.editions.front().id;
}

void SetTrackMusicBrainz(TrackId id, std::string_view releaseGroup, std::string_view release) {
    g_trackMusicBrainz.Set(id, releaseGroup, release);
}

bool AutoGroupAlbums() {
    if (g_lib.Count() == 0) return false;
    std::vector<Candidate> candidates = BuildCandidates();
    bool changed = Reconcile(g_albumGroups, candidates, false);

    // A candidate already in a group is settled. Group the rest by MusicBrainz release group
    // first, then by a conservative artist + base-title key.
    std::unordered_set<TrackId> owned;
    for (const auto& group : g_albumGroups)
        for (const auto& edition : group.editions)
            for (TrackId id : edition.tracks) owned.insert(id);
    std::vector<Candidate> free;
    for (auto& candidate : candidates) {
        // A candidate that partly belongs to an existing edition keeps only its unowned tracks;
        // including an owned one would write the same track into two editions.
        std::erase_if(candidate.tracks, [&](TrackId id) { return owned.count(id) != 0; });
        if (candidate.tracks.empty()) continue;
        free.push_back(std::move(candidate));
    }

    std::map<std::string, std::vector<size_t>> byMbid, byKey;
    for (size_t i = 0; i < free.size(); ++i) {
        if (!free[i].releaseGroup.empty()) byMbid[free[i].releaseGroup].push_back(i);
        if (!free[i].artistKey.empty() && !free[i].baseKey.empty()) byKey[GroupKey(free[i])].push_back(i);
    }
    std::vector<bool> grouped(free.size(), false);

    auto add_bucket = [&](const std::vector<size_t>& bucket, const std::string& mbid) {
        if (bucket.size() < 2) return;
        std::vector<size_t> usable;
        for (size_t index : bucket) if (!grouped[index]) usable.push_back(index);
        if (usable.size() < 2) return;
        // MusicBrainz is authoritative. Heuristics need at least one real edition marker and
        // compatible categories (live vs studio, tribute, karaoke, soundtrack, re-recording).
        if (mbid.empty()) {
            bool anyCue = false;
            for (size_t index : usable) anyCue = anyCue || free[index].cues > 0;
            if (!anyCue) return;
            for (size_t i = 1; i < usable.size(); ++i)
                if (!CompatibleCategory(free[usable[0]], free[usable[i]])) return;
            // Two copies of exactly the same title are ambiguous, not editions: dates, folders or
            // tags may differ, but without MusicBrainz or a readable edition cue leave them alone.
            std::set<std::string> titles;
            for (size_t index : usable)
                if (!titles.insert(NormalizeAlbumText(free[index].album)).second) return;
        }
        std::vector<Candidate> newCandidates;
        for (size_t index : usable) newCandidates.push_back(free[index]);
        std::string key = GroupKey(newCandidates.front());
        AlbumGroup* existing = nullptr;
        if (mbid.empty()) {
            for (auto& group : g_albumGroups)
                if (!group.manual && GroupKey(group) == key) { existing = &group; break; }
        }
        if (existing) {
            for (const auto& candidate : newCandidates)
                for (const auto& edition : existing->editions)
                    if (!CompatibleWithEdition(candidate, edition)) return;
            for (const auto& candidate : newCandidates) {
                bool mb = !mbid.empty() && candidate.releaseGroup == mbid;
                existing->editions.push_back(MakeEdition(candidate, mb ? EditionSource::MusicBrainz : EditionSource::Heuristic,
                                                         (uint8_t)(mb ? 100 : std::min(90, 55 + candidate.cues * 10))));
            }
            SortEditions(*existing);
            FixPreferred(*existing);
            existing->updated = UnixNow();
        } else {
            AddGroup(g_albumGroups, newCandidates, mbid, mbid.empty() ? "heuristic" : "MusicBrainz");
        }
        for (size_t index : usable) grouped[index] = true;
        changed = true;
    };

    for (const auto& [mbid, bucket] : byMbid) add_bucket(bucket, mbid);
    for (const auto& [key, bucket] : byKey) { (void)key; add_bucket(bucket, {}); }
    if (changed) AlbumGroupsChanged();
    return changed;
}

bool ReconcileAlbumGroups() {
    std::vector<Candidate> candidates = BuildCandidates();
    return Reconcile(g_albumGroups, candidates, true);
}

void MarkAlbumTracks(std::vector<bool>& retained) {
    for (const auto& group : g_albumGroups) {
        for (const auto& edition : group.editions)
            for (TrackId id : edition.tracks)
                if (id < retained.size()) retained[id] = true;
        for (const auto& match : group.matches) {
            if (match.a < retained.size()) retained[match.a] = true;
            if (match.b < retained.size()) retained[match.b] = true;
        }
        for (const auto& preferred : group.preferredTracks)
            if (preferred.track < retained.size()) retained[preferred.track] = true;
    }
}

void RemapAlbumGroups(const std::vector<TrackId>& mapping) {
    if (g_albumGroups.empty() && g_trackMusicBrainz.empty()) return;
    for (auto& group : g_albumGroups) {
        for (auto& edition : group.editions) {
            std::vector<TrackId> tracks;
            tracks.reserve(edition.tracks.size());
            for (TrackId id : edition.tracks)
                if (id < mapping.size() && mapping[id] != kInvalidTrack) tracks.push_back(mapping[id]);
            SortTracks(tracks);
            edition.tracks = std::move(tracks);
        }
        std::erase_if(group.matches, [&](const TrackMatch& match) {
            return match.a >= mapping.size() || match.b >= mapping.size() ||
                   mapping[match.a] == kInvalidTrack || mapping[match.b] == kInvalidTrack;
        });
        for (auto& match : group.matches) {
            match.a = mapping[match.a];
            match.b = mapping[match.b];
        }
        std::erase_if(group.preferredTracks, [&](const PreferredTrackVersion& preferred) {
            return preferred.track >= mapping.size() || mapping[preferred.track] == kInvalidTrack;
        });
        for (auto& preferred : group.preferredTracks) preferred.track = mapping[preferred.track];
        if (group.artTrack != kInvalidTrack)
            group.artTrack = group.artTrack < mapping.size() && mapping[group.artTrack] != kInvalidTrack
                                 ? mapping[group.artTrack] : kInvalidTrack;
        FixPreferred(group);
        FixArt(group);
    }
    DropEmpty(g_albumGroups);
    // Rebuilt rather than renumbered in place: the new table pools only the ids still in use.
    TrackMusicBrainzTable musicBrainz;
    g_trackMusicBrainz.ForEach([&](TrackId id, TrackMusicBrainzTable::Ids ids) {
        if (id < mapping.size() && mapping[id] != kInvalidTrack) musicBrainz.Set(mapping[id], ids.releaseGroup, ids.release);
    });
    g_trackMusicBrainz = std::move(musicBrainz);
    AlbumGroupsChanged();
}
