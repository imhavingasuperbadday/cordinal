// SHA-1, AES-128 and the hash58/hash72/hashAB checksum schemes.
//
// Ported from upstream libgpod: src/rijndael.c (Mike Scott's Rijndael, used
// here as a plain AES-128 with nb=4/nk=4), src/itdb_hash58.c (wtbw's
// proof-of-concept via Christophe Fergeau), src/itdb_hash72.c (Chris Lee's
// signature generator/extractor) and src/itdb_hashAB.c (Nikias Bassen).  The
// byte layouts, key derivation and the header fields that are blanked before
// hashing are kept identical so the images stay interchangeable with upstream.

#include "gpod/hash.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

#include "gpod/error.hpp"
#include "gpod/io.hpp"
#include "gpod/model.hpp"
#include "gpod/paths.hpp"

namespace gpod {
namespace {

constexpr std::uint32_t rotl32(std::uint32_t value, unsigned bits) {
    return (value << bits) | (value >> (32U - bits));
}

constexpr std::uint8_t xtime(std::uint8_t value) {
    const auto doubled = static_cast<std::uint8_t>(value << 1);
    return (value & 0x80U) != 0 ? static_cast<std::uint8_t>(doubled ^ 0x1bU) : doubled;
}

// Multiplication in GF(2^8) modulo the AES polynomial x^8 + x^4 + x^3 + x + 1.
constexpr std::uint8_t gmul(std::uint8_t a, std::uint8_t b) {
    std::uint8_t product = 0;
    for (int bit = 0; bit < 8; ++bit) {
        if ((b & 1U) != 0) {
            product = static_cast<std::uint8_t>(product ^ a);
        }
        a = xtime(a);
        b = static_cast<std::uint8_t>(b >> 1);
    }
    return product;
}

}  // namespace

// ---------------------------------------------------------------------------
// SHA-1
// ---------------------------------------------------------------------------

void Sha1::compress(const std::uint8_t* block) {
    std::array<std::uint32_t, 80> w{};
    for (std::size_t i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(block[i * 4 + 0]) << 24) |
               (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
               static_cast<std::uint32_t>(block[i * 4 + 3]);
    }
    for (std::size_t i = 16; i < 80; ++i) {
        w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];

    for (std::size_t i = 0; i < 80; ++i) {
        std::uint32_t f = 0;
        std::uint32_t k = 0;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5a827999U;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1U;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdcU;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6U;
        }
        const std::uint32_t temp = rotl32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rotl32(b, 30);
        b = a;
        a = temp;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
}

void Sha1::update(std::span<const std::uint8_t> data) { update(data.data(), data.size()); }

void Sha1::update(const void* data, std::size_t len) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    total_ += len;
    while (len > 0) {
        const std::size_t take = std::min(len, buffer_.size() - buffered_);
        std::memcpy(buffer_.data() + buffered_, bytes, take);
        buffered_ += take;
        bytes += take;
        len -= take;
        if (buffered_ == buffer_.size()) {
            compress(buffer_.data());
            buffered_ = 0;
        }
    }
}

Sha1Digest Sha1::finish() {
    // The message length is captured before padding so the padding itself does
    // not perturb the bit count.
    const std::uint64_t bits = total_ * 8U;
    const auto pad = static_cast<std::uint8_t>(0x80);
    update(&pad, 1);
    const auto zero = static_cast<std::uint8_t>(0);
    while (buffered_ != 56) {
        update(&zero, 1);
    }
    std::array<std::uint8_t, 8> length{};
    for (std::size_t i = 0; i < length.size(); ++i) {
        length[i] = static_cast<std::uint8_t>(bits >> (56U - 8U * i));
    }
    update(length.data(), length.size());

    Sha1Digest digest{};
    for (std::size_t i = 0; i < state_.size(); ++i) {
        digest[i * 4 + 0] = static_cast<std::uint8_t>(state_[i] >> 24);
        digest[i * 4 + 1] = static_cast<std::uint8_t>(state_[i] >> 16);
        digest[i * 4 + 2] = static_cast<std::uint8_t>(state_[i] >> 8);
        digest[i * 4 + 3] = static_cast<std::uint8_t>(state_[i]);
    }
    return digest;
}

Sha1Digest sha1(std::span<const std::uint8_t> data) {
    Sha1 hasher;
    hasher.update(data);
    return hasher.finish();
}

Sha1Digest sha1(std::string_view data) {
    return sha1(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(data.data()), data.size()));
}

// ---------------------------------------------------------------------------
// AES-128
// ---------------------------------------------------------------------------

