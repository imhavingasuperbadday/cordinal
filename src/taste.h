// What to play next from the music already in the library.
//
// The model learns on this machine only: finishes and early skips, songs that return on later
// days, ratings, ReplayGain loudness and dynamics, a mood sketch once a waveform exists, and the
// passages someone rewinds into. Choose by taste (Preferences, and Playback) turns it on for
// track shuffle and Keep playing like this. Recording starts either way, so turning it on later
// already has something to go on.
//
// A listen is evidence about the moment as much as the song. A skip that ended the listening says
// nothing about the song; one in a run of skips, or after something unlike it, says less than one
// right after a song of its kind that was heard. Each song is measured against songs of its genre
// in the same kinds of moments, and artists, genres, decades and sound are favored by what their
// songs earn when they come on, never by how many of them the library holds.
#pragma once
#include "library.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct Peaks;

// Eight bins across a song. Rewinding into a span raises its bins and jumping over a span lowers
// them; listening straight through moves none, since where someone stopped is not a favorite part.
struct TasteParts {
    TrackId id = kInvalidTrack;
    uint16_t bin[8] = {};
};

// Mean bass, mid and treble, overall energy, where that energy sits (0 start, 255 end) and how
// uneven the song is. All zero means the sketch is unknown.
struct MoodSketch {
    uint8_t bass = 0, mid = 0, treble = 0, energy = 0, arc = 0, contrast = 0;
    bool Known() const { return bass || mid || treble || energy; }
};

struct TasteMood {
    TrackId id = kInvalidTrack;
    MoodSketch mood;
};

struct TasteState {
    std::vector<TasteParts> parts;
    std::vector<TasteMood> moods;
};

// `previous` is the last observed position, `position` the one now. A small forward step is plain
// listening and changes nothing; a jump backward is a passage they wanted again; a jump forward
// passes that span over.
void ObservePlayback(TrackId id, double previous, double position, double duration);
// True once after a rewind, a jump inside a song or a new mood sketch. The caller marks the
// library dirty; this does not know which flag that is.
bool ConsumeTasteSave();
// The songs just started or just counted, newest last. Radio uses them to avoid repeating an artist.
void NoteSession(TrackId id);
void NoteMood(TrackId id, const Peaks& peaks);

TasteState CurrentTaste();
void SetTaste(TasteState state);
void ClearTaste();
void RemapTaste(const std::vector<TrackId>& mapping);

// One weight per candidate, in order. `from` is the song playing, or kInvalidTrack for a shuffle
// of the whole list. `now` is the clock decay and time of day are measured from.
std::vector<double> TasteWeights(const Library& library, const std::vector<TrackId>& candidates, TrackId from,
                                 uint32_t now);
// Taste weights when `taste` is on, otherwise the play log's skip and transition weights. A shuffle
// of the whole list by taste keeps kPlainShare of its draw plain.
constexpr double kPlainShare = 0.15;
std::vector<double> OrderWeights(const Library& library, const std::vector<TrackId>& candidates, TrackId from,
                                 uint32_t now, bool taste);

// The taste section holds mood sketches. Its part list dates from when plain listening also filled
// the bins; it is read for validation and dropped, and written empty so older readers still load it.
std::string EncodeTaste(const TasteState& state, const std::vector<uint32_t>& newId);
bool DecodeTaste(std::string_view data, uint64_t trackCount, const std::vector<TrackId>* savedToId, TasteState* out);
// The part bins, in their own section.
std::string EncodeTasteParts(const TasteState& state, const std::vector<uint32_t>& newId);
bool DecodeTasteParts(std::string_view data, uint64_t trackCount, const std::vector<TrackId>* savedToId,
                      std::vector<TasteParts>* out);
