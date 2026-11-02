#include "gpod/sysinfo.hpp"

#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>

#include "gpod/error.hpp"
#include "gpod/io.hpp"
#include "gpod/paths.hpp"

namespace gpod {
namespace {

[[nodiscard]] bool is_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

[[nodiscard]] std::string trim(std::string_view value) {
    std::size_t begin = 0;
    while (begin < value.size() && is_space(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && is_space(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return std::string(value.substr(begin, end - begin));
}

[[nodiscard]] bool read_whole_file(const std::filesystem::path& path, std::string& content) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }
    content.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return !input.bad();
}

// ---- SysInfoExtended field readers --------------------------------------

[[nodiscard]] std::string string_field(const plist::Value& dict, std::string_view key) {
    const plist::Value* entry = dict.find(key);
    return entry == nullptr ? std::string{} : std::string(entry->text());
}

[[nodiscard]] std::int32_t int_field(const plist::Value& dict, std::string_view key) {
    const plist::Value* entry = dict.find(key);
    // Upstream widened the 64-bit plist value into a gint; values outside that
    // range only appear in corrupted files, so truncation is intentional.
    return entry == nullptr ? 0 : static_cast<std::int32_t>(entry->integer());
}

[[nodiscard]] bool bool_field(const plist::Value& dict, std::string_view key) {
    const plist::Value* entry = dict.find(key);
    return entry != nullptr && entry->boolean();
}

[[nodiscard]] double real_field(const plist::Value& dict, std::string_view key) {
    const plist::Value* entry = dict.find(key);
    return entry == nullptr ? 0.0 : entry->real();
}

// PixelFormat is a string holding the four-character format code as hex.
// Returns false for formats the artwork writer cannot produce.
[[nodiscard]] bool set_pixel_format(ArtworkFormat& format, const plist::Value& dict) {
    const std::string_view code = dict.text_of("PixelFormat");
    if (code == "32767579") {  // 2vuy
        format.format = ThumbFormat::UyvyBE;
    } else if (code == "42353635") {  // B565
        format.format = ThumbFormat::Rgb565BE;
    } else if (code == "4C353635") {  // L565
        format.format = ThumbFormat::Rgb565LE;
    } else if (code == "79343230") {  // y420
        format.format = ThumbFormat::I420LE;
    } else if (code == "4C353535") {  // L555
        // iPods with a palette-based alpha channel ("PixelOrder") stored the
        // same format id with a different byte order.
        format.format = dict.contains("PixelOrder") ? ThumbFormat::RecRgb555LE
                                                    : ThumbFormat::Rgb555LE;
    } else {
        return false;
    }
    return true;
}

void set_back_color(ArtworkFormat& format, const plist::Value& dict) {
    const std::string_view text = dict.text_of("BackColor");
    if (text.empty()) {
        return;
    }
    std::uint32_t value = 0;
    for (const char c : text) {
        unsigned digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<unsigned>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<unsigned>(10 + (c - 'a'));
        } else if (c >= 'A' && c <= 'F') {
            digit = static_cast<unsigned>(10 + (c - 'A'));
        } else {
            continue;  // ignore separators such as "0x"
        }
        value = (value << 4) | digit;
    }
    for (std::size_t index = format.back_color.size(); index > 0; --index) {
        format.back_color[index - 1] = static_cast<std::uint8_t>(value & 0xFF);
        value >>= 8;
    }
}

[[nodiscard]] std::optional<ArtworkFormat> image_format(const plist::Value& dict) {
    const plist::Value* pixel_format = dict.find("PixelFormat");
    if (pixel_format == nullptr || !pixel_format->is_string()) {
        // Formats without a pixel format are unusable for the artwork writer;
        // libgpod dropped them as well.
        return std::nullopt;
    }
    ArtworkFormat format;
    if (!set_pixel_format(format, dict)) {
        return std::nullopt;
    }
    set_back_color(format, dict);

    format.format_id = int_field(dict, "FormatId");
    format.display_width = int_field(dict, "DisplayWidth");
    format.width = int_field(dict, "RenderWidth");
    format.height = int_field(dict, "RenderHeight");
    format.interlaced = bool_field(dict, "Interlaced");
    format.crop = bool_field(dict, "Crop");
    format.rotation = int_field(dict, "Rotation");
    format.color_adjustment = int_field(dict, "ColorAdjustment");
    format.gamma = real_field(dict, "GammaAdjustment");
    format.associated_format = int_field(dict, "AssociatedFormat");
    format.row_bytes_alignment = int_field(dict, "RowBytesAlignment");
    if (format.row_bytes_alignment == 0 && bool_field(dict, "AlignRowBytes")) {
        // The nano 3g only has AlignRowBytes, which means the same as a
        // RowBytesAlignment of 4.
        format.row_bytes_alignment = 4;
    }
    return format;
}

[[nodiscard]] std::vector<ArtworkFormat> formats_field(const plist::Value& dict,
                                                       std::string_view key) {
    std::vector<ArtworkFormat> formats;
    const plist::Value* list = dict.find(key);
    if (list == nullptr || !list->is_array()) {
        return formats;
    }
    for (const plist::Value& entry : list->array()) {
        if (!entry.is_dictionary()) {
            // SysInfoExtended on the iPhone mixes <string> entries into the
            // format lists.
            continue;
        }
        std::optional<ArtworkFormat> format = image_format(entry);
        if (format.has_value()) {
            formats.push_back(*format);
        }
    }
    return formats;
}

// "AlbumArt" replaced "AlbumArt2" on newer devices; the same applies to the
// photo and chapter image lists.
[[nodiscard]] std::vector<ArtworkFormat> formats_field_with_fallback(
    const plist::Value& dict, std::string_view primary, std::string_view fallback) {
    std::vector<ArtworkFormat> formats = formats_field(dict, primary);
    if (formats.empty()) {
        formats = formats_field(dict, fallback);
    }
    return formats;
}

}  // namespace

// ---------------------------------------------------------------------------
// SysInfo
// ---------------------------------------------------------------------------

SysInfoMap parse_sysinfo(std::string_view text) {
    SysInfoMap values;
    while (!text.empty()) {
        const std::size_t newline = text.find('\n');
        std::string_view line =
            newline == std::string_view::npos ? text : text.substr(0, newline);
        text = newline == std::string_view::npos ? std::string_view{}
                                                 : text.substr(newline + 1);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) {
            continue;
        }
        const std::string key = trim(line.substr(0, colon));
        if (key.empty()) {
            continue;
        }
        values.insert_or_assign(key, trim(line.substr(colon + 1)));
    }
    return values;
}

