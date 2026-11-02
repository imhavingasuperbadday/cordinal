#include "gpod/time.hpp"

#include "gpod/model.hpp"
#include "gpod/paths.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// GetTimeZoneInformationForYear() is declared for Windows 7 and later; the
// Win32 headers default to an older target.  EnumDynamicTimeZoneInformation()
// would need Windows 8, so it is resolved at run time instead.
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0601
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#else
#include <time.h>
#endif

namespace gpod {
namespace {

// City ids stored by 6th-generation Preferences files.  The table mirrors
// upstream src/itdb_tzinfo_data.h: city index 0x01..0xcd mapped to an IANA zone.
struct CityZone {
    const char* city_name;
    std::uint8_t city_index;
    const char* tz_name;
};

constexpr CityZone kCityZones[] = {
#include "tzinfo_data.inc"
};

std::string_view zone_for_city(std::int16_t city_id) {
    if (city_id < 0) {
        return {};
    }
    const auto index = static_cast<std::uint8_t>(city_id);
    for (const CityZone& city : kCityZones) {
        if (city.city_index == index) {
            return city.tz_name;
        }
    }
    return {};
}

#if defined(_WIN32)

// Windows zone equivalents for the IANA names above, plus a fixed offset used
// when Windows has no such zone or the lookup fails.
struct WindowsZone {
    const char* tz_name;
    const char* windows_id;
    std::int32_t fallback_offset;
};

constexpr WindowsZone kWindowsZones[] = {
#include "tzinfo_windows.inc"
};

using DynamicZoneMap =
    std::map<std::wstring, DYNAMIC_TIME_ZONE_INFORMATION, std::less<>>;

DynamicZoneMap g_dynamic_zones;
std::once_flag g_dynamic_zones_once;

using EnumDynamicZonesFunction = DWORD(WINAPI*)(DWORD, PDYNAMIC_TIME_ZONE_INFORMATION);

// EnumDynamicTimeZoneInformation() only exists on Windows 8 and later; on older
// systems the fixed offsets below are used instead.  It is exported from
// kernelbase.dll rather than kernel32.dll, so both are probed, and the exported
// address is copied into the typed pointer to avoid an incompatible cast.
EnumDynamicZonesFunction enum_dynamic_zones() {
    static const auto function = [] {
        EnumDynamicZonesFunction resolved = nullptr;
        static_assert(sizeof(resolved) == sizeof(FARPROC), "function pointer sizes differ");
        for (const wchar_t* module_name : {L"kernelbase.dll", L"kernel32.dll"}) {
            const HMODULE module = GetModuleHandleW(module_name);
            if (module == nullptr) {
                continue;
            }
            const FARPROC proc = GetProcAddress(module, "EnumDynamicTimeZoneInformation");
            if (proc == nullptr) {
                continue;
            }
            std::memcpy(&resolved, &proc, sizeof(resolved));
            break;
        }
        return resolved;
    }();
    return function;
}

void load_dynamic_zones() {
    const EnumDynamicZonesFunction enumerate = enum_dynamic_zones();
    if (enumerate == nullptr) {
        return;
    }

    DWORD index = 0;
    DYNAMIC_TIME_ZONE_INFORMATION info{};
    while (enumerate(index, &info) == ERROR_SUCCESS) {
        g_dynamic_zones.emplace(info.TimeZoneKeyName, info);
        ++index;
        info = DYNAMIC_TIME_ZONE_INFORMATION{};
    }
}

const DynamicZoneMap& dynamic_zones() {
    std::call_once(g_dynamic_zones_once, load_dynamic_zones);
    return g_dynamic_zones;
}

std::int64_t filetime_ticks(const FILETIME& time) {
    return (static_cast<std::int64_t>(time.dwHighDateTime) << 32) |
           static_cast<std::int64_t>(time.dwLowDateTime);
}

// Current UTC offset of a Windows zone, DST included, as computed for the year
// of @now.  Windows applies its own transition rules, so zones that observe DST
// follow the rules installed on this machine.
std::optional<std::int32_t> windows_dynamic_offset(std::string_view windows_id,
                                                   const SYSTEMTIME& now) {
    const auto& zones = dynamic_zones();
    const std::wstring key(windows_id.begin(), windows_id.end());
    const auto found = zones.find(key);
    if (found == zones.end()) {
        return std::nullopt;
    }

    DYNAMIC_TIME_ZONE_INFORMATION dynamic = found->second;
    TIME_ZONE_INFORMATION info{};
    if (GetTimeZoneInformationForYear(static_cast<USHORT>(now.wYear), &dynamic, &info) == 0) {
        return std::nullopt;
    }
    SYSTEMTIME local{};
    if (SystemTimeToTzSpecificLocalTime(&info, &now, &local) == 0) {
        return std::nullopt;
    }

    FILETIME utc_filetime{};
    FILETIME local_filetime{};
    if (SystemTimeToFileTime(&now, &utc_filetime) == 0 ||
        SystemTimeToFileTime(&local, &local_filetime) == 0) {
        return std::nullopt;
    }
    const std::int64_t ticks = filetime_ticks(local_filetime) - filetime_ticks(utc_filetime);
    return static_cast<std::int32_t>(ticks / 10'000'000);
}

std::optional<std::int32_t> windows_zone_offset(std::string_view zone) {
    const WindowsZone* entry = nullptr;
    for (const WindowsZone& candidate : kWindowsZones) {
        if (zone == candidate.tz_name) {
            entry = &candidate;
            break;
        }
    }
    if (entry == nullptr) {
        return std::nullopt;
    }

    if (entry->windows_id[0] != '\0') {
        SYSTEMTIME now{};
        GetSystemTime(&now);
        if (const auto offset = windows_dynamic_offset(entry->windows_id, now)) {
            return offset;
        }
    }
    return entry->fallback_offset;
}

#else

constexpr std::size_t kTzifHeaderSize = 44;
// Guards against absurd counts in a corrupt file; real zoneinfo files are far
// below this.
constexpr std::uint32_t kMaxTzifCount = 100'000;
constexpr std::streamoff kMaxTzifFileSize = 1 << 20;

struct TzifHeader {
    std::uint32_t isutcnt = 0;
    std::uint32_t isstdcnt = 0;
    std::uint32_t leapcnt = 0;
    std::uint32_t timecnt = 0;
    std::uint32_t typecnt = 0;
    std::uint32_t charcnt = 0;
};

std::uint32_t be_u32(const std::vector<unsigned char>& data, std::size_t pos) {
    return (static_cast<std::uint32_t>(data[pos]) << 24) |
           (static_cast<std::uint32_t>(data[pos + 1]) << 16) |
           (static_cast<std::uint32_t>(data[pos + 2]) << 8) |
           static_cast<std::uint32_t>(data[pos + 3]);
}

std::int64_t be_time(const std::vector<unsigned char>& data, std::size_t pos,
                     std::size_t width) {
    if (width == 8) {
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < 8; ++i) {
            value = (value << 8) | data[pos + i];
        }
        return static_cast<std::int64_t>(value);
    }
    // The 32-bit block stores signed seconds.
    return static_cast<std::int64_t>(static_cast<std::int32_t>(be_u32(data, pos)));
}

std::optional<TzifHeader> read_header(const std::vector<unsigned char>& data,
                                      std::size_t pos) {
    if (pos > data.size() || data.size() - pos < kTzifHeaderSize) {
        return std::nullopt;
    }
    if (std::memcmp(data.data() + pos, "TZif", 4) != 0) {
        return std::nullopt;
    }

    TzifHeader header;
    header.isutcnt = be_u32(data, pos + 20);
    header.isstdcnt = be_u32(data, pos + 24);
    header.leapcnt = be_u32(data, pos + 28);
    header.timecnt = be_u32(data, pos + 32);
    header.typecnt = be_u32(data, pos + 36);
    header.charcnt = be_u32(data, pos + 40);
    if (header.isutcnt > kMaxTzifCount || header.isstdcnt > kMaxTzifCount ||
        header.leapcnt > kMaxTzifCount || header.timecnt > kMaxTzifCount ||
        header.typecnt > kMaxTzifCount || header.charcnt > kMaxTzifCount) {
        return std::nullopt;
    }
    return header;
}

// End offset of one TZif data block, or nothing for an impossible size.
std::optional<std::size_t> block_end(const TzifHeader& header, std::size_t time_width,
                                     std::size_t start) {
    const std::uint64_t size = static_cast<std::uint64_t>(kTzifHeaderSize) +
                               static_cast<std::uint64_t>(header.timecnt) * (time_width + 1) +
                               static_cast<std::uint64_t>(header.typecnt) * 6 +
                               header.charcnt +
                               static_cast<std::uint64_t>(header.leapcnt) * (time_width + 4) +
                               header.isstdcnt + header.isutcnt;
    if (size > std::numeric_limits<std::size_t>::max() - start) {
        return std::nullopt;
    }
    return start + static_cast<std::size_t>(size);
}

struct ZoneData {
    std::vector<std::int64_t> transitions;
    std::vector<std::uint8_t> types;
    std::vector<std::int32_t> offsets;
};

// Parses a TZif file.  Version 2 and later files carry a second, 64-bit block
// after the legacy one; the newer block is used because slim zoneinfo files may
// leave the legacy block without transitions.
std::optional<ZoneData> parse_zone(const std::vector<unsigned char>& data) {
    auto header = read_header(data, 0);
    if (!header) {
        return std::nullopt;
    }

    std::size_t pos = 0;
    std::size_t width = 4;
    const unsigned char version = data.size() > 4 ? data[4] : 0;
    if (version == '2' || version == '3' || version == '4') {
        const auto first_end = block_end(*header, 4, 0);
        if (!first_end || *first_end >= data.size()) {
            return std::nullopt;
        }
        const auto second_header = read_header(data, *first_end);
        if (!second_header) {
            return std::nullopt;
        }
        header = *second_header;
        pos = *first_end;
        width = 8;
    }

    const auto end = block_end(*header, width, pos);
    if (!end || *end > data.size() || header->typecnt == 0) {
        return std::nullopt;
    }

    ZoneData zone;
    std::size_t cursor = pos + kTzifHeaderSize;
    zone.transitions.reserve(header->timecnt);
    for (std::uint32_t i = 0; i < header->timecnt; ++i) {
        zone.transitions.push_back(be_time(data, cursor, width));
        cursor += width;
    }
    zone.types.reserve(header->timecnt);
    for (std::uint32_t i = 0; i < header->timecnt; ++i) {
        zone.types.push_back(data[cursor + i]);
    }
    cursor += header->timecnt;
    zone.offsets.reserve(header->typecnt);
    for (std::uint32_t i = 0; i < header->typecnt; ++i) {
        zone.offsets.push_back(static_cast<std::int32_t>(be_u32(data, cursor)));
        cursor += 6;
    }

    for (const std::uint8_t type : zone.types) {
        if (type >= zone.offsets.size()) {
            return std::nullopt;
        }
    }
    return zone;
}

std::optional<std::vector<unsigned char>> read_zone_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    input.seekg(0, std::ios::end);
    const std::streamoff end = input.tellg();
    if (end < 0 || end > kMaxTzifFileSize) {
        return std::nullopt;
    }
    input.seekg(0, std::ios::beg);

