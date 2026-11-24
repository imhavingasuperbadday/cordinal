// Checks of CD ripping (cdrip.h) without a drive: the MusicBrainz disc id against libdiscid's own
// example, Enhanced CD sessions, CD-Text packs, disc-id lookups on canned replies, and whole rips from a
// disc held in memory whose sectors misread now and then, at several read offsets, into FLAC and .cord
// with their tags and cover. The Windows self-test ("cd-rip") and the Linux core test ("cd-rip") both
// run them.
#pragma once
#include "cdrip.h"
#include "convert.h"
#include "metadata_checks.h"

#include <map>

namespace cdrip_checks {

using metadata_checks::Group;

// libdiscid's example disc: six tracks, MusicBrainz id 49HHV7Eb8UKF3aQiNmu1GR8vKTY-.
inline cdrip::Toc ExampleToc() {
    cdrip::Toc toc;
    const uint32_t starts[] = {0, 15213, 32164, 46442, 63264, 80339};
    for (uint8_t i = 0; i < 6; ++i) toc.tracks.push_back({(uint8_t)(i + 1), starts[i], true});
    toc.leadout = 95312;
    return toc;
}

// A small disc in memory: every sample is a function of its position, so any misplaced or misread
// sample shows. Sectors in `flaky` read wrong that many times before reading right; sectors in
// `noisy` never read the same way twice; sectors in `failing` report an error that many times.
class MemoryDrive : public cdrip::Drive {
public:
    cdrip::Toc toc;
    std::map<uint32_t, int> flaky, failing;
    std::map<uint32_t, bool> noisy;
    unsigned reads = 0;
    std::string cdText;

