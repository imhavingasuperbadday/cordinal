// Checks of the formats added for game-music packs and older music: songs inside ZIP archives, DSD
// (DSF and DSDIFF), tracker modules (MOD, S3M, XM, IT), Commodore 64 SID tunes and MIDI through a
// SoundFont. Every fixture is written here in code: a tone whose pitch, length and level the
// decoders must reproduce.
#include "selftest_common.h"

#include "archive.h"
#include "chiptune/cpu6502.h"
#include "native_audio.h"

namespace {

void Le16(std::string& s, unsigned v) {
    s += (char)(v & 0xFF);
    s += (char)(v >> 8 & 0xFF);
}
void Le32(std::string& s, uint32_t v) {
    Le16(s, v & 0xFFFF);
    Le16(s, v >> 16);
}
void Le64(std::string& s, uint64_t v) {
    Le32(s, (uint32_t)v);
    Le32(s, (uint32_t)(v >> 32));
}
void Be16(std::string& s, unsigned v) {
    s += (char)(v >> 8 & 0xFF);
    s += (char)(v & 0xFF);
}
void Be32(std::string& s, uint32_t v) {
    Be16(s, v >> 16);
    Be16(s, v & 0xFFFF);
}
void Be64(std::string& s, uint64_t v) {
    Be32(s, (uint32_t)(v >> 32));
    Be32(s, (uint32_t)v);
}
std::string Padded(const char* text, size_t n) {
    std::string s(text);
    s.resize(n, '\0');
    return s;
}
void Put16(std::string& s, size_t at, unsigned v) {
    s[at] = (char)(v & 0xFF);
    s[at + 1] = (char)(v >> 8 & 0xFF);
}
void Put32(std::string& s, size_t at, uint32_t v) {
    Put16(s, at, v & 0xFFFF);
    Put16(s, at + 2, v >> 16);
}

// Sign changes of the left channel over [from, to) seconds, as a frequency in Hz.
double Pitch(const std::vector<float>& pcm, unsigned rate, double from, double to) {
    const size_t a = (size_t)(from * rate), b = std::min(pcm.size() / 2, (size_t)(to * rate));
    int crossings = 0;
    bool high = false, known = false;
    for (size_t i = a; i < b; ++i) {
        const float v = pcm[i * 2];
        if (std::abs(v) < 1e-4f) continue;
        if (known && (v > 0) != high) ++crossings;
        high = v > 0;
        known = true;
    }
    return b > a ? crossings / 2.0 / ((double)(b - a) / rate) : 0;
}

double Rms(const std::vector<float>& pcm, unsigned rate, double from, double to, int channel = 0) {
    const size_t a = (size_t)(from * rate), b = std::min(pcm.size() / 2, (size_t)(to * rate));
    double sum = 0;
    for (size_t i = a; i < b; ++i) sum += (double)pcm[i * 2 + channel] * pcm[i * 2 + channel];
    return b > a ? std::sqrt(sum / (b - a)) : 0;
}

bool Near(double value, double want, double tolerance) { return std::abs(value - want) <= tolerance * want; }

// --- ZIP ---

// A ZIP of stored entries, or of deflate streams made of stored blocks (method 8).
std::string Zip(const std::vector<std::pair<std::string, std::string>>& files, bool deflate) {
    std::string out, dir;
    for (const auto& [name, data] : files) {
        std::string packed = data;
        if (deflate) {
            packed.clear();
            for (size_t at = 0;;) {
                const size_t n = std::min<size_t>(65535, data.size() - at);
                packed += (char)(at + n == data.size() ? 1 : 0);
                Le16(packed, (unsigned)n);
                Le16(packed, (unsigned)~n & 0xFFFF);
                packed += data.substr(at, n);
                at += n;
                if (at == data.size()) break;
            }
        }
        const uint32_t crc = Crc32((const unsigned char*)data.data(), data.size());
        auto header = [&](bool central) {
            std::string h;
            Le32(h, central ? 0x02014B50 : 0x04034B50);
            if (central) Le16(h, 20);
            Le16(h, 20);
            Le16(h, 0x800);  // UTF-8 names
            Le16(h, deflate ? 8 : 0);
            Le32(h, 0);  // time and date
            Le32(h, crc);
            Le32(h, (uint32_t)packed.size());
            Le32(h, (uint32_t)data.size());
            Le16(h, (unsigned)name.size());
            Le16(h, 0);
            if (central) {
                Le32(h, 0);  // comment, disk
                Le16(h, 0);
                Le32(h, 0);  // attributes
                Le32(h, (uint32_t)out.size());
            }
            return h + name;
        };
        dir += header(true);
        out += header(false) + packed;
    }
    const uint32_t at = (uint32_t)out.size();
    out += dir;
    Le32(out, 0x06054B50);
    Le32(out, 0);
    Le16(out, (unsigned)files.size());
    Le16(out, (unsigned)files.size());
    Le32(out, (uint32_t)dir.size());
    Le32(out, at);
    Le16(out, 0);
    return out;
}

// --- SID ---

// A two-song PSID: init sets a pulse tone on voice 1 (song 2 an octave up); play does nothing.
std::string PsidTone() {
    std::string s = "PSID";
    Be16(s, 2);
    Be16(s, 0x7C);
    Be16(s, 0x1000);  // load
    Be16(s, 0x1000);  // init
    Be16(s, 0x1003);  // play
    Be16(s, 2);       // songs
    Be16(s, 1);
    Be32(s, 0);
    s += Padded("cordinal test", 32) + Padded("Tester", 32) + Padded("2026 Test Co", 32);
    Be16(s, 0x14);  // PAL, 6581
    Be32(s, 0);
    static const uint8_t code[] = {
        0x4C, 0x04, 0x10,  // init: JMP setup
        0x60,              // play: RTS
        0xA8,              // setup: TAY
        0xA9, 0x0F, 0x8D, 0x18, 0xD4, 0xA9, 0x00, 0x8D, 0x05, 0xD4, 0xA9, 0xF0, 0x8D, 0x06, 0xD4,
        0xA9, 0x00, 0x8D, 0x02, 0xD4, 0xA9, 0x08, 0x8D, 0x03, 0xD4, 0xA9, 0xD6, 0x8D, 0x00, 0xD4,
        0xB9, 0x2F, 0x10, 0x8D, 0x01, 0xD4,  // frequency high byte from the table
        0xA9, 0x41, 0x8D, 0x04, 0xD4,        // pulse, gate on
        0x60, 0x1C, 0x38,
    };
    s.append((const char*)code, sizeof code);
    return s;
}

// The same tone, but the gate opens only in a CIA timer interrupt the tune installs (play is 0).
std::string PsidInterrupt() {
    std::string s = PsidTone();
    s[0x0C] = s[0x0D] = 0;  // play 0
    s[0x0F] = 1;            // one song
    s.resize(0x7C);
    static const uint8_t code[] = {
        0x78, 0xA9, 0x20, 0x8D, 0x14, 0x03, 0xA9, 0x10, 0x8D, 0x15, 0x03, 0xA9, 0x0F, 0x8D, 0x18, 0xD4,
        0xA9, 0xF0, 0x8D, 0x06, 0xD4, 0xA9, 0x08, 0x8D, 0x03, 0xD4, 0x8D, 0x01, 0xD4, 0x58, 0x60, 0xEA,
        0xA9, 0x41, 0x8D, 0x04, 0xD4, 0x4C, 0x31, 0xEA,  // the handler: gate on, then the KERNAL's exit
    };
    s.append((const char*)code, sizeof code);
    return s;
}

struct RamBus : Cpu6502::Bus {
    uint8_t ram[0x10000] = {};
    uint8_t Read(uint16_t a) override { return ram[a]; }
    void Write(uint16_t a, uint8_t v, uint32_t) override { ram[a] = v; }
};

// --- DSD ---

// One second of DSD64: a 1 kHz sine at half scale on the left, its inverse on the right, from a
// second-order sigma-delta modulator. Bits per channel, in time order.
std::vector<uint8_t> DsdBits(size_t count) {
    std::vector<uint8_t> bits(count);
    double i1 = 0, i2 = 0;
    for (size_t n = 0; n < count; ++n) {
        const double x = 0.5 * std::sin(2 * 3.14159265358979 * 1000 * n / 2822400.0);
        const double y = i2 >= 0 ? 1 : -1;
        i1 += x - y;
        i2 += i1 - y;
        bits[n] = y > 0;
    }
    return bits;
}

std::string Dsf(const std::vector<uint8_t>& bits) {
    const size_t bytes = (bits.size() + 7) / 8, blocks = (bytes + 4095) / 4096;
    std::string data;
    for (size_t b = 0; b < blocks; ++b)
        for (int c = 0; c < 2; ++c)
            for (size_t k = 0; k < 4096; ++k) {
                uint8_t v = 0;
                const size_t byte = b * 4096 + k;
                for (int j = 0; j < 8; ++j) {
                    const size_t n = byte * 8 + j;
                    if (n < bits.size() && (bits[n] ^ c)) v |= (uint8_t)(1 << j);  // LSB first
                }
                data += (char)v;
            }
    std::string f = "DSD ";
    Le64(f, 28);
    Le64(f, 28 + 52 + 12 + data.size());
    Le64(f, 0);
    f += "fmt ";
    Le64(f, 52);
    Le32(f, 1);
    Le32(f, 0);
    Le32(f, 2);  // stereo
    Le32(f, 2);
    Le32(f, 2822400);
    Le32(f, 1);
    Le64(f, bits.size());
    Le32(f, 4096);
    Le32(f, 0);
    f += "data";
    Le64(f, 12 + data.size());
    return f + data;
}

std::string Dff(const std::vector<uint8_t>& bits) {
    const size_t bytes = bits.size() / 8;
    std::string sound;
    for (size_t k = 0; k < bytes; ++k)
        for (int c = 0; c < 2; ++c) {
            uint8_t v = 0;
            for (int j = 0; j < 8; ++j)
                if (bits[k * 8 + j] ^ c) v |= (uint8_t)(0x80 >> j);  // MSB first
            sound += (char)v;
        }
    std::string prop = "SND ";
    prop += "FS  ";
    Be64(prop, 4);
    Be32(prop, 2822400);
    prop += "CHNL";
    Be64(prop, 10);
    Be16(prop, 2);
    prop += "SLFTSRGT";
    prop += "CMPR";
    Be64(prop, 20);
    prop += "DSD ";
    prop += (char)14;
    prop += "not compressed";
    prop += '\0';
    std::string body = "DSD ";
    body += "FVER";
    Be64(body, 4);
    Be32(body, 0x01050000);
    body += "PROP";
    Be64(body, prop.size());
    body += prop;
    body += "DSD ";
    Be64(body, sound.size());
    body += sound;
    std::string f = "FRM8";
    Be64(f, body.size());
    return f + body;
}

// --- Tracker modules ---
// Each plays a 32-sample square wave at middle C (C-2 on the Amiga) for four rows of speed 6 at
// 125 BPM, then jumps back to the start: 0.48 s, where the song would repeat.

std::string SquareSigned8() {
    std::string s;
    for (int i = 0; i < 32; ++i) s += (char)(i < 16 ? 100 : -100);
    return s;
}

std::string Mod() {
    std::string m = Padded("mod test", 20);
    for (int i = 0; i < 31; ++i) {
        std::string h = Padded(i ? "" : "square", 22);
        Be16(h, i ? 0 : 16);
        h += (char)0;
        h += (char)(i ? 0 : 64);
        Be16(h, 0);
        Be16(h, i ? 1 : 16);
        m += h;
    }
    m += (char)1;
    m += (char)127;
    m += std::string(128, '\0');
    m += "M.K.";
    std::string pat(64 * 4 * 4, '\0');
    pat[0] = 0x01;  // sample 1, period 428
    pat[1] = '\xAC';
    pat[2] = 0x10;
    pat[3 * 16 + 2] = 0x0B;  // row 3: B00
    return m + pat + SquareSigned8();
}

std::string S3m() {
    std::string s = Padded("s3m test", 28);
    s += (char)0x1A;
    s += (char)16;
    s += std::string(2, '\0');
    Le16(s, 2);       // orders
    Le16(s, 1);       // instruments
    Le16(s, 1);       // patterns
    Le16(s, 0);
    Le16(s, 0x1320);
    Le16(s, 2);       // unsigned samples
    s += "SCRM";
    s += (char)64;
    s += (char)6;
    s += (char)125;
    s += '\xB0';  // stereo
    s += std::string(12, '\0');
    s += (char)0;      // channel 1: left
    s += (char)8;      // channel 2: right
    s += std::string(30, '\xFF');
    s += (char)0;
    s += '\xFF';    // orders
    Le16(s, 7);        // instrument at 0x70
    Le16(s, 14);       // pattern at 0xE0
    s.resize(0x70, '\0');
    std::string ins(0x50, '\0');
    ins[0] = 1;
    Put16(ins, 0x0E, 12);  // data at 0xC0
    Put32(ins, 0x10, 32);
    Put32(ins, 0x18, 32);
    ins[0x1C] = 64;
    ins[0x1F] = 1;  // loop
    Put32(ins, 0x20, 8363);
    std::memcpy(&ins[0x4C], "SCRS", 4);
    s += ins;
    for (int i = 0; i < 32; ++i) s += (char)(i < 16 ? 228 - 256 : 28);
    s.resize(0xE0, '\0');
    std::string pat;
    pat += "\x60\x40\x01\x40";  // row 0: C-4, instrument 1, volume 64
    pat += '\0';
    pat += '\0';
    pat += '\0';
    pat += "\x80\x02";          // row 3: B00
    pat += '\0';
    pat += '\0';
    pat += std::string(60, '\0');
    std::string packed;
    Le16(packed, (unsigned)pat.size() + 2);
    return s + packed + pat;
}

std::string Xm() {
    std::string x = "Extended Module: " + Padded("xm test", 20);
    x += (char)0x1A;
    x += Padded("cordinal", 20);
    Le16(x, 0x0104);
    Le32(x, 276);
    Le16(x, 1);  // song length
    Le16(x, 0);
    Le16(x, 2);  // channels
    Le16(x, 1);  // patterns
    Le16(x, 1);  // instruments
    Le16(x, 1);  // linear
    Le16(x, 6);
    Le16(x, 125);
    x += std::string(256, '\0');
    // Row 0: C-4, instrument 1, volume 64 on channel 1; rows 1 and 2 empty; row 3: B00.
    const char rows[] = {'\x87', 0x31, 0x01, 0x50, '\x80', '\x80', '\x80', '\x80', '\x80', '\x98', 0x0B, 0x00, '\x80'};
    const std::string data(rows, sizeof rows);
    std::string pat;
    Le32(pat, 9);
    pat += '\0';
    Le16(pat, 4);
    Le16(pat, (unsigned)data.size());
    x += pat + data;
    std::string ins;
    Le32(ins, 243);
    ins += Padded("square", 22);
    ins += '\0';
    Le16(ins, 1);
    Le32(ins, 40);
    ins.resize(243, '\0');
    std::string smp;
    Le32(smp, 32);
    Le32(smp, 0);
    Le32(smp, 32);
    smp += (char)64;
    smp += '\0';
    smp += (char)1;  // forward loop, 8-bit
    smp += '\x80';
    smp += '\0';
    smp += '\0';
    smp += Padded("square", 22);
    std::string delta;
    int last = 0;
    for (char c : SquareSigned8()) {
        delta += (char)(((int)(signed char)c - last) & 0xFF);
        last = (signed char)c;
    }
    return x + ins + smp + delta;
}

std::string It() {
    std::string t = "IMPM" + Padded("it test", 26);
    Le16(t, 0);
    Le16(t, 2);       // orders
    Le16(t, 0);       // instruments
    Le16(t, 1);       // samples
    Le16(t, 1);       // patterns
    Le16(t, 0x0214);
    Le16(t, 0x0214);
    Le16(t, 1 | 8);   // stereo, linear slides
    Le16(t, 0);
    t += '\x80';
    t += (char)48;
    t += (char)6;
    t += (char)125;
    t += '\x80';
    t += '\0';
    Le16(t, 0);
    Le32(t, 0);
    Le32(t, 0);
    t += std::string(64, (char)32);
    t += std::string(64, (char)64);
    t += '\0';
    t += '\xFF';
    Le32(t, 0xD0);    // the sample
    Le32(t, 0x140);   // the pattern
    t.resize(0xD0, '\0');
    std::string h = "IMPS" + std::string(12, '\0');
    h += '\0';
    h += (char)64;
    h += (char)(1 | 16);  // a sample, looped
    h += (char)64;
    h += Padded("square", 26);
    h += (char)1;  // signed
    h += '\xA0';
    Le32(h, 32);
    Le32(h, 0);
    Le32(h, 32);
    Le32(h, 8363);
    Le32(h, 0);
    Le32(h, 0);
    Le32(h, 0x120);
    Le32(h, 0);
    t += h;
    t.resize(0x120, '\0');
    t += SquareSigned8();
    t.resize(0x140, '\0');
    std::string data = "\x81\x07\x3C\x01\x40";  // C-5, sample 1, volume 64
    data += '\0';
    data += '\0';
    data += '\0';
    data += "\x81\x08\x02";
    data += '\0';  // B00
    data += '\0';
    std::string pat;
    Le16(pat, (unsigned)data.size());
    Le16(pat, 4);
    Le32(pat, 0);
    return t + pat + data;
}

// --- MIDI ---

// A SoundFont with one preset: a 441 Hz sine (100 samples a period) looped, rooted on A4.
std::string SoundFont() {
    auto chunk = [](const char* id, const std::string& body) {
        std::string c = id;
        Le32(c, (uint32_t)body.size());
        return c + body + (body.size() & 1 ? std::string(1, '\0') : std::string());
    };
    auto list = [&](const char* type, const std::string& body) { return chunk("LIST", type + body); };
    std::string pcm;
    for (int i = 0; i < 2000; ++i) Le16(pcm, (unsigned)(int16_t)std::lround(16000 * std::sin(2 * 3.14159265358979 * i / 100)) & 0xFFFF);
    pcm += std::string(92, '\0');
    std::string ifil;
    Le16(ifil, 2);
    Le16(ifil, 1);
    std::string phdr = Padded("sine", 20);
    Le16(phdr, 0);
    Le16(phdr, 0);
    Le16(phdr, 0);
    phdr += std::string(12, '\0');
    phdr += Padded("EOP", 20);
    Le16(phdr, 0);
    Le16(phdr, 0);
    Le16(phdr, 1);
    phdr += std::string(12, '\0');
    std::string pbag, pgen, ibag, igen, inst, shdr;
    Le16(pbag, 0); Le16(pbag, 0); Le16(pbag, 1); Le16(pbag, 0);
    Le16(pgen, 41); Le16(pgen, 0); Le32(pgen, 0);
    inst = Padded("sine", 20);
    Le16(inst, 0);
    inst += Padded("EOI", 20);
    Le16(inst, 1);
    Le16(ibag, 0); Le16(ibag, 0); Le16(ibag, 2); Le16(ibag, 0);
    Le16(igen, 54); Le16(igen, 1); Le16(igen, 53); Le16(igen, 0); Le32(igen, 0);
    shdr = Padded("sine", 20);
    Le32(shdr, 0);
    Le32(shdr, 2000);
    Le32(shdr, 1000);
    Le32(shdr, 2000);
    Le32(shdr, 44100);
    shdr += (char)69;
    shdr += '\0';
    Le16(shdr, 0);
    Le16(shdr, 1);
    shdr += std::string(46, '\0');
    const std::string mod(10, '\0');
    std::string body = "sfbk" + list("INFO", chunk("ifil", ifil)) + list("sdta", chunk("smpl", pcm)) +
                       list("pdta", chunk("phdr", phdr) + chunk("pbag", pbag) + chunk("pmod", mod) + chunk("pgen", pgen) +
                                        chunk("inst", inst) + chunk("ibag", ibag) + chunk("imod", mod) + chunk("igen", igen) +
                                        chunk("shdr", shdr));
    return chunk("RIFF", body).substr(0, 8 + body.size());
}

// A one-track MIDI file: A4 at full velocity for one second (at 120 BPM, 480 ticks a beat).
std::string Midi() {
    std::string track = std::string("\x00\xFF\x03\x09", 4) + "Test Song";
    track += std::string("\x00\xFF\x51\x03\x07\xA1\x20", 7);
    track += std::string("\x00\xC0\x00", 3);
    track += std::string("\x00\x90\x45\x7F", 4);
    track += std::string("\x87\x40\x80\x45\x00", 5);  // 960 ticks later
    track += std::string("\x00\xFF\x2F\x00", 4);
    std::string f = "MThd";
    Be32(f, 6);
    Be16(f, 0);
    Be16(f, 1);
    Be16(f, 480);
    f += "MTrk";
    Be32(f, (uint32_t)track.size());
    return f + track;
}

void TestZipArchives(const std::wstring& dir) {
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::string wav = MakeSineWav(0.5, 1);
    const std::wstring plain = dir + L"\\tone.wav";
    Ready(WriteTestBytes(plain, wav), L"ZIP fixture WAV is written");
    const std::vector<std::pair<std::string, std::string>> files = {
        {"Music/tone.wav", wav}, {"chip.sid", PsidTone()}, {"notes.txt", "not music"}, {"../escape.wav", wav}};
    const std::wstring stored = dir + L"\\Pack.zip", deflated = dir + L"\\Deflated.zip";
    Ready(WriteTestBytes(stored, Zip(files, false)) && WriteTestBytes(deflated, Zip(files, true)), L"ZIP fixtures are written");

    std::vector<std::wstring> found;
    CollectAudioFiles(stored, found);
    const std::wstring tone = stored + L"\\Music\\tone.wav";
    const std::vector<std::wstring> want = {tone, stored + L"\\chip.sid|1", stored + L"\\chip.sid|2"};
    Check(found == want, L"a ZIP lists its music, each song of a multi-song rip apart, and nothing else (%zu found)",
          found.size());

    unsigned rate = 0, zipRate = 0, deflateRate = 0;
    const auto original = DecodeAll(plain, rate);
    const auto inside = DecodeAll(tone, zipRate);
    const auto unpacked = DecodeAll(deflated + L"\\Music\\tone.wav", deflateRate);
    Check(!original.empty() && inside == original && unpacked == original && zipRate == rate,
          L"a WAV inside a ZIP, stored or deflated, decodes to exactly the samples of the file itself");

    TagData tags;
    Check(ReadTags(stored + L"\\chip.sid|2", tags) && tags.title == "cordinal test #2" && tags.codec == Codec::SID,
          L"tags are read from a song inside a ZIP");
    Check(ContainerOf(tone) == stored && ContainerOf(plain) == plain && IsArchiveMember(tone) && !IsArchiveMember(plain),
          L"a member path names its archive as the file on disk");

    std::string damaged = Zip(files, false);
    const size_t at = damaged.find("data") + 100;  // inside the WAV's samples
    damaged[at] = (char)(damaged[at] ^ 0x55);
    const std::wstring bad = dir + L"\\Damaged.zip";
    Ready(WriteTestBytes(bad, damaged), L"damaged ZIP is written");
    std::wstring error;
    Check(!OpenDecoder(bad + L"\\Music\\tone.wav", 48000, error) && !ReadArchiveMember(bad + L"\\Music\\tone.wav"),
          L"a member whose CRC doesn't match is refused");

    std::vector<ScanFile> scanned;
    ListAudioFiles(dir, scanned);
    const bool listed = std::any_of(scanned.begin(), scanned.end(), [&](const ScanFile& f) { return f.path == tone; }) &&
                        std::any_of(scanned.begin(), scanned.end(), [&](const ScanFile& f) { return f.path == stored + L"\\chip.sid|2"; });
    Check(listed, L"the library scanner finds the songs inside a ZIP in a library folder");
}

void TestDsd(const std::wstring& dir) {
    CreateDirectoryW(dir.c_str(), nullptr);
    const auto bits = DsdBits(2822400);
    const std::wstring dsf = dir + L"\\tone.dsf", dff = dir + L"\\tone.dff";
    Ready(WriteTestBytes(dsf, Dsf(bits)) && WriteTestBytes(dff, Dff(bits)), L"DSD fixtures are written");
    unsigned rate = 0, dffRate = 0;
    const auto a = DecodeAll(dsf, rate);
    const auto b = DecodeAll(dff, dffRate);
    Check(rate == 88200 && a.size() == 88200 * 2, L"DSD64 decodes to 88.2 kHz for its length (%u Hz, %zu frames)", rate,
          a.size() / 2);
    const double level = Rms(a, rate, 0.1, 0.9), pitch = Pitch(a, rate, 0.1, 0.9);
    Check(Near(level, 0.5 / std::sqrt(2.0), 0.08) && Near(pitch, 1000, 0.02),
          L"DSD's 1 kHz half-scale sine comes out at its level and pitch (RMS %.3f, %.1f Hz)", level, pitch);
    double inverse = 0;
    for (size_t i = 0; i + 1 < a.size(); i += 2) inverse = std::max(inverse, (double)std::abs(a[i] + a[i + 1]));
    Check(inverse < 1e-3, L"DSD channels stay apart (right is the inverse of left; worst sum %.5f)", inverse);
    double same = a.size() == b.size() ? 0 : 1;
    for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) same = std::max(same, (double)std::abs(a[i] - b[i]));
    Check(dffRate == rate && same < 1e-5, L"DSDIFF and DSF of the same bits decode alike (largest difference %.6f)", same);
    std::wstring error;
    auto d = OpenDecoder(dsf, 48000, error);
    std::vector<float> part(4096 * 2);
    const bool sought = d && d->Seek(0.5) && d->Read(part.data(), 4096) == 4096 &&
                        std::equal(part.begin(), part.end(), a.begin() + 44100 * 2);
    Check(sought, L"seeking in DSD lands on the exact sample");
    TagData t;
    Check(ReadTags(dsf, t) && t.codec == Codec::DSD && t.sampleRate == 2822400 && t.channels == 2 && Near(t.duration, 1, 0.001),
          L"DSF tags give DSD, its rate, channels and length");
    TagData u;
    Check(ReadTags(dff, u) && u.codec == Codec::DSD && u.sampleRate == 2822400 && Near(u.duration, 1, 0.001),
          L"DSDIFF tags give DSD, its rate and length");
}

