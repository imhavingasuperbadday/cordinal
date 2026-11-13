// Album editions edited by hand (album_editions_internal.h): grouping chosen albums, removing and splitting
// editions, the preferred edition and cover, labels, and the track-by-track comparison of two editions with
// its matches and preferred versions. Lookups for search, the edition label and MusicBrainz end the file.
#include "album_editions_internal.h"

#include "sort.h"
#include "textcase.h"

#include <algorithm>
#include <set>
#include <unordered_map>
#include <unordered_set>

using namespace albumed;

bool GroupAlbumsAsEditions(const std::vector<AlbumSelection>& albums, std::wstring* error) {
    if (error) error->clear();
    if (albums.size() < 2) {
        if (error) *error = L"Select at least two different albums first.";
        return false;
    }
    // Count distinct albums. A group tile contributes its whole group; repeated selections of the
    // same album do not make a group.
    std::set<std::string> distinct;
    uint32_t targetId = 0;
    std::vector<uint32_t> sources;
    for (const auto& selection : albums) {
        if (selection.groupId) {
            distinct.insert("g:" + std::to_string(selection.groupId));
            if (!targetId) targetId = selection.groupId;
            if (std::find(sources.begin(), sources.end(), selection.groupId) == sources.end())
                sources.push_back(selection.groupId);
        } else {
            distinct.insert("a:" + NormalizeAlbumText(selection.artist) + '\x1f' + NormalizeAlbumText(selection.album));
        }
    }
    if (distinct.size() < 2) {
        if (error) *error = L"Those selections are already the same album.";
        return false;
    }

    std::vector<AlbumGroup> work = g_albumGroups;
    AlbumGroup* target = targetId ? FindGroup(work, targetId) : nullptr;
    if (targetId && !target) {
        if (error) *error = L"That album group no longer exists.";
        return false;
    }
    uint32_t targetGroupId = targetId;
    if (!target) {
        AlbumGroup group;
        group.id = NewGroupId();
        targetGroupId = group.id;
        group.created = group.updated = UnixNow();
        group.manual = true;
        // Keep groups ordered by id for the lookup and the database order.
        auto at = std::lower_bound(work.begin(), work.end(), group.id, [](const AlbumGroup& g, uint32_t id) {
            return g.id < id;
        });
        target = &*work.insert(at, std::move(group));
    }
    target->manual = true;
    target->updated = UnixNow();

    // Merge every other selected group wholesale: selecting two group tiles means merging the two
    // groups, including editions the user did not have to click one by one.
    for (uint32_t sourceId : sources) {
        if (sourceId == targetGroupId) continue;
        target = FindGroup(work, targetGroupId);
        AlbumGroup* source = FindGroup(work, sourceId);
        if (!target || !source) continue;
        for (auto& edition : source->editions) {
            if (EditionOf(*target, edition.id)) continue;
            edition.source = EditionSource::Manual;
            target->editions.push_back(std::move(edition));
        }
        for (auto& match : source->matches) target->matches.push_back(match);
        for (auto& preferred : source->preferredTracks) target->preferredTracks.push_back(preferred);
        if (target->artTrack == kInvalidTrack && source->artTrack != kInvalidTrack) target->artTrack = source->artTrack;
        std::erase_if(work, [&](const AlbumGroup& group) { return group.id == sourceId; });
        target = FindGroup(work, targetGroupId);
        if (!target) {
            if (error) *error = L"The album group could not be merged.";
            return false;
        }
    }

    target = FindGroup(work, targetGroupId);
    if (!target) {
        if (error) *error = L"The album group could not be merged.";
        return false;
    }
    std::string canonicalTitle = target->title, canonicalArtist = target->artist;
    for (const auto& selection : albums) {
        if (selection.groupId) continue;
        std::vector<TrackId> tracks = TracksForAlbum(selection);
        std::erase_if(tracks, [&](TrackId id) { return OwnsTrack(work, id); });
        if (tracks.empty()) continue;
        AlbumEdition edition;
        edition.id = NewEditionId();
        edition.artist = selection.artist;
        edition.album = selection.album;
        edition.date = selection.date;
        EditionKind kind = EditionKind::Original;
        AlbumBaseTitle(selection.album, &kind);
        edition.kind = kind;
        edition.source = EditionSource::Manual;
        edition.confidence = 100;
        edition.created = edition.updated = UnixNow();
        edition.musicBrainzRelease = ReleaseIdForTrack(tracks.front());
        edition.tracks = std::move(tracks);
        SortTracks(edition.tracks);
        if (canonicalTitle.empty()) canonicalTitle = BaseTitleDisplay(selection.album);
        if (canonicalArtist.empty()) canonicalArtist = selection.artist;
        target->editions.push_back(std::move(edition));
    }
    if (canonicalTitle.empty() && target->editions.empty()) {
        if (error) *error = L"No tracks were available to group.";
        return false;
    }
    if (!canonicalTitle.empty()) target->title = canonicalTitle;
    if (!canonicalArtist.empty()) target->artist = canonicalArtist;
    // A manual group remembers the artist as its album artist; use the source when the selection
    // came from ordinary albums without an album-artist tag.
    if (target->artist.empty())
        for (const auto& selection : albums) if (!selection.artist.empty()) { target->artist = selection.artist; break; }
    // Preferred: keep the group's choice; a new group starts on the plain release, else the first.
    SortEditions(*target);
    if (!EditionOf(*target, target->preferred)) FixPreferred(*target);
    target->updated = UnixNow();
    DropEmpty(work);
    target = FindGroup(work, targetGroupId);
    if (!target || target->editions.size() < 2) {
        if (error) *error = L"At least two albums with tracks are needed to make a group.";
        return false;
    }
    // Deduplicate preferred track keys and matches (merges can bring the same pair twice).
    for (auto& group : work) {
        std::sort(group.matches.begin(), group.matches.end(), [](const TrackMatch& a, const TrackMatch& b) {
            return PairKey(a.a, a.b) < PairKey(b.a, b.b);
        });
        group.matches.erase(std::unique(group.matches.begin(), group.matches.end(), [](const TrackMatch& a, const TrackMatch& b) {
            return PairKey(a.a, a.b) == PairKey(b.a, b.b);
        }), group.matches.end());
        std::sort(group.preferredTracks.begin(), group.preferredTracks.end(),
                  [](const PreferredTrackVersion& a, const PreferredTrackVersion& b) { return a.key < b.key; });
        group.preferredTracks.erase(
            std::unique(group.preferredTracks.begin(), group.preferredTracks.end(),
                        [](const PreferredTrackVersion& a, const PreferredTrackVersion& b) { return a.key == b.key; }),
            group.preferredTracks.end());
    }
    g_albumGroups = std::move(work);
    AlbumGroupsChanged();
    return true;
}

