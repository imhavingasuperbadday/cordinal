// POSIX implementation of compat/windows.h (see there).
#include <windows.h>

#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <langinfo.h>
#include <locale.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
#include <unordered_map>

namespace {

// --- handles ----------------------------------------------------------------------------------
struct Object {
    enum Kind { File, Event, Find, Mapping } kind;
    explicit Object(Kind k) : kind(k) {}
    virtual ~Object() = default;
};
struct FileObject : Object {
    int fd = -1;
    std::string deleteOnClose;
    FileObject() : Object(File) {}
};
struct MappingObject : Object {
    int fd = -1;
    explicit MappingObject(int file) : Object(Mapping), fd(file) {}
    ~MappingObject() override { if (fd >= 0) close(fd); }
};
std::mutex g_mappingMx;
std::unordered_map<const void*, size_t> g_mappings;
struct EventObject : Object {
    bool set = false, manual = false;
    EventObject() : Object(Event) {}
};
struct FindObject : Object {
    DIR* dir = nullptr;
    std::string path, pattern;  // the directory, and the name pattern (UTF-8)
    FindObject() : Object(Find) {}
};

bool Valid(HANDLE h) { return h && h != INVALID_HANDLE_VALUE; }
template <class T>
T* As(HANDLE h, Object::Kind kind) {
    if (!Valid(h)) return nullptr;
    auto* o = static_cast<Object*>(h);
    return o->kind == kind ? static_cast<T*>(o) : nullptr;
}

// --- errors ----------------------------------------------------------------------------------
thread_local DWORD t_lastError = 0;
thread_local int t_lastErrno = 0;

DWORD FromErrno(int e) {
    switch (e) {
        case 0: return ERROR_SUCCESS;
        case ENOENT: return ERROR_FILE_NOT_FOUND;
        case ENOTDIR: return ERROR_PATH_NOT_FOUND;
        case EACCES: case EPERM: return ERROR_ACCESS_DENIED;
        case EROFS: return ERROR_WRITE_PROTECT;
        case EEXIST: return ERROR_ALREADY_EXISTS;
        case ENOSPC: return ERROR_DISK_FULL;
        case ENAMETOOLONG: return ERROR_FILENAME_EXCED_RANGE;
        case ENOTEMPTY: return ERROR_DIR_NOT_EMPTY;
        case EXDEV: return ERROR_NOT_SAME_DEVICE;
        case EBADF: return ERROR_INVALID_HANDLE;
        case ENOMEM: return ERROR_NOT_ENOUGH_MEMORY;
        case EBUSY: case ETXTBSY: return ERROR_SHARING_VIOLATION;
        default: return 0x20000000u | (DWORD)e;  // "customer" bit: an errno with no Windows twin
    }
}
BOOL Fail(int e = errno) {
    t_lastErrno = e;
    t_lastError = FromErrno(e);
    return FALSE;
}
BOOL Ok() {
    t_lastError = 0;
    return TRUE;
}

// --- text -------------------------------------------------------------------------------------
locale_t Utf8Locale() {
    static locale_t loc = [] {
        locale_t l = newlocale(LC_CTYPE_MASK, "", (locale_t)0);
        if (l) {
            const char* name = nl_langinfo_l(CODESET, l);
            if (name && strcmp(name, "UTF-8") == 0) return l;
            freelocale(l);
        }
        l = newlocale(LC_CTYPE_MASK, "C.UTF-8", (locale_t)0);
        return l ? l : newlocale(LC_CTYPE_MASK, "C", (locale_t)0);
    }();
    return loc;
}
wchar_t Lower(wchar_t c) { return (wchar_t)towlower_l((wint_t)c, Utf8Locale()); }
wchar_t Upper(wchar_t c) { return (wchar_t)towupper_l((wint_t)c, Utf8Locale()); }

// Windows-1252, the code page cordinal's ANSI fallbacks (old CUE sheets, M3U files) expect.
const char16_t k1252[32] = {0x20AC, 0x81,   0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                            0x2039, 0x0152, 0x8D,   0x017D, 0x8F,   0x90,   0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                            0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D,   0x017E, 0x0178};

bool DecodeUtf8(const unsigned char* s, size_t n, size_t& i, char32_t& out) {
    unsigned char c = s[i];
    int extra = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
    if (extra < 0 || i + extra >= n + (extra == 0)) {
        ++i;
        return false;
    }
    char32_t v = extra == 0 ? c : extra == 1 ? (c & 0x1F) : extra == 2 ? (c & 0x0F) : (c & 0x07);
    for (int k = 1; k <= extra; ++k) {
        if (i + k >= n || (s[i + k] & 0xC0) != 0x80) {
            ++i;
            return false;
        }
        v = (v << 6) | (s[i + k] & 0x3F);
    }
    static const char32_t kMin[] = {0, 0x80, 0x800, 0x10000};
    i += extra + 1;
    if (v < kMin[extra] || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) return false;
    out = v;
    return true;
}

void AppendUtf8(std::string& out, char32_t c) {
    if (c < 0x80) out += (char)c;
    else if (c < 0x800) out += {(char)(0xC0 | (c >> 6)), (char)(0x80 | (c & 0x3F))};
    else if (c < 0x10000)
        out += {(char)(0xE0 | (c >> 12)), (char)(0x80 | ((c >> 6) & 0x3F)), (char)(0x80 | (c & 0x3F))};
    else
        out += {(char)(0xF0 | (c >> 18)), (char)(0x80 | ((c >> 12) & 0x3F)), (char)(0x80 | ((c >> 6) & 0x3F)),
                (char)(0x80 | (c & 0x3F))};
}

std::string ToPath(LPCWSTR path) { return compat::Narrow(path ? path : L""); }

// --- time -------------------------------------------------------------------------------------
constexpr uint64_t kEpochDiff = 11644473600ull;  // seconds from 1601 (FILETIME) to 1970 (Unix)
uint64_t ToU64(const FILETIME& ft) { return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime; }
FILETIME FromU64(uint64_t v) { return {(DWORD)v, (DWORD)(v >> 32)}; }
FILETIME ModTime(const struct stat& st) { return FromU64(compat::UnixToFileTime(st.st_mtim.tv_sec, st.st_mtim.tv_nsec)); }

void TmToSystemTime(const tm& t, int ms, SYSTEMTIME* st) {
    *st = {(WORD)(t.tm_year + 1900), (WORD)(t.tm_mon + 1), (WORD)t.tm_wday, (WORD)t.tm_mday,
           (WORD)t.tm_hour,          (WORD)t.tm_min,       (WORD)t.tm_sec,  (WORD)ms};
}

DWORD Attributes(const std::string& path, const std::string& name) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return INVALID_FILE_ATTRIBUTES;
    DWORD a = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : 0;
    struct stat lst;
    if (lstat(path.c_str(), &lst) == 0 && S_ISLNK(lst.st_mode)) a |= FILE_ATTRIBUTE_REPARSE_POINT;
    if (!name.empty() && name[0] == '.' && name != "." && name != "..") a |= FILE_ATTRIBUTE_HIDDEN;
    if (!(st.st_mode & 0222)) a |= FILE_ATTRIBUTE_READONLY;
    return a ? a : FILE_ATTRIBUTE_NORMAL;
}

