// Backup folders are published only when complete. Restore builds a sibling profile, then swaps
// directories after shutdown; a durable journal lets startup roll back an interrupted swap.
#include "backup.h"
#include "inflate.h"
#include "streamfile.h"
#include <algorithm>
#include <charconv>
#include <set>

namespace backup {
namespace {
struct Error {
    std::wstring text;
};
void Require(bool ok, const char* message) {
    if (!ok) throw Error{FromUtf8(message)};
}
std::wstring Child(const std::wstring& path, const wchar_t* name) {
    std::wstring part = name;
    for (auto& c : part)
        if (c == L'/' || c == L'\\') c = kPathSep;
    return path + kPathSep + part;
}
std::wstring Unique(const std::wstring& path, const wchar_t* suffix) {
    return path + suffix + std::to_wstring(UnixNow()) + L"-" + std::to_wstring(GetCurrentProcessId());
}
bool Exists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}
bool Plain(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_REPARSE_POINT);
}
bool Inside(const std::wstring& path, const std::wstring& root) {
    auto a = NormalizeMediaPath(path), b = NormalizeMediaPath(root);
    if (b.empty() || a.size() < b.size() ||
        CompareStringOrdinal(a.data(), (int)b.size(), b.data(), (int)b.size(), TRUE) != CSTR_EQUAL)
        return false;
    return a.size() == b.size() || b.back() == L'\\' || b.back() == L'/' || a[b.size()] == L'\\' ||
           a[b.size()] == L'/';
}
std::wstring RealDirectory(const std::wstring& path) {
#ifdef _WIN32
    HANDLE h = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    wchar_t full[32768];
    DWORD n = GetFinalPathNameByHandleW(h, full, (DWORD)std::size(full), 0);
    CloseHandle(h);
    return n && n < std::size(full) ? std::wstring(full, n) : std::wstring();
#else
    auto text = ToUtf8(path);
    char* resolved = realpath(text.c_str(), nullptr);
    std::wstring result = resolved ? FromUtf8(resolved) : L"";
    free(resolved);
    return result;
#endif
}
void Make(const std::wstring& path) {
    Require(CreateDirectoryW(path.c_str(), nullptr) != FALSE, "Couldn't create a backup folder.");
}
void Move(const std::wstring& from, const std::wstring& to) {
    // Antivirus and indexers can briefly hold a newly closed file without delete sharing.
    // Retry only these transient locks, on the worker or after shutdown, never while playing.
    for (unsigned attempt = 0;; ++attempt) {
        if (MoveFileExW(from.c_str(), to.c_str(), 0)) return;
        DWORD error = GetLastError();
        if (attempt == 7 || (error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION))
            throw Error{L"Couldn't rename " + from + L" to " + to + L" (Windows error " +
                        std::to_wstring(error) + L")."};
        Sleep(std::min(25u << attempt, 200u));
    }
}
struct Finder {
    HANDLE handle;
    ~Finder() {
        if (handle != INVALID_HANDLE_VALUE) FindClose(handle);
    }
};
void Cancelled(const std::atomic<bool>& cancel) {
    Require(!cancel.load(), "Cancelled. No files were replaced.");
}
template <class F> void Children(const std::wstring& path, F visit) {
    WIN32_FIND_DATAW fd{};
    Finder found{FindFirstFileW(Child(path, L"*").c_str(), &fd)};
    if (found.handle == INVALID_HANDLE_VALUE) {
        Require(GetLastError() == ERROR_FILE_NOT_FOUND, "Couldn't read a backup folder.");
        return;
    }
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        visit(Child(path, fd.cFileName), fd.cFileName, fd.dwFileAttributes);
    } while (FindNextFileW(found.handle, &fd));
    Require(GetLastError() == ERROR_NO_MORE_FILES, "Couldn't finish reading a backup folder.");
}
void RemoveTree(const std::wstring& path) {
    auto attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return;
    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        if (!(attr & FILE_ATTRIBUTE_REPARSE_POINT))
            Children(path, [](const auto& child, const wchar_t*, DWORD) { RemoveTree(child); });
        RemoveDirectoryW(path.c_str());
    } else
        DeleteFileW(path.c_str());
}
void Discard(const std::wstring& path) {
    try {
        RemoveTree(path);
    } catch (...) {
    }
}
void CopyTree(const std::wstring& from, const std::wstring& to, const std::atomic<bool>& cancel) {
    Cancelled(cancel);
    Require(Plain(from), "A backup asset is a link. Copy its real files into the profile before backing up.");
    if (GetFileAttributesW(from.c_str()) & FILE_ATTRIBUTE_DIRECTORY) {
        Make(to);
        Children(from, [&](const auto& child, const wchar_t* name, DWORD) {
            CopyTree(child, Child(to, name), cancel);
        });
    } else
        Require(CopyFileW(from.c_str(), to.c_str(), TRUE) != FALSE, "Couldn't copy a profile file.");
}
void Extras(const std::wstring& from, const std::wstring& to, const std::atomic<bool>& cancel) {
    for (const wchar_t* name : {L"packs", L"soundfonts", L"tagundo", L"organize", L"convert", L"radio"}) {
        auto path = Child(from, name);
        if (Exists(path)) CopyTree(path, Child(to, name), cancel);
    }
}
void Index(const std::wstring& folder, const std::wstring& prefix, std::string& index,
           std::set<std::string>* names) {
    Children(folder, [&](const auto& path, const wchar_t* name, DWORD attr) {
        Require(!(attr & FILE_ATTRIBUTE_REPARSE_POINT), "A backup contains a link.");
        auto relative = prefix + name;
        if (attr & FILE_ATTRIBUTE_DIRECTORY)
            Index(path, relative + L"/", index, names);
        else if (names) {
            auto key = ToUtf8(relative);
            Require(key == "files.txt" || names->contains(key), "The backup contains an unlisted file.");
        } else {
            std::string bytes;
            Require(ReadBytes(path, bytes, 1ull << 31), "Couldn't read back a backup file.");
            auto key = ToUtf8(relative);
            Require(key.find_first_of("\r\n") == key.npos, "A filename contains a newline.");
            index += std::to_string(Crc32((const unsigned char*)bytes.data(), bytes.size())) + " " +
                     std::to_string(bytes.size()) + " " + key + "\n";
        }
    });
}
bool SafeRelative(std::string_view name) {
    if (name.empty() || name.find_first_of("\\:\r\n") != name.npos || name.find('\0') != name.npos ||
        name[0] == '/')
        return false;
    while (!name.empty()) {
        auto end = std::min(name.find('/'), name.size());
        auto part = name.substr(0, end);
        if (part.empty() || part == "." || part == "..") return false;
        name.remove_prefix(std::min(end + 1, name.size()));
    }
    return true;
}
void Verify(const std::wstring& folder) {
    std::string bytes;
    Require(ReadBytes(Child(folder, L"files.txt"), bytes, 16 * 1024 * 1024),
            "The backup has no readable files.txt.");
    std::string_view lines = bytes;
    Require(lines.starts_with("cordinal-files 1\n"), "The backup file index has an unknown version.");
    lines.remove_prefix(17);
    std::set<std::string> seen;
    while (!lines.empty()) {
        auto end = lines.find('\n');
        Require(end != lines.npos, "The backup file index is truncated.");
        auto line = lines.substr(0, end);
        lines.remove_prefix(end + 1);
        auto one = line.find(' '), two = one == line.npos ? one : line.find(' ', one + 1);
        Require(two != line.npos, "The backup file index is invalid.");
        uint32_t crc = 0;
        uint64_t size = 0;
        auto a = std::from_chars(line.data(), line.data() + one, crc);
        auto b = std::from_chars(line.data() + one + 1, line.data() + two, size);
        std::string name(line.substr(two + 1));
        Require(a.ec == std::errc() && a.ptr == line.data() + one && b.ec == std::errc() &&
                    b.ptr == line.data() + two && SafeRelative(name) && seen.insert(name).second,
                "The backup file index contains an invalid or duplicate path.");
        auto path = Child(folder, FromUtf8(name).c_str());
        for (auto at = path; at != folder && !at.empty(); at = DirectoryOf(at))
            Require(Plain(at), "The backup contains a linked file or folder.");
        std::string content;
        Require(ReadBytes(path, content, 1ull << 31) && content.size() == size &&
                    Crc32((const unsigned char*)content.data(), content.size()) == crc,
                "A backup file is missing or damaged.");
    }
    Require(seen.contains("profile/settings.ini"), "The backup has no settings.");
    std::string ignored;
    Index(folder, L"", ignored, &seen);
}
bool Failed(const Error& e, std::wstring& error) {
    error = L"Couldn't finish the backup or restore: " + e.text;
    return false;
}
} // namespace