bool RemoveEditionFromGroup(uint32_t groupId, uint32_t editionId) {
    std::vector<AlbumGroup> work = g_albumGroups;
    AlbumGroup* group = FindGroup(work, groupId);
    if (!group || !EditionOf(*group, editionId) || group->editions.size() <= 1) return false;
    AlbumEdition moved;
    for (auto it = group->editions.begin(); it != group->editions.end(); ++it)
        if (it->id == editionId) {
            moved = std::move(*it);
            group->editions.erase(it);
            break;
        }
    moved.source = EditionSource::Manual;
    moved.updated = UnixNow();
    std::erase_if(group->matches, [&](const TrackMatch& match) {
        return std::find(moved.tracks.begin(), moved.tracks.end(), match.a) != moved.tracks.end() ||
               std::find(moved.tracks.begin(), moved.tracks.end(), match.b) != moved.tracks.end();
    });
    std::erase_if(group->preferredTracks, [&](const PreferredTrackVersion& preferred) {
        return std::find(moved.tracks.begin(), moved.tracks.end(), preferred.track) != moved.tracks.end();
    });
    FixPreferred(*group);
    group->updated = UnixNow();
    AlbumGroup single;
    single.id = NewGroupId();
    single.title = moved.album.empty() ? group->title : BaseTitleDisplay(moved.album);
    single.artist = moved.artist;
    single.musicBrainzReleaseGroup.clear();
    single.preferred = moved.id;
    single.manual = true;
    single.created = single.updated = UnixNow();
    if (group->artTrack != kInvalidTrack &&
        std::find(moved.tracks.begin(), moved.tracks.end(), group->artTrack) != moved.tracks.end()) {
        single.artTrack = group->artTrack;
        group->artTrack = kInvalidTrack;
    }
    single.editions.push_back(std::move(moved));
    FixArt(*group);
    auto at = std::lower_bound(work.begin(), work.end(), single.id, [](const AlbumGroup& g, uint32_t id) {
        return g.id < id;
    });
    work.insert(at, std::move(single));
    DropEmpty(work);
    g_albumGroups = std::move(work);
    AlbumGroupsChanged();
    return true;
}

