// Checksums and crypto primitives for the iPod database files.
//
// This is the pure C++ port of upstream src/rijndael.c, src/itdb_hash58.c,
// src/itdb_hash72.c and src/itdb_hashAB.c.  It provides:
//
//   * SHA-1 (used by every checksum scheme and by the cbk generator),
//   * AES-128 / Rijndael (nb=4, nk=4) block and CBC primitives,
//   * the hash58 HMAC-SHA1 checksum written into the MHBD header,
//   * the hash72 AES-CBC signature plus its per-device HashInfo file,
//   * the hashAB signature plumbing (the digest itself is supplied by a
//     signer callback, see set_hashab_signer).
//
// Everything operates on a mutable iTunesDB image.  The header layout is fixed
// by the MHBD structure in db-itunes-parser.h; the offsets below are the byte
// positions inside that structure:
//
//   0x00  header_id[4]        "mhbd"
//   0x18  db_id[8]            zeroed for the SHA-1 input
//   0x30  hashing_scheme u16  little endian; 1 = hash58, 2 = hash72, 4 = hashAB
//   0x32  unk_0x32[20]        zeroed for the hash58 SHA-1 input only
//   0x58  hash58[20]
//   0x72  hash72[46]
//   0xaa  hashAB[57]
//
// Failure is reported by throwing gpod::Error, matching error.hpp; the
// helpers that upstream models as "maybe missing" return std::optional or bool.

#ifndef GPOD_HASH_HPP
#define GPOD_HASH_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "gpod/types.hpp"

namespace gpod {

class Device;

// ---------------------------------------------------------------------------
// MHBD header geometry used by the checksums
// ---------------------------------------------------------------------------

inline constexpr std::size_t kMhbdHeaderMagicOffset = 0x00;
inline constexpr std::size_t kMhbdDbIdOffset = 0x18;
inline constexpr std::size_t kMhbdDbIdSize = 8;
inline constexpr std::size_t kMhbdHashingSchemeOffset = 0x30;
inline constexpr std::size_t kMhbdUnk0x32Offset = 0x32;
inline constexpr std::size_t kMhbdUnk0x32Size = 20;
inline constexpr std::size_t kMhbdHash58Offset = 0x58;
inline constexpr std::size_t kMhbdHash58Size = 20;
inline constexpr std::size_t kMhbdHash72Offset = 0x72;
inline constexpr std::size_t kMhbdHash72Size = 46;
inline constexpr std::size_t kMhbdHashABOffset = 0xaa;
inline constexpr std::size_t kMhbdHashABSize = 57;

// Smallest database image each scheme can sign, mirroring the upstream length
// checks: hash58/hash72 need the whole hash58 field, hashAB additionally needs
// its 57 byte signature plus the trailing padding the header declares.  The
// SHA-1 input needs a little more room because the hash72 and hashAB fields are
// blanked first, see itunesdb_sha1.
inline constexpr std::size_t kMinHash58DatabaseSize = 0x6c;
inline constexpr std::size_t kMinHash72DatabaseSize = 0x6c;
inline constexpr std::size_t kMinHashABDatabaseSize = 0xf4;

// Values stored in MhbdHeader::hashing_scheme.
inline constexpr std::uint16_t kHashingSchemeNone = 0;
inline constexpr std::uint16_t kHashingSchemeHash58 = 1;
inline constexpr std::uint16_t kHashingSchemeHash72 = 2;
inline constexpr std::uint16_t kHashingSchemeHashAB = 4;

// ---------------------------------------------------------------------------
// SHA-1
// ---------------------------------------------------------------------------

using Sha1Digest = std::array<std::uint8_t, 20>;

// Streaming SHA-1.  update() may be called any number of times; finish()
// returns the digest and leaves the hasher unusable.
class Sha1 {
public:
    Sha1() = default;

    void update(std::span<const std::uint8_t> data);
    void update(const void* data, std::size_t len);
    [[nodiscard]] Sha1Digest finish();

private:
    void compress(const std::uint8_t* block);

