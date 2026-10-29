// The part of the Windows API that cordinal's portable core (library, tags, decoders, engine,
// scanner) uses, implemented on POSIX for the native Linux build (compat.cpp). Only the Linux
// build puts this directory on the include path; the Windows build never sees it.
//
// Paths are UTF-32 std::wstring as on Windows they are UTF-16, converted to UTF-8 at the system
// call. Handles are small objects (files, events, directory searches) that CloseHandle frees.
// PostMessageW hands the message to the front end's queue (SetPostMessageHandler), which is
// how the engine and the scanner talk to the UI thread, as they do with a window on Windows.
#pragma once
#ifdef _WIN32
#error "compat/windows.h is for the Linux build"
#endif

#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <ctime>
#include <strings.h>
#include <functional>
#include <string>

namespace compat {
std::string Narrow(const wchar_t* text);                        // UTF-32 -> UTF-8
std::wstring Widen(const char* text, size_t len = (size_t)-1);  // UTF-8 -> UTF-32
uint64_t UnixToFileTime(int64_t seconds, long nanoseconds);
}  // namespace compat

// --- types -----------------------------------------------------------------------------------
using BOOL = int;
using BYTE = uint8_t;
using WORD = uint16_t;
using DWORD = uint32_t;
using UINT = unsigned int;
using ULONG = uint32_t;
using LONG = int32_t;
using LONGLONG = int64_t;
using ULONGLONG = uint64_t;
using DWORD_PTR = uintptr_t;
using UINT_PTR = uintptr_t;
using ULONG_PTR = uintptr_t;
using WPARAM = uintptr_t;
using LPARAM = intptr_t;
using LRESULT = intptr_t;
using HRESULT = int32_t;
using WCHAR = wchar_t;
using LPWSTR = wchar_t*;
using LPCWSTR = const wchar_t*;
using PWSTR = wchar_t*;
using LPVOID = void*;
using HANDLE = void*;
using HMODULE = void*;
using HINSTANCE = void*;
struct HWND__;
using HWND = HWND__*;

#define TRUE 1
#define FALSE 0
#define WINAPI
#define CALLBACK
#define APIENTRY
#define __declspec(value) __attribute__((value))
#define MAX_PATH 260
#define LF_FACESIZE 32
#define IS_HIGH_SURROGATE(ch) ((ch) >= 0xD800 && (ch) <= 0xDBFF)
#define IS_LOW_SURROGATE(ch) ((ch) >= 0xDC00 && (ch) <= 0xDFFF)
#define INFINITE 0xFFFFFFFFu
#define WAIT_OBJECT_0 0u
#define WAIT_TIMEOUT 258u
#define WAIT_FAILED 0xFFFFFFFFu

#define S_OK ((HRESULT)0)
#define S_FALSE ((HRESULT)1)
#define E_FAIL ((HRESULT)0x80004005)
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#define FAILED(hr) (((HRESULT)(hr)) < 0)

#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define INVALID_FILE_ATTRIBUTES 0xFFFFFFFFu
#define INVALID_FILE_SIZE 0xFFFFFFFFu

#define WM_APP 0x8000u
#define WM_USER 0x0400u

struct FILETIME {
    DWORD dwLowDateTime, dwHighDateTime;
};
struct SYSTEMTIME {
    WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
};
union LARGE_INTEGER {
    struct {
        DWORD LowPart;
        LONG HighPart;
    };
    LONGLONG QuadPart;
};
union ULARGE_INTEGER {
    struct {
        DWORD LowPart, HighPart;
    };
    ULONGLONG QuadPart;
};
struct SECURITY_ATTRIBUTES {
    DWORD nLength;
    void* lpSecurityDescriptor;
    BOOL bInheritHandle;
};
struct OVERLAPPED {  // only the offset of a positional read or write is used (no async I/O)
    ULONG_PTR Internal, InternalHigh;
    DWORD Offset, OffsetHigh;
    HANDLE hEvent;
};

