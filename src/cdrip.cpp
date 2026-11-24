// CD ripping (cdrip.h): the disc id, CD-Text, the two-pass verified track reader, the rip loop, and
// the Windows and Linux drives. Hashes of each sector, not the sectors, are kept between passes, so
// a track's verification costs 8 bytes a sector and the audio is held only one read at a time.
#include "cdrip.h"

#include "convert.h"
#include "cord.h"
#include "ipod.h"
#include "organize.h"
#include "web_remote.h"

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

// Reads [abs, abs + n) sectors from `drive`, zero-filling out-of-disc or unreadable sectors.
void ReadSectors(Drive& drive, int64_t abs, int64_t limit, unsigned n, uint8_t* out, std::vector<uint64_t>& hashes) {
    memset(out, 0, (size_t)n * kSectorBytes);
    std::vector<char> failed(n);
    const int64_t lo = std::max<int64_t>(abs, 0), hi = std::min<int64_t>(abs + n, limit);
    if (lo < hi && !drive.Read((uint32_t)lo, (unsigned)(hi - lo), out + (size_t)(lo - abs) * kSectorBytes))
        for (int64_t s = lo; s < hi; ++s) {
            uint8_t* one = out + (size_t)(s - abs) * kSectorBytes;
            if (!drive.Read((uint32_t)s, 1, one)) {
                memset(one, 0, kSectorBytes);
                failed[(size_t)(s - abs)] = 1;
            }
        }
    hashes.resize(n);
    for (unsigned i = 0; i < n; ++i) hashes[i] = failed[i] ? kFailed : Hash(out + (size_t)i * kSectorBytes);
}

void FlushDriveCache(Drive& drive, int64_t abs, int64_t limit) {
    if (limit <= kChunk) return;
    const uint32_t away = abs < limit / 2 ? (uint32_t)(limit - kChunk) : 0;
    std::vector<uint8_t> scratch((size_t)kChunk * kSectorBytes);
    drive.Read(away, kChunk, scratch.data());
}

void VerifySectors(Drive& drive, int64_t abs, int64_t limit, unsigned n, const uint64_t* firstHashes, uint8_t* out,
                   ReadStats& stats, const std::atomic<bool>& cancel) {
    std::vector<uint64_t> h;
    ReadSectors(drive, abs, limit, n, out, h);
    std::vector<std::vector<uint64_t>> seen(n);
    std::vector<bool> ok(n);
    unsigned pending = 0;
    for (unsigned i = 0; i < n; ++i) {
        const uint64_t was = firstHashes[i];
        ok[i] = h[i] != kFailed && h[i] == was;
        if (ok[i]) continue;
        ++pending;
        for (uint64_t x : {was, h[i]})
            if (x != kFailed) seen[i].push_back(x);
    }
    stats.reread += pending;
    std::vector<uint8_t> again;
    for (unsigned tries = 0; pending && tries < kRetries && !cancel; ++tries) {
        unsigned lo = 0, hi = n;
        while (ok[lo]) ++lo;
        while (ok[hi - 1]) --hi;
        again.resize((size_t)(hi - lo) * kSectorBytes);
        FlushDriveCache(drive, abs + lo, limit);
        ReadSectors(drive, abs + lo, limit, hi - lo, again.data(), h);
        for (unsigned i = lo; i < hi; ++i) {
            const uint64_t x = h[i - lo];
            if (ok[i] || x == kFailed) continue;
            memcpy(out + (size_t)i * kSectorBytes, again.data() + (size_t)(i - lo) * kSectorBytes, kSectorBytes);
            if (std::find(seen[i].begin(), seen[i].end(), x) != seen[i].end()) {
                ok[i] = true;
                --pending;
            } else {
                seen[i].push_back(x);
            }
        }
    }
    stats.unsure += pending;
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
        FlushDriveCache(drive_, first_, limit_);  // the second pass must come from the disc, not cached sectors
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
        ReadSectors(drive_, first_ + at, limit_, n, out, hashes);
    }

    // The second pass over [at, at + n): sectors whose hash matches the first pass are kept; the rest
    // are read again until one read repeats an earlier one.
    void Verify(int64_t at, unsigned n) {
        at_ = at;
        held_ = n;
        VerifySectors(drive_, first_ + at, limit_, n, hashes_.data() + at, chunk_.data(), stats_, cancel_);
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

namespace {

uint32_t DigitSum(uint32_t v) {
    uint32_t s = 0;
    for (; v; v /= 10) s += v % 10;
    return s;
}

uint32_t GetLe32(const char* p) {
    const auto* u = (const uint8_t*)p;
    return (uint32_t)u[0] | ((uint32_t)u[1] << 8) | ((uint32_t)u[2] << 16) | ((uint32_t)u[3] << 24);
}

uint32_t Crc32Step(uint32_t crc, const uint8_t* data, size_t n) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int b = 0; b < 8; ++b) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        ready = true;
    }
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ data[i]) & 0xffu] ^ (crc >> 8);
    return crc;
}

