// The album maker's model, shared by both platforms (album_maker.h): a draft's starting values, its
// order and moves, and the tag edits Save writes.
#include "album_maker.h"

#include <algorithm>
#include <map>
#include <unordered_set>

#include <shlwapi.h>

namespace {

// The value most of the tracks share, ignoring empty ones; ties go to the one met first.
std::wstring Commonest(const Library& library, const std::vector<TrackId>& ids, StringPool::Handle Track::*field) {
    std::map<StringPool::Handle, std::pair<size_t, size_t>> counts;  // handle -> (count, first seen)
    for (size_t i = 0; i < ids.size(); ++i)
        if (StringPool::Handle h = library[ids[i]].*field) {
            auto [it, fresh] = counts.try_emplace(h, 0, i);
            ++it->second.first;
        }
    StringPool::Handle best = 0;
    std::pair<size_t, size_t> bestCount{0, 0};
    for (const auto& [h, c] : counts)
        if (c.first > bestCount.first || (c.first == bestCount.first && c.second < bestCount.second)) {
            best = h;
            bestCount = c;
        }
    return best ? library.Str(best) : std::wstring();
}

std::wstring Trimmed(const std::wstring& s) {
    size_t b = s.find_first_not_of(L" \t\r\n"), e = s.find_last_not_of(L" \t\r\n");
    return b == std::wstring::npos ? std::wstring() : s.substr(b, e - b + 1);
}

uint32_t DiscTrack(const Track& t) { return ((uint32_t)t.disc << 16) | t.trackNo; }

}  // namespace

AlbumDraft DraftAlbum(const Library& library, const std::vector<TrackId>& ids) {
    AlbumDraft d;
    std::unordered_set<TrackId> seen;
    for (TrackId id : ids)
        if (library.Contains(id) && seen.insert(id).second) d.tracks.push_back(id);
    if (d.tracks.empty()) return d;
    d.title = Commonest(library, d.tracks, &Track::album);
    d.artist = Commonest(library, d.tracks, &Track::albumArtist);
    d.date = Commonest(library, d.tracks, &Track::date);
    d.genre = Commonest(library, d.tracks, &Track::genre);
    bool oneArtist = true, numbered = true, compilation = true;
    std::unordered_set<uint32_t> numbers;
    for (TrackId id : d.tracks) {
        const Track& t = library[id];
        oneArtist &= t.artist == library[d.tracks[0]].artist;
        numbered &= t.trackNo != 0 && numbers.insert(DiscTrack(t)).second;
        compilation &= (t.flags & Track::kCompilation) != 0;
    }
    if (d.artist.empty() && oneArtist) d.artist = library.Str(library[d.tracks[0]].artist);
    // Different artists and no album artist: a compilation, as other players would group it.
    if (d.artist.empty() && !oneArtist) {
        d.artist = L"Various Artists";
        compilation = true;
    }
    d.compilation = compilation;
    if (numbered) OrderAlbum(library, d, AlbumOrder::Numbers);
    d.numberTracks = !numbered;
    return d;
}

void OrderAlbum(const Library& library, AlbumDraft& d, AlbumOrder order) {
    auto& v = d.tracks;
    switch (order) {
        case AlbumOrder::Numbers:
            // Unnumbered tracks after the numbered ones, in their present order.
            std::stable_sort(v.begin(), v.end(), [&](TrackId a, TrackId b) {
                const Track &ta = library[a], &tb = library[b];
                if (!ta.trackNo != !tb.trackNo) return ta.trackNo != 0;
                return DiscTrack(ta) < DiscTrack(tb);
            });
            break;
        case AlbumOrder::Title:
        case AlbumOrder::FileName:
        {
            // Str() converts from UTF-8 on every call, so the keys are made once, not once per comparison.
            auto field = order == AlbumOrder::Title ? &Track::title : &Track::file;
            std::vector<std::pair<std::wstring, TrackId>> keyed;
            keyed.reserve(v.size());
            for (TrackId id : v) keyed.emplace_back(library.Str(library[id].*field), id);
            std::stable_sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) {
                if (a.first.empty() != b.first.empty()) return b.first.empty();
                return StrCmpLogicalW(a.first.c_str(), b.first.c_str()) < 0;
            });
            for (size_t i = 0; i < v.size(); ++i) v[i] = keyed[i].second;
            break;
        }
        case AlbumOrder::Reverse: std::reverse(v.begin(), v.end()); break;
    }
}

