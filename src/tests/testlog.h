// Shared assertion accounting for self-tests, tag writers, memory and size checks. Groups keep
// successful logs compact; failures are flushed immediately with their source location. Totals
// count assertions once regardless of group nesting, and skipped checks have their own count.
#pragma once

#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <source_location>
#include <string>
#include <vector>

namespace testlog {

struct Group;

// The implicit conversion captures the assertion's caller, including through variadic wrappers.
struct Condition {
    bool value;
    std::source_location location;
    Condition(bool result, std::source_location at = std::source_location::current())
        : value(result), location(at) {}
    operator bool() const { return value; }
};

struct Failure {
    std::wstring message;
    std::string file;
    unsigned line = 0;
};

struct State {
    FILE* log = nullptr;
    bool console = false;
    int failures = 0;
    int checks = 0;
    int skipped = 0;
    std::vector<Failure> details;
    std::vector<std::wstring> skips;
    std::vector<Group*> stack;
};

inline State& state() {
    static State s;
    return s;
}

// Installs the log file every later Check/Group writes to (null silences output).
inline void SetLog(FILE* log) { state().log = log; }

// Total failed checks so far, for the process's exit code.
inline int Failures() { return state().failures; }

inline void Skip(const wchar_t* message) {
    State& s = state();
    ++s.skipped;
    s.skips.emplace_back(message);
    if (s.log) { fwprintf(s.log, L"SKIP %ls\n", message); fflush(s.log); }
    if (s.console) { fwprintf(stdout, L"SKIP %ls\n", message); fflush(stdout); }
}

struct Group {
    std::wstring label;
    int checks = 0;
    int failed = 0;
    std::wstring detail;

    explicit Group(std::wstring name) : label(std::move(name)) { state().stack.push_back(this); }
    Group(const Group&) = delete;
    Group& operator=(const Group&) = delete;

    ~Group() {
        State& s = state();
        if (!s.stack.empty() && s.stack.back() == this) s.stack.pop_back();
        if (checks && s.log) {
            const wchar_t* plural = checks == 1 ? L"" : L"s";
            if (!failed)
                fwprintf(s.log, L"PASS %ls (%d check%ls)\n", label.c_str(), checks, plural);
            else
                fwprintf(s.log, L"FAIL %ls (%d of %d check%ls failed: %ls)\n", label.c_str(), failed, checks,
                         plural, detail.c_str());
            fflush(s.log);
        }
    }

    // Records one check; failing ones accumulate into `detail`, capped so a wildly failing group
    // still logs a bounded line (the rest become a single "...").
    void Add(bool ok, const wchar_t* message) {
        ++checks;
        if (ok) return;
        ++failed;
        if (failed <= 12) {
            if (!detail.empty()) detail += L" | ";
            detail += message;
        } else if (failed == 13) {
            detail += L" | ...";
        }
    }
};

inline void CheckV(Condition ok, const wchar_t* fmt, va_list args) {
    wchar_t message[2048];
    _vsnwprintf_s(message, 2048, _TRUNCATE, fmt, args);
    State& s = state();
    ++s.checks;
    if (!ok) {
        ++s.failures;
        s.details.push_back({message, ok.location.file_name(), ok.location.line()});
        // Write failures immediately, even inside a group: a later crash must not hide them.
        if (s.log) {
            fwprintf(s.log, L"FAIL %hs:%u: %ls\n", ok.location.file_name(), ok.location.line(), message);
            fflush(s.log);
        }
        if (s.console) {
            fwprintf(stdout, L"FAIL %hs:%u: %ls\n", ok.location.file_name(), ok.location.line(), message);
            fflush(stdout);
        }
    }
    if (!s.stack.empty()) {
        s.stack.back()->Add(ok, message);
        return;
    }
    if (ok && s.log) {
        fwprintf(s.log, L"%ls %ls\n", ok ? L"PASS" : L"FAIL", message);
        fflush(s.log);
    }
}

inline void Check(Condition ok, const wchar_t* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    CheckV(ok, fmt, args);
    va_end(args);
}

// The cases of one check that runs over many (codecs, files, seeded trials): the check counts once,
// and `failed` names the first few cases that did not hold, ready to append to its message.
struct Cases {
    std::wstring failed;
    int count = 0;
    void Add(bool ok, const std::wstring& name) {
        if (ok || ++count > 6) return;
        failed = (count == 1 ? std::wstring(L" (failed: ") : failed.substr(0, failed.size() - 1) + L", ") + name + L")";
    }
    bool Passed() const { return count == 0; }
};

}  // namespace testlog