    static int16_t Sample(int64_t frame, int channel) {
        return (int16_t)((frame * 7919 + channel * 104729) % 65521 - 32760);
    }
    bool ReadToc(cdrip::Toc& out) override {
        out = toc;
        return true;
    }
    bool Read(uint32_t lba, unsigned count, uint8_t* out) override {
        for (unsigned s = 0; s < count; ++s)
            if (auto f = failing.find(lba + s); f != failing.end() && f->second > 0) {
                --f->second;
                return false;
            }
        for (unsigned s = 0; s < count; ++s) {
            uint8_t* p = out + (size_t)s * cdrip::kSectorBytes;
            const int64_t frame0 = (int64_t)(lba + s) * cdrip::kSectorFrames;
            for (unsigned i = 0; i < cdrip::kSectorFrames; ++i)
                for (int c = 0; c < 2; ++c) {
                    const int16_t v = Sample(frame0 + i, c);
                    p[i * 4 + c * 2] = (uint8_t)v;
                    p[i * 4 + c * 2 + 1] = (uint8_t)((uint16_t)v >> 8);
                }
            if (auto f = flaky.find(lba + s); f != flaky.end() && f->second > 0) {
                p[100] ^= (uint8_t)(0x5A + --f->second);  // each misread differs, as real ones do
            }
            if (noisy.count(lba + s)) p[200] = (uint8_t)(++reads * 37);
        }
        ++reads;
        return true;
    }
    std::string CdText() override { return cdText; }
};

// CD-Text packs for `texts` (album first, then each track) of one pack type, as a disc stores them.
inline std::string Packs(uint8_t type, const std::vector<std::string>& texts, uint8_t block = 0) {
    std::string all, out;
    std::vector<uint8_t> trackOf;
    for (size_t t = 0; t < texts.size(); ++t) {
        for (size_t i = 0; i <= texts[t].size(); ++i) trackOf.push_back((uint8_t)t);
        all += texts[t];
        all += '\0';
    }
    for (size_t at = 0, seq = 0; at < all.size(); at += 12, ++seq) {
        std::string pack(18, '\0');
        pack[0] = (char)type;
        pack[1] = (char)trackOf[at];
        pack[2] = (char)seq;
        pack[3] = (char)(block << 4);
        for (size_t i = 0; i < 12 && at + i < all.size(); ++i) pack[4 + i] = all[at + i];
        out += pack;
    }
    return out;
}

inline Group DiscIds() {
    Group g{"disc ids and tables of contents"};
    const cdrip::Toc toc = ExampleToc();
    g.Add(cdrip::DiscId(toc) == "49HHV7Eb8UKF3aQiNmu1GR8vKTY-", "libdiscid's example disc gets its MusicBrainz id");
    g.Add(cdrip::TocParameter(toc) == "1+6+95462+150+15363+32314+46592+63414+80489", "the toc parameter lists lead-out and offsets");
    cdrip::Toc enhanced = toc;
    enhanced.tracks.push_back({7, 95312 + 11400, false});
    enhanced.leadout = 140000;
    g.Add(cdrip::DiscId(enhanced) == cdrip::DiscId(toc) && enhanced.End(5) == 95312 && enhanced.Audio().size() == 6,
          "an Enhanced CD's data session is left out of the id and of the last track's audio");
    g.Add(toc.End(0) == 15213 && toc.End(5) == 95312 && cdrip::DiscId({}).empty(), "tracks end where the next begins");
    return g;
}

inline Group CdText() {
    Group g{"CD-Text"};
    MemoryDrive d;
    const std::string packs = Packs(0x80, {"Caf\xE9 Songs", "One", "A rather long second title", "\t"}) +
                              Packs(0x81, {"The Band", "The Band", "Guest", ""}) + Packs(0x80, {"Ignored", "X", "Y", "Z"}, 1);
    cdrip::Disc disc;
    const bool parsed = cdrip::ParseCdText(packs, 3, disc);
    g.Add(parsed && disc.source == "CD-Text" && disc.album == "Caf\xC3\xA9 Songs" && disc.artist == "The Band",
          "the album and its performer are read, Latin-1 as UTF-8");
    g.Add(disc.titles.size() == 3 && disc.titles[0] == "One" && disc.titles[1] == "A rather long second title" &&
              disc.titles[2] == "A rather long second title",
          "titles run across packs, and a tab repeats the one before");
    g.Add(disc.artists.size() == 3 && disc.artists[0].empty() && disc.artists[1] == "Guest", "a track artist the album shares is left empty");
    cdrip::Disc none;
    g.Add(!cdrip::ParseCdText("", 3, none) && !cdrip::ParseCdText(std::string(17, '\x80'), 3, none) && none.source.empty(),
          "no packs, or a cut pack, names nothing");
    return g;
}

inline Group Lookups() {
    Group g{"MusicBrainz disc lookups"};
    const cdrip::Toc toc = ExampleToc();
    const std::string id = cdrip::DiscId(toc);
    auto media = [](int position, const std::string& disc, int tracks) {
        std::string m = R"({"position":)" + std::to_string(position) + R"(,"format":"CD","discs":[{"id":")" + disc + R"("}],"tracks":[)";
        for (int t = 1; t <= tracks; ++t)
            m += (t > 1 ? "," : "") + std::string(R"({"position":)") + std::to_string(t) + R"(,"title":"Disc )" +
                 std::to_string(position) + " song " + std::to_string(t) + R"(","length":200000,"artist-credit":[{"name":")" +
                 (t == 2 ? "Guest" : "Ann") + R"("}]})";
        return m + "]}";
    };
    const std::string reply = R"({"releases":[{"id":"11111111-1111-4111-8111-111111111111","title":"Big Set","date":"2001-02-03",)"
                              R"("country":"GB","artist-credit":[{"name":"Ann"}],"release-group":{"id":"aaaaaaaa-1111-4111-8111-111111111111"},)"
                              R"("media":[)" + media(1, "other", 4) + "," + media(2, id, 6) + R"(]},)"
                              R"({"id":"../../etc","title":"Bad","media":[]}]})";
    std::string asked;
    bool offline = true;
    online::Cancel cancel;
    const auto found = cdrip::LookUp(toc, cancel, offline, [&](const std::string& url) {
        asked = url;
        return online::Reply{200, reply};
    });
    g.Add(asked.find("/ws/2/discid/" + id + "?toc=1+6+95462+") != std::string::npos && !offline,
          "the lookup sends the disc id and its table of contents");
    g.Add(found.size() == 1 && found[0].album == "Big Set" && found[0].artist == "Ann" && found[0].number == 2 && found[0].count == 2 &&
              found[0].release == "11111111-1111-4111-8111-111111111111" && found[0].discId == id,
          "a release's names come from the medium holding this disc, and a bad id is dropped");
    g.Add(!found.empty() && found[0].titles.size() == 6 && found[0].titles[0] == "Disc 2 song 1" && found[0].artists[1] == "Guest" &&
              found[0].artists[0].empty() && found[0].source.find("disc 2 of 2") != std::string::npos,
          "track titles and guest artists are kept, and the choice names the disc");
    const auto fuzzy = albumtag::ParseDiscLookup(R"({"releases":[{"id":"22222222-2222-4222-8222-222222222222","title":"T",)"
                                                 R"("media":[)" + media(1, "x", 4) + "," + media(2, "y", 6) + "]}]}",
                                                 id, 6);
    g.Add(fuzzy.size() == 1 && fuzzy[0].tracks.size() == 6 && fuzzy[0].tracks[0].disc == 2,
          "a match by table of contents takes the medium with the disc's track count");
    const auto gone = cdrip::LookUp(toc, cancel, offline, [](const std::string&) { return online::Reply{0, {}}; });
    g.Add(gone.empty() && offline, "an unreachable MusicBrainz is reported as offline");
    std::string coverUrl;
    const std::string cover = cdrip::FetchCover("11111111-1111-4111-8111-111111111111", cancel, [&](const std::string& url) {
        coverUrl = url;
        return online::Reply{200, metadata_checks::Png()};
    });
    g.Add(cover == metadata_checks::Png() && coverUrl == "https://coverartarchive.org/release/11111111-1111-4111-8111-111111111111/front-500" &&
              cdrip::FetchCover("../x", cancel, [](const std::string&) { return online::Reply{200, metadata_checks::Png()}; }).empty() &&
              cdrip::FetchCover("11111111-1111-4111-8111-111111111111", cancel,
                                [](const std::string&) { return online::Reply{200, "<html>"}; }).empty(),
          "the cover comes from the Cover Art Archive, and only a real image of a real release is kept");
    return g;
}

