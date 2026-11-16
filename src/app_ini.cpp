// settings.ini held in memory. The Win32 profile API opens, parses and, for a write, rewrites the whole
// file on every call: about 7 ms a write on the maintainer's PC (November 2026), so saving the
// preferences (some 150 keys) froze the window for a second, and every menu toggle paused. Reads and
// writes now touch this copy; a write wakes a short-lived thread that saves the file once, atomically
// (a temporary file replaces it), a moment later. IniFlush saves at once and waits.
//
// The copy follows g.iniPath: when the path changes, unsaved edits go to the old file and the new one is
// read. IniRefresh rereads the file when something else changed it (LoadState and LoadPreferences call
// it). Lookups follow the profile API: names ignore case, the first of duplicate sections or keys
// wins, values are trimmed and lose one pair of enclosing quotes. The file is written as UTF-16.
#include "app.h"
#include "backup.h"

namespace app {
namespace {

struct Section {
    std::wstring name;                // empty for the lines above the first header
    bool header = false;              // false only for those lines
    std::vector<std::wstring> lines;  // everything below the header, as written
};

// Shared with the saving thread: `lock` guards everything but `writing`, which keeps saves in order
// (taken before `lock`, never after).
struct Store {
    SRWLOCK lock = SRWLOCK_INIT;
    CRITICAL_SECTION writing;
    std::wstring path;  // the file `sections` came from
    bool loaded = false;
    std::vector<Section> sections;
    uint64_t edits = 0, saved = 0;  // edits made / edits on disk
    uint64_t fingerprint = 0;  // the file as last read or written
    bool failed = false, reported = false, writer = false;
    Store() { InitializeCriticalSection(&writing); }
} store;

constexpr DWORD kSaveDelayMs = 300;  // a burst of edits (a whole preferences save) becomes one write

struct Exclusive {
    Exclusive() { AcquireSRWLockExclusive(&store.lock); }
    ~Exclusive() { ReleaseSRWLockExclusive(&store.lock); }
};

bool SameName(std::wstring_view a, std::wstring_view b) {
    return CompareStringOrdinal(a.data(), (int)a.size(), b.data(), (int)b.size(), TRUE) == CSTR_EQUAL;
}

std::wstring_view Trim(std::wstring_view s) {
    while (!s.empty() && (s.front() == L' ' || s.front() == L'\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t' || s.back() == L'\r')) s.remove_suffix(1);
    return s;
}

// "key = value" -> key and value, trimmed. A line without '=' is a key with no value.
bool SplitLine(std::wstring_view line, std::wstring_view& key, std::wstring_view& value) {
    size_t eq = line.find(L'=');
    key = Trim(line.substr(0, eq));
    value = eq == std::wstring_view::npos ? std::wstring_view{} : Trim(line.substr(eq + 1));
    return eq != std::wstring_view::npos;
}

// The file's contents as a number. A change of the same length within one tick of the file clock leaves
// the write time alone, so IniRefresh compares the bytes; it runs only when the profile is loaded.
uint64_t Fingerprint(const std::string& bytes) {
    uint64_t hash = 14695981039346656037ull;
    for (char c : bytes) hash = (hash ^ (uint8_t)c) * 1099511628211ull;
    return hash ^ bytes.size();
}
std::string ReadAll(const std::wstring& path) {
    std::string bytes;
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) backup::ReadBytes(path, bytes);
    return bytes;
}

// Text as the profile API reads it: UTF-16 with a byte-order mark, UTF-8 with one, else the ANSI code page.
std::wstring Decode(const std::string& bytes) {
    std::wstring text;
    auto convert = [&](UINT page, size_t skip) {
        int n = MultiByteToWideChar(page, 0, bytes.data() + skip, (int)(bytes.size() - skip), nullptr, 0);
        text.resize(n);
        MultiByteToWideChar(page, 0, bytes.data() + skip, (int)(bytes.size() - skip), text.data(), n);
    };
    if (bytes.size() >= 2 && (uint8_t)bytes[0] == 0xFF && (uint8_t)bytes[1] == 0xFE) {
        text.resize((bytes.size() - 2) / 2);
        memcpy(text.data(), bytes.data() + 2, text.size() * 2);
    } else if (bytes.size() >= 2 && (uint8_t)bytes[0] == 0xFE && (uint8_t)bytes[1] == 0xFF) {
        text.resize((bytes.size() - 2) / 2);
        for (size_t i = 0; i < text.size(); ++i)
            text[i] = (wchar_t)((uint8_t)bytes[2 + i * 2] << 8 | (uint8_t)bytes[3 + i * 2]);
    } else if (bytes.size() >= 3 && !memcmp(bytes.data(), "\xEF\xBB\xBF", 3)) {
        convert(CP_UTF8, 3);
    } else if (!bytes.empty()) {
        convert(CP_ACP, 0);
    }
    return text;
}

// Called with the lock held.
void Parse(const std::wstring& text) {
    store.sections.assign(1, Section{});
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find(L'\n', start);
        if (end == std::wstring::npos) end = text.size();
        std::wstring_view line(text.data() + start, end - start);
        if (!line.empty() && line.back() == L'\r') line.remove_suffix(1);
        start = end + 1;
        std::wstring_view trimmed = Trim(line);
        if (!trimmed.empty() && trimmed.front() == L'[') {
            trimmed.remove_prefix(1);
            Section s;
            s.name = Trim(trimmed.substr(0, trimmed.find(L']')));
            s.header = true;
            store.sections.push_back(std::move(s));
        } else {
            store.sections.back().lines.emplace_back(line);
        }
    }
    if (store.sections.front().lines.empty()) store.sections.erase(store.sections.begin());
}

