// Organize's filesystem logic, kept free of UI: work out a destination for each track from its
// tags (PlanOrganize), carry out a plan with cover images and a running undo log (ExecuteOrganize),
// and reverse the newest log (UndoLastOrganize). The library is never touched here; callers rename
// their tracks with the results.
#include "organize.h"

#include "cue.h"
#include "titleformat.h"

#include <shlobj.h>

#include <algorithm>
#include <map>
#include <unordered_set>

namespace {

// Windows paths compare case-insensitively, so every "same file / already planned" test lowers both sides.
std::wstring LowerPath(std::wstring s) {
    CharLowerBuffW(s.data(), (DWORD)s.size());
    return s;
}

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::wstring ErrorText(DWORD code) {
    wchar_t buf[512] = {};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, buf, (DWORD)std::size(buf), nullptr);
    std::wstring s = buf;
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ')) s.pop_back();
    return s;
}

bool IsImage(const std::wstring& path) {
    std::wstring ext = ExtOf(path);
    return ext == L"jpg" || ext == L"jpeg" || ext == L"png" || ext == L"gif" || ext == L"bmp" || ext == L"webp";
}

// Removes `dir` and its parents while they are empty, stopping at any folder in `keep`.
void RemoveEmptyFolders(std::wstring dir, const std::vector<std::wstring>& keep) {
    while (dir.size() > 3) {
        std::wstring lower = LowerPath(dir);
        if (std::any_of(keep.begin(), keep.end(), [&](const std::wstring& k) { return LowerPath(k) == lower; })) return;
        if (!RemoveDirectoryW(dir.c_str())) return;  // not empty (or not ours to remove)
        dir = DirectoryOf(dir);
    }
}

bool AppendLog(HANDLE log, const std::wstring& to, const std::wstring& from, uint64_t* before = nullptr) {
    if (before) {
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(log, &size) || size.QuadPart < 0) return false;
        *before = (uint64_t)size.QuadPart;
    }
    std::string line = ToUtf8(to) + "\t" + ToUtf8(from) + "\r\n";
    DWORD written;
    return WriteFile(log, line.data(), (DWORD)line.size(), &written, nullptr) && written == line.size() && FlushFileBuffers(log);
}

// Drops the last intent line after its move failed, so a later undo doesn't mistake an untouched
// source for a half-done move.
bool TruncateLog(HANDLE log, uint64_t size) {
    LARGE_INTEGER at{};
    at.QuadPart = (LONGLONG)size;
    return SetFilePointerEx(log, at, nullptr, FILE_BEGIN) && SetEndOfFile(log) && FlushFileBuffers(log);
}

}  // namespace

// Turns one pattern segment into a legal Windows path component: reserved characters and control
// codes become '_', leading spaces and trailing dots/spaces are dropped, and the length is capped.
std::wstring SafePathComponent(const std::wstring& s, size_t maxLen) {
    std::wstring r;
    for (wchar_t c : s) r += (c < 32 || wcschr(L"<>:\"/\\|?*", c)) ? L'_' : c;
    size_t b = r.find_first_not_of(L' ');
    r = b == std::wstring::npos ? std::wstring() : r.substr(b);
    if (r.size() > maxLen) r.resize(maxLen);
    while (!r.empty() && (r.back() == L'.' || r.back() == L' ')) r.pop_back();
    // Reserved device names, with or without an extension.
    std::wstring stem = LowerPath(r.substr(0, r.find(L'.')));
    for (const wchar_t* bad : {L"con", L"prn", L"aux", L"nul", L"com1", L"com2", L"com3", L"com4", L"com5", L"com6", L"com7",
                               L"com8", L"com9", L"lpt1", L"lpt2", L"lpt3", L"lpt4", L"lpt5", L"lpt6", L"lpt7", L"lpt8", L"lpt9"})
        if (stem == bad) {
            if (maxLen <= stem.size()) r.back() = L'_';
            else r.insert(stem.size(), 1, L'_');
            if (r.size() > maxLen) r.resize(maxLen);
            return r;
        }
    return r;
}

std::vector<std::wstring> PatternParts(TrackId id, const std::wstring& pattern) {
    // Tag values can't add folder levels: their slashes become dashes before the pattern's own
    // backslashes split the result into folders.
    std::wstring rel = FormatTitle(pattern, [id](const std::wstring& name, std::wstring& out) {
        bool ok = LookupField(id, name, out);
        for (auto& c : out)
            if (c == L'\\' || c == L'/') c = L'-';
        return ok;
    });
    std::vector<std::wstring> parts;
    for (size_t start = 0; start <= rel.size();) {
        size_t slash = rel.find(L'\\', start);
        if (slash == std::wstring::npos) slash = rel.size();
        std::wstring part = SafePathComponent(rel.substr(start, slash - start));
        if (!part.empty()) parts.push_back(part);
        start = slash + 1;
    }
    if (parts.empty()) parts.push_back(SafePathComponent(StemOf(AudioFileOf(g_lib.Path(id)))));
    return parts;
}