// A short two-track disc: 3 seconds, then 2 seconds.
inline cdrip::Toc ShortToc() {
    cdrip::Toc toc;
    toc.tracks = {{1, 0, true}, {2, 225, true}};
    toc.leadout = 375;
    return toc;
}

// Reads track `t` of `d` and compares every sample with what the disc holds at `offset`.
inline bool ReadsExactly(MemoryDrive& d, size_t t, int offset, cdrip::ReadStats& stats, double* lastProgress = nullptr) {
    std::atomic<bool> cancel{false};
    auto reader = cdrip::OpenTrack(d, d.toc, t, offset, stats, cancel, [&](double f) { if (lastProgress) *lastProgress = f; });
    if (!reader || !cdrip::Scan(*reader)) return false;
    const int64_t limit = (int64_t)d.toc.End(d.toc.Audio().back()) * cdrip::kSectorFrames;
    int64_t frame = (int64_t)d.toc.tracks[t].start * cdrip::kSectorFrames + offset;
    std::vector<int32_t> pcm(1000 * 2);
    bool same = true;
    uint64_t total = 0;
    while (const size_t n = reader->Read(pcm.data(), 1000)) {
        for (size_t i = 0; i < n; ++i, ++frame)
            for (int c = 0; c < 2; ++c) {
                const int32_t want = frame < 0 || frame >= limit ? 0 : MemoryDrive::Sample(frame, c);
                same = same && pcm[i * 2 + c] == want;
            }
        total += n;
    }
    return same && !reader->Damaged() && total == (uint64_t)(d.toc.End(t) - d.toc.tracks[t].start) * cdrip::kSectorFrames;
}

