// Local taste model. See taste.h for what it learns and when playback uses it.
//
// Weights are rebuilt only when a next track is chosen, from the play log plus the small maps
// below. The play log stays the record of finishes and skips. Parts and mood sketches are the
// two things it cannot express, so they have their own library.cdb sections.
//
// Plain shuffle puts songs into moments at random, which is what lets a listen be judged against
// its moment: a song's skips are compared with how often songs of its genre get skipped in the same
// kinds of moments, and a kind of song (an artist, genre, decade or sound) with how much listens in
// its moments usually earn. Raw totals would mostly measure how much of each kind the library holds.
#include "taste.h"

#include "peaks.h"
#include "playlog.h"
#include "sections.h"
#include "track_set.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {

constexpr int kBins = 8;
constexpr size_t kSession = 8;
constexpr size_t kLogLimit = 80000;
// Kinds of moment: first of a sitting, after an early skip or after a song that was heard, crossed
// with how alike the song is to the last one finished in the sitting (close, unrelated, far).
constexpr int kMoments = 9;
constexpr uint8_t kNoEvidence = 0xFF;

struct Acc {
    double finish = 0, early = 0;
    uint32_t days = 0;
};

// What songs of one kind earned when they came on, against what listens in the same moments
// usually earn.
struct Earned {
    double observed = 0, expected = 0;
};

struct Kinds {
    std::unordered_map<StringPool::Handle, Earned> artist, genre;
    std::unordered_map<int, Earned> decade;
};

void AddMoment(double& weight, double& mean, double& m2, double sample, double sampleWeight) {
    if (!(sampleWeight > 0)) return;
    weight += sampleWeight;
    double delta = sample - mean;
    mean += sampleWeight * delta / weight;
    m2 += sampleWeight * delta * (sample - mean);
}

// A weighted mean and spread of one value.
struct Spread {
    double weight = 0, mean = 0, m2 = 0;
    void Add(double sample, double sampleWeight) { AddMoment(weight, mean, m2, sample, sampleWeight); }
};

struct Tally {
    double listens = 0, skips = 0;
};

struct Model {
    std::unordered_map<TrackId, Acc> acc;
    std::unordered_map<uint64_t, uint32_t> next;
    Kinds day, night;
    double prior = 0;  // two typical listens' credit, so one good listen cannot make a favorite
    // Where finished songs sit, weighted by what they earned, and where heard songs sit, weighted by
    // what their moments usually earn: loudness, dynamics, and bass, mid, treble and arc.
    Spread gainEarned, gainHeard, highEarned, highHeard;
    std::array<Spread, 4> moodEarned, moodHeard;
    uint32_t revision = 0, firstEvent = 0, lastEvent = 0;
    size_t logSize = 0;
    bool ready = false;
};

// Per-track records in id order (track_set.h). A mood is 6 bytes and every song whose waveform was
// drawn has one, so a hash node per song cost several times the sketch itself.
TrackIdMap<std::array<uint16_t, kBins>> g_parts;
TrackIdMap<MoodSketch> g_moods;
std::vector<TrackId> g_session;
Model g_model;
bool g_modelDirty = true;
bool g_save = false;

uint64_t PairKey(TrackId from, TrackId to) { return ((uint64_t)from << 32) | to; }

void TouchModel() { g_modelDirty = true; }

void AddBin(uint16_t* bins, int index, int amount) {
    if (!amount || index < 0 || index >= kBins) return;
    int value = (int)bins[index] + amount;
    bins[index] = (uint16_t)std::clamp(value, 0, 65535);
    TouchModel();
}

void CreditSpan(uint16_t* bins, double start, double end, double duration, int amount) {
    if (!(duration > 0.5) || !amount) return;
    start = std::clamp(start, 0.0, duration);
    end = std::clamp(end, 0.0, duration);
    if (end < start) std::swap(start, end);
    double span = end - start;
    if (span < 0.05) {
        AddBin(bins, std::min(kBins - 1, (int)(start / duration * kBins)), amount);
    } else {
        for (int i = 0; i < kBins; ++i) {
            double a = duration * i / kBins, b = duration * (i + 1) / kBins;
            double overlap = std::min(end, b) - std::max(start, a);
            if (overlap > 0) AddBin(bins, i, (int)std::lround(amount * overlap / span));
        }
    }
    uint16_t peak = 0;
    for (int i = 0; i < kBins; ++i) peak = std::max(peak, bins[i]);
    if (peak > 50000)
        for (int i = 0; i < kBins; ++i) bins[i] = (uint16_t)(bins[i] / 2);
}

