// cordinal's own format (cord.h, cord_codec.h): encoder round trips over many sample shapes and a
// FLAC fixture, playback and seeking, damage the decoder must refuse, the format's own tags, and
// how the library sees the files. Run from selftest.cpp's "cordinal format" group.
#include "selftest_common.h"
#include "audio_file.h"
#include "cord.h"
#include "convert.h"
#include "cord_codec.h"
#include "tags.h"
#include "tagwriter.h"
#include "native_audio.h"
#include "encode/flac_encoder.h"

#include <limits>
#include <tuple>

namespace {

// Samples in memory, read as a file's would be.
class MemoryPcm final : public PcmReader {
public:
    MemoryPcm(const std::vector<int32_t>& samples, unsigned channels, unsigned bits, uint32_t mask = 0,
              unsigned rate = 44100) : samples_(samples) {
        format_.rate = rate;
        format_.channels = channels;
        format_.bits = bits;
        format_.channelMask = mask;
        format_.frames = samples.size() / channels;
    }
private:
    size_t Next(int32_t* out, size_t frames) override {
        const size_t n = std::min<size_t>(frames, samples_.size() / format_.channels - at_);
        std::copy_n(samples_.data() + at_ * format_.channels, n * format_.channels, out);
        at_ += n;
        return n;
    }
    const std::vector<int32_t>& samples_;
    size_t at_ = 0;
};

struct Exact {
    bool opened = false, damaged = false;
    PcmFormat format;
    std::vector<int32_t> samples;
    std::array<uint8_t, 16> digest{};
};

Exact ReadAll(std::unique_ptr<PcmReader> reader) {
    Exact e;
    if (!reader) return e;
    e.opened = true;
    e.format = reader->Format();
    std::vector<int32_t> block(4096 * (size_t)e.format.channels);
    while (size_t n = reader->Read(block.data(), 4096))
        e.samples.insert(e.samples.end(), block.begin(), block.begin() + (ptrdiff_t)(n * e.format.channels));
    e.damaged = reader->Damaged();
    e.digest = reader->Digest();
    return e;
}

Exact ReadAll(const std::wstring& path) {
    std::wstring error;
    return ReadAll(OpenPcmReader(path, error));
}

// A test signal at `bits` precision: tones and a little noise per channel, full-scale extremes at
// the start, a silent stretch (constant frames), a stretch with three zero low bits (wasted bits),
// and a near-silent stretch broken by one full-scale impulse, which forces Rice escapes.
std::vector<int32_t> Signal(unsigned bits, unsigned channels, size_t frames, uint32_t seed = 1) {
    std::vector<int32_t> v(frames * channels);
    const int64_t high = (int64_t(1) << (bits - 1)) - 1, low = -high - 1;
    uint32_t x = 2463534242u ^ seed;
    for (size_t i = 0; i < frames; ++i)
        for (unsigned c = 0; c < channels; ++c) {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            const double noise = (double)(x >> 8) / 16777216.0 - 0.5, t = (double)i / 44100;
            double s = 0.6 * std::sin(2 * 3.14159265358979 * (220 + 110 * c) * t) + 0.2 * std::sin(2 * 3.14159265358979 * 3000 * t + c);
            if (i >= frames * 4 / 5 && i < frames * 4 / 5 + 3000) s = noise * 0.0002;
            int64_t q = bits <= 2 ? (int64_t)(x >> 30) % (high - low + 1) + low : std::llround((s + noise * 0.01) * (double)high);
            q = std::clamp(q, low, high);
            if (i >= frames / 4 && i < frames / 4 + 5000) q = 0;
            if (i >= frames / 2 && i < frames / 2 + 5000 && bits > 4) q &= ~int64_t(7);
            v[i * channels + c] = (int32_t)q;
        }
    for (unsigned c = 0; frames > 2 && c < channels; ++c) {
        v[c] = (int32_t)low;
        v[channels + c] = (int32_t)high;
        if (frames > 8000) v[(frames * 4 / 5 + 1500) * channels + c] = (int32_t)(c & 1 ? low : high);
    }
    return v;
}

// Encodes the samples and reads them back exactly: every sample, the format and the MD5 must hold.
bool RoundTrip(const std::wstring& path, const std::vector<int32_t>& samples, unsigned channels, unsigned bits,
               uint32_t mask = 0, const cord::EncodeOptions& options = {}) {
    DeleteFileW(path.c_str());
    MemoryPcm source(samples, channels, bits, mask);
    std::wstring error;
    if (!cord::Encode(source, path, error, nullptr, options)) return false;
    const Exact e = ReadAll(path);
    DeleteFileW(path.c_str());
    return e.opened && !e.damaged && e.samples == samples && e.format.bits == bits && e.format.channels == channels &&
           e.format.channelMask == mask && e.format.rate == 44100 && e.digest == source.Digest() &&
           e.format.md5 == source.Digest();
}

// A chunk spliced in right after INFO, the way a later version might add one.
std::string WithChunk(const std::string& file, const char (&id)[5], const std::string& payload) {
    const size_t afterInfo = cord::kHeaderBytes + cord::kChunkHeaderBytes + cord::kInfoBytes;
    return file.substr(0, afterInfo) + cord::ChunkHeader(cord::FourCc(id), payload.size()) + payload + file.substr(afterInfo);
}

}  // namespace

