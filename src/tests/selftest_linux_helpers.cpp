// Linux visual-feed checks: stable shared ABI, live read-only mappings and inherited descriptors.
// All child scripts and output live in a temporary directory; no installed helper is run.
#ifndef _WIN32
#include "helpers.h"
#include "library.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace lx {
int CheckHelperVisualizerLifecycle();
int CheckLinuxHelperFeed() {
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        printf("%s %s\n", ok ? "ok  " : "FAIL", label);
        failures += !ok;
    };
    HelperFeedWriter writer;
    HANDLE handle = writer.Open() ? writer.ReaderHandle() : nullptr;
    int reader = handle ? (int)(intptr_t)handle : -1;
    auto* shared = reader < 0 ? nullptr : static_cast<const HelperFeed*>(
        mmap(nullptr, sizeof(HelperFeed), PROT_READ, MAP_SHARED, reader, 0));
    if (shared == MAP_FAILED) shared = nullptr;
    check(shared && (fcntl(reader, F_GETFL) & O_ACCMODE) == O_RDONLY && (fcntl(reader, F_GETFD) & FD_CLOEXEC),
        "Linux visual feed exposes a read-only descriptor with close-on-exec by default");
    if (!shared) { if (reader >= 0) close(reader); return failures; }
    HelperFeed copy{};
    bool initial = ReadHelperFeed(shared, copy) && !memcmp(copy.magic, "CDVF", 4) && copy.version == 1 &&
        copy.size == sizeof(copy) && copy.frames == 2048 && copy.bands == 40 && copy.clock == -1 && copy.state == 0;
    writer.Publish([](HelperFeed& f) {
        f.state = 1; f.rate = 48000; f.clock = 2.5; f.position = 3; f.duration = 8;
        f.colors[0] = 0x102030; f.levels[2] = 0.75f; f.wave[123] = -0.5f;
    });
    bool updated = ReadHelperFeed(shared, copy) && copy.state == 1 && copy.rate == 48000 && copy.sequence == 2 &&
        copy.clock == 2.5 && copy.position == 3 && copy.duration == 8 && copy.colors[0] == 0x102030 &&
        copy.levels[2] == 0.75f && copy.wave[123] == -0.5f;
    writer.Publish([](HelperFeed& f) { f.state = 2; });
    bool paused = ReadHelperFeed(shared, copy) && copy.state == 2 && copy.wave[123] == -0.5f && copy.clock == 2.5;
    check(initial && updated && paused, "CDVF snapshots retain the shared ABI, advance sequence and preserve paused samples");
    HelperFeed odd{};
    odd.sequence = 1;
    check(!ReadHelperFeed(&odd, copy) && !ReadHelperFeed(nullptr, copy),
        "visual-feed readers reject in-progress or absent snapshots");
    void* writable = mmap(nullptr, sizeof(HelperFeed), PROT_READ | PROT_WRITE, MAP_SHARED, reader, 0);
    bool readonly = writable == MAP_FAILED;
    if (!readonly) munmap(writable, sizeof(HelperFeed));
    check(readonly && ftruncate(reader, 0) < 0, "visualizers cannot map the feed writable or truncate it");

    char temporary[] = "/tmp/cordinal-visual-feed-XXXXXX";
    if (mkdtemp(temporary) && access("/usr/bin/python3", X_OK) == 0) {
        namespace fs = std::filesystem;
        const fs::path root(temporary), program = root / "reader.py", result = root / "result.txt";
        const std::string script = R"PY(#!/usr/bin/python3
import fcntl, mmap, os, pathlib, struct, sys, time
fd = int(sys.argv[1])
block = mmap.mmap(fd, 16616, access=mmap.ACCESS_READ)
assert block[:4] == b'CDVF' and struct.unpack_from('<I', block, 4)[0] == 1
assert struct.unpack_from('<I', block, 24)[0] == 2
assert struct.unpack_from('<f', block, 232 + 123 * 4)[0] == -0.5
assert fcntl.fcntl(fd, fcntl.F_GETFL) & os.O_ACCMODE == os.O_RDONLY
before = block[:]
reopened = os.open(f'/proc/self/fd/{fd}', os.O_RDWR)
try:
    try:
        os.write(reopened, b'BAD!')
        raise AssertionError('reopened feed allowed writing')
    except OSError:
        pass
    try:
        mmap.mmap(reopened, 16616, access=mmap.ACCESS_WRITE)
        raise AssertionError('reopened feed allowed a writable shared mapping')
    except OSError:
        pass
finally:
    os.close(reopened)
assert block[:] == before
identity = os.fstat(fd)
for name in os.listdir('/proc/self/fd'):
    try:
        other = int(name)
        stat = os.fstat(other)
        if stat.st_ino == identity.st_ino and stat.st_dev == identity.st_dev:
            assert fcntl.fcntl(other, fcntl.F_GETFL) & os.O_ACCMODE == os.O_RDONLY
    except OSError:
        pass
pathlib.Path(sys.argv[2]).write_text('read-only inherited feed')
sys.stdout.write('x' * 100000)
if len(sys.argv) > 3:
    pathlib.Path(sys.argv[3]).write_text(str(os.getpid()))
    while True:
        time.sleep(1)
)PY";
        std::ofstream out(program);
        out << script;
        out.close();
        chmod(program.c_str(), 0700);
        HelperProcess child;
        std::wstring error;
        std::wstring arguments = std::to_wstring(reader) + L" \"" + FromUtf8(result.string()) + L"\"";
        bool started = child.Start(FromUtf8(program.string()), arguments, FromUtf8(root.string()), error, handle, true);
        bool exited = started && child.Finish(5000) == 0;
        std::ifstream in(result);
        std::string text((std::istreambuf_iterator<char>(in)), {});
        check(exited && text == "read-only inherited feed",
            "a visualizer cannot write or remap a reopened feed, and its output cannot block playback");
        const auto pidFile = root / "lingering.pid";
        auto lingering = std::make_unique<HelperProcess>();
        bool lingerStarted = lingering->Start(FromUtf8(program.string()), arguments + L" " +
            FromUtf8(pidFile.string()), FromUtf8(root.string()), error, handle, true);
        uint64_t until = GetTickCount64() + 3000;
        while (lingerStarted && !fs::exists(pidFile) && GetTickCount64() < until) usleep(10000);
        std::ifstream pidInput(pidFile);
        pid_t pid = 0;
        pidInput >> pid;
        const uint64_t beforeClose = GetTickCount64();
        lingering.reset();
        bool responsive = GetTickCount64() - beforeClose < 500;
        until = GetTickCount64() + 3000;
        while (pid > 0 && kill(pid, 0) == 0 && GetTickCount64() < until) {
            ReapHelperPrograms();
            usleep(10000);
        }
        check(lingerStarted && pid > 0 && responsive && kill(pid, 0) < 0,
            "closing a lingering visualizer stays responsive and its stopped process is reaped");
        std::error_code ec;
        fs::remove_all(root, ec);
        check(!ec, "visual-feed checks remove their child fixture and output");
    } else {
        printf("skip visualizer child inheritance: Python fixture interpreter unavailable\n");
        if (*temporary) rmdir(temporary);
    }
    writer.Close();
    check(ReadHelperFeed(shared, copy) && copy.state == 2,
        "an existing reader mapping remains valid when cordinal releases its writer");
    munmap((void*)shared, sizeof(HelperFeed));
    close(reader);
    return failures + CheckHelperVisualizerLifecycle();
}
}
#endif