    std::array<std::uint32_t, 5> state_{0x67452301U, 0xefcdab89U, 0x98badcfeU,
                                        0x10325476U, 0xc3d2e1f0U};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffered_ = 0;
    std::uint64_t total_ = 0;
};

[[nodiscard]] Sha1Digest sha1(std::span<const std::uint8_t> data);
[[nodiscard]] Sha1Digest sha1(std::string_view data);

// ---------------------------------------------------------------------------
// AES-128 (Rijndael with a 128 bit block and a 128 bit key)
// ---------------------------------------------------------------------------

namespace aes {

inline constexpr std::size_t block_size = 16;
inline constexpr std::size_t key_size = 16;

using Block = std::array<std::uint8_t, block_size>;
using Key = std::array<std::uint8_t, key_size>;

// The Rijndael forward and inverse substitution boxes.  hash58's key
// derivation uses the same 256 byte tables, so they are shared.
[[nodiscard]] const std::array<std::uint8_t, 256>& sbox();
[[nodiscard]] const std::array<std::uint8_t, 256>& inv_sbox();

[[nodiscard]] Block encrypt_block(const Key& key, const Block& block);
[[nodiscard]] Block decrypt_block(const Key& key, const Block& block);

// CBC encryption/decryption with the semantics of upstream rijndael.c: a
// trailing partial block is zero padded into the cipher block but only the
// meaningful bytes are combined.  cbc_encrypt() always writes whole cipher
// blocks, so `out` must hold ((in.size() + 15) / 16) * 16 bytes; cbc_decrypt()
// writes `in.size()` bytes and needs no more than that.
void cbc_encrypt(const Key& key, const Block& iv, std::span<const std::uint8_t> in,
                 std::span<std::uint8_t> out);
void cbc_decrypt(const Key& key, const Block& iv, std::span<const std::uint8_t> in,
                 std::span<std::uint8_t> out);

}  // namespace aes

// ---------------------------------------------------------------------------
// iTunesDB SHA-1 inputs
// ---------------------------------------------------------------------------

// SHA-1 over the whole database image with the header fields that the given
// scheme excludes zeroed first.  The buffer is copied, so `db` is left
// untouched and `hashing_scheme` is not modified (the write functions store
// the scheme themselves before hashing).
//
//   ChecksumType::Hash58 : db_id, unk_0x32 and hash58 are zeroed
//   ChecksumType::Hash72 : db_id, hash58 and hash72 are zeroed
//   ChecksumType::HashAB : db_id, hash58, hash72 and hashAB are zeroed
//
// Any other value throws Error(ErrorCode::InvalidArgument); a buffer shorter
// than the highest zeroed field throws Error(ErrorCode::Corrupt): 0x6c for
// hash58, 0xa0 for hash72 and 0xe3 for hashAB.
[[nodiscard]] Sha1Digest itunesdb_sha1(std::span<const std::uint8_t> db, ChecksumType type);

// ---------------------------------------------------------------------------
// FirewireGuid parsing (itdb_device_get_hex_uuid)
// ---------------------------------------------------------------------------

// Parse the device's FirewireGuid ("0xABCDEF..." or bare hex) into its 20 byte
// binary form.  Throws Error(ErrorCode::NotFound) when the device has no
// FirewireGuid and Error(ErrorCode::InvalidArgument) when it is not hex.
[[nodiscard]] std::array<std::uint8_t, 20> firewire_id_bytes(const Device& device);

// ---------------------------------------------------------------------------
// hash58
// ---------------------------------------------------------------------------

// Key schedule of the hash58 HMAC-SHA1: the digest of an 18 byte constant
// followed by a 16 byte value built from the least common multiples of the
// FirewireGuid byte pairs, zero padded to 64 bytes.
[[nodiscard]] std::array<std::uint8_t, 64> hash58_key(
    std::span<const std::uint8_t, 20> firewire_id);

// HMAC-SHA1 over the whole (already zeroed) database image with that key.
[[nodiscard]] Sha1Digest hash58_checksum(std::span<const std::uint8_t, 20> firewire_id,
                                         std::span<const std::uint8_t> db);

// Zero db_id, unk_0x32 and hash58, store hashing_scheme = 1, compute the
// checksum over the whole image and copy its 20 bytes to offset 0x58, then
// restore db_id and unk_0x32.  Requires a buffer of at least 0x6c bytes that
// starts with "mhbd".
void hash58_write(const Device& device, std::span<std::uint8_t> db);

// ---------------------------------------------------------------------------
// hash72
// ---------------------------------------------------------------------------

// Fixed AES-128 key used by the hash72 signature.
inline constexpr aes::Key kHash72AesKey = {0x61, 0x8c, 0xa1, 0x0d, 0xc7, 0xf5, 0x7f, 0xd3,
                                           0xb4, 0x72, 0x3e, 0x08, 0x15, 0x74, 0x63, 0xd7};

using Hash72Signature = std::array<std::uint8_t, kMhbdHash72Size>;
using Hash72Iv = aes::Block;
using Hash72RandomBytes = std::array<std::uint8_t, 12>;

// Contents of the per-device "\iPod_Control\Device\HashInfo" file, which
// caches the IV and random bytes recovered from an existing signature.
struct HashInfo {
    std::array<std::uint8_t, 20> uuid{};
    Hash72RandomBytes random_bytes{};
    Hash72Iv iv{};
};

inline constexpr std::size_t kHashInfoSize = 6 + 20 + 12 + 16;
inline constexpr std::array<char, 6> kHashInfoHeader = {'H', 'A', 'S', 'H', 'v', '0'};

[[nodiscard]] std::filesystem::path hash_info_path(const Device& device);

// Read the HashInfo file.  Returns nullopt when it is missing, has the wrong
// size, or belongs to another device; a stale file for another device is
// removed, as upstream does.
[[nodiscard]] std::optional<HashInfo> read_hash_info(const Device& device);

// Write the HashInfo file atomically.  The uuid field is taken from the
// device's FirewireGuid, as upstream does, so `info.uuid` is ignored.  Throws
// Error on I/O failure.
void write_hash_info(const Device& device, const HashInfo& info);

// Build the 46 byte signature: 01 00, the 12 random bytes, then the AES-CBC
// encryption of sha1 || random_bytes under kHash72AesKey with `iv`.
[[nodiscard]] Hash72Signature hash72_generate(std::span<const std::uint8_t, 20> sha1,
                                              const Hash72Iv& iv,
                                              const Hash72RandomBytes& random_bytes);

// Recover (iv, random_bytes) from a signature and the SHA-1 it signs.  Throws
// Error(ErrorCode::Corrupt) when the signature does not start with 01 00.
[[nodiscard]] std::pair<Hash72Iv, Hash72RandomBytes> hash72_extract(
    const Hash72Signature& signature, std::span<const std::uint8_t, 20> sha1);

// Store hashing_scheme = 2, compute the SHA-1 over `db` and write the
// signature to offset 0x72 using `iv` and `random_bytes`.
void hash72_apply(std::span<std::uint8_t> db, const Hash72Iv& iv,
                  const Hash72RandomBytes& random_bytes);

// Sign `sha1` with the HashInfo cached for the device.  Throws
// Error(ErrorCode::NotFound) when the HashInfo file is missing, which is what
// upstream reports as "can't write iPod database".
[[nodiscard]] Hash72Signature hash72_compute_for_sha1(const Device& device,
                                                      std::span<const std::uint8_t, 20> sha1);

// Called while parsing an iTunesDB: if the device uses hash72 and the HashInfo
// file is absent, derive the IV and random bytes from the existing signature
// and save them.  Returns true when a usable HashInfo file exists afterwards.
[[nodiscard]] bool hash72_extract_hash_info(const Device& device,
                                           std::span<const std::uint8_t> db);

// Store hashing_scheme = 2 and write the signature at offset 0x72.  Requires a
// HashInfo file for the device; the buffer must be at least 0xa0 bytes
// ("mhbd" header plus the hash58 and hash72 fields) because both fields are
// blanked before hashing.
void hash72_write(const Device& device, std::span<std::uint8_t> db);

// ---------------------------------------------------------------------------
// hashAB
// ---------------------------------------------------------------------------

// The hashAB digest lives in a separate, licence restricted blob in upstream
// libgpod (calcHashAB).  This port has no equivalent, so callers register a
// signer; without one, hashAB_write and hashAB_compute_for_sha1 throw
// Error(ErrorCode::Unsupported).
using HashAbSignature = std::array<std::uint8_t, kMhbdHashABSize>;

// Replacement for the blob's calcHashAB entry point: fills the 57 byte
// signature from the database SHA-1, the 20 byte device uuid and the 23 random
// bytes.  Upstream loads these from a libhashab shared object.
using HashAbSigner = std::function<void(HashAbSignature& signature,
                                        std::span<const std::uint8_t, 20> sha1,
                                        std::span<const std::uint8_t, 20> uuid,
                                        std::span<const std::uint8_t, 23> random_bytes)>;

// The 23 random bytes upstream passes as rndpart ("ABCDEFGHIJKLMNOPQRSTUVW").
inline constexpr std::array<std::uint8_t, 23> kHashAbRandomBytes = {
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
    'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W'};

// Install (or clear, with an empty function) the hashAB signer.
void set_hashab_signer(HashAbSigner signer);
[[nodiscard]] bool has_hashab_signer();

// Sign `sha1` for the device.  Throws Error(ErrorCode::Unsupported) when no
// signer is installed.
[[nodiscard]] HashAbSignature hashAB_compute_for_sha1(const Device& device,
                                                      std::span<const std::uint8_t, 20> sha1);

// Store hashing_scheme = 4, compute the SHA-1 of the zeroed image and write the
// 57 byte signature to offset 0xaa.  Requires a buffer of at least 0xf4 bytes
// starting with "mhbd" and a registered signer.
void hashAB_write(const Device& device, std::span<std::uint8_t> db);

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

// Sign `db` with whatever scheme Device::checksum_type() reports: hash58,
// hash72 and hashAB delegate to the functions above, None does nothing.
// Throws Error(ErrorCode::Unsupported) for ChecksumType::Unknown.
void write_checksum(const Device& device, std::span<std::uint8_t> db);

}  // namespace gpod

#endif  // GPOD_HASH_HPP