void Load(const std::wstring& path, const std::string& bytes) {
    Exclusive hold;
    store.path = path;
    store.loaded = true;
    store.fingerprint = Fingerprint(bytes);
    store.saved = store.edits;
    store.failed = store.reported = false;
    Parse(Decode(bytes));
}

std::string Serialize() {
    std::wstring text;
    for (const Section& s : store.sections) {
        if (s.header) text += L"[" + s.name + L"]\r\n";
        for (const std::wstring& line : s.lines) text += line + L"\r\n";
    }
    std::string bytes("\xFF\xFE", 2);
    bytes.append((const char*)text.data(), text.size() * 2);
    return bytes;
}

bool WriteAtomically(const std::wstring& path, const std::string& bytes) {
    const std::wstring temp = path + L".cordinal-tmp";
    HANDLE h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(h, bytes.data(), (DWORD)bytes.size(), &written, nullptr) && written == bytes.size() &&
              FlushFileBuffers(h);
    CloseHandle(h);
    // ReplaceFile keeps the original's attributes and permissions, and refuses a read-only settings.ini.
    if (ok && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
        ok = ReplaceFileW(path.c_str(), temp.c_str(), nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr);
    else if (ok)
        ok = MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH);
    if (!ok) DeleteFileW(temp.c_str());
    return ok;
}

// Writes the copy if it has unsaved edits. Safe from any thread; saves happen one at a time, in order.
bool Save() {
    EnterCriticalSection(&store.writing);
    std::wstring path;
    std::string bytes;
    uint64_t edits;
    {
        Exclusive hold;
        if (store.path.empty()) store.saved = store.edits;  // no profile to save to (some self-tests)
        if (store.edits == store.saved) {
            LeaveCriticalSection(&store.writing);
            return true;
        }
        path = store.path;
        bytes = Serialize();
        edits = store.edits;
    }
    const bool ok = WriteAtomically(path, bytes);
    {
        Exclusive hold;
        if (path == store.path) {
            if (ok) {
                store.saved = edits;
                store.fingerprint = Fingerprint(bytes);
            }
            store.failed = !ok;
        }
    }
    LeaveCriticalSection(&store.writing);
    return ok;
}

DWORD WINAPI SaveLater(void*) {
    for (;;) {
        Sleep(kSaveDelayMs);
        Save();
        Exclusive hold;
        // Done when nothing changed meanwhile; a failed save waits for the next edit to try again.
        if (store.edits == store.saved || store.failed) {
            store.writer = false;
            return 0;
        }
    }
}

// Brings the copy to g.iniPath before any access: a new path saves the old file and reads the new one.
void Follow() {
    const std::wstring& path = g.iniPath;
    {
        Exclusive hold;
        if (store.loaded && store.path == path) return;
    }
    Save();
    Load(path, ReadAll(path));
}

// Called with the lock held, after an edit.
void Edited() {
    ++store.edits;
    if (store.writer) return;
    store.writer = true;
    HANDLE thread = CreateThread(nullptr, 64 * 1024, SaveLater, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (thread) CloseHandle(thread);
    else store.writer = false;  // saved by the next IniFlush
}

Section* FindSection(std::wstring_view name) {
    for (Section& s : store.sections)
        if (s.header && SameName(s.name, name)) return &s;
    return nullptr;
}

std::wstring* FindLine(Section& s, std::wstring_view key) {
    for (std::wstring& line : s.lines) {
        std::wstring_view k, v;
        SplitLine(line, k, v);
        if (!k.empty() && SameName(k, key)) return &line;
    }
    return nullptr;
}

// The value as GetPrivateProfileString returns it, or false when the key is missing.
bool Lookup(const wchar_t* section, const wchar_t* key, std::wstring& out) {
    Section* s = FindSection(section);
    std::wstring* line = s ? FindLine(*s, key) : nullptr;
    if (!line) return false;
    std::wstring_view k, v;
    SplitLine(*line, k, v);
    if (v.size() >= 2 && (v.front() == L'"' || v.front() == L'\'') && v.back() == v.front())
        v = v.substr(1, v.size() - 2);
    out = v;
    return true;
}

// The report is shown once until a save succeeds again, so one failing SaveState is one message.
// Returns false while the last save failed.
bool ReportFailure() {
    bool show = false, failed;
    {
        Exclusive hold;
        failed = store.failed;
        if (!failed) store.reported = false;
        else if (!store.reported) show = store.reported = true;
    }
    if (show) ShowMessage(L"Couldn't save settings to " + g.iniPath + L". Is the file read-only?");
    return !failed;
}

}  // namespace

