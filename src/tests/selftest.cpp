// Shared assertion, scratch-file, decoding and message-pump helpers for the feature tests.
// The selectable suite registry and execution policy live in selftest_runner.cpp.
#include "selftest_common.h"

FILE* g_log = nullptr;

// Keep the call site's location intact through the common variadic assertion wrapper.
void Check(testlog::Condition ok, const wchar_t* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    testlog::CheckV(ok, fmt, args);
    va_end(args);
}

bool Ready(testlog::Condition ok, const wchar_t* what) {
    if (!ok) testlog::Check(ok, L"setup failed: %ls", what);
    return ok;
}

bool WriteTestBytes(const std::wstring& file, std::string_view bytes) {
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(h, bytes.data(), (DWORD)bytes.size(), &written, nullptr) && written == bytes.size();
    CloseHandle(h);
    return ok;
}

std::string ReadTestBytes(const std::wstring& file) {
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    DWORD size = GetFileSize(h, nullptr), got = 0;
    std::string bytes(size, '\0');
    bool ok = ReadFile(h, bytes.data(), size, &got, nullptr) && got == size;
    CloseHandle(h);
    return ok ? bytes : std::string();
}

std::vector<float> DecodeAll(const std::wstring& path, unsigned& rate) {
    std::wstring error;
    std::vector<float> all;
    auto d = OpenDecoder(path, 48000, error);
    if (!d) return all;
    rate = d->Rate();
    float buf[4096 * 2];
    while (size_t n = d->Read(buf, 4096)) all.insert(all.end(), buf, buf + n * 2);
    return all;
}

void Pump(DWORD ms, const std::function<bool()>& done) {
    ULONGLONG until = GetTickCount64() + ms;
    while (GetTickCount64() < until) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
        if (done && done()) return;
        Sleep(10);
    }
}
