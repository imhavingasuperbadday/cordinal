#include "gpod/io.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <fstream>
#include <limits>
#include <system_error>

#include "gpod/error.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace gpod {
namespace {

[[nodiscard]] std::uint16_t load_u16(const std::uint8_t* p, ByteOrder order) noexcept {
    if (order == ByteOrder::Little) {
        return static_cast<std::uint16_t>(p[0]) |
               static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[1]) << 8);
    }
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[0]) << 8) |
           static_cast<std::uint16_t>(p[1]);
}

[[nodiscard]] std::uint32_t load_u24(const std::uint8_t* p, ByteOrder order) noexcept {
    if (order == ByteOrder::Little) {
        return static_cast<std::uint32_t>(p[0]) |
               (static_cast<std::uint32_t>(p[1]) << 8) |
               (static_cast<std::uint32_t>(p[2]) << 16);
    }
    return (static_cast<std::uint32_t>(p[0]) << 16) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           static_cast<std::uint32_t>(p[2]);
}

[[nodiscard]] std::uint32_t load_u32(const std::uint8_t* p, ByteOrder order) noexcept {
    if (order == ByteOrder::Little) {
        return static_cast<std::uint32_t>(p[0]) |
               (static_cast<std::uint32_t>(p[1]) << 8) |
               (static_cast<std::uint32_t>(p[2]) << 16) |
               (static_cast<std::uint32_t>(p[3]) << 24);
    }
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
           static_cast<std::uint32_t>(p[3]);
}

[[nodiscard]] std::uint64_t load_u64(const std::uint8_t* p, ByteOrder order) noexcept {
    std::uint64_t value = 0;
    if (order == ByteOrder::Little) {
        for (int i = 7; i >= 0; --i) {
            value = (value << 8) | p[static_cast<std::size_t>(i)];
        }
    } else {
        for (int i = 0; i < 8; ++i) {
            value = (value << 8) | p[static_cast<std::size_t>(i)];
        }
    }
    return value;
}

void store_u16(std::uint8_t* p, std::uint16_t value, ByteOrder order) noexcept {
    if (order == ByteOrder::Little) {
        p[0] = static_cast<std::uint8_t>(value & 0xffU);
        p[1] = static_cast<std::uint8_t>((value >> 8) & 0xffU);
    } else {
        p[0] = static_cast<std::uint8_t>((value >> 8) & 0xffU);
        p[1] = static_cast<std::uint8_t>(value & 0xffU);
    }
}

void store_u24(std::uint8_t* p, std::uint32_t value, ByteOrder order) noexcept {
    if (order == ByteOrder::Little) {
        p[0] = static_cast<std::uint8_t>(value & 0xffU);
        p[1] = static_cast<std::uint8_t>((value >> 8) & 0xffU);
        p[2] = static_cast<std::uint8_t>((value >> 16) & 0xffU);
    } else {
        p[0] = static_cast<std::uint8_t>((value >> 16) & 0xffU);
        p[1] = static_cast<std::uint8_t>((value >> 8) & 0xffU);
        p[2] = static_cast<std::uint8_t>(value & 0xffU);
    }
}

void store_u32(std::uint8_t* p, std::uint32_t value, ByteOrder order) noexcept {
    if (order == ByteOrder::Little) {
        p[0] = static_cast<std::uint8_t>(value & 0xffU);
        p[1] = static_cast<std::uint8_t>((value >> 8) & 0xffU);
        p[2] = static_cast<std::uint8_t>((value >> 16) & 0xffU);
        p[3] = static_cast<std::uint8_t>((value >> 24) & 0xffU);
    } else {
        p[0] = static_cast<std::uint8_t>((value >> 24) & 0xffU);
        p[1] = static_cast<std::uint8_t>((value >> 16) & 0xffU);
        p[2] = static_cast<std::uint8_t>((value >> 8) & 0xffU);
        p[3] = static_cast<std::uint8_t>(value & 0xffU);
    }
}

void store_u64(std::uint8_t* p, std::uint64_t value, ByteOrder order) noexcept {
    if (order == ByteOrder::Little) {
        for (std::size_t i = 0; i < 8; ++i) {
            p[i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xffU);
        }
    } else {
        for (std::size_t i = 0; i < 8; ++i) {
            p[i] = static_cast<std::uint8_t>((value >> (8 * (7 - i))) & 0xffU);
        }
    }
}

