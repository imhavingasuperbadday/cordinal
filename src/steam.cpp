// Locates the soundtrack folders Steam keeps on disk: steamapps\music in each library, plus
// soundtrack-looking folders inside game installs. Reads Valve KeyValues text and the platform's
// Steam location, and returns deduplicated, outermost folders only.
#include "steam.h"

#include "library.h"
#include "md5.h"
#include "scanner.h"
#include "streamfile.h"
#include "sort.h"

#include <windows.h>

#include <cctype>
#include <algorithm>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <map>
#include <set>
#ifndef _WIN32
#include <cstdlib>
#endif

namespace {

// Steam writes libraryfolders.vdf in a few KB; anything much bigger is not that file.
constexpr LONGLONG kMaxVdfBytes = 16LL << 20;

// How far below a game's install folder to look for a soundtrack subfolder.
constexpr int kGameScanDepth = 3;
#ifdef _WIN32
constexpr wchar_t kSep = L'\\';
#else
constexpr wchar_t kSep = L'/';
#endif

bool Cancelled(const std::atomic<bool>* cancel) { return cancel && cancel->load(); }

// One token of a KeyValues document: a quoted or bare word, or a block delimiter.
struct VdfToken {
    enum Kind { Str, Open, Close } kind;
    std::string text;
};

// Valve KeyValues text: quoted strings with \\ \" \n \t escapes, and nested { } blocks.
std::vector<VdfToken> Tokenize(const std::string& s) {
    std::vector<VdfToken> tokens;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
            while (i < s.size() && s[i] != '\n') ++i;
        } else if (c == '{') {
            tokens.push_back({VdfToken::Open, {}});
        } else if (c == '}') {
            tokens.push_back({VdfToken::Close, {}});
        } else if (c == '\"') {
            VdfToken t{VdfToken::Str, {}};
            for (++i; i < s.size() && s[i] != '\"'; ++i) {
                if (s[i] == '\\' && i + 1 < s.size()) {
                    char e = s[++i];
                    t.text += e == 'n' ? '\n' : e == 't' ? '\t' : e;  // \\ and \" keep the character itself
                } else {
                    t.text += s[i];
                }
            }
            tokens.push_back(std::move(t));  // an unterminated string runs to the end of the file
        } else if (!std::isspace((unsigned char)c)) {
            VdfToken t{VdfToken::Str, {}};  // unquoted word: not written by Steam, read leniently
            for (; i < s.size() && !std::isspace((unsigned char)s[i]) && s[i] != '{' && s[i] != '}' && s[i] != '\"'; ++i)
                t.text += s[i];
            --i;
            tokens.push_back(std::move(t));
        }
    }
    return tokens;
}

// True for a short run of digits, which is how the old library format numbers its entries.
bool AllDigits(const std::string& s) {
    if (s.empty() || s.size() > 4) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

// Keep a native root separator while removing redundant trailing separators.
std::wstring NormalizePath(std::wstring p) {
#ifdef _WIN32
    for (wchar_t& c : p)
        if (c == L'/') c = L'\\';
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
#else
    while (p.size() > 1 && p.back() == L'/') p.pop_back();
#endif
    return p;
}

// Filesystem names are case-sensitive on Linux.
bool SameFolder(const std::wstring& a, const std::wstring& b) {
#ifdef _WIN32
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
#else
    return a == b;
#endif
}

// True if path sits strictly inside dir (an equal path is not "under").
bool IsUnder(const std::wstring& path, const std::wstring& dir) {
    if (dir.empty() || path.size() <= dir.size() || (path[dir.size()] != kSep && dir.back() != kSep)) return false;
#ifdef _WIN32
    return _wcsnicmp(path.c_str(), dir.c_str(), dir.size()) == 0;
#else
    return path.compare(0, dir.size(), dir) == 0;
#endif
}

#ifdef _WIN32
// A registry REG_SZ, read through the requested registry view (0, KEY_WOW64_64KEY, ...).
std::wstring RegString(HKEY root, const wchar_t* subkey, const wchar_t* name, REGSAM view) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey, 0, KEY_READ | view, &key) != ERROR_SUCCESS) return {};
    DWORD type = 0, bytes = 0;
    std::wstring value;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) == ERROR_SUCCESS && type == REG_SZ &&
        bytes >= sizeof(wchar_t)) {
        value.resize(bytes / sizeof(wchar_t) + 1, L'\0');
        if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(value.data()), &bytes) == ERROR_SUCCESS)
            value.resize(wcslen(value.c_str()));
        else
            value.clear();
    }
    RegCloseKey(key);
    return value;
}