    std::vector<unsigned char> data(static_cast<std::size_t>(end));
    if (!data.empty()) {
        input.read(reinterpret_cast<char*>(data.data()),
                   static_cast<std::streamsize>(data.size()));
        if (input.gcount() != static_cast<std::streamsize>(data.size())) {
            return std::nullopt;
        }
    }
    return data;
}

std::optional<ZoneData> load_zone(const std::string& zone) {
    // Only plain relative names are accepted, like upstream.
    if (zone.empty() || zone.front() == '/' || zone.find("..") != std::string::npos) {
        return std::nullopt;
    }

    std::vector<std::string> directories;
    if (const char* tzdir = std::getenv("TZDIR"); tzdir != nullptr && tzdir[0] != '\0') {
        directories.emplace_back(tzdir);
    }
    directories.emplace_back("/usr/share/zoneinfo");
    directories.emplace_back("/usr/lib/zoneinfo");
    // /etc/localtime is deliberately not a fallback: it describes the host zone,
    // so using it would silently return the host offset for a known city.

    for (const std::string& directory : directories) {
        const auto data = read_zone_file(directory + "/" + zone);
        if (!data) {
            continue;
        }
        if (auto parsed = parse_zone(*data)) {
            return parsed;
        }
    }
    return std::nullopt;
}

