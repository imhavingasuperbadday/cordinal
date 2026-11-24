// Library sync through a shared folder: the log format, the merge, and the worker that reads the folder.
// syncfolder.h has the rules; docs/sync.md has the format.
#include "syncfolder.h"
#include "backup.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cwchar>
#include <cmath>
#include <map>

namespace syncfolder {

struct Key {
    std::string rel, artist, album, title;  // rel: '/'-separated, under a library folder
    uint64_t hash = 0;                      // Track::audioHash
    uint32_t ms = 0;
};

// One line of a log. Kinds: B counts a PC had when it joined, D a change in counts, N one listen, R a
// rating, H a heart, S a place in a song, L a playlist, X a playlist closed.
struct Event {
    char kind = 0;
    uint32_t time = 0;
    int64_t n[4] = {};
    Key key;
    bool autoList = false;
    std::string name, rules;
    std::vector<Key> items;
    uint64_t source = 0;  // the log it came from
    std::string line;     // as read, kept while its track isn't here yet
};

struct Shadow {  // a track as this PC last published or applied it
    uint32_t plays = 0, skips = 0, basePlays = 0, baseSkips = 0, ratingTime = 0, favTime = 0;
    uint8_t rating = 0;
    bool fav = false;
};
struct SpotState { uint32_t posMs = 0, time = 0; };
struct ListState { uint32_t time = 0; uint64_t hash = 0; bool deleted = false; };
struct PeerState { uint64_t offset = 0; uint32_t lastSeen = 0; std::wstring name; };
struct Pending { uint64_t source = 0; uint32_t received = 0; std::string line; };

struct KeyLess {
    bool operator()(const std::pair<uint64_t, Shadow>& a, const std::pair<uint64_t, Shadow>& b) const {
        return a.first < b.first;
    }
    bool operator()(const std::pair<uint64_t, Shadow>& a, uint64_t b) const { return a.first < b; }
};

// Path keys to Shadows: a sorted vector, plus a map for keys first seen during this merge. Tens of
// thousands of played tracks cost a flat array, not a hash node each.
class Shadows {
public:
    std::vector<std::pair<uint64_t, Shadow>> sorted;
    Shadow* Find(uint64_t key) {
        auto it = std::lower_bound(sorted.begin(), sorted.end(), key, KeyLess());
        if (it != sorted.end() && it->first == key) return &it->second;
        auto extra = added_.find(key);
        return extra == added_.end() ? nullptr : &extra->second;
    }
    Shadow& Get(uint64_t key) {
        if (Shadow* s = Find(key)) return *s;
        return added_[key];
    }
    void Sort() { std::sort(sorted.begin(), sorted.end(), KeyLess()); }
    void Settle() {  // the keys in added_ are never in sorted, so nothing repeats
        sorted.insert(sorted.end(), added_.begin(), added_.end());
        added_.clear();
        Sort();
    }
private:
    std::map<uint64_t, Shadow> added_;
};

// A short list of keyed entries searched in order: a few dozen, where a std::map's code costs more
// than its searches save.
template <class K, class V>
struct Table {
    std::vector<std::pair<K, V>> items;
    V* Find(const K& key) {
        for (auto& e : items)
            if (e.first == key) return &e.second;
        return nullptr;
    }
    V& operator[](const K& key) {
        if (V* v = Find(key)) return *v;
        items.emplace_back(key, V{});
        return items.back().second;
    }
    auto begin() { return items.begin(); }
    auto end() { return items.end(); }
    auto begin() const { return items.begin(); }
    auto end() const { return items.end(); }
    size_t size() const { return items.size(); }
};

struct State {
    std::wstring folder, file;   // file: this PC's log, inside folder
    uint64_t log = 0;            // this PC's log id
    std::string fingerprint;
    uint32_t enabled = 0;        // when this PC joined the folder
    uint64_t written = 0;        // bytes of this PC's log the library already reflects
    Table<uint64_t, PeerState> peers;
    Shadows tracks;
    Table<uint64_t, SpotState> spots;
    Table<std::string, ListState> lists;  // by playlist name
    std::vector<Pending> pending;
    std::string outbox;          // lines merged here that couldn't be written to the log yet
};

struct Scan {
    std::unique_ptr<State> state;
    std::vector<Event> events;   // this PC's own unsaved tail first, then the other PCs'
    std::vector<std::pair<std::wstring, uint64_t>> files;  // every log file and its size
    uint64_t seen = 0;           // FolderDigest of `files`
    uint64_t ownTail = 0;        // bytes of this PC's log past `written` that were read
    bool idle = false, changed = false;
    bool partial = false;        // a log had more than one pass reads
    std::wstring problem;
};

namespace {

constexpr char kMagic[] = "cordinal-sync";
constexpr char kStateMagic[] = "cordinal-sync-state";
constexpr wchar_t kExt[] = L".cordsync";
constexpr uint32_t kPassSeconds = 60;
constexpr uint32_t kDay = 24 * 3600;
constexpr size_t kMaxPending = 20000, kMaxSpots = 12;
constexpr uint64_t kReadCap = 32ull << 20;  // per log per pass; the rest is read next time

uint64_t Fnv(std::string_view s, uint64_t h = 1469598103934665603ull) {
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

// Windows paths and tags compare without ASCII case, the way the library and the matching treat them.
uint64_t FoldFnv(std::string_view s, uint64_t h = 1469598103934665603ull) {
    for (unsigned char c : s) {
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

uint64_t PathFnv(std::string_view s, uint64_t h) {
#ifdef _WIN32
    for (unsigned char c : s) {
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        if (c == '/') c = '\\';
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
#else
    return Fnv(s, h);
#endif
}

uint32_t Check(std::string_view s) {
    uint64_t h = Fnv(s);
    return (uint32_t)(h ^ (h >> 32));
}

std::string Hex(uint64_t v, int digits = 16) {
    static const char kDigits[] = "0123456789abcdef";
    std::string s((size_t)digits, '0');
    for (int i = digits - 1; i >= 0; --i, v >>= 4) s[(size_t)i] = kDigits[v & 15];
    return s;
}

bool ParseHex(std::string_view s, uint64_t& v) {
    if (s.empty() || s.size() > 16) return false;
    v = 0;
    for (char c : s) {
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (d < 0) return false;
        v = v << 4 | (uint64_t)d;
    }
    return true;
}

bool ParseNum(std::string_view s, int64_t& v) {
    bool negative = !s.empty() && s[0] == '-';
    if (negative) s.remove_prefix(1);
    if (s.empty() || s.size() > 18) return false;
    v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    if (negative) v = -v;
    return true;
}

std::vector<std::string_view> Split(std::string_view s, char sep, size_t max = SIZE_MAX) {
    std::vector<std::string_view> parts;
    while (parts.size() + 1 < max) {
        size_t at = s.find(sep);
        if (at == std::string_view::npos) break;
        parts.push_back(s.substr(0, at));
        s.remove_prefix(at + 1);
    }
    parts.push_back(s);
    return parts;
}

// Free text keeps every character: '%' and control characters (the separators among them) as %XX.
std::string Escape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c < 0x20 || c == '%') out += '%' + Hex(c, 2);
        else out += (char)c;
    }
    return out;
}

std::string Unescape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        uint64_t v;
        if (s[i] == '%' && i + 2 < s.size() && ParseHex(s.substr(i + 1, 2), v)) {
            out += (char)v;
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

// One tab-separated line, built in place rather than from a temporary per field.
struct Line {
    std::string s;
    explicit Line(std::string_view first) : s(first) {}
    Line& operator<<(std::string_view v) {
        s += '\t';
        s += v;
        return *this;
    }
    Line& operator<<(int64_t v) {
        char digits[24];
        s += '\t';
        s.append(digits, std::to_chars(digits, digits + sizeof digits, v).ptr);
        return *this;
    }
};

void Seal(std::string& out, const Line& line) {
    out += line.s;
    out += '\t';
    out += Hex(Check(line.s), 8);
    out += '\n';
}

// The line without its checksum, or false when the line is damaged.
bool Unseal(std::string_view line, std::string_view& body) {
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    size_t tab = line.rfind('\t');
    uint64_t sum;
    if (tab == std::string_view::npos || !ParseHex(line.substr(tab + 1), sum)) return false;
    body = line.substr(0, tab);
    return sum == Check(body);
}

std::string KeyText(const Key& k, char sep) {
    std::string s = Escape(k.rel);
    s += sep;
    if (k.hash) s += Hex(k.hash);
    s += sep;
    for (const std::string* text : {&k.artist, &k.album, &k.title}) {
        s += Escape(*text);
        s += sep;
    }
    s += std::to_string(k.ms);
    return s;
}

bool ParseKey(const std::vector<std::string_view>& f, size_t at, Key& k) {
    if (f.size() < at + 6) return false;
    k.rel = Unescape(f[at]);
    if (!f[at + 1].empty() && !ParseHex(f[at + 1], k.hash)) return false;
    k.artist = Unescape(f[at + 2]);
    k.album = Unescape(f[at + 3]);
    k.title = Unescape(f[at + 4]);
    int64_t ms;
    if (!ParseNum(f[at + 5], ms) || ms < 0 || ms > UINT32_MAX) return false;
    k.ms = (uint32_t)ms;
    return !k.rel.empty() || k.hash || !k.title.empty();
}

// How many numbers each track event carries before its key.
int Numbers(char kind) {
    switch (kind) {
    case 'B': case 'D': return 4;
    case 'N': case 'R': case 'H': return 2;
    case 'S': return 1;
    default: return -1;
    }
}

bool ParseEvent(std::string_view line, Event& e) {
    std::string_view body;
    if (!Unseal(line, body)) return false;
    auto f = Split(body, '\t');
    if (f.size() < 2 || f[0].size() != 1) return false;
    e.kind = f[0][0];
    int64_t time;
    if (!ParseNum(f[1], time) || time < 0 || time > UINT32_MAX) return false;
    e.time = (uint32_t)time;
    if (int numbers = Numbers(e.kind); numbers >= 0) {
        if (f.size() < 2 + (size_t)numbers + 6) return false;
        for (int i = 0; i < numbers; ++i)
            if (!ParseNum(f[2 + (size_t)i], e.n[i])) return false;
        return ParseKey(f, 2 + (size_t)numbers, e.key);
    }
    if (e.kind == 'X') {
        if (f.size() < 3) return false;
        e.name = Unescape(f[2]);
        return !e.name.empty();
    }
    if (e.kind == 'L') {
        if (f.size() < 6 || (f[2] != "A" && f[2] != "S")) return false;
        e.autoList = f[2] == "A";
        e.name = Unescape(f[3]);
        e.rules = Unescape(f[4]);
        if (!f[5].empty())
            for (std::string_view item : Split(f[5], '\x1e')) {
                Key k;
                if (ParseKey(Split(item, '\x1f'), 0, k)) e.items.push_back(std::move(k));
            }
        return !e.name.empty();
    }
    return false;  // a kind from a later version
}

// Complete, checked lines of `bytes`; `used` is how far whole lines reach.
void ParseLines(std::string_view bytes, uint64_t source, std::vector<Event>& out, uint64_t& used, uint32_t* newest) {
    used = 0;
    while (true) {
        size_t end = bytes.find('\n', (size_t)used);
        if (end == std::string_view::npos) break;
        std::string_view line = bytes.substr((size_t)used, end - (size_t)used);
        used = end + 1;
        Event e;
        if (!ParseEvent(line, e)) continue;
        e.source = source;
        e.line.assign(line);
        if (newest) *newest = std::max(*newest, e.time);
        out.push_back(std::move(e));
    }
}

// --- Files ---------------------------------------------------------------------------------------------

bool FileSize(const std::wstring& path, uint64_t& size) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) ||
        (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return false;
    size = (uint64_t)data.nFileSizeHigh << 32 | data.nFileSizeLow;
    return true;
}

bool ReadRange(const std::wstring& path, uint64_t from, uint64_t count, std::string& out) {
    out.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER at;
    at.QuadPart = (LONGLONG)from;
    bool ok = SetFilePointerEx(h, at, nullptr, FILE_BEGIN) != 0;
    out.resize((size_t)count);
    size_t got = 0;
    while (ok && got < out.size()) {
        DWORD read = 0;
        DWORD want = (DWORD)std::min<size_t>(out.size() - got, 1u << 20);
        ok = ReadFile(h, out.data() + got, want, &read, nullptr) != 0;
        if (!read) break;
        got += read;
    }
    CloseHandle(h);
    out.resize(got);
    return ok;
}

// Appends whole lines, starting a fresh line when a failed write left a partial one. A partial line
// fails its checksum and every reader skips it; nothing already in the file is touched.
bool Append(const std::wstring& path, const std::string& header, const std::string& lines, uint64_t& size) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER end{};
    bool ok = GetFileSizeEx(h, &end) != 0;
    std::string bytes;
    if (ok && end.QuadPart == 0) bytes = header;
    else if (ok) {
        LARGE_INTEGER last;
        last.QuadPart = end.QuadPart - 1;
        char c = 0;
        DWORD read = 0;
        if (SetFilePointerEx(h, last, nullptr, FILE_BEGIN) && ReadFile(h, &c, 1, &read, nullptr) && read == 1 && c != '\n')
            bytes = "\n";
    }
    bytes += lines;
    LARGE_INTEGER zero{};
    ok = ok && SetFilePointerEx(h, zero, nullptr, FILE_END);
    size_t done = 0;
    while (ok && done < bytes.size()) {
        DWORD wrote = 0;
        ok = WriteFile(h, bytes.data() + done, (DWORD)std::min<size_t>(bytes.size() - done, 1u << 20), &wrote, nullptr) &&
             wrote > 0;
        done += wrote;
    }
    ok = ok && FlushFileBuffers(h);
    size = (uint64_t)end.QuadPart + done;
    CloseHandle(h);
    return ok;
}

bool WriteAtomically(const std::wstring& path, const std::string& bytes) {
    std::wstring temp = path + L".tmp";
    DeleteFileW(temp.c_str());
    if (backup::WriteBytes(temp, bytes) &&
        MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return true;
    DeleteFileW(temp.c_str());
    return false;
}

std::wstring InFolder(const std::wstring& folder, const std::wstring& name) {
    if (folder.empty() || folder.back() == L'\\' || folder.back() == L'/') return folder + name;
    return folder + kPathSep + name;
}

bool SameFolder(const std::wstring& a, const std::wstring& b) {
    return PathFnv(ToUtf8(a), 0) == PathFnv(ToUtf8(b), 0);
}

std::string Header(uint64_t log, const std::wstring& machine) {
    std::string out;
    Seal(out, Line(kMagic) << 1 << Hex(log) << Escape(ToUtf8(machine)));
    return out;
}

bool ParseHeader(std::string_view bytes, uint64_t& log, std::wstring& machine, uint64_t& length) {
    size_t end = bytes.find('\n');
    std::string_view body;
    if (end == std::string_view::npos || !Unseal(bytes.substr(0, end), body)) return false;
    auto f = Split(body, '\t');
    if (f.size() < 4 || f[0] != kMagic || f[1] != "1" || !ParseHex(f[2], log)) return false;
    machine = FromUtf8(Unescape(f[3]));
    length = end + 1;
    return true;
}

std::wstring FileNameFor(const std::wstring& machine, uint64_t log) {
    std::wstring name;
    for (wchar_t c : machine.substr(0, 40))
        name += (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'-' || c == L'_' ? c : L'-';
    if (name.empty()) name = L"pc";
    return name + L'-' + FromUtf8(Hex(log, 8)) + kExt;
}

uint64_t NewLogId(const std::string& fingerprint) {
    uint64_t h = Fnv(fingerprint);
    h = Fnv(std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()), h);
    h = Fnv(std::to_string(std::chrono::system_clock::now().time_since_epoch().count()), h);
    int local = 0;
    h = Fnv(std::to_string((uintptr_t)&local), h);
    return h ? h : 1;
}

// --- State -----------------------------------------------------------------------------------------------

std::string SaveStateText(const State& st) {
    std::string out;
    auto put = [&](const Line& line) {
        out += line.s;
        out += '\n';
    };
    put(Line(kStateMagic) << 1);
    put(Line("folder") << Escape(ToUtf8(st.folder)));
    put(Line("own") << Hex(st.log) << st.fingerprint << st.enabled << (int64_t)st.written << Escape(ToUtf8(st.file)));
    for (const auto& [id, p] : st.peers) put(Line("peer") << Hex(id) << (int64_t)p.offset << p.lastSeen << Escape(ToUtf8(p.name)));
    for (const auto& [key, s] : st.tracks.sorted)
        put(Line("t") << Hex(key) << s.plays << s.skips << s.basePlays << s.baseSkips << s.rating << s.ratingTime << s.fav
                      << s.favTime);
    for (const auto& [key, s] : st.spots) put(Line("s") << Hex(key) << s.posMs << s.time);
    for (const auto& [name, l] : st.lists) put(Line("p") << l.time << l.deleted << Hex(l.hash) << Escape(name));
    for (const Pending& p : st.pending) put(Line("q") << Hex(p.source) << p.received << p.line);
    for (std::string_view line : Split(st.outbox, '\n'))
        if (!line.empty()) put(Line("o") << line);
    return out;
}

std::unique_ptr<State> LoadState(const std::wstring& file) {
    std::string bytes;
    if (!backup::ReadBytes(file, bytes, 512ull << 20)) return nullptr;
    auto lines = Split(bytes, '\n');
    if (lines.empty() || lines[0] != std::string(kStateMagic) + "\t1") return nullptr;
    auto st = std::make_unique<State>();
    bool own = false;
    for (size_t i = 1; i < lines.size(); ++i) {
        std::string_view line = lines[i];
        if (line.empty()) continue;
        auto f = Split(line, '\t', line.starts_with("q\t") ? 4 : line.starts_with("o\t") ? 2 : SIZE_MAX);
        int64_t n[8] = {};
        uint64_t key = 0;
        auto nums = [&](size_t from, size_t count) {
            if (f.size() < from + count) return false;
            for (size_t j = 0; j < count; ++j)
                if (!ParseNum(f[from + j], n[j])) return false;
            return true;
        };
        if (f[0] == "folder" && f.size() >= 2) st->folder = FromUtf8(Unescape(f[1]));
        else if (f[0] == "own" && f.size() >= 6 && ParseHex(f[1], st->log) && nums(3, 2)) {
            st->fingerprint = std::string(f[2]);
            st->enabled = (uint32_t)n[0];
            st->written = (uint64_t)std::max<int64_t>(0, n[1]);
            st->file = FromUtf8(Unescape(f[5]));
            own = true;
        } else if (f[0] == "peer" && f.size() >= 5 && ParseHex(f[1], key) && nums(2, 2)) {
            PeerState& p = st->peers[key];
            p.offset = (uint64_t)std::max<int64_t>(0, n[0]);
            p.lastSeen = (uint32_t)n[1];
            p.name = FromUtf8(Unescape(f[4]));
        } else if (f[0] == "t" && ParseHex(f.size() > 1 ? f[1] : "", key) && nums(2, 8)) {
            Shadow s;
            s.plays = (uint32_t)n[0];
            s.skips = (uint32_t)n[1];
            s.basePlays = (uint32_t)n[2];
            s.baseSkips = (uint32_t)n[3];
            s.rating = (uint8_t)std::clamp<int64_t>(n[4], 0, 100);
            s.ratingTime = (uint32_t)n[5];
            s.fav = n[6] != 0;
            s.favTime = (uint32_t)n[7];
            st->tracks.sorted.emplace_back(key, s);
        } else if (f[0] == "s" && ParseHex(f.size() > 1 ? f[1] : "", key) && nums(2, 2)) {
            st->spots[key] = {(uint32_t)n[0], (uint32_t)n[1]};
        } else if (f[0] == "p" && f.size() >= 5 && nums(1, 2) && ParseHex(f[3], key)) {
            st->lists[Unescape(f[4])] = {(uint32_t)n[0], key, n[1] != 0};
        } else if (f[0] == "q" && f.size() == 4 && ParseHex(f[1], key) && nums(2, 1)) {
            st->pending.push_back({key, (uint32_t)n[0], std::string(f[3])});
        } else if (f[0] == "o" && f.size() == 2) {
            st->outbox += std::string(f[1]) + '\n';
        }
    }
    if (!own || st->file.empty()) return nullptr;
    st->tracks.Sort();
    return st;
}

std::unique_ptr<State> FreshState(const std::wstring& folder, const std::wstring& machine, const std::string& fingerprint) {
    auto st = std::make_unique<State>();
    st->folder = folder;
    st->fingerprint = fingerprint;
    st->log = NewLogId(fingerprint + ToUtf8(folder));
    st->file = FileNameFor(machine, st->log);
    st->enabled = UnixNow();
    return st;
}

uint64_t FileFnv(const std::wstring& name, uint64_t size) {
    return Fnv(std::string_view((const char*)&size, sizeof size), Fnv(ToUtf8(name)));
}

struct ScanInput {
    std::wstring folder, stateFile, machine;
    std::string fingerprint;
    bool force = false;   // read the state even when nothing looks new
    uint64_t seen = 0;    // the folder's digest after the last pass, 0 = unknown
    std::unique_ptr<State> state;  // merged but not saved yet (only at exit): used instead of the file
};

// The worker's half: reads the folder and this PC's bookkeeping. Never touches the library.
std::unique_ptr<Scan> ScanFolder(ScanInput& in) {
    auto scan = std::make_unique<Scan>();
    WIN32_FILE_ATTRIBUTE_DATA folderData{};
    if (!GetFileAttributesExW(in.folder.c_str(), GetFileExInfoStandard, &folderData) ||
        !(folderData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        scan->problem = L"Can't reach the sync folder " + in.folder + L".";
        return scan;
    }
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW(InFolder(in.folder, L"*").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = fd.cFileName;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || name.size() <= wcslen(kExt) ||
                name.compare(name.size() - wcslen(kExt), std::wstring::npos, kExt) != 0)
                continue;
            const uint64_t size = (uint64_t)fd.nFileSizeHigh << 32 | fd.nFileSizeLow;
            scan->files.emplace_back(name, size);
            scan->seen += FileFnv(name, size);  // a sum: the listing's order doesn't matter
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    if (!in.force && in.seen && scan->seen == in.seen) {
        scan->idle = true;
        return scan;
    }

    scan->changed = in.state != nullptr;  // still to be saved
    std::unique_ptr<State> st = in.state ? std::move(in.state) : LoadState(in.stateFile);
    const std::wstring ownPath = st ? InFolder(in.folder, st->file) : std::wstring();
    uint64_t ownSize = 0;
    const bool ownThere = st && FileSize(ownPath, ownSize);
    if (!st || !SameFolder(st->folder, in.folder) || (!ownThere && st->written > 0)) {
        // A new folder, or this PC's log is gone from it: join afresh. What the library holds now is
        // what this PC brings, merged with the others like any PC joining.
        st = FreshState(in.folder, in.machine, in.fingerprint);
        scan->changed = true;
    } else if (st->fingerprint != in.fingerprint) {
        // This profile was copied from another PC. Its library already holds everything that PC's log
        // says up to `written`, so that log is read on from there, and this PC starts a log of its own.
        st->peers[st->log] = {st->written, UnixNow(), L""};
        auto fresh = FreshState(in.folder, in.machine, in.fingerprint);
        st->log = fresh->log;
        st->file = fresh->file;
        st->fingerprint = in.fingerprint;
        st->enabled = fresh->enabled;
        st->written = 0;
        st->outbox.clear();
        scan->changed = true;
    } else if (ownThere && ownSize > st->written) {
        // Written after the last saved bookkeeping, so the saved library may not hold it: lines that were
        // still waiting to be written are skipped, the rest is merged again (the merge treats this PC's own
        // counts so they aren't added twice).
        std::string tail;
        if (ReadRange(ownPath, st->written, std::min(ownSize - st->written, kReadCap), tail)) {
            std::string_view rest = tail;
            if (!st->outbox.empty() && rest.starts_with(st->outbox)) {
                rest.remove_prefix(st->outbox.size());
                st->written += st->outbox.size();
                st->outbox.clear();
            }
            uint64_t used = 0;
            ParseLines(rest, st->log, scan->events, used, nullptr);
            scan->ownTail = rest.size();  // a partial line is a failed write: past it, not into it
            scan->changed = true;
        }
    }
    for (const auto& [name, size] : scan->files) {
        if (name == st->file) continue;
        const std::wstring path = InFolder(in.folder, name);
        std::string head;
        uint64_t log = 0, headerLength = 0;
        std::wstring machine;
        if (!ReadRange(path, 0, std::min<uint64_t>(size, 4096), head) || !ParseHeader(head, log, machine, headerLength) ||
            log == st->log)
            continue;
        const PeerState* known = st->peers.Find(log);
        uint64_t from = known ? known->offset : 0;
        if (from < headerLength || from > size) from = headerLength;  // new, or started over
        uint64_t used = 0;
        std::string bytes;
        uint32_t newest = known ? known->lastSeen : 0;
        if (size > from && ReadRange(path, from, std::min(size - from, kReadCap), bytes))
            ParseLines(bytes, log, scan->events, used, &newest);
        if (size - from > kReadCap) scan->partial = true;
        PeerState& p = st->peers[log];
        if (p.offset != from + used || p.name != machine || p.lastSeen != newest) scan->changed = true;
        p.offset = from + used;
        p.name = machine;
        p.lastSeen = newest;
    }
    scan->state = std::move(st);
    return scan;
}

// --- Matching ------------------------------------------------------------------------------------------

std::string Clean(std::string_view s) {
    std::string out(s);
    while (!out.empty() && (unsigned char)out.back() <= ' ') out.pop_back();
    size_t start = 0;
    while (start < out.size() && (unsigned char)out[start] <= ' ') ++start;
    return out.substr(start);
}

uint64_t TagFnv(std::string_view artist, std::string_view album, std::string_view title) {
    uint64_t h = FoldFnv(Clean(artist));
    h = FoldFnv("\x1f", h);
    h = FoldFnv(Clean(album), h);
    h = FoldFnv("\x1f", h);
    return FoldFnv(Clean(title), h);
}

uint64_t PathKey(const Library& lib, const Track& t) {
    return PathFnv(lib.View(t.file), PathFnv("/", PathFnv(lib.View(t.dir), 1469598103934665603ull)));
}

// Library folders as UTF-8 without a trailing separator.
std::vector<std::string> RootList(const std::vector<std::wstring>& roots) {
    std::vector<std::string> out;
    for (const std::wstring& r : roots) {
        std::string s = ToUtf8(r);
        while (s.size() > 1 && (s.back() == '\\' || s.back() == '/')) s.pop_back();
        if (!s.empty()) out.push_back(s);
    }
    return out;
}

bool PrefixOf(std::string_view root, std::string_view path) {
    if (path.size() <= root.size() + 1 || (path[root.size()] != '\\' && path[root.size()] != '/')) return false;
#ifdef _WIN32
    for (size_t i = 0; i < root.size(); ++i) {
        char a = root[i], b = path[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a == '/') a = '\\';
        if (b == '/') b = '\\';
        if (a != b) return false;
    }
    return true;
#else
    return path.starts_with(root);
#endif
}

Key KeyOf(const Library& lib, TrackId id, const std::vector<std::string>& roots) {
    const Track& t = lib[id];
    Key k;
    std::string dir(lib.View(t.dir));
    std::string path = dir.empty() ? std::string(lib.View(t.file)) : dir + (char)kPathSep + std::string(lib.View(t.file));
    size_t longest = 0;  // a nested library folder wins over its parent
    for (const std::string& root : roots)
        if (root.size() > longest && PrefixOf(root, path)) longest = root.size();
    if (longest) {
        k.rel = path.substr(longest + 1);
        std::replace(k.rel.begin(), k.rel.end(), '\\', '/');
    }
    k.hash = t.audioHash;
    k.artist = Clean(lib.View(t.artist));
    k.album = Clean(lib.View(t.album));
    k.title = Clean(lib.View(t.title));
    k.ms = t.durationMs;
    return k;
}

// A relative path from another PC is only ever used beneath a library folder.
bool SafeRel(std::string_view rel) {
    if (rel.empty() || rel[0] == '/' || rel[0] == '\\' || rel.find(':') != std::string_view::npos) return false;
    for (std::string_view part : Split(rel, '/'))
        if (part.empty() || part == "." || part == ".." || part.find('\\') != std::string_view::npos) return false;
    return true;
}

class Matcher {
public:
    Matcher(Library& lib, const std::vector<std::string>& roots) : lib_(lib), roots_(roots) {}

    TrackId Find(const Key& k) {
        if (SafeRel(k.rel))
            for (const std::string& root : roots_) {
                TrackId id = lib_.Find(FromUtf8(root + (char)kPathSep + Native(k.rel)));
                if (lib_.Contains(id)) return id;
            }
        if (k.hash) {
            Build();
            auto it = std::lower_bound(byHash_.begin(), byHash_.end(), std::make_pair(k.hash, (TrackId)0));
            if (it != byHash_.end() && it->first == k.hash) return it->second;
        }
        if (!k.title.empty()) {
            Build();
            const uint64_t tags = TagFnv(k.artist, k.album, k.title);
            for (auto it = std::lower_bound(byTags_.begin(), byTags_.end(), std::make_pair(tags, (TrackId)0));
                 it != byTags_.end() && it->first == tags; ++it) {
                const uint32_t ms = lib_[it->second].durationMs;
                if (!k.ms || !ms || (ms > k.ms ? ms - k.ms : k.ms - ms) <= 2000) return it->second;
            }
        }
        return kInvalidTrack;
    }

    // A playlist may name a file another PC has that hasn't arrived or been scanned here yet: it goes in
    // as a track under the library folder that has it (or the first), and reads its tags once it's there.
    TrackId FindOrCreate(const Key& k, std::vector<TrackId>& created) {
        TrackId id = Find(k);
        if (lib_.Contains(id) || !SafeRel(k.rel) || roots_.empty()) return id;
        std::wstring path;
        for (const std::string& root : roots_) {
            std::wstring candidate = FromUtf8(root + (char)kPathSep + Native(k.rel));
            if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
                path = candidate;
                break;
            }
        }
        if (path.empty()) path = FromUtf8(roots_[0] + (char)kPathSep + Native(k.rel));
        id = lib_.Add(path);
        if (lib_.Contains(id)) created.push_back(id);
        return id;
    }

private:
    Library& lib_;
    const std::vector<std::string>& roots_;
    bool built_ = false;
    std::vector<std::pair<uint64_t, TrackId>> byHash_, byTags_;

    static std::string Native(std::string rel) {
        std::replace(rel.begin(), rel.end(), '/', (char)kPathSep);
        return rel;
    }
    void Build() {
        if (built_) return;
        built_ = true;
        for (TrackId id = 0; id < (TrackId)lib_.Count(); ++id) {
            const Track& t = lib_[id];
            if (t.flags & Track::kMissing) continue;
            if (t.audioHash) byHash_.emplace_back(t.audioHash, id);
            if (t.title) byTags_.emplace_back(TagFnv(lib_.View(t.artist), lib_.View(t.album), lib_.View(t.title)), id);
        }
        std::sort(byHash_.begin(), byHash_.end());
        std::sort(byTags_.begin(), byTags_.end());
    }
};

bool Syncable(const Playlist& pl) {
    return (pl.kind == PlaylistKind::Static || pl.kind == PlaylistKind::Auto) && !pl.name.empty();
}

uint64_t ListHash(const Library& lib, const Playlist& pl) {
    uint64_t h = Fnv(pl.kind == PlaylistKind::Auto ? "A" : "S");
    h = Fnv(pl.rules, h);
    if (pl.kind == PlaylistKind::Static)
        for (TrackId id : pl.items) {
            const uint64_t key = lib.Contains(id) ? PathKey(lib, lib[id]) : 0;
            h = Fnv(std::string_view((const char*)&key, sizeof key), h);
        }
    return h;
}

// A change made here is newer than anything this PC has seen for the same thing, even within one second,
// so a quick second edit after taking another PC's still wins everywhere.
uint32_t After(uint32_t seen, uint32_t now) { return std::max(now, seen + 1); }

uint32_t Sat(int64_t v) { return (uint32_t)std::clamp<int64_t>(v, 0, UINT32_MAX); }

void MinFirst(uint32_t& first, int64_t other) {
    if (other > 0 && other <= UINT32_MAX && (!first || (uint32_t)other < first)) first = (uint32_t)other;
}

}  // namespace

// --- The merge -------------------------------------------------------------------------------------------

uint64_t LocalDigest(const Live& live) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    const Library& lib = live.library;
    for (TrackId id = 0; id < (TrackId)lib.Count(); ++id) {
        const Track& t = lib[id];
        const bool fav = lib.Favorite(id);
        if (!t.playCount && !t.skipCount && !t.rating && !fav) continue;
        mix(id);
        mix((uint64_t)t.playCount << 32 | t.skipCount);
        mix((uint64_t)t.rating << 1 | (fav ? 1u : 0u));
    }
    for (const Playlist& pl : live.playlists)
        if (Syncable(pl)) {
            mix(Fnv(ToUtf8(pl.name)));
            mix(Fnv(pl.rules));
            for (TrackId id : pl.items) mix(id);
        }
    for (const auto& [id, pos] : live.spots) mix((uint64_t)id << 32 | (std::isfinite(pos) ? (uint32_t)(pos * 1000) : 0u));
    return h;
}

bool Sync::Merge(Live live, Scan& scan, Changes& changes) {
    State& st = *scan.state;
    Library& lib = live.library;
    const uint32_t now = UnixNow();
    const std::vector<std::string> folders = RootList(roots);
    std::string out;
    bool stateChanged = scan.changed;

    // 1. What changed here since the last pass becomes lines in this PC's log.
    const std::vector<PlayEvent>& log = live.playLog;
    if (listenMark_ > log.size()) listenMark_ = log.size();
    std::vector<std::pair<uint64_t, TrackId>> listens;  // (track, log index): new listens per track, oldest first
    for (size_t i = listenMark_; i < log.size(); ++i) listens.emplace_back(log[i].id, (TrackId)i);
    std::sort(listens.begin(), listens.end());
    auto keyText = [&](TrackId id) { return KeyText(KeyOf(lib, id, folders), '\t'); };
    auto counts = [&](TrackId id, int64_t plays, int64_t skips) {
        const Track& t = lib[id];
        for (auto it = std::lower_bound(listens.begin(), listens.end(), std::make_pair((uint64_t)id, (TrackId)0));
             it != listens.end() && it->first == id; ++it) {
            const PlayEvent& e = log[it->second];
            const bool counted = e.flags & PlayEvent::kCounted, skip = !counted && (e.flags & PlayEvent::kSkip);
            if ((counted && plays > 0) || (skip && skips > 0)) {
                Seal(out, Line("N") << e.time << e.seconds << e.flags << keyText(id));
                (counted ? plays : skips) -= 1;
            }
        }
        if (plays || skips)
            Seal(out, Line("D") << now << plays << skips << t.lastPlayed << t.firstPlayed << keyText(id));
    };
    // Places in songs: published when a song is left partway, and when it drops off the list.
    std::vector<uint64_t> spotKeys, clearedSpots;
    auto has = [](const std::vector<uint64_t>& keys, uint64_t key) { return std::find(keys.begin(), keys.end(), key) != keys.end(); };
    for (const auto& [id, pos] : live.spots) {
        if (!lib.Contains(id) || !std::isfinite(pos)) continue;
        const uint64_t key = PathKey(lib, lib[id]);
        const uint32_t ms = (uint32_t)std::clamp(pos * 1000, 0.0, 4e9);
        spotKeys.push_back(key);
        SpotState& s = st.spots[key];
        if (s.posMs && (s.posMs > ms ? s.posMs - ms : ms - s.posMs) <= 1000) continue;
        s = {ms, After(s.time, now)};
        Seal(out, Line("S") << s.time << ms << keyText(id));
    }
    for (const auto& [key, s] : st.spots)
        if (s.posMs && !has(spotKeys, key)) clearedSpots.push_back(key);
    std::erase_if(st.spots.items, [&](const auto& e) { return !e.second.posMs && e.second.time + 90 * kDay < now; });

    Shadows tracks;
    tracks.sorted.reserve(st.tracks.sorted.size() + 64);
    for (TrackId id = 0; id < (TrackId)lib.Count(); ++id) {
        const Track& t = lib[id];
        const bool fav = lib.Favorite(id);
        const uint64_t key = PathKey(lib, t);
        if (!clearedSpots.empty() && has(clearedSpots, key)) {  // left the list here: clear it everywhere
            SpotState& s = st.spots[key];
            s = {0, After(s.time, now)};
            Seal(out, Line("S") << s.time << 0 << keyText(id));
        }
        const Shadow* old = st.tracks.Find(key);
        if (!old && !t.playCount && !t.skipCount && !t.rating && !fav) continue;
        Shadow s = old ? *old : Shadow{};
        if (!old) {
            // First seen here: plays since joining are news; anything older is what this PC brought, and
            // counts merge by the highest so a copied library isn't counted twice.
            if (t.firstPlayed > st.enabled) {
                counts(id, t.playCount, t.skipCount);
            } else if (t.playCount || t.skipCount) {
                Seal(out, Line("B") << now << t.playCount << t.skipCount << t.lastPlayed << t.firstPlayed << keyText(id));
                s.basePlays = t.playCount;
                s.baseSkips = t.skipCount;
            }
            if (t.rating) Seal(out, Line("R") << now << t.rating << 1 << keyText(id));
            if (fav) Seal(out, Line("H") << now << 1 << 1 << keyText(id));
        } else {
            const int64_t plays = (int64_t)t.playCount - s.plays, skips = (int64_t)t.skipCount - s.skips;
            if (plays || skips) counts(id, plays, skips);
            if (t.rating != s.rating) {
                s.ratingTime = After(s.ratingTime, now);
                Seal(out, Line("R") << s.ratingTime << t.rating << 0 << keyText(id));
            }
            if (fav != s.fav) {
                s.favTime = After(s.favTime, now);
                Seal(out, Line("H") << s.favTime << fav << 0 << keyText(id));
            }
        }
        s.plays = t.playCount;
        s.skips = t.skipCount;
        s.rating = t.rating;
        s.fav = fav;
        tracks.sorted.emplace_back(key, s);
    }
    tracks.Sort();
    st.tracks = std::move(tracks);
    // A place whose track left the library can't be named any more: it just ages out.
    std::erase_if(st.spots.items, [&](const auto& e) {
        return e.second.posMs && e.second.time + 90 * kDay < now && has(clearedSpots, e.first);
    });

    // Playlists, by name. One this PC hasn't published yet only fills gaps on the others, so two PCs
    // with a "Road trip" each keep their own until one of them is edited.
    std::vector<std::string> present;
    for (const Playlist& pl : live.playlists) {
        if (!Syncable(pl)) continue;
        const std::string name = ToUtf8(pl.name);
        if (std::find(present.begin(), present.end(), name) != present.end()) continue;  // a second of one name stays here
        present.push_back(name);
        const uint64_t hash = ListHash(lib, pl);
        const ListState* known = st.lists.Find(name);
        uint32_t time;
        if (!known) time = 0;
        else if (known->deleted || known->hash != hash) time = After(known->time, now);
        else continue;
        std::string items;
        if (pl.kind == PlaylistKind::Static)
            for (TrackId id : pl.items)
                if (lib.Contains(id)) {
                    if (!items.empty()) items += '\x1e';
                    items += KeyText(KeyOf(lib, id, folders), '\x1f');
                }
        Seal(out, Line("L") << time << (pl.kind == PlaylistKind::Auto ? "A" : "S") << Escape(name) << Escape(pl.rules)
                            << items);
        st.lists[name] = {time, hash, false};
    }
    for (auto& [name, l] : st.lists)
        if (!l.deleted && std::find(present.begin(), present.end(), name) == present.end()) {
            l = {After(l.time, now), 0, true};
            Seal(out, Line("X") << l.time << Escape(name));
        }

    // 2. The other PCs' lines (and any of this PC's own that the saved library may lack).
    Matcher match(lib, folders);
    std::vector<TrackId> touched;
    std::vector<Pending> stillPending;
    auto apply = [&](Event& e, uint32_t received) {
        const bool own = e.source == st.log;
        if (e.kind == 'L' || e.kind == 'X') {
            int local = -1;
            for (int i = 0; i < (int)live.playlists.size() && local < 0; ++i)
                if (Syncable(live.playlists[i]) && ToUtf8(live.playlists[i].name) == e.name) local = i;
            const ListState* known = st.lists.Find(e.name);
            if (e.kind == 'X') {
                if (known && e.time <= known->time) return;
                if (local >= 0) changes.playlistsRemoved.push_back(live.playlists[local].name);
                st.lists[e.name] = {e.time, 0, true};
                return;
            }
            if (known && (e.time == 0 || e.time <= known->time)) return;
            if (local >= 0 && (live.playlists[local].kind == PlaylistKind::Auto) != e.autoList) return;
            std::vector<TrackId> items;
            if (!e.autoList)
                for (const Key& k : e.items)
                    if (TrackId id = match.FindOrCreate(k, changes.created); lib.Contains(id)) items.push_back(id);
            if (local >= 0) {
                Playlist& pl = live.playlists[local];
                if (e.autoList) pl.rules = e.rules;
                else {
                    std::vector<TrackId> before = pl.items;
                    pl.items = std::move(items);
                    RecordPlaylistEdit(pl, std::move(before), L"Sync");
                    SanitizePlaylistPosition(pl);
                }
                changes.playlistsEdited.push_back(local);
                st.lists[e.name] = {e.time, ListHash(lib, pl), false};
            } else {
                Playlist pl;
                pl.name = FromUtf8(e.name);
                pl.kind = e.autoList ? PlaylistKind::Auto : PlaylistKind::Static;
                pl.rules = e.rules;
                pl.items = std::move(items);
                st.lists[e.name] = {e.time, ListHash(lib, pl), false};
                live.playlists.push_back(std::move(pl));
                ++changes.playlistsAdded;
            }
            return;
        }
        const TrackId id = match.Find(e.key);
        if (!lib.Contains(id)) {
            if (!own) stillPending.push_back({e.source, received, e.line});
            return;
        }
        Track& t = lib[id];
        Shadow& s = st.tracks.Get(PathKey(lib, t));
        const Track before = t;
        const bool favBefore = lib.Favorite(id);
        switch (e.kind) {
        case 'N': {
            const bool counted = e.n[1] & PlayEvent::kCounted, skip = !counted && (e.n[1] & PlayEvent::kSkip);
            if (!counted && !skip) return;
            const uint32_t when = std::min(e.time, now);
            if (counted) {
                t.playCount = Sat((int64_t)t.playCount + 1);
                MinFirst(t.firstPlayed, when);
                t.lastPlayed = std::max(t.lastPlayed, when);
            } else {
                t.skipCount = Sat((int64_t)t.skipCount + 1);
            }
            changes.listens.push_back({id, when, (uint16_t)std::clamp<int64_t>(e.n[0], 0, 65535),
                                       (uint8_t)(counted ? PlayEvent::kCounted : PlayEvent::kSkip)});
            break;
        }
        case 'D':
            t.playCount = Sat((int64_t)t.playCount + e.n[0]);
            t.skipCount = Sat((int64_t)t.skipCount + e.n[1]);
            if (e.n[0] > 0) t.lastPlayed = std::max<uint32_t>(t.lastPlayed, Sat(std::min<int64_t>(e.n[2], now)));
            MinFirst(t.firstPlayed, e.n[3]);
            break;
        case 'B':
            // The highest count any PC brought: only the difference to the highest seen so far is added.
            // This PC's own counts are already here, so its own line only records them.
            if (e.n[0] > s.basePlays) {
                if (!own) t.playCount = Sat((int64_t)t.playCount + e.n[0] - s.basePlays);
                s.basePlays = (uint32_t)std::min<int64_t>(e.n[0], UINT32_MAX);
            }
            if (e.n[1] > s.baseSkips) {
                if (!own) t.skipCount = Sat((int64_t)t.skipCount + e.n[1] - s.baseSkips);
                s.baseSkips = (uint32_t)std::min<int64_t>(e.n[1], UINT32_MAX);
            }
            t.lastPlayed = std::max<uint32_t>(t.lastPlayed, Sat(std::min<int64_t>(e.n[2], now)));
            MinFirst(t.firstPlayed, e.n[3]);
            break;
        case 'R': {
            const uint8_t rating = (uint8_t)std::clamp<int64_t>(e.n[0], 0, 100);
            if (e.n[1] ? t.rating == 0 && s.ratingTime == 0 : e.time > s.ratingTime) {
                t.rating = rating;
                if (!e.n[1]) s.ratingTime = e.time;
            }
            break;
        }
        case 'H':
            if (e.n[1] ? !favBefore && s.favTime == 0 : e.time > s.favTime) {
                lib.SetFavorite(id, e.n[0] != 0);
                if (!e.n[1]) s.favTime = e.time;
            }
            break;
        case 'S': {
            SpotState& sp = st.spots[PathKey(lib, t)];
            if (e.time <= sp.time) return;
            sp = {(uint32_t)std::clamp<int64_t>(e.n[0], 0, UINT32_MAX), e.time};
            std::erase_if(live.spots, [id](const auto& p) { return p.first == id; });
            const double pos = sp.posMs / 1000.0, length = t.Duration();
            if (sp.posMs && pos >= 5 && (length <= 0 || pos < length - 5)) live.spots.insert(live.spots.begin(), {id, pos});
            changes.spots = true;
            return;
        }
        default:
            return;
        }
        s.plays = t.playCount;
        s.skips = t.skipCount;
        s.rating = t.rating;
        s.fav = lib.Favorite(id);
        if (t.playCount != before.playCount || t.skipCount != before.skipCount || t.rating != before.rating ||
            t.lastPlayed != before.lastPlayed || t.firstPlayed != before.firstPlayed || s.fav != favBefore)
            touched.push_back(id);
    };
    // Lines that waited for their track are tried again once the library has grown.
    std::vector<Pending> waiting = std::move(st.pending);
    st.pending.clear();
    if (lib.Count() != pendingCount_ || !merged_) {
        pendingCount_ = lib.Count();
        for (Pending& p : waiting) {
            Event e;
            if (p.received + 60 * kDay < now || !ParseEvent(p.line, e)) {
                stateChanged = true;
                continue;
            }
            e.source = p.source;
            e.line = std::move(p.line);
            const size_t before = stillPending.size();
            apply(e, p.received);
            if (stillPending.size() == before) stateChanged = true;
        }
    } else {
        stillPending = std::move(waiting);
    }
    for (Event& e : scan.events) apply(e, now);
    if (stillPending.size() > kMaxPending) stillPending.erase(stillPending.begin(), stillPending.end() - kMaxPending);
    st.pending = std::move(stillPending);
    st.tracks.Settle();

    // Newest places first, by when they were left on whichever PC.
    if (changes.spots) {
        auto left = [&](TrackId id) {
            const SpotState* s = st.spots.Find(PathKey(lib, lib[id]));
            return s ? s->time : now;
        };
        for (size_t i = 1; i < live.spots.size(); ++i)
            for (size_t j = i; j > 0 && left(live.spots[j - 1].first) < left(live.spots[j].first); --j)
                std::swap(live.spots[j - 1], live.spots[j]);
        if (live.spots.size() > kMaxSpots) live.spots.resize(kMaxSpots);
    }
    std::sort(touched.begin(), touched.end());
    changes.tracks = (size_t)(std::unique(touched.begin(), touched.end()) - touched.begin());
    if (changes.tracks) lib.Touch();
    listenMark_ = log.size() + changes.listens.size();  // the player logs them right after
    if (scan.ownTail) {
        st.written += scan.ownTail;
        stateChanged = true;
    }

    // 3. Publish. A failed write keeps the lines in the bookkeeping for the next pass.
    const std::wstring ownPath = InFolder(st.folder, st.file);
    if (!out.empty() || !st.outbox.empty()) {
        st.outbox += out;
        uint64_t size = 0;
        if (Append(ownPath, Header(st.log, machine), st.outbox, size)) {
            st.written = size;
            st.outbox.clear();
        } else {
            status_.problem = L"Couldn't write to the sync folder " + st.folder + L". Trying again in a minute.";
        }
        stateChanged = true;
    }
    // What the folder holds after this pass, so the next one can tell at a glance that nothing changed.
    // Left empty while lines wait to be written or a long log is still being read.
    seen_ = 0;
    if (st.outbox.empty() && !scan.partial) {
        seen_ = scan.seen;
        for (const auto& [name, size] : scan.files)
            if (name == st.file) seen_ -= FileFnv(name, size);
        if (uint64_t size = 0; FileSize(ownPath, size)) seen_ += FileFnv(st.file, size);
    }
    status_.peers.clear();
    for (const auto& [id, p] : st.peers)
        if (id != st.log && !p.name.empty()) status_.peers.push_back({p.name, p.lastSeen});
    status_.lastPass = now;
    merged_ = true;
    if (!stateChanged && !changes.Any()) return false;
    pending_ = std::move(scan.state);
    changes.saveNow = true;
    return true;
}

// --- The driver ----------------------------------------------------------------------------------------

Sync::Sync() = default;

Sync::~Sync() { Join(); }

void Sync::Join() {
    if (worker_.joinable()) worker_.join();
}

bool Sync::Start(Live live, bool force) {
    const uint64_t digest = LocalDigest(live);
    const bool local = digest != digest_;
    digest_ = digest;
    ScanInput in{folder, stateFile, machine, fingerprint,
                 force || local || !merged_ || !SameFolder(activeFolder_, folder), seen_, nullptr};
    activeFolder_ = folder;
    lastStart_ = UnixNow();
    done_ = false;
    status_.busy = true;
    worker_ = std::thread([this, in = std::move(in)]() mutable {
        result_ = ScanFolder(in);
        done_ = true;
    });
    return true;
}

bool Sync::Tick(Live live, Changes& changes, bool now) {
    status_.on = !folder.empty();
    if (worker_.joinable()) {
        if (!done_) return false;
        Join();
        status_.busy = false;
        std::unique_ptr<Scan> scan = std::move(result_);
        if (folder.empty() || !scan || !SameFolder(activeFolder_, folder)) return false;  // turned off or moved meanwhile
        status_.problem = scan->problem;
        if (!scan->problem.empty() || scan->idle || !scan->state) return false;
        Merge(live, *scan, changes);
        return changes.Any() || changes.saveNow;
    }
    if (folder.empty() || pending_) return false;  // off, or waiting for the library save
    if (!now && lastStart_ && UnixNow() < lastStart_ + kPassSeconds) return false;
    if (listenMark_ == SIZE_MAX) listenMark_ = live.playLog.size();  // the session's start: all published
    Start(live, now);
    return false;
}

bool Sync::RunNow(Live live, Changes& changes) {
    Join();
    result_.reset();
    status_.busy = false;
    status_.on = !folder.empty();
    if (folder.empty()) return false;
    if (listenMark_ == SIZE_MAX) listenMark_ = live.playLog.size();
    digest_ = LocalDigest(live);
    // Merged state the library hasn't been saved with yet carries on in memory, so the order holds.
    ScanInput in{folder, stateFile, machine, fingerprint, true, 0, SameFolder(activeFolder_, folder) ? std::move(pending_) : nullptr};
    pending_.reset();
    activeFolder_ = folder;
    lastStart_ = UnixNow();
    std::unique_ptr<Scan> scan = ScanFolder(in);
    if (in.state) pending_ = std::move(in.state);  // the folder failed: keep it for the library save
    status_.problem = scan->problem;
    if (!scan->problem.empty() || !scan->state) return false;
    Merge(live, *scan, changes);
    return true;
}

void Sync::Finish(Live live, Changes& changes) {
    if (!folder.empty()) RunNow(live, changes);
}

void Sync::LibrarySaved() {
    if (!pending_ || worker_.joinable()) return;
    if (WriteAtomically(stateFile, SaveStateText(*pending_))) pending_.reset();
}

Status Sync::CurrentStatus() const { return status_; }

std::wstring Changes::Summary() const {
    std::wstring s;
    auto add = [&](size_t n, const wchar_t* one, const wchar_t* many) {
        if (!n) return;
        s += (s.empty() ? L"" : L", ") + std::to_wstring(n) + L" " + (n == 1 ? one : many);
    };
    add(tracks, L"track", L"tracks");
    add(playlistsAdded + playlistsEdited.size(), L"playlist", L"playlists");
    add(playlistsRemoved.size(), L"closed playlist", L"closed playlists");
    if (s.empty() && spots) s = L"places in songs";
    return s.empty() ? s : L"Synced " + s + L" from your other PCs.";
}

}  // namespace syncfolder
