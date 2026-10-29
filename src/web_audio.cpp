// Read-only browser streams. Each response owns its decoder/file and bounded buffers; a range
// request never needs a complete decoded song or a temporary converted file.
#include "web_internal.h"
#include "cue.h"
#include "decoder.h"
#include "peaks.h"
#include "tags.h"
#include "library.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <numeric>

namespace web {
namespace {
struct Source {
    HANDLE h = INVALID_HANDLE_VALUE;
    uint64_t size = 0;
    ~Source() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    bool Open(const std::wstring& path) {
        h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        LARGE_INTEGER n{};
        if (h == INVALID_HANDLE_VALUE || !GetFileSizeEx(h, &n) || n.QuadPart < 0) return false;
        size = (uint64_t)n.QuadPart;
        return true;
    }
    bool Read(uint64_t at, size_t length, char* bytes) {
        LARGE_INTEGER pos{}; pos.QuadPart = (LONGLONG)at;
        DWORD got = 0;
        return SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) &&
            ReadFile(h, bytes, (DWORD)length, &got, nullptr) && got == length;
    }
};
void Put16(char* bytes, uint16_t value) { bytes[0] = (char)value; bytes[1] = (char)(value >> 8); }
void Put32(char* bytes, uint32_t value) { Put16(bytes, (uint16_t)value); Put16(bytes + 2, (uint16_t)(value >> 16)); }
std::string ImageMime(std::string_view bytes) {
    if (bytes.starts_with("\x89PNG\r\n\x1a\n")) return "image/png";
    if (bytes.size() >= 3 && (uint8_t)bytes[0] == 255 && (uint8_t)bytes[1] == 216 && (uint8_t)bytes[2] == 255)
        return "image/jpeg";
    if (bytes.starts_with("GIF87a") || bytes.starts_with("GIF89a")) return "image/gif";
    if (bytes.starts_with("BM")) return "image/bmp";
    if (bytes.size() >= 12 && bytes.starts_with("RIFF") && bytes.substr(8, 4) == "WEBP") return "image/webp";
    return {};
}
}
Media OriginalMedia(const Track& track) {
    Media result;
    if (!track.original || track.mime.empty()) { result.error = "This track needs PCM conversion."; return result; }
    auto file = std::make_shared<Source>();
    if (!file->Open(track.path)) { result.error = "The music file is unavailable."; return result; }
    FILETIME time{};
    if ((track.fileSize && file->size != track.fileSize) ||
        (track.fileTime && GetFileTime(file->h, nullptr, nullptr, &time) &&
         (((uint64_t)time.dwHighDateTime << 32) | time.dwLowDateTime) != track.fileTime)) {
        result.error = "The music file changed. Refresh the library and try again.";
        return result;
    }
    result.size = file->size; result.mime = track.mime;
    result.send = [file](uint64_t at, size_t length, const auto& send) {
        std::array<char, 65536> buffer;
        while (length) {
            size_t n = std::min(length, buffer.size());
            if (!file->Read(at, n, buffer.data()) || !send(buffer.data(), n)) return false;
            at += n; length -= n;
        }
        return true;
    };
    return result;
}
Media WaveMedia(const Track& track, double start, const std::atomic<bool>& cancel) {
    Media result;
    std::wstring error;
    auto decoder = OpenTrackDecoder(track.path, 48000, error);
    if (!decoder) { result.error = "Couldn't decode this track."; return result; }
    return WaveMedia(std::shared_ptr<Decoder>(std::move(decoder)), start, cancel);
}
Media WaveMedia(std::shared_ptr<Decoder> decoder, double start, const std::atomic<bool>& cancel) {
    Media result;
    if (!decoder || cancel) { result.error = "Playback was cancelled."; return result; }
    const double duration = decoder->Duration();
    if (!decoder->Rate() || !std::isfinite(duration) || duration <= 0 || !std::isfinite(start) || start < 0 ||
        duration > (double)INT64_MAX / 48000 || start >= duration) {
        result.error = "The track has no playable range."; return result;
    }
    constexpr unsigned rate = 48000;
    const uint64_t firstFrame = (uint64_t)std::llround(start * rate);
    const uint64_t frames = std::min<uint64_t>(3600ull * rate,
        (uint64_t)std::llround(duration * rate) - firstFrame);
    auto d = std::shared_ptr<Decoder>(std::move(decoder));
    std::string header(44, '\0');
    std::memcpy(header.data(), "RIFF", 4); Put32(header.data() + 4, (uint32_t)(36 + frames * 4));
    std::memcpy(header.data() + 8, "WAVEfmt ", 8); Put32(header.data() + 16, 16);
    Put16(header.data() + 20, 1); Put16(header.data() + 22, 2); Put32(header.data() + 24, rate);
    Put32(header.data() + 28, rate * 4); Put16(header.data() + 32, 4); Put16(header.data() + 34, 16);
    std::memcpy(header.data() + 36, "data", 4); Put32(header.data() + 40, (uint32_t)(frames * 4));
    result.size = header.size() + frames * 4; result.mime = "audio/wav";
    result.send = [d, header, firstFrame, &cancel](uint64_t at, size_t length, const auto& send) {
        if (at < header.size()) {
            size_t n = std::min(length, header.size() - (size_t)at);
            if (!send(header.data() + at, n)) return false;
            at += n; length -= n;
        }
        if (!length) return true;
        uint64_t byte = at - header.size();
        uint64_t target = firstFrame + byte / 4;
        size_t trim = (size_t)(byte % 4);
        // Align the warm-up to a rational resampling phase. Every byte range then reproduces
        // the same samples as decoding from the beginning, including at unusual source rates.
        unsigned sourceRate = d->Rate(), step = sourceRate / std::gcd(sourceRate, 48000u);
        uint64_t sourceFrame = target * sourceRate / 48000;
        uint64_t base = sourceFrame > 64 ? (sourceFrame - 64) / step * step : 0;
        if (cancel || (base && !d->Seek((double)base / sourceRate))) return false;
        uint64_t skip = target - base * 48000 / sourceRate;
        Resampler resampler; resampler.Init(sourceRate, 48000);
        std::array<float, 4096 * 2> input;
        std::vector<float> output;
        std::vector<char> pcm;
        bool flushed = false;
        while (length && !cancel) {
            size_t n = d->Read(input.data(), input.size() / 2);
            if (n) { output.clear(); resampler.Push(input.data(), n, output); }
            else {
                if (flushed) return false;
                output.clear(); resampler.Flush(output); flushed = true;
            }
            size_t count = output.size() / 2;
            size_t omit = (size_t)std::min<uint64_t>(skip, count);
            skip -= omit;
            if (omit == count) continue;
            pcm.resize((count - omit) * 4);
            for (size_t i = omit * 2; i < output.size(); ++i) {
                float f = output[i]; if (!std::isfinite(f)) f = 0;
                int sample = (int)std::lround(std::clamp(f, -1.f, 1.f) * 32768.f);
                Put16(pcm.data() + (i - omit * 2) * 2, (uint16_t)std::clamp(sample, -32768, 32767));
            }
            size_t bytes = std::min(length, pcm.size() - trim);
            if (!send(pcm.data() + trim, bytes)) return false;
            trim = 0; length -= bytes;
        }
        return length == 0;
    };
    return result;
}
std::string Artwork(const Track& track, std::string& mime) {
    std::wstring path = AudioFileOf(track.path);
    std::string bytes;
    ReadEmbeddedArt(path, bytes);
    mime = ImageMime(bytes);
    if (!mime.empty() && bytes.size() <= 16u * 1024 * 1024) return bytes;
    for (const wchar_t* name : {L"cover.jpg",L"folder.jpg",L"front.jpg",L"cover.png",L"folder.png",L"front.png",
                                L"Cover.jpg",L"Folder.jpg",L"Front.jpg",L"Cover.png",L"Folder.png",L"Front.png"}) {
        Source file;
        if (!file.Open(DirectoryOf(path) + kPathSep + name) || file.size > 16u * 1024 * 1024) continue;
        bytes.resize((size_t)file.size);
        if (file.Read(0, bytes.size(), bytes.data()) && !(mime = ImageMime(bytes)).empty()) return bytes;
    }
    mime.clear();
    return {};
}
std::string Waveform(const Track& track, const std::atomic<bool>& cancel) {
    std::wstring error;
    auto decoder = OpenTrackDecoder(track.path, 48000, error);
    if (!decoder || !decoder->Rate()) return {};
    PeakAccumulator accumulator;
    accumulator.Begin((uint64_t)std::llround(decoder->Duration() * decoder->Rate()));
    std::array<float, 8192> samples;
    while (!cancel) {
        size_t frames = decoder->Read(samples.data(), samples.size() / 2);
        if (!frames) break;
        accumulator.Add(samples.data(), frames);
    }
    Peaks p{};
    if (cancel || !accumulator.Finish(p)) return {};
    std::string result = "[";
    for (size_t i = 0; i < kPeakBuckets; ++i) {
        if (i) result += ',';
        result += std::to_string(p.v[i]);
    }
    return result + ']';
}
}  // namespace web