constexpr uint64_t kAr450Start = 450ull * kSectorFrames;
constexpr uint64_t kAr450End = 451ull * kSectorFrames;
constexpr uint64_t kArEdgeFrames = 5ull * kSectorFrames;

// O(1)-per-offset sliding AccurateRip frame-450 hasher over `samples[0..count)`.
// For each valid window start `s = anchor + delta` where [s, s + 588) lies in [0, count), invokes
// `hit(delta, frame450Crc)`. Skips if all samples in the window are zero so silence cannot collide.
template <typename Fn>
void SlideFrame450(const uint32_t* samples, size_t count, size_t anchor, Fn&& hit) {
    if (count < kSectorFrames) return;
    bool nonZero = false;
    for (size_t i = 0; i < count; ++i)
        if (samples[i]) { nonZero = true; break; }
    if (!nonZero) return;
    const size_t maxStart = count - kSectorFrames;
    const size_t loStart = anchor > (size_t)kMaxOffset ? anchor - (size_t)kMaxOffset : 0;
    const size_t hiStart = std::min(maxStart, anchor + (size_t)kMaxOffset);
    if (loStart > hiStart) return;
    uint32_t sumP = 0, sumQ = 0;
    for (size_t k = 0; k < kSectorFrames; ++k) {
        const uint32_t w = samples[loStart + k];
        sumP += w;
        sumQ += w * (uint32_t)(k + 1);
    }
    hit((int)((int64_t)loStart - (int64_t)anchor), sumQ);
    for (size_t s = loStart + 1; s <= hiStart; ++s) {
        const uint32_t nextW = samples[s + kSectorFrames - 1];
        const uint32_t prevW = samples[s - 1];
        sumQ = sumQ - sumP + kSectorFrames * nextW;
        sumP = sumP - prevW + nextW;
        hit((int)((int64_t)s - (int64_t)anchor), sumQ);
    }
}

// Single-pass streaming AccurateRip v1/v2/frame-450 + CRC-32 accumulator wrapping an open track reader.
class ArHashingReader final : public PcmReader {
public:
    ArHashingReader(PcmReader& inner, bool firstAudio, bool lastAudio) : inner_(inner) {
        format_ = inner.Format();
        const uint64_t total = format_.frames;
        startIdx_ = firstAudio ? (kArEdgeFrames - 1) : 0;
        endIdx_ = lastAudio ? (total > kArEdgeFrames ? total - kArEdgeFrames : 0) : total;
        const uint64_t minFor450 = lastAudio ? (456ull * kSectorFrames) : kAr450End;
        if (total >= minFor450 && kAr450Start >= startIdx_ && kAr450End <= endIdx_) {
            hasFrame450_ = true;
            winBegin_ = kAr450Start > (uint64_t)kMaxOffset ? kAr450Start - (uint64_t)kMaxOffset : 0;
            winEnd_ = std::min<uint64_t>(total, kAr450End + (uint64_t)kMaxOffset);
            window_.assign((size_t)(winEnd_ - winBegin_), 0);
        }
    }

