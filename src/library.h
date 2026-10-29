// The media library: a compact track store, playlists and their files.
//
// Memory layout: every string (titles, artists, folders, file names) lives once in a
// deduplicated UTF-8 arena and is referred to by a 32-bit handle. Tracks occupy small fixed
// slabs, so growth never copies the library or reserves a fraction of it as unused records.
// A path is stored as folder handle + file name handle; playlists hold 32-bit track ids.
#pragma once
#include "tags.h"

#include <windows.h>

#include <atomic>
#include <deque>
#include <cmath>
#include <compare>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// Strings and paths

std::string ToUtf8(std::wstring_view s);
std::wstring FromUtf8(std::string_view s);
std::wstring FormatDuration(double seconds);
std::wstring FormatTotalDuration(double seconds);
std::wstring FileNameOf(const std::wstring& path);
std::wstring StemOf(const std::wstring& path);
std::wstring ExtOf(const std::wstring& path);  // lowercase, no dot
std::wstring DirectoryOf(const std::wstring& path);
// A folder tree's root and relative components, filename last. Uses the longest configured root;
// otherwise the containing folder. Accepts Windows and POSIX paths without resolving or opening them.
std::vector<std::wstring> FolderTreePath(std::wstring path, const std::vector<std::wstring>& roots);
// Absolute, with this platform's separators; {} when empty, too long or unusable.
std::wstring NormalizeMediaPath(const std::wstring& path);
// ReplayGain gains are stored as centi-dB, clamped to +/-320 dB; peaks as 1/20000 units.
int16_t EncodeReplayGain(float gain);
uint16_t EncodeReplayPeak(float peak);

// A track inside a file: an entry of a CUE sheet, or one song of a multi-song game music file
// (NSF, GBS, ...). Its path is the real file's path + "|" + the 1-based index, e.g.
// "D:\Music\Album.flac|3". '|' can't occur in Windows file names, so this never collides.
// Everything that opens, moves or writes files must use the real file (SplitSubtrack).
bool SplitSubtrack(std::wstring_view path, std::wstring& file, int& index);  // false: a plain file
std::wstring SubtrackPath(std::wstring_view file, int index);
inline bool IsSubtrack(std::wstring_view path) { return path.find(L'|') != std::wstring_view::npos; }

// ---------------------------------------------------------------------------
// String pool

class StringPool {
public:
    using Handle = uint32_t;  // 0 = empty string
    Handle Intern(std::string_view s);
    Handle Lookup(std::string_view s) const;  // 0 if `s` was never interned
    std::string_view Get(Handle h) const;
    void Reserve(size_t count);
    size_t Count() const { return count_; }
    size_t Bytes() const;

private:
    static constexpr size_t kBlock = 16 * 1024;
    std::vector<std::unique_ptr<char[]>> blocks_;  // fixed-size blocks: no reallocation copies
    size_t used_ = kBlock;                         // bytes used in the last block
    std::vector<Handle> table_;                    // open-addressing hash set of handles
    size_t count_ = 0;
    std::deque<std::string> large_;               // strings longer than a block (never in practice)
    Handle Store(std::string_view s);
    void Grow(size_t minCapacity = 0);
};

// ---------------------------------------------------------------------------
// Tracks

// The separator for paths the program builds (both are accepted when splitting on Windows).
#ifdef _WIN32
constexpr wchar_t kPathSep = L'\\';
#else
constexpr wchar_t kPathSep = L'/';
#endif

constexpr int16_t kNoGain = INT16_MIN;

// Seconds since 1970 (UTC), the unit of the date added / last played stats.
uint32_t UnixNow();