// Offset in effect at @now: the last transition not after @now, or the first
// type when @now predates every transition.
std::int32_t offset_at(const ZoneData& zone, std::int64_t now) {
    std::size_t type = 0;
    for (std::size_t i = 0; i < zone.transitions.size(); ++i) {
        if (zone.transitions[i] > now) {
            break;
        }
        type = zone.types[i];
    }
    return zone.offsets[type];
}

std::mutex g_zone_cache_mutex;
std::map<std::string, std::optional<ZoneData>, std::less<>> g_zone_cache;

std::optional<std::int32_t> posix_zone_offset(std::string_view zone) {
    const auto now = static_cast<std::int64_t>(std::time(nullptr));
    const std::string name(zone);

    std::lock_guard<std::mutex> lock(g_zone_cache_mutex);
    const auto [entry, inserted] = g_zone_cache.try_emplace(name);
    if (inserted) {
        entry->second = load_zone(name);
    }
    if (!entry->second) {
        return std::nullopt;
    }
    return offset_at(*entry->second, now);
}

#endif  // _WIN32

std::optional<std::int32_t> zone_current_offset(std::string_view zone) {
    if (zone.empty()) {
        return std::nullopt;
    }
#if defined(_WIN32)
    return windows_zone_offset(zone);
#else
    return posix_zone_offset(zone);
#endif
}

std::optional<std::int32_t> city_utc_shift(std::int16_t city_id) {
    const std::string_view zone = zone_for_city(city_id);
    if (zone.empty()) {
        return std::nullopt;
    }
    return zone_current_offset(zone);
}