// --- files ---------------------------------------------------------------------------------
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_APPEND_DATA 0x4u  // opens for writing at the end, as O_APPEND
#define FILE_SHARE_READ 1u
#define FILE_SHARE_WRITE 2u
#define FILE_SHARE_DELETE 4u
#define CREATE_NEW 1u
#define CREATE_ALWAYS 2u
#define OPEN_EXISTING 3u
#define OPEN_ALWAYS 4u
#define TRUNCATE_EXISTING 5u
#define FILE_ATTRIBUTE_READONLY 0x1u
#define FILE_ATTRIBUTE_HIDDEN 0x2u
#define FILE_ATTRIBUTE_SYSTEM 0x4u
#define FILE_ATTRIBUTE_DIRECTORY 0x10u
#define FILE_ATTRIBUTE_NORMAL 0x80u
#define FILE_ATTRIBUTE_TEMPORARY 0x100u
#define FILE_ATTRIBUTE_REPARSE_POINT 0x400u
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000u
#define FILE_FLAG_RANDOM_ACCESS 0x10000000u
#define FILE_FLAG_WRITE_THROUGH 0x80000000u
#define FILE_FLAG_BACKUP_SEMANTICS 0x02000000u
#define FILE_FLAG_DELETE_ON_CLOSE 0x04000000u
#define FILE_BEGIN 0u
#define FILE_CURRENT 1u
#define FILE_END 2u
#define MOVEFILE_REPLACE_EXISTING 1u
#define MOVEFILE_COPY_ALLOWED 2u
#define MOVEFILE_WRITE_THROUGH 8u

#define ERROR_SUCCESS 0L
#define ERROR_FILE_NOT_FOUND 2L
#define ERROR_PATH_NOT_FOUND 3L
#define ERROR_GEN_FAILURE 31L
#define ERROR_CANCELLED 1223L
#define ERROR_TIMEOUT 1460L
#define ERROR_ACCESS_DENIED 5L
#define ERROR_INVALID_HANDLE 6L
#define ERROR_NOT_ENOUGH_MEMORY 8L
#define ERROR_WRITE_PROTECT 19L
#define ERROR_SHARING_VIOLATION 32L
#define ERROR_HANDLE_EOF 38L
#define ERROR_HANDLE_DISK_FULL 39L
#define ERROR_FILE_EXISTS 80L
#define ERROR_INVALID_PARAMETER 87L
#define ERROR_DISK_FULL 112L
#define ERROR_INSUFFICIENT_BUFFER 122L
#define ERROR_DIR_NOT_EMPTY 145L
#define ERROR_ALREADY_EXISTS 183L
#define ERROR_FILENAME_EXCED_RANGE 206L
#define ERROR_MORE_DATA 234L
#define ERROR_NO_MORE_FILES 18L
#define ERROR_BAD_PATHNAME 161L
#define ERROR_NOT_SAME_DEVICE 17L

struct WIN32_FIND_DATAW {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow;
    DWORD dwReserved0, dwReserved1;
    wchar_t cFileName[1024];
    wchar_t cAlternateFileName[14];
};
struct WIN32_FILE_ATTRIBUTE_DATA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow;
};
enum FINDEX_INFO_LEVELS { FindExInfoStandard, FindExInfoBasic };
enum FINDEX_SEARCH_OPS { FindExSearchNameMatch, FindExSearchLimitToDirectories };
enum GET_FILEEX_INFO_LEVELS { GetFileExInfoStandard };
#define FIND_FIRST_EX_LARGE_FETCH 2u

HANDLE CreateFileW(LPCWSTR path, DWORD access, DWORD share, SECURITY_ATTRIBUTES* sa, DWORD creation, DWORD flags,
                   HANDLE templ);
BOOL ReadFile(HANDLE h, void* buffer, DWORD bytes, DWORD* read, OVERLAPPED* ov);
BOOL WriteFile(HANDLE h, const void* buffer, DWORD bytes, DWORD* written, OVERLAPPED* ov);
BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER distance, LARGE_INTEGER* newPos, DWORD method);
BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER* size);
#define PAGE_READONLY 2u
#define FILE_MAP_READ 4u
HANDLE CreateFileMappingW(HANDLE file, void*, DWORD protect, DWORD high, DWORD low, LPCWSTR name);
void* MapViewOfFile(HANDLE mapping, DWORD access, DWORD high, DWORD low, size_t bytes);
BOOL UnmapViewOfFile(const void* address);
BOOL GetFileTime(HANDLE h, FILETIME* created, FILETIME* accessed, FILETIME* written);
BOOL SetEndOfFile(HANDLE h);
BOOL FlushFileBuffers(HANDLE h);
BOOL CloseHandle(HANDLE h);
DWORD GetFileAttributesW(LPCWSTR path);
BOOL GetFileAttributesExW(LPCWSTR path, GET_FILEEX_INFO_LEVELS level, void* info);
HANDLE FindFirstFileW(LPCWSTR pattern, WIN32_FIND_DATAW* data);
HANDLE FindFirstFileExW(LPCWSTR pattern, FINDEX_INFO_LEVELS level, void* data, FINDEX_SEARCH_OPS op, void* filter,
                        DWORD flags);
