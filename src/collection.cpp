// Export collection and Import collection (collection.h): the ZIP writer, where each file goes
// in the archive, the export run, and the import's checks and unpacking. Portable: Windows and Linux
// (through compat) share all of it but the free-space and file-system questions at the end.
#include "collection.h"

#include "archive.h"
#include "cue.h"
#include "inflate.h"
#include "organize.h"
#include "streamfile.h"
#include "textcase.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include <shlobj.h>
#include <shlwapi.h>
#ifndef _WIN32
#include <sys/statvfs.h>
#include <sys/vfs.h>
#endif

namespace collection {
namespace {

constexpr uint32_t kChunk = 1u << 20;
constexpr uint64_t kEpochTicks = 116444736000000000ull;  // 1970-01-01 as FILETIME ticks
constexpr char kReadme[] = "README.txt";
constexpr wchar_t kPlaylists[] = L"Playlists";
constexpr wchar_t kOther[] = L"Other";
constexpr wchar_t kPartSuffix[] = L".cordinal-part";  // an unpacked file until its check value matches

void Put16(std::string& s, uint32_t v) {
    s += (char)(v & 0xFF);
    s += (char)(v >> 8 & 0xFF);
}
void Put32(std::string& s, uint32_t v) {
    Put16(s, v & 0xFFFF);
    Put16(s, v >> 16);
}
void Put64(std::string& s, uint64_t v) {
    Put32(s, (uint32_t)v);
    Put32(s, (uint32_t)(v >> 32));
}

uint32_t UnixFromFileTime(uint64_t ft) {
    return ft > kEpochTicks ? (uint32_t)std::min<uint64_t>((ft - kEpochTicks) / 10000000ull, UINT32_MAX) : 0;
}
uint64_t FileTimeFromUnix(uint32_t t) { return kEpochTicks + (uint64_t)t * 10000000ull; }

// MS-DOS date (high half) and time (low half) in local time, as ZIP headers keep them: two-second
// steps, from 1980 to 2107.
uint32_t DosTime(uint64_t ft) {
    FILETIME utc{(DWORD)ft, (DWORD)(ft >> 32)}, local;
    SYSTEMTIME st;
    if (!ft || !FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &st) || st.wYear < 1980)
        return (1u << 5 | 1u) << 16;  // 1980-01-01 00:00
    const uint32_t year = std::min<uint32_t>(st.wYear, 2107) - 1980;
    return (year << 9 | (uint32_t)st.wMonth << 5 | st.wDay) << 16 |
           ((uint32_t)st.wHour << 11 | (uint32_t)st.wMinute << 5 | st.wSecond / 2u);
}

SYSTEMTIME Utc(uint32_t unixTime) {
    const uint64_t ft = FileTimeFromUnix(unixTime);
    FILETIME f{(DWORD)ft, (DWORD)(ft >> 32)};
    SYSTEMTIME st{};
    FileTimeToSystemTime(&f, &st);
    return st;
}

std::wstring ErrorText(DWORD code) {
    if (code == ERROR_DISK_FULL || code == ERROR_HANDLE_DISK_FULL) return L"The drive is full.";
    wchar_t buf[512] = {};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, buf, (DWORD)std::size(buf), nullptr);
    std::wstring s = buf;
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ')) s.pop_back();
    return s.empty() ? L"Error " + std::to_wstring(code) + L"." : s;
}

// How the file system tells two paths apart: Windows ignores case.
std::wstring PathKey(std::wstring s) {
#ifdef _WIN32
    CharLowerBuffW(s.data(), (DWORD)s.size());
#endif
    return s;
}
// Archive names are kept apart ignoring case everywhere, since an archive made on Linux may be
// unpacked on Windows.
bool Inside(const std::wstring& path, const std::wstring& dir) {
    if (dir.empty() || path.size() <= dir.size()) return false;
    const bool ends = dir.back() == L'\\' || dir.back() == L'/';
    if (!ends && path[dir.size()] != L'\\' && path[dir.size()] != L'/') return false;
    return PathKey(path.substr(0, dir.size())) == PathKey(dir);
}

std::wstring Trimmed(std::wstring dir) {
    while (dir.size() > 1 && (dir.back() == L'\\' || dir.back() == L'/') && !(dir.size() == 3 && dir[1] == L':')) dir.pop_back();
    return dir;
}

std::wstring Join(const std::wstring& dir, const std::wstring& rel) {
    return dir.empty() || dir.back() == L'\\' || dir.back() == L'/' ? dir + rel : dir + kPathSep + rel;
}

bool Stat(const std::wstring& path, uint64_t& size) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    size = (uint64_t)a.nFileSizeHigh << 32 | a.nFileSizeLow;
    return true;
}

// The files beside the music that cordinal reads too: cover images, cue sheets and lyrics.
bool IsCompanion(const std::wstring& name) {
    const std::wstring ext = ExtOf(name);
    return ext == L"jpg" || ext == L"jpeg" || ext == L"png" || ext == L"gif" || ext == L"bmp" || ext == L"webp" ||
           ext == L"cue" || ext == L"lrc";
}

// "name", or the first of "name (2)", "name (3)"... (before the extension, with `ext`) that `taken`
// doesn't hold yet; the one returned is added to it.
std::wstring Unique(const std::wstring& name, std::unordered_set<std::wstring>& taken, bool ext) {
    std::wstring stem = name, tail;
    if (const size_t dot = name.rfind(L'.'); ext && dot != std::wstring::npos && dot > 0) {
        stem = name.substr(0, dot);
        tail = name.substr(dot);
    }
    std::wstring candidate = name;
    for (int n = 2; !taken.insert(textcase::Fold(candidate)).second; ++n) candidate = stem + L" (" + std::to_wstring(n) + L")" + tail;
    return candidate;
}

// A folder's own name, safe for any file system, for its place at the archive's top.
std::wstring FolderName(const std::wstring& dir) {
    const std::wstring t = Trimmed(dir);
    if (t.size() >= 2 && t.size() <= 3 && t[1] == L':') return std::wstring(L"Drive ") + (wchar_t)towupper(t[0]);  // "D:\"
    const std::wstring base = SafePathComponent(FileNameOf(t));
    return base.empty() ? L"Music" : base;
}