inline Group Reading() {
    Group g{"verified reading"};
    MemoryDrive d;
    d.toc = ShortToc();
    cdrip::ReadStats clean;
    double progress = 0;
    g.Add(ReadsExactly(d, 0, 0, clean, &progress) && clean.reread == 0 && clean.unsure == 0 && progress == 1.0,
          "a clean track reads exactly, every sector agreeing the first time");
    for (int offset : {6, 667, -472, -1164, 2000}) {
        cdrip::ReadStats s1, s2;
        g.Add(ReadsExactly(d, 0, offset, s1) && ReadsExactly(d, 1, offset, s2),
              "a read offset of " + std::to_string(offset) + " samples moves the audio, with silence beyond the disc");
    }
    d.flaky = {{10, 1}, {40, 2}, {230, 1}};
    d.failing = {{20, 2}};  // the stretch's read and then the sector's own read fail
    cdrip::ReadStats s;
    g.Add(ReadsExactly(d, 0, 0, s) && s.reread == 3 && s.unsure == 0,
          "misreads and a read error are read again until two reads agree, and the audio is exact");
    d.noisy = {{100, true}};
    cdrip::ReadStats n;
    std::atomic<bool> cancel{false};
    auto reader = cdrip::OpenTrack(d, d.toc, 0, 0, n, cancel);
    std::vector<int32_t> pcm(4096 * 2);
    if (reader && cdrip::Scan(*reader))
        while (reader->Read(pcm.data(), 4096)) {}
    g.Add(reader && !reader->Damaged() && n.unsure == 1 && n.reread == 1, "a sector that never reads the same twice is reported, not hidden");
    cancel = true;
    auto stopped = cdrip::OpenTrack(d, d.toc, 1, 0, n, cancel);
    g.Add(stopped && !cdrip::Scan(*stopped) && stopped->Read(pcm.data(), 4096) == 0 && stopped->Damaged(),
          "a cancelled read ends as damaged, so nothing is written from it");
    g.Add(!cdrip::OpenTrack(d, d.toc, 5, 0, n, cancel), "a track the disc doesn't have isn't opened");
    return g;
}

