// ReplayGain: cordinal's levelling model (one gain per track, from its album's and its own
// measurements), album gains worked out from the tracks' measurements, and measuring one track's
// loudness. Shared by the Windows and Linux front ends; each keeps its own scanning thread, its
// messages and its settings, and passes the settings in as ReplayGainOptions.
//
// Everything here runs on the UI thread except Measure, which a worker calls with a copied path.
#pragma once
#include "dsp.h"
#include "library.h"
#include "tagwriter.h"

#include <atomic>
#include <memory>
#include <vector>

class Decoder;
class BackgroundCpuBudget;

enum class ReplayGainMode { Album, Track, PlaybackOrder };

struct ReplayGainOptions {
    bool on = true;
    float preamp = 0, preampNoInfo = 0;  // dB, for measured tracks and for tracks without data
    float keep = 0.4f;   // the share of a loud album's cut that is skipped, so loud songs stay louder
    float headroom = 0;  // dB reserved below the limiter for EQ applied inside a headset
    ReplayGainMode mode = ReplayGainMode::Album;
    bool shuffleTracks = false;
};

// One track's measurement, as a worker posts it.
struct RgResult {
    TrackId id;
    float trackGain, trackPeak;
    float high;      // how far the loud parts sit above the integrated loudness, LU
    bool ok = true;  // false: the track could not be decoded
};

namespace replaygain {

// The linear gain to play `t` at. eqPeakDb is the EQ's highest boost (EqPeakDb), which the gain
// leaves room for so the EQ doesn't push peaks into the limiter.
float Gain(const Track& t, const ReplayGainOptions& o, double eqPeakDb);
// The engine's quick loudness estimate standing in for a measurement (PlayItem::estimate).
float GainForEstimate(const Track& t, double lufs, const ReplayGainOptions& o, double eqPeakDb);
// Highest steady-state gain of the EQ curve at `rate`, dB; cached for the last settings asked about.
double EqPeakDb(const EqSettings& eq, unsigned rate);

// A track that counts toward its album's gain: present and playable.
bool AlbumMember(TrackId id);
// A track the background measurer should pick up.
bool NeedsMeasuring(TrackId id);
bool MeasurementCovers(const AlbumMeasurement& saved, const std::vector<TrackId>& members);
bool MeasurementMatches(const AlbumMeasurement& saved, const std::vector<TrackId>& members);

// Recomputes the albums of `ids` from their tracks' measurements. True when the library changed
// (and should be saved).
bool UpdateAlbumGains(const std::vector<TrackId>& ids);
// Brings saved album measurements in step with the library when its revision moved past
// `revision` (which it updates). gains: playback gains may have changed; dirty: save the library.
struct AlbumRefresh {
    bool gains = false, dirty = false;
};
AlbumRefresh RefreshAlbums(uint32_t& revision);
// Drops the per-album levelling cache (and, unless keepMeasurements, the saved measurements).
void ForgetAlbumDynamics(bool keepMeasurements);
// A bounded background batch: the upcoming tracks first, then their albums, then the library.
std::vector<TrackId> AutoMeasureTracks(const std::vector<TrackId>& upcoming, size_t limit);

// Worker thread: measures one track (a CUE track: just its range) and stores its waveform peaks.
// `current` holds the open decoder so another thread can abort it; `budget` paces a background
// run. A result with ok == false and `cancel` set means cut short, neither measured nor failed.
RgResult Measure(const TrackPath& item, uint64_t fileSize, uint64_t fileTime, const std::atomic<bool>& cancel,
                 std::atomic<std::shared_ptr<Decoder>>& current, BackgroundCpuBudget* budget);
// Stores a worker's result in the library.
void Apply(const RgResult& r);
// Forgets the ReplayGain data of `ids`; returns how many tracks had any.
size_t Clear(const std::vector<TrackId>& ids);
// The tags that save `t`'s gains into its file (ReplayGain 2.0, and Opus's R128 fields).
TagEdit TagsFor(const Track& t);

}  // namespace replaygain
