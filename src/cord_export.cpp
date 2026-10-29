// Independent .cord exports, with tags and pictures: exact FLAC/WAV, or built-in MP3/AAC copies.
// Fixed FLAC predictors keep the encoder small (RFC 9639). Every sibling temporary file is read
// back and verified before a no-replace rename publishes it; originals and undo logs aren't used.
#include "convert.h"
#include "cord.h"
#include "decoder.h"
#include "native_audio.h"
#include "streamfile.h"
#include "audio_encode.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <clocale>
#include <cmath>
#include <cstdio>
#ifndef _WIN32
#include <unistd.h>
#endif

namespace {

void Be(std::string& out, uint64_t value, unsigned bytes) {
    while (bytes) out += (char)(value >> (--bytes * 8));
}

bool MetadataBlock(std::string& out, unsigned type, std::string_view body) {
    if (body.size() > 0xFFFFFF) return false;
    out += (char)type;
    Be(out, body.size(), 3);
    out += body;
    return true;
}

// Keep repeated and free-form fields as separate comments. Unknown typed fields fail explicitly:
// a future extension must never appear to export successfully while silently losing its tags.
bool Comments(std::string_view meta, std::vector<std::string>& comments) {
    static constexpr const char* text[] = {"TITLE", "ARTIST", "ALBUM", "ALBUMARTIST", "GENRE", "DATE",
                                           "COMPOSER", "COMMENT", "GROUPING", "LABEL", "LYRICS"};
    static constexpr const char* numbers[] = {"TRACKNUMBER", "TRACKTOTAL", "DISCNUMBER", "DISCTOTAL", "BPM"};
    static constexpr const char* gains[] = {"REPLAYGAIN_TRACK_GAIN", "REPLAYGAIN_TRACK_PEAK",
                                            "REPLAYGAIN_ALBUM_GAIN", "REPLAYGAIN_ALBUM_PEAK"};
    for (size_t at = 0; at < meta.size();) {
        cord::Entry e;
        if (!cord::NextEntry(meta, at, e)) return false;
        std::string key, value(e.value);
        if (e.id == cord::kFreeForm) key = e.key;
        else if (e.id >= cord::kTitle && e.id <= cord::kLyrics) key = text[e.id - cord::kTitle];
        else if (e.id >= cord::kTrack && e.id <= cord::kBpm && e.value.size() == 4) {
            key = numbers[e.id - cord::kTrack];
            value = std::to_string(cord::Le32((const uint8_t*)e.value.data()));
        } else if (e.id == cord::kCompilation && e.value == std::string_view("\1", 1)) {
            key = "COMPILATION";
            value = "1";
        } else if (e.id >= cord::kTrackGain && e.id <= cord::kAlbumPeak && e.value.size() == 4) {
            key = gains[e.id - cord::kTrackGain];
            float gain;
            memcpy(&gain, e.value.data(), 4);
            if (!std::isfinite(gain)) return false;
            char buffer[64];
            const int n = std::snprintf(buffer, sizeof buffer, "%.9g", (double)gain);
            if (n <= 0 || n >= sizeof buffer) return false;
            value.assign(buffer, (size_t)n);
            // Nine significant digits round-trip a stored float. Vorbis comments always use a
            // dot, even when the UI's locale uses a comma or a multibyte decimal separator.
            const std::string_view decimal = localeconv()->decimal_point;
            if (!decimal.empty() && decimal != ".")
                if (const size_t dot = value.find(decimal); dot != value.npos) value.replace(dot, decimal.size(), ".");
            if (e.id == cord::kTrackGain || e.id == cord::kAlbumGain) value += " dB";
        } else return false;
        if (key.empty() || std::any_of(key.begin(), key.end(), [](unsigned char c) { return c < 32 || c > 125 || c == '='; }))
            return false;
        comments.push_back(key + "=" + value);
    }
    return true;
}

struct ExportTags {
    std::vector<std::string> comments, pictures;
};

bool Metadata(FileSource& input, const PcmFormat& format, std::string& head, ExportTags* tags = nullptr) {
    std::string bytes;
    if (!input.Read(0, cord::kHeaderBytes, bytes) || !cord::IsHeader((const uint8_t*)bytes.data())) return false;
    std::vector<std::string> comments, pictures;
    for (uint64_t at = cord::kHeaderBytes; at < input.Size();) {
        if (!input.Read(at, cord::kChunkHeaderBytes, bytes)) return false;
        const uint32_t id = cord::Le32((const uint8_t*)bytes.data());
        const uint64_t size = cord::Le64((const uint8_t*)bytes.data() + 4);
        at += cord::kChunkHeaderBytes;
        if (size > input.Size() - at) return false;
        if (id == cord::kMeta || id == cord::kPicture) {
            if (size > 0xFFFFFF || !input.Read(at, (size_t)size, bytes)) return false;
            if (id == cord::kMeta) {
                if (!Comments(bytes, comments)) return false;
            } else {
                uint8_t type;
                std::string_view image;
                if (!cord::ParsePicture(bytes, type, image)) return false;
                const size_t mime = (uint8_t)bytes[1], describedAt = 2 + mime;
                const size_t described = cord::Le32((const uint8_t*)bytes.data() + describedAt);
                std::string picture;
                Be(picture, type, 4);
                Be(picture, mime, 4);
                picture.append(bytes, 2, mime);
                Be(picture, described, 4);
                picture.append(bytes, describedAt + 4, described);
                picture.append(16, '\0');  // dimensions and colour depth unspecified in .cord
                Be(picture, image.size(), 4);
                picture += image;
                pictures.push_back(std::move(picture));
                if (tags) tags->pictures.push_back(bytes);
            }
        }
        at += size;
    }
    if (tags) { tags->comments = std::move(comments); return true; }
    if (format.channelMask) {
        // Speaker assignments are independent of the channel count. This standard FLAC comment
        // carries WAVE's channel order without rearranging any samples.
        char mask[16];
        const auto result = std::to_chars(mask, mask + sizeof mask, format.channelMask, 16);
        comments.push_back("WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x" + std::string(mask, result.ptr));
    }
    std::string info;
    Be(info, 4096, 2);
    Be(info, 4096, 2);  // the final block may be shorter, even shorter than 16 frames
    info.append(6, '\0');  // frame byte sizes unknown
    Be(info, (uint64_t)format.rate << 44 | (uint64_t)(format.channels - 1) << 41 |
             (uint64_t)(format.bits - 1) << 36 | format.frames, 8);
    info.append(16, '\0');  // patched with the source's verified digest
    head = "fLaC";
    MetadataBlock(head, 0, info);
    std::string vorbis;
    cord::PutLe(vorbis, 8, 4);
    vorbis += "cordinal";
    cord::PutLe(vorbis, comments.size(), 4);
    for (const auto& c : comments) {
        cord::PutLe(vorbis, c.size(), 4);
        vorbis += c;
    }
    if (!MetadataBlock(head, pictures.empty() ? 0x84 : 4, vorbis)) return false;
    for (size_t i = 0; i < pictures.size(); ++i)
        if (!MetadataBlock(head, i + 1 == pictures.size() ? 0x86 : 6, pictures[i])) return false;
    return true;
}

std::string Integer(uint64_t value, unsigned bytes, bool syncsafe = false) {
    std::string out(bytes, '\0');
    while (bytes) { out[--bytes] = (char)(value & (syncsafe ? 127 : 255)); value >>= syncsafe ? 7 : 8; }
    return out;
}

std::string Box(std::string_view name, std::string_view bytes) {
    return Integer(bytes.size() + 8, 4) + std::string(name) + std::string(bytes);
}

// A field's normal ID3 / MP4 name; unknown keys remain user-text / freeform fields. Repeated
// values stay separate, and ID3v2.4 keeps complete UTF-8 dates rather than reducing them to a year.
struct Field { const char *key, *id3, *mp4; };
constexpr Field kFields[] = {
    {"TITLE", "TIT2", "\xA9nam"}, {"ARTIST", "TPE1", "\xA9" "ART"}, {"ALBUM", "TALB", "\xA9" "alb"},
    {"ALBUMARTIST", "TPE2", "aART"}, {"GENRE", "TCON", "\xA9gen"}, {"DATE", "TDRC", "\xA9" "day"},
    {"COMPOSER", "TCOM", "\xA9wrt"}, {"GROUPING", "TIT1", "\xA9grp"}, {"LABEL", "TPUB", ""},
    {"COMMENT", "COMM", "\xA9" "cmt"}, {"LYRICS", "USLT", "\xA9lyr"},
    {"TRACKNUMBER", "TRCK", "trkn"}, {"DISCNUMBER", "TPOS", "disk"}, {"BPM", "TBPM", "tmpo"}, {"COMPILATION", "TCMP", "cpil"},
};

std::string Tags(const ExportTags& tags, bool mp4, unsigned sourceRate) {
    std::string body;
    auto data = [](unsigned type, const std::string& text) { return Box("data", Integer(type, 4) + std::string(4, '\0') + text); };
    auto frame = [&](std::string_view name, const std::string& text) {
        body += std::string(name) + Integer(text.size(), 4, true) + std::string(2, '\0') + text;
    };
    auto valueOf = [&](std::string_view key) {
        const std::string prefix = std::string(key) + "=";
        for (const auto& c : tags.comments) if (c.starts_with(prefix)) return c.substr(prefix.size());
        return std::string();
    };
    for (const auto& comment : tags.comments) {
        const size_t equal = comment.find('=');
        const std::string key = comment.substr(0, equal);
        std::string value = comment.substr(equal + 1);
        if (sourceRate && key == "WAVEFORMATEXTENSIBLE_CHANNEL_MASK") continue;  // lossy copies are stereo
        if (sourceRate && (key == "LOOPSTART" || key == "LOOPEND" || key == "LOOPLENGTH")) {
            uint64_t n = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), n);
            if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size()) {
                const long double scaled = std::round((long double)n * 48000 / sourceRate);
                if (scaled < (long double)UINT64_MAX) value = std::to_string((uint64_t)scaled);
            }
        }
        const Field* field = nullptr;
        for (const auto& f : kFields) if (key == f.key) { field = &f; break; }
        if (!mp4) {
            if (field) {
                if (key == "TRACKNUMBER" || key == "DISCNUMBER") {
                    const auto total = valueOf(key == "TRACKNUMBER" ? "TRACKTOTAL" : "DISCTOTAL");
                    if (!total.empty()) value += "/" + total;
                }
                frame(field->id3, std::string(1, '\3') +
                      ((key == "COMMENT" || key == "LYRICS") ? std::string("eng\0", 4) : std::string()) + value);
            } else frame("TXXX", std::string(1, '\3') + key + '\0' + value);
        } else {
            std::string type = field ? field->mp4 : "";
            unsigned kind = 1;
            std::string stored = value;
            if (type == "trkn" || type == "disk") {
                const auto total = valueOf(type == "trkn" ? "TRACKTOTAL" : "DISCTOTAL");
                uint64_t n = 0, t = 0;
                const auto a = std::from_chars(value.data(), value.data() + value.size(), n);
                const auto b = std::from_chars(total.data(), total.data() + total.size(), t);
                if (a.ec != std::errc{} || a.ptr != value.data() + value.size() || n > 65535 ||
                    (!total.empty() && (b.ec != std::errc{} || b.ptr != total.data() + total.size() || t > 65535))) type.clear();
                else { kind = 0; stored = std::string(2, '\0') + Integer(n, 2) + Integer(t, 2) + (type == "trkn" ? std::string(2, '\0') : ""); }
            } else if (type == "tmpo" || type == "cpil") {
                uint64_t n = 0;
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), n);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || n > 65535) type.clear();
                else { kind = 21; stored = Integer(n, type == "cpil" ? 1 : 2); }
            }
            if (type.empty())
                body += Box("----", Box("mean", std::string(4, '\0') + "com.apple.iTunes") +
                                      Box("name", std::string(4, '\0') + key) + data(1, value));
            else body += Box(type, data(kind, stored));
        }
    }
    for (const auto& picture : tags.pictures) {
        uint8_t type = 0;
        std::string_view image;
        cord::ParsePicture(picture, type, image);  // Metadata validated it before keeping it
        const size_t mime = (uint8_t)picture[1], at = 2 + mime;
        const size_t described = cord::Le32((const uint8_t*)picture.data() + at);
        if (mp4) body += Box("covr", data(image.starts_with("\x89PNG") ? 14 : 13, std::string(image)));
        else frame("APIC", std::string(1, '\3') + picture.substr(2, mime) + '\0' + (char)type +
                           picture.substr(at + 4, described) + '\0' + std::string(image));
    }
    if (body.size() > 0xFFFFFF) return {};
    return mp4 ? body : std::string("ID3\4\0\0", 6) + Integer(body.size(), 4, true) + body;
}