std::wstring IniRead(const wchar_t* section, const wchar_t* key, const wchar_t* def) {
    Follow();
    std::wstring value;
    Exclusive hold;
    if (Lookup(section, key, value)) return value;
    return std::wstring(Trim(def ? def : L""));
}

// GetPrivateProfileInt: the default when the key is missing or empty, else the leading number as
// RtlUnicodeStringToInteger reads it (a sign, then 0x, 0o or 0b for another base).
int IniInt(const wchar_t* section, const wchar_t* key, int def) {
    std::wstring v = IniRead(section, key, L"");
    if (v.empty()) return def;
    const wchar_t* p = v.c_str();
    while (*p && *p <= L' ') ++p;
    bool negative = false;
    if (*p == L'-' || *p == L'+') negative = *p++ == L'-';
    unsigned base = 10;
    if (p[0] == L'0' && (p[1] == L'x' || p[1] == L'X')) base = 16, p += 2;
    else if (p[0] == L'0' && (p[1] == L'o' || p[1] == L'O')) base = 8, p += 2;
    else if (p[0] == L'0' && (p[1] == L'b' || p[1] == L'B')) base = 2, p += 2;
    uint32_t n = 0;
    for (;; ++p) {
        unsigned d = *p >= L'0' && *p <= L'9' ? *p - L'0' : *p >= L'a' && *p <= L'z' ? *p - L'a' + 10
                   : *p >= L'A' && *p <= L'Z' ? *p - L'A' + 10 : 99;
        if (d >= base) break;
        n = n * base + d;
    }
    return negative ? -(int)n : (int)n;
}

bool IniWrite(const wchar_t* section, const wchar_t* key, const std::wstring& value) {
    Follow();
    {
        Exclusive hold;
        Section* s = FindSection(section);
        if (!s) {
            store.sections.push_back(Section{section, true, {}});
            s = &store.sections.back();
        }
        std::wstring line = std::wstring(key) + L"=" + value;
        if (std::wstring* old = FindLine(*s, key)) {
            if (*old == line) return !store.failed;
            *old = std::move(line);
        } else {
            // After the section's last entry, so blank lines between sections stay between them.
            size_t at = s->lines.size();
            while (at && Trim(s->lines[at - 1]).empty()) --at;
            s->lines.insert(s->lines.begin() + at, std::move(line));
        }
        Edited();
    }
    return ReportFailure();
}

void IniDelete(const wchar_t* section, const wchar_t* key) {
    Follow();
    Exclusive hold;
    for (size_t i = 0; i < store.sections.size(); ++i) {
        Section& s = store.sections[i];
        if (!s.header || !SameName(s.name, section)) continue;
        if (!key) {
            store.sections.erase(store.sections.begin() + i);
            Edited();
            return;
        }
        if (std::wstring* line = FindLine(s, key)) {
            s.lines.erase(s.lines.begin() + (line - s.lines.data()));
            Edited();
        }
        return;
    }
}

std::vector<std::pair<std::wstring, std::wstring>> IniEntries(const wchar_t* section) {
    Follow();
    std::vector<std::pair<std::wstring, std::wstring>> entries;
    Exclusive hold;
    if (Section* s = FindSection(section))
        for (const std::wstring& line : s->lines) {
            std::wstring_view k, v;
            if (SplitLine(line, k, v) && !k.empty()) entries.emplace_back(k, v);
        }
    return entries;
}

void IniReplaceSection(const wchar_t* section, const std::vector<std::pair<std::wstring, std::wstring>>& entries) {
    if (entries.empty()) return IniDelete(section, nullptr);
    Follow();
    Exclusive hold;
    Section* s = FindSection(section);
    if (!s) {
        store.sections.push_back(Section{section, true, {}});
        s = &store.sections.back();
    }
    s->lines.clear();
    for (const auto& [k, v] : entries) s->lines.push_back(k + L"=" + v);
    Edited();
}

std::vector<std::wstring> IniSectionNames() {
    Follow();
    std::vector<std::wstring> names;
    Exclusive hold;
    for (const Section& s : store.sections)
        if (s.header) names.push_back(s.name);
    return names;
}

bool IniFlush() {
    Follow();
    const bool ok = Save();
    return ReportFailure() && ok;
}

void IniRefresh() {
    Follow();
    const std::string bytes = ReadAll(g.iniPath);
    {
        Exclusive hold;
        // Unsaved edits win over a change made elsewhere; they reach the file with the next save.
        if (store.edits != store.saved || Fingerprint(bytes) == store.fingerprint) return;
    }
    Load(g.iniPath, bytes);
}

}  // namespace app