BOOL FindNextFileW(HANDLE h, WIN32_FIND_DATAW* data);
BOOL FindClose(HANDLE h);
BOOL MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags);
BOOL MoveFileW(LPCWSTR from, LPCWSTR to);
BOOL CopyFileW(LPCWSTR from, LPCWSTR to, BOOL failIfExists);
// The progress routine is asked between 1 MB chunks; PROGRESS_CANCEL stops the copy and removes `to`.
#define COPY_FILE_FAIL_IF_EXISTS 0x1u
#define PROGRESS_CONTINUE 0u
#define PROGRESS_CANCEL 1u
typedef DWORD (*LPPROGRESS_ROUTINE)(LARGE_INTEGER total, LARGE_INTEGER done, LARGE_INTEGER streamSize, LARGE_INTEGER streamDone,
                                    DWORD stream, DWORD reason, HANDLE from, HANDLE to, void* data);
BOOL CopyFileExW(LPCWSTR from, LPCWSTR to, LPPROGRESS_ROUTINE progress, void* data, BOOL* cancel, DWORD flags);
// FILE_ATTRIBUTE_NORMAL makes the file writable by its owner; FILE_ATTRIBUTE_READONLY takes that away.
BOOL SetFileAttributesW(LPCWSTR path, DWORD attributes);
BOOL DeleteFileW(LPCWSTR path);
#define REPLACEFILE_WRITE_THROUGH 1u
#define REPLACEFILE_IGNORE_MERGE_ERRORS 2u
// Replaces `replaced` with `replacement` (an atomic rename on Linux).
BOOL ReplaceFileW(LPCWSTR replaced, LPCWSTR replacement, LPCWSTR backup, DWORD flags, void*, void*);
BOOL CreateDirectoryW(LPCWSTR path, SECURITY_ATTRIBUTES* sa);
BOOL RemoveDirectoryW(LPCWSTR path);
int SHCreateDirectoryExW(HWND hwnd, LPCWSTR path, const SECURITY_ATTRIBUTES* sa);
DWORD GetFullPathNameW(LPCWSTR path, DWORD size, LPWSTR out, LPWSTR* filePart);
DWORD GetTempPathW(DWORD size, LPWSTR out);
BOOL PathIsRelativeW(LPCWSTR path);
int StrCmpLogicalW(LPCWSTR a, LPCWSTR b);  // natural order: "2" before "10", ignoring case
HRESULT PathCreateFromUrlW(LPCWSTR url, LPWSTR out, DWORD* size, DWORD flags);
DWORD GetModuleFileNameW(HMODULE module, LPWSTR out, DWORD size);
DWORD GetLastError();
void SetLastError(DWORD error);
#define FORMAT_MESSAGE_ALLOCATE_BUFFER 0x100u
#define FORMAT_MESSAGE_IGNORE_INSERTS 0x200u
#define FORMAT_MESSAGE_FROM_SYSTEM 0x1000u
DWORD FormatMessageW(DWORD flags, const void* source, DWORD id, DWORD lang, LPWSTR buffer, DWORD size, void* args);
void* LocalFree(void* p);

// --- text ----------------------------------------------------------------------------------
#define CP_ACP 0u
#define CP_UTF8 65001u
#define MB_ERR_INVALID_CHARS 8u
#define WC_ERR_INVALID_CHARS 0x80u
int MultiByteToWideChar(UINT codePage, DWORD flags, const char* in, int inLen, LPWSTR out, int outLen);
int WideCharToMultiByte(UINT codePage, DWORD flags, LPCWSTR in, int inLen, char* out, int outLen, const char* defChar,
                        BOOL* usedDef);
