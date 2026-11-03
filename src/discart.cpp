// Finding disc art beside the music (discart.h).
#include "discart.h"

#include "cue.h"
#include "library.h"

#include <windows.h>

#include <cwctype>
#include <vector>

namespace {

// The .png names in `dir`, lowercase.
std::vector<std::wstring> PngNames(const std::wstring& dir) {
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + kPathSep + L"*.png").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return names;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring name = fd.cFileName;
        for (auto& c : name) c = (wchar_t)std::towlower(c);
        names.push_back(std::move(name));
    } while (FindNextFileW(h, &fd) && names.size() < 4096);
    FindClose(h);
    return names;
}

// The name as it is on disk, for a lowercase `wanted`.
std::wstring Spelled(const std::wstring& dir, const std::wstring& wanted) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + kPathSep + L"*.png").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::wstring found;
    do {
        std::wstring name = fd.cFileName, lower = name;
        for (auto& c : lower) c = (wchar_t)std::towlower(c);
        if (lower == wanted) found = dir + kPathSep + name;
    } while (found.empty() && FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

}  // namespace

std::wstring FindDiscArt(const std::wstring& mediaPath, unsigned disc, unsigned discTotal) {
    std::wstring folder = DirectoryOf(AudioFileOf(mediaPath));
    if (folder.empty()) return {};
    std::wstring parent = DirectoryOf(folder);
    std::vector<std::wstring> dirs{folder, folder + kPathSep + L"Artwork", folder + kPathSep + L"Scans"};
    if (!parent.empty() && parent != folder) {
        dirs.push_back(parent + kPathSep + L"Artwork");
        dirs.push_back(parent + kPathSep + L"Scans");
    }
    std::vector<std::wstring> wanted;
    if (disc > 0 && discTotal > 1) {
        wanted.push_back(L"cd" + std::to_wstring(disc) + L".png");
        wanted.push_back(L"disc" + std::to_wstring(disc) + L".png");
    }
    for (const wchar_t* name : {L"cd.png", L"disc.png", L"vinyl.png"}) wanted.push_back(name);
    for (const auto& dir : dirs) {
        auto names = PngNames(dir);
        for (const auto& want : wanted)
            for (const auto& name : names)
                if (name == want) return Spelled(dir, want);
    }
    return {};
}

int DiscCoverSide(int side) { return side >= 40 ? side * 82 / 100 : 0; }
