// Independent .cord exports, with tags and pictures: exact FLAC/WAV, or built-in MP3/AAC copies.
// FLAC frames come from the built-in encoder (encode/flac_encoder.h). Every sibling temporary file
// is read back and verified before a no-replace rename publishes it; originals and undo logs aren't used.
#include "convert.h"
#include "cord.h"
#include "decoder.h"
#include "native_audio.h"
#include "streamfile.h"
#include "audio_encode.h"
#include "encode/flac_encoder.h"

#include <algorithm>
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
    head = "fLaC";  // STREAMINFO's frame sizes and MD5 are patched in once the audio is written
    MetadataBlock(head, 0, cordinal::encode::FlacStreamInfo(format.rate, format.channels, format.bits, format.frames, 0, 0, {}));
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
    if (!cordinal::encode::FlacEncoder::Supports(f.rate, f.channels, f.bits, f.frames)) {
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
    constexpr size_t block = cordinal::encode::FlacEncoder::kBlockFrames;
    std::vector<int32_t> pcm(block * f.channels);
    cordinal::encode::FlacEncoder encoder(f.rate, f.channels, f.bits);
    std::string frame;
    while (!cancel) {
        // The encoder needs whole blocks until the last; a reader may return fewer frames at a time.
        size_t n = 0;
        while (n < block) {
            const size_t got = source->Read(pcm.data() + n * f.channels, block - n);
            if (!got) break;
            n += got;
        }
        if (!n) break;
        frame.clear();
        encoder.Frame(pcm.data(), n, frame);
        if (!out.Write(frame)) return false;
        if (n < block) break;
    }
    if (cancel) { error = L"Cancelled."; return false; }
    if (source->Damaged()) { error = L"The .cord audio is damaged. Nothing was exported."; return false; }
    head.replace(8, 34, cordinal::encode::FlacStreamInfo(f.rate, f.channels, f.bits, f.frames, encoder.MinFrameBytes(),
                                                         encoder.MaxFrameBytes(), source->Digest()));
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
