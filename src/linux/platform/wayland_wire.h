// The Wayland wire format's reading side: messages framed out of the socket's bytes and their
// arguments read with bounds checks. Kept apart from the connection so the fuzz harness
// (src/linux/platform/fuzz.cpp) can feed it arbitrary bytes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace lx::wl {

constexpr size_t kMaxMessage = 4096;  // libwayland's limit for one message

struct Message {
    uint32_t object = 0;
    uint16_t opcode = 0;
    std::string_view body;  // the arguments
};
// The next whole message at the start of `buffer`: its size, 0 when more bytes are needed,
// SIZE_MAX when the header is malformed (a size under 8, unaligned or over kMaxMessage).
size_t NextMessage(std::string_view buffer, Message& out);

// Reads a message's arguments in order; any read past the end (or a malformed string) sets
// failed() and returns zeros from then on.
class Args {
public:
    explicit Args(std::string_view body) : b_(body) {}
    uint32_t Uint() {
        if (bad_ || b_.size() - at_ < 4 || at_ > b_.size()) return bad_ = true, 0;
        uint32_t v;
        memcpy(&v, b_.data() + at_, 4);
        at_ += 4;
        return v;
    }
    int32_t Int() { return (int32_t)Uint(); }
    double Fixed() { return Int() / 256.0; }
    // A string (null for a zero length); the content without its terminating NUL.
    std::string_view String(bool* null = nullptr);
    std::string_view Array();
    bool failed() const { return bad_; }

private:
    std::string_view b_;
    size_t at_ = 0;
    bool bad_ = false;
};

}  // namespace lx::wl