std::string format_sysinfo(const SysInfoMap& values) {
    std::string text;
    for (const auto& [key, value] : values) {
        text.append(key);
        text.append(": ");
        text.append(value);
        text.push_back('\n');
    }
    return text;
}

SysInfoMap read_sysinfo(const std::filesystem::path& mountpoint) {
    if (mountpoint.empty()) {
        return {};
    }
    std::string content;
    if (!read_whole_file(device_dir(mountpoint) / "SysInfo", content)) {
        return {};
    }
    return parse_sysinfo(content);
}

bool write_sysinfo(const std::filesystem::path& mountpoint, const SysInfoMap& values) {
    if (mountpoint.empty()) {
        return false;
    }
    const std::filesystem::path directory = device_dir(mountpoint);
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error)) {
        // libgpod deliberately does not create the device directory.
        return false;
    }
    const std::string content = format_sysinfo(values);
    try {
        write_file_atomic(directory / "SysInfo",
                          std::span<const std::uint8_t>(
                              reinterpret_cast<const std::uint8_t*>(content.data()),
                              content.size()));
    } catch (const Error&) {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// SysInfoExtended
// ---------------------------------------------------------------------------

std::shared_ptr<SysInfoExtended> sysinfo_extended_from_plist(const plist::Value& root) {
    if (!root.is_dictionary()) {
        fail(ErrorCode::Corrupt, "SysInfoExtended: root value is not a dictionary");
    }
    auto properties = std::make_shared<SysInfoExtended>();
    properties->artwork_formats =
        formats_field_with_fallback(root, "AlbumArt", "AlbumArt2");
    properties->photo_formats =
        formats_field_with_fallback(root, "ImageSpecifications", "ImageSpecifications2");
    properties->chapter_image_formats =
        formats_field_with_fallback(root, "ChapterImageSpecs", "ChapterImageSpecs2");

    properties->build_id = string_field(root, "BuildID");
    properties->connected_bus = string_field(root, "ConnectedBus");
    properties->max_transfer_speed = int_field(root, "MaxTransferSpeed");
    properties->family_id = int_field(root, "FamilyID");
    properties->product_type = string_field(root, "ProductType");
    properties->firewire_guid = string_field(root, "FireWireGUID");
    properties->firewire_version = string_field(root, "FireWireVersion");
    properties->min_itunes_version = string_field(root, "MinITunesVersion");
    properties->serial_number = string_field(root, "SerialNumber");
    properties->updater_family_id = int_field(root, "UpdaterFamilyID");
    properties->visible_build_id = string_field(root, "VisibleBuildID");
    properties->oem_id = int_field(root, "OEMID");
    properties->oem_u = int_field(root, "OEMU");
    properties->db_version = int_field(root, "DBVersion");
    properties->min_build_id = string_field(root, "MinBuildID");
    properties->language = string_field(root, "Language");
    properties->update_method = int_field(root, "UpdateMethod");
    properties->max_fw_blocks = int_field(root, "MaxFWBlocks");
    properties->fw_part_size = int_field(root, "FWPartSize");
    properties->volume_format = string_field(root, "VolumeFormat");
    properties->max_thumb_file_size = int_field(root, "MaxThumbFileSize");
    properties->ram = int_field(root, "RAM");
    properties->hotplug_state = int_field(root, "HotPlugState");
    properties->battery_poll_interval = int_field(root, "BatteryPollInterval");
    properties->max_file_size_in_gb = int_field(root, "MaxFileSizeInGB");
    properties->max_tracks = int_field(root, "MaxTracks");
    properties->games_platform_id = int_field(root, "GamesPlatformID");
    properties->games_platform_version = int_field(root, "GamesPlatformVersion");
    properties->rental_clock_bias = int_field(root, "RentalClockBias");
    properties->shadow_db_version = int_field(root, "ShadowDBVersion");

    properties->podcasts_supported = bool_field(root, "PodcastsSupported");
    properties->playlist_folders_supported = bool_field(root, "PlaylistFoldersSupported");
    properties->voice_memos_supported = bool_field(root, "VoiceMemosSupported");
    properties->auto_reboot_after_firmware_update =
        bool_field(root, "AutoRebootAfterFirmwareUpdate");
    properties->forced_disk_mode = bool_field(root, "ForcedDiskMode");
    properties->bang_folder = bool_field(root, "BangFolder");
    properties->corrupt_data_partition = bool_field(root, "CorruptDataPartition");
    properties->corrupt_firmware_partition = bool_field(root, "CorruptFirmwarePartition");
    properties->can_flash_backlight = bool_field(root, "CanFlashBacklight");
    properties->can_hibernate = bool_field(root, "CanHibernate");
    properties->came_with_cd = bool_field(root, "CameWithCD");
    properties->supports_sparse_artwork = bool_field(root, "SupportsSparseArtwork");
    properties->sort_fields_supported = bool_field(root, "SortFieldsSupported");
    properties->vcard_with_jpeg_supported = bool_field(root, "vCardWithJPEGSupported");
    properties->sqlite_db = bool_field(root, "SQLiteDB");

    return properties;
}

std::shared_ptr<SysInfoExtended> parse_sysinfo_extended(std::string_view xml) {
    return sysinfo_extended_from_plist(plist::parse_xml(xml));
}

std::shared_ptr<SysInfoExtended> read_sysinfo_extended(
    const std::filesystem::path& mountpoint) {
    if (mountpoint.empty()) {
        return nullptr;
    }
    const std::filesystem::path file = device_dir(mountpoint) / "SysInfoExtended";
    std::error_code error;
    if (!std::filesystem::is_regular_file(file, error)) {
        return nullptr;
    }
    plist::Value value;
    if (!plist::try_parse_file(file, value)) {
        return nullptr;
    }
    try {
        return sysinfo_extended_from_plist(value);
    } catch (const Error&) {
        return nullptr;
    }
}

// ---------------------------------------------------------------------------
// Device helpers
// ---------------------------------------------------------------------------

std::int32_t count_musicdirs(const std::filesystem::path& mountpoint) {
    if (mountpoint.empty()) {
        return 0;
    }
    const std::filesystem::path directory = music_dir(mountpoint);
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error)) {
        return 0;
    }
    // libgpod counts the contiguous F00.. run and stops at the first gap, so a
    // stray high directory number does not inflate the count.
    std::int32_t count = 0;
    while (count < 100) {
        char name[8] = {};
        std::snprintf(name, sizeof name, "F%02d", count);
        if (!std::filesystem::is_directory(directory / name, error)) {
            break;
        }
        ++count;
    }
    return count;
}

bool hex_uuid(std::string_view firewire_id, std::array<std::uint8_t, 20>& uuid) noexcept {
    uuid.fill(0);
    // libgpod accepted an optional "0x" prefix.
    if (firewire_id.size() > 2 && firewire_id[0] == '0' &&
        (firewire_id[1] == 'x' || firewire_id[1] == 'X')) {
        firewire_id.remove_prefix(2);
    }
    if (firewire_id.empty() || firewire_id.size() % 2 != 0 ||
        firewire_id.size() / 2 > uuid.size()) {
        return false;
    }
    const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return 10 + (c - 'a');
        }
        if (c >= 'A' && c <= 'F') {
            return 10 + (c - 'A');
        }
        return -1;
    };
    for (std::size_t index = 0; index < firewire_id.size(); index += 2) {
        const int high = digit(firewire_id[index]);
        const int low = digit(firewire_id[index + 1]);
        if (high < 0 || low < 0) {
            uuid.fill(0);
            return false;
        }
        uuid[index / 2] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return true;
}

}  // namespace gpod
