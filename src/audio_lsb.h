// Bounded little-endian bit cursor for Cordinal's native lossless formats. Like
// Bits but LSB-first, since those bitstreams pack low bits first; an over-long
// read poisons the cursor instead of reading on.
#pragma once
#include <bit>
#include <cstdint>
#include <cstring>
#include <span>
#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace nativeaudio {
// Consumes the low bits first; Ones counts consecutive set bits up to a limit and
// fails the cursor if the limit is reached.
class LittleBits {
    friend class LittleWindow;
public:
    explicit LittleBits(std::span<const uint8_t> bytes):bytes_(bytes) {}
    uint32_t Get(unsigned n) {
        if (!n) return 0;
        if (n>32) { good_=false;return 0; }
        if (available_<n) {
            Refill();
            if (available_<n) { good_=false;return 0; }
        }
        uint32_t value=(uint32_t)(buffer_&((uint64_t(1)<<n)-1));buffer_>>=n;available_-=n;
        return value;
    }
    // A run whose terminating zero is already buffered is counted at once.
    unsigned Ones(unsigned limit) {
        if (available_<=limit) Refill();
        unsigned run=(unsigned)std::countr_one(buffer_);
        if (run<available_ && run<limit) { buffer_>>=run+1;available_-=run+1;return run; }
        unsigned n=0;
        while (n<limit && Get(1)) ++n;
        if (n==limit) good_=false;
        return n;
    }
    // Tops the buffer up when 8 bytes remain, so the next 56 bits read without refill
    // branches; the 8-byte load leaves already-buffered bits unchanged.
    void Prepare() { if (bytes_.size()-next_>=8) Refill(); }
    // The next n (at most 32) bits without consuming them; past the data they read as zero.
    uint32_t Peek(unsigned n) {
        if (available_<n) Refill();
        return (uint32_t)(buffer_&((uint64_t(1)<<n)-1));
    }
    // Consumes n peeked bits; like Get, more than remain fails the cursor.
    void Skip(unsigned n) {
        if (available_<n) { good_=false;return; }
        buffer_>>=n;available_-=n;
    }
    bool Good() const { return good_; }
    void Fail() { good_=false; }
private:
    // Top the buffer up to 56..63 bits with one 8-byte load where 8 bytes remain: whole
    // bytes are counted in, and bits above the count are the following data or zero.
    void Refill() {
        if (bytes_.size()-next_>=8) {
            uint64_t word;memcpy(&word,bytes_.data()+next_,8);
            buffer_|=word<<available_;next_+=(63-available_)>>3;available_|=56;
        } else while (available_<=56 && next_<bytes_.size()) { buffer_|=uint64_t(bytes_[next_++])<<available_;available_+=8; }
    }
    std::span<const uint8_t> bytes_;
    uint64_t buffer_=0;
    size_t next_=0;
    unsigned available_=0;
    bool good_=true;
};
// A LittleBits cursor held in registers for a residual loop. The state is the cursor's own
// (buffer, bit count, next byte), copied out and handed back by Commit(), so a loop can drop to
// the bounded cursor for an unusual code and pick the window up again. Refill() reads eight
// bytes without a test: call it only while Safe() holds, which covers two refills.
class LittleWindow {
public:
    __forceinline explicit LittleWindow(LittleBits& b) : b_(b) { Load(); }
    __forceinline void Load() {
        p_ = b_.bytes_.data() + b_.next_; end_ = b_.bytes_.data() + b_.bytes_.size();
        buffer_ = b_.buffer_; available_ = b_.available_;
    }
    __forceinline void Commit() { b_.next_ = size_t(p_ - b_.bytes_.data()); b_.buffer_ = buffer_; b_.available_ = available_; }
    __forceinline bool Safe() const { return size_t(end_ - p_) >= 16; }
    // At least 56 bits are buffered afterwards.
    __forceinline void Refill() {
        uint64_t word; std::memcpy(&word, p_, 8);
        buffer_ |= word << available_; p_ += (63 - available_) >> 3; available_ |= 56;
    }
    // The length of the run of set bits at the cursor; 64 when the whole buffer is set.
    __forceinline unsigned Run() const {
        const uint64_t zeros = ~buffer_;
        if (!zeros) return 64;
#ifdef _MSC_VER
        unsigned long index; _BitScanForward64(&index, zeros); return index;
#else
        return (unsigned)__builtin_ctzll(zeros);
#endif
    }
    __forceinline void Drop(unsigned n) { buffer_ >>= n; available_ -= n; }
    // n is 1..32 and no more than the buffered bits.
    __forceinline uint32_t Get(unsigned n) {
        uint32_t value = (uint32_t)(buffer_ & ((uint64_t(1) << n) - 1)); Drop(n);
        return value;
    }
private:
    LittleBits& b_;
    const uint8_t* p_;
    const uint8_t* end_;
    uint64_t buffer_;
    unsigned available_;
};
} // namespace nativeaudio