// The real file behind a library path: a CUE or game-music song's file, an archived song's archive.
std::wstring RealFile(const std::wstring& path) { return ContainerOf(AudioFileOf(path)); }

std::string ExtInf(const Library& library, TrackId id, const std::wstring& path) {
    const Track& t = library[id];
    std::string title = t.title ? std::string(library.View(t.title)) : ToUtf8(StemOf(AudioFileOf(path)));
    std::string display = t.artist ? std::string(library.View(t.artist)) + " - " + title : title;
    for (char& c : display)
        if (c == '\t' || c == '\n' || c == '\r' || c == '\0') c = ' ';
    return "#EXTINF:" + (t.durationMs ? std::to_string(t.durationMs / 1000) : std::string("-1")) + "," + display;
}

std::string IsoDate(uint32_t unixTime) {
    const SYSTEMTIME st = Utc(unixTime);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04u-%02u-%02uT%02u:%02u:%02uZ", (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
             (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond);
    return buf;
}

std::string LocalDay(uint32_t unixTime) {
    const uint64_t ft = FileTimeFromUnix(unixTime);
    FILETIME utc{(DWORD)ft, (DWORD)(ft >> 32)}, local;
    SYSTEMTIME st{};
    if (!FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &st)) st = Utc(unixTime);
    char buf[16];
    snprintf(buf, sizeof(buf), "%04u-%02u-%02u", (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay);
    return buf;
}

// Bytes free to this user on the drive that holds `path` (or its nearest existing folder);
// UINT64_MAX when it can't be told.
std::wstring Existing(std::wstring path) {
    while (!path.empty() && GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::wstring up = DirectoryOf(path);
        if (up.empty() || up == path) return {};
        path = up;
    }
    return path;
}
uint64_t FreeSpace(const std::wstring& path) {
    const std::wstring at = Existing(path);
    if (at.empty()) return UINT64_MAX;
#ifdef _WIN32
    ULARGE_INTEGER free{};
    return GetDiskFreeSpaceExW(at.c_str(), &free, nullptr, nullptr) ? free.QuadPart : UINT64_MAX;
#else
    struct statvfs s;
    return statvfs(ToUtf8(at).c_str(), &s) == 0 ? (uint64_t)s.f_bavail * s.f_frsize : UINT64_MAX;
#endif
}
// FAT32 (memory cards, many USB sticks) can't hold a file of 4 GB or more.
bool FatDrive(const std::wstring& path) {
    const std::wstring at = Existing(path);
    if (at.empty()) return false;
#ifdef _WIN32
    wchar_t root[MAX_PATH + 1], fs[32];
    return GetVolumePathNameW(at.c_str(), root, (DWORD)std::size(root)) &&
           GetVolumeInformationW(root, nullptr, 0, nullptr, nullptr, nullptr, fs, (DWORD)std::size(fs)) &&
           (_wcsicmp(fs, L"FAT32") == 0 || _wcsicmp(fs, L"FAT") == 0);
#else
    struct statfs s;
    return statfs(ToUtf8(at).c_str(), &s) == 0 && s.f_type == 0x4d44;  // MSDOS_SUPER_MAGIC
#endif
}
constexpr uint64_t kFatLimit = 0xFFFFFFFFull;

std::wstring SpaceError(uint64_t needed, uint64_t free, const std::wstring& where) {
    return L"This needs about " + SizeText(needed) + L" of space, but " + where + L" has only " + SizeText(free) +
           L" free. Free some space or choose another drive.";
}

// A file's CRC-32, read in chunks; false when it can't be read or the run is cancelled.
bool FileCrc(const std::wstring& path, const std::atomic<bool>& cancel, uint32_t& crc) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::unique_ptr<char[]> buffer(new char[kChunk]);
    crc = 0;
    DWORD got = 0;
    bool ok = true;
    while (ok && !cancel) {
        if (!ReadFile(h, buffer.get(), kChunk, &got, nullptr)) ok = false;
        else if (!got) break;
        else crc = Crc32((const unsigned char*)buffer.get(), got, crc);
    }
    CloseHandle(h);
    return ok && !cancel;
}

// Writes a small file whole under a name nothing has: through a temp name, never over a file.
bool WriteNew(const std::wstring& path, std::string_view bytes, std::wstring& error) {
    SHCreateDirectoryExW(nullptr, DirectoryOf(path).c_str(), nullptr);
    const std::wstring temp = path + kPartSuffix;
    HANDLE h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        error = ErrorText(GetLastError());
        return false;
    }
    DWORD n = 0;
    const bool ok = WriteFile(h, bytes.data(), (DWORD)bytes.size(), &n, nullptr) && n == bytes.size() && FlushFileBuffers(h);
    if (!ok) error = ErrorText(GetLastError());
    CloseHandle(h);
    if (ok && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH)) return true;
    if (ok) error = ErrorText(GetLastError());
    DeleteFileW(temp.c_str());
    return false;
}

// The first of "path", "name (2).ext", "name (3).ext"... that doesn't exist yet, or (with `same`)
// that already holds exactly the bytes `size` and `crc` describe, which `same` then reports. Empty
// when the run is cancelled while comparing.
std::wstring Place(const std::wstring& path, uint64_t size, uint32_t crc, bool* same, const std::atomic<bool>& cancel) {
    const std::wstring dir = DirectoryOf(path), name = FileNameOf(path);
    const size_t dot = name.rfind(L'.');
    const std::wstring stem = dot != std::wstring::npos && dot > 0 ? name.substr(0, dot) : name;
    const std::wstring ext = dot != std::wstring::npos && dot > 0 ? name.substr(dot) : std::wstring();
    std::wstring candidate = path;
    for (int n = 2;; ++n) {
        if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES) return candidate;
        uint64_t has = 0;
        uint32_t check = 0;
        if (same && Stat(candidate, has) && has == size && FileCrc(candidate, cancel, check) && check == crc) {
            *same = true;
            return candidate;
        }
        if (cancel) return {};
        candidate = Join(dir, stem + L" (" + std::to_wstring(n) + L")" + ext);
    }
}

