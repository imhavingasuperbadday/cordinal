// ReplayGain's levelling model, album gains and track measurement, shared by both front ends.
//
// Cordinal applies its levelling model to the selected track or album gain:
//
//  - Loudness differences inside an album are the artist's intent (a finale is meant to hit
//    harder than the interlude before it), so album mode starts from the album gain. Peak
//    protection can reduce it for an individual track. Missing values fall back to the other gain.
//  - Music with loud and quiet parts is levelled by its loud parts, not only its average. A song
//    that builds from a quiet verse to a big sung chorus has a low average, and lifting the
//    average alone would push the chorus up into the limiter and flatten it. So the further the
//    loud parts sit above the average (Track::rgHigh, the top of the loudness range), the less
//    it is lifted: the chorus arrives at full strength and the verse stays as quiet as written.
//    This only ever damps a lift; a track that is already loud is not turned down twice for
//    being dynamic.
//  - Differences between albums are mostly mastering. A quiet album is lifted all the way to the
//    reference, but a loud one is only turned down part of the way: `keep` of the cut is
//    skipped, so a loud master still plays louder than a quiet one, just not by as much. A
//    master that runs right up to (or past) full scale has no headroom left to reveal either,
//    so such hot, wall-to-wall masters keep even more of their cut and stay as present as the
//    artist made them - the voice especially, which sits up front in that kind of master.
//  - Peaks never clip: the output's look-ahead limiter (PeakLimiter) replaces clipping, and the
//    gain is capped by the track's own true peak plus a small limiting budget. The budget is
//    measured against the loud parts: only peaks that stand well clear of them (drum hits,
//    plucks) may be limited, a little; loud sustained passages and voices never lean on it.
//    The limiter holds a fraction of a dB of headroom, so inter-sample overshoot (which it
//    cannot see) still lands under the hard clip instead of distorting.
//  - Only tracks are scanned; album values are worked out from the tracks' measurements
//    (UpdateAlbumGains). So a scan does exactly the tracks asked for, and a track added to an
//    album never means measuring the rest of it again.
//  - Tracks are measured automatically in the background (each front end's sweep, using
//    AutoMeasureTracks), the next ones to play first, so playback rarely meets a track without data.
#include "replaygain.h"

#include "background_cpu.h"
#include "cue.h"
#include "decoder.h"
#include "peaks.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <tuple>
#include <unordered_set>

namespace replaygain {

namespace {
// Each album's levelling for its loud parts, dB (see Dynamics). Keys are string handles, so
// compaction clears it (ForgetAlbumDynamics) and UpdateAlbumGains fills it again.
std::map<AlbumId, float> g_albumDynamics;

// How far the loud parts sit above the average, LU, or -1 when not measured.
double HighOf(const Track& t) { return t.rgHigh ? (t.rgHigh - 1) / 4.0 : -1.0; }

// How much less to lift music whose loud parts sit `high` LU above its average. Most music sits
// within about 3 LU and is left alone; beyond that, 60% of the extra range comes off the gain
// (so the loud parts still land a little above everything else, as they should), at most 6 dB.
double Dynamics(double high) { return high < 0 ? 0 : std::clamp(0.6 * (high - 3.0), 0.0, 6.0); }

std::string GainText(int16_t gain) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%+.2f dB", gain / 100.0);
    return buf;
}

std::string PeakText(uint16_t peak) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.6f", peak / 20000.0);
    return buf;
}
}  // namespace

double EqPeakDb(const EqSettings& settings, unsigned rate) {
    // Response analysis must not run in the audio callback or once per library track.
    static EqSettings cached;
    static unsigned cachedRate = 0;
    static double peak = 0;
    if (rate != cachedRate || !(settings == cached)) {
        cachedRate = rate;
        cached = settings;
        Equalizer eq;
        eq.Configure(settings, rate);
        peak = eq.PeakGainDb();
    }
    return peak;
}

