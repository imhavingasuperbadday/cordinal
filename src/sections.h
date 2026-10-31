// Byte codec for the extension sections of library.cdb (version 2). Each section is encoded
// into its own byte string first, so a reader can skip sections it doesn't know by length.
#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

// Section tags. 0 ends the list. Never reuse a tag for another meaning.
enum : uint64_t { kSectionEnd = 0, kSectionPlayLog = 1, kSectionPoints = 2, kSectionAudioHash = 3, kSectionPlaylistViews = 4,
       kSectionLoudness = 5, kSectionAlbumMeasurement = 6, kSectionFirstPlayed = 7, kSectionAlbumEditions = 8,
       kSectionAlbumArt = 9 };

// Appends LEB128 varints to a string. U writes an unsigned varint, I a zig-zag-encoded signed one,
// and Bytes a U length followed by the raw bytes.
class ByteWriter {
public:
    explicit ByteWriter(std::string& out) : out_(out) {}
    void U(uint64_t v) {
        do {
            uint8_t b = v & 0x7F;
            v >>= 7;
            out_ += (char)(b | (v ? 0x80 : 0));
        } while (v);
    }
    void I(int64_t v) { U(((uint64_t)v << 1) ^ (uint64_t)-(v < 0)); }
    void Bytes(std::string_view s) {
        U(s.size());
        out_.append(s);
    }

private:
    std::string& out_;
};

// Reads LEB128 varints from memory. Any malformed or out-of-range value clears `ok`; after that
// every read returns 0.
class ByteReader {
public:
    explicit ByteReader(std::string_view in) : in_(in) {}
    bool ok = true;
    bool Done() const { return in_.empty(); }
    std::string_view Bytes(size_t limit = SIZE_MAX) {
        uint64_t length = U(std::min<uint64_t>(limit, in_.size()));
        if (!ok || length > in_.size()) { Fail(); return {}; }
        auto text = in_.substr(0, (size_t)length);
        in_.remove_prefix((size_t)length);
        return text;
    }
    uint64_t U(uint64_t max = UINT64_MAX) {
        if (!ok) return 0;
        uint64_t v = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            if (in_.empty()) return Fail();
            uint8_t c = (uint8_t)in_.front();
            in_.remove_prefix(1);
            if (shift == 63 && c > 1) return Fail();
            v |= (uint64_t)(c & 0x7F) << shift;
            if (!(c & 0x80)) return v <= max ? v : Fail();
        }
        return Fail();
    }
    template <typename T> T Unsigned() { return (T)U(std::numeric_limits<T>::max()); }
    int64_t I() {
        uint64_t v = U();
        return (int64_t)(v >> 1) ^ -(int64_t)(v & 1);
    }

private:
    uint64_t Fail() {
        ok = false;
        in_ = {};
        return 0;
    }
    std::string_view in_;
};
