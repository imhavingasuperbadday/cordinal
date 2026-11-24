// Playback shuffle modes and ordering shared by the Windows and Linux players.
#pragma once
#include "library.h"

#include <random>

enum { kShuffleOff, kShuffleTracks, kShuffleAlbums, kShuffleCount };

// A full permutation of playlist positions, grouped by album and ordered by disc/track, with
// equal numbers retaining playlist order. The current album comes first when `current` is valid;
// callers continue after that occurrence so starting mid-album never jumps back to its beginning.
void ShuffleAlbums(std::vector<uint32_t>& order, const std::vector<TrackId>& items, std::mt19937& rng, int current);
// A permutation of playlist positions for track shuffle, keeping movements of the same classical
// work (on the same album) together in movement/disc/track order. When `current` is valid, the
// remaining movements of `current`'s work come first (and `current` itself is omitted). When
// `weights` is non-null (one weight per playlist position), units are ordered by ShuffleByWeights.
void ShuffleTracks(std::vector<uint32_t>& order, const std::vector<TrackId>& items, std::mt19937& rng, int current,
                   const std::vector<double>* weights = nullptr);
// Reorders the supplied positions by early-skip weights, keeping every occurrence. The next
// position is at the back, matching the Windows playback queue.
void ShuffleByListening(std::vector<uint32_t>& order, const std::vector<TrackId>& items, std::mt19937& rng);
// The same permutation for weights already chosen (one per playlist position). A missing or
// non-positive weight sorts to the end instead of dropping the row.
void ShuffleByWeights(std::vector<uint32_t>& order, const std::vector<double>& weights, std::mt19937& rng);
