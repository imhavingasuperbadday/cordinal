// The media library: the string pool, the track store and its path index, tag application,
// duplicate detection, and the file/playlist collection helpers. It is owned by the UI thread;
// interned string bytes never move, so TrackPath snapshots stay valid for workers.
#include "sort.h"
#include "library.h"

#include "album_editions.h"
#include "archive.h"
#include "cue.h"
#include "helpers.h"

#include <shlwapi.h>

#include <algorithm>
#include <cwctype>
#include <optional>
#include <charconv>
#include <limits>
#include <stdexcept>

Library g_lib;

// Saved album measurements, keyed by string handles of the library they belong to.
std::map<AlbumId, AlbumMeasurement> g_albumMeasurements;

std::wstring NormalizeMediaPath(const std::wstring& path) {
    if (path.empty() || path.size() > 32767 ||
        std::any_of(path.begin(), path.end(), [](wchar_t c) { return c < L' '; })) return {};
    DWORD n = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (!n || n > 32768) return {};
    std::wstring full(n, L'\0');
    DWORD got = GetFullPathNameW(path.c_str(), n, full.data(), nullptr);
    if (!got || got >= n) return {};
    full.resize(got);
#ifdef _WIN32
    std::replace(full.begin(), full.end(), L'/', L'\\');
#endif
    return full;
}

namespace {
// Windows lookups ignore case and extended-length prefixes; POSIX keeps file names exact.
std::wstring PathKey(std::wstring canonical) {
#ifdef _WIN32
    if (canonical.compare(0, 8, L"\\\\?\\UNC\\") == 0) canonical = L"\\\\" + canonical.substr(8);
    else if (canonical.compare(0, 4, L"\\\\?\\") == 0 && canonical.size() > 6 && canonical[5] == L':') canonical.erase(0, 4);
    std::wstring key(canonical.size(), L'\0');
    if (!canonical.empty() && LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, canonical.data(), (int)canonical.size(),
                                     key.data(), (int)key.size(), nullptr, nullptr, 0)) return key;
    return canonical;
#else
    // POSIX names are case-sensitive; Windows separators and case folding would change files.
    return canonical;
#endif
}
}



// ---------------------------------------------------------------------------
// Strings and paths

std::string ToUtf8(std::wstring_view s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string r(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), r.data(), n, nullptr, nullptr);
    return r;
}

std::wstring FromUtf8(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring r(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), r.data(), n);
    return r;
}

std::wstring FormatDuration(double seconds) {
    if (!(seconds > 0) || !std::isfinite(seconds)) return L"0:00";
    seconds = std::min(seconds, 1.0e15);
    long long t = (long long)seconds;
    wchar_t buf[48];
    if (t >= 3600)
        swprintf_s(buf, L"%lld:%02lld:%02lld", t / 3600, (t / 60) % 60, t % 60);
    else
        swprintf_s(buf, L"%lld:%02lld", t / 60, t % 60);
    return buf;
}

std::wstring FormatTotalDuration(double seconds) {
    long long t = (long long)(std::isfinite(seconds) && seconds > 0 ? std::min(seconds, 1.0e15) : 0);
    if (t < 86400) return FormatDuration((double)t);
    wchar_t buf[64];
    swprintf_s(buf, L"%lldd %lld:%02lld:%02lld", t / 86400, (t / 3600) % 24, (t / 60) % 60, t % 60);
    return buf;
}