namespace aes {
namespace {

// Rijndael's forward substitution box.  Upstream spells this table twice: once
// inside rijndael.c and once as table1 in itdb_hash58.c.
constexpr std::array<std::uint8_t, 256> kSbox = {
    0x63, 0x7C, 0x77, 0x7B, 0xF2, 0x6B, 0x6F, 0xC5, 0x30, 0x01, 0x67, 0x2B, 0xFE, 0xD7, 0xAB,
    0x76, 0xCA, 0x82, 0xC9, 0x7D, 0xFA, 0x59, 0x47, 0xF0, 0xAD, 0xD4, 0xA2, 0xAF, 0x9C, 0xA4,
    0x72, 0xC0, 0xB7, 0xFD, 0x93, 0x26, 0x36, 0x3F, 0xF7, 0xCC, 0x34, 0xA5, 0xE5, 0xF1, 0x71,
    0xD8, 0x31, 0x15, 0x04, 0xC7, 0x23, 0xC3, 0x18, 0x96, 0x05, 0x9A, 0x07, 0x12, 0x80, 0xE2,
    0xEB, 0x27, 0xB2, 0x75, 0x09, 0x83, 0x2C, 0x1A, 0x1B, 0x6E, 0x5A, 0xA0, 0x52, 0x3B, 0xD6,
    0xB3, 0x29, 0xE3, 0x2F, 0x84, 0x53, 0xD1, 0x00, 0xED, 0x20, 0xFC, 0xB1, 0x5B, 0x6A, 0xCB,
    0xBE, 0x39, 0x4A, 0x4C, 0x58, 0xCF, 0xD0, 0xEF, 0xAA, 0xFB, 0x43, 0x4D, 0x33, 0x85, 0x45,
    0xF9, 0x02, 0x7F, 0x50, 0x3C, 0x9F, 0xA8, 0x51, 0xA3, 0x40, 0x8F, 0x92, 0x9D, 0x38, 0xF5,
    0xBC, 0xB6, 0xDA, 0x21, 0x10, 0xFF, 0xF3, 0xD2, 0xCD, 0x0C, 0x13, 0xEC, 0x5F, 0x97, 0x44,
    0x17, 0xC4, 0xA7, 0x7E, 0x3D, 0x64, 0x5D, 0x19, 0x73, 0x60, 0x81, 0x4F, 0xDC, 0x22, 0x2A,
    0x90, 0x88, 0x46, 0xEE, 0xB8, 0x14, 0xDE, 0x5E, 0x0B, 0xDB, 0xE0, 0x32, 0x3A, 0x0A, 0x49,
    0x06, 0x24, 0x5C, 0xC2, 0xD3, 0xAC, 0x62, 0x91, 0x95, 0xE4, 0x79, 0xE7, 0xC8, 0x37, 0x6D,
    0x8D, 0xD5, 0x4E, 0xA9, 0x6C, 0x56, 0xF4, 0xEA, 0x65, 0x7A, 0xAE, 0x08, 0xBA, 0x78, 0x25,
    0x2E, 0x1C, 0xA6, 0xB4, 0xC6, 0xE8, 0xDD, 0x74, 0x1F, 0x4B, 0xBD, 0x8B, 0x8A, 0x70, 0x3E,
    0xB5, 0x66, 0x48, 0x03, 0xF6, 0x0E, 0x61, 0x35, 0x57, 0xB9, 0x86, 0xC1, 0x1D, 0x9E, 0xE1,
    0xF8, 0x98, 0x11, 0x69, 0xD9, 0x8E, 0x94, 0x9B, 0x1E, 0x87, 0xE9, 0xCE, 0x55, 0x28, 0xDF,
    0x8C, 0xA1, 0x89, 0x0D, 0xBF, 0xE6, 0x42, 0x68, 0x41, 0x99, 0x2D, 0x0F, 0xB0, 0x54, 0xBB,
    0x16};

// Rijndael's inverse substitution box (table2 in itdb_hash58.c).
constexpr std::array<std::uint8_t, 256> kInvSbox = {
    0x52, 0x09, 0x6A, 0xD5, 0x30, 0x36, 0xA5, 0x38, 0xBF, 0x40, 0xA3, 0x9E, 0x81, 0xF3, 0xD7,
    0xFB, 0x7C, 0xE3, 0x39, 0x82, 0x9B, 0x2F, 0xFF, 0x87, 0x34, 0x8E, 0x43, 0x44, 0xC4, 0xDE,
    0xE9, 0xCB, 0x54, 0x7B, 0x94, 0x32, 0xA6, 0xC2, 0x23, 0x3D, 0xEE, 0x4C, 0x95, 0x0B, 0x42,
    0xFA, 0xC3, 0x4E, 0x08, 0x2E, 0xA1, 0x66, 0x28, 0xD9, 0x24, 0xB2, 0x76, 0x5B, 0xA2, 0x49,
    0x6D, 0x8B, 0xD1, 0x25, 0x72, 0xF8, 0xF6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xD4, 0xA4, 0x5C,
    0xCC, 0x5D, 0x65, 0xB6, 0x92, 0x6C, 0x70, 0x48, 0x50, 0xFD, 0xED, 0xB9, 0xDA, 0x5E, 0x15,
    0x46, 0x57, 0xA7, 0x8D, 0x9D, 0x84, 0x90, 0xD8, 0xAB, 0x00, 0x8C, 0xBC, 0xD3, 0x0A, 0xF7,
    0xE4, 0x58, 0x05, 0xB8, 0xB3, 0x45, 0x06, 0xD0, 0x2C, 0x1E, 0x8F, 0xCA, 0x3F, 0x0F, 0x02,
    0xC1, 0xAF, 0xBD, 0x03, 0x01, 0x13, 0x8A, 0x6B, 0x3A, 0x91, 0x11, 0x41, 0x4F, 0x67, 0xDC,
    0xEA, 0x97, 0xF2, 0xCF, 0xCE, 0xF0, 0xB4, 0xE6, 0x73, 0x96, 0xAC, 0x74, 0x22, 0xE7, 0xAD,
    0x35, 0x85, 0xE2, 0xF9, 0x37, 0xE8, 0x1C, 0x75, 0xDF, 0x6E, 0x47, 0xF1, 0x1A, 0x71, 0x1D,
    0x29, 0xC5, 0x89, 0x6F, 0xB7, 0x62, 0x0E, 0xAA, 0x18, 0xBE, 0x1B, 0xFC, 0x56, 0x3E, 0x4B,
    0xC6, 0xD2, 0x79, 0x20, 0x9A, 0xDB, 0xC0, 0xFE, 0x78, 0xCD, 0x5A, 0xF4, 0x1F, 0xDD, 0xA8,
    0x33, 0x88, 0x07, 0xC7, 0x31, 0xB1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xEC, 0x5F, 0x60, 0x51,
    0x7F, 0xA9, 0x19, 0xB5, 0x4A, 0x0D, 0x2D, 0xE5, 0x7A, 0x9F, 0x93, 0xC9, 0x9C, 0xEF, 0xA0,
    0xE0, 0x3B, 0x4D, 0xAE, 0x2A, 0xF5, 0xB0, 0xC8, 0xEB, 0xBB, 0x3C, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2B, 0x04, 0x7E, 0xBA, 0x77, 0xD6, 0x26, 0xE1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0C,
    0x7D};

constexpr std::size_t kRounds = 10;
constexpr std::size_t kRoundKeySize = block_size * (kRounds + 1);

using RoundKeys = std::array<std::uint8_t, kRoundKeySize>;
using State = std::array<std::uint8_t, block_size>;

RoundKeys expand_key(const Key& key) {
    RoundKeys round_keys{};
    std::copy(key.begin(), key.end(), round_keys.begin());

    std::uint8_t rcon = 1;
    for (std::size_t offset = block_size; offset < round_keys.size(); offset += 4) {
        std::array<std::uint8_t, 4> temp = {round_keys[offset - 4], round_keys[offset - 3],
                                            round_keys[offset - 2], round_keys[offset - 1]};
        if (offset % block_size == 0) {
            // RotWord, then SubWord, then xor the round constant into the head.
            const std::uint8_t first = temp[0];
            temp[0] = static_cast<std::uint8_t>(kSbox[temp[1]] ^ rcon);
            temp[1] = kSbox[temp[2]];
            temp[2] = kSbox[temp[3]];
            temp[3] = kSbox[first];
            rcon = xtime(rcon);
        }
        for (std::size_t i = 0; i < temp.size(); ++i) {
            round_keys[offset + i] =
                static_cast<std::uint8_t>(round_keys[offset + i - block_size] ^ temp[i]);
        }
    }
    return round_keys;
}

void add_round_key(State& state, const RoundKeys& keys, std::size_t round) {
    for (std::size_t i = 0; i < block_size; ++i) {
        state[i] = static_cast<std::uint8_t>(state[i] ^ keys[round * block_size + i]);
    }
}

// The state is kept in input order, so byte 4*c+r belongs to row r and column
// c.  ShiftRows therefore rotates the bytes r, r+4, r+8, r+12 left by r.
void shift_rows(State& state) {
    for (std::size_t row = 1; row < 4; ++row) {
        State copy = state;
        for (std::size_t column = 0; column < 4; ++column) {
            state[row + 4 * column] = copy[row + 4 * ((column + row) % 4)];
        }
    }
}

void inv_shift_rows(State& state) {
    for (std::size_t row = 1; row < 4; ++row) {
        State copy = state;
        for (std::size_t column = 0; column < 4; ++column) {
            state[row + 4 * column] = copy[row + 4 * ((column + 4 - row) % 4)];
        }
    }
}

void sub_bytes(State& state) {
    for (auto& byte : state) {
        byte = kSbox[byte];
    }
}

void inv_sub_bytes(State& state) {
    for (auto& byte : state) {
        byte = kInvSbox[byte];
    }
}

void mix_columns(State& state) {
    for (std::size_t column = 0; column < 4; ++column) {
        std::uint8_t* a = state.data() + 4 * column;
        const std::array<std::uint8_t, 4> in = {a[0], a[1], a[2], a[3]};
        for (std::size_t row = 0; row < 4; ++row) {
            a[row] = static_cast<std::uint8_t>(gmul(in[row], 2) ^ gmul(in[(row + 1) % 4], 3) ^
                                               in[(row + 2) % 4] ^ in[(row + 3) % 4]);
        }
    }
}

void inv_mix_columns(State& state) {
    for (std::size_t column = 0; column < 4; ++column) {
        std::uint8_t* a = state.data() + 4 * column;
        const std::array<std::uint8_t, 4> in = {a[0], a[1], a[2], a[3]};
        for (std::size_t row = 0; row < 4; ++row) {
            a[row] = static_cast<std::uint8_t>(
                gmul(in[row], 14) ^ gmul(in[(row + 1) % 4], 11) ^ gmul(in[(row + 2) % 4], 13) ^
                gmul(in[(row + 3) % 4], 9));
        }
    }
}

std::size_t padded_size(std::size_t size) { return ((size + block_size - 1) / block_size) * block_size; }

}  // namespace

const std::array<std::uint8_t, 256>& sbox() { return kSbox; }
const std::array<std::uint8_t, 256>& inv_sbox() { return kInvSbox; }

Block encrypt_block(const Key& key, const Block& block) {
    const RoundKeys keys = expand_key(key);
    State state = block;

    add_round_key(state, keys, 0);
    for (std::size_t round = 1; round < kRounds; ++round) {
        sub_bytes(state);
        shift_rows(state);
        mix_columns(state);
        add_round_key(state, keys, round);
    }
    sub_bytes(state);
    shift_rows(state);
    add_round_key(state, keys, kRounds);
    return state;
}

Block decrypt_block(const Key& key, const Block& block) {
    const RoundKeys keys = expand_key(key);
    State state = block;

    add_round_key(state, keys, kRounds);
    for (std::size_t round = kRounds; round-- > 1;) {
        inv_shift_rows(state);
        inv_sub_bytes(state);
        add_round_key(state, keys, round);
        inv_mix_columns(state);
    }
    inv_shift_rows(state);
    inv_sub_bytes(state);
    add_round_key(state, keys, 0);
    return state;
}

void cbc_encrypt(const Key& key, const Block& iv, std::span<const std::uint8_t> in,
                 std::span<std::uint8_t> out) {
    if (out.size() < padded_size(in.size())) {
        fail(ErrorCode::InvalidArgument, "AES-CBC output buffer is too small");
    }
    Block chain = iv;
    const std::size_t whole = in.size() / block_size;
    for (std::size_t block = 0; block < whole; ++block) {
        Block plain{};
        for (std::size_t i = 0; i < block_size; ++i) {
            plain[i] = static_cast<std::uint8_t>(in[block * block_size + i] ^ chain[i]);
        }
        chain = encrypt_block(key, plain);
        std::copy(chain.begin(), chain.end(), out.begin() + static_cast<std::ptrdiff_t>(
                                                       block * block_size));
    }

    const std::size_t fraction = in.size() % block_size;
    if (fraction != 0) {
        // The trailing bytes are zero, not the chained block, exactly as the
        // original loop leaves them.
        Block plain{};
        for (std::size_t i = 0; i < fraction; ++i) {
            plain[i] = static_cast<std::uint8_t>(in[whole * block_size + i] ^ chain[i]);
        }
        chain = encrypt_block(key, plain);
        std::copy(chain.begin(), chain.end(),
                  out.begin() + static_cast<std::ptrdiff_t>(whole * block_size));
    }
}

void cbc_decrypt(const Key& key, const Block& iv, std::span<const std::uint8_t> in,
                 std::span<std::uint8_t> out) {
    if (out.size() < in.size()) {
        fail(ErrorCode::InvalidArgument, "AES-CBC output buffer is too small");
    }
    const std::size_t whole = in.size() / block_size;
    for (std::size_t block = 0; block <= whole; ++block) {
        std::size_t fraction = block_size;
        if (block == whole) {
            fraction = in.size() % block_size;
            if (fraction == 0) {
                break;
            }
        }
        Block cipher{};
        std::memcpy(cipher.data(), in.data() + block * block_size, fraction);
        const Block plain = decrypt_block(key, cipher);
        const std::uint8_t* chain = block == 0 ? iv.data() : in.data() + (block - 1) * block_size;
        for (std::size_t i = 0; i < fraction; ++i) {
            out[block * block_size + i] = static_cast<std::uint8_t>(chain[i] ^ plain[i]);
        }
    }
}

}  // namespace aes