// A playlist line's place in the archive: relative to Playlists/, "../" climbing out of it.
// Empty when it is absolute or climbs out of the archive.
std::string Resolve(std::string_view line) {
    if (line.empty() || line[0] == '/' || line[0] == '\\' || (line.size() > 1 && line[1] == ':')) return {};
    std::vector<std::string_view> parts{"Playlists"};
    for (size_t at = 0; at <= line.size();) {
        size_t end = std::min(line.find('/', at), line.size());
        std::string_view part = line.substr(at, end - at);
        at = end + 1;
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            if (parts.empty()) return {};
            parts.pop_back();
        } else {
            parts.push_back(part);
        }
    }
    std::string out;
    for (auto part : parts) (out.empty() ? out : out += '/') += part;
    return out;
}

std::string Readme(uint32_t created) {
    return "This is a music collection exported by cordinal on " + LocalDay(created) +
           ": the songs in their folders,\r\nwith their cover images, and the playlists.\r\n\r\n"
           "To bring it into cordinal, choose File > Import collection... and pick this file. cordinal unpacks it\r\n"
           "into a folder you choose and adds the music to its library.\r\n\r\n"
           "Any unzip program opens it too: the music is in ordinary folders, and the playlists in Playlists use\r\n"
           "paths relative to that folder.\r\n";
}

}  // namespace

// ---------------------------------------------------------------------------
// ZipWriter

ZipWriter::ZipWriter(const std::wstring& path, bool zip64) : zip64_(zip64) {
    handle_ = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    ok_ = handle_ != INVALID_HANDLE_VALUE;
    if (!ok_) error_ = ErrorText(GetLastError());
}

ZipWriter::~ZipWriter() {
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
}

bool ZipWriter::Valid() const { return ok_; }

bool ZipWriter::Write(std::string_view bytes) {
    DWORD n = 0;
    if (!ok_) return false;
    if (!WriteFile(handle_, bytes.data(), (DWORD)bytes.size(), &n, nullptr) || n != bytes.size()) {
        error_ = ErrorText(GetLastError());
        return ok_ = false;
    }
    offset_ += n;
    return true;
}

// Version 4.5 when ZIP64 sizes follow in the extra field, else 2.0. Names are UTF-8 (flag 0x800),
// and every entry is stored: music doesn't shrink, and stored entries unpack at disk speed.
std::string ZipWriter::LocalHeader(const Entry& e) const {
    const bool big = zip64_ || e.size >= 0xFFFFFFFFull;
    std::string h;
    Put32(h, 0x04034B50);
    Put16(h, big ? 45 : 20);
    Put16(h, 0x800);
    Put16(h, 0);
    Put32(h, e.dos);
    Put32(h, e.crc);
    Put32(h, big ? 0xFFFFFFFF : (uint32_t)e.size);
    Put32(h, big ? 0xFFFFFFFF : (uint32_t)e.size);
    Put16(h, (uint32_t)e.name.size());
    Put16(h, big ? 9 + 20 : 9);
    h += e.name;
    Put16(h, 0x5455);  // extended timestamp: the modification time, UTC
    Put16(h, 5);
    h += '\1';
    Put32(h, e.unixTime);
    if (big) {
        Put16(h, 1);
        Put16(h, 16);
        Put64(h, e.size);
        Put64(h, e.size);
    }
    return h;
}

bool ZipWriter::Add(std::string_view name, std::string_view bytes, uint32_t unixTime) {
    Entry e{std::string(name), bytes.size(), offset_, Crc32((const unsigned char*)bytes.data(), bytes.size()), unixTime,
            DosTime(FileTimeFromUnix(unixTime))};
    if (!Write(LocalHeader(e)) || !Write(bytes)) return false;
    entries_.push_back(std::move(e));
    return true;
}

bool ZipWriter::AddFile(std::string_view name, const std::wstring& file, const std::function<bool(uint64_t)>& chunk,
                        std::wstring& error) {
    if (!ok_) return false;
    // Sharing delete lets a tag edit replace the file meanwhile; the archive keeps what was read.
    HANDLE in = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                            FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (in == INVALID_HANDLE_VALUE) {
        error = ErrorText(GetLastError());
        return false;
    }
    LARGE_INTEGER size{};
    FILETIME written{};
    if (!GetFileSizeEx(in, &size) || size.QuadPart < 0 || !GetFileTime(in, nullptr, nullptr, &written)) {
        error = ErrorText(GetLastError());
        CloseHandle(in);
        return false;
    }
    const uint64_t ft = (uint64_t)written.dwHighDateTime << 32 | written.dwLowDateTime;
    Entry e{std::string(name), (uint64_t)size.QuadPart, offset_, 0, UnixFromFileTime(ft), DosTime(ft)};
    std::unique_ptr<char[]> buffer(new char[kChunk]);
    bool whole = Write(LocalHeader(e));
    for (uint64_t left = e.size; whole && left;) {
        const DWORD want = (DWORD)std::min<uint64_t>(left, kChunk);
        DWORD got = 0;
        if (!ReadFile(in, buffer.get(), want, &got, nullptr) || got != want) {
            error = got != want && GetLastError() == 0 ? L"The file got shorter while it was being read." : ErrorText(GetLastError());
            whole = false;
            break;
        }
        e.crc = Crc32((const unsigned char*)buffer.get(), got, e.crc);
        if (!Write({buffer.get(), got})) break;
        left -= got;
        if (chunk && !chunk(got)) {
            error = L"Cancelled.";
            whole = false;
        }
    }
    CloseHandle(in);
    if (!ok_) {
        error = error_;
        return false;
    }
    LARGE_INTEGER at{};
    if (!whole) {  // cut the archive back to where this entry began
        at.QuadPart = (LONGLONG)e.offset;
        if (!SetFilePointerEx(handle_, at, nullptr, FILE_BEGIN) || !SetEndOfFile(handle_)) {
            error_ = ErrorText(GetLastError());
            ok_ = false;
        }
        offset_ = e.offset;
        return false;
    }
    // The check value is known only now: it goes into the header written before the data.
    std::string crc;
    Put32(crc, e.crc);
    DWORD n = 0;
    LARGE_INTEGER end{};
    at.QuadPart = (LONGLONG)(e.offset + 14);
    end.QuadPart = (LONGLONG)offset_;
    if (!SetFilePointerEx(handle_, at, nullptr, FILE_BEGIN) || !WriteFile(handle_, crc.data(), 4, &n, nullptr) || n != 4 ||
        !SetFilePointerEx(handle_, end, nullptr, FILE_BEGIN)) {
        error_ = error = ErrorText(GetLastError());
        return ok_ = false;
    }
    entries_.push_back(std::move(e));
    return true;
}