bool SplitAlbumGroup(uint32_t groupId) {
    std::vector<AlbumGroup> work = g_albumGroups;
    AlbumGroup* group = FindGroup(work, groupId);
    if (!group || group->editions.size() < 2) return false;
    std::vector<AlbumGroup> singles;
    for (auto& edition : group->editions) {
        AlbumGroup single;
        single.id = NewGroupId();
        single.title = edition.album.empty() ? group->title : BaseTitleDisplay(edition.album);
        single.artist = edition.artist;
        single.preferred = edition.id;
        single.manual = true;
        single.created = single.updated = UnixNow();
        if (group->artTrack != kInvalidTrack &&
            std::find(edition.tracks.begin(), edition.tracks.end(), group->artTrack) != edition.tracks.end())
            single.artTrack = group->artTrack;
        edition.source = EditionSource::Manual;
        edition.updated = UnixNow();
        single.editions.push_back(std::move(edition));
        singles.push_back(std::move(single));
    }
    std::erase_if(work, [&](const AlbumGroup& g) { return g.id == groupId; });
    for (auto& single : singles) {
        auto at = std::lower_bound(work.begin(), work.end(), single.id, [](const AlbumGroup& g, uint32_t id) {
            return g.id < id;
        });
        work.insert(at, std::move(single));
    }
    g_albumGroups = std::move(work);
    AlbumGroupsChanged();
    return true;
}

bool SetPreferredEdition(uint32_t groupId, uint32_t editionId) {
    AlbumGroup* group = FindAlbumGroup(groupId);
    if (!group || !EditionOf(*group, editionId)) return false;
    group->preferred = editionId;
    group->manual = true;
    group->updated = UnixNow();
    AlbumGroupsChanged();
    return true;
}

bool SetAlbumGroupArt(uint32_t groupId, TrackId track) {
    AlbumGroup* group = FindAlbumGroup(groupId);
    if (!group || !g_lib.Contains(track) || !EditionOfTrackInGroup(*group, track)) return false;
    group->artTrack = track;
    group->updated = UnixNow();
    AlbumGroupsChanged();
    return true;
}

bool ClearAlbumGroupArt(uint32_t groupId) {
    AlbumGroup* group = FindAlbumGroup(groupId);
    if (!group || group->artTrack == kInvalidTrack) return false;
    group->artTrack = kInvalidTrack;
    group->updated = UnixNow();
    AlbumGroupsChanged();
    return true;
}

bool SetEditionLabel(uint32_t groupId, uint32_t editionId, std::wstring_view label) {
    AlbumGroup* group = FindAlbumGroup(groupId);
    AlbumEdition* edition = group ? EditionOf(*group, editionId) : nullptr;
    if (!edition) return false;
    std::wstring text(label);
    size_t first = text.find_first_not_of(L" \t\r\n");
    size_t last = text.find_last_not_of(L" \t\r\n");
    text = first == std::wstring::npos ? std::wstring{} : text.substr(first, last - first + 1);
    if (text.size() > 200) text.resize(200);
    edition->label = ToUtf8(text);
    edition->source = EditionSource::Manual;
    edition->updated = UnixNow();
    group->manual = true;
    group->updated = UnixNow();
    AlbumGroupsChanged();
    return true;
}