std::string BaseName(const std::string& path) {
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool FillFind(FindObject* f, WIN32_FIND_DATAW* data) {
    while (dirent* e = readdir(f->dir)) {
        if (fnmatch(f->pattern.c_str(), e->d_name, FNM_CASEFOLD) != 0) continue;
        std::string full = f->path + "/" + e->d_name;
        struct stat st;
        if (stat(full.c_str(), &st) != 0 && lstat(full.c_str(), &st) != 0) continue;
        *data = {};
        data->dwFileAttributes = Attributes(full, e->d_name);
        if (data->dwFileAttributes == INVALID_FILE_ATTRIBUTES) data->dwFileAttributes = FILE_ATTRIBUTE_REPARSE_POINT;
        data->ftLastWriteTime = data->ftCreationTime = data->ftLastAccessTime = ModTime(st);
        data->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32);
        data->nFileSizeLow = (DWORD)st.st_size;
        std::wstring name = compat::Widen(e->d_name);
        wcsncpy(data->cFileName, name.c_str(), std::size(data->cFileName) - 1);
        return true;
    }
    return false;
}

// --- events -------------------------------------------------------------------------------------
std::mutex g_eventMx;  // one lock and condition for every event keeps multi-waits simple
std::condition_variable g_eventCv;

bool Signaled(HANDLE h) {
    auto* e = As<EventObject>(h, Object::Event);
    return e ? e->set : true;  // anything else counts as signaled (no waits on files)
}
void Consume(HANDLE h) {
    if (auto* e = As<EventObject>(h, Object::Event); e && !e->manual) e->set = false;
}

std::function<void(HWND, UINT, WPARAM, LPARAM)> g_post;
std::mutex g_postMx;

}  // namespace

// --- compat helpers ---------------------------------------------------------------------------------
namespace compat {
std::string Narrow(const wchar_t* text) {
    std::string out;
    for (; *text; ++text) AppendUtf8(out, (char32_t)*text);
    return out;
}
std::wstring Widen(const char* text, size_t len) {
    if (len == (size_t)-1) len = strlen(text);
    std::wstring out;
    out.reserve(len);
    auto* s = reinterpret_cast<const unsigned char*>(text);
    for (size_t i = 0; i < len;) {
        char32_t c = 0xFFFD;
        DecodeUtf8(s, len, i, c);
        out += (wchar_t)c;
    }
    return out;
}
uint64_t UnixToFileTime(int64_t seconds, long nanoseconds) {
    return (uint64_t)(seconds + (int64_t)kEpochDiff) * 10000000ull + (uint64_t)nanoseconds / 100;
}
}  // namespace compat

