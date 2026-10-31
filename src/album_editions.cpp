// Album groups and editions: normalization, conservative identification, manual edits, track
// correspondence and the library.cdb section. See album_editions.h for the contract.
#include "album_editions.h"

#include "sections.h"
#include "sort.h"

#include <algorithm>
#include <cstdlib>
#include <cwctype>
#include <limits>
#include <set>
#include <unordered_map>
#include <unordered_set>

std::vector<AlbumGroup> g_albumGroups;
std::map<TrackId, TrackMusicBrainz> g_trackMusicBrainz;

namespace {

uint32_t s_revision = 1;
uint32_t s_nextGroup = 1, s_nextEdition = 1;
bool s_idsDirty = true;
AlbumTrackIndex s_index;

constexpr size_t kMaxField = 65536;

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

std::wstring Lower(std::wstring_view text) {
    if (text.empty()) return {};
    std::wstring out(text.size(), L'\0');
    if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, text.data(), (int)text.size(), out.data(), (int)out.size(),
                      nullptr, nullptr, 0) <= 0) {
        for (size_t i = 0; i < text.size(); ++i) out[i] = (wchar_t)towlower(text[i]);
    }
    return out;
}

std::wstring Trim(std::wstring_view text) {
    size_t first = 0, last = text.size();
    while (first < last && iswspace(text[first])) ++first;
    while (last > first && iswspace(text[last - 1])) --last;
    return std::wstring(text.substr(first, last - first));
}

bool HasWord(const std::wstring& text, std::wstring_view word) {
    for (size_t at = text.find(word); at != std::wstring::npos; at = text.find(word, at + 1)) {
        bool left = at == 0 || !iswalnum(text[at - 1]);
        size_t end = at + word.size();
        bool right = end >= text.size() || !iswalnum(text[end]);
        if (left && right) return true;
    }
    return false;
}

bool Contains(const std::wstring& text, std::wstring_view part) { return text.find(part) != std::wstring::npos; }

struct EditionText {
    std::wstring base;       // display title with edition markers removed
    EditionKind kind = EditionKind::Original;
    bool live = false, tribute = false, karaoke = false, soundtrack = false, rerecord = false;
    int cues = 0;
};

int KindRank(EditionKind kind) {
    switch (kind) {
        case EditionKind::Deluxe: return 12;
        case EditionKind::Expanded: return 11;
        case EditionKind::Anniversary: return 10;
        case EditionKind::Remaster: return 9;
        case EditionKind::AlternateMix: return 8;
        case EditionKind::Instrumental: return 7;
        case EditionKind::Drumless: return 7;
        case EditionKind::Reissue: return 6;
        case EditionKind::Special: return 5;
        case EditionKind::Bonus: return 4;
        case EditionKind::Regional: return 3;
        case EditionKind::Mono:
        case EditionKind::Stereo: return 2;
        case EditionKind::Other: return 1;
        default: return 0;
    }
}

void AddCue(EditionText& info, EditionKind kind) {
    ++info.cues;
    if (KindRank(kind) > KindRank(info.kind)) info.kind = kind;
}

// Whether a title segment contains an edition marker, and which one. A segment that only says
// "live" is a category, not an edition cue: live and studio albums must not collapse together.
bool ParseCue(const std::wstring& segment, EditionText& info) {
    std::wstring s = Trim(segment);
    if (s.empty()) return false;
    float year = 0;
    (void)year;
    bool found = false;
    if (Contains(s, L"drumless")) { AddCue(info, EditionKind::Drumless); found = true; }
    if (Contains(s, L"instrumental")) { AddCue(info, EditionKind::Instrumental); found = true; }
    if (Contains(s, L"alternate mix") || Contains(s, L"alternative mix") || Contains(s, L"alt. mix") ||
        Contains(s, L"alt mix") || Contains(s, L"alternate version")) {
        AddCue(info, EditionKind::AlternateMix);
        found = true;
    }
    if (Contains(s, L"deluxe")) { AddCue(info, EditionKind::Deluxe); found = true; }
    if (Contains(s, L"expanded")) { AddCue(info, EditionKind::Expanded); found = true; }
    if (Contains(s, L"anniversary")) { AddCue(info, EditionKind::Anniversary); found = true; }
    if (Contains(s, L"remaster") || Contains(s, L"re-master") || Contains(s, L"remastered")) {
        AddCue(info, EditionKind::Remaster);
        found = true;
    }
    if (Contains(s, L"reissue") || Contains(s, L"re-issue")) { AddCue(info, EditionKind::Reissue); found = true; }
    if (Contains(s, L"special edition") || Contains(s, L"limited edition") || Contains(s, L"collector")) {
        AddCue(info, EditionKind::Special);
        found = true;
    }
    if (Contains(s, L"bonus")) { AddCue(info, EditionKind::Bonus); found = true; }
    if (Contains(s, L"japanese") || Contains(s, L"japan edition") || Contains(s, L"uk edition") ||
        Contains(s, L"u.s. edition") || Contains(s, L"us edition") || Contains(s, L"european") ||
        Contains(s, L"regional") || Contains(s, L"import edition") || Contains(s, L"tour edition")) {
        AddCue(info, EditionKind::Regional);
        found = true;
    }
    if (HasWord(s, L"mono")) { AddCue(info, EditionKind::Mono); found = true; }
    if (HasWord(s, L"stereo")) { AddCue(info, EditionKind::Stereo); found = true; }
    // "2011 remaster" and "30th anniversary edition" are covered above; a bare year is not a cue.
    return found;
}