void append_utf8(std::string& out, char32_t code_point) {
    if (code_point <= 0x7fU) {
        out.push_back(static_cast<char>(code_point));
    } else if (code_point <= 0x7ffU) {
        out.push_back(static_cast<char>(0xc0U | (code_point >> 6)));
        out.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
    } else if (code_point <= 0xffffU) {
        out.push_back(static_cast<char>(0xe0U | (code_point >> 12)));
        out.push_back(static_cast<char>(0x80U | ((code_point >> 6) & 0x3fU)));
        out.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
    } else {
        out.push_back(static_cast<char>(0xf0U | (code_point >> 18)));
        out.push_back(static_cast<char>(0x80U | ((code_point >> 12) & 0x3fU)));
        out.push_back(static_cast<char>(0x80U | ((code_point >> 6) & 0x3fU)));
        out.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
    }
}

}  // namespace

void ByteReader::require(std::size_t pos, std::size_t len) const {
    if (!has(pos, len)) {
        fail(ErrorCode::Seek,
             "read past end of buffer at offset " + std::to_string(pos) +
                 " (need " + std::to_string(len) + ", have " +
                 std::to_string(data_.size()) + ")");
    }
}

std::uint8_t ByteReader::u8(std::size_t pos) const {
    require(pos, 1);
    return data_[pos];
}

std::uint16_t ByteReader::u16(std::size_t pos) const {
    require(pos, 2);
    return load_u16(data_.data() + pos, order_);
}

std::uint16_t ByteReader::u16le(std::size_t pos) const {
    require(pos, 2);
    return load_u16(data_.data() + pos, ByteOrder::Little);
}

std::uint32_t ByteReader::u24(std::size_t pos) const {
    require(pos, 3);
    return load_u24(data_.data() + pos, order_);
}

std::uint32_t ByteReader::u24le(std::size_t pos) const {
    require(pos, 3);
    return load_u24(data_.data() + pos, ByteOrder::Little);
}

std::uint32_t ByteReader::u32(std::size_t pos) const {
    require(pos, 4);
    return load_u32(data_.data() + pos, order_);
}

std::uint32_t ByteReader::u32le(std::size_t pos) const {
    require(pos, 4);
    return load_u32(data_.data() + pos, ByteOrder::Little);
}

std::uint64_t ByteReader::u64(std::size_t pos) const {
    require(pos, 8);
    return load_u64(data_.data() + pos, order_);
}

std::uint64_t ByteReader::u64le(std::size_t pos) const {
    require(pos, 8);
    return load_u64(data_.data() + pos, ByteOrder::Little);
}

float ByteReader::f32(std::size_t pos) const {
    return std::bit_cast<float>(u32(pos));
}

float ByteReader::f32le(std::size_t pos) const {
    return std::bit_cast<float>(u32le(pos));
}

std::uint16_t ByteReader::u16be(std::size_t pos) const {
    require(pos, 2);
    return load_u16(data_.data() + pos, ByteOrder::Big);
}

std::uint32_t ByteReader::u24be(std::size_t pos) const {
    require(pos, 3);
    return load_u24(data_.data() + pos, ByteOrder::Big);
}

std::uint32_t ByteReader::u32be(std::size_t pos) const {
    require(pos, 4);
    return load_u32(data_.data() + pos, ByteOrder::Big);
}

std::uint64_t ByteReader::u64be(std::size_t pos) const {
    require(pos, 8);
    return load_u64(data_.data() + pos, ByteOrder::Big);
}

float ByteReader::f32be(std::size_t pos) const {
    return std::bit_cast<float>(u32be(pos));
}

std::span<const std::uint8_t> ByteReader::bytes(std::size_t pos, std::size_t len) const {
    require(pos, len);
    return data_.subspan(pos, len);
}

bool ByteReader::matches(std::size_t pos, std::string_view fourcc) const {
    if (fourcc.size() != 4 || !has(pos, 4)) {
        return false;
    }
    return std::memcmp(data_.data() + pos, fourcc.data(), 4) == 0;
}

std::u16string ByteReader::utf16(std::size_t pos, std::size_t units) const {
    return order_ == ByteOrder::Little ? utf16le(pos, units) : utf16be(pos, units);
}

std::u16string ByteReader::utf16le(std::size_t pos, std::size_t units) const {
    require(pos, units * 2);
    std::u16string out;
    out.resize(units);
    for (std::size_t i = 0; i < units; ++i) {
        out[i] = static_cast<char16_t>(load_u16(data_.data() + pos + i * 2, ByteOrder::Little));
    }
    return out;
}

std::u16string ByteReader::utf16be(std::size_t pos, std::size_t units) const {
    require(pos, units * 2);
    std::u16string out;
    out.resize(units);
    for (std::size_t i = 0; i < units; ++i) {
        out[i] = static_cast<char16_t>(load_u16(data_.data() + pos + i * 2, ByteOrder::Big));
    }
    return out;
}

