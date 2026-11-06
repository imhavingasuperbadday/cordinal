// The user's upload command (coverupload.h): start the tool with the image's path, take the first https address
// it prints, and give up after ten seconds. Nothing here touches the UI thread; discord.cpp runs it on a worker.
#include "coverupload.h"

#include <algorithm>
#include <atomic>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>

#include <windows.h>  // the Linux build's compat shim, for compat::Narrow from UTF-32 to UTF-8
#endif

namespace coverupload {
namespace {

std::mutex g_mx;
std::wstring g_command;
std::atomic<uint32_t> g_generation{0};  // CancelAll bumps it; a run from an older one stops at once

// Long enough for a slow upload, short enough that a stuck tool never pins a worker for good.
constexpr int kTimeoutMs = 10000;
constexpr size_t kMaxOutput = 64 * 1024;

// The first word of the output that is an address Discord can render. A tool may print progress lines first, and
// the address may be followed by anything.
std::string FirstAddress(const std::string& output) {
    size_t at = 0;
    while (at < output.size()) {
        while (at < output.size() && (unsigned char)output[at] <= ' ') ++at;
        size_t end = at;
        while (end < output.size() && (unsigned char)output[end] > ' ') ++end;
        const std::string word = output.substr(at, end - at);
        at = end;
        if (word.size() <= 8 || word.size() > 256 || !word.starts_with("https://")) continue;
        if (word.find_first_of("\"'<>\\") != std::string::npos) continue;
        return word;
    }
    return {};
}

}  // namespace

bool Configured() {
    std::lock_guard lock(g_mx);
    return !g_command.empty();
}

void SetCommand(const std::wstring& command) {
    std::lock_guard lock(g_mx);
    g_command = command;
}

std::wstring Command() {
    std::lock_guard lock(g_mx);
    return g_command;
}

void CancelAll() { ++g_generation; }

#ifdef _WIN32
namespace {

bool ReadAvailable(HANDLE pipe, std::string& output) {
    DWORD available = 0;
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) return false;  // the tool closed it
    while (available) {
        char buffer[4096];
        DWORD got = 0;
        if (!ReadFile(pipe, buffer, (DWORD)std::min<size_t>(available, sizeof(buffer)), &got, nullptr) || !got)
            return false;
        output.append(buffer, got);
        available -= got;
        if (output.size() >= kMaxOutput) return false;
    }
    return true;
}

}  // namespace

std::string Run(const std::wstring& imagePath) {
    const std::wstring command = Command();
    if (command.empty()) return {};
    const uint32_t generation = g_generation.load();
    // The tool's own arguments come first and the image's path last, so a command with options keeps them.
    std::wstring cmdline = command + L" \"" + imagePath + L"\"";
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &inheritable, 0)) return {};
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    HANDLE nullIn = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, 0, nullptr);
    HANDLE nullErr = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdInput = nullIn;
    startup.hStdOutput = writePipe;
    startup.hStdError = nullErr;
    PROCESS_INFORMATION process{};
    const BOOL started = CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                        nullptr, &startup, &process);
    CloseHandle(writePipe);
    std::string output;
    if (started) {
        const ULONGLONG deadline = GetTickCount64() + kTimeoutMs;
        for (;;) {
            const DWORD wait = WaitForSingleObject(process.hProcess, 20);
            const bool flowing = ReadAvailable(readPipe, output);
            if (wait == WAIT_OBJECT_0 && !flowing) break;  // finished, and its pipe is drained
            if (GetTickCount64() > deadline || g_generation.load() != generation) {
                TerminateProcess(process.hProcess, 1);
                break;
            }
        }
        WaitForSingleObject(process.hProcess, 500);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
    CloseHandle(readPipe);
    if (nullIn != INVALID_HANDLE_VALUE) CloseHandle(nullIn);
    if (nullErr != INVALID_HANDLE_VALUE) CloseHandle(nullErr);
    return FirstAddress(output);
}
#else
namespace {

// True while the tool is still writing; false once it closed its end or the cap was reached.
bool ReadAvailable(int fd, std::string& output) {
    char buffer[4096];
    for (;;) {
        const ssize_t got = ::read(fd, buffer, sizeof(buffer));
        if (got == 0) return false;
        if (got < 0) return errno == EINTR || errno == EAGAIN;
        output.append(buffer, (size_t)got);
        if (output.size() >= kMaxOutput) return false;
    }
}

}  // namespace

std::string Run(const std::wstring& imagePath) {
    const std::wstring command = Command();
    if (command.empty()) return {};
    const uint32_t generation = g_generation.load();
    int ends[2];
    if (::pipe(ends) != 0) return {};
    const std::string path = compat::Narrow(imagePath.c_str());
    const pid_t child = ::fork();
    if (child == 0) {
        ::dup2(ends[1], STDOUT_FILENO);
        const int null = ::open("/dev/null", O_WRONLY);
        if (null >= 0) ::dup2(null, STDERR_FILENO);
        ::close(ends[0]);
        ::close(ends[1]);
        // The path arrives as "$1", so spaces and quotes in it cannot be read as shell syntax.
        const std::string script = compat::Narrow(command.c_str()) + " \"$1\"";
        ::execl("/bin/sh", "sh", "-c", script.c_str(), "sh", path.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }
    ::close(ends[1]);
    if (child < 0) {
        ::close(ends[0]);
        return {};
    }
    std::string output;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kTimeoutMs);
    for (;;) {
        pollfd ready{ends[0], POLLIN, 0};
        const int signaled = ::poll(&ready, 1, 20);
        if (signaled > 0 && (ready.revents & POLLIN) && !ReadAvailable(ends[0], output)) break;
        if (signaled > 0 && (ready.revents & (POLLHUP | POLLERR))) {
            ReadAvailable(ends[0], output);
            break;
        }
        if (::waitpid(child, nullptr, WNOHANG) == child) {
            ReadAvailable(ends[0], output);  // whatever it printed before exiting
            break;
        }
        if (std::chrono::steady_clock::now() > deadline || g_generation.load() != generation) {
            ::kill(child, SIGKILL);
            ::waitpid(child, nullptr, 0);
            break;
        }
    }
    ::close(ends[0]);
    return FirstAddress(output);
}
#endif

}  // namespace coverupload