void TestModules(const std::wstring& dir) {
    CreateDirectoryW(dir.c_str(), nullptr);
    struct Case { const wchar_t* name; std::string bytes; double hz; const char* title; };
    const Case cases[] = {
        {L"tone.mod", Mod(), 14187580.0 / 1712 / 32, "mod test"},
        {L"tone.s3m", S3m(), 8363.0 / 32, "s3m test"},
        {L"tone.xm", Xm(), 8363.0 / 32, "xm test"},
        {L"tone.it", It(), 8363.0 / 32, "it test"},
    };
    testlog::Cases length, sound, tags;
    for (const Case& c : cases) {
        const std::wstring path = dir + L"\\" + c.name;
        Ready(WriteTestBytes(path, c.bytes), L"module fixture is written");
        unsigned rate = 0;
        const auto pcm = DecodeAll(path, rate);
        length.Add(rate == 44100 && pcm.size() / 2 == 21168, c.name);
        sound.Add(Near(Pitch(pcm, rate, 0.05, 0.45), c.hz, 0.05) && Rms(pcm, rate, 0.05, 0.45) > 0.05, c.name);
        TagData t;
        tags.Add(ReadTags(path, t) && t.title == c.title && t.codec == Codec::Module && Near(t.duration, 0.48, 0.001), c.name);
    }
    Check(length.Passed(), L"each module plays to where it would repeat: four rows at speed 6, 125 BPM%s", length.failed.c_str());
    Check(sound.Passed(), L"each module plays its sample at the note's pitch%s", sound.failed.c_str());
    Check(tags.Passed(), L"module tags give the song's name and length%s", tags.failed.c_str());
}

