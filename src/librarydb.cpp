// Loads and saves library.cdb, the one file holding the whole library, its stats and playlists.
#include "sort.h"
// library.cdb: the library, its stats and all playlists in one compact binary file.
//
//   "CDLB" u32 version
//   varint count, then count strings (varint length + UTF-8); string 0 is the empty string
//   varint count, then count tracks (fields below, strings as string indices)
//   varint count, then count playlists (kind, name, rules, varint n + n track indices)
//   version 2 only: sections, each varint tag, varint byte length, payload; tag 0 ends the list
//     1 = the play log (playlog.cpp), 2 = cordinal points (points.cpp). Readers skip unknown tags.
//
// Integers are LEB128 varints (signed ones zigzag-encoded) so typical libraries stay small.
// Saving writes a temp file and swaps it in with ReplaceFile, keeping the previous file as
// library.cdb.bak. A file that fails to parse is set aside instead of being overwritten.
#include "library.h"
#include "library_archive.h"
#include "playlog.h"
#include "points.h"
#include "sections.h"
#include "streamfile.h"
#include <climits>

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>

namespace {

constexpr uint32_t kVersion = 2;
constexpr uint64_t kMaxSectionBytes = 1ull << 30;

// The payloads of the sections a file carries, copied out of the file.
struct Sections {
    std::string playLog, points, audioHash, views, loudness, albums, firstPlayed;
    bool hasPlayLog = false, hasPoints = false, hasAudioHash = false, hasViews = false, hasLoudness = false,
         hasAlbums = false, hasFirstPlayed = false;
};

// Optional version-2 section: old readers skip it, so the database format stays compatible.
std::string EncodeViews(const std::vector<Playlist>& playlists, const std::vector<uint32_t>& newId) {
    std::string out;
    ByteWriter w(out);
    w.U(playlists.size());
    for (const auto& pl : playlists) {
        auto value = [&](TrackId id) {
            return pl.Editable() || id == UINT32_MAX ? id : id < newId.size() ? newId[id] : UINT32_MAX;
        };
        w.Bytes(ToUtf8(pl.position.search.substr(0, 4096)));
        w.I(pl.sortColumn);
        w.U(pl.sortAscending);
        w.U(value(pl.position.focus));
        w.U(value(pl.position.top));
        w.U(value(pl.position.expanded));
        w.U((uint32_t)std::clamp(pl.position.canvasScroll, 0, INT_MAX / 2));
        w.U(pl.position.selected.size());
        for (TrackId id : pl.position.selected) w.U(value(id));
    }
    return out;
}

bool DecodeViews(std::string_view data, uint64_t playlistCount, uint64_t trackCount,
                 std::vector<Playlist>* playlists, const std::vector<TrackId>* ids) {
    ByteReader r(data);
    if (r.U(playlistCount) != playlistCount) return false;
    for (size_t i = 0; i < playlistCount && r.ok; ++i) {
        auto search = r.Bytes(16384);
        if (!search.empty() && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, search.data(), (int)search.size(), nullptr, 0)) return false;
        int64_t column = r.I();
        bool ascending = r.U(1) != 0;
        auto value = [&] {
            uint32_t id = r.Unsigned<uint32_t>();
            if (playlists && !(*playlists)[i].Editable() && id != UINT32_MAX) {
                if (id >= trackCount) { r.ok = false; return UINT32_MAX; }
                if (ids) id = (*ids)[id];
            }
            return id;
        };
        PlaylistPosition pos;
        pos.focus = value();
        pos.top = value();
        pos.expanded = value();
        pos.canvasScroll = (int)r.U(INT_MAX / 2);
        uint64_t count = r.U(data.size());
        if (playlists) pos.selected.reserve((size_t)count);
        for (uint64_t j = 0; j < count && r.ok; ++j) {
            uint32_t id = value();
            if (playlists) pos.selected.push_back(id);
        }
        if (playlists) {
            auto& pl = (*playlists)[i];
            pos.search = FromUtf8(search);
            pl.position = std::move(pos);
            pl.sortColumn = column >= -2 && column <= 24 ? (int)column : -1;
            pl.sortAscending = ascending;
            if (pl.Editable()) SanitizePlaylistPosition(pl);
        }
    }
    return r.ok && r.Done();
}

// The audio hash section: varint count, then per track the saved index (as the gap from the
// previous one) and the hash. Only tracks with a known hash (FLAC) are listed.
std::string EncodeAudioHashes(const Library& library, const std::vector<uint32_t>& newId) {
    std::string out;
    ByteWriter w(out);
    std::vector<std::pair<uint32_t, uint64_t>> items;
    for (TrackId id = 0; id < (TrackId)library.Count(); ++id)
        if (newId[id] != UINT32_MAX && library[id].audioHash) items.push_back({newId[id], library[id].audioHash});
    w.U(items.size());
    uint32_t previous = 0;
    for (auto& [index, hash] : items) {  // saved indices ascend with the library's ids
        w.U(index - previous);
        w.U(hash);
        previous = index;
    }
    return out;
}

