// Checks of the Linux font index and matcher (linux/fontmatch.h), run by the Linux core test:
//   "fonts DIR"           synthetic fonts and a fontconfig configuration made in DIR: the sfnt reader's
//                         fields, aliases, rules, rejected files, collections, the cache, and malformed
//                         fonts, configurations and caches, which must be read without crashing
//   "fonts-match [FILE]"  the installed fonts: each pattern (FILE's lines, or a built-in list) against
//                         fc-match, which is used only as the reference here and is not linked
#pragma once
#include "linux/fontmatch.h"

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace fontmatch_checks {

using lx::fonts::Face;
using lx::fonts::Range;

// ---- A small sfnt writer for the fixtures ----

inline void Put16(std::string& s, uint32_t v) { s += (char)(v >> 8), s += (char)v; }
inline void Put32(std::string& s, uint32_t v) { Put16(s, v >> 16), Put16(s, v & 0xFFFF); }

struct Spec {
    std::string family, style = "Regular", typoFamily;
    int weight = 400, width = 5;
    bool italic = false, fixed = false, symbol = false;
    uint32_t codePages = 1;  // Latin 1
    uint16_t glyphs = 300;
    std::vector<Range> bmp;     // format 4 segments, glyphs counting up from 1
    std::vector<Range> full;    // format 12 groups (a (3,10) subtable), when any
    Range beyondGlyphs{0, 0};   // a segment whose glyph ids run past numGlyphs, when not {0, 0}
    bool fvar = false;          // a wght axis 100..900 with one named instance
    uint32_t revision = 0x00010000;
};

inline std::string NameTable(const Spec& s) {
    std::vector<std::pair<uint16_t, std::string>> names{{1, s.family}, {2, s.style}};
    if (!s.typoFamily.empty()) names.push_back({16, s.typoFamily});
    std::string records, strings;
    for (auto& [id, text] : names) {
        std::string utf16;
        for (unsigned char c : text) Put16(utf16, c);
        Put16(records, 3), Put16(records, 1), Put16(records, 0x409), Put16(records, id);
        Put16(records, (uint32_t)utf16.size()), Put16(records, (uint32_t)strings.size());
        strings += utf16;
    }
    std::string t;
    Put16(t, 0), Put16(t, (uint32_t)names.size()), Put16(t, 6 + 12 * (uint32_t)names.size());
    return t + records + strings;
}

inline std::string Format4(const std::vector<Range>& ranges, Range beyond, uint16_t glyphs) {
    struct Seg { uint32_t start, end; uint16_t delta; };
    std::vector<Seg> segs;
    uint32_t next = 1;
    for (const Range& r : ranges) {
        segs.push_back({r.first, r.last, (uint16_t)(next - r.first)});
        next += r.last - r.first + 1;
    }
    if (beyond.last) segs.push_back({beyond.first, beyond.last, (uint16_t)(glyphs + 5 - beyond.first)});
    std::sort(segs.begin(), segs.end(), [](const Seg& a, const Seg& b) { return a.start < b.start; });
    segs.push_back({0xFFFF, 0xFFFF, 1});
    std::string t;
    uint32_t n = (uint32_t)segs.size();
    Put16(t, 4), Put16(t, 16 + 8 * n), Put16(t, 0), Put16(t, 2 * n), Put16(t, 0), Put16(t, 0), Put16(t, 0);
    for (auto& s : segs) Put16(t, s.end);
    Put16(t, 0);
    for (auto& s : segs) Put16(t, s.start);
    for (auto& s : segs) Put16(t, s.delta);
    for (size_t i = 0; i < segs.size(); ++i) Put16(t, 0);
    return t;
}

inline std::string Format12(const std::vector<Range>& ranges) {
    std::string t;
    Put16(t, 12), Put16(t, 0), Put32(t, 16 + 12 * (uint32_t)ranges.size()), Put32(t, 0), Put32(t, (uint32_t)ranges.size());
    uint32_t next = 1;
    for (const Range& r : ranges) {
        Put32(t, r.first), Put32(t, r.last), Put32(t, next);
        next += r.last - r.first + 1;
    }
    return t;
}

inline std::map<std::string, std::string> Tables(const Spec& s) {
    std::map<std::string, std::string> t;
    std::string head;
    Put32(head, 0x00010000), Put32(head, s.revision), Put32(head, 0), Put32(head, 0x5F0F3CF5);
    Put16(head, 0), Put16(head, 1000);
    head.resize(44, 0);
    Put16(head, (s.weight >= 700 ? 1 : 0) | (s.italic ? 2 : 0));
    head.resize(54, 0);
    t["head"] = head;
    std::string maxp;
    Put32(maxp, 0x00005000), Put16(maxp, s.glyphs);
    t["maxp"] = maxp;
    std::string hhea(34, 0);
    Put16(hhea, 2);
    t["hhea"] = hhea;
    std::string hmtx;
    Put16(hmtx, 500), Put16(hmtx, 0), Put16(hmtx, s.fixed ? 500 : 600), Put16(hmtx, 0);
    t["hmtx"] = hmtx;
    std::string os2;
    Put16(os2, 4), Put16(os2, 500), Put16(os2, (uint32_t)s.weight), Put16(os2, (uint32_t)s.width);
    os2.resize(62, 0);
    Put16(os2, (s.italic ? 1 : 0) | (s.weight >= 700 ? 0x20 : 0) | (!s.italic && s.weight < 700 ? 0x40 : 0));
    os2.resize(78, 0);
    Put32(os2, s.codePages), Put32(os2, 0);
    os2.resize(96, 0);
    t["OS/2"] = os2;
    std::string post;
    Put32(post, 0x00030000), Put32(post, 0), Put32(post, 0), Put32(post, s.fixed ? 1 : 0);
    post.resize(32, 0);
    t["post"] = post;
    t["name"] = NameTable(s);
    std::string sub4 = Format4(s.bmp, s.beyondGlyphs, s.glyphs), sub12 = s.full.empty() ? "" : Format12(s.full);
    std::string cmap;
    uint32_t count = sub12.empty() ? 1 : 2;
    Put16(cmap, 0), Put16(cmap, count);
    Put16(cmap, 3), Put16(cmap, s.symbol ? 0 : 1), Put32(cmap, 4 + 8 * count);
    if (!sub12.empty()) Put16(cmap, 3), Put16(cmap, 10), Put32(cmap, 4 + 8 * count + (uint32_t)sub4.size());
    t["cmap"] = cmap + sub4 + sub12;
    if (s.fvar) {
        std::string f;
        Put16(f, 1), Put16(f, 0), Put16(f, 16), Put16(f, 2), Put16(f, 1), Put16(f, 20), Put16(f, 1), Put16(f, 8);
        f += "wght";
        Put32(f, 100u << 16), Put32(f, 400u << 16), Put32(f, 900u << 16), Put16(f, 0), Put16(f, 256);
        Put16(f, 257), Put16(f, 0), Put32(f, 400u << 16);
        t["fvar"] = f;
    }
    return t;
}

// One sfnt whose table offsets start at `base` within the file it will be part of.
inline std::string Sfnt(const std::map<std::string, std::string>& tables, size_t base = 0) {
    std::string out;
    uint32_t n = (uint32_t)tables.size();
    Put32(out, 0x00010000), Put16(out, n), Put16(out, 0), Put16(out, 0), Put16(out, 0);
    size_t at = 12 + 16 * n;
    std::string body;
    for (auto& [tag, data] : tables) {
        out += tag;
        Put32(out, 0), Put32(out, (uint32_t)(base + at + body.size())), Put32(out, (uint32_t)data.size());
        body += data;
        while (body.size() % 4) body += '\0';
    }
    return out + body;
}

inline std::string Font(const Spec& s) { return Sfnt(Tables(s)); }

inline std::string Collection(const std::vector<Spec>& faces) {
    size_t header = 12 + 4 * faces.size();
    std::string out = "ttcf";
    Put32(out, 0x00010000), Put32(out, (uint32_t)faces.size());
    std::vector<std::string> parts;
    size_t at = header;
    for (const Spec& s : faces) {
        Put32(out, (uint32_t)at);
        parts.push_back(Sfnt(Tables(s), at));
        at += parts.back().size();
    }
    for (auto& p : parts) out += p;
    return out;
}

inline bool WriteFile(const std::string& path, const std::string& bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), (std::streamsize)bytes.size());
    return f.good();
}

