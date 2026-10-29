// Decode benchmark for the test build: cordinal-test.exe --decode-benchmark <dir>.
// Times a full decode of every audio file directly in <dir>, 20 times each, and reports the
// speed as a multiple of realtime. The table goes to both the companion's console and
// decode-benchmark.txt next to the exe for measurement scripts.
// With --pipeline-benchmark it instead times the whole engine pipeline (decode + resample + mix)
// at an optional output rate given as the third argument.
#include "app.h"
#include "decoder.h"
#include "engine.h"

#include <cstdarg>
#include <cstdio>

namespace {

FILE* g_bench = nullptr;

// One formatted line, byte for byte, to stdout and (when open) to the results file.
void BenchPrint(const char* fmt, ...) {
    char line[1024];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(line, sizeof line, fmt, args);
    va_end(args);
    if (n < 0) return;
    if (n > (int)sizeof line - 1) n = (int)sizeof line - 1;
    fwrite(line, 1, (size_t)n, stdout);
    fflush(stdout);
    if (g_bench) {
        fwrite(line, 1, (size_t)n, g_bench);
        fflush(g_bench);
    }
}

// Reads the decoder to its end, discarding the samples; returns the frames decoded.
uint64_t Drain(Decoder& d) {
    float buf[4096 * 2];
    uint64_t frames = 0;
    while (size_t n = d.Read(buf, 4096)) frames += n;
    return frames;
}

}  // namespace

int RunDecodeBenchmark(const std::vector<std::wstring>& args) {
    if (args.size() < 2) return 2;  // --decode-benchmark <dir>
    const std::wstring dir = args[1];
    std::wstring exe(MAX_PATH, L'\0');
    DWORD pathLen = GetModuleFileNameW(nullptr, exe.data(), (DWORD)exe.size());
    exe.resize(pathLen);
    if (_wfopen_s(&g_bench, (DirectoryOf(exe) + L"\\decode-benchmark.txt").c_str(), L"wb")) g_bench = nullptr;

    // Direct files only (never recursive), in a stable order.
    std::vector<std::wstring> files;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring path = dir + L"\\" + fd.cFileName;
            if (IsSupportedAudioFile(path)) files.push_back(std::move(path));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(files.begin(), files.end());

    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    const double ticksPerSecond = (double)freq.QuadPart;
    BenchPrint("decode benchmark: %s (%zu file(s), 20 passes each)\n", ToUtf8(dir).c_str(), files.size());
    BenchPrint("%-24s %12s %12s %14s\n", "file", "audio (s)", "decode (s)", "speed");
    int status=0;
    for (const std::wstring& path : files) {
        const std::string name = ToUtf8(FileNameOf(path));
        // Pipeline mode runs the real playback path (engine decode + resample + mixer) rather than
        // a single decoder, so the reported speed reflects what the player actually does.
        if (args[0]==L"--pipeline-benchmark") {
            unsigned outputRate=args.size()>2?(unsigned)wcstoul(args[2].c_str(),nullptr,10):48000;
            PipelineTiming timing;
            if (!BenchmarkPlayerPipeline(path,outputRate,20,timing)) {
                BenchPrint("%-24s %12s %12s %14s\n",name.c_str(),"-","-","pipeline failed");status=1;continue;
            }
            double audio=timing.inputFrames/(double)timing.inputRate;
            BenchPrint("%-24s %12.3f %12.9f %11.1fx\n",name.c_str(),audio,timing.seconds,audio/timing.seconds);
            continue;
        }
        std::wstring error;
        auto d = OpenDecoder(path, 48000, error);
        if (!d) {
            BenchPrint("%-24s %12s %12s %14s\n", name.c_str(), "-", "-", "open failed");
            status=1;
            continue;
        }
        const unsigned rate = d->Rate();
        const uint64_t frames = Drain(*d);  // one untimed pass, for the audio seconds
        bool complete=frames!=0;
        LARGE_INTEGER from{}, to{};
        QueryPerformanceCounter(&from);
        for (int i = 0; i < 20; ++i) {
            if (!d->Seek(0)) {  // a decoder that cannot rewind is simply reopened
                d = OpenDecoder(path, 48000, error);
                if (!d) { complete=false;break; }
            }
            if (Drain(*d)!=frames) { complete=false;break; }
        }
        QueryPerformanceCounter(&to);
        if (!complete) {
            BenchPrint("%-24s %12s %12s %14s\n",name.c_str(),"-","-","decode failed");
            status=1;continue;
        }
        const double seconds = (double)(to.QuadPart - from.QuadPart) / ticksPerSecond / 20.0;
        const double audio = rate ? (double)frames / rate : 0.0;
        const double speed = seconds > 0 ? audio / seconds : 0.0;
        BenchPrint("%-24s %12.3f %12.9f %11.1fx\n", name.c_str(), audio, seconds, speed);
    }
    BenchPrint("done\n");
    if (g_bench) fclose(g_bench);
    return status;
}