// Bins hold only rewinds and jumps, so a song gathered around one passage is one someone goes back to.
double PartMultiplier(TrackId id) {
    const auto* bins = g_parts.find(id);
    if (!bins) return 1;
    double sum = 0, peak = 0;
    for (int i = 0; i < kBins; ++i) {
        sum += (*bins)[i];
        peak = std::max(peak, (double)(*bins)[i]);
    }
    if (sum < 24 || peak <= 0) return 1;
    double focus = peak / (sum / kBins);
    if (focus < 1.35) return 1;
    return std::min(1.55, 1.0 + 0.22 * (focus - 1.0));
}

int YearOf(const Library& library, StringPool::Handle date) {
    std::string_view text = library.View(date);
    if (text.size() < 4) return 0;
    int year = 0;
    for (int i = 0; i < 4; ++i) {
        if (text[i] < '0' || text[i] > '9') return 0;
        year = year * 10 + (text[i] - '0');
    }
    return year >= 1000 && year <= 9999 ? year : 0;
}

// How much more often finished songs sit at `sample` than heard songs do, as a log factor that grows
// with how much has been finished. Equal spreads give 0: a value is not favored for being common in
// the library.
double LogTilt(const Spread& earned, const Spread& heard, double sample, double scale) {
    if (earned.weight < 2 || heard.weight < 2) return 0;
    double ve = earned.m2 / earned.weight + scale * scale, vh = heard.m2 / heard.weight + scale * scale;
    double de = sample - earned.mean, dh = sample - heard.mean;
    double ratio = 0.5 * (dh * dh / vh - de * de / ve + std::log(vh / ve));
    return ratio * earned.weight / (earned.weight + 4);
}

double Tilt(double logFactor, double low, double high) {
    return std::exp(std::clamp(logFactor, std::log(low), std::log(high)));
}

double PairUnit(double delta, double scale) {
    double z = delta / scale;
    double sim = 1.0 / (1.0 + 0.65 * z * z);
    return 0.62 + 0.63 * sim;
}

bool NightAt(uint32_t now) {
    int hour = LocalHour(now);
    return hour >= 20 || hour < 6;
}

template <typename Key>
void AddEarned(std::unordered_map<Key, Earned>& kinds, Key key, double observed, double expected) {
    if (!key) return;
    Earned& value = kinds[key];
    value.observed += observed;
    value.expected += expected;
}

// 0 for a kind that earns what its moments usually earn, or less; rising to 1 at 1.75 times as much.
template <typename Key>
double Lift(const std::unordered_map<Key, Earned>& kinds, Key key, double prior) {
    if (!key || !(prior > 0)) return 0;
    auto it = kinds.find(key);
    if (it == kinds.end()) return 0;
    double lift = (it->second.observed + prior) / (it->second.expected + prior);
    return std::clamp((lift - 1) / 0.75, 0.0, 1.0);
}

bool Fresh(const Library& library) {
    uint32_t first = PlayLog().empty() ? 0 : PlayLog().front().time;
    uint32_t last = PlayLog().empty() ? 0 : PlayLog().back().time;
    return !g_modelDirty && g_model.ready && g_model.revision == library.Revision() && g_model.logSize == PlayLog().size() &&
           g_model.firstEvent == first && g_model.lastEvent == last;
}

const MoodSketch* SketchOf(TrackId id) {
    const MoodSketch* sketch = g_moods.find(id);
    return sketch && sketch->Known() ? sketch : nullptr;
}

