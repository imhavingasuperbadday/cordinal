#include "gpod/zlib.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "gpod/error.hpp"
#include "gpod/io.hpp"

namespace gpod {
namespace {

constexpr std::size_t kMaxOutput = 1U << 30;
constexpr std::uint32_t kAdlerBase = 65521U;

class BitReader {
  public:
    explicit BitReader(std::span<const std::uint8_t> data) : data_(data) {}

    [[nodiscard]] std::uint32_t bit() {
        if (bits_ == 0U) {
            if (position_ >= data_.size()) {
                fail(ErrorCode::Corrupt, "truncated DEFLATE stream");
            }
            buffer_ = data_[position_++];
            bits_ = 8U;
        }
        const std::uint32_t value = buffer_ & 1U;
        buffer_ >>= 1U;
        --bits_;
        return value;
    }

    [[nodiscard]] std::uint32_t bits(unsigned count) {
        std::uint32_t value = 0;
        for (unsigned index = 0; index < count; ++index) {
            value |= bit() << index;
        }
        return value;
    }

    void align() noexcept {
        buffer_ = 0;
        bits_ = 0;
    }

    [[nodiscard]] std::uint8_t byte() {
        align();
        if (position_ >= data_.size()) {
            fail(ErrorCode::Corrupt, "truncated DEFLATE stream");
        }
        return data_[position_++];
    }

    [[nodiscard]] std::uint16_t little16() {
        const auto low = static_cast<std::uint16_t>(byte());
        const auto high = static_cast<std::uint16_t>(byte());
        return static_cast<std::uint16_t>(low | (high << 8U));
    }

    [[nodiscard]] std::uint32_t big32() {
        std::uint32_t value = 0;
        for (int index = 0; index < 4; ++index) {
            value = (value << 8U) | byte();
        }
        return value;
    }

    [[nodiscard]] bool exhausted() const noexcept { return position_ >= data_.size(); }

  private:
    std::span<const std::uint8_t> data_;
    std::size_t position_ = 0;
    std::uint32_t buffer_ = 0;
    unsigned bits_ = 0;
};

struct Huffman {
    std::array<std::vector<std::pair<std::uint16_t, std::uint16_t>>, 16> codes;

    void build(std::span<const std::uint8_t> lengths) {
        std::array<int, 16> count{};
        for (std::uint8_t length : lengths) {
            if (length > 15U) {
                fail(ErrorCode::Corrupt, "DEFLATE Huffman code is longer than 15 bits");
            }
            ++count[length];
        }
        // Detect an over-subscribed tree.  An incomplete tree is legal (a
        // distance tree may consist of a single one-bit code).
        int left = 1;
        for (int length = 1; length < 16; ++length) {
            left = (left << 1) - count[static_cast<std::size_t>(length)];
            if (left < 0) {
                fail(ErrorCode::Corrupt, "over-subscribed DEFLATE Huffman tree");
            }
        }
        std::array<int, 16> next{};
        // Zero-length entries are not codewords; canonical numbering starts
        // from the first real length.
        count[0] = 0;
        int code = 0;
        for (int length = 1; length < 16; ++length) {
            code = (code + count[static_cast<std::size_t>(length - 1)]) << 1;
            next[static_cast<std::size_t>(length)] = code;
        }
        for (std::size_t symbol = 0; symbol < lengths.size(); ++symbol) {
            const unsigned length = lengths[symbol];
            if (length == 0U) {
                continue;
            }
            const int assigned = next[length]++;
            codes[length].emplace_back(static_cast<std::uint16_t>(assigned),
                                       static_cast<std::uint16_t>(symbol));
        }
    }

