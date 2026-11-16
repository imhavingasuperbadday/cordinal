// Finds songs and albums played a lot that have not been heard for longer than their play history
// suggests (rediscover.h). One pass over the library groups tracks into albums; nothing is stored.
#include "rediscover.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>

double RediscoverGapDays(double plays) {
    return std::clamp(300.0 / std::sqrt(std::max(plays, 1.0)), 14.0, 300.0);
}

namespace {

// How far past its gap something is, weighted so a favourite outranks a song barely past the line.
double Overdue(uint32_t now, uint32_t last, double plays) {
    if (!last || now <= last) return 0;
    double days = (now - last) / 86400.0, ratio = days / RediscoverGapDays(plays);
    return ratio < 1 ? 0 : ratio * std::log2(1 + plays);
}

// Liked enough to suggest: not mostly skipped and not rated two stars or less.
bool Liked(const Track& t) { return t.skipCount <= t.playCount && (t.rating == 0 || t.rating > 40); }

}  // namespace

std::vector<Rediscovery> FindRediscoveries(const Library& lib, uint32_t now, size_t albums, size_t tracks,
                                           const std::function<bool(TrackId)>& hidden) {
    struct Group { std::vector<TrackId> ids; uint32_t last = 0, played = 0, plays = 0; TrackId newest = UINT32_MAX; };
    std::map<AlbumId, Group> groups;
    std::vector<std::pair<double, TrackId>> songs;
    for (TrackId id = 0; id < (TrackId)lib.Count(); ++id) {
        const Track& t = lib[id];
        if (t.flags & Track::kFailed) continue;
        if (t.album) {
            Group& a = groups[AlbumKeyOf(t)];
            a.ids.push_back(id);
            a.plays += t.playCount;
            if (t.playCount) ++a.played;
            if (t.lastPlayed > a.last) a.last = t.lastPlayed, a.newest = id;
        }
        if (t.playCount >= 3 && Liked(t))
            if (double score = Overdue(now, t.lastPlayed, t.playCount); score > 0) songs.push_back({score, id});
    }
    std::vector<std::pair<double, const Group*>> ranked;
    for (const auto& [key, a] : groups) {
        // An album: several tracks, most of them played, so it was listened to as one.
        if (a.ids.size() < 3 || a.played * 2 < a.ids.size() || (hidden && hidden(a.newest))) continue;
        double perTrack = (double)a.plays / a.ids.size();
        if (double score = Overdue(now, a.last, perTrack); score > 0) ranked.push_back({score, &a});
    }
    std::ranges::sort(ranked, std::greater<>{}, &std::pair<double, const Group*>::first);
    std::ranges::sort(songs, std::greater<>{});
    std::vector<Rediscovery> out;
    for (size_t i = 0; i < ranked.size() && i < albums; ++i) {
        Rediscovery r{ranked[i].second->ids, true, ranked[i].second->last,
                      (double)ranked[i].second->plays / ranked[i].second->ids.size()};
        std::ranges::sort(r.tracks, [&](TrackId x, TrackId y) {
            const Track &a = lib[x], &b = lib[y];
            if (a.disc != b.disc) return a.disc < b.disc;
            if (a.trackNo != b.trackNo) return a.trackNo < b.trackNo;
            return lib.Str(a.file) < lib.Str(b.file);
        });
        out.push_back(std::move(r));
    }
    size_t listed = out.size();
    for (const auto& [score, id] : songs) {
        if (out.size() - listed >= tracks) break;
        if (hidden && hidden(id)) continue;
        bool inAlbum = std::ranges::any_of(out.begin(), out.begin() + listed,
                                           [id](const Rediscovery& r) { return std::ranges::find(r.tracks, id) != r.tracks.end(); });
        if (!inAlbum) out.push_back({{id}, false, lib[id].lastPlayed, (double)lib[id].playCount});
    }
    return out;
}