bool ZipWriter::Finish() {
    if (!ok_) return false;
    const uint64_t start = offset_;
    std::string cd;
    for (const Entry& e : entries_) {
        const bool bigSize = zip64_ || e.size >= 0xFFFFFFFFull, bigOffset = zip64_ || e.offset >= 0xFFFFFFFFull;
        std::string extra, z;
        Put16(extra, 0x5455);
        Put16(extra, 5);
        extra += '\1';
        Put32(extra, e.unixTime);
        if (bigSize) {
            Put64(z, e.size);
            Put64(z, e.size);
        }
        if (bigOffset) Put64(z, e.offset);
        if (!z.empty()) {
            Put16(extra, 1);
            Put16(extra, (uint32_t)z.size());
            extra += z;
        }
        Put32(cd, 0x02014B50);
        Put16(cd, 45);  // made by version 4.5, MS-DOS attributes
        Put16(cd, z.empty() ? 20 : 45);
        Put16(cd, 0x800);
        Put16(cd, 0);
        Put32(cd, e.dos);
        Put32(cd, e.crc);
        Put32(cd, bigSize ? 0xFFFFFFFF : (uint32_t)e.size);
        Put32(cd, bigSize ? 0xFFFFFFFF : (uint32_t)e.size);
        Put16(cd, (uint32_t)e.name.size());
        Put16(cd, (uint32_t)extra.size());
        Put32(cd, 0);  // no comment, disk 0
        Put16(cd, 0);  // internal attributes
        Put32(cd, 0);  // external attributes
        Put32(cd, bigOffset ? 0xFFFFFFFF : (uint32_t)e.offset);
        cd += e.name;
        cd += extra;
        if (cd.size() >= kChunk) {
            if (!Write(cd)) return false;
            cd.clear();
        }
    }
    if (!Write(cd)) return false;
    const uint64_t count = entries_.size(), size = offset_ - start;
    std::string end;
    if (zip64_ || count >= 0xFFFF || start >= 0xFFFFFFFFull || size >= 0xFFFFFFFFull) {
        const uint64_t record = offset_;
        Put32(end, 0x06064B50);
        Put64(end, 44);
        Put16(end, 45);
        Put16(end, 45);
        Put64(end, 0);  // this disk, the directory's disk
        Put64(end, count);
        Put64(end, count);
        Put64(end, size);
        Put64(end, start);
        Put32(end, 0x07064B50);
        Put32(end, 0);
        Put64(end, record);
        Put32(end, 1);
        Put32(end, 0x06054B50);
        Put32(end, 0);
        Put32(end, 0xFFFFFFFF);
        Put32(end, 0xFFFFFFFF);
        Put32(end, 0xFFFFFFFF);
    } else {
        Put32(end, 0x06054B50);
        Put32(end, 0);
        Put16(end, (uint32_t)count);
        Put16(end, (uint32_t)count);
        Put32(end, (uint32_t)size);
        Put32(end, (uint32_t)start);
    }
    Put16(end, 0);  // no comment
    if (!Write(end)) return false;
    if (!FlushFileBuffers(handle_)) {
        error_ = ErrorText(GetLastError());
        return ok_ = false;
    }
    CloseHandle(handle_);
    handle_ = INVALID_HANDLE_VALUE;
    return true;
}

// ---------------------------------------------------------------------------
// Export

namespace {

bool Shown(const Track& t, bool showOriginals) {
    return t.InLibrary() && !(t.flags & Track::kExcluded) && (showOriginals || !(t.flags & Track::kOriginal));
}

std::wstring ArtistOf(const Library& library, const Track& t) {
    return library.Str(t.albumArtist ? t.albumArtist : t.artist);
}

}  // namespace

ExportChoices ListChoices(const Library& library, const std::vector<Playlist>& playlists, bool showOriginals) {
    ExportChoices c;
    struct Sorted {
        uint32_t order;  // disc and track number
        TrackId id;
    };
    std::unordered_map<std::wstring, size_t> artistKey;
    std::vector<std::unordered_map<std::wstring, size_t>> albumKey;
    std::vector<std::vector<std::vector<Sorted>>> ids;  // [artist][album] -> songs
    for (TrackId id = 0; id < library.Count(); ++id) {
        const Track& t = library[id];
        if (!Shown(t, showOriginals)) continue;
        std::wstring artist = ArtistOf(library, t);
        auto [a, newArtist] = artistKey.try_emplace(textcase::Fold(artist), c.artists.size());
        if (newArtist) {
            c.artists.push_back({std::move(artist), {}, 0});
            albumKey.emplace_back();
            ids.emplace_back();
        }
        ExportChoices::Artist& ar = c.artists[a->second];
        std::wstring album = library.Str(t.album);
        auto [b, newAlbum] = albumKey[a->second].try_emplace(textcase::Fold(album), ar.albums.size());
        if (newAlbum) {
            ar.albums.push_back({std::move(album), {}});
            ids[a->second].emplace_back();
        }
        ids[a->second][b->second].push_back({(uint32_t)t.disc << 16 | t.trackNo, id});
        ++ar.songs;
    }
    for (size_t i = 0; i < c.artists.size(); ++i)
        for (size_t j = 0; j < c.artists[i].albums.size(); ++j) {
            auto& songs = c.artists[i].albums[j].songs;
            for (const Sorted& s : ids[i][j]) {
                const Track& t = library[s.id];
                std::wstring path = library.Path(s.id);
                std::wstring title = t.title ? library.Str(t.title) : library.Str(t.file);
                songs.push_back({std::move(title), std::move(path)});
            }
            std::vector<std::wstring> folded;
            folded.reserve(songs.size());
            for (const auto& song : songs) folded.push_back(textcase::Fold(song.title));
            std::vector<size_t> order(songs.size());
            for (size_t k = 0; k < order.size(); ++k) order[k] = k;
            std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) {
                if (ids[i][j][x].order != ids[i][j][y].order) return ids[i][j][x].order < ids[i][j][y].order;
                return folded[x] < folded[y];
            });
            std::vector<ExportChoices::Song> sorted;
            for (size_t k : order) sorted.push_back(std::move(songs[k]));
            songs = std::move(sorted);
        }
    // Names are folded once each, not once per comparison; empty names go last.
    auto sortByName = [](auto& items) {
        std::vector<std::pair<std::wstring, size_t>> keys;
        keys.reserve(items.size());
        for (size_t k = 0; k < items.size(); ++k) keys.emplace_back(textcase::Fold(items[k].name), k);
        std::sort(keys.begin(), keys.end(), [&](const auto& a, const auto& b) {
            const bool ae = items[a.second].name.empty(), be = items[b.second].name.empty();
            if (ae != be) return be;
            return a.first < b.first;
        });
        std::decay_t<decltype(items)> sorted;
        sorted.reserve(items.size());
        for (const auto& key : keys) sorted.push_back(std::move(items[key.second]));
        items = std::move(sorted);
    };
    for (auto& ar : c.artists) sortByName(ar.albums);
    sortByName(c.artists);
    for (const Playlist& p : playlists)
        if (p.kind == PlaylistKind::Static) c.playlists.push_back({p.name, p.items.size()});
    return c;
}