// Applies the hashes when a library is supplied (after validation), else only checks the section.
bool DecodeAudioHashes(std::string_view data, uint64_t trackCount, const std::vector<TrackId>* savedToId, Library* library) {
    ByteReader r(data);
    uint64_t count = r.U(trackCount);
    uint64_t index = 0;
    for (uint64_t i = 0; i < count && r.ok; ++i) {
        index += r.U(trackCount);
        uint64_t hash = r.U();
        if (!r.ok || index >= trackCount) return false;
        if (library) (*library)[savedToId ? (*savedToId)[(size_t)index] : (TrackId)index].audioHash = hash;
    }
    return r.ok && r.Done();
}

// The loudness section: varint count, then per track the saved index (as the gap from the
// previous one) and Track::rgHigh. Only measured tracks are listed.
std::string EncodeLoudness(const Library& library, const std::vector<uint32_t>& newId) {
    std::string out;
    ByteWriter w(out);
    std::vector<std::pair<uint32_t, uint8_t>> items;
    for (TrackId id = 0; id < (TrackId)library.Count(); ++id)
        if (newId[id] != UINT32_MAX && library[id].rgHigh) items.push_back({newId[id], library[id].rgHigh});
    w.U(items.size());
    uint32_t previous = 0;
    for (auto& [index, high] : items) {
        w.U(index - previous);
        w.U(high);
        previous = index;
    }
    return out;
}

// Applies the values when a library is supplied (after validation), else only checks the section.
bool DecodeLoudness(std::string_view data, uint64_t trackCount, const std::vector<TrackId>* savedToId, Library* library) {
    ByteReader r(data);
    uint64_t count = r.U(trackCount);
    uint64_t index = 0;
    for (uint64_t i = 0; i < count && r.ok; ++i) {
        index += r.U(trackCount);
        uint64_t high = r.U(255);
        if (!r.ok || index >= trackCount) return false;
        if (library) (*library)[savedToId ? (*savedToId)[(size_t)index] : (TrackId)index].rgHigh = (uint8_t)high;
    }
    return r.ok && r.Done();
}

// First-play dates are optional so older players can still read the same database.
std::string EncodeFirstPlayed(const Library& library, const std::vector<uint32_t>& newId) {
    std::string out;
    ByteWriter w(out);
    for (TrackId id = 0; id < library.Count(); ++id) if (newId[id] != UINT32_MAX && library[id].firstPlayed) {
        w.U(newId[id]);
        w.U(library[id].firstPlayed);
    }
    return out;
}

bool DecodeFirstPlayed(std::string_view data, uint64_t trackCount, const std::vector<TrackId>* ids, Library* library) {
    ByteReader r(data);
    uint64_t previous = 0;
    bool any = false;
    while (!r.Done() && r.ok) {
        uint64_t id = r.U(), time = r.U(UINT32_MAX);
        if (!r.ok || id >= trackCount || (any && id <= previous) || !time) return false;
        if (library) (*library)[ids ? (*ids)[(size_t)id] : (TrackId)id].firstPlayed = (uint32_t)time;
        previous = id;
        any = true;
    }
    return r.ok;
}

// The album section: varint count, then per measured album its gain, peak, levelling (tenths of
// a dB), member count and the members as saved-index gaps. An album record is what lets a
// measured album keep its gain on the next start instead of being worked out again.
std::string EncodeAlbumMeasurements(const Library& library, const std::map<AlbumId, AlbumMeasurement>& measurements,
                                    const std::vector<uint32_t>& newId) {
    std::string out;
    ByteWriter w(out);
    struct Item {
        const AlbumMeasurement* album;
        std::vector<uint32_t> members;
    };
    std::vector<Item> items;
    for (const auto& [key, album] : measurements) {
        // Only a measurement that still describes its live tracks is written: a record left
        // behind by a previous library (or a cleared one) would claim the wrong album.
        Item item{&album, {}};
        bool kept = !album.members.empty();
        if (kept && (album.members.front() >= library.Count() || AlbumKeyOf(library[album.members.front()]) != key))
            kept = false;
        for (TrackId id : album.members) {
            if (id >= library.Count() || !(library[id].flags & Track::kRgAlbumMeasured) || library[id].rgAlbumGain != album.gain ||
                library[id].rgAlbumPeak != album.peak) kept = false;
            if (id < newId.size() && newId[id] != UINT32_MAX) item.members.push_back(newId[id]);
            else kept = false;
        }
        if (kept) items.push_back(std::move(item));
    }
    w.U(items.size());
    for (auto& item : items) {
        const AlbumMeasurement& a = *item.album;
        SortRecords(item.members.begin(), item.members.end());
        w.I(a.gain);
        w.U(a.peak);
        w.U(a.dynamics);
        w.U(item.members.size());
        uint32_t previous = 0;
        for (uint32_t index : item.members) {
            w.U(index - previous);
            previous = index;
        }
    }
    return out;
}

