// JSON and JUnit share suite IDs, failure locations and reproduction inputs. Publish after each
// suite so a terminated process leaves a report naming the work that was running.
#include "test_report.h"
#include "library.h"

namespace {
std::string Json(std::wstring_view wide) {
    std::string out = "\"";
    for (unsigned char c : ToUtf8(wide)) {
        if (c == '"' || c == '\\') { out += '\\'; out += (char)c; }
        else if (c < 32) {
            const char hex[] = "0123456789abcdef";
            out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15];
        } else out += (char)c;
    }
    return out + '"';
}
std::string Xml(std::wstring_view wide) {
    std::string out;
    for (unsigned char c : ToUtf8(wide)) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"') out += "&quot;";
        else if (c == '\'') out += "&apos;";
        else if (c >= 32 || c == '\n' || c == '\t' || c == '\r') out += (char)c;
    }
    return out;
}
struct Totals { int checks = 0, failed = 0, skipped = 0; double ms = 0; };
Totals Count(const TestReport& report) {
    Totals t;
    for (const auto& s : report.suites) {
        t.checks += s.checks; t.failed += s.failures;
        t.skipped += s.skippedChecks + (s.status == L"skipped"); t.ms += s.milliseconds;
    }
    return t;
}
bool RetrySharing(unsigned& attempts) {
    const DWORD error = GetLastError();
    if ((error != ERROR_SHARING_VIOLATION && error != ERROR_LOCK_VIOLATION && error != ERROR_ACCESS_DENIED) ||
        attempts++ == 20) return false;
    Sleep(10);
    return true;
}
bool Publish(const std::wstring& path, const std::string& bytes) {
    const std::wstring temp = path + L".tmp." + std::to_wstring(GetCurrentProcessId());
    // A report viewer or file scanner can briefly deny replacement; Windows also reports held
    // renames as access denied. Permanent failures still preserve the previous report and fail the run.
    unsigned attempts = 0;
    HANDLE file;
    do { file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr); }
    while (file == INVALID_HANDLE_VALUE && RetrySharing(attempts));
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(file, bytes.data(), (DWORD)bytes.size(), &written, nullptr) && written == bytes.size();
    ok = CloseHandle(file) && ok;
    if (ok) {
        attempts = 0;
        do { ok = MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE; }
        while (!ok && RetrySharing(attempts));
    }
    if (!ok) DeleteFileW(temp.c_str());
    return ok;
}
}

std::string TestReportJson(const TestReport& report) {
    auto t = Count(report);
    std::string out = "{\n\"schemaVersion\":1,\"processId\":" + std::to_string(GetCurrentProcessId()) +
        ",\"complete\":" + std::string(report.complete ? "true" : "false") +
        ",\"headless\":" + (report.headless ? "true" : "false") + ",\"seed\":" + std::to_string(report.seed) +
        ",\"fixtures\":" + Json(report.fixtures) + ",\"log\":" + Json(report.log) +
        ",\"checks\":" + std::to_string(t.checks) + ",\"failures\":" + std::to_string(t.failed) +
        ",\"skipped\":" + std::to_string(t.skipped) + ",\"durationMs\":" + std::to_string(t.ms) + ",\n\"suites\":[";
    bool first = true;
    for (const auto& s : report.suites) {
        if (!first) out += ',';
        first = false;
        out += "\n{\"id\":" + Json(s.id) + ",\"name\":" + Json(s.name) + ",\"source\":" + Json(s.source) +
            ",\"requirements\":" + Json(s.requirements) + ",\"status\":" + Json(s.status) + ",\"reason\":" + Json(s.reason) +
            ",\"artifacts\":" + Json(s.artifacts) + ",\"checks\":" + std::to_string(s.checks) +
            ",\"failures\":" + std::to_string(s.failures) + ",\"skippedChecks\":" + std::to_string(s.skippedChecks) +
            ",\"durationMs\":" + std::to_string(s.milliseconds) + ",\"details\":[";
        bool firstDetail = true;
        for (const auto& d : s.details) {
            if (!firstDetail) out += ',';
            firstDetail = false;
            out += "{\"message\":" + Json(d.message) + ",\"file\":" + Json(FromUtf8(d.file)) +
                ",\"line\":" + std::to_string(d.line) + '}';
        }
        out += "],\"skips\":[";
        for (size_t i = 0; i < s.skips.size(); ++i) { if (i) out += ','; out += Json(s.skips[i]); }
        out += "]}";
    }
    return out + "\n]}\n";
}

std::string TestReportXml(const TestReport& report) {
    int failed = 0, skipped = 0, errors = 0;
    for (const auto& s : report.suites) {
        failed += s.status == L"failed"; skipped += s.status == L"skipped"; errors += s.status == L"running";
    }
    auto t = Count(report);
    std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<testsuite name=\"cordinal\" tests=\"" +
        std::to_string(report.suites.size()) + "\" failures=\"" + std::to_string(failed) + "\" skipped=\"" +
        std::to_string(skipped) + "\" errors=\"" + std::to_string(errors) + "\" time=\"" + std::to_string(t.ms / 1000) +
        "\"><properties><property name=\"seed\" value=\"" + std::to_string(report.seed) +
        "\"/><property name=\"headless\" value=\"" + (report.headless ? "true" : "false") +
        "\"/><property name=\"checks\" value=\"" + std::to_string(t.checks) + "\"/></properties>\n";
    for (const auto& s : report.suites) {
        out += "<testcase classname=\"cordinal\" name=\"" + Xml(s.id) + "\" file=\"" + Xml(s.source) +
            "\" time=\"" + std::to_string(s.milliseconds / 1000) + "\">";
        if (s.status == L"skipped") out += "<skipped message=\"" + Xml(s.reason) + "\"/>";
        if (s.status == L"running") out += "<error message=\"Suite did not finish\"/>";
        if (s.failures) {
            out += "<failure message=\"" + std::to_string(s.failures) + " checks failed\">";
            for (const auto& d : s.details)
                out += Xml(FromUtf8(d.file)) + ':' + std::to_string(d.line) + ": " + Xml(d.message) + '\n';
            out += "</failure>";
        }
        out += "<system-out>artifacts=" + Xml(s.artifacts) + '\n';
        for (const auto& skip : s.skips) out += "SKIP " + Xml(skip) + '\n';
        out += "</system-out></testcase>\n";
    }
    return out + "</testsuite>\n";
}

bool WriteTestReports(const TestReport& report) {
    bool json = Publish(report.log + L".json", TestReportJson(report));
    bool xml = Publish(report.log + L".xml", TestReportXml(report));
    return json && xml;
}
