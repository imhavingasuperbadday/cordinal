// Ensure the player's ordinary self-test cannot pass a native-codec regression
// by silently using Media Foundation or an installed FFmpeg process instead.
// Each format is opened directly and through public dispatch. The output is checked for exact
// duration, seeking, clean endings and sticky failures, without a device or screenshots.
#include "native_audio.h"
#include "audio_bits.h"
#include "audio_packets.h"
#include "testlog.h"
#include "selftest_common.h"
#include <cmath>
#include <limits>
#include <cstring>

// Every check lands in the caller's "native codecs" group; the message names the file so a failure
// still points at the format that regressed.
namespace {
struct TestMp4Box { size_t type, body, end; };

bool NextMp4Box(const std::string& bytes, size_t& at, size_t end, TestMp4Box& box) {
    if (at > end || end > bytes.size() || end - at < 8) return false;
    const auto* p = (const uint8_t*)bytes.data() + at;
    uint64_t size = nativeaudio::U32(p, true); size_t head = 8;
    if (size == 1) {
        if (end - at < 16) return false;
        size = nativeaudio::U64(p + 8, true); head = 16;
    } else if (!size) size = end - at;
    if (size < head || size > end - at) return false;
    box = {at + 4, at + head, at + (size_t)size}; at = box.end;
    return true;
}

bool Mp4Child(const std::string& bytes, size_t start, size_t end, const char* type, TestMp4Box& result) {
    TestMp4Box box{};
    while (NextMp4Box(bytes, start, end, box)) if (!bytes.compare(box.type, 4, type)) { result = box; return true; }
    return false;
}

// Change only the audio track's clock: packet data and real playback time remain identical.
bool RescaleMp4Fixture(std::string& bytes, unsigned factor, bool removeEdit, bool inexact = false) {
    TestMp4Box moov{}, trak{}, mdia{}, handler{}, mdhd{}, minf{}, stbl{}, stts{};
    if (!Mp4Child(bytes, 0, bytes.size(), "moov", moov)) return false;
    size_t at = moov.body; bool sound = false;
    while (NextMp4Box(bytes, at, moov.end, trak)) {
        if (bytes.compare(trak.type, 4, "trak") || !Mp4Child(bytes, trak.body, trak.end, "mdia", mdia) ||
            !Mp4Child(bytes, mdia.body, mdia.end, "hdlr", handler) || handler.end - handler.body < 12) continue;
        if (!bytes.compare(handler.body + 8, 4, "soun")) { sound = true; break; }
    }
    if (!sound || !Mp4Child(bytes, mdia.body, mdia.end, "mdhd", mdhd) ||
        !Mp4Child(bytes, mdia.body, mdia.end, "minf", minf) ||
        !Mp4Child(bytes, minf.body, minf.end, "stbl", stbl) ||
        !Mp4Child(bytes, stbl.body, stbl.end, "stts", stts)) return false;
    auto read = [&](size_t pos, size_t width) {
        uint64_t value = 0;
        for (size_t i = 0; i < width; ++i) value = (value << 8) | (uint8_t)bytes[pos + i];
        return value;
    };
    auto write = [&](size_t pos, size_t width, uint64_t value) {
        for (size_t i = width; i-- > 0; value >>= 8) bytes[pos + i] = (char)value;
    };
    auto multiply = [&](size_t pos, size_t width) {
        uint64_t value = read(pos, width), max = width == 8 ? UINT64_MAX : UINT32_MAX;
        if (value > max / factor) return false;
        write(pos, width, value * factor); return true;
    };
    if (mdhd.end - mdhd.body < 24 || stts.end - stts.body < 8) return false;
    bool large = bytes[mdhd.body] == 1;
    if (large && mdhd.end - mdhd.body < 36) return false;
    if (!multiply(mdhd.body + (large ? 20 : 12), 4) ||
        !multiply(mdhd.body + (large ? 24 : 16), large ? 8 : 4)) return false;
    uint64_t count = read(stts.body + 4, 4);
    if (!count || count > (stts.end - stts.body - 8) / 8) return false;
    for (size_t i = 0; i < count; ++i) if (!multiply(stts.body + 12 + i * 8, 4)) return false;
    if (inexact) {
        uint64_t first = read(stts.body + 12, 4);
        if (first == UINT32_MAX) return false;
        write(stts.body + 12, 4, first + 1);
    }
    TestMp4Box edts{}, elst{};
    if (!Mp4Child(bytes, trak.body, trak.end, "edts", edts) ||
        !Mp4Child(bytes, edts.body, edts.end, "elst", elst)) return false;
    {
        if (removeEdit) bytes.replace(elst.type, 4, "free");
        else {
            if (elst.end - elst.body < 8) return false;
            large = bytes[elst.body] == 1;
            size_t width = large ? 8 : 4, stride = large ? 20 : 12;
            count = read(elst.body + 4, 4);
            if (count > (elst.end - elst.body - 8) / stride) return false;
            for (size_t i = 0; i < count; ++i) {
                size_t pos = elst.body + 8 + i * stride + width;
                if (!(read(pos, width) & (uint64_t(1) << (width * 8 - 1))) && !multiply(pos, width)) return false;
            }
        }
    }
    return true;
}

void TestWaveformPreviews(const std::wstring& root, const std::wstring& scratch) {
    const std::wstring path = root + L"\\native\\preview.flac";
    auto decoder = OpenFlacDecoder(path);
    bool sampled = decoder && !decoder->FastSeek() && decoder->CanPreview();
    for (double target : {1.0, 4.0, 9.0, 14.0, 19.0}) {
        if (!decoder) break;
        double actual = target;
        bool found = decoder->PreviewSeek(actual) && actual >= 0 && actual < decoder->Duration();
        auto reference = OpenFlacDecoder(path);
        float got[1024]{}, expected[1024]{};
        bool same = found && reference && reference->Seek(actual + 0.5 / decoder->Rate()) &&
                    decoder->Read(got, 512) == 512 && reference->Read(expected, 512) == 512 &&
                    std::memcmp(got, expected, sizeof(got)) == 0;
        sampled = sampled && same;
    }
    decoder = OpenFlacDecoder(path);
    Peaks whole{}, slice{};
    bool shaped = decoder && PreviewPeaks(*decoder, 0, decoder->Duration(), whole, nullptr) &&
                  whole.v[500] > whole.v[100] && PreviewPeaks(*decoder, 5, 10, slice, nullptr) &&
                  slice.v[500] > slice.v[100];
    Check(sampled && shaped,
          L"unindexed FLAC previews use real decoded frames at their actual positions, including track ranges");

    const std::wstring ogg = root + L"\\media\\sine.ogg";
    auto vorbis = OpenVorbisDecoder(ogg);
    testlog::Cases pages;
    for (double target : {0.5, 2.0, 4.0, 6.0}) {
        double actual = target;
        auto reference = OpenVorbisDecoder(ogg);
        float got[1024]{}, expected[1024]{};
        bool ok = vorbis && vorbis->CanPreview() && vorbis->PreviewSeek(actual) && reference &&
                  reference->Seek(actual + 0.5 / vorbis->Rate()) &&
                  vorbis->Read(got, 512) == 512 && reference->Read(expected, 512) == 512;
        for (size_t i = 0; ok && i < std::size(got); ++i) ok = std::fabs(got[i] - expected[i]) < 1e-6f;
        pages.Add(ok, std::to_wstring((int)target));
    }
    Check(pages.Passed(), L"Vorbis preview pages rebuild the overlap at their actual sample positions%ls", pages.failed.c_str());

    StartPeaks(nullptr, 0, scratch + L"\\preview-cache");
    testlog::Cases exact;
    for (const wchar_t* name : {L"native\\preview.flac", L"media\\sine.ogg", L"media\\sine.opus"}) {
        const std::wstring file = root + L"\\" + name;
        Peaks direct{}, cached{};
        bool computed = ComputePeaks(file, 0, 0, direct, nullptr);
        uint32_t request = 0;
        bool ready = GetPeaks(file, 0, 0, 0, 0, cached, request);
        const ULONGLONG until = GetTickCount64() + 5000;
        while (!ready && request && GetTickCount64() < until) {
            Sleep(1);
            ready = GetPeaks(file, 0, 0, 0, 0, cached, request);
        }
        exact.Add(computed && ready && std::memcmp(direct.v, cached.v, kPeakBuckets) == 0, name);
    }
    StopPeaks();
    Check(exact.Passed(), L"sampled previews are replaced by exact whole-file peaks before caching%ls", exact.failed.c_str());

    testlog::Cases formats;
    for (const wchar_t* name : {L"media\\sine.wav", L"media\\sine.mp3", L"media\\sine.m4a", L"media\\sine.ogg",
                                L"media\\sine.opus", L"native\\sine.wv", L"native\\preview.flac"}) {
        const ULONGLONG started = GetTickCount64();
        std::wstring error;
        auto source = OpenDecoder(root + L"\\" + name, 48000, error);
        Peaks preview{};
        bool ok = source && PreviewPeaks(*source, 0, source->Duration(), preview, nullptr) &&
                  preview.v[kPeakBuckets / 4] && preview.v[3 * kPeakBuckets / 4];
        formats.Add(ok, name);
        if (g_log) fwprintf(g_log, L"waveform preview %ls: %llu ms\n", name, GetTickCount64() - started);
    }
    Check(formats.Passed(), L"common audio formats produce a full-song preview%ls", formats.failed.c_str());
}

void TestMp4TimeScales(const std::wstring& root, const std::wstring& scratch) {
    struct Case { const wchar_t* source; unsigned factor; bool removeEdit; const wchar_t* name; };
    const Case cases[] = {
        {L"native\\video-aac.mp4", 10, false, L"video with AAC and an edit"},
        {L"native\\video-aac.mp4", 10, true, L"video with AAC and no edit"},
        {L"native\\video-aac.mp4", 3, false, L"video with a threefold clock"},
        {L"media\\alac.m4a", 10, false, L"ALAC with an edit"},
    };
    testlog::Cases same;
    for (size_t i = 0; i < std::size(cases); ++i) {
        const auto& c = cases[i];
        std::string original = ReadTestBytes(root + L"\\" + c.source), scaled = original;
        bool prepared = !original.empty() && RescaleMp4Fixture(original, 1, c.removeEdit) &&
                        RescaleMp4Fixture(scaled, c.factor, c.removeEdit);
        std::wstring ext = ExtOf(c.source), base = scratch + L"\\mp4-clock-" + std::to_wstring(i);
        std::wstring referencePath = base + L"-original." + ext, scaledPath = base + L"-scaled." + ext;
        prepared = prepared && WriteTestBytes(referencePath, original) && WriteTestBytes(scaledPath, scaled);
        if (!Ready(prepared, L"the MP4 clock fixture is prepared")) { same.Add(false, c.name); continue; }
        std::wstring error;
        auto reference = OpenDecoder(referencePath, 48000, error), decoder = OpenDecoder(scaledPath, 48000, error);
        bool ok = reference && decoder && reference->Rate() == decoder->Rate() &&
                  reference->Duration() == decoder->Duration();
        std::vector<float> expected;
        float block[4096 * 2];
        if (reference) {
            while (size_t n = reference->Read(block, 4096)) expected.insert(expected.end(), block, block + n * 2);
            ok = ok && !reference->Failed() && expected.size() ==
                 (size_t)std::llround(reference->Duration() * reference->Rate()) * 2;
        }
        size_t position = 0;
        if (decoder) {
            while (size_t n = decoder->Read(block, 4096)) {
                if (position + n * 2 > expected.size() ||
                    memcmp(block, expected.data() + position, n * 2 * sizeof(float))) { ok = false; break; }
                position += n * 2;
            }
            ok = ok && !decoder->Failed() && position == expected.size();
            size_t frames = expected.size() / 2;
            for (size_t target : {size_t(0), size_t(137), frames / 2, frames > 128 ? frames - 128 : size_t(0)}) {
                size_t want = std::min<size_t>(128, frames - std::min(target, frames));
                bool seek = decoder->Seek((target + 0.25) / decoder->Rate()) && decoder->Read(block, want) == want;
                if (seek && want) seek = !memcmp(block, expected.data() + target * 2, want * 2 * sizeof(float));
                ok = ok && seek;
            }
        }
        same.Add(ok, c.name);
    }
    testlog::Check(same.Passed(), L"MP4 audio clocks preserve every decoded sample, edit and seek%ls", same.failed.c_str());

    std::string inexact = ReadTestBytes(root + L"\\native\\video-aac.mp4");
    std::wstring path = scratch + L"\\mp4-inexact-clock.mp4", error;
    if (Ready(!inexact.empty() && RescaleMp4Fixture(inexact, 10, false, true) && WriteTestBytes(path, inexact),
              L"the inexact MP4 clock fixture is prepared"))
        testlog::Check(!OpenDecoder(path, 48000, error), L"MP4 packet boundaries that fall between samples are rejected");
}

// A failed packet may follow good PCM in the same read. Nothing from it may leak on later calls,
// and seeking to an intact part must recover without retaining the old error.
bool FailedPlayback(std::unique_ptr<Decoder> decoder) {
    if (!decoder) return false;
    float first[2], block[4096 * 2];
    if (decoder->Read(first, 1) != 1 || decoder->Failed()) return false;
    uint64_t read = 1, limit = (uint64_t)std::llround(decoder->Duration() * decoder->Rate()) + 4096;
    while (size_t n = decoder->Read(block, 4096)) {
        read += n;
        if (read > limit) return false;
    }
    float tail[2] = {123, 123};
    const bool sticky = decoder->Failed() && !decoder->Read(tail, 0) && decoder->Failed() &&
        !decoder->Read(tail, 1) && tail[0] == 123 && tail[1] == 123 &&
        !decoder->Seek(std::numeric_limits<double>::quiet_NaN()) && decoder->Failed();
    const bool recovered = decoder->Seek(0) && !decoder->Failed() && decoder->Read(tail, 1) == 1 &&
        !decoder->Failed() && tail[0] == first[0] && tail[1] == first[1];
    return sticky && recovered && decoder->Seek(decoder->Duration()) && !decoder->Failed() &&
        !decoder->Read(tail, 1) && !decoder->Failed();
}

// Exercise vector groups, scalar tails and mapping changes with integer extrema.
// Expected PCM comes directly from the stored integers, without another decoder.
void TestPcmBlocks(const std::wstring& scratch) {
    bool exact = true, bounded = true;
    const std::wstring path = scratch + L"\\pcm-blocks.wav";
    for (unsigned depth : {16u, 24u, 32u}) for (unsigned channels : {1u, 2u})
    for (unsigned frames : {1u, 2u, 3u, 4u, 5u, 7u, 9u, 16387u, 349527u}) {
        const unsigned width = depth / 8;
        const int64_t low = -(int64_t(1) << (depth - 1)), high = -low - 1;
        auto value = [&](size_t i) -> int32_t {
            switch (i % 7) {
            case 0: return (int32_t)low;
            case 1: return (int32_t)high;
            case 2: return -1;
            case 3: return 0;
            case 4: return 1;
            default: return (int32_t)(low + (int64_t)((i * 2654435761ull) & (uint64_t)(high - low)));
            }
        };
        std::string bytes;
        auto put = [&](uint32_t v, unsigned n) { for (unsigned i = 0; i < n; ++i) bytes += (char)(v >> (i * 8)); };
        const unsigned dataBytes = frames * channels * width;
        bytes += "RIFF"; put(36 + dataBytes + (dataBytes & 1), 4); bytes += "WAVEfmt "; put(16, 4);
        put(1, 2); put(channels, 2); put(44100, 4); put(44100 * channels * width, 4);
        put(channels * width, 2); put(depth, 2); bytes += "data"; put(dataBytes, 4);
        for (size_t i = 0; i < (size_t)frames * channels; ++i) put((uint32_t)value(i), width);
        if (dataBytes & 1) bytes += '\0';
        auto decoder = WriteTestBytes(path, bytes) ? OpenPcmDecoder(path) : nullptr;
        if (!decoder) { exact = false; continue; }
        size_t at = 0, chunk = 1;
        const float scale = (float)std::ldexp(1.0, 1 - (int)depth);
        while (at < frames) {
            const size_t n = std::min(chunk, (size_t)frames - at);
            std::vector<float> block(n * 2 + 2, 123.0f);
            const size_t got = decoder->Read(block.data() + 1, n);
            bounded &= block.front() == 123.0f && block.back() == 123.0f;
            if (got != n) { exact = false; break; }
            for (size_t i = 0; i < n; ++i) for (unsigned c = 0; c < 2; ++c)
                exact &= block[1 + i * 2 + c] == (float)value((at + i) * channels + (channels == 1 ? 0 : c)) * scale;
            at += n; chunk = chunk * 13 % 16384 + 1;
        }
        float tail[4] = {123, 123, 123, 123};
        bounded &= decoder->Read(tail + 1, 1) == 0 && tail[1] == 123 && tail[2] == 123;
        exact &= decoder->Seek((frames - 0.75) / 44100.0) && decoder->Read(tail + 1, 1) == 1;
        exact &= tail[1] == (float)value((frames - 1) * channels) * scale &&
                 tail[2] == (float)value((frames - 1) * channels + (channels == 1 ? 0 : 1)) * scale;
        bounded &= tail[0] == 123 && tail[3] == 123;
    }
    Check(exact, L"PCM mono/stereo 16/24/32-bit groups, tails, seeks and mapping boundaries reproduce exact samples");
    Check(bounded, L"PCM block conversion preserves unaligned output guards and writes nothing at EOF");
}

// Independent bit-packed fixed-predictor frames cover widths FFmpeg's encoder does
// not emit. Residuals use the polynomial definition, rather than the decoder's integrators.
bool TestFlacFixed(const std::wstring& scratch) {
    auto crc=[](const std::vector<uint8_t>& bytes,unsigned width,unsigned polynomial) {
        unsigned value=0;
        for (auto byte:bytes) {
            value^=(unsigned)byte<<(width-8);
            for (unsigned bit=0;bit<8;++bit)
                value=((value<<1)^((value&(1u<<(width-1)))?polynomial:0))&((1u<<width)-1);
        }
        return value;
    };
    const std::wstring path=scratch+L"\\fixed-predictor.flac";
    constexpr unsigned maxFrames=35;
    bool exact=true,rejected=true,integers=true,truncated=false;
    for (unsigned n:{32u,33u,34u,35u})
    for (unsigned depth:{4u,8u,12u,16u,20u,24u,32u})
    for (unsigned assignment:{1u,8u,9u,10u})
    for (unsigned order=0;order<=4;++order) {
        int64_t left[maxFrames],right[maxFrames];
        const int64_t low=-(int64_t(1)<<(depth-1)),high=-low-1;
        for (unsigned i=0;i<n;++i) {
            left[i]=order?low+(i*3%7):(int)(i*3%7)-3;
            right[i]=order?high-(i*5%7):(int)(i*5%7)-3;
        }
        for (bool corrupt:{false,true}) {
            std::vector<uint8_t> frame{255,248,0x60,(uint8_t)(assignment<<4),0,(uint8_t)(n-1)};
            frame.push_back((uint8_t)crc(frame,8,7));
            unsigned bits=0;
            auto put=[&](uint64_t value,unsigned count) {
                for (unsigned i=count;i--;) {
                    if (!(bits&7)) frame.push_back(0);
                    frame.back()|=(uint8_t)(((value>>i)&1)<<(7-(bits++&7)));
                }
            };
            for (unsigned channel=0;channel<2;++channel) {
                const bool side=(assignment==8 && channel==1)||(assignment==9 && channel==0)||
                                (assignment==10 && channel==1);
                unsigned width=depth+(unsigned)side;
                int64_t samples[maxFrames];
                for (unsigned i=0;i<n;++i)
                    samples[i]=side?left[i]-right[i]:assignment==10?(left[i]+right[i])>>1:channel?right[i]:left[i];
                if (corrupt && channel==0) samples[n-1]=-(int64_t(1)<<(width-1))-1;
                put((8+order)<<1,8);
                for (unsigned i=0;i<order;++i) put((uint64_t)samples[i],width);
                put(1,2); put(0,4); put(30,5);  // Rice method 1, one partition, parameter 30
                for (unsigned i=order;i<n;++i) {
                    int64_t prediction=0;
                    switch (order) {
                    case 1: prediction=samples[i-1];break;
                    case 2: prediction=2*samples[i-1]-samples[i-2];break;
                    case 3: prediction=3*(samples[i-1]-samples[i-2])+samples[i-3];break;
                    case 4: prediction=4*(samples[i-1]+samples[i-3])-6*samples[i-2]-samples[i-4];break;
                    }
                    int64_t residual=samples[i]-prediction;
                    uint64_t folded=residual<0?uint64_t(-(residual+1))*2+1:uint64_t(residual)*2;
                    for (uint64_t q=folded>>30;q;--q) put(0,1);
                    put(1,1); put(folded,30);
                }
            }
            unsigned sum=crc(frame,16,0x8005);
            frame.push_back((uint8_t)(sum>>8));frame.push_back((uint8_t)sum);
            std::vector<uint8_t> file{'f','L','a','C',128,0,0,34,0,(uint8_t)n,0,(uint8_t)n,0,0,0,0,0,0};
            uint64_t info=(44100ull<<44)|(1ull<<41)|(uint64_t(depth-1)<<36)|n;
            for (unsigned i=8;i--;) file.push_back((uint8_t)(info>>(i*8)));
            file.resize(file.size()+16,0);
            file.insert(file.end(),frame.begin(),frame.end());
            bool written=WriteTestBytes(path,{reinterpret_cast<const char*>(file.data()),file.size()});
            auto decoder=written?OpenFlacDecoder(path):nullptr;
            float pcm[maxFrames*2];
            size_t got=decoder?decoder->Read(pcm,n):0;
            if (corrupt) rejected&=written && !got;
            else {
                exact&=got==n;
                double scale=std::ldexp(1.0,1-(int)depth);
                for (size_t i=0;i<got;++i)
                    exact&=pcm[i*2]==(float)(left[i]*scale) && pcm[i*2+1]==(float)(right[i]*scale);
            }
            // The exact reader restores the integers themselves, which float cannot hold above 24 bits.
            auto reader=written?OpenFlacExact(path):nullptr;
            int32_t ints[maxFrames*2],tail[2];
            size_t read=reader?reader->Read(ints,n):0;
            if (corrupt) integers&=written && (!reader || (!read && reader->Damaged()));
            else {
                integers&=read==n && reader->Format().bits==depth && !reader->Read(tail,1) && !reader->Damaged();
                for (size_t i=0;i<read;++i) integers&=ints[i*2]==left[i] && ints[i*2+1]==right[i];
            }
            if (!corrupt && n==35 && depth==32 && assignment==10 && order==4) {
                // A complete first frame, but STREAMINFO promises two: truncation exactly at a
                // frame boundary must fail just as a bad CRC does. Release mappings before rewriting.
                decoder.reset(); reader.reset();
                file[25]=(uint8_t)(n*2);
                truncated=WriteTestBytes(path,{reinterpret_cast<const char*>(file.data()),file.size()}) &&
                          FailedPlayback(OpenFlacDecoder(path));
            }
        }
    }
    DeleteFileW(path.c_str());
    testlog::Check(exact,L"FLAC fixed orders 0..4 restore exact 4..32-bit PCM in all stereo modes, including 33-bit side channels and vector tails");
    testlog::Check(integers,L"FLAC exact reading returns the same 4..32-bit integers and refuses the corrupt frames");
    testlog::Check(rejected,L"FLAC fixed predictors reject out-of-range reconstruction before emitting a frame");
    return truncated;
}
}