void FindCategories(const std::wstring& lower, EditionText& info) {
    info.live = HasWord(lower, L"live");
    info.tribute = HasWord(lower, L"tribute") || HasWord(lower, L"karaoke") || HasWord(lower, L"covers");
    info.karaoke = HasWord(lower, L"karaoke");
    info.soundtrack = HasWord(lower, L"soundtrack") || HasWord(lower, L"ost") || HasWord(lower, L"score") ||
                      HasWord(lower, L"motion picture") || HasWord(lower, L"original cast");
    info.rerecord = Contains(lower, L"re-record") || Contains(lower, L"rerecord") || Contains(lower, L"re-recorded") ||
                    Contains(lower, L"rerecorded") || Contains(lower, L"new recording");
}

bool IsSeparator(wchar_t c) { return iswspace(c) || c == L'-' || c == L'\x2013' || c == L'\x2014' || c == L':' || c == L'/'; }

std::wstring RemoveCueBrackets(const std::wstring& text, EditionText& info) {
    std::wstring out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        wchar_t open = text[i];
        wchar_t close = open == L'(' ? L')' : open == L'[' ? L']' : open == L'{' ? L'}' : 0;
        if (!close) {
            out += text[i++];
            continue;
        }
        size_t end = text.find(close, i + 1);
        if (end == std::wstring::npos) {
            out.append(text.substr(i));
            break;
        }
        std::wstring inside = text.substr(i + 1, end - i - 1);
        EditionText probe = info;
        if (ParseCue(Lower(inside), probe)) {
            info = probe;
            i = end + 1;
        } else {
            out.append(text.substr(i, end - i + 1));
            i = end + 1;
        }
    }
    return out;
}

std::wstring RemoveTrailingSegments(const std::wstring& text, EditionText& info) {
    std::wstring out = text;
    for (int pass = 0; pass < 6; ++pass) {
        size_t cut = std::wstring::npos;
        for (size_t i = out.size(); i-- > 1;) {
            if (IsSeparator(out[i]) && IsSeparator(out[i - 1])) {
                std::wstring tail = Trim(out.substr(i + 1));
                EditionText probe = info;
                if (!tail.empty() && ParseCue(Lower(tail), probe)) {
                    info = probe;
                    cut = i - 1;
                    break;
                }
            }
        }
        if (cut == std::wstring::npos) break;
        out = Trim(out.substr(0, cut));
    }
    // A marker can also run straight into the title ("Album Deluxe Edition"). Only a trailing
    // phrase counts, so "The Remastered Album" keeps its name.
    static const wchar_t* const suffixes[] = {
        L" deluxe edition", L" deluxe", L" expanded edition", L" expanded", L" anniversary edition", L" anniversary",
        L" remastered edition", L" remastered", L" remaster", L" special edition", L" limited edition",
        L" reissue", L" re-issue", L" bonus tracks", L" bonus track", L" japanese edition", L" instrumental",
        L" instrumentals", L" drumless", L" alternate mix", L" alternative mix", L" mono", L" stereo",
    };
    for (int pass = 0; pass < 4; ++pass) {
        std::wstring lower = Lower(out);
        bool removed = false;
        for (const wchar_t* suffix : suffixes) {
            size_t n = wcslen(suffix);
            if (lower.size() > n && lower.compare(lower.size() - n, n, suffix) == 0) {
                EditionText probe = info;
                if (ParseCue(Lower(out.substr(out.size() - n)), probe)) {
                    info = probe;
                    out = Trim(out.substr(0, out.size() - n));
                    removed = true;
                    break;
                }
            }
        }
        if (!removed) break;
    }
    return out;
}

EditionText SplitEditionTitle(std::string_view album) {
    EditionText info;
    std::wstring original = FromUtf8(album);
    std::wstring lower = Lower(original);
    FindCategories(lower, info);
    std::wstring base = RemoveCueBrackets(original, info);
    base = RemoveTrailingSegments(base, info);
    base = Trim(base);
    if (base.empty()) base = Trim(original);
    info.base = base;
    return info;
}

