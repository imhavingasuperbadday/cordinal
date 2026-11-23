// Linux memory checks (memory.cpp, platform/render.cpp), as on Windows (selftest_ui.cpp's TrimWatch
// cases): giving memory back drops a read-only file mapping's pages and keeps them readable, leaves a
// written copy of a file exactly as it was, and a large texture costs nothing until it is drawn into.
// Runs with --check-layout on the offscreen window; nothing is saved.
#ifndef _WIN32
#include "../linux/ui_internal.h"

#include <fcntl.h>
#include <malloc.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace lx {

namespace {

// The resident kB of the mapping that starts at `p`, from /proc/self/smaps; -1 when not found.
long MappingRss(const void* p) {
    FILE* f = fopen("/proc/self/smaps", "r");
    if (!f) return -1;
    char line[512];
    bool mine = false;
    long rss = -1;
    const unsigned long long want = (unsigned long long)(uintptr_t)p;
    while (fgets(line, sizeof line, f)) {
        unsigned long long start = 0, end = 0;
        if (sscanf(line, "%llx-%llx ", &start, &end) == 2 && strchr(line, '-') < strchr(line, ' ')) {
            mine = start == want;
            continue;
        }
        long kb = 0;
        if (mine && sscanf(line, "Rss: %ld kB", &kb) == 1) {
            rss = kb;
            break;
        }
    }
    fclose(f);
    return rss;
}

// The process's anonymous memory in bytes: what a texture's pixels take, without code paging in.
size_t AnonBytes() {
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return 0;
    char line[256];
    long kb = 0;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "RssAnon: %ld kB", &kb) == 1) break;
    fclose(f);
    return (size_t)kb << 10;
}

}  // namespace

int CheckLinuxMemory() {
    int failed = 0;
    auto check = [&](bool ok, const char* message) {
        printf("%s %s\n", ok ? "ok  " : "FAIL", message);
        fflush(stdout);
        failed += !ok;
    };

    // A file read through a read-only mapping and the same file written through a private one.
    char dir[] = "/tmp/cordinal-memory-XXXXXX";
    if (!mkdtemp(dir)) {
        check(false, "memory: a scratch folder");
        return failed;
    }
    const std::string path = std::string(dir) + "/pages.bin";
    constexpr size_t kBytes = 256 << 10;
    std::string bytes(kBytes, '\0');
    for (size_t i = 0; i < kBytes; ++i) bytes[i] = (char)(i * 7 + 3);
    int fd = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    const bool wrote = fd >= 0 && write(fd, bytes.data(), kBytes) == (ssize_t)kBytes;
    auto* ro = wrote ? (const char*)mmap(nullptr, kBytes, PROT_READ, MAP_PRIVATE, fd, 0) : (const char*)MAP_FAILED;
    auto* rw = wrote ? (char*)mmap(nullptr, kBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0) : (char*)MAP_FAILED;
    if (fd >= 0) close(fd);
    if (ro != MAP_FAILED && rw != MAP_FAILED) {
        unsigned sum = 0;
        for (size_t i = 0; i < kBytes; i += 4096) sum += (unsigned char)ro[i] + (unsigned char)rw[i];
        rw[4096] = 'X';  // a written copy of one page, then read-only, as the loader leaves relocated data
        mprotect(rw, kBytes, PROT_READ);
        const long before = MappingRss(ro);
        const size_t dropped = DropCleanFilePages();
        const long after = MappingRss(ro);
        if (!(before >= 200 && after == 0 && dropped >= (size_t)before << 10))
            printf("     read-only mapping: %ld kB before, %ld kB after, %zu bytes dropped\n", before, after, dropped);
        check(before >= 200 && after == 0 && dropped >= (size_t)before << 10,
              "memory: giving memory back drops a read-only file mapping's pages");
        check(memcmp(ro, bytes.data(), kBytes) == 0, "memory: and its pages read the file again afterwards");
        check(rw[4096] == 'X' && rw[0] == bytes[0] && MappingRss(rw) > 0,
              "memory: a written copy of a file, made read-only since, keeps what was written");
        check(sum != 0, "memory: the mappings were read");
    } else {
        check(false, "memory: mapping the scratch file");
    }
    if (ro != MAP_FAILED) munmap((void*)ro, kBytes);
    if (rw != MAP_FAILED) munmap(rw, kBytes);
    unlink(path.c_str());
    rmdir(dir);

    // A glyph atlas page is 1024 by 1024 (4 MiB): it may cost only the rows a glyph lands in.
    malloc_trim(0);
    const size_t start = AnonBytes();
    SDL_Texture* page = SDL_CreateTexture(g.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, 1024, 1024);
    const size_t created = AnonBytes();
    uint32_t glyph[16 * 16];
    for (uint32_t& p : glyph) p = 0x80FFFFFFu;
    SDL_Rect at{8, 8, 16, 16};
    const bool updated = page && SDL_UpdateTexture(page, &at, glyph, 16 * 4);
    const size_t drawn = AnonBytes();
    check(page && created < start + (512 << 10), "memory: a new 4 MiB texture costs nothing until drawn into");
    check(updated && drawn < start + (512 << 10), "memory: one glyph in it costs only the rows it touches");
    SDL_DestroyTexture(page);
    return failed;
}

}  // namespace lx
#endif