// --- files ------------------------------------------------------------------------------------
HANDLE CreateFileW(LPCWSTR path, DWORD access, DWORD, SECURITY_ATTRIBUTES*, DWORD creation, DWORD flags, HANDLE) {
    std::string p = ToPath(path);
    int mode = (access & GENERIC_WRITE) ? ((access & GENERIC_READ) ? O_RDWR : O_WRONLY) : O_RDONLY;
    if (!(access & GENERIC_WRITE) && (access & FILE_APPEND_DATA)) mode = O_WRONLY | O_APPEND;
    switch (creation) {
        case CREATE_NEW: mode |= O_CREAT | O_EXCL; break;
        case CREATE_ALWAYS: mode |= O_CREAT | O_TRUNC; break;
        case OPEN_ALWAYS: mode |= O_CREAT; break;
        case TRUNCATE_EXISTING: mode |= O_TRUNC; break;
        default: break;
    }
    int fd = open(p.c_str(), mode | O_CLOEXEC, 0644);
    if (fd < 0) {
        Fail();
        return INVALID_HANDLE_VALUE;
    }
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode) && !(flags & FILE_FLAG_BACKUP_SEMANTICS)) {
        close(fd);
        Fail(EACCES);
        return INVALID_HANDLE_VALUE;
    }
    if (flags & FILE_FLAG_SEQUENTIAL_SCAN) posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
    auto* f = new FileObject;
    f->fd = fd;
    if (flags & FILE_FLAG_DELETE_ON_CLOSE) f->deleteOnClose = p;
    Ok();
    return f;
}

BOOL ReadFile(HANDLE h, void* buffer, DWORD bytes, DWORD* read, OVERLAPPED* ov) {
    auto* f = As<FileObject>(h, Object::File);
    if (read) *read = 0;
    if (!f) return Fail(EBADF);
    off_t offset = ov ? (off_t)(((uint64_t)ov->OffsetHigh << 32) | ov->Offset) : -1;
    size_t done = 0;
    while (done < bytes) {
        ssize_t n = offset >= 0 ? pread(f->fd, (char*)buffer + done, bytes - done, offset + (off_t)done)
                                : ::read(f->fd, (char*)buffer + done, bytes - done);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return Fail();
        if (n == 0) break;
        done += (size_t)n;
    }
    if (read) *read = (DWORD)done;
    return Ok();
}

BOOL WriteFile(HANDLE h, const void* buffer, DWORD bytes, DWORD* written, OVERLAPPED* ov) {
    auto* f = As<FileObject>(h, Object::File);
    if (written) *written = 0;
    if (!f) return Fail(EBADF);
    off_t offset = ov ? (off_t)(((uint64_t)ov->OffsetHigh << 32) | ov->Offset) : -1;
    size_t done = 0;
    while (done < bytes) {
        ssize_t n = offset >= 0 ? pwrite(f->fd, (const char*)buffer + done, bytes - done, offset + (off_t)done)
                                : ::write(f->fd, (const char*)buffer + done, bytes - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return Fail(n < 0 ? errno : ENOSPC);
        done += (size_t)n;
    }
    if (written) *written = (DWORD)done;
    return Ok();
}

BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER distance, LARGE_INTEGER* newPos, DWORD method) {
    auto* f = As<FileObject>(h, Object::File);
    if (!f) return Fail(EBADF);
    int whence = method == FILE_END ? SEEK_END : method == FILE_CURRENT ? SEEK_CUR : SEEK_SET;
    off_t r = lseek(f->fd, (off_t)distance.QuadPart, whence);
    if (r < 0) return Fail();
    if (newPos) newPos->QuadPart = r;
    return Ok();
}

BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER* size) {
    auto* f = As<FileObject>(h, Object::File);
    struct stat st;
    if (!f || fstat(f->fd, &st) != 0) return Fail(f ? errno : EBADF);
    size->QuadPart = st.st_size;
    return Ok();
}

BOOL GetFileTime(HANDLE h, FILETIME* created, FILETIME* accessed, FILETIME* written) {
    auto* f = As<FileObject>(h, Object::File);
    struct stat st;
    if (!f || fstat(f->fd, &st) != 0) return Fail(f ? errno : EBADF);
    FILETIME t = ModTime(st);
    if (created) *created = t;
    if (accessed) *accessed = t;
    if (written) *written = t;
    return Ok();
}

BOOL SetEndOfFile(HANDLE h) {
    auto* f = As<FileObject>(h, Object::File);
    if (!f) return Fail(EBADF);
    off_t pos = lseek(f->fd, 0, SEEK_CUR);
    return pos >= 0 && ftruncate(f->fd, pos) == 0 ? Ok() : Fail();
}

BOOL FlushFileBuffers(HANDLE h) {
    auto* f = As<FileObject>(h, Object::File);
    return f && fsync(f->fd) == 0 ? Ok() : Fail(f ? errno : EBADF);
}

