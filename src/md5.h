// MD5 (RFC 1321) of decoded audio. FLAC stores the MD5 of its samples and cordinal's own format will
// too (docs/cord-format.md), so the same audio has the same fingerprint in either file. A check
// against damage and a duplicate finder, not a security measure.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

class Md5 {
public:
    void Add(const void* data, size_t bytes);
    // Interleaved samples as FLAC hashes them: each one little-endian in ceil(bits / 8) bytes.
    void AddPcm(const int32_t* samples, size_t count, unsigned bits);
    // Ends the message and returns the digest. Call it once; Add after Finish is meaningless.
    std::array<uint8_t, 16> Finish();

private:
    void Block(const uint8_t* p);
    uint32_t state_[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    uint64_t bytes_ = 0;
    uint8_t buffer_[64]{};
};