    size_t Next(int32_t* out, size_t frames) override {
        const size_t n = inner_.Read(out, frames);
        if (inner_.Damaged()) damaged_ = true;
        uint8_t bytes[4096];
        size_t bpos = 0;
        for (size_t k = 0; k < n; ++k) {
            const uint32_t word = ((uint32_t)(uint16_t)out[k * 2 + 1] << 16) | (uint32_t)(uint16_t)out[k * 2];
            bytes[bpos++] = (uint8_t)(word & 0xffu);
            bytes[bpos++] = (uint8_t)((word >> 8) & 0xffu);
            bytes[bpos++] = (uint8_t)((word >> 16) & 0xffu);
            bytes[bpos++] = (uint8_t)((word >> 24) & 0xffu);
            if (bpos == sizeof bytes) {
                crc32_ = Crc32Step(crc32_, bytes, bpos);
                bpos = 0;
            }
            const uint64_t idx = fed_ + k;
            if (idx >= startIdx_ && idx < endIdx_) {
                const uint64_t prod = (uint64_t)word * (idx + 1);
                v1_ += (uint32_t)prod;
                v2_ += (uint32_t)prod + (uint32_t)(prod >> 32);
            }
            if (hasFrame450_) {
                if (idx >= kAr450Start && idx < kAr450End)
                    frame450_ += word * (uint32_t)(idx - kAr450Start + 1);
                if (idx >= winBegin_ && idx < winEnd_)
                    window_[(size_t)(idx - winBegin_)] = word;
            }
        }
        if (bpos) crc32_ = Crc32Step(crc32_, bytes, bpos);
        fed_ += n;
        return n;
    }

    AccurateRipTrackResult Finalize(const Job& job, size_t trackOrd) const {
        AccurateRipTrackResult out;
        out.v1 = v1_;
        out.v2 = v2_;
        out.frame450 = frame450_;
        out.crc32 = crc32_ ^ 0xffffffffu;
        out.hasFrame450 = hasFrame450_;
        if (!job.arChecked) return out;
        if (job.arPressings.empty()) {
            out.status = ArStatus::NotInDatabase;
            return out;
        }
        bool anyEntry = false;
        for (const auto& p : job.arPressings)
            if (trackOrd < p.tracks.size()) {
                anyEntry = true;
                const auto& e = p.tracks[trackOrd];
                if (e.crc == out.v2) {
                    out.status = ArStatus::VerifiedV2;
                    out.confidence = std::max<unsigned>(out.confidence, e.confidence);
                    out.totalConfidence += e.confidence;
                }
            }
        if (out.status == ArStatus::VerifiedV2) return out;
        for (const auto& p : job.arPressings)
            if (trackOrd < p.tracks.size() && p.tracks[trackOrd].crc == out.v1) {
                const auto& e = p.tracks[trackOrd];
                out.status = ArStatus::VerifiedV1;
                out.confidence = std::max<unsigned>(out.confidence, e.confidence);
                out.totalConfidence += e.confidence;
            }
        if (out.status == ArStatus::VerifiedV1) return out;
        if (hasFrame450_ && anyEntry) {
            int bestDelta = 0;
            unsigned bestConf = 0, sumConf = 0;
            bool hit = false;
            const size_t anchor = (size_t)(kAr450Start - winBegin_);
            SlideFrame450(window_.data(), window_.size(), anchor, [&](int delta, uint32_t crc) {
                if (delta == 0) return;
                unsigned dBest = 0, dSum = 0;
                for (const auto& p : job.arPressings)
                    if (trackOrd < p.tracks.size() && p.tracks[trackOrd].frame450 == crc && p.tracks[trackOrd].frame450 != 0) {
                        dBest = std::max<unsigned>(dBest, p.tracks[trackOrd].confidence);
                        dSum += p.tracks[trackOrd].confidence;
                    }
                if (dBest && (!hit || dSum > sumConf || (dSum == sumConf && std::abs(delta) < std::abs(bestDelta)))) {
                    hit = true;
                    bestDelta = delta;
                    bestConf = dBest;
                    sumConf = dSum;
                }
            });
            if (hit) {
                out.status = ArStatus::MatchedOffset;
                out.matchedOffset = bestDelta;
                out.confidence = bestConf;
                out.totalConfidence = sumConf;
                return out;
            }
        }
        out.status = anyEntry ? ArStatus::Mismatch : ArStatus::NotInDatabase;
        return out;
    }

private:
    PcmReader& inner_;
    uint64_t fed_ = 0, startIdx_ = 0, endIdx_ = 0;
    uint64_t winBegin_ = 0, winEnd_ = 0;
    uint32_t v1_ = 0, v2_ = 0, frame450_ = 0, crc32_ = 0xffffffffu;
    bool hasFrame450_ = false;
    std::vector<uint32_t> window_;
};