HANDLE CreateFileMappingW(HANDLE file, void*, DWORD protect, DWORD high, DWORD low, LPCWSTR name) {
    auto* source = As<FileObject>(file, Object::File);
    if (!source || protect != PAGE_READONLY || high || low || name) { Fail(EINVAL); return nullptr; }
    int fd = dup(source->fd);
    if (fd < 0) { Fail(); return nullptr; }
    return new MappingObject(fd);
}
void* MapViewOfFile(HANDLE handle, DWORD access, DWORD high, DWORD low, size_t bytes) {
    auto* mapping = As<MappingObject>(handle, Object::Mapping);
    if (!mapping || access != FILE_MAP_READ || !bytes) { Fail(EINVAL); return nullptr; }
    void* view = mmap(nullptr, bytes, PROT_READ, MAP_SHARED, mapping->fd, (off_t)(((uint64_t)high << 32) | low));
    if (view == MAP_FAILED) { Fail(); return nullptr; }
    std::lock_guard lock(g_mappingMx); g_mappings.emplace(view, bytes);
    return view;
}
BOOL UnmapViewOfFile(const void* address) {
    std::lock_guard lock(g_mappingMx);
    auto it = g_mappings.find(address);
    if (it == g_mappings.end()) return Fail(EINVAL);
    if (munmap((void*)address, it->second)) return Fail();
    g_mappings.erase(it); return Ok();
}
BOOL CloseHandle(HANDLE h) {
    if (!Valid(h) || h == GetCurrentThread()) return Fail(EBADF);
    auto* o = static_cast<Object*>(h);
    if (o->kind == Object::File) {
        auto* f = static_cast<FileObject*>(o);
        close(f->fd);
        if (!f->deleteOnClose.empty()) unlink(f->deleteOnClose.c_str());
    } else if (o->kind == Object::Find) {
        closedir(static_cast<FindObject*>(o)->dir);
    }
    delete o;
    return Ok();
}

DWORD GetFileAttributesW(LPCWSTR path) {
    std::string p = ToPath(path);
    DWORD a = Attributes(p, BaseName(p));
    if (a == INVALID_FILE_ATTRIBUTES) Fail();
    return a;
}

BOOL GetFileAttributesExW(LPCWSTR path, GET_FILEEX_INFO_LEVELS, void* info) {
    std::string p = ToPath(path);
    struct stat st;
    if (stat(p.c_str(), &st) != 0) return Fail();
    auto* d = static_cast<WIN32_FILE_ATTRIBUTE_DATA*>(info);
    d->dwFileAttributes = Attributes(p, BaseName(p));
    d->ftCreationTime = d->ftLastAccessTime = d->ftLastWriteTime = ModTime(st);
    d->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32);
    d->nFileSizeLow = (DWORD)st.st_size;
    return Ok();
}

HANDLE FindFirstFileW(LPCWSTR pattern, WIN32_FIND_DATAW* data) {
    std::string p = ToPath(pattern);
    size_t slash = p.find_last_of("/\\");
    auto* f = new FindObject;
    f->path = slash == std::string::npos ? "." : slash == 0 ? "/" : p.substr(0, slash);
    f->pattern = slash == std::string::npos ? p : p.substr(slash + 1);
    f->dir = opendir(f->path.c_str());
    if (!f->dir) {
        Fail();
        delete f;
        return INVALID_HANDLE_VALUE;
    }
    if (!FillFind(f, data)) {
        closedir(f->dir);
        delete f;
        Fail(ENOENT);
        return INVALID_HANDLE_VALUE;
    }
    Ok();
    return f;
}

HANDLE FindFirstFileExW(LPCWSTR pattern, FINDEX_INFO_LEVELS, void* data, FINDEX_SEARCH_OPS, void*, DWORD) {
    return FindFirstFileW(pattern, static_cast<WIN32_FIND_DATAW*>(data));
}

BOOL FindNextFileW(HANDLE h, WIN32_FIND_DATAW* data) {
    auto* f = As<FindObject>(h, Object::Find);
    if (!f) return Fail(EBADF);
    if (FillFind(f, data)) return Ok();
    t_lastError = ERROR_NO_MORE_FILES;
    return FALSE;
}

BOOL FindClose(HANDLE h) { return CloseHandle(h); }

static bool CopyBytes(const std::string& from, const std::string& to, bool failIfExists,
                      const std::function<bool()>& keepGoing = {}) {
    int in = open(from.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) return false;
    struct stat st;
    fstat(in, &st);
    int out = open(to.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC | (failIfExists ? O_EXCL : O_TRUNC), st.st_mode & 0777);
    if (out < 0) {
        int e = errno;
        close(in);
        errno = e;
        return false;
    }
    std::vector<char> buf(1 << 20);
    bool ok = true;
    for (ssize_t n; (n = read(in, buf.data(), buf.size())) != 0;) {
        if (n < 0 && errno == EINTR) continue;
        if (keepGoing && !keepGoing()) {
            ok = false;
            errno = ECANCELED;
            break;
        }
        if (n < 0 || write(out, buf.data(), (size_t)n) != n) {
            ok = false;
            break;
        }
    }
    int e = errno;
    close(in);
    if (close(out) != 0) ok = false;
    if (!ok) unlink(to.c_str());
    errno = e;
    return ok;
}

BOOL MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags) {
    std::string a = ToPath(from), b = ToPath(to);
    struct stat st;
    if (!(flags & MOVEFILE_REPLACE_EXISTING) && lstat(b.c_str(), &st) == 0) return Fail(EEXIST);
    if (rename(a.c_str(), b.c_str()) == 0) return Ok();
    if (errno != EXDEV || !(flags & MOVEFILE_COPY_ALLOWED)) return Fail();
    if (!CopyBytes(a, b, false)) return Fail();
    unlink(a.c_str());
    return Ok();
}

BOOL MoveFileW(LPCWSTR from, LPCWSTR to) { return MoveFileExW(from, to, MOVEFILE_COPY_ALLOWED); }