bool ReadBytes(const std::wstring& file, std::string& bytes, uint64_t max) {
    FileSource input(file);
    return input.Valid() && input.Size() <= max && input.Read(0, (size_t)input.Size(), bytes);
}
bool WriteBytes(const std::wstring& file, std::string_view bytes) {
    HANDLE h =
        CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    for (size_t at = 0; at < bytes.size() && ok;) {
        DWORD n = (DWORD)std::min<size_t>(65536, bytes.size() - at), written = 0;
        ok = WriteFile(h, bytes.data() + at, n, &written, nullptr) && written == n;
        at += written;
    }
    ok = ok && FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) DeleteFileW(file.c_str());
    return ok;
}

bool Create(const std::wstring& folder, const std::wstring& profile, const std::string& settings,
            const LibraryArchive& data, const std::atomic<bool>& cancel, std::wstring& error) {
    std::wstring stage;
    bool created = false;
    try {
        auto parent = RealDirectory(DirectoryOf(folder)), source = RealDirectory(profile);
        Require(!parent.empty() && !source.empty(), "Couldn't resolve the profile or backup destination.");
        Require(!Inside(Child(parent, FileNameOf(folder).c_str()), source),
                "Choose a backup folder outside the active profile.");
        Require(!Exists(folder), "That backup folder already exists. Choose a new name.");
        stage = Unique(folder, L".incomplete-");
        Make(stage);
        created = true;
        Make(Child(stage, L"profile"));
        Make(Child(stage, L"playlists"));
        for (const auto& f : migration::Export(data)) {
            Cancelled(cancel);
            Require(WriteBytes(Child(stage, f.path.c_str()), f.bytes), "Couldn't write a backup file.");
        }
        Require(WriteBytes(Child(stage, L"profile/settings.ini"), settings), "Couldn't save settings.");
        Require(WriteBytes(Child(stage, L"profile-path.txt"), ToUtf8(profile)),
                "Couldn't record the original profile location.");
        if (!WriteLibraryArchive(Child(stage, L"profile/library.cdb"), data, error)) throw Error{error};
        Extras(profile, Child(stage, L"profile"), cancel);
        const std::string help =
            "cordinal backup, format 1\r\n\r\n"
            "Restore with File > Restore backup, then choose this folder. Music files are not included.\r\n"
            "Choose new music folder locations in the restore window. Missing files keep their "
            "statistics.\r\n"
            "statistics.csv and history.csv are UTF-8 CSV; dates are ISO 8601 UTC, ratings 0..100 (20 per "
            "star).\r\n"
            "Formula-like titles in statistics.csv start with an apostrophe for spreadsheet safety.\r\n"
            "playlists.csv names the numbered M3U8 files in playlists/. Other players can open ordinary "
            "tracks.\r\n"
            "CUE/game subtracks retain cordinal's file|index notation; CSV keeps their full identity.\r\n"
            "Auto-playlist rules are preserved for cordinal; M3U8 files hold their current contents.\r\n"
            "library.csv and the other CSV tables are the versioned restore contract. profile/library.cdb\r\n"
            "is an additional native copy. files.txt checks every file's byte length and CRC-32.\r\n"
            "Keep all files together, or zip this folder. Windows and Linux have different settings "
            "formats.\r\n"
            "Settings, packs, soundfonts and file-edit undo journals are included. Browser devices must "
            "pair\r\n"
            "again, and ListenBrainz must reconnect on the new computer. Caches and credentials are "
            "omitted.\r\n"
            "A restore retains your previous profile in a sibling .before-restore folder.\r\n";
        Require(WriteBytes(Child(stage, L"README.txt"), help), "Couldn't write backup instructions.");
        std::string index = "cordinal-files 1\n";
        Index(stage, L"", index, nullptr);
        Require(WriteBytes(Child(stage, L"files.txt"), index), "Couldn't write the backup file index.");
        Cancelled(cancel);
        Move(stage, folder);
        return true;
    } catch (const Error& e) {
        if (created) Discard(stage);
        return Failed(e, error);
    }
}