struct Track {
    StringPool::Handle dir = 0, file = 0;
    StringPool::Handle title = 0, artist = 0, album = 0, albumArtist = 0, genre = 0, date = 0;
    StringPool::Handle composer = 0, comment = 0, grouping = 0, label = 0;
    uint64_t fileSize = 0, fileTime = 0;  // FILETIME of the last write; a rescan re-reads tags when either changes
    uint32_t durationMs = 0;
    uint32_t sampleRate = 0;
    uint32_t added = 0, lastPlayed = 0;  // UnixNow() seconds, 0 = never
    uint32_t playCount = 0, skipCount = 0;
    uint16_t bitrate = 0;  // kbps
    uint16_t trackNo = 0, trackTotal = 0, bpm = 0;
    uint8_t disc = 0, discTotal = 0;
    uint8_t channels = 0;
    Codec codec = Codec::Unknown;
    uint8_t rating = 0;  // 0 = unrated, else 10..100 (half stars)
    // How far the loud parts sit above the integrated loudness (LoudnessMeter::HighLoudness),
    // as 1 + quarter-LU; 0 = not measured. Fits the padding before `flags`.
    uint8_t rgHigh = 0;
    uint16_t flags = 0;
    int16_t rgTrackGain = kNoGain, rgAlbumGain = kNoGain;  // centi-dB
    uint16_t rgTrackPeak = 0, rgAlbumPeak = 0;             // 1/20000 units, 0 = unknown
    uint64_t audioHash = 0;  // FLAC audio MD5 prefix (TagData::audioHash), 0 = unknown
    uint32_t firstPlayed = 0;  // first counted play, including imported statistics; 0 = unknown

    enum : uint16_t {
        kLoaded = 1,         // tags have been read
        kFailed = 2,         // couldn't be played this session (not saved)
        kInLibrary = 4,      // found in a library folder
        kInbox = 8,          // in the library but still waiting in the Inbox
        kMissing = 16,       // the file is gone; kept only for playlists and stats
        kCompilation = 32,
        kExcluded = 64,      // removed from the library by the user; folder scans leave it out
        kCollected = 128,    // its collection points were awarded (points.h); never awarded twice
        kRgScanned = 256,    // ReplayGain was measured by cordinal (not only imported from tags)
        kRgTruePeak = 512,   // ...with the 4x true-peak scanner, so the peak is current
        kTagFailed = 1024,   // failed reads stay retryable; never claim their tags were loaded
        kRgScanFailed = 2048, // no usable loudness; retry on file changes or an explicit scan
        kRgAlbumMeasured = 4096,  // rgAlbumGain/Peak belong to a saved album measurement
        kOriginal = 8192,    // a file kept after its conversion to .cord; out of the library unless shown
    };
    bool Loaded() const { return flags & kLoaded; }
    bool Failed() const { return flags & kFailed; }
    bool InLibrary() const { return (flags & (kInLibrary | kInbox | kMissing)) == kInLibrary; }
    double Duration() const { return durationMs / 1000.0; }
};
static_assert(sizeof(Track) <= 128, "keep tracks compact");

using TrackId = uint32_t;
constexpr TrackId kInvalidTrack = UINT32_MAX;

// Album identity for levelling: album artist (or artist), album (or folder) and date. Every disc
// of a multi-disc set shares the key. Handles are per-library, so a change of library or a
// compaction must remap anything keyed by them.
struct AlbumId {
    StringPool::Handle artist = 0, album = 0, date = 0;
    // Maps compare fields directly to keep the size-oriented build's lookup path short.
    constexpr bool operator<(const AlbumId& other) const {
        if (artist != other.artist) return artist < other.artist;
        if (album != other.album) return album < other.album;
        return date < other.date;
    }
    auto operator<=>(const AlbumId&) const = default;
};
static_assert(sizeof(AlbumId) == 3 * sizeof(StringPool::Handle));
inline AlbumId AlbumKeyOf(const Track& t) {
    return {t.albumArtist ? t.albumArtist : t.artist, t.album ? t.album : t.dir, t.date};
}

// A measured album: what its members play at, and the members that measurement covers. It is
// saved with the library, so a measured album keeps its gain after a restart instead of being
// worked out again (and a track added since, while it is unmeasured, does not take it away).
struct AlbumMeasurement {
    int16_t gain = kNoGain;  // centi-dB, the album's level
    uint16_t peak = 0;       // 1/20000 units, the album's loudest track peak
    uint8_t dynamics = 0;    // tenths of a dB held back from a lift, for the album's loud parts
    std::vector<TrackId> members;
};
extern std::map<AlbumId, AlbumMeasurement> g_albumMeasurements;

// Capture on the UI thread; read from a worker without touching Library's mutable indices.
// Pool bytes are immutable and stable through growth, retagging and renames. The snapshot
// lasts until this Library is replaced/destroyed; application workers are joined before that.
struct TrackPath {
    TrackId id;
    std::string_view dir, file;
    std::wstring Path() const;
};