std::string WaveHeader(const PcmFormat& f, const std::string& tags) {
    const unsigned bytes = (f.bits + 7) / 8, align = bytes * f.channels;
    const uint64_t audio = f.frames * align;
    const uint64_t riffBytes = 60 + 8 + tags.size() + (tags.size() & 1) + 8 + audio + (audio & 1);
    const bool rf64 = riffBytes - 8 > UINT32_MAX;
    std::string head = std::string(rf64 ? "RF64" : "RIFF", 4);
    cord::PutLe(head, rf64 ? UINT32_MAX : riffBytes - 8, 4);
    head += "WAVE";
    if (rf64) {
        head += "ds64";
        cord::PutLe(head, 28, 4);
        cord::PutLe(head, riffBytes + 36 - 8, 8);
        cord::PutLe(head, audio, 8);
        cord::PutLe(head, f.frames, 8);
        cord::PutLe(head, 0, 4);
    }
    head += "fmt ";
    cord::PutLe(head, 40, 4);
    cord::PutLe(head, 0xFFFE, 2);
    cord::PutLe(head, f.channels, 2);
    cord::PutLe(head, f.rate, 4);
    cord::PutLe(head, (uint64_t)f.rate * align, 4);
    cord::PutLe(head, align, 2);
    cord::PutLe(head, bytes * 8, 2);
    cord::PutLe(head, 22, 2);
    cord::PutLe(head, f.bits, 2);
    cord::PutLe(head, f.channelMask, 4);
    head += std::string("\1\0\0\0\0\0\x10\0\x80\0\0\xAA\0\x38\x9B\x71", 16);
    head += "id3 ";
    cord::PutLe(head, tags.size(), 4);
    head += tags;
    if (tags.size() & 1) head += '\0';
    head += "data";
    cord::PutLe(head, rf64 ? UINT32_MAX : audio, 4);
    return head;
}