std::string NormalizeWide(const std::wstring& text) {
    std::wstring lower = Lower(text);
    std::wstring out;
    out.reserve(lower.size());
    bool space = false;
    for (wchar_t c : lower) {
        if (iswalnum(c)) {
            out += c;
            space = false;
        } else if (c == L'\'' || c == L'\x2019') {
            // Apostrophes separate words in a search; dropping them would join "don't" to "dont".
            if (!out.empty() && out.back() != L' ') out += L'\'';
            space = false;
        } else if (!space) {
            out += L' ';
            space = true;
        }
    }
    while (!out.empty() && out.back() == L' ') out.pop_back();
    if (out.starts_with(L"the ")) out.erase(0, 4);
    return ToUtf8(out);
}

std::string AlbumKey(const std::string& artist, const std::string& album, const std::string& date) {
    return NormalizeAlbumText(artist) + '\x1f' + NormalizeAlbumText(album) + '\x1f' + NormalizeAlbumText(date);
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

bool OwnsTrack(const std::vector<AlbumGroup>& groups, TrackId id, uint32_t exceptGroup = 0, uint32_t exceptEdition = 0) {
    for (const auto& group : groups) {
        if (group.id == exceptGroup) {
            for (const auto& edition : group.editions) {
                if (edition.id == exceptEdition) continue;
                if (std::find(edition.tracks.begin(), edition.tracks.end(), id) != edition.tracks.end()) return true;
            }
        } else {
            for (const auto& edition : group.editions)
                if (std::find(edition.tracks.begin(), edition.tracks.end(), id) != edition.tracks.end()) return true;
        }
    }
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

std::string ReleaseIdForTrack(TrackId id) {
    auto it = g_trackMusicBrainz.find(id);
    return it == g_trackMusicBrainz.end() ? std::string{} : it->second.release;
}

std::string ReleaseGroupForTrack(TrackId id) {
    auto it = g_trackMusicBrainz.find(id);
    return it == g_trackMusicBrainz.end() ? std::string{} : it->second.releaseGroup;
}

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

}  // namespace

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
    std::map<TrackId, TrackMusicBrainz>().swap(g_trackMusicBrainz);
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
    if (releaseGroup.empty() && release.empty()) g_trackMusicBrainz.erase(id);
    else g_trackMusicBrainz[id] = {std::string(releaseGroup), std::string(release)};
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
    std::map<TrackId, TrackMusicBrainz> musicBrainz;
    for (auto& [id, value] : g_trackMusicBrainz)
        if (id < mapping.size() && mapping[id] != kInvalidTrack) musicBrainz[mapping[id]] = std::move(value);
    g_trackMusicBrainz = std::move(musicBrainz);
    AlbumGroupsChanged();
}

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

namespace {

void PutString(ByteWriter& w, const std::string& text) {
    std::string clean = text;
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
                                const std::map<TrackId, TrackMusicBrainz>& musicBrainz,
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
    std::vector<std::pair<uint32_t, const TrackMusicBrainz*>> mb;
    for (const auto& [id, value] : musicBrainz)
        if (id < newId.size() && newId[id] != UINT32_MAX && (!value.releaseGroup.empty() || !value.release.empty()))
            mb.emplace_back(newId[id], &value);
    std::sort(mb.begin(), mb.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    w.U(mb.size());
    uint32_t previous = 0;
    for (const auto& [index, value] : mb) {
        w.U(index - previous);
        previous = index;
        PutString(w, value->releaseGroup);
        PutString(w, value->release);
    }
    return out;
}

bool DecodeAlbumDatabase(std::string_view data, uint64_t trackCount, const std::vector<TrackId>* savedToId,
                         std::vector<AlbumGroup>* groups, std::map<TrackId, TrackMusicBrainz>* musicBrainz) {
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
    std::map<TrackId, TrackMusicBrainz> loadedMb;
    uint64_t mbCount = r.U(trackCount);
    uint64_t index = 0;
    for (uint64_t i = 0; i < mbCount && r.ok; ++i) {
        if (!ReadTrackIndex(r, trackCount, index)) return false;
        TrackMusicBrainz value;
        if (!ReadString(r, value.releaseGroup) || !ReadString(r, value.release)) return false;
        TrackId id = LiveId(index, savedToId);
        if (id == kInvalidTrack) return false;
        loadedMb[id] = std::move(value);
    }
    if (!r.ok || !r.Done()) return false;
    std::sort(loaded.begin(), loaded.end(), [](const AlbumGroup& a, const AlbumGroup& b) { return a.id < b.id; });
    if (groups) *groups = std::move(loaded);
    if (musicBrainz) *musicBrainz = std::move(loadedMb);
    return true;
}