// Stable addresses and at most 255 unused records, independent of library size. Unlike a
// geometrically growing vector, there is no moment when both an old and a new track array live.
class TrackStore {
public:
    TrackStore() = default;
    TrackStore(TrackStore&& other) noexcept : blocks_(std::move(other.blocks_)), size_(std::exchange(other.size_, 0)) {}
    TrackStore& operator=(TrackStore&& other) noexcept {
        if (this == &other) return *this;
        blocks_ = std::move(other.blocks_);
        size_ = std::exchange(other.size_, 0);
        return *this;
    }
    size_t size() const { return size_; }
    Track& operator[](size_t i) { return blocks_[i / kBlock][i % kBlock]; }
    const Track& operator[](size_t i) const { return blocks_[i / kBlock][i % kBlock]; }
    void push_back(const Track& track) {
        if (size_ % kBlock == 0) blocks_.push_back(std::make_unique<Track[]>(kBlock));
        (*this)[size_++] = track;
    }
    size_t Bytes() const { return blocks_.size() * kBlock * sizeof(Track) + blocks_.capacity() * sizeof(blocks_[0]); }

private:
    static constexpr size_t kBlock = 256;
    std::vector<std::unique_ptr<Track[]>> blocks_;
    size_t size_ = 0;
};

class Library {
public:
    // Returns the existing track for a path, or a new unloaded one.
    TrackId Add(const std::wstring& path);
    TrackId Add(StringPool::Handle dir, StringPool::Handle file);
    // The track for a path, or UINT32_MAX.
    TrackId Find(const std::wstring& path) const;
    // Points a track at a new file (after a move or rename).
    void Rename(TrackId id, const std::wstring& path);
    void Reserve(size_t strings, size_t tracks);
    Track& operator[](TrackId id) { return tracks_[id]; }
    const Track& operator[](TrackId id) const { return tracks_[id]; }
    size_t Count() const { return tracks_.size(); }
    bool Contains(TrackId id) const { return id < tracks_.size(); }
    // Copies a track from another library, re-interning its strings here; returns the local id.
    TrackId ImportTrack(const Library& source, TrackId id);
    // Keeps only the `live` ids, renumbering them densely; returns old id -> new id.
    std::vector<TrackId> Compact(const std::vector<TrackId>& live);
    size_t StringCount() const { return strings_.Count(); }

    std::wstring Path(TrackId id) const;
    TrackPath SnapshotPath(TrackId id) const {
        if (!Contains(id)) return {id, {}, {}};
        const Track& t = tracks_[id];
        return {id, strings_.Get(t.dir), strings_.Get(t.file)};
    }
    std::wstring Str(StringPool::Handle h) const { return FromUtf8(strings_.Get(h)); }
    std::string_view View(StringPool::Handle h) const { return strings_.Get(h); }
    StringPool::Handle Intern(std::string_view s) { return strings_.Intern(s); }
    StringPool::Handle InternW(std::wstring_view s) { return strings_.Intern(ToUtf8(s)); }

    void ApplyTags(TrackId id, const TagData& tags);
    // Groups of two or more library tracks (in the library or the Inbox) whose audio is identical,
    // by audioHash. Each group lists its tracks in ascending id order.
    std::vector<std::vector<TrackId>> FindDuplicateAudio() const;
    size_t MemoryBytes() const;

    // Bumped by every change that can alter what a library view shows, so views rebuild lazily.
    uint32_t Revision() const { return revision_; }
    void Touch() { ++revision_; }

private:
    TrackStore tracks_;
    StringPool strings_;
    std::vector<TrackId> byPath_;  // open-addressing index on (dir, file); empty = UINT32_MAX
    uint32_t revision_ = 1;
    void Index(TrackId id);
    void Unindex(TrackId id);
    size_t Slot(StringPool::Handle dir, StringPool::Handle file) const;
    TrackId FindKey(const std::wstring& key) const;
    void SplitPath(const std::wstring& path, StringPool::Handle& dir, StringPool::Handle& file);
};

// The one library. Mutated only on the UI thread; scanners and tag readers hand results back
// to it as messages rather than touching it themselves.
extern Library g_lib;

// Group a copied list of nonzero audio hashes off the UI thread; each group contains two or more
// track IDs, and both the IDs and the groups are sorted.
std::vector<std::vector<TrackId>> GroupDuplicateAudio(std::vector<std::pair<uint64_t, TrackId>> byHash);

// Static playlists hold tracks the user put there. The others are views: the Library and the
// Inbox are computed from track flags, an Auto playlist from its rules, and Now Playing is the
// snapshot of a view that playback started from (so changing a filter never changes what's next).
enum class PlaylistKind : uint8_t { Static, Library, Inbox, NowPlaying, Auto };