std::int32_t raw_shift_4g(std::int16_t raw) {
    if (raw < 0 || raw > ((2 * 12) << 1)) {
        return std::numeric_limits<std::int32_t>::min();
    }
    raw = static_cast<std::int16_t>(raw - 0x19);
    std::int32_t shift = (raw >> 1) * 3600;
    if ((raw & 1) != 0) {
        shift += 3600;
    }
    return shift;
}

std::int32_t raw_shift_5g(std::int16_t raw) {
    // Minutes from Tokyo, increasing eastward.
    return static_cast<std::int32_t>(raw) * 60 - 8 * 3600;
}

bool read_preferences_shift(const std::filesystem::path& preferences, std::int32_t& shift) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(preferences, ec);
    if (ec) {
        return false;
    }

    std::size_t offset = 0;
    enum class Layout { FourG, FiveG, SixG } layout;
    switch (size) {
    case 2892:
        offset = 0xb10;
        layout = Layout::FourG;
        break;
    case 2924:
        offset = 0xb22;
        layout = Layout::FiveG;
        break;
    case 2952:
    case 2956:
    case 2960:
        // nano 3g, iPod classic and nano 4g store a city id that upstream
        // resolves through the zoneinfo database instead of a raw offset.
        offset = 0xb70;
        layout = Layout::SixG;
        break;
    default:
        return false;
    }

    std::ifstream input(preferences, std::ios::binary);
    if (!input) {
        return false;
    }
    input.seekg(static_cast<std::streamoff>(offset));
    std::array<unsigned char, 2> bytes{};
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (input.gcount() != static_cast<std::streamsize>(bytes.size())) {
        return false;
    }

    const auto raw = static_cast<std::int16_t>(
        static_cast<std::uint16_t>(bytes[0]) | (static_cast<std::uint16_t>(bytes[1]) << 8));
    std::int32_t converted = 0;
    switch (layout) {
    case Layout::FourG:
        converted = raw_shift_4g(raw);
        break;
    case Layout::FiveG:
        converted = raw_shift_5g(raw);
        break;
    case Layout::SixG: {
        const auto city_shift = city_utc_shift(raw);
        if (!city_shift) {
            return false;
        }
        converted = *city_shift;
        break;
    }
    }
    if (converted == std::numeric_limits<std::int32_t>::min()) {
        return false;
    }
    // Upstream only accepts +-12h for the raw 4G/5G offsets.  City zones can sit
    // further east (+13h), e.g. Pacific/Auckland in summer, so the 6G check uses
    // the real-world limit of UTC+14.
    constexpr std::int32_t kEastLimit = 12 * 3600;
    constexpr std::int32_t kCityEastLimit = 14 * 3600;
    if (converted < -12 * 3600 ||
        converted > (layout == Layout::SixG ? kCityEastLimit : kEastLimit)) {
        return false;
    }
    shift = converted;
    return true;
}

}  // namespace

std::time_t mac_time_to_unix(std::uint64_t mac_time, std::int32_t timezone_shift) noexcept {
    if (mac_time == 0) {
        return 0;
    }
    const auto seconds = static_cast<std::int64_t>(mac_time) - kMacEpochOffset - timezone_shift;
    return static_cast<std::time_t>(seconds);
}

std::uint64_t unix_time_to_mac(std::time_t unix_time, std::int32_t timezone_shift) noexcept {
    if (unix_time == 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(unix_time) +
                                      kMacEpochOffset + timezone_shift);
}

std::int32_t local_timezone_shift() noexcept {
    const std::time_t now = std::time(nullptr);
    if (now == static_cast<std::time_t>(-1)) {
        return 0;
    }

    std::tm local{};
#if defined(_WIN32)
    if (localtime_s(&local, &now) != 0) {
        return 0;
    }
    const std::time_t local_as_utc = _mkgmtime64(&local);
#else
    if (localtime_r(&now, &local) == nullptr) {
        return 0;
    }
    const std::time_t local_as_utc = timegm(&local);
#endif
    if (local_as_utc == static_cast<std::time_t>(-1)) {
        return 0;
    }

    const auto shift = static_cast<std::int64_t>(std::difftime(local_as_utc, now));
    if (shift < std::numeric_limits<std::int32_t>::min() ||
        shift > std::numeric_limits<std::int32_t>::max()) {
        return 0;
    }
    return static_cast<std::int32_t>(shift);
}

void update_timezone_shift(Device& device) {
    device.timezone_shift = local_timezone_shift();
    if (device.mountpoint.empty()) {
        return;
    }

    std::filesystem::path preferences = path_in(device_dir(device.mountpoint), "Preferences");
    std::error_code ec;
    if (!std::filesystem::exists(preferences, ec)) {
        preferences = path_in(device_dir(device.mountpoint), "preferences");
    }

    std::int32_t shift = 0;
    if (read_preferences_shift(preferences, shift)) {
        device.timezone_shift = shift;
    }
}

}  // namespace gpod
