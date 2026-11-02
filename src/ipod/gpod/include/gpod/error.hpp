// Error handling for the pure C++ libgpod rewrite.
//
// The original library reported failures through GError.  This rewrite uses
// exceptions carrying a stable ErrorCode so callers can distinguish corrupt
// databases from I/O failures without parsing message text.

#ifndef GPOD_ERROR_HPP
#define GPOD_ERROR_HPP

#include <stdexcept>
#include <string>
#include <string_view>

namespace gpod {

enum class ErrorCode {
    Seek,
    Corrupt,
    NotFound,
    Rename,
    ItdbCorrupt,
    Sqlite,
    Io,
    Unsupported,
    InvalidArgument,
};

class Error : public std::runtime_error {
public:
    Error(ErrorCode code, std::string message)
        : std::runtime_error(std::move(message)), code_(code) {}

    [[nodiscard]] ErrorCode code() const noexcept { return code_; }

private:
    ErrorCode code_;
};

[[noreturn]] inline void fail(ErrorCode code, std::string message) {
    throw Error(code, std::move(message));
}

[[nodiscard]] const char* to_string(ErrorCode code) noexcept;

}  // namespace gpod

#endif  // GPOD_ERROR_HPP