    [[nodiscard]] int decode(BitReader& reader) const {
        std::uint32_t code = 0;
        for (unsigned length = 1; length < 16U; ++length) {
            code = (code << 1U) | reader.bit();
            for (const auto& [candidate, symbol] : codes[length]) {
                if (candidate == code) {
                    return symbol;
                }
            }
        }
        fail(ErrorCode::Corrupt, "invalid DEFLATE Huffman code");
    }
};

std::uint32_t adler32(std::span<const std::uint8_t> data) noexcept {
    std::uint32_t first = 1;
    std::uint32_t second = 0;
    for (std::uint8_t byte : data) {
        first = (first + byte) % kAdlerBase;
        second = (second + first) % kAdlerBase;
    }
    return (second << 16U) | first;
}

void append_output(Bytes& output, std::size_t distance, std::size_t length) {
    if (distance == 0U || distance > output.size()) {
        fail(ErrorCode::Corrupt, "DEFLATE distance points outside the output");
    }
    for (std::size_t index = 0; index < length; ++index) {
        if (output.size() >= kMaxOutput) {
            fail(ErrorCode::Corrupt, "DEFLATE output is too large");
        }
        output.push_back(output[output.size() - distance]);
    }
}

struct DecodeState {
    Bytes output;
};

void inflate_block(BitReader& reader, Huffman& literal, Huffman& distance,
                   DecodeState& state) {
    static constexpr std::array<int, 29> kLengthBase = {
        3,   4,   5,   6,   7,   8,   9,   10,  11,  13,  15,  17,  19,  23,  27,
        31,  35,  43,  51,  59,  67,  83,  99,  115, 131, 163, 195, 227, 258};
    static constexpr std::array<int, 29> kLengthExtra = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
        2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static constexpr std::array<int, 30> kDistanceBase = {
        1,     2,     3,     4,     5,     7,     9,     13,    17,    25,
        33,    49,    65,    97,    129,   193,   257,   385,   513,   769,
        1025,  1537,  2049,  3073,  4097,  6145,  8193,  12289, 16385, 24577};
    static constexpr std::array<int, 30> kDistanceExtra = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
        6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

    for (;;) {
        const int symbol = literal.decode(reader);
        if (symbol < 256) {
            if (state.output.size() >= kMaxOutput) {
                fail(ErrorCode::Corrupt, "DEFLATE output is too large");
            }
            state.output.push_back(static_cast<std::uint8_t>(symbol));
            continue;
        }
        if (symbol == 256) {
            return;
        }
        if (symbol < 257 || symbol > 285) {
            fail(ErrorCode::Corrupt, "invalid DEFLATE length symbol");
        }
        const std::size_t length_index = static_cast<std::size_t>(symbol - 257);
        const int extra_bits = kLengthExtra[length_index];
        const std::size_t length = static_cast<std::size_t>(
            kLengthBase[length_index] + static_cast<int>(reader.bits(
                                               static_cast<unsigned>(extra_bits))));
        const int distance_symbol = distance.decode(reader);
        if (distance_symbol < 0 || distance_symbol >= 30) {
            fail(ErrorCode::Corrupt, "invalid DEFLATE distance symbol");
        }
        const std::size_t distance_index = static_cast<std::size_t>(distance_symbol);
        const int distance_extra = kDistanceExtra[distance_index];
        const std::size_t back = static_cast<std::size_t>(
            kDistanceBase[distance_index] + static_cast<int>(reader.bits(
                                                  static_cast<unsigned>(distance_extra))));
        append_output(state.output, back, length);
    }
}

}  // namespace

Bytes zlib_compress(std::span<const std::uint8_t> data) {
    Bytes output;
    output.reserve(data.size() + (data.size() / 65535U + 1U) * 5U + 6U);
    // CMF: DEFLATE, 32 KiB window.  FLG makes the two bytes a multiple of 31.
    output.push_back(0x78);
    output.push_back(0x01);
    std::size_t position = 0;
    bool final = false;
    do {
        const std::size_t remaining = data.size() - position;
        const auto chunk = static_cast<std::uint16_t>(
            std::min<std::size_t>(remaining, 65535U));
        final = position + chunk == data.size();
        output.push_back(final ? 0x01 : 0x00);  // BFINAL + stored block
        const auto length = static_cast<std::uint16_t>(chunk);
        const auto complement = static_cast<std::uint16_t>(~length);
        output.push_back(static_cast<std::uint8_t>(length & 0xffU));
        output.push_back(static_cast<std::uint8_t>(length >> 8U));
        output.push_back(static_cast<std::uint8_t>(complement & 0xffU));
        output.push_back(static_cast<std::uint8_t>(complement >> 8U));
        output.insert(output.end(), data.begin() + static_cast<std::ptrdiff_t>(position),
                      data.begin() + static_cast<std::ptrdiff_t>(position + chunk));
        position += chunk;
    } while (!final);

    const std::uint32_t checksum = adler32(data);
    output.push_back(static_cast<std::uint8_t>((checksum >> 24U) & 0xffU));
    output.push_back(static_cast<std::uint8_t>((checksum >> 16U) & 0xffU));
    output.push_back(static_cast<std::uint8_t>((checksum >> 8U) & 0xffU));
    output.push_back(static_cast<std::uint8_t>(checksum & 0xffU));
    return output;
}

Bytes zlib_decompress(std::span<const std::uint8_t> data) {
    if (data.size() < 6) {
        fail(ErrorCode::Corrupt, "zlib stream is too short");
    }
    const auto method = static_cast<std::uint8_t>(data[0] & 0x0fU);
    const auto window = static_cast<std::uint8_t>(data[0] >> 4U);
    const std::uint32_t header =
        (static_cast<std::uint32_t>(data[0]) << 8U) | static_cast<std::uint32_t>(data[1]);
    if (method != 8U || window > 7U || header % 31U != 0U) {
        fail(ErrorCode::Corrupt, "invalid zlib header");
    }
    if ((data[1] & 0x20U) != 0U) {
        fail(ErrorCode::Unsupported, "zlib preset dictionaries are not supported");
    }

    BitReader reader(data.subspan(2));
    DecodeState state;
    bool final = false;
    while (!final) {
        final = reader.bit() != 0U;
        const auto type = static_cast<unsigned>(reader.bits(2));
        if (type == 0U) {
            reader.align();
            const std::uint16_t length = reader.little16();
            const std::uint16_t complement = reader.little16();
            if (static_cast<std::uint16_t>(~length) != complement) {
                fail(ErrorCode::Corrupt, "invalid stored DEFLATE block length");
            }
            if (state.output.size() + length > kMaxOutput) {
                fail(ErrorCode::Corrupt, "DEFLATE output is too large");
            }
            for (std::uint16_t index = 0; index < length; ++index) {
                state.output.push_back(reader.byte());
            }
            continue;
        }
        Huffman literal;
        Huffman distance;
        if (type == 1U) {
            std::vector<std::uint8_t> literal_lengths(288);
            for (std::size_t index = 0; index < 144U; ++index) {
                literal_lengths[index] = 8;
            }
            for (std::size_t index = 144U; index < 256U; ++index) {
                literal_lengths[index] = 9;
            }
            for (std::size_t index = 256U; index < 280U; ++index) {
                literal_lengths[index] = 7;
            }
            for (std::size_t index = 280U; index < 288U; ++index) {
                literal_lengths[index] = 8;
            }
            std::vector<std::uint8_t> distance_lengths(32, 5);
            literal.build(literal_lengths);
            distance.build(distance_lengths);
        } else if (type == 2U) {
            const int literal_count = static_cast<int>(reader.bits(5)) + 257;
            const int distance_count = static_cast<int>(reader.bits(5)) + 1;
            const int code_count = static_cast<int>(reader.bits(4)) + 4;
            static constexpr std::array<int, 19> kOrder = {
                16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            std::vector<std::uint8_t> code_lengths(19, 0);
            for (int index = 0; index < code_count; ++index) {
                code_lengths[static_cast<std::size_t>(kOrder[static_cast<std::size_t>(index)])] =
                    static_cast<std::uint8_t>(reader.bits(3));
            }
            Huffman code_tree;
            code_tree.build(code_lengths);
            std::vector<std::uint8_t> lengths;
            lengths.reserve(static_cast<std::size_t>(literal_count + distance_count));
            while (lengths.size() <
                   static_cast<std::size_t>(literal_count + distance_count)) {
                const int symbol = code_tree.decode(reader);
                if (symbol <= 15) {
                    lengths.push_back(static_cast<std::uint8_t>(symbol));
                } else if (symbol == 16) {
                    if (lengths.empty()) {
                        fail(ErrorCode::Corrupt, "DEFLATE repeat without a previous length");
                    }
                    const std::size_t repeat = 3U + reader.bits(2);
                    const std::uint8_t previous = lengths.back();
                    lengths.insert(lengths.end(), repeat, previous);
                } else if (symbol == 17) {
                    lengths.insert(lengths.end(), 3U + reader.bits(3), std::uint8_t{0});
                } else if (symbol == 18) {
                    lengths.insert(lengths.end(), 11U + reader.bits(7), std::uint8_t{0});
                } else {
                    fail(ErrorCode::Corrupt, "invalid DEFLATE code-length symbol");
                }
                if (lengths.size() > static_cast<std::size_t>(literal_count + distance_count)) {
                    fail(ErrorCode::Corrupt, "DEFLATE code-length run is too long");
                }
            }
            literal.build(std::span<const std::uint8_t>(
                lengths.data(), static_cast<std::size_t>(literal_count)));
            distance.build(std::span<const std::uint8_t>(
                lengths.data() + literal_count, static_cast<std::size_t>(distance_count)));
        } else {
            fail(ErrorCode::Corrupt, "reserved DEFLATE block type");
        }
        inflate_block(reader, literal, distance, state);
    }

    reader.align();
    const std::uint32_t expected = reader.big32();
    if (expected != adler32(state.output)) {
        fail(ErrorCode::Corrupt, "zlib Adler-32 checksum mismatch (produced " +
                                     std::to_string(state.output.size()) + " bytes)");
    }
    return state.output;
}

}  // namespace gpod
