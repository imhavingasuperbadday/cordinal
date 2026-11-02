#include "gpod/model.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "gpod/paths.hpp"
#include "gpod/sysinfo.hpp"
#include "gpod/time.hpp"

namespace gpod {
namespace {

#include "device_tables.inc"

// ---------------------------------------------------------------------------
// Hardcoded artwork capability tables
//
// These mirror itdb_device.c: generations whose artwork layout is known get a
// fixed set of formats, used when the device has no SysInfoExtended file.  The
// fields left out of an initialiser keep the ArtworkFormat defaults, exactly as
// the designated C initialisers did upstream.
// ---------------------------------------------------------------------------

constexpr ArtworkFormat kPhotoCoverArtFormats[] = {
    {1017, 56, 56, ThumbFormat::Rgb565LE},
    {1016, 140, 140, ThumbFormat::Rgb565LE},
};

constexpr ArtworkFormat kPhotoPhotoFormats[] = {
    {1009, 42, 30, ThumbFormat::Rgb565LE},
    {1015, 130, 88, ThumbFormat::Rgb565LE},
    {1013, 220, 176, ThumbFormat::Rgb565BE90},
    {1019, 720, 480, ThumbFormat::UyvyBE},
};

constexpr ArtworkFormat kNano1CoverArtFormats[] = {
    {1031, 42, 42, ThumbFormat::Rgb565LE},
    {1027, 100, 100, ThumbFormat::Rgb565LE},
};

constexpr ArtworkFormat kNano1PhotoFormats[] = {
    {1032, 42, 37, ThumbFormat::Rgb565LE},
    {1023, 176, 132, ThumbFormat::Rgb565BE},
};

constexpr ArtworkFormat kNano4CoverArtFormats[] = {
    {1055, 128, 128, ThumbFormat::Rgb565LE},
    {1068, 128, 128, ThumbFormat::Rgb565LE},
    {1071, 240, 240, ThumbFormat::Rgb565LE},
    {1074, 50, 50, ThumbFormat::Rgb565LE},
    {1078, 80, 80, ThumbFormat::Rgb565LE},
    {1084, 240, 240, ThumbFormat::Rgb565LE},
};

constexpr ArtworkFormat kNano4PhotoFormats[] = {
    {1024, 320, 240, ThumbFormat::Rgb565LE},
    {1066, 64, 64, ThumbFormat::Rgb565LE},
    {1079, 80, 80, ThumbFormat::Rgb565LE},
    // 1081 is a JPEG format that the thumbnails writer does not implement.
    {1083, 240, 320, ThumbFormat::Rgb565LE},
};

constexpr ArtworkFormat kNano4ChapterImageFormats[] = {
    {1071, 240, 240, ThumbFormat::Rgb565LE},
};

constexpr ArtworkFormat kNano5CoverArtFormats[] = {
    {1056, 128, 128, ThumbFormat::Rgb565LE},
    {1078, 80, 80, ThumbFormat::Rgb565LE},
    {1073, 240, 240, ThumbFormat::Rgb565LE},
    {1074, 50, 50, ThumbFormat::Rgb565LE},
};

constexpr ArtworkFormat kNano5PhotoFormats[] = {
    {1087, 384, 384, ThumbFormat::Rgb565LE},
    // 1081 is a JPEG format that the thumbnails writer does not implement.
    {1079, 80, 80, ThumbFormat::Rgb565LE},
    {1066, 64, 64, ThumbFormat::Rgb565LE},
};

constexpr ArtworkFormat kVideoCoverArtFormats[] = {
    {1028, 100, 100, ThumbFormat::Rgb565LE},
    {1029, 200, 200, ThumbFormat::Rgb565LE},
};

constexpr ArtworkFormat kVideoPhotoFormats[] = {
    {1036, 50, 41, ThumbFormat::Rgb565LE},
    {1015, 130, 88, ThumbFormat::Rgb565LE},
    {1024, 320, 240, ThumbFormat::Rgb565LE},
    {1019, 720, 480, ThumbFormat::UyvyBE},
};

constexpr ArtworkFormat kMobileCoverArtFormats[] = {
    {2002, 50, 50, ThumbFormat::Rgb565BE},
    {2003, 150, 150, ThumbFormat::Rgb565BE},
};

constexpr ArtworkFormat kTouch1CoverArtFormats[] = {
    {3001, 256, 256, ThumbFormat::RecRgb555LE},
    {3002, 128, 128, ThumbFormat::RecRgb555LE},
    {3003, 64, 64, ThumbFormat::RecRgb555LE},
    {3005, 320, 320, ThumbFormat::Rgb555LE},
    {3006, 56, 56, ThumbFormat::Rgb555LE, 8192},    // pad data to  8192 bytes
    {3007, 88, 88, ThumbFormat::Rgb555LE, 16384},   // pad data to 16384 bytes
};

constexpr ArtworkFormat kTouch1PhotoFormats[] = {
    // The album list uses PHOTO_SMALL to represent a whole album; cropping
    // fills the square completely.
    {3004, 56, 55, ThumbFormat::Rgb555LE, 8192, true},
    // Thumbnail view uses PHOTO_LARGE, 79x79 with a 4px white border on the
    // right and bottom; cropping again fills the square.
    {3011, 80, 79, ThumbFormat::Rgb555LE, 0, true},
    {3009, 160, 120, ThumbFormat::Rgb555LE},
    // PHOTO_TV_SCREEN: the device scales smaller thumbnails to fit, which
    // matters for images taller than they are wide.
    {3008, 640, 480, ThumbFormat::Rgb555LE},
};

// Also used for the 3rd generation nano.
constexpr ArtworkFormat kClassicCoverArtFormats[] = {
    {1061, 56, 56, ThumbFormat::Rgb565LE},  // officially 55x55 -- unverified
    {1055, 128, 128, ThumbFormat::Rgb565LE},
    {1068, 128, 128, ThumbFormat::Rgb565LE},
    {1060, 320, 320, ThumbFormat::Rgb565LE},
};

// Also used for the 3rd generation nano.
constexpr ArtworkFormat kClassicPhotoFormats[] = {
    {1067, 720, 480, ThumbFormat::I420LE},
    {1024, 320, 240, ThumbFormat::Rgb565LE},
    {1066, 64, 64, ThumbFormat::Rgb565LE},
};

// Also used for the 3rd generation nano; these match the iPod video formats,
// which the iPod labels "chapter images".
constexpr ArtworkFormat kClassicChapterImageFormats[] = {
    {1055, 128, 128, ThumbFormat::Rgb565LE},
    {1029, 200, 200, ThumbFormat::Rgb565LE},
};

enum class ArtworkType : std::uint8_t { CoverArt, Photo, ChapterImage };

struct Capabilities {
    std::span<const ArtworkFormat> cover_art;
    std::span<const ArtworkFormat> photo;
    std::span<const ArtworkFormat> chapter_image;
};

template <std::size_t N>
[[nodiscard]] constexpr std::span<const ArtworkFormat> formats_of(
    const ArtworkFormat (&formats)[N]) noexcept {
    return std::span<const ArtworkFormat>(formats, N);
}

[[nodiscard]] Capabilities hardcoded_capabilities(IpodGeneration generation) noexcept {
    switch (generation) {
        case IpodGeneration::Photo:
            return {formats_of(kPhotoCoverArtFormats), formats_of(kPhotoPhotoFormats), {}};
        case IpodGeneration::Video1:
        case IpodGeneration::Video2:
            return {formats_of(kVideoCoverArtFormats), formats_of(kVideoPhotoFormats), {}};
        case IpodGeneration::Nano1:
        case IpodGeneration::Nano2:
            return {formats_of(kNano1CoverArtFormats), formats_of(kNano1PhotoFormats), {}};
        case IpodGeneration::Nano3:
        case IpodGeneration::Classic1:
        case IpodGeneration::Classic2:
        case IpodGeneration::Classic3:
            return {formats_of(kClassicCoverArtFormats), formats_of(kClassicPhotoFormats),
                    formats_of(kClassicChapterImageFormats)};
        case IpodGeneration::Nano4:
            return {formats_of(kNano4CoverArtFormats), formats_of(kNano4PhotoFormats),
                    formats_of(kNano4ChapterImageFormats)};
        case IpodGeneration::Nano5:
            return {formats_of(kNano5CoverArtFormats), formats_of(kNano5PhotoFormats), {}};
        case IpodGeneration::Touch1:
        case IpodGeneration::Touch2:
        case IpodGeneration::Touch3:
        case IpodGeneration::Iphone1:
        case IpodGeneration::Iphone2:
        case IpodGeneration::Iphone3:
            return {formats_of(kTouch1CoverArtFormats), formats_of(kTouch1PhotoFormats), {}};
        case IpodGeneration::Mobile:
            return {formats_of(kMobileCoverArtFormats), {}, {}};
        default:
            // Older iPods, shuffles and later touch devices have no hardcoded
            // layout; they rely on SysInfoExtended.
            return {};
    }
}

// ---------------------------------------------------------------------------
// Serial number to model number mapping
//
// Extracted from the ipod-model-table of podsleuth 0.6.2 (see itdb_device.c).
// The last three characters of an iPod serial number identify its model.
// ---------------------------------------------------------------------------

struct SerialModel {
    std::string_view serial;
    std::string_view model_number;
};

constexpr SerialModel kSerialToModel[] = {
    {"LG6", "8541"},  {"NAM", "8541"},  {"MJ2", "8541"},  {"ML1", "8709"},
    {"MME", "8709"},  {"MMB", "8737"},  {"MMC", "8738"},  {"NGE", "8740"},
    {"NGH", "8740"},  {"MMF", "8741"},  {"NLW", "8946"},  {"NRH", "8976"},
    {"QQF", "9460"},  {"PQ5", "9244"},  {"PNT", "9244"},  {"NLY", "8948"},
    {"NM7", "8948"},  {"PNU", "9245"},  {"PS9", "9282"},  {"Q8U", "9282"},
    {"V9V", "9787"},  {"S2X", "9787"},  {"PQ7", "9268"},  {"TDU", "A079"},
    {"TDS", "A079"},  {"TM2", "A127"},  {"SAZ", "9830"},  {"SB1", "9830"},
    {"SAY", "9829"},  {"R5Q", "9585"},  {"R5R", "9586"},  {"R5T", "9586"},
    {"PFW", "9160"},  {"PRC", "9160"},  {"QKL", "9436"},  {"QKQ", "9436"},
    {"QKK", "9435"},  {"QKP", "9435"},  {"QKJ", "9434"},  {"QKN", "9434"},
    {"QKM", "9437"},  {"QKR", "9437"},  {"S41", "9800"},  {"S4C", "9800"},
    {"S43", "9802"},  {"S45", "9804"},  {"S47", "9806"},  {"S4J", "9806"},
    {"S42", "9801"},  {"S44", "9803"},  {"S48", "9807"},  {"RS9", "9724"},
    {"QGV", "9724"},  {"TSX", "9724"},  {"PFV", "9724"},  {"R80", "9724"},
    {"RSA", "9725"},  {"TSY", "9725"},  {"C60", "9725"},  {"VTE", "A546"},
    {"VTF", "A546"},  {"XQ5", "A947"},  {"XQS", "A947"},  {"XQV", "A949"},
    {"XQX", "A949"},  {"YX7", "A949"},  {"XQY", "A951"},  {"YX8", "A951"},
    {"XR1", "A953"},  {"YXA", "B233"},  {"YX6", "B225"},  {"YX7", "B228"},
    {"YX9", "B225"},  {"8CQ", "C167"},  {"1ZH", "B518"},  {"UNA", "A350"},
    {"UNB", "A350"},  {"UPR", "A352"},  {"UPS", "A352"},  {"SZB", "A004"},
    {"SZV", "A004"},  {"SZW", "A004"},  {"SZC", "A005"},  {"SZT", "A005"},
    {"TJT", "A099"},  {"TJU", "A099"},  {"TK2", "A107"},  {"TK3", "A107"},
    {"VQ5", "A477"},  {"VQ6", "A477"},  {"V8T", "A426"},  {"V8U", "A426"},
    {"V8W", "A428"},  {"V8X", "A428"},  {"VQH", "A487"},  {"VQJ", "A487"},
    {"VQK", "A489"},  {"VKL", "A489"},  {"WL2", "A725"},  {"WL3", "A725"},
    {"X9A", "A726"},  {"X9B", "A726"},  {"VQT", "A497"},  {"VQU", "A497"},
    {"Y0P", "A978"},  {"Y0R", "A980"},  {"YXR", "B249"},  {"YXV", "B257"},
    {"YXT", "B253"},  {"YXX", "B261"},  {"SZ9", "A002"},  {"WEC", "A002"},
    {"WED", "A002"},  {"WEG", "A002"},  {"WEH", "A002"},  {"WEL", "A002"},
    {"TXK", "A146"},  {"TXM", "A146"},  {"WEE", "A146"},  {"WEF", "A146"},
    {"WEJ", "A146"},  {"WEK", "A146"},  {"SZA", "A003"},  {"SZU", "A003"},
    {"TXL", "A147"},  {"TXN", "A147"},  {"V9K", "A444"},  {"V9L", "A444"},
    {"WU9", "A444"},  {"VQM", "A446"},  {"V9M", "A446"},  {"V9N", "A446"},
    {"WEE", "A446"},  {"V9P", "A448"},  {"V9Q", "A448"},  {"V9R", "A450"},
    {"V9S", "A450"},  {"V95", "A450"},  {"V96", "A450"},  {"WUC", "A450"},
    {"W9G", "A664"},  // 30GB iPod Video U2 5.5g
    {"Y5N", "B029"},  // Silver Classic 80GB
    {"YMV", "B147"},  // Black Classic 80GB
    {"YMU", "B145"},  // Silver Classic 160GB
    {"YMX", "B150"},  // Black Classic 160GB
    {"2C5", "B562"},  // Silver Classic 120GB
    {"2C7", "B565"},  // Black Classic 120GB
    {"9ZS", "C293"},  // Silver Classic 160GB (2009)
    {"9ZU", "C297"},  // Black Classic 160GB (2009)

    {"37P", "B663"},  // 4GB Green Nano 4g
    {"37Q", "B666"},  // 4GB Yellow Nano 4g
    {"37H", "B654"},  // 4GB Pink Nano 4g
    {"1P1", "B480"},  // 4GB Silver Nano 4g
    {"37K", "B657"},  // 4GB Purple Nano 4g
    {"37L", "B660"},  // 4GB Orange Nano 4g
    {"2ME", "B598"},  // 8GB Silver Nano 4g
    {"3QS", "B732"},  // 8GB Blue Nano 4g
    {"3QT", "B735"},  // 8GB Pink Nano 4g
    {"3QU", "B739"},  // 8GB Purple Nano 4g
    {"3QW", "B742"},  // 8GB Orange Nano 4g
    {"3QX", "B745"},  // 8GB Green Nano 4g
    {"3QY", "B748"},  // 8GB Yellow Nano 4g
    {"3R0", "B754"},  // 8GB Black Nano 4g
    {"3QZ", "B751"},  // 8GB Red Nano 4g
    {"5B7", "B903"},  // 16GB Silver Nano 4g
    {"5B8", "B905"},  // 16GB Blue Nano 4g
    {"5B9", "B907"},  // 16GB Pink Nano 4g
    {"5BA", "B909"},  // 16GB Purple Nano 4g
    {"5BB", "B911"},  // 16GB Orange Nano 4g
    {"5BC", "B913"},  // 16GB Green Nano 4g
    {"5BD", "B915"},  // 16GB Yellow Nano 4g
    {"5BE", "B917"},  // 16GB Red Nano 4g
    {"5BF", "B918"},  // 16GB Black Nano 4g

    {"71V", "C027"},  // 8GB Silver Nano 5g
    {"71Y", "C031"},  // 8GB Black Nano 5g
    {"721", "C034"},  // 8GB Purple Nano 5g
    {"726", "C037"},  // 8GB Blue Nano 5g
    {"72A", "C040"},  // 8GB Green Nano 5g
    {"72F", "C046"},  // 8GB Orange Nano 5g
    {"72K", "C049"},  // 8GB Red Nano 5g
    {"72L", "C050"},  // 8GB Pink Nano 5g

    {"72Q", "C060"},  // 16GB Silver Nano 5g
    {"72R", "C062"},  // 16GB Black Nano 5g
    {"72S", "C064"},  // 16GB Purple Nano 5g
    {"72X", "C066"},  // 16GB Blue Nano 5g
    {"734", "C068"},  // 16GB Green Nano 5g
    {"738", "C070"},  // 16GB Yellow Nano 5g
    {"739", "C072"},  // 16GB Orange Nano 5g
    {"73A", "C074"},  // 16GB Red Nano 5g
    {"73B", "C075"},  // 16GB Pink Nano 5g

    {"CMN", "C525"},  // 8GB Silver Nano 6g
    {"DVX", "C688"},  // 8GB Black Nano 6g
    {"DVY", "C689"},  // 8GB Blue Nano 6g
    {"DW0", "C690"},  // 8GB Green Nano 6g
    {"DW1", "C691"},  // 8GB Orange Nano 6g
    {"DW2", "C692"},  // 8GB Pink Nano 6g
    {"DW3", "C693"},  // 8GB Pink Nano 6g
    {"CMP", "C526"},  // 16GB Silver Nano 6g
    {"DW4", "C694"},  // 16GB Black Nano 6g
    {"DW5", "C695"},  // 16GB Blue Nano 6g
    {"DW6", "C696"},  // 16GB Green Nano 6g
    {"DW7", "C697"},  // 16GB Orange Nano 6g
    {"DW8", "C698"},  // 16GB Pink Nano 6g
    {"DW9", "C699"},  // 16GB Red Nano 6g

    {"A1S", "C306"},  // 2GB Silver Shuffle 3g
    {"A78", "C323"},  // 2GB Black Shuffle 3g
    {"ALB", "C381"},  // 2GB Green Shuffle 3g
    {"ALD", "C384"},  // 2GB Blue Shuffle 3g
    {"ALG", "C387"},  // 2GB Pink Shuffle 3g
    {"4NZ", "B867"},  // 4GB Silver Shuffle 3g
    {"891", "C164"},  // 4GB Black Shuffle 3g
    {"A1L", "C303"},  // 4GB Stainless Shuffle 3g
    {"A1U", "C307"},  // 4GB Green Shuffle 3g
    {"A7B", "C328"},  // 4GB Blue Shuffle 3g
    {"A7D", "C331"},  // 4GB Pink Shuffle 3g

    {"CMJ", "C584"},  // 2GB Silver Shuffle 4g
    {"CMK", "C585"},  // 2GB Pink Shuffle 4g
    {"FDM", "C749"},  // 2GB Orange Shuffle 4g
    {"FDN", "C750"},  // 2GB Green Shuffle 4g
    {"FDP", "C751"},  // 2GB Blue Shuffle 4g

    {"W4N", "A623"},  // 8GB Silver iPod Touch (1st gen)
    {"W4T", "A627"},  // 16GB Silver iPod Touch (1st gen)
    {"0JW", "B376"},  // 32GB Silver iPod Touch (1st gen)
    {"201", "B528"},  // 8GB Silver iPod Touch (2nd gen)
    {"203", "B531"},  // 16GB Silver iPod Touch (2nd gen)
    {"75J", "C086"},  // 8GB iPod Touch (3rd gen)
    {"6K2", "C008"},  // 32GB iPod Touch (3rd gen)
    {"6K4", "C011"},  // 64GB iPod Touch (3rd gen)
    {"CP7", "C540"},  // 8GB iPod Touch (4th gen)
    {"CP9", "C544"},  // 32GB iPod Touch (4th gen)
    {"CPC", "C547"},  // 64GB iPod Touch (4th gen)

    {"VR0", "A501"},  // 4GB Silver iPhone 1st gen
    {"WH8", "A712"},  // 8GB Silver iPhone
    {"0KH", "B384"},  // 16GB Silver iPhone
    {"Y7H", "B046"},  // 8GB Black iPhone 3G
    {"Y7K", "B496"},  // 16GB Black iPhone 3G
    {"3NP", "C131"},  // 16GB Black iPhone 3GS
    {"3NR", "C133"},  // 32GB Black iPhone 3GS
    {"3NS", "C134"},  // 32GB White iPhone 3GS
    {"A4S", "C603"},  // 16GB Black iPhone 4G
    {"A4T", "C605"},  // 32GB Black iPhone 4G

    {"Z38", "B292"},  // 16GB iPad with Wifi
    {"Z39", "B293"},  // 32GB iPad with Wifi
    {"Z3A", "B294"},  // 64GB iPad with Wifi
};

// ---------------------------------------------------------------------------
// Model table lookups and capability queries
// ---------------------------------------------------------------------------

[[nodiscard]] const IpodInfo* find_model_number(std::string_view model_number) noexcept {
    for (const IpodInfo& info : kIpodInfoTable) {
        if (model_number == info.model_number) {
            return &info;
        }
    }
    return nullptr;
}

[[nodiscard]] std::span<const ArtworkFormat> extended_formats(const Device& device,
                                                              ArtworkType type) noexcept {
    if (device.sysinfo_extended == nullptr) {
        return {};
    }
    const SysInfoExtended& extended = *device.sysinfo_extended;
    switch (type) {
        case ArtworkType::CoverArt:
            return extended.artwork_formats;
        case ArtworkType::Photo:
            return extended.photo_formats;
        case ArtworkType::ChapterImage:
            return extended.chapter_image_formats;
    }
    return {};
}

// SysInfoExtended wins when present (even when it lists no formats, which
// means the device supports none); otherwise the hardcoded table applies.
[[nodiscard]] std::span<const ArtworkFormat> available_formats(const Device& device,
                                                               ArtworkType type) noexcept {
    if (device.sysinfo_extended != nullptr) {
        return extended_formats(device, type);
    }
    const Capabilities capabilities = hardcoded_capabilities(device.ipod_info().generation);
    switch (type) {
        case ArtworkType::CoverArt:
            return capabilities.cover_art;
        case ArtworkType::Photo:
            return capabilities.photo;
        case ArtworkType::ChapterImage:
            return capabilities.chapter_image;
    }
    return {};
}

[[nodiscard]] std::vector<ArtworkFormat> collect_formats(const Device& device,
                                                         ArtworkType type) {
    const std::span<const ArtworkFormat> formats = available_formats(device, type);
    return {formats.begin(), formats.end()};
}

// Keep the public Device::cover_art_formats/photo_formats/chapter_image_formats
// members in sync with the model and SysInfoExtended state.
void refresh_capabilities(Device& device) {
    device.cover_art_formats = collect_formats(device, ArtworkType::CoverArt);
    device.photo_formats = collect_formats(device, ArtworkType::Photo);
    device.chapter_image_formats = collect_formats(device, ArtworkType::ChapterImage);
}

[[nodiscard]] std::optional<ByteOrder> probe_byte_order(const std::filesystem::path& file,
                                                        std::string_view header) {
    std::ifstream input(file, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    std::array<char, 4> buffer{};
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    if (input.gcount() != static_cast<std::streamsize>(buffer.size())) {
        return std::nullopt;
    }
    const std::string_view magic(buffer.data(), buffer.size());
    if (magic == header) {
        return ByteOrder::Little;
    }
    const std::string reversed(header.rbegin(), header.rend());
    if (magic == reversed) {
        return ByteOrder::Big;
    }
    return std::nullopt;
}

// itdb_device_autodetect_endianess() reads the database magic rather than
// trusting the mount path; iPhone dumps name their directory
// ".../iTunes/iTunes_Control" with a reversed byte order.
[[nodiscard]] ByteOrder detect_byte_order(const std::filesystem::path& mountpoint) {
    if (mountpoint.empty()) {
        return ByteOrder::Little;
    }
    if (const auto order = probe_byte_order(itunesdb_path(mountpoint), "mhbd")) {
        return *order;
    }
    if (const auto order = probe_byte_order(artworkdb_path(mountpoint), "mhfd")) {
        return *order;
    }
    if (const auto order = probe_byte_order(photodb_path(mountpoint), "mhfd")) {
        return *order;
    }
    std::string control = control_dir(mountpoint).generic_string();
    std::transform(control.begin(), control.end(), control.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static constexpr std::string_view kItunesControl = "itunes/itunes_control";
    if (control.size() >= kItunesControl.size() &&
        control.compare(control.size() - kItunesControl.size(), kItunesControl.size(),
                        kItunesControl) == 0) {
        return ByteOrder::Big;
    }
    return ByteOrder::Little;
}

}  // namespace

Device::Device() = default;

Device::~Device() = default;

std::optional<StorageInfo> Device::storage_info() const {
    if (mountpoint.empty()) {
        return std::nullopt;
    }
    std::error_code error;
    const std::filesystem::space_info space = std::filesystem::space(mountpoint, error);
    if (error) {
        return std::nullopt;
    }
    return StorageInfo{space.capacity, space.free};
}

const IpodInfo* ipod_info_from_model_number(std::string_view model_number) {
    // SysInfo usually stores "MA350" while the table holds "A350"; the original
    // code always dropped the first character.  Trying the value as given first
    // additionally accepts files that already hold the bare model number.
    if (const IpodInfo* info = find_model_number(model_number)) {
        return info;
    }
    if (!model_number.empty() &&
        std::isalpha(static_cast<unsigned char>(model_number.front())) != 0) {
        return find_model_number(model_number.substr(1));
    }
    return nullptr;
}

const IpodInfo* ipod_info_from_serial(std::string_view serial) {
    if (serial.size() < 3) {
        return nullptr;
    }
    const std::string_view suffix = serial.substr(serial.size() - 3);
    // The upstream table was loaded into a hash table, so later duplicates of
    // the same three-letter code replaced earlier ones; scanning backwards
    // reproduces that precedence.
    for (auto entry = std::rbegin(kSerialToModel); entry != std::rend(kSerialToModel);
         ++entry) {
        if (entry->serial == suffix) {
            return ipod_info_from_model_number(entry->model_number);
        }
    }
    return nullptr;
}

IpodInfo Device::ipod_info() const {
    if (sysinfo_extended != nullptr) {
        if (const IpodInfo* info = ipod_info_from_serial(sysinfo_extended->serial_number)) {
            return *info;
        }
    }
    const auto model = sysinfo.find("ModelNumStr");
    if (model == sysinfo.end()) {
        return kIpodInfoTable[0];
    }
    if (const IpodInfo* info = ipod_info_from_model_number(model->second)) {
        return *info;
    }
    return kIpodInfoTable[1];
}

bool Device::supports_artwork() const {
    return !available_formats(*this, ArtworkType::CoverArt).empty();
}

bool Device::supports_chapter_image() const {
    return !available_formats(*this, ArtworkType::ChapterImage).empty();
}

bool Device::supports_photo() const {
    return !available_formats(*this, ArtworkType::Photo).empty();
}

bool Device::supports_video() const {
    switch (ipod_info().generation) {
        case IpodGeneration::Nano3:
        case IpodGeneration::Nano4:
        case IpodGeneration::Nano5:
        case IpodGeneration::Video1:
        case IpodGeneration::Video2:
        case IpodGeneration::Classic1:
        case IpodGeneration::Classic2:
        case IpodGeneration::Classic3:
        case IpodGeneration::Touch1:
        case IpodGeneration::Touch2:
        case IpodGeneration::Touch3:
        case IpodGeneration::Touch4:
        case IpodGeneration::Iphone1:
        case IpodGeneration::Iphone2:
        case IpodGeneration::Iphone3:
        case IpodGeneration::Iphone4:
        case IpodGeneration::Ipad1:
            return true;
        default:
            return false;
    }
}

bool Device::supports_podcast() const {
    if (sysinfo_extended != nullptr) {
        return sysinfo_extended->podcasts_supported;
    }
    switch (ipod_info().generation) {
        case IpodGeneration::Unknown:
        case IpodGeneration::First:
        case IpodGeneration::Second:
        case IpodGeneration::Third:
        case IpodGeneration::Mobile:
            return false;
        default:
            return true;
    }
}

bool Device::supports_sparse_artwork() const {
    if (sysinfo_extended != nullptr && sysinfo_extended->supports_sparse_artwork) {
        return true;
    }
    switch (ipod_info().generation) {
        case IpodGeneration::Nano3:
        case IpodGeneration::Nano4:
        case IpodGeneration::Nano5:
        case IpodGeneration::Nano6:
        case IpodGeneration::Classic1:
        case IpodGeneration::Classic2:
        case IpodGeneration::Classic3:
        case IpodGeneration::Touch1:
        case IpodGeneration::Touch2:
        case IpodGeneration::Touch3:
        case IpodGeneration::Touch4:
        case IpodGeneration::Iphone1:
        case IpodGeneration::Iphone2:
        case IpodGeneration::Iphone3:
        case IpodGeneration::Iphone4:
        case IpodGeneration::Ipad1:
            return true;
        default:
            return false;
    }
}

bool Device::supports_sqlite_db() const {
    if (sysinfo_extended != nullptr) {
        return sysinfo_extended->sqlite_db;
    }
    switch (ipod_info().generation) {
        case IpodGeneration::Nano5:
        case IpodGeneration::Nano6:
        case IpodGeneration::Touch1:
        case IpodGeneration::Touch2:
        case IpodGeneration::Touch3:
        case IpodGeneration::Touch4:
        case IpodGeneration::Iphone1:
        case IpodGeneration::Iphone2:
        case IpodGeneration::Iphone3:
        case IpodGeneration::Iphone4:
        case IpodGeneration::Ipad1:
            return true;
        default:
            return false;
    }
}

bool Device::supports_compressed_itunesdb() const { return supports_sqlite_db(); }

ChecksumType Device::checksum_type() const {
    if (sysinfo_extended != nullptr) {
        // The database version, not the generation, selects the checksum on
        // devices that ship a SysInfoExtended file.
        switch (sysinfo_extended->db_version) {
            case 0:
            case 1:
            case 2:
                return ChecksumType::None;
            case 3:
                return ChecksumType::Hash58;
            case 4:
                return ChecksumType::Hash72;
            case 5:
                return ChecksumType::HashAB;
            default:
                return ChecksumType::Unknown;
        }
    }
    switch (ipod_info().generation) {
        case IpodGeneration::Classic1:
        case IpodGeneration::Classic2:
        case IpodGeneration::Classic3:
        case IpodGeneration::Nano3:
        case IpodGeneration::Nano4:
            return ChecksumType::Hash58;
        case IpodGeneration::Nano5:
        case IpodGeneration::Touch1:
        case IpodGeneration::Touch2:
        case IpodGeneration::Touch3:
        case IpodGeneration::Iphone1:
        case IpodGeneration::Iphone2:
        case IpodGeneration::Iphone3:
            return ChecksumType::Hash72;
        case IpodGeneration::Ipad1:
        case IpodGeneration::Iphone4:
        case IpodGeneration::Touch4:
        case IpodGeneration::Nano6:
            return ChecksumType::HashAB;
        default:
            return ChecksumType::None;
    }
}

ShadowDbVersion Device::shadow_db_version() const {
    if (sysinfo_extended != nullptr) {
        switch (sysinfo_extended->shadow_db_version) {
            case 1:
                return ShadowDbVersion::V1;
            case 2:
                return ShadowDbVersion::V2;
            case 0:
                break;  // not recorded in the file: fall back to the model
            default:
                // Upstream returned the raw version as the enum value; clamp to
                // Unknown so the value stays inside the enum's range.
                return ShadowDbVersion::Unknown;
        }
    }
    switch (ipod_info().generation) {
        case IpodGeneration::Shuffle1:
        case IpodGeneration::Shuffle2:
            return ShadowDbVersion::V1;
        case IpodGeneration::Shuffle3:
        case IpodGeneration::Shuffle4:
            return ShadowDbVersion::V2;
        default:
            return ShadowDbVersion::Unknown;
    }
}

std::string Device::sysinfo_value(std::string_view field) const {
    const auto entry = sysinfo.find(field);
    return entry == sysinfo.end() ? std::string{} : entry->second;
}

void Device::set_sysinfo(std::string field, std::string value) {
    if (value.empty()) {
        sysinfo.erase(field);
    } else {
        sysinfo.insert_or_assign(std::move(field), std::move(value));
    }
    sysinfo_changed = true;
    refresh_capabilities(*this);
}

void Device::set_mountpoint(const std::filesystem::path& path) {
    mountpoint = path;
    if (path.empty()) {
        musicdirs = 0;
        sysinfo.clear();
        sysinfo_extended.reset();
        sysinfo_changed = false;
        timezone_shift = local_timezone_shift();
        byte_order = ByteOrder::Little;
        refresh_capabilities(*this);
        return;
    }
    musicdirs = count_musicdirs(path);
    // Device metadata is best effort: a missing or damaged SysInfo or
    // SysInfoExtended must not stop the caller from opening the device.
    sysinfo = read_sysinfo(path);
    sysinfo_extended = read_sysinfo_extended(path);
    if (!sysinfo.empty() && sysinfo_extended != nullptr &&
        !sysinfo_extended->firewire_guid.empty()) {
        // libgpod trusts SysInfoExtended for the firewire GUID because the
        // checksum code needs it.
        sysinfo.insert_or_assign("FirewireGuid", sysinfo_extended->firewire_guid);
    }
    sysinfo_changed = false;
    update_timezone_shift(*this);
    byte_order = detect_byte_order(path);
    refresh_capabilities(*this);
}

std::string Device::uuid() const { return sysinfo_value("FirewireGuid"); }

std::string Device::firewire_id() const {
    if (sysinfo_extended != nullptr && !sysinfo_extended->firewire_guid.empty()) {
        return sysinfo_extended->firewire_guid;
    }
    return sysinfo_value("FirewireGuid");
}

std::span<const IpodInfo> ipod_info_table() { return kIpodInfoTable; }

std::string_view to_string(IpodModel model) noexcept {
    const auto index = static_cast<std::size_t>(model);
    if (index < std::size(kIpodModelNames)) {
        return kIpodModelNames[index];
    }
    return "Unknown";
}

std::string_view to_string(IpodGeneration generation) noexcept {
    const auto index = static_cast<std::size_t>(generation);
    if (index < std::size(kIpodGenerationNames)) {
        return kIpodGenerationNames[index];
    }
    return "Unknown";
}

}  // namespace gpod