// The Steam install root. Steam does not always write HKCU\...\SteamPath, so fall back through
// the exe it was last run from, the HKLM copies (both registry views and both keys), and finally
// the default location when it looks like a real install.
std::wstring DiscoverSteamRoot() {
    std::wstring p = RegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", 0);
    if (!p.empty()) return NormalizePath(p);

    p = RegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamExe", 0);  // ...\steam.exe
    if (!p.empty()) return NormalizePath(DirectoryOf(p));

    const REGSAM views[] = {0, KEY_WOW64_64KEY, KEY_WOW64_32KEY};
    const wchar_t* keys[] = {L"SOFTWARE\\Valve\\Steam", L"SOFTWARE\\WOW6432Node\\Valve\\Steam"};
    for (const wchar_t* key : keys)
        for (REGSAM view : views)
            if (!(p = RegString(HKEY_LOCAL_MACHINE, key, L"InstallPath", view)).empty()) return NormalizePath(p);

    const std::wstring fallback = L"C:\\Program Files (x86)\\Steam";
    if (GetFileAttributesW((fallback + L"\\steamapps\\libraryfolders.vdf").c_str()) != INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW((fallback + L"\\steamapps").c_str()) != INVALID_FILE_ATTRIBUTES)
        return fallback;
    return {};
}
#else
// Resolve the standard and Flatpak locations, including Steam's ~/.steam/steam symlink.
std::wstring DiscoverSteamRoot() {
    const char* home = std::getenv("HOME");
    const char* data = std::getenv("XDG_DATA_HOME");
    std::vector<std::string> candidates;
    if (data && data[0] == '/') candidates.push_back(std::string(data) + "/Steam");
    if (home && home[0] == '/') {
        candidates.push_back(std::string(home) + "/.local/share/Steam");
        candidates.push_back(std::string(home) + "/.steam/steam");
        candidates.push_back(std::string(home) + "/.var/app/com.valvesoftware.Steam/.local/share/Steam");
    }
    for (const std::string& candidate : candidates) {
        char* resolved = realpath(candidate.c_str(), nullptr);
        if (!resolved) continue;
        std::wstring root = FromUtf8(resolved);
        std::free(resolved);
        DWORD attr = GetFileAttributesW((root + L"/steamapps").c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) return NormalizePath(root);
    }
    return {};
}
#endif

// Reads a whole file into memory, refusing anything larger than a plausible libraryfolders.vdf.
bool ReadFileBytes(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart >= 0 && size.QuadPart <= kMaxVdfBytes;
    if (ok) {
        out.resize((size_t)size.QuadPart);
        DWORD got = 0;
        ok = out.empty() || (ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr) && got == out.size());
    }
    CloseHandle(h);
    return ok;
}