// ---------------------------------------------------------------------------
// Header helpers
// ---------------------------------------------------------------------------

namespace {

void store_u16le(std::span<std::uint8_t> data, std::size_t offset, std::uint16_t value) {
    data[offset] = static_cast<std::uint8_t>(value & 0xffU);
    data[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

void zero_field(std::vector<std::uint8_t>& image, std::size_t offset, std::size_t length) {
    std::fill(image.begin() + static_cast<std::ptrdiff_t>(offset),
              image.begin() + static_cast<std::ptrdiff_t>(offset + length),
              static_cast<std::uint8_t>(0));
}

void copy_out(std::span<const std::uint8_t> source, std::span<std::uint8_t> target,
              std::size_t offset) {
    std::copy(source.begin(), source.end(),
              target.begin() + static_cast<std::ptrdiff_t>(offset));
}

void require_mhbd(std::span<const std::uint8_t> image) {
    if (image.size() < 4 || std::memcmp(image.data(), "mhbd", 4) != 0) {
        fail(ErrorCode::Corrupt, "buffer is not an iTunesDB image");
    }
}

[[nodiscard]] int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1;
}

// itdb_hex_from_string: an optional 0x prefix followed by hex digit pairs.
[[nodiscard]] std::optional<std::array<std::uint8_t, 20>> try_parse_uuid(std::string_view text) {
    if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text.remove_prefix(2);
    }
    if (text.empty() || text.size() > 40 || text.size() % 2 != 0) {
        return std::nullopt;
    }
    std::array<std::uint8_t, 20> uuid{};
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const int high = hex_value(text[i]);
        const int low = hex_value(text[i + 1]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        uuid[i / 2] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return uuid;
}

// hash58 key derivation constant from itdb_hash58.c.
constexpr std::array<std::uint8_t, 18> kHash58Fixed = {0x67, 0x23, 0xFE, 0x30, 0x45, 0x33,
                                                       0xF8, 0x90, 0x99, 0x21, 0x07, 0xC1,
                                                       0xD0, 0x12, 0xB2, 0xA1, 0x07, 0x81};

int gcd(int a, int b) {
    while (true) {
        a = a % b;
        if (a == 0) {
            return b;
        }
        b = b % a;
        if (b == 0) {
            return a;
        }
    }
}

int lcm(int a, int b) {
    if (a == 0 || b == 0) {
        return 1;
    }
    return (a * b) / gcd(a, b);
}

HashAbSigner& hashab_signer() {
    static HashAbSigner signer;
    return signer;
}

}  // namespace

// ---------------------------------------------------------------------------
// iTunesDB SHA-1 inputs
// ---------------------------------------------------------------------------

Sha1Digest itunesdb_sha1(std::span<const std::uint8_t> db, ChecksumType type) {
    std::size_t required = 0;
    switch (type) {
        case ChecksumType::Hash58:
            required = kMhbdHash58Offset + kMhbdHash58Size;
            break;
        case ChecksumType::Hash72:
            required = kMhbdHash72Offset + kMhbdHash72Size;
            break;
        case ChecksumType::HashAB:
            required = kMhbdHashABOffset + kMhbdHashABSize;
            break;
        default:
            fail(ErrorCode::InvalidArgument, "no checksum scheme selected");
    }
    if (db.size() < required) {
        fail(ErrorCode::Corrupt, "iTunesDB file too small to hold the checksum fields");
    }

    std::vector<std::uint8_t> image(db.begin(), db.end());
    zero_field(image, kMhbdDbIdOffset, kMhbdDbIdSize);
    zero_field(image, kMhbdHash58Offset, kMhbdHash58Size);
    if (type == ChecksumType::Hash58) {
        zero_field(image, kMhbdUnk0x32Offset, kMhbdUnk0x32Size);
    } else {
        zero_field(image, kMhbdHash72Offset, kMhbdHash72Size);
    }
    if (type == ChecksumType::HashAB) {
        zero_field(image, kMhbdHashABOffset, kMhbdHashABSize);
    }
    return sha1(image);
}

// ---------------------------------------------------------------------------
// FirewireGuid
// ---------------------------------------------------------------------------

std::array<std::uint8_t, 20> firewire_id_bytes(const Device& device) {
    const auto parsed = try_parse_uuid(device.firewire_id());
    if (!parsed) {
        fail(ErrorCode::NotFound, "couldn't find the iPod firewire ID");
    }
    return *parsed;
}

// ---------------------------------------------------------------------------
// hash58
// ---------------------------------------------------------------------------

std::array<std::uint8_t, 64> hash58_key(std::span<const std::uint8_t, 20> firewire_id) {
    std::array<std::uint8_t, 16> y{};
    for (std::size_t pair = 0; pair < 4; ++pair) {
        const auto a = static_cast<int>(firewire_id[pair * 2]);
        const auto b = static_cast<int>(firewire_id[pair * 2 + 1]);
        const int current = lcm(a, b);
        const auto high = static_cast<std::uint8_t>((current & 0xff00) >> 8);
        const auto low = static_cast<std::uint8_t>(current & 0xff);
        y[pair * 4 + 0] = aes::sbox()[high];
        y[pair * 4 + 1] = aes::inv_sbox()[high];
        y[pair * 4 + 2] = aes::sbox()[low];
        y[pair * 4 + 3] = aes::inv_sbox()[low];
    }

    Sha1 hasher;
    hasher.update(kHash58Fixed);
    hasher.update(y);
    const Sha1Digest digest = hasher.finish();

    std::array<std::uint8_t, 64> key{};
    std::copy(digest.begin(), digest.end(), key.begin());
    return key;
}

Sha1Digest hash58_checksum(std::span<const std::uint8_t, 20> firewire_id,
                           std::span<const std::uint8_t> db) {
    std::array<std::uint8_t, 64> key = hash58_key(firewire_id);

    // HMAC-SHA1 with the zero padded key block: inner pass first.
    for (auto& byte : key) {
        byte = static_cast<std::uint8_t>(byte ^ 0x36U);
    }
    Sha1 inner;
    inner.update(key);
    inner.update(db);
    const Sha1Digest inner_digest = inner.finish();

    for (auto& byte : key) {
        byte = static_cast<std::uint8_t>(byte ^ 0x36U ^ 0x5cU);
    }
    Sha1 outer;
    outer.update(key);
    outer.update(inner_digest);
    return outer.finish();
}

void hash58_write(const Device& device, std::span<std::uint8_t> db) {
    if (db.size() < kMinHash58DatabaseSize) {
        fail(ErrorCode::Corrupt, "iTunesDB file too small to write checksum");
    }
    require_mhbd(db);
    const auto firewire_id = firewire_id_bytes(device);

    std::array<std::uint8_t, kMhbdDbIdSize> backup_id{};
    std::array<std::uint8_t, kMhbdUnk0x32Size> backup_unk{};
    std::copy_n(db.begin() + static_cast<std::ptrdiff_t>(kMhbdDbIdOffset), kMhbdDbIdSize,
                backup_id.begin());
    std::copy_n(db.begin() + static_cast<std::ptrdiff_t>(kMhbdUnk0x32Offset), kMhbdUnk0x32Size,
                backup_unk.begin());

    std::fill_n(db.begin() + static_cast<std::ptrdiff_t>(kMhbdDbIdOffset), kMhbdDbIdSize,
                static_cast<std::uint8_t>(0));
    std::fill_n(db.begin() + static_cast<std::ptrdiff_t>(kMhbdUnk0x32Offset), kMhbdUnk0x32Size,
                static_cast<std::uint8_t>(0));
    std::fill_n(db.begin() + static_cast<std::ptrdiff_t>(kMhbdHash58Offset), kMhbdHash58Size,
                static_cast<std::uint8_t>(0));
    store_u16le(db, kMhbdHashingSchemeOffset, kHashingSchemeHash58);

    const Sha1Digest checksum = hash58_checksum(firewire_id, db);
    copy_out(checksum, db, kMhbdHash58Offset);

    std::copy(backup_id.begin(), backup_id.end(),
              db.begin() + static_cast<std::ptrdiff_t>(kMhbdDbIdOffset));
    std::copy(backup_unk.begin(), backup_unk.end(),
              db.begin() + static_cast<std::ptrdiff_t>(kMhbdUnk0x32Offset));
}

// ---------------------------------------------------------------------------
// hash72
// ---------------------------------------------------------------------------

std::filesystem::path hash_info_path(const Device& device) {
    return device_dir(device.mountpoint) / "HashInfo";
}

std::optional<HashInfo> read_hash_info(const Device& device) {
    const auto uuid = try_parse_uuid(device.firewire_id());
    if (!uuid) {
        return std::nullopt;
    }
    const std::filesystem::path path = hash_info_path(device);
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        return std::nullopt;
    }
    Bytes data;
    try {
        data = read_file(path);
    } catch (const Error&) {
        return std::nullopt;
    }
    if (data.size() != kHashInfoSize) {
        return std::nullopt;
    }
    if (std::memcmp(data.data() + 6, uuid->data(), uuid->size()) != 0) {
        // The cached signature parameters belong to another device; upstream
        // deletes the file so the next parse can rebuild it.
        std::filesystem::remove(path, error);
        return std::nullopt;
    }