struct Bits {
    std::string bytes;
    unsigned used = 0;
    void Put(uint32_t value, unsigned count) {
        while (count) {
            if (!used) bytes += '\0';
            const unsigned n = std::min(count, 8 - used);
            bytes.back() = (char)((uint8_t)bytes.back() | ((value >> (count - n)) & ((1u << n) - 1)) << (8 - used - n));
            count -= n;
            used = (used + n) % 8;
        }
    }
    void Rice(uint32_t folded, unsigned k) {
        uint32_t zeros = folded >> k;
        while (zeros >= 32) { Put(0, 32); zeros -= 32; }
        Put(1, zeros + 1);
        Put(folded, k);
    }
};

uint16_t Crc(std::string_view bytes, unsigned width, unsigned polynomial) {
    unsigned crc = 0;
    for (unsigned char c : bytes) {
        crc ^= (unsigned)c << (width - 8);
        for (unsigned i = 0; i < 8; ++i) crc = (crc << 1) ^ ((crc & (1u << (width - 1))) ? polynomial : 0);
    }
    return (uint16_t)(crc & ((1u << width) - 1));
}

int64_t Prediction(const std::vector<int32_t>& x, size_t i, unsigned order) {
    switch (order) {
        case 1: return x[i - 1];
        case 2: return 2ll * x[i - 1] - x[i - 2];
        case 3: return 3ll * x[i - 1] - 3ll * x[i - 2] + x[i - 3];
        case 4: return 4ll * x[i - 1] - 6ll * x[i - 2] + 4ll * x[i - 3] - x[i - 4];
        default: return 0;
    }
}