// Pure: builds a target path for every track from `pattern` and reserves a free name for each,
// appending " (2)", " (3)", ... on collision. Anything that can't move (CUE subtrack, already in
// place, name taken) gets a human reason in `skip`. Touches no files except existence checks.
std::vector<OrganizeMove> PlanOrganize(const std::vector<TrackId>& ids, const std::wstring& rootIn, const std::wstring& pattern) {
    std::wstring root = rootIn;
    // Normalise to exactly one trailing backslash, keeping a bare drive like "C:\" intact.
    while (root.size() > 3 && (root.back() == L'\\' || root.back() == L'/')) root.pop_back();
    if (!root.empty() && root.back() != kPathSep) root += kPathSep;
    std::vector<OrganizeMove> plan;
    std::unordered_set<std::wstring> taken;
    for (TrackId id : ids) {
        OrganizeMove m;
        m.id = id;
        m.from = g_lib.Path(id);
        if (IsSubtrack(m.from)) {
            // Several songs share one file, which can't move per song.
            m.skip = L"inside a CUE sheet";
            plan.push_back(std::move(m));
            continue;
        }
        std::vector<std::wstring> parts = PatternParts(id, pattern);
        std::wstring ext = ExtOf(m.from), dir = root;
        for (size_t i = 0; i + 1 < parts.size(); ++i) dir += parts[i] + kPathSep;
        std::wstring name = parts.back();
        // Stay under MAX_PATH: shorten the file name first.
        size_t budget = 250 - std::min<size_t>(250, dir.size() + ext.size() + 6);
        if (name.size() > budget) name.resize(std::min(name.size(), std::max<size_t>(budget, 8)));
        m.to = dir + name + L"." + ext;
        if (LowerPath(m.to) == LowerPath(m.from)) {
            m.skip = L"already in place";
        } else {
            for (int n = 2; (taken.count(LowerPath(m.to)) || Exists(m.to)) && n <= 99; ++n)
                m.to = dir + name + L" (" + std::to_wstring(n) + L")." + ext;
            if (taken.count(LowerPath(m.to)) || Exists(m.to)) m.skip = L"a file with that name already exists";
        }
        taken.insert(LowerPath(m.to));
        plan.push_back(std::move(m));
    }
    return plan;
}

// Runs off the UI thread; the caller has normally stopped the folder watcher so the moves aren't
// mistaken for files appearing and vanishing. Each move is logged before any folder cleanup, so
// even a run killed halfway can be undone. Keeps the library untouched.
OrganizeResult ExecuteOrganize(const std::vector<OrganizeMove>& plan, const std::wstring& logDir, const std::vector<std::wstring>& keep) {
    OrganizeResult r;
    SHCreateDirectoryExW(nullptr, logDir.c_str(), nullptr);
    SYSTEMTIME st;
    GetLocalTime(&st);
    static std::atomic<unsigned> serial{0};
    wchar_t name[96];
    swprintf_s(name, L"%c%04u%02u%02u-%02u%02u%02u-%03u-%010u-%010u.log", kPathSep, st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentProcessId(), ++serial);
    r.log = logDir + name;
    // Written as we go, so even an interrupted run can be undone.
    HANDLE log = CreateFileW(r.log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) {
        std::wstring error = L"Couldn't create the undo log: " + ErrorText(GetLastError());
        for (const auto& m : plan) if (m.skip.empty()) r.failed.emplace_back(m.from, error);
        r.log.clear();
        return r;
    }
    std::map<std::wstring, std::vector<std::wstring>> destsBySource;  // source folder -> destination folders
    std::map<std::wstring, std::wstring> sourceCase;
    size_t stoppedAt = plan.size();
    for (size_t i = 0; i < plan.size(); ++i) {
        const OrganizeMove& m = plan[i];
        if (!m.skip.empty()) {
            destsBySource[LowerPath(DirectoryOf(m.from))].push_back(L"");  // something stays behind
            continue;
        }
        SHCreateDirectoryExW(nullptr, DirectoryOf(m.to).c_str(), nullptr);
        // A destination created after the plan was made wins: never replace it.
        if (Exists(m.to)) {
            r.failed.emplace_back(m.from, L"A file with that name appeared before the move.");
            destsBySource[LowerPath(DirectoryOf(m.from))].push_back(L"");
            continue;
        }
        // The undo record goes first: a crash between the move and the log write would otherwise
        // leave a file that Library > Undo last organize cannot bring back.
        uint64_t logBefore = 0;
        if (!AppendLog(log, m.to, m.from, &logBefore)) {
            r.failed.emplace_back(m.from, L"Couldn't save the undo log; the file was not moved.");
            destsBySource[LowerPath(DirectoryOf(m.from))].push_back(L"");
            stoppedAt = i + 1;
            break;  // stop before risking any further moves without an undo record
        }
        if (MoveFileExW(m.from.c_str(), m.to.c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)) {
            r.moved.emplace_back(m.id, m.to);
            destsBySource[LowerPath(DirectoryOf(m.from))].push_back(DirectoryOf(m.to));
            sourceCase[LowerPath(DirectoryOf(m.from))] = DirectoryOf(m.from);
        } else {
            // Nothing reached the destination (a partial cross-volume copy is kept in the log so
            // the user can still undo it).
            DWORD error = GetLastError();
            if (!Exists(m.to)) TruncateLog(log, logBefore);
            r.failed.emplace_back(m.from, ErrorText(error));
            destsBySource[LowerPath(DirectoryOf(m.from))].push_back(L"");
        }
    }
    // The run stopped early; report every file it did not reach, so the summary can't look complete.
    if (stoppedAt < plan.size())
        for (size_t i = stoppedAt; i < plan.size(); ++i)
            if (plan[i].skip.empty()) r.failed.emplace_back(plan[i].from, L"Not moved: the undo log couldn't be written.");
    // Cover images follow when every audio file of a folder went to the same new folder.
    for (auto& [lower, dests] : destsBySource) {
        auto src = sourceCase.find(lower);
        if (src == sourceCase.end()) continue;
        bool together = std::all_of(dests.begin(), dests.end(), [&](const std::wstring& d) { return !d.empty() && LowerPath(d) == LowerPath(dests[0]); });
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((src->second + kPathSep + L"*").c_str(), &fd);
        bool audioLeft = false;
        std::vector<std::wstring> images;
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                std::wstring file = src->second + kPathSep + fd.cFileName;
                if (IsSupportedAudioFile(file)) audioLeft = true;
                else if (IsImage(file)) images.push_back(fd.cFileName);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        if (together && !audioLeft)
            for (auto& img : images) {
                std::wstring from = src->second + kPathSep + img, to = dests[0] + kPathSep + img;
                if (Exists(to)) continue;
                uint64_t logBefore = 0;
                if (!AppendLog(log, to, from, &logBefore)) {
                    r.failed.emplace_back(from, L"Couldn't save the undo log; the cover was not moved.");
                    continue;
                }
                if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)) {
                    DWORD error = GetLastError();
                    if (!Exists(to)) TruncateLog(log, logBefore);
                    r.failed.emplace_back(from, ErrorText(error));
                }
            }
        RemoveEmptyFolders(src->second, keep);
    }
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (r.moved.empty()) DeleteFileW(r.log.c_str());
    return r;
}

