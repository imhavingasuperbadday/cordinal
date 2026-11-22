// Find duplicates: grouping copies of one song, choosing the copy to keep, and carrying the history of
// removed copies over to it (duplicates.h). Portable: no platform calls, so both front ends and the
// tests share every rule here.
#include "duplicates.h"

#include "playlog.h"
#include "sort.h"
#include "track_set.h"

#include <algorithm>
#include <tuple>

namespace duplicates {

namespace {

bool Lossless(Codec c) {
    return c == Codec::FLAC || c == Codec::ALAC || c == Codec::PCM || c == Codec::Cord || c == Codec::DSD;
}

int TagCount(const Track& t) {
    return !!t.title + !!t.artist + !!t.album + !!t.albumArtist + !!t.date + !!t.genre + !!t.trackNo;
}

// Artist and title as the same-song key: ASCII case and runs of spaces don't matter.
std::string Folded(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == ' ' || c == '\t') {
            if (!out.empty() && out.back() != ' ') out += ' ';
            continue;
        }
        out += c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

TrackId Find(const Library& library, const std::wstring& path) { return path.empty() ? kInvalidTrack : library.Find(path); }

uint32_t Earliest(uint32_t a, uint32_t b) { return !a ? b : !b ? a : std::min(a, b); }

History Of(const Track& t) { return {t.playCount, t.skipCount, t.lastPlayed, t.firstPlayed, t.added, t.rating, t.flags}; }

void Restore(Track& t, const History& h) {
    t.playCount = h.playCount;
    t.skipCount = h.skipCount;
    t.lastPlayed = h.lastPlayed;
    t.firstPlayed = h.firstPlayed;
    t.added = h.added;
    t.rating = h.rating;
}

}  // namespace

bool Eligible(const Library& library, TrackId id) {
    if (!library.Contains(id)) return false;
    const Track& t = library[id];
    return (t.flags & (Track::kInLibrary | Track::kInbox)) && !(t.flags & (Track::kMissing | Track::kExcluded)) &&
           library.View(t.file).find('|') == std::string_view::npos;
}

std::vector<TrackId> Unmeasured(const Library& library) {
    std::vector<TrackId> ids;
    for (TrackId id = 0; id < (TrackId)library.Count(); ++id) {
        const Track& t = library[id];
        if (Eligible(library, id) && t.Loaded() && !t.audioHash && !(t.flags & (Track::kFailed | Track::kRgScanFailed)))
            ids.push_back(id);
    }
    return ids;
}

bool Better(const Library& library, TrackId a, TrackId b) {
    auto key = [&](TrackId id) {
        const Track& t = library[id];
        const bool lossless = Lossless(t.codec);
        // A WAV holds the same sound as a FLAC but hardly any tags, so it comes after it.
        return std::make_tuple(lossless, lossless && t.codec != Codec::PCM, lossless ? t.sampleRate : (uint32_t)t.bitrate,
                               TagCount(t), t.playCount + t.skipCount, !(t.flags & Track::kInbox), UINT32_MAX - id);
    };
    return key(a) > key(b);
}

std::wstring FormatText(const Track& t) {
    std::wstring text = CodecName(t.codec);
    wchar_t detail[48] = L"";
    if (Lossless(t.codec) && t.sampleRate)
        swprintf(detail, 48, t.sampleRate % 1000 ? L"%.1f kHz" : L"%.0f kHz", t.sampleRate / 1000.0);
    else if (t.bitrate)
        swprintf(detail, 48, L"%u kb/s", (unsigned)t.bitrate);
    if (*detail) text += (text.empty() ? L"" : L", ") + std::wstring(detail);
    return text;
}

std::vector<Group> Find(const Library& library) {
    std::vector<Group> groups;
    std::vector<std::pair<uint64_t, TrackId>> byHash;
    for (TrackId id = 0; id < (TrackId)library.Count(); ++id)
        if (library[id].audioHash && Eligible(library, id)) byHash.emplace_back(library[id].audioHash, id);
    auto better = [&](TrackId a, TrackId b) { return Better(library, a, b); };
    TrackIdSet covered;  // identical copies other than their group's best
    for (auto& tracks : GroupDuplicateAudio(std::move(byHash))) {
        SortObjects(tracks.begin(), tracks.end(), better);
        for (size_t i = 1; i < tracks.size(); ++i) covered.insert(tracks[i]);
        groups.push_back({true, std::move(tracks)});
    }
    struct Candidate {
        std::string key;
        uint32_t ms;
        TrackId id;
    };
    std::vector<Candidate> songs;
    for (TrackId id = 0; id < (TrackId)library.Count(); ++id) {
        const Track& t = library[id];
        const StringPool::Handle artist = t.artist ? t.artist : t.albumArtist;
        if (!t.title || !artist || !t.durationMs || covered.count(id) || !Eligible(library, id)) continue;
        std::string key = Folded(library.View(artist));
        key += '\x1f';
        key += Folded(library.View(t.title));
        songs.push_back({std::move(key), t.durationMs, id});
    }
    SortObjects(songs.begin(), songs.end(), [](const Candidate& a, const Candidate& b) {
        return std::tie(a.key, a.ms, a.id) < std::tie(b.key, b.ms, b.id);
    });
    for (size_t i = 0; i < songs.size();) {
        size_t j = i + 1;
        while (j < songs.size() && songs[j].key == songs[i].key && songs[j].ms - songs[j - 1].ms <= 1000) ++j;
        if (j - i >= 2) {
            Group g{false, {}};
            for (size_t k = i; k < j; ++k) g.tracks.push_back(songs[k].id);
            SortObjects(g.tracks.begin(), g.tracks.end(), better);
            groups.push_back(std::move(g));
        }
        i = j;
    }
    return groups;
}

std::vector<Move> Plan(const Library& library, const std::vector<Group>& groups,
                       const std::vector<std::vector<bool>>& remove, TrackId playing, size_t* whole) {
    std::vector<Move> moves;
    if (whole) *whole = 0;
    for (size_t g = 0; g < groups.size() && g < remove.size(); ++g) {
        const auto& tracks = groups[g].tracks;
        const auto& ticks = remove[g];
        size_t keep = 0;
        while (keep < tracks.size() && keep < ticks.size() && ticks[keep]) ++keep;
        if (keep >= tracks.size()) {
            if (whole && std::count(ticks.begin(), ticks.end(), true)) ++*whole;
            continue;
        }
        for (size_t i = 0; i < tracks.size() && i < ticks.size(); ++i)
            if (ticks[i] && tracks[i] != playing && Eligible(library, tracks[i]))
                moves.push_back({library.Path(tracks[i]), library.Path(tracks[keep])});
    }
    return moves;
}

Cleanup Merge(Library& library, std::vector<Playlist>& playlists, const std::vector<Move>& moves,
              const std::vector<bool>& recycled) {
    Cleanup c;
    struct To { TrackId keep; size_t merged; };
    TrackIdMap<To> forward;
    for (size_t i = 0; i < moves.size() && i < recycled.size(); ++i) {
        if (!recycled[i]) continue;
        const TrackId extra = Find(library, moves[i].extra), keep = Find(library, moves[i].keep);
        if (extra == kInvalidTrack || keep == kInvalidTrack || extra == keep || forward.find(extra)) continue;
        Track& e = library[extra];
        Track& k = library[keep];
        Merged m{moves[i].extra, moves[i].keep, Of(e), Of(k), {}};
        m.extraFavorite = library.Favorite(extra);
        m.keepBeforeFavorite = library.Favorite(keep);
        k.playCount = (uint32_t)std::min<uint64_t>((uint64_t)k.playCount + e.playCount, UINT32_MAX);
        k.skipCount = (uint32_t)std::min<uint64_t>((uint64_t)k.skipCount + e.skipCount, UINT32_MAX);
        k.lastPlayed = std::max(k.lastPlayed, e.lastPlayed);
        k.firstPlayed = Earliest(k.firstPlayed, e.firstPlayed);
        k.added = Earliest(k.added, e.added);
        if (!k.rating) k.rating = e.rating;
        if (m.extraFavorite) library.SetFavorite(keep, true);
        library.SetFavorite(extra, false);
        e.playCount = e.skipCount = 0;
        e.lastPlayed = e.firstPlayed = 0;
        e.rating = 0;
        e.flags |= Track::kMissing;  // its file is in the Recycle Bin; a rescan would say the same
        m.keepAfterFavorite = library.Favorite(keep);
        m.keepAfter = Of(k);
        forward[extra] = {keep, c.merged.size()};
        c.merged.push_back(std::move(m));
    }
    if (c.merged.empty()) return c;
    // A copy can inherit from one that inherited too (an identical copy removed in favour of a copy
    // that a same-song group then removes): follow the chain to the copy that stays.
    auto last = [&](TrackId id) {
        for (size_t n = 0; n <= forward.size(); ++n) {
            const To* to = forward.find(id);
            if (!to) break;
            id = to->keep;
        }
        return id;
    };
    std::vector<PlayEvent> log = PlayLog();
    for (size_t i = 0; i < log.size(); ++i) {
        const To* to = forward.find(log[i].id);
        if (!to) continue;
        c.events.push_back({i, log[i].time, to->merged});
        log[i].id = last(log[i].id);
    }
    if (!c.events.empty()) SetPlayLog(std::move(log));
    for (Playlist& pl : playlists) {
        if (!pl.Editable()) continue;
        std::vector<TrackId> after;
        after.reserve(pl.items.size());
        TrackIdSet present;
        for (TrackId id : pl.items)
            if (!forward.find(id)) present.insert(id);
        bool changed = false;
        for (TrackId id : pl.items) {
            if (!forward.find(id)) {
                after.push_back(id);
                continue;
            }
            changed = true;
            const TrackId keep = last(id);
            // Now Playing keeps every place, so what plays next is unchanged; a static playlist that
            // already lists the kept copy doesn't list it twice.
            if (pl.kind == PlaylistKind::NowPlaying || !present.contains(keep)) after.push_back(keep);
        }
        if (!changed) continue;
        std::vector<TrackId> before = std::move(pl.items);
        pl.items = after;
        c.playlists.push_back({pl.identity, before, std::move(after)});
        RecordPlaylistEdit(pl, std::move(before), L"Clean up duplicates");
    }
    library.Touch();
    return c;
}

std::vector<TrackId> Undo(Library& library, std::vector<Playlist>& playlists, const Cleanup& cleanup,
                          const std::vector<bool>& restored, size_t* skipped) {
    std::vector<TrackId> reread;
    if (skipped) *skipped = 0;
    std::vector<TrackId> extras(cleanup.merged.size(), kInvalidTrack);
    for (size_t i = cleanup.merged.size(); i-- > 0;) {
        if (i >= restored.size() || !restored[i]) continue;  // still in the bin: its history stays where it went
        const Merged& m = cleanup.merged[i];
        const TrackId keep = Find(library, m.keep);
        if (keep != kInvalidTrack) {
            Track& k = library[keep];
            const uint32_t plays = m.keepAfter.playCount - m.keepBefore.playCount;
            const uint32_t skips = m.keepAfter.skipCount - m.keepBefore.skipCount;
            k.playCount -= std::min(k.playCount, plays);
            k.skipCount -= std::min(k.skipCount, skips);
            if (k.lastPlayed == m.keepAfter.lastPlayed) k.lastPlayed = m.keepBefore.lastPlayed;
            if (k.firstPlayed == m.keepAfter.firstPlayed) k.firstPlayed = m.keepBefore.firstPlayed;
            if (k.added == m.keepAfter.added) k.added = m.keepBefore.added;
            if (k.rating == m.keepAfter.rating) k.rating = m.keepBefore.rating;
            if (library.Favorite(keep) == m.keepAfterFavorite) library.SetFavorite(keep, m.keepBeforeFavorite);
        }
        TrackId extra = Find(library, m.extra);
        if (extra == kInvalidTrack) {  // tidied away meanwhile: back as a new track with its history
            extra = library.Add(m.extra);
            library[extra].flags |= m.extraBefore.flags & (Track::kInLibrary | Track::kInbox);
        }
        Track& e = library[extra];
        Restore(e, m.extraBefore);
        if (m.extraFavorite) library.SetFavorite(extra, true);
        e.flags &= (uint16_t)~Track::kMissing;
        extras[i] = extra;
        reread.push_back(extra);
    }
    // Each listen goes back to the copy it was heard on, when the log still has it where Merge left it.
    std::vector<PlayEvent> log = PlayLog();
    bool logChanged = false;
    for (const EventChange& ev : cleanup.events) {
        if (ev.merged >= extras.size() || extras[ev.merged] == kInvalidTrack || ev.index >= log.size() ||
            log[ev.index].time != ev.time)
            continue;
        log[ev.index].id = extras[ev.merged];
        logChanged = true;
    }
    if (logChanged) SetPlayLog(std::move(log));
    // A playlist returns only when every copy came back: otherwise it would point at files still in the bin.
    const bool all = restored.size() >= cleanup.merged.size() &&
                     std::all_of(restored.begin(), restored.begin() + (ptrdiff_t)cleanup.merged.size(), [](bool b) { return b; });
    for (const PlaylistChange& change : cleanup.playlists) {
        auto it = std::find_if(playlists.begin(), playlists.end(), [&](const Playlist& p) { return p.identity == change.identity; });
        if (!all || it == playlists.end() || it->items != change.after) {
            if (skipped) ++*skipped;
            continue;
        }
        std::vector<TrackId> before = it->items;
        it->items = change.before;
        RecordPlaylistEdit(*it, std::move(before), L"Undo clean up duplicates");
    }
    library.Touch();
    return reread;
}

}  // namespace duplicates