#define LOCALE_NAME_USER_DEFAULT nullptr
#define LOCALE_NAME_INVARIANT L""
#define LCMAP_LOWERCASE 0x100u
#define LCMAP_UPPERCASE 0x200u
#define LCMAP_LINGUISTIC_CASING 0x01000000u
int LCMapStringEx(LPCWSTR locale, DWORD flags, LPCWSTR in, int inLen, LPWSTR out, int outLen, void*, void*, LPARAM);
#define CSTR_LESS_THAN 1
#define CSTR_EQUAL 2
#define CSTR_GREATER_THAN 3
int CompareStringOrdinal(LPCWSTR a, int aLen, LPCWSTR b, int bLen, BOOL ignoreCase);
#define NORM_IGNORECASE 1u
#define SORT_DIGITSASNUMBERS 8u
#define LINGUISTIC_IGNORECASE 0x10u
int CompareStringEx(LPCWSTR locale, DWORD flags, LPCWSTR a, int aLen, LPCWSTR b, int bLen, void*, void*, LPARAM);
#define FIND_FROMSTART 0x00400000u
int FindNLSStringEx(LPCWSTR locale, DWORD flags, LPCWSTR text, int length, LPCWSTR value, int valueLength,
                    int* foundLength, void*, void*, LPARAM);

inline DWORD CharLowerBuffW(LPWSTR text, DWORD length) {
    for (DWORD i = 0; i < length; ++i) text[i] = (wchar_t)towlower(text[i]);
    return length;
}
inline int _wcsicmp(const wchar_t* a, const wchar_t* b) { return wcscasecmp(a, b); }
inline int _wcsnicmp(const wchar_t* a, const wchar_t* b, size_t n) { return wcsncasecmp(a, b, n); }
inline int _stricmp(const char* a, const char* b) { return strcasecmp(a, b); }
inline int _strnicmp(const char* a, const char* b, size_t n) { return strncasecmp(a, b, n); }
inline int lstrcmpiW(const wchar_t* a, const wchar_t* b) { return wcscasecmp(a, b); }
inline int _wtoi(const wchar_t* s) { return (int)wcstol(s, nullptr, 10); }
inline long long _wtoi64(const wchar_t* s) { return wcstoll(s, nullptr, 10); }
inline double _wtof(const wchar_t* s) { return wcstod(s, nullptr); }
#define _TRUNCATE ((size_t)-1)
template <size_t N>
int wcscpy_s(wchar_t (&dst)[N], const wchar_t* src) {
    wcsncpy(dst, src, N - 1);
    dst[N - 1] = 0;
    return 0;
}
inline int wcscpy_s(wchar_t* dst, size_t n, const wchar_t* src) {
    if (!n) return 0;
    wcsncpy(dst, src, n - 1);
    dst[n - 1] = 0;
    return 0;
}
template <size_t N>
int wcsncpy_s(wchar_t (&dst)[N], const wchar_t* src, size_t count) {
    size_t n = count == _TRUNCATE || count >= N ? N - 1 : count;
    wcsncpy(dst, src, n);
    dst[n] = 0;
    return 0;
}
template <size_t N>
int strcpy_s(char (&dst)[N], const char* src) {
    strncpy(dst, src, N - 1);
    dst[N - 1] = 0;
    return 0;
}
template <size_t N, class... A>
int swprintf_s(wchar_t (&dst)[N], const wchar_t* fmt, A... args) {
    return swprintf(dst, N, fmt, args...);
}
template <class... A>
int swprintf_s(wchar_t* dst, size_t n, const wchar_t* fmt, A... args) {
    return swprintf(dst, n, fmt, args...);
}
template <size_t N, class... A>
int sprintf_s(char (&dst)[N], const char* fmt, A... args) {
    return snprintf(dst, N, fmt, args...);
}
#define sscanf_s sscanf
#define swscanf_s swscanf
inline int _wfopen_s(FILE** f, const wchar_t* path, const wchar_t* mode) {
    *f = fopen(compat::Narrow(path).c_str(), compat::Narrow(mode).c_str());
    return *f ? 0 : errno;
}
#define _countof(a) (sizeof(a) / sizeof((a)[0]))