void Subframe(Bits& out, const std::vector<int32_t>& x, unsigned bits) {
    if (std::all_of(x.begin(), x.end(), [&](int32_t s) { return s == x[0]; })) {
        out.Put(0, 8);
        out.Put((uint32_t)x[0], bits);
        return;
    }
    uint64_t best = x.size() * bits;
    unsigned order = 5, rice = 0;
    std::vector<uint32_t> residual(x.size()), chosen;
    for (unsigned o = 0; o <= 4 && o < x.size(); ++o) {
        uint64_t sum = 0;
        bool fits = true;
        for (size_t i = o; i < x.size(); ++i) {
            const int64_t r = (int64_t)x[i] - Prediction(x, i, o);
            if (r < -INT32_MAX || r > INT32_MAX) { fits = false; break; }
            residual[i] = (uint32_t)(r < 0 ? -2 * r - 1 : 2 * r);
            sum += residual[i];
        }
        if (!fits) continue;
        const unsigned k = std::min(14u, (unsigned)std::bit_width(sum / (x.size() - o)));
        for (unsigned p = k ? k - 1 : 0; p <= k; ++p) {
            uint64_t cost = o * bits + 10 + (x.size() - o) * (p + 1);
            for (size_t i = o; i < x.size(); ++i) cost += residual[i] >> p;
            if (cost < best) { best = cost; order = o; rice = p; chosen = residual; }
        }
    }
    if (order == 5) {
        out.Put(2, 8);  // verbatim when prediction cannot save space or fit signed residuals
        for (int32_t s : x) out.Put((uint32_t)s, bits);
    } else {
        out.Put((8 + order) << 1, 8);
        for (unsigned i = 0; i < order; ++i) out.Put((uint32_t)x[i], bits);
        out.Put(0, 6);  // Rice method 0, one partition
        out.Put(rice, 4);
        for (size_t i = order; i < x.size(); ++i) out.Rice(chosen[i], rice);
    }
}