// Applies the records when a library is supplied (after validation), else only checks the section. Members are
// saved track indices; the album key comes from the first member's live tags, so a record whose
// tracks have since been re-tagged simply stops matching and is worked out again.
bool DecodeAlbumMeasurements(std::string_view data, uint64_t trackCount, const std::vector<TrackId>* savedToId,
                             Library* library, std::map<AlbumId, AlbumMeasurement>* measurements = nullptr) {
    ByteReader r(data);
    uint64_t count = r.U(trackCount);
    std::vector<std::pair<AlbumId, AlbumMeasurement>> albums;
    for (uint64_t i = 0; i < count && r.ok; ++i) {
        int64_t gain = r.I();
        uint64_t peak = r.U(65535), dynamics = r.U(255), members = r.U(trackCount);
        if (!r.ok || gain < INT16_MIN || gain > INT16_MAX || !members) return false;
        AlbumMeasurement album{(int16_t)gain, (uint16_t)peak, (uint8_t)dynamics, {}};
        uint64_t index = 0;
        for (uint64_t k = 0; k < members && r.ok; ++k) {
            index += r.U(trackCount);
            if (!r.ok || index >= trackCount) return false;
            if (library) {
                TrackId id = savedToId ? (*savedToId)[(size_t)index] : (TrackId)index;
                if (id == kInvalidTrack) return false;  // a record whose member was dropped
                album.members.push_back(id);
            }
        }
        if (library) albums.emplace_back(AlbumKeyOf((*library)[album.members.front()]), std::move(album));
    }
    if (!r.ok || !r.Done()) return false;
    if (library)
        for (auto& [key, album] : albums) (*measurements)[key] = std::move(album);
    return true;
}

class Writer {
public:
    Writer(const std::wstring& file, std::wstring& error) : file_(file), error_(error) {
        static std::atomic<unsigned> serial{0};
        temp_ = file + L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(++serial);
        handle_ = CreateFileW(temp_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        ok_ = handle_ != INVALID_HANDLE_VALUE;
        if (!ok_) Fail(L"Couldn't create the temporary library file");
    }
    ~Writer() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            DeleteFileW(temp_.c_str());
        }
    }
    void U(uint64_t v) {
        do {
            uint8_t b = v & 0x7F;
            v >>= 7;
            buf_[used_++] = (char)(b | (v ? 0x80 : 0));
            if (used_ == buf_.size()) Flush();
        } while (v);
    }
    // Signed values are zigzag-encoded so small negatives stay one byte.
    void I(int64_t v) { U(((uint64_t)v << 1) ^ (uint64_t)-(v < 0)); }
    void Bytes(std::string_view s) {
        U(s.size());
        Raw(s);
    }
    void Raw(std::string_view s) {
        while (ok_ && !s.empty()) {
            size_t n = std::min(s.size(), buf_.size() - used_);
            memcpy(buf_.data() + used_, s.data(), n);
            used_ += n;
            s.remove_prefix(n);
            if (used_ == buf_.size()) Flush();
        }
    }
    bool Commit() {
        Flush();
        if (!ok_) return false;
        if (!FlushFileBuffers(handle_)) { Fail(L"Couldn't flush the library file"); return false; }
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
        // A failed replacement must not bypass backup creation.
        if (GetFileAttributesW(file_.c_str()) != INVALID_FILE_ATTRIBUTES)
            ok_ = ReplaceFileW(file_.c_str(), temp_.c_str(), (file_ + L".bak").c_str(), REPLACEFILE_IGNORE_MERGE_ERRORS,
                               nullptr, nullptr) != FALSE;
        else
            ok_ = MoveFileExW(temp_.c_str(), file_.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
        if (!ok_) {
            Fail(L"Couldn't replace the library file and its backup");
            DeleteFileW(temp_.c_str());
        }
        return ok_;
    }

private:
    void Fail(const wchar_t* action) {
        const DWORD code = GetLastError();
        wchar_t message[512]{};
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                       message, (DWORD)std::size(message), nullptr);
        error_ = std::wstring(action) + L" (" + std::to_wstring(code) + L"): " + message;
        while (!error_.empty() && (error_.back() == L'\r' || error_.back() == L'\n')) error_.pop_back();
        ok_ = false;
    }
    void Flush() {
        DWORD written = 0;
        if (ok_ && used_) {
            if (!WriteFile(handle_, buf_.data(), (DWORD)used_, &written, nullptr)) Fail(L"Couldn't write the library file");
            else if (written != used_) {
                error_ = L"Couldn't write the complete library file.";
                ok_ = false;
            }
        }
        used_ = 0;
    }
    std::wstring file_, temp_;
    std::wstring& error_;
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    std::array<char, 64 * 1024> buf_;
    size_t used_ = 0;
    bool ok_ = false;
};