inline std::string ReadAll(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), {});
}

inline bool SameRanges(const std::vector<Range>& a, std::vector<Range> b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].first != b[i].first || a[i].last != b[i].last) return false;
    return true;
}

inline bool HasLang(const Face& f, const char* lang) {
    const auto& langs = lx::fonts::Languages();
    for (size_t i = 0; i < langs.size(); ++i)
        if (langs[i] == lang) return f.langs >> i & 1;
    return false;
}

// Every face's fields stay within what a font can say.
inline bool Sane(const std::vector<Face>& faces) {
    for (const Face& f : faces) {
        if (f.family.empty() || f.coverage.empty() || f.index < 0 || f.index >= 256) return false;
        for (size_t i = 0; i < f.coverage.size(); ++i) {
            if (f.coverage[i].first > f.coverage[i].last || f.coverage[i].last > 0x10FFFF) return false;
            if (i && f.coverage[i].first <= f.coverage[i - 1].last + 1) return false;
        }
        if (!(f.weightMin <= f.weightMax) || !(f.widthMin <= f.widthMax) || !(f.slantMin <= f.slantMax)) return false;
    }
    return true;
}

struct Rng {
    uint64_t s;
    uint32_t Next() {
        s ^= s << 13, s ^= s >> 7, s ^= s << 17;
        return (uint32_t)s;
    }
};