std::wstring FileNameOf(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring StemOf(const std::wstring& path) {
    std::wstring name = FileNameOf(path);
    size_t dot = name.rfind(L'.');
    return (dot == std::wstring::npos || dot == 0) ? name : name.substr(0, dot);
}

std::wstring ExtOf(const std::wstring& path) {
    std::wstring name = FileNameOf(path);
    size_t dot = name.rfind(L'.');
    if (dot == std::wstring::npos) return {};
    std::wstring ext = name.substr(dot + 1);
    for (auto& c : ext) c = (wchar_t)towlower(c);
    return ext;
}

std::wstring DirectoryOf(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::vector<std::wstring> FolderTreePath(std::wstring path, const std::vector<std::wstring>& roots) {
    const bool windows = (path.size() > 1 && path[1] == L':') || path.starts_with(L"\\\\");
    const wchar_t sep = windows ? L'\\' : L'/';
    auto clean = [&](std::wstring s) {
        if (windows) std::replace(s.begin(), s.end(), L'/', sep);
        while (s.size() > 1 && s.back() == sep && !(windows && s.size() == 3 && s[1] == L':'))
            s.pop_back();
        return s;
    };
    path = clean(std::move(path));
    std::wstring root;
    for (const auto& candidate : roots) {
        std::wstring r = clean(candidate);
        if (r.empty() || r.size() <= root.size() || path.size() <= r.size() ||
            (r.back() != sep && path[r.size()] != sep)) continue;
        bool same = windows ? CompareStringOrdinal(path.data(), (int)r.size(), r.data(), (int)r.size(), TRUE) == CSTR_EQUAL
                            : path.compare(0, r.size(), r) == 0;
        if (same) root = std::move(r);
    }
    if (root.empty()) {
        size_t slash = path.find_last_of(sep);
        if (slash != std::wstring::npos) root = path.substr(0, slash ? slash : 1);
        if (windows && root.size() == 2 && root[1] == L':') root += sep;
    }
    std::vector<std::wstring> parts;
    if (!root.empty()) parts.push_back(root);
    for (size_t start = root.size(); start < path.size();) {
        size_t end = path.find(sep, start);
        if (end == std::wstring::npos) end = path.size();
        if (end > start) parts.push_back(path.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

bool SplitSubtrack(std::wstring_view path, std::wstring& file, int& index) {
    size_t bar = path.rfind(L'|');
    if (bar == std::wstring_view::npos || bar + 1 >= path.size()) return false;
    int n = 0;
    for (wchar_t c : path.substr(bar + 1)) {
        if (c < L'0' || c > L'9' || n > 99999) return false;
        n = n * 10 + (c - L'0');
    }
    if (n <= 0) return false;
    file.assign(path.substr(0, bar));
    index = n;
    return true;
}

std::wstring SubtrackPath(std::wstring_view file, int index) {
    std::wstring path(file);
    path += L'|';
    path += std::to_wstring(index);
    return path;
}


// ---------------------------------------------------------------------------
// String pool

namespace {
// FNV-1a with a finalizer. MapSlot takes the top 32 bits of hash*capacity, so the weak low bits
// of the multiply do not matter.
uint32_t Hash(std::string_view s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) h = (h ^ c) * 16777619u;
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    return h;
}
size_t MapSlot(uint32_t hash, size_t cap) { return (size_t)(((uint64_t)hash * cap) >> 32); }

// Path keys are already UTF-16 on Windows. The index never persists its buckets, so hash those
// code units directly instead of allocating and encoding another copy for every lookup.
uint32_t PathHash(std::wstring_view s) {
    uint32_t h = 2166136261u;
    for (wchar_t c : s) h = (h ^ (uint32_t)c) * 16777619u;
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    return h;
}
}  // namespace

// Handles with the top bit set index large_ (no real string is ever that long); every other
// handle is a byte offset into the arena, biased by one so 0 can mean "empty".
std::string_view StringPool::Get(Handle h) const {
    if (!h) return {};
    if (h & 0x80000000u) {
        size_t index = h & 0x7FFFFFFFu;
        return index < large_.size() ? std::string_view(large_[index]) : std::string_view();
    }
    size_t pos = h - 1;
    if (pos / kBlock >= blocks_.size()) return {};
    if (pos / kBlock + 1 == blocks_.size() && pos % kBlock >= used_) return {};
    const char* p = blocks_[pos / kBlock].get() + pos % kBlock;
    size_t available = (pos / kBlock + 1 == blocks_.size() ? used_ : kBlock) - pos % kBlock;
    size_t len = 0;  // LEB128 length prefix
    int shift = 0;
    unsigned char c;
    do {
        if (!available-- || shift >= 35) return {};
        c = (unsigned char)*p++;
        len |= (size_t)(c & 0x7F) << shift;
        shift += 7;
    } while (c & 0x80);
    if (len > available) return {};
    return {p, len};
}

// Append-only: an interned string's bytes never move, so handles stay valid for the pool's life.
StringPool::Handle StringPool::Store(std::string_view s) {
    if (s.size() > (1u << 20)) throw std::length_error("Library string exceeds 1 MiB");
    size_t prefix = 1;
    for (size_t n = s.size() >> 7; n; n >>= 7) ++prefix;
    size_t need = prefix + s.size();
    if (need > kBlock) {
        if (large_.size() >= 0x80000000ull) throw std::length_error("Too many library strings");
        large_.emplace_back(s);
        return 0x80000000u | (Handle)(large_.size() - 1);
    }
    if (used_ + need > kBlock) {
        // Each block ends in a 0 sentinel after its last string; Grow scans blocks with it instead
        // of keeping a separate byte count per block.
        if (!blocks_.empty() && used_ < kBlock) blocks_.back()[used_] = 0;
        if (blocks_.size() >= 0x7fffffffull / kBlock) throw std::length_error("Library string arena is full");
        blocks_.push_back(std::make_unique_for_overwrite<char[]>(kBlock));
        used_ = 0;
    }
    char* base = blocks_.back().get();
    size_t start = used_;
    size_t len = s.size();
    do {
        unsigned char c = len & 0x7F;
        len >>= 7;
        base[used_++] = (char)(c | (len ? 0x80 : 0));
    } while (len);
    memcpy(base + used_, s.data(), s.size());
    used_ += s.size();
    return (Handle)((blocks_.size() - 1) * kBlock + start + 1);
}

// Rebuilds the hash table (load factor stays under 3/4) and re-inserts every string; the string
// bytes themselves are untouched, so existing handles keep working.
void StringPool::Grow(size_t minCapacity) {
    size_t cap = table_.empty() ? 64 : (table_.size() * 3 / 2 + 63) & ~size_t(63);
    if (minCapacity > cap) cap = (minCapacity + 63) & ~size_t(63);
    std::vector<Handle>().swap(table_);
    table_.assign(cap, 0);
    auto insert = [&](Handle h, std::string_view s) {
        size_t i = MapSlot(Hash(s), cap);
        while (table_[i]) {
            if (++i == cap) i = 0;
        }
        table_[i] = h;
    };
    for (size_t b = 0; b < blocks_.size(); ++b) {
        const char* base = blocks_[b].get();
        size_t limit = b + 1 == blocks_.size() ? used_ : kBlock;
        for (size_t pos = 0; pos < limit && base[pos] != 0;) {
            Handle h = (Handle)(b * kBlock + pos + 1);
            size_t len = 0;
            int shift = 0;
            unsigned char c;
            do {
                c = (unsigned char)base[pos++];
                len |= (size_t)(c & 0x7F) << shift;
                shift += 7;
            } while (c & 0x80);
            insert(h, {base + pos, len});
            pos += len;
        }
    }
    for (size_t i = 0; i < large_.size(); ++i) insert(0x80000000u | (Handle)i, large_[i]);
}

void StringPool::Reserve(size_t count) {
    size_t needed = count * 4 / 3 + 16;
    if (needed > table_.size()) Grow(needed);
}

StringPool::Handle StringPool::Lookup(std::string_view s) const {
    if (s.empty() || table_.empty()) return 0;
    size_t cap = table_.size();
    for (size_t i = MapSlot(Hash(s), cap); Handle h = table_[i]; i = (i + 1 == cap ? 0 : i + 1))
        if (Get(h) == s) return h;
    return 0;
}

StringPool::Handle StringPool::Intern(std::string_view s) {
    if (s.empty()) return 0;
    const uint32_t hash = Hash(s);
    size_t cap = table_.size();
    size_t i = MapSlot(hash, cap);
    if (cap) {
        while (Handle h = table_[i]) {
            if (Get(h) == s) return h;
            if (++i == cap) i = 0;
        }
    }
    // A miss already found its empty bucket. Only growth invalidates that position; the string
    // bytes and hash stay valid, including when the caller borrowed them from this pool.
    if ((count_ + 1) * 4 >= cap * 3) {
        Grow();
        cap = table_.size();
        i = MapSlot(hash, cap);
        while (table_[i]) if (++i == cap) i = 0;
    }
    Handle h = Store(s);
    table_[i] = h;
    ++count_;
    return h;
}

size_t StringPool::Bytes() const {
    size_t large = 0;
    for (auto& s : large_) large += s.capacity();
    return blocks_.size() * kBlock + blocks_.capacity() * sizeof(blocks_[0]) +
           table_.capacity() * sizeof(Handle) + large_.size() * sizeof(large_[0]) + large;
}

// ---------------------------------------------------------------------------
// Library

// Maps a (dir, file) pair to an open-addressing slot, hashing the same canonical path text that
// Find and Add compare, so lookups and inserts agree.
size_t Library::Slot(StringPool::Handle dir, StringPool::Handle file) const {
    return MapSlot(PathHash(PathKey(TrackPath{0, View(dir), View(file)}.Path())), byPath_.size());
}

void Library::Index(TrackId id) {
    if (tracks_.size() * 4 >= byPath_.size() * 3) {
        size_t cap = byPath_.empty() ? 64 : (byPath_.size() * 3 / 2 + 63) & ~size_t(63);
        std::vector<TrackId>().swap(byPath_);
        byPath_.assign(cap, UINT32_MAX);
        for (TrackId t = 0; t < (TrackId)tracks_.size(); ++t) {
            if (t == id) continue;
            size_t i = Slot(tracks_[t].dir, tracks_[t].file);
            while (byPath_[i] != UINT32_MAX) {
                if (++i == cap) i = 0;
            }
            byPath_[i] = t;
        }
    }
    size_t cap = byPath_.size(), i = Slot(tracks_[id].dir, tracks_[id].file);
    while (byPath_[i] != UINT32_MAX) {
        if (++i == cap) i = 0;
    }
    byPath_[i] = id;
}

void Library::Unindex(TrackId id) {
    // Linear probing: remove the slot, then re-insert the rest of its cluster.
    size_t cap = byPath_.size(), i = Slot(tracks_[id].dir, tracks_[id].file);
    while (byPath_[i] != id) {
        if (byPath_[i] == UINT32_MAX) return;
        if (++i == cap) i = 0;
    }
    byPath_[i] = UINT32_MAX;
    for (size_t j = (i + 1 == cap ? 0 : i + 1); byPath_[j] != UINT32_MAX; j = (j + 1 == cap ? 0 : j + 1)) {
        TrackId moved = byPath_[j];
        byPath_[j] = UINT32_MAX;
        size_t k = Slot(tracks_[moved].dir, tracks_[moved].file);
        while (byPath_[k] != UINT32_MAX) {
            if (++k == cap) k = 0;
        }
        byPath_[k] = moved;
    }
}

void Library::Reserve(size_t strings, size_t tracks) {
    if (strings) strings_.Reserve(strings);
    size_t needed = (tracks * 4 / 3 + 16 + 63) & ~size_t(63);
    if (tracks && needed > byPath_.size()) {
        std::vector<TrackId>().swap(byPath_);
        byPath_.assign(needed, UINT32_MAX);
        for (TrackId t = 0; t < (TrackId)tracks_.size(); ++t) {
            size_t i = Slot(tracks_[t].dir, tracks_[t].file);
            while (byPath_[i] != UINT32_MAX) {
                if (++i == needed) i = 0;
            }
            byPath_[i] = t;
        }
    }
}

void Library::SplitPath(const std::wstring& path, StringPool::Handle& dir, StringPool::Handle& file) {
    size_t slash = path.find_last_of(L"\\/");
    std::wstring_view view(path);
    dir = InternW(slash == std::wstring::npos ? std::wstring_view() : view.substr(0, slash));
    file = InternW(slash == std::wstring::npos ? view : view.substr(slash + 1));
}

TrackId Library::FindKey(const std::wstring& key) const {
    if (byPath_.empty() || key.empty()) return UINT32_MAX;
    size_t cap = byPath_.size();
    for (size_t i = MapSlot(PathHash(key), cap); byPath_[i] != UINT32_MAX; i = (i + 1 == cap ? 0 : i + 1))
        if (PathKey(Path(byPath_[i])) == key) return byPath_[i];
    return UINT32_MAX;
}

TrackId Library::Find(const std::wstring& path) const {
    return FindKey(PathKey(NormalizeMediaPath(path)));
}

// Joins a split path back together and defers to the string overload.
TrackId Library::Add(StringPool::Handle dir, StringPool::Handle file) {
    std::wstring path = Str(dir);
    if (!path.empty()) path += L'\\';
    path += Str(file);
    return Add(path);
}

TrackId Library::Add(const std::wstring& input) {
    std::wstring path = NormalizeMediaPath(input);
    if (path.empty() || FileNameOf(path).empty()) return UINT32_MAX;
    if (TrackId existing = FindKey(PathKey(path)); existing != UINT32_MAX) return existing;
    if (tracks_.size() >= UINT32_MAX) throw std::length_error("Too many library tracks");
    Track track;
    SplitPath(path, track.dir, track.file);
    track.added = UnixNow();
    tracks_.push_back(track);
    TrackId id = (TrackId)(tracks_.size() - 1);
    Index(id);
    ++revision_;
    return id;
}

// Copies a track into this library, re-interning every string handle so it points at this
// library's pool. A destination that already has tags is left alone.
TrackId Library::ImportTrack(const Library& source, TrackId id) {
    if (!source.Contains(id)) return UINT32_MAX;
    TrackId dest = Add(source.Path(id));
    if (dest == UINT32_MAX || tracks_[dest].Loaded()) return dest;
    Track copy = source[id];
    copy.dir = tracks_[dest].dir;
    copy.file = tracks_[dest].file;
    for (auto member : {&Track::title, &Track::artist, &Track::album, &Track::albumArtist, &Track::genre,
                        &Track::date, &Track::composer, &Track::comment, &Track::grouping, &Track::label})
        copy.*member = Intern(source.View(copy.*member));
    tracks_[dest] = copy;
    return dest;
}

bool Library::Rename(TrackId id, const std::wstring& path) {
    std::wstring full = NormalizeMediaPath(path);
    if (!Contains(id) || full.empty() || FileNameOf(full).empty()) return false;
    TrackId existing = Find(full);
    if (existing != UINT32_MAX && existing != id) return false;
    Unindex(id);
    SplitPath(full, tracks_[id].dir, tracks_[id].file);
    Index(id);
    ++revision_;
    return true;
}

std::wstring Library::Path(TrackId id) const {
    return SnapshotPath(id).Path();
}

std::wstring TrackPath::Path() const {
    std::string full;
    full.reserve(dir.size() + file.size() + 1);
    full.append(dir);
    if (!dir.empty()) full += (char)kPathSep;
    full.append(file);
    return FromUtf8(full);
}

std::vector<std::vector<TrackId>> Library::FindDuplicateAudio() const {
    // A flat (hash, id) list replaces the hash map of vectors: the same groups without a heap
    // node and a separate vector per distinct hash. Sorting the pairs also keeps the ids of a
    // group ascending; groups are sorted below, so the result matches the map's final order.
    std::vector<std::pair<uint64_t, TrackId>> byHash;
    for (TrackId id = 0; id < (TrackId)tracks_.size(); ++id) {
        const Track& t = tracks_[id];
        if (!t.audioHash || (t.flags & Track::kMissing) || !(t.flags & (Track::kInLibrary | Track::kInbox))) continue;
        byHash.emplace_back(t.audioHash, id);
    }
    return GroupDuplicateAudio(std::move(byHash));
}

std::vector<std::vector<TrackId>> GroupDuplicateAudio(std::vector<std::pair<uint64_t, TrackId>> byHash) {
    SortObjects(byHash.begin(), byHash.end());
    std::vector<std::vector<TrackId>> groups;
    for (size_t i = 0; i < byHash.size();) {
        size_t j = i + 1;
        while (j < byHash.size() && byHash[j].first == byHash[i].first) ++j;
        if (j - i >= 2) {
            groups.emplace_back();
            groups.back().reserve(j - i);
            for (size_t k = i; k < j; ++k) groups.back().push_back(byHash[k].second);
        }
        i = j;
    }
    SortObjects(groups.begin(), groups.end());
    return groups;
}

int16_t EncodeReplayGain(float gain) {
    return std::isfinite(gain) ? (int16_t)std::lround(std::clamp(gain, -320.0f, 320.0f) * 100) : kNoGain;
}

uint16_t EncodeReplayPeak(float peak) {
    return std::isfinite(peak) && peak > 0 ?
        (uint16_t)std::clamp(std::lround(std::min(peak, 3.27675f) * 20000), 1L, 65535L) : 0;
}

// Replaces a track's decoded tags. Text is truncated on a UTF-8 boundary and stripped of NULs;
// ReplayGain measured by cordinal survives re-tagging (kRgScanned), so a tag edit cannot undo it.
void Library::ApplyTags(TrackId id, const TagData& d) {
    if (!Contains(id)) return;
    Track& t = tracks_[id];
    Track previous = t;
    auto tag = [&](std::string_view text) {
        size_t length = std::min<size_t>(text.size(), 65536);
        if (length < text.size()) while (length && ((unsigned char)text[length] & 0xC0) == 0x80) --length;
        text = text.substr(0, length);
        if (text.find('\0') == std::string_view::npos) return Intern(text);
        std::string clean(text);
        std::replace(clean.begin(), clean.end(), '\0', ' ');
        return Intern(clean);
    };
    t.audioHash = d.audioHash;
    t.title = tag(d.title);
    t.artist = tag(d.artist);
    t.album = tag(d.album);
    t.albumArtist = tag(d.albumArtist);
    t.genre = tag(d.genre);
    t.date = tag(d.date);
    t.composer = tag(d.composer);
    // Comments can hold long notes and lyrics; retain Cordinal's existing large-field support.
    size_t commentLength = std::min<size_t>(d.comment.size(), 1u << 20);
    if (commentLength < d.comment.size())
        while (commentLength && ((unsigned char)d.comment[commentLength] & 0xC0) == 0x80) --commentLength;
    t.comment = Intern(std::string_view(d.comment).substr(0, commentLength));
    t.grouping = tag(d.grouping);
    t.label = tag(d.label);
    t.trackNo = (uint16_t)std::min<uint32_t>(d.track, 65535);
    t.trackTotal = (uint16_t)std::min<uint32_t>(d.trackTotal, 65535);
    t.disc = (uint8_t)std::min<uint32_t>(d.disc, 255);
    t.discTotal = (uint8_t)std::min<uint32_t>(d.discTotal, 255);
    t.bpm = (uint16_t)std::min<uint32_t>(d.bpm, 65535);
    t.flags = (uint16_t)(d.compilation ? t.flags | Track::kCompilation : t.flags & ~Track::kCompilation);
    t.durationMs = std::isfinite(d.duration) ? (uint32_t)std::clamp(d.duration * 1000.0, 0.0, 4.0e9) : 0;
    t.sampleRate = d.sampleRate;
    t.channels = (uint8_t)std::min<uint32_t>(d.channels, 255);
    t.bitrate = (uint16_t)std::min<uint32_t>(d.bitrate, 65535);
    t.codec = d.codec <= kLastCodec ? d.codec : Codec::Unknown;
    t.rgTrackGain = EncodeReplayGain(d.rgTrackGain);
    t.rgAlbumGain = EncodeReplayGain(d.rgAlbumGain);
    t.rgTrackPeak = EncodeReplayPeak(d.rgTrackPeak);
    t.rgAlbumPeak = EncodeReplayPeak(d.rgAlbumPeak);
    if (previous.flags & Track::kRgScanned) {
        t.rgTrackGain = previous.rgTrackGain;
        t.rgAlbumGain = previous.rgAlbumGain;
        t.rgTrackPeak = previous.rgTrackPeak;
        t.rgAlbumPeak = previous.rgAlbumPeak;
    } else {
        // Tags replaced the album values this track had: its saved album measurement no longer
        // describes it, so the album is worked out again.
        t.flags &= (uint16_t)~Track::kRgAlbumMeasured;
    }
    t.flags &= ~(Track::kFailed | Track::kTagFailed);
    t.flags |= Track::kLoaded;
    // MusicBrainz album ids live beside the library rather than in the compact Track record. They
    // are read from tags, never fetched, so offline browsing and playback never depend on a network.
    if (this == &g_lib) SetTrackMusicBrainz(id, d.musicBrainzReleaseGroup, d.musicBrainzRelease);
    ++revision_;
}

// Rebuilds the library from `live` only, renumbering ids densely. Returns the old id -> new id
// map (UINT32_MAX for dropped tracks) so callers can remap playlists and the play log.
std::vector<TrackId> Library::Compact(const std::vector<TrackId>& live) {
    // The live list is the caller's policy (playlists, history, stats); album editions are library
    // organization and must survive too, or a compaction would silently dissolve a manual group.
    std::vector<TrackId> kept;
    if (this == &g_lib) {
        std::vector<bool> retain(Count());
        for (TrackId id : live) if (Contains(id)) retain[id] = true;
        MarkAlbumTracks(retain);
        for (TrackId id = 0; id < (TrackId)retain.size(); ++id) if (retain[id]) kept.push_back(id);
    } else {
        kept = live;
    }
    std::vector<TrackId> mapping(Count(), UINT32_MAX);
    Library compact;
    compact.Reserve(0, kept.size());
    for (TrackId id : kept)
        if (Contains(id) && mapping[id] == UINT32_MAX) mapping[id] = compact.ImportTrack(*this, id);
    compact.revision_ = revision_ + 1;
    std::map<AlbumId, AlbumMeasurement> measurements;
    for (const auto& [key, saved] : g_albumMeasurements) {
        AlbumMeasurement remapped{saved.gain, saved.peak, saved.dynamics, {}};
        bool complete = !saved.members.empty();
        for (TrackId id : saved.members) {
            if (id >= mapping.size() || mapping[id] == kInvalidTrack) { complete = false; break; }
            remapped.members.push_back(mapping[id]);
        }
        if (!complete) continue;
        SortRecords(remapped.members.begin(), remapped.members.end());
        measurements[AlbumKeyOf(compact[remapped.members.front()])] = std::move(remapped);
    }
    bool wasGlobal = this == &g_lib;
    *this = std::move(compact);
    g_albumMeasurements = std::move(measurements);
    if (wasGlobal) RemapAlbumGroups(mapping);
    return mapping;
}

size_t Library::MemoryBytes() const {
    return tracks_.Bytes() + byPath_.capacity() * sizeof(TrackId) + strings_.Bytes();
}

// Seconds since the Unix epoch in UTC, as the 32-bit stat fields use; wraps in 2106.
uint32_t UnixNow() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (uint32_t)((((uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime) / 10000000ull) - 11644473600ull);
}

std::wstring RatingStars(uint8_t rating) {
    std::wstring s(rating / 20, L'\x2605');
    if (rating % 20) s += L'\x00BD';
    return s;
}

bool LookupField(TrackId id, const std::wstring& name, std::wstring& out) {
    if (!g_lib.Contains(id)) return false;
    const Track& t = g_lib[id];
    auto set = [&](std::wstring v) {
        out = std::move(v);
        return !out.empty();
    };
    auto num = [&](unsigned long long v) {
        if (!v) return false;
        out = std::to_wstring(v);
        return true;
    };
    static const struct { const wchar_t* name; StringPool::Handle Track::*field; } strings[] = {
        {L"artist", &Track::artist}, {L"album", &Track::album}, {L"date", &Track::date}, {L"genre", &Track::genre},
        {L"composer", &Track::composer}, {L"comment", &Track::comment}, {L"grouping", &Track::grouping},
        {L"label", &Track::label}, {L"publisher", &Track::label}, {L"filename_ext", &Track::file},
    };
    for (const auto& field : strings)
        if (name == field.name) return set(g_lib.Str(t.*field.field));
    if (name == L"title") return set(t.title ? g_lib.Str(t.title) : StemOf(g_lib.Str(t.file)));
    if (name == L"album artist") return set(g_lib.Str(t.albumArtist ? t.albumArtist : t.artist));
    if (name == L"edition") {
        const AlbumEdition* edition = AlbumEditionForTrack(id);
        return edition && set(EditionLabel(*edition));
    }
    if (name == L"edition_kind") {
        const AlbumEdition* edition = AlbumEditionForTrack(id);
        return edition && set(EditionKindLabel(edition->kind));
    }
    if (name == L"musicbrainz_albumid") {
        const AlbumEdition* edition = AlbumEditionForTrack(id);
        return edition && !edition->musicBrainzRelease.empty() && set(FromUtf8(edition->musicBrainzRelease));
    }
    if (name == L"musicbrainz_releasegroupid") {
        const AlbumGroup* group = AlbumGroupForTrack(id);
        return group && !group->musicBrainzReleaseGroup.empty() && set(FromUtf8(group->musicBrainzReleaseGroup));
    }
    if (name == L"year") {
        // The year part of a full date ("2005-03-28" -> "2005").
        std::wstring d = g_lib.Str(t.date);
        if (d.size() > 4 && iswdigit(d[0]) && iswdigit(d[1]) && iswdigit(d[2]) && iswdigit(d[3])) d.resize(4);
        return set(d);
    }
    if (name == L"rating_value") return num(t.rating);
    if (name == L"codec") return set(CodecName(t.codec));
    if (name == L"tracknumber") {
        if (!t.trackNo) return false;
        wchar_t buf[16];
        swprintf_s(buf, L"%02u", (unsigned)t.trackNo);
        return set(buf);
    }
    if (name == L"length") return t.durationMs && set(FormatDuration(t.Duration()));
    if (name == L"length_seconds") return num(t.durationMs / 1000);
    if (name == L"bitrate") return num(t.bitrate);
    if (name == L"samplerate") return num(t.sampleRate);
    if (name == L"channels") {
        if (!t.channels) return false;
        return set(t.channels == 1 ? L"mono" : t.channels == 2 ? L"stereo" : std::to_wstring(t.channels) + L"ch");
    }
    if (name == L"replaygain_track_gain" || name == L"replaygain_album_gain") {
        int16_t g = name == L"replaygain_track_gain" ? t.rgTrackGain : t.rgAlbumGain;
        if (g == kNoGain) return false;
        wchar_t buf[32];
        swprintf_s(buf, L"%+.2f dB", g / 100.0);
        return set(buf);
    }
    if (name == L"discnumber") return num(t.disc);
    if (name == L"totaldiscs") return num(t.discTotal);
    if (name == L"totaltracks") return num(t.trackTotal);
    if (name == L"bpm") return num(t.bpm);
    if (name == L"play_count") return num(t.playCount);
    if (name == L"skip_count") return num(t.skipCount);
    if (name == L"rating") {
        if (!t.rating) return false;
        wchar_t buf[8];
        swprintf_s(buf, t.rating % 20 ? L"%.1f" : L"%.0f", t.rating / 20.0);
        return set(buf);
    }
    if (name == L"rating_stars") return set(RatingStars(t.rating));
    if (name == L"last_played" || name == L"first_played" || name == L"added") {
        uint32_t when = name == L"added" ? t.added : name == L"first_played" ? t.firstPlayed : t.lastPlayed;
        if (!when) return false;
        // Local time, sortable as text: 2026-10-12 08:45
        ULARGE_INTEGER u;
        u.QuadPart = (when + 11644473600ull) * 10000000ull;
        FILETIME ft{u.LowPart, u.HighPart};
        SYSTEMTIME utc, local;
        FileTimeToSystemTime(&ft, &utc);
        SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
        wchar_t buf[32];
        swprintf_s(buf, L"%04u-%02u-%02u %02u:%02u", local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute);
        return set(buf);
    }
    if (name == L"filesize") return num(t.fileSize);
    if (name == L"filename") return set(StemOf(g_lib.Str(t.file)));
    if (name == L"path") return set(g_lib.Path(id));
    if (name == L"directory") return set(FileNameOf(g_lib.Str(t.dir)));
    if (name == L"ext") return set(ExtOf(g_lib.Str(t.file)));
    return false;
}

// ---------------------------------------------------------------------------
// Files

namespace {
const wchar_t* const kAudioExt[] = {L"mp3", L"mp2", L"mp1", L"m4a", L"m4b", L"mp4", L"aac", L"adts", L"flac", L"fla", L"wav", L"wave",
                                    L"wma", L"asf", L"ogg", L"oga", L"opus", L"aif", L"aiff", L"aifc", L"ac3", L"ape", L"wv",
                                    L"mka", L"alac", L"spc", L"nsf", L"nsfe", L"gbs", L"vgm", L"vgz", L"gym", L"sid", L"cord",
                                    L"dsf", L"dff", L"mod", L"s3m", L"xm", L"it", L"mid", L"midi", L"kar", L"rmi"};

// The songs in a ZIP: its audio members, each multi-song rip expanded into its songs.
void CollectArchiveAudio(const std::wstring& zip, std::vector<std::wstring>& out) {
    std::vector<std::wstring> members;
    ListArchive(zip, members);
    for (auto& m : members) {
        if (!IsSupportedAudioFile(m)) continue;
        auto tracks = ExpandSubtracks(m);
        if (tracks.empty()) out.push_back(std::move(m));
        else out.insert(out.end(), tracks.begin(), tracks.end());
    }
}

// Recursively expands audio files under dir, standing in for std::filesystem's
// recursive_directory_iterator with skip_permission_denied: folder links are not followed (they
// could loop) and folders that cannot be opened are skipped.
bool CollectFolderAudio(const std::wstring& dir, std::vector<std::wstring>& out, const std::atomic<bool>* cancel, unsigned depth) {
    if (depth > 128 || (cancel && *cancel)) return false;
    // A drive root ("D:\") already ends in a separator; don't double it.
    std::wstring prefix = dir.empty() || dir.back() == L'\\' || dir.back() == L'/' ? dir : dir + kPathSep;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((prefix + L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::optional<CueFolder> folder;
    bool ok = true;
    do {
        if (cancel && *cancel) { ok = false; break; }
        const wchar_t* name = fd.cFileName;
        if (name[0] == L'.' && (!name[1] || (name[1] == L'.' && !name[2]))) continue;
        std::wstring path = prefix + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) ok = CollectFolderAudio(path, out, cancel, depth + 1) && ok;
        } else if (IsSupportedAudioFile(path)) {
            if (!folder) folder.emplace(DirectoryOf(path));
            auto tracks = folder->Expand(path);
            if (tracks.empty()) out.push_back(std::move(path));
            else out.insert(out.end(), tracks.begin(), tracks.end());
        } else if (IsArchiveFile(path)) {
            CollectArchiveAudio(path, out);
        }
    } while (FindNextFileW(h, &fd));
    DWORD error = GetLastError();
    FindClose(h);
    return ok && error == ERROR_NO_MORE_FILES;
}
}  // namespace

bool IsNativeAudioType(const std::wstring& ext) {
    return std::any_of(std::begin(kAudioExt), std::end(kAudioExt), [&](const wchar_t* e) { return ext == e; });
}

bool IsSupportedAudioFile(const std::wstring& path) {
    std::wstring ext = ExtOf(AudioFileOf(path));  // a sub-track is as playable as its file
    return IsNativeAudioType(ext) || HelperForExtension(ext) != nullptr;  // or a helper program plays it (helpers.h)
}

bool IsPlaylistFile(const std::wstring& path) {
    std::wstring ext = ExtOf(path);
    return ext == L"m3u" || ext == L"m3u8";
}

std::wstring OpenFileFilterSpec() {
    std::wstring spec;
    for (auto e : kAudioExt) spec += (spec.empty() ? L"*." : L";*.") + std::wstring(e);
    spec += HelperFileTypes();
    return spec + L";*.zip";
}

bool CollectAudioFiles(const std::wstring& input, std::vector<std::wstring>& out, const std::atomic<bool>* cancel) {
    if (cancel && *cancel) return false;
    std::wstring path = NormalizeMediaPath(input);
    if (path.empty()) return false;
    DWORD attrs = GetFileAttributesW(AudioFileOf(path).c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        // A song inside a ZIP, from a playlist or the command line.
        if (!IsSupportedAudioFile(path) || !IsArchiveMember(path)) return false;
        auto tracks = ExpandSubtracks(path);
        if (tracks.empty()) out.push_back(path);
        else out.insert(out.end(), tracks.begin(), tracks.end());
        return true;
    }
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        size_t start = out.size();
        bool ok = CollectFolderAudio(path, out, cancel, 0);
        SortObjects(out.begin() + (ptrdiff_t)start, out.end(),
                    [](const std::wstring& a, const std::wstring& b) { return StrCmpLogicalW(a.c_str(), b.c_str()) < 0; });
        return ok;
    }
    if (IsPlaylistFile(path)) return ImportM3U(path, out);
    if (IsCueFile(path)) return ExpandCueFile(path, out);
    if (IsArchiveFile(path)) {
        size_t start = out.size();
        CollectArchiveAudio(path, out);
        return out.size() > start;
    }
    if (!IsSupportedAudioFile(path)) return false;
    if (IsSubtrack(path)) { out.push_back(path); return true; }
    auto tracks = ExpandSubtracks(path);
    if (tracks.empty()) out.push_back(path);
    else out.insert(out.end(), tracks.begin(), tracks.end());
    return true;
}