void ByteWriter::align_to(std::size_t alignment) {
    if (alignment == 0) {
        return;
    }
    const std::size_t remainder = data_.size() % alignment;
    if (remainder != 0) {
        zeros(alignment - remainder);
    }
}

void ByteWriter::u8(std::uint8_t value) { data_.push_back(value); }

void ByteWriter::u16(std::uint16_t value) {
    std::uint8_t buffer[2];
    store_u16(buffer, value, order_);
    data_.insert(data_.end(), buffer, buffer + 2);
}

void ByteWriter::u24(std::uint32_t value) {
    std::uint8_t buffer[3];
    store_u24(buffer, value, order_);
    data_.insert(data_.end(), buffer, buffer + 3);
}

void ByteWriter::u32(std::uint32_t value) {
    std::uint8_t buffer[4];
    store_u32(buffer, value, order_);
    data_.insert(data_.end(), buffer, buffer + 4);
}

void ByteWriter::u64(std::uint64_t value) {
    std::uint8_t buffer[8];
    store_u64(buffer, value, order_);
    data_.insert(data_.end(), buffer, buffer + 8);
}

void ByteWriter::f32(float value) { u32(std::bit_cast<std::uint32_t>(value)); }

void ByteWriter::u16le(std::uint16_t value) {
    std::uint8_t buffer[2];
    store_u16(buffer, value, ByteOrder::Little);
    data_.insert(data_.end(), buffer, buffer + 2);
}

void ByteWriter::u32le(std::uint32_t value) {
    std::uint8_t buffer[4];
    store_u32(buffer, value, ByteOrder::Little);
    data_.insert(data_.end(), buffer, buffer + 4);
}

void ByteWriter::u64le(std::uint64_t value) {
    std::uint8_t buffer[8];
    store_u64(buffer, value, ByteOrder::Little);
    data_.insert(data_.end(), buffer, buffer + 8);
}

void ByteWriter::f32le(float value) { u32le(std::bit_cast<std::uint32_t>(value)); }

void ByteWriter::u16be(std::uint16_t value) {
    std::uint8_t buffer[2];
    store_u16(buffer, value, ByteOrder::Big);
    data_.insert(data_.end(), buffer, buffer + 2);
}

void ByteWriter::u32be(std::uint32_t value) {
    std::uint8_t buffer[4];
    store_u32(buffer, value, ByteOrder::Big);
    data_.insert(data_.end(), buffer, buffer + 4);
}

void ByteWriter::u64be(std::uint64_t value) {
    std::uint8_t buffer[8];
    store_u64(buffer, value, ByteOrder::Big);
    data_.insert(data_.end(), buffer, buffer + 8);
}

void ByteWriter::f32be(float value) { u32be(std::bit_cast<std::uint32_t>(value)); }

void ByteWriter::bytes(std::span<const std::uint8_t> value) {
    data_.insert(data_.end(), value.begin(), value.end());
}

void ByteWriter::bytes(std::string_view value) {
    data_.insert(data_.end(), value.begin(), value.end());
}

void ByteWriter::zeros(std::size_t count) { data_.insert(data_.end(), count, 0); }

void ByteWriter::utf16(std::u16string_view value) {
    if (order_ == ByteOrder::Little) {
        utf16le(value);
    } else {
        utf16be(value);
    }
}

void ByteWriter::utf16le(std::u16string_view value) {
    for (char16_t unit : value) {
        u16le(static_cast<std::uint16_t>(unit));
    }
}

void ByteWriter::utf16be(std::u16string_view value) {
    for (char16_t unit : value) {
        u16be(static_cast<std::uint16_t>(unit));
    }
}

void ByteWriter::require_patch(std::size_t pos, std::size_t len) const {
    if (pos > data_.size() || len > data_.size() - pos) {
        fail(ErrorCode::Seek, "patch past end of output buffer");
    }
}

void ByteWriter::patch_u16(std::size_t pos, std::uint16_t value) {
    require_patch(pos, 2);
    store_u16(data_.data() + pos, value, order_);
}

void ByteWriter::patch_u32(std::size_t pos, std::uint32_t value) {
    require_patch(pos, 4);
    store_u32(data_.data() + pos, value, order_);
}

void ByteWriter::patch_u64(std::size_t pos, std::uint64_t value) {
    require_patch(pos, 8);
    store_u64(data_.data() + pos, value, order_);
}