// Buffered reader over a FileSource with a 64 KiB window; any overrun or malformed varint latches
// ok=false and parks the cursor at EOF, so callers only need to check ok at the end.
class Reader {
public:
    explicit Reader(FileSource& file) : file_(file) {}
    bool ok = true;
    size_t Remaining() const { return (size_t)(file_.Size() - p_); }
    uint64_t U(uint64_t max = UINT64_MAX) {
        uint64_t v = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            std::string_view byte = Raw(1);
            if (!ok) return 0;
            uint8_t c = (uint8_t)byte[0];
            if (shift == 63 && c > 1) return Fail();
            v |= (uint64_t)(c & 0x7F) << shift;
            if (!(c & 0x80)) return v <= max ? v : Fail();
        }
        return Fail();
    }
    template <typename T> T Unsigned() { return (T)U(std::numeric_limits<T>::max()); }
    int64_t I() {
        uint64_t v = U();
        return (int64_t)(v >> 1) ^ -(int64_t)(v & 1);
    }
    int16_t Gain() {
        int64_t v = I();
        if (v < INT16_MIN || v > INT16_MAX) Fail();
        return ok ? (int16_t)v : 0;
    }
    std::string_view Bytes() {
        uint64_t n = U();
        if (!ok || n > Remaining()) return Fail(), std::string_view();
        return Raw((size_t)n);
    }
    void SkipBytes() {
        uint64_t n = U();
        if (!ok || n > Remaining()) Fail();
        else p_ += (size_t)n;
    }
    std::string_view Raw(size_t n) {
        if (!ok || n > Remaining()) return Fail(), std::string_view();
        if (!n) return {};
        if (p_ < cacheStart_ || p_ >= cacheStart_ + cached_) {
            cacheStart_ = p_;
            cached_ = std::min(Remaining(), cache_.size());
            if (!file_.Read(p_, cached_, cache_.data())) return Fail(), std::string_view();
        }
        std::string_view s;
        if (n <= cacheStart_ + cached_ - p_) {
            s = {cache_.data() + (p_ - cacheStart_), n};
        } else {
            if (!file_.Read(p_, n, scratch_)) return Fail(), std::string_view();
            s = scratch_;
        }
        p_ += n;
        return s;
    }

private:
    uint64_t Fail() {
        ok = false;
        p_ = (size_t)file_.Size();
        return 0;
    }
    FileSource& file_;
    std::array<char, 64 * 1024> cache_;
    std::string scratch_;  // only strings crossing the read window; consumed immediately
    size_t cacheStart_ = 0, cached_ = 0;
    size_t p_ = 0;
};

void CompactSortedHandles(std::vector<StringPool::Handle>& strings, size_t& sorted) {
    if (strings.size() <= sorted) return;
    SortRecords(strings.begin() + (ptrdiff_t)sorted, strings.end());
    strings.erase(std::unique(strings.begin() + (ptrdiff_t)sorted, strings.end()), strings.end());
    std::inplace_merge(strings.begin(), strings.begin() + (ptrdiff_t)sorted, strings.end());
    strings.erase(std::unique(strings.begin(), strings.end()), strings.end());
    sorted = strings.size();
}