float Gain(const Track& t, const ReplayGainOptions& o, double eqPeakDb) {
    if (!o.on) return 1.0f;
    const bool trackFirst = o.mode == ReplayGainMode::Track ||
                            (o.mode == ReplayGainMode::PlaybackOrder && o.shuffleTracks);
    const bool album = t.rgAlbumGain != kNoGain && (!trackFirst || t.rgTrackGain == kNoGain);
    const int16_t gain = album ? t.rgAlbumGain : t.rgTrackGain;
    const double eqPeak = eqPeakDb + o.headroom;
    const uint16_t peak = t.rgTrackPeak ? t.rgTrackPeak : t.rgAlbumPeak;
    const double peakDb = peak ? 20 * std::log10(peak / 20000.0) : 0.0;
    if (gain == kNoGain) {
        double db = o.preampNoInfo;
        db = peak ? std::min(db, -peakDb - eqPeak) : db - std::max(0.0, eqPeak);
        return (float)std::pow(10.0, db / 20.0);
    }
    double db = gain / 100.0;
    // Damp a lift, never a cut: dynamics only ever holds back turning a quiet track up. A track
    // that is already loud keeps its level instead of being turned down twice for being dynamic.
    if (db > 0) {
        double dynamics = Dynamics(HighOf(t));
        if (album) {
            auto it = g_albumDynamics.find(AlbumKeyOf(t));
            if (it != g_albumDynamics.end()) dynamics = it->second;
        }
        db = std::max(0.0, db - dynamics);
    }
    if (db < 0) {
        double keep = o.keep;
        // A hot master (running at or past full scale) was limited flat in mastering, so its cut
        // only lowers it. Keep more of that cut, in proportion to how far past full scale it is,
        // so wall-to-wall loud masters - and the voice in them - stay as loud as they were made.
        if (keep > 0) {
            const uint16_t keepPeak = album && t.rgAlbumPeak ? t.rgAlbumPeak : peak;
            // The track's peak was already converted; a distinct album peak matters only for a cut.
            const double keepPeakDb = keepPeak == peak ? peakDb :
                keepPeak ? 20 * std::log10(keepPeak / 20000.0) : 0.0;
            if (keepPeakDb > 0.0)
                keep = std::min(0.85, keep + (1.0 - keep) * 0.6 * std::min(keepPeakDb, 1.0));
        }
        db *= 1.0 - keep;
    }
    db += o.preamp;
    if (peak) {
        double budget = 0;  // dB of limiting allowed; none until the loud parts are measured
        if (t.rgTrackGain != kNoGain && t.rgHigh) {
            const double loudParts = kReplayGainReference - t.rgTrackGain / 100.0 + HighOf(t);
            budget = std::clamp((peakDb - loudParts - 9.0) / 3.0, 0.0, 3.0);
        }
        // The EQ preamp and complete filter response share this budget. A negative AutoEq
        // preamp already offsets its boosts; subtracting each positive band would count them twice.
        db = std::min(db, budget - peakDb - eqPeak);
    } else db -= std::max(0.0, eqPeak);  // reserve EQ boost even before the source peak has been measured
    return (float)std::pow(10.0, db / 20.0);
}

// The estimate goes through the same model as a measurement. Its lift is capped at 4 dB: a song's
// first seconds are often its quietest.
float GainForEstimate(const Track& track, double lufs, const ReplayGainOptions& o, double eqPeakDb) {
    Track t = track;
    if (t.rgTrackGain == kNoGain && t.rgAlbumGain == kNoGain && std::isfinite(lufs))
        t.rgTrackGain = (int16_t)std::clamp(std::lround((kReplayGainReference - lufs) * 100), -6000l, 400l);
    return Gain(t, o, eqPeakDb);
}

void ForgetAlbumDynamics(bool keepMeasurements) {
    std::map<AlbumId, float>().swap(g_albumDynamics);
    if (!keepMeasurements) std::map<AlbumId, AlbumMeasurement>().swap(g_albumMeasurements);
}

// One that can't be decoded can't be played either, so it doesn't hold its album's gain back.
bool AlbumMember(TrackId id) {
    const Track& t = g_lib[id];
    return !(t.flags & (Track::kMissing | Track::kFailed | Track::kRgScanFailed));
}

// Tags read (so its album is known), playable, and not measured by cordinal (gains read from tags
// lack the loud-part measurement). Older cordinal scans lack rgHigh, but their saved scan flag
// still counts as measured.
bool NeedsMeasuring(TrackId id) {
    const Track& t = g_lib[id];
    return t.Loaded() && (t.rgTrackGain == kNoGain || (!t.rgHigh && !(t.flags & Track::kRgScanned))) && AlbumMember(id);
}

// Whether a saved album measurement still describes this album: every track it covers is still
// a member, still marked measured, and still carries exactly the gain and peak it was saved
// with. Members are ascending track ids.
bool MeasurementCovers(const AlbumMeasurement& saved, const std::vector<TrackId>& members) {
    if (saved.members.size() > members.size()) return false;
    for (TrackId id : saved.members) {
        if (id >= g_lib.Count() || !std::binary_search(members.begin(), members.end(), id)) return false;
        const Track& t = g_lib[id];
        // A cleared track measurement can no longer stand for the album, even though its album
        // values are still there until this call clears them.
        if (t.rgTrackGain == kNoGain || !(t.flags & Track::kRgAlbumMeasured) || t.rgAlbumGain != saved.gain ||
            t.rgAlbumPeak != saved.peak) return false;
    }
    return !saved.members.empty();
}

