// Bounded MSB-first bit cursor for packet codecs. A failed read never advances outside the packet.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace nativeaudio {
// CRC-16 with polynomial 0x8005, MSB first and a zero start, as FLAC frames and AC-3
// syncframes use it; slice-by-8, defined in audio_flac.cpp.
uint16_t Crc16(const uint8_t* p, size_t n);
// Index of the highest set bit of a nonzero value. MSVC's std::countl_zero tests the CPU for
// LZCNT on every call; BSR needs no test.
__forceinline unsigned HighestBit(uint64_t x) {
#ifdef _MSC_VER
    unsigned long index; _BitScanReverse64(&index, x); return index;
#else
    return 63 - __builtin_clzll(x);
#endif
}
// Bits reads most-significant-bit first. Up to 57 bits at the cursor are held in a
// register, so most reads, peeks and skips are shifts; the window is reloaded with one
// big-endian 64-bit load. It never holds bits past the limit, so an overlong request
// poisons the cursor and returns 0 rather than reading past the packet.
class Bits {
    friend class FastBits;
public:
    Bits(const uint8_t* data, size_t bytes, size_t bits=SIZE_MAX)
        : data_(data), bytes_(bytes), limit_(bits<bytes*8?bits:bytes*8) {}
    __forceinline uint64_t Get(unsigned n) {
        if (n > cached_ && !Fill(n)) return Long(n);
        uint64_t v = (cache_ >> 1) >> (63 - n);
        cache_ <<= n; cached_ -= n; pos_ += n;
        return v;
    }
    __forceinline int64_t Signed(unsigned n) { uint64_t v = Get(n); return n && n < 64 ? int64_t(v << (64 - n)) >> (64 - n) : int64_t(v); }
    __forceinline uint64_t Peek(unsigned n) {
        if (n > cached_ && !Fill(n)) { Bits copy = *this; uint64_t v = copy.Get(n); good_ = copy.good_; return v; }
        return (cache_ >> 1) >> (63 - n);
    }
    __forceinline void Skip(size_t n) {
        if (n <= cached_) { cache_ <<= n; cached_ -= unsigned(n); pos_ += n; }
        else if (n > Left()) good_ = false;
        else { pos_ += n; cached_ = 0; }
    }
    void Align() { Skip((8 - (pos_ & 7)) & 7); }
    size_t Left() const { return limit_ - pos_; }
    size_t Position() const { return pos_; }
    bool Good() const { return good_; }
    void Fail() { good_ = false; }
private:
    // Reload the window at the cursor; false when n bits can't be cached (past the limit
    // or wider than 57 bits).
    bool Fill(unsigned n) {
        const size_t left = Left(), byte = pos_ >> 3;
        if (n > 57 || n > left) return false;
        uint64_t w = 0;
        if (byte + 8 <= bytes_) {
            std::memcpy(&w, data_ + byte, 8);
#ifdef _MSC_VER
            w = _byteswap_uint64(w);
#else
            w = __builtin_bswap64(w);
#endif
        } else for (size_t i = 0; byte + i < bytes_; ++i) w |= uint64_t(data_[byte + i]) << (56 - 8 * i);
        cache_ = w << (pos_ & 7);
        cached_ = left < 57 ? unsigned(left) : 57;
        return true;
    }
    // Reads wider than the window come in two parts; anything past the limit fails.
    uint64_t Long(unsigned n) {
        if (n > 64 || n > Left()) { good_ = false; return 0; }
        uint64_t high = Get(n - 32);
        return (high << 32) | Get(32);
    }
    const uint8_t* data_;
    size_t bytes_, limit_, pos_ = 0;
    uint64_t cache_ = 0;
    unsigned cached_ = 0;
    bool good_ = true;
};
// An MSB-first bit window held in registers for an entropy decoder's inner loop: the window,
// its bit count and the byte pointer are locals, and one Refill() (an unaligned eight-byte load,
// no branch) leaves at least 56 bits ahead. Nothing here is bounded. The caller owns the proof
// that eight bytes are readable at the pointer on every Refill(): either the buffer is padded
// past everything a damaged stream can consume (the MPEG reservoir), or the loop tests first
// (FastBits below). Position() is the cursor in bits, to compare with a limit afterwards.
class BitWindow {
public:
    __forceinline BitWindow(const uint8_t* data, size_t bit) : start_(data), p_(data + (bit >> 3)) {
        Refill(); Drop(unsigned(bit & 7));
    }
    __forceinline void Refill() {
        uint64_t w; std::memcpy(&w, p_, 8);
#ifdef _MSC_VER
        w = _byteswap_uint64(w);
#else
        w = __builtin_bswap64(w);
#endif
        window_ |= w >> held_; p_ += (63 - held_) >> 3; held_ |= 56;
    }
    // The next n bits, 1 <= n <= 56, valid after a Refill() and at most 56 bits of Drop().
    __forceinline unsigned Peek(unsigned n) const { return unsigned(window_ >> (64 - n)); }
    __forceinline void Drop(unsigned n) { window_ <<= n; held_ -= n; }
    __forceinline unsigned Get(unsigned n) { unsigned v = Peek(n); Drop(n); return v; }
    // Get for a width that may be zero (up to 32).
    __forceinline unsigned GetWide(unsigned n) { unsigned v = unsigned((window_ >> 1) >> (63 - n)); Drop(n); return v; }
    // The window itself, most significant bit first, for leading-bit counts.
    __forceinline uint64_t Window() const { return window_; }
    __forceinline size_t Position() const { return size_t(p_ - start_) * 8 - held_; }
protected:
    __forceinline BitWindow(const uint8_t* data, const uint8_t* at) : start_(data), p_(at) {}
    const uint8_t* start_;
    const uint8_t* p_;
    uint64_t window_ = 0;
    unsigned held_ = 0;
};
// A BitWindow over a Bits cursor whose packet is not padded. A loop calls Refill() only while
// Safe() holds, and Safe() leaves room for three refills (21 bytes of advance) after one test.
// When Safe() fails, Pad() moves the loop onto a zero-padded copy of the packet's last bytes, so
// the same loop decodes the tail and no bounded twin is needed; a second failure means the
// stream ran past its end. Bits past the cursor's limit may be consumed here; Commit() then
// fails the cursor, as the bounded read would have.
class FastBits : public BitWindow {
public:
    using Tail = uint8_t[64];
    __forceinline explicit FastBits(Bits& b) : BitWindow(b.data_, b.data_ + (b.pos_ >> 3)), b_(b), end_(b.data_ + b.bytes_) {
        if (Safe()) { live_ = true; Refill(); Drop(unsigned(b.pos_ & 7)); }
    }
    __forceinline bool Safe() const { return size_t(end_ - p_) >= 24; }
    __forceinline bool Pad(Tail& tail) {
        const size_t pos = Position();
        if (padded_ || !b_.good_ || pos > b_.limit_) return false;
        origin_ = pos >> 3;
        CopyTail(tail, b_.data_ + origin_, b_.bytes_ - origin_);
        start_ = p_ = tail; end_ = tail + sizeof(Tail); window_ = 0; held_ = 0; padded_ = live_ = true;
        Refill(); Drop(unsigned(pos & 7));
        return true;
    }
    // Hand the position back. False (and a failed cursor) when the loop ran past the limit.
    __forceinline bool Commit() {
        const size_t pos = Position();
        if (pos > b_.limit_) b_.good_ = false;
        else if (live_) { b_.pos_ = pos; b_.cached_ = 0; }
        return b_.good_;
    }
private:
    __forceinline size_t Position() const { return live_ ? origin_ * 8 + BitWindow::Position() : b_.pos_; }
    // Runs once per packet.
    static void CopyTail(uint8_t* tail, const uint8_t* from, size_t n) {
        if (n > 32) n = 32;  // Safe() failed, so fewer than 32 bytes remain.
        std::memcpy(tail, from, n); std::memset(tail + n, 0, sizeof(Tail) - n);
    }
    Bits& b_;
    const uint8_t* end_;
    size_t origin_ = 0;
    bool live_ = false, padded_ = false;
};
} // namespace nativeaudio
