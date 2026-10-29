// Sound effects: short, quiet sounds for actions that otherwise happen silently (ratings, the
// queue, saved tags, organized files...). Play, pause, skip, seek and volume stay silent: the
// music itself answers those.
//
// Every sound is synthesized when it plays (no sample data in the exe), slightly detuned each
// time so repeats don't sound mechanical. Pitched sounds stay on a major pentatonic scale, and
// while music plays they get shorter and quieter so they sit under the song. They play on their
// own shared-mode stream next to the music, never mixed into it, so EQ, ReplayGain and
// bit-perfect output are untouched.
#pragma once
#include <cstdint>
#include <vector>

enum class Sfx : uint8_t {
    Rate,         // arg: rating in half stars (1..10); one mallet note per star, rising
    Unrate,       // rating cleared
    QueueAdd,     // arg: queue length after adding; the plink rises with it
    PlayNext,     // quick two-note rise
    QueueClear,   // the plinks running back down
    TagsSaved,    // paper stamp
    Organized,    // card riffle
    Unorganized,  // the riffle backwards
    Oops,         // muted two-note fall
    NewFiles,     // cardinal "cheer-cheer" (two wood ticks while music plays)
    Dice,         // dice bouncing on a table (Random / Shuffle order)
    Drop,         // arg: number of dropped items; a plop, lower for bigger drops
    Sparkle,      // three quiet high notes (new auto-playlist)
    Milestone,    // cardinal "what-cheer" (a track's 100th, 250th... play)
    CardinalSong, // the whole song: what-cheer, what-cheer, birdy-birdy-birdy-birdy
    Count,
};

// Everything here is safe to call from the UI thread; Play and PlayClip hand the work to the
// output thread (sfx_output.cpp), which opens and later releases the device on its own.
namespace sfx {

// Renders a sound as mono samples in [-1, 1] at `rate`. `ducked` = music is playing.
// Deterministic for a given seed.
std::vector<float> Render(Sfx sound, int arg, unsigned rate, uint32_t seed, bool ducked);

// Plays a sound at `gain` (linear, applied to the rendered samples). Returns at once; the
// output stream opens on demand and closes again after a few idle seconds.
void Play(Sfx sound, int arg, bool ducked, float gain);
// Suppressed: nothing plays and the output device is released before this returns (exclusive
// output needs the device to itself).
void SetSuppressed(bool suppressed);
// Plays a music clip (interleaved stereo at `rate`) on the same stream, replacing a clip that is
// still playing. The games use it, so the player, its queue and play statistics stay untouched.
// Clips play even with sound effects off, but not while suppressed.
void PlayClip(std::vector<float> stereo, unsigned rate, float gain);
void StopClip();
double ClipPosition();  // seconds of the current clip played so far; -1 when none is playing
void Shutdown();

}  // namespace sfx
