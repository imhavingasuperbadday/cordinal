// Device metadata files of an iPod, without GLib.
//
// Two files under iPod_Control/Device describe a device:
//   SysInfo          "key: value" lines written by iTunes, read by libgpod's
//                    itdb_device.c to find the model number and firewire GUID
//   SysInfoExtended  an XML plist with artwork formats, database versions and
//                    the serial number, parsed by itdb_sysinfo_extended_parser.c
//
// Everything here is best-effort: a device with missing or damaged metadata
// keeps working, it is just treated as an unknown model.

#ifndef GPOD_SYSINFO_HPP
#define GPOD_SYSINFO_HPP

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "gpod/plist.hpp"
#include "gpod/types.hpp"

namespace gpod {

// Key/value pairs of Device/SysInfo.  The transparent comparator lets callers
// look entries up with a std::string_view.
using SysInfoMap = std::map<std::string, std::string, std::less<>>;

// Parsed Device/SysInfoExtended.  Field names and defaults mirror the struct
// in itdb_sysinfo_extended_parser.c; missing entries stay zero or empty.
struct SysInfoExtended {
    std::string build_id;
    std::string connected_bus;
    std::int32_t max_transfer_speed = 0;
    std::int32_t family_id = 0;
    std::string product_type;
    std::string firewire_guid;
    std::string firewire_version;
    std::string min_itunes_version;
    std::string serial_number;
    std::int32_t updater_family_id = 0;
    std::string visible_build_id;
    std::int32_t oem_id = 0;
    std::int32_t oem_u = 0;
    std::int32_t db_version = 0;
    std::string min_build_id;
    std::string language;
    std::int32_t update_method = 0;
    std::int32_t max_fw_blocks = 0;
    std::int32_t fw_part_size = 0;
    std::string volume_format;
    std::int32_t max_thumb_file_size = 0;
    std::int32_t ram = 0;
    std::int32_t hotplug_state = 0;
    std::int32_t battery_poll_interval = 0;
    std::int32_t max_file_size_in_gb = 0;
    std::int32_t max_tracks = 0;
    std::int32_t games_platform_id = 0;
    std::int32_t games_platform_version = 0;
    std::int32_t rental_clock_bias = 0;
    // 0 when the file has no ShadowDBVersion key, 1 for the first and 2 for
    // the second shadow database layout.
    std::int32_t shadow_db_version = 0;

    bool podcasts_supported = false;
    bool playlist_folders_supported = false;
    bool voice_memos_supported = false;
    bool auto_reboot_after_firmware_update = false;
    bool forced_disk_mode = false;
    bool bang_folder = false;
    bool corrupt_data_partition = false;
    bool corrupt_firmware_partition = false;
    bool can_flash_backlight = false;
    bool can_hibernate = false;
    bool came_with_cd = false;
    bool supports_sparse_artwork = false;
    bool sort_fields_supported = false;
    bool vcard_with_jpeg_supported = false;
    bool sqlite_db = false;

    std::vector<ArtworkFormat> artwork_formats;
    std::vector<ArtworkFormat> photo_formats;
    std::vector<ArtworkFormat> chapter_image_formats;
};

// ---- SysInfo -------------------------------------------------------------

// Split "key: value" lines.  Unknown or malformed lines are skipped, the same
// way itdb_device_read_sysinfo() tolerated them.
[[nodiscard]] SysInfoMap parse_sysinfo(std::string_view text);

// Serialise in the SysInfo format ("key: value" per line, sorted by key).
[[nodiscard]] std::string format_sysinfo(const SysInfoMap& values);

// Read Device/SysInfo below a mountpoint; an absent file yields an empty map.
[[nodiscard]] SysInfoMap read_sysinfo(const std::filesystem::path& mountpoint);

// Replace Device/SysInfo atomically.  Returns false when the device directory
// does not exist or the file cannot be replaced.
[[nodiscard]] bool write_sysinfo(const std::filesystem::path& mountpoint,
                                 const SysInfoMap& values);

// ---- SysInfoExtended -----------------------------------------------------

// Convert an already parsed plist (XML or binary) into SysInfoExtended.
// Throws gpod::Error when the root value is not a dictionary.
[[nodiscard]] std::shared_ptr<SysInfoExtended> sysinfo_extended_from_plist(
    const plist::Value& root);

// Parse the SysInfoExtended XML text.  Throws gpod::Error on malformed input.
[[nodiscard]] std::shared_ptr<SysInfoExtended> parse_sysinfo_extended(std::string_view xml);

// Read Device/SysInfoExtended below a mountpoint.  Returns nullptr when the
// file is absent, unreadable or malformed; never throws.
[[nodiscard]] std::shared_ptr<SysInfoExtended> read_sysinfo_extended(
    const std::filesystem::path& mountpoint);

// ---- Device helpers ------------------------------------------------------

// Look up the model table by SysInfo model number ("MA350" or "A350").  Both
// live in itdb_device.c upstream and in device.cpp here.  Returns nullptr when
// the number is unknown; callers fall back to their own "unknown" entry.
[[nodiscard]] const IpodInfo* ipod_info_from_model_number(std::string_view model_number);

// Look up the model table by the last three characters of a serial number
// (see the podsleuth-derived table in itdb_device.c).  Returns nullptr when the
// serial is too short or unknown.
[[nodiscard]] const IpodInfo* ipod_info_from_serial(std::string_view serial);

// Number of consecutive iPod_Control/Music/F00, F01, ... directories.
[[nodiscard]] std::int32_t count_musicdirs(const std::filesystem::path& mountpoint);

// Convert the firewire GUID string ("000A2700xxxxxxxx") into the 20-byte form
// used by the checksum code.  Returns false when it is not valid hex.
[[nodiscard]] bool hex_uuid(std::string_view firewire_id,
                            std::array<std::uint8_t, 20>& uuid) noexcept;

}  // namespace gpod

#endif  // GPOD_SYSINFO_HPP
