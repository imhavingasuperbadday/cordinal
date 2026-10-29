// Chiptune checks of the self test (see selftest.cpp): the gzip inflater and the game-music
// decoders (NSFE, VGM/VGZ, SPC, GBS, GYM) against fixtures written in code.
#include "selftest_common.h"

namespace {

// Chiptune fixtures, written in code: a three-song NSFE with track times and labels, a 10 s VGM
// with a PSG tone (and the same VGM in gzip, as VGZ), and silent SPC, GBS and GYM files.
std::string GmNulStrings(std::initializer_list<const char*> items) {
    std::string s;
    for (const char* t : items) {
        s += t;
        s += '\0';
    }
    return s;
}

char GmByte(unsigned v) { return (char)(v & 0xFF); }

void GmLe16(std::string& s, unsigned v) {
    s += (char)(v & 0xFF);
    s += (char)((v >> 8) & 0xFF);
}

void GmLe32(std::string& s, uint32_t v) {
    GmLe16(s, v & 0xFFFF);
    GmLe16(s, v >> 16);
}

void GmPut32(std::string& s, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) s[at + i] = (char)((v >> (8 * i)) & 0xFF);
}

void GmPlace(std::string& s, size_t at, const char* text) { std::memcpy(&s[at], text, std::strlen(text)); }

std::string GmNsfe() {
    std::string f = "NSFE";
    auto chunk = [&](const char* tag, const std::string& data) {
        GmLe32(f, (uint32_t)data.size());
        f += tag;
        f += data;
    };
    std::string info;
    GmLe16(info, 0x8000);  // load address
    GmLe16(info, 0x8000);  // init: RTS
    GmLe16(info, 0x8001);  // play: RTS
    info += '\0';          // NTSC
    info += '\0';          // no expansion chip
    info += (char)3;       // songs
    info += '\0';          // first song
    info.append(6, '\0');
    chunk("INFO", info);
    chunk("auth", GmNulStrings({"Test Game", "Test Author", "1991 Test Co", "Tester"}));
    std::string times;
    GmLe32(times, 1000);
    GmLe32(times, 2500);
    GmLe32(times, 4000);
    chunk("time", times);
    chunk("tlbl", GmNulStrings({"First", "", "Third"}));
    chunk("DATA", std::string(2, '\x60'));
    chunk("NEND", "");
    return f;
}

// A VGM whose SN76489 plays a 440 Hz square wave at full volume for `samples` at 44100 Hz.
std::string GmVgm(uint32_t samples) {
    std::string v(0x40, '\0');
    GmPlace(v, 0, "Vgm ");
    GmPut32(v, 0x08, 0x150);    // version 1.50
    GmPut32(v, 0x0C, 3579545);  // SN76489 clock
    GmPut32(v, 0x18, samples);  // total samples
    GmPut32(v, 0x34, 0x0C);     // data at 0x40
    v += "\x50\x90\x50\x8E\x50\x0F";  // channel 0: full volume, tone 254 (440 Hz)
    for (uint32_t left = samples; left > 0;) {
        uint32_t n = std::min<uint32_t>(left, 65535);
        v += (char)0x61;  // wait n samples
        GmLe16(v, n);
        left -= n;
    }
    v += (char)0x66;
    GmPut32(v, 0x04, (uint32_t)v.size() - 4);
    return v;
}