// Truncations, overwritten bytes and hostile header fields: each is read without a crash or an
// out-of-range field (the sanitizer build turns any stray read into a failure).
inline int Mutations(const std::string& name, const std::string& bytes, int rounds, int* parses) {
    int bad = 0;
    auto parse = [&](const std::string& b) {
        ++*parses;
        if (!Sane(lx::fonts::ParseFont((const unsigned char*)b.data(), b.size()))) ++bad;
    };
    size_t step = std::max<size_t>(1, bytes.size() / 512);
    for (size_t n = 0; n < bytes.size(); n += step) parse(bytes.substr(0, n));
    Rng rng{0x9E3779B97F4A7C15ull ^ bytes.size()};
    for (int r = 0; r < rounds; ++r) {
        std::string b = bytes;
        int edits = 1 + rng.Next() % 8;
        for (int e = 0; e < edits; ++e) {
            size_t at = rng.Next() % b.size();
            switch (rng.Next() % 4) {
            case 0: b[at] = (char)rng.Next(); break;
            case 1: b[at] = (char)0xFF; break;
            case 2: b[at] = 0; break;
            default:
                for (size_t k = at; k < std::min(b.size(), at + 4); ++k) b[k] = (char)0xFF;
            }
        }
        parse(b);
    }
    // Header fields at their extremes: table counts, offsets, lengths, a collection's face count.
    for (size_t field : {4u, 12u + 8, 12u + 12, 12u + 16 + 8, 12u + 16 + 12, 8u}) {
        for (uint32_t v : {0u, 1u, 0x7FFFFFFFu, 0xFFFFFFFFu}) {
            std::string b = bytes;
            if (field + 4 > b.size()) continue;
            for (int k = 0; k < 4; ++k) b[field + k] = (char)(v >> (24 - 8 * k));
            parse(b);
        }
    }
    if (bad) printf("FAIL %s: %d malformed copies gave out-of-range fields\n", name.c_str(), bad);
    return bad;
}