BOOL CopyFileW(LPCWSTR from, LPCWSTR to, BOOL failIfExists) {
    return CopyBytes(ToPath(from), ToPath(to), failIfExists) ? Ok() : Fail();
}

BOOL CopyFileExW(LPCWSTR from, LPCWSTR to, LPPROGRESS_ROUTINE progress, void* data, BOOL* cancel, DWORD flags) {
    auto keepGoing = [&] {
        if (cancel && *cancel) return false;
        return !progress || progress({}, {}, {}, {}, 0, 0, nullptr, nullptr, data) != PROGRESS_CANCEL;
    };
    return CopyBytes(ToPath(from), ToPath(to), (flags & COPY_FILE_FAIL_IF_EXISTS) != 0, keepGoing) ? Ok() : Fail();
}

BOOL SetFileAttributesW(LPCWSTR path, DWORD attributes) {
    std::string p = ToPath(path);
    struct stat st;
    if (stat(p.c_str(), &st) != 0) return Fail();
    mode_t mode = attributes & FILE_ATTRIBUTE_READONLY ? st.st_mode & ~(mode_t)0222 : st.st_mode | S_IWUSR;
    return chmod(p.c_str(), mode & 07777) == 0 ? Ok() : Fail();
}

BOOL DeleteFileW(LPCWSTR path) { return unlink(ToPath(path).c_str()) == 0 ? Ok() : Fail(); }

BOOL ReplaceFileW(LPCWSTR replaced, LPCWSTR replacement, LPCWSTR backup, DWORD, void*, void*) {
    std::string a = ToPath(replaced), b = ToPath(replacement);
    struct stat st;
    if (stat(a.c_str(), &st) == 0) {
        chmod(b.c_str(), st.st_mode & 07777);  // keep the original's permissions
        if (backup) {  // the original becomes the backup (a hard link, so `replaced` never goes missing)
            std::string c = ToPath(backup);
            unlink(c.c_str());
            if (link(a.c_str(), c.c_str()) != 0 && !CopyBytes(a, c, false)) return Fail();
        }
    }
    return rename(b.c_str(), a.c_str()) == 0 ? Ok() : Fail();
}

BOOL CreateDirectoryW(LPCWSTR path, SECURITY_ATTRIBUTES*) { return mkdir(ToPath(path).c_str(), 0755) == 0 ? Ok() : Fail(); }

BOOL RemoveDirectoryW(LPCWSTR path) { return rmdir(ToPath(path).c_str()) == 0 ? Ok() : Fail(); }

int SHCreateDirectoryExW(HWND, LPCWSTR path, const SECURITY_ATTRIBUTES*) {
    std::string p = ToPath(path);
    struct stat st;
    if (stat(p.c_str(), &st) == 0) return S_ISDIR(st.st_mode) ? ERROR_ALREADY_EXISTS : ERROR_FILE_EXISTS;
    for (size_t i = 1; i <= p.size(); ++i) {
        if (i < p.size() && p[i] != '/') continue;
        std::string part = p.substr(0, i);
        if (mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) {
            Fail();
            return (int)t_lastError;
        }
    }
    return ERROR_SUCCESS;
}

DWORD GetFullPathNameW(LPCWSTR path, DWORD size, LPWSTR out, LPWSTR* filePart) {
    std::wstring p = path ? path : L"";
    if (p.empty() || p[0] != L'/') {
        char cwd[4096];
        if (!getcwd(cwd, sizeof(cwd))) return Fail(), 0;
        p = compat::Widen(cwd) + L"/" + p;
    }
    std::vector<std::wstring> parts;  // resolve "." and ".." like Windows does (without following links)
    for (size_t i = 0; i <= p.size();) {
        size_t j = p.find(L'/', i);
        if (j == std::wstring::npos) j = p.size();
        std::wstring part = p.substr(i, j - i);
        if (part == L"..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!part.empty() && part != L".") {
            parts.push_back(part);
        }
        i = j + 1;
    }
    std::wstring full;
    for (auto& part : parts) full += L"/" + part;
    if (full.empty()) full = L"/";
    if (!p.empty() && p.back() == L'/' && full != L"/") full += L'/';
    if (full.size() + 1 > size) return (DWORD)full.size() + 1;
    wcscpy(out, full.c_str());
    if (filePart) {
        size_t slash = full.find_last_of(L'/');
        *filePart = slash + 1 < full.size() ? out + slash + 1 : nullptr;
    }
    Ok();
    return (DWORD)full.size();
}

DWORD GetTempPathW(DWORD size, LPWSTR out) {
    const char* tmp = getenv("TMPDIR");
    std::wstring p = compat::Widen(tmp && *tmp ? tmp : "/tmp");
    if (p.back() != L'/') p += L'/';
    if (p.size() + 1 > size) return (DWORD)p.size() + 1;
    wcscpy(out, p.c_str());
    return (DWORD)p.size();
}

BOOL PathIsRelativeW(LPCWSTR path) { return !path || path[0] != L'/'; }

