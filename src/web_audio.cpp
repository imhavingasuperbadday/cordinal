// Read-only browser streams. Each response owns its decoder/file and bounded buffers; a range
// request never needs a complete decoded song or a temporary converted file.
#include "web_internal.h"
#include "cue.h"
#include "customcover.h"
#include "decoder.h"
#include "inflate.h"
#include "peaks.h"
#include "tags.h"
#include "library.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <numeric>
#include <set>

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
namespace {
// Opens the track's own file, refusing one that changed since the snapshot (its key would now name
// different bytes). Empty with `error` set when it can't be served.
std::shared_ptr<Source> OpenTrackFile(const Track& track, std::string& error) {
    if (IsSubtrack(track.path)) { error = "This song is part of a larger file, so it has no file of its own."; return {}; }
    auto file = std::make_shared<Source>();
    if (!file->Open(track.path)) { error = "The music file is unavailable."; return {}; }
    FILETIME time{};
    if ((track.fileSize && file->size != track.fileSize) ||
        (track.fileTime && GetFileTime(file->h, nullptr, nullptr, &time) &&
         (((uint64_t)time.dwHighDateTime << 32) | time.dwLowDateTime) != track.fileTime)) {
        error = "The music file changed. Refresh the library and try again.";
        return {};
    }
    return file;
}
}
Media OriginalMedia(const Track& track) {
    if (!track.original || track.mime.empty()) { Media result; result.error = "This track needs PCM conversion."; return result; }
    return FileMedia(track);
}
Media FileMedia(const Track& track) {
    Media result;
    auto file = OpenTrackFile(track, result.error);
    if (!file) return result;
    result.size = file->size; result.mime = track.mime.empty() ? "application/octet-stream" : track.mime;
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
// A file name the phone can keep: no path separators, reserved or control characters, at most
// about 120 bytes without cutting a UTF-8 sequence, and never empty.
std::string SafeFileName(std::string name) {
    for (char& c : name)
        if ((unsigned char)c < 32 || strchr("\\/:*?\"<>|", c)) c = '_';
    if (name.size() > 120) {
        size_t cut = 120;
        while (cut && ((unsigned char)name[cut] & 0xC0) == 0x80) --cut;
        name.resize(cut);
    }
    while (!name.empty() && (name.back() == ' ' || name.back() == '.')) name.pop_back();
    while (!name.empty() && (name.front() == ' ' || name.front() == '.')) name.erase(0, 1);
    return name.empty() ? std::string("track") : name;
}
namespace {
std::string Extension(const Track& track) {
    std::wstring file = AudioFileOf(track.path);
    size_t dot = file.find_last_of(L'.'), slash = file.find_last_of(L"\\/");
    if (dot == file.npos || (slash != file.npos && dot < slash)) return {};
    return ToUtf8(file.substr(dot));
}
}
std::string DownloadName(const Track& track, unsigned number) {
    std::string name = track.title.empty() ? ToUtf8(FileNameOf(AudioFileOf(track.path))) : track.title;
    if (!track.title.empty() && !track.artist.empty() && !number) name = track.artist + " - " + name;
    if (number) {
        char prefix[16];
        snprintf(prefix, sizeof(prefix), "%02u ", number);
        name = prefix + name;
    }
    std::string extension = track.title.empty() ? std::string() : Extension(track);
    return SafeFileName(std::move(name)) + extension;  // it cuts long names without splitting a character
}
// A stored (uncompressed) ZIP built while it is sent: music is compressed already, and the size is
// known before the first byte, so the phone shows real progress. Each entry's CRC follows its data
// (general purpose bit 3), computed on the way. Only a whole-file request is served.
Media ZipMedia(const std::vector<const Track*>& tracks, const std::string& folder, bool numbered) {
    struct Entry {
        const Track* track;
        std::string name;
        uint64_t size = 0, offset = 0;
        uint32_t crc = 0;
    };
    Media result;
    auto entries = std::make_shared<std::vector<Entry>>();
    std::set<std::string> used;
    uint64_t total = 0;
    const std::string base = SafeFileName(folder);
    unsigned number = 0;
    for (const Track* t : tracks) {
        ++number;
        if (IsSubtrack(t->path)) continue;
        Source probe;
        if (!probe.Open(t->path)) continue;
        std::string name = base + '/' + DownloadName(*t, numbered ? number : 0);
        // Two songs can share a title (and a playlist can hold a song twice): keep both.
        for (int copy = 2; !used.insert(name).second; ++copy) {
            std::string extension = Extension(*t);
            std::string stem = base + '/' + DownloadName(*t, numbered ? number : 0);
            stem.resize(stem.size() - extension.size());
            name = stem + " (" + std::to_string(copy) + ')' + extension;
        }
        Entry e{t, std::move(name), probe.size, total};
        total += 30 + e.name.size() + e.size + 16;
        entries->push_back(std::move(e));
    }
    if (entries->empty()) { result.error = "None of these songs has a file of its own to download."; return result; }
    uint64_t directory = 0;
    for (const auto& e : *entries) directory += 46 + e.name.size();
    if (entries->size() >= 0xFFFF || total + directory + 22 >= 0xFFFFFFFFull) {
        result.error = "That is more than one ZIP can hold (4 GB). Download an album or a shorter playlist.";
        return result;
    }
    // The same date on every entry: now, in local time as ZIP expects.
    time_t now = time(nullptr);
    tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    const uint16_t dosTime = (uint16_t)((local.tm_hour << 11) | (local.tm_min << 5) | (local.tm_sec / 2));
    const uint16_t dosDate = (uint16_t)(((std::max(local.tm_year, 80) - 80) << 9) | ((local.tm_mon + 1) << 5) | local.tm_mday);
    result.size = total + directory + 22;
    result.mime = "application/zip";
    result.send = [entries, total, directory, dosTime, dosDate](uint64_t at, size_t length, const auto& send) {
        if (at != 0) return false;  // ranges are refused before this; sent whole, in order
        auto header = [&](const Entry& e, bool central) {
            std::string h(central ? 46 : 30, '\0');
            char* p = h.data();
            Put32(p, central ? 0x02014b50 : 0x04034b50);
            int o = central ? 6 : 4;
            if (central) Put16(p + 4, 20);                       // made by: 2.0
            Put16(p + o, 20);                                    // needed: 2.0
            Put16(p + o + 2, 0x0808);                            // data descriptor, UTF-8 names
            Put16(p + o + 6, dosTime); Put16(p + o + 8, dosDate);
            if (central) {
                Put32(p + 16, e.crc); Put32(p + 20, (uint32_t)e.size); Put32(p + 24, (uint32_t)e.size);
                Put16(p + 28, (uint16_t)e.name.size());
                Put32(p + 42, (uint32_t)e.offset);
            } else {
                // The CRC follows the data, but the sizes are known: readers that stream a stored
                // entry need them, and every other reader takes the central directory's.
                Put32(p + 18, (uint32_t)e.size); Put32(p + 22, (uint32_t)e.size);
                Put16(p + 26, (uint16_t)e.name.size());
            }
            return h + e.name;
        };
        uint64_t sent = 0;
        for (auto& e : *entries) {
            std::string error;
            auto file = OpenTrackFile(*e.track, error);
            // A file that changed size since the plan would break every offset after it.
            if (!file || file->size != e.size) return false;
            std::string h = header(e, false);
            if (!send(h.data(), h.size())) return false;
            std::array<char, 65536> buffer;
            uint32_t crc = 0;
            for (uint64_t done = 0; done < e.size;) {
                size_t n = (size_t)std::min<uint64_t>(buffer.size(), e.size - done);
                if (!file->Read(done, n, buffer.data())) return false;
                crc = Crc32((const unsigned char*)buffer.data(), n, crc);
                if (!send(buffer.data(), n)) return false;
                done += n;
            }
            e.crc = crc;
            char descriptor[16];
            Put32(descriptor, 0x08074b50); Put32(descriptor + 4, crc);
            Put32(descriptor + 8, (uint32_t)e.size); Put32(descriptor + 12, (uint32_t)e.size);
            if (!send(descriptor, sizeof(descriptor))) return false;
            sent += h.size() + e.size + sizeof(descriptor);
        }
        std::string tail;
        for (const auto& e : *entries) tail += header(e, true);
        std::string end(22, '\0');
        Put32(end.data(), 0x06054b50);
        Put16(end.data() + 8, (uint16_t)entries->size()); Put16(end.data() + 10, (uint16_t)entries->size());
        Put32(end.data() + 12, (uint32_t)directory); Put32(end.data() + 16, (uint32_t)total);
        tail += end;
        return sent == total && sent + tail.size() == length && send(tail.data(), tail.size());
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
    std::string bytes;
    if (std::wstring custom = customcover::ImageFor(track.path); !custom.empty()) {
        Source file;
        if (file.Open(custom) && file.size <= 16u * 1024 * 1024) {
            bytes.resize((size_t)file.size);
            if (file.Read(0, bytes.size(), bytes.data()) && !(mime = ImageMime(bytes)).empty()) return bytes;
        }
        bytes.clear();
    }
    std::wstring path = AudioFileOf(track.path);
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
    accumulator.Begin((uint64_t)std::llround(decoder->Duration() * decoder->Rate()), decoder->Rate());
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
