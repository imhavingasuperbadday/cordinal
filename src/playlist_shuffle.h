// Playback shuffle modes and ordering shared by the Windows and Linux players.
#pragma once
#include "library.h"

#include <random>

enum { kShuffleOff, kShuffleTracks, kShuffleAlbums, kShuffleCount };

// A full permutation of playlist positions, grouped by album and ordered by disc/track, with
// equal numbers retaining playlist order. The current album comes first when `current` is valid;
// callers continue after that occurrence so starting mid-album never jumps back to its beginning.
void ShuffleAlbums(std::vector<uint32_t>& order, const std::vector<TrackId>& items, std::mt19937& rng, int current);
// Reorders the supplied positions by early-skip weights, keeping every occurrence. The next
// position is at the back, matching the Windows playback queue.
void ShuffleByListening(std::vector<uint32_t>& order, const std::vector<TrackId>& items, std::mt19937& rng);