bool Read(const std::wstring& folder, LibraryArchive& data, std::string& settings, std::wstring& error) {
    try {
        Require(Plain(folder), "Choose a backup folder containing real files.");
        Verify(folder);
        std::vector<migration::File> files;
        for (const wchar_t* name :
             {L"backup.ini", L"library.csv", L"history.csv", L"playlists.csv", L"playlist-items.csv",
              L"views.csv", L"albums.csv", L"points.csv", L"points-days.csv", L"points-recent.csv"}) {
            migration::File f{name, {}};
            Require(ReadBytes(Child(folder, name), f.bytes), "Couldn't read a backup table.");
            files.push_back(std::move(f));
        }
        std::string ini;
        Require(ReadBytes(Child(folder, L"profile/settings.ini"), ini, 16 * 1024 * 1024),
                "Couldn't read backup settings.");
        Require(
            (ini.size() >= 2 && (uint8_t)ini[0] == 255 && (uint8_t)ini[1] == 254 && ini.size() % 2 == 0) ||
                ini.starts_with("# cordinal settings\n"),
            "The backup settings file has an unknown encoding.");
        if (!migration::Import(files, data, error)) return false;
        settings = std::move(ini);
        return true;
    } catch (const Error& e) {
        return Failed(e, error);
    }
}