bool AllowAutomaticGrouping(uint32_t groupId) {
    std::vector<AlbumGroup> work = g_albumGroups;
    AlbumGroup* group = FindGroup(work, groupId);
    if (!group) return false;
    if (group->editions.size() <= 1) {
        std::erase_if(work, [&](const AlbumGroup& g) { return g.id == groupId; });
        g_albumGroups = std::move(work);
        AlbumGroupsChanged();
        AutoGroupAlbums();
        return true;
    }
    group->manual = false;
    group->updated = UnixNow();
    g_albumGroups = std::move(work);
    AlbumGroupsChanged();
    return true;
}

std::vector<EditionComparisonRow> CompareEditions(uint32_t groupId, uint32_t editionA, uint32_t editionB) {
    std::vector<EditionComparisonRow> rows;
    const AlbumGroup* group = FindAlbumGroupConst(groupId);
    if (!group || editionA == editionB) return rows;
    const AlbumEdition* a = FindAlbumEdition(*group, editionA);
    const AlbumEdition* b = FindAlbumEdition(*group, editionB);
    if (!a || !b) return rows;
    std::vector<EditionComparisonRow> out;
    std::unordered_map<TrackId, const TrackMatch*> confirmed;
    std::unordered_set<std::string> rejected;
    for (const auto& match : group->matches) {
        if (match.status == MatchStatus::Confirmed) {
            confirmed[match.a] = &match;
            confirmed[match.b] = &match;
        } else if (match.status == MatchStatus::Rejected) {
            rejected.insert(PairKey(match.a, match.b));
        }
    }
    std::unordered_set<TrackId> usedB;
    std::vector<std::pair<TrackId, const Track*>> order;
    for (TrackId id : a->tracks) order.emplace_back(id, g_lib.Contains(id) ? &g_lib[id] : nullptr);
    for (size_t i = 0; i < order.size(); ++i) {
        TrackId id = order[i].first;
        EditionComparisonRow row;
        row.a = id;
        if (auto it = confirmed.find(id); it != confirmed.end()) {
            const TrackMatch& match = *it->second;
            TrackId other = match.a == id ? match.b : match.a;
            if (std::find(b->tracks.begin(), b->tracks.end(), other) != b->tracks.end()) {
                row.b = other;
                row.relation = match.relation;
                row.status = MatchStatus::Confirmed;
                row.confidence = 100;
                usedB.insert(other);
                out.push_back(row);
                continue;
            }
        }
        std::string title = NormalizedTitle(id);
        if (title.empty()) { out.push_back(row); continue; }
        int best = -1, bestScore = 0, second = 0;
        for (size_t k = 0; k < b->tracks.size(); ++k) {
            TrackId other = b->tracks[k];
            if (usedB.count(other) || rejected.count(PairKey(id, other))) continue;
            if (NormalizedTitle(other) != title) continue;
            const Track& x = g_lib[id];
            const Track& y = g_lib[other];
            int score = 20;
            if (x.trackNo && x.trackNo == y.trackNo) score += 30;
            if (x.disc && x.disc == y.disc) score += 5;
            long long diff = std::llabs((long long)x.durationMs - (long long)y.durationMs);
            if (diff <= 1500) score += 30;
            else if (diff <= 5000) score += 10;
            if (score > bestScore) {
                second = bestScore;
                bestScore = score;
                best = (int)k;
            } else if (score > second) second = score;
        }
        // Two equally plausible performances stay unknown rather than becoming a false match.
        if (best < 0 || (bestScore < 30 && b->tracks.size() > 1) || bestScore == second) {
            out.push_back(row);
            continue;
        }
        TrackId other = b->tracks[(size_t)best];
        row.b = other;
        long long diff = std::llabs((long long)g_lib[id].durationMs - (long long)g_lib[other].durationMs);
        if (a->kind == EditionKind::Instrumental || a->kind == EditionKind::Drumless ||
            b->kind == EditionKind::Instrumental || b->kind == EditionKind::Drumless ||
            a->kind == EditionKind::AlternateMix || b->kind == EditionKind::AlternateMix)
            row.relation = TrackRelation::AlternateMix;
        else if (diff > 5000) row.relation = TrackRelation::DifferentMaster;
        else row.relation = TrackRelation::SameRecording;
        row.status = MatchStatus::Suggested;
        row.confidence = (uint8_t)std::min(95, bestScore + 35);
        usedB.insert(other);
        out.push_back(row);
    }
    for (TrackId id : b->tracks) {
        if (usedB.count(id)) continue;
        EditionComparisonRow row;
        row.b = id;
        out.push_back(row);
    }
    // Keep A in album order and B-only rows after the paired list, matching the editor's reading.
    auto rank = [&](TrackId id) {
        auto it = std::find(a->tracks.begin(), a->tracks.end(), id);
        return it == a->tracks.end() ? (size_t)a->tracks.size() + 1 : (size_t)(it - a->tracks.begin());
    };
    std::stable_sort(out.begin(), out.end(), [&](const EditionComparisonRow& x, const EditionComparisonRow& y) {
        if (x.a != kInvalidTrack && y.a != kInvalidTrack) return rank(x.a) < rank(y.a);
        if (x.a != kInvalidTrack) return true;
        if (y.a != kInvalidTrack) return false;
        return rank(x.b) < rank(y.b);
    });
    return out;
}

