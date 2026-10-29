// Exact PCM reading (decoder.h's PcmReader) and the MD5 it verifies: RFC 1321's test suite, FLAC
// fixtures whose encoder stored the MD5 of their samples, damaged copies of one, and WAV and AIFF
// files written here around known samples. Run from selftest.cpp's "exact PCM and MD5" group.
#include "selftest_common.h"
#include "md5.h"
#include "native_audio.h"

namespace {

using Digest = std::array<uint8_t, 16>;

std::string Hex(const Digest& d) {
    static const char digits[] = "0123456789abcdef";
    std::string s;
    for (uint8_t b : d) {
        s += digits[b >> 4];
        s += digits[b & 15];
    }
    return s;
}

Digest Md5Of(std::string_view bytes) {
    Md5 md5;
    md5.Add(bytes.data(), bytes.size());
    return md5.Finish();
}

struct Exact {
    bool opened = false, damaged = false;
    PcmFormat format;
    std::vector<int32_t> samples;
    Digest digest{};
};

// Reads to the end in uneven requests, so copies start and stop inside the format's frames.
Exact ReadAll(std::unique_ptr<PcmReader> reader) {
    Exact e;
    if (!reader) return e;
    e.opened = true;
    e.format = reader->Format();
    std::vector<int32_t> block(4096 * (size_t)e.format.channels);
    size_t want = 1;
    while (size_t n = reader->Read(block.data(), want)) {
        e.samples.insert(e.samples.end(), block.begin(), block.begin() + (ptrdiff_t)(n * e.format.channels));
        want = want * 13 % 4096 + 1;
    }
    e.damaged = reader->Damaged();
    e.digest = reader->Digest();
    return e;
}

Exact ReadAll(const std::wstring& path) {
    std::wstring error;
    return ReadAll(OpenPcmReader(path, error));
}

// Samples at `bits` precision: a pseudo-random full-scale pattern that starts with both extremes.
std::vector<int32_t> Pattern(unsigned bits, unsigned channels, unsigned frames) {
    std::vector<int32_t> v((size_t)channels * frames);
    uint32_t x = 2463534242u ^ bits;
    for (auto& s : v) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        s = (int32_t)x >> (32 - bits);
    }
    v[0] = INT32_MIN >> (32 - bits);
    v[1] = INT32_MAX >> (32 - bits);
    return v;
}

void Le(std::string& s, uint64_t v, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) s += (char)(v >> (8 * i));
}
void Be(std::string& s, uint64_t v, unsigned bytes) {
    for (unsigned i = bytes; i--;) s += (char)(v >> (8 * i));
}

// Samples left-justified in `container`-bit slots, as WAVE_FORMAT_EXTENSIBLE and AIFF store a
// narrower precision. WAV's 8-bit samples are unsigned.
std::string Slots(const std::vector<int32_t>& samples, unsigned bits, unsigned container, bool big, bool unsigned8) {
    std::string s;
    for (int32_t v : samples) {
        uint32_t u = (uint32_t)v << (container - bits);
        if (unsigned8) u += 128;
        if (big) Be(s, u, container / 8);
        else Le(s, u, container / 8);
    }
    return s;
}

// A 44.1 kHz RIFF WAVE file. An extensible header carries the significant bits and speaker mask.
std::string Wave(const std::string& data, unsigned channels, unsigned container, unsigned bits, uint32_t mask,
                 bool extensible, uint16_t tag = 1) {
    std::string fmt;
    Le(fmt, extensible ? 0xFFFE : tag, 2);
    Le(fmt, channels, 2);
    Le(fmt, 44100, 4);
    Le(fmt, 44100ull * channels * container / 8, 4);
    Le(fmt, channels * container / 8, 2);
    Le(fmt, container, 2);
    if (extensible) {
        Le(fmt, 22, 2);
        Le(fmt, bits, 2);
        Le(fmt, mask, 4);
        Le(fmt, tag, 4);
        fmt.append("\x00\x00\x10\x00\x80\x00\x00\xAA\x00\x38\x9B\x71", 12);  // the rest of the subformat GUID
    }
    std::string s = "RIFF";
    Le(s, 4 + 8 + fmt.size() + 8 + data.size() + (data.size() & 1), 4);
    s += "WAVEfmt ";
    Le(s, fmt.size(), 4);
    s += fmt + "data";
    Le(s, data.size(), 4);
    s += data;
    if (data.size() & 1) s += '\0';
    return s;
}