std::string Hex32(uint32_t v) {
    char buf[16];
    snprintf(buf, sizeof buf, "%08X", v);
    return buf;
}

std::string ArLine(const AccurateRipTrackResult& ar, size_t pressings) {
    switch (ar.status) {
        case ArStatus::VerifiedV2:
            return "Accurately ripped (v2, confidence " + std::to_string(ar.confidence) + ")";
        case ArStatus::VerifiedV1:
            return "Accurately ripped (v1, confidence " + std::to_string(ar.confidence) + ")";
        case ArStatus::MatchedOffset:
            return "Matched different pressing at offset " +
                   std::string(ar.matchedOffset >= 0 ? "+" : "") + std::to_string(ar.matchedOffset) +
                   " samples (confidence " + std::to_string(ar.confidence) + ")";
        case ArStatus::Mismatch:
            return "Cannot be verified against AccurateRip (" + std::to_string(pressings) + " pressings)";
        case ArStatus::NotInDatabase:
            return "Track not present in AccurateRip database";
        default:
            return "AccurateRip not checked";
    }
}

bool WriteTextAtomic(const std::wstring& path, std::string_view utf8) {
    const std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = utf8.empty() || (WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, nullptr) && written == utf8.size());
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

std::wstring WriteRipLog(const Job& job, const Result& result, const std::wstring& dir) {
#ifdef _WIN32
    const wchar_t sep = L'\\';
#else
    const wchar_t sep = L'/';
#endif
    const Disc& d = job.disc;
    const std::wstring album = SafePathComponent(FromUtf8(d.album.empty() ? "Audio CD " + DiscId(job.toc).substr(0, 8) : d.album));
    const std::wstring logPath = dir + sep + album + L".log";
    std::string log = "cordinal CD rip log\n\n";
    if (!d.artist.empty()) log += "Artist:      " + d.artist + "\n";
    if (!d.album.empty()) log += "Album:       " + d.album + "\n";
    if (!d.date.empty()) log += "Date:        " + d.date + "\n";
    if (!job.driveName.empty()) log += "Drive:       " + ToUtf8(job.driveName) + "\n";
    log += "Format:      " + std::string(job.flac ? "FLAC" : ".cord") + "\n";
    log += std::string("Read offset: ") + (job.offset > 0 ? "+" : "") + std::to_string(job.offset) + " samples\n";
    AccurateRipIds ids{};
    if (ComputeAccurateRipIds(job.toc, ids))
        log += "AccurateRip: " + Hex32(ids.id1) + "-" + Hex32(ids.id2) + "-" + Hex32(ids.cddb) + "\n";
    log += "\nTOC:\n";
    for (size_t t = 0; t < job.toc.tracks.size(); ++t) {
        const auto& tr = job.toc.tracks[t];
        const uint32_t end = job.toc.End(t);
        log += "  Track " + std::to_string(tr.number) + (tr.audio ? "" : " (data)") +
               ": start " + std::to_string(tr.start) + ", length " + std::to_string(end > tr.start ? end - tr.start : 0) + "\n";
    }
    log += "\nTracks:\n";
    for (size_t i = 0; i < result.tracks.size(); ++i) {
        const auto& td = result.tracks[i];
        const size_t slash = td.path.find_last_of(L"\\/");
        const std::string name = ToUtf8(slash == std::wstring::npos ? td.path : td.path.substr(slash + 1));
        log += "  " + name + "\n";
        if (!td.error.empty()) {
            log += "    Error: " + ToUtf8(td.error) + "\n";
            continue;
        }
        log += "    CRC-32:      " + Hex32(td.ar.crc32) + "\n";
        log += "    AccurateRip: " + ArLine(td.ar, job.arPressings.size()) +
               " [v2=" + Hex32(td.ar.v2) + ", v1=" + Hex32(td.ar.v1) +
               (td.ar.hasFrame450 ? ", f450=" + Hex32(td.ar.frame450) : "") + "]\n";
        log += "    Sectors:     " + std::to_string(td.stats.reread) + " reread, " +
               std::to_string(td.stats.unsure) + " unsure\n";
    }
    return WriteTextAtomic(logPath, log) ? logPath : std::wstring();
}

}  // namespace