std::vector<TrackVersion> OtherVersions(uint32_t groupId, TrackId track) {
    std::vector<TrackVersion> versions;
    const AlbumGroup* group = FindAlbumGroupConst(groupId);
    if (!group || !g_lib.Contains(track)) return versions;
    uint32_t own = EditionOfTrackInGroup(*group, track);
    if (!own) return versions;
    std::unordered_set<TrackId> seen{track};
    auto add = [&](TrackId id, uint32_t edition) {
        if (id == kInvalidTrack || id >= g_lib.Count() || edition == own || !seen.insert(id).second) return;
        if (!g_lib.Contains(id)) return;
        versions.push_back({id, edition});
    };
    for (const auto& match : group->matches)
        if (match.status == MatchStatus::Confirmed) {
            if (match.a == track) add(match.b, EditionOfTrackInGroup(*group, match.b));
            else if (match.b == track) add(match.a, EditionOfTrackInGroup(*group, match.a));
        }
    std::string title = NormalizedTitle(track);
    if (!title.empty())
        for (const auto& edition : group->editions) {
            if (edition.id == own) continue;
            for (TrackId id : edition.tracks)
                if (NormalizedTitle(id) == title) add(id, edition.id);
        }
    std::sort(versions.begin(), versions.end(), [&](const TrackVersion& a, const TrackVersion& b) {
        if (a.edition != b.edition) return a.edition < b.edition;
        return TrackOrderLess(a.track, b.track);
    });
    return versions;
}

bool SetTrackMatch(uint32_t groupId, TrackId a, TrackId b, MatchStatus status, TrackRelation relation) {
    AlbumGroup* group = FindAlbumGroup(groupId);
    if (!group || a == b || !g_lib.Contains(a) || !g_lib.Contains(b)) return false;
    uint32_t editionA = EditionOfTrackInGroup(*group, a), editionB = EditionOfTrackInGroup(*group, b);
    if (!editionA || !editionB || editionA == editionB) return false;
    std::erase_if(group->matches, [&](const TrackMatch& match) {
        return (match.a == a && match.b == b) || (match.a == b && match.b == a);
    });
    TrackMatch match;
    match.a = a;
    match.b = b;
    match.status = status;
    match.relation = relation;
    match.confidence = status == MatchStatus::Confirmed ? 100 : status == MatchStatus::Rejected ? 0 : 50;
    group->matches.push_back(match);
    group->manual = true;
    group->updated = UnixNow();
    AlbumGroupsChanged();
    return true;
}