inline int Check(const char* dirArg) {
    int failures = 0;
    auto check = [&](bool ok, const std::string& what) {
        printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
        failures += !ok;
    };
    std::string dir = dirArg;
    std::string fonts = dir + "/data/fonts", conf = dir + "/conf", cache = dir + "/cache";
    for (const std::string& d : {dir + "/home", dir + "/data", fonts, fonts + "/reject", fonts + "/sub", conf, conf + "/conf.d", cache})
        mkdir(d.c_str(), 0700);
    setenv("HOME", (dir + "/home").c_str(), 1);
    setenv("XDG_DATA_HOME", (dir + "/data").c_str(), 1);
    setenv("XDG_CACHE_HOME", cache.c_str(), 1);
    setenv("XDG_CONFIG_HOME", (dir + "/home/.config").c_str(), 1);
    setenv("FONTCONFIG_FILE", (conf + "/fonts.conf").c_str(), 1);
    unsetenv("FC_LANG");
    setenv("LANG", "en_US.UTF-8", 1);
    unsetenv("LC_ALL");
    unsetenv("LC_CTYPE");

    const std::vector<Range> latin{{0x20, 0x7E}, {0xA0, 0xFF}};
    Spec sans{"Cordinal Test Sans"};
    sans.bmp = latin, sans.bmp.push_back({0xE000, 0xE00F});
    sans.beyondGlyphs = {0xE100, 0xE1FF};
    Spec bold = sans;
    bold.style = "Bold", bold.weight = 700;
    Spec italic = sans;
    italic.style = "Italic", italic.italic = true;
    Spec mono{"Cordinal Test Mono"};
    mono.bmp = latin, mono.fixed = true, mono.full = {{0x20, 0x7E}, {0xA0, 0xFF}, {0x10FFF0, 0x10FFF3}};
    mono.glyphs = 1000;
    Spec flex{"Cordinal Test Flex"};
    flex.bmp = latin, flex.fvar = true;
    Spec heavy{"Cordinal Test Flex Black", "Black", "Cordinal Test Flex"};
    heavy.bmp = latin, heavy.weight = 900;
    Spec kana{"Cordinal Test Kana"}, hangul{"Cordinal Test Hangul"};
    kana.glyphs = hangul.glyphs = 65535;
    kana.bmp = hangul.bmp = latin;
    kana.full = hangul.full = {{0x20, 0x7E}, {0xA0, 0xFF}, {0x1100, 0x11FF}, {0x3000, 0x9FFF}, {0xAC00, 0xD7A3}, {0xF900, 0xFAFF}};
    kana.codePages = 1u << 17;    // JIS only: the collection's first face is Japanese, not Chinese
    hangul.codePages = 1u << 19;  // Wansung only: Korean
    Spec rejected{"Cordinal Test Rejected"};
    rejected.bmp = latin, rejected.bmp.push_back({0x10FF, 0x10FF});
    Spec symbol{"Cordinal Test Symbol"};
    symbol.bmp = {{0xF020, 0xF0FF}}, symbol.symbol = true;

    // The reader's fields, from bytes whose answers are known.
    std::string sansBytes = Font(sans);
    std::vector<Face> f = lx::fonts::ParseFont((const unsigned char*)sansBytes.data(), sansBytes.size());
    check(f.size() == 1 && f[0].family == "Cordinal Test Sans" && f[0].style == "Regular" && f[0].weightMin == 80 &&
              f[0].weightMax == 80 && !f[0].Italic() && !f[0].Mono() && !f[0].symbol && !f[0].color &&
              SameRanges(f[0].coverage, {{0x20, 0x7E}, {0xA0, 0xFF}, {0xE000, 0xE00F}}) && HasLang(f[0], "en") &&
              HasLang(f[0], "de") && !HasLang(f[0], "ru") && f[0].version == 0x00010000,
          "a TrueType font's family, style, weight, coverage (none past numGlyphs) and languages are read");
    std::string boldBytes = Font(bold), italicBytes = Font(italic), monoBytes = Font(mono), flexBytes = Font(flex);
    std::vector<Face> b = lx::fonts::ParseFont((const unsigned char*)boldBytes.data(), boldBytes.size());
    std::vector<Face> it = lx::fonts::ParseFont((const unsigned char*)italicBytes.data(), italicBytes.size());
    std::vector<Face> m = lx::fonts::ParseFont((const unsigned char*)monoBytes.data(), monoBytes.size());
    std::vector<Face> v = lx::fonts::ParseFont((const unsigned char*)flexBytes.data(), flexBytes.size());
    check(b.size() == 1 && b[0].weightMin == 200 && it.size() == 1 && it[0].Italic() && it[0].slantMin == 100,
          "bold is 200 and italic 100 on fontconfig's scales");
    check(m.size() == 1 && m[0].Mono() && m[0].Covers(0x10FFF1) && !m[0].Covers(0x10FFF4) && v.size() == 1 &&
              v[0].weightMin == 0 && v[0].weightMax == 210,
          "a monospace font's flag and full-repertoire cmap, and a variable font's weight axis");
    std::string ttc = Collection({kana, hangul});
    std::vector<Face> c = lx::fonts::ParseFont((const unsigned char*)ttc.data(), ttc.size());
    check(c.size() == 2 && c[0].index == 0 && c[1].index == 1 && c[0].family == "Cordinal Test Kana" &&
              HasLang(c[0], "ja") && !HasLang(c[0], "zh-cn") && !HasLang(c[0], "ko") && HasLang(c[1], "ko") &&
              !HasLang(c[1], "ja") && HasLang(c[0], "en"),
          "a collection's faces, each claiming only the Han language of its one code page");
    std::string symbolBytes = Font(symbol);
    std::vector<Face> s = lx::fonts::ParseFont((const unsigned char*)symbolBytes.data(), symbolBytes.size());
    check(s.size() == 1 && s[0].symbol && s[0].Covers(0xF041), "a symbol-encoded cmap");
    check(lx::fonts::ParseFont((const unsigned char*)"OTTO", 4).empty() && lx::fonts::ParseFont(nullptr, 0).empty() &&
              lx::fonts::ParseFont((const unsigned char*)"ttcf\0\1\0\0\xff\xff\xff\xff", 12).empty(),
          "files too short to be fonts give no faces");

    // An installed set and a configuration in fontconfig's format.
    bool wrote = WriteFile(fonts + "/CordinalTestSans-Regular.ttf", sansBytes) &&
                 WriteFile(fonts + "/CordinalTestSans-Bold.ttf", boldBytes) &&
                 WriteFile(fonts + "/CordinalTestSans-Italic.ttf", italicBytes) &&
                 WriteFile(fonts + "/sub/CordinalTestMono.TTF", monoBytes) &&
                 WriteFile(fonts + "/sub/CordinalTestFlex[wght].ttf", flexBytes) &&
                 WriteFile(fonts + "/sub/CordinalTestFlex-Black.otf", Font(heavy)) &&
                 WriteFile(fonts + "/sub/CordinalTestCJK.ttc", ttc) &&
                 WriteFile(fonts + "/sub/CordinalTestSymbol.ttf", symbolBytes) &&
                 WriteFile(fonts + "/reject/CordinalTestRejected.ttf", Font(rejected)) &&
                 WriteFile(fonts + "/sub/not-a-font.ttf", "this is text") &&
                 WriteFile(fonts + "/sub/truncated.ttf", sansBytes.substr(0, sansBytes.size() / 3)) &&
                 WriteFile(conf + "/fonts.conf",
                           "<?xml version=\"1.0\"?>\n<!DOCTYPE fontconfig SYSTEM \"urn:fontconfig:fonts.dtd\">\n<fontconfig>\n"
                           "  <!-- the test's own directory, twice: once by prefix -->\n"
                           "  <dir prefix=\"xdg\">fonts</dir>\n  <dir>" + fonts + "</dir>\n"
                           "  <include ignore_missing=\"yes\">conf.d</include>\n"
                           "  <include ignore_missing=\"yes\">missing.conf</include>\n"
                           "  <selectfont><rejectfont><glob>*/reject/*</glob></rejectfont></selectfont>\n"
                           "</fontconfig>\n") &&
                 WriteFile(conf + "/conf.d/60-aliases.conf",
                           "<fontconfig>\n"
                           "  <alias><family>Test Generic</family><prefer><family>Cordinal Test Mono</family></prefer></alias>\n"
                           "  <alias binding=\"same\"><family>Windows Face</family><accept><family>Cordinal&#32;Test Flex</family></accept></alias>\n"
                           "  <alias><family>sans-serif</family><prefer><family>Cordinal Test Sans</family></prefer></alias>\n"
                           "</fontconfig>\n") &&
                 WriteFile(conf + "/conf.d/49-sansserif.conf",
                           "<fontconfig><match target=\"pattern\">"
                           "<test qual=\"all\" name=\"family\" compare=\"not_eq\"><string>sans-serif</string></test>"
                           "<test qual=\"all\" name=\"family\" compare=\"not_eq\"><string>serif</string></test>"
                           "<test qual=\"all\" name=\"family\" compare=\"not_eq\"><string>monospace</string></test>"
                           "<edit name=\"family\" mode=\"append_last\"><string>sans-serif</string></edit></match></fontconfig>") &&
                 WriteFile(conf + "/conf.d/70-cjk.conf",
                           "<fontconfig><match target=\"pattern\"><test name=\"lang\"><string>ja</string></test>"
                           "<test name=\"family\"><string>sans-serif</string></test>"
                           "<edit name=\"family\" mode=\"prepend\" binding=\"strong\"><string>Cordinal Test Kana</string></edit>"
                           "</match><match target=\"font\"><edit name=\"embolden\"><bool>true</bool></edit></match></fontconfig>") &&
                 WriteFile(conf + "/conf.d/80-broken.conf", "<fontconfig><alias><family>Broken") &&
                 WriteFile(conf + "/conf.d/README", "<fontconfig><dir>/nonexistent</dir></fontconfig>");
    check(wrote, "the fixtures are written");
    lx::fonts::Reload();
    lx::fonts::IndexInfo info = lx::fonts::Info();
    check(info.config && info.parsed > 0 && std::count(info.dirs.begin(), info.dirs.end(), fonts) == 1,
          "the configuration is read and its directory scanned once (" + std::to_string(info.faces) + " faces)");
    auto file = [](const char* pattern) {
        lx::fonts::Match m;
        return lx::fonts::FindFont(pattern, &m) ? m.file.substr(m.file.rfind('/') + 1) + "#" + std::to_string(m.index) : "";
    };
    auto expect = [&](const char* pattern, const std::string& want) {
        std::string got = file(pattern);
        check(got == want, std::string("\"") + pattern + "\" is " + want + (got == want ? "" : " (got " + got + ")"));
    };
    expect("Cordinal Test Sans", "CordinalTestSans-Regular.ttf#0");
    expect("cordinaltestsans:weight=bold", "CordinalTestSans-Bold.ttf#0");
    expect("Cordinal Test Sans:slant=italic", "CordinalTestSans-Italic.ttf#0");
    expect("Cordinal Test Sans:italic", "CordinalTestSans-Italic.ttf#0");
    expect("Missing Face,Cordinal Test Sans:weight=bold", "CordinalTestSans-Bold.ttf#0");
    expect("Test Generic", "CordinalTestMono.TTF#0");
    expect("Windows Face:weight=bold", "CordinalTestFlex[wght].ttf#0");
    expect("Cordinal Test Flex:weight=bold", "CordinalTestFlex[wght].ttf#0");  // inside the axis, not Black's 210
    expect("Cordinal Test Flex Black", "CordinalTestFlex-Black.otf#0");
    expect("sans-serif:lang=ja", "CordinalTestCJK.ttc#0");
    expect("Cordinal Test Hangul", "CordinalTestCJK.ttc#1");
    expect("sans-serif:charset=10fff0 10fff3", "CordinalTestMono.TTF#0");
    expect("No Such Family Anywhere", "CordinalTestSans-Regular.ttf#0");
    check(file("Cordinal Test Rejected") != "CordinalTestRejected.ttf#0", "a rejected file is not offered");

    // The cache: reused whole, a changed file read again, a damaged cache rebuilt.
    lx::fonts::Reload();
    info = lx::fonts::Info();
    std::string cacheFile = info.cache;
    check(info.parsed == 0 && cacheFile == cache + "/cordinal/fonts.idx", "a second start reads every font from the cache");
    WriteFile(fonts + "/CordinalTestSans-Bold.ttf", Font(heavy));
    lx::fonts::Reload();
    info = lx::fonts::Info();
    check(info.parsed == 1 && file("Cordinal Test Sans:weight=bold") == "CordinalTestSans-Regular.ttf#0",
          "a font replaced in place is read again");
    WriteFile(fonts + "/CordinalTestSans-Bold.ttf", boldBytes);
    std::string good = ReadAll(cacheFile);
    int cacheBad = 0;
    Rng rng{42};
    for (int round = 0; round < 40; ++round) {
        std::string damaged = good;
        if (round < 20) damaged.resize(good.size() * round / 20);
        else
            for (int k = 0; k < 16; ++k) damaged[rng.Next() % damaged.size()] = (char)rng.Next();
        WriteFile(cacheFile, damaged);
        lx::fonts::Reload();
        cacheBad += file("Cordinal Test Sans:weight=bold") != "CordinalTestSans-Bold.ttf#0" || file("Test Generic") != "CordinalTestMono.TTF#0";
    }
    check(cacheBad == 0, "truncated and scrambled caches are dropped or read safely, and matching is unchanged");

    // Without a configuration: the built-in aliases and the usual directories.
    setenv("FONTCONFIG_FILE", (conf + "/missing.conf").c_str(), 1);
    lx::fonts::Reload();
    info = lx::fonts::Info();
    check(!info.config && file("Cordinal Test Sans:weight=bold") == "CordinalTestSans-Bold.ttf#0" &&
              file("Cordinal Test Rejected") == "CordinalTestRejected.ttf#0",
          "without fontconfig's files, the XDG font directory is still scanned and nothing is rejected");
    lx::fonts::Match any;
    check(lx::fonts::FindFont("emoji", &any) && lx::fonts::FindFont("monospace:weight=bold", &any) &&
              lx::fonts::FindFont("sans-serif:lang=ar", &any) && lx::fonts::FindFont("", &any) &&
              lx::fonts::FindFont(":::,,\\", &any) && lx::fonts::FindFont("x:weight=:charset=zz:lang=", &any),
          "generic and malformed patterns still give a face");
    setenv("FONTCONFIG_FILE", (conf + "/fonts.conf").c_str(), 1);

    // Malformed fonts: the fixtures, and installed fonts when the caller names some.
    int parses = 0, bad = 0;
    bad += Mutations("Cordinal Test Sans", sansBytes, 4000, &parses);
    bad += Mutations("Cordinal Test Mono", monoBytes, 2000, &parses);
    bad += Mutations("Cordinal Test Flex", flexBytes, 2000, &parses);
    bad += Mutations("the CJK collection", ttc, 2000, &parses);
    for (const char* pattern : {"DejaVu Sans", "emoji", "sans-serif:lang=ja", "monospace"}) {
        lx::fonts::Match real;
        if (!lx::fonts::FindFont(pattern, &real) || real.file.find(dir) == 0) continue;
        std::string bytes = ReadAll(real.file);
        if (bytes.size() > (64u << 20)) continue;
        bad += Mutations(real.file, bytes, 60, &parses);
    }
    check(bad == 0, "malformed fonts are read without a crash or an out-of-range field (" + std::to_string(parses) + " copies)");
    return failures ? 1 : 0;
}