void TestCordFormat(const std::wstring& root, const std::wstring& scratch) {
    const std::wstring path = scratch + L"\\cord-test.cord";

    // Each batch reports one check over the assertions it holds, so a scenario fails as a whole
    // while every case still logs its own message and location.
    testlog::Batch batch;
    batch.Next(L"cord round trips every bit depth, channel count and speaker mask, lengths around frame "
               L"and block edges, full-scale noise, and every cascade encoded on several threads");

    // Round trips: every bit depth, the channel counts, and lengths around frame and block edges.
    std::wstring failed;
    for (unsigned bits : {1u, 4u, 8u, 12u, 16u, 20u, 24u, 31u, 32u})
        for (unsigned channels : {1u, 2u})
            if (!RoundTrip(path, Signal(bits, channels, 40000, bits * 7 + channels), channels, bits))
                failed += L" " + std::to_wstring(bits) + L"-bit/" + std::to_wstring(channels) + L"ch";
    Check(failed.empty(), L"cord round trips 1- to 32-bit mono and stereo exactly, MD5 included%ls", failed.c_str());
    failed.clear();
    for (size_t frames : {0, 1, 2, 31, 4095, 4096, 4097, 16383, 16384, 16385, 32768, 32769})
        if (!RoundTrip(path, Signal(16, 2, frames), 2, 16)) failed += L" " + std::to_wstring(frames);
    Check(failed.empty(), L"cord round trips lengths around frame and block edges%ls", failed.c_str());
    failed.clear();
    for (auto [channels, mask] : {std::pair{3u, 0x7u}, {6u, 0x3Fu}, {8u, 0x63Fu}, {5u, 0u}})
        if (!RoundTrip(path, Signal(24, channels, 20000, channels), channels, 24, mask))
            failed += L" " + std::to_wstring(channels) + L"ch";
    Check(failed.empty(), L"cord round trips 3 to 8 channels with their speaker masks%ls", failed.c_str());
    // The decoder has a fast path for the encoder's own cascade and a general one for every other
    // the format allows: no cascade, a cross filter alone, three stages up to the longest filter.
    // Each must give the samples back exactly, as must encoding on several threads.
    failed.clear();
    cord::CascadeSpec plain, crossOnly, deep;
    crossOnly.crossTaps = 48;
    crossOnly.crossShift = 13;
    crossOnly.scale = 2;
    deep.stages = 3;
    deep.taps[0] = 2048; deep.shifts[0] = 15;
    deep.taps[1] = 32; deep.shifts[1] = 11;
    deep.taps[2] = 16; deep.shifts[2] = 1;
    deep.crossTaps = 32;
    deep.crossShift = 12;
    deep.scale = 1;
    for (const cord::CascadeSpec* spec : {&plain, &crossOnly, &deep})
        for (unsigned channels : {1u, 2u})
            for (unsigned bits : {16u, 24u}) {
                cord::EncodeOptions options;
                options.cascade = spec;
                options.threads = 3;
                if (!RoundTrip(path, Signal(bits, channels, 140000, bits + channels), channels, bits, 0, options))
                    failed += L" " + std::to_wstring(spec->stages) + L"/" + std::to_wstring(spec->crossTaps) + L" " +
                              std::to_wstring(bits) + L"-bit/" + std::to_wstring(channels) + L"ch";
            }
    Check(failed.empty(), L"cord round trips every kind of cascade, encoded on several threads%ls", failed.c_str());
    std::vector<int32_t> noise(2 * 30000);
    uint32_t x = 88172645u;
    for (auto& s : noise) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        s = (int32_t)x;
    }
    std::vector<int32_t> noise16(noise.size());
    for (size_t i = 0; i < noise.size(); ++i) noise16[i] = noise[i] >> 16;
    Check(RoundTrip(path, noise, 2, 32) && RoundTrip(path, noise16, 2, 16), L"cord round trips full-scale 16- and 32-bit noise");

    batch.Next(L"sine.flac converts to cord with every sample intact, in fewer bytes, and reads back "
               L"its technical tags and audio hash");

    // A FLAC fixture through the exact reader: the same samples, a smaller file, and the same
    // audio hash, since both keep the MD5 of the samples in FLAC's layout.
    const std::wstring flac = root + L"\\media\\sine.flac", copy = scratch + L"\\cord-sine.cord";
    DeleteFileW(copy.c_str());
    std::wstring error;
    auto source = OpenPcmReader(flac, error);
    const bool encoded = source && cord::Encode(*source, copy, error);
    const Exact original = ReadAll(flac), converted = ReadAll(copy);
    WIN32_FILE_ATTRIBUTE_DATA flacSize{}, cordSize{};
    GetFileAttributesExW(flac.c_str(), GetFileExInfoStandard, &flacSize);
    GetFileAttributesExW(copy.c_str(), GetFileExInfoStandard, &cordSize);
    Check(encoded && converted.opened && !converted.damaged && !original.samples.empty() &&
              converted.samples == original.samples && converted.format.bits == original.format.bits,
          L"sine.flac converts to cord with every sample intact %ls", error.c_str());
    Check(cordSize.nFileSizeLow && cordSize.nFileSizeLow < flacSize.nFileSizeLow,
          L"cord stores sine.flac's 24-bit audio in fewer bytes than its FLAC (%lu vs %lu)", cordSize.nFileSizeLow,
          flacSize.nFileSizeLow);
    TagData flacTags, cordTags;
    ReadTags(flac, flacTags);
    ReadTags(copy, cordTags);
    Check(cordTags.codec == Codec::Cord && cordTags.audioHash && cordTags.audioHash == flacTags.audioHash &&
              cordTags.sampleRate == 44100 && cordTags.channels == 2 && std::fabs(cordTags.duration - 8.0) < 1e-9 &&
              std::wstring(CodecName(Codec::Cord)) == L"Cord",
          L"cord's technical tags read back, with the same audio hash as the FLAC it came from");

    batch.Next(L"cord playback: float output is the integers scaled, seeks land on the exact frame and refuse "
               L"non-finite positions, and six channels play through the shared stereo downmix");

    // Playback: float output is the integers scaled, seeks land on the exact frame, and the
    // decoder refuses non-finite seeks and reads nothing past the end.
    auto decoder = OpenCordDecoder(copy);
    std::vector<float> floats;
    if (decoder) {
        float block[4096 * 2];
        while (size_t n = decoder->Read(block, 1000)) floats.insert(floats.end(), block, block + n * 2);
    }
    const float scale = std::ldexp(1.0f, 1 - (int)original.format.bits);
    bool scaled = !floats.empty() && floats.size() == original.samples.size();
    for (size_t i = 0; scaled && i < floats.size(); ++i) scaled = floats[i] == (float)original.samples[i] * scale;
    Check(scaled, L"cord's float output is its exact integers, scaled");
    bool seeks = decoder != nullptr;
    const uint64_t total = original.samples.size() / 2;
    uint32_t r = 26101;
    for (int i = 0; seeks && i < 60; ++i) {
        r = r * 1664525 + 1013904223;
        const uint64_t at = i == 0 ? 0 : i == 1 ? 16384 : i == 2 ? 16383 : i == 3 ? total - 10 : r % total;
        float block[300 * 2];
        const size_t want = (size_t)std::min<uint64_t>(300, total - at);
        seeks = decoder->Seek((at + 0.25) / 44100) && decoder->Read(block, 300) == want &&
                std::equal(block, block + want * 2, floats.begin() + (ptrdiff_t)(at * 2));
    }
    float none[8];
    Check(seeks && decoder->Read(none, 0) == 0 && decoder->Seek(decoder->Duration()) && decoder->Read(none, 4) == 0 &&
              !decoder->Seek(std::numeric_limits<double>::quiet_NaN()) &&
              !decoder->Seek(std::numeric_limits<double>::infinity()),
          L"cord seeks land on the exact frame and refuse non-finite positions");
    decoder.reset();
    // More than two channels play through the same downmix as every other decoder.
    const std::vector<int32_t> six = Signal(24, 6, 5000, 6);
    DeleteFileW(path.c_str());
    MemoryPcm sixSource(six, 6, 24, 0x3F);
    bool mixed = cord::Encode(sixSource, path, error);
    if (auto d = mixed ? OpenCordDecoder(path) : nullptr) {
        std::vector<float> out(5000 * 2);
        mixed = d->Read(out.data(), 5000) == 5000;
        for (size_t i = 0; mixed && i < 5000; ++i) {
            float channels[8], expect[2];
            for (unsigned c = 0; c < 6; ++c) channels[c] = (float)six[i * 6 + c] * std::ldexp(1.0f, -23);
            nativeaudio::Mix(channels, 6, expect);
            mixed = out[i * 2] == expect[0] && out[i * 2 + 1] == expect[1];
        }
    } else {
        mixed = false;
    }
    Check(mixed, L"cord plays 6 channels through the shared stereo downmix");

    batch.Next(L"cord refuses a bad index, a cut end, version 0 or 2 and the retired codec, reads "
               L"damaged blocks and a wrong MD5 as damaged, and reads a later minor version and an "
               L"unknown chunk");

    // Damage: a flipped bit in a block, a wrong INDX entry, a cut-short file, a wrong stored MD5,
    // a frozen-format version, a retired codec. An unknown chunk is skipped.
    const std::string good = ReadTestBytes(copy);
    testlog::Cases damage;
    auto test = [&](std::string bytes, const wchar_t* what, bool opens, bool damaged) {
        WriteTestBytes(path, bytes);
        const Exact e = ReadAll(path);
        damage.Add(e.opened == opens && (!opens || e.damaged == damaged) && (opens || !OpenCordDecoder(path)),
                   std::wstring(what) + L" should be " + (opens ? (damaged ? L"read as damaged" : L"read") : L"refused"));
    };
    if (good.size() > 10000) {
        std::string bad = good;
        bad[bad.size() - 5000] ^= 0x20;
        test(bad, L"a flipped bit in a block", true, true);
        const size_t index = cord::kHeaderBytes + cord::kChunkHeaderBytes + cord::kInfoBytes + cord::kChunkHeaderBytes + 4096 +
                             cord::kChunkHeaderBytes;
        bad = good;
        bad[index] ^= 1;
        test(bad, L"a wrong INDX entry", false, false);
        test(good.substr(0, good.size() - 100), L"its end cut off", false, false);
        bad = good;
        bad[cord::kHeaderBytes + cord::kChunkHeaderBytes + 36] ^= 1;
        test(bad, L"a wrong stored MD5", true, true);
        // Version 1 is frozen: the development bitstream (0) and a later major version are refused,
        // and a later minor version, which may only add what readers skip, still reads.
        bad = good;
        bad[4] = 0;
        test(bad, L"the development bitstream, major version 0", false, false);
        bad[4] = 2;
        test(bad, L"major version 2", false, false);
        bad = good;
        bad[5] = 7;
        test(bad, L"minor version 7", true, false);
        bad = good;
        bad[cord::kHeaderBytes + cord::kChunkHeaderBytes + 3] = '2';
        test(bad, L"the retired CL02 codec", false, false);
        test(WithChunk(good, "XTRA", "later data"), L"an unknown chunk", true, false);
        Check(damage.Passed(), L"cord refuses a bad index, a cut end, version 0 or 2 and the retired codec; reads damaged "
              L"blocks and a wrong MD5 as damaged; reads a later minor version and an unknown chunk%ls",
              damage.failed.c_str());
    } else {
        Check(false, L"cord has a fixture large enough for the damage cases");
    }

    // The format's own tags: the whole edit matrix the other writers pass, in place when they fit.
    const std::wstring tagged = scratch + L"\\cord-tags.cord";
    DeleteFileW(tagged.c_str());
    CopyFileW(copy.c_str(), tagged.c_str(), FALSE);
    // TestTagWriterFiles reports its own checks, so no batch may be active across the call.
    batch.End();
    TestTagWriterFiles({tagged}, g_log, L"tag writing: cordinal format");
    batch.Next(L"cord keeps free-form entries, unknown fields and chunks through an edit, stores "
               L"ReplayGain as numbers, and a failing edit leaves the file byte-identical with its "
               L"samples intact");
    // Free-form entries carry what cordinal has no field for, and survive an edit; an unknown
    // typed field and an unknown chunk are kept too. ReplayGain is stored as numbers.
    std::string meta;
    cord::PutEntry(meta, cord::kTitle, {}, "Typed");
    cord::PutEntry(meta, cord::kFreeForm, "MUSICBRAINZ_TRACKID", "abc-123");
    cord::PutEntry(meta, cord::kFreeForm, "LOOPSTART", "1000");
    cord::PutEntry(meta, cord::kFreeForm, "LOOPEND", "200000");
    cord::PutEntry(meta, cord::kFreeForm, "CHAPTER001", "00:00:02.500");
    cord::PutEntry(meta, cord::kFreeForm, "CHAPTER001NAME", "Second part");
    cord::PutEntry(meta, 200, {}, "from the future");
    WriteTestBytes(tagged, WithChunk(WithChunk(good, "XTRA", "later data"), "META", meta));
    TagEdit edit;
    edit.artist = "Edited";
    edit.rgTrackGain = "-6.50 dB";
    edit.rgTrackPeak = "0.988000";
    const bool wrote = WriteTags(tagged, edit, error);
    TagData t;
    ReadTags(tagged, t);
    int64_t loopStart = 0, loopEnd = 0;
    std::vector<Chapter> chapters;
    const std::string after = ReadTestBytes(tagged);
    Check(wrote && t.title == "Typed" && t.artist == "Edited" && std::fabs(t.rgTrackGain + 6.5f) < 1e-6f &&
              std::fabs(t.rgTrackPeak - 0.988f) < 1e-6f && ReadLoopPoints(tagged, loopStart, loopEnd) && loopStart == 1000 &&
              loopEnd == 200000 && ReadChapters(tagged, chapters) && chapters.size() == 1 && chapters[0].title == "Second part" &&
              after.find("MUSICBRAINZ_TRACKID") != std::string::npos && after.find("from the future") != std::string::npos &&
              after.find("later data") != std::string::npos,
          L"cord keeps free-form entries, unknown fields and chunks through an edit, and stores ReplayGain %ls", error.c_str());
    edit = TagEdit();
    edit.rgAlbumGain = "loud";
    Check(!WriteTags(tagged, edit, error) && ReadTestBytes(tagged) == after,
          L"a cord edit that fails leaves the file byte-identical");
    const Exact retagged = ReadAll(tagged);
    Check(!retagged.damaged && retagged.samples == original.samples, L"cord tag edits leave the samples bit-identical");

    batch.Next(L"cord files are library audio, and encoding never overwrites an existing file");

    // The library sees .cord files, and the encoder never replaces an existing one.
    MemoryPcm again(noise16, 2, 16);
    Check(IsSupportedAudioFile(L"x.cord") && CanWriteTags(L"x.cord") && !cord::Encode(again, tagged, error) &&
              ReadTestBytes(tagged) == after && GetFileAttributesW((tagged + L".cordinal-tmp").c_str()) == INVALID_FILE_ATTRIBUTES,
          L"cord files are library audio, and encoding never overwrites an existing file");
    DeleteFileW(path.c_str());
    DeleteFileW(copy.c_str());
    DeleteFileW(tagged.c_str());
}

