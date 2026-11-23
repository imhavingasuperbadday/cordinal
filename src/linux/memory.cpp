// Giving memory back to Linux, as app_memory.cpp does for Windows and on the same schedule
// (TrimWatch, trimwatch.h): once startup's work is done, two seconds after the window is put away,
// and whenever the watch sees memory stay above where the player settled. Never while the window has
// the keyboard and was used in the last ten seconds, so a scroll doesn't fault its pages back in
// mid-gesture.
//
// What goes back: the allocator's freed pages (malloc_trim), and the pages of read-only file
// mappings no one wrote to, which is most of a fresh start's figure: the program's code (the
// kernel maps 64 KiB around every page startup touched, so about 10 MB of a 21 MB program was
// resident) and the parts of font files the first frames read. Those pages are clean copies of
// files still in the page cache, so dropping them from the process loses nothing; the ones still
// in use fault straight back in. Writable and anonymous memory is never touched.
// The window's own pixels go back separately, inside the platform layer (wayland.cpp, x11.cpp).
#include "app.h"
#include "trimwatch.h"

#include <fcntl.h>
#include <malloc.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace lx {

namespace {

struct GivingBack {
    bool first = true;  // startup's trim hasn't happened yet
    uint64_t started = 0, sampled = 0, due = 0;
    TrimWatch watch;
} gb;

constexpr uint64_t kStartupSettle = 20000;  // the window, library load and first save are done by then
constexpr uint64_t kSoon = 2000, kWatchTick = 5000;
constexpr uint64_t kQuiet = 10000;          // no input this long: nobody is using the window

std::string ReadProcFile(const char* path) {
    std::string out;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return out;
    char buf[16384];
    for (ssize_t n; (n = read(fd, buf, sizeof buf)) > 0;) out.append(buf, (size_t)n);
    close(fd);
    return out;
}

// The kB figure on `key`'s line ("Rss:") in `text`; 0 when absent.
size_t Field(std::string_view text, std::string_view key) {
    for (size_t at = 0; (at = text.find(key, at)) != std::string_view::npos; at += key.size())
        if (at == 0 || text[at - 1] == '\n') return (size_t)strtoull(text.data() + at + key.size(), nullptr, 10);
    return 0;
}

// A mapping's first line in /proc/self/smaps: "start-end perms offset dev inode path". The lines
// about it after that are "Key: value", whose key never has a '-'.
bool MappingLine(std::string_view line, uintptr_t& start, uintptr_t& stop, bool& candidate) {
    const size_t dash = line.find('-'), space = line.find(' ');
    if (dash == std::string_view::npos || space == std::string_view::npos || dash > space) return false;
    const std::string text(line);
    char perms[5] = {};
    unsigned long long a = 0, b = 0, inode = 0;
    if (sscanf(text.c_str(), "%llx-%llx %4s %*s %*s %llu", &a, &b, perms, &inode) != 4) return false;
    start = (uintptr_t)a, stop = (uintptr_t)b;
    candidate = perms[1] != 'w' && inode != 0 && stop > start;  // a file no one maps writable here
    return true;
}

bool InUse() {
    return g.window && (SDL_GetWindowFlags(g.window) & SDL_WINDOW_INPUT_FOCUS) && SDL_GetTicks() - LastInputTicks() < kQuiet;
}

void GiveBack() {
    GiveMemoryBack();
    gb.watch.Trimmed(SDL_GetTicks());
    gb.first = false;
    gb.due = 0;
}

}  // namespace