// --- events, waits, time -------------------------------------------------------------------
HANDLE CreateEventW(SECURITY_ATTRIBUTES* sa, BOOL manualReset, BOOL initialState, LPCWSTR name);
BOOL SetEvent(HANDLE h);
BOOL ResetEvent(HANDLE h);
DWORD WaitForSingleObject(HANDLE h, DWORD ms);
DWORD WaitForMultipleObjects(DWORD count, const HANDLE* handles, BOOL waitAll, DWORD ms);
void Sleep(DWORD ms);
ULONGLONG GetTickCount64();
DWORD GetTickCount();
void GetSystemTimeAsFileTime(FILETIME* ft);
void GetLocalTime(SYSTEMTIME* st);
void GetSystemTime(SYSTEMTIME* st);
BOOL FileTimeToSystemTime(const FILETIME* ft, SYSTEMTIME* st);
BOOL SystemTimeToFileTime(const SYSTEMTIME* st, FILETIME* ft);
BOOL FileTimeToLocalFileTime(const FILETIME* ft, FILETIME* local);
inline LONG CompareFileTime(const FILETIME* a, const FILETIME* b) {
    uint64_t x = ((uint64_t)a->dwHighDateTime << 32) | a->dwLowDateTime, y = ((uint64_t)b->dwHighDateTime << 32) | b->dwLowDateTime;
    return x < y ? -1 : x > y ? 1 : 0;
}
BOOL SystemTimeToTzSpecificLocalTime(const void* zone, const SYSTEMTIME* utc, SYSTEMTIME* local);
BOOL QueryPerformanceCounter(LARGE_INTEGER* count);
BOOL QueryPerformanceFrequency(LARGE_INTEGER* freq);
using __time64_t = int64_t;
inline int _localtime64_s(struct tm* out, const __time64_t* t) {
    time_t v = (time_t)*t;
    return localtime_r(&v, out) ? 0 : 1;
}
inline int _gmtime64_s(struct tm* out, const __time64_t* t) {
    time_t v = (time_t)*t;
    return gmtime_r(&v, out) ? 0 : 1;
}
inline __time64_t _time64(__time64_t* t) {
    __time64_t v = (__time64_t)time(nullptr);
    if (t) *t = v;
    return v;
}
inline __time64_t _mkgmtime64(struct tm* t) { return (__time64_t)timegm(t); }
DWORD GetCurrentProcessId();
DWORD GetCurrentThreadId();
HANDLE GetCurrentThread();
#define THREAD_PRIORITY_LOWEST (-2)
#define THREAD_PRIORITY_BELOW_NORMAL (-1)
#define THREAD_PRIORITY_NORMAL 0
#define THREAD_PRIORITY_ABOVE_NORMAL 1
#define THREAD_PRIORITY_HIGHEST 2
#define THREAD_PRIORITY_TIME_CRITICAL 15
#define THREAD_MODE_BACKGROUND_BEGIN 0x10000
#define THREAD_MODE_BACKGROUND_END 0x20000
BOOL SetThreadPriority(HANDLE thread, int priority);
DWORD GetEnvironmentVariableW(LPCWSTR name, LPWSTR out, DWORD size);
inline long InterlockedIncrement(volatile long* v) { return __atomic_add_fetch(v, 1, __ATOMIC_SEQ_CST); }
inline long InterlockedDecrement(volatile long* v) { return __atomic_sub_fetch(v, 1, __ATOMIC_SEQ_CST); }

// --- COM (nothing to start on Linux) -----------------------------------------------------------
#define COINIT_MULTITHREADED 0u
#define COINIT_APARTMENTTHREADED 2u
#define COINIT_DISABLE_OLE1DDE 4u
inline HRESULT CoInitializeEx(void*, DWORD) { return S_OK; }
inline void CoUninitialize() {}
inline void CoTaskMemFree(void* p) { free(p); }

// --- messages to the front end -------------------------------------------------------------
BOOL PostMessageW(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
// The front end receives everything PostMessageW sends, on whichever thread posted it.
void SetPostMessageHandler(std::function<void(HWND, UINT, WPARAM, LPARAM)> handler);

