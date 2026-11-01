// MSB-first bit writer shared by cordinal's built-in encoders.
// Bits are packed into whole bytes as soon as they are complete; the caller can
// append raw bytes between aligned fields (frame headers, ADTS, padding).
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

namespace cordinal::encode {

class BitWriter {
public:
    void Put(uint32_t value, unsigned bits) {
        if (!bits) return;
        if (bits < 32) value &= (1u << bits) - 1u;
        acc_ = (acc_ << bits) | value;
        count_ += bits;
        while (count_ >= 8) {
            count_ -= 8;
            bytes_.push_back((uint8_t)(acc_ >> count_));
        }
        if (count_) acc_ &= (1ull << count_) - 1ull;
    }

    // Append a byte-aligned span. The writer must already be aligned.
    void Bytes(const void* data, size_t size) {
        Align();
        const auto* p = static_cast<const uint8_t*>(data);
        bytes_.insert(bytes_.end(), p, p + size);
    }

    void Byte(uint8_t value) { Bytes(&value, 1); }

    void Align() {
        if (count_) {
            bytes_.push_back((uint8_t)(acc_ << (8 - count_)));
            acc_ = 0;
            count_ = 0;
        }
    }

    size_t Bits() const { return bytes_.size() * 8 + count_; }
    size_t BytesCount() const { AlignCheck(); return bytes_.size() + (count_ ? 1 : 0); }
    bool Empty() const { return bytes_.empty() && !count_; }
    void Clear() { bytes_.clear(); acc_ = 0; count_ = 0; }
    const std::vector<uint8_t>& BytesVector() const { return bytes_; }
    std::vector<uint8_t>& BytesVector() { return bytes_; }

    // Returns the completed bytes, padding the final partial byte with zeros.
    std::vector<uint8_t> Finish() {
        Align();
        return std::move(bytes_);
    }

private:
    void AlignCheck() const {}
    uint64_t acc_ = 0;
    unsigned count_ = 0;
    std::vector<uint8_t> bytes_;
};

}  // namespace cordinal::encode