std::string Frame(const int32_t* pcm, size_t count, const PcmFormat& f, uint32_t number) {
    // Common rates and depths are stated in the frame as well as STREAMINFO for hardware players.
    constexpr unsigned rates[] = {0,88200,176400,192000,8000,16000,22050,24000,32000,44100,48000,96000};
    constexpr unsigned depths[] = {0,8,12,0,16,20,24,32};
    unsigned rate = 0, depth = 0;
    for (unsigned i = 1; i < std::size(rates); ++i) if (rates[i] == f.rate) rate = i;
    for (unsigned i = 1; i < std::size(depths); ++i) if (depths[i] == f.bits) depth = i;
    if (!rate && f.rate <= 65535) rate = 13;
    else if (!rate && f.rate % 10 == 0 && f.rate <= 655350) rate = 14;
    Bits out;
    out.bytes = std::string("\xFF\xF8", 2) + (char)(0x70 | rate) + (char)((f.channels - 1) << 4 | depth << 1);
    if (number < 128) out.bytes += (char)number;
    else {
        unsigned bytes = 2;
        while (number >= (1ull << (5 * bytes + 1))) ++bytes;
        out.bytes += (char)((0xFFu << (8 - bytes)) | (number >> ((bytes - 1) * 6)));
        while (--bytes) out.bytes += (char)(0x80 | ((number >> ((bytes - 1) * 6)) & 63));
    }
    Be(out.bytes, count - 1, 2);
    if (rate >= 13) Be(out.bytes, f.rate / (rate == 14 ? 10 : 1), 2);
    out.bytes += (char)Crc(out.bytes, 8, 0x07);
    std::vector<int32_t> channel(count);
    for (unsigned c = 0; c < f.channels; ++c) {
        for (size_t i = 0; i < count; ++i) channel[i] = pcm[i * f.channels + c];
        Subframe(out, channel, f.bits);
    }
    Be(out.bytes, Crc(out.bytes, 16, 0x8005), 2);  // Bits already zero-pads the final byte
    return std::move(out.bytes);
}

class Output {
public:
    explicit Output(const std::wstring& path) {
        static std::atomic<unsigned> serial{0};
        const std::wstring temp = path + L".cordinal-tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                  std::to_wstring(++serial) + L"." + ExtOf(path);
        handle_ = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ != INVALID_HANDLE_VALUE) path_ = temp;
    }
    ~Output() {
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
        if (!path_.empty()) DeleteFileW(path_.c_str());
    }
    bool Write(std::string_view bytes) {
        DWORD written = 0;
        return handle_ != INVALID_HANDLE_VALUE && WriteFile(handle_, bytes.data(), (DWORD)bytes.size(), &written, nullptr) &&
               written == bytes.size();
    }
    bool Finish(const std::string& head) {
        LARGE_INTEGER zero{};
        return SetFilePointerEx(handle_, zero, nullptr, FILE_BEGIN) && Write(head) && Close();
    }
    bool Close() {
        if (handle_ == INVALID_HANDLE_VALUE) return false;
        const bool ok = FlushFileBuffers(handle_) != FALSE;
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
        return ok;
    }
    const std::wstring& Path() const { return path_; }
    bool Commit(const std::wstring& path) {
#ifdef _WIN32
        if (!MoveFileExW(path_.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH)) return false;
#else
        // The compatibility layer's rename checks for collisions before renaming. A hard link
        // publishes atomically with exclusive creation, including a destination created meanwhile.
        if (link(ToUtf8(path_).c_str(), ToUtf8(path).c_str()) != 0) return false;
        DeleteFileW(path_.c_str());
#endif
        path_.clear();
        return true;
    }
