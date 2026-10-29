// The Windows file operations used by pack staging; existing files are never overwritten by Copy.
#pragma once
#include <windows.h>
#include <string>

namespace app::packfiles {
bool Exists(const std::wstring& path);
bool Same(const std::wstring& first, const std::wstring& second);
DWORD CreateDirectories(const std::wstring& path);
DWORD Copy(const std::wstring& from, const std::wstring& to);
DWORD Remove(const std::wstring& path);
DWORD Move(const std::wstring& from, const std::wstring& to);
}
