// Built-in AAC/MP3 encoding and gapless M4A muxing, shared by exports and device copies.
// Decoding and resampling use cordinal's own PCM path; no playback effects are applied.
#include "audio_encode.h"
#include "aac_encoder.h"
#include "cue.h"
#include "decoder.h"
#include <windows.h>
#include <algorithm>
#include <cmath>

namespace {
std::string Be(uint64_t v, int bytes) {
    std::string s((size_t)bytes, '\0');
    for (int i = bytes - 1; i >= 0; --i, v >>= 8) s[(size_t)i] = (char)(v & 0xFF);
    return s;
}

std::string Box(const char* type, const std::string& body) { return Be(body.size() + 8, 4) + type + body; }
std::string FullBox(const char* type, uint32_t versionFlags, const std::string& body) { return Box(type, Be(versionFlags, 4) + body); }

// The identity matrix every mvhd and tkhd carries.
std::string Matrix() { return Be(0x10000, 4) + Be(0, 12) + Be(0x10000, 4) + Be(0, 12) + Be(0x40000000, 4); }

// The codec's lookahead/overlap is excluded from playback by both standard gapless descriptions.
// Native decoder checks cover the length and alignment, including the stream's final padding.
constexpr uint32_t kAacDelay = aac::kEncoderDelay;

std::string Moov(const std::vector<uint32_t>& sizes, uint64_t frames, uint32_t mdatData, unsigned kbps, const std::string& metadata) {
    const uint64_t media = (uint64_t)sizes.size() * 1024;
    const std::string mvhd = FullBox("mvhd", 0, Be(0, 8) + Be(48000, 4) + Be(frames, 4) + Be(0x10000, 4) + Be(0x100, 2) + Be(0, 10) +
                                                    Matrix() + Be(0, 24) + Be(2, 4));
    const std::string tkhd = FullBox("tkhd", 7, Be(0, 8) + Be(1, 4) + Be(0, 4) + Be(frames, 4) + Be(0, 8) + Be(0, 4) + Be(0x100, 2) +
                                                    Be(0, 2) + Matrix() + Be(0, 8));
    const std::string edts = Box("edts", FullBox("elst", 0, Be(1, 4) + Be(frames, 4) + Be(kAacDelay, 4) + Be(0x10000, 4)));
    // AudioSpecificConfig: AAC LC, 48 kHz, two channels.
    const std::string config = std::string("\x05\x02\x11\x90", 4);
    const std::string decoder = std::string("\x04\x11\x40\x15", 4) + Be(6144, 3) + Be(kbps * 1000, 4) + Be(kbps * 1000, 4) + config;
    const std::string es = std::string("\x03\x19\x00\x01\x00", 5) + decoder + std::string("\x06\x01\x02", 3);
    const std::string mp4a = Box("mp4a", Be(0, 6) + Be(1, 2) + Be(0, 8) + Be(2, 2) + Be(16, 2) + Be(0, 4) + Be(48000u << 16, 4) +
                                             FullBox("esds", 0, es));
    std::string stsz = Be(0, 4) + Be(sizes.size(), 4);
    for (uint32_t s : sizes) stsz += Be(s, 4);
    const std::string stbl = Box("stbl", FullBox("stsd", 0, Be(1, 4) + mp4a) + FullBox("stts", 0, Be(1, 4) + Be(sizes.size(), 4) + Be(1024, 4)) +
                                             FullBox("stsc", 0, Be(1, 4) + Be(1, 4) + Be(sizes.size(), 4) + Be(1, 4)) +
                                             FullBox("stsz", 0, stsz) + FullBox("stco", 0, Be(1, 4) + Be(mdatData, 4)));
    const std::string minf = Box("minf", FullBox("smhd", 0, Be(0, 4)) +
                                             Box("dinf", FullBox("dref", 0, Be(1, 4) + FullBox("url ", 1, ""))) + stbl);
    const std::string mdia = Box("mdia", FullBox("mdhd", 0, Be(0, 8) + Be(48000, 4) + Be(media, 4) + Be(0x55C4, 2) + Be(0, 2)) +
                                             FullBox("hdlr", 0, Be(0, 4) + "soun" + Be(0, 12) + std::string("SoundHandler", 13)) + minf);
    // iTunSMPB, for players that read Apple's gapless note rather than the edit list.
    const uint64_t padding = media > frames + kAacDelay ? media - frames - kAacDelay : 0;
    char smpb[160];
    sprintf_s(smpb, " 00000000 %08X %08X %016llX 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000",
              kAacDelay, (unsigned)padding, (unsigned long long)frames);
    const std::string ilst = Box("ilst", metadata + Box("----", FullBox("mean", 0, "com.apple.iTunes") + FullBox("name", 0, "iTunSMPB") +
                                                         Box("data", Be(1, 4) + Be(0, 4) + smpb)));
    const std::string meta = FullBox("meta", 0, FullBox("hdlr", 0, Be(0, 4) + "mdir" + "appl" + Be(0, 9)) + ilst);
    return Box("moov", mvhd + Box("trak", tkhd + edts + mdia) + Box("udta", meta));
}

}  // namespace