// How alike two songs are on what the library knows: artist, album, genre, era, tempo, ReplayGain
// loudness and dynamics, and the sound sketch once both exist. 1 is unrelated.
double Likeness(const Library& library, const Track& seed, TrackId seedId, const Track& track, TrackId id) {
    double sim = 1;
    bool sameArtist = seed.artist && seed.artist == track.artist;
    if (sameArtist) sim *= 2.5;
    else if (seed.albumArtist && seed.albumArtist == track.albumArtist) sim *= 1.55;
    if (seed.album && seed.album == track.album) sim *= 1.35;
    if (seed.genre && track.genre) sim *= seed.genre == track.genre ? 1.55 : 0.72;
    int seedYear = YearOf(library, seed.date), year = YearOf(library, track.date);
    if (seedYear && year) {
        int gap = std::abs(seedYear - year);
        if (gap <= 2) sim *= 1.22;
        else if (gap <= 8) sim *= 1.08;
        else if (gap >= 25) sim *= 0.78;
    }
    if (seed.bpm && track.bpm) {
        double ratio = std::abs((int)seed.bpm - (int)track.bpm) / (double)std::max(seed.bpm, track.bpm);
        if (ratio <= 0.06) sim *= 1.2;
        else if (ratio <= 0.15) sim *= 1.08;
        else if (ratio >= 0.45) sim *= 0.75;
    }
    if (seed.album && seed.album == track.album && seed.disc == track.disc && seed.trackNo && track.trackNo &&
        track.trackNo == seed.trackNo + 1)
        sim *= 1.65;
    if (seed.rgTrackGain != kNoGain && track.rgTrackGain != kNoGain) {
        double gain = PairUnit((double)seed.rgTrackGain - track.rgTrackGain, 400);
        double dynamics = seed.rgHigh && track.rgHigh ? PairUnit((double)seed.rgHigh - track.rgHigh, 16) : 1;
        sim *= gain * dynamics;
    }
    if (const MoodSketch* a = SketchOf(seedId)) {
        if (const MoodSketch* b = SketchOf(id)) {
            double db = (a->bass - b->bass) / 255.0, dm = (a->mid - b->mid) / 255.0, dt = (a->treble - b->treble) / 255.0;
            double da = (a->arc - b->arc) / 255.0;
            double alike = 1.0 / (1.0 + 4.5 * (db * db + dm * dm + dt * dt + 0.25 * da * da));
            sim *= 0.64 + 0.66 * alike;
        }
    }
    return sim;
}

int LikenessClass(double likeness) { return likeness >= 1.5 ? 0 : likeness <= 0.8 ? 2 : 1; }

// How much of the song a counted listen heard.
double Fraction(const Track& track, const PlayEvent& event) {
    double duration = track.durationMs ? track.durationMs / 1000.0 : event.seconds;
    return duration > 0 ? std::min(1.0, event.seconds / duration) : 1;
}

// What a counted listen earns: how much of the song was heard, scaled by its rating. Skips earn nothing.
double Credit(const Track& track, const PlayEvent& event) {
    if (!(event.flags & PlayEvent::kCounted)) return 0;
    double ratingMul = track.rating >= 10 ? std::pow(track.rating / 50.0, 1.2) : 1;
    return (0.45 + Fraction(track, event)) * ratingMul;
}

uint64_t MomentKey(uint32_t value, int moment) { return ((uint64_t)value << 4) | (uint64_t)moment; }