bool ComputeAccurateRipIds(const Toc& toc, AccurateRipIds& ids) {
    const auto audio = toc.Audio();
    if (audio.empty() || audio.size() > 99) return false;
    const uint32_t leadout = toc.End(audio.back());
    ids = {};
    ids.tracks = (unsigned)audio.size();
    for (size_t i = 0; i < audio.size(); ++i) {
        const uint32_t start = toc.tracks[audio[i]].start;
        ids.id1 += start;
        ids.id2 += (start ? start : 1u) * (uint32_t)(i + 1);
    }
    ids.id1 += leadout;
    ids.id2 += (leadout ? leadout : 1u) * (uint32_t)(ids.tracks + 1);
    uint32_t cddbSum = 0;
    for (const auto& t : toc.tracks) cddbSum += DigitSum((t.start + kPregap) / 75u);
    const uint32_t firstSec = (toc.tracks.front().start + kPregap) / 75u;
    const uint32_t leadSec = (toc.leadout + kPregap) / 75u;
    const uint32_t totalSec = leadSec >= firstSec ? leadSec - firstSec : 0;
    ids.cddb = ((cddbSum % 255u) << 24) | (totalSec << 8) | (uint32_t)toc.tracks.size();
    return true;
}

std::string AccurateRipUrl(const AccurateRipIds& ids) {
    if (!ids.tracks || ids.tracks > 99) return {};
    char buf[128];
    snprintf(buf, sizeof buf,
             "http://www.accuraterip.com/accuraterip/%x/%x/%x/dBAR-%03u-%08x-%08x-%08x.bin",
             ids.id1 & 0xfu, (ids.id1 >> 4) & 0xfu, (ids.id1 >> 8) & 0xfu,
             ids.tracks, ids.id1, ids.id2, ids.cddb);
    return buf;
}

std::vector<AccurateRipPressing> ParseAccurateRip(std::string_view bin, unsigned expectedTracks) {
    std::vector<AccurateRipPressing> out;
    if (!expectedTracks || expectedTracks > 99) return out;
    const size_t recBytes = 13u + 9u * (size_t)expectedTracks;
    for (size_t pos = 0; pos + recBytes <= bin.size(); pos += recBytes) {
        const uint8_t n = (uint8_t)bin[pos];
        if (n != expectedTracks) break;
        AccurateRipPressing p;
        p.ids.tracks = n;
        p.ids.id1 = GetLe32(bin.data() + pos + 1);
        p.ids.id2 = GetLe32(bin.data() + pos + 5);
        p.ids.cddb = GetLe32(bin.data() + pos + 9);
        p.tracks.resize(n);
        for (size_t k = 0; k < n; ++k) {
            const size_t at = pos + 13 + k * 9;
            p.tracks[k].confidence = (uint8_t)bin[at];
            p.tracks[k].crc = GetLe32(bin.data() + at + 1);
            p.tracks[k].frame450 = GetLe32(bin.data() + at + 5);
        }
        out.push_back(std::move(p));
    }
    return out;
}

std::vector<AccurateRipPressing> FetchAccurateRip(const Toc& toc, online::Cancel& cancel, const albumtag::Fetch& fetch) {
    AccurateRipIds ids{};
    if (!ComputeAccurateRipIds(toc, ids)) return {};
    const std::string url = AccurateRipUrl(ids);
    if (url.empty()) return {};
    online::Reply reply;
    if (fetch) {
        reply = fetch(url);
    } else {
        const web::HttpReply http = web::HttpFetch(cancel.Session(), url, "GET", {}, {}, 10000, 1u << 20);
        reply.status = http.status;
        reply.body = http.body;
    }
    if (cancel.Stopped() || reply.status != 200) return {};
    return ParseAccurateRip(reply.body, ids.tracks);
}

