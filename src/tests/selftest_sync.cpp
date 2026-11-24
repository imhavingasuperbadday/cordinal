// Library sync through a shared folder (syncfolder.h): the shared checks of sync_checks.h, which the Linux
// core test runs too.
#include "selftest_common.h"
#include "sync_checks.h"

void TestSync(const std::wstring& scratch) {
    for (const auto& [ok, what] : sync_checks::All(scratch)) Check(ok, L"%hs", what.c_str());
}