void Rebuild(const Library& library, uint32_t now) {
    g_model = {};
    g_model.revision = library.Revision();
    g_model.logSize = PlayLog().size();
    g_model.firstEvent = PlayLog().empty() ? 0 : PlayLog().front().time;
    g_model.lastEvent = PlayLog().empty() ? 0 : PlayLog().back().time;
    g_model.ready = true;
    g_modelDirty = false;

    const auto& log = PlayLog();
    size_t start = log.size() > kLogLimit ? log.size() - kLogLimit : 0;
    std::vector<ListenMoment> moments = ListenMoments(start);

    // First pass: the moment of every listen that is evidence, and how often early skips happen and
    // what listens earn in each kind of moment, overall and per genre and song.
    std::vector<uint8_t> momentOf(log.size() - start, kNoEvidence);
    std::array<Tally, kMoments> byMoment{};
    std::array<double, kMoments> creditByMoment{};
    std::unordered_map<uint64_t, Tally> byGenre, bySong;
    // Each listen's genre and own tallies, kept for the second pass. Map entries do not move when
    // the map grows, so the pointers stay valid.
    std::vector<std::pair<const Tally*, const Tally*>> tallies(log.size() - start);
    double judged = 0, skipped = 0, credited = 0;
    TrackId lastHeard = kInvalidTrack;
    for (size_t i = start; i < log.size(); ++i) {
        const PlayEvent& event = log[i];
        ListenMoment moment = moments[i - start];
        if (moment == ListenMoment::First) lastHeard = kInvalidTrack;
        if (moment == ListenMoment::Left || event.id == kInvalidTrack || !library.Contains(event.id)) continue;
        const Track& track = library[event.id];
        int like = 1;
        if (lastHeard != kInvalidTrack && library.Contains(lastHeard))
            like = LikenessClass(Likeness(library, library[lastHeard], lastHeard, track, event.id));
        int m = (int)moment * 3 + like;
        momentOf[i - start] = (uint8_t)m;
        bool skip = EarlySkip(event);
        double credit = Credit(track, event);
        byMoment[m].listens += 1;
        byMoment[m].skips += skip;
        creditByMoment[m] += credit;
        Tally& genre = byGenre[MomentKey(track.genre, m)];
        genre.listens += 1;
        genre.skips += skip;
        Tally& own = bySong[MomentKey(event.id, m)];
        own.listens += 1;
        own.skips += skip;
        tallies[i - start] = {&genre, &own};
        judged += 1;
        skipped += skip;
        credited += credit;
        if (event.flags & PlayEvent::kCounted) lastHeard = event.id;
    }
    const double overall = judged > 0 ? skipped / judged : 0;
    const double typical = judged > 0 ? credited / judged : 0;
    g_model.prior = 2 * typical;
    std::array<double, kMoments> rate{}, earns{};
    for (int m = 0; m < kMoments; ++m) {
        rate[m] = MomentSkipRate(byMoment[m].skips, byMoment[m].listens, overall);
        earns[m] = (creditByMoment[m] + 8 * typical) / (byMoment[m].listens + 8);
    }

    // Second pass: each listen against songs of its genre in the same kind of moment. The song's own
    // listens stay out of that rate, or a genre of one song would explain its song away.
    std::unordered_set<uint64_t> days;
    days.reserve(std::min(log.size(), kLogLimit));
    // Index of the previous counted row. The loop rebinds `event`, so this names the row itself.
    size_t previous = SIZE_MAX;
    for (size_t i = start; i < log.size(); ++i) {
        const PlayEvent& event = log[i];
        if (event.id == kInvalidTrack || !library.Contains(event.id)) {
            previous = SIZE_MAX;
            continue;
        }
        bool counted = (event.flags & PlayEvent::kCounted) != 0;
        if (counted && previous != SIZE_MAX && log[previous].id != event.id && Follows(log[previous], event))
            ++g_model.next[PairKey(log[previous].id, event.id)];
        previous = counted ? i : SIZE_MAX;
        int m = momentOf[i - start];
        if (m == kNoEvidence) continue;  // a skip that ended the listening
        const Track& track = library[event.id];
        const Tally& genre = *tallies[i - start].first;
        const Tally& own = *tallies[i - start].second;
        double here = std::clamp((genre.skips - own.skips + 8 * rate[m]) / (genre.listens - own.listens + 8), 0.02, 0.98);
        double ageDays = now > event.time ? (now - event.time) / 86400.0 : 0;
        double decay = std::exp2(-ageDays / 120.0);
        Acc& acc = g_model.acc[event.id];
        if (EarlySkip(event)) acc.early += decay * SkipWorth(overall, here);
        if (counted) {
            acc.finish += decay * Fraction(track, event) * PlayWorth(overall, here);
            uint32_t day = (uint32_t)std::max<int64_t>(0, LocalDay(event.time));
            if (days.insert(((uint64_t)event.id << 32) | day).second) ++acc.days;
        }
        double observed = decay * Credit(track, event), expected = decay * earns[m];
        Kinds& kinds = NightAt(event.time) ? g_model.night : g_model.day;
        AddEarned(kinds.artist, track.artist, observed, expected);
        AddEarned(kinds.genre, track.genre, observed, expected);
        AddEarned(kinds.decade, YearOf(library, track.date) / 10, observed, expected);
        if (track.rgTrackGain != kNoGain) {
            g_model.gainEarned.Add(track.rgTrackGain, observed);
            g_model.gainHeard.Add(track.rgTrackGain, expected);
        }
        if (track.rgHigh) {
            g_model.highEarned.Add(track.rgHigh, observed);
            g_model.highHeard.Add(track.rgHigh, expected);
        }
        if (const MoodSketch* sketch = SketchOf(event.id)) {
            const double values[4] = {(double)sketch->bass, (double)sketch->mid, (double)sketch->treble, (double)sketch->arc};
            for (int k = 0; k < 4; ++k) {
                g_model.moodEarned[k].Add(values[k], observed);
                g_model.moodHeard[k].Add(values[k], expected);
            }
        }
    }
}