OffsetDetection DetectOffset(Drive& drive, const Toc& toc, const std::vector<AccurateRipPressing>& pressings,
                             const std::atomic<bool>& cancel, const std::function<void(double)>& progress) {
    OffsetDetection out;
    if (pressings.empty() || toc.tracks.empty()) return out;
    const auto audio = toc.Audio();
    if (audio.empty()) return out;
    const int64_t limit = (int64_t)toc.End(audio.back());
    std::vector<size_t> candidates;
    for (size_t k = 0; k < audio.size() && candidates.size() < 3; ++k) {
        const size_t idx = audio[k];
        const uint32_t len = toc.End(idx) > toc.tracks[idx].start ? toc.End(idx) - toc.tracks[idx].start : 0;
        const bool isLast = k + 1 == audio.size();
        if (len >= (isLast ? 456u : 451u)) candidates.push_back(k);
    }
    if (candidates.empty()) return out;

    const size_t spanSlots = (size_t)(2 * kMaxOffset + 1);
    std::vector<unsigned> matchedTracks(spanSlots, 0);
    std::vector<unsigned> sumConfidence(spanSlots, 0);
    std::vector<uint8_t> firstBuf, verifiedBuf;
    std::vector<uint64_t> firstHashes;

    for (size_t ci = 0; ci < candidates.size() && !cancel; ++ci) {
        const size_t trackOrd = candidates[ci];
        const size_t idx = audio[trackOrd];
        const int64_t f450Begin = (int64_t)toc.tracks[idx].start * kSectorFrames + (int64_t)kAr450Start;
        const int64_t winBeginSample = f450Begin - kMaxOffset;
        const int64_t winEndSample = f450Begin + kSectorFrames + kMaxOffset;
        const int64_t secBegin = FloorDiv(winBeginSample, kSectorFrames);
        const int64_t secEnd = FloorDiv(winEndSample + kSectorFrames - 1, kSectorFrames);
        const unsigned nSec = (unsigned)(secEnd - secBegin);
        firstBuf.resize((size_t)nSec * kSectorBytes);
        verifiedBuf.resize((size_t)nSec * kSectorBytes);
        ReadSectors(drive, secBegin, limit, nSec, firstBuf.data(), firstHashes);
        FlushDriveCache(drive, secBegin, limit);
        ReadStats dummyStats;
        VerifySectors(drive, secBegin, limit, nSec, firstHashes.data(), verifiedBuf.data(), dummyStats, cancel);
        if (cancel) break;
        ++out.checkedTracks;

        const size_t nSamples = (size_t)nSec * kSectorFrames;
        std::vector<uint32_t> words(nSamples);
        for (size_t s = 0; s < nSamples; ++s) {
            const uint8_t* p = verifiedBuf.data() + s * 4;
            words[s] = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        }
        const size_t anchor = (size_t)(f450Begin - secBegin * kSectorFrames);
        std::vector<unsigned> trackBest(spanSlots, 0);
        SlideFrame450(words.data(), nSamples, anchor, [&](int delta, uint32_t crc) {
            unsigned bestC = 0;
            for (const auto& p : pressings)
                if (trackOrd < p.tracks.size() && p.tracks[trackOrd].frame450 == crc && p.tracks[trackOrd].frame450 != 0)
                    bestC = std::max<unsigned>(bestC, p.tracks[trackOrd].confidence);
            if (bestC) trackBest[(size_t)(delta + kMaxOffset)] = std::max(trackBest[(size_t)(delta + kMaxOffset)], bestC);
        });
        for (size_t j = 0; j < spanSlots; ++j)
            if (trackBest[j]) {
                ++matchedTracks[j];
                sumConfidence[j] += trackBest[j];
            }
        if (progress) progress((double)(ci + 1) / (double)candidates.size());
    }
    if (cancel) return {};

    size_t bestIdx = 0;
    for (size_t j = 0; j < spanSlots; ++j) {
        if (!matchedTracks[j]) continue;
        const int d = (int)j - kMaxOffset;
        const int bd = (int)bestIdx - kMaxOffset;
        if (!matchedTracks[bestIdx] || matchedTracks[j] > matchedTracks[bestIdx] ||
            (matchedTracks[j] == matchedTracks[bestIdx] &&
             (sumConfidence[j] > sumConfidence[bestIdx] ||
              (sumConfidence[j] == sumConfidence[bestIdx] && std::abs(d) < std::abs(bd))))) {
            bestIdx = j;
        }
    }
    if (matchedTracks[bestIdx]) {
        out.found = true;
        out.offset = (int)bestIdx - kMaxOffset;
        out.matchedTracks = matchedTracks[bestIdx];
        out.confidence = sumConfidence[bestIdx];
    }
    return out;
}

