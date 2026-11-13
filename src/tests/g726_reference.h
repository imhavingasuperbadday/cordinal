// Independent test oracle for G.726, never compiled into the player: cordinal's own counterpart of
// the G.726 module of the ITU-T G.191 Software Tools Library. It follows section 4 of the
// recommendation (12/1990) literally, block by block, on the unsigned fixed-width words that
// section defines, and includes the encoder, which the player has no need of.
#pragma once
#include <cstdint>
#include <vector>

namespace g726ref {
// One encoder or decoder at 2, 3, 4 or 5 bits a code (16, 24, 32 or 40 kbit/s), from the
// recommendation's reset state.
class Codec {
public:
    explicit Codec(unsigned bits) : bits_(bits) {}
    // Encodes one 14-bit uniform PCM sample (SL, -8192..8191) and returns its code I.
    unsigned Encode(int sl);
    // Decodes one code I and returns the reconstructed signal SR (16-bit two's complement, in the
    // same 14-bit units as SL).
    int Decode(unsigned code);
    // How often tone detection (TDP) and the transition detector (TR) fired, so a test can show
    // its signal reaches those paths.
    unsigned tones = 0, transitions = 0;

private:
    void Estimate();                 // SE, SEZ and Y from the delayed state
    int Reconstruct(unsigned code);  // RECONST onwards: every update for code I; returns SR
    unsigned bits_;
    // Delayed state (DELAY outputs), with the recommendation's reset values.
    uint32_t yl_ = 34816, yu_ = 544, dms_ = 0, dml_ = 0, ap_ = 0, td_ = 0, pk1_ = 0, pk2_ = 0;
    uint32_t a_[2] = {0, 0}, b_[6] = {0, 0, 0, 0, 0, 0};
    uint32_t dq_[6] = {32, 32, 32, 32, 32, 32}, sr_[2] = {32, 32};
    // This sample's estimates.
    uint32_t se_ = 0, sez_ = 0, y_ = 0;
};

// Encodes a whole signal; `codec`, when given, is left in its final state (for its counters).
std::vector<unsigned> Encode(const std::vector<int>& sl, unsigned bits, Codec* codec = nullptr);
std::vector<int> Decode(const std::vector<unsigned>& codes, unsigned bits);
}  // namespace g726ref