double Affinity(const Library& library, TrackId id) {
    const Track& track = library[id];
    auto found = g_model.acc.find(id);
    const Acc acc = found == g_model.acc.end() ? Acc{} : found->second;
    bool played = acc.finish > 0 || acc.early > 0 || acc.days > 0;
    double part = PartMultiplier(id);
    double ratingMul = track.rating >= 10 ? std::pow(track.rating / 50.0, 1.7) : 1;
    double loyalty = acc.days > 1 ? 1.0 + 0.85 * std::log((double)acc.days) : 1;
    double core;
    if (!played && track.rating < 10) core = 0.52;
    else if (!played) core = 0.9 * ratingMul;
    else {
        double completion = acc.finish / (acc.finish + 1.0);
        double skipFrac = acc.early / (acc.early + acc.finish + 1.0);
        core = (0.42 + 0.85 * completion) * (1.0 - 0.72 * skipFrac) * ratingMul * loyalty;
    }
    core *= part;
    // Rewinding into a passage is a vote for the song even before it counts as a play.
    if (!played && track.rating < 10 && part > 1.2) core = std::max(core, 0.85 * part);
    return std::clamp(core, 0.04, 8.0);
}

double TasteFit(const Library& library, const Track& track, bool night) {
    const Kinds& kinds = night ? g_model.night : g_model.day;
    double fit = 0.35 + 1.25 * Lift(kinds.artist, track.artist, g_model.prior) +
                 0.8 * Lift(kinds.genre, track.genre, g_model.prior) +
                 0.35 * Lift(kinds.decade, YearOf(library, track.date) / 10, g_model.prior);
    if (track.rgTrackGain != kNoGain)
        fit *= Tilt(LogTilt(g_model.gainEarned, g_model.gainHeard, track.rgTrackGain, 80), 0.72, 1.2);
    if (track.rgHigh) fit *= Tilt(LogTilt(g_model.highEarned, g_model.highHeard, track.rgHigh, 4), 0.72, 1.2);
    return fit;
}

double MoodFit(TrackId id) {
    const MoodSketch* sketch = SketchOf(id);
    if (!sketch) return 1;
    const double values[4] = {(double)sketch->bass, (double)sketch->mid, (double)sketch->treble, (double)sketch->arc};
    double logFactor = 0;
    for (int k = 0; k < 4; ++k)
        logFactor += (k == 3 ? 0.35 : 1.0) * LogTilt(g_model.moodEarned[k], g_model.moodHeard[k], values[k], 24);
    return Tilt(logFactor, 0.72, 1.22);
}

// Likeness to the song playing, raised by the songs that have followed it before.
double SeedSim(const Library& library, const Track& seed, TrackId seedId, const Track& track, TrackId id) {
    double sim = Likeness(library, seed, seedId, track, id);
    auto hop = g_model.next.find(PairKey(seedId, id));
    if (hop != g_model.next.end() && hop->second)
        sim *= 1.0 + std::min(3.0, 0.85 * hop->second);
    return sim;
}

double Diversity(const Library& library, const Track& track, TrackId id, TrackId from) {
    int hits = 0, seen = 0;
    for (auto it = g_session.rbegin(); it != g_session.rend() && seen < 4; ++it) {
        if (*it == id || *it == from || !library.Contains(*it)) continue;
        ++seen;
        const Track& recent = library[*it];
        if (recent.artist && recent.artist == track.artist) ++hits;
    }
    double factor = 1;
    if (hits >= 2) factor *= 0.22;
    else if (hits == 1) factor *= 0.45;
    return factor;
}

