// Explorer's folder verbs: the command lines Explorer will run, and a registry round trip under a
// scratch key, so a check never touches the user's real folder menu.
#include "selftest_common.h"
#include "command_line.h"

namespace {

std::wstring VerbValue(const std::wstring& base, const wchar_t* subkey, const wchar_t* name) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, (base + L"\\" + subkey).c_str(), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return {};
    wchar_t value[2048]{};
    DWORD size = sizeof(value), type = 0;
    const LSTATUS status = RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(value), &size);
    RegCloseKey(key);
    return status == ERROR_SUCCESS && type == REG_SZ ? std::wstring(value) : std::wstring();
}

bool SetVerbCommand(const std::wstring& base, const wchar_t* subkey, const std::wstring& command) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, (base + L"\\" + subkey + L"\\command").c_str(), 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    const LSTATUS status = RegSetValueExW(key, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                                          (DWORD)((command.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

std::wstring ExePath() {
    wchar_t exe[MAX_PATH * 2]{};
    const DWORD n = GetModuleFileNameW(nullptr, exe, (DWORD)std::size(exe));
    return std::wstring(exe, n);
}

}  // namespace

void TestExplorerMenu() {
    using namespace app;
    const std::wstring base = L"Software\\Cordinal\\Test\\ExplorerMenu";
    // A scratch base: a failed earlier run must not make this pass or fail by accident.
    RemoveExplorerMenu(base.c_str());
    Ready(!ExplorerMenuInstalled(base.c_str()), L"the scratch folder-menu base starts without verbs");

    const bool installed = InstallExplorerMenu(base.c_str());
    const bool readBack = installed && ExplorerMenuInstalled(base.c_str());
    Check(readBack, L"Explorer: installing writes the folder verbs and every verb reads back pointing at this "
          L"build (installed=%d read back=%d)", installed, readBack);
    const std::wstring exe = L"\"" + ExePath() + L"\"";
    Check(ExplorerPlayCommand() == exe + L" \"%1\"" && ExplorerAddCommand() == exe + L" --add \"%1\"",
          L"Explorer: Play passes the clicked folder as %%1, and Add passes it with --add so nothing starts "
          L"playing");
    testlog::Cases verbs;
    verbs.Add(VerbValue(base, L"Directory\\shell\\Cordinal.Play", nullptr) == L"Play with cordinal",
              L"the Directory Play name");
    verbs.Add(VerbValue(base, L"Directory\\shell\\Cordinal.Play", L"Icon") == exe + L",0",
              L"the Directory Play icon");
    verbs.Add(VerbValue(base, L"Directory\\shell\\Cordinal.Add", nullptr) == L"Add to cordinal",
              L"the Directory Add name");
    verbs.Add(VerbValue(base, L"Directory\\Background\\shell\\Cordinal.Play", nullptr) == L"Play with cordinal",
              L"the Background Play name");
    verbs.Add(VerbValue(base, L"Directory\\Background\\shell\\Cordinal.Play\\command", nullptr) == exe + L" \"%V\"",
              L"the Background Play command (%V)");
    verbs.Add(VerbValue(base, L"Directory\\shell\\Cordinal.Play\\command", nullptr) == ExplorerPlayCommand(),
              L"the written Play command");
    verbs.Add(VerbValue(base, L"Directory\\shell\\Cordinal.Add\\command", nullptr) == ExplorerAddCommand(),
              L"the written Add command");
    Check(verbs.Passed(), L"Explorer: the folder and background menus name their verbs, show this build's icon "
          L"and write the commands the player reads back%ls", verbs.failed.c_str());

    // A build that moved must be re-registered, not trusted because a key exists.
    const bool stale =
        SetVerbCommand(base, L"Directory\\shell\\Cordinal.Play", L"\"C:\\old\\cordinal.exe\" \"%1\"") &&
        !ExplorerMenuInstalled(base.c_str());
    const bool repaired = InstallExplorerMenu(base.c_str()) && ExplorerMenuInstalled(base.c_str());
    Check(stale && repaired,
          L"Explorer: a verb left by a previous location counts as stale, and installing again repairs it and "
          L"keeps the menu idempotent (stale=%d repaired=%d)", stale, repaired);

    const bool removed = RemoveExplorerMenu(base.c_str()) && !ExplorerMenuInstalled(base.c_str());
    const bool absent = RemoveExplorerMenu(base.c_str());
    Check(removed && absent, L"Explorer: turning the menu off removes every verb, and removing it again "
          L"succeeds (removed=%d absent=%d)", removed, absent);

    // Explorer's Add verb is a command-line switch, stripped before any path reader sees it.
    std::vector<std::wstring> args{L"--add", L"C:\\Music\\Album"};
    testlog::Cases addSwitch;
    addSwitch.Add(TakeAddSwitch(args) && args.size() == 1 && args[0] == L"C:\\Music\\Album",
                  L"--add is taken out and the folder is left");
    args = {L"C:\\Music\\Album"};
    addSwitch.Add(!TakeAddSwitch(args) && args.size() == 1, L"a plain path is left alone");
    args = {L"--add"};
    addSwitch.Add(TakeAddSwitch(args) && args.empty(), L"--add alone leaves no paths");
    args = {L"--add", L"--add", L"C:\\Music"};
    addSwitch.Add(TakeAddSwitch(args) && args.size() == 1, L"every --add is removed");
    Check(addSwitch.Passed(), L"Explorer: --add is taken out of the command line before any path reader sees "
          L"it%ls", addSwitch.failed.c_str());

    // Explorer substitutes a drive root into a quoted verb as `"C:\"`, which CommandLineToArgvW
    // reads as `C:"`. The repair is what makes right-clicking inside a drive work.
    testlog::Cases quoted;
    quoted.Add(RepairTrailingQuote(L"C:\"") == L"C:\\", L"a quoted drive root keeps its backslash");
    quoted.Add(RepairTrailingQuote(L"C:\\Music with space\"") == L"C:\\Music with space",
               L"a quoted path with a trailing backslash loses the stray quote");
    quoted.Add(RepairTrailingQuote(L"C:\\Music") == L"C:\\Music", L"an ordinary path is left alone");
    quoted.Add(RepairTrailingQuote(L"--add") == L"--add", L"an ordinary switch is left alone");
    Check(quoted.Passed(), L"Explorer: a quoted drive root keeps its backslash and a stray trailing quote is "
          L"removed, other arguments untouched%ls", quoted.failed.c_str());

    RegDeleteTreeW(HKEY_CURRENT_USER, (base + L"\\Directory").c_str());
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Cordinal\\Test\\ExplorerMenu");
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Cordinal\\Test");
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Cordinal");
}