HRESULT PathCreateFromUrlW(LPCWSTR url, LPWSTR out, DWORD* size, DWORD) {
    std::string u = ToPath(url);
    if (u.rfind("file://", 0) != 0) return E_FAIL;
    u = u.substr(7);
    if (u.rfind("localhost/", 0) == 0) u = u.substr(9);
    std::string decoded;
    for (size_t i = 0; i < u.size(); ++i) {
        if (u[i] == '%' && i + 2 < u.size() && isxdigit((unsigned char)u[i + 1]) && isxdigit((unsigned char)u[i + 2])) {
            decoded += (char)strtol(u.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else {
            decoded += u[i];
        }
    }
    std::wstring w = compat::Widen(decoded.c_str());
    if (w.size() + 1 > *size) {
        *size = (DWORD)w.size() + 1;
        return E_FAIL;
    }
    wcscpy(out, w.c_str());
    *size = (DWORD)w.size();
    return S_OK;
}

int StrCmpLogicalW(LPCWSTR a, LPCWSTR b) {
    while (*a && *b) {
        if (iswdigit((wint_t)*a) && iswdigit((wint_t)*b)) {
            while (*a == L'0') ++a;
            while (*b == L'0') ++b;
            const wchar_t *ea = a, *eb = b;
            while (iswdigit((wint_t)*ea)) ++ea;
            while (iswdigit((wint_t)*eb)) ++eb;
            if (ea - a != eb - b) return ea - a < eb - b ? -1 : 1;
            for (; a < ea; ++a, ++b)
                if (*a != *b) return *a < *b ? -1 : 1;
            continue;
        }
        wchar_t x = Lower(*a), y = Lower(*b);
        if (x != y) return x < y ? -1 : 1;
        ++a;
        ++b;
    }
    return *a ? 1 : *b ? -1 : 0;
}

DWORD GetModuleFileNameW(HMODULE, LPWSTR out, DWORD size) {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return Fail(), 0;
    std::wstring w = compat::Widen(buf, (size_t)n);
    if (!size) return 0;
    wcsncpy(out, w.c_str(), size - 1);
    out[size - 1] = 0;
    return (DWORD)std::min<size_t>(w.size(), size - 1);
}

DWORD GetLastError() { return t_lastError; }
void SetLastError(DWORD error) { t_lastError = error; }

DWORD FormatMessageW(DWORD flags, const void*, DWORD id, DWORD, LPWSTR buffer, DWORD size, void*) {
    int e = (id & 0x20000000u) ? (int)(id & ~0x20000000u) : id == t_lastError ? t_lastErrno : 0;
    std::wstring text = e ? compat::Widen(strerror(e)) : L"Error " + std::to_wstring(id);
    if (flags & FORMAT_MESSAGE_ALLOCATE_BUFFER) {
        auto* p = static_cast<wchar_t*>(malloc((text.size() + 1) * sizeof(wchar_t)));
        wcscpy(p, text.c_str());
        *reinterpret_cast<wchar_t**>(buffer) = p;
        return (DWORD)text.size();
    }
    if (!size) return 0;
    wcsncpy(buffer, text.c_str(), size - 1);
    buffer[size - 1] = 0;
    return (DWORD)std::min<size_t>(text.size(), size - 1);
}

void* LocalFree(void* p) {
    free(p);
    return nullptr;
}

// --- text -----------------------------------------------------------------------------------------
int MultiByteToWideChar(UINT codePage, DWORD flags, const char* in, int inLen, LPWSTR out, int outLen) {
    size_t n = inLen < 0 ? strlen(in) + 1 : (size_t)inLen;
    auto* s = reinterpret_cast<const unsigned char*>(in);
    std::wstring w;
    w.reserve(n);
    if (codePage == CP_UTF8) {
        for (size_t i = 0; i < n;) {
            char32_t c = 0xFFFD;
            if (!DecodeUtf8(s, n, i, c) && (flags & MB_ERR_INVALID_CHARS)) {
                t_lastError = 1113;  // ERROR_NO_UNICODE_TRANSLATION
                return 0;
            }
            w += (wchar_t)c;
        }
    } else {
        for (size_t i = 0; i < n; ++i) w += s[i] >= 0x80 && s[i] < 0xA0 ? (wchar_t)k1252[s[i] - 0x80] : (wchar_t)s[i];
    }
    if (outLen == 0) return (int)w.size();
    if ((int)w.size() > outLen) {
        t_lastError = ERROR_INSUFFICIENT_BUFFER;
        return 0;
    }
    std::copy(w.begin(), w.end(), out);
    return (int)w.size();
}

int WideCharToMultiByte(UINT codePage, DWORD, LPCWSTR in, int inLen, char* out, int outLen, const char*, BOOL* usedDef) {
    size_t n = inLen < 0 ? wcslen(in) + 1 : (size_t)inLen;
    std::string s;
    s.reserve(n);
    if (usedDef) *usedDef = FALSE;
    for (size_t i = 0; i < n; ++i) {
        char32_t c = (char32_t)in[i];
        if (codePage == CP_UTF8) {
            AppendUtf8(s, c);
            continue;
        }
        if (c < 0x80 || (c >= 0xA0 && c < 0x100)) {
            s += (char)c;
            continue;
        }
        auto it = std::find(std::begin(k1252), std::end(k1252), (char16_t)c);
        if (it != std::end(k1252)) {
            s += (char)(0x80 + (it - std::begin(k1252)));
        } else {
            s += '?';
            if (usedDef) *usedDef = TRUE;
        }
    }
    if (outLen == 0) return (int)s.size();
    if ((int)s.size() > outLen) {
        t_lastError = ERROR_INSUFFICIENT_BUFFER;
        return 0;
    }
    memcpy(out, s.data(), s.size());
    return (int)s.size();
}

int LCMapStringEx(LPCWSTR, DWORD flags, LPCWSTR in, int inLen, LPWSTR out, int outLen, void*, void*, LPARAM) {
    size_t n = inLen < 0 ? wcslen(in) + 1 : (size_t)inLen;
    if (outLen == 0) return (int)n;
    if ((int)n > outLen) return 0;
    for (size_t i = 0; i < n; ++i)
        out[i] = (flags & LCMAP_LOWERCASE) ? Lower(in[i]) : (flags & LCMAP_UPPERCASE) ? Upper(in[i]) : in[i];
    return (int)n;
}

int CompareStringOrdinal(LPCWSTR a, int aLen, LPCWSTR b, int bLen, BOOL ignoreCase) {
    size_t na = aLen < 0 ? wcslen(a) : (size_t)aLen, nb = bLen < 0 ? wcslen(b) : (size_t)bLen;
    for (size_t i = 0; i < na && i < nb; ++i) {
        wchar_t x = ignoreCase ? Upper(a[i]) : a[i], y = ignoreCase ? Upper(b[i]) : b[i];
        if (x != y) return x < y ? CSTR_LESS_THAN : CSTR_GREATER_THAN;
    }
    return na == nb ? CSTR_EQUAL : na < nb ? CSTR_LESS_THAN : CSTR_GREATER_THAN;
}

int FindNLSStringEx(LPCWSTR, DWORD flags, LPCWSTR text, int length, LPCWSTR value, int valueLength,
                    int* foundLength, void*, void*, LPARAM) {
    if (length < 0) length = (int)wcslen(text);
    if (valueLength < 0) valueLength = (int)wcslen(value);
    bool ignoreCase = (flags & (NORM_IGNORECASE | LINGUISTIC_IGNORECASE)) != 0;
    for (int at = 0; at <= length - valueLength; ++at) {
        int n = 0;
        for (; n < valueLength; ++n)
            if ((ignoreCase ? Lower(text[at + n]) : text[at + n]) !=
                (ignoreCase ? Lower(value[n]) : value[n])) break;
        if (n == valueLength) { if (foundLength) *foundLength = n; return at; }
    }
    return -1;
}
int CompareStringEx(LPCWSTR, DWORD flags, LPCWSTR a, int aLen, LPCWSTR b, int bLen, void*, void*, LPARAM) {
    std::wstring x(a, aLen < 0 ? wcslen(a) : (size_t)aLen), y(b, bLen < 0 ? wcslen(b) : (size_t)bLen);
    if (flags & SORT_DIGITSASNUMBERS) {
        int r = StrCmpLogicalW(x.c_str(), y.c_str());
        return r < 0 ? CSTR_LESS_THAN : r > 0 ? CSTR_GREATER_THAN : CSTR_EQUAL;
    }
    if (flags & (NORM_IGNORECASE | LINGUISTIC_IGNORECASE)) {
        for (auto& c : x) c = Lower(c);
        for (auto& c : y) c = Lower(c);
    }
    int r = wcscoll_l(x.c_str(), y.c_str(), Utf8Locale());
    return r < 0 ? CSTR_LESS_THAN : r > 0 ? CSTR_GREATER_THAN : CSTR_EQUAL;
}

// --- events, waits, time ---------------------------------------------------------------------------
HANDLE CreateEventW(SECURITY_ATTRIBUTES*, BOOL manualReset, BOOL initialState, LPCWSTR) {
    auto* e = new EventObject;
    e->manual = manualReset;
    e->set = initialState;
    return e;
}

BOOL SetEvent(HANDLE h) {
    auto* e = As<EventObject>(h, Object::Event);
    if (!e) return Fail(EBADF);
    {
        std::lock_guard lock(g_eventMx);
        e->set = true;
    }
    g_eventCv.notify_all();
    return TRUE;
}

BOOL ResetEvent(HANDLE h) {
    auto* e = As<EventObject>(h, Object::Event);
    if (!e) return Fail(EBADF);
    std::lock_guard lock(g_eventMx);
    e->set = false;
    return TRUE;
}

DWORD WaitForMultipleObjects(DWORD count, const HANDLE* handles, BOOL waitAll, DWORD ms) {
    std::unique_lock lock(g_eventMx);
    auto ready = [&]() -> int {
        if (waitAll) {
            for (DWORD i = 0; i < count; ++i)
                if (!Signaled(handles[i])) return -1;
            for (DWORD i = 0; i < count; ++i) Consume(handles[i]);
            return 0;
        }
        for (DWORD i = 0; i < count; ++i)
            if (Signaled(handles[i])) {
                Consume(handles[i]);
                return (int)i;
            }
        return -1;
    };
    int r = ready();
    if (r < 0 && ms != 0) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while ((r = ready()) < 0) {
            if (ms == INFINITE) g_eventCv.wait(lock);
            else if (g_eventCv.wait_until(lock, deadline) == std::cv_status::timeout && (r = ready()) < 0) break;
        }
    }
    return r < 0 ? WAIT_TIMEOUT : WAIT_OBJECT_0 + (DWORD)r;
}

DWORD WaitForSingleObject(HANDLE h, DWORD ms) { return WaitForMultipleObjects(1, &h, FALSE, ms); }

void Sleep(DWORD ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

ULONGLONG GetTickCount64() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ULONGLONG)ts.tv_sec * 1000 + (ULONGLONG)ts.tv_nsec / 1000000;
}
DWORD GetTickCount() { return (DWORD)GetTickCount64(); }