MoodSketch SketchPeaks(const Peaks& peaks) {
    double bass = 0, mid = 0, treble = 0, energy = 0, centroid = 0, energySum = 0, square = 0;
    for (int i = 0; i < kPeakBuckets; ++i) {
        double b = peaks.mood[kMoodBass][i], m = peaks.mood[kMoodMid][i], t = peaks.mood[kMoodTreble][i];
        double level = (b + m + t) / 3.0;
        bass += b;
        mid += m;
        treble += t;
        energy += level;
        centroid += level * i;
        energySum += level;
        square += level * level;
    }
    MoodSketch sketch;
    if (!(energySum > 0)) return sketch;
    auto byte = [](double value) { return (uint8_t)std::clamp((int)std::lround(value), 0, 255); };
    sketch.bass = byte(bass / kPeakBuckets);
    sketch.mid = byte(mid / kPeakBuckets);
    sketch.treble = byte(treble / kPeakBuckets);
    sketch.energy = byte(energy / kPeakBuckets);
    sketch.arc = byte(centroid / energySum / (kPeakBuckets - 1) * 255.0);
    double mean = energy / kPeakBuckets;
    double variance = std::max(0.0, square / kPeakBuckets - mean * mean);
    sketch.contrast = byte(std::sqrt(variance));
    return sketch;
}

// Part rows: a count, then for each row the gap from the previous saved index and eight bins.
void WriteParts(ByteWriter& writer, const TasteState& state, const std::vector<uint32_t>& newId) {
    struct Row {
        uint32_t index;
        const uint16_t* bins;
    };
    std::vector<Row> parts;
    for (const TasteParts& row : state.parts) {
        if (row.id >= newId.size() || newId[row.id] == UINT32_MAX) continue;
        bool any = false;
        for (uint16_t bin : row.bin) any = any || bin;
        if (any) parts.push_back({newId[row.id], row.bin});
    }
    std::sort(parts.begin(), parts.end(), [](const Row& a, const Row& b) { return a.index < b.index; });
    parts.erase(std::unique(parts.begin(), parts.end(), [](const Row& a, const Row& b) { return a.index == b.index; }), parts.end());
    writer.U(parts.size());
    uint32_t previous = 0;
    for (const Row& row : parts) {
        writer.U(row.index - previous);
        previous = row.index;
        for (int i = 0; i < kBins; ++i) writer.U(row.bins[i]);
    }
}

bool ReadParts(ByteReader& reader, uint64_t trackCount, const std::vector<TrackId>* savedToId, std::vector<TasteParts>* out) {
    uint64_t partCount = reader.U(trackCount);
    uint64_t index = 0;
    bool any = false;
    for (uint64_t i = 0; i < partCount && reader.ok; ++i) {
        uint64_t gap = reader.U(trackCount);
        if ((any && !gap) || !reader.ok) return false;
        index += gap;
        if (index >= trackCount) return false;
        any = true;
        TasteParts row;
        row.id = savedToId ? (*savedToId)[(size_t)index] : (TrackId)index;
        for (int b = 0; b < kBins; ++b) row.bin[b] = reader.Unsigned<uint16_t>();
        if (out && row.id != kInvalidTrack) out->push_back(row);
    }
    return reader.ok;
}

}  // namespace

void ObservePlayback(TrackId id, double previous, double position, double duration) {
    if (id == kInvalidTrack || !std::isfinite(previous) || !std::isfinite(position) || !(duration > 0.5)) return;
    double delta = position - previous;
    // Listening straight through moves no bin: how far someone got before leaving is not a passage
    // they love. A jump back is a passage wanted again; a jump forward passes a span over.
    if (delta <= -1.0) {
        CreditSpan(g_parts[id].data(), position, previous, duration, 240);
        g_save = true;
    } else if (delta >= 4.0) {
        auto* bins = g_parts.find(id);
        if (!bins) return;
        CreditSpan(bins->data(), previous, position, duration, -36);
        g_save = true;
    }
}

bool ConsumeTasteSave() {
    bool save = g_save;
    g_save = false;
    return save;
}

void NoteSession(TrackId id) {
    if (id == kInvalidTrack) return;
    if (!g_session.empty() && g_session.back() == id) return;
    g_session.push_back(id);
    if (g_session.size() > kSession) g_session.erase(g_session.begin());
}