private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    std::wstring path_;
};

}  // namespace

bool ExportCordFlac(const std::wstring& from, const std::wstring& to, std::wstring& error, const std::atomic<bool>& cancel) {
    error.clear();
    FileSource input(from);  // pin the source against tag edits or replacement until verification ends
    auto source = OpenCordExact(from);
    if (!input.Valid() || !source) { error = L"Couldn't read this .cord file."; return false; }
    const PcmFormat& f = source->Format();
    if (f.bits < 4 || f.frames >= (1ull << 36)) {
        error = L"FLAC needs 4- to 32-bit samples and fewer than 68,719,476,736 frames.";
        return false;
    }
    if (GetFileAttributesW(to.c_str()) != INVALID_FILE_ATTRIBUTES) {
        error = L"The destination already exists; choose another name or move it first.";
        return false;
    }
    std::string head;
    if (!Metadata(input, f, head)) {
        error = L"The .cord tags are damaged, unsupported, or too large for FLAC. Nothing was exported.";
        return false;
    }
    Output out(to);
    error = L"Couldn't write the FLAC file next to its source. Check free space and folder permissions.";
    if (!out.Write(head)) return false;
    std::vector<int32_t> pcm(4096 * f.channels);
    uint32_t number = 0;
    while (!cancel) {
        const size_t n = source->Read(pcm.data(), 4096);
        if (!n) break;
        if (!out.Write(Frame(pcm.data(), n, f, number++))) return false;
    }
    if (cancel) { error = L"Cancelled."; return false; }
    if (source->Damaged()) { error = L"The .cord audio is damaged. Nothing was exported."; return false; }
    head.replace(26, 16, (const char*)source->Digest().data(), 16);
    if (!out.Finish(head)) return false;
    bool same = false;
    if (auto back = OpenFlacExact(out.Path())) {
        while (!cancel && back->Read(pcm.data(), 4096)) {}
        const PcmFormat& b = back->Format();
        same = !cancel && !back->Damaged() && back->Digest() == source->Digest() && b.frames == f.frames &&
               b.bits == f.bits && b.channels == f.channels && b.rate == f.rate;
    }
    // Audio and metadata are checked separately: a PCM hash says nothing about the tags or covers.
    FileSource check(out.Path());
    std::string bytes;
    for (size_t at = 0; same && at < head.size();) {
        const size_t n = std::min<size_t>(65536, head.size() - at);
        same = !cancel && check.Read(at, n, bytes) && bytes == std::string_view(head).substr(at, n);
        at += n;
    }
    check.Close();
    if (!same || cancel) {
        error = cancel ? L"Cancelled." : L"The FLAC audio or tags didn't verify. Nothing was exported.";
        return false;
    }
    if (!out.Commit(to)) { error = L"Couldn't publish the FLAC file; existing files are never overwritten."; return false; }
    error.clear();
    return true;
}