bool SetPreferredTrackVersion(uint32_t groupId, TrackId track) {
    AlbumGroup* group = FindAlbumGroup(groupId);
    if (!group || !g_lib.Contains(track) || !EditionOfTrackInGroup(*group, track)) return false;
    std::string key = NormalizedTitle(track);
    if (key.empty()) return false;
    std::erase_if(group->preferredTracks, [&](const PreferredTrackVersion& preferred) { return preferred.key == key; });
    group->preferredTracks.push_back({std::move(key), track});
    group->manual = true;
    group->updated = UnixNow();
    AlbumGroupsChanged();
    return true;
}

bool ClearPreferredTrackVersion(uint32_t groupId, TrackId track) {
    AlbumGroup* group = FindAlbumGroup(groupId);
    if (!group || !g_lib.Contains(track)) return false;
    std::string key = NormalizedTitle(track);
    size_t before = group->preferredTracks.size();
    std::erase_if(group->preferredTracks, [&](const PreferredTrackVersion& preferred) {
        return preferred.key == key || preferred.track == track;
    });
    if (group->preferredTracks.size() == before) return false;
    group->manual = true;
    group->updated = UnixNow();
    AlbumGroupsChanged();
    return true;
}

std::wstring AlbumEditionSearchText(TrackId id) {
    const AlbumEdition* edition = AlbumEditionForTrack(id);
    if (!edition) return {};
    const AlbumGroup* group = AlbumGroupForTrack(id);
    std::wstring text;
    if (group) {
        text += FromUtf8(group->title) + L" " + FromUtf8(group->artist);
        if (!group->musicBrainzReleaseGroup.empty()) text += L" " + FromUtf8(group->musicBrainzReleaseGroup);
    }
    text += L" " + FromUtf8(edition->album) + L" " + EditionLabel(*edition) + L" " + EditionKindName(edition->kind);
    if (!edition->musicBrainzRelease.empty()) text += L" " + FromUtf8(edition->musicBrainzRelease);
    return text;
}

std::wstring TrackEditionLabel(TrackId id) {
    const AlbumEdition* edition = AlbumEditionForTrack(id);
    return edition ? EditionLabel(*edition) : std::wstring{};
}

std::vector<AlbumLookupCandidate> AlbumLookupCandidates() {
    std::vector<Candidate> candidates = BuildCandidates();
    struct Bucket {
        std::string artist, title;
        size_t copies = 0;
        bool missing = false;
    };
    std::map<std::string, Bucket> buckets;
    for (const auto& candidate : candidates) {
        if (candidate.artistKey.empty() || candidate.baseKey.empty()) continue;
        std::string key = GroupKey(candidate);
        Bucket& bucket = buckets[key];
        bucket.artist = candidate.artist;
        bucket.title = candidate.displayBase;
        ++bucket.copies;
        for (TrackId id : candidate.tracks) bucket.missing = bucket.missing || ReleaseGroupForTrack(id).empty();
    }
    std::vector<AlbumLookupCandidate> result;
    for (const auto& [key, bucket] : buckets) {
        (void)key;
        if (bucket.copies >= 2 && bucket.missing && !bucket.title.empty() && !bucket.artist.empty())
            result.push_back({bucket.artist, bucket.title});
    }
    return result;
}

bool ApplyMusicBrainzReleaseGroup(std::string_view artist, std::string_view title, std::string_view releaseGroup) {
    if (releaseGroup.empty()) return false;
    std::string artistKey = NormalizeAlbumText(artist), titleKey = NormalizeAlbumText(title);
    if (artistKey.empty() || titleKey.empty()) return false;
    bool changed = false;
    std::vector<Candidate> candidates = BuildCandidates();
    for (const auto& candidate : candidates) {
        if (candidate.artistKey != artistKey || candidate.baseKey != titleKey) continue;
        for (TrackId id : candidate.tracks) {
            if (!ReleaseGroupForTrack(id).empty()) continue;  // a tag-supplied id is authoritative
            SetTrackMusicBrainz(id, releaseGroup, ReleaseIdForTrack(id));
            changed = true;
        }
    }
    return changed;
}