void TestSid(const std::wstring& dir) {
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring tone = dir + L"\\tone.sid", irq = dir + L"\\irq.sid";
    Ready(WriteTestBytes(tone, PsidTone()) && WriteTestBytes(irq, PsidInterrupt()), L"SID fixtures are written");
    Check(GameMusicTrackCount(tone) == 2, L"a two-song PSID has two songs");
    unsigned rate = 0;
    std::wstring error;
    auto first = OpenDecoder(tone + L"|1", 48000, error), second = OpenDecoder(tone + L"|2", 48000, error);
    std::vector<float> a(44100 * 2 * 2), b(44100 * 2 * 2);
    const bool read = first && second && first->Read(a.data(), 88200) == 88200 && second->Read(b.data(), 88200) == 88200;
    rate = 44100;
    const double pa = read ? Pitch(a, rate, 0.2, 1.8) : 0, pb = read ? Pitch(b, rate, 0.2, 1.8) : 0;
    const double want = 0x1CD6 * 985248.0 / 16777216;
    Check(Near(pa, want, 0.02) && Near(pb, want * 2, 0.02) && Rms(a, rate, 0.2, 1.8) > 0.05,
          L"a SID tune's init sets a pulse tone at the pitch its registers give (%.1f and %.1f Hz)", pa, pb);
    auto withIrq = OpenDecoder(irq, 48000, error);
    std::vector<float> c(44100 * 2);
    const bool irqRead = withIrq && withIrq->Read(c.data(), 44100) == 44100;
    Check(irqRead && Rms(c, rate, 0.2, 1.0) > 0.05, L"a tune that plays from its own timer interrupt is heard");
    TagData t;
    Check(ReadTags(tone + L"|2", t) && t.title == "cordinal test #2" && t.album == "cordinal test" && t.artist == "Tester" &&
              t.date == "2026" && t.codec == Codec::SID && t.trackTotal == 2,
          L"SID tags give the tune, author, year and song number");
    // The C64's 6510 adds in BCD when D is set; the NES's 2A03 has no decimal mode.
    static const uint8_t program[] = {0xF8, 0x18, 0xA9, 0x09, 0x69, 0x01, 0x85, 0x10, 0x38, 0xA9, 0x10, 0xE9, 0x01, 0x85, 0x11, 0xD8, 0x60};
    auto run = [&](bool decimal, uint8_t& sum, uint8_t& diff) {
        RamBus bus;
        std::memcpy(bus.ram + 0x200, program, sizeof program);
        Cpu6502 cpu(bus);
        cpu.decimal = decimal;
        cpu.Reset();
        cpu.Call(0x200, 0x300);
        cpu.Run(1000, 0x300);
        sum = bus.ram[0x10];
        diff = bus.ram[0x11];
    };
    uint8_t s1, d1, s2, d2;
    run(true, s1, d1);
    run(false, s2, d2);
    Check(s1 == 0x10 && d1 == 0x09 && s2 == 0x0A && d2 == 0x0F, L"decimal mode: 09+01 is 10 and 10-01 is 09 in BCD");
}

