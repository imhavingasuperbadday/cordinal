// Album groups in library.cdb (album_editions_internal.h): the versioned sections that save groups,
// editions, track matches, chosen art and per-track MusicBrainz ids, and their careful reading back.
#include "album_editions_internal.h"

#include "sections.h"
#include "sort.h"

#include <algorithm>
#include <limits>
#include <set>
#include <unordered_map>
#include <unordered_set>

using namespace albumed;

namespace {

void PutString(ByteWriter& w, std::string_view text) {
    std::string clean(text);
    std::replace(clean.begin(), clean.end(), '\0', ' ');
    std::string_view view = clean;
    if (view.size() > kMaxField) {
        size_t length = kMaxField;
        while (length && ((unsigned char)view[length] & 0xC0) == 0x80) --length;
        view = view.substr(0, length);
    }
    w.Bytes(view);
}

bool ValidText(std::string_view text) {
    if (text.size() > kMaxField) return false;
    if (text.find('\0') != std::string_view::npos) return false;
    if (text.empty()) return true;
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), (int)text.size(), nullptr, 0) > 0;
}

bool ReadString(ByteReader& r, std::string& out) {
    std::string_view text = r.Bytes(kMaxField);
    if (!r.ok || !ValidText(text)) return false;
    out.assign(text);
    return true;
}

bool ReadId(ByteReader& r, uint32_t& id) {
    id = r.Unsigned<uint32_t>();
    return r.ok;
}

bool ReadTrackIndex(ByteReader& r, uint64_t trackCount, uint64_t& index) {
    index += r.U(trackCount);
    return r.ok && index < trackCount;
}

TrackId LiveId(uint64_t savedIndex, const std::vector<TrackId>* savedToId) {
    if (!savedToId) return (TrackId)savedIndex;
    return savedIndex < savedToId->size() ? (*savedToId)[(size_t)savedIndex] : kInvalidTrack;
}

void PutTrackGaps(ByteWriter& w, const std::vector<uint32_t>& indices) {
    w.U(indices.size());
    uint32_t previous = 0;
    for (uint32_t index : indices) {
        w.U(index - previous);
        previous = index;
    }
}

}  // namespace

std::string EncodeAlbumArt(const std::vector<AlbumGroup>& groups, const std::vector<uint32_t>& newId) {
    std::string out;
    ByteWriter w(out);
    w.U(1);
    std::vector<std::pair<uint32_t, uint32_t>> items;
    for (const auto& group : groups)
        if (group.artTrack != kInvalidTrack && group.artTrack < newId.size() && newId[group.artTrack] != UINT32_MAX)
            items.emplace_back(group.id, newId[group.artTrack]);
    w.U(items.size());
    for (const auto& [group, track] : items) {
        w.U(group);
        w.U(track);
    }
    return out;
}

bool DecodeAlbumArt(std::string_view data, uint64_t trackCount, const std::vector<TrackId>* savedToId,
                    std::vector<AlbumGroup>* groups) {
    ByteReader r(data);
    if (r.U(1) != 1) return false;
    uint64_t count = r.U(std::min<uint64_t>(1u << 20, data.size() + 1));
    std::unordered_set<uint32_t> seenGroups;
    for (uint64_t i = 0; i < count && r.ok; ++i) {
        uint32_t groupId = r.Unsigned<uint32_t>();
        uint64_t index = r.U(trackCount);
        if (!r.ok || index >= trackCount || !groupId || !seenGroups.insert(groupId).second) return false;
        if (!groups) continue;
        AlbumGroup* group = FindGroup(*groups, groupId);
        if (!group) return false;
        TrackId id = LiveId(index, savedToId);
        if (id == kInvalidTrack || !EditionOfTrackInGroup(*group, id)) return false;
        group->artTrack = id;
    }
    return r.ok && r.Done();
}