// Whether a saved measurement covers exactly this album (a track has since joined or left).
bool MeasurementMatches(const AlbumMeasurement& saved, const std::vector<TrackId>& members) {
    return saved.members.size() == members.size() && MeasurementCovers(saved, members);
}

// Album gain from the album's track measurements: the duration-weighted energy mean of the
// tracks' loudness, which is what measuring the album as one long stream gives (to within the
// gating, a fraction of a dB in practice), and the loudest track peak. It is set once every
// playable track of the album is measured; until then each track plays at its own gain and any
// album gain from the files' tags is left as it was.
//
// The album's loud parts are the loud parts of its loudest tenth (by playing time): an album of
// quiet pieces with one big finale is levelled so the finale lands right, not drowned out by the
// average of everything else. That sets the album's levelling (Dynamics) for all its tracks.
bool UpdateAlbumGains(const std::vector<TrackId>& ids) {
    std::set<AlbumId> keys;
    for (TrackId id : ids)
        if (id < g_lib.Count()) keys.insert(AlbumKeyOf(g_lib[id]));
    if (keys.empty()) return false;
    bool dirty = false;
    struct Album {
        std::vector<TrackId> members;
        double energy = 0, seconds = 0;
        uint16_t peak = 0;
        bool complete = true, highs = true;
        std::vector<std::pair<double, double>> loudParts;  // per track: LUFS of its loud parts, seconds
    };
    std::map<AlbumId, Album> albums;
    for (const auto& key : keys) albums.try_emplace(key);
    for (TrackId id = 0; id < (TrackId)g_lib.Count(); ++id) {
        const Track& t = g_lib[id];
        if (!AlbumMember(id)) continue;
        auto it = albums.find(AlbumKeyOf(t));
        if (it == albums.end()) continue;
        Album& a = it->second;
        a.members.push_back(id);
        if (t.rgTrackGain == kNoGain) {
            a.complete = false;
            continue;
        }
        const double weight = std::max(t.durationMs / 1000.0, 1.0);
        const double loudness = kReplayGainReference - t.rgTrackGain / 100.0;
        a.energy += weight * std::pow(10.0, loudness / 10.0);
        a.seconds += weight;
        a.peak = std::max(a.peak, t.rgTrackPeak);
        if (t.rgHigh) a.loudParts.emplace_back(loudness + HighOf(t), weight);
        else a.highs = false;
    }
    for (auto& [key, a] : albums) {
        auto saved = g_albumMeasurements.find(key);
        if (!a.complete || a.members.empty() || a.seconds <= 0) {
            // An album keeps the measurement it already has while a track added since is still
            // unmeasured. It is given up only when the measurement no longer describes the
            // album (a member's measurement was cleared, or the track was retagged or removed),
            // as then it cannot speak for what is left.
            if (saved != g_albumMeasurements.end() && MeasurementCovers(saved->second, a.members)) {
                g_albumDynamics[key] = saved->second.dynamics / 10.0f;
                continue;
            }
            if (saved != g_albumMeasurements.end()) g_albumMeasurements.erase(saved);
            g_albumDynamics.erase(key);
            for (TrackId id : a.members) {
                Track& t = g_lib[id];
                if (!(t.flags & (Track::kRgScanned | Track::kRgAlbumMeasured)) || (t.rgAlbumGain == kNoGain && !t.rgAlbumPeak))
                    continue;
                t.rgAlbumGain = kNoGain;
                t.rgAlbumPeak = 0;
                t.flags &= (uint16_t)~Track::kRgAlbumMeasured;
                dirty = true;
            }
            continue;
        }
        const double loudness = 10 * std::log10(a.energy / a.seconds);
        const int16_t gain = EncodeReplayGain((float)(kReplayGainReference - loudness));
        double dynamics = 0;
        if (a.highs) {
            std::sort(a.loudParts.begin(), a.loudParts.end(), std::greater<>());
            double counted = 0, top = a.loudParts.front().first;
            for (const auto& [lufs, seconds] : a.loudParts) {
                top = lufs;
                if ((counted += seconds) >= a.seconds * 0.1) break;
            }
            dynamics = Dynamics(std::max(0.0, top - loudness));
        }
        // The measurement is saved with the library: a restart reuses it instead of working the
        // album out again, and playback keeps the same levelling before the first tick.
        const uint8_t levelling = (uint8_t)std::clamp((int)std::lround(dynamics * 10.0), 0, 255);
        g_albumDynamics[key] = levelling / 10.0f;
        auto previous = g_albumMeasurements.find(key);
        const bool restate = previous == g_albumMeasurements.end() || previous->second.gain != gain || previous->second.peak != a.peak ||
                             previous->second.dynamics != levelling || previous->second.members != a.members;
        g_albumMeasurements[key] = AlbumMeasurement{gain, a.peak, levelling, a.members};
        if (restate) dirty = true;  // write the new state (also the first time an old library is opened)
        for (TrackId id : a.members) {
            Track& t = g_lib[id];
            const bool changed = t.rgAlbumGain != gain || t.rgAlbumPeak != a.peak || !(t.flags & Track::kRgAlbumMeasured);
            t.rgAlbumGain = gain;
            t.rgAlbumPeak = a.peak;
            t.flags |= Track::kRgAlbumMeasured;
            if (changed) dirty = true;
        }
    }
    return dirty;
}

