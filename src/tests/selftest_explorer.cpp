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
    Check(!ExplorerMenuInstalled(base.c_str()), L"Explorer: the scratch base starts without folder verbs");

    Check(InstallExplorerMenu(base.c_str()), L"Explorer: the folder verbs are written");
    Check(ExplorerMenuInstalled(base.c_str()), L"Explorer: every verb reads back pointing at this build");
    const std::wstring exe = L"\"" + ExePath() + L"\"";
    Check(ExplorerPlayCommand() == exe + L" \"%1\"", L"Explorer: Play passes the clicked folder as %%1");
    Check(ExplorerAddCommand() == exe + L" --add \"%1\"",
          L"Explorer: Add passes the folder and --add, so nothing starts playing");
    Check(VerbValue(base, L"Directory\\shell\\Cordinal.Play", nullptr) == L"Play with cordinal" &&
              VerbValue(base, L"Directory\\shell\\Cordinal.Play", L"Icon") == exe + L",0",
          L"Explorer: the folder menu names the verb and shows this build's icon");
    Check(VerbValue(base, L"Directory\\shell\\Cordinal.Add", nullptr) == L"Add to cordinal",
          L"Explorer: the second verb appends to the active playlist");
    Check(VerbValue(base, L"Directory\\Background\\shell\\Cordinal.Play", nullptr) == L"Play with cordinal",
          L"Explorer: right-clicking inside a folder offers the same play verb");
    Check(VerbValue(base, L"Directory\\Background\\shell\\Cordinal.Play\\command", nullptr) == exe + L" \"%V\"",
          L"Explorer: the background verb plays the folder being viewed (%%V)");
    Check(VerbValue(base, L"Directory\\shell\\Cordinal.Play\\command", nullptr) == ExplorerPlayCommand() &&
              VerbValue(base, L"Directory\\shell\\Cordinal.Add\\command", nullptr) == ExplorerAddCommand(),
          L"Explorer: the written commands are the ones the player reads back");

    // A build that moved must be re-registered, not trusted because a key exists.
    Check(SetVerbCommand(base, L"Directory\\shell\\Cordinal.Play", L"\"C:\\old\\cordinal.exe\" \"%1\"") &&
              !ExplorerMenuInstalled(base.c_str()),
          L"Explorer: a verb left by a previous location counts as stale");
    Check(InstallExplorerMenu(base.c_str()) && ExplorerMenuInstalled(base.c_str()),
          L"Explorer: installing again repairs a stale verb and keeps the menu idempotent");

    Check(RemoveExplorerMenu(base.c_str()) && !ExplorerMenuInstalled(base.c_str()),
          L"Explorer: turning the menu off removes every verb");
    Check(RemoveExplorerMenu(base.c_str()), L"Explorer: removing a menu that is already gone succeeds");

    // Explorer's Add verb is a command-line switch, stripped before any path reader sees it.
    std::vector<std::wstring> args{L"--add", L"C:\\Music\\Album"};
    Check(TakeAddSwitch(args) && args.size() == 1 && args[0] == L"C:\\Music\\Album",
          L"Explorer: --add is taken out and the folder is left for the import");
    args = {L"C:\\Music\\Album"};
    Check(!TakeAddSwitch(args) && args.size() == 1, L"Explorer: a plain path is left alone");
    args = {L"--add"};
    Check(TakeAddSwitch(args) && args.empty(), L"Explorer: --add alone leaves no paths");
    args = {L"--add", L"--add", L"C:\\Music"};
    Check(TakeAddSwitch(args) && args.size() == 1, L"Explorer: every --add is removed");

    // Explorer substitutes a drive root into a quoted verb as `"C:\"`, which CommandLineToArgvW
    // reads as `C:"`. The repair is what makes right-clicking inside a drive work.
    Check(RepairTrailingQuote(L"C:\"") == L"C:\\", L"Explorer: a quoted drive root keeps its backslash");
    Check(RepairTrailingQuote(L"C:\\Music with space\"") == L"C:\\Music with space",
          L"Explorer: a quoted path with a trailing backslash loses the stray quote");
    Check(RepairTrailingQuote(L"C:\\Music") == L"C:\\Music" && RepairTrailingQuote(L"--add") == L"--add",
          L"Explorer: ordinary arguments are left alone");

    RegDeleteTreeW(HKEY_CURRENT_USER, (base + L"\\Directory").c_str());
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Cordinal\\Test\\ExplorerMenu");
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Cordinal\\Test");
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Cordinal");
}