// Reads the section list after the playlists. Each payload is copied out before the next read.
bool ReadSections(Reader& r, Sections& out) {
    for (;;) {
        uint64_t tag = r.U();
        if (!r.ok) return false;
        if (tag == kSectionEnd) return true;
        uint64_t length = r.U(r.Remaining());
        if (!r.ok || length > kMaxSectionBytes) return false;
        std::string_view body = r.Raw((size_t)length);
        if (!r.ok) return false;
        // A repeated tag is malformed; the first copy wins.
        if ((tag == kSectionPlayLog && out.hasPlayLog) || (tag == kSectionPoints && out.hasPoints) ||
            (tag == kSectionAudioHash && out.hasAudioHash) || (tag == kSectionPlaylistViews && out.hasViews) ||
            (tag == kSectionLoudness && out.hasLoudness) || (tag == kSectionAlbumMeasurement && out.hasAlbums) ||
            (tag == kSectionFirstPlayed && out.hasFirstPlayed)) return false;
        if (tag == kSectionPlaylistViews) {
            out.views.assign(body);
            out.hasViews = true;
        } else if (tag == kSectionPlayLog && !out.hasPlayLog) {
            out.playLog.assign(body);
            out.hasPlayLog = true;
        } else if (tag == kSectionPoints && !out.hasPoints) {
            out.points.assign(body);
            out.hasPoints = true;
        } else if (tag == kSectionAudioHash && !out.hasAudioHash) {
            out.audioHash.assign(body);
            out.hasAudioHash = true;
        } else if (tag == kSectionLoudness) {
            out.loudness.assign(body);
            out.hasLoudness = true;
        } else if (tag == kSectionFirstPlayed) {
            out.firstPlayed.assign(body);
            out.hasFirstPlayed = true;
        } else if (tag == kSectionAlbumMeasurement) {
            out.albums.assign(body);
            out.hasAlbums = true;
        }
    }
}

// Walks the whole file without building anything, returning the counts. Every section is checked
// here, so LoadLibrary can decode afterwards knowing it cannot fail on bad input.
bool ValidateLibrary(FileSource& input, uint64_t& stringCount, uint64_t& trackCount, uint64_t& playlistCount,
                     uint32_t& version) {
    Reader r(input);
    std::string_view head = r.Raw(8);
    if (r.ok) memcpy(&version, head.data() + 4, 4);
    if (!r.ok || head.substr(0, 4) != "CDLB" || (version != 1 && version != kVersion)) return false;
    stringCount = r.U(r.Remaining());
    std::vector<uint32_t> pathLengths((size_t)stringCount + 1, UINT32_MAX);
    pathLengths[0] = 0;
    for (uint64_t i = 0; i < stringCount && r.ok; ++i) {
        auto text = r.Bytes();
        if (text.size() > (1u << 20) || (!text.empty() &&
            !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), (int)text.size(), nullptr, 0))) return false;
        if (std::none_of(text.begin(), text.end(), [](unsigned char c) { return c < 32; })) {
            int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), (int)text.size(), nullptr, 0);
            if (length <= 32767) pathLengths[(size_t)i + 1] = (uint32_t)length;
        }
    }
    trackCount = r.U(r.Remaining() / 34);
    for (uint64_t i = 0; i < trackCount && r.ok; ++i) {
        size_t dir = (size_t)r.U(stringCount), file = (size_t)r.U(stringCount);
        if (!r.ok || !file || !pathLengths[file] || pathLengths[dir] == UINT32_MAX || pathLengths[file] == UINT32_MAX ||
            (uint64_t)pathLengths[dir] + pathLengths[file] + 1 > 32767) return false;
        for (int field = 0; field < 10; ++field) r.U(stringCount);
        r.U();
        r.U();
        for (int u = 0; u < 6; ++u) r.Unsigned<uint32_t>();
        for (int u = 0; u < 4; ++u) r.Unsigned<uint16_t>();
        for (int u = 0; u < 3; ++u) r.Unsigned<uint8_t>();
        r.U((uint64_t)kLastCodec);
        r.U(100);
        for (int u = 0; u < 3; ++u) r.Unsigned<uint16_t>();
        r.Gain();
        r.Gain();
    }
    playlistCount = r.U(r.Remaining() / 4);
    for (uint64_t i = 0; i < playlistCount && r.ok; ++i) {
        r.U((uint64_t)PlaylistKind::Auto);
        r.SkipBytes();
        r.SkipBytes();
        uint64_t items = r.U(r.Remaining());
        for (uint64_t k = 0; k < items && r.ok; ++k) {
            if (!trackCount) r.ok = false;
            else r.U(trackCount - 1);
        }
    }
    if (version >= 2) {
        Sections sections;
        if (!ReadSections(r, sections)) return false;
        std::vector<PlayEvent> log;
        PointsState points;
        if (sections.hasPlayLog && !DecodePlayLog(sections.playLog, trackCount, nullptr, log)) return false;
        if (sections.hasPoints && !DecodePoints(sections.points, trackCount, nullptr, points)) return false;
        if (sections.hasAudioHash && !DecodeAudioHashes(sections.audioHash, trackCount, nullptr, nullptr)) return false;
        if (sections.hasLoudness && !DecodeLoudness(sections.loudness, trackCount, nullptr, nullptr)) return false;
        if (sections.hasAlbums && !DecodeAlbumMeasurements(sections.albums, trackCount, nullptr, nullptr)) return false;
        if (sections.hasFirstPlayed && !DecodeFirstPlayed(sections.firstPlayed, trackCount, nullptr, nullptr)) return false;
        if (sections.hasViews && !DecodeViews(sections.views, playlistCount, trackCount, nullptr, nullptr)) return false;
    }
    return r.ok && r.Remaining() == 0;
}

}  // namespace

