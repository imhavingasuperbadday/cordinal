// iPod clock conversion.  The database stores seconds since 1904-01-01 local
// time; these helpers convert to and from Unix time using the device's
// timezone adjustment.

#ifndef GPOD_TIME_HPP
#define GPOD_TIME_HPP

#include <cstdint>
#include <ctime>

namespace gpod {

class Device;

// 1904-01-01 to 1970-01-01 in seconds.
inline constexpr std::int64_t kMacEpochOffset = 2082844800;

[[nodiscard]] std::time_t mac_time_to_unix(std::uint64_t mac_time,
                                           std::int32_t timezone_shift = 0) noexcept;
[[nodiscard]] std::uint64_t unix_time_to_mac(std::time_t unix_time,
                                             std::int32_t timezone_shift = 0) noexcept;

// Offset east of UTC for the host's current local time, in seconds.
[[nodiscard]] std::int32_t local_timezone_shift() noexcept;

// Re-reads the device Preferences file when it has a known layout and updates
// device.timezone_shift.  Falls back to the host timezone on unknown models.
void update_timezone_shift(Device& device);

}  // namespace gpod

#endif  // GPOD_TIME_HPP