inline Group Rips(const std::wstring& scratch) {
    Group g{"rips into FLAC and .cord"};
    MemoryDrive d;
    d.toc = ShortToc();
    d.flaky = {{50, 1}};
    for (bool flac : {true, false}) {
        cdrip::Job job;
        job.toc = d.toc;
        job.disc.artist = "Ann";
        job.disc.album = "Small: Disc?";
        job.disc.date = "2001";
        job.disc.titles = {"Opening", ""};
        job.disc.artists = {"", "Guest"};
        job.disc.release = "11111111-1111-4111-8111-111111111111";
        job.disc.discId = cdrip::DiscId(d.toc);
        job.tracks = job.toc.Audio();
        job.folder = scratch + (flac ? L"/flac" : L"/cord");
        job.flac = flac;
        job.cover = metadata_checks::Png();
        std::atomic<bool> cancel{false};
        size_t calls = 0;
        const cdrip::Result r = cdrip::Rip(d, job, cancel, [&](size_t, double) { ++calls; });
        const std::wstring ext = flac ? L".flac" : L".cord";
        const bool written = r.tracks.size() == 2 && r.tracks[0].error.empty() && r.tracks[1].error.empty() && !r.cancelled &&
                             FileNameOf(r.tracks[0].path) == L"01 Opening" + ext && FileNameOf(r.tracks[1].path) == L"02 Track 2" + ext &&
                             r.tracks[0].path.find(L"Small_ Disc_") != std::wstring::npos;
        g.Add(written && calls > 0, std::string(flac ? "FLAC" : ".cord") + ": both tracks are written under artist and album folders");
        bool exact = written;
        for (size_t t = 0; exact && t < 2; ++t) {
            std::wstring error;
            auto back = OpenPcmReader(r.tracks[t].path, error);
            std::vector<int32_t> pcm(4096 * 2);
            int64_t frame = (int64_t)d.toc.tracks[t].start * cdrip::kSectorFrames;
            exact = back && back->Format().rate == 44100 && back->Format().channels == 2 && back->Format().bits == 16;
            while (exact)
                if (const size_t n = back->Read(pcm.data(), 4096)) {
                    for (size_t i = 0; i < n * 2; ++i) exact = exact && pcm[i] == MemoryDrive::Sample(frame + (int64_t)i / 2, (int)(i % 2));
                    frame += (int64_t)n;
                } else break;
            exact = exact && back && !back->Damaged() && frame == (int64_t)d.toc.End(t) * cdrip::kSectorFrames;
        }
        g.Add(exact, std::string(flac ? "FLAC" : ".cord") + ": the files hold exactly the disc's samples");
        TagData a, b;
        std::vector<std::string> pictures;
        const bool tagged = written && ReadTags(r.tracks[0].path, a) && ReadTags(r.tracks[1].path, b) &&
                            (CordTagsFrom(r.tracks[0].path, pictures), pictures.size() == 1);
        g.Add(tagged && a.title == "Opening" && a.artist == "Ann" && a.album == "Small: Disc?" && a.date == "2001" && a.track == 1 &&
                  a.trackTotal == 2 && a.albumArtist.empty() && a.musicBrainzRelease == job.disc.release && b.title == "Track 2" &&
                  b.artist == "Guest" && b.albumArtist == "Ann" && b.track == 2,
              std::string(flac ? "FLAC" : ".cord") + ": tags, MusicBrainz ids and the cover are written");
        std::string before, after;
        if (!r.tracks.empty()) backup::ReadBytes(r.tracks[0].path, before);
        const cdrip::Result again = cdrip::Rip(d, job, cancel);
        g.Add(again.tracks.size() == 2 && !again.tracks[0].error.empty() && !before.empty() &&
                  backup::ReadBytes(r.tracks[0].path, after) && after == before,
              std::string(flac ? "FLAC" : ".cord") + ": ripping again never overwrites a file");
    }
    cdrip::Result shown;
    shown.tracks = {{L"a", L"", {2, 0}}, {L"b", L"", {0, 0}}};
    const std::wstring text = cdrip::DoneText(shown);
    shown.tracks.push_back({L"c", L"Failed.", {}});
    shown.tracks[1].stats.unsure = 3;
    g.Add(text == L"Ripped 2 tracks, every sector verified (2 needed more reads)." &&
              cdrip::DoneText(shown) == L"Ripped 2 tracks; 1 has sectors that never read the same way twice, so listen for clicks; 1 failed.",
          "the status line says how sure the rip is");
    return g;
}