    HashInfo info;
    std::copy_n(data.begin() + 6, info.uuid.size(), info.uuid.begin());
    std::copy_n(data.begin() + 26, info.random_bytes.size(), info.random_bytes.begin());
    std::copy_n(data.begin() + 38, info.iv.size(), info.iv.begin());
    return info;
}

void write_hash_info(const Device& device, const HashInfo& info) {
    const auto uuid = firewire_id_bytes(device);

    Bytes data(kHashInfoSize, 0);
    std::copy(kHashInfoHeader.begin(), kHashInfoHeader.end(), data.begin());
    std::copy(uuid.begin(), uuid.end(), data.begin() + 6);
    std::copy(info.random_bytes.begin(), info.random_bytes.end(), data.begin() + 26);
    std::copy(info.iv.begin(), info.iv.end(), data.begin() + 38);

    write_file_atomic(hash_info_path(device), data);
}

Hash72Signature hash72_generate(std::span<const std::uint8_t, 20> sha1,
                                const Hash72Iv& iv,
                                const Hash72RandomBytes& random_bytes) {
    std::array<std::uint8_t, 2 * aes::block_size> plaintext{};
    std::copy(sha1.begin(), sha1.end(), plaintext.begin());
    std::copy(random_bytes.begin(), random_bytes.end(), plaintext.begin() + 20);

    Hash72Signature signature{};
    signature[0] = 0x01;
    signature[1] = 0x00;
    std::copy(random_bytes.begin(), random_bytes.end(), signature.begin() + 2);

    std::array<std::uint8_t, 2 * aes::block_size> output{};
    aes::cbc_encrypt(kHash72AesKey, iv, plaintext, output);
    std::copy(output.begin(), output.end(), signature.begin() + 14);
    return signature;
}