// Reverses the newest log (the file names sort by timestamp), newest move first, then removes any
// folders the undo left empty. The log is renamed ".undone" so it can't be replayed twice.
std::vector<std::pair<std::wstring, std::wstring>> UndoLastOrganize(const std::wstring& logDir, const std::vector<std::wstring>& keep,
                                                                    std::wstring& error) {
    error.clear();
    std::vector<std::pair<std::wstring, std::wstring>> undone;
    WIN32_FIND_DATAW fd;
    std::wstring newest;
    HANDLE h = FindFirstFileW((logDir + kPathSep + L"*.log").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do
            if (newest.empty() || wcscmp(fd.cFileName, newest.c_str()) > 0) newest = fd.cFileName;  // names sort by time
        while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (newest.empty()) {
        error = L"There's no organize to undo.";
        return undone;
    }
    std::wstring path = logDir + kPathSep + newest;
    std::string data;
    {
        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) {
            error = L"Couldn't read " + path;
            return undone;
        }
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(f, &size) || size.QuadPart < 0 || size.QuadPart > 64 * 1024 * 1024) {
            CloseHandle(f);
            error = L"The organize log is unreadable or too large.";
            return undone;
        }
        data.resize((size_t)size.QuadPart);
        DWORD got = 0;
        bool read = ReadFile(f, data.data(), (DWORD)data.size(), &got, nullptr) && got == data.size();
        CloseHandle(f);
        if (!read) { error = L"Couldn't read the complete organize log."; return undone; }
    }
    std::vector<std::pair<std::wstring, std::wstring>> moves;  // to, from
    for (size_t start = 0; start < data.size();) {
        size_t end = data.find('\n', start);
        if (end == std::string::npos) end = data.size();
        std::string line = data.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t tab = line.find('\t');
        if (tab != std::string::npos) moves.emplace_back(FromUtf8(line.substr(0, tab)), FromUtf8(line.substr(tab + 1)));
        start = end + 1;
    }
    std::vector<std::wstring> emptied;
    bool complete = true;
    for (auto it = moves.rbegin(); it != moves.rend(); ++it) {
        auto& [to, from] = *it;
        if (!Exists(to)) { if (!Exists(from)) complete = false; continue; }
        if (Exists(from)) { complete = false; continue; }
        SHCreateDirectoryExW(nullptr, DirectoryOf(from).c_str(), nullptr);
        if (MoveFileExW(to.c_str(), from.c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)) {
            undone.emplace_back(from, to);
            emptied.push_back(DirectoryOf(to));
        } else complete = false;
    }
    for (auto& dir : emptied) RemoveEmptyFolders(dir, keep);
    if (!complete) error = L"Some files could not be restored. The undo log was kept so you can retry.";
    else if (!MoveFileExW(path.c_str(), (path + L".undone").c_str(), MOVEFILE_REPLACE_EXISTING))
        error = L"Files were restored, but the undo log could not be marked complete.";
    return undone;
}