// A 44.1 kHz AIFF file, or AIFF-C 'sowt' (little-endian samples). COMM's sample size is the
// significant bits; the samples sit in whole-byte containers.
std::string Aiff(const std::string& data, unsigned channels, unsigned bits, size_t frames, bool sowt) {
    std::string comm;
    Be(comm, channels, 2);
    Be(comm, frames, 4);
    Be(comm, bits, 2);
    Be(comm, 0x400E, 2);  // 44100 as an 80-bit extended float: exponent 15, then the mantissa
    Be(comm, 44100ull << 48, 8);
    if (sowt) comm += std::string("sowt\0\0", 6);  // an empty compression name, padded to even
    std::string s = "FORM";
    Be(s, 4 + 8 + comm.size() + 16 + data.size() + (data.size() & 1), 4);
    s += sowt ? "AIFC" : "AIFF";
    s += "COMM";
    Be(s, comm.size(), 4);
    s += comm + "SSND";
    Be(s, 8 + data.size(), 4);
    Be(s, 0, 8);  // data offset and block size
    s += data;
    if (data.size() & 1) s += '\0';
    return s;
}

}  // namespace

void TestMd5() {
    // RFC 1321, appendix A.5.
    const struct {
        const char* text;
        const char* digest;
    } suite[] = {
        {"", "d41d8cd98f00b204e9800998ecf8427e"},
        {"a", "0cc175b9c0f1b6a831c399e269772661"},
        {"abc", "900150983cd24fb0d6963f7d28e17f72"},
        {"message digest", "f96b697d7cb7938d525a2f31aaf161d0"},
        {"abcdefghijklmnopqrstuvwxyz", "c3fcd3d76192e4007dfb496cca67e13b"},
        {"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", "d174ab98d277d9f5a5611c2c9f419d9f"},
        {"12345678901234567890123456789012345678901234567890123456789012345678901234567890",
         "57edf4a22be3c955ac49da2e2107b67a"}};
    testlog::Cases vectors;
    for (const auto& t : suite)
        vectors.Add(Hex(Md5Of(t.text)) == t.digest, L"string " + std::to_wstring(&t - suite + 1));
    Check(vectors.Passed(), L"MD5 of RFC 1321's seven test strings%ls", vectors.failed.c_str());
    // A million 'a's in uneven pieces, so buffered and whole blocks meet at every offset.
    Md5 pieces;
    const std::string a(1000, 'a');
    for (size_t fed = 0, step = 1; fed < 1000000; step = step * 7 % 1000 + 1) {
        const size_t n = std::min(step, 1000000 - fed);
        pieces.Add(a.data(), n);
        fed += n;
    }
    Check(Hex(pieces.Finish()) == "7707d6ae4e027c70eea2a935c2296f21", L"MD5 of a million 'a's fed in uneven pieces");
    // FLAC's layout: each sample little-endian in ceil(bits / 8) bytes.
    const int32_t samples[] = {1, -2, 0x7f, -0x80};
    const struct {
        unsigned bits;
        std::string_view bytes;
    } layouts[] = {{8, {"\x01\xfe\x7f\x80", 4}},
                   {12, {"\x01\x00\xfe\xff\x7f\x00\x80\xff", 8}},
                   {16, {"\x01\x00\xfe\xff\x7f\x00\x80\xff", 8}},
                   {20, {"\x01\x00\x00\xfe\xff\xff\x7f\x00\x00\x80\xff\xff", 12}},
                   {24, {"\x01\x00\x00\xfe\xff\xff\x7f\x00\x00\x80\xff\xff", 12}},
                   {32, {"\x01\x00\x00\x00\xfe\xff\xff\xff\x7f\x00\x00\x00\x80\xff\xff\xff", 16}}};
    testlog::Cases widths;
    for (const auto& layout : layouts) {
        Md5 pcm;
        pcm.AddPcm(samples, 4, layout.bits);
        widths.Add(pcm.Finish() == Md5Of(layout.bytes), std::to_wstring(layout.bits) + L"-bit");
    }
    Check(widths.Passed(), L"MD5 hashes 8- to 32-bit samples in ceil(bits / 8) bytes, as FLAC does%ls", widths.failed.c_str());
}