bool ExportCordAudio(const std::wstring& from, const std::wstring& to, CordExport format, std::wstring& error,
                     const std::atomic<bool>& cancel) {
    if (format == CordExport::Flac) return ExportCordFlac(from, to, error, cancel);
    error.clear();
    if (format <= CordExport::None || format >= CordExport::Count) { error = L"Choose an export format."; return false; }
    const bool wave = format == CordExport::Wav;
    if (cancel) { error = L"Cancelled."; return false; }
    FileSource input(from);
    auto source = OpenCordExact(from);
    if (!input.Valid() || !source) { error = L"Couldn't read this .cord file."; return false; }
    const PcmFormat& f = source->Format();
    if (GetFileAttributesW(to.c_str()) != INVALID_FILE_ATTRIBUTES) {
        error = L"The destination already exists; move it first or choose another folder.";
        return false;
    }
    ExportTags tags;
    std::string unused;
    if (!Metadata(input, f, unused, &tags)) { error = L"The .cord tags are damaged or unsupported."; return false; }
    const bool aac = format == CordExport::Aac128 || format == CordExport::Aac192;
    const std::string metadata = Tags(tags, aac, wave ? 0 : f.rate);
    if (metadata.empty() && (!tags.comments.empty() || !tags.pictures.empty())) {
        error = L"The tags are too large for this export format.";
        return false;
    }
    if (f.frames > (UINT64_MAX - metadata.size() - 256) / (4 * f.channels)) {
        error = L"This file is too long to export.";
        return false;
    }
    Output out(to);
    std::vector<int32_t> pcm(4096 * f.channels);
    std::string head = wave ? WaveHeader(f, metadata) : aac ? std::string() : metadata;
    error = L"Couldn't write the export. Check free space and folder permissions.";
    if (!out.Write(head)) return false;
    const unsigned bytes = (f.bits + 7) / 8;
    std::string block;
    while (!cancel) {
        const size_t n = source->Read(pcm.data(), 4096);
        if (!n) break;
        if (wave) {
            block.clear();
            for (size_t i = 0; i < n * f.channels; ++i) {
                const uint32_t value = ((uint32_t)pcm[i] << (bytes * 8 - f.bits)) + (bytes == 1 ? 128u : 0u);
                cord::PutLe(block, value, bytes);
            }
            if (!out.Write(block)) return false;
        }
    }
    if (cancel || source->Damaged()) {
        error = cancel ? L"Cancelled." : L"The .cord audio is damaged. Nothing was exported.";
        return false;
    }
    error.clear();
    bool finished = false;
    if (wave) finished = (!((f.frames * f.channels * bytes) & 1) || out.Write(std::string_view("\0", 1))) && out.Close();
    else if (aac) {
        finished = out.Close() && EncodeM4a(from, out.Path(), kCordExports[(unsigned)format].kbps, cancel, error, metadata);
    } else {
        uint64_t frames = 0;
        finished = EncodeLossyAudio(from, kCordExports[(unsigned)format].kbps, true, cancel,
                                    [&](const char* p, size_t n) { return out.Write({p, n}); }, frames, error) && out.Close();
    }
    if (!finished) {
        if (error.empty()) error = L"Couldn't finish writing the export. Check free space and folder permissions.";
        return false;
    }
    bool same = false;
    if (wave) {
        if (auto back = OpenPcmExact(out.Path())) {
            while (!cancel && back->Read(pcm.data(), 4096)) {}
            const PcmFormat& b = back->Format();
            same = !back->Damaged() && back->Digest() == source->Digest() && b.frames == f.frames &&
                   b.rate == f.rate && b.bits == f.bits && b.channels == f.channels && b.channelMask == f.channelMask;
        }
    } else if (auto back = OpenDecoder(out.Path(), 48000, error)) {
        std::vector<float> decoded(4096 * 2);
        uint64_t frames = 0;
        bool finite = true;
        while (!cancel) {
            const size_t n = back->Read(decoded.data(), 4096);
            if (!n) break;
            frames += n;
            for (size_t i = 0; i < n * 2; ++i) finite &= std::isfinite(decoded[i]);
        }
        const double expected = (double)f.frames / f.rate;
        const double duration = (double)frames / back->Rate();
        same = finite && frames && !back->Failed() && back->Rate() == 48000 &&
               (aac ? std::fabs(duration - expected) <= 1.0 / 48000 : duration >= expected && duration - expected <= 0.15);
    }
    FileSource check(out.Path());
    if (aac && same && !metadata.empty()) {
        same = false;
        // The M4A writer puts all export items together in moov's ilst, ahead of iTunSMPB.
        for (uint64_t at = 0; check.Size() - at >= 8;) {
            std::string atom;
            if (!check.Read(at, 8, atom)) break;
            const auto* p = (const uint8_t*)atom.data();
            const uint64_t n = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
            if (n < 8 || n > check.Size() - at) break;
            if (atom.substr(4) == "moov") {
                same = n <= (64u << 20) && check.Read(at, (size_t)n, atom) && atom.find(metadata) != atom.npos;
                break;
            }
            at += n;
        }
    } else {
        for (size_t at = 0; same && at < head.size();) {
            const size_t n = std::min<size_t>(65536, head.size() - at);
            same = check.Read(at, n, block) && block == std::string_view(head).substr(at, n);
            at += n;
        }
    }
    check.Close();
    if (!same || cancel) {
        error = cancel ? L"Cancelled." : L"The exported audio or tags didn't verify. Nothing was exported.";
        return false;
    }
    if (!out.Commit(to)) { error = L"Couldn't publish the export; existing files are never overwritten."; return false; }
    error.clear();
    return true;
}