// Steam games can ship thousands of empty audio placeholders. They are not soundtrack content,
// and counting them used to add effects folders to the library.
bool IsAudioContent(const std::wstring& dir, const WIN32_FIND_DATAW& fd) {
    return !(fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
           (fd.nFileSizeHigh || fd.nFileSizeLow >= 4) && IsSupportedAudioFile(dir + kSep + fd.cFileName);
}

bool HasContent(const std::wstring& folder, int depthLeft, const std::atomic<bool>* cancel) {
    if (Cancelled(cancel)) return false;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((folder + kSep + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool any = false;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        any = IsAudioContent(folder, fd);
        if (!any && depthLeft > 0 && (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            any = HasContent(folder + kSep + fd.cFileName, depthLeft - 1, cancel);
    } while (!any && !Cancelled(cancel) && FindNextFileW(h, &fd));
    FindClose(h);
    return any;
}

// The subdirectories of dir, skipping junctions/symlinks so a scan cannot loop or wander.
std::vector<std::wstring> Subdirs(const std::wstring& dir) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + kSep + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) out.push_back(dir + kSep + fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

// A game subfolder counts as a soundtrack when its name says so, or when it is a pile of audio
// (>= 6 files) that outnumbers every other file in it. The name test catches "Soundtrack"/"OST";
// the count test catches "Bonus Tracks" style folders, while a handful of stray SFX do not.
bool LooksLikeSoundtrackDir(const std::wstring& dir, const std::atomic<bool>* cancel) {
    std::wstring name = FileNameOf(dir);
    for (wchar_t& c : name) c = (wchar_t)towlower(c);
    if (name.find(L"soundtrack") != std::wstring::npos || name == L"ost" || name == L"music")
        return HasContent(dir, kGameScanDepth, cancel);
    if (name == L"sfx" || name == L"soundeffects" || name == L"sound effects" || name == L"loadedsoundeffects")
        return false;

    int audio = 0, other = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + kSep + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;  // only files count
        if (IsAudioContent(dir, fd)) ++audio; else ++other;
    } while (!Cancelled(cancel) && FindNextFileW(h, &fd));
    FindClose(h);
    return audio >= 6 && audio > other;
}

// Looks for soundtrack folders inside one game's install folder, down to depthLeft levels.
void ScanGameForSoundtracks(const std::wstring& game, int depthLeft, std::vector<std::wstring>& out,
                            const std::atomic<bool>* cancel) {
    for (const std::wstring& sub : Subdirs(game)) {
        if (Cancelled(cancel)) return;
        if (LooksLikeSoundtrackDir(sub, cancel)) out.push_back(sub);
        if (depthLeft > 1) ScanGameForSoundtracks(sub, depthLeft - 1, out, cancel);
    }
}

// Dedupe and keep only the outermost of any nested pair, so the scanner is
// pointed at the fewest roots that still cover every track (never both music\ and music\Album).
void KeepOutermost(std::vector<std::wstring>& folders) {
    std::vector<std::wstring> unique;
    for (const std::wstring& f : folders) {
        bool seen = false;
        for (const std::wstring& k : unique) seen = seen || SameFolder(k, f);
        if (!seen) unique.push_back(f);
    }
    std::vector<std::wstring> out;
    for (const std::wstring& f : unique) {
        bool nested = false;
        for (const std::wstring& k : unique) nested = nested || IsUnder(f, k);
        if (!nested) out.push_back(f);
    }
    folders = std::move(out);
}

}  // namespace

namespace {

std::string SongText(std::string_view value) {
    std::wstring text = FromUtf8(value);
    for (auto& c : text) c = (wchar_t)towlower(c);
    size_t first = 0, last = text.size();
    while (first < last && iswspace(text[first])) ++first;
    while (last > first && iswspace(text[last - 1])) --last;
    return ToUtf8(text.substr(first, last - first));
}

std::string SongKey(std::string_view title, std::string_view artist, std::string_view album,
                    std::string_view date, unsigned track, unsigned disc) {
    if (title.empty() || artist.empty() || album.empty()) return {};
    return SongText(artist) + '\x1f' + SongText(album) + '\x1f' + SongText(title) + '\x1f' + SongText(date) + '\x1f' +
           std::to_string(track) + ':' + std::to_string(disc);
}

// Untagged WAV/MP3 exports often sit in parallel format folders. Match only within the same
// album directory, with the same filename stem and duration; never guess from a title alone.
std::string FormatPathKey(const std::wstring& path) {
    std::wstring parent = DirectoryOf(path);
    const std::string name = SongText(ToUtf8(FileNameOf(parent)));
    for (const char* format : {"flac", "mp3", "wav", "ogg", "opus", "aac", "m4a", "alac", "lossless"}) {
        if (name == format || name.starts_with(std::string(format) + " ")) {
            parent = DirectoryOf(parent);
            break;
        }
    }
    return SongText(ToUtf8(parent + kSep + StemOf(path)));
}

// Untagged copies need byte evidence, not a filename guess. Read in bounded chunks on the worker.
std::string FileKey(const std::wstring& path, const std::atomic<bool>* cancel) {
    FileSource file(path);
    if (!file.Valid() || !file.Size()) return {};
    Md5 md5;
    std::string block;
    for (uint64_t at = 0; at < file.Size();) {
        if (Cancelled(cancel)) return {};
        size_t count = (size_t)std::min<uint64_t>(65536, file.Size() - at);
        if (!file.Read(at, count, block)) return {};
        md5.Add(block.data(), block.size());
        at += count;
    }
    auto digest = md5.Finish();
    return "file:" + std::string((const char*)digest.data(), digest.size());
}

bool SameSong(const SteamSong& a, const SteamSong& b) {
    if (a.audioHash && a.audioHash == b.audioHash) return true;
    if (!a.key.empty() && a.key == b.key && a.key.compare(0, 5, "file:") == 0) return true;
    if (!a.durationMs || !b.durationMs ||
        (a.durationMs > b.durationMs ? a.durationMs - b.durationMs : b.durationMs - a.durationMs) > 1000) return false;
    if (!a.key.empty() && a.key == b.key) return true;
    return a.codec != b.codec && !a.pathKey.empty() && a.pathKey == b.pathKey &&
           (a.key.empty() || b.key.empty() || a.key.compare(0, 5, "file:") == 0 || b.key.compare(0, 5, "file:") == 0);
}

}  // namespace

std::vector<SteamSong> SnapshotSteamSongs(const Library& library) {
    std::vector<SteamSong> songs;
    for (TrackId id = 0; id < library.Count(); ++id) {
        const auto& t = library[id];
        if (!(t.flags & (Track::kInLibrary | Track::kInbox)) || (t.flags & (Track::kMissing | Track::kExcluded))) continue;
        songs.push_back({library.Path(id), SongKey(library.View(t.title), library.View(t.artist), library.View(t.album),
                                                 library.View(t.date), t.trackNo, t.disc),
                         t.audioHash, t.durationMs, t.codec, FormatPathKey(library.Path(id))});
    }
    return songs;
}

bool PrepareSteamImport(SteamImport& result, std::vector<SteamSong> known, const std::atomic<bool>* cancel) {
    result.oneCopy.clear();
    result.eachFormat.clear();
    result.multipleFormats = false;
    std::vector<SteamSong> songs;
    std::set<Codec> codecs;
    for (const auto& folder : result.folders) {
        std::vector<ScanFile> files;
        if (!ListAudioFiles(folder, files, cancel) || Cancelled(cancel)) return false;
        for (const auto& file : files) {
            if (Cancelled(cancel)) return false;
            TagData tags;
            if (!ReadTags(file.path, tags)) continue;
            SteamSong song{file.path, SongKey(tags.title, tags.artist, tags.album, tags.date, tags.track, tags.disc),
                           tags.audioHash, std::isfinite(tags.duration) ?
                               (uint32_t)std::clamp(tags.duration * 1000, 0.0, 4.0e9) : 0, tags.codec, FormatPathKey(file.path)};
            if (song.key.empty() && !song.audioHash) song.key = FileKey(song.path, cancel);
            codecs.insert(song.codec);
            songs.push_back(std::move(song));
        }
    }
    result.multipleFormats = codecs.size() > 1;
    // Existing Library paths win. Among new files, prefer lossless audio and then a stable path.
    auto quality = [](Codec codec) {
        return codec == Codec::FLAC || codec == Codec::ALAC || codec == Codec::PCM || codec == Codec::Cord ? 1 : 0;
    };
    SortObjects(songs.begin(), songs.end(), [&](const auto& a, const auto& b) {
        return quality(a.codec) != quality(b.codec) ? quality(a.codec) > quality(b.codec) : a.path < b.path;
    });
    for (auto& song : known) {
        if (Cancelled(cancel)) return false;
        if (song.key.empty() && !song.audioHash) song.key = FileKey(song.path, cancel);
    }
    // Prefer an existing music-folder copy over a previously imported Steam copy, even if the
    // Steam copy has the older library id. That also cleans up duplicates from earlier imports.
    std::stable_partition(known.begin(), known.end(), [&](const auto& song) {
        return std::none_of(result.folders.begin(), result.folders.end(), [&](const auto& root) {
            return IsUnder(song.path, root);
        });
    });
    auto collect = [&](bool eachFormat, SteamDuplicates& duplicates) {
        std::vector<SteamSong> keep;
        keep.reserve(known.size() + songs.size());
        std::map<std::string, std::vector<size_t>> keys;
        std::map<uint64_t, std::vector<size_t>> hashes;
        std::map<std::string, std::vector<size_t>> paths;
        auto index = [&](size_t i) {
            if (!keep[i].key.empty()) keys[keep[i].key].push_back(i);
            if (keep[i].audioHash) hashes[keep[i].audioHash].push_back(i);
            if (!keep[i].pathKey.empty()) paths[keep[i].pathKey].push_back(i);
        };
        auto find = [&](const SteamSong& song) {
            size_t match = keep.size();
            auto search = [&](const auto& indices) {
                for (size_t i : indices)
                    if ((!eachFormat || keep[i].codec == song.codec ||
                         (keep[i].codec == Codec::Unknown && !song.key.empty() && song.key == keep[i].key &&
                          song.key.compare(0, 5, "file:") == 0)) && SameSong(song, keep[i])) {
                        match = std::min(match, i);
                        break;
                    }
            };
            if (!song.key.empty()) if (auto it = keys.find(song.key); it != keys.end()) search(it->second);
            if (song.audioHash) if (auto it = hashes.find(song.audioHash); it != hashes.end()) search(it->second);
            if (!eachFormat && !song.pathKey.empty())
                if (auto it = paths.find(song.pathKey); it != paths.end()) search(it->second);
            return match;
        };
        for (auto song : known) {
            if (Cancelled(cancel)) return;
            size_t match = find(song);
            if (match != keep.size()) song.path = keep[match].path;
            keep.push_back(std::move(song));
            index(keep.size() - 1);
        }
        for (const auto& song : songs) {
            if (Cancelled(cancel)) return;
            size_t match = find(song);
            if (match != keep.size()) {
                if (!SameFolder(song.path, keep[match].path)) duplicates.emplace_back(song.path, keep[match].path);
                // A retagged FLAC can link a differently named Library copy to an MP3 export.
                // Keep that metadata as an alias of the retained path for later candidates.
                SteamSong alias = song;
                alias.path = keep[match].path;
                keep.push_back(std::move(alias));
                index(keep.size() - 1);
            } else {
                keep.push_back(song);
                index(keep.size() - 1);
            }
        }
    };
    collect(false, result.oneCopy);
    collect(true, result.eachFormat);
    if (Cancelled(cancel)) { result.oneCopy.clear(); result.eachFormat.clear(); }
    return !Cancelled(cancel);
}

size_t SkipSteamDuplicates(Library& library, const SteamImport& imported, bool eachFormat) {
    auto underSteam = [&](const std::wstring& path) {
        return std::any_of(imported.folders.begin(), imported.folders.end(), [&](const auto& root) {
            return IsUnder(path, root);
        });
    };
    bool restored = false;
    for (TrackId id = 0; id < library.Count(); ++id) {
        auto& track = library[id];
        if ((track.flags & Track::kSteamDuplicate) && underSteam(library.Path(id))) {
            track.flags = (uint16_t)((track.flags | Track::kInLibrary) & ~(Track::kExcluded | Track::kSteamDuplicate));
            restored = true;
        }
    }
    size_t skipped = 0;
    for (const auto& [path, keeper] : eachFormat ? imported.eachFormat : imported.oneCopy) {
        if (GetFileAttributesW(keeper.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        TrackId kept = library.Find(keeper);
        if (kept != kInvalidTrack &&
            (!(library[kept].flags & (Track::kInLibrary | Track::kInbox)) ||
             (library[kept].flags & (Track::kExcluded | Track::kMissing)))) continue;
        if (kept == kInvalidTrack && !underSteam(keeper)) continue;
        TrackId id = library.Add(path);
        if (id == kInvalidTrack || (library[id].flags & Track::kExcluded)) continue;
        library[id].flags = (uint16_t)((library[id].flags | Track::kExcluded | Track::kSteamDuplicate) &
                                      ~(Track::kInLibrary | Track::kInbox));
        ++skipped;
    }
    if (skipped || restored) library.Touch();
    return skipped;
}

std::vector<std::wstring> ParseSteamLibraryFolders(const std::string& vdf) {
    // Walks the tokens as key/value pairs. Values under a "path" key are library folders. The
    // old format has no "path" key: its numbered keys hold the folder directly.
    std::vector<std::wstring> folders;
    auto add = [&](const std::string& value) {
        if (value.find_first_of(":\\/") == std::string::npos) return;
        std::wstring p = FromUtf8(value);
        p = NormalizePath(p);
#ifndef _WIN32
        if (p.empty() || p[0] != L'/') return;
#endif
        if (!p.empty()) folders.push_back(p);
    };
    std::string key;
    bool haveKey = false;
    for (const VdfToken& t : Tokenize(vdf)) {
        if (t.kind != VdfToken::Str) {
            haveKey = false;  // a block opens or closes: the pending key named a block
            continue;
        }
        if (!haveKey) {
            key = t.text;
            haveKey = true;
            continue;
        }
        haveKey = false;
        if (_stricmp(key.c_str(), "path") == 0 || AllDigits(key)) add(t.text);
    }
    return folders;
}

std::vector<std::wstring> FindSteamMusicFoldersIn(const std::wstring& libraryPath, const std::atomic<bool>* cancel) {
    if (Cancelled(cancel)) return {};
    std::wstring lib = NormalizePath(libraryPath);
    if (lib.empty()) return {};

    std::vector<std::wstring> roots;
    // soundtracks that are their own entry live flat under steamapps\music (albums below it).
    std::wstring music = lib + kSep + L"steamapps" + kSep + L"music";
    DWORD attr = GetFileAttributesW(music.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) && HasContent(music, 16, cancel))
        roots.push_back(music);

    // Soundtracks sold as DLC sit inside the game's install folder.
    std::wstring common = lib + kSep + L"steamapps" + kSep + L"common";
    for (const std::wstring& game : Subdirs(common)) {
        if (Cancelled(cancel)) break;
        ScanGameForSoundtracks(game, kGameScanDepth, roots, cancel);
    }

#ifndef _WIN32
    for (std::wstring& root : roots) {
        char* resolved = realpath(ToUtf8(root).c_str(), nullptr);
        if (resolved) { root = FromUtf8(resolved); std::free(resolved); }
    }
#endif
    if (Cancelled(cancel)) return {};
    KeepOutermost(roots);
    return roots;
}

std::vector<std::wstring> FindSteamMusicFolders(const std::atomic<bool>* cancel) {
    if (Cancelled(cancel)) return {};
    std::wstring steam = DiscoverSteamRoot();
    if (steam.empty()) return {};

    // The Steam root is a library too: everything else comes from its libraryfolders.vdf.
    std::vector<std::wstring> libraries{steam};
    std::string vdf;
    if (ReadFileBytes(steam + kSep + L"steamapps" + kSep + L"libraryfolders.vdf", vdf)) {
        for (std::wstring p : ParseSteamLibraryFolders(vdf)) {
#ifndef _WIN32
            char* resolved = realpath(ToUtf8(p).c_str(), nullptr);
            if (resolved) { p = FromUtf8(resolved); std::free(resolved); }
#endif
            bool seen = false;
            for (const std::wstring& known : libraries) seen = seen || SameFolder(known, p);
            if (!seen) libraries.push_back(p);
        }
    }

    std::vector<std::wstring> roots;
    for (const std::wstring& lib : libraries) {
        if (Cancelled(cancel)) break;
        for (const std::wstring& r : FindSteamMusicFoldersIn(lib, cancel)) roots.push_back(r);
    }
    KeepOutermost(roots);
    return roots;
}