// ---- Against fc-match ----

inline bool RunFcMatch(const std::string& pattern, std::string* out) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return false;
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        dup2(pipefd[1], 1);
        close(pipefd[0]);
        close(pipefd[1]);
        execlp("fc-match", "fc-match", "-f", "%{file}\\t%{index}\\t%{family[0]}", pattern.c_str(), (char*)nullptr);
        _exit(127);
    }
    close(pipefd[1]);
    out->clear();
    char buf[4096];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof buf)) > 0) out->append(buf, (size_t)n);
    close(pipefd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 && !out->empty();
}

inline bool SfntPath(const std::string& path) {
    std::string ext = path.substr(path.rfind('.') + 1);
    for (char& ch : ext) ch = (char)tolower((unsigned char)ch);
    return ext == "ttf" || ext == "otf" || ext == "ttc" || ext == "otc";
}

inline int CheckInstalled(const char* queries) {
    std::string probe;
    if (!RunFcMatch("sans-serif", &probe)) {
        printf("skip fc-match is not installed, so there is nothing to compare with\n");
        return 0;
    }
    std::vector<std::string> patterns;
    if (queries) {
        std::ifstream f(queries);
        for (std::string line; std::getline(f, line);) patterns.push_back(line);
    } else {
        // text.cpp's patterns, the generic names, each fallback language and symbol, common Windows faces.
        patterns = {"Segoe UI,Selawik,Noto Sans,sans-serif:weight=regular", "Segoe UI,Selawik,Noto Sans,sans-serif:weight=bold",
                    "Georgia,Gelasio,DejaVu Serif,serif:weight=regular", "Georgia,Gelasio,DejaVu Serif,serif:weight=bold",
                    "Bahnschrift,DIN Alternate,Barlow Semi Condensed,DejaVu Sans Condensed,sans-serif:weight=regular",
                    "Bahnschrift,DIN Alternate,Barlow Semi Condensed,DejaVu Sans Condensed,sans-serif:weight=bold",
                    "Consolas,Inconsolata,DejaVu Sans Mono,monospace:weight=regular",
                    "Consolas,Inconsolata,DejaVu Sans Mono,monospace:weight=bold", "Tahoma,sans-serif:weight=regular",
                    "Comic Sans MS,sans-serif:weight=bold", "Times New Roman,sans-serif:weight=regular",
                    "Courier New,sans-serif:weight=regular", "sans-serif:lang=ja", "sans-serif:lang=ko", "sans-serif:lang=zh-cn",
                    "sans-serif:lang=ar", "sans-serif:lang=he", "sans-serif:lang=th", "sans-serif:lang=hi",
                    "sans-serif:charset=2605", "sans-serif:charset=2606", "sans-serif:charset=26a1", "DejaVu Sans", "emoji",
                    "monospace:weight=regular", "monospace:weight=bold", "sans-serif", "serif", "monospace",
                    "serif:weight=bold:slant=italic", "sans-serif:lang=zh-tw", "sans-serif:lang=ru", "serif:lang=ja",
                    "monospace:lang=ko", "sans-serif:charset=1f600", "sans-serif:charset=20ac 2713",
                    "Helvetica:weight=bold", "Arial:slant=italic", "Verdana", "Calibri", "Cambria", "Lucida Console",
                    "Nonexistent Family", "sans-serif:weight=light", "sans-serif:weight=black", "sans-serif:width=75"};
    }
    int same = 0, family = 0, differ = 0;
    for (const std::string& p : patterns) {
        std::string fc;
        lx::fonts::Match ours;
        bool found = lx::fonts::FindFont(p, &ours);
        if (!RunFcMatch(p, &fc)) {
            printf("FAIL fc-match could not answer \"%s\"\n", p.c_str());
            ++differ;
            continue;
        }
        size_t t1 = fc.find('\t'), t2 = fc.find('\t', t1 + 1);
        std::string fcFile = fc.substr(0, t1), fcFamily = t2 == std::string::npos ? "" : fc.substr(t2 + 1);
        int fcIndex = atoi(fc.substr(t1 + 1).c_str()) & 0xFFFF;  // the upper bits name a variable font's instance
        if (found && ours.file == fcFile && ours.index == fcIndex) {
            ++same;
        } else if (found && !SfntPath(fcFile) && strcasecmp(ours.family.c_str(), fcFamily.c_str()) == 0) {
            // fontconfig also offers Type 1 and bitmap files, which this index leaves out; the same family
            // from an OpenType file is what the player would draw either way.
            ++family;
            printf("ok   \"%s\": %s, where fc-match has the same family's %s\n", p.c_str(), ours.file.c_str(), fcFile.c_str());
        } else {
            ++differ;
            printf("FAIL \"%s\": %s#%d (%s), fc-match %s#%d (%s)\n", p.c_str(), ours.file.c_str(), ours.index,
                   ours.family.c_str(), fcFile.c_str(), fcIndex, fcFamily.c_str());
        }
    }
    printf("%s %d of %zu patterns pick fc-match's file and face, %d its family from an OpenType file, %d differ\n",
           differ ? "FAIL" : "ok  ", same, patterns.size(), family, differ);
    return differ ? 1 : 0;
}

}  // namespace fontmatch_checks
