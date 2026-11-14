#include "platform/wayland_wire.h"

namespace lx::wl {

size_t NextMessage(std::string_view b, Message& m) {
    if (b.size() < 8) return 0;
    uint32_t word;
    memcpy(&m.object, b.data(), 4);
    memcpy(&word, b.data() + 4, 4);
    const size_t size = word >> 16;
    if (size < 8 || size % 4 || size > kMaxMessage) return SIZE_MAX;
    if (b.size() < size) return 0;
    m.opcode = (uint16_t)(word & 0xffff);
    m.body = b.substr(8, size - 8);
    return size;
}

std::string_view Args::String(bool* null) {
    const uint32_t len = Uint();
    if (null) *null = !bad_ && len == 0;
    if (bad_ || len == 0) return {};
    const size_t padded = ((size_t)len + 3) & ~(size_t)3;
    if (b_.size() - at_ < padded || b_[at_ + len - 1] != '\0') return bad_ = true, std::string_view();
    std::string_view s = b_.substr(at_, len - 1);
    at_ += padded;
    return s;
}

std::string_view Args::Array() {
    const uint32_t len = Uint();
    if (bad_) return {};
    const size_t padded = ((size_t)len + 3) & ~(size_t)3;
    if (padded < len || b_.size() - at_ < padded) return bad_ = true, std::string_view();
    std::string_view s = b_.substr(at_, len);
    at_ += padded;
    return s;
}

}  // namespace lx::wl