inline Group AccurateRip(const std::wstring& scratch) {
    Group g{"AccurateRip verification, offset detection and rip log"};

    // 1. Disc IDs and URL on libdiscid's example TOC (and Enhanced CD data track omission).
    const cdrip::Toc example = ExampleToc();
    cdrip::AccurateRipIds ids{};
    // ExampleToc starts: {0, 15213, 32164, 46442, 63264, 80339}, leadout = 95312
    const uint32_t wantId1 = 0u + 15213u + 32164u + 46442u + 63264u + 80339u + 95312u;
    const uint32_t wantId2 = 1u * 1u + 15213u * 2u + 32164u * 3u + 46442u * 4u + 63264u * 5u + 80339u * 6u + 95312u * 7u;
    g.Add(cdrip::ComputeAccurateRipIds(example, ids) && ids.tracks == 6 && ids.id1 == wantId1 && ids.id2 == wantId2 &&
              ids.cddb != 0 &&
              cdrip::AccurateRipUrl(ids).starts_with("http://www.accuraterip.com/accuraterip/") &&
              cdrip::AccurateRipUrl(ids).find("dBAR-006-") != std::string::npos,
          "AccurateRip computes id1, id2, cddb and dBAR URL from a TOC");
    cdrip::Toc enhanced = example;
    enhanced.tracks.push_back({7, 95312 + 11400, false});
    enhanced.leadout = 140000;
    cdrip::AccurateRipIds enhIds{};
    g.Add(cdrip::ComputeAccurateRipIds(enhanced, enhIds) && enhIds.tracks == ids.tracks && enhIds.id1 == ids.id1 &&
              enhIds.id2 == ids.id2,
          "Enhanced CD audio session ends before the trailing data session in AccurateRip ids");

    // 2. Binary dBAR parser: valid multi-pressing, truncated, and mismatched records.
    auto put32 = [](std::string& s, uint32_t v) {
        for (int b = 0; b < 4; ++b) s.push_back((char)((v >> (b * 8)) & 0xff));
    };
    auto encodeRecord = [&](const cdrip::AccurateRipIds& recIds,
                            const std::vector<cdrip::AccurateRipEntry>& entries) {
        std::string out;
        out.push_back((char)entries.size());
        put32(out, recIds.id1);
        put32(out, recIds.id2);
        put32(out, recIds.cddb);
        for (const auto& e : entries) {
            out.push_back((char)e.confidence);
            put32(out, e.crc);
            put32(out, e.frame450);
        }
        return out;
    };

    // 3. Three-track 470-sector/track TOC for offline v1, v2, frame-450 and offset detection.
    cdrip::Toc arToc;
    arToc.tracks = {{1, 0, true}, {2, 470, true}, {3, 940, true}};
    arToc.leadout = 1410;
    cdrip::AccurateRipIds arIds{};
    cdrip::ComputeAccurateRipIds(arToc, arIds);

    struct BruteAr {
        uint32_t v1 = 0, v2 = 0, f450 = 0;
    };
    auto computeOracle = [&](size_t t) -> BruteAr {
        const uint64_t total = (uint64_t)(arToc.End(t) - arToc.tracks[t].start) * cdrip::kSectorFrames;
        const int64_t baseFrame = (int64_t)arToc.tracks[t].start * cdrip::kSectorFrames;
        const uint64_t lo = (t == 0) ? (5ull * cdrip::kSectorFrames - 1) : 0;
        const uint64_t hi = (t + 1 == arToc.tracks.size()) ? (total - 5ull * cdrip::kSectorFrames) : total;
        BruteAr r;
        for (uint64_t i = lo; i < hi; ++i) {
            const uint16_t l = (uint16_t)MemoryDrive::Sample(baseFrame + (int64_t)i, 0);
            const uint16_t rr = (uint16_t)MemoryDrive::Sample(baseFrame + (int64_t)i, 1);
            const uint32_t w = ((uint32_t)rr << 16) | l;
            const uint64_t prod = (uint64_t)w * (i + 1);
            r.v1 += (uint32_t)prod;
            r.v2 += (uint32_t)prod + (uint32_t)(prod >> 32);
        }
        for (uint64_t k = 0; k < cdrip::kSectorFrames; ++k) {
            const int64_t f = baseFrame + (int64_t)(450ull * cdrip::kSectorFrames + k);
            const uint32_t w = ((uint32_t)(uint16_t)MemoryDrive::Sample(f, 1) << 16) |
                               (uint16_t)MemoryDrive::Sample(f, 0);
            r.f450 += w * (uint32_t)(k + 1);
        }
        return r;
    };
    const BruteAr ref0 = computeOracle(0), ref1 = computeOracle(1), ref2 = computeOracle(2);

    const std::string binValid =
        encodeRecord(arIds, {{4, ref0.v1, ref0.f450}, {4, ref1.v1, ref1.f450}, {2, 0x11112222u, 0x33334444u}}) +
        encodeRecord(arIds, {{9, ref0.v2, ref0.f450}, {1, 0x55556666u, ref1.f450}, {3, 0x77778888u, 0x9999aaaau}});
    const auto parsed = cdrip::ParseAccurateRip(binValid, 3);
    g.Add(parsed.size() == 2 && parsed[0].tracks[0].crc == ref0.v1 && parsed[0].tracks[0].confidence == 4 &&
              parsed[1].tracks[0].crc == ref0.v2 && parsed[1].tracks[0].frame450 == ref0.f450 &&
              parsed[1].tracks[0].confidence == 9,
          "ParseAccurateRip reads multi-pressing dBAR binary records");
    g.Add(cdrip::ParseAccurateRip(binValid.substr(0, binValid.size() - 5), 3).size() == 1 &&
              cdrip::ParseAccurateRip(binValid, 2).empty() && cdrip::ParseAccurateRip(binValid.substr(0, 10), 3).empty(),
          "ParseAccurateRip drops truncated tails and track-count mismatches");

    online::Cancel cancelFetch;
    const auto fetchedOk = cdrip::FetchAccurateRip(arToc, cancelFetch, [&](const std::string& url) {
        return online::Reply{url == cdrip::AccurateRipUrl(arIds) ? 200u : 404u, binValid};
    });
    const auto fetched404 = cdrip::FetchAccurateRip(arToc, cancelFetch, [](const std::string&) {
        return online::Reply{404u, {}};
    });
    g.Add(fetchedOk.size() == 2 && fetched404.empty(),
          "FetchAccurateRip parses HTTP 200 dBAR replies and returns empty on 404");

    // 4. Offset detection across negative and positive drive offsets (with flaky sectors!).
    class OffsetDrive final : public MemoryDrive {
    public:
        int shift = 0;  // physical read lag: sample at disc frame F appears at (F + shift)
        bool silent = false;
        bool Read(uint32_t lba, unsigned count, uint8_t* out) override {
            for (unsigned s = 0; s < count; ++s)
                if (auto f = failing.find(lba + s); f != failing.end() && f->second > 0) {
                    --f->second;
                    return false;
                }
            const int64_t limit = (int64_t)toc.leadout * cdrip::kSectorFrames;
            for (unsigned s = 0; s < count; ++s) {
                uint8_t* p = out + (size_t)s * cdrip::kSectorBytes;
                const int64_t frame0 = (int64_t)(lba + s) * cdrip::kSectorFrames;
                for (unsigned i = 0; i < cdrip::kSectorFrames; ++i)
                    for (int c = 0; c < 2; ++c) {
                        const int64_t src = frame0 + (int64_t)i - shift;
                        const int16_t v = silent || src < 0 || src >= limit ? 0 : Sample(src, c);
                        p[i * 4 + c * 2] = (uint8_t)(v & 0xff);
                        p[i * 4 + c * 2 + 1] = (uint8_t)((v >> 8) & 0xff);
                    }
            }
            return true;
        }
    };

    bool offsetsOk = true;
    std::atomic<bool> cancelOp{false};
    const auto detectPressings = cdrip::ParseAccurateRip(
        encodeRecord(arIds, {{5, ref0.v2, ref0.f450}, {5, ref1.v2, ref1.f450}, {5, ref2.v2, ref2.f450}}), 3);
    for (int shift : {0, 6, 667, -472, -1164, 2000}) {
        OffsetDrive od;
        od.toc = arToc;
        od.shift = shift;
        od.failing[450] = 1;  // force single-sector retry on track 1 sector 450
        const auto det = cdrip::DetectOffset(od, arToc, detectPressings, cancelOp);
        offsetsOk &= det.found && det.offset == shift && det.matchedTracks == 3;
    }
    OffsetDrive silentDrive;
    silentDrive.toc = arToc;
    silentDrive.silent = true;
    const auto zeroDb = cdrip::ParseAccurateRip(encodeRecord(arIds, {{1, 0, 0}, {1, 0, 0}, {1, 0, 0}}), 3);
    const auto silentDet = cdrip::DetectOffset(silentDrive, arToc, zeroDb, cancelOp);
    g.Add(offsetsOk && !silentDet.found,
          "DetectOffset calibrates negative and positive drive offsets and rejects silence");

    // 5. Full Rip() streaming AccurateRip v1/v2, MatchedOffset, Mismatch, atomic .log, and DoneText.
    const std::wstring arOut = scratch + L"\\ar-verify";
    SHCreateDirectoryExW(nullptr, arOut.c_str(), nullptr);
    OffsetDrive ripDrive;
    ripDrive.toc = arToc;
    ripDrive.shift = 667;
    cdrip::Job job;
    job.toc = arToc;
    job.disc.artist = "AR Test";
    job.disc.album = "Verify Album";
    job.tracks = {0, 1, 2};
    job.folder = arOut;
    job.flac = true;
    job.offset = 667;
    job.arPressings = parsed;
    job.arChecked = true;
    job.writeLog = true;
    job.driveName = L"VirtualDrive";
    const cdrip::Result verified = cdrip::Rip(ripDrive, job, cancelOp);
    std::string logText;
    const bool logRead = !verified.logPath.empty() && backup::ReadBytes(verified.logPath, logText);
    g.Add(verified.tracks.size() == 3 &&
              verified.tracks[0].ar.status == cdrip::ArStatus::VerifiedV2 && verified.tracks[0].ar.confidence == 9 &&
              verified.tracks[0].ar.v1 == ref0.v1 && verified.tracks[0].ar.v2 == ref0.v2 &&
              verified.tracks[0].ar.frame450 == ref0.f450 &&
              verified.tracks[1].ar.status == cdrip::ArStatus::VerifiedV1 && verified.tracks[1].ar.confidence == 4 &&
              verified.tracks[2].ar.status == cdrip::ArStatus::Mismatch && logRead &&
              logText.find("Accurately ripped (v2, confidence 9)") != std::string::npos &&
              logText.find("Accurately ripped (v1, confidence 4)") != std::string::npos &&
              logText.find("Cannot be verified against AccurateRip") != std::string::npos &&
              logText.find("Read offset: +667 samples") != std::string::npos,
          "Rip verifies AccurateRip v2/v1/Mismatch in one pass and writes an atomic rip log");

    // Cross-pressing offset match during Rip() (+37 shift relative to DB).
    const std::wstring shiftOut = scratch + L"\\ar-shifted";
    SHCreateDirectoryExW(nullptr, shiftOut.c_str(), nullptr);
    OffsetDrive shiftedDrive;
    shiftedDrive.toc = arToc;
    shiftedDrive.shift = 37;
    cdrip::Job shiftJob = job;
    shiftJob.folder = shiftOut;
    shiftJob.offset = 0;  // DB pressing sits +37 samples relative to uncalibrated drive
    shiftJob.arPressings = cdrip::ParseAccurateRip(
        encodeRecord(arIds, {{6, 0x12345678u, ref0.f450}, {6, 0x23456789u, ref1.f450}, {6, 0x3456789au, ref2.f450}}), 3);
    const cdrip::Result shifted = cdrip::Rip(shiftedDrive, shiftJob, cancelOp);
    g.Add(shifted.tracks.size() == 3 &&
              shifted.tracks[0].ar.status == cdrip::ArStatus::MatchedOffset && shifted.tracks[0].ar.matchedOffset == 37 &&
              cdrip::DoneText(shifted).find(L"pressing offset") != std::wstring::npos,
          "Rip detects different-pressing frame-450 offset matches when primary CRCs differ");

    return g;
}

inline std::vector<Group> All(const std::wstring& scratch) {
    SHCreateDirectoryExW(nullptr, scratch.c_str(), nullptr);
    return {DiscIds(), CdText(), Lookups(), Reading(), Rips(scratch), AccurateRip(scratch)};
}

}  // namespace cdrip_checks