ExportSource CaptureExport(const Library& library, const std::vector<Playlist>& playlists,
                           const std::vector<std::wstring>& roots, bool showOriginals, const ExportSelection* selection) {
    ExportSource s;
    s.roots = roots;
    s.created = UnixNow();
    std::unordered_set<std::wstring> songs;
    if (selection)
        for (const std::wstring& path : selection->songs) songs.insert(PathKey(path));
    std::vector<char> taken(library.Count(), 0);
    for (TrackId id = 0; id < library.Count(); ++id) {
        const Track& t = library[id];
        if (!Shown(t, showOriginals)) continue;
        if (selection && !songs.count(PathKey(library.Path(id)))) continue;
        s.tracks.push_back(library.Path(id));
        taken[id] = 1;
    }
    for (const Playlist& p : playlists) {
        if (p.kind != PlaylistKind::Static) continue;
        if (selection && std::find(selection->playlists.begin(), selection->playlists.end(), p.name) == selection->playlists.end())
            continue;
        ExportSource::List list{p.name, {}, {}};
        for (TrackId id : p.items) {
            if (!library.Contains(id) || (library[id].flags & Track::kMissing)) continue;
            std::wstring path = library.Path(id);
            list.infos.push_back(ExtInf(library, id, path));
            if (!taken[id]) {
                taken[id] = 1;
                s.tracks.push_back(path);
            }
            list.paths.push_back(std::move(path));
        }
        s.playlists.push_back(std::move(list));
    }
    return s;
}

