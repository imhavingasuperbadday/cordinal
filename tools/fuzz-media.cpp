// libFuzzer targets for the file parsers: the native decoders, the tag reader and the cover-art
// path. Each case is written to a file in a private temporary folder (the parsers read files, as
// the scanner and the engine do) and run through tools/fuzz-exercise.h, the same checks as the
// Windows mutation fuzzer. Built on Linux by CMake with -DCORDINAL_FUZZ=ON (clang), one program per
// target, and run by tools/fuzz-media.sh with seeds made by FFmpeg; CI runs every target under
// AddressSanitizer and UndefinedBehaviorSanitizer (README.md, "Fuzzing and sanitizers").
//
//   fuzz-flac, fuzz-mp3, fuzz-opus, fuzz-vorbis   one format through the decoder and the tag reader
//   fuzz-aac                                      the first byte picks ADTS (.aac) or MP4 (.m4a)
//   fuzz-tags                                     the first byte picks a container; tags, art, lyrics,
//                                                 chapters and loop points only
//   fuzz-art                                      the first byte picks a container or a bare image;
//                                                 the embedded picture is decoded as cover.cpp does
//   fuzz-decoders                                 the first byte picks any other format the player
//                                                 decodes, chiptune, tracker modules and MIDI included
//
// A finding (a broken Decoder contract, a non-finite sample, an escaped exception, a file left open)
// is printed and aborts, so libFuzzer keeps the input as a crash like a sanitizer report.
#include "fuzz-exercise.h"

#include <dirent.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#ifdef CORDINAL_FUZZ_IMAGES
// The cover decoder exactly as src/linux/cover.cpp compiles it.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#include "third_party/stb_image.h"
#endif

#ifndef CORDINAL_FUZZ_TARGET
#error "define CORDINAL_FUZZ_TARGET as the target's name (CMakeLists.txt does)"
#endif

namespace {

struct Target {
    const char* name;
    std::vector<const char*> extensions;  // more than one: the first byte of the input picks
    int mode;                             // fuzzcase::kDecoder, kTags or both
    bool images = false;                  // decode the embedded picture; "img" is a bare image
};

const Target kTargets[] = {
    {"flac", {"flac"}, fuzzcase::kBoth},
    {"mp3", {"mp3"}, fuzzcase::kBoth},
    {"opus", {"opus"}, fuzzcase::kBoth},
    {"vorbis", {"ogg"}, fuzzcase::kBoth},
    {"aac", {"aac", "m4a"}, fuzzcase::kBoth},
    {"tags", {"mp3", "flac", "ogg", "opus", "m4a", "wav", "aiff", "wma", "mka", "wv", "ape", "mpc", "dsf", "cord"},
     fuzzcase::kTags},
    {"art", {"flac", "mp3", "m4a", "ogg", "opus", "wma", "mka", "img"}, fuzzcase::kTags, true},
    {"decoders", {"wav", "aiff", "au", "caf", "voc", "8svx", "adx", "g722", "gsm", "dfpwm", "mp2", "oga", "wv", "ape",
                  "mpc", "tta", "ac3", "eac3", "mka", "wma", "dsf", "dff", "cord", "mod", "s3m", "xm", "it", "mid",
                  "spc", "nsf", "gbs", "vgm"},
     fuzzcase::kBoth},
};

const Target& This() {
    for (const Target& t : kTargets)
        if (std::string(t.name) == CORDINAL_FUZZ_TARGET) return t;
    fprintf(stderr, "fuzz-media: no target named %s\n", CORDINAL_FUZZ_TARGET);
    abort();
}

[[noreturn]] void Report(const std::string& what) {
    fprintf(stderr, "==FINDING== %s\n", what.c_str());
    fflush(stderr);
    abort();
}

std::string g_dir;  // this process's own folder for the case file

int OpenFiles() {
    int count = 0;
    if (DIR* d = opendir("/proc/self/fd")) {
        while (readdir(d)) ++count;
        closedir(d);
    }
    return count;
}

#ifdef CORDINAL_FUZZ_IMAGES
// As cover.cpp's Decode: any image stb_image reads, as RGBA. Its size checks are DecodeCover's
// callers', so a bare image is checked the way a fetched cover is first.
void DecodeImage(const std::string& bytes) {
    if (bytes.empty() || bytes.size() > INT32_MAX) return;
    int w = 0, h = 0, n = 0;
    const auto* data = reinterpret_cast<const stbi_uc*>(bytes.data());
    if (!stbi_info_from_memory(data, (int)bytes.size(), &w, &h, &n) || w <= 0 || h <= 0 || (uint64_t)w * h > 100000000)
        return;
    if (stbi_uc* px = stbi_load_from_memory(data, (int)bytes.size(), &w, &h, &n, 4)) {
        volatile unsigned sum = 0;
        for (size_t i = 0; i < (size_t)w * h * 4; i += 4099) sum += px[i];
        stbi_image_free(px);
    }
}
#endif

}  // namespace

extern "C" int LLVMFuzzerInitialize(int*, char***) {
    const char* tmp = getenv("TMPDIR");
    std::string pattern = std::string(tmp && *tmp ? tmp : "/tmp") + "/cordinal-fuzz-XXXXXX";
    if (!mkdtemp(pattern.data())) Report("can't make a temporary folder");
    g_dir = pattern;
    atexit([] {
        for (const char* ext : This().extensions) unlink((g_dir + "/case." + ext).c_str());
        rmdir(g_dir.c_str());
    });
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const Target& t = This();
    const char* ext = t.extensions[0];
    if (t.extensions.size() > 1) {
        if (!size) return 0;
        ext = t.extensions[data[0] % t.extensions.size()];
        ++data, --size;
    }
#ifdef CORDINAL_FUZZ_IMAGES
    if (t.images && std::string(ext) == "img") {
        DecodeImage(std::string((const char*)data, size));
        return 0;
    }
#endif
    const std::string path = g_dir + "/case." + ext;
    FILE* f = fopen(path.c_str(), "wb");
    if (!f || fwrite(data, 1, size, f) != size || fclose(f)) Report("can't write the case file");
    const int before = OpenFiles();
    std::string problem;
    if (t.images) {
        fuzzcase::Problem().clear();
        try {
            std::string art = fuzzcase::ExerciseTags(FromUtf8(path));
#ifdef CORDINAL_FUZZ_IMAGES
            DecodeImage(art);
#endif
        } catch (const std::exception& e) {
            fuzzcase::Finding(std::string("uncaught exception: ") + e.what());
        } catch (...) {
            fuzzcase::Finding("uncaught exception of an unknown type");
        }
        problem = fuzzcase::Problem();
    } else {
        problem = fuzzcase::Run(FromUtf8(path), data, size, t.mode);
    }
    // Every reader has closed the file by now: one left open leaks a descriptor per scanned file.
    if (problem.empty() && OpenFiles() > before) problem = "a file descriptor is still open after every reader finished";
    if (!problem.empty()) Report(problem);
    unlink(path.c_str());
    return 0;
}