void TestExactPcm(const std::wstring& root, const std::wstring& scratch) {
    // FFmpeg's FLAC encoder stored the MD5 of each fixture's samples; reading to the end checks it.
    testlog::Cases stored;
    for (const wchar_t* name : {L"media\\sine.flac", L"gap2\\a.flac", L"gap2\\b.flac", L"rg\\rg.flac", L"tags2\\full.flac"}) {
        const Exact e = ReadAll(root + L"\\" + name);
        stored.Add(e.opened && !e.damaged && e.format.md5 != Digest{} && e.digest == e.format.md5 &&
                       e.samples.size() == e.format.frames * e.format.channels,
                   name);
    }
    Check(stored.Passed(), L"every FLAC fixture reads exactly and matches the MD5 its encoder stored%ls", stored.failed.c_str());
    // gap2's halves were encoded from ref.wav, so together they hold exactly its samples.
    const Exact a = ReadAll(root + L"\\gap2\\a.flac"), b = ReadAll(root + L"\\gap2\\b.flac");
    const Exact ref = ReadAll(root + L"\\gap2\\ref.wav");
    std::vector<int32_t> joined = a.samples;
    joined.insert(joined.end(), b.samples.begin(), b.samples.end());
    Check(ref.opened && !ref.damaged && ref.format.bits == 16 && a.format.bits == 16 && !joined.empty() &&
              joined == ref.samples,
          L"gap2's FLAC halves hold exactly the samples of the WAV they were encoded from");
    // Float playback and exact reading share FLAC's frame decoder: each float is its integer, scaled.
    const Exact sine = ReadAll(root + L"\\media\\sine.flac");
    unsigned rate = 0;
    const std::vector<float> floats = DecodeAll(root + L"\\media\\sine.flac", rate);
    bool scaled = sine.format.channels == 2 && !floats.empty() && floats.size() == sine.samples.size();
    const float scale = std::ldexp(1.0f, 1 - (int)sine.format.bits);
    for (size_t i = 0; scaled && i < floats.size(); ++i) scaled = floats[i] == (float)sine.samples[i] * scale;
    Check(scaled, L"FLAC's float output is its exact integers, scaled");
    // A Matroska track may be trimmed, so its STREAMINFO MD5 is not claimed for what it reads.
    const Exact mka = ReadAll(OpenFlacExact(root + L"\\native\\sine.mka"));
    Check(mka.opened && !mka.damaged && mka.format.md5 == Digest{} && mka.samples == sine.samples,
          L"FLAC in Matroska reads the same samples as the FLAC file, without claiming its MD5");

    // Damaged copies: cut short, one flipped bit in an audio frame, and a wrong stored MD5.
    const std::string flac = ReadTestBytes(root + L"\\gap2\\a.flac");
    std::string cut = flac.substr(0, flac.size() * 3 / 5), flipped = flac, md5 = flac;
    if (flac.size() > 64) {
        flipped[flac.size() / 2] ^= 0x10;
        md5[26] ^= 1;  // "fLaC", the block header, then STREAMINFO's MD5 at byte 18
    }
    const std::wstring copy = scratch + L"\\exact-damaged.flac";
    const std::pair<const std::string*, const wchar_t*> damaged[] = {
        {&cut, L"a file cut short"}, {&flipped, L"a flipped bit"}, {&md5, L"a wrong stored MD5"}};
    testlog::Cases reported;
    for (const auto& [bytes, what] : damaged) {
        const Exact e = WriteTestBytes(copy, *bytes) ? ReadAll(copy) : Exact{};
        reported.Add(e.opened && e.damaged && e.digest == Digest{}, what);
    }
    Check(reported.Passed(), L"exact FLAC reading reports a cut file, a flipped bit and a wrong stored MD5 as damaged%ls",
          reported.failed.c_str());
    DeleteFileW(copy.c_str());

    // WAV and AIFF written around known samples, each read back with its format and MD5.
    testlog::Cases layouts;
    auto expect = [&](const wchar_t* name, const std::string& bytes, const std::vector<int32_t>& samples, unsigned bits,
                      unsigned channels, uint32_t mask) {
        const std::wstring path = scratch + L"\\" + name;
        const Exact e = WriteTestBytes(path, bytes) ? ReadAll(path) : Exact{};
        Md5 md5;
        md5.AddPcm(samples.data(), samples.size(), bits);
        layouts.Add(e.opened && !e.damaged && e.samples == samples && e.format.bits == bits && e.format.channels == channels &&
                        e.format.channelMask == mask && e.format.rate == 44100 &&
                        e.format.frames == samples.size() / channels && e.digest == md5.Finish(),
                    name);
        DeleteFileW(path.c_str());
    };
    const auto mono8 = Pattern(8, 1, 999), stereo16 = Pattern(16, 2, 1000), six24 = Pattern(24, 6, 333);
    const auto stereo32 = Pattern(32, 2, 500), twenty = Pattern(20, 2, 777);
    expect(L"exact-u8.wav", Wave(Slots(mono8, 8, 8, false, true), 1, 8, 8, 0, false), mono8, 8, 1, 0);
    expect(L"exact-16.wav", Wave(Slots(stereo16, 16, 16, false, false), 2, 16, 16, 0, false), stereo16, 16, 2, 0);
    expect(L"exact-24.wav", Wave(Slots(six24, 24, 24, false, false), 6, 24, 24, 0x3F, true), six24, 24, 6, 0x3F);
    expect(L"exact-32.wav", Wave(Slots(stereo32, 32, 32, false, false), 2, 32, 32, 3, true), stereo32, 32, 2, 3);
    expect(L"exact-20.wav", Wave(Slots(twenty, 20, 24, false, false), 2, 24, 20, 3, true), twenty, 20, 2, 3);
    expect(L"exact-20.aif", Aiff(Slots(twenty, 20, 24, true, false), 2, 20, 777, false), twenty, 20, 2, 0);
    expect(L"exact-s8.aiff", Aiff(Slots(mono8, 8, 8, true, false), 1, 8, 999, false), mono8, 8, 1, 0);
    expect(L"exact-sowt.aifc", Aiff(Slots(stereo16, 16, 16, false, false), 2, 16, 1000, true), stereo16, 16, 2, 0);
    // One padding bit set, in the very last sample: the header's 20 bits would drop it, so the
    // container's 24 are reported instead.
    std::string dirty = Slots(twenty, 20, 24, false, false);
    dirty[dirty.size() - 3] |= 1;
    std::vector<int32_t> container(twenty.size());
    for (size_t i = 0; i < twenty.size(); ++i) container[i] = twenty[i] * 16;
    container.back() |= 1;
    expect(L"exact-20-padded.wav", Wave(dirty, 2, 24, 20, 3, true), container, 24, 2, 3);
    Check(layouts.Passed(), L"WAV, AIFF and AIFF-C from 8 to 32 bits and 1 to 6 channels read exactly with their format and "
          L"MD5, a set padding bit reporting the container's width%ls", layouts.failed.c_str());

    // Samples that aren't integers, and lossy streams, are refused with a reason.
    const std::wstring floatWav = scratch + L"\\exact-float.wav";
    std::wstring floatError, mp3Error;
    const bool floatRefused = WriteTestBytes(floatWav, Wave(std::string(800, '\0'), 2, 32, 32, 0, false, 3)) &&
                              !OpenPcmReader(floatWav, floatError) && !floatError.empty();
    DeleteFileW(floatWav.c_str());
    Check(floatRefused && !OpenPcmReader(root + L"\\media\\sine.mp3", mp3Error) && !mp3Error.empty(),
          L"float WAV and MP3 are refused for exact reading, with a reason");
}