// FLAC and AC-3 frame checksums fold whole 16-byte blocks by carry-less multiplication where the CPU
// has it and finish with tables; every length and start offset must give the bitwise CRC.
void TestCrc16() {
    std::vector<uint8_t> data(5000);
    uint32_t seed = 0x2545F491;
    for (auto& byte : data) { seed = seed * 1664525u + 1013904223u; byte = (uint8_t)(seed >> 24); }
    auto bitwise = [](const uint8_t* p, size_t n) {
        unsigned value = 0;
        for (size_t i = 0; i < n; ++i) {
            value ^= (unsigned)p[i] << 8;
            for (unsigned bit = 0; bit < 8; ++bit) value = ((value << 1) ^ ((value & 0x8000) ? 0x8005 : 0)) & 0xFFFF;
        }
        return (uint16_t)value;
    };
    testlog::Cases cases;
    for (size_t offset = 0; offset < 16; offset += 3)
        for (size_t n : {0, 1, 7, 8, 15, 16, 17, 63, 64, 65, 79, 80, 127, 128, 129, 255, 767, 768, 3839, 4096, 4985}) {
            if (offset + n > data.size()) continue;
            cases.Add(nativeaudio::Crc16(data.data() + offset, n) == bitwise(data.data() + offset, n),
                      std::to_wstring(n) + L" bytes at +" + std::to_wstring(offset));
        }
    testlog::Check(cases.Passed(), L"CRC-16 matches the bitwise definition at every length and offset%ls",
                   cases.failed.c_str());
}