bool Stage(const std::wstring& folder, const std::wstring& profile, const std::string& settings,
           const LibraryArchive& data, const std::atomic<bool>& cancel, std::wstring& stage,
           std::wstring& error) {
    std::wstring temp;
    bool created = false;
    try {
        Verify(folder);
        temp = Unique(profile, L".restoring-");
        Make(temp);
        created = true;
        Extras(Child(folder, L"profile"), temp, cancel);
        Require(WriteBytes(Child(temp, L"settings.ini"), settings), "Couldn't stage settings.");
        if (!WriteLibraryArchive(Child(temp, L"library.cdb"), data, error)) throw Error{error};
        Cancelled(cancel);
        stage = temp;
        return true;
    } catch (const Error& e) {
        if (created) Discard(temp);
        return Failed(e, error);
    }
}

bool Recover(const std::wstring& profile, std::wstring& error) {
    try {
        auto journal = profile + L".restore-journal";
        if (!Exists(journal)) return true;
        std::string bytes;
        Require(ReadBytes(journal, bytes, 65536), "Couldn't read the restore recovery journal.");
        auto split = bytes.find('\n');
        Require(split != bytes.npos, "Invalid restore recovery journal.");
        auto stage = FromUtf8(bytes.substr(0, split)), previous = FromUtf8(bytes.substr(split + 1));
        Require(stage.starts_with(profile + L".restoring-") &&
                    previous.starts_with(profile + L".before-restore-") &&
                    DirectoryOf(stage) == DirectoryOf(profile) &&
                    DirectoryOf(previous) == DirectoryOf(profile),
                "The restore recovery journal contains invalid paths.");
        if (!Exists(profile)) {
            Require(Exists(previous), "Restore recovery needs the original profile folder.");
            Move(previous, profile);
        }
        Require(DeleteFileW(journal.c_str()) != FALSE, "Couldn't clear the restore recovery journal.");
        return true;
    } catch (const Error& e) {
        return Failed(e, error);
    }
}