ExportResult Export(const ExportSource& source, const std::wstring& zip, const std::atomic<bool>& cancel,
                    const ProgressFn& progress) {
    ExportResult r;
    struct Item {
        std::wstring path, name;  // name: the place in the archive, '/'-separated
        uint64_t size = 0;
        size_t songs = 0;
        bool added = false;
    };
    std::vector<Item> items;
    std::unordered_map<std::wstring, size_t> byPath;  // PathKey(real file) -> item, or SIZE_MAX when missing
    std::vector<std::wstring> dirs;                   // folders with songs, in the order met
    std::unordered_set<std::wstring> dirSeen;

    // The real files behind the songs, once each: a CUE sheet's songs, or a pack's, share one.
    for (const std::wstring& track : source.tracks) {
        if (cancel) break;
        const std::wstring real = RealFile(track);
        auto [it, added] = byPath.try_emplace(PathKey(real), items.size());
        if (!added) {
            if (it->second != SIZE_MAX) ++items[it->second].songs;
            continue;
        }
        Item item{real, {}, 0, 1};
        if (!Stat(real, item.size)) {
            it->second = SIZE_MAX;
            r.skipped.push_back({real, L"The file is missing."});
            continue;
        }
        if (dirSeen.insert(PathKey(DirectoryOf(real))).second) dirs.push_back(DirectoryOf(real));
        items.push_back(std::move(item));
    }
    // The folders' cover images, cue sheets and lyrics come along.
    for (const std::wstring& dir : dirs) {
        if (cancel) break;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(Join(dir, L"*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !IsCompanion(fd.cFileName)) continue;
            const std::wstring path = Join(dir, fd.cFileName);
            if (byPath.try_emplace(PathKey(path), items.size()).second)
                items.push_back({path, {}, (uint64_t)fd.nFileSizeHigh << 32 | fd.nFileSizeLow, 0});
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (cancel) {
        r.cancelled = true;
        return r;
    }
    if (items.empty()) {
        r.error = source.tracks.empty() ? L"The library is empty, so there is nothing to export."
                                        : L"None of the library's music files could be found.";
        return r;
    }
    if (const auto at = byPath.find(PathKey(zip)); at != byPath.end()) {
        r.error = L"That file is part of the collection. Choose another name or folder for the export.";
        return r;
    }

    // Where each file goes: under its library folder's name, as it is arranged there; files from
    // elsewhere go into Other, a folder for each folder they came from.
    std::vector<std::wstring> roots;
    for (const std::wstring& root : source.roots)
        if (!root.empty()) roots.push_back(Trimmed(root));
    std::sort(roots.begin(), roots.end(), [](const std::wstring& a, const std::wstring& b) { return a.size() > b.size(); });
    std::unordered_set<std::wstring> tops{textcase::Fold(FromUtf8(kCollectionManifest)), textcase::Fold(FromUtf8(kReadme)), textcase::Fold(kPlaylists),
                                          textcase::Fold(kOther)},
        others;
    std::unordered_map<std::wstring, std::wstring> topOf, otherOf;  // root, or PathKey(folder) -> its name
    for (Item& item : items) {
        auto root = std::find_if(roots.begin(), roots.end(), [&](const std::wstring& r) { return Inside(item.path, r); });
        std::wstring name;
        if (root != roots.end()) {
            auto [top, added] = topOf.try_emplace(*root);
            if (added) top->second = Unique(FolderName(*root), tops, false);
            const bool ends = root->back() == L'\\' || root->back() == L'/';
            name = top->second + L"/" + item.path.substr(root->size() + (ends ? 0 : 1));
        } else {
            const std::wstring dir = DirectoryOf(item.path);
            auto [other, added] = otherOf.try_emplace(PathKey(dir));
            if (added) other->second = Unique(FolderName(dir), others, false);
            name = std::wstring(kOther) + L"/" + other->second + L"/" + FileNameOf(item.path);
        }
        if constexpr (kPathSep != L'/') std::replace(name.begin(), name.end(), kPathSep, L'/');
        item.name = std::move(name);
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return StrCmpLogicalW(a.name.c_str(), b.name.c_str()) < 0; });
    byPath.clear();
    for (size_t i = 0; i < items.size(); ++i) byPath[PathKey(items[i].path)] = i;

    Progress p;
    for (const Item& item : items) p.totalBytes += item.size;
    p.totalFiles = items.size() + source.playlists.size() + 2;
    const std::wstring folder = DirectoryOf(zip);
    const uint64_t needed = p.totalBytes + 1024 * (uint64_t)p.totalFiles + (1u << 20), avail = FreeSpace(folder);
    if (avail != UINT64_MAX && needed > avail) {
        r.error = SpaceError(needed, avail, L"that drive");
        return r;
    }
    if (p.totalBytes >= kFatLimit && FatDrive(folder)) {
        r.error = L"That drive uses FAT32, which can't hold a file of 4 GB or more, and the collection is " +
                  SizeText(p.totalBytes) + L". Choose a drive formatted as NTFS or exFAT.";
        return r;
    }

    const std::wstring part = zip + L".part";
    DeleteFileW(part.c_str());
    bool written = false;
    {
        ZipWriter w(part);
        auto fail = [&](const std::wstring& why) { r.error = L"Couldn't write the archive: " + why; };
        if (!w.Valid() || !w.Add(kReadme, Readme((uint32_t)source.created), (uint32_t)source.created)) fail(w.Error());
        for (Item& item : items) {
            if (!r.error.empty() || cancel) break;
            if (progress) progress(p);
            std::wstring error;
            const uint64_t before = p.bytes;
            item.added = w.AddFile(ToUtf8(item.name), item.path, [&](uint64_t n) {
                p.bytes += n;
                if (progress) progress(p);
                return !cancel;
            }, error);
            ++p.files;
            if (item.added) {
                r.songs += item.songs;
                continue;
            }
            p.bytes = before + item.size;
            if (!w.Valid()) fail(w.Error());
            else if (!cancel) r.skipped.push_back({item.path, error});
        }
        // The playlists, each song at its place in the archive, relative to the Playlists folder.
        std::unordered_set<std::wstring> names;
        for (size_t i = 0; i < source.playlists.size() && r.error.empty() && !cancel; ++i) {
            const auto& list = source.playlists[i];
            std::string text = "\xEF\xBB\xBF#EXTM3U\n";
            for (size_t k = 0; k < list.paths.size(); ++k) {
                const std::wstring& path = list.paths[k];
                const std::wstring real = RealFile(path);
                const auto it = byPath.find(PathKey(real));
                if (it == byPath.end() || !items[it->second].added) continue;
                std::wstring tail = path.substr(std::min(real.size(), path.size()));  // "|3", or a song inside a pack
                if constexpr (kPathSep != L'/') std::replace(tail.begin(), tail.end(), kPathSep, L'/');
                text += list.infos[k] + "\n../" + ToUtf8(items[it->second].name + tail) + "\n";
            }
            std::wstring safe = SafePathComponent(list.name);
            const std::wstring file = Unique((safe.empty() ? L"Playlist" : safe) + L".m3u8", names, true);
            if (!w.Add(ToUtf8(std::wstring(kPlaylists) + L"/" + file), text, (uint32_t)source.created)) fail(w.Error());
            else ++r.playlists;
            ++p.files;
        }
        if (r.error.empty() && !cancel) {
            const std::string manifest = "cordinal-collection " + std::to_string(kFormat) + "\ncreated " +
                                         IsoDate((uint32_t)source.created) + "\nsongs " + std::to_string(r.songs) +
                                         "\nplaylists " + std::to_string(r.playlists) + "\n";
            if (!w.Add(kCollectionManifest, manifest, (uint32_t)source.created) || !w.Finish()) fail(w.Error());
            r.files = w.Count();
            r.bytes = w.Size();
            written = r.error.empty();
        }
    }
    if (cancel && !written) r.cancelled = true;
    if (written && !MoveFileExW(part.c_str(), zip.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        r.error = L"Couldn't save the archive as " + zip + L": " + ErrorText(GetLastError());
        written = false;
    }
    if (!written) {
        DeleteFileW(part.c_str());
        return r;
    }
    r.ok = true;
    return r;
}

// ---------------------------------------------------------------------------
// Import

bool ReadCollection(const std::wstring& zip, ImportPlan& plan, std::wstring& error) {
    plan = ImportPlan{};
    plan.zip = zip;
    FileSource src(zip);
    std::vector<ZipEntry> entries;
    if (!src.Valid()) {
        error = L"Couldn't open " + zip + L".";
        return false;
    }
    const std::wstring notOurs = L"This isn't a collection exported by cordinal. Import collection takes only the archives that "
                                 L"File \x25B8 Export collection makes.";
    if (!ReadZipDirectory(src, entries)) {
        error = notOurs;
        return false;
    }
    auto manifest = std::find_if(entries.begin(), entries.end(), [](const ZipEntry& e) { return e.name == kCollectionManifest; });
    if (manifest == entries.end()) {
        error = notOurs;
        return false;
    }
    std::string text;
    const uint64_t at = manifest->method == 0 && manifest->size == manifest->packed && manifest->size < 65536 ? ZipDataStart(src, *manifest) : 0;
    if (!at || !src.Read(at, (size_t)manifest->size, text) || Crc32((const unsigned char*)text.data(), text.size()) != manifest->crc) {
        error = L"This collection is damaged: its description can't be read.";
        return false;
    }
    const std::string_view first = std::string_view(text).substr(0, text.find('\n'));
    constexpr std::string_view kTag = "cordinal-collection ";
    const int format = first.starts_with(kTag) ? atoi(std::string(first.substr(kTag.size())).c_str()) : 0;
    if (format > kFormat) {
        error = L"This collection was exported by a newer cordinal. Update cordinal to import it.";
        return false;
    }
    if (format != kFormat) {
        error = L"This collection is damaged: its description can't be read.";
        return false;
    }
    if (const size_t songs = text.find("\nsongs "); songs != std::string::npos) plan.songs = (size_t)strtoull(text.c_str() + songs + 7, nullptr, 10);

    std::unordered_set<std::wstring> tops;
    size_t counted = 0;
    for (const ZipEntry& e : entries) {
        if (e.name == kCollectionManifest || e.name == kReadme || e.name.empty() || e.name.back() == '/') continue;
        if ((e.flags & 1) || e.method != 0 || e.packed != e.size) {
            error = L"This archive was changed after cordinal exported it: some of its files are compressed or encrypted. "
                    L"Unzip it with another program, then add the folder with File \x25B8 Import folder once.";
            return false;
        }
        const std::wstring name = ZipEntryName(e);
        std::vector<std::wstring> parts;
        bool safe = name.find(L'\0') == std::wstring::npos;
        for (size_t start = 0; safe && start <= name.size();) {
            const size_t end = std::min(name.find(L'/', start), name.size());
            std::wstring part = name.substr(start, end - start);
            start = end + 1;
            if (part.empty() || part == L"." || part == L"..") safe = false;
#ifdef _WIN32
            // Names from Linux may hold characters Windows can't; ':' and '\' would leave the folder.
            part = SafePathComponent(part, 240);
            if (part.empty()) part = L"_";
#endif
            parts.push_back(std::move(part));
        }
        if (!safe) {
            error = L"This archive names a file outside its own folders (" + name + L"), so it isn't imported.";
            return false;
        }
        ImportPlan::File f;
        for (const std::wstring& part : parts) f.rel += (f.rel.empty() ? L"" : std::wstring(1, kPathSep)) + part;
        f.name = e.name;
        f.size = e.size;
        f.local = e.local;
        f.crc = e.crc;
        f.time = e.unixTime;
        f.playlist = parts.size() == 2 && name.starts_with(std::wstring(kPlaylists) + L"/") && ExtOf(parts[1]) == L"m3u8";
        if (f.playlist) {
            ++plan.playlists;
        } else {
            plan.bytes += f.size;
            if (IsNativeAudioType(ExtOf(parts.back()))) ++counted;
            if (parts.size() > 1 && tops.insert(PathKey(parts[0])).second) plan.folders.push_back(parts[0]);
        }
        plan.files.push_back(std::move(f));
    }
    if (!plan.songs) plan.songs = counted;
    if (plan.files.empty()) {
        error = L"This collection holds no music.";
        return false;
    }
    return true;
}

ImportResult Import(const ImportPlan& plan, const std::wstring& destIn, const std::atomic<bool>& cancel,
                    const ProgressFn& progress) {
    ImportResult r;
    const std::wstring dest = Trimmed(destIn);
    FileSource src(plan.zip);
    if (!src.Valid()) {
        r.error = L"Couldn't open " + plan.zip + L" again.";
        return r;
    }
    Progress p;
    p.totalBytes = plan.bytes;
    p.totalFiles = plan.files.size();
    // Room for what isn't there already; a file with the same name and size is checked when its turn comes.
    uint64_t needed = 0, largest = 0;
    for (const auto& f : plan.files) {
        if (cancel) break;
        uint64_t size = 0;
        if (!f.playlist && !(Stat(Join(dest, f.rel), size) && size == f.size)) needed += f.size;
        largest = std::max(largest, f.size);
    }
    if (cancel) {
        r.cancelled = true;
        return r;
    }
    const uint64_t avail = FreeSpace(dest);
    if (avail != UINT64_MAX && needed + (16u << 20) > avail) {
        r.error = SpaceError(needed + (16u << 20), avail, dest);
        return r;
    }
    if (largest >= kFatLimit && FatDrive(dest)) {
        r.error = L"That drive uses FAT32, which can't hold a file of 4 GB or more, and this collection has one. Choose a drive "
                  L"formatted as NTFS or exFAT.";
        return r;
    }

    std::unordered_map<std::string, std::wstring> placed;  // archive name -> the file that now holds it
    std::unordered_set<std::wstring> used;                  // PathKey of the top folders that got music
    std::unique_ptr<char[]> buffer(new char[kChunk]);
    auto top = [](const std::wstring& rel) { return PathKey(rel.substr(0, rel.find(kPathSep))); };
    for (const auto& f : plan.files) {
        if (f.playlist) continue;
        if (cancel) break;
        if (progress) progress(p);
        // A file already there with these bytes is this one (an earlier import, perhaps cut short);
        // a different one keeps its name and this one goes beside it.
        bool same = false;
        const std::wstring wanted = Join(dest, f.rel), target = Place(wanted, f.size, f.crc, &same, cancel);
        if (target.empty()) break;
        if (same) {
            placed[f.name] = target;
            if (f.rel.find(kPathSep) != std::wstring::npos) used.insert(top(f.rel));
            ++r.same;
            ++p.files;
            p.bytes += f.size;
            continue;
        }
        const bool beside = target != wanted;
        // The data goes to a temp name beside the file and gets the real name once it checks out.
        std::wstring error;
        const std::wstring temp = target + kPartSuffix;
        SHCreateDirectoryExW(nullptr, DirectoryOf(target).c_str(), nullptr);
        ZipEntry entry;
        entry.local = f.local;
        uint64_t offset = ZipDataStart(src, entry);
        HANDLE out = offset ? CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)
                            : INVALID_HANDLE_VALUE;
        bool ok = out != INVALID_HANDLE_VALUE, full = false;
        if (!offset) error = L"It is damaged in the archive.";
        else if (!ok) error = ErrorText(GetLastError());
        uint32_t crc = 0;
        for (uint64_t left = f.size; ok && left && !cancel;) {
            const size_t n = (size_t)std::min<uint64_t>(left, kChunk);
            DWORD wrote = 0;
            if (!src.Read(offset, n, buffer.get())) {
                error = L"It couldn't be read from the archive.";
                ok = false;
            } else if (!WriteFile(out, buffer.get(), (DWORD)n, &wrote, nullptr) || wrote != n) {
                const DWORD code = GetLastError();
                full = code == ERROR_DISK_FULL || code == ERROR_HANDLE_DISK_FULL;
                error = ErrorText(code);
                ok = false;
            } else {
                crc = Crc32((const unsigned char*)buffer.get(), n, crc);
                offset += n;
                left -= n;
                p.bytes += n;
                if (progress) progress(p);
            }
        }
        if (ok && cancel) ok = false;
        if (ok && crc != f.crc) {
            error = L"It is damaged in the archive: its check value doesn't match.";
            ok = false;
        }
        if (ok && f.time) {
            const uint64_t ft = FileTimeFromUnix(f.time);
            FILETIME t{(DWORD)ft, (DWORD)(ft >> 32)};
            SetFileTime(out, nullptr, nullptr, &t);
        }
        if (ok && !FlushFileBuffers(out)) {
            error = ErrorText(GetLastError());
            ok = false;
        }
        if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
        if (ok && !MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
            error = ErrorText(GetLastError());
            ok = false;
        }
        ++p.files;
        if (!ok) {
            DeleteFileW(temp.c_str());
            if (cancel) break;
            if (full) {
                r.error = L"The drive is full. The files unpacked so far stay in " + dest + L".";
                break;
            }
            r.failed.push_back({FromUtf8(f.name), error});
            continue;
        }
        placed[f.name] = target;
        if (f.rel.find(kPathSep) != std::wstring::npos) used.insert(top(f.rel));
        ++r.written;
        r.bytes += f.size;
        if (beside) ++r.renamed;
    }
    if (cancel) r.cancelled = true;
    if (r.cancelled || !r.error.empty()) return r;

    // The playlists, pointed at the files as they are now, in dest/Playlists and for the library.
    for (const auto& f : plan.files) {
        if (!f.playlist) continue;
        ++p.files;
        std::string text;
        ZipEntry entry;
        entry.local = f.local;
        const uint64_t at = f.size < (16u << 20) ? ZipDataStart(src, entry) : 0;
        if (!at || !src.Read(at, (size_t)f.size, text) || Crc32((const unsigned char*)text.data(), text.size()) != f.crc) {
            r.failed.push_back({FromUtf8(f.name), L"It is damaged in the archive."});
            continue;
        }
        ImportResult::List list;
        std::wstring stored = FromUtf8(f.name.substr(f.name.find('/') + 1));
        list.name = stored.substr(0, stored.size() - 5);  // without ".m3u8"
        const std::wstring folder = Join(dest, kPlaylists);
        std::string out = "\xEF\xBB\xBF#EXTM3U\n", info;
        if (text.starts_with("\xEF\xBB\xBF")) text.erase(0, 3);
        for (size_t start = 0; start < text.size();) {
            size_t end = std::min(text.find('\n', start), text.size());
            std::string line = text.substr(start, end - start);
            start = end + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line == "#EXTM3U") continue;
            if (line[0] == '#') {
                info = line;
                continue;
            }
            // A song number ("|3") and a song inside a pack ("pack.zip/song.spc") follow the file.
            std::string where = Resolve(line), suffix;
            if (const size_t bar = where.rfind('|'); bar != std::string::npos && bar + 1 < where.size() &&
                std::all_of(where.begin() + (ptrdiff_t)bar + 1, where.end(), [](char c) { return c >= '0' && c <= '9'; })) {
                suffix = where.substr(bar);
                where.resize(bar);
            }
            std::string inner;
            auto found = placed.find(where);
            while (found == placed.end() && !where.empty()) {
                const size_t slash = where.rfind('/');
                if (slash == std::string::npos) break;
                inner = where.substr(slash) + inner;
                where.resize(slash);
                found = placed.find(where);
            }
            if (found == placed.end()) {
                info.clear();
                continue;
            }
            std::wstring tail = FromUtf8(inner + suffix);
            if constexpr (kPathSep != L'/') std::replace(tail.begin(), tail.end(), L'/', kPathSep);
            const std::wstring path = found->second + tail;
            list.paths.push_back(path);
            std::wstring rel = L"../" + path.substr(dest.size() + (dest.back() == L'\\' || dest.back() == L'/' ? 0 : 1));
            if constexpr (kPathSep != L'/') std::replace(rel.begin(), rel.end(), kPathSep, L'/');
            if (!info.empty()) out += info + "\n";
            out += ToUtf8(rel) + "\n";
            info.clear();
        }
        // Never over a file: one with the same contents is this playlist already.
        bool same = false;
        std::wstring error;
        const std::wstring file = Place(Join(folder, FileNameOf(f.rel)), out.size(),
                                        Crc32((const unsigned char*)out.data(), out.size()), &same, cancel);
        if (!file.empty() && !same && !WriteNew(file, out, error)) r.failed.push_back({file, error});
        r.playlists.push_back(std::move(list));
    }
    for (const std::wstring& folder : plan.folders)
        if (used.count(PathKey(folder))) r.folders.push_back(Join(dest, folder));
    if (progress) progress(p);
    return r;
}

// ---------------------------------------------------------------------------

std::wstring DefaultName(uint64_t unixTime) {
    return L"cordinal collection " + FromUtf8(LocalDay((uint32_t)unixTime)) + L".zip";
}

std::wstring SizeText(uint64_t bytes) {
    wchar_t buf[32];
    if (bytes >= (1ull << 30)) swprintf_s(buf, L"%.1f GB", bytes / (double)(1ull << 30));
    else if (bytes >= (1u << 20)) swprintf_s(buf, L"%.0f MB", bytes / (double)(1u << 20));
    else swprintf_s(buf, L"%.0f KB", std::max(1.0, bytes / 1024.0));
    return buf;
}

}  // namespace collection