std::pair<Hash72Iv, Hash72RandomBytes> hash72_extract(const Hash72Signature& signature,
                                                      std::span<const std::uint8_t, 20> sha1) {
    if (signature[0] != 0x01 || signature[1] != 0x00) {
        fail(ErrorCode::Corrupt, "invalid hash72 signature prefix");
    }

    std::array<std::uint8_t, 2 * aes::block_size> plaintext{};
    std::copy(sha1.begin(), sha1.end(), plaintext.begin());
    std::copy_n(signature.begin() + 2, 12, plaintext.begin() + 20);

    // Upstream decrypts the first cipher block with the known plaintext as the
    // CBC initial value, which yields the IV directly.  Its follow-up "uh oh"
    // comparison is vacuous, so an unverifiable signature is accepted here too.
    Hash72Iv iv{};
    std::copy(plaintext.begin(), plaintext.begin() + static_cast<std::ptrdiff_t>(aes::block_size),
              iv.begin());
    std::array<std::uint8_t, aes::block_size> decrypted{};
    aes::cbc_decrypt(kHash72AesKey, iv,
                     std::span<const std::uint8_t>(signature.data() + 14, aes::block_size),
                     decrypted);

    Hash72Iv recovered_iv{};
    std::copy(decrypted.begin(), decrypted.end(), recovered_iv.begin());
    Hash72RandomBytes random_bytes{};
    std::copy_n(signature.begin() + 2, random_bytes.size(), random_bytes.begin());
    return {recovered_iv, random_bytes};
}