void GetSystemTimeAsFileTime(FILETIME* ft) {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    *ft = FromU64(compat::UnixToFileTime(ts.tv_sec, ts.tv_nsec));
}

void GetLocalTime(SYSTEMTIME* st) {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    tm t;
    localtime_r(&ts.tv_sec, &t);
    TmToSystemTime(t, (int)(ts.tv_nsec / 1000000), st);
}

void GetSystemTime(SYSTEMTIME* st) {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    tm t;
    gmtime_r(&ts.tv_sec, &t);
    TmToSystemTime(t, (int)(ts.tv_nsec / 1000000), st);
}

BOOL FileTimeToSystemTime(const FILETIME* ft, SYSTEMTIME* st) {
    uint64_t v = ToU64(*ft);
    time_t secs = (time_t)(v / 10000000ull) - (time_t)kEpochDiff;
    tm t;
    if (!gmtime_r(&secs, &t)) return FALSE;
    TmToSystemTime(t, (int)((v / 10000) % 1000), st);
    return TRUE;
}

BOOL SystemTimeToFileTime(const SYSTEMTIME* st, FILETIME* ft) {
    tm t{};
    t.tm_year = st->wYear - 1900;
    t.tm_mon = st->wMonth - 1;
    t.tm_mday = st->wDay;
    t.tm_hour = st->wHour;
    t.tm_min = st->wMinute;
    t.tm_sec = st->wSecond;
    *ft = FromU64(compat::UnixToFileTime(timegm(&t), st->wMilliseconds * 1000000L));
    return TRUE;
}