// Tab navigation state, persisted in the optional playlist-view database section.
struct PlaylistPosition {
    std::wstring search;
    // Editable lists use item indices so duplicate tracks stay distinct; views use track IDs.
    std::vector<TrackId> selected;
    TrackId focus = UINT32_MAX, top = UINT32_MAX, expanded = UINT32_MAX;
    int canvasScroll = 0;  // at 96 DPI
};

struct Playlist {
    std::wstring name;
    std::vector<TrackId> items;
    PlaylistKind kind = PlaylistKind::Static;
    std::string rules;  // Auto playlists: serialized rules (autoplaylist.h)
    // Remembered sort; views reapply it when their library revision changes.
    int sortColumn = -1;
    bool sortAscending = true;
    uint32_t builtRevision = 0;
    PlaylistPosition position;
    uint64_t identity = 0;
    bool Editable() const { return kind == PlaylistKind::Static || kind == PlaylistKind::NowPlaying; }
    bool IsView() const { return kind == PlaylistKind::Library || kind == PlaylistKind::Inbox || kind == PlaylistKind::Auto; }
};

// Title-formatting field lookup (%artist%, %title%, %length%, ...).
bool LookupField(TrackId id, const std::wstring& name, std::wstring& out);
// Stars for a 0..100 rating ("★★★½" style, empty when unrated).
std::wstring RatingStars(uint8_t rating);

// Files
// The file types cordinal plays itself (lowercase, no dot). On Windows, helper programs
// (helpers.h) may play others, and IsSupportedAudioFile counts those too.
bool IsNativeAudioType(const std::wstring& ext);
bool IsSupportedAudioFile(const std::wstring& path);
bool IsPlaylistFile(const std::wstring& path);
std::wstring OpenFileFilterSpec();
// Expands files, folders (recursively) and M3U playlists into audio file paths.
bool CollectAudioFiles(const std::wstring& path, std::vector<std::wstring>& out,
                       const std::atomic<bool>* cancel = nullptr);

// Persistence. library.cdb holds every library track with its stats, plus all playlists; the
// older playlists.wfl text format (Waveforge) is still read for the one-time import.
bool SaveLibrary(const std::wstring& file, const std::vector<Playlist>& playlists, std::wstring* error = nullptr);
// Replaces the library and playlists only after the entire file has been validated.
bool LoadLibrary(const std::wstring& file, std::vector<Playlist>& playlists);
bool LoadPlaylists(const std::wstring& file, std::vector<Playlist>& playlists);
bool ImportM3U(const std::wstring& file, std::vector<std::wstring>& paths);
struct M3UExportEntry { std::string info, path; };
// Capture on the library's owner thread; the immutable entries can be written on a worker.
bool CaptureM3U(const Playlist& playlist, std::vector<M3UExportEntry>& entries);
bool ExportM3U(const std::wstring& file, const std::vector<M3UExportEntry>& entries);
bool ExportM3U(const std::wstring& file, const Playlist& playlist);
// An .m3u8 for another device, as text: each track at the location given (relative to the
// playlist, '/'-separated), so the paths mean something there.
std::string M3UText(const std::vector<std::pair<TrackId, std::string>>& entries);

// Shared playlist/search rules, independent of native controls.
std::wstring CleanPlaylistName(std::wstring_view name);
std::wstring UniquePlaylistName(const std::vector<Playlist>& playlists, std::wstring_view base);
int RemapPlaylistIndex(int index, int from, int to);
bool MovePlaylist(std::vector<Playlist>& playlists, int from, int to);
std::vector<std::wstring> FilterWords(std::wstring_view filter);
bool MatchesTrack(const Library& library, TrackId id, const std::vector<std::wstring>& words);
// One immutable query over one library. Each field remembers its last pooled value,
// so adjacent tracks with shared tags reuse Unicode conversion and linguistic matches.
class TrackMatcher {
public:
    TrackMatcher(const Library& library, const std::vector<std::wstring>& words) : library_(library), words_(words) {}
    bool Matches(TrackId id);
private:
    struct Field {
        StringPool::Handle handle = 0;
        std::wstring text;
        uint64_t tested = 0, found = 0;
    };
    const Library& library_;
    const std::vector<std::wstring>& words_;
    Field fields_[13];
};
void SanitizePlaylistPosition(Playlist& playlist);
