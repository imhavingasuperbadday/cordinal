// Test-mode entry point for cordinal-test.exe: --selftest, --memory-benchmark and
// --decode-benchmark. The shipping exe links src\stub\no_tests.cpp instead, which never enters
// a test mode directly.
#include "app.h"
#include "packed_image.h"
#include <cstdio>
#include <fcntl.h>
#include <io.h>

// Implemented in the other test translation units; each writes a log file and returns its exit code.
int RunSelfTest(const std::vector<std::wstring>& args);
int ListSelfTests();
int RunMemoryBenchmark(const std::vector<std::wstring>& args);
int RunDecodeBenchmark(const std::vector<std::wstring>& args);
int RunPerformanceBenchmark(const std::vector<std::wstring>& args);
int RunRuntimeBenchmark(const std::vector<std::wstring>& args);
int RunWebPreview(const std::vector<std::wstring>& args);
int RunAwayProbe();
int RunFakeHelper(const std::vector<std::wstring>& args);

// Dispatches on the first argument; nullopt means "not a test mode", so the player starts normally.
// --pipeline-benchmark deliberately shares RunDecodeBenchmark, which switches on args[0] itself.
std::optional<int> RunTestMode(const std::vector<std::wstring>& args) {
    if (!args.empty() && args[0] == L"--list-tests") return ListSelfTests();
    if (!args.empty() && args[0] == L"--fake-helper") return RunFakeHelper(args);  // the helpers suite's program
    if (!args.empty() && args[0] == L"--web-preview") return RunWebPreview(args);
    if (!args.empty() && args[0] == L"--away-probe") return RunAwayProbe();
    if (!args.empty() && args[0] == L"--runtime-benchmark") return RunRuntimeBenchmark(args);
    if (!args.empty() && args[0] == L"--performance-benchmark") return RunPerformanceBenchmark(args);
    if (!args.empty() && args[0] == L"--selftest") return RunSelfTest(args);
    if (!args.empty() && args[0] == L"--memory-benchmark") return RunMemoryBenchmark(args);
    if (!args.empty() && args[0] == L"--decode-benchmark") return RunDecodeBenchmark(args);
    if (!args.empty() && args[0] == L"--pipeline-benchmark") return RunDecodeBenchmark(args);
    return std::nullopt;
}

// The developer companion is a console program; the shipping player still enters wWinMain.
int wmain(int argc, wchar_t** argv) {
    RestorePackedImageProtection();
    std::vector<std::wstring> args(argv + 1, argv + argc);
    // Decode benchmarks already write UTF-8 bytes with fwrite; other diagnostics use wide I/O.
    bool byteOutput = !args.empty() && (args[0] == L"--decode-benchmark" || args[0] == L"--pipeline-benchmark");
    _setmode(_fileno(stdout), byteOutput ? _O_BINARY : _O_U8TEXT);
    _setmode(_fileno(stderr), _O_U8TEXT);
    SetConsoleOutputCP(CP_UTF8);
    if (args.empty() || args[0] == L"--help") {
        fwprintf(stdout, L"cordinal headless testing and diagnostics\n"
            L"  --list-tests\n"
            L"  --selftest <fixtures> <log> [--headless] [--suite id,...] [--seed N] [--fail-fast]\n"
            L"  --decode-benchmark <audio-folder> | --pipeline-benchmark <audio-folder>\n"
            L"  --memory-benchmark | --performance-benchmark | --runtime-benchmark\n"
            L"  --away-probe   check this network for away-from-home listening (asks the router and STUN servers)\n"
            L"test.bat adds fixture generation, timeouts, fresh run folders and repeated runs.\n");
        return 0;
    }
    if (auto code = RunTestMode(args)) return *code;
    fwprintf(stderr, L"Unknown diagnostic mode: %ls; use --help\n", args[0].c_str());
    return 2;
}