bool EncodeLossyAudio(const std::wstring& source, unsigned kbps, bool mp3, const std::atomic<bool>& cancel,
                      const std::function<bool(const char*, size_t)>& send, uint64_t& frames, std::wstring& error) {
    auto decoder = OpenTrackDecoder(source, 48000, error);
    if (!decoder || !decoder->Rate()) {
        if (error.empty()) error = L"Couldn't decode this file.";
        return false;
    }
    aac::Encoder encoder;
    if (!encoder.Open(kbps, mp3)) {
        error = L"The built-in audio encoder couldn't start with this bitrate.";
        return false;
    }
    Resampler resampler;
    resampler.Init(decoder->Rate(), 48000);
    std::vector<float> input(4096 * 2), output;
    std::vector<char> pcm;
    frames = 0;
    bool ok = true;
    for (bool last = false; ok && !last && !cancel;) {
        output.clear();
        if (const size_t n = decoder->Read(input.data(), 4096)) resampler.Push(input.data(), n, output);
        else {
            if (decoder->Failed()) { error = L"The source audio is damaged."; return false; }
            resampler.Flush(output);
            last = true;
        }
        pcm.resize(output.size() * 2);
        for (size_t i = 0; i < output.size(); ++i) {
            const float v = std::isfinite(output[i]) ? std::clamp(output[i], -1.f, 1.f) : 0.f;
            const int s = std::clamp((int)std::lround(v * 32768.f), -32768, 32767);
            pcm[i * 2] = (char)(s & 0xFF);
            pcm[i * 2 + 1] = (char)(s >> 8);
        }
        frames += output.size() / 2;
        if (!pcm.empty()) ok = encoder.Write(pcm.data(), pcm.size(), send, cancel);
    }
    ok = ok && !cancel && frames && encoder.Finish(send, cancel);
    if (!ok && error.empty()) error = cancel ? L"Cancelled." : L"The audio encoder couldn't finish this file.";
    return ok;
}

bool EncodeM4a(const std::wstring& source, const std::wstring& out, unsigned kbps, const std::atomic<bool>& cancel,
               std::wstring& error, const std::string& metadata) {
    HANDLE f = CreateFileW(out.c_str(), GENERIC_WRITE | GENERIC_READ, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        error = L"Couldn't create the encoded file. Check free space and folder permissions.";
        return false;
    }
    std::string buffer = Box("ftyp", std::string("M4A ") + Be(0, 4) + "M4A mp42isom") + Be(0, 4) + "mdat";
    const uint32_t mdatData = (uint32_t)buffer.size();
    uint64_t written = 0;
    bool ok = true;
    auto flush = [&] {
        DWORD n = 0;
        ok = ok && WriteFile(f, buffer.data(), (DWORD)buffer.size(), &n, nullptr) && n == buffer.size();
        written += buffer.size();
        buffer.clear();
        return ok;
    };
    // The encoder gives ADTS; each frame's header comes off and its size goes into the sample table.
    std::vector<uint32_t> sizes;
    std::string adts;
    const aac::Sender take = [&](const char* data, size_t n) {
        adts.append(data, n);
        size_t at = 0;
        while (adts.size() - at >= 7) {
            const uint8_t* p = (const uint8_t*)adts.data() + at;
            const size_t length = (size_t)(p[3] & 3) << 11 | (size_t)p[4] << 3 | p[5] >> 5, header = (p[1] & 1) ? 7 : 9;
            if (p[0] != 0xFF || (p[1] & 0xF6) != 0xF0 || length <= header) return ok = false;
            if (adts.size() - at < length) break;
            buffer.append(adts, at + header, length - header);
            sizes.push_back((uint32_t)(length - header));
            at += length;
        }
        adts.erase(0, at);
        return buffer.size() < (1u << 20) || flush();
    };
    uint64_t frames = 0;
    ok = EncodeLossyAudio(source, kbps, false, cancel, take, frames, error) && ok && adts.empty() && !sizes.empty() &&
         frames <= UINT32_MAX && sizes.size() <= UINT32_MAX / 1024 && flush();
    // The mdat runs from its header to the moov; its size is known only now.
    const uint64_t mdatSize = written - (mdatData - 8);
    if (ok) {
        buffer = Moov(sizes, frames, mdatData, kbps, metadata);
        LARGE_INTEGER at{};
        at.QuadPart = mdatData - 8;
        const std::string size = Be(mdatSize, 4);
        DWORD n = 0;
        ok = flush() && mdatSize < (1ull << 32) && SetFilePointerEx(f, at, nullptr, FILE_BEGIN) &&
             WriteFile(f, size.data(), 4, &n, nullptr) && n == 4 && FlushFileBuffers(f);
    }
    CloseHandle(f);
    if (!ok) {
        DeleteFileW(out.c_str());
        if (error.empty()) error = cancel ? L"Cancelled." : L"The AAC encoder failed on this file.";
    }
    return ok;
}
