// Byte-level I/O primitives used by the iTunesDB, ArtworkDB and PhotoDB
// parsers.  The original code used raw pointers and a GError; this version is
// bounds-checked and reports failures by exception.

#ifndef GPOD_IO_HPP
#define GPOD_IO_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "gpod/types.hpp"

namespace gpod {

using Bytes = std::vector<std::uint8_t>;

[[nodiscard]] Bytes read_file(const std::filesystem::path& path);
void write_file(const std::filesystem::path& path, std::span<const std::uint8_t> data);
void write_file_atomic(const std::filesystem::path& path,
                       std::span<const std::uint8_t> data);

class ByteReader {
public:
    ByteReader(std::span<const std::uint8_t> data, ByteOrder order)
        : data_(data), order_(order) {}

    [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }
    [[nodiscard]] ByteOrder order() const noexcept { return order_; }
    [[nodiscard]] bool has(std::size_t pos, std::size_t len) const noexcept {
        return pos <= data_.size() && len <= data_.size() - pos;
    }
    void require(std::size_t pos, std::size_t len) const;

    [[nodiscard]] std::uint8_t u8(std::size_t pos) const;
    [[nodiscard]] std::uint16_t u16(std::size_t pos) const;
    [[nodiscard]] std::uint32_t u24(std::size_t pos) const;
    [[nodiscard]] std::uint32_t u32(std::size_t pos) const;
    [[nodiscard]] std::uint64_t u64(std::size_t pos) const;
    [[nodiscard]] float f32(std::size_t pos) const;

    [[nodiscard]] std::uint16_t u16le(std::size_t pos) const;
    [[nodiscard]] std::uint32_t u24le(std::size_t pos) const;
    [[nodiscard]] std::uint32_t u32le(std::size_t pos) const;
    [[nodiscard]] std::uint64_t u64le(std::size_t pos) const;
    [[nodiscard]] float f32le(std::size_t pos) const;

    [[nodiscard]] std::uint16_t u16be(std::size_t pos) const;
    [[nodiscard]] std::uint32_t u24be(std::size_t pos) const;
    [[nodiscard]] std::uint32_t u32be(std::size_t pos) const;
    [[nodiscard]] std::uint64_t u64be(std::size_t pos) const;
    [[nodiscard]] float f32be(std::size_t pos) const;

    [[nodiscard]] std::span<const std::uint8_t> bytes(std::size_t pos,
                                                      std::size_t len) const;
    [[nodiscard]] bool matches(std::size_t pos, std::string_view fourcc) const;

    [[nodiscard]] std::u16string utf16(std::size_t pos, std::size_t units) const;
    [[nodiscard]] std::u16string utf16le(std::size_t pos, std::size_t units) const;
    [[nodiscard]] std::u16string utf16be(std::size_t pos, std::size_t units) const;

private:
    std::span<const std::uint8_t> data_;
    ByteOrder order_;
};

class ByteWriter {
public:
    explicit ByteWriter(ByteOrder order = ByteOrder::Little) : order_(order) {}

    [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }
    [[nodiscard]] ByteOrder order() const noexcept { return order_; }
    [[nodiscard]] const Bytes& data() const noexcept { return data_; }
    [[nodiscard]] Bytes& data_mutable() noexcept { return data_; }
    [[nodiscard]] Bytes take() noexcept { return std::move(data_); }

    void reserve(std::size_t size) { data_.reserve(size); }
    void align_to(std::size_t alignment);

    void u8(std::uint8_t value);
    void u16(std::uint16_t value);
    void u24(std::uint32_t value);
    void u32(std::uint32_t value);
    void u64(std::uint64_t value);
    void f32(float value);

    void u16le(std::uint16_t value);
    void u32le(std::uint32_t value);
    void u64le(std::uint64_t value);
    void f32le(float value);
    void u16be(std::uint16_t value);
    void u32be(std::uint32_t value);
    void u64be(std::uint64_t value);
    void f32be(float value);

    void bytes(std::span<const std::uint8_t> value);
    void bytes(std::string_view value);
    void zeros(std::size_t count);
    void utf16(std::u16string_view value);
    void utf16le(std::u16string_view value);
    void utf16be(std::u16string_view value);

    void patch_u16(std::size_t pos, std::uint16_t value);
    void patch_u32(std::size_t pos, std::uint32_t value);
    void patch_u64(std::size_t pos, std::uint64_t value);
    void patch_u16le(std::size_t pos, std::uint16_t value);
    void patch_u32le(std::size_t pos, std::uint32_t value);
    void patch_u32be(std::size_t pos, std::uint32_t value);

private:
    void require_patch(std::size_t pos, std::size_t len) const;

    Bytes data_;
    ByteOrder order_;
};

[[nodiscard]] std::string utf16_to_utf8(std::u16string_view value);
[[nodiscard]] std::u16string utf8_to_utf16(std::string_view value);

}  // namespace gpod

#endif  // GPOD_IO_HPP
