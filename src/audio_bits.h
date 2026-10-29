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
} // namespace nativeaudio
