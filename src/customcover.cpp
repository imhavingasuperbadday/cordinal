// Custom covers (customcover.h): the map from track paths to images in the data folder's covers
// folder, its file covers.txt, and the images themselves. covers.txt starts with "1" and holds one
// "<image name>\t<track path>" line per track, in UTF-8. sources.txt holds "<image name>\t<https
// address>" for the images that were downloaded, so the Discord status can show the same picture.
#include "customcover.h"

#include "backup.h"
#include "library.h"
#include "metadata_fetch.h"

#include <windows.h>

#include <cwctype>
#include <map>
#include <mutex>
#include <set>

namespace customcover {

namespace {

struct Entry {
    std::wstring path;  // as the library names the track
    std::wstring file;  // the image's name in the covers folder
};

std::mutex g_mx;
std::wstring g_dir;
bool g_loaded = false;
std::map<std::wstring, Entry> g_entries;        // by Key(path)
std::map<std::wstring, std::string> g_sources;  // image file -> the https address it came from

// An address Discord can render as activity art: https, short enough, and free of anything that would have to
// be escaped in the JSON.
bool UsableUrl(const std::string& url) {
    if (!url.starts_with("https://") || url.size() > 256) return false;
    for (char c : url)
        if (c <= ' ' || c == '"' || c == '\\') return false;  // nothing that would need escaping in the JSON
    return true;
}

// Windows paths compare without case, so a track keeps its cover when the library spells it anew.
std::wstring Key(const std::wstring& path) {
#ifdef _WIN32
    std::wstring key = path;
    for (wchar_t& c : key) c = (wchar_t)std::towlower(c);
    return key;
#else
    return path;
#endif
}

bool Usable(const std::wstring& path) {
    return !path.empty() && path.find_first_of(L"\t\r\n") == std::wstring::npos;
}

bool WriteAtomically(const std::wstring& file, const std::string& bytes) {
    std::wstring temp = file + L".tmp";
    DeleteFileW(temp.c_str());
    if (backup::WriteBytes(temp, bytes) &&
        MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return true;
    DeleteFileW(temp.c_str());
    return false;
}

bool SaveLocked() {
    std::string bytes = "1\n";
    for (const auto& [key, entry] : g_entries) bytes += ToUtf8(entry.file) + '\t' + ToUtf8(entry.path) + '\n';
    return WriteAtomically(g_dir + kPathSep + L"covers.txt", bytes);
}

// The addresses are advisory: they only decide what the Discord status can show, so a failed write costs the
// status's picture and never the cover itself.
void SaveSourcesLocked() {
    std::string bytes = "1\n";
    for (const auto& [key, entry] : g_entries) {
        auto it = g_sources.find(entry.file);
        if (it != g_sources.end()) bytes += ToUtf8(entry.file) + '\t' + it->second + '\n';
    }
    WriteAtomically(g_dir + kPathSep + L"sources.txt", bytes);
}

// Deletes those of `files` that no track uses any more.
void DropUnusedLocked(const std::set<std::wstring>& files) {
    std::set<std::wstring> used;
    for (const auto& [key, entry] : g_entries) used.insert(entry.file);
    for (const std::wstring& file : files)
        if (!used.count(file)) {
            DeleteFileW((g_dir + kPathSep + file).c_str());
            g_sources.erase(file);
        }
}

// The image's name: its content's FNV-1a hash, so one picture given to many tracks is kept once.
std::wstring ImageName(const std::string& image, online::ImageType type) {
    uint64_t h = 14695981039346656037ull;
    for (unsigned char c : image) h = (h ^ c) * 1099511628211ull;
    wchar_t name[32];
    swprintf(name, 32, L"%016llx.%ls", (unsigned long long)h, type == online::ImageType::Png ? L"png" : L"jpg");
    return name;
}

// Caller holds g_mx. An empty map is also what a profile with no covers looks like, so the
// loaded flag is what decides whether covers.txt still has to be read.
void EnsureLoadedLocked() {
    if (g_loaded) return;
    g_loaded = true;
    g_entries.clear();
    g_sources.clear();
    if (g_dir.empty()) return;
    std::string bytes;
    if (!backup::ReadBytes(g_dir + kPathSep + L"covers.txt", bytes, 64u << 20) || !bytes.starts_with("1\n"))
        bytes.clear();
    for (size_t at = 2; at < bytes.size();) {
        size_t end = bytes.find('\n', at);
        if (end == std::string::npos) end = bytes.size();
        std::string_view line(bytes.data() + at, end - at);
        at = end + 1;
        size_t tab = line.find('\t');
        if (tab == line.npos) continue;
        Entry entry{FromUtf8(line.substr(tab + 1)), FromUtf8(line.substr(0, tab))};
        // Only a bare name inside the covers folder, never a path that leads out of it.
        if (!Usable(entry.path) || entry.file.empty() ||
            entry.file.find_first_of(L"/\\:") != std::wstring::npos)
            continue;
        g_entries[Key(entry.path)] = std::move(entry);
    }
    // The addresses are kept separately: a missing or damaged sources.txt only costs the Discord status its
    // picture, and every cover still shows in cordinal.
    std::string sources;
    if (!backup::ReadBytes(g_dir + kPathSep + L"sources.txt", sources, 16u << 20) || !sources.starts_with("1\n"))
        return;
    for (size_t at = 2; at < sources.size();) {
        size_t end = sources.find('\n', at);
        if (end == std::string::npos) end = sources.size();
        std::string_view line(sources.data() + at, end - at);
        at = end + 1;
        const size_t tab = line.find('\t');
        if (tab == line.npos) continue;
        const std::wstring file = FromUtf8(line.substr(0, tab));
        const std::string url(line.substr(tab + 1));
        if (file.empty() || file.find_first_of(L"/\\:") != std::wstring::npos || !UsableUrl(url)) continue;
        g_sources[file] = url;
    }
}

}  // namespace

void Start(const std::wstring& dataDir) {
    std::lock_guard lock(g_mx);
    g_entries.clear();
    g_sources.clear();
    g_loaded = false;
    g_dir = dataDir.empty() ? std::wstring() : dataDir + kPathSep + L"covers";
}

std::wstring ImageFor(const std::wstring& trackPath) {
    std::lock_guard lock(g_mx);
    EnsureLoadedLocked();
    if (g_entries.empty()) return {};
    auto it = g_entries.find(Key(trackPath));
    return it == g_entries.end() ? std::wstring() : g_dir + kPathSep + it->second.file;
}

std::wstring SourceUrlFor(const std::wstring& trackPath) {
    std::lock_guard lock(g_mx);
    EnsureLoadedLocked();
    if (g_entries.empty() || g_sources.empty()) return {};
    auto it = g_entries.find(Key(trackPath));
    if (it == g_entries.end()) return {};
    auto url = g_sources.find(it->second.file);
    return url == g_sources.end() ? std::wstring() : FromUtf8(url->second);
}

void RememberImageUrl(const std::wstring& imagePath, const std::string& url) {
    if (!UsableUrl(url)) return;
    const size_t cut = imagePath.find_last_of(L"/\\");
    const std::wstring file = cut == std::wstring::npos ? imagePath : imagePath.substr(cut + 1);
    if (file.empty() || file.find_first_of(L":") != std::wstring::npos) return;
    std::lock_guard lock(g_mx);
    EnsureLoadedLocked();
    if (g_dir.empty()) return;
    bool used = false;
    for (const auto& [key, entry] : g_entries) used = used || entry.file == file;
    if (!used || g_sources[file] == url) return;
    g_sources[file] = url;
    SaveSourcesLocked();
}

bool Set(const std::vector<std::wstring>& trackPaths, const std::string& image, std::wstring& error,
         const std::string& sourceUrl) {
    int width = 0, height = 0;
    online::ImageType type = online::InspectImage(image, width, height);
    if (type == online::ImageType::None) {
        error = L"the image is not a complete JPEG or PNG file";
        return false;
    }
    std::lock_guard lock(g_mx);
    EnsureLoadedLocked();
    if (g_dir.empty()) {
        error = L"cordinal has no data folder to keep covers in";
        return false;
    }
    CreateDirectoryW(g_dir.c_str(), nullptr);
    const std::wstring file = ImageName(image, type);
    if (!WriteAtomically(g_dir + kPathSep + file, image)) {
        error = L"couldn't save the image in " + g_dir;
        return false;
    }
    auto previous = g_entries;
    auto previousSources = g_sources;
    if (UsableUrl(sourceUrl)) g_sources[file] = sourceUrl;
    else g_sources.erase(file);
    std::set<std::wstring> replaced;
    for (const std::wstring& path : trackPaths) {
        if (!Usable(path)) continue;
        Entry& entry = g_entries[Key(path)];
        if (!entry.file.empty() && entry.file != file) replaced.insert(entry.file);
        entry = {path, file};
    }
    if (!SaveLocked()) {
        g_entries = std::move(previous);
        g_sources = std::move(previousSources);
        DropUnusedLocked({file});
        error = L"couldn't save the list of custom covers in " + g_dir;
        return false;
    }
    replaced.insert(file);  // when no path could take it
    DropUnusedLocked(replaced);
    SaveSourcesLocked();
    return true;
}

size_t Clear(const std::vector<std::wstring>& trackPaths) {
    std::lock_guard lock(g_mx);
    EnsureLoadedLocked();
    auto previous = g_entries;
    std::set<std::wstring> removed;
    for (const std::wstring& path : trackPaths) {
        auto it = g_entries.find(Key(path));
        if (it == g_entries.end()) continue;
        removed.insert(it->second.file);
        g_entries.erase(it);
    }
    if (previous.size() == g_entries.size()) return 0;
    if (!SaveLocked()) {
        g_entries = std::move(previous);
        return 0;
    }
    DropUnusedLocked(removed);
    SaveSourcesLocked();
    return previous.size() - g_entries.size();
}

size_t Count(const std::vector<std::wstring>& trackPaths) {
    std::lock_guard lock(g_mx);
    EnsureLoadedLocked();
    size_t n = 0;
    if (!g_entries.empty())
        for (const std::wstring& path : trackPaths) n += g_entries.count(Key(path));
    return n;
}

}  // namespace customcover
