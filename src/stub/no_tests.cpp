// Preserve the player's diagnostic command line without embedding the test suite in it.
// This is the shipping stub of the test entry point: the real one (src\tests\test_entry.cpp) is
// linked only into cordinal-test.exe. When the player is asked for a test or benchmark mode it
// relaunches that companion exe next to it with the same arguments and forwards its exit code.
#include "app.h"
#include "command_line.h"

// Returns nullopt for an ordinary run (the player then starts normally); otherwise it hands the
// mode off to cordinal-test.exe and blocks until that process finishes.
std::optional<int> RunTestMode(const std::vector<std::wstring>& args) {
    if (args.empty()) return std::nullopt;
    if (args[0] != L"--selftest" && args[0] != L"--list-tests" && args[0] != L"--memory-benchmark" && args[0] != L"--decode-benchmark" &&
        args[0] != L"--pipeline-benchmark" && args[0] != L"--performance-benchmark" &&
        args[0] != L"--runtime-benchmark") return std::nullopt;
    std::wstring exe(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, exe.data(), (DWORD)exe.size());
    if (!n || n >= exe.size()) return ERROR_BAD_PATHNAME;
    exe.resize(n);
    std::wstring companion = DirectoryOf(exe) + L"\\cordinal-test.exe";
    std::wstring command = QuoteArg(companion);
    for (const auto& arg : args) command += L" " + QuoteArg(arg);
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(companion.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) return (int)GetLastError();
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = ERROR_GEN_FAILURE;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}