static bool WriteDatabase(const std::wstring& file, const Library& library,
                          const std::vector<Playlist>& playlists, const std::vector<PlayEvent>& log,
                          const PointsState* points, const std::map<AlbumId, AlbumMeasurement>& measurements,
                          std::wstring* error) {
    std::wstring ignored;
    std::wstring& failure = error ? *error : ignored;
    failure.clear();
    // Keep library tracks (also missing ones: their stats survive an unplugged drive) and
    // anything a stored playlist refers to. View playlists are recomputed on load.
    // newId: old id -> saved index, or UINT32_MAX for a track that is dropped.
    std::vector<uint32_t> newId(library.Count(), UINT32_MAX);
    for (TrackId id = 0; id < (TrackId)library.Count(); ++id)
        if ((library[id].flags & (Track::kInLibrary | Track::kInbox | Track::kMissing | Track::kExcluded | Track::kCollected)) || library[id].rating ||
            library[id].playCount || library[id].skipCount || library[id].firstPlayed || library[id].lastPlayed) newId[id] = 0;
    for (const auto& e : log) if (e.id < newId.size()) newId[e.id] = 0;
    if (points) for (const auto& e : points->recent) if (e.id < newId.size()) newId[e.id] = 0;
    for (const auto& pl : playlists)
        if (pl.Editable())
            for (TrackId id : pl.items) {
                if (id >= newId.size()) {
                    failure = L"A playlist contains an invalid track reference.";
                    return false;
                }
                newId[id] = 0;
            }

    // A sorted handle dictionary replaces heap nodes per string; binary search needs no
    // second index. Incremental compaction keeps `strings` bounded by unique handles + 8K.
    std::vector<StringPool::Handle> strings;
    size_t sorted = 0;
    uint32_t count = 0;
    for (TrackId id = 0; id < (TrackId)library.Count(); ++id) {
        if (newId[id] == UINT32_MAX) continue;
        newId[id] = count++;
        const Track& t = library[id];
        for (auto h : {t.dir, t.file, t.title, t.artist, t.album, t.albumArtist, t.genre, t.date, t.composer, t.comment,
                       t.grouping, t.label})
            if (h) strings.push_back(h);
        if (strings.size() - sorted >= 8192) CompactSortedHandles(strings, sorted);
    }
    CompactSortedHandles(strings, sorted);
    strings.shrink_to_fit();
    auto str = [&](StringPool::Handle h) -> uint32_t {
        return h ? (uint32_t)(std::lower_bound(strings.begin(), strings.end(), h) - strings.begin()) + 1 : 0;
    };
    Writer w(file, failure);
    w.Raw("CDLB");
    w.Raw(std::string_view(reinterpret_cast<const char*>(&kVersion), 4));
    w.U(strings.size());
    for (auto h : strings) w.Bytes(library.View(h));
    w.U(count);
    for (TrackId id = 0; id < (TrackId)library.Count(); ++id) {
        if (newId[id] == UINT32_MAX) continue;
        const Track& t = library[id];
        for (auto h : {t.dir, t.file, t.title, t.artist, t.album, t.albumArtist, t.genre, t.date, t.composer, t.comment,
                       t.grouping, t.label})
            w.U(str(h));
        for (uint64_t v : {t.fileSize, t.fileTime, (uint64_t)t.durationMs, (uint64_t)t.sampleRate, (uint64_t)t.added,
                           (uint64_t)t.lastPlayed, (uint64_t)t.playCount, (uint64_t)t.skipCount, (uint64_t)t.bitrate,
                           (uint64_t)t.trackNo, (uint64_t)t.trackTotal, (uint64_t)t.bpm, (uint64_t)t.disc,
                           (uint64_t)t.discTotal, (uint64_t)t.channels, (uint64_t)t.codec, (uint64_t)t.rating,
                           (uint64_t)(t.flags & ~Track::kFailed), (uint64_t)t.rgTrackPeak, (uint64_t)t.rgAlbumPeak})
            w.U(v);
        w.I(t.rgTrackGain);
        w.I(t.rgAlbumGain);
    }
    std::vector<StringPool::Handle>().swap(strings);
    w.U(playlists.size());
    for (const auto& pl : playlists) {
        w.U((uint64_t)pl.kind);
        w.Bytes(ToUtf8(pl.name));
        w.Bytes(pl.rules);
        if (!pl.Editable() && pl.sortColumn != -2) {
            w.U(0);
            continue;
        }
        std::vector<TrackId> kept;
        kept.reserve(pl.items.size());
        for (TrackId id : pl.items)
            if (id < newId.size() && newId[id] != UINT32_MAX) kept.push_back(id);
        w.U(kept.size());
        for (TrackId id : kept) w.U(newId[id]);
    }
    // Version 2 sections. The points section is written only once points have been granted, so
    // a file saved before that still gets its back pay on the next start.
    w.U(kSectionPlayLog);
    w.Bytes(EncodePlayLog(log, newId));
    if (points != nullptr) {
        w.U(kSectionPoints);
        w.Bytes(EncodePoints(*points, newId));
    }
    w.U(kSectionAudioHash);
    w.Bytes(EncodeAudioHashes(library, newId));
    w.U(kSectionLoudness);
    w.Bytes(EncodeLoudness(library, newId));
    w.U(kSectionAlbumMeasurement);
    w.Bytes(EncodeAlbumMeasurements(library, measurements, newId));
    w.U(kSectionPlaylistViews);
    w.Bytes(EncodeViews(playlists, newId));
    w.U(kSectionFirstPlayed);
    w.Bytes(EncodeFirstPlayed(library, newId));
    w.U(kSectionEnd);
    std::vector<uint32_t>().swap(newId);
    return w.Commit();
}