uint32_t GmCrc32(const std::string& s) {
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : s) {
        c ^= b;
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

// gzip with one stored (uncompressed) deflate block, so the payload must be under 64 KB.
std::string GmGzipStored(const std::string& data) {
    std::string g("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\xff", 10);
    g += (char)0x01;  // final block, stored
    GmLe16(g, (unsigned)data.size());
    GmLe16(g, (unsigned)data.size() ^ 0xFFFF);
    g += data;
    GmLe32(g, GmCrc32(data));
    GmLe32(g, (uint32_t)data.size());
    return g;
}

// SPC700 snapshot: RAM, DSP and registers all zero (silence), with ID666 text tags.
std::string GmSpc() {
    std::string s(0x10200, '\0');
    GmPlace(s, 0, "SNES-SPC700 Sound File Data v0.30");
    s[33] = 0x1A;
    s[34] = 0x1A;
    s[0x23] = 26;  // ID666 tag present
    s[0x24] = 30;
    s[0x25] = 0x00;  // PC = 0x0200
    s[0x26] = 0x02;
    s[0x2B] = GmByte(0xEF);  // SP
    GmPlace(s, 0x2E, "Test SPC Song");
    GmPlace(s, 0x4E, "Test SPC Game");
    GmPlace(s, 0x6E, "Tester");
    GmPlace(s, 0x9E, "10/14/2026");
    GmPlace(s, 0xA9, "010");  // length in seconds, as text
    GmPlace(s, 0xAC, "0000");
    GmPlace(s, 0xB0, "Test SPC Author");
    return s;
}

// GBS with three songs whose init and play routines just return.
std::string GmGbs() {
    std::string g(0x70, '\0');
    GmPlace(g, 0, "GBS");
    g[3] = 1;
    g[4] = 3;                                 // songs
    g[5] = 1;                                 // first song
    g[6] = 0x00, g[7] = 0x04;                 // load 0x0400
    g[8] = 0x00, g[9] = 0x04;                 // init
    g[10] = 0x01, g[11] = 0x04;               // play
    g[12] = GmByte(0xFE), g[13] = GmByte(0xFF);  // stack
    GmPlace(g, 16, "Test GBS");
    GmPlace(g, 48, "Test GBS Author");
    GmPlace(g, 80, "1992 Test Co");
    g += "\xC9\xC9";  // RET, RET
    return g;
}

// GYM: a 428-byte header and one wait per frame (1/60 s), with no register writes.
std::string GmGym(size_t frames) {
    std::string g(428, '\0');
    GmPlace(g, 0, "GYMX");
    GmPlace(g, 4, "Test GYM Song");
    GmPlace(g, 36, "Test GYM Game");
    GmPlace(g, 68, "1993 Test Co");
    GmPlace(g, 132, "Tester");
    g.append(frames, '\0');
    return g;
}

double GmRms(const std::vector<float>& s, size_t fromFrame, size_t toFrame) {
    double sum = 0;
    size_t n = 0;
    for (size_t i = fromFrame * 2; i < toFrame * 2 && i < s.size(); ++i, ++n) sum += (double)s[i] * s[i];
    return n ? std::sqrt(sum / n) : 0;
}

bool GmFinite(const std::vector<float>& s) {
    return std::all_of(s.begin(), s.end(), [](float v) { return std::isfinite(v) && std::fabs(v) <= 1.0f; });
}

std::vector<float> GmReadAll(Decoder& d) {
    std::vector<float> all, buf(4096 * 2);
    while (size_t n = d.Read(buf.data(), 4096)) all.insert(all.end(), buf.begin(), buf.begin() + n * 2);
    return all;
}

// gzip test vectors from Python's gzip.compress at level 9: one fixed-Huffman block, and one
// dynamic-Huffman block with long matches.
const char kGzFixed[] =
    "\x1f\x8b\x08\x00\x00\x00\x00\x00\x02\xff\x73\xce\x2f\x4a\xc9\xcc\x4b\xcc\x51\x28\xc8\x49\xac\x2c"
    "\x56\x48\xce\xc8\x2c\x28\x29\xcd\x4b\x2d\xd6\x53\x70\x26\x43\x06\x00\x91\xe2\x0d\x80\x4e\x00\x00"
    "\x00";
const char kGzDynamic[] =
    "\x1f\x8b\x08\x00\x00\x00\x00\x00\x02\xff\xed\xd3\x47\x0e\xc2\x40\x10\x44\xd1\xab\x0c\x60\x92\x49"
    "\x26\xe7\x60\x72\xce\x19\x8e\x80\x04\xf7\x5f\xa1\x19\x69\xac\x7f\x04\x84\x7a\xd3\xab\x56\xd5\xdb"
    "\x94\x8a\x67\xcb\xad\xc1\x7c\x77\x0d\x27\xbd\x6a\x77\xbc\x3a\x3e\xa2\xe9\x62\xc3\x9f\x6e\xce\xa1"
    "\x44\xae\xd2\x1e\x2e\xf6\xb7\x88\x9b\xaf\xf5\x26\xeb\xd3\x33\x96\x29\x35\xfb\xb3\xed\x45\xbf\x76"
    "\x46\xcb\xc3\xdd\x49\x15\xea\xfa\x55\xd9\x10\xfd\x1a\x84\xe8\xd7\x20\x44\xbf\x06\x21\xb6\xcf\x84"
    "\xd8\x3e\x13\x62\xfb\x4c\x88\xed\x33\x21\xb6\xcf\x84\x10\xdd\x21\x5a\x11\xdd\x25\x3a\x44\x74\x8f"
    "\xe8\x30\xd1\x3e\xd1\x11\xa2\xfb\x44\x3b\x44\x0f\x88\x8e\x12\x3d\x24\x3a\x46\xf4\x88\xe8\x38\xd1"
    "\x63\xa2\x13\x44\x4f\x88\x4e\x12\x3d\x25\xda\x25\x7a\x46\x74\x8a\xe8\x39\xd1\x69\xa2\x17\x44\x67"
    "\x88\x5e\x12\x9d\x25\x7a\x45\x74\x8e\xe8\x35\xd1\x1e\xd1\x1b\xa2\xf3\x44\x6f\x89\x2e\x10\xbd\x23"
    "\xba\x48\xf4\x9e\xe8\x12\xd1\x07\xa2\xcb\x44\x1f\x89\xae\x10\x7d\x52\x32\x0c\x19\x86\x0c\x43\x86"
    "\x21\xc3\x90\x61\xc8\x30\x64\x18\xbf\x35\x8c\xd7\xfb\xfd\x51\x72\xe4\xc8\xf9\xeb\xf3\x05\xf7\x31"
    "\xf4\xb6\xa0\x0f\x00\x00";
}  // namespace