// Converting to .cord (convert.h), without the Recycle Bin: the tests write only into their own
// folder. Tags carried over whole, samples verified, the files that can't convert left alone, the
// log that undoes it, and old logs forgotten.
void TestCordConversion(const std::wstring& root, const std::wstring& scratch) {
    const std::wstring dir = scratch + L"\\cord-convert", logDir = dir + L"\\log";
    DeleteTestFolder(dir);
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring tagged = dir + L"\\tagged.flac", gains = dir + L"\\gains.flac", wave = dir + L"\\plain.wav",
                       broken = dir + L"\\broken.flac", lossy = dir + L"\\lossy.mp3";
    CopyFileW((root + L"\\tags2\\full.flac").c_str(), tagged.c_str(), FALSE);
    CopyFileW((root + L"\\rg\\rg.flac").c_str(), gains.c_str(), FALSE);
    CopyFileW((root + L"\\media\\sine.wav").c_str(), wave.c_str(), FALSE);
    CopyFileW((root + L"\\media\\sine.mp3").c_str(), lossy.c_str(), FALSE);
    const std::string whole = ReadTestBytes(root + L"\\media\\sine.flac");
    WriteTestBytes(broken, whole.substr(0, whole.size() * 2 / 3));
    // A cover, so FLAC's PICTURE block has to come across too.
    const std::string cover(
        "\x89" "PNG\r\n\x1a\n\x00\x00\x00\x0d" "IHDR\x00\x00\x00\x02\x00\x00\x00\x02\x08\x02\x00\x00\x00"
        "\xfd\xd4\x9a\x73\x00\x00\x00\x14" "IDAT\x78\x9c\x63\xf8\xdf\xc0\xf0\xbf\x81\x01\x84\xff\x37\x30\x00\x00"
        "\x2d\xee\x05\xfd\x2e\xeb\x32\xac\x00\x00\x00\x00" "IEND\xae\x42\x60\x82", 77);
    TagEdit edit;
    edit.cover = cover;
    edit.title = "Convert me";
    edit.rgTrackGain = "-6.50 dB";
    edit.rgAlbumPeak = "0.995000";
    std::wstring error;
    const bool covered = WriteTags(tagged, edit, error);

    testlog::Batch batch;
    batch.Next(L"conversion plans FLAC and WAV only, converts the readable files with their samples and "
               L"every tag, leaves nothing for a damaged one, and keeps originals when asked");

    const auto plan = PlanConvert({{1, tagged}, {2, gains}, {3, wave}, {4, broken}, {5, lossy}, {6, dir + L"\\x.cord"},
                                   {7, dir + L"\\cue.flac|2"}});
    Check(plan.size() == 7 && plan[0].skip.empty() && plan[0].to == dir + L"\\tagged.cord" && plan[1].skip.empty() &&
              plan[2].skip.empty() && plan[3].skip.empty() && !plan[4].skip.empty() && plan[5].skip == L"already .cord" &&
              plan[6].skip == L"part of a cue sheet",
          L"conversion plans FLAC and WAV, and leaves lossy files, .cord files and cue sheet parts alone");

    std::atomic<bool> cancel{false};
    size_t started = 0;
    ConvertSettings settings;
    settings.recycle = false;
    settings.threads = 2;
    const ConvertResult r = ExecuteConvert(plan, settings, logDir, cancel, [&](size_t) { ++started; });
    const std::wstring cord = dir + L"\\tagged.cord";
    Check(r.done.size() == 3 && r.failed.size() == 1 && r.failed[0].first == broken && started == 4 && !r.cancelled &&
              !r.log.empty() && GetFileAttributesW((dir + L"\\broken.cord").c_str()) == INVALID_FILE_ATTRIBUTES &&
              GetFileAttributesW(tagged.c_str()) != INVALID_FILE_ATTRIBUTES && !r.done[0].recycled,
          L"conversion converts the readable files, leaves nothing for a damaged one, and keeps originals when asked");

    const Exact before = ReadAll(tagged), after = ReadAll(cord), waveBefore = ReadAll(wave), waveAfter = ReadAll(dir + L"\\plain.cord");
    Check(before.opened && after.opened && !after.damaged && after.samples == before.samples && waveAfter.samples == waveBefore.samples &&
              !waveAfter.samples.empty(),
          L"converted files hold their originals' samples exactly");
    TagData a, b, c, d;
    ReadTags(tagged, a);
    ReadTags(cord, b);
    ReadTags(gains, c);
    ReadTags(dir + L"\\gains.cord", d);
    std::string art, lyrics;
    const bool hasEncoder = ReadTestBytes(tagged).find("encoder=") != std::string::npos;
    const std::string cordBytes = ReadTestBytes(cord);
    Check(covered && b.title == "Convert me" && b.composer == a.composer && b.comment == a.comment && b.grouping == a.grouping &&
              b.label == a.label && b.track == 3 && b.trackTotal == 12 && b.disc == 2 && b.discTotal == 3 && b.bpm == 128 &&
              b.compilation && ReadEmbeddedArt(cord, art) && art == cover && ReadLyrics(cord, lyrics) && lyrics == "La la la" &&
              std::fabs(d.rgTrackGain - c.rgTrackGain) < 1e-4f && std::fabs(d.rgAlbumPeak - c.rgAlbumPeak) < 1e-6f &&
              (!hasEncoder || cordBytes.find("ENCODER") != std::string::npos) && b.audioHash == a.audioHash,
          L"conversion carries every tag over: fields, ReplayGain, lyrics, the cover and comments it has no field for");

    batch.Next(L"conversion undo returns the originals and forgets its log; a cancelled run or an "
               L"unwritable log converts nothing and keeps the original, none replaces an existing "
               L".cord, and old logs are forgotten");

    // Undo without the Recycle Bin: the originals were kept, so the log is done with at once.
    const auto undone = UndoLastConvert(logDir, false, error);
    Check(undone.size() == 3 && !undone[0].removed && error.empty() && !CanUndoConvert(logDir) &&
              GetFileAttributesW(cord.c_str()) != INVALID_FILE_ATTRIBUTES,
          L"undoing a conversion returns its originals and forgets its log %ls", error.c_str());

    // A cancelled run converts nothing; an existing .cord is never planned over.
    cancel = true;
    const std::wstring fresh = dir + L"\\fresh.flac";
    CopyFileW(gains.c_str(), fresh.c_str(), FALSE);
    const ConvertResult none = ExecuteConvert(PlanConvert({{1, fresh}}), settings, logDir, cancel, nullptr);
    Check(none.cancelled && none.done.empty() && PlanConvert({{1, tagged}})[0].skip == L"a .cord file of that name already exists",
          L"a cancelled conversion converts nothing, and none replaces an existing .cord");

    // Without an undo log the original must not leave its folder, even when recycling is requested.
    cancel = false;
    settings.recycle = true;
    const std::wstring blockedLogs = dir + L"\\blocked-log";
    const std::wstring noLogSource = dir + L"\\nolog.flac";
    WriteTestBytes(blockedLogs, "a regular file, not a log directory");
    CopyFileW(gains.c_str(), noLogSource.c_str(), FALSE);
    const ConvertResult noLog = ExecuteConvert(PlanConvert({{1, noLogSource}}), settings, blockedLogs, cancel, nullptr);
    Check(noLog.done.size() == 1 && !noLog.done[0].recycled && !noLog.logError.empty() && noLog.log.empty() &&
              GetFileAttributesW(noLogSource.c_str()) != INVALID_FILE_ATTRIBUTES &&
              GetFileAttributesW((dir + L"\\nolog.cord").c_str()) != INVALID_FILE_ATTRIBUTES,
          L"conversion keeps the original when its undo log can't be written");
    settings.recycle = false;

    // Logs older than the chosen memory are forgotten.
    CreateDirectoryW(logDir.c_str(), nullptr);
    const std::wstring old = logDir + L"\\20000101-000000-000.log";
    WriteTestBytes(old, "a.cord\ta.flac\t0\r\n");
    HANDLE h = CreateFileW(old.c_str(), FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    FILETIME then{};
    SYSTEMTIME y2k{2000, 1, 0, 1};
    SystemTimeToFileTime(&y2k, &then);
    if (h != INVALID_HANDLE_VALUE) {
        SetFileTime(h, &then, nullptr, nullptr);
        CloseHandle(h);
    }
    const bool remembered = CanUndoConvert(logDir);
    PruneConvertLogs(logDir, 49);
    Check(remembered && !CanUndoConvert(logDir), L"conversion logs older than the undo memory are forgotten");

    batch.End();

    // The FLAC escape route has only the .cord files: no originals, no journal, even when the
    // conversion preference would normally recycle originals. These exports always keep them.
    if (!Ready(DeleteFileW(tagged.c_str()) && DeleteFileW(gains.c_str()) && DeleteFileW(wave.c_str()),
               L"remove scratch originals before exporting .cord")) return;
    batch.Next(L"FLAC export restores audio, tags, covers and ReplayGain from the .cord files without "
               L"originals or undo history, and keeps lossless precision and tags across rates, "
               L"channels and final blocks");
    cancel = false;
    settings.recycle = true;
    const auto exports = PlanConvert({{1, cord}, {2, dir + L"\\gains.cord"}, {3, dir + L"\\plain.cord"}}, CordExport::Flac);
    const ConvertResult exported = ExecuteConvert(exports, settings, logDir, cancel, nullptr);
    testlog::Cases restored;
    for (const auto& item : exports) {
        std::vector<std::string> originalArt, exportedArt;
        const std::string exportedTags = CordTagsFrom(item.to, exportedArt);
        // Extract the .cord's META directly: exporting should reconstruct the same fields,
        // including comments which the library doesn't display.
        std::string meta;
        const std::string sourceBytes = ReadTestBytes(item.from);
        for (size_t at = cord::kHeaderBytes; at + cord::kChunkHeaderBytes <= sourceBytes.size();) {
            const auto* p = (const uint8_t*)sourceBytes.data() + at;
            const uint32_t id = cord::Le32(p);
            const size_t n = (size_t)cord::Le64(p + 4);
            at += cord::kChunkHeaderBytes;
            if (n > sourceBytes.size() - at) break;
            if (id == cord::kMeta) meta.append(sourceBytes, at, n);
            if (id == cord::kPicture) originalArt.push_back(sourceBytes.substr(at, n));
            at += n;
        }
        const Exact x = ReadAll(item.from), y = ReadAll(item.to);
        restored.Add(y.opened && !y.damaged && y.samples == x.samples && y.digest == x.digest &&
                         exportedTags == meta && originalArt == exportedArt && ReadTestBytes(item.from) == sourceBytes,
                     FileNameOf(item.from));
    }
    Check(exported.done.size() == 3 && exported.failed.empty() && exported.log.empty() && !CanUndoConvert(logDir) && restored.Passed(),
          L"FLAC export restores audio, tags, covers and ReplayGain without originals or undo history%ls", restored.failed.c_str());

    testlog::Cases formats;
    for (const auto [bits, channels, frames, rate] : {std::tuple{8u, 1u, 1u, 44100u}, {16u, 2u, 8301u, 44100u},
          {20u, 3u, 8192u, 12345u}, {24u, 6u, 8193u, 192000u}, {32u, 8u, 8301u, 123450u},
          {4u, 2u, 4097u, 123451u}, {16u, 1u, 0u, 44100u}}) {
        const std::wstring name = std::to_wstring(bits) + L"-" + std::to_wstring(channels) + L"-" + std::to_wstring(frames);
        const std::wstring from = dir + L"\\export-" + name + L".cord", to = dir + L"\\export-" + name + L".flac";
        const auto samples = Signal(bits, channels, frames);
        const uint32_t mask = channels == 6 ? 0x3F : 0;
        MemoryPcm pcm(samples, channels, bits, mask, rate);
        cord::EncodeOptions tags;
        cord::PutEntry(tags.meta, cord::kArtist, {}, "Björk");
        cord::PutEntry(tags.meta, cord::kArtist, {}, "Second artist");
        cord::PutEntry(tags.meta, cord::kFreeForm, "MUSICBRAINZ_TRACKID", "keep this id");
        const bool encoded = cord::Encode(pcm, from, error, nullptr, tags);
        const bool saved = encoded && ExportFlac(from, to, error, cancel);
        const Exact back = ReadAll(to);
        const std::string flacBytes = ReadTestBytes(to);
        formats.Add(saved && back.opened && !back.damaged && back.samples == samples && back.format.bits == bits && back.format.rate == rate &&
                        back.format.channels == channels && ReadTestBytes(to).find("ARTIST=Björk") != std::string::npos &&
                        flacBytes.find("ARTIST=Second artist") != std::string::npos &&
                        flacBytes.find("MUSICBRAINZ_TRACKID=keep this id") != std::string::npos &&
                        (!mask || flacBytes.find("WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x3f") != std::string::npos),
                    name + L" " + error);
        const std::wstring wav = dir + L"\\export-" + name + L".wav";
        const bool waveSaved = ExportAudio(from, wav, CordExport::Wav, error, cancel);
        const Exact waveBack = ReadAll(wav);
        formats.Add(waveSaved && waveBack.opened && !waveBack.damaged && waveBack.samples == samples && waveBack.format.bits == bits &&
                        waveBack.format.rate == rate && waveBack.format.channels == channels && waveBack.format.channelMask == mask &&
                        ReadTestBytes(wav).find("MUSICBRAINZ_TRACKID") != std::string::npos,
                    L"WAV " + name + L" " + error);
        if (bits == 16 && channels == 2 && frames) {
            // This signal has full-scale impulses, silence, noise and a partial final block:
            // exercise the native lossy encoders beyond the steady fixture used below.
            for (const auto target : {CordExport::Mp3_192, CordExport::Mp3_320, CordExport::Aac128, CordExport::Aac192}) {
                const auto& info = kCordExports[(unsigned)target];
                const auto transientCopy = dir + L"\\transients-" + std::to_wstring((unsigned)target) + L"." + info.extension;
                const bool made = ExportAudio(from, transientCopy, target, error, cancel);
                const std::string output = ReadTestBytes(transientCopy);
                formats.Add(made && output.find("MUSICBRAINZ_TRACKID") != output.npos && output.find("Björk") != output.npos &&
                                output.find("Second artist") != output.npos, std::wstring(info.label) + L" transients " + error);
            }
        }
    }
    Check(formats.Passed(), L"exports preserve lossless precision and tags across rates, channels, transients and final blocks%ls", formats.failed.c_str());

    batch.Next(L"the built-in FLAC encoder decodes back exactly at every depth, channel count and block "
               L"length, codes matching channels as side, and predicts tones in a fraction of their size");

    // The stream as the encoder alone makes it: STREAMINFO (the last metadata block), then frames.
    auto encodeFlac = [](const std::wstring& path, const std::vector<int32_t>& samples, unsigned channels,
                         unsigned bits, unsigned rate) {
        MemoryPcm pcm(samples, channels, bits, 0, rate);
        cordinal::encode::FlacEncoder encoder(rate, channels, bits);
        std::string body;
        std::vector<int32_t> block(cordinal::encode::FlacEncoder::kBlockFrames * channels);
        while (const size_t n = pcm.Read(block.data(), cordinal::encode::FlacEncoder::kBlockFrames))
            encoder.Frame(block.data(), n, body);
        const bool sized = samples.empty() || (encoder.MinFrameBytes() && encoder.MinFrameBytes() <= encoder.MaxFrameBytes());
        const std::string file = std::string("fLaC\x80\0\0\x22", 8) +
                                 cordinal::encode::FlacStreamInfo(rate, channels, bits, samples.size() / channels, encoder.MinFrameBytes(),
                                                                  encoder.MaxFrameBytes(), pcm.Digest()) + body;
        WriteTestBytes(path, file);
        const Exact back = ReadAll(path);
        return std::pair{sized && back.opened && !back.damaged && back.samples == samples && back.digest == pcm.Digest() &&
                             back.format.bits == bits && back.format.channels == channels && back.format.rate == rate,
                         file};
    };
    testlog::Cases flac;
    for (const auto [bits, channels, frames, rate] : {std::tuple{16u, 2u, 50000u, 44100u}, {24u, 2u, 12289u, 96000u},
          {32u, 2u, 9000u, 44100u}, {31u, 2u, 9000u, 48000u}, {12u, 1u, 4096u, 8000u}, {20u, 5u, 9000u, 22050u},
          {4u, 2u, 300u, 11025u}, {8u, 2u, 17u, 44100u}, {16u, 1u, 1u, 44100u}, {16u, 8u, 4097u, 705600u}}) {
        const std::wstring name = std::to_wstring(bits) + L"-" + std::to_wstring(channels) + L"-" + std::to_wstring(frames);
        flac.Add(encodeFlac(dir + L"\\encoder-" + name + L".flac", Signal(bits, channels, frames, 7), channels, bits, rate).first, name);
    }
    // Identical channels: the second is all zeros as side, so every frame uses a side mode (8..10).
    std::vector<int32_t> twin = Signal(16, 1, 20000, 3), twins(twin.size() * 2);
    for (size_t i = 0; i < twin.size(); ++i) twins[2 * i] = twins[2 * i + 1] = twin[i];
    const auto [twinsOk, twinsFile] = encodeFlac(dir + L"\\encoder-twins.flac", twins, 2, 16, 44100);
    flac.Add(twinsOk && twinsFile.size() > 45 && ((uint8_t)twinsFile[45] >> 4) >= 8, L"side mode");
    // Three steady tones with one step of dither: under 4.5 bits a sample, which reference flac
    // missed at -5 and -8 (4.9 and 4.7) when this was written.
    std::vector<int32_t> tones(44100 * 2);
    for (size_t i = 0; i < tones.size(); ++i) {
        const double t = (double)(i / 2) / 44100;
        tones[i] = (int32_t)std::lround(9000 * std::sin(2 * 3.14159265358979 * 440 * t) + 6000 * std::sin(2 * 3.14159265358979 * 1234.5 * t + (i & 1)) +
                                        3000 * std::sin(2 * 3.14159265358979 * 5021 * t)) + (int32_t)((uint32_t)(i * 2654435761u) >> 31);
    }
    const auto [tonesOk, tonesFile] = encodeFlac(dir + L"\\encoder-tones.flac", tones, 2, 16, 44100);
    flac.Add(tonesOk && tonesFile.size() < tones.size() * 9 / 16,
             L"tones in " + std::to_wstring(tonesFile.size()) + L" of " + std::to_wstring(tones.size() * 2) + L" bytes");
    Check(flac.Passed(), L"the built-in FLAC encoder decodes back exactly and compresses%ls", flac.failed.c_str());

    batch.Next(L"WAV, MP3 and AAC copies keep tags, lyrics and covers, decode completely and never "
               L"overwrite files");

    testlog::Cases copies;
    for (const auto target : {CordExport::Wav, CordExport::Mp3_192, CordExport::Mp3_320, CordExport::Aac128, CordExport::Aac192}) {
        const auto& info = kCordExports[(unsigned)target];
        const std::wstring output = dir + L"\\copy-" + std::to_wstring((unsigned)target) + L"." + info.extension;
        const auto exportPlan = PlanConvert({{1, cord}}, target);
        const bool made = ExportAudio(cord, output, target, error, cancel);
        TagData tags;
        std::string image, words;
        ReadTags(output, tags);
        unsigned rate = 0;
        const auto decoded = DecodeAll(output, rate);
        const double duration = rate ? (double)(decoded.size() / 2) / rate : -1;
        copies.Add(made && !decoded.empty() && std::fabs(duration - a.duration) < 0.15 && tags.title == b.title &&
                       tags.composer == b.composer && tags.comment == b.comment && tags.track == b.track && tags.trackTotal == b.trackTotal &&
                       tags.disc == b.disc && tags.discTotal == b.discTotal && tags.bpm == b.bpm && tags.compilation == b.compilation &&
                       std::fabs(tags.rgTrackGain - b.rgTrackGain) < 1e-5f && std::fabs(tags.rgAlbumPeak - b.rgAlbumPeak) < 1e-6f &&
                       ReadEmbeddedArt(output, image) && image == cover && ReadLyrics(output, words) && words == "La la la" &&
                       exportPlan.size() == 1 && exportPlan[0].skip.empty() && ExtOf(exportPlan[0].to) == info.extension,
                   std::wstring(info.label) + L" " + error);
        const std::string bytes = ReadTestBytes(output);
        cancel = true;
        const bool stopped = !ExportAudio(cord, output + L".cancelled", target, error, cancel);
        cancel = false;
        copies.Add(stopped && !ExportAudio(cord, output, target, error, cancel) && ReadTestBytes(output) == bytes,
                   std::wstring(info.label) + L" cancellation and collision");
    }
    Check(copies.Passed(), L"WAV, MP3 and AAC copies keep tags, lyrics and covers, decode completely and never overwrite files%ls", copies.failed.c_str());

    batch.Next(L"FLAC export leaves sources and existing files intact, and removes cancelled or damaged "
               L"exports");

    const std::wstring safe = dir + L"\\safe.flac", damaged = dir + L"\\damaged.cord";
    const std::string preserved = ReadTestBytes(cord), existing = ReadTestBytes(tagged);
    const auto collision = PlanConvert({{1, cord}, {2, tagged}, {3, cord + L"|1"}}, CordExport::Flac);
    const bool noOverwrite = !ExportFlac(cord, tagged, error, cancel) && ReadTestBytes(tagged) == existing &&
                             !collision[0].skip.empty() && !collision[1].skip.empty() && !collision[2].skip.empty();
    cancel = true;
    const bool cancelled = !ExportFlac(cord, safe, error, cancel) && GetFileAttributesW(safe.c_str()) == INVALID_FILE_ATTRIBUTES;
    cancel = false;
    WriteTestBytes(damaged, preserved.substr(0, preserved.size() - 1));
    const bool rejected = !ExportFlac(damaged, safe, error, cancel) && GetFileAttributesW(safe.c_str()) == INVALID_FILE_ATTRIBUTES;
    // Valid container, corrupt audio: verification must also catch damage found after opening it.
    std::string corrupt = preserved;
    corrupt.back() ^= 1;
    WriteTestBytes(damaged, corrupt);
    const bool badAudio = !ExportFlac(damaged, safe, error, cancel) && GetFileAttributesW(safe.c_str()) == INVALID_FILE_ATTRIBUTES;
    const std::vector<int32_t> tiny{0, -1, 0, -1};
    MemoryPcm tooNarrow(tiny, 1, 1);
    const bool narrow = cord::Encode(tooNarrow, dir + L"\\one-bit.cord", error) &&
                        !ExportFlac(dir + L"\\one-bit.cord", safe, error, cancel) &&
                        GetFileAttributesW(safe.c_str()) == INVALID_FILE_ATTRIBUTES;
    WIN32_FIND_DATAW found{};
    HANDLE temporary = FindFirstFileW((dir + L"\\*.cordinal-tmp-*").c_str(), &found);
    if (temporary != INVALID_HANDLE_VALUE) FindClose(temporary);
    Check(noOverwrite && cancelled && rejected && badAudio && narrow && ReadTestBytes(cord) == preserved && temporary == INVALID_HANDLE_VALUE,
          L"FLAC export leaves sources and existing files intact, and removes cancelled or damaged exports");

    batch.Next(L"converted copies start from any format: exact FLAC and WAV from FLAC or WAV with every tag, "
               L"AAC from FLAC or MP3, and nothing lossless from a lossy file or into the source's own format");

    const auto anyFlac = PlanConvert({{1, tagged}, {2, dir + L"\\song.wav"}, {3, lossy}}, CordExport::Flac);
    const auto anyAac = PlanConvert({{1, tagged}, {2, lossy}, {3, dir + L"\\song.m4a"}}, CordExport::Aac128);
    testlog::Cases any;
    any.Add(anyFlac.size() == 3 && anyFlac[0].skip == L"already .flac" && anyFlac[1].skip.empty() &&
                anyFlac[1].to == dir + L"\\song.flac" && !anyFlac[2].skip.empty() && anyFlac[2].to.empty(),
            L"lossless plan");
    any.Add(anyAac.size() == 3 && anyAac[0].skip.empty() && anyAac[0].to == dir + L"\\tagged.m4a" && anyAac[1].skip.empty() &&
                anyAac[2].skip == L"already .m4a",
            L"lossy plan");
    const std::string flacBefore = ReadTestBytes(tagged), mp3Before = ReadTestBytes(lossy);
    // FLAC -> WAV -> FLAC keeps every sample, and the WAV keeps the FLAC's tags, cover and lyrics. (The
    // FLAC is the one exported from .cord above; the original WAV is gone by now.)
    const std::wstring flacWave = dir + L"\\tagged.wav", waveFlac = dir + L"\\from-wave.flac";
    const bool madeWave = ExportAudio(tagged, flacWave, CordExport::Wav, error, cancel);
    const std::wstring waveError = error;
    const bool madeFlac = ExportAudio(flacWave, waveFlac, CordExport::Flac, error, cancel);
    const std::wstring flacError = error;
    const Exact source = ReadAll(tagged), asWave = ReadAll(flacWave), asFlac = ReadAll(waveFlac);
    TagData waveTags;
    ReadTags(flacWave, waveTags);
    std::string waveArt, waveLyrics;
    any.Add(madeWave && asWave.opened && !asWave.damaged && asWave.samples == source.samples && waveTags.title == "Convert me" &&
                waveTags.composer == a.composer && waveTags.track == a.track && ReadEmbeddedArt(flacWave, waveArt) && waveArt == cover &&
                ReadLyrics(flacWave, waveLyrics) && waveLyrics == "La la la",
            L"FLAC to WAV " + waveError);
    any.Add(madeFlac && asFlac.opened && !asFlac.damaged && !asFlac.samples.empty() && asFlac.samples == source.samples,
            L"WAV to FLAC " + flacError);
    // AAC from FLAC (exact source) and from MP3 (decoded source): tags and a complete decode.
    for (const auto& [from, to] : {std::pair{tagged, dir + L"\\tagged.m4a"}, std::pair{lossy, dir + L"\\lossy.m4a"}}) {
        const bool made = ExportAudio(from, to, CordExport::Aac128, error, cancel);
        TagData fromTags, toTags;
        ReadTags(from, fromTags);
        ReadTags(to, toTags);
        unsigned fromRate = 0, toRate = 0;
        const auto original = DecodeAll(from, fromRate), copy = DecodeAll(to, toRate);
        const double want = fromRate ? (double)(original.size() / 2) / fromRate : -1, got = toRate ? (double)(copy.size() / 2) / toRate : -2;
        std::string copyArt;
        any.Add(made && !copy.empty() && std::fabs(want - got) < 0.01 && toTags.title == fromTags.title && toTags.artist == fromTags.artist &&
                    (from != tagged || (ReadEmbeddedArt(to, copyArt) && copyArt == cover)),
                FileNameOf(from) + L" to AAC " + error);
    }
    any.Add(!ExportAudio(lossy, dir + L"\\lossy.flac", CordExport::Flac, error, cancel) &&
                GetFileAttributesW((dir + L"\\lossy.flac").c_str()) == INVALID_FILE_ATTRIBUTES,
            L"no lossless copy of a lossy file");
    any.Add(ReadTestBytes(tagged) == flacBefore && ReadTestBytes(lossy) == mp3Before,
            L"sources untouched");
    Check(any.Passed(), L"converted copies start from any format, keep tags and samples, and leave their sources alone%ls",
          any.failed.c_str());

    batch.Next(L"MP3 copies are gapless: through the LAME header's delay and padding the copy lines up with the "
               L"original to the sample and is exactly as long");
    // Tones that swell and fade over 1.5 s (an odd length, so the last frame is partial). The lag at
    // which the copy best matches the original must be 0, and its length the same: this holds
    // aac::kMp3Latency to the encoder. A click would not do: the encoder's lowpass smears it.
    constexpr size_t kGapFrames = 72001;
    std::vector<int32_t> tonal(kGapFrames * 2);
    for (size_t i = 0; i < kGapFrames; ++i) {
        const double t = (double)i / 48000, swell = std::sin(3.14159265358979 * t / 1.5);
        const double v = swell * (6000 * std::sin(2 * 3.14159265358979 * 440 * t) + 3000 * std::sin(2 * 3.14159265358979 * 1250 * t) +
                                  2000 * std::sin(2 * 3.14159265358979 * 3100 * t * (1 + t)));
        tonal[i * 2] = tonal[i * 2 + 1] = (int32_t)std::lround(v);
    }
    MemoryPcm tonalPcm(tonal, 2, 16, 0, 48000);
    const std::wstring tonalCord = dir + L"\\tonal.cord";
    // The lag in [-limit, limit] at which `decoded` (left channel) best matches the source.
    auto bestLag = [&](const std::vector<float>& decoded, int limit) {
        int best = 0;
        double top = -1e300;
        for (int lag = -limit; lag <= limit; ++lag) {
            double sum = 0;
            for (size_t i = 4000; i + 4000 < kGapFrames; i += 3) {
                const int64_t j = (int64_t)i + lag;
                if (j >= 0 && (size_t)j < decoded.size() / 2) sum += tonal[i * 2] * (double)decoded[(size_t)j * 2];
            }
            if (sum > top) { top = sum; best = lag; }
        }
        return best;
    };
    testlog::Cases gapless;
    const bool tonalMade = cord::Encode(tonalPcm, tonalCord, error);
    for (const auto target : {CordExport::Mp3_192, CordExport::Mp3_320}) {
        const std::wstring mp3 = dir + L"\\tonal-" + std::to_wstring((unsigned)target) + L".mp3";
        const bool made = tonalMade && ExportAudio(tonalCord, mp3, target, error, cancel);
        unsigned rate = 0;
        const auto decoded = DecodeAll(mp3, rate);
        const int lag = bestLag(decoded, 64);
        const std::string bytes = ReadTestBytes(mp3);
        gapless.Add(made && rate == 48000 && decoded.size() / 2 == kGapFrames && lag == 0 && bytes.find("LAMEcord1") != bytes.npos,
                    std::wstring(kCordExports[(unsigned)target].label) + L": " + std::to_wstring(decoded.size() / 2) + L" frames, lag " +
                        std::to_wstring(lag) + L" " + error);
    }
    Check(gapless.Passed(), L"MP3 copies line up with the original to the sample and are exactly as long%ls", gapless.failed.c_str());

    batch.Next(L"ALAC, WavPack and TTA read exactly and convert losslessly; copies go into another folder with "
               L"their albums apart, or as .cord; a replacement to FLAC is logged and undone");
    testlog::Cases more;
    // alac.caf is sine.wav through FFmpeg's ALAC encoder, so their samples must match exactly.
    const Exact alac = ReadAll(root + L"\\native\\alac.caf"), pcm16 = ReadAll(root + L"\\media\\sine.wav");
    more.Add(alac.opened && !alac.damaged && !alac.samples.empty() && alac.samples == pcm16.samples && alac.format.bits == pcm16.format.bits,
             L"ALAC reads exactly");
    for (const wchar_t* name : {L"pcm16.wv", L"sine.tta", L"alac.caf"}) {
        const std::wstring from = dir + L"\\lossless-" + name, toFlac = from + L".flac";
        CopyFileW((root + L"\\native\\" + name).c_str(), from.c_str(), FALSE);
        const Exact exact = ReadAll(from);
        const bool made = ExportAudio(from, toFlac, CordExport::Flac, error, cancel);
        const Exact flacBack = ReadAll(toFlac);
        more.Add(exact.opened && !exact.damaged && !exact.samples.empty() && made && flacBack.samples == exact.samples &&
                     flacBack.format.bits == exact.format.bits && flacBack.format.channels == exact.format.channels &&
                     (wcscmp(name, L"pcm16.wv") || exact.samples == pcm16.samples),
                 std::wstring(name) + L" to FLAC " + error);
    }
    // sine.wv holds float samples, which no integer format keeps exactly: refused, not rounded.
    more.Add(!ReadAll(root + L"\\native\\sine.wv").opened, L"float WavPack is not read as exact");
    // MP4 holds ALAC or AAC: the library's codec decides without opening the file.
    const auto aacOrAlac = PlanConvert({{1, dir + L"\\a.m4a"}, {2, dir + L"\\b.m4a"}}, CordExport::Flac, ConvertMode::Copy, {},
                                       [](TrackId id) { return id == 1 ? Codec::AAC : Codec::ALAC; });
    more.Add(aacOrAlac.size() == 2 && !aacOrAlac[0].skip.empty() && aacOrAlac[1].skip.empty(), L"ALAC or AAC in .m4a");
    // Copies into a folder keep the folders below the selection's common folder; one folder's songs
    // keep that folder's name.
    const std::wstring out = dir + L"\\out";
    const auto spread = PlanConvert({{1, dir + L"\\a\\x\\one.flac"}, {2, dir + L"\\b\\two.flac"}}, CordExport::Mp3_192,
                                    ConvertMode::Copy, out + L"\\");
    const auto together = PlanConvert({{1, dir + L"\\a\\x\\one.flac"}, {2, dir + L"\\a\\x\\two.flac"}}, CordExport::Mp3_192,
                                      ConvertMode::Copy, out);
    more.Add(spread.size() == 2 && spread[0].to == out + L"\\a\\x\\one.mp3" && spread[1].to == out + L"\\b\\two.mp3" &&
                 together.size() == 2 && together[0].to == out + L"\\x\\one.mp3" && together[1].to == out + L"\\x\\two.mp3" &&
                 !spread[0].replace,
             L"copies keep their folders below the common one");
    // A .cord copy into a new folder: made, its folders created, nothing logged, the source kept.
    const std::wstring wv = dir + L"\\lossless-pcm16.wv";
    const auto cordCopy = PlanConvert({{1, wv}}, CordExport::None, ConvertMode::Copy, out);
    ConvertSettings keep;
    keep.recycle = false;
    const ConvertResult copied = ExecuteConvert(cordCopy, keep, logDir, cancel, nullptr);
    const Exact copiedBack = ReadAll(out + L"\\cord-convert\\lossless-pcm16.cord"), wvBack = ReadAll(wv);
    more.Add(cordCopy.size() == 1 && cordCopy[0].skip.empty() && !cordCopy[0].replace && copied.done.size() == 1 && copied.log.empty() &&
                 copiedBack.samples == wvBack.samples && !wvBack.samples.empty(),
             L".cord copy into a folder");
    // Replacing TTA with FLAC: logged, the original kept (by preference), and undo brings it back
    // as the library's file while the FLAC stays (undo without recycling).
    const std::wstring tta = dir + L"\\lossless-sine.tta", replaced = dir + L"\\lossless-sine.flac";
    const auto replacePlan = PlanConvert({{1, tta}}, CordExport::Flac, ConvertMode::Replace);
    const ConvertResult swapped = ExecuteConvert(replacePlan, keep, logDir, cancel, nullptr);
    const bool logged = CanUndoConvert(logDir);
    const auto back = UndoLastConvert(logDir, false, error);
    more.Add(replacePlan.size() == 1 && replacePlan[0].replace && replacePlan[0].to == replaced && swapped.done.size() == 1 &&
                 !swapped.log.empty() && logged && back.size() == 1 && back[0].original == tta && back[0].cord == replaced &&
                 !CanUndoConvert(logDir) && GetFileAttributesW(tta.c_str()) != INVALID_FILE_ATTRIBUTES &&
                 ReadAll(replaced).samples == ReadAll(tta).samples,
             L"replacement to FLAC and its undo " + error);
    Check(more.Passed(), L"lossless sources, folders, .cord copies and replacements%ls", more.failed.c_str());

    batch.Next(L"planning 100,000 songs takes under 2 seconds, so the Convert window's count keeps up with a whole library");
    // The window replans everything it was opened for on each change, on the UI thread. A library of
    // 100,000 songs in albums of twelve (docs/memory.md's size), planned as copies into a folder and
    // as replacements; outputs that would collide are found without comparing every pair.
    std::vector<std::pair<TrackId, std::wstring>> library;
    library.reserve(100000);
    for (TrackId i = 0; i < 100000; ++i)
        library.push_back({i, dir + L"\\library\\Artist " + std::to_wstring(i / 1200) + L"\\Album " + std::to_wstring(i / 12) + L"\\" +
                                  std::to_wstring(i % 12 + 1) + L" Song.flac"});
    library.push_back({100000, library[5].second});  // the same file twice: its second output collides
    using Clock = std::chrono::steady_clock;
    const Clock::time_point t0 = Clock::now();
    const auto asCopies = PlanConvert(library, CordExport::Mp3_320, ConvertMode::Copy, dir + L"\\copies");
    const Clock::time_point t1 = Clock::now();
    const auto asReplacements = PlanConvert(library, CordExport::None, ConvertMode::Replace);
    const Clock::time_point t2 = Clock::now();
    const double copiesSeconds = std::chrono::duration<double>(t1 - t0).count(),
                 replaceSeconds = std::chrono::duration<double>(t2 - t1).count();
    const size_t copyReady = (size_t)std::count_if(asCopies.begin(), asCopies.end(), [](const ConvertItem& i) { return i.skip.empty(); });
    Check(asCopies.size() == 100001 && copyReady == 100000 && !asCopies.back().skip.empty() && asReplacements.size() == 100001 &&
              asCopies[0].to == dir + L"\\copies\\Artist 0\\Album 0\\1 Song.mp3" && copiesSeconds < 2 && replaceSeconds < 2,
          L"planning 100,000 songs: copies in %.2f s, replacements in %.2f s (target under 2 s each)", copiesSeconds, replaceSeconds);
    DeleteTestFolder(dir);
}