bool ReadLibraryArchive(const std::wstring& file, LibraryArchive& out) {
    FileSource input(file);
    if (!input.Valid() || input.Size() >= (1ull << 31)) return false;
    auto reject = [] { return false; };
    uint64_t stringCount = 0, trackCount = 0, playlistCount = 0;
    uint32_t version = 0;
    if (!ValidateLibrary(input, stringCount, trackCount, playlistCount, version)) {
        // Never let the next save overwrite a library we couldn't read.
        return reject();
    }
    // Decode into a replacement. An invalid path or failed read leaves the live state intact.
    Library pending;
    std::vector<Playlist> loaded;
    Reader r(input);
    r.Raw(8);
    uint64_t n = r.U();
    pending.Reserve((size_t)n, (size_t)trackCount);
    std::vector<StringPool::Handle> strings((size_t)n + 1, 0);
    for (uint64_t i = 0; i < n; ++i) strings[(size_t)i + 1] = pending.Intern(r.Bytes());
    auto str = [&]() -> StringPool::Handle { return strings[(size_t)r.U()]; };
    n = r.U();
    std::vector<TrackId> ids;
    for (uint64_t i = 0; i < n; ++i) {
        StringPool::Handle dir = str(), name = str();
        TrackId id;
        if (pending.View(name).find_first_of("\\/") == std::string_view::npos && (!dir || !pending.View(name).empty())) {
            id = pending.Add(dir, name);
        } else {
            std::string path(pending.View(dir));
            if (!path.empty()) path += (char)kPathSep;
            path.append(pending.View(name));
            id = pending.Add(FromUtf8(path));
        }
        if (id == UINT32_MAX) return reject();
        Track& t = pending[id];
        for (auto* h : {&t.title, &t.artist, &t.album, &t.albumArtist, &t.genre, &t.date, &t.composer, &t.comment,
                        &t.grouping, &t.label})
            *h = str();
        t.fileSize = r.U();
        t.fileTime = r.U();
        t.durationMs = r.Unsigned<uint32_t>();
        t.sampleRate = r.Unsigned<uint32_t>();
        t.added = r.Unsigned<uint32_t>();
        t.lastPlayed = r.Unsigned<uint32_t>();
        t.playCount = r.Unsigned<uint32_t>();
        t.skipCount = r.Unsigned<uint32_t>();
        t.bitrate = r.Unsigned<uint16_t>();
        t.trackNo = r.Unsigned<uint16_t>();
        t.trackTotal = r.Unsigned<uint16_t>();
        t.bpm = r.Unsigned<uint16_t>();
        t.disc = r.Unsigned<uint8_t>();
        t.discTotal = r.Unsigned<uint8_t>();
        t.channels = r.Unsigned<uint8_t>();
        t.codec = (Codec)r.U();
        t.rating = (uint8_t)r.U();
        t.flags = r.Unsigned<uint16_t>();
        t.rgTrackPeak = r.Unsigned<uint16_t>();
        t.rgAlbumPeak = r.Unsigned<uint16_t>();
        t.rgTrackGain = r.Gain();
        t.rgAlbumGain = r.Gain();
        // ids stays empty while saved order matches library order; only a path change (a re-Add
        // that merged or reordered tracks) forces the explicit remap table.
        if (!ids.empty() || id != (TrackId)i) {
            if (ids.empty()) {
                ids.resize((size_t)n);
                for (size_t k = 0; k < (size_t)i; ++k) ids[k] = (TrackId)k;
            }
            ids[(size_t)i] = id;
        }
    }
    std::vector<StringPool::Handle>().swap(strings);
    n = r.U();
    loaded.reserve((size_t)n);
    for (uint64_t i = 0; i < n; ++i) {
        Playlist pl;
        pl.kind = (PlaylistKind)r.U();
        pl.name = FromUtf8(r.Bytes());
        pl.rules = std::string(r.Bytes());
        uint64_t items = r.U();
        pl.items.resize((size_t)items);
        for (uint64_t k = 0; k < items; ++k) {
            size_t index = (size_t)r.U();
            pl.items[(size_t)k] = ids.empty() ? (TrackId)index : ids[index];
        }
        loaded.push_back(std::move(pl));
    }
    // Sections were validated above, so decoding them now cannot fail.
    Sections sections;
    if (version >= 2) ReadSections(r, sections);
    if (!r.ok || r.Remaining()) return reject();
    const std::vector<TrackId>* savedToId = ids.empty() ? nullptr : &ids;
    if (sections.hasViews && !DecodeViews(sections.views, playlistCount, trackCount, &loaded, savedToId)) return reject();
    LibraryArchive data;
    data.library = std::move(pending);
    data.playlists = std::move(loaded);
    std::vector<PlayEvent> log;
    if (sections.hasPlayLog) DecodePlayLog(sections.playLog, trackCount, savedToId, log);
    data.log = std::move(log);
    if (sections.hasAudioHash) DecodeAudioHashes(sections.audioHash, trackCount, savedToId, &data.library);
    if (sections.hasLoudness) DecodeLoudness(sections.loudness, trackCount, savedToId, &data.library);
    if (sections.hasAlbums) DecodeAlbumMeasurements(sections.albums, trackCount, savedToId, &data.library, &data.albums);
    data.hasPoints = sections.hasPoints;
    if (data.hasPoints) DecodePoints(sections.points, trackCount, savedToId, data.points);
    if (sections.hasFirstPlayed) DecodeFirstPlayed(sections.firstPlayed, trackCount, savedToId, &data.library);
    for (const auto& e : data.log) if (e.flags & PlayEvent::kCounted) {
        auto& first = data.library[e.id].firstPlayed;
        if (!first || e.time < first) first = e.time;
    }
    data.library.Touch();
    out = std::move(data);
    return true;
}