// A measured album whose members are unchanged keeps its gain and is not worked out again, while
// one a track has joined, left or been retagged from is recomputed (or keeps its old gain until
// the new member is measured). It costs nothing while the library revision stays put.
AlbumRefresh RefreshAlbums(uint32_t& revision) {
    AlbumRefresh result;
    if (revision == g_lib.Revision()) return result;
    revision = g_lib.Revision();
    std::map<AlbumId, std::vector<TrackId>> albums;
    for (TrackId id = 0; id < (TrackId)g_lib.Count(); ++id)
        if (AlbumMember(id)) albums[AlbumKeyOf(g_lib[id])].push_back(id);
    // Measurements of albums that no longer exist go with them.
    for (auto it = g_albumMeasurements.begin(); it != g_albumMeasurements.end();) {
        if (albums.count(it->first)) {
            ++it;
            continue;
        }
        it = g_albumMeasurements.erase(it);
        result.dirty = true;
    }
    std::vector<TrackId> work;
    bool restored = false;
    for (const auto& [key, members] : albums) {
        auto saved = g_albumMeasurements.find(key);
        if (saved != g_albumMeasurements.end() && MeasurementMatches(saved->second, members)) {
            g_albumDynamics[key] = saved->second.dynamics / 10.0f;
            restored = true;
            continue;
        }
        work.push_back(members.front());
    }
    if (!work.empty() && UpdateAlbumGains(work)) result.dirty = true;
    result.gains = restored || !work.empty();
    return result;
}

std::vector<TrackId> AutoMeasureTracks(const std::vector<TrackId>& upcoming, size_t limit) {
    std::vector<TrackId> ids;
    if (!limit) return ids;
    ids.reserve(std::min(limit, g_lib.Count()));
    std::set<AlbumId> soon;
    std::unordered_set<TrackId> selected;
    for (TrackId id : upcoming) {
        if (id >= g_lib.Count() || !NeedsMeasuring(id) || !selected.insert(id).second) continue;
        ids.push_back(id);
        soon.insert(AlbumKeyOf(g_lib[id]));
        if (ids.size() == limit) return ids;
    }
    struct Candidate { bool later; AlbumId album; TrackId id; };
    auto before = [](const Candidate& a, const Candidate& b) {
        return std::tie(a.later, a.album, a.id) < std::tie(b.later, b.album, b.id);
    };
    // Keep only the best remaining candidates in a max heap. A large library no longer
    // allocates/sorts one entry per unmeasured track to start a 300-track sweep.
    const size_t remaining = limit - ids.size();
    std::vector<Candidate> rest;
    rest.reserve(std::min(remaining, g_lib.Count()));
    for (TrackId id = 0; id < (TrackId)g_lib.Count(); ++id) {
        if (!NeedsMeasuring(id) || selected.count(id)) continue;
        AlbumId album = AlbumKeyOf(g_lib[id]);
        Candidate candidate{soon.count(album) == 0, album, id};
        if (rest.size() < remaining) {
            rest.push_back(candidate);
            std::push_heap(rest.begin(), rest.end(), before);
        } else if (before(candidate, rest.front())) {
            std::pop_heap(rest.begin(), rest.end(), before);
            rest.back() = candidate;
            std::push_heap(rest.begin(), rest.end(), before);
        }
    }
    std::sort_heap(rest.begin(), rest.end(), before);
    for (const auto& candidate : rest) ids.push_back(candidate.id);
    return ids;
}