BOOL FileTimeToLocalFileTime(const FILETIME* ft, FILETIME* local) {
    uint64_t v = ToU64(*ft);
    time_t secs = (time_t)(v / 10000000ull) - (time_t)kEpochDiff;
    tm t;
    localtime_r(&secs, &t);
    *local = FromU64(v + (uint64_t)((int64_t)t.tm_gmtoff * 10000000));
    return TRUE;
}

BOOL SystemTimeToTzSpecificLocalTime(const void*, const SYSTEMTIME* utc, SYSTEMTIME* local) {
    FILETIME ft, lft;
    SystemTimeToFileTime(utc, &ft);
    FileTimeToLocalFileTime(&ft, &lft);
    return FileTimeToSystemTime(&lft, local);
}

BOOL QueryPerformanceCounter(LARGE_INTEGER* count) {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    count->QuadPart = (LONGLONG)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    return TRUE;
}
BOOL QueryPerformanceFrequency(LARGE_INTEGER* freq) {
    freq->QuadPart = 1000000000LL;
    return TRUE;
}

DWORD GetCurrentProcessId() { return (DWORD)getpid(); }
DWORD GetCurrentThreadId() { return (DWORD)syscall(SYS_gettid); }
HANDLE GetCurrentThread() { return (HANDLE)(intptr_t)-2; }

BOOL SetThreadPriority(HANDLE, int priority) {
    // Linux threads have their own nice value. Lowering it needs no privileges; raising it is
    // only attempted (the audio thread gets real-time scheduling from PipeWire instead).
    int nice = priority == THREAD_MODE_BACKGROUND_BEGIN ? 10
               : priority == THREAD_MODE_BACKGROUND_END ? 0
               : priority <= THREAD_PRIORITY_LOWEST     ? 5
               : priority < 0                           ? 2
                                                        : 0;
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), nice);
    return TRUE;
}

DWORD GetEnvironmentVariableW(LPCWSTR name, LPWSTR out, DWORD size) {
    const char* v = getenv(ToPath(name).c_str());
    if (!v) {
        t_lastError = 203;  // ERROR_ENVVAR_NOT_FOUND
        return 0;
    }
    std::wstring w = compat::Widen(v);
    if (!out || w.size() + 1 > size) return (DWORD)w.size() + 1;
    wcscpy(out, w.c_str());
    return (DWORD)w.size();
}

// --- messages ------------------------------------------------------------------------------------
BOOL PostMessageW(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    std::function<void(HWND, UINT, WPARAM, LPARAM)> post;
    {
        std::lock_guard lock(g_postMx);
        post = g_post;
    }
    if (!post) return FALSE;
    post(hwnd, msg, wp, lp);
    return TRUE;
}

void SetPostMessageHandler(std::function<void(HWND, UINT, WPARAM, LPARAM)> handler) {
    std::lock_guard lock(g_postMx);
    g_post = std::move(handler);
}
