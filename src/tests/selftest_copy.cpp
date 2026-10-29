// Copy to folder or device (devicecopy.h): the manifest, the AAC .m4a writer's length and gapless
// alignment, and a copy run into a folder: names, conversion, tags, the relative playlist, the
// second run that copies nothing, collisions with files cordinal didn't put there, cancelling, and
// removing only cordinal's own earlier copies.
#include "selftest_common.h"
#include "devicecopy.h"

namespace {

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

// Nothing a run leaves behind under a .part name.
bool NoParts(const std::wstring& dir) {
    for (const auto& e : std::filesystem::recursive_directory_iterator(dir))
        if (e.path().extension() == L".part") return false;
    return true;
}

// A 48 kHz stereo WAV of a rising sweep: never periodic, so a cross-correlation finds one lag.
std::string SweepWave(uint32_t frames) {
    std::string data;
    for (uint32_t i = 0; i < frames; ++i) {
        const double t = i / 48000.0, v = 0.5 * std::sin(2 * 3.14159265358979 * (100 * t + 900 * t * t));
        const int16_t s = (int16_t)std::lround(v * 32767);
        for (int ch = 0; ch < 2; ++ch) data.append((const char*)&s, 2);
    }
    auto le = [](uint32_t v, int n) { return std::string((const char*)&v, (size_t)n); };
    return "RIFF" + le(36 + (uint32_t)data.size(), 4) + "WAVEfmt " + le(16, 4) + le(1, 2) + le(2, 2) + le(48000, 4) + le(192000, 4) +
           le(4, 2) + le(16, 2) + "data" + le((uint32_t)data.size(), 4) + data;
}

// The lag (in frames) at which `got` best matches `want`, over the middle of the sweep.
int BestLag(const std::vector<float>& want, const std::vector<float>& got, double& score) {
    int best = 0;
    score = -1;
    const size_t from = 24000, count = 48000;
    for (int lag = -3000; lag <= 3000; ++lag) {
        double dot = 0, a = 0, b = 0;
        for (size_t i = from; i < from + count; i += 2) {
            const int64_t j = (int64_t)i + lag;
            if (j < 0 || (size_t)j * 2 >= got.size() || i * 2 >= want.size()) continue;
            dot += want[i * 2] * got[(size_t)j * 2];
            a += want[i * 2] * want[i * 2];
            b += got[(size_t)j * 2] * got[(size_t)j * 2];
        }
        const double s = a && b ? dot / std::sqrt(a * b) : 0;
        if (s > score) {
            score = s;
            best = lag;
        }
    }
    return best;
}

}  // namespace

