// Domain types shared by the pure C++ libgpod rewrite.
//
// Numeric values match the on-disk iTunesDB constants and the original
// libgpod headers so the rewrite stays byte-compatible with existing iPods.

#ifndef GPOD_TYPES_HPP
#define GPOD_TYPES_HPP

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace gpod {

enum class ByteOrder { Little, Big };

enum class IpodGeneration : std::uint8_t {
    Unknown,
    First,
    Second,
    Third,
    Fourth,
    Photo,
    Mobile,
    Mini1,
    Mini2,
    Shuffle1,
    Shuffle2,
    Shuffle3,
    Nano1,
    Nano2,
    Nano3,
    Nano4,
    Video1,
    Video2,
    Classic1,
    Classic2,
    Touch1,
    Iphone1,
    Shuffle4,
    Touch2,
    Iphone2,
    Iphone3,
    Classic3,
    Nano5,
    Touch3,
    Ipad1,
    Iphone4,
    Touch4,
    Nano6,
};

enum class IpodModel : std::uint8_t {
    Invalid,
    Unknown,
    Color,
    ColorU2,
    Regular,
    RegularU2,
    Mini,
    MiniBlue,
    MiniPink,
    MiniGreen,
    MiniGold,
    Shuffle,
    NanoWhite,
    NanoBlack,
    VideoWhite,
    VideoBlack,
    Mobile1,
    VideoU2,
    NanoSilver,
    NanoBlue,
    NanoGreen,
    NanoPink,
    NanoRed,
    NanoYellow,
    NanoPurple,
    NanoOrange,
    Iphone1,
    ShuffleSilver,
    ShufflePink,
    ShuffleBlue,
    ShuffleGreen,
    ShuffleOrange,
    ShufflePurple,
    ShuffleRed,
    ClassicSilver,
    ClassicBlack,
    TouchSilver,
    ShuffleBlack,
    IphoneWhite,
    IphoneBlack,
    ShuffleGold,
    ShuffleStainless,
    Ipad,
};

struct IpodInfo {
    // Literals from the fixed table, so the table needs no startup construction.
    std::string_view model_number;
    double capacity = 0.0;
    IpodModel model = IpodModel::Invalid;
    IpodGeneration generation = IpodGeneration::Unknown;
    std::uint32_t musicdirs = 0;
};

enum class ThumbFormat : std::uint8_t {
    UyvyLE,
    UyvyBE,
    I420LE,
    I420BE,
    Rgb565LE,
    Rgb565LE90,
    Rgb565BE,
    Rgb565BE90,
    Rgb555LE,
    Rgb555LE90,
    Rgb555BE,
    Rgb555BE90,
    RecRgb555LE,
    RecRgb555LE90,
    RecRgb555BE,
    RecRgb555BE90,
    Rgb888LE,
    Rgb888LE90,
    Rgb888BE,
    Rgb888BE90,
    ExperimentalLE,
    ExperimentalBE,
};

enum class ChecksumType : std::int8_t {
    Unknown = -1,
    None = 0,
    Hash58 = 1,
    Hash72 = 2,
    HashAB = 3,
};

enum class ShadowDbVersion : std::uint8_t { Unknown, V1, V2 };

enum class PlaylistType : std::uint8_t { Normal = 0, Master = 1 };
enum class PlaylistFlag : std::uint8_t { Normal = 0, Podcasts = 1 };

enum class PlaylistSortOrder : std::uint8_t {
    Manual = 1,
    Title = 3,
    Album = 4,
    Artist = 5,
    Bitrate = 6,
    Genre = 7,
    Filetype = 8,
    TimeModified = 9,
    TrackNr = 10,
    Size = 11,
    Time = 12,
    Year = 13,
    Samplerate = 14,
    Comment = 15,
    TimeAdded = 16,
    Equalizer = 17,
    Composer = 18,
    Playcount = 20,
    TimePlayed = 21,
    CdNr = 22,
    Rating = 23,
    ReleaseDate = 24,
    Bpm = 25,
    Grouping = 26,
    Category = 27,
    Description = 28,
};

enum class Mediatype : std::uint32_t {
    None = 0,
    Audio = 1u << 0,
    Movie = 1u << 1,
    Podcast = 1u << 2,
    Audiobook = 1u << 3,
    MusicVideo = 1u << 5,
    TvShow = 1u << 6,
    Ringtone = 1u << 14,
    Rental = 1u << 15,
    ItunesExtra = 1u << 16,
    Memo = 1u << 20,
    ItunesU = 1u << 21,
    EpubBook = 1u << 22,
    PdfBook = 1u << 23,
};

constexpr Mediatype operator|(Mediatype a, Mediatype b) noexcept {
    return static_cast<Mediatype>(static_cast<std::uint32_t>(a) |
                                  static_cast<std::uint32_t>(b));
}
constexpr Mediatype operator&(Mediatype a, Mediatype b) noexcept {
    return static_cast<Mediatype>(static_cast<std::uint32_t>(a) &
                                  static_cast<std::uint32_t>(b));
}
constexpr bool has_flag(Mediatype value, Mediatype flag) noexcept {
    return (value & flag) == flag;
}

constexpr Mediatype kMediatypeVideoPodcast =
    Mediatype::Movie | Mediatype::Podcast;
constexpr Mediatype kMediatypeMusicTvShow =
    Mediatype::MusicVideo | Mediatype::TvShow;

enum class SmartPlaylistMatch : std::uint32_t { And = 0, Or = 1 };

enum class SmartPlaylistLimitType : std::uint32_t {
    Minutes = 0x01,
    Megabytes = 0x02,
    Songs = 0x03,
    Hours = 0x04,
    Gigabytes = 0x05,
};

enum class SmartPlaylistLimitSort : std::uint32_t {
    Random = 0x02,
    SongName = 0x03,
    Album = 0x04,
    Artist = 0x05,
    Genre = 0x07,
    MostRecentlyAdded = 0x10,
    LeastRecentlyAdded = 0x80000010,
    MostOftenPlayed = 0x14,
    LeastOftenPlayed = 0x80000014,
    MostRecentlyPlayed = 0x15,
    LeastRecentlyPlayed = 0x80000015,
    HighestRating = 0x17,
    LowestRating = 0x80000017,
};

enum class SmartPlaylistFieldType : std::uint8_t {
    String = 1,
    Int,
    Boolean,
    Date,
    Playlist,
    Unknown,
    BinaryAnd,
};

enum class SmartPlaylistActionType : std::uint8_t {
    String = 1,
    Int,
    Date,
    RangeInt,
    RangeDate,
    InTheLast,
    Playlist,
    None,
    Invalid,
    Unknown,
    BinaryAnd,
};

enum class SmartPlaylistAction : std::uint32_t {
    IsInt = 0x00000001,
    IsGreaterThan = 0x00000010,
    IsLessThan = 0x00000040,
    IsInTheRange = 0x00000100,
    IsInTheLast = 0x00000200,
    BinaryAnd = 0x00000400,
    BinaryUnknown1 = 0x00000800,
    IsString = 0x01000001,
    Contains = 0x01000002,
    StartsWith = 0x01000004,
    EndsWith = 0x01000008,
    IsNotInt = 0x02000001,
    IsNotGreaterThan = 0x02000010,
    IsNotLessThan = 0x02000040,
    IsNotInTheRange = 0x02000100,
    IsNotInTheLast = 0x02000200,
    NotBinaryAnd = 0x02000400,
    BinaryUnknown2 = 0x02000800,
    IsNot = 0x03000001,
    DoesNotContain = 0x03000002,
    DoesNotStartWith = 0x03000004,
    DoesNotEndWith = 0x03000008,
};

enum class SmartPlaylistField : std::uint32_t {
    SongName = 0x02,
    Album = 0x03,
    Artist = 0x04,
    Bitrate = 0x05,
    SampleRate = 0x06,
    Year = 0x07,
    Genre = 0x08,
    Kind = 0x09,
    DateModified = 0x0a,
    TrackNumber = 0x0b,
    Size = 0x0c,
    Time = 0x0d,
    Comment = 0x0e,
    DateAdded = 0x10,
    Composer = 0x12,
    PlayCount = 0x16,
    LastPlayed = 0x17,
    DiscNumber = 0x18,
    Rating = 0x19,
    Compilation = 0x1f,
    Bpm = 0x23,
    Grouping = 0x27,
    Playlist = 0x28,
    Purchase = 0x29,
    Description = 0x36,
    Category = 0x37,
    Podcast = 0x39,
    VideoKind = 0x3c,
    TvShow = 0x3e,
    SeasonNr = 0x3f,
    SkipCount = 0x44,
    LastSkipped = 0x45,
    AlbumArtist = 0x47,
    SortSongName = 0x4e,
    SortAlbum = 0x4f,
    SortArtist = 0x50,
    SortAlbumArtist = 0x51,
    SortComposer = 0x52,
    SortTvShow = 0x53,
    AlbumRating = 0x5a,
};

constexpr std::uint32_t kSplStringMaxLen = 255;
constexpr std::uint64_t kSplDateIdentifier = 0x2dae2dae2dae2daeULL;
constexpr std::uint32_t kRatingStep = 20;

struct SmartPlaylistPrefs {
    std::uint8_t live_update = 0;
    std::uint8_t check_rules = 0;
    std::uint8_t check_limits = 0;
    std::uint32_t limit_type = 0;
    std::uint32_t limit_sort = 0;
    std::uint32_t limit_value = 0;
    std::uint8_t match_checked_only = 0;
};

struct SmartPlaylistRule {
    SmartPlaylistField field = SmartPlaylistField::SongName;
    SmartPlaylistAction action = SmartPlaylistAction::IsString;
    std::string string;
    std::uint64_t from_value = 0;
    std::int64_t from_date = 0;
    std::uint64_t from_units = 0;
    std::uint64_t to_value = 0;
    std::int64_t to_date = 0;
    std::uint64_t to_units = 0;
    std::uint32_t unk052 = 0;
    std::uint32_t unk056 = 0;
    std::uint32_t unk060 = 0;
    std::uint32_t unk064 = 0;
    std::uint32_t unk068 = 0;
};

struct SmartPlaylistRules {
    std::uint32_t unk004 = 0;
    SmartPlaylistMatch match_operator = SmartPlaylistMatch::And;
    std::vector<SmartPlaylistRule> rules;
};

struct ArtworkFormat {
    std::int32_t format_id = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
    ThumbFormat format = ThumbFormat::Rgb565LE;
    std::int32_t padding = 0;
    bool crop = false;
    std::int32_t rotation = 0;
    std::array<std::uint8_t, 4> back_color{0, 0, 0, 0};
    std::int32_t display_width = 0;
    bool interlaced = false;
    std::int32_t color_adjustment = 0;
    double gamma = 0.0;
    std::int32_t associated_format = 0;
    std::int32_t row_bytes_alignment = 0;
};

enum class Mhsd5Type : std::uint8_t {
    None = 0,
    Movies = 2,
    TvShows = 3,
    Music = 4,
    Audiobooks = 5,
    Ringtones = 6,
    MovieRentals = 7,
};

enum class DbType : std::uint8_t { Itunes, Photo };

}  // namespace gpod

#endif  // GPOD_TYPES_HPP
