// Test the test harness itself: lost assertions, hidden skips or malformed escaping could make
// every feature's result misleading. Deliberate failures below stay in a private logger state.
#include "selftest_common.h"
#include "test_report.h"
#include <thread>

void TestHarness(const std::wstring& scratch) {
    auto& state = testlog::state();
    auto previous = std::move(state);
    state = {};
    const unsigned line = __LINE__ + 3;
    {
        testlog::Group outer(L"outer");
        Check(false, L"intentional failure <雪> & \"quote\"\nnext line");
        { testlog::Group inner(L"inner"); Check(true, L"inner passes"); Check(false, L"inner fails"); }
        testlog::Skip(L"unavailable hardware");
        Check(true, L"outer passes");
    }
    auto captured = std::move(state);
    state = std::move(previous);
    Check(captured.checks == 4 && captured.failures == 2 && captured.details.size() == 2 && captured.stack.empty(),
        L"nested groups count every assertion and failure exactly once");
    Check(captured.skipped == 1 && captured.skips == std::vector<std::wstring>{L"unavailable hardware"},
        L"skipped checks are separate from passes and failures");
    Check(captured.details.size() == 2 && captured.details[0].line == line &&
        captured.details[0].file.find("selftest_harness.cpp") != std::string::npos,
        L"failure diagnostics retain the assertion's original source file and line");
    TestReport report;
    report.headless = true; report.seed = 73; report.complete = true;
    SuiteResult failed; failed.id = L"sample"; failed.source = L"src/tests/selftest_harness.cpp";
    failed.status = L"failed"; failed.checks = 4; failed.failures = 2; failed.details = captured.details;
    failed.skippedChecks = 1; failed.skips = captured.skips;
    report.suites.push_back(failed);
    SuiteResult skipped; skipped.id = L"device"; skipped.status = L"skipped"; skipped.reason = L"no device";
    report.suites.push_back(skipped);
    const auto json = TestReportJson(report), xml = TestReportXml(report);
    Check(json.find("\"checks\":4") != json.npos && json.find("\"failures\":2") != json.npos &&
        json.find("\"skipped\":2") != json.npos && json.find("\"seed\":73") != json.npos &&
        json.find("\\\"quote\\\"\\u000anext line") != json.npos && json.find("雪") != json.npos,
        L"JSON reports preserve totals, seeds, Unicode and escaped assertion text");
    Check(xml.find("tests=\"2\" failures=\"1\" skipped=\"1\" errors=\"0\"") != xml.npos &&
        xml.find("&lt;雪&gt; &amp; &quot;quote&quot;") != xml.npos && xml.find("<skipped message=\"no device\"/>") != xml.npos,
        L"JUnit reports count suites separately from assertions and escape failure text");
    report.complete = false; report.suites[0].status = L"running";
    Check(TestReportJson(report).find("\"complete\":false") != std::string::npos &&
        TestReportXml(report).find("<error message=\"Suite did not finish\"/>") != std::string::npos,
        L"an interrupted suite remains distinguishable from a completed run");

    report.log = scratch + L"\\publication";
    const std::wstring path = report.log + L".json";
    const bool initial = WriteTestReports(report);
    const auto original = ReadTestBytes(path);
    HANDLE locked = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    report.complete = true;
    bool published = false;
    if (locked != INVALID_HANDLE_VALUE) {
        std::thread reader([locked] { Sleep(50); CloseHandle(locked); });
        published = WriteTestReports(report);
        reader.join();
    }
    Check(initial && published && ReadTestBytes(path) == TestReportJson(report),
        L"a brief reader lock does not prevent atomic report publication");

    const auto saved = ReadTestBytes(path);
    locked = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    report.seed = 74;
    const bool rejected = locked != INVALID_HANDLE_VALUE && !WriteTestReports(report);
    if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
    const std::wstring temp = path + L".tmp." + std::to_wstring(GetCurrentProcessId());
    Check(rejected && !original.empty() && ReadTestBytes(path) == saved &&
        GetFileAttributesW(temp.c_str()) == INVALID_FILE_ATTRIBUTES,
        L"a persistent report lock fails without changing the previous report or leaving temporary output");
}
