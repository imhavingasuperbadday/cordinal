// Capture one owned library and play-log copy in UI-thread batches. Handles are random per server
// generation and remain attached to a path, so compaction cannot turn an old URL into another song.
#include "web_library.h"
#include "web_internal.h"
#include "autoplaylist.h"
#include "playlog.h"

#include <algorithm>
#include <map>
#include <set>

namespace web {
struct LibraryBuilder::Impl {
    std::string epoch;
    uint64_t serial = 0, revision = 0, due = 0;
    uint64_t built = 0;          // signature of the last published snapshot
    uint64_t startSignature = 0;  // signature this pass is capturing
    std::map<std::wstring, std::string> keys;
    std::shared_ptr<Snapshot> result;
    std::shared_ptr<Library> owned;
    std::shared_ptr<std::vector<PlayEvent>> log;
    std::vector<TrackId> remap;
    std::vector<std::string> localKeys;
    std::vector<std::pair<size_t, std::string>> rules;
    size_t track = 0, count = 0, playlist = 0, item = 0, event = 0;
    int phase = 0;
};

// A cheap fingerprint of everything a snapshot shows. A browser left open for hours would
// otherwise make cordinal copy the whole library and play log every few seconds, even when
// nothing changed; this lets Tick return without touching the library.
uint64_t SnapshotSignature(const Library& library, const std::vector<::Playlist>& playlists, bool showOriginals) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (i * 8)) & 0xFF;
            h *= 1099511628211ull;
        }
    };
    auto text = [&](std::string_view s) {
        for (unsigned char c : s) {
            h ^= c;
            h *= 1099511628211ull;
        }
        h ^= 0;
        h *= 1099511628211ull;
    };
    auto wide = [&](const std::wstring& s) {
        for (wchar_t c : s) {
            h ^= (uint64_t)c;
            h *= 1099511628211ull;
        }
        h ^= 0;
        h *= 1099511628211ull;
    };
    mix(library.Revision());
    mix(PlayLog().size());
    mix(showOriginals);
    mix(playlists.size());
    for (const ::Playlist& p : playlists) {
        mix(p.identity);
        mix((uint64_t)p.kind);
        mix(p.items.size());
        wide(p.name);
        text(p.rules);
        for (TrackId id : p.items) mix(id);
    }
    return h;
}
LibraryBuilder::LibraryBuilder() : p_(std::make_unique<Impl>()) { Reset(); }
LibraryBuilder::~LibraryBuilder() = default;
bool LibraryBuilder::Busy() const { return p_ && p_->result != nullptr; }
void LibraryBuilder::Reset() {
    auto next = std::make_unique<Impl>(); next->epoch = RandomHex(12);
    next->revision = p_ ? p_->revision : 0; p_ = std::move(next);
}
std::shared_ptr<Snapshot> LibraryBuilder::Tick(const Library& library, const std::vector<::Playlist>& playlists,
                                             bool showOriginals) {
    auto& p = *p_;
    if (p.epoch.empty()) return {};
    uint64_t now = ClockMs(), deadline = now + 3;
    if (!p.result) {
        if (now < p.due) return {};
        const uint64_t signature = SnapshotSignature(library, playlists, showOriginals);
        if (signature == p.built) {
            p.due = now + 3000;
            return {};
        }
        p.startSignature = signature;
        p.result = std::make_shared<Snapshot>(); p.result->revision = ++p.revision;
        p.owned = std::make_shared<Library>(); p.log = std::make_shared<std::vector<PlayEvent>>();
        p.count = library.Count(); p.track = p.playlist = p.item = p.event = 0; p.phase = 0;
        p.remap.assign(p.count, kInvalidTrack); p.localKeys.clear(); p.rules.clear();
    }
    if (library.Count() < p.count) { Reset(); return {}; }
    do {
        if (p.phase == 0) {
            if (p.track == p.count) { p.phase = 1; continue; }
            TrackId id = (TrackId)p.track++;
            const auto& t = library[id];
            if (t.flags & ::Track::kMissing) continue;
            if (!t.Loaded()) continue;
            auto path = library.Path(id);
            auto [key, added] = p.keys.try_emplace(path);
            if (added) key->second = p.epoch + '-' + std::to_string(++p.serial);
            auto local = p.owned->ImportTrack(library, id); p.remap[id] = local;
            if (p.localKeys.size() <= local) p.localKeys.resize((size_t)local + 1);
            p.localKeys[local] = key->second;
            Track copy;
            copy.key = key->second; copy.id = id; copy.path = path; copy.title = library.View(t.title);
            if (copy.title.empty()) copy.title = ToUtf8(StemOf(path));
            copy.artist = library.View(t.artist); copy.album = library.View(t.album);
            copy.albumArtist = library.View(t.albumArtist ? t.albumArtist : t.artist);
            copy.genre = library.View(t.genre); copy.date = library.View(t.date); copy.codec = ToUtf8(CodecName(t.codec));
            copy.duration = t.Duration(); copy.number = t.trackNo; copy.disc = t.disc; copy.plays = t.playCount;
            copy.fileSize = t.fileSize; copy.fileTime = t.fileTime;
            copy.library = t.InLibrary() && !(t.flags & ::Track::kExcluded) &&
                (showOriginals || !(t.flags & ::Track::kOriginal));
            if (!copy.library) (*p.owned)[local].flags &= ~::Track::kInLibrary;
            copy.mime = Mime(path, copy.codec); copy.original = !IsSubtrack(path) && !copy.mime.empty();
            p.result->tracks.push_back(std::move(copy));
        } else if (p.phase == 1) {
            if (p.playlist >= playlists.size()) { p.phase = 2; continue; }
            const auto& source = playlists[p.playlist];
            if (source.kind != PlaylistKind::Static && source.kind != PlaylistKind::Auto) { ++p.playlist; p.item = 0; continue; }
            if (!p.item) {
                p.result->playlists.push_back({"p" + std::to_string(source.identity ? source.identity : p.playlist + 1),
                    ToUtf8(source.name), {}});
                if (source.kind == PlaylistKind::Auto) {
                    p.rules.emplace_back(p.result->playlists.size() - 1, source.rules);
                    ++p.playlist; continue;
                }
            }
            if (p.item < source.items.size()) {
                auto id = source.items[p.item++];
                if (id < p.remap.size() && p.remap[id] != kInvalidTrack)
                    p.result->playlists.back().tracks.push_back(p.localKeys[p.remap[id]]);
            } else { ++p.playlist; p.item = 0; }
        } else {
            const auto& events = PlayLog();
            if (p.event < events.size()) {
                auto e = events[p.event++];
                if (e.id < p.remap.size() && p.remap[e.id] != kInvalidTrack) {
                    e.id = p.remap[e.id]; p.log->push_back(e);
                }
            } else {
                auto result = std::move(p.result);
                result->prepare = [library = std::move(p.owned), log = std::move(p.log),
                    rules = std::move(p.rules), keys = std::move(p.localKeys)](Snapshot& s) {
                    for (const auto& [index, text] : rules) {
                        AutoPlaylist a; std::vector<TrackId> ids;
                        if (!ParseAutoPlaylist(text, a)) continue;
                        EvaluateAutoPlaylist(a, *library, *log, ids);
                        for (auto id : ids) if (id < keys.size()) s.playlists[index].tracks.push_back(keys[id]);
                    }
                    std::set<std::string> exposed;
                    for (const auto& list : s.playlists) for (const auto& key : list.tracks) exposed.insert(key);
                    std::erase_if(s.tracks, [&](const auto& t) { return !t.library && !exposed.count(t.key); });
                    uint64_t hash = 1469598103934665603ull;
                    auto mix = [&](std::string_view text) {
                        for (unsigned char c : text) hash = (hash ^ c) * 1099511628211ull;
                        hash = (hash ^ 0) * 1099511628211ull;
                    };
                    for (const auto& t : s.tracks) {
                        mix(TrackJson(t)); mix(std::to_string(t.fileSize)); mix(std::to_string(t.fileTime));
                        mix(t.library ? "library" : "playlist");
                    }
                    for (const auto& p : s.playlists) { mix(p.key); mix(p.name); for (const auto& key : p.tracks) mix(key); }
                    s.revision = hash & ((1ull << 53) - 1);  // exactly representable in browser JSON numbers
                };
                p.due = now + 3000;
                p.built = p.startSignature;
                return result;
            }
        }
    } while (ClockMs() < deadline);
    return {};
}
}  // namespace web
