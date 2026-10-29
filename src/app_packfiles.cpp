// Pack staging on Windows, without the general filesystem path/iterator machinery. Copy follows
// source links like the old recursive copy; removal opens links themselves and never walks a target.
#include "app_packfiles.h"
#include <cstring>

namespace app::packfiles {
namespace {
struct Handle {
    HANDLE value;
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct FindHandle {
    HANDLE value;
    ~FindHandle() { if (value != INVALID_HANDLE_VALUE) FindClose(value); }
};

bool Missing(DWORD error) { return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND; }
DWORD CopyEntry(const std::wstring& from, const std::wstring& to, DWORD attributes);
DWORD RemoveEntryOrTree(const std::wstring& path, DWORD attributes);

DWORD CreateOneDirectory(const std::wstring& path) {
    if (CreateDirectoryW(path.c_str(), nullptr)) return ERROR_SUCCESS;
    DWORD error = GetLastError();
    if (error == ERROR_ALREADY_EXISTS) {
        DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) return ERROR_SUCCESS;
    }
    return error;
}

HANDLE Open(const std::wstring& path, DWORD access, DWORD flags = 0) {
    return CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | flags, nullptr);
}

// Keep the deletion handle tied to the directory entry, even if a link's target is missing.
DWORD RemoveEntry(const std::wstring& path) {
    Handle file{Open(path, DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES, FILE_FLAG_OPEN_REPARSE_POINT)};
    if (file.value == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED)
        file.value = Open(path, DELETE, FILE_FLAG_OPEN_REPARSE_POINT);
    if (file.value == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        return Missing(error) ? ERROR_SUCCESS : error;
    }
    FILE_DISPOSITION_INFO_EX remove{FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS |
                                    FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE};
    if (SetFileInformationByHandle(file.value, FileDispositionInfoEx, &remove, sizeof(remove))) return ERROR_SUCCESS;
    DWORD error = GetLastError();
    if (error != ERROR_INVALID_PARAMETER && error != ERROR_NOT_SUPPORTED && error != ERROR_INVALID_FUNCTION)
        return error;
    FILE_DISPOSITION_INFO basic{TRUE};
    if (SetFileInformationByHandle(file.value, FileDispositionInfo, &basic, sizeof(basic))) return ERROR_SUCCESS;
    error = GetLastError();
    FILE_BASIC_INFO attributes{};
    if (error != ERROR_ACCESS_DENIED ||
        !GetFileInformationByHandleEx(file.value, FileBasicInfo, &attributes, sizeof(attributes)) ||
        !(attributes.FileAttributes & FILE_ATTRIBUTE_READONLY)) return error;
    FILE_BASIC_INFO writable = attributes;
    writable.FileAttributes &= ~FILE_ATTRIBUTE_READONLY;
    if (!SetFileInformationByHandle(file.value, FileBasicInfo, &writable, sizeof(writable))) return GetLastError();
    if (SetFileInformationByHandle(file.value, FileDispositionInfo, &basic, sizeof(basic))) return ERROR_SUCCESS;
    error = GetLastError();
    SetFileInformationByHandle(file.value, FileBasicInfo, &attributes, sizeof(attributes));
    return error;
}

DWORD Children(const std::wstring& from, const std::wstring* to) {
    WIN32_FIND_DATAW data{};
    FindHandle find{FindFirstFileW((from + L"\\*").c_str(), &data)};
    if (find.value == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : error;
    }
    DWORD error = ERROR_SUCCESS;
    do {
        if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L"..")) continue;
        const std::wstring child = from + L"\\" + data.cFileName;
        error = to ? CopyEntry(child, *to + L"\\" + data.cFileName, data.dwFileAttributes)
                   : RemoveEntryOrTree(child, data.dwFileAttributes);
        if (error) break;
    } while (FindNextFileW(find.value, &data));
    if (!error && GetLastError() != ERROR_NO_MORE_FILES) error = GetLastError();
    return error;
}

DWORD CopyEntry(const std::wstring& from, const std::wstring& to, DWORD attributes) {
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY))
        return CopyFileW(from.c_str(), to.c_str(), TRUE) ? ERROR_SUCCESS : GetLastError();
    // A directory template can clone its reparse data. Follow a source link into a plain
    // staged directory instead: the installed copy must not point back into Downloads.
    bool made = attributes & FILE_ATTRIBUTE_REPARSE_POINT ? CreateDirectoryW(to.c_str(), nullptr) != FALSE
                                                        : CreateDirectoryExW(from.c_str(), to.c_str(), nullptr) != FALSE;
    if (!made) {
        DWORD error = GetLastError(), target = GetFileAttributesW(to.c_str());
        if (error != ERROR_ALREADY_EXISTS || target == INVALID_FILE_ATTRIBUTES || !(target & FILE_ATTRIBUTE_DIRECTORY))
            return error;
    }
    return Children(from, &to);
}

DWORD RemoveEntryOrTree(const std::wstring& path, DWORD attributes) {
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        if (DWORD error = Children(path, nullptr)) return error;
    return RemoveEntry(path);
}
}

bool Exists(const std::wstring& path) {
    DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return false;
    if (!(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return true;
    Handle file{Open(path, FILE_READ_ATTRIBUTES)};
    return file.value != INVALID_HANDLE_VALUE;
}

bool Same(const std::wstring& first, const std::wstring& second) {
    Handle a{Open(first, FILE_READ_ATTRIBUTES)}, b{Open(second, FILE_READ_ATTRIBUTES)};
    if (a.value == INVALID_HANDLE_VALUE || b.value == INVALID_HANDLE_VALUE) return false;
    FILE_ID_INFO idA{}, idB{};
    if (GetFileInformationByHandleEx(a.value, FileIdInfo, &idA, sizeof(idA)) &&
        GetFileInformationByHandleEx(b.value, FileIdInfo, &idB, sizeof(idB)))
        return idA.VolumeSerialNumber == idB.VolumeSerialNumber && !memcmp(&idA.FileId, &idB.FileId, sizeof(idA.FileId));
    BY_HANDLE_FILE_INFORMATION infoA{}, infoB{};
    return GetFileInformationByHandle(a.value, &infoA) && GetFileInformationByHandle(b.value, &infoB) &&
           infoA.dwVolumeSerialNumber == infoB.dwVolumeSerialNumber && infoA.nFileIndexHigh == infoB.nFileIndexHigh &&
           infoA.nFileIndexLow == infoB.nFileIndexLow;
}

DWORD CreateDirectories(const std::wstring& path) {
    DWORD error = CreateOneDirectory(path);
    if (error != ERROR_PATH_NOT_FOUND) return error;
    size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos || !slash) return error;
    if ((error = CreateDirectories(path.substr(0, slash)))) return error;
    return CreateOneDirectory(path);
}

DWORD Copy(const std::wstring& from, const std::wstring& to) {
    DWORD attributes = GetFileAttributesW(from.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return GetLastError();
    return CopyEntry(from, to, attributes);
}

DWORD Remove(const std::wstring& path) {
    DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        DWORD error = GetLastError();
        return Missing(error) ? ERROR_SUCCESS : error;
    }
    return RemoveEntryOrTree(path, attributes);
}

DWORD Move(const std::wstring& from, const std::wstring& to) {
    return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) ? ERROR_SUCCESS : GetLastError();
}
}