bool WriteLibraryArchive(const std::wstring& file, const LibraryArchive& data, std::wstring& error) {
    return WriteDatabase(file, data.library, data.playlists, data.log, data.hasPoints ? &data.points : nullptr,
                         data.albums, &error);
}

LibraryArchive CaptureLibraryArchive(const std::vector<Playlist>& playlists) {
    LibraryArchive data;
    for (TrackId id = 0; id < g_lib.Count(); ++id) data.library.ImportTrack(g_lib, id);
    data.playlists = playlists;
    for (auto& pl : data.playlists) {
        if (pl.Editable()) SanitizePlaylistPosition(pl);
        else {
            auto valid = [&](TrackId id) { return data.library.Contains(id); };
            std::erase_if(pl.position.selected, [&](TrackId id) { return !valid(id); });
            for (auto* id : {&pl.position.focus, &pl.position.top, &pl.position.expanded})
                if (!valid(*id)) *id = kInvalidTrack;
        }
    }
    data.log = PlayLog();
    data.hasPoints = PointsInitialized();
    data.points = CurrentPointsState();
    // Album keys contain pool handles; the snapshot has its own string pool.
    for (const auto& [key, album] : g_albumMeasurements) if (!album.members.empty())
        data.albums[AlbumKeyOf(data.library[album.members.front()])] = album;
    return data;
}

bool SaveLibrary(const std::wstring& file, const std::vector<Playlist>& playlists, std::wstring* error) {
    return WriteDatabase(file, g_lib, playlists, PlayLog(), PointsInitialized() ? &CurrentPointsState() : nullptr,
                         g_albumMeasurements, error);
}

bool LoadLibrary(const std::wstring& file, std::vector<Playlist>& playlists) {
    LibraryArchive data;
    if (!ReadLibraryArchive(file, data)) {
        // Keep the existing live-profile recovery contract, but never mutate backup input.
        static std::atomic<unsigned> serial{0};
        std::wstring original = file + L".bad-" + std::to_wstring(UnixNow()) + L"-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++serial);
        MoveFileExW(file.c_str(), original.c_str(), 0);
        return false;
    }
    g_lib = std::move(data.library);
    playlists = std::move(data.playlists);
    SetPlayLog(std::move(data.log));
    g_albumMeasurements = std::move(data.albums);
    if (data.hasPoints) SetPointsState(std::move(data.points));
    else ResetPoints();
    return true;
}