bool Commit(const std::wstring& profile, const std::wstring& stage, std::wstring& previous,
            std::wstring& error) {
    try {
        Require(DirectoryOf(stage) == DirectoryOf(profile) && stage.starts_with(profile + L".restoring-") &&
                    Plain(stage) && Plain(profile),
                "The staged profile is invalid.");
        previous = Unique(profile, L".before-restore-");
        auto journal = profile + L".restore-journal";
        Require(WriteBytes(journal, ToUtf8(stage) + "\n" + ToUtf8(previous)),
                "Couldn't create the restore recovery journal.");
        Move(profile, previous);
        try {
            Move(stage, profile);
        } catch (...) {
            Move(previous, profile);
            DeleteFileW(journal.c_str());
            throw;
        }
        DeleteFileW(journal.c_str());
        return true;
    } catch (const Error& e) {
        return Failed(e, error);
    }
}

std::string RemapSettings(const std::string& bytes, const std::vector<migration::PathMap>& maps) {
    bool utf16 = bytes.size() >= 2 && (uint8_t)bytes[0] == 255 && (uint8_t)bytes[1] == 254;
    std::wstring text, out;
    if (utf16) {
        for (size_t i = 2; i + 1 < bytes.size(); i += 2) {
            uint32_t c = (uint8_t)bytes[i] | (uint8_t)bytes[i + 1] << 8;
#ifndef _WIN32
            if (c >= 0xD800 && c <= 0xDBFF && i + 3 < bytes.size()) {
                uint32_t low = (uint8_t)bytes[i + 2] | (uint8_t)bytes[i + 3] << 8;
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    c = 0x10000 + ((c - 0xD800) << 10) + low - 0xDC00;
                    i += 2;
                }
            }
#endif
            text += (wchar_t)c;
        }
    } else
        text = FromUtf8(bytes);
    for (size_t at = 0; at < text.size();) {
        auto end = std::min(text.find(L'\n', at), text.size());
        auto line = text.substr(at, end - at);
        auto equals = line.find(L'=');
        if (equals != line.npos) {
            auto value = line.substr(equals + 1);
            bool cr = !value.empty() && value.back() == L'\r';
            if (cr) value.pop_back();
            std::wstring remapped;
            // The library folder setting is a pipe-separated list. A subtrack's numeric
            // suffix is also preserved because only complete folder prefixes are replaced.
            for (size_t pos = 0; pos <= value.size();) {
                auto bar = std::min(value.find(L'|', pos), value.size());
                remapped += migration::RemapPath(value.substr(pos, bar - pos), maps);
                if (bar < value.size()) remapped += L'|';
                pos = bar + 1;
            }
            line = line.substr(0, equals + 1) + remapped + (cr ? L"\r" : L"");
        }
        out += line;
        if (end < text.size()) out += L'\n';
        at = end + 1;
    }
    if (!utf16) return ToUtf8(out);
    std::string result = "\xFF\xFE";
    auto unit = [&](uint32_t c) {
        result += (char)c;
        result += (char)(c >> 8);
    };
    for (uint32_t c : out) {
        if (c > 0xFFFF) {
            c -= 0x10000;
            unit(0xD800 + (c >> 10));
            unit(0xDC00 + (c & 1023));
        } else
            unit(c);
    }
    return result;
}
} // namespace backup
