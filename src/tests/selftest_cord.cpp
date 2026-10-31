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
    TestTagWriterFiles({tagged}, g_log, L"tag writing: cordinal format");
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

    // The FLAC escape route has only the .cord files: no originals, no journal, even when the
    // conversion preference would normally recycle originals. These exports always keep them.
    if (!Ready(DeleteFileW(tagged.c_str()) && DeleteFileW(gains.c_str()) && DeleteFileW(wave.c_str()),
               L"remove scratch originals before exporting .cord")) return;
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
        const bool saved = encoded && ExportCordFlac(from, to, error, cancel);
        const Exact back = ReadAll(to);
        const std::string flacBytes = ReadTestBytes(to);
        formats.Add(saved && back.opened && !back.damaged && back.samples == samples && back.format.bits == bits && back.format.rate == rate &&
                        back.format.channels == channels && ReadTestBytes(to).find("ARTIST=Björk") != std::string::npos &&
                        flacBytes.find("ARTIST=Second artist") != std::string::npos &&
                        flacBytes.find("MUSICBRAINZ_TRACKID=keep this id") != std::string::npos &&
                        (!mask || flacBytes.find("WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x3f") != std::string::npos),
                    name + L" " + error);
        const std::wstring wav = dir + L"\\export-" + name + L".wav";
        const bool waveSaved = ExportCordAudio(from, wav, CordExport::Wav, error, cancel);
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
                const bool made = ExportCordAudio(from, transientCopy, target, error, cancel);
                const std::string output = ReadTestBytes(transientCopy);
                formats.Add(made && output.find("MUSICBRAINZ_TRACKID") != output.npos && output.find("Björk") != output.npos &&
                                output.find("Second artist") != output.npos, std::wstring(info.label) + L" transients " + error);
            }
        }
    }
    Check(formats.Passed(), L"exports preserve lossless precision and tags across rates, channels, transients and final blocks%ls", formats.failed.c_str());

    testlog::Cases copies;
    for (const auto target : {CordExport::Wav, CordExport::Mp3_192, CordExport::Mp3_320, CordExport::Aac128, CordExport::Aac192}) {
        const auto& info = kCordExports[(unsigned)target];
        const std::wstring output = dir + L"\\copy-" + std::to_wstring((unsigned)target) + L"." + info.extension;
        const auto exportPlan = PlanConvert({{1, cord}}, target);
        const bool made = ExportCordAudio(cord, output, target, error, cancel);
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
        const bool stopped = !ExportCordAudio(cord, output + L".cancelled", target, error, cancel);
        cancel = false;
        copies.Add(stopped && !ExportCordAudio(cord, output, target, error, cancel) && ReadTestBytes(output) == bytes,
                   std::wstring(info.label) + L" cancellation and collision");
    }
    Check(copies.Passed(), L"WAV, MP3 and AAC copies keep tags, lyrics and covers, decode completely and never overwrite files%ls", copies.failed.c_str());

    const std::wstring safe = dir + L"\\safe.flac", damaged = dir + L"\\damaged.cord";
    const std::string preserved = ReadTestBytes(cord), existing = ReadTestBytes(tagged);
    const auto collision = PlanConvert({{1, cord}, {2, tagged}, {3, cord + L"|1"}}, CordExport::Flac);
    const bool noOverwrite = !ExportCordFlac(cord, tagged, error, cancel) && ReadTestBytes(tagged) == existing &&
                             !collision[0].skip.empty() && !collision[1].skip.empty() && !collision[2].skip.empty();
    cancel = true;
    const bool cancelled = !ExportCordFlac(cord, safe, error, cancel) && GetFileAttributesW(safe.c_str()) == INVALID_FILE_ATTRIBUTES;
    cancel = false;
    WriteTestBytes(damaged, preserved.substr(0, preserved.size() - 1));
    const bool rejected = !ExportCordFlac(damaged, safe, error, cancel) && GetFileAttributesW(safe.c_str()) == INVALID_FILE_ATTRIBUTES;
    // Valid container, corrupt audio: verification must also catch damage found after opening it.
    std::string corrupt = preserved;
    corrupt.back() ^= 1;
    WriteTestBytes(damaged, corrupt);
    const bool badAudio = !ExportCordFlac(damaged, safe, error, cancel) && GetFileAttributesW(safe.c_str()) == INVALID_FILE_ATTRIBUTES;
    const std::vector<int32_t> tiny{0, -1, 0, -1};
    MemoryPcm tooNarrow(tiny, 1, 1);
    const bool narrow = cord::Encode(tooNarrow, dir + L"\\one-bit.cord", error) &&
                        !ExportCordFlac(dir + L"\\one-bit.cord", safe, error, cancel) &&
                        GetFileAttributesW(safe.c_str()) == INVALID_FILE_ATTRIBUTES;
    WIN32_FIND_DATAW found{};
    HANDLE temporary = FindFirstFileW((dir + L"\\*.cordinal-tmp-*").c_str(), &found);
    if (temporary != INVALID_HANDLE_VALUE) FindClose(temporary);
    Check(noOverwrite && cancelled && rejected && badAudio && narrow && ReadTestBytes(cord) == preserved && temporary == INVALID_HANDLE_VALUE,
          L"FLAC export leaves sources and existing files intact, and removes cancelled or damaged exports");
    DeleteTestFolder(dir);
}
