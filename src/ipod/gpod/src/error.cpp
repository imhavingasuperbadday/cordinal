#include "gpod/error.hpp"

namespace gpod {

const char* to_string(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::Seek:
            return "seek";
        case ErrorCode::Corrupt:
            return "corrupt";
        case ErrorCode::NotFound:
            return "not found";
        case ErrorCode::Rename:
            return "rename";
        case ErrorCode::ItdbCorrupt:
            return "iTunesDB corrupt";
        case ErrorCode::Sqlite:
            return "sqlite";
        case ErrorCode::Io:
            return "I/O";
        case ErrorCode::Unsupported:
            return "unsupported";
        case ErrorCode::InvalidArgument:
            return "invalid argument";
    }
    return "unknown";
}

}  // namespace gpod