void TestNativeCodecs(const std::wstring& root, const std::wstring& scratch) {
    TestWaveformPreviews(root, scratch);
    TestPcmBlocks(scratch);
    TestCrc16();
    TestMp4TimeScales(root, scratch);
    testlog::Cases failures;
    failures.Add(TestFlacFixed(scratch), L"FLAC truncated at a frame boundary");
    const std::wstring hybrid = scratch + L"\\hybrid.wv", plain = scratch + L"\\plain.wv";
    Ready(WriteTestBytes(hybrid, "wvpk") && WriteTestBytes(hybrid + L"c", "wvpk") && WriteTestBytes(plain, "wvpk"),
          L"WavPack correction-file fixtures are prepared");
    Check(IgnoresWavPackCorrection(hybrid) && IgnoresWavPackCorrection(hybrid + L"|2") &&
              !IgnoresWavPackCorrection(plain),
          L"WAVPACK a .wvc beside a track is noticed, so the player can say it plays the lossy part");
    // Trim(): a container-level discard and the codec's own priming are tracked separately, so
    // explicit leading audio is not lost when the codec consumes its delay. At 48 kHz, 2 ms of
    // explicit discard is 96 frames; 48000 - 96 = 47904 remain.
    nativeaudio::PacketFile file;file.matroska=true;file.frames=48000;file.consumedDelay=128;
    file.trimStartNs=2000000;
    bool priming=file.Trim(48000) && file.skip==96 && file.valid==47904;
    file.skip=file.valid=0;file.delayNs=2666667;
    testlog::Check(priming && file.Trim(48000) && file.skip==96 && file.valid==47904,
                   L"container priming: codec priming is consumed once while explicit discard remains");
    // exactDuration is false for the lossy codecs (WMA, AC-3) whose decoder cannot report the exact
    // sample count, so their length is only checked against the eight-second source.
    struct Fixture {
        const wchar_t* file;
        std::unique_ptr<Decoder> (*open)(const std::wstring&);
        bool exactDuration=true;
    };
    const Fixture fixtures[] = {{L"media\\sine.wav",OpenPcmDecoder},{L"media\\sine.flac",OpenFlacDecoder},
                                {L"media\\sine.mp3",OpenMpegDecoder},{L"media\\sine.ogg",OpenVorbisDecoder},
                                {L"media\\alac.m4a",OpenAlacDecoder},{L"media\\sine.m4a",OpenAacDecoder},
                                {L"media\\sine.opus",OpenOpusDecoder},{L"media\\sine.wma",OpenWmaDecoder,false},
                                {L"native\\sine.wv",OpenWavPackDecoder},{L"native\\sine.ac3",OpenAc3Decoder,false},
                                {L"native\\sine.mka",OpenMatroskaDecoder}};
    testlog::Cases opened, lengths, seeks, edges;
    for (const auto& fixture : fixtures) {
        auto d = fixture.open(root+L"\\"+fixture.file);
        std::wstring error;
        auto dispatched = OpenDecoder(root+L"\\"+fixture.file, 48000, error);
        opened.Add(d && dispatched && error.empty() && d->Rate() == dispatched->Rate() &&
                   d->Duration() == dispatched->Duration(),fixture.file);
        if (!d) continue;
        uint64_t total = 0;
        float pcm[4096*2], expected[128*2]{};
        bool finite = true;
        while (size_t n = d->Read(pcm,4096)) {
            for (size_t i = 0; i < n*2; ++i) finite &= std::isfinite(pcm[i]);
            if (total <= 256 && total+n >= 384) memcpy(expected,pcm+(256-total)*2,sizeof expected);
            total += n;
        }
        uint64_t expectedTotal=(uint64_t)std::llround(d->Duration()*d->Rate());
        bool duration=fixture.exactDuration?total==d->Rate()*8ull:std::abs((double)total-d->Rate()*8.0)<4096;
        lengths.Add(finite && duration && total==expectedTotal && !d->Failed(),fixture.file);
        bool seek = d->Seek(256.5/d->Rate()) && d->Read(pcm,128) == 128;
        if (seek) for (size_t i = 0; i < 256; ++i) seek &= std::abs(pcm[i]-expected[i]) < 0.000001f;
        seeks.Add(seek,fixture.file);
        edges.Add(d->Read(pcm,0) == 0 && !d->Failed() && d->Seek(d->Duration()) && d->Read(pcm,4096) == 0 && !d->Failed() &&
                      !d->Seek(std::numeric_limits<double>::quiet_NaN()) &&
                      !d->Seek(std::numeric_limits<double>::infinity()),
                  fixture.file);
        if (fixture.open == OpenMpegDecoder) {
            // A fresh MP3 has only its first offset indexed. Seeks before sequential playback
            // must still rebuild the same reservoir/filter state as the fully read decoder.
            auto fresh = OpenMpegDecoder(root+L"\\"+fixture.file);
            bool earlySeek = fresh && fresh->Duration() == d->Duration();
            for (double seconds : {7.5, 0.01, 4.25, 0.0}) {
                bool read = fresh && d->Seek(seconds) && fresh->Seek(seconds) &&
                            d->Read(expected,128) == 128 && fresh->Read(pcm,128) == 128;
                earlySeek = earlySeek && read;
                if (read) for (size_t i = 0; i < 256; ++i) earlySeek &= pcm[i] == expected[i];
            }
            testlog::Check(earlySeek,L"native MP3 seeks before playback match the fully indexed stream exactly");
        }
    }
    testlog::Check(opened.Passed(),L"all 11 native codecs open their fixture directly without a fallback%ls",opened.failed.c_str());
    testlog::Check(lengths.Passed(),L"native codecs give finite samples matching their duration and the eight-second source%ls",
                   lengths.failed.c_str());
    testlog::Check(seeks.Passed(),L"native codec seeks restore the exact native output samples%ls",seeks.failed.c_str());
    testlog::Check(edges.Passed(),L"native codecs refuse zero-size and end reads and non-finite seeks%ls",edges.failed.c_str());

    for (const wchar_t* name : {L"sine.flac", L"sine.mp3", L"sine.m4a", L"alac.m4a"}) {
        const std::wstring path = scratch + L"\\damaged-" + name;
        std::string bytes = ReadTestBytes(root + L"\\media\\" + name);
        if (!Ready(bytes.size() > 4096, L"source audio for damage checks exists")) continue;
        if (ExtOf(name) == L"mp3") bytes.resize(bytes.size() / 2);
        else {
            size_t at = bytes.size() / 2;
            if (ExtOf(name) == L"m4a") {
                nativeaudio::PacketFile packets;
                if (!Ready(packets.OpenMp4(root + L"\\media\\" + name) && packets.packets.size() > 4,
                           L"MP4 damage check locates an audio packet")) continue;
                at = (size_t)packets.packets[packets.packets.size() / 2].offset;
            }
            std::fill_n(bytes.begin() + at, 32, '\xFF');
        }
        if (!Ready(WriteTestBytes(path, bytes), L"damaged audio copy is prepared")) continue;
        auto open = [&] { std::wstring error; return OpenDecoder(path, 48000, error); };
        failures.Add(FailedPlayback(open()), name);
        failures.Add(FailedPlayback(OpenRange(open(), 0, 0)), std::wstring(name) + L" range");
        auto looped = open();
        const int64_t end = looped ? (int64_t)std::llround(looped->Duration() * looped->Rate()) : 0;
        failures.Add(FailedPlayback(OpenLooped(std::move(looped), 0, end, 1, 0)), std::wstring(name) + L" loop");
    }
    // IMA ADPCM: two small independent blocks. The second carries an invalid step index.
    const std::wstring adpcm = scratch + L"\\damaged-adpcm.wav";
    const char wave[] = "RIFF\x30\0\0\0WAVEfmt \x14\0\0\0\x11\0\x01\0\x40\x1F\0\0"
                        "\0\0\0\0\x04\0\x04\0\x02\0\x01\0data\x08\0\0\0\0\0\0\0\0\0\xFF\0";
    if (Ready(WriteTestBytes(adpcm, {wave, sizeof(wave) - 1}), L"damaged ADPCM blocks are prepared"))
        failures.Add(FailedPlayback(OpenPcmDecoder(adpcm)), L"ADPCM invalid second block");
    testlog::Check(failures.Passed(), L"damaged playback fails, stays failed without leaking PCM, and recovers on seek%ls",
                   failures.failed.c_str());
}