void ByteWriter::patch_u16le(std::size_t pos, std::uint16_t value) {
    require_patch(pos, 2);
    store_u16(data_.data() + pos, value, ByteOrder::Little);
}

void ByteWriter::patch_u32le(std::size_t pos, std::uint32_t value) {
    require_patch(pos, 4);
    store_u32(data_.data() + pos, value, ByteOrder::Little);
}

void ByteWriter::patch_u32be(std::size_t pos, std::uint32_t value) {
    require_patch(pos, 4);
    store_u32(data_.data() + pos, value, ByteOrder::Big);
}

std::string utf16_to_utf8(std::u16string_view value) {
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        char32_t code_point = value[i];
        if (code_point >= 0xd800U && code_point <= 0xdbffU) {
            if (i + 1 < value.size() && value[i + 1] >= 0xdc00U && value[i + 1] <= 0xdfffU) {
                code_point = 0x10000U + ((code_point - 0xd800U) << 10) +
                             (value[i + 1] - 0xdc00U);
                ++i;
            } else {
                code_point = 0xfffdU;
            }
        } else if (code_point >= 0xdc00U && code_point <= 0xdfffU) {
            code_point = 0xfffdU;
        }
        append_utf8(out, code_point);
    }
    return out;
}

std::u16string utf8_to_utf16(std::string_view value) {
    std::u16string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size();) {
        const auto lead = static_cast<std::uint8_t>(value[i]);
        std::uint32_t code_point = 0;
        std::size_t extra = 0;
        if (lead < 0x80U) {
            code_point = lead;
        } else if ((lead & 0xe0U) == 0xc0U) {
            code_point = lead & 0x1fU;
            extra = 1;
        } else if ((lead & 0xf0U) == 0xe0U) {
            code_point = lead & 0x0fU;
            extra = 2;
        } else if ((lead & 0xf8U) == 0xf0U) {
            code_point = lead & 0x07U;
            extra = 3;
        } else {
            out.push_back(static_cast<char16_t>(0xfffdU));
            ++i;
            continue;
        }
        if (i + extra >= value.size()) {
            out.push_back(static_cast<char16_t>(0xfffdU));
            break;
        }
        bool valid = true;
        for (std::size_t j = 1; j <= extra; ++j) {
            const auto continuation = static_cast<std::uint8_t>(value[i + j]);
            if ((continuation & 0xc0U) != 0x80U) {
                valid = false;
                break;
            }
            code_point = (code_point << 6) | (continuation & 0x3fU);
        }
        if (!valid || code_point > 0x10ffffU ||
            (code_point >= 0xd800U && code_point <= 0xdfffU)) {
            out.push_back(static_cast<char16_t>(0xfffdU));
            ++i;
            continue;
        }
        if (code_point <= 0xffffU) {
            out.push_back(static_cast<char16_t>(code_point));
        } else {
            code_point -= 0x10000U;
            out.push_back(static_cast<char16_t>(0xd800U + (code_point >> 10)));
            out.push_back(static_cast<char16_t>(0xdc00U + (code_point & 0x3ffU)));
        }
        i += extra + 1;
    }
    return out;
}

Bytes read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        fail(ErrorCode::NotFound, "cannot open '" + path.string() + "'");
    }
    const std::streamoff end = input.tellg();
    if (end < 0) {
        fail(ErrorCode::Io, "cannot determine size of '" + path.string() + "'");
    }
    Bytes data(static_cast<std::size_t>(end));
    input.seekg(0);
    if (!data.empty()) {
        input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!input) {
            fail(ErrorCode::Io, "cannot read '" + path.string() + "'");
        }
    }
    return data;
}

void write_file(const std::filesystem::path& path, std::span<const std::uint8_t> data) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        fail(ErrorCode::Io, "cannot open '" + path.string() + "' for writing");
    }
    if (!data.empty()) {
        output.write(reinterpret_cast<const char*>(data.data()),
                     static_cast<std::streamsize>(data.size()));
    }
    output.flush();
    if (!output) {
        fail(ErrorCode::Io, "cannot write '" + path.string() + "'");
    }
}

void write_file_atomic(const std::filesystem::path& path, std::span<const std::uint8_t> data) {
    std::filesystem::path temporary = path;
    temporary += ".gpod-tmp";
    write_file(temporary, data);
#ifdef _WIN32
    if (MoveFileExW(temporary.c_str(), path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        fail(ErrorCode::Rename, "cannot replace '" + path.string() + "'");
    }
#else
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        fail(ErrorCode::Rename, "cannot replace '" + path.string() + "'");
    }
#endif
}

}  // namespace gpod