size_t DropCleanFilePages() {
    const std::string smaps = ReadProcFile("/proc/self/smaps");
    size_t dropped = 0;
    uintptr_t start = 0, stop = 0;
    bool candidate = false;
    size_t from = 0;  // where the current mapping's "Key: value" lines start
    auto finish = [&](size_t to) {
        if (!candidate) return;
        const std::string_view fields = std::string_view(smaps).substr(from, to - from);
        const size_t rss = Field(fields, "Rss:");
        // Only the file's own pages: a page written through a once-writable mapping (the relocated
        // data the loader made read-only) is anonymous, and dropping it would lose the write. Dirty
        // file pages (tmpfs, a file still being written) are fine: the page cache keeps them.
        if (!rss || Field(fields, "Anonymous:") || Field(fields, "Locked:")) return;
        if (madvise((void*)start, stop - start, MADV_DONTNEED) == 0) dropped += rss << 10;
    };
    for (size_t at = 0; at < smaps.size();) {
        size_t end = smaps.find('\n', at);
        if (end == std::string::npos) end = smaps.size();
        uintptr_t a = 0, b = 0;
        bool c = false;
        if (MappingLine(std::string_view(smaps).substr(at, end - at), a, b, c)) {
            finish(at);
            start = a, stop = b, candidate = c, from = end;
        }
        at = end + 1;
    }
    finish(smaps.size());
    return dropped;
}

// linux/code-profile.sh asks for this (CORDINAL_CODE_PROFILE): every page of the program's code
// becomes a mapping of its own, by alternating read-ahead advice, and the kernel never maps a page
// around a fault beyond its mapping. After the first trim, then, the code pages resident are exactly
// the ones the player ran since, which is what linux/hot-code.ld links together.
void PrepareCodeProfile() {
    if (!getenv("CORDINAL_CODE_PROFILE")) return;
    char exe[4096];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return;
    exe[n] = 0;
    const long page = sysconf(_SC_PAGESIZE);
    const std::string maps = ReadProcFile("/proc/self/maps");
    for (size_t at = 0; at < maps.size();) {
        size_t end = maps.find('\n', at);
        if (end == std::string::npos) end = maps.size();
        const std::string line = maps.substr(at, end - at);
        at = end + 1;
        unsigned long long a = 0, b = 0;
        char perms[5] = {};
        if (line.size() <= (size_t)n || line.compare(line.size() - (size_t)n, (size_t)n, exe) != 0 ||
            sscanf(line.c_str(), "%llx-%llx %4s", &a, &b, perms) != 3 || perms[2] != 'x')
            continue;
        for (unsigned long long p = a + page; p < b; p += 2 * page) madvise((void*)(uintptr_t)p, page, MADV_RANDOM);
    }
}

void GiveMemoryBack() {
    malloc_trim(0);
    DropCleanFilePages();
}

size_t ResidentBytes() {
    // The process's own pages: what the allocator holds and the files it reads. Shared memory (the
    // window's pixels, which come and go with drawing) would only make the watch chase frames.
    const std::string status = ReadProcFile("/proc/self/status");
    return (Field(status, "RssAnon:") + Field(status, "RssFile:")) << 10;
}

void GiveMemoryBackSoon() {
    if (g.settings.lightweightTrim) gb.due = SDL_GetTicks() + kSoon;
}

// Starting a song runs code and fills buffers it never touches again: the file opened, its decoder
// set up, the cover decoded and the library saved. They go once that is over, as the watch alone
// would leave them when the song started right as it measured where the player settles.
void MemoryTrackStarted() {
    constexpr uint64_t kAfterStart = 8000;
    if (g.settings.lightweightTrim && !gb.first && !gb.due) gb.due = SDL_GetTicks() + kAfterStart;
}

void MemoryTick() {
    if (!g.settings.lightweightTrim || g.quit) return;
    const uint64_t now = SDL_GetTicks();
    if (!gb.started) gb.started = now;
    if (gb.due) {
        if (now >= gb.due && !InUse()) GiveBack();
        return;
    }
    if (now - gb.sampled < kWatchTick) return;
    gb.sampled = now;
    if (gb.first) {
        if (now - gb.started >= kStartupSettle && !g.scanner.Busy() && !InUse()) GiveBack();
        return;
    }
    if (gb.watch.Sample(ResidentBytes(), now) && !InUse()) GiveBack();
}

}  // namespace lx