void hash72_apply(std::span<std::uint8_t> db, const Hash72Iv& iv,
                  const Hash72RandomBytes& random_bytes) {
    if (db.size() < kMinHash72DatabaseSize) {
        fail(ErrorCode::Corrupt, "iTunesDB file too small to write checksum");
    }
    require_mhbd(db);
    store_u16le(db, kMhbdHashingSchemeOffset, kHashingSchemeHash72);
    const Sha1Digest digest = itunesdb_sha1(db, ChecksumType::Hash72);
    const Hash72Signature signature = hash72_generate(digest, iv, random_bytes);
    copy_out(signature, db, kMhbdHash72Offset);
}

Hash72Signature hash72_compute_for_sha1(const Device& device,
                                        std::span<const std::uint8_t, 20> sha1) {
    const auto info = read_hash_info(device);
    if (!info) {
        fail(ErrorCode::NotFound,
             "can't write iPod database because of missing HashInfo file");
    }
    return hash72_generate(sha1, info->iv, info->random_bytes);
}

bool hash72_extract_hash_info(const Device& device, std::span<const std::uint8_t> db) {
    if (device.checksum_type() != ChecksumType::Hash72 || !device.supports_sqlite_db()) {
        return false;
    }
    if (db.size() < kMinHash72DatabaseSize) {
        return false;
    }
    if (read_hash_info(device)) {
        return true;
    }

    Hash72Signature signature{};
    std::copy_n(db.begin() + static_cast<std::ptrdiff_t>(kMhbdHash72Offset),
                kMhbdHash72Size, signature.begin());

    try {
        const Sha1Digest digest = itunesdb_sha1(db, ChecksumType::Hash72);
        const auto [iv, random_bytes] = hash72_extract(signature, digest);
        HashInfo info;
        info.random_bytes = random_bytes;
        info.iv = iv;
        write_hash_info(device, info);
    } catch (const Error&) {
        return false;
    }
    return true;
}