void TestDeviceCopy(const std::wstring& media, const std::wstring& scratch) {
    std::vector<CopyRecord> parsed;
    const std::vector<CopyRecord> records = {{L"A\\one.m4a", L"C:\\Music\\one.flac", 12, 34, 192, {L"Mix", L"*"}},
                                             {L"cover.jpg", L"C:\\Music\\cover.jpg", 5, 6, 0, {L"Mix"}}};
    const bool roundTrip = ParseCopyManifest(CopyManifestText(records), parsed) && parsed.size() == 2 && parsed[0].rel == records[0].rel &&
                           parsed[0].from == records[0].from && parsed[0].size == 12 && parsed[0].time == 34 && parsed[0].format == 192 &&
                           parsed[0].sets == records[0].sets && parsed[1].sets == records[1].sets;
    const bool outside = ParseCopyManifest("cordinal-copy 1\n..\\up.mp3\tx\t1\t2\t0\tMix\nC:\\abs.mp3\tx\t1\t2\t0\tMix\n", parsed) && parsed.empty();
    Check(roundTrip && outside, L"COPY manifest round trip keeps every field, and never names a file outside the destination");

    const std::wstring root = scratch + L".copy";
    std::filesystem::remove_all(root);
    const std::wstring in = root + L"\\in", out = root + L"\\out";
    for (auto d : {root, in, in + L"\\A", in + L"\\B", out}) CreateDirectoryW(d.c_str(), nullptr);
    std::atomic<bool> cancel{false};
    std::wstring error;

    // The writer: exact length through the edit list, and the encoder's priming skipped.
    const std::wstring sweep = in + L"\\sweep.wav", m4a = root + L"\\sweep.m4a";
    WriteTestBytes(sweep, SweepWave(96000));
    if (!EncodeM4a(sweep, m4a, 192, cancel, error)) {
        Check(false, L"COPY built-in AAC conversion failed: %ls", error.c_str());
    } else {
        unsigned rate = 0, sourceRate = 0;
        const std::vector<float> got = DecodeAll(m4a, rate), want = DecodeAll(sweep, sourceRate);
        double score = 0;
        const int lag = BestLag(want, got, score);
        TagData t;
        ReadTags(m4a, t);
        Check(rate == 48000 && got.size() == want.size() && lag == 0 && score > 0.98 && t.codec == Codec::AAC,
              L"COPY .m4a decodes to exactly the source's %zu frames (got %zu at %u Hz), aligned (lag %d, match %.3f)",
              want.size() / 2, got.size() / 2, rate, lag, score);
    }

    g_lib = Library();
    CopyFileW((media + L"\\sine.flac").c_str(), (in + L"\\A\\one.flac").c_str(), FALSE);
    CopyFileW((media + L"\\sine.mp3").c_str(), (in + L"\\B\\two.mp3").c_str(), FALSE);
    WriteTestBytes(in + L"\\A\\cover.jpg", "not really a jpeg");
    std::vector<TrackId> ids;
    for (auto f : {in + L"\\A\\one.flac", in + L"\\B\\two.mp3"}) {
        TrackId id = g_lib.Add(f);
        TagData t;
        ReadTags(f, t);
        if (f.find(L"two") != std::wstring::npos) t.album = "Other Album";
        g_lib.ApplyTags(id, t);
        g_lib[id].flags |= Track::kInLibrary;
        g_lib[id].rgTrackGain = -650;
        g_lib[id].rgTrackPeak = 19000;
        ids.push_back(id);
    }
    const std::string flacBefore = ReadTestBytes(in + L"\\A\\one.flac"), mp3Before = ReadTestBytes(in + L"\\B\\two.mp3");

    // A file cordinal didn't put there keeps its name: the copy takes " (2)".
    const std::wstring album = L"Test Artist\\Test Album\\", name = L"07 Sine \x00DCn\x00EF" L"code";
    SHCreateDirectoryExW(nullptr, (out + L"\\Test Artist\\Other Album").c_str(), nullptr);
    WriteTestBytes(out + L"\\Test Artist\\Other Album\\" + name + L".mp3", "the user's own file");
    std::vector<CopyRecord> manifest;
    auto plan = PlanCopy(ids, out, false, kDefaultOrganizePattern, CopyFormat::Aac192, manifest);
    const bool planned = plan.size() == 2 && plan[0].convert && plan[0].rel == album + name + L".m4a" && !plan[1].convert &&
                         plan[1].rel == L"Test Artist\\Other Album\\" + name + L" (2).mp3";
    Check(planned, L"COPY plan converts lossless, keeps lossy, and steps around a file it didn't copy: %ls | %ls",
          plan.size() > 0 ? plan[0].rel.c_str() : L"", plan.size() > 1 ? plan[1].rel.c_str() : L"");

    // Cancelled before the first file: nothing arrives, nothing half-written.
    CopySettings settings;
    settings.dest = out;
    settings.staging = root + L"\\staging";
    settings.set = L"Mix";
    settings.kbps = 192;
    cancel = true;
    CopyResult r = ExecuteCopy(plan, settings, manifest, cancel, {});
    Check(r.cancelled && !r.copied && !r.converted && !Exists(out + L"\\" + plan[0].rel) && NoParts(out) && !Exists(settings.staging),
          L"COPY cancelled leaves no copies, no .part files and no staging folder");
    cancel = false;

    std::string text = M3UText({{ids[0], ToUtf8(album + name + L".m4a")}, {ids[1], "x/y.mp3"}});
    std::replace(text.begin(), text.end(), '\\', '/');
    settings.playlistName = L"Mix";
    settings.playlist = text;
    r = ExecuteCopy(plan, settings, manifest, cancel, {});
    TagData m4aTags, mp3Tags;
    ReadTags(out + L"\\" + plan[0].rel, m4aTags);
    ReadTags(out + L"\\" + plan[1].rel, mp3Tags);
    const bool copied = r.copied == 1 && r.failed.empty() && r.converted == 1 && NoParts(out) && !Exists(settings.staging) &&
                        Exists(out + L"\\Test Artist\\Test Album\\cover.jpg") && !Exists(out + L"\\Test Artist\\Other Album\\cover.jpg") &&
                        ReadTestBytes(out + L"\\Test Artist\\Other Album\\" + name + L".mp3") == "the user's own file";
    const bool tagged = m4aTags.title == ToUtf8(name.substr(3)) && m4aTags.artist == "Test Artist" &&
                        std::fabs(m4aTags.rgTrackGain + 6.5f) < 0.01f && std::fabs(mp3Tags.rgTrackGain + 6.5f) < 0.01f;
    const bool untouched = ReadTestBytes(in + L"\\A\\one.flac") == flacBefore && ReadTestBytes(in + L"\\B\\two.mp3") == mp3Before;
    Check(copied && tagged && untouched,
              L"COPY run converts and copies with tags and ReplayGain, brings the cover only to a whole folder, and leaves the "
              L"originals byte-identical (copied=%d tagged=%d untouched=%d, %zu failed%ls)",
              copied, tagged, untouched, r.failed.size(), r.failed.empty() ? L"" : (L": " + r.failed[0].second).c_str());
    const std::string written = ReadTestBytes(out + L"\\Mix.m3u8");
    Check(written.rfind("\xEF\xBB\xBF#EXTM3U\n", 0) == 0 && written.find("\nTest Artist/Test Album/" + ToUtf8(name) + ".m4a\n") != std::string::npos &&
              written.find(":\\") == std::string::npos,
          L"COPY playlist lists the copies by relative '/' paths, never this PC's paths");

    // The second run finds everything there.
    const bool read = ReadCopyManifest(out, false, manifest);
    Check(read && manifest.size() == 3, L"COPY manifest records the songs and the cover (%zu)", manifest.size());
    plan = PlanCopy(ids, out, false, kDefaultOrganizePattern, CopyFormat::Aac192, manifest);
    Check(plan.size() == 2 && plan[0].done && plan[1].done, L"COPY second run plans nothing new");

    // Dropping the FLAC from "Mix": its copy and its folder's cover go, the user's file and a
    // selection's copy stay.
    std::vector<CopyRecord> withSelection = manifest;
    for (CopyRecord& rec : withSelection)
        if (rec.rel.find(L"(2)") != std::wstring::npos) rec.sets = {L"Mix", kCopySelectionSet};
    plan = PlanCopy({ids[1]}, out, false, kDefaultOrganizePattern, CopyFormat::Aac192, withSelection);
    const auto stale = StaleCopies(plan, withSelection, L"Mix");
    settings.removeStale = true;
    settings.playlistName.clear();
    r = ExecuteCopy(plan, settings, withSelection, cancel, {});
    const bool removed = stale.size() == 2 && r.removed == 2 && !Exists(out + L"\\" + album + name + L".m4a") &&
                         !Exists(out + L"\\Test Artist\\Test Album") && Exists(out + L"\\Test Artist\\Other Album\\" + name + L" (2).mp3") &&
                         Exists(out + L"\\Test Artist\\Other Album\\" + name + L".mp3");
    Check(removed, L"COPY removes only cordinal's copies the playlist no longer holds (%zu stale, %zu removed)", stale.size(), r.removed);
    g_lib = Library();
    std::filesystem::remove_all(root);
}
