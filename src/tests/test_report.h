// Machine-readable results from the same assertions that produce the self-test log.
#pragma once
#include "testlog.h"

struct SuiteResult {
    std::wstring id, name, source, requirements, status = L"running", reason, artifacts;
    int checks = 0, failures = 0, skippedChecks = 0;
    double milliseconds = 0;
    std::vector<testlog::Failure> details;
    std::vector<std::wstring> skips;
};

struct TestReport {
    std::wstring log, fixtures;
    bool headless = false, complete = false;
    unsigned seed = 1;
    std::vector<SuiteResult> suites;
};

std::string TestReportJson(const TestReport& report);
std::string TestReportXml(const TestReport& report);
bool WriteTestReports(const TestReport& report);