void TestInflate() {
    auto gunzip = [](const std::string& gz, std::string& out, size_t maxOut = 1 << 20) {
        return Gunzip(reinterpret_cast<const unsigned char*>(gz.data()), gz.size(), maxOut, out);
    };
    std::string fixedExpect, dynamicExpect;
    for (int i = 0; i < 3; ++i) fixedExpect += "Cordinal plays chiptunes. ";
    for (int i = 0; i < 3000; ++i) dynamicExpect += (char)((i * 7 + i / 13) % 61 + 32);
    for (int i = 0; i < 200; ++i) dynamicExpect += "loop ";
    const std::string fixedGz(kGzFixed, sizeof kGzFixed - 1), dynamicGz(kGzDynamic, sizeof kGzDynamic - 1);

    std::string out;
    bool fixedOk = gunzip(fixedGz, out) && out == fixedExpect;
    bool dynamicOk = gunzip(dynamicGz, out) && out == dynamicExpect;
    Check(fixedOk && dynamicOk, L"gzip fixed- and dynamic-Huffman blocks inflate to the original bytes");

    // A file name in the header (FNAME) is skipped.
    std::string named = dynamicGz;
    named[3] = 8;
    named.insert(10, std::string("song.vgm\0", 9));
    Check(gunzip(named, out) && out == dynamicExpect, L"gzip header with a file name");

    // Every truncation fails, a flipped bit fails (CRC or structure), and the size limit holds.
    bool truncOk = true;
    for (size_t cut = 0; cut < dynamicGz.size(); ++cut)
        if (gunzip(dynamicGz.substr(0, cut), out)) truncOk = false;
    bool flipOk = true;
    for (size_t at = 10; at < dynamicGz.size(); ++at) {
        if (at == dynamicGz.size() - 9) continue;  // the last deflate byte, whose top bits are padding

        std::string bad = dynamicGz;
        bad[at] ^= 0x10;
        if (gunzip(bad, out) && out == dynamicExpect) flipOk = false;
    }
    Check(truncOk && flipOk && !gunzip(dynamicGz, out, dynamicExpect.size() - 1) && gunzip(dynamicGz, out, dynamicExpect.size()),
          L"gzip rejects truncation, damage and output past the limit");
    const unsigned char check[] = "123456789";
    Check(Crc32(check, 9) == 0xCBF43926u, L"CRC-32 check value");
}