std::vector<size_t> MoveAlbumTracks(AlbumDraft& d, const std::vector<size_t>& positions, int delta) {
    if (positions.empty() || (delta != -1 && delta != 1)) return positions;
    if (positions.back() >= d.tracks.size()) return positions;
    if ((delta < 0 && positions.front() == 0) || (delta > 0 && positions.back() + 1 >= d.tracks.size())) return positions;
    std::vector<char> picked(d.tracks.size(), 0);
    for (size_t p : positions) picked[p] = 1;
    // Each unpicked neighbour hops over the block beside it, which keeps a scattered selection's
    // gaps as they were.
    if (delta < 0) {
        for (size_t i = 1; i < d.tracks.size(); ++i)
            if (picked[i] && !picked[i - 1]) {
                std::swap(d.tracks[i], d.tracks[i - 1]);
                std::swap(picked[i], picked[i - 1]);
            }
    } else {
        for (size_t i = d.tracks.size() - 1; i-- > 0;)
            if (picked[i] && !picked[i + 1]) {
                std::swap(d.tracks[i], d.tracks[i + 1]);
                std::swap(picked[i], picked[i + 1]);
            }
    }
    std::vector<size_t> moved;
    for (size_t i = 0; i < picked.size(); ++i)
        if (picked[i]) moved.push_back(i);
    return moved;
}

size_t AddAlbumTracks(const Library& library, AlbumDraft& d, const std::vector<TrackId>& ids) {
    std::unordered_set<TrackId> have(d.tracks.begin(), d.tracks.end());
    size_t added = 0;
    for (TrackId id : ids)
        if (library.Contains(id) && have.insert(id).second) {
            d.tracks.push_back(id);
            ++added;
        }
    return added;
}

std::wstring AlbumDraftProblem(const AlbumDraft& d) {
    if (d.tracks.empty()) return L"Add at least one track.";
    if (Trimmed(d.title).empty()) return L"Give the album a title.";
    if (Trimmed(d.artist).empty() && !d.compilation) return L"Name the album's artist, or mark it as a compilation.";
    if (d.discTotal && d.disc > d.discTotal) return L"The disc number is larger than the number of discs.";
    return {};
}

std::vector<std::pair<TrackId, TagEdit>> AlbumEdits(const Library& library, const AlbumDraft& d) {
    std::vector<std::pair<TrackId, TagEdit>> out;
    if (!AlbumDraftProblem(d).empty()) return out;
    const std::wstring title = Trimmed(d.title), date = Trimmed(d.date), genre = Trimmed(d.genre);
    std::wstring artist = Trimmed(d.artist);
    if (artist.empty()) artist = L"Various Artists";
    // A field is written only where the track's tag says something else, so saving an album as it
    // already is rewrites no file.
    auto text = [&](std::optional<std::string>& slot, StringPool::Handle now, const std::wstring& want) {
        if (library.Str(now) != want) slot = ToUtf8(want);
    };
    auto number = [](std::optional<uint32_t>& slot, uint32_t now, uint32_t want) {
        if (now != want) slot = want;
    };
    const uint32_t count = (uint32_t)d.tracks.size();
    for (uint32_t i = 0; i < count; ++i) {
        const TrackId id = d.tracks[i];
        const Track& t = library[id];
        TagEdit e;
        text(e.album, t.album, title);
        text(e.albumArtist, t.albumArtist, artist);
        if (d.artistToTracks) text(e.artist, t.artist, artist);
        if (!date.empty()) text(e.date, t.date, date);
        if (!genre.empty()) text(e.genre, t.genre, genre);
        if (d.numberTracks) {
            number(e.track, t.trackNo, i + 1);
            number(e.trackTotal, t.trackTotal, count);
        }
        if (d.disc) number(e.disc, t.disc, d.disc);
        if (d.discTotal) number(e.discTotal, t.discTotal, d.discTotal);
        if (d.compilation != ((t.flags & Track::kCompilation) != 0)) e.compilation = d.compilation;
        if (d.cover) e.cover = *d.cover;
        if (!e.Empty()) out.emplace_back(id, std::move(e));
    }
    return out;
}