void TestMidi(const std::wstring& dir) {
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring song = dir + L"\\song.mid", bank = dir + L"\\song.sf2", rmi = dir + L"\\song.rmi";
    std::string riff = "RIFF";
    const std::string smf = Midi();
    Le32(riff, (uint32_t)(4 + 8 + smf.size()));
    riff += "RMIDdata";
    Le32(riff, (uint32_t)smf.size());
    riff += smf;
    Ready(WriteTestBytes(song, smf) && WriteTestBytes(bank, SoundFont()) && WriteTestBytes(rmi, riff),
          L"MIDI and SoundFont fixtures are written");
    unsigned rate = 0;
    const auto pcm = DecodeAll(song, rate);
    const double pitch = Pitch(pcm, rate, 0.1, 0.9), level = Rms(pcm, rate, 0.1, 0.9), after = Rms(pcm, rate, 1.1, 2.4);
    Check(rate == 44100 && pcm.size() / 2 == (size_t)(2.5 * 44100) && Near(pitch, 441, 0.01) && level > 0.03 && after < 1e-4,
          L"MIDI plays a SoundFont's sample at the note's pitch for the note's length (%.1f Hz, RMS %.3f, then %.5f)", pitch,
          level, after);
    double right = Rms(pcm, rate, 0.1, 0.9, 1);
    Check(Near(right, level, 0.01), L"a centred MIDI note is as loud on the left as on the right");
    unsigned rmiRate = 0;
    Check(DecodeAll(rmi, rmiRate) == pcm, L"an RMI file plays the MIDI file it wraps");
    TagData t;
    Check(ReadTags(song, t) && t.title == "Test Song" && t.codec == Codec::MIDI && Near(t.duration, 2.5, 0.001),
          L"MIDI tags give the sequence name and length");
    // With no SoundFont beside it, a song uses Windows' own General MIDI sounds.
    const std::wstring alone = dir + L"\\gm";
    CreateDirectoryW(alone.c_str(), nullptr);
    wchar_t windows[MAX_PATH] = {};
    GetWindowsDirectoryW(windows, MAX_PATH);
    if (GetFileAttributesW((std::wstring(windows) + L"\\System32\\drivers\\gm.dls").c_str()) == INVALID_FILE_ATTRIBUTES) {
        testlog::Skip(L"no gm.dls on this system: the DLS check needs Windows' General MIDI set");
        return;
    }
    Ready(WriteTestBytes(alone + L"\\song.mid", smf), L"MIDI fixture without a SoundFont is written");
    unsigned gmRate = 0;
    const auto gm = DecodeAll(alone + L"\\song.mid", gmRate);
    Check(Rms(gm, gmRate, 0.05, 0.9) > 0.005, L"MIDI falls back to the General MIDI set in gm.dls (DLS)");
}

}  // namespace

void TestFormats(const std::wstring& scratch) {
    CreateDirectoryW(scratch.c_str(), nullptr);
    TestZipArchives(scratch + L"\\zip");
    TestDsd(scratch + L"\\dsd");
    TestModules(scratch + L"\\modules");
    TestSid(scratch + L"\\sid");
    TestMidi(scratch + L"\\midi");
}