Result Rip(Drive& drive, const Job& job, const std::atomic<bool>& cancel, const std::function<void(size_t, double)>& progress) {
    Result result;
    std::vector<std::string> pictures;
    if (!job.cover.empty()) pictures.push_back(cord::PictureBytes(job.cover));
    const auto audio = job.toc.Audio();
    std::wstring logDir;
    for (size_t i = 0; i < job.tracks.size() && !cancel; ++i) {
        TrackDone done;
        done.path = TrackPath(job, i);
        const std::wstring dir = done.path.substr(0, done.path.find_last_of(L"\\/"));
        if (logDir.empty()) logDir = dir;
        if (GetFileAttributesW(done.path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            done.error = L"A file of that name already exists.";
            result.tracks.push_back(std::move(done));
            continue;
        }
        SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
        auto reader = OpenTrack(drive, job.toc, job.tracks[i], job.offset, done.stats, cancel,
                                [&](double f) { if (progress) progress(i, f); });
        if (!reader) {
            done.error = L"This isn't an audio track.";
        } else if (Scan(*reader)) {
            const auto it = std::find(audio.begin(), audio.end(), job.tracks[i]);
            const size_t trackOrd = it == audio.end() ? (size_t)-1 : (size_t)(it - audio.begin());
            const bool isFirst = trackOrd == 0;
            const bool isLast = trackOrd != (size_t)-1 && trackOrd + 1 == audio.size();
            ArHashingReader arReader(*reader, isFirst, isLast);
            if (!WriteLossless(arReader, Meta(job, i), pictures, done.path, job.flac, done.error, cancel, job.threads) &&
                done.error.empty())
                done.error = L"The track couldn't be written.";
            else if (done.error.empty() && trackOrd != (size_t)-1)
                done.ar = arReader.Finalize(job, trackOrd);
        }
        if (cancel) break;
        result.tracks.push_back(std::move(done));
    }
    result.cancelled = cancel;
    if (job.writeLog && !result.tracks.empty() && !logDir.empty())
        result.logPath = WriteRipLog(job, result, logDir);
    return result;
}

std::wstring DoneText(const Result& result) {
    size_t written = 0, unsure = 0, arExact = 0, arOffset = 0, arMismatch = 0;
    uint32_t reread = 0;
    for (const TrackDone& t : result.tracks) {
        if (!t.error.empty()) continue;
        ++written;
        reread += t.stats.reread;
        if (t.stats.unsure) ++unsure;
        switch (t.ar.status) {
            case ArStatus::VerifiedV2:
            case ArStatus::VerifiedV1: ++arExact; break;
            case ArStatus::MatchedOffset: ++arOffset; break;
            case ArStatus::Mismatch: ++arMismatch; break;
            default: break;
        }
    }
    std::wstring s = (result.cancelled ? L"Rip cancelled after " : L"Ripped ") + std::to_wstring(written) +
                     (written == 1 ? L" track" : L" tracks");
    if (!written) return s + L".";
    if (unsure) s += L"; " + std::to_wstring(unsure) + (unsure == 1 ? L" has" : L" have") +
                     L" sectors that never read the same way twice, so listen for clicks";
    else if (reread) s += L", every sector verified (" + std::to_wstring(reread) + L" needed more reads)";
    else s += L", every sector read the same twice";
    if (arExact + arOffset + arMismatch > 0) {
        if (arExact == written) s += L", all verified by AccurateRip";
        else if (arExact + arOffset == written) s += L", all matched in AccurateRip (" + std::to_wstring(arOffset) + L" at pressing offset)";
        else {
            if (arExact) s += L"; " + std::to_wstring(arExact) + L" verified by AccurateRip";
            if (arOffset) s += L"; " + std::to_wstring(arOffset) + L" matched at pressing offset";
            if (arMismatch) s += L"; " + std::to_wstring(arMismatch) + L" not verified by AccurateRip";
        }
    }
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