RgResult Measure(const TrackPath& item, uint64_t fileSize, uint64_t fileTime, const std::atomic<bool>& cancel,
                 std::atomic<std::shared_ptr<Decoder>>& current, BackgroundCpuBudget* budget) {
    std::wstring error;
    std::shared_ptr<Decoder> dec = OpenTrackDecoder(item.Path(), 48000, error);
    current.store(dec);
    if (cancel && dec) dec->Abort();
    RgResult result{item.id, NAN, NAN, 0, false};
    if (dec) {
        std::vector<float> buf(4096 * 2);
        LoudnessMeter meter(dec->Rate());
        TruePeak truePeak;
        PeakAccumulator peaks;
        peaks.Begin((uint64_t)(dec->Duration() * dec->Rate()));
        bool complete = false;
        while (!cancel) {
            size_t n = dec->Read(buf.data(), 4096);
            if (!n) {
                complete = true;
                break;
            }
            meter.Add(buf.data(), n);
            truePeak.Add(buf.data(), n);
            peaks.Add(buf.data(), n);
            if (budget) budget->Pace(cancel);
        }
        const double lufs = meter.Integrated();
        if (complete && std::isfinite(lufs)) {
            // Only whole files: a sub-track's peaks depend on its range, which the seek bar asks for.
            Peaks wave{};
            if (item.Path().find(L'|') == std::wstring::npos && peaks.Finish(wave))
                StorePeaks(item.Path(), fileSize, fileTime, 0, 0, wave);
            const double high = meter.HighLoudness();
            result = {item.id, (float)(kReplayGainReference - lufs), truePeak.Peak(),
                      std::isfinite(high) ? (float)std::max(0.0, high - lufs) : 0.0f, true};
        }
    }
    current.store(nullptr);
    return result;
}

void Apply(const RgResult& r) {
    if (r.id >= g_lib.Count()) return;
    Track& t = g_lib[r.id];
    if (!r.ok) {
        t.flags |= Track::kRgScanFailed;  // saved, so startup doesn't repeat an unusable scan
        return;
    }
    t.flags &= (uint16_t)~Track::kRgScanFailed;
    t.rgTrackGain = EncodeReplayGain(r.trackGain);
    t.rgTrackPeak = EncodeReplayPeak(r.trackPeak);
    t.rgHigh = (uint8_t)std::clamp(1 + (int)std::lround(r.high * 4), 1, 255);
    t.flags |= Track::kRgScanned | Track::kRgTruePeak;
}

size_t Clear(const std::vector<TrackId>& ids) {
    std::unordered_set<TrackId> seen;
    size_t n = 0;
    for (TrackId id : ids) {
        if (id >= g_lib.Count() || !seen.insert(id).second) continue;
        Track& t = g_lib[id];
        const bool flagged = (t.flags & (Track::kRgScanned | Track::kRgTruePeak | Track::kRgScanFailed | Track::kRgAlbumMeasured)) != 0;
        if (t.rgTrackGain == kNoGain && t.rgAlbumGain == kNoGain && !t.rgTrackPeak && !t.rgAlbumPeak && !t.rgHigh && !flagged)
            continue;
        t.rgTrackGain = t.rgAlbumGain = kNoGain;
        t.rgTrackPeak = t.rgAlbumPeak = 0;
        t.rgHigh = 0;
        t.flags &= (uint16_t)~(Track::kRgScanned | Track::kRgTruePeak | Track::kRgScanFailed | Track::kRgAlbumMeasured);
        ++n;
    }
    if (n) {
        g_lib.Touch();
        UpdateAlbumGains(ids);
    }
    return n;
}

TagEdit TagsFor(const Track& t) {
    TagEdit e;
    e.rgTrackGain = t.rgTrackGain == kNoGain ? std::string() : GainText(t.rgTrackGain);
    e.rgTrackPeak = t.rgTrackPeak ? PeakText(t.rgTrackPeak) : std::string();
    e.rgAlbumGain = t.rgAlbumGain == kNoGain ? std::string() : GainText(t.rgAlbumGain);
    e.rgAlbumPeak = t.rgAlbumPeak ? PeakText(t.rgAlbumPeak) : std::string();
    // Native Opus R128 tags are relative to -23 LUFS; ReplayGain 2.0 is relative to -18 LUFS.
    auto r128 = [](int16_t gain) { return std::to_string(std::lround((gain / 100.0 - 5.0) * 256.0)); };
    e.r128TrackGain = t.rgTrackGain == kNoGain ? std::string() : r128(t.rgTrackGain);
    e.r128AlbumGain = t.rgAlbumGain == kNoGain ? std::string() : r128(t.rgAlbumGain);
    return e;
}

}  // namespace replaygain
