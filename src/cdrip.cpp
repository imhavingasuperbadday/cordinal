// CD ripping (cdrip.h): the disc id, CD-Text, the two-pass verified track reader, the rip loop, and
// the Windows and Linux drives. Hashes of each sector, not the sectors, are kept between passes, so
// a track's verification costs 8 bytes a sector and the audio is held only one read at a time.
#include "cdrip.h"

#include "convert.h"
#include "cord.h"
#include "ipod.h"
#include "organize.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#include <fcntl.h>
#include <linux/cdrom.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace cdrip {
namespace {

// Sectors per drive read: under the 64 KB that every Windows and Linux CD driver accepts.
constexpr unsigned kChunk = 26;
// Reads of a disagreeing stretch before its sectors are kept as last read.
constexpr unsigned kRetries = 20;
// A data track in a second session starts this far after the audio session's lead-out.
constexpr uint32_t kSessionGap = 11400;
constexpr uint64_t kFailed = 0;  // a sector the drive couldn't read; real hashes are never 0

// A sector's fingerprint, eight bytes at a time; no two different reads of a sector are expected to
// share one by chance.
uint64_t Hash(const uint8_t* p) {
    uint64_t h = 0x243F6A8885A308D3ull;
    for (unsigned i = 0; i < kSectorBytes; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
    return h | 1;
}

int64_t FloorDiv(int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

std::string Latin1(std::string_view s) {
    std::string out;
    for (unsigned char c : s) {
        if (c < 0x80) out += (char)c;
        else {
            out += (char)(0xC0 | c >> 6);
            out += (char)(0x80 | (c & 0x3F));
        }
    }
    return out;
}

bool IsMbid(std::string_view id) {
    if (id.size() != 36) return false;
    for (size_t i = 0; i < 36; ++i) {
        const char c = id[i];
        if (i == 8 || i == 13 || i == 18 || i == 23 ? c != '-' : !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

std::string Le32(uint32_t v) {
    std::string s;
    cord::PutLe(s, v, 4);
    return s;
}

// One track's audio, read twice and compared sector by sector (cdrip.h).
class TrackReader final : public PcmReader {
public:
    TrackReader(Drive& drive, const Toc& toc, size_t track, int offset, ReadStats& stats, const std::atomic<bool>& cancel,
                std::function<void(double)> progress)
        : drive_(drive), stats_(stats), cancel_(cancel), progress_(std::move(progress)) {
        const auto audio = toc.Audio();
        limit_ = audio.empty() ? 0 : toc.End(audio.back());
        const uint32_t start = toc.tracks[track].start, end = std::max(start, toc.End(track));
        format_.rate = 44100;
        format_.channels = 2;
        format_.bits = 16;
        format_.frames = (uint64_t)(end - start) * kSectorFrames;
        begin_ = (int64_t)start * kSectorFrames + offset;
        first_ = FloorDiv(begin_, kSectorFrames);
        count_ = FloorDiv(begin_ + (int64_t)format_.frames + kSectorFrames - 1, kSectorFrames) - first_;
        chunk_.resize((size_t)kChunk * kSectorBytes);
    }

    bool Scan() {
        hashes_.assign((size_t)count_, kFailed);
        std::vector<uint64_t> h;
        for (int64_t at = 0; at < count_; at += kChunk) {
            if (cancel_) return false;
            const unsigned n = (unsigned)std::min<int64_t>(kChunk, count_ - at);
            ReadSpan(at, n, chunk_.data(), h);
            std::copy(h.begin(), h.end(), hashes_.begin() + at);
            if (progress_) progress_(0.5 * (double)(at + n) / (double)count_);
        }
        scanned_ = true;
        Flush(0);  // the second pass must come from the disc, not from what the drive just cached
        return !cancel_;
    }

protected:
    size_t Next(int32_t* out, size_t frames) override {
        if (cancel_ || !scanned_) {
            damaged_ = true;
            return 0;
        }
        if (pos_ >= format_.frames) return 0;
        const int64_t sample = begin_ + (int64_t)pos_;
        const int64_t sector = FloorDiv(sample, kSectorFrames) - first_;
        if (sector < at_ || sector >= at_ + (int64_t)held_) {
            Verify(sector, (unsigned)std::min<int64_t>(kChunk, count_ - sector));
            if (progress_) progress_(0.5 + 0.5 * (double)(at_ + held_) / (double)count_);
        }
        const size_t from = (size_t)(sample - (first_ + at_) * kSectorFrames);
        const size_t n = std::min({frames, (size_t)held_ * kSectorFrames - from, (size_t)(format_.frames - pos_)});
        const uint8_t* p = chunk_.data() + from * 4;
        for (size_t i = 0; i < n * 2; ++i) out[i] = (int16_t)(p[2 * i] | p[2 * i + 1] << 8);
        pos_ += n;
        return n;
    }

private:
    // Reads sectors [at, at + n) of the track's span into `out`, with a hash for each. Sectors outside
    // the disc's audio are silence; a stretch the drive refuses is retried one sector at a time.
    void ReadSpan(int64_t at, unsigned n, uint8_t* out, std::vector<uint64_t>& hashes) {
        memset(out, 0, (size_t)n * kSectorBytes);
        std::vector<char> failed(n);
        const int64_t abs = first_ + at;
        const int64_t lo = std::max<int64_t>(abs, 0), hi = std::min<int64_t>(abs + n, limit_);
        if (lo < hi && !drive_.Read((uint32_t)lo, (unsigned)(hi - lo), out + (size_t)(lo - abs) * kSectorBytes))
            for (int64_t s = lo; s < hi; ++s) {
                uint8_t* one = out + (size_t)(s - abs) * kSectorBytes;
                if (!drive_.Read((uint32_t)s, 1, one)) {
                    memset(one, 0, kSectorBytes);
                    failed[(size_t)(s - abs)] = 1;
                }
            }
        hashes.resize(n);
        for (unsigned i = 0; i < n; ++i) hashes[i] = failed[i] ? kFailed : Hash(out + (size_t)i * kSectorBytes);
    }

    // Pushes the drive's read-ahead cache out with a read far from `at`, so the next read of `at`
    // comes from the disc.
    void Flush(int64_t at) {
        if (limit_ <= kChunk) return;
        const int64_t abs = first_ + at;
        const uint32_t away = abs < limit_ / 2 ? (uint32_t)(limit_ - kChunk) : 0;
        std::vector<uint8_t> scratch((size_t)kChunk * kSectorBytes);
        drive_.Read(away, kChunk, scratch.data());
    }

    // The second pass over [at, at + n): sectors whose hash matches the first pass are kept; the rest
    // are read again until one read repeats an earlier one.
    void Verify(int64_t at, unsigned n) {
        std::vector<uint64_t> h;
        ReadSpan(at, n, chunk_.data(), h);
        at_ = at;
        held_ = n;
        std::vector<std::vector<uint64_t>> seen(n);
        std::vector<bool> ok(n);
        unsigned pending = 0;
        for (unsigned i = 0; i < n; ++i) {
            const uint64_t was = hashes_[(size_t)(at + i)];
            ok[i] = h[i] != kFailed && h[i] == was;
            if (ok[i]) continue;
            ++pending;
            for (uint64_t x : {was, h[i]})
                if (x != kFailed) seen[i].push_back(x);
        }
        stats_.reread += pending;
        std::vector<uint8_t> again;
        for (unsigned tries = 0; pending && tries < kRetries && !cancel_; ++tries) {
            unsigned lo = 0, hi = n;
            while (ok[lo]) ++lo;
            while (ok[hi - 1]) --hi;
            again.resize((size_t)(hi - lo) * kSectorBytes);
            Flush(at + lo);
            ReadSpan(at + lo, hi - lo, again.data(), h);
            for (unsigned i = lo; i < hi; ++i) {
                const uint64_t x = h[i - lo];
                if (ok[i] || x == kFailed) continue;
                memcpy(chunk_.data() + (size_t)i * kSectorBytes, again.data() + (size_t)(i - lo) * kSectorBytes, kSectorBytes);
                if (std::find(seen[i].begin(), seen[i].end(), x) != seen[i].end()) {
                    ok[i] = true;
                    --pending;
                } else {
                    seen[i].push_back(x);
                }
            }
        }
        stats_.unsure += pending;
    }

    Drive& drive_;
    ReadStats& stats_;
    const std::atomic<bool>& cancel_;
    std::function<void(double)> progress_;
    int64_t begin_ = 0, first_ = 0, count_ = 0, limit_ = 0;  // samples and sectors from the disc's start
    std::vector<uint64_t> hashes_;                           // the first pass, one per sector
    std::vector<uint8_t> chunk_;                             // the verified sectors being handed out
    int64_t at_ = -1;
    unsigned held_ = 0;
    uint64_t pos_ = 0;
    bool scanned_ = false;
};

std::string Meta(const Job& job, size_t i) {
    const Disc& d = job.disc;
    const auto audio = job.toc.Audio();
    const size_t k = (size_t)(std::find(audio.begin(), audio.end(), job.tracks[i]) - audio.begin());
    std::string meta;
    auto text = [&](uint8_t id, const std::string& value) {
        if (!value.empty()) cord::PutEntry(meta, id, {}, value);
    };
    auto custom = [&](const char* key, const std::string& value) {
        if (!value.empty()) cord::PutEntry(meta, cord::kFreeForm, key, value);
    };
    const std::string title = k < d.titles.size() && !d.titles[k].empty() ? d.titles[k] : "Track " + std::to_string(k + 1);
    const std::string artist = k < d.artists.size() && !d.artists[k].empty() ? d.artists[k] : d.artist;
    text(cord::kTitle, title);
    text(cord::kArtist, artist);
    text(cord::kAlbum, d.album);
    if (artist != d.artist) text(cord::kAlbumArtist, d.artist);
    text(cord::kDate, d.date);
    cord::PutEntry(meta, cord::kTrack, {}, Le32(job.toc.tracks[job.tracks[i]].number));
    cord::PutEntry(meta, cord::kTrackTotal, {}, Le32((uint32_t)audio.size()));
    if (d.count > 1 && d.number) {
        cord::PutEntry(meta, cord::kDisc, {}, Le32(d.number));
        cord::PutEntry(meta, cord::kDiscTotal, {}, Le32(d.count));
    }
    custom("MUSICBRAINZ_ALBUMID", d.release);
    custom("MUSICBRAINZ_RELEASEGROUPID", d.releaseGroup);
    custom("MUSICBRAINZ_DISCID", d.discId);
    return meta;
}

}  // namespace

uint32_t Toc::End(size_t i) const {
    if (i + 1 >= tracks.size()) return leadout;
    uint32_t end = tracks[i + 1].start;
    if (tracks[i].audio && !tracks[i + 1].audio && i + 2 == tracks.size() && end >= tracks[i].start + kSessionGap)
        end -= kSessionGap;
    return end;
}

std::vector<size_t> Toc::Audio() const {
    std::vector<size_t> out;
    for (size_t i = 0; i < tracks.size(); ++i)
        if (tracks[i].audio) out.push_back(i);
    return out;
}

std::string DiscId(const Toc& toc) {
    if (toc.tracks.empty()) return {};
    // An Enhanced CD's data track is left out, and its audio session ends where that track's gap begins.
    size_t last = toc.tracks.size() - 1;
    uint32_t leadout = toc.leadout;
    if (last > 0 && !toc.tracks[last].audio) leadout = toc.End(--last);
    uint32_t offsets[100] = {leadout + kPregap};
    for (size_t i = 0; i <= last; ++i)
        if (toc.tracks[i].number >= 1 && toc.tracks[i].number <= 99) offsets[toc.tracks[i].number] = toc.tracks[i].start + kPregap;
    char hex[8 + 100 * 8 + 1];
    int n = snprintf(hex, sizeof hex, "%02X%02X", toc.tracks.front().number, toc.tracks[last].number);
    for (uint32_t o : offsets) n += snprintf(hex + n, sizeof hex - n, "%08X", o);
    const auto digest = ipod::Sha1(std::string_view(hex, (size_t)n));
    // Base64 with MusicBrainz's substitutions: '.', '_' and '-' for '+', '/' and '='.
    static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._";
    std::string id;
    for (size_t i = 0; i < digest.size(); i += 3) {
        const uint32_t v = digest[i] << 16 | (i + 1 < digest.size() ? digest[i + 1] << 8 : 0) | (i + 2 < digest.size() ? digest[i + 2] : 0);
        const size_t have = std::min<size_t>(3, digest.size() - i);
        for (size_t k = 0; k < 4; ++k) id += k <= have ? kAlphabet[v >> (18 - 6 * k) & 63] : '-';
    }
    return id;
}

std::string TocParameter(const Toc& toc) {
    const auto audio = toc.Audio();
    if (audio.empty()) return {};
    size_t last = toc.tracks.size() - 1;
    uint32_t leadout = toc.leadout;
    if (last > 0 && !toc.tracks[last].audio) leadout = toc.End(--last);
    std::string s = std::to_string(toc.tracks.front().number) + '+' + std::to_string(toc.tracks[last].number) + '+' +
                    std::to_string(leadout + kPregap);
    for (size_t i = 0; i <= last; ++i) s += '+' + std::to_string(toc.tracks[i].start + kPregap);
    return s;
}

bool ParseCdText(std::string_view packs, size_t tracks, Disc& out) {
    // Titles (0x80) and performers (0x81) of block 0; each pack's text runs on from the last, a NUL
    // ends one track's text, and a lone tab repeats the previous track's.
    std::vector<std::string> text[2];
    for (auto& t : text) t.assign(tracks + 1, {});
    std::string buffer[2];
    int track[2] = {-1, -1};
    for (size_t at = 0; at + 18 <= packs.size(); at += 18) {
        const uint8_t* p = (const uint8_t*)packs.data() + at;
        if ((p[0] != 0x80 && p[0] != 0x81) || (p[3] & 0xF0)) continue;  // other types, other blocks, double-byte text
        const int k = p[0] - 0x80;
        if (track[k] < 0) track[k] = p[1] & 0x7F;
        for (int i = 4; i < 16; ++i) {
            if (p[i]) {
                buffer[k] += (char)p[i];
                continue;
            }
            if (track[k] >= 0 && (size_t)track[k] <= tracks)
                text[k][(size_t)track[k]] = buffer[k] == "\t" && track[k] > 0 ? text[k][(size_t)track[k] - 1] : Latin1(buffer[k]);
            buffer[k].clear();
            ++track[k];
        }
    }
    out.album = text[0][0];
    out.artist = text[1][0];
    out.titles.assign(text[0].begin() + 1, text[0].end());
    out.artists.assign(text[1].begin() + 1, text[1].end());
    for (auto& a : out.artists)
        if (a == out.artist) a.clear();
    const bool any = !out.album.empty() || std::any_of(out.titles.begin(), out.titles.end(), [](auto& t) { return !t.empty(); });
    if (any) out.source = "CD-Text";
    return any;
}

Disc FromRelease(const albumtag::Release& r, size_t tracks) {
    Disc d;
    d.artist = r.artist;
    d.album = r.title;
    d.date = r.date;
    d.release = r.id;
    d.releaseGroup = r.groupId;
    d.count = r.discs;
    d.number = r.tracks.empty() ? 1 : r.tracks.front().disc;
    for (size_t i = 0; i < tracks; ++i) {
        const albumtag::ReleaseTrack* t = i < r.tracks.size() ? &r.tracks[i] : nullptr;
        d.titles.push_back(t ? t->title : std::string());
        d.artists.push_back(t && t->artist != r.artist ? t->artist : std::string());
    }
    d.source = r.artist + " \xE2\x80\x93 " + r.title;
    std::string about = r.date.substr(0, 4);
    for (const std::string* s : {&r.format, &r.country})
        if (!s->empty()) about += (about.empty() ? "" : ", ") + *s;
    if (d.count > 1) about += (about.empty() ? "disc " : ", disc ") + std::to_string(d.number) + " of " + std::to_string(d.count);
    if (!about.empty()) d.source += " (" + about + ")";
    return d;
}

std::vector<Disc> LookUp(const Toc& toc, online::Cancel& cancel, bool& offline, const albumtag::Fetch& fetch) {
    offline = false;
    std::vector<Disc> out;
    const std::string id = DiscId(toc), param = TocParameter(toc);
    const size_t tracks = toc.Audio().size();
    if (id.empty() || !tracks) return out;
    const std::string url = "https://musicbrainz.org/ws/2/discid/" + id + "?toc=" + param +
                            "&inc=recordings+artist-credits+release-groups&fmt=json";
    const online::Reply reply = fetch ? fetch(url) : online::Get(url, cancel, 10000, 4u << 20);
    if (reply.status == 0) offline = !cancel.Stopped();
    if (reply.status != 200) return out;
    for (const auto& r : albumtag::ParseDiscLookup(reply.body, id, tracks)) {
        out.push_back(FromRelease(r, tracks));
        out.back().discId = id;
    }
    return out;
}

std::string FetchCover(const std::string& release, online::Cancel& cancel, const albumtag::Fetch& fetch) {
    if (!IsMbid(release)) return {};
    const std::string url = "https://coverartarchive.org/release/" + release + "/front-500";
    const online::Reply reply = fetch ? fetch(url) : online::Get(url, cancel, 15000, online::kMaxImageBytes);
    int w = 0, h = 0;
    return reply.status == 200 && online::InspectImage(reply.body, w, h) != online::ImageType::None ? reply.body : std::string();
}

std::unique_ptr<PcmReader> OpenTrack(Drive& drive, const Toc& toc, size_t track, int offset, ReadStats& stats,
                                     const std::atomic<bool>& cancel, std::function<void(double)> progress) {
    if (track >= toc.tracks.size() || !toc.tracks[track].audio) return nullptr;
    offset = std::clamp(offset, -kMaxOffset, kMaxOffset);
    return std::make_unique<TrackReader>(drive, toc, track, offset, stats, cancel, std::move(progress));
}

bool Scan(PcmReader& reader) { return static_cast<TrackReader&>(reader).Scan(); }

std::wstring TrackPath(const Job& job, size_t i) {
#ifdef _WIN32
    const wchar_t sep = L'\\';
#else
    const wchar_t sep = L'/';
#endif
    const Disc& d = job.disc;
    const auto audio = job.toc.Audio();
    const size_t k = (size_t)(std::find(audio.begin(), audio.end(), job.tracks[i]) - audio.begin());
    std::wstring folder = job.folder;
    while (folder.size() > 1 && (folder.back() == L'\\' || folder.back() == L'/')) folder.pop_back();
    const std::wstring artist = SafePathComponent(FromUtf8(d.artist.empty() ? "Unknown artist" : d.artist));
    const std::wstring album = SafePathComponent(FromUtf8(d.album.empty() ? "Audio CD " + DiscId(job.toc).substr(0, 8) : d.album));
    wchar_t number[16];
    const unsigned n = job.toc.tracks[job.tracks[i]].number;
    if (d.count > 1 && d.number) swprintf(number, 16, L"%u-%02u ", d.number, n);
    else swprintf(number, 16, L"%02u ", n);
    const std::string title = k < d.titles.size() && !d.titles[k].empty() ? d.titles[k] : "Track " + std::to_string(k + 1);
    return folder + sep + artist + sep + album + sep + number + SafePathComponent(FromUtf8(title)) + (job.flac ? L".flac" : L".cord");
}

Result Rip(Drive& drive, const Job& job, const std::atomic<bool>& cancel, const std::function<void(size_t, double)>& progress) {
    Result result;
    std::vector<std::string> pictures;
    if (!job.cover.empty()) pictures.push_back(cord::PictureBytes(job.cover));
    for (size_t i = 0; i < job.tracks.size() && !cancel; ++i) {
        TrackDone done;
        done.path = TrackPath(job, i);
        if (GetFileAttributesW(done.path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            done.error = L"A file of that name already exists.";
            result.tracks.push_back(std::move(done));
            continue;
        }
        SHCreateDirectoryExW(nullptr, done.path.substr(0, done.path.find_last_of(L"\\/")).c_str(), nullptr);
        auto reader = OpenTrack(drive, job.toc, job.tracks[i], job.offset, done.stats, cancel,
                                [&](double f) { if (progress) progress(i, f); });
        if (!reader) {
            done.error = L"This isn't an audio track.";
        } else if (Scan(*reader)) {
            if (!WriteLossless(*reader, Meta(job, i), pictures, done.path, job.flac, done.error, cancel, job.threads) &&
                done.error.empty())
                done.error = L"The track couldn't be written.";
        }
        if (cancel) break;
        result.tracks.push_back(std::move(done));
    }
    result.cancelled = cancel;
    return result;
}

std::wstring DoneText(const Result& result) {
    size_t written = 0, unsure = 0;
    uint32_t reread = 0;
    for (const TrackDone& t : result.tracks) {
        if (!t.error.empty()) continue;
        ++written;
        reread += t.stats.reread;
        if (t.stats.unsure) ++unsure;
    }
    std::wstring s = (result.cancelled ? L"Rip cancelled after " : L"Ripped ") + std::to_wstring(written) +
                     (written == 1 ? L" track" : L" tracks");
    if (!written) return s + L".";
    if (unsure) s += L"; " + std::to_wstring(unsure) + (unsure == 1 ? L" has" : L" have") +
                     L" sectors that never read the same way twice, so listen for clicks";
    else if (reread) s += L", every sector verified (" + std::to_wstring(reread) + L" needed more reads)";
    else s += L", every sector read the same twice";
    if (const size_t failed = result.tracks.size() - written) s += L"; " + std::to_wstring(failed) + L" failed";
    return s + L".";
}

// ---------------------------------------------------------------------------------------------
// The drives

#ifdef _WIN32

namespace {

// CTL_CODE(FILE_DEVICE_CD_ROM, function, method, FILE_READ_ACCESS), spelled out so no DDK header is needed.
constexpr DWORD kReadToc = 0x24000, kRawRead = 0x2403E, kReadTocEx = 0x24054;

class WinDrive final : public Drive {
public:
    explicit WinDrive(HANDLE h) : h_(h) {}
    ~WinDrive() override { CloseHandle(h_); }

    bool ReadToc(Toc& toc) override {
        uint8_t b[4 + 100 * 8] = {};
        DWORD got = 0;
        if (!DeviceIoControl(h_, kReadToc, nullptr, 0, b, sizeof b, &got, nullptr) || got < 4) return false;
        const int n = b[3] - b[2] + 1;
        if (b[2] < 1 || n < 1 || n > 99 || got < 4 + (DWORD)(n + 1) * 8) return false;
        toc = {};
        for (int k = 0; k <= n; ++k) {
            const uint8_t* t = b + 4 + k * 8;  // reserved, control:4 adr:4, number, reserved, address (MSF)
            const int lba = (t[5] * 60 + t[6]) * 75 + t[7] - (int)kPregap;
            if (lba < 0) return false;
            if (k == n) toc.leadout = (uint32_t)lba;
            else toc.tracks.push_back({t[2], (uint32_t)lba, !(t[1] & 0x04)});
        }
        return true;
    }

    bool Read(uint32_t lba, unsigned count, uint8_t* out) override {
        struct {
            int64_t offset;  // in 2,048-byte units, as the driver wants
            uint32_t sectors, mode;  // mode 2: CDDA
        } info{(int64_t)lba * 2048, count, 2};
        DWORD got = 0;
        return DeviceIoControl(h_, kRawRead, &info, sizeof info, out, count * kSectorBytes, &got, nullptr) &&
               got == count * kSectorBytes;
    }

    std::string CdText() override {
        const uint8_t format[4] = {5, 0, 0, 0};  // CDROM_READ_TOC_EX_FORMAT_CDTEXT
        std::string b(4 + 18 * 2048, '\0');
        DWORD got = 0;
        if (!DeviceIoControl(h_, kReadTocEx, (void*)format, sizeof format, b.data(), (DWORD)b.size(), &got, nullptr) || got < 4)
            return {};
        const size_t length = std::min<size_t>(got, ((uint8_t)b[0] << 8 | (uint8_t)b[1]) + 2u);
        return length > 4 ? b.substr(4, length - 4) : std::string();
    }

private:
    HANDLE h_;
};

}  // namespace

std::vector<std::wstring> ListDrives() {
    std::vector<std::wstring> out;
    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        const wchar_t root[] = {(wchar_t)(L'A' + i), L':', L'\\', 0};
        if (mask >> i & 1 && GetDriveTypeW(root) == DRIVE_CDROM) out.push_back(std::wstring(root, 2));
    }
    return out;
}

std::unique_ptr<Drive> OpenDrive(const std::wstring& name, std::wstring& error) {
    const HANDLE h = CreateFileW((L"\\\\.\\" + name).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        error = L"Couldn't open the drive " + name + L".";
        return nullptr;
    }
    return std::make_unique<WinDrive>(h);
}

#else

namespace {

class LinuxDrive final : public Drive {
public:
    explicit LinuxDrive(int fd) : fd_(fd) {}
    ~LinuxDrive() override { close(fd_); }

    bool ReadToc(Toc& toc) override {
        cdrom_tochdr hdr{};
        if (ioctl(fd_, CDROMREADTOCHDR, &hdr) != 0 || hdr.cdth_trk0 < 1 || hdr.cdth_trk1 < hdr.cdth_trk0 || hdr.cdth_trk1 > 99)
            return false;
        toc = {};
        for (int t = hdr.cdth_trk0; t <= hdr.cdth_trk1 + 1; ++t) {
            cdrom_tocentry e{};
            e.cdte_track = (uint8_t)(t > hdr.cdth_trk1 ? CDROM_LEADOUT : t);
            e.cdte_format = CDROM_LBA;
            if (ioctl(fd_, CDROMREADTOCENTRY, &e) != 0 || e.cdte_addr.lba < 0) return false;
            if (t > hdr.cdth_trk1) toc.leadout = (uint32_t)e.cdte_addr.lba;
            else toc.tracks.push_back({(uint8_t)t, (uint32_t)e.cdte_addr.lba, !(e.cdte_ctrl & CDROM_DATA_TRACK)});
        }
        return true;
    }

    bool Read(uint32_t lba, unsigned count, uint8_t* out) override {
        cdrom_read_audio ra{};
        ra.addr.lba = (int)lba;
        ra.addr_format = CDROM_LBA;
        ra.nframes = (int)count;
        ra.buf = out;
        return ioctl(fd_, CDROMREADAUDIO, &ra) == 0;
    }

    std::string CdText() override {
        // READ TOC/PMA/ATIP, format 5 (CD-Text), through the kernel's packet interface.
        std::string b(4 + 18 * 2048, '\0');
        cdrom_generic_command cgc{};
        cgc.cmd[0] = 0x43;
        cgc.cmd[2] = 5;
        cgc.cmd[7] = (uint8_t)(b.size() >> 8);
        cgc.cmd[8] = (uint8_t)b.size();
        cgc.buffer = (unsigned char*)b.data();
        cgc.buflen = (unsigned)b.size();
        cgc.data_direction = CGC_DATA_READ;
        cgc.quiet = 1;
        if (ioctl(fd_, CDROM_SEND_PACKET, &cgc) != 0) return {};
        const size_t length = std::min<size_t>(b.size(), ((uint8_t)b[0] << 8 | (uint8_t)b[1]) + 2u);
        return length > 4 ? b.substr(4, length - 4) : std::string();
    }

private:
    int fd_;
};

}  // namespace

std::vector<std::wstring> ListDrives() {
    std::vector<std::wstring> out;
    for (int i = 0; i < 8; ++i) {
        const std::string path = "/dev/sr" + std::to_string(i);
        if (access(path.c_str(), F_OK) == 0) out.push_back(FromUtf8(path));
    }
    return out;
}

std::unique_ptr<Drive> OpenDrive(const std::wstring& name, std::wstring& error) {
    const int fd = open(ToUtf8(name).c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        error = L"Couldn't open " + name + L". Your account may need to be in the drive's group (often \"cdrom\" or \"optical\").";
        return nullptr;
    }
    return std::make_unique<LinuxDrive>(fd);
}

#endif

}  // namespace cdrip