void hash72_write(const Device& device, std::span<std::uint8_t> db) {
    if (db.size() < kMinHash72DatabaseSize) {
        fail(ErrorCode::Corrupt, "iTunesDB file too small to write checksum");
    }
    require_mhbd(db);
    const auto info = read_hash_info(device);
    if (!info) {
        fail(ErrorCode::NotFound,
             "can't write iPod database because of missing HashInfo file");
    }
    store_u16le(db, kMhbdHashingSchemeOffset, kHashingSchemeHash72);
    hash72_apply(db, info->iv, info->random_bytes);
}

// ---------------------------------------------------------------------------
// hashAB
// ---------------------------------------------------------------------------

void set_hashab_signer(HashAbSigner signer) { hashab_signer() = std::move(signer); }

bool has_hashab_signer() { return static_cast<bool>(hashab_signer()); }

HashAbSignature hashAB_compute_for_sha1(const Device& device,
                                        std::span<const std::uint8_t, 20> sha1) {
    HashAbSigner& signer = hashab_signer();
    if (!signer) {
        fail(ErrorCode::Unsupported, "unsupported checksum type");
    }
    const auto uuid = firewire_id_bytes(device);
    HashAbSignature signature{};
    signer(signature, sha1, uuid, kHashAbRandomBytes);
    return signature;
}

void hashAB_write(const Device& device, std::span<std::uint8_t> db) {
    if (db.size() < kMinHashABDatabaseSize) {
        fail(ErrorCode::Corrupt, "iTunesDB file too small to write checksum");
    }
    require_mhbd(db);
    if (!has_hashab_signer()) {
        fail(ErrorCode::Unsupported, "unsupported checksum type");
    }
    store_u16le(db, kMhbdHashingSchemeOffset, kHashingSchemeHashAB);
    const Sha1Digest digest = itunesdb_sha1(db, ChecksumType::HashAB);
    const HashAbSignature signature = hashAB_compute_for_sha1(device, digest);
    copy_out(signature, db, kMhbdHashABOffset);
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

void write_checksum(const Device& device, std::span<std::uint8_t> db) {
    switch (device.checksum_type()) {
        case ChecksumType::None:
            return;
        case ChecksumType::Hash58:
            hash58_write(device, db);
            return;
        case ChecksumType::Hash72:
            hash72_write(device, db);
            return;
        case ChecksumType::HashAB:
            hashAB_write(device, db);
            return;
        case ChecksumType::Unknown:
            fail(ErrorCode::Unsupported, "unsupported checksum type");
    }
}

}  // namespace gpod