// Game music from fixtures written in code: an NSFE with three timed songs, a plain and gzip-wrapped
// VGM, and silent SPC/GBS/GYM files. Checks the sub-track suffix parser, per-song tags, decode
// length and seek, plus that damaged or truncated files fail cleanly instead of crashing.
void TestGameMusic(const std::wstring& dir) {
    auto write = [&](const wchar_t* name, const std::string& bytes) {
        std::wstring path = dir + L"\\" + name;
        WriteTestBytes(path, bytes);
        return path;
    };
    std::wstring file, error;
    int song = 0;
    bool split = SplitGameMusicPath(L"C:\\Music\\zelda.nsf|3", file, song) && song == 3 && file == L"C:\\Music\\zelda.nsf";
    bool plain = SplitGameMusicPath(L"C:\\Music\\zelda.nsf", file, song) && song == 1;
    bool malformed = !SplitGameMusicPath(L"C:\\Music\\zelda.nsf|0", file, song) &&
                     !SplitGameMusicPath(L"C:\\Music\\zelda.nsf|", file, song) &&
                     !SplitGameMusicPath(L"C:\\Music\\zelda.nsf|12345", file, song);
    bool names = IsGameMusicFile(L"a.SPC") && IsGameMusicFile(L"a.nsfe|2") && IsGameMusicFile(L"a.gym") &&
                 IsGameMusicFile(L"a.vgz") && !IsGameMusicFile(L"a.mp3") && !IsGameMusicFile(L"a.nsf|x");
    Check(split && plain && malformed && names, L"GME names and subtrack suffixes parse (\"x.nsf|3\" is song 3)");

    // NSFE: three songs, times 1 s / 2.5 s / 4 s, the second untitled.
    const std::wstring nsfe = write(L"gm_test.nsfe", GmNsfe());
    Check(GameMusicTrackCount(nsfe) == 3, L"GME NSFE track count %d (expect 3)", GameMusicTrackCount(nsfe));
    TagData t;
    bool tagged = ReadTags(nsfe + L"|2", t);
    Check(tagged && t.title == "Track 2" && t.album == "Test Game" && t.artist == "Test Author" &&
              t.albumArtist == "Test Author" && t.date == "1991" && t.genre == "Game" && t.track == 2 && t.trackTotal == 3 &&
              std::fabs(t.duration - 2.5) < 1e-6 && t.sampleRate == 44100 && t.channels == 2 && t.codec == Codec::NSF &&
              t.comment.find("dumped by Tester") != std::string::npos,
          L"GME NSFE tags, song 2: title=%ls album=%ls year=%ls track=%u/%u dur=%.3f comment=%ls", FromUtf8(t.title).c_str(),
          FromUtf8(t.album).c_str(), FromUtf8(t.date).c_str(), t.track, t.trackTotal, t.duration,
          FromUtf8(t.comment).c_str());
    TagData first;
    bool firstRead = ReadTags(nsfe, first);  // read before Check: its arguments may be evaluated first
    Check(firstRead && first.title == "First" && first.track == 1, L"GME NSFE plain path is song 1 (%ls)",
          FromUtf8(first.title).c_str());
    unsigned rate = 0;
    auto all1 = DecodeAll(nsfe, rate);
    auto all2 = DecodeAll(nsfe + L"|2", rate);
    auto all3 = DecodeAll(nsfe + L"|3", rate);
    auto trackDecoder = OpenTrackDecoder(nsfe + L"|2", 48000, error);
    Check(trackDecoder && GmReadAll(*trackDecoder) == all2,
          L"GME track decoder used by loudness scans selects the game-music subsong without a CUE sheet");
    Check(all1.size() / 2 == 44100 && all2.size() / 2 == 110250 && all3.size() / 2 == 176400 && rate == 44100 &&
              GmFinite(all2) && GmFinite(all3),
          L"GME NSFE songs decode to their times: %zu / %zu / %zu frames (expect 44100 / 110250 / 176400)", all1.size() / 2,
          all2.size() / 2, all3.size() / 2);
    auto seeking = OpenGameMusicDecoder(nsfe, 3, error);
    bool seekOk = seeking && seeking->Seek(2.5);
    size_t afterSeek = seekOk ? GmReadAll(*seeking).size() / 2 : 0;
    Check(seekOk && afterSeek == 66150, L"GME NSFE seek to 2.5 s leaves %zu frames (expect 66150)", afterSeek);
    std::wstring badError;
    Check(!OpenGameMusicDecoder(nsfe, 4, badError) && !badError.empty() && !OpenDecoder(nsfe + L"|4", 48000, badError) &&
              !OpenDecoder(nsfe + L"|0", 48000, badError),
          L"GME NSFE song 4 and suffix |0 are refused");

    // VGM: a 10 s tone, plain and gzip-wrapped. The tone fades over the last 8 s.
    const std::string vgmBytes = GmVgm(441000);
    const std::wstring vgm = write(L"gm_test.vgm", vgmBytes);
    const std::wstring vgz = write(L"gm_test.vgz", GmGzipStored(vgmBytes));
    Check(GameMusicTrackCount(vgm) == 1 && GameMusicTrackCount(vgz) == 1, L"GME VGM and VGZ each have one song");
    auto plainPcm = DecodeAll(vgm, rate);
    auto zipPcm = DecodeAll(vgz, rate);
    const double mid = GmRms(plainPcm, 44100, 132300), tail = GmRms(plainPcm, 441000 - 4410, 441000);
    Check(plainPcm.size() / 2 == 441000 && rate == 44100 && GmFinite(plainPcm) && mid > 0.01 && tail < mid * 0.02,
          L"GME VGM 10 s tone: %zu frames, RMS at 1-3 s %.4f, last 0.1 s %.6f", plainPcm.size() / 2, mid, tail);
    Check(plainPcm == zipPcm, L"GME VGZ decodes exactly like the same VGM");
    TagData vt;
    bool vgzRead = ReadTags(vgz, vt);
    Check(vgzRead && std::fabs(vt.duration - 10.0) < 1e-6 && vt.codec == Codec::VGM && vt.genre == "Game",
          L"GME VGZ tags: duration %.3f codec %ls", vt.duration, CodecName(vt.codec));

    // Seeking into the fade, then back to the start, where the fade has to come back too.
    auto vd = OpenGameMusicDecoder(vgm, 1, error);
    bool seekFade = vd && vd->Seek(9.5);
    double lateRms = 0;
    if (seekFade) {
        std::vector<float> late(4410 * 2);
        size_t got = vd->Read(late.data(), 4410);
        lateRms = GmRms(late, 0, got);
    }
    bool restarted = seekFade && vd->Seek(0.0);
    auto again = restarted ? GmReadAll(*vd) : std::vector<float>();
    Check(seekFade && restarted && lateRms < mid * 0.02 && again.size() / 2 == 441000 && GmRms(again, 0, 44100) > 0.01 &&
              GmRms(again, 441000 - 4410, 441000) < mid * 0.02,
          L"GME VGM seek into the fade and back: late RMS %.6f, after restart %zu frames, fade reapplied", lateRms,
          again.size() / 2);

    // One song each: tags and a length from the file itself.
    const std::wstring spc = write(L"gm_test.spc", GmSpc());
    TagData st;
    bool spcRead = ReadTags(spc, st);
    Check(spcRead && st.title == "Test SPC Song" && st.album == "Test SPC Game" && st.artist == "Test SPC Author" &&
              st.codec == Codec::SPC && std::fabs(st.duration - 10.0) < 1e-6 && st.comment.find("dumped by Tester") != std::string::npos,
          L"GME SPC tags: title=%ls dur=%.3f", FromUtf8(st.title).c_str(), st.duration);
    auto spcPcm = DecodeAll(spc, rate);
    Check(GmFinite(spcPcm) && !spcPcm.empty(), L"GME SPC decodes finite samples (%zu frames of 441000: a silent song ends when its fade starts, as in gme)", spcPcm.size() / 2);

    const std::wstring gbs = write(L"gm_test.gbs", GmGbs());
    TagData gt;
    bool gbsRead = ReadTags(gbs + L"|3", gt);
    Check(GameMusicTrackCount(gbs) == 3 && gbsRead && gt.title == "Track 3" && gt.album == "Test GBS" &&
              gt.date == "1992" && gt.codec == Codec::GBS && std::fabs(gt.duration - 150.0) < 1e-6,
          L"GME GBS has 3 songs; song 3: title=%ls dur=%.1f", FromUtf8(gt.title).c_str(), gt.duration);
    auto gbsDec = OpenGameMusicDecoder(gbs, 3, error);
    std::vector<float> gbsHead(4410 * 2);
    size_t gbsGot = gbsDec ? gbsDec->Read(gbsHead.data(), 4410) : 0;
    Check(gbsGot == 4410 && GmFinite(gbsHead) && gbsDec->Seek(1.0), L"GME GBS song 3 plays and seeks (%zu frames)", gbsGot);

    const std::wstring gym = write(L"gm_test.gym", GmGym(600));
    TagData yt;
    bool gymRead = ReadTags(gym, yt);
    Check(GameMusicTrackCount(gym) == 1 && gymRead && yt.album == "Test GYM Game" && yt.date == "1993" &&
              std::fabs(yt.duration - 10.0) < 1e-6,
          L"GME GYM tags: album=%ls dur=%.3f", FromUtf8(yt.album).c_str(), yt.duration);
    auto gymPcm = DecodeAll(gym, rate);
    Check(GmFinite(gymPcm) && !gymPcm.empty(), L"GME GYM decodes finite samples (%zu frames of 441000: a silent song ends when its fade starts, as in gme)", gymPcm.size() / 2);

    // Damaged files: truncated at several lengths, and bytes flipped at pseudo-random places.
    // Each must fail or decode cleanly, never crash. Truncated VGZ must fail, since its gzip
    // size and CRC no longer match.
    struct Fixture {
        const wchar_t* name;
        std::string bytes;
    };
    const std::vector<Fixture> fixtures = {{L"gm_bad.nsfe", GmNsfe()}, {L"gm_bad.vgm", vgmBytes},
                                           {L"gm_bad.vgz", GmGzipStored(vgmBytes)}, {L"gm_bad.spc", GmSpc()},
                                           {L"gm_bad.gbs", GmGbs()}, {L"gm_bad.gym", GmGym(600)}};
    int opened = 0, tried = 0;
    bool truncatedOk = true;
    for (const Fixture& fx : fixtures) {
        const size_t size = fx.bytes.size();
        const bool gzipped = std::wstring(fx.name).find(L".vgz") != std::wstring::npos;
        for (size_t cut : {(size_t)0, (size_t)3, (size_t)16, size / 2, size - 1}) {
            std::wstring path = write(fx.name, fx.bytes.substr(0, cut));
            ++tried;
            int songs = GameMusicTrackCount(path);
            auto d = OpenGameMusicDecoder(path, 1, error);
            if (d) {
                ++opened;
                std::vector<float> head(2048 * 2);
                d->Read(head.data(), 2048);
                d->Seek(0.5);
            }
            if (cut == 0 && (songs != 0 || d)) truncatedOk = false;
            if (gzipped && cut < size && (songs != 0 || d)) truncatedOk = false;
        }
        unsigned seed = 12345;
        for (int round = 0; round < 24; ++round) {
            std::string bytes = fx.bytes;
            for (int k = 0; k < 3; ++k) {
                seed = seed * 1103515245u + 12345u;
                bytes[(seed >> 8) % size] ^= (char)(1 + (seed >> 24) % 255);
            }
            std::wstring path = write(fx.name, bytes);
            ++tried;
            GameMusicTrackCount(path);
            if (auto d = OpenGameMusicDecoder(path, 1, error)) {
                ++opened;
                std::vector<float> head(2048 * 2);
                d->Read(head.data(), 2048);
                d->Seek(0.25);
            }
        }
    }
    // One flipped byte inside the gzip payload: the CRC must catch it.
    std::string zipBad = GmGzipStored(vgmBytes);
    zipBad[15 + 50] ^= 0x40;
    const std::wstring zipBadPath = write(L"gm_bad_crc.vgz", zipBad);
    Check(truncatedOk && GameMusicTrackCount(zipBadPath) == 0 && !OpenGameMusicDecoder(zipBadPath, 1, error),
          L"GME damaged files: %d of %d opened without a crash; truncated and flipped VGZ refused", opened, tried);
}