void NoteMood(TrackId id, const Peaks& peaks) {
    if (id == kInvalidTrack) return;
    MoodSketch sketch = SketchPeaks(peaks);
    if (!sketch.Known()) return;
    const MoodSketch* known = g_moods.find(id);
    if (known && known->bass == sketch.bass && known->mid == sketch.mid && known->treble == sketch.treble &&
        known->energy == sketch.energy && known->arc == sketch.arc && known->contrast == sketch.contrast)
        return;
    g_moods[id] = sketch;
    TouchModel();
    g_save = true;
}

TasteState CurrentTaste() {
    TasteState state;
    state.parts.reserve(g_parts.size());
    for (const auto& [id, bins] : g_parts) {  // in id order, as the section is saved
        bool any = false;
        for (int i = 0; i < kBins; ++i) any = any || bins[i];
        if (!any) continue;
        TasteParts row;
        row.id = id;
        std::copy(bins.begin(), bins.end(), row.bin);
        state.parts.push_back(row);
    }
    state.moods.reserve(g_moods.size());
    for (const auto& [id, mood] : g_moods)
        if (mood.Known()) state.moods.push_back({id, mood});
    return state;
}

void SetTaste(TasteState state) {
    g_parts.clear();
    g_moods.clear();
    for (const TasteParts& row : state.parts) {
        if (row.id == kInvalidTrack) continue;
        std::copy(row.bin, row.bin + kBins, g_parts[row.id].begin());
    }
    for (const TasteMood& row : state.moods)
        if (row.id != kInvalidTrack && row.mood.Known()) g_moods[row.id] = row.mood;
    g_save = false;
    TouchModel();
}

void ClearTaste() {
    SetTaste({});
    g_session.clear();
}

void RemapTaste(const std::vector<TrackId>& mapping) {
    TasteState state = CurrentTaste();
    auto map = [&](TrackId id) { return id < mapping.size() ? mapping[id] : kInvalidTrack; };
    for (auto& row : state.parts) row.id = map(row.id);
    for (auto& row : state.moods) row.id = map(row.id);
    std::erase_if(state.parts, [](const TasteParts& row) { return row.id == kInvalidTrack; });
    std::erase_if(state.moods, [](const TasteMood& row) { return row.id == kInvalidTrack; });
    std::vector<TrackId> session;
    for (TrackId id : g_session) {
        TrackId mapped = map(id);
        if (mapped != kInvalidTrack) session.push_back(mapped);
    }
    SetTaste(std::move(state));
    g_session = std::move(session);
}

std::vector<double> TasteWeights(const Library& library, const std::vector<TrackId>& candidates, TrackId from, uint32_t now) {
    if (!Fresh(library)) Rebuild(library, now);
    const bool seeded = from != kInvalidTrack && library.Contains(from);
    const Track* seed = seeded ? &library[from] : nullptr;
    const bool night = NightAt(now);
    std::vector<double> weights(candidates.size(), 0.02);
    double maxWeight = 0;
    for (size_t i = 0; i < candidates.size(); ++i) {
        TrackId id = candidates[i];
        if (!library.Contains(id) || id == from) continue;
        const Track& track = library[id];
        auto found = g_model.acc.find(id);
        bool played = found != g_model.acc.end() && (found->second.finish > 0 || found->second.early > 0 || found->second.days > 0);
        bool cold = !played && track.rating < 10 && PartMultiplier(id) <= 1.02;
        double fit = TasteFit(library, track, night) * MoodFit(id);
        double weight;
        if (seeded) {
            double sim = SeedSim(library, *seed, from, track, id);
            if (cold) weight = (0.16 + 0.22 * std::min(fit, 2.2)) * sim * std::min(sim, 2.5);
            else weight = Affinity(library, id) * sim * (0.7 + 0.3 * std::min(fit, 2.2));
        } else if (cold) {
            weight = 0.28 + 0.85 * std::min(fit, 2.4);
        } else {
            weight = Affinity(library, id) * (0.55 + 0.45 * std::min(fit, 2.2));
        }
        weight *= Diversity(library, track, id, from);
        weight = std::max(0.02, weight);
        weights[i] = weight;
        maxWeight = std::max(maxWeight, weight);
    }
    // A large playlist is mostly weak matches. Keep the draw inside the plausible set so one
    // lucky unknown cannot outweigh the songs that actually fit.
    if (seeded && maxWeight > 0) {
        double cut = maxWeight * 0.2;
        for (double& weight : weights)
            if (weight < cut) weight = std::max(0.01, weight * 0.04);
    }
    return weights;
}

