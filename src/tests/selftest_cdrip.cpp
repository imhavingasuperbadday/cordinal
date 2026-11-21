// CD ripping without a drive: the shared checks of cdrip_checks.h (disc ids, CD-Text, lookups on
// canned replies, verified reads from a disc in memory, and whole rips into FLAC and .cord).
#include "selftest_common.h"

#include "cdrip_checks.h"

void TestCdRip(const std::wstring& scratch) {
    for (const auto& group : cdrip_checks::All(scratch)) {
        std::wstring failed;
        for (const auto& f : group.failures) failed += L" | " + FromUtf8(f);
        Check(group.Passed(), L"CD RIP %ls (%d cases)%ls", FromUtf8(group.name).c_str(), group.cases, failed.c_str());
    }
}