std::string EncodeAlbumDatabase(const std::vector<AlbumGroup>& groups,
                                const TrackMusicBrainzTable& musicBrainz,
                                const std::vector<uint32_t>& newId) {
    std::string out;
    ByteWriter w(out);
    w.U(1);  // section format
    std::vector<const AlbumGroup*> kept;
    for (const auto& group : groups) {
        if (!group.id || group.editions.empty()) continue;
        bool any = false;
        for (const auto& edition : group.editions)
            for (TrackId id : edition.tracks)
                if (id < newId.size() && newId[id] != UINT32_MAX) any = true;
        if (any) kept.push_back(&group);
    }
    w.U(kept.size());
    std::set<uint32_t> usedTracks;  // one track belongs to one edition; never write an invalid duplicate
    for (const AlbumGroup* group : kept) {
        w.U(group->id);
        PutString(w, group->title);
        PutString(w, group->artist);
        PutString(w, group->musicBrainzReleaseGroup);
        // A preferred edition whose tracks all vanished cannot be named in the saved group. Fall
        // back to a packed edition so a damaged record never makes the whole library unreadable.
        uint32_t preferredEdition = group->preferred;
        bool preferredKept = false;
        for (const auto& edition : group->editions) {
            if (edition.id != preferredEdition) continue;
            for (TrackId id : edition.tracks)
                if (id < newId.size() && newId[id] != UINT32_MAX) { preferredKept = true; break; }
        }
        if (!preferredKept) {
            preferredEdition = 0;
            for (const auto& edition : group->editions) {
                bool keptEdition = false;
                for (TrackId id : edition.tracks)
                    if (id < newId.size() && newId[id] != UINT32_MAX) { keptEdition = true; break; }
                if (keptEdition) { preferredEdition = edition.id; break; }
            }
        }
        w.U(preferredEdition);
        w.U(group->manual ? 1 : 0);
        w.U(group->created);
        w.U(group->updated);
        struct PackedEdition {
            const AlbumEdition* edition;
            std::vector<uint32_t> tracks;
        };
        std::vector<PackedEdition> editions;
        for (const auto& edition : group->editions) {
            PackedEdition packed{&edition, {}};
            for (TrackId id : edition.tracks) {
                if (id >= newId.size() || newId[id] == UINT32_MAX || !usedTracks.insert(newId[id]).second) continue;
                packed.tracks.push_back(newId[id]);
            }
            SortRecords(packed.tracks.begin(), packed.tracks.end());
            packed.tracks.erase(std::unique(packed.tracks.begin(), packed.tracks.end()), packed.tracks.end());
            if (!packed.tracks.empty()) editions.push_back(std::move(packed));
        }
        w.U(editions.size());
        for (const auto& packed : editions) {
            const AlbumEdition& edition = *packed.edition;
            w.U(edition.id);
            PutString(w, edition.artist);
            PutString(w, edition.album);
            PutString(w, edition.date);
            PutString(w, edition.label);
            w.U((uint64_t)edition.kind);
            w.U((uint64_t)edition.source);
            PutString(w, edition.musicBrainzRelease);
            w.U(edition.confidence);
            w.U(edition.created);
            w.U(edition.updated);
            PutTrackGaps(w, packed.tracks);
        }
        std::vector<TrackMatch> matches;
        for (const auto& match : group->matches) {
            if (match.a >= newId.size() || match.b >= newId.size() || newId[match.a] == UINT32_MAX ||
                newId[match.b] == UINT32_MAX) continue;
            TrackMatch packed = match;
            packed.a = newId[match.a];
            packed.b = newId[match.b];
            matches.push_back(packed);
        }
        w.U(matches.size());
        for (const auto& match : matches) {
            w.U(match.a);
            w.U(match.b);
            w.U((uint64_t)match.relation);
            w.U((uint64_t)match.status);
            w.U(match.confidence);
        }
        std::vector<PreferredTrackVersion> preferredVersions;
        for (const auto& item : group->preferredTracks)
            if (item.track < newId.size() && newId[item.track] != UINT32_MAX)
                preferredVersions.push_back({item.key, newId[item.track]});
        w.U(preferredVersions.size());
        for (const auto& item : preferredVersions) {
            PutString(w, item.key);
            w.U(item.track);
        }
    }
    std::vector<std::pair<uint32_t, TrackMusicBrainzTable::Ids>> mb;
    musicBrainz.ForEach([&](TrackId id, TrackMusicBrainzTable::Ids ids) {
        if (id < newId.size() && newId[id] != UINT32_MAX) mb.emplace_back(newId[id], ids);
    });
    std::sort(mb.begin(), mb.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    w.U(mb.size());
    uint32_t previous = 0;
    for (const auto& [index, ids] : mb) {
        w.U(index - previous);
        previous = index;
        PutString(w, ids.releaseGroup);
        PutString(w, ids.release);
    }
    return out;
}

bool DecodeAlbumDatabase(std::string_view data, uint64_t trackCount, const std::vector<TrackId>* savedToId,
                         std::vector<AlbumGroup>* groups, TrackMusicBrainzTable* musicBrainz) {
    ByteReader r(data);
    if (r.U(1) != 1) return false;
    uint64_t groupCount = r.U(std::min<uint64_t>(1u << 20, data.size() + 1));
    std::vector<AlbumGroup> loaded;
    loaded.reserve((size_t)groupCount);
    std::unordered_set<uint32_t> groupIds, editionIds;
    std::vector<uint8_t> owned((size_t)trackCount, 0);
    for (uint64_t i = 0; i < groupCount && r.ok; ++i) {
        AlbumGroup group;
        if (!ReadId(r, group.id) || !group.id || !groupIds.insert(group.id).second) return false;
        if (!ReadString(r, group.title) || !ReadString(r, group.artist) ||
            !ReadString(r, group.musicBrainzReleaseGroup)) return false;
        if (!ReadId(r, group.preferred)) return false;
        uint64_t manual = r.U(1);
        if (!r.ok || manual > 1) return false;
        group.manual = manual != 0;
        group.created = r.Unsigned<uint32_t>();
        group.updated = r.Unsigned<uint32_t>();
        uint64_t editionCount = r.U(std::min<uint64_t>(1u << 20, data.size() + 1));
        for (uint64_t e = 0; e < editionCount && r.ok; ++e) {
            AlbumEdition edition;
            if (!ReadId(r, edition.id) || !edition.id || !editionIds.insert(edition.id).second) return false;
            if (!ReadString(r, edition.artist) || !ReadString(r, edition.album) || !ReadString(r, edition.date) ||
                !ReadString(r, edition.label)) return false;
            edition.kind = (EditionKind)r.U((uint64_t)EditionKind::Count - 1);
            edition.source = (EditionSource)r.U((uint64_t)EditionSource::Count - 1);
            if (!ReadString(r, edition.musicBrainzRelease)) return false;
            edition.confidence = r.Unsigned<uint8_t>();
            edition.created = r.Unsigned<uint32_t>();
            edition.updated = r.Unsigned<uint32_t>();
            uint64_t trackTotal = r.U(trackCount);
            uint64_t index = 0;
            for (uint64_t t = 0; t < trackTotal && r.ok; ++t) {
                if (!ReadTrackIndex(r, trackCount, index)) return false;
                if (owned[(size_t)index]) return false;
                owned[(size_t)index] = 1;
                TrackId id = LiveId(index, savedToId);
                if (id == kInvalidTrack) return false;
                edition.tracks.push_back(id);
            }
            // The encoder writes tracks in saved-index order, so no live-library lookup is needed
            // here: during validation g_lib still holds the previous (or an empty) library.
            if (edition.tracks.empty()) return false;
            group.editions.push_back(std::move(edition));
        }
        if (group.editions.empty()) return false;
        uint64_t matchCount = r.U(trackCount * trackCount);
        for (uint64_t m = 0; m < matchCount && r.ok; ++m) {
            TrackMatch match;
            if (!ReadId(r, match.a) || !ReadId(r, match.b) || match.a == match.b || match.a >= trackCount ||
                match.b >= trackCount) return false;
            match.relation = (TrackRelation)r.U((uint64_t)TrackRelation::Count - 1);
            match.status = (MatchStatus)r.U((uint64_t)MatchStatus::Count - 1);
            match.confidence = r.Unsigned<uint8_t>();
            match.a = LiveId(match.a, savedToId);
            match.b = LiveId(match.b, savedToId);
            if (match.a == kInvalidTrack || match.b == kInvalidTrack) return false;
            group.matches.push_back(match);
        }
        uint64_t preferredCount = r.U(trackCount);
        for (uint64_t p = 0; p < preferredCount && r.ok; ++p) {
            PreferredTrackVersion preferred;
            if (!ReadString(r, preferred.key) || preferred.key.empty()) return false;
            uint64_t index = r.U(trackCount);
            if (!r.ok || index >= trackCount) return false;
            preferred.track = LiveId(index, savedToId);
            if (preferred.track == kInvalidTrack) return false;
            group.preferredTracks.push_back(std::move(preferred));
        }
        if (group.preferred && !EditionOf(group, group.preferred)) return false;
        if (!group.preferred) FixPreferred(group);
        SortEditions(group);
        loaded.push_back(std::move(group));
    }
    TrackMusicBrainzTable loadedMb;
    uint64_t mbCount = r.U(trackCount);
    uint64_t index = 0;
    std::string releaseGroup, release;
    for (uint64_t i = 0; i < mbCount && r.ok; ++i) {
        if (!ReadTrackIndex(r, trackCount, index)) return false;
        if (!ReadString(r, releaseGroup) || !ReadString(r, release)) return false;
        TrackId id = LiveId(index, savedToId);
        if (id == kInvalidTrack) return false;
        loadedMb.Set(id, releaseGroup, release);
    }
    if (!r.ok || !r.Done()) return false;
    std::sort(loaded.begin(), loaded.end(), [](const AlbumGroup& a, const AlbumGroup& b) { return a.id < b.id; });
    if (groups) *groups = std::move(loaded);
    if (musicBrainz) *musicBrainz = std::move(loadedMb);
    return true;
}