std::vector<double> OrderWeights(const Library& library, const std::vector<TrackId>& candidates, TrackId from, uint32_t now,
                                 bool taste) {
    if (!taste) return ListeningWeights(candidates, from);
    std::vector<double> weights = TasteWeights(library, candidates, from, now);
    // A shuffle of the whole list keeps a slice of plain shuffle, about one pick in seven: songs the
    // model has passed over still come up, and listens it did not steer are the fair test of them.
    // Keep playing like this stays close to the song playing instead.
    if (from == kInvalidTrack && !weights.empty()) {
        double total = 0;
        for (double weight : weights) total += weight;
        if (total > 0)
            for (double& weight : weights) weight = (1 - kPlainShare) * weight / total + kPlainShare / weights.size();
    }
    return weights;
}

std::string EncodeTaste(const TasteState& state, const std::vector<uint32_t>& newId) {
    std::string out;
    ByteWriter writer(out);
    writer.U(0);  // the part list moved to its own section
    struct MoodRow {
        uint32_t index;
        MoodSketch mood;
    };
    std::vector<MoodRow> moods;
    for (const TasteMood& row : state.moods) {
        if (row.id >= newId.size() || newId[row.id] == UINT32_MAX || !row.mood.Known()) continue;
        moods.push_back({newId[row.id], row.mood});
    }
    std::sort(moods.begin(), moods.end(), [](const MoodRow& a, const MoodRow& b) { return a.index < b.index; });
    moods.erase(std::unique(moods.begin(), moods.end(), [](const MoodRow& a, const MoodRow& b) { return a.index == b.index; }),
                moods.end());
    writer.U(moods.size());
    uint32_t previous = 0;
    for (const MoodRow& row : moods) {
        writer.U(row.index - previous);
        previous = row.index;
        writer.U(row.mood.bass);
        writer.U(row.mood.mid);
        writer.U(row.mood.treble);
        writer.U(row.mood.energy);
        writer.U(row.mood.arc);
        writer.U(row.mood.contrast);
    }
    return out;
}

bool DecodeTaste(std::string_view data, uint64_t trackCount, const std::vector<TrackId>* savedToId, TasteState* out) {
    ByteReader reader(data);
    auto map = [&](uint64_t index) {
        if (index >= trackCount) return kInvalidTrack;
        return savedToId ? (*savedToId)[(size_t)index] : (TrackId)index;
    };
    // Part rows written here before they had their own section also counted plain listening, which
    // made wherever someone stopped look like a favorite passage. They are checked, then dropped.
    if (!ReadParts(reader, trackCount, savedToId, nullptr)) return false;
    TasteState state;
    uint64_t moodCount = reader.U(trackCount);
    uint64_t index = 0;
    bool any = false;
    for (uint64_t i = 0; i < moodCount && reader.ok; ++i) {
        uint64_t gap = reader.U(trackCount);
        if ((any && !gap) || !reader.ok) return false;
        index += gap;
        if (index >= trackCount) return false;
        any = true;
        TasteMood row;
        row.id = map(index);
        row.mood.bass = reader.Unsigned<uint8_t>();
        row.mood.mid = reader.Unsigned<uint8_t>();
        row.mood.treble = reader.Unsigned<uint8_t>();
        row.mood.energy = reader.Unsigned<uint8_t>();
        row.mood.arc = reader.Unsigned<uint8_t>();
        row.mood.contrast = reader.Unsigned<uint8_t>();
        if (out && row.id != kInvalidTrack && row.mood.Known()) state.moods.push_back(row);
    }
    if (!reader.ok || !reader.Done()) return false;
    if (out) *out = std::move(state);
    return true;
}

std::string EncodeTasteParts(const TasteState& state, const std::vector<uint32_t>& newId) {
    std::string out;
    ByteWriter writer(out);
    WriteParts(writer, state, newId);
    return out;
}

bool DecodeTasteParts(std::string_view data, uint64_t trackCount, const std::vector<TrackId>* savedToId,
                      std::vector<TasteParts>* out) {
    ByteReader reader(data);
    std::vector<TasteParts> parts;
    if (!ReadParts(reader, trackCount, savedToId, out ? &parts : nullptr) || !reader.Done()) return false;
    if (out) *out = std::move(parts);
    return true;
}
